"""Focused native checks of shipping manual admission and the real LCD action.

Only hardware/Arduino boundaries are faked. Policy/codec, user-action latch,
awake-proof predicate, ACK byte replay and Sense duplicate suppression are the
production definitions, compiled directly from their source.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHARED = ROOT / 'halo_ota_demo/firmware/shared'
ARDUINO_JSON = Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src'


def definition(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def policy_harness():
    return r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "DurableOtaDiscovery.h"
using namespace durable_ota;
int main() {
  constexpr uint32_t prior=20706UL*86400+86000, today=20707UL*86400+600;
  Target target{};strcpy(target.version,"6.4.114");strcpy(target.peer_version,"6.4.114");
  strcpy(target.url,"https://example.com/sense114.bin");target.bytes=1803312;target.peer_bytes=1877264;
  target.sha256[0]=1;target.peer_sha256[0]=2;
  uint8_t campaign[16]={1}, newer[16]={2};Record r{},n{};
  assert(start_discovery("nightly_20260910",campaign,{prior,true,true},false,false,r));
  assert(bind_discovery(r,{prior+1,true,true},target,true,true,n));r=n;
  assert(reserve_apply(r,{prior+2,true,true},2000,kDailyWorkMs,true,n));r=n;
  assert(reserve_begin(r,{prior+3,true,true},0,n));r=n;
  assert(reserve_begin(r,{prior+4,true,true},1,n));r=n;
  assert(resolve(r,{prior+5,true,true},target,true,true,true,n));r=n;
  // Actual completed114 ledger shape: one window/apply/begin each,13ms left.
  r.work_remaining_ms=13;assert(shape(r));
  uint8_t before[kRecordBytes],after[kRecordBytes];assert(encode(r,before));
  assert(reserve_discovery(r,{today,true,false},false,false,n,"manual",newer,true)==Admission::ALLOWED);
  assert(n.phase==Phase::DISCOVERY&&n.budget_day==20707&&n.network_windows==1);
  assert(n.work_remaining_ms==kDailyWorkMs-kPreflightMs&&n.reserved_work_ms==kPreflightMs);
  assert(n.day_attempts==0&&n.begins[0]==0&&n.begins[1]==0&&same_target(n.target,r.target));
  assert(strcmp(n.origin,"manual")==0&&memcmp(n.campaign,newer,16)==0);
  Record decoded;assert(encode(n,after)&&decode(after,sizeof(after),decoded));
  // Neither an automatic wake nor force/reason text supplies the explicit latch.
  assert(reserve_discovery(r,{today,true,false},false,false,n,"manual",newer)==Admission::NOT_DUE);
  assert(reserve_discovery(r,{prior+10,true,false},false,false,n,"manual",newer,true)==Admission::BUDGET);
  assert(reserve_discovery(r,{today,false,false},false,false,n,"manual",newer,true)==Admission::CLOCK);
  assert(reserve_discovery(r,{today,true,false},true,false,n,"manual",newer,true)==Admission::LEGACY);
  assert(reserve_discovery(r,{today,true,false},false,true,n,"manual",newer,true)==Admission::BUSY);
  assert(reserve_discovery(r,{prior-1,true,false},false,false,n,"manual",newer,true)==Admission::CLOCK);
  assert(reserve_discovery(r,{today,true,true},false,false,n,"nightly",newer)==Admission::ALLOWED);
  Record failed=r;failed.phase=Phase::DEFERRED;failed.deferred_path=true;failed.not_before=today+86400;
  assert(shape(failed));uint8_t debt[kRecordBytes];assert(encode(failed,debt));
  assert(reserve_discovery(failed,{today,true,false},false,false,n,"manual",newer,true)==Admission::IDENTITY);
  assert(encode(failed,after)&&memcmp(debt,after,sizeof(debt))==0);
  failed.phase=Phase::QUARANTINED;assert(shape(failed));
  assert(reserve_discovery(failed,{today,true,false},false,false,n,"manual",newer,true)==Admission::IDENTITY);
  // A closed read-only check may spend a remaining same-day window, without
  // replacing its retained target or refreshing any daily allowance.
  assert(reserve_discovery(r,{today,true,false},false,false,n,"manual",newer,true)==Admission::ALLOWED);
  Record discovery=n,closed;
  assert(reserve_discovery(discovery,{today+1,true,false},false,false,n,"manual",campaign,true)==Admission::BUSY);
  assert(close_discovery(discovery,{today+2,true,false},2000,true,today+86400,closed,true));
  assert(reserve_discovery(closed,{today+3,true,false},false,false,n,"manual",campaign,true)==Admission::ALLOWED);
  assert(same_target(n.target,closed.target)&&n.network_windows==closed.network_windows+1);
  assert(reserve_discovery(closed,{today+86401,true,false},false,false,n,"manual",campaign,true)==Admission::NOT_DUE);
  // Actual138-style no-update ledger: generation2, phase9, one network window,
  // no bound target, no apply/begin attempts, and a future normal-maintenance due.
  Record initial,empty_closed,reopened,finished;
  constexpr uint32_t normal_due=today+600;
  assert(start_discovery("manual_138",campaign,{today,true,false},false,false,initial));
  assert(close_discovery(initial,{today+2,true,false},2000,true,normal_due,empty_closed,true));
  assert(empty_closed.generation==2&&empty_closed.phase==Phase::DISCOVERY&&target_empty(empty_closed.target));
  assert(empty_closed.network_windows==1&&!empty_closed.day_attempts&&!empty_closed.begins[0]&&!empty_closed.begins[1]);
  uint8_t closed_bytes[kRecordBytes];assert(encode(empty_closed,closed_bytes));
  assert(reserve_discovery(empty_closed,{today+3,true,false},false,false,reopened,"manual_again",newer,true)==Admission::ALLOWED);
  assert(reopened.generation==3&&reopened.network_windows==2&&reopened.reserved_work_ms==kPreflightMs);
  assert(reopened.work_remaining_ms+kPreflightMs==empty_closed.work_remaining_ms);
  assert(reopened.budget_day==empty_closed.budget_day&&reopened.budget_granted==empty_closed.budget_granted);
  assert(reopened.created==empty_closed.created&&reopened.not_before==normal_due);
  assert(strcmp(reopened.origin,empty_closed.origin)==0&&!memcmp(reopened.campaign,empty_closed.campaign,16));
  assert(target_empty(reopened.target)&&!reopened.day_attempts&&!reopened.begins[0]&&!reopened.begins[1]&&!reopened.fast_opportunities);
  assert(encode(reopened,after)&&decode(after,sizeof(after),decoded));
  assert(decoded.network_windows==2&&decoded.reserved_work_ms==kPreflightMs&&target_empty(decoded.target));
  // A duplicate cannot allocate while the first explicit check still owns work.
  assert(reserve_discovery(reopened,{today+4,true,false},false,false,n,"duplicate",campaign,true)==Admission::BUSY);
  assert(close_discovery(reopened,{today+5,true,false},2000,true,normal_due,finished,true));
  assert(finished.network_windows==2&&!finished.reserved_work_ms);
  assert(reserve_discovery(finished,{today+6,true,false},false,false,n,"third",campaign,true)==Admission::BUDGET);
  // Neither automatic wakes nor the word "manual" bypasses the existing due gate.
  assert(reserve_discovery(empty_closed,{today+3,true,false},false,false,n,"manual",newer)==Admission::NOT_DUE);
  assert(reserve_discovery(empty_closed,{normal_due-1,true,true},false,false,n,"nightly",newer)==Admission::NOT_DUE);
  assert(reserve_discovery(empty_closed,{normal_due,true,true},false,false,n,"nightly",newer)==Admission::ALLOWED);
  assert(n.network_windows==2&&n.work_remaining_ms+kPreflightMs==empty_closed.work_remaining_ms);
  // Production previously persisted no distinction for successful and failed
  // unbound reads. An explicit retry of either is charged; automatic cooldown stays.
  Record failed_read;
  assert(close_discovery(initial,{today+2,true,false},2000,true,normal_due,failed_read,false));
  assert(encode(failed_read,after)&&!memcmp(closed_bytes,after,sizeof(after)));
  assert(reserve_discovery(failed_read,{today+3,true,false},false,false,n,"retry",newer,true)==Admission::ALLOWED);
  assert(n.network_windows==2&&n.work_remaining_ms+kPreflightMs==failed_read.work_remaining_ms);
  assert(reserve_discovery(failed_read,{today+3,true,false},false,false,n,"automatic",newer)==Admission::NOT_DUE);
  // A reset during discovery remains fully charged; an explicit retry cannot refund it.
  Record interrupted;
  assert(reconcile_reset(initial,{today+2,true,false},normal_due,interrupted));
  assert(interrupted.work_remaining_ms==initial.work_remaining_ms&&!interrupted.reserved_work_ms);
  assert(reserve_discovery(interrupted,{today+3,true,false},false,false,n,"retry",newer,true)==Admission::ALLOWED);
  assert(n.network_windows==2&&n.work_remaining_ms+kPreflightMs==interrupted.work_remaining_ms);
  // Remaining work is independently bounded even when a network window remains.
  Record low=empty_closed;low.work_remaining_ms=999;assert(shape(low));
  assert(reserve_discovery(low,{today+3,true,false},false,false,n,"manual",newer,true)==Admission::BUDGET);
  low.work_remaining_ms=1000;assert(shape(low));
  assert(reserve_discovery(low,{today+3,true,false},false,false,n,"manual",newer,true)==Admission::ALLOWED);
  assert(n.reserved_work_ms==1000&&n.work_remaining_ms==0&&n.network_windows==2);
  // No new-day refill, legacy slow-path escape, stale clock or target-debt bypass.
  assert(reserve_discovery(empty_closed,{today+86401,true,false},false,false,n,"manual",newer,true)==Admission::NOT_DUE);
  Record later_due=empty_closed;later_due.not_before=today+2*86400;assert(shape(later_due));
  assert(reserve_discovery(later_due,{today+86401,true,true},false,false,n,"manual",newer,true)==Admission::NOT_DUE);
  assert(reserve_discovery(empty_closed,{today+3,true,false},true,false,n,"manual",newer,true)==Admission::LEGACY);
  assert(reserve_discovery(empty_closed,{today+3,false,false},false,false,n,"manual",newer,true)==Admission::CLOCK);
  assert(reserve_discovery(empty_closed,{today+1,true,false},false,false,n,"manual",newer,true)==Admission::CLOCK);
  Record slow=empty_closed;slow.deferred_path=true;assert(shape(slow));
  assert(reserve_discovery(slow,{today+3,true,false},false,false,n,"manual",newer,true)==Admission::NOT_DUE);
  Record target_debt;
  assert(bind_discovery(initial,{today+1,true,false},target,true,true,target_debt));
  assert(reserve_discovery(target_debt,{today+3,true,false},false,false,n,"manual",newer,true)==Admission::BUSY);
  assert(encode(empty_closed,after)&&!memcmp(closed_bytes,after,sizeof(after)));
  assert(encode(r,after)&&memcmp(before,after,sizeof(before))==0);
  puts("PASS charged same-day manual discovery retry, duplicate/work/network bounds, normal cooldown and pending debt preserved");
}
'''


def admission_result_harness():
    runtime = (SHARED / 'SenseDurablePolicyRuntime.h').read_text()
    declaration = next(line for line in runtime.splitlines()
                       if line.startswith('static durable_ota::Admission last_admission='))
    # Compile the actual reset and complete early-return predicate from enter;
    # everything after the first work reset is outside this invocation guard test.
    enter = definition(runtime, 'static bool enter(const char* reason,bool retained_legacy) {')
    enter_prefix = enter[:enter.index('  work={};')] + '  return true;\n}'
    return r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "DurableOtaPolicy.h"
using durable_ota::Admission;
static durable_ota::Record record;
static durable_ota::Record* observed=&record;
static const durable_ota::Record* current(){return observed;}
static struct {bool live=false;} work;
static bool g_lcd_work_budget_live=true,image_valid=true;
static struct {uint32_t remaining=1000;uint32_t remaining_ms(){return remaining;}} g_lcd_work_budget;
static bool nvs_capacity_image_valid(){return image_valid;}
''' + declaration + '\n' + '\n'.join((
        definition(runtime, 'static bool admission_allowed(durable_ota::Admission result) {'),
        definition(runtime, 'static const char* refusal_result(bool manual) {'),
        enter_prefix,
    )) + r'''
int main(){
  for(auto phase:{durable_ota::Phase::DISCOVERY,durable_ota::Phase::DEFERRED,
                  durable_ota::Phase::RESOLVED,durable_ota::Phase::PREFLIGHT}){
    record.phase=phase;
    for(unsigned value=0;value<=unsigned(Admission::DISCOVERY);++value){
      auto reason=Admission(value);
      assert(admission_allowed(reason)==(reason==Admission::ALLOWED));
      assert(last_admission==reason);
      assert(!strcmp(refusal_result(true),reason==Admission::BUDGET?"policy_daily_limit":"policy_deferred"));
      assert(!strcmp(refusal_result(false),phase==durable_ota::Phase::RESOLVED?"policy_target_valid":"policy_deferred"));
    }
  }
  observed=nullptr;assert(!admission_allowed(Admission::BUDGET));
  assert(!strcmp(refusal_result(true),"policy_daily_limit"));
  assert(!strcmp(refusal_result(false),"policy_deferred"));
  observed=&record;record.phase=durable_ota::Phase::RESOLVED;
  // Each actual early return must discard a previous invocation's BUDGET result.
  for(unsigned guard=0;guard<4;++guard){
    work.live=guard==0;g_lcd_work_budget_live=guard!=1;
    g_lcd_work_budget.remaining=guard==2?0:1000;image_valid=guard!=3;
    assert(!admission_allowed(Admission::BUDGET));assert(!enter("manual",false));
    assert(last_admission==Admission::NOT_DUE);
    assert(!strcmp(refusal_result(true),"policy_deferred"));
  }
  work.live=false;g_lcd_work_budget_live=image_valid=true;g_lcd_work_budget.remaining=1000;
  assert(!admission_allowed(Admission::BUDGET));assert(enter("manual",false));
  assert(last_admission==Admission::NOT_DUE);
  puts("PASS actual admission/result routing and stale-budget reset before every invocation guard");
}
'''


def lcd_harness():
    main = (ROOT / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    action = (ROOT / 'LCD_Minimal/lcd_ship_action.h').read_text()
    uart = (ROOT / 'LCD_Minimal/lcd_uart.h').read_text()
    ack = (ROOT / 'LCD_Minimal/lcd_link_ack.h').read_text()
    sense = (ROOT / 'Sense_Minimal/sense_uart_msg.h').read_text()
    prefix = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <utility>
static uint32_t now_ms=100;static uint32_t millis(){return now_ms;}
static bool g_manual_ota_override,g_manual_ota_ui_requested,g_manual_ota_ui_active,ota_locked,g_ota_screen_active;
static bool g_lcd_ota_uart_receiving,provision_return_home_pending,busy,queue_ok=true;
static std::atomic<uint8_t> g_manual_ota_result{0};
static unsigned long g_manual_ota_override_until_ms,g_manual_ota_result_until_ms,ota_stay_awake_until_ms,g_manual_ota_ui_deadline_ms;
static unsigned long MANUAL_OTA_OVERRIDE_TTL_MS=300000;
static unsigned sends,wakes,activity;
struct tx_msg_t{char type[32];};
static bool provisioning_input_locked(){return busy;}
static bool uart_tx_enqueue(const tx_msg_t* m,const char*){assert(!strcmp(m->type,"INPUT_OTA_CHECK"));if(queue_ok)++sends;return queue_ok;}
static void request_sense_wake(const char*){++wakes;}
static void resetActivityTimer(){++activity;}
static size_t test_strlcpy(char* d,const char* s,size_t n){const auto l=strlen(s);if(n){snprintf(d,n,"%s",s);}return l;}
#define strlcpy test_strlcpy
static struct {template<class... A>void printf(const char*,A...){}void println(const char*){}} Serial;
static std::vector<std::string> wire;
static std::vector<std::pair<std::string,bool>> delete_results;
static void post_list_delete_result(const char* id,bool ok){delete_results.emplace_back(id,ok);}
static struct{void print(const char* s){wire.emplace_back(s);}void flush(){}} senseSerial;
static void lcd_errlog_store_with_context(const char*,const char*,const char*,int,const char*){}
struct app_event_t{int type;char data[8];};static void* app_event_queue=nullptr;
static constexpr int EVT_LINK_SEND_FAILED=1;
static void xQueueSend(void*,app_event_t*,int){}
#define HALO_DEMO_MODE 0
#define LINK_ACK_SLOTS 4
#define LINK_ACK_RETRY_MS 400
#define LINK_ACK_MAX_ATTEMPTS 5
#define LINK_ACK_PAYLOAD_MAX 224
'''
    ack_state = ack[ack.index('typedef struct {'):ack.index('// Which messages are worth')]
    seen_state = sense[sense.index('#ifndef SENSE_INPUT_SEEN_RING'):sense.index('static bool sense_input_seen_recently')]
    functions = '\n'.join([
        definition(main, 'static void lcd_manual_ota_override_set'),
        definition(main, 'static void lcd_manual_ota_override_clear'),
        definition(main, 'static void lcd_manual_ota_finish'),
        definition(action, 'static void ship_menu_send_manual_ota'),
        definition(uart, 'static bool tx_msg_requires_awake_proof'),
        *[definition(ack, sig) for sig in ['static bool link_ack_should_track', 'static void link_ack_track', 'static bool link_ack_on_ack','static void link_ack_service']],
        definition(sense,'static bool sense_input_seen_recently'),
        definition(sense,'static void sense_input_mark_seen'),
    ])
    ui = (ROOT / 'LCD_Minimal/lcd_ui_task.h').read_text()
    ui_tick = 'static void actual_ui_tick(){' + ui[ui.index("    // USB 'ota' and"):ui.index('    // ── Cold-boot repaint')] + '}'
    cases = r'''
int main(){
  ship_menu_send_manual_ota("menu_action");ship_menu_send_manual_ota("menu_action");
  assert(sends==1&&wakes==1&&g_manual_ota_ui_active&&g_ota_screen_active);
  tx_msg_t m{};strcpy(m.type,"INPUT_OTA_CHECK");assert(tx_msg_requires_awake_proof(&m));
  assert(link_ack_should_track(m.type));
  const char* payload="{\"type\":\"INPUT_OTA_CHECK\",\"msg_id\":42}";
  link_ack_track(42,m.type,payload);now_ms+=400;link_ack_service();
  assert(wire.size()==2&&wire[0]==payload&&wire[1]=="\n");
  assert(!sense_input_seen_recently(42));sense_input_mark_seen(42);assert(sense_input_seen_recently(42));
  assert(link_ack_on_ack(42));wire.clear();now_ms+=400;link_ack_service();assert(wire.empty());
  lcd_manual_ota_finish("policy_daily_limit");
  assert(g_manual_ota_result==2&&!g_manual_ota_override&&g_manual_ota_ui_active&&g_ota_screen_active);
  assert(g_manual_ota_result_until_ms==now_ms+8000&&ota_stay_awake_until_ms==now_ms+8000);
  assert(!provision_return_home_pending);
  ship_menu_send_manual_ota("usb_settings_action");assert(sends==1&&wakes==1);
  g_manual_ota_result=0;lcd_manual_ota_finish("unknown_denial");assert(g_manual_ota_result==4);
  g_manual_ota_result=0;lcd_manual_ota_finish("up_to_date");assert(g_manual_ota_result==1);
  g_manual_ota_result=0;lcd_manual_ota_finish("policy_deferred");assert(g_manual_ota_result==3);
  // Queue/ACK exhaustion is a terminal user-visible result, never a requeue.
  g_manual_ota_ui_active=g_ota_screen_active=false;queue_ok=false;
  ship_menu_send_manual_ota("menu_action");assert(g_manual_ota_result==4&&sends==1);
  g_manual_ota_result=0;link_ack_track(43,m.type,payload);
  for(unsigned i=0;i<5;++i){now_ms+=400;link_ack_service();}
  assert(g_manual_ota_result==4&&g_link_ack_failed==1&&sends==1&&delete_results.empty());
  const auto until=g_manual_ota_result_until_ms;now_ms=until-1;actual_ui_tick();
  assert(g_manual_ota_ui_active&&g_ota_screen_active&&!provision_return_home_pending);
  lcd_manual_ota_finish("up_to_date");assert(g_manual_ota_result_until_ms==until&&g_manual_ota_result==4);
  now_ms=until;actual_ui_tick();assert(!g_manual_ota_ui_active&&!g_ota_screen_active&&provision_return_home_pending);
  // The supported serial command runs the same physical action on this task.
  queue_ok=true;g_manual_ota_ui_requested=true;actual_ui_tick();assert(sends==2&&g_manual_ota_ui_active);
  now_ms=g_manual_ota_ui_deadline_ms;actual_ui_tick();assert(g_manual_ota_result==4);
  // Keep the production delete-failure branch in this shared ACK-service test.
  // Real ArduinoJson must recover the original escaped ID, without changing
  // the manual result or interpreting malformed payloads as another item's ID.
  const char* delete_payload=R"({"type":"INPUT_DELETE","msg_id":44,"id":"fixture\"id\\row"})";
  wire.clear();link_ack_track(44,"INPUT_DELETE",delete_payload);
  for(unsigned i=0;i<5;++i){now_ms+=400;link_ack_service();}
  assert(delete_results.size()==1&&delete_results[0].first=="fixture\"id\\row"&&!delete_results[0].second);
  assert(wire.size()==8&&g_manual_ota_result==4&&g_link_ack_failed==2);
  for(unsigned i=0;i<wire.size();i+=2)assert(wire[i]==delete_payload&&wire[i+1]=="\n");
  link_ack_track(45,"INPUT_DELETE","not json");
  for(unsigned i=0;i<5;++i){now_ms+=400;link_ack_service();}
  assert(delete_results.size()==1&&g_link_ack_failed==3&&g_manual_ota_result==4);
  puts("PASS actual LCD action single queued request, awake-proof, byte-identical ACK replay, Sense dedupe, readable refusal, bounded delivery failure and delete-failure routing");
}
'''
    return '\n'.join([prefix,ack_state,seen_state,functions,ui_tick,cases])


class ManualOtaTests(unittest.TestCase):
    def compile_run(self, text):
        self.assertTrue((ARDUINO_JSON / 'ArduinoJson.h').is_file(), 'Canonical ArduinoJson include directory is required')
        with tempfile.TemporaryDirectory(prefix='halo-manual-ota-') as directory:
            path=Path(directory);cpp=path/'check.cpp';binary=path/'check';cpp.write_text(text)
            subprocess.run([shutil.which('c++'),'-std=c++17','-Wno-deprecated-declarations','-I',str(SHARED),'-I',str(ARDUINO_JSON),str(cpp),'-o',str(binary)],check=True,timeout=30)
            subprocess.run([str(binary)],check=True,timeout=5)

    def test_actual_policy(self):
        self.compile_run(policy_harness())

    def test_actual_admission_result(self):
        self.compile_run(admission_result_harness())

    def test_actual_lcd_action_and_delivery(self):
        self.compile_run(lcd_harness())


if __name__=='__main__':
    unittest.main()

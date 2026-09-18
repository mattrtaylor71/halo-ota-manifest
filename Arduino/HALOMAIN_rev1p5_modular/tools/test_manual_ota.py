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
static void roundtrip(const Record& r) {
  uint8_t bytes[kRecordBytes],same[kRecordBytes];Record decoded{};
  assert(encode(r,bytes)&&decode(bytes,sizeof(bytes),decoded)&&encode(decoded,same));
  assert(!memcmp(bytes,same,sizeof(bytes))&&bytes[4]<=3&&bytes[7]==0);
}
static Record exhausted(Record r) {
  r.network_windows=2;r.day_attempts=2;r.begins[0]=r.begins[1]=4;
  r.work_remaining_ms=0;assert(shape(r));return r;
}
int main() {
  constexpr uint32_t today=20707UL*86400+600, normal_due=today+86400;
  Target target{};strcpy(target.version,"6.4.192");strcpy(target.peer_version,"6.4.192");
  strcpy(target.url,"https://example.com/sense192.bin");target.bytes=1803312;target.peer_bytes=1877264;
  target.sha256[0]=1;target.peer_sha256[0]=2;
  uint8_t campaign[16]={1}, newer[16]={2};Record initial{},closed{},n{},r{};
  assert(start_discovery("nightly",campaign,{today,true,true},false,false,initial));
  assert(close_discovery(initial,{today+1,true,true},1000,true,normal_due,closed));
  closed=exhausted(closed);roundtrip(closed);
  const Record retained=closed;
  // Same exhausted record: scheduled work remains capped; every actual new tap
  // gets a finite reservation. Reason strings or automatic wakes grant nothing.
  assert(reserve_discovery(closed,{normal_due,true,true},false,false,n,"manual",newer)==Admission::ALLOWED);
  assert(reserve_discovery(closed,{today+2,true,false},false,false,n,"manual",newer)==Admission::NOT_DUE);
  closed.not_before=today+2;
  assert(reserve_discovery(closed,{today+2,true,true},false,false,n,"nightly",newer)==Admission::BUDGET);
  for(unsigned i=0;i<12;++i) {
    const uint32_t when=today+3+i*3;
    assert(reserve_discovery(closed,{when,true,false},false,false,r,"manual",newer,true)==Admission::ALLOWED);
    assert(r.network_windows==1&&r.day_attempts==0&&!r.begins[0]&&!r.begins[1]);
    assert(r.work_remaining_ms==kDailyWorkMs-kPreflightMs&&r.reserved_work_ms==kPreflightMs);
    assert(!strcmp(r.origin,closed.origin)&&!memcmp(r.campaign,closed.campaign,16));
    assert(target_empty(r.target));roundtrip(r);
    assert(reserve_discovery(r,{when,true,false},false,false,n,"duplicate",newer,true)==Admission::BUSY);
    assert(close_discovery(r,{when+1,true,false},1000,true,normal_due,closed,true));roundtrip(closed);
  }
  // Reset retains the spent, compatible reservation; only another explicit
  // request grants more work. No target or successful completion is invented.
  assert(reserve_discovery(closed,{today+50,true,false},false,false,r,"manual",newer,true)==Admission::ALLOWED);
  assert(reconcile_reset(r,{today+51,true,false},normal_due,n));
  assert(n.work_remaining_ms==r.work_remaining_ms&&!active_phase(n)&&target_empty(n.target));
  assert(reserve_discovery(n,{today+52,true,false},false,false,r,"manual",newer)==Admission::NOT_DUE);
  assert(reserve_discovery(n,{today+52,true,false},false,false,r,"manual",newer,true)==Admission::ALLOWED);
  // Actual discovery -> bind -> APPLY -> both board BEGINs -> resolve, repeated
  // for distinct target releases, then a no-update check on the same day.
  assert(close_discovery(r,{today+53,true,false},1000,true,normal_due,closed));
  for(unsigned i=0;i<4;++i) {
    const uint32_t when=today+60+i*10;newer[0]=uint8_t(i+3);
    snprintf(target.version,sizeof(target.version),"6.4.%u",192+i);
    snprintf(target.peer_version,sizeof(target.peer_version),"6.4.%u",192+i);
    target.sha256[0]=uint8_t(i+5);target.peer_sha256[0]=uint8_t(i+9);
    closed=exhausted(closed);
    assert(reserve_discovery(closed,{when,true,false},false,false,r,"new_manual",newer,true)==Admission::ALLOWED);
    assert(bind_discovery(r,{when+1,true,false},target,true,true,n));r=n;roundtrip(r);
    assert(reserve_apply(r,{when+2,true,false},2000,kDailyWorkMs,true,n));r=n;
    assert(r.day_attempts==1&&r.attempt_ordinal==i+1);roundtrip(r);
    for(unsigned board=0;board<2;++board) {
      assert(reserve_begin(r,{when+3,true,false},board,n));r=n;
      assert(reserve_begin(r,{when+3,true,false},board,n));r=n;
      assert(!reserve_begin(r,{when+3,true,false},board,n));
    }
    roundtrip(r);
    assert(resolve(r,{when+4,true,false},target,true,true,true,closed));roundtrip(closed);
  }
  newer[0]=9;
  assert(reserve_discovery(exhausted(closed),{today+110,true,false},false,false,r,"manual_no_update",newer,true)==Admission::ALLOWED);
  assert(same_target(r.target,closed.target));
  assert(close_discovery(r,{today+111,true,false},1000,true,normal_due,n,true));roundtrip(n);
  assert(same_target(n.target,closed.target)&&n.phase==Phase::DISCOVERY);
  // Same-target manual recovery retains all identity/validation evidence and
  // may retry exhausted DEFERRED debt without replacing it with latest.
  Record debt=exhausted(closed);debt.phase=Phase::DEFERRED;debt.deferred_path=true;
  debt.not_before=normal_due;debt.validation_repeats=1;debt.validation_stage=7;debt.validation_error=-42;
  assert(shape(debt));const Record original_debt=debt;
  assert(reserve_preflight(debt,{today+120,true,false},false,n)==Admission::NOT_DUE);
  assert(reserve_discovery(debt,{today+120,true,false},false,false,n,"manual",newer,true)==Admission::IDENTITY);
  assert(reserve_manual_preflight(debt,{today+120,true,false},false,r)==Admission::ALLOWED);
  assert(same_target(r.target,debt.target)&&!strcmp(r.origin,debt.origin)&&!memcmp(r.campaign,debt.campaign,16));
  assert(r.validation_repeats==1&&r.validation_stage==7&&r.validation_error==-42&&r.deferred_path);
  assert(r.network_windows==1&&r.day_attempts==0&&r.work_remaining_ms==kDailyWorkMs-kPreflightMs);roundtrip(r);
  assert(reserve_apply(r,{today+121,true,false},1000,120000,true,n));r=n;
  assert(reserve_begin(r,{today+122,true,false},0,n));r=n;
  assert(reserve_begin(r,{today+122,true,false},1,n));r=n;
  assert(finish(r,{today+123,true,false},2000,true,Failure::TEMPORARY,0,0,normal_due,nullptr,debt));
  assert(debt.phase==Phase::DEFERRED&&same_target(debt.target,original_debt.target));
  assert(reserve_preflight(debt,{today+124,true,false},false,n)==Admission::NOT_DUE);
  assert(reserve_manual_preflight(debt,{today+124,true,false},false,r)==Admission::ALLOWED);
  assert(reconcile_reset(r,{today+125,true,false},normal_due,n)&&n.phase==Phase::DEFERRED);
  assert(same_target(n.target,original_debt.target)&&n.work_remaining_ms==r.work_remaining_ms);roundtrip(n);
  // Neither manual entry bypasses clock, storage, busy, legacy, quarantine,
  // active/armed work or an unbound legacy slow-path discovery.
  assert(reserve_manual_preflight(debt,{today+126,false,false},false,n)==Admission::CLOCK);
  assert(reserve_manual_preflight(debt,{today+126,true,false},true,n)==Admission::BUSY);
  assert(reserve_manual_preflight(r,{today+126,true,false},false,n)==Admission::BUSY);
  debt.phase=Phase::QUARANTINED;assert(reserve_manual_preflight(debt,{today+126,true,false},false,n)==Admission::QUARANTINED);
  Record slow=retained;slow.deferred_path=true;
  assert(reserve_discovery(slow,{today+2,true,false},false,false,n,"manual",newer,true)==Admission::NOT_DUE);
  assert(reserve_discovery(retained,{today+2,false,false},false,false,n,"manual",newer,true)==Admission::CLOCK);
  assert(reserve_discovery(retained,{today-1,true,false},false,false,n,"manual",newer,true)==Admission::CLOCK);
  assert(reserve_discovery(retained,{today+2,true,false},true,false,n,"manual",newer,true)==Admission::LEGACY);
  assert(reserve_discovery(retained,{today+2,true,false},false,true,n,"manual",newer,true)==Admission::BUSY);
  Record full=retained;full.generation=UINT32_MAX;
  assert(reserve_discovery(full,{today+2,true,false},false,false,n,"manual",newer,true)==Admission::STORAGE);
  puts("PASS repeated explicit manual grants, full paired apply, codec compatibility, no-update/reset and exact-target recovery; automatic caps and safety guards retained");
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
      assert(!strcmp(refusal_result(true),"policy_deferred"));
      assert(!strcmp(refusal_result(false),phase==durable_ota::Phase::RESOLVED?"policy_target_valid":"policy_deferred"));
    }
  }
  observed=nullptr;assert(!admission_allowed(Admission::BUDGET));
  assert(!strcmp(refusal_result(true),"policy_deferred"));
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
static unsigned link_ui_errors=0;
static void xQueueSend(void*,app_event_t* evt,int){assert(evt->type==EVT_LINK_SEND_FAILED);++link_ui_errors;}
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
        *[definition(ack, sig) for sig in ['static bool link_ack_should_track', 'static void link_ack_track', 'static bool link_ack_on_ack','static void link_ack_service','static bool link_ack_inflight']],
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
  // Wake-only user activity must reach Sense reliably without creating a
  // foreground operation or replacing the user's screen if the peer is gone.
  const char* active_payload=R"({"ver":1,"type":"INPUT_USER_ACTIVE","msg_id":46,"ts":100})";
  strcpy(m.type,"INPUT_USER_ACTIVE");assert(tx_msg_requires_awake_proof(&m));
  assert(link_ack_should_track(m.type));wire.clear();
  app_event_queue=&m;link_ack_track(46,m.type,active_payload);
  assert(link_ack_inflight());now_ms+=399;link_ack_service();assert(wire.empty());
  ++now_ms;link_ack_service();assert(wire.size()==2&&wire[0]==active_payload&&wire[1]=="\n");
  assert(!link_ack_on_ack(999)&&link_ack_inflight());
  assert(!sense_input_seen_recently(46));sense_input_mark_seen(46);assert(sense_input_seen_recently(46));
  assert(link_ack_on_ack(46)&&!link_ack_inflight());
  wire.clear();now_ms+=400;link_ack_service();assert(wire.empty()&&!link_ui_errors);
  const char* lost_active_payload=R"({"ver":1,"type":"INPUT_USER_ACTIVE","msg_id":47,"ts":100})";
  link_ack_track(47,m.type,lost_active_payload);
  for(unsigned i=0;i<5;++i){now_ms+=400;link_ack_service();}
  assert(!link_ack_inflight()&&wire.size()==8&&g_link_ack_failed==4&&!link_ui_errors);
  for(unsigned i=0;i<wire.size();i+=2)assert(wire[i]==lost_active_payload&&wire[i+1]=="\n");
  // Real capture exhaustion remains visible; silence is specific to the notice.
  link_ack_track(48,"INPUT_MENU_SELECT","capture fixture");
  for(unsigned i=0;i<5;++i){now_ms+=400;link_ack_service();}
  assert(link_ui_errors==1&&g_link_ack_failed==5&&!link_ack_inflight());
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

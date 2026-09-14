"""Native regression for the shipping retry predicate and five-second arm exchange.

Compile the actual small runtime functions with fake UART/clock/storage boundaries;
the canonical reservation/codec/arm operations are the production implementation.
No Arduino build, device, network, or repository mutation is performed.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SHARED = SOURCE / 'halo_ota_demo/firmware/shared'


def definition(text, signature, structure=False):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end + int(structure)]


def harness():
    runtime = (SHARED / 'SenseDurablePolicyRuntime.h').read_text()
    wrapper = (SOURCE / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino').read_text()
    query = (SOURCE / 'Sense_Minimal/sense_ota_lcd.h').read_text()
    prefix = r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "DurableOtaDiscovery.h"
using durable_ota::Record;
using durable_ota::Phase;
struct OtaManifest {};
'''
    types = '\n'.join((
        definition(runtime, 'struct RetryBaseline', True),
        definition(runtime, 'struct Work', True),
        definition(query, 'struct LcdOtaQuerySnapshot', True),
        definition(wrapper, 'static int compare_semver_part'),
        definition(wrapper, 'static int compareSemver'),
    ))
    boundaries = r'''
static Work work;
static std::vector<std::string> diagnostic_records;
static bool sense_lcd_terminal_store(const char* detail,int){diagnostic_records.emplace_back(detail);return true;}
static Record live;
static LcdOtaQuerySnapshot reply;
static struct {bool entered,locked,legacy; char owner[40];} g_peer_gate;
static bool unconfirmed, g_lcd_ota_proxy_owns_uart, g_lcd_ota_task_running;
static uint32_t s_lcd_reboot_cleanup_boot;
static bool busy, query_arrives, arm_ack, lose_cleanup_on_commit;
static uint32_t now_ms, query_calls, arm_calls, base_epoch=1700000000;
static std::vector<unsigned> committed_phases;
static std::vector<unsigned> committed_generations;
static uint32_t millis(){return now_ms;}
static void delay(unsigned n){now_ms+=n;}
static unsigned esp_random(){return 42;}
static bool sense_lcd_ota_retry_safe(){return !unconfirmed;}
static bool self_retry_user_busy(){return busy;}
static bool sense_time_has_fresh_sync(){return true;}
static const Record* current(){return &live;}
static durable_ota::Clock fresh_clock(bool normal=false){return {base_epoch+now_ms/1000,true,normal};}
static uint32_t next_normal_epoch(){return base_epoch+86400;}
static uint32_t remaining(){return work.original_budget-now_ms;}
static bool peer_valid(){return true;}
static bool commit_record(const Record& r){
  uint8_t raw[durable_ota::kRecordBytes];Record decoded;
  assert(durable_ota::encode(r,raw));assert(durable_ota::decode(raw,sizeof(raw),decoded));
  live=decoded;committed_phases.push_back(unsigned(r.phase));
  committed_generations.push_back(r.generation);
  if(lose_cleanup_on_commit)unconfirmed=true;
  return true;
}
static bool commit(const Record& r){return commit_record(r);}
static bool commit_candidate(const Record& r,uint32_t,uint32_t){return commit_record(r);}
static void pump_uart_rx_once(){}
enum {LCD_QUERY_WAITING,LCD_QUERY_READY,LCD_QUERY_TIMEOUT};
static bool sense_lcd_ota_query_start(const char*,uint32_t){++query_calls;return true;}
static unsigned sense_lcd_ota_query_poll(LcdOtaQuerySnapshot& p){
  if(!query_arrives)return LCD_QUERY_WAITING;
  // Deliberately mimic query_poll's mode confirmation: failed cleanup must
  // prevent entry here, rather than being repaired by this side effect.
  unconfirmed=false;p=reply;return LCD_QUERY_READY;
}
struct MaintenanceWindow {uint32_t duration_sec,grace_before_sec,grace_after_sec,start_epoch;char request_id[64];};
static struct {
  uint32_t started_ms,budget_ms,peer_boot_id,start_epoch,remaining_s;
  char request_id[64],challenge[40];bool waiting,matched;
} g_self_retry_arm;
static void send_maint_window(const MaintenanceWindow*,uint32_t,uint32_t,bool,const char*,uint32_t){
  ++arm_calls;g_self_retry_arm.matched=arm_ack;
}
static struct {template<class... A> void printf(const char*,A...) {}} Serial;
static size_t test_strlcpy(char* out,const char* in,size_t n){
  const size_t size=strlen(in);if(n){const size_t used=size<n-1?size:n-1;memcpy(out,in,used);out[used]=0;}return size;
}
#define strlcpy test_strlcpy
'''
    functions = '\n'.join(definition(runtime, x) for x in (
        'static bool retry_transport_ready', 'static bool retry_peer_ready', 'static void finish()'))
    cases = r'''
static void baseline(){
  work={};live={};reply={};g_peer_gate={};g_self_retry_arm={};diagnostic_records.clear();
  unconfirmed=g_lcd_ota_proxy_owns_uart=g_lcd_ota_task_running=busy=false;
  lose_cleanup_on_commit=false;query_arrives=arm_ack=true;
  s_lcd_reboot_cleanup_boot=0;
  query_calls=arm_calls=0;committed_phases.clear();committed_generations.clear();now_ms=60126;
  durable_ota::Target target{};strcpy(target.version,"6.4.103");strcpy(target.peer_version,"6.4.103");
  strcpy(target.url,"https://example.com/sense.bin");target.sha256[0]=1;target.peer_sha256[0]=2;
  target.bytes=1803072;target.peer_bytes=1877264;uint8_t campaign[16]={1};Record r;
  assert(durable_ota::start_discovery("nightly_test",campaign,{base_epoch,true,true},false,false,live));
  assert(durable_ota::bind_discovery(live,{base_epoch+5,true,true},target,true,true,r));live=r;
  assert(durable_ota::reserve_apply(live,{base_epoch+5,true,true},5000,2400000,true,r));live=r;
  assert(durable_ota::reserve_begin(live,{base_epoch+15,true,true},1,r));live=r;
  assert(live.generation==4&&live.begins[1]==1&&live.attempt_begins[1]==1);
  work.live=true;work.original_budget=2400000;work.phase_start=5000;work.phase_budget=live.reserved_work_ms;
  strcpy(work.retry_baseline.fw,"6.4.102");strcpy(work.retry_baseline.part,"app0");
  work.retry_baseline.boot=123;work.retry_baseline.part_size=2621440;
  g_peer_gate.entered=g_peer_gate.locked=true;strcpy(g_peer_gate.owner,"current-owner");
  strcpy(reply.fw,"6.4.102");strcpy(reply.running_part,"app0");strcpy(reply.boot_part,"app0");
  strcpy(reply.running_state,"VALID");strcpy(reply.coord_owner,"current-owner");
  reply.peer_boot_id=123;reply.part_size=2621440;reply.boot_ready=reply.correlated=true;reply.coord_lease_ms=10000;
}
int main(){
  baseline();assert(retry_peer_ready(reply,live));finish();
  assert(live.phase==Phase::ARMED&&query_calls==1&&arm_calls==1);
  assert(diagnostic_records.size()==1&&diagnostic_records.back().find("gates=15")!=std::string::npos&&diagnostic_records.back().find("outcome=armed")!=std::string::npos);
  assert((committed_phases==std::vector<unsigned>{4,5}));
  assert((committed_generations==std::vector<unsigned>{5,6}));
  assert(live.work_remaining_ms==2334874&&live.reserved_work_ms==0);
  assert(live.begins[1]==1&&live.attempt_begins[1]==1&&live.network_windows==1);
  assert(live.arm_peer_boot==123&&live.fast_due==base_epoch+360);
  Record retry;
  assert(durable_ota::reserve_preflight(live,{live.fast_due,true,false},false,retry)==durable_ota::Admission::ALLOWED);
  assert(retry.network_windows==2&&retry.fast_opportunities==2&&retry.begins[1]==1);
  assert(retry.work_remaining_ms==live.work_remaining_ms-durable_ota::kPreflightMs);
  // The normal proxy's plain OTA_LOCK clears the preflight lease. Cleanup
  // plus the same fresh baseline still permits a single durable retry arm.
  baseline();reply.coord_owner[0]=0;reply.coord_lease_ms=0;finish();
  assert(live.phase==Phase::ARMED&&query_calls==1&&arm_calls==1);
  assert(live.work_remaining_ms==2334874&&live.begins[1]==1);
  // A late failed END can outlive the 120-second lease. Keep both begin
  // charges and all elapsed work rather than extending/refilling its budget.
  baseline();reply.coord_owner[0]=0;reply.coord_lease_ms=0;now_ms=240126;
  Record second_begin;
  assert(durable_ota::reserve_begin(live,fresh_clock(),1,second_begin));live=second_begin;
  finish();assert(live.phase==Phase::ARMED&&query_calls==1&&arm_calls==1);
  assert(live.begins[1]==2&&live.attempt_begins[1]==2&&live.work_remaining_ms==2154874);
  assert(live.fast_due==base_epoch+540&&work.original_budget==2400000);
  // Target VALID proof remains independently admissible; old images need the exact baseline.
  baseline();strcpy(reply.fw,"6.4.103");reply.peer_boot_id=456;work.retry_baseline={};assert(retry_peer_ready(reply,live));
  // Only explicit same-invocation reboot cleanup proof admits a different
  // boot still running the old VALID image; the arm query must prove idle again.
  baseline();reply.peer_boot_id=456;reply.recovery_idle=true;s_lcd_reboot_cleanup_boot=456;
  finish();assert(live.phase==Phase::ARMED&&live.arm_peer_boot==456&&query_calls==1&&arm_calls==1);
  assert(live.begins[1]==1&&live.network_windows==1&&live.work_remaining_ms==2334874);
  baseline();reply.peer_boot_id=456;s_lcd_reboot_cleanup_boot=456;
  finish();assert(live.phase==Phase::DEFERRED&&arm_calls==0);
  baseline();reply.peer_boot_id=789;reply.recovery_idle=true;s_lcd_reboot_cleanup_boot=456;
  finish();assert(live.phase==Phase::DEFERRED&&arm_calls==0);
  unsigned negatives=0;
#define REJECT(change) do{baseline();change;assert(!retry_peer_ready(reply,live));++negatives;}while(0)
  REJECT(reply.correlated=false);REJECT(reply.peer_boot_id=0);REJECT(reply.peer_boot_id=456);
  REJECT(reply.boot_ready=false);REJECT(strcpy(reply.fw,"6.4.101"));
  REJECT(strcpy(reply.running_state,"NEW"));REJECT(strcpy(reply.running_state,"PENDING_VERIFY"));
  REJECT(strcpy(reply.running_state,"INVALID"));REJECT(strcpy(reply.boot_part,"app1"));
  REJECT(strcpy(reply.running_part,"app1");strcpy(reply.boot_part,"app1"));
  REJECT(reply.running_part[0]=0);REJECT(strcpy(reply.running_part,"?"));REJECT(reply.part_size--);
  REJECT(work.retry_baseline={});REJECT(strcpy(reply.coord_owner,"other-owner"));
  REJECT(reply.coord_owner[0]=0);REJECT(reply.coord_lease_ms=0);REJECT(reply.coord_lease_ms=120001);
  REJECT(g_peer_gate.entered=false);REJECT(g_peer_gate.locked=false);REJECT(g_peer_gate.legacy=true);
  REJECT(g_peer_gate.owner[0]=0);REJECT(unconfirmed=true);REJECT(g_lcd_ota_proxy_owns_uart=true);
  REJECT(g_lcd_ota_task_running=true);
#undef REJECT
  // Failed cleanup cannot be rehabilitated by a query which confirms JSON mode.
  for(unsigned gate=0;gate<4;++gate){
    baseline();if(gate==0)unconfirmed=true;if(gate==1)g_lcd_ota_proxy_owns_uart=true;
    if(gate==2)g_lcd_ota_task_running=true;if(gate==3)busy=true;
    finish();assert(query_calls==0&&arm_calls==0&&live.phase==Phase::DEFERRED);
    assert(diagnostic_records.size()==1&&diagnostic_records.back().find(gate==3?"gates=13":"gates=11")!=std::string::npos);
    assert(diagnostic_records.back().find(gate==0?"transport=6":gate==1?"transport=5":gate==2?"transport=3":"transport=7")!=std::string::npos);
  }
  // Cleanup lost during the pre-query commit also prevents query_poll's side effect.
  baseline();lose_cleanup_on_commit=true;finish();assert(query_calls==0&&arm_calls==0&&live.phase==Phase::ARM_PENDING);
  // A clean receiver with wrong identity cannot get a MAINT_WINDOW.
  baseline();reply.peer_boot_id=456;finish();assert(query_calls==1&&arm_calls==0&&live.phase==Phase::DEFERRED);
  // Missing nonce response and missing stored ACK retain the finite exchange bound.
  baseline();query_arrives=false;finish();assert(arm_calls==0&&now_ms==65126&&live.phase==Phase::ARM_PENDING);
  baseline();arm_ack=false;finish();assert(arm_calls==1&&now_ms==65126&&live.phase==Phase::ARM_PENDING);
  assert(live.work_remaining_ms==2334874&&live.begins[1]==1&&live.reserved_work_ms==0);
  Record reconciled;assert(durable_ota::close_fast(live,{base_epoch+66,true,false},next_normal_epoch(),reconciled));
  assert(reconciled.phase==Phase::DEFERRED&&reconciled.work_remaining_ms==live.work_remaining_ms);
  puts("PASS: partial/late LCD failures arm once with live/cleared/expired lease; 25 identity/ownership negatives; cleanup-before-query; unchanged debit, caps, deadlines and missing-ACK settlement");
  assert(negatives==25);
}
'''
    return '\n'.join((prefix, types, boundaries, functions, cases))


class RetryPeerTests(unittest.TestCase):
    def test_partial_write_retry_and_refusals(self):
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler, 'Native C++ compiler is required')
        with tempfile.TemporaryDirectory(prefix='halo-retry-peer-') as tmp:
            cpp = Path(tmp) / 'retry.cpp'
            binary = Path(tmp) / 'retry'
            cpp.write_text(harness())
            subprocess.run([compiler, '-std=c++17', '-I', str(SHARED), str(cpp), '-o', str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()

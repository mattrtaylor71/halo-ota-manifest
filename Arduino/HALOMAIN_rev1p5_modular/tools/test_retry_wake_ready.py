"""Native regression for an early LCD retry wake joining pending recovery.

Extract the production readiness functions and use the real canonical codec,
arm and reservation operations. Only clock/storage/UART observations are fake.
No Arduino build, device or network is used.
"""
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SHARED = SOURCE / 'halo_ota_demo/firmware/shared'


def definition(text, signature, structure=False):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end + int(structure)]


def harness():
    runtime = (SHARED / 'SenseDurablePolicyRuntime.h').read_text()
    boundaries = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "DurableOtaDiscovery.h"
using durable_ota::Record;
using durable_ota::Phase;
static Record live;
static uint32_t now_ms, epoch, load_ms;
static bool clock_fresh, storage_ok, image_valid;
static bool g_boot_ota_pending, g_peer_episode_finished, g_ota_check_done;
static uint32_t g_boot_ota_deadline_ms, g_lcd_timer_seen_boot;
static const char* g_boot_ota_reason;
static struct {uint32_t boot_id; int wake; char schedule[64];} g_lcd_timer_origin;
static struct {bool active,ready,legacy; uint32_t peer_boot,deadline_ms;} g_peer_gate;
static unsigned cancel_calls, finish_calls;
static constexpr int ESP_SLEEP_WAKEUP_TIMER=4;
static uint32_t millis(){return now_ms;}
static bool nvs_capacity_image_valid(){return image_valid;}
static void ota_peer_cancel(const char*){++cancel_calls;g_peer_gate.active=false;g_peer_gate.ready=false;}
static void boot_ota_finish(const char*){++finish_calls;g_boot_ota_pending=false;}
static struct {template<class... A> void printf(const char*,A...) {}} Serial;
namespace sense_policy {
static const Record* current(){return &live;}
static bool absent(){return false;}
static bool load_state(uint32_t,uint32_t){now_ms+=load_ms;return storage_ok;}
static bool normal_entry(){return false;}
static durable_ota::Clock fresh_clock(bool normal=false){return {epoch,clock_fresh,normal};}
static bool bench_retry_phase(const Record&){return false;}
static bool bench_deferred_request(const Record&,char (&)[64]){return false;}
}
'''
    functions = '\n'.join((
        definition(runtime, 'struct PolicyReadinessObservation', True),
        'static PolicyReadinessObservation g_policy_readiness;',
        definition(runtime, 'static void halo_policy_note_readiness'),
        definition(runtime, 'static bool halo_policy_accepted_lcd_origin'),
        definition(runtime, 'static bool halo_policy_boot_ready'),
    ))
    cases = r'''
static void canonical_roundtrip(){
  uint8_t raw[durable_ota::kRecordBytes];Record decoded;
  assert(durable_ota::encode(live,raw));assert(durable_ota::decode(raw,sizeof(raw),decoded));
  live=decoded;
}
static void unchanged(const Record& before){
  uint8_t a[durable_ota::kRecordBytes],b[durable_ota::kRecordBytes];
  assert(durable_ota::encode(before,a)&&durable_ota::encode(live,b));
  assert(!memcmp(a,b,sizeof(a)));
}
static void baseline(const char* reason="lcd_timer"){
  constexpr uint32_t base=1789070722;
  durable_ota::Target target{};
  strcpy(target.version,"6.4.106");strcpy(target.peer_version,"6.4.106");
  strcpy(target.url,"https://example.com/sense.bin");target.sha256[0]=1;target.peer_sha256[0]=2;
  target.bytes=1803360;target.peer_bytes=1877264;
  uint8_t campaign[16]={1};Record r;
  assert(durable_ota::start_discovery("nightly_20260910",campaign,{base,true,true},false,false,live));
  assert(durable_ota::bind_discovery(live,{base+5,true,true},target,true,true,r));live=r;
  assert(durable_ota::reserve_apply(live,{base+5,true,true},5000,2400000,true,r));live=r;
  assert(durable_ota::reserve_begin(live,{base+15,true,true},1,r));live=r;
  assert(durable_ota::finish(live,{base+595,true,true},590000,true,
      durable_ota::Failure::TEMPORARY,0,0,base+86400,"retry_test_1",r));live=r;
  assert(durable_ota::confirm_arm(live,{base+596,true,true},live.arm_id,live.arm_epoch,
      300637462,300637462,true,true,r));live=r;
  assert(live.phase==Phase::ARMED&&live.generation==6&&live.network_windows==1);
  assert(live.day_attempts==1&&live.begins[1]==1&&live.arm_peer_boot==300637462);
  canonical_roundtrip();
  now_ms=10000;epoch=live.fast_due-10;load_ms=0;
  clock_fresh=storage_ok=image_valid=true;
  g_boot_ota_pending=true;g_peer_episode_finished=g_ota_check_done=false;
  g_boot_ota_reason=reason;g_boot_ota_deadline_ms=now_ms+120000;
  g_lcd_timer_seen_boot=700;
  g_lcd_timer_origin={};g_lcd_timer_origin.boot_id=700;
  g_lcd_timer_origin.wake=ESP_SLEEP_WAKEUP_TIMER;
  strcpy(g_lcd_timer_origin.schedule,live.arm_id);
  // The prior arm ACK names the old LCD boot. This accepted TIMER notice
  // and fresh queried peer name the new boot after that arm fired.
  g_peer_gate={true,true,false,700,now_ms+110000};
  cancel_calls=finish_calls=0;g_policy_readiness={};
}
static void rejected_origin(){
  const Record before=live;
  assert(!halo_policy_accepted_lcd_origin(live.arm_id));
  assert(!halo_policy_boot_ready());assert(!strcmp(g_policy_readiness.decision,"not_due"));
  assert(cancel_calls==1&&finish_calls==1&&!g_boot_ota_pending);
  assert(g_peer_episode_finished&&g_ota_check_done);unchanged(before);
}
int main(){
  unsigned negatives=0;
#define REJECT(change) do{baseline();change;rejected_origin();++negatives;}while(0)
  REJECT(strcpy(g_lcd_timer_origin.schedule,"unrelated"));
  REJECT(g_lcd_timer_origin.boot_id=0);
  REJECT(g_lcd_timer_seen_boot=701);
  REJECT(g_lcd_timer_origin.wake=2);
  REJECT(g_peer_gate.peer_boot=701);
  REJECT(g_peer_gate.legacy=true);
  REJECT(g_peer_gate.ready=false);
  REJECT(g_peer_gate.active=false);
  REJECT(g_lcd_timer_origin={});
  REJECT(g_boot_ota_pending=false);
#undef REJECT
  baseline();assert(!halo_policy_accepted_lcd_origin(nullptr));
  assert(!halo_policy_accepted_lcd_origin(""));
  baseline();clock_fresh=false;
  {const Record before=live;assert(!halo_policy_boot_ready());
   assert(!strcmp(g_policy_readiness.decision,"clock_wait"));
   assert(g_boot_ota_pending&&!cancel_calls&&!finish_calls);unchanged(before);}
  baseline();image_valid=false;assert(!halo_policy_boot_ready());
  assert(!strcmp(g_policy_readiness.decision,"local_not_valid"));
  baseline();storage_ok=false;assert(!halo_policy_boot_ready());
  assert(!strcmp(g_policy_readiness.decision,"storage_wait"));
  baseline();epoch=live.fast_expiry;assert(!halo_policy_boot_ready());
  assert(!strcmp(g_policy_readiness.decision,"expired")&&cancel_calls==1);
  baseline();g_boot_ota_deadline_ms=now_ms;assert(!halo_policy_boot_ready());
  assert(!strcmp(g_policy_readiness.decision,"deadline")&&!cancel_calls);
  baseline();load_ms=110001;assert(!halo_policy_boot_ready());
  assert(!strcmp(g_policy_readiness.decision,"deadline")&&!cancel_calls);
  // A due time equal to or beyond the remaining opportunity cannot be reached;
  // neither the peer nor boot deadline may be renewed by accepting an origin.
  for(uint32_t remaining:{9999U,10000U}){
    baseline();g_peer_gate.deadline_ms=now_ms+remaining;
    const uint32_t boot_end=g_boot_ota_deadline_ms,peer_end=g_peer_gate.deadline_ms;
    assert(!halo_policy_boot_ready());assert(!strcmp(g_policy_readiness.decision,"not_due"));
    assert(g_boot_ota_deadline_ms==boot_end&&g_peer_gate.deadline_ms==peer_end);
  }
  // No early wait exception is needed at the actual due time.
  baseline("coord_recovery");epoch=live.fast_due;g_lcd_timer_origin={};
  assert(halo_policy_boot_ready()&&!strcmp(g_policy_readiness.decision,"ready"));
  puts("PASS: ten wrong-origin refusals; stale clock, expiry, storage, deadline and exact due boundaries");
  for(const char* reason:{"lcd_timer","coord_recovery","lcd_due","nightly","policy_recovery"}){
    baseline(reason);const Record before=live;
    const uint32_t boot_end=g_boot_ota_deadline_ms,peer_end=g_peer_gate.deadline_ms;
    const bool accepted=halo_policy_accepted_lcd_origin(live.arm_id);
    const bool ready=halo_policy_boot_ready();
    if(!accepted||ready||strcmp(g_policy_readiness.decision,"wait_due")||
       !g_boot_ota_pending||cancel_calls||finish_calls){
      fprintf(stderr,"FAIL: exact accepted LCD retry origin with queued reason=%s became %s (accepted=%d, pending=%d)\n",
          reason,g_policy_readiness.decision,accepted,g_boot_ota_pending);return 1;
    }
    assert(g_boot_ota_deadline_ms==boot_end&&g_peer_gate.deadline_ms==peer_end);
    unchanged(before);
    // The canonical operation permits the existing15-second peer lead, but
    // this caller must not invoke it while its readiness result is wait_due.
    Record early;assert(durable_ota::reserve_preflight(live,{live.fast_due-16,true,false},false,early)==durable_ota::Admission::NOT_DUE);
    now_ms+=10000;epoch=live.fast_due;
    assert(halo_policy_boot_ready()&&!strcmp(g_policy_readiness.decision,"ready"));
    assert(g_boot_ota_deadline_ms==boot_end&&g_peer_gate.deadline_ms==peer_end);
    unchanged(before);
    Record retry;assert(durable_ota::reserve_preflight(live,{epoch,true,false},false,retry)==durable_ota::Admission::ALLOWED);
    assert(retry.network_windows==2&&retry.fast_opportunities==2&&retry.day_attempts==1);
    assert(retry.begins[1]==1&&retry.attempt_begins[1]==1);
    assert(retry.work_remaining_ms==before.work_remaining_ms-durable_ota::kPreflightMs);
    live=retry;canonical_roundtrip();
  }
  assert(negatives==10);
  puts("PASS: exact LCD TIMER origin survives five queued reasons until due with unchanged deadlines and canonical debit");
}
'''
    return '\n'.join((boundaries, functions, cases))


class RetryWakeTests(unittest.TestCase):
    def test_early_retry_origin_and_reservation(self):
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler, 'Native C++ compiler is required')
        with tempfile.TemporaryDirectory(prefix='halo-retry-wake-') as tmp:
            cpp, binary = Path(tmp) / 'retry.cpp', Path(tmp) / 'retry'
            cpp.write_text(harness())
            subprocess.run([compiler, '-std=c++17', '-I', str(SHARED), str(cpp), '-o', str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5,
                           preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))


if __name__ == '__main__':
    unittest.main()

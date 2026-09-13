"""Native regression for an early LCD retry wake joining pending recovery.

Extract the production readiness functions and use the real canonical codec,
arm and reservation operations. Only clock/storage/UART observations are fake.
No Arduino build, device or network is used.
"""
from pathlib import Path
import resource
import os
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
    if os.environ.get('HALO_READINESS_BASELINE_REF'):
        runtime = subprocess.check_output(['git','show',os.environ['HALO_READINESS_BASELINE_REF']+':Arduino/HALOMAIN_rev1p5_modular/halo_ota_demo/firmware/shared/SenseDurablePolicyRuntime.h'],cwd=SOURCE,text=True)
    sketch = (SHARED.parent / 'halo_sense_prod/halo_sense_prod.ino').read_text()
    caller = definition(sketch, 'static void nightly_maintenance_tick()')
    guard_start = caller.index('if (sense_action_inflight()')
    guard_end = caller.index('return;', guard_start) + len('return;')
    assert guard_start < caller.index('if (!ota_peer_ready()) return;')
    assert guard_end < caller.index('if (!self_retry_boot_admit()) return;')
    # Exercise the real caller guard; only its void-return is adapted so that
    # the harness can assert it. Busy state is not a policy-helper-local claim.
    caller_guard = ('static bool production_user_or_transport_idle(){' +
                    caller[guard_start:guard_end].replace('return;', 'return false;') +
                    'return true;}')
    boundaries = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "DurableOtaDiscovery.h"
#include "CoordinatorCreditState.h"
using durable_ota::Record;
using durable_ota::Phase;
static Record live;
static uint32_t now_ms, epoch, load_ms;
static bool clock_fresh, storage_ok, image_valid;
static bool g_boot_ota_pending, g_peer_episode_finished, g_ota_check_done;
static uint32_t g_boot_ota_deadline_ms, g_lcd_timer_seen_boot;
static const char* g_boot_ota_reason;
static NightlyCreditOrigin g_calendar_timer_origin;
static CoordinatorCreditState calendar_state;
static struct {bool pending;char schedule[64];} g_lcd_timer_notice;
static CoordinatorCreditState coord_credit_base(){return calendar_state;}
static struct {uint32_t boot_id; int wake; char schedule[64];} g_lcd_timer_origin;
static struct {bool active,ready,legacy; uint32_t peer_boot,deadline_ms;} g_peer_gate;
static unsigned cancel_calls, finish_calls;
static constexpr int ESP_SLEEP_WAKEUP_TIMER=4;
static uint32_t millis(){return now_ms;}
static bool nvs_capacity_image_valid(){return image_valid;}
static void ota_peer_cancel(const char*){++cancel_calls;g_peer_gate.active=false;g_peer_gate.ready=false;}
static void boot_ota_finish(const char*){++finish_calls;g_boot_ota_pending=false;}
static struct {template<class... A> void printf(const char*,A...) {}} Serial;
static bool action_inflight,foreground_active,voice_recording_active,g_list_screen_active;
static bool setup_mode,g_ota_check_in_progress,g_ota_apply_in_progress;
static bool g_lcd_ota_task_running,g_lcd_ota_proxy_owns_uart;
static void* op_queue;
static unsigned queue_messages;
static bool sense_action_inflight(){return action_inflight;}
static unsigned uxQueueMessagesWaiting(void*){return queue_messages;}
static struct {bool isSetupModeActive(){return setup_mode;}} g_provisioning_manager;
namespace sense_policy {
static const Record* current(){return &live;}
static bool absent(){return false;}
// Model the real load boundary with the real record shape check.
static bool load_state(uint32_t,uint32_t){now_ms+=load_ms;return storage_ok&&durable_ota::shape(live);}
static bool normal_entry();
static bool bench_deferred_due(const Record&,durable_ota::Clock){return false;}
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
        'namespace sense_policy {',
        definition(runtime, 'static uint32_t normal_calendar_due') if 'static uint32_t normal_calendar_due' in runtime else '',
        definition(runtime, 'static bool normal_entry()'),
        '}',
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
  constexpr uint32_t base=1789077853;
  durable_ota::Target target{};
  strcpy(target.version,"6.4.112");strcpy(target.peer_version,"6.4.112");
  strcpy(target.url,"https://example.com/sense.bin");target.sha256[0]=1;target.peer_sha256[0]=2;
  target.bytes=1803360;target.peer_bytes=1877264;
  uint8_t campaign[16]={1};Record r;
  assert(durable_ota::start_discovery("nightly_20260910",campaign,{base,true,true},false,false,live));
  assert(durable_ota::bind_discovery(live,{base+5,true,true},target,true,true,r));live=r;
  assert(durable_ota::reserve_apply(live,{base+5,true,true},5000,2400000,true,r));live=r;
  assert(durable_ota::reserve_begin(live,{base+15,true,true},1,r));live=r;
  assert(durable_ota::reserve_begin(live,{base+16,true,true},1,r));live=r;
  assert(durable_ota::finish(live,{base+569,true,true},564000,true,
      durable_ota::Failure::TEMPORARY,0,0,base+86400,"retry_test_1",r));live=r;
  assert(durable_ota::confirm_arm(live,{base+570,true,true},live.arm_id,live.arm_epoch,
      300637462,300637462,true,true,r));live=r;
  assert(live.phase==Phase::ARMED&&live.generation==7&&live.network_windows==1);
  assert(live.day_attempts==1&&live.begins[1]==2&&live.arm_peer_boot==300637462);
  assert(live.fast_due==1789078722);
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
  calendar_state={};g_calendar_timer_origin={};g_lcd_timer_notice={};
}
static void expect_wait(bool accepted=false){
  const Record before=live;
  const uint32_t boot_end=g_boot_ota_deadline_ms,peer_end=g_peer_gate.deadline_ms;
  assert(!halo_policy_boot_ready());
  assert(!strcmp(g_policy_readiness.decision,"wait_due"));
  assert(g_policy_readiness.accepted_origin==accepted);
  assert(g_boot_ota_pending&&!cancel_calls&&!finish_calls);
  assert(!g_peer_episode_finished&&!g_ota_check_done);
  assert(g_boot_ota_deadline_ms==boot_end&&g_peer_gate.deadline_ms==peer_end);
  unchanged(before);
}
static void expect_refusal(const char* decision,bool cancelled){
  const Record before=live;
  const uint32_t boot_end=g_boot_ota_deadline_ms,peer_end=g_peer_gate.deadline_ms;
  assert(!halo_policy_boot_ready());assert(!strcmp(g_policy_readiness.decision,decision));
  assert(cancel_calls==unsigned(cancelled)&&finish_calls==unsigned(cancelled));
  assert(g_boot_ota_deadline_ms==boot_end&&g_peer_gate.deadline_ms==peer_end);
  if(cancelled)assert(!g_boot_ota_pending&&g_peer_episode_finished&&g_ota_check_done);
  unchanged(before);
}
static void no_origin(){g_lcd_timer_origin={};g_lcd_timer_seen_boot=0;}
static void admit_once(){
  const Record before=live;
  assert(halo_policy_boot_ready()&&!strcmp(g_policy_readiness.decision,"ready"));
  assert(!g_policy_readiness.accepted_origin);unchanged(before);
  Record retry;assert(durable_ota::reserve_preflight(live,{epoch,true,false},false,retry)==durable_ota::Admission::ALLOWED);
  assert(retry.network_windows==before.network_windows+1&&retry.fast_opportunities==before.fast_opportunities+1);
  assert(retry.day_attempts==before.day_attempts&&retry.attempt_ordinal==before.attempt_ordinal);
  assert(retry.begins[0]==0&&retry.begins[1]==2&&retry.attempt_begins[1]==2);
  assert(retry.budget_day==before.budget_day&&retry.budget_granted==before.budget_granted);
  assert(retry.created==before.created&&durable_ota::same_target(retry.target,before.target));
  assert(retry.work_remaining_ms==before.work_remaining_ms-durable_ota::kPreflightMs);
  assert(retry.reserved_work_ms==durable_ota::kPreflightMs);
  live=retry;canonical_roundtrip();
}
static void calendar_baseline(bool discovery=false){
  baseline("coord_recovery");
  const uint32_t due=(live.high_water/86400+1)*86400+7200;
  Record next;
  if(discovery){
    uint8_t campaign[16]={9};
    assert(durable_ota::start_discovery("ordinary",campaign,{live.created,true,false},false,false,next));live=next;
    assert(durable_ota::close_discovery(live,{live.created+5,true,false},5000,true,due,next));
  }else assert(durable_ota::close_fast(live,{epoch,true,false},due,next));
  live=next;canonical_roundtrip();epoch=due-11;no_origin();
  setenv("TZ","UTC0",1);tzset();time_t time=due;struct tm tm;gmtime_r(&time,&tm);
  char id[64];strftime(id,sizeof(id),"nightly_%Y%m%d",&tm);
  assert(nightly_credit_bind(calendar_state.schedule,id,due,"UTC0",true));
  assert(credit_state_shape(calendar_state));
  // ota_peer_service has already consumed the mailbox. Its accepted TIMER
  // origin remains tied to this freshly queried boot.
  g_lcd_timer_origin.boot_id=g_lcd_timer_seen_boot=g_peer_gate.peer_boot;
  g_lcd_timer_origin.wake=ESP_SLEEP_WAKEUP_TIMER;
  strcpy(g_lcd_timer_origin.schedule,id);
  assert(!g_lcd_timer_notice.pending);
}
static bool calendar_tests(){
  calendar_baseline();
  const Record original=live;const uint32_t due=live.not_before;
  assert(!sense_policy::normal_entry());
  if(halo_policy_boot_ready()||strcmp(g_policy_readiness.decision,"wait_due")||cancel_calls||!g_boot_ota_pending){
    fprintf(stderr,"FAIL deferred calendar lead: decision=%s due=%u pending=%d normal=%d\\n",g_policy_readiness.decision,g_policy_readiness.due,g_boot_ota_pending,sense_policy::normal_entry());return false;
  }
  assert(g_policy_readiness.due==due);unchanged(original);
  for(bool discovery:{false,true}){
    for(uint32_t lead:{15U,11U,1U}){
      calendar_baseline(discovery);epoch=live.not_before-lead;
      const Record before=live;assert(!sense_policy::normal_entry());expect_wait();
      Record denied;
      const auto pre=discovery?durable_ota::reserve_discovery(live,{epoch,true,false},false,false,denied):durable_ota::reserve_preflight(live,{epoch,true,false},false,denied);
      assert(pre==durable_ota::Admission::NOT_DUE);unchanged(before);
    }
    calendar_baseline(discovery);epoch=live.not_before;
    assert(sense_policy::normal_entry()&&halo_policy_boot_ready());
    Record admitted;
    if(discovery){assert(durable_ota::reserve_discovery(live,{epoch,true,true},false,false,admitted)==durable_ota::Admission::ALLOWED);}
    else{
      Record rolled;assert(durable_ota::rollover(live,{epoch,true,true},rolled));
      assert(durable_ota::same_target(rolled.target,live.target)&&!strcmp(rolled.origin,live.origin));live=rolled;
      assert(durable_ota::reserve_preflight(live,{epoch,true,true},false,admitted)==durable_ota::Admission::ALLOWED);
    }
    assert(admitted.network_windows==1&&admitted.reserved_work_ms==durable_ota::kPreflightMs);
    live=admitted;canonical_roundtrip();Record duplicate;
    const auto dup=discovery?durable_ota::reserve_discovery(live,{epoch,true,true},false,false,duplicate):durable_ota::reserve_preflight(live,{epoch,true,true},false,duplicate);
    assert(dup==durable_ota::Admission::BUSY);
  }
  // Actual Sense calendar origin and the still-pending notice remain supported.
  calendar_baseline();g_calendar_timer_origin=calendar_state.schedule;no_origin();expect_wait();
  calendar_baseline();g_lcd_timer_notice.pending=true;strcpy(g_lcd_timer_notice.schedule,calendar_state.schedule.id);no_origin();expect_wait();
  calendar_baseline();calendar_state.deferred=calendar_state.schedule;calendar_state.schedule={};expect_wait();
  // Policy eligibility can be older than this morning's actual calendar arm.
  calendar_baseline();live.not_before-=3600;canonical_roundtrip();expect_wait();assert(g_policy_readiness.due==calendar_state.schedule.target_epoch);
#define CALENDAR_REFUSE(change) do{calendar_baseline();change;expect_refusal("not_due",true);}while(0)
  CALENDAR_REFUSE(epoch=live.not_before-16);
  CALENDAR_REFUSE(no_origin());CALENDAR_REFUSE(g_lcd_timer_origin.boot_id++);
  CALENDAR_REFUSE(g_lcd_timer_origin.wake=2);CALENDAR_REFUSE(strcpy(g_lcd_timer_origin.schedule,"nightly_19990101"));
  CALENDAR_REFUSE(calendar_state.schedule.bound=false);
  CALENDAR_REFUSE(g_peer_gate.legacy=true);CALENDAR_REFUSE(g_peer_gate.active=false);
  CALENDAR_REFUSE(g_peer_gate.ready=false);CALENDAR_REFUSE(g_peer_gate.peer_boot=0);
  CALENDAR_REFUSE(g_boot_ota_pending=false);
  CALENDAR_REFUSE(g_boot_ota_deadline_ms=now_ms+11000);
  CALENDAR_REFUSE(g_peer_gate.deadline_ms=now_ms+11000);
  CALENDAR_REFUSE(load_ms=1000;g_peer_gate.deadline_ms=now_ms+12000);
#undef CALENDAR_REFUSE
  calendar_baseline();clock_fresh=false;expect_refusal("clock_wait",false);
  calendar_baseline();epoch=live.not_before-1;g_peer_gate.deadline_ms=now_ms;expect_refusal("deadline",false);
  calendar_baseline();epoch=live.not_before+86400;assert(!sense_policy::normal_entry());expect_refusal("not_due",true);
  puts("PASS: DEFERRED/inactive DISCOVERY consumed calendar origin waits15/11/1s; actual due-only normal entry and single charged reservation; stale peer/origin/date/deadline refusals");
  return true;
}
int main(){
  if(!calendar_tests())return 42;
  // Observed109 failure: fresh due-12, no accepted notice, correlated peer.
  baseline("coord_recovery");epoch=1789078710;no_origin();
  g_boot_ota_deadline_ms=g_peer_gate.deadline_ms=now_ms+116733;
  const Record observed_before=live;
  const bool observed_ready=halo_policy_boot_ready();
  if(observed_ready||strcmp(g_policy_readiness.decision,"wait_due")||
     g_policy_readiness.accepted_origin||!g_boot_ota_pending||cancel_calls||finish_calls){
    fprintf(stderr,"FAIL: observed due-12 absent origin became %s (ready=%d, origin=%d, pending=%d, cancels=%u)\n",
        g_policy_readiness.decision,observed_ready,g_policy_readiness.accepted_origin,g_boot_ota_pending,cancel_calls);return 1;
  }
  unchanged(observed_before);
  assert(g_boot_ota_deadline_ms==126733&&g_peer_gate.deadline_ms==126733);

  // The codec itself allows the lead. The actual wrapper must ALWAYS return
  // false before due, so this proposal is not falsely tested at codec level.
  Record codec_early;
  assert(durable_ota::reserve_preflight(live,{epoch,true,false},false,codec_early)==durable_ota::Admission::ALLOWED);
  unchanged(observed_before);
  now_ms+=11000;epoch=live.fast_due-1;expect_wait();
  now_ms+=1000;epoch=live.fast_due;
  const uint32_t boot_end=g_boot_ota_deadline_ms,peer_end=g_peer_gate.deadline_ms;
  admit_once();
  assert(g_boot_ota_deadline_ms==boot_end&&g_peer_gate.deadline_ms==peer_end);
  // At observed due+4 the admitted path already has exactly one new charge.
  now_ms+=4000;epoch+=4;
  const Record charged=live;Record duplicate;
  assert(durable_ota::reserve_preflight(live,{epoch,true,false},false,duplicate)==durable_ota::Admission::BUSY);
  assert(live.network_windows==2&&live.day_attempts==1);unchanged(charged);
  // Also cover a first serviced tick four seconds after due, without a notice.
  baseline();no_origin();epoch=live.fast_due+4;admit_once();
  puts("PASS: observed due-12/no origin waits; due-1 unchanged; exact due charges once; due+4 cannot duplicate");

  for(const char* reason:{"lcd_timer","coord_recovery","lcd_due","nightly","policy_recovery"}){
    for(uint32_t lead:{15U,12U,1U}){
      baseline(reason);no_origin();epoch=live.fast_due-lead;expect_wait();
    }
  }
  // A wrong/missing notice no longer cancels a good current peer. It still
  // MUST NOT be reported as accepted. Old arm ACK boot != fresh retry boot.
  unsigned origin_refusals=0;
#define ORIGIN_FALSE(change) do{baseline();change;assert(!halo_policy_accepted_lcd_origin(live.arm_id));expect_wait();++origin_refusals;}while(0)
  ORIGIN_FALSE(strcpy(g_lcd_timer_origin.schedule,"unrelated"));
  ORIGIN_FALSE(g_lcd_timer_origin.boot_id=0);
  ORIGIN_FALSE(g_lcd_timer_seen_boot=701);
  ORIGIN_FALSE(g_lcd_timer_origin.wake=2);
  ORIGIN_FALSE(g_peer_gate.peer_boot=701);
  ORIGIN_FALSE(no_origin());
#undef ORIGIN_FALSE
  assert(origin_refusals==6);
  baseline();assert(live.arm_peer_boot!=g_peer_gate.peer_boot);
  assert(halo_policy_accepted_lcd_origin(live.arm_id));expect_wait(true);
  assert(!halo_policy_accepted_lcd_origin(nullptr)&&!halo_policy_accepted_lcd_origin(""));
  // A late notice changes only observation, not the deadline or canonical.
  baseline();const auto late_notice=g_lcd_timer_origin;no_origin();expect_wait();
  g_lcd_timer_origin=late_notice;g_lcd_timer_seen_boot=late_notice.boot_id;expect_wait(true);
  // Readiness can be evaluated stale, then fresh SNTP can arrive before the
  // pending notice is serviced. It must wait without inventing that notice.
  baseline();no_origin();clock_fresh=false;expect_refusal("clock_wait",false);
  clock_fresh=true;expect_wait();
  puts("PASS: five queue reasons,15-second lead, wrong/absent/late notices, truthful accepted_origin and SNTP ordering");

#define PEER_REFUSE(change) do{baseline();no_origin();change;expect_refusal("not_due",true);}while(0)
  PEER_REFUSE(g_peer_gate.legacy=true);
  PEER_REFUSE(g_peer_gate.ready=false);
  PEER_REFUSE(g_peer_gate.active=false);
  PEER_REFUSE(g_peer_gate.peer_boot=0);
  PEER_REFUSE(g_boot_ota_pending=false);
#undef PEER_REFUSE
  baseline();no_origin();epoch=live.fast_due-16;expect_refusal("not_due",true);
  baseline();no_origin();epoch=live.high_water-1;expect_refusal("clock_wait",false);
  baseline();image_valid=false;expect_refusal("local_not_valid",false);
  baseline();storage_ok=false;expect_refusal("storage_wait",false);
  baseline();live.arm_peer_boot=0;
  {const Record malformed=live;assert(!durable_ota::shape(live));
   assert(!halo_policy_boot_ready()&&!strcmp(g_policy_readiness.decision,"storage_wait"));
   assert(!memcmp(&live,&malformed,sizeof(live))&&!cancel_calls&&!finish_calls);}
  baseline();no_origin();epoch=live.fast_expiry;expect_refusal("expired",true);
  baseline();g_boot_ota_deadline_ms=now_ms;expect_refusal("deadline",false);
  baseline();g_peer_gate.deadline_ms=now_ms;expect_refusal("deadline",false);
  baseline();load_ms=110001;expect_refusal("deadline",false);
  // Due must fit STRICTLY inside BOTH original opportunities, including time
  // spent loading. A limit equal to due is not extended or accepted.
  for(bool boot_limit:{false,true})for(uint32_t remaining:{11999U,12000U}){
    baseline();no_origin();epoch=live.fast_due-12;
    (boot_limit?g_boot_ota_deadline_ms:g_peer_gate.deadline_ms)=now_ms+remaining;
    expect_refusal("not_due",true);
  }
  baseline();no_origin();epoch=live.fast_due-12;
  g_peer_gate.deadline_ms=now_ms+13000;load_ms=1000;expect_refusal("not_due",true);
  // At due, existing budget/busy guards still own reservation. Waiting never
  // replenishes exhausted credit and the wrapper is not a busy-state gate.
  baseline();no_origin();live.work_remaining_ms=0;canonical_roundtrip();expect_wait();
  epoch=live.fast_due;
  {const Record before=live;Record denied;assert(halo_policy_boot_ready());
   assert(durable_ota::reserve_preflight(live,{epoch,true,false},false,denied)==durable_ota::Admission::BUDGET);unchanged(before);}
  baseline();no_origin();epoch=live.fast_due;
  {const Record before=live;Record denied;assert(halo_policy_boot_ready());
   assert(durable_ota::reserve_preflight(live,{epoch,true,false},true,denied)==durable_ota::Admission::BUSY);unchanged(before);}
  // Each real caller guard remains ahead of policy readiness.
  for(bool* busy:{&action_inflight,&foreground_active,&voice_recording_active,
      &g_list_screen_active,&setup_mode,&g_ota_check_in_progress,
      &g_ota_apply_in_progress,&g_lcd_ota_task_running,&g_lcd_ota_proxy_owns_uart}){
    baseline();no_origin();const Record before=live;*busy=true;
    assert(!production_user_or_transport_idle());*busy=false;
    assert(!strcmp(g_policy_readiness.decision,"unobserved"));unchanged(before);
  }
  op_queue=&queue_messages;queue_messages=1;
  assert(!production_user_or_transport_idle());
  queue_messages=0;assert(production_user_or_transport_idle());op_queue=nullptr;
  puts("PASS: invalid peers, early16, stale/high-water, malformed/storage/localVALID, expiry/deadlines and budget refusals");
  puts("PASS: actual caller user/provisioning/queue/transport guards precede readiness");
}

'''
    return '\n'.join((boundaries, caller_guard, functions, cases))


class RetryWakeTests(unittest.TestCase):
    def test_early_retry_origin_and_reservation(self):
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler, 'Native C++ compiler is required')
        with tempfile.TemporaryDirectory(prefix='halo-retry-wake-') as tmp:
            cpp, binary = Path(tmp) / 'retry.cpp', Path(tmp) / 'retry'
            cpp.write_text(harness())
            subprocess.run([compiler, '-std=c++17', '-I', str(SHARED), str(cpp), '-o', str(binary)],
                           check=True, timeout=30)
            run=subprocess.run([str(binary)], timeout=5,
                           preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            if os.environ.get('HALO_READINESS_BASELINE_REF'):
                self.assertEqual(run.returncode,42,'Baseline must reproduce the precise early DEFERRED readiness failure')
            else:
                self.assertEqual(run.returncode,0)


if __name__ == '__main__':
    unittest.main()

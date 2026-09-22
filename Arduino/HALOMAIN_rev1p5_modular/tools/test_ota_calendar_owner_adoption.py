"""Actual timer mailbox/acceptance -> calendar credit -> durable discovery.

Host SDK/UART/NVS boundaries only; production readiness, credit codec/commit,
prepare-work and durable enter execute unchanged. The negative control removes
only the new calendar-origin gate to reproduce the ownerless lcd_due refusal.
No hardware, network, target completion or firmware build.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_manual_ota_readiness_join as join
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]
SHARED = ROOT / 'halo_ota_demo/firmware/shared'
WRAPPER = SHARED.parent / 'halo_sense_prod/halo_sense_prod.ino'


def harness(negative=False):
    wrapper = WRAPPER.read_text()
    runtime = (SHARED / 'SenseDurablePolicyRuntime.h').read_text()
    source = join.harness(ROOT, with_clock=False).split('int main(){', 1)[0]
    source = source.replace('static bool normal_entry(){return normal_window;}',
                            'static bool normal_entry();')
    source = source.replace('static uint32_t normal_calendar_due(){return 0;}',
                            'static uint32_t normal_calendar_due();')
    source = source.replace('static bool coord_credit_accepted_calendar(const CoordinatorCreditState&){return false;}',
                            'static bool coord_credit_accepted_calendar(const CoordinatorCreditState&);')
    # Use one common mailbox type, as in production. Accepted notice consumption
    # is the real service prefix; subsequent serial polling is outside this test.
    source = source.replace('static struct{bool pending=false;char schedule[64]{};}g_lcd_timer_notice;', r'''
struct TimerNotice{bool pending=false;char schedule[64]{};uint32_t boot_id=0,epoch=0;int wake=0,reset=0;bool resumed=false;};
static TimerNotice g_lcd_timer_notice;
''')
    source = source.replace('static struct{uint32_t boot_id=0;int wake=0;char schedule[64]{};}g_lcd_timer_origin;',
                            'static TimerNotice g_lcd_timer_origin;')
    source += r'''
static NightlyCreditOrigin g_calendar_timer_origin;
static uint32_t g_lcd_work_peer_boot=0;
static uint32_t g_coord_seen_boots[3]{};
static uint8_t g_coord_seen_count=0;
static unsigned queues=0,credit_writes=0;
static bool credit_fail_write=false,credit_fail_readback=false;
static uint32_t credit_write_delay=0;
static bool self_retry_notice_due(const char*){return false;}
static void ota_control_probe_service(){}
static bool coord_credit_notice_future_without_repair(const char*,uint64_t*){return false;}
static bool coord_credit_future_notice_wait(const char*,uint64_t){return false;}
static void boot_ota_queue(const char*){++queues;assert(false&&"existing request must retain its reason/deadline");}
struct CreditStore{
 std::vector<uint8_t> bytes;
 size_t putBytes(const char*,const uint8_t*b,size_t n){
  ++credit_writes;now_ms+=credit_write_delay;if(credit_fail_write)return 0;
  bytes.assign(b,b+n);return n;
 }
 size_t getBytesLength(const char*){return bytes.size();}
 size_t getBytes(const char*,uint8_t*b,size_t n){
  if(bytes.size()!=n)return 0;memcpy(b,bytes.data(),n);
  if(credit_fail_readback&&n)b[0]^=1;return n;
 }
}credit_store;
'''
    # Checked coordinator persistence uses the real codec/write/readback core.
    old_save = definition(source, 'static bool coord_credit_save(')
    source = source.replace(old_save, 'static bool coord_credit_save(const CoordinatorCreditState&candidate);')
    source += r'''
static bool coord_credit_save(const CoordinatorCreditState&candidate){
 if(!legacy_write_ok||!g_coord_credit_mutations||g_coord_credit_uncertain)return false;
 const bool ok=credit_commit(credit_store,candidate,g_coord_credit,g_coord_credit_mutations);
 if(!ok)g_coord_credit_uncertain=true;
 if(ok)coord_credit_publish();return ok;
}
'''
    source += '\n'.join(definition(wrapper, s) for s in (
        'void halo_prod_note_lcd_timer(', 'static bool coord_credit_notice_matches(',
        'static bool coord_credit_notice_ready()', 'static bool coord_credit_accepted_calendar(',
    )) + '\n'
    source += 'namespace sense_policy {\nstatic bool bench_deferred_due(const durable_ota::Record&,durable_ota::Clock){return false;}\n' + '\n'.join(definition(runtime, s) for s in (
        'static uint32_t normal_calendar_due()', 'static bool normal_entry()',
    )) + '\n}\n'
    service = definition(wrapper, 'static void ota_peer_service() {')
    prefix = service[:service.index('  if (!g_peer_gate.active || g_peer_gate.entered) return;')]
    source += prefix.replace('static void ota_peer_service()', 'static void accept_timer_notice()') + '}\n'
    if negative:
        old = 'base.deferred.id[0] || coord_credit_accepted_calendar(base) ||'
        assert old in source
        source = source.replace(old, 'base.deferred.id[0] ||')
    # Reuse exact source guard statements and their order at the real caller.
    common = definition(wrapper, 'static void maybeRunOtaCheck(')
    owner = 'if (!ota_primary_work_ready()) return;'
    prepare = 'if (!coord_credit_prepare_work(reason)) return;'
    recheck = 'if (!ota_peer_ready() || !ota_primary_work_ready()) return;'
    assert common.index(owner) < common.index(prepare) < common.index(recheck)
    source += r'''
static unsigned network_entries=0;
static void scheduled_pipeline(){
 const char* reason=g_boot_ota_reason;
 if(!g_boot_ota_pending||int32_t(now_ms-g_boot_ota_deadline_ms)>=0)return;
 if(!ota_peer_ready())return;
''' + owner + r'''
 if(!halo_policy_boot_ready())return;
''' + prepare + '\n' + recheck + r'''
 g_peer_gate.entered=true;g_lcd_work_budget={now_ms,120000};g_lcd_work_budget_live=true;
 if(sense_policy::enter(reason,true))++network_entries;
}
static void reset_calendar(const char* reason="lcd_due"){
 reset_join();using namespace sense_policy;
 epoch=1789635585;now_ms=10000;g_boot_ota_reason=reason;
 g_boot_ota_pending=true;g_peer_episode_finished=false;g_ota_check_done=false;
 g_peer_gate.active=g_peer_gate.locked=g_peer_gate.ready=true;g_peer_gate.entered=false;
 g_peer_gate.legacy=false;g_peer_gate.proof_ms=now_ms;g_peer_gate.peer_boot=12;
 g_boot_ota_deadline_ms=now_ms+45000;g_peer_gate.deadline_ms=now_ms+30000;
 g_coord_credit={};g_calendar_timer_origin={};
 CHECK(nightly_credit_bind(g_coord_credit.schedule,"nightly_20260917",1789635600,fixture_tz,true));
 strcpy(g_coord_schedule,g_coord_credit.schedule.id);g_coord_pending[0]=g_coord_completion_target[0]=0;
 g_lcd_timer_notice={};g_lcd_timer_origin={};g_lcd_timer_seen_boot=0;
 g_coord_seen_count=0;memset(g_coord_seen_boots,0,sizeof(g_coord_seen_boots));
 queues=credit_writes=network_entries=0;credit_store.bytes.clear();
 credit_fail_write=credit_fail_readback=false;credit_write_delay=0;
 manual_request=false;g_manual_ota_override=g_manual_ota_joined_readiness=false;
 work={};g_lcd_work_budget_live=false;events.clear();
}
static void notice(const char* id="nightly_20260917",uint32_t boot=12,int wake=ESP_SLEEP_WAKEUP_TIMER){
 halo_prod_note_lcd_timer(id,boot,wake,5,epoch,false);accept_timer_notice();
}
static void due(){epoch=1789635600;now_ms+=15000;g_peer_gate.proof_ms=now_ms;}
static void no_credit(){CHECK(!credit_writes&&!network_entries&&!g_coord_pending[0]);}
'''
    return source


class CalendarOwnerAdoptionTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory(prefix='halo-calendar-owner-') as directory:
            cpp, exe = Path(directory)/'test.cpp', Path(directory)/'test'
            cpp.write_text(code)
            built = subprocess.run([shutil.which('c++'), '-std=c++17', '-O1',
                                    '-I'+str(SHARED), str(cpp), '-o', str(exe)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            print(result.stdout, end='')

    def test_actual_timer_due_adopts_with_original_lease_and_native_charge(self):
        self.compile_run(harness()+r'''
int main(){using namespace sense_policy;
 for(const char* reason:{"lcd_due","policy_recovery","coord_recovery","lcd_timer","nightly"}){
  reset_calendar(reason);const auto old=state_record;const auto history=done_ids;
  const auto boot_dead=g_boot_ota_deadline_ms,peer_dead=g_peer_gate.deadline_ms;
  // Retained ownerless debt queued first. Real UART notice joins that request.
  notice();CHECK(g_lcd_timer_seen_boot==12&&!g_lcd_timer_notice.pending&&g_coord_seen_count==1);
  CHECK(coord_credit_accepted_calendar(g_coord_credit));
  scheduled_pipeline();no_credit();CHECK(g_boot_ota_pending&&g_peer_gate.active);
  CHECK(!memcmp(&state_record,&old,sizeof(old))&&done_ids==history);
  due();scheduled_pipeline();
  CHECK(network_entries==1&&credit_writes==1&&work.live&&work.normal);
  CHECK(!strcmp(g_coord_pending,"nightly_20260917")&&g_coord_credit.pending_credit_admitted);
  CHECK(g_coord_credit.admitted_epoch==epoch&&!strcmp(state_record.origin,g_coord_pending));
  CHECK(state_record.phase==durable_ota::Phase::DISCOVERY&&durable_ota::active_phase(state_record));
  CHECK(state_record.network_windows==1&&state_record.budget_day==epoch/86400);
  CHECK(state_record.day_attempts==0&&!state_record.begins[0]&&!state_record.begins[1]);
  CHECK(done_ids==history&&debt_value&&legacy_commits==0);
  CHECK(g_boot_ota_deadline_ms==boot_dead&&g_peer_gate.deadline_ms==peer_dead&&!strcmp(g_boot_ota_reason,reason)&&!queues);
  // Replayed notice and newer next arm cannot overwrite the admitted owner.
  const auto pending=g_coord_credit.pending;notice();CHECK(g_coord_seen_count==1);
  NightlyCreditOrigin next;CHECK(nightly_credit_bind(next,"nightly_20260918",1789722000,fixture_tz,true));
  CoordinatorCreditState candidate;CHECK(credit_next_arm(g_coord_credit,next,candidate));g_coord_credit=candidate;coord_credit_publish();
  CHECK(coord_credit_prepare_work(reason)&&credit_writes==1);
  CHECK(credit_same_origin(pending,g_coord_credit.pending));
  // Reset/reload keeps the exact checked owner and never adds a second credit.
  CoordinatorCreditState reloaded;bool writable=true;CHECK(credit_reload(credit_store,reloaded,writable));
  CHECK(credit_same_origin(reloaded.pending,pending)&&reloaded.pending_credit_admitted);
 }
 puts("PASS real timer mailbox/acceptance, early wait, due adoption, native bounded charge, immutable deadline/history and checked reload");}
''')

    def test_old_gate_reproduces_ownerless_refusal(self):
        self.compile_run(harness(negative=True)+r'''
int main(){reset_calendar();notice();due();const auto before=sense_policy::store.bytes;
 scheduled_pipeline();CHECK(!network_entries&&!credit_writes&&!g_coord_pending[0]);
 CHECK(!sense_policy::work.live&&sense_policy::store.bytes==before&&debt_value);
 puts("REPRODUCED old lcd_due gate: accepted real due TIMER has no pending owner and durable discovery refuses");}
''')

    def test_bad_proofs_clock_storage_user_and_deadlines_cannot_add_credit(self):
        self.compile_run(harness()+r'''
int main(){using namespace sense_policy;
 for(unsigned fault=0;fault<28;++fault){reset_calendar();notice();due();
  switch(fault){
   case 0:g_lcd_timer_origin.boot_id=0;break;
   case 1:g_lcd_timer_seen_boot=13;break;
   case 2:g_peer_gate.peer_boot=13;break;
   case 3:g_lcd_timer_origin.wake=2;break;
   case 4:g_peer_gate.legacy=true;break;
   case 5:strcpy(g_lcd_timer_origin.schedule,"nightly_20260918");break;
   case 6:g_coord_credit.schedule.bound=false;g_coord_credit.schedule.target_epoch=0;g_coord_credit.schedule.timezone[0]=0;break;
   case 7:g_peer_gate.proof_ms=now_ms-2000;break;
   case 8:clock_fresh=false;break;
   case 9:tz_ok=false;break;
   case 10:done_ids.push_back(g_coord_schedule);break;
   case 11:g_boot_ota_deadline_ms=now_ms;break;
   case 12:g_peer_gate.deadline_ms=now_ms;break;
   case 13:g_coord_credit_uncertain=true;break;
   case 14:g_ota_storage_uncertain=true;break;
   case 15:g_nvs_reclaim_uncertain=true;break;
   case 16:g_serial_install_uncertain=true;break;
   case 17:current_job.active=true;break;
   case 18:foreground_active=true;break;
   case 19:voice_recording_active=true;break;
   case 20:upload_count=1;break;
   case 21:http_inflight=true;break;
   case 22:g_boot_ota_pending=false;break;
   case 23:g_coord_sense_boot_id=0;break;
   case 24:g_lcd_timer_origin={};g_lcd_timer_notice.pending=true;break;
   case 25:g_coord_credit_mutations=false;break;
   case 26:g_coord_credit_loaded=false;break;
   case 27:local_valid=false;break;
  }
  const auto old=store.bytes;scheduled_pipeline();no_credit();CHECK(store.bytes==old&&debt_value);
 }
 // A reason string alone, user wake, malformed/unbound noncalendar notice or
 // future target cannot manufacture accepted due ownership.
 for(unsigned fault=0;fault<5;++fault){reset_calendar("lcd_due");
  if(fault==0)notice("nightly_20260917",12,2);
  if(fault==1)notice("nightly_20260917",0);
  if(fault==2)notice("nightly_20260918");
  if(fault==3){strcpy(g_coord_credit.schedule.id,"private_relative");g_coord_credit.schedule.bound=false;
   g_coord_credit.schedule.target_epoch=0;g_coord_credit.schedule.timezone[0]=0;strcpy(g_coord_schedule,"private_relative");notice("private_relative");}
  if(fault==4){epoch=1789635584;notice();}
  if(fault!=4)due();scheduled_pipeline();no_credit();
 }
 reset_calendar();notice();due();setenv("TZ","UTC0",1);scheduled_pipeline();no_credit();
 puts("PASS28 proof/clock/storage/user/deadline refusals plus wrong/user/unbound/future notices and actual timezone mismatch");}
''')

    def test_checked_save_pending_priority_and_blocking_write_expiry(self):
        self.compile_run(harness()+r'''
int main(){using namespace sense_policy;
 for(unsigned fault=0;fault<2;++fault){reset_calendar();notice();due();
  credit_fail_write=fault==0;credit_fail_readback=fault==1;
  const auto old=store.bytes;scheduled_pipeline();
  CHECK(credit_writes==1&&!network_entries&&!g_coord_pending[0]);
  CHECK(g_coord_credit_uncertain&&!g_coord_credit_mutations&&store.bytes==old&&debt_value);
  scheduled_pipeline();CHECK(credit_writes==1&&!network_entries);
 }
 // A later canonical policy write/readback failure also forbids network
 // admission; the already checked calendar owner remains available to reboot.
 for(unsigned fault=0;fault<2;++fault){reset_calendar();notice();due();
  fail_write=fault==0;fail_readback=fault==1;scheduled_pipeline();
  CHECK(credit_writes==1&&!network_entries&&g_coord_credit.pending_credit_admitted);
  CHECK(!work.live&&!state_allowed&&debt_value);
 }
 reset_calendar();notice();due();credit_write_delay=g_peer_gate.deadline_ms-now_ms;
 scheduled_pipeline();CHECK(credit_writes==1&&!network_entries&&g_coord_credit.pending_credit_admitted);
 CHECK(g_peer_gate.deadline_ms==now_ms&&debt_value); // persisted ownership, no late network
 reset_calendar();notice();due();
 CHECK(nightly_credit_bind(g_coord_credit.pending,"nightly_20260916",1789549200,fixture_tz,true));
 g_coord_credit.pending_credit_admitted=true;g_coord_credit.admitted_epoch=1789549200;coord_credit_publish();
 const auto pending=g_coord_credit.pending;CHECK(coord_credit_prepare_work("lcd_due"));
 CHECK(!credit_writes&&credit_same_origin(g_coord_credit.pending,pending));
 CHECK(!strcmp(g_coord_pending,"nightly_20260916"));
 // Readiness millisecond wrap preserves signed original-lease comparisons.
 reset_calendar();notice();due();now_ms=UINT32_MAX-999;g_boot_ota_deadline_ms=now_ms+3000;
 g_peer_gate.deadline_ms=now_ms+2000;g_peer_gate.proof_ms=now_ms;
 CHECK(coord_credit_accepted_calendar(g_coord_credit));now_ms+=2000;
 CHECK(!coord_credit_accepted_calendar(g_coord_credit));
 puts("PASS checked coordinator save/writeback fail-closed, deadline expiry during write, pending-first ownership and timer wrap");}
''')


if __name__ == '__main__':
    unittest.main()

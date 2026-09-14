"""Execute production calendar/cancellation seams with bounded host I/O stubs.

--baseline-source accepts a frozen wrapper to reproduce CASE9's early cancel.
No firmware build, network or hardware. Prefix tests stop before service I/O.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
import unittest
from test_manual_ota_clock import ROOT, definition

WRAPPER = ROOT / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino'
SHARED = ROOT / 'halo_ota_demo/firmware/shared'


def harness(source=None, baseline=False):
    source = WRAPPER.read_text() if source is None else source
    text = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include "CoordinatorCreditState.h"
#define HALO_DURABLE_OTA_POLICY 1
static constexpr const char* TZ_RULE="ACC-12:00:00";
static uint64_t epoch=1789394383ULL;
static uint32_t now_ms=4572, g_boot_ota_deadline_ms=122684;
static uint32_t millis(){return now_ms;}
static bool fresh=true, tz_confirmed=true, debt=false, image_valid=true;
static unsigned cancels=0, finishes=0, commits=0;
static bool g_boot_ota_pending=true, g_ota_check_done=false, g_ota_check_requested=false;
static const char* g_boot_ota_reason="lcd_timer";
static bool g_coord_credit_loaded=true,g_coord_credit_mutations=true,g_coord_credit_uncertain=false;
static bool g_nvs_reclaim_uncertain=false,g_serial_install_uncertain=false,g_ota_storage_uncertain=false;
static bool g_peer_continue_work=false,g_lcd_work_budget_live=false;
static bool g_ota_check_in_progress=false,g_ota_apply_in_progress=false,g_lcd_ota_task_running=false;
static bool g_peer_episode_finished=false,g_ota_pending_verify_active=false;
static char g_coord_pending[64]={},g_coord_completion_target[32]={};
static const char* kFirmwareVersion="6.4.155";
static uint32_t g_coord_sense_boot_id=939;
static struct {bool active=true,entered=false;uint32_t deadline_ms=122684;} g_peer_gate;
static struct {char schedule[64]="nightly_20260915";} g_lcd_timer_origin;
static struct {bool pending=true;char schedule[64]="nightly_20260915";} g_lcd_timer_notice;
static CoordinatorCreditState g_coord_credit;
static NightlyCreditOrigin g_calendar_timer_origin;
static std::recursive_mutex g_time_mutex,config_mutex;
struct ProvisioningState {static std::recursive_mutex& timezoneMutex(){return config_mutex;}};
static CoordinatorCreditState coord_credit_base(){return g_coord_credit;}
static bool coord_credit_timezone_matches_configuration(char*out){strcpy(out,TZ_RULE);return tz_confirmed;}
static void coord_credit_clock(uint64_t&now,bool&confirmed,char*tz,bool&finished){now=epoch;confirmed=fresh;strcpy(tz,TZ_RULE);finished=true;}
static bool get_lcd_ota_due_nvs(){return debt;}
static void ota_peer_cancel(const char*){++cancels;g_peer_gate.active=false;}
static void boot_ota_finish(const char*){++finishes;g_boot_ota_pending=false;}
static bool coord_credit_reserve_timer(){return true;}
static bool coord_credit_retire_configured_timezone(){return true;}
static bool ota_peer_schedule_completed(const char*){return false;}
static void ensure_timezone_pt(const char*){}
static bool halo_policy_short_due(){return false;}
static bool nvs_capacity_image_valid(){return image_valid;}
static bool coord_credit_save(const CoordinatorCreditState&next){
 ++commits;g_coord_credit=next;strcpy(g_coord_pending,next.pending.id);return true;
}
'''
    text += definition(source, 'static bool coord_credit_notice_future_without_repair(')
    if not baseline:
        text += definition(source, 'static bool coord_credit_future_notice_wait(')
    text += '\n'.join(definition(source, sig) for sig in (
        'static bool coord_credit_cancel_future_notice()',
        'static bool coord_credit_calendar_entry(',
        'static bool coord_credit_prepare_work(',
    ))
    # Exact source prefixes cover all three cancellation callers, including
    # future mailbox disposal. No modeled replacement of those decisions.
    peer = definition(source, 'static void ota_peer_service()')
    text += peer[:peer.index('  ota_control_probe_service();')] + '}\n'
    nightly = definition(source, 'static void nightly_maintenance_tick()')
    text += nightly[:nightly.index('  const bool nightly =')] + '}\n'
    text += r'''
static void reset(){
 epoch=1789394383ULL;now_ms=4572;g_boot_ota_deadline_ms=122684;
 fresh=tz_confirmed=image_valid=true;debt=false;cancels=finishes=commits=0;
 g_boot_ota_pending=true;g_boot_ota_reason="lcd_timer";g_peer_gate={true,false,122684};
 g_coord_credit_loaded=g_coord_credit_mutations=true;
 g_coord_credit_uncertain=g_nvs_reclaim_uncertain=g_serial_install_uncertain=g_ota_storage_uncertain=false;
 g_peer_continue_work=g_lcd_work_budget_live=false;
 g_ota_check_requested=g_ota_check_done=g_ota_check_in_progress=g_ota_apply_in_progress=g_lcd_ota_task_running=false;
 g_coord_pending[0]=g_coord_completion_target[0]=0;g_peer_episode_finished=g_ota_pending_verify_active=false;
 strcpy(g_lcd_timer_origin.schedule,"nightly_20260915");
 g_lcd_timer_notice.pending=true;strcpy(g_lcd_timer_notice.schedule,"nightly_20260915");
 setenv("TZ",TZ_RULE,1);tzset();g_coord_credit={};
 assert(nightly_credit_bind(g_coord_credit.schedule,"nightly_20260915",1789394400,TZ_RULE,true));
 g_calendar_timer_origin=g_coord_credit.schedule;
}
'''
    return text


class FutureCalendarWaitTests(unittest.TestCase):
    def compile_run(self, code):
        with tempfile.TemporaryDirectory(prefix='halo-calendar-wait-') as folder:
            cpp, binary = Path(folder)/'check.cpp', Path(folder)/'check'
            cpp.write_text(code)
            subprocess.run([shutil.which('c++'), '-std=c++17', '-I', str(SHARED), str(cpp), '-o', str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_case9_actual_three_callers_wait_then_due(self):
        self.compile_run(harness()+r'''
int main(){reset();const auto original=g_coord_credit;const auto boot=g_boot_ota_deadline_ms,peer=g_peer_gate.deadline_ms;
 assert(nightly_credit_decide(original.schedule,original.schedule.id,epoch,true,TZ_RULE)==NightlyCreditDecision::Future);
 // Actual repeated service/tick/work-entry callers cannot consume a2s wait.
 for(unsigned i=0;i<3;++i){ota_peer_service();nightly_maintenance_tick();assert(!coord_credit_prepare_work("lcd_timer"));}
 assert(!cancels&&!finishes&&!commits&&g_boot_ota_pending&&g_lcd_timer_notice.pending);
 assert(g_boot_ota_deadline_ms==boot&&g_peer_gate.deadline_ms==peer&&!g_peer_gate.entered);
 assert(!memcmp(&original,&g_coord_credit,sizeof(original)));
 ++epoch;now_ms+=1000;ota_peer_service();nightly_maintenance_tick();assert(!coord_credit_prepare_work("lcd_timer"));
 assert(!commits&&!cancels);++epoch;now_ms+=1000;
 assert(nightly_credit_decide(original.schedule,original.schedule.id,epoch,true,TZ_RULE)==NightlyCreditDecision::Due);
 nightly_maintenance_tick();assert(coord_credit_prepare_work("lcd_timer"));
 assert(commits==1&&g_coord_credit.pending_credit_admitted&&g_coord_credit.admitted_epoch==epoch);
 assert(g_coord_credit.pending.target_epoch==1789394400&&!cancels&&!finishes);
 assert(coord_credit_prepare_work("lcd_timer"));assert(commits==1);
 puts("PASS exactCASE9: all three source callers WAIT without writes/renewal, then actual15s-boundary credit once");}
''')

    def test_deadlines_wrap_and_initial_notice(self):
        self.compile_run(harness()+r'''
int main(){
 for(unsigned which=0;which<2;++which){
  reset();if(which)g_peer_gate.deadline_ms=now_ms+2001;else g_boot_ota_deadline_ms=now_ms+2001;
  ota_peer_service();assert(!cancels&&g_boot_ota_pending&&g_lcd_timer_notice.pending);
  reset();if(which)g_peer_gate.deadline_ms=now_ms+2000;else g_boot_ota_deadline_ms=now_ms+2000;
  ota_peer_service();assert(cancels==1&&finishes==1&&!g_boot_ota_pending&&!g_lcd_timer_notice.pending&&!commits);
  reset();if(which)g_peer_gate.deadline_ms=now_ms;else g_boot_ota_deadline_ms=now_ms;
  assert(coord_credit_cancel_future_notice());assert(!commits);
 }
 reset();now_ms=UINT32_MAX-999;g_boot_ota_deadline_ms=now_ms+3000;g_peer_gate.deadline_ms=now_ms+4000;
 assert(!coord_credit_cancel_future_notice());now_ms+=1000;++epoch;
 assert(!coord_credit_cancel_future_notice());assert(!commits);
 reset();epoch-=200;ota_peer_service();assert(cancels==1&&!commits);
 reset();g_boot_ota_pending=false;ota_peer_service();assert(!g_lcd_timer_notice.pending&&!cancels&&!commits);
 reset();g_peer_gate.active=false;assert(!coord_credit_cancel_future_notice());assert(!commits);
 puts("PASS exact deadline equality/expiry, far future, wrapping monotonic clock and no new readiness request");}
''')

    def test_clock_identity_and_independent_repair(self):
        self.compile_run(harness()+r'''
int main(){
 for(unsigned failure=0;failure<7;++failure){reset();
  if(failure==0)fresh=false;if(failure==1)tz_confirmed=false;
  if(failure==2)strcpy(g_lcd_timer_origin.schedule,"nightly_20260916");
  if(failure==3)g_coord_credit_uncertain=true;if(failure==4)debt=true;
  if(failure==5)strcpy(g_coord_pending,"owed_campaign");if(failure==6)g_peer_gate.entered=true;
  assert(!coord_credit_cancel_future_notice());assert(!cancels&&!finishes&&!commits);
 }
 reset();assert(!coord_credit_future_notice_wait("nightly_20260916",2000));
 reset();g_boot_ota_reason="manual";assert(!coord_credit_cancel_future_notice());
 assert(!coord_credit_future_notice_wait(g_lcd_timer_origin.schedule,2000));
 reset();fresh=false;ota_peer_service();nightly_maintenance_tick();assert(!coord_credit_prepare_work("lcd_timer"));
 assert(!commits&&!cancels&&g_boot_ota_pending);
 reset();g_peer_continue_work=true;assert(coord_credit_prepare_work("lcd_timer"));assert(!commits&&!cancels);
 puts("PASS stale clock, wrong identity/configuration, uncertainty, manual and independent repair retain existing behavior");}
''')

    def test_actual_admission_keeps_fresh_peer_and_clock_gates(self):
        source = WRAPPER.read_text()
        common = definition(source, 'static void maybeRunOtaCheck(')
        entry = common.index('if (!coord_credit_prepare_work(reason)) return;')
        before, after = common[:entry], common[entry:]
        self.assertLess(before.index('if (!ota_clock_ready_before_work()) return;'), before.rindex('if (!ota_peer_ready()) return;'))
        self.assertLess(after.index('if (!ota_peer_ready()) return;'), after.index('g_peer_gate.entered = true;'))
        self.assertLess(after.index('g_peer_gate.entered = true;'), after.index('sense_policy::enter(reason,retained_legacy)'))
        service = definition(source, 'static void ota_peer_service()')
        self.assertLess(service.index('!coord_credit_future_notice_wait('), service.index('g_lcd_timer_notice.pending = false;'))


def reproduce_baseline(path):
    code = harness(Path(path).read_text(), baseline=True)+r'''
int main(){reset();ota_peer_service();assert(cancels==1&&finishes==1&&!g_boot_ota_pending&&!g_lcd_timer_notice.pending&&!commits);
 epoch=1789394416;const auto seconds=halo_seconds_until_maintenance(epoch);
 assert(seconds==86384&&epoch+seconds==1789480800);
 puts("REPRODUCED frozen155 CASE9: fresh4383 cancels2s-early request; final4416 calendar arms1789480800, no credit");}
'''
    FutureCalendarWaitTests().compile_run(code)


if __name__ == '__main__':
    parser = argparse.ArgumentParser();parser.add_argument('--baseline-source');args,remaining=parser.parse_known_args()
    if args.baseline_source:reproduce_baseline(args.baseline_source)
    else:unittest.main(argv=[__file__]+remaining)

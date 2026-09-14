"""Host execution of actual scheduled clock eligibility and SNTP generations.

No hardware/network writes. --baseline-ref executes the old source to reproduce
the clean scheduled wake's single expired SNTP opportunity.
"""
from pathlib import Path
import argparse
import subprocess
import unittest
import test_manual_ota_clock as manual_clock
from test_manual_ota_clock import ROOT, definition, time_harness


TIME = ROOT / 'Sense_Minimal/sense_time.h'
WRAPPER = ROOT / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino'


def scheduled_time_harness(source=None, baseline=False):
    text = time_harness(source)
    text = text[:text.index('int main(){')]
    if baseline:
        return text + r'''
int main(){
 reset();sense_ntp_begin();assert(starts==1);expire();sense_ntp_begin();
 assert(starts==1&&!sense_time_has_fresh_sync()&&!sense_ntp_attempt_pending());
 puts("REPRODUCED baseline clean scheduled clock: no second server opportunity");
}
'''
    return text + r'''
int main(){
 timeval tv{};tv.tv_sec=1800000000;
 // Qualified scheduled request uses distinct callbacks and rotates the server.
 reset();sense_ntp_begin();expire();sense_ntp_request_scheduled_retry(now_ms+100000);
 sense_ntp_begin();assert(starts==2&&g_ntp_attempt_budget_ms==40000);
 assert(!g_ntp_manual_retry_requested&&selected_servers[0]=="10.0.0.2");
 const auto original_deadline=g_ntp_accept_until_ms.load();
 sense_ntp_on_sync(&tv);assert(g_ntp_received_epoch==0);
 now_ms+=1000;sense_ntp_request_scheduled_retry(now_ms+120000);sense_ntp_begin();
 assert(starts==2&&g_ntp_accept_until_ms==original_deadline);
 sync_callback(&tv);sense_ntp_service();assert(sense_time_has_fresh_sync());
 // Failure consumes the sole generation. Repeated service cannot reset it.
 reset();sense_ntp_begin();expire();sense_ntp_request_scheduled_retry(now_ms+40000);
 sense_ntp_begin();expire();sync_callback(&tv);sense_ntp_service();
 for(unsigned i=0;i<8;++i){sense_ntp_request_scheduled_retry(now_ms+40000);sense_ntp_begin();}
 assert(starts==2&&!sense_time_has_fresh_sync()&&!sense_ntp_attempt_pending());
 // Both UDP and DNS stop at the original shorter readiness deadline.
 reset();dns_available=false;sense_ntp_begin();expire();
 sense_ntp_request_scheduled_retry(now_ms+7000);
 sense_ntp_request_scheduled_retry(now_ms+60000);sense_ntp_begin();
 assert(g_ntp_attempt_budget_ms==7000&&g_ntp_manual_resolve_until_ms==now_ms+7000);
 now_ms+=6999;dns_reply(3,11);sense_ntp_begin();assert(starts==1);
 now_ms+=1;dns_reply(4,22);sync_callback(&tv);sense_ntp_service();
 assert(g_ntp_manual_servers[1].ipv4==0&&!sense_time_has_fresh_sync());
 // Expired admission is refused, including exact boundary and wrap.
 reset();sense_ntp_begin();expire();sense_ntp_request_scheduled_retry(now_ms);
 sense_ntp_begin();assert(starts==1&&!g_ntp_manual_retry_used);
 reset();now_ms=UINT32_MAX-20000;sense_ntp_begin();expire();
 sense_ntp_request_scheduled_retry(now_ms+30000);sense_ntp_begin();
 assert(g_ntp_attempt_budget_ms==30000);now_ms+=30000;sense_ntp_service();
 assert(!sense_ntp_attempt_pending());
 // Timely original reply wins; an expired callback cannot cross generations.
 reset();sense_ntp_begin();now_ms+=14999;sense_ntp_on_sync(&tv);
 now_ms+=2;sense_ntp_request_scheduled_retry(now_ms+40000);
 sense_ntp_service();sense_ntp_begin();assert(starts==1&&sense_time_has_fresh_sync());
 // Nested DNS holds consume the fixed window and cannot restart it on release.
 reset();sense_ntp_begin();expire();halo_sntp_dns_acquire();halo_sntp_dns_acquire();
 sense_ntp_request_scheduled_retry(now_ms+9000);sense_ntp_begin();assert(starts==1);
 now_ms+=9000;halo_sntp_dns_release();halo_sntp_dns_release();sense_ntp_service();sense_ntp_begin();
 assert(starts==1&&!sense_time_has_fresh_sync()&&!sense_ntp_attempt_pending());
 // Existing active policy55s and manual40s remain unchanged.
 reset(55000);sense_ntp_begin();expire();sense_ntp_request_scheduled_retry(now_ms+120000);
 sense_ntp_begin();assert(starts==1&&g_ntp_attempt_budget_ms==55000);
 reset();sense_ntp_begin();expire();sense_ntp_request_manual_retry();sense_ntp_begin();
 assert(starts==2&&g_ntp_attempt_budget_ms==40000);
 sense_ntp_request_scheduled_retry(now_ms+1);assert(g_ntp_attempt_budget_ms==40000);
 puts("PASS scheduled clock: alternate server, one generation, bounded DNS/UDP, late replies, guard expiry, wrap, manual and active-policy controls");
}
'''


def eligibility_harness():
    source = WRAPPER.read_text()
    return r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#define HALO_DURABLE_OTA_POLICY 1
static constexpr int ESP_SLEEP_WAKEUP_TIMER=4;
static uint32_t now_ms=20000;
static uint32_t tick_ms=0;
static uint32_t millis(){const uint32_t result=now_ms;now_ms+=tick_ms;return result;}
static bool manual=false,typed_absent=true,completed=false,tz_matches=true;
static bool halo_ota_manual_override_active(){return manual;}
namespace sense_policy {static bool absent(){return typed_absent;}}
static bool g_boot_ota_pending=true,g_coord_credit_loaded=true;
static bool g_coord_credit_uncertain=false,g_ota_storage_uncertain=false,g_peer_continue_work=false;
static bool g_tz_initialized=true;
static const char*g_tz_current="ACC-19:00:00";
static std::recursive_mutex g_time_mutex;
static char g_coord_pending[64]={},g_coord_completion_target[32]={};
static uint32_t g_boot_ota_deadline_ms=120000,g_lcd_timer_seen_boot=123;
struct Peer{bool active=true,ready=true,legacy=false,entered=false;
 uint32_t peer_boot=123,proof_ms=19500,deadline_ms=110000;}g_peer_gate;
struct Notice{uint32_t boot_id=123;int wake=ESP_SLEEP_WAKEUP_TIMER;
 char schedule[64]="nightly_20260915";}g_lcd_timer_origin;
struct NightlyCreditOrigin{char id[64]={},timezone[64]={};uint32_t target_epoch=0;bool bound=false;};
struct CoordinatorCreditState{NightlyCreditOrigin schedule,deferred;}base;
static CoordinatorCreditState coord_credit_base(){return base;}
static bool ota_peer_schedule_completed(const char*){return completed;}
static bool nightly_credit_timezone_matches(const char*){return tz_matches;}
''' + definition(source, 'static uint32_t ota_scheduled_clock_retry_deadline()') + r'''
int main(){
 strcpy(base.schedule.id,g_lcd_timer_origin.schedule);strcpy(base.schedule.timezone,g_tz_current);
 base.schedule.target_epoch=1789369200;base.schedule.bound=true;
 assert(ota_scheduled_clock_retry_deadline()==110000); // typed absent, genuine timer
 typed_absent=false;assert(!ota_scheduled_clock_retry_deadline());typed_absent=true;
 manual=true;assert(!ota_scheduled_clock_retry_deadline());manual=false;
 g_boot_ota_pending=false;assert(!ota_scheduled_clock_retry_deadline());g_boot_ota_pending=true;
 g_lcd_timer_origin.wake=2;assert(!ota_scheduled_clock_retry_deadline());g_lcd_timer_origin.wake=4;
 g_peer_gate.peer_boot=124;assert(!ota_scheduled_clock_retry_deadline());g_peer_gate.peer_boot=123;
 g_peer_gate.legacy=true;assert(!ota_scheduled_clock_retry_deadline());g_peer_gate.legacy=false;
 g_peer_gate.ready=false;assert(!ota_scheduled_clock_retry_deadline());g_peer_gate.ready=true;
 g_peer_gate.proof_ms=now_ms-2000;assert(!ota_scheduled_clock_retry_deadline());g_peer_gate.proof_ms=now_ms;
 g_peer_gate.deadline_ms=now_ms;assert(!ota_scheduled_clock_retry_deadline());g_peer_gate.deadline_ms=110000;
 g_boot_ota_deadline_ms=now_ms;assert(!ota_scheduled_clock_retry_deadline());g_boot_ota_deadline_ms=100000;
 assert(ota_scheduled_clock_retry_deadline()==100000);
 completed=true;assert(!ota_scheduled_clock_retry_deadline());completed=false;
 base.schedule.bound=false;assert(!ota_scheduled_clock_retry_deadline());base.schedule.bound=true;
 g_coord_pending[0]='x';assert(!ota_scheduled_clock_retry_deadline());g_coord_pending[0]=0;
 g_coord_credit_uncertain=true;assert(!ota_scheduled_clock_retry_deadline());g_coord_credit_uncertain=false;
 g_ota_storage_uncertain=true;assert(!ota_scheduled_clock_retry_deadline());g_ota_storage_uncertain=false;
 tz_matches=false;assert(!ota_scheduled_clock_retry_deadline());tz_matches=true;
 strcpy(base.schedule.timezone,"PST8PDT");assert(!ota_scheduled_clock_retry_deadline());
 strcpy(base.schedule.timezone,g_tz_current);
 base.deferred=base.schedule;base.schedule={};assert(ota_scheduled_clock_retry_deadline()==100000);
 // Merely stored bound calendar without original observed LCD boot is refused.
 g_lcd_timer_origin.boot_id=0;assert(!ota_scheduled_clock_retry_deadline());
 puts("PASS actual scheduled eligibility: absent policy, exact timer/peer/TZ, both deadlines, no manual/debt/storage/fallback bypass");
}
'''


class ScheduledClockTests(unittest.TestCase):
    def compile_run(self, text):
        manual_clock.ManualOtaClockTests().compile_run(text)

    def test_scheduled_generation(self):
        self.compile_run(scheduled_time_harness())

    def test_actual_scheduled_eligibility(self):
        self.compile_run(eligibility_harness())

    def test_actual_scheduled_terminal(self):
        text = manual_clock.terminal_harness().split('int main(){')[0]
        text = text.replace('static unsigned ota_scheduled_clock_retry_deadline(){return 0;}',
                            'static unsigned ota_scheduled_clock_retry_deadline(){return 100000;}')
        text = text.replace('static void sense_ntp_request_scheduled_retry(unsigned){assert(false);}',
                            'static unsigned scheduled_requests=0; static void sense_ntp_request_scheduled_retry(unsigned d){assert(d==100000);if(++scheduled_requests==1)retry_ready=true;}')
        self.compile_run(text + r'''
int main(){
 g_boot_ota_pending=true;manual=false;fresh=false;pending=false;
 assert(!ota_clock_ready_before_work());
 assert(pending&&retries==0&&scheduled_requests==1&&wire_result.empty()&&clears==0);
 // Exhaustion cannot grant work, refund allowance or promise a sleep retry.
 pending=false;assert(!ota_clock_ready_before_work());
 assert(retries==0&&wire_result=="clock_unconfirmed"&&clears==1&&finishes==1);
 assert(g_peer_episode_finished&&g_ota_check_done&&!g_ota_check_requested);
 fresh=true;assert(ota_clock_ready_before_work());
 puts("PASS actual scheduled preflight: pending is not admission; exhaustion terminates honestly without manual intent");
}
''')

    def test_actual_peer_expiry_composition(self):
        # Compile the actual eligibility, stale-proof wait, and common terminal
        # together. The first caller observes1999ms; the inner gate sees2000ms.
        source = WRAPPER.read_text()
        text = eligibility_harness().split('int main(){')[0]
        text += definition(source, 'static bool ota_scheduled_clock_peer_refresh_pending()')
        text += r'''
static bool fresh=false,pending=false,used=false,g_peer_episode_finished=false;
static bool g_ota_check_done=false,g_ota_check_requested=false;
static unsigned scheduled=0,terminals=0;
static bool sense_time_has_fresh_sync(){return fresh;}
static void sense_ntp_request_manual_retry(){assert(false);}
static void sense_ntp_request_scheduled_retry(uint32_t){if(!used){used=true;++scheduled;pending=true;}}
static void halo_prod_kick_time_sync(const char*){}
static bool sense_ntp_attempt_pending(){return pending;}
static void sense_ntp_service(){}
static void sense_ntp_begin(){}
#define LOG_ERROR(...) ((void)0)
static void diag_record_error_persistent(const char*,int,const char*){}
static void ota_set_last_result(const char*s){assert(!strcmp(s,"clock_unconfirmed"));}
static void ota_peer_cancel(const char*){++terminals;}
struct OtaIntent{static void clearForceAndCheck(){}};
static void manual_ota_override_clear(const char*){}
static void boot_ota_finish(const char*){}
'''
        text += definition(source, 'static bool ota_clock_ready_before_work()')
        text += r'''
int main(){
 strcpy(base.schedule.id,g_lcd_timer_origin.schedule);strcpy(base.schedule.timezone,g_tz_current);
 base.schedule.target_epoch=1789369200;base.schedule.bound=true;
 g_peer_gate.proof_ms=now_ms-1999;assert(ota_scheduled_clock_retry_deadline());
 ++now_ms;assert(!ota_clock_ready_before_work()&&scheduled==0&&terminals==0);
 g_peer_gate.ready=false;assert(!ota_clock_ready_before_work()&&terminals==0);
 g_peer_gate.ready=true;g_peer_gate.proof_ms=now_ms;
 assert(!ota_clock_ready_before_work()&&scheduled==1&&pending&&terminals==0);
 pending=false;assert(!ota_clock_ready_before_work()&&scheduled==1&&terminals==1);
 // The internal calls can straddle the same boundary too. A qualified first
 // snapshot is consumed once; a prior WAIT then second eligibility would lose it.
 used=false;pending=false;scheduled=terminals=0;now_ms=20000;
 g_peer_gate.proof_ms=18001;tick_ms=1;
 assert(!ota_clock_ready_before_work()&&scheduled==1&&pending&&terminals==0);
 tick_ms=0;
 g_peer_gate.proof_ms=now_ms-2000;g_boot_ota_deadline_ms=now_ms;
 assert(!ota_scheduled_clock_peer_refresh_pending());
 puts("PASS composed clock boundary: stale proof waits, refreshed peer gets one attempt, exhaustion terminates within original deadline");
}
'''
        self.compile_run(text)

    def test_automatic_call_order(self):
        body = definition(WRAPPER.read_text(), 'static void nightly_maintenance_tick()')
        admit = body.index('if (!self_retry_boot_admit()) return;')
        clock = body.index('if (ota_scheduled_clock_retry_deadline() && !ota_clock_ready_before_work()) return;')
        retained = body.index('if (!is_time_valid()')
        manifest = body.index('maybeRunOtaCheck(g_boot_ota_reason, true)')
        self.assertLess(admit, clock)
        self.assertLess(clock, retained)
        self.assertLess(retained, manifest)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-ref')
    args = parser.parse_args()
    if args.baseline_ref:
        old = subprocess.check_output(['git', 'show', args.baseline_ref + ':./' + str(TIME.relative_to(ROOT))], cwd=ROOT, text=True)
        manual_clock.ManualOtaClockTests().compile_run(scheduled_time_harness(old, baseline=True))
    else:
        unittest.main(argv=[__file__])

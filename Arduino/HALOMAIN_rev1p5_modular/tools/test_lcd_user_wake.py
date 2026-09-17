"""Run the real LCD wake/ping/RX functions against a deterministic host clock.

Only GPIO, UART transport and unrelated UI/refresh effects are stubbed. The
SLEEP_READY, PONG and missed-pong transitions are extracted from production.
Use --source with an old firmware tree (or LCD_Minimal.ino) to reproduce failure.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]
ARGS = argparse.Namespace(source=None)


def harness(source, rx, uart, anim):
    names = ('WAKE_RETRY_INTERVAL_MS', 'WAKE_RETRY_WINDOW_MS',
             'WAKE_TIMER_WAIT_WINDOW_MS',
             'WAKE_PULSE_DURATION_MS', 'WAKE_PULSE_SHORT_MS',
             'SENSE_PONG_TIMEOUT_MS', 'SENSE_MISSED_PONGS_FOR_ASLEEP',
             'SENSE_PING_MIN_INTERVAL_MS', 'SENSE_AWAKE_TRUST_MS',
             'REFRESH_WAKE_PING_INTERVAL_MS')
    constants = '\n'.join(re.search(
        r'^static const [^;\n]+\b' + name + r'\s*=\s*[^;]+;', source,
        re.MULTILINE).group() for name in names)
    signatures = [
        'static const char* sense_state_name(',
        'static void sense_state_set(',
        'static void set_sense_awake_estimate(',
        'static void note_sense_proof_of_life(',
        'static bool should_wake_sense(',
        'static bool lcd_should_wake_sense(',
        'static bool sense_recently_heard(',
        'static bool wake_reason_requires_immediate_pulse(',
        'static void start_sense_wake_handshake(',
        'static unsigned long wake_retry_interval_for_attempt(',
        'static void send_sense_ping(',
        'static void cancel_pending_sleep_for_user_input(',
        'static bool wake_sense_for_request(',
        'static bool lcd_maybe_pulse_sense_int(',
        'static void request_sense_wake(',
    ]
    has_claim = 'static bool lcd_claim_sense_wake_pulse(' in source
    if has_claim:
        index = signatures.index('static bool wake_sense_for_request(')
        signatures[index:index] = ['static bool lcd_claim_sense_wake_pulse(',
                                   'static void lcd_release_sense_wake_pulse(']
    has_episode_mux = 'static bool lcd_claim_sense_wake_expiry(' in source
    if has_episode_mux:
        index = signatures.index('static bool lcd_claim_sense_wake_pulse(')
        signatures[index:index] = ['static void lcd_rearm_sense_wake_for_user(',
                                   'static bool lcd_claim_sense_wake_expiry(']
    has_pong_helpers = 'static void sense_pong_reset(' in source
    if has_pong_helpers:
        signatures[:0] = ['static void sense_pong_reset(', 'static void sense_pong_arm(',
                           'static bool sense_pong_take_expired(']
    functions = '\n'.join(definition(source, signature) for signature in signatures)
    functions += '\n' + definition(anim, 'static void ensure_awake_for_ui(')
    timeout = definition(source, 'if (sense_pong_take_expired(now_ms))' if has_pong_helpers
                         else 'if (sense_pong_pending &&')
    pong = definition(rx, 'if (strcmp(type, "PONG") == 0)')
    ready = definition(rx, 'if (strcmp(type, "SLEEP_READY") == 0)')
    retry_loop = definition(source, 'if (!g_in_light_sleep && !sense_awake_confirmed && wake_retry_until_ms > 0 &&')
    timer_wait = definition(source, 'if (wake_timer_wait_mode && !sense_awake_confirmed)')
    # Preserve real UART message-to-wake routing, stopping before serialization.
    routing = uart[uart.index('  auto input_requires_sense = '):]
    routing = routing[:routing.index('  // Serial-injected voice:')]
    prefix = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
static thread_local unsigned critical_depth=0;
using portMUX_TYPE=std::mutex;
static portMUX_TYPE s_sense_pong_mux;
static portMUX_TYPE s_sense_wake_state_mux;
static void portENTER_CRITICAL(portMUX_TYPE*lock){assert(!critical_depth);lock->lock();++critical_depth;}
static void portEXIT_CRITICAL(portMUX_TYPE*lock){assert(critical_depth==1);--critical_depth;lock->unlock();}
static unsigned long clock_ms=10000;
// Episode helpers use their caller's time sample; no nested reads or I/O.
static unsigned long millis(){assert(!critical_depth);return clock_ms;}
static unsigned pulses=0,short_pulses=0,miss_logs=0,line_releases=0;
static void(*release_hook)()=nullptr;
static std::atomic<bool> s_sense_wake_pulse_busy(false);
static std::atomic<bool> s_sense_wake_retry_exhausted(false);
static std::vector<std::string> sent;
static bool sense_awake_confirmed=false,sense_awake_estimate=false;
enum SenseState {SENSE_UNKNOWN=0,SENSE_AWAKE,SENSE_ASLEEP};
static SenseState sense_state=SENSE_UNKNOWN;
static uint8_t sense_missed_pongs=0,wake_retry_attempts=0;
static bool sense_pong_pending=false,sleep_ready_received=false;
static unsigned long sense_pong_deadline_ms=0,last_sense_ping_ms=0;
static unsigned long last_sense_any_rx_ms=0,last_sense_rx_ms=0;
static unsigned long last_sense_sleep_ready_ms=0,last_proof_of_life_ms=0;
static unsigned long wake_retry_until_ms=0,last_wake_retry_ms=0,last_int_pulse_ms=0;
static unsigned long wake_timer_wait_start_ms=0;
static bool sense_rx_stale_logged=false,wake_timer_wait_mode=false;
static bool g_in_light_sleep=false,g_sleep_transition=false;
static bool sense_wake_explicit_request=false,sense_status_sync_requested=false;
static bool sense_ota_apply_required=false,refresh_request_pending=false;
static bool refresh_request_needs_send=false,waiting_for_list_response=false;
static bool waiting_for_scan_response=false,waiting_for_voice_response=false;
static bool provision_refresh_pending=false,lcd_refresh_inflight=false;
static bool provisioning_active=false,provision_qr_waiting=false;
static bool sense_ota_active=false,g_ota_mode_active=false,g_lcd_maintenance_active=false;
enum RefreshState {REFRESH_IDLE,REFRESH_WAKE_PENDING,REFRESH_INFLIGHT};
static RefreshState refresh_state=REFRESH_IDLE;
static bool link_sync_pending=false,link_synced=false,queued=false;
static bool sense_sleep_intent_pending=false,sleep_deny_active=false;
static bool sleep_wait_for_sense_idle=false,sleep_cancelled_by_user_input=false;
static bool sleep_retry_requires_user=false,sleep_deny_received=false;
static unsigned sleep_handshake_fail_count=0;
static unsigned long sleep_retry_allowed_ms=0,sleep_deny_retry_ms=0;
static char sleep_deny_reason[64]={};
static bool g_lcd_maintenance_headless=false,g_idle_screen_dark=false;
static bool g_panel_enabled=true,g_lvgl_running=true;
static int g_backlight_duty=128;
static void* app_event_queue=nullptr;
static struct {int count=0;}g_active;
struct app_event_t {int type;struct {int new_count;}data;};
enum {EVT_RENDER_ACTIVE_LIST,EVT_RESET_UI};
#define pdMS_TO_TICKS(n) (n)
static void xQueueSend(void*,app_event_t*,int){}
static void lcd_timer_receiver_wait_release(const char*){}
static void lcd_media_user_wake(){}
static void lcd_media_retry_wait_release(const char*){}
static void lcd_allow_visible_ui(const char*){}
static void sleep_fallback_reset(const char*){}
static void lcd_clear_maintenance_state(const char*,bool){}
static void lcd_exit_ota_mode(const char*){}
static void lcd_set_backlight_binary(bool on,const char*){g_backlight_duty=on?128:0;}
static void lcd_panel_set_power(bool on){g_panel_enabled=on;}
static bool lv_is_initialized(){return false;}
static void* lv_scr_act(){return nullptr;}
static void lv_obj_invalidate(void*){}
static struct {template<class... T>void printf(const char*,T...){assert(!critical_depth);}
 void println(const char*){assert(!critical_depth);}}Serial;
static void pulseWakeSense(){assert(!critical_depth);++pulses;}
static void pulseWakeSenseShort(){assert(!critical_depth);++short_pulses;}
static bool lcd_maintenance_active(){return g_lcd_maintenance_active;}
static bool deferred_awake_tx_pending(){return queued;}
static void maybe_extend_sense_awake_grace(const char*){}
static void refresh_sm_proof_of_life(const char*){}
static void refresh_sm_on_awake_proof(const char*){}
static void send_wifi_status(){}
static int ship_user_state_current(){return 0;}
static const char* ship_user_state_name(int){return "test";}
static void lcd_sleep_ts(const char*){}
static void release_wake_line(const char*){
 assert(!critical_depth);
 if(release_hook){auto hook=release_hook;release_hook=nullptr;hook();}
 ++line_releases;
}
static void lcd_errlog_store_with_context(const char*,const char*,const char*,int,const char*){++miss_logs;}
static const char* sense_state_name(SenseState);
static void send_sense_ping();
static void request_sense_wake(const char*);
static void start_sense_wake_handshake();
static void cancel_pending_sleep_for_user_input(const char*);
static void uart_send_input_message(const char*);
'''
    bridge = '\n'.join((
        'static void uart_send_input_message(const char*type){assert(!critical_depth);\n'+routing+'\nsent.emplace_back(type);}',
        'static void receive(const char*type){const auto prev_rx_ms=last_sense_rx_ms;'
        'last_sense_any_rx_ms=millis();\n'+pong+'\n'+ready+'\n}',
        'static void timeout_tick(){const auto now_ms=millis();\n'+timeout+'\n}',
        'static void retry_tick(){\n'+retry_loop+'\n'+timer_wait+'\n}',
    ))
    cases = r'''
static unsigned pings(){unsigned n=0;for(auto&s:sent)if(s=="INPUT_PING")++n;return n;}
static void ready_then_first_miss(){
 receive("SLEEP_READY");
 assert(sleep_ready_received&&sense_state==SENSE_ASLEEP&&!sense_awake_confirmed);
 clock_ms+=100;send_sense_ping();assert(sense_pong_pending);
 clock_ms=sense_pong_deadline_ms-1;timeout_tick();assert(sense_missed_pongs==0);
 clock_ms+=2;timeout_tick();
 assert(!sense_awake_confirmed&&sleep_ready_received);
 assert(sense_missed_pongs==1&&!sense_pong_pending);
}
int main(int argc,char**argv){
 assert(argc==2);std::string test=argv[1];
 if(test.rfind("user:",0)==0){
  ready_then_first_miss();sense_state_set(SENSE_UNKNOWN,"retained_unknown");
  const char*reason=argv[1]+5;
  if(!strcmp(reason,"settings_fw_info")||!strcmp(reason,"manual_ota"))request_sense_wake(reason);
  else uart_send_input_message(reason); // Actual input_requires_sense dispatch.
  assert(pulses==1&&short_pulses==0&&!sleep_ready_received);
  assert(wake_retry_until_ms==clock_ms+WAKE_RETRY_WINDOW_MS);
  assert(sense_pong_pending&&sense_pong_deadline_ms==clock_ms+SENSE_PONG_TIMEOUT_MS);
 }else if(test.rfind("short:",0)==0){
  ready_then_first_miss();sense_state_set(SENSE_UNKNOWN,"retained_unknown");
  sense_wake_explicit_request=true;
  assert(wake_sense_for_request(argv[1]+6));
  assert(short_pulses==1&&pulses==0&&!sleep_ready_received);
 }else if(test=="sleep_ready_timeout"){
  ready_then_first_miss();assert(sense_state==SENSE_ASLEEP&&miss_logs==0);
 }else if(test=="pending_deadline"){
  send_sense_ping();const auto original=sense_pong_deadline_ms;
  assert(original==11500&&pings()==1);
  for(unsigned i=1;i<=3;++i){clock_ms=10000+400*i;send_sense_ping();
   assert(pings()==i+1&&sense_pong_deadline_ms==original);}
  clock_ms=original-1;timeout_tick();assert(sense_pong_pending&&sense_missed_pongs==0);
  ++clock_ms;timeout_tick();assert(!sense_pong_pending&&sense_state==SENSE_UNKNOWN);
  assert(sense_missed_pongs==1); // Repeated sends cannot postpone this transition.
  clock_ms=11600;send_sense_ping();assert(sense_pong_deadline_ms==13100);
  clock_ms=13101;timeout_tick();assert(sense_state==SENSE_ASLEEP&&sense_missed_pongs==2);
 }else if(test=="pong_restarts_deadline"){
  send_sense_ping();clock_ms=10400;receive("PONG");
  assert(sense_awake_confirmed&&sense_state==SENSE_AWAKE&&!sense_pong_pending);
  assert(!sense_pong_deadline_ms&&!sense_missed_pongs&&last_proof_of_life_ms==10400);
  send_sense_ping();assert(pings()==1); // Fresh proof does not bypass awake rate limit.
  queued=true;send_sense_ping();assert(pings()==2&&sense_pong_deadline_ms==11900);
  clock_ms=10800;send_sense_ping();assert(pings()==3&&sense_pong_deadline_ms==11900);
 }else if(test=="ping_rates"){
  send_sense_ping();send_sense_ping();assert(pings()==1);
  clock_ms=10399;send_sense_ping();assert(pings()==1);
  clock_ms=10400;send_sense_ping();assert(pings()==2);
  receive("PONG");clock_ms=15399;send_sense_ping();assert(pings()==2);
  clock_ms=15400;send_sense_ping();assert(pings()==3);
  assert(sense_pong_deadline_ms==16900);
 }else if(test.rfind("background:",0)==0){
  ready_then_first_miss();sense_state_set(SENSE_UNKNOWN,"retained_unknown");
  request_sense_wake(argv[1]+11);
  assert(pulses==0&&short_pulses==0&&sleep_ready_received);
  assert(wake_retry_until_ms==0);
 }else if(test=="awake"){
  receive("PONG");sleep_ready_received=true;
  request_sense_wake("settings_fw_info");
  uart_send_input_message("INPUT_SENSE_FW");uart_send_input_message("INPUT_OTA_CHECK");
  assert(pulses==0&&short_pulses==0&&sleep_ready_received&&pings()==1);
 }else if(test=="ordinary_asleep"){
  receive("SLEEP_READY");clock_ms+=1600;request_sense_wake("status_sync");
  assert(pulses==1&&!sleep_ready_received&&sense_pong_pending);
 }else if(test=="sleep_transition"){
  ready_then_first_miss();g_sleep_transition=true;
  request_sense_wake("settings_fw_info");assert(!pulses&&!short_pulses);
  g_sleep_transition=false;g_in_light_sleep=true;
  request_sense_wake("INPUT_OTA_CHECK");assert(!pulses&&!short_pulses);
 }else if(test=="coalescing_and_horizon"){
  request_sense_wake("settings_fw_info");assert(pulses==1);
  const auto horizon=wake_retry_until_ms;
  uart_send_input_message("INPUT_SENSE_FW");uart_send_input_message("INPUT_OTA_CHECK");
  assert(pulses==1&&wake_retry_until_ms==horizon&&wake_retry_attempts==0);
  clock_ms+=400;request_sense_wake("settings_fw_info");
  assert(pulses==1&&wake_retry_until_ms==horizon);
  clock_ms=horizon+1;request_sense_wake("INPUT_OTA_CHECK");
  assert(pulses==1&&wake_retry_until_ms<=horizon);
  wake_retry_until_ms=0;wake_timer_wait_mode=true;
  request_sense_wake("INPUT_SENSE_FW");assert(pulses==1);
  ensure_awake_for_ui("touch_press");request_sense_wake("settings_fw_info");
  assert(pulses==2&&wake_retry_until_ms==clock_ms+WAKE_RETRY_WINDOW_MS);
 }else if(test=="atomic_claim"){
#if HAVE_CLAIM
  assert(lcd_claim_sense_wake_pulse(clock_ms));
  assert(!lcd_claim_sense_wake_pulse(clock_ms));
  lcd_release_sense_wake_pulse();
  assert(!s_sense_wake_pulse_busy.load());
#else
  assert(false&&"original source has no coalescing claim");
#endif
 }else if(test=="retry_cadence"){
  request_sense_wake("settings_fw_info");assert(pulses==1);
  const auto horizon=wake_retry_until_ms;
  for(unsigned delay:{1200U,2200U,3200U}){
   const unsigned before=pulses;
   clock_ms=last_wake_retry_ms+delay-1;
   ensure_awake_for_ui("touch_press");request_sense_wake("settings_fw_info");
   assert(pulses==before&&wake_retry_until_ms==horizon);
   ++clock_ms;request_sense_wake("settings_fw_info");
   assert(pulses==before+1&&wake_retry_until_ms==horizon);
   assert(wake_retry_attempts==pulses-1);
  }
  assert(wake_retry_interval_for_attempt(wake_retry_attempts)==4500);
  clock_ms=horizon;request_sense_wake("INPUT_OTA_CHECK");assert(pulses==4);
  assert(!s_sense_wake_pulse_busy.load());
 }else if(test=="wrap_deadline"){
  clock_ms=UINT32_MAX-1499U;send_sense_ping();
  assert(static_cast<uint32_t>(sense_pong_deadline_ms)==0&&sense_pong_pending);
  clock_ms=UINT32_MAX;timeout_tick();assert(sense_pong_pending);
  clock_ms=0;timeout_tick();assert(!sense_pong_pending&&sense_missed_pongs==1);
 }else if(test=="no_peer_45s"){
  request_sense_wake("settings_fw_info");assert(pulses==1);
  const auto start=clock_ms;clock_ms=wake_retry_until_ms+1;retry_tick();
  assert(wake_retry_until_ms==0&&wake_timer_wait_mode);
  assert(s_sense_wake_retry_exhausted.load());
  clock_ms=start+45000;retry_tick();
  assert(!wake_timer_wait_mode&&wake_retry_until_ms==0);
  for(unsigned i=0;i<5;++i){
   uart_send_input_message("INPUT_SENSE_FW");clock_ms+=400;retry_tick();
   assert(pulses==1&&wake_retry_until_ms==0&&s_sense_wake_retry_exhausted.load());
  }
  ensure_awake_for_ui("touch_press");request_sense_wake("settings_fw_info");
  assert(pulses==2&&!s_sense_wake_retry_exhausted.load());
  assert(wake_retry_until_ms==clock_ms+WAKE_RETRY_WINDOW_MS);
 }else if(test=="manual_new_request"){
  request_sense_wake("manual_ota");assert(pulses==1);
  const auto original=wake_retry_until_ms;
  clock_ms+=400;request_sense_wake("manual_ota");
  assert(pulses==1&&wake_retry_until_ms==original);
  clock_ms=original+1;retry_tick();
  assert(s_sense_wake_retry_exhausted.load()&&wake_timer_wait_mode);
  uart_send_input_message("INPUT_OTA_CHECK");assert(pulses==1);
  request_sense_wake("manual_ota");
  assert(pulses==2&&!s_sense_wake_retry_exhausted.load()&&!wake_timer_wait_mode);
  assert(wake_retry_until_ms==clock_ms+WAKE_RETRY_WINDOW_MS);
 }else if(test=="new_claim_precedes_old_expiry"){
#if HAVE_EPISODE_MUX
  request_sense_wake("settings_fw_info");assert(pulses==1);
  clock_ms=wake_retry_until_ms+1;const auto stale_expiry_now=clock_ms;
  ensure_awake_for_ui("touch_press");
  assert(lcd_claim_sense_wake_pulse(clock_ms));
  const auto new_horizon=wake_retry_until_ms;
  assert(new_horizon==clock_ms+WAKE_RETRY_WINDOW_MS);
  assert(!lcd_claim_sense_wake_expiry(stale_expiry_now));
  assert(s_sense_wake_pulse_busy.load());
  assert(wake_retry_until_ms==new_horizon&&!wake_timer_wait_mode&&!line_releases);
  pulseWakeSense();lcd_release_sense_wake_pulse();
  assert(!lcd_claim_sense_wake_expiry(stale_expiry_now));
  assert(wake_retry_until_ms==new_horizon&&!s_sense_wake_retry_exhausted.load());
#else
  assert(false&&"original source has no serialized episode expiry");
#endif
 }else if(test=="expiry_custody_precedes_new_claim"){
  request_sense_wake("settings_fw_info");assert(pulses==1);
  clock_ms=wake_retry_until_ms+1;
  release_hook=+[]{
   // The actual old expiry branch has committed and is about to release GPIO.
   // Interleave a UI action at that hardware boundary, before custody release.
   assert(s_sense_wake_pulse_busy.load());
   ensure_awake_for_ui("touch_press");request_sense_wake("settings_fw_info");
   assert(pulses==1&&wake_retry_until_ms==0);
  };
  retry_tick();assert(!release_hook&&line_releases==1);
  assert(!s_sense_wake_pulse_busy.load()&&!s_sense_wake_retry_exhausted.load());
  request_sense_wake("settings_fw_info");
  assert(pulses==2&&wake_retry_until_ms==clock_ms+WAKE_RETRY_WINDOW_MS);
 }else{assert(false);}
 assert(!critical_depth);
 puts(("PASS actual-source "+test).c_str());
}
'''
    return '\n'.join(('#define HAVE_CLAIM '+str(int(has_claim)),
                      '#define HAVE_EPISODE_MUX '+str(int(has_episode_mux)),
                      prefix, constants, functions, bridge, cases))


class UserWake(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        chosen = Path(ARGS.source).resolve() if ARGS.source else ROOT
        main = chosen if chosen.is_file() else chosen / 'LCD_Minimal/LCD_Minimal.ino'
        siblings = main.parent
        cls.directory = tempfile.TemporaryDirectory(prefix='halo-lcd-user-wake-')
        cls.addClassCleanup(cls.directory.cleanup)
        cpp = Path(cls.directory.name) / 'test.cpp'
        cls.binary = Path(cls.directory.name) / 'test'
        cpp.write_text(harness(main.read_text(), (siblings/'lcd_uart_rx.h').read_text(),
                               (siblings/'lcd_uart.h').read_text(),
                               (siblings/'lcd_anim.h').read_text()))
        subprocess.run([shutil.which('c++'), '-std=c++17', str(cpp), '-o', str(cls.binary)],
                       check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_explicit_user_actions_after_sleep_ready_and_unknown(self):
        for reason in ('settings_fw_info', 'INPUT_SENSE_FW', 'INPUT_OTA_CHECK', 'manual_ota', 'INPUT_USER_ACTIVE'):
            with self.subTest(reason=reason):
                self.run_case('user:' + reason)
                self.run_case('short:' + reason)

    def test_repeated_400ms_pings_keep_original_deadline(self):
        self.run_case('pending_deadline')

    def test_timeout_preserves_confirmed_sleep_ready(self):
        self.run_case('sleep_ready_timeout')

    def test_fresh_pong_clears_pending_and_allows_new_deadline(self):
        self.run_case('pong_restarts_deadline')

    def test_initial_and_repeated_ping_rate_limits(self):
        self.run_case('ping_rates')

    def test_background_requests_cannot_override_unknown_sleep_ready(self):
        for reason in ('status_sync', 'wake_retry', 'uart_tx', 'unknown_background'):
            with self.subTest(reason=reason):
                self.run_case('background:' + reason)

    def test_confirmed_awake_has_no_gpio_pulse(self):
        self.run_case('awake')

    def test_ordinary_asleep_wake_and_sleep_transition_guards(self):
        self.run_case('ordinary_asleep')
        self.run_case('sleep_transition')

    def test_requests_coalesce_and_only_new_gesture_rearms_expired_episode(self):
        self.run_case('coalescing_and_horizon')

    def test_atomic_claim_excludes_overlapping_callers(self):
        self.run_case('atomic_claim')

    def test_retry_cadence_and_original_window_boundary(self):
        self.run_case('retry_cadence')

    def test_pending_pong_deadline_can_wrap_to_zero(self):
        self.run_case('wrap_deadline')

    def test_no_peer_after_45s_background_traffic_cannot_restart(self):
        self.run_case('no_peer_45s')

    def test_fresh_manual_action_rearms_but_deferred_ota_does_not(self):
        self.run_case('manual_new_request')

    def test_old_expiry_cannot_cancel_new_claimed_episode(self):
        self.run_case('new_claim_precedes_old_expiry')

    def test_expiry_line_release_excludes_interleaved_new_user_pulse(self):
        self.run_case('expiry_custody_precedes_new_claim')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', help='Firmware source directory or LCD_Minimal.ino')
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__] + remaining)

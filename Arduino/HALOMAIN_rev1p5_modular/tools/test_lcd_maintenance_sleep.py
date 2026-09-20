#!/usr/bin/env python3
"""Execute actual LCD maintenance-arm/sleep code with deterministic UART/input races.

Only hardware, rendering and time are stubbed. --source-root can select a sealed
older snapshot to reproduce the false cancellation without editing firmware.
"""
import argparse
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ('ordinary_arm', 'handshake_future_race', 'handshake_future_hold',
         'touch', 'scroll', 'early_provisioning', 'early_ota',
         'early_recent_ready', 'early_asleep', 'early_link',
         'early_wake_line', 'denied', 'timeout', 'abort_ota_early',
         'abort_ota_late', 'abort_bench', 'abort_ota_touch_early', 'abort_ota_touch_late',
         'ordinary_dark_abort', 'ordinary_lit_abort', 'ordinary_dark_touch_abort')


def definition(text, anchor):
    start = text.index(anchor)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def harness(root):
    main = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    sleep = (root / 'LCD_Minimal/lcd_sleep.h').read_text()
    activity = (root / 'LCD_Minimal/lcd_activity.h').read_text()
    animation = (root / 'LCD_Minimal/lcd_anim.h').read_text()
    uart = (root / 'LCD_Minimal/lcd_uart_rx.h').read_text()
    arm_start = uart.index('    // ALWAYS arm wake timer')
    arm_end = uart.index('    // Persist to NVS BEFORE headless entry', arm_start)
    arm = 'static void receive_future_arm(uint32_t remaining_s, uint32_t wake_in_s){\n' + uart[arm_start:arm_end] + '\n}\n'
    scope_anchor = 'struct LcdSleepHandshakeScope {'
    scope = definition(sleep, scope_anchor) + ';\n' if scope_anchor in sleep else ''
    media = ('static bool s_sleep_media_deferred=false;\n' + definition(sleep, 'static bool sleep_defer_for_media() {')) if 'static bool sleep_defer_for_media() {' in sleep else ''
    provision = ('#include "' + str(root / 'LCD_Minimal/lcd_provision_flow.h') + '"\nstatic LcdProvisionFlow provision_flow;\nstatic bool s_sleep_provision_deferred=false;\n' + definition(sleep, 'static bool sleep_defer_for_provisioning() {')) if 'static bool sleep_defer_for_provisioning() {' in sleep else ''
    actual = '\n'.join((
        definition(activity, 'static void resetActivityTimer() {'),
        definition(main, 'static bool sleep_blocked_for_ota() {'),
        definition(main, 'static void cancel_pending_sleep_for_user_input(const char* reason) {'),
        definition(animation, 'static void abort_sleep_transition(const char* reason, bool user_input = true) {'),
        arm, scope, media, provision,
        definition(sleep, 'static bool notify_sense_sleep() {'),
    ))
    # Execute the actual teardown call arguments, including a coincident real
    # touch; do not model all OTA aborts as implicit user input.
    abort_calls = {name: next(line.strip() for line in sleep.splitlines()
                             if 'abort_sleep_transition("'+reason+'"' in line)
                   for name,reason in (('early','ota_before_teardown'),
                                       ('late','ota_or_cancel_during_teardown'),
                                       ('bench','bench_timer_arm_failed'))}
    actual += '\nstatic void teardown_abort(const char* stage){\n'
    for name,call in abort_calls.items():
        actual += 'if(!strcmp(stage,"'+name+'")){'+call+'return;}\n'
    actual += '}\n'
    return r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#undef assert
#define assert(condition) do { if (!(condition)) { \
  fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition);std::exit(1); \
} } while (false)
#define pdMS_TO_TICKS(value) (value)
static unsigned long now_ms=10000;
static unsigned long millis(){return now_ms;}
static struct {template<class... A>void printf(const char*,A...){} void println(const char*){}} Serial;
struct LcdCoordCriticalGuard {};
enum {SCREEN_HOME,SCREEN_SHOPPING_LIST,SCREEN_SETTINGS};
static int ui_screen_state=SCREEN_HOME;
static unsigned long last_user_activity_ms=9000,home_shown_ms=9000,last_scroll_activity_ms=9000;
static unsigned long last_sleep_skip_log_ms;
static char last_sleep_skip_reason[32];
static std::atomic<bool> g_lcd_sleep_handshake_active{false},g_lcd_validation_pending{false},g_lcd_ota_uart_receiving{false},g_lcd_coord_notice_clear{false};
static unsigned long ota_stay_awake_until_ms,g_ota_lock_window_until_ms,g_ota_continuation_hold_start_ms,g_ota_check_block_since_ms;
static constexpr unsigned long OTA_CONTINUATION_HOLD_MS=300000,OTA_CHECK_BLOCK_MAX_MS=60000;
static bool ota_locked,ota_check_requested,ota_check_pending,sense_ota_active,sense_ota_apply_required,g_lcd_maintenance_active,g_ota_mode_active,g_ota_screen_active,provision_return_home_pending;
static unsigned long g_lcd_maintenance_deadline_ms;
static uint64_t g_lcd_maintenance_start_epoch;
static uint8_t g_lcd_maintenance_timer_armed;
static uint32_t g_lcd_maintenance_wake_in_s,g_lcd_maintenance_remaining_s;
static bool lcd_timer_receiver_wait_active(){return false;}
static bool lcd_media_retry_wait_active(){return false;}
static bool lcd_time_valid(){return false;}
static bool lcd_maintenance_window_is_current(uint64_t){return false;}
static bool lcd_maintenance_active(){return g_lcd_maintenance_active;}
enum {SENSE_ASLEEP,SENSE_AWAKE,SENSE_UNKNOWN};
static int sense_state=SENSE_AWAKE;
static const char* sense_state_name(int){return "stub";}
static bool sense_awake_estimate=true,link_synced=true,link_sync_pending,provisioning_active;
static unsigned long last_sense_rx_ms=9500,last_sense_sleep_ready_ms,sense_awake_grace_until_ms;
static constexpr unsigned long SENSE_RX_STALE_MS=8000,SENSE_SLEEP_READY_GRACE_MS=15000,SENSE_RECENT_RX_FOR_SLEEP_MS=10000,SENSE_UNKNOWN_STALE_EXTENDED_MS=30000;
static bool sleep_ready_received,sleep_deny_received,sleep_deny_active,sleep_handshake_fail_link,sleep_wait_for_sense_idle,sleep_retry_requires_user,sleep_cancelled_by_user_input;
static bool sense_sleep_intent_pending=true,g_sleep_transition,g_in_light_sleep;
static bool g_img_rx_active=false,g_img_rx_binary_mode=false,g_spool_tx_pending=false,g_spool_tx_active=false;
static unsigned long sleep_deny_retry_ms,sleep_deny_received_ms,sleep_retry_allowed_ms;
static unsigned sleep_handshake_fail_count,sleep_fallback_timer_sec;
static constexpr uint32_t SLEEP_DENY_RETRY_DEFAULT_MS=5000,SLEEP_FALLBACK_TIMER_SEC=15,SLEEP_HANDSHAKE_RETRY_DELAY_MS=800;
static constexpr uint8_t SLEEP_HANDSHAKE_MAX_ATTEMPTS=3;
static char sleep_deny_reason[64];
static bool touch_pressed,touch_wake_only_pending,long_press_sent,ship_ai_touch_active;
static unsigned long touch_ignore_until,scroll_ignore_until,touch_press_time;
static uint16_t touch_press_x,touch_press_y;
static bool g_background_wake_dark=false,g_idle_screen_dark=true,g_panel_enabled=false,g_lvgl_running=true;
static int g_backlight_duty=0;
static unsigned relights,visible_calls,sleep_messages,pings,delay_calls;
static bool wake_line_ok=true,injected,guard_seen;
static std::string scenario;
static int ship_user_state_current(){return 0;}
static const char* ship_user_state_name(int){return "stub";}
static void lcd_send_diag_pre_sleep(){}
static void lcd_sleep_ts(const char*){}
static void refresh_sense_awake_estimate(unsigned long){}
static void send_sense_ping(){++pings;}
static bool sleep_prepare_wake_line_for_request(){return wake_line_ok;}
static void send_input_sleep_message(){++sleep_messages;}
static void sleep_enter_wait_low_power(const char*){}
static void lcd_timer_receiver_wait_release(const char*){}
static unsigned media_user_notices=0;
static void lcd_media_user_wake(){++media_user_notices;}
static bool host_sleep_touch_irq=false;
static bool lcd_sleep_touch_fired(){return host_sleep_touch_irq;}
static void sleep_fallback_reset(const char*){}
static void lcd_allow_visible_ui(const char*){++visible_calls;g_background_wake_dark=false;}
static void lcd_set_backlight_binary(bool on,const char*){g_backlight_duty=on?128:0;if(on)++relights;}
static void lcd_panel_set_power(bool){}
static void* app_event_queue=nullptr;
enum {EVT_RENDER_ACTIVE_LIST,EVT_RESET_UI};
struct app_event_t {int type;struct {int new_count;} data;};
static struct {int count;} g_active;
static void xQueueSend(void*,const app_event_t*,int){}
static int getTouch(uint16_t*,uint16_t*);
static void vTaskDelay(unsigned long);
''' + actual + r'''
static int getTouch(uint16_t*,uint16_t*){
  if(!injected && (scenario=="handshake_future_race"||scenario=="touch"||scenario=="scroll")){
    injected=true;guard_seen=g_lcd_sleep_handshake_active.load();now_ms+=10;
    receive_future_arm(3600,3500);
    if(scenario=="touch")return 1;
    if(scenario=="scroll")last_scroll_activity_ms=millis();
  }
  return 0;
}
static void vTaskDelay(unsigned long delay){
  now_ms+=delay;++delay_calls;
  if(scenario=="handshake_future_hold" && !injected){
    injected=true;guard_seen=g_lcd_sleep_handshake_active.load();receive_future_arm(3600,3500);
  }else if(scenario=="denied"){
    sleep_deny_received=true;strcpy(sleep_deny_reason,"op_inflight");
  }else if(scenario!="timeout")sleep_ready_received=true;
}
static void assert_future_arm_preserved(){
  assert(g_lcd_maintenance_timer_armed==1);
  assert(g_lcd_maintenance_wake_in_s==3500 && g_lcd_maintenance_remaining_s==3600);
  assert(g_lcd_maintenance_deadline_ms>now_ms);
}
int main(int argc,char** argv){
  assert(argc==2);scenario=argv[1];
  if(scenario.find("ordinary_")==0 && scenario!="ordinary_arm"){
    const bool touch=scenario=="ordinary_dark_touch_abort";
    const bool lit=scenario=="ordinary_lit_abort";
    g_background_wake_dark=false;g_sleep_transition=true;g_idle_screen_dark=!lit;
    g_panel_enabled=lit;g_lvgl_running=lit;g_backlight_duty=lit?128:0;
    host_sleep_touch_irq=touch;teardown_abort("early");
    assert(media_user_notices==(touch?1u:0u));
    assert(visible_calls==(touch?1u:0u));
    assert(relights==(touch?1u:0u));
    assert(g_backlight_duty==((lit||touch)?128:0));
    assert(g_panel_enabled==(lit||touch) && g_idle_screen_dark==!(lit||touch));
    assert(!g_sleep_transition);
    printf("OBS %s user_input=%d relights=%u backlight=%d idle_dark=%d\n",
           scenario.c_str(),touch,relights,g_backlight_duty,g_idle_screen_dark);
  }else if(scenario.find("abort_")==0){
    const bool touch=scenario.find("touch")!=std::string::npos;
    host_sleep_touch_irq=touch;g_background_wake_dark=true;g_sleep_transition=true;
    const char* stage=scenario.find("bench")!=std::string::npos?"bench":
                      (scenario.find("early")!=std::string::npos?"early":"late");
    teardown_abort(stage);
    assert(media_user_notices==(touch?1u:0u));
    assert(visible_calls==(touch?1u:0u) && relights==(touch?1u:0u));
    assert(g_backlight_duty==(touch?128:0) && g_background_wake_dark==!touch);
    assert(!g_sleep_transition);
  }else if(scenario=="ordinary_arm"){
    receive_future_arm(3600,3500);assert_future_arm_preserved();
    assert(last_user_activity_ms==now_ms && home_shown_ms==now_ms);
    assert(ota_stay_awake_until_ms==now_ms+8000 && sleep_blocked_for_ota());
    now_ms+=100;ota_stay_awake_until_ms=now_ms+20000;receive_future_arm(3600,3500);
    assert(ota_stay_awake_until_ms==now_ms+20000);
    ui_screen_state=SCREEN_SHOPPING_LIST;now_ms+=100;receive_future_arm(3600,3500);
    assert(home_shown_ms==now_ms && last_user_activity_ms==now_ms);
    assert(!g_lcd_sleep_handshake_active.load());
  }else{
    if(scenario=="early_provisioning")provisioning_active=true;
    if(scenario=="early_ota")g_lcd_validation_pending=true;
    if(scenario=="early_recent_ready")last_sense_sleep_ready_ms=now_ms-10;
    if(scenario=="early_asleep")sense_state=SENSE_ASLEEP;
    if(scenario=="early_link"){sense_state=SENSE_UNKNOWN;sense_awake_estimate=false;last_sense_rx_ms=0;}
    if(scenario=="early_wake_line")wake_line_ok=false;
    const bool result=notify_sense_sleep();
    assert(!g_lcd_sleep_handshake_active.load());
    if(scenario=="handshake_future_race"||scenario=="handshake_future_hold"){
      printf("OBS %s slept=%d relights=%u cancelled=%d activity=%lu hold=%lu\n",
             scenario.c_str(),result,relights,sleep_cancelled_by_user_input,
             last_user_activity_ms,ota_stay_awake_until_ms);
      assert(result);assert(guard_seen);assert_future_arm_preserved();
      assert(last_user_activity_ms==9000 && home_shown_ms==9000 && last_scroll_activity_ms==9000);
      assert(!sleep_cancelled_by_user_input && !ota_stay_awake_until_ms && !sleep_blocked_for_ota());
      assert(g_backlight_duty==0 && !g_panel_enabled && g_idle_screen_dark && relights==0 && visible_calls==0);
      assert(media_user_notices==0);
      assert(sleep_messages==1);
    }else if(scenario=="touch"||scenario=="scroll"){
      assert(!result && guard_seen && sleep_cancelled_by_user_input && !sense_sleep_intent_pending);
      assert_future_arm_preserved();assert(visible_calls==1 && relights==1 && g_backlight_duty==128);
      assert(!ota_stay_awake_until_ms);
      assert(media_user_notices==1);
    }else if(scenario=="early_recent_ready"||scenario=="early_asleep"){
      assert(result && sleep_messages==0);
    }else if(scenario=="denied"){
      assert(!result && sleep_deny_active && sleep_wait_for_sense_idle && sleep_messages==1);
    }else if(scenario=="timeout"){
      assert(result && sleep_messages==SLEEP_HANDSHAKE_MAX_ATTEMPTS && sleep_fallback_timer_sec==SLEEP_FALLBACK_TIMER_SEC);
    }else{
      assert(!result && sleep_messages==0);
    }
    // Every exit releases the scope: later ordinary arming still refreshes idle.
    now_ms+=100;receive_future_arm(3600,3500);
    assert(last_user_activity_ms==now_ms && ota_stay_awake_until_ms==now_ms+8000);
  }
  printf("PASS %s\n",scenario.c_str());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='halo-maintenance-sleep-') as directory:
        path = Path(directory); source = path / 'check.cpp'; binary = path / 'check'
        source.write_text(harness(args.source_root))
        subprocess.run([shutil.which('c++'), '-std=c++17', str(source), '-o', str(binary)], check=True, timeout=30)
        failures = 0
        for case in args.case or CASES:
            result = subprocess.run([str(binary), case], timeout=5)
            failures += result.returncode != 0
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Native regression for the real shopping idle clock and finite OTA hold.

Compile production definitions and bounded caller slices with fake clock/UI
boundaries. This tests state transitions, not rendering or cross-core timing.
--source-root can point at an older firmware tree to reproduce the regression.
"""
import argparse
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def declaration(text, marker):
    start = text.index(marker)
    return text[start:text.index(';', start) + 1]


def harness(root):
    main = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    activity = (root / 'LCD_Minimal/lcd_activity.h').read_text()
    screens = (root / 'LCD_Minimal/lcd_ship_screens.h').read_text()
    ui = (root / 'LCD_Minimal/lcd_ui_task.h').read_text()
    entry = block(screens, 'static void show_shopping_list_screen_impl()')
    entry = entry[entry.index('  ui_screen_state = SCREEN_SHOPPING_LIST;'):
                  entry.index('  ui_busy = false;')]
    ready = ui[ui.index('// If on shopping list screen, re-populate with new data'):]
    ready = block(ready, 'if (ui_screen_state == SCREEN_SHOPPING_LIST)')
    scroll = ui[ui.index('if (evt.type == EVT_SCROLL_DELTA)'):]
    scroll = scroll[scroll.index('        user_activity_bump("scroll");'):
                    scroll.index('        bool scroll_wake_context')]
    clock = main[main.index('    bool on_menu_screen = ui_is_sleep_eligible_menu_screen(ui_screen_state);'):]
    clock = clock[:clock.index('    bool dish_processing')]
    decision = main[main.index('    bool eligible = home_age_ms >= HOME_SLEEP_DELAY_MS;'):]
    decision = decision[:decision.index('    const char* decision_reason')]
    wake = main[main.index('// Unified 20s hard watchdog:'):]
    wake = block(wake, 'if (ui_screen_state == SCREEN_SHOPPING_LIST &&')
    inflight = main[main.index('// Hard timeout on the shopping list:'):]
    inflight = block(inflight, 'if (ui_screen_state == SCREEN_SHOPPING_LIST &&')
    return r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <initializer_list>
// Preserve active assertions without invoking the macOS crash reporter.
#undef assert
#define assert(condition) do { if (!(condition)) { \
  std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); \
  std::exit(1); } } while (false)
enum ui_screen_t {SCREEN_HOME,SCREEN_SECOND,SCREEN_SETTINGS,SCREEN_SHOPPING_LIST,SCREEN_OTHER};
enum RefreshState {REFRESH_IDLE,REFRESH_WAKE_PENDING,REFRESH_INFLIGHT,REFRESH_COMPLETE,REFRESH_FAILED};
static unsigned long now_ms;
static unsigned long millis(){return now_ms;}
static ui_screen_t ui_screen_state;
static RefreshState refresh_state;
static unsigned long home_shown_ms,last_user_activity_ms,last_touch_or_input_ms;
static unsigned long last_sleep_skip_log_ms,sleep_retry_allowed_ms;
static char last_sleep_skip_reason[8];
static bool user_activity_since_sleep,sleep_retry_requires_user,sleep_wait_for_sense_idle;
static unsigned sleep_handshake_fail_count,list_closed,timeout_events,populates,ticks,latch_resets;
static bool waiting_for_list_response,lcd_refresh_inflight,refresh_request_pending;
static bool refresh_request_needs_send,refresh_input_wake_sent,refresh_grace_extended;
static bool lcd_refresh_ack_seen,refresh_retry_pending,refresh_requested_again,refresh_wake_sent;
static unsigned long refresh_request_retry_count,lcd_refresh_retry_count,lcd_refresh_sent_ms;
static unsigned long refresh_wake_pending_attempts,refresh_wake_pending_last_ping_ms;
static unsigned long refresh_wake_pending_next_pulse_ms,refresh_wake_pending_start_ms;
static unsigned long refresh_last_ui_pol_ms,refresh_last_proof_ms,refresh_pulse_count;
static unsigned long refresh_last_pulse_ms,refresh_last_wake_send_ms,refresh_done_ms;
static unsigned long lcd_refresh_start_ms,refresh_timeout_count;
static bool provision_return_home_pending,g_ota_screen_active,ota_locked;
static bool g_lcd_maintenance_active,g_lcd_maintenance_aborted,g_ota_mode_active;
static bool sense_ota_active,sense_ota_apply_required,ota_check_requested,ota_check_pending;
static bool g_lcd_ota_uart_receiving,timer_wait,time_valid,current_window;
static std::atomic<bool> g_lcd_validation_pending{false},g_lcd_coord_notice_clear{false};
static unsigned long ota_stay_awake_until_ms,g_ota_lock_window_until_ms;
static unsigned long g_ota_continuation_hold_start_ms,g_lcd_maintenance_deadline_ms;
static unsigned long g_lcd_maintenance_start_epoch,g_ota_check_block_since_ms;
static constexpr uint32_t OTA_CONTINUATION_HOLD_MS=180000,OTA_CHECK_BLOCK_MAX_MS=20000;
static constexpr int SENSE_ASLEEP=0,SENSE_AWAKE=1;
static int sense_state;
struct LcdCoordCriticalGuard {};
static bool lcd_timer_receiver_wait_active(){return timer_wait;}
static bool lcd_time_valid(){return time_valid;}
static bool lcd_maintenance_window_is_current(uint64_t){return current_window;}
static bool lcd_maintenance_active(){return g_lcd_maintenance_active;}
static struct {template<class... A>void printf(const char*,A...){}void println(const char*){}} Serial;
static void uart_send_list_active(bool value){assert(!value);++list_closed;}
static void example_lvgl_unlock(){}
struct app_event_t {int type;int data[1];};
static int queue_token;
static void* app_event_queue=&queue_token;
static constexpr int EVT_REFRESH_TIMEOUT=1;
static unsigned pdMS_TO_TICKS(unsigned n){return n;}
static void xQueueSend(void*,app_event_t* e,unsigned){assert(e->type==EVT_REFRESH_TIMEOUT);++timeout_events;}
static int g_active;
static uint32_t actual_signature,shopping_list_rendered_sig;
static bool shopping_list_reveal_pending;
static uint32_t shopping_list_content_sig(int*){return actual_signature;}
static void shopping_list_reset_pull_latch(){++latch_resets;}
static void shopping_list_refresh_indicator_sync(bool){}
static void shopping_list_screen_populate(){++populates;}
static void ui_lvgl_tick(){++ticks;}
''' + '\n'.join([
        declaration(activity, 'static const unsigned long HOME_SLEEP_DELAY_MS'),
        declaration(main, 'static const unsigned long REFRESH_HARD_TIMEOUT_MS'),
        block(activity, 'static bool ui_is_sleep_eligible_menu_screen(ui_screen_t state)'),
        block(activity, 'static void resetActivityTimer()'),
        block(main, 'static void user_activity_bump_quiet('),
        block(main, 'static void user_activity_bump(const char* reason) {'),
        block(main, 'static void refresh_hard_timeout_clear('),
        block(main, 'static bool sleep_blocked_for_ota() {'),
        'static void actual_entry(){' + entry + '}',
        'static void actual_list_ready(){' + ready + '}',
        'static void actual_scroll_activity(){' + scroll + '}',
        'static void actual_idle_tick(){unsigned long now_ms=millis();' + clock +
        decision + '\nloop_continue:;}',
        'static void actual_hard_timeout_tick(){unsigned long now=millis();'
        'if(refresh_state==REFRESH_WAKE_PENDING){' + wake +
        '}else if(refresh_state==REFRESH_INFLIGHT){unsigned long total_ms=now-lcd_refresh_start_ms;'
        + inflight + '}}',
    ]) + r'''
static void reset(){
  now_ms=100;ui_screen_state=SCREEN_HOME;home_shown_ms=100;
  last_user_activity_ms=last_touch_or_input_ms=100;
  last_sleep_skip_log_ms=0;last_sleep_skip_reason[0]=0;
  sleep_retry_requires_user=sleep_wait_for_sense_idle=user_activity_since_sleep=false;
  sleep_handshake_fail_count=sleep_retry_allowed_ms=0;
  refresh_state=REFRESH_IDLE;
  waiting_for_list_response=lcd_refresh_inflight=refresh_request_pending=false;
  refresh_request_needs_send=refresh_input_wake_sent=refresh_grace_extended=false;
  lcd_refresh_ack_seen=refresh_retry_pending=refresh_requested_again=refresh_wake_sent=false;
  refresh_wake_pending_start_ms=lcd_refresh_start_ms=refresh_timeout_count=0;
  provision_return_home_pending=g_ota_screen_active=ota_locked=false;
  g_lcd_maintenance_active=g_lcd_maintenance_aborted=g_ota_mode_active=false;
  sense_ota_active=sense_ota_apply_required=ota_check_requested=ota_check_pending=false;
  g_lcd_ota_uart_receiving=timer_wait=time_valid=current_window=false;
  g_lcd_validation_pending=false;g_lcd_coord_notice_clear=false;
  ota_stay_awake_until_ms=g_ota_lock_window_until_ms=g_ota_continuation_hold_start_ms=0;
  g_lcd_maintenance_deadline_ms=g_lcd_maintenance_start_epoch=g_ota_check_block_since_ms=0;
  sense_state=SENSE_ASLEEP;
  list_closed=timeout_events=populates=ticks=latch_resets=0;
  actual_signature=shopping_list_rendered_sig=1;shopping_list_reveal_pending=false;
}
static void idle_until(unsigned long deadline){
  now_ms=deadline-1;actual_idle_tick();assert(!provision_return_home_pending);
  now_ms=deadline;actual_idle_tick();assert(provision_return_home_pending&&list_closed==1);
}
int main(){
  assert(HOME_SLEEP_DELAY_MS==10000);
  // Opening a list after an old Home clock must never close it immediately.
  reset();now_ms=60000;actual_entry();
  assert(home_shown_ms==60000&&last_user_activity_ms==60000);
  actual_idle_tick();assert(!provision_return_home_pending);
  idle_until(70000);
  puts("PASS late shopping entry receives a fresh full 10-second viewing interval");

  // Each real pending/inflight signal blocks expiry independently.
  for(int signal=0;signal<5;++signal){
    reset();actual_entry();now_ms=15000;
    if(signal==0)waiting_for_list_response=true;
    if(signal==1)lcd_refresh_inflight=true;
    if(signal==2)refresh_request_pending=true;
    if(signal==3)refresh_state=REFRESH_WAKE_PENDING;
    if(signal==4)refresh_state=REFRESH_INFLIGHT;
    actual_idle_tick();assert(!provision_return_home_pending&&list_closed==0);
  }
  // Actual002 timing: response t=7846 followed by former expiry t=10829.
  reset();now_ms=829;actual_entry();now_ms=7846;actual_list_ready();
  now_ms=10829;actual_idle_tick();assert(!provision_return_home_pending);
  idle_until(17846);
  puts("PASS slow/pending refresh and observed002 response-to-Home regression");

  // Both identical cached content and changed content restart the same clock.
  for(bool changed:{false,true}){
    reset();now_ms=50000;actual_entry();refresh_state=REFRESH_INFLIGHT;
    waiting_for_list_response=lcd_refresh_inflight=refresh_request_pending=true;
    now_ms=65000;actual_idle_tick();assert(!provision_return_home_pending);
    // Model the UART UI_LIST completion boundary; execute the actual UI branch.
    waiting_for_list_response=lcd_refresh_inflight=refresh_request_pending=false;
    refresh_state=REFRESH_COMPLETE;actual_signature=changed?2:1;
    actual_list_ready();assert(home_shown_ms==65000&&latch_resets==1&&ticks==1);
    assert(populates==(changed?1u:0u));
    idle_until(75000);
  }
  reset();now_ms=60000;actual_entry();now_ms=69000;actual_scroll_activity();
  assert(home_shown_ms==69000&&last_touch_or_input_ms==69000&&user_activity_since_sleep);
  idle_until(79000);
  // Ordinary reset behavior outside Home/list stays unchanged.
  reset();ui_screen_state=SCREEN_OTHER;now_ms=999;resetActivityTimer();
  assert(home_shown_ms==100&&last_user_activity_ms==999);
  puts("PASS ready-to-full-10s for unchanged/changed data, scroll reset and other screens");

  // Existing hard failure path ends the new wait; no auto-retry or perpetual hold.
  for(RefreshState state:{REFRESH_WAKE_PENDING,REFRESH_INFLIGHT}){
    reset();now_ms=1000;actual_entry();refresh_state=state;
    refresh_wake_pending_start_ms=lcd_refresh_start_ms=1000;
    waiting_for_list_response=lcd_refresh_inflight=refresh_request_pending=true;
    refresh_request_needs_send=refresh_input_wake_sent=refresh_retry_pending=true;
    now_ms=1000+REFRESH_HARD_TIMEOUT_MS;actual_hard_timeout_tick();
    assert(refresh_state==state&&timeout_events==0);
    actual_idle_tick();assert(!provision_return_home_pending);
    ++now_ms;actual_hard_timeout_tick();
    assert(refresh_state==REFRESH_IDLE&&timeout_events==1&&refresh_timeout_count==1);
    assert(!waiting_for_list_response&&!lcd_refresh_inflight&&!refresh_request_pending);
    assert(!refresh_request_needs_send&&!refresh_input_wake_sent&&!refresh_retry_pending);
    actual_idle_tick();assert(provision_return_home_pending&&list_closed==1);
  }
  puts("PASS bounded existing hard-timeout cleanup releases failed refresh and permits real idle");

  for(bool visible:{false,true}){
    reset();ui_screen_state=SCREEN_SHOPPING_LIST;now_ms=10000;
    ota_locked=true;g_ota_screen_active=visible;
    ota_stay_awake_until_ms=g_ota_lock_window_until_ms=11000;
    now_ms=10999;assert(sleep_blocked_for_ota());
    assert(ota_locked&&g_ota_screen_active==visible&&!provision_return_home_pending);
    now_ms=11000;assert(!sleep_blocked_for_ota());
    assert(!ota_locked&&!g_ota_screen_active&&g_lcd_coord_notice_clear.load());
    assert(provision_return_home_pending==visible);
    assert(ui_screen_state==SCREEN_SHOPPING_LIST);
  }
  // Hardware validation, binary receive and rendezvous remain absolute guards.
  for(int guard=0;guard<3;++guard){
    reset();ota_locked=true;ota_stay_awake_until_ms=50;g_ota_screen_active=true;
    if(guard==0)g_lcd_validation_pending=true;
    if(guard==1)g_lcd_ota_uart_receiving=true;
    if(guard==2)timer_wait=true;
    assert(sleep_blocked_for_ota()&&ota_locked&&g_ota_screen_active);
    assert(!provision_return_home_pending&&!g_lcd_coord_notice_clear.load());
  }
  puts("PASS silent versus visible finite lease expiry; active ownership protections unchanged");
}
'''


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path,
                        default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('C++ compiler unavailable')
    with tempfile.TemporaryDirectory(prefix='halo-shopping-idle-') as directory:
        cpp = Path(directory) / 'test.cpp'
        executable = Path(directory) / 'test'
        cpp.write_text(harness(args.source_root))
        subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra',
                        '-Wno-unused-variable', str(cpp), '-o', str(executable)],
                       check=True, timeout=30, preexec_fn=no_core)
        result = subprocess.run([str(executable)], timeout=5, preexec_fn=no_core)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()

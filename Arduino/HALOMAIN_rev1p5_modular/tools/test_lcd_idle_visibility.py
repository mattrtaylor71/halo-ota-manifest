#!/usr/bin/env python3
"""Execute real LCD visibility paths after a user session has gone dark.

Hardware/time/queues are observations only. Production activity, maintenance,
list-completion, Home, touch and OTA visibility bodies are extracted verbatim.
--source-root accepts an older snapshot to reproduce the non-user relight.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_lcd_maintenance_sleep import definition, maintenance_awake_source

ROOT = Path(__file__).resolve().parents[1]
CASES = ('idle', 'keepalive', 'future_arm', 'list_home', 'list_screen',
         'timer_keepalive', 'denied_keepalive', 'transition_keepalive',
         'handshake_future_arm', 'handshake_keepalive', 'touch', 'timer_touch',
         'ota', 'timer_ota', 'background_screen_change')


def harness(root):
    main = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    activity = (root / 'LCD_Minimal/lcd_activity.h').read_text()
    anim = (root / 'LCD_Minimal/lcd_anim.h').read_text()
    uart = (root / 'LCD_Minimal/lcd_uart_rx.h').read_text()
    ui = (root / 'LCD_Minimal/lcd_ui_task.h').read_text()
    guardian = ('static void lcd_guardian_begin_foreground(' in activity)
    actual = (definition(activity, 'static void lcd_guardian_begin_foreground(')
              + '\n') if guardian else ''
    actual += '\n'.join((
        maintenance_awake_source(main),
        definition(activity, 'static void resetActivityTimer() {'),
        definition(main, 'static void lcd_allow_visible_ui('),
        definition(anim, 'static void lcd_set_backlight_level('),
        definition(anim, 'static void lcd_set_backlight_binary('),
        definition(anim, 'static void lcd_set_idle_screen_dark('),
        definition(anim, 'static void ensure_awake_for_ui('),
    ))
    actual += '\nstatic void keepalive(){\n' + definition(
        uart, 'if (strcmp(type, "MAINT_KEEPALIVE") == 0) {') + '\n}\n'
    start = uart.index('    // ALWAYS arm wake timer')
    end = uart.index('    // Persist to NVS BEFORE headless entry', start)
    actual += ('static void future_arm(uint32_t remaining_s,uint32_t wake_in_s){\n'
               + uart[start:end] + '\n}\n')
    start = ui.index('            unsigned long time_since_wake = millis() - last_wake_time;')
    end = ui.index('            Serial.printf("[UI] Swapped pending', start)
    actual += 'static void list_complete(){\n' + ui[start:end] + '\n}\n'
    start = main.index('    const char* decision_reason = eligible ? "eligible" : "home_age_lt_timeout";')
    end = main.index('    if (ota_locked)', start)
    actual += ('static void evaluate_home(){\nconst auto now_ms=millis();'
               'const char* screen_name="HOME";const auto home_age_ms=now_ms-home_shown_ms;\n'
               + next(line for line in main.splitlines()
                      if 'bool eligible = home_age_ms >= HOME_SLEEP_DELAY_MS;' in line)
               + '\n' + main[start:end] + '\nloop_continue: ;\n}\n')
    start = ui.index('    if (g_ota_screen_active) {')
    end = ui.index('      // Dark designer system surface.', start)
    actual += 'static void ota_presentation(){\n' + ui[start:end] + '\n}\n}\n'
    return r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#define assert(condition) do { if (!(condition)) { \
 fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition);std::exit(1); \
} } while(false)
#define pdMS_TO_TICKS(n) (n)
static unsigned long now_ms=20000;
static unsigned long millis(){return now_ms;}
static void delay(unsigned long ms){now_ms+=ms;}
static struct {template<class... A>void printf(const char*,A...){} void println(const char*){}} Serial;
enum {SCREEN_HOME,SCREEN_SHOPPING_LIST};
static int ui_screen_state=SCREEN_HOME;
static unsigned long last_user_activity_ms=1,home_shown_ms=1,last_sleep_skip_log_ms=0;
static unsigned long last_wake_time=1;
static bool was_just_woke=false;
static char last_sleep_skip_reason[32]={};
static constexpr unsigned long HOME_SLEEP_DELAY_MS=10000;
static bool g_background_wake_dark=false,g_idle_screen_dark=true,g_sleep_transition=false,g_in_light_sleep=false;
static bool g_ota_mode_active=false,g_ota_screen_active=false,g_lvgl_running=false,g_panel_enabled=false;
static bool g_backlight_initialized=true,sleep_deny_active=false,g_lcd_initialized=true,g_lcd_maintenance_headless=false;
static int g_backlight_duty=0,g_user_brightness_duty=128,pwm=0,power_on_count=0;
static unsigned long ota_stay_awake_until_ms=0,g_lcd_maintenance_deadline_ms=0;
static uint8_t g_lcd_maintenance_timer_armed=0;
static uint32_t g_lcd_maintenance_wake_in_s=0,g_lcd_maintenance_remaining_s=0;
static std::atomic<bool> g_lcd_sleep_handshake_active{false};
static std::atomic<uint32_t> guardian_awake_start_ms{0};
static std::atomic<bool> guardian_sleep_triggered{false};
struct LcdCoordCriticalGuard {};
static bool lv_is_initialized(){return true;}
static void lcd_lvgl_wait_tx_done(int){}
static void lcd_panel_set_power(bool on){if(on)++power_on_count;}
static void lcd_bl_pwm_bsp_init(int){}
static void setUpdutySubdivide(int v){pwm=v;}
static const char* lcd_backlight_state_label(){return "host";}
static void log_sleep_decision(unsigned long,const char*,unsigned long,bool,const char*){}
static const char* type="MAINT_KEEPALIVE";
struct Value{const char*operator|(const char* fallback)const{return fallback;}};
struct Doc{Value operator[](const char*)const{return {};}};
static Doc doc;
static unsigned user_notices=0,paint_invalidations=0;
static void lcd_media_user_wake(){++user_notices;}
static void lcd_timer_receiver_wait_release(const char*){}
static void lcd_rearm_sense_wake_for_user(unsigned long){}
static void sleep_fallback_reset(const char*){}
static void cancel_pending_sleep_for_user_input(const char*){}
static void lcd_clear_maintenance_state(const char*,bool){assert(false);}
static void lcd_exit_ota_mode(const char*){assert(false);}
static int screen;
static void*lv_scr_act(){return &screen;}
static void lv_obj_invalidate(void*){++paint_invalidations;}
enum{EVT_RENDER_ACTIVE_LIST,EVT_RESET_UI};
struct app_event_t{int type;struct{int new_count;}data;};
static void*app_event_queue=nullptr;
static struct{int count;}g_active;
static void xQueueSend(void*,const app_event_t*,int){}
''' + actual + r'''
int main(int argc,char**argv){
 assert(argc==2);std::string test=argv[1];
 if(test.find("timer_")==0)g_background_wake_dark=true;
 if(test=="denied_keepalive")sleep_deny_active=true;
 if(test=="transition_keepalive")g_sleep_transition=true;
 if(test.find("handshake_")==0)g_lcd_sleep_handshake_active=true;
 if(test.find("keepalive")!=std::string::npos)keepalive();
 if(test=="future_arm"||test=="handshake_future_arm"){
  future_arm(3600,3500);
  assert(g_lcd_maintenance_timer_armed&&g_lcd_maintenance_wake_in_s==3500);
 }
 if(test=="list_home"||test=="list_screen"){
  if(test=="list_screen")ui_screen_state=SCREEN_SHOPPING_LIST;
  list_complete();assert(home_shown_ms==now_ms&&last_user_activity_ms==now_ms);
 }
 if(test=="background_screen_change")lcd_set_idle_screen_dark(false,"screen_not_home");
 if(test=="touch"||test=="timer_touch"){
  ensure_awake_for_ui("touch_press");
  assert(user_notices==1&&paint_invalidations==1&&!g_background_wake_dark);
 }else if(test=="ota"||test=="timer_ota"){
  g_ota_screen_active=true;ota_presentation();assert(user_notices==0);
 }else evaluate_home();
 const bool visible=test=="touch"||test=="timer_touch"||test=="ota"||test=="timer_ota";
 printf("OBS %s visible=%d backlight=%d panel=%d idle_dark=%d background_dark=%d\n",
        test.c_str(),visible,g_backlight_duty,g_panel_enabled,g_idle_screen_dark,g_background_wake_dark);
 assert(g_backlight_duty==(visible?128:0)&&pwm==(visible?128:0));
 assert(g_panel_enabled==visible&&g_lvgl_running==visible&&g_idle_screen_dark==!visible);
 assert(power_on_count==(visible?1:0));
 if(!visible)assert(user_notices==0);
 printf("PASS %s\n",test.c_str());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--case', action='append', choices=CASES)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-idle-visibility-') as directory:
        path = Path(directory)
        source, binary = path / 'test.cpp', path / 'test'
        source.write_text(harness(args.source_root))
        subprocess.run([shutil.which('c++'), '-std=c++17', str(source), '-o', str(binary)],
                       check=True, timeout=30)
        failures = 0
        for case in args.case or CASES:
            failures += subprocess.run([str(binary), case], timeout=5).returncode != 0
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())

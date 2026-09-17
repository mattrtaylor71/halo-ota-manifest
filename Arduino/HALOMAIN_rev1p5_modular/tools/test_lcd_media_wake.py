#!/usr/bin/env python3
"""Execute actual media boot timer, UI initialization and user-wake functions.

GPIO/PWM/panel, LVGL, RTOS and UART queue calls are host observations only.
This is not physical proof of a flash-free panel or measured touch latency.
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def harness():
    spec = importlib.util.spec_from_file_location('maintenance', ROOT / 'tools/test_lcd_maintenance_sleep.py')
    base = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(base)
    define = base.definition
    ino = (ROOT / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    anim = (ROOT / 'LCD_Minimal/lcd_anim.h').read_text()
    activity = (ROOT / 'LCD_Minimal/lcd_activity.h').read_text()
    sleep = (ROOT / 'LCD_Minimal/lcd_sleep.h').read_text()
    ui = (ROOT / 'LCD_Minimal/lcd_ui_task.h').read_text()
    assert '(effective_timer_wake && !g_lcd_media_retry_boot && maintenance_context && maintenance_resume_hint)' in ino
    assert '(effective_timer_wake && !g_lcd_media_retry_boot && g_lcd_schedule_timer_armed)' in ino
    assert '  lcd_media_retry_wait_release(reason);' in define(ino, 'static void lcd_timer_receiver_wait_release(')
    assert 'abort_sleep_transition("media_timer_arm_failed", false);' in sleep
    assert 'ensure_awake_for_ui("touch_press");' in ino and 'touch_ignore_until = now + 300;' in ino
    assert 'ensure_awake_for_ui("scroll_evt");' in ui
    bsp = (ROOT / 'LCD_Minimal/lcd_bsp.c').read_text()
    assert '//ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel_handle, true));' in bsp
    assert 'DISPLAY_ON here would flash the panel before a timer boot can stay dark.' in bsp
    text = base.harness(ROOT).split('int main(int argc,char** argv){', 1)[0]
    text = text.replace('static void lcd_timer_receiver_wait_release(const char*){}', r'''
#define RTC_DATA_ATTR
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
static unsigned media_lock_depth=0;
static void portENTER_CRITICAL(int*){assert(!media_lock_depth);++media_lock_depth;}
static void portEXIT_CRITICAL(int*){--media_lock_depth;}
static int64_t esp_timer_get_time(){return int64_t(now_ms)*1000;}
#include "LCD_Minimal/lcd_media_retry.h"
#include "LCD_Minimal/lcd_media_foreground.h"
static void lcd_timer_receiver_wait_release(const char* r){lcd_media_retry_wait_release(r);}
static bool g_backlight_initialized=false;
static int g_user_brightness_duty=128;
static unsigned pwm_positive=0,pwm_inits=0,panel_on=0,menus=0,wake_notices=0;
static void lcd_bl_pwm_bsp_init(int duty){assert(duty==0);++pwm_inits;}
static void setUpdutySubdivide(int duty){if(duty>0)++pwm_positive;}
static const char* lcd_backlight_state_label(){return "test";}
struct tx_msg_t{char type[24];};
static bool uart_tx_enqueue(const tx_msg_t* msg,const char*){assert(!strcmp(msg->type,"INPUT_USER_ACTIVE"));++wake_notices;return true;}
''')
    text = text.replace('static bool lcd_media_retry_wait_active(){return false;}', 'static bool lcd_media_retry_wait_active();')
    # The maintenance fixture counts user notices; replace that entire boundary
    # definition so this suite always executes the real cancellation/queue code.
    wake_stub = define(text, 'static void lcd_media_user_wake(')
    text = text.replace(wake_stub, define(anim, 'static void lcd_media_user_wake('), 1)
    text = text.replace('static void lcd_allow_visible_ui(const char*){++visible_calls;g_background_wake_dark=false;}', define(ino, 'static void lcd_allow_visible_ui('))
    text = text.replace('static void lcd_set_backlight_binary(bool on,const char*){g_backlight_duty=on?128:0;if(on)++relights;}', '\n'.join(define(anim, s) for s in (
        'static void backlight_apply_pct(', 'static void lcd_set_backlight_level(', 'static void lcd_set_backlight_binary(')))
    text = text.replace('static void lcd_panel_set_power(bool){}', 'static void lcd_panel_set_power(bool on){if(on)++panel_on;}')
    text += r'''
static bool g_ui_initialized=false,g_lcd_initialized=false,g_lcd_maintenance_headless=false;
static void* ui_task_handle=nullptr;
static void init_touch_once(){}
static void haptic_init(){}
static void lcd_lvgl_Init(){assert(pwm_inits==1&&pwm_positive==0);}
using lv_disp_t=int;
static int display;
static lv_disp_t* lv_disp_get_default(){return &display;}
static constexpr int LV_DISP_ROT_180=2;
static void lv_disp_set_rotation(lv_disp_t*,int){}
static void create_custom_ui(){}
static void show_ship_main_menu(){++menus;}
static void status_overlay_init(){}
static void init_knob_once(){}
static void ui_task(void*){}
static void xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,int,void**,int){}
static void delay(unsigned n){now_ms+=n;}
static void lcd_rearm_sense_wake_for_user(unsigned long){}
static void lcd_clear_maintenance_state(const char*,bool){assert(false);}
static void lcd_exit_ota_mode(const char*){assert(false);}
static bool lv_is_initialized(){return true;}
static void* lv_scr_act(){return &display;}
static void lv_obj_invalidate(void*){}
#define SHIP_MENU_UI 1
'''
    text += define(activity, 'static void init_ui_stack(') + '\n'
    text += define(anim, 'static void ensure_awake_for_ui(') + '\n'
    return text + r'''
int main(int argc,char**argv){
  assert(argc==2);now_ms=1000;
  // Saved brightness is restored before wake classification, while PWM remains
  // uninitialized. That real helper must make no positive hardware write.
  backlight_apply_pct(80);assert(!pwm_positive&&!pwm_inits);
  assert(lcd_media_retry_arm("0123456789abcdef",300));
  bool selected=false;auto seconds=lcd_media_retry_choose_timer(3500,&selected);
  assert(selected&&seconds==295);lcd_media_retry_commit_sleep(seconds,selected,true);
  now_ms=10;lcd_media_retry_note_boot(true,true);assert(g_lcd_media_retry_boot);
  bool effective_timer_wake=true,maintenance_context=true,maintenance_resume_hint=true;
  g_lcd_maintenance_timer_armed=1;g_lcd_maintenance_wake_in_s=3500;g_lcd_maintenance_remaining_s=3600;
  bool g_lcd_schedule_timer_armed=true;
  // The exact production expressions are asserted above; neither future arm
  // may be consumed or treated as the origin of this actual media timer boot.
  bool maintenance_wake=(effective_timer_wake&&!g_lcd_media_retry_boot&&maintenance_context&&maintenance_resume_hint);
  bool schedule_wake=(effective_timer_wake&&!g_lcd_media_retry_boot&&g_lcd_schedule_timer_armed);
  assert(!maintenance_wake&&!schedule_wake);
  g_background_wake_dark=effective_timer_wake;init_ui_stack(0);
  assert(g_ui_initialized&&menus==1&&pwm_inits==1&&!pwm_positive&&!panel_on);
  assert(g_background_wake_dark&&g_idle_screen_dark&&!g_panel_enabled&&!g_lvgl_running&&lcd_media_retry_wait_active());
  // Even an incidental generic brightness-on request cannot flash a headless boot.
  lcd_set_backlight_binary(true,"background");assert(!pwm_positive&&!panel_on);
  if(!strcmp(argv[1],"timer_failure")){
    g_sleep_transition=g_in_light_sleep=true;
    abort_sleep_transition("media_timer_arm_failed",false);
    assert(g_background_wake_dark&&!pwm_positive&&!panel_on&&!wake_notices&&!g_lcd_media_user_session);
    puts("PASS media SDK timer failure abort stays dark and grants no user ownership");return 0;
  }
  if(!strcmp(argv[1],"ordinary_user")){
    g_lcd_media_retry_boot=false;
    ensure_awake_for_ui("touch_press");ensure_awake_for_ui("scroll_evt");
    assert(wake_notices==1&&g_lcd_media_user_session&&!lcd_media_try_claim(true,false));
    puts("PASS ordinary user entry publishes one pause notice without an LCD media timer or transfer");return 0;
  }
  assert(lcd_media_try_claim(true,false));
  ensure_awake_for_ui(!strcmp(argv[1],"touch")?"touch_press":"scroll_evt");
  assert(!g_background_wake_dark&&!g_idle_screen_dark&&g_panel_enabled&&g_lvgl_running);
  assert(pwm_positive==1&&panel_on==1&&!lcd_media_retry_wait_active());
  assert(lcd_media_replay_cancelled()&&g_lcd_media_user_session&&wake_notices==1);
  lcd_media_release();now_ms+=20000;
  assert(!lcd_media_try_claim(true,false));
  assert(lcd_media_try_claim(false,false));lcd_media_release();
  ensure_awake_for_ui("touch_press");assert(wake_notices==1&&pwm_positive==1&&panel_on==1);
  assert(g_lcd_maintenance_timer_armed&&g_lcd_maintenance_wake_in_s==3500&&g_lcd_maintenance_remaining_s==3600&&g_lcd_schedule_timer_armed);
  printf("PASS media %s wake: dark boot, immediate visible user wake, one notice, session replay pause, retained OTA arms\n",argv[1]);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='halo-media-wake-') as directory:
        source = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        source.write_text(harness())
        subprocess.run([shutil.which('c++'), '-std=c++17', '-I', str(ROOT), str(source), '-o', str(binary)], check=True)
        for case in ('touch', 'encoder', 'timer_failure', 'ordinary_user'):
            subprocess.run([str(binary), case], check=True, timeout=10)


if __name__ == '__main__':
    main()

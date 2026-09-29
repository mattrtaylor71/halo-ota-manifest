#!/usr/bin/env python3
"""Execute production maintenance delivery, idle visibility and sleep admission.

Clock, panel I/O, RTOS and peer transport are controlled host boundaries. The
lease, UART branches, idle decision, sleep funnel/handshake and guardian are
actual source. --source-root retains an unchanged old-source negative control.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

import test_lcd_idle_visibility as visibility
import test_lcd_sleep_media_custody as custody
from test_lcd_maintenance_sleep import definition

ROOT = Path(__file__).resolve().parents[1]
VISUAL = ('repeated_keepalive', 'future_arm', 'timer_dark', 'touch', 'ota',
          'expiry', 'wrap', 'zero_deadline', 'expired_no_resurrection', 'commit_refusal')
SLEEP = ('idle_hold', 'forced_hold', 'ready_hold', 'late_hold', 'guard_hold', 'commit_hold',
         'intent_hold', 'guardian_hold', 'headless_hold', 'expired_sleep')


def visual_harness(root):
    text = visibility.harness(root).split('\nint main(', 1)[0]
    main = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    # Include the real OTA keep-lit checks and actual darkening call: testing
    # only the home-age branch would miss the second half of the observed bug.
    start = main.index('    const char* decision_reason = eligible ? "eligible" : "home_age_lt_timeout";')
    end = main.index('    unsigned long wifi_age_ms', start)
    old = definition(text, 'static void evaluate_home(){')
    prefix = old[:old.index('    const char* decision_reason')]
    text = text.replace(old, prefix + main[start:end] + '\nloop_continue: ;\n}')
    text = text.replace('static void evaluate_home(){',
        'static bool ota_locked=false,g_lcd_ota_uart_receiving=false;\n'
        'static bool lcd_ota_uart_active(){return false;}\nstatic void evaluate_home(){')
    return text + r'''
int main(int argc,char**argv){
 assert(argc==2);const std::string name=argv[1];
 now_ms=1000;last_user_activity_ms=home_shown_ms=1000;
 g_idle_screen_dark=false;g_panel_enabled=g_lvgl_running=true;g_backlight_duty=pwm=128;
 if(name=="wrap"||name=="zero_deadline"||name=="expired_no_resurrection"){
  now_ms=name=="zero_deadline"?uint32_t(0U-8000U):0xfffffff0U;
  const auto began=uint32_t(now_ms);keepalive();assert(lcd_maintenance_awake_active());
  now_ms=uint32_t(began+7999U);assert(lcd_maintenance_awake_active());
  now_ms=uint32_t(began+8001U);assert(!lcd_maintenance_awake_active());
  if(name=="expired_no_resurrection"){
   now_ms=uint32_t(began-1U);assert(!lcd_maintenance_awake_active());
  }
 }else if(name=="commit_refusal"){
  g_sleep_transition=true;keepalive();assert(!lcd_maintenance_awake_active());
  assert(last_user_activity_ms==1000&&home_shown_ms==1000&&!ota_stay_awake_until_ms);
 }else if(name=="expiry"){
  keepalive();now_ms=8999;assert(lcd_maintenance_awake_active());
  now_ms=9000;assert(!lcd_maintenance_awake_active());
 }else{
  if(name=="timer_dark"){
   g_background_wake_dark=g_idle_screen_dark=true;g_panel_enabled=g_lvgl_running=false;
   g_backlight_duty=pwm=0;
  }
  for(now_ms=3000;now_ms<=25000;now_ms+=2000){
   if(name=="future_arm")future_arm(3600,3500);else keepalive();
   assert(last_user_activity_ms==1000&&home_shown_ms==1000);
   assert(!ota_stay_awake_until_ms&&lcd_maintenance_awake_active());
   evaluate_home();
   const bool visible=name!="timer_dark"&&now_ms<11000;
   assert(g_panel_enabled==visible&&g_lvgl_running==visible);
   assert(g_backlight_duty==(visible?128:0));
  }
  if(name=="future_arm")assert(g_lcd_maintenance_timer_armed&&g_lcd_maintenance_wake_in_s==3500);
  if(name=="touch"){
   ensure_awake_for_ui("touch_press");resetActivityTimer();
   assert(g_panel_enabled&&g_lvgl_running&&g_backlight_duty==128&&!g_background_wake_dark);
   assert(last_user_activity_ms==now_ms&&home_shown_ms==now_ms&&user_notices==1);
   assert(lcd_maintenance_awake_active()); // input need not discard schedule delivery
  }else if(name=="ota"){
   g_ota_screen_active=true;ota_presentation();
   now_ms=50000;evaluate_home();assert(g_panel_enabled&&g_backlight_duty==128);
   g_ota_screen_active=false;ota_stay_awake_until_ms=now_ms+20000;
   evaluate_home();assert(g_panel_enabled&&g_backlight_duty==128);
  }
 }
 printf("PASS %s\n",name.c_str());
}
'''


def sleep_harness(root):
    text = custody.harness(root).split('\nint main(', 1)[0]
    main = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    activity = (root / 'LCD_Minimal/lcd_activity.h').read_text()
    uart = (root / 'LCD_Minimal/lcd_uart_rx.h').read_text()
    text = text.replace('static void lcd_sleep_touch_watch_begin(){}', 'static unsigned touch_watch_started=0;static void lcd_sleep_touch_watch_begin(){++touch_watch_started;}')
    text += '\nstatic const char*type="MAINT_KEEPALIVE";\n'
    text += 'struct Value{const char*operator|(const char*s)const{return s;}};struct Doc{Value operator[](const char*)const{return {};}};static Doc doc;\n'
    text += 'static void keepalive(){' + definition(uart, 'if (strcmp(type, "MAINT_KEEPALIVE") == 0) {') + '}\n'
    text = text.replace('static void vTaskDelay(unsigned long delay){', 'static void keepalive();\nstatic void vTaskDelay(unsigned long delay){')
    old = definition(text, 'static void vTaskDelay(unsigned long delay){')
    text = text.replace(old, '''static void vTaskDelay(unsigned long delay){
      now_ms+=delay;++delay_calls;
      if(!injected&&scenario=="late_hold"){injected=true;keepalive();}
    }''')
    text = text.replace(' LcdMaintenanceStorageGuard(){',
        ' LcdMaintenanceStorageGuard(){maintenance_guard_hook();')
    text = text.replace('struct LcdMaintenanceStorageGuard {', 'static void maintenance_guard_hook();\nstruct LcdMaintenanceStorageGuard {')
    text = text.replace('struct LcdCoordCriticalGuard {};',
        'static void maintenance_commit_hook();\nstruct LcdCoordCriticalGuard {LcdCoordCriticalGuard(){maintenance_commit_hook();}};')
    text += '''static void maintenance_guard_hook(){if(scenario=="guard_hold")keepalive();}
static void maintenance_commit_hook(){
 if(!injected&&scenario=="commit_hold"){injected=true;keepalive();}
}
'''
    intent = definition(activity, 'static bool lcd_sleep_intent_allowed(')
    intent = intent[:intent.index('  if (provisioning_input_locked())')] + 'return true;\n}\n'
    text += '''static bool g_test_mode_active=false,wake_timer_wait_mode=false;
static unsigned long g_test_mode_expire_ms=0;
static void test_mode_sync_remaining(){}static void test_mode_clear(const char*){}
''' + intent
    text += 'static bool lcd_ota_uart_active(){return false;}\n'
    text += next(line for line in activity.splitlines() if ' GUARDIAN_FORCE_SLEEP_MS =' in line) + '\n'
    text += definition(activity, 'static bool lcd_sleep_retry_ready(') + '\n'
    text += 'static void guardian_tick(){const auto now_ms=millis();' + definition(main, '  if (!g_in_light_sleep && GUARDIAN_FORCE_SLEEP_MS > 0) {') + '\nloop_continue: ;}\n'
    for name in ('enter_ship_ota_sleep', 'enter_maintenance_sleep'):
        body = definition(activity, 'static void ' + name + '() {')
        prefix = body[:body.index('  if (g_lcd_initialized)')]
        text += prefix + '++teardown;}\n'
    return text + r'''
int main(int argc,char**argv){
 assert(argc==2);scenario=argv[1];now_ms=300150;sense_state=SENSE_ASLEEP;
 last_user_activity_ms=home_shown_ms=9000;const auto fail=sleep_handshake_fail_count;
 if(scenario=="commit_hold"){
  user_activity_since_sleep=true;g_panel_enabled=g_lvgl_running=true;g_idle_screen_dark=false;g_backlight_duty=128;
 }
 if(scenario=="forced_hold")sleep_deny_count=SLEEP_DENY_MAX_COUNT;
 const auto denies=sleep_deny_count;
 if(scenario=="ready_hold")last_sense_sleep_ready_ms=now_ms-1;
 if(scenario=="late_hold"){sense_state=SENSE_AWAKE;last_sense_rx_ms=now_ms;}
 if(scenario!="late_hold"&&scenario!="guard_hold"&&scenario!="commit_hold")keepalive();
 if(scenario=="expired_sleep")now_ms+=8000;
 if(scenario=="intent_hold"){
  const char*reason=nullptr;assert(!lcd_sleep_intent_allowed(&reason));
  assert(reason&&std::string(reason)=="maintenance_delivery");
 }else if(scenario=="guardian_hold")guardian_tick();
 else if(scenario=="headless_hold"){
  enter_ship_ota_sleep();enter_maintenance_sleep();assert(!g_sleep_transition);
  now_ms+=8000;enter_maintenance_sleep();assert(g_sleep_transition&&teardown==1);
 }else enterLightSleep();
 const bool expired=scenario=="expired_sleep"||scenario=="headless_hold";
 assert(teardown==(expired?1:0));
 assert(last_user_activity_ms==9000&&home_shown_ms==9000);
 if(!expired)assert(sleep_handshake_fail_count==fail&&sleep_deny_count==denies);
 if(scenario=="late_hold")assert(injected&&sleep_messages==1&&!g_lcd_sleep_handshake_active);
 if(scenario=="commit_hold"){
  assert(injected&&!g_sleep_transition&&!g_in_light_sleep&&!g_lcd_sleep_commit_gate);
  assert(user_activity_since_sleep&&sense_state==SENSE_ASLEEP&&!touch_watch_started);
  assert(g_panel_enabled&&g_lvgl_running&&!g_idle_screen_dark&&g_backlight_duty==128);
  assert(!relights&&!visible_calls&&!media_user_notices);
 }
 printf("PASS %s\n",scenario.c_str());
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--case', choices=VISUAL + SLEEP, action='append')
    a = p.parse_args(); failures = 0
    with tempfile.TemporaryDirectory(prefix='halo-background-awake-') as directory:
        for name, cases, make in (('visual', VISUAL, visual_harness), ('sleep', SLEEP, sleep_harness)):
            selected = [x for x in a.case or cases if x in cases]
            if not selected: continue
            cpp = Path(directory) / (name + '.cpp'); exe = cpp.with_suffix('')
            cpp.write_text(make(a.source_root))
            subprocess.run([shutil.which('c++'), '-std=c++17', str(cpp), '-o', str(exe)], check=True, timeout=30)
            for case in selected:
                failures += subprocess.run([str(exe), case], timeout=5).returncode != 0
    return int(bool(failures))


if __name__ == '__main__':
    raise SystemExit(main())

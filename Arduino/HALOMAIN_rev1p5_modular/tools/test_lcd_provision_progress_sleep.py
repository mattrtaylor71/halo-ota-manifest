#!/usr/bin/env python3
"""Actual LCD setup flow/service and sleep funnel under prolonged claim silence.

Also executes the guardian, physical-input wake, cancellation, completion service
and UART status admission code. Rendering, UART delivery and RTOS waits are
controlled boundaries; no panel/hardware claim is made. Frozen175 and205 source
controls execute their unchanged runtime functions.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = ('observed_claim_gap', 'connecting_gap', 'deadline_before', 'deadline_at',
         'nonrenewing_claim', 'nonrenewing_app', 'clock_zero', 'wrap',
         'deadline_zero', 'terminal_connected', 'terminal_failed', 'terminal_error',
         'cancel_idle', 'cancel_close', 'retry_clears', 'qr_timeout',
         'stale_qr', 'inactive', 'late_progress', 'timeout_service',
         'timeout_no_service', 'timeout_recovery', 'preview_no_lease',
         'complete_heartbeat_guardian', 'complete_stalled_before',
         'complete_stalled_at', 'complete_nonrenewing', 'complete_wrap',
         'complete_deadline_zero', 'complete_clock_zero',
         'complete_peer_lost',
         'touch_guardian', 'scroll_guardian', 'touch_no_pending',
         'abort_user_guardian', 'abort_background_guardian',
         'background_guardian', 'guardian_sample_race', 'guardian_wrap')
LEGACY_CASES = CASES[:23]
COMPLETION_NEGATIVE_CASES = ('complete_heartbeat_guardian', 'complete_stalled_before',
    'complete_stalled_at', 'complete_nonrenewing', 'touch_guardian', 'scroll_guardian',
    'touch_no_pending', 'abort_user_guardian', 'abort_background_guardian', 'background_guardian')
COMPLETION_NEGATIVE_FAILURES = {'complete_heartbeat_guardian', 'complete_stalled_before',
    'complete_nonrenewing', 'touch_guardian', 'scroll_guardian', 'touch_no_pending',
    'abort_user_guardian'}
NEGATIVE_FAILURES = {'observed_claim_gap', 'connecting_gap', 'deadline_before',
                     'nonrenewing_claim', 'nonrenewing_app', 'clock_zero', 'wrap',
                     'deadline_zero', 'late_progress', 'terminal_connected', 'timeout_recovery'}
FILES = ('LCD_Minimal/lcd_provision_flow.h', 'LCD_Minimal/lcd_provision.h',
         'LCD_Minimal/lcd_sleep.h', 'LCD_Minimal/LCD_Minimal.ino',
         'LCD_Minimal/lcd_activity.h', 'LCD_Minimal/lcd_anim.h', 'LCD_Minimal/lcd_uart_rx.h')


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def harness(root):
    base = load('test_lcd_provision_sleep')
    definition = load('test_lcd_maintenance_sleep').definition
    text = base.harness(root).split('\nint main(', 1)[0]
    ui = (root / FILES[1]).read_text()
    main, activity, animation, uart = ((root / f).read_text() for f in FILES[3:])
    # Model the MCU's uint32 clock rather than the host's 64-bit unsigned long.
    text = text.replace('static unsigned long now_ms=10000;', 'static uint32_t now_ms=10000;')
    text = text.replace('static void sleep_enter_wait_low_power(const char*){}',
        'static void sleep_enter_wait_low_power(const char*){g_backlight_duty=0;g_panel_enabled=g_lvgl_running=false;g_idle_screen_dark=true;}')
    render_publish = next(line.strip() for line in ui.splitlines()
                          if 'if (!provision_ui_preview_active) provisioning_active = true;' in line)
    text += r'''
static bool provision_ui_preview_active=false,provision_ui_deferred=false;
static bool provision_ui_qr_failed=false,provision_ui_can_scroll=false;
static bool provision_user_requested=true,provision_intro_tapped=true,provision_qr_cached=true;
static bool provision_screen_visible=true;
static uint32_t provision_ui_preview_at_ms=0;
static unsigned renders=0,hidden=0;
static void* provision_screen=nullptr;
static void *provision_qr,*provision_status_label,*provision_title_label;
static void *provision_ssid_label,*provision_url_label,*provision_ui_back_btn,*provision_ui_retry_btn;
static constexpr int LV_OBJ_FLAG_HIDDEN=1;
static void lv_obj_add_flag(void*,int){}
static void lv_obj_clean(void*){}
static void provision_ui_success_stop(){}
static void provision_qr_wait_clear(const char*){}
static void hide_provision_intro_screen(const char*){}
static void provision_ui_render(){++renders;''' + render_publish + r'''}
static void provision_ui_preview(int){provision_ui_preview_active=false;}
static bool g_lcd_maintenance_headless=false;
static bool lcd_ota_uart_active(){return false;}
static void lcd_rearm_sense_wake_for_user(uint32_t){}
static void lcd_clear_maintenance_state(const char*,bool){}
static void lcd_exit_ota_mode(const char*){}
static bool lv_is_initialized(){return true;}
static void* lv_scr_act(){return reinterpret_cast<void*>(1);}
static void lv_obj_invalidate(void*){}
static unsigned long last_touch_or_input_ms=0;
static bool g_lcd_maintenance_aborted=false;
static bool ui_is_sleep_eligible_menu_screen(int screen){return screen==SCREEN_HOME||screen==SCREEN_SHOPPING_LIST||screen==SCREEN_SETTINGS;}
'''
    for anchor in ('static void hide_provisioning_screen() {',
                   'static void update_provision_status_label(',
                   'static void provision_ui_service() {',
                   'static void provision_ui_qr_timeout() {'):
        text += definition(ui, anchor) + '\n'
    for src, anchor in ((animation, 'static void ensure_awake_for_ui('),
                        (main, 'static void user_activity_bump_quiet('),
                        (activity, 'static bool lcd_sleep_retry_ready(')):
        text += definition(src, anchor) + '\n'
    text += next(line for line in activity.splitlines() if ' GUARDIAN_FORCE_SLEEP_MS =' in line) + '\n'
    text += 'static void guardian_tick(){\n' + definition(main,
        '  if (!g_in_light_sleep && GUARDIAN_FORCE_SLEEP_MS > 0) {') + '\nloop_continue:;\n}\n'
    status = uart[uart.index('  } else if (strcmp(type, "PROVISION_STATUS") == 0) {'):]
    # Exact UART-owned flag update; the subsequent queue is a delivery boundary.
    admission = status[status.index('      bool active ='):status.index('      bool unprovisioned =')]
    text += 'static void receive_status(const char* state,bool deliver_ui=true){\nlast_sense_rx_ms=now_ms;\n' + admission
    text += 'if(deliver_ui)update_provision_status_label(state);\n}\n'
    # The post-service heartbeat does not reopen a guide. A stalled UI's last
    # success lease remains finite even when its legacy flag is false.
    has_guardian = 'static uint32_t lcd_guardian_awake_ms(' in activity
    if not has_guardian:
        text += 'static uint32_t lcd_guardian_awake_ms(uint32_t now){return now-guardian_awake_start_ms.load();}\n'
    if has_guardian:
        service = definition(ui, 'static void provision_ui_service() {')
        assert service.index('lcd_guardian_begin_foreground(') < service.index('provision_flow.close();')
    # Inject a newly accepted claim during the actual sleep handshake wait.
    delay = definition(text, 'static void vTaskDelay(unsigned long delay){')
    text = text.replace(delay, '''static void accept_late_progress();
static void vTaskDelay(unsigned long delay){
 now_ms+=delay;++delay_calls;
 if(scenario=="late_progress")accept_late_progress();else sleep_ready_received=true;
}''')
    return text + r'''
static void accept_late_progress(){
 if(!injected){injected=true;provision_flow.begin();update_provision_status_label("claiming");}
}
static void expect_held(){
 const auto before=now_ms;enterLightSleep();
 assert(teardown==0&&sleep_messages==0&&now_ms==before);
 assert(!g_lcd_sleep_handshake_active.load()&&!g_lcd_sleep_commit_gate.load()&&lock_depth==0);
}
static void complete_at(uint32_t start){
 provision_flow.close();provision_flow.begin();now_ms=start-100;
 receive_status("claiming");now_ms=start;receive_status("connected");
 assert(provision_flow.step==LcdProvisionFlow::Complete);
}
static void fresh_physical_input(const char* reason){
 ensure_awake_for_ui(reason);user_activity_bump_quiet(reason,false);resetActivityTimer();
}
int main(int argc,char**argv){
 assert(argc==2);scenario=argv[1];now_ms=300000;last_sense_rx_ms=1;
 provisioning_active=true;link_synced=false;sense_state=SENSE_ASLEEP;sleep_deny_count=0;
 provision_flow.begin();
 const bool completion_case=scenario.find("complete_")==0;
 if(completion_case){
   uint32_t completed=371635;
   if(scenario=="complete_wrap")completed=UINT32_MAX-100;
   if(scenario=="complete_deadline_zero")completed=uint32_t(0u-14500u);
   if(scenario=="complete_clock_zero")completed=0;
   complete_at(completed);
   const uint32_t deadline=provision_flow.progress_sleep_deadline_ms.load();
   now_ms=completed+3237;receive_status("connected");
   assert(!provisioning_active&&provision_flow.step==LcdProvisionFlow::Complete);
   if(scenario=="complete_heartbeat_guardian"){
     guardian_tick();assert(teardown==0&&sleep_messages==0);
     now_ms=completed+4499;provision_ui_service();guardian_tick();
     assert(provision_flow.active()&&teardown==0);
     now_ms=completed+4500;provision_ui_service();
     assert(!provision_flow.active()&&!provisioning_active&&provision_return_home_pending);
     assert(guardian_awake_start_ms==now_ms&&!guardian_sleep_triggered);
     guardian_tick();assert(teardown==0&&sleep_messages==0);
     now_ms+=9999;receive_status("connected");resetActivityTimer();guardian_tick();
     assert(teardown==0&&sleep_messages==0&&guardian_awake_start_ms==completed+4500);
     now_ms=completed+4500+GUARDIAN_FORCE_SLEEP_MS-1;guardian_tick();assert(teardown==0);
     ++now_ms;guardian_tick();assert(teardown==1); // Unattended guardian still terminates.
   }else if(scenario=="complete_stalled_at"){
     now_ms=completed+14500;receive_status("connected");enterLightSleep();assert(teardown==1);
   }else if(scenario=="complete_peer_lost"){
     last_sense_rx_ms=0;sleep_deny_count=SLEEP_DENY_MAX_COUNT;
     now_ms=completed+14499;expect_held();
     now_ms=completed+14500;enterLightSleep();assert(teardown==1);
   }else{
     for(uint32_t age:{4000u,7000u,10000u,14499u}){
       now_ms=completed+age;receive_status("connected");expect_held();
     }
     assert(provision_flow.progress_sleep_deadline_ms.load()==deadline);
     now_ms=completed+14500;receive_status("connected");enterLightSleep();assert(teardown==1);
   }
   printf("PASS %s completion finite; sleep_messages=%u teardown=%d\n",scenario.c_str(),sleep_messages,teardown);return 0;
 }
 if(scenario=="touch_guardian"||scenario=="scroll_guardian"||scenario=="touch_no_pending"||
    scenario=="abort_user_guardian"||scenario=="abort_background_guardian"||
    scenario=="background_guardian"||scenario=="guardian_sample_race"||scenario=="guardian_wrap"){
   provision_flow.close();provisioning_active=false;now_ms=390000;
   g_panel_enabled=g_lvgl_running=true;g_idle_screen_dark=false;g_backlight_duty=128;
   guardian_sleep_triggered=true;
   if(scenario=="guardian_sample_race"){
     guardian_awake_start_ms=now_ms+1;guardian_tick();assert(teardown==0);
     assert(lcd_guardian_awake_ms(now_ms)==0);
   }else if(scenario=="guardian_wrap"){
     guardian_awake_start_ms=UINT32_MAX-99;now_ms=20;
     assert(lcd_guardian_awake_ms(now_ms)==120);guardian_tick();assert(teardown==0);
     now_ms=uint32_t(UINT32_MAX-99+GUARDIAN_FORCE_SLEEP_MS);guardian_tick();assert(teardown==1);
   }else if(scenario=="background_guardian"||scenario=="abort_background_guardian"){
     if(scenario=="background_guardian"){
       receive_status("connected");resetActivityTimer();user_activity_bump_quiet("settings_fw_info",false);
     }else abort_sleep_transition("ota_before_teardown",false);
     assert(guardian_awake_start_ms==0&&guardian_sleep_triggered);
     guardian_tick();assert(teardown==1);
   }else{
     const bool pending=scenario!="touch_no_pending"&&scenario!="abort_user_guardian";
     sleep_deny_count=pending?SLEEP_DENY_MAX_COUNT:0;
     sleep_deny_active=sleep_wait_for_sense_idle=pending;
     sleep_handshake_fail_count=pending?3:0;sleep_retry_allowed_ms=pending?now_ms+5000:0;
     g_idle_screen_dark=true;g_panel_enabled=g_lvgl_running=false;g_backlight_duty=0;
     if(scenario=="abort_user_guardian")abort_sleep_transition("touch_during_teardown",true);
     else fresh_physical_input(scenario=="scroll_guardian"?"scroll_pending":"touch_press");
     assert(guardian_awake_start_ms==now_ms&&!guardian_sleep_triggered);
     assert(!sleep_wait_for_sense_idle&&!sleep_deny_active&&sleep_deny_count==0);
     assert(g_panel_enabled&&g_lvgl_running&&g_backlight_duty>0&&!g_idle_screen_dark);
     guardian_tick();assert(teardown==0&&sleep_messages==0);
     const auto physical=now_ms;
     for(uint32_t age:{1u,10u,1000u,9999u,299999u}){now_ms=physical+age;resetActivityTimer();guardian_tick();assert(teardown==0);}
     now_ms=physical+GUARDIAN_FORCE_SLEEP_MS;guardian_tick();assert(teardown==1);
   }
   printf("PASS %s guardian=%u teardown=%d\n",scenario.c_str(),guardian_awake_start_ms.load(),teardown);return 0;
 }
 const bool no_start=scenario=="stale_qr"||scenario=="inactive"||scenario=="late_progress"||scenario=="preview_no_lease";
 uint32_t start=300000;
 if(scenario=="clock_zero")start=0;
 if(scenario=="wrap")start=0xfffffff0u;
 if(scenario=="deadline_zero")start=uint32_t(0u-240000u);
 now_ms=start;
 if(!no_start)update_provision_status_label("connecting");
 now_ms=uint32_t(start+9001);
 if(scenario=="observed_claim_gap"){
   // Recorded LCD timeline: guardian already due at300054, claiming318700,
   // lastPONG320332, sleep begins328335. Sense resumes121.335s afterlastPONG.
   provision_flow.close();provision_flow.begin();now_ms=292000;
   update_provision_status_label("connecting");now_ms=318700;
   update_provision_status_label("claiming");last_sense_rx_ms=320332;
   for(uint32_t t:{328335u,354146u,379957u,405109u,441667u}){now_ms=t;expect_held();provision_ui_service();}
   assert(provision_flow.claiming&&provision_flow.step==LcdProvisionFlow::Connecting);
   now_ms=459000;update_provision_status_label("connected");
   assert(provision_flow.step==LcdProvisionFlow::Complete);
   now_ms+=4500;provision_ui_service();assert(!provision_flow.active()&&!provisioning_active);
 }else if(scenario=="deadline_before"||scenario=="nonrenewing_claim"||scenario=="nonrenewing_app"){
   now_ms=uint32_t(start+239999);
   if(scenario=="nonrenewing_claim")update_provision_status_label("claiming");
   if(scenario=="nonrenewing_app")update_provision_status_label("app_connected");
   expect_held();now_ms=uint32_t(start+240000);enterLightSleep();assert(teardown==1);
 }else if(scenario=="deadline_at"||scenario=="timeout_no_service"){
   now_ms=uint32_t(start+240000);enterLightSleep();assert(teardown==1);
 }else if(scenario=="timeout_service"||scenario=="timeout_recovery"){
   now_ms=uint32_t(start+240000);provision_ui_service();
   assert(provision_flow.step==LcdProvisionFlow::Failed&&provision_flow.progress_timed_out&&renders==2);
   update_provision_status_label("claiming");assert(provision_flow.step==LcdProvisionFlow::Failed);
   if(scenario=="timeout_recovery"){
     update_provision_status_label("connected");assert(provision_flow.step==LcdProvisionFlow::Complete);
     expect_held();now_ms+=14500;
   }
   enterLightSleep();assert(teardown==1);
 }else if(scenario=="late_progress"){
   sense_state=SENSE_AWAKE;link_synced=true;last_sense_rx_ms=now_ms-9001;
   enterLightSleep();assert(injected&&sleep_messages==1&&teardown==0);
 }else if(scenario=="terminal_connected"||scenario=="terminal_failed"||scenario=="terminal_error"||
          scenario=="cancel_idle"||scenario=="cancel_close"||scenario=="retry_clears"||scenario=="qr_timeout"||no_start){
   if(scenario=="terminal_connected")update_provision_status_label("connected");
   if(scenario=="terminal_failed")update_provision_status_label("failed");
   if(scenario=="terminal_error")update_provision_status_label("error");
   if(scenario=="cancel_idle")update_provision_status_label("idle");
   if(scenario=="cancel_close")hide_provisioning_screen();
   if(scenario=="retry_clears")provision_flow.retry(true);
   if(scenario=="qr_timeout")provision_ui_qr_timeout();
   if(scenario=="inactive")provisioning_active=false;
   if(scenario=="preview_no_lease"){
     provision_ui_preview_active=true;update_provision_status_label("claiming");
   }
   if(scenario=="terminal_connected"){expect_held();now_ms+=14500;}
   enterLightSleep();assert(teardown==1);
 }else{
   if(scenario!="connecting_gap")update_provision_status_label("claiming");
   if(scenario=="clock_zero")last_sense_rx_ms=0;
   expect_held();now_ms=uint32_t(start+240000);enterLightSleep();assert(teardown==1);
 }
 assert(!g_lcd_sleep_handshake_active.load()&&!g_lcd_sleep_commit_gate.load()&&lock_depth==0);
 printf("PASS %s renders=%u sleep_messages=%u teardown=%d\n",scenario.c_str(),renders,sleep_messages,teardown);
}
'''


def run(root, out, expected, cases=CASES):
    out.mkdir(parents=True, exist_ok=True)
    source, binary = out / 'check.cpp', out / 'check'
    source.write_text(harness(root))
    command = [shutil.which('clang++') or shutil.which('c++'), '-std=c++17',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               str(source), '-o', str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=45)
    (out / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    results = []
    for case in cases:
        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=8)
        (out / (case + '.log')).write_text(result.stdout + result.stderr)
        results.append({'case': case, 'exit': result.returncode})
    failed = {r['case'] for r in results if r['exit']}
    assert failed == expected, f'{out}: failures {sorted(failed)} expected {sorted(expected)}'
    return {'source_root': str(root), 'source_sha256': {f: hashlib.sha256((root / f).read_bytes()).hexdigest() for f in FILES},
            'cases': results, 'expected_failures': sorted(expected), 'matched': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--negative-source-root', type=Path)
    parser.add_argument('--completion-negative-source-root', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = {'scope': __doc__, 'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'current': run(args.source_root.resolve(), args.out / 'current', set())}
    if args.negative_source_root:
        result['negative'] = run(args.negative_source_root.resolve(), args.out / 'negative', NEGATIVE_FAILURES, LEGACY_CASES)
    if args.completion_negative_source_root:
        result['completion_negative'] = run(args.completion_negative_source_root.resolve(),
            args.out / 'completion-negative', COMPLETION_NEGATIVE_FAILURES, COMPLETION_NEGATIVE_CASES)
    (args.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS {len(CASES)} current cases; progress negative failures='
          f'{len(NEGATIVE_FAILURES) if args.negative_source_root else "not run"}; completion negative failures='
          f'{len(COMPLETION_NEGATIVE_FAILURES) if args.completion_negative_source_root else "not run"}')


if __name__ == '__main__':
    main()

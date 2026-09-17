#!/usr/bin/env python3
"""Actual LCD setup flow/service and sleep funnel under prolonged claim silence.

The frozen175 negative control executes unchanged runtime functions. Rendering,
UART and RTOS waits are controlled boundaries; no panel/hardware claim is made.
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
         'timeout_no_service', 'timeout_recovery', 'preview_no_lease')
NEGATIVE_FAILURES = {'observed_claim_gap', 'connecting_gap', 'deadline_before',
                     'nonrenewing_claim', 'nonrenewing_app', 'clock_zero', 'wrap',
                     'deadline_zero', 'late_progress'}
FILES = ('LCD_Minimal/lcd_provision_flow.h', 'LCD_Minimal/lcd_provision.h',
         'LCD_Minimal/lcd_sleep.h')


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
static void provision_qr_wait_clear(const char*){}
static void hide_provision_intro_screen(const char*){}
static void provision_ui_render(){++renders;}
static void provision_ui_preview(int){provision_ui_preview_active=false;}
'''
    for anchor in ('static void hide_provisioning_screen() {',
                   'static void update_provision_status_label(',
                   'static void provision_ui_service() {',
                   'static void provision_ui_qr_timeout() {'):
        text += definition(ui, anchor) + '\n'
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
int main(int argc,char**argv){
 assert(argc==2);scenario=argv[1];now_ms=300000;last_sense_rx_ms=1;
 provisioning_active=true;link_synced=false;sense_state=SENSE_ASLEEP;sleep_deny_count=0;
 provision_flow.begin();
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


def run(root, out, expected):
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
    for case in CASES:
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
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = {'scope': __doc__, 'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'current': run(args.source_root.resolve(), args.out / 'current', set())}
    if args.negative_source_root:
        result['negative'] = run(args.negative_source_root.resolve(), args.out / 'negative', NEGATIVE_FAILURES)
    (args.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS {len(CASES)} current cases; frozen negative failures={len(NEGATIVE_FAILURES) if args.negative_source_root else "not run"}')


if __name__ == '__main__':
    main()

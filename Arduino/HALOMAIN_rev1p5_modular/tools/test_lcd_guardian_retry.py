#!/usr/bin/env python3
"""Execute the LCD guardian and shared retry gate against a controlled peer.

Uses actual production guardian, retry-readiness, denial handling and user
activity functions. Serial/time and final sleep teardown are host boundaries;
this does not prove physical sleep, display or radio behavior. The negative
control removes only the new guardian call-site gate, reproducing its previous
behavior while leaving the retry helper and peer responses unchanged.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FILES = ('LCD_Minimal/LCD_Minimal.ino', 'LCD_Minimal/lcd_activity.h',
         'LCD_Minimal/lcd_sleep.h')
CASES = ('busy_peer', 'pre_ready_peer', 'before_guardian', 'asleep_peer',
         'silent_peer_boundary', 'unknown_without_rx', 'timed_retry_boundary',
         'timed_denial_budget', 'shared_ota_hold', 'uart_ota_hold',
         'touch_releases_wait', 'user_gate_guardian_recovery', 'normal_retry_gate')
NEGATIVE_FAILURES = {'busy_peer', 'pre_ready_peer', 'silent_peer_boundary',
                     'timed_retry_boundary', 'timed_denial_budget'}


def definition(text, anchor):
    spec = importlib.util.spec_from_file_location(
        'maintenance_sleep', ROOT / 'tools/test_lcd_maintenance_sleep.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.definition(text, anchor)


def harness(root, negative=False):
    main, activity, sleep = ((root / path).read_text() for path in FILES)
    guardian = definition(main, '  if (!g_in_light_sleep && GUARDIAN_FORCE_SLEEP_MS > 0) {')
    gate = definition(activity, 'static bool lcd_sleep_retry_ready(unsigned long now_ms) {')
    touch = definition(main, 'static void user_activity_bump_quiet(')
    helpers = '\n'.join(definition(activity, anchor) for anchor in (
        'static void lcd_guardian_begin_foreground(', 'static uint32_t lcd_guardian_awake_ms(')
        if anchor in activity)
    # Call-site closure: both paths must use this exact helper. This supplements
    # behavioral execution and is not a substitute for the guardian test.
    assert guardian.count('lcd_sleep_retry_ready(now_ms)') == 1
    assert main.count('if (!lcd_sleep_retry_ready(now_ms)) {') == 1
    if negative:
        guardian = guardian.replace('else if (lcd_sleep_retry_ready(now_ms))', 'else')
    notify = definition(sleep, 'static bool notify_sense_sleep() {')
    denial = definition(notify, '      if (sleep_deny_received) {')
    enter = definition(sleep, 'static void enterLightSleep() {')
    force = definition(enter, '  if (sleep_deny_count >= SLEEP_DENY_MAX_COUNT) {')
    count = definition(enter, '    if (sleep_deny_active) {')
    sleep_cleanup = '\n'.join(next(line for line in enter.splitlines() if line.strip() == statement)
                              for statement in ('sleep_retry_allowed_ms = 0;',
                                                'sleep_wait_for_sense_idle = false;'))
    constants = '\n'.join(next(line for line in text.splitlines() if f' {name} =' in line)
                          for text, name in ((activity, 'GUARDIAN_FORCE_SLEEP_MS'),
                                             (main, 'SENSE_RECENT_RX_FOR_SLEEP_MS'),
                                             (main, 'SLEEP_DENY_MAX_COUNT'),
                                             (main, 'SLEEP_DENY_RETRY_DEFAULT_MS')))
    return r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
static unsigned long now_ms=300000;
static unsigned long millis(){return now_ms;}
static struct {template<class... A>void printf(const char*,A...){} void println(const char*){}} Serial;
enum SenseState {SENSE_UNKNOWN,SENSE_AWAKE,SENSE_ASLEEP};
static SenseState sense_state=SENSE_AWAKE;
static const char* sense_state_name(SenseState){return "controlled_peer";}
static bool g_in_light_sleep=false;
static std::atomic<uint32_t> guardian_awake_start_ms{0};
static std::atomic<bool> guardian_sleep_triggered{false};
static unsigned long last_sense_rx_ms=299999;
static unsigned long sleep_retry_allowed_ms=0,last_sleep_retry_log_ms=0;
static bool sleep_wait_for_sense_idle=false,sleep_deny_active=false,sleep_deny_received=false;
static char sleep_deny_reason[24]="op_inflight";
static unsigned long sleep_deny_retry_ms=5000;
static unsigned sleep_deny_count=0,sleep_handshake_fail_count=0;
static bool sleep_retry_requires_user=false,user_activity_since_sleep=false;
static bool ota_hold=false,uart_ota_hold=false;
static bool lcd_ota_uart_active(){return uart_ota_hold;}
static bool sleep_blocked_for_ota(){return ota_hold;}
static bool peer_denies=true,panel_dark=false;
static unsigned handshakes=0,forced=0,teardowns=0,service_ticks=0;
static void sleep_enter_wait_low_power(const char*){panel_dark=true;}
static unsigned long last_user_activity_ms=0,last_touch_or_input_ms=0,home_shown_ms=0;
static int ui_screen_state=0;
static bool ui_is_sleep_eligible_menu_screen(int){return true;}
static bool g_lcd_maintenance_active=false,g_lcd_maintenance_aborted=false;
''' + constants + '\n' + helpers + '\n' + gate + '\n' + touch + r'''
static bool controlled_handshake(){
 ++handshakes;
 if(!peer_denies)return true;
 sleep_deny_received=true;last_sense_rx_ms=now_ms;
''' + denial + r'''
 return true;
}
static void enterLightSleep(){
 bool force_sleep=false;
''' + force + r'''
 if(force_sleep){++forced;++teardowns;return;}
 if(!controlled_handshake()){
''' + count + r'''
 }
''' + sleep_cleanup + r'''
 ++teardowns;
}
static void guardian_tick(){
''' + guardian + r'''
 ++service_ticks;
loop_continue:;
}
int main(int argc,char**argv){
 assert(argc==2);const std::string name=argv[1];
 if(name=="busy_peer"||name=="pre_ready_peer"){
   if(name=="pre_ready_peer")strcpy(sleep_deny_reason,"pre_ready_block");
   guardian_tick();assert(handshakes==1&&sleep_deny_count==1&&panel_dark);
   for(unsigned i=0;i<600;++i){now_ms+=100;last_sense_rx_ms=now_ms;guardian_tick();}
   assert(handshakes==1&&sleep_deny_count==1&&forced==0&&teardowns==0);
   assert(sleep_wait_for_sense_idle&&service_ticks==600&&panel_dark);
 }else if(name=="before_guardian"){
   now_ms=GUARDIAN_FORCE_SLEEP_MS-1;guardian_tick();assert(handshakes==0&&service_ticks==1);
 }else if(name=="asleep_peer"||name=="unknown_without_rx"){
   peer_denies=false;sleep_wait_for_sense_idle=true;
   sense_state=name=="asleep_peer"?SENSE_ASLEEP:SENSE_UNKNOWN;
   if(name=="unknown_without_rx")last_sense_rx_ms=0;
   guardian_tick();assert(handshakes==1&&teardowns==1&&!sleep_wait_for_sense_idle);
 }else if(name=="silent_peer_boundary"){
   peer_denies=false;sleep_wait_for_sense_idle=true;
   last_sense_rx_ms=now_ms-SENSE_RECENT_RX_FOR_SLEEP_MS+1;
   guardian_tick();assert(handshakes==0&&sleep_wait_for_sense_idle);
   ++now_ms;guardian_tick();assert(handshakes==1&&teardowns==1&&!sleep_wait_for_sense_idle);
 }else if(name=="timed_retry_boundary"){
   peer_denies=false;sleep_retry_allowed_ms=now_ms+5000;
   for(unsigned i=0;i<500;++i){guardian_tick();now_ms+=10;}
   assert(handshakes==0&&teardowns==0);
   guardian_tick();assert(handshakes==1&&teardowns==1);
 }else if(name=="timed_denial_budget"){
   strcpy(sleep_deny_reason,"controlled_busy");
   for(unsigned n=0;n<SLEEP_DENY_MAX_COUNT;++n){
     guardian_tick();assert(handshakes==n+1&&sleep_deny_count==n+1);
     assert(forced==0&&teardowns==0);
     for(unsigned tick=0;tick<49;++tick){now_ms+=100;guardian_tick();}
     assert(handshakes==n+1&&forced==0);
     now_ms+=100;
   }
   guardian_tick();assert(forced==1&&teardowns==1&&sleep_deny_count==0);
 }else if(name=="shared_ota_hold"||name=="uart_ota_hold"){
   ota_hold=name=="shared_ota_hold";uart_ota_hold=name=="uart_ota_hold";
   sleep_deny_count=SLEEP_DENY_MAX_COUNT;
   for(unsigned i=0;i<100;++i){guardian_tick();now_ms+=100;}
   assert(handshakes==0&&forced==0&&teardowns==0);
 }else if(name=="touch_releases_wait"){
   sleep_wait_for_sense_idle=true;sleep_retry_requires_user=true;
   sleep_handshake_fail_count=3;sleep_retry_allowed_ms=now_ms+60000;
   g_lcd_maintenance_active=true;user_activity_bump_quiet("touch",false);
   assert(!sleep_wait_for_sense_idle&&!sleep_retry_requires_user);
   assert(sleep_retry_allowed_ms==0&&sleep_handshake_fail_count==0);
   assert(user_activity_since_sleep&&g_lcd_maintenance_aborted);
   assert(last_user_activity_ms==now_ms&&home_shown_ms==now_ms);
 }else if(name=="user_gate_guardian_recovery"){
   peer_denies=false;sleep_retry_requires_user=true;sleep_retry_allowed_ms=now_ms;
   guardian_tick();assert(teardowns==1&&handshakes==1);
 }else if(name=="normal_retry_gate"){
   sleep_wait_for_sense_idle=true;assert(!lcd_sleep_retry_ready(now_ms));
   sense_state=SENSE_ASLEEP;assert(lcd_sleep_retry_ready(now_ms));
   sleep_retry_allowed_ms=now_ms+100;assert(!lcd_sleep_retry_ready(now_ms));
   now_ms+=100;assert(lcd_sleep_retry_ready(now_ms));
 }else assert(false);
 printf("PASS %s handshakes=%u denials=%u forced=%u teardown=%u service=%u\n",
        name.c_str(),handshakes,sleep_deny_count,forced,teardowns,service_ticks);
}
'''


def run(root, out, negative):
    out.mkdir(parents=True, exist_ok=True)
    source, binary = out / 'check.cpp', out / 'check'
    source.write_text(harness(root, negative))
    command = [shutil.which('clang++') or shutil.which('c++'), '-std=c++17',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               str(source), '-o', str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=45)
    (out / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    compiled.check_returncode()
    cases = []
    for case in CASES:
        result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=8)
        (out / (case + '.log')).write_text(result.stdout + result.stderr)
        cases.append({'case': case, 'exit': result.returncode})
    failed = {case['case'] for case in cases if case['exit']}
    expected = NEGATIVE_FAILURES if negative else set()
    assert failed == expected, f'Failures {sorted(failed)}; expected {sorted(expected)}'
    return {'negative': negative, 'cases': cases, 'expected_failures': sorted(expected),
            'matched': True, 'harness_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--negative-control', action='store_true')
    args = parser.parse_args()
    root = args.source_root.resolve()
    result = {'scope': __doc__, 'source_root': str(root),
              'source_sha256': {f: hashlib.sha256((root / f).read_bytes()).hexdigest() for f in FILES},
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'current': run(root, args.out / 'current', False)}
    if args.negative_control:
        result['negative'] = run(root, args.out / 'negative', True)
    (args.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'PASS {len(CASES)} current cases; negative regression failures='
          f'{len(NEGATIVE_FAILURES) if args.negative_control else "not run"}')


if __name__ == '__main__':
    main()

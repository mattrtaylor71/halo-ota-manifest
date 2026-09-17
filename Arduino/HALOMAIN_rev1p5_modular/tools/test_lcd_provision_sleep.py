#!/usr/bin/env python3
"""Actual LCD sleep funnel/handshake under live and stale setup evidence.

Uses production function extraction and controlled RTOS/clock/peer boundaries.
No radio, display, GPIO, or physical deep-sleep claim. Frozen169 is a negative
control via --source-root; failures remain failures unless explicitly expected.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ('fresh_unsynced', 'fresh_synced', 'fresh_asleep', 'fresh_unknown',
         'fresh_force_count', 'fresh_ready', 'fresh_boundary', 'fresh_wrap',
         'stale_asleep', 'stale_unsynced', 'inactive_asleep',
         'late_provision', 'ready_provision_race', 'before_guard_provision',
         'guardian_denial_loop')


def harness(root):
    spec = importlib.util.spec_from_file_location(
        'sleep_custody', ROOT / 'tools/test_lcd_sleep_media_custody.py')
    base = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(base)
    definition = base.load_base().definition
    text = base.harness(root)
    text = text[:text.index('\nint main(')]
    main = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    deny_max = re.search(r'SLEEP_DENY_MAX_COUNT\s*=\s*(\d+);', main).group(1)
    text = text.replace('SLEEP_DENY_MAX_COUNT=4,', 'SLEEP_DENY_MAX_COUNT=' + deny_max + ',')
    old = definition(text, 'static void vTaskDelay(unsigned long delay){')
    text = text.replace(old, '''static void vTaskDelay(unsigned long delay){
      now_ms+=delay;++delay_calls;
      if(scenario=="late_provision"){
        provisioning_active=true;last_sense_rx_ms=now_ms;sense_state=SENSE_AWAKE;
      }else if(scenario=="guardian_denial_loop"){
        sleep_deny_received=true;strcpy(sleep_deny_reason,"op_inflight");
        sleep_deny_retry_ms=5000;last_sense_rx_ms=now_ms;
      }else sleep_ready_received=true;
    }''')
    old = definition(text, 'static void serial_hook(const char* s){')
    text = text.replace(old, '''static void serial_hook(const char* s){
      if(!injected && scenario=="ready_provision_race" && strstr(s,"sense_ready_recent ->")){
        injected=true;provisioning_active=true;last_sense_rx_ms=now_ms;
      }
    }''')
    text = text.replace('struct LcdMaintenanceStorageGuard {',
                        'static void provision_guard_hook();\nstruct LcdMaintenanceStorageGuard {')
    text = text.replace('LcdMaintenanceStorageGuard(){',
                        'LcdMaintenanceStorageGuard(){provision_guard_hook();')
    return text + r'''
static void provision_guard_hook(){
 if(scenario=="before_guard_provision"){
  provisioning_active=true;last_sense_rx_ms=now_ms;
 }
}
int main(int argc,char** argv){
 assert(argc==2);scenario=argv[1];now_ms=300150;last_sense_rx_ms=now_ms-1500;
 provisioning_active=true;link_synced=false;sense_state=SENSE_AWAKE;
 sleep_deny_count=0;
 const unsigned initial_fail=sleep_handshake_fail_count;
 const unsigned initial_deny=sleep_deny_count;
 if(scenario=="fresh_synced")link_synced=true;
 if(scenario=="fresh_asleep"||scenario=="stale_asleep"||scenario=="inactive_asleep")sense_state=SENSE_ASLEEP;
 if(scenario=="fresh_unknown")sense_state=SENSE_UNKNOWN;
 if(scenario=="fresh_force_count")sleep_deny_count=SLEEP_DENY_MAX_COUNT;
 if(scenario=="fresh_ready"||scenario=="ready_provision_race")last_sense_sleep_ready_ms=now_ms-10;
 if(scenario=="fresh_boundary")last_sense_rx_ms=now_ms-SENSE_RX_STALE_MS;
 if(scenario=="fresh_wrap"){now_ms=20;last_sense_rx_ms=0xfffffff0UL;}
 if(scenario=="stale_asleep"||scenario=="stale_unsynced")last_sense_rx_ms=now_ms-90000;
 if(scenario=="inactive_asleep"||scenario=="late_provision"||scenario=="ready_provision_race"||scenario=="before_guard_provision")provisioning_active=false;
 if(scenario=="before_guard_provision")sense_state=SENSE_ASLEEP;
 if(scenario=="guardian_denial_loop"){
   for(unsigned i=0;i<22;++i){enterLightSleep();now_ms+=70;}
 }else enterLightSleep();
 const bool may_sleep=scenario=="stale_asleep"||scenario=="stale_unsynced"||scenario=="inactive_asleep";
 printf("OBS %s teardown=%d msgs=%u denies=%u failures=%u now=%lu\n",scenario.c_str(),teardown,sleep_messages,sleep_deny_count,sleep_handshake_fail_count,now_ms);
 assert(teardown==(may_sleep?1:0));
 if(!may_sleep){
   assert(sleep_messages==(scenario=="late_provision"?1u:0u));
   assert(sleep_handshake_fail_count==initial_fail);
   assert(sleep_deny_count==(scenario=="fresh_force_count"?SLEEP_DENY_MAX_COUNT:initial_deny));
 }
 assert(lock_depth==0&&!g_lcd_sleep_handshake_active&&!g_lcd_sleep_commit_gate);
 printf("PASS %s\n",scenario.c_str());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--case', choices=CASES, action='append')
    parser.add_argument('--expect-failures', type=int, default=0)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-provision-sleep-') as tmp:
        tmp = Path(tmp)
        source, binary = tmp / 'check.cpp', tmp / 'check'
        source.write_text(harness(args.source_root))
        subprocess.run([shutil.which('c++'), '-std=c++17', str(source), '-o', str(binary)],
                       check=True, timeout=30)
        cases = args.case or CASES
        failures = sum(subprocess.run([str(binary), case], timeout=5).returncode != 0
                       for case in cases)
        print(f'{len(cases)} cases, {failures} failures; expected={args.expect_failures}')
        return int(failures != args.expect_failures)


if __name__ == '__main__':
    raise SystemExit(main())

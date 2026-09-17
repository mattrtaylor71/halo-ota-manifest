#!/usr/bin/env python3
"""Exercise actual Wi-Fi polling when provisioning replaces a normal STA owner.

Reuses the Wi-Fi recovery SDK doubles; no device, network or firmware build.
The optional frozen negative source must compile and fail the same assertions.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_provisioning_display_status import definition
from test_wifi_recovery import harness as recovery_harness, no_core


ROOT = Path(__file__).resolve().parents[1]
WIFI = 'Sense_Minimal/sense_wifi.h'


TESTS = r'''
static unsigned checks=0, failures=0;
static void check(bool ok,const char* name){
  ++checks;
  if(!ok){if(failures<12)std::printf("FAIL %s\n",name);++failures;}
}
int main(){
  const wl_status_t statuses[]={WL_IDLE_STATUS,WL_NO_SSID_AVAIL,
      static_cast<wl_status_t>(2),WL_CONNECTED,WL_CONNECT_FAILED,
      WL_CONNECTION_LOST,WL_DISCONNECTED,static_cast<wl_status_t>(254)};
  const uint32_t starts[]={0,1,UINT32_MAX-15};
  const uint32_t ages[]={0,1,WIFI_CONNECT_TIMEOUT_MS,WIFI_CONNECT_TIMEOUT_MS+1};
  for(auto status:statuses)for(auto start:starts)for(auto age:ages)
  for(bool recently_polled:{false,true})for(bool other_work:{false,true}){
    reset();provisioning=true;live_status=status;
    wifi_connect_inflight=true;wifi_inflight_start_ms=start;
    std::strcpy(wifi_connect_owner,"normal-sta");
    now_ms=start+age;wifi_last_poll_ms=recently_polled?now_ms:now_ms-1000;
    const auto poll_before=wifi_last_poll_ms,clock_before=now_ms;
    http_mutex=(void*)1;mutex_busy=other_work;http_inflight=other_work;
    wifi_maint_consecutive_fails=2;wifi_maint_retry_pending=true;
    wifi_maint_last_failure_ms=17;
    service_wifi_maintenance(now_ms);wifi_guard_poll();
    check(!wifi_connect_inflight&&!wifi_inflight_start_ms&&!wifi_connect_owner[0],
          "provisioning retires stale STA owner before throttle/status/deadline");
    check(!poll_failures&&!poll_timeouts&&!begin_calls&&!reset_calls,
          "provisioning does not enter scan, disconnect, mode or begin paths");
    check(now_ms==clock_before&&wifi_last_poll_ms==poll_before&&
          !mutex_takes&&!mutex_gives&&!logs&&!uart_calls,
          "handoff has no wait, HTTP lease, poll consumption or failure report");
    check(wifi_maint_consecutive_fails==2&&wifi_maint_retry_pending&&
          wifi_maint_last_failure_ms==17&&live_status==status&&provisioning,
          "handoff preserves provisioning, live Wi-Fi and recovery budget");
  }

  // After handoff, repeated polling cannot resurrect the previous attempt.
  reset();provisioning=true;wifi_connect_inflight=true;
  wifi_inflight_start_ms=1;std::strcpy(wifi_connect_owner,"normal-sta");
  now_ms=2;wifi_guard_poll();now_ms+=60000;wifi_guard_poll();
  check(!wifi_connect_inflight&&!poll_failures&&!poll_timeouts,
        "retired attempt stays inert beyond its old deadline");
  provisioning=false;tick();
  check(begin_calls==1&&wifi_connect_inflight&&begun_ssid=="saved-home",
        "ordinary maintenance can claim a fresh attempt after setup ends");

  // Preserve the existing independent diagnostic owner and normal171 guards.
  reset();provisioning=true;backup_allowed=false;wifi_connect_inflight=true;
  wifi_inflight_start_ms=1;std::strcpy(wifi_connect_owner,"normal-sta");
  wifi_guard_poll();
  check(wifi_connect_inflight&&wifi_inflight_start_ms==1&&
        !std::strcmp(wifi_connect_owner,"normal-sta"),
        "backup lease rejection remains authoritative");
  reset();wifi_connect_inflight=true;wifi_inflight_start_ms=0;
  now_ms=WIFI_CONNECT_TIMEOUT_MS;wifi_guard_poll();
  check(wifi_connect_inflight&&!poll_failures&&!poll_timeouts,
        "normal zero-clock attempt retains its inclusive deadline");
  now_ms+=1000;http_inflight=true;wifi_guard_poll();
  check(wifi_connect_inflight&&!poll_failures&&!poll_timeouts,
        "normal timeout still yields to live HTTP");
  now_ms+=1000;http_inflight=false;wifi_guard_poll();
  check(!wifi_connect_inflight&&poll_failures==1&&!poll_timeouts,
        "normal timeout resumes after HTTP releases ownership");
  reset();wifi_connect_inflight=true;live_status=WL_CONNECTED;
  http_inflight=true;now_ms=30000;wifi_guard_poll();
  check(!wifi_connect_inflight&&wifi_connected_ms==now_ms&&!poll_failures,
        "normal connected status can complete while HTTP is busy");
  std::printf("%s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def harness(source_root):
    text = (source_root / WIFI).read_text()
    code = recovery_harness(source_root / WIFI)
    code = code[:code.index('int main(){')]
    # Check retirement of both the guard flag and its real timestamp/name.
    actual = definition(text, 'static void wifi_guard_set_inflight(bool inflight)')
    code = code.replace(
        definition(code, 'static void wifi_guard_set_inflight(bool value)'),
        'static char wifi_connect_owner[32];\n' + actual)
    return code + TESTS


def run(source_root, out, compiler):
    out.mkdir(parents=True, exist_ok=True)
    cpp, exe = out / 'test.cpp', out / 'test'
    cpp.write_text(harness(source_root))
    command = [compiler, '-std=c++11', '-Wall', '-Wextra', '-pthread',
               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
               str(cpp), '-I', str(source_root / 'Sense_Minimal'), '-o', str(exe)]
    compiled = subprocess.run(command, capture_output=True, text=True,
                              timeout=30, preexec_fn=no_core)
    (out / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise RuntimeError(f'Compilation failed: {out / "compile.log"}')
    result = subprocess.run([str(exe)], capture_output=True, text=True,
                            timeout=10, preexec_fn=no_core)
    (out / 'run.log').write_text(result.stdout + result.stderr)
    print(f'{out.name}: {result.stdout.strip()}')
    return {'source_root': str(source_root), 'exit_code': result.returncode,
            'source_sha256': hashlib.sha256((source_root / WIFI).read_bytes()).hexdigest(),
            'output': result.stdout + result.stderr}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--negative-source-root', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('C++ compiler unavailable')
    with tempfile.TemporaryDirectory(prefix='halo-provisioning-owner-') as temp:
        out = args.out or Path(temp)
        current = run(args.source_root.resolve(), out / 'current', compiler)
        results = {'current': current, 'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
        passed = current['exit_code'] == 0
        if args.negative_source_root:
            negative = run(args.negative_source_root.resolve(), out / 'negative', compiler)
            results['negative'] = negative
            # A build error, signal or sanitizer failure is not the expected red.
            passed &= negative['exit_code'] == 1 and 'FAIL checks=1543 failures=' in negative['output']
        results['passed'] = passed
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Run the real shopping Wi-Fi admission with fake clock/connection boundaries.

--source accepts an older sense_list.h to reproduce the startup failure. No
Arduino build, network, device, or shared Wi-Fi implementation is changed.
"""
import argparse
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile


def definition(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def harness(source):
    text = source.read_text()
    fetch = text[text.index('static bool fetch_shopping_list_from_api()'):]
    # Stop exactly where network request construction begins. This retains the
    # real caller's early return, and also supports the pre-fix inline guard.
    preamble = fetch[fetch.index('{') + 1:
                     fetch.index('Serial.println("\\n=== Fetching Shopping List')]
    helper = (definition(text, 'static bool list_ensure_wifi_ready()')
              if 'static bool list_ensure_wifi_ready()' in text else '')
    budget = text[text.index('static const uint32_t LIST_FETCH_WIFI_BUDGET_MS'):]
    budget = budget[:budget.index(';') + 1]
    return r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#define portMUX_TYPE std::mutex
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
#define HALO_SENSE_PROD_WRAPPER 1
static constexpr int WL_CONNECTED=3, WL_DISCONNECTED=6;
static uint32_t now_ms;
static uint64_t elapsed, connect_at, fail_at, provisioning_at;
static bool wifi_connect_inflight, connected, provisioning;
static unsigned ensure_calls, begin_calls, setup_ms;
static uint32_t ensure_budget;
static bool start_allowed, claim_raced;
static std::string failure, output;
static void observe(){
  if(elapsed>=provisioning_at) provisioning=true;
  if(elapsed>=fail_at) wifi_connect_inflight=false;
  if(elapsed>=connect_at){connected=true;wifi_connect_inflight=false;}
}
static uint32_t millis(){return now_ms;}
static void delay(uint32_t ms){now_ms+=ms;elapsed+=ms;observe();}
static bool halo_provisioning_active(){observe();return provisioning;}
static struct {int status(){observe();return connected?WL_CONNECTED:WL_DISCONNECTED;}} WiFi;
static struct {
  template<class... A> void printf(const char* f,A... a){
    char b[256];std::snprintf(b,sizeof(b),f,a...);output+=b;
  }
  void println(const char* s){output+=s;output+='\n';}
} Serial;
static void list_refresh_fail(const char* reason){failure=reason;}
// Model the existing ensure boundary: an already-active connection returns
// false immediately; guarded initiation starts at most one connection. A
// competing owner can win the claim between the caller's observation and call.
static bool ensure_wifi_connected(const char* reason,uint32_t timeout){
  assert(!std::strcmp(reason,"list_fetch"));
  ++ensure_calls;ensure_budget=timeout;
  if(wifi_connect_inflight||!start_allowed) return false;
  wifi_connect_inflight=true;
  if(!claim_raced) ++begin_calls;
  delay(setup_ms);
  return false;
}
#include "sense_action_summary.h"
''' + budget + '\n' + helper + '\n' + (
        'static bool production_list_network_admission(){\n' + preamble +
        '\nreturn true;\n}\n') + r'''
static void reset(bool inflight=true){
  now_ms=1234;elapsed=0;
  connect_at=fail_at=provisioning_at=UINT64_MAX;
  wifi_connect_inflight=inflight;connected=false;provisioning=false;
  ensure_calls=begin_calls=0;setup_ms=50;ensure_budget=UINT32_MAX;
  start_allowed=true;claim_raced=false;failure.clear();output.clear();
}
static void bounded(){assert(elapsed<=LIST_FETCH_WIFI_BUDGET_MS);}
int main(){
  // Observed startup race: boot owns Wi-Fi; readiness arrives after the
  // refresh begins. The old code returns wifi_not_connected immediately.
  reset();connect_at=2000;
  bool ok=production_list_network_admission();
  if(!ok){
    std::fprintf(stderr,"startup connection rejected: elapsed=%llu reason=%s\n",
                 (unsigned long long)elapsed,failure.c_str());
    return 1;
  }
  assert(elapsed==2000&&ensure_calls==0&&begin_calls==0&&failure.empty());
  assert(output.find("wifi_connect_pending")!=std::string::npos);bounded();
  std::puts("PASS startup refresh joins existing connection without another begin");

  reset(false);connected=true;
  assert(production_list_network_admission()&&elapsed==0&&ensure_calls==0);
  reset(false);connect_at=1000;
  assert(production_list_network_admission());
  assert(ensure_calls==1&&begin_calls==1&&ensure_budget==0);bounded();
  reset(false);claim_raced=true;connect_at=1000;
  assert(production_list_network_admission());
  assert(ensure_calls==1&&begin_calls==0&&ensure_budget==0);bounded();
  // Existing cooldown/credentials refusal is not a consumed six-second wait.
  reset(false);start_allowed=false;
  assert(!production_list_network_admission()&&elapsed==0);
  assert(failure=="wifi_not_connected"&&ensure_calls==1&&begin_calls==0);
  std::puts("PASS connected, single guarded start, competing owner and refusal paths");

  reset();
  assert(!production_list_network_admission());
  assert(elapsed==LIST_FETCH_WIFI_BUDGET_MS&&failure=="wifi_connect_timeout");
  assert(wifi_connect_inflight&&ensure_calls==0&&begin_calls==0);
  reset();connect_at=LIST_FETCH_WIFI_BUDGET_MS;
  assert(production_list_network_admission());bounded();
  reset();connect_at=LIST_FETCH_WIFI_BUDGET_MS+1;
  assert(!production_list_network_admission()&&failure=="wifi_connect_timeout");bounded();
  reset(false);setup_ms=73;
  assert(!production_list_network_admission());
  assert(elapsed==LIST_FETCH_WIFI_BUDGET_MS&&begin_calls==1&&ensure_calls==1);
  // The same deadline survives millis() wrap; setup and final partial sleep
  // consume the original budget rather than opening a second wait window.
  reset();now_ms=UINT32_MAX-999;connect_at=2000;
  assert(production_list_network_admission()&&elapsed==2000);
  reset();now_ms=UINT32_MAX-999;
  assert(!production_list_network_admission()&&elapsed==LIST_FETCH_WIFI_BUDGET_MS);
  std::puts("PASS original deadline, late/exact readiness, setup cost and clock wrap");

  reset();fail_at=500;connect_at=1000;
  assert(!production_list_network_admission()&&elapsed==500);
  assert(failure=="wifi_not_connected"&&ensure_calls==0&&begin_calls==0);
  reset();provisioning=true;
  assert(!production_list_network_admission()&&elapsed==0&&ensure_calls==0);
  assert(failure=="wifi_provisioning_active"&&wifi_connect_inflight);
  reset();provisioning_at=500;connect_at=500;
  assert(!production_list_network_admission()&&elapsed==500);
  assert(failure=="wifi_provisioning_active"&&ensure_calls==0&&begin_calls==0);
  std::puts("PASS failed connection and provisioning stop without reconnect/reset");
}
'''


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] /
                        'Sense_Minimal/sense_list.h')
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('C++ compiler unavailable')
    with tempfile.TemporaryDirectory(prefix='halo-list-wifi-') as directory:
        source = Path(directory) / 'test.cpp'
        executable = Path(directory) / 'test'
        source.write_text(harness(args.source))
        subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra', str(source),
                        '-I', str(Path(__file__).resolve().parents[1] / 'Sense_Minimal'),
                        '-o', str(executable)], check=True, timeout=30,
                       preexec_fn=no_core)
        result = subprocess.run([str(executable)], timeout=5, preexec_fn=no_core)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()

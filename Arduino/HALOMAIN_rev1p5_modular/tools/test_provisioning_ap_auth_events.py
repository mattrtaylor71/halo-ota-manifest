#!/usr/bin/env python3
"""Exercise the actual raw AP-auth observer, registration and owner drain.

Host-only SDK/clock/critical-section doubles; no device, network or key material.
Checks include a concurrent callback/drain stream and source-pinned integration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "halo_ota_demo/firmware/shared/ProvisioningManager.cpp"

PREFIX = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
static unsigned checks=0,failures=0;
static void check(bool ok,const char* label){++checks;if(!ok){++failures;printf("FAIL %s\n",label);}}
using esp_event_base_t=const char*;
using esp_event_handler_instance_t=void*;
using esp_err_t=int;
static constexpr int ESP_OK=0;
static const char wifi_base[]="WIFI_EVENT", other_base[]="OTHER_EVENT";
static esp_event_base_t WIFI_EVENT=wifi_base;
enum {WIFI_EVENT_AP_WRONG_PASSWORD=45};
using Callback=void(*)(void*,esp_event_base_t,int32_t,void*);
static Callback installed=nullptr;
static int register_calls=0,register_result=0;
static bool emit_during_registration=false;
static thread_local bool in_callback=false;
static thread_local unsigned lock_depth=0;
static std::atomic<uint32_t> clock_ms{0};
static unsigned long millis(){return clock_ms.load();}
#define portMUX_TYPE std::mutex
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) do { (m)->lock(); ++lock_depth; } while(0)
#define portEXIT_CRITICAL(m) do { assert(lock_depth==1); --lock_depth; (m)->unlock(); } while(0)
static std::vector<std::string> output;
static struct {
  template<class... A> void printf(const char* format,A... args){
    assert(!in_callback&&!lock_depth);
    char buffer[512];snprintf(buffer,sizeof(buffer),format,args...);output.emplace_back(buffer);
  }
} Serial;
static esp_err_t esp_event_handler_instance_register(esp_event_base_t base,int32_t id,
    Callback callback,void* arg,esp_event_handler_instance_t* instance){
  assert(!in_callback&&!lock_depth);
  check(base==WIFI_EVENT&&id==WIFI_EVENT_AP_WRONG_PASSWORD,"register only exact AP auth event");
  check(arg==nullptr&&callback!=nullptr,"no manager or credential pointer in callback context");
  ++register_calls;
  *instance=reinterpret_cast<void*>(uintptr_t(7));
  if(register_result==ESP_OK)installed=callback;
  if(emit_during_registration){
    in_callback=true;callback(nullptr,base,id,reinterpret_cast<void*>(uintptr_t(1)));in_callback=false;
  }
  return register_result;
}
'''

TESTS = r'''
static bool contains(const char* s){for(const auto& line:output)if(line.find(s)!=std::string::npos)return true;return false;}
static void reset(){
  installed=nullptr;register_calls=0;register_result=0;emit_during_registration=false;
  clock_ms=0;output.clear();g_provision_ap_auth_observer=nullptr;
  g_provision_ap_auth_total=g_provision_ap_auth_pending=g_provision_ap_auth_last_ms=0;
}
static void event(esp_event_base_t base=WIFI_EVENT,int32_t id=WIFI_EVENT_AP_WRONG_PASSWORD){
  in_callback=true;
  onProvisionApWrongPassword(reinterpret_cast<void*>(uintptr_t(1)),base,id,
                            reinterpret_cast<void*>(uintptr_t(1)));
  in_callback=false;
  assert(!lock_depth);
}
int main(){
  reset();registerProvisionApAuthObserver();
  check(installed==onProvisionApWrongPassword&&register_calls==1,"SDK callback installed");
  check(contains("observer_registered=1 rc=0 event=45"),"registration receipt explicit");
  for(unsigned i=0;i<10;++i)registerProvisionApAuthObserver();
  check(register_calls==1,"repeated setup/restart cannot duplicate observer");
  reset();register_result=123;registerProvisionApAuthObserver();
  check(!g_provision_ap_auth_observer&&!installed,"failed registration is not latched");
  check(contains("observer_registered=0 rc=123"),"registration failure visible");
  register_result=0;registerProvisionApAuthObserver();
  check(register_calls==2&&installed,"later setup retries failed registration");
  reset();emit_during_registration=true;registerProvisionApAuthObserver();serviceProvisionApAuthEvents();
  check(contains("count=1 total=1 last_ms=0"),"callback safe even during initial registration at clock zero");
  reset();event(other_base);event(WIFI_EVENT,44);event(nullptr);event(WIFI_EVENT,-1);
  check(g_provision_ap_auth_total==0&&output.empty(),"wrong event/base ignored without logging");
  serviceProvisionApAuthEvents();check(output.empty(),"idle owner drain is silent");
  clock_ms=99;event();
  check(g_provision_ap_auth_total==1&&g_provision_ap_auth_pending==1&&output.empty(),
        "callback only records counters/time and never reads payload or logs");
  clock_ms=123;serviceProvisionApAuthEvents();
  check(contains("event=ap_wrong_password count=1 total=1 last_ms=99 serviced_ms=123"),"owner emits exact event evidence");
  auto lines=output.size();serviceProvisionApAuthEvents();
  check(output.size()==lines&&g_provision_ap_auth_pending==0,"drained batch cannot duplicate");
  reset();for(uint32_t i=0;i<1000;++i){clock_ms=i;event();}
  serviceProvisionApAuthEvents();
  check(output.size()==1&&contains("count=1000 total=1000 last_ms=999"),"bounded coalescing preserves complete count");
  reset();g_provision_ap_auth_total=g_provision_ap_auth_pending=UINT32_MAX-1;
  clock_ms=UINT32_MAX;event();clock_ms=0;event();
  check(g_provision_ap_auth_total==UINT32_MAX&&g_provision_ap_auth_pending==UINT32_MAX&&
        g_provision_ap_auth_last_ms==0,"counter saturation and timestamp wrap safe");
  serviceProvisionApAuthEvents();clock_ms=1;event();serviceProvisionApAuthEvents();
  check(contains("count=1 total=4294967295 last_ms=1"),"new batch still reported after total saturates");
  reset();std::atomic<bool> done{false};
  std::thread producer([&]{for(unsigned i=0;i<20000;++i){clock_ms=i;event();}done=true;});
  while(!done.load()){serviceProvisionApAuthEvents();std::this_thread::yield();}
  producer.join();serviceProvisionApAuthEvents();
  uint64_t counted=0;uint32_t last_total=0;
  for(const auto& line:output){
    unsigned long batch=0,total=0,at=0,serviced=0;
    int parsed=sscanf(line.c_str(),"[PROVISION_AUTH] event=ap_wrong_password count=%lu total=%lu last_ms=%lu serviced_ms=%lu",
                      &batch,&total,&at,&serviced);
    check(parsed==4&&batch>0&&total>=last_total,"concurrent drain emits valid monotonic batches");
    counted+=batch;last_total=(uint32_t)total;
  }
  check(counted==20000&&g_provision_ap_auth_total==20000&&g_provision_ap_auth_pending==0,
        "concurrent SDK callbacks/owner drain have no loss or duplicates");
  printf("%s raw AP auth observer: %u checks, %u failures; 20000 concurrent events\n",
         failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root',type=Path,default=ROOT)
    p.add_argument('--out',type=Path)
    args=p.parse_args()
    root=args.source_root.resolve()
    text=(root/SOURCE).read_text()
    start=text.index('static esp_event_handler_instance_t g_provision_ap_auth_observer')
    end=text.index('static bool g_scan_inflight',start)
    actual=text[start:end]
    startup=definition(text,'bool ProvisioningManager::startSoftAP()')
    assert startup.index('WiFi.mode(WIFI_AP_STA)') < startup.index('registerProvisionApAuthObserver();')
    assert startup.index('registerProvisionApAuthObserver();') < startup.index('WiFi.softAP(ap_ssid, ap_password)')
    update=definition(text,'void ProvisioningManager::update()')
    assert update.index('serviceProvisionApAuthEvents();') < update.index('if (!setup_mode_active)')
    assert update.index('serviceProvisionApAuthEvents();') < update.index('server->handleClient();')
    assert 'esp_event_handler_instance_unregister' not in text
    callback=definition(text,'static void onProvisionApWrongPassword(')
    for forbidden in ('Serial.', 'LOG_', 'WiFi.', 'delay(', 'esp_wifi_', 'malloc(', 'new '):
        assert forbidden not in callback,forbidden
    harness='\n'.join([PREFIX,actual,TESTS])
    compiler=shutil.which('clang++') or shutil.which('g++')
    with tempfile.TemporaryDirectory(prefix='halo-provision-auth-') as tmp:
        cpp,binary=Path(tmp)/'test.cpp',Path(tmp)/'test'
        cpp.write_text(harness)
        build=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                              '-fsanitize=address,undefined',str(cpp),'-o',str(binary)],
                             capture_output=True,text=True,timeout=30)
        run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10) if build.returncode==0 else None
    result={'status':'PASS' if run and run.returncode==0 else 'FAIL',
            'scope':'Actual raw SDK observer, registration and owner drain with mocked registration/time/critical section; 20000 concurrent events. Startup/owner wiring pinned. No hardware.',
            'source_root':str(root),'source_sha256':{SOURCE:hashlib.sha256(text.encode()).hexdigest()},
            'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'harness_sha256':hashlib.sha256(harness.encode()).hexdigest(),
            'build_returncode':build.returncode,'run_returncode':run.returncode if run else None,
            'output':build.stdout+build.stderr+(run.stdout+run.stderr if run else '')}
    if args.out:
        args.out.mkdir(parents=True,exist_ok=True)
        (args.out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
        (args.out/'run.log').write_text(result['output'])
        (args.out/'harness.cpp').write_text(harness)
    print(result['output'],end='')
    return 0 if result['status']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())

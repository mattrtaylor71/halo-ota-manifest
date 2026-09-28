#!/usr/bin/env python3
"""Execute setup scan-cache teardown with the installed Arduino3.3.8 String.

Uses the real SDK header and allocator/copy/move definitions, instrumenting only
malloc ownership. Radio, owner state, clock and heap queries are host fakes. This
proves backing-store release and teardown ordering, not target heap recovery.
"""
import argparse
import hashlib
import json
from pathlib import Path
from host_paths import arduino_data
import shutil
import subprocess
from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]
MANAGER = "halo_ota_demo/firmware/shared/ProvisioningManager.cpp"
SDK = arduino_data() / 'packages/esp32/hardware/esp32/3.3.8/cores/esp32'

PREFIX = r'''
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "WString.h"
static unsigned checks=0,failures=0;
static void check(bool ok,const char* name){
 ++checks;if(!ok){++failures;printf("FAIL %s\n",name);}
}
static std::map<void*,size_t> allocations;
static std::vector<std::string> events;
static size_t allocated(){size_t n=0;for(const auto& p:allocations)n+=p.second;return n;}
static void* sdk_realloc(void* old,size_t n){
 auto it=allocations.find(old);
 void* p=std::realloc(old,n);
 if(p){if(it!=allocations.end())allocations.erase(it);allocations[p]=n;}
 return p;
}
static void sdk_free(void* p){
 check(allocations.erase(p)==1,"SDK frees an owned allocation exactly once");
 events.push_back("cache_free");std::free(p);
}
#define realloc sdk_realloc
#define free sdk_free
'''

STUBS = r'''
#undef realloc
#undef free
static String g_scan_cached_response;
static bool g_scan_inflight=false,rebooting=false,claim_busy=false,owner_valid=true;
static unsigned long g_scan_started_ms=0,g_scan_cached_ms=0;
static uint32_t now_ms=20000;
static int scan_status=0;
static constexpr int MALLOC_CAP_DMA=1,MALLOC_CAP_INTERNAL=2;
static size_t heap_caps_get_free_size(uint32_t caps){check(caps==3,"DMA internal query");return 32768-allocated();}
static size_t heap_caps_get_largest_free_block(uint32_t caps){return heap_caps_get_free_size(caps);}
static void log_info(const char* format,...){events.push_back(std::string(format).find("scan_cache_released")!=std::string::npos?"cache_log":"log");}
#define LOG_INFO(...) log_info(__VA_ARGS__)
using wifi_mode_t=int;using wl_status_t=int;
static constexpr int WIFI_AP_STA=3,WIFI_STA=1,WL_CONNECTED=3,WIFI_SCAN_RUNNING=-1;
static struct Radio {
 int getMode(){return WIFI_AP_STA;}int status(){return WL_CONNECTED;}
 int scanComplete(){return scan_status;}
 void scanDelete(){events.push_back("scan_delete");}
 void mode(int){events.push_back("mode");}
} WiFi;
static struct Dns {void stop(){events.push_back("dns_stop");}} *dns_server=nullptr;
static unsigned long millis(){return now_ms;}
static bool halo_rebooting(){return rebooting;}
static void delay(unsigned ms){now_ms+=ms;events.push_back("delay");}
static void dump_system_truth(const char*){events.push_back("truth");}
static void halo_provisioning_dma_reserve(bool active){
 check(!active,"teardown restores ordinary reserve admission");
 check(allocated()==0,"scan backing freed before reserve restore");events.push_back("reserve");
}
namespace ProvisioningState {
 enum State{STATE_CONNECTED,STATE_AP_SETUP};static State state=STATE_CONNECTED;
 static State getState(){return state;}
 static bool loadOwnerId(char* out,size_t n){snprintf(out,n,"%s",owner_valid?"owner":"");return owner_valid;}
}
class ProvisioningManager {public:
 bool setup_mode_active=true,claim_in_progress=false;
 unsigned long connected_verified_ms=1000,connected_state_set_ms=1000,owner_id_set_ms=1000;
 static const unsigned long MIN_CONNECTED_DELAY_MS=1500,OWNER_SUCCESS_GRACE_MS=15000;
 bool finishCompletedSetup();void stopSetupMode();
 bool claimTransportBusy()const{return claim_busy;}
 void stopHttpServer(){events.push_back("http_stop");}
 void stopSoftAP(){events.push_back("ap_stop");}
} manager;
'''

TESTS = r'''
static size_t at(const char* event){
 auto it=std::find(events.begin(),events.end(),event);return it==events.end()?events.size():size_t(it-events.begin());
}
static void reset(){
 g_scan_cached_response=static_cast<const char*>(nullptr);
 delete dns_server;dns_server=new Dns;
 manager={};claim_busy=rebooting=g_scan_inflight=false;owner_valid=true;
 g_scan_started_ms=g_scan_cached_ms=0;scan_status=0;now_ms=20000;
 ProvisioningState::state=ProvisioningState::STATE_CONNECTED;
 events.clear();g_scan_cached_response=std::string(1800,'x').c_str();
 check(allocated()>1800,"real SDK allocated a non-SSO scan response");
 events.clear();
}
int main(){
 reset();const size_t held=allocated();
 g_scan_cached_response="";
 check(allocated()==held,"empty literal retains real SDK capacity");
 g_scan_cached_response=String();
 check(allocated()==held,"empty temporary retains real SDK capacity");
 g_scan_cached_response.clear();
 check(allocated()==held,"clear retains real SDK capacity");
 g_scan_cached_response=static_cast<const char*>(nullptr);
 check(allocated()==0,"null C string frees real SDK backing");
 reset();clearProvisionScanCache();
 check(!allocated()&&!g_scan_cached_response.length(),"cache invalidation releases actual SDK backing");
 const unsigned before=events.size();clearProvisionScanCache();
 check(!allocated()&&events.size()==before+1,"repeat invalidation only deletes scan results");
 // Completed setup owns all cleanup, including caches with no in-flight scan.
 for(int state:{0,3,-2}){
  reset();g_scan_inflight=state!=0;scan_status=state;
  g_scan_started_ms=1200;g_scan_cached_ms=1500;
  check(manager.finishCompletedSetup(),"verified setup can finish");
  check(!allocated()&&!g_scan_inflight&&!g_scan_started_ms&&!g_scan_cached_ms,"successful teardown clears all scan storage and state");
  check(at("http_stop")<at("cache_free")&&at("cache_free")<at("dns_stop")&&
        at("dns_stop")<at("ap_stop")&&at("ap_stop")<at("reserve"),"HTTP closes before cache release and reserve follows AP stop");
  check(std::count(events.begin(),events.end(),"scan_delete")==1,"successful teardown retires scan exactly once");
  check(std::count(events.begin(),events.end(),"cache_log")==1,"bounded cleanup measurement once");
  events.clear();manager.stopSetupMode();check(events.empty(),"already-stopped setup is a no-op");
 }
 reset();claim_busy=true;manager.stopSetupMode();
 check(allocated()>0&&events.empty()&&manager.setup_mode_active,"live claim keeps transports and cache untouched");
 reset();manager.setup_mode_active=false;manager.stopSetupMode();
 check(allocated()>0&&events.empty(),"inactive setup does not release another lifecycle's storage");
 reset();g_scan_inflight=true;scan_status=WIFI_SCAN_RUNNING;
 check(!manager.finishCompletedSetup()&&allocated()>0&&events.empty(),"running scan keeps setup and cache intact");
 reset();now_ms=15999;
 check(!manager.finishCompletedSetup()&&allocated()>0&&events.empty(),"owner success grace keeps response available");
 reset();owner_valid=false;
 check(!manager.finishCompletedSetup()&&allocated()>0&&events.empty(),"unclaimed setup is preserved");
 reset();claim_busy=true;
 check(!manager.finishCompletedSetup()&&allocated()>0&&events.empty(),"claim worker is not interrupted");
 g_scan_cached_response=static_cast<const char*>(nullptr);
 delete dns_server;dns_server=nullptr;
 check(allocations.empty(),"all backing allocations released");
 printf("%s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);
 return failures?1:0;
}
'''


def sdk_string_source(sdk):
    source = (sdk / "WString.cpp").read_text()
    signatures = [
        "String::String(const char *cstr)", "String::~String()",
        "inline void String::init(", "void String::invalidate(",
        "bool String::reserve(", "bool String::changeBuffer(",
        "String &String::copy(const char *cstr, unsigned int length)",
        "void String::move(String &rhs)", "String &String::operator=(String &&rval)",
        "String &String::operator=(const char *cstr)",
    ]
    return "\n".join(definition(source, name) for name in signatures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--sdk-root", type=Path, default=SDK)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "pgmspace.h").write_text("#pragma once\n#define PSTR(s) (s)\n")
    manager = (args.source_root / MANAGER).read_text()
    clear = definition(manager, "static void clearProvisionScanCache()")
    stop = definition(manager, "void ProvisioningManager::stopSetupMode()")
    finish = definition(manager, "bool ProvisioningManager::finishCompletedSetup()")
    base = PREFIX + sdk_string_source(args.sdk_root) + STUBS
    variants = [
        ("fixed", clear, stop),
        ("retained_empty_string", clear.replace("static_cast<const char*>(nullptr)", '""'), stop),
        ("missing_teardown_release", clear, stop.replace("  clearProvisionScanCache();", "")),
    ]
    compiler = shutil.which("clang++") or shutil.which("c++")
    if not compiler:
        parser.error("Native C++ compiler required")
    outcomes = []
    for name, clear_code, stop_code in variants:
        cpp, binary = args.out / (name + ".cpp"), args.out / name
        cpp.write_text(base + clear_code + stop_code + finish + TESTS)
        cmd = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-function",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I", str(args.out), "-I", str(args.sdk_root), str(cpp), "-o", str(binary)]
        build = subprocess.run(cmd, capture_output=True, text=True, timeout=40)
        (args.out / (name + "-compile.log")).write_text(build.stdout + build.stderr)
        if build.returncode:
            raise RuntimeError("Compile failed: " + str(args.out / (name + "-compile.log")))
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        output = run.stdout + run.stderr
        (args.out / (name + ".log")).write_text(output)
        outcomes.append({"name": name, "returncode": run.returncode, "output": output})
    passed = (outcomes[0]["returncode"] == 0 and
              all(item["returncode"] == 1 and "FAIL scan backing freed before reserve restore" in item["output"]
                  for item in outcomes[1:]))
    inputs = {MANAGER: args.source_root / MANAGER,
              "sdk/WString.h": args.sdk_root / "WString.h", "sdk/WString.cpp": args.sdk_root / "WString.cpp"}
    result = {"status": "PASS" if passed else "FAIL", "scope": __doc__,
              "inputs": {name: {"path": str(path.resolve()), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                         for name, path in inputs.items()}, "results": outcomes,
              "limits": "Actual SDK String on host ABI; heap topology, Wi-Fi teardown and camera/AES allocation outcomes require target validation."}
    (args.out / "RESULT.json").write_text(json.dumps(result, indent=2) + "\n")
    print(outcomes[0]["output"], end="")
    print(("PASS" if passed else "FAIL") + " actual SDK scan-cache release with two negative controls")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Execute production reserve helpers and setup lifecycle against faulted heap/radio boundaries."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]
SENSE = 'Sense_Minimal/Sense_Minimal.ino'
WRAPPER = 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino'
MANAGER = 'halo_ota_demo/firmware/shared/ProvisioningManager.cpp'
PREFIX = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <functional>
#include <string>
static unsigned checks=0,failures=0;
static void check(bool v,const char* s){++checks;if(!v){++failures;if(failures<20)printf("FAIL %s\n",s);}}
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static thread_local int critical=0;
static void portENTER_CRITICAL(portMUX_TYPE* m){m->lock();++critical;}
static void portEXIT_CRITICAL(portMUX_TYPE* m){--critical;m->unlock();}
static std::atomic<uint8_t*> g_camera_dma_reserve{nullptr};
static portMUX_TYPE g_camera_dma_reserve_mux;
static std::atomic<bool> g_camera_dma_provisioning{false},g_dma_reserve_suppressed{false};
static constexpr size_t CAMERA_DMA_RESERVE_BYTES=16384;
static constexpr int MALLOC_CAP_DMA=1,MALLOC_CAP_INTERNAL=2;
static std::set<void*> live;
static unsigned allocations=0,frees=0,delays=0,fail_allocations=0;
static std::function<void()> allocation_hook;
static void* heap_caps_malloc(size_t bytes,int caps){
 check(!critical,"malloc outside critical");check(bytes==16384&&caps==3,"exact internal DMA reserve");
 ++allocations;if(fail_allocations){--fail_allocations;return nullptr;}
 void* p=malloc(bytes);live.insert(p);
 if(allocation_hook){auto hook=allocation_hook;allocation_hook=nullptr;hook();}
 return p;
}
static void heap_caps_free(void* p){check(!critical,"free outside critical");check(live.erase(p)==1,"no double/free foreign");++frees;free(p);}
static size_t heap_caps_get_free_size(int){check(!critical,"heap query outside critical");return 65536-live.size()*16384;}
static size_t heap_caps_get_largest_free_block(int){check(!critical,"heap query outside critical");return 32768-live.size()*16384;}
static void delay(unsigned ms){check(!critical,"delay outside critical");delays+=ms;}
static std::string output;
static struct {template<class... A> void printf(const char* fmt,A... a){
 check(!critical,"log outside critical");char b[1024];snprintf(b,sizeof b,fmt,a...);output+=b;
}} Serial;
#define LOG_INFO(...) do{}while(0)
#define LOG_WARN(...) do{}while(0)
#define LOG_ERROR(...) do{}while(0)
using wifi_mode_t=int;using wl_status_t=int;using esp_err_t=int;
static constexpr int WIFI_MODE_NULL=0,WIFI_MODE_AP=2,ESP_OK=0,ESP_ERR_WIFI_NOT_INIT=1;
static constexpr int WIFI_AP=2,WIFI_AP_STA=3,WIFI_STA=1,WL_CONNECTED=3,DNS_PORT=53;
static int radio_mode=1,radio_status=6,mode_read_error=ESP_OK;
static esp_err_t esp_wifi_get_mode(wifi_mode_t* mode){*mode=radio_mode;return mode_read_error;}
static struct {int getMode(){return radio_mode;}int status(){return radio_status;}
 void mode(int m){radio_mode=m;}int softAPIP(){return 1;}} WiFi;
namespace DNSReplyCode {static constexpr int NoError=0;}
struct DNSServer {void setErrorReplyCode(int){}bool start(int,const char*,int){return true;}void stop(){}};
static DNSServer* dns_server=nullptr;
namespace ProvisioningState {
 enum State {STATE_UNPROVISIONED,STATE_AP_SETUP};
 static State state=STATE_UNPROVISIONED;
 static void generateApSsid(char* p,size_t){strcpy(p,"ap");}
 static void generateRandomPassword(char* p,size_t){strcpy(p,"secret");}
 static void saveApCreds(const char*,const char*){}
 static void setState(State s){state=s;}
}
static void clearProvisionScanCache(){}
static void primeProvisionScanCache(){check(g_camera_dma_provisioning&&!g_camera_dma_reserve,"reserve released before scan");}
static void dump_system_truth(const char*){}
static bool ap_start_ok=true,http_start_ok=true,ap_stop_ok=true;
static int start_mode=3;
class ProvisioningManager {public:
 bool setup_mode_active=false;char ap_ssid[64]={},ap_password[16]={},target_home_ssid[64]={};
 unsigned sta_failure_count=0,last_app_request_ms=0,connected_state_set_ms=0;
 bool startSetupMode();void stopSetupMode();
 bool startSoftAP(){check(g_camera_dma_provisioning&&!g_camera_dma_reserve,"reserve released before AP");radio_mode=start_mode;return ap_start_ok;}
 bool startHttpServer(){return http_start_ok;}
 void stopHttpServer(){}
 void stopSoftAP(){check(g_camera_dma_provisioning&&!g_camera_dma_reserve,"no early reacquire before AP teardown");if(ap_stop_ok)radio_mode=1;}
 void setLastError(const char*){}
 void sendProvisionStatus(const char*){}
} manager;
'''
TESTS = r'''
static void reset(){
 allocation_hook=nullptr;camera_dma_reserve_release("reset");
 check(live.empty(),"no leaked candidates");g_camera_dma_provisioning=false;g_dma_reserve_suppressed=false;
 allocations=frees=delays=fail_allocations=0;output.clear();
 delete dns_server;dns_server=nullptr;manager={};radio_mode=1;radio_status=6;
 ap_start_ok=http_start_ok=ap_stop_ok=true;start_mode=3;mode_read_error=ESP_OK;
}
int main(){
 // Cold boot: actual setup entry precedes ordinary reservation admission.
 reset();check(manager.startSetupMode(),"cold setup starts");
 check(!camera_dma_reserve_acquire("setup")&&!g_camera_dma_reserve&&allocations==0,"cold setup never banks DMA");
 for(int i=0;i<4;++i){halo_tls_free_dma_reserve();halo_tls_restore_dma_reserve();}
 check(!g_camera_dma_reserve&&allocations==0,"nested/repeated TLS restores cannot rehold during AP");
 check(!camera_dma_reserve_acquire("camera_deinit"),"camera deinit cannot rehold during AP");
 check(output.find("provisioning=1 held=0 free_before=")!=std::string::npos,"memory proof logged");
 manager.stopSetupMode();check(!g_camera_dma_provisioning&&g_camera_dma_reserve&&allocations==1,"exit reacquires after AP down");
 auto ptr=g_camera_dma_reserve.load();check(camera_dma_reserve_acquire("normal")&&ptr==g_camera_dma_reserve,"normal acquire idempotent");
 camera_dma_reserve_release("camera_init");check(!g_camera_dma_reserve,"actual camera can consume reserve");
 check(camera_dma_reserve_acquire("camera_deinit"),"normal camera deinit restores reserve");
 // Warm entry returns an existing block before scan/start; duplicate entry does not toggle ownership.
 reset();camera_dma_reserve_acquire("normal");check(manager.startSetupMode(),"warm setup starts");
 check(frees==1&&!g_camera_dma_reserve&&g_camera_dma_provisioning,"warm releases existing block");
 auto count=allocations;manager.startSetupMode();check(allocations==count&&!g_camera_dma_reserve,"active setup idempotent");
 // Failed starts: restoration only when public driver mode proves AP absent.
 for(bool live_ap:{false,true}){
  reset();camera_dma_reserve_acquire("normal");ap_start_ok=false;start_mode=live_ap?3:1;
  check(!manager.startSetupMode(),"AP start failure returned");
  check(bool(g_camera_dma_provisioning)==live_ap&&bool(g_camera_dma_reserve)==!live_ap,"failed start honors actual AP mode");
 }
 for(bool stop_ok:{false,true}){
  reset();camera_dma_reserve_acquire("normal");http_start_ok=false;ap_stop_ok=stop_ok;
  check(!manager.startSetupMode(),"HTTP start failure returned");
  check(bool(g_camera_dma_provisioning)==!stop_ok&&bool(g_camera_dma_reserve)==stop_ok,"HTTP failure honors actual AP teardown");
 }
 reset();manager.startSetupMode();ap_stop_ok=false;radio_status=WL_CONNECTED;manager.stopSetupMode();
 check(g_camera_dma_provisioning&&!g_camera_dma_reserve,"failed connected-AP teardown stays suppressed");
 // Unknown SDK readback cannot claim AP is down; uninitialized driver can.
 reset();manager.startSetupMode();mode_read_error=123;manager.stopSetupMode();
 check(g_camera_dma_provisioning&&!g_camera_dma_reserve,"unknown AP readback fails closed");
 reset();manager.startSetupMode();mode_read_error=ESP_ERR_WIFI_NOT_INIT;manager.stopSetupMode();
 check(!g_camera_dma_provisioning&&g_camera_dma_reserve,"uninitialized Wi-Fi cannot retain an AP");
 // OTA remains authoritative on setup exit and TLS restoration.
 reset();camera_dma_reserve_acquire("normal");camera_dma_reserve_suppress_for_ota();
 check(!g_camera_dma_reserve&&g_dma_reserve_suppressed,"OTA releases and suppresses");
 manager.startSetupMode();manager.stopSetupMode();halo_tls_restore_dma_reserve();
 check(!g_camera_dma_reserve,"setup/TLS exit cannot override OTA");
 g_dma_reserve_suppressed=false;check(camera_dma_reserve_acquire("camera_deinit"),"OTA exit permits later ordinary restore");
 // Deterministic interleavings at the real allocation/publication boundary.
 reset();allocation_hook=[](){manager.startSetupMode();};
 check(!camera_dma_reserve_acquire("racing_tls"),"setup rejects in-progress allocation");
 check(!g_camera_dma_reserve&&live.empty(),"racing candidate freed without publication");
 reset();allocation_hook=[](){camera_dma_reserve_suppress_for_ota();};
 check(!camera_dma_reserve_acquire("racing_tls")&&!g_camera_dma_reserve&&live.empty(),"OTA rejects in-progress allocation");
 reset();allocation_hook=[](){check(camera_dma_reserve_acquire("other_task"),"second task acquires");};
 check(camera_dma_reserve_acquire("first_task")&&live.size()==1&&frees==1,"competing acquire retains one allocation");
 // Existing bounded retries/failure and zero-allocation suppression.
 reset();fail_allocations=2;check(camera_dma_reserve_acquire("retry")&&allocations==3&&delays==40,"original bounded retry succeeds");
 reset();fail_allocations=3;check(!camera_dma_reserve_acquire("oom")&&allocations==3&&delays==40&&!g_camera_dma_reserve,"original bounded OOM preserved");
 reset();manager.startSetupMode();fail_allocations=3;manager.stopSetupMode();
 check(!g_camera_dma_provisioning&&!g_camera_dma_reserve&&allocations==3,"failed exit restore remains recoverable");
 check(camera_dma_reserve_acquire("camera_deinit"),"later camera lifecycle can retry restore");
 reset();delete dns_server;dns_server=nullptr;
 printf("%s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
'''

def harness(root, negative):
    s=(root/SENSE).read_text();w=(root/WRAPPER).read_text();m=(root/MANAGER).read_text()
    names=['static void camera_dma_reserve_release(', 'static bool camera_dma_reserve_acquire(',
           'static void camera_dma_reserve_provisioning(', 'static void camera_dma_reserve_suppress_for_ota(']
    functions=[definition(s,n) for n in names]
    if negative:
        old=(negative/SENSE).read_text()
        functions[1]=definition(old,names[1]).replace('g_camera_dma_reserve, attempt','g_camera_dma_reserve.load(), attempt')
    assert 'static std::atomic<uint8_t*> g_camera_dma_reserve{nullptr};' in s
    assert 'static std::atomic<bool> g_camera_dma_provisioning{false};' in s
    assert 'static std::atomic<bool> g_dma_reserve_suppressed{false};' in s
    setup=definition(s,'void setup()')
    assert 'camera_dma_reserve_acquire("setup")' in setup
    assert 'g_camera_dma_reserve = ' not in setup
    assert 'heap_caps_free(g_camera_dma_reserve)' not in w
    assert 'camera_dma_reserve_suppress_for_ota();' in w
    code=PREFIX+'\n'.join(functions)
    for name in ['extern "C" void halo_tls_free_dma_reserve(', 'extern "C" void halo_tls_restore_dma_reserve(', 'extern "C" void halo_provisioning_dma_reserve(']: code+='\n'+definition(w,name)
    for name in ['bool ProvisioningManager::startSetupMode()', 'void ProvisioningManager::stopSetupMode()']:code+='\n'+definition(m,name)
    return code+TESTS

def run(root,out,negative=None):
    out.mkdir(parents=True,exist_ok=True);cpp=out/'test.cpp';exe=out/'test'
    cpp.write_text(harness(root,negative))
    cmd=[shutil.which('clang++') or 'c++','-std=c++11','-pthread','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(exe)]
    c=subprocess.run(cmd,text=True,capture_output=True,timeout=30);(out/'compile.log').write_text(c.stdout+c.stderr)
    if c.returncode:raise RuntimeError('compile failed: '+str(out/'compile.log'))
    r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=15);(out/'run.log').write_text(r.stdout+r.stderr)
    if (r.returncode!=0)!=bool(negative):raise RuntimeError('unexpected outcome: '+str(out/'run.log'))
    return {'status':'EXPECTED_FAILURE' if negative else 'PASS','returncode':r.returncode,'output':r.stdout.strip()}

def main():
    p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,default=ROOT);p.add_argument('--out',type=Path,required=True);p.add_argument('--negative-source-root',type=Path);a=p.parse_args()
    report={'current':run(a.source_root,a.out/'current'),'sources':{x:hashlib.sha256((a.source_root/x).read_bytes()).hexdigest() for x in [SENSE,WRAPPER,MANAGER]}}
    if a.negative_source_root:report['negative']=run(a.source_root,a.out/'negative',a.negative_source_root)
    (a.out/'RESULT.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()

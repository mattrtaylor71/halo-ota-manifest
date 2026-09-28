#!/usr/bin/env python3
"""Execute actual claim mailbox, transport and owner functions with faulted I/O.

No network/device access. SDK doubles exercise timeout settings, cancellation,
bounded bodies and cleanup; they do not prove the SDK's DNS wall-clock bound.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]
SHARED = 'halo_ota_demo/firmware/shared/'
FILES = [SHARED+x for x in ['ProvisioningClaimJob.h', 'ProvisioningClaimTransport.h',
         'ProvisioningManager.cpp', 'ProvisioningManager.h', 'ScopedTlsMemory.h']]+[
         'Sense_Minimal/sense_memory_diag.h',
         'Sense_Minimal/Sense_Minimal.ino', 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino']

STUB = r'''
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <string>
#include <functional>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <cstdarg>
using String=std::string;
static std::atomic<uint32_t> clock_ms{1};
static uint32_t millis(){return clock_ms.load();}
static unsigned checks=0,failures=0;
static void check(bool b,const char* text){++checks;if(!b){++failures;printf("FAIL %s\n",text);}}
static unsigned ssl_objects=0,http_objects=0;
static bool tls_connected=false,in_failure_hook=false,destructor_fault=false;
static std::string diagnostic_output;
using FailedAlloc=void(*)(size_t,uint32_t,const char*);
static FailedAlloc allocation_hook=nullptr;
static void allocation_failure(size_t bytes,uint32_t caps,const char* fn){
 in_failure_hook=true;if(allocation_hook)allocation_hook(bytes,caps,fn);in_failure_hook=false;
}
static constexpr int ESP_OK=0,MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_DMA=4,MALLOC_CAP_SPIRAM=8;
static int heap_caps_register_failed_alloc_callback(FailedAlloc hook){allocation_hook=hook;return ESP_OK;}
static size_t heap_caps_get_free_size(int){check(!in_failure_hook,"failure hook never queries heap");return 30000-256*ssl_objects-128*http_objects-(tls_connected?10000:0);}
static size_t heap_caps_get_largest_free_block(int){check(!in_failure_hook,"failure hook never queries largest block");return tls_connected?5000:12000;}
static void* heap_caps_calloc(size_t n,size_t size,int caps){check(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),"TLS scope requests only external8bit");return std::calloc(n,size);}
static void heap_caps_free(void* p){std::free(p);}
using TaskHandle_t=void*;
static TaskHandle_t xTaskGetCurrentTaskHandle(){static thread_local int task;return &task;}
static uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t target){
 if(target||in_failure_hook||ssl_objects||http_objects||tls_connected)std::abort();return 4321;
}
static struct {
 void printf(const char* fmt,...){
  check(!in_failure_hook&&!ssl_objects&&!http_objects&&!tls_connected,"diagnostic prints after both client destructors");
  char text[700];va_list args;va_start(args,fmt);std::vsnprintf(text,sizeof text,fmt,args);va_end(args);diagnostic_output+=text;
 }
} Serial;
static int handshake_s=0,connect_ms=0,read_ms=0,stop_calls=0,post_calls=0,body_reads=0;
static int response_code=200,content_size=-1;
static bool begin_ok=true,connect_ok=true,ca_set=false,insecure_set=false;
static String response_body="{\"owner_id\":\"new-owner\"}";
static std::function<void()> connect_hook,post_hook,read_hook;
class Stream {public:
 virtual ~Stream(){};virtual size_t write(uint8_t)=0;virtual size_t write(const uint8_t*,size_t)=0;
 virtual int read()=0;virtual int available()=0;virtual int peek()=0;virtual void flush(){}
 void setWriteError(){write_error=true;}void setTimeout(unsigned v){timeout=v;}
 bool write_error=false;unsigned timeout=3000;
};
class WiFiClientSecure:public Stream {public:
 WiFiClientSecure(){++ssl_objects;}
 virtual ~WiFiClientSecure(){if(destructor_fault)allocation_failure(24,MALLOC_CAP_DMA,"destructor_fixture");--ssl_objects;}
 virtual int connect(const char*,uint16_t,int32_t v){connect_ms=v;tls_connected=true;if(connect_hook)connect_hook();return connect_ok?1:0;}
 virtual uint8_t connected(){return 1;}
 virtual int read(uint8_t*,size_t){if(read_hook)read_hook();return 1;}
 int read()override{if(read_hook)read_hook();return 1;}
 int available()override{return 1;}int peek()override{return 1;}
 size_t write(uint8_t)override{return 1;}
 size_t write(const uint8_t*,size_t n)override{return n;}
 void setHandshakeTimeout(unsigned v){handshake_s=v;}
 void setCACert(const char*){ca_set=true;}void setInsecure(){insecure_set=true;}
 void stop(){++stop_calls;tls_connected=false;}
};
class HTTPClient {public:
 HTTPClient(){++http_objects;}
 ~HTTPClient(){--http_objects;}
 bool begin(WiFiClientSecure& c,const char*){client=&c;return begin_ok;}
 void setConnectTimeout(int n){ct=n;}void setTimeout(unsigned n){read_ms=n;}
 void setReuse(bool b){reuse=b;}void addHeader(const char*,const char*){}
 int POST(String b){
  ++post_calls;posted=b;
  if(!client->connect("claim.invalid",443,ct))return -1;
  if(!client->write(reinterpret_cast<const uint8_t*>(b.data()),b.size()))return -1;
  if(post_hook)post_hook();return response_code;
 }
 int getSize(){return content_size;}
 int writeToStream(Stream* sink){
  ++body_reads;size_t sent=0;
  while(sent<response_body.size()){
   if(!client->connected())return -1;
   uint8_t scratch=0;if(client->read(&scratch,1)<0)return -1;
   size_t n=std::min(size_t(73),response_body.size()-sent);
   if(sink->write(reinterpret_cast<const uint8_t*>(response_body.data()+sent),n)!=n)return -1;
   sent+=n;
  }
  return int(sent);
 }
 void end(){if(client)client->stop();}
 WiFiClientSecure* client=nullptr;int ct=0;bool reuse=true;String posted;
};
'''

PREFIX = r'''
#include "HTTPClient.h"
#define HALO_SCOPED_TLS_MEMORY_TEST 1
#define HALO_MEMORY_DIAGNOSTICS_TEST 1
#include "ScopedTlsMemory.h"
#include "ProvisioningClaimJob.h"
#include "ProvisioningClaimTransport.h"
#include <ArduinoJson.h>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define HTTP_CODE_OK 200
#define OTA_TLS_INSECURE_DEBUG 0
#define PROVISIONING_CLAIM_BASE_URL "https://claim.invalid"
static const char* kProvisioningClaimPath="/v1/provisioning/claim";
static const char* kFirmwareVersion="test";
static const char* kAmazonRootCa1="test-ca";
static const std::thread::id owner_thread=std::this_thread::get_id();
static std::atomic<unsigned> wrong_thread_access{0},ntp_active{0},dma_active{0};
static void owner_only(){if(std::this_thread::get_id()!=owner_thread)++wrong_thread_access;}
static bool sntp_pending=false;
static bool halo_sntp_sync_pending(){return sntp_pending;}
struct HaloNtpDnsGuard {HaloNtpDnsGuard(){++ntp_active;}~HaloNtpDnsGuard(){--ntp_active;}};
extern "C" void halo_tls_free_dma_reserve(){++dma_active;}
extern "C" void halo_tls_restore_dma_reserve(){--dma_active;}
namespace ProvisioningState {
 enum State {STATE_CONNECTED,STATE_CONNECTING_HOME_WIFI,STATE_AP_SETUP,STATE_UNPROVISIONED};
 static State state=STATE_CONNECTED;
 static String owner,code="CODE123",timezone;
 static unsigned owner_writes=0,code_clears=0,all_clears=0;
 static State getState(){owner_only();return state;}
 static void setState(State v){owner_only();state=v;}
 static bool loadOwnerId(char* p,size_t n){owner_only();snprintf(p,n,"%s",owner.c_str());return !owner.empty();}
 static bool loadOwnerCode(char* p,size_t n){owner_only();snprintf(p,n,"%s",code.c_str());return !code.empty();}
 static void clearOwnerId(){owner_only();owner.clear();++all_clears;}
 static void clearOwnerCode(){owner_only();code.clear();++code_clears;}
 static void saveOwnerId(const char* p){owner_only();owner=p;++owner_writes;}
 static bool saveTimezone(const char* p){owner_only();timezone=p;return true;}
 static void clearHomeWifiCreds(){owner_only();++all_clears;}
 static void clearApCreds(){owner_only();++all_clears;}
 static void setProvisioned(bool){owner_only();}
}
static provision_claim::Job g_owner_claim_job;
class ProvisioningManager {public:
 unsigned long last_claim_attempt_ms=0,owner_id_set_ms=0;
 unsigned claim_attempts=0;
 bool claim_in_progress=false,claim_completed=false,setup=true;
 static constexpr unsigned OWNER_CLAIM_MAX_ATTEMPTS=4;
 char device_id[32]="device";String error;
 void setLastError(const char* e){owner_only();error=e;}
 bool claimTransportBusy()const;void cancelOwnerClaim();void resetOwnerClaimState();
 bool tryClaimOwnerId();bool applyClaimResult();
 bool isSetupModeActive()const{return setup;}
 void stopSetupMode(){check(!claimTransportBusy(),"teardown only after worker drains");setup=false;}
 bool startSetupMode(){setup=true;resetOwnerClaimState();return true;}
};
static ProvisioningManager g_provisioning_manager_instance;
static ProvisioningManager* g_provisioning_manager=&g_provisioning_manager_instance;
static struct Server {
 unsigned responses=0;int code=0;String body;
 void sendHeader(const char*,const char*){}
 void send(int c,const char*,const char* b){++responses;code=c;body=b;}
} server_instance;
static Server* server=&server_instance;
static std::atomic<bool> g_provision_reset_pending{false};
static struct {void reset(){}} g_provision_display_status;
static char g_last_provision_display_status[32];
static unsigned g_provision_display_poll_ms=0,g_last_provision_qr_ms=0;
static bool g_pending_provision_qr=false;
static ProvisioningState::State g_last_prov_state=ProvisioningState::STATE_CONNECTED;
'''

TESTS = r'''
static void reset(){
 g_owner_claim_job.cancel();
 if(g_owner_claim_job.take())g_owner_claim_job.finish(provision_claim::Result{});
 provision_claim::Request q;provision_claim::Result r;bool current;
 g_owner_claim_job.consume(q,r,current);
 g_provisioning_manager_instance=ProvisioningManager{};
 ProvisioningState::owner.clear();ProvisioningState::code="CODE123";
 ProvisioningState::timezone.clear();ProvisioningState::state=ProvisioningState::STATE_CONNECTED;
 ProvisioningState::owner_writes=ProvisioningState::code_clears=ProvisioningState::all_clears=0;
 clock_ms=1;sntp_pending=false;g_provision_reset_pending=false;
 connect_hook=post_hook=read_hook=nullptr;response_code=200;content_size=-1;
 begin_ok=connect_ok=true;response_body="{\"owner_id\":\"new-owner\"}";
 handshake_s=connect_ms=read_ms=stop_calls=post_calls=body_reads=0;ca_set=insecure_set=false;
 check(!ssl_objects&&!http_objects&&!tls_connected&&!sense_memory::current,"prior transport fully destroyed and trace unowned");
 diagnostic_output.clear();destructor_fault=false;
 sense_memory::failures.guard.clear();sense_memory::failures.sequence=0;sense_memory::failures.dropped=0;
 sense_memory::failures.phase=sense_memory::Idle;
 for(auto& x:sense_memory::failures.ring)x={};
 server_instance=Server{};
}
static void queue(){const int before=post_calls;check(!g_provisioning_manager->tryClaimOwnerId(),"queue returns without success");check(g_owner_claim_job.busy()&&post_calls==before,"main submits without network");}
static void run(){check(halo_provisioning_claim_worker_poll(),"worker takes one job");check(!ntp_active&&!dma_active,"all transport guards closed before publication");}
int main(){
 sense_memory::begin();sense_memory::bind_upload_worker();halo_tls_memory::initialize(true);
 reset();queue();check(!g_provisioning_manager->tryClaimOwnerId()&&g_provisioning_manager->claim_attempts==1,"one slot and one attempt");
 run();check(ProvisioningState::owner.empty(),"worker never applies owner");
 check(g_provisioning_manager->applyClaimResult()&&ProvisioningState::owner=="new-owner","main commits matching success");
 check(ProvisioningState::owner_writes==1&&ProvisioningState::code_clears==1,"exactly one owner commit");
 check(!g_provisioning_manager->applyClaimResult()&&!halo_provisioning_claim_worker_poll(),"no duplicate consume or transport");
 check(handshake_s==8&&connect_ms==5000&&read_ms==3000&&ca_set&&!insecure_set,"real client bounded with CA verification");
 check(!g_owner_claim_job.busy()&&!ntp_active&&!dma_active,"success releases slot and guards");
 check(diagnostic_output.find("[CLAIM_MEM] queued_ms=1 generation=")!=String::npos,"claim trace identifies queue timestamp and generation");
 check(diagnostic_output.find("qualified=1 min_free_bytes=4321 since=task_start")!=String::npos,"claim worker stack bytes measured after client cleanup");
 check(diagnostic_output.find("http=200 tls_bytes=")!=String::npos&&diagnostic_output.find("mask=63 hook=1 failures=0 loss_seen=0 complete=1")!=String::npos,"successful claim has all six heap phases");
 check(diagnostic_output.find("point=0 internal=30000")!=String::npos&&diagnostic_output.find("point=5 internal=30000")!=String::npos,"claim snapshots bracket full client lifetime");
 check(diagnostic_output.find("CODE123")==String::npos&&diagnostic_output.find("new-owner")==String::npos&&diagnostic_output.find("claim.invalid")==String::npos,"claim diagnostic contains no request response or endpoint");

 // Actual dispatcher counters include TLS allocations while the worker scope
 // is alive, and failed allocations are captured in their transport phase.
 reset();queue();connect_hook=[](){
  void* p=halo_tls_memory::calloc(1,96);halo_tls_memory::free(p);
  allocation_failure(48,MALLOC_CAP_DMA,"heap_caps_aligned_alloc");connect_ok=false;
 };run();g_provisioning_manager->applyClaimResult();
 check(diagnostic_output.find("http=-1 tls_bytes=0 mask=39 hook=1 failures=1 loss_seen=0 complete=1")!=String::npos,"failed connect has truthful phase mask and failure count");
 check(diagnostic_output.find("bytes=48 caps=00000004 phase=2")!=String::npos,"claim preserves failed size caps and connect phase");
 check(diagnostic_output.find("[TLS_ALLOC] owner=CLAIM_MEM")!=String::npos&&diagnostic_output.find("external_calls=1 external_requested_bytes=96 small_external_calls=1 small_external_requested_bytes=96 external_failures=0 default_fallbacks=0")!=String::npos,"claim reports actual external request deltas");
 diagnostic_output.clear();clock_ms=2200;connect_hook=nullptr;connect_ok=true;queue();run();g_provisioning_manager->applyClaimResult();
 check(diagnostic_output.find("[CLAIM_MEM] queued_ms=2200")!=String::npos&&diagnostic_output.find("failures=0 loss_seen=0 complete=1")!=String::npos,"retry excludes first claim allocation failure");
 check(diagnostic_output.find("external_calls=0 external_requested_bytes=0")!=String::npos,"retry excludes earlier allocator counters");
 reset();queue();destructor_fault=true;run();g_provisioning_manager->applyClaimResult();
 check(diagnostic_output.find("bytes=24 caps=00000004")!=String::npos,"failure in client destructor remains within trace lifetime");

 // Main continues servicing callbacks while a real worker is held in POST.
 reset();queue();std::mutex mu;std::condition_variable cv;bool entered=false,release=false;
 post_hook=[&](){std::unique_lock<std::mutex> l(mu);entered=true;cv.notify_one();cv.wait(l,[&]{return release;});};
 std::thread worker([](){halo_provisioning_claim_worker_poll();});
 {std::unique_lock<std::mutex> l(mu);cv.wait(l,[&]{return entered;});}
 for(unsigned i=0;i<50;++i){check(!g_provisioning_manager->applyClaimResult(),"held network never blocks owner result poll");}
 auto clears=ProvisioningState::all_clears;
 halo_prod_reset_wifi();halo_prod_reset_wifi();
 check(g_provision_reset_pending&&halo_provisioning_active()&&ProvisioningState::all_clears==clears,"reset deferred without radio/NVS effects");
 {std::lock_guard<std::mutex> l(mu);release=true;}cv.notify_one();worker.join();
 check(!ntp_active&&!dma_active,"cancel drains worker guards");
 check(!g_provisioning_manager->applyClaimResult()&&ProvisioningState::owner.empty(),"cancelled response cannot assign owner");
 halo_prod_reset_wifi();check(!g_provision_reset_pending&&ProvisioningState::all_clears>clears,"reset executes after drain");
 check(wrong_thread_access==0,"NVS and manager mutation stayed on owner thread");

 // Generation plus code/state prevents stale completion at every boundary.
 for(int boundary=0;boundary<4;++boundary){reset();queue();
  if(boundary==0)g_provisioning_manager->resetOwnerClaimState();
  if(boundary==1)connect_hook=[](){g_owner_claim_job.cancel();};
  if(boundary==2)post_hook=[](){g_owner_claim_job.cancel();};
  run();if(boundary==3)g_provisioning_manager->resetOwnerClaimState();
  check(!g_provisioning_manager->applyClaimResult()&&ProvisioningState::owner_writes==0,"stale result discarded");
 }
 reset();queue();run();ProvisioningState::code="DIFFERENT";
 check(!g_provisioning_manager->applyClaimResult()&&!ProvisioningState::owner_writes,"different code rejects receipt");
 reset();ProvisioningState::code="co-de 123";queue();run();
 check(g_provisioning_manager->applyClaimResult()&&ProvisioningState::owner=="new-owner","normalized stored code preserves matching receipt");
 reset();queue();run();ProvisioningState::state=ProvisioningState::STATE_CONNECTING_HOME_WIFI;
 check(!g_provisioning_manager->applyClaimResult()&&!ProvisioningState::owner_writes,"changed connection rejects receipt");

 // Rejected duplicate POSTs retain the live claim and give a retry response.
 reset();queue();check(rejectClaimMutationWhileBusy(),"mutation blocked while queued");
 check(server->code==409&&server->body.find("retryable")!=String::npos&&!ProvisioningState::all_clears,"busy reply has no NVS mutation");
 run();check(g_provisioning_manager->applyClaimResult(),"rejected duplicate does not cancel live claim");
 check(!rejectClaimMutationWhileBusy(),"mutation admitted after drain");

 for(int fault=0;fault<8;++fault){reset();queue();
  if(fault==0)begin_ok=false;
  if(fault==1)connect_ok=false;
  if(fault==2)response_code=-1;
  if(fault==3){content_size=769;response_body=String(769,'x');}
  if(fault==4)response_body=String(4000,'x');
  if(fault==5)response_body="{bad-json";
  if(fault==6)connect_hook=[](){clock_ms+=25000;};
  if(fault==7)read_hook=[](){clock_ms+=21000;};
  run();check(!g_provisioning_manager->applyClaimResult()&&!ProvisioningState::owner_writes,"transport fault cannot commit owner");
  check(!g_owner_claim_job.busy()&&!ntp_active&&!dma_active,"fault releases slot and guards");
  if(fault==3)check(body_reads==0,"oversized content-length rejected before read");
 }
 reset();queue();clock_ms+=provision_claim::kAttemptMs;run();
 check(!post_calls&&!g_provisioning_manager->applyClaimResult(),"expired queued job never starts HTTP");
 reset();sntp_pending=true;g_provisioning_manager->tryClaimOwnerId();
 check(!g_owner_claim_job.busy()&&!post_calls,"pending SNTP leaves owner responsive");
 reset();response_code=403;response_body="{\"error\":\"expired_code\"}";queue();run();g_provisioning_manager->applyClaimResult();
 check(g_provisioning_manager->claim_completed&&ProvisioningState::code.empty()&&!ProvisioningState::owner_writes,"terminal claim error preserved");
 reset();response_code=-1;
 for(unsigned i=0;i<4;++i){clock_ms=1+i*11000;queue();run();g_provisioning_manager->applyClaimResult();}
 clock_ms+=11000;g_provisioning_manager->tryClaimOwnerId();
 check(!g_owner_claim_job.busy()&&g_provisioning_manager->claim_attempts==4,"four-attempt limit preserved");
 reset();queue();response_body="{\"owner_id\":\"new-owner\",\"timezone\":\"UTC0\"}";run();g_provisioning_manager->applyClaimResult();
 check(ProvisioningState::timezone=="UTC0","timezone applies on main loop");
 reset();
 // Wrap-safe queue expiry, and bounded sink including exact capacity.
 provision_claim::Job job;provision_claim::Request q;q.queued_ms=UINT32_MAX-100;
 job.submit(q);auto ptr=job.take();check(!job.cancelled(*ptr,50),"deadline across millis wrap");
 check(job.cancelled(*ptr,20000),"wrapped deadline expires");
 provision_claim::Result r;provision_claim::ResponseSink sink(r);String full(768,'a');
 check(sink.write(reinterpret_cast<const uint8_t*>(full.data()),full.size())==768&&!r.overflow&&r.response[768]==0,"exact response capacity terminates");
 check(sink.write('x')==0&&r.overflow&&r.response[768]==0,"one extra byte rejected");
 printf("%s checks=%u failures=%u mailbox_bytes=%zu\n",failures?"FAIL":"PASS",checks,failures,sizeof(provision_claim::Job));
 return failures?1:0;
}
'''

def harness(root):
    manager=(root/(SHARED+'ProvisioningManager.cpp')).read_text()
    wrapper=(root/FILES[-1]).read_text();sense=(root/FILES[-2]).read_text()
    worker=definition(sense,'static void upload_worker_task(void *arg)')
    assert worker.index('halo_provisioning_claim_worker_poll()') < worker.index('UploadWorkerClaim worker_claim')
    update=definition(manager,'void ProvisioningManager::update()')
    assert update.index('applyClaimResult();') < update.index('if (!setup_mode_active)')
    assert 'server->handleClient();' in update
    stop=definition(manager,'void ProvisioningManager::stopSetupMode()')
    assert stop.index('if (claimTransportBusy()) return;') < stop.index('stopHttpServer();')
    for name,mutation in [('void handleWifiPost() {', 'ProvisioningState::clearOwnerId()'),
                           ('void handleUserIdPost() {', 'ProvisioningState::saveOwnerId(')]:
        body=definition(manager,name)
        assert body.index('if (rejectClaimMutationWhileBusy()) return;') < body.index(mutation)
    scan=definition(manager,'void handleScan() {')
    assert scan.index('claimTransportBusy()') < scan.index('WiFi.scanNetworks(')
    applied=definition(manager,'bool ProvisioningManager::applyClaimResult()')
    assert applied.index('if (!current || !same_code') < applied.index('ProvisioningState::saveTimezone')
    work=definition(manager,'extern "C" bool halo_provisioning_claim_worker_poll()')
    assert work.index('}\n  g_owner_claim_job.finish(result);') > work.index('provision_claim::transport')
    assert 'xTaskCreate' not in work
    loop=definition(wrapper,'void halo_prod_loop()')
    assert 'g_provision_reset_pending.load() && !g_provisioning_manager.claimTransportBusy()' in loop
    code=PREFIX+'\n'+definition(manager,'static bool normalize_owner_code(')
    for name in ['bool ProvisioningManager::claimTransportBusy() const',
                 'void ProvisioningManager::cancelOwnerClaim()', 'void ProvisioningManager::resetOwnerClaimState()',
                 'extern "C" bool halo_provisioning_claim_worker_poll()', 'bool ProvisioningManager::tryClaimOwnerId()',
                 'bool ProvisioningManager::applyClaimResult()', 'static bool rejectClaimMutationWhileBusy()']:
        code+='\n'+definition(manager,name)
    for name in ['bool halo_provisioning_active()', 'void halo_prod_reset_wifi()']:
        code+='\n'+definition(wrapper,name).replace('g_provisioning_manager.', 'g_provisioning_manager->')
    return code+TESTS

def main():
    p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,default=ROOT);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--arduino-json',type=Path,default=Path.home()/'Documents/Arduino/libraries/ArduinoJson/src')
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
    (a.out/'HTTPClient.h').write_text(STUB);(a.out/'WiFiClientSecure.h').write_text('#pragma once\n#include "HTTPClient.h"\n')
    (a.out/'esp_heap_caps.h').write_text('#pragma once\n#include "HTTPClient.h"\n')
    (a.out/'freertos').mkdir(exist_ok=True)
    for name in ('FreeRTOS.h','task.h'):
        (a.out/'freertos'/name).write_text('#pragma once\n#include "HTTPClient.h"\n')
    (a.out/'test.cpp').write_text(harness(a.source_root))
    cmd=[shutil.which('clang++') or 'c++','-std=c++17','-pthread','-Wall','-Wextra',
         '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(a.out),
         '-I'+str(a.source_root/SHARED),'-I'+str(a.arduino_json),str(a.out/'test.cpp'),'-o',str(a.out/'test')]
    c=subprocess.run(cmd,text=True,capture_output=True,timeout=45);(a.out/'compile.log').write_text(c.stdout+c.stderr)
    if c.returncode:raise RuntimeError('compile failed: '+str(a.out/'compile.log'))
    r=subprocess.run([str(a.out/'test')],text=True,capture_output=True,timeout=30);(a.out/'run.log').write_text(r.stdout+r.stderr)
    report={'status':'PASS' if not r.returncode else 'FAIL','output':r.stdout.strip(),
            'sources':{f:hashlib.sha256((a.source_root/f).read_bytes()).hexdigest() for f in FILES},
            'limits':'Faulted host SDK boundaries; no device/real DNS timing, heap or radio acceptance.'}
    (a.out/'RESULT.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    if r.returncode:raise RuntimeError('test failed')
if __name__=='__main__':main()

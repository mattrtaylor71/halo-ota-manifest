#!/usr/bin/env python3
"""Run the entire production image PUT and actual cancellation client with SDK doubles.

Exercises request/response, retries, DMA-reservation cleanup, cancellation and
allocation-report lifetime, including image pacing under modeled TX pressure.
No hardware, cloud or real TLS memory claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <set>
#include <vector>
struct String:std::string {
 using std::string::string;using std::string::operator=;
 String()=default;String(const std::string& s):std::string(s){}
 bool startsWith(const char* s)const{return rfind(s,0)==0;}
 int indexOf(char c,size_t p=0)const{auto n=find(c,p);return n==npos?-1:int(n);}
 int indexOf(const char* c,size_t p=0)const{auto n=find(c,p);return n==npos?-1:int(n);}
 String substring(size_t p)const{return substr(p);}
 String substring(size_t a,size_t b)const{return substr(a,b-a);}
 int toInt()const{return std::atoi(c_str());}
 void trim(){auto a=find_first_not_of(" \r\n\t");if(a==npos){clear();return;}
  *this=substr(a,find_last_not_of(" \r\n\t")-a+1);}
};
struct Scenario {
 bool saved=false,initial_cancel=false,cancel_connect=false,foreground_connect=false;
 unsigned connect_failures=0,write_failures=0;
 size_t fail_at=SIZE_MAX,cancel_at=SIZE_MAX,foreground_at=SIZE_MAX,partial=512;
 size_t queue_limit=SIZE_MAX,image_bytes=4096;
 uint32_t write_ms=0,body_write_ms=0,budget=60000,start_ms=1000;
 unsigned cancel_pace=0,foreground_pace=0;
 bool external_oom=false;
 std::string reply="HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
};
static Scenario scenario;
static bool paused=false,foreground_active=false,alive=false,locked=false,in_sdk=false;
static uint32_t now_ms=1000;
static unsigned attempts=0,resets=0,locks=0,unlocks=0,releases=0,acquires=0,trace_reports=0;
static size_t max_write=0,closed_writes=0;
static size_t queued_bytes=0,peak_queue=0,wire_at_pace_abort=0;
static unsigned queue_failures=0,body_writes=0,writes_after_abort=0,pace_waits=0;
static std::vector<uint32_t> paced_delays;
static std::vector<std::string> requests;
static std::string diagnostics;
static uint8_t reserve;
static uint8_t* g_camera_dma_reserve=&reserve;
static constexpr int ESP_OK=0,MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_DMA=4,MALLOC_CAP_SPIRAM=8;
using TaskHandle_t=void*;
static TaskHandle_t task=reinterpret_cast<void*>(1);
static TaskHandle_t xTaskGetCurrentTaskHandle(){return task;}
static std::set<void*> external_live;
static unsigned tls_allocations=0,tls_frees=0,foreign_probes=0;
static bool expect_tls_scope=false;
static void* heap_caps_calloc(size_t n,size_t size,uint32_t caps){
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(scenario.external_oom)return nullptr;
 void* p=std::calloc(n,size);if(p)external_live.insert(p);return p;
}
static void heap_caps_free(void* p){external_live.erase(p);std::free(p);}
#include "halo_ota_demo/firmware/shared/ScopedTlsMemory.h"
static void* tls_allocate(size_t bytes,bool scoped){
 void* p=halo_tls_memory::calloc(1,bytes);assert(p);
 assert((external_live.count(p)!=0)==(scoped&&!scenario.external_oom));
 for(size_t n=0;n<bytes;++n)assert(static_cast<uint8_t*>(p)[n]==0);
 ++tls_allocations;return p;
}
static void tls_free(void* p){if(p){halo_tls_memory::free(p);++tls_frees;}}
static void foreign_probe(){
 const TaskHandle_t saved=task;task=reinterpret_cast<void*>(2);
 {halo_tls_memory::Scope foreign;assert(!expect_tls_scope||!foreign.active());
  // While the image owns the scope, foreground/OTA/voice tasks still use
  // the exact default allocator even if they attempt their own scope.
  void* p=tls_allocate(511,!expect_tls_scope);tls_free(p);}
 task=saved;++foreign_probes;
}
static size_t heap_caps_get_free_size(int){return alive?6000:30000;}
static size_t heap_caps_get_largest_free_block(int){return alive?700:13000;}
using Hook=void(*)(size_t,uint32_t,const char*);static Hook failure_hook=nullptr;
static int heap_caps_register_failed_alloc_callback(Hook h){failure_hook=h;return 0;}
static struct {
 template<class... A>void printf(const char* f,A... a){char b[768];snprintf(b,sizeof b,f,a...);
  if(!strncmp(b,"[IMAGE_PUT_MEM]",15)||!strncmp(b,"[ALLOC_FAIL]",12)){
   assert(!alive&&!in_sdk);diagnostics+=b;
   if(strstr(b,"tls_bytes="))++trace_reports;
  }}
 template<class T>void print(const T&){} template<class T>void println(const T&){}
}Serial;
#define HALO_MEMORY_DIAGNOSTICS_TEST 1
#include "Sense_Minimal/sense_memory_diag.h"
#include "Sense_Minimal/sense_image_http.h"
static uint32_t millis(){return now_ms;}
static void delay(uint32_t n){
 now_ms+=n;
 // A scheduling yield without positive elapsed time cannot drain this queue.
 // Rate/capacity are adversarial SDK doubles, not measured Wi-Fi throughput.
 if(n)queued_bytes-=std::min(queued_bytes,size_t(n)*256);
 if(alive&&body_writes){
  ++pace_waits;paced_delays.push_back(n);
  if(scenario.cancel_pace==pace_waits)paused=true;
  if(scenario.foreground_pace==pace_waits)foreground_active=true;
  if(paused||foreground_active)wire_at_pace_abort=requests.back().size();
 }
}
static bool media_upload_network_active(){return true;}
static bool media_retry_network_active(){return scenario.saved;}
static bool media_retry_network_cancelled(){return paused;}
static bool media_voice_list_active(){return false;}
static uint32_t media_voice_list_remaining_ms(){return UINT32_MAX;}
static bool deadline_expired(uint32_t d){return d&&int32_t(now_ms-d)>=0;}
static constexpr uint32_t ACTION_MIN_REMAINING_MS=1000;
static void presign_set_error_text(const char*){}
static void diag_record_error(const char*,int,const char*){}
static void diag_record_error_persistent(const char*,int,const char*){}
static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){}
static void camera_dma_reserve_release(const char*){assert(g_camera_dma_reserve);g_camera_dma_reserve=nullptr;++releases;}
static void camera_dma_reserve_acquire(const char*){assert(!g_camera_dma_reserve);g_camera_dma_reserve=&reserve;++acquires;}
static void http_queue_lock(const char*,uint32_t){assert(!locked);locked=true;++locks;}
static void http_queue_unlock(const char*,uint32_t){assert(locked);locked=false;++unlocks;}
static bool wifi_hard_reset_and_reconnect(const char*,uint32_t){assert(!alive&&!locked);++resets;return true;}
static struct {uint32_t getFreeHeap(){return 30000;}}ESP;
static struct {bool active=false;uint32_t job_id=0;}current_job;
struct SenseBackupWifiCall{explicit operator bool()const{return true;}};
struct HaloNtpDnsGuard{};
struct PresignReply{bool immutable_image=true;size_t expected_image_bytes=0;String checksum_sha256_b64;};
struct Stream {
 uint32_t timeout=60000;
 void setTimeout(uint32_t n){timeout=n;}
 virtual int read()=0;virtual uint8_t connected()=0;
 String readStringUntil(char delimiter){String out;uint32_t start=now_ms;
  while(uint32_t(now_ms-start)<timeout){int ch=read();if(ch==delimiter)break;
   if(ch<0){if(!connected())break;delay(1);}else out+=char(ch);
  }return out;
 }
};
struct WiFiClientSecure:Stream {
 size_t response_offset=0;
 void* context=nullptr;void* records=nullptr;
 WiFiClientSecure(){context=tls_allocate(512,expect_tls_scope);}
 virtual ~WiFiClientSecure(){
  stop();
  // Destructor work must still see the image scope. These are SDK doubles,
  // not a model of the actual Arduino context's C++ new allocation.
  void* p=tls_allocate(127,expect_tls_scope);tls_free(p);tls_free(context);
 }
 void stop(){assert(!in_sdk);alive=false;queued_bytes=0;tls_free(records);records=nullptr;}
 void setHandshakeTimeout(uint32_t){}
 int lastError(char* b,size_t n){snprintf(b,n,"test");return 48;}
 virtual int connect(const char*,uint16_t,int32_t){
  assert(!alive);++attempts;requests.emplace_back();response_offset=0;queued_bytes=0;body_writes=0;
  assert(!records);records=tls_allocate(2048,expect_tls_scope);foreign_probe();
  alive=attempts>scenario.connect_failures;
  if(scenario.cancel_connect)paused=true;
  if(scenario.foreground_connect)foreground_active=true;
  return alive?1:0;
 }
 virtual size_t write(uint8_t b){return write(&b,1);}
 virtual size_t write(const uint8_t* p,size_t n){
  if(!alive){++closed_writes;return 0;}in_sdk=true;max_write=std::max(max_write,n);
  auto& wire=requests.back();
  if(paused||foreground_active)++writes_after_abort;
  const bool body=wire.find("\r\n\r\n")!=std::string::npos;
  if(body)++body_writes;
  size_t sent=std::min(n,scenario.partial);
  if(sent>scenario.queue_limit-queued_bytes){
   ++queue_failures;failure_hook(512,MALLOC_CAP_DMA,"heap_caps_aligned_alloc");
   alive=false;in_sdk=false;return 0;
  }
  const bool fail=attempts<=scenario.write_failures&&wire.size()+sent>=scenario.fail_at;
  if(fail)sent=scenario.fail_at>wire.size()?scenario.fail_at-wire.size():0;
  wire.append(reinterpret_cast<const char*>(p),sent);now_ms+=scenario.write_ms+(body?scenario.body_write_ms:0);
  queued_bytes+=sent;peak_queue=std::max(peak_queue,queued_bytes);
  if(wire.size()>=scenario.cancel_at)paused=true;
  if(wire.size()>=scenario.foreground_at)foreground_active=true;
  if(fail){failure_hook(544,MALLOC_CAP_DMA,"heap_caps_aligned_alloc");alive=false;}
  in_sdk=false;return fail?0:sent;
 }
 virtual int read(){if(!alive||response_offset>=scenario.reply.size())return -1;
  return uint8_t(scenario.reply[response_offset++]);}
 virtual int read(uint8_t* p,size_t n){size_t got=0;while(got<n){int c=read();if(c<0)break;p[got++]=uint8_t(c);}return int(got);}
 virtual int available(){return alive?int(scenario.reply.size()-response_offset):0;}
 virtual uint8_t connected(){return alive&&response_offset<scenario.reply.size();}
 virtual int peek(){return available()?uint8_t(scenario.reply[response_offset]):-1;}
 size_t printf(const char* fmt,...){char b[8192];va_list args;va_start(args,fmt);
  int n=vsnprintf(b,sizeof b,fmt,args);va_end(args);assert(n>=0&&size_t(n)<sizeof b);
  return write(reinterpret_cast<uint8_t*>(b),size_t(n));}
 size_t print(const char* s){return write(reinterpret_cast<const uint8_t*>(s),strlen(s));}
};
#include "Sense_Minimal/sense_media_retry_client.h"
static void tls_configure(SenseMediaRetryClient&,const char*){}
#define UPLOAD_TLS_CHUNK_BYTES 512
'''

TESTS = r'''
static unsigned checks=0,cases=0;
static const char* case_name="";
static void require(bool ok){++checks;if(!ok){fprintf(stderr,"FAIL %s linecheck=%u attempts=%u code-independent\n",case_name,checks,attempts);std::abort();}}
static const String url="https://fixture.s3.amazonaws.com/images/a.jpg?X-Amz-Signature="+std::string(1740,'a')+"&x=%2F+%20";
static const String checksum="+cGrAmgp6P59Lr/1Fuwsv3cHoqlvOdvNH5eo9Sf6CRk=";
static const std::string image(4096,'I');
using Put=bool(*)(const String&,const uint8_t*,size_t,const char*,uint32_t,uint32_t,bool*,const PresignReply*,int*);
static void run(Put put,const char* name,Scenario config,bool expect_ok,int expect_code,unsigned expect_attempts,bool expect_budget=false,bool durable=true,bool reserve_held=true){
 ++cases;case_name=name;scenario=config;paused=config.initial_cancel;foreground_active=false;
 expect_tls_scope=put==candidate_put;require(external_live.empty());
 tls_allocations=tls_frees=foreign_probes=0;task=reinterpret_cast<void*>(1);
 alive=locked=in_sdk=false;now_ms=config.start_ms;attempts=resets=locks=unlocks=releases=acquires=trace_reports=0;
 max_write=closed_writes=0;requests.clear();diagnostics.clear();g_camera_dma_reserve=reserve_held?&reserve:nullptr;
 queued_bytes=peak_queue=wire_at_pace_abort=0;queue_failures=body_writes=writes_after_abort=pace_waits=0;paced_delays.clear();
 require(!sense_memory::current);const String before=url;const std::string payload(config.image_bytes,'I'),saved_image=payload;
 PresignReply proof;proof.expected_image_bytes=payload.size();proof.checksum_sha256_b64=checksum;
 bool aborted=false;int code=77;
 const bool ok=put(url,reinterpret_cast<const uint8_t*>(payload.data()),payload.size(),"image/jpeg",118,
   now_ms+config.budget,&aborted,durable?&proof:nullptr,&code);
 require(ok==expect_ok);require(code==expect_code);require(attempts==expect_attempts);require(aborted==expect_budget);
 require(!alive&&!locked&&!sense_memory::current&&sense_memory::failures.phase==sense_memory::Idle);
 require(locks==unlocks&&releases==acquires&&bool(g_camera_dma_reserve)==reserve_held);
 require(tls_allocations==tls_frees&&external_live.empty());require(foreign_probes==attempts);
 // No image transport return may leave allocation routing active for the
 // next task operation, including early cancellation and exhausted retries.
 void* outside=tls_allocate(333,false);tls_free(outside);
 require(url==before&&payload==saved_image);require(max_write<=512);require(writes_after_abort==0);
 if(config.saved||paused)require(resets==0);
 if(ok){const std::string path=url.substr(url.find('/',8));
  std::string expected="PUT "+path+" HTTP/1.1\r\nHost: fixture.s3.amazonaws.com\r\nContent-Type: image/jpeg\r\nContent-Length: "+std::to_string(payload.size())+"\r\n";
  if(durable)expected+="If-None-Match: *\r\nx-amz-checksum-sha256: "+checksum+"\r\n";
  expected+="Connection: close\r\n\r\n"+payload;
  require(requests.back()==expected);
 }
 if(put==candidate_put&&attempts){require(trace_reports==attempts);
  if(config.write_failures)require(diagnostics.find("bytes=544 caps=00000004 phase=3")!=std::string::npos);
  if(config.write_failures&&config.fail_at<2000)require(closed_writes==0);
 }
}
static void exercise(Put put){
 Scenario s;
 const size_t header_bytes=std::string("PUT ").size()+url.substr(url.find('/',8)).size()+
  std::string(" HTTP/1.1\r\nHost: fixture.s3.amazonaws.com\r\nContent-Type: image/jpeg\r\nContent-Length: 4096\r\nIf-None-Match: *\r\nx-amz-checksum-sha256: ").size()+checksum.size()+
  std::string("\r\nConnection: close\r\n\r\n").size();
 run(put,"normal exact request",s,true,200,1);
 s.external_oom=true;run(put,"PSRAM allocation failure retains default fallback and cleanup",s,true,200,1);s={};
 run(put,"legacy optional receipt headers",s,true,200,1,false,false);
 run(put,"no DMA reservation initially",s,true,200,1,false,true,false);
 s.saved=true;run(put,"saved exact retry",s,true,200,1);
 s={};s.partial=3;run(put,"positive short writes",s,true,200,1);
 s={};s.connect_failures=1;run(put,"connect then recover",s,true,200,2);
 s.connect_failures=5;run(put,"connect retries exhausted",s,false,0,5);
 s.saved=true;run(put,"saved connect yields immediately",s,false,0,1);
 s={};s.write_failures=1;s.fail_at=2500;run(put,"body allocation failure then exact retry",s,true,200,2);
 s.write_failures=5;run(put,"body failures preserve image",s,false,0,5);
 s.saved=true;run(put,"saved body failure yields",s,false,0,1);
 s={};s.write_failures=5;s.fail_at=0;run(put,"failed headers never accepted",s,false,0,5);
 s={};s.write_failures=1;s.fail_at=header_bytes-1;run(put,"final header byte fails then retry",s,true,200,2);
 s={};s.saved=true;s.write_failures=1;s.fail_at=header_bytes-1;run(put,"saved final header failure retains image",s,false,0,1);
 s={};s.cancel_at=header_bytes;run(put,"input after last header before body",s,false,0,1);
 require(requests.back().size()==header_bytes);
 s={};s.foreground_at=header_bytes;run(put,"foreground after last header before body",s,false,0,1);
 require(requests.back().size()==header_bytes);
 s={};s.reply="HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\n\r\n";
 run(put,"412 retains reconciliation status",s,false,412,1);
 s.reply="HTTP/1.1 503 Retry\r\nContent-Length: 0\r\n\r\n";run(put,"HTTP failure retained",s,false,503,1);
 s.reply="HTTP/1.1 200 OK\r\nContent-Length: 0\r\n";run(put,"truncated response is not custody",s,false,-1,1);
 s.reply="";run(put,"lost response is not custody",s,false,-1,1);
 s={};s.initial_cancel=true;run(put,"cancelled admission",s,false,0,0);
 s={};s.cancel_connect=true;run(put,"input during handshake",s,false,0,1);
 s={};s.foreground_connect=true;run(put,"foreground before header",s,false,0,1);
 s={};s.cancel_at=100;run(put,"input during header",s,false,0,1);
 s={};s.cancel_at=3000;run(put,"input during body",s,false,0,1);
 s={};s.foreground_at=3000;run(put,"foreground during body",s,false,0,1);
 s={};s.budget=0;run(put,"expired before admission",s,false,0,0,true);
 s={};s.budget=1100;s.write_ms=500;run(put,"deadline during header",s,false,0,1,true);
 for(size_t cut=0;cut<header_bytes+image.size();cut+=127){
  s={};s.saved=true;s.write_failures=1;s.fail_at=cut;
  run(put,"saved uncertain partial request retains bytes",s,false,0,1);
  require(requests.back().size()==cut);
 }
 s={};s.saved=true;s.write_failures=1;s.fail_at=header_bytes+image.size();
 run(put,"all bytes handed off but SDK returns error retains image",s,false,0,1);
 require(requests.back().size()==header_bytes+image.size());
}
static void exercise_pacing(Put put,bool paced){
 Scenario s;
 run(put,"pacing exact successful body",s,true,200,1);
 const size_t header_bytes=requests.back().find("\r\n\r\n")+4;
 require(pace_waits==(paced?7u:0u));require(body_writes==8);
 require(now_ms==s.start_ms+(paced?14u:0u));
 for(uint32_t waited:paced_delays)require(waited==2);
 // All request headers and one body chunk fit. Continued zero-time writes
 // exceed capacity; positive two-ms waits retire one512-byte body chunk.
 for(bool saved:{false,true})for(size_t partial:{size_t(512),size_t(3)}){
  s={};s.saved=saved;s.partial=partial;s.queue_limit=header_bytes+512;
  run(put,"TX queue drains only with positive time",s,paced,paced?200:0,paced||saved?1:5);
  require(peak_queue<=s.queue_limit);require((queue_failures==0)==paced);
  if(paced)require(pace_waits==7);else require(requests.back().size()<=s.queue_limit);
 }
 if(!paced)return; // These fault arrivals specifically occur in the new wait.
 for(bool saved:{false,true})for(bool foreground:{false,true})for(unsigned at:{1u,4u,7u}){
  s={};s.saved=saved;if(foreground)s.foreground_pace=at;else s.cancel_pace=at;
  run(put,"user arrival during pacing sends no next byte",s,false,0,1);
  require(pace_waits==at);require(body_writes==at);
  require(requests.back().size()==header_bytes+at*512);
  require(requests.back().size()==wire_at_pace_abort);require(resets==0);
 }
 for(uint32_t start:{1000u,UINT32_MAX-15u}){
  s={};s.start_ms=start;s.budget=1001;s.body_write_ms=1000;
  run(put,"deadline during clamped one-ms pacing",s,false,0,1,true);
  require(paced_delays==std::vector<uint32_t>{1});require(body_writes==1);
  require(requests.back().size()==header_bytes+512);
  require(now_ms==uint32_t(start+1001));
  s.budget=1000;
  run(put,"deadline already expired skips pacing",s,false,0,1,true);
  require(paced_delays.empty());require(body_writes==1);
  require(requests.back().size()==header_bytes+512);
 }
 for(size_t bytes:{size_t(1),size_t(511),size_t(512),size_t(513)}){
  s={};s.image_bytes=bytes;
  run(put,"no wait before first or after last body chunk",s,true,200,1);
  require(pace_waits==(bytes>512?1u:0u));
  require(now_ms==s.start_ms+(bytes>512?2u:0u));
 }
}
int main(){halo_tls_memory::initialize(true);sense_memory::begin();exercise(candidate_put);
 exercise_pacing(candidate_put,true);
 BASELINE_RUN
 printf("PASS %u whole-production PUT cases / %u assertions; actual client and TLS allocation scope, exact requests, retries, 412/uncertainty, DMA/HTTP cleanup, positive-time TX pressure, pacing input/deadlines, allocation-report lifetime\n",cases,checks);
}
'''


def harness(root, baseline=None):
    put = (root/'Sense_Minimal/sense_upload_exec.h').read_text()
    http = (root/'Sense_Minimal/sense_http.h').read_text()
    functions = definition(http, 'static uint32_t deadline_remaining_ms(')
    functions += definition(http, 'static uint32_t clamp_timeout_ms(')
    functions += definition(http, 'static bool parse_url_parts(')
    functions += definition(put, 'struct UploadPutResponseFraming')+';\n'
    functions += definition(put, 'static bool upload_put_drain_response(')
    functions += definition(put, 'static bool upload_put_reset_with_budget(')
    functions += definition(put, 'static bool put_to_presigned_url(').replace('put_to_presigned_url(', 'candidate_put(', 1)
    if baseline:
        old = (baseline/'Sense_Minimal/sense_upload_exec.h').read_text()
        functions += definition(old, 'static bool put_to_presigned_url(').replace('put_to_presigned_url(', 'baseline_put(', 1)
    return PREFIX + functions + TESTS.replace('BASELINE_RUN', 'exercise(baseline_put);exercise_pacing(baseline_put,false);' if baseline else '')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--baseline-source-root', type=Path)
    p.add_argument('--out', type=Path)
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-put-integration-') as tmp:
        out = a.out or Path(tmp);out.mkdir(parents=True, exist_ok=True)
        cpp, exe = out/'integration.cpp', out/'integration'
        for name in ('esp_heap_caps.h', 'freertos/FreeRTOS.h', 'freertos/task.h'):
            stub=out/'sdk-stubs'/name;stub.parent.mkdir(parents=True,exist_ok=True);stub.write_text('#pragma once\n')
        cpp.write_text(harness(a.source_root,a.baseline_source_root))
        cmd = [shutil.which('clang++') or 'c++','-std=c++17','-O1','-g','-Wall','-Wextra',
               '-Wno-unused-function','-Wno-unused-variable','-fsanitize=address,undefined','-fno-omit-frame-pointer',
               '-I',str(out/'sdk-stubs'),'-I',str(a.source_root),str(cpp),'-o',str(exe)]
        build = subprocess.run(cmd,capture_output=True,text=True,timeout=40)
        (out/'build.log').write_text(build.stdout+build.stderr)
        run = subprocess.run([str(exe)],capture_output=True,text=True,timeout=30) if not build.returncode else None
        text = build.stdout+build.stderr+(run.stdout+run.stderr if run else '')
        result={'status':'PASS' if run and run.returncode==0 else 'FAIL','output':text,
                'scope':'Actual production PUT + cancellation client/diagnostic code; SDK/socket/heap boundaries are doubles, not physical TLS evidence.',
                'source_sha256':hashlib.sha256((a.source_root/'Sense_Minimal/sense_upload_exec.h').read_bytes()).hexdigest(),
                'baseline_compared':bool(a.baseline_source_root)}
        (out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n');print(text,end='')
        return result['status']!='PASS'


if __name__=='__main__':
    raise SystemExit(main())

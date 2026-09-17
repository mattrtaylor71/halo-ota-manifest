#!/usr/bin/env python3
"""Run actual voice retry code and PUT timeout setup against scripted HTTP faults.

The transport is fake; the production retry function, ArduinoJson parsing,
request headers/payload, timeout helper and reset-budget helper are compiled.
No device/network access and no claim of an end-to-end SDK wall-clock bound.
"""
from pathlib import Path
import argparse
import resource
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def definition(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def harness(root):
    voice = (root / 'Sense_Minimal/sense_voice.h').read_text()
    put = (root / 'Sense_Minimal/sense_upload_exec.h').read_text()
    http = (root / 'Sense_Minimal/sense_http.h').read_text()
    upload = (root / 'Sense_Minimal/sense_upload.h').read_text()
    main = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    op_queue = (root / 'Sense_Minimal/sense_op_queue.h').read_text()
    functions = definition(http, 'static uint32_t deadline_remaining_ms(')
    functions += definition(http, 'static uint32_t clamp_timeout_ms(')
    functions += definition(put, 'static bool upload_put_reset_with_budget(')
    functions += definition(voice, 'static bool voice_upload_and_parse(')
    functions += definition(upload, 'static bool http_post_json_with_retries(')
    functions += definition(upload, 'static void http_queue_unlock(').replace(
        'static void http_queue_unlock(', 'static void production_http_queue_unlock(', 1)
    functions += definition(main, 'static bool net_ready_for_tls(const char* reason, uint32_t timeout_ms, const char* mode, uint32_t job_id, const char* ui_policy) {')
    functions += definition(op_queue, 'static bool foreground_priority_reason_is_transient(')
    functions += definition(op_queue, 'static bool upload_wait_for_foreground_window(')
    functions += definition(op_queue, 'static bool upload_wait_for_foreground_clear_in_place(')
    attempt_line = next(line for line in main.splitlines()
                        if 'const uint8_t max_retries = media_retry_network_active()' in line)
    functions += '\nstatic unsigned production_outer_attempts() {\n' + attempt_line + '\nreturn max_retries;\n}\n'
    # Execute the actual PUT phase configuration and actual three-argument
    # connect expression; no duplicate model of how helpers are integrated.
    begin = put.index('    uint32_t tls_timeout = clamp_timeout_ms(60000, deadline_ms);')
    end = put.index('      Serial.printf("[UPLOAD] TLS connect failed', begin)
    setup = put[begin:end]
    setup = setup[:setup.rfind('{')] + '{ return false; }\nreturn true;\n'
    return r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#include "Sense_Minimal/sense_ops.h"
#include "Sense_Minimal/sense_media_network.h"
using String=std::string;
static uint32_t clock_ms,delay_ms,reset_budget,last_handshake,last_connect,last_stream_timeout;
static unsigned posts,resets,locks,unlocks,dma_releases,dma_acquires;
static bool connected=true,identity_ok=true,age_ok=true,receipt_ok=true,begin_ok=true;
static bool transport_ok=true,aborted=false;
static bool background=false,paused=false,in_sdk=false,cancel_connect=false,cancel_post=false,cancel_reply=false;
static unsigned task=1,owner=1,stops=0,io_writes=0,cancel_write=0,recoveries=0,readiness_waits=0;
static uint32_t cancel_delay=0;
static bool http_inflight=false,wifi_recover_requested=false;
static void* http_mutex=nullptr;
static void xSemaphoreGive(void*){}
static std::vector<size_t> write_sizes;
static bool media_retry_network_active(){return background&&task==owner;}
static bool media_retry_network_cancelled(){return media_retry_network_active()&&paused;}
static bool media_upload_network_active(){return media_retry_network_active();}
static bool foreground_active=false,dish_scan_inflight=false,voice_recording_active=false;
static bool upload_worker_holding_in_place=false,g_upload_flush_requested=false;
static const char* foreground_reason="foreground_active";
static bool foreground_priority_active(uint32_t,const char** reason){if(reason)*reason=foreground_reason;return foreground_active;}
static struct {bool active=false;uint32_t job_id=0;} current_job;
static std::vector<int> statuses;
static std::vector<std::map<std::string,std::string>> sent_headers;
static std::vector<std::vector<uint8_t>> sent_payloads;
static std::vector<uint32_t> handshakes,connections;
static const uint32_t ACTION_MIN_REMAINING_MS=1000;
static const int WL_CONNECTED=3,WL_DISCONNECTED=6,HTTPC_ERROR_CONNECTION_REFUSED=-1;
static const char* QUICK_ACK_BASE_URL="https://example.invalid";
static const char* QUICK_ACK_ENDPOINT="/voice";
static const char* TREPO_DEVICE_ID="fallback";
static uint8_t reserve;
static uint8_t* g_camera_dma_reserve=&reserve;
static uint32_t millis(){return clock_ms;}
static void delay(uint32_t ms){clock_ms+=ms;delay_ms+=ms;if(cancel_delay&&delay_ms>=cancel_delay)paused=true;}
static void vTaskDelay(uint32_t ms){delay(ms);}
#define pdMS_TO_TICKS(value) (value)
static struct {
  template<class... A>void printf(const char*,A...){}
  template<class T>void print(const T&){}
  template<class T>void println(const T&){}
} Serial;
static struct {int status(){return connected?WL_CONNECTED:WL_DISCONNECTED;}} WiFi;
struct SenseBackupWifiCall {explicit operator bool()const{return true;}};
struct HaloNtpDnsGuard {};
static void camera_dma_reserve_release(const char*){++dma_releases;}
static void camera_dma_reserve_acquire(const char*){++dma_acquires;}
static bool sense_voice_owner_matches(const UploadJob&){return identity_ok;}
static bool sense_voice_replay_age_ok(const UploadJob&){return age_ok;}
static uint32_t sense_voice_crc32(const uint8_t*,size_t){return 42;}
static bool sense_voice_backend_ack(const UploadJob&,int code,JsonDocument&){return code==202&&receipt_ok;}
static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){}
static void diag_record_error_persistent(const char*,int,const char*){}
static void diag_record_error(const char*,int,const char*){}
static void presign_set_error_text(const char*){}
static bool deadline_expired(uint32_t deadline){return deadline&&int32_t(clock_ms-deadline)>=0;}
static bool ensure_wifi_ready(const char*,uint32_t){++readiness_waits;return connected;}
static bool ensure_time_valid(const char*,uint32_t){++readiness_waits;return age_ok;}
static bool sense_time_has_fresh_sync(){return age_ok;}
static bool wifi_is_connected(){return connected;}
static void scan_ui_status_emit(const char*,const char*,const char*,uint32_t,bool){}
static void log_wifi_snapshot(const char*){}
static void wifi_recover_if_needed(const char*,int){++recoveries;}
static void http_queue_lock(const char*,uint32_t){++locks;}
static void http_queue_unlock(const char*,uint32_t){++unlocks;}
static bool wifi_hard_reset_and_reconnect(const char*,uint32_t budget){++resets;reset_budget=budget;return connected;}
struct Stream {
  uint32_t stream_timeout=15000;
  void setTimeout(uint32_t ms){stream_timeout=ms;last_stream_timeout=ms;}
};
struct WiFiClientSecure : Stream {
  virtual ~WiFiClientSecure()=default;
  void setInsecure(){}
  void setHandshakeTimeout(uint32_t seconds){last_handshake=seconds;handshakes.push_back(seconds);}
  virtual int connect(const char*,uint16_t,int32_t timeout){
    in_sdk=true;last_connect=timeout;if(cancel_connect)paused=true;in_sdk=false;return transport_ok;
  }
  virtual size_t write(uint8_t value){return write(&value,1);}
  virtual size_t write(const uint8_t*,size_t n){
    in_sdk=true;++io_writes;write_sizes.push_back(n);
    if(cancel_write&&io_writes==cancel_write)paused=true;in_sdk=false;return n;
  }
  virtual int read(){return -1;}
  virtual int read(uint8_t*,size_t){return -1;}
  virtual int available(){return 0;}
  virtual uint8_t connected(){return 1;}
  virtual int peek(){return -1;}
  void stop(){assert(!in_sdk);++stops;}
};
#include "Sense_Minimal/sense_media_retry_client.h"
static void tls_configure(WiFiClientSecure&,const char*){}
static void log_http_failure_details(const char*,const char*,int,WiFiClientSecure*){}
struct HTTPClient {
  WiFiClientSecure* client=nullptr;
  std::map<std::string,std::string> headers;
  bool begin(WiFiClientSecure& value,const String&){client=&value;return begin_ok;}
  void setReuse(bool){}
  void setTimeout(uint32_t){}
  void setConnectTimeout(uint32_t ms){last_connect=ms;connections.push_back(ms);}
  void addHeader(const char* k,const char* v){headers[k]=v;}
  void addHeader(const char* k,const String& v){headers[k]=v;}
  int POST(uint8_t* p,size_t n){
    sent_headers.push_back(headers);sent_payloads.emplace_back(p,p+n);
    assert(posts<statuses.size());const int code=statuses[posts++];
    if(cancel_post)paused=true;return code;
  }
  int POST(const String& body){return POST((uint8_t*)body.data(),body.size());}
  int getSize(){return 2;}
  String getString(){if(cancel_reply)paused=true;return "{}";}
  void end(){}
};
''' + functions + r'''
static bool production_put_connect(uint32_t deadline_ms){
  WiFiClientSecure tls;String host="example.invalid";uint16_t port=443;
  bool* aborted_for_budget=&aborted;uint32_t effective_job=1;
''' + setup + r'''
}
static UploadJob fixture(){
  static uint8_t pcm[]={1,2,3,4,5,6};
  UploadJob job={};job.job_id=7;job.is_voice=true;job.image_buf=pcm;job.image_len=sizeof(pcm);
  std::strcpy(job.voice.owner_id,"test-owner");std::strcpy(job.voice.device_id,"test-device");
  std::strcpy(job.voice.session_id,"test-session");
  std::strcpy(job.voice.request_id,"0123456789abcdef0123456789abcdef");job.voice.crc32=42;
  return job;
}
static void reset(std::vector<int> reply){
  clock_ms=1000;delay_ms=reset_budget=last_handshake=last_connect=last_stream_timeout=0;
  posts=resets=locks=unlocks=dma_releases=dma_acquires=0;
  background=paused=in_sdk=cancel_connect=cancel_post=cancel_reply=false;
  task=owner=1;stops=io_writes=cancel_write=recoveries=readiness_waits=0;write_sizes.clear();
  cancel_delay=0;http_inflight=wifi_recover_requested=false;
  foreground_active=dish_scan_inflight=voice_recording_active=false;
  upload_worker_holding_in_place=g_upload_flush_requested=false;
  foreground_reason="foreground_active";
  connected=identity_ok=age_ok=receipt_ok=begin_ok=transport_ok=true;aborted=false;
  statuses=reply;sent_headers.clear();sent_payloads.clear();handshakes.clear();connections.clear();
}
static void closed(){assert(locks==unlocks&&dma_releases==dma_acquires);}
int main(){
  const UploadJob job=fixture();
  for(int first: {408,429,500,502,503,504,599}){
    reset({first,202});assert(voice_upload_and_parse(job));
    assert(posts==2&&resets==0&&delay_ms==250);
    assert(sent_headers[0]==sent_headers[1]&&sent_payloads[0]==sent_payloads[1]);
    assert(sent_headers[0]["x-request-id"]==job.voice.request_id);
    assert(sent_headers[0]["x-owner-id"]==job.voice.owner_id);
    assert(sent_headers[0]["x-device-id"]==job.voice.device_id);
    assert(sent_headers[0]["x-session-id"]==job.voice.session_id);
    assert(handshakes==std::vector<uint32_t>({8,8})&&connections==std::vector<uint32_t>({8000,8000}));closed();
  }
  std::puts("PASS transient HTTP retries once with identical request/account/session/payload and no radio reset");
  for(int first: {200,201,400,401,403,404,409,410,422,499,600}){
    reset({first});assert(!voice_upload_and_parse(job));
    assert(posts==1&&resets==0&&delay_ms==0);closed();
  }
  reset({202});receipt_ok=false;assert(!voice_upload_and_parse(job));
  assert(posts==1&&resets==0&&delay_ms==0);closed();
  reset({503,503});assert(!voice_upload_and_parse(job));assert(posts==2&&resets==0);closed();
  reset({202});assert(voice_upload_and_parse(job));assert(posts==1&&resets==0);closed();
  std::puts("PASS permanent responses/malformed receipts stop; two transient responses exhaust the same two-attempt limit");
  reset({-1,202});assert(voice_upload_and_parse(job));assert(posts==2&&resets==1&&reset_budget==15000);closed();
  reset({202});identity_ok=false;assert(!voice_upload_and_parse(job)&&posts==0);closed();
  reset({202});age_ok=false;assert(!voice_upload_and_parse(job)&&posts==0);closed();
  std::puts("PASS existing transport recovery and frozen identity/age admission remain intact");

  for(uint32_t left:{0U,1U,999U,1000U,1999U,7000U,7999U,8000U,120000U}){
    reset({});const bool ok=production_put_connect(clock_ms+left);
    if(left<1000){assert(!ok&&aborted&&last_handshake==0&&last_connect==0);}
    else {
      assert(ok&&!aborted);assert(last_handshake==(left<8000?left:8000)/1000);
      assert(last_connect==(left<8000?left:8000));
    }
    assert(resets==0);
  }
  reset({});clock_ms=UINT32_MAX-3000;
  assert(production_put_connect(clock_ms+7000));assert(last_handshake==7&&last_connect==7000);
  reset({});assert(production_put_connect(0));assert(last_handshake==8&&last_connect==8000);
  std::puts("PASS actual PUT connect setup clamps TLS seconds/TCP milliseconds, refuses subsecond budget, handles rollover");
  for(uint32_t left:{0U,499U,500U,1499U,1500U,7500U,15500U,20000U}){
    reset({});const bool ok=upload_put_reset_with_budget("test",clock_ms+left);
    if(left<1500)assert(!ok&&resets==0);
    else {assert(ok&&resets==1);assert(reset_budget==(left<15500?left-500:15000));}
  }
  reset({});assert(upload_put_reset_with_budget("test",0)&&reset_budget==15000);
  std::puts("PASS actual PUT reset helper reserves radio-reset setup time and cannot request more remaining wait time");

  reset({});background=true;
  { SenseMediaRetryClient client;WiFiClientSecure* polymorphic=&client;
    std::vector<uint8_t> payload(4096,42);cancel_write=1;
    assert(polymorphic->write(payload.data(),payload.size())==0);
    assert(io_writes==1&&write_sizes[0]==512&&stops==1&&client.stream_timeout==0);
    assert(!polymorphic->connected()&&!polymorphic->available()&&polymorphic->read()==-1);
    assert(polymorphic->read(payload.data(),1)==-1&&polymorphic->peek()==-1&&stops==1);
  }
  reset({});background=true;
  { SenseMediaRetryClient client;cancel_connect=true;
    assert(!client.connect("test",443,60000));assert(last_connect==8000&&stops==1);
    assert(io_writes==0&&last_handshake==8);
  }
  reset({});background=true;
  { SenseMediaRetryClient client;paused=true;assert(client.available()==0&&stops==1); }
  reset({});background=true;task=2;
  { SenseMediaRetryClient client;paused=true;uint8_t payload[2048]{};
    assert(client.write(payload,sizeof(payload))==sizeof(payload));
    assert(write_sizes==std::vector<size_t>{2048}&&stops==0);
    assert(client.connect("test",443,23000)&&last_connect==23000);
  }
  std::puts("PASS actual client uses owner-task cancellation, chunks writes, closes only after SDK returns, unwinds Stream timeout, leaves foreground callers unchanged");

  reset({202});background=true;cancel_post=true;
  assert(!voice_upload_and_parse(job)&&posts==1&&resets==0&&delay_ms==0);closed();
  reset({202});background=true;cancel_reply=true;
  assert(!voice_upload_and_parse(job)&&posts==1&&resets==0&&delay_ms==0);closed();
  reset({-1});background=true;
  assert(!voice_upload_and_parse(job)&&posts==1&&resets==0&&delay_ms==0);closed();
  reset({202});background=true;paused=true;
  assert(!voice_upload_and_parse(job)&&posts==0&&resets==0);closed();
  assert(!upload_put_reset_with_budget("background",0)&&resets==0);
  std::puts("PASS actual voice cancellation retains uncertain accepted work and skips transport reset; saved PUT cannot reset radio");

  int code=0;String response;
  reset({200});background=true;cancel_post=true;
  assert(!http_post_json_with_retries("https://test","payload",code,response,"IMAGE_PRESIGN",nullptr,nullptr,7,20000));
  assert(posts==1&&recoveries==0&&delay_ms==0&&readiness_waits==0);closed();
  reset({200});background=true;cancel_reply=true;
  assert(!http_post_json_with_retries("https://test","payload",code,response,"IMAGE_RECONCILE",nullptr,nullptr,7,20000));
  assert(posts==1&&recoveries==0&&delay_ms==0);closed();
  reset({500,500,500});background=true;
  assert(!http_post_json_with_retries("https://test","payload",code,response,"IMAGE_PRESIGN",nullptr,nullptr,7,20000));
  assert(posts==3&&recoveries==0);closed();
  reset({-1});background=true;
  assert(!http_post_json_with_retries("https://test","payload",code,response,"IMAGE_PRESIGN",nullptr,nullptr,7,20000));
  assert(posts==1&&recoveries==0&&delay_ms==0&&readiness_waits==0);closed();
  reset({500,200});
  assert(http_post_json_with_retries("https://test","payload",code,response,"OTHER",nullptr,nullptr,7,20000));
  assert(posts==2&&recoveries==1&&readiness_waits==4);closed();
  std::puts("PASS actual presign/reconcile HTTP abort after pause without reset and ordinary HTTP retains existing recovery");

  reset({500});background=true;cancel_delay=20;
  assert(!http_post_json_with_retries("https://test","payload",code,response,"IMAGE_PRESIGN",nullptr,nullptr,7,20000));
  assert(posts==1&&recoveries==0&&delay_ms==20);closed();
  reset({});background=true;cancel_delay=20;
  assert(!media_retry_network_wait(3500)&&delay_ms==20);
  reset({});background=true;wifi_recover_requested=http_inflight=true;
  production_http_queue_unlock("saved",1);
  assert(!http_inflight&&!wifi_recover_requested&&resets==0);
  reset({});wifi_recover_requested=http_inflight=true;
  production_http_queue_unlock("fresh",1);
  assert(!http_inflight&&!wifi_recover_requested&&resets==1&&reset_budget==15000);
  std::puts("PASS actual cooldown yields within20ms polling and actual HTTP unlock suppresses only saved-media deferred radio reset");

  for(unsigned fault=0;fault<4;++fault){
    reset({});background=true;
    if(fault==1)paused=true;
    if(fault==2)connected=false;
    if(fault==3)age_ok=false;
    assert(net_ready_for_tls("saved",15000,"checkin",7,nullptr)==(fault==0));
    assert(readiness_waits==0&&resets==0&&delay_ms==0&&production_outer_attempts()==1);
  }
  reset({});paused=true;
  assert(net_ready_for_tls("fresh",15000,"checkin",7,nullptr));
  assert(readiness_waits==2&&production_outer_attempts()==3);
  reset({});connected=false;
  assert(!net_ready_for_tls("fresh",15000,"checkin",7,nullptr)&&readiness_waits==1);
  reset({});age_ok=false;
  assert(!net_ready_for_tls("fresh",15000,"checkin",7,nullptr)&&readiness_waits==2);
  std::puts("PASS actual worker readiness: saved cancel/offline/no-clock fail without waits; fresh requests retain normal readiness and three outer attempts");

  reset({});background=true;paused=true;
  assert(!upload_wait_for_foreground_clear_in_place(job,"presign",20000,&aborted));
  assert(!aborted&&delay_ms==0);
  reset({});background=true;foreground_active=true;cancel_delay=20;
  assert(!upload_wait_for_foreground_clear_in_place(job,"put",20000,&aborted));
  assert(!aborted&&delay_ms==80&&!upload_worker_holding_in_place);
  reset({});background=true;foreground_active=true;foreground_reason="recent_user_input";cancel_delay=20;
  assert(!upload_wait_for_foreground_window(job,foreground_reason,2200));
  assert(delay_ms==80&&!upload_worker_holding_in_place);
  reset({});paused=true;foreground_active=true;
  assert(!upload_wait_for_foreground_clear_in_place(job,"fresh",clock_ms+1080,&aborted));
  assert(aborted&&delay_ms==160&&!upload_worker_holding_in_place);
  std::puts("PASS actual foreground phase waits stop saved retries on pause, release hold state, and retain fresh-job deadline behavior");
}
'''


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.source_root
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        parser.error('C++ compiler required')
    with tempfile.TemporaryDirectory(prefix='halo-media-network-') as directory:
        cpp, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(harness(root))
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra',
                        '-Wno-deprecated-declarations', '-Wno-unused-variable',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(root), '-I', str(root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
                        str(cpp), '-o', str(exe)], check=True, timeout=30, preexec_fn=no_core)
        subprocess.run([str(exe)], check=True, timeout=10, preexec_fn=no_core)


if __name__ == '__main__':
    main()

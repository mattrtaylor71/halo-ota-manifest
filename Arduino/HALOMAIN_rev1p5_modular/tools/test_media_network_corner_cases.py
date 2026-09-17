#!/usr/bin/env python3
"""Deterministic fault injection at actual network/receipt/owner boundaries.

Imports the existing host SDK scaffolds, replaces their permissive HTTP/receipt
and owner mocks with actual production functions, and executes new adversarial
sequences. No hardware or network. Includes the exact zero-clock Wi-Fi timeout
regression originally reproduced against 170.
"""
import argparse
import hashlib
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile

import test_media_network_retry as network
import test_wifi_recovery as wifi

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text, old, new):
    assert text.count(old) == 1, (old[:100], text.count(old))
    return text.replace(old, new, 1)


def network_harness(root):
    source = network.harness(root).split('int main(){', 1)[0]
    upload = (root / 'Sense_Minimal/sense_upload.h').read_text()
    voice = (root / 'Sense_Minimal/sense_voice.h').read_text()
    retry = (root / 'Sense_Minimal/sense_media_retry.h').read_text()
    owner = retry[retry.index('static std::atomic<bool> g_media_retry_user_paused'):
                  retry.index('static void media_retry_load_locked()')]
    source = source.replace('#include <cassert>', '#include <cassert>\n#include <atomic>\n#include <functional>\n#include <random>\n#include <CommonCrypto/CommonDigest.h>')
    source = replace_once(source, 'static void xSemaphoreGive(void*){}', r'''
static bool semaphore_held=false;
static unsigned semaphore_acquires=0,semaphore_releases=0;
static std::function<void(const char*)> boundary_hook;
static void boundary(const char* name){if(boundary_hook)boundary_hook(name);}
static constexpr uint32_t portMAX_DELAY=UINT32_MAX;
static int xSemaphoreTake(void*,uint32_t timeout){
  assert(timeout==portMAX_DELAY);boundary("lock_wait");
  assert(!semaphore_held);semaphore_held=true;++semaphore_acquires;return 1;
}
static void xSemaphoreGive(void*){assert(semaphore_held);semaphore_held=false;++semaphore_releases;}
static size_t write_limit=SIZE_MAX;
static std::vector<uint8_t> wire_bytes;
static std::string input_bytes,reply_body;
static size_t input_offset=0;
static unsigned ack_checks=0;
''')
    source = replace_once(source, 'static bool media_retry_network_active(){return background&&task==owner;}\nstatic bool media_retry_network_cancelled(){return media_retry_network_active()&&paused;}', r'''
using TaskHandle_t=void*;
static TaskHandle_t xTaskGetCurrentTaskHandle(){return reinterpret_cast<void*>(static_cast<uintptr_t>(task));}
''' + owner)
    source = replace_once(source, 'static bool sense_voice_backend_ack(const UploadJob&,int code,JsonDocument&){return code==202&&receipt_ok;}', r'''
static int mbedtls_sha256(const unsigned char* data,size_t len,uint8_t* digest,int mode){
  assert(mode==0);++ack_checks;boundary("ack_hash_enter");
  const bool ok=CC_SHA256(data,static_cast<CC_LONG>(len),digest)!=nullptr;
  boundary("ack_hash_done");return ok?0:-1;
}
''' + network.definition(voice, 'static bool sense_voice_backend_ack('))
    source = replace_once(source, 'static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){}',
                          'static void uart_send_sense_diag(const char*,const char* event,const char*,int32_t,const char*){boundary(event);}')
    source = replace_once(source, 'static void http_queue_lock(const char*,uint32_t){++locks;}\nstatic void http_queue_unlock(const char*,uint32_t){++unlocks;}',
                          'static void http_queue_lock(const char*,uint32_t);\nstatic void http_queue_unlock(const char*,uint32_t);')
    source = replace_once(source, 'in_sdk=true;last_connect=timeout;if(cancel_connect)paused=true;in_sdk=false;return transport_ok;',
                          'in_sdk=true;last_connect=timeout;boundary("sdk_connect");in_sdk=false;return transport_ok;')
    source = replace_once(source, 'virtual size_t write(const uint8_t*,size_t n){\n    in_sdk=true;++io_writes;write_sizes.push_back(n);\n    if(cancel_write&&io_writes==cancel_write)paused=true;in_sdk=false;return n;\n  }', r'''
virtual size_t write(const uint8_t* p,size_t n){
  in_sdk=true;++io_writes;write_sizes.push_back(n);boundary("sdk_write_enter");
  const auto amount=n<write_limit?n:write_limit;
  wire_bytes.insert(wire_bytes.end(),p,p+amount);boundary("sdk_write_done");
  in_sdk=false;return amount;
}''')
    source = replace_once(source, 'virtual int read(){return -1;}\n  virtual int read(uint8_t*,size_t){return -1;}\n  virtual int available(){return 0;}\n  virtual uint8_t connected(){return 1;}\n  virtual int peek(){return -1;}', r'''
virtual int read(){
  in_sdk=true;boundary("sdk_read_enter");
  const int value=input_offset<input_bytes.size()?(uint8_t)input_bytes[input_offset++]:-1;
  boundary("sdk_read_done");in_sdk=false;return value;
}
virtual int read(uint8_t* p,size_t n){
  in_sdk=true;boundary("sdk_bulk_read_enter");
  const auto amount=std::min(n,input_bytes.size()-input_offset);
  memcpy(p,input_bytes.data()+input_offset,amount);input_offset+=amount;
  boundary("sdk_bulk_read_done");in_sdk=false;return static_cast<int>(amount);
}
virtual int available(){
  in_sdk=true;boundary("sdk_available");in_sdk=false;
  return static_cast<int>(input_bytes.size()-input_offset);
}
virtual uint8_t connected(){boundary("sdk_connected");return 1;}
virtual int peek(){boundary("sdk_peek");return input_offset<input_bytes.size()?(uint8_t)input_bytes[input_offset]:-1;}
''')
    source = replace_once(source, 'bool begin(WiFiClientSecure& value,const String&){client=&value;return begin_ok;}',
                          'bool begin(WiFiClientSecure& value,const String&){client=&value;boundary("http_begin");return begin_ok;}')
    source = replace_once(source, 'sent_headers.push_back(headers);sent_payloads.emplace_back(p,p+n);\n    assert(posts<statuses.size());const int code=statuses[posts++];\n    if(cancel_post)paused=true;return code;', r'''
boundary("post_enter");
    if(!client->connect("example.invalid",443,8000))return -1;
    if(client->write(p,n)!=n)return -2;
    sent_headers.push_back(headers);sent_payloads.emplace_back(p,p+n);
    assert(posts<statuses.size());const int code=statuses[posts++];
    boundary("post_status");return code;''')
    source = replace_once(source, 'int getSize(){return 2;}\n  String getString(){if(cancel_reply)paused=true;return "{}";}\n  void end(){}', r'''
int getSize(){boundary("reply_size");return reply_body.size();}
  String getString(){boundary("reply_body");return reply_body;}
  void end(){boundary("http_end");}''')
    # Replace renamed standalone unlock plus lock with real serialization used
    # by every actual POST/voice call, not just a separately asserted helper.
    start = source.index('static void production_http_queue_unlock(')
    original = network.definition(source, 'static void production_http_queue_unlock(')
    source = source[:start] + source[start:].replace(original, '', 1)
    marker = 'static bool voice_upload_and_parse('
    source = source.replace(marker, network.definition(upload, 'static void http_queue_lock(') + '\n' +
                            network.definition(upload, 'static void http_queue_unlock(') + '\n' + marker, 1)
    digest = hashlib.sha256(b'request:test-owner|test-device|test-session|0123456789abcdef0123456789abcdef').hexdigest()
    return source + r'''
static void corner_reset(std::vector<int> codes={202}){
  reset(codes);g_media_retry_user_paused=false;g_media_retry_network_owner=nullptr;
  boundary_hook={};semaphore_held=false;semaphore_acquires=semaphore_releases=0;
  http_mutex=(void*)1;wire_bytes.clear();write_limit=SIZE_MAX;input_bytes="reply";input_offset=0;
  ack_checks=0;reply_body=R"({"accepted":true,"async":true,"duplicate":false,"jobId":"HASH_FIXTURE"})";
}
static void clean(){
  assert(!in_sdk&&!semaphore_held&&!http_inflight);
  assert(semaphore_acquires==semaphore_releases&&dma_releases==dma_acquires);
}
static void pause_at(const char* event,unsigned occurrence=1){
  boundary_hook=[event,occurrence,count=0U](const char* name)mutable{
    if(!strcmp(event,name)&&++count==occurrence)g_media_retry_user_paused=true;
  };
}
int main(){
  auto job=fixture();job.from_voice_sd=true;
  for(const char* event:{"lock_wait","start","http_begin","post_enter","sdk_connect",
                        "sdk_write_enter","sdk_write_done","post_status","reply_size",
                        "reply_body","ack_hash_enter","ack_hash_done"}){
    corner_reset();pause_at(event);
    {MediaRetryNetworkScope scope(job);assert(!voice_upload_and_parse(job));}
    assert(resets==0&&recoveries==0&&!media_retry_network_active());clean();
    // A fresh user request on this worker must inherit neither cancellation
    // nor the old socket's mutex ownership even while pause remains latched.
    boundary_hook={};statuses.push_back(202);auto fresh=job;fresh.from_voice_sd=false;
    {MediaRetryNetworkScope scope(fresh);assert(!media_retry_network_active());assert(voice_upload_and_parse(fresh));}
    clean();assert(sent_headers.back().at("x-request-id")==job.voice.request_id);
  }
  std::puts("PASS 12 pause boundaries through real receipt hash reject uncertain acceptance, release actual mutex/DMA, and admit fresh request");

  for(const char* event:{"upload_ok","http_end","done"}){
    corner_reset();pause_at(event);
    {MediaRetryNetworkScope scope(job);assert(voice_upload_and_parse(job));}
    assert(ack_checks==1&&posts==1&&resets==0);clean();
  }
  // Proven acceptance is allowed to finish cleanup after the final decision.
  std::puts("PASS pause after validated acknowledgement retains proven success and completes exactly one unlock");

  std::mt19937 rng(170001);unsigned partial_cases=0;
  for(unsigned trial=0;trial<128;++trial){
    corner_reset();write_limit=1+(rng()%511);const unsigned stop_after=1+rng()%12;
    std::vector<uint8_t> bytes(8192);for(size_t n=0;n<bytes.size();++n)bytes[n]=n%251;
    pause_at("sdk_write_done",stop_after);
    {MediaRetryNetworkScope scope(job);SenseMediaRetryClient client;
      assert(client.write(bytes.data(),bytes.size())==0);
      assert(io_writes==stop_after&&stops==1&&client.stream_timeout==0);
      assert(wire_bytes.size()==write_limit*stop_after);
      assert(std::equal(wire_bytes.begin(),wire_bytes.end(),bytes.begin()));
      g_media_retry_user_paused=false;assert(client.write(bytes.data(),1)==0&&stops==1);
    }
    ++partial_cases;
  }
  for(size_t chunk:{1U,3U,255U,511U,512U}){
    corner_reset();write_limit=chunk;std::vector<uint8_t> bytes(4097,42);
    {MediaRetryNetworkScope scope(job);SenseMediaRetryClient client;assert(client.write(bytes.data(),bytes.size())==bytes.size());}
    assert(wire_bytes==bytes&&stops==0);
  }
  std::printf("PASS %u seeded partial-write cancellations preserve exact prefix, latch closed client, and bounded chunks; uncancelled short writes complete\n",partial_cases);

  for(const char* event:{"sdk_read_enter","sdk_read_done","sdk_bulk_read_enter","sdk_bulk_read_done","sdk_available"}){
    corner_reset();pause_at(event);
    {MediaRetryNetworkScope scope(job);SenseMediaRetryClient client;uint8_t bytes[3]={};
      if(strstr(event,"bulk"))assert(client.read(bytes,sizeof(bytes))==-1);
      else if(strstr(event,"available"))assert(client.available()==0);
      else assert(client.read()==-1);
      assert(stops==1&&client.stream_timeout==0&&!client.connected());
    }
  }
  std::puts("PASS pause inside partial read/available suppresses returned data and closes only on owner after SDK return");

  for(const char* event:{"sdk_connect","sdk_write_done","sdk_read_done","sdk_available"}){
    corner_reset();pause_at(event);
    {MediaRetryNetworkScope scope(job);task=2;
      {auto fresh=job;fresh.from_voice_sd=false;MediaRetryNetworkScope other(fresh);SenseMediaRetryClient client;
       assert(client.connect("host",443,23000)&&last_connect==23000);
       uint8_t bytes[513]{};assert(client.write(bytes,sizeof(bytes))==sizeof(bytes));
       assert(client.read()=='r'&&client.available()==4&&stops==0);}
      task=1;assert(media_retry_network_active());SenseMediaRetryClient saved;
      assert(saved.available()==0&&stops==1);
    }
    assert(!media_retry_network_active());
  }
  std::puts("PASS actual atomic owner scope isolates other-task foreground client and owner cancellation survives that scope exit");

  for(const char* event:{"lock_wait","start","http_begin","post_enter","sdk_connect","sdk_write_done","post_status","reply_body","http_end"}){
    corner_reset({200});reply_body="{}";pause_at(event);int code=0;String body;
    {MediaRetryNetworkScope scope(job);
      assert(!http_post_json_with_retries("https://test","payload",code,body,"IMAGE_PRESIGN",nullptr,nullptr,7,20000));}
    assert(resets==0&&recoveries==0);clean();
    boundary_hook={};statuses.push_back(200);int fresh_code=0;String fresh_body;
    assert(http_post_json_with_retries("https://test","fresh",fresh_code,fresh_body,"LIST",nullptr,nullptr,8,20000));clean();
  }
  std::puts("PASS presign pause at nine request/reply boundaries releases real mutex; immediate fresh HTTP succeeds");

  for(uint32_t start:{1U,UINT32_MAX-9,UINT32_MAX-19,UINT32_MAX-39}){
    corner_reset();clock_ms=start;
    {MediaRetryNetworkScope scope(job);assert(media_retry_network_wait(61));assert(uint32_t(clock_ms-start)==61);}
  }
  std::puts("PASS cancellable wait crosses uint32 rollover without extending deadline");
}
'''.replace('HASH_FIXTURE', digest)


def wifi_harness(root):
    source = wifi.harness(root / 'Sense_Minimal/sense_wifi.h').split('int main(){', 1)[0]
    source = replace_once(source,
        'template<class... A> void printf(const char*,A...){assert(!in_callback);++logs;}',
        'template<class... A> void printf(const char*,A...){assert(!in_callback);++logs;}\n'
        'template<class T> void print(const T&){}\n'
        'template<class T> void println(const T&){}')
    ensure = network.definition((root / 'Sense_Minimal/sense_wifi.h').read_text(),
                                'static bool ensure_wifi_connected(').replace('unsigned long', 'uint32_t')
    return source + r'''
static bool wifi_is_connected(){return WiFi.status()==WL_CONNECTED;}
static const char* wifi_guard_connect_owner(){return "test";}
static void wifi_diag_note_attempt(){}
static void apply_public_dns_for_api(const char*){}
''' + ensure + r'''
int main(){
  for(uint32_t start:{1U,UINT32_MAX-100000,UINT32_MAX-4000,UINT32_MAX-50}){
    for(unsigned failure=1;failure<8;++failure){
      reset();now_ms=start;http_mutex=(void*)1;tick();
      for(unsigned n=0;n<failure;++n){
        fail_attempt();tick(n==0?4000:n==1?8000:16000);
      }
      assert(begin_calls==failure+1&&reset_calls==std::min(failure,3U));
      assert(!mutex_held&&mutex_takes==mutex_gives);
    }
  }
  std::puts("PASS 28 reconnect timelines straddling wrap preserve reset cap, backoff and HTTP lease balance");
  reset();http_mutex=(void*)1;tick();fail_attempt();connect_on_claim=busy_on_claim=true;tick(4000);
  assert(reset_calls==0&&begin_calls==1&&wifi_maint_retry_pending&&!wifi_connect_inflight&&!mutex_held);
  http_inflight=false;tick();assert(!wifi_maint_retry_pending&&reset_calls==0&&wifi_maint_consecutive_fails==0);
  std::puts("PASS simultaneous connected status and busy-at-claim preserve pending work until live success clears it");

  // Exact zero start used to disable maintenance and poll timeouts. Verify
  // both entry paths and the inclusive 25-second association allowance.
  reset();now_ms=UINT32_MAX-19;tick();
  assert(wifi_inflight_start_ms==0&&wifi_connect_inflight&&begin_calls==1);
  tick(25000);assert(wifi_connect_inflight&&!wifi_maint_retry_pending);
  tick(1);assert(!wifi_connect_inflight&&wifi_maint_retry_pending&&begin_calls==1);
  tick(3999);assert(begin_calls==1);tick(1);assert(begin_calls==2&&reset_calls==1);
  reset();now_ms=0;assert(!ensure_wifi_connected("maintenance",0));
  assert(wifi_inflight_start_ms==0&&wifi_connect_inflight&&now_ms==50&&begin_calls==1);
  assert(!ensure_wifi_connected("ordinary",1000));assert(now_ms==50&&begin_calls==1);
  now_ms=25000;wifi_guard_poll();assert(wifi_connect_inflight);
  now_ms=25150;wifi_guard_poll();assert(!wifi_connect_inflight&&poll_failures==1);
  reset();wifi_connect_inflight=true;wifi_inflight_start_ms=0;now_ms=25150;
  live_status=WL_IDLE_STATUS;wifi_guard_poll();assert(!wifi_connect_inflight&&poll_timeouts==1);
  std::puts("PASS clock-zero maintenance and ordinary connection expire, avoid duplicate wait, retain 25s allowance and retry backoff");
}
'''


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--assert-zero-clock-fixed', action='store_true',
                        help='Run only the Wi-Fi cases, including the exact zero-clock regression')
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        parser.error('C++ compiler required')
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='halo-network-corners-') as directory:
        cases = [('wifi', wifi_harness)] if args.assert_zero_clock_fixed else [('network', network_harness), ('wifi', wifi_harness)]
        for name, build in cases:
            cpp, exe = Path(directory) / (name + '.cpp'), Path(directory) / name
            content = build(args.source_root)
            cpp.write_text(content)
            if args.out:
                shutil.copy2(cpp, args.out / cpp.name)
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-pthread',
                       '-Wno-deprecated-declarations', '-Wno-unused-variable', '-Wno-unused-function',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I', str(args.source_root), '-I', str(args.source_root / 'Sense_Minimal'),
                       '-I', str(args.source_root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
                       str(cpp), '-o', str(exe)]
            result = subprocess.run(command, capture_output=True, timeout=30, preexec_fn=no_core)
            output = result.stdout + result.stderr
            if result.returncode == 0:
                run = subprocess.run([str(exe)], capture_output=True, timeout=10, preexec_fn=no_core)
                output += run.stdout + run.stderr
                result = run
            if args.out:
                (args.out / (name + '.log')).write_bytes(output)
            print(output.decode(), end='')
            if result.returncode:
                raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()

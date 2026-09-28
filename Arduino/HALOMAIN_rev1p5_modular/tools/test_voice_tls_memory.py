#!/usr/bin/env python3
"""Voice TLS regression: real allocator/receipt/worker logic.

SDK transports/heaps are doubles. Production functions and allocator are compiled
under ASan/UBSan; these host checks make no physical RAM claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import test_fresh_upload_user_priority as fresh

ROOT = Path(__file__).resolve().parents[1]


def harness(root, mutation=None):
    code = fresh.harness(root).split('int main(){', 1)[0]
    replace = fresh.corners.replace_once
    allocator = root / 'halo_ota_demo/firmware/shared/ScopedTlsMemory.h'
    assert allocator.is_file()
    voice = fresh.corners.network.definition(code, 'static bool voice_upload_and_parse(')
    declarations = ('    halo_tls_memory::Scope tls_memory;\n'
                    '    sense_memory::VoiceTrace memory_trace(voice_job_id, attempt, tls_memory.active());\n'
                    '    SenseMediaRetryClient client;\n'
                    '    HTTPClient http;')
    assert declarations in voice, 'Requires the Scope -> Trace -> client -> HTTP ordering'
    if mutation == 'missing_scope':
        voice = replace(voice, '    halo_tls_memory::Scope tls_memory;\n', '')
        voice = voice.replace('tls_memory.active()', 'false')
    elif mutation == 'scope_after_client':
        voice = replace(voice, declarations,
                        '    sense_memory::VoiceTrace memory_trace(voice_job_id, attempt, true);\n'
                        '    SenseMediaRetryClient client;\n'
                        '    halo_tls_memory::Scope tls_memory;\n'
                        '    HTTPClient http;')
    elif mutation == 'scope_after_trace':
        voice = replace(voice, declarations,
                        '    sense_memory::VoiceTrace memory_trace(voice_job_id, attempt, true);\n'
                        '    halo_tls_memory::Scope tls_memory;\n'
                        '    SenseMediaRetryClient client;\n'
                        '    HTTPClient http;')
    elif mutation is not None:
        raise ValueError(mutation)
    original = fresh.corners.network.definition(code, 'static bool voice_upload_and_parse(')
    code = replace(code, original, voice)

    # Embed the unchanged allocator via the common harness; embed the actual
    # diagnostic and actual client without duplicate platform include headers.
    diag = (root / 'Sense_Minimal/sense_memory_diag.h').read_text()
    assert 'heap[PointCount][5]' in diag
    assert 'if (owner != Owner::Voice || voice_scope_active)' in diag
    for directive in ('#pragma once', '#include "../halo_ota_demo/firmware/shared/ScopedTlsMemory.h"',
                      '#include <esp_heap_caps.h>'):
        diag = replace(diag, directive, '')
    client = (root / 'Sense_Minimal/sense_media_retry_client.h').read_text()
    client = replace(client, '#pragma once', '')
    client = replace(client, '#include "sense_memory_diag.h"', '')
    client = replace(client, '#include "sense_media_network.h"', '#include "Sense_Minimal/sense_media_network.h"')
    diagnostics = r'''
static constexpr int ESP_OK=0;
static uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t target){assert(!target);return 4321;}
using FailureHook=void(*)(size_t,uint32_t,const char*);
static FailureHook failure_hook=nullptr;
static int heap_caps_register_failed_alloc_callback(FailureHook p){failure_hook=p;return 0;}
static size_t heap_caps_get_free_size(uint32_t caps){
 if(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))return host_tls_external_live.empty()?2000000:1990000;
 assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)||caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
 return 30000;
}
static size_t heap_caps_get_largest_free_block(uint32_t caps){
 assert(caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)||caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 return caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)?1000000:16000;
}
#define HALO_MEMORY_DIAGNOSTICS_TEST 1
#define HALO_SCOPED_TLS_MEMORY_TEST 1
'''
    code = replace(code, '#include "Sense_Minimal/sense_media_retry_client.h"',
                   diagnostics + diag + client)
    code = code.replace('#include <cassert>', '#include <cassert>\n#include <cstdarg>')
    state = r'''
static bool expect_voice_scope=true;
static unsigned voice_clients=0,http_clients=0,client_constructors=0,client_destructors=0;
static unsigned tls_allocations=0,tls_frees=0,trace_summaries=0,allocator_reports=0;
static std::set<void*> tls_live;
static std::string tls_diagnostics;
static void tls_check(bool ok,const char* label){
 if(!ok){fprintf(stderr,"VOICE_TLS_ASSERT %s\n",label);std::abort();}
}
static void* tls_request(size_t bytes){
 const bool owns=halo_tls_memory::detail::owner.load()==xTaskGetCurrentTaskHandle();
 tls_check(owns==expect_voice_scope,"allocation-scope-owner");
 void* p=halo_tls_memory::calloc(1,bytes);
 tls_check(p!=nullptr,"fallback-allocation-present");
 tls_check((host_tls_external_live.count(p)!=0)==(expect_voice_scope&&!host_tls_external_oom),"allocator-placement");
 for(size_t n=0;n<bytes;++n)tls_check(static_cast<uint8_t*>(p)[n]==0,"calloc-zeroed");
 ++tls_allocations;tls_live.insert(p);return p;
}
static void tls_release(void*& p){if(p){tls_check(tls_live.erase(p)==1,"unique-free");
 halo_tls_memory::free(p);p=nullptr;++tls_frees;}}
static void tls_foreign_and_nested_probe(){
 {halo_tls_memory::Scope nested;
  tls_check(nested.active()==expect_voice_scope,"nested-scope");
  void* p=tls_request(31);tls_release(p);}
 const unsigned saved=task;task=99;
 {halo_tls_memory::Scope other;tls_check(!other.active(),"foreign-scope-refused");
  void* p=halo_tls_memory::calloc(1,47);
  tls_check(p&&!host_tls_external_live.count(p),"foreign-task-default");halo_tls_memory::free(p);}
 task=saved;
 // A different task that already owns the scope may allocate while this voice
 // attempt falls back. Its counters must never be labeled voice deltas.
 const auto existing=halo_tls_memory::detail::owner.load();
 if(existing&&existing!=xTaskGetCurrentTaskHandle()){
  task=static_cast<unsigned>(reinterpret_cast<uintptr_t>(existing));
  void* p=halo_tls_memory::calloc(1,89);
  tls_check(p&&host_tls_external_live.count(p),"foreign-owner-counters-advanced");
  halo_tls_memory::free(p);task=saved;
 }
}
'''
    # The common SDK functions and real allocator precede Stream. State must
    # precede Serial so its diagnostic assertions can check final destruction.
    serial = 'static struct {\n  template<class... A>void printf(const char*,A...){}'
    assert code.count(serial) == 1
    allocator_begin = code.index('#include <atomic>\n#include <set>\n#include <cstdlib>\n#ifndef HALO_HOST_TLS_TASK_DEFINED')
    allocator_end = code.index('struct Stream {', allocator_begin)
    allocator_code = code[allocator_begin:allocator_end]
    code = code[:allocator_begin] + code[allocator_end:]
    code = replace(code, serial, allocator_code + state + r'''static struct {
  template<class... A>void printf(const char* format,A... args){
   char line[1024];snprintf(line,sizeof(line),format,args...);
   if(!strncmp(line,"[VOICE_MEM]",11)||!strncmp(line,"[TLS_ALLOC]",11)||!strncmp(line,"[ALLOC_FAIL]",12)||!strncmp(line,"[WORKER_STACK]",14)){
    tls_check(voice_clients==0&&http_clients==0&&tls_live.empty(),"report-after-client-destruction");
    const bool owns=halo_tls_memory::detail::owner.load()==xTaskGetCurrentTaskHandle();
    tls_check(owns==expect_voice_scope,"report-before-scope-release");
    if(strstr(line,"tls_bytes="))++trace_summaries;
    if(!strncmp(line,"[TLS_ALLOC]",11))++allocator_reports;
    tls_diagnostics+=line;
   }
  }''')
    code = replace(code, '  virtual ~WiFiClientSecure()=default;', r'''
  void* tls_context=nullptr;void* tls_records=nullptr;
  WiFiClientSecure(){++voice_clients;++client_constructors;tls_context=tls_request(512);}
  virtual ~WiFiClientSecure(){
   tls_check(http_clients==0,"http-before-client-destruction");stop();
   void* cleanup=tls_request(127);tls_release(cleanup);tls_release(tls_context);
   --voice_clients;++client_destructors;
  }''')
    code = replace(code, '  void stop(){assert(!in_sdk);++stops;}',
                   '  void stop(){assert(!in_sdk);++stops;tls_release(tls_records);}')
    code = replace(code, 'in_sdk=true;last_connect=timeout;boundary("sdk_connect");in_sdk=false;return transport_ok;',
                   'tls_records=tls_request(2048);tls_foreign_and_nested_probe();\n'
                   '    in_sdk=true;last_connect=timeout;boundary("sdk_connect");in_sdk=false;return transport_ok;')
    code = replace(code, 'struct HTTPClient {', r'''struct HTTPClient {
  HTTPClient(){++http_clients;}
  ~HTTPClient(){tls_check(voice_clients==1,"client-outlives-http");--http_clients;}
''')
    code = replace(code, 'static void camera_dma_reserve_acquire(const char*){++dma_acquires;}',
                   'static void camera_dma_reserve_acquire(const char*){\n'
                   ' tls_check(voice_clients==0&&http_clients==0&&tls_live.empty(),"dma-after-client-cleanup");++dma_acquires;}')
    return code + TESTS


TESTS = r'''
static unsigned cases=0;
static void tls_reset(std::vector<int> codes={202},bool psram=true,bool fail_external=false){
 tls_check(tls_live.empty()&&host_tls_external_live.empty(),"no-retained-heap-between-cases");
 tls_check(!halo_tls_memory::detail::owner.load(),"no-retained-scope-between-cases");
 fresh_reset(codes);halo_tls_memory::initialize(psram);expect_voice_scope=psram;
 host_tls_external_oom=fail_external;
 voice_clients=http_clients=client_constructors=client_destructors=0;
 tls_allocations=tls_frees=trace_summaries=allocator_reports=0;tls_diagnostics.clear();
}
static void tls_closed(const UploadJob& job){
 released();++cases;
 tls_check(voice_clients==0&&http_clients==0&&tls_live.empty()&&host_tls_external_live.empty(),"all-tls-buffers-retired");
 tls_check(client_constructors==client_destructors,"all-clients-destroyed");
 if(client_constructors){
  const char* q=expect_voice_scope?"scope_active=1 counters_qualified=1":"scope_active=0 counters_qualified=0";
  tls_check(tls_diagnostics.find(q)!=std::string::npos,"allocator-report-qualified");
  if(!expect_voice_scope)tls_check(tls_diagnostics.find("external_calls=")==std::string::npos,"inactive-no-deltas");
 }
 tls_check(trace_summaries==client_constructors&&allocator_reports==client_constructors,"one-report-per-attempt");
 tls_check(tls_allocations==tls_frees,"balanced-tls-allocations");
 for(size_t i=0;i<sent_headers.size();++i){
  tls_check(sent_headers[i].at("x-request-id")==job.voice.request_id,"immutable-request");
  tls_check(sent_headers[i].at("x-session-id")==job.voice.session_id,"immutable-session");
  tls_check(sent_headers[i].at("x-owner-id")==job.voice.owner_id,"immutable-owner");
  tls_check(sent_payloads[i]==std::vector<uint8_t>(job.image_buf,job.image_buf+job.image_len),"exact-pcm-payload");
 }
 if(client_constructors){
  tls_check(tls_diagnostics.find("owner=VOICE_MEM")!=std::string::npos,"voice-allocator-report");
  tls_check(tls_diagnostics.find("psram_free=")!=std::string::npos&&tls_diagnostics.find("psram_largest=")!=std::string::npos,"phase-psram-metrics");
  tls_check(tls_diagnostics.find("qualified=1 min_free_bytes=4321 since=task_start")!=std::string::npos,"worker-stack-bytes-after-client-cleanup");
 }
}
static bool run_voice(const UploadJob& job){
 MediaRetryNetworkScope scope(job);
 const bool accepted=voice_upload_and_parse(job);actual_worker_voice_dispatch(job,accepted);return accepted;
}
int main(){
 sense_memory::begin();sense_memory::bind_upload_worker();auto job=fixture();
 for(unsigned mode=0;mode<5;++mode){
  tls_reset({202},mode!=1,mode==2);
  const auto before=halo_tls_memory::snapshot();
  if(mode==3){
   task=2;{halo_tls_memory::Scope foreign;tls_check(foreign.active(),"foreign-owner-fixture");
    task=1;expect_voice_scope=false;tls_check(run_voice(job),"foreign-default-success");
    tls_closed(job);tls_check(halo_tls_memory::detail::owner.load()==reinterpret_cast<void*>(2),"foreign-owner-preserved");task=2;
   }task=1;
  }else if(mode==4){
   {halo_tls_memory::Scope outer;tls_check(outer.active(),"outer-owner-fixture");
    tls_check(run_voice(job),"nested-success");tls_closed(job);
    tls_check(halo_tls_memory::detail::owner.load()==xTaskGetCurrentTaskHandle(),"outer-owner-preserved");}
  }else{tls_check(run_voice(job),"normal-disabled-fallback-success");tls_closed(job);}
  const auto after=halo_tls_memory::snapshot();
  tls_check(delivered==1&&worker_freed==1&&persisted==0&&ack_checks==1,"accepted-custody");
  if(mode==0||mode==4)tls_check(after.external_calls>before.external_calls&&after.default_fallbacks==before.default_fallbacks,"normal-preference-counts");
  if(mode==1)tls_check(after.external_calls==before.external_calls,"disabled-default-counts");
  if(mode==3)tls_check(after.external_calls>before.external_calls,"foreign-counter-noise-present");
  if(mode==2){
   tls_check(after.external_failures>before.external_failures&&after.default_fallbacks>before.default_fallbacks,"forced-fallback-counts");
   tls_check(tls_diagnostics.find("default_fallbacks="+std::to_string(after.default_fallbacks-before.default_fallbacks)+" scope_active=1 counters_qualified=1")!=std::string::npos,"qualified-fallback-counts-reported");
  }
  tls_check(!halo_tls_memory::detail::owner.load(),"post-case-owner-retired");
 }
 for(int first:{408,429,500,502,503,504,599}){
  tls_reset({first,202});tls_check(run_voice(job),"transient-retry-success");tls_closed(job);
  tls_check(client_constructors==2&&posts==2&&resets==0&&delivered==1,"bounded-transient-retry");
  tls_check(sent_headers[0]==sent_headers[1]&&sent_payloads[0]==sent_payloads[1],"retry-identical-request");
 }
 tls_reset({-1,202});tls_check(run_voice(job),"transport-retry-success");tls_closed(job);
 tls_check(posts==2&&resets==1&&client_constructors==2,"fresh-reset-policy-retained");
 for(int code:{400,401,403,404,409,422}){
  tls_reset({code});tls_check(!run_voice(job),"permanent-response-fails");tls_closed(job);
  tls_check(posts==1&&delivered==0&&persisted==1,"permanent-custody-retained");
 }
 tls_reset({503,503});tls_check(!run_voice(job),"retry-exhaustion");tls_closed(job);
 tls_check(posts==2&&persisted==1&&delivered==0,"exhausted-custody-retained");
 tls_reset();begin_ok=false;tls_check(!run_voice(job),"begin-failure");tls_closed(job);
 tls_check(posts==0&&client_constructors==2&&persisted==1,"begin-failure-cleanup");
 for(const char* bad:{"{}",R"({"accepted":false})",R"({"accepted":true,"async":true,"duplicate":false,"jobId":"wrong"})"}){
  tls_reset();reply_body=bad;tls_check(!run_voice(job),"malformed-ack-refused");tls_closed(job);
  tls_check(delivered==0&&persisted==1,"malformed-ack-keeps-custody");
 }
 for(unsigned bad=0;bad<4;++bad){
  tls_reset();auto invalid=job;
  if(bad==0)identity_ok=false;if(bad==1)age_ok=false;
  if(bad==2)invalid.image_len=0;if(bad==3)invalid.voice.crc32=99;
  {MediaRetryNetworkScope scope(invalid);tls_check(!voice_upload_and_parse(invalid),"early-admission-refused");}
  tls_closed(invalid);tls_check(client_constructors==0&&posts==0,"admission-before-client");
 }
 for(const char* event:{"lock_wait","http_begin","post_enter","sdk_connect","sdk_write_enter",
                       "sdk_write_done","post_status","reply_size","reply_body","ack_hash_enter","ack_hash_done"}){
  tls_reset();input_at(event);tls_check(!run_voice(job),"foreground-cancels-voice");tls_closed(job);
  tls_check(delivered==0&&persisted==0&&worker_freed==0&&upload_worker_parked_pending(),"cancel-parks-exact-job");
  tls_check(upload_worker_parked_job.image_buf==job.image_buf&&upload_worker_parked_job.job_id==job.job_id&&
            !memcmp(&upload_worker_parked_job.voice,&job.voice,sizeof(job.voice)),"cancel-frozen-custody");
 }
 tls_reset({202,202});input_at("post_status");
 tls_check(!run_voice(job),"park-then-resume-initial-cancel");tls_closed(job);
 UploadJob resumed{};tls_check(upload_worker_take_parked_job(resumed),"parked-job-taken-once");
 tls_check(!memcmp(&resumed,&job,sizeof(job)),"resumed-exact-descriptor");boundary_hook={};
 tls_check(run_voice(resumed),"resumed-request-accepted");tls_closed(resumed);
 tls_check(posts==2&&delivered==1&&worker_freed==1&&persisted==0&&!upload_worker_parked_pending(),"resume-sole-terminal-custody");
 tls_reset({503,202});cancel_delay=20;
 {MediaRetryNetworkScope scope(job);
  tls_check(!voice_upload_and_parse(job),"cancel-during-backoff");
  tls_check(posts==1&&delay_ms==20,"backoff-20ms-priority-boundary");
  actual_worker_voice_dispatch(job,false);
 }
 tls_closed(job);
 tls_check(upload_worker_parked_pending()&&resets==0,"backoff-park-without-reset");
 for(int status:{-1,503}){
  tls_reset({status,status});auto saved=job;saved.from_voice_sd=true;
  tls_check(!run_voice(saved),"saved-retry-failure");tls_closed(saved);
  tls_check(deleted==0&&delivered==0&&resets==0,"saved-custody-not-deleted");
 }
 tls_reset({202});auto saved=job;saved.from_voice_sd=true;
 tls_check(run_voice(saved),"saved-accepted");tls_closed(saved);
 tls_check(deleted==1&&delivered==1&&persisted==0,"saved-delete-after-ack");
 printf("PASS voice TLS: %u cases; actual allocator, receipt/custody, Scope/Trace/client/HTTP order, retries and cancellation; no physical RAM claim\n",cases);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mutation', choices=['missing_scope', 'scope_after_client', 'scope_after_trace'])
    args = parser.parse_args()
    root = args.source_root.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    source = harness(root, args.mutation)
    (args.out / 'test.cpp').write_text(source)
    compiler = shutil.which('clang++') or shutil.which('g++')
    assert compiler
    command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Wno-deprecated-declarations',
               '-Wno-unused-variable', '-Wno-unused-function', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-pthread', '-I', str(root),
               '-I', str(root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
               str(args.out / 'test.cpp'), '-o', str(args.out / 'test')]
    built = subprocess.run(command, capture_output=True, text=True, timeout=45)
    (args.out / 'compile.log').write_text(built.stdout + built.stderr)
    ran = subprocess.run([str(args.out / 'test')], capture_output=True, text=True, timeout=15,
                         preexec_fn=fresh.corners.network.no_core) if built.returncode == 0 else None
    output = (ran.stdout + ran.stderr) if ran else ''
    (args.out / 'test.log').write_text(output)
    expected = {'missing_scope': 'allocation-scope-owner', 'scope_after_client': 'allocation-scope-owner',
                'scope_after_trace': 'report-before-scope-release'}
    negative_ok = bool(args.mutation and ran and ran.returncode != 0 and
                       'VOICE_TLS_ASSERT ' + expected[args.mutation] in output and
                       'AddressSanitizer' not in output and 'runtime error:' not in output)
    passed = negative_ok if args.mutation else bool(ran and ran.returncode == 0)
    files = ['Sense_Minimal/sense_voice.h', 'Sense_Minimal/sense_memory_diag.h',
             'Sense_Minimal/sense_media_retry_client.h',
             'halo_ota_demo/firmware/shared/ScopedTlsMemory.h']
    result = {'status': 'PASS' if passed else 'FAIL', 'qualification': 'HOST_REGRESSION_NO_PHYSICAL_RAM_CLAIM',
              'mutation': args.mutation, 'compiled': built.returncode == 0,
              'exit_code': ran.returncode if ran else None, 'hardware_commands': 0,
              'source_files': {f: hashlib.sha256((root / f).read_bytes()).hexdigest() for f in files}}
    (args.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result));print(output)
    if built.returncode:print(built.stderr)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())

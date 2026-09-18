#!/usr/bin/env python3
"""Actual sleep/worker admission with deterministic job ownership and time.

The three-job timing is modeled from hardware164-burst002. Storage/worker
processing are explicit doubles; this does not claim UART or device coverage.
Use --negative-root with the immutable164 snapshot to reproduce its lost job.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    sleep = (root / 'Sense_Minimal/sense_sleep.h').read_text()
    ino = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    block = definition(sleep[sleep.index('// --- Upload flush window:'):], '{')
    cancel = definition(sleep, 'static bool sleep_upload_flush_yield_to_user(')
    gate = definition(ino, 'static bool sense_can_sleep_now(')
    hold = definition(ino, 'static bool uploads_held_for_session(')
    background = '\n'.join(definition(ino, signature) for signature in (
        'static bool sleep_reason_is_background_deferable(',
        'static void sleep_background_force_reset(',
        'static bool sleep_background_force_ready('))
    claim = definition(ino, 'class UploadWorkerClaim') + ';' if 'class UploadWorkerClaim' in ino else ''
    worker = definition(ino, 'static void upload_worker_task(')
    start = worker.index('for (;;) {') + len('for (;;) {')
    admission = worker[start:worker.index('    if (got_job) {', start)]
    activity = (root / 'Sense_Minimal/sense_user_activity.h').read_text()
    return r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>
#ifndef HALO_DEFER_UPLOADS_TO_SLEEP
#define HALO_DEFER_UPLOADS_TO_SLEEP 1
#endif
''' + activity + r'''
static unsigned long clock_ms=1000;
static unsigned long millis(){return clock_ms;}
static std::function<void(unsigned long)> advance;
static void delay(unsigned long n){if(advance)advance(n);else clock_ms+=n;}
static void vTaskDelay(unsigned long n){delay(n);}
static unsigned long pdMS_TO_TICKS(unsigned long n){return n;}
struct SerialType {template<class... A>void printf(const char*,A...){}void println(const char*){}} Serial;
struct UploadJob {uint32_t job_id=0;char mode[16]="discard";uint8_t* image_buf=nullptr;size_t image_len=1;bool is_voice=false;bool from_voice_sd=false,from_image_sd=false,from_persisted=false;};
static std::vector<UploadJob> queue;
static void* upload_queue=&queue;
static uint32_t upload_queue_count(){return queue.size();}
static bool guardian_force_sleep=false,g_upload_flush_requested=false,upload_inflight=false;
static bool upload_worker_holding_in_place=false,upload_worker_has_parked_job=false;
static UploadJob parked_job;
static std::atomic<bool> upload_worker_claim_active{false};
static std::atomic<bool> g_media_retry_user_paused{false};
static std::atomic<uint32_t> g_voice_list_attempted_job{0};
static std::atomic<uint32_t> g_media_custody_waiters{0};
static bool g_spool_owns_uart=false,g_img_spool_tx_active=false,g_spool_drain_wake=false;
static bool http_inflight=false,background_sleep_bypass_active=false,g_list_screen_active=false;
static bool list_refresh_inflight=false;
static unsigned long list_refresh_start_ms=0;
static const unsigned long LIST_REFRESH_BLOCK_MAX_MS=10000;
static unsigned long background_sleep_block_start_ms=0;
static const unsigned long PRE_SLEEP_BLOCK_MAX_MS=10000;
static bool sleep_block_should_log(const char*,const char*,const char*){return false;}
static bool sense_ntp_attempt_pending(){return false;}
static bool upload_worker_parked_pending(){return upload_worker_has_parked_job;}
static bool upload_worker_peek_parked_job(UploadJob& out){
 if(!upload_worker_has_parked_job)return false;out=parked_job;return true;
}
// These sleep cases never open the interactive shopping-list voice path.
static bool voice_list_pending(){return false;}
static bool upload_worker_take_parked_job(UploadJob& out,const char** stage=nullptr,unsigned long* at=nullptr){
 if(!upload_worker_has_parked_job)return false;
 out=parked_job;upload_worker_has_parked_job=false;if(stage)*stage="test";if(at)*at=0;return true;
}
static const int pdTRUE=1;
static int xQueuePeek(void*,UploadJob* out,int){if(queue.empty())return 0;*out=queue.front();return pdTRUE;}
static bool foreground_priority_active(unsigned long,const char**){return false;}
static std::function<void()> after_take;
static int xQueueReceive(void*,UploadJob* out,int){if(queue.empty())return 0;*out=queue.front();queue.erase(queue.begin());if(after_take)after_take();return pdTRUE;}
static unsigned denials=0,pumps=0,teardowns=0,main_saves=0,parallel_attempts=0;
static std::set<unsigned> committed,freed,lost;
static std::vector<std::string> notes;
static std::function<void()> on_pump;
static void pump_uart_rx_once(){++pumps;if(on_pump)on_pump();}
static void uart_send_sense_diag(const char*,const char* event,const char*,int32_t,const char*){notes.push_back(event);}
static void sleep_notify_late_block(const char*){++denials;}
static void record_free(void* p){unsigned id=(unsigned)(uintptr_t)p;assert(!freed.count(id));freed.insert(id);if(!committed.count(id))lost.insert(id);}
#define free record_free
// Models current local-admission refusal while the worker owns SD: two calls
// separated by500ms, no successful custody. This is the observed164 failure.
static bool upload_persist_handle_failure(const UploadJob& job,const char*){
 ++main_saves;
 if(upload_inflight){parallel_attempts+=2;delay(500);return false;}
 committed.insert(job.job_id);delay(20);return true;
}
static unsigned background_drains=0;
static void sleep_defer_queued_background_uploads(){++background_drains;while(!queue.empty()){record_free(queue.front().image_buf);queue.erase(queue.begin());}}
static unsigned long g_upload_hold_since_ms=0;
static const unsigned long UPLOAD_HOLD_MAX_MS=600000;
static const uint32_t UPLOAD_HOLD_HIGHWATER=8;
''' + claim + '\n' + gate + '\n' + hold + '\n' + cancel + '\n' + background + r'''
static void run_flush(){
 const uint32_t sleep_user_generation=sense_user_action_generation();
''' + block + r'''
 // Same final ordinary sleep admission used before production Wi-Fi teardown.
 if(sense_can_sleep_now(nullptr))++teardowns;
}
static void actual_worker_admission_once(){
''' + admission + r'''
 if(got_job){assert(upload_worker_claim_active.load());committed.insert(job.job_id);free(job.image_buf);}
}
static UploadJob job(unsigned id){UploadJob j;j.job_id=id;j.image_buf=(uint8_t*)(uintptr_t)id;return j;}
static bool has_note(const char* x){return std::find(notes.begin(),notes.end(),x)!=notes.end();}
static unsigned active_job=0;static unsigned long complete_at=0;
static void worker_time(unsigned long n){
 for(unsigned long i=0;i<n;++i){++clock_ms;
  if(active_job&&clock_ms>=complete_at){committed.insert(active_job);record_free((void*)(uintptr_t)active_job);active_job=0;upload_inflight=false;g_media_custody_waiters=0;upload_worker_claim_active=false;}
  if(!active_job&&g_upload_flush_requested&&!queue.empty()){
   upload_worker_claim_active=true;auto j=queue.front();queue.erase(queue.begin());active_job=j.job_id;upload_inflight=true;g_media_custody_waiters=1;
   complete_at=clock_ms+(active_job==36?37000:19000);
  }
 }
}
int main(int argc,char**argv){assert(argc==2);std::string test=argv[1];auto start=millis();
 if(test=="burst"||test=="old_burst"){
  queue={job(36),job(52),job(63)};advance=worker_time;run_flush();
  assert(millis()-start>=45000&&millis()-start<46000);
  if(test=="old_burst")assert(lost==std::set<unsigned>{63}&&parallel_attempts==2&&main_saves==1);
  else assert(lost.empty()&&queue.size()==1&&queue[0].job_id==63&&main_saves==0&&has_note("flush_defer"));
  assert(!g_upload_flush_requested&&!teardowns);
  delay(22000);assert(!upload_inflight);run_flush();
  if(test=="old_burst")assert(committed==std::set<unsigned>({36,52})&&lost.size()==1);
  else assert(committed==std::set<unsigned>({36,52,63})&&lost.empty()&&parallel_attempts==0&&queue.empty());
  assert(!g_upload_flush_requested&&teardowns==1);
 }else if(test=="background"||test=="background_legacy"){
  queue={job(63)};assert(!sleep_background_force_ready(millis(),"wifi_connect","test"));
  delay(10000);assert(sleep_background_force_ready(millis(),"wifi_connect","test"));
  assert(background_sleep_bypass_active&&!g_upload_flush_requested);
  if(test=="background_legacy")assert(background_drains==1&&queue.empty());
  else{
   assert(background_drains==0&&queue.size()==1&&freed.empty());
   advance=worker_time;run_flush();assert(committed.count(63)&&queue.empty()&&lost.empty()&&teardowns==1);
  }
  sleep_background_force_reset();assert(!background_sleep_bypass_active&&!background_sleep_block_start_ms);
 }else if(test=="claim_gap"){
  queue={job(63)};g_upload_flush_requested=true;bool observed=false;
  after_take=[&]{observed=true;assert(queue.empty()&&!upload_inflight);const char* why=nullptr;assert(!sense_can_sleep_now(&why));assert(!strcmp(why,"upload_worker_claim"));};
  actual_worker_admission_once();assert(observed&&!upload_worker_claim_active.load()&&lost.empty());
  assert(committed.count(63)&&freed.count(63));
 }else if(test=="claim_idle"){
  actual_worker_admission_once();assert(!upload_worker_claim_active.load());
  queue={job(63)};actual_worker_admission_once();assert(!upload_worker_claim_active.load()&&queue.size()==1);
 }else if(test=="empty"){
  run_flush();assert(millis()==start&&teardowns==1&&main_saves==0&&!g_upload_flush_requested);
 }else if(test=="user"){
  queue={job(63)};bool sent=false;on_pump=[&]{if(!sent&&millis()-start>=2700){sent=true;sense_note_admitted_user_action();}};
  run_flush();assert(sent&&millis()-start==2700&&queue.size()==1&&!teardowns&&!main_saves&&has_note("flush_cancel")&&!g_upload_flush_requested);
 }else if(test=="guardian"){
  queue={job(63)};guardian_force_sleep=true;run_flush();assert(millis()-start==30000&&teardowns==1&&queue.size()==1&&main_saves==0&&lost.empty()&&has_note("flush_forced"));
 }else{
  if(test=="queued")queue={job(63)};
  else if(test=="parked"){parked_job=job(63);upload_worker_has_parked_job=true;}
  else if(test=="inflight")upload_inflight=true;
  else if(test=="held")upload_worker_holding_in_place=true;
  else if(test=="claim")upload_worker_claim_active=true;
  else assert(false);
  run_flush();assert(millis()-start==30000&&!teardowns&&!main_saves&&freed.empty()&&has_note("flush_defer")&&!g_upload_flush_requested);
  if(test=="queued")assert(queue.size()==1);
  if(test=="parked")assert(upload_worker_has_parked_job);
 }
 puts("PASS");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--negative-root', type=Path)
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    cases = ['burst', 'background', 'claim_gap', 'claim_idle', 'empty', 'user', 'guardian',
             'queued', 'parked', 'inflight', 'held', 'claim']
    runs = [(args.source_root, cases)]
    if args.negative_root:
        runs.append((args.negative_root, ['old_burst']))
    for root, selected in runs:
        with tempfile.TemporaryDirectory(prefix='halo-flush-custody-') as tmp:
            path = Path(tmp); (path/'test.cpp').write_text(harness(root))
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra',
                                     '-Wno-unused-function', str(path/'test.cpp'), '-o', str(path/'test')],
                                    text=True, capture_output=True, timeout=30)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            for name in selected:
                result = subprocess.run([str(path/'test'), name], text=True, capture_output=True, timeout=5)
                if result.returncode:
                    raise RuntimeError(name + ': ' + result.stdout + result.stderr)
                print('PASS', name, flush=True)
            if root == args.source_root:
                result = subprocess.run([compiler, '-std=c++17', '-DHALO_DEFER_UPLOADS_TO_SLEEP=0',
                                         str(path/'test.cpp'), '-o', str(path/'legacy')],
                                        text=True, capture_output=True, timeout=30)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                result = subprocess.run([str(path/'legacy'), 'background_legacy'],
                                        text=True, capture_output=True, timeout=5)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                print('PASS background_legacy', flush=True)
    print('PASS actual-source worker custody; hardware/storage timing are modeled')


if __name__ == '__main__':
    main()

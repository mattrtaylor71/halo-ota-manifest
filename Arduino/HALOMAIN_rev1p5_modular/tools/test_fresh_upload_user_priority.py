#!/usr/bin/env python3
"""Exercise actual fresh-upload scope, socket client, receipt and custody dispatch.

Uses the production network-corner harness (real HTTP functions and receipt hash),
with SDK/NVS boundaries doubled. New input is injected before/during/after I/O;
this proves owner-local cleanup, not hardware DNS/TLS cancellation wall time.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

import test_media_network_corner_cases as corners

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    source = corners.network_harness(root).split('int main(){', 1)[0]
    source = corners.replace_once(source,
        'if(cancel_delay&&delay_ms>=cancel_delay)paused=true;',
        'if(cancel_delay&&delay_ms>=cancel_delay)sense_note_admitted_user_action();')
    source = corners.replace_once(source,
        'static void scan_ui_status_emit(const char*,const char*,const char*,uint32_t,bool){}',
        'static void scan_ui_status_emit(const char*,const char*,const char*,uint32_t,bool);')
    if 'static bool media_upload_network_active(' not in source:
        # Older source has no fresh-worker predicate; retain its saved-only
        # semantics so a negative run fails behavior, rather than compilation.
        source += '\nstatic bool media_upload_network_active(){return media_retry_network_active();}\n'
    source += r'''
static unsigned checks=0;
#undef assert
#define assert(x) do { ++checks; if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);abort();} } while(0)
static unsigned persisted=0,delivered=0,deleted=0;
static unsigned worker_freed=0,worker_errors=0,phase_continued=0,spool_success=0,spool_failure=0;
static uint16_t g_cycle_uploads_fail=0,g_cycle_uploads_ok=0;
static bool upload_inflight=false;
static std::atomic<bool> g_voice_list_followup{false};
static UploadJob retained={};
static std::vector<uint8_t> retained_bytes;
static unsigned g_upload_persist_cached_count=0;
static void media_retry_delivered(){++delivered;}
static bool sense_voice_spool_delete(const UploadJob&){++deleted;return true;}
static void upload_persist_delete_voice(const UploadJob&){++deleted;}
static void upload_persist_note_event(const char*,const char*,unsigned,unsigned){}
static void dump_system_truth(const char*){}
static void upload_persist_handle_failure(const UploadJob& job,const char* reason){
  assert(!strcmp(reason,"voice_post_fail")||!strcmp(reason,"presign_timeout")||
         !strcmp(reason,"presign_fail")||!strcmp(reason,"put_timeout")||!strcmp(reason,"put_fail"));
  ++persisted;retained=job;
  retained_bytes.assign(job.image_buf,job.image_buf+job.image_len);
}
static void worker_free(void* p){assert(p);++worker_freed;}
static void scan_ui_status_emit(const char*,const char*,const char*,uint32_t,bool){++worker_errors;}
static const char* presign_error_text(){return "fixture failure";}
static void diag_record_action_event(const char*,const char*,const char*,const char*,int){}
static void sense_spool_on_upload_result(uint32_t,bool ok){if(ok)++spool_success;else ++spool_failure;}
static bool sense_image_spool_delete(const UploadJob&){++deleted;return true;}
static void upload_persist_delete(){++deleted;}
#include <mutex>
#include <thread>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static void portENTER_CRITICAL(std::mutex* mux){mux->lock();}
static void portEXIT_CRITICAL(std::mutex* mux){mux->unlock();}
static uint32_t upload_queue_count(){return 0;}
'''
    worker = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    op = (root / 'Sense_Minimal/sense_op_queue.h').read_text()
    source += op[op.index('static bool upload_worker_has_parked_job ='):
                 op.index('static bool park_upload_job_if_foreground_active(')]
    helper = (corners.network.definition(worker, 'auto park_cancelled_fresh =') + ';\n'
              if 'auto park_cancelled_fresh =' in worker else
              'auto park_cancelled_fresh = [&](const char*){return false;};\n')
    source += '#define free worker_free\nstatic void actual_worker_voice_dispatch(const UploadJob& job,bool accepted){const bool list_voice=media_voice_list_active();do{\n' + helper
    start = worker.index('        if (accepted) {', worker.index('static void upload_worker_task('))
    end = worker.index('        continue;', worker.index('        free(job.image_buf);', start)) + len('        continue;')
    source += worker[start:end] + '\n}while(false);}\n'
    presign_at = worker.index('uint32_t presign_deadline_ms =', start)
    begin = worker.index('\n      if (budget_exhausted) {', presign_at)
    if '      if ((!presign_success || budget_exhausted)' in worker:
        begin = worker.index('      if ((!presign_success || budget_exhausted)', presign_at)
    end = worker.index('\n      if (is_dish) {', begin)
    source += 'static void actual_worker_presign(const UploadJob& job,bool presign_success,bool budget_exhausted){const bool is_dish=true;do{\n' + helper
    source += worker[begin:end] + '\n++phase_continued;\n}while(false);}\n'
    put_at = worker.index('uint32_t put_deadline_ms =', end)
    begin = worker.index('\n      if (budget_exhausted) {', put_at)
    if '      if (!upload_success && park_cancelled_fresh(' in worker:
        begin = worker.index('      if (!upload_success && park_cancelled_fresh(', put_at)
    end = worker.index('      free(job.image_buf);', worker.index('      diag_record_action_event("upload", job.mode, "ok"', begin)) + len('      free(job.image_buf);')
    source += 'static void actual_worker_put(const UploadJob& job,bool upload_success,bool budget_exhausted){const bool is_dish=true;do{\n' + helper
    source += worker[begin:end] + '\nupload_inflight=false;\n}while(false);}\n#undef free\n'
    return source + r'''
static void fresh_reset(std::vector<int> codes={202}){
  corner_reset(codes);g_sense_user_action_generation=17;
  persisted=delivered=deleted=0;retained={};retained_bytes.clear();
  worker_freed=worker_errors=phase_continued=spool_success=spool_failure=0;
  g_cycle_uploads_fail=g_cycle_uploads_ok=0;upload_inflight=true;
  upload_worker_has_parked_job=false;upload_worker_parked_job={};
}
static void input_at(const char* event,unsigned occurrence=1){
 boundary_hook=[event,occurrence,count=0U](const char* name)mutable{
  if(!strcmp(event,name)&&++count==occurrence)sense_note_admitted_user_action();
 };
}
static void released(){clean();assert(!g_media_retry_network_owner.load());assert(!media_upload_network_active());}
static void fail_worker_phase(const UploadJob& job,unsigned phase){
 if(phase==0)actual_worker_voice_dispatch(job,false);
 else if(phase<3)actual_worker_presign(job,false,phase==2);
 else actual_worker_put(job,false,phase==4);
}
int main(){
 auto job=fixture();
 for(bool prior_pause:{false,true}){
  fresh_reset();g_media_retry_user_paused=prior_pause;
  {MediaRetryNetworkScope scope(job);
   assert(!media_retry_network_active()&&!media_retry_network_cancelled());
   bool accepted=voice_upload_and_parse(job);assert(accepted);actual_worker_voice_dispatch(job,accepted);
   assert(production_outer_attempts()==3); // saved policy was not extended to fresh jobs
  }
  assert(delivered==1&&persisted==0&&deleted==0&&posts==1);released();
 }
 puts("PASS fresh accepted upload ignores older pause and retains fresh retry policy");
 for(const char* event:{"lock_wait","start","http_begin","post_enter","sdk_connect",
                       "sdk_write_enter","sdk_write_done","post_status","reply_size",
                       "reply_body","ack_hash_enter","ack_hash_done"}){
  fresh_reset();g_media_retry_user_paused=true;input_at(event);
  {MediaRetryNetworkScope scope(job);
   bool accepted=voice_upload_and_parse(job);assert(!accepted);actual_worker_voice_dispatch(job,accepted);
   assert(media_retry_network_cancelled());
  }
  assert(delivered==0&&deleted==0&&persisted==0&&resets==0&&worker_freed==0&&!upload_inflight);
  assert(upload_worker_has_parked_job&&!memcmp(&upload_worker_parked_job,&job,sizeof(job)));
  UploadJob resumed{};assert(upload_worker_take_parked_job(resumed)&&resumed.image_buf==job.image_buf);
  assert(!memcmp(&resumed.voice,&job.voice,sizeof(job.voice))&&!upload_worker_parked_pending());released();
  // An uncertain remote accept is retained with the same identity. A later
  // session may retry safely, without the earlier gesture cancelling it.
  boundary_hook={};statuses.push_back(202);
  {MediaRetryNetworkScope next(job);assert(voice_upload_and_parse(job));}
  released();
 }
 puts("PASS 12 new-input boundaries park exact fresh RAM custody before storage, close ownership and permit later retry");
 for(const char* event:{"upload_ok","http_end","done"}){
  fresh_reset();input_at(event);
  {MediaRetryNetworkScope scope(job);bool accepted=voice_upload_and_parse(job);
   assert(accepted);actual_worker_voice_dispatch(job,accepted);}
  assert(delivered==1&&persisted==0&&deleted==0&&ack_checks==1);released();
 }
 puts("PASS input after validated acceptance cannot undo completed delivery cleanup");
 for(const char* label:{"IMAGE_PRESIGN","IMAGE_RECONCILE"}){
  fresh_reset({200});g_media_retry_user_paused=true;reply_body="{}";
  {MediaRetryNetworkScope scope(job);int code=0;String body;
   assert(http_post_json_with_retries("https://test","payload",code,body,label,nullptr,nullptr,7,20000));
   assert(last_handshake==8&&last_connect==8000&&posts==1&&!media_retry_network_active());}
  released();
  for(const char* event:{"lock_wait","start","http_begin","post_enter","sdk_connect","sdk_write_done","post_status","reply_body","http_end"}){
   fresh_reset({200});reply_body="{}";input_at(event);
   {MediaRetryNetworkScope scope(job);int code=0;String body;
    assert(!http_post_json_with_retries("https://test","payload",code,body,label,nullptr,nullptr,7,20000));}
   assert(resets==0&&recoveries==0&&delay_ms==0);released();
  }
 }
 puts("PASS fresh image presign/reconcile uses 8s handshake and cancels at nine boundaries without radio recovery");
 for(const char* event:{"sdk_connect","sdk_write_enter","sdk_write_done","sdk_read_enter",
                       "sdk_read_done","sdk_bulk_read_enter","sdk_bulk_read_done","sdk_available","sdk_connected","sdk_peek"}){
  fresh_reset();input_at(event);std::vector<uint8_t> bytes(4096,42);
  {MediaRetryNetworkScope scope(job);SenseMediaRetryClient client;
   if(!strcmp(event,"sdk_connect"))assert(!client.connect("test",443,60000)&&last_connect==8000);
   else if(strstr(event,"write"))assert(client.write(bytes.data(),bytes.size())==0&&io_writes==1&&write_sizes[0]==512);
   else if(strstr(event,"bulk_read"))assert(client.read(bytes.data(),bytes.size())==-1);
   else if(strstr(event,"read"))assert(client.read()==-1);
   else if(!strcmp(event,"sdk_available"))assert(client.available()==0);
   else if(!strcmp(event,"sdk_connected"))client.connected();
   else client.peek();
   assert(media_retry_network_cancelled());assert(!client.connected());
   assert(stops==1&&last_stream_timeout==0&&!in_sdk);
  }
  released();
 }
 puts("PASS fresh socket checks at SDK boundaries; no stop inside SDK and one owner-local close");
 fresh_reset();
 {MediaRetryNetworkScope outer(job);sense_note_admitted_user_action();
  {MediaRetryNetworkScope inner(job);assert(media_retry_network_cancelled());}
  assert(media_retry_network_cancelled()&&media_upload_network_active());
 }
 released();
 fresh_reset();auto saved=job;saved.from_voice_sd=true;g_media_retry_user_paused=true;
 {MediaRetryNetworkScope outer(job);
  {MediaRetryNetworkScope nested(saved);assert(media_retry_network_active()&&media_retry_network_cancelled());}
  assert(!media_retry_network_active()&&!media_retry_network_cancelled());
 }
 released();
 fresh_reset();
 {MediaRetryNetworkScope outer(saved);g_media_retry_user_paused=true;
  {MediaRetryNetworkScope inner(job);assert(media_retry_network_active()&&media_retry_network_cancelled());}
  task=2;
  {MediaRetryNetworkScope foreign(job);SenseMediaRetryClient client;uint8_t bytes[513]{};
   assert(!media_upload_network_active()&&!media_retry_network_cancelled());
   assert(client.write(bytes,sizeof(bytes))==sizeof(bytes)&&stops==0&&write_sizes[0]==513);
   assert(client.connect("foreground",443,23000)&&last_connect==23000);
  }
  task=1;assert(media_retry_network_active()&&media_retry_network_cancelled());
 }
 released();
 puts("PASS nested generation/policy restoration and foreign task cannot steal or inherit worker cancellation");
 for(bool new_input:{false,true}){
  fresh_reset();
  {MediaRetryNetworkScope scope(job);http_queue_lock("fresh",7);wifi_recover_requested=true;
   if(new_input)sense_note_admitted_user_action();
   http_queue_unlock("fresh",7);
   assert(resets==(new_input?0U:1U)&&!wifi_recover_requested&&!http_inflight);
  }
  released();
 }
 puts("PASS actual unlock suppresses deferred radio reset only when new input cancelled fresh work");
 fresh_reset({503,202});cancel_delay=20;
 {MediaRetryNetworkScope scope(job);assert(!voice_upload_and_parse(job));
  assert(media_retry_network_cancelled()&&posts==1&&delay_ms==20&&resets==0);}
 released();
 puts("PASS new input during fresh retry backoff stops within its 20ms polling boundary");
 for(uint32_t start:{0U,UINT32_MAX}){
  fresh_reset();g_sense_user_action_generation=start;
  {MediaRetryNetworkScope scope(job);assert(!media_retry_network_cancelled());sense_note_admitted_user_action();assert(media_retry_network_cancelled());}
  released();
 }
 fresh_reset();
 {MediaRetryNetworkScope scope(job);
  boundary_hook={};assert(media_retry_network_wait(61)&&delay_ms==61);
  sense_note_admitted_user_action();assert(!media_retry_network_wait(5000)&&delay_ms==61);
 }
 released();
 for(unsigned origin=0;origin<3;++origin){
  fresh_reset();auto replay=job;replay.from_voice_sd=origin==0;replay.from_image_sd=origin==1;replay.from_persisted=origin==2;
  {MediaRetryNetworkScope scope(replay);assert(media_retry_network_active());g_media_retry_user_paused=true;assert(media_retry_network_cancelled());}
  released();
 }
 // Execute actual voice/presign/PUT failure blocks and the actual parked slot.
 // Cancellation must precede error UI, persistence, free and receipt cleanup.
 for(unsigned phase=0;phase<5;++phase){
  fresh_reset();auto interrupted=job;interrupted.is_voice=phase==0;
  {MediaRetryNetworkScope scope(interrupted);sense_note_admitted_user_action();
   fail_worker_phase(interrupted,phase);
   assert(upload_worker_parked_pending()&&!upload_inflight);
   assert(persisted==0&&worker_errors==0&&worker_freed==0&&delivered==0&&deleted==0&&spool_failure==0);
   assert(!memcmp(&upload_worker_parked_job,&interrupted,sizeof(interrupted)));
  }
  UploadJob resumed{};const char* stage=nullptr;unsigned long at=0;
  assert(upload_worker_take_parked_job(resumed,&stage,&at)&&stage&&strstr(stage,"cancelled"));
  assert(!memcmp(&resumed,&interrupted,sizeof(resumed))&&!upload_worker_parked_pending());
  assert(!upload_worker_take_parked_job(resumed));
  {MediaRetryNetworkScope next(interrupted);assert(!media_retry_network_cancelled());}
  released();
  // Refused parking cannot overwrite the older slot or discard current RAM.
  fresh_reset();auto older=job;older.job_id=99;
  assert(upload_worker_park_job(older,"existing","fixture"));
  {MediaRetryNetworkScope scope(interrupted);sense_note_admitted_user_action();fail_worker_phase(interrupted,phase);}
  assert(persisted==1&&worker_freed==1&&!memcmp(&retained,&interrupted,sizeof(retained)));
  assert(!memcmp(&upload_worker_parked_job,&older,sizeof(older)));released();
  // Ordinary failure and saved replay keep the established durable path.
  for(unsigned origin=0;origin<4;++origin){fresh_reset();auto ordinary=interrupted;
   ordinary.from_voice_sd=origin==1;ordinary.from_image_sd=origin==2;ordinary.from_persisted=origin==3;
   {MediaRetryNetworkScope scope(ordinary);if(origin)sense_note_admitted_user_action();fail_worker_phase(ordinary,phase);}
   assert(!upload_worker_parked_pending()&&persisted==1&&worker_freed==1&&!upload_inflight);
   assert(!memcmp(&retained,&ordinary,sizeof(retained)));released();
  }
 }
 // Input arriving after positive acceptance cannot turn delivery into a retry.
 for(bool voice:{false,true}){fresh_reset();auto accepted_job=job;accepted_job.is_voice=voice;
  {MediaRetryNetworkScope scope(accepted_job);sense_note_admitted_user_action();
   if(voice)actual_worker_voice_dispatch(accepted_job,true);else actual_worker_put(accepted_job,true,false);}
  assert(!upload_worker_parked_pending()&&persisted==0&&worker_freed==1&&!upload_inflight&&worker_errors==0);
  assert(voice?delivered==1:spool_success==1);released();
 }
 // The worker and sleep rescue transfer one parked descriptor exactly once.
 fresh_reset();assert(upload_worker_park_job(job,"race","fixture"));
 UploadJob one{},two{};bool got_one=false,got_two=false;
 std::thread a([&]{got_one=upload_worker_take_parked_job(one);});
 std::thread b([&]{got_two=upload_worker_take_parked_job(two);});a.join();b.join();
 assert(got_one!=got_two&&!upload_worker_parked_pending());
 assert(!memcmp(got_one?&one:&two,&job,sizeof(job))&&worker_freed==0&&persisted==0);
 puts("PASS actual fresh cancellation parking across five failure branches, full-slot fallback, accepted receipts and single-consumer rescue");
 printf("PASS %u fresh upload priority checks (actual functions, SDK/storage boundaries doubled)\n",checks);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    cpp, binary = a.out / 'test.cpp', a.out / 'test'
    cpp.write_text(harness(a.source_root))
    command = [shutil.which('clang++') or 'c++', '-std=c++17', '-pthread',
               '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I', str(a.source_root),
               '-I', str(a.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
               str(cpp), '-o', str(binary)]
    build = subprocess.run(command, capture_output=True, timeout=30, preexec_fn=corners.no_core)
    (a.out/'compile.log').write_bytes(build.stdout+build.stderr)
    if build.returncode: print(build.stderr.decode());raise SystemExit(build.returncode)
    run = subprocess.run([str(binary)], capture_output=True, timeout=15, preexec_fn=corners.no_core)
    (a.out/'run.log').write_bytes(run.stdout+run.stderr)
    print((run.stdout+run.stderr).decode(), end='')
    sources = ['Sense_Minimal/sense_media_retry.h','Sense_Minimal/sense_media_retry_client.h',
               'Sense_Minimal/sense_user_activity.h','Sense_Minimal/sense_voice.h','Sense_Minimal/sense_upload.h','Sense_Minimal/sense_op_queue.h',
               'Sense_Minimal/Sense_Minimal.ino']
    (a.out/'RESULT.json').write_text(json.dumps({'status':'PASS' if run.returncode==0 else 'FAIL',
        'hardware':False,'sdk_and_storage_boundaries':'doubled','exit_code':run.returncode,
        'sources':{s:hashlib.sha256((a.source_root/s).read_bytes()).hexdigest() for s in sources}},indent=2)+'\n')
    raise SystemExit(run.returncode)


if __name__=='__main__':main()

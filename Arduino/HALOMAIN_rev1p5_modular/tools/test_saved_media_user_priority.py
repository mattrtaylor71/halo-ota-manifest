#!/usr/bin/env python3
"""Execute actual saved SD admission and worker pause disposal with host edges.

Fetch, queue and network work are boundary doubles. Real replay gates and the
actual worker's paused-copy branch execute; original media bytes are immutable.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import re
from test_voice_persistence import definition

ROOT=Path(__file__).resolve().parents[1]


def harness():
    voice=(ROOT/'Sense_Minimal/sense_voice_spool.h').read_text()
    image=(ROOT/'Sense_Minimal/sense_image_spool.h').read_text()
    main=(ROOT/'Sense_Minimal/Sense_Minimal.ino').read_text()
    worker=definition(main,'static void upload_worker_task(')
    pause=definition(worker,'      if ((job.from_voice_sd || job.from_image_sd || job.from_persisted) &&')
    ticks=definition(voice,'static void sense_voice_spool_replay_tick(')+'\n'+definition(image,'static void sense_image_spool_replay_tick(')
    state=(ROOT/'Sense_Minimal/sense_media_retry.h').read_text()
    network=definition(state,'static bool media_retry_network_active(')+'\n'+definition(state,'class MediaRetryNetworkScope {')+';\n'
    guard='if (is_dish && !media_retry_network_active())'
    ui_guards=[definition(worker[m.start():],guard) for m in re.finditer(re.escape(guard),worker)]
    assert len(ui_guards)==4,'All four saved dish failure routes require foreground UI isolation'
    return r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "Sense_Minimal/sense_ops.h"
#include "halo_common/MediaRetryPolicy.h"
#define HALO_SENSE_PROD_WRAPPER 1
static unsigned checks=0;
static void check(bool ok,const char* why){++checks;if(!ok){fprintf(stderr,"FAIL %s\n",why);abort();}}
static uint32_t clock_ms=1000;
static uint32_t millis(){return clock_ms;}
static std::atomic<bool> g_media_retry_user_paused{false};
using TaskHandle_t=void*;
static int worker_task;
static TaskHandle_t xTaskGetCurrentTaskHandle(){return &worker_task;}
static std::atomic<TaskHandle_t> g_media_retry_network_owner{nullptr};
''' + network + r'''
static bool g_media_spool_replayed_this_boot=false,g_voice_spool_replayed_this_boot=false,g_image_spool_replayed_this_boot=false;
static bool upload_inflight=false,foreground_active=false,voice_recording_active=false,dish_scan_inflight=false,scan_ui_inflight=false;
static struct {bool active=false;} current_job;
static unsigned fetches=0,queues=0,attempts=0,freed=0,network_work=0;
static unsigned g_voice_spool_depth=1,g_image_spool_depth=1;
static char g_voice_spool_cursor[33]={},g_image_spool_cursor[33]={};
static bool pause_during_fetch=false;
static UploadJob fetched={},queued={};
static const std::vector<uint8_t> original={1,2,3,4,5,6,7,8};
static bool wifi_is_connected(){return true;}
static bool sense_time_has_fresh_sync(){return true;}
static bool sense_uart_ordinary_tx_allowed(){return true;}
static bool halo_provisioning_active(){return false;}
static bool halo_prod_boot_ota_pending(){return false;}
static unsigned upload_queue_count(){return 0;}
static bool sense_voice_replay_age_ok(const UploadJob&){return true;}
static void media_retry_attempted(halo_media_retry::Store){++attempts;}
template<class... T>static void uart_send_sense_diag(T...){}
static struct {template<class... T>void printf(const char*,T...){}} Serial;
static bool fetch(UploadJob& out,bool voice){
 ++fetches;out={};out.job_id=17;out.is_voice=voice;out.from_voice_sd=voice;out.from_image_sd=!voice;
 out.image_len=original.size();out.image_buf=(uint8_t*)malloc(out.image_len);memcpy(out.image_buf,original.data(),out.image_len);
 memset(out.voice.request_id,'a',32);memset(out.image.request_id,'b',32);fetched=out;
 if(pause_during_fetch)g_media_retry_user_paused=true;
 return true;
}
static bool sense_voice_spool_fetch(UploadJob& out){return fetch(out,true);}
static bool sense_image_spool_fetch(UploadJob& out){return fetch(out,false);}
static bool queue_voice_upload_job(uint32_t id,uint8_t* bytes,size_t len,uint8_t,bool,uint32_t,const UploadJob::VoiceEnvelope* envelope,bool sd){
 check(id==fetched.job_id&&bytes==fetched.image_buf&&len==original.size()&&sd&&!memcmp(envelope,&fetched.voice,sizeof(*envelope)),"voice queue keeps frozen fetched custody");
 ++queues;queued=fetched;return true;
}
static bool queue_upload_job(uint32_t id,const char*,const char*,uint16_t,bool,const UploadJob::CameraUploadMeta*,uint8_t* bytes,size_t len,uint8_t,bool,uint32_t,const UploadJob::ImageEnvelope* envelope,bool sd){
 check(id==fetched.job_id&&bytes==fetched.image_buf&&len==original.size()&&sd&&!memcmp(envelope,&fetched.image,sizeof(*envelope)),"image queue keeps frozen fetched custody");
 ++queues;queued=fetched;return true;
}
static void release_copy(void* bytes){check(bytes!=nullptr,"release only existing fetched RAM");++freed;free(bytes);}
#define free release_copy
''' + ticks + r'''
static void actual_worker_pause_boundary(UploadJob job){
 upload_inflight=true;
 for(unsigned once=0;once<1;++once){
''' + pause + r'''
  ++network_work;upload_inflight=false;
 }
}
#undef free
static unsigned foreground_errors=0;
static const char* presign_error_text(){return "network error";}
static void scan_ui_status_emit(const char* phase,const char*,const char*,uint32_t,bool){
 check(!strcmp(phase,"ERROR"),"failure routes retain ERROR semantics for foreground captures");++foreground_errors;
}
static void actual_dish_error_routes(const UploadJob& job){
 const bool is_dish=true;
''' + '\n'.join(ui_guards) + r'''
}
static void reset(){
 clock_ms=1000;g_media_retry_user_paused=false;g_media_spool_replayed_this_boot=false;
 g_voice_spool_replayed_this_boot=g_image_spool_replayed_this_boot=false;
 upload_inflight=foreground_active=voice_recording_active=dish_scan_inflight=scan_ui_inflight=current_job.active=false;
 fetches=queues=attempts=freed=network_work=0;pause_during_fetch=false;
 fetched=queued={};g_voice_spool_cursor[0]=g_image_spool_cursor[0]=0;
}
int main(){
 for(bool voice:{true,false}){
  reset();auto tick=voice?sense_voice_spool_replay_tick:sense_image_spool_replay_tick;
  g_media_retry_user_paused=true;
  for(uint32_t elapsed:{1000U,600000U,21600000U}){
   clock_ms=elapsed;foreground_active=current_job.active=false;tick();
   check(fetches==0&&queues==0&&attempts==0&&!g_media_spool_replayed_this_boot,
         "saved SD remains untouched for whole user wake despite idle foreground and elapsed time");
  }
  // A simulated later boot resets only wake-local state; storage stays present.
  g_media_retry_user_paused=false;tick();
  check(fetches==1&&queues==1&&attempts==1&&g_media_spool_replayed_this_boot,"next background wake admits retained SD copy");
  g_media_retry_user_paused=true;actual_worker_pause_boundary(queued);
  check(freed==1&&network_work==0&&!upload_inflight,"dequeued saved copy yields before network without storage deletion");
  reset();pause_during_fetch=true;tick();
  check(g_media_retry_user_paused,"user can arrive while storage fetch is finishing");
  actual_worker_pause_boundary(queued);
  check(freed==1&&network_work==0&&!upload_inflight,"pause during fetch still prevents the queued copy reaching network");
 }
 for(unsigned origin=0;origin<4;++origin){
  reset();UploadJob job={};job.image_buf=(uint8_t*)malloc(original.size());job.image_len=original.size();
  job.from_voice_sd=origin==0;job.from_image_sd=origin==1;job.from_persisted=origin==2;
  g_media_retry_user_paused=true;actual_worker_pause_boundary(job);
  check(network_work==(origin==3?1U:0U)&&freed==(origin==3?0U:1U)&&!upload_inflight,
        "saved origins release copies while fresh capture continues its normal flush path");
  if(origin==3)free(job.image_buf);
  foreground_errors=0;
  {MediaRetryNetworkScope scope(job);actual_dish_error_routes(job);}
  check(foreground_errors==(origin==3?4U:0U),"saved dish errors stay diagnostic while fresh dish errors still reach foreground UI");
 }
 check(original==std::vector<uint8_t>({1,2,3,4,5,6,7,8}),"pause never mutates original durable media fixture");
 printf("PASS %u actual SD replay/worker user-priority checks\n",checks);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='halo-user-replay-') as temp:
        cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test';cpp.write_text(harness())
        subprocess.run([shutil.which('clang++') or 'c++','-std=c++17','-fsanitize=address,undefined',
                        '-I',str(ROOT),str(cpp),'-o',str(exe)],check=True,timeout=30)
        subprocess.run([str(exe)],check=True,timeout=15)


if __name__=='__main__':main()

"""Execute the actual pre-sleep flush block with deterministic UART/queue I/O.

The production activity classifier and flush/custody decisions are compiled.
Hardware transport, queue producers and storage are explicit host doubles; this
does not test the UART lock implementation or claim SD/hardware acceptance.
--source may name the old sense_sleep.h to reproduce its starvation behavior.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]
ARGS = argparse.Namespace(source=None)


def harness(source):
    block = definition(source[source.index('// --- Upload flush window:'):], '{')
    signature = 'static bool sleep_upload_flush_yield_to_user('
    helper = definition(source, signature) if signature in source else ''
    activity = (ROOT / 'Sense_Minimal/sense_user_activity.h').read_text()
    ino = (ROOT / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    hold = definition(ino, 'static bool uploads_held_for_session(')
    hold_state = ino[ino.index('static unsigned long g_upload_hold_since_ms ='):ino.index('static bool uploads_held_for_session(')]
    parked_source = (ROOT / 'Sense_Minimal/sense_op_queue.h').read_text()
    parked = ''
    if 'upload_worker_parked_pending(' in source:
        signatures = ['static bool upload_worker_parked_pending(']
        if 'upload_worker_take_parked_job(' in source:
            signatures.append('static bool upload_worker_take_parked_job(')
        parked = '\n'.join(definition(parked_source, sig) for sig in signatures)
    return r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#define HALO_SENSE_PROD_WRAPPER 1
#define HALO_SENSE_UPLOAD_PERSISTENCE 1
#define HALO_DEFER_UPLOADS_TO_SLEEP 1
''' + activity + r'''
static unsigned long clock_ms=1000;
static unsigned long millis(){return clock_ms;}
static void delay(unsigned long n){clock_ms+=n;}
struct SerialType {template<class... A> void printf(const char*,A...){}
 void println(const char*){}} Serial;
struct UploadJob {
 uint32_t job_id=0; char mode[16]="discard"; uint8_t* image_buf=nullptr;
 size_t image_len=0; uint8_t retries=0;
 bool from_voice_sd=false,from_image_sd=false,from_persisted=false;
};
static std::vector<UploadJob> queue;
static void* upload_queue=&queue;
static uint32_t upload_queue_count(){return queue.size();}
static const int pdTRUE=1;
static int xQueuePeek(void*,UploadJob* out,int){if(queue.empty())return 0;*out=queue.front();return pdTRUE;}
static bool foreground_busy=false;
static std::atomic<bool> g_media_retry_user_paused{false};
static bool foreground_priority_active(unsigned long,const char**){return foreground_busy;}
static bool guardian_force_sleep=false,g_upload_flush_requested=false;
static bool upload_inflight=false,upload_worker_has_parked_job=false;
static bool upload_worker_holding_in_place=false;
static std::atomic<bool> upload_worker_claim_active{false};
static UploadJob upload_worker_parked_job;
static const char* upload_worker_parked_stage="host";
static unsigned long upload_worker_parked_at_ms=0;
using portMUX_TYPE=std::mutex;
static portMUX_TYPE upload_worker_parked_mux;
static void portENTER_CRITICAL(portMUX_TYPE* m){m->lock();}
static void portEXIT_CRITICAL(portMUX_TYPE* m){m->unlock();}
''' + parked + '\n' + hold_state + '\n' + hold + r'''

static unsigned pumps=0,acks=0,spools=0,frees=0,denials=0,teardowns=0;
static unsigned long ack_ms=0,first_spool_ms=0;
static bool raw_lease_busy=false,spool_succeeds=false,in_spool=false;
static unsigned long raw_lease_release_ms=0;
static bool persist_succeeds=false;
static unsigned persist_calls=0;
static std::function<void()> on_pump,on_spool;
struct Note {std::string event,detail;};
static std::vector<Note> notes;
static void uart_send_sense_diag(const char*,const char* event,const char*,int32_t,const char* detail){
 notes.push_back({event,detail});
}
static void sleep_notify_late_block(const char*){++denials;}
static bool upload_persist_save(const UploadJob&,uint8_t){++persist_calls;return persist_succeeds;}
static void upload_persist_note_event(const char*,const char*,int,uint8_t){}
static void pump_uart_rx_once(){
 assert(!in_spool); ++pumps;
 if(raw_lease_busy && millis()>=raw_lease_release_ms)raw_lease_busy=false;
 if(!raw_lease_busy && on_pump)on_pump();
}
static void deliver(const char* type,bool duplicate=false){
 ++acks;ack_ms=millis();
 if(!duplicate && sense_user_action_cancels_flush(type))sense_note_admitted_user_action();
}
static int xQueueReceive(void*,UploadJob* out,int){
 if(queue.empty())return 0;
 *out=queue.front();queue.erase(queue.begin());return pdTRUE;
}
static bool sense_spool_image_to_lcd(const UploadJob&,const uint8_t*,size_t){
 assert(!in_spool);in_spool=true;
 if(!spools)first_spool_ms=millis();++spools;
 if(on_spool)on_spool();
 delay(60);in_spool=false;return spool_succeeds;
}
// Storage custody remains an explicit boundary double. The production flush
// now routes all media through one safe-store entry; callback/cancellation
// assertions below remain unchanged.
static bool upload_persist_handle_failure(const UploadJob& job,const char*){
 if(upload_persist_save(job,job.retries))return true;
 return sense_spool_image_to_lcd(job,job.image_buf,job.image_len);
}
static void record_free(void* p){assert(p);++frees;}
#define free record_free
''' + helper + r'''
static void run_flush(){
 const uint32_t sleep_user_generation=sense_user_action_generation();
''' + block + r'''
 ++teardowns;
}
static UploadJob job(unsigned id){UploadJob j;j.job_id=id;j.image_buf=reinterpret_cast<uint8_t*>(uintptr_t(id));j.image_len=170000;return j;}
static unsigned count_note(const char* name){return std::count_if(notes.begin(),notes.end(),[&](const Note& n){return n.event==name;});}
int main(int argc,char**argv){
 assert(argc==2);std::string test=argv[1];queue={job(1),job(2),job(3)};
 const auto started=millis();
 if(test=="hold_reopen"){
   const char* why=nullptr;
   assert(uploads_held_for_session(&why) && !strcmp(why,"session_active"));
   g_upload_flush_requested=true;assert(!uploads_held_for_session(&why) && !strcmp(why,"flush_requested"));
   g_upload_flush_requested=false;assert(uploads_held_for_session(&why));
   g_upload_flush_requested=true;assert(!uploads_held_for_session(&why));
   g_upload_flush_requested=false;queue.clear();upload_worker_has_parked_job=true;
   assert(!uploads_held_for_session(&why) && !strcmp(why,"empty"));
   queue.assign(8,job(1));assert(!uploads_held_for_session(&why) && !strcmp(why,"highwater"));
   queue={job(1)};assert(uploads_held_for_session(&why));
   for(unsigned store=0;store<3;++store){
     queue={job(1)};
     if(store==0)queue.front().from_voice_sd=true;
     if(store==1)queue.front().from_image_sd=true;
     if(store==2)queue.front().from_persisted=true;
     assert(!uploads_held_for_session(&why) && !strcmp(why,"durable_replay"));
     foreground_busy=true;assert(uploads_held_for_session(&why));foreground_busy=false;
     g_media_retry_user_paused=true;assert(uploads_held_for_session(&why));
     // The flush may dequeue retained copies for release, but this session's
     // fresh captures still need their existing safe upload/storage path.
     g_upload_flush_requested=true;assert(!uploads_held_for_session(&why));g_upload_flush_requested=false;
     assert(g_media_retry_user_paused&&uploads_held_for_session(&why));g_media_retry_user_paused=false;
     queue.insert(queue.begin(),job(2));assert(uploads_held_for_session(&why));
   }
   queue={job(1)};
   g_media_retry_user_paused=true;assert(uploads_held_for_session(&why));
   g_upload_flush_requested=true;assert(!uploads_held_for_session(&why));g_upload_flush_requested=false;
   delay(UPLOAD_HOLD_MAX_MS+1);assert(!uploads_held_for_session(&why) && !strcmp(why,"max_age"));
 }else if(test=="new_user"){
   bool sent=false;on_pump=[&]{if(!sent && millis()-started>=2700){sent=true;deliver("INPUT_MENU_SELECT");}};
   run_flush();
   assert(sent && acks==1 && ack_ms-started<=2800);
   assert(!teardowns && !spools && !frees && queue.size()==3 && !g_upload_flush_requested);
   assert(count_note("flush_start")==1 && count_note("flush_cancel")==1 && count_note("flush_end")==0);
 }else if(test=="background"){
   on_pump=[] {deliver("INPUT_PING");deliver("LINK_HB");deliver("INPUT_SENSE_FW");deliver("INPUT_SLEEP");deliver("LCD_DIAG");};
   run_flush();
   assert(pumps>=450 && sense_user_action_generation()==0);
   assert(millis()-started==45000 && !spools && !frees);
   assert(!teardowns && queue.size()==3 && !g_upload_flush_requested);
   assert(count_note("flush_defer")==1 && count_note("flush_end")==0 && count_note("flush_cancel")==0);
 }else if(test=="duplicate"){
   sense_note_admitted_user_action();
   on_pump=[] {deliver("INPUT_MENU_SELECT",true);};run_flush();
   assert(sense_user_action_generation()==1 && millis()-started==45000);
   assert(!spools && !frees && queue.size()==3 && !teardowns && !g_upload_flush_requested);
   assert(count_note("flush_defer")==1 && count_note("flush_cancel")==0);
 }else if(test=="deadline_user"){
   bool sent=false;on_pump=[&]{if(!sent && millis()-started>=45000){sent=true;deliver("INPUT_DISCARD_OPTIONS");}};
   run_flush();assert(sent && millis()-started==45000);
   assert(queue.size()==3 && !spools && !frees && !teardowns && !g_upload_flush_requested);
 }else if(test=="busy_rx"){
   // The transport double refuses dispatch while a raw owner holds its lease.
   // Its release at900ms does not restart the flush deadline.
   raw_lease_busy=true;raw_lease_release_ms=started+900;
   on_pump=[] {deliver("INPUT_MENU_SELECT");};
   run_flush();assert(ack_ms-started==900 && !spools && queue.size()==3);
   assert(!g_upload_flush_requested && !teardowns);
 }else if(test=="current_item"){
   // A worker already owns an item. The user is dispatched only after its
   // raw transport lease releases; main never takes or frees another item.
   upload_inflight=true;upload_worker_claim_active=true;
   raw_lease_busy=true;raw_lease_release_ms=started+900;
   on_pump=[&]{deliver("INPUT_MENU_SELECT");};
   run_flush();
   assert(!spools && !frees && queue.size()==3 && queue.front().job_id==1);
   assert(upload_worker_claim_active && upload_inflight && ack_ms-started==900);
   assert(!teardowns && !g_upload_flush_requested && count_note("flush_cancel")==1);
 }else if(test=="parked_item"){
   upload_worker_has_parked_job=true;upload_worker_parked_job=job(4);
   bool sent=false;on_pump=[&]{if(!sent && millis()-started>=60000){sent=true;deliver("INPUT_MENU_SELECT");}};
   run_flush();assert(sent && millis()-started==60000);
   assert(!spools && !frees && queue.size()==3 && upload_worker_has_parked_job);
   assert(upload_worker_parked_job.image_buf==job(4).image_buf);
   assert(!teardowns && !g_upload_flush_requested);
 }else if(test=="parked_saved"){
   queue.clear();upload_worker_has_parked_job=true;upload_worker_parked_job=job(4);
   // Model the sole worker consuming the parked descriptor during the open
   // flush. The storage action is a declared boundary double, not main rescue.
   persist_succeeds=true;on_pump=[&]{if(millis()-started>=500 && upload_worker_has_parked_job){
     assert(g_upload_flush_requested);upload_worker_claim_active=true;
     auto local=upload_worker_parked_job;upload_worker_has_parked_job=false;
     upload_worker_parked_job={};assert(upload_persist_handle_failure(local,"host_worker"));
     record_free(local.image_buf);upload_worker_claim_active=false;
   }};run_flush();
   assert(persist_calls==1 && !spools && frees==1 && !upload_worker_has_parked_job);
   assert(millis()-started<=600);
   assert(teardowns==1 && !g_upload_flush_requested);
 }else if(test=="guardian"){
   guardian_force_sleep=true;on_pump=[] {deliver("INPUT_MENU_SELECT");};run_flush();
   assert(!pumps && !acks && millis()-started==45000 && teardowns==1);
   assert(!spools && !frees && queue.size()==3 && !g_upload_flush_requested);
   assert(count_note("flush_forced")==1); // Explicit RAM-at-risk hard stop.
 }else if(test=="later_flush"){
   run_flush();assert(!teardowns && queue.size()==3 && !g_upload_flush_requested);
   assert(count_note("flush_defer")==1 && millis()-started==45000);
   delay(10000);unsigned worker_completed=0;
   on_pump=[&]{if(g_upload_flush_requested && !queue.empty()){
     // Same pointer/id order survived the first window and RAII gate closure.
     assert(queue.front().job_id==worker_completed+1);
     assert(queue.front().image_buf==job(worker_completed+1).image_buf);
     queue.erase(queue.begin());++worker_completed;
   }};run_flush();
   assert(worker_completed==3 && queue.empty() && teardowns==1);
   assert(!spools && !frees && !g_upload_flush_requested && count_note("flush_end")==1);
 }else if(test=="empty"){
   queue.clear();run_flush();assert(millis()==started && !spools && !frees);
   assert(teardowns==1 && !g_upload_flush_requested && count_note("flush_end")==1);
 }else if(test=="drained"){
   on_pump=[&]{if(millis()-started>=500)queue.clear();};run_flush();
   assert(!spools && !frees && teardowns==1 && !g_upload_flush_requested);
   assert(millis()-started<=600 && count_note("flush_end")==1);
 }else assert(false);
}
'''


class SleepFlushTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='halo-sleep-flush-')
        cls.addClassCleanup(cls.temp.cleanup)
        path = Path(cls.temp.name)
        source = Path(ARGS.source) if ARGS.source else ROOT / 'Sense_Minimal/sense_sleep.h'
        (path / 'test.cpp').write_text(harness(source.read_text()))
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('C++ compiler unavailable')
        cls.exe = path / 'test'
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra',
                                 str(path / 'test.cpp'), '-o', str(cls.exe)],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)


def case(name):
    def run(self):
        result = subprocess.run([str(self.exe), name], capture_output=True,
                                text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    return run


for name in ('new_user', 'background', 'duplicate', 'deadline_user', 'busy_rx',
             'current_item', 'parked_item', 'parked_saved', 'guardian', 'empty', 'drained', 'later_flush', 'hold_reopen'):
    setattr(SleepFlushTest, 'test_' + name, case(name))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__] + remaining)

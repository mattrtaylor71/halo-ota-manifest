#!/usr/bin/env python3
"""Actual failure custody and store-refusal logic with controlled host boundaries.

The full persistence header and full Sense store function execute. SPIFFS, UART,
RTOS and clock are doubles. Tests distinguish correlated foreground contention
from an I/O failure, bound the original deadline, and verify RAM custody release.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_manual_ota_clock import definition
from test_voice_persistence import harness as persist_harness
from test_sense_image_spool import harness as image_harness

ROOT=Path(__file__).resolve().parents[1]

def custody_harness(root):
    code=persist_harness(root).replace('int main(){','static void original_persistence_controls(){')
    start=code.index('static bool sense_voice_spool_store(');end=code.index('\nstatic bool sense_voice_spool_mark_attempt',start)
    code=code[:start]+r'''
static unsigned storage_calls=0,storage_budget=120000,busy_count=0,io_count=0;
static bool busy_forever=false,script_success=true;
static uint32_t first_deadline=0;
static const uint8_t* expected_payload=nullptr;
static std::vector<uint8_t> expected_bytes;
static bool storage(const UploadJob& j,uint32_t deadline,bool* busy){
 check(g_media_custody_waiters.load()==1,"RAM capture custody held across each storage attempt");
 check(j.image_buf==expected_payload&&std::vector<uint8_t>(j.image_buf,j.image_buf+j.image_len)==expected_bytes,"busy retry preserves exact RAM pointer and payload");
 if(!storage_calls)first_deadline=deadline;else check(deadline==first_deadline,"busy retry never renews original deadline");
 ++storage_calls;if(busy)*busy=false;
 if(busy_forever||busy_count){if(busy_count)--busy_count;if(busy)*busy=true;return false;}
 if(io_count){--io_count;return false;}return script_success;
}
static bool use_script=false;
static bool sense_voice_spool_store(const UploadJob& j,uint32_t d,bool* busy=nullptr){
 if(use_script)return storage(j,d,busy);if(busy)*busy=false;++sd_writes;return sd_save_ok;
}
'''+code[end:]
    start=code.index('static bool sense_image_spool_store(');end=code.index('\n',start)
    code=code[:start]+'''static bool sense_image_spool_store(const UploadJob& j,uint32_t d,bool* busy=nullptr){if(use_script)return storage(j,d,busy);if(busy)*busy=false;check(!j.is_voice,"voice never routes through image storage");return false;}'''+code[end:]
    # Use the actual new sleep guard and check its place in the whole source.
    ino=(root/'Sense_Minimal/Sense_Minimal.ino').read_text()
    guard=definition(ino,'if (g_media_custody_waiters.load())')
    guardpos=ino.index(guard)
    assert ino.index('if (background_sleep_bypass',guardpos)>guardpos, 'custody guard must precede background bypass'
    code+='\nstatic bool custody_allows_sleep(const char** reason){'+guard+'return true;}\n'
    parked=(root/'Sense_Minimal/sense_op_queue.h').read_text()
    parked=parked[parked.index('static bool upload_worker_has_parked_job'):parked.index('static bool park_upload_job_if_foreground_active(')]
    code+=r'''
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
static thread_local unsigned park_lock_depth=0;
static void portENTER_CRITICAL(portMUX_TYPE* m){assert(!park_lock_depth);m->lock();++park_lock_depth;}
static void portEXIT_CRITICAL(portMUX_TYPE* m){assert(park_lock_depth==1);--park_lock_depth;m->unlock();}
'''+parked

    return code+r'''
static UploadJob prepare(bool voice){
 reset();g_sense_main_task_handle=1;current_task=2;uart_dispatch_depth=0;busy_pumps=busy_collects=0;use_script=true;storage_calls=busy_count=io_count=0;first_deadline=0;busy_forever=false;script_success=true;
 UploadJob j=fixture();j.is_voice=voice;if(!voice)strcpy(j.mode,"discard");
 expected_payload=j.image_buf;expected_bytes.assign(j.image_buf,j.image_buf+j.image_len);return j;
}
static void check_closed(){check(g_media_custody_waiters.load()==0,"failure function releases RAM custody on return");}
int main(){
 original_persistence_controls();check_closed();
 for(bool voice:{false,true}){
  auto j=prepare(voice);busy_count=3;auto initial=files;const auto started=millis();
  check(upload_persist_handle_failure(j,"host_busy"),"three busy refusals then actual scripted storage success");
  check(storage_calls==4&&millis()-started==750,"busy waits 250ms and does not consume two I/O attempts");
  check(files==initial&&writes==0,"busy contention does not fall through to SPIFFS");check_closed();
  j=prepare(voice);busy_forever=true;initial=files;const auto before=millis();
  check(!upload_persist_handle_failure(j,"host_deadline"),"persistent foreground busy returns unsaved at original budget");
  check(millis()-before==119750&&storage_calls==480&&millis()<first_deadline,"busy retries remain strictly within one 120 second deadline");
  check(files==initial&&writes==0&&errors==1,"busy deadline neither fabricates durable custody nor overwrites internal fallback");check_closed();
  j=prepare(voice);io_count=2;script_success=false;g_upload_persist_ready=false;
  check(!upload_persist_handle_failure(j,"host_io")&&storage_calls==2,"two genuine I/O failures retain original bounded attempt count");check_closed();
  j=prepare(voice);if(voice)j.from_voice_sd=true;else j.from_image_sd=true;
  check(upload_persist_handle_failure(j,"host_origin")&&storage_calls==0,"SD-origin failure retains original without new storage");check_closed();
 }
 for(bool voice:{false,true})for(unsigned context=0;context<4;++context){
  auto j=prepare(voice);busy_count=3;current_task=context==1?2:1;uart_dispatch_depth=context==2?1:0;
  if(context==3)g_sense_main_task_handle=0;
  check(upload_persist_handle_failure(j,"host_task"),"busy boundary preserves job until scripted storage success");
  check(busy_pumps==(context==0?3u:0u)&&busy_collects==(context==0?0u:3u),"only depth-zero registered main task dispatches; worker/reentrant/unknown task collects only");check_closed();
 }
 auto j=prepare(true);io_count=1;script_success=false;
 check(upload_persist_handle_failure(j,"host_fallback")&&storage_calls==1&&writes>0,"genuine voice SD I/O failure still permits checked SPIFFS fallback");check_closed();
 const char* reason=nullptr;{UploadMediaCustodyLease one;UploadMediaCustodyLease two;
 check(g_media_custody_waiters==2&&!custody_allows_sleep(&reason)&&!strcmp(reason,"media_custody"),"actual pre-bypass sleep guard blocks nested worker and rescue custody");}
 check(custody_allows_sleep(&reason)&&g_media_custody_waiters==0,"all scoped custody owners must finish before sleep admission");

 // Actual parked descriptor owner: producer and two consumers share one mux.
 auto first=fixture('a'),second=fixture('b');second.job_id=99;
 check(upload_worker_park_job(first,"stage-a","host"),"first producer publishes one parked descriptor");
 check(!upload_worker_park_job(second,"stage-b","host"),"occupied slot refuses replacement without losing caller ownership");
 UploadJob a={},b={};bool got_a=false,got_b=false;
 std::atomic<bool> go{false};
 std::thread worker([&]{while(!go.load())std::this_thread::yield();got_a=upload_worker_take_parked_job(a);});
 std::thread sleeper([&]{while(!go.load())std::this_thread::yield();got_b=upload_worker_take_parked_job(b);});
 go=true;worker.join();sleeper.join();
 check(got_a!=got_b&&!upload_worker_parked_pending(),"concurrent worker and sleep rescuer transfer descriptor exactly once");
 const UploadJob local=got_a?a:b;
 check(local.job_id==first.job_id&&local.image_buf==first.image_buf&&!memcmp(local.voice.request_id,first.voice.request_id,33),"winning consumer retains exact full original identity/payload");
 check(upload_worker_park_job(second,"new-stage","host"),"producer may publish a new job after old descriptor is locally claimed");
 check(local.job_id==first.job_id&&!memcmp(local.voice.request_id,first.voice.request_id,33),"new producer cannot rewrite consumer's local custody during a pump");
 UploadJob next={};const char* stage=nullptr;unsigned long at=0;
 check(upload_worker_take_parked_job(next,&stage,&at)&&next.job_id==second.job_id&&!strcmp(stage,"new-stage")&&at==millis(),"next owner receives matching descriptor and publication metadata");
 check(!upload_worker_take_parked_job(next)&&!upload_worker_has_parked_job&&!upload_worker_parked_job.image_buf,"empty second take cannot duplicate or retain shared pointer");
 printf("PASS %u actual persistence/busy/deadline/custody checks\n",checks);
}
'''


def refusal_harness(root,media):
    # Reuse the actual common RX/query/lease and full image codec/fetch harness;
    # inject BEGIN responses at the LCD boundary, not inside the tested store.
    code=image_harness(root).split('int main(){')[0]
    spool=(root/'Sense_Minimal/sense_image_spool.h').read_text()
    hook=r'''
static unsigned begin_mutation=0,begin_count=0;
'''
    point='static void sense_image_spool_json(JsonDocument& d,const char* type){'
    code=code.replace(point,hook+point)
    anchor=' if(!strcmp(type,"IMAGE_SPOOL_LIST_REQ")){'
    branch=r'''
 if(!strcmp(type,"IMAGE_XFER_BEGIN")){
  ++begin_count;ready(r,stored);r["type"]="IMAGE_XFER_READY";r["ok"]=0;r["json_ready"]=true;r["reason"]="busy";
  switch(begin_mutation){
   case 1:r["request_id"]="wrong";break;
   case 2:r["job_id"]=stored.job_id+1;break;
   case 3:r["len"]=(uint32_t)stored.image_len+1;break;
   case 4:r["crc32"]=stored.image.crc32^1;break;
   case 5:r["json_ready"]=false;break;
   case 6:r["json_ready"]=1;break;
   case 7:r["reason"]="io";break;
   case 8:r["image_schema"]=2;break;
   case 9:r["ok"]=true;break;
  }
 }else if(!strcmp(type,"IMAGE_SPOOL_LIST_REQ")){
'''
    assert anchor in code;code=code.replace(anchor,branch)
    code+='\n'+definition(spool,'static bool sense_image_spool_store(')+'\n'
    code+=r'''
int main(){
 uart_json_tx_init();
 for(unsigned mutation=0;mutation<10;++mutation){
  prepare();begin_mutation=mutation;begin_count=0;bool busy=true;
  bool saved=sense_image_spool_store(stored,millis()+20000,&busy);
  check(!saved&&begin_count==1,"busy and malformed negative READY never report stored");
  check(busy==(mutation==0),"only strict matching typed negative READY/json_ready/busy grants contention retry");
  check(!uart_rx_mutex->held&&!uart_json_tx_mutex->held&&!g_img_spool_tx_active,"refused store releases common UART before caller delay");
 }
 printf("PASS %u actual image store strict busy-refusal checks\n",checks);
}
'''
    if media=='voice':
        # Full voice function and helpers execute as themselves. The image fixture
        # supplies shared RX/query doubles only; voice identity/envelope is real.
        voice=(root/'Sense_Minimal/sense_voice.h').read_text();spool=(root/'Sense_Minimal/sense_voice_spool.h').read_text()
        # This separate conversion of the harness boundary is deliberate: all
        # production functions are replaced with the exact voice definitions.
        for sig in ('sense_image_spool_store','sense_image_spool_cleanup','sense_image_spool_remaining','sense_image_spool_step_deadline','sense_image_spool_operation_deadline','sense_image_spool_read','sense_image_spool_identity','sense_image_spool_meta','sense_image_spool_ready','sense_image_spool_u32','sense_image_spool_committed'):
            import re
            m=re.search(r'static (?:bool|uint32_t|void) '+sig+r'\(',code)
            if m:
                old=definition(code,m.group());newname=sig.replace('image','voice')
                match=re.search(r'static (?:bool|uint32_t|void) '+newname+r'\(',spool)
                new=definition(spool,match.group())
                # common harness already defines voice remaining/read.
                if newname in ('sense_voice_spool_remaining','sense_voice_spool_read'):new=''
                code=code.replace(old,new)
        # Discard image fetch/decode: not exercised by store-refusal tests.
        for name in ('sense_image_spool_fetch','sense_image_spool_decode'):
            old=definition(code,'static bool '+name+'(');code=code.replace(old,'')
        code=code.replace('sense_image_spool_','sense_voice_spool_').replace('IMAGE_SPOOL_','VOICE_SPOOL_').replace('IMAGE_XFER_','VOICE_XFER_').replace('image_schema','voice_schema').replace('SenseImageUartLease=SenseVoiceUartLease','UnusedImageLease=SenseVoiceUartLease')
        insert='\n'.join(definition(voice,sig) for sig in ('static bool sense_voice_envelope_valid(', 'static bool sense_voice_owner_matches('))
        code=code.replace('static uint32_t sense_voice_spool_step_deadline(',insert+'\nstatic uint32_t sense_voice_spool_step_deadline(',1)
        # Convert boundary fixture and receipt fields, preserving real PCM CRC.
        code=code.replace('j.image.','j.voice.').replace('stored.image.','stored.voice.')
        code=code.replace('strcpy(j.mode,"discard");','j.is_voice=true;strcpy(j.mode,"voice");strcpy(j.voice.session_id,"session");')
        code=code.replace('sense_image_hash(jpeg,sizeof(jpeg),j.voice.checksum_sha256);','')
        code=code.replace('static constexpr uint32_t VOICE_SPOOL_TRANSACTION_MS','static constexpr uint32_t VOICE_SPOOL_TRANSACTION_MS')
        code=code.replace('actual image store','actual voice store')
    return code


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-root',type=Path,default=ROOT);a=p.parse_args()
    for name,source in [('custody',custody_harness(a.source_root)),('image-refusal',refusal_harness(a.source_root,'image')),('voice-refusal',refusal_harness(a.source_root,'voice'))]:
        with tempfile.TemporaryDirectory(prefix='halo-media-busy-') as td:
            work=Path(td);(work/'test.cpp').write_text(source)
            r=subprocess.run([shutil.which('clang++') or 'c++','-std=c++17','-pthread','-Wno-deprecated-declarations','-I',str(a.source_root),'-I',str(a.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),str(work/'test.cpp'),'-o',str(work/'test')],capture_output=True,text=True)
            if r.returncode:raise RuntimeError(name+' compile failed:\n'+r.stderr)
            subprocess.run([str(work/'test')],check=True,timeout=30)
if __name__=='__main__':main()

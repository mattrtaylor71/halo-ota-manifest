#!/usr/bin/env python3
"""Run the actual retry NVS adapter through write failures, reboot and contention.

Preferences, clock and logging are host boundaries. Production mutation,
serialization, readback, cooldown and mutex code run unchanged. No hardware.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "halo_common/MediaRetryPolicy.h"
#include "Sense_Minimal/sense_ops.h"
using TaskHandle_t=void*;
static int background_task,foreground_task;
static thread_local TaskHandle_t current_task=&background_task;
static TaskHandle_t xTaskGetCurrentTaskHandle(){return current_task;}
static std::atomic<unsigned> checks{0};
static void check(bool pass,const char* name){++checks;if(!pass){fprintf(stderr,"FAIL %s\n",name);abort();}}
static uint32_t clock_ms=1000,stored_word=0;
static bool fail_read_open=false,fail_write_open=false,fail_put=false,fail_readback=false;
static unsigned opens=0,write_opens=0,put_calls=0,active_handles=0;
static uint32_t millis(){return clock_ms;}
class Preferences {
 public:
  bool begin(const char* name,bool ro){
   check(!strcmp(name,"media_retry"),"hint uses own namespace");
   ++opens;if(!ro)++write_opens;
   if(ro?fail_read_open:fail_write_open)return false;
   check(!opened&&active_handles==0,"mutex serializes NVS handles across workers");
   opened=true;read_only=ro;++active_handles;return true;
  }
  uint32_t getUInt(const char* key,uint32_t fallback){
   check(opened&&!strcmp(key,"state"),"read uses live hint handle");
   if(!read_only&&fail_readback)return stored_word^1U;
   return stored_word?stored_word:fallback;
  }
  size_t putUInt(const char* key,uint32_t word){
   check(opened&&!read_only&&!strcmp(key,"state"),"write uses writable hint handle");
   ++put_calls;if(fail_put)return 0;stored_word=word;return sizeof(word);
  }
  void end(){check(opened&&active_handles==1,"hint handle closes exactly once");opened=false;--active_handles;}
 private:bool opened=false,read_only=false;
};
static struct {template<class... T>void printf(const char*,T...){check(active_handles==0,"NVS handle closed before diagnostics");}}Serial;
#define RTC_DATA_ATTR
#include "Sense_Minimal/sense_media_retry.h"
using namespace halo_media_retry;
static uint32_t word(uint8_t pending,uint8_t backoff=0,bool image=false){State s;s.pending=pending;s.backoff=backoff;s.image_first=image;return encode(s);}
static void reboot(){
 check(active_handles==0,"no outstanding handle at reboot boundary");
 g_media_retry_state={};g_media_retry_loaded=false;g_media_retry_durable_word=0;
 g_media_retry_write_failed=false;g_media_retry_write_attempt_ms=0;
 g_media_retry_progress=false;g_media_retry_committed=false;
 g_media_retry_timer_selected=false;g_media_retry_timer_boot=false;
 g_media_retry_user_paused=false;
 g_media_retry_network_owner=nullptr;
}
static void reset(uint32_t durable=0){
 stored_word=durable;clock_ms=1000;fail_read_open=fail_write_open=fail_put=fail_readback=false;
 opens=write_opens=put_calls=0;reboot();
}
int main(){
 for(uint32_t initial:{0U,0xffffffffU,word(0)^64U}){
  reset(initial);check(media_retry_interval()==300&&g_media_retry_state.pending==kAllStores,
                       "missing or invalid hint discovers all stores");
  check(put_calls==0,"read-only load does not manufacture a durable hint");
  media_retry_commit_sleep();check(stored_word==word(kAllStores,1),"sleep checkpoints conservative discovery state");
 }
 reset(word(0));fail_read_open=true;
 check(media_retry_interval()==300&&g_media_retry_state.pending==kAllStores,"NVS read failure cannot imply empty storage");
 fail_read_open=false;media_retry_commit_sleep();reboot();
 check(media_retry_interval()==900,"successful later checkpoint survives reboot");

 for(unsigned failure=0;failure<3;++failure){
  reset(word(VoiceSd,3));check(media_retry_interval()==21600,"load last durable state");
  fail_write_open=failure==0;fail_put=failure==1;fail_readback=failure==2;
  media_retry_inventory(ImageSd,true);
  check(g_media_retry_write_failed&&g_media_retry_durable_word==word(VoiceSd,3)&&
        g_media_retry_state.pending==(VoiceSd|ImageSd),"failed write/readback leaves latest hint dirty in RAM");
  const auto attempted=write_opens;
  for(unsigned i=0;i<20;++i){++clock_ms;media_retry_inventory(ImageSd,true);}
  check(write_opens==attempted,"repeated same-boot observations respect failure cooldown");
  fail_write_open=fail_put=fail_readback=false;
  media_retry_commit_sleep();
  check(write_opens==attempted+1&&!g_media_retry_write_failed&&stored_word==word(VoiceSd|ImageSd,3),
        "sleep force retries dirty unchanged max-backoff word before cooldown");
  check(g_media_retry_durable_word==stored_word,"only verified readback advances durable state");
  const auto saved_puts=put_calls;media_retry_commit_sleep();
  check(put_calls==saved_puts,"one sleep commit cannot advance or rewrite twice");
  reboot();check(media_retry_interval()==21600&&g_media_retry_state.pending==(VoiceSd|ImageSd),
                 "both pending namespaces survive reboot after recovery");
 }

 reset(word(0));fail_put=true;media_retry_saved(VoiceSd);const auto first_attempts=write_opens;
 media_retry_saved(ImageSd);media_retry_inventory(VoiceFlash,true);
 check(write_opens==first_attempts&&g_media_retry_state.pending==kAllStores,
       "cooldown coalesces mutations without forgetting any pending store");
 fail_put=false;clock_ms+=4999;media_retry_inventory(ImageSd,true);
 check(write_opens==first_attempts,"cooldown remains closed before five seconds");
 ++clock_ms;media_retry_inventory(ImageSd,true);
 check(write_opens==first_attempts+1&&stored_word==word(kAllStores),"first eligible observation persists newest complete state");
 const auto stable_puts=put_calls;media_retry_inventory(ImageSd,true);media_retry_saved(VoiceSd);
 check(put_calls==stable_puts,"durable unchanged hint avoids write wear");

 reset(word(0));clock_ms=UINT32_MAX-1000;fail_put=true;media_retry_saved(VoiceSd);
 const auto wrap_attempts=write_opens;fail_put=false;clock_ms+=5000;
 media_retry_inventory(VoiceSd,true);check(write_opens==wrap_attempts+1&&stored_word==word(VoiceSd),
                                        "write retry cooldown survives millis wrap");

 reset(word(VoiceSd|ImageSd,3));media_retry_delivered();
 check(media_retry_interval()==60,"actual progress uses short recovery interval");
 media_retry_commit_sleep();check(stored_word==word(VoiceSd|ImageSd),"progress resets next offline backoff durably");
 reboot();media_retry_attempted(VoiceSd);reboot();
 check(media_retry_image_first(),"completed voice turn changes durable next namespace");
 media_retry_attempted(ImageSd);reboot();check(!media_retry_image_first(),"completed image turn returns voice priority");
 media_retry_inventory(VoiceSd,false);media_retry_inventory(ImageSd,false);
 check(media_retry_interval()==0&&stored_word==word(0),"verified final empty inventory stops scheduling");

 reset(word(0));std::atomic<bool> go{false};std::vector<std::thread> workers;
 g_media_retry_user_paused=true;
 for(unsigned worker=0;worker<6;++worker)workers.emplace_back([&,worker]{
  while(!go.load())std::this_thread::yield();
  for(unsigned pass=0;pass<100;++pass){
   media_retry_saved(worker%2?ImageSd:VoiceSd);media_retry_inventory(VoiceFlash,false);
  }
 });
 go=true;for(auto& worker:workers)worker.join();
 check(stored_word==word(VoiceSd|ImageSd)&&!active_handles,"concurrent worker hints retain both stores with no overlapping NVS handles");
 check(g_media_retry_user_paused,"inventory/save mutations cannot clear user's wake-long pause");
 media_retry_delivered();media_retry_attempted(VoiceSd);media_retry_commit_sleep();
 check(g_media_retry_user_paused,"progress and sleep checkpoint cannot resume this wake's paused replay");
 reboot();check(!g_media_retry_user_paused,"pause is boot-local and does not erase retained hint");
 for(unsigned store=0;store<3;++store){
  UploadJob saved={};saved.from_voice_sd=store==0;saved.from_image_sd=store==1;saved.from_persisted=store==2;
  g_media_retry_user_paused=false;
  {
   MediaRetryNetworkScope background(saved);
   check(media_retry_network_active()&&!media_retry_network_cancelled(),"saved job scope belongs to its upload worker");
   g_media_retry_user_paused=true;
   check(media_retry_network_cancelled(),"paused saved job cancels only its owning worker socket");
   std::thread foreground([]{
    current_task=&foreground_task;UploadJob fresh={};MediaRetryNetworkScope live(fresh);
    check(!media_retry_network_active()&&!media_retry_network_cancelled(),"another task's fresh HTTP never inherits background cancellation");
   });foreground.join();
   check(media_retry_network_active()&&media_retry_network_cancelled(),"foreign fresh scope destructor cannot clear background owner");
  }
  check(!media_retry_network_active()&&!media_retry_network_cancelled()&&!g_media_retry_network_owner.load(),
        "saved scope destructor clears socket ownership even while user pause remains");
 }
 {UploadJob fresh={};MediaRetryNetworkScope live(fresh);
  check(!media_retry_network_active()&&!media_retry_network_cancelled(),"fresh job on worker stays uncancelled during paused user session");}
 auto early_exit=[](){UploadJob saved={};saved.from_voice_sd=true;MediaRetryNetworkScope active(saved);return;};
 early_exit();check(!g_media_retry_network_owner.load(),"early return still releases saved network scope");
 printf("PASS %u actual Sense retry NVS failure/reboot/cooldown/concurrency checks\n",checks.load());
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='halo-sense-media-state-') as temp:
        cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test'
        cpp.write_text(SOURCE)
        subprocess.run([shutil.which('clang++') or 'c++','-std=c++17','-pthread',
                        '-fsanitize=address,undefined','-I',str(ROOT),str(cpp),'-o',str(exe)],
                       check=True,timeout=30)
        subprocess.run([str(exe)],check=True,timeout=15)


if __name__=='__main__':
    main()

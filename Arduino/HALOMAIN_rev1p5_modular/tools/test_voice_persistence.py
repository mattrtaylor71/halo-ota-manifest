#!/usr/bin/env python3
"""Compile the actual voice persistence header against an in-memory SPIFFS.

No firmware build/device/network. The real metadata codec, file ownership,
first-attempt marker and replay admission execute; file/RTOS/clock boundaries
are controlled host doubles. --source-root chooses the source under review.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


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
    functions = '\n'.join(definition(voice, name) for name in (
        'static uint32_t sense_voice_crc32(',
        'static bool sense_voice_request_id_valid(',
        'static bool sense_voice_envelope_valid(',
        'static bool sense_voice_replay_age_ok(',
        'static bool sense_voice_owner_matches('))
    functions += '\n' + definition((root / 'Sense_Minimal/sense_image_identity.h').read_text(), 'static bool sense_image_hash(')
    return r'''
#include <CommonCrypto/CommonDigest.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Sense_Minimal/sense_ops.h"
#include "halo_common/MediaRetryPolicy.h"
#define HALO_SENSE_PROD_WRAPPER 1
#define HALO_SENSE_UPLOAD_PERSISTENCE 1
static unsigned checks;
static void check(bool ok,const char* name){++checks;if(!ok){fprintf(stderr,"FAIL %s\n",name);abort();}}
static uint32_t now_ms=1000;
static time_t now_epoch=1800000000;
static bool fresh=true;
static uint32_t millis(){return now_ms;}
static void delay(uint32_t n){now_ms+=n;}
static time_t fake_time(time_t*){return now_epoch;}
#define time fake_time
static const uint32_t TIME_VALID_MIN_EPOCH=1700000000;
static bool sense_time_has_fresh_sync(){return fresh;}
static const char* owner="owner-a",*device="device-a",*TREPO_DEVICE_ID="device-fallback";
static void load_owner_id_or_default(char* p,size_t n){snprintf(p,n,"%s",owner);}
static void load_runtime_device_id(char* p,size_t n){snprintf(p,n,"%s",device);}
struct StaticSemaphore_t{std::recursive_timed_mutex mutex;};
using SemaphoreHandle_t=StaticSemaphore_t*;
static thread_local unsigned lock_depth=0;
static const int pdTRUE=1;
static uint32_t pdMS_TO_TICKS(uint32_t n){return n;}
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* s){return s;}
static int xSemaphoreTakeRecursive(SemaphoreHandle_t s,uint32_t n){
  assert(n==1000);if(!s->mutex.try_lock_for(std::chrono::milliseconds(n)))return 0;
  ++lock_depth;return pdTRUE;
}
static void xSemaphoreGiveRecursive(SemaphoreHandle_t s){assert(lock_depth);--lock_depth;s->mutex.unlock();}
static struct Logger{template<class...T>void printf(const char*,T...){}void println(const char*){}}Serial;
static std::map<std::string,std::vector<uint8_t>> files;
static unsigned writes=0,removes=0;
static bool truncate_next_write=false,rename_fail=false,mount_failure=false;
static std::vector<bool> mount_arguments;
class File{
 public:
  File()=default;
  File(std::string p,bool w):path(std::move(p)),valid(true),write_mode(w){}
  explicit operator bool()const{return valid;}
  size_t size()const{return valid?files.at(path).size():0;}
  size_t write(const uint8_t* b,size_t n){assert(valid&&write_mode&&lock_depth);++writes;
    if(truncate_next_write){truncate_next_write=false;if(n)--n;}
    auto&v=files[path];v.insert(v.end(),b,b+n);return n;
  }
  int read(uint8_t* b,size_t n){assert(valid&&lock_depth);auto&v=files.at(path);
    if(n>v.size()-position)n=v.size()-position;memcpy(b,v.data()+position,n);position+=n;return (int)n;
  }
  void close(){valid=false;}
  void flush(){}
 private:std::string path;bool valid=false,write_mode=false;size_t position=0;
};
static struct FakeSpiffs{
  bool begin(bool format){mount_arguments.push_back(format);if(format){files.clear();return true;}return !mount_failure;}
  bool exists(const char* p){return files.count(p);}
  File open(const char* p,const char* mode){
    if(mode[0]=='w'){assert(lock_depth);files[p].clear();return File(p,true);}
    return files.count(p)?File(p,false):File();
  }
  bool remove(const char* p){assert(lock_depth);++removes;return files.erase(p)!=0;}
  bool rename(const char* a,const char* b){assert(lock_depth);if(rename_fail||files.count(b)||!files.count(a))return false;
    files[b]=files[a];files.erase(a);return true;}
  size_t totalBytes(){return 192*1024;}
  size_t usedBytes(){size_t n=0;for(auto& p:files)n+=p.second.size();return n;}
}SPIFFS;
static void diag_sanitize_token(char* d,size_t n,const char* s){snprintf(d,n,"%s",s?s:"");}
template<class...T>static void diag_record_action_event(T...){}
template<class...T>static void uart_send_sense_diag(T...){}
static unsigned errors=0;
template<class...T>static void uart_send_ui_status_extended(T...){++errors;}
static void dump_system_truth(const char*){}
static bool wifi_is_connected(){return true;}
static bool halo_provisioning_active(){return false;}
static bool boot_ota_pending=false,ordinary_uart_allowed=true;
static bool halo_prod_boot_ota_pending(){return boot_ota_pending;}
static bool sense_uart_ordinary_tx_allowed(){return ordinary_uart_allowed;}
static bool foreground_active=false,voice_recording_active=false;
static std::atomic<bool> g_media_retry_user_paused{false};
static struct {bool active=false;} current_job;
static void media_retry_inventory(halo_media_retry::Store,bool){}
static void media_retry_saved(halo_media_retry::Store){}
static bool upload_inflight=false,dish_scan_inflight=false,scan_ui_inflight=false;
static int g_boot_reset_reason=0;
static const char* reset_reason_label(int){return "host";}
static bool g_voice_spool_replayed_this_boot=false;
static std::atomic<uint32_t> g_media_custody_waiters{0};
using TaskHandle_t=uintptr_t;
static TaskHandle_t g_sense_main_task_handle=1,current_task=2;
static std::atomic<unsigned> uart_dispatch_depth{0};
static unsigned busy_pumps=0,busy_collects=0;
static TaskHandle_t xTaskGetCurrentTaskHandle(){return current_task;}
static void pump_uart_rx_once(){assert(!lock_depth);++busy_pumps;}
static void uart_collect_rx_once(){assert(!lock_depth);++busy_collects;}

static unsigned sd_writes=0,sd_marks=0;
static bool sd_save_ok=false;
static uint32_t sense_voice_spool_operation_deadline(){return millis()+120000;}
static uint32_t sense_voice_spool_remaining(uint32_t d){int32_t n=(int32_t)(d-millis());return n>0?n:0;}
static bool sense_voice_spool_store(const UploadJob&,uint32_t,bool* busy=nullptr){if(busy)*busy=false;++sd_writes;return sd_save_ok;}
static bool sense_voice_spool_mark_attempt(UploadJob& j,uint32_t e){++sd_marks;j.created_epoch=e;return true;}
static bool sense_spool_image_to_lcd(const UploadJob&,const uint8_t*,size_t){return false;}
// The image branch is outside this voice/persisted-record suite; any accidental
// routing of a voice job into it is a failure rather than a permissive stub.
static bool g_image_spool_replayed_this_boot=false;
static uint32_t sense_image_spool_operation_deadline(){return millis()+120000;}
static uint32_t sense_image_spool_remaining(uint32_t d){return sense_voice_spool_remaining(d);}
static bool sense_image_spool_store(const UploadJob& j,uint32_t,bool* busy=nullptr){if(busy)*busy=false;check(!j.is_voice,"voice never routes through image storage");return false;}

static int mbedtls_sha256(const uint8_t* p,size_t n,uint8_t* out,int is224){return is224?-1:(CC_SHA256(p,(CC_LONG)n,out)?0:-1);}
''' + functions + r'''
#include "Sense_Minimal/sense_upload_persist.h"
static uint8_t* allocate_upload_buffer(size_t n,bool* psram){*psram=false;return (uint8_t*)malloc(n);}
static uint32_t upload_queue_count(){return 0;}
static bool queue_upload_job(uint32_t,const char*,const char*,uint16_t,bool,const UploadJob::CameraUploadMeta*,uint8_t*,size_t,uint8_t,bool,uint32_t){return false;}
static unsigned replay_queue_calls=0;
static bool replay_queue_accept=false;
static bool queue_voice_upload_job(uint32_t,uint8_t* p,size_t,uint8_t,bool,uint32_t,const UploadJob::VoiceEnvelope*,bool){
 ++replay_queue_calls;if(replay_queue_accept){free(p);return true;}return false;
}
static uint8_t pcm[8]={1,2,3,4,5,6,7,8};
static UploadJob fixture(char id='a'){
 UploadJob j={};j.job_id=41;j.is_voice=true;j.image_buf=pcm;j.image_len=sizeof(pcm);strcpy(j.mode,"voice");
 strcpy(j.voice.owner_id,"owner-a");strcpy(j.voice.device_id,"device-a");strcpy(j.voice.session_id,"session-a");
 memset(j.voice.request_id,id,32);j.voice.request_id[32]=0;j.voice.crc32=sense_voice_crc32(pcm,sizeof(pcm));return j;
}
static void reset(){files.clear();writes=removes=errors=sd_writes=sd_marks=0;now_ms=1000;now_epoch=1800000000;
 fresh=true;owner="owner-a";device="device-a";truncate_next_write=rename_fail=sd_save_ok=mount_failure=false;mount_arguments.clear();
 g_upload_persist_ready=false;g_upload_persist_attempted_this_boot=false;g_voice_spool_replayed_this_boot=false;
 g_upload_persist_replay_not_before_ms=0;g_upload_persist_last_check_ms=0;
 boot_ota_pending=false;ordinary_uart_allowed=true;foreground_active=voice_recording_active=current_job.active=false;
 g_media_retry_user_paused=false;
 replay_queue_calls=0;replay_queue_accept=false;
 upload_persist_setup();}
static UploadJob load(){UploadJob j={};check(upload_persist_load(&j),"load actual saved voice");return j;}
int main(){
 reset();UploadJob j=fixture();
 check(upload_persist_save(j,1),"epoch-zero voice saved without inventing attempt time");
 const auto original=files;const unsigned initial_writes=writes;
 check(upload_persist_save(j,2)&&files==original&&writes==initial_writes,"same voice replay does not rewrite sole payload");
 UploadJob other=fixture('b');other.job_id=42;
 check(!upload_persist_save(other,1)&&files==original,"new voice cannot overwrite unrelated pending voice");
 UploadJob photo=j;photo.is_voice=false;strcpy(photo.mode,"dish");
 check(!upload_persist_save(photo,1)&&files==original,"photo cannot overwrite pending voice");
 UploadJob restored=load();check(restored.created_epoch==0&&restored.from_persisted,"zero epoch survives reboot as never attempted");
 fresh=false;check(!sense_voice_prepare_first_attempt(restored)&&files==original,"no clock cannot create attempt marker");fresh=true;
 check(sense_voice_prepare_first_attempt(restored)&&restored.created_epoch==(uint32_t)now_epoch,"first POST requires actual durable marker");
 check(files.count(UPLOAD_PERSIST_VOICE_ATTEMPT_PATH)==1,"attempt sidecar committed before admission");
 const auto attempted=files;const auto first_epoch=restored.created_epoch;
 free(restored.image_buf);g_upload_persist_ready=false;upload_persist_setup();now_epoch+=60;
 restored=load();check(restored.created_epoch==first_epoch,"uncertain attempt epoch preserved through power cycle");
 check(sense_voice_prepare_first_attempt(restored)&&restored.created_epoch==first_epoch&&files==attempted,"retry never renews age");
 const unsigned before_sd=sd_writes;
 check(upload_persist_handle_failure(restored,"host")&&files==attempted&&sd_writes==before_sd,"SPIFFS-origin failure retains original without duplicate SD copy");
 check(g_upload_persist_attempted_this_boot&&!g_voice_spool_replayed_this_boot&&g_upload_persist_replay_not_before_ms>now_ms,
       "flash failure suppresses only its same-boot replay and leaves SD voice eligible");
 free(restored.image_buf);
 owner="new-owner";UploadJob blocked={};check(!upload_persist_load(&blocked)&&files==attempted,"reclaimed device retains old account recording without replay/delete");owner="owner-a";
 now_epoch=first_epoch+7*86400+1;check(!upload_persist_load(&blocked)&&files==attempted,"expired voice retained without replay/delete");
 now_epoch=first_epoch+1;files[UPLOAD_PERSIST_IMAGE_PATH][0]^=1;const auto corrupt=files;
 check(!upload_persist_load(&blocked)&&files==corrupt,"corrupt voice retained for recovery, never uploaded");
 // Whole-record CRC must precede normalization and the epoch sidecar. A
 // syntactically valid request/session mutation would otherwise mint a new
 // backend identity while the original audio CRC continues to match.
 reset();j=fixture();check(upload_persist_save(j,1),"prepare envelope corruption fixture");
 restored=load();check(sense_voice_prepare_first_attempt(restored),"prepare actual committed attempt overlay");
 free(restored.image_buf);const auto intact_envelope=files;
 PersistedUploadMeta layout={};
 struct Mutation {uint8_t* field;uint8_t mask;const char* name;};
 const Mutation mutations[]={
  {(uint8_t*)&layout.voice.session_id[0],3,"valid session mutation rejected before replay"},
  {(uint8_t*)&layout.voice.request_id[0],3,"valid request mutation rejected before replay"},
  {(uint8_t*)&layout.created_epoch,1,"epoch mutation rejected before attempt overlay"},
  {(uint8_t*)&layout.voice.owner_id[0],3,"owner mutation rejected"},
  {(uint8_t*)&layout.voice.device_id[0],3,"device mutation rejected"},
  {(uint8_t*)&layout.reserved,2,"voice flag corruption cannot become legacy photo"},
  {(uint8_t*)&layout.retries,1,"retry counter corruption rejected"},
  {(uint8_t*)&layout.image_len,2,"payload length corruption rejected"},
  {(uint8_t*)&layout.voice.crc32,1,"payload checksum corruption rejected"},
  {(uint8_t*)&layout.mode[15],1,"checksum checked before mode terminator normalization"},
  {(uint8_t*)&layout.expiry_date[15],1,"checksum checked before expiry terminator normalization"},
  {(uint8_t*)&layout.checksum,1,"metadata checksum corruption rejected"}
 };
 for(const auto& mutation:mutations){
  files=intact_envelope;
  const size_t offset=mutation.field-(uint8_t*)&layout;
  files[UPLOAD_PERSIST_META_PATH][offset]^=mutation.mask;const auto damaged=files;
  PersistedUploadMeta rejected={};
  check(!upload_persist_read_meta(&rejected)&&!upload_persist_load(&blocked)&&files==damaged,mutation.name);
  check(!upload_persist_save(fixture('b'),1)&&files==damaged,"corrupt original cannot be overwritten by another voice");
 }
 files=intact_envelope;
 // A new-size file cannot opt out of its CRC by forging the legacy format
 // fields; genuine V1/V2 file-size compatibility is exercised below.
 const uint16_t legacy_version=2,legacy_size=sizeof(PersistedUploadMetaV2);
 memcpy(files[UPLOAD_PERSIST_META_PATH].data()+((uint8_t*)&layout.version-(uint8_t*)&layout),&legacy_version,sizeof(legacy_version));
 memcpy(files[UPLOAD_PERSIST_META_PATH].data()+((uint8_t*)&layout.header_size-(uint8_t*)&layout),&legacy_size,sizeof(legacy_size));
 const auto downgrade=files;check(!upload_persist_load(&blocked)&&files==downgrade,"new-size metadata cannot bypass CRC using a legacy header");
 files=intact_envelope;files[UPLOAD_PERSIST_META_PATH].resize(sizeof(PersistedUploadMeta)-sizeof(uint32_t));
 const auto old_v3=files;check(!upload_persist_load(&blocked)&&files==old_v3,"pre-checksum V3 bytes held without replay or cleanup");
 reset();j=fixture();truncate_next_write=true;
 check(!upload_persist_save(j,1)&&removes==0,"partial voice write refuses success without destructive cleanup");
 const auto partial=files;check(!upload_persist_save(j,1)&&files==partial,"orphan voice bytes not overwritten by fresh save");
 reset();j=fixture();check(upload_persist_save(j,1),"prepare marker fault fixture");
 restored=load();rename_fail=true;const auto before_marker=files;
 check(!sense_voice_prepare_first_attempt(restored)&&restored.created_epoch==0,"failed marker commit blocks first POST");
 check(files.at(UPLOAD_PERSIST_IMAGE_PATH)==before_marker.at(UPLOAD_PERSIST_IMAGE_PATH)&&
       files.at(UPLOAD_PERSIST_META_PATH)==before_marker.at(UPLOAD_PERSIST_META_PATH),"marker fault preserves original payload/meta");
 free(restored.image_buf);
 reset();j=fixture();j.created_epoch=now_epoch;check(upload_persist_save(j,1),"prepare accepted cleanup");
 check(!upload_persist_delete_voice(other)&&files.size()==2,"wrong voice receipt cannot delete pending payload");
 check(upload_persist_delete_voice(j)&&files.empty(),"matching backend-owned voice removes only own SPIFFS record");
 reset();j=fixture();j.from_voice_sd=true;const unsigned before=writes;
 check(upload_persist_handle_failure(j,"host")&&sd_writes==0&&writes==before&&files.empty()&&errors==0,"SD-origin failure neither copies/deletes its committed slot nor emits foreground error");
 reset();j=fixture();fresh=false;check(!sense_voice_prepare_first_attempt(j)&&j.created_epoch==0,"fresh RAM waits for valid time before first POST");
 fresh=true;check(sense_voice_prepare_first_attempt(j)&&j.created_epoch==(uint32_t)now_epoch&&files.empty(),"healthy live RAM remains fallback-only latency");
 reset();j=fixture();other=fixture('b');other.job_id=42;std::atomic<bool> go=false;bool a=false,b=false;
 std::thread first([&]{while(!go.load())std::this_thread::yield();a=upload_persist_save(j,1);});
 std::thread second([&]{while(!go.load())std::this_thread::yield();b=upload_persist_save(other,1);});
 go=true;first.join();second.join();check(a!=b,"simultaneous worker/sleep rescue admits exactly one empty-slot writer");
 restored=load();check(!memcmp(restored.voice.request_id,(a?j:other).voice.request_id,33),"concurrent write metadata matches winning payload identity");free(restored.image_buf);
 // V1/V2 codecs remain recognizable, but neither legacy media kind has the
 // frozen request/account contract. Retain old bytes rather than invent identity.
 for(unsigned version:{1u,2u}){
  reset();PersistedUploadMetaV2 m={};m.magic=UPLOAD_PERSIST_MAGIC;m.version=version;m.job_id=11;
  m.header_size=version==1?sizeof(PersistedUploadMetaV1):sizeof(m);m.image_len=sizeof(pcm);m.created_epoch=now_epoch;strcpy(m.mode,"dish");
  files[UPLOAD_PERSIST_META_PATH]=std::vector<uint8_t>((uint8_t*)&m,(uint8_t*)&m+m.header_size);
  files[UPLOAD_PERSIST_IMAGE_PATH]=std::vector<uint8_t>(pcm,pcm+sizeof(pcm));
  auto legacy=files;UploadJob old={};check(!upload_persist_load(&old)&&!old.image_buf&&files==legacy,"legacy photo without frozen identity remains held byte exactly");
  check(!upload_persist_save(fixture(),1)&&files==legacy,"voice cannot replace pending legacy photo");
  m.reserved=UPLOAD_PERSIST_FLAG_IS_VOICE;strcpy(m.mode,"voice");
  files[UPLOAD_PERSIST_META_PATH]=std::vector<uint8_t>((uint8_t*)&m,(uint8_t*)&m+m.header_size);legacy=files;
  check(!upload_persist_load(&old)&&files==legacy,"legacy unbound voice retained without cross-account replay");
 }
 reset();j=fixture();check(upload_persist_save(j,1),"prepare retained voice before mount failure");
 auto retained=files;g_upload_persist_ready=false;mount_failure=true;mount_arguments.clear();upload_persist_setup();
 check(!g_upload_persist_ready&&mount_arguments==std::vector<bool>{false},"failed setup never invokes format fallback");
 check(files==retained,"mount failure preserves every retained voice byte");
 mount_failure=false;upload_persist_setup();check(g_upload_persist_ready&&files==retained,"later good mount recovers original data without format");
 // Real replay entry point must defer before consuming its one boot attempt.
 // The queue double accepts ownership only after every external gate clears.
 for(unsigned gate=0;gate<5;++gate){
  reset();j=fixture();j.created_epoch=now_epoch;
  check(upload_persist_save(j,1),"prepare replay admission fixture");
  const auto intact=files;now_ms=10000;replay_queue_accept=true;
  if(gate==0)fresh=false;
  if(gate==1)foreground_active=true;
  if(gate==2)current_job.active=true;
  if(gate==3)ordinary_uart_allowed=false;
  if(gate==4)boot_ota_pending=true;
  upload_persist_maybe_replay();
  check(!g_upload_persist_attempted_this_boot&&replay_queue_calls==0&&files==intact,
        "blocked replay retains boot eligibility and exact durable bytes");
  fresh=true;foreground_active=current_job.active=false;ordinary_uart_allowed=true;boot_ota_pending=false;
  upload_persist_maybe_replay();
  check(g_upload_persist_attempted_this_boot&&replay_queue_calls==1&&files==intact,
        "clearing admission gate permits replay during this same wake");
 }
 reset();j=fixture();j.created_epoch=now_epoch;
 check(upload_persist_save(j,1),"prepare user-paused flash replay");
 const auto paused_bytes=files;replay_queue_accept=true;g_media_retry_user_paused=true;
 for(unsigned elapsed:{10000U,600000U,21600000U}){
  now_ms=elapsed;foreground_active=current_job.active=voice_recording_active=false;
  upload_persist_maybe_replay();
  check(!g_upload_persist_attempted_this_boot&&replay_queue_calls==0&&files==paused_bytes&&g_media_retry_user_paused,
        "cleared foreground and elapsed time cannot restart saved replay during user's wake");
 }
 // A later boot constructs a fresh pause flag; the retained bytes stay intact.
 g_media_retry_user_paused=false;g_upload_persist_last_check_ms=0;
 upload_persist_maybe_replay();
 check(g_upload_persist_attempted_this_boot&&replay_queue_calls==1&&files==paused_bytes,
       "next background wake can admit the untouched flash recording");
 printf("PASS %u actual-header voice persistence/attempt/ownership checks\n",checks);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-voice-persist-') as temp:
        cpp = Path(temp) / 'test.cpp'
        exe = Path(temp) / 'test'
        cpp.write_text(harness(a.source_root))
        subprocess.run([shutil.which('clang++') or 'c++', '-std=c++17', '-pthread', '-O1',
                        '-I', str(a.source_root), str(cpp), '-o', str(exe)], check=True, timeout=30)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()

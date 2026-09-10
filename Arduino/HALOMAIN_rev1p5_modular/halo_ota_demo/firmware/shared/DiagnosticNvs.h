#pragma once
#include "DiagnosticStore.h"
#include <nvs.h>
#include "DiagnosticLegacyProof.h"

namespace halo_diag {
// No private mutex: these callbacks MUST use the board's existing NVS writer
// lease, also used by policy/eligibility. safe() excludes OTA flash, user work,
// ISR use and expired caller deadline. SDK NVS may allocate internally.
struct NvsGuard {
  void* arg;bool (*take)(void*);void (*give)(void*);
  bool (*safe)(void*);bool (*writers_retired)(void*);bool (*layout_valid)(void*);
  bool (*policy_allocated)(void*); // typed/CRC-checked768B policy, SAME held lease
  void (*uncertain)(void*,bool retirement);
};
enum class LegacyType:uint8_t { String,Blob,U16,I32,U32 };
struct LegacyKey {const char* ns;const char* key;LegacyType type;uint16_t bytes;};
// Exact source-owned key/type inventory. String limits come from the actual
// writers; observed bytes below remain provenance, not a fixed string length.
// Missing keys permit interrupted retirement. Never clear a namespace.
static const LegacyKey SENSE_LEGACY[]={
 {"sense_err","slot_0",LegacyType::String,160},{"sense_err","slot_1",LegacyType::String,163},
 {"sense_err","slot_2",LegacyType::String,163},{"sense_err","slot_3",LegacyType::String,173},
 {"sense_err","slot_4",LegacyType::String,160},{"sense_err","slot_5",LegacyType::String,175},
 {"sense_err","slot_6",LegacyType::String,177},{"sense_err","slot_7",LegacyType::String,177},
 {"sense_err","head",LegacyType::I32,4},{"sense_err","count",LegacyType::I32,4},
 {"sense_err","next_seq",LegacyType::U32,4},{"sense_err","del_seq",LegacyType::U32,4},
 {"wakelog","e24",LegacyType::Blob,28},{"wakelog","e25",LegacyType::Blob,28},
 {"wakelog","e26",LegacyType::Blob,28},{"wakelog","e27",LegacyType::Blob,28},
 {"wakelog","e28",LegacyType::Blob,28},{"wakelog","e30",LegacyType::Blob,28},
 {"wakelog","e31",LegacyType::Blob,28},{"wakelog","e32",LegacyType::Blob,28},
 {"wakelog","e33",LegacyType::Blob,28},{"wakelog","e34",LegacyType::Blob,28},
 {"wakelog","e35",LegacyType::Blob,28},{"wakelog","e36",LegacyType::Blob,28},
 {"wakelog","e37",LegacyType::Blob,28},{"wakelog","e39",LegacyType::Blob,28},
 {"wakelog","head",LegacyType::U16,2},{"wakelog","total",LegacyType::U16,2}
};
static const LegacyKey LCD_LEGACY[]={
 {"err_log","slot_0",LegacyType::String,142},{"err_log","slot_1",LegacyType::String,150},
 {"err_log","slot_2",LegacyType::String,150},{"err_log","slot_3",LegacyType::String,137},
 {"err_log","slot_4",LegacyType::String,153},{"err_log","slot_5",LegacyType::String,142},
 {"err_log","slot_6",LegacyType::String,150},{"err_log","slot_7",LegacyType::String,150},
 {"err_log","slot_8",LegacyType::String,138},{"err_log","slot_9",LegacyType::String,154},
 {"err_log","slot_10",LegacyType::String,142},{"err_log","slot_11",LegacyType::String,150},
 {"err_log","slot_12",LegacyType::String,150},{"err_log","slot_13",LegacyType::String,138},
 {"err_log","slot_14",LegacyType::String,154},{"err_log","slot_15",LegacyType::String,142},
 {"err_log","slot_16",LegacyType::String,150},{"err_log","slot_17",LegacyType::String,150},
 {"err_log","slot_18",LegacyType::String,138},{"err_log","slot_19",LegacyType::String,154},
 {"err_log","head",LegacyType::I32,4},{"err_log","count",LegacyType::I32,4},
 {"wifi_diag","slot_0",LegacyType::String,247},{"wifi_diag","slot_1",LegacyType::String,247},
 {"wifi_diag","slot_2",LegacyType::String,247},{"wifi_diag","slot_3",LegacyType::String,247},
 {"wifi_diag","slot_4",LegacyType::String,247},{"wifi_diag","slot_5",LegacyType::String,248},
 {"wifi_diag","slot_6",LegacyType::String,247},{"wifi_diag","slot_7",LegacyType::String,248},
 {"wifi_diag","slot_8",LegacyType::String,248},{"wifi_diag","slot_9",LegacyType::String,248},
 {"wifi_diag","slot_10",LegacyType::String,247},{"wifi_diag","slot_11",LegacyType::String,247},
 {"wifi_diag","slot_12",LegacyType::String,247},{"wifi_diag","slot_13",LegacyType::String,247},
 {"wifi_diag","slot_14",LegacyType::String,247},{"wifi_diag","slot_15",LegacyType::String,247},
 {"wifi_diag","slot_16",LegacyType::String,247},{"wifi_diag","slot_17",LegacyType::String,247},
 {"wifi_diag","slot_18",LegacyType::String,250},{"wifi_diag","slot_19",LegacyType::String,247},
 {"wifi_diag","head",LegacyType::I32,4},{"wifi_diag","count",LegacyType::I32,4}
};
// RAM-only qualification outcome; no new NVS key or capacity authority.
enum class Qualification:uint8_t { Unknown,Ready,Disabled,NotAuthorized,LeaseOrSafety,
  Layout,Unsafe,LegacyMetadataOrIo,Stats,Capacity,RetirementIo };
class NvsAdapter {
  NvsGuard g_;Board board_;bool enabled_,held_=false,fault_=false,qualified_=false;
  nvs_handle_t handle_=0;bool writable_=false;Qualification qualification_=Qualification::Unknown;
  static const char* key(uint8_t s){static const char* const names[]={"ctx","fail0","fail1","state","cp0","cp1"};return s<6?names[s]:nullptr;}
  bool safe()const{return !fault_&&g_.safe(g_.arg)&&g_.writers_retired(g_.arg);}
  bool fail(bool retirement=false){fault_=true;g_.uncertain(g_.arg,retirement);return false;}
  bool stats(size_t&n){nvs_stats_t x{};if(!safe()||nvs_get_stats(nullptr,&x)!=ESP_OK||x.available_entries>x.free_entries||x.free_entries>x.total_entries)return false;n=x.available_entries;return true;}
  static size_t entries(size_t n){return 2+(n+31)/32;}
  bool take(){if(!enabled_||fault_||!g_.take(g_.arg))return false;held_=true;if(!safe()){give();return false;}esp_err_t e=nvs_open("ota_diag",NVS_READONLY,&handle_);if(e==ESP_ERR_NVS_NOT_FOUND){handle_=0;return true;}if(e!=ESP_OK){fail();give();return false;}return true;}
  void give(){if(handle_)nvs_close(handle_);handle_=0;writable_=false;if(held_)g_.give(g_.arg);held_=false;}
  Read read(uint8_t s,uint8_t*out,size_t cap,size_t*got){*got=0;if(!safe()||s>=6)return Read::Error;if(!handle_)return Read::Missing;size_t n=0;esp_err_t e=nvs_get_blob(handle_,key(s),nullptr,&n);if(e==ESP_ERR_NVS_NOT_FOUND)return Read::Missing;if(e!=ESP_OK||n!=slot_size(s)||n>cap)return Read::Error;*got=n;e=nvs_get_blob(handle_,key(s),out,got);return e==ESP_OK&&*got==n?Read::Ok:Read::Error;}
  bool missing_entries(size_t&missing){missing=handle_?0:1;for(uint8_t i=0;i<6;++i){size_t len=0;esp_err_t e=handle_?nvs_get_blob(handle_,key(i),nullptr,&len):ESP_ERR_NVS_NOT_FOUND;if(e==ESP_ERR_NVS_NOT_FOUND)missing+=entries(slot_size(i));else if(e!=ESP_OK||len!=slot_size(i))return fail();}return true;}
  bool capacity(uint8_t s,size_t n){if(!qualified_||!held_||!safe()||s>=6||n!=slot_size(s))return false;
    size_t missing=0;if(!missing_entries(missing))return false;
    // Reserve a future768B policy plus its worst two-chunk replacement (27+27).
    // Once the actual policy is verified present, stats already count its live
    // footprint; reserve only its27-entry replacement. Never use a RAM guess.
    const size_t reserve=board_==Board::Sense?96:64;
    const size_t future_and_peak=board_==Board::Sense?(g_.policy_allocated(g_.arg)?27:54):10;
    size_t available=0;if(!stats(available))return false;
    // Final-footprint qualification: every missing permanent profile record,
    // missing permanent policy and the maximum serialized replacement peak.
    if(available<reserve+missing+future_and_peak)return false;
    // Actual-write admission separately reserves the new blob while its old
    // version remains live. Never add this peak to the future replacement peak.
    const size_t missing_policy=board_==Board::Sense&&!g_.policy_allocated(g_.arg)?27:0;
    if(available<reserve+missing_policy+entries(n)+(handle_?0:1))return false;
    if(!writable_){if(handle_)nvs_close(handle_);handle_=0;if(!safe()||nvs_open("ota_diag",NVS_READWRITE,&handle_)!=ESP_OK)return fail();writable_=true;}
    return safe();
  }
  bool erase_diagnostic(uint8_t slot){
    if(!qualified_||!held_||!safe()||!handle_||slot>=6)return false;
    size_t n=0;const auto e=nvs_get_blob(handle_,key(slot),nullptr,&n);
    if(e!=ESP_OK||n!=slot_size(slot))return fail(false);
    // Exact existing namespace/key only. No new allocation, namespace erase,
    // critical-store uncertainty or independent lease is introduced here.
    if(!writable_){nvs_close(handle_);handle_=0;if(!safe()||nvs_open("ota_diag",NVS_READWRITE,&handle_)!=ESP_OK)return fail(false);writable_=true;}
    return safe()&&nvs_erase_key(handle_,key(slot))==ESP_OK?true:fail(false);
  }
  static size_t string_limit(const LegacyKey&k){return !strcmp(k.ns,"sense_err")?256:769;}
  esp_err_t legacy_read(const LegacyKey&k,nvs_handle_t h,uint8_t*b,size_t capacity,size_t&n){
    n=k.bytes;esp_err_t e=ESP_FAIL;
    if(k.type==LegacyType::String){
      n=0;e=nvs_get_str(h,k.key,nullptr,&n);
      if(e!=ESP_OK)return e;
      if(n<2||n>string_limit(k)||n>capacity)return ESP_FAIL;
      const size_t expected=n;e=nvs_get_str(h,k.key,(char*)b,&n);
      // Size succeeded: a full-read NOT_FOUND may be CRC-triggered removal,
      // never a sparse/missing key eligible to ignore.
      if(e!=ESP_OK||n!=expected||b[n-1]!=0||strnlen((char*)b,n)!=n-1)return ESP_FAIL;
      return ESP_OK;
    }
    if(n>capacity)return ESP_FAIL;
    if(k.type==LegacyType::Blob)e=nvs_get_blob(h,k.key,b,&n);
    else if(k.type==LegacyType::U16){uint16_t v;e=nvs_get_u16(h,k.key,&v);if(e==ESP_OK)put16(b,v);}
    else if(k.type==LegacyType::I32){int32_t v;e=nvs_get_i32(h,k.key,&v);if(e==ESP_OK)put32(b,uint32_t(v));}
    else {uint32_t v;e=nvs_get_u32(h,k.key,&v);if(e==ESP_OK)put32(b,v);}
    if(e==ESP_OK&&n!=k.bytes)return ESP_FAIL;
    return e;
  }
  bool bench_read(uint8_t*b,size_t capacity,bool&state_present,bool&valid_present){
    state_present=valid_present=false;
#if defined(HALO_OTA_BENCH_CASE)
    (void)b;(void)capacity;return false; // a fault-capable writer is never eligible
#else
    if(board_!=Board::Sense||capacity<sizeof(retired_case4::state)||!safe())return false;
    nvs_handle_t h=0;esp_err_t e=nvs_open("ota_bench",NVS_READONLY,&h);
    if(e==ESP_ERR_NVS_NOT_FOUND)return true;
    if(e!=ESP_OK)return false;
    struct Close{nvs_handle_t h;~Close(){nvs_close(h);}}close{h};
    size_t n=0;e=nvs_get_str(h,"state",nullptr,&n);
    if(e==ESP_OK){
      if(n!=sizeof(retired_case4::state))return false;
      if(nvs_get_str(h,"state",(char*)b,&n)!=ESP_OK||n!=sizeof(retired_case4::state)||memcmp(b,retired_case4::state,n))return false;
      state_present=true;
    }else if(e!=ESP_ERR_NVS_NOT_FOUND)return false;
    uint8_t valid=0;e=nvs_get_u8(h,"valid",&valid);
    if(e==ESP_OK){if(valid!=1)return false;valid_present=true;}
    else if(e!=ESP_ERR_NVS_NOT_FOUND)return false;
    // Erase valid first, then state. Any other partial/unknown layout refuses.
    if(!state_present&&valid_present)return false;
    // An existing namespace remains a resumed retirement even after both
    // optional keys are gone. Its exact replay guard must still be present.
    n=sizeof(retired_case4::token);
    if(nvs_get_str(h,"c4",(char*)b,&n)!=ESP_OK||n!=sizeof(retired_case4::token)||memcmp(b,retired_case4::token,n))return false;
    return safe();
#endif
  }
  bool bench_erase(const char*key,bool expected_state,bool expected_valid,uint8_t*b,size_t capacity){
    bool state=false,valid=false;
    if(!bench_read(b,capacity,state,valid)||state!=expected_state||valid!=expected_valid||!safe())return fail(true);
    nvs_handle_t h=0;if(nvs_open("ota_bench",NVS_READWRITE,&h)!=ESP_OK)return fail(true);
    bool ok=safe()&&nvs_erase_key(h,key)==ESP_OK&&safe()&&nvs_commit(h)==ESP_OK;
    if(ok){size_t n=0;uint8_t v=0;const auto e=!strcmp(key,"valid")?nvs_get_u8(h,key,&v):nvs_get_str(h,key,nullptr,&n);ok=e==ESP_ERR_NVS_NOT_FOUND&&safe();}
    if(ok&&!strcmp(key,"state")){
      size_t n=sizeof(retired_case4::token);
      ok=nvs_get_str(h,"c4",(char*)b,&n)==ESP_OK&&n==sizeof(retired_case4::token)&&
        !memcmp(b,retired_case4::token,n)&&safe();
    }
    nvs_close(h);return ok?true:fail(true);
  }

 public:
  NvsAdapter(Board b,const NvsGuard&g,bool enabled=false):g_(g),board_(b),enabled_(enabled&&(b==Board::Sense||b==Board::Lcd)&&g.take&&g.give&&g.safe&&g.writers_retired&&g.layout_valid&&g.policy_allocated&&g.uncertain){}
  Qualification last_qualification()const{return qualification_;}
  // Ordinary production qualification does not reclaim legacy keys. Existing
  // bytes consume their actual capacity; only the owned ota_diag namespace may
  // be opened for later journal writes. Journal CRC/lifecycle/ACK rules remain
  // authoritative for opening, capture and eventual acknowledged retirement.
  bool qualify_without_retirement(){
    qualification_=Qualification::Disabled;if(!enabled_)return false;
    qualification_=Qualification::LeaseOrSafety;if(!take())return false;
    struct Release{NvsAdapter&a;~Release(){a.give();}}release{*this};
    qualification_=Qualification::Layout;if(!g_.layout_valid(g_.arg))return false;
    qualification_=Qualification::Unsafe;if(!safe())return false;
    qualification_=Qualification::Capacity;qualified_=true;
    if(!capacity(0,slot_size(0))){qualified_=false;return false;}
    qualification_=Qualification::Ready;return true;
  }
  // The caller has independently preserved the exact snapshot and explicitly
  // authorized this profile. All legacy writers MUST already be disabled.
  // At most44 keys, two validation passes, one commit/readback per exact erase.
  bool qualify_and_retire(bool snapshot_authorized,uint8_t (&scratch)[MAX_RECORD]){
    return qualify_and_retire(snapshot_authorized,scratch,MAX_RECORD,false);
  }
  // Larger LCD legacy strings borrow its EXISTING lease-owned4000B scratch.
  // The optional exact case4 proof is separately authorized for Sense only.
  bool qualify_and_retire(bool snapshot_authorized,uint8_t*scratch,size_t scratch_capacity,bool retire_case4=false){
    qualification_=Qualification::NotAuthorized;if(!snapshot_authorized||!scratch||scratch_capacity<MAX_RECORD||(retire_case4&&board_!=Board::Sense))return false;
    qualification_=Qualification::Disabled;if(!enabled_)return false;
    qualification_=Qualification::LeaseOrSafety;if(!take())return false;
    struct Release{NvsAdapter&a;~Release(){a.give();}}release{*this};
    qualification_=Qualification::Layout;if(!g_.layout_valid(g_.arg))return false;
    qualification_=Qualification::Unsafe;if(!safe())return false;
    const LegacyKey* list=board_==Board::Sense?SENSE_LEGACY:LCD_LEGACY;
    const size_t count=board_==Board::Sense?sizeof(SENSE_LEGACY)/sizeof(*list):sizeof(LCD_LEGACY)/sizeof(*list);
    uint32_t fingerprints[44]{};uint16_t lengths[44]{};uint64_t present=0;size_t reclaimable=0;
    for(size_t i=0;i<count;++i){qualification_=Qualification::Unsafe;if(!safe())return false;qualification_=Qualification::LegacyMetadataOrIo;nvs_handle_t h=0;esp_err_t e=nvs_open(list[i].ns,NVS_READONLY,&h);if(e==ESP_ERR_NVS_NOT_FOUND)continue;if(e!=ESP_OK)return fail();size_t n=0;e=legacy_read(list[i],h,scratch,scratch_capacity,n);nvs_close(h);if(e==ESP_ERR_NVS_NOT_FOUND)continue;if(e!=ESP_OK)return fail();fingerprints[i]=crc32(scratch,n);lengths[i]=uint16_t(n);present|=uint64_t(1)<<i;reclaimable+=(list[i].type==LegacyType::Blob?entries(n):(list[i].type==LegacyType::String?1+(n+31)/32:1));}
    bool bench_state=false,bench_valid=false;
    qualification_=Qualification::LegacyMetadataOrIo;
    if(retire_case4){if(!bench_read(scratch,scratch_capacity,bench_state,bench_valid))return fail();reclaimable+=(bench_state?9:0)+(bench_valid?1:0);}
    qualification_=Qualification::Stats;size_t before=0;if(!stats(before))return fail();
    const size_t reserve=board_==Board::Sense?96:64;
    const size_t policy_peak=board_==Board::Sense?(g_.policy_allocated(g_.arg)?27:54):10;
    // Missing profile entries include the namespace when absent. No erase starts
    // for a known-insufficient inventory, including resumed partial setup.
    qualification_=Qualification::Capacity;size_t missing=0;if(!missing_entries(missing))return false;
    if(before+reclaimable<reserve+missing+policy_peak)return false;
    if(bench_valid){qualification_=Qualification::RetirementIo;if(!bench_erase("valid",true,true,scratch,scratch_capacity))return false;}
    if(bench_state){qualification_=Qualification::RetirementIo;if(!bench_erase("state",true,false,scratch,scratch_capacity))return false;}
    for(size_t i=0;i<count;++i)if(present&(uint64_t(1)<<i)){qualification_=Qualification::Unsafe;if(!safe())return false;qualification_=Qualification::RetirementIo;nvs_handle_t h=0;esp_err_t e=nvs_open(list[i].ns,NVS_READWRITE,&h);if(e!=ESP_OK)return fail();size_t n=0;e=legacy_read(list[i],h,scratch,scratch_capacity,n);bool ok=e==ESP_OK&&n==lengths[i]&&crc32(scratch,n)==fingerprints[i]&&safe()&&nvs_erase_key(h,list[i].key)==ESP_OK&&safe()&&nvs_commit(h)==ESP_OK;
      if(ok){e=legacy_read(list[i],h,scratch,scratch_capacity,n);ok=e==ESP_ERR_NVS_NOT_FOUND&&safe();}nvs_close(h);if(!ok)return fail(true);}
    qualification_=Qualification::Stats;size_t available=0;if(!stats(available))return fail(true);
    // Initial full profile incl namespace + future policy/replacement. If this
    // fails after a partial retirement, preserve the evidence and stay disabled.
    qualification_=Qualification::Capacity;qualified_=true;if(!capacity(0,slot_size(0))){qualified_=false;return false;}qualification_=Qualification::Ready;return true;
  }
  Store store(){return Store{this,enabled_,
    [](void*p){return ((NvsAdapter*)p)->take();},[](void*p){((NvsAdapter*)p)->give();},
    [](void*p){return ((NvsAdapter*)p)->safe();},
    [](void*p,uint8_t s,uint8_t*b,size_t n,size_t*g){return ((NvsAdapter*)p)->read(s,b,n,g);},
    [](void*p,uint8_t s,const uint8_t*b,size_t n){auto&a=*(NvsAdapter*)p;if(!a.safe()||!a.writable_)return false;return nvs_set_blob(a.handle_,key(s),b,n)==ESP_OK?true:a.fail();},
    [](void*p){auto&a=*(NvsAdapter*)p;if(!a.safe())return false;return nvs_commit(a.handle_)==ESP_OK?true:a.fail();},
    [](void*p,uint8_t s,size_t n){return ((NvsAdapter*)p)->capacity(s,n);},
    [](void*p,uint8_t s){return ((NvsAdapter*)p)->erase_diagnostic(s);}};}
};
} // namespace halo_diag

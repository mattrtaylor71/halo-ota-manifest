#pragma once
#include "DurableOtaPolicy.h"
#include <nvs.h>

namespace durable_ota {
// Existing ota_coord namespace; no namespace creation, erase, repair or fallback
// record. The caller supplies its existing shared writer lease and original
// deadline. Optional telemetry content/ACK status is never policy authority.
struct NvsPolicyGuard {
  void* owner;
  bool (*take)(void*);
  void (*give)(void*);
  bool (*read_safe)(void*);
  bool (*write_safe)(void*); // same original deadline + actual current VALID
  size_t (*other_reserved_entries)(void*); // missing planned profile, held lease
  void (*uncertain)(void*);
};
enum class StorageStatus:uint8_t {
  READY, FEATURE_OFF, BUSY, UNSAFE, NAMESPACE_MISSING, READ_ERROR,
  CAPACITY, WRITE_ERROR, COMMIT_ERROR, VERIFY_ERROR
};
constexpr const char* kPolicyNamespace="ota_coord";
constexpr const char* kPolicyKey="retry_v1";
constexpr size_t kPolicyWorstEntries=27, kEssentialEntries=96;

class NvsPolicyTransaction {
  NvsPolicyGuard g_;
  bool owned_=false, attempted_=false, verified_=false;
  nvs_handle_t handle_=0;
  StorageStatus status_=StorageStatus::FEATURE_OFF;
  bool safe()const{return owned_&&g_.read_safe(g_.owner);}
  bool capacity() {
    if(!safe()||!g_.write_safe(g_.owner)){status_=StorageStatus::UNSAFE;return false;}
    nvs_stats_t stats{};
    if(nvs_get_stats(nullptr,&stats)!=ESP_OK || stats.available_entries>stats.free_entries ||
       stats.free_entries>stats.total_entries){status_=StorageStatus::CAPACITY;return false;}
    size_t size=0;
    const esp_err_t existing=nvs_get_blob(handle_,kPolicyKey,nullptr,&size);
    if(existing!=ESP_OK && existing!=ESP_ERR_NVS_NOT_FOUND){status_=StorageStatus::READ_ERROR;return false;}
    if(existing==ESP_OK && size!=kRecordBytes){status_=StorageStatus::READ_ERROR;return false;}
    const size_t other=g_.other_reserved_entries(g_.owner);
    // Initial allocation reserves its permanent bytes AND a later replacement.
    // An existing record is already charged in stats; retain only its peak.
    const size_t policy=(existing==ESP_OK)?kPolicyWorstEntries:2*kPolicyWorstEntries;
    if(other>SIZE_MAX-kEssentialEntries-policy ||
       stats.available_entries<kEssentialEntries+policy+other){status_=StorageStatus::CAPACITY;return false;}
    if(!safe()||!g_.write_safe(g_.owner)){status_=StorageStatus::UNSAFE;return false;}
    return true;
  }
public:
  explicit NvsPolicyTransaction(const NvsPolicyGuard&g,bool enabled):g_(g) {
    if(!enabled||!g.take||!g.give||!g.read_safe||!g.write_safe||
       !g.other_reserved_entries||!g.uncertain)return;
    status_=StorageStatus::BUSY;if(!g_.take(g_.owner))return;
    owned_=true;
    if(!safe()){status_=StorageStatus::UNSAFE;return;}
    const esp_err_t e=nvs_open(kPolicyNamespace,NVS_READONLY,&handle_);
    status_=e==ESP_OK?StorageStatus::READY:
      e==ESP_ERR_NVS_NOT_FOUND?StorageStatus::NAMESPACE_MISSING:StorageStatus::READ_ERROR;
  }
  ~NvsPolicyTransaction(){
    if(handle_)nvs_close(handle_);
    // A returned failed set/commit/readback can still have durable effects.
    // Do not retry with an old allowance in this boot.
    if(attempted_&&!verified_)g_.uncertain(g_.owner);
    if(owned_)g_.give(g_.owner);
  }
  NvsPolicyTransaction(const NvsPolicyTransaction&)=delete;
  NvsPolicyTransaction& operator=(const NvsPolicyTransaction&)=delete;
  StorageStatus status()const{return status_;}
  bool ready()const{return status_==StorageStatus::READY&&handle_&&safe();}
  bool attempted_write()const{return attempted_;}
  ReadResult read(uint8_t*out,size_t capacity,size_t&size) {
    size=0;if(!ready()||!out)return ReadResult::ERROR;
    esp_err_t e=nvs_get_blob(handle_,kPolicyKey,nullptr,&size);
    if(e==ESP_ERR_NVS_NOT_FOUND){size=0;return ReadResult::ABSENT;}
    if(e!=ESP_OK||size!=kRecordBytes||capacity<size){status_=StorageStatus::READ_ERROR;return ReadResult::ERROR;}
    if(!safe()){status_=StorageStatus::UNSAFE;return ReadResult::ERROR;}
    e=nvs_get_blob(handle_,kPolicyKey,out,&size);
    if(e!=ESP_OK||size!=kRecordBytes||!safe()){status_=StorageStatus::READ_ERROR;return ReadResult::ERROR;}
    return ReadResult::PRESENT;
  }
  size_t write(const uint8_t*bytes,size_t size) {
    if(!ready()||attempted_||!bytes||size!=kRecordBytes||!capacity())return 0;
    nvs_close(handle_);handle_=0;
    if(!safe()||!g_.write_safe(g_.owner)){status_=StorageStatus::UNSAFE;return 0;}
    if(nvs_open(kPolicyNamespace,NVS_READWRITE,&handle_)!=ESP_OK){status_=StorageStatus::WRITE_ERROR;return 0;}
    if(!safe()||!g_.write_safe(g_.owner)){status_=StorageStatus::UNSAFE;return 0;}
    attempted_=true;
    if(nvs_set_blob(handle_,kPolicyKey,bytes,size)!=ESP_OK){status_=StorageStatus::WRITE_ERROR;return 0;}
    if(!safe()||!g_.write_safe(g_.owner)){status_=StorageStatus::UNSAFE;return 0;}
    if(nvs_commit(handle_)!=ESP_OK){status_=StorageStatus::COMMIT_ERROR;return 0;}
    if(!safe()||!g_.write_safe(g_.owner)){status_=StorageStatus::UNSAFE;return 0;}
    return size;
  }
  // Only core commit's exact candidate readback may establish this result.
  bool verified(bool ok){
    verified_=ok&&attempted_&&ready()&&g_.write_safe(g_.owner);
    if(!verified_&&status_==StorageStatus::READY)status_=StorageStatus::VERIFY_ERROR;
    return verified_;
  }
  void invalid_read(){if(status_==StorageStatus::READY)status_=StorageStatus::VERIFY_ERROR;}
};

inline ReadResult load_nvs(const NvsPolicyGuard&guard,bool enabled,Record&record,
                          bool&allowed,uint8_t(&scratch)[kRecordBytes],StorageStatus&status) {
  NvsPolicyTransaction tx(guard,enabled);
  const ReadResult result=load(tx,record,allowed,scratch);
  if(result==ReadResult::ERROR)tx.invalid_read();status=tx.status();return result;
}
inline bool commit_nvs(const NvsPolicyGuard&guard,bool enabled,const Record&candidate,
                       Record&live,bool&allowed,uint8_t(&scratch)[kRecordBytes],
                       StorageStatus&status,bool first=false) {
  NvsPolicyTransaction tx(guard,enabled);
  const bool ok=commit(tx,candidate,live,allowed,scratch,first);
  const bool verified=tx.verified(ok);if(ok&&!verified)allowed=false;
  status=tx.status();return ok&&verified;
}
} // namespace durable_ota

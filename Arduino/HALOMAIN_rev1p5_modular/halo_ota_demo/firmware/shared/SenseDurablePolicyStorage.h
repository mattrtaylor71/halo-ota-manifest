#pragma once
#include "DurableOtaPolicyNvs.h"
#ifndef HALO_DURABLE_OTA_POLICY
#define HALO_DURABLE_OTA_POLICY 0
#endif
// Included after the existing Sense main-task owners and shared NVS flag.
// Idle calls retain the original no-apply guard. Only the synchronous applier
// callback may hold SelfBeginLease after it has returned the prior HTTP and OTA
// handles; it cannot be retained across esp_ota_begin or another callback.
namespace sense_policy {
class StorageScope;
static StorageScope* storage_scope=nullptr;
static bool storage_uncertain=false;
static const void* self_begin_owner=nullptr;
class SelfBeginLease {
  const void* owner_;bool held_=false;
public:
  explicit SelfBeginLease(const void* owner):owner_(owner) {
    if(owner && !self_begin_owner && !storage_scope && g_ota_apply_in_progress &&
       !g_lcd_ota_task_running && !g_lcd_ota_proxy_owns_uart) {
      self_begin_owner=owner;held_=true;
    }
  }
  ~SelfBeginLease(){if(held_&&self_begin_owner==owner_)self_begin_owner=nullptr;}
  bool active()const{return held_&&self_begin_owner==owner_;}
  SelfBeginLease(const SelfBeginLease&)=delete;SelfBeginLease&operator=(const SelfBeginLease&)=delete;
};
class StorageScope {
  uint32_t original_start_,original_budget_,entered_;
  bool active_=false,held_=false;
  const void* begin_owner_=nullptr;
  bool clock_open()const {
    return active_ && original_budget_ && original_budget_<=0x7fffffffUL &&
      uint32_t(millis()-original_start_)<original_budget_ &&
      uint32_t(millis()-entered_)<1500;
  }
  bool read_safe()const {
    return held_ && clock_open() && !storage_uncertain &&
      !g_nvs_reclaim_uncertain && !g_ota_storage_uncertain &&
      !g_coord_credit_uncertain && !g_serial_install_uncertain &&
      !xPortInIsrContext();
  }
  bool write_safe()const {
    return read_safe() && nvs_capacity_image_valid() &&
      (!g_ota_apply_in_progress || (begin_owner_ && begin_owner_==self_begin_owner)) && !g_lcd_ota_task_running && !g_lcd_ota_proxy_owns_uart &&
      !halo_primary_user_work_busy() &&
      !upload_inflight && !http_inflight && !voice_recording_active &&
      !scan_ui_inflight && !dish_scan_inflight && !foreground_active &&
      !current_job.active && !upload_queue_count() &&
      !(op_queue && uxQueueMessagesWaiting(op_queue)) && !halo_provisioning_active();
  }
  size_t missing_profile()const {
    // Capacity metadata only. CRC/content, cloud ACK and journal availability
    // never authorize or deny a policy decision. An unknown shape reserves the
    // complete future footprint conservatively; it is not a new error latch.
    if(!read_safe())return SIZE_MAX;
    nvs_handle_t h=0;const esp_err_t opened=nvs_open("ota_diag",NVS_READONLY,&h);
    if(opened==ESP_ERR_NVS_NOT_FOUND)return 49;
    if(opened!=ESP_OK)return 49;
    const char* keys[]={"ctx","fail0","fail1","state","cp0","cp1"};
    size_t missing=0;
    for(unsigned i=0;i<6;++i){
      if(!read_safe()){nvs_close(h);return SIZE_MAX;}
      size_t n=0;const esp_err_t e=nvs_get_blob(h,keys[i],nullptr,&n);
      const size_t expected=i<4?256:64;
      if(e==ESP_ERR_NVS_NOT_FOUND)missing+=2+(expected+31)/32;
      else if(e!=ESP_OK||n!=expected){nvs_close(h);return 49;}
    }
    nvs_close(h);return read_safe()?missing:SIZE_MAX;
  }
public:
  StorageScope(uint32_t original_start,uint32_t original_budget,const void* begin_owner=nullptr):
      original_start_(original_start),original_budget_(original_budget),entered_(millis()),begin_owner_(begin_owner) {
    if(HALO_DURABLE_OTA_POLICY!=1 || storage_scope || !original_budget ||
       original_budget>0x7fffffffUL || uint32_t(millis()-original_start)>=original_budget ||
       (begin_owner && (begin_owner!=self_begin_owner || !g_ota_apply_in_progress)))return;
    active_=true;storage_scope=this;
  }
  ~StorageScope(){if(active_)storage_scope=nullptr;}
  StorageScope(const StorageScope&)=delete;StorageScope&operator=(const StorageScope&)=delete;
  bool active()const{return active_&&clock_open();}
  durable_ota::NvsPolicyGuard guard(){return {this,
    [](void*p){auto&s=*static_cast<StorageScope*>(p);if(!s.clock_open()||s.held_||g_optional_nvs_writer.test_and_set(std::memory_order_acquire))return false;s.held_=true;return true;},
    [](void*p){auto&s=*static_cast<StorageScope*>(p);if(s.held_){s.held_=false;g_optional_nvs_writer.clear(std::memory_order_release);}},
    [](void*p){return static_cast<StorageScope*>(p)->read_safe();},
    [](void*p){return static_cast<StorageScope*>(p)->write_safe();},
    [](void*p){return static_cast<StorageScope*>(p)->missing_profile();},
    [](void*){storage_uncertain=true;g_nvs_reclaim_uncertain.store(true);}
  };}
};
// Caller already owns the SAME shared NVS lease and its diagnostic safe/deadline
// check. Observational typed+CRC proof only, using caller-borrowed768B scratch;
// this never loads/refunds policy allowance or clears any uncertain state.
inline bool allocated_held(uint8_t(&scratch)[durable_ota::kRecordBytes]) {
  if(HALO_DURABLE_OTA_POLICY!=1)return false;
  nvs_handle_t h=0;if(nvs_open(durable_ota::kPolicyNamespace,NVS_READONLY,&h)!=ESP_OK)return false;
  size_t n=0;esp_err_t e=nvs_get_blob(h,durable_ota::kPolicyKey,nullptr,&n);
  if(e==ESP_OK&&n==sizeof(scratch))e=nvs_get_blob(h,durable_ota::kPolicyKey,scratch,&n);
  nvs_close(h);if(e!=ESP_OK||n!=sizeof(scratch))return false;
  durable_ota::Record record{};return durable_ota::decode(scratch,n,record);
}
} // namespace sense_policy

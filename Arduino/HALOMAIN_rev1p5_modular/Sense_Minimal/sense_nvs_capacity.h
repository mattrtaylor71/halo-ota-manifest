#pragma once
#include <Arduino.h>
#include <atomic>
#include <nvs.h>
#include <esp_ota_ops.h>
#include <bootloader_common.h>

// All owned optional writers share this nonblocking admission. SDK NVS users
// are outside the lock; a successful stats check is not an allocation promise.
static constexpr size_t NVS_ESSENTIAL_CUSHION_ENTRIES = 96;
static std::atomic_flag g_optional_nvs_writer = ATOMIC_FLAG_INIT;
static std::atomic<uint32_t> g_optional_nvs_skipped{0};
static bool g_nvs_reclaim_attempted = false;
static bool g_nvs_essential_space_prepared = false;
static std::atomic<bool> g_nvs_reclaim_uncertain{false};

class NvsCapacityLease {
  bool owned_;
 public:
  NvsCapacityLease():owned_(!g_optional_nvs_writer.test_and_set(std::memory_order_acquire)){}
  ~NvsCapacityLease(){if(owned_)g_optional_nvs_writer.clear(std::memory_order_release);}
  explicit operator bool()const{return owned_;}
  NvsCapacityLease(const NvsCapacityLease&)=delete;
  NvsCapacityLease& operator=(const NvsCapacityLease&)=delete;
};

static bool nvs_capacity_available(size_t& available) {
  nvs_stats_t stats{};
  if(nvs_get_stats(nullptr,&stats)!=ESP_OK || stats.available_entries>stats.free_entries ||
      stats.free_entries>stats.total_entries)return false;
  available=stats.available_entries;return true;
}
static bool nvs_capacity_has_room(size_t new_entries) {
  size_t available=0;
  return new_entries<=SIZE_MAX-NVS_ESSENTIAL_CUSHION_ENTRIES &&
      nvs_capacity_available(available) && available>=NVS_ESSENTIAL_CUSHION_ENTRIES+new_entries;
}

class NvsOptionalWrite {
  NvsCapacityLease lease_;
  bool admitted_;
 public:
  explicit NvsOptionalWrite(size_t new_entries):
      admitted_(bool(lease_) && !g_nvs_reclaim_uncertain && nvs_capacity_has_room(new_entries)) {
    if(!admitted_)g_optional_nvs_skipped.fetch_add(1,std::memory_order_relaxed);
  }
  explicit operator bool()const{return admitted_;}
};

static const esp_partition_t* nvs_capacity_known_layout() {
  unsigned count=0,seen=0;
  esp_partition_iterator_t it=esp_partition_find(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_ANY,nullptr);
  while(it){
    const esp_partition_t* p=esp_partition_get(it);
    const bool expected=p && p->size==0x1e0000 &&
        ((p->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_0 && p->address==0x10000) ||
         (p->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_1 && p->address==0x1f0000));
    const unsigned bit=p && p->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_0?1:2;
    if(!expected || (seen&bit) || ++count>2){esp_partition_iterator_release(it);return nullptr;}
    seen|=bit;it=esp_partition_next(it);
  }
  if(count!=2 || seen!=3)return nullptr;
  const esp_partition_t* metadata=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,nullptr);
  const esp_partition_t* settings=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,nullptr);
  if(!metadata || metadata->address!=0xe000 || metadata->size!=8192 || metadata->encrypted ||
      !settings || settings->address!=0x9000 || settings->size!=0x5000)return nullptr;
  return metadata;
}
static bool nvs_capacity_image_valid() {
  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* selected=esp_ota_get_boot_partition();
  esp_ota_img_states_t state;
  if(!running || !selected || running->address!=selected->address ||
      esp_ota_get_state_partition(running,&state)!=ESP_OK || state!=ESP_OTA_IMG_VALID)return false;
  const esp_partition_t* metadata=nvs_capacity_known_layout();
  esp_ota_select_entry_t records[2];static_assert(sizeof(records)==64,"two exact OTA bank headers");
  if(!metadata || esp_partition_read(metadata,0,&records[0],sizeof(records[0]))!=ESP_OK ||
      esp_partition_read(metadata,4096,&records[1],sizeof(records[1]))!=ESP_OK)return false;
  // get_state_partition returns the first matching bank, which alone can
  // misclassify older VALID/newer PENDING records for the same application.
  const bool valid0=bootloader_common_ota_select_valid(&records[0]);
  const bool valid1=bootloader_common_ota_select_valid(&records[1]);
  if(valid0 && valid1 && ((records[0].ota_seq-1)%2)==((records[1].ota_seq-1)%2))return false;
  const int active=bootloader_common_get_active_otadata(records);
  if(active<0 || records[active].ota_state!=ESP_OTA_IMG_VALID)return false;
  const unsigned slot=(records[active].ota_seq-1)%2;
  selected=esp_ota_get_boot_partition();
  return selected && selected->address==running->address && running->type==ESP_PARTITION_TYPE_APP &&
      running->subtype==(slot?ESP_PARTITION_SUBTYPE_APP_OTA_1:ESP_PARTITION_SUBTYPE_APP_OTA_0) &&
      running->address==(slot?0x1f0000:0x10000) && running->size==0x1e0000;
}

static size_t nvs_capacity_string_entries(size_t bytes_without_nul) {
  return 1+(bytes_without_nul+1+31)/32;
}

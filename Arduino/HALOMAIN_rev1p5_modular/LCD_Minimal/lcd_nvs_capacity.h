#ifndef LCD_NVS_CAPACITY_H
#define LCD_NVS_CAPACITY_H
#include <Arduino.h>
#include <atomic>
#include <nvs.h>
#include <esp_ota_ops.h>
#include <bootloader_common.h>

// 64 entries cover the bounded essential transitions (three <=104-byte
// records, counter/tombstone replacements and the existing arm/disarm), while
// diagnostic strings need their complete temporary footprint in addition.
static constexpr size_t LCD_NVS_ESSENTIAL_RESERVE = 64;
static constexpr size_t LCD_NVS_DIAGNOSTIC_MAX_BYTES = 768;
static constexpr int32_t LCD_NVS_RETAIN_DIAGNOSTIC_POSITIONS = 8;
// A cooperative limit: an SDK call already in progress cannot be cancelled.
// Nested storage/reclamation uses the SAME deadline and starts no new mutation
// after it. An OTA caller additionally caps this against its original budget.
class LcdNvsDeadline {
  uint32_t end_;
  explicit LcdNvsDeadline(uint32_t end):end_(end){}
 public:
  LcdNvsDeadline():end_((uint32_t)millis()+2000){}
  static LcdNvsDeadline fromAttempt(uint32_t start,uint32_t budget) {
    const uint32_t now=millis(),elapsed=now-start;
    const uint32_t remaining=elapsed<budget?budget-elapsed:0;
    return LcdNvsDeadline(now+(remaining<2000?remaining:2000));
  }
  bool live() const {return (int32_t)(end_-(uint32_t)millis())>0;}
};
static std::atomic_flag g_lcd_nvs_writer = ATOMIC_FLAG_INIT;
static std::atomic<bool> g_lcd_nvs_uncertain{false};
static std::atomic<uint32_t> g_lcd_nvs_optional_skipped{0};
static bool g_lcd_nvs_reclaim_attempted = false;
// Only the nonblocking writer lease owner uses this scratch area. Keeping it
// out of the 12 KiB UART stack avoids adding a 4 KiB cleanup frame to JSON/OTA.
static char g_lcd_nvs_diagnostic_scratch[4000];
class LcdNvsLease {
  bool owned_;
 public:
  LcdNvsLease():owned_(!g_lcd_nvs_writer.test_and_set(std::memory_order_acquire)){}
  ~LcdNvsLease(){if(owned_)g_lcd_nvs_writer.clear(std::memory_order_release);}
  explicit operator bool() const {return owned_;}
  LcdNvsLease(const LcdNvsLease&)=delete;
  LcdNvsLease& operator=(const LcdNvsLease&)=delete;
};
static bool lcd_nvs_available(size_t& available) {
  nvs_stats_t stats{};
  if(nvs_get_stats(nullptr,&stats)!=ESP_OK || stats.available_entries>stats.free_entries ||
      stats.free_entries>stats.total_entries)return false;
  available=stats.available_entries;return true;
}
static size_t lcd_nvs_string_entries(size_t bytes) {return 1+(bytes+1+31)/32;}
static size_t lcd_nvs_blob_entries(size_t bytes) {return 2+(bytes+31)/32;}
class LcdNvsOptionalWrite {
  LcdNvsLease lease_;
  bool admitted_;
 public:
  explicit LcdNvsOptionalWrite(size_t peak_entries):admitted_(false) {
    size_t available=0;
    admitted_=bool(lease_) && !g_lcd_nvs_uncertain &&
        peak_entries<=SIZE_MAX-LCD_NVS_ESSENTIAL_RESERVE &&
        lcd_nvs_available(available) && available>=LCD_NVS_ESSENTIAL_RESERVE+peak_entries;
    if(!admitted_)g_lcd_nvs_optional_skipped.fetch_add(1,std::memory_order_relaxed);
  }
  explicit operator bool() const {return admitted_;}
};

static const esp_partition_t* lcd_nvs_known_layout() {
  unsigned count=0,seen=0;
  esp_partition_iterator_t it=esp_partition_find(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_ANY,nullptr);
  while(it){
    const esp_partition_t* p=esp_partition_get(it);
    const bool expected=p && p->size==0x280000 &&
        ((p->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_0 && p->address==0x10000) ||
         (p->subtype==ESP_PARTITION_SUBTYPE_APP_OTA_1 && p->address==0x290000));
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
static bool lcd_nvs_image_valid() {
  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* selected=esp_ota_get_boot_partition();
  esp_ota_img_states_t state;
  if(!running || !selected || running->address!=selected->address ||
      esp_ota_get_state_partition(running,&state)!=ESP_OK || state!=ESP_OTA_IMG_VALID)return false;
  const esp_partition_t* metadata=lcd_nvs_known_layout();
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
      running->address==(slot?0x290000:0x10000) && running->size==0x280000;
}

// Caller owns LcdNvsLease. Only diagnostic slots outside the retained stored
// positions are candidates. No namespace clear, list/settings or history keys.
// A non-null read exercises NVS's variable-payload CRC path. Size-only reads
// are insufficient. The final NUL must be the sole terminator in the payload.
static esp_err_t lcd_nvs_read_diagnostic_string(nvs_handle_t h,const char* key,
    char* bytes,size_t capacity,size_t& length,const LcdNvsDeadline& deadline) {
  if(!deadline.live())return ESP_FAIL;
  length=0;esp_err_t err=nvs_get_str(h,key,nullptr,&length);
  if(err!=ESP_OK)return err;
  if(!length || length>capacity || !deadline.live())return ESP_FAIL;
  const size_t expected=length;
  err=nvs_get_str(h,key,bytes,&length);
  // IDF can discard a CRC-bad item during this full read and report NOT_FOUND.
  // Once size succeeded, that is an I/O/validation failure, never a sparse hole.
  if(err!=ESP_OK)return ESP_FAIL;
  if(length!=expected || bytes[length-1]!=0 || strnlen(bytes,length)!=length-1)return ESP_FAIL;
  return ESP_OK;
}
static bool lcd_nvs_empty_diagnostic_slots(nvs_handle_t h,const LcdNvsDeadline& deadline) {
  for(int i=0;i<20;++i){
    if(!deadline.live())return false;
    char key[12];snprintf(key,sizeof(key),"slot_%d",i);size_t length=0;
    if(nvs_get_str(h,key,nullptr,&length)!=ESP_ERR_NVS_NOT_FOUND)return false;
  }
  return deadline.live();
}
static bool lcd_nvs_trim_diagnostic(const char* ns,const LcdNvsDeadline& deadline,
    int32_t retained_positions=LCD_NVS_RETAIN_DIAGNOSTIC_POSITIONS) {
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  (void)ns;(void)deadline;(void)retained_positions;return false; // exact retirement owns these keys
#else
  if(!deadline.live() || retained_positions<0 || retained_positions>8)return false;
  nvs_handle_t h=0;esp_err_t err=nvs_open(ns,NVS_READONLY,&h);
  if(err==ESP_ERR_NVS_NOT_FOUND)return true;
  if(err!=ESP_OK)return false;
  int32_t head=-1,count=-1;
  const esp_err_t he=nvs_get_i32(h,"head",&head),ce=nvs_get_i32(h,"count",&count);
  if(he==ESP_ERR_NVS_NOT_FOUND && ce==ESP_ERR_NVS_NOT_FOUND){
    const bool empty=lcd_nvs_empty_diagnostic_slots(h,deadline);nvs_close(h);return empty;
  }
  if(he!=ESP_OK || ce!=ESP_OK || head<0 || head>=20 || count<0 || count>20){nvs_close(h);return false;}
  const int32_t keep=count<retained_positions?count:retained_positions;
  bool retained[20]={};
  for(int32_t i=0;i<keep;++i)retained[(head-1-i+40)%20]=true;
  // Validate complete present strings before the first mutation. Retain the
  // last eight STORED POSITIONS, including holes; count denotes positions, not
  // extant strings or a proven chronology after partial optional writes.
  bool present[20]={};char key[12];
  for(int i=0;i<20;++i){
    snprintf(key,sizeof(key),"slot_%d",i);size_t length=0;
    err=lcd_nvs_read_diagnostic_string(h,key,g_lcd_nvs_diagnostic_scratch,
        sizeof(g_lcd_nvs_diagnostic_scratch),length,deadline);
    if(err==ESP_ERR_NVS_NOT_FOUND)continue;
    if(err!=ESP_OK){nvs_close(h);return false;}
    present[i]=true;
  }
  nvs_close(h);h=0;
  if(!deadline.live() || nvs_open(ns,NVS_READWRITE,&h)!=ESP_OK)return false;
  // Publish the smaller diagnostic suffix first. A power loss before erasing
  // old entries loses capacity reclamation, never points the ring at new data.
  if(count!=keep){
    if(!deadline.live() || nvs_set_i32(h,"count",keep)!=ESP_OK ||
        !deadline.live() || nvs_commit(h)!=ESP_OK){nvs_close(h);return false;}
    int32_t readback=-1;
    if(nvs_get_i32(h,"count",&readback)!=ESP_OK || readback!=keep){nvs_close(h);return false;}
  }
  for(int i=0;i<20;++i)if(present[i] && !retained[i]){
    snprintf(key,sizeof(key),"slot_%d",i);
    if(!deadline.live() || nvs_erase_key(h,key)!=ESP_OK ||
        !deadline.live() || nvs_commit(h)!=ESP_OK){nvs_close(h);return false;}
    size_t length=0;
    if(nvs_get_str(h,key,nullptr,&length)!=ESP_ERR_NVS_NOT_FOUND){nvs_close(h);return false;}
  }
  nvs_close(h);return deadline.live();
#endif
}
// One bounded reclamation pass per boot, at most 40 typed diagnostic slot
// deletions. A stats observation is not an allocation guarantee; callers still
// check the actual set/commit/readback. This helper never takes the lease again.
static bool lcd_nvs_prepare_essential(size_t new_entries,const LcdNvsDeadline& deadline) {
  size_t available=0;
  if(!deadline.live() || g_lcd_nvs_uncertain || !lcd_nvs_image_valid() ||
      !deadline.live() || !lcd_nvs_available(available) || !deadline.live())return false;
  if(available>=new_entries)return true;
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  return false; // essential stats/admission remain; retirement is separately checked
#else
  if(g_lcd_nvs_reclaim_attempted)return false;
  g_lcd_nvs_reclaim_attempted=true;
  if(!lcd_nvs_trim_diagnostic("err_log",deadline) || !lcd_nvs_trim_diagnostic("wifi_diag",deadline)){
    g_lcd_nvs_uncertain.store(true);return false;
  }
  return deadline.live() && lcd_nvs_available(available) && deadline.live() && available>=new_entries;
#endif
}

static bool lcd_nvs_store_diagnostic(const char* ns,const char* value,
    const LcdNvsDeadline& deadline=LcdNvsDeadline()) {
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  (void)ns;(void)value;(void)deadline;return false; // optional writers cannot regrow
#else
  if(!value)return false;
  const size_t bytes=strnlen(value,LCD_NVS_DIAGNOSTIC_MAX_BYTES+1);
  if(!bytes || bytes>LCD_NVS_DIAGNOSTIC_MAX_BYTES){
    g_lcd_nvs_optional_skipped.fetch_add(1,std::memory_order_relaxed);return false;
  }
  // String footprint, two index scalars and a possible new namespace. No
  // truncated terminal record is ever substituted for an oversized value.
  LcdNvsOptionalWrite admission(lcd_nvs_string_entries(bytes)+3);
  if(!admission || !deadline.live())return false;
  nvs_handle_t h=0;
  if(nvs_open(ns,NVS_READWRITE,&h)!=ESP_OK)return false;
  int32_t head=-1,count=-1;
  const esp_err_t he=nvs_get_i32(h,"head",&head),ce=nvs_get_i32(h,"count",&count);
  if(he==ESP_ERR_NVS_NOT_FOUND && ce==ESP_ERR_NVS_NOT_FOUND){
    if(!lcd_nvs_empty_diagnostic_slots(h,deadline)){nvs_close(h);return false;}
    head=0;count=0;
  }
  else if(he!=ESP_OK || ce!=ESP_OK || head<0 || head>=20 || count<0 || count>20){nvs_close(h);return false;}
  char key[12];snprintf(key,sizeof(key),"slot_%d",head);
  const int32_t next=(head+1)%20,next_count=count<20?count+1:20;
  bool ok=deadline.live() && nvs_set_str(h,key,value)==ESP_OK && deadline.live() && nvs_commit(h)==ESP_OK;
  if(ok)ok=deadline.live() && nvs_set_i32(h,"head",next)==ESP_OK && deadline.live() && nvs_commit(h)==ESP_OK;
  if(ok)ok=deadline.live() && nvs_set_i32(h,"count",next_count)==ESP_OK && deadline.live() && nvs_commit(h)==ESP_OK;
  int32_t read_head=-1,read_count=-1;
  size_t length=0;char readback[LCD_NVS_DIAGNOSTIC_MAX_BYTES+1];
  if(ok)ok=nvs_get_i32(h,"head",&read_head)==ESP_OK && read_head==next &&
      nvs_get_i32(h,"count",&read_count)==ESP_OK && read_count==next_count &&
      lcd_nvs_read_diagnostic_string(h,key,readback,sizeof(readback),length,deadline)==ESP_OK &&
      length==bytes+1 && memcmp(readback,value,length)==0;
  // The write API's successful commit plus checked metadata determines success.
  // Failed/partial diagnostic writes are reported, never logged recursively.
  nvs_close(h);return ok;
#endif
}
static bool lcd_nvs_clear_diagnostic(const char* ns) {
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  (void)ns;return false; // no broad clear of retired inventory
#else
  const LcdNvsDeadline deadline;LcdNvsLease lease;
  if(!lease || g_lcd_nvs_uncertain || !lcd_nvs_image_valid())return false;
  const bool cleared=lcd_nvs_trim_diagnostic(ns,deadline,0);
  if(!cleared)g_lcd_nvs_uncertain.store(true);
  return cleared;
#endif
}
static bool lcd_nvs_known_serial_template(const esp_partition_t* running,
    const LcdNvsDeadline& deadline=LcdNvsDeadline()) {
  if(!running || running->type!=ESP_PARTITION_TYPE_APP ||
      running->subtype!=ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
      running->address!=0x10000 || running->size!=0x280000)return false;
  const esp_partition_t* metadata=lcd_nvs_known_layout();
  if(!metadata)return false;
  uint8_t bytes[256];static const uint8_t first_crc[4]={0x9a,0x98,0x43,0x47};
  for(size_t offset=0;offset<8192;offset+=sizeof(bytes)){
    if(!deadline.live() || esp_partition_read(metadata,offset,bytes,sizeof(bytes))!=ESP_OK)return false;
    for(size_t i=0;i<sizeof(bytes);++i){
      const size_t at=offset+i;uint8_t expected=0xff;
      if(at<4)expected=at==0?1:0;
      else if(at>=28 && at<32)expected=first_crc[at-28];
      else if(at>=4096 && at<4100)expected=0;
      if(bytes[i]!=expected)return false;
    }
  }
  return deadline.live();
}
#endif

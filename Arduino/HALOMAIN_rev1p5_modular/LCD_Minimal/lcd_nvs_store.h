#ifndef LCD_NVS_STORE_H
#define LCD_NVS_STORE_H
#include "lcd_nvs_capacity.h"
#include "lcd_nvs_records.h"

static constexpr const char* LCD_DURABLE_NAMESPACE = "lcd_durable";
static constexpr const char* LCD_COORD_KEY = "coord_v2";
static constexpr const char* LCD_CONTINUATION_KEY = "cont_v2";
static constexpr const char* LCD_PROGRESS_KEY = "progress_v2";

static LcdNvsRecords::ReadStatus lcd_nvs_failed_protected_read(LcdNvsRecords::ReadStatus status) {
  g_lcd_nvs_uncertain.store(true);return status;
}

template<class Record>
static LcdNvsRecords::ReadStatus lcd_nvs_read_record_unlocked(const char* key, Record& result) {
  using LcdNvsRecords::ReadStatus;
  if(g_lcd_nvs_uncertain)return ReadStatus::IoError;
  nvs_handle_t handle=0;
  esp_err_t err=nvs_open(LCD_DURABLE_NAMESPACE,NVS_READONLY,&handle);
  if(err==ESP_ERR_NVS_NOT_FOUND)return ReadStatus::Absent;
  if(err!=ESP_OK)return lcd_nvs_failed_protected_read(ReadStatus::IoError);
  size_t length=0;err=nvs_get_blob(handle,key,nullptr,&length);
  if(err==ESP_ERR_NVS_NOT_FOUND){nvs_close(handle);return ReadStatus::Absent;}
  if(err==ESP_ERR_NVS_TYPE_MISMATCH || (err==ESP_OK && length!=sizeof(Record))){
    nvs_close(handle);return lcd_nvs_failed_protected_read(ReadStatus::Invalid);
  }
  if(err!=ESP_OK){nvs_close(handle);return lcd_nvs_failed_protected_read(ReadStatus::IoError);}
  Record loaded={};length=sizeof(loaded);
  err=nvs_get_blob(handle,key,&loaded,&length);nvs_close(handle);
  if(err!=ESP_OK)return lcd_nvs_failed_protected_read(ReadStatus::IoError);
  if(length!=sizeof(loaded) || !LcdNvsRecords::valid(loaded))return lcd_nvs_failed_protected_read(ReadStatus::Invalid);
  result=loaded;return ReadStatus::Present;
}

template<class Record>
static LcdNvsRecords::ReadStatus lcd_nvs_read_record(const char* key, Record& result) {
  LcdNvsLease lease;
  if(!lease)return LcdNvsRecords::ReadStatus::Busy;
  return lcd_nvs_read_record_unlocked(key,result);
}

template<class Record>
static bool lcd_nvs_commit_record_unlocked(const char* key,const Record& candidate,
    const LcdNvsDeadline& deadline=LcdNvsDeadline()) {
  using LcdNvsRecords::ReadStatus;
  if(!deadline.live() || g_lcd_nvs_uncertain || !LcdNvsRecords::valid(candidate) ||
      !lcd_nvs_prepare_essential(lcd_nvs_blob_entries(sizeof(candidate))+1,deadline))return false;
  Record prior={};
  const ReadStatus status=lcd_nvs_read_record_unlocked(key,prior);
  if(!deadline.live() || (status!=ReadStatus::Absent && status!=ReadStatus::Present))return false;
  nvs_handle_t handle=0;
  if(nvs_open(LCD_DURABLE_NAMESPACE,NVS_READWRITE,&handle)!=ESP_OK)return false;
  // Recheck the metadata barrier after any potentially synchronous preparation.
  if(!deadline.live() || !lcd_nvs_image_valid() || !deadline.live()){nvs_close(handle);return false;}
  const esp_err_t set=nvs_set_blob(handle,key,&candidate,sizeof(candidate));
  const esp_err_t commit=set==ESP_OK && deadline.live()?nvs_commit(handle):ESP_FAIL;
  nvs_close(handle);
  if(set!=ESP_OK || commit!=ESP_OK){
    // Even same-handle readback cannot promote a failed persistence operation.
    // No subsequent helper reload in this boot may reinterpret uncertainty.
    g_lcd_nvs_uncertain.store(true);return false;
  }
  Record observed={};
  if(lcd_nvs_read_record_unlocked(key,observed)!=ReadStatus::Present ||
      memcmp(&candidate,&observed,sizeof(candidate))!=0){
    g_lcd_nvs_uncertain.store(true);return false;
  }
  return true;
}

template<class Record>
static bool lcd_nvs_commit_record(const char* key,const Record& candidate,
    const LcdNvsDeadline& deadline=LcdNvsDeadline()) {
  LcdNvsLease lease;
  return bool(lease) && lcd_nvs_commit_record_unlocked(key,candidate,deadline);
}
#endif

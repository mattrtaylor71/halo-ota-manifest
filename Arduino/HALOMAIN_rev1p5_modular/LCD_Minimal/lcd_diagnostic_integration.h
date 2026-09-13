#pragma once
// Called by the existing LCD UART/main tasks. No display, wake or OTA decision
// depends on this optional journal. The operation owner protects the RAM-only
// Journal/scratch lifetime; every storage call uses the original NVS lease.
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
#include "lcd_diagnostics.h"
#include "../halo_ota_demo/firmware/shared/DiagnosticNvs.h"
#include "../halo_ota_demo/firmware/shared/BuildInfo.h"
#include <esp_app_desc.h>
#include <mbedtls/sha1.h>
#ifndef HALO_DIAG_RETIRE_SNAPSHOT_20260908
#define HALO_DIAG_RETIRE_SNAPSHOT_20260908 0
#endif
static std::atomic_flag g_lcd_diag_operation=ATOMIC_FLAG_INIT;
static uint32_t g_lcd_diag_started=0,g_lcd_diag_budget=0;
static bool g_lcd_diag_uncertain=false,g_lcd_diag_qualified=false,g_lcd_diag_ready=false;
static bool g_lcd_diag_mac_ready=false,g_lcd_diag_boot_seen=false,g_lcd_diag_sleep_owner=false;
static uint8_t g_lcd_diag_scratch[halo_diag::MAX_RECORD],g_lcd_diag_mac[6];
static uint64_t g_lcd_diag_timer_us=0,g_lcd_diag_timer_epoch=0;
static int32_t g_lcd_diag_timer_sdk=0;
static bool g_lcd_diag_timer_seen=false;
struct LcdDiagnosticScope {
  LcdMaintenanceStorageTryGuard storage;
  bool active=false;
  explicit LcdDiagnosticScope(uint32_t budget=1500,bool sleep_owner=false){
    if(!budget||xPortInIsrContext()||g_lcd_diag_operation.test_and_set(std::memory_order_acquire))return;
    if(!storage.acquire()){g_lcd_diag_operation.clear(std::memory_order_release);return;}
    g_lcd_diag_started=millis();g_lcd_diag_budget=budget<1500?budget:1500;
    g_lcd_diag_sleep_owner=sleep_owner;active=true;
  }
  ~LcdDiagnosticScope(){if(active){g_lcd_diag_budget=0;g_lcd_diag_sleep_owner=false;g_lcd_diag_operation.clear(std::memory_order_release);}}
  LcdDiagnosticScope(const LcdDiagnosticScope&)=delete;
  LcdDiagnosticScope& operator=(const LcdDiagnosticScope&)=delete;
};
static bool lcd_diag_safe(void*){
  return g_lcd_diag_budget&&(uint32_t)(millis()-g_lcd_diag_started)<g_lcd_diag_budget&&
    !g_lcd_diag_uncertain&&!g_lcd_nvs_uncertain&&!g_lcd_ota_uart_receiving&&
    (!g_lcd_sleep_commit_gate||g_lcd_diag_sleep_owner)&&!provisioning_input_locked()&&
    !waiting_for_scan_response&&!waiting_for_voice_response&&!waiting_for_list_response&&
    !lcd_refresh_inflight&&refresh_state==REFRESH_IDLE&&!ui_busy&&!dish_processing_active&&
    !ship_hold_capture_phase&&!g_img_rx_active&&!g_spool_tx_active&&!g_spool_tx_pending&&!xPortInIsrContext();
}
static halo_diag::NvsGuard g_lcd_diag_guard{
  nullptr,[](void*){return !g_lcd_nvs_writer.test_and_set(std::memory_order_acquire);},
  [](void*){g_lcd_nvs_writer.clear(std::memory_order_release);},lcd_diag_safe,
  [](void*){return true;},[](void*){return lcd_nvs_image_valid();},[](void*){return false;},
  [](void*,bool retirement){g_lcd_diag_uncertain=true;if(retirement)g_lcd_nvs_uncertain.store(true);}
};
static halo_diag::NvsAdapter g_lcd_diag_adapter(halo_diag::Board::Lcd,g_lcd_diag_guard,true);
static halo_diag::Store g_lcd_diag_store=g_lcd_diag_adapter.store();
static LcdDiagnostics& lcd_diag_module(){
  // The sole operation owner must read the actual MAC before first construction.
  static LcdDiagnostics module(g_lcd_diag_store,g_lcd_diag_scratch,g_lcd_diag_mac);return module;
}
static bool lcd_diag_open(){
  if(!lcd_diag_safe(nullptr)||!g_lcd_coord_boot_id)return false;
  if(!g_lcd_diag_mac_ready){if(esp_read_mac(g_lcd_diag_mac,ESP_MAC_ETH)!=ESP_OK)return false;g_lcd_diag_mac_ready=true;}
  if(!g_lcd_diag_qualified){g_lcd_diag_qualified=(HALO_DIAG_RETIRE_SNAPSHOT_20260908==1
      ?g_lcd_diag_adapter.qualify_and_retire(true,reinterpret_cast<uint8_t*>(g_lcd_nvs_diagnostic_scratch),sizeof(g_lcd_nvs_diagnostic_scratch),false)
      :g_lcd_diag_adapter.qualify_without_retirement());if(!g_lcd_diag_qualified){
    static uint8_t reported=255;const uint8_t why=uint8_t(g_lcd_diag_adapter.last_qualification());
    if(reported!=why){reported=why;Serial.printf("[OTA_DIAG] qualification=%u active=0 uncertain=%u\n",why,g_lcd_diag_uncertain?1:0);}
    return false;}}
  if(g_lcd_diag_ready)return true;
  const auto r=lcd_diag_module().open();g_lcd_diag_ready=r==halo_diag::Result::Ok;
  return g_lcd_diag_ready||r==halo_diag::Result::Empty||r==halo_diag::Result::Incomplete;
}
static void lcd_diag_state(halo_diag::Stage stage,bool orphan=false){
  if((!orphan&&(!g_lcd_diag_ready||!lcd_diag_module().capture_enabled()))||!lcd_diag_safe(nullptr))return;
  halo_diag::State s{};s.boot_id=g_lcd_coord_boot_id;s.epoch=lcd_time_valid()?uint64_t(time(nullptr)):0;s.uptime_ms=millis();s.stage=stage;
  if(!halo_diag::text_ok(kFirmwareVersion,sizeof(s.fw))||!halo_diag::text_ok(kBuildId,sizeof(s.build)))return;
  memcpy(s.fw,kFirmwareVersion,strlen(kFirmwareVersion)+1);memcpy(s.build,kBuildId,strlen(kBuildId)+1);
  const esp_app_desc_t*d=esp_app_get_description();if(d){memcpy(s.image_hash,d->app_elf_sha256,32);s.hash_kind=halo_diag::HashKind::CompiledElf;}
  const esp_partition_t*r=esp_ota_get_running_partition(),*b=esp_ota_get_boot_partition();esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
  s.running_offset=r?r->address:0;s.boot_offset=b?b->address:0;s.partition_size=r?r->size:0;
  s.image_state_sdk=r?esp_ota_get_state_partition(r,&state):ESP_ERR_NOT_FOUND;s.image_state=uint8_t(state);
  s.wake=uint8_t(esp_sleep_get_wakeup_cause());s.reset=uint8_t(esp_reset_reason());
  // A plausible carried LCD clock is not a new SNTP-fresh observation.
  s.time_quality=lcd_time_valid()?halo_diag::TimeQuality::Retained:halo_diag::TimeQuality::Unknown;
  s.internal_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  s.internal_largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  s.internal_min=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  s.timer_us=g_lcd_diag_timer_us;s.selected_epoch=g_lcd_diag_timer_epoch;s.timer_sdk=g_lcd_diag_timer_sdk;s.timer_flags=g_lcd_diag_timer_seen?1:0;
  if(orphan){s.flags=halo_diag::STATE_ORPHAN_CLOSURE;if(lcd_diag_module().close_orphan(s)==halo_diag::Result::Ok)g_lcd_diag_ready=true;}
  else lcd_diag_module().state(s);
}
static void lcd_diag_boot(){
  LcdDiagnosticScope scope;if(!scope.active||g_lcd_diag_boot_seen||!lcd_nvs_image_valid()||!lcd_diag_open())return;
  g_lcd_diag_boot_seen=true;if(lcd_diag_module().orphan_closing())lcd_diag_state(halo_diag::Stage::JournalBefore,true);else if(g_lcd_diag_ready)lcd_diag_state(halo_diag::Stage::Boot);
}
static void lcd_diag_flush_failure(uint32_t budget,bool sleep_owner);
static bool lcd_diag_has_pending_failure();
static halo_diag::Result lcd_diag_context(const halo_diag::Context&ctx){
  lcd_diag_flush_failure(1500,false);
  LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open())return halo_diag::Result::Busy;
  if(lcd_diag_has_pending_failure()){lcd_diag_module().close_capture();return halo_diag::Result::Busy;}
  const auto r=lcd_diag_module().accept_context(ctx);
  if(r!=halo_diag::Result::Ok&&r!=halo_diag::Result::Already)lcd_diag_state(halo_diag::Stage::JournalBefore,true);
  // Conflict closes capture but keeps old read/export independently available.
  if(r==halo_diag::Result::Ok||r==halo_diag::Result::Already)g_lcd_diag_ready=true;
  if(lcd_diag_module().capture_enabled())lcd_diag_state(halo_diag::Stage::Admission);return r;
}
static halo_diag::Result lcd_diag_read(uint8_t slot,uint8_t*out,size_t cap,size_t&n){
  n=0;LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open())return halo_diag::Result::Busy;
  return lcd_diag_module().read_record(slot,out,cap,n);
}
static halo_diag::Result lcd_diag_next(uint8_t*out,size_t cap,size_t&n,uint8_t&slot){
  n=0;LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open())return halo_diag::Result::Busy;
  return lcd_diag_module().next_record(out,cap,n,slot);
}
static halo_diag::Result lcd_diag_ack(uint8_t slot,int status,const char*receipt,size_t n){
  LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open())return halo_diag::Result::Busy;
  return lcd_diag_module().cloud_ack(slot,status,receipt,n,
    [](void*,const uint8_t*p,size_t len,uint8_t digest[20]){return mbedtls_sha1(p,len,digest)==0;},nullptr);
}

// Typed current-time handoff/retirement operations share the same existing
// operation owner and NVS lease. An ordinary UART ACK is never sufficient.
static halo_diag::Result lcd_diag_seal(const uint8_t journal[16],uint32_t context_crc){
  LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open()||lcd_diag_has_pending_failure()||!journal)return halo_diag::Result::Busy;
  uint8_t raw[256];size_t n=0;const auto r=lcd_diag_module().read_record(0,raw,sizeof(raw),n);if(r!=halo_diag::Result::Ok)return r;
  if(memcmp(raw+halo_diag::HEADER,journal,16)||halo_diag::record_crc(raw,0)!=context_crc)return halo_diag::Result::Invalid;
  lcd_diag_state(halo_diag::Stage::JournalBefore,true);if(!g_lcd_diag_ready)return halo_diag::Result::Busy;return lcd_diag_module().seal();
}
static halo_diag::Result lcd_diag_handoff(uint32_t epoch,uint8_t*out,size_t cap,size_t&n){
  n=0;LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open()||lcd_diag_has_pending_failure()||cap<halo_diag::HANDOFF_BYTES)return halo_diag::Result::Busy;
  if(lcd_diag_module().orphan_closing())lcd_diag_state(halo_diag::Stage::JournalBefore,true);if(!g_lcd_diag_ready)return halo_diag::Result::Busy;
  halo_diag::Handoff h;auto r=lcd_diag_module().read_handoff(h);
  if(r==halo_diag::Result::Busy){const auto prepared=lcd_diag_module().prepare_handoff(epoch);if(prepared!=halo_diag::Result::Ok&&prepared!=halo_diag::Result::Already)return prepared;r=lcd_diag_module().read_handoff(h);}
  if(r==halo_diag::Result::Ok){halo_diag::encode_handoff(out,h);n=halo_diag::HANDOFF_BYTES;}return r;
}
static halo_diag::Result lcd_diag_handoff_ack(int status,const char*receipt,size_t n){
  LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open()||!g_lcd_diag_ready)return halo_diag::Result::Busy;
  return lcd_diag_module().cloud_handoff_ack(status,receipt,n,[](void*,const uint8_t*p,size_t len,uint8_t digest[20]){return mbedtls_sha1(p,len,digest)==0;},nullptr);
}
static halo_diag::Result lcd_diag_retire(){
  LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open()||!g_lcd_diag_ready||lcd_diag_has_pending_failure())return halo_diag::Result::Busy;
  const auto r=lcd_diag_module().retire_if_ready();if(r==halo_diag::Result::Empty)g_lcd_diag_ready=false;return r;
}

// Discovery v1,88 bytes: journal16; fail0/fail1/current/last-ACKed-state
// (sequenceLE64,CRCLE32)x4; highestLE64; uncertainty/fullDrops/ioDrops/coalescedLE32.
// Derived from existing keys/checkpoint; no new persistent metadata.
static halo_diag::Result lcd_diag_discovery(uint8_t (&out)[256],size_t&n){
  n=0;LcdDiagnosticScope scope;if(!scope.active||!lcd_diag_open()||!g_lcd_diag_ready)return halo_diag::Result::Busy;
  uint8_t record[256];size_t bytes=0;halo_diag::Watermark w;
  if(lcd_diag_module().watermark(w)!=halo_diag::Result::Ok||lcd_diag_module().read_record(0,record,sizeof(record),bytes)!=halo_diag::Result::Ok)return halo_diag::Result::Uncertain;
  memset(out,0,88);memcpy(out,record+halo_diag::HEADER,16);
  for(uint8_t slot=1;slot<=3;++slot){auto r=lcd_diag_module().read_record(slot,record,sizeof(record),bytes);if(r==halo_diag::Result::Empty)continue;if(r!=halo_diag::Result::Ok)return r;halo_diag::put64(out+16+(slot-1)*12,halo_diag::sequence(record));halo_diag::put32(out+24+(slot-1)*12,halo_diag::record_crc(record,slot));}
  halo_diag::put64(out+52,w.state_ack);halo_diag::put32(out+60,w.state_ack_crc);halo_diag::put64(out+64,w.highest_committed);halo_diag::put32(out+72,w.uncertainty?1:0);halo_diag::put32(out+76,w.dropped_full);halo_diag::put32(out+80,w.dropped_io);halo_diag::put32(out+84,w.coalesced);n=88;return halo_diag::Result::Ok;
}

static void lcd_diag_flush_failure(uint32_t budget=1500,bool sleep_owner=false);
static void lcd_diag_timer_selected(uint64_t timer_us,int32_t sdk){
  Serial.printf("[OTA_DIAG] timer_sdk boot=%lu us=%llu sdk=%ld\n",(unsigned long)g_lcd_coord_boot_id,(unsigned long long)timer_us,(long)sdk);
  lcd_diag_flush_failure(1500,true);
  LcdDiagnosticScope scope(1500,true);if(!scope.active)return;
  g_lcd_diag_timer_us=timer_us;g_lcd_diag_timer_sdk=sdk;g_lcd_diag_timer_seen=true;
  g_lcd_diag_timer_epoch=lcd_time_valid()?uint64_t(time(nullptr))+timer_us/1000000ULL:0;
  if(lcd_diag_open())lcd_diag_state(halo_diag::Stage::PreSleep);
}
// One allocation-free failure sample is published by the existing UART task.
// Storage happens only after the OTA receiving/flash owner has released it.
struct LcdDiagnosticFailureSample {
  uint64_t epoch=0;uint32_t uptime=0,accepted=0,expected=0,free=0,largest=0,minimum=0;
  uint32_t running=0,selected=0,image_state=0,transport_flags=0;int32_t sdk=0;
  halo_diag::Stage stage=halo_diag::Stage::Failure;uint8_t reason=0,time_quality=0;
};
static LcdDiagnosticFailureSample g_lcd_diag_failure;
static std::atomic<bool> g_lcd_diag_failure_pending{false};
static bool lcd_diag_has_pending_failure(){return g_lcd_diag_failure_pending.load(std::memory_order_acquire);}
static bool g_lcd_diag_attempt_open=false;
static uint8_t g_lcd_diag_attempt_id[16],g_lcd_diag_attempt=0;
static void lcd_diag_flush_failure(uint32_t budget,bool sleep_owner){
  if(!g_lcd_diag_failure_pending.load(std::memory_order_acquire))return;
  LcdDiagnosticScope scope(budget,sleep_owner);if(!scope.active||!lcd_diag_safe(nullptr)||!lcd_diag_open()||!g_lcd_diag_ready)return;
  const auto&v=g_lcd_diag_failure;halo_diag::Failure f{};
  memcpy(f.attempt_id,g_lcd_diag_attempt_id,16);f.attempt_ordinal=g_lcd_diag_attempt;
  f.boot_id=g_lcd_coord_boot_id;f.epoch=v.epoch;f.uptime_ms=v.uptime;f.stage=v.stage;f.sdk=v.sdk;
  f.accepted=v.accepted;f.expected=v.expected;f.internal_free=v.free;f.internal_largest=v.largest;f.internal_min=v.minimum;
  f.running_offset=v.running;f.boot_offset=v.selected;f.image_state=v.image_state;
  // bit1=LCD failure detail, bits8..15=reason, bits24..25=capture time quality.
  // Optional transport flags occupy only bit2 and bits16..23/26..31. The
  // existing 256-byte record, SDK fields and compiled-image hash stay intact.
  f.flags=2u|(uint32_t(v.reason)<<8)|(uint32_t(v.time_quality)<<24)|v.transport_flags;
  if(!halo_diag::text_ok(kFirmwareVersion,sizeof(f.fw))||!halo_diag::text_ok(kBuildId,sizeof(f.build)))return;
  memcpy(f.fw,kFirmwareVersion,strlen(kFirmwareVersion)+1);memcpy(f.build,kBuildId,strlen(kBuildId)+1);
  const esp_app_desc_t*d=esp_app_get_description();if(d){memcpy(f.image_hash,d->app_elf_sha256,32);f.hash_kind=halo_diag::HashKind::CompiledElf;}
  const auto result=lcd_diag_module().flush_retained_failure(f);
  if(result==halo_diag::Result::Ok||result==halo_diag::Result::Already){
    g_lcd_diag_failure_pending.store(false,std::memory_order_release);lcd_diag_state(halo_diag::Stage::Cleanup);
  }
}
static void lcd_diag_prepare_attempt(const char*version,const char*sha,uint32_t expected,uint32_t budget){
  lcd_diag_flush_failure(budget);
  // Never replace a sample that still awaits durable storage.
  if(g_lcd_diag_failure_pending.load(std::memory_order_acquire))return;
  g_lcd_diag_attempt_open=false;
  LcdDiagnosticScope scope(budget);if(!scope.active||!lcd_diag_open()||!g_lcd_diag_ready||!lcd_diag_module().capture_enabled()||!version||!sha||strlen(sha)!=64)return;
  uint8_t raw[256],hash[32];size_t n=0;
  for(unsigned i=0;i<32;++i){unsigned a=0,b=0;char x=sha[i*2],y=sha[i*2+1];
    if(x>='0'&&x<='9')a=x-'0';else if(x>='a'&&x<='f')a=x-'a'+10;else return;
    if(y>='0'&&y<='9')b=y-'0';else if(y>='a'&&y<='f')b=y-'a'+10;else return;hash[i]=uint8_t(a*16+b);}
  if(lcd_diag_module().read_record(0,raw,sizeof(raw),n)!=halo_diag::Result::Ok||!halo_diag::valid_context(raw)||
      strcmp((const char*)raw+halo_diag::HEADER+64,version)||memcmp(raw+halo_diag::HEADER+32,hash,32)||
      halo_diag::get32(raw+halo_diag::HEADER+168)!=expected)return;
  for(uint8_t slot=1;slot<=2;++slot){const auto r=lcd_diag_module().read_record(slot,raw,sizeof(raw),n);
    if(r==halo_diag::Result::Empty){g_lcd_diag_attempt=slot-1;esp_fill_random(g_lcd_diag_attempt_id,16);g_lcd_diag_attempt_open=true;break;}
    if(r!=halo_diag::Result::Ok)return;}
  lcd_diag_state(halo_diag::Stage::ProxyBegin);
}
static void lcd_diag_capture_failure(halo_diag::Stage stage,uint8_t reason,int32_t sdk,uint32_t accepted,uint32_t expected,uint32_t transport_flags=0){
  if(!g_lcd_diag_attempt_open||!lcd_diag_module().capture_enabled()||g_lcd_diag_failure_pending.load(std::memory_order_acquire))return;
  g_lcd_diag_attempt_open=false;auto&v=g_lcd_diag_failure;v={};
  v.free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  v.largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  v.minimum=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  v.epoch=lcd_time_valid()?uint64_t(time(nullptr)):0;v.uptime=millis();v.time_quality=lcd_time_valid()?1:0;
  v.stage=stage;v.reason=reason;v.sdk=sdk;v.accepted=accepted;v.expected=expected;v.transport_flags=transport_flags;
  const esp_partition_t*r=esp_ota_get_running_partition(),*b=esp_ota_get_boot_partition();esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
  v.running=r?r->address:0;v.selected=b?b->address:0;if(r)esp_ota_get_state_partition(r,&state);v.image_state=uint32_t(state);
  g_lcd_diag_failure_pending.store(true,std::memory_order_release);
}
#endif

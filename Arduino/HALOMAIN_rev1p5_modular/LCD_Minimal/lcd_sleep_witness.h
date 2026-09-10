#pragma once
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS && HALO_LCD_SLEEP_WITNESS
#include "../halo_ota_demo/firmware/shared/LcdSleepWitness.h"
#include "../halo_ota_demo/firmware/shared/MaintenanceWitnessDigest.h"
#include <mbedtls/sha256.h>

static_assert(ESP_RST_DEEPSLEEP==halo_lcd_sleep_witness::kDeepSleepReset,"reset enum");
static_assert(ESP_SLEEP_WAKEUP_TIMER==halo_lcd_sleep_witness::kTimerWake,"wake enum");
// RTC contains only a one-use observation. It cannot restore or authorize an arm.
RTC_DATA_ATTR static uint8_t g_lcd_sleep_witness_rtc[halo_lcd_sleep_witness::kBytes] = {};
static uint8_t g_lcd_sleep_witness_observed[halo_lcd_sleep_witness::kBytes] = {};
static bool g_lcd_sleep_witness_observed_ok = false;
static LcdMaintenanceArm g_lcd_sleep_witness_arm = {};
static uint8_t g_lcd_sleep_witness_digest[32] = {};
static uint32_t g_lcd_sleep_witness_sense_boot = 0;
static bool g_lcd_sleep_witness_cache_ok = false;
static halo_lcd_sleep_witness::Snapshot g_lcd_sleep_witness_selected = {};
static bool g_lcd_sleep_witness_prepared = false;
static bool g_lcd_sleep_witness_captured = false;
static std::atomic<uint32_t> g_lcd_sleep_witness_cancel_generation{0};
static uint32_t g_lcd_sleep_witness_selected_generation = 0;

static bool lcd_sleep_witness_hash(const uint8_t* raw,size_t bytes,uint8_t* out) {
  return mbedtls_sha256(raw,bytes,out,0)==0;
}
// Caller owns the existing maintenance storage/sleep lock. The UI cancellation
// path only changes an atomic generation, never concurrent RTC bytes.
static void lcd_sleep_witness_cancel() {
  g_lcd_sleep_witness_cancel_generation.fetch_add(1,std::memory_order_relaxed);
}
static void lcd_sleep_witness_forget_future() {
  lcd_sleep_witness_cancel();
  g_lcd_sleep_witness_cache_ok=false;
  g_lcd_sleep_witness_prepared=false;
  g_lcd_sleep_witness_captured=false;
  halo_lcd_sleep_witness::invalidate(g_lcd_sleep_witness_rtc);
}
static void lcd_sleep_witness_arm_committed(const LcdMaintenanceArm& arm) {
  if(!g_lcd_sleep_witness_cache_ok || memcmp(&arm,&g_lcd_sleep_witness_arm,sizeof(arm)))
    lcd_sleep_witness_forget_future();
}
static void lcd_sleep_witness_accept(const LcdMaintenanceArm& arm,uint32_t sense_boot) {
  lcd_sleep_witness_forget_future();
  if(!sense_boot || !g_lcd_coord_boot_id || !lcd_arm_valid(arm) || !arm.armed)return;
  halo_maintenance_witness::Window w{};
  w.prior_lcd=g_lcd_coord_boot_id;w.prior_sense=sense_boot;w.armed=arm.armed;
  w.wake_s=arm.wake_s;w.remaining_s=arm.remaining_s;w.start_epoch=arm.start_epoch;
  w.duration_s=arm.duration_s;w.grace_before_s=arm.grace_before_s;w.grace_after_s=arm.grace_after_s;
  memcpy(w.request,arm.request_id,sizeof(w.request));
  if(!halo_maintenance_witness::digest(w,lcd_sleep_witness_hash,g_lcd_sleep_witness_digest))return;
  g_lcd_sleep_witness_arm=arm;g_lcd_sleep_witness_sense_boot=sense_boot;
  g_lcd_sleep_witness_cache_ok=true;
}
static bool lcd_sleep_witness_cache_current() {
  const LcdMaintenanceArm actual=lcd_arm_from_ram();
  return g_lcd_sleep_witness_cache_ok && !g_lcd_arm_storage_fault &&
    !memcmp(&actual,&g_lcd_sleep_witness_arm,sizeof(actual));
}
static void lcd_sleep_witness_prepare(const char* reason,uint64_t selected_epoch,uint32_t timer_sec,uint32_t wake_lead_s) {
  g_lcd_sleep_witness_prepared=false;g_lcd_sleep_witness_captured=false;
  halo_lcd_sleep_witness::invalidate(g_lcd_sleep_witness_rtc);
  if(!reason || strcmp(reason,"maintenance_abs") || !selected_epoch || selected_epoch>UINT32_MAX ||
     !timer_sec || !lcd_sleep_witness_cache_current())return;
  const uint64_t lead=uint64_t(g_lcd_sleep_witness_arm.grace_before_s)+wake_lead_s;
  const uint64_t due=g_lcd_sleep_witness_arm.start_epoch;
  if(due<=lead || due-lead<=selected_epoch || due-lead-selected_epoch!=timer_sec)return;
  auto& s=g_lcd_sleep_witness_selected;s={};
  s.prior_lcd_boot=g_lcd_coord_boot_id;s.prior_sense_boot=g_lcd_sleep_witness_sense_boot;
  s.due_epoch=uint32_t(due);s.selected_epoch=uint32_t(selected_epoch);
  s.timer_us=uint64_t(timer_sec)*1000000ULL;
  memcpy(s.request_sha256,g_lcd_sleep_witness_digest,32);
  g_lcd_sleep_witness_selected_generation=g_lcd_sleep_witness_cancel_generation.load(std::memory_order_relaxed);
  g_lcd_sleep_witness_prepared=true;
}
static void lcd_sleep_witness_configure_begin() {
  g_lcd_sleep_witness_captured=false;
  halo_lcd_sleep_witness::invalidate(g_lcd_sleep_witness_rtc);
}
static void lcd_sleep_witness_after_sdk(uint64_t timer_us,int32_t sdk_result) {
  const bool prepared=g_lcd_sleep_witness_prepared;
  g_lcd_sleep_witness_prepared=false; // one actual SDK callback, never reuse
  if(!prepared || timer_us!=g_lcd_sleep_witness_selected.timer_us || !lcd_sleep_witness_cache_current() ||
     g_lcd_sleep_witness_selected_generation!=g_lcd_sleep_witness_cancel_generation.load(std::memory_order_relaxed))return;
  g_lcd_sleep_witness_selected.sdk_result=sdk_result;
  g_lcd_sleep_witness_captured=halo_lcd_sleep_witness::capture_after_sdk(g_lcd_sleep_witness_rtc,g_lcd_sleep_witness_selected);
}
static void lcd_sleep_witness_configure_end() { g_lcd_sleep_witness_prepared=false; }
static void lcd_sleep_witness_enter() {
  if(!g_lcd_sleep_witness_captured || !lcd_sleep_witness_cache_current() ||
     g_lcd_sleep_witness_selected_generation!=g_lcd_sleep_witness_cancel_generation.load(std::memory_order_relaxed)) {
    halo_lcd_sleep_witness::invalidate(g_lcd_sleep_witness_rtc);return;
  }
  // The existing caller has completed all sleep cancellation/teardown checks.
  halo_lcd_sleep_witness::commit_before_sleep(g_lcd_sleep_witness_rtc);
}
static void lcd_sleep_witness_on_boot() {
  memcpy(g_lcd_sleep_witness_observed,g_lcd_sleep_witness_rtc,sizeof(g_lcd_sleep_witness_observed));
  const auto observed=halo_lcd_sleep_witness::consume_on_boot(g_lcd_sleep_witness_rtc,
      uint32_t(esp_reset_reason()),uint32_t(esp_sleep_get_wakeup_cause()),g_lcd_coord_boot_id);
  g_lcd_sleep_witness_observed_ok=observed.quality==halo_lcd_sleep_witness::Quality::TimerPredecessor;
  if(!g_lcd_sleep_witness_observed_ok)memset(g_lcd_sleep_witness_observed,0,sizeof(g_lcd_sleep_witness_observed));
  // Consumed RAM proof survives later maintenance-arm restoration/replacement.
  g_lcd_sleep_witness_cache_ok=false;g_lcd_sleep_witness_prepared=false;g_lcd_sleep_witness_captured=false;
}
static bool lcd_sleep_witness_read(uint8_t* out,size_t capacity,size_t& bytes) {
  bytes=0;
  if(!out || capacity<sizeof(g_lcd_sleep_witness_observed) || !g_lcd_sleep_witness_observed_ok)return false;
  memcpy(out,g_lcd_sleep_witness_observed,sizeof(g_lcd_sleep_witness_observed));
  bytes=sizeof(g_lcd_sleep_witness_observed);return true;
}
#endif

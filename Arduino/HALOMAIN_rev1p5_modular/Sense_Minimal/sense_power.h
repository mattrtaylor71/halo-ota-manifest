#pragma once
#include <esp_timer.h>
#include <time.h>
#include "../halo_ota_demo/firmware/shared/SystemPower.h"

// Volatile send-time metadata, never media identity or durable capture state.
// Reboot/deep sleep starts missing; an awake LCD supplies a new measurement.
static halo_power::Cache g_system_power_cache;
static portMUX_TYPE g_system_power_mux=portMUX_INITIALIZER_UNLOCKED;

static halo_power::View sense_power_view() {
  portENTER_CRITICAL(&g_system_power_mux);
  const halo_power::Cache cache=g_system_power_cache;
  portEXIT_CRITICAL(&g_system_power_mux);
  return cache.view(uint64_t(esp_timer_get_time())/1000);
}

extern "C" bool halo_system_power_json(char* out,size_t capacity) {
  return halo_power::json(sense_power_view(),out,capacity);
}
static bool sense_power_json(char* out,size_t capacity) {
  return halo_system_power_json(out,capacity);
}
static bool sense_power_append(JsonObject target) {
  // Populate the report's own document directly: no second JSON allocation or
  // serialize/parse roundtrip while the report/network resources overlap.
  const halo_power::View v=sense_power_view();
  const halo_power::Snapshot& s=v.sample;
  const bool measured=v.received && halo_power::valid(s);
  JsonObject p=target["system_power"].to<JsonObject>();
  if(p.isNull()) return false;
  bool ok=p["schema"].set(1);
  ok &= p["measurement"].set("lcd_system_supply");
  ok &= measured?p["system_supply_mv"].set(s.system_supply_mv):p["system_supply_mv"].set(nullptr);
  ok &= p["status"].set(halo_power::status_name(v.received?s.status:halo_power::Status::PeerMissing));
  ok &= p["valid"].set(measured);
  ok &= p["fresh"].set(measured && v.age_known && v.age_ms<=halo_power::kFreshMs);
  ok &= v.received&&v.age_known?p["age_ms"].set(v.age_ms):p["age_ms"].set(nullptr);
  ok &= p["age_known"].set(v.received&&v.age_known);
  ok &= v.received&&s.epoch_s>=1700000000ULL?p["sample_epoch_s"].set(s.epoch_s):p["sample_epoch_s"].set(nullptr);
  ok &= p["sample_uptime_ms"].set(s.uptime_ms);
  ok &= p["sample_boot_id"].set(s.boot_id);
  ok &= p["sample_sequence"].set(s.sequence);
  ok &= p["samples"].set(s.samples);
  ok &= p["raw_min"].set(s.raw_min);
  ok &= p["raw_max"].set(s.raw_max);
  ok &= p["power_source"].set("unknown");
  ok &= p["cell_mv"].set(nullptr);
  ok &= p["battery_percent"].set(nullptr);
  return ok;
}

static bool sense_power_accept_uart(JsonDocument& doc) {
  if(strcmp(doc["type"]|"","LCD_POWER")) return false;
  // Consume even malformed power frames: they must never reset user/link idle
  // clocks or request a retry. They are optional observational metadata.
  JsonObjectConst p=doc["p"].as<JsonObjectConst>();
  if(p.isNull() || !p["v"].is<unsigned>() || p["v"].as<unsigned>()!=1 ||
     !p["b"].is<uint32_t>() || !p["q"].is<uint32_t>() ||
     !p["u"].is<uint64_t>() || !p["e"].is<uint64_t>() ||
     !p["a"].is<uint64_t>() || !p["s"].is<uint8_t>() ||
     !p["mv"].is<uint16_t>() || !p["lo"].is<uint16_t>() ||
     !p["hi"].is<uint16_t>() || !p["n"].is<uint8_t>()) return true;
  halo_power::Snapshot s;
  s.boot_id=p["b"];s.sequence=p["q"];s.uptime_ms=p["u"];s.epoch_s=p["e"];
  s.status=halo_power::Status(p["s"].as<uint8_t>());
  s.system_supply_mv=p["mv"];s.raw_min=p["lo"];s.raw_max=p["hi"];s.samples=p["n"];
  uint64_t age=p["a"].as<uint64_t>();
  // A serial frame may have waited behind a blocking network operation. Its
  // sender age alone is only a lower bound, not receipt-time freshness. With
  // credible paired clocks, use an upper bound including second quantization;
  // otherwise retain the measurement but explicitly refuse to call it fresh.
  const time_t epoch_now=time(nullptr);
  const bool age_known=s.epoch_s>=1700000000ULL && epoch_now>=1700000000 &&
      uint64_t(epoch_now)>=s.epoch_s && uint64_t(epoch_now)-s.epoch_s<UINT64_MAX/1000-1;
  if(age_known) {
    const uint64_t upper_age=(uint64_t(epoch_now)-s.epoch_s+1)*1000;
    if(upper_age>age) age=upper_age;
  }
  const uint64_t now=uint64_t(esp_timer_get_time())/1000;
  portENTER_CRITICAL(&g_system_power_mux);
  const bool accepted=g_system_power_cache.accept(s,age,now,age_known);
  portEXIT_CRITICAL(&g_system_power_mux);
  if(accepted) Serial.printf("[POWER] rx boot=%lu seq=%lu status=%s system_supply_mv=%u age_ms=%llu\n",
    (unsigned long)s.boot_id,(unsigned long)s.sequence,halo_power::status_name(s.status),
    unsigned(s.system_supply_mv),(unsigned long long)age);
  return true;
}

// sense_wakelog.h — per-wake-cycle history that survives deep sleep.
//
// WHY: on a ship build the device is observable over USB only between boot and
// its first sleep — after a deep-sleep wake the USB CDC does not re-enumerate,
// and a tap wakes the device without bringing the port back. So an overnight
// soak on a real ship binary is blind: one window at the start, then nothing
// until morning, and the only evidence is whatever the backend happened to see.
//
// This records one compact entry per wake cycle into RTC memory, which survives
// deep sleep and software reset (including panics). One `wakelog` command in the
// morning then reports every cycle: what woke it, how long it stayed up, what it
// armed on the way out, and whether uploads landed.
//
// NOT gated behind a bench flag on purpose. It costs ~40 bytes per cycle of RTC
// memory and nothing at runtime, and it is exactly the data worth having from a
// device misbehaving in someone's kitchen — where there is no serial cable at
// all. Diagnostics that only exist on the bench cannot explain a field failure.
//
// RTC memory does NOT survive a power cut. That is the right trade here: a soak
// or a field fault is a sleep/reset story, and surviving those is what matters.

#pragma once

#include <Arduino.h>
#include "esp_sleep.h"
#include "sense_nvs_capacity.h"

#ifndef WAKELOG_SLOTS
#define WAKELOG_SLOTS 48          // ~2 days of nightly cycles, or a long soak
#endif

typedef struct {
  uint32_t epoch;            // wall clock at wake (0 if unknown)
  uint32_t awake_ms;         // how long this cycle stayed up
  uint32_t timer_armed_s;    // what we armed on the way back to sleep
  uint16_t uploads_ok;
  uint16_t uploads_fail;
  uint16_t captures;
  uint16_t spool_depth;      // images still on the LCD's SD card
  uint8_t  wake_cause;       // esp_sleep_get_wakeup_cause()
  uint8_t  reset_reason;
  uint8_t  ext0_armed;
  uint8_t  closed;           // 0 = cycle still open (device died before sleeping)
  uint8_t  drain_end;        // why a spool drain wake stopped (WakeDrainEnd)
} wakelog_entry_t;

// Why a drain wake ended. A drain wake is by definition unobserved -- attaching
// USB to watch it resets the Sense (reset_reason=11) and turns it into a cold
// boot -- so the only way to find out what one did is to have it write down the
// answer before it sleeps.
enum WakeDrainEnd : uint8_t {
  WAKE_DRAIN_NONE = 0,       // not a drain wake
  WAKE_DRAIN_EMPTY,          // card reported empty
  WAKE_DRAIN_BUDGET,         // spent its time budget
  WAKE_DRAIN_USER,           // a capture or foreground action took over
  WAKE_DRAIN_ACTIVE,         // still draining when the cycle closed
};
static uint8_t g_cycle_drain_end = WAKE_DRAIN_NONE;
static const char* wakelog_drain_end_str(uint8_t v) {
  switch (v) {
    case WAKE_DRAIN_EMPTY:  return "empty";
    case WAKE_DRAIN_BUDGET: return "budget";
    case WAKE_DRAIN_USER:   return "user";
    case WAKE_DRAIN_ACTIVE: return "active";
    default:                return "-";
  }
}

RTC_DATA_ATTR static wakelog_entry_t g_wakelog[WAKELOG_SLOTS];
RTC_DATA_ATTR static uint16_t g_wakelog_head = 0;    // next slot to write
RTC_DATA_ATTR static uint16_t g_wakelog_total = 0;   // cycles ever recorded
RTC_DATA_ATTR static uint32_t g_wakelog_magic = 0;
RTC_DATA_ATTR static uint64_t g_wakelog_present = 0;
RTC_DATA_ATTR static bool g_wakelog_persisted_base_known = false;
static_assert(WAKELOG_SLOTS==48,"reviewed persisted wake-ring geometry");
static_assert(sizeof(wakelog_entry_t)==28,"reviewed old wake-record length");
static uint16_t wakelog_retained_count() {
  uint16_t n=0;for(unsigned i=0;i<WAKELOG_SLOTS;++i)if(g_wakelog_present&(uint64_t(1)<<i))++n;return n;
}

#define WAKELOG_MAGIC 0x57414B32u   // 'WAK2': presence-aware RTC layout

// Live counters for the current cycle.
static uint16_t g_cycle_uploads_ok = 0;
static uint16_t g_cycle_uploads_fail = 0;
static uint16_t g_cycle_captures = 0;

static const char* wakelog_cause_name(uint8_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_EXT0:      return "tap";       // LCD pulled the wake line
    case ESP_SLEEP_WAKEUP_EXT1:      return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:     return "timer";     // the nightly/fallback wake
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "poweron";   // cold boot or reset
    default:                         return "other";
  }
}

// ── NVS mirror ────────────────────────────────────────────────────────
//
// RTC memory survives deep sleep and reset but NOT a power cut — and getting a
// sleeping board back on USB may require exactly that power cut, which would
// destroy the log in the act of reading it. An overnight soak is too expensive
// to lose that way, so each closed cycle is also written to NVS.
//
// Cost is one small blob per wake. At the real nightly cadence that is ~365
// writes a year, which is nothing for flash endurance.
#define WAKELOG_NVS_NS "wakelog"

static void wakelog_nvs_store(const wakelog_entry_t* e, uint16_t slot) {
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  (void)e;(void)slot;return; // RTC wake history remains live
#else
  if(!g_wakelog_persisted_base_known)return; // ambiguous restore cannot reset persisted counters
  NvsOptionalWrite admission(6);if(!admission)return;
  Preferences p;
  if (!p.begin(WAKELOG_NVS_NS, false)) return;
  char key[8];snprintf(key,sizeof(key),"e%u",(unsigned)(slot%WAKELOG_SLOTS));
  wakelog_entry_t actual{};
  bool ok=p.putBytes(key,e,sizeof(*e))==sizeof(*e) &&
      p.getBytesLength(key)==sizeof(actual) && p.getBytes(key,&actual,sizeof(actual))==sizeof(actual) &&
      !memcmp(e,&actual,sizeof(actual));
  if(ok)ok=p.putUShort("head",(uint16_t)((slot+1)%WAKELOG_SLOTS))==sizeof(uint16_t);
  if(ok)ok=p.putUShort("total",g_wakelog_total)==sizeof(uint16_t);
  p.end();
  if(!ok)Serial.println("[WAKELOG] persistence incomplete; stored positions may be ambiguous");
#endif
}

// Caller holds NvsCapacityLease across this check and the essential write.
// This preserves eight STORED ring positions, not provably newest legacy
// records: the former implementation did not check blob/head/total writes.
static bool wakelog_nvs_prepare_essential(size_t new_entries, bool (*budget_open)()) {
  if(g_nvs_reclaim_uncertain || !budget_open() || !nvs_capacity_image_valid())return false;
  const uint32_t began=millis();
  size_t before=0;unsigned eligible=0,deleted=0,retained=0;
  const bool before_known=nvs_capacity_available(before);
  auto report=[&](const char*result,bool ok){
    size_t after=0;const bool after_known=nvs_capacity_available(after);
    Serial.printf("[OTA_NVS_CAP] result=%s before=%ld after=%ld eligible=%u deleted=%u protected=%u elapsed_ms=%lu\n",
        result,before_known?(long)before:-1L,after_known?(long)after:-1L,eligible,deleted,retained,(unsigned long)(millis()-began));
    return ok;
  };
  if(!before_known)return report("stats_unavailable",false);
  if(new_entries>NVS_ESSENTIAL_CUSHION_ENTRIES)return report("size_invalid",false);
  // Optional writes protect this cushion; essential writes are its consumers.
  // Do not require restoring the whole cushion before every admitted write.
  if(g_nvs_essential_space_prepared)return before>=new_entries;
  if(before>=NVS_ESSENTIAL_CUSHION_ENTRIES){g_nvs_essential_space_prepared=true;return true;}
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  return report("retired_optional_profile",false);
#else
  if(g_nvs_reclaim_attempted)return false;
  g_nvs_reclaim_attempted=true; // at most one bounded reclamation pass per boot
  auto open=[&](){return (uint32_t)(millis()-began)<2000 && budget_open() && nvs_capacity_image_valid();};
  nvs_handle_t handle;
  if(!open() || nvs_open(WAKELOG_NVS_NS,NVS_READONLY,&handle)!=ESP_OK)return report("readonly_open",false);
  auto fail=[&](){nvs_close(handle);return report(g_nvs_reclaim_uncertain?"uncertain":"validation_or_deadline",false);};
  uint16_t head=0,total=0;
  if(nvs_get_u16(handle,"head",&head)!=ESP_OK || nvs_get_u16(handle,"total",&total)!=ESP_OK ||
      head>=WAKELOG_SLOTS || !total)return fail();
  uint64_t present=0,keep=0;
  // Validate the complete known-key candidate set before removing anything.
  // Unknown keys/namespaces are never enumerated or deleted.
  for(unsigned slot=0;slot<WAKELOG_SLOTS;++slot){
    if(!open())return fail();
    char key[8];snprintf(key,sizeof(key),"e%u",slot);
    size_t size=0;esp_err_t result=nvs_get_blob(handle,key,nullptr,&size);
    if(result==ESP_ERR_NVS_NOT_FOUND)continue; // prior partial pruning is idempotent
    if(result!=ESP_OK || size!=sizeof(wakelog_entry_t))return fail();
    wakelog_entry_t record{};size_t actual=sizeof(record);
    if(nvs_get_blob(handle,key,&record,&actual)!=ESP_OK || actual!=sizeof(record))return fail();
    present|=uint64_t(1)<<slot;
  }
  for(unsigned offset=1;offset<=WAKELOG_SLOTS && retained<8;++offset){
    const uint64_t bit=uint64_t(1)<<((head+WAKELOG_SLOTS-offset)%WAKELOG_SLOTS);
    if(present&bit){keep|=bit;++retained;}
  }
  for(unsigned slot=0;slot<WAKELOG_SLOTS;++slot)if((present&(uint64_t(1)<<slot)) && !(keep&(uint64_t(1)<<slot)))++eligible;
  nvs_close(handle);
  if(!open() || nvs_open(WAKELOG_NVS_NS,NVS_READWRITE,&handle)!=ESP_OK)return report("write_open",false);
  uint16_t check_head=0,check_total=0;
  if(nvs_get_u16(handle,"head",&check_head)!=ESP_OK || nvs_get_u16(handle,"total",&check_total)!=ESP_OK ||
      check_head!=head || check_total!=total)return fail();
  // Oldest stored positions first; retain up to eight actual typed records.
  for(unsigned offset=0;offset<WAKELOG_SLOTS;++offset){
    unsigned slot=(head+offset)%WAKELOG_SLOTS;uint64_t bit=uint64_t(1)<<slot;
    if(!(present&bit) || (keep&bit))continue;
    if(!open())return fail();
    size_t available=0;if(!nvs_capacity_available(available))return fail();
    if(available>=NVS_ESSENTIAL_CUSHION_ENTRIES){nvs_close(handle);g_nvs_essential_space_prepared=true;return report("reclaimed",true);}
    char key[8];snprintf(key,sizeof(key),"e%u",slot);
    if(nvs_erase_key(handle,key)!=ESP_OK || nvs_commit(handle)!=ESP_OK){
      g_nvs_reclaim_uncertain=true;return fail();
    }
    size_t size=0;
    if(nvs_get_blob(handle,key,nullptr,&size)!=ESP_ERR_NVS_NOT_FOUND){
      g_nvs_reclaim_uncertain=true;return fail();
    }
    ++deleted;
    // RTC still has its existing full records; only persisted retention shrank.
  }
  nvs_close(handle);
  size_t after=0;
  const bool ok=open() && nvs_capacity_available(after) && after>=NVS_ESSENTIAL_CUSHION_ENTRIES;
  if(ok)g_nvs_essential_space_prepared=true;
  return report(ok?"reclaimed":"insufficient_or_deadline",ok);
#endif
}

// Reload RTC state from NVS after a power cut wiped it.
static void wakelog_nvs_restore() {
  nvs_handle_t handle;
  g_wakelog_persisted_base_known=false;
  const esp_err_t opened=nvs_open(WAKELOG_NVS_NS,NVS_READONLY,&handle);
  if(opened==ESP_ERR_NVS_NOT_FOUND){g_wakelog_persisted_base_known=true;g_wakelog_magic=0;return;}
  if(opened!=ESP_OK)return;
  uint16_t head=0,total=0;uint64_t present=0;
  wakelog_entry_t records[WAKELOG_SLOTS]{};
  const esp_err_t head_result=nvs_get_u16(handle,"head",&head),total_result=nvs_get_u16(handle,"total",&total);
  const bool no_headers=head_result==ESP_ERR_NVS_NOT_FOUND && total_result==ESP_ERR_NVS_NOT_FOUND;
  bool ok=no_headers || (head_result==ESP_OK && total_result==ESP_OK && head<WAKELOG_SLOTS && total>0);
  for(unsigned i=0;ok && i<WAKELOG_SLOTS;++i){
    char key[8];snprintf(key,sizeof(key),"e%u",i);size_t size=0;
    esp_err_t result=nvs_get_blob(handle,key,nullptr,&size);
    if(result==ESP_ERR_NVS_NOT_FOUND)continue;
    if(result!=ESP_OK || size!=sizeof(records[i])){ok=false;break;}
    size_t actual=sizeof(records[i]);
    ok=nvs_get_blob(handle,key,&records[i],&actual)==ESP_OK && actual==sizeof(records[i]);
    if(ok)present|=uint64_t(1)<<i;
  }
  nvs_close(handle);if(!ok || (no_headers && present))return;
  g_wakelog_persisted_base_known=true;
  if(no_headers){g_wakelog_magic=0;return;} // definitively empty, discard stale RAM base
  memcpy(g_wakelog,records,sizeof(records));g_wakelog_present=present;
  g_wakelog_total=total;g_wakelog_head=head;g_wakelog_magic=WAKELOG_MAGIC;
  Serial.printf("[WAKELOG] restored %u retained cycle(s) total=%u order=stored_positions\n",
                (unsigned)wakelog_retained_count(),(unsigned)total);
}

// Open a cycle. Call early in setup().
static void wakelog_begin_cycle(uint32_t epoch_now, uint8_t reset_reason) {
  if (g_wakelog_magic != WAKELOG_MAGIC || !g_wakelog_persisted_base_known) {   // retry ambiguous restore only on normal boot
    wakelog_nvs_restore();                 // may repopulate from flash
  }
  if (g_wakelog_magic != WAKELOG_MAGIC) {  // genuinely nothing to recover
    memset((void*)g_wakelog, 0, sizeof(g_wakelog));
    g_wakelog_head = 0;
    g_wakelog_present = 0;
    g_wakelog_total = 0;
    g_wakelog_magic = WAKELOG_MAGIC;
  }
  if(!g_wakelog_persisted_base_known)Serial.println("[WAKELOG] persisted counter unknown; RAM-only diagnostics this boot");
  wakelog_entry_t* e = &g_wakelog[g_wakelog_head];
  memset(e, 0, sizeof(*e));
  g_wakelog_present |= uint64_t(1)<<g_wakelog_head;
  e->epoch = epoch_now;
  e->wake_cause = (uint8_t)esp_sleep_get_wakeup_cause();
  e->reset_reason = reset_reason;
  e->closed = 0;
  g_cycle_uploads_ok = g_cycle_uploads_fail = g_cycle_captures = 0;
  g_cycle_drain_end = WAKE_DRAIN_NONE;
  Serial.printf("[WAKELOG] cycle %u opened cause=%s reset=%u epoch=%lu\n",
                (unsigned)g_wakelog_total, wakelog_cause_name(e->wake_cause),
                (unsigned)reset_reason, (unsigned long)epoch_now);
}

// Close a cycle. Call at sleep entry, AFTER the wake timer is chosen.
//
// An entry left open (closed=0) is itself a finding: it means the device never
// reached sleep — it panicked, browned out, or hung — and the next boot will
// show the gap.
static void wakelog_end_cycle(uint32_t timer_s, bool ext0, uint16_t spool_depth) {
  wakelog_entry_t* e = &g_wakelog[g_wakelog_head];
  e->awake_ms = millis();
  e->timer_armed_s = timer_s;
  e->ext0_armed = ext0 ? 1 : 0;
  e->uploads_ok = g_cycle_uploads_ok;
  e->uploads_fail = g_cycle_uploads_fail;
  e->captures = g_cycle_captures;
  e->spool_depth = spool_depth;
  e->drain_end = g_cycle_drain_end;
  e->closed = 1;
  const uint16_t closed_slot = g_wakelog_head;
  g_wakelog_head = (uint16_t)((g_wakelog_head + 1) % WAKELOG_SLOTS);
  if (g_wakelog_total < 0xFFFF) g_wakelog_total++;
  wakelog_nvs_store(e, closed_slot);   // survives a power cut, unlike RTC
  Serial.printf("[WAKELOG] cycle closed awake=%lums timer=%lus ext0=%d cap=%u up_ok=%u up_fail=%u spool=%u drain=%s\n",
                (unsigned long)e->awake_ms, (unsigned long)timer_s, ext0 ? 1 : 0,
                (unsigned)e->captures, (unsigned)e->uploads_ok,
                (unsigned)e->uploads_fail, (unsigned)spool_depth,
                wakelog_drain_end_str(e->drain_end));
}

// Relay the wake history to the LCD over UART.
//
// REQUIRED, not a convenience: opening the Sense's USB port RESETS the Sense,
// and that reset clears RTC memory — so reading the log over the Sense's own USB
// destroys the very thing being read. Observed exactly that: a confirmed 75s
// timer wake reported back as "cause=poweron reset=11 (ESP_RST_USB), 0 cycles".
//
// The LCD is mains-powered, stays awake and already has an open serial session,
// so relaying through it is the only way to see the history at all — and in the
// field it is the only way, since there is no cable on either board.
static void wakelog_report_to_lcd() {
  const bool magic_ok = (g_wakelog_magic == WAKELOG_MAGIC);
  const uint16_t n = magic_ok
                     ? wakelog_retained_count()
                     : 0;
  // ALWAYS send the summary, even when there is nothing to report. The earlier
  // version returned silently on an empty log, which hid the one fact being
  // investigated: whether RTC memory survived at all. A diagnostic that goes
  // quiet in exactly the failing case is worse than no diagnostic — three sleep
  // cycles produced no output and no explanation.
  {
    char det[96];
    snprintf(det, sizeof(det), "magic_ok=%d total=%u cause=%s reset=%u",
             magic_ok ? 1 : 0, (unsigned)g_wakelog_total,
             wakelog_cause_name((uint8_t)esp_sleep_get_wakeup_cause()),
             (unsigned)esp_reset_reason());
    uart_send_sense_diag("wakelog", "boot", "SUMMARY", (int32_t)n, det);
  }
  if (n == 0) return;
  // Newest few only: this shares the link with real traffic, and the interesting
  // entries are always the most recent ones.
  const uint16_t show = (n < 5) ? n : 5;
  uint16_t skip=n-show;
  for (uint16_t i=0;i<WAKELOG_SLOTS;++i) {
    const uint16_t idx=(uint16_t)((g_wakelog_head+i)%WAKELOG_SLOTS);
    if(!(g_wakelog_present&(uint64_t(1)<<idx)))continue;
    if(skip){--skip;continue;}
    const wakelog_entry_t* e = &g_wakelog[idx];
    char det[128];
    snprintf(det, sizeof(det),
             "awake=%lums timer=%lus ext0=%u cap=%u ok=%u fail=%u spool=%u drain=%s%s",
             (unsigned long)e->awake_ms, (unsigned long)e->timer_armed_s,
             (unsigned)e->ext0_armed, (unsigned)e->captures,
             (unsigned)e->uploads_ok, (unsigned)e->uploads_fail,
             (unsigned)e->spool_depth, wakelog_drain_end_str(e->drain_end),
             e->closed ? "" : " NEVER_SLEPT");
    uart_send_sense_diag("wakelog", wakelog_cause_name(e->wake_cause),
                         "CYCLE", (int32_t)e->epoch, det);
  }
  Serial.printf("[WAKELOG] relayed %u recent cycle(s) to LCD\n", (unsigned)show);
}

static void wakelog_dump() {
  if (g_wakelog_magic != WAKELOG_MAGIC) {
    Serial.println("[WAKELOG] empty (no cycles recorded since power-up)");
    return;
  }
  const uint16_t n = wakelog_retained_count();
  Serial.printf("[WAKELOG] %u cycle(s) recorded (total %u since power-up)\n",
                (unsigned)n, (unsigned)g_wakelog_total);
  Serial.println("[WAKELOG]  idx  cause    awake_ms  timer_s  ext0  cap  up_ok  up_fail  spool  closed  local_time");
  for(uint16_t i=0;i<WAKELOG_SLOTS;++i) {
    // Stored-position order; legacy writes did not prove real chronology.
    const uint16_t idx=(uint16_t)((g_wakelog_head+i)%WAKELOG_SLOTS);
    if(!(g_wakelog_present&(uint64_t(1)<<idx)))continue;
    const wakelog_entry_t* e = &g_wakelog[idx];
    char tbuf[24] = "-";
    if (e->epoch) {
      time_t t = (time_t)e->epoch;
      struct tm lt;
      if (localtime_r(&t, &lt)) strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M:%S", &lt);
    }
    Serial.printf("[WAKELOG]  %3u  %-7s  %8lu  %7lu  %4u  %3u  %5u  %7u  %5u  %6u  %s%s\n",
                  (unsigned)i, wakelog_cause_name(e->wake_cause),
                  (unsigned long)e->awake_ms, (unsigned long)e->timer_armed_s,
                  (unsigned)e->ext0_armed, (unsigned)e->captures,
                  (unsigned)e->uploads_ok, (unsigned)e->uploads_fail,
                  (unsigned)e->spool_depth, (unsigned)e->closed, tbuf,
                  e->closed ? "" : "   <-- NEVER SLEPT (panic/hang/brownout)");
  }
}

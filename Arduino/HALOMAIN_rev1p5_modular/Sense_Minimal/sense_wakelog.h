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
} wakelog_entry_t;

RTC_DATA_ATTR static wakelog_entry_t g_wakelog[WAKELOG_SLOTS];
RTC_DATA_ATTR static uint16_t g_wakelog_head = 0;    // next slot to write
RTC_DATA_ATTR static uint16_t g_wakelog_total = 0;   // cycles ever recorded
RTC_DATA_ATTR static uint32_t g_wakelog_magic = 0;

#define WAKELOG_MAGIC 0x57414B45u   // 'WAKE'

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
  Preferences p;
  if (!p.begin(WAKELOG_NVS_NS, false)) return;
  char key[8];
  snprintf(key, sizeof(key), "e%u", (unsigned)(slot % WAKELOG_SLOTS));
  p.putBytes(key, e, sizeof(*e));
  p.putUShort("head", (uint16_t)((slot + 1) % WAKELOG_SLOTS));
  p.putUShort("total", g_wakelog_total);
  p.end();
}

// Reload RTC state from NVS after a power cut wiped it.
static void wakelog_nvs_restore() {
  Preferences p;
  if (!p.begin(WAKELOG_NVS_NS, true)) return;
  const uint16_t total = p.getUShort("total", 0);
  const uint16_t head  = p.getUShort("head", 0);
  if (total == 0) { p.end(); return; }
  for (uint16_t i = 0; i < WAKELOG_SLOTS; i++) {
    char key[8];
    snprintf(key, sizeof(key), "e%u", (unsigned)i);
    if (p.isKey(key)) p.getBytes(key, &g_wakelog[i], sizeof(wakelog_entry_t));
  }
  p.end();
  g_wakelog_total = total;
  g_wakelog_head = head;
  g_wakelog_magic = WAKELOG_MAGIC;
  Serial.printf("[WAKELOG] restored %u cycle(s) from NVS after power loss\n",
                (unsigned)total);
}

// Open a cycle. Call early in setup().
static void wakelog_begin_cycle(uint32_t epoch_now, uint8_t reset_reason) {
  if (g_wakelog_magic != WAKELOG_MAGIC) {   // RTC lost: power cut, or first ever boot
    wakelog_nvs_restore();                 // may repopulate from flash
  }
  if (g_wakelog_magic != WAKELOG_MAGIC) {  // genuinely nothing to recover
    memset((void*)g_wakelog, 0, sizeof(g_wakelog));
    g_wakelog_head = 0;
    g_wakelog_total = 0;
    g_wakelog_magic = WAKELOG_MAGIC;
  }
  wakelog_entry_t* e = &g_wakelog[g_wakelog_head];
  memset(e, 0, sizeof(*e));
  e->epoch = epoch_now;
  e->wake_cause = (uint8_t)esp_sleep_get_wakeup_cause();
  e->reset_reason = reset_reason;
  e->closed = 0;
  g_cycle_uploads_ok = g_cycle_uploads_fail = g_cycle_captures = 0;
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
  e->closed = 1;
  const uint16_t closed_slot = g_wakelog_head;
  g_wakelog_head = (uint16_t)((g_wakelog_head + 1) % WAKELOG_SLOTS);
  if (g_wakelog_total < 0xFFFF) g_wakelog_total++;
  wakelog_nvs_store(e, closed_slot);   // survives a power cut, unlike RTC
  Serial.printf("[WAKELOG] cycle closed awake=%lums timer=%lus ext0=%d cap=%u up_ok=%u up_fail=%u spool=%u\n",
                (unsigned long)e->awake_ms, (unsigned long)timer_s, ext0 ? 1 : 0,
                (unsigned)e->captures, (unsigned)e->uploads_ok,
                (unsigned)e->uploads_fail, (unsigned)spool_depth);
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
                     ? ((g_wakelog_total < WAKELOG_SLOTS) ? g_wakelog_total : WAKELOG_SLOTS)
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
  for (uint16_t i = 0; i < show; i++) {
    const uint16_t idx = (uint16_t)((g_wakelog_head + WAKELOG_SLOTS - show + i) % WAKELOG_SLOTS);
    const wakelog_entry_t* e = &g_wakelog[idx];
    char det[96];
    snprintf(det, sizeof(det),
             "awake=%lums timer=%lus ext0=%u cap=%u ok=%u fail=%u spool=%u%s",
             (unsigned long)e->awake_ms, (unsigned long)e->timer_armed_s,
             (unsigned)e->ext0_armed, (unsigned)e->captures,
             (unsigned)e->uploads_ok, (unsigned)e->uploads_fail,
             (unsigned)e->spool_depth, e->closed ? "" : " NEVER_SLEPT");
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
  const uint16_t n = (g_wakelog_total < WAKELOG_SLOTS) ? g_wakelog_total : WAKELOG_SLOTS;
  Serial.printf("[WAKELOG] %u cycle(s) recorded (total %u since power-up)\n",
                (unsigned)n, (unsigned)g_wakelog_total);
  Serial.println("[WAKELOG]  idx  cause    awake_ms  timer_s  ext0  cap  up_ok  up_fail  spool  closed  local_time");
  for (uint16_t i = 0; i < n; i++) {
    // Oldest first.
    const uint16_t idx = (uint16_t)((g_wakelog_head + WAKELOG_SLOTS - n + i) % WAKELOG_SLOTS);
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

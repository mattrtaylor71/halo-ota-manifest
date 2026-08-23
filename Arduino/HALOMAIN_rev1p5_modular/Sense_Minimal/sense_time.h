/*
 * sense_time.h
 *
 * NTP/RTC time caching: persist last-known epoch to RTC memory and
 * NVS (Preferences) so TLS can bootstrap time after deep sleep.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 20.
 *
 * Prerequisites (must be declared before #include "sense_time.h"):
 *   - <time.h>, <sys/time.h>, <Preferences.h>
 */

#ifndef SENSE_TIME_H
#define SENSE_TIME_H

#include "esp_sntp.h"

// ── Constants ────────────────────────────────────────────────────────

static const time_t TIME_VALID_MIN_EPOCH = 1700000000;
// Nightly maintenance schedule (02:00 local, DST-correct). Lives beside the
// time cache because it is the only consumer of the RTC-persisted wall clock.
#include "../halo_ota_demo/firmware/shared/NightlySchedule.h"

static const char* TIME_CACHE_NS = "time_cache";
static const char* TIME_CACHE_EPOCH_KEY = "epoch";

// ── Globals ──────────────────────────────────────────────────────────

RTC_DATA_ATTR static uint32_t g_time_cache_epoch = 0;
static bool g_time_cache_loaded = false;

// ── Time cache functions ─────────────────────────────────────────────

static uint32_t time_cache_load() {
  if (g_time_cache_epoch >= (uint32_t)TIME_VALID_MIN_EPOCH) {
    return g_time_cache_epoch;
  }
  if (g_time_cache_loaded) {
    return g_time_cache_epoch;
  }
  g_time_cache_loaded = true;
  Preferences prefs;
  if (prefs.begin(TIME_CACHE_NS, true)) {
    uint32_t epoch = prefs.getUInt(TIME_CACHE_EPOCH_KEY, 0);
    prefs.end();
    if (epoch >= (uint32_t)TIME_VALID_MIN_EPOCH) {
      g_time_cache_epoch = epoch;
    }
  }
  return g_time_cache_epoch;
}

// Best available wall clock RIGHT NOW.
//
// time_cache_load() is deliberately coarse — time_cache_store() only refreshes
// it when the new epoch is at least an hour past the old one — so it is a
// checkpoint for surviving deep sleep, NOT a clock. Reading it as "now" makes
// every derived time up to an hour stale and, worse, frozen: two calls 80s apart
// return the identical epoch. That is fine for TLS validity but wrong for
// scheduling a wake at a specific local hour.
//
// Prefer the live system clock once NTP has set it; fall back to the cache when
// it has not (early boot, or straight out of deep sleep before a sync).
static uint32_t sense_now_epoch() {
  const time_t sys_now = time(nullptr);
  if (sys_now >= (time_t)TIME_VALID_MIN_EPOCH) {
    return (uint32_t)sys_now;
  }
  return time_cache_load();
}

// POSIX TZ string used until the backend supplies the owner's real zone.
// NightlySchedule.h computes a LOCAL 02:00, so an unset or wrong TZ moves the
// maintenance wake by whole hours — observed on-device flipping between
// PST8PDT and UTC0DST0 between two calls, a 7-hour swing.
#ifndef HALO_DEFAULT_TZ
#define HALO_DEFAULT_TZ "PST8PDT,M3.2.0,M11.1.0"
#endif

static char g_tz_current[64] = HALO_DEFAULT_TZ;

// Apply a POSIX TZ string. Call at boot, and again whenever the backend hands
// us the owner's zone. Idempotent.
static void sense_set_timezone(const char* posix_tz) {
  const char* tz = (posix_tz && posix_tz[0]) ? posix_tz : HALO_DEFAULT_TZ;
  snprintf(g_tz_current, sizeof(g_tz_current), "%s", tz);
  setenv("TZ", tz, 1);
  tzset();
  Serial.printf("[TIME] TZ set to %s\n", tz);
}

// Start SNTP WITHOUT destroying the timezone.
//
// configTime(0, 0, ...) sets the process TZ to UTC as a side effect — that is
// what the 0,0 offsets mean. Every NTP (re)sync therefore silently reverted the
// zone, and the maintenance wake computed from it jumped by whole hours:
//     local="02:50:21" TZ=PST8PDT   -> next wake in 23.16h
//     local="09:50:23" TZ=UTC0DST0  -> next wake in 16.16h   (~4s later)
// The wrapper even set PST8PDT explicitly 16 lines before calling configTime,
// which then undid it. Re-applying afterwards is the fix; SNTP itself only
// needs UTC internally.
static void sense_ntp_begin() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  setenv("TZ", g_tz_current, 1);
  tzset();
}

// Stop SNTP once the clock is good. THIS IS A CRASH FIX, not tidiness.
//
// configTime() leaves SNTP running for the whole boot. If it cannot reach a
// server -- which is routine right after a WiFi hard reset -- it sits with a
// PENDING DNS request and keeps cycling servers. lwIP then runs that pending
// callback INLINE on whichever thread next resolves a name. When that thread was
// upload_worker_task doing the presign lookup, the callback chain was:
//
//   hostByName -> dns_gethostbyname -> dns_clear_cache -> dns_call_found
//     -> sntp_dns_found -> sntp_try_next_server -> sntp_request
//     -> dns_gethostbyname -> dns_alloc_pcb -> udp_new_ip_type
//   assert failed: udp_new_ip_type udp.c:1278 (Required to lock TCPIP core!)
//
// i.e. SNTP called raw lwIP from a task that does not hold the TCPIP core lock,
// and the board PANICKED mid-upload -- losing the capture held in PSRAM. Seen
// once in a 30-cycle soak (2026-08-22), always with time ALREADY valid, so the
// retries were pure liability. Stopping SNTP removes the pending callback.
static void sense_ntp_stop_if_time_valid(const char* reason) {
  if (time(nullptr) < TIME_VALID_MIN_EPOCH) {
    return;   // still need it
  }
  if (esp_sntp_enabled()) {
    esp_sntp_stop();
    Serial.printf("[TIME] SNTP stopped (%s) - clock valid, no pending DNS callback\n",
                  reason ? reason : "");
  }
}

static void time_cache_store(time_t now) {
  if (now < TIME_VALID_MIN_EPOCH) {
    return;
  }
  uint32_t epoch = (uint32_t)now;
  if (g_time_cache_epoch >= (uint32_t)TIME_VALID_MIN_EPOCH &&
      epoch < g_time_cache_epoch + 3600) {
    return;
  }
  g_time_cache_epoch = epoch;
  g_time_cache_loaded = true;
  Preferences prefs;
  if (prefs.begin(TIME_CACHE_NS, false)) {
    prefs.putUInt(TIME_CACHE_EPOCH_KEY, g_time_cache_epoch);
    prefs.end();
  }
}

static bool time_cache_bootstrap(const char* reason) {
  time_t now = time(nullptr);
  if (now >= TIME_VALID_MIN_EPOCH) {
    return true;
  }
  uint32_t cached = time_cache_load();
  if (cached < (uint32_t)TIME_VALID_MIN_EPOCH) {
    return false;
  }
  timeval tv = {};
  tv.tv_sec = (time_t)cached;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.printf("[TLS_GUARD] time_bootstrap epoch=%lu reason=%s\n",
                (unsigned long)cached,
                reason ? reason : "unknown");
  return true;
}

#endif // SENSE_TIME_H

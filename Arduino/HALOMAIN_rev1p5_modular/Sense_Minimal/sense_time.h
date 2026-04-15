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

// ── Constants ────────────────────────────────────────────────────────

static const time_t TIME_VALID_MIN_EPOCH = 1700000000;
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

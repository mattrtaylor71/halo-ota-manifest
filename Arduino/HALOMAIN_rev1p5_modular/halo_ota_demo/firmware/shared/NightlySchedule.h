// NightlySchedule.h — when to wake for the nightly maintenance check.
//
// The device is mostly off. Once a night it wakes, asks whether an update
// exists, applies it or not, and sleeps again. This computes the timer.
//
// WHY THIS IS ITS OWN FILE
// Two things here are easy to get wrong and impossible to catch on-device
// (the bugs surface twice a year, at 2am, in someone's kitchen):
//
//   1. "Tomorrow" is NOT now + 86400. Across a DST transition a local day is
//      23 or 25 hours. Adding a fixed 86400 drifts the wake an hour every
//      spring and autumn, and can skip or double a night.
//      We advance tm_mday and let mktime() re-normalise instead.
//
//   2. On spring-forward day the target hour may NOT EXIST (in US zones the
//      clock jumps 02:00 -> 03:00, so "02:00 local" is not a real instant).
//      mktime() with tm_isdst = -1 resolves this to the following hour, which
//      is the behaviour we want: wake an hour late that one night rather than
//      not at all.
//
// Timezone comes from the process TZ (setenv("TZ", ...) + tzset()), which the
// Sense sets from the value the backend returns at owner-claim, falling back to
// Pacific when the device has never received one.
//
// Pure and host-testable on purpose — see tools/test_nightly_schedule.c.

#pragma once

#include <time.h>
#include <stdint.h>

// Hour of the local day to wake for maintenance.
#define HALO_MAINTENANCE_HOUR_LOCAL 2

// Sanity bound. A correct answer is always < 25h (a 25-hour day is the
// autumn DST maximum). Anything larger means the clock or TZ is nonsense.
#define HALO_MAINTENANCE_MAX_SLEEP_S (26UL * 3600UL)

// Used when the wall clock is unusable (never synced, or nonsense), so the
// device still wakes and gets a chance to re-sync NTP and try again.
//
// This exists because "no usable clock" must never mean "no timer". A device
// that sleeps with no wake source is a brick until a human taps it — and on a
// mostly-off device in someone's kitchen, nobody knows to do that. Six hours is
// short enough to recover a bad clock the same day and long enough not to
// matter for battery.
#define HALO_MAINTENANCE_FALLBACK_S (6UL * 3600UL)

// Seconds from `now_epoch` until the next `hour`:00:00 local time.
// Returns 0 if `now_epoch` is not a usable wall clock (caller should fall back
// to a plain interval timer rather than trusting a bogus absolute wake).
static inline uint32_t halo_seconds_until_local_hour(time_t now_epoch, int hour) {
  if (now_epoch <= 0) return 0;

  struct tm lt;
  if (localtime_r(&now_epoch, &lt) == NULL) return 0;

  struct tm target = lt;
  target.tm_hour = hour;
  target.tm_min = 0;
  target.tm_sec = 0;
  target.tm_isdst = -1;          // let mktime resolve DST for this wall time

  time_t t = mktime(&target);

  // Already past (or exactly at) today's slot -> go to tomorrow. Advance the
  // CALENDAR day and re-normalise; never add 86400.
  if (t == (time_t)-1 || t <= now_epoch) {
    target = lt;
    target.tm_mday += 1;         // mktime normalises overflow (month/year ends)
    target.tm_hour = hour;
    target.tm_min = 0;
    target.tm_sec = 0;
    target.tm_isdst = -1;
    t = mktime(&target);
    if (t == (time_t)-1) return 0;
  }

  if (t <= now_epoch) return 0;  // still not in the future: unusable clock

  uint32_t delta = (uint32_t)(t - now_epoch);
  if (delta > HALO_MAINTENANCE_MAX_SLEEP_S) return 0;
  return delta;
}

// Convenience wrapper for the maintenance hour.
static inline uint32_t halo_seconds_until_maintenance(time_t now_epoch) {
  return halo_seconds_until_local_hour(now_epoch, HALO_MAINTENANCE_HOUR_LOCAL);
}

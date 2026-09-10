#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "NightlySchedule.h"

// Calendar policy core. The production coordinator wrapper owns storage and callers.
// A persisted representation must be versioned and atomic; do not write this
// in-memory struct directly to NVS or infer a binding for an old ID-only key.
struct NightlyCreditOrigin {
  char id[64] = {};
  char timezone[64] = {};
  uint32_t target_epoch = 0;
  bool bound = false;
};
enum class NightlyCreditDecision {
  NonCalendar, Due, Future, ClockUnconfirmed, TimezoneUnconfirmed,
  MissingOrigin, Invalid
};

static bool nightly_credit_is_calendar(const char* id) {
  return id && strncmp(id, "nightly_", 8) == 0;
}
static bool nightly_credit_date(const char* id, int& year, int& month, int& day) {
  if (!nightly_credit_is_calendar(id) || strlen(id) != 16) return false;
  for (unsigned i = 8; i != 16; ++i) if (id[i] < '0' || id[i] > '9') return false;
  year = 0; for (unsigned i = 8; i != 12; ++i) year = year * 10 + id[i] - '0';
  month = (id[12] - '0') * 10 + id[13] - '0';
  day = (id[14] - '0') * 10 + id[15] - '0';
  if (year < 1970 || year > 9999 || month < 1 || month > 12) return false;
  const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day >= 1 && (unsigned)day <= days[month-1] + (month == 2 && leap ? 1U : 0U);
}
static bool nightly_credit_timezone_matches(const char* configured) {
  const char* active = getenv("TZ");
  return configured && configured[0] && strlen(configured) < 64 && active &&
         strcmp(active, configured) == 0;
}

// Caller must hold the same time/TZ owner lock used for normal scheduling.
// target is the actual newly constructed arm, not a target inferred from an ID.
static bool nightly_credit_bind(NightlyCreditOrigin& out, const char* id,
                                uint32_t target, const char* configured,
                                bool fresh) {
  int year, month, day;
  if (!fresh || !nightly_credit_date(id, year, month, day) ||
      !nightly_credit_timezone_matches(configured)) return false;
  struct tm t = {};
  t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day;
  t.tm_hour = HALO_MAINTENANCE_HOUR_LOCAL; t.tm_isdst = -1;
  const time_t epoch = mktime(&t);
  // Match NightlySchedule's spring-gap normalization, but reject invalid dates
  // before mktime and reject normalization into a different calendar date.
  if (epoch <= 0 || (uint64_t)epoch > UINT32_MAX ||
      t.tm_year != year - 1900 || t.tm_mon != month - 1 || t.tm_mday != day ||
      (uint32_t)epoch != target) return false;
  NightlyCreditOrigin candidate;
  memcpy(candidate.id, id, strlen(id) + 1);
  memcpy(candidate.timezone, configured, strlen(configured) + 1);
  candidate.target_epoch = target; candidate.bound = true;
  out = candidate;
  return true;
}

static NightlyCreditDecision nightly_credit_decide(
    const NightlyCreditOrigin& origin, const char* id, uint64_t now,
    bool fresh, const char* configured) {
  if (!id || !id[0]) return NightlyCreditDecision::Invalid;
  // Relative/private and pre-existing opaque noncalendar IDs retain their
  // existing semantics; this proposal never reinterprets them as nightly dates.
  if (!nightly_credit_is_calendar(id)) return NightlyCreditDecision::NonCalendar;
  int year, month, day;
  if (!nightly_credit_date(id, year, month, day)) return NightlyCreditDecision::Invalid;
  if (!fresh || now < 1700000000ULL || now > UINT32_MAX)
    return NightlyCreditDecision::ClockUnconfirmed;
  if (!nightly_credit_timezone_matches(configured))
    return NightlyCreditDecision::TimezoneUnconfirmed;
  if (!origin.bound || !origin.target_epoch ||
      memchr(origin.id, 0, sizeof(origin.id)) == nullptr ||
      memchr(origin.timezone, 0, sizeof(origin.timezone)) == nullptr ||
      strcmp(origin.id, id) != 0)
    return NightlyCreditDecision::MissingOrigin;
  if (strcmp(origin.timezone, configured) != 0)
    return NightlyCreditDecision::TimezoneUnconfirmed;
  NightlyCreditOrigin checked;
  if (!nightly_credit_bind(checked, id, origin.target_epoch, configured, fresh))
    return NightlyCreditDecision::Invalid;
  // The original absolute target is authoritative. Never calculate "next 02"
  // at completion, add86400, renew a timeout, or impose a new debt-age cutoff.
  return now + 15ULL >= origin.target_epoch ? NightlyCreditDecision::Due
                                           : NightlyCreditDecision::Future;
}

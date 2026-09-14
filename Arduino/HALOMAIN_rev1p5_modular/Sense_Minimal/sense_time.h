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
#include <atomic>
#include <mutex>
#include "lwip/priv/tcpip_priv.h"
#include "lwip/dns.h"
#include "../halo_ota_demo/firmware/shared/NtpDnsGuard.h"

// esp_sntp_stop() only queues a callback in IDF 5.5.4. DNS quiescence needs the
// raw stop to have completed before a worker can call hostByName().
extern "C" void sntp_stop(void);
#if !CONFIG_LWIP_TCPIP_CORE_LOCKING
#error "HALO SNTP/DNS serialization requires the production TCPIP core lock"
#endif

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
static std::recursive_mutex g_time_mutex;
static const uint32_t SENSE_NTP_ATTEMPT_MS = 15000;
// Chosen once before DNS. An active durable OTA may need the SDK's next server:
// 15s DNS + <5s SDK startup + 15s first receive + 15s second receive + 5s margin.
// DNS retains its original15s deadline; no callback generation is reopened.
static uint32_t g_ntp_attempt_budget_ms = SENSE_NTP_ATTEMPT_MS;
static bool g_ntp_attempt_started = false;
static bool g_ntp_attempt_finished = false;
static bool g_ntp_running = false;
static bool g_ntp_sleep_quiesced = false;
static uint32_t g_ntp_attempt_start_ms = 0;
static unsigned g_ntp_dns_users = 0;
static uint32_t g_ntp_dns_held_ms = 0, g_ntp_dns_hold_start_ms = 0;
static std::atomic<uint32_t> g_ntp_accept_until_ms{0};
static std::atomic<uint32_t> g_ntp_received_epoch{0};
static std::atomic<bool> g_ntp_fresh_this_boot{false};
// An explicit manual action or a qualified calendar wake may use one shared
// extra opportunity after the ordinary 15s boot window. Its DNS slots are
// separate; no generation is ever reopened. Scheduled use keeps the original
// readiness deadline and never sets a manual OTA intent.
static const uint32_t SENSE_NTP_MANUAL_RETRY_MS = 40000;
static bool g_ntp_manual_retry_requested = false;  // g_time_mutex
static bool g_ntp_scheduled_retry_requested = false;  // g_time_mutex
static uint32_t g_ntp_scheduled_retry_deadline_ms = 0;
static std::atomic<bool> g_ntp_manual_retry_used{false};
static std::atomic<uint32_t> g_ntp_resolve_until_ms{0};
static std::atomic<uint32_t> g_ntp_manual_resolve_until_ms{0};
struct SenseNtpServer {
  const char* hostname;
  const bool manual_generation = false;
  bool requested = false;  // owner context, protected by g_time_mutex
  std::atomic<uint32_t> ipv4{0};
  std::atomic<bool> done{false};
};
static SenseNtpServer g_ntp_servers[3] = {
    {"pool.ntp.org"}, {"time.nist.gov"}, {"time.google.com"}};
// Never reuse/reset the original DNS callback arguments after a timeout.
// These static slots belong only to the one permitted secondary window.
static SenseNtpServer g_ntp_manual_servers[3] = {
    {"pool.ntp.org", true}, {"time.nist.gov", true}, {"time.google.com", true}};
// SNTP retains these pointers. Never pass a temporary String/caller buffer.
static char g_ntp_numeric_servers[3][16] = {};
static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS == 3, "Update all SNTP numeric server slots when SDK server count changes");

// ── Time cache functions ─────────────────────────────────────────────

static uint32_t time_cache_load() {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
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
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
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
// SNTP's callback runs after its immediate system-clock update. Only publish a
// reply received inside its own bounded client opportunity; no logging, NVS,
// mutex acquisition or network work runs in the TCPIP callback.
static void sense_ntp_accept_sync(struct timeval* tv, bool manual_retry) {
  // A late callback belonging to the first client cannot populate the retry's
  // mailbox. The original DNS callbacks keep their original, closed deadline.
  if (g_ntp_manual_retry_used.load() != manual_retry) return;
  const uint32_t deadline = g_ntp_accept_until_ms.load();
  if (!tv || tv->tv_sec < TIME_VALID_MIN_EPOCH || deadline == 0 ||
      (int32_t)((uint32_t)millis() - deadline) >= 0) return;
  uint32_t empty = 0;
  g_ntp_received_epoch.compare_exchange_strong(empty, (uint32_t)tv->tv_sec);
}
static void sense_ntp_on_sync(struct timeval* tv) {
  sense_ntp_accept_sync(tv, false);
}
static void sense_ntp_on_manual_sync(struct timeval* tv) {
  sense_ntp_accept_sync(tv, true);
}

// IDF's raw sntp_stop does NOT cancel an outstanding sntp_dns_found callback.
// Own the hostname lookups instead: this callback is safe even if Arduino's
// foreground dns_clear_cache invokes it outside the TCPIP task. Static slot
// lifetime, record-only writes and a deadline per immutable generation make
// late results harmless. SNTP itself is only given numeric IPv4 strings.
static void sense_ntp_on_dns(const char*, const ip_addr_t* address, void* arg) {
  SenseNtpServer* server = static_cast<SenseNtpServer*>(arg);
  if (!server) return;
  if (server->manual_generation != g_ntp_manual_retry_used.load()) return;
  const uint32_t deadline = server->manual_generation
      ? g_ntp_manual_resolve_until_ms.load() : g_ntp_resolve_until_ms.load();
  if (deadline != 0 && (int32_t)((uint32_t)millis() - deadline) < 0 &&
      address && IP_IS_V4(address)) {
    server->ipv4.store(ip4_addr_get_u32(ip_2_ip4(address)));
  }
  server->done.store(true);
}

// Caller holds g_time_mutex. Lock order is always application mutex -> TCPIP
// core. The callback above never takes the application mutex, avoiding inversion.
static void sense_ntp_stop_locked() {
  LOCK_TCPIP_CORE();
  g_ntp_accept_until_ms.store(0);
  sntp_stop();
  UNLOCK_TCPIP_CORE();
  g_ntp_running = false;
}

static bool sense_ntp_attempt_pending() {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  return g_ntp_attempt_started && !g_ntp_attempt_finished &&
         !g_ntp_sleep_quiesced && g_ntp_received_epoch.load() == 0 &&
         ((uint32_t)millis() - g_ntp_attempt_start_ms) < g_ntp_attempt_budget_ms;
}

static bool sense_time_has_fresh_sync() {
  return g_ntp_fresh_this_boot.load();
}

// Called only for a live explicit manual request after peer readiness. It does
// not reset the existing attempt, extend its DNS, or create a persistent retry.
static void sense_ntp_request_manual_retry() {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  if (!g_ntp_sleep_quiesced && !g_ntp_fresh_this_boot.load() &&
      !g_ntp_manual_retry_used.load()) g_ntp_manual_retry_requested = true;
}

// The production coordinator supplies an already qualified calendar wake and
// the earlier of its original readiness deadlines. Repeated service can only
// shorten that deadline, never renew the secondary opportunity.
static void sense_ntp_request_scheduled_retry(uint32_t deadline_ms) {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  if (g_ntp_sleep_quiesced || g_ntp_fresh_this_boot.load() ||
      g_ntp_manual_retry_used.load() ||
      (int32_t)(deadline_ms - (uint32_t)millis()) <= 0) return;
  if (!g_ntp_scheduled_retry_requested ||
      (int32_t)(deadline_ms - g_ntp_scheduled_retry_deadline_ms) < 0)
    g_ntp_scheduled_retry_deadline_ms = deadline_ms;
  g_ntp_scheduled_retry_requested = true;
}

// Caller holds g_time_mutex. The first attempt must already be closed and its
// mailbox drained. Missing addresses get new static DNS slots/deadline; late
// callbacks keep their original arguments and can never fill these new slots.
static void sense_ntp_try_manual_retry_locked(uint32_t now_ms) {
  if ((!g_ntp_manual_retry_requested && !g_ntp_scheduled_retry_requested) ||
      g_ntp_manual_retry_used.load() ||
      !g_ntp_attempt_finished || g_ntp_sleep_quiesced ||
      g_ntp_fresh_this_boot.load() || g_ntp_received_epoch.load() ||
      g_ntp_attempt_budget_ms != SENSE_NTP_ATTEMPT_MS) return;
  const bool scheduled = !g_ntp_manual_retry_requested;
  uint32_t retry_budget = SENSE_NTP_MANUAL_RETRY_MS;
  if (scheduled) {
    const int32_t left = (int32_t)(g_ntp_scheduled_retry_deadline_ms - now_ms);
    if (left <= 0) return;
    if ((uint32_t)left < retry_budget) retry_budget = (uint32_t)left;
  }
  sense_ntp_stop_locked();  // closes old UDP client/timers under TCPIP lock
  g_ntp_resolve_until_ms.store(0);  // original DNS generation stays closed
  g_ntp_manual_retry_used.store(true);
  unsigned cached = 0;
  for (unsigned i = 0; i < 3; ++i) {
    const uint32_t ip = g_ntp_servers[i].ipv4.load();
    g_ntp_manual_servers[i].ipv4.store(ip);
    g_ntp_manual_servers[i].requested = ip != 0;
    g_ntp_manual_servers[i].done.store(ip != 0);
    if (ip) ++cached;
  }
  g_ntp_manual_resolve_until_ms.store(now_ms +
      (retry_budget < SENSE_NTP_ATTEMPT_MS ? retry_budget : SENSE_NTP_ATTEMPT_MS));
  g_ntp_manual_retry_requested = false;
  g_ntp_scheduled_retry_requested = false;
  g_ntp_attempt_start_ms = now_ms;
  g_ntp_dns_held_ms = 0;
  g_ntp_dns_hold_start_ms = now_ms;
  g_ntp_attempt_budget_ms = retry_budget;
  g_ntp_attempt_finished = false;
  Serial.printf("[TIME] %s SNTP retry generation=1 budget_ms=%lu cached_numeric=%u fresh_dns_slots=%u\n",
                scheduled ? "scheduled" : "manual", (unsigned long)retry_budget, cached, 3U - cached);
}

static void sense_ntp_begin() {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  const uint32_t now_ms = millis();
  sense_ntp_try_manual_retry_locked(now_ms);
  if (g_ntp_attempt_finished || g_ntp_sleep_quiesced || g_ntp_received_epoch.load() != 0) return;
  if (!g_ntp_attempt_started) {
    g_ntp_attempt_started = true;
    g_ntp_attempt_start_ms = now_ms;
    g_ntp_dns_held_ms = 0;
    g_ntp_dns_hold_start_ms = now_ms;
#if defined(HALO_SENSE_PROD_WRAPPER) && HALO_DURABLE_OTA_POLICY
    g_ntp_attempt_budget_ms = halo_policy_ntp_budget(now_ms);
#endif
    g_ntp_resolve_until_ms.store(now_ms + SENSE_NTP_ATTEMPT_MS);
  }
  if ((now_ms - g_ntp_attempt_start_ms) >= g_ntp_attempt_budget_ms ||
      g_ntp_dns_users != 0 || g_ntp_running) return;
  const bool manual_retry = g_ntp_manual_retry_used.load();
  SenseNtpServer* servers = manual_retry ? g_ntp_manual_servers : g_ntp_servers;
  const uint32_t resolve_until = manual_retry ? g_ntp_manual_resolve_until_ms.load()
                                              : g_ntp_resolve_until_ms.load();
  for (unsigned i = 0; i < 3; ++i) {
    SenseNtpServer& server = servers[i];
    if (server.requested) continue;
    if (!resolve_until || (int32_t)((uint32_t)millis() - resolve_until) >= 0) continue;
    server.requested = true;
    ip_addr_t address = {};
    LOCK_TCPIP_CORE();
    const err_t result = dns_gethostbyname_addrtype(server.hostname, &address,
                                                  sense_ntp_on_dns, &server,
                                                  LWIP_DNS_ADDRTYPE_IPV4);
    if (result == ERR_OK) sense_ntp_on_dns(server.hostname, &address, &server);
    else if (result != ERR_INPROGRESS) sense_ntp_on_dns(server.hostname, nullptr, &server);
    UNLOCK_TCPIP_CORE();
  }
  const char* numeric[3] = {};
  bool have_address = false;
  for (unsigned i = 0; i < 3; ++i) {
    const uint32_t ip = servers[i].ipv4.load();
    if (ip == 0) continue;
    IPAddress(ip).toString().toCharArray(g_ntp_numeric_servers[i], sizeof(g_ntp_numeric_servers[i]));
    numeric[i] = g_ntp_numeric_servers[i];
    have_address = true;
  }
  if (!have_address || ((uint32_t)millis() - g_ntp_attempt_start_ms) >= g_ntp_attempt_budget_ms) return;
  const char* first_address = nullptr;
  for (const char* address : numeric) if (address) { first_address = address; break; }
  for (const char*& address : numeric) if (!address) address = first_address;
  // A suspended attempt resumes with the SAME absolute deadline. A stop is
  // synchronous, so no earlier callback can race registration/restart.
  sense_ntp_stop_locked();
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
  esp_sntp_set_time_sync_notification_cb(manual_retry ? sense_ntp_on_manual_sync
                                                    : sense_ntp_on_sync);
  g_ntp_accept_until_ms.store(g_ntp_attempt_start_ms + g_ntp_attempt_budget_ms);
  // configTime replaces all three server-name slots. A numeric lookup returns
  // ERR_OK directly in lwIP, without registering its unsafe sntp_dns_found.
  // Give the next resolved server the first opportunity after a timeout.
  const unsigned first = manual_retry ? 1U : 0U;
  configTime(0, 0, numeric[first], numeric[(first + 1U) % 3U],
             numeric[(first + 2U) % 3U]);
  setenv("TZ", g_tz_current, 1);
  tzset();
  g_ntp_running = true;
  Serial.printf("[TIME] SNTP attempt active remaining_ms=%lu\n",
                (unsigned long)(g_ntp_attempt_budget_ms - (now_ms - g_ntp_attempt_start_ms)));
}

// Quiescing before non-SNTP DNS is a crash fix, not tidiness.
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
// once in a 30-cycle soak (2026-08-22). A plausible retained clock does not prove
// a fresh reply. Nested network scopes stop SNTP synchronously and prevent any
// other task from restarting it while an application DNS operation may run.
extern "C" void halo_sntp_dns_acquire() {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  if (g_ntp_dns_users++ == 0) g_ntp_dns_hold_start_ms = millis();
  if (g_ntp_running) sense_ntp_stop_locked();
}

extern "C" void halo_sntp_dns_release() {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  if (g_ntp_dns_users != 0 && --g_ntp_dns_users == 0)
    g_ntp_dns_held_ms += uint32_t((uint32_t)millis() - g_ntp_dns_hold_start_ms);
  // Only normal loop service may resume the active client's fixed deadline.
}

extern "C" bool halo_sntp_sync_pending() {
  return sense_ntp_attempt_pending();
}

static void time_cache_store(time_t now, bool authoritative = false) {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  // A worker may have sampled its argument before a backward correction, then
  // waited for this mutex. Do not let that stale sample undo the fresh cache.
  if (!authoritative && g_ntp_fresh_this_boot.load()) now = time(nullptr);
  if (now < TIME_VALID_MIN_EPOCH) {
    return;
  }
  uint32_t epoch = (uint32_t)now;
  if (g_time_cache_epoch >= (uint32_t)TIME_VALID_MIN_EPOCH &&
      !(authoritative && epoch < g_time_cache_epoch) &&
      (uint64_t)epoch < (uint64_t)g_time_cache_epoch + 3600ULL) {
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

// Called by the Arduino loop/boot owner, never by the SNTP callback or upload
// worker. A timely reply remains valid if service is delayed by other work;
// an out-of-budget callback never enters the mailbox in the first place.
static void sense_ntp_service() {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  const bool expired = g_ntp_attempt_started && !g_ntp_attempt_finished &&
      ((uint32_t)millis() - g_ntp_attempt_start_ms) >= g_ntp_attempt_budget_ms;
  if (g_ntp_received_epoch.load() == 0 && !expired) return;
  g_ntp_resolve_until_ms.store(0);
  g_ntp_manual_resolve_until_ms.store(0);
  // Stop under the core lock before draining the mailbox. A timely callback
  // may finish between the first read above and acquiring that lock.
  sense_ntp_stop_locked();
  const uint32_t received = g_ntp_received_epoch.exchange(0);
  if (received >= (uint32_t)TIME_VALID_MIN_EPOCH) {
    g_ntp_attempt_finished = true;
    time_cache_store((time_t)received, true);
    g_ntp_fresh_this_boot.store(true);
    Serial.printf("[TIME] SNTP fresh epoch=%lu elapsed_ms=%lu\n",
                  (unsigned long)received,
                  (unsigned long)((uint32_t)millis() - g_ntp_attempt_start_ms));
  } else if (expired) {
    g_ntp_attempt_finished = true;
    const bool secondary = g_ntp_manual_retry_used.load();
    SenseNtpServer* servers = secondary ? g_ntp_manual_servers : g_ntp_servers;
    unsigned ready = 0, requested = 0;
    for (unsigned i = 0; i < 3; ++i) {
      if (servers[i].ipv4.load()) ++ready;
      if (servers[i].requested) ++requested;
    }
    const uint32_t held = g_ntp_dns_held_ms + (g_ntp_dns_users
        ? uint32_t((uint32_t)millis() - g_ntp_dns_hold_start_ms) : 0);
    Serial.printf("[TIME] SNTP timeout generation=%u budget_ms=%lu dns_ready=%u dns_requested=%u dns_holds=%u held_ms=%lu\n",
                  secondary ? 1U : 0U, (unsigned long)g_ntp_attempt_budget_ms,
                  ready, requested, g_ntp_dns_users, (unsigned long)held);
    Serial.println("[TIME] SNTP attempt timed out; retained time is unconfirmed");
  }
}

static void sense_ntp_quiesce_for_sleep() {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  g_ntp_sleep_quiesced = true;
  g_ntp_resolve_until_ms.store(0);
  g_ntp_manual_resolve_until_ms.store(0);
  sense_ntp_stop_locked();
  sense_ntp_service();  // preserve any timely reply that completed before stop
}

static bool time_cache_bootstrap(const char* reason) {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  time_t now = time(nullptr);
  if (now >= TIME_VALID_MIN_EPOCH) {
    return true;
  }
  uint32_t cached = time_cache_load();
  if (cached < (uint32_t)TIME_VALID_MIN_EPOCH) {
    return false;
  }
  // The SDK updates system time before invoking our sync callback. Serialize
  // the final recheck/write with that update, so a reply arriving during the
  // cache read cannot be overwritten by the retained checkpoint.
  LOCK_TCPIP_CORE();
  const bool needs_bootstrap = time(nullptr) < TIME_VALID_MIN_EPOCH;
  if (needs_bootstrap) {
    timeval tv = {};
    tv.tv_sec = (time_t)cached;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
  }
  UNLOCK_TCPIP_CORE();
  if (needs_bootstrap) {
    Serial.printf("[TLS_GUARD] time_bootstrap epoch=%lu reason=%s\n",
                  (unsigned long)cached,
                  reason ? reason : "unknown");
  }
  return true;
}

#endif // SENSE_TIME_H

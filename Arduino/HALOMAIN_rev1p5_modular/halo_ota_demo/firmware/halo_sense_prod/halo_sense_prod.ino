/*
 * HALO SENSE PROD
 *
 * Wrapper around Sense_Minimal with OTA/MQTT framework.
 * - Functional Sense code remains intact (imported from Sense_Minimal).
 * - Adds MQTT/TRUTH, OTA intent parsing, and OTA apply pipeline (manual trigger only).
 */

#define HALO_SENSE_PROD_WRAPPER 1
// Backend rejects report_type="window_armed" with 400 invalid_field. Off until
// that changes; see the call site for why leaving it on is actively harmful.
#ifndef HALO_OTA_REPORT_WINDOW_ARMED
#define HALO_OTA_REPORT_WINDOW_ARMED 0
#endif
#define HALO_SENSE_UPLOAD_PERSISTENCE 1
// Overridable from the build command, like the other bench flags. It was an
// unconditional #define, which silently beat --build-property -DOTA_TEST_BUILD=1
// and made the OTA_NOW test hook impossible to enable.
#ifndef OTA_TEST_BUILD
#define OTA_TEST_BUILD 0
#endif
#define HALO_MQTT_ALWAYS_ON 1
#define HALO_UART_SEND_WIFI_PASS 1

#include <Arduino.h>

#include <mbedtls/platform.h>

// Coalesce LIST_REFRESH requests (Sense wrapper only)
static bool g_list_refresh_inflight = false;
static unsigned long g_last_list_refresh_request_ms = 0;
static const unsigned long LIST_REFRESH_COOLDOWN_MS = 3000;

static volatile bool g_lcd_ota_done = false;
static char g_lcd_ota_result[32] = "unknown";
static char g_lcd_ota_version[32] = "";
static uint32_t g_lcd_ota_request_id = 0;
static bool g_lcd_ota_request_active = false;
static bool g_lcd_ota_ack_received = false;
static unsigned long g_lcd_ota_request_start_ms = 0;
static uint32_t g_lcd_ota_last_request_epoch = 0;
// millis() of the last time g_lcd_ota_version was set from a REAL
// LCD_OTA_QUERY_RESP (not from a manifest). 0 = never / invalidated.
static unsigned long g_lcd_fw_query_ms = 0;
// Refresh the cached LCD fw version if it is older than this when the UART
// link is recent at pre-sleep. Keeps cloud lcd_fw close to reality after a
// LCD OTA reboot without querying on every sleep.
static const unsigned long LCD_FW_QUERY_STALE_MS = 300000;  // 5 min
RTC_DATA_ATTR static uint32_t g_lcd_ota_recent_request_id = 0;
static bool g_lcd_ota_attempted_this_window = false;
static uint32_t g_lcd_ota_window_remaining_s = 0;
static bool g_lcd_maint_ack_received = false;
static unsigned long g_lcd_maint_ack_ms = 0;
static uint32_t g_lcd_maint_ack_remaining_s = 0;
static uint32_t g_lcd_maint_ack_wake_in_s = 0;
static unsigned long g_maint_sync_last_tx_ms = 0;
static bool g_maint_pending_last = false;
static uint8_t g_maint_sync_send_count = 0;
static bool g_maint_sync_no_ack_logged = false;
static const unsigned long MAINT_SYNC_RESEND_MS = 1500;
static const uint8_t MAINT_SYNC_MAX_SENDS = 3;
// Arm-time delivery race fix: after a tap wakes both boards the LCD idle-sleeps at
// ~10s, but the Sense needs ~10-15s for WiFi+NTP+schedule-fetch before it can send
// the real MAINT_WINDOW (which needs now_epoch). The pending-sync flag is only set
// AFTER the HTTPS schedule fetch (which already requires valid time), so a
// "pending && !time_valid" trigger never fires on a fresh arm. Instead we send a
// lightweight MAINT_KEEPALIVE throughout the post-wake connect/fetch phase so the
// freshly-tapped LCD does NOT idle-sleep before the real window can be delivered.
static unsigned long g_maint_keepalive_last_tx_ms = 0;
static const unsigned long MAINT_KEEPALIVE_RESEND_MS = 2000;
// Bound the arm-time keepalive to the early part of a wake. last_wake_ms (set in
// setup() every wake, since the Sense reboots on deep-sleep wake and millis()
// resets) is the wake-start reference.
static const unsigned long KEEPALIVE_ARM_WINDOW_MS = 25000;
static const unsigned long MAINT_SYNC_BLOCK_SLEEP_MS = 5000;
static const unsigned long LCD_OTA_ACK_TIMEOUT_MS = 15000;
static const unsigned long LCD_OTA_RESULT_TIMEOUT_MS = 600000;
static const uint32_t LCD_OTA_MIN_INTERVAL_S = 21600;  // 6 hours
static const uint32_t MAINT_TIME_JUMP_RESYNC_S = 300;
static uint32_t g_last_time_valid_epoch = 0;
static const uint32_t PRE_SLEEP_WIFI_RECOVERY_MS = 30000;
static const uint32_t PRE_SLEEP_WIFI_RETRY_INTERVAL_MS = 2000;
static const uint32_t PRE_SLEEP_WIFI_HARD_RESET_MS = 6000;
static bool g_ota_pending_verify_active = false;
static volatile bool g_lcd_ota_task_running = false;

#include "../../../Sense_Minimal/Sense_Minimal.ino"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "../shared/BuildFlags.h"
#include "../shared/BuildInfo.h"
#include "../shared/Version.h"
#include "../shared/Log.h"
#include "../shared/BootState.h"

// Forward-declared enum (must precede the .ino auto-prototype hoist of
// ota_sched_revalidate(), whose return type is SchedRevalidate; definition is
// near line ~1525).
enum SchedRevalidate { REVAL_VALID, REVAL_CANCELLED, REVAL_REPLACED, REVAL_FETCH_FAILED };

#include "../shared/ProvisioningState.h"
#include "../shared/ProvisioningManager.h"
#include "../shared/Truth.h"
#include "../shared/SenseOtaPolicy.h"
#include "../shared/MaintenanceWindow.h"
#include "../shared/MqttClient.h"
#include "../shared/OtaIntent.h"
#include "../shared/ManifestClient.h"
#include "../shared/SenseOtaApplier.h"
#include "../shared/OtaExpect.h"
#include "../shared/HealthGate.h"
#include "../shared/WifiGuard.h"
#include "../shared/Watchdog.h"

// Up to this many extra string key/values can be attached to a report
// (keeps the payload builder allocation-free / on the stack). Defined here —
// before the FIRST function in the .ino (line ~below) and thus before Arduino's
// auto-generated prototype block — so the generated prototypes for
// ota_report_build_payload() / ota_report_post(), which reference
// OtaReportExtras by type, see a complete declaration.
#define OTA_REPORT_MAX_EXTRA 4
struct OtaReportExtras {
  uint8_t count;
  const char* keys[OTA_REPORT_MAX_EXTRA];
  const char* vals[OTA_REPORT_MAX_EXTRA];
  OtaReportExtras() : count(0) {}
  void add(const char* k, const char* v) {
    if (count < OTA_REPORT_MAX_EXTRA && k && v && v[0]) {
      keys[count] = k;
      vals[count] = v;
      ++count;
    }
  }
};

extern "C" bool halo_wifi_hard_reset_for_ota(const char* reason, uint32_t timeout_ms) {
  return wifi_hard_reset_and_reconnect(reason ? reason : "ota_http_retry", timeout_ms);
}

// ── TLS DMA-reserve hooks (used by the owner-claim in ProvisioningManager.cpp) ──
// The owner-claim HTTPS POST runs during provisioning when internal heap is
// fragmented (AP_STA mode); the TLS handshake needs ~25-30KB CONTIGUOUS internal
// RAM. The 16KB g_camera_dma_reserve (held since setup(), camera idle during
// provisioning) is freed before the handshake and re-acquired after — the same
// proven pattern the upload + voice TLS paths use (sense_upload.h / sense_voice.h).
// g_camera_dma_reserve and CAMERA_DMA_RESERVE_BYTES are statics in
// Sense_Minimal.ino, which is #included into this translation unit (line above),
// so they are in scope here. These hooks have external linkage so the shared
// ProvisioningManager.cpp (a separate TU) can call them.
// Both delegate to camera_dma_reserve_{release,acquire} (Sense_Minimal.ino) so
// every release/re-acquire in the firmware is logged the same way and a failure
// is loud. The previous restore logged "restored reserve, ptr=%p" unconditionally,
// which prints ptr=0x0 on failure and still READS as success -- misleading rather
// than silent, but it hides the same state: an unprotected DMA region that makes
// the next esp_camera_init() fail with a bare 0xffffffff.
extern "C" void halo_tls_free_dma_reserve() {
  camera_dma_reserve_release("provisioning_tls");
}

extern "C" void halo_tls_restore_dma_reserve() {
  camera_dma_reserve_acquire("provisioning_tls");
}

// Shared RAII guard for outbound TLS in THIS translation unit: frees the 16KB
// camera DMA reserve for the duration of an HTTPS handshake and ALWAYS restores
// it on every scope exit (early returns included). Mirrors commit 809's
// DmaReserveTlsGuard (ProvisioningManager.cpp) — the schedule-fetch and
// report-POST TLS paths can run while the camera reserve is held (and, during
// provisioning, while the SoftAP fragments internal RAM), which starves
// esp_aes_process_dma and panics. The free/restore hooks are idempotent. The
// `active` flag lets a call site arm the guard only on its HTTPS branch (so the
// plain-HTTP branch doesn't needlessly free/restore the reserve).
struct ScopedTlsDmaReserve {
  bool active_;
  explicit ScopedTlsDmaReserve(bool active) : active_(active) {
    if (active_) halo_tls_free_dma_reserve();
  }
  ~ScopedTlsDmaReserve() {
    if (active_) halo_tls_restore_dma_reserve();
  }
};

extern "C" bool halo_uart_link_recent(unsigned long max_age_ms);

static const unsigned long LCD_MAINT_LINK_RECENT_MS = 15000;
static const unsigned long LCD_OTA_RETRY_INTERVAL_MS = 4000;
static const unsigned long LCD_MAINT_RESEND_INTERVAL_MS = 5000;
static const unsigned long LCD_MAINT_KEEPALIVE_MS = 12000;
static const unsigned long MAINT_OTA_RETRY_INTERVAL_MS = 15000;
static const uint8_t SENSE_MAINT_OTA_MAX_ATTEMPTS = 3;
static const uint8_t LCD_MAINT_OTA_MAX_ATTEMPTS = 3;
static const uint8_t MAINT_FOLLOWUP_RETRY_MAX_ATTEMPTS = 3;
static const uint32_t MAINT_FOLLOWUP_RETRY_DELAYS_S[MAINT_FOLLOWUP_RETRY_MAX_ATTEMPTS] = {120, 300, 600};

// OTA configuration defaults (same behavior as halo_ota_demo)
#ifndef OTA_CHANNEL
#define OTA_CHANNEL "dev"
#endif
// PROD by default (2026-08-21). A plain `arduino-cli compile` must be
// ship-safe, exactly like the bench flags in SHIP_CHECKLIST §1 — the previous
// default meant every device built from this tree fetched firmware from
// halo-ota-dev, the bucket we push TEST builds to. A stray dev push would have
// gone straight to customer hardware.
//
// Bench/dev builds opt IN:  --build-property "compiler.cpp.extra_flags=-DOTA_DEFAULT_ENV=\"dev\""
#ifndef OTA_DEFAULT_ENV
#define OTA_DEFAULT_ENV "prod"
#endif
#ifndef OTA_S3_BUCKET
#define OTA_S3_BUCKET ""
#endif
#ifndef OTA_S3_REGION
#define OTA_S3_REGION ""
#endif
#ifndef OTA_S3_PREFIX
#define OTA_S3_PREFIX ""
#endif
#define OTA_TEST_BYPASS_REBOOT_LOOP_GUARD 0  // 0 = NORMAL (reboot-loop guard active); 1 = test-only OTA forcing
#define OTA_DEV_BUCKET  "halo-ota-dev"
#define OTA_DEV_REGION  "us-east-1"
#define OTA_DEV_PREFIX  "halo/ota"
#define OTA_PROD_BUCKET "halo-ota-prod"
#define OTA_PROD_REGION "us-east-1"
#define OTA_PROD_PREFIX "halo/ota"
#ifndef OTA_SCHED_HTTP_URL
#define OTA_SCHED_HTTP_URL "https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com/ota/schedule"
#endif
#ifndef OTA_SCHED_HTTP_TIMEOUT_MS
#define OTA_SCHED_HTTP_TIMEOUT_MS 8000UL
#endif
#ifndef OTA_SCHED_HTTP_ROOT_CA
#define OTA_SCHED_HTTP_ROOT_CA ""
#endif
#ifndef OTA_SCHED_HTTP_INSECURE
#define OTA_SCHED_HTTP_INSECURE 0
#endif
#ifndef OTA_REPORT_HTTP_URL
#define OTA_REPORT_HTTP_URL "https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com/ota/report"
#endif
#ifndef OTA_REPORT_HTTP_TIMEOUT_MS
#define OTA_REPORT_HTTP_TIMEOUT_MS 2000UL
#endif
#ifndef OTA_REPORT_HTTP_ROOT_CA
#define OTA_REPORT_HTTP_ROOT_CA OTA_SCHED_HTTP_ROOT_CA
#endif
#ifndef OTA_REPORT_HTTP_INSECURE
#define OTA_REPORT_HTTP_INSECURE OTA_SCHED_HTTP_INSECURE
#endif

static void ota_http_configure_tls(WiFiClientSecure& client,
                                   const char* log_tag,
                                   const char* configured_root_ca) {
#if HAS_CRT_BUNDLE
  if (configured_root_ca && configured_root_ca[0]) {
    client.setCACert(configured_root_ca);
    LOG_INFO("[%s] using configured root CA", log_tag ? log_tag : "OTA_TLS");
  } else {
    extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
    extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
    client.setCACertBundle(x509_crt_bundle_start,
                           x509_crt_bundle_end - x509_crt_bundle_start);
    LOG_INFO("[%s] using crt bundle", log_tag ? log_tag : "OTA_TLS");
  }
#else
  if (configured_root_ca && configured_root_ca[0]) {
    client.setCACert(configured_root_ca);
    LOG_INFO("[%s] using configured root CA", log_tag ? log_tag : "OTA_TLS");
  } else if (rootCA && rootCA[0]) {
    client.setCACert(rootCA);
    LOG_INFO("[%s] using default root CA", log_tag ? log_tag : "OTA_TLS");
  } else {
    client.setInsecure();
    LOG_WARN("[%s] no root CA configured; using insecure fallback", log_tag ? log_tag : "OTA_TLS");
  }
#endif
}

// Extern for OtaIntent.cpp
volatile bool mqtt_ota_check_requested = false;

static uint32_t g_boot_count = 0;
static unsigned long g_boot_time_ms = 0;
static bool g_reboot_loop_detected = false;

ManifestClient g_manifest_client;
static SenseOtaApplier g_ota_applier;
static HealthGate g_health_gate;
static ProvisioningManager g_provisioning_manager;

static bool g_ota_check_done = false;
static bool g_ota_check_requested = false;
static bool g_ota_apply_in_progress = false;
static bool g_ota_skip_logged = false;
static bool g_ota_check_in_progress = false;
// One bounded automatic check per timer boot, or to heal a persisted LCD debt.
// Keep this independent of the retired maintenance-window mode flags.
static bool g_boot_ota_pending = false;
static uint32_t g_boot_ota_deadline_ms = 0;
static uint32_t g_boot_ota_next_try_ms = 0;
static bool g_boot_ota_time_sync_started = false;
static bool g_boot_ota_begin_reported = false;
static const char* g_boot_ota_reason = "nightly";
// Corroborate the original automatic trigger once after LCD answers a query.
// The initial diagnostic can precede the LCD's co-wake timer by several seconds.
struct BootOtaBeginRecord {
  const char* event = nullptr;
  const char* reason = nullptr;
  int32_t code = 0;
  char detail[96] = {0};  // Same capacity as the original begin diagnostic.
  bool repeat_pending = false;
  bool noop_reported = false;
};
static BootOtaBeginRecord g_boot_ota_begin_record;
static bool g_boot_ota_diag_context = false;
// One main-loop readiness gate is shared by manual and automatic callers. Its
// deadline is never renewed by a retry or a repeated LCD announcement.
struct OtaPeerGate {
  bool active = false, ready = false, querying = false, locked = false;
  bool legacy = false, entered = false;
  uint32_t deadline_ms = 0, next_query_ms = 0, proof_ms = 0, peer_boot = 0, sequence = 0;
  char owner[40] = {0}, challenge[40] = {0};
};
static OtaPeerGate g_peer_gate;
static uint32_t g_coord_sense_boot_id = 0;
static uint32_t g_coord_sequence = 0;
struct LcdTimerNotice {
  char schedule[64] = {0};
  uint32_t boot_id = 0, epoch = 0;
  int wake = -1, reset = -1;
  bool resumed = false, pending = false;
};
static LcdTimerNotice g_lcd_timer_notice;
static LcdTimerNotice g_lcd_timer_origin;
static uint32_t g_coord_seen_boots[3] = {0};
static uint8_t g_coord_seen_count = 0;
static uint32_t g_lcd_timer_seen_boot = 0;
static char g_coord_schedule[64] = {0};
static char g_coord_completed[64] = {0};
static char g_coord_completed_ids[8][64] = {{0}};
static uint8_t g_coord_completed_count = 0;
static char g_coord_pending[64] = {0};
static bool g_peer_episode_finished = false;
static char g_coord_completion_target[32] = {0};
static LcdOtaBudget g_lcd_work_budget;
static bool g_lcd_work_budget_live = false, g_peer_continue_work = false;
static char g_lcd_work_schedule[64] = {0};
static uint32_t g_lcd_work_peer_boot = 0;
// A reset during binary RX may leave the peer's control mode unknown without
// an OTA request. One boot-local, query-only probe can restore normal UART.
static bool g_control_probe_active = false, g_control_probe_finished = false;
static bool g_control_probe_querying = false;
static uint32_t g_control_probe_deadline_ms = 0, g_control_probe_next_ms = 0;
static void ota_peer_service();
static bool ota_peer_accept_new_request();
static void ota_peer_cancel(const char* reason);
static bool ota_peer_ready();
static void ota_peer_schedule_complete();

static volatile bool g_manual_ota_override = false;
static unsigned long g_manual_ota_override_until_ms = 0;
static const unsigned long MANUAL_OTA_OVERRIDE_TTL_MS = 5UL * 60UL * 1000UL;
static unsigned long g_manual_ota_request_ms = 0;
static unsigned long g_manual_ota_wifi_retry_ms = 0;
static const unsigned long MANUAL_OTA_WIFI_RETRY_MS = 5000;

static bool g_ota_simple_proof_started = false;
static bool g_ota_simple_proof_done = false;
static unsigned long g_ota_proof_start_ms = 0;
static bool g_ota_validated_this_boot = false;
static unsigned long g_ota_validated_time_ms = 0;
static bool g_ota_uart_active = false;
#if OTA_TEST_BUILD
static bool g_ota_test_skip_mark_valid = false;
static uint32_t g_ota_test_crash_after_boot_ms = 0;
#endif
static bool g_pending_provision_qr = false;
static unsigned long g_last_provision_qr_ms = 0;
static const unsigned long PROVISION_QR_RESEND_MS = 3000;
static ProvisioningState::State g_last_prov_state = ProvisioningState::STATE_UNPROVISIONED;
static bool g_defer_ota_post_provision = false;
static bool g_post_provision_list_refresh_pending = false;
static bool g_lcd_wifi_creds_sent = false;
static const char* g_ota_config_source = "unknown";
static bool g_lcd_alive = false;
static bool g_lcd_got_creds_ack = false;
static bool g_lcd_got_wifion_ack = false;
static unsigned long g_lcd_wifi_send_start_ms = 0;
static unsigned long g_last_creds_send_ms = 0;
static unsigned long g_last_wifion_send_ms = 0;
static bool g_lcd_wifi_send_failed = false;
static const unsigned long LCD_WIFI_SEND_RETRY_MS = 2000;
static const unsigned long LCD_WIFI_SEND_TIMEOUT_MS = 60000;
RTC_DATA_ATTR static char g_sched_last_event[24] = "none";
RTC_DATA_ATTR static uint32_t g_sched_last_event_epoch = 0;
RTC_DATA_ATTR static char g_sched_last_event_request_id[64] = "";
RTC_DATA_ATTR static uint8_t g_maint_consumed_valid = 0;
RTC_DATA_ATTR static uint64_t g_maint_consumed_start_epoch = 0;
RTC_DATA_ATTR static uint64_t g_maint_consumed_end_epoch = 0;
RTC_DATA_ATTR static char g_maint_consumed_request_id[64] = "";
RTC_DATA_ATTR static uint32_t g_lcd_maint_ack_epoch = 0;
RTC_DATA_ATTR static char g_lcd_maint_ack_request_id[64] = "";
RTC_DATA_ATTR static char g_lcd_maint_ack_status[24] = "";
RTC_DATA_ATTR static uint8_t g_lcd_maint_ack_persisted = 0;
RTC_DATA_ATTR static uint64_t g_lcd_maint_ack_start_epoch = 0;
RTC_DATA_ATTR static uint32_t g_lcd_maint_ack_duration_sec = 0;
RTC_DATA_ATTR static uint32_t g_lcd_maint_ack_grace_before_sec = 0;
RTC_DATA_ATTR static uint32_t g_lcd_maint_ack_grace_after_sec = 0;
RTC_DATA_ATTR static uint32_t g_maint_last_tx_epoch = 0;
RTC_DATA_ATTR static char g_maint_last_tx_request_id[64] = "";
RTC_DATA_ATTR static uint32_t g_maint_last_tx_remaining_s = 0;
RTC_DATA_ATTR static uint32_t g_maint_last_tx_wake_in_s = 0;
RTC_DATA_ATTR static uint8_t g_maint_last_tx_clear = 0;
RTC_DATA_ATTR static uint8_t g_maint_last_tx_link_recent = 0;
RTC_DATA_ATTR static uint8_t g_maint_last_tx_send_count = 0;
RTC_DATA_ATTR static char g_maint_sync_resolution[24] = "idle";
RTC_DATA_ATTR static uint32_t g_maint_sync_resolution_epoch = 0;
RTC_DATA_ATTR static char g_maint_sync_resolution_request_id[64] = "";
RTC_DATA_ATTR static uint32_t g_maint_last_idle_ms = 0;
RTC_DATA_ATTR static uint8_t g_maint_last_idle_gate_hit = 0;
RTC_DATA_ATTR static uint8_t g_maint_last_idle_gate_bypassed = 0;
RTC_DATA_ATTR static uint8_t g_maint_last_idle_http_window = 0;
RTC_DATA_ATTR static char g_maint_last_idle_gate_reason[32] = "none";
RTC_DATA_ATTR static uint8_t g_maint_followup_retry_active = 0;
RTC_DATA_ATTR static uint8_t g_maint_followup_retry_attempts = 0;
RTC_DATA_ATTR static uint32_t g_maint_followup_retry_wake_epoch = 0;
RTC_DATA_ATTR static char g_maint_followup_retry_reason[32] = "";
RTC_DATA_ATTR static char g_maint_followup_retry_request_id[64] = "";
static char g_http_sched_last_status[24] = "never";
static int32_t g_http_sched_last_http_code = 0;
static uint32_t g_http_sched_last_check_epoch = 0;
static char g_http_sched_last_request_id[64] = "";
static char g_maint_pending_request_id[64] = "";
static unsigned long g_maint_sync_wait_until_ms = 0;
static bool g_maint_followup_retry_wake = false;

static void ota_http_schedule_note(const char* status, int http_code, const char* request_id) {
  strncpy(g_http_sched_last_status, status ? status : "unknown", sizeof(g_http_sched_last_status) - 1);
  g_http_sched_last_status[sizeof(g_http_sched_last_status) - 1] = '\0';
  g_http_sched_last_http_code = http_code;
  time_t now = time(nullptr);
  g_http_sched_last_check_epoch = (now > 1700000000) ? (uint32_t)now : 0;
  if (request_id && request_id[0]) {
    strncpy(g_http_sched_last_request_id, request_id, sizeof(g_http_sched_last_request_id) - 1);
    g_http_sched_last_request_id[sizeof(g_http_sched_last_request_id) - 1] = '\0';
  } else {
    g_http_sched_last_request_id[0] = '\0';
  }
  // Forward schedule status to LCD for in-enclosure debugging
  char dev_id[32] = {0};
  load_runtime_device_id(dev_id, sizeof(dev_id));
  char detail[160];
  snprintf(detail, sizeof(detail), "status=%s http=%d req=%s dev=%s",
           status ? status : "?", http_code,
           (request_id && request_id[0]) ? request_id : "-",
           dev_id[0] ? dev_id : "?");
  uart_send_sense_diag("ota_sched", "note", "OTA_SCHED", http_code, detail);
}

static void sched_event_note(const char* event, const char* request_id) {
  strncpy(g_sched_last_event, event ? event : "unknown", sizeof(g_sched_last_event) - 1);
  g_sched_last_event[sizeof(g_sched_last_event) - 1] = '\0';
  time_t now = time(nullptr);
  g_sched_last_event_epoch = (now > 1700000000) ? (uint32_t)now : 0;
  if (request_id && request_id[0]) {
    strncpy(g_sched_last_event_request_id, request_id, sizeof(g_sched_last_event_request_id) - 1);
    g_sched_last_event_request_id[sizeof(g_sched_last_event_request_id) - 1] = '\0';
  } else {
    g_sched_last_event_request_id[0] = '\0';
  }
}

static uint32_t maint_sync_epoch_now() {
  time_t now = time(nullptr);
  return (now > 1700000000) ? (uint32_t)now : 0;
}

static void maint_sync_copy_str(char* dst, size_t dst_len, const char* src) {
  if (!dst || dst_len == 0) {
    return;
  }
  if (src && src[0]) {
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
  } else {
    dst[0] = '\0';
  }
}

static bool maintenance_followup_retry_pending() {
  return g_maint_followup_retry_active != 0 &&
         g_maint_followup_retry_wake_epoch > 0 &&
         g_maint_followup_retry_attempts > 0 &&
         g_maint_followup_retry_attempts <= MAINT_FOLLOWUP_RETRY_MAX_ATTEMPTS;
}

static void maintenance_followup_retry_clear(const char* reason) {
  bool had_state = maintenance_followup_retry_pending() || g_maint_followup_retry_attempts > 0;
  if (had_state) {
    LOG_INFO("[MAINT_RETRY] cleared attempts=%u reason=%s request_id=%s",
             (unsigned)g_maint_followup_retry_attempts,
             reason ? reason : "unknown",
             g_maint_followup_retry_request_id[0] ? g_maint_followup_retry_request_id : "-");
  }
  g_maint_followup_retry_active = 0;
  g_maint_followup_retry_attempts = 0;
  g_maint_followup_retry_wake_epoch = 0;
  g_maint_followup_retry_reason[0] = '\0';
  g_maint_followup_retry_request_id[0] = '\0';
}


static bool maintenance_followup_retry_consume_wake() {
  if (!maintenance_followup_retry_pending()) {
    return false;
  }
  uint32_t now_epoch = maint_sync_epoch_now();
  bool due = (now_epoch == 0) ? true : (now_epoch + 2 >= g_maint_followup_retry_wake_epoch);
  if (!due) {
    return false;
  }
  uint32_t wake_epoch = g_maint_followup_retry_wake_epoch;
  g_maint_followup_retry_active = 0;
  g_maint_followup_retry_wake_epoch = 0;
  LOG_INFO("[MAINT_RETRY] wake attempt=%u wake_epoch=%lu now=%lu reason=%s request_id=%s",
           (unsigned)g_maint_followup_retry_attempts,
           (unsigned long)wake_epoch,
           (unsigned long)now_epoch,
           g_maint_followup_retry_reason[0] ? g_maint_followup_retry_reason : "-",
           g_maint_followup_retry_request_id[0] ? g_maint_followup_retry_request_id : "-");
  return true;
}

static bool maint_sync_request_matches(const char* expected, const char* actual) {
  if (!expected || !expected[0]) {
    return (!actual || !actual[0]);
  }
  if (!actual || !actual[0]) {
    return false;
  }
  return strcmp(expected, actual) == 0;
}

static void maint_sync_note_resolution(const char* resolution, const char* request_id) {
  maint_sync_copy_str(g_maint_sync_resolution, sizeof(g_maint_sync_resolution), resolution ? resolution : "unknown");
  g_maint_sync_resolution_epoch = maint_sync_epoch_now();
  maint_sync_copy_str(g_maint_sync_resolution_request_id,
                      sizeof(g_maint_sync_resolution_request_id),
                      request_id);
}


static void maint_sync_reset_ack_state() {
  g_lcd_maint_ack_received = false;
  g_lcd_maint_ack_ms = 0;
  g_lcd_maint_ack_remaining_s = 0;
  g_lcd_maint_ack_wake_in_s = 0;
  g_lcd_maint_ack_epoch = 0;
  g_lcd_maint_ack_request_id[0] = '\0';
  g_lcd_maint_ack_status[0] = '\0';
  g_lcd_maint_ack_persisted = 0;
  g_lcd_maint_ack_start_epoch = 0;
  g_lcd_maint_ack_duration_sec = 0;
  g_lcd_maint_ack_grace_before_sec = 0;
  g_lcd_maint_ack_grace_after_sec = 0;
}

static void maint_sync_note_tx(const char* request_id,
                               uint32_t remaining_s,
                               uint32_t wake_in_s,
                               bool clear_schedule,
                               bool link_recent) {
  g_maint_last_tx_epoch = maint_sync_epoch_now();
  maint_sync_copy_str(g_maint_last_tx_request_id, sizeof(g_maint_last_tx_request_id), request_id);
  g_maint_last_tx_remaining_s = remaining_s;
  g_maint_last_tx_wake_in_s = wake_in_s;
  g_maint_last_tx_clear = clear_schedule ? 1 : 0;
  g_maint_last_tx_link_recent = link_recent ? 1 : 0;
  g_maint_last_tx_send_count = g_maint_sync_send_count;
}

static void maintenance_window_consumed_clear(const char* reason) {
  bool had_value = (g_maint_consumed_valid != 0);
  g_maint_consumed_valid = 0;
  g_maint_consumed_start_epoch = 0;
  g_maint_consumed_end_epoch = 0;
  g_maint_consumed_request_id[0] = '\0';
  if (had_value) {
    LOG_INFO("[MAINT_GUARD] cleared reason=%s", reason ? reason : "unknown");
  }
}

static bool maintenance_window_is_consumed(const MaintenanceWindow& mw, uint64_t now_epoch) {
  if (!g_maint_consumed_valid) {
    return false;
  }
  if (g_maint_consumed_end_epoch > 0 && now_epoch > g_maint_consumed_end_epoch) {
    maintenance_window_consumed_clear("expired");
    return false;
  }
  if (mw.request_id[0] && g_maint_consumed_request_id[0]) {
    return strcmp(mw.request_id, g_maint_consumed_request_id) == 0;
  }
  return (mw.start_epoch == g_maint_consumed_start_epoch &&
          (mw.start_epoch + mw.duration_sec + mw.grace_after_sec) == g_maint_consumed_end_epoch);
}


static bool sense_action_inflight() {
  if (http_inflight || upload_inflight) {
    return true;
  }
  if (scan_ui_inflight) {
    return true;
  }
  if (upload_queue_count() > 0) {
    return true;
  }
  if (current_job.state != OP_IDLE && current_job.state != OP_DONE) {
    return true;
  }
  return false;
}

// pump_uart_rx_once() is defined once in Sense_Minimal/sense_uart.h (with the
// g_lcd_ota_proxy_owns_uart guard so it's safe to call from the main loop and
// from blocking waits). The previous unguarded copy here was removed to avoid a
// duplicate definition.

static const unsigned long OTA_PROOF_TIMEOUT_MS = 15000;

static void maybeRunOtaCheck(const char* reason, bool skip_boot_delay);
static bool is_time_valid();
void halo_prod_kick_time_sync(const char* reason);  // Start SNTP immediately on STA-connect (idempotent, non-blocking)
static void ota_sched_save();

// Legacy OTA orchestration (schedule fetch / arm / revalidate / windows /
// cooldowns / the 7-reason dispatcher). Set to 0 to run the nightly 02:00 path
// ALONE — which is the state the teardown produces, so building with it off is
// how the replacement gets tested before ~842 lines are deleted.
// DEFAULT 0 as of 2026-08-18: ship builds now run the nightly 02:00 path alone.
//
// Verified on hardware before flipping — with legacy off, a timer wake produced
// nightly_begin -> nightly_done and the schedule machinery went completely
// silent (0 ota_sched lines, vs 3 fetch/arm cycles in five minutes with it on).
// Build with =1 to restore the old orchestrator if the nightly path ever needs
// to be backed out in a hurry; the code is still here until it is deleted.
#ifndef HALO_LEGACY_OTA_ORCHESTRATOR
#define HALO_LEGACY_OTA_ORCHESTRATOR 0
#endif

static bool maintenance_window_load(MaintenanceWindow* mw);

static const char* reset_reason_to_str(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

static void log_ota_partition_info() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  esp_ota_img_states_t ota_state;
  const char* state_str = "UNKNOWN";
  if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    switch (ota_state) {
      case ESP_OTA_IMG_NEW: state_str = "NEW"; break;
      case ESP_OTA_IMG_PENDING_VERIFY: state_str = "PENDING_VERIFY"; break;
      case ESP_OTA_IMG_VALID: state_str = "VALID"; break;
      case ESP_OTA_IMG_INVALID: state_str = "INVALID"; break;
      case ESP_OTA_IMG_ABORTED: state_str = "ABORTED"; break;
      default: state_str = "OTHER"; break;
    }
  }

  const esp_partition_t* ota0 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* ota1 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);

  Serial.printf("[OTA_DIAG] running=%s addr=0x%06x size=%u state=%s\n",
                running ? running->label : "null",
                running ? running->address : 0,
                running ? running->size : 0,
                state_str);
  Serial.printf("[OTA_DIAG] boot=%s addr=0x%06x size=%u\n",
                boot ? boot->label : "null",
                boot ? boot->address : 0,
                boot ? boot->size : 0);
  Serial.printf("[OTA_DIAG] ota_0=%s addr=0x%06x size=%u ota_1=%s addr=0x%06x size=%u\n",
                ota0 ? ota0->label : "null",
                ota0 ? ota0->address : 0,
                ota0 ? ota0->size : 0,
                ota1 ? ota1->label : "null",
                ota1 ? ota1->address : 0,
                ota1 ? ota1->size : 0);

  uint32_t sketch_size = ESP.getSketchSize();
  uint32_t free_sketch = ESP.getFreeSketchSpace();
  esp_reset_reason_t reset_reason = esp_reset_reason();
  Serial.printf("[BUILD_DIAG] sketch_size=%u free_sketch=%u running_size=%u reset_reason=%s\n",
                sketch_size, free_sketch, running ? running->size : 0,
                reset_reason_to_str(reset_reason));
}

static void log_health_diag(const char* reason) {
  unsigned long uptime_ms = millis() - g_boot_time_ms;
  bool pending_verify = g_health_gate.getPendingVerify();
  bool rollback_eligible = g_health_gate.isRollbackEligible();
  bool marked_valid = g_health_gate.getMarkedValid();
  bool can_mark_valid = g_health_gate.canMarkValid();
  bool wifi_ok = wifi_is_connected();
  bool uart_ok = Truth::getUartSyncEstablished();
  const char* why = "eligible";
  char blocker[64] = "";

  if (!pending_verify) {
    why = "not_pending_verify";
  } else if (!can_mark_valid) {
    bool first = true;
    if (uptime_ms < 10000) {
      strcat(blocker, "uptime");
      first = false;
    }
    if (!wifi_ok) {
      if (!first) strcat(blocker, ",");
      strcat(blocker, "wifi");
      first = false;
    }
    if (!uart_ok) {
      if (!first) strcat(blocker, ",");
      strcat(blocker, "uart");
    }
    why = blocker[0] ? blocker : "blocked";
  }

#if OTA_TEST_BUILD
  if (g_health_gate.getSkipMarkValid()) {
    why = "skip_mark_valid";
  }
#endif

  Serial.printf("[HEALTH] reason=%s pending_verify=%d rollback_eligible=%d marked_valid=%d can_mark_valid=%d uptime_ms=%lu wifi=%d uart_sync=%d why=%s\n",
                reason ? reason : "unknown",
                pending_verify ? 1 : 0,
                rollback_eligible ? 1 : 0,
                marked_valid ? 1 : 0,
                can_mark_valid ? 1 : 0,
                uptime_ms,
                wifi_ok ? 1 : 0,
                uart_ok ? 1 : 0,
                why);
}

static void manual_ota_override_set(const char* reason) {
  g_manual_ota_override = true;
  g_manual_ota_override_until_ms = millis() + MANUAL_OTA_OVERRIDE_TTL_MS;
  g_manual_ota_request_ms = millis();
  g_manual_ota_wifi_retry_ms = 0;
  LOG_INFO("[OTA_MANUAL] override=1 reason=%s", reason ? reason : "unknown");
}

static void manual_ota_override_clear(const char* reason) {
  if (!g_manual_ota_override) {
    return;
  }
  g_manual_ota_override = false;
  g_manual_ota_override_until_ms = 0;
  g_manual_ota_request_ms = 0;
  g_manual_ota_wifi_retry_ms = 0;
  LOG_INFO("[OTA_MANUAL] override cleared reason=%s", reason ? reason : "unknown");
}

bool halo_ota_manual_override_active() {
  if (!g_manual_ota_override) {
    return false;
  }
  if (g_manual_ota_override_until_ms > 0 && millis() > g_manual_ota_override_until_ms) {
    LOG_INFO("[OTA_MANUAL] override expired");
    g_ota_check_requested = false;
    OtaIntent::clearForceAndCheck();
    manual_ota_override_clear("expired");
    return false;
  }
  return true;
}

#if OTA_TEST_BUILD
static void ota_test_save_skip_mark_valid(bool value) {
  Preferences prefs;
  if (!prefs.begin("ota_test", false)) {
    return;
  }
  prefs.putBool("skip_mark_valid", value);
  prefs.end();
}

static void ota_test_save_crash_after_boot(uint32_t value) {
  Preferences prefs;
  if (!prefs.begin("ota_test", false)) {
    return;
  }
  prefs.putUInt("crash_after_ms", value);
  prefs.end();
}

static void ota_test_load_prefs() {
  Preferences prefs;
  if (!prefs.begin("ota_test", true)) {
    return;
  }
  g_ota_test_skip_mark_valid = prefs.getBool("skip_mark_valid", false);
  g_ota_test_crash_after_boot_ms = prefs.getUInt("crash_after_ms", 0);
  prefs.end();
  g_health_gate.setSkipMarkValid(g_ota_test_skip_mark_valid);
  Serial.printf("[OTA_TEST] loaded skip_mark_valid=%d crash_after_boot_ms=%u\n",
                g_ota_test_skip_mark_valid ? 1 : 0,
                (unsigned int)g_ota_test_crash_after_boot_ms);
}

static void ota_test_process_command(const char* cmd) {
  if (!cmd || !cmd[0]) {
    return;
  }
  if (strcmp(cmd, "OTA_NOW") == 0) {
    if (!ota_peer_accept_new_request()) return;
    LOG_INFO("[OTA_TEST] OTA_NOW requested");
    uint32_t now_ts = (uint32_t)time(nullptr);
    OtaIntent::updateDesired(nullptr, nullptr, true, false, now_ts, "serial_cmd");
    g_ota_check_done = false;
    g_ota_skip_logged = false;
    maybeRunOtaCheck("serial_cmd", true);
    return;
  }
  if (strcmp(cmd, "OTA_DIAG") == 0) {
    log_ota_partition_info();
    return;
  }
  if (strcmp(cmd, "HEALTH") == 0) {
    log_health_diag("serial_cmd");
    return;
  }
  if (strncmp(cmd, "CRASH_AFTER_BOOT_MS=", 20) == 0) {
    uint32_t val = (uint32_t)strtoul(cmd + 20, nullptr, 10);
    g_ota_test_crash_after_boot_ms = val;
    ota_test_save_crash_after_boot(val);
    LOG_INFO("[OTA_TEST] CRASH_AFTER_BOOT_MS=%u", (unsigned int)val);
    return;
  }
  if (strncmp(cmd, "SKIP_MARK_VALID=", 16) == 0) {
    int val = atoi(cmd + 16);
    g_ota_test_skip_mark_valid = (val != 0);
    g_health_gate.setSkipMarkValid(g_ota_test_skip_mark_valid);
    ota_test_save_skip_mark_valid(g_ota_test_skip_mark_valid);
    LOG_INFO("[OTA_TEST] SKIP_MARK_VALID=%d", g_ota_test_skip_mark_valid ? 1 : 0);
    return;
  }
  if (strcmp(cmd, "OTA_TEST_STATUS") == 0) {
    Serial.printf("[OTA_TEST] status skip_mark_valid=%d crash_after_boot_ms=%u\n",
                  g_ota_test_skip_mark_valid ? 1 : 0,
                  (unsigned int)g_ota_test_crash_after_boot_ms);
    return;
  }
  LOG_INFO("[OTA_TEST] unknown_cmd=%s", cmd);
}

static void ota_test_handle_serial() {
  static char buf[96];
  static size_t len = 0;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (len > 0) {
        buf[len] = '\0';
        ota_test_process_command(buf);
        len = 0;
      }
      continue;
    }
    if (len < (sizeof(buf) - 1)) {
      buf[len++] = c;
    }
  }
}
#endif


static bool ota_disabled_for_local_dev() {
#if (HALO_DEV_NO_OTA || LOCAL_DEV_BUILD)
  return !ota_override_enabled();
#else
  return false;
#endif
}

static int compare_semver_part(const char*& p) {
  while (*p == '.') {
    p++;
  }
  if (*p == '\0') {
    return 0;
  }
  char* end = nullptr;
  long val = strtol(p, &end, 10);
  if (end == p) {
    while (*p && *p != '.') {
      p++;
    }
    if (*p == '.') {
      p++;
    }
    return 0;
  }
  p = end;
  while (*p && *p != '.') {
    p++;
  }
  if (*p == '.') {
    p++;
  }
  if (val < 0) {
    val = 0;
  }
  return (int)val;
}

static int compareSemver(const char* a, const char* b) {
  if (a == nullptr) {
    a = "";
  }
  if (b == nullptr) {
    b = "";
  }
  const char* pa = a;
  const char* pb = b;
  for (int i = 0; i < 3; ++i) {
    int va = compare_semver_part(pa);
    int vb = compare_semver_part(pb);
    if (va < vb) {
      return -1;
    }
    if (va > vb) {
      return 1;
    }
  }
  return 0;
}

struct OtaScheduleConfig {
  bool enabled;
  uint16_t window_start_min;
  uint16_t window_dur_min;
  uint8_t jitter_min;
  uint8_t min_idle_min;
};

static Preferences g_ota_sched_prefs;
static const char* OTA_SCHED_NS = "ota_sched";
static const char* OTA_SCHED_ENABLE_KEY = "enable";
static const char* OTA_SCHED_START_KEY = "start_min";
static const char* OTA_SCHED_WINDOW_KEY = "window_min";
static const char* OTA_SCHED_DUR_KEY = "dur_min";
static const char* OTA_SCHED_JITTER_KEY = "jitter_min";
static const char* OTA_SCHED_IDLE_KEY = "idle_min";
static const char* OTA_SCHED_NEXT_KEY = "next_epoch";
static const char* OTA_SCHED_SYNC_KEY = "last_sync";

#ifndef OTA_SCHED_ENABLED
#define OTA_SCHED_ENABLED 1
#endif
#ifndef OTA_SCHED_TEST_OFFSET_SEC
#define OTA_SCHED_TEST_OFFSET_SEC 0
#endif

static OtaScheduleConfig g_ota_sched = {OTA_SCHED_ENABLED, 120, 30, 10, 10};
static uint32_t g_next_ota_epoch = 0;
static uint32_t g_last_time_sync_epoch = 0;
static esp_sleep_wakeup_cause_t g_last_wake_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static bool g_maintenance_mode = false;
static bool g_maintenance_handled = false;
static bool g_maintenance_in_window = false;
static uint32_t g_sleep_timer_delta_s = 0;
static uint32_t g_last_ota_check_epoch = 0;
static char g_last_ota_result[32] = "none";
static unsigned long g_time_retry_last_ms = 0;

static const uint32_t OTA_TIME_RETRY_SEC = 900;  // 15 minutes
static const unsigned long OTA_TIME_RETRY_COOLDOWN_MS = 900000UL;

#ifndef OTA_SCHED_SELF_TEST
#define OTA_SCHED_SELF_TEST 0
#endif
#ifndef OTA_SCHED_DEBUG_ON_BOOT
#define OTA_SCHED_DEBUG_ON_BOOT 0
#endif

static uint32_t ota_sched_device_hash() {
  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  char owner_id[64] = {0};
  bool owner_ok = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  const char* seed = (owner_ok && owner_id[0]) ? owner_id : device_id;
  uint32_t hash = 2166136261u;
  if (!seed || !seed[0]) {
    uint64_t mac = ESP.getEfuseMac();
    char mac_buf[24];
    snprintf(mac_buf, sizeof(mac_buf), "%08lx%08lx",
             (unsigned long)(mac >> 32), (unsigned long)(mac & 0xffffffffUL));
    seed = mac_buf;
  }
  for (const char* p = seed; *p; ++p) {
    hash ^= (uint8_t)(*p);
    hash *= 16777619u;
  }
  return hash;
}

static uint32_t ota_rollout_bucket(uint32_t seed) {
  uint32_t hash = ota_sched_device_hash();
  hash ^= seed;
  return (hash % 100);
}

static uint16_t ota_sched_jitter_max_min() {
  uint16_t window_min = g_ota_sched.window_dur_min;
  uint16_t max_jitter = window_min / 4;
  if (max_jitter > 10) {
    max_jitter = 10;
  }
  if (g_ota_sched.jitter_min > 0 && g_ota_sched.jitter_min < max_jitter) {
    max_jitter = g_ota_sched.jitter_min;
  }
  return max_jitter;
}

static uint16_t ota_sched_device_jitter_min() {
  uint16_t max_jitter = ota_sched_jitter_max_min();
  if (max_jitter == 0) {
    return 0;
  }
  uint32_t h = ota_sched_device_hash();
  return (uint16_t)(h % (max_jitter + 1));
}

static void ota_set_last_result(const char* result) {
  if (!result) {
    return;
  }
  strncpy(g_last_ota_result, result, sizeof(g_last_ota_result) - 1);
  g_last_ota_result[sizeof(g_last_ota_result) - 1] = '\0';
}

static void ota_mark_check_start() {
  if (is_time_valid()) {
    g_last_ota_check_epoch = (uint32_t)time(nullptr);
  } else {
    g_last_ota_check_epoch = 0;
  }
}

static bool ota_sched_schedule_time_retry(const char* reason) {
  unsigned long now_ms = millis();
  if (g_time_retry_last_ms > 0 && (now_ms - g_time_retry_last_ms) < OTA_TIME_RETRY_COOLDOWN_MS) {
    return false;
  }
  g_time_retry_last_ms = now_ms;
  g_sleep_timer_delta_s = OTA_TIME_RETRY_SEC;
  esp_sleep_enable_timer_wakeup((uint64_t)OTA_TIME_RETRY_SEC * 1000000ULL);
  LOG_INFO("[OTA_SCHED] time_invalid_retry reason=%s retry_s=%lu",
           reason ? reason : "unknown",
           (unsigned long)OTA_TIME_RETRY_SEC);
  return true;
}

static void ota_sched_print_status(const char* reason) {
  char next_buf[32] = "-";
  if (g_next_ota_epoch > 0) {
    time_t next_t = (time_t)g_next_ota_epoch;
    tm utc = {};
    gmtime_r(&next_t, &utc);
    snprintf(next_buf, sizeof(next_buf), "%04d-%02d-%02d %02d:%02d:%02dZ",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec);
  }
  Serial.printf("[OTA_SCHED_STATUS] reason=%s enabled=%d start_min=%u window_min=%u next_epoch=%lu next_utc=%s last_check_epoch=%lu last_result=%s\n",
                reason ? reason : "-",
                g_ota_sched.enabled ? 1 : 0,
                (unsigned)g_ota_sched.window_start_min,
                (unsigned)g_ota_sched.window_dur_min,
                (unsigned long)g_next_ota_epoch,
                next_buf,
                (unsigned long)g_last_ota_check_epoch,
                g_last_ota_result);
}


static bool ota_sched_http_configured() {
  return OTA_SCHED_HTTP_URL[0] != '\0';
}




static const char* ota_report_reset_reason_str(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

static const char* ota_report_wake_cause_str(int cause) {
  switch (static_cast<esp_sleep_wakeup_cause_t>(cause)) {
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP: return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO: return "GPIO";
    case ESP_SLEEP_WAKEUP_UART: return "UART";
    case ESP_SLEEP_WAKEUP_WIFI: return "WIFI";
    case ESP_SLEEP_WAKEUP_COCPU: return "COCPU";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG: return "COCPU_TRAP";
    case ESP_SLEEP_WAKEUP_BT: return "BT";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
      return "UNDEFINED";
  }
}

static bool ota_report_http_is_https(const char* url) {
  return url && strncmp(url, "https://", 8) == 0;
}

static void ota_report_add_optional_str(JsonObject obj, const char* key, const char* value) {
  if (key && value && value[0]) {
    obj[key] = value;
  }
}

// Builds the rich device-state report payload. Identical to the historical
// pre_sleep payload except report_type is parametrized and an optional small
// set of extra string fields can be appended (used by window_armed /
// ota_complete). Pass report_type == nullptr to default to "pre_sleep".
static bool ota_report_build_payload(String& out,
                                     const char* report_type,
                                     const OtaReportExtras* extras) {
  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  if (!device_id[0]) {
    LOG_WARN("[OTA_REPORT] missing device_id");
    return false;
  }

  time_t now = time(nullptr);
  if (now <= 1700000000) {
    LOG_WARN("[OTA_REPORT] time invalid");
    return false;
  }

  char owner_id[64] = {0};
  bool owner_ok = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  MaintenanceWindow mw;
  bool has_mw = maintenance_window_load(&mw);
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  TruthManifestState& manifest_state = truth_get_manifest_state();

  DynamicJsonDocument doc(4096);
  JsonObject payload = doc.to<JsonObject>();
  payload["device_id"] = device_id;
  payload["device_type"] = "sense";
  payload["board"] = HALO_BOARD_NAME;
  payload["channel"] = OTA_CHANNEL;
  payload["fw"] = kFirmwareVersion;
  payload["build"] = kBuildId;
  payload["report_type"] = (report_type && report_type[0]) ? report_type : "pre_sleep";
  payload["ts_epoch"] = static_cast<uint32_t>(now);
  if (owner_ok && owner_id[0]) {
    payload["owner_id"] = owner_id;
  }
  if (has_mw && mw.request_id[0]) {
    payload["request_id"] = mw.request_id;
  }

  payload["boot_count"] = truth_get_boot_count();
  payload["wake_cause"] = truth_get_wake_cause();
  payload["wake_cause_str"] = ota_report_wake_cause_str(truth_get_wake_cause());
  payload["reset_reason"] = ota_report_reset_reason_str(esp_reset_reason());
  payload["uptime_ms"] = millis();
  payload["maintenance_mode"] = truth_get_maintenance_mode() ? 1 : 0;
  payload["maintenance_in_window"] = truth_get_maintenance_in_window() ? 1 : 0;
  payload["maint_scheduled"] = truth_get_ota_sched_enable() ? 1 : 0;
  payload["maint_min_idle_min"] = static_cast<uint32_t>(g_ota_sched.min_idle_min);
  payload["maint_sync_pending"] = truth_get_maint_sync_pending() ? 1 : 0;
  payload["maint_sync_attempts"] = truth_get_maint_sync_attempts();
  payload["lcd_maint_ack"] = truth_get_lcd_maint_ack() ? 1 : 0;
  payload["sched_fetch_http"] = truth_get_sched_fetch_http_code();
  payload["sched_fetch_age_s"] = truth_get_sched_fetch_age_s();
  payload["sched_last_event_age_s"] = truth_get_sched_last_event_age_s();
  payload["lcd_fw_age_s"] = truth_get_lcd_fw_age_s();
  payload["lcd_maint_ack_age_s"] = truth_get_lcd_maint_ack_age_s();
  payload["lcd_maint_ack_remaining_s"] = truth_get_lcd_maint_ack_remaining_s();
  payload["lcd_maint_ack_wake_in_s"] = truth_get_lcd_maint_ack_wake_in_s();
  payload["lcd_maint_ack_persisted"] = truth_get_lcd_maint_ack_persisted() ? 1 : 0;
  payload["maint_last_tx_age_s"] = truth_get_maint_last_tx_age_s();
  payload["maint_last_tx_remaining_s"] = truth_get_maint_last_tx_remaining_s();
  payload["maint_last_tx_wake_in_s"] = truth_get_maint_last_tx_wake_in_s();
  payload["maint_last_tx_clear"] = truth_get_maint_last_tx_clear() ? 1 : 0;
  payload["maint_last_tx_link_recent"] = truth_get_maint_last_tx_link_recent() ? 1 : 0;
  payload["maint_followup_retry_active"] = g_maint_followup_retry_active ? 1 : 0;
  payload["maint_followup_retry_attempts"] = g_maint_followup_retry_attempts;
  payload["maint_last_idle_ms"] = g_maint_last_idle_ms;
  payload["maint_last_idle_gate_hit"] = g_maint_last_idle_gate_hit ? 1 : 0;
  payload["maint_last_idle_gate_bypassed"] = g_maint_last_idle_gate_bypassed ? 1 : 0;
  payload["maint_last_idle_http_window"] = g_maint_last_idle_http_window ? 1 : 0;
  payload["lcd_ota_request_active"] = g_lcd_ota_request_active ? 1 : 0;
  payload["lcd_ota_done"] = g_lcd_ota_done ? 1 : 0;
  payload["lcd_ota_window_remaining_s"] = g_lcd_ota_window_remaining_s;
  payload["reboot_loop_detected"] = truth_get_reboot_loop_detected() ? 1 : 0;
  payload["uart_tx_count"] = truth_get_uart_tx_count();
  payload["uart_rx_count"] = truth_get_uart_rx_count();
  payload["last_action_age_ms"] = truth_get_last_action_age_ms();
  payload["lcd_diag_age_ms"] = truth_get_lcd_diag_age_ms();

  unsigned long last_activity_ms = sense_get_last_user_activity_ms();
  if (last_activity_ms > 0) {
    unsigned long idle_ms = millis() - last_activity_ms;
    payload["user_idle_ms"] = static_cast<uint32_t>(idle_ms);
  }

  ota_report_add_optional_str(payload, "sched_fetch", truth_get_sched_fetch_state());
  ota_report_add_optional_str(payload, "sched_fetch_request_id", truth_get_sched_fetch_request_id());
  ota_report_add_optional_str(payload, "sched_last_event", truth_get_sched_last_event());
  ota_report_add_optional_str(payload, "sched_last_event_request_id", truth_get_sched_last_event_request_id());
  ota_report_add_optional_str(payload, "maint_sync_resolution", truth_get_maint_sync_resolution());
  ota_report_add_optional_str(payload, "maint_last_idle_gate_reason", g_maint_last_idle_gate_reason);
  ota_report_add_optional_str(payload, "maint_last_tx_request_id", truth_get_maint_last_tx_request_id());
  // Always send lcd_fw — even if empty, so backend knows field exists
  {
    const char* lcd_v = truth_get_lcd_fw_version();
    payload["lcd_fw"] = (lcd_v && lcd_v[0]) ? lcd_v : "unknown";
  }
  ota_report_add_optional_str(payload, "lcd_ota_result", truth_get_lcd_ota_result());
  ota_report_add_optional_str(payload, "lcd_maint_ack_request_id", truth_get_lcd_maint_ack_request_id());
  ota_report_add_optional_str(payload, "lcd_maint_ack_status", truth_get_lcd_maint_ack_status());
  ota_report_add_optional_str(payload, "last_uart_tx_type", truth_get_last_uart_tx_type());
  ota_report_add_optional_str(payload, "last_uart_rx_type", truth_get_last_uart_rx_type());
  ota_report_add_optional_str(payload, "last_action", truth_get_last_action());
  ota_report_add_optional_str(payload, "lcd_diag_last_tx", truth_get_lcd_diag_last_tx());
  ota_report_add_optional_str(payload, "lcd_diag_last_rx", truth_get_lcd_diag_last_rx());
  ota_report_add_optional_str(payload, "ota_result", g_last_ota_result);
  ota_report_add_optional_str(payload, "maint_followup_retry_reason", g_maint_followup_retry_reason);
  ota_report_add_optional_str(payload, "maint_followup_retry_request_id", g_maint_followup_retry_request_id);

  if (truth_get_next_ota_epoch() > 0) {
    payload["maint_wake_epoch"] = truth_get_next_ota_epoch();
  }
  if (g_maint_followup_retry_wake_epoch > 0) {
    payload["maint_followup_retry_wake_epoch"] = static_cast<uint64_t>(g_maint_followup_retry_wake_epoch);
  }
  if (has_mw) {
    payload["maint_start_epoch"] = static_cast<uint64_t>(mw.start_epoch);
    payload["maint_end_epoch"] = static_cast<uint64_t>(mw.start_epoch + mw.duration_sec + mw.grace_after_sec);
    if (mw.start_epoch > mw.grace_before_sec) {
      payload["maint_schedule_wake_epoch"] = static_cast<uint64_t>(mw.start_epoch - mw.grace_before_sec);
    }
  }
  if (running && running->label) {
    payload["part"] = running->label;
  }
  if (boot && boot->label) {
    payload["boot"] = boot->label;
  }
  if (wifi_is_connected()) {
    payload["wifi"] = "connected";
    payload["ip"] = WiFi.localIP().toString();
    payload["rssi"] = WiFi.RSSI();
  } else {
    payload["wifi"] = "disconnected";
  }
  if (manifest_state.status == TruthManifestState::OK && manifest_state.version[0]) {
    payload["manifest_version"] = manifest_state.version;
  }

  // Caller-supplied extras (e.g. sense_fw_target on ota_complete). Added last
  // so they can override/augment without touching the common builder body.
  if (extras) {
    for (uint8_t i = 0; i < extras->count; ++i) {
      ota_report_add_optional_str(payload, extras->keys[i], extras->vals[i]);
    }
  }

  out = "";
  serializeJson(doc, out);
  if (out.length() == 0) {
    LOG_WARN("[OTA_REPORT] serialize failed");
    return false;
  }
  return true;
}

// Generic device-state report POST. Builds the SAME rich payload as the
// historical pre_sleep report but with an arbitrary report_type and an optional
// small set of extra string fields. Non-blocking-ish: guards on wifi and uses
// the existing bounded-timeout HTTP pattern, so it skips gracefully (no hang)
// when WiFi is down. report_type == nullptr => "pre_sleep".
static bool ota_report_post(const char* report_type,
                            const OtaReportExtras* extras,
                            uint32_t timeout_ms) {
  HaloNtpDnsGuard ntp_dns_guard;
  const char* rt = (report_type && report_type[0]) ? report_type : "pre_sleep";
  if (!OTA_REPORT_HTTP_URL[0]) {
    return false;
  }
  if (!wifi_is_connected()) {
    LOG_INFO("[OTA_REPORT] skip %s (wifi_down)", rt);
    return false;
  }
  String body;
  if (!ota_report_build_payload(body, rt, extras)) {
    return false;
  }

  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  char owner_id[64] = {0};
  bool owner_ok = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  MaintenanceWindow mw;
  bool has_mw = maintenance_window_load(&mw);

  // Free the camera DMA reserve for the duration of the HTTPS handshake/POST and
  // auto-restore on every return path. This report can fire during a maintenance
  // window with the 16KB camera reserve held, which otherwise starves the TLS
  // esp-aes DMA alloc and panics. Only armed on the HTTPS branch.
  const bool report_is_https = ota_report_http_is_https(OTA_REPORT_HTTP_URL);
  ScopedTlsDmaReserve tls_dma_guard(report_is_https);

  HTTPClient http;
  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  bool started = false;
  if (report_is_https) {
#if OTA_REPORT_HTTP_INSECURE
    secure_client.setInsecure();
#else
    ota_http_configure_tls(secure_client, "OTA_REPORT", OTA_REPORT_HTTP_ROOT_CA);
#endif
    secure_client.setHandshakeTimeout(15);
    secure_client.setTimeout(timeout_ms);
    started = http.begin(secure_client, OTA_REPORT_HTTP_URL);
  } else {
    plain_client.setTimeout(timeout_ms);
    started = http.begin(plain_client, OTA_REPORT_HTTP_URL);
  }
  if (!started) {
    LOG_WARN("[OTA_REPORT] begin failed url=%s", OTA_REPORT_HTTP_URL);
    return false;
  }

  http.setConnectTimeout(static_cast<int>(timeout_ms));
  http.setTimeout(static_cast<int>(timeout_ms));
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");
  if (device_id[0]) {
    http.addHeader("X-Device-Id", device_id);
  }
  if (owner_ok && owner_id[0]) {
    http.addHeader("X-Owner-Id", owner_id);
  }
  if (has_mw && mw.request_id[0]) {
    http.addHeader("X-Request-Id", mw.request_id);
  }

  int http_code = http.POST(body);
  String response;
  if (http_code > 0) {
    response = http.getString();
  }
  http.end();

  bool ok = (http_code == HTTP_CODE_OK || http_code == HTTP_CODE_ACCEPTED);
  if (ok) {
    LOG_INFO("[OTA_REPORT] %s ok code=%d body_len=%u", rt, http_code, static_cast<unsigned>(body.length()));
    return true;
  }

  String response_preview = response.length() ? response.substring(0, 160) : String("-");
  LOG_WARN("[OTA_REPORT] %s failed code=%d resp=%s",
           rt,
           http_code,
           response_preview.c_str());
  return false;
}

// Thin wrapper: preserves the historical pre_sleep call site / behavior.
static bool ota_report_post_pre_sleep(uint32_t timeout_ms) {
  return ota_report_post("pre_sleep", nullptr, timeout_ms);
}




static void ota_sched_self_test() {
#if OTA_SCHED_SELF_TEST
  uint16_t max_jitter = ota_sched_jitter_max_min();
  uint16_t jitter = ota_sched_device_jitter_min();
  if (jitter > max_jitter) {
    Serial.printf("[OTA_SCHED_TEST] FAIL jitter=%u max=%u\n", jitter, max_jitter);
  } else {
    Serial.printf("[OTA_SCHED_TEST] OK jitter=%u max=%u\n", jitter, max_jitter);
  }
#endif
}

// ----------------------------------------------------------------------------
// OTA URL resolution + allowlist (Truth.cpp depends on containsDisallowedHost)
// ----------------------------------------------------------------------------
struct OtaUrlConfig {
  char base_dir[256];
  char manifest_url[512];
  char channel[32];
  char env[8];
  char host[96];
  bool channel_enabled;
  bool allowed_host;
};

static bool extractHostFromUrl(const char* url, char* dst, size_t dst_len) {
  if (!url || !dst || dst_len == 0) return false;
  const char* start = strstr(url, "://");
  if (!start) return false;
  start += 3;
  const char* end = strchr(start, '/');
  if (!end) end = start + strlen(start);
  size_t n = (size_t)(end - start);
  if (n >= dst_len) n = dst_len - 1;
  memcpy(dst, start, n);
  dst[n] = '\0';
  return n > 0;
}

static const char* const OTA_ALLOWED_HOSTS[] = {
  "halo-ota-dev.s3.us-east-1.amazonaws.com",
  "halo-ota-prod.s3.us-east-1.amazonaws.com",
  nullptr
};

static bool isAllowedOtaHost(const char* url) {
  if (!url || strlen(url) == 0) return false;
  const char* host_start = strstr(url, "://");
  if (!host_start) return false;
  host_start += 3;
  const char* host_end = strchr(host_start, '/');
  if (!host_end) host_end = host_start + strlen(host_start);
  size_t host_len = (size_t)(host_end - host_start);
  char host_buf[96];
  if (host_len >= sizeof(host_buf)) return false;
  memcpy(host_buf, host_start, host_len);
  host_buf[host_len] = '\0';
  for (const char* const* p = OTA_ALLOWED_HOSTS; *p; p++) {
    if (strcasecmp(host_buf, *p) == 0) return true;
  }
  return false;
}

bool containsDisallowedHost(const char* url) {
  if (!url || strlen(url) == 0) return false;
  const char* p = url;
  while (*p) {
    if ((p[0]=='g'||p[0]=='G')&&(p[1]=='i'||p[1]=='I')&&(p[2]=='t'||p[2]=='T')&&(p[3]=='h'||p[3]=='H')&&(p[4]=='u'||p[4]=='U')&&(p[5]=='b'||p[5]=='B')&&p[6]=='.'&&(p[7]=='i'||p[7]=='I')&&(p[8]=='o'||p[8]=='O')&&(p[9]=='\0'||p[9]=='/'||p[9]==':'||p[9]=='?'||p[9]=='&'||p[9]=='#')) return true;
    if ((p[0]=='g'||p[0]=='G')&&(p[1]=='i'||p[1]=='I')&&(p[2]=='t'||p[2]=='T')&&(p[3]=='h'||p[3]=='H')&&(p[4]=='u'||p[4]=='U')&&(p[5]=='b'||p[5]=='B')&&(p[6]=='u'||p[6]=='U')&&(p[7]=='s'||p[7]=='S')&&(p[8]=='e'||p[8]=='E')&&(p[9]=='r'||p[9]=='R')&&(p[10]=='c'||p[10]=='C')&&(p[11]=='o'||p[11]=='O')&&(p[12]=='n'||p[12]=='N')&&(p[13]=='t'||p[13]=='T')&&(p[14]=='e'||p[14]=='E')&&(p[15]=='n'||p[15]=='N')&&(p[16]=='t'||p[16]=='T')&&p[17]=='.'&&(p[18]=='c'||p[18]=='C')&&(p[19]=='o'||p[19]=='O')&&(p[20]=='m'||p[20]=='M')&&(p[21]=='\0'||p[21]=='/'||p[21]==':'||p[21]=='?'||p[21]=='&'||p[21]=='#')) return true;
    p++;
  }
  return false;
}

void resolveOtaManifestUrl(OtaUrlConfig* config) {
  memset(config, 0, sizeof(OtaUrlConfig));
  const char* source = "unknown";
#if defined(OTA_CHANNEL_ENABLED)
  {
    const char* bucket = OTA_S3_BUCKET;
    const char* region = OTA_S3_REGION;
    const char* prefix = OTA_S3_PREFIX;
    const char* ch = OTA_CHANNEL;
    if (bucket && strlen(bucket) > 0 && region && strlen(region) > 0) {
      const char* p = (prefix && strlen(prefix) > 0) ? prefix : "halo/ota";
      const char* c = (ch && strlen(ch) > 0) ? ch : "dev";
      snprintf(config->base_dir, sizeof(config->base_dir),
               "https://%s.s3.%s.amazonaws.com/%s", bucket, region, p);
      config->base_dir[sizeof(config->base_dir) - 1] = '\0';
      size_t bl = strlen(config->base_dir);
      if (bl > 0 && config->base_dir[bl - 1] == '/') {
        config->base_dir[bl - 1] = '\0';
      }
      snprintf(config->manifest_url, sizeof(config->manifest_url), "%s/%s/manifest_latest.json",
               config->base_dir, c);
      config->manifest_url[sizeof(config->manifest_url) - 1] = '\0';
      strncpy(config->channel, c, sizeof(config->channel) - 1);
      config->channel[sizeof(config->channel) - 1] = '\0';
      config->channel_enabled = true;
      strncpy(config->env, (strcmp(c, "prod") == 0) ? "prod" : "dev", sizeof(config->env) - 1);
      config->env[sizeof(config->env) - 1] = '\0';
      config->allowed_host = isAllowedOtaHost(config->manifest_url);
      extractHostFromUrl(config->manifest_url, config->host, sizeof(config->host));
      source = "channel_mode";
      g_ota_config_source = source;
      if (containsDisallowedHost(config->manifest_url) || !config->allowed_host) {
        memset(config->manifest_url, 0, sizeof(config->manifest_url));
        config->allowed_host = false;
      }
      Serial.printf("[OTA_CFG] resolved_manifest_url=%s source=%s\n",
                    config->manifest_url[0] ? config->manifest_url : "(ABORTED)",
                    source);
      return;
    }
  }
#endif
  {
    const char* env = OTA_DEFAULT_ENV;
    const char* bucket = (env && strcmp(env, "prod") == 0) ? OTA_PROD_BUCKET : OTA_DEV_BUCKET;
    const char* region = (env && strcmp(env, "prod") == 0) ? OTA_PROD_REGION : OTA_DEV_REGION;
    const char* prefix = (env && strcmp(env, "prod") == 0) ? OTA_PROD_PREFIX : OTA_DEV_PREFIX;
    const char* ch = (env && strcmp(env, "prod") == 0) ? "prod" : "dev";
    strncpy(config->env, (env && strcmp(env, "prod") == 0) ? "prod" : "dev", sizeof(config->env) - 1);
    config->env[sizeof(config->env) - 1] = '\0';
    snprintf(config->base_dir, sizeof(config->base_dir),
             "https://%s.s3.%s.amazonaws.com/%s", bucket, region, prefix);
    config->base_dir[sizeof(config->base_dir) - 1] = '\0';
    snprintf(config->manifest_url, sizeof(config->manifest_url), "%s/%s/manifest_latest.json",
             config->base_dir, ch);
    config->manifest_url[sizeof(config->manifest_url) - 1] = '\0';
    strncpy(config->channel, ch, sizeof(config->channel) - 1);
    config->channel[sizeof(config->channel) - 1] = '\0';
    config->channel_enabled = true;
    config->allowed_host = isAllowedOtaHost(config->manifest_url);
    extractHostFromUrl(config->manifest_url, config->host, sizeof(config->host));
    source = "default_env";
    g_ota_config_source = source;
    if (containsDisallowedHost(config->manifest_url) || !config->allowed_host) {
      memset(config->manifest_url, 0, sizeof(config->manifest_url));
      config->allowed_host = false;
    }
    Serial.printf("[OTA_CFG] resolved_manifest_url=%s source=%s\n",
                  config->manifest_url[0] ? config->manifest_url : "(ABORTED)",
                  source);
  }
}

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static void log_mqtt_prefix() {
  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  char owner_id[64] = {0};
  bool owner_ok = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  char prefix[160];
  if (MQTT_USE_OWNER_PREFIX && owner_ok && owner_id[0]) {
    snprintf(prefix, sizeof(prefix), "%s/%s/%s", MQTT_TOPIC_PREFIX, owner_id, device_id);
  } else {
    snprintf(prefix, sizeof(prefix), "%s/%s", MQTT_TOPIC_PREFIX, device_id);
  }
  Serial.printf("[MQTT] prefix=%s\n", prefix);
}

static void maybeRunOtaCheck(const char* reason, bool skip_boot_delay);
void maybeRunOtaCheck(const char* reason);
static void handle_mqtt_commands();

bool halo_provisioning_active() {
  return g_provisioning_manager.isSetupModeActive();
}

bool halo_get_provisioned_wifi(char* ssid, size_t ssid_sz, char* pass, size_t pass_sz) {
  if (!ssid || !pass) {
    return false;
  }
  return ProvisioningState::loadHomeWifiCreds(ssid, ssid_sz, pass, pass_sz);
}

void halo_prod_on_lcd_message(const char* type) {
  (void)type;
  g_lcd_alive = true;
}

void halo_prod_on_lcd_wifi_creds_ack(const char* status, int err_code) {
  g_lcd_got_creds_ack = true;
  LOG_INFO("[UART] WIFI_CREDS_ACK received from LCD status=%s err_code=%d",
           status ? status : "OK",
           err_code);
}

void halo_prod_on_lcd_wifi_on_ack() {
  g_lcd_got_wifion_ack = true;
  LOG_INFO("[UART] WIFI_ON_ACK received from LCD");
}

void halo_prod_on_lcd_maint_ack(uint32_t remaining_s,
                                uint32_t wake_in_s,
                                bool clear,
                                const char* request_id,
                                const char* status,
                                bool persisted,
                                uint64_t start_epoch,
                                uint32_t duration_sec,
                                uint32_t grace_before_sec,
                                uint32_t grace_after_sec) {
  g_lcd_maint_ack_received = true;
  g_lcd_maint_ack_ms = millis();
  g_lcd_maint_ack_epoch = maint_sync_epoch_now();
  g_lcd_maint_ack_remaining_s = remaining_s;
  g_lcd_maint_ack_wake_in_s = wake_in_s;
  maint_sync_copy_str(g_lcd_maint_ack_request_id, sizeof(g_lcd_maint_ack_request_id), request_id);
  maint_sync_copy_str(g_lcd_maint_ack_status,
                      sizeof(g_lcd_maint_ack_status),
                      (status && status[0]) ? status : (clear ? "cleared" : "stored"));
  g_lcd_maint_ack_persisted = persisted ? 1 : 0;
  g_lcd_maint_ack_start_epoch = start_epoch;
  g_lcd_maint_ack_duration_sec = duration_sec;
  g_lcd_maint_ack_grace_before_sec = grace_before_sec;
  g_lcd_maint_ack_grace_after_sec = grace_after_sec;
  g_maint_sync_no_ack_logged = false;
  g_maint_sync_wait_until_ms = 0;
  MaintenanceWindow mw;
  bool has_window = maintenance_window_load(&mw);
  const char* effective_request_id = (request_id && request_id[0]) ? request_id
                                                                    : (has_window && mw.request_id[0] ? mw.request_id : nullptr);
  bool ack_matches = true;
  if (has_window && mw.request_id[0]) {
    ack_matches = maint_sync_request_matches(mw.request_id, effective_request_id);
  } else if (g_maint_pending_request_id[0]) {
    ack_matches = maint_sync_request_matches(g_maint_pending_request_id, effective_request_id);
  }
  sched_event_note(clear ? "lcd_clear_ack" : "lcd_ack", effective_request_id);
  if (ack_matches) {
    maint_sync_note_resolution("acked", effective_request_id);
    if (maintenance_schedule_pending_sync_to_lcd()) {
      set_maintenance_schedule_pending_sync_to_lcd(false);
    }
    g_maint_pending_last = false;
    g_maint_pending_request_id[0] = '\0';
  } else {
    maint_sync_note_resolution("ack_mismatch", effective_request_id);
  }
  LOG_INFO("[UART] MAINT_WINDOW_ACK remaining_s=%lu wake_in_s=%lu clear=%d request_id=%s status=%s persisted=%d match=%d",
           (unsigned long)remaining_s,
           (unsigned long)wake_in_s,
           clear ? 1 : 0,
           effective_request_id ? effective_request_id : "-",
           g_lcd_maint_ack_status[0] ? g_lcd_maint_ack_status : "-",
           persisted ? 1 : 0,
           ack_matches ? 1 : 0);
}

bool ota_test_bypass_reboot_guard_enabled() {
#if OTA_TEST_BYPASS_REBOOT_LOOP_GUARD
  return true;
#else
  return false;
#endif
}

static bool should_block_sleep_for_ota(bool* apply_active,
                                       bool* manifest_inflight,
                                       bool* scheduled_pending) {
  bool apply_flag = g_ota_apply_in_progress || (g_ota_simple_proof_started && !g_ota_simple_proof_done);
  bool manifest_flag = g_ota_check_in_progress;
  bool scheduled_flag = (OtaIntent::getOtaIntentActive() && !g_ota_check_done) ||
                        g_ota_check_requested ||
                        mqtt_ota_check_requested || halo_prod_boot_ota_pending();
  if (apply_active) {
    *apply_active = apply_flag;
  }
  if (manifest_inflight) {
    *manifest_inflight = manifest_flag;
  }
  if (scheduled_pending) {
    *scheduled_pending = scheduled_flag;
  }
  return (apply_flag || manifest_flag || scheduled_flag);
}

static bool should_block_sleep_for_mqtt(bool* publish_inflight, bool* queued_cmd) {
  MqttMetrics metrics = {};
  mqtt_get_metrics(&metrics);
  bool pub_flag = metrics.pub_inflight > 0;
  bool queued_flag = metrics.cmd_queue_depth > 0;
  if (publish_inflight) {
    *publish_inflight = pub_flag;
  }
  if (queued_cmd) {
    *queued_cmd = queued_flag;
  }
  return (pub_flag || queued_flag);
}

bool halo_prod_should_defer_sleep_ack(bool* ota_busy, bool* mqtt_busy, bool* time_invalid, bool* ota_check_busy) {
  bool apply_active = false;
  bool manifest_inflight = false;
  bool scheduled_pending = false;
  bool ota_busy_flag = should_block_sleep_for_ota(&apply_active, &manifest_inflight, &scheduled_pending);
  bool ota_check_flag = manifest_inflight ? true : false;
  bool mqtt_pub = false;
  bool mqtt_cmd = false;
  bool mqtt_flag = should_block_sleep_for_mqtt(&mqtt_pub, &mqtt_cmd);
  bool time_invalid_flag = false;
  if ((ota_busy_flag || ota_check_flag) && !is_time_valid()) {
    time_invalid_flag = true;
  }

  if (ota_busy) {
    *ota_busy = ota_busy_flag ? true : false;
  }
  if (mqtt_busy) {
    *mqtt_busy = mqtt_flag ? true : false;
  }
  if (time_invalid) {
    *time_invalid = time_invalid_flag ? true : false;
  }
  if (ota_check_busy) {
    *ota_check_busy = ota_check_flag ? true : false;
  }
  return (ota_busy_flag || mqtt_flag || time_invalid_flag || ota_check_flag);
}

static void maybe_set_reboot_guard_override() {
  static bool logged = false;
#if OTA_TEST_BYPASS_REBOOT_LOOP_GUARD
  Preferences prefs;
  if (prefs.begin("halo", false)) {
    prefs.putUInt("dev_dis_rstgrd", 1);
    prefs.end();
  }
  if (!logged) {
    LOG_INFO("[OTA_TEST] bypass reboot_loop_guard (forcing ota_en=1)");
    logged = true;
  }
#else
  Preferences prefs;
  if (prefs.begin("halo", false)) {
    prefs.putUInt("dev_dis_rstgrd", 0);
    prefs.end();
  }
#endif
}

// File-scope: shared between halo_prod_loop() and halo_prod_should_delay_sleep()
static bool s_post_ap_claim_retry_pending = false;
static unsigned long s_post_ap_shutdown_ms = 0;

bool halo_prod_should_delay_sleep() {
  static unsigned long tls_wait_start_ms = 0;
  static bool tls_wait_logged = false;
  static bool tls_time_valid_logged = false;
  static const unsigned long TLS_TIME_WAIT_MS = 45000;

  // Block sleep while post-AP claim retry is pending.
  // This gives the owner code claim time to execute after SoftAP frees memory.
  if (s_post_ap_claim_retry_pending) {
    unsigned long elapsed = (s_post_ap_shutdown_ms > 0) ? (millis() - s_post_ap_shutdown_ms) : 0;
    if (elapsed < 30000) {  // Max 30s awake for claim retry
      LOG_INFO("[TLS_RECOVERY] blocking sleep for claim retry (elapsed=%lu ms)", elapsed);
      return true;
    }
  }

  if (sense_action_inflight()) {
    mqtt_set_allowed(false);
    return false;
  }

  bool apply_active = false;
  bool manifest_inflight = false;
  bool scheduled_pending = false;
  bool ota_pending = should_block_sleep_for_ota(&apply_active, &manifest_inflight, &scheduled_pending);
  bool mqtt_pub = false;
  bool mqtt_cmd = false;
  bool mqtt_pending = should_block_sleep_for_mqtt(&mqtt_pub, &mqtt_cmd);
  MqttMetrics metrics = {};
  mqtt_get_metrics(&metrics);
  bool mqtt_connected = metrics.connected;

  if (!ota_pending && !apply_active) {
    mqtt_set_allowed(false);
  }

  if (wifi_is_connected() && !is_time_valid() && ota_pending) {
    if (tls_wait_start_ms == 0) {
      tls_wait_start_ms = millis();
      tls_wait_logged = false;
      tls_time_valid_logged = false;
    }
    if (!tls_wait_logged) {
      LOG_INFO("[TLS_GUARD] holding awake for SNTP (max 45000ms)");
      tls_wait_logged = true;
    }
    if (millis() - tls_wait_start_ms < TLS_TIME_WAIT_MS) {
      return true;
    }
  } else if (wifi_is_connected() && is_time_valid()) {
    if (!tls_time_valid_logged) {
      time_t now = time(nullptr);
      LOG_INFO("[TLS_GUARD] time_valid epoch=%ld", (long)now);
      tls_time_valid_logged = true;
    }
    tls_wait_start_ms = 0;
  } else {
    tls_wait_start_ms = 0;
    tls_wait_logged = false;
    tls_time_valid_logged = false;
  }

  bool maint_sync_block = false;
  if (maintenance_schedule_pending_sync_to_lcd() &&
      !g_lcd_maint_ack_received &&
      g_maint_sync_wait_until_ms > 0) {
    unsigned long now_ms = millis();
    maint_sync_block = (now_ms < g_maint_sync_wait_until_ms);
    if (maint_sync_block) {
      LOG_INFO("[MAINT_SYNC] hold_awake pending=1 sends=%u wait_left_ms=%lu",
               (unsigned)g_maint_sync_send_count,
               (unsigned long)(g_maint_sync_wait_until_ms - now_ms));
    } else {
      g_maint_sync_wait_until_ms = 0;
    }
  }

  return (ota_pending || mqtt_pending || maint_sync_block);
}

static void send_ota_uart_message(const char* type, bool terminal = false) {
  if (!type || !type[0] || !sense_lcd_ota_retry_safe()) return;
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = type;
  // Legacy/intermediate unlocks retain their continuation hold. Only a proven
  // final outcome releases it; older LCD firmware safely ignores this field.
  if (terminal && strcmp(type, "OTA_UNLOCK") == 0) doc["terminal"] = true;
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void ota_notify_lcd_activity() {
  bool active = g_ota_apply_in_progress ||
                g_ota_check_in_progress ||
                (g_ota_simple_proof_started && !g_ota_simple_proof_done);
  if (active == g_ota_uart_active) {
    return;
  }
  g_ota_uart_active = active;
  send_ota_uart_message(active ? "SENSE_OTA_ACTIVE" : "SENSE_OTA_IDLE");
}

static void maybe_cancel_manual_ota_unready() {
  if (!g_manual_ota_override) {
    return;
  }
  if (g_ota_check_in_progress || g_ota_apply_in_progress) {
    return;
  }
  bool wifi_ok = wifi_is_connected();
  bool time_ok = is_time_valid();
  if (wifi_ok && time_ok) {
    return;
  }
  unsigned long now = millis();
  if (g_manual_ota_request_ms == 0) {
    g_manual_ota_request_ms = now;
  }
  if (!wifi_ok) {
    if (g_manual_ota_wifi_retry_ms == 0 || now >= g_manual_ota_wifi_retry_ms) {
      LOG_INFO("[OTA_MANUAL] wifi_retry connected=0");
      bool connected = ensure_wifi_connected("manual_ota", 3000);
      g_manual_ota_wifi_retry_ms = now + MANUAL_OTA_WIFI_RETRY_MS;
      if (connected) {
        g_manual_ota_wifi_retry_ms = 0;
      }
    }
  }
  static unsigned long last_wait_log_ms = 0;
  if (now - last_wait_log_ms > 10000) {
    LOG_INFO("[OTA_MANUAL] waiting wifi=%d time=%d",
             wifi_ok ? 1 : 0,
             time_ok ? 1 : 0);
    last_wait_log_ms = now;
  }
}


static bool ota_peer_accept_new_request() {
  if (g_ota_check_in_progress || g_ota_apply_in_progress || g_lcd_ota_task_running ||
      g_lcd_ota_proxy_owns_uart || g_peer_gate.active || g_boot_ota_pending) return false;
  g_peer_episode_finished = false;
  g_ota_check_done = false;
  g_lcd_work_budget_live = false; g_peer_continue_work = false;
  g_lcd_work_schedule[0] = 0;
  return true;
}

void halo_prod_request_manual_ota(const char* reason) {
  if (!ota_peer_accept_new_request()) {
    LOG_INFO("[OTA_MANUAL] joined existing bounded OTA episode");
    return;
  }
  manual_ota_override_set(reason ? reason : "manual");
  const uint32_t ts = is_time_valid() ? (uint32_t)time(nullptr) : 0;
  OtaIntent::updateDesired(nullptr, nullptr, true, false, ts, "manual");
  g_ota_check_requested = true;
  g_ota_check_done = false;
  g_ota_skip_logged = false;
  g_lcd_ota_done = false;
  strlcpy(g_lcd_ota_result, "pending", sizeof(g_lcd_ota_result));
  g_lcd_ota_attempted_this_window = false;
  LOG_INFO("[OTA_MANUAL] accepted; waiting for fresh LCD readiness");
}

void halo_prod_request_maint_test(uint32_t duration_sec) {
  if (!is_time_valid()) {
    Serial.println("[MAINT_TEST] ERROR: time not valid, cannot create window");
    return;
  }
  if (g_maintenance_mode) {
    Serial.println("[MAINT_TEST] ERROR: maintenance already active");
    return;
  }

  // Create a maintenance window starting NOW
  time_t now = time(nullptr);
  uint64_t now_epoch = (uint64_t)now;

  MaintenanceWindow mw;
  mw.scheduled = true;
  mw.start_epoch = now_epoch;
  mw.duration_sec = duration_sec;
  snprintf(mw.request_id, sizeof(mw.request_id), "test_%llu", (unsigned long long)now_epoch);
  mw.grace_before_sec = 60;
  mw.grace_after_sec = 600;
  mw.saveToNvs();

  // Clear any consumed state for this test
  maintenance_window_consumed_clear("maint_test");
  maintenance_followup_retry_clear("maint_test");

  Serial.printf("[MAINT_TEST] window created start=%llu dur=%lu request_id=%s\n",
                (unsigned long long)mw.start_epoch,
                (unsigned long)mw.duration_sec,
                mw.request_id);

  // Sync to LCD
  uint32_t remaining_s = mw.duration_sec + mw.grace_after_sec;
  set_maintenance_schedule_pending_sync_to_lcd(true);

  // Arm maintenance mode — next loop() iteration runs run_maintenance_if_needed()
  g_maintenance_mode = true;
  g_maintenance_handled = false;
  g_maintenance_in_window = false;

  Serial.println("[MAINT_TEST] maintenance mode armed — will run on next loop()");
}

static void send_maint_window(const MaintenanceWindow* mw,
                              uint32_t remaining_s,
                              uint32_t wake_in_s,
                              bool clear_schedule) {
  StaticJsonDocument<384> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "MAINT_WINDOW";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["remaining_s"] = remaining_s;
  // Carry the Sense's current wall clock so the LCD (which has no NTP/RTC time
  // source of its own) can settimeofday() and run its absolute-epoch maintenance
  // machinery. Omitted when our own clock is invalid; the LCD then falls back to
  // the relative wake_in_s/remaining_s offsets (backward compatible).
  if (is_time_valid()) {
    doc["now_epoch"] = (uint64_t)time(nullptr);
  }
  if (mw) {
    if (mw->request_id[0]) {
      doc["request_id"] = mw->request_id;
    }
    if (mw->start_epoch > 0) {
      doc["start_epoch"] = mw->start_epoch;
      doc["duration_sec"] = mw->duration_sec;
      doc["grace_before_sec"] = mw->grace_before_sec;
      doc["grace_after_sec"] = mw->grace_after_sec;
    }
  }
  if (wake_in_s > 0) {
    doc["wake_in_s"] = wake_in_s;
  }
  if (clear_schedule) {
    doc["clear"] = true;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  LOG_INFO("[MAINT_TX] to_lcd remaining_s=%lu wake_in_s=%lu clear=%d request_id=%s start_epoch=%llu now_epoch=%llu",
           (unsigned long)remaining_s,
           (unsigned long)wake_in_s,
           clear_schedule ? 1 : 0,
           (mw && mw->request_id[0]) ? mw->request_id : "-",
           (unsigned long long)((mw && mw->start_epoch > 0) ? mw->start_epoch : 0ULL),
           (unsigned long long)(is_time_valid() ? (uint64_t)time(nullptr) : 0ULL));
}

// Co-schedule hook (registered into sense_sleep.h at setup). Called from the
// deep-sleep path with the Sense's FINAL wake delta so the LCD arms a matching
// maintenance wake and is awake for the LCD-OTA proxy on the unattended nightly
// path. Reuses the tested send_maint_window() sender; mw=nullptr => a bare
// relative wake (remaining_s + wake_in_s), which is all the LCD needs to arm
// g_lcd_maintenance_timer_armed (see lcd_uart_rx.h). now_epoch rides along when
// our clock is valid so the LCD can align absolutely.
static void prod_co_schedule_lcd_maint_wake(uint32_t wake_in_s) {
  if (wake_in_s == 0) {
    return;
  }
  MaintenanceWindow identity;
  memset(&identity, 0, sizeof(identity));
  const time_t target = time(nullptr) + wake_in_s;
  struct tm local_target;
  localtime_r(&target, &local_target);
  if (sense_time_has_fresh_sync() && local_target.tm_hour == 2 && local_target.tm_min == 0) {
    snprintf(identity.request_id, sizeof(identity.request_id), "nightly_%04d%02d%02d",
             local_target.tm_year + 1900, local_target.tm_mon + 1, local_target.tm_mday);
  } else {
    snprintf(identity.request_id, sizeof(identity.request_id), "relative_%lu_%lu",
             (unsigned long)target, (unsigned long)wake_in_s);
  }
  Preferences p;
  if (p.begin("ota_coord", false)) {
    p.putString("schedule", identity.request_id);
    p.end();
  }
  strlcpy(g_coord_schedule, identity.request_id, sizeof(g_coord_schedule));
  send_maint_window(&identity, wake_in_s, wake_in_s, false);
  LOG_INFO("[MAINT_COSCHED] told LCD to wake in %lus (align nightly maintenance)",
           (unsigned long)wake_in_s);
}

// Arm-time delivery race fix: lightweight Sense->LCD keep-awake used ONLY while a
// maintenance schedule is pending-sync but our clock is not yet valid (so we can't
// yet send the real MAINT_WINDOW with now_epoch). The LCD handler for this type
// just resets its activity timer + nudges stay-awake (see lcd_uart_rx.h), so the
// freshly-tapped LCD does not idle-sleep before NTP completes and the real window
// (with now_epoch) is delivered and acked. Carries the request_id for traceability.
static void send_maint_keepalive(const char* request_id) {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "MAINT_KEEPALIVE";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (request_id && request_id[0]) {
    doc["request_id"] = request_id;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  LOG_INFO("[MAINT_TX] keepalive_to_lcd request_id=%s",
           (request_id && request_id[0]) ? request_id : "-");
}

// Arm-time delivery race fix: keep the LCD awake during the post-wake
// WiFi+NTP+schedule-fetch phase. The pending-sync flag is only set AFTER the
// HTTPS fetch (which needs valid time), so a "pending && !time_valid" trigger
// can't catch the early window — by then the LCD has already idle-slept during
// the fetch. This runs every awake loop and sends a lightweight MAINT_KEEPALIVE
// while we are still early in the wake AND either the OTA/schedule check hasn't
// run yet (g_ota_check_done false — spans the fetch) OR the window is pending but
// not yet acked (spans delivery). NOT gated on halo_uart_link_recent(): that
// tracks LCD->Sense traffic which goes stale a few seconds after the tap even
// while the LCD is awake, which would cut keepalives off too early. Sending to an
// already-asleep LCD is a harmless no-op; the wake-window + gates bound it.
static void keep_lcd_awake_during_maint_arm() {
  unsigned long now_ms = millis();
  // Only early in the wake. last_wake_ms is reset every wake in setup().
  if ((now_ms - last_wake_ms) >= KEEPALIVE_ARM_WINDOW_MS) {
    return;
  }
  bool pending = maintenance_schedule_pending_sync_to_lcd();
  // "not yet acked" mirrors the ack_ok logic in sync_pending_maintenance_to_lcd().
  bool acked = g_lcd_maint_ack_received;
  if (acked && g_maint_pending_request_id[0]) {
    acked = maint_sync_request_matches(g_maint_pending_request_id, g_lcd_maint_ack_request_id);
  }
  bool want_keepalive = (!g_ota_check_done) || (pending && !acked);
  if (!want_keepalive) {
    return;
  }
  if (g_maint_keepalive_last_tx_ms != 0 &&
      (now_ms - g_maint_keepalive_last_tx_ms) < MAINT_KEEPALIVE_RESEND_MS) {
    return;
  }
  // request_id is available once the window is loaded; "" before the fetch lands.
  MaintenanceWindow mw;
  const char* req_id = "";
  if (maintenance_window_load(&mw) && mw.request_id[0]) {
    req_id = mw.request_id;
  }
  send_maint_keepalive(req_id);
  g_maint_keepalive_last_tx_ms = now_ms;
}


static void sync_pending_maintenance_to_lcd(const char* reason) {
  bool pending = maintenance_schedule_pending_sync_to_lcd();
  if (!pending) {
    g_maint_pending_last = false;
    g_maint_sync_no_ack_logged = false;
    g_maint_sync_wait_until_ms = 0;
    g_maint_pending_request_id[0] = '\0';
    // NOTE: do NOT reset g_maint_keepalive_last_tx_ms here — this not-pending path
    // runs every loop during the pre-fetch phase (pending is set only AFTER the
    // HTTPS fetch), and keep_lcd_awake_during_maint_arm() owns the keepalive
    // cadence; zeroing it here would defeat the 2s rate-limit (arm-time race fix).
    return;
  }
  MaintenanceWindow mw;
  bool has_window = maintenance_window_load(&mw);
  const char* current_request_id = (has_window && mw.request_id[0]) ? mw.request_id : "";
  if (!g_maint_pending_last) {
    g_maint_pending_last = true;
    maint_sync_reset_ack_state();
    g_maint_sync_send_count = 0;
    g_maint_sync_last_tx_ms = 0;
    g_maint_sync_no_ack_logged = false;
    maint_sync_copy_str(g_maint_pending_request_id, sizeof(g_maint_pending_request_id), current_request_id);
    maint_sync_note_resolution("pending", current_request_id);
  } else if (!maint_sync_request_matches(g_maint_pending_request_id, current_request_id)) {
    maint_sync_reset_ack_state();
    g_maint_sync_send_count = 0;
    g_maint_sync_last_tx_ms = 0;
    g_maint_sync_no_ack_logged = false;
    maint_sync_copy_str(g_maint_pending_request_id, sizeof(g_maint_pending_request_id), current_request_id);
    maint_sync_note_resolution("request_changed", current_request_id);
  }
  unsigned long now_ms = millis();
  if (g_maint_sync_last_tx_ms > 0 &&
      (now_ms - g_maint_sync_last_tx_ms) < MAINT_SYNC_RESEND_MS) {
    return;
  }
  bool link_recent = halo_uart_link_recent(LCD_MAINT_LINK_RECENT_MS);
  if (!link_recent) {
    LOG_INFO("[MAINT_WINDOW] link_not_recent reason=%s link_recent=0",
             reason ? reason : "unknown");
  }
  bool time_ok = is_time_valid();
  if (!time_ok) {
    ensure_time_valid(reason ? reason : "maint_sync", 4000);
    time_ok = is_time_valid();
  }
  LOG_INFO("[MAINT_SYNC] pending=1 has_window=%d time_ok=%d reason=%s",
           has_window ? 1 : 0,
           time_ok ? 1 : 0,
           reason ? reason : "unknown");
  uint64_t now_epoch = 0;
  if (time_ok) {
    time_t now_s = time(nullptr);
    if (now_s > 0) {
      now_epoch = (uint64_t)now_s;
    }
  }
  bool consumed = (has_window && now_epoch > 0) ? maintenance_window_is_consumed(mw, now_epoch) : false;
  uint32_t maint_remaining_s = 0;
  uint32_t maint_wake_in_s = (has_window && time_ok)
                                 ? maintenance_window_wake_delta_s(&maint_remaining_s)
                                 : 0;
  if (has_window && time_ok && maint_remaining_s == 0) {
    maint_remaining_s = mw.duration_sec + mw.grace_after_sec;
  }
  LOG_INFO("[MAINT_SYNC] computed remaining_s=%lu wake_in_s=%lu request_id=%s consumed=%d",
           (unsigned long)maint_remaining_s,
           (unsigned long)maint_wake_in_s,
           has_window && mw.request_id[0] ? mw.request_id : "-",
           consumed ? 1 : 0);
  bool sent_window = false;
  bool sent_clear = false;
  if (has_window && consumed) {
    send_maint_window(&mw, 0, 0, true);
    sent_clear = true;
    sched_event_note("lcd_clear_consumed", mw.request_id);
  } else if (has_window && time_ok && (maint_wake_in_s > 0 || maint_remaining_s > 0)) {
    send_maint_window(&mw, maint_remaining_s, maint_wake_in_s, false);
    sent_window = true;
  } else if (!has_window) {
    send_maint_window(nullptr, 0, 0, true);
    sent_clear = true;
  }
  if (sent_window || sent_clear) {
    g_maint_sync_last_tx_ms = now_ms;
    if (g_maint_sync_send_count < 255) {
      g_maint_sync_send_count++;
    }
    maint_sync_note_tx(current_request_id,
                       maint_remaining_s,
                       maint_wake_in_s,
                       sent_clear,
                       link_recent);
    maint_sync_note_resolution("tx_sent", current_request_id);
  }
  bool ack_ok = g_lcd_maint_ack_received;
  if (ack_ok && current_request_id[0]) {
    ack_ok = maint_sync_request_matches(current_request_id, g_lcd_maint_ack_request_id);
  }
  if (!ack_ok && g_maint_sync_send_count >= MAINT_SYNC_MAX_SENDS && !g_maint_sync_no_ack_logged) {
    g_maint_sync_no_ack_logged = true;
    LOG_INFO("[MAINT_SYNC] no_ack pending=1 sends=%u link_recent=%d",
             (unsigned)g_maint_sync_send_count,
             link_recent ? 1 : 0);
    maint_sync_note_resolution("timeout_no_ack", current_request_id);
    dump_system_truth("maint_sync_no_ack");
  }
  if ((sent_window || sent_clear) && ack_ok) {
    set_maintenance_schedule_pending_sync_to_lcd(false);
    g_maint_pending_last = false;
    g_maint_pending_request_id[0] = '\0';
    g_maint_sync_wait_until_ms = 0;
    // keep_lcd_awake_during_maint_arm() naturally stops once acked (its
    // pending&&!acked gate goes false) and/or the wake window elapses; the
    // keepalive cadence timer is per-wake-fresh, so no reset needed here.
    maint_sync_note_resolution("acked", current_request_id);
    LOG_INFO("[MAINT_SYNC] sent_to_lcd window=%d clear=%d ack=1",
             sent_window ? 1 : 0,
             sent_clear ? 1 : 0);
  } else if (sent_window || sent_clear) {
    if (!link_recent) {
      maint_sync_note_resolution("sent_no_recent_link", current_request_id);
    }
    LOG_INFO("[MAINT_SYNC] sent_to_lcd best_effort pending=1 link_recent=%d ack=%d",
             link_recent ? 1 : 0,
             ack_ok ? 1 : 0);
  } else if (!time_ok) {
    // Arm-time delivery race fix: the LCD keep-awake during this not-yet-valid
    // phase is centralized in keep_lcd_awake_during_maint_arm() (called every
    // awake loop, BEFORE this function), so this branch is back to log-only.
    maint_sync_note_resolution("deferred_time_invalid", current_request_id);
    LOG_INFO("[MAINT_WINDOW] time_invalid defer_sync");
  } else {
    maint_sync_note_resolution("skip_empty_window", current_request_id);
    LOG_INFO("[MAINT_SYNC] skip_send reason=empty_window has_window=%d",
             has_window ? 1 : 0);
  }
}

static void maintenance_resync_on_time_jump(const char* reason) {
  if (!is_time_valid()) {
    return;
  }
  uint32_t now_epoch = (uint32_t)time(nullptr);
  if (g_last_time_valid_epoch > 0) {
    uint32_t delta = (now_epoch > g_last_time_valid_epoch)
                         ? (now_epoch - g_last_time_valid_epoch)
                         : (g_last_time_valid_epoch - now_epoch);
    if (delta >= MAINT_TIME_JUMP_RESYNC_S) {
      MaintenanceWindow mw;
      if (maintenance_window_load(&mw)) {
        LOG_INFO("[MAINT_WINDOW] time_jump_resync delta_s=%lu reason=%s",
                 (unsigned long)delta,
                 reason ? reason : "unknown");
        set_maintenance_schedule_pending_sync_to_lcd(true);
      }
    }
  }
  g_last_time_valid_epoch = now_epoch;
}

static void lcd_ota_proxy_task(void* param) {
  HaloNtpDnsGuard ntp_dns_guard;
  LOG_INFO("[LCD_OTA_PROXY_TASK] started stack=%u",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));

  // Release camera DMA reservation to free 16KB of internal SRAM for TLS.
  // Camera is not used during OTA. Device reboots after LCD OTA, re-reserving in setup().
  if (g_camera_dma_reserve) {
    heap_caps_free(g_camera_dma_reserve);
    g_camera_dma_reserve = nullptr;
    LOG_INFO("[LCD_OTA_PROXY] Camera DMA reservation released for TLS headroom");
  }

  // Step 1: Query LCD for its current firmware version
  char lcd_fw[32] = {0};
  uint32_t lcd_part_size = 0;
  if (!sense_lcd_ota_query(lcd_fw, sizeof(lcd_fw), &lcd_part_size)) {
    LOG_INFO("[LCD_OTA_ORCH] lcd_query_fail (proxy)");
    strncpy(g_lcd_ota_result, "lcd_query_fail", sizeof(g_lcd_ota_result) - 1);
    g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
    g_lcd_ota_done = true;
    send_ota_uart_message("OTA_UNLOCK");
    LOG_INFO("[LCD_OTA_ORCH] OTA_UNLOCK sent (lcd_query_fail)");
    g_lcd_ota_task_running = false;
    ntp_dns_guard.release();
    vTaskDelete(NULL);
    return;
  }

  // Step 2: Fetch LCD manifest from S3
  // (MQTT + manifest client already released before task creation)
  const OtaUrlConfig* cfg = ota_get_config();
  OtaManifest lcd_manifest;
  if (!sense_lcd_ota_fetch_manifest(cfg->base_dir, cfg->channel, lcd_manifest)) {
    LOG_INFO("[LCD_OTA_ORCH] manifest_fetch_fail (proxy)");
    strncpy(g_lcd_ota_result, "manifest_fetch_fail", sizeof(g_lcd_ota_result) - 1);
    g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
    g_lcd_ota_done = true;
    send_ota_uart_message("OTA_UNLOCK");
    LOG_INFO("[LCD_OTA_ORCH] OTA_UNLOCK sent (manifest_fetch_fail)");
    mqtt_force_connect();
    g_lcd_ota_task_running = false;
    ntp_dns_guard.release();
    vTaskDelete(NULL);
    return;
  }

  // Step 4: Proxy the update (downloads binary, streams to LCD via UART)
  const char* proxy_result = sense_lcd_ota_proxy(lcd_manifest, lcd_fw);
  LOG_INFO("[LCD_OTA_ORCH] proxy_result=%s", proxy_result);

  // Map proxy result to g_lcd_ota_result
  if (strcmp(proxy_result, "success") == 0) {
    strncpy(g_lcd_ota_result, "updated", sizeof(g_lcd_ota_result) - 1);
    // Do NOT assume the manifest version is now running. The LCD reboots into
    // the new image; the cloud-reported lcd_fw must reflect the REAL running
    // version from a future LCD_OTA_QUERY_RESP, not the OTA target. Invalidate
    // the cached value so the pre-sleep / periodic query path re-queries the
    // LCD and overwrites it with the actual booted version.
    g_lcd_ota_version[0] = '\0';
    g_lcd_fw_query_ms = 0;
    LOG_INFO("[LCD_OTA_ORCH] cleared cached lcd_fw; will re-query real version post-reboot");
  } else if (strcmp(proxy_result, "up_to_date") == 0) {
    strncpy(g_lcd_ota_result, "noop", sizeof(g_lcd_ota_result) - 1);
  } else {
    strncpy(g_lcd_ota_result, proxy_result, sizeof(g_lcd_ota_result) - 1);
  }
  g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
  g_lcd_ota_done = true;

  // Unlock LCD so it can sleep (success case: LCD reboots, but unlock is harmless)
  send_ota_uart_message("OTA_UNLOCK");
  LOG_INFO("[LCD_OTA_ORCH] OTA_UNLOCK sent (proxy_result=%s)", proxy_result);

  // Restore MQTT connection
  mqtt_force_connect();

  LOG_INFO("[LCD_OTA_PROXY_TASK] done stack_remaining=%u",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  g_lcd_ota_task_running = false;
  ntp_dns_guard.release();
  vTaskDelete(NULL);
}

static void maybe_trigger_lcd_ota_check() {
  // Foreground coordination owns this episode; never create a second worker
  // from one of its failure/no-op cleanup branches. Debt survives to recovery.
  if (g_peer_gate.active || g_peer_episode_finished || g_ota_check_in_progress) return;
  // Don't spawn a second proxy task
  if (g_lcd_ota_task_running) {
    return;
  }
  // Allow when: maintenance window is active OR a manual request is pending
  bool manual_pending = !g_lcd_ota_done && strcmp(g_lcd_ota_result, "pending") == 0;
  bool maintenance_allowed = g_maintenance_in_window;
  if (!maintenance_allowed && !manual_pending) {
    return;
  }
  // Skip maintenance-sync gates for manual requests
  if (maintenance_allowed && !manual_pending) {
    if (maintenance_schedule_pending_sync_to_lcd()) {
      LOG_INFO("[LCD_OTA_ORCH] defer (maint_sync_pending)");
      return;
    }
    if (!g_lcd_maint_ack_received) {
      bool link_recent = halo_uart_link_recent(LCD_MAINT_LINK_RECENT_MS);
      bool allow_without_ack = link_recent || g_maint_sync_send_count >= MAINT_SYNC_MAX_SENDS;
      if (!allow_without_ack) {
        LOG_INFO("[LCD_OTA_ORCH] defer (maint_ack_missing)");
        return;
      }
      LOG_INFO("[LCD_OTA_ORCH] override (maint_ack_missing) sends=%u link_recent=%d",
               (unsigned)g_maint_sync_send_count,
               link_recent ? 1 : 0);
    }
  }
  if (g_lcd_ota_attempted_this_window) {
    return;
  }
  ProvisioningState::State prov_state = ProvisioningState::getState();
  maybe_send_lcd_wifi_creds(prov_state);

  // ── LCD OTA proxy: run on dedicated task (TLS needs ~12KB stack) ──
  g_lcd_ota_attempted_this_window = true;
  g_lcd_ota_task_running = true;

  // Free internal RAM before task creation — the 12KB stack needs contiguous
  // internal memory, and MQTT + manifest client can hold ~2-4KB.
  g_manifest_client.releaseConnection();
  mqtt_stop_for_ota();
  vTaskDelay(pdMS_TO_TICKS(300));  // Let memory coalesce

  LOG_INFO("[LCD_OTA_ORCH] heap before task: free=%u largest=%u psram_free=%u",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  BaseType_t rc = xTaskCreatePinnedToCore(
    lcd_ota_proxy_task, "lcd_ota_proxy", 12288, NULL, 3, NULL, tskNO_AFFINITY);
  if (rc != pdPASS) {
    LOG_ERROR("[LCD_OTA_ORCH] task create FAILED rc=%d", (int)rc);
    g_lcd_ota_task_running = false;
    send_ota_uart_message("OTA_UNLOCK");
    LOG_INFO("[LCD_OTA_ORCH] OTA_UNLOCK sent (task create failed)");
    strncpy(g_lcd_ota_result, "task_create_fail", sizeof(g_lcd_ota_result) - 1);
    g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
    g_lcd_ota_done = true;
    mqtt_force_connect();
  } else {
    LOG_INFO("[LCD_OTA_ORCH] proxy task spawned");
  }
}

static void send_provision_status(const char* state) {
  if (!state || !state[0]) return;
  StaticJsonDocument<160> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "PROVISION_STATUS";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["state"] = state;
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void maybe_send_lcd_wifi_creds(ProvisioningState::State prov_state) {
  if (!ProvisioningState::isProvisioned() || prov_state != ProvisioningState::STATE_CONNECTED) {
    return;
  }
  if (!g_lcd_alive || g_lcd_wifi_creds_sent || g_lcd_wifi_send_failed) {
    return;
  }
  if (g_lcd_wifi_send_start_ms == 0) {
    g_lcd_wifi_send_start_ms = millis();
  }
  if (millis() - g_lcd_wifi_send_start_ms > LCD_WIFI_SEND_TIMEOUT_MS) {
    LOG_INFO("[UART][WIFI] LCD ack timeout (giving up)");
    g_lcd_wifi_send_failed = true;
    return;
  }

  if (!g_lcd_got_creds_ack && (millis() - g_last_creds_send_ms) >= LCD_WIFI_SEND_RETRY_MS) {
    char ssid[64];
    char pass[64];
    if (ProvisioningState::loadHomeWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass))) {
      uart_send_wifi_creds_json(ssid, pass);
      LOG_INFO("[UART][WIFI_CREDS] sent to LCD (ssid_len=%u pass_len=%u)",
               (unsigned)strlen(ssid), (unsigned)strlen(pass));
      g_last_creds_send_ms = millis();
    }
    return;
  }

  if (g_lcd_got_creds_ack && !g_lcd_got_wifion_ack &&
      (millis() - g_last_wifion_send_ms) >= LCD_WIFI_SEND_RETRY_MS) {
    if (!g_maintenance_in_window) {
      LOG_INFO("[UART][WIFI_ON] skip (ota_only)");
      g_lcd_wifi_creds_sent = true;
      return;
    }
    uint32_t lcd_wifi_timeout_ms = 600000;
    if (g_lcd_ota_window_remaining_s > 0) {
      lcd_wifi_timeout_ms = g_lcd_ota_window_remaining_s * 1000UL;
      if (lcd_wifi_timeout_ms < 60000UL) {
        lcd_wifi_timeout_ms = 60000UL;
      }
      if (lcd_wifi_timeout_ms > 600000UL) {
        lcd_wifi_timeout_ms = 600000UL;
      }
    }
    uart_send_wifi_on_json(lcd_wifi_timeout_ms);
    LOG_INFO("[UART][WIFI_ON] sent to LCD timeout_ms=%lu", (unsigned long)lcd_wifi_timeout_ms);
    g_last_wifion_send_ms = millis();
    return;
  }

  if (g_lcd_got_creds_ack && g_lcd_got_wifion_ack) {
    LOG_INFO("[UART][WIFI] LCD ACKs received");
    g_lcd_wifi_creds_sent = true;
  }
}

static void send_provision_qr() {
  const char* ssid = g_provisioning_manager.getApSsid();
  const char* pass = g_provisioning_manager.getApPassword();
  if (!ssid || !ssid[0] || !pass || !pass[0]) {
    LOG_WARN("[PROVISION] QR not sent (missing AP creds)");
    return;
  }
  LOG_INFO("[PROVISION] Sending PROVISION_QR ssid=%s", ssid);
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "PROVISION_QR";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["ssid"] = ssid;
  doc["password"] = pass;
  doc["url"] = "http://192.168.4.1";
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static bool ensure_wifi_connected_for_sleep(uint32_t timeout_ms, uint32_t max_total_ms) {
  if (wifi_is_connected()) {
    return true;
  }
  if (max_total_ms == 0) {
    LOG_INFO("[MQTT] pre_sleep skip wifi (budget_exhausted)");
    return false;
  }
  if (g_provisioning_manager.isSetupModeActive()) {
    LOG_INFO("[MQTT] pre_sleep skip wifi (setup mode active)");
    return false;
  }
  unsigned long overall_start_ms = millis();
  uint32_t connect_timeout_ms = timeout_ms;
  if (max_total_ms > 0 && max_total_ms < connect_timeout_ms) {
    connect_timeout_ms = max_total_ms;
  }
  if (connect_timeout_ms < 1000) {
    LOG_INFO("[MQTT] pre_sleep skip wifi (budget_too_small) timeout_ms=%lu",
             (unsigned long)connect_timeout_ms);
    return false;
  }
  if (!ensure_wifi_connected("pre_sleep", connect_timeout_ms)) {
    unsigned long elapsed_ms = millis() - overall_start_ms;
    if (max_total_ms > 0 && elapsed_ms >= max_total_ms) {
      LOG_INFO("[MQTT] pre_sleep skip wifi (budget_exhausted) elapsed_ms=%lu",
               (unsigned long)elapsed_ms);
      return false;
    }
    uint32_t retry_timeout_ms = timeout_ms >= 2000 ? 2000 : timeout_ms;
    if (retry_timeout_ms >= 1000) {
      LOG_INFO("[MQTT] pre_sleep retry wifi (hard reset) timeout_ms=%lu",
               (unsigned long)retry_timeout_ms);
      if (wifi_hard_reset_and_reconnect("pre_sleep_retry", retry_timeout_ms)) {
        LOG_INFO("[MQTT] pre_sleep wifi recovered after retry");
        return true;
      }
    }
    // Keep a short recovery window in case Wi-Fi comes back mid-run.
    uint32_t recovery_window_ms = PRE_SLEEP_WIFI_RECOVERY_MS;
    if (max_total_ms > 0) {
      elapsed_ms = millis() - overall_start_ms;
      uint32_t remaining_budget_ms =
          max_total_ms > elapsed_ms ? (max_total_ms - (uint32_t)elapsed_ms) : 0;
      if (remaining_budget_ms < 1000) {
        LOG_INFO("[MQTT] pre_sleep skip wifi (budget_exhausted)");
        return false;
      }
      if (remaining_budget_ms < recovery_window_ms) {
        recovery_window_ms = remaining_budget_ms;
      }
    }
    unsigned long start_ms = millis();
    unsigned long next_retry_ms = start_ms;
    while ((millis() - start_ms) < recovery_window_ms) {
      if (wifi_is_connected()) {
        LOG_INFO("[MQTT] pre_sleep wifi recovered during recovery_window");
        return true;
      }
      unsigned long now_ms = millis();
      if (now_ms >= next_retry_ms) {
        uint32_t remaining_ms =
            (uint32_t)(recovery_window_ms - (now_ms - start_ms));
        uint32_t attempt_ms =
            (remaining_ms > PRE_SLEEP_WIFI_HARD_RESET_MS)
                ? PRE_SLEEP_WIFI_HARD_RESET_MS
                : remaining_ms;
        if (attempt_ms >= 1000) {
          LOG_INFO("[MQTT] pre_sleep recovery retry timeout_ms=%lu",
                   (unsigned long)attempt_ms);
          if (wifi_hard_reset_and_reconnect("pre_sleep_recover", attempt_ms)) {
            LOG_INFO("[MQTT] pre_sleep wifi recovered during recovery_retry");
            return true;
          }
        }
        next_retry_ms = now_ms + PRE_SLEEP_WIFI_RETRY_INTERVAL_MS;
      }
      delay(200);
    }
    LOG_INFO("[MQTT] pre_sleep skip wifi (no connect)");
    return false;
  }
  return true;
}

static bool wait_for_time_valid(uint32_t timeout_ms) {
  unsigned long start = millis();
  while ((millis() - start) < timeout_ms) {
    if (is_time_valid()) {
      return true;
    }
    delay(100);
  }
  return false;
}


static bool wifiReadyForHttps() {
  if (!wifi_is_connected()) {
    return false;
  }
  if (static_cast<uint32_t>(WiFi.localIP()) == 0) {
    return false;
  }
  IPAddress d0 = WiFi.dnsIP(0);
  if (static_cast<uint32_t>(d0) == 0) {
    return false;
  }
  return true;
}

static bool is_time_valid() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

static bool g_tz_initialized = false;

// Apply the owner's timezone once, at the first point we have a network.
//
// This used to hardcode `setenv("TZ", "PST8PDT,...")`. It runs on WiFi connect,
// i.e. AFTER setup() has already applied the zone loaded from NVS — so it
// silently overwrote it and forced US Pacific on every device in the fleet. The
// nightly maintenance wake is defined in LOCAL time, so an owner in New York was
// being woken at 05:00 and one in London at 10:00, and the boot log looked
// perfectly healthy because `nextwake` faithfully reported the wrong zone.
//
// Reading NVS here (rather than trusting what setup() applied) is deliberate:
// this runs well after ProvisioningState::init(), so it is the first moment the
// stored zone is guaranteed readable.
static void ensure_timezone_pt(const char* reason) {
  if (g_tz_initialized) {
    return;
  }
  char saved_tz[64];
  const bool have = ProvisioningState::loadTimezone(saved_tz, sizeof(saved_tz)) && saved_tz[0];
  sense_set_timezone(have ? saved_tz : nullptr);   // nullptr -> HALO_DEFAULT_TZ
  g_tz_initialized = true;
  LOG_INFO("[TZ] set=%s source=%s reason=%s",
           have ? saved_tz : HALO_DEFAULT_TZ,
           have ? "nvs" : "default",
           reason ? reason : "unknown");
}

// Start SNTP/NTP the instant WiFi (STA) connects, so the owner-claim TLS and OTA
// schedule calls have valid time on the first attempt rather than failing http=-1
// while time is still invalid right after connect. Idempotent (only starts once per
// boot) and non-blocking. Suspended network work can resume the same attempt
// inside its original deadline; a completed/expired attempt never re-arms.
void halo_prod_kick_time_sync(const char* reason) {
  ensure_timezone_pt(reason ? reason : "kick_time_sync");
  sense_ntp_service();
  sense_ntp_begin();
}

static int clamp_int(int value, int min_value, int max_value) {
  if (value < min_value) return min_value;
  if (value > max_value) return max_value;
  return value;
}

static void ota_sched_save() {
  if (!g_ota_sched_prefs.begin(OTA_SCHED_NS, false)) {
    return;
  }
  g_ota_sched_prefs.putBool(OTA_SCHED_ENABLE_KEY, g_ota_sched.enabled);
  g_ota_sched_prefs.putUInt(OTA_SCHED_START_KEY, g_ota_sched.window_start_min);
  g_ota_sched_prefs.putUInt(OTA_SCHED_WINDOW_KEY, g_ota_sched.window_dur_min);
  g_ota_sched_prefs.putUInt(OTA_SCHED_DUR_KEY, g_ota_sched.window_dur_min);
  g_ota_sched_prefs.putUInt(OTA_SCHED_JITTER_KEY, g_ota_sched.jitter_min);
  g_ota_sched_prefs.putUInt(OTA_SCHED_IDLE_KEY, g_ota_sched.min_idle_min);
  g_ota_sched_prefs.putUInt(OTA_SCHED_NEXT_KEY, g_next_ota_epoch);
  g_ota_sched_prefs.putUInt(OTA_SCHED_SYNC_KEY, g_last_time_sync_epoch);
  g_ota_sched_prefs.end();
}

static void ota_sched_load() {
#if !OTA_SCHED_ENABLED
  g_ota_sched.enabled = false;
  g_next_ota_epoch = 0;
  g_last_time_sync_epoch = 0;
  return;
#endif
  if (!g_ota_sched_prefs.begin(OTA_SCHED_NS, true)) {
    return;
  }
  bool stored_enable = g_ota_sched_prefs.getBool(OTA_SCHED_ENABLE_KEY, true);
  g_ota_sched.enabled = stored_enable;
  g_ota_sched.window_start_min = g_ota_sched_prefs.getUInt(OTA_SCHED_START_KEY, 120);
  uint16_t window_min = g_ota_sched_prefs.getUInt(OTA_SCHED_WINDOW_KEY, 0);
  if (window_min == 0) {
    window_min = g_ota_sched_prefs.getUInt(OTA_SCHED_DUR_KEY, 30);
  }
  g_ota_sched.window_dur_min = (uint16_t)clamp_int(window_min, 1, 180);
  g_ota_sched.jitter_min = g_ota_sched_prefs.getUInt(OTA_SCHED_JITTER_KEY, 10);
  g_ota_sched.min_idle_min = g_ota_sched_prefs.getUInt(OTA_SCHED_IDLE_KEY, 10);
  g_next_ota_epoch = g_ota_sched_prefs.getUInt(OTA_SCHED_NEXT_KEY, 0);
  g_last_time_sync_epoch = g_ota_sched_prefs.getUInt(OTA_SCHED_SYNC_KEY, 0);
  g_ota_sched_prefs.end();
}

static void ota_sched_log_next_epoch(uint32_t epoch, const char* reason) {
  if (epoch == 0) {
    return;
  }
  time_t target = (time_t)epoch;
  tm local = {};
  localtime_r(&target, &local);
  LOG_INFO("[OTA_SCHED] next_epoch=%lu local=%02d:%02d reason=%s",
           (unsigned long)epoch,
           local.tm_hour,
           local.tm_min,
           reason ? reason : "unknown");
}

static uint32_t ota_sched_compute_next_epoch(time_t now) {
  if (now <= 0) {
    return 0;
  }
  if (OTA_SCHED_TEST_OFFSET_SEC > 0) {
    return (uint32_t)(now + (time_t)OTA_SCHED_TEST_OFFSET_SEC);
  }
  tm local = {};
  localtime_r(&now, &local);
  int cur_min = local.tm_hour * 60 + local.tm_min;
  time_t day_start = now - (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
  time_t target = day_start + (g_ota_sched.window_start_min * 60);
  if ((cur_min * 60 + local.tm_sec) >= (g_ota_sched.window_start_min * 60)) {
    target += 86400;
  }
  uint16_t jitter_min = ota_sched_device_jitter_min();
  target += jitter_min * 60;
  return (uint32_t)target;
}

static void ota_sched_update_next_epoch(time_t now) {
  if (!g_ota_sched.enabled || now <= 0 || !sense_time_has_fresh_sync()) {
    return;
  }
  static bool applied_fresh_clock = false;
  if (!applied_fresh_clock) {
    g_next_ota_epoch = 0;  // discard a cached target after authoritative correction
    applied_fresh_clock = true;
  }
  ensure_timezone_pt("sched_update");
  if (OTA_SCHED_TEST_OFFSET_SEC > 0) {
    LOG_INFO("[OTA_SCHED] test_override offset_s=%d", OTA_SCHED_TEST_OFFSET_SEC);
  }
  if (g_next_ota_epoch == 0 || g_next_ota_epoch <= (uint32_t)now) {
    g_next_ota_epoch = ota_sched_compute_next_epoch(now);
    g_last_time_sync_epoch = (uint32_t)now;
    ota_sched_save();
    ota_sched_log_next_epoch(g_next_ota_epoch, "update");
  }
}

static bool maintenance_window_load(MaintenanceWindow* mw) {
  if (!mw) {
    return false;
  }
  if (!mw->loadFromNvs()) {
    return false;
  }
  if (!(mw->scheduled && mw->start_epoch > 0)) {
    return false;
  }
  // Reject AND clear an expired window at the single load chokepoint. Previously
  // only the sleep-scheduling path (maintenance_window_wake_delta_s) checked
  // expiry, so a stale window lingered in NVS whenever the device wasn't sleeping
  // -- and keep_lcd_awake_during_maint_arm() would keep reading it and spam
  // MAINT_KEEPALIVE, pinning the LCD fully lit forever (observed: a Sep-02 test
  // window "test_..." still held the LCD awake days later). clear() wipes the NVS
  // namespace, so this self-heals on the next load and never re-clears (a wiped
  // window fails loadFromNvs() above). Only acts when time is valid, so we never
  // discard a real window during the pre-NTP boot phase.
  if (is_time_valid()) {
    time_t now = time(nullptr);
    if (now > 0 && mw->hasExpired((uint64_t)now)) {
      mw->clear();
      maintenance_window_consumed_clear("expired_on_load");
      return false;
    }
  }
  return true;
}

static uint32_t maintenance_window_wake_delta_s(uint32_t* remaining_s_out) {
  if (remaining_s_out) {
    *remaining_s_out = 0;
  }
  if (!is_time_valid()) {
    return 0;
  }
  time_t now = time(nullptr);
  if (now <= 0) {
    return 0;
  }
  MaintenanceWindow mw;
  if (!maintenance_window_load(&mw)) {
    return 0;
  }
  uint64_t now_epoch = (uint64_t)now;
  if (mw.hasExpired(now_epoch)) {
    mw.clear();
    maintenance_window_consumed_clear("expired");
    return 0;
  }
  uint32_t wake_in_s = 0;
  uint64_t wake_epoch = mw.nextWakeEpochForSleep(now_epoch, 15, 5);
  if (wake_epoch > now_epoch) {
    wake_in_s = (uint32_t)(wake_epoch - now_epoch);
  }
  uint64_t window_end = mw.start_epoch + mw.duration_sec + mw.grace_after_sec;
  if (now_epoch >= mw.start_epoch && now_epoch <= window_end) {
    uint64_t remaining = (window_end > now_epoch) ? (window_end - now_epoch) : 0;
    if (remaining_s_out) {
      *remaining_s_out = (uint32_t)remaining;
    }
  }
  return wake_in_s;
}

void ota_schedule_update_from_mqtt(bool enable,
                                   int window_start_min,
                                   int window_dur_min,
                                   int jitter_min,
                                   int min_idle_min) {
#if !OTA_SCHED_ENABLED
  LOG_INFO("[OTA_SCHED] disabled compile_time ignore mqtt update");
  return;
#endif
  g_ota_sched.enabled = enable;
  g_ota_sched.window_start_min = (uint16_t)clamp_int(window_start_min, 0, 1439);
  g_ota_sched.window_dur_min = (uint16_t)clamp_int(window_dur_min, 1, 180);
  g_ota_sched.jitter_min = (uint8_t)clamp_int(jitter_min, 0, 30);
  g_ota_sched.min_idle_min = (uint8_t)clamp_int(min_idle_min, 0, 120);
  if (is_time_valid()) {
    time_t now = time(nullptr);
    g_next_ota_epoch = ota_sched_compute_next_epoch(now);
    g_last_time_sync_epoch = (uint32_t)now;
  }
  ota_sched_save();
  ota_sched_print_status("mqtt_sched_update");
}

static void ota_sched_configure_timer_wakeup() {
  g_sleep_timer_delta_s = 0;
  if (!sense_time_has_fresh_sync()) {
    g_timer_wake_armed = 0;
    LOG_INFO("[OTA_SCHED] unconfirmed_clock skip_absolute_timer");
    return;  // normal sleep still arms the shared relative six-hour fallback
  }
  MaintenanceWindow mw;
  bool maint_scheduled = maintenance_window_load(&mw);
  bool retry_pending = maintenance_followup_retry_pending();
  bool sched_compile_enabled = (OTA_SCHED_ENABLED != 0);
  bool sched_runtime_enabled = sched_compile_enabled && g_ota_sched.enabled;
  if (!sched_runtime_enabled && !maint_scheduled && !retry_pending) {
    g_timer_wake_armed = 0;
    LOG_INFO("[OTA_SCHED] no_schedule sched_compile=%d sched_enabled=%d maint_scheduled=0 retry_pending=0",
             sched_compile_enabled ? 1 : 0,
             g_ota_sched.enabled ? 1 : 0);
    // Forward to LCD
    char detail_ns[48];
    snprintf(detail_ns, sizeof(detail_ns), "compile=%d enabled=%d", sched_compile_enabled ? 1 : 0, g_ota_sched.enabled ? 1 : 0);
    uart_send_sense_diag("ota_sched", "no_schedule", "OTA_SCHED", 0, detail_ns);
    return;
  }
  if (!is_time_valid()) {
    if (!ota_sched_schedule_time_retry("time_invalid")) {
      LOG_INFO("[OTA_SCHED] time_invalid_retry_suppressed");
    }
    LOG_INFO("[OTA_SCHED] time_invalid skip_timer");
    return;
  }
  time_t now = time(nullptr);
  uint32_t retry_delta_s = 0;
  if (retry_pending && now > 0) {
    if (g_maint_followup_retry_wake_epoch > (uint32_t)now) {
      retry_delta_s = g_maint_followup_retry_wake_epoch - (uint32_t)now;
    } else {
      retry_delta_s = 5;
    }
  }
  uint32_t maint_remaining_s = 0;
  uint32_t maint_delta_s = maint_scheduled ? maintenance_window_wake_delta_s(&maint_remaining_s) : 0;
  bool maint_consumed = (maint_scheduled && now > 0)
                            ? maintenance_window_is_consumed(mw, (uint64_t)now)
                            : false;
  if (maint_delta_s == 0 && maint_remaining_s > 0) {
    if (maint_consumed) {
      LOG_INFO("[OTA_SCHED] in_window skip_short_wake (consumed=1) remaining_s=%lu",
               (unsigned long)maint_remaining_s);
    } else if (g_maintenance_handled) {
      // Maintenance ran this boot but window is still open -- arm a longer
      // re-check so the device wakes before the window closes (followup
      // retries, OTA apply, etc.).  Cap at remaining time.
      uint32_t recheck_s = (maint_remaining_s > 60) ? 60 : maint_remaining_s;
      if (recheck_s < 1) {
        recheck_s = 1;
      }
      maint_delta_s = recheck_s;
      LOG_INFO("[OTA_SCHED] in_window handled_recheck delta_s=%lu remaining_s=%lu",
               (unsigned long)maint_delta_s,
               (unsigned long)maint_remaining_s);
    } else {
      maint_delta_s = (maint_remaining_s > 10) ? 10 : maint_remaining_s;
      if (maint_delta_s < 1) {
        maint_delta_s = 1;
      }
      LOG_INFO("[OTA_SCHED] in_window schedule_short_wake delta_s=%lu remaining_s=%lu",
               (unsigned long)maint_delta_s,
               (unsigned long)maint_remaining_s);
    }
  }

  uint32_t sched_delta_s = 0;
  if (sched_runtime_enabled) {
    ota_sched_update_next_epoch(now);
    if (g_next_ota_epoch > (uint32_t)now) {
      sched_delta_s = g_next_ota_epoch - (uint32_t)now;
      if (sched_delta_s < 60) {
        sched_delta_s = 60;
      }
      if (sched_delta_s > 86400) {
        sched_delta_s = 86400;
      }
    }
  }

  uint32_t delta_s = 0;
  if (retry_delta_s > 0) {
    delta_s = retry_delta_s;
  }
  if (maint_delta_s > 0 && (delta_s == 0 || maint_delta_s < delta_s)) {
    delta_s = maint_delta_s;
  }
  if (sched_delta_s > 0 && (delta_s == 0 || sched_delta_s < delta_s)) {
    delta_s = sched_delta_s;
  }

  if (delta_s == 0) {
    g_timer_wake_armed = 0;
    LOG_INFO("[OTA_SCHED] no_timer retry_delta_s=%lu maint_delta_s=%lu sched_delta_s=%lu remaining_s=%lu maint_sched=%d handled=%d consumed=%d",
             (unsigned long)retry_delta_s,
             (unsigned long)maint_delta_s,
             (unsigned long)sched_delta_s,
             (unsigned long)maint_remaining_s,
             maint_scheduled ? 1 : 0,
             g_maintenance_handled ? 1 : 0,
             maint_consumed ? 1 : 0);
    return;
  }
  g_sleep_timer_delta_s = delta_s;
  g_timer_wake_armed = 1;
  esp_sleep_enable_timer_wakeup((uint64_t)delta_s * 1000000ULL);
  LOG_INFO("[OTA_SCHED] timer_arm delta_s=%lu retry_delta_s=%lu maint_delta_s=%lu sched_delta_s=%lu remaining_s=%lu sched_compile=%d sched_enabled=%d",
           (unsigned long)delta_s,
           (unsigned long)retry_delta_s,
           (unsigned long)maint_delta_s,
           (unsigned long)sched_delta_s,
           (unsigned long)maint_remaining_s,
           sched_compile_enabled ? 1 : 0,
           g_ota_sched.enabled ? 1 : 0);
  // Forward timer arm to LCD
  char detail[64];
  snprintf(detail, sizeof(detail), "delta_s=%lu enabled=%d", (unsigned long)delta_s, g_ota_sched.enabled ? 1 : 0);
  uart_send_sense_diag("ota_sched", "timer_arm", "OTA_SCHED", (int32_t)delta_s, detail);
}










static void mark_lcd_ota_still_pending(const char* reason) {
  LOG_INFO("[LCD_OTA_ORCH] pending request_id=%lu reason=%s",
           (unsigned long)g_lcd_ota_request_id,
           reason ? reason : "unknown");
  strncpy(g_lcd_ota_result, "pending", sizeof(g_lcd_ota_result) - 1);
  g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
  g_lcd_ota_done = true;
  g_lcd_ota_request_active = false;
}

static void set_lcd_ota_due_nvs(bool value) {
  Preferences p;
  if (p.begin("halo", false)) {
    if (value) {
      p.putUInt("lcd_ota_due", 1);
    } else {
      p.remove("lcd_ota_due");
    }
    p.end();
  }
}

static bool get_lcd_ota_due_nvs() {
  Preferences p;
  bool due = false;
  if (p.begin("halo", true)) {
    due = (p.getUInt("lcd_ota_due", 0) == 1);
    p.end();
  }
  return due;
}

// Persist an inline LCD OTA outcome: updated/target or noop/queried version.
// The Sense self-OTAs and reboots immediately after the inline proxy returns
// success, so RAM globals (g_lcd_ota_result / g_lcd_ota_version) are wiped
// before the post-reboot pre_sleep cloud OTA report is built. We stash the
// result here so load_lcd_ota_result_nvs() can repopulate those globals on the
// next boot. NVS key names MUST be <=15 chars (longer keys silently fail);
// "lcd_ota_res" (11) and "lcd_ota_ver" (11) are both within budget.
static void set_lcd_ota_result_nvs(const char* result, const char* version) {
  Preferences p;
  if (p.begin("halo", false)) {
    p.putString("lcd_ota_res", result ? result : "");
    p.putString("lcd_ota_ver", version ? version : "");
    p.end();
  }
}

// One-shot load of a persisted inline-LCD-OTA result into the RAM truth
// globals. Consumes (removes) the keys so a single success is reported exactly
// once. Returns true if a persisted result was found and loaded.
static bool load_lcd_ota_result_nvs() {
  Preferences p;
  bool loaded = false;
  if (p.begin("halo", false)) {
    if (p.isKey("lcd_ota_res")) {
      String res = p.getString("lcd_ota_res", "");
      String ver = p.getString("lcd_ota_ver", "");
      strncpy(g_lcd_ota_result, res.c_str(), sizeof(g_lcd_ota_result) - 1);
      g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
      strncpy(g_lcd_ota_version, ver.c_str(), sizeof(g_lcd_ota_version) - 1);
      g_lcd_ota_version[sizeof(g_lcd_ota_version) - 1] = '\0';
      p.remove("lcd_ota_res");
      p.remove("lcd_ota_ver");
      loaded = true;
      LOG_INFO("[OTA_REPORT] loaded persisted lcd result=%s ver=%s",
               g_lcd_ota_result, g_lcd_ota_version);
    }
    p.end();
  }
  return loaded;
}


// ── Bounded automatic OTA check ─────────────────────────────────────
// A real timer boot checks for an update. A persisted LCD debt also earns one
// recovery check on boot. Both use the existing OTA implementation and normal
// idle sleep; neither bypasses user work, provisioning, or OTA safety guards.
bool halo_prod_boot_ota_pending() {
  return (g_boot_ota_pending && (int32_t)(millis() - g_boot_ota_deadline_ms) < 0) ||
         (g_control_probe_active && (int32_t)(millis() - g_control_probe_deadline_ms) < 0) ||
         (g_peer_gate.active && !g_peer_gate.entered &&
          (int32_t)(millis() - g_peer_gate.deadline_ms) < 0);
}

static void boot_ota_queue(const char* reason) {
  if (g_boot_ota_pending) return;  // Preserve the actual timer trigger if both apply.
  g_boot_ota_pending = true;
  g_boot_ota_deadline_ms = millis() + 120000UL;
  g_boot_ota_next_try_ms = 0;
  g_boot_ota_time_sync_started = false;
  g_boot_ota_begin_reported = false;
  g_boot_ota_begin_record = {};
  g_boot_ota_diag_context = false;
  g_boot_ota_reason = reason;
  const bool nightly = strcmp(reason, "nightly") == 0;
  g_boot_ota_begin_record.event = nightly ? "nightly_begin" :
      (strcmp(reason, "lcd_timer") == 0 ? "lcd_timer_begin" : "lcd_recovery_begin");
  g_boot_ota_begin_record.reason = reason;
  g_boot_ota_begin_record.code = (int32_t)esp_sleep_get_wakeup_cause();
  snprintf(g_boot_ota_begin_record.detail, sizeof(g_boot_ota_begin_record.detail),
           "wake=%d reset=%d epoch=%lu reason=%s",
           (int)g_boot_ota_begin_record.code, (int)esp_reset_reason(),
           (unsigned long)sense_now_epoch(), reason);
  g_boot_ota_begin_record.repeat_pending = true;
  // Keep the last true TIMER origin even if a later EXT0/debt boot replaces
  // this RAM episode. Recovery diagnostics never relabel their current cause.
  if (nightly) {
    Preferences p;
    if (p.begin("ota_coord", false)) {
      p.putString("sense_origin", g_boot_ota_begin_record.detail);
      p.end();
    }
  }
  Serial.printf("[BOOT_OTA] check pending reason=%s budget_ms=120000\n", reason);
}

static void boot_ota_finish(const char* result) {
  g_boot_ota_pending = false;
  g_boot_ota_begin_record.repeat_pending = false;
  g_boot_ota_begin_record.noop_reported = false;
  g_boot_ota_diag_context = false;
  g_maintenance_mode = false;
  g_maintenance_handled = true;
  Serial.printf("[BOOT_OTA] finished reason=%s result=%s\n", g_boot_ota_reason, result);
}


// UART dispatch only publishes a bounded mailbox. NVS and coordinator decisions
// remain on the main task; announcements cannot run HTTP or renew a lease.
void halo_prod_note_lcd_timer(const char* schedule_id, uint32_t boot_id, int wake,
                              int reset, uint32_t epoch, bool resumed) {
  if (!schedule_id || !schedule_id[0] || strlen(schedule_id) >= sizeof(g_lcd_timer_notice.schedule) ||
      !boot_id || wake != (int)ESP_SLEEP_WAKEUP_TIMER || g_lcd_timer_notice.pending) return;
  strlcpy(g_lcd_timer_notice.schedule, schedule_id, sizeof(g_lcd_timer_notice.schedule));
  g_lcd_timer_notice.boot_id = boot_id;
  g_lcd_timer_notice.wake = wake;
  g_lcd_timer_notice.reset = reset;
  g_lcd_timer_notice.epoch = epoch;
  g_lcd_timer_notice.resumed = resumed;
  g_lcd_timer_notice.pending = true;
}

static void ota_peer_send_lock(bool release) {
  // An unconfirmed binary-to-JSON handoff permits local teardown only. A
  // later independent nonblocking challenge must prove control readiness.
  if (!sense_lcd_ota_retry_safe()) return;
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = release ? "OTA_UNLOCK" : "OTA_LOCK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["coord_id"] = g_peer_gate.owner;
  if (release) doc["terminal"] = true;
  else {
    const int32_t remaining = (int32_t)(g_peer_gate.deadline_ms - millis());
    if (remaining <= 0) return;
    doc["lease_ms"] = (uint32_t)remaining;
    doc["peer_boot_id"] = g_peer_gate.peer_boot;
    doc["sense_boot_id"] = g_coord_sense_boot_id;
    doc["coord_seq"] = g_peer_gate.sequence;
  }
  String output; serializeJson(doc, output); uart_send_json(output.c_str());
}

static void ota_peer_cancel(const char* reason) {
  if (g_peer_gate.locked) ota_peer_send_lock(true);
  g_peer_gate.active = false;
  g_peer_gate.ready = false;
  g_peer_gate.querying = false;
  g_peer_gate.locked = false;
  Serial.printf("[OTA_PEER] finished reason=%s\n", reason);
}

static bool ota_peer_schedule_completed(const char* id) {
  if (!id || !id[0]) return false;
  if (strcmp(id, g_coord_completed) == 0) return true; // old-key migration
  for (uint8_t i = 0; i < g_coord_completed_count; ++i)
    if (strcmp(id, g_coord_completed_ids[i]) == 0) return true;
  return false;
}

static bool ota_peer_continuation() {
  return g_peer_continue_work && g_lcd_work_budget_live && g_lcd_work_budget.remaining_ms() &&
      g_boot_ota_pending && strcmp(g_boot_ota_reason, "lcd_timer") == 0 &&
      g_coord_pending[0] && strcmp(g_coord_pending, g_lcd_work_schedule) == 0;
}

static void ota_peer_load_completed_history(const char* encoded) {
  if (!encoded || !encoded[0] || strlen(encoded) >= 512) return;
  char parsed[8][64] = {{0}};
  uint8_t count = 0;
  const char* cursor = encoded;
  while (*cursor) {
    const char* end = strchr(cursor, '\n');
    const size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
    if (!len || len >= 64 || count == 8 || memchr(cursor, '\r', len)) return;
    memcpy(parsed[count++], cursor, len);
    if (!end) break;
    cursor = end + 1;
    if (!*cursor) return;
  }
  memcpy(g_coord_completed_ids, parsed, sizeof(parsed));
  g_coord_completed_count = count;
  strlcpy(g_coord_completed, parsed[0], sizeof(g_coord_completed));
}

static void ota_peer_schedule_complete() {
  if (!g_coord_pending[0]) return;
  // Keep exact recent IDs, not a date high-water mark: a bad future clock
  // must not suppress a legitimate nightly check after fresh correction.
  if (strchr(g_coord_pending, '\n') || strchr(g_coord_pending, '\r')) return;
  char next[8][64] = {{0}};
  strlcpy(next[0], g_coord_pending, sizeof(next[0]));
  uint8_t count = 1;
  for (uint8_t i = 0; i <= g_coord_completed_count && count < 8; ++i) {
    const char* id = i == g_coord_completed_count ? g_coord_completed : g_coord_completed_ids[i];
    if (!id[0] || strchr(id, '\n') || strchr(id, '\r')) continue;
    bool duplicate = false;
    for (uint8_t j = 0; j < count; ++j) if (strcmp(next[j], id) == 0) duplicate = true;
    if (!duplicate) strlcpy(next[count++], id, sizeof(next[0]));
  }
  char encoded[512] = {0};
  for (uint8_t i = 0; i < count; ++i) {
    if (i) strlcat(encoded, "\n", sizeof(encoded));
    strlcat(encoded, next[i], sizeof(encoded));
  }
  Preferences p;
  if (p.begin("ota_coord", false)) {
    // Opening NVS can block too. Do not begin a new completion commit after
    // the current work budget expires; a successful write below is final.
    if (g_lcd_work_budget_live && !g_lcd_work_budget.remaining_ms()) {
      p.end();
      return;
    }
    // The entire eight-ID history is one atomic NVS value. Publish RAM only
    // after it succeeds; a reset before pending removal still finds the ID.
    if (p.putString("done_ids", encoded) == strlen(encoded)) {
      memcpy(g_coord_completed_ids, next, sizeof(next));
      g_coord_completed_count = count;
      strlcpy(g_coord_completed, g_coord_pending, sizeof(g_coord_completed));
      g_coord_pending[0] = 0;
      g_lcd_work_budget_live = false; g_peer_continue_work = false;
      p.remove("pending");
      p.remove("target");
    }
    p.end();
  }
}

static bool ota_peer_ready() {
  if (g_peer_episode_finished) return false;
  if (!g_peer_gate.active) {
    g_peer_gate = {};
    g_peer_gate.active = true;
    g_peer_gate.sequence = ++g_coord_sequence;
    if (!g_peer_gate.sequence) g_peer_gate.sequence = ++g_coord_sequence;
    g_peer_gate.deadline_ms = g_boot_ota_pending ? g_boot_ota_deadline_ms : millis() + 120000UL;
    snprintf(g_peer_gate.owner, sizeof(g_peer_gate.owner), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
  }
  ota_peer_service();
  return g_peer_gate.active && g_peer_gate.ready &&
         (int32_t)(millis() - g_peer_gate.deadline_ms) < 0;
}

static void ota_control_probe_service() {
  if (sense_lcd_ota_retry_safe()) { g_control_probe_active = false; return; }
  if (g_peer_gate.active || g_boot_ota_pending || g_ota_check_requested ||
      g_ota_check_in_progress || g_ota_apply_in_progress || g_lcd_ota_task_running) {
    // A real request owns the same query mailbox and its own original budget.
    if (g_control_probe_active) g_control_probe_finished = true;
    g_control_probe_active = false; g_control_probe_querying = false;
    return;
  }
  if (g_control_probe_finished || g_peer_episode_finished) return;
  const uint32_t now = millis();
  if (!g_control_probe_active) {
    g_control_probe_active = true;
    g_control_probe_deadline_ms = now + 120000UL;
  }
  if ((int32_t)(now - g_control_probe_deadline_ms) >= 0) {
    g_control_probe_active = false; g_control_probe_querying = false;
    g_control_probe_finished = true;
    set_lcd_ota_due_nvs(true);
    return;
  }
  if (g_lcd_ota_proxy_owns_uart || sense_action_inflight() || foreground_active ||
      voice_recording_active || g_list_screen_active || g_provisioning_manager.isSetupModeActive()) return;
  if (g_control_probe_querying) {
    LcdOtaQuerySnapshot snapshot;
    const LcdOtaQueryPoll result = sense_lcd_ota_query_poll(snapshot);
    if (result == LCD_QUERY_WAITING) return;
    g_control_probe_querying = false; g_control_probe_next_ms = now + 200;
    if (result == LCD_QUERY_READY) {
      g_control_probe_active = false; g_control_probe_finished = true;
    }
    return;
  }
  if ((int32_t)(now - g_control_probe_next_ms) < 0) return;
  char challenge[40];
  snprintf(challenge, sizeof(challenge), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  const uint32_t remaining = g_control_probe_deadline_ms - now;
  g_control_probe_querying = sense_lcd_ota_query_start(challenge, remaining < 3000 ? remaining : 3000);
}

static void ota_peer_service() {
  ota_control_probe_service();
  // Do not consume/mark a new boot seen while the old transaction is busy or
  // waiting to run its deadline teardown. The next loop can accept it after
  // that teardown, preserving the late peer's independent opportunity.
  const uint32_t notice_now = millis();
  const bool old_wait_expired =
      (g_boot_ota_pending && (int32_t)(notice_now - g_boot_ota_deadline_ms) >= 0) ||
      (g_peer_gate.active && !g_peer_gate.entered &&
       (int32_t)(notice_now - g_peer_gate.deadline_ms) >= 0);
  if (g_lcd_timer_notice.pending && !old_wait_expired &&
      !g_ota_check_in_progress && !g_ota_apply_in_progress && !g_lcd_ota_task_running &&
      !(g_peer_gate.active && g_peer_gate.entered)) {
    g_lcd_timer_notice.pending = false;
    const bool expected =
        ((g_coord_pending[0] && strcmp(g_lcd_timer_notice.schedule, g_coord_pending) == 0) ||
         (g_coord_schedule[0] && strcmp(g_lcd_timer_notice.schedule, g_coord_schedule) == 0)) &&
        !ota_peer_schedule_completed(g_lcd_timer_notice.schedule);
    bool seen = false;
    for (uint8_t i = 0; i < g_coord_seen_count; ++i) {
      if (g_coord_seen_boots[i] == g_lcd_timer_notice.boot_id) seen = true;
    }
    if (expected && !seen && g_coord_seen_count < 3 &&
        !g_ota_check_in_progress && !g_ota_apply_in_progress && !g_lcd_ota_task_running) {
      g_coord_seen_boots[g_coord_seen_count++] = g_lcd_timer_notice.boot_id;
      g_lcd_timer_seen_boot = g_lcd_timer_notice.boot_id;
      g_lcd_timer_origin = g_lcd_timer_notice;
      if (!g_coord_pending[0]) {
        strlcpy(g_coord_pending, g_lcd_timer_notice.schedule, sizeof(g_coord_pending));
        Preferences p;
        if (p.begin("ota_coord", false)) { p.putString("pending", g_coord_pending); p.end(); }
      }
      if (!g_boot_ota_pending && !g_peer_gate.active) {
        const bool same_work = g_lcd_work_budget_live && g_lcd_work_schedule[0] &&
            strcmp(g_lcd_timer_notice.schedule, g_lcd_work_schedule) == 0;
        if (same_work && (!g_lcd_work_budget.remaining_ms() ||
                          g_lcd_timer_notice.boot_id == g_lcd_work_peer_boot)) return;
        g_peer_continue_work = same_work;
        g_peer_episode_finished = false;
        g_ota_check_done = false;
        boot_ota_queue("lcd_timer");
      }
      // A pending true Sense TIMER keeps its original kind and deadline.
      if (g_peer_gate.active && g_peer_gate.peer_boot &&
          g_peer_gate.peer_boot != g_lcd_timer_notice.boot_id) {
        const uint32_t original_deadline = g_peer_gate.deadline_ms;
        ota_peer_cancel("peer_reboot");
        g_peer_gate = {};
        g_peer_gate.active = true;
        g_peer_gate.sequence = ++g_coord_sequence;
        if (!g_peer_gate.sequence) g_peer_gate.sequence = ++g_coord_sequence;
        g_peer_gate.deadline_ms = original_deadline;
        snprintf(g_peer_gate.owner, sizeof(g_peer_gate.owner), "%08lx%08lx",
                 (unsigned long)esp_random(), (unsigned long)esp_random());
      }
    }
  }

  if (!g_peer_gate.active || g_peer_gate.entered) return;
  // Notice persistence can block; never use its entry timestamp as remaining
  // readiness time after that work returns.
  const uint32_t now = millis();
  if ((int32_t)(now - g_peer_gate.deadline_ms) >= 0 ||
      (g_peer_continue_work && !g_lcd_work_budget.remaining_ms())) {
    ota_peer_cancel("readiness_deadline");
    g_peer_episode_finished = true;
    set_lcd_ota_due_nvs(true);
    OtaIntent::clearForceAndCheck();
    ota_set_last_result("peer_unavailable");
    g_ota_check_done = true;
    g_ota_check_requested = false;
    manual_ota_override_clear("peer_unavailable");
    return;
  }
  if (sense_action_inflight() || foreground_active || voice_recording_active ||
      g_list_screen_active || g_provisioning_manager.isSetupModeActive()) {
    // User work releases preflight ownership; the original budget continues.
    if (g_peer_gate.locked) {
      ota_peer_send_lock(true);
      g_peer_gate.locked = false; g_peer_gate.ready = false;
    }
    return;
  }
  if (g_lcd_ota_proxy_owns_uart || g_lcd_ota_task_running) return;
  if (g_peer_gate.ready && (uint32_t)(now - g_peer_gate.proof_ms) < 2000) return;
  g_peer_gate.ready = false;
  if (g_peer_gate.querying) {
    LcdOtaQuerySnapshot snapshot;
    const LcdOtaQueryPoll result = sense_lcd_ota_query_poll(snapshot);
    if (result == LCD_QUERY_WAITING) return;
    g_peer_gate.querying = false;
    g_peer_gate.next_query_ms = now + 200;
    if (result != LCD_QUERY_READY) return;
    if (snapshot.correlated && !snapshot.peer_boot_id) return;
    if (g_boot_ota_pending && strcmp(g_boot_ota_reason, "lcd_timer") == 0 &&
        (!snapshot.correlated || snapshot.peer_boot_id != g_lcd_timer_seen_boot ||
         (!snapshot.coord_waiting && !g_peer_gate.locked))) return;
    if (g_peer_gate.peer_boot && snapshot.correlated && snapshot.peer_boot_id != g_peer_gate.peer_boot) {
      g_peer_gate.peer_boot = 0; g_peer_gate.locked = false;
      g_peer_gate.sequence = ++g_coord_sequence;
      if (!g_peer_gate.sequence) g_peer_gate.sequence = ++g_coord_sequence;
      snprintf(g_peer_gate.owner, sizeof(g_peer_gate.owner), "%08lx%08lx",
               (unsigned long)esp_random(), (unsigned long)esp_random());
      return;
    }
    g_peer_gate.peer_boot = snapshot.peer_boot_id;
    g_peer_gate.legacy = !snapshot.correlated;
    if (!g_peer_gate.locked) {
      ota_peer_send_lock(false);
      g_peer_gate.locked = true;
      // Legacy14 has no ownership echo. A fresh VALID/ready reply is its
      // explicit compatibility proof; its existing lock has a finite hold.
      if (!g_peer_gate.legacy) return;
    } else if (!g_peer_gate.legacy &&
               (strcmp(snapshot.coord_owner, g_peer_gate.owner) != 0 || snapshot.coord_lease_ms == 0 ||
                snapshot.coord_lease_ms > 120000)) return;
    g_peer_gate.ready = true;
    g_peer_gate.proof_ms = now;
    Serial.printf("[OTA_PEER] ready boot=%lu correlated=%d\n",
                  (unsigned long)snapshot.peer_boot_id, snapshot.correlated ? 1 : 0);
    return;
  }
  if ((int32_t)(now - g_peer_gate.next_query_ms) < 0) return;
  snprintf(g_peer_gate.challenge, sizeof(g_peer_gate.challenge), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  const uint32_t left = g_peer_gate.deadline_ms - now;
  g_peer_gate.querying = sense_lcd_ota_query_start(g_peer_gate.challenge, left < 3000 ? left : 3000);
}

struct OtaPeerTransactionCleanup {
  ~OtaPeerTransactionCleanup() {
    g_peer_episode_finished = true;
    ota_peer_cancel("transaction_return");
  }
};

// Called only after a fresh LCD query succeeds, before binary OTA takes UART.
// A manual check can run while an earlier automatic episode remains pending;
// only the synchronous automatic invocation may repeat its cached trigger.
static void boot_ota_repeat_begin_after_lcd_query() {
  if (!g_boot_ota_diag_context || !g_boot_ota_begin_record.repeat_pending) return;
  g_boot_ota_begin_record.repeat_pending = false;
  uart_send_sense_diag_persist("ota", g_boot_ota_begin_record.event,
                              g_boot_ota_begin_record.reason,
                              g_boot_ota_begin_record.code,
                              g_boot_ota_begin_record.detail);
}

// Only the initial both-current branch calls this, after fresh LCD identity and
// manifest validation. A begin/replay alone cannot prove a successful no-op.
static void boot_ota_report_both_current(const char* lcd_fw, const char* manifest_fw) {
  if (!g_boot_ota_diag_context || !g_boot_ota_begin_reported ||
      g_boot_ota_begin_record.noop_reported || !g_boot_ota_begin_record.event ||
      !g_boot_ota_begin_record.reason) return;
  const bool sense_timer = strcmp(g_boot_ota_begin_record.event, "nightly_begin") == 0 &&
      strcmp(g_boot_ota_begin_record.reason, "nightly") == 0 &&
      g_boot_ota_begin_record.code == (int32_t)ESP_SLEEP_WAKEUP_TIMER;
  const bool lcd_timer = strcmp(g_boot_ota_begin_record.event, "lcd_timer_begin") == 0 &&
      strcmp(g_boot_ota_begin_record.reason, "lcd_timer") == 0;
  if (!sense_timer && !lcd_timer) return;
  // Both version inputs have 32-byte storage. Preserve the original trigger
  // detail verbatim; do not sample a later wake/reset/epoch at completion.
  char detail[192];
  snprintf(detail, sizeof(detail), "%s lcd=%.31s manifest=%.31s",
           g_boot_ota_begin_record.detail, lcd_fw, manifest_fw);
  g_boot_ota_begin_record.noop_reported = true;
  uart_send_sense_diag_persist("ota", sense_timer ? "nightly_noop" : "lcd_timer_noop", "both_current",
                              g_boot_ota_begin_record.code, detail);
}

// Call once early in setup. USB reset / touch wake is not a timer wake.
static void nightly_maintenance_note_wake() {
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    boot_ota_queue("nightly");
  }
}

// Called after normal provisioning/time/health service, independent of the old
// maintenance flags. A readiness skip keeps the request alive until the bound;
// a check that began consumes it even if its manifest fetch or update failed.
static void nightly_maintenance_tick() {
  if (!g_boot_ota_pending) return;
  if (g_coord_completion_target[0] && strcmp(g_coord_completion_target, kFirmwareVersion) == 0 &&
      g_ota_pending_verify_active) return;
  const bool nightly = strcmp(g_boot_ota_reason, "nightly") == 0;
  if (g_ota_check_done) {
    boot_ota_finish("check_already_started");
    return;
  }
  const uint32_t now_ms = millis();
  if ((int32_t)(now_ms - g_boot_ota_deadline_ms) >= 0) {
    uart_send_sense_diag("ota", nightly ? "nightly_skip" : "lcd_recovery_skip",
                         g_boot_ota_reason, 0, "readiness_deadline");
    ota_peer_cancel("readiness_deadline");
    g_peer_episode_finished = true;
    g_ota_check_done = true;
    set_lcd_ota_due_nvs(true);
    boot_ota_finish("readiness_deadline");
    return;
  }
  if (g_boot_ota_next_try_ms != 0 &&
      (int32_t)(now_ms - g_boot_ota_next_try_ms) < 0) return;
  g_boot_ota_next_try_ms = now_ms + 1000UL;

  // Automatic work must not take the camera/mic/network from a person or run
  // beside SoftAP provisioning. The true argument below only skips boot delay;
  // it does not set a force/manual intent.
  if (sense_action_inflight() || foreground_active || voice_recording_active ||
      g_list_screen_active ||
      (op_queue != nullptr && uxQueueMessagesWaiting(op_queue) > 0) ||
      g_provisioning_manager.isSetupModeActive() ||
      g_ota_check_in_progress || g_ota_apply_in_progress ||
      g_lcd_ota_task_running || g_lcd_ota_proxy_owns_uart) return;
  if (!g_boot_ota_begin_reported && sense_lcd_ota_retry_safe()) {
    g_boot_ota_begin_reported = true;
    if (strcmp(g_boot_ota_reason, "nightly") != 0) {
      Preferences p;
      if (p.begin("ota_coord", true)) {
        const String previous = p.getString("sense_origin", "");
        if (previous.length()) uart_send_sense_diag_persist("ota", "previous_timer_origin",
                                                         "retained", 0, previous.c_str());
        p.end();
      }
    }
    if (strcmp(g_boot_ota_reason, "lcd_timer") == 0) {
      char origin[160];
      snprintf(origin, sizeof(origin), "schedule=%.63s lcd_wake=%d lcd_reset=%d lcd_epoch=%lu boot=%lu resumed=%d",
               g_lcd_timer_origin.schedule, g_lcd_timer_origin.wake, g_lcd_timer_origin.reset,
               (unsigned long)g_lcd_timer_origin.epoch, (unsigned long)g_lcd_timer_seen_boot,
               g_lcd_timer_origin.resumed ? 1 : 0);
      uart_send_sense_diag_persist("ota", "lcd_timer_origin", "lcd_timer", g_lcd_timer_origin.wake, origin);
    }
    uart_send_sense_diag_persist("ota", g_boot_ota_begin_record.event,
                                g_boot_ota_begin_record.reason,
                                g_boot_ota_begin_record.code,
                                g_boot_ota_begin_record.detail);
  }
  if (!ota_peer_ready()) return;
  // A mode probe may have made UART safe in this tick. Publish the retained
  // original begin on the next tick before any manifest/no-op work starts.
  if (!g_boot_ota_begin_reported) return;
  if (!ProvisioningState::isProvisioned() ||
      ProvisioningState::getState() != ProvisioningState::STATE_CONNECTED ||
      !wifiReadyForHttps()) return;

  // Start SNTP once and let regular loop service progress while time becomes
  // valid. Repeating a blocking wait here would starve health and user input.
  if (!g_boot_ota_time_sync_started) {
    g_boot_ota_time_sync_started = true;
    halo_prod_kick_time_sync(g_boot_ota_reason);
  }
  // Give the nonblocking fresh-time attempt its bounded chance before long
  // HTTPS work. An unavailable NTP server must not suppress already-due OTA:
  // after the deadline, a plausible retained clock remains sufficient for TLS.
  if (sense_ntp_attempt_pending()) return;
  if (!is_time_valid() || (!OtaIntent::cooldownAllows() && !ota_peer_continuation())) return;


  g_boot_ota_diag_context = true;
  maybeRunOtaCheck(g_boot_ota_reason, true);
  g_boot_ota_diag_context = false;
  if (g_ota_check_done) {
    uart_send_sense_diag("ota", nightly ? "nightly_done" : "lcd_recovery_done",
                         g_boot_ota_reason, 1, "check_started");
    boot_ota_finish("check_started");
  }
}


void halo_prod_pre_sleep() {
  LOG_INFO("[PRE_SLEEP] window start");
  const unsigned long pre_sleep_budget_ms = 20000;
  const unsigned long pre_sleep_start_ms = millis();
  auto remaining_budget_ms = [&]() -> unsigned long {
    unsigned long elapsed = millis() - pre_sleep_start_ms;
    if (elapsed >= pre_sleep_budget_ms) {
      return 0;
    }
    return pre_sleep_budget_ms - elapsed;
  };
  if (sense_action_inflight()) {
    LOG_INFO("[PRE_SLEEP] skip (action_inflight)");
    return;
  }
  bool ota_needed = OtaIntent::shouldUpdateNow() ||
                    g_ota_check_requested ||
                    g_ota_check_in_progress ||
                    g_ota_apply_in_progress ||
                    OtaIntent::getOtaIntentActive();
  bool schedule_fetch_needed = ota_sched_http_configured();
  bool report_needed = (OTA_REPORT_HTTP_URL[0] != '\0');
  bool lcd_sync_needed = maintenance_schedule_pending_sync_to_lcd();
  bool needs_work = ota_needed || schedule_fetch_needed || report_needed || lcd_sync_needed;
  if (!needs_work) {
    LOG_INFO("[PRE_SLEEP] skip (no work)");
    return;
  }
  // Abort pre-sleep if LCD is pulsing the wake pin (user action pending)
  if (wake_pin_check_pulsing(250)) {
    LOG_INFO("[PRE_SLEEP] abort (lcd_pulsing)");
    return;
  }
  // Keep the sleep path quiet; maintenance scheduling is HTTP-based now.
  mqtt_set_allowed(false);
  bool need_http = ota_needed || schedule_fetch_needed || report_needed;
  if (need_http) {
    unsigned long now_ms = millis();
    if (!wifi_is_connected() &&
        wifi_last_fail_ms() > 0 &&
        (now_ms - wifi_last_fail_ms()) < WIFI_FAIL_COOLDOWN_MS) {
      LOG_INFO("[PRE_SLEEP] skip http (wifi cooldown) age_ms=%lu reason=%s",
               (unsigned long)(now_ms - wifi_last_fail_ms()),
               wifi_last_fail_reason());
      return;
    }
    uint32_t budget_ms = (uint32_t)remaining_budget_ms();
    if (budget_ms < 1000) {
      LOG_INFO("[PRE_SLEEP] skip http (budget_exhausted)");
      return;
    }
    uint32_t wifi_timeout_ms = 10000;  // 10s — WiFi often takes 5-8s on cold boot
    if (budget_ms < wifi_timeout_ms) {
      wifi_timeout_ms = budget_ms;
    }
    if (!ensure_wifi_connected_for_sleep(wifi_timeout_ms, budget_ms)) {
      LOG_INFO("[PRE_SLEEP] skip http (wifi not connected)");
      return;
    }
    if (!is_time_valid()) {
      LOG_INFO("[PRE_SLEEP] time invalid; syncing");
      budget_ms = (uint32_t)remaining_budget_ms();
      if (budget_ms < 1000) {
        LOG_INFO("[PRE_SLEEP] skip http (budget_exhausted)");
        return;
      }
      uint32_t time_sync_ms = budget_ms < 4000 ? budget_ms : 4000;
      ensure_time_valid("pre_sleep_http", time_sync_ms);
      budget_ms = (uint32_t)remaining_budget_ms();
      if (budget_ms < 1000) {
        LOG_INFO("[PRE_SLEEP] skip http (budget_exhausted)");
        return;
      }
      uint32_t wait_ms = budget_ms < 8000 ? budget_ms : 8000;
      if (!wait_for_time_valid(wait_ms)) {
        LOG_INFO("[PRE_SLEEP] skip http (time invalid)");
        return;
      }
    }
  }
  maintenance_resync_on_time_jump("pre_sleep");
  // Abort pre-sleep if LCD is pulsing the wake pin (user action pending)
  if (wake_pin_check_pulsing(250)) {
    LOG_INFO("[PRE_SLEEP] abort (lcd_pulsing before sched_fetch)");
    return;
  }
  {
    uint32_t remaining_ms = (uint32_t)remaining_budget_ms();
    if (remaining_ms >= 1000 && schedule_fetch_needed) {
      uint32_t http_timeout_ms = OTA_SCHED_HTTP_TIMEOUT_MS;
      if (remaining_ms < http_timeout_ms) {
        http_timeout_ms = remaining_ms;
      }
      if (http_timeout_ms >= 1000) {
      } else {
        ota_http_schedule_note("budget_low", 0, nullptr);
      }
    } else if (schedule_fetch_needed) {
      ota_http_schedule_note("budget_low", 0, nullptr);
    }
  }
  if (need_http && wifi_is_connected()) {
    IPAddress ip = WiFi.localIP();
    IPAddress dns = WiFi.dnsIP(0);
    LOG_INFO("[PRE_SLEEP] net_ready ip=%s dns=%s rssi=%d time=%ld",
             ip.toString().c_str(),
             dns.toString().c_str(),
             WiFi.RSSI(),
             static_cast<long>(time(nullptr)));
  }
  sync_pending_maintenance_to_lcd("pre_sleep");
  while (maintenance_schedule_pending_sync_to_lcd() &&
         !g_lcd_maint_ack_received &&
         g_maint_sync_send_count < MAINT_SYNC_MAX_SENDS) {
    uint32_t budget_ms = (uint32_t)remaining_budget_ms();
    unsigned long retry_wait_ms = MAINT_SYNC_RESEND_MS + 100;
    if (budget_ms < (retry_wait_ms + 500)) {
      break;
    }
    LOG_INFO("[MAINT_SYNC] retry_before_sleep wait_ms=%lu attempt=%u",
             retry_wait_ms,
             (unsigned)(g_maint_sync_send_count + 1));
    // Poll UART during wait so we can receive LCD's ACK instead of
    // letting it pile up in the hardware buffer during a blocking delay
    {
      unsigned long wait_start = millis();
      while ((millis() - wait_start) < retry_wait_ms) {
        pump_uart_rx_once();
        if (g_lcd_maint_ack_received) {
          LOG_INFO("[MAINT_SYNC] ack_received_during_wait elapsed=%lums",
                   millis() - wait_start);
          break;
        }
        delay(50);
      }
    }
    sync_pending_maintenance_to_lcd("pre_sleep_retry");
  }
  if (maintenance_schedule_pending_sync_to_lcd() && !g_lcd_maint_ack_received) {
    if (g_maint_sync_send_count < MAINT_SYNC_MAX_SENDS) {
      g_maint_sync_wait_until_ms = millis() + MAINT_SYNC_BLOCK_SLEEP_MS;
      maint_sync_note_resolution("waiting_for_ack", g_maint_pending_request_id);
    } else {
      g_maint_sync_wait_until_ms = 0;
    }
  } else {
    g_maint_sync_wait_until_ms = 0;
  }

  // Query LCD for its REAL running firmware version (for OTA report).
  // The cloud-reported lcd_fw must come from an actual LCD_OTA_QUERY_RESP,
  // never from an assumed manifest version. Refresh when:
  //   - the cache is empty (e.g. just cleared after a successful LCD OTA), or
  //   - the cached value is stale (older than LCD_FW_QUERY_STALE_MS),
  // provided the UART link is recent and no OTA proxy is in flight.
  {
    bool empty = (g_lcd_ota_version[0] == '\0');
    bool stale = (g_lcd_fw_query_ms == 0) ||
                 ((millis() - g_lcd_fw_query_ms) > LCD_FW_QUERY_STALE_MS);
    if ((empty || stale) && !g_lcd_ota_task_running &&
        halo_uart_link_recent(3000)) {
      char lcd_fw_buf[32] = {0};
      if (sense_lcd_ota_query(lcd_fw_buf, sizeof(lcd_fw_buf), nullptr)) {
        strncpy(g_lcd_ota_version, lcd_fw_buf, sizeof(g_lcd_ota_version) - 1);
        g_lcd_ota_version[sizeof(g_lcd_ota_version) - 1] = '\0';
        g_lcd_fw_query_ms = millis();
        LOG_INFO("[PRE_SLEEP] lcd_fw queried (real): %s", g_lcd_ota_version);
      } else if (empty) {
        LOG_INFO("[PRE_SLEEP] lcd_fw query failed; cache remains empty");
      }
    }
  }

  dump_system_truth("pre_sleep");
  // Abort pre-sleep if LCD is pulsing the wake pin (user action pending)
  if (wake_pin_check_pulsing(250)) {
    LOG_INFO("[PRE_SLEEP] abort (lcd_pulsing before report)");
    return;
  }
  {
    uint32_t report_timeout_ms = (uint32_t)remaining_budget_ms();
    if (report_timeout_ms > OTA_REPORT_HTTP_TIMEOUT_MS) {
      report_timeout_ms = OTA_REPORT_HTTP_TIMEOUT_MS;
    }
    if (report_timeout_ms >= 500) {
      ota_report_post_pre_sleep(report_timeout_ms);
    } else {
      LOG_INFO("[OTA_REPORT] skip pre_sleep (budget_exhausted)");
    }
  }
  // Drain UART before OTA policy check — INPUT_OTA_CHECK from LCD
  // may have arrived while we were doing HTTP work
  pump_uart_rx_once();
  // The first drain may have sent SYNC_ACK/FW_INFO to LCD, triggering
  // LCD to resend INPUT_OTA_CHECK. Wait briefly for that round-trip.
  if (!halo_ota_manual_override_active()) {
    delay(150);
    pump_uart_rx_once();
  }

  bool ota_allowed = SenseOtaPolicy::allowOtaWorkNow("pre_sleep");
  // An EXPLICIT request must run the check even when nothing is "desired" yet.
  //
  // This used to be gated on OtaIntent::shouldUpdateNow() alone, which is
  // `desired_sense > current`. desired_sense only ever gets set by the cloud, so
  // the device would fetch the manifest ONLY if it already knew a newer version
  // existed -- and the manifest is the thing that would have told it. Circular:
  // nothing could ever discover an update on this path.
  //
  // Worse, it failed SILENTLY. `ota_allowed` is true, so the else-if below did
  // not fire either; the log showed "[OTA_POLICY] allow=1 why=pre_sleep" and then
  // nothing at all, which is why this took so long to find (2026-08-24).
  //
  // g_ota_check_requested is what UART OTA_CHECK sets, and it was not consulted
  // here, so that command could never do anything. The nightly path (see
  // maybeRunOtaCheck("nightly")) is deliberately UNCONDITIONAL and is left alone
  // -- discovery on the nightly wake is the intended design.
  const bool ota_explicitly_requested =
      g_ota_check_requested || mqtt_ota_check_requested || OtaIntent::getOtaIntentActive();
  if (ota_allowed && (OtaIntent::shouldUpdateNow() || ota_explicitly_requested)) {
    Serial.printf("[OTA_PRESLEEP] running check (desired=%d requested=%d)\n",
                  OtaIntent::shouldUpdateNow() ? 1 : 0,
                  ota_explicitly_requested ? 1 : 0);
    g_ota_check_done = false;
    g_ota_skip_logged = false;
    maybeRunOtaCheck("pre_sleep", true);
  } else if (!ota_allowed) {
    Serial.println("[OTA_POLICY] maintenance_only skip ota_check (not in window)");
  } else {
    // Say so. A path that decides NOT to check must not be invisible.
    Serial.println("[OTA_PRESLEEP] skip: nothing desired and no check requested");
  }
  mqtt_set_allowed(false);

}

void halo_prod_pre_setup() {
  // Record the wake cause before anything else can disturb it.
  nightly_maintenance_note_wake();
  g_provisioning_manager.init(nullptr);
  bool provisioned = ProvisioningState::isProvisioned();
  if (!provisioned) {
    if (g_provisioning_manager.startSetupMode()) {
      g_pending_provision_qr = true;
      g_last_prov_state = ProvisioningState::getState();
    }
  } else {
    char home_ssid[64];
    char home_password[64];
    if (!ProvisioningState::loadHomeWifiCreds(home_ssid, sizeof(home_ssid),
                                              home_password, sizeof(home_password))) {
      if (g_provisioning_manager.startSetupMode()) {
        g_pending_provision_qr = true;
        g_last_prov_state = ProvisioningState::getState();
      }
    }
  }
}

void halo_prod_reset_wifi() {
  LOG_INFO("[PROVISION] Reset Wi-Fi requested");
  ProvisioningState::clearHomeWifiCreds();
  ProvisioningState::clearApCreds();
  ProvisioningState::clearOwnerId();
  ProvisioningState::clearOwnerCode();
  ProvisioningState::setProvisioned(false);
  ProvisioningState::setState(ProvisioningState::STATE_UNPROVISIONED);
  if (g_provisioning_manager.startSetupMode()) {
    g_pending_provision_qr = true;
    g_last_provision_qr_ms = 0;
    g_last_prov_state = ProvisioningState::getState();
    LOG_INFO("[PROVISION] Setup mode started (reset Wi-Fi)");
  } else {
    LOG_ERROR("[PROVISION] Setup mode failed to start (reset Wi-Fi)");
  }
}

static void handle_pending_ota_expectation() {
  if (!OtaExpect::isPending()) {
    return;
  }
  char expected_version[OtaExpect::EXPECTED_VERSION_MAX_LEN + 1];
  expected_version[0] = '\0';
  bool have_expected = OtaExpect::getExpectedVersion(expected_version, sizeof(expected_version));
  const char* actual_version = kFirmwareVersion;

  Serial.printf("[OTA_EXPECT] pending expected=%s running=%s\n",
                have_expected ? expected_version : "<none>",
                actual_version);

  if (!have_expected || expected_version[0] == '\0') {
    Serial.println("[OTA_EXPECT][WARN] missing expected_version - clearing pending");
    OtaExpect::clearPending();
    return;
  }

  if (strcmp(actual_version, expected_version) != 0) {
    Serial.printf("[OTA_EXPECT][ERROR] version mismatch expected=%s running=%s\n",
                  expected_version, actual_version);
    char prev_label[OtaExpect::PREV_LABEL_MAX_LEN + 1];
    uint32_t prev_addr = 0;
    bool have_prev = OtaExpect::getPrevPartitionInfo(prev_label, sizeof(prev_label), prev_addr);
    const esp_partition_t* prev_part = nullptr;
    if (have_prev && prev_label[0] != '\0') {
      prev_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                           ESP_PARTITION_SUBTYPE_ANY,
                                           prev_label);
    }
    if (!prev_part && prev_addr != 0) {
      esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                       ESP_PARTITION_SUBTYPE_ANY,
                                                       nullptr);
      while (it) {
        const esp_partition_t* p = esp_partition_get(it);
        if (p && p->address == prev_addr) {
          prev_part = p;
          break;
        }
        it = esp_partition_next(it);
      }
      esp_partition_iterator_release(it);
    }
    if (prev_part) {
      Serial.printf("[OTA_EXPECT][INFO] reverting to partition=%s (0x%x)\n",
                    prev_part->label, prev_part->address);
      esp_err_t set_boot_err = esp_ota_set_boot_partition(prev_part);
      if (set_boot_err == ESP_OK) {
        OtaExpect::clearPending();
        delay(200);
        halo_reboot("ota_revert");
      } else {
        Serial.printf("[OTA_EXPECT][ERROR] set_boot_partition failed: %s\n",
                      esp_err_to_name(set_boot_err));
      }
    } else {
      Serial.println("[OTA_EXPECT][ERROR] previous partition not found");
    }
    return;
  }

  // OTA succeeded; release LCD OTA lock — unless LCD OTA is still pending
  if (get_lcd_ota_due_nvs()) {
    Serial.println("[OTA_EXPECT] version match — keep OTA_LOCK (lcd_ota_due pending)");
  } else {
    Serial.println("[OTA_EXPECT] version match -> send OTA_UNLOCK to LCD");
    send_ota_uart_message("OTA_UNLOCK", true);
  }
  // Clear expectation after a successful version match to avoid repeated OTA_UNLOCK.
  OtaExpect::setLastSuccessTs((uint32_t)(millis() / 1000));
  OtaExpect::clearPending();

#if HALO_SIMPLE_OTA_PROOF
  g_ota_validated_this_boot = true;
  g_ota_validated_time_ms = millis();
  g_ota_simple_proof_started = true;
  g_ota_proof_start_ms = millis();
  Serial.println("[OTA_PROOF] begin");
#endif
}

static void handle_ota_proof() {
#if HALO_SIMPLE_OTA_PROOF
  if (!g_ota_simple_proof_started || g_ota_simple_proof_done) {
    return;
  }
#if OTA_TEST_BUILD
  if (g_health_gate.getSkipMarkValid()) {
    Serial.println("[OTA_TEST] skip_mark_valid=1 -> bypass OTA proof/mark_valid");
    return;
  }
#endif
  unsigned long proof_elapsed_ms = millis() - g_ota_proof_start_ms;
  bool wifi_ok = !ProvisioningState::isProvisioned() || wifi_is_connected();
  bool uart_ok = true;
  if (uart_ok && wifi_ok) {
    OtaExpect::setLastSuccessTs((uint32_t)(proof_elapsed_ms / 1000));
    OtaExpect::clearPending();
    send_ota_uart_message("OTA_UNLOCK");
    maybe_trigger_lcd_ota_check();
    g_ota_simple_proof_done = true;
    Serial.println("[OTA_PROOF] pass");
  } else if (proof_elapsed_ms >= OTA_PROOF_TIMEOUT_MS) {
    const char* reason = uart_ok ? "wifi" : "uart";
    char err_msg[OtaExpect::LAST_ERROR_MAX_LEN];
    snprintf(err_msg, sizeof(err_msg), "proof_failed:%s", reason);
    OtaExpect::setLastError(err_msg);
    send_ota_uart_message("OTA_UNLOCK");
    maybe_trigger_lcd_ota_check();
    g_ota_simple_proof_done = true;
    Serial.printf("[OTA_PROOF] fail reason=%s\n", reason);
  }
#endif
}

static bool prepare_lcd_ota_proxy_retry(char* lcd_fw, size_t fw_len) {
  if (!sense_lcd_ota_retry_safe() || !g_lcd_work_budget.remaining_ms()) return false;
  // An abort clears the old lock. Reacquire it before a fresh query so the
  // receiver can finish cleanup and prove it is ready for another BEGIN.
  send_ota_uart_message("OTA_LOCK");
  if (!sense_lcd_ota_query(lcd_fw, fw_len, nullptr, nullptr, g_lcd_work_budget.remaining_ms())) {
    LOG_INFO("[OTA_ORCH] lcd retry query failed - leaving update unresolved");
    diag_record_error_persistent("ota_orch", -1, "lcd_retry_query_fail");
    return false;
  }
  boot_ota_repeat_begin_after_lcd_query();
  return true;
}

// FIX B: proxy the LCD OTA INLINE on the main task (NOT the fragile background
// lcd_ota_proxy_task, which DMA-starves and has many skip paths). Used by the
// up_to_date path where the Sense is already current and only the LCD is
// behind — this directly heals a Sense-ahead / LCD-behind split on the next
// manual OTA. Runs synchronously on the main loop with the LCD held awake via
// OTA_LOCK, so it is reliable. Sends OTA_LOCK before streaming; the caller is
// responsible for the final OTA_UNLOCK. Retries the proxy once on transient
// failure (mirrors FIX A). Returns true if the LCD is up-to-date afterwards
// (already current OR proxy succeeded); false if it needed an update but the
// proxy failed (lcd_ota_due left set for the next cycle).
static bool prod_proxy_lcd_inline(bool report_both_current = true) {
  if (!sense_lcd_ota_retry_safe() || !g_lcd_work_budget.remaining_ms()) {
    set_lcd_ota_due_nvs(true);
    return false;
  }
  // Free internal RAM for the LCD download/stream (mirrors the both-behind
  // inline block and lcd_ota_proxy_task). MQTT/camera DMA are already released
  // on the OTA path by the caller.
  g_manifest_client.releaseConnection();
  delay(100);  // Let memory coalesce before the LCD TLS download

  const OtaUrlConfig* lcd_cfg = ota_get_config();
  char lcd_fw[32] = {0};
  OtaManifest lcd_manifest;
  {
    char crumb[96];
    snprintf(crumb, sizeof(crumb), "lcd_inline_query_tx t=%lu link_recent=%d",
             (unsigned long)millis(), (int)halo_uart_link_recent(3000));
    diag_record_error_persistent("ota_orch", 0, crumb);
  }
  if (!sense_lcd_ota_query(lcd_fw, sizeof(lcd_fw), nullptr, nullptr, g_lcd_work_budget.remaining_ms())) {
    LOG_INFO("[OTA_ORCH] up_to_date inline lcd proxy result=lcd_query_fail");
    set_lcd_ota_due_nvs(true);
    return false;
  }
  boot_ota_repeat_begin_after_lcd_query();
  if (!sense_lcd_ota_fetch_manifest(lcd_cfg->base_dir, lcd_cfg->channel, lcd_manifest, &g_lcd_work_budget)) {
    LOG_INFO("[OTA_ORCH] up_to_date inline lcd proxy result=manifest_fetch_fail");
    set_lcd_ota_due_nvs(true);
    return false;
  }
  if (ManifestClient::compareVersions(lcd_manifest.version, lcd_fw) <= 0) {
    if (!g_lcd_work_budget.remaining_ms()) { set_lcd_ota_due_nvs(true); return false; }
    if (!report_both_current) {
      // A definitive policy completion needs current VALID/setup proof after
      // the manifest result, not just the earlier preflight ownership proof.
      char confirmed_lcd_fw[32] = {0};
      if (!sense_lcd_ota_retry_safe() ||
          !sense_lcd_ota_query(confirmed_lcd_fw, sizeof(confirmed_lcd_fw), nullptr,
                              lcd_fw, g_lcd_work_budget.remaining_ms()) ||
          !g_lcd_work_budget.remaining_ms()) {
        set_lcd_ota_due_nvs(true);
        return false;
      }
      strlcpy(g_lcd_ota_result, "noop", sizeof(g_lcd_ota_result));
      set_lcd_ota_result_nvs("noop", confirmed_lcd_fw);
      strlcpy(g_lcd_ota_version, confirmed_lcd_fw, sizeof(g_lcd_ota_version));
      g_lcd_fw_query_ms = millis();
      if (!g_lcd_work_budget.remaining_ms()) { set_lcd_ota_due_nvs(true); return false; }
    }
    LOG_INFO("[OTA_ORCH] up_to_date inline lcd already current (lcd=%s manifest=%s)",
             lcd_fw, lcd_manifest.version);
    set_lcd_ota_due_nvs(false);
    if (report_both_current) boot_ota_report_both_current(lcd_fw, lcd_manifest.version);
    return true;
  }

  // LCD is behind while the Sense is already current: proxy inline, holding the
  // LCD awake with OTA_LOCK. Retry once on transient failure.
  LOG_INFO("[OTA_ORCH] up_to_date inline lcd behind (lcd=%s manifest=%s) — proxying inline",
           lcd_fw, lcd_manifest.version);
  send_ota_uart_message("OTA_LOCK");
  bool ok = false;
  for (int attempt = 1; attempt <= 2 && !ok; ++attempt) {
    if (!sense_lcd_ota_retry_safe() || !g_lcd_work_budget.remaining_ms()) break;
    if (attempt > 1 && !prepare_lcd_ota_proxy_retry(lcd_fw, sizeof(lcd_fw))) {
      break;
    }
    {
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_inline_proxy_start ver=%s attempt=%d t=%lu",
               lcd_manifest.version, attempt, (unsigned long)millis());
      diag_record_error_persistent("ota_orch", 0, crumb);
    }
    const char* lcd_res = sense_lcd_ota_proxy(lcd_manifest, lcd_fw, &g_lcd_work_budget);
    LOG_INFO("[OTA_ORCH] up_to_date inline lcd proxy result=%s (attempt=%d)",
             lcd_res ? lcd_res : "(null)", attempt);
    if (lcd_res && (strcmp(lcd_res, "success") == 0 || strcmp(lcd_res, "up_to_date") == 0)) {
      const bool updated = strcmp(lcd_res, "success") == 0;
      char confirmed_lcd_fw[32] = {0};
      if (!sense_lcd_ota_retry_safe() || !sense_lcd_ota_query(confirmed_lcd_fw, sizeof(confirmed_lcd_fw), nullptr,
                               updated ? lcd_manifest.version : lcd_fw, g_lcd_work_budget.remaining_ms())) {
        // END_ACK proves bytes/boot selection, not completed boot/setup. Do not
        // retry into a version-only no-op and accidentally clear this debt.
        strncpy(g_lcd_ota_result, "lcd_postboot_unconfirmed", sizeof(g_lcd_ota_result) - 1);
        g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
        set_lcd_ota_result_nvs("lcd_postboot_unconfirmed", "");
        g_lcd_ota_version[0] = '\0';
        g_lcd_fw_query_ms = 0;
        diag_record_error_persistent("ota_orch", -1, "lcd_postboot_unconfirmed");
        break;
      }
      ok = true;
      const char* result = updated ? "updated" : "noop";
      strncpy(g_lcd_ota_result, result, sizeof(g_lcd_ota_result) - 1);
      g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
      set_lcd_ota_result_nvs(result, confirmed_lcd_fw);
      strncpy(g_lcd_ota_version, confirmed_lcd_fw, sizeof(g_lcd_ota_version) - 1);
      g_lcd_ota_version[sizeof(g_lcd_ota_version) - 1] = '\0';
      g_lcd_fw_query_ms = millis();
    } else if (attempt < 2) {
      LOG_INFO("[OTA_ORCH] up_to_date inline lcd proxy failed (attempt %d) — retrying once", attempt);
    }
  }
  ok = ok && sense_lcd_ota_retry_safe() && g_lcd_work_budget.remaining_ms();
  set_lcd_ota_due_nvs(!ok);
  LOG_INFO("[OTA_ORCH] up_to_date inline lcd proxy done ok=%d", ok ? 1 : 0);
  return ok;
}

// Sense policy may forbid its downgrade while the LCD still needs work. Resolve
// that work inline, then complete this schedule only after both boot proofs.
static bool complete_downgrade_policy_check(const char* manifest_fw, const char* reason) {
  const auto sense_boot_valid = []() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t state;
    return running && boot && running->address == boot->address &&
        esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_VALID;
  };
  if (!sense_lcd_ota_retry_safe() || !g_lcd_work_budget.remaining_ms() || !sense_boot_valid()) return false;
  strlcpy(g_lcd_ota_result, "pending", sizeof(g_lcd_ota_result));
  g_lcd_ota_version[0] = 0;
  if (!prod_proxy_lcd_inline(false) || !sense_lcd_ota_retry_safe() ||
      !g_lcd_work_budget.remaining_ms() || !sense_boot_valid() || get_lcd_ota_due_nvs()) return false;

  const bool had_pending = g_coord_pending[0] != 0;
  if (!g_lcd_work_budget.remaining_ms()) return false;
  ota_peer_schedule_complete();  // The atomic completion write precedes RAM/pending removal.
  if (had_pending && g_coord_pending[0]) return false;
  if (had_pending) g_coord_completion_target[0] = 0;

  char detail[192];
  snprintf(detail, sizeof(detail), "sense=%.31s manifest=%.31s policy=downgrade_blocked lcd=%.31s lcd_result=%.31s",
           kFirmwareVersion, manifest_fw, g_lcd_ota_version, g_lcd_ota_result);
  uart_send_sense_diag_persist("ota", "policy_result", "downgrade_blocked", 0, detail);
  if (g_boot_ota_diag_context && g_boot_ota_begin_reported) {
    snprintf(detail, sizeof(detail), "%s policy=downgrade_blocked lcd=%.31s result=%.15s",
             g_boot_ota_begin_record.detail, g_lcd_ota_version, g_lcd_ota_result);
    uart_send_sense_diag_persist("ota", "policy_complete", g_boot_ota_begin_record.reason,
                                g_boot_ota_begin_record.code, detail);
  } else {
    uart_send_sense_diag_persist("ota", "policy_complete", reason ? reason : "manual", 0, detail);
  }
  return true;
}

static void maybeRunOtaCheck(const char* reason, bool skip_boot_delay) {
  if (g_peer_episode_finished) { g_ota_check_done = true; return; }
  if (g_ota_check_done || g_ota_apply_in_progress) {
    return;
  }
  if (!ota_peer_ready()) {
    if (!g_boot_ota_pending && !g_ota_check_done) g_ota_check_requested = true;
    return;
  }

  if (!SenseOtaPolicy::allowOtaWorkNow(reason)) {
    static bool g_ota_policy_skip_logged_this_boot = false;
    if (!g_ota_policy_skip_logged_this_boot) {
      Serial.println("[OTA_POLICY] maintenance_only skip ota_check (not in window)");
      Serial.println("[OTA_POLICY] skip manifest/ota (maintenance-only)");
      g_ota_policy_skip_logged_this_boot = true;
    }
    return;
  }
  if (!OtaIntent::cooldownAllows() && !ota_peer_continuation()) {
    if (!g_ota_skip_logged) {
      LOG_INFO("[OTA_INTENT] reason=cooldown result=0");
      g_ota_skip_logged = true;
    }
    return;
  }
  if (g_reboot_loop_detected) {
    // Re-evaluate guard so it can clear without a reboot
    g_reboot_loop_detected = BootState::checkRebootLoop(3, 30000);
    if (g_reboot_loop_detected) {
      if (ota_test_bypass_reboot_guard_enabled()) {
        g_reboot_loop_detected = false;
      } else if (halo_ota_manual_override_active()) {
        LOG_INFO("[OTA] reboot_loop_guard bypassed by manual OTA request");
        g_reboot_loop_detected = false;
      } else {
        LOG_INFO("[OTA] skip reboot_loop_guard");
        return;
      }
    }
  }
  if (ota_disabled_for_local_dev()) {
    LOG_INFO("[OTA] disabled_local_dev");
    return;
  }
  if (HALO_OTA_ENABLED == 0) {
    LOG_INFO("[OTA] skip ota_disabled");
    return;
  }
  if (HALO_SHIP_TEST_MODE != 0) {
    LOG_INFO("[OTA] skip ship_test_mode");
    return;
  }
  if (!ProvisioningState::isProvisioned()) {
    LOG_INFO("[OTA] skip not_provisioned");
    return;
  }
  if (ProvisioningState::getState() != ProvisioningState::STATE_CONNECTED) {
    LOG_INFO("[OTA] skip not_connected_state");
    return;
  }
  if (!wifiReadyForHttps()) {
    IPAddress ip = WiFi.localIP();
    IPAddress d0 = WiFi.dnsIP(0);
    LOG_INFO("[WIFI_GUARD] skip_https_not_ready status=%d ip=%s dns0=%s",
             (int)WiFi.status(),
             ip.toString().c_str(),
             d0.toString().c_str());
    return;
  }
  if (!is_time_valid()) {
    static unsigned long last_tls_guard_log_ms = 0;
    unsigned long now_ms = millis();
    if (now_ms - last_tls_guard_log_ms > 5000) {
      time_t now = time(nullptr);
      LOG_INFO("[TLS_GUARD] skip_https_time_not_ready epoch=%ld", (long)now);
      last_tls_guard_log_ms = now_ms;
    }
    return;
  }
  if (!skip_boot_delay && (millis() - g_boot_time_ms) < 5000) {
    return;
  }

  g_peer_gate.entered = true;
  OtaPeerTransactionCleanup peer_cleanup;
  if (g_peer_continue_work) {
    if (!ota_peer_continuation()) { g_ota_check_done = true; set_lcd_ota_due_nvs(true); return; }
  } else {
    g_lcd_work_budget = {millis(), 2400000UL};
    g_lcd_work_budget_live = true;
    strlcpy(g_lcd_work_schedule, g_coord_pending, sizeof(g_lcd_work_schedule));
  }
  g_lcd_work_peer_boot = g_peer_gate.peer_boot;

  // Record actual check entry after readiness guards, so skipped retries do
  // not write NVS. The manual-OTA decision/handshake trail survives reboots
  // and is readable later via the LCD error log.
  HaloNtpDnsGuard ntp_dns_guard;
  {
    char crumb[96];
    snprintf(crumb, sizeof(crumb), "reason=%s manual=%d t=%lu",
             reason ? reason : "(null)",
             (int)halo_ota_manual_override_active(),
             (unsigned long)millis());
    diag_record_error_persistent("ota_orch", 0, crumb);
  }
  if (halo_ota_manual_override_active()) {
    manual_ota_override_clear("ota_check_begin");
  }
  ota_mark_check_start();
  ota_set_last_result("check_begin");
  g_ota_check_in_progress = true;
  g_ota_check_done = true;
  // Release camera DMA reservation to free 16KB of internal SRAM for TLS.
  // Camera is not used during OTA. Device reboots after OTA, re-reserving in setup().
  // Hold it released for the WHOLE check, not just this instant. Freeing it here
  // was not enough: the provisioning TLS guard re-acquires on scope exit and runs
  // during the check, so by the time the applier tested the heap the 16KB was
  // back and the update was rejected every single time.
  g_dma_reserve_suppressed = true;
  if (g_camera_dma_reserve) {
    heap_caps_free(g_camera_dma_reserve);
    g_camera_dma_reserve = nullptr;
    LOG_INFO("[OTA] Camera DMA reservation released for TLS headroom");
  }
  OtaIntent::recordOtaAttempt("begin");
  dump_system_truth("ota_check_begin");
  auto clear_intent_once = []() {
    OtaIntent::clearForceAndCheck();
  };
  auto release_waiting_lcd_ota = [&](const char* why) {
    if (!g_lcd_ota_request_active) {
      return;
    }
    LOG_INFO("[LCD_OTA_ORCH] unlock deferred lcd reason=%s request_id=%lu",
             why ? why : "unknown",
             (unsigned long)g_lcd_ota_request_id);
    send_ota_uart_message("OTA_UNLOCK");
  };

  OtaUrlConfig* cfg_mut = ota_get_config_mutable();
  resolveOtaManifestUrl(cfg_mut);
  const OtaUrlConfig* cfg = ota_get_config();

  char manifest_url[512];
  snprintf(manifest_url, sizeof(manifest_url), "%s?v=%s-boot%lu",
           cfg->manifest_url, kFirmwareVersion, g_boot_count);

  if (containsDisallowedHost(manifest_url) || !cfg->allowed_host || strlen(cfg->manifest_url) == 0) {
    truth_get_manifest_state().setErr();
    dump_system_truth("manifest_err");
    ota_set_last_result("manifest_url_invalid");
    release_waiting_lcd_ota("manifest_url_invalid");
    // No LCD proxy runs on this path; release the LCD in case a manual OTA_LOCK
    // (prod:1877) is holding its "Updating…" screen.
    send_ota_uart_message("OTA_UNLOCK", true);
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    clear_intent_once();
    return;
  }

  // Free MQTT TLS buffers (~16KB internal SRAM) before manifest fetch.
  // MQTT will be reconnected if no OTA update is needed.
  Serial.printf("[OTA] Disconnecting MQTT before manifest fetch (free=%lu)\n", (unsigned long)ESP.getFreeHeap());
  mqtt_stop_for_ota();
  delay(100);
  Serial.printf("[OTA] Post-MQTT-disconnect heap free=%lu\n", (unsigned long)ESP.getFreeHeap());

  LOG_INFO("[MANIFEST] Fetching manifest: %s", manifest_url);
  OtaManifest manifest;
  const uint32_t manifest_remaining_ms = g_lcd_work_budget.remaining_ms();
  if (!manifest_remaining_ms ||
      !g_manifest_client.fetchManifest(manifest_url, manifest,
          manifest_remaining_ms < 10000 ? manifest_remaining_ms : 10000) ||
      !g_lcd_work_budget.remaining_ms()) {
    truth_get_manifest_state().setErr();
    dump_system_truth("manifest_err");
    ota_set_last_result("manifest_fetch_fail");
    release_waiting_lcd_ota("manifest_fetch_fail");
    // No LCD proxy runs on this path; release the LCD in case a manual OTA_LOCK
    // (prod:1877) is holding its "Updating…" screen.
    send_ota_uart_message("OTA_UNLOCK", true);
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    mqtt_set_allowed(true);
    mqtt_force_connect();
    clear_intent_once();
    return;
  }

  truth_get_manifest_state().setOk(manifest.version);
  dump_system_truth("manifest_ok");

  if (manifest.board_specified && strcasecmp(manifest.board, HALO_BOARD_NAME) != 0) {
    LOG_ERROR("[OTA] board_mismatch manifest=%s device=%s",
              manifest.board, HALO_BOARD_NAME);
    ota_set_last_result("board_mismatch");
    release_waiting_lcd_ota("board_mismatch");
    // No LCD proxy runs on this path; release the LCD in case a manual OTA_LOCK
    // (prod:1877) is holding its "Updating…" screen.
    send_ota_uart_message("OTA_UNLOCK", true);
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    mqtt_set_allowed(true);
    mqtt_force_connect();
    clear_intent_once();
    return;
  }

  if (containsDisallowedHost(manifest.url) || !isAllowedOtaHost(manifest.url)) {
    LOG_ERROR("[OTA_HOST] Disallowed bin_url: %s", manifest.url);
    ota_set_last_result("bin_url_disallowed");
    release_waiting_lcd_ota("bin_url_disallowed");
    // No LCD proxy runs on this path; release the LCD in case a manual OTA_LOCK
    // (prod:1877) is holding its "Updating…" screen.
    send_ota_uart_message("OTA_UNLOCK", true);
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    mqtt_set_allowed(true);
    mqtt_force_connect();
    clear_intent_once();
    return;
  }

  int version_cmp = compareSemver(manifest.version, kFirmwareVersion);
  bool desired_force = OtaIntent::getDesiredForce();
  bool allow_downgrade = OtaIntent::getAllowDowngrade();
  if (version_cmp == 0) {
    LOG_INFO("[MANIFEST] up_to_date version=%s", manifest.version);
    OtaIntent::recordOtaResult("up_to_date");
    ota_set_last_result("up_to_date");
    // FIX B: the Sense is already current — proxy the LCD INLINE on the main
    // task (not the fragile background lcd_ota_proxy_task). This runs with the
    // LCD held awake by OTA_LOCK, so it reliably heals a Sense-ahead /
    // LCD-behind split on this manual/nightly OTA instead of depending on the
    // DMA-starving background task that may never run.
    bool lcd_inline_ok = prod_proxy_lcd_inline();
    if (lcd_inline_ok && g_lcd_work_budget.remaining_ms()) ota_peer_schedule_complete();
    OtaIntent::markNoUpdateNeeded();
    release_waiting_lcd_ota("up_to_date");
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
      // The inline proxy above took/held the LCD lock; release it now so a
      // manual OTA_LOCK (prod:1877) doesn't strand the LCD on "Updating…".
      send_ota_uart_message("OTA_UNLOCK", true);
    }
    clear_intent_once();
    return;
  }
  if (version_cmp < 0 && !allow_downgrade) {
    LOG_INFO("[MANIFEST] downgrade_blocked current=%s manifest=%s forced=%d",
             kFirmwareVersion,
             manifest.version,
             desired_force ? 1 : 0);
    OtaIntent::recordOtaResult("downgrade_blocked");
    ota_set_last_result("downgrade_blocked");
    if (!complete_downgrade_policy_check(manifest.version, reason)) {
      diag_record_error_persistent("ota_orch", -1, "downgrade_policy_deferred");
    }
    // Keep the proved peer lease while resolving LCD work. Even an unmarked
    // unlock clears that lease and can release a recovery/continuation hold.
    OtaIntent::markNoUpdateNeeded();
    release_waiting_lcd_ota("downgrade_blocked");
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
      // No LCD proxy task will run to release the lock; unlock the LCD now so a
      // manual OTA_LOCK (prod:1877) doesn't strand it on "Updating…".
      send_ota_uart_message("OTA_UNLOCK", true);
    }
    clear_intent_once();
    return;
  }
  if (version_cmp < 0 && allow_downgrade) {
    LOG_INFO("[MANIFEST] downgrade_allowed=1 current=%s manifest=%s forced=%d",
             kFirmwareVersion,
             manifest.version,
             desired_force ? 1 : 0);
  }
  LOG_INFO("[MANIFEST] update_available current=%s manifest=%s forced=%d",
           kFirmwareVersion,
           manifest.version,
           desired_force ? 1 : 0);
  OtaIntent::recordOtaResult("update_available");

  if (manifest.min_version_allowed_specified &&
      compareSemver(kFirmwareVersion, manifest.min_version_allowed) < 0) {
    LOG_INFO("[OTA_ROLLOUT] min_version_blocked current=%s min_allowed=%s",
             kFirmwareVersion,
             manifest.min_version_allowed);
    OtaIntent::recordOtaResult("rollout_min_version");
    ota_set_last_result("rollout_min_version");
    release_waiting_lcd_ota("rollout_min_version");
    maybe_trigger_lcd_ota_check();
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
      // No LCD proxy task will run to release the lock; unlock the LCD now so a
      // manual OTA_LOCK (prod:1877) doesn't strand it on "Updating…".
      send_ota_uart_message("OTA_UNLOCK", true);
    }
    clear_intent_once();
    return;
  }

  if (manifest.rollout_pct_specified) {
    uint32_t seed = manifest.rollout_seed_specified ? manifest.rollout_seed : 0;
    uint32_t bucket = ota_rollout_bucket(seed);
    bool allow = (manifest.rollout_pct >= 100) || (bucket < manifest.rollout_pct);
    LOG_INFO("[OTA_ROLLOUT] bucket=%lu pct=%u seed=%lu decision=%s",
             (unsigned long)bucket,
             (unsigned)manifest.rollout_pct,
             (unsigned long)seed,
             allow ? "apply" : (desired_force ? "override" : "skip"));
    if (!allow && !desired_force) {
      OtaIntent::recordOtaResult("rollout_skip");
      ota_set_last_result("rollout_skip");
      release_waiting_lcd_ota("rollout_skip");
      maybe_trigger_lcd_ota_check();
      g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
      if (!g_lcd_ota_task_running) {
        mqtt_set_allowed(true);
        mqtt_force_connect();
        // No LCD proxy task will run to release the lock; unlock the LCD now so
        // a manual OTA_LOCK (prod:1877) doesn't strand it on "Updating…".
        send_ota_uart_message("OTA_UNLOCK", true);
      }
      clear_intent_once();
      return;
    }
  }

  bool apply_allowed = false;
  char why[32] = "unknown";
  Truth::evaluateOtaApply(apply_allowed, why, sizeof(why));
  if (!apply_allowed && !desired_force) {
    LOG_INFO("[OTA] apply blocked reason=%s", why);
    char result[64];
    snprintf(result, sizeof(result), "apply_blocked:%s", why);
    OtaIntent::recordOtaResult(result);
    ota_set_last_result(result);
    release_waiting_lcd_ota(result);
    maybe_trigger_lcd_ota_check();
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
      // No LCD proxy task will run to release the lock; unlock the LCD now so a
      // manual OTA_LOCK (prod:1877) doesn't strand it on "Updating…".
      send_ota_uart_message("OTA_UNLOCK", true);
    }
    clear_intent_once();
    return;
  }
  if (!apply_allowed && desired_force) {
    LOG_INFO("[OTA] apply blocked reason=%s override=1", why);
  }

  // ── LCD proxy FIRST, while the LCD is still awake from the button press ──
  // The Sense self-OTA below reboots the device, so if we deferred the LCD
  // proxy to the next boot (the old lcd_ota_due path), the rebooted Sense
  // would query an LCD that has gone back to sleep -> intermittent
  // lcd_query_fail. By proxying inline here, on the main task, while the LCD
  // is awake, we avoid that race. lcd_ota_due remains as the fallback if the
  // proxy is skipped or fails transiently.
  //
  // This runs on the main task (maybeRunOtaCheck is called from the main
  // loop), so the main-loop UART drain is blocked while it runs — no race.
  // sense_lcd_ota_proxy() manages g_lcd_ota_proxy_owns_uart itself (true
  // during COBS streaming, false after); we do not double-manage it.
  bool lcd_proxy_succeeded = false;
  // Advance Sense only after positively confirming the LCD is current or its
  // proxy succeeded. An unanswered query/failed manifest is unresolved too;
  // assuming success there strands a split just like a failed transfer.
  {
    // Free internal RAM for the LCD download/stream + later Sense apply.
    // MQTT is already stopped (mqtt_stop_for_ota() above) and camera DMA is
    // already released; release the manifest-client connection too so the
    // TLS download of the LCD binary has headroom — same as
    // maybe_trigger_lcd_ota_check() / lcd_ota_proxy_task().
    g_manifest_client.releaseConnection();
    delay(100);  // Let memory coalesce before the LCD TLS download

    const OtaUrlConfig* lcd_cfg = ota_get_config();
    char lcd_fw[32] = {0};
    OtaManifest lcd_manifest;
    {
      // Breadcrumb: about to send LCD_OTA_QUERY over UART.
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_query_tx t=%lu link_recent=%d",
               (unsigned long)millis(), (int)halo_uart_link_recent(3000));
      diag_record_error_persistent("ota_orch", 0, crumb);
    }
    const bool lcd_query_ok = sense_lcd_ota_retry_safe() && sense_lcd_ota_query(lcd_fw, sizeof(lcd_fw), nullptr, nullptr, g_lcd_work_budget.remaining_ms());
    if (lcd_query_ok) boot_ota_repeat_begin_after_lcd_query();
    if (!lcd_query_ok) {
      LOG_INFO("[OTA_ORCH] lcd proxy result=lcd_query_fail (will defer to lcd_ota_due)");
      // Breadcrumb: LCD never answered the query (the failure we're chasing).
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_query_fail t=%lu", (unsigned long)millis());
      diag_record_error_persistent("ota_orch", -1, crumb);
    } else if (!sense_lcd_ota_fetch_manifest(lcd_cfg->base_dir, lcd_cfg->channel, lcd_manifest, &g_lcd_work_budget)) {
      LOG_INFO("[OTA_ORCH] lcd proxy result=manifest_fetch_fail (will defer to lcd_ota_due)");
      // Breadcrumb: query succeeded; record the LCD fw it reported.
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_query_ok lcd_fw=%s t=%lu", lcd_fw, (unsigned long)millis());
      diag_record_error_persistent("ota_orch", 0, crumb);
    } else if (ManifestClient::compareVersions(lcd_manifest.version, lcd_fw) > 0) {
      // The LCD is behind: proxy with one retry before allowing Sense to advance.
      // Breadcrumb: query succeeded; record the LCD fw it reported.
      {
        char crumb[96];
        snprintf(crumb, sizeof(crumb), "lcd_query_ok lcd_fw=%s t=%lu", lcd_fw, (unsigned long)millis());
        diag_record_error_persistent("ota_orch", 0, crumb);
      }
      send_ota_uart_message("OTA_LOCK");
      // Attempt the LCD proxy up to twice: a transient failure (DMA/UART
      // hiccup) on the first pass must not strand the LCD behind while the
      // Sense self-updates ahead of it.
      for (int attempt = 1; attempt <= 2 && !lcd_proxy_succeeded; ++attempt) {
        if (!sense_lcd_ota_retry_safe() || !g_lcd_work_budget.remaining_ms()) break;
        if (attempt > 1 && !prepare_lcd_ota_proxy_retry(lcd_fw, sizeof(lcd_fw))) {
          break;
        }
        {
          // Breadcrumb: starting the LCD OTA proxy stream.
          char crumb[96];
          snprintf(crumb, sizeof(crumb), "lcd_proxy_start ver=%s attempt=%d t=%lu",
                   lcd_manifest.version, attempt, (unsigned long)millis());
          diag_record_error_persistent("ota_orch", 0, crumb);
        }
        const char* lcd_res = sense_lcd_ota_proxy(lcd_manifest, lcd_fw, &g_lcd_work_budget);
        LOG_INFO("[OTA_ORCH] lcd proxy result=%s (attempt=%d)",
                 lcd_res ? lcd_res : "(null)", attempt);
        {
          // Breadcrumb: LCD OTA proxy returned.
          char crumb[96];
          snprintf(crumb, sizeof(crumb), "lcd_proxy_done res=%s attempt=%d t=%lu",
                   lcd_res ? lcd_res : "(null)", attempt, (unsigned long)millis());
          diag_record_error_persistent("ota_orch", 0, crumb);
        }
        if (lcd_res && (strcmp(lcd_res, "success") == 0 || strcmp(lcd_res, "up_to_date") == 0)) {
          const bool updated = strcmp(lcd_res, "success") == 0;
          char confirmed_lcd_fw[32] = {0};
          if (!sense_lcd_ota_retry_safe() || !sense_lcd_ota_query(confirmed_lcd_fw, sizeof(confirmed_lcd_fw), nullptr,
                                   updated ? lcd_manifest.version : lcd_fw,
                                   g_lcd_work_budget.remaining_ms())) {
            set_lcd_ota_result_nvs("lcd_postboot_unconfirmed", "");
            diag_record_error_persistent("ota_orch", -1, "lcd_postboot_unconfirmed");
            break;
          }
          lcd_proxy_succeeded = true;
          // Persist only after target boot/setup/VALID was freshly proved.
          const char* result = updated ? "updated" : "noop";
          strncpy(g_lcd_ota_result, result, sizeof(g_lcd_ota_result) - 1);
          g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
          set_lcd_ota_result_nvs(result, updated ? lcd_manifest.version : lcd_fw);
          if (updated) {
            // A completed transfer needs a fresh query after the LCD reboot.
            g_lcd_ota_version[0] = '\0';
            g_lcd_fw_query_ms = 0;
          } else {
            // A fresh query may report the target or a newer running image.
            strncpy(g_lcd_ota_version, lcd_fw, sizeof(g_lcd_ota_version) - 1);
            g_lcd_ota_version[sizeof(g_lcd_ota_version) - 1] = '\0';
            g_lcd_fw_query_ms = millis();
          }
        } else if (attempt < 2) {
          LOG_INFO("[OTA_ORCH] lcd proxy failed (attempt %d) — retrying once before deciding", attempt);
        }
      }
      // OTA_LOCK above leaves the LCD locked-awake; the Sense reboots right
      // after the apply below. The LCD's own OTA_LOCK timeout / post-OTA
      // reboot handles re-sleeping. (On the up-to-date branch below no lock
      // was taken, so nothing to unlock.)
    } else {
      LOG_INFO("[OTA_ORCH] lcd proxy result=up_to_date (lcd=%s manifest=%s)",
               lcd_fw, lcd_manifest.version);
      // Breadcrumb: query succeeded; LCD already up-to-date.
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_query_ok lcd_fw=%s t=%lu", lcd_fw, (unsigned long)millis());
      diag_record_error_persistent("ota_orch", 0, crumb);
      lcd_proxy_succeeded = g_lcd_work_budget.remaining_ms() > 0;
    }
  }

  // Fallback: only owe an lcd_ota_due retry on the next boot if the LCD was
  // NOT brought up-to-date here (query/manifest fail, or proxy non-success).
  // On success (or already up-to-date) clear it — the LCD is already done.
  lcd_proxy_succeeded = lcd_proxy_succeeded && sense_lcd_ota_retry_safe() && g_lcd_work_budget.remaining_ms();
  set_lcd_ota_due_nvs(!lcd_proxy_succeeded);
  LOG_INFO("[OTA] lcd_ota_due=%d (lcd_proxy_succeeded=%d)",
           lcd_proxy_succeeded ? 0 : 1, lcd_proxy_succeeded ? 1 : 0);

  // Never advance Sense while LCD state is unresolved. Keep the debt in NVS
  // for the next bounded boot/manual/nightly check, then release the LCD now:
  // this attempt is over and leaving its update screen locked is not a retry.
  if (!lcd_proxy_succeeded) {
    LOG_ERROR("[OTA] LCD unresolved — deferring Sense self-OTA to avoid split; will retry next cycle");
    {
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "sense_apply_deferred lcd_split_guard ver=%s t=%lu",
               manifest.version, (unsigned long)millis());
      diag_record_error_persistent("ota_orch", -1, crumb);
    }
    OtaIntent::recordOtaResult("lcd_proxy_failed_defer");
    ota_set_last_result("lcd_proxy_failed_defer");
    // Return to normal operation without advancing Sense.
    send_ota_uart_message("OTA_UNLOCK", true);
    mqtt_set_allowed(true);
    mqtt_force_connect();
    clear_intent_once();
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    return;
  }

  // ── THEN the Sense self-OTA (reboots on success, never returns) ──
  {
    // Breadcrumb: about to apply the Sense self-OTA (this reboots on success,
    // so this is the last crumb before the LCD-owed state at next boot).
    char crumb[96];
    snprintf(crumb, sizeof(crumb), "sense_apply_start ver=%s lcd_due=%d t=%lu",
             manifest.version, (int)(!lcd_proxy_succeeded), (unsigned long)millis());
    diag_record_error_persistent("ota_orch", 0, crumb);
  }
  if (!sense_lcd_ota_retry_safe() || !g_lcd_work_budget.remaining_ms()) {
    OtaIntent::recordOtaResult("paired_deadline");
    ota_set_last_result("paired_deadline");
    clear_intent_once();
    g_ota_check_in_progress = false; g_dma_reserve_suppressed = false;
    return;
  }
  if (g_coord_pending[0]) {
    Preferences p;
    if (p.begin("ota_coord", false)) { p.putString("target", manifest.version); p.end(); }
  }
  send_ota_uart_message("OTA_LOCK");
  // NVS and UART calls consume the same pair budget. Sample at the actual
  // applier boundary rather than giving it time spent in those operations.
  const uint32_t pair_remaining_ms = g_lcd_work_budget.remaining_ms();
  if (!sense_lcd_ota_retry_safe() || !pair_remaining_ms) {
    OtaIntent::recordOtaResult("paired_deadline");
    ota_set_last_result("paired_deadline");
    clear_intent_once();
    g_ota_check_in_progress = false; g_dma_reserve_suppressed = false;
    return;
  }
  g_ota_apply_in_progress = true;

  SenseOtaApplier::Result res = g_ota_applier.applyToOtaPartition(
      manifest.url, manifest.sha256, manifest.size,
      pair_remaining_ms < 1200000UL ? pair_remaining_ms : 1200000UL, true, manifest.version);
  g_ota_apply_in_progress = false;
  // NOTE: lcd_ota_due was already set above based on whether the inline LCD
  // proxy succeeded (clear) or was skipped/failed (set as next-boot fallback).
  // Do NOT clear it unconditionally here — that would drop the fallback when
  // the LCD proxy failed but the Sense apply then succeeds and reboots.
  clear_intent_once();

  if (res != SenseOtaApplier::RESULT_SUCCESS) {
    const char* res_str = SenseOtaApplier::getResultString(res);
    LOG_ERROR("[OTA] apply failed: %s", res_str);
    // Re-enable MQTT after failed OTA (on success, device reboots)
    mqtt_set_allowed(true);
    mqtt_force_connect();
    OtaIntent::recordOtaResult(res_str);
    ota_set_last_result(res_str);
    send_ota_uart_message("OTA_UNLOCK");
    maybe_trigger_lcd_ota_check();
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    return;
  }
  ota_set_last_result("apply_success");
  (void)reason;
  g_ota_check_in_progress = false;
}

void maybeRunOtaCheck(const char* reason) {
  if (reason && strcmp(reason, "post_provision") == 0 &&
      !g_provisioning_manager.isSetupModeActive() && !ota_peer_accept_new_request()) return;
  if (reason && strcmp(reason, "post_provision") == 0 && g_provisioning_manager.isSetupModeActive()) {
    g_defer_ota_post_provision = true;
    LOG_INFO("[OTA] defer post_provision check until setup mode ends");
    return;
  }
  maybeRunOtaCheck(reason, true);
}

// On-demand OTA check requested over UART (LCD forwards type=OTA_CHECK).
//
// Until 6.2.0 this arrived and was dropped -- the Sense printed
// "[PROTO] Unknown type: OTA_CHECK". The on-demand path used to be an MQTT
// command, and MQTT was deleted, leaving the nightly timer wake as the only
// possible trigger. Policy still decides: this REQUESTS a check, it does not
// force a download while the user is busy.
static void halo_request_ota_check(const char* why, bool allow_reboot) {
  const char* reason = (why && *why) ? why : "uart_cmd";
  if (SenseOtaPolicy::allowOtaWorkNow(reason) && ota_peer_accept_new_request()) {
    g_ota_check_requested = true;
    LOG_INFO("[OTA_INTENT] reason=%s result=1 allow_reboot=%d (uart)", reason,
             allow_reboot ? 1 : 0);
  } else {
    LOG_INFO("[OTA_POLICY] skip manifest/ota (maintenance-only) reason=%s", reason);
  }
}

static void handle_mqtt_commands() {
  MqttCommand cmd = {};
  while (mqtt_get_next_command(&cmd)) {
    switch (cmd.type) {
      case MQTT_CMD_PING:
        break;
      case MQTT_CMD_REBOOT:
        halo_reboot("mqtt_cmd_reboot");
        break;
      case MQTT_CMD_OTA_CHECK:
        if (SenseOtaPolicy::allowOtaWorkNow("cmd") && ota_peer_accept_new_request()) {
          g_ota_check_requested = true;
          mqtt_ota_check_requested = true;
          LOG_INFO("[OTA_INTENT] reason=cmd result=1");
        } else {
          LOG_INFO("[OTA_POLICY] skip manifest/ota (maintenance-only)");
        }
        break;
      case MQTT_CMD_OTA_FORCE_NOW: {
        if (SenseOtaPolicy::allowOtaWorkNow("force_now") && ota_peer_accept_new_request()) {
          const uint32_t ts = is_time_valid() ? (uint32_t)time(nullptr) : 0;
          OtaIntent::updateDesired(nullptr, nullptr, true, false, ts, "force_now");
          g_ota_check_requested = true;
          mqtt_ota_check_requested = true;
          LOG_INFO("[OTA_INTENT] reason=force_now result=1");
        } else {
          LOG_INFO("[OTA_POLICY] skip manifest/ota (maintenance-only)");
        }
        break;
      }
      case MQTT_CMD_OTA_STATUS_GET:
        ota_sched_print_status("mqtt_cmd");
        dump_system_truth("ota_status");
        break;
      default:
        break;
    }
  }
}

void halo_prod_loop() {
  ota_peer_service();
  ota_notify_lcd_activity();
  if (sense_action_inflight()) {
    static unsigned long last_skip_log_ms = 0;
    unsigned long now_ms = millis();
    if (now_ms - last_skip_log_ms > 2000) {
      LOG_INFO("[HALO_PROD] action_inflight -> skip ota/maintenance");
      last_skip_log_ms = now_ms;
    }
    mqtt_set_allowed(false);
    return;
  }
  bool wifi_connected = wifi_is_connected();
  static bool last_wifi_connected = false;
  static unsigned long s_mqtt_awake_failover_ms = 0;
  static bool s_last_setup_mode_active = true;


#if OTA_TEST_BUILD
  ota_test_handle_serial();
  if (g_ota_test_crash_after_boot_ms > 0 &&
      (millis() - g_boot_time_ms) >= g_ota_test_crash_after_boot_ms) {
    LOG_ERROR("[OTA_TEST] crash_after_boot_ms=%u (forcing restart)",
              (unsigned int)g_ota_test_crash_after_boot_ms);
    ota_test_save_crash_after_boot(0);
    delay(50);
    esp_restart();
  }
#endif


  // Claim-before-MQTT: hold MQTT during SoftAP grace period and post-AP claim retry.
  // MQTT's TLS connection needs ~40KB internal SRAM — keep it off while SoftAP or
  // claim retry are consuming that headroom.
  {
    bool mqtt_ok = wifi_connected && ProvisioningState::isProvisioned();
    if (mqtt_ok && (g_provisioning_manager.isSetupModeActive() || s_post_ap_claim_retry_pending)) {
      mqtt_ok = false;
    }
    mqtt_set_allowed(mqtt_ok);
  }

  if (g_lcd_ota_request_active &&
      g_lcd_ota_request_start_ms > 0 &&
      (millis() - g_lcd_ota_request_start_ms) > LCD_OTA_RESULT_TIMEOUT_MS) {
    mark_lcd_ota_still_pending("async_watchdog_expired");
  }

  g_provisioning_manager.update();
  ProvisioningState::State prov_state = ProvisioningState::getState();
  if (prov_state != g_last_prov_state) {
    send_provision_status(ProvisioningState::getStateString(prov_state));
    g_last_prov_state = prov_state;
    if (prov_state == ProvisioningState::STATE_AP_SETUP) {
      g_pending_provision_qr = true;
    } else if (prov_state == ProvisioningState::STATE_CONNECTED) {
      g_post_provision_list_refresh_pending = true;
    }
    if (prov_state != ProvisioningState::STATE_CONNECTED) {
      g_lcd_wifi_creds_sent = false;
      g_lcd_got_creds_ack = false;
      g_lcd_got_wifion_ack = false;
      g_lcd_wifi_send_start_ms = 0;
      g_last_creds_send_ms = 0;
      g_last_wifion_send_ms = 0;
      g_lcd_wifi_send_failed = false;
    }
  }
  maybe_send_lcd_wifi_creds(prov_state);

  // Post-AP claim retry: when SoftAP shuts down, internal SRAM is freed.
  // Keep MQTT held and sleep blocked while we retry the owner code claim.
  {
    bool current_setup_active = g_provisioning_manager.isSetupModeActive();
    if (s_last_setup_mode_active && !current_setup_active) {
      s_post_ap_shutdown_ms = millis();
      s_post_ap_claim_retry_pending = true;
      g_provisioning_manager.resetClaimForRetry();
      LOG_INFO("[TLS_RECOVERY] SoftAP shutdown — claim retry pending, heap_internal=%u",
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    s_last_setup_mode_active = current_setup_active;

    if (s_post_ap_claim_retry_pending && s_post_ap_shutdown_ms > 0) {
      unsigned long elapsed = millis() - s_post_ap_shutdown_ms;
      // Check if claim succeeded (owner_id now in NVS)
      char owner_id[64] = {0};
      bool has_owner = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
      if (has_owner && owner_id[0] != '\0') {
        s_post_ap_claim_retry_pending = false;
        LOG_INFO("[TLS_RECOVERY] Claim succeeded post-AP, owner_id set");
      } else if (elapsed > 30000) {
        s_post_ap_claim_retry_pending = false;
        LOG_WARN("[TLS_RECOVERY] Claim retry timeout (30s), heap_internal=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      } else if (elapsed > 500 && elapsed < 1000) {
        // Log once after memory settling period
        LOG_INFO("[TLS_RECOVERY] Claim retry window open (elapsed=%lu), heap_internal=%u",
                 elapsed, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      }
    }
  }

  if (g_provisioning_manager.isSetupModeActive() && prov_state == ProvisioningState::STATE_AP_SETUP) {
    unsigned long now = millis();
    unsigned long qr_resend_ms = g_provisioning_manager.isAppSessionActive(now) ? 15000UL : PROVISION_QR_RESEND_MS;
    if (g_pending_provision_qr || (now - g_last_provision_qr_ms) >= qr_resend_ms) {
      send_provision_qr();
      g_pending_provision_qr = false;
      g_last_provision_qr_ms = now;
    }
  }
  if (g_defer_ota_post_provision && !g_provisioning_manager.isSetupModeActive()) {
    g_defer_ota_post_provision = false;
    if (ota_peer_accept_new_request()) {
      g_ota_skip_logged = false;
      maybeRunOtaCheck("post_provision", true);
    }
  }

  if (wifi_connected) {
    // Idempotent: no-op if the provisioning STATE_CONNECTED path already kicked SNTP.
    halo_prod_kick_time_sync("sntp_start");
    if (is_time_valid()) {
      time_t now = time(nullptr);
      ota_sched_update_next_epoch(now);
    }
    g_health_gate.markWifiConnected();
  } else {
    g_health_gate.markWifiDisconnected();
  }
  // Wait for setup mode to END before the first list refresh.
  //
  // This used to fire the moment WiFi connected, which is while the SoftAP and
  // its HTTP server are STILL UP -- so a TLS handshake ran with AP+STA both
  // active. Internal heap fell to ~18 KB and mbedtls aborted inside a printf
  // (lock_init_generic could not allocate a mutex): the device rebooted and the
  // user's first capture was destroyed (2026-08-31).
  //
  // The OTA post_provision check directly above already waits for exactly this
  // ("defer post_provision check until setup mode ends"); the list refresh did
  // not, and it is the one that runs first. Deferring also stops the capture
  // queueing behind a 16s list fetch, which is what made the first check-in take
  // ~10s to reach the shutter.
  if (g_post_provision_list_refresh_pending && wifi_connected &&
      !g_provisioning_manager.isSetupModeActive()) {
    g_post_provision_list_refresh_pending = false;
    LOG_INFO("[PROVISION] Connected + setup mode ended - requesting initial list refresh");
    request_list_refresh("post_provision", true);
  }
  if (wifi_connected && !last_wifi_connected) {
    mqtt_reset_failover_to_primary();
    s_mqtt_awake_failover_ms = 0;
    dump_system_truth("wifi_connected");
  }
  last_wifi_connected = wifi_connected;

  g_health_gate.update();
  g_ota_pending_verify_active = g_health_gate.getPendingVerify() && !g_health_gate.getMarkedValid();
  if (g_coord_pending[0] && g_coord_completion_target[0] &&
      strcmp(g_coord_completion_target, kFirmwareVersion) == 0 && !get_lcd_ota_due_nvs()) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_VALID) {
      ota_peer_schedule_complete();
      if (!g_coord_pending[0]) g_coord_completion_target[0] = 0;
      if (!g_coord_pending[0] && g_boot_ota_pending) {
        g_ota_check_done = true; g_peer_episode_finished = true;
        ota_peer_cancel("verified_postboot_complete");
        boot_ota_finish("verified_postboot_complete");
      }
    }
  }
  nightly_maintenance_tick();
  maybe_cancel_manual_ota_unready();
  handle_ota_proof();
  handle_mqtt_commands();
  // Arm-time delivery race fix: hold the LCD awake through the post-wake
  // connect/fetch phase (before pending-sync is even set) so it doesn't
  // idle-sleep before the real MAINT_WINDOW can be delivered. Must run BEFORE
  // sync_pending_maintenance_to_lcd() so a keepalive precedes any window send.
  keep_lcd_awake_during_maint_arm();
  sync_pending_maintenance_to_lcd("awake");

  // --- Awake-path schedule fetch (arm-flake fix) ---------------------------
  // The schedule fetch otherwise lives only in pre_sleep, so a continuously
  // active device never picks up a freshly-posted window before its start_epoch
  // passes (the schedule GET is future-only). Refresh while awake (only after
  // we've been awake a while, throttled) so the NVS window is current; the
  // existing sleep-entry arm (sense_config_deep_sleep_wakeup) then arms it.
  {
    static unsigned long s_last_awake_sched_attempt_ms = 0;
    const unsigned long AWAKE_SCHED_MIN_AWAKE_MS = 20000UL;   // only sustained wakes
    const unsigned long AWAKE_SCHED_RETRY_MS     = 30000UL;   // backstop between attempts
    const int32_t       AWAKE_SCHED_AGE_S        = 90;        // refetch if last fetch >90s
    unsigned long now_ms = millis();
    bool awake_long_enough = (now_ms - last_wake_ms) >= AWAKE_SCHED_MIN_AWAKE_MS;
    bool attempt_throttle_ok = (s_last_awake_sched_attempt_ms == 0) ||
                               ((now_ms - s_last_awake_sched_attempt_ms) >= AWAKE_SCHED_RETRY_MS);
    int32_t fetch_age = truth_get_sched_fetch_age_s();
    bool fetch_overdue = (fetch_age < 0) || (fetch_age >= AWAKE_SCHED_AGE_S);
    if (awake_long_enough && attempt_throttle_ok && fetch_overdue &&
        ota_sched_http_configured() && wifi_is_connected() && is_time_valid() &&
        !sense_action_inflight() && !g_lcd_ota_task_running && !g_maintenance_mode &&
        // Never run schedule-fetch (or the window_armed report) TLS during
        // provisioning/setup: the SoftAP is still up (AP+STA fragments internal
        // RAM) AND the 16KB camera DMA reserve is held, so the TLS esp-aes
        // DMA alloc panics -> ESP_RST_PANIC -> reboot before provisioned/owner
        // -claimed. Runs normally once provisioned and out of setup mode.
        !halo_provisioning_active()) {
      s_last_awake_sched_attempt_ms = now_ms;
      uint32_t to_ms = OTA_SCHED_HTTP_TIMEOUT_MS < 5000UL ? OTA_SCHED_HTTP_TIMEOUT_MS : 5000UL;
      LOG_INFO("[AWAKE_SCHED] fetch awake_ms=%lu fetch_age_s=%ld",
               (unsigned long)(now_ms - last_wake_ms), (long)fetch_age);
      if (maintenance_schedule_pending_sync_to_lcd()) {
        sync_pending_maintenance_to_lcd("awake_sched_fetch");
      }

      // "window_armed" telemetry is DISABLED: the backend rejects it outright.
      //
      //   [OTA_REPORT] window_armed failed code=400
      //   resp={"ok": false, "error": "invalid_field", "field": "report_type"}
      //
      // Observed 4 times in a 19-cycle soak, every single attempt. It is
      // vestigial telemetry from the maintenance-window orchestrator the nightly
      // redesign replaced, and the backend has moved on -- "pre_sleep" reports
      // from the same function still return 200, so this is the value, not the
      // endpoint.
      //
      // Worse than a harmless failure: the de-dupe below only records the
      // request_id ON SUCCESS, so a permanently-failing report is retried on
      // EVERY awake schedule fetch rather than once. Each attempt is a full TLS
      // handshake on the pre-sleep path -- which is precisely where contiguous
      // internal SRAM is scarcest and where the S3 upload has to get its own
      // handshake moments later (see the DMA starvation work in this repo).
      //
      // Re-enable only alongside a backend that accepts the value.
#if HALO_OTA_REPORT_WINDOW_ARMED
      {
        static char s_last_armed_report_request_id[64] = {0};
        MaintenanceWindow armed_mw;
        if (maintenance_window_load(&armed_mw) && armed_mw.request_id[0] &&
            strncmp(s_last_armed_report_request_id, armed_mw.request_id,
                    sizeof(s_last_armed_report_request_id)) != 0) {
          uint32_t report_to_ms =
              OTA_REPORT_HTTP_TIMEOUT_MS < 5000UL ? OTA_REPORT_HTTP_TIMEOUT_MS : 5000UL;
          if (ota_report_post("window_armed", nullptr, report_to_ms)) {
            strncpy(s_last_armed_report_request_id, armed_mw.request_id,
                    sizeof(s_last_armed_report_request_id) - 1);
            s_last_armed_report_request_id[sizeof(s_last_armed_report_request_id) - 1] = '\0';
            LOG_INFO("[OTA_REPORT] window_armed sent request_id=%s", armed_mw.request_id);
          }
        }
      }
#endif
    }
  }
  // ------------------------------------------------------------------------

  if (!g_ota_check_done && OtaIntent::shouldUpdateNow()) {
    MqttMetrics metrics = {};
    mqtt_get_metrics(&metrics);
    if (metrics.connected) {
      maybeRunOtaCheck("awake", true);
    }
  }

  if (g_ota_check_requested) {
    g_ota_check_requested = false;
    g_ota_check_done = false;
    g_ota_skip_logged = false;
    maybeRunOtaCheck("mqtt_cmd", true);
    // If maybeRunOtaCheck() hit a transient guard (e.g. time not valid yet),
    // restore the request so it retries on the next loop iteration.
    if (!g_ota_check_in_progress && !g_ota_check_done) {
      g_ota_check_requested = true;
    }
  }
}

bool truth_get_ota_sched_enable() {
  MaintenanceWindow mw;
  return g_ota_sched.enabled || maintenance_window_load(&mw);
}

uint32_t truth_get_next_ota_epoch() {
  uint32_t retry_epoch = maintenance_followup_retry_pending() ? g_maint_followup_retry_wake_epoch : 0;
  if (is_time_valid()) {
    time_t now = time(nullptr);
    if (now > 0) {
      MaintenanceWindow mw;
      if (maintenance_window_load(&mw)) {
        uint32_t mw_epoch = (uint32_t)mw.nextWakeEpochForSleep((uint64_t)now, 15, 5);
        if (retry_epoch > 0 && (mw_epoch == 0 || retry_epoch < mw_epoch)) {
          return retry_epoch;
        }
        return mw_epoch;
      }
    }
  }
  if (retry_epoch > 0) {
    return retry_epoch;
  }
  return g_next_ota_epoch;
}

int truth_get_wake_cause() {
  return (int)g_last_wake_cause;
}

bool truth_get_maintenance_mode() {
  return g_maintenance_mode;
}

bool truth_get_maintenance_in_window() {
  return g_maintenance_in_window;
}

const char* truth_get_sched_fetch_state() {
  return g_http_sched_last_status;
}

int32_t truth_get_sched_fetch_http_code() {
  return g_http_sched_last_http_code;
}

int32_t truth_get_sched_fetch_age_s() {
  if (g_http_sched_last_check_epoch == 0) {
    return -1;
  }
  time_t now = time(nullptr);
  if (now <= 0) {
    return -1;
  }
  int32_t age_s = static_cast<int32_t>(now - g_http_sched_last_check_epoch);
  return (age_s >= 0) ? age_s : -1;
}

const char* truth_get_sched_fetch_request_id() {
  return g_http_sched_last_request_id;
}

const char* truth_get_sched_last_event() {
  return g_sched_last_event;
}

int32_t truth_get_sched_last_event_age_s() {
  if (g_sched_last_event_epoch == 0) {
    return -1;
  }
  time_t now = time(nullptr);
  if (now <= 0) {
    return -1;
  }
  int32_t age_s = static_cast<int32_t>(now - g_sched_last_event_epoch);
  return (age_s >= 0) ? age_s : -1;
}

const char* truth_get_sched_last_event_request_id() {
  return g_sched_last_event_request_id;
}

const char* truth_get_lcd_ota_result() {
  return g_lcd_ota_result;
}

const char* truth_get_lcd_fw_version() {
  return g_lcd_ota_version;
}

// True when any OTA activity is in flight (LCD proxy owns UART, an LCD OTA
// request/transfer is active, or a Sense OTA check/apply is running).
// Used by Sense_Minimal.ino to suppress non-OTA HTTP (list refresh) that
// would otherwise starve the single net stack and stall the OTA download.
bool lcd_ota_in_progress() {
  return g_lcd_ota_proxy_owns_uart ||
         g_lcd_ota_request_active ||
         g_lcd_ota_task_running ||
         g_ota_apply_in_progress ||
         g_ota_check_in_progress;
}

// Real LCD running partition state, captured from the most recent
// LCD_OTA_QUERY_RESP (sense_ota_lcd.h getter). Observability only.
// sense_lcd_last_running_state() is visible here because this TU includes
// Sense_Minimal.ino which includes sense_ota_lcd.h.
const char* truth_get_lcd_running_state() {
  const char* s = sense_lcd_last_running_state();
  return (s && s[0]) ? s : "UNKNOWN";
}

int32_t truth_get_lcd_fw_age_s() {
  if (g_lcd_ota_last_request_epoch == 0) {
    return -1;
  }
  time_t now = time(nullptr);
  if (now <= 0) {
    return -1;
  }
  int32_t delta = static_cast<int32_t>(now - g_lcd_ota_last_request_epoch);
  return (delta >= 0) ? delta : -1;
}

bool truth_get_lcd_maint_ack() {
  return g_lcd_maint_ack_received;
}

int32_t truth_get_lcd_maint_ack_age_s() {
  if (!g_lcd_maint_ack_received) {
    return -1;
  }
  if (g_lcd_maint_ack_epoch > 0) {
    time_t now = time(nullptr);
    if (now > 0) {
      int32_t age_s = static_cast<int32_t>(now - g_lcd_maint_ack_epoch);
      return (age_s >= 0) ? age_s : -1;
    }
  }
  if (g_lcd_maint_ack_ms == 0) {
    return -1;
  }
  unsigned long now_ms = millis();
  unsigned long age_ms = now_ms >= g_lcd_maint_ack_ms ? (now_ms - g_lcd_maint_ack_ms) : 0;
  return static_cast<int32_t>(age_ms / 1000UL);
}

int32_t truth_get_lcd_maint_ack_remaining_s() {
  if (!g_lcd_maint_ack_received) {
    return -1;
  }
  return static_cast<int32_t>(g_lcd_maint_ack_remaining_s);
}

int32_t truth_get_lcd_maint_ack_wake_in_s() {
  if (!g_lcd_maint_ack_received) {
    return -1;
  }
  return static_cast<int32_t>(g_lcd_maint_ack_wake_in_s);
}

const char* truth_get_lcd_maint_ack_request_id() {
  return g_lcd_maint_ack_request_id;
}

const char* truth_get_lcd_maint_ack_status() {
  return g_lcd_maint_ack_status;
}

bool truth_get_lcd_maint_ack_persisted() {
  return g_lcd_maint_ack_persisted != 0;
}

uint64_t truth_get_lcd_maint_ack_start_epoch() {
  return g_lcd_maint_ack_start_epoch;
}

uint32_t truth_get_lcd_maint_ack_duration_sec() {
  return g_lcd_maint_ack_duration_sec;
}

uint32_t truth_get_lcd_maint_ack_grace_before_sec() {
  return g_lcd_maint_ack_grace_before_sec;
}

uint32_t truth_get_lcd_maint_ack_grace_after_sec() {
  return g_lcd_maint_ack_grace_after_sec;
}

bool truth_get_maint_sync_pending() {
  return maintenance_schedule_pending_sync_to_lcd();
}

uint32_t truth_get_maint_sync_attempts() {
  return g_maint_sync_send_count;
}

const char* truth_get_maint_sync_resolution() {
  return g_maint_sync_resolution;
}

const char* truth_get_maint_last_tx_request_id() {
  return g_maint_last_tx_request_id;
}

int32_t truth_get_maint_last_tx_age_s() {
  if (g_maint_last_tx_epoch == 0) {
    return -1;
  }
  time_t now = time(nullptr);
  if (now <= 0) {
    return -1;
  }
  int32_t age_s = static_cast<int32_t>(now - g_maint_last_tx_epoch);
  return (age_s >= 0) ? age_s : -1;
}

int32_t truth_get_maint_last_tx_remaining_s() {
  if (g_maint_last_tx_epoch == 0) {
    return -1;
  }
  return static_cast<int32_t>(g_maint_last_tx_remaining_s);
}

int32_t truth_get_maint_last_tx_wake_in_s() {
  if (g_maint_last_tx_epoch == 0) {
    return -1;
  }
  return static_cast<int32_t>(g_maint_last_tx_wake_in_s);
}

bool truth_get_maint_last_tx_clear() {
  return g_maint_last_tx_clear != 0;
}

bool truth_get_maint_last_tx_link_recent() {
  return g_maint_last_tx_link_recent != 0;
}

uint32_t truth_get_boot_count() {
  return g_boot_count;
}

bool truth_get_reboot_loop_detected() {
  return g_reboot_loop_detected;
}

uint32_t truth_get_uart_tx_count() {
  return uart_tx_count;
}

uint32_t truth_get_uart_rx_count() {
  return uart_rx_count;
}

const char* truth_get_last_uart_tx_type() {
  return last_uart_tx_type;
}

const char* truth_get_last_uart_rx_type() {
  return last_uart_rx_type;
}

const char* truth_get_last_action() {
  return last_action_label;
}

int32_t truth_get_last_action_age_ms() {
  if (last_action_ms == 0) {
    return -1;
  }
  unsigned long now_ms = millis();
  return static_cast<int32_t>(now_ms - last_action_ms);
}

const char* truth_get_lcd_diag_wake() {
  return lcd_diag_wake;
}

const char* truth_get_lcd_diag_screen() {
  return lcd_diag_screen;
}

const char* truth_get_lcd_diag_input() {
  return lcd_diag_last_input;
}

const char* truth_get_lcd_diag_last_tx() {
  return lcd_diag_last_tx;
}

const char* truth_get_lcd_diag_last_rx() {
  return lcd_diag_last_rx;
}

int32_t truth_get_lcd_diag_input_age_ms() {
  return lcd_diag_input_age_ms;
}

int32_t truth_get_lcd_diag_sense_rx_age_ms() {
  return lcd_diag_sense_rx_age_ms;
}

uint32_t truth_get_lcd_diag_uart_tx() {
  return lcd_diag_uart_tx;
}

uint32_t truth_get_lcd_diag_uart_rx() {
  return lcd_diag_uart_rx;
}

int32_t truth_get_lcd_diag_age_ms() {
  if (last_lcd_diag_ms == 0) {
    return -1;
  }
  unsigned long now_ms = millis();
  return static_cast<int32_t>(now_ms - last_lcd_diag_ms);
}

const char* truth_get_last_error_stage() {
  return last_error_stage;
}

int32_t truth_get_last_error_code() {
  return last_error_code;
}

const char* truth_get_last_error_text() {
  return last_error_text;
}

int32_t truth_get_last_error_age_ms() {
  if (last_error_ms == 0) {
    return -1;
  }
  unsigned long now_ms = millis();
  return static_cast<int32_t>(now_ms - last_error_ms);
}

const char* truth_get_recent_action_log() {
  return sense_get_recent_action_log();
}

const char* truth_get_camera_timeline() {
  return sense_get_camera_timeline();
}

uint32_t truth_get_upload_cache_count() {
  return sense_get_upload_cache_count();
}

const char* truth_get_upload_cache_last_result() {
  return sense_get_upload_cache_last_result();
}

const char* truth_get_upload_cache_last_reason() {
  return sense_get_upload_cache_last_reason();
}

int32_t truth_get_upload_cache_last_age_ms() {
  return sense_get_upload_cache_last_age_ms();
}

uint32_t truth_get_upload_cache_last_retries() {
  return sense_get_upload_cache_last_retries();
}

const char* truth_get_last_stage() {
  return sense_get_last_stage();
}

int32_t truth_get_last_stage_code() {
  return sense_get_last_stage_code();
}

int32_t truth_get_last_stage_uptime_ms() {
  return sense_get_last_stage_uptime_ms();
}

int32_t truth_get_prev_clean_shutdown() {
  return sense_get_prev_clean_shutdown();
}

uint32_t truth_get_crash_count() {
  return sense_get_crash_count();
}

const char* truth_get_last_crash_stage() {
  return sense_get_last_crash_stage();
}

int32_t truth_get_last_crash_stage_code() {
  return sense_get_last_crash_stage_code();
}

int32_t truth_get_last_crash_stage_uptime_ms() {
  return sense_get_last_crash_stage_uptime_ms();
}

int32_t truth_get_last_crash_reason() {
  return sense_get_last_crash_reason();
}

int32_t truth_get_last_crash_wake_cause() {
  return sense_get_last_crash_wake_cause();
}

void ota_on_timer_wake() {
  bool retry_wake = maintenance_followup_retry_consume_wake();
  g_maint_followup_retry_wake = retry_wake;
  bool maint_allowed = g_ota_sched.enabled;
  MaintenanceWindow mw;
  if (!maint_allowed && mw.loadFromNvs() && mw.scheduled) {
    maint_allowed = true;
  }
  if (retry_wake) {
    maint_allowed = true;
  }
  if (mw.scheduled) {
    sched_event_note("timer_wake", mw.request_id);
  } else if (retry_wake) {
    sched_event_note("timer_retry_wake", g_maint_followup_retry_request_id);
  } else if (maint_allowed) {
    sched_event_note("timer_wake", nullptr);
  }
  if (maint_allowed) {
    g_maintenance_mode = true;
    g_maintenance_handled = false;
  }
  LOG_INFO("[OTA_SCHED] wake reason=timer sched_enabled=%d mw_scheduled=%d retry_wake=%d",
           g_ota_sched.enabled ? 1 : 0,
           mw.scheduled ? 1 : 0,
           retry_wake ? 1 : 0);
  Serial.printf("[MAINT_WAKE] timer_wake sched_enabled=%d mw_scheduled=%d retry_wake=%d maint_mode=%d\n",
                g_ota_sched.enabled ? 1 : 0,
                mw.scheduled ? 1 : 0,
                retry_wake ? 1 : 0,
                g_maintenance_mode ? 1 : 0);
}

void ota_configure_timer_wakeup() {
  ota_sched_configure_timer_wakeup();
}

uint32_t ota_get_timer_delta_s() {
  return g_sleep_timer_delta_s;
}

void halo_prod_setup() {
  {
    Preferences p;
    if (p.begin("ota_coord", false)) {
      uint32_t next_boot = p.getUInt("generation", 0) + 1;
      if (!next_boot) next_boot = 1;
      if (p.putUInt("generation", next_boot) == sizeof(next_boot)) g_coord_sense_boot_id = next_boot;
      strlcpy(g_coord_schedule, p.getString("schedule", "").c_str(), sizeof(g_coord_schedule));
      strlcpy(g_coord_completed, p.getString("complete", "").c_str(), sizeof(g_coord_completed));
      ota_peer_load_completed_history(p.getString("done_ids", "").c_str());
      strlcpy(g_coord_pending, p.getString("pending", "").c_str(), sizeof(g_coord_pending));
      strlcpy(g_coord_completion_target, p.getString("target", "").c_str(), sizeof(g_coord_completion_target));
      p.end();
    }
  }
  // A reset after the atomic history commit but before key removal must not
  // attach a later manual request to the already completed pending ID.
  if (ota_peer_schedule_completed(g_coord_pending)) g_coord_pending[0] = 0;
  if (g_boot_ota_pending && strcmp(g_boot_ota_reason, "nightly") == 0 && g_coord_schedule[0]) {
    if (ota_peer_schedule_completed(g_coord_schedule)) {
      g_ota_check_done = true; g_peer_episode_finished = true;
      boot_ota_finish("schedule_already_completed");
    } else {
      strlcpy(g_coord_pending, g_coord_schedule, sizeof(g_coord_pending));
      Preferences p;
      if (p.begin("ota_coord", false)) { p.putString("pending", g_coord_pending); p.end(); }
    }
  }
  if (!g_boot_ota_pending && g_coord_pending[0] &&
      !ota_peer_schedule_completed(g_coord_pending)) {
    boot_ota_queue("coord_recovery");
  }
  // Override mbedTLS allocator: allow PSRAM for TLS buffers.
  // The prebuilt libs use CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC which restricts
  // mbedTLS to internal SRAM only (~32KB available). Standard calloc/free
  // with CONFIG_SPIRAM_USE_MALLOC routes large allocations (>4KB) to PSRAM.
  mbedtls_platform_set_calloc_free(calloc, free);
  Serial.printf("[TLS_PSRAM] mbedTLS allocator overridden to use default heap (PSRAM-capable)\n");

  g_boot_time_ms = millis();
  ensure_timezone_pt("boot");
  halo_wifi_guard_boot_log();

  // Wire the deep-sleep path to co-schedule the LCD's maintenance wake, so the
  // LCD is awake for the LCD-OTA proxy on the unattended nightly path.
  g_lcd_maint_coschedule_hook = prod_co_schedule_lcd_maint_wake;

  log_ota_partition_info();
#if OTA_TEST_BUILD
  ota_test_load_prefs();
#endif

  ota_sched_load();
  ota_sched_self_test();
#if OTA_SCHED_DEBUG_ON_BOOT
  ota_sched_print_status("boot");
#endif
  g_last_wake_cause = esp_sleep_get_wakeup_cause();
  g_maintenance_mode = ((g_last_wake_cause == ESP_SLEEP_WAKEUP_TIMER && g_ota_sched.enabled) ||
                        g_maint_followup_retry_wake);
#if HALO_OTA_POLICY_MAINTENANCE_ONLY
  {
    time_t now_s = time(nullptr);
    uint64_t now_epoch = (now_s >= 0) ? (uint64_t)now_s : 0ULL;
    MaintenanceWindow mw;
    bool in_window = mw.loadFromNvs() && mw.isWithinWindow(now_epoch);
    bool consumed = in_window ? maintenance_window_is_consumed(mw, now_epoch) : false;
    if (in_window && !consumed) {
      g_maintenance_mode = true;
    } else if (consumed) {
      sched_event_note("boot_consumed", mw.request_id);
    }
    Serial.printf("[MAINT] wake_cause=%d now=%llu in_window=%d consumed=%d retry_wake=%d maint_mode=%d\n",
                  (int)g_last_wake_cause, (unsigned long long)now_epoch,
                  in_window ? 1 : 0, consumed ? 1 : 0,
                  g_maint_followup_retry_wake ? 1 : 0,
                  g_maintenance_mode ? 1 : 0);
  }
#endif

  if (get_lcd_ota_due_nvs()) {
    LOG_INFO("[MAINT] lcd_ota_due from NVS - queueing bounded recovery check");
    // An independently recorded LCD debt can still heal after this date's
    // scheduled check completed; it is a recovery, not another nightly run.
    if (!g_boot_ota_pending) { g_peer_episode_finished = false; g_ota_check_done = false; }
    boot_ota_queue("lcd_due");
  }

  // Repopulate the inline LCD OTA outcome saved before a Sense self-OTA
  // reboot: updated/target or noop/queried version. Keys are consumed once;
  // the live pre_sleep LCD query confirms the real running version.
  load_lcd_ota_result_nvs();

  BootState::init();
  maybe_set_reboot_guard_override();
  {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running) {
      esp_ota_img_states_t ota_state;
      if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        g_ota_pending_verify_active = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
      }
    }
    if (g_ota_pending_verify_active) {
      LOG_INFO("[BOOT] ota_pending_verify=1 (sleep blocked until marked valid)");
    }
  }
  resolveOtaManifestUrl(ota_get_config_mutable());
  {
    const OtaUrlConfig* cfg = ota_get_config();
    Serial.printf("[OTA_CFG] sense_manifest_url=%s source=%s\n",
                  cfg->manifest_url[0] ? cfg->manifest_url : "(EMPTY)",
                  g_ota_config_source ? g_ota_config_source : "unknown");
  }
  g_boot_count = BootState::nextBootCount();
  {
    esp_reset_reason_t rr = esp_reset_reason();
    bool boot_was_crash = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                           rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
                           rr == ESP_RST_BROWNOUT);
    if (boot_was_crash) {
      // Only genuine crashes feed the reboot-loop guard.
      BootState::recordBootTimestamp();
      LOG_INFO("[BOOT] crash reset=%s recorded for reboot-loop guard", reset_reason_to_str(rr));
    } else {
      // Clean boot (deep-sleep wake / power-on / SW restart from OTA) = healthy:
      // clear the reboot-loop history so normal wakes and scheduled-OTA wakes can
      // never trip the guard. A real crash-loop still trips it (3 consecutive
      // crashes with no clean boot between).
      BootState::clearRebootHistory();
      LOG_INFO("[BOOT] reset=%s (clean) -> reboot-loop history cleared", reset_reason_to_str(rr));
    }
  }
  g_reboot_loop_detected = BootState::checkRebootLoop(3, 30000);

  ProvisioningState::init();
  g_last_prov_state = ProvisioningState::getState();
  if (wifi_is_connected()) {
    g_health_gate.markWifiConnected();
  } else {
    g_health_gate.markWifiDisconnected();
  }

  g_health_gate.markUartInitialized();
  Truth::setUartSyncEstablished(true);

  OtaIntent::init();
  handle_pending_ota_expectation();
  // Proactively announce the Sense firmware version on boot (fast, non-blocking:
  // reports sense_fw immediately + cached lcd_fw, no blocking LCD query) so the
  // LCD gets FW_INFO with the current sense_fw promptly after a manual OTA.
  // The SYNC handler also pushes FW_INFO, covering the case where the UART/LCD
  // link is not ready this early.
  uart_send_fw_info(false);

  mqtt_init();
  mqtt_set_allowed(false);

  if (g_pending_provision_qr && g_provisioning_manager.isSetupModeActive()) {
    send_provision_qr();
    g_pending_provision_qr = false;
  }

  log_mqtt_prefix();
  dump_system_truth("boot");
  log_health_diag("boot");

}

// ── loop task stack ───────────────────────────────────────────────────────
//
// At the END of the file on purpose. This is a STATEMENT, and the Arduino .ino
// preprocessor inserts its auto-generated prototypes immediately before the
// FIRST statement in the file. Anywhere earlier drags that point ahead of the
// types those prototypes mention -- near the includes it broke
// WiFiClientSecure, after the include block it broke OtaReportExtras. At the
// end the first statement is unchanged and nothing moves.
//
// WHY 16 KB: maybeRunOtaCheck() -- manifest fetch, TLS handshake, mbedTLS --
// runs on loopTask. At the Arduino default 8 KB it overflows. On 2026-08-31 the
// FIRST check-in after provisioning panicked with
//     Stack canary watchpoint triggered (loopTask)
// during the post_provision manifest fetch, while the capture sat at
// WAITING_INPUT for the expiry date. The reset wiped the image out of PSRAM:
// the user's first ever capture was lost and the UI looked like it had hung.
//
// Same failure and same fix as the LCD's uart_task (8 KB -> 12 KB after it
// tripped the identical canary parsing JSON). TLS needs more headroom than
// JSON. Note mbedTLS is pointed at PSRAM for its HEAP (see TLS_PSRAM) -- that
// does nothing for STACK, which is always internal.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

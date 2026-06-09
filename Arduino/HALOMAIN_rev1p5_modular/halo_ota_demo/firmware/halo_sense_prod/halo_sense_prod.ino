/*
 * HALO SENSE PROD
 *
 * Wrapper around Sense_Minimal with OTA/MQTT framework.
 * - Functional Sense code remains intact (imported from Sense_Minimal).
 * - Adds MQTT/TRUTH, OTA intent parsing, and OTA apply pipeline (manual trigger only).
 */

#define HALO_SENSE_PROD_WRAPPER 1
#define HALO_SENSE_UPLOAD_PERSISTENCE 1
#define OTA_TEST_BUILD 0
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

extern "C" bool halo_wifi_hard_reset_for_ota(const char* reason, uint32_t timeout_ms) {
  return wifi_hard_reset_and_reconnect(reason ? reason : "ota_http_retry", timeout_ms);
}

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
#ifndef OTA_DEFAULT_ENV
#define OTA_DEFAULT_ENV "dev"
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

static bool maintenance_followup_retry_schedule(const MaintenanceWindow* mw, const char* reason) {
  if (g_maint_followup_retry_attempts >= MAINT_FOLLOWUP_RETRY_MAX_ATTEMPTS) {
    LOG_INFO("[MAINT_RETRY] exhausted attempts=%u reason=%s",
             (unsigned)g_maint_followup_retry_attempts,
             reason ? reason : "unknown");
    return false;
  }
  uint32_t base_epoch = maint_sync_epoch_now();
  if (base_epoch == 0) {
    LOG_INFO("[MAINT_RETRY] schedule_failed reason=%s base_epoch=0",
             reason ? reason : "unknown");
    return false;
  }
  uint8_t next_attempt = g_maint_followup_retry_attempts;
  uint32_t delay_s = MAINT_FOLLOWUP_RETRY_DELAYS_S[next_attempt];
  g_maint_followup_retry_active = 1;
  g_maint_followup_retry_attempts = next_attempt + 1;
  g_maint_followup_retry_wake_epoch = base_epoch + delay_s;
  maint_sync_copy_str(g_maint_followup_retry_reason,
                      sizeof(g_maint_followup_retry_reason),
                      reason ? reason : "unknown");
  maint_sync_copy_str(g_maint_followup_retry_request_id,
                      sizeof(g_maint_followup_retry_request_id),
                      (mw && mw->request_id[0]) ? mw->request_id : "");
  LOG_INFO("[MAINT_RETRY] scheduled attempt=%u/%u delay_s=%lu wake_epoch=%lu reason=%s request_id=%s",
           (unsigned)g_maint_followup_retry_attempts,
           (unsigned)MAINT_FOLLOWUP_RETRY_MAX_ATTEMPTS,
           (unsigned long)delay_s,
           (unsigned long)g_maint_followup_retry_wake_epoch,
           g_maint_followup_retry_reason[0] ? g_maint_followup_retry_reason : "-",
           g_maint_followup_retry_request_id[0] ? g_maint_followup_retry_request_id : "-");
  sched_event_note("followup_retry", g_maint_followup_retry_request_id);
  return true;
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

static void maintenance_idle_diag_note(unsigned long idle_ms,
                                       bool gate_hit,
                                       bool bypassed,
                                       bool http_window,
                                       const char* reason) {
  g_maint_last_idle_ms = static_cast<uint32_t>(idle_ms);
  g_maint_last_idle_gate_hit = gate_hit ? 1 : 0;
  g_maint_last_idle_gate_bypassed = bypassed ? 1 : 0;
  g_maint_last_idle_http_window = http_window ? 1 : 0;
  maint_sync_copy_str(g_maint_last_idle_gate_reason,
                      sizeof(g_maint_last_idle_gate_reason),
                      reason ? reason : "unknown");
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

static void maintenance_window_mark_consumed(const MaintenanceWindow& mw, const char* reason) {
  maintenance_followup_retry_clear("consumed");
  g_maint_consumed_valid = 1;
  g_maint_consumed_start_epoch = mw.start_epoch;
  g_maint_consumed_end_epoch = mw.start_epoch + mw.duration_sec + mw.grace_after_sec;
  strncpy(g_maint_consumed_request_id, mw.request_id, sizeof(g_maint_consumed_request_id) - 1);
  g_maint_consumed_request_id[sizeof(g_maint_consumed_request_id) - 1] = '\0';
  sched_event_note(reason ? reason : "maint_consumed", mw.request_id);
  set_maintenance_schedule_pending_sync_to_lcd(true);
  LOG_INFO("[MAINT_GUARD] consumed request_id=%s start=%llu end=%llu reason=%s",
           mw.request_id[0] ? mw.request_id : "-",
           (unsigned long long)g_maint_consumed_start_epoch,
           (unsigned long long)g_maint_consumed_end_epoch,
           reason ? reason : "unknown");
}

static bool sense_action_inflight() {
  if (http_inflight || upload_inflight || waiting_for_mqtt_result) {
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
static void ota_sched_save();
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

static bool ota_override_enabled() {
  Preferences prefs;
  if (!prefs.begin("ota_override", true)) {
    return false;
  }
  bool enabled = prefs.getBool("ota_override", false);
  prefs.end();
  return enabled;
}

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

static bool maintenance_window_equals(const MaintenanceWindow& a, const MaintenanceWindow& b) {
  return a.scheduled == b.scheduled &&
         a.start_epoch == b.start_epoch &&
         a.duration_sec == b.duration_sec &&
         a.grace_before_sec == b.grace_before_sec &&
         a.grace_after_sec == b.grace_after_sec &&
         strncmp(a.request_id, b.request_id, sizeof(a.request_id)) == 0;
}

static bool ota_sched_http_configured() {
  return OTA_SCHED_HTTP_URL[0] != '\0';
}

static bool ota_sched_http_is_https(const char* url) {
  return url && strncmp(url, "https://", 8) == 0;
}

static void ota_sched_http_append_query(String& url, const char* key, const char* value) {
  if (!key || !key[0] || !value || !value[0]) {
    return;
  }
  url += (url.indexOf('?') >= 0) ? "&" : "?";
  url += key;
  url += '=';
  for (const char* p = value; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool safe = (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      url += static_cast<char>(c);
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      url += hex;
    }
  }
}

static String ota_sched_http_build_url() {
  String url = OTA_SCHED_HTTP_URL;
  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  char owner_id[64] = {0};
  bool owner_ok = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  ota_sched_http_append_query(url, "device_id", device_id);
  if (owner_ok && owner_id[0]) {
    ota_sched_http_append_query(url, "owner_id", owner_id);
  }
  ota_sched_http_append_query(url, "board", HALO_BOARD_NAME);
  ota_sched_http_append_query(url, "fw", kFirmwareVersion);
  ota_sched_http_append_query(url, "build", kBuildId);
  ota_sched_http_append_query(url, "channel", OTA_CHANNEL);
  ota_sched_http_append_query(url, "device_type", "sense");
  return url;
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

static bool ota_report_build_pre_sleep_payload(String& out) {
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
  payload["report_type"] = "pre_sleep";
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

  out = "";
  serializeJson(doc, out);
  if (out.length() == 0) {
    LOG_WARN("[OTA_REPORT] serialize failed");
    return false;
  }
  return true;
}

static bool ota_report_post_pre_sleep(uint32_t timeout_ms) {
  if (!OTA_REPORT_HTTP_URL[0]) {
    return false;
  }
  if (!wifi_is_connected()) {
    LOG_INFO("[OTA_REPORT] skip pre_sleep (wifi_down)");
    return false;
  }
  String body;
  if (!ota_report_build_pre_sleep_payload(body)) {
    return false;
  }

  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  char owner_id[64] = {0};
  bool owner_ok = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  MaintenanceWindow mw;
  bool has_mw = maintenance_window_load(&mw);

  HTTPClient http;
  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  bool started = false;
  if (ota_report_http_is_https(OTA_REPORT_HTTP_URL)) {
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
    LOG_INFO("[OTA_REPORT] pre_sleep ok code=%d body_len=%u", http_code, static_cast<unsigned>(body.length()));
    return true;
  }

  String response_preview = response.length() ? response.substring(0, 160) : String("-");
  LOG_WARN("[OTA_REPORT] pre_sleep failed code=%d resp=%s",
           http_code,
           response_preview.c_str());
  return false;
}

static JsonObject ota_sched_http_get_payload_root(DynamicJsonDocument& doc) {
  JsonVariant maintenance = doc["maintenance"];
  if (!maintenance.isNull() && maintenance.is<JsonObject>()) {
    return maintenance.as<JsonObject>();
  }
  return doc.as<JsonObject>();
}

static bool ota_sched_http_fetch_window(uint32_t timeout_ms) {
  if (!ota_sched_http_configured()) {
    ota_http_schedule_note("disabled", 0, nullptr);
    return false;
  }
  if (!wifi_is_connected()) {
    ota_http_schedule_note("wifi_down", 0, nullptr);
    return false;
  }
  if (!is_time_valid()) {
    ota_http_schedule_note("time_invalid", 0, nullptr);
    return false;
  }

  String url = ota_sched_http_build_url();
  LOG_INFO("[OTA_HTTP_SCHED] fetch url=%s timeout_ms=%lu", url.c_str(), (unsigned long)timeout_ms);
  // Forward URL to LCD for in-enclosure debugging (truncate if needed)
  {
    char url_detail[200];
    snprintf(url_detail, sizeof(url_detail), "url=%s", url.c_str());
    uart_send_sense_diag("ota_sched", "fetch_url", "OTA_SCHED", 0, url_detail);
  }

  HTTPClient http;
  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  int http_code = 0;
  String body;
  bool started = false;
  if (ota_sched_http_is_https(url.c_str())) {
#if OTA_SCHED_HTTP_INSECURE
    secure_client.setInsecure();
#else
    ota_http_configure_tls(secure_client, "OTA_HTTP_SCHED", OTA_SCHED_HTTP_ROOT_CA);
#endif
    secure_client.setHandshakeTimeout(15);
    secure_client.setTimeout(timeout_ms);
    started = http.begin(secure_client, url);
  } else {
    plain_client.setTimeout(timeout_ms);
    started = http.begin(plain_client, url);
  }
  if (!started) {
    ota_http_schedule_note("begin_fail", 0, nullptr);
    LOG_WARN("[OTA_HTTP_SCHED] begin failed url=%s", url.c_str());
    return false;
  }

  http.setConnectTimeout((int)timeout_ms);
  http.setTimeout((int)timeout_ms);
  http.setReuse(false);
  http_code = http.GET();
  if (http_code > 0) {
    body = http.getString();
  }
  http.end();

  time_t now_s = time(nullptr);
  uint64_t now_epoch = (now_s > 0) ? (uint64_t)now_s : 0ULL;

  MaintenanceWindow existing;
  bool had_existing = maintenance_window_load(&existing);

  if (http_code == HTTP_CODE_NO_CONTENT) {
    ota_http_schedule_note("none", http_code, nullptr);
    LOG_INFO("[OTA_HTTP_SCHED] no schedule (204) preserve_existing=%d", had_existing ? 1 : 0);
    return true;
  }

  if (http_code != HTTP_CODE_OK) {
    ota_http_schedule_note("http_error", http_code, nullptr);
    LOG_WARN("[OTA_HTTP_SCHED] http error code=%d", http_code);
    return false;
  }

  if (body.length() == 0) {
    ota_http_schedule_note("empty_body", http_code, nullptr);
    LOG_WARN("[OTA_HTTP_SCHED] empty response");
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, body);
  if (err || !doc.is<JsonObject>()) {
    ota_http_schedule_note("parse_error", http_code, nullptr);
    LOG_WARN("[OTA_HTTP_SCHED] parse error: %s", err.c_str());
    return false;
  }

  JsonObject root = ota_sched_http_get_payload_root(doc);
  if (root.isNull()) {
    ota_http_schedule_note("invalid_payload", http_code, nullptr);
    LOG_WARN("[OTA_HTTP_SCHED] invalid payload root");
    return false;
  }

  const char* request_id = root["request_id"] | "";
  if (!request_id || !request_id[0]) {
    request_id = doc["request_id"] | "";
  }
  bool enabled = root["enabled"].isNull() ? true : (root["enabled"] | false);
  if (!enabled) {
    if (had_existing) {
      existing.clear();
      maintenance_followup_retry_clear("http_clear");
      maintenance_window_consumed_clear("http_clear");
      set_maintenance_schedule_pending_sync_to_lcd(true);
      sched_event_note("http_cleared", request_id);
      ota_http_schedule_note("cleared", http_code, request_id);
      LOG_INFO("[OTA_HTTP_SCHED] cleared request_id=%s", request_id && request_id[0] ? request_id : "-");
      return true;
    }
    ota_http_schedule_note("none", http_code, request_id);
    LOG_INFO("[OTA_HTTP_SCHED] no schedule enabled=0");
    return true;
  }
  if (!request_id || !request_id[0]) {
    ota_http_schedule_note("missing_request_id", http_code, nullptr);
    LOG_WARN("[OTA_HTTP_SCHED] missing request_id");
    return false;
  }

  if (had_existing && request_id && request_id[0] &&
      strcmp(existing.request_id, request_id) != 0) {
    maintenance_followup_retry_clear("new_request_id");
    maintenance_window_consumed_clear("new_request_id");
  }

  MaintenanceWindow incoming;
  if (!incoming.setFromJson(root, now_epoch, 60)) {
    ota_http_schedule_note("invalid_window", http_code, request_id);
    LOG_WARN("[OTA_HTTP_SCHED] invalid future window request_id=%s",
             request_id && request_id[0] ? request_id : "-");
    return false;
  }

  int min_idle_min = root["min_idle_min"] | g_ota_sched.min_idle_min;
  if (doc["min_idle_min"].is<int>()) {
    min_idle_min = doc["min_idle_min"].as<int>();
  }
  g_ota_sched.min_idle_min = (uint8_t)clamp_int(min_idle_min, 0, 120);
  ota_sched_save();

  if (had_existing && maintenance_window_equals(existing, incoming)) {
    ota_http_schedule_note("unchanged", http_code, incoming.request_id);
    LOG_INFO("[OTA_HTTP_SCHED] unchanged request_id=%s start=%llu dur=%lu",
             incoming.request_id,
             (unsigned long long)incoming.start_epoch,
             (unsigned long)incoming.duration_sec);
    return true;
  }

  if (g_maint_followup_retry_attempts > 0 &&
      incoming.request_id[0] &&
      strcmp(g_maint_followup_retry_request_id, incoming.request_id) != 0) {
    maintenance_followup_retry_clear("http_saved_new_request");
  }

  incoming.saveToNvs();
  sched_event_note("http_saved", incoming.request_id);
  set_maintenance_schedule_pending_sync_to_lcd(true);
  ota_http_schedule_note("saved", http_code, incoming.request_id);
  LOG_INFO("[OTA_HTTP_SCHED] saved request_id=%s start=%llu dur=%lu grace_b=%lu grace_a=%lu min_idle_min=%u",
           incoming.request_id,
           (unsigned long long)incoming.start_epoch,
           (unsigned long)incoming.duration_sec,
           (unsigned long)incoming.grace_before_sec,
           (unsigned long)incoming.grace_after_sec,
           (unsigned)g_ota_sched.min_idle_min);
  return true;
}

// Cancel-safety: re-validate the LIVE cloud schedule at maintenance-window start.
// The device may have fetched+armed a window earlier; if the operator has since
// deleted/disabled/replaced it in the cloud, the cached RTC/NVS window must NOT
// be acted upon. This GET inspects the response directly (unlike
// ota_sched_http_fetch_window, which treats 204 as "keep existing window").
// NOTE: the schedule GET only returns enabled windows whose start_epoch is in
// the FUTURE, so a 204 means "no future window" — which includes a live,
// enabled window whose start has already passed (the normal wake-at-window
// case) as well as a truly deleted row. We therefore fail-open on 204. A real
// cancel must be expressed as enabled=false on the schedule row (HTTP 200),
// which returns at any wake time and reliably aborts.
//   204                      -> REVAL_FETCH_FAILED (no future window / past-start, fail-open)
//   200 enabled=false        -> REVAL_CANCELLED (schedule disabled)
//   200 enabled=true, rid !=  -> REVAL_REPLACED (different request_id)
//   200 enabled=true, rid ==  -> REVAL_VALID
//   any error / not ready    -> REVAL_FETCH_FAILED (fail-open, proceed with OTA)
static SchedRevalidate ota_sched_revalidate(const MaintenanceWindow& cached,
                                            uint64_t now_epoch,
                                            uint32_t timeout_ms) {
  (void)now_epoch;
  if (!ota_sched_http_configured() || !wifi_is_connected() || !is_time_valid()) {
    LOG_INFO("[MAINT_RUN] revalidate skip (not_ready) -> REVAL_FETCH_FAILED (fail-open)");
    return REVAL_FETCH_FAILED;
  }

  String url = ota_sched_http_build_url();
  LOG_INFO("[MAINT_RUN] revalidate GET url=%s timeout_ms=%lu", url.c_str(), (unsigned long)timeout_ms);

  HTTPClient http;
  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  int http_code = 0;
  String body;
  bool started = false;
  if (ota_sched_http_is_https(url.c_str())) {
#if OTA_SCHED_HTTP_INSECURE
    secure_client.setInsecure();
#else
    ota_http_configure_tls(secure_client, "OTA_HTTP_REVAL", OTA_SCHED_HTTP_ROOT_CA);
#endif
    secure_client.setHandshakeTimeout(15);
    secure_client.setTimeout(timeout_ms);
    started = http.begin(secure_client, url);
  } else {
    plain_client.setTimeout(timeout_ms);
    started = http.begin(plain_client, url);
  }
  if (!started) {
    LOG_WARN("[MAINT_RUN] revalidate begin failed -> REVAL_FETCH_FAILED (fail-open)");
    return REVAL_FETCH_FAILED;
  }

  http.setConnectTimeout((int)timeout_ms);
  http.setTimeout((int)timeout_ms);
  http.setReuse(false);
  http_code = http.GET();
  if (http_code > 0) {
    body = http.getString();
  }
  http.end();

  if (http_code == HTTP_CODE_NO_CONTENT) {
    // 204 = no FUTURE window. The schedule GET only returns enabled windows
    // whose start_epoch is still in the future. A live, enabled window whose
    // start has already passed (the normal case — the device wakes AT the
    // window) returns 204, as does a deleted row. Treating 204 as CANCELLED
    // would falsely abort legitimate scheduled OTAs, so fail-open here. A real
    // cancel is expressed as enabled=false (HTTP 200), which reliably aborts.
    LOG_INFO("[MAINT_RUN] revalidate http=204 (no future window / past-start) -> fail_open (REVAL_FETCH_FAILED)");
    return REVAL_FETCH_FAILED;
  }

  if (http_code != HTTP_CODE_OK) {
    LOG_WARN("[MAINT_RUN] revalidate http=%d -> REVAL_FETCH_FAILED (fail-open)", http_code);
    return REVAL_FETCH_FAILED;
  }

  if (body.length() == 0) {
    LOG_WARN("[MAINT_RUN] revalidate http=200 empty_body -> REVAL_FETCH_FAILED (fail-open)");
    return REVAL_FETCH_FAILED;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, body);
  if (err || !doc.is<JsonObject>()) {
    LOG_WARN("[MAINT_RUN] revalidate parse_error=%s -> REVAL_FETCH_FAILED (fail-open)", err.c_str());
    return REVAL_FETCH_FAILED;
  }

  JsonObject root = ota_sched_http_get_payload_root(doc);
  if (root.isNull()) {
    LOG_WARN("[MAINT_RUN] revalidate invalid_payload -> REVAL_FETCH_FAILED (fail-open)");
    return REVAL_FETCH_FAILED;
  }

  bool enabled = root["enabled"].isNull() ? true : (root["enabled"] | false);
  if (!enabled) {
    LOG_INFO("[MAINT_RUN] revalidate http=200 enabled=false -> REVAL_CANCELLED (disabled)");
    return REVAL_CANCELLED;
  }

  const char* resp_request_id = root["request_id"] | "";
  if (!resp_request_id || !resp_request_id[0]) {
    resp_request_id = doc["request_id"] | "";
  }
  if (cached.request_id[0] && resp_request_id && resp_request_id[0] &&
      strcmp(cached.request_id, resp_request_id) != 0) {
    LOG_INFO("[MAINT_RUN] revalidate http=200 request_id changed cached=%s live=%s -> REVAL_REPLACED",
             cached.request_id, resp_request_id);
    return REVAL_REPLACED;
  }

  LOG_INFO("[MAINT_RUN] revalidate http=200 request_id=%s -> REVAL_VALID",
           (resp_request_id && resp_request_id[0]) ? resp_request_id : "-");
  return REVAL_VALID;
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
                        mqtt_ota_check_requested;
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

static void send_ota_uart_message(const char* type) {
  if (!type || !type[0]) return;
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = type;
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

static bool send_lcd_ota_check_request(const char* reason, bool allow_reboot) {
  if (g_lcd_ota_request_active) {
    LOG_INFO("[LCD_OTA_ORCH] skip already_active");
    return false;
  }
  uint32_t req_id = get_next_msg_id();
  g_lcd_ota_request_id = req_id;
  g_lcd_ota_recent_request_id = req_id;
  g_lcd_ota_request_active = true;
  g_lcd_ota_ack_received = false;
  g_lcd_ota_done = false;
  g_lcd_ota_request_start_ms = millis();
  strncpy(g_lcd_ota_result, "pending", sizeof(g_lcd_ota_result) - 1);
  g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
  g_lcd_ota_version[0] = '\0';
  if (is_time_valid()) {
    g_lcd_ota_last_request_epoch = (uint32_t)time(nullptr);
  }

  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "OTA_CHECK";
  doc["msg_id"] = req_id;
  doc["ts"] = millis();
  doc["request_id"] = req_id;
  doc["allow_reboot"] = allow_reboot ? true : false;
  if (reason && reason[0]) {
    doc["reason"] = reason;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  LOG_INFO("[LCD_OTA_ORCH] tx OTA_CHECK request_id=%lu reason=%s allow_reboot=%d",
           (unsigned long)req_id,
           reason ? reason : "unknown",
           allow_reboot ? 1 : 0);
  return true;
}

void halo_prod_request_manual_ota(const char* reason) {
  manual_ota_override_set(reason ? reason : "manual");
  const uint32_t ts = is_time_valid() ? (uint32_t)time(nullptr) : 0;
  OtaIntent::updateDesired(nullptr, nullptr, true, false, ts, "manual");
  g_ota_check_requested = true;
  g_ota_check_done = false;
  g_ota_skip_logged = false;
  if (g_ota_check_in_progress || g_ota_apply_in_progress) {
    LOG_INFO("[OTA_MANUAL] request queued (busy)");
  } else {
    LOG_INFO("[OTA_MANUAL] request accepted");
  }
  // Queue the LCD OTA request behind an explicit lock so the LCD defers
  // its own apply until Sense finishes and later sends OTA_UNLOCK.
  // LCD OTA is now proxy-driven: Sense will fetch the manifest and stream
  // the binary to LCD during the maintenance window via maybe_trigger_lcd_ota_check().
  send_ota_uart_message("OTA_LOCK");
  g_lcd_ota_done = false;
  strncpy(g_lcd_ota_result, "pending", sizeof(g_lcd_ota_result) - 1);
  g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
  g_lcd_ota_attempted_this_window = false;
  LOG_INFO("[LCD_OTA_ORCH] manual lcd ota queued (proxy mode)");
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

static void keep_lcd_awake_for_maintenance(uint32_t remaining_s, const char* reason) {
  if (remaining_s == 0) {
    return;
  }
  MaintenanceWindow mw;
  bool has_window = maintenance_window_load(&mw);
  unsigned long start_ms = millis();
  unsigned long last_tx_ms = 0;
  bool link_recent = false;
  bool sent_after_link = false;
  while ((millis() - start_ms) < LCD_MAINT_KEEPALIVE_MS) {
    pump_uart_rx_once();
    link_recent = halo_uart_link_recent(LCD_MAINT_LINK_RECENT_MS);
    unsigned long now_ms = millis();
    if (last_tx_ms == 0 || (now_ms - last_tx_ms) >= LCD_MAINT_RESEND_INTERVAL_MS) {
      send_maint_window(has_window ? &mw : nullptr, remaining_s, 0, false);
      last_tx_ms = now_ms;
      if (link_recent) {
        sent_after_link = true;
      }
    }
    if (link_recent && sent_after_link) {
      LOG_INFO("[MAINT_TX] keepalive_done reason=%s link_recent=1",
               reason ? reason : "unknown");
      return;
    }
    delay(50);
  }
  LOG_INFO("[MAINT_TX] keepalive_timeout reason=%s link_recent=%d",
           reason ? reason : "unknown",
           link_recent ? 1 : 0);
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
  vTaskDelete(NULL);
}

static void maybe_trigger_lcd_ota_check() {
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

static bool ensure_maintenance_wifi_connected() {
  const uint32_t initial_timeout_ms = 15000;
  if (ensure_wifi_connected("maintenance", initial_timeout_ms)) {
    return true;
  }

  uint32_t retry_timeout_ms = initial_timeout_ms >= 3000 ? 3000 : initial_timeout_ms;
  if (retry_timeout_ms >= 1000) {
    LOG_INFO("[MAINT_WIFI] retry wifi (hard reset) timeout_ms=%lu",
             (unsigned long)retry_timeout_ms);
    if (wifi_hard_reset_and_reconnect("maintenance_retry", retry_timeout_ms)) {
      LOG_INFO("[MAINT_WIFI] recovered after hard reset retry");
      return true;
    }
  }

  unsigned long start_ms = millis();
  unsigned long next_retry_ms = start_ms;
  while ((millis() - start_ms) < PRE_SLEEP_WIFI_RECOVERY_MS) {
    if (wifi_is_connected()) {
      LOG_INFO("[MAINT_WIFI] recovered during recovery_window");
      return true;
    }

    unsigned long now_ms = millis();
    if (now_ms >= next_retry_ms) {
      uint32_t remaining_ms =
          (uint32_t)(PRE_SLEEP_WIFI_RECOVERY_MS - (now_ms - start_ms));
      uint32_t attempt_ms =
          (remaining_ms > PRE_SLEEP_WIFI_HARD_RESET_MS)
              ? PRE_SLEEP_WIFI_HARD_RESET_MS
              : remaining_ms;
      if (attempt_ms >= 1000) {
        LOG_INFO("[MAINT_WIFI] recovery retry timeout_ms=%lu",
                 (unsigned long)attempt_ms);
        if (wifi_hard_reset_and_reconnect("maintenance_recover", attempt_ms)) {
          LOG_INFO("[MAINT_WIFI] recovered during recovery_retry");
          return true;
        }
      }
      next_retry_ms = now_ms + PRE_SLEEP_WIFI_RETRY_INTERVAL_MS;
    }
    delay(200);
  }

  LOG_INFO("[MAINT_WIFI] failed after recovery_window");
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

static void ensure_timezone_pt(const char* reason) {
  if (g_tz_initialized) {
    return;
  }
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
  tzset();
  g_tz_initialized = true;
  LOG_INFO("[TZ] set=PT reason=%s", reason ? reason : "unknown");
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
  if (!g_ota_sched.enabled || now <= 0) {
    return;
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
  return mw->scheduled && mw->start_epoch > 0;
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

static void ota_sched_reschedule_after_failure(time_t now, bool time_valid) {
  if (!g_ota_sched.enabled) {
    return;
  }
  uint32_t base = time_valid ? (uint32_t)now : g_last_time_sync_epoch;
  if (base == 0) {
    return;
  }
  uint32_t window_start = g_next_ota_epoch;
  if (window_start == 0) {
    window_start = ota_sched_compute_next_epoch(base);
  }
  uint32_t window_end = window_start + (uint32_t)g_ota_sched.window_dur_min * 60;
  uint32_t candidate = base + 3600;
  if (candidate <= window_end) {
    g_next_ota_epoch = candidate;
  } else {
    g_next_ota_epoch = ota_sched_compute_next_epoch(base);
  }
  ota_sched_save();
}

static bool ota_sched_in_window(time_t now) {
  if (g_next_ota_epoch == 0) {
    return false;
  }
  uint32_t start = g_next_ota_epoch;
  uint32_t end = start + (uint32_t)g_ota_sched.window_dur_min * 60;
  return (now >= (time_t)start && now <= (time_t)end);
}

static bool maintenance_window_active_for_retry(uint32_t window_end_epoch, uint32_t* remaining_s_out) {
  if (remaining_s_out) {
    *remaining_s_out = 0;
  }
  if (window_end_epoch == 0 || !is_time_valid()) {
    return false;
  }
  uint32_t now_epoch = (uint32_t)time(nullptr);
  if (now_epoch >= window_end_epoch) {
    return false;
  }
  if (remaining_s_out) {
    *remaining_s_out = window_end_epoch - now_epoch;
  }
  return true;
}

static bool sense_maintenance_result_retryable(const char* result) {
  if (!result || !result[0] || strcmp(result, "pending") == 0 || strcmp(result, "check_begin") == 0) {
    return true;
  }
  if (strcmp(result, "apply_success") == 0 ||
      strcmp(result, "up_to_date") == 0 ||
      strcmp(result, "downgrade_blocked") == 0 ||
      strcmp(result, "rollout_min_version") == 0 ||
      strcmp(result, "rollout_skip") == 0 ||
      strcmp(result, "manifest_url_invalid") == 0 ||
      strcmp(result, "board_mismatch") == 0 ||
      strcmp(result, "bin_url_disallowed") == 0) {
    return false;
  }
  if (strncmp(result, "apply_blocked:", 14) == 0) {
    return false;
  }
  return true;
}

static bool lcd_maintenance_result_retryable(const char* result) {
  if (!result || !result[0] || strcmp(result, "pending") == 0) {
    return true;
  }
  if (strcmp(result, "updated") == 0 ||
      strcmp(result, "noop") == 0 ||
      strcmp(result, "skipped_user_active") == 0 ||
      strcmp(result, "skipped_rate_limited") == 0 ||
      strcmp(result, "skipped_no_request") == 0) {
    return false;
  }
  if (strncmp(result, "success:", 8) == 0 || strncmp(result, "no_update:", 10) == 0) {
    return false;
  }
  if (strncmp(result, "fail:user_active", 16) == 0 ||
      strncmp(result, "fail:reboot_not_allowed", 23) == 0 ||
      strncmp(result, "fail:maintenance_only", 21) == 0 ||
      strncmp(result, "fail:manifest_url_invalid", 25) == 0 ||
      strncmp(result, "fail:board_mismatch", 19) == 0 ||
      strncmp(result, "fail:bin_url_disallowed", 23) == 0 ||
      strncmp(result, "fail:guard_disabled", 19) == 0 ||
      strncmp(result, "fail:wake_window_retry_exhausted", 32) == 0) {
    return false;
  }
  return true;
}

static bool lcd_maintenance_result_successful(const char* result) {
  if (!result || !result[0]) {
    return false;
  }
  if (strcmp(result, "updated") == 0 ||
      strcmp(result, "noop") == 0) {
    return true;
  }
  return (strncmp(result, "success:", 8) == 0 ||
          strncmp(result, "no_update:", 10) == 0);
}

static bool maintenance_window_should_consume_after_run(const MaintenanceWindow& mw,
                                                        uint32_t window_end_epoch) {
  time_t now_s = time(nullptr);
  uint64_t now_epoch = (now_s > 0) ? (uint64_t)now_s : 0ULL;
  if (now_epoch > 0 && now_epoch >= (uint64_t)window_end_epoch) {
    LOG_INFO("[MAINT_GUARD] consume_after_run request_id=%s reason=window_expired now=%llu end=%lu",
             mw.request_id[0] ? mw.request_id : "-",
             (unsigned long long)now_epoch,
             (unsigned long)window_end_epoch);
    return true;
  }

  bool sense_terminal = !sense_maintenance_result_retryable(g_last_ota_result);
  bool lcd_success = lcd_maintenance_result_successful(g_lcd_ota_result);
  bool consume = sense_terminal && lcd_success;
  LOG_INFO("[MAINT_GUARD] consume_after_run request_id=%s sense_result=%s lcd_result=%s sense_terminal=%d lcd_success=%d consume=%d",
           mw.request_id[0] ? mw.request_id : "-",
           g_last_ota_result[0] ? g_last_ota_result : "pending",
           g_lcd_ota_result[0] ? g_lcd_ota_result : "pending",
           sense_terminal ? 1 : 0,
           lcd_success ? 1 : 0,
           consume ? 1 : 0);
  return consume;
}

static bool maintenance_wait_for_retry_slot(uint32_t window_end_epoch,
                                            unsigned long retry_interval_ms,
                                            const char* keepalive_reason,
                                            uint32_t* remaining_s_io) {
  unsigned long wait_start_ms = millis();
  unsigned long last_keepalive_ms = 0;
  uint32_t remaining_s = 0;
  while ((millis() - wait_start_ms) < retry_interval_ms) {
    pump_uart_rx_once();
    if (!maintenance_window_active_for_retry(window_end_epoch, &remaining_s)) {
      if (remaining_s_io) {
        *remaining_s_io = 0;
      }
      return false;
    }
    if (remaining_s_io) {
      *remaining_s_io = remaining_s;
    }
    g_lcd_ota_window_remaining_s = remaining_s;
    unsigned long now_ms = millis();
    if (last_keepalive_ms == 0 || (now_ms - last_keepalive_ms) >= LCD_MAINT_KEEPALIVE_MS) {
      keep_lcd_awake_for_maintenance(remaining_s, keepalive_reason);
      last_keepalive_ms = now_ms;
    }
    delay(50);
  }
  return maintenance_window_active_for_retry(window_end_epoch, remaining_s_io);
}

static void reset_lcd_maintenance_ota_state(const char* result) {
  g_lcd_ota_done = false;
  strncpy(g_lcd_ota_result, result ? result : "pending", sizeof(g_lcd_ota_result) - 1);
  g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
  g_lcd_ota_version[0] = '\0';
  g_lcd_ota_request_active = false;
  g_lcd_ota_ack_received = false;
  g_lcd_ota_request_id = 0;
  g_lcd_ota_request_start_ms = 0;
  g_lcd_ota_recent_request_id = 0;
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

// Persist the outcome+target version of a SUCCESSFUL inline LCD OTA proxy.
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

static void run_lcd_maintenance_ota_attempt(const char* maintenance_reason,
                                            uint32_t window_end_epoch,
                                            uint32_t* remaining_s_io) {
  uint32_t remaining_s = remaining_s_io ? *remaining_s_io : 0;
  g_lcd_ota_window_remaining_s = remaining_s;
  keep_lcd_awake_for_maintenance(remaining_s, "maintenance_attempt");

  bool force_lcd = OtaIntent::getDesiredForce();
  unsigned long last_act_ms = sense_get_last_user_activity_ms();
  bool explicit_http_window =
      (maintenance_reason && strcmp(maintenance_reason, "scheduled_http") == 0);
  if (!force_lcd && last_act_ms > 0) {
    unsigned long idle_ms = millis() - last_act_ms;
    if (idle_ms < (unsigned long)g_ota_sched.min_idle_min * 60000UL) {
      if (explicit_http_window) {
        maintenance_idle_diag_note(idle_ms, true, true, true, "http_window_lcd_override");
        LOG_INFO("[LCD_OTA_ORCH] bypass user_active idle_ms=%lu min_idle_min=%u reason=http_window",
                 (unsigned long)idle_ms,
                 (unsigned)g_ota_sched.min_idle_min);
      } else {
        maintenance_idle_diag_note(idle_ms, true, false, false, "lcd_user_active");
        LOG_INFO("[LCD_OTA_ORCH] skip user_active idle_ms=%lu", (unsigned long)idle_ms);
        strncpy(g_lcd_ota_result, "skipped_user_active", sizeof(g_lcd_ota_result) - 1);
        g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
        g_lcd_ota_done = true;
        return;
      }
    } else {
      maintenance_idle_diag_note(idle_ms, false, false, explicit_http_window, "lcd_idle_ok");
    }
  } else if (explicit_http_window) {
    maintenance_idle_diag_note(0, false, true, true, "http_window_no_activity");
  } else {
    maintenance_idle_diag_note(0, false, false, false, "lcd_no_activity");
  }

  // ── LCD OTA proxy: spawn task (TLS needs >8KB stack) ──
  if (!g_lcd_ota_done && !g_lcd_ota_task_running) {
    g_lcd_ota_task_running = true;
    g_manifest_client.releaseConnection();
    mqtt_stop_for_ota();
    vTaskDelay(pdMS_TO_TICKS(300));
    LOG_INFO("[LCD_OTA_ORCH] maint heap: free=%u largest=%u psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    BaseType_t rc2 = xTaskCreatePinnedToCore(
      lcd_ota_proxy_task, "lcd_ota_proxy", 12288, NULL, 3, NULL, tskNO_AFFINITY);
    if (rc2 != pdPASS) {
      LOG_ERROR("[LCD_OTA_ORCH] maint task create FAILED rc=%d", (int)rc2);
      g_lcd_ota_task_running = false;
      send_ota_uart_message("OTA_UNLOCK");
      LOG_INFO("[LCD_OTA_ORCH] OTA_UNLOCK sent (maint task create failed)");
      strncpy(g_lcd_ota_result, "task_create_fail", sizeof(g_lcd_ota_result) - 1);
      g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
      g_lcd_ota_done = true;
      mqtt_force_connect();
    } else {
      LOG_INFO("[LCD_OTA_ORCH] proxy maint task spawned");
    }
  }

  // Wait for proxy task to complete regardless of whether we just spawned it
  // or it was already running from a previous attempt. This blocks the
  // orchestrator so it cannot send MAINT_WINDOW JSON on the UART during
  // binary COBS transfer.
  //
  // CRITICAL: pump UART when the proxy is in JSON mode so that mailbox
  // flags (QUERY_RESP, BEGIN_ACK, END_ACK) get set. Without this, the
  // proxy task deadlocks — it waits for flags that only parse_input_message()
  // can set, but parse_input_message() is called from pump_uart_rx_once()
  // which only runs from this loop during maintenance.
  if (g_lcd_ota_task_running && !g_lcd_ota_done) {
    unsigned long wait_start = millis();
    unsigned long last_log_ms = 0;
    const unsigned long LCD_OTA_PROXY_TIMEOUT_MS = 600000UL; // 10 min max
    while (!g_lcd_ota_done && (millis() - wait_start) < LCD_OTA_PROXY_TIMEOUT_MS) {
      // Pump UART when proxy is NOT in binary COBS mode — safe because
      // reads and the proxy's writes don't conflict (full duplex), and
      // during binary mode the proxy reads lcdSerial directly.
      if (!g_lcd_ota_proxy_owns_uart) {
        pump_uart_rx_once();
      }
      vTaskDelay(pdMS_TO_TICKS(10));  // 10ms for responsive UART polling
      // Log progress every 30s
      unsigned long elapsed = millis() - wait_start;
      if (elapsed - last_log_ms >= 30000) {
        last_log_ms = elapsed;
        LOG_INFO("[LCD_OTA_ORCH] waiting for proxy task elapsed=%lus result=%s",
                 (unsigned long)(elapsed / 1000),
                 g_lcd_ota_result);
      }
    }
    if (!g_lcd_ota_done) {
      LOG_ERROR("[LCD_OTA_ORCH] proxy task timeout after %lus",
                (unsigned long)((millis() - wait_start) / 1000));
      strncpy(g_lcd_ota_result, "proxy_timeout", sizeof(g_lcd_ota_result) - 1);
      g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
      g_lcd_ota_done = true;
    } else {
      LOG_INFO("[LCD_OTA_ORCH] proxy task completed in %lus result=%s",
               (unsigned long)((millis() - wait_start) / 1000),
               g_lcd_ota_result);
    }
  }
}

static void run_maintenance_if_needed() {
  if (!g_maintenance_mode || g_maintenance_handled) {
    return;
  }
  LOG_INFO("[MAINT_RUN] enter mode=1 handled=0");
  g_maintenance_handled = true;
  bool retry_wake = g_maint_followup_retry_wake;
  g_maint_followup_retry_wake = false;
  g_maintenance_in_window = false;
  g_lcd_ota_done = false;
  strncpy(g_lcd_ota_result, "pending", sizeof(g_lcd_ota_result) - 1);
  g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
  g_lcd_ota_version[0] = '\0';
  g_lcd_ota_request_active = false;
  g_lcd_ota_ack_received = false;
  g_lcd_ota_request_id = 0;
  g_lcd_ota_request_start_ms = 0;
  g_lcd_ota_attempted_this_window = false;

  // lcd_ota_due_nvs is set before a Sense self-OTA reboot — it means
  // the LCD OTA is owed immediately after reboot, regardless of whether
  // a maintenance window is active. Check this FIRST, before window logic.
  if (get_lcd_ota_due_nvs()) {
    LOG_INFO("[MAINT_RUN] lcd_ota_due from NVS — bypassing window check");
    if (!ensure_maintenance_wifi_connected()) {
      LOG_INFO("[MAINT_RUN] lcd_ota_due wifi_fail -> sleep");
      set_lcd_ota_due_nvs(false);
      send_ota_uart_message("OTA_UNLOCK");
      g_maintenance_mode = false;
      sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
      return;
    }
    run_lcd_maintenance_ota_attempt("lcd_ota_due", 0, nullptr);
    set_lcd_ota_due_nvs(false);
    LOG_INFO("[MAINT_RUN] lcd_ota_due completed result=%s", g_lcd_ota_result);
    g_maintenance_mode = false;
    sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
    return;
  }

  MaintenanceWindow mw;
  bool has_mw = maintenance_window_load(&mw);
  if (!g_ota_sched.enabled && !has_mw && !retry_wake) {
    LOG_INFO("[MAINT_RUN] no_schedule enabled=0 has_mw=0 -> sleep");
    maintenance_followup_retry_clear("no_schedule");
    g_maintenance_mode = false;
    sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
    return;
  }

  if (!ensure_maintenance_wifi_connected()) {
    LOG_INFO("[MAINT_RUN] wifi_fail -> sleep");
    ota_sched_reschedule_after_failure(time(nullptr), is_time_valid());
    if (!maintenance_followup_retry_schedule(has_mw ? &mw : nullptr, "wifi_fail")) {
      maintenance_followup_retry_clear("wifi_fail");
    }
    ota_set_last_result("maintenance_wifi_fail");
    g_maintenance_mode = false;
    sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
    return;
  }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  ensure_timezone_pt("maintenance");
  if (!is_time_valid()) {
    LOG_INFO("[MAINT_RUN] time_invalid -> sleep");
    ota_sched_schedule_time_retry("maintenance_time_invalid");
    if (!maintenance_followup_retry_schedule(has_mw ? &mw : nullptr, "time_invalid")) {
      maintenance_followup_retry_clear("time_invalid");
    }
    ota_set_last_result("maintenance_time_invalid");
    g_maintenance_mode = false;
    sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
    return;
  }

  if (retry_wake && ota_sched_http_configured()) {
    uint32_t fetch_timeout_ms = OTA_SCHED_HTTP_TIMEOUT_MS;
    if (fetch_timeout_ms < 2000UL) {
      fetch_timeout_ms = 2000UL;
    }
    LOG_INFO("[MAINT_RETRY] refresh_schedule timeout_ms=%lu", (unsigned long)fetch_timeout_ms);
    ota_sched_http_fetch_window(fetch_timeout_ms);
    has_mw = maintenance_window_load(&mw);
    if (maintenance_schedule_pending_sync_to_lcd()) {
      sync_pending_maintenance_to_lcd("maint_retry_refresh");
    }
    if (!g_ota_sched.enabled && !has_mw) {
      LOG_INFO("[MAINT_RETRY] no_schedule_after_refresh -> sleep");
      if (!maintenance_followup_retry_schedule(nullptr, "retry_no_schedule")) {
        maintenance_followup_retry_clear("retry_no_schedule");
      }
      ota_set_last_result("maintenance_retry_no_schedule");
      g_maintenance_mode = false;
      sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
      return;
    }
  }

  time_t now = time(nullptr);
  uint32_t remaining_s = 0;
  uint32_t window_end_epoch = 0;
  if (has_mw) {
    uint64_t now_epoch = (uint64_t)now;
    uint64_t window_start = (mw.grace_before_sec >= mw.start_epoch) ? 0 : (mw.start_epoch - mw.grace_before_sec);
    uint64_t window_end = mw.start_epoch + mw.duration_sec + mw.grace_after_sec;
    if (maintenance_window_is_consumed(mw, now_epoch)) {
      LOG_INFO("[MAINT_RUN] consumed request_id=%s now=%llu start=%llu end=%llu -> sleep",
               mw.request_id[0] ? mw.request_id : "-",
               (unsigned long long)now_epoch,
               (unsigned long long)window_start,
               (unsigned long long)window_end);
      sched_event_note("skip_consumed", mw.request_id);
      ota_set_last_result("maintenance_consumed");
      maintenance_followup_retry_clear("consumed");
      g_maintenance_mode = false;
      sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
      return;
    }
    if (now_epoch < window_start || now_epoch > window_end) {
      if (mw.hasExpired(now_epoch)) {
        mw.clear();
        maintenance_window_consumed_clear("expired");
      }
      LOG_INFO("[MAINT_RUN] outside_window now=%llu start=%llu end=%llu -> sleep",
               (unsigned long long)now_epoch,
               (unsigned long long)window_start,
               (unsigned long long)window_end);
      if (now_epoch < window_start && !mw.hasExpired(now_epoch)) {
        // Woke BEFORE the window opens — clock was off at arm-time (NTP just
        // corrected it) or a fixed-delay retry landed early. Don't burn misaligned
        // followup retries that may also miss and ultimately abandon the window;
        // clear the retry so the pre-sleep timer re-arm (ota_sched_configure_timer_
        // wakeup) re-targets the actual window start (start-15) using the now-synced
        // clock. Converges in one cycle.
        LOG_INFO("[MAINT_RUN] woke_before_window -> clear retry, re-arm for window start");
        maintenance_followup_retry_clear("woke_before_window");
      } else if (retry_wake || g_maint_followup_retry_attempts > 0) {
        if (!maintenance_followup_retry_schedule(&mw, "outside_window")) {
          maintenance_followup_retry_clear("outside_window");
        }
      } else {
        maintenance_followup_retry_clear("outside_window");
      }
      ota_set_last_result("maintenance_outside_window");
      g_maintenance_mode = false;
      sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
      return;
    }
    g_maintenance_in_window = true;
    window_end_epoch = (uint32_t)window_end;
    remaining_s = (window_end > now_epoch) ? (uint32_t)(window_end - now_epoch) : 0;
  } else {
    ota_sched_update_next_epoch(now);
    if (!ota_sched_in_window(now)) {
      g_next_ota_epoch = ota_sched_compute_next_epoch(now);
      ota_sched_save();
      LOG_INFO("[MAINT_RUN] sched_outside_window now=%ld next_epoch=%lu -> sleep",
               (long)now,
               (unsigned long)g_next_ota_epoch);
      if (retry_wake || g_maint_followup_retry_attempts > 0) {
        if (!maintenance_followup_retry_schedule(nullptr, "sched_outside_window")) {
          maintenance_followup_retry_clear("sched_outside_window");
        }
      } else {
        maintenance_followup_retry_clear("sched_outside_window");
      }
      ota_set_last_result("maintenance_outside_window");
      g_maintenance_mode = false;
      sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
      return;
    }
    g_maintenance_in_window = true;
    window_end_epoch = g_next_ota_epoch + (uint32_t)g_ota_sched.window_dur_min * 60;
    remaining_s = (now < (time_t)window_end_epoch) ? (window_end_epoch - (uint32_t)now) : 0;
  }

  unsigned long last_activity_ms = sense_get_last_user_activity_ms();
  if (last_activity_ms > 0) {
    unsigned long idle_ms = millis() - last_activity_ms;
    if (idle_ms < (unsigned long)g_ota_sched.min_idle_min * 60000UL) {
      if (has_mw && mw.request_id[0]) {
        maintenance_idle_diag_note(idle_ms, true, true, true, "http_window_override");
        LOG_INFO("[MAINT_RUN] idle_bypassed idle_ms=%lu min_idle_min=%u request_id=%s",
                 idle_ms,
                 (unsigned)g_ota_sched.min_idle_min,
                 mw.request_id);
      } else {
        maintenance_idle_diag_note(idle_ms, true, false, false, "maintenance_idle_blocked");
        LOG_INFO("[MAINT_RUN] idle_blocked idle_ms=%lu min_idle_min=%u -> sleep",
                 idle_ms,
                 (unsigned)g_ota_sched.min_idle_min);
        ota_sched_reschedule_after_failure(now, true);
        if (!maintenance_followup_retry_schedule(has_mw ? &mw : nullptr, "idle_blocked")) {
          maintenance_followup_retry_clear("idle_blocked");
        }
        ota_set_last_result("maintenance_idle_blocked");
        g_maintenance_mode = false;
        sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
        return;
      }
    } else {
      maintenance_idle_diag_note(idle_ms, false, false, has_mw, "maintenance_idle_ok");
    }
  } else {
    maintenance_idle_diag_note(0, false, false, has_mw, "maintenance_no_activity");
  }

  // Cancel-safety: re-validate the LIVE cloud schedule before committing to OTA.
  // If the operator disabled (enabled=false) or replaced (new request_id) the
  // window after the device armed its cached copy, abort here. To pull a
  // release, set enabled=false on the schedule row — do NOT delete it: a delete
  // (204) is indistinguishable from a live past-start window and is fail-open.
  // OTA_LOCK has NOT been sent yet, so no OTA_UNLOCK is needed on this path.
  // Fail-open: a fetch failure / 204 proceeds with the cached window.
  if (has_mw) {
    SchedRevalidate rv = ota_sched_revalidate(
        mw, (uint64_t)now, max(2000UL, (unsigned long)OTA_SCHED_HTTP_TIMEOUT_MS));
    if (rv == REVAL_CANCELLED || rv == REVAL_REPLACED) {
      const char* res = (rv == REVAL_CANCELLED) ? "schedule_cancelled" : "schedule_replaced";
      char aborted_request_id[64];
      strncpy(aborted_request_id, mw.request_id, sizeof(aborted_request_id) - 1);
      aborted_request_id[sizeof(aborted_request_id) - 1] = '\0';
      LOG_INFO("[MAINT_RUN] %s at window-start -> abort OTA (request_id=%s)",
               res, aborted_request_id[0] ? aborted_request_id : "-");
      sched_event_note(res, aborted_request_id);
      ota_set_last_result(res);
      maintenance_followup_retry_clear(res);
      maintenance_window_consumed_clear(res);
      mw.clear();  // wipe cached NVS window so pre_sleep won't re-arm it
      g_maintenance_in_window = false;
      g_maintenance_mode = false;
      sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
      return;
    }
    // REVAL_VALID or REVAL_FETCH_FAILED -> proceed (fail-open on fetch failure)
  }

  const char* maintenance_reason = has_mw ? "scheduled_http" : "maintenance";

  // ── New scheduled-OTA order: LCD proxy FIRST, then Sense self-OTA ──
  // At window entry BOTH boards are freshly awake (timer wake). The Sense
  // self-OTA (inside maybeRunOtaCheck below) esp_restart()s on a successful
  // apply; if the LCD were proxied AFTER that, the rebooted Sense would have
  // to find/wake an LCD that has already idle-slept (Sense cannot wake the
  // LCD — GPIO39 is LCD→Sense only), leaving the LCD stranded on old fw.
  // Doing the LCD proxy first, while the LCD is still awake from this wake,
  // sidesteps the limitation entirely: neither board needs to wake the other
  // after a reboot.
  LOG_INFO("[MAINT_RUN] LCD proxy first (pre-sense-ota)");

  // OTA_LOCK keeps the LCD awake/listening for the UART-proxied OTA stream
  // (extends ota_stay_awake_until_ms, wakes display from idle-dark, extends
  // the maintenance deadline, blocks the LCD's own autonomous OTA check).
  send_ota_uart_message("OTA_LOCK");
  LOG_INFO("[MAINT_RUN] OTA_LOCK sent (lcd proxy first)");

  // ── Phase 1: LCD OTA proxy (independent of any Sense self-OTA) ──
  // run_lcd_maintenance_ota_attempt() spawns lcd_ota_proxy_task which does its
  // own LCD_OTA_QUERY → manifest fetch → sense_lcd_ota_proxy(); it no-ops with
  // result "noop" when the LCD is already on the target version. Success ->
  // g_lcd_ota_result "updated" (or "noop"); failures leave a retryable result.
  // The proxy task sends its own OTA_UNLOCK at completion (preserving the
  // stay-awake window). Window budget is enforced via
  // maintenance_window_active_for_retry / maintenance_wait_for_retry_slot.
  for (uint8_t attempt = 0; attempt < LCD_MAINT_OTA_MAX_ATTEMPTS; ++attempt) {
    if (!maintenance_window_active_for_retry(window_end_epoch, &remaining_s)) {
      break;
    }
    if (attempt > 0) {
      LOG_INFO("[MAINT_RUN] retry_lcd attempt=%u last_result=%s remaining_s=%lu",
               (unsigned)(attempt + 1),
               g_lcd_ota_result,
               (unsigned long)remaining_s);
      reset_lcd_maintenance_ota_state("pending");
    }
    run_lcd_maintenance_ota_attempt(maintenance_reason, window_end_epoch, &remaining_s);
    if (!lcd_maintenance_result_retryable(g_lcd_ota_result)) {
      break;
    }
    if ((attempt + 1) >= LCD_MAINT_OTA_MAX_ATTEMPTS) {
      LOG_INFO("[MAINT_RUN] retry_lcd exhausted last_result=%s", g_lcd_ota_result);
      break;
    }
    if (!maintenance_wait_for_retry_slot(window_end_epoch,
                                         MAINT_OTA_RETRY_INTERVAL_MS,
                                         "maintenance_lcd_retry",
                                         &remaining_s)) {
      break;
    }
  }
  bool lcd_proxy_succeeded = lcd_maintenance_result_successful(g_lcd_ota_result);
  LOG_INFO("[MAINT_RUN] lcd proxy phase done result=%s succeeded=%d",
           g_lcd_ota_result[0] ? g_lcd_ota_result : "pending",
           lcd_proxy_succeeded ? 1 : 0);

  // ── Phase 2: Sense self-OTA (may esp_restart on success — EXPECTED) ──
  // Re-assert OTA_LOCK so the LCD's stay-awake window covers the Sense self-OTA
  // download + reboot (the LCD proxy task above sent OTA_UNLOCK at its end;
  // OTA_LOCK only ever extends the window, never shortens it). The LCD is
  // already up-to-date now, so the inline LCD-proxy inside maybeRunOtaCheck()
  // will see compareVersions<=0 and no-op ("up_to_date") rather than wastefully
  // re-streaming — confirmed in sense_lcd_ota_proxy()'s version check.
  send_ota_uart_message("OTA_LOCK");
  LOG_INFO("[MAINT_RUN] OTA_LOCK re-asserted (sense self-ota next)");

  // Reset OTA check gate — a previous check in this boot cycle may have set
  // g_ota_check_done=true, which would cause maybeRunOtaCheck() to skip entirely.
  g_ota_check_done = false;
  g_ota_skip_logged = false;
  maybeRunOtaCheck(maintenance_reason, true);
  if (sense_maintenance_result_retryable(g_last_ota_result)) {
    for (uint8_t attempt = 1; attempt < SENSE_MAINT_OTA_MAX_ATTEMPTS; ++attempt) {
      if (!maintenance_wait_for_retry_slot(window_end_epoch,
                                           MAINT_OTA_RETRY_INTERVAL_MS,
                                           "maintenance_sense_retry",
                                           &remaining_s)) {
        break;
      }
      LOG_INFO("[MAINT_RUN] retry_sense attempt=%u last_result=%s remaining_s=%lu",
               (unsigned)(attempt + 1),
               g_last_ota_result,
               (unsigned long)remaining_s);
      g_ota_check_done = false;
      g_ota_skip_logged = false;
      maybeRunOtaCheck(maintenance_reason, true);
      if (!sense_maintenance_result_retryable(g_last_ota_result)) {
        break;
      }
      if ((attempt + 1) >= SENSE_MAINT_OTA_MAX_ATTEMPTS) {
        LOG_INFO("[MAINT_RUN] retry_sense exhausted last_result=%s", g_last_ota_result);
      }
    }
  }

  // The Sense self-OTA did not reboot (no update / blocked / failed); release
  // the LCD so it can sleep. (On a successful Sense apply, esp_restart() above
  // never returns here.)
  send_ota_uart_message("OTA_UNLOCK");
  LOG_INFO("[MAINT_RUN] OTA_UNLOCK sent (sense self-ota done, no reboot)");

  // Conditional lcd_ota_due fallback: only CLEAR the next-boot LCD retry when
  // the LCD proxy actually succeeded above (or was already up-to-date). If the
  // LCD proxy failed/was skipped, SET it so the boot-time get_lcd_ota_due_nvs()
  // branch at the top of run_maintenance_if_needed() reliably re-proxies the
  // LCD next boot/window as the fallback. (In the Sense-update case the inline
  // proxy inside maybeRunOtaCheck() already manages this flag before its reboot;
  // since the LCD is up-to-date by then it clears it — consistent with success.)
  set_lcd_ota_due_nvs(!lcd_proxy_succeeded);
  LOG_INFO("[MAINT_RUN] lcd_ota_due=%d (lcd_proxy_succeeded=%d)",
           lcd_proxy_succeeded ? 0 : 1, lcd_proxy_succeeded ? 1 : 0);
  dump_system_truth("maintenance_done");
  bool followup_retry_needed =
      sense_maintenance_result_retryable(g_last_ota_result) ||
      lcd_maintenance_result_retryable(g_lcd_ota_result);
  if (has_mw) {
    if (maintenance_window_should_consume_after_run(mw, window_end_epoch)) {
      maintenance_window_mark_consumed(mw, "maint_done");
    } else {
      LOG_INFO("[MAINT_GUARD] leaving_request_reusable request_id=%s last_sense=%s last_lcd=%s",
               mw.request_id[0] ? mw.request_id : "-",
               g_last_ota_result[0] ? g_last_ota_result : "pending",
               g_lcd_ota_result[0] ? g_lcd_ota_result : "pending");
    }
  }
  if (followup_retry_needed) {
    if (!maintenance_followup_retry_schedule(has_mw ? &mw : nullptr, "maintenance_followup")) {
      maintenance_followup_retry_clear("maintenance_followup_exhausted");
    }
  } else {
    maintenance_followup_retry_clear("maintenance_terminal");
  }
  g_next_ota_epoch = ota_sched_compute_next_epoch(time(nullptr));
  ota_sched_save();
  g_maintenance_in_window = false;
  g_maintenance_mode = false;
  sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT);
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
        ota_sched_http_fetch_window(http_timeout_ms);
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
  if (ota_allowed && OtaIntent::shouldUpdateNow()) {
    g_ota_check_done = false;
    g_ota_skip_logged = false;
    maybeRunOtaCheck("pre_sleep", true);
  } else if (!ota_allowed) {
    Serial.println("[OTA_POLICY] maintenance_only skip ota_check (not in window)");
  }
  mqtt_set_allowed(false);

  ota_sched_configure_timer_wakeup();
}

void halo_prod_pre_setup() {
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
    send_ota_uart_message("OTA_UNLOCK");
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

static void maybeRunOtaCheck(const char* reason, bool skip_boot_delay) {
  // Breadcrumb: record OTA-check entry in the persistent black box (area
  // "ota_orch") so the manual-OTA decision/handshake trail survives reboots
  // and is readable later via the LCD error log.
  {
    char crumb[96];
    snprintf(crumb, sizeof(crumb), "reason=%s manual=%d t=%lu",
             reason ? reason : "(null)",
             (int)halo_ota_manual_override_active(),
             (unsigned long)millis());
    diag_record_error_persistent("ota_orch", 0, crumb);
  }
  if (g_ota_check_done || g_ota_apply_in_progress) {
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
  if (!OtaIntent::cooldownAllows()) {
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

  if (halo_ota_manual_override_active()) {
    manual_ota_override_clear("ota_check_begin");
  }
  ota_mark_check_start();
  ota_set_last_result("check_begin");
  g_ota_check_in_progress = true;
  g_ota_check_done = true;
  // Release camera DMA reservation to free 16KB of internal SRAM for TLS.
  // Camera is not used during OTA. Device reboots after OTA, re-reserving in setup().
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
    g_ota_check_in_progress = false;
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
  if (!g_manifest_client.fetchManifest(manifest_url, manifest, 10000)) {
    truth_get_manifest_state().setErr();
    dump_system_truth("manifest_err");
    ota_set_last_result("manifest_fetch_fail");
    release_waiting_lcd_ota("manifest_fetch_fail");
    g_ota_check_in_progress = false;
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
    g_ota_check_in_progress = false;
    mqtt_set_allowed(true);
    mqtt_force_connect();
    clear_intent_once();
    return;
  }

  if (containsDisallowedHost(manifest.url) || !isAllowedOtaHost(manifest.url)) {
    LOG_ERROR("[OTA_HOST] Disallowed bin_url: %s", manifest.url);
    ota_set_last_result("bin_url_disallowed");
    release_waiting_lcd_ota("bin_url_disallowed");
    g_ota_check_in_progress = false;
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
    OtaIntent::markNoUpdateNeeded();
    release_waiting_lcd_ota("up_to_date");
    maybe_trigger_lcd_ota_check();
    g_ota_check_in_progress = false;
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
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
    OtaIntent::markNoUpdateNeeded();
    release_waiting_lcd_ota("downgrade_blocked");
    maybe_trigger_lcd_ota_check();
    g_ota_check_in_progress = false;
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
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
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
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
      if (!g_lcd_ota_task_running) {
        mqtt_set_allowed(true);
        mqtt_force_connect();
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
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
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
    if (!sense_lcd_ota_query(lcd_fw, sizeof(lcd_fw), nullptr)) {
      LOG_INFO("[OTA_ORCH] lcd proxy result=lcd_query_fail (will defer to lcd_ota_due)");
      // Breadcrumb: LCD never answered the query (the failure we're chasing).
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_query_fail t=%lu", (unsigned long)millis());
      diag_record_error_persistent("ota_orch", -1, crumb);
    } else if (!sense_lcd_ota_fetch_manifest(lcd_cfg->base_dir, lcd_cfg->channel, lcd_manifest)) {
      LOG_INFO("[OTA_ORCH] lcd proxy result=manifest_fetch_fail (will defer to lcd_ota_due)");
      // Breadcrumb: query succeeded; record the LCD fw it reported.
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_query_ok lcd_fw=%s t=%lu", lcd_fw, (unsigned long)millis());
      diag_record_error_persistent("ota_orch", 0, crumb);
    } else if (ManifestClient::compareVersions(lcd_manifest.version, lcd_fw) > 0) {
      // Breadcrumb: query succeeded; record the LCD fw it reported.
      {
        char crumb[96];
        snprintf(crumb, sizeof(crumb), "lcd_query_ok lcd_fw=%s t=%lu", lcd_fw, (unsigned long)millis());
        diag_record_error_persistent("ota_orch", 0, crumb);
      }
      send_ota_uart_message("OTA_LOCK");
      {
        // Breadcrumb: starting the LCD OTA proxy stream.
        char crumb[96];
        snprintf(crumb, sizeof(crumb), "lcd_proxy_start ver=%s t=%lu",
                 lcd_manifest.version, (unsigned long)millis());
        diag_record_error_persistent("ota_orch", 0, crumb);
      }
      const char* lcd_res = sense_lcd_ota_proxy(lcd_manifest, lcd_fw);
      LOG_INFO("[OTA_ORCH] lcd proxy result=%s", lcd_res);
      {
        // Breadcrumb: LCD OTA proxy returned.
        char crumb[96];
        snprintf(crumb, sizeof(crumb), "lcd_proxy_done res=%s t=%lu",
                 lcd_res ? lcd_res : "(null)", (unsigned long)millis());
        diag_record_error_persistent("ota_orch", 0, crumb);
      }
      if (lcd_res && strcmp(lcd_res, "success") == 0) {
        lcd_proxy_succeeded = true;
        // Record the success to the truth globals AND persist to NVS. The Sense
        // self-OTAs and reboots right after this, wiping RAM; the persisted
        // result is reloaded next boot so the cloud report shows updated/target.
        strncpy(g_lcd_ota_result, "updated", sizeof(g_lcd_ota_result) - 1);
        g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
        set_lcd_ota_result_nvs("updated", lcd_manifest.version);
        // LCD reboots into the new image; invalidate the cached version so a
        // future LCD_OTA_QUERY_RESP overwrites it with the real booted version
        // (mirrors lcd_ota_proxy_task success handling).
        g_lcd_ota_version[0] = '\0';
        g_lcd_fw_query_ms = 0;
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
      lcd_proxy_succeeded = true;  // nothing owed; don't set lcd_ota_due
    }
  }

  // Fallback: only owe an lcd_ota_due retry on the next boot if the LCD was
  // NOT brought up-to-date here (query/manifest fail, or proxy non-success).
  // On success (or already up-to-date) clear it — the LCD is already done.
  set_lcd_ota_due_nvs(!lcd_proxy_succeeded);
  LOG_INFO("[OTA] lcd_ota_due=%d (lcd_proxy_succeeded=%d)",
           lcd_proxy_succeeded ? 0 : 1, lcd_proxy_succeeded ? 1 : 0);

  // ── THEN the Sense self-OTA (reboots on success, never returns) ──
  {
    // Breadcrumb: about to apply the Sense self-OTA (this reboots on success,
    // so this is the last crumb before the LCD-owed state at next boot).
    char crumb[96];
    snprintf(crumb, sizeof(crumb), "sense_apply_start ver=%s lcd_due=%d t=%lu",
             manifest.version, (int)(!lcd_proxy_succeeded), (unsigned long)millis());
    diag_record_error_persistent("ota_orch", 0, crumb);
  }
  send_ota_uart_message("OTA_LOCK");
  g_ota_apply_in_progress = true;

  SenseOtaApplier::Result res = g_ota_applier.applyToOtaPartition(
      manifest.url, manifest.sha256, manifest.size, 1200000, true, manifest.version);
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
    return;
  }
  ota_set_last_result("apply_success");
  (void)reason;
  g_ota_check_in_progress = false;
}

void maybeRunOtaCheck(const char* reason) {
  if (reason && strcmp(reason, "post_provision") == 0 && g_provisioning_manager.isSetupModeActive()) {
    g_defer_ota_post_provision = true;
    LOG_INFO("[OTA] defer post_provision check until setup mode ends");
    return;
  }
  maybeRunOtaCheck(reason, true);
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
        if (SenseOtaPolicy::allowOtaWorkNow("cmd")) {
          g_ota_check_requested = true;
          mqtt_ota_check_requested = true;
          LOG_INFO("[OTA_INTENT] reason=cmd result=1");
        } else {
          LOG_INFO("[OTA_POLICY] skip manifest/ota (maintenance-only)");
        }
        break;
      case MQTT_CMD_OTA_FORCE_NOW: {
        if (SenseOtaPolicy::allowOtaWorkNow("force_now")) {
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
  static bool sntp_started = false;
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

  if (g_maintenance_mode && !g_maintenance_handled) {
    run_maintenance_if_needed();
    return;
  }


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
    g_ota_check_done = false;
    g_ota_skip_logged = false;
    maybeRunOtaCheck("post_provision", true);
  }

  if (wifi_connected) {
    if (!sntp_started) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
      ensure_timezone_pt("sntp_start");
      sntp_started = true;
      LOG_INFO("[TLS_GUARD] SNTP init");
    }
    if (is_time_valid()) {
      time_t now = time(nullptr);
      ota_sched_update_next_epoch(now);
    }
    g_health_gate.markWifiConnected();
  } else {
    g_health_gate.markWifiDisconnected();
  }
  if (g_post_provision_list_refresh_pending && wifi_connected) {
    g_post_provision_list_refresh_pending = false;
    LOG_INFO("[PROVISION] Connected - requesting initial list refresh");
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
  maybe_cancel_manual_ota_unready();
  handle_ota_proof();
  handle_mqtt_commands();
  // Arm-time delivery race fix: hold the LCD awake through the post-wake
  // connect/fetch phase (before pending-sync is even set) so it doesn't
  // idle-sleep before the real MAINT_WINDOW can be delivered. Must run BEFORE
  // sync_pending_maintenance_to_lcd() so a keepalive precedes any window send.
  keep_lcd_awake_during_maint_arm();
  sync_pending_maintenance_to_lcd("awake");

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
  // Override mbedTLS allocator: allow PSRAM for TLS buffers.
  // The prebuilt libs use CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC which restricts
  // mbedTLS to internal SRAM only (~32KB available). Standard calloc/free
  // with CONFIG_SPIRAM_USE_MALLOC routes large allocations (>4KB) to PSRAM.
  mbedtls_platform_set_calloc_free(calloc, free);
  Serial.printf("[TLS_PSRAM] mbedTLS allocator overridden to use default heap (PSRAM-capable)\n");

  g_boot_time_ms = millis();
  ensure_timezone_pt("boot");
  halo_wifi_guard_boot_log();

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
    LOG_INFO("[MAINT] lcd_ota_due from NVS (sense rebooted during maintenance)");
    g_maintenance_mode = true;
  }

  // Repopulate g_lcd_ota_result / g_lcd_ota_version from a SUCCESSFUL inline
  // LCD OTA proxy that happened just before the Sense self-OTA reboot. One-shot
  // (the keys are consumed) so the post-reboot pre_sleep cloud report shows
  // last_lcd_ota_result=updated + last_lcd_fw=<target>. The live pre_sleep LCD
  // query later confirms/corrects this with the real booted version.
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


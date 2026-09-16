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
#ifndef HALO_DURABLE_OTA_POLICY
#define HALO_DURABLE_OTA_POLICY 0
#endif
#ifndef HALO_DIAGNOSTIC_ADMISSION
#define HALO_DIAGNOSTIC_ADMISSION 0
#endif
#ifndef HALO_IDLE_NETWORK_PROBE
#define HALO_IDLE_NETWORK_PROBE 0
#endif
#if HALO_IDLE_NETWORK_PROBE
static bool halo_idle_probe_usb_line(const char*,size_t);
#endif
#ifndef HALO_OTA_ONE_SHOT
#define HALO_OTA_ONE_SHOT 0
#endif
#if HALO_DURABLE_OTA_POLICY
static bool halo_policy_network_admitted();
static bool halo_policy_allocated_held();
static bool halo_policy_lcd_begin();
static const char* halo_policy_lcd_manifest_version();
static bool halo_policy_arm_waiting();
static uint32_t halo_policy_timer_delta();
static bool halo_policy_bench_deferred_arm(uint32_t&,uint32_t&,char*,size_t);
static bool halo_policy_notice_due(const char*);
static bool halo_policy_boot_ready();
static bool halo_policy_short_due();
static void halo_policy_service_boot();
static uint32_t halo_policy_ntp_budget(uint32_t);
static int32_t halo_policy_arm_timer(uint32_t&,uint32_t);
static bool halo_policy_one_shot_ack(uint32_t,uint32_t,bool,const char*,const char*,bool,uint64_t,uint32_t,uint32_t,uint32_t,const char*,uint32_t);
#endif
#include "../shared/OtaHeapTrace.h"
#include "../shared/ProvisioningDisplayStatus.h"
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
static void sense_diag_note_timer_sdk(uint64_t timer_us,int32_t sdk_result);
static void sense_diag_presleep();
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_OTA_POLICY
static void halo_sleep_witness_enter();
static bool halo_diag_auth_usb_line(const char*,size_t);
#endif
#endif

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
struct LcdVerifiedArmAck {
  bool waiting = false, matched = false;
  uint32_t started_ms = 0, budget_ms = 0, peer_boot_id = 0;
  uint32_t start_epoch = 0, remaining_s = 0;
  char challenge[40] = {0}, request_id[64] = {0};
};
static LcdVerifiedArmAck g_lcd_verified_arm;
static LcdVerifiedArmAck g_self_retry_arm;
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
#include "../shared/CoordinatorCreditState.h"
#include <nvs.h>

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
#include "../shared/SelfOtaRetry.h"
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
RTC_DATA_ATTR static SelfOtaRetry g_self_retry = {};
RTC_DATA_ATTR static uint32_t g_last_selected_sleep_s = 0, g_last_selected_sleep_epoch = 0;
static bool g_self_retry_boot = false, g_self_retry_execution = false;
static uint32_t self_retry_now() {
  const time_t now=time(nullptr);
  return now>=1700000000 && uint64_t(now)<=UINT32_MAX ? (uint32_t)now : 0;
}
static bool self_retry_bound() {
  return self_retry_identity(g_self_retry,g_coord_pending,g_coord_completion_target);
}
static uint32_t self_retry_selected_delta() {
#if HALO_DURABLE_OTA_POLICY
  return halo_policy_timer_delta();
#else
  // If user/retained work kept this boot awake through the opportunity, close
  // it explicitly. A zero delta must not silently renew it or masquerade as an
  // armed retry. No work is forced through the person's activity/ownership.
  if (self_retry_bound() && g_self_retry.phase==SelfOtaRetryPhase::ARMED &&
      !g_self_retry_boot && sense_time_has_fresh_sync() && self_retry_now()>=g_self_retry.due_epoch) {
    g_self_retry.phase=SelfOtaRetryPhase::CLOSED;
    LOG_INFO("[SELF_RETRY] closed reason=due_passed_while_awake origin=%s target=%s",
             g_self_retry.origin,g_self_retry.version);
  }
  return self_retry_delta(g_self_retry,g_coord_pending,g_coord_completion_target,
                          self_retry_now(),sense_time_has_fresh_sync());
#endif
}

static bool self_retry_notice_due(const char* id) {
#if HALO_DURABLE_OTA_POLICY
  return halo_policy_notice_due(id);
#else
  const uint32_t now=self_retry_now();
  return id && self_retry_bound() && g_self_retry.phase==SelfOtaRetryPhase::ARMED &&
      !strcmp(id,g_self_retry.arm_id) && self_retry_time(g_self_retry,now,sense_time_has_fresh_sync()) &&
      uint64_t(now)+SELF_OTA_RETRY_LCD_LEAD_S>=g_self_retry.due_epoch;
#endif
}

static void self_retry_note_selected_timer(uint32_t seconds) {
  g_last_selected_sleep_s=seconds;g_last_selected_sleep_epoch=self_retry_now();
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  sense_diag_presleep();
#endif
}
static CoordinatorCreditState g_coord_credit;
static bool g_coord_credit_loaded = false, g_coord_credit_load_attempted = false;
// Any uncertain write closes this for the rest of this boot. NVS recovery on
// the next normal boot, followed by validated load, is the only reopen path.
static bool g_coord_credit_mutations = false, g_coord_credit_uncertain = false;
static bool g_tz_initialized = false;
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
enum class CoordCompletion { Deferred, NoPending, Credited, ResolvedUncredited };
static CoordCompletion ota_peer_schedule_complete();
static bool coord_credit_store_arm(const char* id, uint32_t target);
static bool coord_credit_prepare_work(const char* reason);
static uint32_t lcd_verified_arm_apply_budget();

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
static ProvisioningDisplayStatus g_provision_display_status;
static char g_last_provision_display_status[24] = "";
static uint32_t g_provision_display_poll_ms = 0;
static uint32_t g_provision_display_send_ms = 0;
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
static bool ota_clock_ready_before_work();
static uint32_t ota_scheduled_clock_retry_deadline();
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
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
static void sense_diag_report_discovery(JsonObject& payload);
#endif
#if HALO_DURABLE_OTA_POLICY
static void halo_policy_append_report(JsonDocument&);
#endif
#include "../shared/SenseIdleNetworkGuard.h"
#include "../shared/SenseBoundedPost.h"

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
  // IDF returns bytes, not FreeRTOS words. This task-lifetime minimum includes
  // earlier diagnostic/admission work, but not this payload's future POST.
  payload["loop_stack_min_free_bytes"] = uint32_t(uxTaskGetStackHighWaterMark(nullptr));
  payload["loop_stack_sample_ms"] = millis();
  payload["last_sleep_timer_selected_s"]=g_last_selected_sleep_s;
  payload["last_sleep_timer_selected_epoch"]=g_last_selected_sleep_epoch;
#if HALO_DURABLE_OTA_POLICY
  halo_policy_append_report(doc);
#else
  if (self_retry_shape(g_self_retry)) {
    payload["self_retry_phase"]=(unsigned)g_self_retry.phase;
    payload["self_retry_due_epoch"]=g_self_retry.due_epoch;
    payload["self_retry_expires_epoch"]=g_self_retry.expires_epoch;
    payload["self_retry_origin"]=g_self_retry.origin;
    payload["self_retry_target"]=g_self_retry.version;
  }
#endif
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
  // Same-loop applier/report ownership; detail is volatile and never a boot gate.
  // Seven bounded scalar fields; preserve the existing high-level result string.
  const auto failure = g_ota_applier.getFailureSnapshot();
  if (failure.stage != SenseOtaApplier::FailureStage::NONE &&
      strcmp(g_last_ota_result, "FAILED_WRITE") == 0) {
    payload["ota_fail_stage"] = SenseOtaApplier::getFailureStageString(failure.stage);
    payload["ota_fail_sdk"] = failure.sdk_error;
    payload["ota_fail_offset"] = failure.offset;
    payload["ota_fail_expected"] = failure.expected_bytes;
    payload["ota_fail_heap"] = failure.free_heap;
    payload["ota_fail_largest"] = failure.largest_internal;
    payload["ota_fail_minimum"] = failure.minimum_internal;
  }
  ota_heap::report(payload);
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

#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  if(!report_type||!strcmp(report_type,"pre_sleep"))sense_diag_report_discovery(payload);
#endif
  // Caller-supplied extras (e.g. sense_fw_target on ota_complete). Added last
  // so they can override/augment without touching the common builder body.
  if (extras) {
    for (uint8_t i = 0; i < extras->count; ++i) {
      ota_report_add_optional_str(payload, extras->keys[i], extras->vals[i]);
    }
  }

  out = "";
  // ArduinoJson 7 does not enforce the legacy constructor capacity. Refuse an
  // allocation-truncated document or output instead of reporting partial detail.
  if (doc.overflowed()) {
    LOG_WARN("[OTA_REPORT] document allocation failed");
    return false;
  }
  sense_idle_network::append(doc);
  const size_t expected_json_bytes = measureJson(doc);
  const size_t written_json_bytes = serializeJson(doc, out);
  if (written_json_bytes == 0 || written_json_bytes != expected_json_bytes ||
      out.length() != written_json_bytes) {
    out = "";
    LOG_WARN("[OTA_REPORT] serialize failed");
    return false;
  }
  // Optional cumulative shopping metrics. Reuse the verified boot generation
  // and existing report; lack of room/identity is absence, never fabricated zero.
  sense_action_summary::append(out, g_coord_sense_boot_id, sense_post::PAYLOAD_MAX);
  return true;
}

// Generic device-state report POST. Builds the SAME rich payload as the
// historical pre_sleep report but with an arbitrary report_type and an optional
// small set of extra string fields. Uses one cooperative POST deadline and
// bounded cleanup; the existing watchdog remains the fallback for SDK stalls.
// report_type == nullptr => "pre_sleep".
static bool ota_report_post(const char* report_type,
                            const OtaReportExtras* extras,
                            uint32_t timeout_ms) {
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

  const bool idle_report=HALO_IDLE_NETWORK_RECOVERY&&!strcmp(rt,"pre_sleep");
  // Metadata/NVS reads above finish before the subscription. The guard outlives
  // HTTP/TLS/DNS and DMA cleanup below; it never spans policy work or storage.
  const uint32_t guard_deadline=idle_report?sense_idle_network::window_deadline:0;
  const uint32_t post_started=millis();
  // Non-idle window_armed reports are outside this isolated guard scope.
  struct ReportGuard {
    alignas(sense_idle_network::Guard) uint8_t memory[sizeof(sense_idle_network::Guard)];
    sense_idle_network::Guard*guard=nullptr;
    ReportGuard(bool enabled,uint32_t d){if(enabled)guard=new(memory)sense_idle_network::Guard(halo_idle_net::Op::Ordinary,d);}
    ~ReportGuard(){if(guard)guard->~Guard();}
    bool active()const{return !guard||guard->active();}
  } recovery(idle_report,guard_deadline);
  if(!recovery.active()){LOG_INFO("[OTA_REPORT] skip pre_sleep (network_guard=%u)",sense_idle_network::last_decision);return false;}
  HaloNtpDnsGuard ntp_dns_guard;
  // Free the camera DMA reserve for the duration of the HTTPS handshake/POST and
  // auto-restore on every return path. This report can fire during a maintenance
  // window with the 16KB camera reserve held, which otherwise starves the TLS
  // esp-aes DMA alloc and panics. Only armed on the HTTPS branch.
  const bool report_is_https = ota_report_http_is_https(OTA_REPORT_HTTP_URL);
  ScopedTlsDmaReserve tls_dma_guard(report_is_https);

  const uint32_t budget=timeout_ms<sense_post::REQUEST_MS?timeout_ms:sense_post::REQUEST_MS;
  const uint32_t owner_end=idle_report?guard_deadline:post_started+budget+sense_post::CLEANUP_MS;
  const sense_post::Request request{OTA_REPORT_HTTP_URL,device_id,owner_ok?owner_id:nullptr,
      has_mw?mw.request_id:nullptr,reinterpret_cast<const uint8_t*>(body.c_str()),body.length(),false};
  const sense_post::Lease lease{const_cast<bool*>(&idle_report),[](void*arg){
      return wifi_is_connected()&&(!*static_cast<bool*>(arg)||halo_idle_network_safe());
    },owner_end,timeout_ms,post_started};
  sense_post::Response response;
  const sense_post::Result result=sense_post::post_once(request,lease,response);
  // Bounded existing serial logging after TLS cleanup; this sample includes
  // the just-returned POST. It creates no new report, wake or transport retry.
  LOG_INFO("[OTA_STACK] after_post_min_free_bytes=%lu",static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
  const bool ok=result==sense_post::Result::Received&&(response.status==HTTP_CODE_OK||response.status==HTTP_CODE_ACCEPTED);
  if(ok){LOG_INFO("[OTA_REPORT] %s ok code=%d body_len=%u",rt,response.status,static_cast<unsigned>(body.length()));return true;}
  LOG_WARN("[OTA_REPORT] %s failed code=%d stage=%u result=%u sent=%u cleanup=%u",rt,response.status,
      unsigned(response.stage),unsigned(result),unsigned(response.bytes_sent),unsigned(response.cleanup_ok));
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
                                uint32_t grace_after_sec,
                                const char* coord_id, uint32_t peer_boot_id) {
  if (coord_id && coord_id[0]) {
#if HALO_DURABLE_OTA_POLICY && HALO_OTA_ONE_SHOT
    if(halo_policy_one_shot_ack(remaining_s,wake_in_s,clear,request_id,status,persisted,start_epoch,
       duration_sec,grace_before_sec,grace_after_sec,coord_id,peer_boot_id))return;
#endif
    // A future sleep arm never completes or replaces the current OTA origin.
    if (g_lcd_verified_arm.waiting &&
        (uint32_t)(millis() - g_lcd_verified_arm.started_ms) < g_lcd_verified_arm.budget_ms &&
        g_lcd_work_budget_live && g_lcd_work_budget.remaining_ms() &&
        strcmp(coord_id, g_lcd_verified_arm.challenge) == 0 &&
        peer_boot_id == g_lcd_verified_arm.peer_boot_id &&
        strcmp(request_id, g_lcd_verified_arm.request_id) == 0 &&
        !clear && persisted && strcmp(status, "stored_verified") == 0 &&
        start_epoch == g_lcd_verified_arm.start_epoch &&
        remaining_s == g_lcd_verified_arm.remaining_s && wake_in_s == remaining_s &&
        !duration_sec && !grace_before_sec && !grace_after_sec) {
      g_lcd_verified_arm.matched = true;
    }
    // Separate bounded arm proof: never renew the completed apply/work budget.
    if (g_self_retry_arm.waiting &&
#if HALO_DURABLE_OTA_POLICY
        halo_policy_arm_waiting() &&
#else
        self_retry_bound() && g_self_retry.phase==SelfOtaRetryPhase::RESERVED &&
#endif
        (uint32_t)(millis()-g_self_retry_arm.started_ms)<g_self_retry_arm.budget_ms &&
        strcmp(coord_id,g_self_retry_arm.challenge)==0 &&
        peer_boot_id==g_self_retry_arm.peer_boot_id && request_id && status &&
        strcmp(request_id,g_self_retry_arm.request_id)==0 && !clear && persisted &&
        strcmp(status,"stored_verified")==0 && start_epoch==g_self_retry_arm.start_epoch &&
        remaining_s==g_self_retry_arm.remaining_s && wake_in_s==remaining_s &&
        !duration_sec && !grace_before_sec && !grace_after_sec) {
      g_self_retry_arm.matched=true;
    }
    return;
  }
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
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = type;
  // Legacy/intermediate unlocks retain their continuation hold. Only a proven
  // final outcome releases it; older LCD firmware safely ignores this field.
  if (terminal && strcmp(type, "OTA_UNLOCK") == 0) {
    doc["terminal"] = true;
    doc["result"] = g_last_ota_result;
  }
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
  // Do not consume debt/requests or begin a new OTA before local VALID.
  if (!nvs_capacity_image_valid()) return false;
  if (g_ota_check_in_progress || g_ota_apply_in_progress || g_lcd_ota_task_running ||
      g_lcd_ota_proxy_owns_uart || g_peer_gate.active || g_boot_ota_pending) return false;
  g_peer_episode_finished = false;
  g_ota_check_done = false;
  g_self_retry_boot = false; g_self_retry_execution = false; // explicit new request keeps its own policy
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

#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
static void halo_sleep_witness_sent(const MaintenanceWindow*,uint32_t,uint32_t,bool,uint32_t,bool);
static void halo_sleep_witness_boot();
#endif
static void send_maint_window(const MaintenanceWindow* mw,
                              uint32_t remaining_s,
                              uint32_t wake_in_s,
                              bool clear_schedule,
                              const char* coord_id = nullptr, uint32_t peer_boot_id = 0) {
  StaticJsonDocument<512> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "MAINT_WINDOW";
  if (coord_id && coord_id[0]) { doc["coord_id"] = coord_id; doc["peer_boot_id"] = peer_boot_id; }
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
  // Optional observational identity. Reuse the checked boot counter carried by
  // OTA_LOCK; an old LCD may ignore it. Never grow an existing frame beyond the
  // peer limit or prevent an otherwise unchanged schedule from being sent.
  if (g_coord_sense_boot_id) {
    doc["sense_boot_id"] = g_coord_sense_boot_id;
    if (doc.overflowed() || measureJson(doc) > UART_RX_FRAME_MAX)
      doc.remove("sense_boot_id");
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
  halo_sleep_witness_sent(mw,remaining_s,wake_in_s,clear_schedule,peer_boot_id,
                         doc["sense_boot_id"].is<uint32_t>()&&doc["sense_boot_id"].as<uint32_t>()==g_coord_sense_boot_id);
#endif
  LOG_INFO("[MAINT_TX] to_lcd remaining_s=%lu wake_in_s=%lu clear=%d request_id=%s start_epoch=%llu now_epoch=%llu",
           (unsigned long)remaining_s,
           (unsigned long)wake_in_s,
           clear_schedule ? 1 : 0,
           (mw && mw->request_id[0]) ? mw->request_id : "-",
           (unsigned long long)((mw && mw->start_epoch > 0) ? mw->start_epoch : 0ULL),
           (unsigned long long)(is_time_valid() ? (uint64_t)time(nullptr) : 0ULL));
}

// The ordinary calendar sleep uses the same absolute representation as the
// pre-apply fallback. Private shortened test sleeps retain their relative ID.
static void prod_co_schedule_lcd_maint_wake(uint32_t wake_in_s) {
  if (!wake_in_s) return;
  // This exact future arm was readback-ACKed before OTA_UNLOCK. Do not replace
  // it with an unverified relative/calendar message on an intervening user wake
  // or an earlier drain/pin timer. Original pending credit remains separate.
  uint32_t bench_due=0,bench_delta=0;char bench_id[64]{};
#if HALO_DURABLE_OTA_POLICY
  bool bench_arm=halo_policy_bench_deferred_arm(bench_due,bench_delta,bench_id,sizeof(bench_id));
#else
  bool bench_arm=false;
#endif
  if (self_retry_selected_delta()&&!bench_arm) {
    LOG_INFO("[SELF_RETRY] retain_verified_arm id=%s target=%lu selected=%lu",
             g_self_retry.arm_id,(unsigned long)g_self_retry.due_epoch,(unsigned long)wake_in_s);
    return;
  }
  // Do not replace a previously verified absolute arm with an untrusted
  // relative fallback after reboot. Without an arm LCD already has periodic
  // six-hour fallback; no exact fallback co-scheduling claim is made.
  if (!sense_time_has_fresh_sync()) {
    LOG_WARN("[MAINT_COSCHED] no fresh clock; existing LCD arm and schedule retained");
    return;
  }
#if HALO_DURABLE_OTA_POLICY
  // selected_delta may just have closed a missed short opportunity. Never
  // transmit its earlier delta as a new peer arm; keep the normal fallback.
  {
    std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
    bench_arm=halo_policy_bench_deferred_arm(bench_due,bench_delta,bench_id,sizeof(bench_id));
    wake_in_s=bench_arm?bench_delta:halo_seconds_until_maintenance(time(nullptr));
  }
  if(!wake_in_s)return;
#endif
  MaintenanceWindow identity = {};
  // Timer-only arm: the maintenance constructor defaults are not zero.
  identity.duration_sec = 0;
  identity.grace_before_sec = 0;
  identity.grace_after_sec = 0;
  const time_t now = time(nullptr);
  const time_t target = now + wake_in_s;
  struct tm local_target = {};
  const bool local_ok = localtime_r(&target, &local_target) != nullptr;
  bool calendar = sense_time_has_fresh_sync() && local_ok &&
      local_target.tm_hour == 2 && local_target.tm_min == 0;
#if defined(HALO_MAINT_TEST_S) && HALO_MAINT_TEST_S > 0
  calendar = false;
  // Explicit compiled test arm still has a fresh absolute target. Its LCD
  // timer includes the ordinary 15-second lead; the request ID retains delta.
  if (now < 1700000000 || uint64_t(now) + wake_in_s > UINT32_MAX) return;
  identity.start_epoch = (uint64_t)target;
#endif
  if(bench_arm){
    identity.start_epoch=bench_due;strlcpy(identity.request_id,bench_id,sizeof(identity.request_id));
  } else if (calendar) {
    const uint32_t delta = halo_seconds_until_maintenance(now);
    if (!delta || uint64_t(now) + delta > UINT32_MAX) return;
    identity.start_epoch = uint64_t(now) + delta;
    time_t absolute = (time_t)identity.start_epoch;
    if (!localtime_r(&absolute, &local_target)) return;
    snprintf(identity.request_id, sizeof(identity.request_id), "nightly_%04d%02d%02d",
             local_target.tm_year + 1900, local_target.tm_mon + 1, local_target.tm_mday);
  } else {
    snprintf(identity.request_id, sizeof(identity.request_id), "relative_%lu_%lu",
             (unsigned long)target, (unsigned long)wake_in_s);
  }
  const bool committed = coord_credit_store_arm(identity.request_id, (uint32_t)identity.start_epoch);
  if (!committed) {
    LOG_WARN("[MAINT_COSCHED] schedule identity storage failed; prior arm retained");
    return;
  }
  strlcpy(g_coord_schedule, identity.request_id, sizeof(g_coord_schedule));
  send_maint_window(&identity, wake_in_s, wake_in_s, false);
  LOG_INFO("[MAINT_COSCHED] sent wake=%lus target=%llu id=%s (delivery not verified)",
           (unsigned long)wake_in_s, (unsigned long long)identity.start_epoch, identity.request_id);
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
#if HALO_DURABLE_OTA_POLICY
  // Route legacy proof/background triggers through the same persisted paired
  // admission. No second worker can fetch or erase outside those budgets.
  g_ota_check_requested=true;
  return;
#endif
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
  // Proof reporting remains observational. Queue the existing paired owner;
  // do not reset terminal flags, owner state, readiness or work deadlines.
  g_lcd_ota_attempted_this_window=true;
  g_ota_check_requested=true;
  LOG_INFO("[LCD_OTA_ORCH] queued existing paired owner (proof/fallback)");
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

static void service_provision_display(ProvisioningState::State state, bool changed) {
  const bool setup = g_provisioning_manager.isSetupModeActive();
  if (!setup && !g_provision_display_status.tracking() && !changed) return;
  const uint32_t now = millis();
  if (!changed && g_provision_display_poll_ms &&
      (uint32_t)(now - g_provision_display_poll_ms) < 500) return;
  g_provision_display_poll_ms = now;
  char owner[64] = {0};
  const bool needs_owner = state == ProvisioningState::STATE_CONNECTED &&
                           (setup || g_provision_display_status.tracking());
  const bool owner_set = needs_owner &&
      ProvisioningState::loadOwnerId(owner, sizeof(owner)) && owner[0];
  const bool claim_failed = !s_post_ap_claim_retry_pending &&
      (g_provisioning_manager.ownerClaimExhausted() || (!setup && s_post_ap_shutdown_ms));
  const char* display = g_provision_display_status.observe(
      ProvisioningState::getStateString(state), setup,
      g_provisioning_manager.isAppSessionActive(now), owner_set,
      claim_failed);
  // Repeat while setup is in progress so a missed UART frame cannot strand
  // the guide on an earlier page. LCD treats these messages idempotently.
  if (strcmp(display, g_last_provision_display_status) != 0 ||
      (uint32_t)(now - g_provision_display_send_ms) >= 3000) {
    send_provision_status(display);
    strlcpy(g_last_provision_display_status, display, sizeof(g_last_provision_display_status));
    g_provision_display_send_ms = now;
    LOG_INFO("[PROVISION_UI] state=%s", display);
  }
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

// Refresh from typed persisted configuration, including a same-boot backend
// update. Only definite absence selects the default. Unknown storage leaves
// the current display/runtime zone but cannot make calendar readiness true.
static void ensure_timezone_pt(const char* reason) {
  std::lock_guard<std::recursive_mutex> config_lock(ProvisioningState::timezoneMutex());
  char saved_tz[64];
  const auto status=ProvisioningState::loadTimezoneStatus(saved_tz,sizeof(saved_tz));
  std::lock_guard<std::recursive_mutex> time_lock(g_time_mutex);
  if(status==ProvisioningState::TimezoneStatus::Unknown){g_tz_initialized=false;return;}
  const char* desired=status==ProvisioningState::TimezoneStatus::Present?saved_tz:HALO_DEFAULT_TZ;
  if(!g_tz_initialized || strcmp(g_tz_current,desired) || !nightly_credit_timezone_matches(desired)){
    sense_set_timezone(desired);
    LOG_INFO("[TZ] confirmed source=%s reason=%s",status==ProvisioningState::TimezoneStatus::Present?"nvs":"absent-default",reason?reason:"unknown");
  }
  g_tz_initialized=true;
}

// Start SNTP/NTP the instant WiFi (STA) connects, so the owner-claim TLS and OTA
// schedule calls have valid time on the first attempt rather than failing http=-1
// while time is still invalid right after connect. The kickoff is idempotent
// and non-blocking. Suspended network work resumes its fixed deadline.
// A live manual action may grant one separate bounded retry with static DNS slots after the
// ordinary attempt; its original hostname/DNS generation never re-arms.
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

static bool set_lcd_ota_due_nvs(bool value);
static bool get_lcd_ota_due_nvs();

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
  // The readiness opportunity has ended even when user work prevented its
  // retry-admission hook from running. Never retain that per-boot gate forever.
  g_self_retry_boot = false;
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
  if (release) { doc["terminal"] = true; doc["result"] = g_last_ota_result; }
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


// BEGIN COORDINATOR_CREDIT_INTEGRATION
static bool coord_credit_budget_open() {
  return !(g_lcd_work_budget_live && !g_lcd_work_budget.remaining_ms()) &&
      !(g_peer_gate.active && !g_peer_gate.entered &&
        (int32_t)(millis() - g_peer_gate.deadline_ms) >= 0) &&
      (!g_lcd_verified_arm.waiting || lcd_verified_arm_apply_budget() > 0);
}
static void coord_credit_clock(uint64_t& now, bool& fresh, char (&tz)[64], bool& finished) {
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  now = (uint64_t)time(nullptr);
  fresh = sense_time_has_fresh_sync() && g_tz_initialized;
  strlcpy(tz, g_tz_initialized ? g_tz_current : "", sizeof(tz));
  finished = (g_ntp_attempt_finished && !sense_time_has_fresh_sync()) ||
      (g_boot_ota_deadline_ms != 0 && (int32_t)(millis() - g_boot_ota_deadline_ms) >= 0);
}
static NightlyCreditOrigin g_calendar_timer_origin;
static bool g_coord_credit_persisted=false;
static bool coord_credit_same_persisted(const CoordinatorCreditState& candidate) {
  if(!g_coord_credit_persisted || g_coord_credit_uncertain || !g_coord_credit_mutations)return false;
  uint8_t before[COORDINATOR_CREDIT_BYTES],after[COORDINATOR_CREDIT_BYTES];
  return credit_encode(g_coord_credit,before) && credit_encode(candidate,after) && !memcmp(before,after,sizeof(before));
}
// Standard Arduino upload installs this exact two-bank UNDEFINED template.
// Blank otadata is a distinct bootloader path that normally becomes VALID.
static bool g_serial_install_checked=false,g_serial_install_uncertain=false;
static bool coord_credit_known_serial_template(const esp_partition_t* running) {
  if(!running || running->type!=ESP_PARTITION_TYPE_APP ||
      running->subtype!=ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
      running->address!=0x10000 || running->size!=0x1e0000)return false;
  const esp_partition_t* metadata=nvs_capacity_known_layout();
  if(!metadata)return false;
  uint8_t bytes[256];static const uint8_t first_crc[4]={0x9a,0x98,0x43,0x47};
  for(size_t offset=0;offset<8192;offset+=sizeof(bytes)){
    if(esp_partition_read(metadata,offset,bytes,sizeof(bytes))!=ESP_OK)return false;
    for(size_t i=0;i<sizeof(bytes);++i){
      const size_t at=offset+i;uint8_t expected=0xff;
      if(at<4)expected=at==0?1:0;
      else if(at>=28 && at<32)expected=first_crc[at-28];
      else if(at>=4096 && at<4100)expected=0;
      if(bytes[i]!=expected)return false;
    }
  }
  return true;
}
static void coord_credit_service_serial_install() {
  if(g_serial_install_checked || g_health_gate.getSkipMarkValid() ||
      !g_health_gate.hasRuntimeReadiness() || g_ota_apply_in_progress)return;
  // Existing logical repair/check budgets may need VALID to persist resolution.
  // Do not make that obligation a prerequisite for validating this same image.
#ifdef CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
  return; // this narrow serial-install contract does not change secure-version fuses
#else
  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* selected=esp_ota_get_boot_partition();
  esp_ota_img_states_t state;
  if(!running || !selected || running->address!=selected->address ||
      esp_ota_get_state_partition(running,&state)!=ESP_OK || state!=ESP_OTA_IMG_UNDEFINED)return;
  g_serial_install_checked=true; // one finite validation/mutation attempt per boot
  if(!coord_credit_known_serial_template(running)){
    Serial.println("[OTA_NVS_BOOT] serial_template=0 metadata_unchanged=1");return;
  }
  selected=esp_ota_get_boot_partition();
  if(!selected || selected->address!=running->address || g_ota_apply_in_progress ||
      g_health_gate.getSkipMarkValid() || !g_health_gate.hasRuntimeReadiness() ||
      esp_ota_get_state_partition(running,&state)!=ESP_OK || state!=ESP_OTA_IMG_UNDEFINED)return;
  // The SDK changes the selected active record directly to VALID. No NEW,
  // forced reboot or additional first-provisioning rollback exposure is added.
  g_serial_install_uncertain=true;
  const esp_err_t result=esp_ota_mark_app_valid_cancel_rollback();
  selected=esp_ota_get_boot_partition();
  if(result==ESP_OK && selected && selected->address==running->address &&
      esp_ota_get_state_partition(running,&state)==ESP_OK && state==ESP_OTA_IMG_VALID &&
      nvs_capacity_image_valid())
    g_serial_install_uncertain=false;
  Serial.printf("[OTA_NVS_BOOT] serial_template=1 verified_valid=%u sdk_result=%d\n",
      g_serial_install_uncertain?0:1,(int)result);
#endif
}

// No new-format NVS while an automatic rollback to an older reader is possible.
// A first upgraded PENDING_VERIFY boot may read v1, but cannot persist v2.
static bool coord_credit_format_write_ready() {
  if(g_serial_install_uncertain)return false;
  return nvs_capacity_image_valid();
}
// Boot-only failure/uncertainty latch. Optional report-cache semantics remain
// separate; no read failure can manufacture a cleared LCD obligation.
static bool g_ota_storage_uncertain=false,g_lcd_due_ram_obligation=false;
static bool ota_storage_read_debt(bool& due) {
  due=true;
  if(g_ota_storage_uncertain)return false;
  nvs_handle_t h;const esp_err_t opened=nvs_open("halo",NVS_READONLY,&h);
  if(opened==ESP_ERR_NVS_NOT_FOUND){due=false;return true;}
  if(opened!=ESP_OK){g_ota_storage_uncertain=true;return false;}
  uint32_t value=0;const esp_err_t result=nvs_get_u32(h,"lcd_ota_due",&value);nvs_close(h);
  if(result==ESP_ERR_NVS_NOT_FOUND){due=false;return true;}
  if(result!=ESP_OK || value>1){g_ota_storage_uncertain=true;return false;}
  due=value==1;return true;
}
static bool get_lcd_ota_due_nvs() {
  bool due=true;return !ota_storage_read_debt(due) || due || g_lcd_due_ram_obligation;
}
static bool ota_storage_known_clear() {
  return !g_ota_storage_uncertain && !get_lcd_ota_due_nvs();
}
static bool ota_storage_mutation_open() {
  return !g_ota_storage_uncertain && !g_coord_credit_uncertain && !g_nvs_reclaim_uncertain &&
      coord_credit_format_write_ready() && coord_credit_budget_open();
}
static bool set_lcd_ota_due_nvs(bool value) {
  if(value)g_lcd_due_ram_obligation=true;
  if(!ota_storage_mutation_open())return false;
  bool old=true;if(!ota_storage_read_debt(old))return false;
  if(old==value){g_lcd_due_ram_obligation=value;return true;}
  NvsCapacityLease lease;
  if(!lease || !wakelog_nvs_prepare_essential(2,coord_credit_budget_open))return false;
  nvs_handle_t h;
  if(nvs_open("halo",NVS_READWRITE,&h)!=ESP_OK){g_ota_storage_uncertain=true;return false;}
  if(!ota_storage_mutation_open()){nvs_close(h);return false;}
  g_ota_storage_uncertain=true;
  const esp_err_t wrote=value?nvs_set_u32(h,"lcd_ota_due",1):nvs_erase_key(h,"lcd_ota_due");
  const esp_err_t committed=wrote==ESP_OK?nvs_commit(h):wrote;
  uint32_t actual=0;const esp_err_t read=committed==ESP_OK?nvs_get_u32(h,"lcd_ota_due",&actual):committed;
  const bool verified=committed==ESP_OK && (value?(read==ESP_OK && actual==1):read==ESP_ERR_NVS_NOT_FOUND);
  nvs_close(h);
  if(verified){g_ota_storage_uncertain=false;g_lcd_due_ram_obligation=value;}
  else g_lcd_due_ram_obligation=true;
  Serial.printf("[OTA_NVS] key=lcd_ota_due due=%u verified=%u\n",value?1:0,verified?1:0);
  return verified;
}
static bool ota_storage_bind_target(const char* version) {
  if(!ota_storage_mutation_open() || !ota_storage_known_clear() || !version || !version[0] ||
      strlen(version)>=sizeof(g_coord_completion_target))return false;
  if(!g_coord_pending[0])return true;
  NvsCapacityLease lease;
  if(!lease || !wakelog_nvs_prepare_essential(1+nvs_capacity_string_entries(strlen(version)),coord_credit_budget_open))return false;
  nvs_handle_t h;
  if(nvs_open("ota_coord",NVS_READWRITE,&h)!=ESP_OK){g_ota_storage_uncertain=true;return false;}
  if(!ota_storage_mutation_open() || !ota_storage_known_clear()){nvs_close(h);return false;}
  g_ota_storage_uncertain=true;
  const esp_err_t wrote=nvs_set_str(h,"target",version);
  const esp_err_t committed=wrote==ESP_OK?nvs_commit(h):wrote;
  char actual[sizeof(g_coord_completion_target)]={0};size_t size=sizeof(actual);
  const bool verified=committed==ESP_OK && nvs_get_str(h,"target",actual,&size)==ESP_OK &&
      size==strlen(version)+1 && !strcmp(actual,version);
  nvs_close(h);
  if(verified){g_ota_storage_uncertain=false;strlcpy(g_coord_completion_target,version,sizeof(g_coord_completion_target));}
  Serial.printf("[OTA_NVS] key=target verified=%u\n",verified?1:0);
  return verified;
}
static bool ota_storage_init_generation() {
  g_coord_sense_boot_id=0;
  if(g_ota_storage_uncertain || g_serial_install_uncertain || g_coord_credit_uncertain || g_nvs_reclaim_uncertain)return false;
  nvs_handle_t h;const esp_err_t opened=nvs_open("ota_coord",NVS_READONLY,&h);
  uint32_t previous=0;esp_err_t read=ESP_ERR_NVS_NOT_FOUND;
  if(opened==ESP_OK){read=nvs_get_u32(h,"generation",&previous);nvs_close(h);}
  else if(opened!=ESP_ERR_NVS_NOT_FOUND){g_ota_storage_uncertain=true;return false;}
  if(read!=ESP_OK && read!=ESP_ERR_NVS_NOT_FOUND){g_ota_storage_uncertain=true;return false;}
  const uint32_t next=previous==UINT32_MAX?1:previous+1; // existing wire wrap rule
  NvsCapacityLease lease;size_t available=0;
  if(!lease){g_ota_storage_uncertain=true;return false;}
  // The generation scalar is old-reader compatible. Before VALID it can use
  // available room, but cannot trigger new-codec reclamation or bootstrap.
  const bool room=coord_credit_format_write_ready()?
      wakelog_nvs_prepare_essential(2,coord_credit_budget_open):
      (nvs_capacity_available(available) && available>=2 && coord_credit_budget_open());
  if(!room){g_ota_storage_uncertain=true;return false;}
  if(nvs_open("ota_coord",NVS_READWRITE,&h)!=ESP_OK){g_ota_storage_uncertain=true;return false;}
  if(!coord_credit_budget_open()){nvs_close(h);g_ota_storage_uncertain=true;return false;}
  g_ota_storage_uncertain=true;
  const esp_err_t wrote=nvs_set_u32(h,"generation",next);
  const esp_err_t committed=wrote==ESP_OK?nvs_commit(h):wrote;
  uint32_t actual=0;
  const bool verified=committed==ESP_OK && nvs_get_u32(h,"generation",&actual)==ESP_OK && actual==next;
  nvs_close(h);
  if(verified){g_coord_sense_boot_id=next;g_ota_storage_uncertain=false;}
  Serial.printf("[OTA_NVS] key=generation verified=%u\n",verified?1:0);
  return verified;
}

// Caller holds NvsCapacityLease through this proof and boot selection. An old
// automatic previous-slot revert is allowed only while storage is known to be
// readable by the legacy firmware. Do not guess compatibility from its version.
static bool coord_credit_legacy_revert_safe() {
  if(!g_coord_credit_loaded || !g_coord_credit_mutations || g_coord_credit_uncertain ||
     g_ota_storage_uncertain || g_serial_install_uncertain || g_nvs_reclaim_uncertain)return false;
  auto unknown=[](){g_coord_credit_uncertain=true;g_coord_credit_mutations=false;return false;};
  nvs_handle_t h;const esp_err_t opened=nvs_open("ota_coord",NVS_READONLY,&h);
  if(opened==ESP_ERR_NVS_NOT_FOUND)return true;
  if(opened!=ESP_OK)return unknown();
  size_t needed=0;const esp_err_t sized=nvs_get_blob(h,"elig_v1",nullptr,&needed);
  if(sized==ESP_ERR_NVS_NOT_FOUND){nvs_close(h);return true;}
  if(sized!=ESP_OK){nvs_close(h);return unknown();}
  if(needed!=COORDINATOR_CREDIT_V1_BYTES){nvs_close(h);return needed==COORDINATOR_CREDIT_BYTES?false:unknown();}
  uint8_t bytes[COORDINATOR_CREDIT_V1_BYTES];size_t actual=sizeof(bytes);
  const esp_err_t read=nvs_get_blob(h,"elig_v1",bytes,&actual);nvs_close(h);
  CoordinatorCreditState legacy;
  if(read!=ESP_OK || actual!=sizeof(bytes) || !credit_decode(bytes,sizeof(bytes),legacy))return unknown();
  return true;
}

static CoordinatorCreditState coord_credit_base() {
  CoordinatorCreditState candidate = g_coord_credit;
  if(candidate.deferred.id[0] && ota_peer_schedule_completed(candidate.deferred.id))candidate.deferred={};
  if(candidate.schedule_observed && ota_peer_schedule_completed(candidate.schedule.id))candidate.schedule_observed=false;
  if (candidate.pending.id[0] && ota_peer_schedule_completed(candidate.pending.id)) {
    candidate.pending = {}; candidate.pending_resolved = false;
    candidate.pending_credit_admitted = false; candidate.admitted_epoch = 0;
  }
  return candidate;
}
static void coord_credit_publish() {
  strlcpy(g_coord_schedule, g_coord_credit.schedule.id, sizeof(g_coord_schedule));
  const bool inactive = g_coord_credit.pending_resolved ||
      ota_peer_schedule_completed(g_coord_credit.pending.id);
  strlcpy(g_coord_pending, inactive ? "" : g_coord_credit.pending.id, sizeof(g_coord_pending));
  if (inactive) g_coord_completion_target[0] = 0;
}
static bool coord_credit_save(const CoordinatorCreditState& candidate) {
  if (!g_coord_credit_loaded || !g_coord_credit_mutations || !coord_credit_budget_open() ||
      !coord_credit_format_write_ready() || g_nvs_reclaim_uncertain || g_ota_storage_uncertain) return false;
  if(coord_credit_same_persisted(candidate))return true;
  NvsCapacityLease lease;
  if(!lease || !wakelog_nvs_prepare_essential(2+(COORDINATOR_CREDIT_BYTES+31)/32,coord_credit_budget_open))return false;
  Preferences p;
  if (!p.begin("ota_coord", false)) return false;
  if (!coord_credit_budget_open() || !coord_credit_format_write_ready()) { p.end(); return false; }
  const bool ok = credit_commit(p, candidate, g_coord_credit, g_coord_credit_mutations);
  if (!ok) g_coord_credit_uncertain = true;
  p.end();
  Serial.printf("[OTA_NVS] key=elig_v1 codec=2 verified=%u bytes=%u\n",ok?1:0,(unsigned)COORDINATOR_CREDIT_BYTES);
  if (ok) {g_coord_credit_persisted=true;coord_credit_publish();}
  return ok;
}
static bool coord_credit_read_text(nvs_handle_t handle, const char* key, char* out, size_t capacity) {
  size_t needed = 0;
  esp_err_t status = nvs_get_str(handle, key, nullptr, &needed);
  if (status == ESP_ERR_NVS_NOT_FOUND) { out[0] = 0; return true; }
  if (status != ESP_OK || needed == 0 || needed > capacity) return false;
  size_t actual = needed;
  status = nvs_get_str(handle, key, out, &actual);
  return status == ESP_OK && actual == needed && out[needed - 1] == 0 &&
      memchr(out, 0, needed - 1) == nullptr;
}
static bool coord_credit_history_valid(const char* text) {
  if (!text[0]) return true;
  unsigned count = 0; const char* cursor = text;
  while (*cursor) {
    const char* end = strchr(cursor, '\n');
    const size_t size = end ? (size_t)(end - cursor) : strlen(cursor);
    if (!size || size >= 64 || ++count > 8 || memchr(cursor, '\r', size)) return false;
    if (!end) return true;
    cursor = end + 1;
    if (!*cursor) return false;
  }
  return true;
}
static void coord_credit_load(Preferences& p) {
  if (g_coord_credit_load_attempted) return;
  g_coord_credit_load_attempted = true;
  // Only explicit NOT_FOUND means absent. Preferences isKey/getString collapse
  // read/type failures, which must never authorize import or empty history.
  nvs_handle_t handle;
  if (nvs_open("ota_coord", NVS_READONLY, &handle) != ESP_OK) return;
  char schedule[64] = {}, pending[64] = {}, complete[64] = {}, history[512] = {}, target[32] = {};
  const bool text_ok = coord_credit_read_text(handle, "schedule", schedule, sizeof(schedule)) &&
      coord_credit_read_text(handle, "pending", pending, sizeof(pending)) &&
      coord_credit_read_text(handle, "complete", complete, sizeof(complete)) &&
      coord_credit_read_text(handle, "done_ids", history, sizeof(history)) &&
      coord_credit_read_text(handle, "target", target, sizeof(target));
  size_t blob_size = 0;
  const esp_err_t blob_status = nvs_get_blob(handle, "elig_v1", nullptr, &blob_size);
  nvs_close(handle);
  if (!text_ok || (blob_status != ESP_OK && blob_status != ESP_ERR_NVS_NOT_FOUND) ||
      !credit_text(schedule, sizeof(schedule), true) || !credit_text(pending, sizeof(pending), true) ||
      !credit_text(complete, sizeof(complete), true) || !credit_text(target, sizeof(target), true) ||
      !coord_credit_history_valid(history)) return;
  strlcpy(g_coord_schedule, schedule, sizeof(g_coord_schedule));
  strlcpy(g_coord_pending, pending, sizeof(g_coord_pending));
  strlcpy(g_coord_completed, complete, sizeof(g_coord_completed));
  ota_peer_load_completed_history(history);
  strlcpy(g_coord_completion_target, target, sizeof(g_coord_completion_target));
  // Preferences/NVS startup recovery has happened before this first load.
  // Do not reload after a write failure in this boot: REMOVE_FAILED may leave
  // duplicate indexes requiring normal initialization recovery.
  if (blob_status == ESP_OK) {
    g_coord_credit_loaded = credit_reload(p, g_coord_credit, g_coord_credit_mutations);
    g_coord_credit_persisted=g_coord_credit_loaded;
  } else {
    CoordinatorCreditState imported;
    strlcpy(imported.schedule.id, schedule, sizeof(imported.schedule.id));
    strlcpy(imported.pending.id, pending, sizeof(imported.pending.id));
    if (!credit_state_shape(imported)) return;
    // Legacy IDs have no trustworthy target/TZ/admission; never infer them.
    g_coord_credit_mutations = true;
    if(coord_credit_format_write_ready()){
      NvsCapacityLease lease;
      if(!lease || !wakelog_nvs_prepare_essential(2+(COORDINATOR_CREDIT_BYTES+31)/32,coord_credit_budget_open)){
        g_coord_credit_mutations=false;return;
      }
      g_coord_credit_loaded = credit_commit(p, imported, g_coord_credit, g_coord_credit_mutations);
      g_coord_credit_persisted=g_coord_credit_loaded;
      Serial.printf("[OTA_NVS] key=elig_v1 codec=2 import=1 verified=%u bytes=%u\n",g_coord_credit_loaded?1:0,(unsigned)COORDINATOR_CREDIT_BYTES);
      if (!g_coord_credit_loaded) g_coord_credit_uncertain = true;
    }else{
      // Definitive absent blob + typed legacy values are authoritative RAM
      // input only. Leave rollback-readable storage untouched before VALID.
      g_coord_credit=imported;g_coord_credit_loaded=true;
    }
  }
  if (g_coord_credit_loaded) coord_credit_publish();
}
static bool coord_credit_timezone_matches_configuration(char (&confirmed)[64]) {
  const auto status=ProvisioningState::loadTimezoneStatus(confirmed,sizeof(confirmed));
  if(status==ProvisioningState::TimezoneStatus::Unknown)return false;
  if(status==ProvisioningState::TimezoneStatus::Absent)strlcpy(confirmed,HALO_DEFAULT_TZ,sizeof(confirmed));
  return g_tz_initialized && !strcmp(confirmed,g_tz_current) && nightly_credit_timezone_matches(confirmed);
}
static bool coord_credit_retire_configured_timezone() {
  std::lock_guard<std::recursive_mutex> config_lock(ProvisioningState::timezoneMutex());
  std::lock_guard<std::recursive_mutex> time_lock(g_time_mutex);
  char confirmed[64];
  // Temporary readiness/unknown configuration does not discard origins or
  // prevent independent repair. Calendar arm binding separately requires proof.
  if(!coord_credit_timezone_matches_configuration(confirmed))return true;
  if(!g_coord_credit_loaded || !g_coord_credit_mutations || g_ota_apply_in_progress)return true;
  CoordinatorCreditState candidate;
  const unsigned retired=credit_retire_timezone(g_coord_credit,confirmed,g_calendar_timer_origin,candidate);
  if(!retired)return true;
  const bool timer_retired=((retired&1)&&credit_same_origin(g_calendar_timer_origin,g_coord_credit.deferred)) ||
      ((retired&2)&&credit_same_origin(g_calendar_timer_origin,g_coord_credit.schedule));
  char old_deferred[64],old_schedule[64];
  strlcpy(old_deferred,g_coord_credit.deferred.id,sizeof(old_deferred));
  strlcpy(old_schedule,g_coord_credit.schedule.id,sizeof(old_schedule));
  if(!coord_credit_save(candidate))return false;
  if(timer_retired)g_calendar_timer_origin={};
  Serial.printf("[OTA_TZ_RETIRE] deferred=%s observed_schedule=%s verified=1 credited=0\n",
      (retired&1)?old_deferred:"-",(retired&2)?old_schedule:"-");
  return true;
}

static bool coord_credit_reserve_timer() {
  if(!coord_credit_retire_configured_timezone())return false;
  if(!g_calendar_timer_origin.id[0])return true;
  if(!g_coord_credit_loaded || !g_coord_credit_mutations || g_coord_credit_uncertain)return false;
  const CoordinatorCreditState base=coord_credit_base();
  if(ota_peer_schedule_completed(g_calendar_timer_origin.id) ||
     (base.pending_credit_admitted && !strcmp(base.pending.id,g_calendar_timer_origin.id)))return true;
  CoordinatorCreditState candidate;
  if(!credit_observe_calendar(base,g_calendar_timer_origin,candidate))return false;
  if(credit_same_origin(candidate.deferred,base.deferred) &&
     candidate.schedule_observed==base.schedule_observed)return true;
  return coord_credit_save(candidate);
}
static bool coord_credit_store_arm(const char* id, uint32_t target) {
  std::lock_guard<std::recursive_mutex> config_lock(ProvisioningState::timezoneMutex());
  if (!g_coord_credit_loaded || !g_coord_credit_mutations || !id || !id[0]) return false;
  if(!coord_credit_reserve_timer())return false;
  NightlyCreditOrigin origin;
  {
    std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
    if (nightly_credit_is_calendar(id)) {
      char confirmed[64];
      if(!coord_credit_timezone_matches_configuration(confirmed))return false;
      if (!g_tz_initialized || !nightly_credit_bind(origin, id, target, g_tz_current,
                                                    sense_time_has_fresh_sync())) return false;
    } else {
      if (strlen(id) >= sizeof(origin.id)) return false;
      strlcpy(origin.id, id, sizeof(origin.id));
    }
  }
  CoordinatorCreditState candidate;
  return credit_next_arm(coord_credit_base(), origin, candidate) && coord_credit_save(candidate);
}
static bool coord_credit_notice_matches(const char* id) {
  return id && id[0] &&
      (self_retry_notice_due(id) ||
       (g_coord_pending[0] && !strcmp(id,g_coord_pending)) ||
       (g_coord_schedule[0] && !strcmp(id,g_coord_schedule)) ||
       (g_coord_credit_loaded && g_coord_credit.deferred.id[0] &&
        !strcmp(id,g_coord_credit.deferred.id)));
}
static bool coord_credit_notice_ready() {
  if(!coord_credit_retire_configured_timezone())return false;
  // This due arm only wakes the existing unresolved repair; it is not a new
  // calendar origin and never substitutes for its pending completion identity.
  if (self_retry_notice_due(g_lcd_timer_notice.schedule)) return true;
  if (!g_coord_credit_loaded || !g_coord_credit_mutations) {
    // Existing unknown debt can still repair, but cannot gain nightly credit.
    return g_coord_pending[0] && strcmp(g_lcd_timer_notice.schedule, g_coord_pending) == 0;
  }
  const CoordinatorCreditState base = coord_credit_base();
  const bool original = base.pending.id[0] && !base.pending_resolved &&
      strcmp(g_lcd_timer_notice.schedule, base.pending.id) == 0;
  const bool deferred=base.deferred.id[0] && !strcmp(g_lcd_timer_notice.schedule,base.deferred.id);
  const NightlyCreditOrigin& origin = original ? base.pending : deferred ? base.deferred : base.schedule;
  if (strcmp(g_lcd_timer_notice.schedule, origin.id) != 0) return false;
  if (original || !origin.bound) return true; // uncredited repair keeps original pending
  uint64_t now; bool fresh, finished; char tz[64]; coord_credit_clock(now, fresh, tz, finished);
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  const auto decision = nightly_credit_decide(origin, origin.id, now, fresh, tz);
  return decision == NightlyCreditDecision::Due || decision == NightlyCreditDecision::NonCalendar;
}
// A matching future calendar announcement is not itself a repair obligation.
// Unknown clock/storage/configuration and independently owed work keep their
// existing bootstrap/recovery path. Use the same bound-origin calendar policy
// as notice readiness and work admission, never the peer's claimed wake epoch.
static bool coord_credit_notice_future_without_repair(const char* id, uint64_t* until_due_ms = nullptr) {
  if (!id || !id[0] || !g_coord_credit_loaded || !g_coord_credit_mutations ||
      g_coord_credit_uncertain || g_nvs_reclaim_uncertain || g_serial_install_uncertain ||
      g_ota_storage_uncertain || g_peer_continue_work || g_lcd_work_budget_live ||
      g_ota_check_requested || g_ota_check_in_progress || g_ota_apply_in_progress ||
      g_lcd_ota_task_running) return false;
  const CoordinatorCreditState base = coord_credit_base();
  if (g_coord_pending[0] || (base.pending.id[0] && !base.pending_resolved) ||
      base.deferred.id[0] || !base.schedule.bound || strcmp(id, base.schedule.id)) return false;
  {
    std::lock_guard<std::recursive_mutex> config_lock(ProvisioningState::timezoneMutex());
    std::lock_guard<std::recursive_mutex> time_lock(g_time_mutex);
    char confirmed[64];
    if (!coord_credit_timezone_matches_configuration(confirmed)) return false;
    uint64_t now; bool fresh, finished; char tz[64]; coord_credit_clock(now, fresh, tz, finished);
    if (nightly_credit_decide(base.schedule, id, now, fresh, tz) != NightlyCreditDecision::Future) return false;
    if (until_due_ms) *until_due_ms = (uint64_t(base.schedule.target_epoch) - 15ULL - now) * 1000ULL;
  }
  // Read debt only for a positively future origin; a failed typed read returns
  // owed/unknown, so it cannot turn an uncertain repair into a cancellation.
  return !get_lcd_ota_due_nvs();
}
// Keep an already captured early LCD wake only while its actual due boundary
// fits inside the original readiness lease. This grants no work or new time.
static bool coord_credit_future_notice_wait(const char* id, uint64_t until_due_ms) {
  if (!g_boot_ota_pending || strcmp(g_boot_ota_reason, "lcd_timer") || !id ||
      strcmp(id, g_lcd_timer_origin.schedule) || g_peer_gate.entered) return false;
  const uint32_t now_ms = millis();
  int32_t remaining = (int32_t)(g_boot_ota_deadline_ms - now_ms);
  if (g_peer_gate.active) {
    const int32_t peer_remaining = (int32_t)(g_peer_gate.deadline_ms - now_ms);
    if (peer_remaining < remaining) remaining = peer_remaining;
  }
  return remaining > 0 && until_due_ms < (uint32_t)remaining;
}
static bool coord_credit_cancel_future_notice() {
  // Clock proof can arrive after the unknown-time readiness opportunity was
  // queued. Classify its captured ID, not a newer mailbox notice or next arm.
  uint64_t until_due_ms = 0;
  if (!g_boot_ota_pending || strcmp(g_boot_ota_reason, "lcd_timer") ||
      g_peer_gate.entered || !coord_credit_notice_future_without_repair(g_lcd_timer_origin.schedule, &until_due_ms)) return false;
  if (coord_credit_future_notice_wait(g_lcd_timer_origin.schedule, until_due_ms)) return false;
  ota_peer_cancel("calendar_future_rearm");
  boot_ota_finish("calendar_future_rearm");
  // No debt, credit, manual-intent change or sticky episode-finished latch.
  return true;
}
// Returns false while the existing automatic request waits, or after ending
// an unreachable early opportunity. Independent repair retains its old path.
static bool coord_credit_calendar_entry(const char* reason) {
  if(!reason || strcmp(reason,"nightly"))return true;
  if(!coord_credit_reserve_timer())return false;
  const CoordinatorCreditState base=coord_credit_base();
  NightlyCreditOrigin origin=base.deferred.id[0]?base.deferred:g_calendar_timer_origin;
  if(!origin.bound || (base.pending.id[0] && !base.pending_resolved) || get_lcd_ota_due_nvs())return true;
  uint64_t now;bool fresh,finished;char tz[64];coord_credit_clock(now,fresh,tz,finished);
  NightlyCreditDecision decision;
  {std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
   if(credit_due_origin(base,now,fresh,tz,!ota_peer_schedule_completed(base.schedule.id)))return true;
   decision=nightly_credit_decide(origin,origin.id,now,fresh,tz);
   const auto schedule_decision=nightly_credit_decide(base.schedule,base.schedule.id,now,fresh,tz);
   if(schedule_decision==NightlyCreditDecision::Future &&
      (decision!=NightlyCreditDecision::Future || base.schedule.target_epoch<origin.target_epoch)){
     origin=base.schedule;decision=schedule_decision;
   }}
  if(decision==NightlyCreditDecision::ClockUnconfirmed ||
     decision==NightlyCreditDecision::TimezoneUnconfirmed)return false;
  if(decision!=NightlyCreditDecision::Future)return true;
  const uint64_t until_due_ms=(uint64_t(origin.target_epoch)-15ULL-now)*1000ULL;
  int32_t remaining=(int32_t)(g_boot_ota_deadline_ms-millis());
  if(g_peer_gate.active && !g_peer_gate.entered){
    const int32_t peer_remaining=(int32_t)(g_peer_gate.deadline_ms-millis());
    if(peer_remaining<remaining)remaining=peer_remaining;
  }
  if(remaining<=0 || until_due_ms>=(uint32_t)remaining){
    ota_peer_cancel("calendar_future_rearm");
    g_peer_episode_finished=true;g_ota_check_done=true;g_ota_check_requested=false;
    boot_ota_finish("calendar_future_rearm");
  }
  // No new readiness/work/SNTP budget and no timestamp is moved to the due time.
  return false;
}
static bool coord_credit_prepare_work(const char* reason) {
  // Existing continuation is checked again at actual entry against its exact
  // original ID, lcd_timer cause and live budget. It gains no new admission.
  if (g_peer_continue_work) return true;
  if (g_coord_credit_uncertain || g_nvs_reclaim_uncertain || g_serial_install_uncertain || g_ota_storage_uncertain || !g_coord_sense_boot_id) return false; // no new entry after uncertain persistence this boot
#if HALO_DURABLE_OTA_POLICY
  // A verified short arm resumes its own durable campaign. It cannot claim or
  // overwrite a future calendar origin merely because the RTC wake is TIMER.
  if(halo_policy_short_due())return true;
#endif
  ensure_timezone_pt("credit_entry");
  if(!coord_credit_retire_configured_timezone())return false;
  if (reason && !strcmp(reason, "lcd_timer") && coord_credit_cancel_future_notice()) return false;
  if (!coord_credit_calendar_entry(reason)) return false;
  if (!g_coord_credit_loaded || !g_coord_credit_mutations) return !g_ota_storage_uncertain; // repair only; never enter after a newly failed debt read
  CoordinatorCreditState base = coord_credit_base(), candidate;
  uint64_t now; bool fresh, finished; char tz[64]; coord_credit_clock(now, fresh, tz, finished);
  bool changed = false;
  {
    std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
    if (base.pending.id[0] && !base.pending_resolved) {
      changed = credit_admit_pending(base, now, fresh, tz, candidate);
    } else if ((base.deferred.id[0] ||
                (reason && (!strcmp(reason, "nightly") || !strcmp(reason, "lcd_timer")))) &&
               !ota_peer_schedule_completed(base.deferred.id[0]?base.deferred.id:base.schedule.id)) {
      changed = credit_claim_due(base, now, fresh, tz, candidate, !ota_peer_schedule_completed(base.schedule.id));
    }
  }
  if (changed && !coord_credit_save(candidate)) return false;
  // A future scheduled notice cannot become unsolicited work just because
  // its clock wait finished. Independent LCD debt retains its old repair path.
  if (reason && !strcmp(reason, "lcd_timer") && !g_coord_pending[0] &&
      base.schedule.bound && !get_lcd_ota_due_nvs()) return false;
  return !g_ota_storage_uncertain; // debt reads above may have closed this boot
}
// END COORDINATOR_CREDIT_INTEGRATION

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

static bool coord_history_commit() {
  if (!g_coord_pending[0] || g_nvs_reclaim_uncertain || !coord_credit_format_write_ready() || !ota_storage_known_clear()) return false;
  // Keep exact recent IDs, not a date high-water mark: a bad future clock
  // must not suppress a legitimate nightly check after fresh correction.
  if (strchr(g_coord_pending, '\n') || strchr(g_coord_pending, '\r')) return false;
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
  NvsCapacityLease lease;
  if(!lease || !wakelog_nvs_prepare_essential(nvs_capacity_string_entries(strlen(encoded)),coord_credit_budget_open))return false;
  Preferences p;
  if (p.begin("ota_coord", false)) {
    // Opening NVS can block too. Do not begin a new completion commit after
    // the current work budget expires; a successful write below is final.
    if (g_lcd_work_budget_live && !g_lcd_work_budget.remaining_ms()) {
      p.end();
      return false;
    }
    // The entire eight-ID history is one atomic NVS value. Publish RAM only
    // after it succeeds; a reset before pending removal still finds the ID.
    g_coord_credit_mutations = false;
    g_coord_credit_uncertain = true;
    if (p.putString("done_ids", encoded) == strlen(encoded) &&
        p.getString("done_ids", "") == encoded) {
      Serial.println("[OTA_NVS] key=done_ids verified=1");
      g_coord_credit_mutations = true;
      g_coord_credit_uncertain = false;
      memcpy(g_coord_completed_ids, next, sizeof(next));
      g_coord_completed_count = count;
      strlcpy(g_coord_completed, g_coord_pending, sizeof(g_coord_completed));
      g_coord_pending[0] = 0;
      g_lcd_work_budget_live = false; g_peer_continue_work = false;
      p.remove("pending");
      p.remove("target");
      p.end(); return true;
    }
    p.end();
  }
  return false;
}

static CoordCompletion ota_peer_schedule_complete() {
  if(!ota_storage_known_clear())return CoordCompletion::Deferred;
  if (!g_coord_pending[0]) return CoordCompletion::NoPending;
  if (!g_coord_credit_loaded || !g_coord_credit_mutations ||
      strcmp(g_coord_credit.pending.id, g_coord_pending) != 0 || !coord_credit_budget_open())
    return CoordCompletion::Deferred;
  uint64_t now; bool fresh, finished; char tz[64]; coord_credit_clock(now, fresh, tz, finished);
  CoordinatorCreditState candidate; CreditResolution resolution;
  {
    std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
    resolution = credit_resolve(g_coord_credit, true, now, fresh, tz, finished, candidate);
  }
  if (resolution == CreditResolution::Unresolved) return CoordCompletion::Deferred;
  if (resolution == CreditResolution::CreditDue)
    return coord_history_commit() ? CoordCompletion::Credited : CoordCompletion::Deferred;
  if (!coord_credit_save(candidate)) return CoordCompletion::Deferred;
  // The authoritative blob resolves repair before compatibility cleanup.
  // Failures below cannot resurrect it or credit the nightly ID after reboot.
  g_coord_completion_target[0] = 0;
  g_lcd_work_budget_live = false; g_peer_continue_work = false;
  Preferences p;
  if (p.begin("ota_coord", false)) { p.remove("pending"); p.remove("target"); p.end(); }
  return CoordCompletion::ResolvedUncredited;
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
  coord_credit_cancel_future_notice();
  uint64_t until_due_ms = 0;
  if (g_lcd_timer_notice.pending &&
      coord_credit_notice_future_without_repair(g_lcd_timer_notice.schedule, &until_due_ms) &&
      !coord_credit_future_notice_wait(g_lcd_timer_notice.schedule, until_due_ms))
    g_lcd_timer_notice.pending = false; // Leave room for a later due/relative notice.
  ota_control_probe_service();
  // Do not consume/mark a new boot seen while the old transaction is busy or
  // waiting to run its deadline teardown. The next loop can accept it after
  // that teardown, preserving the late peer's independent opportunity.
  const uint32_t notice_now = millis();
  if (g_lcd_timer_notice.pending && !g_boot_ota_pending && !g_peer_gate.active &&
      !g_peer_episode_finished && !g_ota_check_in_progress && !g_ota_apply_in_progress &&
      !g_lcd_ota_task_running &&
      coord_credit_notice_matches(g_lcd_timer_notice.schedule) &&
      !ota_peer_schedule_completed(g_lcd_timer_notice.schedule)) {
    // Establish the existing readiness opportunity once; a deferred notice
    // neither renews it nor blocks the rest of this service function.
    const bool same_work = g_lcd_work_budget_live && g_lcd_work_schedule[0] &&
        !strcmp(g_lcd_timer_notice.schedule, g_lcd_work_schedule);
    if (!same_work || (g_lcd_work_budget.remaining_ms() &&
                      g_lcd_timer_notice.boot_id != g_lcd_work_peer_boot)) {
      g_peer_continue_work = same_work;
      g_lcd_timer_origin = g_lcd_timer_notice;
      g_lcd_timer_seen_boot = g_lcd_timer_notice.boot_id;
      boot_ota_queue("lcd_timer");
    }
  }
  const bool old_wait_expired =
      (g_boot_ota_pending && (int32_t)(notice_now - g_boot_ota_deadline_ms) >= 0) ||
      (g_peer_gate.active && !g_peer_gate.entered &&
       (int32_t)(notice_now - g_peer_gate.deadline_ms) >= 0);
  if (g_lcd_timer_notice.pending && !old_wait_expired &&
      !g_ota_check_in_progress && !g_ota_apply_in_progress && !g_lcd_ota_task_running &&
      !(g_peer_gate.active && g_peer_gate.entered) && coord_credit_notice_ready()) {
    g_lcd_timer_notice.pending = false;
    const bool expected =
        coord_credit_notice_matches(g_lcd_timer_notice.schedule) &&
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
      // Pending creation and due admission occur only at actual work entry.
      // The notice itself is not proof of an eligible nightly obligation.
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

// A user/EXT0 wake before the verified retry target is not permission to run
// the short retry early. Finish that readiness opportunity without moving any
// deadline; the already persisted LCD arm and RTC due time remain unchanged.
static bool self_retry_boot_admit() {
#if HALO_DURABLE_OTA_POLICY
  return halo_policy_boot_ready();
#else
  // Do not consume debt/requests or begin a new OTA before local VALID.
  if (!nvs_capacity_image_valid()) return false;
  if (!g_self_retry_boot) return true;
  if (!self_retry_bound() || !self_retry_time(g_self_retry,self_retry_now(),sense_time_has_fresh_sync())) {
    if (!sense_time_has_fresh_sync()) return false; // existing readiness bound still expires
    g_self_retry.phase=SelfOtaRetryPhase::CLOSED;
    ota_peer_cancel("self_retry_expired");boot_ota_finish("self_retry_expired");
    g_peer_episode_finished=true;g_ota_check_done=true;return false;
  }
  if (!self_retry_consume(g_self_retry,g_coord_pending,g_coord_completion_target,
                          self_retry_now(),sense_time_has_fresh_sync())) {
    ota_peer_cancel("self_retry_not_due");boot_ota_finish("self_retry_not_due");
    g_self_retry_boot=false;
    g_peer_episode_finished=true;g_ota_check_done=true;return false;
  }
  g_self_retry_boot=false;g_self_retry_execution=true;
  LOG_INFO("[SELF_RETRY] consumed origin=%s target=%s expires=%lu",
           g_self_retry.origin,g_self_retry.version,(unsigned long)g_self_retry.expires_epoch);
  return true;
#endif
}

// Called after normal provisioning/time/health service, independent of the old
// maintenance flags. A readiness skip keeps the request alive until the bound;
// a check that began consumes it even if its manifest fetch or update failed.

static void nightly_maintenance_tick() {
  // Do not consume debt/requests or begin a new OTA before local VALID.
  if (!nvs_capacity_image_valid()) return;
  if (coord_credit_cancel_future_notice()) return;
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
  // HTTPS work. The common entrypoint requires an actual fresh reply; a
  // retained TLS clock alone cannot authorize the paired absolute sleep arm.
  if (sense_ntp_attempt_pending()) return;
  if (!self_retry_boot_admit()) return;
  // A clean scheduled wake can recover even without a plausible retained TLS
  // clock. This gate still requires a fresh reply before any OTA admission.
  if (ota_scheduled_clock_retry_deadline() && !ota_clock_ready_before_work()) return;
  if (!is_time_valid() || (!OtaIntent::cooldownAllows() && !ota_peer_continuation())) return;


  g_boot_ota_diag_context = true;
  maybeRunOtaCheck(g_boot_ota_reason, true);
  g_boot_ota_diag_context = false;
  if (g_ota_check_done && g_boot_ota_pending) {
    uart_send_sense_diag("ota", nightly ? "nightly_done" : "lcd_recovery_done",
                         g_boot_ota_reason, 1, "check_started");
    boot_ota_finish("check_started");
  }
}


#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
static void sense_diag_export_retained(uint32_t deadline);
static void sense_diag_export_budget_skipped(uint32_t deadline);
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_OTA_POLICY
static void halo_diag_admission_export(uint32_t deadline);
#endif
#endif

void halo_prod_pre_sleep() {
  LOG_INFO("[PRE_SLEEP] window start");
  const unsigned long pre_sleep_budget_ms = 20000;
  const unsigned long pre_sleep_start_ms = millis();
  sense_idle_network::Window idle_window(uint32_t(pre_sleep_start_ms+pre_sleep_budget_ms));
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
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  // Reserve6s for ordinary-report detector admission and local preparation.
  // Reset completion is not claimed within this window. Exports use only the rest
  // of this original20s window and never open an OTA/network retry opportunity.
  if(remaining_budget_ms()>sense_idle_network::REPORT_RESERVE_MS+sense_idle_network::POST_ADMISSION_MS)
    sense_diag_export_retained(uint32_t(pre_sleep_start_ms+pre_sleep_budget_ms-sense_idle_network::REPORT_RESERVE_MS));
  else sense_diag_export_budget_skipped(uint32_t(pre_sleep_start_ms+pre_sleep_budget_ms-sense_idle_network::REPORT_RESERVE_MS));
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_OTA_POLICY
  // Sequential to avoid nesting the full exporter frame. Keep the original
  // deadline and ordinary-report reserve; this call grants no extra allowance.
  if(remaining_budget_ms()>sense_idle_network::REPORT_RESERVE_MS+sense_idle_network::POST_ADMISSION_MS)
    halo_diag_admission_export(uint32_t(pre_sleep_start_ms+pre_sleep_budget_ms-sense_idle_network::REPORT_RESERVE_MS));
#endif
#endif
  {
    uint32_t report_timeout_ms = (uint32_t)remaining_budget_ms();
    if (report_timeout_ms > OTA_REPORT_HTTP_TIMEOUT_MS) {
      report_timeout_ms = OTA_REPORT_HTTP_TIMEOUT_MS;
    }
    if (remaining_budget_ms() >= sense_idle_network::POST_ADMISSION_MS) {
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
  // Explicit retry owns a fresh setup session. startSetupMode() is otherwise
  // idempotent while the failed session's AP is still running, leaving the
  // state unprovisioned and preventing the periodic QR from being sent.
  if (g_provisioning_manager.isSetupModeActive()) g_provisioning_manager.stopSetupMode();
  g_provision_display_status.reset();
  g_last_provision_display_status[0] = 0;
  g_provision_display_poll_ms = 0;
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
#if HALO_DURABLE_OTA_POLICY
  // The checked policy owns completion/rollback. Preserve legacy expectation
  // bytes for its read-only migration; unchecked partial clears are not proof.
  return;
#endif
  if (!nvs_capacity_image_valid()) return; // No expectation/peer side effects before actual VALID.
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
      NvsCapacityLease storage_lease;
      if(!storage_lease || !coord_credit_legacy_revert_safe()) {
        Serial.println("[OTA_EXPECT] previous-slot revert blocked: storage compatibility unproven; pending retained");
        return;
      }
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

  // Actual VALID is already proven. Keep metadata for a later boot if the
  // existing expectation API refuses the clear; do not emit success/unlock.
  if (!OtaExpect::clearPending()) return;
  OtaExpect::setLastSuccessTs((uint32_t)(millis() / 1000));

  // OTA succeeded; release LCD OTA lock — unless LCD OTA is still pending
  if (get_lcd_ota_due_nvs()) {
    Serial.println("[OTA_EXPECT] version match — keep OTA_LOCK (lcd_ota_due pending)");
  } else {
    Serial.println("[OTA_EXPECT] version match -> send OTA_UNLOCK to LCD");
    send_ota_uart_message("OTA_UNLOCK", true);
  }

#if HALO_SIMPLE_OTA_PROOF
  g_ota_validated_this_boot = true;
  g_ota_validated_time_ms = millis();
  g_ota_simple_proof_started = true;
  g_ota_proof_start_ms = millis();
  Serial.println("[OTA_PROOF] begin");
#endif
}

static void service_pending_ota_expectation_after_validation() {
#if HALO_DURABLE_OTA_POLICY
  // The checked policy owns completion/rollback. Preserve legacy expectation
  // bytes for its read-only migration; unchecked partial clears are not proof.
  return;
#endif
  // Reuse the existing once-per-boot validation flag. Setup may have deferred
  // this event; HealthGate retains its expected/previous metadata until here.
  if (g_ota_validated_this_boot || !nvs_capacity_image_valid()) return;
  g_ota_validated_this_boot = true;
  handle_pending_ota_expectation();
}

static void handle_ota_proof() {
#if HALO_DURABLE_OTA_POLICY
  // The checked policy owns completion/rollback. Preserve legacy expectation
  // bytes for its read-only migration; unchecked partial clears are not proof.
  return;
#endif
#if HALO_SIMPLE_OTA_PROOF
  if (!nvs_capacity_image_valid()) return; // Deferred proof remains pending until actual VALID.
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
    // Expectation success belongs to the validated handler, not this connectivity proof.
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

#if HALO_DURABLE_OTA_POLICY
static bool halo_policy_bind_pair(const OtaManifest&,const char*);
static bool halo_policy_resolve_pair();
static bool halo_policy_resolve_superseded();
#endif
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
  // MQTT/camera DMA are already released on the OTA path by the caller.
  // Manifest fetch owns and destroys its request-local TLS client.
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
#if HALO_DURABLE_OTA_POLICY
  if(!halo_policy_bind_pair(lcd_manifest,lcd_fw))return false;
#endif
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
    if(!set_lcd_ota_due_nvs(false))return false;
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
    if(!set_lcd_ota_due_nvs(true))break; // durable retry obligation before mutating LCD
    ota_heap::proxy_invoked();
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
  if(!set_lcd_ota_due_nvs(!ok))ok=false;
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
  const CoordCompletion resolution = ota_peer_schedule_complete();
  if (resolution == CoordCompletion::Deferred) return false;
  if (had_pending) g_coord_completion_target[0] = 0;

  char detail[192];
  snprintf(detail, sizeof(detail), "sense=%.31s manifest=%.31s policy=downgrade_blocked lcd=%.31s lcd_result=%.31s",
           kFirmwareVersion, manifest_fw, g_lcd_ota_version, g_lcd_ota_result);
  uart_send_sense_diag_persist("ota", "policy_result", "downgrade_blocked", 0, detail);
  if (g_boot_ota_diag_context && g_boot_ota_begin_reported) {
    snprintf(detail, sizeof(detail), "%s policy=downgrade_blocked lcd=%.31s result=%.15s",
             g_boot_ota_begin_record.detail, g_lcd_ota_version, g_lcd_ota_result);
    uart_send_sense_diag_persist("ota", resolution == CoordCompletion::ResolvedUncredited ?
                                "repair_resolved" : "policy_complete", g_boot_ota_begin_record.reason,
                                g_boot_ota_begin_record.code, detail);
  } else {
    uart_send_sense_diag_persist("ota", resolution == CoordCompletion::ResolvedUncredited ?
                                "repair_resolved" : "policy_complete", reason ? reason : "manual", 0, detail);
  }
#if HALO_DURABLE_OTA_POLICY
  if(!halo_policy_resolve_superseded())return false;
#endif
  return true;
}

// Use the existing two-minute proof budget as reserve before the LCD's 15s
// maintenance lead. Neither admission nor retransmissions reset the pair clock.
static const uint32_t LCD_ARM_PROOF_BUDGET_MS = 120000UL;
static uint32_t lcd_verified_arm_apply_budget() {
  if (!g_lcd_verified_arm.matched || !sense_time_has_fresh_sync() ||
      (uint32_t)(millis() - g_lcd_verified_arm.started_ms) >= g_lcd_verified_arm.budget_ms ||
      !g_lcd_work_budget_live || !sense_lcd_ota_retry_safe()) return 0;
  const time_t now = time(nullptr);
  const uint32_t target = g_lcd_verified_arm.start_epoch;
  if (now < 1700000000 || uint64_t(now) + 15 >= target ||
      uint64_t(now) + halo_seconds_until_maintenance(now) != target) return 0;
  uint64_t until_lcd_ms = (uint64_t(target) - uint64_t(now) - 15) * 1000ULL;
  uint32_t left = g_lcd_work_budget.remaining_ms();
  if (until_lcd_ms < left) left = (uint32_t)until_lcd_ms;
  if (left <= LCD_ARM_PROOF_BUDGET_MS) return 0;
  left -= LCD_ARM_PROOF_BUDGET_MS;
  return left < 1200000UL ? left : 1200000UL;
}
static bool prepare_lcd_absolute_sleep_before_apply() {
  g_lcd_verified_arm = LcdVerifiedArmAck{};
  if (!g_lcd_work_budget_live || !g_lcd_work_budget.remaining_ms() ||
      !sense_lcd_ota_retry_safe() || !sense_time_has_fresh_sync()) return false;
  const uint32_t started = millis();
  uint32_t proof_budget = g_lcd_work_budget.remaining_ms();
  if (proof_budget > LCD_ARM_PROOF_BUDGET_MS) proof_budget = LCD_ARM_PROOF_BUDGET_MS;
  char expected_fw[32]; strlcpy(expected_fw, g_lcd_ota_query_resp_fw, sizeof(expected_fw));
  if (!expected_fw[0]) return false;
  char query_challenge[40];
  snprintf(query_challenge, sizeof(query_challenge), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  if (!sense_lcd_ota_query_start(query_challenge, proof_budget)) return false;
  LcdOtaQuerySnapshot peer = {};
  bool ready = false;
  while ((uint32_t)(millis() - started) < proof_budget && g_lcd_work_budget.remaining_ms()) {
    pump_uart_rx_once();
    LcdOtaQueryPoll result = sense_lcd_ota_query_poll(peer);
    if (result == LCD_QUERY_READY) {
      ready = peer.correlated && peer.peer_boot_id && strcmp(peer.fw, expected_fw) == 0;
      break;
    }
    if (result == LCD_QUERY_TIMEOUT) break;
    delay(10);
  }
  if (!ready || !sense_time_has_fresh_sync()) return false;
  time_t now = time(nullptr);
  const uint32_t delta = halo_seconds_until_maintenance(now);
  if (now < 1700000000 || !delta || uint64_t(now) + delta > UINT32_MAX) return false;
  MaintenanceWindow mw = {};
  // Timer-only arm: the maintenance constructor defaults are not zero.
  mw.duration_sec = 0;
  mw.grace_before_sec = 0;
  mw.grace_after_sec = 0;
  mw.start_epoch = uint64_t(now) + delta;
  struct tm local_target; time_t target = (time_t)mw.start_epoch;
  if (!localtime_r(&target, &local_target)) return false;
  snprintf(mw.request_id, sizeof(mw.request_id), "nightly_%04d%02d%02d",
           local_target.tm_year + 1900, local_target.tm_mon + 1, local_target.tm_mday);
  g_lcd_verified_arm.started_ms = started;
  g_lcd_verified_arm.budget_ms = proof_budget;
  g_lcd_verified_arm.peer_boot_id = peer.peer_boot_id;
  g_lcd_verified_arm.start_epoch = (uint32_t)mw.start_epoch;
  strlcpy(g_lcd_verified_arm.request_id, mw.request_id, sizeof(g_lcd_verified_arm.request_id));
  g_lcd_verified_arm.waiting = true;
  struct WaitingGuard { ~WaitingGuard() { g_lcd_verified_arm.waiting = false; } } waiting_guard;
  unsigned sends = 0;
  uint32_t last_send = 0;
  while ((uint32_t)(millis() - started) < proof_budget && g_lcd_work_budget.remaining_ms()) {
    if (!sense_time_has_fresh_sync() || !sense_lcd_ota_retry_safe()) return false;
    const uint32_t ms = millis();
    if (sends < MAINT_SYNC_MAX_SENDS && (!sends || (uint32_t)(ms - last_send) >= MAINT_SYNC_RESEND_MS)) {
      now = time(nullptr);
      if (now < 1700000000 || uint64_t(now) >= mw.start_epoch) return false;
      const uint64_t remaining = mw.start_epoch - uint64_t(now);
      if (remaining > HALO_MAINTENANCE_MAX_SLEEP_S) return false;
      g_lcd_verified_arm.remaining_s = (uint32_t)remaining;
      snprintf(g_lcd_verified_arm.challenge, sizeof(g_lcd_verified_arm.challenge), "%08lx%08lx",
               (unsigned long)esp_random(), (unsigned long)esp_random());
      g_lcd_verified_arm.matched = false;
      last_send = ms; ++sends;
      send_maint_window(&mw, (uint32_t)remaining, (uint32_t)remaining, false,
                       g_lcd_verified_arm.challenge, peer.peer_boot_id);
    }
    pump_uart_rx_once();
    if (g_lcd_verified_arm.matched) {
      if (!lcd_verified_arm_apply_budget()) return false;
      // One record preserves the NEXT arm and the original pending separately.
      const bool committed = coord_credit_store_arm(mw.request_id, (uint32_t)mw.start_epoch);
      if (!committed) return false;
      strlcpy(g_coord_schedule, mw.request_id, sizeof(g_coord_schedule));
      LOG_INFO("[OTA_ARM] stored_verified id=%s target=%lu lcd_boot=%lu pending=%s",
          mw.request_id, (unsigned long)mw.start_epoch, (unsigned long)peer.peer_boot_id,
          g_coord_pending[0] ? g_coord_pending : "-");
      return lcd_verified_arm_apply_budget() > 0;
    }
    delay(10);
  }
  return false;
}

// Reserve at failure and try once while the LCD may still be awake/locked.
// The query+ACK share a five-second deadline. No old OTA deadline is extended,
// no legacy followup field is used, and failed proof returns to calendar sleep.
static bool self_retry_user_busy() {
  return sense_action_inflight() || foreground_active || voice_recording_active ||
      g_list_screen_active || (op_queue && uxQueueMessagesWaiting(op_queue)>0) ||
      g_provisioning_manager.isSetupModeActive();
}
static void self_retry_reserve_and_arm(const OtaManifest& manifest) {
  if (!g_coord_pending[0] || strcmp(g_coord_completion_target,manifest.version) ||
      !self_retry_reserve(g_self_retry,g_coord_pending,manifest.version,manifest.sha256,
                          manifest.size,self_retry_now(),sense_time_has_fresh_sync())) return;
  struct CloseUnverified {
    ~CloseUnverified() {
      g_self_retry_arm.waiting=false;
      if(g_self_retry.phase==SelfOtaRetryPhase::RESERVED)g_self_retry.phase=SelfOtaRetryPhase::CLOSED;
      LOG_INFO("[SELF_RETRY] arm phase=%u origin=%s target=%s due=%lu expires=%lu",
          (unsigned)g_self_retry.phase,g_self_retry.origin,g_self_retry.version,
          (unsigned long)g_self_retry.due_epoch,(unsigned long)g_self_retry.expires_epoch);
    }
  } close_unverified;
  if (self_retry_user_busy() || g_lcd_ota_proxy_owns_uart || g_lcd_ota_task_running ||
      !sense_lcd_ota_retry_safe()) return;
  const uint32_t started=millis(),budget=5000;
  // The confirmed pre-apply query survives the deliberate display-version
  // cache clear after LCD update. Snapshot it before this new query runs.
  char expected_fw[32];strlcpy(expected_fw,g_lcd_ota_query_resp_fw,sizeof(expected_fw));
  if (!expected_fw[0]) return;
  char challenge[40];snprintf(challenge,sizeof(challenge),"%08lx%08lx",
      (unsigned long)esp_random(),(unsigned long)esp_random());
  if (!sense_lcd_ota_query_start(challenge,budget)) return;
  LcdOtaQuerySnapshot peer={};bool ready=false;
  while ((uint32_t)(millis()-started)<budget) {
    if (self_retry_user_busy()) return;
    pump_uart_rx_once();const auto result=sense_lcd_ota_query_poll(peer);
    if(result==LCD_QUERY_READY){ready=peer.correlated && peer.peer_boot_id &&
        !strcmp(peer.fw,expected_fw);break;}
    if(result==LCD_QUERY_TIMEOUT)return;
    delay(10);
  }
  uint32_t now=self_retry_now();
  if (!ready || !self_retry_time(g_self_retry,now,sense_time_has_fresh_sync()) ||
      uint64_t(now)+SELF_OTA_RETRY_LCD_LEAD_S>=g_self_retry.due_epoch ||
      (uint32_t)(millis()-started)>=budget) return;
  MaintenanceWindow mw={};mw.duration_sec=mw.grace_before_sec=mw.grace_after_sec=0;
  mw.start_epoch=g_self_retry.due_epoch;
  snprintf(g_self_retry.arm_id,sizeof(g_self_retry.arm_id),"self_retry_%08lx_%08lx",
      (unsigned long)g_self_retry.due_epoch,(unsigned long)esp_random());
  strlcpy(mw.request_id,g_self_retry.arm_id,sizeof(mw.request_id));
  g_self_retry_arm={};g_self_retry_arm.started_ms=started;g_self_retry_arm.budget_ms=budget;
  g_self_retry_arm.peer_boot_id=peer.peer_boot_id;g_self_retry_arm.start_epoch=g_self_retry.due_epoch;
  g_self_retry_arm.remaining_s=g_self_retry.due_epoch-now;
  strlcpy(g_self_retry_arm.request_id,mw.request_id,sizeof(g_self_retry_arm.request_id));
  snprintf(g_self_retry_arm.challenge,sizeof(g_self_retry_arm.challenge),"%08lx%08lx",
      (unsigned long)esp_random(),(unsigned long)esp_random());
  g_self_retry_arm.waiting=true;
  send_maint_window(&mw,g_self_retry_arm.remaining_s,g_self_retry_arm.remaining_s,false,
                    g_self_retry_arm.challenge,peer.peer_boot_id);
  while((uint32_t)(millis()-started)<budget) {
    if(self_retry_user_busy() || !self_retry_time(g_self_retry,self_retry_now(),sense_time_has_fresh_sync()))return;
    pump_uart_rx_once();
    if(g_self_retry_arm.matched){
      if ((uint32_t)(millis()-started)<budget && !self_retry_user_busy() &&
          self_retry_time(g_self_retry,self_retry_now(),sense_time_has_fresh_sync()))
        g_self_retry.phase=SelfOtaRetryPhase::ARMED;
      return;
    }
    delay(10);
  }
}

#if HALO_DURABLE_OTA_POLICY
static bool halo_policy_diagnostic_identity(const OtaManifest&,uint8_t(&)[16],char(&)[64],uint32_t&);
#endif
// Production user-work owners: parked images remain owned even with an empty
// queue and no active upload. OTA ownership stays in each caller's own gate.
static bool halo_primary_user_work_busy() {
  return current_job.active || upload_inflight || http_inflight ||
      voice_recording_active || scan_ui_inflight || dish_scan_inflight ||
      foreground_active || upload_worker_has_parked_job || upload_queue_count()!=0 ||
      (op_queue && uxQueueMessagesWaiting(op_queue)) || halo_provisioning_active();
}

#include "../shared/SenseDiagnosticIntegration.h"
#include "../shared/SenseDiagnosticUart.h"
#include "../../../Sense_Minimal/sense_diagnostic_transport.h"
#include "../shared/SenseDiagnosticExport.h"
#include "../shared/SenseDurablePolicyRuntime.h"
#include "../shared/SenseSleepWitnessIntegration.h"
#include "../shared/SenseAdmissionBreadcrumb.h"
#include "../shared/SenseAdmissionExport.h"
#include "../shared/SenseOneShotControl.h"
#include "../shared/SenseBenchControl.h"
#include "../shared/SenseDiagnosticAuth.h"
#include "../shared/SenseDurablePolicyDispatch.h"
#include "../shared/SenseIdleNetworkProbe.h"

static bool halo_idle_network_safe(){
#if HALO_IDLE_NETWORK_RECOVERY && HALO_DURABLE_DIAGNOSTICS
  return nvs_capacity_image_valid()&&sense_diag_export_idle(nullptr)&&!g_diag_budget_ms&&!g_peer_gate.active&&!g_ota_pending_verify_active&&!xPortInIsrContext();
#else
  return false;
#endif
}
static uint32_t halo_idle_network_boot_id(){return g_coord_sense_boot_id;}
static bool halo_idle_network_fresh(){return sense_time_has_fresh_sync();}
static uint32_t halo_idle_network_epoch(){const time_t n=time(nullptr);return n>=halo_idle_net::MIN_EPOCH&&uint64_t(n)<=halo_idle_net::MAX_EPOCH?uint32_t(n):0;}



// A stored arm alone is not proof that this wake was scheduled: spool and
// fallback timers also queue "nightly". Require the actual LCD TIMER notice,
// its current nonce-qualified peer, and a typed absent policy. This authorizes
// a time query only; fresh clock, calendar due/day and budgets still gate OTA.
static uint32_t ota_scheduled_clock_retry_deadline() {
#if HALO_DURABLE_OTA_POLICY
  if (!g_boot_ota_pending || halo_ota_manual_override_active() ||
      !sense_policy::absent() || !g_coord_credit_loaded ||
      g_coord_credit_uncertain || g_ota_storage_uncertain ||
      g_coord_pending[0] || g_coord_completion_target[0] || g_peer_continue_work ||
      !g_peer_gate.active || !g_peer_gate.ready || g_peer_gate.legacy ||
      g_peer_gate.entered || !g_lcd_timer_origin.boot_id ||
      g_lcd_timer_origin.boot_id != g_lcd_timer_seen_boot ||
      g_lcd_timer_origin.boot_id != g_peer_gate.peer_boot ||
      g_lcd_timer_origin.wake != (int)ESP_SLEEP_WAKEUP_TIMER) return 0;
  const uint32_t now = millis();
  if (uint32_t(now - g_peer_gate.proof_ms) >= 2000) return 0;
  int32_t left = int32_t(g_boot_ota_deadline_ms - now);
  const int32_t peer_left = int32_t(g_peer_gate.deadline_ms - now);
  if (peer_left < left) left = peer_left;
  if (left <= 0) return 0;
  const CoordinatorCreditState base = coord_credit_base();
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  auto matches = [&](const NightlyCreditOrigin& origin) {
    return origin.bound && origin.target_epoch && origin.id[0] &&
      !strcmp(origin.id, g_lcd_timer_origin.schedule) &&
      !ota_peer_schedule_completed(origin.id) && g_tz_initialized &&
      !strcmp(origin.timezone, g_tz_current) &&
      nightly_credit_timezone_matches(origin.timezone);
  };
  if (matches(base.schedule) || matches(base.deferred)) return now + uint32_t(left);
#endif
  return 0;
}

// Peer proof can cross its 2s freshness boundary between the caller's ready
// check and this clock gate. Wait for the existing query service in that case;
// it neither grants a secondary attempt nor renews either readiness deadline.
static bool ota_scheduled_clock_peer_refresh_pending() {
#if HALO_DURABLE_OTA_POLICY
  const uint32_t now = millis();
  return g_boot_ota_pending && !halo_ota_manual_override_active() &&
    sense_policy::absent() && g_peer_gate.active && !g_peer_gate.entered &&
    int32_t(g_boot_ota_deadline_ms - now) > 0 &&
    int32_t(g_peer_gate.deadline_ms - now) > 0 &&
    (!g_peer_gate.ready || uint32_t(now - g_peer_gate.proof_ms) >= 2000);
#else
  return false;
#endif
}

// Complete the bounded SNTP opportunity (including one qualified secondary
// attempt) before a long DNS guard can suspend it. Pending is normal-loop
// service, not a consumed OTA attempt.
static bool ota_clock_ready_before_work() {
  if (sense_time_has_fresh_sync()) return true;
  if (halo_ota_manual_override_active()) sense_ntp_request_manual_retry();
  else {
    // Use one qualified deadline snapshot. If proof just expired, the later
    // WAIT check sees that refusal; reversing these checks creates a 2s race.
    const uint32_t deadline = ota_scheduled_clock_retry_deadline();
    if (deadline) sense_ntp_request_scheduled_retry(deadline);
    else if (ota_scheduled_clock_peer_refresh_pending()) return false;
  }
  halo_prod_kick_time_sync("ota_preflight");
  if (sense_time_has_fresh_sync()) return true;
  if (sense_ntp_attempt_pending()) return false;
  // A reply can arrive after kick's service but before the pending check.
  // Drain that mailbox (or the expired attempt) before deciding to defer.
  sense_ntp_service();
  if (sense_time_has_fresh_sync()) return true;
  // The ordinary deadline can expire between kick's service/begin and the
  // pending observation. Give the now-closed attempt its already requested
  // manual retry before issuing a terminal result. No DNS generation reopens.
  sense_ntp_begin();
  if (sense_ntp_attempt_pending()) return false;
  sense_ntp_service();  // also drain an immediate retry reply before deciding
  if (sense_time_has_fresh_sync()) return true;

  LOG_ERROR("[OTA_CLOCK] deferred reason=fresh_sync_unavailable before_manifest=1");
  diag_record_error_persistent("ota_orch", -1, "clock_unconfirmed_defer before_manifest");
  // Cancellation emits the terminal result immediately. Publish its reason
  // first; this manual failure does not promise a newly scheduled short retry.
  ota_set_last_result("clock_unconfirmed");
  ota_peer_cancel("clock_unconfirmed");
  g_peer_episode_finished = true;
  g_ota_check_done = true;
  g_ota_check_requested = false;
  OtaIntent::clearForceAndCheck();
  manual_ota_override_clear("clock_unconfirmed");
  if (g_boot_ota_pending) boot_ota_finish("clock_unconfirmed");
  // The original persisted schedule/debt remains pending for a later boot.
  return false;
}

static void maybeRunOtaCheck(const char* reason, bool skip_boot_delay) {
  // Do not consume debt/requests or begin a new OTA before local VALID.
  if (!nvs_capacity_image_valid()) return;
  if (g_peer_episode_finished) { g_ota_check_done = true; return; }
  if (g_ota_check_done || g_ota_apply_in_progress) {
    return;
  }
  const bool automatic_request = g_boot_ota_pending;
  if (!ota_peer_ready()) {
    // Peer service may finish an automatic request after fresh clock proof.
    // That cancellation must not become an unrelated generic/manual retry.
    if (!automatic_request && !g_boot_ota_pending && !g_ota_check_done) g_ota_check_requested = true;
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
  if (!ota_clock_ready_before_work()) return;
  // Time-cache persistence may block; refresh the existing proof/deadline
  // before transaction entry without granting a new readiness lease.
  if (!ota_peer_ready()) return;
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

  if (g_self_retry_execution &&
      !self_retry_work_ms(g_self_retry,g_coord_pending,g_coord_completion_target,
                          self_retry_now(),sense_time_has_fresh_sync(),2400000UL)) {
    g_ota_check_done=true;return;
  }
#if HALO_DURABLE_OTA_POLICY
  const bool retained_legacy=sense_policy::unresolved_legacy();
#endif
  if (!coord_credit_prepare_work(reason)) return;
  // Admission persistence can block; recheck the original readiness deadline.
  if (!ota_peer_ready()) return;
  g_peer_gate.entered = true;
  OtaPeerTransactionCleanup peer_cleanup;
  if (g_peer_continue_work) {
    if (!ota_peer_continuation()) { g_ota_check_done = true; set_lcd_ota_due_nvs(true); return; }
  } else {
    const uint32_t work_ms = g_self_retry_execution
        ? self_retry_work_ms(g_self_retry,g_coord_pending,g_coord_completion_target,
                             self_retry_now(),sense_time_has_fresh_sync(),2400000UL)
        : 2400000UL;
    if (!work_ms) { g_ota_check_done=true; return; }
    g_lcd_work_budget = {millis(), work_ms};
    g_lcd_work_budget_live = true;
    strlcpy(g_lcd_work_schedule, g_coord_pending, sizeof(g_lcd_work_schedule));
  }
  g_lcd_work_peer_boot = g_peer_gate.peer_boot;
#if HALO_DURABLE_OTA_POLICY
  if(!sense_policy::enter(reason,retained_legacy)) {
    const bool manual = halo_ota_manual_override_active();
    ota_set_last_result(sense_policy::refusal_result(manual));
    // Refusal consumes this user request, not its campaign credit. A retained
    // force/check latch must not silently recreate it during pre-sleep.
    if (manual) {
      g_ota_check_requested = false;
      OtaIntent::clearForceAndCheck();
      manual_ota_override_clear("policy_refused");
    }
    g_ota_check_done=true;return;
  }
#endif
  sense_policy::WorkCleanup policy_cleanup;

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
  ota_heap::reset();ota_heap::sample(ota_heap::BeforeDma);
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
  ota_heap::sample(ota_heap::AfterDma);
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
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK");
  };

  OtaUrlConfig* cfg_mut = ota_get_config_mutable();
  resolveOtaManifestUrl(cfg_mut);
  const OtaUrlConfig* cfg = ota_get_config();

  char manifest_url[512];
#if HALO_DURABLE_OTA_POLICY
  const auto* fixed_policy=sense_policy::current();
  if(fixed_policy&&fixed_policy->phase!=durable_ota::Phase::DISCOVERY&&durable_ota::target_valid(fixed_policy->target))
    snprintf(manifest_url,sizeof(manifest_url),"%s/%s/manifest_%s.json",cfg->base_dir,cfg->channel,fixed_policy->target.version);
  else
#endif
  snprintf(manifest_url, sizeof(manifest_url), "%s?v=%s-boot%lu",
           cfg->manifest_url, kFirmwareVersion, g_boot_count);

  if (containsDisallowedHost(manifest_url) || !cfg->allowed_host || strlen(cfg->manifest_url) == 0) {
    truth_get_manifest_state().setErr();
    dump_system_truth("manifest_err");
    ota_set_last_result("manifest_url_invalid");
    release_waiting_lcd_ota("manifest_url_invalid");
    // No LCD proxy runs on this path; release the LCD in case a manual OTA_LOCK
    // (prod:1877) is holding its "Updating…" screen.
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
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
  ota_heap::sample(ota_heap::AfterMqtt);

  LOG_INFO("[MANIFEST] Fetching manifest: %s", manifest_url);
  OtaManifest manifest;
#if HALO_DURABLE_OTA_POLICY
  halo_policy_diagnostic_prefetch(g_lcd_work_budget.remaining_ms());
#endif
  const uint32_t manifest_remaining_ms = g_lcd_work_budget.remaining_ms();
  ManifestOutcome manifest_outcome{};
  bool manifest_fetched=false;
  if(manifest_remaining_ms){
    manifest_fetched=g_manifest_client.fetchManifest(manifest_url,manifest,
        manifest_remaining_ms<10000?manifest_remaining_ms:10000);
    manifest_outcome=g_manifest_client.outcome();
  } else manifest_outcome.stage=ManifestStage::DeadlineBefore;
  const uint32_t manifest_left_ms=g_lcd_work_budget.remaining_ms();
  if(!manifest_left_ms){
    manifest_outcome.deadline_after=manifest_remaining_ms?1:0;
    if(manifest_fetched)manifest_outcome.stage=ManifestStage::DeadlineAfter;
  }
  if (!manifest_fetched || !manifest_left_ms) {
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
    sense_diag_manifest_failure(manifest_outcome,manifest_left_ms);
#endif
    truth_get_manifest_state().setErr();
    dump_system_truth("manifest_err");
    ota_set_last_result("manifest_fetch_fail");
    release_waiting_lcd_ota("manifest_fetch_fail");
    // No LCD proxy runs on this path; release the LCD in case a manual OTA_LOCK
    // (prod:1877) is holding its "Updating…" screen.
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    mqtt_set_allowed(true);
    mqtt_force_connect();
    clear_intent_once();
    return;
  }

  if(!sense_policy::manifest_matches(manifest)) {
    ota_set_last_result("policy_target_mismatch");g_ota_check_in_progress=false;g_dma_reserve_suppressed=false;return;
  }
  ota_heap::sample(ota_heap::AfterSenseManifest);
  truth_get_manifest_state().setOk(manifest.version);
  dump_system_truth("manifest_ok");
  if (g_self_retry_execution &&
      (strcmp(manifest.version,g_self_retry.version) ||
       strcasecmp(manifest.sha256,g_self_retry.sha256) || manifest.size!=g_self_retry.expected_bytes)) {
    LOG_WARN("[SELF_RETRY] target_changed; original recovery opportunity consumed");
    ota_set_last_result("retry_target_changed");
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK",true);mqtt_set_allowed(true);mqtt_force_connect();
    clear_intent_once();g_ota_check_in_progress=false;g_dma_reserve_suppressed=false;return;
  }

  if (manifest.board_specified && strcasecmp(manifest.board, HALO_BOARD_NAME) != 0) {
    LOG_ERROR("[OTA] board_mismatch manifest=%s device=%s",
              manifest.board, HALO_BOARD_NAME);
    ota_set_last_result("board_mismatch");
    release_waiting_lcd_ota("board_mismatch");
    // No LCD proxy runs on this path; release the LCD in case a manual OTA_LOCK
    // (prod:1877) is holding its "Updating…" screen.
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
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
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
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
#if HALO_DURABLE_OTA_POLICY
    if(lcd_inline_ok&&!halo_policy_resolve_pair())lcd_inline_ok=false;
#endif
    if (lcd_inline_ok && g_lcd_work_budget.remaining_ms()) {
      const CoordCompletion resolution = ota_peer_schedule_complete();
      if (resolution == CoordCompletion::ResolvedUncredited)
        LOG_INFO("[OTA_CREDIT] repair_resolved nightly_credit=0");
      // Even deferred persistence must release the repaired peer below; its
      // original pending/target remain the retry obligation for a later boot.
    }
    OtaIntent::markNoUpdateNeeded();
    release_waiting_lcd_ota("up_to_date");
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    if (!g_lcd_ota_task_running) {
      mqtt_set_allowed(true);
      mqtt_force_connect();
      // The inline proxy above took/held the LCD lock; release it now so a
      // manual OTA_LOCK (prod:1877) doesn't strand the LCD on "Updating…".
      sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
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
      sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
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
      sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
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
        sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
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
      sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
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
    // already released. Manifest fetch destroyed its request-local TLS client;
    // there is no persistent connection to release here.
    delay(100);  // Let memory coalesce before the LCD TLS download
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS && !HALO_DURABLE_OTA_POLICY
    sense_diag_begin(manifest,g_lcd_work_budget.remaining_ms());
#endif

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
      ota_heap::sample(ota_heap::AfterLcdManifest);
      LOG_INFO("[OTA_ORCH] lcd proxy result=manifest_fetch_fail (will defer to lcd_ota_due)");
      // Breadcrumb: query succeeded; record the LCD fw it reported.
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_query_ok lcd_fw=%s t=%lu", lcd_fw, (unsigned long)millis());
      diag_record_error_persistent("ota_orch", 0, crumb);
    }
#if HALO_DURABLE_OTA_POLICY
    else if(!halo_policy_bind_pair(lcd_manifest,lcd_fw)) {
      LOG_WARN("[OTA_POLICY] pair identity/admission deferred");
    }
#endif
    else if (ManifestClient::compareVersions(lcd_manifest.version, lcd_fw) > 0) {
      ota_heap::sample(ota_heap::AfterLcdManifest);
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
      sense_diag_lcd_context(lcd_manifest,lcd_fw,g_lcd_work_budget.remaining_ms());
#endif
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
        if(!set_lcd_ota_due_nvs(true))break; // durable retry obligation before mutating LCD
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
        sense_diag_note_stage(halo_diag::Stage::ProxyBegin,g_lcd_work_budget.remaining_ms());
#endif
        ota_heap::proxy_invoked();
        const char* lcd_res = sense_lcd_ota_proxy(lcd_manifest, lcd_fw, &g_lcd_work_budget);
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
        sense_diag_note_stage(halo_diag::Stage::ProxyCleaned,g_lcd_work_budget.remaining_ms());
#endif
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
      ota_heap::sample(ota_heap::AfterLcdManifest);
      LOG_INFO("[OTA_ORCH] lcd proxy result=up_to_date (lcd=%s manifest=%s)",
               lcd_fw, lcd_manifest.version);
      // Breadcrumb: query succeeded; LCD already up-to-date.
      char crumb[96];
      snprintf(crumb, sizeof(crumb), "lcd_query_ok lcd_fw=%s t=%lu", lcd_fw, (unsigned long)millis());
      diag_record_error_persistent("ota_orch", 0, crumb);
      lcd_proxy_succeeded = g_lcd_work_budget.remaining_ms() > 0;
    }
  }

  ota_heap::sample(ota_heap::AfterLcdScope);

  // Fallback: only owe an lcd_ota_due retry on the next boot if the LCD was
  // NOT brought up-to-date here (query/manifest fail, or proxy non-success).
  // On success (or already up-to-date) clear it — the LCD is already done.
  lcd_proxy_succeeded = lcd_proxy_succeeded && sense_lcd_ota_retry_safe() && g_lcd_work_budget.remaining_ms();
  if(!set_lcd_ota_due_nvs(!lcd_proxy_succeeded))lcd_proxy_succeeded=false;
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
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
    mqtt_set_allowed(true);
    mqtt_force_connect();
    clear_intent_once();
    g_ota_check_in_progress = false;
    g_dma_reserve_suppressed = false;   // OTA over: the camera may bank its block again
    return;
  }

  if (!prepare_lcd_absolute_sleep_before_apply()) {
    LOG_ERROR("[OTA_ARM] unverified_or_no_time_budget; Sense apply deferred");
    diag_record_error_persistent("ota_orch", -1, "sense_apply_deferred arm_unverified");
    OtaIntent::recordOtaResult("lcd_arm_unverified_defer");
    ota_set_last_result("lcd_arm_unverified_defer");
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK", true);
    clear_intent_once();
    g_ota_check_in_progress = false; g_dma_reserve_suppressed = false;
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
  if(!ota_storage_bind_target(manifest.version)) {
    OtaIntent::recordOtaResult("target_storage_unverified");
    ota_set_last_result("target_storage_unverified");
    clear_intent_once();
    g_ota_check_in_progress=false;g_dma_reserve_suppressed=false;
    return; // actual transaction destructor releases the peer; pending stays
  }
  send_ota_uart_message("OTA_LOCK");
  // NVS and UART calls consume the same pair budget. Sample at the actual
  // applier boundary rather than giving it time spent in those operations.
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  sense_diag_note_stage(halo_diag::Stage::SelfBegin,g_lcd_work_budget.remaining_ms());
#endif
  const uint32_t pair_remaining_ms = lcd_verified_arm_apply_budget();
  if (!sense_lcd_ota_retry_safe() || !pair_remaining_ms) {
    OtaIntent::recordOtaResult("paired_deadline");
    ota_set_last_result("paired_deadline");
    clear_intent_once();
    g_ota_check_in_progress = false; g_dma_reserve_suppressed = false;
    return;
  }
  ota_heap::sample(ota_heap::BeforeApply);
  g_ota_apply_in_progress = true;

  SenseOtaApplier::Result res = g_ota_applier.applyToOtaPartition(
      manifest.url, manifest.sha256, manifest.size,
      pair_remaining_ms, true, manifest.version
#if HALO_DURABLE_OTA_POLICY
      , &sense_policy::begin_admission
#endif
      );
  g_ota_apply_in_progress = false;
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  if(res!=SenseOtaApplier::RESULT_SUCCESS)sense_diag_failure(res);
#endif
  // NOTE: lcd_ota_due was already set above based on whether the inline LCD
  // proxy succeeded (clear) or was skipped/failed (set as next-boot fallback).
  // Do NOT clear it unconditionally here — that would drop the fallback when
  // the LCD proxy failed but the Sense apply then succeeds and reboots.
  clear_intent_once();

  if (res != SenseOtaApplier::RESULT_SUCCESS) {
    const char* res_str = SenseOtaApplier::getResultString(res);
    LOG_ERROR("[OTA] apply failed: %s", res_str);
#if HALO_DURABLE_OTA_POLICY
    sense_policy::classify(res);sense_policy::finish();
#else
    self_retry_reserve_and_arm(manifest); // before MQTT restoration/OTA_UNLOCK
#endif
    // Re-enable MQTT after failed OTA (on success, device reboots)
    mqtt_set_allowed(true);
    mqtt_force_connect();
    OtaIntent::recordOtaResult(res_str);
    ota_set_last_result(res_str);
    sense_policy::finish(); send_ota_uart_message("OTA_UNLOCK");
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
  // The direct-USB offline diagnostic is bounded and RAM-only. Defer new
  // network/OTA work until its automatic expiry or explicit local resume.
  if (sense_backup_offline_active()) return;
  // Validation is local main-loop work even when user operations own the rest
  // of this iteration. A Wi-Fi outage cannot invalidate a healthy image.
  g_health_gate.markUartInitialized(uart_initialized && uart_is_driver_installed(LCD_UART_PORT));
  if (wifi_is_connected()) g_health_gate.markWifiConnected();
  else g_health_gate.markWifiDisconnected();
  g_health_gate.update();
  service_pending_ota_expectation_after_validation();
  g_ota_pending_verify_active = g_health_gate.getPendingVerify() && !g_health_gate.getMarkedValid();
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  sense_diag_open_boot();
#endif
#if HALO_DURABLE_OTA_POLICY
  halo_policy_service_boot();
#endif
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
  const bool prov_state_changed = prov_state != g_last_prov_state;
  if (prov_state_changed) {
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

  // Evaluate after post-AP recovery establishes its retry window. A transient
  // claim failure at AP teardown must not suppress the later success update.
  service_provision_display(prov_state, prov_state_changed);
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

  coord_credit_service_serial_install();
  if(!g_ota_apply_in_progress){
    ensure_timezone_pt("loop");
    coord_credit_retire_configured_timezone();
  }
  g_ota_pending_verify_active = g_health_gate.getPendingVerify() && !g_health_gate.getMarkedValid();
  if (g_coord_pending[0] && g_coord_completion_target[0] &&
#if HALO_DURABLE_OTA_POLICY
      sense_policy::postboot_completion_ready() &&
#endif
      strcmp(g_coord_completion_target, kFirmwareVersion) == 0 && !get_lcd_ota_due_nvs()) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_VALID) {
      char completed_request[sizeof(g_coord_pending)] = {0};
      strlcpy(completed_request, g_coord_pending, sizeof(completed_request));
      const CoordCompletion resolution = ota_peer_schedule_complete();
      if (resolution == CoordCompletion::Credited || resolution == CoordCompletion::ResolvedUncredited) {
        // Optional retained evidence after the done_ids commit. The existing
        // JSON ownership guard may suppress delivery; completion never waits.
        char detail[112];
        snprintf(detail, sizeof(detail), "rid=%.63s fw=%.31s", completed_request, kFirmwareVersion);
        uart_send_sense_diag_persist("ota", resolution == CoordCompletion::Credited ?
                                    "verified_postboot_complete" : "verified_repair_uncredited",
                                    resolution == CoordCompletion::Credited ? "done_ids" : "resolved", 0, detail);
        g_coord_completion_target[0] = 0;
      }
      if (!g_coord_pending[0] && g_boot_ota_pending) {
        g_ota_check_done = true; g_peer_episode_finished = true;
        const char* result = resolution == CoordCompletion::Credited ?
            "verified_postboot_complete" : "verified_repair_uncredited";
        ota_peer_cancel(result);
        boot_ota_finish(result);
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
  const uint32_t retry=self_retry_selected_delta();
  return retry && (!g_sleep_timer_delta_s || retry<g_sleep_timer_delta_s)
      ? retry : g_sleep_timer_delta_s;
}

void halo_prod_setup() {
  {
    Preferences p;
    if (p.begin("ota_coord", false)) {
      coord_credit_load(p);
      p.end();
      ota_storage_init_generation();
    } else {
      g_ota_storage_uncertain=true;g_coord_sense_boot_id=0;
    }
  }
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
  halo_sleep_witness_boot();
#endif
  sense_idle_network::boot();
  // A reset after the atomic history commit but before key removal must not
  // attach a later manual request to the already completed pending ID.
  if (ota_peer_schedule_completed(g_coord_pending)) g_coord_pending[0] = 0;
  if(g_boot_ota_pending && !strcmp(g_boot_ota_reason,"nightly") &&
     g_coord_credit_loaded && g_coord_credit.schedule.bound &&
     !ota_peer_schedule_completed(g_coord_credit.schedule.id)){
    g_calendar_timer_origin=g_coord_credit.schedule;
    coord_credit_reserve_timer();
  }
  if (g_boot_ota_pending && strcmp(g_boot_ota_reason, "nightly") == 0 && g_coord_schedule[0] && !g_coord_pending[0]) {
    if (ota_peer_schedule_completed(g_coord_schedule) && !coord_credit_base().deferred.id[0]) {
      g_ota_check_done = true; g_peer_episode_finished = true;
      boot_ota_finish("schedule_already_completed");
    }
    // Preserve the authoritative origin before any late arm can replace it.
    // This is not admission; a pre-VALID boot leaves rollback-readable bytes.
    coord_credit_reserve_timer();
    // Configured TZ/fresh clock and due admission remain actual-entry checks.
  }
  // RTC loss closes only this short retry; durable pending repair is unchanged.
  g_self_retry_boot=self_retry_bound() && g_self_retry.phase==SelfOtaRetryPhase::ARMED &&
      strcmp(kFirmwareVersion,g_self_retry.version)!=0;
  if (self_retry_shape(g_self_retry) && !g_self_retry_boot &&
      (!self_retry_bound() || !strcmp(kFirmwareVersion,g_self_retry.version)))
    g_self_retry.phase=SelfOtaRetryPhase::CLOSED;
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
  g_sleep_timer_selected_hook = self_retry_note_selected_timer;
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  sense_diag_open_boot();
#endif

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

  g_health_gate.markUartInitialized(uart_initialized && uart_is_driver_installed(LCD_UART_PORT));
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

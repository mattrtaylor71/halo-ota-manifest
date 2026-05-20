/*
 * HALO LCD PROD
 *
 * Wrapper around LCD_Minimal with OTA framework.
 * - Functional LCD UI remains intact (imported from LCD_Minimal).
 * - LCD OTA is now proxied via UART by the Sense board.
 *   All WiFi/TLS/SNTP/HTTPS/AES code has been removed from this file.
 */

#define HALO_BOARD_LCD 1
#define HALO_LCD_PROD_WRAPPER 1
#define SHIP_MENU_UI 1
#include "../../../LCD_Minimal/LCD_Minimal.ino"

#include <Arduino.h>

#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <time.h>
#include <stdlib.h>
#include <Preferences.h>

#include "../shared/BuildFlags.h"
#include "../shared/BuildInfo.h"
#include "../shared/Log.h"
#include "../shared/LcdWifiCreds.h"

static bool ota_check_in_progress = false;
static unsigned long ota_check_last_attempt_ms = 0;
static const unsigned long OTA_RETRY_INTERVAL_MS = 15000;
static unsigned long next_ota_check_ms = 0;
static bool ota_paused_processing_anim = false;
static char ota_prev_processing_op[24] = "none";
static bool tz_initialized = false;
static Preferences ota_guard_prefs;
static const char* OTA_GUARD_NAMESPACE = "lcd_ota_guard";
static const char* OTA_GUARD_COUNT_KEY = "time_fail";
static const char* OTA_GUARD_DISABLE_KEY = "disabled_until";
static Preferences ota_fail_prefs;
static const char* OTA_FAIL_NAMESPACE = "lcd_ota_fail";
static const char* OTA_FAIL_COUNT_KEY = "fail_count";
static const char* OTA_FAIL_LAST_EPOCH_KEY = "last_epoch";
static uint32_t ota_fail_count = 0;
static uint32_t ota_last_fail_epoch = 0;
static uint8_t ota_attempts_this_wake_window = 0;
static const uint8_t OTA_WAKE_WINDOW_MAX_ATTEMPTS = 3;

// From LCD_Minimal: maintenance and wake cause for maintenance-only gate
extern bool lcd_maintenance_active(void);
extern unsigned long lcd_maintenance_remaining_ms(void);
extern bool lcd_schedule_maintenance_retry(unsigned long delay_ms, const char* reason);
extern const char* lcd_wake_cause_label(void);
static bool lcd_is_maintenance_allowed_now(void) {
  if (lcd_manual_ota_override_active() || lcd_ota_request_allows_outside_maintenance()) {
    return true;
  }
  return lcd_maintenance_active() || lcd_schedule_window_active();
}

static bool lcd_maintenance_failure_retryable(const char* code) {
  if (!code || !code[0]) {
    return true;
  }
  if (strcmp(code, "user_active") == 0 ||
      strcmp(code, "reboot_not_allowed") == 0 ||
      strcmp(code, "maintenance_only") == 0 ||
      strcmp(code, "manifest_url_invalid") == 0 ||
      strcmp(code, "board_mismatch") == 0 ||
      strcmp(code, "bin_url_disallowed") == 0 ||
      strcmp(code, "guard_disabled") == 0 ||
      strcmp(code, "wake_window_retry_exhausted") == 0) {
    return false;
  }
  return true;
}

static void ensure_timezone_pt(const char* reason) {
  if (tz_initialized) {
    return;
  }
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
  tzset();
  tz_initialized = true;
  Serial.printf("[LCD_TZ] set=PT reason=%s\n", reason ? reason : "unknown");
}

static bool ota_guard_is_disabled() {
  if (!ota_guard_prefs.begin(OTA_GUARD_NAMESPACE, false)) {
    return false;
  }
  uint32_t disabled_until = ota_guard_prefs.getUInt(OTA_GUARD_DISABLE_KEY, 0);
  if (disabled_until == 0) {
    ota_guard_prefs.end();
    return false;
  }
  time_t now = time(nullptr);
  uint32_t now_u = now > 0 ? static_cast<uint32_t>(now) : 0;
  if (now_u >= disabled_until) {
    ota_guard_prefs.putUInt(OTA_GUARD_DISABLE_KEY, 0);
    ota_guard_prefs.end();
    return false;
  }
  ota_guard_prefs.end();
  return true;
}

static void ota_guard_reset_time_invalid() {
  if (!ota_guard_prefs.begin(OTA_GUARD_NAMESPACE, false)) {
    return;
  }
  ota_guard_prefs.putUInt(OTA_GUARD_COUNT_KEY, 0);
  ota_guard_prefs.end();
}

static void ota_guard_record_time_invalid() {
  if (!ota_guard_prefs.begin(OTA_GUARD_NAMESPACE, false)) {
    return;
  }
  uint32_t count = ota_guard_prefs.getUInt(OTA_GUARD_COUNT_KEY, 0);
  count++;
  ota_guard_prefs.putUInt(OTA_GUARD_COUNT_KEY, count);
  if (count >= 3) {
    time_t now = time(nullptr);
    uint32_t now_u = now > 0 ? static_cast<uint32_t>(now) : 0;
    uint32_t disabled_until = now_u + 600;
    ota_guard_prefs.putUInt(OTA_GUARD_DISABLE_KEY, disabled_until);
    ota_guard_prefs.putUInt(OTA_GUARD_COUNT_KEY, 0);
    Serial.printf("[LCD_OTA] guard tripped -> ota_disabled_until=%lu\n",
                  static_cast<unsigned long>(disabled_until));
  }
  ota_guard_prefs.end();
}

static void ota_fail_load() {
  if (!ota_fail_prefs.begin(OTA_FAIL_NAMESPACE, false)) {
    return;
  }
  ota_fail_count = ota_fail_prefs.getUInt(OTA_FAIL_COUNT_KEY, 0);
  ota_last_fail_epoch = ota_fail_prefs.getUInt(OTA_FAIL_LAST_EPOCH_KEY, 0);
  ota_fail_prefs.end();
}

static void ota_fail_save() {
  if (!ota_fail_prefs.begin(OTA_FAIL_NAMESPACE, false)) {
    return;
  }
  ota_fail_prefs.putUInt(OTA_FAIL_COUNT_KEY, ota_fail_count);
  ota_fail_prefs.putUInt(OTA_FAIL_LAST_EPOCH_KEY, ota_last_fail_epoch);
  ota_fail_prefs.end();
}

static void ota_fail_record(const char* reason) {
  if (ota_fail_count < 0xFFFFFFFFUL) {
    ota_fail_count++;
  }
  time_t now = time(nullptr);
  ota_last_fail_epoch = (now > 0) ? (uint32_t)now : 0;
  ota_fail_save();
  Serial.printf("[LCD_OTA] fail_record reason=%s count=%lu last_epoch=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)ota_fail_count,
                (unsigned long)ota_last_fail_epoch);
}

static void ota_fail_reset(const char* reason) {
  if (ota_fail_count == 0 && ota_last_fail_epoch == 0) {
    return;
  }
  ota_fail_count = 0;
  ota_last_fail_epoch = 0;
  ota_fail_save();
  Serial.printf("[LCD_OTA] fail_reset reason=%s\n", reason ? reason : "unknown");
}

static void log_lcd_heap_stats(const char* tag) {
  const uint32_t internal_free =
      (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t internal_largest =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t psram_free =
      (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const uint32_t psram_largest =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Serial.printf("[LCD_OTA] heap %s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u\n",
                tag ? tag : "unknown",
                (unsigned)internal_free, (unsigned)internal_largest,
                (unsigned)psram_free, (unsigned)psram_largest);
}

static bool lcd_ota_prepare_memory(const char* reason) {
  ota_paused_processing_anim = false;
  if (is_glowing_animation) {
    ota_paused_processing_anim = true;
    strncpy(ota_prev_processing_op, processing_op, sizeof(ota_prev_processing_op) - 1);
    ota_prev_processing_op[sizeof(ota_prev_processing_op) - 1] = '\0';
    stop_glowing_animation();
    Serial.printf("[LCD_OTA] processing_paused=1 reason=%s\n", reason ? reason : "unknown");
  }
  // Flush pending UI work so LVGL can release transient allocations.
  lv_timer_handler();
  return ota_paused_processing_anim;
}

static void lcd_ota_restore_ui_after_attempt(const char* reason) {
  if (ota_paused_processing_anim) {
    start_glowing_animation(ota_prev_processing_op);
    Serial.printf("[LCD_OTA] processing_paused=0 reason=%s\n", reason ? reason : "unknown");
  }
  ota_paused_processing_anim = false;
}

static void runLcdOtaCheckOnce();

void halo_lcd_prod_on_wifi_creds(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) {
    Serial.println("[LCD_WIFI] reject empty ssid");
    return;
  }
  bool ok = LcdWifiCreds::saveCreds(ssid, pass ? pass : "");
  Serial.printf("[LCD_WIFI] saved creds from Sense result=%s\n", ok ? "OK" : "FAIL");
}

void halo_lcd_prod_on_wifi_on(uint32_t timeout_ms) {
  // LCD OTA is now proxied via UART by the Sense board.
  // WiFi is no longer managed on the LCD board.
  Serial.printf("[LCD_WIFI] wifi_on ignored (OTA proxied via Sense UART) timeout_ms=%u\n",
                (unsigned)timeout_ms);
}

bool halo_lcd_prod_should_stay_awake() {
  return ota_check_in_progress || ota_check_requested;
}

void halo_lcd_prod_run_ota_check_once() {
  runLcdOtaCheckOnce();
}

bool halo_lcd_prod_wifi_ok() {
  // WiFi is no longer managed on the LCD board
  return false;
}

bool halo_lcd_prod_wifi_connecting() {
  // WiFi is no longer managed on the LCD board
  return false;
}

bool halo_lcd_prod_wifi_failed() {
  // WiFi is no longer managed on the LCD board
  return false;
}

int halo_lcd_prod_wifi_status() {
  // WiFi is no longer managed on the LCD board
  return 0;
}

unsigned long halo_lcd_prod_next_wifi_retry_ms() {
  return 0;
}

bool halo_lcd_prod_wifi_retry_ready() {
  return true;
}

unsigned long halo_lcd_prod_wifi_budget_ms(unsigned long safety_margin_ms) {
  return 0;
}

void halo_lcd_prod_wifi_schedule_backoff(const char* reason) {
  // WiFi is no longer managed on the LCD board
  Serial.printf("[LCD_WIFI] backoff ignored (OTA proxied via Sense UART) reason=%s\n",
                reason ? reason : "unknown");
}

void halo_lcd_prod_wifi_abort(const char* reason) {
  // WiFi is no longer managed on the LCD board
  Serial.printf("[LCD_WIFI] abort ignored (OTA proxied via Sense UART) reason=%s\n",
                reason ? reason : "unknown");
}

// LCD OTA is now proxied via UART by the Sense board.
// This function reports that the LCD board no longer performs direct
// WiFi/TLS OTA checks.  The Sense board downloads the firmware and
// streams it to the LCD over UART.
static void runLcdOtaCheckOnce() {
  Serial.println("[LCD_OTA] OTA is now proxied via UART by the Sense board");
  lcd_ota_request_finish("fail", "uart_proxy", kFirmwareVersion, "uart_proxy");
  ota_check_in_progress = false;
}

void halo_lcd_prod_loop() {
  if (ota_locked && (millis() - ota_lock_at_ms) >= OTA_LOCK_TIMEOUT_MS) {
    ota_locked = false;
    Serial.println("[LCD_OTA] auto-unlock after timeout");
  }

  if (lcd_maintenance_active()) {
    unsigned long remaining_ms = lcd_maintenance_remaining_ms();
    if (remaining_ms == 0 && next_ota_check_ms > 0) {
      Serial.println("[LCD_OTA] maintenance window expired before retry");
      next_ota_check_ms = 0;
      if (!ota_check_in_progress && !ota_check_requested) {
        lcd_finish_maintenance("failed");
      }
    } else if (next_ota_check_ms > 0 &&
               millis() >= next_ota_check_ms &&
               !ota_check_requested &&
               !ota_check_in_progress &&
               !ota_locked) {
      next_ota_check_ms = 0;
      ota_check_requested = true;
      Serial.printf("[LCD_OTA] maintenance retry due remaining_ms=%lu\n",
                    (unsigned long)remaining_ms);
    }
  } else if (next_ota_check_ms > 0) {
    Serial.println("[LCD_OTA] clearing orphaned retry timer");
    next_ota_check_ms = 0;
  }

  if (ota_check_requested && !ota_locked) {
    ota_check_requested = false;
    runLcdOtaCheckOnce();
  }
}

void halo_lcd_prod_setup() {
  ensure_timezone_pt("boot");
  Serial.printf("[BUILD_FLAGS] MAINT_ONLY=%d OTA_EN=%d SHIP_TEST=%d\n",
                HALO_OTA_POLICY_MAINTENANCE_ONLY ? 1 : 0, OTA_ENABLED ? 1 : 0, HALO_SHIP_TEST_MODE ? 1 : 0);
  Serial.printf("[LCD_MAINT] wake_cause=%s maint_flag=%d allowed_now=%d\n",
                lcd_wake_cause_label(), lcd_maintenance_active() ? 1 : 0, lcd_is_maintenance_allowed_now() ? 1 : 0);
#if HALO_OTA_POLICY_MAINTENANCE_ONLY
  if (!lcd_is_maintenance_allowed_now()) {
    Serial.println("[LCD_OTA] maintenance_only skip all ota prep");
    return;
  }
#endif
  Serial.println("[LCD_OTA] OTA is now proxied via UART by the Sense board");
  ota_fail_load();
  Serial.printf("[LCD_OTA] fail_state count=%lu last_epoch=%lu\n",
                (unsigned long)ota_fail_count,
                (unsigned long)ota_last_fail_epoch);
  log_lcd_heap_stats("boot_after_ui");
}

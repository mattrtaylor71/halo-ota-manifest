/*
 * HALO LCD PROD
 *
 * Wrapper around LCD_Minimal with OTA framework.
 * - Functional LCD UI remains intact (imported from LCD_Minimal).
 * - Adds LCD OTA manifest/apply pipeline (manual trigger only).
 */

#define HALO_BOARD_LCD 1
#define HALO_LCD_PROD_WRAPPER 1
#define SHIP_MENU_UI 1
#include "../../../LCD_Minimal/LCD_Minimal.ino"

#include <Arduino.h>

// Forward declare so OtaUrlConfig is in scope for all declarations in this file
struct OtaUrlConfig;
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <strings.h>
#include <time.h>
#include <stdlib.h>
#include <Preferences.h>

#include "../shared/BuildFlags.h"
#include "../shared/BuildInfo.h"
#include "../shared/Log.h"
#include "../shared/ManifestClient.h"
#include "../shared/SenseOtaApplier.h"
#include "../shared/LcdWifiCreds.h"
#include "../shared/WifiManager.h"
#include "../shared/WifiGuard.h"

// Wi-Fi defaults (disabled by default)
#ifndef LCD_WIFI_ENABLED_DEFAULT
#define LCD_WIFI_ENABLED_DEFAULT false
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef WIFI_DEFAULT_TIMEOUT_SEC
#define WIFI_DEFAULT_TIMEOUT_SEC 60
#endif

// OTA configuration (LCD manifest path includes /lcd/)
#ifndef OTA_DEFAULT_ENV
#define OTA_DEFAULT_ENV "dev"
#endif
#define OTA_DEV_BUCKET  "halo-ota-dev"
#define OTA_DEV_REGION  "us-east-1"
#define OTA_DEV_PREFIX  "halo/ota"
#define OTA_PROD_BUCKET "halo-ota-prod"
#define OTA_PROD_REGION "us-east-1"
#define OTA_PROD_PREFIX "halo/ota"

static WifiManager wifi_manager;
static ManifestClient manifest_client;
static SenseOtaApplier ota_applier;

static bool wifi_enabled = LCD_WIFI_ENABLED_DEFAULT;
static unsigned long wifi_enable_time_ms = 0;
static unsigned long wifi_timeout_sec = WIFI_DEFAULT_TIMEOUT_SEC;
static unsigned long wifi_connected_at_ms = 0;
static bool wifi_dns_set = false;
static bool wifi_connect_defer_logged = false;
static bool wifi_connecting_in_progress = false;
static unsigned long wifi_connecting_start_ms = 0;
static bool wifi_heap_logged = false;
static unsigned long wifi_next_retry_ms = 0;
static uint8_t wifi_fail_count = 0;
static const unsigned long WIFI_BACKOFF_STEPS_MS[] = {
  5UL * 60UL * 1000UL,
  15UL * 60UL * 1000UL,
  30UL * 60UL * 1000UL
};

static bool ota_check_in_progress = false;
static unsigned long ota_check_last_attempt_ms = 0;
static const unsigned long OTA_RETRY_INTERVAL_MS = 15000;
static unsigned long next_ota_check_ms = 0;
static bool ota_paused_processing_anim = false;
static char ota_prev_processing_op[24] = "none";
static bool ota_deferred = false;
static bool sntp_started = false;
static bool tz_initialized = false;
static bool ota_dns_set = false;
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

struct OtaUrlConfig {
  char base_dir[256];
  char manifest_url[512];
  char channel[32];
  char env[8];
  char host[96];
  bool channel_enabled;
  bool allowed_host;
};

static OtaUrlConfig g_ota_config;

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

static bool containsDisallowedHost(const char* url) {
  if (!url || strlen(url) == 0) return false;
  const char* p = url;
  while (*p) {
    if ((p[0]=='g'||p[0]=='G')&&(p[1]=='i'||p[1]=='I')&&(p[2]=='t'||p[2]=='T')&&(p[3]=='h'||p[3]=='H')&&(p[4]=='u'||p[4]=='U')&&(p[5]=='b'||p[5]=='B')&&p[6]=='.'&&(p[7]=='i'||p[7]=='I')&&(p[8]=='o'||p[8]=='O')&&(p[9]=='\0'||p[9]=='/'||p[9]==':'||p[9]=='?'||p[9]=='&'||p[9]=='#')) return true;
    if ((p[0]=='g'||p[0]=='G')&&(p[1]=='i'||p[1]=='I')&&(p[2]=='t'||p[2]=='T')&&(p[3]=='h'||p[3]=='H')&&(p[4]=='u'||p[4]=='U')&&(p[5]=='b'||p[5]=='B')&&(p[6]=='u'||p[6]=='U')&&(p[7]=='s'||p[7]=='S')&&(p[8]=='e'||p[8]=='E')&&(p[9]=='r'||p[9]=='R')&&(p[10]=='c'||p[10]=='C')&&(p[11]=='o'||p[11]=='O')&&(p[12]=='n'||p[12]=='N')&&(p[13]=='t'||p[13]=='T')&&(p[14]=='e'||p[14]=='E')&&(p[15]=='n'||p[15]=='N')&&(p[16]=='t'||p[16]=='T')&&p[17]=='.'&&(p[18]=='c'||p[18]=='C')&&(p[19]=='o'||p[19]=='O')&&(p[20]=='m'||p[20]=='M')&&(p[21]=='\0'||p[21]=='/'||p[21]==':'||p[21]=='?'||p[21]=='&'||p[21]=='#')) return true;
    p++;
  }
  return false;
}

static void resolveLcdOtaManifestUrl(OtaUrlConfig* config) {
  memset(config, 0, sizeof(OtaUrlConfig));
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
  snprintf(config->manifest_url, sizeof(config->manifest_url),
           "%s/%s/lcd/manifest_latest.json", config->base_dir, ch);
  config->manifest_url[sizeof(config->manifest_url) - 1] = '\0';
  strncpy(config->channel, ch, sizeof(config->channel) - 1);
  config->channel[sizeof(config->channel) - 1] = '\0';
  config->channel_enabled = true;
  config->allowed_host = isAllowedOtaHost(config->manifest_url);
  extractHostFromUrl(config->manifest_url, config->host, sizeof(config->host));

  if (containsDisallowedHost(config->manifest_url) || !config->allowed_host) {
    memset(config->manifest_url, 0, sizeof(config->manifest_url));
    config->allowed_host = false;
  }
}

static bool wifi_ok() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  IPAddress ip = WiFi.localIP();
  return !(ip == IPAddress(0, 0, 0, 0));
}

static void schedule_wifi_backoff(const char* reason) {
  if (wifi_next_retry_ms > 0 && millis() < wifi_next_retry_ms) {
    return;
  }
  if (wifi_fail_count < (sizeof(WIFI_BACKOFF_STEPS_MS) / sizeof(WIFI_BACKOFF_STEPS_MS[0]))) {
    wifi_fail_count++;
  }
  size_t idx = (wifi_fail_count == 0) ? 0 : (wifi_fail_count - 1);
  if (idx >= (sizeof(WIFI_BACKOFF_STEPS_MS) / sizeof(WIFI_BACKOFF_STEPS_MS[0]))) {
    idx = (sizeof(WIFI_BACKOFF_STEPS_MS) / sizeof(WIFI_BACKOFF_STEPS_MS[0])) - 1;
  }
  unsigned long backoff_ms = WIFI_BACKOFF_STEPS_MS[idx];
  if (backoff_ms == 0) {
    backoff_ms = 300000;
  }
  wifi_next_retry_ms = millis() + backoff_ms;
  wifi_connecting_in_progress = false;
  wifi_enabled = false;
  wifi_enable_time_ms = 0;
  wifi_connecting_start_ms = 0;
  wifi_manager.disconnect();
  wifi_pending_clear_for_backoff();
  long next_retry_in_ms = (long)(wifi_next_retry_ms - millis());
  if (next_retry_in_ms < 0) {
    next_retry_in_ms = 0;
  }
  Serial.printf("[LCD_WIFI] BACKOFF reason=%s backoff_ms=%lu next_retry_in_ms=%ld\n",
                reason ? reason : "unknown",
                backoff_ms,
                next_retry_in_ms);
}

static void schedule_wifi_backoff_with_count(const char* reason, uint32_t fail_count_override) {
  if (wifi_next_retry_ms > 0 && millis() < wifi_next_retry_ms) {
    return;
  }
  wifi_fail_count = (fail_count_override > 255) ? 255 : (uint8_t)fail_count_override;
  size_t idx = (wifi_fail_count == 0) ? 0 : (wifi_fail_count - 1);
  if (idx >= (sizeof(WIFI_BACKOFF_STEPS_MS) / sizeof(WIFI_BACKOFF_STEPS_MS[0]))) {
    idx = (sizeof(WIFI_BACKOFF_STEPS_MS) / sizeof(WIFI_BACKOFF_STEPS_MS[0])) - 1;
  }
  unsigned long backoff_ms = WIFI_BACKOFF_STEPS_MS[idx];
  if (backoff_ms == 0) {
    backoff_ms = 300000;
  }
  wifi_next_retry_ms = millis() + backoff_ms;
  wifi_connecting_in_progress = false;
  wifi_enabled = false;
  wifi_enable_time_ms = 0;
  wifi_connecting_start_ms = 0;
  wifi_manager.disconnect();
  wifi_pending_clear_for_backoff();
  long next_retry_in_ms = (long)(wifi_next_retry_ms - millis());
  if (next_retry_in_ms < 0) {
    next_retry_in_ms = 0;
  }
  Serial.printf("[LCD_WIFI] BACKOFF reason=%s backoff_ms=%lu next_retry_in_ms=%ld\n",
                reason ? reason : "unknown",
                backoff_ms,
                next_retry_in_ms);
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

static bool tryConnectWifi() {
  if (wifi_manager.getStatus() == WifiManager::STATUS_CONNECTED) {
    return true;
  }
  if (wifi_manager.getStatus() == WifiManager::STATUS_CONNECTING) {
    return true;
  }
  char ssid_buf[33];
  char pass_buf[64];
  if (LcdWifiCreds::hasCreds()) {
    if (LcdWifiCreds::loadCreds(ssid_buf, sizeof(ssid_buf), pass_buf, sizeof(pass_buf))) {
      WifiManager::ConnectResult r = wifi_manager.connect(ssid_buf, pass_buf, 25000, 3, 3000);
      bool started = (r == WifiManager::CONNECT_RESULT_STARTED || r == WifiManager::CONNECT_RESULT_IN_PROGRESS);
      if (started) {
        wifi_connecting_in_progress = true;
        wifi_connecting_start_ms = millis();
      }
      return started;
    }
  }
  if (strlen(WIFI_SSID) > 0) {
    WifiManager::ConnectResult r = wifi_manager.connect(WIFI_SSID, WIFI_PASSWORD, 25000, 3, 3000);
    bool started = (r == WifiManager::CONNECT_RESULT_STARTED || r == WifiManager::CONNECT_RESULT_IN_PROGRESS);
    if (started) {
      wifi_connecting_in_progress = true;
      wifi_connecting_start_ms = millis();
    }
    return started;
  }
  Serial.println("[LCD_WIFI] no creds; cannot connect");
  return false;
}

static void maybeEnableWifiForOta(const char* reason) {
  if (wifi_enabled) {
    return;
  }
  if (wifi_next_retry_ms > 0 && millis() < wifi_next_retry_ms) {
    long wait_ms = (long)(wifi_next_retry_ms - millis());
    Serial.printf("[LCD_WIFI] auto_enable backoff (%ld ms) reason=%s\n",
                  wait_ms, reason ? reason : "ota");
    return;
  }
  bool have_creds = LcdWifiCreds::hasCreds() || (strlen(WIFI_SSID) > 0);
  if (!have_creds) {
    Serial.printf("[LCD_WIFI] auto_enable skipped (no creds) reason=%s\n", reason ? reason : "ota");
    return;
  }
  wifi_enabled = true;
  wifi_enable_time_ms = millis();
  wifi_timeout_sec = WIFI_DEFAULT_TIMEOUT_SEC;
  Serial.printf("[LCD_WIFI] auto_enable reason=%s\n", reason ? reason : "ota");
  tryConnectWifi();
  wifi_pending_start(WIFI_REASON_OTA);
}

static bool ensure_wifi_connected_for_ota(uint32_t timeout_ms) {
  if (wifi_ok()) {
    return true;
  }
  tryConnectWifi();
  unsigned long start = millis();
  int retry_count = 0;
  const int MAX_RETRIES = 5;
  while ((millis() - start) < timeout_ms) {
    wifi_manager.update();
    if (wifi_ok()) {
      Serial.printf("[LCD_OTA_WIFI] connected elapsed=%lums retries=%d\n",
                    millis() - start, retry_count);
      return true;
    }
    if (wifi_manager.getStatus() == WifiManager::STATUS_FAILED) {
      retry_count++;
      if (retry_count > MAX_RETRIES) {
        Serial.printf("[LCD_OTA_WIFI] giving up after %d retries\n", retry_count);
        return false;
      }
      unsigned long elapsed = millis() - start;
      Serial.printf("[LCD_OTA_WIFI] failed, retry #%d (elapsed=%lums)\n", retry_count, elapsed);
      // Reset WiFi and try again
      WiFi.disconnect(true);
      delay(500);
      WiFi.mode(WIFI_STA);
      delay(100);
      tryConnectWifi();
    }
    delay(100);
  }
  Serial.printf("[LCD_OTA_WIFI] timeout after %lums retries=%d\n",
                millis() - start, retry_count);
  return wifi_ok();
}

static void start_sntp_if_needed() {
  if (sntp_started) {
    return;
  }
  ensure_timezone_pt("sntp_start");
  IPAddress d0 = WiFi.dnsIP(0);
  IPAddress d1 = WiFi.dnsIP(1);
  Serial.printf("[LCD_OTA] sntp_init dns0=%s dns1=%s\n",
                d0.toString().c_str(),
                d1.toString().c_str());
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  Serial.println("[LCD_OTA] sntp_init started");
  sntp_started = true;
}

static void ensure_dns_for_ota() {
  if (ota_dns_set) {
    return;
  }
  if (!wifi_ok()) {
    return;
  }
  IPAddress d0 = WiFi.dnsIP(0);
  IPAddress d1 = WiFi.dnsIP(1);
  if (!(d0 == IPAddress(0, 0, 0, 0) && d1 == IPAddress(0, 0, 0, 0))) {
    Serial.printf("[LCD_OTA] dns_keep dhcp d0=%s d1=%s\n",
                  d0.toString().c_str(),
                  d1.toString().c_str());
    ota_dns_set = true;
    return;
  }
  IPAddress dns1(1, 1, 1, 1);
  IPAddress dns2(8, 8, 8, 8);
  bool dns_ok = WiFi.setDNS(dns1, dns2);
  Serial.printf("[LCD_OTA] dns_set ok=%d\n", dns_ok ? 1 : 0);
  ota_dns_set = true;
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

static bool ensure_time_synced_for_tls() {
  start_sntp_if_needed();
  const unsigned long timeout_ms = 10000;
  const unsigned long poll_ms = 250;
  unsigned long start = millis();
  int attempt = 0;
  while ((millis() - start) < timeout_ms) {
    time_t now = time(nullptr);
    attempt++;
    Serial.printf("[LCD_OTA] waiting_time_valid attempt=%d epoch=%ld\n",
                  attempt, static_cast<long>(now));
    if (now > 1700000000) {
      return true;
    }
    delay(poll_ms);
  }
  return false;
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
  if (wifi_next_retry_ms > 0 && millis() < wifi_next_retry_ms) {
    long wait_ms = (long)(wifi_next_retry_ms - millis());
    Serial.printf("[LCD_WIFI] backoff active (%ld ms) - skipping enable\n", wait_ms);
    return;
  }
  uint32_t timeout_sec = timeout_ms ? (timeout_ms / 1000U) : WIFI_DEFAULT_TIMEOUT_SEC;
  if (timeout_sec == 0) {
    timeout_sec = WIFI_DEFAULT_TIMEOUT_SEC;
  }
  wifi_timeout_sec = timeout_sec;
  if (!wifi_enabled) {
    wifi_enabled = true;
    wifi_enable_time_ms = millis();
    Serial.printf("[LCD_WIFI] enable from Sense timeout_s=%lu\n", (unsigned long)wifi_timeout_sec);
    wifi_connecting_in_progress = true;
    wifi_connecting_start_ms = millis();
    tryConnectWifi();
  } else {
    wifi_enable_time_ms = millis();
    Serial.printf("[LCD_WIFI] extend from Sense timeout_s=%lu\n", (unsigned long)wifi_timeout_sec);
    if (!wifi_ok()) {
      wifi_connecting_in_progress = true;
      wifi_connecting_start_ms = millis();
    } else {
      wifi_connecting_in_progress = false;
      wifi_connecting_start_ms = 0;
    }
  }
}

bool halo_lcd_prod_should_stay_awake() {
  return ota_check_in_progress || ota_check_requested;
}

void halo_lcd_prod_run_ota_check_once() {
  runLcdOtaCheckOnce();
}

bool halo_lcd_prod_wifi_ok() {
  return wifi_ok();
}

bool halo_lcd_prod_wifi_connecting() {
  return wifi_connecting_in_progress;
}

bool halo_lcd_prod_wifi_failed() {
  return wifi_manager.getStatus() == WifiManager::STATUS_FAILED;
}

int halo_lcd_prod_wifi_status() {
  return (int)wifi_manager.getStatus();
}

unsigned long halo_lcd_prod_next_wifi_retry_ms() {
  return wifi_next_retry_ms;
}

bool halo_lcd_prod_wifi_retry_ready() {
  return (wifi_next_retry_ms == 0 || millis() >= wifi_next_retry_ms);
}

unsigned long halo_lcd_prod_wifi_budget_ms(unsigned long safety_margin_ms) {
  return wifi_manager.getBudgetMs(safety_margin_ms);
}

void halo_lcd_prod_wifi_schedule_backoff(const char* reason) {
  schedule_wifi_backoff(reason);
}

void halo_lcd_prod_wifi_abort(const char* reason) {
  Serial.printf("[LCD_WIFI] abort_wifi reason=%s\n", reason ? reason : "unknown");
  wifi_manager.disconnect();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  wifi_enabled = false;
  wifi_enable_time_ms = 0;
  wifi_connecting_in_progress = false;
  wifi_connecting_start_ms = 0;
}

static void runLcdOtaCheckOnce() {
  if (!lcd_is_maintenance_allowed_now()) {
    Serial.println("[LCD_OTA] maintenance_only skip all ota prep");
    lcd_ota_request_finish("fail", "maintenance_only", kFirmwareVersion, "maintenance_only");
    lcd_finish_maintenance("failed");
    return;
  }
  lcd_manual_ota_override_clear("ota_check_begin");
  bool wake_window = g_ship_ota_wake_window || g_lcd_maintenance_wake_window;
  bool guard_disabled = ota_guard_is_disabled();
  bool rate_limited = (next_ota_check_ms > 0 && millis() < next_ota_check_ms);
  Serial.printf("[LCD_OTA] check_enter maint_active=%d wake_window=%d ota_locked=%d guard_disabled=%d rate_limited=%d\n",
                lcd_maintenance_active() ? 1 : 0,
                wake_window ? 1 : 0,
                ota_locked ? 1 : 0,
                guard_disabled ? 1 : 0,
                rate_limited ? 1 : 0);
  auto schedule_maintenance_retry = [&](const char* failure_code) -> bool {
    if (!lcd_maintenance_active()) {
      return false;
    }
    if (!lcd_maintenance_failure_retryable(failure_code)) {
      return false;
    }
    unsigned long remaining_ms = lcd_maintenance_remaining_ms();
    if (remaining_ms <= 3000UL) {
      Serial.printf("[LCD_OTA] maintenance_retry_skipped code=%s remaining_ms=%lu\n",
                    failure_code ? failure_code : "unknown",
                    (unsigned long)remaining_ms);
      return false;
    }
    unsigned long delay_ms = OTA_RETRY_INTERVAL_MS;
    if (delay_ms >= remaining_ms) {
      delay_ms = remaining_ms > 1000UL ? (remaining_ms - 1000UL) : remaining_ms;
    }
    if (delay_ms < 1000UL) {
      Serial.printf("[LCD_OTA] maintenance_retry_skipped_short code=%s remaining_ms=%lu\n",
                    failure_code ? failure_code : "unknown",
                    (unsigned long)remaining_ms);
      return false;
    }
    next_ota_check_ms = millis() + delay_ms;
    if (!lcd_schedule_maintenance_retry(delay_ms, failure_code)) {
      next_ota_check_ms = 0;
      return false;
    }
    Serial.printf("[LCD_OTA] maintenance_retry_scheduled code=%s delay_ms=%lu remaining_ms=%lu\n",
                  failure_code ? failure_code : "unknown",
                  (unsigned long)delay_ms,
                  (unsigned long)remaining_ms);
    return true;
  };
  auto finish_maintenance = [](const char* result) {
    if (!g_lcd_maintenance_active) {
      return;
    }
    lcd_finish_maintenance(result);
  };
  auto report_result = [](const char* result, const char* detail, const char* new_version, const char* err_code) {
    lcd_ota_request_finish(result, detail, new_version, err_code);
  };
  auto finish_attempt = [&](const char* maintenance_result,
                            const char* exit_reason,
                            const char* failure_code = nullptr) {
    ota_check_in_progress = false;
    if (failure_code && failure_code[0] && schedule_maintenance_retry(failure_code)) {
      if (g_ota_mode_active) {
        lcd_exit_ota_mode("maintenance_retry_wait");
      }
      return;
    }
    next_ota_check_ms = 0;
    finish_maintenance(maintenance_result);
    if (g_ota_mode_active) {
      lcd_exit_ota_mode(exit_reason ? exit_reason : "ota_check_done");
    }
    ota_stay_awake_until_ms = 0;
  };
  if (ota_check_in_progress || ota_locked) {
    report_result("fail", "busy", kFirmwareVersion, "busy");
    finish_attempt("failed", "busy", "busy");
    return;
  }
  if (guard_disabled) {
    report_result("fail", "guard_disabled", kFirmwareVersion, "guard_disabled");
    finish_attempt("failed", "guard_disabled", "guard_disabled");
    return;
  }
  if (rate_limited) {
    report_result("fail", "rate_limited", kFirmwareVersion, "rate_limited");
    finish_attempt("failed", "rate_limited", "rate_limited");
    return;
  }
  if (wake_window) {
    if (ota_attempts_this_wake_window >= OTA_WAKE_WINDOW_MAX_ATTEMPTS) {
      report_result("fail", "wake_window_retry_exhausted", kFirmwareVersion, "wake_window_retry_exhausted");
      finish_attempt("failed", "wake_window_attempted", "wake_window_retry_exhausted");
      return;
    }
    ota_attempts_this_wake_window++;
    Serial.printf("[LCD_OTA] wake_window attempt=%u/%u\n",
                  (unsigned)ota_attempts_this_wake_window,
                  (unsigned)OTA_WAKE_WINDOW_MAX_ATTEMPTS);
  }
  resolveLcdOtaManifestUrl(&g_ota_config);
  if (!g_ota_config.allowed_host || strlen(g_ota_config.manifest_url) == 0 ||
      containsDisallowedHost(g_ota_config.manifest_url)) {
    Serial.println("[LCD_OTA] Manifest URL invalid or disallowed");
    report_result("fail", "manifest_url_invalid", kFirmwareVersion, "manifest_url_invalid");
    finish_attempt("failed", "manifest_url_invalid", "manifest_url_invalid");
    return;
  }

  if (g_lcd_maintenance_aborted) {
    report_result("fail", "aborted", kFirmwareVersion, "aborted");
    finish_attempt("aborted", "aborted", "aborted");
    return;
  }

  if (!ensure_wifi_connected_for_ota(60000)) {
    Serial.println("[LCD_OTA] Wi-Fi connect failed in OTA mode - backing off");
    ota_fail_record("wifi_connect_fail");
    schedule_wifi_backoff_with_count("ota_wifi_fail", ota_fail_count);
    report_result("fail", "wifi_connect_fail", kFirmwareVersion, "wifi_connect_fail");
    finish_attempt("failed", "wifi_connect_fail", "wifi_connect_fail");
    return;
  }

  if (!ensure_time_synced_for_tls()) {
    Serial.println("[LCD_OTA] time_invalid_timeout -> skipping_ota_this_boot");
    ota_deferred = true;
    ota_guard_record_time_invalid();
    report_result("fail", "time_invalid", kFirmwareVersion, "time_invalid");
    finish_attempt("failed", "time_invalid", "time_invalid");
    return;
  }
  ensure_dns_for_ota();

  if (g_lcd_maintenance_aborted) {
    report_result("fail", "aborted", kFirmwareVersion, "aborted");
    finish_attempt("aborted", "aborted", "aborted");
    return;
  }
  ota_guard_reset_time_invalid();

  char manifest_url[512];
  snprintf(manifest_url, sizeof(manifest_url), "%s?v=%s-%lu",
           g_ota_config.manifest_url, kFirmwareVersion, (unsigned long)millis());

  char host_buf[96];
  if (!extractHostFromUrl(manifest_url, host_buf, sizeof(host_buf))) {
    Serial.println("[LCD_OTA] Failed to extract host from manifest URL");
    report_result("fail", "host_extract_fail", kFirmwareVersion, "host_extract_fail");
    finish_attempt("failed", "host_extract_fail", "host_extract_fail");
    return;
  }
  IPAddress resolved_ip;
  if (!WiFi.hostByName(host_buf, resolved_ip)) {
    Serial.printf("[LCD_OTA] DNS lookup failed host=%s\n", host_buf);
    report_result("fail", "dns_fail", kFirmwareVersion, "dns_fail");
    finish_attempt("failed", "dns_fail", "dns_fail");
    return;
  }
  Serial.printf("[LCD_OTA] DNS host=%s ip=%s\n", host_buf, resolved_ip.toString().c_str());

  ota_check_in_progress = true;
  ota_check_last_attempt_ms = millis();

  // Pause display to free heap for TLS
  Serial.printf("[LCD_OTA][HEAP_PRE] free=%lu largest=%lu\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  
  // Enter OTA mode (deinit LVGL and LCD to free heap for TLS)
  bool ota_mode_ok = lcd_enter_ota_mode(40 * 1024);  // Need ~40KB for TLS
  Serial.printf("[LCD_OTA] ota_mode_entry ok=%d\n", ota_mode_ok ? 1 : 0);
  
  Serial.printf("[LCD_OTA][HEAP_POST_PAUSE] free=%lu largest=%lu\n",
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  // === END OTA MODE ===

  OtaManifest manifest;
  if (!manifest_client.fetchManifest(manifest_url, manifest, 10000)) {
    Serial.println("[LCD_OTA] Manifest fetch failed - aborting");
    report_result("fail", "manifest_fetch_fail", kFirmwareVersion, "manifest_fetch_fail");
    finish_attempt("failed", "manifest_fetch_fail", "manifest_fetch_fail");
    return;
  }

  if (manifest.board_specified && strcasecmp(manifest.board, HALO_BOARD_NAME) != 0) {
    Serial.printf("[LCD_OTA] board_mismatch manifest=%s device=%s\n",
                  manifest.board, HALO_BOARD_NAME);
    report_result("fail", "board_mismatch", kFirmwareVersion, "board_mismatch");
    finish_attempt("failed", "board_mismatch", "board_mismatch");
    return;
  }

  if (containsDisallowedHost(manifest.url) || !isAllowedOtaHost(manifest.url)) {
    Serial.printf("[LCD_OTA] bin_url disallowed: %s\n", manifest.url);
    report_result("fail", "bin_url_disallowed", kFirmwareVersion, "bin_url_disallowed");
    finish_attempt("failed", "bin_url_disallowed", "bin_url_disallowed");
    return;
  }

  if (!manifest_client.isVersionNewer(manifest, kFirmwareVersion)) {
    Serial.printf("[LCD_OTA] up_to_date current=%s manifest=%s\n", kFirmwareVersion, manifest.version);
    report_result("no_update", "up_to_date", kFirmwareVersion, "");
    finish_attempt("noop", "no_update");
    return;
  }

  if (g_lcd_maintenance_aborted) {
    report_result("fail", "aborted", kFirmwareVersion, "aborted");
    finish_attempt("aborted", "aborted", "aborted");
    return;
  }

  Serial.printf("[LCD_OTA] update available %s -> %s\n", kFirmwareVersion, manifest.version);
  if (lcd_ota_request_active && !lcd_ota_request_allow_reboot) {
    Serial.println("[LCD_OTA] reboot_not_allowed -> skipping apply");
    report_result("fail", "reboot_not_allowed", manifest.version, "reboot_not_allowed");
    finish_attempt("failed", "reboot_not_allowed", "reboot_not_allowed");
    return;
  }
  if (!lcd_enter_ota_mode(56 * 1024)) {
    Serial.println("[LCD_OTA] ota_mode heap too low - skipping apply");
    report_result("fail", "heap_low", manifest.version, "heap_low");
    finish_attempt("failed", "heap_low", "heap_low");
    return;
  }
  ota_fail_reset("ota_success");
  report_result("success", "update_available", manifest.version, "");
  const uint32_t lcd_ota_hard_deadline_ms =
      (manifest.size >= (3U * 1024U * 1024U)) ? 1200000UL : 600000UL;
  Serial.printf("[LCD_OTA] apply hard_deadline_ms=%lu size=%u\n",
                (unsigned long)lcd_ota_hard_deadline_ms,
                (unsigned)manifest.size);
  SenseOtaApplier::Result res = ota_applier.applyToOtaPartition(
      manifest.url, manifest.sha256, manifest.size, lcd_ota_hard_deadline_ms, true, manifest.version);
  if (res == SenseOtaApplier::RESULT_SUCCESS) {
    Serial.println("[LCD_OTA] apply returned without reboot - rebooting");
    ESP.restart();
    return;
  }
  const char* res_str = SenseOtaApplier::getResultString(res);
  Serial.printf("[LCD_OTA] apply failed: %s\n", res_str ? res_str : "unknown");
  report_result("fail", res_str ? res_str : "apply_failed", manifest.version, res_str ? res_str : "apply_failed");
  finish_attempt("failed", res_str ? res_str : "apply_failed", res_str ? res_str : "apply_failed");
}

void halo_lcd_prod_loop() {
  wifi_manager.update();
  if (wifi_ok()) {
    if (wifi_connecting_in_progress) {
      wifi_connecting_in_progress = false;
      wifi_connecting_start_ms = 0;
    }
    if (wifi_connected_at_ms == 0) {
      wifi_connected_at_ms = millis();
      wifi_connect_defer_logged = false;
      wifi_fail_count = 0;
      wifi_next_retry_ms = 0;
      wifi_heap_logged = false;
      if (!wifi_dns_set) {
        IPAddress d0 = WiFi.dnsIP(0);
        IPAddress d1 = WiFi.dnsIP(1);
        if (d0 == IPAddress(0, 0, 0, 0) && d1 == IPAddress(0, 0, 0, 0)) {
          IPAddress dns1(1, 1, 1, 1);
          IPAddress dns2(8, 8, 8, 8);
          bool dns_ok = WiFi.setDNS(dns1, dns2);
          Serial.printf("[LCD_WIFI] set DNS 1.1.1.1/8.8.8.8 ok=%d\n", dns_ok ? 1 : 0);
        } else {
          Serial.printf("[LCD_WIFI] keep DHCP DNS d0=%s d1=%s\n",
                        d0.toString().c_str(),
                        d1.toString().c_str());
        }
        wifi_dns_set = true;
      }
      start_sntp_if_needed();
    }
    if (!sntp_started) {
      start_sntp_if_needed();
    }
    if (!wifi_heap_logged) {
      log_lcd_heap_stats("after_wifi");
      wifi_heap_logged = true;
    }
  } else {
    wifi_connected_at_ms = 0;
    wifi_dns_set = false;
    wifi_connect_defer_logged = false;
    wifi_heap_logged = false;
    if (wifi_connecting_in_progress && halo_lcd_prod_wifi_failed()) {
      schedule_wifi_backoff("status_failed");
    }
    if (wifi_connecting_in_progress && wifi_connecting_start_ms > 0 &&
        (millis() - wifi_connecting_start_ms) > 45000) {
      schedule_wifi_backoff("connect_timeout");
    }
  }

  if (ota_locked && (millis() - ota_lock_at_ms) >= OTA_LOCK_TIMEOUT_MS) {
    ota_locked = false;
    Serial.println("[LCD_OTA] auto-unlock after timeout");
  }

  if (wifi_enabled && wifi_enable_time_ms > 0 &&
      (millis() - wifi_enable_time_ms) >= (wifi_timeout_sec * 1000UL)) {
    Serial.printf("[LCD_WIFI] timeout (%lu s) - turning off\n", wifi_timeout_sec);
    if (!wifi_ok()) {
      schedule_wifi_backoff("enable_timeout");
    }
    wifi_manager.disconnect();
    wifi_enabled = false;
    wifi_enable_time_ms = 0;
    wifi_connecting_in_progress = false;
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
  resolveLcdOtaManifestUrl(&g_ota_config);
  Serial.printf("[LCD_OTA] manifest_url=%s host_allowed=%d\n",
                g_ota_config.manifest_url, g_ota_config.allowed_host ? 1 : 0);
  ota_fail_load();
  Serial.printf("[LCD_OTA] fail_state count=%lu last_epoch=%lu\n",
                (unsigned long)ota_fail_count,
                (unsigned long)ota_last_fail_epoch);
  if (wifi_enabled) {
    wifi_pending_start(WIFI_REASON_IDLE);
  } else {
    Serial.println("[LCD_WIFI] disabled by default");
  }
  log_lcd_heap_stats("boot_after_ui");
}


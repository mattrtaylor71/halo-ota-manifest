#include "Truth.h"
#include "BuildInfo.h"
#include "ProvisioningState.h"
#include "BuildFlags.h"
#include "BootState.h"
#include "MqttClient.h"
#include "OtaIntent.h"
#include "MaintenanceWindow.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <string.h>
#include <time.h>
#if __has_include(<esp_netif.h>)
#include <esp_netif.h>
#define TRUTH_HAS_ESP_NETIF 1
#else
#define TRUTH_HAS_ESP_NETIF 0
#endif

__attribute__((weak)) bool ota_test_bypass_reboot_guard_enabled() {
  return false;
}

__attribute__((weak)) bool truth_get_ota_sched_enable() { return false; }
__attribute__((weak)) uint32_t truth_get_next_ota_epoch() { return 0; }
__attribute__((weak)) int truth_get_wake_cause() { return 0; }
__attribute__((weak)) bool truth_get_maintenance_mode() { return false; }
__attribute__((weak)) bool truth_get_maintenance_in_window() { return false; }
__attribute__((weak)) const char* truth_get_sched_fetch_state() { return ""; }
__attribute__((weak)) int32_t truth_get_sched_fetch_http_code() { return 0; }
__attribute__((weak)) int32_t truth_get_sched_fetch_age_s() { return -1; }
__attribute__((weak)) const char* truth_get_sched_fetch_request_id() { return ""; }
__attribute__((weak)) const char* truth_get_sched_last_event() { return ""; }
__attribute__((weak)) int32_t truth_get_sched_last_event_age_s() { return -1; }
__attribute__((weak)) const char* truth_get_sched_last_event_request_id() { return ""; }
__attribute__((weak)) const char* truth_get_lcd_ota_result() { return ""; }
__attribute__((weak)) const char* truth_get_lcd_fw_version() { return ""; }
__attribute__((weak)) const char* truth_get_lcd_running_state() { return "UNKNOWN"; }
__attribute__((weak)) int32_t truth_get_lcd_fw_age_s() { return -1; }
__attribute__((weak)) bool truth_get_lcd_maint_ack() { return false; }
__attribute__((weak)) int32_t truth_get_lcd_maint_ack_age_s() { return -1; }
__attribute__((weak)) int32_t truth_get_lcd_maint_ack_remaining_s() { return -1; }
__attribute__((weak)) int32_t truth_get_lcd_maint_ack_wake_in_s() { return -1; }
__attribute__((weak)) const char* truth_get_lcd_maint_ack_request_id() { return ""; }
__attribute__((weak)) const char* truth_get_lcd_maint_ack_status() { return ""; }
__attribute__((weak)) bool truth_get_lcd_maint_ack_persisted() { return false; }
__attribute__((weak)) uint64_t truth_get_lcd_maint_ack_start_epoch() { return 0; }
__attribute__((weak)) uint32_t truth_get_lcd_maint_ack_duration_sec() { return 0; }
__attribute__((weak)) uint32_t truth_get_lcd_maint_ack_grace_before_sec() { return 0; }
__attribute__((weak)) uint32_t truth_get_lcd_maint_ack_grace_after_sec() { return 0; }
__attribute__((weak)) bool truth_get_maint_sync_pending() { return false; }
__attribute__((weak)) uint32_t truth_get_maint_sync_attempts() { return 0; }
__attribute__((weak)) const char* truth_get_maint_sync_resolution() { return ""; }
__attribute__((weak)) const char* truth_get_maint_last_tx_request_id() { return ""; }
__attribute__((weak)) int32_t truth_get_maint_last_tx_age_s() { return -1; }
__attribute__((weak)) int32_t truth_get_maint_last_tx_remaining_s() { return -1; }
__attribute__((weak)) int32_t truth_get_maint_last_tx_wake_in_s() { return -1; }
__attribute__((weak)) bool truth_get_maint_last_tx_clear() { return false; }
__attribute__((weak)) bool truth_get_maint_last_tx_link_recent() { return false; }
__attribute__((weak)) uint32_t truth_get_boot_count() { return 0; }
__attribute__((weak)) bool truth_get_reboot_loop_detected() { return false; }
__attribute__((weak)) uint32_t truth_get_uart_tx_count() { return 0; }
__attribute__((weak)) uint32_t truth_get_uart_rx_count() { return 0; }
__attribute__((weak)) const char* truth_get_last_uart_tx_type() { return ""; }
__attribute__((weak)) const char* truth_get_last_uart_rx_type() { return ""; }
__attribute__((weak)) const char* truth_get_last_action() { return ""; }
__attribute__((weak)) int32_t truth_get_last_action_age_ms() { return -1; }
__attribute__((weak)) const char* truth_get_lcd_diag_wake() { return ""; }
__attribute__((weak)) const char* truth_get_lcd_diag_screen() { return ""; }
__attribute__((weak)) const char* truth_get_lcd_diag_input() { return ""; }
__attribute__((weak)) const char* truth_get_lcd_diag_last_tx() { return ""; }
__attribute__((weak)) const char* truth_get_lcd_diag_last_rx() { return ""; }
__attribute__((weak)) int32_t truth_get_lcd_diag_input_age_ms() { return -1; }
__attribute__((weak)) int32_t truth_get_lcd_diag_sense_rx_age_ms() { return -1; }
__attribute__((weak)) uint32_t truth_get_lcd_diag_uart_tx() { return 0; }
__attribute__((weak)) uint32_t truth_get_lcd_diag_uart_rx() { return 0; }
__attribute__((weak)) int32_t truth_get_lcd_diag_age_ms() { return -1; }
__attribute__((weak)) const char* truth_get_last_error_stage() { return ""; }
__attribute__((weak)) int32_t truth_get_last_error_code() { return 0; }
__attribute__((weak)) const char* truth_get_last_error_text() { return ""; }
__attribute__((weak)) int32_t truth_get_last_error_age_ms() { return -1; }
__attribute__((weak)) const char* truth_get_recent_action_log() { return ""; }
__attribute__((weak)) const char* truth_get_camera_timeline() { return ""; }
__attribute__((weak)) uint32_t truth_get_upload_cache_count() { return 0; }
__attribute__((weak)) const char* truth_get_upload_cache_last_result() { return "none"; }
__attribute__((weak)) const char* truth_get_upload_cache_last_reason() { return ""; }
__attribute__((weak)) int32_t truth_get_upload_cache_last_age_ms() { return -1; }
__attribute__((weak)) uint32_t truth_get_upload_cache_last_retries() { return 0; }
__attribute__((weak)) const char* truth_get_last_stage() { return ""; }
__attribute__((weak)) int32_t truth_get_last_stage_code() { return 0; }
__attribute__((weak)) int32_t truth_get_last_stage_uptime_ms() { return -1; }
__attribute__((weak)) int32_t truth_get_prev_clean_shutdown() { return 0; }
__attribute__((weak)) uint32_t truth_get_crash_count() { return 0; }
__attribute__((weak)) const char* truth_get_last_crash_stage() { return ""; }
__attribute__((weak)) int32_t truth_get_last_crash_stage_code() { return 0; }
__attribute__((weak)) int32_t truth_get_last_crash_stage_uptime_ms() { return -1; }
__attribute__((weak)) int32_t truth_get_last_crash_reason() { return 0; }
__attribute__((weak)) int32_t truth_get_last_crash_wake_cause() { return 0; }

extern "C" __attribute__((weak)) const char* halo_wifi_guard_truth_state();
__attribute__((weak)) bool wifi_is_connected();

static bool truth_ip_is_bad(const IPAddress& ip) {
  bool all_zero = (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
  bool all_ff = (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255);
  return all_zero || all_ff;
}

static void truth_ip_to_str(const IPAddress& ip, char* out, size_t out_size) {
  snprintf(out, out_size, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

static bool truth_ip_from_netif(char* out, size_t out_size) {
#if TRUTH_HAS_ESP_NETIF
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) {
    return false;
  }
  esp_netif_ip_info_t ip_info;
  if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
    return false;
  }
  IPAddress ip(ip_info.ip.addr);
  if (truth_ip_is_bad(ip)) {
    return false;
  }
  truth_ip_to_str(ip, out, out_size);
  return true;
#else
  (void)out;
  (void)out_size;
  return false;
#endif
}

static void truth_get_ip_str(char* out, size_t out_size, bool connected) {
  strncpy(out, "0.0.0.0", out_size - 1);
  out[out_size - 1] = '\0';
  if (!connected) {
    return;
  }
  IPAddress ip = WiFi.localIP();
  if (truth_ip_is_bad(ip)) {
    Serial.printf("[NET_SANITY] connected_but_bad_ip ip=%d.%d.%d.%d\n",
                  ip[0], ip[1], ip[2], ip[3]);
    if (truth_ip_from_netif(out, out_size)) {
      return;
    }
  }
  truth_ip_to_str(ip, out, out_size);
}

static const char* truth_reset_reason_str(esp_reset_reason_t reason) {
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

static const char* truth_wake_cause_str(int cause) {
  switch (static_cast<esp_sleep_wakeup_cause_t>(cause)) {
    case ESP_SLEEP_WAKEUP_EXT0: return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1: return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "TOUCH";
    case ESP_SLEEP_WAKEUP_ULP: return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO: return "GPIO";
    case ESP_SLEEP_WAKEUP_UART: return "UART";
    case ESP_SLEEP_WAKEUP_WIFI: return "WIFI";
    case ESP_SLEEP_WAKEUP_COCPU: return "COCPU";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG: return "COCPU_TRAP";
    case ESP_SLEEP_WAKEUP_BT: return "BT";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED";
    default: return "OTHER";
  }
}

static void truth_append_flag(char* out, size_t out_size, const char* flag) {
  if (!out || out_size == 0 || !flag || !flag[0]) {
    return;
  }
  size_t len = strlen(out);
  if (len == 0) {
    strncpy(out, flag, out_size - 1);
    out[out_size - 1] = '\0';
    return;
  }
  size_t remaining = out_size - len - 1;
  if (remaining <= 1) {
    return;
  }
  strncat(out, ",", remaining);
  remaining = out_size - strlen(out) - 1;
  if (remaining > 0) {
    strncat(out, flag, remaining);
  }
}

// Forward declaration for OTA config structure (defined in sense_ota_demo.ino)
struct OtaUrlConfig {
  char base_dir[256];
  char manifest_url[512];
  char channel[32];
  char env[8];
  char host[96];
  bool channel_enabled;
  bool allowed_host;
};

// Forward declarations for helper functions (defined in sense_ota_demo.ino)
extern bool containsDisallowedHost(const char* url);

// Single global OTA config cache
static OtaUrlConfig g_ota_config;

const OtaUrlConfig* ota_get_config() {
  return &g_ota_config;
}

// Mutable accessor for resolveOtaManifestUrl (needs non-const pointer)
OtaUrlConfig* ota_get_config_mutable() {
  return &g_ota_config;
}

// Manifest state singleton
static TruthManifestState g_truth_manifest_state;

TruthManifestState::TruthManifestState() : status(NONE) {
  version[0] = '\0';
}

void TruthManifestState::setOk(const char* ver) {
  status = OK;
  if (ver) {
    strncpy(version, ver, sizeof(version) - 1);
    version[sizeof(version) - 1] = '\0';
  } else {
    version[0] = '\0';
  }
}

void TruthManifestState::setErr() {
  status = ERR;
  version[0] = '\0';
}

void TruthManifestState::reset() {
  status = NONE;
  version[0] = '\0';
}

TruthManifestState& truth_get_manifest_state() {
  return g_truth_manifest_state;
}

// UART sync status
namespace Truth {
  static bool g_uart_sync_established = false;
  
  bool getUartSyncEstablished() {
    return g_uart_sync_established;
  }
  
  void setUartSyncEstablished(bool value) {
    g_uart_sync_established = value;
  }
  
  void evaluateOtaApply(bool& apply_allowed, char* why_buf, size_t why_buf_size) {
    if (!why_buf || why_buf_size == 0) {
      apply_allowed = false;
      return;
    }
    
    // Default: not allowed
    apply_allowed = false;
    strncpy(why_buf, "other", why_buf_size - 1);
    why_buf[why_buf_size - 1] = '\0';
    
    // Check OTA_ENABLED
    if (HALO_OTA_ENABLED == 0) {
      strncpy(why_buf, "ota_disabled", why_buf_size - 1);
      why_buf[why_buf_size - 1] = '\0';
      return;
    }
    
    // Check SHIP_TEST_MODE
    if (HALO_SHIP_TEST_MODE != 0) {
      strncpy(why_buf, "ship_test_mode", why_buf_size - 1);
      why_buf[why_buf_size - 1] = '\0';
      return;
    }
    
    // Check provisioning state (must be provisioned, owner_id set, and CONNECTED state)
    bool provisioned = ProvisioningState::isProvisioned();
    char owner_id_check[64];
    bool owner_id_set = ProvisioningState::loadOwnerId(owner_id_check, sizeof(owner_id_check));
    ProvisioningState::State prov_state = ProvisioningState::getState();
    bool is_connected_state = (prov_state == ProvisioningState::STATE_CONNECTED);
    
    if (!provisioned) {
      strncpy(why_buf, "not_provisioned", why_buf_size - 1);
      why_buf[why_buf_size - 1] = '\0';
      return;
    }
    
    if (!owner_id_set) {
      strncpy(why_buf, "no_owner", why_buf_size - 1);
      why_buf[why_buf_size - 1] = '\0';
      return;
    }
    
    if (!is_connected_state) {
      strncpy(why_buf, "not_connected_state", why_buf_size - 1);
      why_buf[why_buf_size - 1] = '\0';
      return;
    }
    
    // All checks passed
    apply_allowed = true;
    strncpy(why_buf, "ok", why_buf_size - 1);
    why_buf[why_buf_size - 1] = '\0';
  }
}

const char* ota_disabled_reason() {
  static bool bypass_logged = false;
  static bool guard_logged = false;
  bool guard_disabled = false;

#if OTA_TEST_BYPASS_REBOOT_LOOP_GUARD
  guard_disabled = true;
#else
  guard_disabled = ota_test_bypass_reboot_guard_enabled();
#endif

  if (guard_disabled && !bypass_logged) {
    Serial.println("[OTA_TEST] reboot_loop_guard bypass ACTIVE (guard suppressed)");
    bypass_logged = true;
  }

  // Check compile-time flag first
  if (HALO_OTA_ENABLED == 0) {
    if (!guard_logged) {
      Serial.printf("[BOOT_GUARD] disabled=%d reason=%s\n", guard_disabled ? 1 : 0, "enabled_flag_off");
      guard_logged = true;
    }
    return "enabled_flag_off";
  }
  
  // Check reboot loop guard (must check before other conditions)
  // Use same thresholds as sense_ota_demo.ino for consistency
  // Note: checkRebootLoop() returns false if dev override is enabled
  if (!guard_disabled && BootState::checkRebootLoop(3, 30000)) {  // 3 reboots in 30 seconds
    if (!guard_logged) {
      Serial.printf("[BOOT_GUARD] disabled=%d reason=%s\n", guard_disabled ? 1 : 0, "reboot_loop_guard");
      guard_logged = true;
    }
    return "reboot_loop_guard";
  }
  
  const OtaUrlConfig* config = ota_get_config();
  
  // Check if manifest URL is empty
  if (strlen(config->manifest_url) == 0) {
    if (!guard_logged) {
      Serial.printf("[BOOT_GUARD] disabled=%d reason=%s\n", guard_disabled ? 1 : 0, "no_manifest_url");
      guard_logged = true;
    }
    return "no_manifest_url";
  }
  
  // Check if GitHub blocked
  if (containsDisallowedHost(config->manifest_url)) {
    if (!guard_logged) {
      Serial.printf("[BOOT_GUARD] disabled=%d reason=%s\n", guard_disabled ? 1 : 0, "github_blocked");
      guard_logged = true;
    }
    return "github_blocked";
  }
  
  // Check if host not allowed
  if (!config->allowed_host) {
    if (!guard_logged) {
      Serial.printf("[BOOT_GUARD] disabled=%d reason=%s\n", guard_disabled ? 1 : 0, "host_not_allowed");
      guard_logged = true;
    }
    return "host_not_allowed";
  }
  
  // Check if channel disabled
  if (!config->channel_enabled) {
    if (!guard_logged) {
      Serial.printf("[BOOT_GUARD] disabled=%d reason=%s\n", guard_disabled ? 1 : 0, "channel_disabled");
      guard_logged = true;
    }
    return "channel_disabled";
  }
  
  // All checks passed
  if (!guard_logged) {
    Serial.printf("[BOOT_GUARD] disabled=%d reason=%s\n", guard_disabled ? 1 : 0, "ok");
    guard_logged = true;
  }
  return "ok";
}

bool ota_is_enabled() {
  return (strcmp(ota_disabled_reason(), "ok") == 0);
}

// Static flag to ensure BUILD_DIAG prints only once per boot
static bool g_build_diag_printed = false;

void dump_system_truth(const char* reason) {
  // BUILD_DIAG: Print once per boot to verify BUILD_ID consistency
  if (!g_build_diag_printed) {
    Serial.printf("[BUILD_DIAG] truth BUILD_ID=%s VERSION=%s\n", kBuildId, kFirmwareVersion);
    g_build_diag_printed = true;
  }
  
  // Firmware version and build ID (use shared variables from BuildInfo.cpp)
  const char* fw_version = kFirmwareVersion;
  const char* build_id = kBuildId;
  
  // Provisioning state
  ProvisioningState::State prov_state = ProvisioningState::getState();
  const char* prov_state_str = ProvisioningState::getStateString(prov_state);
  bool provisioned = ProvisioningState::isProvisioned();
  
  // Owner ID check
  char owner_id[64];
  bool owner_set = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  
  // Setup mode check (infer from provisioning state)
  bool setup_mode = (prov_state == ProvisioningState::STATE_AP_SETUP);
  
  // WiFi status - canonical source of truth
  wl_status_t wifi_status = WiFi.status();
  bool connected = false;
  if (wifi_is_connected) {
    connected = wifi_is_connected();
  } else {
    connected = (wifi_status == WL_CONNECTED);
  }
  const char* wifi_override = halo_wifi_guard_truth_state ? halo_wifi_guard_truth_state() : nullptr;
  const char* wifi_str = "unknown";
  if (connected) {
    wifi_str = "connected";
  } else if (prov_state == ProvisioningState::STATE_ERROR) {
    wifi_str = "failed";  // Match /status wifi_state so TRUTH and portal agree
  } else if (wifi_override && strcmp(wifi_override, "failed") == 0) {
    wifi_str = "failed";
  } else {
    switch (wifi_status) {
      case WL_CONNECT_FAILED:
      case WL_NO_SSID_AVAIL:
      case WL_CONNECTION_LOST:
        wifi_str = "failed";
        break;
      case WL_IDLE_STATUS:
      case WL_SCAN_COMPLETED:
      case WL_DISCONNECTED:
      default:
        wifi_str = "disconnected";
        break;
    }
  }
  
  // IP address (avoid String lifetime bug: use local buffer)
  char ip_str[16];
  truth_get_ip_str(ip_str, sizeof(ip_str), connected);
  
  // RSSI
  int rssi = 0;
  if (connected) {
    rssi = WiFi.RSSI();
  }
  
  if (connected && strcmp(wifi_str, "failed") == 0) {
    Serial.printf("[WIFI_SANITY] ERROR: truth_wifi=failed but WiFi.status=WL_CONNECTED ip=%s\n",
                  ip_str);
    wifi_str = "connected";
  }
  
  // Partition labels
  const esp_partition_t* running = esp_ota_get_running_partition();
  const char* part_label = running ? running->label : "-";
  
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const char* boot_label = boot ? boot->label : "-";
  
  // OTA enabled (runtime check via ota_is_enabled())
  bool ota_en = ota_is_enabled();
  
  // SHIP_TEST_MODE (compile-time, from BuildFlags.h)
  bool ship = (HALO_SHIP_TEST_MODE != 0);
  
  // OTA disabled reason (from ota_disabled_reason())
  const char* ota_disabled_reason_str = ota_disabled_reason();
  
  // OTA apply gating evaluation (only if OTA is enabled)
  bool ota_apply_allowed = false;
  char ota_why[32] = "ok";
  if (ota_en) {
    Truth::evaluateOtaApply(ota_apply_allowed, ota_why, sizeof(ota_why));
  } else {
    // OTA disabled - use the disabled reason
    strncpy(ota_why, ota_disabled_reason_str, sizeof(ota_why) - 1);
    ota_why[sizeof(ota_why) - 1] = '\0';
  }
  
  // Manifest state
  TruthManifestState& manifest_state = truth_get_manifest_state();
  const char* manifest_str = "none";
  const char* mver_str = "-";
  switch (manifest_state.status) {
    case TruthManifestState::OK:
      manifest_str = "ok";
      mver_str = manifest_state.version[0] != '\0' ? manifest_state.version : "-";
      break;
    case TruthManifestState::ERR:
      manifest_str = "err";
      break;
    case TruthManifestState::NONE:
    default:
      manifest_str = "none";
      break;
  }
  
  // UART sync status
  bool uart_sync = Truth::getUartSyncEstablished();

  // MQTT metrics
  MqttMetrics mqtt_metrics = {};
  mqtt_get_metrics(&mqtt_metrics);
  const char* mqtt_state = mqtt_metrics.connected ? "connected" : "disconnected";

  // OTA intent/desired
  const char* desired_sense = OtaIntent::getDesiredSense();
  const char* desired_lcd = OtaIntent::getDesiredLcd();
  bool desired_force = OtaIntent::getDesiredForce();
  int32_t desired_age_s = OtaIntent::getDesiredAgeS();
  bool ota_intent = OtaIntent::getOtaIntentActive();
  // (duplicate declarations removed)
  bool sched_enable = truth_get_ota_sched_enable();
  uint32_t next_epoch = truth_get_next_ota_epoch();
  const char* sched_fetch_state = truth_get_sched_fetch_state();
  int32_t sched_fetch_http = truth_get_sched_fetch_http_code();
  int32_t sched_fetch_age_s = truth_get_sched_fetch_age_s();
  const char* sched_fetch_request_id = truth_get_sched_fetch_request_id();
  int wake_cause = truth_get_wake_cause();
  const char* wake_cause_str = truth_wake_cause_str(wake_cause);
  bool maint_mode = truth_get_maintenance_mode();
  bool maint_window = truth_get_maintenance_in_window();
  const char* lcd_ota_result = truth_get_lcd_ota_result();
  uint32_t upload_cache_count = truth_get_upload_cache_count();
  const char* upload_cache_last_result = truth_get_upload_cache_last_result();
  const char* upload_cache_last_reason = truth_get_upload_cache_last_reason();
  int32_t upload_cache_last_age_ms = truth_get_upload_cache_last_age_ms();
  uint32_t upload_cache_last_retries = truth_get_upload_cache_last_retries();

  unsigned long uptime_ms = millis();
  esp_reset_reason_t reset_reason = esp_reset_reason();
  const char* reset_reason_str = truth_reset_reason_str(reset_reason);
  size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  size_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  const char* ota_state = "idle";
  if (!ota_en) {
    ota_state = "disabled";
  } else if (!ota_apply_allowed) {
    ota_state = "blocked";
  } else if (manifest_state.status == TruthManifestState::ERR) {
    ota_state = "manifest_err";
  } else if (ota_intent) {
    ota_state = "intent";
  } else if (maint_mode || maint_window) {
    ota_state = "maintenance";
  } else {
    ota_state = "ready";
  }

  uint64_t now_epoch = 0;
  time_t now_s = time(nullptr);
  if (now_s > 0) {
    now_epoch = static_cast<uint64_t>(now_s);
  }
  MaintenanceWindow mw;
  bool maint_scheduled = mw.loadFromNvs();
  uint64_t maint_start = maint_scheduled ? mw.start_epoch : 0;
  uint64_t maint_end = maint_scheduled ? (mw.start_epoch + mw.duration_sec + mw.grace_after_sec) : 0;
  uint64_t maint_wake = (maint_scheduled && now_epoch > 0) ? mw.nextWakeEpochForSleep(now_epoch) : 0;

  char health_flags[128];
  health_flags[0] = '\0';
  if (!provisioned) {
    truth_append_flag(health_flags, sizeof(health_flags), "not_provisioned");
  }
  if (!owner_set) {
    truth_append_flag(health_flags, sizeof(health_flags), "no_owner");
  }
  if (setup_mode) {
    truth_append_flag(health_flags, sizeof(health_flags), "setup_mode");
  }
  if (strcmp(wifi_str, "connected") != 0) {
    truth_append_flag(health_flags, sizeof(health_flags), "wifi_down");
  }
  if (connected && strcmp(ip_str, "0.0.0.0") == 0) {
    truth_append_flag(health_flags, sizeof(health_flags), "ip_bad");
  }
  if (strcmp(mqtt_state, "connected") != 0) {
    truth_append_flag(health_flags, sizeof(health_flags), "mqtt_down");
  }
  if (!uart_sync) {
    truth_append_flag(health_flags, sizeof(health_flags), "uart_unsync");
  }
  if (!ota_en) {
    truth_append_flag(health_flags, sizeof(health_flags), "ota_disabled");
  } else if (!ota_apply_allowed) {
    truth_append_flag(health_flags, sizeof(health_flags), "ota_blocked");
  }
  if (manifest_state.status == TruthManifestState::ERR) {
    truth_append_flag(health_flags, sizeof(health_flags), "manifest_err");
  }
  if (heap_free < 40960) {
    truth_append_flag(health_flags, sizeof(health_flags), "heap_low");
  }
  if (upload_cache_count > 0) {
    truth_append_flag(health_flags, sizeof(health_flags), "upload_cached");
  }
  const char* health = health_flags[0] ? "warn" : "ok";
  
  // Single-line Serial.printf output
  Serial.printf("[TRUTH] reason=%s fw=%s build=%s prov_state=%s provisioned=%d owner=%d setup=%d wifi=%s ip=%s rssi=%d part=%s boot=%s ota_en=%d ship=%d ota_apply=%d why=%s ota_guard=%s ota_state=%s manifest=%s mver=%s uart=%d mqtt=%s mqtt_last_rx_ms=%lu mqtt_last_pub_ms=%lu mqtt_fail_count=%lu mqtt_pub_inflight=%lu mqtt_cmd_q=%lu desired_sense=%s desired_lcd=%s desired_force=%d desired_age_s=%ld lcd_fw=%s lcd_fw_age_s=%ld ota_intent=%d ota_sched_enable=%d next_ota_epoch=%lu sched_fetch=%s sched_fetch_http=%ld sched_fetch_age_s=%ld sched_fetch_request_id=%s wake_cause=%d wake_cause_str=%s maintenance_mode=%d maintenance_in_window=%d maint_scheduled=%d maint_start_epoch=%llu maint_end_epoch=%llu maint_wake_epoch=%llu boot_count=%lu reboot_loop=%d uptime_ms=%lu reset_reason=%s heap_free=%lu heap_min=%lu heap_largest=%lu health=%s health_flags=%s lcd_ota_result=%s lcd_maint_ack=%d lcd_maint_ack_age_s=%ld maint_sync_pending=%d maint_sync_attempts=%lu upload_cache_count=%lu upload_cache_last_result=%s upload_cache_last_reason=%s upload_cache_last_age_ms=%ld upload_cache_last_retries=%lu\n",
                reason ? reason : "(null)",
                fw_version,
                build_id,
                prov_state_str,
                provisioned ? 1 : 0,
                owner_set ? 1 : 0,
                setup_mode ? 1 : 0,
                wifi_str,
                ip_str,
                rssi,
                part_label,
                boot_label,
                ota_en ? 1 : 0,
                ship ? 1 : 0,
                ota_apply_allowed ? 1 : 0,
                ota_why,
                ota_disabled_reason_str,
                ota_state,
                manifest_str,
                mver_str,
                uart_sync ? 1 : 0,
                mqtt_state,
                static_cast<unsigned long>(mqtt_metrics.last_rx_ms),
                static_cast<unsigned long>(mqtt_metrics.last_pub_ms),
                static_cast<unsigned long>(mqtt_metrics.fail_count),
                static_cast<unsigned long>(mqtt_metrics.pub_inflight),
                static_cast<unsigned long>(mqtt_metrics.cmd_queue_depth),
                desired_sense && desired_sense[0] ? desired_sense : "-",
                desired_lcd && desired_lcd[0] ? desired_lcd : "-",
                desired_force ? 1 : 0,
                static_cast<long>(desired_age_s),
                truth_get_lcd_fw_version() && truth_get_lcd_fw_version()[0] ? truth_get_lcd_fw_version() : "-",
                static_cast<long>(truth_get_lcd_fw_age_s()),
                ota_intent ? 1 : 0,
                sched_enable ? 1 : 0,
                static_cast<unsigned long>(next_epoch),
                sched_fetch_state && sched_fetch_state[0] ? sched_fetch_state : "-",
                static_cast<long>(sched_fetch_http),
                static_cast<long>(sched_fetch_age_s),
                sched_fetch_request_id && sched_fetch_request_id[0] ? sched_fetch_request_id : "-",
                wake_cause,
                wake_cause_str,
                maint_mode ? 1 : 0,
                maint_window ? 1 : 0,
                maint_scheduled ? 1 : 0,
                static_cast<unsigned long long>(maint_start),
                static_cast<unsigned long long>(maint_end),
                static_cast<unsigned long long>(maint_wake),
                static_cast<unsigned long>(truth_get_boot_count()),
                truth_get_reboot_loop_detected() ? 1 : 0,
                uptime_ms,
                reset_reason_str,
                static_cast<unsigned long>(heap_free),
                static_cast<unsigned long>(heap_min),
                static_cast<unsigned long>(heap_largest),
               health,
               health_flags[0] ? health_flags : "-",
               lcd_ota_result && lcd_ota_result[0] ? lcd_ota_result : "-",
               truth_get_lcd_maint_ack() ? 1 : 0,
               static_cast<long>(truth_get_lcd_maint_ack_age_s()),
               truth_get_maint_sync_pending() ? 1 : 0,
               static_cast<unsigned long>(truth_get_maint_sync_attempts()),
               static_cast<unsigned long>(upload_cache_count),
               upload_cache_last_result && upload_cache_last_result[0] ? upload_cache_last_result : "-",
               upload_cache_last_reason && upload_cache_last_reason[0] ? upload_cache_last_reason : "-",
               static_cast<long>(upload_cache_last_age_ms),
               static_cast<unsigned long>(upload_cache_last_retries));
}

bool build_truth_json(const char* reason, char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return false;
  }

  const char* fw_version = kFirmwareVersion;
  const char* build_id = kBuildId;

  ProvisioningState::State prov_state = ProvisioningState::getState();
  const char* prov_state_str = ProvisioningState::getStateString(prov_state);
  bool provisioned = ProvisioningState::isProvisioned();

  char owner_id[64];
  bool owner_set = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));

  bool setup_mode = (prov_state == ProvisioningState::STATE_AP_SETUP);

  wl_status_t wifi_status = WiFi.status();
  bool connected = false;
  if (wifi_is_connected) {
    connected = wifi_is_connected();
  } else {
    connected = (wifi_status == WL_CONNECTED);
  }
  const char* wifi_str = "unknown";
  const char* wifi_override = halo_wifi_guard_truth_state ? halo_wifi_guard_truth_state() : nullptr;
  if (connected) {
    wifi_str = "connected";
  } else if (prov_state == ProvisioningState::STATE_ERROR) {
    wifi_str = "failed";
  } else if (wifi_override && strcmp(wifi_override, "failed") == 0) {
    wifi_str = "failed";
  } else {
    switch (wifi_status) {
      case WL_CONNECT_FAILED:
      case WL_NO_SSID_AVAIL:
      case WL_CONNECTION_LOST:
        wifi_str = "failed";
        break;
      case WL_IDLE_STATUS:
      case WL_SCAN_COMPLETED:
      case WL_DISCONNECTED:
      default:
        wifi_str = "disconnected";
        break;
    }
  }

  char ip_str[16];
  truth_get_ip_str(ip_str, sizeof(ip_str), connected);

  int rssi = 0;
  if (connected) {
    rssi = WiFi.RSSI();
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  const char* part_label = running ? running->label : "-";

  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const char* boot_label = boot ? boot->label : "-";

  bool ota_en = ota_is_enabled();
  bool ship = (HALO_SHIP_TEST_MODE != 0);

  const char* ota_disabled_reason_str = ota_disabled_reason();

  bool ota_apply_allowed = false;
  char ota_why[32] = "ok";
  if (ota_en) {
    Truth::evaluateOtaApply(ota_apply_allowed, ota_why, sizeof(ota_why));
  } else {
    strncpy(ota_why, ota_disabled_reason_str, sizeof(ota_why) - 1);
    ota_why[sizeof(ota_why) - 1] = '\0';
  }

  TruthManifestState& manifest_state = truth_get_manifest_state();
  const char* manifest_str = "none";
  const char* mver_str = "-";
  switch (manifest_state.status) {
    case TruthManifestState::OK:
      manifest_str = "ok";
      mver_str = manifest_state.version[0] != '\0' ? manifest_state.version : "-";
      break;
    case TruthManifestState::ERR:
      manifest_str = "err";
      break;
    case TruthManifestState::NONE:
    default:
      manifest_str = "none";
      break;
  }

  bool uart_sync = Truth::getUartSyncEstablished();

  MqttMetrics mqtt_metrics = {};
  mqtt_get_metrics(&mqtt_metrics);
  const char* mqtt_state = mqtt_metrics.connected ? "connected" : "disconnected";

  const char* desired_sense = OtaIntent::getDesiredSense();
  const char* desired_lcd = OtaIntent::getDesiredLcd();
  bool desired_force = OtaIntent::getDesiredForce();
  int32_t desired_age_s = OtaIntent::getDesiredAgeS();
  bool ota_intent = OtaIntent::getOtaIntentActive();
  bool sched_enable = truth_get_ota_sched_enable();
  uint32_t next_epoch = truth_get_next_ota_epoch();
  const char* sched_fetch_state = truth_get_sched_fetch_state();
  int32_t sched_fetch_http = truth_get_sched_fetch_http_code();
  int32_t sched_fetch_age_s = truth_get_sched_fetch_age_s();
  const char* sched_fetch_request_id = truth_get_sched_fetch_request_id();
  const char* sched_last_event = truth_get_sched_last_event();
  int32_t sched_last_event_age_s = truth_get_sched_last_event_age_s();
  const char* sched_last_event_request_id = truth_get_sched_last_event_request_id();
  int wake_cause = truth_get_wake_cause();
  const char* wake_cause_str = truth_wake_cause_str(wake_cause);
  bool maint_mode = truth_get_maintenance_mode();
  bool maint_window = truth_get_maintenance_in_window();
  const char* lcd_ota_result = truth_get_lcd_ota_result();
  const char* lcd_fw = truth_get_lcd_fw_version();
  int32_t lcd_fw_age_s = truth_get_lcd_fw_age_s();
  const char* lcd_running_state = truth_get_lcd_running_state();
  bool lcd_maint_ack = truth_get_lcd_maint_ack();
  int32_t lcd_maint_ack_age_s = truth_get_lcd_maint_ack_age_s();
  int32_t lcd_maint_ack_remaining_s = truth_get_lcd_maint_ack_remaining_s();
  int32_t lcd_maint_ack_wake_in_s = truth_get_lcd_maint_ack_wake_in_s();
  const char* lcd_maint_ack_request_id = truth_get_lcd_maint_ack_request_id();
  const char* lcd_maint_ack_status = truth_get_lcd_maint_ack_status();
  bool lcd_maint_ack_persisted = truth_get_lcd_maint_ack_persisted();
  uint64_t lcd_maint_ack_start_epoch = truth_get_lcd_maint_ack_start_epoch();
  uint32_t lcd_maint_ack_duration_sec = truth_get_lcd_maint_ack_duration_sec();
  uint32_t lcd_maint_ack_grace_before_sec = truth_get_lcd_maint_ack_grace_before_sec();
  uint32_t lcd_maint_ack_grace_after_sec = truth_get_lcd_maint_ack_grace_after_sec();
  bool maint_sync_pending = truth_get_maint_sync_pending();
  uint32_t maint_sync_attempts = truth_get_maint_sync_attempts();
  const char* maint_sync_resolution = truth_get_maint_sync_resolution();
  const char* maint_last_tx_request_id = truth_get_maint_last_tx_request_id();
  int32_t maint_last_tx_age_s = truth_get_maint_last_tx_age_s();
  int32_t maint_last_tx_remaining_s = truth_get_maint_last_tx_remaining_s();
  int32_t maint_last_tx_wake_in_s = truth_get_maint_last_tx_wake_in_s();
  bool maint_last_tx_clear = truth_get_maint_last_tx_clear();
  bool maint_last_tx_link_recent = truth_get_maint_last_tx_link_recent();
  uint32_t upload_cache_count = truth_get_upload_cache_count();
  const char* upload_cache_last_result = truth_get_upload_cache_last_result();
  const char* upload_cache_last_reason = truth_get_upload_cache_last_reason();
  int32_t upload_cache_last_age_ms = truth_get_upload_cache_last_age_ms();
  uint32_t upload_cache_last_retries = truth_get_upload_cache_last_retries();

  unsigned long uptime_ms = millis();
  esp_reset_reason_t reset_reason = esp_reset_reason();
  const char* reset_reason_str = truth_reset_reason_str(reset_reason);
  size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  size_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  const char* ota_state = "idle";
  if (!ota_en) {
    ota_state = "disabled";
  } else if (!ota_apply_allowed) {
    ota_state = "blocked";
  } else if (manifest_state.status == TruthManifestState::ERR) {
    ota_state = "manifest_err";
  } else if (ota_intent) {
    ota_state = "intent";
  } else if (maint_mode || maint_window) {
    ota_state = "maintenance";
  } else {
    ota_state = "ready";
  }

  uint64_t now_epoch = 0;
  time_t now_s = time(nullptr);
  if (now_s > 0) {
    now_epoch = static_cast<uint64_t>(now_s);
  }
  MaintenanceWindow mw;
  bool maint_scheduled = mw.loadFromNvs();
  uint64_t maint_start = maint_scheduled ? mw.start_epoch : 0;
  uint64_t maint_end = maint_scheduled ? (mw.start_epoch + mw.duration_sec + mw.grace_after_sec) : 0;
  uint64_t maint_wake = (maint_scheduled && now_epoch > 0) ? mw.nextWakeEpochForSleep(now_epoch) : 0;
  const char* maint_request_id = (maint_scheduled && mw.request_id[0]) ? mw.request_id : "-";

  char health_flags[128];
  health_flags[0] = '\0';
  if (!provisioned) {
    truth_append_flag(health_flags, sizeof(health_flags), "not_provisioned");
  }
  if (!owner_set) {
    truth_append_flag(health_flags, sizeof(health_flags), "no_owner");
  }
  if (setup_mode) {
    truth_append_flag(health_flags, sizeof(health_flags), "setup_mode");
  }
  if (strcmp(wifi_str, "connected") != 0) {
    truth_append_flag(health_flags, sizeof(health_flags), "wifi_down");
  }
  if (connected && strcmp(ip_str, "0.0.0.0") == 0) {
    truth_append_flag(health_flags, sizeof(health_flags), "ip_bad");
  }
  if (strcmp(mqtt_state, "connected") != 0) {
    truth_append_flag(health_flags, sizeof(health_flags), "mqtt_down");
  }
  if (!uart_sync) {
    truth_append_flag(health_flags, sizeof(health_flags), "uart_unsync");
  }
  if (!ota_en) {
    truth_append_flag(health_flags, sizeof(health_flags), "ota_disabled");
  } else if (!ota_apply_allowed) {
    truth_append_flag(health_flags, sizeof(health_flags), "ota_blocked");
  }
  if (manifest_state.status == TruthManifestState::ERR) {
    truth_append_flag(health_flags, sizeof(health_flags), "manifest_err");
  }
  if (heap_free < 40960) {
    truth_append_flag(health_flags, sizeof(health_flags), "heap_low");
  }
  if (upload_cache_count > 0) {
    truth_append_flag(health_flags, sizeof(health_flags), "upload_cached");
  }
  const char* health = health_flags[0] ? "warn" : "ok";

  int written = snprintf(out, out_len,
                         "{\"reason\":\"%s\",\"fw\":\"%s\",\"build\":\"%s\",\"prov_state\":\"%s\","
                         "\"provisioned\":%d,\"owner\":%d,\"setup\":%d,\"wifi\":\"%s\","
                         "\"ip\":\"%s\",\"rssi\":%d,\"part\":\"%s\",\"boot\":\"%s\","
                         "\"ota_en\":%d,\"ship\":%d,\"ota_apply\":%d,\"why\":\"%s\","
                         "\"ota_guard\":\"%s\",\"ota_state\":\"%s\","
                         "\"manifest\":\"%s\",\"mver\":\"%s\",\"uart\":%d,"
                         "\"mqtt\":\"%s\",\"mqtt_last_rx_ms\":%lu,\"mqtt_last_pub_ms\":%lu,"
                         "\"mqtt_fail_count\":%lu,\"mqtt_pub_inflight\":%lu,\"mqtt_cmd_q\":%lu,"
                         "\"desired_sense\":\"%s\",\"desired_lcd\":\"%s\",\"desired_force\":%d,"
                         "\"desired_age_s\":%ld,\"lcd_fw\":\"%s\",\"lcd_fw_age_s\":%ld,\"ota_intent\":%d,"
                         "\"ota_sched_enable\":%d,\"next_ota_epoch\":%lu,"
                         "\"sched_fetch\":\"%s\",\"sched_fetch_http\":%ld,"
                         "\"sched_fetch_age_s\":%ld,\"sched_fetch_request_id\":\"%s\","
                         "\"sched_last_event\":\"%s\",\"sched_last_event_age_s\":%ld,"
                         "\"sched_last_event_request_id\":\"%s\","
                         "\"wake_cause\":%d,\"wake_cause_str\":\"%s\","
                         "\"maintenance_mode\":%d,\"maintenance_in_window\":%d,"
                         "\"maint_scheduled\":%d,\"maint_start_epoch\":%llu,\"maint_end_epoch\":%llu,"
                         "\"maint_wake_epoch\":%llu,\"maint_request_id\":\"%s\","
                         "\"boot_count\":%lu,\"reboot_loop\":%d,"
                         "\"uptime_ms\":%lu,\"reset_reason\":\"%s\","
                         "\"heap_free\":%lu,\"heap_min\":%lu,\"heap_largest\":%lu,"
                         "\"health\":\"%s\",\"health_flags\":\"%s\",\"lcd_ota_result\":\"%s\","
                         "\"lcd_running_state\":\"%s\","
                         "\"lcd_maint_ack\":%d,\"lcd_maint_ack_age_s\":%ld,"
                         "\"lcd_maint_ack_remaining_s\":%ld,\"lcd_maint_ack_wake_in_s\":%ld,"
                         "\"lcd_maint_ack_request_id\":\"%s\",\"lcd_maint_ack_status\":\"%s\","
                         "\"lcd_maint_ack_persisted\":%d,"
                         "\"lcd_maint_ack_start_epoch\":%llu,\"lcd_maint_ack_duration_sec\":%lu,"
                         "\"lcd_maint_ack_grace_before_sec\":%lu,\"lcd_maint_ack_grace_after_sec\":%lu,"
                         "\"maint_sync_pending\":%d,\"maint_sync_attempts\":%lu,"
                         "\"maint_sync_resolution\":\"%s\","
                         "\"maint_last_tx_request_id\":\"%s\",\"maint_last_tx_age_s\":%ld,"
                         "\"maint_last_tx_remaining_s\":%ld,\"maint_last_tx_wake_in_s\":%ld,"
                         "\"maint_last_tx_clear\":%d,\"maint_last_tx_link_recent\":%d,"
                         "\"upload_cache_count\":%lu,\"upload_cache_last_result\":\"%s\","
                         "\"upload_cache_last_reason\":\"%s\",\"upload_cache_last_age_ms\":%ld,"
                         "\"upload_cache_last_retries\":%lu}",
                         reason ? reason : "",
                         fw_version,
                         build_id,
                         prov_state_str,
                         provisioned ? 1 : 0,
                         owner_set ? 1 : 0,
                         setup_mode ? 1 : 0,
                         wifi_str,
                         ip_str,
                         rssi,
                         part_label,
                         boot_label,
                         ota_en ? 1 : 0,
                         ship ? 1 : 0,
                         ota_apply_allowed ? 1 : 0,
                         ota_why,
                         ota_disabled_reason_str,
                         ota_state,
                         manifest_str,
                         mver_str,
                         uart_sync ? 1 : 0,
                         mqtt_state,
                         static_cast<unsigned long>(mqtt_metrics.last_rx_ms),
                         static_cast<unsigned long>(mqtt_metrics.last_pub_ms),
                         static_cast<unsigned long>(mqtt_metrics.fail_count),
                         static_cast<unsigned long>(mqtt_metrics.pub_inflight),
                         static_cast<unsigned long>(mqtt_metrics.cmd_queue_depth),
                         desired_sense && desired_sense[0] ? desired_sense : "-",
                         desired_lcd && desired_lcd[0] ? desired_lcd : "-",
                         desired_force ? 1 : 0,
                         static_cast<long>(desired_age_s),
                         lcd_fw && lcd_fw[0] ? lcd_fw : "-",
                         static_cast<long>(lcd_fw_age_s),
                         ota_intent ? 1 : 0,
                         sched_enable ? 1 : 0,
                         static_cast<unsigned long>(next_epoch),
                         sched_fetch_state && sched_fetch_state[0] ? sched_fetch_state : "-",
                         static_cast<long>(sched_fetch_http),
                         static_cast<long>(sched_fetch_age_s),
                         sched_fetch_request_id && sched_fetch_request_id[0] ? sched_fetch_request_id : "-",
                         sched_last_event && sched_last_event[0] ? sched_last_event : "-",
                         static_cast<long>(sched_last_event_age_s),
                         sched_last_event_request_id && sched_last_event_request_id[0] ? sched_last_event_request_id : "-",
                         wake_cause,
                         wake_cause_str,
                         maint_mode ? 1 : 0,
                         maint_window ? 1 : 0,
                         maint_scheduled ? 1 : 0,
                         static_cast<unsigned long long>(maint_start),
                         static_cast<unsigned long long>(maint_end),
                         static_cast<unsigned long long>(maint_wake),
                         maint_request_id,
                         static_cast<unsigned long>(truth_get_boot_count()),
                         truth_get_reboot_loop_detected() ? 1 : 0,
                         uptime_ms,
                         reset_reason_str,
                         static_cast<unsigned long>(heap_free),
                         static_cast<unsigned long>(heap_min),
                         static_cast<unsigned long>(heap_largest),
                         health,
                         health_flags[0] ? health_flags : "-",
                         lcd_ota_result && lcd_ota_result[0] ? lcd_ota_result : "-",
                         lcd_running_state && lcd_running_state[0] ? lcd_running_state : "UNKNOWN",
                         lcd_maint_ack ? 1 : 0,
                         static_cast<long>(lcd_maint_ack_age_s),
                         static_cast<long>(lcd_maint_ack_remaining_s),
                         static_cast<long>(lcd_maint_ack_wake_in_s),
                         lcd_maint_ack_request_id && lcd_maint_ack_request_id[0] ? lcd_maint_ack_request_id : "-",
                         lcd_maint_ack_status && lcd_maint_ack_status[0] ? lcd_maint_ack_status : "-",
                         lcd_maint_ack_persisted ? 1 : 0,
                         static_cast<unsigned long long>(lcd_maint_ack_start_epoch),
                         static_cast<unsigned long>(lcd_maint_ack_duration_sec),
                         static_cast<unsigned long>(lcd_maint_ack_grace_before_sec),
                         static_cast<unsigned long>(lcd_maint_ack_grace_after_sec),
                         maint_sync_pending ? 1 : 0,
                         static_cast<unsigned long>(maint_sync_attempts),
                         maint_sync_resolution && maint_sync_resolution[0] ? maint_sync_resolution : "-",
                         maint_last_tx_request_id && maint_last_tx_request_id[0] ? maint_last_tx_request_id : "-",
                         static_cast<long>(maint_last_tx_age_s),
                         static_cast<long>(maint_last_tx_remaining_s),
                         static_cast<long>(maint_last_tx_wake_in_s),
                         maint_last_tx_clear ? 1 : 0,
                         maint_last_tx_link_recent ? 1 : 0,
                         static_cast<unsigned long>(upload_cache_count),
                         upload_cache_last_result && upload_cache_last_result[0] ? upload_cache_last_result : "-",
                         upload_cache_last_reason && upload_cache_last_reason[0] ? upload_cache_last_reason : "-",
                         static_cast<long>(upload_cache_last_age_ms),
                         static_cast<unsigned long>(upload_cache_last_retries));
  return (written > 0 && static_cast<size_t>(written) < out_len);
}

bool build_diag_json(const char* reason, char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return false;
  }

  const char* fw_version = kFirmwareVersion;
  const char* build_id = kBuildId;

  wl_status_t wifi_status = WiFi.status();
  bool connected = false;
  if (wifi_is_connected) {
    connected = wifi_is_connected();
  } else {
    connected = (wifi_status == WL_CONNECTED);
  }
  const char* wifi_str = connected ? "connected" : "disconnected";

  char ip_str[16];
  truth_get_ip_str(ip_str, sizeof(ip_str), connected);
  int rssi = connected ? WiFi.RSSI() : 0;

  MqttMetrics mqtt_metrics = {};
  mqtt_get_metrics(&mqtt_metrics);
  const char* mqtt_state = mqtt_metrics.connected ? "connected" : "disconnected";
  const char* mqtt_prefix = mqtt_get_topic_prefix();
  const char* sched_fetch_state = truth_get_sched_fetch_state();
  int32_t sched_fetch_http = truth_get_sched_fetch_http_code();
  int32_t sched_fetch_age_s = truth_get_sched_fetch_age_s();
  const char* sched_fetch_request_id = truth_get_sched_fetch_request_id();
  const char* sched_last_event = truth_get_sched_last_event();
  int32_t sched_last_event_age_s = truth_get_sched_last_event_age_s();
  const char* sched_last_event_request_id = truth_get_sched_last_event_request_id();

  int wake_cause = truth_get_wake_cause();
  const char* wake_cause_str = truth_wake_cause_str(wake_cause);
  unsigned long uptime_ms = millis();
  esp_reset_reason_t reset_reason = esp_reset_reason();
  const char* reset_reason_str = truth_reset_reason_str(reset_reason);

  uint32_t uart_tx = truth_get_uart_tx_count();
  uint32_t uart_rx = truth_get_uart_rx_count();
  const char* last_tx = truth_get_last_uart_tx_type();
  const char* last_rx = truth_get_last_uart_rx_type();
  const char* last_action = truth_get_last_action();
  int32_t last_action_age_ms = truth_get_last_action_age_ms();

  const char* lcd_wake = truth_get_lcd_diag_wake();
  const char* lcd_screen = truth_get_lcd_diag_screen();
  const char* lcd_input = truth_get_lcd_diag_input();
  const char* lcd_last_tx = truth_get_lcd_diag_last_tx();
  const char* lcd_last_rx = truth_get_lcd_diag_last_rx();
  int32_t lcd_input_age_ms = truth_get_lcd_diag_input_age_ms();
  int32_t lcd_sense_rx_age_ms = truth_get_lcd_diag_sense_rx_age_ms();
  uint32_t lcd_uart_tx = truth_get_lcd_diag_uart_tx();
  uint32_t lcd_uart_rx = truth_get_lcd_diag_uart_rx();
  int32_t lcd_diag_age_ms = truth_get_lcd_diag_age_ms();
  const char* last_error_stage = truth_get_last_error_stage();
  int32_t last_error_code = truth_get_last_error_code();
  const char* last_error_text = truth_get_last_error_text();
  int32_t last_error_age_ms = truth_get_last_error_age_ms();
  const char* recent_actions = truth_get_recent_action_log();
  const char* camera_timeline = truth_get_camera_timeline();
  uint32_t upload_cache_count = truth_get_upload_cache_count();
  const char* upload_cache_last_result = truth_get_upload_cache_last_result();
  const char* upload_cache_last_reason = truth_get_upload_cache_last_reason();
  int32_t upload_cache_last_age_ms = truth_get_upload_cache_last_age_ms();
  uint32_t upload_cache_last_retries = truth_get_upload_cache_last_retries();
  bool lcd_maint_ack = truth_get_lcd_maint_ack();
  int32_t lcd_maint_ack_age_s = truth_get_lcd_maint_ack_age_s();
  int32_t lcd_maint_ack_remaining_s = truth_get_lcd_maint_ack_remaining_s();
  int32_t lcd_maint_ack_wake_in_s = truth_get_lcd_maint_ack_wake_in_s();
  const char* lcd_maint_ack_request_id = truth_get_lcd_maint_ack_request_id();
  const char* lcd_maint_ack_status = truth_get_lcd_maint_ack_status();
  bool lcd_maint_ack_persisted = truth_get_lcd_maint_ack_persisted();
  uint64_t lcd_maint_ack_start_epoch = truth_get_lcd_maint_ack_start_epoch();
  uint32_t lcd_maint_ack_duration_sec = truth_get_lcd_maint_ack_duration_sec();
  uint32_t lcd_maint_ack_grace_before_sec = truth_get_lcd_maint_ack_grace_before_sec();
  uint32_t lcd_maint_ack_grace_after_sec = truth_get_lcd_maint_ack_grace_after_sec();
  bool maint_sync_pending = truth_get_maint_sync_pending();
  uint32_t maint_sync_attempts = truth_get_maint_sync_attempts();
  const char* maint_sync_resolution = truth_get_maint_sync_resolution();
  const char* maint_last_tx_request_id = truth_get_maint_last_tx_request_id();
  int32_t maint_last_tx_age_s = truth_get_maint_last_tx_age_s();
  int32_t maint_last_tx_remaining_s = truth_get_maint_last_tx_remaining_s();
  int32_t maint_last_tx_wake_in_s = truth_get_maint_last_tx_wake_in_s();
  bool maint_last_tx_clear = truth_get_maint_last_tx_clear();
  bool maint_last_tx_link_recent = truth_get_maint_last_tx_link_recent();
  const char* last_stage = truth_get_last_stage();
  int32_t last_stage_code = truth_get_last_stage_code();
  int32_t last_stage_uptime_ms = truth_get_last_stage_uptime_ms();
  int32_t prev_clean_shutdown = truth_get_prev_clean_shutdown();
  uint32_t crash_count = truth_get_crash_count();
  const char* last_crash_stage = truth_get_last_crash_stage();
  int32_t last_crash_stage_code = truth_get_last_crash_stage_code();
  int32_t last_crash_stage_uptime_ms = truth_get_last_crash_stage_uptime_ms();
  int32_t last_crash_reason = truth_get_last_crash_reason();
  int32_t last_crash_wake_cause = truth_get_last_crash_wake_cause();
  const char* last_crash_reason_str = truth_reset_reason_str(static_cast<esp_reset_reason_t>(last_crash_reason));
  const char* last_crash_wake_cause_str = truth_wake_cause_str(last_crash_wake_cause);

  int written = snprintf(out, out_len,
                         "{\"reason\":\"%s\",\"fw\":\"%s\",\"build\":\"%s\","
                         "\"wake_cause\":%d,\"wake_cause_str\":\"%s\","
                         "\"reset_reason\":\"%s\",\"uptime_ms\":%lu,"
                         "\"wifi\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
                         "\"mqtt\":\"%s\",\"mqtt_fail_count\":%lu,"
                         "\"mqtt_last_pub_ms\":%lu,\"mqtt_prefix\":\"%s\","
                         "\"sched_fetch\":\"%s\",\"sched_fetch_http\":%ld,"
                         "\"sched_fetch_age_s\":%ld,\"sched_fetch_request_id\":\"%s\","
                         "\"sched_last_event\":\"%s\",\"sched_last_event_age_s\":%ld,"
                         "\"sched_last_event_request_id\":\"%s\","
                         "\"lcd_maint_ack\":%d,\"lcd_maint_ack_age_s\":%ld,"
                         "\"lcd_maint_ack_remaining_s\":%ld,\"lcd_maint_ack_wake_in_s\":%ld,"
                         "\"lcd_maint_ack_request_id\":\"%s\",\"lcd_maint_ack_status\":\"%s\","
                         "\"lcd_maint_ack_persisted\":%d,"
                         "\"lcd_maint_ack_start_epoch\":%llu,\"lcd_maint_ack_duration_sec\":%lu,"
                         "\"lcd_maint_ack_grace_before_sec\":%lu,\"lcd_maint_ack_grace_after_sec\":%lu,"
                         "\"maint_sync_pending\":%d,\"maint_sync_attempts\":%lu,"
                         "\"maint_sync_resolution\":\"%s\","
                         "\"maint_last_tx_request_id\":\"%s\",\"maint_last_tx_age_s\":%ld,"
                         "\"maint_last_tx_remaining_s\":%ld,\"maint_last_tx_wake_in_s\":%ld,"
                         "\"maint_last_tx_clear\":%d,\"maint_last_tx_link_recent\":%d,"
                         "\"uart_tx\":%lu,\"uart_rx\":%lu,"
                         "\"last_tx\":\"%s\",\"last_rx\":\"%s\","
                         "\"last_action\":\"%s\",\"last_action_age_ms\":%ld,"
                         "\"lcd_wake\":\"%s\",\"lcd_screen\":\"%s\",\"lcd_input\":\"%s\","
                         "\"lcd_input_age_ms\":%ld,\"lcd_uart_tx\":%lu,\"lcd_uart_rx\":%lu,"
                         "\"lcd_last_tx\":\"%s\",\"lcd_last_rx\":\"%s\","
                         "\"lcd_sense_rx_age_ms\":%ld,\"lcd_diag_age_ms\":%ld,"
                         "\"last_error_stage\":\"%s\",\"last_error_code\":%ld,"
                         "\"last_error_text\":\"%s\",\"last_error_age_ms\":%ld,"
                         "\"last_stage\":\"%s\",\"last_stage_code\":%ld,\"last_stage_uptime_ms\":%ld,"
                         "\"last_shutdown_clean\":%ld,\"crash_count\":%lu,"
                         "\"last_crash_reason\":\"%s\",\"last_crash_reason_code\":%ld,"
                         "\"last_crash_wake_cause\":\"%s\",\"last_crash_wake_cause_code\":%ld,"
                         "\"last_crash_stage\":\"%s\",\"last_crash_stage_code\":%ld,"
                         "\"last_crash_stage_uptime_ms\":%ld,"
                         "\"camera_timeline\":\"%s\","
                         "\"upload_cache_count\":%lu,\"upload_cache_last_result\":\"%s\","
                         "\"upload_cache_last_reason\":\"%s\",\"upload_cache_last_age_ms\":%ld,"
                         "\"upload_cache_last_retries\":%lu,"
                         "\"recent_actions\":\"%s\"}",
                         reason ? reason : "",
                         fw_version ? fw_version : "",
                         build_id ? build_id : "",
                         wake_cause,
                         wake_cause_str ? wake_cause_str : "",
                         reset_reason_str ? reset_reason_str : "",
                         uptime_ms,
                         wifi_str ? wifi_str : "",
                         ip_str,
                         rssi,
                         mqtt_state ? mqtt_state : "",
                         static_cast<unsigned long>(mqtt_metrics.fail_count),
                         static_cast<unsigned long>(mqtt_metrics.last_pub_ms),
                         mqtt_prefix ? mqtt_prefix : "",
                         sched_fetch_state && sched_fetch_state[0] ? sched_fetch_state : "-",
                         static_cast<long>(sched_fetch_http),
                         static_cast<long>(sched_fetch_age_s),
                         sched_fetch_request_id && sched_fetch_request_id[0] ? sched_fetch_request_id : "-",
                         sched_last_event && sched_last_event[0] ? sched_last_event : "-",
                         static_cast<long>(sched_last_event_age_s),
                         sched_last_event_request_id && sched_last_event_request_id[0] ? sched_last_event_request_id : "-",
                         lcd_maint_ack ? 1 : 0,
                         static_cast<long>(lcd_maint_ack_age_s),
                         static_cast<long>(lcd_maint_ack_remaining_s),
                         static_cast<long>(lcd_maint_ack_wake_in_s),
                         lcd_maint_ack_request_id && lcd_maint_ack_request_id[0] ? lcd_maint_ack_request_id : "-",
                         lcd_maint_ack_status && lcd_maint_ack_status[0] ? lcd_maint_ack_status : "-",
                         lcd_maint_ack_persisted ? 1 : 0,
                         static_cast<unsigned long long>(lcd_maint_ack_start_epoch),
                         static_cast<unsigned long>(lcd_maint_ack_duration_sec),
                         static_cast<unsigned long>(lcd_maint_ack_grace_before_sec),
                         static_cast<unsigned long>(lcd_maint_ack_grace_after_sec),
                         maint_sync_pending ? 1 : 0,
                         static_cast<unsigned long>(maint_sync_attempts),
                         maint_sync_resolution && maint_sync_resolution[0] ? maint_sync_resolution : "-",
                         maint_last_tx_request_id && maint_last_tx_request_id[0] ? maint_last_tx_request_id : "-",
                         static_cast<long>(maint_last_tx_age_s),
                         static_cast<long>(maint_last_tx_remaining_s),
                         static_cast<long>(maint_last_tx_wake_in_s),
                         maint_last_tx_clear ? 1 : 0,
                         maint_last_tx_link_recent ? 1 : 0,
                         static_cast<unsigned long>(uart_tx),
                         static_cast<unsigned long>(uart_rx),
                         last_tx ? last_tx : "",
                         last_rx ? last_rx : "",
                         last_action ? last_action : "",
                         static_cast<long>(last_action_age_ms),
                         lcd_wake ? lcd_wake : "",
                         lcd_screen ? lcd_screen : "",
                         lcd_input ? lcd_input : "",
                         static_cast<long>(lcd_input_age_ms),
                         static_cast<unsigned long>(lcd_uart_tx),
                         static_cast<unsigned long>(lcd_uart_rx),
                         lcd_last_tx ? lcd_last_tx : "",
                         lcd_last_rx ? lcd_last_rx : "",
                         static_cast<long>(lcd_sense_rx_age_ms),
                         static_cast<long>(lcd_diag_age_ms),
                         last_error_stage ? last_error_stage : "",
                         static_cast<long>(last_error_code),
                         last_error_text ? last_error_text : "",
                         static_cast<long>(last_error_age_ms),
                         last_stage ? last_stage : "",
                         static_cast<long>(last_stage_code),
                         static_cast<long>(last_stage_uptime_ms),
                         static_cast<long>(prev_clean_shutdown),
                         static_cast<unsigned long>(crash_count),
                         last_crash_reason_str ? last_crash_reason_str : "",
                         static_cast<long>(last_crash_reason),
                         last_crash_wake_cause_str ? last_crash_wake_cause_str : "",
                         static_cast<long>(last_crash_wake_cause),
                         last_crash_stage ? last_crash_stage : "",
                         static_cast<long>(last_crash_stage_code),
                         static_cast<long>(last_crash_stage_uptime_ms),
                         camera_timeline ? camera_timeline : "",
                         static_cast<unsigned long>(upload_cache_count),
                         upload_cache_last_result && upload_cache_last_result[0] ? upload_cache_last_result : "-",
                         upload_cache_last_reason && upload_cache_last_reason[0] ? upload_cache_last_reason : "-",
                         static_cast<long>(upload_cache_last_age_ms),
                         static_cast<unsigned long>(upload_cache_last_retries),
                         recent_actions ? recent_actions : "");
  return (written > 0 && static_cast<size_t>(written) < out_len);
}

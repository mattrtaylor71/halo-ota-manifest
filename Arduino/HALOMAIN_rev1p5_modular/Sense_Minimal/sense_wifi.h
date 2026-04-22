/*
 * sense_wifi.h
 *
 * WiFi guard state machine, connection management, time sync,
 * and recovery functions for Sense_Minimal.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 5.
 *
 * Prerequisites (must be declared before #include "sense_wifi.h"):
 *   - WiFi.h, esp_wifi.h, freertos/semphr.h
 *   - wifi_connect_mutex (SemaphoreHandle_t)
 *   - WIFI_SSID, WIFI_PASS (const char*)
 *   - ACTION_MIN_REMAINING_MS (uint32_t)
 *   - sntp_started (bool), TIME_VALID_MIN_EPOCH (time_t)
 *   - time_cache_bootstrap(), time_cache_store() from .ino
 *   - http_inflight, wifi_recover_requested globals
 *   - uart_send_sense_diag() from sense_uart.h
 *   - apply_public_dns_for_api(), log_wifi_snapshot() from sense_http.h
 *   - hardResetSta() from WifiUtils.h
 */

#ifndef SENSE_WIFI_H
#define SENSE_WIFI_H

// ── Wi-Fi Functions ────────────────────────────────────────────────
bool wifi_is_connected() {
  return WiFi.status() == WL_CONNECTED;
}

enum WifiGuardState {
  WIFI_STATE_DISCONNECTED = 0,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_CONNECTED,
  WIFI_STATE_FAILED,
  WIFI_STATE_FAILED_TIMEOUT
};

static volatile bool wifi_connect_inflight = false;
static unsigned long wifi_inflight_start_ms = 0;
static unsigned long wifi_last_begin_ms = 0;
static unsigned long wifi_connected_ms = 0;
static unsigned long wifi_last_poll_ms = 0;
static IPAddress wifi_last_ip;
static int wifi_last_rssi = 0;
static WifiGuardState wifi_state = WIFI_STATE_DISCONNECTED;
static const char* wifi_truth_state = "disconnected";
static unsigned long last_wifi_fail_ms = 0;
static char last_wifi_fail_reason[24] = "";
static char wifi_connect_owner[24] = "";
static unsigned long wifi_last_scan_dump_ms = 0;

// ── WiFi constants ──────────────────────────────────────────────────

static const unsigned long WIFI_BEGIN_COOLDOWN_MS = 2000;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 25000;
static const unsigned long WIFI_POLL_INTERVAL_MS = 150;
static const unsigned long WIFI_FAIL_COOLDOWN_MS = 0UL;  /* no cooldown: retry every pre_sleep so bad wifi can recover */

// ── WiFi guard state helpers ────────────────────────────────────────

static const char* wifi_state_to_string(WifiGuardState state) {
  switch (state) {
    case WIFI_STATE_DISCONNECTED: return "disconnected";
    case WIFI_STATE_CONNECTING: return "connecting";
    case WIFI_STATE_CONNECTED: return "connected";
    case WIFI_STATE_FAILED: return "failed";
    case WIFI_STATE_FAILED_TIMEOUT: return "failed_timeout";
    default: return "unknown";
  }
}

static const char* wifi_state_to_truth(WifiGuardState state) {
  switch (state) {
    case WIFI_STATE_CONNECTED: return "connected";
    case WIFI_STATE_FAILED:
    case WIFI_STATE_FAILED_TIMEOUT:
      return "failed";
    case WIFI_STATE_CONNECTING:
    case WIFI_STATE_DISCONNECTED:
    default:
      return "disconnected";
  }
}

// ── WiFi guard core functions ───────────────────────────────────────

static void wifi_guard_set_state(WifiGuardState new_state,
                                 const char* reason,
                                 wl_status_t status) {
  if (wifi_state == new_state) {
    wifi_truth_state = wifi_state_to_truth(new_state);
    return;
  }
  wifi_state = new_state;
  wifi_truth_state = wifi_state_to_truth(new_state);
  if (new_state == WIFI_STATE_CONNECTED) {
    last_wifi_fail_ms = 0;
    last_wifi_fail_reason[0] = '\0';
  }
  if (new_state == WIFI_STATE_CONNECTED) {
    wifi_last_ip = WiFi.localIP();
    wifi_last_rssi = WiFi.RSSI();
  }
  Serial.printf("[WIFI_GUARD] state=%s truth=%s status=%d reason=%s\n",
                wifi_state_to_string(new_state),
                wifi_truth_state,
                (int)status,
                reason ? reason : "unknown");
}

static unsigned long wifi_last_fail_ms() {
  return last_wifi_fail_ms;
}

static const char* wifi_last_fail_reason() {
  return last_wifi_fail_reason[0] ? last_wifi_fail_reason : "none";
}

extern "C" const char* halo_wifi_guard_truth_state() {
  return wifi_truth_state;
}

static void wifi_guard_set_inflight(bool inflight) {
  wifi_connect_inflight = inflight;
  if (!inflight) {
    wifi_inflight_start_ms = 0;
    wifi_connect_owner[0] = '\0';
  }
}

static const char* wifi_guard_connect_owner() {
  return wifi_connect_owner[0] ? wifi_connect_owner : "none";
}

static bool wifi_guard_try_claim_connect(const char* owner) {
  if (wifi_connect_mutex != NULL) {
    if (xSemaphoreTake(wifi_connect_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
      return false;
    }
  }

  bool claimed = false;
  if (!wifi_connect_inflight) {
    claimed = true;
    wifi_connect_inflight = true;
    wifi_inflight_start_ms = millis();
    wifi_last_begin_ms = wifi_inflight_start_ms;
    if (owner && owner[0]) {
      strncpy(wifi_connect_owner, owner, sizeof(wifi_connect_owner) - 1);
      wifi_connect_owner[sizeof(wifi_connect_owner) - 1] = '\0';
    } else {
      strncpy(wifi_connect_owner, "unknown", sizeof(wifi_connect_owner) - 1);
      wifi_connect_owner[sizeof(wifi_connect_owner) - 1] = '\0';
    }
  }

  if (wifi_connect_mutex != NULL) {
    xSemaphoreGive(wifi_connect_mutex);
  }
  return claimed;
}

static void wifi_guard_note_fail(const char* reason) {
  last_wifi_fail_ms = millis();
  if (reason && reason[0]) {
    strncpy(last_wifi_fail_reason, reason, sizeof(last_wifi_fail_reason) - 1);
    last_wifi_fail_reason[sizeof(last_wifi_fail_reason) - 1] = '\0';
  } else {
    strncpy(last_wifi_fail_reason, "unknown", sizeof(last_wifi_fail_reason) - 1);
    last_wifi_fail_reason[sizeof(last_wifi_fail_reason) - 1] = '\0';
  }
}

static void wifi_dump_scan(const char* reason) {
  unsigned long now = millis();
  if (wifi_last_scan_dump_ms > 0 && (now - wifi_last_scan_dump_ms) < 30000) {
    return;
  }
  wifi_last_scan_dump_ms = now;
  int count = WiFi.scanNetworks();
  Serial.printf("[WIFI_SCAN] reason=%s count=%d\n", reason ? reason : "unknown", count);
  if (count <= 0) {
    return;
  }
  for (int i = 0; i < count; ++i) {
    String ssid = WiFi.SSID(i);
    int32_t rssi = WiFi.RSSI(i);
    int32_t chan = WiFi.channel(i);
    Serial.printf("[WIFI_SCAN] %02d ssid=\"%s\" rssi=%d chan=%d\n",
                  i, ssid.c_str(), (int)rssi, (int)chan);
  }
  WiFi.scanDelete();
}

static void wifi_guard_handle_timeout(unsigned long elapsed_ms) {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  wifi_guard_set_inflight(false);
  wifi_guard_set_state(WIFI_STATE_FAILED_TIMEOUT, "timeout", WiFi.status());
  wifi_guard_note_fail("timeout");
  Serial.printf("[WIFI_GUARD] connect_timeout elapsed_ms=%lu\n", elapsed_ms);
  uart_send_sense_diag("wifi", "timeout", "timeout", (int32_t)WiFi.status(), "connect_timeout");
}

static void wifi_guard_mark_failed(wl_status_t status, const char* reason) {
  wifi_guard_set_inflight(false);
  wifi_guard_set_state(WIFI_STATE_FAILED, reason, status);
  wifi_guard_note_fail(reason);
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL || status == WL_DISCONNECTED) {
    wifi_dump_scan(reason);
  }
  Serial.printf("[WIFI_GUARD] connect_fail status=%d reason=%s\n",
                (int)status,
                reason ? reason : "unknown");
  uart_send_sense_diag("wifi", "fail", reason, (int32_t)status, wifi_state_to_string(wifi_state));
}

static void wifi_guard_poll() {
  if (!wifi_connect_inflight) {
    return;
  }
  unsigned long now = millis();
  if ((now - wifi_last_poll_ms) < WIFI_POLL_INTERVAL_MS) {
    return;
  }
  wifi_last_poll_ms = now;
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = now;
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "poll", status);
    Serial.printf("[WIFI_GUARD] connect_ok ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    return;
  }
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    if (wifi_inflight_start_ms > 0 &&
        (now - wifi_inflight_start_ms) > WIFI_CONNECT_TIMEOUT_MS) {
      wifi_guard_mark_failed(status, "poll");
    }
    return;
  }
  if (status == WL_DISCONNECTED || status == WL_CONNECTION_LOST) {
    if (wifi_inflight_start_ms > 0 &&
        (now - wifi_inflight_start_ms) > WIFI_CONNECT_TIMEOUT_MS) {
      wifi_guard_mark_failed(status, "poll");
    }
    return;
  }
  if (wifi_inflight_start_ms > 0 &&
      (now - wifi_inflight_start_ms) > WIFI_CONNECT_TIMEOUT_MS) {
    wifi_guard_handle_timeout(now - wifi_inflight_start_ms);
  }
}

// ── WiFi event handler ──────────────────────────────────────────────

static void handle_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.printf("[WIFI_EVENT] event=%d t=%lu status=%d inflight=%d state=%d\n",
                (int)event,
                millis(),
                (int)WiFi.status(),
                wifi_connect_inflight ? 1 : 0,
                (int)wifi_state);
#if defined(ARDUINO_EVENT_WIFI_STA_GOT_IP)
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = millis();
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "event_got_ip", WL_CONNECTED);
    Serial.printf("[WIFI_GUARD] connect_ok ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    String ip = WiFi.localIP().toString();
    uart_send_sense_diag("wifi", "got_ip", "event_got_ip", (int32_t)WiFi.RSSI(), ip.c_str());
    apply_public_dns_for_api("wifi_got_ip");
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wl_status_t status = WiFi.status();
    if (wifi_connect_inflight) {
      wifi_guard_mark_failed(status, "event_disconnect");
    } else if (wifi_state == WIFI_STATE_FAILED || wifi_state == WIFI_STATE_FAILED_TIMEOUT) {
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(wifi_state, "event_disconnect", status);
    } else {
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "event_disconnect", status);
    }
  }
#elif defined(SYSTEM_EVENT_STA_GOT_IP)
  if (event == SYSTEM_EVENT_STA_GOT_IP) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = millis();
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "event_got_ip", WL_CONNECTED);
    Serial.printf("[WIFI_GUARD] connect_ok ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    String ip = WiFi.localIP().toString();
    uart_send_sense_diag("wifi", "got_ip", "event_got_ip", (int32_t)WiFi.RSSI(), ip.c_str());
    apply_public_dns_for_api("wifi_got_ip");
  } else if (event == SYSTEM_EVENT_STA_DISCONNECTED) {
    wl_status_t status = WiFi.status();
    if (wifi_connect_inflight) {
      wifi_guard_mark_failed(status, "event_disconnect");
    } else if (wifi_state == WIFI_STATE_FAILED || wifi_state == WIFI_STATE_FAILED_TIMEOUT) {
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(wifi_state, "event_disconnect", status);
    } else {
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "event_disconnect", status);
    }
  }
#endif
  (void)info;
}

// ── WiFi connection functions ───────────────────────────────────────

static bool ensure_wifi_connected(const char* reason, uint32_t timeout_ms) {
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active()) {
    Serial.println("[WIFI] Provisioning active - skipping Wi-Fi connect");
    wifi_guard_set_inflight(false);
    wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "provisioning_active", WiFi.status());
    return false;
  }
#endif
  if (wifi_is_connected()) {
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "already_connected", WL_CONNECTED);
    return true;
  }
  unsigned long now = millis();
  wifi_guard_poll();
  if (wifi_connect_inflight && wifi_inflight_start_ms > 0) {
    unsigned long inflight_elapsed = now - wifi_inflight_start_ms;
    if (reason && strcmp(reason, "pre_sleep") == 0 &&
        inflight_elapsed < WIFI_CONNECT_TIMEOUT_MS) {
      unsigned long wait_start = millis();
      while (wifi_connect_inflight && (millis() - wait_start) < 2000) {
        wifi_guard_poll();
        if (WiFi.status() == WL_CONNECTED) {
          return true;
        }
        delay(100);
      }
      if (!wifi_connect_inflight && WiFi.status() == WL_CONNECTED) {
        return true;
      }
    }
    Serial.printf("[WIFI_GUARD] skip_begin inflight=1 reason=wifi_inflight caller=%s\n",
                  reason ? reason : "unknown");
    return false;
  }
  bool bypass_cooldown = false;
  if (reason && (strcmp(reason, "maintenance") == 0 ||
                 strcmp(reason, "pre_sleep") == 0 ||
                 strcmp(reason, "manual_ota") == 0 ||
                 strcmp(reason, "wifi_connect") == 0 ||
                 strcmp(reason, "voice_upload") == 0)) {
    bypass_cooldown = true;
  }
  if (!bypass_cooldown && wifi_last_begin_ms > 0 &&
      (now - wifi_last_begin_ms) < WIFI_BEGIN_COOLDOWN_MS) {
    Serial.printf("[WIFI_GUARD] skip_begin cooldown_ms=%lu reason=%s\n",
                  (unsigned long)(now - wifi_last_begin_ms), reason ? reason : "unknown");
    return false;
  }

  const char* ssid = WIFI_SSID;
  const char* pass = WIFI_PASS;
#ifdef HALO_SENSE_PROD_WRAPPER
  char provision_ssid[64];
  char provision_pass[64];
  if (halo_get_provisioned_wifi(provision_ssid, sizeof(provision_ssid),
                                provision_pass, sizeof(provision_pass))) {
    ssid = provision_ssid;
    pass = provision_pass;
  }
#endif
  if (!ssid || !ssid[0]) {
    Serial.println("[WIFI_GUARD] no_ssid - skipping Wi-Fi connect");
    wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "no_ssid", WiFi.status());
    return false;
  }

  if (!wifi_guard_try_claim_connect(reason ? reason : "unknown")) {
    Serial.printf("[WIFI_GUARD] skip_begin inflight=1 reason=claim_busy caller=%s owner=%s\n",
                  reason ? reason : "unknown",
                  wifi_guard_connect_owner());
    if (timeout_ms > 0) {
      unsigned long wait_start = millis();
      while (wifi_connect_inflight &&
             WiFi.status() != WL_CONNECTED &&
             (millis() - wait_start) < timeout_ms) {
        wifi_guard_poll();
        delay(100);
      }
      if (WiFi.status() == WL_CONNECTED) {
        wifi_guard_set_inflight(false);
        wifi_connected_ms = millis();
        wifi_guard_set_state(WIFI_STATE_CONNECTED, "claim_wait_connected", WL_CONNECTED);
        return true;
      }
    }
    return false;
  }

  Serial.printf("[WIFI_GUARD] begin_connect inflight=1 reason=%s owner=%s ssid=%s\n",
                reason ? reason : "unknown",
                wifi_guard_connect_owner(),
                ssid);
  uart_send_sense_diag("wifi", "begin", reason, (int32_t)WiFi.status(), wifi_guard_connect_owner());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(50);
  WiFi.begin(ssid, pass);
  wifi_guard_set_state(WIFI_STATE_CONNECTING, "begin_connect", WiFi.status());

  if (timeout_ms == 0) {
    return false;
  }
  bool allow_retry = false;
  if (reason && (strcmp(reason, "maintenance") == 0 ||
                 strcmp(reason, "pre_sleep") == 0 ||
                 strcmp(reason, "manual_ota") == 0 ||
                 strcmp(reason, "wifi_connect") == 0 ||
                 strcmp(reason, "voice_upload") == 0)) {
    allow_retry = true;
  }
  unsigned long next_retry_ms = 0;
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
    wifi_guard_poll();
    if (!wifi_connect_inflight && WiFi.status() != WL_CONNECTED) {
      if (allow_retry) {
        unsigned long now_ms = millis();
        if (now_ms >= next_retry_ms) {
          hardResetSta();
          WiFi.setAutoReconnect(true);
          WiFi.setSleep(false);
          esp_wifi_set_ps(WIFI_PS_NONE);
          WiFi.begin(ssid, pass);
          wifi_connect_inflight = true;
          wifi_inflight_start_ms = millis();
          wifi_last_begin_ms = wifi_inflight_start_ms;
          wifi_guard_set_state(WIFI_STATE_CONNECTING, "retry_connect", WiFi.status());
          next_retry_ms = now_ms + 1000;
        }
      } else {
      break;
      }
    }
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = millis();
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "wait_connected", WL_CONNECTED);
    Serial.println("\nWi-Fi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    String ip = WiFi.localIP().toString();
    uart_send_sense_diag("wifi", "connected", "wait_connected", (int32_t)WiFi.RSSI(), ip.c_str());
    apply_public_dns_for_api("wifi_connected");
    return true;
  }
  if (wifi_connect_inflight) {
    wifi_guard_mark_failed(WiFi.status(), "wait_timeout");
  }
  return false;
}

static bool wifi_connect() {
  unsigned long start_ms = millis();
  Serial.printf("[BOOT_FLOW] stage=wifi_connect_begin t=%lu status=%d inflight=%d state=%d\n",
                start_ms,
                (int)WiFi.status(),
                wifi_connect_inflight ? 1 : 0,
                (int)wifi_state);
  Serial.print("Connecting to Wi-Fi: ");
  const char* ssid = WIFI_SSID;
#ifdef HALO_SENSE_PROD_WRAPPER
  char provision_ssid[64];
  char provision_pass[64];
  if (halo_get_provisioned_wifi(provision_ssid, sizeof(provision_ssid),
                                provision_pass, sizeof(provision_pass))) {
    ssid = provision_ssid;
  }
#endif
  Serial.println(ssid ? ssid : "(null)");
  if (ensure_wifi_connected("wifi_connect", 15000)) {
    Serial.printf("[BOOT_FLOW] stage=wifi_connect_ok t=%lu elapsed_ms=%lu status=%d inflight=%d state=%d\n",
                  millis(),
                  millis() - start_ms,
                  (int)WiFi.status(),
                  wifi_connect_inflight ? 1 : 0,
                  (int)wifi_state);
    return true;
  }
  Serial.printf("[BOOT_FLOW] stage=wifi_connect_fail t=%lu elapsed_ms=%lu status=%d inflight=%d state=%d\n",
                millis(),
                millis() - start_ms,
                (int)WiFi.status(),
                wifi_connect_inflight ? 1 : 0,
                (int)wifi_state);
  Serial.println("\nWi-Fi connection failed!");
  return false;
}

// ── WiFi background maintenance ────────────────────────────────────

static unsigned long wifi_maint_last_attempt_ms = 0;
static unsigned long wifi_maint_last_log_ms = 0;
static uint8_t wifi_maint_consecutive_fails = 0;
static const uint8_t WIFI_MAINT_MAX_FAILS_BEFORE_RESET = 3;
static const unsigned long WIFI_MAINT_LOG_INTERVAL_MS = 5000;

static void service_wifi_maintenance(unsigned long now_ms) {
  // Already connected — nothing to do
  if (WiFi.status() == WL_CONNECTED) {
    wifi_maint_consecutive_fails = 0;
    return;
  }

  // Connection attempt in flight — monitor for timeout
  if (wifi_connect_inflight) {
    if (wifi_inflight_start_ms > 0 &&
        (now_ms - wifi_inflight_start_ms) > WIFI_CONNECT_TIMEOUT_MS) {
      // Timed out — hard reset and retry
      Serial.printf("[WIFI_MAINT] connect_timeout elapsed_ms=%lu fails=%u -> hard_reset\n",
                    (unsigned long)(now_ms - wifi_inflight_start_ms),
                    (unsigned)wifi_maint_consecutive_fails);
      wifi_maint_consecutive_fails++;
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(WIFI_STATE_FAILED_TIMEOUT, "maint_timeout", WiFi.status());
      wifi_guard_note_fail("maint_timeout");
      // Immediately retry with hard reset if under fail limit
      if (wifi_maint_consecutive_fails <= WIFI_MAINT_MAX_FAILS_BEFORE_RESET) {
        hardResetSta();
        delay(50);
        const char* ssid = WIFI_SSID;
        const char* pass = WIFI_PASS;
#ifdef HALO_SENSE_PROD_WRAPPER
        char provision_ssid[64];
        char provision_pass[64];
        if (halo_get_provisioned_wifi(provision_ssid, sizeof(provision_ssid),
                                      provision_pass, sizeof(provision_pass))) {
          ssid = provision_ssid;
          pass = provision_pass;
        }
#endif
        if (ssid && ssid[0]) {
          WiFi.mode(WIFI_STA);
          WiFi.setAutoReconnect(true);
          WiFi.setSleep(false);
          esp_wifi_set_ps(WIFI_PS_NONE);
          WiFi.begin(ssid, pass);
          wifi_connect_inflight = true;
          wifi_inflight_start_ms = now_ms;
          wifi_last_begin_ms = now_ms;
          wifi_maint_last_attempt_ms = now_ms;
          wifi_guard_set_state(WIFI_STATE_CONNECTING, "maint_hard_reset_retry", WiFi.status());
          Serial.printf("[WIFI_MAINT] hard_reset_retry fails=%u\n",
                        (unsigned)wifi_maint_consecutive_fails);
        }
      }
    }
    return;
  }

  // Not connected, not inflight — respect cooldown then start a new attempt
  unsigned long cooldown_ms = WIFI_BEGIN_COOLDOWN_MS;
  // Back off after consecutive failures: 2s, 4s, 8s (capped)
  if (wifi_maint_consecutive_fails > 0) {
    cooldown_ms = WIFI_BEGIN_COOLDOWN_MS << (wifi_maint_consecutive_fails < 3 ? wifi_maint_consecutive_fails : 3);
    if (cooldown_ms > 16000) cooldown_ms = 16000;
  }
  if (wifi_maint_last_attempt_ms > 0 &&
      (now_ms - wifi_maint_last_attempt_ms) < cooldown_ms) {
    // Log periodically while waiting
    if (wifi_maint_last_log_ms == 0 ||
        (now_ms - wifi_maint_last_log_ms) >= WIFI_MAINT_LOG_INTERVAL_MS) {
      Serial.printf("[WIFI_MAINT] waiting cooldown_ms=%lu fails=%u next_ms=%lu\n",
                    cooldown_ms,
                    (unsigned)wifi_maint_consecutive_fails,
                    (unsigned long)(wifi_maint_last_attempt_ms + cooldown_ms));
      wifi_maint_last_log_ms = now_ms;
    }
    return;
  }

  // Start a non-blocking connection attempt
  const char* ssid = WIFI_SSID;
  const char* pass = WIFI_PASS;
#ifdef HALO_SENSE_PROD_WRAPPER
  char provision_ssid[64];
  char provision_pass[64];
  if (halo_get_provisioned_wifi(provision_ssid, sizeof(provision_ssid),
                                provision_pass, sizeof(provision_pass))) {
    ssid = provision_ssid;
    pass = provision_pass;
  }
#endif
  if (!ssid || !ssid[0]) {
    return;  // No SSID configured
  }

#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active()) {
    return;  // Don't interfere with provisioning
  }
#endif

  if (!wifi_guard_try_claim_connect("wifi_maint")) {
    return;  // Someone else owns the connection attempt
  }

  wifi_maint_last_attempt_ms = now_ms;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(20);
  WiFi.begin(ssid, pass);
  wifi_guard_set_state(WIFI_STATE_CONNECTING, "maint_begin", WiFi.status());
  Serial.printf("[WIFI_MAINT] begin_connect fails=%u cooldown=%lu ssid=%s\n",
                (unsigned)wifi_maint_consecutive_fails,
                cooldown_ms,
                ssid);
}

// ── Time synchronization ────────────────────────────────────────────

static bool ensure_time_valid(const char* reason, uint32_t timeout_ms) {
  // Sense_Minimal/Sense_Minimal.ino: ensure_time_valid
  time_t now = time(nullptr);
  if (now >= TIME_VALID_MIN_EPOCH) {
    Serial.printf("[TLS_GUARD] time_valid epoch=%ld\n", (long)now);
    time_cache_store(now);
    return true;
  }
  if (time_cache_bootstrap(reason)) {
    now = time(nullptr);
    if (now >= TIME_VALID_MIN_EPOCH) {
      Serial.printf("[TLS_GUARD] time_valid epoch=%ld\n", (long)now);
      time_cache_store(now);
      return true;
    }
  }
  if (!sntp_started) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    sntp_started = true;
    Serial.printf("[TLS_GUARD] SNTP init reason=%s\n", reason ? reason : "unknown");
  }
  unsigned long start = millis();
  unsigned long wait_ms = timeout_ms ? timeout_ms : 15000;
  if (wait_ms < ACTION_MIN_REMAINING_MS) {
    Serial.printf("[TLS_GUARD] time_wait_budget_too_low wait_ms=%lu\n", wait_ms);
    return false;
  }
  while ((millis() - start) < wait_ms) {
    now = time(nullptr);
    if (now >= TIME_VALID_MIN_EPOCH) {
      Serial.printf("[TLS_GUARD] time_valid epoch=%ld\n", (long)now);
      time_cache_store(now);
      return true;
    }
    delay(250);
  }
  Serial.printf("[TLS_GUARD] time_invalid epoch=%ld\n", (long)now);
  return false;
}

// ── WiFi readiness & recovery ───────────────────────────────────────

// Forward declaration - wifi_hard_reset_and_reconnect defined below
static bool wifi_hard_reset_and_reconnect(const char* reason, uint32_t timeout_ms);

static bool ensure_wifi_ready(const char* reason, uint32_t timeout_ms) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  if (ensure_wifi_connected(reason, timeout_ms)) {
    log_wifi_snapshot("wifi_ready");
    return true;
  }
  uint32_t reconnect_timeout_ms = timeout_ms;
  if (reconnect_timeout_ms < 4000) {
    reconnect_timeout_ms = 4000;
  }
  Serial.printf("[WIFI_READY] escalating hard reset reason=%s timeout_ms=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)reconnect_timeout_ms);
  if (wifi_hard_reset_and_reconnect(reason ? reason : "wifi_ready_recover", reconnect_timeout_ms)) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = millis();
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "hard_reset_reconnect", WL_CONNECTED);
    log_wifi_snapshot("wifi_ready_recovered");
    return true;
  }
  wifi_guard_set_inflight(false);
  return false;
}

static bool wifi_hard_reset_and_reconnect(const char* reason, uint32_t timeout_ms) {
  if (wifi_connect_inflight) {
    Serial.printf("[WIFI_RECOVER] force_reset reason=%s owner=%s (was inflight)\n",
                  reason ? reason : "unknown",
                  wifi_guard_connect_owner());
    wifi_guard_set_inflight(false);
  }

  const char* ssid = WIFI_SSID;
  const char* pass = WIFI_PASS;
#ifdef HALO_SENSE_PROD_WRAPPER
  char provision_ssid[64];
  char provision_pass[64];
  if (halo_get_provisioned_wifi(provision_ssid, sizeof(provision_ssid),
                                provision_pass, sizeof(provision_pass))) {
    ssid = provision_ssid;
    pass = provision_pass;
  }
#endif
  if (!ssid || !ssid[0]) {
    Serial.println("[WIFI_RECOVER] no_ssid");
    return false;
  }
  Serial.printf("[WIFI_RECOVER] hard_reset reason=%s ssid=%s\n",
                reason ? reason : "unknown",
                ssid);
  uart_send_sense_diag("wifi", "hard_reset_begin", reason, (int32_t)WiFi.status(), "wifi_recover");
  if (!wifi_guard_try_claim_connect(reason ? reason : "wifi_recover")) {
    Serial.printf("[WIFI_RECOVER] claim_busy reason=%s owner=%s\n",
                  reason ? reason : "unknown",
                  wifi_guard_connect_owner());
    return ensure_wifi_connected(reason ? reason : "wifi_recover_busy", timeout_ms);
  }
  hardResetSta();
  WiFi.begin(ssid, pass);
  wifi_guard_set_state(WIFI_STATE_CONNECTING, "hard_reset_begin", WiFi.status());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
    wifi_guard_poll();
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifi_guard_set_inflight(false);
    log_wifi_snapshot("wifi_recovered");
    String ip = WiFi.localIP().toString();
    uart_send_sense_diag("wifi", "hard_reset_ok", reason, (int32_t)WiFi.RSSI(), ip.c_str());
    return true;
  }
  wifi_guard_mark_failed(WiFi.status(), "hard_reset_timeout");
  Serial.printf("[WIFI_RECOVER] failed status=%d\n", (int)WiFi.status());
  uart_send_sense_diag("wifi", "hard_reset_fail", reason, (int32_t)WiFi.status(), "hard_reset_timeout");
  return false;
}

static void wifi_recover_if_needed(const char* reason, int http_code) {
  if (http_code >= 0) {
    return;
  }
  if (http_inflight) {
    wifi_recover_requested = true;
    Serial.printf("[WIFI_RECOVER] deferred reason=%s code=%d\n",
                  reason ? reason : "http_fail",
                  http_code);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    ensure_wifi_ready(reason ? reason : "http_fail", 15000);
    return;
  }
  wifi_hard_reset_and_reconnect(reason ? reason : "http_fail", 15000);
}

#endif // SENSE_WIFI_H

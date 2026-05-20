/*
 * sense_upload.h
 *
 * HTTP request operations with retry logic, queue serialization,
 * presign error text, and network readiness checks for Sense_Minimal.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 6.
 *
 * Prerequisites (must be declared before #include "sense_upload.h"):
 *   - WiFi.h, WiFiClientSecure.h, HTTPClient.h, ArduinoJson.h
 *   - freertos/semphr.h
 *   - http_mutex (SemaphoreHandle_t), http_inflight (volatile bool)
 *   - wifi_recover_requested (bool)
 *   - current_job (OpJob) for effective_job fallback
 *   - presign_last_error_text[] buffer
 *   - ACTION_MIN_REMAINING_MS constant
 *   - uart_send_sense_diag() from sense_uart.h
 *   - tls_configure(), log_wifi_snapshot(), log_http_failure_details(),
 *     deadline_expired(), clamp_timeout_ms() from sense_http.h
 *   - ensure_wifi_ready(), ensure_time_valid(),
 *     wifi_recover_if_needed(), wifi_hard_reset_and_reconnect() from sense_wifi.h
 */

#ifndef SENSE_UPLOAD_H
#define SENSE_UPLOAD_H

// ── Presign error text ──────────────────────────────────────────────

static void presign_set_error_text(const char* text) {
  if (!text || !text[0]) {
    presign_last_error_text[0] = '\0';
    return;
  }
  strncpy(presign_last_error_text, text, sizeof(presign_last_error_text) - 1);
  presign_last_error_text[sizeof(presign_last_error_text) - 1] = '\0';
}

static const char* presign_error_text() {
  return presign_last_error_text[0] ? presign_last_error_text : "Network error. Tap to retry.";
}

// ── Simple network readiness check ──────────────────────────────────

static bool net_ready_for_tls(char* why, size_t why_len) {
  if (why && why_len > 0) {
    why[0] = '\0';
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (why && why_len > 0) {
      snprintf(why, why_len, "wifi_disconnected");
    }
    return false;
  }
  IPAddress ip = WiFi.localIP();
  if ((uint32_t)ip == 0) {
    if (why && why_len > 0) {
      snprintf(why, why_len, "no_ip");
    }
    return false;
  }
  time_t now = time(nullptr);
  if (now < 1700000000) {
    if (why && why_len > 0) {
      snprintf(why, why_len, "time_unsynced");
    }
    return false;
  }
  return true;
}

// ── HTTP queue serialization ────────────────────────────────────────

static void http_queue_lock(const char* label, uint32_t job_id) {
  Serial.printf("[HTTP_QUEUE] enqueue label=%s job=%lu\n",
                label ? label : "http",
                (unsigned long)job_id);
  if (http_mutex) {
    xSemaphoreTake(http_mutex, portMAX_DELAY);
  }
  http_inflight = true;
  Serial.printf("[HTTP_QUEUE] start label=%s job=%lu\n",
                label ? label : "http",
                (unsigned long)job_id);
  uart_send_sense_diag("http", "start", label, (int32_t)job_id, "queue_lock");
}

static void http_queue_unlock(const char* label, uint32_t job_id) {
  http_inflight = false;
  Serial.printf("[HTTP_QUEUE] done label=%s job=%lu\n",
                label ? label : "http",
                (unsigned long)job_id);
  uart_send_sense_diag("http", "done", label, (int32_t)job_id, "queue_unlock");
  if (http_mutex) {
    xSemaphoreGive(http_mutex);
  }
  if (wifi_recover_requested) {
    wifi_recover_requested = false;
    wifi_hard_reset_and_reconnect("deferred_recover", 15000);
  }
}

// ── HTTP POST with retries ──────────────────────────────────────────

static bool http_post_json_with_retries(const char* url,
                                        const String& body,
                                        int& http_code,
                                        String& resp_body,
                                        const char* label,
                                        const char* api_key,
                                        const char* bearer,
                                        uint32_t job_id,
                                        uint32_t deadline_ms) {
  // Release camera DMA reservation to defragment internal SRAM for TLS.
  bool dma_was_reserved_post = (g_camera_dma_reserve != nullptr);
  if (dma_was_reserved_post) {
    heap_caps_free(g_camera_dma_reserve);
    g_camera_dma_reserve = nullptr;
  }
  struct DmaGuardPost {
    bool should_reacquire;
    ~DmaGuardPost() {
      if (should_reacquire && !g_camera_dma_reserve) {
        g_camera_dma_reserve = (uint8_t*)heap_caps_malloc(
            CAMERA_DMA_RESERVE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      }
    }
  } dma_guard_post{dma_was_reserved_post};

  static const unsigned long backoff_ms[] = {500, 1500, 3500};
  const int max_attempts = 3;
  presign_set_error_text("");
  uint32_t effective_job = job_id;
  if (effective_job == 0 && current_job.active) {
    effective_job = current_job.job_id;
  }
  http_queue_lock(label, effective_job);
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (deadline_expired(deadline_ms)) {
      presign_set_error_text("Network timeout");
      Serial.printf("[%s] deadline_exceeded\n", label ? label : "HTTP");
      break;
    }
    // Yield to foreground user action (scan/voice) — break and release http_inflight
    if (foreground_active || dish_scan_inflight || voice_recording_active) {
      const char* fg = foreground_active ? "foreground" : dish_scan_inflight ? "scan" : "voice";
      Serial.printf("[%s] yield_to_foreground reason=%s attempt=%d\n",
                    label ? label : "HTTP", fg, attempt + 1);
      uart_send_sense_diag("http", "yield_fg", label, attempt + 1, fg);
      presign_set_error_text("Yielded to user action");
      break;
    }
    Serial.printf("[%s] attempt=%d/%d url=%s\n",
                  label ? label : "HTTP",
                  attempt + 1,
                  max_attempts,
                  url ? url : "");
    char attempt_detail[32];
    snprintf(attempt_detail, sizeof(attempt_detail), "attempt=%d/%d", attempt + 1, max_attempts);
    uart_send_sense_diag("http", "attempt", label, attempt + 1, attempt_detail);
    uint32_t wifi_timeout = clamp_timeout_ms(15000, deadline_ms);
    if (wifi_timeout < ACTION_MIN_REMAINING_MS) {
      presign_set_error_text("Network timeout");
      break;
    }
    if (!ensure_wifi_ready("http_ready", wifi_timeout)) {
      presign_set_error_text("Wi-Fi not ready");
      log_wifi_snapshot("wifi_not_ready");
      uint32_t backoff = clamp_timeout_ms(backoff_ms[attempt], deadline_ms);
      if (backoff == 0) {
        break;
      }
      delay(backoff);
      continue;
    }
    uint32_t time_timeout = clamp_timeout_ms(15000, deadline_ms);
    if (time_timeout < ACTION_MIN_REMAINING_MS) {
      presign_set_error_text("Network timeout");
      break;
    }
    if (!ensure_time_valid("http_ready", time_timeout)) {
      presign_set_error_text("Time not set");
      log_wifi_snapshot("time_not_ready");
      uint32_t backoff = clamp_timeout_ms(backoff_ms[attempt], deadline_ms);
      if (backoff == 0) {
        break;
      }
      delay(backoff);
      continue;
    }
    WiFiClientSecure client;
    tls_configure(client, label);
    HTTPClient http;
    if (!http.begin(client, url)) {
      http_code = -1;
      resp_body = "";
      presign_set_error_text("Network error. Tap to retry.");
      log_http_failure_details(label, url, http_code, &client);
      wifi_recover_if_needed("http_begin", http_code);
      uint32_t backoff = clamp_timeout_ms(backoff_ms[attempt], deadline_ms);
      if (backoff == 0) {
        break;
      }
      delay(backoff);
      continue;
    }
  http.addHeader("Content-Type", "application/json");
    if (api_key && api_key[0]) {
      http.addHeader("x-api-key", api_key);
    }
    if (bearer && bearer[0]) {
      String auth = String("Bearer ") + bearer;
      http.addHeader("Authorization", auth);
    }
    http_code = http.POST(body);
    resp_body = http.getString();
    http.end();
    if (http_code >= 200 && http_code < 300) {
      uart_send_sense_diag("http", "success", label, (int32_t)http_code, "post");
      http_queue_unlock(label, effective_job);
      return true;
    }
    presign_set_error_text("Network error. Tap to retry.");
    log_http_failure_details(label, url, http_code, &client);
    wifi_recover_if_needed("http_post", http_code);
    uint32_t backoff = clamp_timeout_ms(backoff_ms[attempt], deadline_ms);
    if (backoff == 0) {
      break;
    }
    delay(backoff);
  }
  http_queue_unlock(label, effective_job);
  return false;
}

// ── HTTP GET with retries ───────────────────────────────────────────

static bool http_get_with_retries(const char* url,
                                  int& http_code,
                                  String& resp_body,
                                  const char* label,
                                  const char* api_key,
                                  const char* bearer,
                                  uint32_t job_id,
                                  uint32_t deadline_ms) {
  // Release camera DMA reservation to defragment internal SRAM for TLS.
  bool dma_was_reserved_get = (g_camera_dma_reserve != nullptr);
  if (dma_was_reserved_get) {
    heap_caps_free(g_camera_dma_reserve);
    g_camera_dma_reserve = nullptr;
  }
  struct DmaGuardGet {
    bool should_reacquire;
    ~DmaGuardGet() {
      if (should_reacquire && !g_camera_dma_reserve) {
        g_camera_dma_reserve = (uint8_t*)heap_caps_malloc(
            CAMERA_DMA_RESERVE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
      }
    }
  } dma_guard_get{dma_was_reserved_get};

  static const unsigned long backoff_ms[] = {500, 1500, 3500};
  const int max_attempts = 3;
  uint32_t effective_job = job_id;
  if (effective_job == 0 && current_job.active) {
    effective_job = current_job.job_id;
  }
  http_queue_lock(label, effective_job);
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    if (deadline_expired(deadline_ms)) {
      Serial.printf("[%s] deadline_exceeded\n", label ? label : "HTTP_GET");
      break;
    }
    // Yield to foreground user action (scan/voice) — break and release http_inflight
    if (foreground_active || dish_scan_inflight || voice_recording_active) {
      const char* fg = foreground_active ? "foreground" : dish_scan_inflight ? "scan" : "voice";
      Serial.printf("[%s] yield_to_foreground reason=%s attempt=%d\n",
                    label ? label : "HTTP_GET", fg, attempt + 1);
      uart_send_sense_diag("http", "yield_fg", label, attempt + 1, fg);
      presign_set_error_text("Yielded to user action");
      break;
    }
    Serial.printf("[%s] attempt=%d/%d url=%s\n",
                  label ? label : "HTTP_GET",
                  attempt + 1,
                  max_attempts,
                  url ? url : "");
    char attempt_detail[32];
    snprintf(attempt_detail, sizeof(attempt_detail), "attempt=%d/%d", attempt + 1, max_attempts);
    uart_send_sense_diag("http", "attempt", label, attempt + 1, attempt_detail);
    uint32_t wifi_timeout = clamp_timeout_ms(15000, deadline_ms);
    if (wifi_timeout < ACTION_MIN_REMAINING_MS) {
      break;
    }
    if (!ensure_wifi_ready("http_get_ready", wifi_timeout)) {
      log_wifi_snapshot("http_get_wifi_not_ready");
      uint32_t backoff = clamp_timeout_ms(backoff_ms[attempt], deadline_ms);
      if (backoff == 0) {
        break;
      }
      delay(backoff);
      continue;
    }
    uint32_t time_timeout = clamp_timeout_ms(15000, deadline_ms);
    if (time_timeout < ACTION_MIN_REMAINING_MS) {
      break;
    }
    if (!ensure_time_valid("http_get_ready", time_timeout)) {
      log_wifi_snapshot("http_get_time_not_ready");
      uint32_t backoff = clamp_timeout_ms(backoff_ms[attempt], deadline_ms);
      if (backoff == 0) {
        break;
      }
      delay(backoff);
      continue;
    }
    WiFiClientSecure client;
    tls_configure(client, label);
    HTTPClient http;
    if (!http.begin(client, url)) {
      http_code = -1;
      resp_body = "";
      log_http_failure_details(label, url, http_code, &client);
      wifi_recover_if_needed("http_get_begin", http_code);
      uint32_t backoff = clamp_timeout_ms(backoff_ms[attempt], deadline_ms);
      if (backoff == 0) {
        break;
      }
      delay(backoff);
      continue;
    }
    http.addHeader("Accept", "application/json");
    if (api_key && api_key[0]) {
      http.addHeader("x-api-key", api_key);
    }
    if (bearer && bearer[0]) {
      String auth = String("Bearer ") + bearer;
      http.addHeader("Authorization", auth);
    }
    http_code = http.GET();
    resp_body = http.getString();
    http.end();
    if (http_code >= 200 && http_code < 300) {
      uart_send_sense_diag("http", "success", label, (int32_t)http_code, "get");
      http_queue_unlock(label, effective_job);
      return true;
    }
    log_http_failure_details(label, url, http_code, &client);
    if ((http_code >= 400 && http_code < 500) && http_code != 429) {
      http_queue_unlock(label, effective_job);
      return false;
    }
    wifi_recover_if_needed("http_get", http_code);
    uint32_t backoff = clamp_timeout_ms(backoff_ms[attempt], deadline_ms);
    if (backoff == 0) {
      break;
    }
    delay(backoff);
  }
  http_queue_unlock(label, effective_job);
  return false;
}

#endif // SENSE_UPLOAD_H

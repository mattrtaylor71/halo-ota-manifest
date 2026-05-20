/*
 * sense_upload_exec.h
 *
 * Upload execution: check-in presign, S3 PUT upload, dish result
 * HTTP polling, and UI meal result message.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 16.
 *
 * Prerequisites (must be declared before #include "sense_upload_exec.h"):
 *   - ArduinoJson.h, WiFiClientSecure.h
 *   - PresignReply struct, UploadJob struct
 *   - PROTOCOL_VERSION, get_next_msg_id(), uart_send_json() from sense_uart.h
 *   - diag_record_error(), diag_record_action_event() from sense_diag.h
 *   - presign_set_error_text(), http_post_json_with_retries(),
 *     http_get_with_retries(), http_queue_lock(), http_queue_unlock(),
 *     parse_url_parts() from sense_upload.h
 *   - clamp_timeout_ms(), deadline_remaining_ms(), deadline_expired() from sense_http.h
 *   - append_camera_meta_json(), log_camera_meta_for_presign() from sense_camera.h
 *   - scan_ui_status_emit(), clear_active_dish_job(), flow_step(),
 *     dish_upload_pending() from sense_scan.h
 *   - uart_send_ui_toast() from sense_uart_msg.h
 *   - uart_send_sense_diag() from sense_diag.h
 *   - load_owner_id_or_default(), load_runtime_device_id()
 *   - CHECKIN_API_BASE_URL, CHECKIN_PRESIGN_ENDPOINT, API_KEY, BEARER_TOKEN
 *   - current_job (OpJob), waiting_for_mqtt_result, mqtt_wait_deadline,
 *     current_scan_job_id, current_result_local_job_id, current_result_mode,
 *     DISH_RESULT_TIMEOUT_MS, ACTION_MIN_REMAINING_MS
 *   - g_dish_timing (DishTimingTrace)
 *   - mqtt_clear_result_subscription() from sense_mqtt.h
 */

#ifndef SENSE_UPLOAD_EXEC_H
#define SENSE_UPLOAD_EXEC_H

// Forward declaration (defined later in this header)
static void uart_send_ui_meal_result(int kcal, const char* meal_summary, int health_score, const char* recommendation, const char* mode, float protein_g, float carbs_g, float fat_g, float confidence, uint32_t job_id);

// ── Dish result URL builder ───────────────────────────────────────

static String build_dish_result_url(const char* user_id, const char* device_id, const char* job_id) {
  if (!user_id || user_id[0] == '\0' ||
      !device_id || device_id[0] == '\0' ||
      !job_id || job_id[0] == '\0') {
    return String();
  }
  return String(CHECKIN_API_BASE_URL) + "/dish/result?user_id=" + String(user_id) +
         "&device_id=" + String(device_id) +
         "&job_id=" + String(job_id);
}

// ── Dish result poll delay ────────────────────────────────────────

static uint32_t dish_result_poll_delay_ms(const JsonDocument& doc, uint32_t fallback_ms) {
  uint32_t poll_after_ms = doc["poll_after_ms"] | fallback_ms;
  if (poll_after_ms < 250) {
    poll_after_ms = 250;
  }
  if (poll_after_ms > 5000) {
    poll_after_ms = 5000;
  }
  return poll_after_ms;
}

// ── Wait for dish result via HTTP polling ─────────────────────────

static bool wait_for_dish_result_http(const UploadJob& job,
                                      const PresignReply& presign,
                                      uint32_t job_deadline_ms) {
  auto clear_wait_state = [&]() {
    waiting_for_mqtt_result = false;
    mqtt_wait_deadline = 0;
    current_scan_job_id = "";
    mqtt_clear_result_subscription();
  };

  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));

  String result_url = presign.result_url;
  if (result_url == "null") {
    result_url = "";
  }
  if (result_url.length() == 0) {
    result_url = build_dish_result_url(owner_id, device_id, presign.job_id.c_str());
  }
  if (result_url.length() == 0) {
    Serial.println("[RESULT_HTTP] missing result_url");
    scan_ui_status_emit("ERROR", "Dish result URL missing", job.mode, job.job_id, true);
    diag_record_error("result_http", -1, "missing_result_url");
    diag_record_action_event("result", job.mode, "err", "missing_url", -1);
    clear_wait_state();
    clear_active_dish_job(job.job_id, "http_missing_result_url");
    return false;
  }

  uint32_t remaining_ms = deadline_remaining_ms(job_deadline_ms);
  uint32_t max_wait_ms = (remaining_ms < DISH_RESULT_TIMEOUT_MS) ? remaining_ms : DISH_RESULT_TIMEOUT_MS;
  if (max_wait_ms < ACTION_MIN_REMAINING_MS) {
    scan_ui_status_emit("ERROR", "Analysis timeout", job.mode, job.job_id, true);
    diag_record_error("result_http", -1, "timeout_budget");
    diag_record_action_event("result", job.mode, "err", "timeout_budget", -1);
    clear_wait_state();
    clear_active_dish_job(job.job_id, "http_timeout_budget");
    return false;
  }

  clear_wait_state();
  waiting_for_mqtt_result = true;  // Reused as generic dish-result wait state.
  mqtt_wait_deadline = millis() + max_wait_ms;
  scan_ui_status_emit("RESULT_WAITING", "AI working its magic", job.mode, job.job_id, false);
  flow_step(job.job_id, "RESULT_WAITING");
  g_dish_timing.mqtt_refresh_start_ms = millis();
  g_dish_timing.mqtt_refresh_end_ms = millis();
  g_dish_timing.result_wait_start_ms = millis();

  const uint32_t local_job_id = current_result_local_job_id ? current_result_local_job_id : job.job_id;
  const char* result_mode = current_result_mode[0] ? current_result_mode : job.mode;
  bool had_fast_result = false;
  bool fast_emitted = false;
  uint32_t wait_start = millis();

  Serial.printf("[RESULT_HTTP] start job_id=%s timeout_ms=%lu url=%s\n",
                presign.job_id.c_str(),
                (unsigned long)max_wait_ms,
                result_url.c_str());
  Serial.printf("[TIMING][DISH] job_id=%lu result_http_setup_ms=%lu upload_to_wait_ms=%lu wait_budget_ms=%lu\n",
                (unsigned long)job.job_id,
                (unsigned long)(g_dish_timing.mqtt_refresh_end_ms - g_dish_timing.mqtt_refresh_start_ms),
                (unsigned long)(g_dish_timing.result_wait_start_ms - g_dish_timing.put_end_ms),
                (unsigned long)max_wait_ms);

  while (waiting_for_mqtt_result &&
         !deadline_expired(job_deadline_ms) &&
         (millis() - wait_start) < max_wait_ms) {
    int http_code = 0;
    String resp_body;
    if (!http_get_with_retries(result_url.c_str(),
                               http_code,
                               resp_body,
                               "DISH_RESULT",
                               API_KEY,
                               BEARER_TOKEN,
                               job.job_id,
                               job_deadline_ms)) {
      if (http_code == 404) {
        scan_ui_status_emit("ERROR", "Unknown dish job", job.mode, job.job_id, true);
        diag_record_error("result_http", http_code, "unknown_job");
        diag_record_action_event("result", job.mode, "err", "http_404", http_code);
        clear_wait_state();
        clear_active_dish_job(job.job_id, "http_unknown_job");
        return false;
      }
      if (http_code == 401 || http_code == 403) {
        scan_ui_status_emit("ERROR", "Dish auth failed", job.mode, job.job_id, true);
        diag_record_error("result_http", http_code, "auth");
        diag_record_action_event("result", job.mode, "err", "http_auth", http_code);
        clear_wait_state();
        clear_active_dish_job(job.job_id, "http_auth_error");
        return false;
      }
      if (http_code >= 400 && http_code < 500) {
        scan_ui_status_emit("ERROR", "Dish result failed", job.mode, job.job_id, true);
        diag_record_error("result_http", http_code, "client_error");
        diag_record_action_event("result", job.mode, "err", "http_client", http_code);
        clear_wait_state();
        clear_active_dish_job(job.job_id, "http_client_error");
        return false;
      }

      uint32_t retry_delay_ms = clamp_timeout_ms(had_fast_result ? 1500UL : 1000UL, job_deadline_ms);
      Serial.printf("[RESULT_HTTP] transient_error code=%d delay_ms=%lu had_fast=%d\n",
                    http_code,
                    (unsigned long)retry_delay_ms,
                    had_fast_result ? 1 : 0);
      if (retry_delay_ms == 0) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
      continue;
    }

    DynamicJsonDocument doc(6144);
    DeserializationError err = deserializeJson(doc, resp_body);
    if (err) {
      uint32_t retry_delay_ms = clamp_timeout_ms(had_fast_result ? 1500UL : 1000UL, job_deadline_ms);
      Serial.printf("[RESULT_HTTP] parse_error=%s delay_ms=%lu\n",
                    err.c_str(),
                    (unsigned long)retry_delay_ms);
      if (retry_delay_ms == 0) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
      continue;
    }

    const char* state = doc["state"] | "";
    const char* phase = doc["phase"] | "";
    const char* latest_phase = doc["latest_phase"] | "";
    const char* pipeline_stage = doc["pipeline_stage"] | "";
    bool is_terminal = doc["is_terminal"] | false;
    bool is_pending = strcmp(state, "pending") == 0 || strcmp(phase, "pending") == 0;
    bool is_fast = strcmp(phase, "fast") == 0 ||
                   (!is_terminal && strcmp(latest_phase, "fast") == 0 && strcmp(state, "ready") == 0);
    bool is_final = strcmp(phase, "final") == 0 ||
                    is_terminal ||
                    strcmp(latest_phase, "final") == 0 ||
                    (strcmp(state, "ready") == 0 && strcmp(pipeline_stage, "DONE") == 0);
    uint32_t poll_after_ms = dish_result_poll_delay_ms(doc, is_fast ? 1500UL : 1000UL);

    Serial.printf("[RESULT_HTTP] job_id=%s state=%s phase=%s latest=%s terminal=%d stage=%s poll_after_ms=%lu\n",
                  presign.job_id.c_str(),
                  state,
                  phase,
                  latest_phase,
                  is_terminal ? 1 : 0,
                  pipeline_stage,
                  (unsigned long)poll_after_ms);

    if (is_pending) {
      vTaskDelay(pdMS_TO_TICKS(poll_after_ms));
      continue;
    }

    if (!is_fast && !is_final) {
      const char* msg = doc["message"] | "";
      if (msg && msg[0]) {
        uart_send_ui_toast(msg);
      }
      vTaskDelay(pdMS_TO_TICKS(poll_after_ms));
      continue;
    }

    const char* meal_summary = doc["meal_summary"] | "";
    const char* summary = doc["summary"] | "";
    const char* dish_name = doc["dish_name"] | "";
    String summary_text;
    if (meal_summary && meal_summary[0]) {
      summary_text = meal_summary;
    } else if (summary && summary[0]) {
      summary_text = summary;
    } else if (dish_name && dish_name[0]) {
      summary_text = dish_name;
    }

    int calories = doc["calories"] | 0;
    if (doc["calories"].isNull()) {
      calories = doc["kcal"] | 0;
    }
    float protein_g = doc["protein_g"] | 0.0f;
    float carbs_g = doc["carbs_g"] | 0.0f;
    float fat_g = doc["fat_g"] | 0.0f;
    float confidence = doc["confidence"] | 0.0f;
    int health_score = doc["health_score"] | 0;
    const char* recommendation = doc["recommendation"] | "";

    if (summary_text.length() > 0 && (is_final || !fast_emitted)) {
      uart_send_ui_meal_result(calories,
                               summary_text.c_str(),
                               health_score,
                               (recommendation && recommendation[0]) ? recommendation : NULL,
                               result_mode,
                               protein_g,
                               carbs_g,
                               fat_g,
                               confidence,
                               local_job_id);
    } else if (summary_text.length() == 0) {
      const char* msg = doc["message"] | "";
      if (msg && msg[0]) {
        uart_send_ui_toast(msg);
      }
    }

    if (is_fast) {
      had_fast_result = true;
      if (summary_text.length() > 0) {
        fast_emitted = true;
      }
      diag_record_action_event("result", job.mode, "ok", "http_fast", 0);
      uint32_t result_now_ms = millis();
      Serial.printf("[TIMING][DISH] job_id=%lu result_http_fast total_ms=%lu post_upload_to_result_ms=%lu result_wait_loop_ms=%lu ui_wait_ms=%lu\n",
                    (unsigned long)g_dish_timing.sense_job_id,
                    (unsigned long)(g_dish_timing.upload_start_ms ? (result_now_ms - g_dish_timing.upload_start_ms) : 0),
                    (unsigned long)(g_dish_timing.put_end_ms ? (result_now_ms - g_dish_timing.put_end_ms) : 0),
                    (unsigned long)(g_dish_timing.result_wait_start_ms ? (result_now_ms - g_dish_timing.result_wait_start_ms) : 0),
                    (unsigned long)(g_dish_timing.ui_wait_start_ms ? (result_now_ms - g_dish_timing.ui_wait_start_ms) : 0));
      clear_wait_state();
      clear_active_dish_job(job.job_id, "http_fast_result");
      Serial.println("[RESULT_HTTP] dish result processed (fast)");
      return true;
    }

    diag_record_action_event("result", job.mode, "ok", "http_final", 0);
    uint32_t result_now_ms = millis();
    Serial.printf("[TIMING][DISH] job_id=%lu result_http_final total_ms=%lu post_upload_to_result_ms=%lu result_wait_loop_ms=%lu ui_wait_ms=%lu\n",
                  (unsigned long)g_dish_timing.sense_job_id,
                  (unsigned long)(g_dish_timing.upload_start_ms ? (result_now_ms - g_dish_timing.upload_start_ms) : 0),
                  (unsigned long)(g_dish_timing.put_end_ms ? (result_now_ms - g_dish_timing.put_end_ms) : 0),
                  (unsigned long)(g_dish_timing.result_wait_start_ms ? (result_now_ms - g_dish_timing.result_wait_start_ms) : 0),
                  (unsigned long)(g_dish_timing.ui_wait_start_ms ? (result_now_ms - g_dish_timing.ui_wait_start_ms) : 0));
    clear_wait_state();
    clear_active_dish_job(job.job_id, "http_result_final");
    Serial.println("[RESULT_HTTP] dish result processed (final)");
    return true;
  }

  uint32_t timeout_now_ms = millis();
  Serial.printf("[TIMING][DISH] job_id=%lu timeout total_ms=%lu post_upload_wait_ms=%lu result_wait_loop_ms=%lu ui_wait_ms=%lu had_fast=%d\n",
                (unsigned long)job.job_id,
                (unsigned long)(timeout_now_ms - g_dish_timing.upload_start_ms),
                (unsigned long)(timeout_now_ms - g_dish_timing.put_end_ms),
                (unsigned long)(timeout_now_ms - g_dish_timing.result_wait_start_ms),
                (unsigned long)(g_dish_timing.ui_wait_start_ms ? (timeout_now_ms - g_dish_timing.ui_wait_start_ms) : 0),
                had_fast_result ? 1 : 0);
  clear_wait_state();
  if (had_fast_result) {
    diag_record_action_event("result", job.mode, "ok", "http_fast_timeout", 0);
    clear_active_dish_job(job.job_id, "http_fast_timeout");
    Serial.println("[RESULT_HTTP] final timeout -> keeping last fast result");
    return true;
  }

  scan_ui_status_emit("ERROR", "Analysis timeout", job.mode, job.job_id, true);
  diag_record_error("result_http", -1, "timeout");
  diag_record_action_event("result", job.mode, "err", "timeout", -1);
  clear_active_dish_job(job.job_id, "http_result_timeout");
  Serial.println("[RESULT_HTTP] dish result timeout");
  return false;
}

// ── Check-in presign ──────────────────────────────────────────────

static bool get_presign_checkin(PresignReply& out, const char* expiry_date, uint16_t quantity, const UploadJob::CameraUploadMeta* camera_meta, uint32_t deadline_ms) {
  String presign_url = String(CHECKIN_API_BASE_URL) + String(CHECKIN_PRESIGN_ENDPOINT);

  StaticJsonDocument<512> doc;
  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));
  doc["user_id"] = owner_id;
  doc["device_id"] = device_id;
  doc["owner"] = owner_id;
  doc["action"] = "IN";
  doc["type"] = "grocery";
  doc["content_type"] = "image/jpeg";

  if (expiry_date != NULL && strlen(expiry_date) > 0) {
    doc["product_expiration"] = expiry_date;
    Serial.printf("[CHECKIN_PRESIGN] Including expiration date: %s\n", expiry_date);
  }
  uint16_t safe_quantity = (quantity < 1) ? 1 : quantity;
  doc["quantity"] = safe_quantity;
  Serial.printf("[CHECKIN_PRESIGN] Including quantity: %u\n", (unsigned)safe_quantity);
  if (camera_meta) {
    append_camera_meta_json(doc, *camera_meta);
  }

  String body;
  serializeJson(doc, body);

  log_camera_meta_for_presign("CHECKIN_PRESIGN", camera_meta);
  Serial.println("[CHECKIN_PRESIGN] POST " + presign_url);
  Serial.println("[CHECKIN_PRESIGN] Body: " + body);

  int http_code = 0;
  String resp_body;
  if (!http_post_json_with_retries(presign_url.c_str(),
                                   body,
                                   http_code,
                                   resp_body,
                                   "CHECKIN_PRESIGN",
                                   NULL,
                                   NULL,
                                   current_job.job_id,
                                   deadline_ms)) {
    Serial.printf("[CHECKIN_PRESIGN] Request failed with code %d\n", http_code);
    return false;
  }
  Serial.printf("[CHECKIN_PRESIGN] HTTP %d\n", http_code);
  if (resp_body.length()) Serial.println("[CHECKIN_PRESIGN] Body: " + resp_body);

  StaticJsonDocument<768> r;
  auto err = deserializeJson(r, resp_body);
  if (err) {
    Serial.print("[CHECKIN_PRESIGN] JSON parse error: ");
    Serial.println(err.c_str());
    diag_record_error("presign_parse", -1, "json_parse");
    return false;
  }

  out.job_id       = r["job_id"].as<String>();
  out.put_url      = r["put_url"].as<String>();
  out.s3_key       = r["s3_key"].as<String>();
  out.content_type = "image/jpeg";
  out.ttl_s        = r["expires_in"] | 300;

  bool success = !(out.job_id.isEmpty() || out.put_url.isEmpty());
  if (success) {
    Serial.println("[CHECKIN_PRESIGN] Presign OK");
  } else {
    Serial.println("[CHECKIN_PRESIGN] Presign failed - missing fields");
    diag_record_error("presign_parse", -1, "missing_fields");
  }
  return success;
}

// ── S3 PUT upload ─────────────────────────────────────────────────

static bool put_to_presigned_url(const String& url,
                                 const uint8_t* buf,
                                 size_t len,
                                 const char* contentType,
                                 uint32_t job_id,
                                 bool allow_abort,
                                 bool* aborted_for_dish,
                                 uint32_t deadline_ms,
                                 bool* aborted_for_budget) {
  Serial.printf("[UPLOAD] Starting PUT to S3, size: %u bytes\n", len);

  // Release camera DMA reservation to defragment internal SRAM for TLS.
  // The 16KB block sits mid-heap and prevents esp-aes from finding a
  // contiguous DMA region during TLS handshake. Camera is not used during
  // uploads; re-acquire at function exit via RAII guard.
  bool dma_was_reserved = (g_camera_dma_reserve != nullptr);
  if (dma_was_reserved) {
    heap_caps_free(g_camera_dma_reserve);
    g_camera_dma_reserve = nullptr;
    Serial.println("[UPLOAD] Camera DMA reservation released for TLS headroom");
  }
  // RAII guard: re-acquire DMA reservation on any return path
  struct DmaGuard {
    bool should_reacquire;
    ~DmaGuard() {
      if (should_reacquire && !g_camera_dma_reserve) {
        g_camera_dma_reserve = (uint8_t*)heap_caps_malloc(
            CAMERA_DMA_RESERVE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (g_camera_dma_reserve) {
          Serial.printf("[UPLOAD] Camera DMA reservation re-acquired at %p\n", g_camera_dma_reserve);
        }
      }
    }
  } dma_guard{dma_was_reserved};
  if (aborted_for_dish) {
    *aborted_for_dish = false;
  }
  if (aborted_for_budget) {
    *aborted_for_budget = false;
  }
  if (deadline_expired(deadline_ms)) {
    if (aborted_for_budget) {
      *aborted_for_budget = true;
    }
    presign_set_error_text("Upload timeout");
    diag_record_error("upload_put", -1, "deadline_expired");
    return false;
  }
  if (allow_abort && dish_upload_pending()) {
    if (aborted_for_dish) {
      *aborted_for_dish = true;
    }
    Serial.println("[UPLOAD] abort before connect (dish preempt)");
    return false;
  }
  uint32_t effective_job = job_id;
  if (effective_job == 0 && current_job.active) {
    effective_job = current_job.job_id;
  }
  http_queue_lock("UPLOAD_PUT", effective_job);

  bool https = false;
  String host;
  String path;
  uint16_t port = 0;
  if (!parse_url_parts(url, https, host, port, path)) {
    Serial.println("[UPLOAD] URL parse failed for PUT");
    diag_record_error_persistent("upload_put", -1, "bad_url");
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "bad_url");
    http_queue_unlock("UPLOAD_PUT", effective_job);
    return false;
  }

  if (deadline_expired(deadline_ms)) {
    if (aborted_for_budget) {
      *aborted_for_budget = true;
    }
    presign_set_error_text("Upload timeout");
    diag_record_error("upload_put", -1, "deadline_expired");
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "deadline_expired");
    http_queue_unlock("UPLOAD_PUT", effective_job);
    return false;
  }
  const int put_max_attempts = 2;
  bool write_ok = false;
  int put_attempt;
  WiFiClientSecure tls;
  for (put_attempt = 1; put_attempt <= put_max_attempts; put_attempt++) {
    tls.stop();
    tls.setInsecure();
    uint32_t tls_timeout = clamp_timeout_ms(60000, deadline_ms);
    if (tls_timeout < ACTION_MIN_REMAINING_MS) {
      if (aborted_for_budget) {
        *aborted_for_budget = true;
      }
      presign_set_error_text("Upload timeout");
      diag_record_error("upload_put", -1, "timeout");
      uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "timeout");
      http_queue_unlock("UPLOAD_PUT", effective_job);
      return false;
    }
    tls.setTimeout(tls_timeout);

    if (!tls.connect(host.c_str(), port)) {
      Serial.printf("[UPLOAD] TLS connect failed for PUT (attempt %d/%d)\n", put_attempt, put_max_attempts);
      tls.stop();
      if (put_attempt < put_max_attempts) {
        Serial.println("[UPLOAD] Hard WiFi reset before PUT retry");
        http_queue_unlock("UPLOAD_PUT", effective_job);
        wifi_hard_reset_and_reconnect("put_tls_connect", 15000);
        http_queue_lock("UPLOAD_PUT", effective_job);
        continue;
      }
      diag_record_error_persistent("upload_put", -1, "tls_connect");
      uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "tls_connect");
      http_queue_unlock("UPLOAD_PUT", effective_job);
      return false;
    }

    const char* resolved_ct = (contentType && *contentType && strcmp(contentType, "null") != 0)
                                ? contentType
                                : "image/jpeg";
    Serial.printf("[UPLOAD] Using Content-Type: %s\n", resolved_ct);

    tls.printf("PUT %s HTTP/1.1\r\n", path.c_str());
    tls.printf("Host: %s\r\n", host.c_str());
    tls.printf("Content-Type: %s\r\n", resolved_ct);
    tls.printf("Content-Length: %u\r\n", (unsigned)len);
    tls.print("Connection: close\r\n\r\n");

    Serial.printf("[UPLOAD] Sending PUT request (attempt %d/%d)...\n", put_attempt, put_max_attempts);
    unsigned long upload_start = millis();

    size_t offset = 0;
    const size_t chunk_size = 2048;
    write_ok = true;
    while (offset < len) {
      if (deadline_expired(deadline_ms)) {
        if (aborted_for_budget) {
          *aborted_for_budget = true;
        }
        presign_set_error_text("Upload timeout");
        Serial.println("[UPLOAD] abort during PUT (budget)");
        diag_record_error("upload_put", -1, "timeout");
        uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "timeout");
        tls.stop();
        http_queue_unlock("UPLOAD_PUT", effective_job);
        return false;
      }
      if (foreground_active) {
        Serial.println("[UPLOAD] abort during PUT (foreground user action)");
        diag_record_error("upload_put", -1, "foreground_preempt");
        uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "foreground_preempt");
        tls.stop();
        http_queue_unlock("UPLOAD_PUT", effective_job);
        return false;
      }
      if (allow_abort && dish_upload_pending()) {
        if (aborted_for_dish) {
          *aborted_for_dish = true;
        }
        Serial.println("[UPLOAD] abort during PUT (dish preempt)");
        diag_record_error("upload_put", -1, "dish_preempt");
        uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "dish_preempt");
        tls.stop();
        http_queue_unlock("UPLOAD_PUT", effective_job);
        return false;
      }
      size_t to_write = len - offset;
      if (to_write > chunk_size) {
        to_write = chunk_size;
      }
      int written = tls.write(buf + offset, to_write);
      if (written <= 0) {
        write_ok = false;
        break;
      }
      offset += (size_t)written;
    }

    unsigned long upload_duration = millis() - upload_start;
    Serial.printf("[UPLOAD] PUT completed in %lu ms\n", upload_duration);

    if (!write_ok) {
      Serial.printf("[UPLOAD] PUT write failed (attempt %d/%d)\n", put_attempt, put_max_attempts);
      tls.stop();
      if (put_attempt < put_max_attempts) {
        Serial.println("[UPLOAD] Hard WiFi reset before PUT retry");
        http_queue_unlock("UPLOAD_PUT", effective_job);
        wifi_hard_reset_and_reconnect("put_write_fail", 15000);
        http_queue_lock("UPLOAD_PUT", effective_job);
        continue;
      }
      diag_record_error_persistent("upload_put", -1, "write_failed");
      uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "write_failed");
      http_queue_unlock("UPLOAD_PUT", effective_job);
      Serial.println("[UPLOAD] ✗ Upload failed");
      return false;
    }

    // Write succeeded, break out of retry loop
    break;
  }  // end retry loop

  String status_line = tls.readStringUntil('\n');
  status_line.trim();
  int code = -1;
  if (status_line.startsWith("HTTP/")) {
    int space = status_line.indexOf(' ');
    if (space > 0) {
      code = status_line.substring(space + 1).toInt();
    }
  }

  String resp;
  const unsigned long resp_deadline = millis() + 2000;
  while (millis() < resp_deadline) {
    while (tls.available()) {
      char c = (char)tls.read();
      if (resp.length() < 1024) {
        resp += c;
      }
    }
    if (!tls.available()) {
      delay(10);
    }
  }
  tls.stop();
  http_queue_unlock("UPLOAD_PUT", effective_job);

  Serial.printf("[UPLOAD] PUT status: %d\n", code);
  if (resp.length()) {
    Serial.print("[UPLOAD] PUT response: ");
    Serial.println(resp);
  }

  bool success = (code >= 200 && code < 300);
  if (success) {
    Serial.println("[UPLOAD] ✓ Upload successful");
    uart_send_sense_diag("http", "success", "UPLOAD_PUT", (int32_t)code, "put");
  } else {
    Serial.println("[UPLOAD] ✗ Upload failed");
    diag_record_error_persistent("upload_put", code, "http_status");
    char detail[32];
    snprintf(detail, sizeof(detail), "http_status=%d", code);
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", (int32_t)code, detail);
  }

  return success;
}

// ── UI meal result message ────────────────────────────────────────

static void uart_send_ui_meal_result(int kcal, const char* meal_summary, int health_score, const char* recommendation, const char* mode, float protein_g, float carbs_g, float fat_g, float confidence, uint32_t job_id) {
  StaticJsonDocument<1024> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "UI_MEAL_RESULT";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["calories"] = kcal;
  doc["protein_g"] = protein_g;
  doc["carbs_g"] = carbs_g;
  doc["fat_g"] = fat_g;
  doc["confidence"] = confidence;
  if (meal_summary != NULL && strlen(meal_summary) > 0) {
    doc["meal_summary"] = meal_summary;
  }
  if (recommendation != NULL && strlen(recommendation) > 0) {
    doc["recommendation"] = recommendation;
  }
  if (mode != NULL && strlen(mode) > 0) {
    doc["mode"] = mode;
  }
  if (job_id != 0) {
    doc["job_id"] = job_id;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

#endif // SENSE_UPLOAD_EXEC_H

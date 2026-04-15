/*
 * sense_voice.h
 *
 * Voice recording, WiFi connect for voice, session management,
 * audio capture callback, and PCM upload to quick-ack API.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 10.
 *
 * Prerequisites (must be declared before #include "sense_voice.h"):
 *   - WiFi.h, WiFiClientSecure.h, HTTPClient.h, ArduinoJson.h
 *   - esp_wifi.h (esp_wifi_set_ps)
 *   - wifi_guard_set_state(), wifi_guard_poll(), wifi_guard_set_inflight(),
 *     wifi_guard_connect_owner(), ensure_wifi_connected() from sense_wifi.h
 *   - wifi_connect_inflight, wifi_inflight_start_ms, wifi_connected_ms,
 *     wifi_state, WIFI_CONNECT_TIMEOUT_MS, upload_inflight
 *   - http_queue_lock(), http_queue_unlock() from sense_upload.h
 *   - log_http_failure_details() from sense_http.h
 *   - http_inflight (volatile bool)
 *   - load_owner_id_or_default(), load_runtime_device_id()
 *   - QUICK_ACK_BASE_URL, QUICK_ACK_ENDPOINT, TREPO_DEVICE_ID
 *   - AUDIO_BUFFER_SIZE, VOICE_SESSION_IDLE_TIMEOUT_MS
 *   - voice_audio_buffer, voice_audio_pos, voice_audio_size,
 *     voice_recording_active, voice_finalize_requested,
 *     voice_peak_abs, voice_sum_abs, voice_sample_count,
 *     voice_nonzero_sample_count
 *   - g_voice_session_id[], g_voice_session_last_turn_ms
 *   - g_sense_boot_count
 *   - current_job (OpJob), OP_VOICE, OP_RECORDING, OP_FINALIZE
 */

#ifndef SENSE_VOICE_H
#define SENSE_VOICE_H

// ── Voice WiFi helpers ─────────────────────────────────────────────

static bool voice_ensure_wifi_connected() {
  if (WiFi.status() == WL_CONNECTED) {
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "voice_already_connected", WL_CONNECTED);
    return true;
  }

  if (wifi_connect_inflight && wifi_inflight_start_ms > 0) {
    unsigned long inflight_elapsed = millis() - wifi_inflight_start_ms;
    unsigned long remaining_ms =
        (inflight_elapsed < WIFI_CONNECT_TIMEOUT_MS)
            ? (WIFI_CONNECT_TIMEOUT_MS - inflight_elapsed)
            : 0;
    unsigned long wait_ms = remaining_ms > 3000UL ? 3000UL : remaining_ms;
    if (wait_ms > 0) {
      unsigned long wait_start = millis();
      Serial.printf("[VOICE_WIFI] await_inflight elapsed_ms=%lu wait_ms=%lu status=%d state=%d\n",
                    inflight_elapsed,
                    wait_ms,
                    (int)WiFi.status(),
                    (int)wifi_state);
      while (wifi_connect_inflight &&
             WiFi.status() != WL_CONNECTED &&
             (millis() - wait_start) < wait_ms) {
        wifi_guard_poll();
        delay(100);
      }
      if (WiFi.status() == WL_CONNECTED) {
        wifi_guard_set_inflight(false);
        wifi_connected_ms = millis();
        wifi_guard_set_state(WIFI_STATE_CONNECTED, "voice_inflight_wait", WL_CONNECTED);
        Serial.println("[VOICE_WIFI] connect_ok inflight_wait");
        return true;
      }
    }
  }

  const uint8_t max_attempts = 3;
  for (uint8_t attempt = 1; attempt <= max_attempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("[VOICE_WIFI] reset attempt=%u status=%d inflight=%d state=%d\n",
                    (unsigned)attempt,
                    (int)WiFi.status(),
                    wifi_connect_inflight ? 1 : 0,
                    (int)wifi_state);
      WiFi.disconnect(true, true);
      delay(200);
      WiFi.mode(WIFI_OFF);
      delay(150);
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      WiFi.setSleep(false);
      esp_wifi_set_ps(WIFI_PS_NONE);
      delay(100);
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "voice_reset", WiFi.status());
    }

    Serial.printf("[VOICE_WIFI] connect attempt=%u status=%d inflight=%d state=%d\n",
                  (unsigned)attempt,
                  (int)WiFi.status(),
                  wifi_connect_inflight ? 1 : 0,
                  (int)wifi_state);
    if (ensure_wifi_connected("voice_upload", 12000)) {
      Serial.printf("[VOICE_WIFI] connect_ok attempt=%u\n", (unsigned)attempt);
      return true;
    }

    Serial.printf("[VOICE_WIFI] connect_fail attempt=%u status=%d inflight=%d state=%d\n",
                  (unsigned)attempt,
                  (int)WiFi.status(),
                  wifi_connect_inflight ? 1 : 0,
                  (int)wifi_state);
    if (attempt < max_attempts) {
      delay(600);
    }
  }

  return WiFi.status() == WL_CONNECTED;
}

static void voice_begin_wifi_preconnect() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[VOICE_WIFI] preconnect_skip already_connected");
    return;
  }
  if (upload_inflight || http_inflight) {
    Serial.printf("[VOICE_WIFI] preconnect_skip busy upload=%d http=%d owner=%s\n",
                  upload_inflight ? 1 : 0,
                  http_inflight ? 1 : 0,
                  wifi_guard_connect_owner());
    return;
  }
  if (wifi_connect_inflight) {
    unsigned long inflight_elapsed = wifi_inflight_start_ms > 0
                                         ? (millis() - wifi_inflight_start_ms)
                                         : 0;
    Serial.printf("[VOICE_WIFI] preconnect_skip inflight=1 elapsed_ms=%lu state=%d owner=%s\n",
                  inflight_elapsed,
                  (int)wifi_state,
                  wifi_guard_connect_owner());
    return;
  }
  Serial.printf("[VOICE_WIFI] preconnect_begin status=%d inflight=%d state=%d\n",
                (int)WiFi.status(),
                wifi_connect_inflight ? 1 : 0,
                (int)wifi_state);
  ensure_wifi_connected("voice_upload", 0);
  Serial.printf("[VOICE_WIFI] preconnect_started status=%d inflight=%d state=%d\n",
                (int)WiFi.status(),
                wifi_connect_inflight ? 1 : 0,
                (int)wifi_state);
}

// ── Voice session management ───────────────────────────────────────

static void voice_session_reset(const char* reason) {
  if (g_voice_session_id[0] != '\0') {
    Serial.printf("[VOICE_SESSION] reset reason=%s session_id=%s\n",
                  reason ? reason : "unknown",
                  g_voice_session_id);
  }
  g_voice_session_id[0] = '\0';
  g_voice_session_last_turn_ms = 0;
}

static const char* voice_session_get_or_create() {
  unsigned long now_ms = millis();
  bool expired = (g_voice_session_id[0] != '\0' &&
                  g_voice_session_last_turn_ms > 0 &&
                  (now_ms - g_voice_session_last_turn_ms) > VOICE_SESSION_IDLE_TIMEOUT_MS);
  if (expired) {
    voice_session_reset("idle_timeout");
  }
  if (g_voice_session_id[0] == '\0') {
    char device_id[32] = {0};
    load_runtime_device_id(device_id, sizeof(device_id));
    if (device_id[0] == '\0') {
      strncpy(device_id, "halo-device", sizeof(device_id) - 1);
      device_id[sizeof(device_id) - 1] = '\0';
    }
    snprintf(g_voice_session_id,
             sizeof(g_voice_session_id),
             "%s-%lu-%lu",
             device_id,
             (unsigned long)g_sense_boot_count,
             (unsigned long)now_ms);
    Serial.printf("[VOICE_SESSION] created session_id=%s\n", g_voice_session_id);
  } else {
    Serial.printf("[VOICE_SESSION] reuse session_id=%s idle_ms=%lu\n",
                  g_voice_session_id,
                  g_voice_session_last_turn_ms > 0 ? (unsigned long)(now_ms - g_voice_session_last_turn_ms) : 0UL);
  }
  return g_voice_session_id;
}

static void voice_session_note_response(const String& response_json) {
  g_voice_session_last_turn_ms = millis();
  if (response_json.length() == 0) {
    return;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, response_json);
  if (err) {
    Serial.printf("[VOICE_SESSION] response_meta_parse_failed error=%s\n", err.c_str());
    return;
  }

  const char* response_type = doc["type"] | "";
  const char* backend_session_id = "";
  bool memory_used = false;
  if (doc["app_output"].is<JsonObject>()) {
    JsonObject app_output = doc["app_output"].as<JsonObject>();
    if (app_output["meta"].is<JsonObject>()) {
      JsonObject meta = app_output["meta"].as<JsonObject>();
      backend_session_id = meta["session_id"] | "";
      memory_used = meta["memory_used"] | false;
    }
  }

  Serial.printf("[VOICE_SESSION] response type=%s memory_used=%d session_id=%s\n",
                response_type[0] ? response_type : "-",
                memory_used ? 1 : 0,
                backend_session_id[0] ? backend_session_id : g_voice_session_id);

  if (backend_session_id[0] != '\0' &&
      strcmp(backend_session_id, g_voice_session_id) != 0) {
    strncpy(g_voice_session_id, backend_session_id, sizeof(g_voice_session_id) - 1);
    g_voice_session_id[sizeof(g_voice_session_id) - 1] = '\0';
    Serial.printf("[VOICE_SESSION] backend_session_applied session_id=%s\n", g_voice_session_id);
  }
}

static void voice_request_finalize(const char* reason) {
  if (voice_finalize_requested) {
    Serial.printf("[VOICE] finalize duplicate ignored reason=%s job_type=%d state=%d\n",
                  reason ? reason : "unknown",
                  (int)current_job.type,
                  (int)current_job.state);
    return;
  }
  voice_finalize_requested = true;
  Serial.printf("[VOICE] finalize requested reason=%s job_type=%d state=%d\n",
                reason ? reason : "unknown",
                (int)current_job.type,
                (int)current_job.state);
  if (current_job.type == OP_VOICE && current_job.state == OP_RECORDING) {
    current_job.state = OP_FINALIZE;
    Serial.println("[OP] VOICE job signaled to finalize recording");
  }
}

// ── Voice audio capture ────────────────────────────────────────────

// Audio recording callback (called from I2S task when recording)
static void voice_audio_callback(const int16_t *samples, size_t num_samples) {
  if (!voice_recording_active || voice_audio_buffer == NULL) {
    return;
  }

  for (size_t i = 0; i < num_samples; ++i) {
    int32_t sample = samples[i];
    uint16_t abs_sample = (uint16_t)(sample < 0 ? -sample : sample);
    if (abs_sample > voice_peak_abs) {
      voice_peak_abs = abs_sample;
    }
    voice_sum_abs += abs_sample;
    voice_sample_count++;
    if (abs_sample > 8) {
      voice_nonzero_sample_count++;
    }
  }

  size_t bytes_to_write = num_samples * sizeof(int16_t);

  if (voice_audio_pos + bytes_to_write <= AUDIO_BUFFER_SIZE) {
    memcpy(voice_audio_buffer + voice_audio_pos, samples, bytes_to_write);
    voice_audio_pos += bytes_to_write;
    voice_audio_size = voice_audio_pos;
  } else {
    // Buffer overflow - stop recording
    Serial.println("[VOICE] Audio buffer overflow - stopping recording");
    voice_recording_active = false;
  }
}

// ── Voice upload ───────────────────────────────────────────────────

// Upload raw PCM audio to async quick-ack API.
// Success means backend durably accepted the work; firmware does not wait for result payloads.
// Backend now accepts raw PCM sample-rate via header, so firmware sends original 16 kHz capture directly.
static bool voice_upload_and_parse(const uint8_t* audio_buf, size_t audio_size, uint32_t voice_job_id) {
  if (audio_buf == NULL || audio_size == 0) {
    Serial.println("[VOICE] No audio data to upload");
    return false;
  }

  Serial.printf("[VOICE] Uploading %d bytes (raw PCM) to quick-ack API\n", audio_size);
  Serial.printf("[VOICE] Sending original PCM size=%u sample_rate=16000 format=s16le mono wav=0\n",
                (unsigned)audio_size);

  // Upload raw PCM to quick-ack API (no WAV header)
  String quick_ack_url = String(QUICK_ACK_BASE_URL) + String(QUICK_ACK_ENDPOINT);
  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));
  const char* session_id = voice_session_get_or_create();
  g_voice_session_last_turn_ms = millis();

  Serial.print("[VOICE] Uploading to: ");
  Serial.println(quick_ack_url);
  Serial.printf("[VOICE] PCM size: %u bytes (16 kHz, 16-bit LE, mono)\n", (unsigned)audio_size);
  Serial.printf("[VOICE] session_id=%s owner_id=%s device_id=%s\n",
                session_id ? session_id : "",
                owner_id,
                device_id[0] ? device_id : TREPO_DEVICE_ID);
  Serial.println("[VOICE] client_surface=halo");

  const uint8_t max_attempts = 2;
  uint32_t voice_http_job = voice_job_id;
  bool accepted = false;
  for (uint8_t attempt = 1; attempt <= max_attempts; ++attempt) {
    WiFiClientSecure client;
    HTTPClient http;
    bool http_locked = false;
    client.setInsecure();
    client.setTimeout(15000);

    http_queue_lock("VOICE_POST", voice_http_job);
    http_locked = true;

    if (!http.begin(client, quick_ack_url)) {
      Serial.printf("[VOICE] http.begin failed attempt=%u\n", (unsigned)attempt);
      log_http_failure_details("VOICE_QUICK_ACK_BEGIN", quick_ack_url.c_str(), HTTPC_ERROR_CONNECTION_REFUSED, &client);
      http_queue_unlock("VOICE_POST", voice_http_job);
      http_locked = false;
      if (attempt < max_attempts) {
        delay(250);
        continue;
      }
      break;
    }

    http.setReuse(false);
    http.setTimeout(60000);
    http.setConnectTimeout(10000);
    http.addHeader("Content-Type", "audio/pcm");  // Raw PCM, not WAV
    http.addHeader("x-owner-id", owner_id);
    http.addHeader("x-device-id", device_id[0] ? device_id : TREPO_DEVICE_ID);
    http.addHeader("x-client-surface", "halo");
    http.addHeader("x-audio-sample-rate", "16000");
    http.addHeader("x-audio-format", "pcm_s16le_mono");
    http.addHeader("x-session-id", session_id ? session_id : "");

    Serial.printf("[VOICE] HTTP POST attempt=%u/%u\n", (unsigned)attempt, (unsigned)max_attempts);
    int httpResponseCode = http.POST((uint8_t*)audio_buf, audio_size);
    Serial.printf("[VOICE] HTTP Response: %d\n", httpResponseCode);

    if (httpResponseCode >= 200 && httpResponseCode < 300) {
      Serial.printf("[VOICE] Async accept success: %d\n", httpResponseCode);
      http.end();
      client.stop();
      http_queue_unlock("VOICE_POST", voice_http_job);
      http_locked = false;
      accepted = true;
      break;
    }

    Serial.printf("[VOICE] Upload failed: %d attempt=%u\n",
                  httpResponseCode,
                  (unsigned)attempt);
    log_http_failure_details("VOICE_QUICK_ACK_POST", quick_ack_url.c_str(), httpResponseCode, &client);
    if (httpResponseCode > 0) {
      String error_response = http.getString();
      Serial.printf("[VOICE] Error response: %s\n", error_response.c_str());
    }

    http.end();
    client.stop();
    if (http_locked) {
      http_queue_unlock("VOICE_POST", voice_http_job);
      http_locked = false;
    }

    if (httpResponseCode < 0 && attempt < max_attempts) {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[VOICE] transport failure with Wi-Fi down, reconnecting before retry");
        if (!voice_ensure_wifi_connected()) {
          Serial.println("[VOICE] reconnect before retry failed");
          break;
        }
      } else {
        Serial.println("[VOICE] transient transport failure, retrying once");
      }
      delay(250);
      continue;
    }
    break;
  }

  if (!accepted) {
    Serial.println("[VOICE] Upload failed or backend did not accept request");
  }

  return accepted;
}

#endif // SENSE_VOICE_H

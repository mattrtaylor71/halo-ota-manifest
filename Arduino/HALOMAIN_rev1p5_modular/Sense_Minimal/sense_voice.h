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
#include <mbedtls/sha256.h>
#include "sense_media_network.h"

// ── Voice WiFi helpers ─────────────────────────────────────────────

static bool voice_ensure_wifi_connected() {
  SenseBackupWifiCall backup_call; if (!backup_call) return false;
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
    // Buffer full - stop recording, keep what we have
    Serial.println("[VOICE] Audio buffer full - stopping recording");
    voice_recording_active = false;
    voice_finalize_requested = true;  // Signal op_worker to proceed with what we have
  }
}

// ── Voice upload ───────────────────────────────────────────────────

static uint32_t sense_voice_crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (unsigned b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

static bool sense_voice_request_id_valid(const char* id) {
  if (!id || strnlen(id, 33) != 32) return false;
  for (unsigned i = 0; i < 32; ++i)
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  return true;
}

static bool sense_voice_envelope_valid(const UploadJob& job) {
  return job.is_voice && job.image_len > 0 && job.image_len <= 512U * 1024U &&
         (job.image_len & 1U) == 0 && job.voice.owner_id[0] &&
         job.voice.device_id[0] && job.voice.session_id[0] &&
         memchr(job.voice.owner_id, 0, sizeof(job.voice.owner_id)) &&
         memchr(job.voice.device_id, 0, sizeof(job.voice.device_id)) &&
         memchr(job.voice.session_id, 0, sizeof(job.voice.session_id)) &&
         job.voice.request_id[32] == 0 &&
         sense_voice_request_id_valid(job.voice.request_id);
}

static bool sense_voice_replay_age_ok(const UploadJob& job) {
  const time_t now = time(nullptr);
  return sense_time_has_fresh_sync() && job.created_epoch >= TIME_VALID_MIN_EPOCH &&
         now >= (time_t)job.created_epoch &&
         (uint64_t)(now - job.created_epoch) <= 7ULL * 86400ULL;
}

static bool sense_voice_owner_matches(const UploadJob& job) {
  char owner[64] = {}, device[32] = {};
  load_owner_id_or_default(owner, sizeof(owner));
  load_runtime_device_id(device, sizeof(device));
  return sense_voice_envelope_valid(job) && !strcmp(owner, job.voice.owner_id) &&
         !strcmp(device[0] ? device : TREPO_DEVICE_ID, job.voice.device_id);
}

static bool sense_voice_freeze_envelope(UploadJob& job) {
  load_owner_id_or_default(job.voice.owner_id, sizeof(job.voice.owner_id));
  load_runtime_device_id(job.voice.device_id, sizeof(job.voice.device_id));
  if (!job.voice.device_id[0])
    snprintf(job.voice.device_id, sizeof(job.voice.device_id), "%s", TREPO_DEVICE_ID);
  snprintf(job.voice.session_id, sizeof(job.voice.session_id), "%s", voice_session_get_or_create());
  g_voice_session_last_turn_ms = millis();
  snprintf(job.voice.request_id, sizeof(job.voice.request_id), "%08lx%08lx%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random());
  job.voice.crc32 = sense_voice_crc32(job.image_buf, job.image_len);
  const time_t now = time(nullptr);
  if (sense_time_has_fresh_sync() && now >= (time_t)TIME_VALID_MIN_EPOCH)
    job.created_epoch = (uint32_t)now;
  return sense_voice_envelope_valid(job);
}

static bool sense_voice_backend_ack(const UploadJob& job, int code, JsonDocument& ack) {
  if (code != 202 || !ack["accepted"].is<bool>() || !ack["accepted"].as<bool>() ||
      !ack["async"].is<bool>() || !ack["async"].as<bool>() ||
      !ack["duplicate"].is<bool>() || !ack["jobId"].is<const char*>()) return false;
  if (ack["duplicate"].as<bool>()) {
    const char* state = ack["status"] | "";
    if (strcmp(state, "accepted") && strcmp(state, "enqueued") &&
        strcmp(state, "processing") && strcmp(state, "completed")) return false;
  }
  char key[256], expected[65];
  const int n = snprintf(key, sizeof(key), "request:%s|%s|%s|%s", job.voice.owner_id,
                         job.voice.device_id, job.voice.session_id, job.voice.request_id);
  if (n < 0 || (size_t)n >= sizeof(key)) return false;
  uint8_t digest[32];
  if (mbedtls_sha256((const unsigned char*)key, (size_t)n, digest, 0) != 0) return false;
  for (unsigned i = 0; i < 32; ++i) snprintf(expected + 2 * i, 3, "%02x", digest[i]);
  return strcmp(expected, ack["jobId"].as<const char*>()) == 0;
}

// Upload raw PCM audio to async quick-ack API.
// Success means backend durably accepted the work; firmware does not wait for result payloads.
// Backend now accepts raw PCM sample-rate via header, so firmware sends original 16 kHz capture directly.
static bool voice_upload_and_parse(const UploadJob& job) {
  if (media_retry_network_cancelled()) return false;
  // Own the full transport lifetime: a list request cannot release/reacquire
  // the same camera reserve while this voice client's TLS objects are alive.
  if (!http_queue_lock("VOICE_POST", job.job_id)) return false;
  struct VoiceHttpLease {
    uint32_t job;
    ~VoiceHttpLease() { http_queue_unlock("VOICE_POST", job); }
  } http_lease{job.job_id};
  if (media_retry_network_cancelled()) return false;
  SenseBackupWifiCall backup_call; if (!backup_call) return false;
  const uint8_t* audio_buf = job.image_buf;
  const size_t audio_size = job.image_len;
  const uint32_t voice_job_id = job.job_id;
  if (audio_buf == NULL || audio_size == 0) {
    Serial.println("[VOICE] No audio data to upload");
    uart_send_sense_diag("voice", "no_audio", "VOICE_POST", 0, "empty_buffer");
    return false;
  }
  if (!sense_voice_owner_matches(job) || !sense_voice_replay_age_ok(job) ||
      sense_voice_crc32(audio_buf, audio_size) != job.voice.crc32) {
    uart_send_sense_diag("voice", "upload_blocked", "VOICE_POST", -1, "identity_or_crc");
    return false;
  }

  // Release camera DMA reservation to defragment internal SRAM for TLS.
  bool dma_was_reserved_voice = (g_camera_dma_reserve != nullptr);
  if (dma_was_reserved_voice) camera_dma_reserve_release("voice_upload");
  struct DmaGuardVoice {
    bool should_reacquire;
    ~DmaGuardVoice() {
      if (should_reacquire) {
        camera_dma_reserve_acquire("voice_upload");
      }
    }
  } dma_guard_voice{dma_was_reserved_voice};

  Serial.printf("[VOICE] Uploading %d bytes (raw PCM) to quick-ack API\n", audio_size);
  Serial.printf("[VOICE] Sending original PCM size=%u sample_rate=16000 format=s16le mono wav=0\n",
                (unsigned)audio_size);

  // Upload raw PCM to quick-ack API (no WAV header)
  String quick_ack_url = String(QUICK_ACK_BASE_URL) + String(QUICK_ACK_ENDPOINT);
  char owner_id[64] = {0};
  char device_id[32] = {0};
  snprintf(owner_id, sizeof(owner_id), "%s", job.voice.owner_id);
  snprintf(device_id, sizeof(device_id), "%s", job.voice.device_id);
  if (owner_id[0] == '\0') {
    Serial.println("[VOICE] ERROR: owner_id empty — device not fully provisioned");
    uart_send_sense_diag("voice", "error", "voice", -1, "owner_id_empty");
    return false;
  }
  const char* session_id = job.voice.session_id;

  Serial.print("[VOICE] Uploading to: ");
  Serial.println(quick_ack_url);
  Serial.printf("[VOICE] PCM size: %u bytes (16 kHz, 16-bit LE, mono)\n", (unsigned)audio_size);
  Serial.printf("[VOICE] session_id=%s owner_id=%s device_id=%s\n",
                session_id ? session_id : "",
                owner_id,
                device_id[0] ? device_id : TREPO_DEVICE_ID);
  Serial.println("[VOICE] client_surface=halo");

  HaloNtpDnsGuard ntp_dns_guard;
  const uint8_t max_attempts = media_voice_list_active() ? 1 : 2;
  bool accepted = false;
  for (uint8_t attempt = 1; attempt <= max_attempts; ++attempt) {
    if (media_retry_network_cancelled()) break;
    // Declared before the TLS client: reporting runs after its destruction.
    sense_memory::VoiceTrace memory_trace(voice_job_id, attempt);
    SenseMediaRetryClient client;
    HTTPClient http;
    client.setInsecure();
    client.setTimeout(media_voice_list_active() ? 1500 : 15000);
    client.setHandshakeTimeout(sense_media_network::handshake_timeout_seconds(media_voice_list_remaining_ms()));


    if (!http.begin(client, quick_ack_url)) {
      Serial.printf("[VOICE] http.begin failed attempt=%u\n", (unsigned)attempt);
      log_http_failure_details("VOICE_QUICK_ACK_BEGIN", quick_ack_url.c_str(), HTTPC_ERROR_CONNECTION_REFUSED, &client);
      if (attempt < max_attempts) {
        if (!media_retry_network_wait(250)) break;
        continue;
      }
      break;
    }

    http.setReuse(false);
    http.setTimeout(media_voice_list_active() ? 1500 : 60000);
    http.setConnectTimeout(sense_media_network::connect_timeout_ms(media_voice_list_remaining_ms()));
    http.addHeader("Content-Type", "audio/pcm");  // Raw PCM, not WAV
    http.addHeader("x-owner-id", owner_id);
    http.addHeader("x-device-id", device_id[0] ? device_id : TREPO_DEVICE_ID);
    http.addHeader("x-client-surface", "halo");
    http.addHeader("x-audio-sample-rate", "16000");
    http.addHeader("x-audio-format", "pcm_s16le_mono");
    http.addHeader("x-session-id", session_id ? session_id : "");
    http.addHeader("x-request-id", job.voice.request_id);

    Serial.printf("[VOICE] HTTP POST attempt=%u/%u\n", (unsigned)attempt, (unsigned)max_attempts);
    int httpResponseCode = http.POST((uint8_t*)audio_buf, audio_size);
    memory_trace.response(httpResponseCode);
    Serial.printf("[VOICE] HTTP Response: %d\n", httpResponseCode);
    if (media_retry_network_cancelled()) {
      http.end(); client.stop();
      break;
    }

    StaticJsonDocument<512> voice_ack;
    bool durable_ack = false;
    if (httpResponseCode == 202 && http.getSize() >= 0 && http.getSize() <= 512) {
      const String reply = http.getString();
      durable_ack = reply.length() <= 512 &&
                    deserializeJson(voice_ack, reply) == DeserializationError::Ok &&
                    sense_voice_backend_ack(job, httpResponseCode, voice_ack);
    }
    if (durable_ack && !media_retry_network_cancelled()) {
      Serial.printf("[VOICE] Async accept success: %d\n", httpResponseCode);
      uart_send_sense_diag("voice", "upload_ok", "VOICE_POST", (int32_t)httpResponseCode, "accepted");
      http.end();
      client.stop();
      accepted = true;
      break;
    }

    Serial.printf("[VOICE] Upload failed: %d attempt=%u\n",
                  httpResponseCode,
                  (unsigned)attempt);
    { char vfail[64];
      snprintf(vfail, sizeof(vfail), "attempt=%u/%u", (unsigned)attempt, (unsigned)max_attempts);
      uart_send_sense_diag("voice", "upload_fail", "VOICE_POST", (int32_t)httpResponseCode, vfail);
    }
    log_http_failure_details("VOICE_QUICK_ACK_POST", quick_ack_url.c_str(), httpResponseCode, &client);
    if (httpResponseCode > 0 && httpResponseCode != 202 && http.getSize() >= 0 && http.getSize() <= 512) {
      String error_response = http.getString();
      Serial.printf("[VOICE] Error response: %s\n", error_response.c_str());
    }

    http.end();
    client.stop();
    if (media_retry_network_cancelled()) break;

    if (attempt < max_attempts && sense_media_network::transient_http_status(httpResponseCode)) {
      // Retry the frozen request once. An HTTP response proves the transport
      // worked; resetting the radio does not help a busy/transient backend.
      Serial.println("[VOICE] Transient HTTP response, retrying original request");
      if (!media_retry_network_wait(250)) break;
      continue;
    }
    if (httpResponseCode < 0 && attempt < max_attempts) {
      // Saved media keeps its original copy. Let the next scheduled wake and
      // owner Wi-Fi maintenance retry instead of holding a radio-reset wait.
      if (media_retry_network_active()) break;
      Serial.println("[VOICE] TLS transport failure, hard WiFi reset before retry");
      wifi_hard_reset_and_reconnect("voice_tls_retry", 15000);
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[VOICE] WiFi reconnect failed after hard reset");
        break;
      }
      if (!media_retry_network_wait(250)) break;
      continue;
    }
    break;
  }

  if (!accepted) {
    Serial.println("[VOICE] Upload failed or backend did not accept request");
    diag_record_error_persistent("voice_upload", -1, "all_attempts_failed");
    uart_send_sense_diag("voice", "upload_rejected", "VOICE_POST", -1, "all_attempts_failed");
  }

  return accepted;
}

#endif // SENSE_VOICE_H

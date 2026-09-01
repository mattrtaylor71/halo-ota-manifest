/*
 * sense_upload_exec.h
 *
 * Upload execution: check-in presign and S3 PUT upload.
 *
 * The dish-result HTTP poller and the UI meal-result message were deleted
 * 2026-08-21 with the nutrition feature. Dish is now a plain capture-and-log.
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
 *   - scan_ui_status_emit(), flow_step() from sense_scan.h
 *   - uart_send_ui_toast() from sense_uart_msg.h
 *   - uart_send_sense_diag() from sense_diag.h
 *   - load_owner_id_or_default(), load_runtime_device_id()
 *   - CHECKIN_API_BASE_URL, CHECKIN_PRESIGN_ENDPOINT, API_KEY, BEARER_TOKEN
 *   - current_job (OpJob), ACTION_MIN_REMAINING_MS
 */

#ifndef SENSE_UPLOAD_EXEC_H
#define SENSE_UPLOAD_EXEC_H

// Bytes handed to tls.write() per call during the S3 PUT body.
//
// This is a MEMORY parameter, not a throughput one. Each call becomes one TLS
// record, and the ESP32-S3 hardware AES needs a contiguous DMA-capable internal
// block to encrypt it. By upload time the internal heap is already fragmented by
// the presign handshake, so a large record is the thing that cannot be served.
#ifndef UPLOAD_TLS_CHUNK_BYTES
#define UPLOAD_TLS_CHUNK_BYTES 512
#endif


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

  // Piggyback undelivered error telemetry
  char err_buf[512];
  uint32_t err_seq = sense_errlog_collect_json(err_buf, sizeof(err_buf), 3);
  if (err_seq > 0 && body.length() > 1) {
    body.remove(body.length() - 1);
    body += ",\"errors\":";
    body += err_buf;
    body += "}";
    Serial.printf("[CHECKIN_PRESIGN] piggyback %s (seq<=%lu)\n", err_buf, (unsigned long)err_seq);
  }

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

  // Presign responses are parsed on the HEAP, not a 768-byte stack buffer.
  //
  // The production backend returns ~1858 bytes -- the presigned put_url alone is
  // ~1614 chars of signed query string. StaticJsonDocument<768> silently ran out
  // of capacity, put_url came back TRUNCATED, and the PUT to the malformed URL
  // failed in 3 ms. Roughly half of all uploads failed this way
  // (2026-09-01 soak: 6 of 15 cycles) with a healthy-looking "Presign OK" first.
  //
  // Dev used to return short URLs, which is why this only appeared after the
  // prod cutover. Sizing to the real response plus headroom, on the heap so a
  // 3 KB buffer does not eat task stack. A short response costs nothing extra --
  // DynamicJsonDocument allocates what it needs.
  DynamicJsonDocument r(PRESIGN_RESPONSE_DOC_BYTES);
  if (resp_body.length() + 256 > PRESIGN_RESPONSE_DOC_BYTES) {
    Serial.printf("[PRESIGN] WARN response %u bytes vs %u capacity -- may truncate\n",
                  (unsigned)resp_body.length(), (unsigned)PRESIGN_RESPONSE_DOC_BYTES);
  }
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
    sense_errlog_mark_delivered(g_errlog_last_collect_seq);
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
                                 uint32_t deadline_ms,
                                 bool* aborted_for_budget) {
  Serial.printf("[UPLOAD] Starting PUT to S3, size: %u bytes\n", len);

  // Release camera DMA reservation to defragment internal SRAM for TLS.
  // The 16KB block sits mid-heap and prevents esp-aes from finding a
  // contiguous DMA region during TLS handshake. Camera is not used during
  // uploads; re-acquire at function exit via RAII guard.
  bool dma_was_reserved = (g_camera_dma_reserve != nullptr);
  if (dma_was_reserved) camera_dma_reserve_release("upload_put");
  // RAII guard: re-acquire DMA reservation on any return path
  struct DmaGuard {
    bool should_reacquire;
    ~DmaGuard() {
      if (should_reacquire) {
        camera_dma_reserve_acquire("upload_put");
      }
    }
  } dma_guard{dma_was_reserved};

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
    diag_record_error_persistent("upload_put", -1, "bad_url_parse");
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
  // The PUT fails for a MEMORY reason, not a network one, and that failure is
  // stochastic: consecutive attempts see different amounts of contiguous DMA and
  // one of them gets enough. Observed on a single drain, no camera init in the
  // boot at all:
  //   attempt 1  dma_largest=7668  failed
  //   attempt 2  dma_largest=9716  failed
  //   attempt 3                    PUT status: 200
  // Two attempts stopped one short of the win and threw the photo away.
  //
  // Attempt count comes from the measured distribution over a 30-cycle soak,
  // where 19 uploads needed at least one retry:
  //   failed at attempt 1: 19    attempt 2: 10    attempt 3: 10
  //   failed at attempt 4:  8    attempt 5:  6
  // Roughly half survive each retry and it converges slowly, so 5 attempts still
  // lost 6 uploads. Ten cheap retries put the expected loss under one per 30.
  //
  // A failed attempt dies in single-digit milliseconds, so ten of them cost
  // almost nothing; the budget goes entirely on the WiFi resets between them,
  // which is why those are pushed late (see below).
  const int put_max_attempts = 5;
  // A full WiFi hard reset costs up to 15s, and reasoning said it could not help
  // because it does not defragment the heap. MEASUREMENT SAYS OTHERWISE, so the
  // reasoning was wrong. Failures per attempt, same rig, same day:
  //
  //   reset from attempt 4 :  19 -> 10 -> 10 ->  8 ->  6      (halves each time)
  //   reset from attempt 9 :  27 -> 24 -> 22 -> 20 -> 20 ...  (barely converges)
  //
  // Cheap TLS-only retries recover far less than a reset does, so the resets are
  // doing real work and are kept early. The cost is bounded by the deadline check
  // at the top of the loop rather than by refusing to reset -- an unbounded retry
  // chain used to hold op_inflight for minutes, and every LCD sleep request in
  // that window came back "SLEEP_DENY reason=op_inflight". On a mostly-off device
  // that is the most expensive failure in this file.
  const int put_hard_reset_from_attempt = 4;
  bool write_ok = false;
  int put_attempt;
  WiFiClientSecure tls;
  // Retrying must never outlive the upload budget. The per-write deadline check
  // below only covers a write already in progress; without this, ten attempts
  // plus their settles could sail past it between attempts.
  for (put_attempt = 1; put_attempt <= put_max_attempts; put_attempt++) {
    if (put_attempt > 1 && deadline_expired(deadline_ms)) {
      Serial.printf("[UPLOAD] out of budget after %d attempts - giving the slot back\n",
                    put_attempt - 1);
      if (aborted_for_budget) *aborted_for_budget = true;
      presign_set_error_text("Upload timeout");
      uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "retry_budget");
      http_queue_unlock("UPLOAD_PUT", effective_job);
      return false;
    }
    tls.stop();
    // Validate the peer for the photo upload too.
    //
    // This was setInsecure() while presign, MQTT and the OTA paths all validated
    // — the one leg carrying user photo data was the one not checking who it was
    // talking to. The presigned URL is obtained over a validated channel, so this
    // was not wide open, but "the URL is secret" is not peer authentication.
    //
    // tls_configure() is the same helper the rest of the HTTP path uses: cert
    // bundle when available, else the pinned Amazon Root CA 1 (byte-identical to
    // the OTA/provisioning cert). S3 presigned URLs are Amazon-fronted, so that
    // chain validates.
    //
    // Heap is logged either side because THIS path has a heap-exhaustion panic
    // history (the AES-DMA fault fixed in 6.1.815) and validation costs more than
    // setInsecure. If free heap or the largest DMA block collapses here, revert.
    const uint32_t heap_before = ESP.getFreeHeap();
    // Peer validation stays ON. A/B tested 2026-08-21: reverting this to
    // setInsecure() made camera init failures WORSE (45 vs 20 baseline), so the
    // cert bundle is not what fragments the DMA region. No reason to weaken it.
    tls_configure(tls, "upload_put");
    Serial.printf("[UPLOAD_TLS] validated heap_before=%lu after=%lu delta=%ld dma_largest=%u\n",
                  (unsigned long)heap_before,
                  (unsigned long)ESP.getFreeHeap(),
                  (long)ESP.getFreeHeap() - (long)heap_before,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
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
        if (put_attempt >= put_hard_reset_from_attempt) {
          Serial.println("[UPLOAD] Hard WiFi reset before PUT retry (connect)");
          http_queue_unlock("UPLOAD_PUT", effective_job);
          wifi_hard_reset_and_reconnect("put_tls_connect", 15000);
          http_queue_lock("UPLOAD_PUT", effective_job);
        } else {
          Serial.printf("[UPLOAD] retrying PUT connect (attempt %d) without WiFi reset\n",
                        put_attempt + 1);
          delay(400);
        }
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

    // Dump the request target in chunks. A presigned S3 path is ~1500 chars and a
    // single Serial line that long gets truncated in transit, so a truncated URL
    // and an intact one look the same in the log. Chunking makes the difference
    // visible, and lets the exact device URL be replayed from a workstation.
    Serial.printf("[UPLOAD_URL] host=%s port=%u path_len=%u\n",
                  host.c_str(), (unsigned)port, (unsigned)path.length());
    for (size_t i = 0; i < path.length(); i += 100) {
      Serial.printf("[UPLOAD_URL] %03u:%s\n", (unsigned)i,
                    path.substring(i, i + 100).c_str());
    }

    // Header writes were fire-and-forget. If the request line already fails to go
    // out, every body write fails too and the log blames the body -- so check the
    // first write and report the headers as the failure when that is what it is.
    const int hdr_written = tls.printf("PUT %s HTTP/1.1\r\n", path.c_str());
    tls.printf("Host: %s\r\n", host.c_str());
    tls.printf("Content-Type: %s\r\n", resolved_ct);
    tls.printf("Content-Length: %u\r\n", (unsigned)len);
    tls.print("Connection: close\r\n\r\n");
    if (hdr_written <= 0) {
      char hdr_err[128] = {0};
      const int hdr_err_code = tls.lastError(hdr_err, sizeof(hdr_err));
      Serial.printf("[UPLOAD_PUT_FAIL] HEADER write failed rc=%d connected=%d errno=%d "
                    "tls_err=%d (%s)\n",
                    hdr_written, (int)tls.connected(), errno, hdr_err_code,
                    hdr_err[0] ? hdr_err : "-");
    }

    Serial.printf("[UPLOAD] Sending PUT request (attempt %d/%d)...\n", put_attempt, put_max_attempts);
    unsigned long upload_start = millis();

    size_t offset = 0;
    write_ok = true;

    const size_t chunk_size = UPLOAD_TLS_CHUNK_BYTES;
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
      size_t to_write = len - offset;
      if (to_write > chunk_size) {
        to_write = chunk_size;
      }
      int written = tls.write(buf + offset, to_write);
      if (written <= 0) {
        // Capture WHY, at the moment it happens. "write returned <=0" is not a
        // diagnosis -- an mbedtls alloc failure, a peer RST and a timeout all
        // look identical from here, and they have completely different fixes.
        char tls_err[128] = {0};
        const int tls_err_code = tls.lastError(tls_err, sizeof(tls_err));
        Serial.printf("[UPLOAD_PUT_FAIL] written=%d offset=%u/%u connected=%d errno=%d "
                      "tls_err=%d (%s)\n",
                      written, (unsigned)offset, (unsigned)len,
                      (int)tls.connected(), errno, tls_err_code,
                      tls_err[0] ? tls_err : "-");
        Serial.printf("[UPLOAD_PUT_FAIL] heap=%lu internal=%u dma_largest=%u int_largest=%u\n",
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
        if (put_attempt >= put_hard_reset_from_attempt) {
          Serial.println("[UPLOAD] Hard WiFi reset before PUT retry (write)");
          http_queue_unlock("UPLOAD_PUT", effective_job);
          wifi_hard_reset_and_reconnect("put_write_fail", 15000);
          http_queue_lock("UPLOAD_PUT", effective_job);
        } else {
          // Give the allocator a moment: TLS teardown returns internal SRAM
          // slightly after the socket closes, so the next attempt often sees a
          // larger contiguous block than this one did.
          // Escalating pause: TLS teardown returns internal SRAM slightly after
          // the socket closes, and a later attempt sees a larger contiguous
          // block than an immediate one would.
          const uint32_t settle_ms = (uint32_t)put_attempt * 250;
          Serial.printf("[UPLOAD] retrying PUT (attempt %d) without WiFi reset, settle=%lums\n",
                        put_attempt + 1, (unsigned long)(settle_ms > 1500 ? 1500 : settle_ms));
          delay(settle_ms > 1500 ? 1500 : settle_ms);
        }
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


#endif // SENSE_UPLOAD_EXEC_H

// Typed SD custody. One media upload per eligible wake; independent retry
// timers and rotating media priority provide bounded unattended recovery.
#pragma once

static constexpr uint32_t VOICE_SPOOL_TRANSACTION_MS = 120000;
static constexpr uint32_t VOICE_SPOOL_CLEANUP_MS = 2500;
static bool g_voice_spool_replayed_this_boot = false;
static bool g_media_spool_replayed_this_boot = false;
RTC_DATA_ATTR static char g_voice_spool_cursor[33] = {};
static uint32_t g_voice_spool_depth = 0;

static uint32_t sense_voice_spool_remaining(uint32_t deadline) {
  const int32_t remaining = (int32_t)(deadline - millis());
  return remaining > 0 ? (uint32_t)remaining : 0;
}

static uint32_t sense_voice_spool_step_deadline(uint32_t deadline, uint32_t maximum) {
  const uint32_t left = sense_voice_spool_remaining(deadline);
  return millis() + (left < maximum ? left : maximum);
}

static uint32_t sense_voice_spool_operation_deadline() {
  const uint32_t now = millis();
  uint32_t budget = VOICE_SPOOL_TRANSACTION_MS;
  if (GUARDIAN_FORCE_SLEEP_MS) {
    const uint32_t awake = (uint32_t)(now - guardian_awake_start_ms);
    const uint32_t left = awake < GUARDIAN_FORCE_SLEEP_MS ? GUARDIAN_FORCE_SLEEP_MS - awake : 0;
    const uint32_t available = left > 5000 ? left - 5000 : 0;
    if (available < budget) budget = available;
  }
  return now + budget;
}

class SenseVoiceUartLease {
 public:
  explicit SenseVoiceUartLease(uint32_t deadline) : held_(false) {
    const uint32_t left = sense_voice_spool_remaining(deadline);
    if (!left) return;
    // Shared lock order: admission TX, then the one raw RX owner.
    UartJsonTxLock tx;
    bool query_busy = false;
#ifdef HALO_SENSE_PROD_WRAPPER
    query_busy = sense_lcd_query_busy();
#endif
    if (!tx.held() || !sense_uart_ordinary_tx_allowed() || query_busy || !sense_voice_spool_remaining(deadline)) return;
    if (!uart_rx_mutex || xSemaphoreTake(uart_rx_mutex, pdMS_TO_TICKS(left < 1000 ? left : 1000)) != pdTRUE) return;
    if (!sense_uart_ordinary_tx_allowed() || uart_dispatch_depth.load() || !sense_voice_spool_remaining(deadline)) {
      xSemaphoreGive(uart_rx_mutex); return;
    }
    g_img_spool_tx_active = true; // existing sleep/OTA/JSON guards cover both payload kinds
    held_ = true;
  }
  ~SenseVoiceUartLease() {
    if (held_) {
      g_img_spool_tx_active = false;
      xSemaphoreGive(uart_rx_mutex);
    }
  }
  bool held() const { return held_; }
 private:
  bool held_;
};

static void sense_voice_spool_json(JsonDocument& d, const char* type) {
  d["ver"] = PROTOCOL_VERSION;
  d["type"] = type;
  d["msg_id"] = get_next_msg_id();
  d["ts"] = millis();
  d["voice_schema"] = 1;
  serializeJson(d, lcdSerial);
  lcdSerial.print('\n'); // one delimiter before COBS
  lcdSerial.flush();
}

static bool sense_voice_spool_read(JsonDocument& d, const char* type,
                                  const char* request, uint32_t deadline) {
  // Preserve the common collector's partial frame and all unrelated complete
  // JSON. A typed response may arrive between INPUT/SYNC retries; custody must
  // never discard them or run an action while the worker owns a payload.
  while (sense_voice_spool_remaining(deadline)) {
    while (lcdSerial.available() && sense_voice_spool_remaining(deadline)) {
      const char c = (char)lcdSerial.read();
      if (c == '\r') continue;
      if (c == '\n') {
        bool matched = false;
        uart_rx_frame[uart_rx_frame_len] = 0;
        if (!uart_rx_frame_overflow && uart_rx_frame_len) {
          d.clear();
          matched = deserializeJson(d, uart_rx_frame) == DeserializationError::Ok &&
              d["ver"].is<unsigned>() && d["ver"].as<unsigned>() == PROTOCOL_VERSION &&
              d["voice_schema"].is<unsigned>() && d["voice_schema"].as<unsigned>() == 1 &&
              !strcmp(d["type"] | "", type) &&
              (!request || !strcmp(d["request_id"] | "", request));
          if (!matched && uart_rx_frame_len + 1 <= UART_RX_RING_SIZE - uart_rx_ring_count) {
            for (size_t i = 0; i < uart_rx_frame_len; ++i) uart_ring_push(uart_rx_frame[i]);
            uart_ring_push('\n');
          } else if (!matched) uart_rx_dropped_since_frame += uart_rx_frame_len + 1;
        }
        uart_rx_frame_len = 0; uart_rx_frame_overflow = false;
        if (matched) return true; // leave following binary bytes for their owner
      } else if (uart_rx_is_printable(c)) {
        if (uart_rx_frame_len < UART_RX_FRAME_MAX && !uart_rx_frame_overflow)
          uart_rx_frame[uart_rx_frame_len++] = c;
        else uart_rx_frame_overflow = true;
      } else ++uart_rx_dropped_since_frame;
    }
    delay(2);
  }
  return false;
}

static void sense_voice_spool_identity(JsonDocument& d, const UploadJob& job) {
  d["request_id"] = job.voice.request_id;
  d["owner_id"] = job.voice.owner_id;
  d["device_id"] = job.voice.device_id;
}

static void sense_voice_spool_meta(JsonDocument& d, const UploadJob& job) {
  sense_voice_spool_identity(d, job);
  d["kind"] = "voice";
  d["pcm_fmt"] = "s16le_mono";
  d["rate"] = 16000;
  d["session_id"] = job.voice.session_id;
  d["job_id"] = job.job_id;
  d["len"] = (uint32_t)job.image_len;
  d["crc32"] = job.voice.crc32;
  d["epoch"] = job.created_epoch;
  d["retries"] = job.retries;
}

static bool sense_voice_spool_ready(JsonDocument& d, const UploadJob& job) {
  return d["ok"].is<unsigned>() && d["ok"].as<unsigned>() == 1 &&
         d["job_id"].is<uint32_t>() && d["job_id"].as<uint32_t>() == job.job_id &&
         d["len"].is<uint32_t>() && d["len"].as<uint32_t>() == job.image_len &&
         d["crc32"].is<uint32_t>() && d["crc32"].as<uint32_t>() == job.voice.crc32 &&
         !strcmp(d["request_id"] | "", job.voice.request_id);
}

static uint32_t sense_voice_spool_u32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// An old/late chunk ACK is not durable storage confirmation.
static bool sense_voice_spool_committed(uint8_t type, uint16_t seq, uint16_t expected,
                                       const uint8_t* payload, size_t len,
                                       const UploadJob& job) {
  return type == MSG_IMG_ACK && seq == expected && len == 40 &&
         sense_voice_spool_u32(payload) == job.image_len &&
         sense_voice_spool_u32(payload + 4) == job.voice.crc32 &&
         !memcmp(payload + 8, job.voice.request_id, 32);
}

static bool sense_voice_spool_cleanup(UartOtaProtocol& proto, const UploadJob& job,
                                     uint16_t seq, uint32_t deadline) {
  proto.send_frame(MSG_IMG_NACK, seq, nullptr, 0);
  lcdSerial.write((uint8_t)0);
  StaticJsonDocument<512> d;
  sense_voice_spool_identity(d, job);
  d["job_id"] = job.job_id;
  d["len"] = (uint32_t)job.image_len;
  d["crc32"] = job.voice.crc32;
  sense_voice_spool_json(d, "VOICE_XFER_ABORT");
  const bool idle = sense_voice_spool_read(d, "VOICE_XFER_ABORT_ACK", job.voice.request_id, deadline) &&
                    sense_voice_spool_ready(d, job) &&
                    d["json_ready"].is<bool>() && d["json_ready"].as<bool>();
  if (idle) sense_lcd_mode_confirm();
  // Missing cleanup proof retains the existing JSON quarantine, including its
  // persistent marker. Normal bounded LCD_OTA_QUERY can recover fresh idle proof.
  return idle;
}

static bool sense_voice_spool_store(const UploadJob& job, uint32_t deadline, bool* busy_refused = nullptr) {
  if (busy_refused) *busy_refused = false;
  if (job.from_voice_sd) return true; // failed replay still has its original committed slot
  if (!job.image_buf || !sense_voice_owner_matches(job) ||
      sense_voice_crc32(job.image_buf, job.image_len) != job.voice.crc32) return false;
  // Retain a retry hint even if the commit reply is lost after the LCD saved it.
  media_retry_inventory(halo_media_retry::VoiceSd, true);
  if (sense_voice_spool_remaining(deadline) <= VOICE_SPOOL_CLEANUP_MS) return false;
  const uint32_t work_deadline = deadline - VOICE_SPOOL_CLEANUP_MS;
  SenseVoiceUartLease lease(deadline);
  if (!lease.held()) return false;
  StaticJsonDocument<2048> d;
  sense_voice_spool_meta(d, job);
  UartOtaProtocol proto(&lcdSerial); proto.quiet = true;
  if (!proto.valid()) return false;
  // Persist quarantine before the peer can enter binary mode, never after.
  if (!sense_lcd_mode_before_begin()) return false;
  g_lcd_ota_mode_unconfirmed.store(true);
  sense_voice_spool_json(d, "VOICE_XFER_BEGIN");
  bool ok = false;
  uint16_t seq = 0;
  const uint32_t ready_deadline = sense_voice_spool_step_deadline(work_deadline, 15000);
  const bool got_ready = sense_voice_spool_read(d, "VOICE_XFER_READY", job.voice.request_id, ready_deadline);
  if (got_ready && d["ok"].is<unsigned>() && d["ok"].as<unsigned>() == 0 &&
      d["json_ready"].is<bool>() && d["json_ready"].as<bool>() &&
      d["job_id"].is<uint32_t>() && d["job_id"].as<uint32_t>() == job.job_id &&
      d["len"].is<uint32_t>() && d["len"].as<uint32_t>() == job.image_len &&
      d["crc32"].is<uint32_t>() && d["crc32"].as<uint32_t>() == job.voice.crc32) {
    sense_lcd_mode_confirm();
    if (busy_refused) *busy_refused = !strcmp(d["reason"] | "", "busy");
    return false; // ordinary refusal is not stored, but did not enter binary
  }
  if (got_ready &&
      sense_voice_spool_ready(d, job) && d["stored"].is<unsigned>()) {
    if (d["stored"].as<unsigned>() == 1) {
      sense_lcd_mode_confirm();
      media_retry_saved(halo_media_retry::VoiceSd);
      return true;
    }
    if (d["stored"].as<unsigned>() == 0) {
      delay(120);
      size_t off = 0;
      uint8_t frame[MAX_FRAME_SIZE];
      while (off < job.image_len && sense_voice_spool_remaining(work_deadline)) {
        const size_t n = (job.image_len - off < MAX_CHUNK_SIZE) ? job.image_len - off : MAX_CHUNK_SIZE;
        if (!proto.send_frame(MSG_IMG_CHUNK, seq, job.image_buf + off, n)) break;
        uint8_t type = 0; uint16_t received_seq = 0; size_t size = sizeof(frame);
        const uint32_t left = sense_voice_spool_remaining(work_deadline);
        if (!left || !proto.recv_frame(&type, &received_seq, frame, &size, left < 4000 ? left : 4000) ||
            type != MSG_IMG_ACK || received_seq != seq || size != 0) break;
        off += n; ++seq;
      }
      if (off == job.image_len && sense_voice_spool_remaining(work_deadline) &&
          proto.send_frame(MSG_IMG_END, seq, nullptr, 0)) {
        uint8_t type = 0; uint16_t received_seq = 0; size_t size = sizeof(frame);
        const uint32_t left = sense_voice_spool_remaining(work_deadline);
        ok = left && proto.recv_frame(&type, &received_seq, frame, &size, left < 4000 ? left : 4000) &&
             sense_voice_spool_committed(type, received_seq, seq, frame, size, job);
      }
    }
  }
  if (ok) {
    sense_lcd_mode_confirm();
    media_retry_saved(halo_media_retry::VoiceSd);
  } else sense_voice_spool_cleanup(proto, job, seq, deadline);
  return ok;
}

static bool sense_voice_spool_decode(JsonDocument& d, UploadJob& job) {
  JsonObject meta = d["meta"];
  if (meta.isNull() || strcmp(meta["kind"] | "", "voice") ||
      strcmp(meta["pcm_fmt"] | "", "s16le_mono") ||
      !meta["rate"].is<unsigned>() || meta["rate"].as<unsigned>() != 16000) return false;
  auto copy = [&](const char* key, char* dst, size_t cap) {
    if (!meta[key].is<const char*>()) return false;
    const char* text = meta[key].as<const char*>();
    if (!text[0] || strlen(text) >= cap) return false;
    memcpy(dst, text, strlen(text) + 1); return true;
  };
  job = UploadJob{};
  job.is_voice = true; job.from_voice_sd = true;
  snprintf(job.mode, sizeof(job.mode), "voice");
  if (!copy("owner_id", job.voice.owner_id, sizeof(job.voice.owner_id)) ||
      !copy("device_id", job.voice.device_id, sizeof(job.voice.device_id)) ||
      !copy("session_id", job.voice.session_id, sizeof(job.voice.session_id)) ||
      !copy("request_id", job.voice.request_id, sizeof(job.voice.request_id)) ||
      !meta["len"].is<uint32_t>() || !meta["crc32"].is<uint32_t>() ||
      !meta["job_id"].is<uint32_t>() || !meta["epoch"].is<uint32_t>() ||
      !meta["retries"].is<uint8_t>()) return false;
  job.image_len = meta["len"].as<uint32_t>();
  job.voice.crc32 = meta["crc32"].as<uint32_t>();
  job.job_id = meta["job_id"].as<uint32_t>();
  job.created_epoch = meta["epoch"].as<uint32_t>();
  job.retries = meta["retries"].as<uint8_t>();
  return sense_voice_owner_matches(job) && sense_voice_spool_ready(d, job);
}

static bool sense_voice_spool_delete(const UploadJob& job) {
  const uint32_t deadline = millis() + 4000;
  SenseVoiceUartLease lease(deadline);
  if (!lease.held()) return false;
  StaticJsonDocument<512> d;
  sense_voice_spool_identity(d, job);
  d["len"] = (uint32_t)job.image_len; d["crc32"] = job.voice.crc32;
  sense_voice_spool_json(d, "VOICE_SPOOL_DELETE");
  return sense_voice_spool_read(d, "VOICE_SPOOL_DELETE_ACK", job.voice.request_id, deadline) &&
         d["ok"].is<unsigned>() && d["ok"].as<unsigned>() == 1;
}

static bool sense_voice_spool_mark_attempt(UploadJob& job, uint32_t epoch) {
  const uint32_t deadline = millis() + 15000;
  SenseVoiceUartLease lease(deadline);
  if (!lease.held()) return false;
  StaticJsonDocument<512> d;
  sense_voice_spool_identity(d, job);
  d["job_id"] = job.job_id; d["len"] = (uint32_t)job.image_len;
  d["crc32"] = job.voice.crc32; d["epoch"] = epoch;
  sense_voice_spool_json(d, "VOICE_SPOOL_ATTEMPT");
  if (!sense_voice_spool_read(d, "VOICE_SPOOL_ATTEMPT_ACK", job.voice.request_id, deadline) ||
      !sense_voice_spool_ready(d, job) || !d["epoch"].is<uint32_t>() || d["epoch"].as<uint32_t>() != epoch) return false;
  job.created_epoch = epoch;
  return true;
}

static bool sense_voice_spool_fetch(UploadJob& job) {
  const uint32_t deadline = sense_voice_spool_operation_deadline();
  if (sense_voice_spool_remaining(deadline) <= VOICE_SPOOL_CLEANUP_MS) return false;
  const uint32_t work_deadline = deadline - VOICE_SPOOL_CLEANUP_MS;
  SenseVoiceUartLease lease(deadline);
  if (!lease.held()) return false;
  StaticJsonDocument<2048> d;
  char owner[64] = {}, device[32] = {};
  load_owner_id_or_default(owner, sizeof(owner)); load_runtime_device_id(device, sizeof(device));
  if (!owner[0]) return false;
  char request[33] = {};
  const uint32_t list_deadline = sense_voice_spool_step_deadline(work_deadline, 15000);
  for (unsigned scan = 0; scan < 4 && sense_voice_spool_remaining(list_deadline); ++scan) {
    d.clear();
    d["owner_id"] = owner; d["device_id"] = device[0] ? device : TREPO_DEVICE_ID;
    if (sense_voice_request_id_valid(g_voice_spool_cursor)) d["after_request_id"] = g_voice_spool_cursor;
    char probe[17] = {};
    snprintf(probe, sizeof(probe), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    d["probe"] = probe;
    sense_voice_spool_json(d, "VOICE_SPOOL_LIST_REQ");
    if (!sense_voice_spool_read(d, "VOICE_SPOOL_LIST", nullptr, list_deadline) ||
        !d["ok"].is<unsigned>() || d["ok"].as<unsigned>() != 1 ||
        (strcmp(d["reason"] | "", "ok") && strcmp(d["reason"] | "", "empty")) ||
        strcmp(d["probe"] | "", probe) || strcmp(d["owner_id"] | "", owner) ||
        strcmp(d["device_id"] | "", device[0] ? device : TREPO_DEVICE_ID) ||
        !d["count"].is<uint32_t>()) return false;
    g_voice_spool_depth = d["count"].as<uint32_t>();
    media_retry_inventory(halo_media_retry::VoiceSd, g_voice_spool_depth != 0);
    if (!g_voice_spool_depth) { g_voice_spool_cursor[0] = 0; return false; }
    const char* id = d["request_id"] | "";
    if (!sense_voice_request_id_valid(id)) { g_voice_spool_cursor[0] = 0; continue; }
    snprintf(request, sizeof(request), "%s", id);
    if (!d["epoch"].is<uint32_t>()) return false;
    const uint32_t epoch = d["epoch"].as<uint32_t>();
    const time_t now = time(nullptr);
    if (epoch == 0 || (sense_time_has_fresh_sync() && epoch >= TIME_VALID_MIN_EPOCH &&
        now >= (time_t)epoch && (uint64_t)(now - epoch) <= 7ULL * 86400ULL)) break;
    // Cursor refers to LCD's commit ordinal, not random request-ID ordering.
    // Held/expired records remain present and do not block newer commands.
    snprintf(g_voice_spool_cursor, sizeof(g_voice_spool_cursor), "%s", request);
    request[0] = 0;
  }
  if (!request[0]) return false;
  d.clear(); d["request_id"] = request;
  d["owner_id"] = owner; d["device_id"] = device[0] ? device : TREPO_DEVICE_ID;
  UartOtaProtocol proto(&lcdSerial); proto.quiet = true;
  if (!proto.valid()) return false;
  if (!sense_lcd_mode_before_begin()) return false;
  g_lcd_ota_mode_unconfirmed.store(true);
  sense_voice_spool_json(d, "VOICE_SPOOL_FETCH");
  uint16_t seq = 0;
  bool ok = false;
  const bool got_ready = sense_voice_spool_read(d, "VOICE_SPOOL_FETCH_READY", request, sense_voice_spool_step_deadline(work_deadline, 15000));
  if (got_ready && d["ok"].is<unsigned>() && d["ok"].as<unsigned>() == 0 &&
      d["json_ready"].is<bool>() && d["json_ready"].as<bool>()) {
    sense_lcd_mode_confirm();
    return false;
  }
  if (got_ready &&
      sense_voice_spool_decode(d, job)) {
    job.image_buf = (uint8_t*)heap_caps_malloc(job.image_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (job.image_buf) {
      size_t off = 0;
      uint8_t frame[MAX_FRAME_SIZE];
      while (sense_voice_spool_remaining(work_deadline)) {
        uint8_t type = 0; uint16_t received_seq = 0; size_t size = sizeof(frame);
        const uint32_t left = sense_voice_spool_remaining(work_deadline);
        if (!proto.recv_frame(&type, &received_seq, frame, &size, left < 4000 ? left : 4000) || received_seq != seq) break;
        if (type == MSG_IMG_END) {
          ok = size == 40 && sense_voice_spool_u32(frame) == job.image_len &&
               sense_voice_spool_u32(frame + 4) == job.voice.crc32 &&
               !memcmp(frame + 8, job.voice.request_id, 32) &&
               off == job.image_len && sense_voice_crc32(job.image_buf, off) == job.voice.crc32;
          const bool acknowledged = proto.send_frame(ok ? MSG_IMG_ACK : MSG_IMG_NACK, seq, nullptr, 0);
          ok = ok && acknowledged;
          break;
        }
        if (type != MSG_IMG_CHUNK || !size || size > job.image_len - off) break;
        memcpy(job.image_buf + off, frame, size); off += size;
        proto.send_frame(MSG_IMG_ACK, seq, nullptr, 0); ++seq;
      }
    }
  }
  if (ok) {
    // Our ACK may be lost while the LCD still owns its sender. Payload proof
    // is separate from line-mode proof: join the peer's actual closed stream
    // via the request-bound ABORT/json_ready exchange before ordinary JSON.
    ok = sense_voice_spool_cleanup(proto, job, seq, deadline);
    if (!ok) { free(job.image_buf); job.image_buf = nullptr; }
  }
  else {
    // Use requested identity even if malformed metadata prevented decode.
    snprintf(job.voice.request_id, sizeof(job.voice.request_id), "%s", request);
    sense_voice_spool_cleanup(proto, job, seq, deadline);
    if (job.image_buf) free(job.image_buf);
    job.image_buf = nullptr;
  }
  return ok;
}

static void sense_voice_spool_replay_tick() {
  if (g_media_retry_user_paused.load() || g_media_spool_replayed_this_boot || g_voice_spool_replayed_this_boot || !wifi_is_connected() || upload_inflight ||
      upload_queue_count() || foreground_active || current_job.active || voice_recording_active ||
      dish_scan_inflight || scan_ui_inflight || !sense_uart_ordinary_tx_allowed()) return;
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active() || halo_prod_boot_ota_pending()) return;
#endif
  if (!sense_time_has_fresh_sync()) return;
  // One probe/transfer per kind per wake; the scheduler rotates first choice.
  g_voice_spool_replayed_this_boot = true;
  UploadJob job = {};
  const bool fetched = sense_voice_spool_fetch(job);
  const bool eligible = fetched && (job.created_epoch == 0 || sense_voice_replay_age_ok(job));
  const bool queued = eligible && queue_voice_upload_job(job.job_id, job.image_buf, job.image_len,
                                        job.retries, false, job.created_epoch, &job.voice, true);
  if (queued) {
    g_media_spool_replayed_this_boot = true;
    media_retry_attempted(halo_media_retry::VoiceSd);
    // Advance even if the cloud later rejects this request. Keep its original
    // durable slot; another pending capture must get a turn on the next wake.
    snprintf(g_voice_spool_cursor, sizeof(g_voice_spool_cursor), "%s", job.voice.request_id);
  }
  if (fetched && !queued) {
    free(job.image_buf); // original committed SD slot remains untouched
  }
  uart_send_sense_diag("voice", queued ? "sd_replay_queued" : "sd_pending",
                       "voice", (int32_t)g_voice_spool_depth,
                       queued ? "original_slot_kept" : (fetched ? "age_or_queue_blocked" : "empty_or_unavailable"));
}

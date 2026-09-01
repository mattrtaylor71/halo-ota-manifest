// sense_spool_drain.h — pull spooled captures back off the LCD's SD card and
// upload them.
//
// WHY: sense_img_spool.h saves a photo to the LCD's SD card when SPIFFS cannot
// (every real capture: the partition is 173,441 B and images are 177-189 KB).
// Without this file those images are written and never sent — durable, and
// useless. This is the other half of "capture reliably AND get it to the
// backend".
//
// SHAPE: the Sense owns WiFi/TLS/presign/retry and the LCD owns the card, so the
// bytes come back over the same 115200 UART (~16s each). That cost is paid only
// when the device is idle and the user has walked away, which is exactly when
// the spool has something in it.
//
// A state machine rather than a blocking call: the SPOOL_LIST / SPOOL_FETCH_READY
// replies arrive through the normal RX dispatch, and blocking the main loop for
// 16s would stall INPUT_* handling — a user walking up mid-drain must not wait.
//
// DELETION IS THE SUBTLE PART. The slot is removed only after the upload is
// CONFIRMED (Sense_Minimal.ino, the put-success path). Deleting at queue time
// would be a silent regression to the original bug: an upload that then failed
// would have nowhere to fall back to, because SPIFFS still cannot hold the image.

#pragma once

#include <Arduino.h>

#ifndef SPOOL_DRAIN_INTERVAL_MS
#define SPOOL_DRAIN_INTERVAL_MS 30000   // how often to ask when idle
#endif
#ifndef SPOOL_DRAIN_REPLY_TIMEOUT_MS
#define SPOOL_DRAIN_REPLY_TIMEOUT_MS 4000
#endif
#ifndef SPOOL_DRAIN_FRAME_TIMEOUT_MS
#define SPOOL_DRAIN_FRAME_TIMEOUT_MS 5000
#endif

enum SpoolDrainState : uint8_t {
  SPOOL_IDLE = 0,
  SPOOL_WAIT_LIST,
  SPOOL_WAIT_READY,
  SPOOL_RECEIVING,
};

static SpoolDrainState g_spool_state = SPOOL_IDLE;
// g_spool_owns_uart is declared early in Sense_Minimal.ino, before sense_uart.h,
// because pump_uart_rx_once() must test it and that header is included long
// before this one.
static uint32_t g_spool_slot = 0;
static uint32_t g_spool_len = 0;
static uint32_t g_spool_got = 0;
static uint16_t g_spool_next_seq = 0;
static uint8_t* g_spool_buf = nullptr;
static UploadJob g_spool_job = {};
static uint32_t g_spool_deadline_ms = 0;
static uint32_t g_spool_next_try_ms = 0;
static UartOtaProtocol* g_spool_proto = nullptr;

// Set once a drained image is queued; cleared when the upload is confirmed (and
// the slot deleted) or when it fails (slot kept for another attempt).
static uint32_t g_drain_slot_inflight = 0;
static uint32_t g_drain_job_id = 0;

static uint32_t g_spool_drained_ok = 0;
static uint32_t g_spool_drain_fails = 0;
// Last spool depth the LCD reported. Recorded in the wake log so an overnight
// run shows whether images are accumulating on the card rather than draining.
static uint16_t g_spool_last_known_depth = 0;

#if HALO_SPOOL_TEST
// Bench-only: run the drain but stop before queueing the upload, and instead
// verify the received bytes against the spooltest pattern.
//
// The spooltest image is a synthetic 180KB pattern, not a real JPEG. Driving it
// through the full drain would PUT it to the real backend and create a junk
// check-in in someone's actual kitchen. This exercises everything that is new
// and risky - SPOOL_LIST, SPOOL_FETCH, the reverse COBS transfer, the metadata
// round-trip - and stops at the one step that has side effects off-device.
static bool g_spool_verify_only = false;
#endif

static void sense_spool_drain_reset(const char* why) {
  if (g_spool_buf) { free(g_spool_buf); g_spool_buf = nullptr; }
  if (g_spool_state != SPOOL_IDLE) {
    Serial.printf("[SPOOL_DRAIN] reset state=%d reason=%s\n", (int)g_spool_state, why ? why : "");
  }
  g_spool_state = SPOOL_IDLE;
  g_spool_owns_uart = false;
  g_spool_slot = g_spool_len = g_spool_got = 0;
  g_spool_next_seq = 0;
  g_spool_next_try_ms = millis() + SPOOL_DRAIN_INTERVAL_MS;
}

// A user action arrived mid-drain. The drain is strictly lower priority than
// anything the person standing at the device is doing, so give the link back.
static void sense_spool_drain_yield_to_user() {
  if (g_spool_state == SPOOL_IDLE) return;

  // Tell the LCD to stop RIGHT NOW if a transfer is actually in flight.
  //
  // Abandoning silently leaves the LCD streaming into a Sense that has stopped
  // listening, and it only notices after a 4s ack timeout — four seconds of the
  // link tied up by a dead transfer at precisely the moment someone is standing
  // at the device. A NACK is what lcd_spool_send_file() already treats as an
  // abort (it checks `rtype != MSG_IMG_ACK`), so this needs no new protocol.
  //
  // Sent through the proto rather than as JSON because the link is mid-COBS:
  // a JSON line here would be exactly the stream corruption fixed three times
  // elsewhere.
  if (g_spool_state == SPOOL_RECEIVING && g_spool_proto) {
    g_spool_proto->send_frame(MSG_IMG_NACK, g_spool_next_seq, nullptr, 0);
    Serial.println("[SPOOL_DRAIN] user acted - NACKed to stop the LCD immediately");
  }
  sense_spool_drain_reset("user_input");
  g_spool_drain_fails++;
}

static bool sense_spool_drain_conditions_ok() {
  if (!wifi_is_connected()) return false;
  if (upload_inflight || upload_queue_count() > 0) return false;
  if (dish_scan_inflight || scan_ui_inflight) return false;
  if (g_drain_slot_inflight != 0) return false;   // one at a time
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active()) return false;
#endif
  return true;
}

// Called from the main loop.
static void sense_spool_drain_tick() {
  const uint32_t now = millis();

  if (g_spool_state != SPOOL_IDLE && g_spool_deadline_ms &&
      (int32_t)(now - g_spool_deadline_ms) >= 0) {
    sense_spool_drain_reset("timeout");
    g_spool_drain_fails++;
    return;
  }
  if (g_spool_state != SPOOL_IDLE) return;
  if (g_spool_next_try_ms && (int32_t)(now - g_spool_next_try_ms) < 0) return;
  if (!sense_spool_drain_conditions_ok()) {
    g_spool_next_try_ms = now + SPOOL_DRAIN_INTERVAL_MS;
    return;
  }

  StaticJsonDocument<128> d;
  d["ver"] = PROTOCOL_VERSION;
  d["type"] = "SPOOL_LIST_REQ";
  d["msg_id"] = get_next_msg_id();
  d["ts"] = now;
  String out; serializeJson(d, out);
  uart_send_json(out.c_str());
  g_spool_state = SPOOL_WAIT_LIST;
  g_spool_deadline_ms = now + SPOOL_DRAIN_REPLY_TIMEOUT_MS;
}

#if HALO_SPOOL_TEST
// Bench-only: kick a drain immediately, bypassing the idle/WiFi/queue gating so
// the transfer can be tested on demand rather than by waiting for the device to
// go quiet at the right moment.
static void sense_spool_drain_force(bool verify_only) {
  sense_spool_drain_reset("force");
  g_spool_verify_only = verify_only;
  g_spool_next_try_ms = 0;
  StaticJsonDocument<128> d;
  d["ver"] = PROTOCOL_VERSION;
  d["type"] = "SPOOL_LIST_REQ";
  d["msg_id"] = get_next_msg_id();
  d["ts"] = millis();
  String out; serializeJson(d, out);
  uart_send_json(out.c_str());
  g_spool_state = SPOOL_WAIT_LIST;
  g_spool_deadline_ms = millis() + SPOOL_DRAIN_REPLY_TIMEOUT_MS;
  Serial.printf("[DRAINTEST] forced drain (verify_only=%d)\n", verify_only ? 1 : 0);
}
#endif

// SPOOL_LIST from the LCD: what is waiting, plus the sidecar we need to rebuild
// the UploadJob. Everything except the pixels comes from here.
static void sense_spool_on_list(JsonDocument& doc) {
  if (g_spool_state != SPOOL_WAIT_LIST) return;
  const uint32_t slot = (uint32_t)(doc["slot"] | 0);
  const uint32_t len  = (uint32_t)(doc["len"] | 0);
  const uint32_t count = (uint32_t)(doc["count"] | 0);
  g_spool_last_known_depth = (uint16_t)count;   // for the wake log
  if (slot == 0 || len == 0) {
    sense_spool_drain_reset("empty");
    return;
  }
  JsonObject meta = doc["meta"];
  if (meta.isNull()) {
    // No sidecar means we cannot know the mode; uploading it would risk filing
    // a check-in as a dish. Leave it on the card and say so.
    Serial.printf("[SPOOL_DRAIN] slot=%lu has no metadata - skipping\n", (unsigned long)slot);
    sense_spool_drain_reset("no_meta");
    return;
  }

  g_spool_job = UploadJob{};
  g_spool_job.job_id   = (uint32_t)(meta["job_id"] | 0);
  g_spool_job.is_voice = ((int)(meta["is_voice"] | 0) != 0);
  snprintf(g_spool_job.mode, sizeof(g_spool_job.mode), "%s",
           (const char*)(meta["mode"] | ""));
  snprintf(g_spool_job.expiry_date, sizeof(g_spool_job.expiry_date), "%s",
           (const char*)(meta["expiry"] | ""));
  g_spool_job.quantity             = (uint16_t)(meta["qty"] | 1);
  g_spool_job.add_to_shopping_list = ((int)(meta["add_list"] | 0) != 0);
  g_spool_job.retries              = (uint8_t)(meta["retries"] | 0);
  g_spool_job.created_epoch        = (uint32_t)(meta["epoch"] | 0);
  JsonObject cm = meta["cam"];
  if (!cm.isNull()) {
    g_spool_job.camera_meta.profile             = (uint8_t)(cm["p"] | 0);
    g_spool_job.camera_meta.flash_enabled       = (uint8_t)(cm["f"] | 0);
    g_spool_job.camera_meta.jpeg_quality        = (uint8_t)(cm["q"] | 0);
    g_spool_job.camera_meta.actual_width        = (uint16_t)(cm["w"] | 0);
    g_spool_job.camera_meta.actual_height       = (uint16_t)(cm["h"] | 0);
    g_spool_job.camera_meta.configured_framesize= (uint16_t)(cm["fs"] | 0);
    g_spool_job.camera_meta.scene_luma          = (int16_t)(cm["l"] | 0);
    g_spool_job.camera_meta.scene_green_ratio   = (int16_t)(cm["g"] | 0);
    g_spool_job.camera_meta.xclk_hz             = (uint32_t)(cm["x"] | 0);
  }

  g_spool_buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!g_spool_buf) {
    Serial.printf("[SPOOL_DRAIN] alloc failed for %lu bytes\n", (unsigned long)len);
    sense_spool_drain_reset("alloc");
    g_spool_drain_fails++;
    return;
  }
  g_spool_slot = slot; g_spool_len = len; g_spool_got = 0; g_spool_next_seq = 0;

  Serial.printf("[SPOOL_DRAIN] fetching slot=%lu len=%lu mode=%s (queue depth %lu)\n",
                (unsigned long)slot, (unsigned long)len, g_spool_job.mode,
                (unsigned long)count);

  StaticJsonDocument<128> d;
  d["ver"] = PROTOCOL_VERSION;
  d["type"] = "SPOOL_FETCH";
  d["msg_id"] = get_next_msg_id();
  d["ts"] = millis();
  d["slot"] = slot;
  String out; serializeJson(d, out);
  uart_send_json(out.c_str());
  g_spool_state = SPOOL_WAIT_READY;
  g_spool_deadline_ms = millis() + SPOOL_DRAIN_REPLY_TIMEOUT_MS;
}

static void sense_spool_on_fetch_ready(JsonDocument& doc) {
  if (g_spool_state != SPOOL_WAIT_READY) return;
  if ((int)(doc["ok"] | 0) != 1) {
    sense_spool_drain_reset("lcd_not_ready");
    g_spool_drain_fails++;
    return;
  }
  // The LCD waits 120ms after this reply before its first frame. Drain whatever
  // the handshake left behind: a single leftover byte would be consumed as the
  // first COBS byte and shift the entire decode (that exact bug cost a full
  // debug cycle on the inbound direction).
  const uint32_t quiet_deadline = millis() + 60;
  uint32_t last_byte_ms = millis();
  while ((int32_t)(millis() - quiet_deadline) < 0) {
    if (lcdSerial.available()) { lcdSerial.read(); last_byte_ms = millis(); }
    else if ((millis() - last_byte_ms) >= 10) break;
    else delay(1);
  }
  g_spool_owns_uart = true;          // JSON line reader stands down
  g_spool_state = SPOOL_RECEIVING;
  g_spool_deadline_ms = millis() + 60000;   // whole-transfer bound
}

// Pump inbound frames while the drain owns the port. Called from the main loop.
static void sense_spool_receive_pump() {
  if (!g_spool_owns_uart) return;
  if (!g_spool_proto) {
    g_spool_proto = new UartOtaProtocol(&lcdSerial);
    if (!g_spool_proto) { sense_spool_drain_reset("no_proto"); return; }
    g_spool_proto->quiet = true;
  }
  uint8_t type = 0; uint16_t seq = 0;
  static uint8_t frame[MAX_FRAME_SIZE];
  size_t len = sizeof(frame);        // IN/OUT: must carry capacity IN
  if (!g_spool_proto->recv_frame(&type, &seq, frame, &len, SPOOL_DRAIN_FRAME_TIMEOUT_MS)) {
    sense_spool_drain_reset("frame_timeout");
    g_spool_drain_fails++;
    return;
  }
  switch (type) {
    case MSG_IMG_CHUNK:
      if (seq != g_spool_next_seq || (g_spool_got + len) > g_spool_len) {
        // A gap would silently corrupt the image; refuse rather than write past.
        Serial.printf("[SPOOL_DRAIN] seq/size fault seq=%u want=%u got=%lu+%u len=%lu\n",
                      (unsigned)seq, (unsigned)g_spool_next_seq,
                      (unsigned long)g_spool_got, (unsigned)len,
                      (unsigned long)g_spool_len);
        g_spool_proto->send_frame(MSG_IMG_NACK, seq, nullptr, 0);
        sense_spool_drain_reset("seq_gap");
        g_spool_drain_fails++;
        return;
      }
      memcpy(g_spool_buf + g_spool_got, frame, len);
      g_spool_got += len;
      g_spool_next_seq++;
      g_spool_proto->send_frame(MSG_IMG_ACK, seq, nullptr, 0);
      break;

    case MSG_IMG_END: {
      const bool complete = (g_spool_got == g_spool_len);
      g_spool_proto->send_frame(complete ? MSG_IMG_ACK : MSG_IMG_NACK, seq, nullptr, 0);
      g_spool_owns_uart = false;
      if (!complete) {
        Serial.printf("[SPOOL_DRAIN] incomplete got=%lu expect=%lu\n",
                      (unsigned long)g_spool_got, (unsigned long)g_spool_len);
        sense_spool_drain_reset("incomplete");
        g_spool_drain_fails++;
        return;
      }
#if HALO_SPOOL_TEST
      if (g_spool_verify_only) {
        // Byte-for-byte check against the pattern the Sense wrote in spooltest.
        // A matching LENGTH proves nothing: a framing bug that swaps, drops or
        // repeats bytes preserves length exactly.
        uint32_t bad = 0; long first_bad = -1;
        for (uint32_t i = 0; i < g_spool_len; i++) {
          if (g_spool_buf[i] != (uint8_t)((i * 31 + 7) & 0xFF)) {
            if (first_bad < 0) first_bad = (long)i;
            bad++;
          }
        }
        Serial.printf("[DRAINTEST] slot=%lu bytes=%lu mismatches=%lu first_bad=%ld -> %s\n",
                      (unsigned long)g_spool_slot, (unsigned long)g_spool_got,
                      (unsigned long)bad, first_bad,
                      (bad == 0 && g_spool_got == g_spool_len) ? "PASS" : "FAIL");
        g_spool_verify_only = false;
        sense_spool_drain_reset("verify_done");   // frees the buffer, keeps the slot
        return;
      }
#endif
      // Hand to the normal upload path. from_persisted=true so the existing
      // retry/telemetry treats it as a replay, which is what it is.
      const bool queued = queue_upload_job(g_spool_job.job_id,
                                           g_spool_job.mode,
                                           g_spool_job.expiry_date,
                                           g_spool_job.quantity,
                                           g_spool_job.add_to_shopping_list,
                                           &g_spool_job.camera_meta,
                                           g_spool_buf,
                                           g_spool_len,
                                           g_spool_job.retries,
                                           true,
                                           g_spool_job.created_epoch);
      if (queued) {
        // Ownership of the buffer passes to the queue.
        g_drain_slot_inflight = g_spool_slot;
        g_drain_job_id = g_spool_job.job_id;
        g_spool_buf = nullptr;
        Serial.printf("[SPOOL_DRAIN] slot=%lu queued for upload job_id=%lu mode=%s\n",
                      (unsigned long)g_spool_slot, (unsigned long)g_spool_job.job_id,
                      g_spool_job.mode);
      } else {
        Serial.println("[SPOOL_DRAIN] queue failed - leaving image on SD");
        g_spool_drain_fails++;
      }
      sense_spool_drain_reset(queued ? "queued" : "queue_failed");
      return;
    }

    default:
      Serial.printf("[SPOOL_DRAIN] unexpected frame type=0x%02X\n", (unsigned)type);
      sense_spool_drain_reset("wrong_frame_type");
      g_spool_drain_fails++;
      return;
  }
}

// Called from the upload success path. Only now is it safe to drop the slot:
// until the bytes are actually at the backend, the SD copy is the only copy.
static void sense_spool_on_upload_result(uint32_t job_id, bool ok) {
  if (g_drain_slot_inflight == 0 || job_id != g_drain_job_id) return;
  if (ok) {
    StaticJsonDocument<128> d;
    d["ver"] = PROTOCOL_VERSION;
    d["type"] = "SPOOL_DELETE";
    d["msg_id"] = get_next_msg_id();
    d["ts"] = millis();
    d["slot"] = g_drain_slot_inflight;
    String out; serializeJson(d, out);
    uart_send_json(out.c_str());
    g_spool_drained_ok++;
    Serial.printf("[SPOOL_DRAIN] slot=%lu uploaded - asked LCD to delete (total %lu)\n",
                  (unsigned long)g_drain_slot_inflight, (unsigned long)g_spool_drained_ok);
  } else {
    // Keep the slot. The photo is still only on the SD card.
    Serial.printf("[SPOOL_DRAIN] slot=%lu upload failed - keeping it on SD\n",
                  (unsigned long)g_drain_slot_inflight);
    g_spool_drain_fails++;
  }
  g_drain_slot_inflight = 0;
  g_drain_job_id = 0;
}

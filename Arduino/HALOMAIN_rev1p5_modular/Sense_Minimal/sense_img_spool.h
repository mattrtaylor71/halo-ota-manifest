// sense_img_spool.h — stream a capture image to the LCD's SD card over UART.
//
// WHY: the Sense's SPIFFS spool is 173,441 B and captures are 177-189 KB, so
// upload_persist_save_job() always hits `skip_save` and the photo is lost when
// the device sleeps (PSRAM does not survive deep sleep). Measured on-device:
//   [UPLOAD_PERSIST] skip_save len=189246 avail=173441 required=254858
//   [UPLOAD_PERSIST] failure reason=put_fail job_id=3 saved=0
// The LCD has a 480MB SD card. This is the durable path.
//
// Transport is UartOtaProtocol (COBS + CRC16) — the same framing that already
// moves 2MB firmware images over this link — with DISTINCT message types so an
// image can never be confused for firmware.
//
// Cost: ~11 kB/s over the 115200 link, so ~16s for a 180KB image. That is only
// ever paid on the sleep/failure path, when the user has already walked away
// and been told "Logged!". The baud is deliberately NOT raised: this link also
// carries the OTA firmware transfer, and a version mismatch mid-update would
// leave a new-baud Sense unable to talk to an old-baud LCD, which would make
// the LCD permanently un-updatable.

#pragma once

#include <Arduino.h>
#include "../halo_ota_demo/firmware/shared/UartOtaProtocol.h"

#ifndef SENSE_IMG_SPOOL_ACK_TIMEOUT_MS
#define SENSE_IMG_SPOOL_ACK_TIMEOUT_MS 4000
#endif
#ifndef SENSE_IMG_SPOOL_READY_TIMEOUT_MS
#define SENSE_IMG_SPOOL_READY_TIMEOUT_MS 3000
#endif

static uint32_t g_img_spool_sent = 0;
static uint32_t g_img_spool_failed = 0;

// READY is consumed by the common line collector, never by a private reader
// which could swallow INPUT_MENU_SELECT. All fields here are protected by the
// raw RX lease except the published result read by the waiting worker.
static uint32_t s_img_ready_job = 0;
static bool sense_img_spool_begin_matches(const char* line) {
  if (!line || strlen(line) > 1024 || !g_img_spool_request_active.load() ||
      s_img_ready_result.load() != -1) return false;
  StaticJsonDocument<768> doc;
  return deserializeJson(doc, line) == DeserializationError::Ok &&
         !strcmp(doc["type"] | "", "IMG_XFER_BEGIN") &&
         doc["ver"].is<unsigned>() && doc["ver"].as<unsigned>() == PROTOCOL_VERSION &&
         doc["job_id"].is<uint32_t>() && doc["job_id"].as<uint32_t>() == s_img_ready_job;
}
static bool sense_img_spool_on_json(const char* line) {
  if (!line || !strstr(line, "IMG_XFER_READY")) return false;
  StaticJsonDocument<192> doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok ||
      strcmp(doc["type"] | "", "IMG_XFER_READY")) return false;
  // This legacy control reply has no msg_id/ts. It never validates an INPUT.
  if (!g_img_spool_request_active.load() || s_img_ready_result.load() != -1 ||
      !doc["ver"].is<unsigned>() || doc["ver"].as<unsigned>() != PROTOCOL_VERSION ||
      !doc["job_id"].is<uint32_t>() || doc["job_id"].as<uint32_t>() != s_img_ready_job ||
      !doc["ok"].is<unsigned>() || doc["ok"].as<unsigned>() > 1) return true;
  const bool accepted = doc["ok"].as<unsigned>() == 1;
  int pending = -1;
  // Cancellation can release this request while a collector owns RX. A late
  // reply must never resurrect binary ownership after that release.
  s_img_ready_result.compare_exchange_strong(pending, accepted ? 1 : 0);
  return true;
}

class SenseImgSpoolLease {
 public:
  explicit SenseImgSpoolLease(uint32_t job) : held_(false) {
    UartJsonTxLock tx;
    if (!tx.held() || !sense_uart_ordinary_tx_allowed() || sense_lcd_query_busy()) return;
    UartRxLock rx(1000);
    if (!rx.held() || !sense_uart_ordinary_tx_allowed() || uart_dispatch_depth.load()) return;
    s_img_ready_job = job;
    s_img_ready_result.store(-1);
    g_img_spool_request_active.store(true);
    held_ = true;
  }
  ~SenseImgSpoolLease() {
    if (!held_) return;
    // No destructor wait: collection has a finite byte bound, but its task
    // can be preempted. Atomic cancellation closes admission even in that case.
    s_img_ready_result.store(-2);
    g_img_spool_tx_active = false;
    g_img_spool_request_active.store(false);
  }
  bool held() const { return held_; }
 private: bool held_;
};

// Send one image to the LCD. Returns true only when the LCD confirms the whole
// file landed on SD — the caller must NOT drop its copy on false.
//
// Blocking by design: it runs on the sleep path, where the alternative is
// losing the photo. Caller should have already told the user "Logged!".
static bool sense_spool_image_to_lcd_once(const UploadJob& job,
                                          const uint8_t* buf,
                                          size_t len) {
  if (!buf || len == 0 || !sense_uart_ordinary_tx_allowed()) return false;
  const uint32_t job_id = job.job_id;
  SenseImgSpoolLease lease(job_id);
  if (!lease.held()) return false;
  const char* mode = job.mode;

  // 1) Ask the LCD to open the file. It replies IMG_XFER_READY and switches to
  //    binary mode; until then the link is still line-based JSON.
  //
  // The handshake carries the FULL replay metadata, not just the length. The
  // drain has to reconstruct an UploadJob faithfully, and mode/expiry/quantity
  // live only in RAM that deep sleep destroys — spooling the pixels without
  // them would leave the SD card holding images that cannot be uploaded
  // correctly (a check-in would replay as the wrong type). The LCD persists
  // these as <job_id>.json beside the .jpg.
  {
    StaticJsonDocument<512> d;
    d["ver"] = PROTOCOL_VERSION;
    d["type"] = "IMG_XFER_BEGIN";
    d["is_voice"]  = job.is_voice ? 1 : 0;
    d["expiry"]    = job.expiry_date;
    d["qty"]       = job.quantity;
    d["add_list"]  = job.add_to_shopping_list ? 1 : 0;
    d["retries"]   = job.retries;
    d["epoch"]     = job.created_epoch;
    // Camera metadata travels with the image so the backend sees the same
    // capture context it would have on the direct path.
    JsonObject cm = d.createNestedObject("cam");
    cm["p"]  = job.camera_meta.profile;
    cm["f"]  = job.camera_meta.flash_enabled;
    cm["q"]  = job.camera_meta.jpeg_quality;
    cm["w"]  = job.camera_meta.actual_width;
    cm["h"]  = job.camera_meta.actual_height;
    cm["fs"] = job.camera_meta.configured_framesize;
    cm["l"]  = job.camera_meta.scene_luma;
    cm["g"]  = job.camera_meta.scene_green_ratio;
    cm["x"]  = job.camera_meta.xclk_hz;
    // msg_id and ts are MANDATORY: validate_protocol_message() (lcd_uart.h:112)
    // requires ver/type/msg_id/ts and drops anything else SILENTLY — no log line
    // at all. Omitting them made this message vanish with no diagnostic on
    // either board, which cost a full debug cycle to find.
    d["msg_id"] = (uint32_t)millis();
    d["ts"] = (uint32_t)millis();
    d["job_id"] = job_id;
    d["len"] = (uint32_t)len;
    d["mode"] = mode ? mode : "";
    String out; serializeJson(d, out);
    // Deliberately NOT println(): that emits "\r\n", and the LCD's line parser
    // terminates on the '\r' then stops reading the moment this message puts it
    // into binary mode — leaving the '\n' in its FIFO to be eaten as the first
    // byte of the first COBS frame. A single stray byte shifts the whole decode
    // (host-reproduced: decoded_len=520, data_len=59649). The LCD also drains
    // defensively; this removes the residue at the source so neither side has
    // to be right for the transfer to work.
    uart_send_json(out.c_str(), false, true);
  }

  // Collection does not dispatch app callbacks while this caller owns a
  // photo. A separate main/sleep pump can dispatch ordinary queued input when
  // this function runs on the upload worker. Main-owned rescue waits until its
  // next safe custody boundary before dispatching those same preserved bytes.
  const uint32_t ready_started = millis();
  while (s_img_ready_result.load() == -1 &&
         uint32_t(millis() - ready_started) < SENSE_IMG_SPOOL_READY_TIMEOUT_MS) {
    uart_collect_rx_once();
    delay(5);
  }
  const bool ready = uint32_t(millis() - ready_started) < SENSE_IMG_SPOOL_READY_TIMEOUT_MS &&
                     s_img_ready_result.load() == 1;
  if (!ready) {
    Serial.printf("[IMG_SPOOL] job=%lu LCD not ready — keeping image in RAM\n",
                  (unsigned long)job_id);
    g_img_spool_failed++;
    return false;
  }

  // READY switched the common collector off before any binary byte. Preserve
  // already queued ordinary lines; never flush them as handshake residue.
  UartRxLock binary_rx(1000);
  if (!binary_rx.held()) return false;
  delay(120);

  // 2) Stream the payload as COBS frames.
  g_img_spool_tx_active = true;   // hold off our own JSON for the whole transfer
  UartOtaProtocol proto(&lcdSerial);
  proto.quiet = true;   // per-frame logs would dominate the transfer time
  uint32_t t0 = millis();
  uint16_t seq = 0;
  size_t off = 0;
  bool ok = true;

  while (off < len) {
    size_t n = (len - off < MAX_CHUNK_SIZE) ? (len - off) : MAX_CHUNK_SIZE;
    if (!proto.send_frame(MSG_IMG_CHUNK, seq, buf + off, n)) { ok = false; break; }

    // Wait for this chunk's ACK before sending the next. Lock-step is slower
    // than windowing but it means a dropped frame is caught immediately rather
    // than silently corrupting the file — and the LCD is writing to SD between
    // frames anyway, so it could not keep up with a window.
    static uint8_t rframe[MAX_FRAME_SIZE];
    uint8_t rtype = 0; uint16_t rseq = 0;
    size_t rlen = sizeof(rframe);   // IN/OUT: must carry capacity IN
    if (!proto.recv_frame(&rtype, &rseq, rframe, &rlen, SENSE_IMG_SPOOL_ACK_TIMEOUT_MS)) {
      Serial.printf("[IMG_SPOOL] job=%lu ack timeout at seq=%u\n",
                    (unsigned long)job_id, (unsigned)seq);
      ok = false; break;
    }
    if (rtype != MSG_IMG_ACK || rseq != seq) {
      Serial.printf("[IMG_SPOOL] job=%lu bad ack type=0x%02X seq=%u (want %u)\n",
                    (unsigned long)job_id, (unsigned)rtype, (unsigned)rseq, (unsigned)seq);
      ok = false; break;
    }
    off += n;
    seq++;

    // Progress every ~32KB so a stall is visible in the log without spamming.
    if ((seq % 64) == 0) {
      Serial.printf("[IMG_SPOOL] job=%lu %u/%u bytes\n",
                    (unsigned long)job_id, (unsigned)off, (unsigned)len);
    }
  }

  // 3) Finish. The LCD only promotes .part -> .jpg when its byte count matches,
  //    so a truncated transfer cannot be mistaken for a complete image.
  if (ok) {
    ok = proto.send_frame(MSG_IMG_END, seq, nullptr, 0);
    static uint8_t rframe[MAX_FRAME_SIZE];
    uint8_t rtype = 0; uint16_t rseq = 0;
    size_t rlen = sizeof(rframe);   // IN/OUT: must carry capacity IN
    ok = ok && proto.recv_frame(&rtype, &rseq, rframe, &rlen, SENSE_IMG_SPOOL_ACK_TIMEOUT_MS)
         && rtype == MSG_IMG_ACK && rseq == seq;
  }

  binary_rx.release();
  uint32_t ms = millis() - t0;
  Serial.printf("[IMG_SPOOL] job=%lu result=%s bytes=%u ms=%lu rate_Bps=%lu\n",
                (unsigned long)job_id, ok ? "OK" : "FAIL", (unsigned)off,
                (unsigned long)ms, ms ? (unsigned long)(off * 1000UL / ms) : 0UL);
  if (ok) g_img_spool_sent++; else g_img_spool_failed++;
  return ok;
}

// Retrying wrapper. THIS is what callers use.
//
// A failed spool is not a failed retry -- it is a DESTROYED CAPTURE. Spooling
// only ever runs after the upload has already failed, PSRAM does not survive the
// deep sleep that follows, and the SPIFFS partition (173,441 B) cannot hold a
// 150-190 KB image. So this call is the last thing standing between a user's
// photo and nothing at all, and it was a single shot with a 3s timeout:
//   [UPLOAD_PERSIST] SD spool FAILED for job_id=7
//   {"event":"spool_failed","label":"dish","detail":"PHOTO_LOST"}
//
// Two reasons it missed, both addressed here:
//
// 1. The LCD was on its way to sleep. Spooling happens at pre-sleep, which is
//    exactly when the LCD is winding down, and its IMG_XFER_BEGIN handler does
//    not reset the activity timer. MAINT_KEEPALIVE does -- it holds the LCD up
//    and explicitly aborts a sleep transition -- and it is already deployed on
//    every LCD, so this works without an LCD reflash.
// 2. One transient miss was fatal. Now it is not.
#ifndef SENSE_IMG_SPOOL_ATTEMPTS
#define SENSE_IMG_SPOOL_ATTEMPTS 3
#endif

static void sense_spool_hold_lcd_awake() {
  if (!sense_uart_ordinary_tx_allowed()) return;
  StaticJsonDocument<128> k;
  k["ver"] = PROTOCOL_VERSION;
  k["type"] = "MAINT_KEEPALIVE";
  k["msg_id"] = (uint32_t)millis();
  k["ts"] = (uint32_t)millis();
  k["request_id"] = "img_spool";
  String out; serializeJson(k, out);
  uart_send_json(out.c_str());
  delay(60);   // let the LCD act on it before the transfer handshake
}

static bool sense_spool_image_to_lcd(const UploadJob& job,
                                     const uint8_t* buf,
                                     size_t len) {
  if (!sense_uart_ordinary_tx_allowed()) return false;
  for (int attempt = 1; attempt <= SENSE_IMG_SPOOL_ATTEMPTS; attempt++) {
    sense_spool_hold_lcd_awake();
    if (sense_spool_image_to_lcd_once(job, buf, len)) {
      if (attempt > 1) {
        Serial.printf("[IMG_SPOOL] job=%lu recovered on attempt %d\n",
                      (unsigned long)job.job_id, attempt);
      }
      return true;
    }
    if (attempt < SENSE_IMG_SPOOL_ATTEMPTS) {
      Serial.printf("[IMG_SPOOL] job=%lu attempt %d/%d failed - retrying\n",
                    (unsigned long)job.job_id, attempt, SENSE_IMG_SPOOL_ATTEMPTS);
      delay(400);
    }
  }
  Serial.printf("[IMG_SPOOL] job=%lu FAILED after %d attempts - capture will be lost\n",
                (unsigned long)job.job_id, SENSE_IMG_SPOOL_ATTEMPTS);
  return false;
}

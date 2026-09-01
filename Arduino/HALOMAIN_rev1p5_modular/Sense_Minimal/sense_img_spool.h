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

// Send one image to the LCD. Returns true only when the LCD confirms the whole
// file landed on SD — the caller must NOT drop its copy on false.
//
// Blocking by design: it runs on the sleep path, where the alternative is
// losing the photo. Caller should have already told the user "Logged!".
static bool sense_spool_image_to_lcd(const UploadJob& job,
                                     const uint8_t* buf,
                                     size_t len) {
  if (!buf || len == 0) return false;
  const uint32_t job_id = job.job_id;
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
    lcdSerial.print(out); lcdSerial.print('\n'); lcdSerial.flush();
  }

  // Wait for IMG_XFER_READY. Parsed loosely on purpose: we only need to know
  // the LCD opened the file, and a strict parse here would add a failure mode
  // on the path whose whole job is not to lose data.
  bool ready = false;
  {
    unsigned long deadline = millis() + SENSE_IMG_SPOOL_READY_TIMEOUT_MS;
    String line;
    while ((long)(millis() - deadline) < 0) {
      while (lcdSerial.available()) {
        char c = (char)lcdSerial.read();
        if (c == '\n' || c == '\r') {
          if (line.indexOf("IMG_XFER_READY") >= 0) {
            ready = (line.indexOf("\"ok\":1") >= 0);
            deadline = 0;   // break outer
            break;
          }
          line = "";
        } else if (line.length() < 220) {
          line += c;
        }
      }
      if (deadline == 0) break;
      delay(5);
    }
  }
  if (!ready) {
    Serial.printf("[IMG_SPOOL] job=%lu LCD not ready — keeping image in RAM\n",
                  (unsigned long)job_id);
    g_img_spool_failed++;
    return false;
  }

  // Let the LCD actually enter binary mode before the first frame goes out.
  // It sets g_img_rx_binary_mode inside its RX handler, but its uart_task may
  // still be part-way through a line-mode iteration; bytes arriving in that
  // window get eaten by the line parser and the transfer dies at seq=0 with a
  // frame timeout on both sides (observed exactly that). Also drain anything
  // left over from the JSON handshake so it cannot be mistaken for COBS data.
  delay(120);
  while (lcdSerial.available()) lcdSerial.read();

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
    proto.send_frame(MSG_IMG_END, seq, nullptr, 0);
    static uint8_t rframe[MAX_FRAME_SIZE];
    uint8_t rtype = 0; uint16_t rseq = 0;
    size_t rlen = sizeof(rframe);   // IN/OUT: must carry capacity IN
    ok = proto.recv_frame(&rtype, &rseq, rframe, &rlen, SENSE_IMG_SPOOL_ACK_TIMEOUT_MS)
         && rtype == MSG_IMG_ACK;
  }

  g_img_spool_tx_active = false;
  uint32_t ms = millis() - t0;
  Serial.printf("[IMG_SPOOL] job=%lu result=%s bytes=%u ms=%lu rate_Bps=%lu\n",
                (unsigned long)job_id, ok ? "OK" : "FAIL", (unsigned)off,
                (unsigned long)ms, ms ? (unsigned long)(off * 1000UL / ms) : 0UL);
  if (ok) g_img_spool_sent++; else g_img_spool_failed++;
  return ok;
}

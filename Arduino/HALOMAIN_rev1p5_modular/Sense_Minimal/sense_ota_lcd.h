/*
 * sense_ota_lcd.h
 *
 * LCD OTA proxy: Sense board fetches LCD firmware manifest and binary
 * from S3 over HTTPS, then streams the binary to the LCD board via
 * UART using COBS-framed UartOtaProtocol.
 *
 * The LCD board cannot perform TLS directly due to insufficient DMA RAM,
 * so the Sense board acts as a download-and-forward proxy.
 *
 * Prerequisites (must be declared before #include "sense_ota_lcd.h"):
 *   - ArduinoJson.h, WiFiClientSecure.h, HTTPClient.h
 *   - lcdSerial (HardwareSerial)
 *   - uart_send_json(), get_next_msg_id(), PROTOCOL_VERSION
 *   - ManifestClient.h (OtaManifest, ManifestClient::compareVersions)
 *   - UartOtaProtocol.h
 */

#pragma once

#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "../halo_ota_demo/firmware/shared/UartOtaProtocol.h"
#include "../halo_ota_demo/firmware/shared/ManifestClient.h"

// ── Constants ────────────────────────────────────────────────────────

#define LCD_OTA_PROXY_CHUNK_SIZE        512
#define LCD_OTA_PROXY_MAX_RETRIES       5
#define LCD_OTA_PROXY_ACK_TIMEOUT_MS    3000
#define LCD_OTA_PROXY_QUERY_TIMEOUT_MS  5000
#define LCD_OTA_PROXY_BEGIN_TIMEOUT_MS  10000
#define LCD_OTA_PROXY_END_TIMEOUT_MS    60000
#define LCD_OTA_PROXY_DOWNLOAD_TIMEOUT_MS 2400000  // 40 min
#define LCD_OTA_PROXY_STALL_WINDOW_MS   30000      // no-data window before stall
#define LCD_OTA_PROXY_MAX_RECONNECTS    5          // consecutive stall reconnect budget

// ── TEST-ONLY debug hook ───────────────────────────────────────────
// When 1, injects a SINGLE forced mid-transfer stall (closes the HTTP
// connection once, at ~50% of the image) to exercise and verify the
// reconnect/resume-on-stall path. This MUST be 0 in shipping builds.
// It has no effect on real OTA behavior when set to 0.
#define LCD_OTA_PROXY_TEST_FORCE_STALL 0   // TEST-ONLY: set to 1 to inject one mid-transfer stall (verified reconnect/resume 2026-06-01).

// ── Forward declarations of externs from Sense_Minimal.ino ──────────

extern HardwareSerial lcdSerial;

// ── Response mailbox for LCD OTA proxy task ─────────────────────────
// Set by Sense UART RX dispatch (parse_input_message), read by proxy task.
// This avoids the proxy task reading lcdSerial directly, which would
// race with the main loop's UART RX processing.

static volatile bool g_lcd_ota_query_resp_ready = false;
static char          g_lcd_ota_query_resp_fw[32] = {0};
static uint32_t      g_lcd_ota_query_resp_part_size = 0;

// Extended LCD partition/state fields captured from LCD_OTA_QUERY_RESP.
// Filled by parse_input_message dispatch (Sense_Minimal.ino) alongside
// g_lcd_ota_query_resp_fw. Observability only — does not affect OTA control.
static char          g_lcd_query_running_part[16]  = {0};  // LCD running partition label
static char          g_lcd_query_running_state[20] = {0};  // NEW|PENDING_VERIFY|VALID|INVALID|ABORTED|UNDEFINED|UNKNOWN
static char          g_lcd_query_boot_part[16]     = {0};  // LCD boot partition label

// Getters exposing the last captured LCD running-state observability fields.
// Persist across queries; reflect the most recent successful QUERY_RESP.
static inline const char* sense_lcd_last_running_part() {
  return g_lcd_query_running_part;
}
static inline const char* sense_lcd_last_running_state() {
  return g_lcd_query_running_state;
}
static inline const char* sense_lcd_last_boot_part() {
  return g_lcd_query_boot_part;
}

static volatile bool g_lcd_ota_begin_ack_ready = false;
static bool          g_lcd_ota_begin_ack_accepted = false;
static char          g_lcd_ota_begin_ack_reason[32] = {0};
static uint32_t      g_lcd_ota_begin_ack_resume_offset = 0;

static volatile bool g_lcd_ota_end_ack_ready = false;
static bool          g_lcd_ota_end_ack_sha_match = false;

// When true, the proxy task owns lcdSerial for binary COBS framing.
// The main loop must skip reading lcdSerial while this is set.
// g_lcd_ota_proxy_owns_uart is declared in sense_uart.h (included earlier)

// ── Helpers ──────────────────────────────────────────────────────────

/*
 * send_lcd_ota_abort
 *
 * Notify the LCD that the OTA session is being aborted.
 */
static void send_lcd_ota_abort(uint16_t session_id, const char* reason) {
  StaticJsonDocument<256> doc;
  doc["ver"]        = PROTOCOL_VERSION;
  doc["type"]       = "LCD_OTA_ABORT";
  doc["msg_id"]     = get_next_msg_id();
  doc["ts"]         = (uint32_t)millis();
  doc["session_id"] = session_id;
  doc["reason"]     = reason;

  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());

  Serial.printf("[LCD_OTA_PROXY] ABORT session=%u reason=%s\n",
                session_id, reason);
}

// ── Public API ───────────────────────────────────────────────────────

/*
 * sense_lcd_ota_query
 *
 * Query the LCD board for its current firmware version and OTA partition
 * size.  Sends LCD_OTA_QUERY over UART JSON and waits for
 * LCD_OTA_QUERY_RESP.
 *
 * Returns true on success, populating lcd_fw_out and part_size_out.
 */
static bool sense_lcd_ota_query(char* lcd_fw_out, size_t fw_len,
                                uint32_t* part_size_out) {
  if (!lcd_fw_out || fw_len == 0) return false;
  lcd_fw_out[0] = '\0';
  if (part_size_out) *part_size_out = 0;

  // Clear mailbox before sending
  g_lcd_ota_query_resp_ready = false;

  // Send query
  StaticJsonDocument<128> doc;
  doc["ver"]    = PROTOCOL_VERSION;
  doc["type"]   = "LCD_OTA_QUERY";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"]     = (uint32_t)millis();

  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.println("[LCD_OTA_PROXY] TX LCD_OTA_QUERY");

  // Wait for mailbox to be filled by parse_input_message dispatch
  unsigned long start = millis();
  while (!g_lcd_ota_query_resp_ready &&
         (millis() - start) < LCD_OTA_PROXY_QUERY_TIMEOUT_MS) {
    delay(10);
  }

  if (!g_lcd_ota_query_resp_ready) {
    Serial.println("[LCD_OTA_PROXY] LCD_OTA_QUERY_RESP timeout");
    return false;
  }

  if (g_lcd_ota_query_resp_fw[0] == '\0') {
    Serial.println("[LCD_OTA_PROXY] LCD_OTA_QUERY_RESP missing lcd_fw");
    return false;
  }

  strncpy(lcd_fw_out, g_lcd_ota_query_resp_fw, fw_len - 1);
  lcd_fw_out[fw_len - 1] = '\0';

  if (part_size_out) {
    *part_size_out = g_lcd_ota_query_resp_part_size;
  }

  Serial.printf("[LCD_OTA_PROXY] LCD reports fw=%s part_size=%lu\n",
                lcd_fw_out,
                part_size_out ? (unsigned long)*part_size_out : 0UL);
  return true;
}

/*
 * sense_lcd_ota_fetch_manifest
 *
 * Build the LCD manifest URL from base_dir and channel, then fetch and
 * parse the manifest JSON from S3.
 *
 * URL pattern: <base_dir>/<channel>/lcd/manifest_latest.json
 *
 * Returns true if the manifest was fetched, parsed, and (if the board
 * field is present) validated as targeting "lcd".
 */
static bool sense_lcd_ota_fetch_manifest(const char* base_dir,
                                         const char* channel,
                                         OtaManifest& manifest) {
  if (!base_dir || !channel) {
    Serial.println("[LCD_OTA_PROXY] fetch_manifest: null base_dir/channel");
    return false;
  }

  // Extern reference to the global ManifestClient
  extern ManifestClient g_manifest_client;

  char url[512];
  snprintf(url, sizeof(url), "%s/%s/lcd/manifest_latest.json",
           base_dir, channel);
  url[sizeof(url) - 1] = '\0';

  Serial.printf("[LCD_OTA_PROXY] fetching LCD manifest: %s\n", url);

  if (!g_manifest_client.fetchManifest(url, manifest, 10000)) {
    Serial.println("[LCD_OTA_PROXY] manifest fetch failed");
    return false;
  }

  if (!manifest.valid) {
    Serial.println("[LCD_OTA_PROXY] manifest invalid after parse");
    return false;
  }

  // Validate board field if present
  if (manifest.board_specified && manifest.board[0] != '\0') {
    if (strcasecmp(manifest.board, "lcd") != 0) {
      Serial.printf("[LCD_OTA_PROXY] manifest board mismatch: '%s'\n",
                    manifest.board);
      return false;
    }
  }

  Serial.printf("[LCD_OTA_PROXY] manifest ok version=%s size=%lu\n",
                manifest.version, (unsigned long)manifest.size);
  return true;
}

/*
 * sense_lcd_ota_proxy
 *
 * Main LCD OTA proxy routine.  Compares the manifest version against the
 * LCD's current version, negotiates an OTA session over UART JSON, then
 * downloads the firmware binary from S3 and streams it to the LCD board
 * as COBS-framed chunks with per-chunk ACK/retry.
 *
 * Returns a result string:
 *   "success"               - OTA completed, SHA verified by LCD
 *   "up_to_date"            - LCD already at or ahead of manifest version
 *   "manifest_fetch_fail"   - (caller should catch before calling)
 *   "lcd_query_fail"        - could not query LCD version
 *   "begin_rejected:<reason>"- LCD refused the OTA session
 *   "chunk_retry_exhausted" - a chunk failed after max retries
 *   "sha_mismatch"          - LCD reported SHA mismatch after END
 *   "download_fail"         - HTTPS download setup failed
 *   "timeout"               - various timeout failures
 */
static const char* sense_lcd_ota_proxy(const OtaManifest& manifest,
                                       const char* lcd_fw_version) {
  // ── a. Version check ──────────────────────────────────────────────
  if (ManifestClient::compareVersions(manifest.version, lcd_fw_version) <= 0) {
    Serial.printf("[LCD_OTA_PROXY] up_to_date manifest=%s lcd=%s\n",
                  manifest.version, lcd_fw_version);
    return "up_to_date";
  }

  Serial.printf("[LCD_OTA_PROXY] update available: %s -> %s  size=%lu\n",
                lcd_fw_version, manifest.version,
                (unsigned long)manifest.size);

  // ── b. Generate session ID and send LCD_OTA_BEGIN ─────────────────
  uint16_t session_id = (uint16_t)((esp_random() & 0xFFFE) + 1);  // 1..65535

  {
    StaticJsonDocument<384> doc;
    doc["ver"]        = PROTOCOL_VERSION;
    doc["type"]       = "LCD_OTA_BEGIN";
    doc["msg_id"]     = get_next_msg_id();
    doc["ts"]         = (uint32_t)millis();
    doc["session_id"] = session_id;
    doc["image_size"] = manifest.size;
    doc["sha256"]     = manifest.sha256;
    doc["version"]    = manifest.version;
    if (manifest.build_id_specified && manifest.build_id[0]) {
      doc["build_id"] = manifest.build_id;
    }

    String output;
    serializeJson(doc, output);
    uart_send_json(output.c_str());
    Serial.printf("[LCD_OTA_PROXY] TX LCD_OTA_BEGIN session=%u size=%lu\n",
                  session_id, (unsigned long)manifest.size);
  }

  // ── c. Wait for LCD_OTA_BEGIN_ACK (via mailbox) ───────────────────
  g_lcd_ota_begin_ack_ready = false;
  uint32_t resume_offset = 0;
  {
    unsigned long start = millis();
    while (!g_lcd_ota_begin_ack_ready &&
           (millis() - start) < LCD_OTA_PROXY_BEGIN_TIMEOUT_MS) {
      delay(10);
    }

    if (!g_lcd_ota_begin_ack_ready) {
      Serial.println("[LCD_OTA_PROXY] LCD_OTA_BEGIN_ACK timeout");
      send_lcd_ota_abort(session_id, "begin_ack_timeout");
      return "timeout";
    }

    if (!g_lcd_ota_begin_ack_accepted) {
      Serial.printf("[LCD_OTA_PROXY] BEGIN rejected: %s\n",
                    g_lcd_ota_begin_ack_reason);
      static char reject_buf[64];
      snprintf(reject_buf, sizeof(reject_buf), "begin_rejected:%s",
               g_lcd_ota_begin_ack_reason);
      reject_buf[sizeof(reject_buf) - 1] = '\0';
      return reject_buf;
    }

    resume_offset = g_lcd_ota_begin_ack_resume_offset;
    Serial.printf("[LCD_OTA_PROXY] BEGIN accepted resume_offset=%lu\n",
                  (unsigned long)resume_offset);
  }

  // ── d. Download firmware binary from S3 ───────────────────────────
  WiFiClientSecure tls_client;
  tls_client.setInsecure();  // Integrity verified via SHA256 post-transfer
  tls_client.setTimeout(30);  // 30 s socket timeout

  HTTPClient http;
  const char* download_result = nullptr;
  WiFiClient* stream = nullptr;

  // Open (or re-open) the HTTP GET at byte offset `from`. On resume/reconnect
  // a Range header is sent. Acquires `stream` on success. Returns the HTTP
  // status code (>0) on success, or a negative value on begin/stream failure.
  // Shared by the initial fetch and the stall-reconnect path so they use
  // identical request setup.
  auto open_stream = [&](uint32_t from) -> int {
    http.end();  // harmless if not yet begun; closes a stalled socket
    stream = nullptr;
    if (!http.begin(tls_client, manifest.url)) {
      Serial.printf("[LCD_OTA_PROXY] HTTP begin failed: %s\n", manifest.url);
      return -1;
    }
    if (from > 0) {
      char range_hdr[48];
      snprintf(range_hdr, sizeof(range_hdr), "bytes=%lu-",
               (unsigned long)from);
      http.addHeader("Range", range_hdr);
      Serial.printf("[LCD_OTA_PROXY] requesting Range: %s\n", range_hdr);
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK && code != 206) {
      return code <= 0 ? code : -code;  // keep negative to signal failure
    }
    stream = http.getStreamPtr();
    if (!stream) {
      Serial.println("[LCD_OTA_PROXY] null stream pointer");
      return -2;
    }
    // Reduce Stream timeout to avoid blocking on partial TCP segments.
    // Default is 1000ms — at 2134 chunks, even 50% partial-reads would add
    // >17 minutes of dead wait time.
    stream->setTimeout(50);
    return code;
  };

  Serial.printf("[LCD_OTA_PROXY] DL heap: largest=%u free=%u\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)esp_get_free_heap_size());
  Serial.printf("[LCD_OTA_PROXY] starting HTTP GET: %s\n", manifest.url);
  int http_code = open_stream(resume_offset);
  Serial.printf("[LCD_OTA_PROXY] HTTP GET result code=%d\n", http_code);
  if (http_code <= 0 || !stream) {
    Serial.printf("[LCD_OTA_PROXY] HTTP GET failed code=%d\n", http_code);
    diag_record_error_persistent("lcd_ota", http_code, "download_fail");
    http.end();
    send_lcd_ota_abort(session_id, "download_fail");
    return "download_fail";
  }

  // ── e. Create UartOtaProtocol ─────────────────────────────────────
  UartOtaProtocol* protocol = new UartOtaProtocol(&lcdSerial);
  if (protocol) protocol->quiet = true;  // suppress per-frame logs for throughput
  if (!protocol) {
    Serial.println("[LCD_OTA_PROXY] failed to allocate UartOtaProtocol");
    http.end();
    send_lcd_ota_abort(session_id, "out_of_memory");
    return "download_fail";
  }

  // ── f. Streaming loop ─────────────────────────────────────────────
  // Take exclusive ownership of lcdSerial for binary COBS framing.
  // The main loop must skip reading lcdSerial while this flag is set.
  g_lcd_ota_proxy_owns_uart = true;

  // open_stream() already set the per-stream read timeout (50ms).

  uint8_t chunk_buf[LCD_OTA_PROXY_CHUNK_SIZE];
  uint32_t bytes_sent = resume_offset;
  uint16_t seq = 1;
  uint8_t last_pct_10 = 0xFF;  // Force first progress log
  unsigned long stream_start_ms = millis();
  unsigned long last_data_ms = millis();
  int reconnects = 0;  // consecutive stalls; reset on any data progress
#if LCD_OTA_PROXY_TEST_FORCE_STALL
  bool forced_stall_done = false;  // TEST-ONLY: one-shot stall injection guard
#endif

  // Attempt to recover from a stall by re-opening the HTTP GET at the current
  // offset with a Range header. Returns true if a fresh connected stream was
  // acquired and the loop may continue; false if the reconnect budget is
  // exhausted or the reconnect failed (caller aborts with "timeout").
  auto try_reconnect = [&]() -> bool {
    if (reconnects >= LCD_OTA_PROXY_MAX_RECONNECTS) {
      Serial.printf("[LCD_OTA_PROXY] reconnect budget exhausted (%d/%d) at offset %lu\n",
                    reconnects, LCD_OTA_PROXY_MAX_RECONNECTS,
                    (unsigned long)bytes_sent);
      return false;
    }
    reconnects++;
    Serial.printf("[LCD_OTA_PROXY] stall -> reconnect attempt %d/%d at offset %lu\n",
                  reconnects, LCD_OTA_PROXY_MAX_RECONNECTS,
                  (unsigned long)bytes_sent);
    int code = open_stream(bytes_sent);
    if (code <= 0 || !stream) {
      Serial.printf("[LCD_OTA_PROXY] reconnect failed code=%d\n", code);
      return false;
    }
    Serial.printf("[LCD_OTA_PROXY] reconnect ok code=%d\n", code);
    last_data_ms = millis();  // grant a fresh stall window after reconnect
    return true;
  };

  while (bytes_sent < manifest.size) {
    // Stall / overall timeout detection
    if ((millis() - stream_start_ms) > LCD_OTA_PROXY_DOWNLOAD_TIMEOUT_MS) {
      Serial.println("[LCD_OTA_PROXY] overall download timeout");
      download_result = "timeout";
      break;
    }

    size_t remaining = manifest.size - bytes_sent;
    size_t to_read = (remaining < LCD_OTA_PROXY_CHUNK_SIZE)
                         ? remaining
                         : LCD_OTA_PROXY_CHUNK_SIZE;

    // If the stream dropped before completion, treat as a stall and try to
    // resume from the current offset rather than aborting the whole OTA.
    if (!stream->available() && !stream->connected()) {
      Serial.println("[LCD_OTA_PROXY] stream disconnected before completion");
      if (!try_reconnect()) {
        download_result = "timeout";
        break;
      }
      continue;
    }

    // Wait for data with stall detection
    unsigned long read_start = millis();
    while (!stream->available() && stream->connected() &&
           (millis() - read_start) < LCD_OTA_PROXY_STALL_WINDOW_MS) {
      delay(10);
    }
    // Read only what's available to avoid blocking on partial TCP segments
    size_t avail = stream->available();
    if (avail > 0 && avail < to_read) {
      to_read = avail;
    }

    unsigned long t_read_start = millis();
    size_t got = (avail > 0) ? stream->readBytes(chunk_buf, to_read) : 0;
    unsigned long t_read_ms = millis() - t_read_start;
    if (seq == 1 && got > 0) {
      Serial.printf("[LCD_OTA_PROXY] first chunk read: got=%u to_read=%u read_ms=%lu\n",
                    (unsigned)got, (unsigned)to_read, t_read_ms);
    }
    if (got == 0) {
      // No data for the stall window — attempt a reconnect/resume before
      // giving up. The reconnect budget bounds total retries.
      if ((millis() - last_data_ms) > LCD_OTA_PROXY_STALL_WINDOW_MS) {
        Serial.printf("[LCD_OTA_PROXY] stream stall (%lu s no data)\n",
                      (unsigned long)(LCD_OTA_PROXY_STALL_WINDOW_MS / 1000));
        if (!try_reconnect()) {
          download_result = "timeout";
          break;
        }
      }
      continue;
    }
    last_data_ms = millis();
    reconnects = 0;  // any data progress resets the consecutive-stall counter

    // Send chunk with retry
    unsigned long t_send_start = millis();
    bool acked = false;
    for (int retry = 0; retry < LCD_OTA_PROXY_MAX_RETRIES; retry++) {
      protocol->send_frame(MSG_CHUNK, seq, chunk_buf, got);

      uint8_t resp_type;
      uint16_t resp_seq;
      uint8_t resp_data[8];
      size_t resp_len = sizeof(resp_data);

      if (protocol->recv_frame(&resp_type, &resp_seq, resp_data, &resp_len,
                                LCD_OTA_PROXY_ACK_TIMEOUT_MS)) {
        if (resp_type == MSG_ACK && resp_seq == seq) {
          acked = true;
          break;
        }
        if (resp_type == MSG_NACK) {
          uint8_t err_code = (resp_len > 0) ? resp_data[0] : 0;
          Serial.printf("[LCD_OTA_PROXY] NACK seq=%u err=%u retry=%d\n",
                        seq, err_code, retry);
        } else {
          Serial.printf("[LCD_OTA_PROXY] unexpected resp type=%u seq=%u "
                        "expected_seq=%u retry=%d\n",
                        resp_type, resp_seq, seq, retry);
        }
      } else {
        Serial.printf("[LCD_OTA_PROXY] timeout seq=%u retry=%d\n",
                      seq, retry);
      }
    }

    if (!acked) {
      Serial.printf("[LCD_OTA_PROXY] chunk %u retry exhausted\n", seq);
      download_result = "chunk_retry_exhausted";
      break;
    }

    unsigned long t_rtt_ms = millis() - t_send_start;
    bytes_sent += got;
    seq++;

    // Progress logging every 10%
    uint8_t pct = (uint8_t)((uint64_t)bytes_sent * 100 / manifest.size);
    uint8_t pct_bucket = pct / 10;
    if (pct_bucket != last_pct_10) {
      unsigned long elapsed_s = (millis() - stream_start_ms) / 1000;
      unsigned long rate_bps = elapsed_s > 0 ? bytes_sent / elapsed_s : 0;
      Serial.printf("[LCD_OTA_PROXY] progress %u%% (%lu/%lu) elapsed=%lus rate=%luB/s last_read=%lums last_rtt=%lums\n",
                    pct, (unsigned long)bytes_sent,
                    (unsigned long)manifest.size,
                    elapsed_s, rate_bps, t_read_ms, t_rtt_ms);
      last_pct_10 = pct_bucket;
    }

#if LCD_OTA_PROXY_TEST_FORCE_STALL
    if (!forced_stall_done && bytes_sent >= (manifest.size / 2)) {
      forced_stall_done = true;
      Serial.printf("[LCD_OTA_PROXY][TEST] stopping stream at offset %lu (%.0f%%) to exercise reconnect\n",
                    (unsigned long)bytes_sent, 100.0 * bytes_sent / manifest.size);
      stream->stop();   // close the actual TCP socket the loop reads from.
                        // http.end() alone does NOT stop a user-supplied
                        // tls_client, so the stream kept flowing. Stopping the
                        // stream makes next iteration's pre-read
                        // !available() && !connected() check fire -> try_reconnect().
    }
#endif
  }

  // ── Cleanup on stream error ───────────────────────────────────────
  if (download_result != nullptr) {
    diag_record_error_persistent("lcd_ota", -1, download_result);
    g_lcd_ota_proxy_owns_uart = false;
    delete protocol;
    http.end();
    send_lcd_ota_abort(session_id, download_result);
    return download_result;
  }

  // ── g. Send LCD_OTA_END and wait for LCD_OTA_END_ACK ──────────────
  // Release UART ownership — back to JSON mode for END/ACK exchange
  g_lcd_ota_proxy_owns_uart = false;
  delete protocol;  // Done with binary framing
  protocol = nullptr;

  // Close HTTP before sending END (free TLS RAM for UART traffic)
  http.end();

  // Clear mailbox before sending END
  g_lcd_ota_end_ack_ready = false;

  {
    StaticJsonDocument<256> end_doc;
    end_doc["ver"]        = PROTOCOL_VERSION;
    end_doc["type"]       = "LCD_OTA_END";
    end_doc["msg_id"]     = get_next_msg_id();
    end_doc["ts"]         = (uint32_t)millis();
    end_doc["session_id"] = session_id;
    end_doc["image_size"] = manifest.size;
    end_doc["sha256"]     = manifest.sha256;

    String output;
    serializeJson(end_doc, output);
    uart_send_json(output.c_str());
    Serial.printf("[LCD_OTA_PROXY] TX LCD_OTA_END session=%u bytes=%lu\n",
                  session_id, (unsigned long)bytes_sent);
  }

  // Wait for LCD_OTA_END_ACK via mailbox (LCD verifies SHA and reboots)
  {
    unsigned long start = millis();
    while (!g_lcd_ota_end_ack_ready &&
           (millis() - start) < LCD_OTA_PROXY_END_TIMEOUT_MS) {
      delay(10);
    }

    if (!g_lcd_ota_end_ack_ready) {
      Serial.println("[LCD_OTA_PROXY] LCD_OTA_END_ACK timeout");
      diag_record_error_persistent("lcd_ota", -1, "end_ack_timeout");
      return "timeout";
    }

    if (!g_lcd_ota_end_ack_sha_match) {
      Serial.println("[LCD_OTA_PROXY] SHA mismatch reported by LCD");
      diag_record_error_persistent("lcd_ota", -1, "sha_mismatch");
      return "sha_mismatch";
    }

    Serial.printf("[LCD_OTA_PROXY] success version=%s bytes=%lu elapsed=%lu ms\n",
                  manifest.version, (unsigned long)bytes_sent,
                  (unsigned long)(millis() - stream_start_ms));
    return "success";
  }
}

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
#define LCD_OTA_PROXY_QUERY_TIMEOUT_MS  7000   // 7s per attempt: extra margin for the
                                               // scheduled cold-wake case (LCD wakes from
                                               // its own timer and needs LVGL init before
                                               // it can answer LCD_OTA_QUERY)
// The LCD's LCD_OTA_BEGIN handler calls esp_ota_begin(), which SYNCHRONOUSLY
// erases the full ~2.5MB OTA partition BEFORE it can send LCD_OTA_BEGIN_ACK.
// That erase takes ~10s on this flash. The old 10000ms value raced the erase:
// the Sense aborted the LCD half at exactly ~10061ms (confirmed in the on-device
// ota_orch breadcrumbs: lcd_proxy_start -> lcd_proxy_done res=timeout, 10061ms)
// while the LCD was still erasing and about to ACK -> Sense self-updated but LCD
// stayed one version behind. 45s gives >4x margin over the erase. A genuine
// LCD reject (no partition / image too large) still returns accepted=false
// immediately, so this longer window only elapses when the LCD is truly silent.
#define LCD_OTA_PROXY_BEGIN_TIMEOUT_MS  45000
#define LCD_OTA_PROXY_END_TIMEOUT_MS    60000
#define LCD_OTA_PROXY_DOWNLOAD_TIMEOUT_MS 2400000  // 40 min
#define LCD_OTA_PROXY_STALL_WINDOW_MS   30000      // no-data window ends this session

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
// g_lcd_ota_query_resp_fw. Also used to confirm LCD-only completion after reboot.
static char          g_lcd_query_running_part[16]  = {0};  // LCD running partition label
static char          g_lcd_query_running_state[20] = {0};  // NEW|PENDING_VERIFY|VALID|INVALID|ABORTED|UNDEFINED|UNKNOWN
static char          g_lcd_query_boot_part[16]     = {0};  // LCD boot partition label
static bool          g_lcd_query_boot_ready        = false;

// Getters exposing the last captured LCD running-state and boot-proof fields.
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
static inline bool sense_lcd_last_boot_ready() {
  return g_lcd_query_boot_ready;
}

static volatile bool g_lcd_ota_begin_ack_ready = false;
static bool          g_lcd_ota_begin_ack_accepted = false;
static bool          g_lcd_ota_begin_ack_json_ready = false;
static char          g_lcd_ota_begin_ack_reason[32] = {0};
static uint32_t      g_lcd_ota_begin_ack_resume_offset = 0;
static uint16_t      g_lcd_ota_begin_ack_session = 0;
static uint8_t       g_lcd_ota_begin_ack_proto = 1;

static volatile bool g_lcd_ota_end_ack_ready = false;
static bool          g_lcd_ota_end_ack_sha_match = false;
static bool          g_lcd_ota_end_ack_ota_ok = false;

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

// A caller may share this monotonic budget across both fresh attempts.
struct LcdOtaBudget {
  uint32_t started_ms;
  uint32_t limit_ms;
  uint32_t remaining_ms() const {
    const uint32_t elapsed = (uint32_t)(millis() - started_ms);
    return elapsed < limit_ms ? limit_ms - elapsed : 0;
  }
};

struct LcdOtaQuerySnapshot {
  char fw[32], running_part[16], running_state[20], boot_part[16], coord_owner[40];
  uint32_t part_size, peer_boot_id, coord_lease_ms;
  bool boot_ready, correlated, coord_waiting;
};
enum LcdOtaQueryPoll { LCD_QUERY_WAITING, LCD_QUERY_READY, LCD_QUERY_TIMEOUT };
static char g_lcd_query_coord_id[40] = {0};
static uint32_t g_lcd_query_peer_boot_id = 0;
static bool g_lcd_query_coord_waiting = false;
static char g_lcd_query_coord_owner[40] = {0};
static uint32_t g_lcd_query_coord_lease_ms = 0;
static char s_lcd_query_requested_id[40] = {0};
static uint32_t s_lcd_query_started_ms = 0;
static uint32_t s_lcd_query_timeout_ms = 0;
static bool s_lcd_query_pending = false;
static bool s_lcd_query_legacy_boundary = false;
static bool sense_lcd_ota_retry_safe() { return !g_lcd_ota_mode_unconfirmed.load(); }

static void sense_lcd_terminal_flush();

static bool sense_lcd_ota_query_start(const char* coord_id, uint32_t timeout_ms) {
  if (!coord_id || !coord_id[0] || strlen(coord_id) >= sizeof(s_lcd_query_requested_id) ||
      !timeout_ms || timeout_ms > 120000 || g_lcd_ota_proxy_owns_uart ||
      g_spool_owns_uart || g_img_spool_tx_active) return false;
  // Never discard queued user/peer input. The normal pump owns it. A new
  // correlated reply is safe even with pending input; legacy fallback requires
  // an observed empty parser/FIFO boundary before this challenge is sent.
  s_lcd_query_legacy_boundary = lcdSerial.available() == 0 && uart_rx_ring_count == 0 &&
                               uart_rx_frame_len == 0 && !uart_rx_frame_overflow;
  g_lcd_ota_query_resp_ready = false;
  g_lcd_query_coord_id[0] = '\0';
  g_lcd_query_peer_boot_id = 0;
  g_lcd_query_coord_waiting = false;
  g_lcd_query_coord_owner[0] = '\0';
  g_lcd_query_coord_lease_ms = 0;
  strlcpy(s_lcd_query_requested_id, coord_id, sizeof(s_lcd_query_requested_id));
  s_lcd_query_started_ms = millis();
  s_lcd_query_timeout_ms = timeout_ms;
  s_lcd_query_pending = true;
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "LCD_OTA_QUERY";
  doc["msg_id"] = get_next_msg_id();
  doc["coord_id"] = coord_id;
  doc["ts"] = (uint32_t)millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str(), true);  // Explicit bounded readiness probe.
  return true;
}

static LcdOtaQueryPoll sense_lcd_ota_query_poll(LcdOtaQuerySnapshot& snapshot) {
  if (!s_lcd_query_pending ||
      (uint32_t)(millis() - s_lcd_query_started_ms) >= s_lcd_query_timeout_ms) {
    s_lcd_query_pending = false;
    return LCD_QUERY_TIMEOUT;
  }
  if (!g_lcd_ota_query_resp_ready) return LCD_QUERY_WAITING;
  g_lcd_ota_query_resp_ready = false;
  const bool correlated = strcmp(g_lcd_query_coord_id, s_lcd_query_requested_id) == 0;
  const bool legacy = s_lcd_query_legacy_boundary && g_lcd_query_coord_id[0] == '\0';
  if (!correlated && !legacy) return LCD_QUERY_WAITING;
  strlcpy(snapshot.fw, g_lcd_ota_query_resp_fw, sizeof(snapshot.fw));
  strlcpy(snapshot.running_part, g_lcd_query_running_part, sizeof(snapshot.running_part));
  strlcpy(snapshot.running_state, g_lcd_query_running_state, sizeof(snapshot.running_state));
  strlcpy(snapshot.boot_part, g_lcd_query_boot_part, sizeof(snapshot.boot_part));
  snapshot.part_size = g_lcd_ota_query_resp_part_size;
  snapshot.peer_boot_id = g_lcd_query_peer_boot_id;
  snapshot.boot_ready = g_lcd_query_boot_ready;
  snapshot.correlated = correlated;
  snapshot.coord_waiting = g_lcd_query_coord_waiting;
  strlcpy(snapshot.coord_owner, g_lcd_query_coord_owner, sizeof(snapshot.coord_owner));
  snapshot.coord_lease_ms = g_lcd_query_coord_lease_ms;
  if (!snapshot.fw[0] || !snapshot.part_size || !snapshot.boot_ready ||
      strcmp(snapshot.running_state, "VALID") != 0 || !snapshot.running_part[0] ||
      strcmp(snapshot.running_part, "?") == 0 ||
      strcmp(snapshot.running_part, snapshot.boot_part) != 0) return LCD_QUERY_WAITING;
  s_lcd_query_pending = false;
  sense_lcd_mode_confirm();  // Fresh, validated readiness response.
  return LCD_QUERY_READY;
}

// ── Public API ───────────────────────────────────────────────────────

/*
 * sense_lcd_ota_query
 *
 * Query the LCD board for its current firmware version and OTA partition
 * size.  Sends LCD_OTA_QUERY over UART JSON and waits for
 * LCD_OTA_QUERY_RESP.
 *
 * Returns true on success, populating lcd_fw_out and part_size_out. Optional
 * expected_boot_fw requires a post-reboot VALID/ready response within the same
 * 35s budget; its first 7s slot lets END finalization/reboot settle without TX.
 */
static bool sense_lcd_ota_query(char* lcd_fw_out, size_t fw_len,
                                uint32_t* part_size_out,
                                const char* expected_boot_fw = nullptr,
                                uint32_t max_budget_ms = 35000) {
  if (!lcd_fw_out || fw_len == 0 || !max_budget_ms) return false;
  lcd_fw_out[0] = '\0';
  if (part_size_out) *part_size_out = 0;

  // Retry the send + wait a few times so a single missed response doesn't
  // immediately fail the proxy (defense-in-depth alongside the LCD-side
  // keep-awake fix).
  // 5 attempts x 7s = up to 35s cold-boot query budget. The scheduled-OTA
  // case wakes the LCD from its own deep-sleep timer, so it may still be
  // running LVGL init when the first query arrives; give it more retries.
  const int LCD_OTA_QUERY_ATTEMPTS = 5;
  const unsigned long query_started_ms = millis();
  const unsigned long normal_budget_ms = LCD_OTA_QUERY_ATTEMPTS * LCD_OTA_PROXY_QUERY_TIMEOUT_MS;
  const unsigned long query_budget_ms = max_budget_ms < normal_budget_ms ? max_budget_ms : normal_budget_ms;
  bool boot_confirmed = !expected_boot_fw;
  if (expected_boot_fw) {
    // END_ACK precedes the LCD's reboot. Do not queue a command behind its
    // still-active FINALIZING handler; consume one existing query slot first.
    while ((millis() - query_started_ms) < LCD_OTA_PROXY_QUERY_TIMEOUT_MS &&
           (millis() - query_started_ms) < query_budget_ms) {
      pump_uart_rx_once();
      delay(10);
    }
  }
  for (int attempt = expected_boot_fw ? 2 : 1; attempt <= LCD_OTA_QUERY_ATTEMPTS; attempt++) {
    if ((millis() - query_started_ms) >= query_budget_ms) break;
    // Clear mailbox before sending
    g_lcd_ota_query_resp_ready = false;

    // Flush stale UART RX before sending the fresh query. During the Sense's
    // HTTPS-blocking OTA window the main-loop UART drain is starved, so a
    // backlog or ring overflow can accumulate on lcdSerial; if left in place it
    // would desync parsing of the LCD_OTA_QUERY_RESP (the no-response failure
    // we're hardening). The mailbox was already cleared above, so we cannot drop
    // an already-parsed response here. Drain the hardware FIFO, then reset the
    // RX ring / partial-frame state so the next response parses cleanly.
    // Bound the legacy drain by the installed hardware FIFO capacity and the
    // original query budget. Continuous input cannot keep this loop alive.
    size_t discarded = 0;
    while (lcdSerial.available() > 0 && discarded < 1024 &&
           (millis() - query_started_ms) < query_budget_ms) {
      lcdSerial.read();
      ++discarded;
    }
    if (lcdSerial.available() > 0 || (millis() - query_started_ms) >= query_budget_ms) return false;
    uart_reset_rx_state();

    // Send query (fresh msg_id each attempt)
    StaticJsonDocument<128> doc;
    doc["ver"]    = PROTOCOL_VERSION;
    doc["type"]   = "LCD_OTA_QUERY";
    doc["msg_id"] = get_next_msg_id();
    doc["ts"]     = (uint32_t)millis();

    String output;
    serializeJson(doc, output);
    uart_send_json(output.c_str(), expected_boot_fw != nullptr);
    Serial.printf("[LCD_OTA_PROXY] TX LCD_OTA_QUERY (attempt %d/%d)\n",
                  attempt, LCD_OTA_QUERY_ATTEMPTS);

    // Wait for mailbox to be filled by parse_input_message dispatch
    unsigned long start = millis();
    while (!g_lcd_ota_query_resp_ready &&
           (millis() - start) < LCD_OTA_PROXY_QUERY_TIMEOUT_MS &&
           (millis() - query_started_ms) < query_budget_ms) {
      pump_uart_rx_once();   // process incoming frames so INPUT_OTA_CHECK isn't dropped
      delay(10);
    }

    // A blocking RX/TX call may return after the deadline with a response.
    // Never turn that late response into successful postboot proof.
    if ((millis() - query_started_ms) >= query_budget_ms) break;
    if (g_lcd_ota_query_resp_ready) {
      if (!expected_boot_fw) break;
      boot_confirmed = strcmp(g_lcd_ota_query_resp_fw, expected_boot_fw) == 0 &&
                       strcmp(g_lcd_query_running_state, "VALID") == 0 &&
                       g_lcd_query_boot_ready && g_lcd_query_running_part[0] &&
                       strcmp(g_lcd_query_running_part, "?") != 0 &&
                       strcmp(g_lcd_query_running_part, g_lcd_query_boot_part) == 0;
      if (boot_confirmed) break;
      Serial.printf("[LCD_OTA_PROXY] postboot waiting expected=%s actual=%s state=%s ready=%d running=%s boot=%s\n",
                    expected_boot_fw, g_lcd_ota_query_resp_fw, g_lcd_query_running_state,
                    g_lcd_query_boot_ready ? 1 : 0, g_lcd_query_running_part, g_lcd_query_boot_part);
      // A quick pre-ready reply must not exhaust all retries during setup.
      while ((millis() - start) < LCD_OTA_PROXY_QUERY_TIMEOUT_MS &&
             (millis() - query_started_ms) < query_budget_ms) {
        pump_uart_rx_once();
        delay(10);
      }
      continue;
    }

    Serial.printf("[LCD_OTA_PROXY] LCD_OTA_QUERY_RESP timeout (attempt %d/%d)\n",
                  attempt, LCD_OTA_QUERY_ATTEMPTS);
  }

  if ((millis() - query_started_ms) >= query_budget_ms) return false;
  if (expected_boot_fw && !boot_confirmed) {
    Serial.println("[LCD_OTA_PROXY] lcd_postboot_unconfirmed");
    return false;
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

  if (expected_boot_fw) sense_lcd_mode_confirm();
  sense_lcd_terminal_flush();
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
                                         OtaManifest& manifest,
                                         const LcdOtaBudget* budget = nullptr) {
  if (budget && !budget->remaining_ms()) return false;
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

  uint32_t timeout_ms = 10000;
  if (budget && budget->remaining_ms() < timeout_ms) timeout_ms = budget->remaining_ms();
  if (!timeout_ms || !g_manifest_client.fetchManifest(url, manifest, timeout_ms) ||
      (budget && !budget->remaining_ms())) {
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
// Keep terminal evidence separate from the general eight-entry error ring.
// Four attempts survive ordinary reboots; only confirmed JSON mode may bridge.
static uint32_t s_lcd_terminal_store_failures = 0;
static bool sense_lcd_terminal_store(const char* detail, int code) {
  sense_errlog_store("lcd_ota", code, detail);
  Preferences prefs;
  if (!prefs.begin("lcd_xfer", false)) {
    ++s_lcd_terminal_store_failures;
    Serial.println("[LCD_XFER] persistence_failed open");
    return false;
  }
  const uint32_t slot = prefs.getUInt("next", 0) % 4;
  char key[12], valid[12];
  snprintf(valid, sizeof(valid), "valid%lu", (unsigned long)slot);
  bool stored = prefs.putBool(valid, false) == 1;
  if (!stored) {
    prefs.end();
    ++s_lcd_terminal_store_failures;
    Serial.printf("[LCD_XFER] persistence_failed invalidate_slot=%lu\n", (unsigned long)slot);
    return false;
  }
  snprintf(key, sizeof(key), "rec%lu", (unsigned long)slot);
  stored = (prefs.putString(key, detail) == strlen(detail)) && stored;
  snprintf(key, sizeof(key), "build%lu", (unsigned long)slot);
  stored = (prefs.putString(key, kBuildId) == strlen(kBuildId)) && stored;
  snprintf(key, sizeof(key), "code%lu", (unsigned long)slot);
  stored = (prefs.putInt(key, code) == sizeof(int32_t)) && stored;
  snprintf(key, sizeof(key), "pend%lu", (unsigned long)slot);
  stored = (prefs.putBool(key, true) == 1) && stored;
  if (stored) stored = prefs.putBool(valid, true) == 1;
  if (stored) stored = prefs.putUInt("next", slot + 1) == sizeof(uint32_t);
  prefs.end();
  if (!stored) {
    ++s_lcd_terminal_store_failures;
    Serial.printf("[LCD_XFER] persistence_failed slot=%lu\n", (unsigned long)slot);
  }
  return stored;
}

// Supported USB command: lcdxfer. Read-only, bounded to four records, never
// consumes evidence. Refuse while binary ownership is active to avoid pressure
// on the transfer task or a misleading partial journal read.
static void sense_lcd_terminal_dump(Stream& out) {
  if (g_lcd_ota_proxy_owns_uart) { out.println("[LCD_XFER] busy"); return; }
  const uint32_t started_ms = millis();
  auto emit = [&](const char* line) -> bool {
    size_t sent = 0, length = strlen(line);
    while (sent < length && (uint32_t)(millis() - started_ms) < 2000) {
      size_t chunk = length - sent;
      if (chunk > 64) chunk = 64;
      const size_t written = out.write(reinterpret_cast<const uint8_t*>(line + sent), chunk);
      if (written > chunk) return false;
      sent += written;
      if (sent < length) delay(1);
    }
    return sent == length;
  };
  char line[512];
  snprintf(line, sizeof(line), "[LCD_XFER] BEGIN slots=4 write_failures=%lu\n", (unsigned long)s_lcd_terminal_store_failures);
  if (!emit(line)) return;
  Preferences prefs;
  if (!prefs.begin("lcd_xfer", true)) {
    if (!emit("[LCD_XFER] unavailable\n")) return;
  } else {
    for (unsigned slot = 0; slot < 4; ++slot) {
      char key[12];
      snprintf(key, sizeof(key), "valid%u", slot);
      const bool valid = prefs.getBool(key, false);
      snprintf(key, sizeof(key), "rec%u", slot);
      const String detail = prefs.getString(key, "");
      snprintf(key, sizeof(key), "build%u", slot);
      const String build = prefs.getString(key, "");
      snprintf(key, sizeof(key), "code%u", slot);
      const int code = prefs.getInt(key, -1);
      snprintf(key, sizeof(key), "pend%u", slot);
      const bool bounded = build.length() < 96 && detail.length() < 224;
      const int length = snprintf(line, sizeof(line), "[LCD_XFER] slot=%u valid=%u pending=%u code=%d build=%.95s detail=%.223s",
               slot, valid && bounded ? 1 : 0, prefs.getBool(key, false) ? 1 : 0, code,
               build.c_str(), detail.c_str());
      if (length < 0 || (size_t)length >= sizeof(line)) { prefs.end(); return; }
      // Cover the exact record prefix, excluding this suffix and newline. USB
      // drivers can drop previously accepted bytes; END alone cannot prove it.
      const uint16_t crc = UartOtaProtocol::crc16_ccitt(reinterpret_cast<const uint8_t*>(line), length);
      const int suffix = snprintf(line + length, sizeof(line) - length, " bytes=%u crc=%04x\n", (unsigned)length, (unsigned)crc);
      if (suffix < 0 || (size_t)suffix >= sizeof(line) - length) { prefs.end(); return; }
      if (!emit(line)) { prefs.end(); return; }
    }
    prefs.end();
  }
  // No END on a short/blocked write: retrieval must never look complete after
  // truncation. This is a cooperative bound; driver calls remain synchronous.
  emit("[LCD_XFER] END\n");
}

static void sense_lcd_terminal_flush() {
  if (g_lcd_ota_proxy_owns_uart || !sense_lcd_ota_retry_safe()) return;
  Preferences prefs;
  if (!prefs.begin("lcd_xfer", false)) return;
  for (unsigned slot = 0; slot < 4; ++slot) {
    char key[12], pending[12];
    snprintf(key, sizeof(key), "valid%u", slot);
    if (!prefs.getBool(key, false)) continue;
    snprintf(key, sizeof(key), "rec%u", slot);
    snprintf(pending, sizeof(pending), "pend%u", slot);
    if (!prefs.getBool(pending, false)) continue;
    const String detail = prefs.getString(key, "");
    if (!detail.length()) continue;
    uart_send_sense_diag_persist("lcd_ota", "LCD_PROXY_END", "terminal", 0, detail.c_str());
    // This is a best-effort bridge; the dedicated local record remains intact.
    if (prefs.putBool(pending, false) != 1) {
      ++s_lcd_terminal_store_failures;
      Serial.printf("[LCD_XFER] persistence_failed bridge_slot=%u\n", slot);
    }
  }
  prefs.end();
}

static const char* sense_lcd_ota_proxy(const OtaManifest& manifest,
                                      const char* lcd_fw_version,
                                      const LcdOtaBudget* shared_budget = nullptr) {
  if (!sense_lcd_ota_retry_safe()) return "control_mode_unconfirmed";
  if (!sense_uart_ordinary_tx_allowed()) return "uart_busy";
  if (ManifestClient::compareVersions(manifest.version, lcd_fw_version) <= 0) return "up_to_date";
  const LcdOtaBudget local_budget{(uint32_t)millis(), LCD_OTA_PROXY_DOWNLOAD_TIMEOUT_MS};
  auto remaining_ms = [&]() -> uint32_t {
    uint32_t remaining = local_budget.remaining_ms();
    if (shared_budget && shared_budget->remaining_ms() < remaining) remaining = shared_budget->remaining_ms();
    return remaining;
  };
  auto phase_limit = [&](uint32_t wanted) -> uint32_t {
    const uint32_t remaining = remaining_ms();
    return remaining < wanted ? remaining : wanted;
  };
  // Reserve enough remaining time for the already-established END bound.
  if (remaining_ms() <= LCD_OTA_PROXY_END_TIMEOUT_MS) return "attempt_deadline";
  if (!manifest.size || !manifest.sha256[0]) return "download_fail";

  UartOtaProtocol protocol(&lcdSerial);
  protocol.quiet = true;
  if (!protocol.valid()) return "download_fail";
  const uint16_t session_id = (uint16_t)((esp_random() & 0xFFFE) + 1);
  bool control_v2 = false;
  bool json_ready = false;
  bool receiver_open = false;
  uint32_t bytes_read = 0, bytes_sent = 0, retries = 0;
  uint32_t last_read_ms = millis(), last_ack_ms = millis();
  uint16_t seq = 1;
  const char* phase = "begin";
  char terminal[224] = {0};
  WiFiClientSecure tls_client;
  tls_client.setInsecure();
  HTTPClient http;
  http.setReuse(false);
  WiFiClient* stream = nullptr;
  auto close_stream = [&]() {
    http.end();
    tls_client.stop();
    stream = nullptr;
  };
  auto record = [&](const char* origin, int code) {
    snprintf(terminal, sizeof(terminal), "s=%u why=%s p=%s e=%d ms=%lu r=%lu a=%lu q=%u n=%lu c=%lu rd=%lu ak=%lu",
             session_id, origin, phase, code, (unsigned long)(uint32_t)(millis() - local_budget.started_ms),
             (unsigned long)bytes_read, (unsigned long)bytes_sent,
             seq, (unsigned long)retries, (unsigned long)protocol.crc_error_count(),
             (unsigned long)(millis() - last_read_ms), (unsigned long)(millis() - last_ack_ms));
    sense_lcd_terminal_store(terminal, code); // no UART side effect
  };
  auto send_control = [&](const char* type, const char* reason) -> bool {
    StaticJsonDocument<512> doc;
    doc["ver"] = PROTOCOL_VERSION;
    doc["type"] = type;
    doc["msg_id"] = get_next_msg_id();
    doc["ts"] = (uint32_t)millis();
    doc["session_id"] = session_id;
    doc["image_size"] = manifest.size;
    if (reason) doc["reason"] = reason;
    if (terminal[0]) doc["detail"] = terminal;
    String output;
    serializeJson(doc, output);
    if (output.length() > MAX_CHUNK_SIZE) return false;
    if (control_v2) return protocol.send_frame(MSG_OTA_CONTROL, 0,
        reinterpret_cast<const uint8_t*>(output.c_str()), output.length());
    // Legacy peers require JSON controls. A delimiter terminates a damaged
    // prior frame; no ordinary diagnostic JSON is emitted in binary mode.
    const uint8_t delimiter = 0;
    if (lcdSerial.write(&delimiter, 1) != 1) return false;
    const size_t written = lcdSerial.write(reinterpret_cast<const uint8_t*>(output.c_str()), output.length());
    const uint8_t newline = '\n';
    const size_t ended = lcdSerial.write(&newline, 1);
    lcdSerial.flush();
    return written == output.length() && ended == 1;
  };
  auto receive_control = [&](uint32_t timeout_ms, bool want_end) -> const char* {
    const uint32_t started = millis();
    while ((uint32_t)(millis() - started) < timeout_ms && remaining_ms()) {
      uint8_t type = 0, data[MAX_CHUNK_SIZE];
      uint16_t response_seq = 0;
      size_t length = sizeof(data);
      char json[512];
      uint32_t wait = timeout_ms - (uint32_t)(millis() - started);
      if (wait > 100) wait = 100;
      if (wait > remaining_ms()) wait = remaining_ms();
      auto event = protocol.recv_event(&type, &response_seq, data, &length, json, sizeof(json), wait);
      if (!remaining_ms() || (uint32_t)(millis() - started) >= timeout_ms) break;
      if (event != UartOtaProtocol::JSON) continue;
      if (!UartOtaProtocol::json_record_complete(json, strlen(json))) continue;
      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, json) != DeserializationError::Ok ||
          !doc["session_id"].is<uint16_t>() ||
          (doc["session_id"] | (uint16_t)0) != session_id) continue;
      const char* response = doc["type"] | "";
      if (strcmp(response, "LCD_OTA_ABORT_ACK") == 0 && doc["json_ready"].is<bool>() && (doc["json_ready"] | false)) {
        json_ready = true;
        return "receiver_aborted";
      }
      if (want_end && strcmp(response, "LCD_OTA_END_ACK") == 0) {
        if (!doc["sha_match"].is<bool>() || !doc["ota_ok"].is<bool>()) continue;
        json_ready = true;
        if (strcmp(doc["reason"] | "", "attempt_deadline") == 0) return "receiver_deadline";
        if (!(doc["sha_match"] | false)) return "sha_mismatch";
        return (doc["ota_ok"] | false) ? "success" : "lcd_boot_part_fail";
      }
    }
    return "control_ack_timeout";
  };
  auto fail = [&](const char* origin, int code) -> const char* {
    close_stream();
    record(origin, code);
    if (receiver_open && !json_ready && remaining_ms()) {
      g_lcd_ota_proxy_owns_uart = true;
      send_control("LCD_OTA_ABORT", origin);
      // New peers explicitly ACK after cleanup. Old peers have no ABORT_ACK;
      // their 30s idle + 5s parser bound is the fallback, never a renewed transfer.
      receive_control(phase_limit(36000), false);
    }
    // Missing ACK never proves mode handoff. Release local ownership after the
    // bounded grace, but defer new controls until a later readiness episode
    // validates a fresh query response. Never silently renew this attempt.
    if (json_ready || !receiver_open) sense_lcd_mode_confirm();
    else g_lcd_ota_mode_unconfirmed.store(true);
    g_lcd_ota_proxy_owns_uart = false;
    if (json_ready) sense_lcd_terminal_flush();
    return origin;
  };

  // Clear every ACK field before TX; a fast response must not be erased.
  g_lcd_ota_begin_ack_ready = false;
  g_lcd_ota_begin_ack_accepted = false;
  g_lcd_ota_begin_ack_json_ready = false;
  g_lcd_ota_begin_ack_proto = 1;
  g_lcd_ota_begin_ack_session = 0;
  if (!sense_lcd_mode_before_begin()) return fail("mode_marker_failed", -1);
  if (remaining_ms() <= LCD_OTA_PROXY_END_TIMEOUT_MS) return fail("attempt_deadline", -1);
  {
    StaticJsonDocument<512> doc;
    doc["ver"] = PROTOCOL_VERSION;
    doc["type"] = "LCD_OTA_BEGIN";
    doc["msg_id"] = get_next_msg_id();
    doc["ts"] = (uint32_t)millis();
    doc["session_id"] = session_id;
    doc["image_size"] = manifest.size;
    doc["sha256"] = manifest.sha256;
    doc["version"] = manifest.version;
    doc["ota_proto"] = OTA_UART_PROTOCOL_VERSION;
    doc["budget_ms"] = remaining_ms();
    if (manifest.build_id_specified && manifest.build_id[0]) doc["build_id"] = manifest.build_id;
    String output;
    serializeJson(doc, output);
    uart_send_json(output.c_str());
  }
  receiver_open = true; // even a lost BEGIN_ACK can leave an active receiver
  g_lcd_ota_mode_unconfirmed.store(true);
  const uint32_t begin_start = millis(), begin_limit = phase_limit(LCD_OTA_PROXY_BEGIN_TIMEOUT_MS);
  while ((uint32_t)(millis() - begin_start) < begin_limit && remaining_ms()) {
    pump_uart_rx_once();
    if (g_lcd_ota_begin_ack_ready) {
      if (g_lcd_ota_begin_ack_session == session_id ||
          (g_lcd_ota_begin_ack_session == 0 && g_lcd_ota_begin_ack_proto == 1 &&
           !g_lcd_ota_begin_ack_accepted)) break;
      g_lcd_ota_begin_ack_ready = false;
    }
    delay(10);
  }
  if (!remaining_ms()) return fail("attempt_deadline", -1);
  if ((uint32_t)(millis() - begin_start) >= begin_limit) return fail("begin_ack_timeout", -1);
  if (!g_lcd_ota_begin_ack_ready) return fail("begin_ack_timeout", -1);
  control_v2 = g_lcd_ota_begin_ack_proto == OTA_UART_PROTOCOL_VERSION;
  if (!g_lcd_ota_begin_ack_accepted) {
    // A legacy rejection can be stale, and already_active means a peer may
    // still own binary RX. Only explicit correlated cleanup proves JSON mode.
    receiver_open = !(g_lcd_ota_begin_ack_session == session_id &&
                      g_lcd_ota_begin_ack_json_ready);
    static char rejection[64];
    snprintf(rejection, sizeof(rejection), "begin_rejected:%s", g_lcd_ota_begin_ack_reason);
    return fail(rejection, -1);
  }
  if (g_lcd_ota_begin_ack_resume_offset != 0) return fail("resume_not_supported", -1);
  if (remaining_ms() <= LCD_OTA_PROXY_END_TIMEOUT_MS) return fail("attempt_deadline", -1);
  g_lcd_ota_proxy_owns_uart = true;
  phase = "http";

  // Explicit library timeouts remain below the available budget. DNS/driver
  // calls are synchronous: recheck on return and never accept late success.
  http.setTimeout((uint16_t)phase_limit(5000));
  http.setConnectTimeout((int32_t)phase_limit(5000));
  tls_client.setHandshakeTimeout((phase_limit(15000) + 999) / 1000);
  const int http_code = http.begin(tls_client, manifest.url) ? http.GET() : -1;
  if (!remaining_ms()) return fail("attempt_deadline", http_code);
  if (http_code != HTTP_CODE_OK) return fail("http_get_failed", http_code);
  stream = http.getStreamPtr();
  if (!stream) return fail("http_stream_missing", -1);
  const int content_length = http.getSize();
  if (content_length >= 0 && (uint32_t)content_length != manifest.size) return fail("http_size_mismatch", content_length);
  stream->setTimeout(50);
  last_read_ms = last_ack_ms = millis();
  uint8_t chunk[MAX_CHUNK_SIZE];
  while (bytes_sent < manifest.size) {
    phase = "http";
    if (remaining_ms() <= LCD_OTA_PROXY_END_TIMEOUT_MS) return fail("attempt_deadline", -1);
    size_t available = stream->available();
    if (!available && !stream->connected()) return fail("http_disconnected", -1);
    while (!available && stream->connected() &&
           (uint32_t)(millis() - last_read_ms) < LCD_OTA_PROXY_STALL_WINDOW_MS &&
           remaining_ms() > LCD_OTA_PROXY_END_TIMEOUT_MS) {
      delay(10);
      available = stream->available();
    }
    if (remaining_ms() <= LCD_OTA_PROXY_END_TIMEOUT_MS) return fail("attempt_deadline", -1);
    if (!available && !stream->connected()) return fail("http_disconnected", -1);
    if (!available && (uint32_t)(millis() - last_read_ms) >= LCD_OTA_PROXY_STALL_WINDOW_MS)
      return fail("http_no_data", -1);
    size_t wanted = manifest.size - bytes_sent;
    if (wanted > sizeof(chunk)) wanted = sizeof(chunk);
    if (available < wanted) wanted = available;
    const size_t got = wanted ? stream->readBytes(chunk, wanted) : 0;
    if (!remaining_ms()) return fail("attempt_deadline", -1);
    if ((uint32_t)(millis() - last_read_ms) >= LCD_OTA_PROXY_STALL_WINDOW_MS)
      return fail("http_read_stall", -1);
    if (!got) continue;
    if (got > wanted) return fail("http_read_size", -1);
    bytes_read += got;
    phase = "chunk";
    last_read_ms = millis();
    bool acked = false;
    const char* chunk_failure = "chunk_retry_exhausted";
    // A legacy receiver appends duplicate CHUNKs. Once an ACK is uncertain,
    // abort that session rather than corrupt its image with a retransmission.
    const unsigned chunk_attempts = control_v2 ? LCD_OTA_PROXY_MAX_RETRIES : 1;
    for (unsigned retry = 0; retry < chunk_attempts && remaining_ms(); ++retry) {
      if (retry) ++retries;
      if (!protocol.send_frame(MSG_CHUNK, seq, chunk, got)) {
        chunk_failure = "uart_short_write";
        continue;
      }
      const uint32_t ack_start = millis(), ack_limit = phase_limit(LCD_OTA_PROXY_ACK_TIMEOUT_MS);
      while ((uint32_t)(millis() - ack_start) < ack_limit && remaining_ms()) {
        uint8_t type = 0, data[MAX_CHUNK_SIZE]; uint16_t response_seq = 0;
        size_t length = sizeof(data); char json[512];
        uint32_t wait = ack_limit - (uint32_t)(millis() - ack_start);
        if (wait > 100) wait = 100;
        if (wait > remaining_ms()) wait = remaining_ms();
        auto event = protocol.recv_event(&type, &response_seq, data, &length, json, sizeof(json), wait);
        if (!remaining_ms() || (uint32_t)(millis() - ack_start) >= ack_limit) break;
        if (event == UartOtaProtocol::JSON) {
          if (!UartOtaProtocol::json_record_complete(json, strlen(json))) continue;
          StaticJsonDocument<512> doc;
          if (deserializeJson(doc, json) != DeserializationError::Ok ||
              !doc["session_id"].is<uint16_t>() ||
          (doc["session_id"] | (uint16_t)0) != session_id) continue;
          const char* response = doc["type"] | "";
          if (strcmp(response, "LCD_OTA_ABORT_ACK") == 0 && doc["json_ready"].is<bool>() && (doc["json_ready"] | false)) {
            json_ready = true;
            return fail("receiver_aborted", -1);
          }
          if (strcmp(response, "LCD_OTA_ABORT") == 0) chunk_failure = "receiver_aborted";
          continue;
        }
        if (event != UartOtaProtocol::FRAME || response_seq != seq) continue;
        if (type == MSG_ACK && length == 0) { acked = true; last_ack_ms = millis(); break; }
        if (type == MSG_NACK) return fail("receiver_nack", length ? data[0] : -1);
      }
      if (acked) break;
    }
    if (!remaining_ms()) return fail("attempt_deadline", -1);
    if (!acked) return fail(chunk_failure, -1);
    bytes_sent += got;
    seq = (uint16_t)(seq + 1);
  }
  close_stream();
  phase = "end";
  if (!remaining_ms()) return fail("attempt_deadline", -1);
  snprintf(terminal, sizeof(terminal), "s=%u why=bytes_complete r=%lu a=%lu q=%u n=%lu",
           session_id, (unsigned long)bytes_read, (unsigned long)bytes_sent, seq, (unsigned long)retries);
  if (!send_control("LCD_OTA_END", nullptr)) return fail("end_short_write", -1);
  const char* result = receive_control(phase_limit(LCD_OTA_PROXY_END_TIMEOUT_MS), true);
  if (json_ready) sense_lcd_mode_confirm(strcmp(result, "success") != 0);
  else g_lcd_ota_mode_unconfirmed.store(true);
  g_lcd_ota_proxy_owns_uart = false;
  record(result, strcmp(result, "success") == 0 ? 0 : -1);
  // Successful END_ACK precedes reboot; leave the bridge pending until a fresh
  // query proves the new receiver is ready. Failed finalization is also queried.
  return result;
}

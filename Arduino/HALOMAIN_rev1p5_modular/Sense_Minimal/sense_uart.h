/*
 * sense_uart.h
 *
 * UART transport layer for Sense_Minimal.
 * Ring buffer, frame assembly, core send/receive, protocol validation,
 * message ID generation, and basic diagnostic sender.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 3.
 *
 * Prerequisites (must be declared before #include "sense_uart.h"):
 *   - Arduino.h, ArduinoJson.h, HardwareSerial.h
 *   - driver/uart.h (uart_wait_tx_done, UART_NUM_1)
 *   - lcdSerial (HardwareSerial)
 *   - LCD_UART_PORT (uart_port_t)
 *   - PROTOCOL_VERSION, MAX_LINE_LENGTH, UART_RX_RING_SIZE, UART_RX_FRAME_MAX
 *   - UART_BAUD_RATE, UART_TX_PIN, UART_RX_PIN, UART_RX_DEBUG
 *   - HALO_DEBUG_SENSITIVE, HALO_SENSE_LCD_DIAG_BRIDGE
 *   - UART tracking globals: last_uart_rx_ms, last_uart_tx_ms,
 *     uart_tx_count, uart_rx_count
 */

#ifndef SENSE_UART_H
#define SENSE_UART_H

// Forward declaration - parse_input_message stays in .ino (dispatch layer)
static bool parse_input_message(const char* json_str);

// LCD OTA proxy UART ownership flag — when true, suppress JSON TX
// (binary COBS framing is in progress on lcdSerial)
static volatile bool g_lcd_ota_proxy_owns_uart = false;

// ── UART ring buffer & protocol state ────────────────────────────────
static uint32_t sense_msg_id_counter = 1;
static char uart_rx_ring[UART_RX_RING_SIZE];
static size_t uart_rx_ring_head = 0;
static size_t uart_rx_ring_tail = 0;
static size_t uart_rx_ring_count = 0;
static char uart_rx_frame[UART_RX_FRAME_MAX + 1];
static size_t uart_rx_frame_len = 0;
static bool uart_rx_frame_overflow = false;
static unsigned long uart_rx_dropped_since_frame = 0;
static size_t uart_rx_oversize_drop = 0;
static bool uart_initialized = false;

// ── TX/RX type tracking ──────────────────────────────────────────────

static void uart_note_tx_type(const char* json_str) {
  if (!json_str) {
    return;
  }
  const char* key = "\"type\":\"";
  const char* start = strstr(json_str, key);
  if (!start) {
    return;
  }
  start += strlen(key);
  const char* end = strchr(start, '"');
  if (!end || end <= start) {
    return;
  }
  size_t len = static_cast<size_t>(end - start);
  if (len >= sizeof(last_uart_tx_type)) {
    len = sizeof(last_uart_tx_type) - 1;
  }
  memcpy(last_uart_tx_type, start, len);
  last_uart_tx_type[len] = '\0';
  last_uart_tx_type_ms = millis();
}

static void uart_note_rx_type(const char* type) {
  if (!type || !type[0]) {
    return;
  }
  strncpy(last_uart_rx_type, type, sizeof(last_uart_rx_type) - 1);
  last_uart_rx_type[sizeof(last_uart_rx_type) - 1] = '\0';
  last_uart_rx_type_ms = millis();
}

// ── UART initialization ──────────────────────────────────────────────

static void initUarts() {
  if (uart_initialized) {
    Serial.println("[UART_INIT] skipped already_initialized=1");
    return;
  }
  Serial.begin(115200);
  delay(50);
  lcdSerial.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  delay(50);
  uart_rx_ring_head = 0;
  uart_rx_ring_tail = 0;
  uart_rx_ring_count = 0;
  uart_rx_frame_len = 0;
  uart_rx_frame_overflow = false;
  uart_rx_dropped_since_frame = 0;
  uart_initialized = true;
  Serial.printf("UART initialized: TX=GPIO%d, RX=GPIO%d, Baud=%d\n",
                UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
}

// ── RX ring buffer primitives ────────────────────────────────────────

static bool uart_rx_is_printable(char c) {
  return c >= 0x20 && c <= 0x7E;
}

static void uart_reset_rx_state() {
  uart_rx_ring_head = 0;
  uart_rx_ring_tail = 0;
  uart_rx_ring_count = 0;
  uart_rx_frame_len = 0;
  uart_rx_frame_overflow = false;
  uart_rx_dropped_since_frame = 0;
  uart_rx_oversize_drop = 0;
}

static void link_reset_parser_state() {
  uart_reset_rx_state();
}

static void uart_ring_push(char c) {
  if (uart_rx_ring_count >= UART_RX_RING_SIZE) {
    uart_rx_dropped_since_frame++;
    return;
  }
  uart_rx_ring[uart_rx_ring_head] = c;
  uart_rx_ring_head = (uart_rx_ring_head + 1) % UART_RX_RING_SIZE;
  uart_rx_ring_count++;
}

static bool uart_ring_pop(char* out) {
  if (uart_rx_ring_count == 0) {
    return false;
  }
  *out = uart_rx_ring[uart_rx_ring_tail];
  uart_rx_ring_tail = (uart_rx_ring_tail + 1) % UART_RX_RING_SIZE;
  uart_rx_ring_count--;
  return true;
}

// ── RX frame processing ─────────────────────────────────────────────

static void uart_process_rx_ring() {
  char c = '\0';
  while (uart_ring_pop(&c)) {
    if (c == '\n') {
      if (uart_rx_frame_overflow) {
        Serial.printf("[PROTO] frame_oversize dropped_to_newline bytes=%u\n",
                      (unsigned)uart_rx_oversize_drop);
      } else if (uart_rx_frame_len > 0) {
        size_t start = 0;
        size_t end = uart_rx_frame_len;
        while (start < end && uart_rx_frame[start] == ' ') {
          start++;
        }
        while (end > start && uart_rx_frame[end - 1] == ' ') {
          end--;
        }
        size_t trimmed_len = end > start ? (end - start) : 0;
        if (trimmed_len > 0) {
          if (uart_rx_frame[start] != '{') {
            Serial.printf("[PROTO] dropped_nonjson_line len=%u first_char=%c\n",
                          (unsigned)trimmed_len,
                          uart_rx_frame[start]);
          } else {
            if (start > 0 || end < uart_rx_frame_len) {
              memmove(uart_rx_frame, uart_rx_frame + start, trimmed_len);
            }
            uart_rx_frame[trimmed_len] = '\0';
            parse_input_message(uart_rx_frame);
          }
        }
      }
      uart_rx_frame_len = 0;
      uart_rx_frame_overflow = false;
      uart_rx_dropped_since_frame = 0;
      uart_rx_oversize_drop = 0;
      continue;
    }
    if (uart_rx_frame_overflow) {
      uart_rx_dropped_since_frame++;
      uart_rx_oversize_drop++;
      continue;
    }
    if (uart_rx_frame_len >= UART_RX_FRAME_MAX) {
      uart_rx_frame_overflow = true;
      uart_rx_dropped_since_frame++;
      uart_rx_oversize_drop++;
      continue;
    }
    uart_rx_frame[uart_rx_frame_len++] = c;
  }
}

// ── Post-wake RX sanitization ────────────────────────────────────────

static void wake_rx_sanitize() {
  uart_reset_rx_state();
  const unsigned long start_ms = millis();
  const unsigned long max_ms = 20;
  const int max_bytes = 256;
  int read_bytes = 0;
  uint8_t first_bytes[8];
  size_t first_len = 0;
  int last_byte = -1;
  bool saw_frame_start = false;
  while (lcdSerial.available() > 0 &&
         (millis() - start_ms) < max_ms &&
         read_bytes < max_bytes) {
    int raw = lcdSerial.read();
    if (raw < 0) {
      break;
    }
    char c = static_cast<char>(raw);
    read_bytes++;
    last_byte = raw & 0xFF;
    if (c == '{') {
      uart_ring_push(c);
      saw_frame_start = true;
      break;
    }
    if (uart_rx_dropped_since_frame < 0xFFFFFFFFu) {
      uart_rx_dropped_since_frame++;
    }
    if (first_len < sizeof(first_bytes)) {
      first_bytes[first_len++] = static_cast<uint8_t>(raw);
    }
  }
  char first_hex[3 * 8 + 1];
  size_t pos = 0;
  for (size_t i = 0; i < first_len && pos + 3 < sizeof(first_hex); i++) {
    int n = snprintf(first_hex + pos, sizeof(first_hex) - pos, "%02X", first_bytes[i]);
    if (n <= 0) {
      break;
    }
    pos += static_cast<size_t>(n);
    if (i + 1 < first_len && pos + 1 < sizeof(first_hex)) {
      first_hex[pos++] = ' ';
      first_hex[pos] = '\0';
    }
  }
  if (pos == 0) {
    strcpy(first_hex, "-");
  }
  Serial.printf("[UART_SAN] dropped_bytes=%lu first_bytes_hex=%s last_byte=0x%02X\n",
                uart_rx_dropped_since_frame,
                first_hex,
                last_byte >= 0 ? last_byte : 0xFF);
  if (saw_frame_start) {
    uart_process_rx_ring();
  }
}

// ── Message ID generation ────────────────────────────────────────────

static uint32_t get_next_msg_id() {
  uint32_t id = sense_msg_id_counter++;
  if (sense_msg_id_counter == 0) {
    sense_msg_id_counter = 1;  // Wrap at 2^32, but avoid 0
  }
  return id;
}

// ── Protocol validation ──────────────────────────────────────────────

static bool validate_protocol_message(JsonDocument& doc) {
  if (!doc.containsKey("ver")) {
    Serial.println("[PROTO] Invalid: missing ver");
    return false;
  }
  int ver = doc["ver"] | 0;
  if (ver != PROTOCOL_VERSION) {
    Serial.printf("[PROTO] Invalid version: %d (expected %d)\n", ver, PROTOCOL_VERSION);
    return false;
  }
  if (!doc.containsKey("type")) {
    Serial.println("[PROTO] Invalid: missing type");
    return false;
  }
  if (!doc.containsKey("msg_id")) {
    Serial.println("[PROTO] Invalid: missing msg_id");
    return false;
  }
  if (!doc.containsKey("ts")) {
    Serial.println("[PROTO] Invalid: missing ts");
    return false;
  }
  return true;
}

// ── Core TX ──────────────────────────────────────────────────────────

static void uart_send_json(const char* json_str) {
  // Block JSON TX while LCD OTA proxy owns the UART for binary COBS framing
  if (g_lcd_ota_proxy_owns_uart) {
    return;
  }
  size_t len = strlen(json_str);
  last_uart_tx_ms = millis();
  uart_tx_count++;
  uart_note_tx_type(json_str);
  lcdSerial.print(json_str);
  lcdSerial.print("\n");
  lcdSerial.flush();
#if HALO_DEBUG_SENSITIVE
  Serial.printf("[PROTO] TX: len=%d, %s\n", (int)len, json_str);
#else
  auto contains_sensitive_key = [](const char* s) -> bool {
    if (!s) return false;
    return strstr(s, "\"pass\"") != nullptr ||
           strstr(s, "\"password\"") != nullptr ||
           strstr(s, "\"psk\"") != nullptr;
  };
  if (!contains_sensitive_key(json_str)) {
    Serial.printf("[PROTO] TX: len=%d, %s\n", (int)len, json_str);
  } else {
    char redacted[256];
    redacted[0] = '\0';
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, json_str);
    if (!err && doc.is<JsonObject>()) {
      if (doc.containsKey("pass")) doc["pass"] = "***";
      if (doc.containsKey("password")) doc["password"] = "***";
      if (doc.containsKey("psk")) doc["psk"] = "***";
      serializeJson(doc, redacted, sizeof(redacted));
    }
    if (redacted[0] == '\0') {
      strncpy(redacted, "<redacted>", sizeof(redacted) - 1);
      redacted[sizeof(redacted) - 1] = '\0';
    }
    Serial.printf("[PROTO] TX: len=%d, %s\n", (int)len, redacted);
  }
#endif
}

// ── Diagnostic sender ────────────────────────────────────────────────

static void uart_send_sense_diag(const char* area,
                                 const char* event,
                                 const char* label,
                                 int32_t code,
                                 const char* detail) {
#if HALO_SENSE_LCD_DIAG_BRIDGE
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "SENSE_DIAG";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["area"] = area ? area : "";
  doc["event"] = event ? event : "";
  doc["code"] = code;
  if (label && label[0]) {
    doc["label"] = label;
  }
  if (detail && detail[0]) {
    doc["detail"] = detail;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
#else
  (void)area;
  (void)event;
  (void)label;
  (void)code;
  (void)detail;
#endif
}

// ── Persistent diagnostic sender (forwarded to LCD for NVS storage) ──

static void uart_send_sense_diag_persist(const char* area,
                                         const char* event,
                                         const char* label,
                                         int32_t code,
                                         const char* detail) {
#if HALO_SENSE_LCD_DIAG_BRIDGE
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "SENSE_DIAG";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["area"] = area ? area : "";
  doc["event"] = event ? event : "";
  doc["code"] = code;
  doc["persist"] = true;
  if (label && label[0]) {
    doc["label"] = label;
  }
  if (detail && detail[0]) {
    doc["detail"] = detail;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
#else
  (void)area;
  (void)event;
  (void)label;
  (void)code;
  (void)detail;
#endif
}

#endif // SENSE_UART_H

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

#include <atomic>

// Forward declaration - parse_input_message stays in .ino (dispatch layer)
static bool parse_input_message(const char* json_str);
static bool sense_img_spool_on_json(const char* json_str);
static bool sense_img_spool_begin_matches(const char* json_str);
static bool sense_lcd_query_busy();
static std::atomic<bool> g_img_spool_request_active{false};
static std::atomic<int> s_img_ready_result{-2}; // cancelled, pending(-1), refused(0), binary(1)
static bool sense_img_spool_binary_pending() { return s_img_ready_result.load() == 1; }

// LCD OTA proxy UART ownership flag — when true, suppress JSON TX
// (binary COBS framing is in progress on lcdSerial)
static volatile bool g_lcd_ota_proxy_owns_uart = false;
// An interrupted sender must not forget that its peer may still be receiving.
// This blocks ordinary JSON, independently of local binary-task ownership.
static std::atomic<bool> g_lcd_ota_mode_unconfirmed{true};
static bool s_lcd_mode_marker_pending = true;
static bool sense_uart_ordinary_tx_allowed() {
  return !g_lcd_ota_proxy_owns_uart && !g_spool_owns_uart && !g_img_spool_tx_active &&
         !g_img_spool_request_active.load() &&
         !g_lcd_ota_mode_unconfirmed.load();
}

static void sense_lcd_mode_restore() {
  Preferences prefs;
  bool unsafe = true;  // Missing/unreadable state requires a fresh mode probe.
  if (prefs.begin("lcd_xfer", true)) {
    unsafe = prefs.getBool("unsafe", true);
    prefs.end();
  }
  s_lcd_mode_marker_pending = unsafe;
  g_lcd_ota_mode_unconfirmed.store(unsafe);
}

static bool sense_lcd_mode_before_begin() {
  Preferences prefs;
  if (!prefs.begin("lcd_xfer", false)) return false;
  const bool stored = prefs.putBool("unsafe", true) == 1;
  prefs.end();
  if (stored) s_lcd_mode_marker_pending = true;
  else Serial.println("[LCD_XFER] mode_marker_write_failed");
  return stored;
}

static void sense_lcd_mode_confirm(bool persist = true) {
  g_lcd_ota_mode_unconfirmed.store(false);
  if (!persist || !s_lcd_mode_marker_pending) return;
  Preferences prefs;
  bool stored = false;
  if (prefs.begin("lcd_xfer", false)) {
    stored = prefs.putBool("unsafe", false) == 1;
    prefs.end();
  }
  if (stored) s_lcd_mode_marker_pending = false;
  else Serial.println("[LCD_XFER] mode_marker_clear_failed; next boot will recheck");
}

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

// JSON frames have two writes (payload and newline). HardwareSerial locks each
// write separately, so a worker can otherwise append its payload to a diagnostic
// frame before its newline; the LCD then silently parses only the first object.
// Initialize once in initUarts(), before any worker task starts. Static storage
// avoids adding an allocation to the memory-sensitive upload/OTA paths.
static StaticSemaphore_t uart_json_tx_mutex_storage;
static SemaphoreHandle_t uart_json_tx_mutex = NULL;
static StaticSemaphore_t uart_rx_mutex_storage;
static SemaphoreHandle_t uart_rx_mutex = NULL;
static std::atomic<uint32_t> uart_dispatch_depth{0};

class UartDispatchLease {
 public:
  void claim() { uart_dispatch_depth.fetch_add(1); held_ = true; }
  ~UartDispatchLease() { if (held_) uart_dispatch_depth.fetch_sub(1); }
 private:
  bool held_ = false;
};
static constexpr uint32_t UART_JSON_TX_LOCK_WAIT_MS = 2000;

class UartRxLock {
 public:
  explicit UartRxLock(uint32_t wait_ms = 0) : held_(uart_rx_mutex != NULL &&
      xSemaphoreTake(uart_rx_mutex, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {}
  ~UartRxLock() { release(); }
  bool held() const { return held_; }
  void release() { if (held_) { xSemaphoreGive(uart_rx_mutex); held_ = false; } }
 private:
  bool held_;
};

static void uart_json_tx_init() {
  if (uart_rx_mutex == NULL) {
    uart_rx_mutex = xSemaphoreCreateMutexStatic(&uart_rx_mutex_storage);
  }
  if (uart_json_tx_mutex == NULL) {
    uart_json_tx_mutex = xSemaphoreCreateMutexStatic(&uart_json_tx_mutex_storage);
  }
}

class UartJsonTxLock {
 public:
  UartJsonTxLock() : held_(uart_json_tx_mutex != NULL &&
      xSemaphoreTake(uart_json_tx_mutex, pdMS_TO_TICKS(UART_JSON_TX_LOCK_WAIT_MS)) == pdTRUE) {}
  ~UartJsonTxLock() { release(); }
  bool held() const { return held_; }
  void release() {
    if (held_) {
      xSemaphoreGive(uart_json_tx_mutex);
      held_ = false;
    }
  }
 private:
  bool held_;
};

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
  uart_json_tx_init();
  sense_lcd_mode_restore();
  Serial.begin(115200);
  // Keep USB console backpressure bounded when a cable remains attached but
  // no host drains logs. Core3.3.8 HWCDC::write decrements its unsigned retry
  // counter before checking zero; timeout 0 underflows when its BYTEBUF is full
  // because a zero-byte ring send succeeds without progress. A positive 1 ms
  // timeout bounds that no-progress path (and flush) without changing UART1.
  // This is the SDK retry timeout, not a hard wall-clock deadline per write.
  Serial.setTxTimeoutMs(1);
  delay(50);
  // 1024, matching the LCD. Until the SD-spool drain existed the Sense only
  // ever RECEIVED small JSON lines and only ever SENT binary, so the 256-byte
  // default was enough. Receiving a ~521-byte COBS frame overflows it and drops
  // bytes mid-frame, which surfaces as a truncated decode rather than an error:
  //     [UART_OTA] Frame too short: decoded_len=391, expected=519 (data_len=512)
  // The header parses correctly there - the frame is simply missing bytes.
  lcdSerial.setRxBufferSize(1024);
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
  UartRxLock rx(1000);
  if (!rx.held()) return;
  // SYNC resets only an unfinished wire frame. Complete queued INPUTs retain
  // their order and must not disappear because a SYNC preceded them.
  uart_rx_frame_len = 0;
  uart_rx_frame_overflow = false;
  uart_rx_oversize_drop = 0;
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

// Only complete ordinary JSON lines enter the ring. The raw collector and
// binary reader share one lease; protocol callbacks run after releasing it.
static void uart_process_rx_ring() {
  for (size_t frames = 0; frames < UART_RX_RING_SIZE; ++frames) {
    char line[UART_RX_FRAME_MAX + 1];
    size_t n = 0;
    UartDispatchLease dispatch;
    {
      UartRxLock rx;
      if (!rx.held() || g_lcd_ota_proxy_owns_uart || g_spool_owns_uart ||
          (g_img_spool_request_active.load() && s_img_ready_result.load() != 0) ||
          (g_img_spool_tx_active || sense_img_spool_binary_pending()) || !uart_rx_ring_count) return;
      char c;
      while (uart_ring_pop(&c) && c != '\n') {
        if (n < UART_RX_FRAME_MAX) line[n++] = c;
      }
      line[n] = '\0';
      // Reserve callback admission before releasing RX. A worker must not
      // BEGIN between this check and the callback's ACK/UI writes. Nested
      // pumps add depth; neither RX nor TX stays locked across callbacks.
      dispatch.claim();
    }
    if (n) parse_input_message(line);
  }
}

// Collection alone is safe from either the main sleep path or upload worker.
// It never executes user actions while a caller owns a dequeued photo. READY
// has a dedicated typed mailbox because deployed LCD159 omits msg_id/ts.
static void uart_collect_rx_once() {
  UartRxLock rx;
  if (!rx.held() || g_lcd_ota_proxy_owns_uart || g_spool_owns_uart ||
      (g_img_spool_tx_active || sense_img_spool_binary_pending())) return;
  size_t read = 0;
  while (lcdSerial.available() > 0 && read++ < 1024) {
    const char c = (char)lcdSerial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (!uart_rx_is_printable(c)) { ++uart_rx_dropped_since_frame; continue; }
      if (uart_rx_frame_len < UART_RX_FRAME_MAX && !uart_rx_frame_overflow)
        uart_rx_frame[uart_rx_frame_len++] = c;
      else { uart_rx_frame_overflow = true; ++uart_rx_oversize_drop; }
      continue;
    }
    uart_rx_frame[uart_rx_frame_len] = '\0';
    if (!uart_rx_frame_overflow && uart_rx_frame_len) {
      if (!sense_img_spool_on_json(uart_rx_frame)) {
        if (uart_rx_frame_len + 1 <= UART_RX_RING_SIZE - uart_rx_ring_count) {
          for (size_t i = 0; i < uart_rx_frame_len; ++i) uart_ring_push(uart_rx_frame[i]);
          uart_ring_push('\n');
        } else {
          // Refuse a whole line; never overwrite a queued command or enqueue
          // a truncated JSON prefix. The existing sender retries unacked input.
          uart_rx_dropped_since_frame += uart_rx_frame_len + 1;
        }
      }
    }
    uart_rx_frame_len = 0;
    uart_rx_frame_overflow = false;
    uart_rx_oversize_drop = 0;
    if (sense_img_spool_binary_pending()) break; // positive READY: COBS owns subsequent bytes
  }
}

static void pump_uart_rx_once() {
  uart_collect_rx_once();
  uart_process_rx_ring();
}

// ── Post-wake RX sanitization ────────────────────────────────────────

static void wake_rx_sanitize() {
  UartRxLock rx(1000);
  if (!rx.held() || g_lcd_ota_proxy_owns_uart || g_spool_owns_uart ||
      g_img_spool_request_active.load()) return;
  uart_reset_rx_state();
  const unsigned long start_ms = millis();
  const unsigned long max_ms = 20;
  const int max_bytes = 256;
  int read_bytes = 0;
  uint8_t first_bytes[8];
  size_t first_len = 0;
  int last_byte = -1;
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
      uart_rx_frame[0] = c;
      uart_rx_frame_len = 1;
      break;
    }
    if (uart_rx_dropped_since_frame < 0xFFFFFFFFu) {
      uart_rx_dropped_since_frame++;
    }
    if (first_len < sizeof(first_bytes)) {
      first_bytes[first_len++] = static_cast<uint8_t>(raw);
    }
  }
  const unsigned long dropped = uart_rx_dropped_since_frame;
  rx.release();
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
                dropped,
                first_hex,
                last_byte >= 0 ? last_byte : 0xFF);
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

static void uart_send_json(const char* json_str, bool explicit_mode_probe = false,
                           bool explicit_img_begin = false) {
  if (!json_str) return;
  const size_t len = strlen(json_str);
  // Task context only; there are no callbacks or recursive sends in this scope.
  // A 12KB list occupies ~1.07s at 115200 baud. Bound contention at 2s, and keep
  // slow USB diagnostics outside the lock. Recheck ownership AFTER waiting.
  UartJsonTxLock tx_lock;
  if (!tx_lock.held()) {
    Serial.println("[UART_TX] json_skipped reason=tx_lock_unavailable");
    return;
  }
  // The LCD may switch to binary as soon as it emits READY, before our RX
  // collector observes it. Reserve TX from BEGIN admission until explicit
  // refusal or transfer completion; only this request's own BEGIN may pass.
  if (g_img_spool_request_active.load() && s_img_ready_result.load() != 0) {
    if (!explicit_img_begin || !sense_img_spool_begin_matches(json_str)) return;
  }
  // Block JSON TX while LCD OTA proxy owns the UART for binary COBS framing
  if (g_lcd_ota_proxy_owns_uart) {
    return;
  }
  if (g_lcd_ota_mode_unconfirmed.load()) {
    // Only the bounded query APIs can opt in. No general producer can bypass
    // quarantine by choosing a type string or racing a global probe flag.
    if (!explicit_mode_probe || !json_str || strlen(json_str) >= 256) return;
    StaticJsonDocument<256> probe;
    if (deserializeJson(probe, json_str) != DeserializationError::Ok ||
        strcmp(probe["type"] | "", "LCD_OTA_QUERY") != 0) return;
  }
  // Same rule for the SD-spool drain. The Sense emits SENSE_DIAG rssi reports
  // roughly every 2s; during a drain those land inside the LCD's ACK reads and
  // corrupt its frame parser. Dropping a couple of diagnostic lines for the
  // ~18s of a transfer is free — they are periodic and the next one is along
  // shortly — whereas a corrupted frame costs the whole image.
  if (g_spool_owns_uart) {
    return;
  }
  // And while the Sense is SENDING an image. This is the direction that was
  // missing: the RX pump was already guarded, so ACKs were not being stolen —
  // but nothing stopped this board writing a SENSE_DIAG line into the middle of
  // its own outbound COBS stream. The transfer then dies at a random chunk
  // (seq=5, 24, 34 across runs) because the interferer is periodic, not
  // positional.
  if (g_img_spool_tx_active || sense_img_spool_binary_pending()) {
    return;
  }
  last_uart_tx_ms = millis();
  uart_tx_count++;
  uart_note_tx_type(json_str);
  lcdSerial.print(json_str);
  lcdSerial.print("\n");
  lcdSerial.flush();
  tx_lock.release();
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

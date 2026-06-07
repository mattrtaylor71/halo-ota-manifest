/*
 * lcd_uart.h
 *
 * UART TX protocol layer: initialization, message IDs, protocol
 * validation, send helpers, and TX-queue message dispatch.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 1.
 *
 * Prerequisites (must be declared before #include "lcd_uart.h"):
 *   - HardwareSerial.h, ArduinoJson.h, driver/gpio.h, <string.h>
 *   - PROTOCOL_VERSION, MAX_LINE_LENGTH
 *   - UART_BAUD_RATE, UART_TX_PIN, UART_RX_PIN
 *   - senseSerial (HardwareSerial)
 *   - lcd_msg_id_counter, uart_rx_line_buffer, uart_rx_line_pos
 *   - uart_rx_raw_bytes_seen, uart_rx_completed_lines_seen,
 *     uart_rx_valid_msgs_seen, uart_rx_partial_started_ms,
 *     uart_rx_last_summary_ms, uart_rx_last_partial_log_ms,
 *     uart_rx_diag_raw_logged
 *   - uart_tx_count, uart_rx_count
 *   - last_uart_tx_type[], last_uart_tx_type_ms
 *   - last_uart_rx_type[], last_uart_rx_type_ms
 *   - last_input_type[], last_input_type_ms
 */

#ifndef LCD_UART_H
#define LCD_UART_H

// Forward declarations for .ino functions called by UART helpers
static void request_sense_wake(const char* reason);

// When true, suppress all JSON TX on senseSerial to avoid corrupting
// binary COBS frames during LCD OTA.  Set by lcd_ota_uart.h.
static bool g_suppress_uart_json_tx = false;

// ── UART TX Queue ────────────────────────────────────────────────────
typedef struct {
  char type[24];
  int delta;
  char id[40];
  bool has_delta;
  bool has_id;
} tx_msg_t;

static QueueHandle_t uart_tx_queue = NULL;
static tx_msg_t deferred_awake_tx_msg = {};
static bool deferred_awake_tx_valid = false;
static unsigned long deferred_awake_tx_last_ping_ms = 0;

// ── UART init ────────────────────────────────────────────────────────
static void init_uart() {
  Serial.println("Initializing UART to Sense board...");
  senseSerial.setRxBufferSize(1024);  // OTA COBS frames can be ~521 bytes
  senseSerial.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.printf("UART initialized: TX=GPIO%d, RX=GPIO%d, Baud=%d\n",
                UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
  Serial.printf("[UART_DIAG] init rx_gpio=%d rx_level=%d tx_gpio=%d\n",
                UART_RX_PIN,
                (int)gpio_get_level((gpio_num_t)UART_RX_PIN),
                UART_TX_PIN);
  uart_rx_line_pos = 0;
  uart_rx_line_buffer[0] = '\0';
  uart_rx_raw_bytes_seen = 0;
  uart_rx_completed_lines_seen = 0;
  uart_rx_valid_msgs_seen = 0;
  uart_rx_partial_started_ms = 0;
  uart_rx_last_summary_ms = 0;
  uart_rx_last_partial_log_ms = 0;
  uart_rx_diag_raw_logged = 0;
}

// ── Protocol helpers ─────────────────────────────────────────────────
static uint32_t get_next_msg_id() {
  uint32_t id = lcd_msg_id_counter++;
  if (lcd_msg_id_counter == 0) {
    lcd_msg_id_counter = 1;
  }
  return id;
}

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

// ── UART note helpers ────────────────────────────────────────────────
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

static void uart_note_input_type(const char* type) {
  if (!type || !type[0]) {
    return;
  }
  if (strncmp(type, "INPUT_", 6) != 0) {
    return;
  }
  strncpy(last_input_type, type, sizeof(last_input_type) - 1);
  last_input_type[sizeof(last_input_type) - 1] = '\0';
  last_input_type_ms = millis();
}

// ── UART base send ───────────────────────────────────────────────────
static void uart_send_json(const char* json_str) {
  if (json_str == NULL || strlen(json_str) == 0) {
    return;
  }
  if (g_suppress_uart_json_tx) return;
  uart_tx_count++;
  uart_note_tx_type(json_str);
  senseSerial.print(json_str);
  senseSerial.print("\n");
  senseSerial.flush();
  Serial.printf("[PROTO] TX: %s\n", json_str);
}

// ── UART send functions ──────────────────────────────────────────────
static void uart_send_input_message(const char* type, int delta = 0, const char* id = NULL) {
  if (g_suppress_uart_json_tx) return;
  // Suppress non-essential input messages during OTA to prevent UART buffer
  // flooding on Sense side. Scroll/ping messages serve no purpose during OTA
  // and can overwhelm the 2KB ring buffer, causing OTA responses to be lost.
  if (ota_locked && type) {
    if (strcmp(type, "INPUT_SCROLL") == 0 ||
        strcmp(type, "INPUT_PING") == 0) {
      return;
    }
  }
  auto input_requires_sense = [](const char* msg_type) -> bool {
    if (!msg_type) return false;
    return strcmp(msg_type, "INPUT_WAKE") == 0 ||
           strcmp(msg_type, "INPUT_DELETE") == 0 ||
           strcmp(msg_type, "INPUT_LONG_PRESS_START") == 0 ||
           strcmp(msg_type, "INPUT_LONG_PRESS_END") == 0 ||
           strcmp(msg_type, "INPUT_MENU_SELECT") == 0 ||
           strcmp(msg_type, "INPUT_RETRY") == 0 ||
           strcmp(msg_type, "INPUT_RESET_WIFI") == 0 ||
           strcmp(msg_type, "INPUT_FW_INFO") == 0;
  };
  if (input_requires_sense(type)) {
    request_sense_wake(type);
  }
  // Serial-injected voice: set fire-and-forget so LCD doesn't show
  // Processing screen and wait for MQTT response that may never come.
  // Matches the behavior of touch-driven long press (LCD_Minimal.ino:3838).
  if (type && strcmp(type, "INPUT_LONG_PRESS_END") == 0) {
    g_voice_fire_and_forget_ignore_ui = true;
    waiting_for_voice_response = false;
    voice_response_deadline_ms = 0;
  }
  uart_note_input_type(type);
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = type;
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (delta != 0) {
    doc["delta"] = delta;
  }
  if (id != NULL && strlen(id) > 0) {
    doc["id"] = id;
  }

  String output;
  serializeJson(doc, output);
  senseSerial.print(output);
  senseSerial.print("\n");
  senseSerial.flush();
  Serial.printf("[PROTO] TX: %s\n", output.c_str());
}

static void uart_send_ack(const char* type) {
  if (!type || !type[0]) {
    return;
  }
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = type;
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.printf("[PROTO] TX: type=%s\n", type);
}

// LIST_ACTIVE: tell the Sense to stay awake + keep WiFi up while the user is
// on the shopping-list screen (state=1), or that the user has left and the
// Sense may sleep again (state=0). The Sense auto-clears the keep-awake flag
// after ~30s of silence, so this is re-asserted periodically while on the list.
// Plain UART send — safe to call from loop()/Core 0 (no LVGL).
static void uart_send_list_active(bool state) {
  if (g_suppress_uart_json_tx) return;
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "LIST_ACTIVE";
  doc["state"] = state ? 1 : 0;
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.printf("[LIST_ACTIVE] tx state=%d\n", state ? 1 : 0);
}

static void uart_send_maint_window_ack(uint32_t remaining_s,
                                       uint32_t wake_in_s,
                                       bool clear,
                                       const char* request_id,
                                       const char* status,
                                       bool persisted,
                                       uint64_t start_epoch,
                                       uint32_t duration_sec,
                                       uint32_t grace_before_sec,
                                       uint32_t grace_after_sec) {
  StaticJsonDocument<320> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "MAINT_WINDOW_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["remaining_s"] = remaining_s;
  if (request_id && request_id[0]) {
    doc["request_id"] = request_id;
  }
  if (status && status[0]) {
    doc["status"] = status;
  }
  doc["persisted"] = persisted ? true : false;
  if (start_epoch > 0) {
    doc["start_epoch"] = start_epoch;
    doc["duration_sec"] = duration_sec;
    doc["grace_before_sec"] = grace_before_sec;
    doc["grace_after_sec"] = grace_after_sec;
  }
  if (wake_in_s > 0) {
    doc["wake_in_s"] = wake_in_s;
  }
  if (clear) {
    doc["clear"] = true;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void uart_send_wifi_on_ack(const char* status, const char* reason) {
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "WIFI_ON_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (status && status[0]) {
    doc["status"] = status;
  }
  if (reason && reason[0]) {
    doc["reason"] = reason;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.printf("[PROTO] TX: type=WIFI_ON_ACK status=%s reason=%s\n",
                status ? status : "", reason ? reason : "");
}

static void uart_send_wifi_creds_ack(const char* status, int err_code) {
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "WIFI_CREDS_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["status"] = (status && status[0]) ? status : "ERR";
  doc["err_code"] = err_code;
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.printf("[PROTO] TX: type=WIFI_CREDS_ACK status=%s err_code=%d\n",
                doc["status"].as<const char*>(), err_code);
}

// ── TX message queue helpers ─────────────────────────────────────────
static const char* tx_msg_wake_reason(const tx_msg_t* tx_msg) {
  if (!tx_msg || !tx_msg->type[0]) {
    return "uart_tx";
  }
  if (strcmp(tx_msg->type, "INPUT_MENU_SELECT") == 0) {
    return "menu_select";
  }
  return tx_msg->type;
}

static bool tx_msg_requires_awake_proof(const tx_msg_t* tx_msg) {
  if (!tx_msg || !tx_msg->type[0]) {
    return false;
  }
  if (strcmp(tx_msg->type, "INPUT_MENU_SELECT") == 0) {
    return true;
  }
  return strcmp(tx_msg->type, "INPUT_FW_INFO") == 0;
}

static void tx_msg_send_now(const tx_msg_t* tx_msg) {
  if (!tx_msg || !tx_msg->type[0]) {
    return;
  }
  if (g_suppress_uart_json_tx) return;
  if (strcmp(tx_msg->type, "INPUT_MENU_SELECT") == 0) {
    request_sense_wake("menu_select");
    StaticJsonDocument<256> doc;
    doc["ver"] = PROTOCOL_VERSION;
    doc["type"] = "INPUT_MENU_SELECT";
    doc["msg_id"] = get_next_msg_id();
    doc["ts"] = millis();
    doc["menu_item"] = tx_msg->has_id ? tx_msg->id : "";
    doc["menu_index"] = tx_msg->has_delta ? tx_msg->delta : -1;
    String output;
    serializeJson(doc, output);
    senseSerial.println(output);
    senseSerial.flush();
    Serial.printf("[MENU] queued menu selection sent: %s\n", output.c_str());
    return;
  }
  if (tx_msg->has_delta) {
    uart_send_input_message(tx_msg->type, tx_msg->delta);
  } else if (tx_msg->has_id) {
    uart_send_input_message(tx_msg->type, 0, tx_msg->id);
  } else {
    uart_send_input_message(tx_msg->type);
  }
}

#endif // LCD_UART_H

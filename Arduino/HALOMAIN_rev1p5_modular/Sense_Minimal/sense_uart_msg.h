/*
 * sense_uart_msg.h
 *
 * UART message formatting and sending: sync, toast, voice response,
 * pong, heartbeat, sleep protocol messages, and release wake.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 13.
 *
 * Prerequisites (must be declared before #include "sense_uart_msg.h"):
 *   - ArduinoJson.h
 *   - PROTOCOL_VERSION, get_next_msg_id(), uart_send_json() from sense_uart.h
 *   - diag_note_stage() from sense_diag.h
 */

#ifndef SENSE_UART_MSG_H
#define SENSE_UART_MSG_H

// ── Sync ack ───────────────────────────────────────────────────────

static void uart_send_sync_ack() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "SYNC_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.println("[LNK] sync_ack_tx");
}

// ── UI toast ───────────────────────────────────────────────────────

static void uart_send_ui_toast(const char* message) {
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "UI_TOAST";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["text"] = message;
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

// ── UI voice response ──────────────────────────────────────────────

static void uart_send_ui_voice_response(const String& response_json) {
  const size_t kMaxVoiceJsonChars = 2400;
  const size_t kMaxVoiceLineChars = 2800;
  uint32_t msg_id = get_next_msg_id();
  uint32_t ts = millis();

  String transport_json = response_json;
  DynamicJsonDocument source(16384);
  DeserializationError parse_error = deserializeJson(source, response_json);
  if (!parse_error) {
    auto build_candidate = [&](bool include_ui,
                               bool include_quick_items,
                               bool include_transcript,
                               size_t max_text_len) -> String {
      DynamicJsonDocument compact(12288);
      const char* type = source["type"] | "";
      const char* transcript = source["transcript"] | "";
      String text = source["text"].is<const char*>() ? String(source["text"].as<const char*>()) : String("");
      if (max_text_len > 0 && text.length() > max_text_len) {
        text.remove(max_text_len);
        text += "...";
      }

      if (text.length() > 0) {
        compact["text"] = text;
      }
      if (type[0]) {
        compact["type"] = type;
      }
      if (include_transcript && transcript[0]) {
        compact["transcript"] = transcript;
      }
      if (!source["error"].isNull()) {
        compact["error"] = source["error"];
      }
      if (include_quick_items && source["quickItems"].is<JsonArray>()) {
        compact["quickItems"] = source["quickItems"];
      }
      if (include_ui && source["ui"].is<JsonObject>()) {
        JsonObject ui = source["ui"].as<JsonObject>();
        JsonArray screens = ui["screens"].as<JsonArray>();
        if (!ui.isNull() && !screens.isNull() && screens.size() > 0) {
          compact["ui"] = ui;
        }
      }

      String candidate;
      serializeJson(compact, candidate);
      return candidate;
    };

    const size_t text_len = source["text"].is<const char*>() ? strlen(source["text"].as<const char*>()) : 0;
    String candidates[] = {
      build_candidate(true,  true,  true,  0),
      build_candidate(false, true,  true,  0),
      build_candidate(false, false, true,  0),
      build_candidate(false, false, false, text_len > 1600 ? 1600 : 0),
      build_candidate(false, false, false, text_len > 1000 ? 1000 : 0),
      String("{\"text\":\"Sorry, I couldn't format that response.\",\"type\":\"error\"}")
    };

    transport_json = candidates[0];
    for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); ++i) {
      if (candidates[i].length() > 0 && candidates[i].length() <= kMaxVoiceJsonChars) {
        transport_json = candidates[i];
        break;
      }
    }

    if (transport_json.length() > kMaxVoiceJsonChars) {
      transport_json = String("{\"text\":\"Sorry, I couldn't format that response.\",\"type\":\"error\"}");
    }

    Serial.printf("[VOICE_UI] transport_json raw=%u compact=%u\n",
                  (unsigned)response_json.length(),
                  (unsigned)transport_json.length());
  } else {
    Serial.printf("[VOICE_UI] source_parse_failed error=%s\n", parse_error.c_str());
    if (transport_json.length() > kMaxVoiceJsonChars) {
      transport_json = String("{\"text\":\"Sorry, I couldn't format that response.\",\"type\":\"error\"}");
    }
  }

  String output;
  DynamicJsonDocument escaped_doc(transport_json.length() + 64);
  escaped_doc.set(transport_json);
  String escaped_json;
  serializeJson(escaped_doc.as<JsonVariant>(), escaped_json);

  output.reserve(escaped_json.length() + 96);
  output = "{\"ver\":";
  output += String(PROTOCOL_VERSION);
  output += ",\"type\":\"UI_VOICE_RESPONSE\",\"msg_id\":";
  output += String(msg_id);
  output += ",\"ts\":";
  output += String(ts);
  output += ",\"json\":";
  output += escaped_json;
  output += "}";

  if (output.length() > kMaxVoiceLineChars) {
    String fallback_json = "{\"text\":\"Sorry, I couldn't format that response.\",\"type\":\"error\"}";
    DynamicJsonDocument fallback_doc(fallback_json.length() + 64);
    fallback_doc.set(fallback_json);
    escaped_json = "";
    serializeJson(fallback_doc.as<JsonVariant>(), escaped_json);
    output = "{\"ver\":";
    output += String(PROTOCOL_VERSION);
    output += ",\"type\":\"UI_VOICE_RESPONSE\",\"msg_id\":";
    output += String(msg_id);
    output += ",\"ts\":";
    output += String(ts);
    output += ",\"json\":";
    output += escaped_json;
    output += "}";
    Serial.printf("[VOICE_UI] line_too_long compact=%u line=%u -> fallback\n",
                  (unsigned)transport_json.length(),
                  (unsigned)output.length());
  } else {
    Serial.printf("[VOICE_UI] line_len=%u compact=%u\n",
                  (unsigned)output.length(),
                  (unsigned)transport_json.length());
  }

  uart_send_json(output.c_str());
}

// ── Protocol messages ──────────────────────────────────────────────

static void uart_send_pong() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "PONG";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void uart_send_link_hb() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "LINK_HB";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

// ── Sleep protocol messages ────────────────────────────────────────

static void uart_send_sleep_ready() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "SLEEP_READY";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void uart_send_sleep_intent(const char* reason) {
  diag_note_stage("sleep_intent", 0);
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "SENSE_SLEEP_INTENT";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (reason && reason[0]) {
    doc["reason"] = reason;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void uart_send_sleep_deny(const char* reason, uint32_t retry_ms) {
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "SLEEP_DENY";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["reason"] = reason ? reason : "unknown";
  doc["retry_ms"] = retry_ms;
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void uart_send_release_wake() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "RELEASE_WAKE";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.println("[SLEEP_PROTO] tx RELEASE_WAKE");
}

#endif // SENSE_UART_MSG_H

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

// ── User-intent ack + duplicate suppression ────────────────────────
//
// The LCD holds every user-intent message (INPUT_MENU_SELECT and friends) until
// we ack its msg_id, retransmitting on a 400ms timer — see lcd_link_ack.h. That
// closes the hole where a capture request lost to a cold UART left the user on
// the capturing screen forever.
//
// The retransmit replays the ORIGINAL bytes, so the same msg_id can legitimately
// arrive more than once whenever an ACK is what got lost. Suppressing the repeat
// ACTION is therefore mandatory: without it, this fix would turn one tap into
// two captures. Note we still RE-ACK a duplicate — the LCD is retrying precisely
// because it never heard us, so staying silent would guarantee the next retry.

#ifndef SENSE_INPUT_SEEN_RING
#define SENSE_INPUT_SEEN_RING 8
#endif

static uint32_t g_input_seen_ids[SENSE_INPUT_SEEN_RING] = {0};
static uint8_t  g_input_seen_pos = 0;
static uint32_t g_input_dupes_suppressed = 0;

static bool sense_input_seen_recently(uint32_t msg_id) {
  if (msg_id == 0) return false;   // no id: cannot dedupe, treat as fresh
  for (uint8_t i = 0; i < SENSE_INPUT_SEEN_RING; i++) {
    if (g_input_seen_ids[i] == msg_id) return true;
  }
  return false;
}

static void sense_input_mark_seen(uint32_t msg_id) {
  if (msg_id == 0) return;
  g_input_seen_ids[g_input_seen_pos] = msg_id;
  g_input_seen_pos = (uint8_t)((g_input_seen_pos + 1) % SENSE_INPUT_SEEN_RING);
}

#if HALO_SPOOL_TEST
// Bench-only: swallow the next N acks to simulate a LOST ACK, which is the only
// path that makes the LCD retransmit a message the Sense already executed. That
// is precisely the case duplicate suppression exists for, and it cannot be
// reached by making the Sense deaf (then it never executes anything). Reuses
// the existing bench gate deliberately rather than adding another flag that
// would have to be remembered at ship time.
static uint32_t g_test_drop_acks = 0;
#endif

static void uart_send_input_ack(uint32_t ack_id) {
#if HALO_SPOOL_TEST
  if (g_test_drop_acks > 0) {
    g_test_drop_acks--;
    Serial.printf("[ACKTEST] dropping INPUT_ACK for ack_id=%lu (%lu left)\n",
                  (unsigned long)ack_id, (unsigned long)g_test_drop_acks);
    return;
  }
#endif
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["ack_id"] = ack_id;          // the LCD msg_id being acknowledged
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

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

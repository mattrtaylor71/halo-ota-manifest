#ifndef HALO_PROTOCOL_H
#define HALO_PROTOCOL_H

#include <Arduino.h>

// ============================================================================
// Halo Protocol - Shared between LCD and Sense firmwares
// ============================================================================
// Message format: newline-delimited JSON
// Fields: type, src, seq, optional ack
// ============================================================================

// Message source identifiers
#define HALO_SRC_LCD    "lcd"
#define HALO_SRC_SENSE  "sense"

// Protocol version
#define HALO_PROTOCOL_VERSION      1

// Message type constants
#define HALO_MSG_PING              "PING"
#define HALO_MSG_PONG              "PONG"
#define HALO_MSG_READY             "READY"
#define HALO_MSG_UI_STATUS         "UI_STATUS"
#define HALO_MSG_INPUT_SCROLL      "INPUT_SCROLL"
#define HALO_MSG_INPUT_SELECT      "INPUT_SELECT"
#define HALO_MSG_INPUT_LONG_PRESS_START  "INPUT_LONG_PRESS_START"
#define HALO_MSG_INPUT_LONG_PRESS_END    "INPUT_LONG_PRESS_END"
#define HALO_MSG_LIST_REQUEST     "LIST_REQUEST"
#define HALO_MSG_UI_STATE          "UI_STATE"
#define HALO_MSG_STATE_RESYNC_REQUEST  "STATE_RESYNC_REQUEST"
#define HALO_MSG_ERROR             "ERROR"
#define HALO_MSG_CAMERA_CAPTURE    "CAMERA_CAPTURE"
#define HALO_MSG_UI_PROGRESS       "UI_PROGRESS"
#define HALO_MSG_CAMERA_RESULT     "CAMERA_RESULT"
#define HALO_MSG_MEAL_ANALYSIS_RESULT  "MEAL_ANALYSIS_RESULT"
#define HALO_MSG_UI_ERROR          "UI_ERROR"
#define HALO_MSG_UI_TOAST          "UI_TOAST"
#define HALO_MSG_NET_STATUS        "NET_STATUS"

// Error codes
#define HALO_ERROR_UNKNOWN         "UNKNOWN"
#define HALO_ERROR_BUSY            "BUSY"
#define HALO_ERROR_INVALID         "INVALID"
#define HALO_ERROR_TIMEOUT         "TIMEOUT"
#define HALO_ERROR_PROTOCOL        "PROTOCOL"

// UI Status values
#define HALO_UI_STATUS_IDLE        "IDLE"
#define HALO_UI_STATUS_LISTENING   "LISTENING"
#define HALO_UI_STATUS_PROCESSING  "PROCESSING"

// Helper functions for JSON message construction
namespace HaloProtocol {
  // Build a JSON message string
  // Example: buildMessage("PING", "lcd", 1) -> {"type":"PING","src":"lcd","seq":1,"v":1}
  String buildMessage(const char* type, const char* src, uint32_t seq, bool ack = false) {
    String msg = "{";
    msg += "\"type\":\"" + String(type) + "\"";
    msg += ",\"src\":\"" + String(src) + "\"";
    msg += ",\"seq\":" + String(seq);
    msg += ",\"v\":" + String(HALO_PROTOCOL_VERSION);
    if (ack) {
      msg += ",\"ack\":true";
    }
    msg += "}";
    return msg;
  }

  // Build a message with additional data field
  // Example: buildMessageWithData("UI_STATUS", "sense", 2, "LISTENING")
  String buildMessageWithData(const char* type, const char* src, uint32_t seq, const char* dataKey, const char* dataValue, bool ack = false) {
    String msg = "{";
    msg += "\"type\":\"" + String(type) + "\"";
    msg += ",\"src\":\"" + String(src) + "\"";
    msg += ",\"seq\":" + String(seq);
    msg += ",\"v\":" + String(HALO_PROTOCOL_VERSION);
    msg += ",\"" + String(dataKey) + "\":\"" + String(dataValue) + "\"";
    if (ack) {
      msg += ",\"ack\":true";
    }
    msg += "}";
    return msg;
  }

  // Build a message with numeric data field
  String buildMessageWithInt(const char* type, const char* src, uint32_t seq, const char* dataKey, int32_t dataValue, bool ack = false) {
    String msg = "{";
    msg += "\"type\":\"" + String(type) + "\"";
    msg += ",\"src\":\"" + String(src) + "\"";
    msg += ",\"seq\":" + String(seq);
    msg += ",\"v\":" + String(HALO_PROTOCOL_VERSION);
    msg += ",\"" + String(dataKey) + "\":" + String(dataValue);
    if (ack) {
      msg += ",\"ack\":true";
    }
    msg += "}";
    return msg;
  }

  // Build error message
  String buildErrorMessage(const char* src, uint32_t seq, const char* errorCode, const char* message = "") {
    String msg = "{";
    msg += "\"type\":\"" + String(HALO_MSG_ERROR) + "\"";
    msg += ",\"src\":\"" + String(src) + "\"";
    msg += ",\"seq\":" + String(seq);
    msg += ",\"v\":" + String(HALO_PROTOCOL_VERSION);
    msg += ",\"error\":\"" + String(errorCode) + "\"";
    if (message && strlen(message) > 0) {
      msg += ",\"msg\":\"" + String(message) + "\"";
    }
    msg += "}";
    return msg;
  }

  // Parse message type from JSON string (simple extraction)
  String parseType(const String& json) {
    int typeIdx = json.indexOf("\"type\":\"");
    if (typeIdx < 0) return "";
    typeIdx += 8; // length of "type":"
    int endIdx = json.indexOf("\"", typeIdx);
    if (endIdx < 0) return "";
    return json.substring(typeIdx, endIdx);
  }

  // Parse src from JSON string
  String parseSrc(const String& json) {
    int srcIdx = json.indexOf("\"src\":\"");
    if (srcIdx < 0) return "";
    srcIdx += 7; // length of "src":"
    int endIdx = json.indexOf("\"", srcIdx);
    if (endIdx < 0) return "";
    return json.substring(srcIdx, endIdx);
  }

  // Parse seq from JSON string
  uint32_t parseSeq(const String& json) {
    int seqIdx = json.indexOf("\"seq\":");
    if (seqIdx < 0) return 0;
    seqIdx += 6; // length of "seq":
    int endIdx = json.indexOf(",", seqIdx);
    if (endIdx < 0) endIdx = json.indexOf("}", seqIdx);
    if (endIdx < 0) return 0;
    return json.substring(seqIdx, endIdx).toInt();
  }

  // Check if message has ack field
  bool hasAck(const String& json) {
    return json.indexOf("\"ack\":true") >= 0;
  }

  // Parse a string field value from JSON (e.g., "status":"LISTENING")
  String parseStringField(const String& json, const char* fieldName) {
    String searchStr = "\"" + String(fieldName) + "\":\"";
    int fieldIdx = json.indexOf(searchStr);
    if (fieldIdx < 0) return "";
    fieldIdx += searchStr.length();
    int endIdx = json.indexOf("\"", fieldIdx);
    if (endIdx < 0) return "";
    return json.substring(fieldIdx, endIdx);
  }

  // Parse an integer field value from JSON (e.g., "delta":1)
  int32_t parseIntField(const String& json, const char* fieldName) {
    String searchStr = "\"" + String(fieldName) + "\":";
    int fieldIdx = json.indexOf(searchStr);
    if (fieldIdx < 0) return 0;
    fieldIdx += searchStr.length();
    // Skip whitespace
    while (fieldIdx < json.length() && (json[fieldIdx] == ' ' || json[fieldIdx] == '\t')) {
      fieldIdx++;
    }
    int endIdx = fieldIdx;
    // Find end of number (comma, }, or whitespace)
    while (endIdx < json.length() && 
           json[endIdx] != ',' && json[endIdx] != '}' && 
           json[endIdx] != ' ' && json[endIdx] != '\t') {
      endIdx++;
    }
    if (endIdx <= fieldIdx) return 0;
    return json.substring(fieldIdx, endIdx).toInt();
  }

  // Build UI_STATE message with items array
  // items: array of {id, text} objects
  String buildUiStateMessage(const char* src, uint32_t seq, int32_t selected, 
                            const char* items[], const char* itemIds[], int itemCount) {
    String msg = "{";
    msg += "\"type\":\"" + String(HALO_MSG_UI_STATE) + "\"";
    msg += ",\"src\":\"" + String(src) + "\"";
    msg += ",\"seq\":" + String(seq);
    msg += ",\"v\":" + String(HALO_PROTOCOL_VERSION);
    msg += ",\"selected\":" + String(selected);
    msg += ",\"items\":[";
    for (int i = 0; i < itemCount; i++) {
      if (i > 0) msg += ",";
      msg += "{";
      msg += "\"id\":\"" + String(itemIds[i]) + "\"";
      msg += ",\"text\":\"" + String(items[i]) + "\"";
      msg += "}";
    }
    msg += "]}";
    return msg;
  }

  // Parse protocol version from JSON
  uint32_t parseVersion(const String& json) {
    int vIdx = json.indexOf("\"v\":");
    if (vIdx < 0) return 0; // No version field = old protocol
    vIdx += 4; // length of "v":
    int endIdx = json.indexOf(",", vIdx);
    if (endIdx < 0) endIdx = json.indexOf("}", vIdx);
    if (endIdx < 0) return 0;
    return json.substring(vIdx, endIdx).toInt();
  }

  // Parse error code from ERROR message
  String parseErrorCode(const String& json) {
    return parseStringField(json, "error");
  }
}

#endif // HALO_PROTOCOL_H


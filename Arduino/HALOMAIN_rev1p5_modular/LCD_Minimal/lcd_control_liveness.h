#pragma once

// These messages prove the coordinator is executing even while SNTP prevents
// a maintenance arm. Validate their fields before granting awake proof; merely
// seeing a type string must not cancel a PONG timeout or a prior SLEEP_READY.
static bool lcd_control_awake_proof(const JsonDocument& doc) {
  if (!doc["ver"].is<uint32_t>() || doc["ver"].as<uint32_t>() != PROTOCOL_VERSION ||
      !doc["type"].is<const char*>() || !doc["msg_id"].is<uint32_t>() ||
      !doc["msg_id"].as<uint32_t>() || !doc["ts"].is<uint32_t>()) return false;
  const char* type = doc["type"].as<const char*>();
  if (!strcmp(type, "FW_INFO")) {
    const char* version = doc["sense_fw"].is<const char*>() ? doc["sense_fw"].as<const char*>() : nullptr;
    return version && version[0] && strnlen(version, 64) < 64;
  }
  if (!strcmp(type, "LCD_OTA_QUERY")) {
    // The ordinary query has no body; coordinator queries add a bounded ID.
    if (!doc.containsKey("coord_id")) return true;
    const char* id = doc["coord_id"].is<const char*>() ? doc["coord_id"].as<const char*>() : nullptr;
    return id && id[0] && strnlen(id, 40) < 40;
  }
  if (!strcmp(type, "SENSE_DIAG")) {
    if (strcmp(doc["area"] | "", "wifi") || strcmp(doc["event"] | "", "rssi_report") ||
        !doc["code"].is<int32_t>() || !doc["detail"].is<const char*>()) return false;
    const char* detail = doc["detail"].as<const char*>();
    if (!detail[0] || strnlen(detail, 192) >= 192) return false;
    const char* label = doc["label"] | "";
    const int32_t code = doc["code"].as<int32_t>();
    return (!strcmp(label, "connected") && code >= -127 && code < 0) ||
           (!strcmp(label, "disconnected") && code >= 0 && code <= 255);
  }
  if (!strcmp(type, "MEDIA_RETRY_ARM")) {
    if (!doc["token"].is<const char*>() || !doc["wake_in_s"].is<uint32_t>() ||
        !halo_media_timer::token_valid(doc["token"].as<const char*>())) return false;
    const uint32_t seconds = doc["wake_in_s"].as<uint32_t>();
    return !seconds || (seconds >= halo_media_timer::kMinArmSeconds &&
                        seconds <= halo_media_timer::kMaxArmSeconds);
  }
  return false;
}

static void lcd_note_control_awake_proof(const JsonDocument& doc) {
  if (lcd_control_awake_proof(doc)) note_sense_binary_media_rx(doc["type"].as<const char*>());
}

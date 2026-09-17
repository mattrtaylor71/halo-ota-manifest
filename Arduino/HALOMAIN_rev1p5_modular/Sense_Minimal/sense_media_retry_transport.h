#pragma once

static char g_media_retry_arm_token[17] = {};
static uint32_t g_media_retry_arm_seconds = 0;
static bool g_media_retry_arm_acked = false;

static void media_retry_arm_ack(JsonDocument& doc) {
  if (g_media_retry_arm_token[0] && doc["token"].is<const char*>() &&
      !strcmp(doc["token"].as<const char*>(), g_media_retry_arm_token) &&
      doc["wake_in_s"].is<uint32_t>() &&
      doc["wake_in_s"].as<uint32_t>() == g_media_retry_arm_seconds &&
      doc["ok"].is<unsigned>() && doc["ok"].as<unsigned>() == 1)
    g_media_retry_arm_acked = true;
}

// Main-task only, before radio/task teardown. Ordinary receive dispatch remains
// live: a user gesture aborts sleep rather than being eaten by a private reader.
// This arm has no schedule identity and cannot create or spend OTA credit.
static bool media_retry_arm_lcd(uint32_t seconds, uint32_t user_generation) {
  g_media_retry_arm_acked = false;
  g_media_retry_arm_token[0] = 0;
  if (!sense_uart_ordinary_tx_allowed()) return false;
#ifdef HALO_SENSE_PROD_WRAPPER
  if (sense_lcd_query_busy() || halo_provisioning_active()) return false;
#endif
  snprintf(g_media_retry_arm_token, sizeof(g_media_retry_arm_token), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  g_media_retry_arm_seconds = seconds;
  g_media_retry_arm_acked = false;
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "MEDIA_RETRY_ARM";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["token"] = g_media_retry_arm_token;
  doc["wake_in_s"] = seconds;
  String line;
  serializeJson(doc, line);
  const uint32_t started = millis();
  uart_send_json(line.c_str());
  bool repeated = false;
  while (!g_media_retry_arm_acked && (uint32_t)(millis() - started) < 1800) {
    pump_uart_rx_once();
    if (sense_user_action_generation() != user_generation ||
        !sense_uart_ordinary_tx_allowed()) break;
    if (!repeated && (uint32_t)(millis() - started) >= 900) {
      // Same token means an idempotent re-ACK, never renewal of the LCD due time.
      uart_send_json(line.c_str());
      repeated = true;
    }
    delay(5);
  }
  g_media_retry_arm_token[0] = 0;
  Serial.printf("[MEDIA_RETRY] peer_arm seconds=%lu ack=%u\n",
                (unsigned long)seconds, g_media_retry_arm_acked ? 1 : 0);
  return g_media_retry_arm_acked;
}

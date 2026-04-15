/*
 * sense_mqtt.h
 *
 * MQTT connection management, subscription, and message handling
 * for Sense_Minimal.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 7.
 *
 * Prerequisites (must be declared before #include "sense_mqtt.h"):
 *   - PubSubClient.h, ArduinoJson.h, WiFiClientSecure.h, WiFi.h
 *   - awsEndpoint, RESULT_TOPIC_TEMPLATE (const char*)
 *   - mqtt_wait_deadline, mqtt_subscribed, current_scan_job_id,
 *     current_result_topic, subscribed_result_topic,
 *     waiting_for_mqtt_result, active_dish_job_id,
 *     current_result_local_job_id, current_result_mode[],
 *     DishTimingTrace, g_dish_timing
 *   - rootCA, deviceCert, privateKey (certificate strings)
 *   - wifiClient (WiFiClientSecure), mqttClient (PubSubClient)
 *   - load_runtime_device_id(), build_runtime_ota_topic()
 *   - uart_send_ui_meal_result() (forward declared)
 *   - clear_active_dish_job() (forward declared)
 *   - diag_record_action_event() from sense_diag.h
 */

#ifndef SENSE_MQTT_H
#define SENSE_MQTT_H

// Forward declarations for .ino functions called by on_mqtt_message
static void uart_send_ui_toast(const char* message);

// ── MQTT topic construction ─────────────────────────────────────────

static String build_result_topic(const char* user_id, const char* device_id, const char* job_id) {
  if (!user_id || !device_id || !job_id || job_id[0] == '\0') {
    return String();
  }
  char buf[192];
  int n = snprintf(buf, sizeof(buf), RESULT_TOPIC_TEMPLATE, user_id, device_id, job_id);
  if (n <= 0 || n >= (int)sizeof(buf)) {
    return String();
  }
  return String(buf);
}

// ── MQTT subscription management ────────────────────────────────────

static void mqtt_clear_result_subscription() {
  if (subscribed_result_topic.length() > 0 && mqttClient.connected()) {
    mqttClient.unsubscribe(subscribed_result_topic.c_str());
  }
  subscribed_result_topic = "";
  current_result_topic = "";
  mqtt_subscribed = false;
}

static void mqtt_reset_connection(const char* reason, bool clear_result_topic) {
  if (subscribed_result_topic.length() > 0 && mqttClient.connected()) {
    mqttClient.unsubscribe(subscribed_result_topic.c_str());
  }
  if (mqttClient.connected()) {
    Serial.printf("[MQTT] Resetting connection reason=%s\n", reason ? reason : "unknown");
    mqttClient.disconnect();
    delay(50);
  }
  wifiClient.stop();
  subscribed_result_topic = "";
  mqtt_subscribed = false;
  if (clear_result_topic) {
    current_result_topic = "";
  }
}

static void mqtt_subscribe_result_topic_if_needed() {
  if (!waiting_for_mqtt_result || current_result_topic.length() == 0 || !mqttClient.connected()) {
    return;
  }
  if (current_result_topic == subscribed_result_topic) {
    return;
  }
  if (subscribed_result_topic.length() > 0) {
    mqttClient.unsubscribe(subscribed_result_topic.c_str());
  }
  mqtt_subscribed = mqttClient.subscribe(current_result_topic.c_str());
  subscribed_result_topic = mqtt_subscribed ? current_result_topic : "";
  Serial.printf("[MQTT] Result subscribed=%d to %s\n",
                mqtt_subscribed,
                current_result_topic.c_str());
}

// ── MQTT message callback ───────────────────────────────────────────

static void on_mqtt_message(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message: topic=%s len=%u\n", topic, length);
  String message;
  message.reserve(length);
  for (unsigned i=0; i<length; i++) {
    message += (char)payload[i];
  }
  Serial.println("[MQTT] Payload: " + message);

  // Parse JSON
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, message);
  if (err) {
    Serial.printf("[MQTT] JSON parse error: %s\n", err.c_str());
    return;
  }

  // Check for OTA message first (doesn't require job_id matching)
  const char* type = doc["type"] | "";
  if (strcmp(type, "ota") == 0) {
    Serial.println("[MQTT] OTA message received - TODO: implement OTA");
    return;  // OTA handled, don't process as job result
  }

  // Must match current_scan_job_id while we're waiting for SCAN result
  const char* job_id = doc["job_id"] | "";
  const char* phase = doc["phase"] | "";
  const char* mode = doc["mode"] | "";
  if (!waiting_for_mqtt_result || current_scan_job_id.length() == 0 ||
      strcmp(job_id, current_scan_job_id.c_str()) != 0) {
    Serial.printf("[MQTT] Ignoring: waiting_for=%d, wanted=%s, got=%s\n",
                  waiting_for_mqtt_result, current_scan_job_id.c_str(), job_id);
    return;
  }

  // Fast dish payload is final; handle and stop waiting immediately.
  if (strcmp(mode, "dish") == 0 && strcmp(phase, "fast") == 0) {
    const char* summary = doc["summary"] | "";
    const char* dish_name = doc["dish_name"] | "";
    const char* meal_summary = NULL;
    if (summary != NULL && strlen(summary) > 0) {
      meal_summary = summary;
    } else if (dish_name != NULL && strlen(dish_name) > 0) {
      meal_summary = dish_name;
    }
    int kcal = doc["calories"] | 0;
    float protein_g = doc["protein_g"] | 0.0;
    float carbs_g = doc["carbs_g"] | 0.0;
    float fat_g = doc["fat_g"] | 0.0;
    float confidence = doc["confidence"] | 0.0;
    int health_score = 0;
    const char* recommendation = NULL;

    if (meal_summary != NULL && strlen(meal_summary) > 0) {
      Serial.printf("[MQTT] Sending meal result (fast): %d kcal, %.1fg protein, %.1fg carbs, %.1fg fat (mode: %s)\n",
                    kcal, protein_g, carbs_g, fat_g, current_result_mode);
      uart_send_ui_meal_result(kcal, meal_summary, health_score, recommendation, current_result_mode, protein_g, carbs_g, fat_g, confidence, current_result_local_job_id);
    } else if (doc.containsKey("message")) {
      const char* msg = doc["message"];
      if (msg != NULL && strlen(msg) > 0) {
        uart_send_ui_toast(msg);
      }
    }

    diag_record_action_event("result", mode, "ok", "mqtt_fast", 0);
    uint32_t result_now_ms = millis();
    Serial.printf("[TIMING][DISH] job_id=%lu result_fast total_ms=%lu post_upload_to_result_ms=%lu result_wait_loop_ms=%lu ui_wait_ms=%lu\n",
                  (unsigned long)g_dish_timing.sense_job_id,
                  (unsigned long)(g_dish_timing.upload_start_ms ? (result_now_ms - g_dish_timing.upload_start_ms) : 0),
                  (unsigned long)(g_dish_timing.put_end_ms ? (result_now_ms - g_dish_timing.put_end_ms) : 0),
                  (unsigned long)(g_dish_timing.result_wait_start_ms ? (result_now_ms - g_dish_timing.result_wait_start_ms) : 0),
                  (unsigned long)(g_dish_timing.ui_wait_start_ms ? (result_now_ms - g_dish_timing.ui_wait_start_ms) : 0));
    waiting_for_mqtt_result = false;
    mqtt_wait_deadline = 0;
    current_scan_job_id = "";
    mqtt_clear_result_subscription();
    clear_active_dish_job(current_result_local_job_id, "mqtt_fast_result");

    Serial.println("[MQTT] SCAN result processed (fast)");
    return;
  }

  // Process result: send UI_MEAL_RESULT to LCD
  if (doc.containsKey("result")) {
    JsonObject result = doc["result"];

    // Send structured meal result
    if (result.containsKey("meal_summary")) {
      int kcal = result["kcal"] | 0;
      const char* meal_summary = result["meal_summary"];
      int health_score = result["health_score"] | 0;

      // Get first recommendation if available
      const char* recommendation = NULL;
      if (result.containsKey("recommendations") && result["recommendations"].is<JsonArray>()) {
        JsonArray recs = result["recommendations"];
        if (recs.size() > 0 && recs[0].is<const char*>()) {
          recommendation = recs[0].as<const char*>();
        }
      }

      // Extract macros from result
      float protein_g = result["protein_g"] | 0.0;
      float carbs_g = result["carbs_g"] | 0.0;
      float fat_g = result["fat_g"] | 0.0;
      float confidence = result["confidence"] | 0.0;

      if (meal_summary != NULL && strlen(meal_summary) > 0) {
        Serial.printf("[MQTT] Sending meal result: %d kcal, %.1fg protein, %.1fg carbs, %.1fg fat, health_score=%d (mode: %s)\n",
                      kcal, protein_g, carbs_g, fat_g, health_score, current_result_mode);
        // Only send meal result for dish mode (discard mode doesn't need nutrition info)
        // But we still send it with mode so LCD can decide whether to display
        uart_send_ui_meal_result(kcal, meal_summary, health_score, recommendation, current_result_mode, protein_g, carbs_g, fat_g, confidence, current_result_local_job_id);
      }
    }
    // Fallback to "message" field if meal_summary not available
    else if (result.containsKey("message")) {
      const char* msg = result["message"];
      if (msg != NULL && strlen(msg) > 0) {
        uart_send_ui_toast(msg);
      }
    }
  } else {
    const char* summary = doc["summary"] | "";
    const char* dish_name = doc["dish_name"] | "";
    const char* meal_summary = NULL;
    if (summary != NULL && strlen(summary) > 0) {
      meal_summary = summary;
    } else if (dish_name != NULL && strlen(dish_name) > 0) {
      meal_summary = dish_name;
    }
    int kcal = doc["calories"] | 0;
    float protein_g = doc["protein_g"] | 0.0;
    float carbs_g = doc["carbs_g"] | 0.0;
    float fat_g = doc["fat_g"] | 0.0;
    float confidence = doc["confidence"] | 0.0;
    int health_score = 0;
    const char* recommendation = NULL;

    if (meal_summary != NULL && strlen(meal_summary) > 0) {
      Serial.printf("[MQTT] Sending meal result (flat): %d kcal, %.1fg protein, %.1fg carbs, %.1fg fat (mode: %s)\n",
                    kcal, protein_g, carbs_g, fat_g, current_result_mode);
      uart_send_ui_meal_result(kcal, meal_summary, health_score, recommendation, current_result_mode, protein_g, carbs_g, fat_g, confidence, current_result_local_job_id);
    } else if (doc.containsKey("message")) {
      const char* msg = doc["message"];
      if (msg != NULL && strlen(msg) > 0) {
        uart_send_ui_toast(msg);
      }
    }
  }

  diag_record_action_event("result", mode, "ok", "mqtt", 0);
  uint32_t result_now_ms = millis();
  Serial.printf("[TIMING][DISH] job_id=%lu result total_ms=%lu post_upload_to_result_ms=%lu result_wait_loop_ms=%lu ui_wait_ms=%lu\n",
                (unsigned long)g_dish_timing.sense_job_id,
                (unsigned long)(g_dish_timing.upload_start_ms ? (result_now_ms - g_dish_timing.upload_start_ms) : 0),
                (unsigned long)(g_dish_timing.put_end_ms ? (result_now_ms - g_dish_timing.put_end_ms) : 0),
                (unsigned long)(g_dish_timing.result_wait_start_ms ? (result_now_ms - g_dish_timing.result_wait_start_ms) : 0),
                (unsigned long)(g_dish_timing.ui_wait_start_ms ? (result_now_ms - g_dish_timing.ui_wait_start_ms) : 0));

  // Clear waiting state
  waiting_for_mqtt_result = false;
  mqtt_wait_deadline = 0;
  current_scan_job_id = "";
  mqtt_clear_result_subscription();
  clear_active_dish_job(current_result_local_job_id, "mqtt_result");

  Serial.println("[MQTT] SCAN result processed");
}

// ── MQTT connection ─────────────────────────────────────────────────

static bool connect_to_mqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MQTT] WiFi not connected for MQTT");
    return false;
  }

  wifiClient.stop();
  wifiClient.setCACert(rootCA);
  wifiClient.setCertificate(deviceCert);
  wifiClient.setPrivateKey(privateKey);
  wifiClient.setTimeout(15000);

  mqttClient.setServer(awsEndpoint, 8883);
  mqttClient.setBufferSize(12000);
  mqttClient.setKeepAlive(30);
  mqttClient.setCallback(on_mqtt_message);

  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  const String clientId = "ESP32Camera_" + String(device_id);

  if (!mqttClient.connect(clientId.c_str())) {
    Serial.printf("[MQTT] Connection failed, state: %d\n", mqttClient.state());
    return false;
  }

  Serial.println("[MQTT] Connected");
  return true;
}

static void mqtt_ensure_connected() {
  if (mqttClient.connected()) {
    mqtt_subscribe_result_topic_if_needed();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MQTT] Connect skipped (wifi_disconnected)");
    return;
  }

  const unsigned long connect_timeout_ms = waiting_for_mqtt_result ? 15000UL : 8000UL;
  const unsigned long connect_retry_delay_ms = 250;
  const unsigned long io_timeout_ms = 5000;

  wifiClient.stop();

  // Set creds and connection params
  wifiClient.setCACert(rootCA);
  wifiClient.setCertificate(deviceCert);
  wifiClient.setPrivateKey(privateKey);
  wifiClient.setTimeout(io_timeout_ms);
#if defined(ARDUINO_ARCH_ESP32)
  wifiClient.setHandshakeTimeout(io_timeout_ms);
#endif

  mqttClient.setServer(awsEndpoint, 8883);
  mqttClient.setBufferSize(12000);
  mqttClient.setKeepAlive(30);
  mqttClient.setCallback(on_mqtt_message);

  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  String clientId = "ESP32Camera_" + String(device_id);
  unsigned long start_ms = millis();
  unsigned int attempt = 0;
  while (!mqttClient.connected() &&
         (millis() - start_ms) < connect_timeout_ms) {
    attempt++;
    wifiClient.stop();
    if (mqttClient.connect(clientId.c_str())) {
      break;
    }
    Serial.printf("[MQTT] Reconnect failed attempt=%u state=%d\n",
                  attempt, mqttClient.state());
    if ((attempt % 4) == 0) {
      mqtt_reset_connection("reconnect_retry", false);
    } else {
      wifiClient.stop();
    }
    delay(connect_retry_delay_ms);
  }

  if (mqttClient.connected()) {
    mqtt_subscribe_result_topic_if_needed();
    String ota_topic = build_runtime_ota_topic();
    bool ota_subscribed = mqttClient.subscribe(ota_topic.c_str());
    Serial.printf("[MQTT] Connected, ota_subscribed=%d to %s\n", ota_subscribed, ota_topic.c_str());
  } else {
    Serial.printf("[MQTT] Reconnect timeout after %lu ms\n",
                  (unsigned long)(millis() - start_ms));
  }
}

#endif // SENSE_MQTT_H

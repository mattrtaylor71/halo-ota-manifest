#include "MqttClient.h"

#include "ProvisioningState.h"
#include "Truth.h"
#include "Watchdog.h"
#include "OtaIntent.h"
#include "Log.h"

#include <WiFi.h>
#include <esp_system.h>
#include <Preferences.h>
#include <time.h>
#if __has_include(<esp_crt_bundle.h>)
#include <esp_crt_bundle.h>
#define MQTT_HAS_CERT_BUNDLE 1
#else
#define MQTT_HAS_CERT_BUNDLE 0
#endif
#include <mqtt_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <ArduinoJson.h>

__attribute__((weak)) void ota_schedule_update_from_mqtt(bool enable,
                                                         int window_start_min,
                                                         int window_dur_min,
                                                         int jitter_min,
                                                         int min_idle_min) {}

namespace {
  static const char* kLogTag = "MQTT";

  struct BrokerProfile {
    const char* uri;
    uint32_t port;
    const char* label;
    const char* alpn;
  };

  static const BrokerProfile kPrimaryBroker = {
    MQTT_BROKER_URI,
    MQTT_BROKER_PORT,
    "primary",
    nullptr
  };

  static const BrokerProfile kAltBroker = {
    MQTT_ALT_BROKER_URI,
    MQTT_ALT_BROKER_PORT,
    "alt",
    MQTT_ALT_ALPN
  };

  static const char* s_alpn_list[2] = {nullptr, nullptr};

  static esp_mqtt_client_handle_t s_client = nullptr;
  static bool s_connected = false;
  static bool s_started = false;
  static bool s_need_stop = false;
  static bool s_allowed = true;
  static bool s_task_started = false;

  static uint32_t s_last_connect_ms = 0;
  static uint32_t s_last_pub_ms = 0;
  static uint32_t s_last_rx_ms = 0;
  static uint32_t s_fail_count = 0;
  static uint32_t s_fail_streak = 0;
  static uint32_t s_pub_inflight = 0;

  static uint32_t s_backoff_ms = 1000;
  static uint32_t s_next_attempt_ms = 0;
  static const BrokerProfile* s_active_broker = &kPrimaryBroker;
  static bool s_reinit_pending = false;
  static const BrokerProfile* s_reinit_target = nullptr;

  static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data);

  static QueueHandle_t s_cmd_queue = nullptr;
  static const uint32_t kQueueDepth = 6;

  static const char* kCmdOta = "ota_check";
  static const char* kCmdReboot = "reboot";
  static const char* kCmdPing = "ping";
  static const char* kCmdOtaForceNow = "ota/force_now";
  static const char* kCmdOtaStatusGet = "ota/status/get";

  static const char* kDesiredNvsNamespace = "mqtt_desired";
  static const char* kDesiredKeyLastId = "last_req_id";
  static const char* kDesiredKeyLastTs = "last_ts";
  static const uint32_t kDesiredMaxAgeS = 24 * 60 * 60;
  static const time_t kDesiredMinValidEpoch = 1609459200;
  static bool s_desired_state_loaded = false;
  static char s_last_desired_id[64] = {0};
  static uint32_t s_last_desired_ts = 0;

  static char s_topic_prefix[160] = {0};
  static char s_fleet_prefix[160] = {0};
  static bool s_prefix_ready = false;
  static bool s_subscribe_needed = false;
  static char s_truth_payload[2048] = {0};
  static char s_diag_payload[2048] = {0};

  static void build_topic(char* out, size_t out_len, const char* suffix) {
    if (!out || out_len == 0) {
      return;
    }
    if (!suffix) {
      suffix = "";
    }
    const char* base = (s_prefix_ready && s_topic_prefix[0]) ? s_topic_prefix : MQTT_TOPIC_PREFIX;
    snprintf(out, out_len, "%s/%s", base, suffix);
  }

  static void build_fleet_topic(char* out, size_t out_len, const char* suffix) {
    if (!out || out_len == 0) {
      return;
    }
    if (!suffix) {
      suffix = "";
    }
    const char* base = (s_prefix_ready && s_fleet_prefix[0]) ? s_fleet_prefix : "halo/fleet";
    snprintf(out, out_len, "%s/%s", base, suffix);
  }

  static void build_client_id(char* out, size_t out_len) {
    if (!out || out_len == 0) {
      return;
    }
    uint64_t mac = ESP.getEfuseMac();
    uint32_t lo = static_cast<uint32_t>(mac & 0xFFFFFFFFULL);
    uint32_t hi = static_cast<uint32_t>((mac >> 32) & 0xFFFFFFFFULL);
    snprintf(out, out_len, "halo-sense-%08lx%08lx",
             static_cast<unsigned long>(hi),
             static_cast<unsigned long>(lo));
  }

  static void enqueue_command(MqttCommandType type, const char* payload) {
    if (!s_cmd_queue) {
      return;
    }
    MqttCommand cmd = {};
    cmd.type = type;
    if (payload) {
      strncpy(cmd.payload, payload, sizeof(cmd.payload) - 1);
      cmd.payload[sizeof(cmd.payload) - 1] = '\0';
    } else {
      cmd.payload[0] = '\0';
    }
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
      s_fail_count++;
    }
  }

  static void desired_state_load() {
    if (s_desired_state_loaded) {
      return;
    }
    Preferences prefs;
    if (prefs.begin(kDesiredNvsNamespace, true)) {
      prefs.getString(kDesiredKeyLastId, s_last_desired_id, sizeof(s_last_desired_id));
      s_last_desired_ts = prefs.getUInt(kDesiredKeyLastTs, 0);
      prefs.end();
    }
    s_desired_state_loaded = true;
  }

  static void desired_state_store(const char* req_id, uint32_t ts) {
    if (req_id && req_id[0]) {
      strncpy(s_last_desired_id, req_id, sizeof(s_last_desired_id) - 1);
      s_last_desired_id[sizeof(s_last_desired_id) - 1] = '\0';
    }
    if (ts > 0) {
      s_last_desired_ts = ts;
    }
    Preferences prefs;
    if (prefs.begin(kDesiredNvsNamespace, false)) {
      if (s_last_desired_id[0]) {
        prefs.putString(kDesiredKeyLastId, s_last_desired_id);
      } else {
        prefs.remove(kDesiredKeyLastId);
      }
      prefs.putUInt(kDesiredKeyLastTs, s_last_desired_ts);
      prefs.end();
    }
  }

  static void handle_cmd_message(const char* topic, const char* payload) {
    if (!topic || !payload) {
      return;
    }
    char cmd_topic[128];
    build_topic(cmd_topic, sizeof(cmd_topic), "cmd");
    char fleet_topic[128];
    build_fleet_topic(fleet_topic, sizeof(fleet_topic), "cmd");
    if (strcmp(topic, cmd_topic) != 0) {
      if (strcmp(topic, fleet_topic) != 0) {
        return;
      }
    }

    if (strncmp(payload, kCmdOta, strlen(kCmdOta)) == 0) {
      enqueue_command(MQTT_CMD_OTA_CHECK, payload);
    } else if (strncmp(payload, kCmdReboot, strlen(kCmdReboot)) == 0) {
      enqueue_command(MQTT_CMD_REBOOT, payload);
    } else if (strncmp(payload, kCmdPing, strlen(kCmdPing)) == 0) {
      enqueue_command(MQTT_CMD_PING, payload);
    } else if (strncmp(payload, kCmdOtaForceNow, strlen(kCmdOtaForceNow)) == 0) {
      enqueue_command(MQTT_CMD_OTA_FORCE_NOW, payload);
    } else if (strncmp(payload, kCmdOtaStatusGet, strlen(kCmdOtaStatusGet)) == 0) {
      enqueue_command(MQTT_CMD_OTA_STATUS_GET, payload);
    }
  }

  static void handle_desired_message(const char* topic, const char* payload, bool retained) {
    if (!topic || !payload) {
      return;
    }
    char desired_topic[128];
    build_topic(desired_topic, sizeof(desired_topic), "desired");
    if (strcmp(topic, desired_topic) != 0) {
      return;
    }

    DynamicJsonDocument doc(768);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      LOG_WARN("[MQTT_DESIRED] parse error: %s", err.c_str());
      return;
    }
    if (!doc.is<JsonObject>()) {
      LOG_WARN("[MQTT_DESIRED] payload is not object");
      return;
    }

    const char* sense_ver = doc["sense"] | "";
    const char* lcd_ver = doc["lcd"] | "";
    bool force = doc["force"] | false;
    bool allow_downgrade = doc["allow_downgrade"] | false;
    if (!allow_downgrade) {
      allow_downgrade = doc["desired_allow_downgrade"] | false;
    }
    const char* reason = doc["reason"] | "";
    const char* request_id = doc["desired_request_id"] | "";
    if (!request_id || !request_id[0]) {
      request_id = doc["request_id"] | "";
    }
    uint32_t ts = doc["desired_ts"] | 0;
    if (ts == 0) {
      ts = doc["ts"] | 0;
    }

    desired_state_load();
    const bool has_req_id = (request_id && request_id[0]);
    const bool has_ts = (ts > 0);
    time_t now_s = time(nullptr);
    bool time_valid = (now_s >= kDesiredMinValidEpoch);
    int32_t age_s = -1;
    if (has_ts && time_valid && now_s >= static_cast<time_t>(ts)) {
      age_s = static_cast<int32_t>(now_s - ts);
    }
    LOG_INFO("[MQTT_DESIRED] ts=%lu now=%ld age_s=%ld time_valid=%d retained=%d",
             static_cast<unsigned long>(ts),
             static_cast<long>(now_s),
             static_cast<long>(age_s),
             time_valid ? 1 : 0,
             retained ? 1 : 0);

    if (!has_req_id && !has_ts) {
      LOG_WARN("[MQTT_DESIRED] missing_request_id_or_ts retained=%d", retained ? 1 : 0);
      return;
    }
    if (retained && !has_req_id) {
      LOG_INFO("[MQTT_DESIRED] retained_ignored retained=1");
      return;
    }
    if (has_req_id && s_last_desired_id[0] && strcmp(request_id, s_last_desired_id) == 0) {
      LOG_INFO("[MQTT_DESIRED] duplicate_ignored request_id=%s retained=%d",
               request_id,
               retained ? 1 : 0);
      return;
    }
    if (has_ts) {
      if (!has_req_id && s_last_desired_ts > 0 && ts <= s_last_desired_ts) {
        LOG_INFO("[MQTT_DESIRED] duplicate_ignored ts=%lu retained=%d",
                 static_cast<unsigned long>(ts),
                 retained ? 1 : 0);
        return;
      }
      if (!time_valid && !has_req_id) {
        LOG_WARN("[MQTT_DESIRED] stale_ignored age_s=%ld time_valid=0 retained=%d",
                 static_cast<long>(age_s),
                 retained ? 1 : 0);
        return;
      }
      if (time_valid && age_s >= 0 && static_cast<uint32_t>(age_s) > kDesiredMaxAgeS) {
        LOG_WARN("[MQTT_DESIRED] stale_ignored age_s=%ld retained=%d",
                 static_cast<long>(age_s),
                 retained ? 1 : 0);
        return;
      }
    }

    LOG_INFO("[MQTT_DESIRED] accepted request_id=%s ts=%lu retained=%d",
             has_req_id ? request_id : "-",
             static_cast<unsigned long>(ts),
             retained ? 1 : 0);
    desired_state_store(has_req_id ? request_id : nullptr, ts);
    OtaIntent::updateDesired(sense_ver, lcd_ver, force, allow_downgrade, ts, reason);

    bool sched_enable = doc["ota_sched_enable"] | false;
    int sched_start_min = doc["ota_window_start_min"] | 120;
    int sched_dur_min = doc["ota_window_min"] | (doc["ota_window_dur_min"] | 30);
    int sched_jitter_min = doc["ota_jitter_min"] | 10;
    int sched_min_idle_min = doc["ota_min_idle_min"] | 10;
    ota_schedule_update_from_mqtt(sched_enable,
                                  sched_start_min,
                                  sched_dur_min,
                                  sched_jitter_min,
                                  sched_min_idle_min);
  }

  static void mqtt_destroy_client() {
    if (s_client) {
      esp_mqtt_client_stop(s_client);
      esp_mqtt_client_destroy(s_client);
      s_client = nullptr;
    }
    s_started = false;
    s_connected = false;
  }

  static void mqtt_init_client(const BrokerProfile* profile) {
    if (!profile) {
      profile = &kPrimaryBroker;
    }
    s_active_broker = profile;

    char client_id[48];
    build_client_id(client_id, sizeof(client_id));

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s_active_broker->uri;
    cfg.broker.address.port = s_active_broker->port;
    cfg.credentials.username = MQTT_USERNAME;
    cfg.credentials.authentication.password = MQTT_PASSWORD;
    cfg.credentials.client_id = client_id;
    cfg.session.keepalive = MQTT_KEEPALIVE_SEC;
    cfg.network.disable_auto_reconnect = true;

    if (MQTT_ROOT_CA[0]) {
      cfg.broker.verification.certificate = MQTT_ROOT_CA;
    } else if (MQTT_USE_CERT_BUNDLE && MQTT_HAS_CERT_BUNDLE) {
      cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (MQTT_SKIP_CN_CHECK) {
      cfg.broker.verification.skip_cert_common_name_check = true;
    }
    if (MQTT_CLIENT_CERT[0] && MQTT_CLIENT_KEY[0]) {
      cfg.credentials.authentication.certificate = MQTT_CLIENT_CERT;
      cfg.credentials.authentication.key = MQTT_CLIENT_KEY;
    }
    if (s_active_broker->alpn && s_active_broker->alpn[0]) {
      s_alpn_list[0] = s_active_broker->alpn;
      s_alpn_list[1] = nullptr;
      cfg.broker.verification.alpn_protos = s_alpn_list;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client) {
      esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
    }
  }

  static void mqtt_schedule_reinit(const BrokerProfile* profile, const char* reason) {
    s_reinit_pending = true;
    s_reinit_target = profile ? profile : &kPrimaryBroker;
    s_need_stop = true;
    LOG_INFO("[MQTT] schedule reinit broker=%s reason=%s",
             s_reinit_target->label,
             reason ? reason : "unknown");
  }

  static void mqtt_record_failure(const char* reason) {
    s_fail_streak++;
    if (MQTT_FAILOVER_THRESHOLD <= 0) {
      return;
    }
    if (s_active_broker != &kPrimaryBroker) {
      return;
    }
    if (s_fail_streak < static_cast<uint32_t>(MQTT_FAILOVER_THRESHOLD)) {
      return;
    }
    LOG_INFO("[MQTT] failover to alt broker reason=%s", reason ? reason : "unknown");
    mqtt_schedule_reinit(&kAltBroker, reason);
  }

  static void mqtt_apply_backoff() {
    if (s_backoff_ms < 60000) {
      s_backoff_ms = s_backoff_ms * 2;
      if (s_backoff_ms > 60000) {
        s_backoff_ms = 60000;
      }
    }
    s_next_attempt_ms = millis() + s_backoff_ms;
  }

  static esp_err_t mqtt_handle_event(esp_mqtt_event_handle_t event) {
    if (!event) {
      return ESP_FAIL;
    }
    switch (event->event_id) {
      case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        s_started = true;
        s_last_connect_ms = millis();
        s_backoff_ms = 1000;
        s_next_attempt_ms = 0;
        s_fail_streak = 0;
        LOG_INFO("[MQTT] connected broker=%s port=%lu alpn=%s",
                 s_active_broker ? s_active_broker->label : "unknown",
                 s_active_broker ? static_cast<unsigned long>(s_active_broker->port) : 0UL,
                 (s_active_broker && s_active_broker->alpn) ? s_active_broker->alpn : "(none)");
        char cmd_topic[128];
        build_topic(cmd_topic, sizeof(cmd_topic), "cmd");
        esp_mqtt_client_subscribe(event->client, cmd_topic, 0);
        char desired_topic[128];
        build_topic(desired_topic, sizeof(desired_topic), "desired");
        esp_mqtt_client_subscribe(event->client, desired_topic, 0);
#if MQTT_FLEET_ENABLED
        char fleet_topic[128];
        build_fleet_topic(fleet_topic, sizeof(fleet_topic), "cmd");
        esp_mqtt_client_subscribe(event->client, fleet_topic, 0);
#endif
        break;
      }
      case MQTT_EVENT_DISCONNECTED:
      case MQTT_EVENT_ERROR: {
        s_connected = false;
        s_fail_count++;
        s_need_stop = true;
        s_pub_inflight = 0;
        mqtt_apply_backoff();
        mqtt_record_failure(event->event_id == MQTT_EVENT_ERROR ? "event_error" : "event_disconnected");
        break;
      }
      case MQTT_EVENT_PUBLISHED: {
        if (s_pub_inflight > 0) {
          s_pub_inflight--;
        }
        LOG_INFO("[MQTT] published msg_id=%d", event->msg_id);
        break;
      }
      case MQTT_EVENT_DATA: {
        s_last_rx_ms = millis();
        if (event->data_len <= 0 || event->total_data_len != event->data_len) {
          break;
        }
        if (!event->topic || !event->data) {
          break;
        }
        char topic[128];
        char payload[128];
        int tlen = event->topic_len < static_cast<int>(sizeof(topic) - 1)
                   ? event->topic_len : static_cast<int>(sizeof(topic) - 1);
        int dlen = event->data_len < static_cast<int>(sizeof(payload) - 1)
                   ? event->data_len : static_cast<int>(sizeof(payload) - 1);
        memcpy(topic, event->topic, tlen);
        topic[tlen] = '\0';
        memcpy(payload, event->data, dlen);
        payload[dlen] = '\0';
        handle_desired_message(topic, payload, event->retain != 0);
        handle_cmd_message(topic, payload);
        break;
      }
      default:
        break;
    }
    return ESP_OK;
  }

  static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    (void)handler_args;
    (void)base;
    (void)event_id;
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
    (void)mqtt_handle_event(event);
  }

  static bool mqtt_ready_for_connect() {
    if (!s_allowed) {
      return false;
    }
    if (!ProvisioningState::isProvisioned()) {
      return false;
    }
    time_t now_s = time(nullptr);
    if (now_s <= 1700000000) {
      return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
      return false;
    }
    IPAddress ip = WiFi.localIP();
    if (ip == IPAddress(0, 0, 0, 0)) {
      return false;
    }
    return true;
  }

  static void update_prefix() {
    char device_id[32];
    ProvisioningState::getDeviceId(device_id, sizeof(device_id));
    char owner_id[64] = {0};
    bool owner_ok = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));

    char new_prefix[160];
    if (MQTT_USE_OWNER_PREFIX && owner_ok && owner_id[0]) {
      snprintf(new_prefix, sizeof(new_prefix), "%s/%s/%s", MQTT_TOPIC_PREFIX, owner_id, device_id);
    } else {
      snprintf(new_prefix, sizeof(new_prefix), "%s/%s", MQTT_TOPIC_PREFIX, device_id);
    }

    char new_fleet[160];
    if (MQTT_USE_OWNER_PREFIX && owner_ok && owner_id[0]) {
      snprintf(new_fleet, sizeof(new_fleet), "%s/%s/fleet", MQTT_TOPIC_PREFIX, owner_id);
    } else {
      snprintf(new_fleet, sizeof(new_fleet), "%s/fleet", MQTT_TOPIC_PREFIX);
    }

    if (strcmp(new_prefix, s_topic_prefix) != 0 || strcmp(new_fleet, s_fleet_prefix) != 0) {
      strncpy(s_topic_prefix, new_prefix, sizeof(s_topic_prefix) - 1);
      s_topic_prefix[sizeof(s_topic_prefix) - 1] = '\0';
      strncpy(s_fleet_prefix, new_fleet, sizeof(s_fleet_prefix) - 1);
      s_fleet_prefix[sizeof(s_fleet_prefix) - 1] = '\0';
      s_prefix_ready = true;
      s_subscribe_needed = true;
    }
  }

  static void mqtt_task(void* param) {
    (void)param;
    for (;;) {
      update_prefix();
      if (s_reinit_pending) {
        if (s_client) {
          esp_mqtt_client_stop(s_client);
        }
        mqtt_destroy_client();
        s_active_broker = s_reinit_target ? s_reinit_target : &kPrimaryBroker;
        s_reinit_target = nullptr;
        s_reinit_pending = false;
        s_need_stop = false;
        s_fail_streak = 0;
        mqtt_init_client(s_active_broker);
      }
      bool can_connect = mqtt_ready_for_connect();
      uint32_t now = millis();

      if (!can_connect) {
        if (s_started && s_client) {
          esp_mqtt_client_stop(s_client);
          s_started = false;
          s_connected = false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        Watchdog::kick("mqtt");
        continue;
      }

      if (s_need_stop) {
        if (s_client) {
          esp_mqtt_client_stop(s_client);
        }
        s_started = false;
        s_connected = false;
        s_need_stop = false;
      }

      if (!s_started && now >= s_next_attempt_ms && s_client) {
        esp_err_t st = esp_mqtt_client_start(s_client);
        if (st != ESP_OK) {
          s_fail_count++;
          mqtt_apply_backoff();
          mqtt_record_failure("start_failed");
        } else {
          s_started = true;
          LOG_INFO("[MQTT] start broker=%s port=%lu alpn=%s",
                   s_active_broker ? s_active_broker->label : "unknown",
                   s_active_broker ? static_cast<unsigned long>(s_active_broker->port) : 0UL,
                   (s_active_broker && s_active_broker->alpn) ? s_active_broker->alpn : "(none)");
        }
      }

      if (s_connected && s_subscribe_needed) {
        char cmd_topic[128];
        build_topic(cmd_topic, sizeof(cmd_topic), "cmd");
        esp_mqtt_client_subscribe(s_client, cmd_topic, 0);
        char desired_topic[128];
        build_topic(desired_topic, sizeof(desired_topic), "desired");
        esp_mqtt_client_subscribe(s_client, desired_topic, 0);
#if MQTT_FLEET_ENABLED
        char fleet_topic[128];
        build_fleet_topic(fleet_topic, sizeof(fleet_topic), "cmd");
        esp_mqtt_client_subscribe(s_client, fleet_topic, 0);
#endif
        s_subscribe_needed = false;
      }

      Watchdog::kick("mqtt");
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }
}  // namespace

void mqtt_init() {
  if (!s_cmd_queue) {
    s_cmd_queue = xQueueCreate(kQueueDepth, sizeof(MqttCommand));
  }
  if (!s_client) {
    mqtt_init_client(s_active_broker);
  }
  if (!s_task_started) {
    xTaskCreatePinnedToCore(mqtt_task, "mqtt_task", 8192, nullptr, 4, nullptr, 1);
    s_task_started = true;
  }
}

void mqtt_publish_truth(const char* reason) {
  if (!s_client || !s_connected) {
    return;
  }
  char topic[128];
  build_topic(topic, sizeof(topic), "truth");

  s_truth_payload[0] = '\0';
  if (!build_truth_json(reason, s_truth_payload, sizeof(s_truth_payload))) {
    LOG_WARN("[MQTT] truth_build_failed reason=%s", reason ? reason : "(null)");
    return;
  }
  const int qos = 1;
  const int retain = 0;
  int msg_id = esp_mqtt_client_publish(s_client, topic, s_truth_payload, 0, qos, retain);
  if (msg_id >= 0) {
    s_last_pub_ms = millis();
    if (qos > 0) {
      s_pub_inflight++;
    }
    LOG_INFO("[MQTT] truth_publish ok topic=%s msg_id=%d qos=%d retain=%d",
             topic, msg_id, qos, retain);
  } else {
    s_fail_count++;
    LOG_INFO("[MQTT] truth_publish failed topic=%s", topic);
  }
}

void mqtt_publish_diag(const char* reason) {
  s_diag_payload[0] = '\0';
  if (!build_diag_json(reason, s_diag_payload, sizeof(s_diag_payload))) {
    LOG_WARN("[MQTT] diag_build_failed reason=%s", reason ? reason : "(null)");
    return;
  }
  (void)mqtt_publish_diag_payload(s_diag_payload);
}

bool mqtt_publish_diag_payload(const char* payload) {
  if (!s_client || !s_connected || !payload || !payload[0]) {
    return false;
  }
  char topic[128];
  build_topic(topic, sizeof(topic), "diag");

  const int qos = 1;
  const int retain = 0;
  int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
  if (msg_id >= 0) {
    s_last_pub_ms = millis();
    if (qos > 0) {
      s_pub_inflight++;
    }
    LOG_INFO("[MQTT] diag_publish ok topic=%s msg_id=%d qos=%d retain=%d",
             topic, msg_id, qos, retain);
    return true;
  }
  s_fail_count++;
  LOG_INFO("[MQTT] diag_publish failed topic=%s", topic);
  return false;
}

void mqtt_publish_pong(const char* reason) {
  if (!s_client || !s_connected) {
    return;
  }
  char topic[128];
  build_topic(topic, sizeof(topic), "event");
  unsigned long uptime = millis();
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"event\":\"pong\",\"reason\":\"%s\",\"uptime_ms\":%lu}",
           reason ? reason : "", uptime);
  int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
  if (msg_id >= 0) {
    s_last_pub_ms = millis();
    // QoS0 publish: do not track inflight for sleep gating.
  } else {
    s_fail_count++;
  }
}

bool mqtt_get_next_command(MqttCommand* out) {
  if (!out || !s_cmd_queue) {
    return false;
  }
  if (xQueueReceive(s_cmd_queue, out, 0) == pdTRUE) {
    return true;
  }
  return false;
}

void mqtt_get_metrics(MqttMetrics* out) {
  if (!out) {
    return;
  }
  out->connected = s_connected;
  out->last_connect_ms = s_last_connect_ms;
  out->last_pub_ms = s_last_pub_ms;
  out->last_rx_ms = s_last_rx_ms;
  out->fail_count = s_fail_count;
  out->pub_inflight = s_pub_inflight;
  out->cmd_queue_depth = s_cmd_queue ? uxQueueMessagesWaiting(s_cmd_queue) : 0;
}

const char* mqtt_get_topic_prefix() {
  if (s_prefix_ready && s_topic_prefix[0]) {
    return s_topic_prefix;
  }
  return MQTT_TOPIC_PREFIX;
}

void mqtt_force_connect() {
  s_next_attempt_ms = 0;
  s_need_stop = false;
}

void mqtt_reset_failover_to_primary() {
  s_fail_streak = 0;
  if (s_active_broker != &kPrimaryBroker) {
    mqtt_schedule_reinit(&kPrimaryBroker, "reset_primary");
  }
}

void mqtt_force_failover_to_alt() {
  s_fail_streak = 0;
  if (s_active_broker != &kAltBroker) {
    mqtt_schedule_reinit(&kAltBroker, "force_alt");
  }
}

void mqtt_set_allowed(bool allowed) {
  // MQTT disabled: s_allowed starts false and stays false.
  // All mqtt_set_allowed(true) + mqtt_force_connect() calls become no-ops.
  // To re-enable MQTT, change s_allowed default back to true above.
  if (!s_allowed && allowed) {
    return;  // Don't re-enable if disabled at init
  }
  s_allowed = allowed;
  if (!allowed) {
    s_need_stop = true;
  } else {
    s_next_attempt_ms = 0;
    s_need_stop = false;
  }
}

void mqtt_stop_for_ota() {
  Serial.printf("[MQTT] stop_for_ota: stopping client to free heap (connected=%d started=%d)\n",
                s_connected ? 1 : 0, s_started ? 1 : 0);
  if (s_client) {
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = nullptr;
  }
  s_started = false;
  s_connected = false;
  s_allowed = false;
  s_need_stop = false;
  Serial.printf("[MQTT] stop_for_ota: done free_heap=%lu\n", (unsigned long)ESP.getFreeHeap());
}

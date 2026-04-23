#ifndef HALO_MQTT_CLIENT_H
#define HALO_MQTT_CLIENT_H

#include <Arduino.h>

// Auto-load MQTT secrets if available
#include "MqttSecrets.h"

// MQTT topics and payloads (Sense):
// - <prefix>/truth : JSON truth payload (periodic + event-driven)
// - <prefix>/event : {"event":"pong","uptime_ms":123}
// - <prefix>/cmd   : Commands (payload: "ota_check" | "reboot" | "ping" |
//                              "ota/force_now" | "ota/status/get")
// - <prefix>/desired : Retained desired OTA JSON payload
// - <fleet_prefix>/cmd : Fleet broadcast commands (same payloads)
// Prefix behavior:
// - Default base: "halo"
// - If owner_id is available -> "halo/<owner_id>/<device_id>"
// - Else -> "halo/<device_id>"
//   Commands are queued and handled in the main loop (no OTA in callback).

// Compile-time config (override via -D flags)
#ifndef MQTT_BROKER_URI
#define MQTT_BROKER_URI "mqtt://localhost"
#endif
#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 1883
#endif
#ifndef MQTT_ALT_BROKER_URI
#define MQTT_ALT_BROKER_URI MQTT_BROKER_URI
#endif
#ifndef MQTT_ALT_BROKER_PORT
#define MQTT_ALT_BROKER_PORT 443
#endif
#ifndef MQTT_ALT_ALPN
#define MQTT_ALT_ALPN "x-amzn-mqtt-ca"
#endif
#ifndef MQTT_FAILOVER_THRESHOLD
#define MQTT_FAILOVER_THRESHOLD 3
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif
#ifndef MQTT_TOPIC_PREFIX
#define MQTT_TOPIC_PREFIX "halo"
#endif
#ifndef MQTT_USE_OWNER_PREFIX
#define MQTT_USE_OWNER_PREFIX 1
#endif
#ifndef MQTT_FLEET_ENABLED
#define MQTT_FLEET_ENABLED 1
#endif
#ifndef MQTT_KEEPALIVE_SEC
#define MQTT_KEEPALIVE_SEC 60
#endif
#ifndef MQTT_TRUTH_INTERVAL_MS
#define MQTT_TRUTH_INTERVAL_MS 30000
#endif

// Fail build if secrets missing unless explicitly allowed
#ifndef MQTT_DEV_INSECURE
#define MQTT_DEV_INSECURE 0
#endif
#ifndef MQTT_SECRETS_PRESENT
#define MQTT_SECRETS_PRESENT 0
#endif
#if !MQTT_DEV_INSECURE && !MQTT_SECRETS_PRESENT
#error "MQTT secrets missing. Copy MqttSecrets.example.{h,cpp} to MqttSecrets.local.{h,cpp} or define MQTT_DEV_INSECURE=1."
#endif

// TLS config (optional; needed for mqtts:// brokers)
#ifndef MQTT_ROOT_CA
#define MQTT_ROOT_CA ""
#endif
#ifndef MQTT_CLIENT_CERT
#define MQTT_CLIENT_CERT ""
#endif
#ifndef MQTT_CLIENT_KEY
#define MQTT_CLIENT_KEY ""
#endif
#ifndef MQTT_SKIP_CN_CHECK
#define MQTT_SKIP_CN_CHECK 0
#endif
#ifndef MQTT_USE_CERT_BUNDLE
#define MQTT_USE_CERT_BUNDLE 1
#endif

enum MqttCommandType {
  MQTT_CMD_NONE = 0,
  MQTT_CMD_OTA_CHECK,
  MQTT_CMD_REBOOT,
  MQTT_CMD_PING,
  MQTT_CMD_OTA_FORCE_NOW,
  MQTT_CMD_OTA_STATUS_GET
};

struct MqttCommand {
  MqttCommandType type;
  char payload[128];
};

struct MqttMetrics {
  bool connected;
  uint32_t last_connect_ms;
  uint32_t last_pub_ms;
  uint32_t last_rx_ms;
  uint32_t fail_count;
  uint32_t pub_inflight;
  uint32_t cmd_queue_depth;
};

// Initialize MQTT task
void mqtt_init();

// Publish truth JSON with reason
void mqtt_publish_truth(const char* reason);

// Publish diagnostics JSON with reason
void mqtt_publish_diag(const char* reason);

// Publish diagnostics payload directly (returns true if queued)
bool mqtt_publish_diag_payload(const char* payload);

// Publish a small pong event
void mqtt_publish_pong(const char* reason);

// Fetch next queued command (returns true if a command was available)
bool mqtt_get_next_command(MqttCommand* out);

// Get current MQTT metrics
void mqtt_get_metrics(MqttMetrics* out);

// Get current MQTT topic prefix (device scoped)
const char* mqtt_get_topic_prefix();

// Force MQTT task to attempt connect ASAP
void mqtt_force_connect();

// Reset failover state back to primary broker
void mqtt_reset_failover_to_primary();

// Force a switch to the alternate broker
void mqtt_force_failover_to_alt();

// Allow or block MQTT connections (task still runs)
void mqtt_set_allowed(bool allowed);

// Stop MQTT and free TLS buffers for OTA. Call mqtt_force_connect() after OTA fails.
void mqtt_stop_for_ota();

#endif  // HALO_MQTT_CLIENT_H

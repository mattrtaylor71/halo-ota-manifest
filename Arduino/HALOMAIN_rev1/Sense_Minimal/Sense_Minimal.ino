/*
 * Sense_Minimal.ino
 * 
 * MINIMAL VERSION - Only essential functionality:
 * - LCD screen asleep by default
 * - Wake on INPUT_WAKE from LCD
 * - Connect to Wi-Fi
 * - Fetch list from AWS API
 * - Send UI_LIST to LCD
 * 
 * REMOVED: Camera, audio, MQTT, OTA, state machine, sleep/wake logic, all other features
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_sleep.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <driver/uart.h>
#include <driver/rtc_io.h>
#include <lwip/inet.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#if __has_include(<esp_crt_bundle.h>)
#include <esp_crt_bundle.h>
#define HAS_CRT_BUNDLE 1
#else
#define HAS_CRT_BUNDLE 0
#endif
#include "audio_bsp.h"  // Audio recording support
#include "../halo_ota_demo/firmware/shared/HaloPins.h"
#ifndef HALO_BOARD_SENSE
#define HALO_BOARD_SENSE 1
#endif
#include "../halo_common/BoardConfig.h"

// Guardrail: Sense firmware must never use light sleep.
#ifdef esp_light_sleep_start
#undef esp_light_sleep_start
#endif
#define esp_light_sleep_start(...) static_assert(false, "esp_light_sleep_start disabled; use esp_deep_sleep_start")

// ── Camera Model Definition (must be before camera_pins.h) ──────────
#define CAMERA_MODEL_XIAO_ESP32S3
#include "esp_camera.h"  // Camera support
#include "camera_pins.h"  // Camera pin definitions
#include <PubSubClient.h>  // MQTT support

#ifdef HALO_SENSE_PROD_WRAPPER
// Provisioning integration (Sense prod wrapper)
#include "../halo_ota_demo/firmware/shared/ProvisioningState.h"
#include "../halo_ota_demo/firmware/shared/OtaIntent.h"
bool halo_provisioning_active();
bool halo_get_provisioned_wifi(char* ssid, size_t ssid_sz, char* pass, size_t pass_sz);
void halo_prod_pre_setup();
void halo_prod_setup();
void halo_prod_loop();
void halo_prod_pre_sleep();
void halo_prod_reset_wifi();
bool halo_prod_should_delay_sleep();
void halo_prod_on_lcd_message(const char* type);
void halo_prod_on_lcd_wifi_creds_ack(const char* status, int err_code);
void halo_prod_on_lcd_wifi_on_ack();
bool halo_prod_should_defer_sleep_ack(bool* ota_busy, bool* mqtt_busy, bool* time_invalid, bool* ota_check_busy);
#endif

#ifdef HALO_SENSE_PROD_WRAPPER
void ota_on_timer_wake();
void ota_configure_timer_wakeup();
uint32_t ota_get_timer_delta_s();
#else
__attribute__((weak)) void ota_on_timer_wake() {}
__attribute__((weak)) void ota_configure_timer_wakeup() {}
__attribute__((weak)) uint32_t ota_get_timer_delta_s() { return 0; }
#endif

// ── Protocol Configuration ──────────────────────────────────────────
#define PROTOCOL_VERSION 1
#define MAX_LINE_LENGTH 2048  // Increased for UI_LIST messages (can be >1KB)
#define UART_RX_RING_SIZE 2048
#define UART_RX_FRAME_MAX 512

#ifndef HALO_DEBUG_SENSITIVE
#define HALO_DEBUG_SENSITIVE 0
#endif
#ifndef HALO_UART_SEND_WIFI_PASS
#define HALO_UART_SEND_WIFI_PASS 0
#endif

// ── UART Configuration ────────────────────────────────────────────────
#define UART_BAUD_RATE 115200
#ifndef UART_TX_PIN
#define UART_TX_PIN HaloPins::kSenseUartTxPin   // D6 on XIAO ESP32S3
#endif
#ifndef UART_RX_PIN
#define UART_RX_PIN HaloPins::kSenseUartRxPin   // D7 on XIAO ESP32S3
#endif
#ifndef UART_RX_DEBUG
#define UART_RX_DEBUG 0
#endif
static const uart_port_t LCD_UART_PORT = UART_NUM_1;
HardwareSerial lcdSerial(1);  // Use UART1

// ── Wakeup Configuration ──────────────────────────────────────────
// Wake roles:
// - Sense EXT0 wakes on GPIO2 (active low), driven by LCD INT_PIN pulses.
// - Sense does not drive LCD wake; LCD wakes on touch EXT0 GPIO9.
#define WAKE_GPIO HALO_WAKE_GPIO   // Sense pin that receives LCD INT (GPIO2/D1)
static const int WAKE_LEVEL = HALO_WAKE_LEVEL;  // Wake when GPIO2 is LOW
static const int WAKE_ACTIVE_LEVEL = WAKE_LEVEL;
static const int WAKE_INACTIVE_LEVEL = (WAKE_LEVEL == 0) ? 1 : 0;
static const unsigned long WAKE_PIN_INACTIVE_STABLE_MS = 50;
static const unsigned long WAKE_PIN_STABLE_WAIT_MS = 200;
static const unsigned long WAKE_PIN_MITIGATION_MS = 20;
static const uint32_t WAKE_PIN_FAILSAFE_TIMER_S = 15;
static const unsigned long WAKE_PIN_BOOT_WARN_MS = 200;
static const unsigned long WAKE_LINE_STUCK_WARN_MS = 3000;
static const uint32_t SLEEP_DENY_RETRY_DEFAULT_MS = 5000;
static const uint32_t SLEEP_DENY_RETRY_COOLDOWN_MAX_MS = 30000;
static bool last_wake_pin_state = false;
static unsigned long last_wake_pin_check = 0;
static unsigned long last_wake_ms = 0;  // Track last wake time for sleep guard
static const unsigned long MIN_AWAKE_BEFORE_SLEEP_MS = 10000;  // Give MQTT time to connect
static uint32_t g_sense_boot_count = 0;

// ── Sleep Timeout Configuration ────────────────────────────────────
static unsigned long last_lcd_communication = 0;  // Track last time we received a message from LCD
static const unsigned long LCD_INACTIVITY_TIMEOUT_MS = 10000;  // 10 seconds (LCD is sleep leader)
static const unsigned long LCD_INACTIVITY_TIMEOUT_PROVISION_MS = 1800000;  // 30 minutes during provisioning

// ── UART Heartbeat (Sense -> LCD) ─────────────────────────────────────
static const unsigned long LINK_HB_INTERVAL_MS = 4000;
static unsigned long last_link_hb_ms = 0;

// ── Wi-Fi ───────────────────────────────────────────────────────────
const char* WIFI_SSID = "Garage Member";
const char* WIFI_PASS = "build00!";

// ── Trepo API Configuration ────────────────────────────────────
const char* TREPO_API_BASE_URL = "https://1zc0nh8x48.execute-api.us-east-1.amazonaws.com";
const char* TREPO_API_HOST = "1zc0nh8x48.execute-api.us-east-1.amazonaws.com";
const char* TREPO_LIST_ENDPOINT = "/v1/list";
const char* TREPO_VOICE_ENDPOINT = "/v1/voice";
const char* TREPO_OWNER_ID = "7d7df434-d942-4037-b054-2d3005ea6abc";
const char* TREPO_DEVICE_ID = "*";

// ── Quick-Ack API Configuration (OpenAI Realtime API) ──────────
const char* QUICK_ACK_BASE_URL = "https://qq5tn5i3t3.execute-api.us-east-1.amazonaws.com";
const char* QUICK_ACK_ENDPOINT = "/voice-ack";

// ── Camera/Presign API Configuration ──────────────────────────
const char* PRESIGN_ENDPOINT_PRIMARY = "https://4a4bilqeq6.execute-api.us-east-1.amazonaws.com/Prod/v1/images/presign";
const char* PRESIGN_ENDPOINT_FALLBACK = "";  // Optional fallback
const char* API_KEY = "";  // Optional x-api-key header
const char* BEARER_TOKEN = "";  // Optional Authorization header
const char* DEVICE_ID = "dev123";
const char* USER_ID = "userA";
const char* DEFAULT_JOB_TYPE = "dish_eval";
static String current_job_type = String(DEFAULT_JOB_TYPE);

// ── Check-in API Configuration (Grocery Recognition) ──────────
const char* CHECKIN_API_BASE_URL = "https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com";
const char* CHECKIN_PRESIGN_ENDPOINT = "/presign";
const char* CHECKIN_OWNER_ID = "7d7df434-d942-4037-b054-2d3005ea6abc";  // Owner UUID (required for check-in API)

// ── Camera Configuration ───────────────────────────────────────
// Note: CAMERA_MODEL_XIAO_ESP32S3 is defined above before camera_pins.h include
static framesize_t CAPTURE_SIZE = FRAMESIZE_UXGA;  // 1600x1200
static int JPEG_QUALITY = 8;  // Lower = better quality
static const int FILL_LED_PIN = -1;  // No fill LED
static const int FILL_LED_MS = 160;

// ── MQTT Configuration ────────────────────────────────────────
const char* awsEndpoint = "arq86ma48kw9j-ats.iot.us-east-1.amazonaws.com";
const char* RESULT_TOPIC = "iot/trepo/dev/dev123/job/result";
const char* OTA_TOPIC = "trepo/halo/dev123/ota";
static unsigned long mqtt_wait_deadline = 0;
static bool mqtt_subscribed = false;
static String current_scan_job_id = "";  // Track SCAN job ID for MQTT matching
static bool waiting_for_mqtt_result = false;

// AWS IoT Certificates
const char* rootCA = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
)EOF";

const char* deviceCert = R"KEY(
-----BEGIN CERTIFICATE-----
MIIDWjCCAkKgAwIBAgIVAKUaKokE9Z0B7oRmoWGE1ZKzA76EMA0GCSqGSIb3DQEB
CwUAME0xSzBJBgNVBAsMQkFtYXpvbiBXZWIgU2VydmljZXMgTz1BbWF6b24uY29t
IEluYy4gTD1TZWF0dGxlIFNUPVdhc2hpbmd0b24gQz1VUzAeFw0yNDA3MDYwNTM2
NTFaFw00OTEyMzEyMzU5NTlaMB4xHDAaBgNVBAMME0FXUyBJb1QgQ2VydGlmaWNh
dGUwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC5ahrAj9IBu88TZ3tE
pm1gKP2ffZuoWUWEVGfMcZTKl6D9tOQHT0mg4D/LjafVSxBCYj9rIwdWVs+rW8sn
jNJN0zwaUdfl5lbmVdl4v+ipLBgLrHM7jQ91zu8tlzNl8bT3eN1a6NflbHbT3iz6
9Romai8wUbGpYALSWCSpAx4+gHJBuUCl8gYZy9c/DgqmS37lIH8gpv8KQR8/UYSw
LTGe6VXpiwhadNysA/MSwFf1IBxgYbQIIkFvgl9Hj2hO6/eekJegKULukE/h+vMq
421nNJDvIbh3TU1o0lz3JWkZkxmFKcVH4AvkG3ryVU2gj8+twxXLcaMuJ/sM6rPS
5aDJAgMBAAGjYDBeMB8GA1UdIwQYMBaAFJyChrq7fy4KVIEdjHeSVhooY6w+MB0G
A1UdDgQWBBSA/FyJTuOFzM2z1TTFyKll7p4UTjAMBgNVHRMBAf8EAjAAMA4GA1Ud
DwEB/wQEAwIHgDANBgkqhkiG9w0BAQsFAAOCAQEALNpaEzo0VekKuS1iMxpQsWfw
OR97M6vqLaWY7C/WFSVPwSCFk/hA/XTZSe7LfXFpl/BndQiR2A1fOPAZ/NbFhlkD
WYYQexeRGemiI9+SJoqXVB6sVUj9BoJq4fvcLIRy3ewLPYsYiQRKzveOBDhxrppS
Mz+YeEBIcJGSmzTUhopK+wlk2Fkg+j3sKiOKfINTbZMnoAGvU3I9QWV/PzCsBOIf
dHEvyUnVZ0ebdAFR8RxNCUyaVHLMh1d5t6OrCtbdl9WdDrmPrwirfClkrOsyZKN7
YwOY3IVRy4cx4mOFweWeZ/KZvu2azYIrfCFcsEJVPUl2+Tn+bbM4J40ADnLB9w==
-----END CERTIFICATE-----
)KEY";

const char* privateKey = R"KEY(
-----BEGIN RSA PRIVATE KEY-----
MIIEowIBAAKCAQEAuWoawI/SAbvPE2d7RKZtYCj9n32bqFlFhFRnzHGUypeg/bTk
B09JoOA/y42n1UsQQmI/ayMHVlbPq1vLJ4zSTdM8GlHX5eZW5lXZeL/oqSwYC6xz
O40Pdc7vLZczZfG093jdWujX5Wx2094s+vUaJmovMFGxqWAC0lgkqQMePoByQblA
pfIGGcvXPw4Kpkt+5SB/IKb/CkEfP1GEsC0xnulV6YsIWnTcrAPzEsBX9SAcYGG0
CCJBb4JfR49oTuv3npCXoClC7pBP4frzKuNtZzSQ7yG4d01NaNJc9yVpGZMZhSnF
R+AL5Bt68lVNoI/PrcMVy3GjLif7DOqz0uWgyQIDAQABAoIBAAIVDvfapaEfWtP8
9YVv2Qqbaz2/S2A4oG88A25oWCNuUICI54atfUYxPoYqsRfUH/qe39d0LUDq+KoT
/dQT4Mi+9o3VHXeXfqJXlHmmrhY4SIzZAvJIQ0QvbsA0Un9yl3WwBcxfkQ0iirMW
a6rl1cVYq+7++9/LFD0IgGDliBFrmu/s0job5XNyYV7mkFzM7LvSeCQ9xtMiwa0B
MFURj6RbU1jS5fP51/nFnJ+3Zm9GUkf/tqT6YJbBepsIUXydNBOwhUZWFbz0IWb0
7iIWZ6FGRNt6yS1YExwsxzsc2tJSi0foRLeXgzGjhNB9WaYUy/v1P2BFqdaL6D3y
t1bQe4ECgYEA9ExpPeP3Ush1j3S3AoXh9QsFrLZjT+BCU3uNsEma+FRwo1pPO0w5
vlOYslKsd4PGhumnUOMCVQyrH9XH4YJqJqxW8zcOO4dIR/gDAAq6Y63xtB6mYfKE
dtCiOjf1iKhbPo3JuumkZc8R6A9hcmjZVJHf+g/+oVZFPpvBmCE9aQMCgYEAwkuo
TkKhz19PCiCfoshP9h0qI48pkwVFeF6YO/abtzuGtq5z4OzpDGRjdoyXTWzpvQEq
Kdxr5m1Ag3DDLAouGPvBp5c9ySEQ81yis3egF7+OpI3Swv7IrijsKyHwmGFwV1S/
0sO35a6HVEZfol81wzG4NaNvfZirgVynROaWt0MCgYA+ObxQxGE518+B89Ots9Zj
KSSP4oEXVmLuirkDXyw29qMeKKGn0/mdTgPF4CMH6ivGL3ursbblXO21lSltel95
bEpVdv+MECBMHJL/Drx9KVA4ddohdrlg3jGELL7AyUk8fLcWge6a9Ax2lHxYvPYm
gWWQd0R/ac8HbHr6OfU/awKBgDBKn7V76D3joYCR5TuPcBhq3UtjTOEG4WJumIXm
4IMlX3FOYOzZ1X7IANS5Uu3ikSHyBSnMaGEobG1+/HOYwCZjhJmEBM5V0qG6N5JF
vFvKt8h8m5LtwrFO6Iw77lHhfgumu9rF3JJQ08AFkcWIxpMSa4ehbJeZ9566ibSd
X36DAoGBAMvVUYoO9UDxTlpUv8Bps5YbdOM8zG5r7QwfaPLzvmAuL1fyewDnfGzS
woQmfUu2XsmY27PLqWRngqzv8kOuwIgUMaevxbMjG0yvb5GQIuqVuq9T4yesKTfV
XoIKKh5tJj0rxpbpDZXgiTSQ0isFwzPrkTPGuRdSyx7n/0q98u/d
-----END RSA PRIVATE KEY-----
)KEY";

WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);

// ── Operation Manager ──────────────────────────────────────────────
enum OpType { OP_VOICE, OP_SCAN, OP_LIST_REFRESH };
enum OpPriority { PRI_USER, PRI_BG };
enum OpState { OP_IDLE, OP_RECORDING, OP_FINALIZE, OP_UPLOAD, OP_PARSE, OP_APPLY, OP_DONE };

struct OpJob {
  OpType type;
  OpPriority pri;
  uint32_t job_id;
  uint32_t created_ts;
  OpState state;
  bool active;
  char mode[16];  // "dish", "discard", or "check-in" for SCAN operations, empty for others
  char expiry_date[16];  // "YYYY-MM-DD" format for check-in mode
};

// ── Expiration Date Storage (for check-in mode) ──────────────────────
static char pending_expiry_date[16] = "";  // Store expiration date received from LCD
static bool expiry_date_response_received = false;  // Flag to indicate expiry date response was received (even if empty)
static OpJob* active_checkin_job = NULL;  // Pointer to active check-in job (if any)
static uint8_t* captured_image_buffer = NULL;  // Store captured image data for check-in mode (copied from camera buffer)
static size_t captured_image_size = 0;  // Size of captured image in bytes

// ── VOICE Operation State ──────────────────────────────────────────
#define AUDIO_BUFFER_SIZE (512 * 1024)  // 512KB buffer (~6 seconds at 24kHz, 16-bit mono)
static uint8_t* voice_audio_buffer = NULL;
static size_t voice_audio_pos = 0;
static size_t voice_audio_size = 0;
static bool voice_recording_active = false;
static SemaphoreHandle_t mic_mutex = NULL;

enum SenseSleepKind { SENSE_SLEEP_DEEP_IDLE = 0, SENSE_SLEEP_DEEP_MAINT = 1 };

// Forward declarations
static String voice_upload_and_parse();
static void voice_audio_callback(const int16_t *samples, size_t num_samples);
static void uart_send_ui_status(const char* text);
static void uart_reset_rx_state();
static bool parse_input_message(const char* json_str);

// Camera and SCAN operation forward declarations
struct PresignReply {
  String job_id;
  String put_url;
  String s3_key;
  String content_type;
  int    ttl_s;
  
  PresignReply() : ttl_s(0) {}  // Constructor to initialize ttl_s
};
static bool init_camera();
static void deinit_camera();
static void tune_sensor_for_food();
static bool warmup_and_capture(camera_fb_t*& fb);
static bool do_presign_request(const char* url, PresignReply& out, int& http_code, String& resp_body);
static bool get_presign(PresignReply& out);
static bool net_ready_for_tls(char* why, size_t why_len);
static void scan_terminal_reset();
static void scan_send_terminal_status(const char* phase, const char* text, const char* mode);
static void tls_configure(WiFiClientSecure& client, const char* reason);
static void log_wifi_snapshot(const char* label);
static void log_http_failure_details(const char* label, const char* url, int http_code, WiFiClientSecure* client);
static void presign_set_error_text(const char* text);
static const char* presign_error_text();
static bool ensure_time_valid(const char* reason);
static bool ensure_wifi_ready(const char* reason, uint32_t timeout_ms);
static bool wifi_hard_reset_and_reconnect(const char* reason, uint32_t timeout_ms);
static void wifi_recover_if_needed(const char* reason, int http_code);
static bool http_post_json_with_retries(const char* url, const String& body, int& http_code, String& resp_body, const char* label, const char* api_key, const char* bearer);
static bool put_to_presigned_url(const String& url, const uint8_t* buf, size_t len, const char* contentType);
static bool connect_to_mqtt();
static void mqtt_ensure_connected();
static void on_mqtt_message(char* topic, byte* payload, unsigned int length);
static void uart_send_ui_meal_result(int kcal, const char* meal_summary, int health_score, const char* recommendation, const char* mode = NULL, float protein_g = 0.0, float carbs_g = 0.0, float fat_g = 0.0, float confidence = 0.0);
static void apply_public_dns_for_api(const char* reason);
static bool ensure_dns_ready(const char* host);
static void sense_config_deep_sleep_wakeup();
static bool wake_pin_is_active_level(int level);
static void wake_pin_configure_rtc_input_inactive_pull();
static uint32_t sleep_deny_retry_ms(const char* reason, unsigned long now_ms);
static void sense_enter_deep_sleep(SenseSleepKind kind);
static void print_wake_cause(esp_sleep_wakeup_cause_t cause);
static void sense_enter_sleep(SenseSleepKind kind);

enum UiEvtType { UI_EVT_STATUS, UI_EVT_LIST_UPDATE, UI_EVT_MEAL_RESULT, UI_EVT_VOICE_ITEMS, UI_EVT_ERROR };

struct UiEvent {
  UiEvtType type;
  char op[8];      // "VOICE"/"SCAN"
  char phase[16];  // "RECORDING"/"UPLOADING"/...
  char text[64];
  // Payload data (items, meal result, etc.)
  JsonArray items;  // For UI_VOICE_ITEMS
  int calories;
  float protein_g, carbs_g, fat_g;
  float confidence;
};

// Operation queues
static QueueHandle_t op_queue = NULL;
static QueueHandle_t ui_event_queue = NULL;
static volatile bool foreground_active = false;
static OpJob current_job = {OP_LIST_REFRESH, PRI_BG, 0, 0, OP_IDLE, false, "", ""};
static TaskHandle_t op_worker_task_handle = NULL;  // Handle to suspend/resume task
static bool scan_terminal_sent = false;
static char presign_last_error_text[64] = "";
static bool sntp_started = false;

#ifndef HALO_SENSE_PROD_WRAPPER
static volatile bool g_lcd_ota_done = false;
static char g_lcd_ota_result[32] = "unknown";
static char g_lcd_ota_version[32] = "";
#endif

// ── Work State ─────────────────────────────────────────────────────
// Use work state pattern so UART RX never blocks on Wi-Fi
volatile bool refresh_requested = false;
volatile bool delete_requested = false;
volatile bool reset_wifi_requested = false;

static bool list_refresh_inflight = false;
static unsigned long list_refresh_start_ms = 0;
static unsigned long list_refresh_cooldown_until_ms = 0;
static const unsigned long LIST_REFRESH_TIMEOUT_MS = 20000;
#ifndef HALO_SENSE_PROD_WRAPPER
static const unsigned long LIST_REFRESH_COOLDOWN_MS = 3000;
#endif
static char g_last_refresh_reason[32] = "unknown";
static bool link_synced = false;
static unsigned long last_user_activity_ms = 0;
static unsigned long sleep_grace_until_ms = 0;
static bool wake_requested = false;
static unsigned long last_input_wake_ms = 0;
static unsigned long last_uart_rx_ms = 0;
static unsigned long last_uart_tx_ms = 0;
static unsigned long last_link_hb_rx_ms = 0;
static const unsigned long LINK_RECENT_MS = 10000;
static unsigned long last_sleep_coord_log_ms = 0;
static unsigned long last_sleep_coord_status_log_ms = 0;
static unsigned long last_uart_diag_ms = 0;
static unsigned long last_wake_pin_log_ms = 0;
static unsigned long sleep_holdoff_until_ms = 0;
static const unsigned long SLEEP_HOLDOFF_MS = 10000;
static bool lcd_wifi_has_creds = false;
static uint32_t lcd_wifi_checksum = 0;
static uint32_t last_sent_wifi_checksum = 0;

static void wake_net_stabilize(const char* reason) {
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  if (!wifi_ok) {
    return;
  }
  IPAddress ip = WiFi.localIP();
  Serial.printf("[WAKE_NET] check reason=%s ip=%s\n",
                reason ? reason : "unknown",
                ip.toString().c_str());
  apply_public_dns_for_api(reason ? reason : "wake_net");
  ensure_dns_ready(TREPO_API_HOST);
}

static void request_list_refresh(const char* reason, bool send_status) {
  unsigned long now = millis();
  sleep_holdoff_until_ms = now + SLEEP_HOLDOFF_MS;
  Serial.printf("[SLEEP_HOLDOFF] set until=%lu reason=%s\n",
                sleep_holdoff_until_ms,
                reason ? reason : "unknown");
  wake_net_stabilize(reason);
  if (list_refresh_inflight) {
    Serial.println("[LIST_REFRESH] suppress reason=inflight");
    if (send_status) {
      uart_send_ui_status("Refreshing...");
    }
    return;
  }
  if (list_refresh_cooldown_until_ms > 0 && now < list_refresh_cooldown_until_ms) {
    Serial.println("[LIST_REFRESH] suppress reason=cooldown");
    if (send_status) {
      uart_send_ui_status("Refreshing...");
    }
    return;
  }
  list_refresh_inflight = true;
  list_refresh_start_ms = now;
  refresh_requested = true;
  if (reason && reason[0]) {
    strncpy(g_last_refresh_reason, reason, sizeof(g_last_refresh_reason) - 1);
    g_last_refresh_reason[sizeof(g_last_refresh_reason) - 1] = '\0';
  }
  Serial.printf("[LIST_REFRESH] inflight=1 start_ms=%lu reason=%s\n",
                list_refresh_start_ms,
                reason ? reason : "unknown");
  if (send_status) {
    uart_send_ui_status("Refreshing...");
  }
}

unsigned long sense_get_last_user_activity_ms() {
  return last_user_activity_ms;
}

static void list_refresh_mark_complete(const char* reason) {
  if (list_refresh_inflight) {
    unsigned long now = millis();
    unsigned long dur_ms = list_refresh_start_ms > 0 ? (now - list_refresh_start_ms) : 0;
    list_refresh_inflight = false;
    list_refresh_start_ms = 0;
    list_refresh_cooldown_until_ms = now + LIST_REFRESH_COOLDOWN_MS;
    Serial.printf("[LIST_REFRESH] inflight=0 result=%s dur_ms=%lu\n",
                  reason ? reason : "done",
                  dur_ms);
  }
}

static const char* op_type_name(OpType type) {
  switch (type) {
    case OP_VOICE:
      return "voice";
    case OP_SCAN:
      return "scan";
    case OP_LIST_REFRESH:
      return "list_refresh";
    default:
      return "unknown";
  }
}
static char delete_item_id[64] = {0};
volatile bool sleep_requested = false;  // Flag to request sleep from main loop
static bool sleep_ack_pending = false;  // Send SLEEP_ACK right before sleep
static bool sleep_ack_defer_logged = false;
static bool sleep_request_logged = false;
static unsigned long sleep_request_ms = 0;
static bool uart_wake_enabled_state = false;
static bool sleep_ack_sent_for_request = false;
static bool sleep_ready_sent_for_cycle = false;
static bool sleep_coord_requested = false;
static const char* sleep_ready_reason = "unknown";
static unsigned long pre_sleep_block_start_ms = 0;
static const unsigned long PRE_SLEEP_BLOCK_MAX_MS = 10000;

enum SleepSmState {
  SLEEP_SM_IDLE = 0,
  SLEEP_SM_REQ_RX,
  SLEEP_SM_ACK_SENT,
  SLEEP_SM_READY_SENT,
  SLEEP_SM_SLEEPING
};

static SleepSmState sleep_sm_state = SLEEP_SM_IDLE;
static uint32_t sleep_sm_msg_id = 0;

// ── Shopping List State ─────────────────────────────────────────────
#define MAX_LIST_ITEMS 50
#define MAX_ITEM_LENGTH 64

struct shopping_list_item_t {
  char text[MAX_ITEM_LENGTH];
  char id[MAX_ITEM_LENGTH];
};

static shopping_list_item_t g_shopping_list[MAX_LIST_ITEMS];
static int g_list_count = 0;
static int g_selected_index = -1;
static SemaphoreHandle_t g_list_mutex = NULL;

// ── Protocol State ──────────────────────────────────────────────────
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
static volatile bool wake_pulse_seen = false;
static const unsigned long WAKE_PIN_DEASSERT_WAIT_MS = 1500;
static unsigned long sleep_abort_active_pin_count = 0;
static unsigned long sleep_retry_deassert_count = 0;
static char last_sleep_abort_reason[32] = "";
static unsigned long wake_pin_abort_count_total = 0;
static unsigned long wake_pin_abort_count_this_boot = 0;
static int wake_pin_stuck_last_level = -1;
static bool sleep_deny_sent_for_request = false;

// ── UART Functions ──────────────────────────────────────────────────
static void initUarts() {
  if (uart_initialized) {
    Serial.println("[UART_INIT] skipped already_initialized=1");
    return;
  }
  Serial.begin(115200);
  delay(50);
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
  // IDF expects 3..0x3ff for wake threshold (3 edges ≈ 'a' on 8N1)
  esp_err_t wake_err = uart_set_wakeup_threshold(LCD_UART_PORT, 3);
  if (wake_err != ESP_OK) {
    Serial.printf("[UART] Wake threshold set failed: %d\n", wake_err);
  }
}

static bool uart_rx_is_printable(char c) {
  return c >= 0x20 && c <= 0x7E;
}

static void link_reset_parser_state() {
  uart_reset_rx_state();
  int dropped = 0;
  while (lcdSerial.available() > 0 && dropped < 256) {
    (void)lcdSerial.read();
    dropped++;
  }
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

static void uart_process_rx_ring() {
  char c = '\0';
  while (uart_ring_pop(&c)) {
    if (c == '\n') {
      if (uart_rx_frame_overflow) {
        Serial.printf("[PROTO] frame_oversize dropped_to_newline bytes=%u\n",
                      (unsigned)uart_rx_oversize_drop);
      } else if (uart_rx_frame_len > 0) {
        size_t start = 0;
        size_t end = uart_rx_frame_len;
        while (start < end && uart_rx_frame[start] == ' ') {
          start++;
        }
        while (end > start && uart_rx_frame[end - 1] == ' ') {
          end--;
        }
        size_t trimmed_len = end > start ? (end - start) : 0;
        if (trimmed_len > 0) {
          if (uart_rx_frame[start] != '{') {
            Serial.printf("[PROTO] dropped_nonjson_line len=%u first_char=%c\n",
                          (unsigned)trimmed_len,
                          uart_rx_frame[start]);
          } else {
            if (start > 0 || end < uart_rx_frame_len) {
              memmove(uart_rx_frame, uart_rx_frame + start, trimmed_len);
            }
            uart_rx_frame[trimmed_len] = '\0';
            parse_input_message(uart_rx_frame);
          }
        }
      }
      uart_rx_frame_len = 0;
      uart_rx_frame_overflow = false;
      uart_rx_dropped_since_frame = 0;
      uart_rx_oversize_drop = 0;
      continue;
    }
    if (uart_rx_frame_overflow) {
      uart_rx_dropped_since_frame++;
      uart_rx_oversize_drop++;
      continue;
    }
    if (uart_rx_frame_len >= UART_RX_FRAME_MAX) {
      uart_rx_frame_overflow = true;
      uart_rx_dropped_since_frame++;
      uart_rx_oversize_drop++;
      continue;
    }
    uart_rx_frame[uart_rx_frame_len++] = c;
  }
}

static void wake_rx_sanitize() {
  uart_reset_rx_state();
  const unsigned long start_ms = millis();
  const unsigned long max_ms = 20;
  const int max_bytes = 256;
  int read_bytes = 0;
  uint8_t first_bytes[8];
  size_t first_len = 0;
  int last_byte = -1;
  bool saw_frame_start = false;
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
      uart_ring_push(c);
      saw_frame_start = true;
      break;
    }
    if (uart_rx_dropped_since_frame < 0xFFFFFFFFu) {
      uart_rx_dropped_since_frame++;
    }
    if (first_len < sizeof(first_bytes)) {
      first_bytes[first_len++] = static_cast<uint8_t>(raw);
    }
  }
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
                uart_rx_dropped_since_frame,
                first_hex,
                last_byte >= 0 ? last_byte : 0xFF);
  if (saw_frame_start) {
    uart_process_rx_ring();
  }
}

static uint32_t get_next_msg_id() {
  uint32_t id = sense_msg_id_counter++;
  if (sense_msg_id_counter == 0) {
    sense_msg_id_counter = 1;  // Wrap at 2^32, but avoid 0
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

static void uart_send_json(const char* json_str) {
  size_t len = strlen(json_str);
  last_uart_tx_ms = millis();
  lcdSerial.print(json_str);
  lcdSerial.print("\n");
  lcdSerial.flush();
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

static void uart_send_ui_list() {
  StaticJsonDocument<4096> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "UI_LIST";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["selected_index"] = g_selected_index;
  
  JsonArray items = doc.createNestedArray("items");
  if (g_list_mutex != NULL && xSemaphoreTake(g_list_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < g_list_count; i++) {
      JsonObject item = items.createNestedObject();
      item["id"] = g_shopping_list[i].id;
      item["text"] = g_shopping_list[i].text;
    }
    xSemaphoreGive(g_list_mutex);
  }
  
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void uart_send_ui_status(const char* text) {
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "UI_STATUS";
  uint32_t msg_id = get_next_msg_id();
  doc["msg_id"] = msg_id;
  doc["ts"] = millis();
  doc["text"] = text;
  String output;
  serializeJson(doc, output);
  Serial.printf("[UART_TX] UI_STATUS msg_id=%u\n", (unsigned)msg_id);
  uart_send_json(output.c_str());
}

static void uart_send_ui_status_extended(const char* op, const char* phase, const char* text, const char* mode = NULL) {
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "UI_STATUS";
  uint32_t msg_id = get_next_msg_id();
  doc["msg_id"] = msg_id;
  doc["ts"] = millis();
  if (op != NULL && strlen(op) > 0) doc["op"] = op;
  if (phase != NULL && strlen(phase) > 0) doc["phase"] = phase;
  doc["text"] = text;
  if (mode != NULL && strlen(mode) > 0) doc["mode"] = mode;  // "dish" or "discard" for SCAN operations
  String output;
  serializeJson(doc, output);
  Serial.printf("[UART_TX] UI_STATUS msg_id=%u\n", (unsigned)msg_id);
  uart_send_json(output.c_str());
}

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

static void uart_send_input_sleep_ack(const char* status, const char* reason) {
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_SLEEP_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["status"] = (status && status[0]) ? status : "OK";
  doc["reason"] = (reason && reason[0]) ? reason : "received";
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void uart_send_sleep_ack() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "SLEEP_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void uart_send_sleep_busy() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "SLEEP_BUSY";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

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

static const char* sleep_sm_state_name(SleepSmState state) {
  switch (state) {
    case SLEEP_SM_IDLE:
      return "IDLE";
    case SLEEP_SM_REQ_RX:
      return "REQ_RX";
    case SLEEP_SM_ACK_SENT:
      return "ACK_SENT";
    case SLEEP_SM_READY_SENT:
      return "READY_SENT";
    case SLEEP_SM_SLEEPING:
      return "SLEEPING";
    default:
      return "UNKNOWN";
  }
}

static void sleep_sm_transition(SleepSmState next, const char* event, uint32_t msg_id) {
  sleep_sm_state = next;
  Serial.printf("[SLEEP_SM] state=%s event=%s msg_id=%u\n",
                sleep_sm_state_name(next),
                event ? event : "none",
                (unsigned)msg_id);
}

static uint32_t wifi_creds_checksum(const char* ssid, const char* pass) {
  const uint32_t FNV_OFFSET = 2166136261u;
  const uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = FNV_OFFSET;
  const char* s = ssid ? ssid : "";
  const char* p = pass ? pass : "";
  while (*s) {
    hash ^= (uint8_t)(*s++);
    hash *= FNV_PRIME;
  }
  hash ^= 0xFF;
  while (*p) {
    hash ^= (uint8_t)(*p++);
    hash *= FNV_PRIME;
  }
  return hash;
}

static uint32_t sense_current_wifi_checksum(bool* has_creds_out,
                                            char* ssid_buf,
                                            size_t ssid_sz,
                                            char* pass_buf,
                                            size_t pass_sz) {
  const char* ssid = WIFI_SSID;
  const char* pass = WIFI_PASS;
  if (ssid_buf && ssid_sz > 0) ssid_buf[0] = '\0';
  if (pass_buf && pass_sz > 0) pass_buf[0] = '\0';
#ifdef HALO_SENSE_PROD_WRAPPER
  if (ssid_buf && pass_buf && ssid_sz > 0 && pass_sz > 0) {
    if (halo_get_provisioned_wifi(ssid_buf, ssid_sz, pass_buf, pass_sz)) {
      ssid = ssid_buf;
      pass = pass_buf;
    }
  }
#endif
  bool has_creds = (ssid && ssid[0]);
  if (has_creds_out) {
    *has_creds_out = has_creds;
  }
  if (!has_creds) {
    return 0;
  }
  return wifi_creds_checksum(ssid, pass);
}

void uart_send_wifi_creds_json(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) {
    return;
  }
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "WIFI_CREDS";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["ssid"] = ssid;
#if HALO_UART_SEND_WIFI_PASS
  doc["pass"] = pass ? pass : "";
#else
  doc["pass_present"] = (pass && pass[0]) ? 1 : 0;
#endif
  doc["checksum"] = wifi_creds_checksum(ssid, pass ? pass : "");
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static unsigned long sleep_block_wifi_on_until_ms = 0;
static const unsigned long WIFI_ON_GRACE_MS = 60000;

void uart_send_wifi_on_json(uint32_t timeout_ms) {
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "WIFI_ON";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (timeout_ms > 0) {
    doc["timeout_ms"] = timeout_ms;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  sleep_block_wifi_on_until_ms = millis() + WIFI_ON_GRACE_MS;
}

// ── Wi-Fi Functions ────────────────────────────────────────────────
bool wifi_is_connected() {
  return WiFi.status() == WL_CONNECTED;
}

enum WifiGuardState {
  WIFI_STATE_DISCONNECTED = 0,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_CONNECTED,
  WIFI_STATE_FAILED,
  WIFI_STATE_FAILED_TIMEOUT
};

static volatile bool wifi_connect_inflight = false;
static unsigned long wifi_inflight_start_ms = 0;
static unsigned long wifi_last_begin_ms = 0;
static unsigned long wifi_connected_ms = 0;
static unsigned long wifi_last_poll_ms = 0;
static IPAddress wifi_last_ip;
static int wifi_last_rssi = 0;
static WifiGuardState wifi_state = WIFI_STATE_DISCONNECTED;
static const char* wifi_truth_state = "disconnected";
static unsigned long last_wifi_fail_ms = 0;
static char last_wifi_fail_reason[24] = "";

static bool sleep_block_active(unsigned long now_ms, const char** reason, const char** op) {
  if (reason) {
    *reason = NULL;
  }
  if (op) {
    *op = "none";
  }
  if (list_refresh_inflight) {
    if (reason) *reason = "list_refresh_inflight";
    if (op) *op = "list_refresh";
    return true;
  }
  if (sleep_block_wifi_on_until_ms > 0 && now_ms < sleep_block_wifi_on_until_ms) {
    if (reason) *reason = "wifi_on_grace";
    if (op) *op = "wifi_on";
    return true;
  }
  if (now_ms < sleep_grace_until_ms) {
    if (reason) *reason = "grace_window";
    if (op) *op = "wake";
    return true;
  }
  if (wifi_connect_inflight) {
    if (reason) *reason = "op_inflight";
    if (op) *op = "wifi";
    return true;
  }
  if (current_job.state != OP_IDLE && current_job.state != OP_DONE) {
    if (reason) *reason = "op_inflight";
    if (op) *op = op_type_name(current_job.type);
    return true;
  }
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_prod_should_delay_sleep()) {
    if (reason) *reason = "op_inflight";
    if (op) *op = "halo_prod";
    return true;
  }
#endif
  return false;
}

static const unsigned long WIFI_BEGIN_COOLDOWN_MS = 4000;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 25000;
static const unsigned long WIFI_POLL_INTERVAL_MS = 150;
static const unsigned long WIFI_FAIL_COOLDOWN_MS = 0UL;  /* no cooldown: retry every pre_sleep so bad wifi can recover */

static const char* wifi_state_to_string(WifiGuardState state) {
  switch (state) {
    case WIFI_STATE_DISCONNECTED: return "disconnected";
    case WIFI_STATE_CONNECTING: return "connecting";
    case WIFI_STATE_CONNECTED: return "connected";
    case WIFI_STATE_FAILED: return "failed";
    case WIFI_STATE_FAILED_TIMEOUT: return "failed_timeout";
    default: return "unknown";
  }
}

static const char* wifi_state_to_truth(WifiGuardState state) {
  switch (state) {
    case WIFI_STATE_CONNECTED: return "connected";
    case WIFI_STATE_FAILED:
    case WIFI_STATE_FAILED_TIMEOUT:
      return "failed";
    case WIFI_STATE_CONNECTING:
    case WIFI_STATE_DISCONNECTED:
    default:
      return "disconnected";
  }
}

static void wifi_guard_set_state(WifiGuardState new_state,
                                 const char* reason,
                                 wl_status_t status) {
  if (wifi_state == new_state) {
    wifi_truth_state = wifi_state_to_truth(new_state);
    return;
  }
  wifi_state = new_state;
  wifi_truth_state = wifi_state_to_truth(new_state);
  if (new_state == WIFI_STATE_CONNECTED) {
    last_wifi_fail_ms = 0;
    last_wifi_fail_reason[0] = '\0';
  }
  if (new_state == WIFI_STATE_CONNECTED) {
    wifi_last_ip = WiFi.localIP();
    wifi_last_rssi = WiFi.RSSI();
  }
  Serial.printf("[WIFI_GUARD] state=%s truth=%s status=%d reason=%s\n",
                wifi_state_to_string(new_state),
                wifi_truth_state,
                (int)status,
                reason ? reason : "unknown");
}

static unsigned long wifi_last_fail_ms() {
  return last_wifi_fail_ms;
}

static const char* wifi_last_fail_reason() {
  return last_wifi_fail_reason[0] ? last_wifi_fail_reason : "none";
}

extern "C" const char* halo_wifi_guard_truth_state() {
  return wifi_truth_state;
}

static void wifi_guard_set_inflight(bool inflight) {
  wifi_connect_inflight = inflight;
  if (!inflight) {
    wifi_inflight_start_ms = 0;
  }
}

static void wifi_guard_note_fail(const char* reason) {
  last_wifi_fail_ms = millis();
  if (reason && reason[0]) {
    strncpy(last_wifi_fail_reason, reason, sizeof(last_wifi_fail_reason) - 1);
    last_wifi_fail_reason[sizeof(last_wifi_fail_reason) - 1] = '\0';
  } else {
    strncpy(last_wifi_fail_reason, "unknown", sizeof(last_wifi_fail_reason) - 1);
    last_wifi_fail_reason[sizeof(last_wifi_fail_reason) - 1] = '\0';
  }
}

static void wifi_guard_handle_timeout(unsigned long elapsed_ms) {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_STA);
  wifi_guard_set_inflight(false);
  wifi_guard_set_state(WIFI_STATE_FAILED_TIMEOUT, "timeout", WiFi.status());
  wifi_guard_note_fail("timeout");
  Serial.printf("[WIFI_GUARD] connect_timeout elapsed_ms=%lu\n", elapsed_ms);
}

static void wifi_guard_mark_failed(wl_status_t status, const char* reason) {
  wifi_guard_set_inflight(false);
  wifi_guard_set_state(WIFI_STATE_FAILED, reason, status);
  wifi_guard_note_fail(reason);
  Serial.printf("[WIFI_GUARD] connect_fail status=%d reason=%s\n",
                (int)status,
                reason ? reason : "unknown");
}

static void wifi_guard_poll() {
  if (!wifi_connect_inflight) {
    return;
  }
  unsigned long now = millis();
  if ((now - wifi_last_poll_ms) < WIFI_POLL_INTERVAL_MS) {
    return;
  }
  wifi_last_poll_ms = now;
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = now;
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "poll", status);
    Serial.printf("[WIFI_GUARD] connect_ok ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    return;
  }
  if (status == WL_CONNECT_FAILED ||
      status == WL_NO_SSID_AVAIL ||
      status == WL_DISCONNECTED ||
      status == WL_CONNECTION_LOST) {
    wifi_guard_mark_failed(status, "poll");
    return;
  }
  if (wifi_inflight_start_ms > 0 &&
      (now - wifi_inflight_start_ms) > WIFI_CONNECT_TIMEOUT_MS) {
    wifi_guard_handle_timeout(now - wifi_inflight_start_ms);
  }
}

static bool sense_idle_mode_active() {
  if (list_refresh_inflight) {
    return false;
  }
  if (wifi_connect_inflight) {
    return false;
  }
  if (current_job.state != OP_IDLE && current_job.state != OP_DONE) {
    return false;
  }
  if (waiting_for_mqtt_result) {
    return false;
  }
  if (sleep_requested) {
    return false;
  }
  return true;
}

static void handle_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
#if defined(ARDUINO_EVENT_WIFI_STA_GOT_IP)
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = millis();
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "event_got_ip", WL_CONNECTED);
    Serial.printf("[WIFI_GUARD] connect_ok ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    apply_public_dns_for_api("wifi_got_ip");
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    wl_status_t status = WiFi.status();
    if (wifi_connect_inflight) {
      wifi_guard_mark_failed(status, "event_disconnect");
    } else if (wifi_state == WIFI_STATE_FAILED || wifi_state == WIFI_STATE_FAILED_TIMEOUT) {
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(wifi_state, "event_disconnect", status);
    } else {
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "event_disconnect", status);
    }
  }
#elif defined(SYSTEM_EVENT_STA_GOT_IP)
  if (event == SYSTEM_EVENT_STA_GOT_IP) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = millis();
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "event_got_ip", WL_CONNECTED);
    Serial.printf("[WIFI_GUARD] connect_ok ip=%s rssi=%d\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    apply_public_dns_for_api("wifi_got_ip");
  } else if (event == SYSTEM_EVENT_STA_DISCONNECTED) {
    wl_status_t status = WiFi.status();
    if (wifi_connect_inflight) {
      wifi_guard_mark_failed(status, "event_disconnect");
    } else if (wifi_state == WIFI_STATE_FAILED || wifi_state == WIFI_STATE_FAILED_TIMEOUT) {
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(wifi_state, "event_disconnect", status);
    } else {
      wifi_guard_set_inflight(false);
      wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "event_disconnect", status);
    }
  }
#endif
  (void)info;
}

static bool ensure_wifi_connected(const char* reason, uint32_t timeout_ms) {
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active()) {
    Serial.println("[WIFI] Provisioning active - skipping Wi-Fi connect");
    wifi_guard_set_inflight(false);
    wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "provisioning_active", WiFi.status());
    return false;
  }
#endif
  if (wifi_is_connected()) {
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "already_connected", WL_CONNECTED);
    return true;
  }
  unsigned long now = millis();
  wifi_guard_poll();
  if (wifi_connect_inflight && wifi_inflight_start_ms > 0) {
    unsigned long inflight_elapsed = now - wifi_inflight_start_ms;
    if (reason && strcmp(reason, "pre_sleep") == 0 &&
        inflight_elapsed < WIFI_CONNECT_TIMEOUT_MS) {
      unsigned long wait_start = millis();
      while (wifi_connect_inflight && (millis() - wait_start) < 2000) {
        wifi_guard_poll();
        if (WiFi.status() == WL_CONNECTED) {
          return true;
        }
        delay(100);
      }
      if (!wifi_connect_inflight && WiFi.status() == WL_CONNECTED) {
        return true;
      }
    }
    Serial.printf("[WIFI_GUARD] skip_begin inflight=1 reason=wifi_inflight caller=%s\n",
                  reason ? reason : "unknown");
    return false;
  }
  if (wifi_last_begin_ms > 0 && (now - wifi_last_begin_ms) < WIFI_BEGIN_COOLDOWN_MS) {
    Serial.printf("[WIFI_GUARD] skip_begin cooldown_ms=%lu reason=%s\n",
                  (unsigned long)(now - wifi_last_begin_ms), reason ? reason : "unknown");
    return false;
  }

  const char* ssid = WIFI_SSID;
  const char* pass = WIFI_PASS;
#ifdef HALO_SENSE_PROD_WRAPPER
  char provision_ssid[64];
  char provision_pass[64];
  if (halo_get_provisioned_wifi(provision_ssid, sizeof(provision_ssid),
                                provision_pass, sizeof(provision_pass))) {
    ssid = provision_ssid;
    pass = provision_pass;
  }
#endif
  if (!ssid || !ssid[0]) {
    Serial.println("[WIFI_GUARD] no_ssid - skipping Wi-Fi connect");
    wifi_guard_set_state(WIFI_STATE_DISCONNECTED, "no_ssid", WiFi.status());
    return false;
  }

  Serial.printf("[WIFI_GUARD] begin_connect inflight=1 reason=%s ssid=%s\n",
                reason ? reason : "unknown", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  wifi_guard_set_inflight(true);
  wifi_inflight_start_ms = millis();
  wifi_last_begin_ms = wifi_inflight_start_ms;
  wifi_guard_set_state(WIFI_STATE_CONNECTING, "begin_connect", WiFi.status());

  if (timeout_ms == 0) {
    return false;
  }
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
    wifi_guard_poll();
    if (!wifi_connect_inflight && WiFi.status() != WL_CONNECTED) {
      break;
    }
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_guard_set_inflight(false);
    wifi_connected_ms = millis();
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "wait_connected", WL_CONNECTED);
    Serial.println("\nWi-Fi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    apply_public_dns_for_api("wifi_connected");
    return true;
  }
  if (wifi_connect_inflight) {
    wifi_guard_mark_failed(WiFi.status(), "wait_timeout");
  }
  return false;
}

static bool wifi_connect() {
  Serial.print("Connecting to Wi-Fi: ");
  const char* ssid = WIFI_SSID;
#ifdef HALO_SENSE_PROD_WRAPPER
  char provision_ssid[64];
  char provision_pass[64];
  if (halo_get_provisioned_wifi(provision_ssid, sizeof(provision_ssid),
                                provision_pass, sizeof(provision_pass))) {
    ssid = provision_ssid;
  }
#endif
  Serial.println(ssid ? ssid : "(null)");
  if (ensure_wifi_connected("wifi_connect", 10000)) {
    return true;
  }
  Serial.println("\nWi-Fi connection failed!");
  return false;
}

static void list_refresh_fail(const char* reason) {
  Serial.printf("[LIST_REFRESH] fail reason=%s\n", reason ? reason : "unknown");
  uart_send_ui_status("IDLE");
}

// ── List Fetching from API ──────────────────────────────────────────
static bool parse_and_update_shopping_list(const String& json_response) {
  Serial.println("\n=== Parsing Shopping List from JSON ===");
  
  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, json_response);
  
  if (error) {
    Serial.print("✗ JSON parse error: ");
    Serial.println(error.c_str());
    return false;
  }
  
  if (!doc.containsKey("items") || !doc["items"].is<JsonArray>()) {
    Serial.println("✗ No 'items' array in response");
    return false;
  }
  
  JsonArray items = doc["items"].as<JsonArray>();
  int item_count = items.size();
  Serial.print("Found ");
  Serial.print(item_count);
  Serial.println(" items in response");
  
  if (g_list_mutex == NULL) {
    Serial.println("✗ List mutex not initialized!");
    return false;
  }
  
  if (xSemaphoreTake(g_list_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("✗ Failed to acquire list mutex");
    return false;
  }
  
  // Clear existing list
  g_list_count = 0;
  g_selected_index = -1;
  
  // Parse items
  int actual_count = 0;
  for (JsonObject item : items) {
    if (actual_count >= MAX_LIST_ITEMS) break;
    
    // Skip checked items
    const char* action = item["action"] | "";
    if (strcmp(action, "CHECKED") == 0) continue;
    
    // Extract product_name
    const char* product_name = item["product_name"] | "";
    if (product_name == NULL || strlen(product_name) == 0) continue;
    
    // Copy product name
    size_t name_len = strlen(product_name);
    if (name_len >= MAX_ITEM_LENGTH) name_len = MAX_ITEM_LENGTH - 1;
    strncpy(g_shopping_list[actual_count].text, product_name, name_len);
    g_shopping_list[actual_count].text[name_len] = '\0';
    
    // Extract item ID
    const char* item_id_str = NULL;
    if (item.containsKey("id")) {
      if (item["id"].is<const char*>()) {
        item_id_str = item["id"].as<const char*>();
      } else if (item["id"].is<int>()) {
        int item_id_int = item["id"].as<int>();
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "%d", item_id_int);
        item_id_str = id_buf;
      }
    }
    
    if (item_id_str != NULL && strlen(item_id_str) > 0) {
      size_t id_len = strlen(item_id_str);
      if (id_len >= MAX_ITEM_LENGTH) id_len = MAX_ITEM_LENGTH - 1;
      strncpy(g_shopping_list[actual_count].id, item_id_str, id_len);
      g_shopping_list[actual_count].id[id_len] = '\0';
    } else {
      g_shopping_list[actual_count].id[0] = '\0';
    }
    
    actual_count++;
  }
  
  g_list_count = actual_count;
  g_selected_index = (actual_count > 0) ? 0 : -1;
  
  xSemaphoreGive(g_list_mutex);
  
  Serial.print("✓ Parsed ");
  Serial.print(actual_count);
  Serial.println(" items");
  
  // Send updated list to LCD
  uart_send_ui_list();
  
  // Send IDLE status
  delay(50);
  uart_send_ui_status("IDLE");
  return true;
}

// ── Delete Item from API ────────────────────────────────────────────
static void delete_item_from_api(const char* item_id) {
  if (item_id == NULL || strlen(item_id) == 0) {
    Serial.println("✗ Cannot delete item: invalid ID!");
    return;
  }
  
  Serial.printf("\n=== Deleting Item ID: %s ===\n", item_id);
  
  // Build JSON request body for remove operation
  String request_body = "{";
  request_body += "\"operation\":\"remove\",";
  request_body += "\"ownerId\":\"" + String(TREPO_OWNER_ID) + "\",";
  request_body += "\"id\":\"" + String(item_id) + "\"";
  request_body += "}";
  
  // Create HTTPS client
  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();
  
  String list_url = String(TREPO_API_BASE_URL) + String(TREPO_LIST_ENDPOINT);
  http.begin(client, list_url);
  http.setTimeout(30000);
  http.setConnectTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  
  Serial.print("URL: ");
  Serial.println(list_url);
  Serial.print("Request body: ");
  Serial.println(request_body);
  
  int httpResponseCode = http.POST(request_body);
  Serial.print("HTTP Response code: ");
  Serial.println(httpResponseCode);
  
  String response_json = "";
  if (httpResponseCode > 0) {
    response_json = http.getString();
    Serial.print("Response length: ");
    Serial.println(response_json.length());
  }
  
  http.end();
  client.stop();
  
  if (httpResponseCode == 200) {
    Serial.println("✓ Item deleted successfully from backend");
    uart_send_ui_status("Item deleted");
  } else {
    Serial.printf("✗ Delete failed with HTTP code: %d\n", httpResponseCode);
    Serial.println("[DELETE] fail -> UI idle");
    uart_send_ui_status("IDLE");
  }
}

static bool extract_host_from_url(const String& url, String& host) {
  int scheme = url.indexOf("://");
  int start = scheme >= 0 ? scheme + 3 : 0;
  int slash = url.indexOf('/', start);
  if (slash < 0) {
    slash = url.length();
  }
  host = url.substring(start, slash);
  return host.length() > 0;
}

static void net_diag_dns_for_host(const char* host, const char* reason) {
  IPAddress dns0 = WiFi.dnsIP(0);
  IPAddress dns1 = WiFi.dnsIP(1);
  IPAddress resolved_ip;
  bool resolve_ok = false;
  if (host && host[0]) {
    resolve_ok = WiFi.hostByName(host, resolved_ip);
  }
  Serial.printf("[NET_DIAG] reason=%s host=%s dns0=%s dns1=%s resolve_ok=%d\n",
                reason ? reason : "unknown",
                host ? host : "(null)",
                dns0.toString().c_str(),
                dns1.toString().c_str(),
                resolve_ok ? 1 : 0);
}

static bool dns_ip_is_empty(const IPAddress& ip) {
  bool all_zero = (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
  bool all_ff = (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255);
  return all_zero || all_ff;
}

static bool dns_resolve_test(const char* host) {
  if (!host || !host[0]) {
    return true;
  }
  IPAddress resolved_ip;
  bool resolve_ok = WiFi.hostByName(host, resolved_ip);
  Serial.printf("[DNS] resolve_test host=%s ok=%d\n",
                host,
                resolve_ok ? 1 : 0);
  return resolve_ok;
}

static bool apply_dns_strategy(const char* reason, const char* host_a, const char* host_b) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  IPAddress dns0 = WiFi.dnsIP(0);
  IPAddress dns1 = WiFi.dnsIP(1);
  bool dns_missing = dns_ip_is_empty(dns0) && dns_ip_is_empty(dns1);
  bool resolve_ok = true;
  resolve_ok = dns_resolve_test(host_a) && dns_resolve_test(host_b);
  const char* decision = (dns_missing || !resolve_ok) ? "use_fallback" : "use_dhcp";
  const char* why = dns_missing ? "dhcp_missing" : (!resolve_ok ? "resolve_fail" : "dhcp_ok");
  Serial.printf("[DNS] dhcp d0=%s d1=%s decision=%s reason=%s\n",
                dns0.toString().c_str(),
                dns1.toString().c_str(),
                decision,
                why);
  if (dns_missing || !resolve_ok) {
    IPAddress new_dns1(1, 1, 1, 1);
    IPAddress new_dns2(8, 8, 8, 8);
    bool applied = WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, new_dns1, new_dns2);
    if (applied) {
      delay(100);
    }
    bool retry_ok = dns_resolve_test(host_a) && dns_resolve_test(host_b);
    (void)reason;
    return retry_ok;
  }
  (void)reason;
  return resolve_ok;
}

static void apply_public_dns_for_api(const char* reason) {
  (void)apply_dns_strategy(reason ? reason : "wifi", TREPO_API_HOST, awsEndpoint);
}

static bool ensure_dns_ready(const char* host) {
  if (!host || !host[0]) {
    return false;
  }
  return apply_dns_strategy("ensure_dns", host, NULL);
}

static bool wait_for_network_ready(const char* host, uint32_t timeout_ms) {
  const uint32_t STABLE_MS = 1000;
  unsigned long start = millis();
  unsigned long stable_start = 0;
  bool dns_ok = (host == NULL || !host[0]);
  bool dns_checked = (host == NULL || !host[0]);
  Serial.printf("[NET] waiting_ready timeout=%lu host=%s\n",
                (unsigned long)timeout_ms,
                host ? host : "(null)");
  while ((millis() - start) < timeout_ms) {
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    IPAddress ip = WiFi.localIP();
    bool ip_ok = (static_cast<uint32_t>(ip) != 0);
    if (!dns_checked && wifi_ok && ip_ok && host && host[0]) {
      dns_ok = ensure_dns_ready(host);
      dns_checked = true;
    }
    if (wifi_ok && ip_ok) {
      if (stable_start == 0) {
        stable_start = millis();
      }
      if ((millis() - stable_start) >= STABLE_MS) {
        if (dns_checked && !dns_ok) {
          Serial.printf("[NET] dns_error host=%s\n", host ? host : "(null)");
        }
        Serial.printf("[NET] ready wifi=1 ip=%s dns_ok=%d stable_ms=%lu\n",
                      ip.toString().c_str(),
                      dns_ok ? 1 : 0,
                      (unsigned long)(millis() - stable_start));
        return true;
      }
    } else {
      stable_start = 0;
    }
    delay(100);
  }
  IPAddress ip = WiFi.localIP();
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  bool ip_ok = (static_cast<uint32_t>(ip) != 0);
  if (!dns_checked && wifi_ok && ip_ok && host && host[0]) {
    dns_ok = ensure_dns_ready(host);
    dns_checked = true;
  }
  if (wifi_ok && ip_ok) {
    if (dns_checked && !dns_ok) {
      Serial.printf("[NET] dns_error host=%s\n", host ? host : "(null)");
    }
    Serial.printf("[NET] ready wifi=1 ip=%s dns_ok=%d stable_ms=%lu\n",
                  ip.toString().c_str(),
                  dns_ok ? 1 : 0,
                  (unsigned long)timeout_ms);
    return true;
  }
  Serial.printf("[NET] not_ready wifi=%d ip=%s dns_ok=%d\n",
                wifi_ok ? 1 : 0,
                ip.toString().c_str(),
                dns_ok ? 1 : 0);
  return false;
}

static void fetch_shopping_list_from_api() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("✗ Cannot fetch list: WiFi not connected!");
    list_refresh_fail("wifi_not_connected");
    return;
  }
  
  Serial.println("\n=== Fetching Shopping List from Trepo API ===");
  
  // Build JSON request body
  String request_body = "{";
  request_body += "\"operation\":\"view\",";
  request_body += "\"ownerId\":\"" + String(TREPO_OWNER_ID) + "\",";
  request_body += "\"device\":\"" + String(TREPO_DEVICE_ID) + "\"";
  request_body += "}";
  
  // Build full URL
  String list_url = String(TREPO_API_BASE_URL) + String(TREPO_LIST_ENDPOINT);
  String list_host;
  if (!extract_host_from_url(list_url, list_host)) {
    Serial.println("✗ List fetch: Failed to parse host from URL");
    list_refresh_fail("host_parse_fail");
    return;
  }
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  IPAddress ip = WiFi.localIP();
  bool ip_ok = (static_cast<uint32_t>(ip) != 0);
  if (!wifi_ok || !ip_ok) {
    Serial.println("[NET] not ready - skipping list fetch");
    list_refresh_fail("net_not_ready");
    return;
  }
  bool dns_ok = ensure_dns_ready(list_host.c_str());
  if (!dns_ok) {
    Serial.println("[NET] dns_error; proceeding with fetch");
  }
  IPAddress dns0 = WiFi.dnsIP(0);
  IPAddress dns1 = WiFi.dnsIP(1);
  Serial.printf("[NET_DIAG] fetch_attempt reason=%s wifi=%d ip=%s dns0=%s dns1=%s dns_ok=%d\n",
                g_last_refresh_reason,
                wifi_ok ? 1 : 0,
                ip.toString().c_str(),
                dns0.toString().c_str(),
                dns1.toString().c_str(),
                dns_ok ? 1 : 0);
  
  Serial.print("URL: ");
  Serial.println(list_url);
  Serial.print("Request body: ");
  Serial.println(request_body);

  auto do_list_request = [&](String& response_json,
                             String& err_str,
                             unsigned long& duration_ms,
                             bool& begin_ok,
                             int& tls_err,
                             String& tls_err_str) -> int {
    unsigned long start_ms = millis();
    WiFiClientSecure req_client;
    HTTPClient req_http;
    req_client.setInsecure();
    begin_ok = false;
    tls_err = 0;
    tls_err_str = "";
    if (!req_http.begin(req_client, list_url)) {
      err_str = "begin_failed";
      duration_ms = millis() - start_ms;
      return -1;
    }
    begin_ok = true;
    req_http.setTimeout(30000);
    req_http.setConnectTimeout(10000);
    req_http.addHeader("Content-Type", "application/json");
    int httpResponseCode = req_http.POST(request_body);
    err_str = req_http.errorToString(httpResponseCode);
    if (httpResponseCode <= 0) {
      char tls_err_buf[128] = {0};
      tls_err = req_client.lastError(tls_err_buf, sizeof(tls_err_buf));
      tls_err_str = String(tls_err_buf);
    }
    if (httpResponseCode > 0) {
      response_json = req_http.getString();
    }
    req_http.end();
    req_client.stop();
    duration_ms = millis() - start_ms;
    return httpResponseCode;
  };

  const int kMaxAttempts = 2;
  for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
    String response_json = "";
    String err_str = "";
    unsigned long request_duration_ms = 0;
    bool begin_ok = false;
    int tls_err = 0;
    String tls_err_str = "";
    int httpResponseCode =
        do_list_request(response_json, err_str, request_duration_ms, begin_ok, tls_err, tls_err_str);
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    if (httpResponseCode == 200) {
      Serial.print("Response length: ");
      Serial.println(response_json.length());
      
      if (response_json.length() > 0 && response_json.charAt(0) == '{') {
        Serial.println("✓ Response appears to be JSON");
        if (!parse_and_update_shopping_list(response_json)) {
          list_refresh_fail("json_parse");
        }
      } else {
        Serial.println("⚠ Response doesn't look like JSON");
        Serial.print("First 100 chars: ");
        Serial.println(response_json.substring(0, 100));
      }
      return;
    }
    if (httpResponseCode > 0) {
      Serial.printf("[NET] fetch_fail kind=HTTP code=%d errno=%d duration_ms=%lu\n",
                    httpResponseCode,
                    errno,
                    request_duration_ms);
      list_refresh_fail("http_error");
      return;
    }

    String err_lower = err_str;
    err_lower.toLowerCase();
    IPAddress dns_ip;
    bool dns_resolve_ok = WiFi.hostByName(list_host.c_str(), dns_ip);
    const char* fail_kind = "TLS";
    if (!dns_resolve_ok) {
      fail_kind = "DNS";
    } else if (httpResponseCode == HTTPC_ERROR_READ_TIMEOUT ||
               err_lower.indexOf("timeout") >= 0 ||
               err_lower.indexOf("timed out") >= 0 ||
               err_lower.indexOf("connect") >= 0) {
      fail_kind = "TIMEOUT";
    }
    Serial.printf("[NET] fetch_fail kind=%s code=%d errno=%d duration_ms=%lu\n",
                  fail_kind,
                  httpResponseCode,
                  errno,
                  request_duration_ms);
    Serial.printf("[NET] fetch_fail begin_ok=%d dns_ok=%d tls_err=%d tls_msg=%s\n",
                  begin_ok ? 1 : 0,
                  dns_resolve_ok ? 1 : 0,
                  tls_err,
                  tls_err_str.c_str());

    if (httpResponseCode == -1 && attempt == 0) {
      unsigned long backoff_ms = 500 + (unsigned long)random(0, 1001);
      Serial.printf("[HTTP] code=-1 retrying backoff_ms=%lu\n", backoff_ms);
      vTaskDelay(pdMS_TO_TICKS(backoff_ms));
      continue;
    }

    Serial.print("✗ HTTP request failed! Error: ");
    Serial.println(err_str);
    if (strcmp(fail_kind, "DNS") == 0) {
      list_refresh_fail("dns_error");
    } else if (strcmp(fail_kind, "TIMEOUT") == 0) {
      list_refresh_fail("timeout");
    } else {
      list_refresh_fail("network_fail");
    }
    return;
  }
}

// ── VOICE Operation Functions ──────────────────────────────────────
// Audio recording callback (called from I2S task when recording)
static void voice_audio_callback(const int16_t *samples, size_t num_samples) {
  if (!voice_recording_active || voice_audio_buffer == NULL) {
    return;
  }
  
  size_t bytes_to_write = num_samples * sizeof(int16_t);
  
  if (voice_audio_pos + bytes_to_write <= AUDIO_BUFFER_SIZE) {
    memcpy(voice_audio_buffer + voice_audio_pos, samples, bytes_to_write);
    voice_audio_pos += bytes_to_write;
    voice_audio_size = voice_audio_pos;
  } else {
    // Buffer overflow - stop recording
    Serial.println("[VOICE] Audio buffer overflow - stopping recording");
    voice_recording_active = false;
  }
}

// Resample audio from 16 kHz to 24 kHz using linear interpolation
// Input: 16-bit mono PCM at 16 kHz
// Output: 16-bit mono PCM at 24 kHz (raw, no WAV header)
static size_t resample_16k_to_24k(const int16_t* input_samples, size_t input_sample_count, int16_t* output_samples) {
  const float ratio = 16000.0f / 24000.0f;  // 0.6667 (upsampling)
  size_t output_sample_count = (size_t)(input_sample_count / ratio);
  
  for (size_t i = 0; i < output_sample_count; i++) {
    float src_index = i * ratio;
    size_t src_index_int = (size_t)src_index;
    float fraction = src_index - src_index_int;
    
    // Linear interpolation
    if (src_index_int + 1 < input_sample_count) {
      int16_t sample1 = input_samples[src_index_int];
      int16_t sample2 = input_samples[src_index_int + 1];
      output_samples[i] = (int16_t)(sample1 + (sample2 - sample1) * fraction);
    } else {
      // Last sample, no interpolation possible
      output_samples[i] = input_samples[src_index_int];
    }
  }
  
  return output_sample_count;
}

// Upload raw PCM audio to quick-ack API and return response JSON
// Quick-ack API expects: raw PCM at 24kHz, 16-bit, mono (no WAV header)
// Audio is recorded at 16 kHz, so we resample to 24 kHz
static String voice_upload_and_parse() {
  if (voice_audio_buffer == NULL || voice_audio_size == 0) {
    Serial.println("[VOICE] No audio data to upload");
    return "";
  }
  
  Serial.printf("[VOICE] Uploading %d bytes (raw PCM) to quick-ack API\n", voice_audio_size);
  Serial.printf("[VOICE] Input: %d bytes at 16 kHz, resampling to 24 kHz\n", voice_audio_size);
  
  // Convert to int16_t samples for resampling
  const int16_t* input_samples = (const int16_t*)voice_audio_buffer;
  size_t input_sample_count = voice_audio_size / sizeof(int16_t);
  
  // Calculate output size (16 kHz -> 24 kHz: 150% of original, upsampling)
  size_t output_sample_count = (size_t)(input_sample_count * 24000.0f / 16000.0f);
  size_t output_size = output_sample_count * sizeof(int16_t);
  
  Serial.printf("[VOICE] Output: %d samples (%d bytes) at 24 kHz\n", output_sample_count, output_size);
  
  // Allocate buffer for resampled PCM (in PSRAM if available)
  int16_t* resampled_pcm = NULL;
  if (ESP.getPsramSize() > 0) {
    resampled_pcm = (int16_t*)heap_caps_malloc(output_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (resampled_pcm != NULL) {
      Serial.println("[VOICE] Resampled PCM buffer allocated in PSRAM");
    }
  }
  
  // Fallback to internal heap if PSRAM not available or allocation failed
  if (resampled_pcm == NULL) {
    Serial.println("[VOICE] PSRAM allocation failed, trying internal heap");
    resampled_pcm = (int16_t*)malloc(output_size);
  }
  
  if (resampled_pcm == NULL) {
    Serial.println("[VOICE] Failed to allocate memory for resampled PCM!");
    return "";
  }
  
  // Resample from 16 kHz to 24 kHz
  size_t actual_output_count = resample_16k_to_24k(input_samples, input_sample_count, resampled_pcm);
  size_t actual_output_size = actual_output_count * sizeof(int16_t);
  
  Serial.printf("[VOICE] Resampled: %d bytes (24 kHz raw PCM, no WAV header)\n", actual_output_size);
  
  // Upload raw PCM to quick-ack API (no WAV header)
  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();
  
  String quick_ack_url = String(QUICK_ACK_BASE_URL) + String(QUICK_ACK_ENDPOINT);
  http.begin(client, quick_ack_url);
  http.setTimeout(60000);
  http.setConnectTimeout(10000);
  http.addHeader("Content-Type", "audio/pcm");  // Raw PCM, not WAV
  http.addHeader("x-owner-id", TREPO_OWNER_ID);
  http.addHeader("x-device-id", TREPO_DEVICE_ID);
  
  Serial.print("[VOICE] Uploading to: ");
  Serial.println(quick_ack_url);
  Serial.printf("[VOICE] PCM size: %d bytes (24 kHz, 16-bit LE, mono)\n", actual_output_size);
  
  int httpResponseCode = http.POST((uint8_t*)resampled_pcm, actual_output_size);
  
  // Free resampled PCM buffer
  if (ESP.getPsramSize() > 0 && resampled_pcm != NULL) {
    heap_caps_free(resampled_pcm);
  } else if (resampled_pcm != NULL) {
    free(resampled_pcm);
  }
  Serial.printf("[VOICE] HTTP Response: %d\n", httpResponseCode);
  
  String response_json = "";
  if (httpResponseCode == 200) {
    response_json = http.getString();
    Serial.printf("[VOICE] Response length: %d\n", response_json.length());
  } else {
    Serial.printf("[VOICE] Upload failed: %d\n", httpResponseCode);
    if (httpResponseCode > 0) {
      String error_response = http.getString();
      Serial.printf("[VOICE] Error response: %s\n", error_response.c_str());
    }
  }
  
  http.end();
  client.stop();
  
  return response_json;
}

// ── Camera Functions ────────────────────────────────────────────────
static bool init_camera() {
  Serial.println("[CAMERA] Initializing camera...");
  
  Serial.printf("[CAMERA] Free heap before camera init: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("[CAMERA] PSRAM size: %d bytes\n", ESP.getPsramSize());
  Serial.printf("[CAMERA] Free PSRAM: %d bytes\n", ESP.getFreePsram());
  
  if (FILL_LED_PIN >= 0) {
    pinMode(FILL_LED_PIN, OUTPUT);
    digitalWrite(FILL_LED_PIN, LOW);
  }

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;  // 20MHz
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = CAPTURE_SIZE;
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAMERA] Camera init failed: 0x%x\n", err);
    Serial.printf("[CAMERA] Error details: %s\n", esp_err_to_name(err));
    return false;
  }

  Serial.printf("[CAMERA] Free heap after camera init: %d bytes\n", ESP.getFreeHeap());
  Serial.println("[CAMERA] Camera initialized successfully");
  tune_sensor_for_food();
  return true;
}

static void deinit_camera() {
  Serial.println("[CAMERA] De-initializing camera to save power...");
  // Suppress benign GDMA disconnect error during camera deinit
  esp_log_level_set("gdma", ESP_LOG_NONE);
  esp_camera_deinit();
  esp_log_level_set("gdma", ESP_LOG_ERROR);
  Serial.printf("[CAMERA] Free heap after camera deinit: %d bytes\n", ESP.getFreeHeap());
  Serial.println("[CAMERA] Camera de-initialized successfully");
}

static void tune_sensor_for_food() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  // Ensure automatics are on
  s->set_exposure_ctrl(s, 1);   // AEC on
  s->set_gain_ctrl(s, 1);       // AGC on
  s->set_whitebal(s, 1);        // AWB on
  s->set_awb_gain(s, 1);        // AWB gain on

  // Brighter default; allow higher analog gain for low light
  s->set_ae_level(s, 2);        // -2..2 (2 = brighter)
  s->set_gainceiling(s, GAINCEILING_64X);

  // Gentle image tweaks
  s->set_brightness(s, 1);      // -2..2
  s->set_contrast(s, 0);        // -2..2
  s->set_saturation(s, 0);      // -2..2
  s->set_lenc(s, 1);            // lens correction on

  // Orientation sane defaults
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);

  // Keep framesize/quality in sync with globals
  s->set_framesize(s, CAPTURE_SIZE);
  s->set_quality(s, JPEG_QUALITY);
}

static bool warmup_and_capture(camera_fb_t*& fb) {
  Serial.println("[CAMERA] Starting warmup and capture...");
  Serial.printf("[CAMERA] Free heap: %d bytes, Free PSRAM: %d bytes\n", 
                ESP.getFreeHeap(), ESP.getFreePsram());
  
  // Optional short fill light
  if (FILL_LED_PIN >= 0) {
    digitalWrite(FILL_LED_PIN, HIGH);
  }

  // Let AEC/AWB settle: grab & discard frames
  camera_fb_t* tmp = nullptr;
  int warmup_success = 0;
  for (int i = 0; i < 5; ++i) {
    tmp = esp_camera_fb_get();
    if (tmp) {
      warmup_success++;
      Serial.printf("[CAMERA] Warmup frame %d: %u bytes\n", i+1, tmp->len);
      esp_camera_fb_return(tmp);
      tmp = nullptr;
    } else {
      Serial.printf("[CAMERA] WARNING: Warmup frame %d failed\n", i+1);
    }
    delay(40); // small settle delay
  }
  
  Serial.printf("[CAMERA] Warmup complete: %d/5 frames captured\n", warmup_success);

  // Final capture
  Serial.println("[CAMERA] Attempting final capture...");
  fb = esp_camera_fb_get();

  // Turn off fill LED
  if (FILL_LED_PIN >= 0) {
    delay(FILL_LED_MS);
    digitalWrite(FILL_LED_PIN, LOW);
  }

  if (!fb) {
    Serial.println("[CAMERA] ERROR: Final capture returned NULL");
    return false;
  }
  
  Serial.printf("[CAMERA] SUCCESS: Captured %u bytes (%dx%d)\n", 
                fb->len, fb->width, fb->height);
  return true;
}

// ── Presign Functions ──────────────────────────────────────────────
static bool do_presign_request(const char* url, PresignReply& out, int& http_code, String& resp_body) {
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["user_id"]   = USER_ID;
  doc["type"]      = current_job_type;
  String body; 
  serializeJson(doc, body);
  
  Serial.printf("[PRESIGN] Using job_type: %s\n", current_job_type.c_str());
  Serial.println("[PRESIGN] POST " + String(url));
  if (!http_post_json_with_retries(url, body, http_code, resp_body, "PRESIGN", API_KEY, BEARER_TOKEN)) {
    Serial.printf("[PRESIGN] Request failed with code %d\n", http_code);
    return false;
  }
  Serial.printf("[PRESIGN] HTTP %d\n", http_code);
  if (resp_body.length()) Serial.println("[PRESIGN] Body: " + resp_body);

  StaticJsonDocument<768> r;
  auto err = deserializeJson(r, resp_body);
  if (err) { 
    Serial.print("[PRESIGN] JSON parse error: "); 
    Serial.println(err.c_str()); 
    return false; 
  }

  out.job_id       = r["job_id"].as<String>();
  out.put_url      = r["put_url"].as<String>();
  out.s3_key       = r["s3_key"].as<String>();
  out.content_type = r["content_type"].as<String>();
  out.ttl_s        = r["ttl_s"] | 0;

  return !(out.job_id.isEmpty() || out.put_url.isEmpty());
}

static bool get_presign(PresignReply& out) {
  int code = 0; 
  String body;
  if (do_presign_request(PRESIGN_ENDPOINT_PRIMARY, out, code, body)) {
    Serial.println("[PRESIGN] Presign OK (primary)");
    return true;
  }
  Serial.printf("[PRESIGN] Presign primary failed: %d\n", code);

  if (PRESIGN_ENDPOINT_FALLBACK && PRESIGN_ENDPOINT_FALLBACK[0]) {
    if (do_presign_request(PRESIGN_ENDPOINT_FALLBACK, out, code, body)) {
      Serial.println("[PRESIGN] Presign OK (fallback)");
      return true;
    }
    Serial.printf("[PRESIGN] Presign fallback failed: %d\n", code);
  }
  return false;
}

static bool net_ready_for_tls(char* why, size_t why_len) {
  if (why && why_len > 0) {
    why[0] = '\0';
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (why && why_len > 0) {
      snprintf(why, why_len, "wifi_disconnected");
    }
    return false;
  }
  IPAddress ip = WiFi.localIP();
  if ((uint32_t)ip == 0) {
    if (why && why_len > 0) {
      snprintf(why, why_len, "no_ip");
    }
    return false;
  }
  time_t now = time(nullptr);
  if (now < 1700000000) {
    if (why && why_len > 0) {
      snprintf(why, why_len, "time_unsynced");
    }
    return false;
  }
  return true;
}

static void scan_terminal_reset() {
  scan_terminal_sent = false;
}

static void scan_send_terminal_status(const char* phase, const char* text, const char* mode) {
  if (scan_terminal_sent) {
    return;
  }
  scan_terminal_sent = true;
  uart_send_ui_status_extended("SCAN", phase, text ? text : "", mode);
}

static void presign_set_error_text(const char* text) {
  if (!text || !text[0]) {
    presign_last_error_text[0] = '\0';
    return;
  }
  strncpy(presign_last_error_text, text, sizeof(presign_last_error_text) - 1);
  presign_last_error_text[sizeof(presign_last_error_text) - 1] = '\0';
}

static const char* presign_error_text() {
  return presign_last_error_text[0] ? presign_last_error_text : "Presign failed (net). Tap to retry.";
}

static void tls_configure(WiFiClientSecure& client, const char* reason) {
#if HAS_CRT_BUNDLE
  client.setCACertBundle(esp_crt_bundle_attach);
  Serial.printf("[TLS] using crt bundle reason=%s\n", reason ? reason : "unknown");
#else
  client.setCACert(rootCA);
  Serial.printf("[TLS] using root CA reason=%s\n", reason ? reason : "unknown");
#endif
  client.setTimeout(15000);
}

static void log_wifi_snapshot(const char* label) {
  IPAddress ip = WiFi.localIP();
  IPAddress gw = WiFi.gatewayIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress dns1 = WiFi.dnsIP(0);
  IPAddress dns2 = WiFi.dnsIP(1);
  int32_t rssi = WiFi.RSSI();
  Serial.printf("[WIFI] %s status=%d rssi=%ld ip=%s gw=%s mask=%s dns1=%s dns2=%s\n",
                label ? label : "snapshot",
                (int)WiFi.status(),
                (long)rssi,
                ip.toString().c_str(),
                gw.toString().c_str(),
                mask.toString().c_str(),
                dns1.toString().c_str(),
                dns2.toString().c_str());
}

static void log_http_failure_details(const char* label, const char* url, int http_code, WiFiClientSecure* client) {
  const bool is_https = url && (strncmp(url, "https://", 8) == 0);
  char tls_err_msg[128] = {0};
  int tls_err = 0;
  if (client) {
    tls_err = client->lastError(tls_err_msg, sizeof(tls_err_msg));
  }
  Serial.printf("[HTTP_FAIL] label=%s code=%d err=%s errno=%d tls_err=%d tls_msg=%s url=%s https=%d heap=%u\n",
                label ? label : "http",
                http_code,
                HTTPClient::errorToString(http_code).c_str(),
                errno,
                tls_err,
                tls_err_msg,
                url ? url : "",
                is_https ? 1 : 0,
                (unsigned)ESP.getFreeHeap());
  log_wifi_snapshot(label);
}

static bool ensure_time_valid(const char* reason) {
  time_t now = time(nullptr);
  if (now >= 1700000000) {
    return true;
  }
  if (!sntp_started) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    sntp_started = true;
    Serial.printf("[SNTP] start reason=%s\n", reason ? reason : "unknown");
  }
  unsigned long start = millis();
  const unsigned long wait_ms = 10000;
  while ((millis() - start) < wait_ms) {
    now = time(nullptr);
    if (now >= 1700000000) {
      Serial.printf("[SNTP] synced epoch=%ld\n", (long)now);
      return true;
    }
    delay(200);
  }
  Serial.printf("[SNTP] sync_timeout epoch=%ld\n", (long)now);
  return false;
}

static bool ensure_wifi_ready(const char* reason, uint32_t timeout_ms) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (ensure_wifi_connected(reason, timeout_ms)) {
      log_wifi_snapshot("wifi_ready");
      return true;
    }
    delay(200);
  }
  return false;
}

static bool wifi_hard_reset_and_reconnect(const char* reason, uint32_t timeout_ms) {
  const char* ssid = WIFI_SSID;
  const char* pass = WIFI_PASS;
#ifdef HALO_SENSE_PROD_WRAPPER
  char provision_ssid[64];
  char provision_pass[64];
  if (halo_get_provisioned_wifi(provision_ssid, sizeof(provision_ssid),
                                provision_pass, sizeof(provision_pass))) {
    ssid = provision_ssid;
    pass = provision_pass;
  }
#endif
  if (!ssid || !ssid[0]) {
    Serial.println("[WIFI_RECOVER] no_ssid");
    return false;
  }
  Serial.printf("[WIFI_RECOVER] hard_reset reason=%s ssid=%s\n",
                reason ? reason : "unknown",
                ssid);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(200);
  WiFi.begin(ssid, pass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    log_wifi_snapshot("wifi_recovered");
    return true;
  }
  Serial.printf("[WIFI_RECOVER] failed status=%d\n", (int)WiFi.status());
  return false;
}

static void wifi_recover_if_needed(const char* reason, int http_code) {
  if (http_code >= 0) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    ensure_wifi_ready(reason ? reason : "http_fail", 15000);
    return;
  }
  wifi_hard_reset_and_reconnect(reason ? reason : "http_fail", 15000);
}

static bool http_post_json_with_retries(const char* url, const String& body, int& http_code, String& resp_body, const char* label, const char* api_key, const char* bearer) {
  static const unsigned long backoff_ms[] = {500, 1500, 3500};
  const int max_attempts = 3;
  presign_set_error_text("");
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    Serial.printf("[%s] attempt=%d/%d url=%s\n",
                  label ? label : "HTTP",
                  attempt + 1,
                  max_attempts,
                  url ? url : "");
    if (!ensure_wifi_ready("http_ready", 15000)) {
      presign_set_error_text("Wi-Fi not ready");
      log_wifi_snapshot("wifi_not_ready");
      delay(backoff_ms[attempt]);
      continue;
    }
    if (!ensure_time_valid("http_ready")) {
      presign_set_error_text("Time not set");
      log_wifi_snapshot("time_not_ready");
      delay(backoff_ms[attempt]);
      continue;
    }
    WiFiClientSecure client;
    tls_configure(client, label);
    HTTPClient http;
    if (!http.begin(client, url)) {
      http_code = -1;
      resp_body = "";
      presign_set_error_text("Presign failed (net). Tap to retry.");
      log_http_failure_details(label, url, http_code, &client);
      wifi_recover_if_needed("http_begin", http_code);
      delay(backoff_ms[attempt]);
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    if (api_key && api_key[0]) {
      http.addHeader("x-api-key", api_key);
    }
    if (bearer && bearer[0]) {
      String auth = String("Bearer ") + bearer;
      http.addHeader("Authorization", auth);
    }
    http_code = http.POST(body);
    resp_body = http.getString();
    http.end();
    if (http_code >= 200 && http_code < 300) {
      return true;
    }
    presign_set_error_text("Presign failed (net). Tap to retry.");
    log_http_failure_details(label, url, http_code, &client);
    wifi_recover_if_needed("http_post", http_code);
    delay(backoff_ms[attempt]);
  }
  return false;
}

// ── Check-in Presign Function (Grocery Recognition API) ───────
static bool get_presign_checkin(PresignReply& out, const char* expiry_date = NULL) {
  String presign_url = String(CHECKIN_API_BASE_URL) + String(CHECKIN_PRESIGN_ENDPOINT);

  // Build request body for check-in API
  StaticJsonDocument<256> doc;
  doc["user_id"] = USER_ID;
  doc["device_id"] = DEVICE_ID;
  doc["owner"] = CHECKIN_OWNER_ID;  // Required UUID
  doc["action"] = "IN";  // "IN" for adding items to inventory
  doc["type"] = "grocery";
  doc["content_type"] = "image/jpeg";
  
  // Add expiration date if provided
  if (expiry_date != NULL && strlen(expiry_date) > 0) {
    doc["product_expiration"] = expiry_date;
    Serial.printf("[CHECKIN_PRESIGN] Including expiration date: %s\n", expiry_date);
  }
  
  String body; 
  serializeJson(doc, body);
  
  Serial.println("[CHECKIN_PRESIGN] POST " + presign_url);
  Serial.println("[CHECKIN_PRESIGN] Body: " + body);
  
  int http_code = 0;
  String resp_body;
  if (!http_post_json_with_retries(presign_url.c_str(), body, http_code, resp_body, "CHECKIN_PRESIGN", NULL, NULL)) {
    Serial.printf("[CHECKIN_PRESIGN] Request failed with code %d\n", http_code);
    return false;
  }
  Serial.printf("[CHECKIN_PRESIGN] HTTP %d\n", http_code);
  if (resp_body.length()) Serial.println("[CHECKIN_PRESIGN] Body: " + resp_body);

  StaticJsonDocument<768> r;
  auto err = deserializeJson(r, resp_body);
  if (err) { 
    Serial.print("[CHECKIN_PRESIGN] JSON parse error: "); 
    Serial.println(err.c_str()); 
    return false; 
  }

  out.job_id       = r["job_id"].as<String>();
  out.put_url      = r["put_url"].as<String>();
  out.s3_key       = r["s3_key"].as<String>();
  out.content_type = "image/jpeg";  // Default for check-in
  out.ttl_s        = r["expires_in"] | 300;  // API returns "expires_in", default 300s

  bool success = !(out.job_id.isEmpty() || out.put_url.isEmpty());
  if (success) {
    Serial.println("[CHECKIN_PRESIGN] Presign OK");
  } else {
    Serial.println("[CHECKIN_PRESIGN] Presign failed - missing fields");
  }
  return success;
}

static bool put_to_presigned_url(const String& url, const uint8_t* buf, size_t len, const char* contentType) {
  Serial.printf("[UPLOAD] Starting PUT to S3, size: %u bytes\n", len);
  
  WiFiClientSecure tls; 
  tls.setInsecure();
  tls.setTimeout(60000);  // 60 second timeout for large uploads
  
  HTTPClient http;
  if (!http.begin(tls, url)) {
    Serial.println("[UPLOAD] HTTP begin() failed for PUT");
    return false;
  }
  
  // Set timeouts for large uploads
  http.setTimeout(60000);  // 60 seconds total timeout
  http.setConnectTimeout(15000);  // 15 seconds connection timeout
  
  http.addHeader("Content-Type", (contentType && *contentType) ? contentType : "image/jpeg");

  Serial.println("[UPLOAD] Sending PUT request...");
  unsigned long upload_start = millis();

  int code = http.sendRequest("PUT", (uint8_t*)buf, len);
  
  unsigned long upload_duration = millis() - upload_start;
  Serial.printf("[UPLOAD] PUT completed in %lu ms\n", upload_duration);
  
  String resp = http.getString();
  http.end();

  Serial.printf("[UPLOAD] PUT status: %d\n", code);
  
  if (code < 0) {
    Serial.printf("[UPLOAD] HTTPClient error code: %d\n", code);
  }
  
  if (resp.length()) {
    Serial.print("[UPLOAD] PUT response: ");
    Serial.println(resp);
  }
  
  bool success = (code >= 200 && code < 300);
  if (success) {
    Serial.println("[UPLOAD] ✓ Upload successful");
  } else {
    Serial.println("[UPLOAD] ✗ Upload failed");
  }
  
  return success;
}

// ── MQTT Functions ────────────────────────────────────────────────
static bool connect_to_mqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[MQTT] WiFi not connected for MQTT");
    return false;
  }
  
  wifiClient.setCACert(rootCA);
  wifiClient.setCertificate(deviceCert);
  wifiClient.setPrivateKey(privateKey);
  wifiClient.setTimeout(15000);
  
  mqttClient.setServer(awsEndpoint, 8883);
  mqttClient.setBufferSize(12000);
  mqttClient.setKeepAlive(30);
  mqttClient.setCallback(on_mqtt_message);
  
  const String clientId = "ESP32Camera_" + String(DEVICE_ID);
  
  if (!mqttClient.connect(clientId.c_str())) {
    Serial.printf("[MQTT] Connection failed, state: %d\n", mqttClient.state());
    return false;
  }
  
  Serial.println("[MQTT] Connected");
  return true;
}

static void mqtt_ensure_connected() {
  if (mqttClient.connected()) return;

  // Set creds and connection params
  wifiClient.setCACert(rootCA);
  wifiClient.setCertificate(deviceCert);
  wifiClient.setPrivateKey(privateKey);
  wifiClient.setTimeout(15000);

  mqttClient.setServer(awsEndpoint, 8883);
  mqttClient.setBufferSize(12000);
  mqttClient.setKeepAlive(30);
  mqttClient.setCallback(on_mqtt_message);

  String clientId = "ESP32Camera_" + String(DEVICE_ID);
  if (mqttClient.connect(clientId.c_str())) {
    mqtt_subscribed = mqttClient.subscribe(RESULT_TOPIC);
    bool ota_subscribed = mqttClient.subscribe(OTA_TOPIC);
    Serial.printf("[MQTT] Connected, subscribed=%d to %s\n", mqtt_subscribed, RESULT_TOPIC);
    Serial.printf("[MQTT] OTA subscribed=%d to %s\n", ota_subscribed, OTA_TOPIC);
  } else {
    Serial.printf("[MQTT] Reconnect failed, state=%d\n", mqttClient.state());
  }
}

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
  if (!waiting_for_mqtt_result || current_scan_job_id.length() == 0 || 
      strcmp(job_id, current_scan_job_id.c_str()) != 0) {
    Serial.printf("[MQTT] Ignoring: waiting_for=%d, wanted=%s, got=%s\n",
                  waiting_for_mqtt_result, current_scan_job_id.c_str(), job_id);
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
                      kcal, protein_g, carbs_g, fat_g, health_score, current_job.mode);
        // Only send meal result for dish mode (discard mode doesn't need nutrition info)
        // But we still send it with mode so LCD can decide whether to display
        uart_send_ui_meal_result(kcal, meal_summary, health_score, recommendation, current_job.mode, protein_g, carbs_g, fat_g, confidence);
      }
    }
    // Fallback to "message" field if meal_summary not available
    else if (result.containsKey("message")) {
      const char* msg = result["message"];
      if (msg != NULL && strlen(msg) > 0) {
        uart_send_ui_toast(msg);
      }
    }
  }

  // Clear waiting state
  waiting_for_mqtt_result = false;
  mqtt_wait_deadline = 0;
  current_scan_job_id = "";
  
  Serial.println("[MQTT] SCAN result processed");
}

static void uart_send_ui_meal_result(int kcal, const char* meal_summary, int health_score, const char* recommendation, const char* mode, float protein_g, float carbs_g, float fat_g, float confidence) {
  StaticJsonDocument<1024> doc;  // Increased size for meal_summary
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "UI_MEAL_RESULT";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["calories"] = kcal;
  doc["protein_g"] = protein_g;
  doc["carbs_g"] = carbs_g;
  doc["fat_g"] = fat_g;
  doc["confidence"] = confidence;
  if (meal_summary != NULL && strlen(meal_summary) > 0) {
    doc["meal_summary"] = meal_summary;
  }
  if (recommendation != NULL && strlen(recommendation) > 0) {
    doc["recommendation"] = recommendation;
  }
  if (mode != NULL && strlen(mode) > 0) {
    doc["mode"] = mode;  // "dish" or "discard" - only send meal result for dish mode
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

// ── UART Message Parsing ───────────────────────────────────────────
static bool parse_input_message(const char* json_str) {
  // Skip empty strings
  if (json_str == NULL || strlen(json_str) == 0) {
    return false;
  }
  size_t json_len = strlen(json_str);
  
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, json_str);
  
  if (error) {
    char head[17];
    char tail[17];
    size_t head_len = json_len < 16 ? json_len : 16;
    memcpy(head, json_str, head_len);
    head[head_len] = '\0';
    size_t tail_len = json_len < 16 ? json_len : 16;
    const char* tail_start = json_len > tail_len ? (json_str + json_len - tail_len) : json_str;
    memcpy(tail, tail_start, tail_len);
    tail[tail_len] = '\0';
    Serial.printf("[PROTO] json_parse_fail len=%u head=\"%s\" tail=\"%s\"\n",
                  (unsigned)json_len, head, tail);
    return false;
  }
  
  if (!validate_protocol_message(doc)) {
    return false;  // Validation failed, message dropped
  }
  
  // Update last communication time (any message from LCD resets the timeout)
  last_lcd_communication = millis();
  last_uart_rx_ms = last_lcd_communication;
  
  const char* type = doc["type"] | "";
  Serial.printf("[PROTO] RX: type=%s\n", type);
#ifdef HALO_SENSE_PROD_WRAPPER
  if (strncmp(type, "INPUT_", 6) == 0) {
    last_user_activity_ms = millis();
  }
#else
  if (strncmp(type, "INPUT_", 6) == 0) {
    last_user_activity_ms = millis();
  }
#endif
#ifdef HALO_SENSE_PROD_WRAPPER
  halo_prod_on_lcd_message(type);
#endif
  
  if (strcmp(type, "INPUT_WAKE") == 0) {
    unsigned long now_ms = millis();
    if (last_input_wake_ms > 0 && (now_ms - last_input_wake_ms) < 1500) {
      Serial.printf("[INPUT_WAKE] ignored debounce age_ms=%lu\n",
                    (unsigned long)(now_ms - last_input_wake_ms));
      return true;
    }
    last_input_wake_ms = now_ms;
    sleep_holdoff_until_ms = now_ms + SLEEP_HOLDOFF_MS;
    Serial.printf("[SLEEP_HOLDOFF] set until=%lu reason=input_wake\n", sleep_holdoff_until_ms);
    sleep_grace_until_ms = now_ms + MIN_AWAKE_BEFORE_SLEEP_MS;
    wake_requested = true;
    Serial.printf("[WAKE_GRACE] set until=%lu reason=input_wake\n", sleep_grace_until_ms);
    if (list_refresh_inflight) {
      Serial.println("[INPUT_WAKE] ignored inflight");
      return true;
    }
    Serial.println("[INPUT_WAKE] accepted");
    Serial.println("[UART] LCD wake-up detected - requesting list refresh...");

    // Send proof-of-life immediately (before any network work)
    uart_send_ui_status("Refreshing...");
    last_wake_ms = now_ms;
    request_list_refresh("input_wake", false);
  } else if (strcmp(type, "INPUT_SCROLL") == 0) {
    // Scroll event from LCD - acknowledge but don't process (LCD handles selection locally)
    // This prevents "Unknown type" errors
    int delta = doc["delta"] | 0;
    Serial.printf("[UART] Scroll event received (delta=%d) - LCD handles selection locally\n", delta);
  } else if (strcmp(type, "INPUT_TOUCH") == 0) {
    // Brief touch event from LCD
    Serial.println("[UART] Touch event received from LCD");
    // TODO: Handle touch event (e.g., toggle item selection, open menu, etc.)
  } else if (strcmp(type, "INPUT_LONG_PRESS_START") == 0) {
    // Long press start event from LCD (user started holding)
    Serial.println("[UART] Long press START received - queuing VOICE operation");
    if (op_queue != NULL) {
      OpJob job = {OP_VOICE, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, "", ""};
      if (xQueueSend(op_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
        Serial.println("[OP] VOICE job queued");
      } else {
        Serial.println("[OP] Failed to queue VOICE job (queue full?)");
      }
    }
  } else if (strcmp(type, "INPUT_LONG_PRESS_END") == 0) {
    // Long press end event from LCD (user released after long press)
    Serial.println("[UART] Long press END received - signaling VOICE to finalize");
    // TODO: Signal current VOICE job to transition from RECORDING to FINALIZE
    if (current_job.type == OP_VOICE && current_job.state == OP_RECORDING) {
      current_job.state = OP_FINALIZE;
      Serial.println("[OP] VOICE job signaled to finalize recording");
    }
  } else if (strcmp(type, "INPUT_DELETE") == 0) {
    // Delete item request from LCD
    const char* item_id = doc["id"] | "";
    if (item_id != NULL && strlen(item_id) > 0) {
      strncpy(delete_item_id, item_id, sizeof(delete_item_id) - 1);
      delete_item_id[sizeof(delete_item_id) - 1] = '\0';
      delete_requested = true;
      Serial.printf("[UART] Delete requested for item ID: %s\n", delete_item_id);
    } else {
      Serial.println("[UART] Delete request missing item ID!");
    }
  } else if (strcmp(type, "INPUT_MENU_PRESS") == 0) {
    // Menu button pressed on LCD (legacy - defaults to dish mode)
    Serial.println("[UART] Menu button pressed - queuing SCAN operation (dish mode)");
    if (op_queue != NULL) {
      OpJob job = {OP_SCAN, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, ""};
      strncpy(job.mode, "dish", sizeof(job.mode) - 1);
      if (xQueueSend(op_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
        Serial.println("[OP] SCAN job queued (dish mode)");
      } else {
        Serial.println("[OP] Failed to queue SCAN job (queue full?)");
      }
    }
  } else if (strcmp(type, "INPUT_SLEEP") == 0) {
    // LCD is going to sleep - Sense should also sleep
    uint32_t msg_id = doc["msg_id"] | 0;
    unsigned long now_ms = millis();
    Serial.printf("[SLEEP_PROTO] rx INPUT_SLEEP -> ack msg_id=%u\n", (unsigned)msg_id);
    uart_send_input_sleep_ack("OK", "received");
    Serial.println("[SLEEP_PROTO] tx INPUT_SLEEP_ACK");
    const char* block_reason = NULL;
    const char* block_op = "none";
    bool blocked = sleep_block_active(now_ms, &block_reason, &block_op);
    const char* deny_reason = NULL;
#ifdef HALO_SENSE_PROD_WRAPPER
    bool ota_busy = false;
    bool mqtt_busy = false;
    bool time_invalid = false;
    bool ota_check_busy = false;
    if (halo_prod_should_defer_sleep_ack(&ota_busy, &mqtt_busy, &time_invalid, &ota_check_busy)) {
      if (ota_busy || ota_check_busy) {
        deny_reason = "ota_pending";
      } else if (mqtt_busy) {
        deny_reason = "mqtt_pending";
      } else if (time_invalid) {
        deny_reason = "cooldown";
      }
    }
#endif
    if (!deny_reason && blocked) {
      if (block_reason && strcmp(block_reason, "grace_window") == 0) {
        deny_reason = "cooldown";
      } else {
        deny_reason = "op_inflight";
      }
    }
    if (!deny_reason) {
      wake_pin_configure_rtc_input_inactive_pull();
      int wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      if (wake_pin_is_active_level(wake_pin_level)) {
        Serial.printf("[SLEEP_SANITY] wake_pin_active_at_input_sleep level=%d wake_active=%d\n",
                      wake_pin_level,
                      WAKE_ACTIVE_LEVEL);
        uart_send_release_wake();
        deny_reason = "wake_pin_active";
      }
    }
    Serial.printf("[WAKE_PIN] gpio2=%d phase=input_sleep\n",
                  rtc_gpio_get_level(WAKE_GPIO));

    bool new_request = (sleep_sm_state == SLEEP_SM_IDLE);
    sleep_requested = true;
    sleep_ack_pending = false;
    sleep_coord_requested = true;
    if (new_request) {
      sleep_ack_sent_for_request = false;
      sleep_ready_sent_for_cycle = false;
      sleep_deny_sent_for_request = false;
      sleep_sm_msg_id = msg_id;
      sleep_sm_transition(SLEEP_SM_REQ_RX, "INPUT_SLEEP", msg_id);
    }
    if (sleep_request_ms == 0) {
      sleep_request_ms = now_ms;
    }
    uart_send_sleep_ack();
    Serial.printf("[SLEEP_PROTO] tx SLEEP_ACK reason=accepted msg_id=%u\n", (unsigned)sleep_sm_msg_id);
    sleep_ack_sent_for_request = true;
    sleep_sm_transition(SLEEP_SM_ACK_SENT, "ACK_IMMEDIATE", sleep_sm_msg_id);
    if (deny_reason) {
      uint32_t retry_ms = sleep_deny_retry_ms(deny_reason, now_ms);
      uart_send_sleep_deny(deny_reason, retry_ms);
      Serial.printf("[SLEEP_PROTO] tx SLEEP_DENY reason=%s retry_ms=%lu\n",
                    deny_reason,
                    (unsigned long)retry_ms);
      sleep_deny_sent_for_request = true;
      sleep_requested = false;
      sleep_request_ms = 0;
      sleep_coord_requested = false;
      sleep_sm_transition(SLEEP_SM_IDLE, "DENY_IMMEDIATE", sleep_sm_msg_id);
      sleep_sm_msg_id = 0;
      return true;
    }
    Serial.println("[SLEEP] INPUT_SLEEP -> coordinated pre_sleep starting");
    sleep_request_logged = true;
  } else if (strcmp(type, "INPUT_WAKE") == 0) {
    // LCD is waking Sense - reset wake timer
    last_wake_ms = millis();
  } else if (strcmp(type, "INPUT_PING") == 0) {
    // LCD heartbeat to keep Sense awake
    Serial.println("[UART] INPUT_PING received - sync heartbeat");
    last_link_hb_rx_ms = millis();
    uart_send_pong();
  } else if (strcmp(type, "LINK_HB") == 0) {
    last_link_hb_rx_ms = millis();
    Serial.println("[UART] LINK_HB received");
  } else if (strcmp(type, "WIFI_CREDS_ACK") == 0) {
    const char* status = doc["status"] | "OK";
    int err_code = doc["err_code"] | 0;
    Serial.printf("[UART] WIFI_CREDS_ACK received status=%s err_code=%d\n",
                  status, err_code);
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_on_lcd_wifi_creds_ack(status, err_code);
#endif
  } else if (strcmp(type, "WIFI_ON_ACK") == 0) {
    Serial.println("[UART] WIFI_ON_ACK received");
    sleep_block_wifi_on_until_ms = 0;
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_on_lcd_wifi_on_ack();
#endif
  } else if (strcmp(type, "INPUT_RESET_WIFI") == 0) {
    Serial.println("[UART] INPUT_RESET_WIFI received - resetting Wi-Fi credentials...");
    reset_wifi_requested = true;
    uart_send_ui_status("Resetting Wi-Fi...");
  } else if (strcmp(type, "INPUT_MENU_SELECT") == 0) {
    // Menu item selected on LCD
    const char* menu_item = doc["menu_item"] | "";
    int menu_index = doc["menu_index"] | -1;
    Serial.printf("[UART] Menu item selected: %s (index %d)\n", menu_item, menu_index);
    
    // Handle "Dish" selection - trigger SCAN operation (meal nutrition)
    if (strcmp(menu_item, "Dish") == 0) {
      Serial.println("[UART] Dish selected - queuing SCAN operation for meal nutrition");
      if (op_queue != NULL) {
        OpJob job = {OP_SCAN, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, "", ""};
        strncpy(job.mode, "dish", sizeof(job.mode) - 1);
        job.expiry_date[0] = '\0';
        if (xQueueSend(op_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
          Serial.println("[OP] SCAN job queued for meal nutrition (dish mode)");
        } else {
          Serial.println("[OP] Failed to queue SCAN job (queue full?)");
        }
      }
    }
    // Handle "Discard" selection - trigger SCAN operation (discard, no nutrition display)
    else if (strcmp(menu_item, "Discard") == 0) {
      Serial.println("[UART] Discard selected - queuing SCAN operation for discard");
      if (op_queue != NULL) {
        OpJob job = {OP_SCAN, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, "", ""};
        strncpy(job.mode, "discard", sizeof(job.mode) - 1);
        job.expiry_date[0] = '\0';
        if (xQueueSend(op_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
          Serial.println("[OP] SCAN job queued for discard (discard mode)");
        } else {
          Serial.println("[OP] Failed to queue SCAN job (queue full?)");
        }
      }
    }
    // Handle "Check-in" selection - trigger SCAN operation (check-in, uses grocery recognition API)
    else if (strcmp(menu_item, "Check-in") == 0) {
      Serial.println("[UART] Check-in selected - queuing SCAN operation for check-in");
      if (op_queue != NULL) {
        OpJob job = {OP_SCAN, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, "", ""};
        strncpy(job.mode, "check-in", sizeof(job.mode) - 1);
        job.expiry_date[0] = '\0';  // Initialize empty - will be set when expiration date is received
        if (xQueueSend(op_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
          Serial.println("[OP] SCAN job queued for check-in (check-in mode)");
        } else {
          Serial.println("[OP] Failed to queue SCAN job (queue full?)");
        }
      }
    }
    // TODO: Handle other menu items (Kitchen, Health, Home) in future
  } else if (strcmp(type, "INPUT_EXPIRY_DATE") == 0) {
    // Expiration date received from LCD (for check-in mode)
    const char* expiry_date = doc["expiry_date"] | "";
    Serial.printf("[UART] Expiration date received: '%s' (len=%d)\n", expiry_date, strlen(expiry_date));
    
    // Mark that we received a response (even if empty)
    expiry_date_response_received = true;
    
    // Handle both empty and non-empty expiry dates
    // Empty string means user skipped entering a date - proceed immediately
    if (strlen(expiry_date) < sizeof(pending_expiry_date)) {
      strncpy(pending_expiry_date, expiry_date, sizeof(pending_expiry_date) - 1);
      pending_expiry_date[sizeof(pending_expiry_date) - 1] = '\0';
      
      // Update active check-in job if it exists
      if (active_checkin_job != NULL && strcmp(active_checkin_job->mode, "check-in") == 0) {
        strncpy(active_checkin_job->expiry_date, expiry_date, sizeof(active_checkin_job->expiry_date) - 1);
        active_checkin_job->expiry_date[sizeof(active_checkin_job->expiry_date) - 1] = '\0';
        if (strlen(expiry_date) > 0) {
          Serial.printf("[UART] Updated active check-in job with expiration date: %s\n", expiry_date);
        } else {
          Serial.println("[UART] Updated active check-in job with empty expiry date (user skipped) - proceeding immediately");
        }
      } else {
        if (strlen(expiry_date) > 0) {
          Serial.printf("[UART] Stored expiration date for next check-in job: %s\n", expiry_date);
        } else {
          Serial.println("[UART] Stored empty expiry date for next check-in job (user skipped)");
        }
      }
    }
  } else if (strcmp(type, "SYNC") == 0) {
    Serial.println("[LNK] sync_rx -> reset_parser");
    link_reset_parser_state();
    link_synced = true;
    Serial.println("[LNK] synced=1");
    uart_send_sync_ack();
  } else if (strcmp(type, "WIFI_STATUS") == 0) {
    lcd_wifi_has_creds = (doc["has_creds"] | 0) != 0;
    lcd_wifi_checksum = (uint32_t)(doc["checksum"] | 0);
    Serial.printf("[UART] WIFI_STATUS has_creds=%d checksum=%lu\n",
                  lcd_wifi_has_creds ? 1 : 0,
                  (unsigned long)lcd_wifi_checksum);
    char ssid_buf[64];
    char pass_buf[64];
    bool has_creds = false;
    uint32_t local_checksum =
        sense_current_wifi_checksum(&has_creds, ssid_buf, sizeof(ssid_buf), pass_buf, sizeof(pass_buf));
    bool mismatch = (lcd_wifi_checksum != local_checksum);
    if (!lcd_wifi_has_creds || mismatch) {
      if (has_creds) {
        uart_send_wifi_creds_json(ssid_buf, pass_buf);
        last_sent_wifi_checksum = local_checksum;
        Serial.printf("[UART] WIFI_CREDS sent reason=%s\n",
                      lcd_wifi_has_creds ? "checksum_mismatch" : "lcd_missing");
      } else {
        Serial.println("[UART] WIFI_CREDS skip reason=no_local_creds");
      }
    }
  } else if (strcmp(type, "SYNC_ACK") == 0) {
    link_synced = true;
    Serial.println("[LNK] synced=1");
  } else if (strcmp(type, "OTA_CHECK_ACK") == 0) {
#ifdef HALO_SENSE_PROD_WRAPPER
    uint32_t req_id = (uint32_t)(doc["request_id"] | 0);
    if (g_lcd_ota_request_active && (req_id == 0 || req_id == g_lcd_ota_request_id)) {
      g_lcd_ota_ack_received = true;
      Serial.printf("[UART] OTA_CHECK_ACK request_id=%lu\n", (unsigned long)req_id);
    } else {
      Serial.printf("[UART] OTA_CHECK_ACK ignored request_id=%lu\n", (unsigned long)req_id);
    }
#else
    Serial.println("[UART] OTA_CHECK_ACK received (ignored)");
#endif
  } else if (strcmp(type, "OTA_CHECK_RESULT") == 0) {
#ifdef HALO_SENSE_PROD_WRAPPER
    uint32_t req_id = (uint32_t)(doc["request_id"] | 0);
    const char* result = doc["result"] | "";
    const char* detail = doc["detail"] | "";
    const char* new_version = doc["new_version"] | doc["version"] | "";
    const char* err_code = doc["err_code"] | "";
    if (g_lcd_ota_request_active && (req_id == 0 || req_id == g_lcd_ota_request_id)) {
      if (detail && detail[0]) {
        snprintf(g_lcd_ota_result, sizeof(g_lcd_ota_result), "%s:%s", result, detail);
      } else {
        strncpy(g_lcd_ota_result, result ? result : "", sizeof(g_lcd_ota_result) - 1);
        g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
      }
      if (new_version && new_version[0]) {
        strncpy(g_lcd_ota_version, new_version, sizeof(g_lcd_ota_version) - 1);
      } else {
        g_lcd_ota_version[0] = '\0';
      }
      g_lcd_ota_version[sizeof(g_lcd_ota_version) - 1] = '\0';
      g_lcd_ota_done = true;
      g_lcd_ota_request_active = false;
      Serial.printf("[UART] OTA_CHECK_RESULT request_id=%lu result=%s err=%s version=%s\n",
                    (unsigned long)req_id,
                    g_lcd_ota_result,
                    err_code ? err_code : "",
                    g_lcd_ota_version);
    } else {
      Serial.printf("[UART] OTA_CHECK_RESULT ignored request_id=%lu\n", (unsigned long)req_id);
    }
#else
    Serial.println("[UART] OTA_CHECK_RESULT received (ignored)");
#endif
  } else if (strcmp(type, "LCD_OTA_DONE") == 0) {
    const char* result = doc["result"] | "";
    const char* version = doc["version"] | "";
    strncpy(g_lcd_ota_result, result ? result : "", sizeof(g_lcd_ota_result) - 1);
    g_lcd_ota_result[sizeof(g_lcd_ota_result) - 1] = '\0';
    strncpy(g_lcd_ota_version, version ? version : "", sizeof(g_lcd_ota_version) - 1);
    g_lcd_ota_version[sizeof(g_lcd_ota_version) - 1] = '\0';
    g_lcd_ota_done = true;
#ifdef HALO_SENSE_PROD_WRAPPER
    g_lcd_ota_request_active = false;
#endif
    Serial.printf("[UART] LCD_OTA_DONE result=%s version=%s\n",
                  g_lcd_ota_result,
                  g_lcd_ota_version);
  } else {
    Serial.printf("[PROTO] Unknown type: %s\n", type);
  }
  return true;
}

// ── Arduino lifecycle ──────────────────────────────────────────────
// ── Operation Worker Task ──────────────────────────────────────────
// Runs operations (VOICE/SCAN) as state machines
static void op_worker_task(void *arg) {
  Serial.println("[OP_WORKER] Operation worker task started");
  
  for (;;) {
    OpJob job;
    
    // Check for new jobs (non-blocking)
    if (op_queue != NULL && xQueueReceive(op_queue, &job, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.printf("[OP_WORKER] Processing job: type=%d, pri=%d, id=%d\n", job.type, job.pri, job.job_id);
      
      // Set as current job and mark foreground if user-initiated
      current_job = job;
      current_job.active = true;
      
      // If this is a check-in job, set it as the active job and apply pending expiration date
      if (strcmp(job.mode, "check-in") == 0) {
        active_checkin_job = &current_job;
        // Clear expiry date first to ensure we start fresh
        current_job.expiry_date[0] = '\0';
        // Reset flag for new job
        expiry_date_response_received = false;
        // Apply pending expiration date if available
        if (pending_expiry_date[0] != '\0') {
          strncpy(current_job.expiry_date, pending_expiry_date, sizeof(current_job.expiry_date) - 1);
          current_job.expiry_date[sizeof(current_job.expiry_date) - 1] = '\0';
          Serial.printf("[OP_WORKER] Applied pending expiration date to check-in job: %s\n", pending_expiry_date);
          pending_expiry_date[0] = '\0';  // Clear pending date
          expiry_date_response_received = true;  // Mark as received if we had a pending date
        }
      }
      
      if (job.pri == PRI_USER) {
        foreground_active = true;
      }
      
      // Execute operation state machine
      if (job.type == OP_VOICE) {
        // VOICE state machine: IDLE -> RECORDING -> FINALIZE -> UPLOAD -> PARSE -> APPLY -> DONE
        Serial.println("[OP_WORKER] Starting VOICE operation state machine");
        current_job.state = OP_RECORDING;
        voice_audio_pos = 0;
        voice_audio_size = 0;
        voice_recording_active = true;
        
        // Send status: RECORDING
        uart_send_ui_status_extended("VOICE", "RECORDING", "Listening…");
        
        // Start I2S audio recording (max 30 seconds, but will stop on INPUT_LONG_PRESS_END)
        Serial.println("[OP_WORKER] VOICE: Starting I2S audio recording...");
        audio_start_recording_ms(30000);
        
        // Wait for recording to complete (will be signaled by INPUT_LONG_PRESS_END)
        // For now, record for max 10 seconds or until signaled
        uint32_t record_start = millis();
        const uint32_t MAX_RECORD_MS = 10000;
        
        while (current_job.state == OP_RECORDING && (millis() - record_start) < MAX_RECORD_MS) {
          vTaskDelay(pdMS_TO_TICKS(100));
          // Check if we should finalize (signaled by INPUT_LONG_PRESS_END)
          if (current_job.state == OP_FINALIZE) {
            break;
          }
        }
        
        // If still recording, finalize now (timeout or user released)
        if (current_job.state == OP_RECORDING) {
          current_job.state = OP_FINALIZE;
        }
        
        // Stop I2S audio recording
        voice_recording_active = false;
        audio_stop_recording();
        
        // Wait a bit for any remaining samples to be written by callback
        delay(200);
        
        if (voice_audio_size == 0) {
          Serial.println("[OP_WORKER] VOICE: No audio recorded, aborting");
          uart_send_ui_status_extended("VOICE", "ERROR", "Recording too short");
          current_job.state = OP_DONE;
        } else {
          Serial.printf("[OP_WORKER] VOICE: Recording complete, %d bytes\n", voice_audio_size);
          
          // Transition to UPLOAD
          current_job.state = OP_UPLOAD;
          uart_send_ui_status_extended("VOICE", "UPLOADING", "Transcribing…");
          
          // Upload audio to voice API
          if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[OP_WORKER] VOICE: Wi-Fi not connected, connecting...");
            if (!wifi_connect()) {
              Serial.println("[OP_WORKER] VOICE: Wi-Fi connection failed!");
              uart_send_ui_status_extended("VOICE", "ERROR", "Wi-Fi failed");
              current_job.state = OP_DONE;
            }
          }
          
          if (current_job.state == OP_UPLOAD && WiFi.status() == WL_CONNECTED) {
            // Upload audio and parse response
            String response_json = voice_upload_and_parse();
            
            if (response_json.length() > 0) {
              // Print response JSON to serial monitor for debugging
              Serial.println("\n=== Quick-Ack API Response ===");
              Serial.println(response_json);
              Serial.println("=== End Response ===\n");
              
              // Transition to PARSE
              current_job.state = OP_PARSE;
              uart_send_ui_status_extended("VOICE", "PROCESSING", "Processing…");
              
              // Parse JSON response - Realtime API format: {text, transcript, quickItems, ...}
              DynamicJsonDocument doc(4096);
              DeserializationError error = deserializeJson(doc, response_json);
              
              if (!error) {
                Serial.println("[OP_WORKER] VOICE: JSON parsed successfully");
                
                // Check for quickItems array (array of strings like ["chicken", "beef"])
                JsonArray quickArray = doc["quickItems"].as<JsonArray>();
                if (!quickArray.isNull() && quickArray.size() > 0) {
                  Serial.printf("[OP_WORKER] VOICE: Found %d quick items\n", quickArray.size());
                  
                  // Build UI_VOICE_ITEMS message
                  StaticJsonDocument<2048> voice_doc;
                  voice_doc["ver"] = PROTOCOL_VERSION;
                  voice_doc["type"] = "UI_VOICE_ITEMS";
                  voice_doc["msg_id"] = get_next_msg_id();
                  voice_doc["ts"] = millis();
                  JsonArray voice_items = voice_doc.createNestedArray("items");
                  
                  // Convert quickItems (strings) to items array (objects with text field)
                  int items_added = 0;
                  for (JsonVariant v : quickArray) {
                    const char* item_text = NULL;
                    
                    // Handle array of strings (most common format from Realtime API)
                    if (v.is<const char*>()) {
                      item_text = v.as<const char*>();
                    } else if (v.is<JsonObject>()) {
                      // Fallback: handle object format if API changes
                      JsonObject item = v.as<JsonObject>();
                      item_text = item["item"] | "";
                    }
                    
                    if (item_text != NULL && strlen(item_text) > 0) {
                      Serial.printf("  ➕ Item: %s\n", item_text);
                      JsonObject voice_item = voice_items.createNestedObject();
                      voice_item["text"] = item_text;
                      items_added++;
                    }
                  }
                  
                  if (items_added > 0) {
                    // Send UI_VOICE_ITEMS to LCD
                    String voice_output;
                    serializeJson(voice_doc, voice_output);
                    uart_send_json(voice_output.c_str());
                    Serial.printf("[OP_WORKER] VOICE: Sent UI_VOICE_ITEMS with %d items to LCD\n", items_added);
                  }
                } else {
                  Serial.println("[OP_WORKER] VOICE: No quickItems in response");
                }
                
                // Send toast message if available (text field)
                if (doc.containsKey("text")) {
                  const char* msg = doc["text"];
                  if (msg && strlen(msg) > 0) {
                    Serial.printf("[OP_WORKER] VOICE: Toast message: %s\n", msg);
                    uart_send_ui_toast(msg);
                  }
                }
                
                // Transition to APPLY - refresh list to get updated list with new items
                current_job.state = OP_APPLY;
                Serial.println("[OP_WORKER] VOICE: Refreshing list from API to get updated items...");
                fetch_shopping_list_from_api();
                
                current_job.state = OP_DONE;
                uart_send_ui_status_extended("VOICE", "DONE", "Items added");
              } else {
                Serial.printf("[OP_WORKER] VOICE: JSON parse error: %s\n", error.c_str());
                Serial.printf("[OP_WORKER] VOICE: Response was: %s\n", response_json.c_str());
                uart_send_ui_status_extended("VOICE", "ERROR", "Parse failed");
                current_job.state = OP_DONE;
              }
            } else {
              Serial.println("[OP_WORKER] VOICE: Upload failed or empty response");
              uart_send_ui_status_extended("VOICE", "ERROR", "Upload failed");
              current_job.state = OP_DONE;
            }
          }
        }
        
        Serial.println("[OP_WORKER] VOICE operation complete");
      } else if (job.type == OP_SCAN) {
        // SCAN state machine: IDLE -> CAPTURE -> PRESIGN -> UPLOAD -> WAIT_MQTT -> DONE
        Serial.printf("[OP_WORKER] Starting SCAN operation state machine (mode: %s)\n", job.mode);
        scan_terminal_reset();
        
        // State: CAPTURE - Initialize camera and capture image
        current_job.state = OP_RECORDING;  // Reuse RECORDING state for capture phase
        uart_send_ui_status_extended("SCAN", "CAPTURING", "Capturing image…", job.mode);
        
        // Ensure camera is OFF before starting
        sensor_t* s_check = esp_camera_sensor_get();
        if (s_check != NULL) {
          Serial.println("[OP_WORKER] SCAN: Camera already initialized - deinitializing first...");
          deinit_camera();
          delay(100);
        }
        
        // For check-in mode: Capture image FIRST, then wait for expiry date, then presign and upload
        // For other modes: Get presign first, then capture, then upload
        camera_fb_t* fb = nullptr;
        PresignReply upload_presign;  // Store presign reply for upload (used by both paths)
          
        if (strcmp(job.mode, "check-in") == 0) {
          // CHECK-IN MODE: Capture image first
          Serial.println("[OP_WORKER] SCAN: Check-in mode - capturing image first...");
          uart_send_ui_status_extended("SCAN", "CAPTURING", "Capturing image…", job.mode);
          
          // Calculate delay for user positioning (2 seconds)
          delay(2000);
          
          // Initialize camera right before capture
          Serial.println("[OP_WORKER] SCAN: Initializing camera...");
          if (!init_camera()) {
            Serial.println("[OP_WORKER] SCAN: Camera initialization FAILED");
            deinit_camera();
            scan_send_terminal_status("ERROR", "Camera init failed", job.mode);
            current_job.state = OP_DONE;
          } else {
            if (!warmup_and_capture(fb)) {
              Serial.println("[OP_WORKER] SCAN: Camera capture FAILED");
              deinit_camera();
              scan_send_terminal_status("ERROR", "Camera failed", job.mode);
              current_job.state = OP_DONE;
            } else {
              Serial.printf("[OP_WORKER] SCAN: Captured %u bytes (%dx%d) - copying for later upload\n", 
                           fb->len, fb->width, fb->height);
              
              // Copy the captured image data to our own buffer (camera buffer will be freed on deinit)
              if (captured_image_buffer != NULL) {
                free(captured_image_buffer);
                captured_image_buffer = NULL;
              }
              captured_image_buffer = (uint8_t*)malloc(fb->len);
              if (captured_image_buffer == NULL) {
                Serial.println("[OP_WORKER] SCAN: ERROR - Failed to allocate memory for image copy!");
                esp_camera_fb_return(fb);
                deinit_camera();
                scan_send_terminal_status("ERROR", "Memory failed", job.mode);
                current_job.state = OP_DONE;
              } else {
                memcpy(captured_image_buffer, fb->buf, fb->len);
                captured_image_size = fb->len;
                Serial.printf("[OP_WORKER] SCAN: Copied %u bytes to buffer at %p\n", captured_image_size, captured_image_buffer);
                
                // Return the camera frame buffer (we have our own copy)
                esp_camera_fb_return(fb);
                fb = nullptr;
                
                // Power down camera immediately after capture (we have the image copied)
                Serial.println("[OP_WORKER] SCAN: Powering down camera after capture...");
                deinit_camera();
              }
              
              // Now wait for expiry date before getting presign URL (only if we successfully copied the image)
              if (captured_image_buffer != NULL && captured_image_size > 0) {
                Serial.println("[OP_WORKER] SCAN: Check-in mode - waiting for expiry date...");
                uart_send_ui_status_extended("SCAN", "PREPARING", "Waiting for expiry date…", job.mode);
                
                // Wait up to 30 seconds for expiry date
                unsigned long wait_start = millis();
                const unsigned long MAX_EXPIRY_WAIT_MS = 30000;  // 30 seconds max wait
                
                // Reset flag before waiting
                expiry_date_response_received = false;
                
                Serial.println("[OP_WORKER] SCAN: Waiting for expiry date (checking every 100ms)...");
                while (!expiry_date_response_received && (millis() - wait_start) < MAX_EXPIRY_WAIT_MS) {
                  // Check if expiry date response was received (flag is set by INPUT_EXPIRY_DATE handler)
                  if (expiry_date_response_received) {
                    // Response received - copy expiry date from active job (even if empty)
                    if (active_checkin_job != NULL) {
                      strncpy(current_job.expiry_date, active_checkin_job->expiry_date, sizeof(current_job.expiry_date) - 1);
                      current_job.expiry_date[sizeof(current_job.expiry_date) - 1] = '\0';
                      if (current_job.expiry_date[0] != '\0') {
                        Serial.printf("[OP_WORKER] SCAN: Expiry date received: %s\n", current_job.expiry_date);
                      } else {
                        Serial.println("[OP_WORKER] SCAN: Empty expiry date received (user skipped) - proceeding immediately");
                      }
                    }
                    break;
                  }
                  vTaskDelay(pdMS_TO_TICKS(100));  // Check every 100ms
                }
                
                if (!expiry_date_response_received) {
                  Serial.println("[OP_WORKER] SCAN: Check-in mode - expiry date timeout, proceeding without it");
                } else if (current_job.expiry_date[0] == '\0') {
                  Serial.println("[OP_WORKER] SCAN: Check-in mode - empty expiry date received, proceeding without it");
                }
                
                // Now get presign URL with expiry date
                // Keep state as OP_UPLOAD (already set) - we'll use it for upload phase
                uart_send_ui_status_extended("SCAN", "PREPARING", "Preparing upload…", job.mode);
                
                Serial.println("[OP_WORKER] SCAN: Using check-in presign API");
                const char* expiry = (current_job.expiry_date[0] != '\0') ? current_job.expiry_date : NULL;
                bool presign_success = get_presign_checkin(upload_presign, expiry);
                
                if (!presign_success) {
                  Serial.println("[OP_WORKER] SCAN: Presign step FAILED");
                  if (captured_image_buffer != NULL) {
                    free(captured_image_buffer);
                    captured_image_buffer = NULL;
                    captured_image_size = 0;
                  }
                  scan_send_terminal_status("ERROR", presign_error_text(), job.mode);
                  current_job.state = OP_DONE;
                } else {
                  Serial.printf("[OP_WORKER] SCAN: Presign OK — job_id: %s\n", upload_presign.job_id.c_str());
                  current_scan_job_id = upload_presign.job_id;
                  
                  // State: UPLOAD - Upload stored image to S3
                  // Ensure state is set to OP_UPLOAD (not OP_DONE) so upload code executes
                  current_job.state = OP_UPLOAD;
                  uart_send_ui_status_extended("SCAN", "UPLOADING", "Uploading image…", job.mode);
                  
                  // We'll upload the copied buffer directly in the upload section below
                  Serial.printf("[OP_WORKER] SCAN: Ready to upload copied image: %u bytes, buf=%p, state=%d\n", 
                               captured_image_size, captured_image_buffer, current_job.state);
                }
              }
            }
          }
        } else {
          // DISH/DISCARD MODES: Get presign first, then capture, then upload (original flow)
          // State: PRESIGN - Get presigned URL (happens in parallel with user positioning)
          current_job.state = OP_UPLOAD;  // Reuse UPLOAD state for presign phase
          uart_send_ui_status_extended("SCAN", "PREPARING", "Preparing upload…", job.mode);
          
          unsigned long presign_start_time = millis();
          
          // Get presigned URL - use meal nutrition API
          Serial.println("[OP_WORKER] SCAN: Using meal nutrition presign API");
          bool presign_success = get_presign(upload_presign);
          
          if (!presign_success) {
            Serial.println("[OP_WORKER] SCAN: Presign step FAILED");
            scan_send_terminal_status("ERROR", presign_error_text(), job.mode);
            current_job.state = OP_DONE;
          } else {
            Serial.printf("[OP_WORKER] SCAN: Presign OK — job_id: %s\n", upload_presign.job_id.c_str());
            current_scan_job_id = upload_presign.job_id;
            
            // Calculate delay for user positioning (2 seconds total)
            unsigned long elapsed = millis() - presign_start_time;
            unsigned long target_delay = 2000;
            if (elapsed < target_delay) {
              unsigned long remaining_delay = target_delay - elapsed;
              Serial.printf("[OP_WORKER] SCAN: Presign took %lums, waiting %lums more for positioning\n", 
                            elapsed, remaining_delay);
              delay(remaining_delay);
            }
            
            // State: CAPTURE - Capture image
            uart_send_ui_status_extended("SCAN", "CAPTURING", "Capturing image…", job.mode);
            Serial.println("[OP_WORKER] SCAN: Initializing camera...");
            if (!init_camera()) {
              Serial.println("[OP_WORKER] SCAN: Camera initialization FAILED");
              deinit_camera();
              scan_send_terminal_status("ERROR", "Camera init failed", job.mode);
              current_job.state = OP_DONE;
            } else {
              if (!warmup_and_capture(fb)) {
                Serial.println("[OP_WORKER] SCAN: Camera capture FAILED");
                deinit_camera();
                scan_send_terminal_status("ERROR", "Camera failed", job.mode);
                current_job.state = OP_DONE;
              } else {
                Serial.printf("[OP_WORKER] SCAN: Captured %u bytes (%dx%d)\n", fb->len, fb->width, fb->height);

                // Copy image so we can power down camera immediately
                if (captured_image_buffer != NULL) {
                  free(captured_image_buffer);
                  captured_image_buffer = NULL;
                }
                captured_image_buffer = (uint8_t*)malloc(fb->len);
                if (captured_image_buffer == NULL) {
                  Serial.println("[OP_WORKER] SCAN: ERROR - Failed to allocate memory for image copy!");
                  esp_camera_fb_return(fb);
                  deinit_camera();
                  scan_send_terminal_status("ERROR", "Memory failed", job.mode);
                  current_job.state = OP_DONE;
                } else {
                  memcpy(captured_image_buffer, fb->buf, fb->len);
                  captured_image_size = fb->len;
                  Serial.printf("[OP_WORKER] SCAN: Copied %u bytes to buffer at %p\n", captured_image_size, captured_image_buffer);
                  esp_camera_fb_return(fb);
                  fb = nullptr;
                  Serial.println("[OP_WORKER] SCAN: Powering down camera after capture...");
                  deinit_camera();

                  // State: UPLOAD - Upload to S3
                  uart_send_ui_status_extended("SCAN", "UPLOADING", "Uploading image…", job.mode);
                }
              }
            }
          }
          }
        }
          
          // Upload image (for both check-in and dish/discard modes)
          bool upload_success = false;
          
          if (strcmp(job.mode, "check-in") == 0) {
            // Check-in mode: Use copied buffer (camera was already deinitialized)
            Serial.printf("[OP_WORKER] SCAN: Upload check (check-in) - buffer=%p, size=%u, state=%d\n", 
                         captured_image_buffer, captured_image_size, current_job.state);
            
            if (captured_image_buffer != NULL && current_job.state != OP_DONE && captured_image_size > 0) {
              Serial.printf("[OP_WORKER] SCAN: Uploading copied image: %u bytes, buf=%p\n", captured_image_size, captured_image_buffer);
              upload_success = put_to_presigned_url(upload_presign.put_url, captured_image_buffer, captured_image_size,
                                                   upload_presign.content_type.length() ? upload_presign.content_type.c_str() : "image/jpeg");
              // Free the copied buffer after upload
              free(captured_image_buffer);
              captured_image_buffer = NULL;
              captured_image_size = 0;
            } else {
              Serial.printf("[OP_WORKER] SCAN: Cannot upload check-in image - buffer=%p, size=%u, state=%d\n", 
                           captured_image_buffer, captured_image_size, current_job.state);
            }
          } else {
            // Dish/discard mode: Use copied buffer so camera is already off
            Serial.printf("[OP_WORKER] SCAN: Upload check (dish/discard) - buffer=%p, size=%u, state=%d\n", 
                         captured_image_buffer, captured_image_size, current_job.state);
            
            if (captured_image_buffer != NULL && current_job.state != OP_DONE && captured_image_size > 0) {
              Serial.printf("[OP_WORKER] SCAN: Uploading image: %u bytes, buf=%p\n", captured_image_size, captured_image_buffer);
              upload_success = put_to_presigned_url(upload_presign.put_url, captured_image_buffer, captured_image_size,
                                                   upload_presign.content_type.length() ? upload_presign.content_type.c_str() : "image/jpeg");
              free(captured_image_buffer);
              captured_image_buffer = NULL;
              captured_image_size = 0;
            } else if (fb != nullptr && current_job.state != OP_DONE && fb->buf != nullptr && fb->len > 0) {
              // Fallback if copy failed
              Serial.printf("[OP_WORKER] SCAN: Uploading image (fb fallback): %u bytes, buf=%p\n", fb->len, fb->buf);
              upload_success = put_to_presigned_url(upload_presign.put_url, fb->buf, fb->len,
                                                   upload_presign.content_type.length() ? upload_presign.content_type.c_str() : "image/jpeg");
              esp_camera_fb_return(fb);
              fb = nullptr;
              deinit_camera();
            }
          }
          
          if (!upload_success) {
            // Upload failed
            if (captured_image_buffer != NULL) {
              free(captured_image_buffer);
              captured_image_buffer = NULL;
              captured_image_size = 0;
            }
            Serial.println("[OP_WORKER] SCAN: Upload FAILED");
            scan_send_terminal_status("ERROR", "Upload failed", job.mode);
            waiting_for_mqtt_result = false;
            current_scan_job_id = "";
            current_job.state = OP_DONE;
          } else {
            Serial.println("[OP_WORKER] SCAN: Upload OK");
            
            // For discard and check-in modes, we don't need nutrition data - complete immediately
            if (strcmp(job.mode, "discard") == 0 || strcmp(job.mode, "check-in") == 0) {
              Serial.printf("[OP_WORKER] SCAN: %s mode - skipping MQTT wait, completing immediately\n", job.mode);
              // Clear MQTT waiting state (if it was set)
              waiting_for_mqtt_result = false;
              current_scan_job_id = "";
              current_job.state = OP_DONE;
              scan_send_terminal_status("SUCCESS", "Logged!", job.mode);
            } else {
              // For dish mode, wait for MQTT result to get nutrition data
              Serial.println("[OP_WORKER] SCAN: Dish mode - waiting for MQTT result...");
              Serial.printf("[OP_WORKER] SCAN: Backend will analyze and publish to MQTT:\n");
              Serial.printf("[OP_WORKER] SCAN:   Topic: %s\n", RESULT_TOPIC);
              Serial.printf("[OP_WORKER] SCAN:   job_id=%s\n", upload_presign.job_id.c_str());
              
              // Ensure MQTT is connected and subscribed (only for dish mode)
              mqtt_ensure_connected();
              if (!mqttClient.connected() || !mqtt_subscribed) {
                Serial.println("[OP_WORKER] SCAN: MQTT not ready (no sub); continuing but may miss result");
              }
              
              // Set up MQTT waiting (only for dish mode)
              waiting_for_mqtt_result = true;
              mqtt_wait_deadline = millis() + 30000;  // 30s window
              
              // State: WAIT_MQTT - Wait for MQTT result (with timeout)
              current_job.state = OP_PARSE;  // Reuse PARSE state for waiting
              uart_send_ui_status_extended("SCAN", "PROCESSING", "Analyzing image…", job.mode);
              
              // Wait for MQTT result (with timeout)
              uint32_t wait_start = millis();
              const uint32_t MAX_WAIT_MS = 30000;  // 30 seconds max wait
              
              while (waiting_for_mqtt_result && (millis() - wait_start) < MAX_WAIT_MS) {
                // Process MQTT messages
                if (mqttClient.connected()) {
                  mqttClient.loop();
                } else {
                  mqtt_ensure_connected();
                }
                
                // Check if we got the result
                if (!waiting_for_mqtt_result) {
                  break;  // Got result!
                }
                
                vTaskDelay(pdMS_TO_TICKS(100));
              }
              
              if (waiting_for_mqtt_result) {
                // Timeout - no result received
                Serial.println("[OP_WORKER] SCAN: MQTT wait timeout");
                scan_send_terminal_status("ERROR", "Analysis timeout", job.mode);
                waiting_for_mqtt_result = false;
                current_scan_job_id = "";
              } else {
                Serial.println("[OP_WORKER] SCAN: MQTT result received and processed");
              }
              
              current_job.state = OP_DONE;
              scan_send_terminal_status("SUCCESS", "Logged!", job.mode);
            }
          }
        
        Serial.println("[OP_WORKER] SCAN operation complete");
      } else if (job.type == OP_LIST_REFRESH) {
        // Background list refresh
        Serial.println("[OP_WORKER] LIST_REFRESH operation");
        if (WiFi.status() != WL_CONNECTED) {
          if (!wifi_connect()) {
            Serial.println("[OP_WORKER] Wi-Fi connection failed!");
            list_refresh_fail("wifi_failed");
          }
        }
        if (WiFi.status() == WL_CONNECTED) {
          fetch_shopping_list_from_api();
        }
        list_refresh_mark_complete("job_done");
      }
      
      // Job complete
      current_job.active = false;
      if (job.pri == PRI_USER) {
        foreground_active = false;
      }
      
      // Clear active check-in job pointer and expiry date if this was a check-in job
      if (strcmp(current_job.mode, "check-in") == 0) {
        active_checkin_job = NULL;
        current_job.expiry_date[0] = '\0';  // Clear expiry date for next job
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── Sleep/Wake Functions ────────────────────────────────────────────
static bool wake_pin_is_active_level(int level) {
  return level == WAKE_ACTIVE_LEVEL;
}

static bool sense_link_recent(unsigned long now_ms, unsigned long* rx_age_out, unsigned long* hb_age_out) {
  unsigned long rx_age = (last_uart_rx_ms > 0) ? (now_ms - last_uart_rx_ms) : 0xFFFFFFFFUL;
  unsigned long hb_age = (last_link_hb_rx_ms > 0) ? (now_ms - last_link_hb_rx_ms) : 0xFFFFFFFFUL;
  if (rx_age_out) {
    *rx_age_out = rx_age;
  }
  if (hb_age_out) {
    *hb_age_out = hb_age;
  }
  bool recent_rx = (rx_age < LINK_RECENT_MS);
  bool recent_hb = (hb_age < LINK_RECENT_MS);
  bool recent_sync = (link_synced && last_uart_rx_ms > 0 && rx_age < LINK_RECENT_MS);
  return recent_rx || recent_hb || recent_sync;
}

static void uart_drain_tx(uint32_t timeout_ms) {
  unsigned long start_ms = millis();
  lcdSerial.flush();
  Serial.flush();
  (void)uart_wait_tx_done(LCD_UART_PORT, pdMS_TO_TICKS(timeout_ms));
  unsigned long done_ms = millis() - start_ms;
  Serial.printf("[SLEEP_PROTO] tx_drain done_ms=%lu\n", done_ms);
}

static void log_wake_pin_boot_state(const char* phase) {
  int level = gpio_get_level(WAKE_GPIO);
  Serial.printf("[WAKE_PIN_BOOT] board=%s gpio=%d active=%d level=%d phase=%s\n",
                HALO_BOARD_NAME,
                (int)WAKE_GPIO,
                WAKE_LEVEL,
                level,
                phase ? phase : "boot");
}

static void warn_wake_pin_active_at_boot(unsigned long window_ms) {
  unsigned long start_ms = millis();
  bool stuck_active = true;
  while ((millis() - start_ms) < window_ms) {
    int level = gpio_get_level(WAKE_GPIO);
    if (!wake_pin_is_active_level(level)) {
      stuck_active = false;
      break;
    }
    delay(10);
  }
  if (stuck_active) {
    Serial.printf("[WAKE_PIN_WARN] board=%s gpio=%d active_level=%d held_active_ms=%lu\n",
                  HALO_BOARD_NAME,
                  (int)WAKE_GPIO,
                  WAKE_LEVEL,
                  window_ms);
  }
}

static bool wake_line_active_for_ms(unsigned long window_ms) {
  unsigned long start_ms = millis();
  while ((millis() - start_ms) < window_ms) {
    int level = rtc_gpio_get_level(WAKE_GPIO);
    if (!wake_pin_is_active_level(level)) {
      return false;
    }
    delay(10);
  }
  return true;
}

static void wake_pin_configure_rtc_input_inactive_pull() {
  rtc_gpio_init(WAKE_GPIO);
  rtc_gpio_set_direction(WAKE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  if (WAKE_INACTIVE_LEVEL == 1) {
    rtc_gpio_pullup_en(WAKE_GPIO);
    rtc_gpio_pulldown_dis(WAKE_GPIO);
  } else {
    rtc_gpio_pullup_dis(WAKE_GPIO);
    rtc_gpio_pulldown_en(WAKE_GPIO);
  }
}

static void wake_pin_apply_mitigation(const char* reason) {
  Serial.printf("[SLEEP_SANITY] wake_pin_mitigation reason=%s inactive_level=%d hold_ms=%lu\n",
                reason ? reason : "unknown",
                WAKE_INACTIVE_LEVEL,
                (unsigned long)WAKE_PIN_MITIGATION_MS);
  wake_pin_configure_rtc_input_inactive_pull();
  delay(WAKE_PIN_MITIGATION_MS);
  wake_pin_configure_rtc_input_inactive_pull();
}

static uint32_t sleep_deny_retry_ms(const char* reason, unsigned long now_ms) {
  if (!reason) {
    return SLEEP_DENY_RETRY_DEFAULT_MS;
  }
  if (strcmp(reason, "cooldown") == 0) {
    if (sleep_grace_until_ms > now_ms) {
      unsigned long remaining = sleep_grace_until_ms - now_ms;
      if (remaining > SLEEP_DENY_RETRY_COOLDOWN_MAX_MS) {
        remaining = SLEEP_DENY_RETRY_COOLDOWN_MAX_MS;
      }
      return (uint32_t)remaining;
    }
    if ((now_ms - last_wake_ms) < MIN_AWAKE_BEFORE_SLEEP_MS) {
      unsigned long remaining = MIN_AWAKE_BEFORE_SLEEP_MS - (now_ms - last_wake_ms);
      if (remaining > SLEEP_DENY_RETRY_COOLDOWN_MAX_MS) {
        remaining = SLEEP_DENY_RETRY_COOLDOWN_MAX_MS;
      }
      return (uint32_t)remaining;
    }
    return SLEEP_DENY_RETRY_DEFAULT_MS;
  }
  if (strcmp(reason, "wake_pin_active") == 0) {
    return WAKE_PIN_DEASSERT_WAIT_MS;
  }
  if (strcmp(reason, "ota_pending") == 0 || strcmp(reason, "mqtt_pending") == 0) {
    return 10000;
  }
  return SLEEP_DENY_RETRY_DEFAULT_MS;
}

static void sense_config_deep_sleep_wakeup(bool enable_ext0, uint32_t fallback_timer_s) {
  // Configure WAKE_GPIO for deep sleep wake on LOW (LCD pulses LOW)
  wake_pin_configure_rtc_input_inactive_pull();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (enable_ext0) {
    esp_sleep_enable_ext0_wakeup(WAKE_GPIO, WAKE_LEVEL);
    ota_configure_timer_wakeup();
    Serial.printf("[SENSE] Wake EXT0 configured: GPIO%d level=%d\n", WAKE_GPIO, WAKE_LEVEL);
  } else {
    if (fallback_timer_s > 0) {
      esp_sleep_enable_timer_wakeup((uint64_t)fallback_timer_s * 1000000ULL);
      Serial.printf("[SENSE] Wake EXT0 disabled; timer fallback_s=%lu\n",
                    (unsigned long)fallback_timer_s);
    } else {
      Serial.println("[SENSE] Wake EXT0 disabled; no timer fallback");
    }
  }
}

static bool sleep_wait_wake_pin_deassert(unsigned long wait_ms) {
  unsigned long start_ms = millis();
  unsigned long next_log_ms = start_ms;
  unsigned long stable_start_ms = 0;
  while ((millis() - start_ms) < wait_ms) {
    unsigned long now_ms = millis();
    int level = rtc_gpio_get_level(WAKE_GPIO);
    if (wake_pin_is_active_level(level)) {
      stable_start_ms = 0;
    } else {
      if (stable_start_ms == 0) {
        stable_start_ms = now_ms;
      }
      if ((now_ms - stable_start_ms) >= WAKE_PIN_INACTIVE_STABLE_MS) {
        return true;
      }
    }
    if (now_ms >= next_log_ms) {
      unsigned long stable_ms = stable_start_ms ? (now_ms - stable_start_ms) : 0;
      Serial.printf("[SLEEP_SANITY] wake_pin_level=%d wake_active=%d wait_left_ms=%lu stable_ms=%lu\n",
                    level,
                    WAKE_ACTIVE_LEVEL,
                    (unsigned long)(wait_ms - (now_ms - start_ms)),
                    stable_ms);
      next_log_ms = now_ms + 100;
    }
    delay(50);
  }
  return false;
}

static void sleep_send_deny_and_clear(const char* reason, unsigned long now_ms) {
  if (sleep_deny_sent_for_request) {
    return;
  }
  uint32_t retry_ms = sleep_deny_retry_ms(reason, now_ms);
  uart_send_sleep_deny(reason, retry_ms);
  Serial.printf("[SLEEP_PROTO] tx SLEEP_DENY reason=%s retry_ms=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)retry_ms);
  sleep_deny_sent_for_request = true;
  sleep_requested = false;
  sleep_request_ms = 0;
  sleep_request_logged = false;
  sleep_coord_requested = false;
  sleep_ack_pending = false;
  sleep_ack_sent_for_request = false;
  sleep_ready_sent_for_cycle = false;
  sleep_sm_transition(SLEEP_SM_IDLE, "DENY", sleep_sm_msg_id);
  sleep_sm_msg_id = 0;
}

static void sense_enter_deep_sleep(SenseSleepKind kind) {
  Serial.println("========================================");
  Serial.println("[SENSE] Preparing for DEEP SLEEP...");
  Serial.println("========================================");
  unsigned long now_ms = millis();
  if (sleep_holdoff_until_ms > 0 && now_ms < sleep_holdoff_until_ms) {
    Serial.println("[SLEEP] inhibited reason=holdoff");
    return;
  }
  const char* sleep_kind_label = "DEEP";
  const char* sleep_mode_label = (kind == SENSE_SLEEP_DEEP_MAINT) ? "MAINT" : "IDLE";
  Serial.printf("[SLEEP_DIAG] sleep_kind=%s sleep_mode=%s\n",
                sleep_kind_label,
                sleep_mode_label);
  // Test plan:
  // 1) Trigger LCD GPIO2 wake and confirm Sense boots (EXT0) + UART handshake logs.
  // 2) Enable scheduled OTA, confirm TIMER wake enters maintenance flow.
  // 3) Verify SLEEP_ACK/SLEEP_READY logs appear before deep sleep.
  
  // Check current job state
  Serial.printf("[SENSE] Current job: type=%d, state=%d\n", current_job.type, current_job.state);

#ifdef HALO_SENSE_PROD_WRAPPER
  halo_prod_pre_sleep();
#endif

  // Ensure wake pin is RTC input with inactive pull before sleep checks.
  wake_pin_configure_rtc_input_inactive_pull();
  int wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
  int wake_pin_pullup = (WAKE_INACTIVE_LEVEL == 1) ? 1 : 0;
  int wake_pin_pulldown = (WAKE_INACTIVE_LEVEL == 0) ? 1 : 0;
  Serial.printf("[SLEEP_GPIO] gpio=%d rtc_pullup=%d pulldown=%d level_now=%d\n",
                (int)WAKE_GPIO,
                wake_pin_pullup,
                wake_pin_pulldown,
                wake_pin_level);
  Serial.printf("[WAKE_LINE] sleep_config board=%s wake_gpio=%d inactive_level=%d level_now=%d\n",
                HALO_BOARD_NAME,
                (int)WAKE_GPIO,
                WAKE_INACTIVE_LEVEL,
                wake_pin_level);
  Serial.printf("[WAKE_PIN] gpio2=%d phase=pre_sleep\n", wake_pin_level);
  bool ext0_allowed = true;
  bool wake_pin_stuck = false;
  if (wake_pin_is_active_level(wake_pin_level)) {
    Serial.printf("[SLEEP_SANITY] wake_pin_active_at_sleep_entry level=%d wake_active=%d\n",
                  wake_pin_level, WAKE_ACTIVE_LEVEL);
    if (wake_line_active_for_ms(WAKE_LINE_STUCK_WARN_MS)) {
      Serial.printf("[WAKE_LINE][WARN] stuck_active gpio=%d level=%d held_ms=%lu\n",
                    (int)WAKE_GPIO,
                    WAKE_LEVEL,
                    (unsigned long)WAKE_LINE_STUCK_WARN_MS);
    }
    uart_send_release_wake();
    sleep_retry_deassert_count++;
    Serial.printf("[SLEEP_SANITY] waiting_deassert_ms=%lu retry_count=%lu\n",
                  (unsigned long)WAKE_PIN_DEASSERT_WAIT_MS,
                  sleep_retry_deassert_count);
    if (!sleep_wait_wake_pin_deassert(WAKE_PIN_DEASSERT_WAIT_MS)) {
      wake_pin_apply_mitigation("pre_sleep");
      wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      if (wake_pin_is_active_level(wake_pin_level)) {
        wake_pin_abort_count_total++;
        wake_pin_abort_count_this_boot++;
        wake_pin_stuck_last_level = wake_pin_level;
        sleep_abort_active_pin_count++;
        strncpy(last_sleep_abort_reason, "wake_pin_stuck", sizeof(last_sleep_abort_reason) - 1);
        last_sleep_abort_reason[sizeof(last_sleep_abort_reason) - 1] = '\0';
        Serial.printf("[WAKE_LINE][WARN] stuck_active gpio=%d level=%d -> disable_ext0\n",
                      (int)WAKE_GPIO,
                      wake_pin_level);
        Serial.printf("[SLEEP_FAILSAFE_TIMER] reason=wake_pin_stuck level=%d total=%lu boot=%lu\n",
                      wake_pin_level,
                      wake_pin_abort_count_total,
                      wake_pin_abort_count_this_boot);
        ext0_allowed = false;
        wake_pin_stuck = true;
      }
    }
  } else {
    if (!sleep_wait_wake_pin_deassert(WAKE_PIN_STABLE_WAIT_MS)) {
      wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      wake_pin_abort_count_total++;
      wake_pin_abort_count_this_boot++;
      wake_pin_stuck_last_level = wake_pin_level;
      strncpy(last_sleep_abort_reason, "wake_pin_unstable", sizeof(last_sleep_abort_reason) - 1);
      last_sleep_abort_reason[sizeof(last_sleep_abort_reason) - 1] = '\0';
      Serial.printf("[SLEEP_FAILSAFE_TIMER] reason=wake_pin_unstable level=%d total=%lu boot=%lu\n",
                    wake_pin_level,
                    wake_pin_abort_count_total,
                    wake_pin_abort_count_this_boot);
      ext0_allowed = false;
      wake_pin_stuck = true;
    }
  }

  if (!sleep_ready_sent_for_cycle) {
    Serial.printf("[SLEEP] sending SLEEP_READY reason=%s\n",
                  sleep_ready_reason ? sleep_ready_reason : "unknown");
    Serial.printf("[SLEEP_PROTO] tx SLEEP_READY reason=%s\n",
                  sleep_ready_reason ? sleep_ready_reason : "unknown");
    uart_send_sleep_ready();
    sleep_ready_sent_for_cycle = true;
    sleep_sm_transition(SLEEP_SM_READY_SENT, "READY_SENT", sleep_sm_msg_id);
  }
  delay(100);
  if (ext0_allowed) {
    if (wake_pin_is_active_level(rtc_gpio_get_level(WAKE_GPIO))) {
      Serial.println("[SLEEP_SANITY] wake_pin_active_after_ready -> request_release");
      uart_send_release_wake();
    }
    if (!sleep_wait_wake_pin_deassert(500)) {
      wake_pin_apply_mitigation("post_ready");
      wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      if (wake_pin_is_active_level(wake_pin_level)) {
        wake_pin_abort_count_total++;
        wake_pin_abort_count_this_boot++;
        wake_pin_stuck_last_level = wake_pin_level;
        sleep_abort_active_pin_count++;
        strncpy(last_sleep_abort_reason, "wake_pin_stuck_post_ready", sizeof(last_sleep_abort_reason) - 1);
        last_sleep_abort_reason[sizeof(last_sleep_abort_reason) - 1] = '\0';
        Serial.printf("[SLEEP_FAILSAFE_TIMER] reason=wake_pin_stuck_post_ready level=%d total=%lu boot=%lu\n",
                      wake_pin_level,
                      wake_pin_abort_count_total,
                      wake_pin_abort_count_this_boot);
        ext0_allowed = false;
        wake_pin_stuck = true;
      }
    }
  }

  // 1. Disconnect MQTT if connected (active connections can prevent sleep)
  if (mqttClient.connected()) {
    Serial.println("[SENSE] Disconnecting MQTT before sleep...");
    mqttClient.disconnect();
    delay(100);  // Give MQTT time to clean up
  }
  
  // 2. Shut down Wi-Fi + BT before deep sleep
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[SENSE] Disconnecting Wi-Fi before deep sleep...");
  }
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  btStop();

  // 3. Ensure UART is idle
  Serial.println("[SENSE] Flushing UART buffers...");
  uart_drain_tx(150);
  
  // 4. Suspend the op_worker_task to prevent it from interfering
  if (op_worker_task_handle != NULL) {
    Serial.println("[SENSE] Suspending op_worker_task...");
    vTaskSuspend(op_worker_task_handle);
  }
  
  // 5. Small delay to let everything settle
  delay(50);
  
  // 6. Configure wake sources (EXT0 + timer)
  uint32_t timer_delta_s = ext0_allowed ? ota_get_timer_delta_s()
                                        : (wake_pin_stuck ? WAKE_PIN_FAILSAFE_TIMER_S : 0);
  sense_config_deep_sleep_wakeup(ext0_allowed, timer_delta_s);
  Serial.printf("[SLEEP_DIAG] wake_sources ext0_gpio=%d ext0_level=%d ext0_enabled=%d timer_delta_s=%lu kind=%d\n",
                WAKE_GPIO,
                WAKE_LEVEL,
                ext0_allowed ? 1 : 0,
                (unsigned long)timer_delta_s,
                (int)kind);
  bool uart_wake_enabled = uart_wake_enabled_state;
  if (uart_wake_enabled_state) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_UART);
    uart_wake_enabled_state = false;
    Serial.println("[SLEEP_DIAG] uart_wake_disabled reason=low_power");
  } else {
    Serial.println("[SLEEP_DIAG] uart_wake_disable_skip reason=not_enabled");
  }

  Serial.printf("[SLEEP_DIAG] entering_deep_sleep gpio2=%d wake_level=%d uart_wake=%d\n",
                gpio_get_level(WAKE_GPIO),
                WAKE_LEVEL,
                uart_wake_enabled ? 1 : (uart_wake_enabled_state ? 1 : 0));
  // SLEEP_READY already sent before shutdown; EXT0 remains enabled.
  if (sleep_ack_pending) {
    sleep_ack_pending = false;
  }

  Serial.printf("[SLEEP_STATE] entering_deep_sleep now_ms=%lu wake_gpio=%d wake_level=%d\n",
                (unsigned long)millis(),
                (int)WAKE_GPIO,
                WAKE_LEVEL);
  Serial.println("[SENSE] Entering DEEP SLEEP (will wake on EXT0/timer)...");
  Serial.flush();  // Final flush before sleep
  
  // Record time before sleep to verify sleep duration
  unsigned long sleep_start_time = millis();
  
  // 7. Enter deep sleep (no return)
  sleep_sm_transition(SLEEP_SM_SLEEPING, "ENTER_SLEEP", sleep_sm_msg_id);
  esp_deep_sleep_start();
  (void)sleep_start_time;
}

static void sense_enter_sleep(SenseSleepKind kind) {
  sense_enter_deep_sleep(kind);
}

static void print_wake_cause(esp_sleep_wakeup_cause_t cause) {
  const char* wake_cause_label = "OTHER";
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    wake_cause_label = "EXT0";
  } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    wake_cause_label = "EXT1";
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    wake_cause_label = "TIMER";
  } else if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    wake_cause_label = "COLD_BOOT";
  }
  Serial.println("[SLEEP_DIAG] sleep_kind=DEEP");
  Serial.printf("[SLEEP_DIAG] wake_cause=%s wake_gpio=%d wake_level=%d raw=%d\n",
                wake_cause_label,
                (int)WAKE_GPIO,
                WAKE_LEVEL,
                (int)cause);
  Serial.printf("[WAKE_STATE] cause=%s boot_count=%lu uptime_ms=%lu\n",
                wake_cause_label,
                (unsigned long)g_sense_boot_count,
                (unsigned long)millis());
}

void setup() {
#ifdef HALO_SENSE_PROD_WRAPPER
  halo_prod_pre_setup();
#endif
  initUarts();
  g_sense_boot_count++;
  last_wake_ms = millis();
  
  // Check wake-up cause
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  esp_reset_reason_t reset_reason = esp_reset_reason();
  int wake_gpio = gpio_get_level(WAKE_GPIO);
  int uart_rx = gpio_get_level((gpio_num_t)UART_RX_PIN);
  Serial.printf("[BOOT_DIAG] reset_reason=%d wake_cause=%d gpio2=%d uart_rx=%d\n",
                (int)reset_reason, (int)cause, wake_gpio, uart_rx);
  print_wake_cause(cause);
  if (reset_reason == ESP_RST_BROWNOUT ||
      reset_reason == ESP_RST_TASK_WDT ||
      reset_reason == ESP_RST_INT_WDT) {
    Serial.printf("[BOOT_DIAG] WARNING reset_reason=%d (brownout/wdt)\n", (int)reset_reason);
  }
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("[SENSE] Booted from deep sleep (EXT0 GPIO wake)");
    // Re-initialize UARTs after wake
    initUarts();
    wake_rx_sanitize();
    // Reset communication timer (LCD just woke us up)
    last_lcd_communication = millis();
    // Send READY status to LCD
    uart_send_ui_status("IDLE");
    Serial.printf("[WAKE_PIN] gpio2=%d phase=post_wake\n",
                  rtc_gpio_get_level(WAKE_GPIO));
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    ota_on_timer_wake();
  } else {
    Serial.println("[SENSE] Cold boot");
  }
  
  // Configure wake GPIO for polling (to detect wake pulses even when already awake)
  gpio_set_direction(WAKE_GPIO, GPIO_MODE_INPUT);
  gpio_pullup_en(WAKE_GPIO);
  gpio_pulldown_dis(WAKE_GPIO);
  last_wake_pin_state = gpio_get_level(WAKE_GPIO);
  last_wake_pin_check = millis();
  log_wake_pin_boot_state("boot");
  warn_wake_pin_active_at_boot(WAKE_PIN_BOOT_WARN_MS);
  Serial.printf("[WAKE_LINE] board=%s ext0_gpio=%d ext0_level=%d wake_src=lcd_int\n",
                HALO_BOARD_NAME,
                (int)WAKE_GPIO,
                WAKE_LEVEL);
  
  // Initialize last communication time (cold boot - start timer)
  last_lcd_communication = millis();
  
  // Initialize operation queues
  op_queue = xQueueCreate(10, sizeof(OpJob));
  ui_event_queue = xQueueCreate(20, sizeof(UiEvent));
  if (op_queue == NULL || ui_event_queue == NULL) {
    Serial.println("ERROR: Failed to create operation queues!");
  } else {
    Serial.println("[SETUP] Operation queues created");
  }
  
  // Initialize voice audio buffer (in PSRAM if available)
  voice_audio_buffer = (uint8_t*)heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (voice_audio_buffer == NULL) {
    // Fallback to regular heap
    voice_audio_buffer = (uint8_t*)malloc(AUDIO_BUFFER_SIZE);
  }
  if (voice_audio_buffer == NULL) {
    Serial.println("ERROR: Failed to allocate voice audio buffer!");
  } else {
    Serial.printf("[SETUP] Voice audio buffer allocated: %d bytes\n", AUDIO_BUFFER_SIZE);
  }
  
  // Initialize mutexes
  mic_mutex = xSemaphoreCreateMutex();
  if (mic_mutex == NULL) {
    Serial.println("ERROR: Failed to create mic_mutex!");
  }
  
  // Initialize I2S audio system and register voice_audio_callback
  audio_bsp_init();
  audio_set_record_callback(voice_audio_callback);
  Serial.println("[SETUP] I2S audio system initialized");
  
  // Create operation worker task (medium priority)
  xTaskCreatePinnedToCore(op_worker_task, "op_worker", 16384, NULL, 2, &op_worker_task_handle, 1);
  if (op_worker_task_handle == NULL) {
    Serial.println("ERROR: Failed to create op_worker_task!");
  } else {
    Serial.println("[SETUP] op_worker_task created with handle");
  }
  
  // Check wake-up cause
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.print("[SENSE] Boot/Wake cause: ");
  Serial.println((int)wakeup_reason);
  
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("[SENSE] Woke from deep sleep via GPIO2 (INT pin)");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.println("[SENSE] Power-on reset or first boot");
  }
  
  Serial.println("Booting…");
  
  // Phase 0: Print protocol validation message
  Serial.println("Sense ESP32-S3: booted, PROTO OK v1.");
  
  // Initialize mutex for shopping list
  g_list_mutex = xSemaphoreCreateMutex();
  if (g_list_mutex == NULL) {
    Serial.println("ERROR: Failed to create list mutex!");
  }
  
  // On initial boot, send AWAKE immediately
  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.println("[SENSE] Initial boot - sending AWAKE to LCD");
    lcdSerial.println("AWAKE");
    lcdSerial.flush();
  }

  // Register Wi-Fi event handler for connect guard state
  WiFi.onEvent(handle_wifi_event);
  
  // Connect to Wi-Fi
  if (!wifi_connect()) {
    Serial.println("Continuing without Wi-Fi (list fetch will fail).");
  }
  
  // Initialize MQTT (will connect when needed)
  Serial.println("[SETUP] MQTT system initialized (will connect on demand)");
  
  Serial.println("\nReady. Sense board initialized.");
  Serial.println("UART communication with LCD board active.");

#ifdef HALO_SENSE_PROD_WRAPPER
  halo_prod_setup();
#endif
}

void loop() {
  // Process incoming UART messages (line-based JSON protocol) - non-blocking
  while (lcdSerial.available() > 0) {
    char c = lcdSerial.read();
    if (UART_RX_DEBUG) {
      Serial.printf("[UART_RAW] rx_byte=0x%02X\n", (uint8_t)c);
    }
    if (c == '\n') {
      uart_ring_push('\n');
    } else if (c == '\r') {
      continue;
    } else if (uart_rx_is_printable(c)) {
      uart_ring_push(c);
    } else {
      uart_rx_dropped_since_frame++;
    }
  }
  uart_process_rx_ring();

  if (wifi_connect_inflight) {
    wifi_guard_poll();
  }
  if (wifi_is_connected() && wifi_state != WIFI_STATE_CONNECTED) {
    wifi_guard_set_inflight(false);
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "loop_status_connected", WL_CONNECTED);
  }
  
  // Check for refresh work request (from INPUT_WAKE)
  // Queue as background job instead of processing directly
  if (reset_wifi_requested) {
    reset_wifi_requested = false;
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_reset_wifi();
#endif
    // Skip other work this cycle so provisioning can start cleanly
    return;
  }

  if (refresh_requested) {
    refresh_requested = false;  // Clear flag immediately
    
    Serial.println("[LOOP] Queueing LIST_REFRESH job...");
    if (op_queue != NULL) {
      OpJob job = {OP_LIST_REFRESH, PRI_BG, get_next_msg_id(), millis(), OP_IDLE, false, ""};
      if (xQueueSend(op_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
        Serial.println("[LOOP] LIST_REFRESH job queued");
      } else {
        Serial.println("[LOOP] Failed to queue LIST_REFRESH (queue full?)");
        list_refresh_mark_complete("queue_failed");
      }
    } else {
      Serial.println("[LOOP] LIST_REFRESH queue unavailable");
      list_refresh_mark_complete("queue_missing");
    }
  }
  
  // Check for delete work request (from INPUT_DELETE)
  if (delete_requested) {
    delete_requested = false;  // Clear flag immediately
    
    Serial.printf("[LOOP] Processing delete request for ID: %s\n", delete_item_id);
    
    // Ensure Wi-Fi is connected
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[LOOP] Wi-Fi not connected, connecting...");
      if (!wifi_connect()) {
        Serial.println("[LOOP] Wi-Fi connection failed!");
        Serial.println("[DELETE] wifi_failed -> UI idle");
        uart_send_ui_status("IDLE");
        // Don't return - continue processing UART
      }
    }
    
    // Delete item from AWS and refresh list
    if (WiFi.status() == WL_CONNECTED) {
      delete_item_from_api(delete_item_id);
      // After delete, refresh the list
      fetch_shopping_list_from_api();
    }
  }
  
  // Process MQTT messages (needed for SCAN operation results)
  if (mqttClient.connected()) {
    mqttClient.loop();
  }
  
  // Poll wake GPIO to detect wake pulses (even when already awake)
  // This allows LCD to wake Sense board even if Sense is already awake
  unsigned long now = millis();
  if (now - last_wake_pin_check >= 50) {  // Check every 50ms
    bool current_wake_pin_state = gpio_get_level(WAKE_GPIO);
    // Detect falling edge (HIGH -> LOW) = wake pulse from LCD
    if (last_wake_pin_state && !current_wake_pin_state) {
      // Runtime pulse should never re-init UART or trigger wake logic.
      wake_pulse_seen = true;
      Serial.println("[WAKE_PULSE] seen runtime=1 action=flag_only");
    }
    last_wake_pin_state = current_wake_pin_state;
    last_wake_pin_check = now;
  }

  // Watchdog for list refresh inflight
  if (list_refresh_inflight && list_refresh_start_ms > 0) {
    unsigned long inflight_ms = now - list_refresh_start_ms;
    if (inflight_ms > LIST_REFRESH_TIMEOUT_MS) {
      Serial.printf("[LIST_REFRESH] timeout inflight=1 elapsed_ms=%lu\n", inflight_ms);
      list_refresh_mark_complete("timeout");
    }
  }
  if (wake_requested && sleep_grace_until_ms > 0 && now >= sleep_grace_until_ms) {
    wake_requested = false;
  }
  
  // Check for sleep request (from INPUT_SLEEP message)
  if (sleep_requested) {
    unsigned long now_ms = millis();
    if (sleep_request_ms > 0 && (now_ms - sleep_request_ms) > 60000) {
      Serial.println("[SLEEP] sleep_requested timeout -> proceeding");
      sleep_sm_transition(SLEEP_SM_IDLE, "TIMEOUT", sleep_sm_msg_id);
      sleep_requested = false;
      sleep_ack_pending = false;
      sleep_request_ms = 0;
      sleep_request_logged = false;
      sleep_ack_defer_logged = false;
      sleep_ack_sent_for_request = false;
      sleep_ready_sent_for_cycle = false;
      sleep_deny_sent_for_request = false;
      sleep_coord_requested = false;
      sleep_sm_msg_id = 0;
      goto loop_end;
    }
    if (!sleep_coord_requested && (now_ms - last_wake_ms < MIN_AWAKE_BEFORE_SLEEP_MS)) {
      static unsigned long last_sleep_deferral_log_ms = 0;
      if (now_ms - last_sleep_deferral_log_ms > 2000) {
        Serial.println("[SENSE] Sleep deferred - recent wake (allowing MQTT connect)");
        last_sleep_deferral_log_ms = now_ms;
      }
      last_lcd_communication = now_ms;
      goto loop_end;
    }
    if (sleep_coord_requested && (now_ms - last_wake_ms < MIN_AWAKE_BEFORE_SLEEP_MS)) {
      Serial.println("[SLEEP] coordinated deny reason=cooldown (recent_wake)");
      sleep_send_deny_and_clear("cooldown", now_ms);
      goto loop_end;
    }
#ifdef HALO_SENSE_PROD_WRAPPER
    if (halo_prod_should_delay_sleep()) {
      bool ota_busy = false;
      bool mqtt_busy = false;
      bool time_invalid = false;
      bool ota_check_busy = false;
      (void)halo_prod_should_defer_sleep_ack(&ota_busy, &mqtt_busy, &time_invalid, &ota_check_busy);
      if (pre_sleep_block_start_ms == 0) {
        pre_sleep_block_start_ms = now_ms;
      }
      unsigned long elapsed_ms = now_ms - pre_sleep_block_start_ms;
      if (elapsed_ms >= PRE_SLEEP_BLOCK_MAX_MS) {
        Serial.printf("[SLEEP] pre_sleep_cap_hit forcing_sleep elapsed_ms=%lu\n",
                      elapsed_ms);
        if (mqttClient.connected()) {
          Serial.println("[SLEEP] forcing MQTT disconnect (pre_sleep_cap)");
          mqttClient.disconnect();
          delay(100);
        }
        OtaIntent::clearDesired();
        pre_sleep_block_start_ms = 0;
      } else {
        if (!sleep_ack_defer_logged) {
          Serial.printf("[SLEEP] not ready after pre_sleep; staying awake (ota=%d mqtt=%d)\n",
                        ota_busy ? 1 : 0,
                        mqtt_busy ? 1 : 0);
          sleep_ack_defer_logged = true;
        }
        if (sleep_coord_requested) {
          const char* deny_reason = ota_busy || ota_check_busy ? "ota_pending"
                                   : (mqtt_busy ? "mqtt_pending"
                                                : (time_invalid ? "cooldown" : "op_inflight"));
          sleep_send_deny_and_clear(deny_reason, now_ms);
          pre_sleep_block_start_ms = 0;
          goto loop_end;
        }
        last_lcd_communication = now_ms;
        goto loop_end;
      }
    }
    pre_sleep_block_start_ms = 0;
#endif
    if (sleep_sm_state == SLEEP_SM_REQ_RX && sleep_ack_pending && !sleep_ack_sent_for_request) {
      uart_send_sleep_ack();
      sleep_ack_sent_for_request = true;
      sleep_ack_pending = false;
      sleep_sm_transition(SLEEP_SM_ACK_SENT, "ACK_DEFERRED", sleep_sm_msg_id);
    }
    sleep_ready_reason = sleep_coord_requested ? "coordinated" : "requested";
    sleep_requested = false;  // Clear flag only when we can proceed
    sleep_request_ms = 0;
    sleep_request_logged = false;
    sleep_ack_defer_logged = false;
    sleep_ack_sent_for_request = false;
    sleep_ready_sent_for_cycle = false;
    sleep_coord_requested = false;
    sleep_sm_transition(SLEEP_SM_IDLE, "CLEAR", sleep_sm_msg_id);
    sleep_sm_msg_id = 0;

#ifdef HALO_SENSE_PROD_WRAPPER
    if (halo_provisioning_active()) {
      Serial.println("[SENSE] Sleep ignored (provisioning active)");
      last_lcd_communication = millis();
      goto loop_end;
    }
#endif
    
    // Check if there are any active operations that should prevent sleep
    bool can_sleep = true;
    const char* block_reason = NULL;
    const char* block_op = "none";
    if (list_refresh_inflight) {
      block_reason = "list_refresh_inflight";
      block_op = "list_refresh";
    } else if (now_ms < sleep_grace_until_ms) {
      block_reason = "grace_window";
      block_op = "wake";
    } else if (wifi_connect_inflight) {
      block_reason = "op_inflight";
      block_op = "wifi";
    } else if (current_job.state != OP_IDLE && current_job.state != OP_DONE) {
      block_reason = "op_inflight";
      block_op = op_type_name(current_job.type);
    }
    if (block_reason) {
      Serial.printf("[SLEEP_BLOCK] reason=%s op=%s\n", block_reason, block_op);
      can_sleep = false;
    }
    
    if (can_sleep) {
      wake_pin_configure_rtc_input_inactive_pull();
      int wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      if (wake_pin_is_active_level(wake_pin_level)) {
        Serial.printf("[SLEEP] coordinated deny reason=wake_pin_active level=%d\n",
                      wake_pin_level);
        uart_send_release_wake();
        sleep_send_deny_and_clear("wake_pin_active", now_ms);
        goto loop_end;
      }
      if (!sleep_wait_wake_pin_deassert(WAKE_PIN_STABLE_WAIT_MS)) {
        Serial.println("[SLEEP] coordinated deny reason=wake_pin_active (unstable)");
        uart_send_release_wake();
        sleep_send_deny_and_clear("wake_pin_active", now_ms);
        goto loop_end;
      }
      Serial.println("[SENSE] No active operations - entering sleep...");
      sense_enter_sleep(SENSE_SLEEP_DEEP_IDLE);
      // After wake, reset communication timer
      last_lcd_communication = millis();
    } else {
      Serial.println("[SENSE] Sleep deferred - will retry when operation completes");
      // Keep sleep_requested flag set? No, clear it and let inactivity timeout handle it
      if (sleep_coord_requested) {
        const char* deny_reason = (block_reason && strcmp(block_reason, "grace_window") == 0)
                                    ? "cooldown"
                                    : "op_inflight";
        sleep_send_deny_and_clear(deny_reason, now_ms);
        goto loop_end;
      }
    }
  }
  
  // Idle timeout: LCD presence is optional; only sleep if no blocking ops
  if (last_lcd_communication > 0) {
    unsigned long time_since_last_comm = now - last_lcd_communication;
    unsigned long lcd_timeout = LCD_INACTIVITY_TIMEOUT_MS;
#ifdef HALO_SENSE_PROD_WRAPPER
    if (halo_provisioning_active()) {
      lcd_timeout = LCD_INACTIVITY_TIMEOUT_PROVISION_MS;
    }
#endif
    {
      unsigned long now_ms = millis();
      unsigned long idle_age_ms = now_ms - last_lcd_communication;
      unsigned long rx_age_ms = 0;
      unsigned long hb_age_ms = 0;
      bool link_recent = sense_link_recent(now_ms, &rx_age_ms, &hb_age_ms);
      const char* sleep_block_reason = "eligible";
      if (link_recent) {
        sleep_block_reason = "link_recent";
      } else if (idle_age_ms < lcd_timeout) {
        sleep_block_reason = "idle_age";
      } else if (now_ms - last_wake_ms < MIN_AWAKE_BEFORE_SLEEP_MS) {
        sleep_block_reason = "recent_wake";
      } else if (list_refresh_inflight) {
        sleep_block_reason = "list_refresh";
      } else if (wifi_connect_inflight) {
        sleep_block_reason = "wifi_connect";
      } else if (current_job.state != OP_IDLE && current_job.state != OP_DONE) {
        sleep_block_reason = "op_inflight";
      } else if (now_ms < sleep_grace_until_ms) {
        sleep_block_reason = "grace_window";
      }
      if ((now_ms - last_sleep_coord_status_log_ms) >= 10000) {
        Serial.printf("[SLEEP_COORD] sense idle_age_ms=%lu idle_timeout_ms=%lu link_recent=%d rx_age_ms=%lu sleep_block_reason=%s\n",
                      idle_age_ms,
                      lcd_timeout,
                      link_recent ? 1 : 0,
                      rx_age_ms,
                      sleep_block_reason);
        last_sleep_coord_status_log_ms = now_ms;
      }
    }
    if (time_since_last_comm >= lcd_timeout) {
      unsigned long now_ms = millis();
      unsigned long rx_age = 0;
      unsigned long hb_age = 0;
      bool link_recent = sense_link_recent(now_ms, &rx_age, &hb_age);
      if (link_recent && !sleep_coord_requested) {
        if ((now_ms - last_sleep_coord_log_ms) > 2000) {
          Serial.printf("[SLEEP_COORD] blocked_by_link link_recent=1 rx_age=%lu hb_age=%lu\n",
                        rx_age, hb_age);
          last_sleep_coord_log_ms = now_ms;
        }
        last_lcd_communication = now_ms;
        goto loop_end;
      }
      if (now_ms - last_wake_ms < MIN_AWAKE_BEFORE_SLEEP_MS) {
        Serial.println("[SENSE] Inactivity timeout ignored (recent wake)");
        last_lcd_communication = now_ms;
        goto loop_end;
      }
#ifdef HALO_SENSE_PROD_WRAPPER
      if (halo_prod_should_delay_sleep()) {
        if (pre_sleep_block_start_ms == 0) {
          pre_sleep_block_start_ms = now_ms;
        }
        unsigned long elapsed_ms = now_ms - pre_sleep_block_start_ms;
        if (elapsed_ms >= PRE_SLEEP_BLOCK_MAX_MS) {
          Serial.printf("[SLEEP] pre_sleep_cap_hit forcing_sleep elapsed_ms=%lu\n",
                        elapsed_ms);
          if (mqttClient.connected()) {
            Serial.println("[SLEEP] forcing MQTT disconnect (pre_sleep_cap)");
            mqttClient.disconnect();
            delay(100);
          }
          OtaIntent::clearDesired();
          pre_sleep_block_start_ms = 0;
        } else {
          Serial.println("[SENSE] Inactivity timeout ignored (OTA/MQTT pending)");
          last_lcd_communication = now_ms;
          goto loop_end;
        }
      }
#endif
      // Check if there are any active operations that should prevent sleep
      bool can_sleep = true;
      const char* block_reason = NULL;
      const char* block_op = "none";
      if (list_refresh_inflight) {
        block_reason = "list_refresh_inflight";
        block_op = "list_refresh";
      } else if (now_ms < sleep_grace_until_ms) {
        block_reason = "grace_window";
        block_op = "wake";
      } else if (wifi_connect_inflight) {
        block_reason = "op_inflight";
        block_op = "wifi";
      } else if (current_job.state != OP_IDLE && current_job.state != OP_DONE) {
        block_reason = "op_inflight";
        block_op = op_type_name(current_job.type);
      }
      if (block_reason) {
        Serial.printf("[SLEEP_BLOCK] reason=%s op=%s\n", block_reason, block_op);
        can_sleep = false;
        // Reset timer to check again later
        if (lcd_timeout > 10000) {
          last_lcd_communication = millis() - (lcd_timeout - 10000);  // Check again in 10 seconds
        }
      }
      
      if (can_sleep) {
        Serial.printf("[SLEEP_COORD] allow_sleep reason=inactivity link_recent=%d rx_age=%lu hb_age=%lu\n",
                      link_recent ? 1 : 0, rx_age, hb_age);
        Serial.printf("[SENSE] Idle timeout (%lu s) - entering sleep (lcd_optional=1)\n",
                      time_since_last_comm / 1000);
        sleep_ready_reason = "inactivity";
        sense_enter_sleep(SENSE_SLEEP_DEEP_IDLE);
        // After wake, reset communication timer
        last_lcd_communication = millis();
      }
    }
  }

#ifdef HALO_SENSE_PROD_WRAPPER
loop_end:
#endif

  // Periodic UART link heartbeat (suppressed during sleep handshake)
  if (link_synced && !sleep_requested && sleep_sm_state == SLEEP_SM_IDLE) {
    unsigned long now_ms = millis();
    if (now_ms - last_link_hb_ms >= LINK_HB_INTERVAL_MS) {
      unsigned long delta_ms = last_link_hb_ms > 0 ? (now_ms - last_link_hb_ms) : 0;
      last_link_hb_ms = now_ms;
      uart_send_link_hb();
      unsigned long rx_age = last_uart_rx_ms > 0 ? (now_ms - last_uart_rx_ms) : 0;
      Serial.printf("[LINK_HB] tx delta_ms=%lu rx_age=%lu\n", delta_ms, rx_age);
    }
  }

  bool idle_mode = sense_idle_mode_active();
  if (millis() - last_uart_diag_ms >= 5000) {
    unsigned long now_ms = millis();
    last_uart_diag_ms = now_ms;
    unsigned long rx_age = last_uart_rx_ms > 0 ? (now_ms - last_uart_rx_ms) : 0;
    unsigned long tx_age = last_uart_tx_ms > 0 ? (now_ms - last_uart_tx_ms) : 0;
    Serial.printf("[UART_DIAG] rx_age=%lu tx_age=%lu\n", rx_age, tx_age);
  }
  unsigned long wake_pin_log_interval_ms = idle_mode ? 8000 : 1000;
  if (millis() - last_wake_pin_log_ms >= wake_pin_log_interval_ms) {
    unsigned long now_ms = millis();
    last_wake_pin_log_ms = now_ms;
    unsigned long rx_age = last_uart_rx_ms > 0 ? (now_ms - last_uart_rx_ms) : 0;
    unsigned long tx_age = last_uart_tx_ms > 0 ? (now_ms - last_uart_tx_ms) : 0;
    Serial.printf("[WAKE_PIN] gpio2=%d phase=awake_tick rx_age=%lu tx_age=%lu\n",
                  gpio_get_level(WAKE_GPIO),
                  rx_age,
                  tx_age);
  }

#ifdef HALO_SENSE_PROD_WRAPPER
  halo_prod_loop();
#endif

  delay(idle_mode ? 50 : 10);
}



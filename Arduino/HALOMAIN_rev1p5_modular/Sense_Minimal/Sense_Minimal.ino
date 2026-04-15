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
#include <esp_heap_caps.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/rtc_io.h>
#include <lwip/inet.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <Preferences.h>
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
#include <FS.h>
#include <SPIFFS.h>
#endif
#ifdef HALO_SENSE_PROD_WRAPPER
#include "../halo_ota_demo/firmware/shared/BuildInfo.h"
void dump_system_truth(const char* reason);
#endif
#include "../halo_ota_demo/firmware/shared/WifiUtils.h"
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
void halo_prod_on_lcd_maint_ack(uint32_t remaining_s,
                                uint32_t wake_in_s,
                                bool clear,
                                const char* request_id,
                                const char* status,
                                bool persisted,
                                uint64_t start_epoch,
                                uint32_t duration_sec,
                                uint32_t grace_before_sec,
                                uint32_t grace_after_sec);
bool halo_prod_should_defer_sleep_ack(bool* ota_busy, bool* mqtt_busy, bool* time_invalid, bool* ota_check_busy);
void halo_prod_request_manual_ota(const char* reason);
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
#ifndef HALO_SENSE_LCD_DIAG_BRIDGE
#define HALO_SENSE_LCD_DIAG_BRIDGE 1
#endif
#define MAX_LINE_LENGTH 4096  // Allows richer UI_VOICE_RESPONSE payloads over UART
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
RTC_DATA_ATTR static uint8_t g_timer_wake_armed = 0;
RTC_DATA_ATTR static uint8_t g_rtc_clean_shutdown = 0;
RTC_DATA_ATTR static char g_rtc_last_stage[24] = "";
RTC_DATA_ATTR static int32_t g_rtc_last_stage_code = 0;
RTC_DATA_ATTR static uint32_t g_rtc_last_stage_uptime_ms = 0;
RTC_DATA_ATTR static uint32_t g_rtc_crash_count = 0;
RTC_DATA_ATTR static uint32_t g_rtc_last_crash_reason = 0;
RTC_DATA_ATTR static uint32_t g_rtc_last_crash_wake_cause = 0;
RTC_DATA_ATTR static char g_rtc_last_crash_stage[24] = "";
RTC_DATA_ATTR static int32_t g_rtc_last_crash_stage_code = 0;
RTC_DATA_ATTR static uint32_t g_rtc_last_crash_stage_uptime_ms = 0;
static const unsigned long WAKE_PIN_MITIGATION_MS = 20;
static const uint32_t WAKE_PIN_FAILSAFE_TIMER_S = 15;
static const unsigned long WAKE_PIN_BOOT_WARN_MS = 200;
static const unsigned long WAKE_LINE_STUCK_WARN_MS = 3000;
static const uint32_t SLEEP_DENY_RETRY_DEFAULT_MS = 5000;
static const uint32_t SLEEP_DENY_RETRY_COOLDOWN_MAX_MS = 30000;
static bool last_wake_pin_state = false;
static unsigned long last_wake_pin_check = 0;
static unsigned long last_wake_ms = 0;  // Track last wake time for sleep guard
static const unsigned long GUARDIAN_FORCE_SLEEP_MS = 5UL * 60UL * 1000UL;
static unsigned long guardian_awake_start_ms = 0;
static bool guardian_force_sleep = false;
static const unsigned long MIN_AWAKE_BEFORE_SLEEP_MS = 10000;  // Give MQTT time to connect
static const uint32_t ACTION_AWAKE_BUDGET_MS = 60000;
static const uint32_t ACTION_MIN_REMAINING_MS = 1000;
static const uint32_t BACKGROUND_UPLOAD_PRESIGN_BUDGET_MS = 30000;
static const uint32_t BACKGROUND_UPLOAD_PUT_BUDGET_MS = 120000;
static uint32_t g_sense_boot_count = 0;

// ── Sleep Timeout Configuration ────────────────────────────────────
static unsigned long last_lcd_communication = 0;  // Track last time we received a message from LCD
static const unsigned long LCD_INACTIVITY_TIMEOUT_MS = 10000;  // 10 seconds (LCD is sleep leader)
static const unsigned long LCD_INACTIVITY_TIMEOUT_PROVISION_MS = 300000;  // 5 minutes during provisioning

// ── UART Heartbeat (Sense -> LCD) ─────────────────────────────────────
static const unsigned long LINK_HB_INTERVAL_MS = 4000;
static unsigned long last_link_hb_ms = 0;

// ── Dish result timing ───────────────────────────────────────────────
static const uint32_t DISH_RESULT_TIMEOUT_MS = 60000;  // Wait up to 60s for HTTP final result

// ── Wi-Fi ───────────────────────────────────────────────────────────
const char* WIFI_SSID = "Garage Member";
const char* WIFI_PASS = "build00!";

// ── Trepo API Configuration ────────────────────────────────────
const char* TREPO_API_BASE_URL = "https://1zc0nh8x48.execute-api.us-east-1.amazonaws.com";
const char* TREPO_API_HOST = "1zc0nh8x48.execute-api.us-east-1.amazonaws.com";
const char* TREPO_LIST_ENDPOINT = "/v1/list";
const char* TREPO_VOICE_ENDPOINT = "/v1/voice";
const char* TREPO_OWNER_ID = "";
const char* TREPO_DEVICE_ID = "*";

// ── Quick-Ack API Configuration (OpenAI Realtime API) ──────────
const char* QUICK_ACK_BASE_URL = "https://ivwu7ls6p8.execute-api.us-east-1.amazonaws.com";
const char* QUICK_ACK_ENDPOINT = "/voice-ack-async";

// ── Camera/Presign API Configuration ──────────────────────────
const char* API_KEY = "";  // Optional x-api-key header
const char* BEARER_TOKEN = "";  // Optional Authorization header
const char* USER_ID = "";

// ── Check-in / Dish / Discard API Configuration ───────────────
const char* CHECKIN_API_BASE_URL = "https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com";
const char* CHECKIN_PRESIGN_ENDPOINT = "/presign";
const char* CHECKIN_OWNER_ID = "";  // Fallback owner UUID (disabled)
const char* DISH_PRESIGN_URL = "https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com/presign";
const char* DISCARD_PRESIGN_ENDPOINT = "/presign/discard";  // Legacy fallback for discard

// ── Camera Configuration ───────────────────────────────────────
// Note: CAMERA_MODEL_XIAO_ESP32S3 is defined above before camera_pins.h include
static const gpio_num_t CAM_PWDN_GPIO = GPIO_NUM_1;
static framesize_t CAPTURE_SIZE = FRAMESIZE_SXGA;  // 1280x1024 (low-light preset)
static int JPEG_QUALITY = 10;  // Reliability-first production baseline
static int CAMERA_XCLK_HZ = 10000000;  // 10 MHz (low-light preset)
static const int FILL_LED_PIN = GPIO_NUM_4;  // D3 flash LED switch
static const uint32_t CAMERA_PWDN_WAKE_DELAY_MS = 15;
static const uint32_t CAMERA_PWDN_DISABLE_DELAY_MS = 2;
static bool g_cam_pwdn_hold_enabled = false;
#ifndef CAMERA_DIAG
#define CAMERA_DIAG 1
#endif
static const framesize_t CAMERA_PREFLIGHT_FRAMESIZE = FRAMESIZE_QQVGA;  // 160x120 (fast preflight)
static const int CAMERA_PREFLIGHT_LUMA_LOW = 60;
static const int CAMERA_PREFLIGHT_LUMA_HIGH = 220;
static const int CAMERA_PREFLIGHT_GREEN_RATIO_PCT = 140;  // G > 1.4x avg(R,B)
static int32_t g_camera_last_init_err = 0;
static const uint32_t CAMERA_UI_CAPTURE_DELAY_MS = 0;
static const uint32_t CAMERA_PREFLIGHT_SETTLE_MS = 40;
static const uint8_t CAMERA_PREFLIGHT_WARMUP_FRAMES = 2;
static const uint32_t CAMERA_PREFLIGHT_WARMUP_DELAY_MS = 80;
static const uint32_t CAMERA_PREFLIGHT_BUDGET_MS = 1000;
static const uint32_t CAMERA_INIT_SETTLE_DELAY_MS = 150;
static const uint8_t CAMERA_INIT_WARMUP_FRAMES = 1;
static const uint32_t CAMERA_INIT_WARMUP_DELAY_MS = 80;
static const size_t CAMERA_DMA_LARGEST_BLOCK_MIN_BYTES = 24 * 1024;
static const uint32_t CAMERA_NETWORK_QUIESCE_DELAY_MS = 250;
static const uint32_t CAMERA_CAPTURE_SETTLE_MS = 40;
static const uint32_t CAMERA_WARMUP_DELAY_FAST_MS = 30;
static const uint32_t CAMERA_WARMUP_DELAY_SLOW_MS = 50;
static const uint32_t CAMERA_CAPTURE_RETRY_DELAY_MS = 60;
static const uint32_t CAMERA_RETRY_SETTLE_MS = 80;
static const uint32_t CAMERA_PROFILE_SWITCH_SETTLE_MS = 100;
static const uint32_t CAMERA_SVGA_FALLBACK_SETTLE_MS = 80;
static const bool CAMERA_ENABLE_REINIT_FALLBACK = false;
enum CameraPreflightMode { CAMERA_PREFLIGHT_OFF = 0, CAMERA_PREFLIGHT_ON_FAIL = 1, CAMERA_PREFLIGHT_ALWAYS = 2 };
static const CameraPreflightMode CAMERA_PREFLIGHT_MODE = CAMERA_PREFLIGHT_OFF;
static const uint32_t CAMERA_CAPTURE_TARGET_MS = 3000;
static const uint32_t CAMERA_CAPTURE_BUDGET_FAST_MS = 4000;
static const uint32_t CAMERA_CAPTURE_BUDGET_SLOW_MS = 8000;
enum CameraProfile { CAM_PROFILE_NORMAL = 0, CAM_PROFILE_LOW_LIGHT = 1, CAM_PROFILE_FLASH = 2 };

// ── MQTT Configuration ────────────────────────────────────────
const char* awsEndpoint = "arq86ma48kw9j-ats.iot.us-east-1.amazonaws.com";
const char* RESULT_TOPIC_TEMPLATE = "trepo/%s/%s/jobs/%s/result";
static unsigned long mqtt_wait_deadline = 0;
static bool mqtt_subscribed = false;
static String current_scan_job_id = "";  // Track SCAN job ID for MQTT matching
static String current_result_topic = "";
static String subscribed_result_topic = "";
static volatile bool waiting_for_mqtt_result = false;
static uint32_t active_dish_job_id = 0;
static uint32_t current_result_local_job_id = 0;
static char current_result_mode[16] = "";
struct DishTimingTrace {
  uint32_t sense_job_id;
  uint32_t ui_wait_start_ms;
  uint32_t upload_start_ms;
  uint32_t presign_start_ms;
  uint32_t presign_end_ms;
  uint32_t put_start_ms;
  uint32_t put_end_ms;
  uint32_t mqtt_refresh_start_ms;
  uint32_t mqtt_refresh_end_ms;
  uint32_t result_wait_start_ms;
};
static DishTimingTrace g_dish_timing = {};

// ── Owner/Provisioning Helpers ─────────────────────────────────────
static void load_owner_id_or_default(char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  char owner_code[32] = {0};
  if (ProvisioningState::loadOwnerCode(owner_code, sizeof(owner_code))) {
    // Owner code pending -> block stale owner_id usage until claim completes.
    Serial.println("[OWNER] owner_code pending -> suppress owner_id");
    out[0] = '\0';
    return;
  }
  bool ok = ProvisioningState::loadOwnerId(out, out_len);
  if (!ok || out[0] == '\0') {
    out[0] = '\0';
  }
}

static void load_runtime_device_id(char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';

  uint64_t mac = ESP.getEfuseMac();
  snprintf(out, out_len, "halo-%02x%02x-%02x%02x",
           (unsigned)((mac >> 24) & 0xFF),
           (unsigned)((mac >> 16) & 0xFF),
           (unsigned)((mac >> 8) & 0xFF),
           (unsigned)(mac & 0xFF));
}

static String build_runtime_ota_topic() {
  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  return String("trepo/halo/") + device_id + "/ota";
}

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

#include "sense_ops.h"

// ── Expiration Date Storage (for check-in mode) ──────────────────────
static char pending_expiry_date[16] = "";  // Store expiration date received from LCD
static bool expiry_date_response_received = false;  // Flag to indicate expiry date response was received (even if empty)
static OpJob* active_checkin_job = NULL;  // Pointer to active check-in job (if any)
static uint16_t pending_quantity = 1;  // Store quantity received from LCD (default 1)
static bool pending_discard_add_to_shopping_list = false;
static bool discard_choice_response_received = false;
static OpJob* active_discard_job = NULL;

// ── VOICE Operation State ──────────────────────────────────────────
#define AUDIO_BUFFER_SIZE (512 * 1024)  // 512KB buffer (~6 seconds at 24kHz, 16-bit mono)
static uint8_t* voice_audio_buffer = NULL;
static size_t voice_audio_pos = 0;
static size_t voice_audio_size = 0;
static bool voice_recording_active = false;
static volatile bool voice_finalize_requested = false;
static uint16_t voice_peak_abs = 0;
static uint64_t voice_sum_abs = 0;
static size_t voice_sample_count = 0;
static size_t voice_nonzero_sample_count = 0;
static SemaphoreHandle_t mic_mutex = NULL;
static SemaphoreHandle_t wifi_connect_mutex = NULL;
static char g_voice_session_id[96] = "";
static unsigned long g_voice_session_last_turn_ms = 0;
static const unsigned long VOICE_SESSION_IDLE_TIMEOUT_MS = 90000;
static esp_reset_reason_t g_boot_reset_reason = ESP_RST_UNKNOWN;

enum SenseSleepKind { SENSE_SLEEP_DEEP_IDLE = 0, SENSE_SLEEP_DEEP_MAINT = 1 };

// Forward declarations
static bool voice_upload_and_parse(const uint8_t* audio_buf, size_t audio_size, uint32_t voice_job_id);
static void voice_audio_callback(const int16_t *samples, size_t num_samples);
static void voice_request_finalize(const char* reason);
static bool voice_ensure_wifi_connected();
static void voice_begin_wifi_preconnect();
static void voice_session_reset(const char* reason);
static const char* voice_session_get_or_create();
static void voice_session_note_response(const String& response_json);
static void service_boot_wifi_connect(unsigned long now_ms);
static bool enqueue_op_job(const OpJob& job, bool prioritize_front, const char* source);
static bool foreground_priority_active(unsigned long now_ms, const char** reason_out);
static bool requeue_upload_job(const UploadJob& job, bool prioritize_front, const char* reason);
static bool park_upload_job_if_foreground_active(const UploadJob& job, const char* stage);
static bool upload_wait_for_foreground_clear_in_place(const UploadJob& job,
                                                      const char* stage,
                                                      uint32_t deadline_ms,
                                                      bool* budget_exhausted);
static bool sleep_reason_is_background_deferable(const char* reason);
static void sleep_background_force_reset();
static bool sleep_background_force_ready(unsigned long now_ms, const char* reason, const char* where);
static void sleep_defer_queued_background_uploads();
// http_queue_lock, http_queue_unlock → sense_upload.h
// camera_profile_name, init_camera, deinit_camera, capture_fill_led_set,
// camera_pwdn_gpio_init, camera_power_*, camera_stop_xclk, camera_set_pins_high_z,
// tune_sensor_*, apply_sensor_profile_*, apply_camera_profile,
// capture_camera_meta_snapshot, append_camera_meta_json, log_camera_meta_for_presign,
// compute_scene_brightness_preflight, is_frame_quality_ok, camera_settle_discard,
// warmup_and_capture, camera_timeline_complete → sense_camera.h
static void uart_send_ui_status(const char* text);
// SCAN operation forward declarations
struct PresignReply {
  String job_id;
  String put_url;
  String s3_key;
  String content_type;
  String result_url;
  int    ttl_s;

  PresignReply() : ttl_s(0) {}  // Constructor to initialize ttl_s
};
static void clear_active_dish_job(uint32_t job_id, const char* reason);
static bool do_presign_request_simple(const char* url,
                                      const char* type,
                                      PresignReply& out,
                                      int& http_code,
                                      String& resp_body,
                                      uint32_t deadline_ms = 0);
static bool do_presign_request(const char* base_url,
                               const char* endpoint,
                               const char* type,
                               const char* action,
                               const char* owner,
                               const char* expiry_date,
                               bool add_to_shopping_list,
                               const UploadJob::CameraUploadMeta* camera_meta,
                               PresignReply& out,
                               int& http_code,
                               String& resp_body,
                               uint32_t deadline_ms = 0);
static bool get_presign(PresignReply& out,
                        const char* mode,
                        const char* expiry_date,
                        bool add_to_shopping_list,
                        const UploadJob::CameraUploadMeta* camera_meta = nullptr,
                        uint32_t deadline_ms = 0);
static bool get_presign_checkin(PresignReply& out, const char* expiry_date = NULL, uint16_t quantity = 1, const UploadJob::CameraUploadMeta* camera_meta = nullptr, uint32_t deadline_ms = 0);
// net_ready_for_tls(char*,size_t) → sense_upload.h
static bool net_ready_for_tls(const char* reason, uint32_t timeout_ms, const char* mode, uint32_t job_id, const char* ui_policy = NULL);
static uint32_t upload_queue_count();
static bool upload_queue_is_full();
static bool sense_can_sleep_now(const char** reason);
static void scan_terminal_reset();
static void scan_send_terminal_status(const char* phase, const char* text, const char* mode, bool immediate_error = false);
// presign_set_error_text, presign_error_text → sense_upload.h
// clamp_timeout_ms → sense_http.h
// ensure_time_valid, ensure_wifi_ready, wifi_hard_reset_and_reconnect,
// wifi_recover_if_needed → sense_wifi.h
static uint8_t* allocate_upload_buffer(size_t len, bool* used_psram);
static bool queue_upload_job(uint32_t job_id,
                             const char* mode,
                             const char* expiry,
                             uint16_t quantity,
                             bool add_to_shopping_list,
                             const UploadJob::CameraUploadMeta* camera_meta,
                             uint8_t* image_buf,
                             size_t image_len,
                             uint8_t retries = 0,
                             bool from_persisted = false,
                             uint32_t created_epoch = 0);
static bool queue_voice_upload_job(uint32_t job_id,
                                   uint8_t* audio_buf,
                                   size_t audio_len,
                                   uint8_t retries = 0,
                                   bool from_persisted = false,
                                   uint32_t created_epoch = 0);
// http_post_json_with_retries, http_get_with_retries → sense_upload.h
static String build_dish_result_url(const char* user_id, const char* device_id, const char* job_id);
static uint32_t dish_result_poll_delay_ms(const JsonDocument& doc, uint32_t fallback_ms);
static bool wait_for_dish_result_http(const UploadJob& job,
                                      const PresignReply& presign,
                                      uint32_t job_deadline_ms);
static bool put_to_presigned_url(const String& url,
                                 const uint8_t* buf,
                                 size_t len,
                                 const char* contentType,
                                 uint32_t job_id = 0,
                                 bool allow_abort = false,
                                 bool* aborted_for_dish = NULL,
                                 uint32_t deadline_ms = 0,
                                 bool* aborted_for_budget = NULL);
// connect_to_mqtt, mqtt_ensure_connected, on_mqtt_message,
// build_result_topic, mqtt_clear_result_subscription,
// mqtt_subscribe_result_topic_if_needed, mqtt_reset_connection → sense_mqtt.h
static void uart_send_ui_meal_result(int kcal, const char* meal_summary, int health_score, const char* recommendation, const char* mode = NULL, float protein_g = 0.0, float carbs_g = 0.0, float fat_g = 0.0, float confidence = 0.0, uint32_t job_id = 0);
static const char* sense_user_state_name();
static const char* sense_device_state_name();
// Definitions in sense_sleep.h (late include) — forward declarations
// needed for callers above the include point.
static void sleep_send_deny_and_clear(const char* reason, unsigned long now_ms);
static bool wake_pin_is_active_level(int level);
static void wake_pin_configure_rtc_input_inactive_pull();
static uint32_t sleep_deny_retry_ms(const char* reason, unsigned long now_ms);
static void sense_enter_deep_sleep(SenseSleepKind kind);
static void print_wake_cause(esp_sleep_wakeup_cause_t cause);
static void sense_enter_sleep(SenseSleepKind kind);
static void apply_public_dns_for_api(const char* reason);
static bool ensure_dns_ready(const char* host);

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
static QueueHandle_t upload_queue = NULL;
static QueueHandle_t upload_queue_dish = NULL;
static const uint8_t UPLOAD_QUEUE_MAX = 10;
static const uint8_t UPLOAD_QUEUE_DISH_MAX = 10;
static const uint8_t OP_QUEUE_MAX = 20;
static TaskHandle_t upload_worker_task_handle = NULL;
static bool scan_ui_inflight = false;
static volatile bool dish_scan_inflight = false;
static volatile bool upload_inflight = false;
static SemaphoreHandle_t http_mutex = NULL;
static volatile bool http_inflight = false;
static bool wifi_recover_requested = false;
static bool boot_wifi_connect_pending = false;
static unsigned long boot_wifi_connect_earliest_ms = 0;
static unsigned long boot_wifi_connect_last_attempt_ms = 0;
static unsigned long boot_wifi_last_defer_log_ms = 0;
static TaskHandle_t op_worker_task_handle = NULL;  // Handle to suspend/resume task
static bool scan_terminal_sent = false;
static char presign_last_error_text[64] = "";
static bool sntp_started = false;
static const time_t TIME_VALID_MIN_EPOCH = 1700000000;
static const char* TIME_CACHE_NS = "time_cache";
static CameraProfile g_camera_profile = CAM_PROFILE_LOW_LIGHT;
static bool g_camera_preflight_force = false;
static int g_last_scene_luma = -1;
static int g_last_scene_green_ratio = -1;
static const char* TIME_CACHE_EPOCH_KEY = "epoch";
RTC_DATA_ATTR static uint32_t g_time_cache_epoch = 0;
static bool g_time_cache_loaded = false;

static uint32_t time_cache_load() {
  if (g_time_cache_epoch >= (uint32_t)TIME_VALID_MIN_EPOCH) {
    return g_time_cache_epoch;
  }
  if (g_time_cache_loaded) {
    return g_time_cache_epoch;
  }
  g_time_cache_loaded = true;
  Preferences prefs;
  if (prefs.begin(TIME_CACHE_NS, true)) {
    uint32_t epoch = prefs.getUInt(TIME_CACHE_EPOCH_KEY, 0);
    prefs.end();
    if (epoch >= (uint32_t)TIME_VALID_MIN_EPOCH) {
      g_time_cache_epoch = epoch;
    }
  }
  return g_time_cache_epoch;
}

static void time_cache_store(time_t now) {
  if (now < TIME_VALID_MIN_EPOCH) {
    return;
  }
  uint32_t epoch = (uint32_t)now;
  if (g_time_cache_epoch >= (uint32_t)TIME_VALID_MIN_EPOCH &&
      epoch < g_time_cache_epoch + 3600) {
    return;
  }
  g_time_cache_epoch = epoch;
  g_time_cache_loaded = true;
  Preferences prefs;
  if (prefs.begin(TIME_CACHE_NS, false)) {
    prefs.putUInt(TIME_CACHE_EPOCH_KEY, g_time_cache_epoch);
    prefs.end();
  }
}

static bool time_cache_bootstrap(const char* reason) {
  time_t now = time(nullptr);
  if (now >= TIME_VALID_MIN_EPOCH) {
    return true;
  }
  uint32_t cached = time_cache_load();
  if (cached < (uint32_t)TIME_VALID_MIN_EPOCH) {
    return false;
  }
  timeval tv = {};
  tv.tv_sec = (time_t)cached;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.printf("[TLS_GUARD] time_bootstrap epoch=%lu reason=%s\n",
                (unsigned long)cached,
                reason ? reason : "unknown");
  return true;
}

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
static uint32_t uart_tx_count = 0;
static uint32_t uart_rx_count = 0;
static unsigned long last_uart_tx_type_ms = 0;
static unsigned long last_uart_rx_type_ms = 0;
static char last_uart_tx_type[24] = "";
static char last_uart_rx_type[24] = "";

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

#include "sense_diag.h"
#include "sense_uart.h"
#include "sense_http.h"
#include "sense_wifi.h"
#include "sense_upload.h"
#include "sense_mqtt.h"
#include "sense_voice.h"
#include "sense_list.h"
#include "sense_camera.h"
static unsigned long last_lcd_diag_ms = 0;
static char lcd_diag_wake[16] = "";
static char lcd_diag_screen[16] = "";
static char lcd_diag_last_input[24] = "";
static char lcd_diag_last_tx[24] = "";
static char lcd_diag_last_rx[24] = "";
static int32_t lcd_diag_input_age_ms = -1;
static int32_t lcd_diag_sense_rx_age_ms = -1;
static uint32_t lcd_diag_uart_tx = 0;
static uint32_t lcd_diag_uart_rx = 0;
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


// camera_timeline_complete → sense_camera.h

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
  (void)send_status;
  if (reason && reason[0]) {
    strncpy(g_last_refresh_reason, reason, sizeof(g_last_refresh_reason) - 1);
    g_last_refresh_reason[sizeof(g_last_refresh_reason) - 1] = '\0';
  }
  Serial.printf("[LIST_REFRESH] request ignored reason=%s (shopping list disabled)\n",
                reason ? reason : "unknown");
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

static char delete_item_id[64] = {0};
volatile bool sleep_requested = false;  // Flag to request sleep from main loop
static bool sleep_ack_defer_logged = false;
static bool sleep_request_logged = false;
static unsigned long sleep_request_ms = 0;
static bool uart_wake_enabled_state = false;
static bool sleep_ready_sent_for_cycle = false;
static bool sleep_coord_requested = false;
static bool sleep_coord_pending_for_ready = false;
static const char* sleep_ready_reason = "unknown";
static unsigned long pre_sleep_block_start_ms = 0;
static unsigned long background_sleep_block_start_ms = 0;
static bool background_sleep_bypass_active = false;
static const unsigned long PRE_SLEEP_BLOCK_MAX_MS = 10000;
static unsigned long last_sleep_block_log_ms = 0;
static const unsigned long SLEEP_BLOCK_LOG_INTERVAL_MS = 2000;
static char last_sleep_block_reason[32] = {0};
static char last_sleep_block_where[24] = {0};
static char last_sleep_block_op[24] = {0};

static bool sleep_intent_pending = false;
static unsigned long sleep_intent_sent_ms = 0;
static unsigned long sleep_intent_cooldown_until_ms = 0;
static const unsigned long SENSE_SLEEP_INTENT_TIMEOUT_MS = 5000;

enum SleepSmState {
  SLEEP_SM_IDLE = 0,
  SLEEP_SM_REQ_RX,
  SLEEP_SM_READY_SENT,
  SLEEP_SM_SLEEPING
};

static SleepSmState sleep_sm_state = SLEEP_SM_IDLE;
static uint32_t sleep_sm_msg_id = 0;

static volatile bool wake_pulse_seen = false;
static const unsigned long WAKE_PIN_DEASSERT_WAIT_MS = 1500;
static unsigned long sleep_abort_active_pin_count = 0;
static unsigned long sleep_retry_deassert_count = 0;
static char last_sleep_abort_reason[32] = "";
static unsigned long wake_pin_abort_count_total = 0;
static unsigned long wake_pin_abort_count_this_boot = 0;
static int wake_pin_stuck_last_level = -1;
static bool sleep_deny_sent_for_request = false;


// camera_profile_name, capture_camera_meta_snapshot,
// append_camera_meta_json, log_camera_meta_for_presign → sense_camera.h

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
  // Sense_Minimal/Sense_Minimal.ino: uart_send_ui_status
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

static const char* get_sense_fw_version() {
#ifdef HALO_SENSE_PROD_WRAPPER
  return kFirmwareVersion ? kFirmwareVersion : "unknown";
#else
  return "unknown";
#endif
}

static void uart_send_fw_info() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "FW_INFO";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["sense_fw"] = get_sense_fw_version();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.println("[UART_TX] FW_INFO sent");
}

static const char* scan_screen_hint(const char* phase, const char* mode);

static void uart_send_ui_status_extended(const char* op, const char* phase, const char* text, const char* mode = NULL, uint32_t job_id = 0, const char* ui_policy = NULL, const char* screen_hint = NULL) {
  // Sense_Minimal/Sense_Minimal.ino: uart_send_ui_status_extended
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
  if (job_id == 0 && current_job.active && current_job.job_id > 0 && op != NULL && strcmp(op, "SCAN") == 0) {
    job_id = current_job.job_id;
  }
  if (job_id > 0) {
    doc["job_id"] = job_id;
  }
  if (ui_policy && ui_policy[0]) {
    doc["ui_policy"] = ui_policy;
  }
  if (!screen_hint && op != NULL && strcmp(op, "SCAN") == 0) {
    screen_hint = scan_screen_hint(phase, mode);
  }
  if (screen_hint && screen_hint[0]) {
    doc["debug_screen_hint"] = screen_hint;
  }
  String output;
  serializeJson(doc, output);
  Serial.printf("[UART_TX] UI_STATUS msg_id=%u\n", (unsigned)msg_id);
  uart_send_json(output.c_str());
}

static const char* scan_screen_hint(const char* phase, const char* mode) {
  const char* safe_phase = phase ? phase : "";
  if (strcmp(safe_phase, "CAPTURING") == 0) {
    return "SHIP_HOLD_STILL";
  }
  if (strcmp(safe_phase, "WAITING_INPUT") == 0) {
    return "SHIP_EXPIRY";
  }
  if (strcmp(safe_phase, "DONE") == 0) {
    return "SHIP_LOGGED";
  }
  if (strcmp(safe_phase, "UPLOAD_STARTING") == 0 ||
      strcmp(safe_phase, "UPLOADING") == 0 ||
      strcmp(safe_phase, "RESULT_WAITING") == 0 ||
      strcmp(safe_phase, "PROCESSING") == 0) {
    return "SHIP_PROCESSING";
  }
  if (strcmp(safe_phase, "RESULT_READY") == 0) {
    return "MEAL_RESULT";
  }
  if (strcmp(safe_phase, "ERROR") == 0) {
    return "SHIP_RESULT";
  }
  (void)mode;
  return "";
}

static bool scan_mode_is_quiet(const char* mode) {
  return mode && (strcmp(mode, "check-in") == 0 ||
                  strcmp(mode, "check-out") == 0 ||
                  strcmp(mode, "check_out") == 0 ||
                  strcmp(mode, "discard") == 0);
}

static bool scan_mode_is_check(const char* mode) {
  return mode && (strcmp(mode, "check-in") == 0 ||
                  strcmp(mode, "check-out") == 0 ||
                  strcmp(mode, "check_out") == 0);
}

static bool scan_mode_is_discard(const char* mode) {
  return mode && (strcmp(mode, "discard") == 0);
}

static bool scan_mode_is_dish(const char* mode) {
  return mode && (strcmp(mode, "dish") == 0 ||
                  strcmp(mode, "dish-log") == 0 ||
                  strcmp(mode, "dish_log") == 0);
}

static bool enqueue_op_job(const OpJob& job, bool prioritize_front, const char* source) {
  if (op_queue == NULL) {
    Serial.printf("[OP_QUEUE] unavailable type=%d source=%s\n",
                  (int)job.type,
                  source ? source : "unknown");
    return false;
  }
  BaseType_t queued = prioritize_front
                          ? xQueueSendToFront(op_queue, &job, pdMS_TO_TICKS(10))
                          : xQueueSend(op_queue, &job, pdMS_TO_TICKS(10));
  Serial.printf("[OP_QUEUE] %s type=%d pri=%d id=%lu source=%s front=%d depth=%lu\n",
                queued == pdTRUE ? "queued" : "queue_failed",
                (int)job.type,
                (int)job.pri,
                (unsigned long)job.job_id,
                source ? source : "unknown",
                prioritize_front ? 1 : 0,
                (unsigned long)(op_queue ? uxQueueMessagesWaiting(op_queue) : 0));
  return queued == pdTRUE;
}

static bool foreground_priority_active(unsigned long now_ms, const char** reason_out) {
  const char* reason = NULL;
  if (foreground_active ||
      (current_job.active &&
       current_job.pri == PRI_USER &&
       current_job.state != OP_IDLE &&
       current_job.state != OP_DONE)) {
    reason = "foreground_active";
  } else if (scan_ui_inflight || dish_scan_inflight) {
    reason = "scan_ui_inflight";
  } else if (op_queue != NULL && uxQueueMessagesWaiting(op_queue) > 0) {
    OpJob queued_job = {};
    if (xQueuePeek(op_queue, &queued_job, 0) == pdTRUE && queued_job.pri == PRI_USER) {
      reason = "queued_user_job";
    }
  }
  if (reason == NULL && last_input_wake_ms > 0 && (now_ms - last_input_wake_ms) < 2500UL) {
    reason = "recent_input_wake";
  }
  if (reason == NULL && last_user_activity_ms > 0 && (now_ms - last_user_activity_ms) < 1800UL) {
    reason = "recent_user_input";
  }
  if (reason == NULL && last_lcd_communication > 0 && (now_ms - last_lcd_communication) < 1000UL) {
    reason = "recent_lcd_link";
  }
  if (reason_out) {
    *reason_out = reason;
  }
  return reason != NULL;
}

static bool foreground_priority_reason_is_transient(const char* reason) {
  if (!reason) {
    return false;
  }
  return strcmp(reason, "recent_input_wake") == 0 ||
         strcmp(reason, "recent_user_input") == 0 ||
         strcmp(reason, "recent_lcd_link") == 0;
}

static bool upload_wait_for_foreground_window(const UploadJob& job,
                                              const char* initial_reason,
                                              uint32_t max_wait_ms) {
  if (!foreground_priority_reason_is_transient(initial_reason)) {
    return false;
  }
  unsigned long start_ms = millis();
  unsigned long last_log_ms = 0;
  while ((millis() - start_ms) < max_wait_ms) {
    const char* active_reason = NULL;
    if (!foreground_priority_active(millis(), &active_reason)) {
      return true;
    }
    if (!foreground_priority_reason_is_transient(active_reason)) {
      return false;
    }
    unsigned long now_ms = millis();
    if (last_log_ms == 0 || (now_ms - last_log_ms) >= 700UL) {
      Serial.printf("[UPLOAD_QUEUE] hold_in_place job_id=%lu mode=%s voice=%d reason=%s waited=%lu\n",
                    (unsigned long)job.job_id,
                    job.mode,
                    job.is_voice ? 1 : 0,
                    active_reason ? active_reason : "foreground_priority",
                    (unsigned long)(now_ms - start_ms));
      last_log_ms = now_ms;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  return false;
}

static bool upload_worker_has_parked_job = false;
static UploadJob upload_worker_parked_job = {};
static const char* upload_worker_parked_stage = "idle";
static unsigned long upload_worker_parked_at_ms = 0;

static void upload_worker_park_job(const UploadJob& job, const char* stage, const char* reason) {
  upload_worker_parked_job = job;
  upload_worker_has_parked_job = true;
  upload_worker_parked_stage = stage ? stage : "foreground";
  upload_worker_parked_at_ms = millis();
  Serial.printf("[UPLOAD_QUEUE] parked job_id=%lu mode=%s voice=%d stage=%s reason=%s q=%lu\n",
                (unsigned long)job.job_id,
                job.mode,
                job.is_voice ? 1 : 0,
                upload_worker_parked_stage,
                reason ? reason : "foreground_priority",
                (unsigned long)upload_queue_count());
}

static bool park_upload_job_if_foreground_active(const UploadJob& job, const char* stage) {
  const char* reason = NULL;
  if (!foreground_priority_active(millis(), &reason)) {
    return false;
  }
  if (upload_wait_for_foreground_window(job, reason, 2200UL)) {
    return false;
  }
  upload_worker_park_job(job, stage, reason);
  return true;
}

static bool upload_wait_for_foreground_clear_in_place(const UploadJob& job,
                                                      const char* stage,
                                                      uint32_t deadline_ms,
                                                      bool* budget_exhausted) {
  const char* reason = NULL;
  if (!foreground_priority_active(millis(), &reason)) {
    return true;
  }
  if (upload_wait_for_foreground_window(job, reason, 2200UL)) {
    return true;
  }
  unsigned long parked_start_ms = millis();
  unsigned long last_log_ms = 0;
  while (foreground_priority_active(millis(), &reason)) {
    if (deadline_ms != 0) {
      uint32_t remaining_ms = deadline_remaining_ms(deadline_ms);
      if (remaining_ms < ACTION_MIN_REMAINING_MS) {
        if (budget_exhausted) {
          *budget_exhausted = true;
        }
        return false;
      }
    }
    unsigned long now_ms = millis();
    if (last_log_ms == 0 || (now_ms - last_log_ms) >= 1000UL) {
      Serial.printf("[UPLOAD_QUEUE] parked_in_place job_id=%lu mode=%s voice=%d stage=%s reason=%s waited=%lu\n",
                    (unsigned long)job.job_id,
                    job.mode,
                    job.is_voice ? 1 : 0,
                    stage ? stage : "foreground",
                    reason ? reason : "foreground_priority",
                    (unsigned long)(now_ms - parked_start_ms));
      last_log_ms = now_ms;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  return true;
}

static bool requeue_upload_job(const UploadJob& job, bool prioritize_front, const char* reason) {
  QueueHandle_t target_queue = scan_mode_is_dish(job.mode) ? upload_queue_dish : upload_queue;
  if (job.is_voice) {
    target_queue = upload_queue;
  }
  if (target_queue == NULL) {
    Serial.printf("[UPLOAD_QUEUE] requeue_missing_queue mode=%s voice=%d reason=%s\n",
                  job.mode,
                  job.is_voice ? 1 : 0,
                  reason ? reason : "unknown");
    return false;
  }
  BaseType_t queued = prioritize_front
                          ? xQueueSendToFront(target_queue, &job, pdMS_TO_TICKS(10))
                          : xQueueSend(target_queue, &job, pdMS_TO_TICKS(10));
  Serial.printf("[UPLOAD_QUEUE] %s mode=%s job_id=%lu voice=%d front=%d reason=%s q=%lu\n",
                queued == pdTRUE ? "requeued" : "requeue_failed",
                job.mode,
                (unsigned long)job.job_id,
                job.is_voice ? 1 : 0,
                prioritize_front ? 1 : 0,
                reason ? reason : "unknown",
                (unsigned long)upload_queue_count());
  return queued == pdTRUE;
}

static bool scan_phase_is_allowed_quiet(const char* phase) {
  if (!phase) return false;
  return (strcmp(phase, "CAPTURING") == 0 ||
          strcmp(phase, "WAITING_INPUT") == 0 ||
          strcmp(phase, "DONE") == 0 ||
          strcmp(phase, "ERROR") == 0);
}

static bool scan_ui_should_emit(const char* mode, const char* phase, bool immediate_error) {
  if (!scan_mode_is_quiet(mode)) {
    return true;
  }
  if (phase && strcmp(phase, "ERROR") == 0) {
    return immediate_error;
  }
  return scan_phase_is_allowed_quiet(phase);
}

static bool scan_ui_status_emit(const char* phase,
                                const char* text,
                                const char* mode,
                                uint32_t job_id,
                                bool immediate_error) {
  if (!scan_ui_should_emit(mode, phase, immediate_error)) {
    Serial.printf("[UI_STATUS] suppressed op=SCAN phase=%s mode=%s\n",
                  phase ? phase : "",
                  mode ? mode : "");
    return false;
  }
  if (phase &&
      strcmp(phase, "RESULT_WAITING") == 0 &&
      scan_mode_is_dish(mode) &&
      g_dish_timing.ui_wait_start_ms == 0) {
    g_dish_timing.sense_job_id = job_id;
    g_dish_timing.ui_wait_start_ms = millis();
  }
  uart_send_ui_status_extended("SCAN", phase, text ? text : "", mode, job_id);
  return true;
}

static bool sense_can_sleep_now(const char** reason) {
  if (guardian_force_sleep) {
    return true;
  }
  if (http_inflight) {
    if (background_sleep_bypass_active) {
      return true;
    }
    if (reason) *reason = "http_inflight";
    return false;
  }
  if (upload_inflight) {
    if (background_sleep_bypass_active) {
      return true;
    }
    if (reason) *reason = "upload_inflight";
    return false;
  }
  if (upload_queue_count() > 0) {
    if (background_sleep_bypass_active) {
      return true;
    }
    if (reason) *reason = "upload_queue";
    return false;
  }
  if (waiting_for_mqtt_result) {
    if (background_sleep_bypass_active) {
      return true;
    }
    if (reason) *reason = "result_pending";
    return false;
  }
  return true;
}

static void log_sleep_flags(const char* where) {
  Serial.printf("[SLEEP_FLAGS] where=%s http=%d upload=%d q=%lu result=%d\n",
                where ? where : "",
                http_inflight ? 1 : 0,
                upload_inflight ? 1 : 0,
                (unsigned long)upload_queue_count(),
                waiting_for_mqtt_result ? 1 : 0);
}

static bool sleep_block_should_log(const char* reason, const char* where, const char* op) {
  const char* safe_reason = reason ? reason : "";
  const char* safe_where = where ? where : "";
  const char* safe_op = op ? op : "";
  bool changed = (strcmp(last_sleep_block_reason, safe_reason) != 0) ||
                 (strcmp(last_sleep_block_where, safe_where) != 0) ||
                 (strcmp(last_sleep_block_op, safe_op) != 0);
  unsigned long now = millis();
  if (changed || (now - last_sleep_block_log_ms) >= SLEEP_BLOCK_LOG_INTERVAL_MS) {
    strncpy(last_sleep_block_reason, safe_reason, sizeof(last_sleep_block_reason) - 1);
    last_sleep_block_reason[sizeof(last_sleep_block_reason) - 1] = '\0';
    strncpy(last_sleep_block_where, safe_where, sizeof(last_sleep_block_where) - 1);
    last_sleep_block_where[sizeof(last_sleep_block_where) - 1] = '\0';
    strncpy(last_sleep_block_op, safe_op, sizeof(last_sleep_block_op) - 1);
    last_sleep_block_op[sizeof(last_sleep_block_op) - 1] = '\0';
    last_sleep_block_log_ms = now;
    return true;
  }
  return false;
}

static bool sleep_allowed_now(const char* where, const char** reason_out) {
  const char* reason = NULL;
  if (!sense_can_sleep_now(&reason)) {
    if (sleep_block_should_log(reason ? reason : "net_inflight", where, "net")) {
      Serial.printf("[SLEEP_BLOCK] reason=%s where=%s http=%d upload=%d q=%lu result=%d\n",
                    reason ? reason : "net_inflight",
                    where ? where : "",
                    http_inflight ? 1 : 0,
                    upload_inflight ? 1 : 0,
                    (unsigned long)upload_queue_count(),
                    waiting_for_mqtt_result ? 1 : 0);
    }
    if (reason_out) *reason_out = reason;
    return false;
  }
  return true;
}

static bool sleep_reason_is_background_deferable(const char* reason) {
  if (!reason || !reason[0]) {
    return false;
  }
  return strcmp(reason, "upload_queue") == 0 ||
         strcmp(reason, "result_pending") == 0 ||
         strcmp(reason, "list_refresh_inflight") == 0 ||
         strcmp(reason, "wifi_connect") == 0;
}

static void sleep_background_force_reset() {
  background_sleep_block_start_ms = 0;
  background_sleep_bypass_active = false;
}

static bool sleep_background_force_ready(unsigned long now_ms, const char* reason, const char* where) {
  if (!sleep_reason_is_background_deferable(reason)) {
    sleep_background_force_reset();
    return false;
  }
  if (background_sleep_block_start_ms == 0) {
    background_sleep_block_start_ms = now_ms;
  }
  unsigned long elapsed_ms = now_ms - background_sleep_block_start_ms;
  if (elapsed_ms < PRE_SLEEP_BLOCK_MAX_MS) {
    if (sleep_block_should_log(reason, where ? where : "background_wait", "background")) {
      Serial.printf("[SLEEP] background_wait reason=%s where=%s elapsed_ms=%lu remaining_ms=%lu\n",
                    reason,
                    where ? where : "",
                    elapsed_ms,
                    (unsigned long)(PRE_SLEEP_BLOCK_MAX_MS - elapsed_ms));
    }
    return false;
  }
  if (!background_sleep_bypass_active) {
    if (strcmp(reason, "upload_queue") == 0 && upload_queue_count() != 1) {
      if (sleep_block_should_log("upload_queue_multi", where ? where : "background_wait", "background")) {
        Serial.printf("[SLEEP] background_wait reason=upload_queue_multi where=%s queued=%lu\n",
                      where ? where : "",
                      (unsigned long)upload_queue_count());
      }
      return false;
    }
    Serial.printf("[SLEEP] background_force_defer reason=%s where=%s elapsed_ms=%lu upload=%d q=%lu result=%d http=%d\n",
                  reason,
                  where ? where : "",
                  elapsed_ms,
                  upload_inflight ? 1 : 0,
                  (unsigned long)upload_queue_count(),
                  waiting_for_mqtt_result ? 1 : 0,
                  http_inflight ? 1 : 0);
    sleep_defer_queued_background_uploads();
    if (waiting_for_mqtt_result) {
      waiting_for_mqtt_result = false;
      mqtt_wait_deadline = 0;
      current_scan_job_id = "";
      mqtt_clear_result_subscription();
      clear_active_dish_job(current_result_local_job_id, "sleep_force_defer");
      current_result_local_job_id = 0;
    }
    background_sleep_bypass_active = true;
  }
  return true;
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

static const char* sleep_sm_state_name(SleepSmState state) {
  switch (state) {
    case SLEEP_SM_IDLE:
      return "IDLE";
    case SLEEP_SM_REQ_RX:
      return "REQ_RX";
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

static void cancel_pending_sleep_for_user_action(const char* reason) {
  unsigned long now_ms = millis();
  if (!sleep_requested &&
      !sleep_coord_requested &&
      sleep_sm_state == SLEEP_SM_IDLE &&
      !sleep_coord_pending_for_ready) {
    return;
  }
  sleep_holdoff_until_ms = now_ms + SLEEP_HOLDOFF_MS;
  Serial.printf("[SLEEP] cancel pending sense sleep reason=%s user_state=%s device_state=%s sm=%s\n",
                reason ? reason : "user_input",
                sense_user_state_name(),
                sense_device_state_name(),
                sleep_sm_state_name(sleep_sm_state));
  if (sleep_coord_requested || sleep_sm_state != SLEEP_SM_IDLE ||
      sleep_coord_pending_for_ready) {
    sleep_send_deny_and_clear("user_activity", now_ms);
    return;
  }
  sleep_requested = false;
  sleep_request_ms = 0;
  sleep_request_logged = false;
  sleep_ack_defer_logged = false;
  sleep_ready_sent_for_cycle = false;
  sleep_coord_requested = false;
  sleep_coord_pending_for_ready = false;
  sleep_sm_transition(SLEEP_SM_IDLE, "USER_ACTIVITY", sleep_sm_msg_id);
  sleep_sm_msg_id = 0;
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
  if (wifi_connect_inflight && !upload_inflight) {
    if (reason) *reason = "op_inflight";
    if (op) *op = "wifi";
    return true;
  }
  if (current_job.state != OP_IDLE && current_job.state != OP_DONE) {
    if (current_job.type == OP_SCAN && !scan_ui_inflight) {
      return false;
    }
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

extern "C" bool halo_uart_link_recent(unsigned long max_age_ms) {
  unsigned long now_ms = millis();
  unsigned long rx_age = (last_uart_rx_ms > 0) ? (now_ms - last_uart_rx_ms) : 0xFFFFFFFFUL;
  unsigned long limit_ms = max_age_ms > 0 ? max_age_ms : LINK_RECENT_MS;
  return link_synced && last_uart_rx_ms > 0 && rx_age < limit_ms;
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


static void service_boot_wifi_connect(unsigned long now_ms) {
  if (!boot_wifi_connect_pending) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    boot_wifi_connect_pending = false;
    boot_wifi_connect_last_attempt_ms = 0;
    Serial.printf("[BOOT_WIFI] connect_skip already_connected t=%lu\n", now_ms);
    return;
  }

  if (wifi_connect_inflight) {
    return;
  }

  if (upload_inflight || upload_queue_count() > 0) {
    static unsigned long last_upload_owner_log_ms = 0;
    if (last_upload_owner_log_ms == 0 ||
        (now_ms - last_upload_owner_log_ms) >= 1500UL) {
      Serial.printf("[BOOT_WIFI] defer reason=background_upload_owner t=%lu upload=%d q=%lu\n",
                    now_ms,
                    upload_inflight ? 1 : 0,
                    (unsigned long)upload_queue_count());
      last_upload_owner_log_ms = now_ms;
    }
    boot_wifi_connect_earliest_ms = now_ms + 1500UL;
    return;
  }

  if (now_ms < boot_wifi_connect_earliest_ms) {
    return;
  }

  const char* defer_reason = NULL;
  if (foreground_priority_active(now_ms, &defer_reason)) {
    boot_wifi_connect_earliest_ms = now_ms + 750;
    if (boot_wifi_last_defer_log_ms == 0 ||
        (now_ms - boot_wifi_last_defer_log_ms) >= 1000UL) {
      Serial.printf("[BOOT_WIFI] defer reason=%s t=%lu next_t=%lu\n",
                    defer_reason ? defer_reason : "foreground",
                    now_ms,
                    boot_wifi_connect_earliest_ms);
      boot_wifi_last_defer_log_ms = now_ms;
    }
    return;
  }

  unsigned long idle_since_lcd_ms = now_ms - last_lcd_communication;
  if (idle_since_lcd_ms < 2000) {
    return;
  }

  if (boot_wifi_connect_last_attempt_ms > 0 &&
      (now_ms - boot_wifi_connect_last_attempt_ms) < WIFI_BEGIN_COOLDOWN_MS) {
    return;
  }

  boot_wifi_connect_last_attempt_ms = now_ms;
  Serial.printf("[BOOT_WIFI] connect_begin t=%lu idle_since_lcd_ms=%lu queue=%lu\n",
                now_ms,
                idle_since_lcd_ms,
                (unsigned long)(op_queue ? uxQueueMessagesWaiting(op_queue) : 0));
  (void)ensure_wifi_connected("wifi_connect", 0);
  if (wifi_connect_inflight) {
    boot_wifi_connect_earliest_ms = now_ms + WIFI_BEGIN_COOLDOWN_MS;
    Serial.printf("[BOOT_WIFI] connect_started t=%lu next_retry_t=%lu\n",
                  now_ms,
                  boot_wifi_connect_earliest_ms);
  } else {
    boot_wifi_connect_earliest_ms = now_ms + WIFI_BEGIN_COOLDOWN_MS;
    Serial.printf("[BOOT_WIFI] connect_deferred_no_begin t=%lu next_retry_t=%lu status=%d state=%d\n",
                  now_ms,
                  boot_wifi_connect_earliest_ms,
                  (int)WiFi.status(),
                  (int)wifi_state);
  }
}

// (voice functions removed — see sense_voice.h)
// (shopping list API functions removed — see sense_list.h)

// ── Camera Functions → sense_camera.h ────────────────────────────────

// (camera functions removed — see sense_camera.h)



// ── Presign Functions ──────────────────────────────────────────────
static bool do_presign_request_simple(const char* url,
                                      const char* type,
                                      PresignReply& out,
                                      int& http_code,
                                      String& resp_body,
                                      uint32_t deadline_ms) {
  diag_note_stage("presign", 0);
  StaticJsonDocument<256> doc;
  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));
  if (owner_id[0] == '\0') {
    presign_set_error_text("Owner not set");
    Serial.println("[PRESIGN] owner_id empty -> abort");
    diag_record_error("presign", -1, "owner_missing");
    return false;
  }
  doc["device_id"] = device_id;
  doc["user_id"] = owner_id;
  doc["type"] = type ? type : "";
  String body;
  serializeJson(doc, body);

  Serial.printf("[PRESIGN] Using job_type: %s\n", type ? type : "");
  Serial.println("[PRESIGN] POST " + String(url ? url : ""));
  if (!http_post_json_with_retries(url,
                                   body,
                                   http_code,
                                   resp_body,
                                   "PRESIGN",
                                   API_KEY,
                                   BEARER_TOKEN,
                                   current_job.job_id,
                                   deadline_ms)) {
    Serial.printf("[PRESIGN] Request failed with code %d\n", http_code);
    diag_record_error("presign_http", http_code, "request_failed");
    return false; 
  }
  Serial.printf("[PRESIGN] HTTP %d\n", http_code);
  if (resp_body.length()) Serial.println("[PRESIGN] Body: " + resp_body);

  StaticJsonDocument<768> r;
  auto err = deserializeJson(r, resp_body);
  if (err) {
    Serial.print("[PRESIGN] JSON parse error: ");
    Serial.println(err.c_str());
    diag_record_error("presign_parse", -1, "json_parse");
    return false;
  }

  out.job_id       = r["job_id"].as<String>();
  out.put_url      = r["put_url"].as<String>();
  out.s3_key       = r["s3_key"].as<String>();
  out.content_type = r["content_type"].as<String>();
  out.result_url   = r["result_url"].as<String>();
  if (out.content_type.isEmpty() || out.content_type == "null") {
    out.content_type = "image/jpeg";
  }
  if (out.result_url == "null") {
    out.result_url = "";
  }
  out.ttl_s        = r["ttl_s"] | 0;

  if (out.job_id.isEmpty() || out.put_url.isEmpty()) {
    diag_record_error("presign_parse", -1, "missing_fields");
    return false;
  }
  return true;
}

static bool do_presign_request(const char* base_url,
                               const char* endpoint,
                               const char* type,
                               const char* action,
                               const char* owner,
                               const char* expiry_date,
                               bool add_to_shopping_list,
                               const UploadJob::CameraUploadMeta* camera_meta,
                               PresignReply& out,
                               int& http_code,
                               String& resp_body,
                               uint32_t deadline_ms) {
  diag_note_stage("presign", 0);
  String presign_url = String(base_url) + String(endpoint ? endpoint : "");
  StaticJsonDocument<512> doc;
  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));
  if (owner_id[0] == '\0') {
    presign_set_error_text("Owner not set");
    Serial.println("[PRESIGN] owner_id empty -> abort");
    diag_record_error("presign", -1, "owner_missing");
    return false;
  }
  doc["user_id"] = owner_id;
  doc["device_id"] = device_id;
  doc["owner"] = owner ? owner : "";
  doc["action"] = action ? action : "";
  doc["type"] = type ? type : "";
  doc["content_type"] = "image/jpeg";
  if (expiry_date && expiry_date[0]) {
    doc["product_expiration"] = expiry_date;
  }
  if (type && strcmp(type, "discard") == 0) {
    doc["add_to_shopping_list"] = add_to_shopping_list;
  }
  if (camera_meta) {
    append_camera_meta_json(doc, *camera_meta);
  }
  String body; 
  serializeJson(doc, body);
  
  Serial.printf("[PRESIGN] Using type=%s action=%s\n",
                type ? type : "",
                action ? action : "");
  log_camera_meta_for_presign("PRESIGN", camera_meta);
  Serial.println("[PRESIGN] POST " + presign_url);
  if (!http_post_json_with_retries(presign_url.c_str(),
                                   body,
                                   http_code,
                                   resp_body,
                                   "PRESIGN",
                                   API_KEY,
                                   BEARER_TOKEN,
                                   current_job.job_id,
                                   deadline_ms)) {
    Serial.printf("[PRESIGN] Request failed with code %d\n", http_code);
    diag_record_error("presign_http", http_code, "request_failed");
    return false;
  }
  Serial.printf("[PRESIGN] HTTP %d\n", http_code);
  if (resp_body.length()) Serial.println("[PRESIGN] Body: " + resp_body);

  StaticJsonDocument<768> r;
  auto err = deserializeJson(r, resp_body);
  if (err) { 
    Serial.print("[PRESIGN] JSON parse error: "); 
    Serial.println(err.c_str()); 
    diag_record_error("presign_parse", -1, "json_parse");
    return false; 
  }

  out.job_id       = r["job_id"].as<String>();
  out.put_url      = r["put_url"].as<String>();
  out.s3_key       = r["s3_key"].as<String>();
  out.content_type = r["content_type"].as<String>();
  out.result_url   = r["result_url"].as<String>();
  if (out.content_type.isEmpty() || out.content_type == "null") {
    out.content_type = "image/jpeg";
  }
  if (out.result_url == "null") {
    out.result_url = "";
  }
  out.ttl_s        = r["ttl_s"] | 0;

  if (out.job_id.isEmpty() || out.put_url.isEmpty()) {
    diag_record_error("presign_parse", -1, "missing_fields");
    return false;
  }
  return true;
}

static bool get_presign(PresignReply& out,
                        const char* mode,
                        const char* expiry_date,
                        bool add_to_shopping_list,
                        const UploadJob::CameraUploadMeta* camera_meta,
                        uint32_t deadline_ms) {
  const bool is_discard = scan_mode_is_discard(mode);
  int code = 0; 
  String body;
  char owner_id[64] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  if (is_discard) {
    if (do_presign_request(CHECKIN_API_BASE_URL,
                           CHECKIN_PRESIGN_ENDPOINT,
                           "discard",
                           "IN",
                           owner_id,
                           expiry_date,
                           add_to_shopping_list,
                           camera_meta,
                           out,
                           code,
                           body,
                           deadline_ms)) {
      Serial.println("[PRESIGN] Presign OK");
      return true;
    }
    Serial.printf("[PRESIGN] Presign failed (primary): %d\n", code);
    if (do_presign_request(CHECKIN_API_BASE_URL,
                           DISCARD_PRESIGN_ENDPOINT,
                           "discard",
                           "IN",
                           owner_id,
                           expiry_date,
                           add_to_shopping_list,
                           camera_meta,
                           out,
                           code,
                           body,
                           deadline_ms)) {
      Serial.println("[PRESIGN] Presign OK (fallback)");
      return true;
    }
    Serial.printf("[PRESIGN] Presign failed (fallback): %d\n", code);
    diag_record_error("presign_http", code, "discard_failed");
    return false;
  }
  if (do_presign_request(DISH_PRESIGN_URL,
                         "",
                         "dish",
                         "IN",
                         owner_id,
                         NULL,
                         false,
                         camera_meta,
                         out,
                         code,
                         body,
                         deadline_ms)) {
    Serial.println("[PRESIGN] Presign OK");
    return true;
  }
  Serial.printf("[PRESIGN] Presign failed: %d\n", code);
  diag_record_error("presign_http", code, "dish_failed");
  return false;
}

// net_ready_for_tls(char*, size_t) → sense_upload.h

static void scan_terminal_reset() {
  scan_terminal_sent = false;
}

static void scan_ui_inflight_set(bool active, const char* reason) {
  if (scan_ui_inflight == active) {
    return;
  }
  scan_ui_inflight = active;
  Serial.printf("[OP_INFLIGHT] %s op=scan reason=%s\n",
                active ? "set" : "clear",
                reason ? reason : "unknown");
}

static void dish_scan_inflight_set(bool active, const char* reason) {
  if (dish_scan_inflight == active) {
    return;
  }
  dish_scan_inflight = active;
  Serial.printf("[OP_INFLIGHT] %s op=dish_scan reason=%s\n",
                active ? "set" : "clear",
                reason ? reason : "unknown");
}

static void flow_plan_print(uint32_t job_id, const char* mode) {
  Serial.printf("[FLOW_PLAN] job_id=%lu mode=%s\n", (unsigned long)job_id, mode ? mode : "");
  if (mode && (strcmp(mode, "check-in") == 0 || strcmp(mode, "check-out") == 0 || strcmp(mode, "check_out") == 0)) {
    Serial.println("1) LCD: show HOLD_STILL (phase=CAPTURING)");
    Serial.println("2) Sense: init camera + capture");
    Serial.println("3) LCD: show EXPIRY (phase=WAITING_INPUT)");
    Serial.println("4) LCD->Sense: INPUT_EXPIRY_DATE");
    Serial.println("5) LCD: show LOGGED for 2s (phase=DONE)");
    Serial.println("6) Sense: background presign + upload (no UI_STATUS)");
  } else if (scan_mode_is_dish(mode)) {
    Serial.println("1) LCD: show HOLD_STILL (phase=CAPTURING)");
    Serial.println("2) Sense: capture + enqueue upload");
    Serial.println("3) LCD: show LOGGED for 2s (phase=DONE)");
    Serial.println("4) Sense: background presign + upload (no nutrition wait)");
  } else {
    Serial.println("1) LCD: show HOLD_STILL (phase=CAPTURING)");
    Serial.println("2) Sense: capture + enqueue upload");
    Serial.println("3) LCD: show PROCESSING (phase=RESULT_WAITING)");
    Serial.println("4) Sense: upload + wait for result");
    Serial.println("5) LCD: show nutrition (UI_MEAL_RESULT)");
  }
}

static void flow_step(uint32_t job_id, const char* step_name) {
  Serial.printf("[FLOW_STEP] job_id=%lu %s\n",
                (unsigned long)job_id,
                step_name ? step_name : "");
}

static void scan_send_terminal_status(const char* phase, const char* text, const char* mode, bool immediate_error) {
  if (scan_terminal_sent) {
    return;
  }
  if (scan_ui_status_emit(phase, text, mode, current_job.job_id, immediate_error)) {
    scan_terminal_sent = true;
  }
}

static uint32_t upload_queue_count() {
  uint32_t count = 0;
  if (upload_queue) {
    count += (uint32_t)uxQueueMessagesWaiting(upload_queue);
  }
  if (upload_queue_dish) {
    count += (uint32_t)uxQueueMessagesWaiting(upload_queue_dish);
  }
  return count;
}

static bool dish_upload_pending() {
  if (active_dish_job_id != 0) {
    return true;
  }
  if (dish_scan_inflight) {
    return true;
  }
  if (upload_queue_dish && uxQueueMessagesWaiting(upload_queue_dish) > 0) {
    return true;
  }
  if (current_job.active && current_job.type == OP_SCAN && scan_mode_is_dish(current_job.mode)) {
    return true;
  }
  if (waiting_for_mqtt_result && current_result_local_job_id != 0) {
    return true;
  }
  return false;
}

static bool foreground_scan_pending() {
  if (scan_ui_inflight || dish_scan_inflight) {
    return true;
  }
  if (current_job.active && current_job.type == OP_SCAN &&
      current_job.state != OP_IDLE && current_job.state != OP_DONE) {
    return true;
  }
  return false;
}

static const char* sense_user_state_name() {
  if (foreground_scan_pending()) {
    return "CAPTURE_COMMITTED";
  }
  if (waiting_for_mqtt_result) {
    return "USER_WAITING_RESULT";
  }
  return "MENU_READY";
}

static const char* sense_device_state_name() {
  if (sleep_requested || sleep_coord_requested || sleep_sm_state != SLEEP_SM_IDLE) {
    return "PRE_SLEEP";
  }
  if (waiting_for_mqtt_result) {
    return "BACKGROUND_RESULT_WAIT";
  }
  if (upload_inflight || upload_queue_count() > 0) {
    return "BACKGROUND_UPLOAD";
  }
  if (wifi_connect_inflight) {
    return "WIFI_RECOVERY";
  }
  return "AWAKE_IDLE";
}

static bool scan_request_pending_for_mode(const char* requested_mode) {
  if (foreground_scan_pending()) {
    return true;
  }
  if (requested_mode && strcmp(requested_mode, "dish") == 0 && dish_upload_pending()) {
    return true;
  }
  if (upload_queue_is_full()) {
    return true;
  }
  return false;
}

static void set_result_context(uint32_t job_id, const char* mode) {
  current_result_local_job_id = job_id;
  if (mode) {
    strncpy(current_result_mode, mode, sizeof(current_result_mode) - 1);
    current_result_mode[sizeof(current_result_mode) - 1] = '\0';
  } else {
    current_result_mode[0] = '\0';
  }
}

static void clear_result_context() {
  current_result_local_job_id = 0;
  current_result_mode[0] = '\0';
}

static void clear_active_dish_job(uint32_t job_id, const char* reason) {
  if (active_dish_job_id != 0 && (job_id == 0 || active_dish_job_id == job_id)) {
    Serial.printf("[DISH] clear active job_id=%lu reason=%s\n",
                  (unsigned long)active_dish_job_id,
                  reason ? reason : "unknown");
    active_dish_job_id = 0;
  }
  if (current_result_local_job_id != 0 && (job_id == 0 || current_result_local_job_id == job_id)) {
    clear_result_context();
  }
}

static bool upload_queue_is_full() {
  if (!upload_queue || !upload_queue_dish) {
    return true;
  }
  return upload_queue_count() >= UPLOAD_QUEUE_MAX;
}

#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
struct PersistedUploadMetaV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t job_id;
  uint32_t image_len;
  uint32_t created_epoch;
  uint16_t quantity;
  uint8_t retries;
  uint8_t reserved;
  char mode[16];
  char expiry_date[16];
};

struct PersistedUploadMeta {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t job_id;
  uint32_t image_len;
  uint32_t created_epoch;
  uint16_t quantity;
  uint8_t retries;
  uint8_t reserved;
  char mode[16];
  char expiry_date[16];
  UploadJob::CameraUploadMeta camera_meta;
};

static const uint32_t UPLOAD_PERSIST_MAGIC = 0x48555031UL;  // HUP1
static const uint16_t UPLOAD_PERSIST_VERSION = 2;
static const char* UPLOAD_PERSIST_META_PATH = "/upload_retry.meta";
static const char* UPLOAD_PERSIST_IMAGE_PATH = "/upload_retry.bin";
static const uint8_t UPLOAD_PERSIST_MAX_RETRIES = 5;
static const uint32_t UPLOAD_PERSIST_MAX_AGE_S = 86400UL;
static const unsigned long UPLOAD_PERSIST_CHECK_INTERVAL_MS = 2000;
static const unsigned long UPLOAD_PERSIST_VOICE_REPLAY_BACKOFF_MS = 60000UL;
static const size_t UPLOAD_PERSIST_FREE_RESERVE_BYTES = 64 * 1024;
static const size_t UPLOAD_PERSIST_MAX_IMAGE_BYTES = 1024 * 1024;

static bool g_upload_persist_ready = false;
static bool g_upload_persist_attempted_this_boot = false;
static unsigned long g_upload_persist_last_check_ms = 0;
static unsigned long g_upload_persist_replay_not_before_ms = 0;
static uint8_t g_upload_persist_cached_count = 0;
static char g_upload_persist_last_result[24] = "none";
static char g_upload_persist_last_reason[32] = "";
static unsigned long g_upload_persist_last_event_ms = 0;
static uint8_t g_upload_persist_last_retries = 0;

static const uint8_t UPLOAD_PERSIST_FLAG_ADD_TO_SHOPPING = 1u << 0;
static const uint8_t UPLOAD_PERSIST_FLAG_IS_VOICE = 1u << 1;

static void upload_persist_note_event(const char* result,
                                      const char* reason,
                                      uint8_t cached_count,
                                      uint8_t retries) {
  diag_sanitize_token(g_upload_persist_last_result, sizeof(g_upload_persist_last_result), result);
  diag_sanitize_token(g_upload_persist_last_reason, sizeof(g_upload_persist_last_reason), reason);
  g_upload_persist_cached_count = cached_count;
  g_upload_persist_last_retries = retries;
  g_upload_persist_last_event_ms = millis();
  diag_record_action_event("upload_cache",
                           "",
                           g_upload_persist_last_result,
                           g_upload_persist_last_reason,
                           cached_count);
}

uint32_t sense_get_upload_cache_count() {
  return g_upload_persist_cached_count;
}

const char* sense_get_upload_cache_last_result() {
  return g_upload_persist_last_result;
}

const char* sense_get_upload_cache_last_reason() {
  return g_upload_persist_last_reason;
}

int32_t sense_get_upload_cache_last_age_ms() {
  if (g_upload_persist_last_event_ms == 0) {
    return -1;
  }
  return static_cast<int32_t>(millis() - g_upload_persist_last_event_ms);
}

uint32_t sense_get_upload_cache_last_retries() {
  return g_upload_persist_last_retries;
}

static size_t upload_persist_file_size(const char* path) {
  if (!g_upload_persist_ready || !path || !SPIFFS.exists(path)) {
    return 0;
  }
  File file = SPIFFS.open(path, "r");
  if (!file) {
    return 0;
  }
  size_t size = file.size();
  file.close();
  return size;
}

static bool upload_persist_has_pending() {
  bool pending = g_upload_persist_ready &&
         SPIFFS.exists(UPLOAD_PERSIST_META_PATH) &&
         SPIFFS.exists(UPLOAD_PERSIST_IMAGE_PATH);
  g_upload_persist_cached_count = pending ? 1 : 0;
  return pending;
}

static bool upload_persist_delete() {
  if (!g_upload_persist_ready) {
    return false;
  }
  bool removed = false;
  if (SPIFFS.exists(UPLOAD_PERSIST_META_PATH)) {
    removed = SPIFFS.remove(UPLOAD_PERSIST_META_PATH) || removed;
  }
  if (SPIFFS.exists(UPLOAD_PERSIST_IMAGE_PATH)) {
    removed = SPIFFS.remove(UPLOAD_PERSIST_IMAGE_PATH) || removed;
  }
  g_upload_persist_cached_count = upload_persist_has_pending() ? 1 : 0;
  return removed;
}

static bool upload_persist_read_meta(PersistedUploadMeta* meta) {
  if (!meta || !upload_persist_has_pending()) {
    return false;
  }
  memset(meta, 0, sizeof(*meta));
  File file = SPIFFS.open(UPLOAD_PERSIST_META_PATH, "r");
  if (!file) {
    return false;
  }
  size_t file_size = file.size();
  bool ok = false;
  if (file_size == sizeof(PersistedUploadMeta)) {
    ok = (file.read((uint8_t*)meta, sizeof(*meta)) == (int)sizeof(*meta));
  } else if (file_size == sizeof(PersistedUploadMetaV1)) {
    PersistedUploadMetaV1 legacy = {};
    ok = (file.read((uint8_t*)&legacy, sizeof(legacy)) == (int)sizeof(legacy));
    if (ok) {
      meta->magic = legacy.magic;
      meta->version = legacy.version;
      meta->header_size = legacy.header_size;
      meta->job_id = legacy.job_id;
      meta->image_len = legacy.image_len;
      meta->created_epoch = legacy.created_epoch;
      meta->quantity = legacy.quantity;
      meta->retries = legacy.retries;
      meta->reserved = legacy.reserved;
      strncpy(meta->mode, legacy.mode, sizeof(meta->mode) - 1);
      strncpy(meta->expiry_date, legacy.expiry_date, sizeof(meta->expiry_date) - 1);
    }
  }
  file.close();
  if (!ok) {
    return false;
  }
  bool valid_v2 = (meta->version == UPLOAD_PERSIST_VERSION &&
                   meta->header_size == sizeof(*meta));
  bool valid_v1 = (meta->version == 1 &&
                   meta->header_size == sizeof(PersistedUploadMetaV1));
  if (meta->magic != UPLOAD_PERSIST_MAGIC ||
      (!valid_v2 && !valid_v1) ||
      meta->image_len == 0 ||
      meta->image_len > UPLOAD_PERSIST_MAX_IMAGE_BYTES) {
    return false;
  }
  meta->mode[sizeof(meta->mode) - 1] = '\0';
  meta->expiry_date[sizeof(meta->expiry_date) - 1] = '\0';
  return true;
}

static bool upload_persist_is_stale(const PersistedUploadMeta& meta) {
  if (meta.created_epoch < TIME_VALID_MIN_EPOCH) {
    return false;
  }
  time_t now = time(nullptr);
  if (now < (time_t)TIME_VALID_MIN_EPOCH) {
    return false;
  }
  return (uint32_t)(now - (time_t)meta.created_epoch) > UPLOAD_PERSIST_MAX_AGE_S;
}

static bool upload_persist_write_blob(const char* path, const uint8_t* data, size_t len) {
  File file = SPIFFS.open(path, "w");
  if (!file) {
    return false;
  }
  size_t written = file.write(data, len);
  file.close();
  return written == len;
}

static bool upload_persist_save(const UploadJob& job, uint8_t next_retries) {
  if (!g_upload_persist_ready || !job.image_buf || job.image_len == 0) {
    return false;
  }

  PersistedUploadMeta meta = {};
  meta.magic = UPLOAD_PERSIST_MAGIC;
  meta.version = UPLOAD_PERSIST_VERSION;
  meta.header_size = sizeof(meta);
  meta.job_id = job.job_id;
  meta.image_len = (uint32_t)job.image_len;
  meta.created_epoch = job.created_epoch;
  if (meta.created_epoch < TIME_VALID_MIN_EPOCH) {
    time_t now = time(nullptr);
    if (now >= (time_t)TIME_VALID_MIN_EPOCH) {
      meta.created_epoch = (uint32_t)now;
    } else {
      meta.created_epoch = 0;
    }
  }
  meta.quantity = (job.quantity < 1) ? 1 : job.quantity;
  meta.retries = next_retries;
  meta.reserved = 0;
  if (job.add_to_shopping_list) {
    meta.reserved |= UPLOAD_PERSIST_FLAG_ADD_TO_SHOPPING;
  }
  if (job.is_voice) {
    meta.reserved |= UPLOAD_PERSIST_FLAG_IS_VOICE;
  }
  strncpy(meta.mode, job.mode, sizeof(meta.mode) - 1);
  strncpy(meta.expiry_date, job.expiry_date, sizeof(meta.expiry_date) - 1);
  meta.camera_meta = job.camera_meta;

  size_t existing_bytes = upload_persist_file_size(UPLOAD_PERSIST_META_PATH) +
                          upload_persist_file_size(UPLOAD_PERSIST_IMAGE_PATH);
  size_t available_bytes = 0;
  if (SPIFFS.totalBytes() > SPIFFS.usedBytes()) {
    available_bytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
  }
  available_bytes += existing_bytes;
  size_t required_bytes = sizeof(meta) + job.image_len + UPLOAD_PERSIST_FREE_RESERVE_BYTES;
  if (job.image_len > UPLOAD_PERSIST_MAX_IMAGE_BYTES || available_bytes < required_bytes) {
    Serial.printf("[UPLOAD_PERSIST] skip_save len=%u avail=%u required=%u\n",
                  (unsigned)job.image_len,
                  (unsigned)available_bytes,
                  (unsigned)required_bytes);
    return false;
  }

  if (!upload_persist_write_blob(UPLOAD_PERSIST_IMAGE_PATH, job.image_buf, job.image_len)) {
    Serial.println("[UPLOAD_PERSIST] image_write_failed");
    upload_persist_delete();
    return false;
  }
  if (!upload_persist_write_blob(UPLOAD_PERSIST_META_PATH, (const uint8_t*)&meta, sizeof(meta))) {
    Serial.println("[UPLOAD_PERSIST] meta_write_failed");
    upload_persist_delete();
    return false;
  }

  Serial.printf("[UPLOAD_PERSIST] saved job_id=%lu len=%u retries=%u mode=%s\n",
                (unsigned long)meta.job_id,
                (unsigned)meta.image_len,
                (unsigned)meta.retries,
                meta.mode);
  return true;
}

static bool upload_persist_load(UploadJob* job) {
  if (!job) {
    return false;
  }
  PersistedUploadMeta meta = {};
  if (!upload_persist_read_meta(&meta)) {
    Serial.println("[UPLOAD_PERSIST] invalid_meta clearing");
    upload_persist_delete();
    return false;
  }
  if (meta.retries >= UPLOAD_PERSIST_MAX_RETRIES) {
    Serial.printf("[UPLOAD_PERSIST] drop_max_retries retries=%u\n", (unsigned)meta.retries);
    upload_persist_delete();
    return false;
  }
  if (upload_persist_is_stale(meta)) {
    Serial.printf("[UPLOAD_PERSIST] drop_stale age_limit_s=%lu\n",
                  (unsigned long)UPLOAD_PERSIST_MAX_AGE_S);
    upload_persist_delete();
    return false;
  }

  File file = SPIFFS.open(UPLOAD_PERSIST_IMAGE_PATH, "r");
  if (!file) {
    Serial.println("[UPLOAD_PERSIST] image_open_failed");
    upload_persist_delete();
    return false;
  }
  if (file.size() != meta.image_len) {
    Serial.printf("[UPLOAD_PERSIST] image_size_mismatch file=%u meta=%u\n",
                  (unsigned)file.size(),
                  (unsigned)meta.image_len);
    file.close();
    upload_persist_delete();
    return false;
  }

  bool used_psram = false;
  uint8_t* image_buf = allocate_upload_buffer(meta.image_len, &used_psram);
  if (!image_buf) {
    file.close();
    Serial.printf("[UPLOAD_PERSIST] alloc_failed len=%u\n", (unsigned)meta.image_len);
    return false;
  }
  bool ok = (file.read(image_buf, meta.image_len) == (int)meta.image_len);
  file.close();
  if (!ok) {
    free(image_buf);
    Serial.println("[UPLOAD_PERSIST] image_read_failed");
    upload_persist_delete();
    return false;
  }

  UploadJob loaded = {};
  loaded.job_id = meta.job_id;
  loaded.quantity = (meta.quantity < 1) ? 1 : meta.quantity;
  loaded.add_to_shopping_list = (meta.reserved & UPLOAD_PERSIST_FLAG_ADD_TO_SHOPPING) != 0;
  loaded.is_voice = (meta.reserved & UPLOAD_PERSIST_FLAG_IS_VOICE) != 0 ||
                    strcmp(meta.mode, "voice") == 0;
  loaded.image_buf = image_buf;
  loaded.image_len = meta.image_len;
  loaded.retries = meta.retries;
  loaded.created_ms = millis();
  loaded.created_epoch = meta.created_epoch;
  loaded.from_persisted = true;
  loaded.camera_meta = meta.camera_meta;
  strncpy(loaded.mode, meta.mode, sizeof(loaded.mode) - 1);
  strncpy(loaded.expiry_date, meta.expiry_date, sizeof(loaded.expiry_date) - 1);
  *job = loaded;

  Serial.printf("[UPLOAD_PERSIST] loaded job_id=%lu len=%u retries=%u psram=%d mode=%s\n",
                (unsigned long)loaded.job_id,
                (unsigned)loaded.image_len,
                (unsigned)loaded.retries,
                used_psram ? 1 : 0,
                loaded.mode);
  return true;
}

static void upload_persist_setup() {
  if (g_upload_persist_ready) {
    return;
  }
  if (!SPIFFS.begin(false)) {
    Serial.println("[UPLOAD_PERSIST] mount_failed trying_format");
    if (!SPIFFS.begin(true)) {
      Serial.println("[UPLOAD_PERSIST] mount_failed disabled");
      return;
    }
    Serial.println("[UPLOAD_PERSIST] mounted_after_format");
  }
  g_upload_persist_ready = true;
  g_upload_persist_cached_count = upload_persist_has_pending() ? 1 : 0;
  Serial.printf("[UPLOAD_PERSIST] mounted total=%u used=%u pending=%d\n",
                (unsigned)SPIFFS.totalBytes(),
                (unsigned)SPIFFS.usedBytes(),
                g_upload_persist_cached_count ? 1 : 0);
}

static void upload_persist_maybe_replay() {
  if (!g_upload_persist_ready || g_upload_persist_attempted_this_boot) {
    return;
  }
  if (!upload_persist_has_pending()) {
    return;
  }
  if (!wifi_is_connected() || upload_inflight || upload_queue_count() > 0 ||
      waiting_for_mqtt_result || dish_scan_inflight || scan_ui_inflight) {
    return;
  }
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active()) {
    return;
  }
#endif
  unsigned long now_ms = millis();
  if (g_upload_persist_replay_not_before_ms > 0 &&
      now_ms < g_upload_persist_replay_not_before_ms) {
    static unsigned long last_defer_log_ms = 0;
    if (last_defer_log_ms == 0 || (now_ms - last_defer_log_ms) >= 5000UL) {
      Serial.printf("[UPLOAD_PERSIST] replay_deferred remaining_ms=%lu reset_reason=%s\n",
                    (unsigned long)(g_upload_persist_replay_not_before_ms - now_ms),
                    reset_reason_label(g_boot_reset_reason));
      last_defer_log_ms = now_ms;
    }
    return;
  }
  if ((now_ms - g_upload_persist_last_check_ms) < UPLOAD_PERSIST_CHECK_INTERVAL_MS) {
    return;
  }
  g_upload_persist_last_check_ms = now_ms;

  UploadJob job = {};
  if (!upload_persist_load(&job)) {
    g_upload_persist_attempted_this_boot = true;
    return;
  }
  bool queued = job.is_voice
                  ? queue_voice_upload_job(job.job_id,
                                           job.image_buf,
                                           job.image_len,
                                           job.retries,
                                           true,
                                           job.created_epoch)
                  : queue_upload_job(job.job_id,
                                     job.mode,
                                     job.expiry_date,
                                     job.quantity,
                                     job.add_to_shopping_list,
                                     &job.camera_meta,
                                     job.image_buf,
                                     job.image_len,
                                     job.retries,
                                     true,
                                     job.created_epoch);
  if (!queued) {
    Serial.println("[UPLOAD_PERSIST] replay_queue_failed");
    free(job.image_buf);
    return;
  }

  g_upload_persist_attempted_this_boot = true;
  upload_persist_note_event("retry_queued", job.mode, 1, job.retries);
  dump_system_truth("upload_retry_queued");
  Serial.printf("[UPLOAD_PERSIST] replay_queued job_id=%lu retries=%u\n",
                (unsigned long)job.job_id,
                (unsigned)job.retries);
}
#endif

static void upload_persist_handle_failure(const UploadJob& job, const char* reason) {
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
  uint8_t next_retries = (job.retries < 0xFF) ? (uint8_t)(job.retries + 1) : 0xFF;
  bool saved = upload_persist_save(job, next_retries);
  if (saved && job.is_voice) {
    g_upload_persist_replay_not_before_ms = millis() + UPLOAD_PERSIST_VOICE_REPLAY_BACKOFF_MS;
  }
  uint8_t cached_count = upload_persist_has_pending() ? 1 : 0;
  g_upload_persist_attempted_this_boot = true;
  upload_persist_note_event(saved ? "cached" : "cache_fail",
                            reason ? reason : "unknown",
                            cached_count,
                            next_retries);
  dump_system_truth(saved ? "upload_cached" : "upload_cache_fail");
  Serial.printf("[UPLOAD_PERSIST] failure reason=%s job_id=%lu saved=%d next_retries=%u from_persisted=%d\n",
                reason ? reason : "unknown",
                (unsigned long)job.job_id,
                saved ? 1 : 0,
                (unsigned)next_retries,
                job.from_persisted ? 1 : 0);
#else
  (void)job;
  (void)reason;
#endif
}

static void sleep_defer_queued_background_uploads() {
  bool saved_one = false;
  auto drain_queue = [&](QueueHandle_t queue, const char* label) {
    if (!queue) {
      return;
    }
    UploadJob queued = {};
    while (xQueueReceive(queue, &queued, 0) == pdTRUE) {
      bool saved = false;
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
      if (!saved_one && queued.image_buf && queued.image_len > 0) {
        uint8_t next_retries = (queued.retries < 0xFF) ? (uint8_t)(queued.retries + 1) : 0xFF;
        saved = upload_persist_save(queued, next_retries);
        if (saved) {
          saved_one = true;
          uint8_t cached_count = upload_persist_has_pending() ? 1 : 0;
          g_upload_persist_attempted_this_boot = true;
          upload_persist_note_event("sleep_deferred", queued.mode, cached_count, next_retries);
          Serial.printf("[SLEEP] deferred_upload_saved label=%s job_id=%lu mode=%s retries=%u\n",
                        label ? label : "upload",
                        (unsigned long)queued.job_id,
                        queued.mode,
                        (unsigned)next_retries);
        }
      }
#endif
      if (!saved) {
        Serial.printf("[SLEEP] deferred_upload_dropped label=%s job_id=%lu mode=%s voice=%d\n",
                      label ? label : "upload",
                      (unsigned long)queued.job_id,
                      queued.mode,
                      queued.is_voice ? 1 : 0);
      }
      if (scan_mode_is_dish(queued.mode)) {
        clear_active_dish_job(queued.job_id, saved ? "sleep_deferred" : "sleep_dropped");
      }
      if (queued.image_buf) {
        free(queued.image_buf);
        queued.image_buf = NULL;
      }
    }
  };

  drain_queue(upload_queue_dish, "dish");
  drain_queue(upload_queue, "normal");
}

static uint8_t* allocate_upload_buffer(size_t len, bool* used_psram) {
  if (used_psram) {
    *used_psram = false;
  }
#if (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM)
  uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf) {
    if (used_psram) {
      *used_psram = true;
    }
    return buf;
  }
#endif
  return (uint8_t*)malloc(len);
}

static bool queue_upload_job(uint32_t job_id,
                             const char* mode,
                             const char* expiry,
                             uint16_t quantity,
                             bool add_to_shopping_list,
                             const UploadJob::CameraUploadMeta* camera_meta,
                             uint8_t* image_buf,
                             size_t image_len,
                             uint8_t retries,
                             bool from_persisted,
                             uint32_t created_epoch) {
  if (!image_buf || image_len == 0) {
    return false; 
  }
  const bool is_dish = scan_mode_is_dish(mode);
  QueueHandle_t target_queue = is_dish ? upload_queue_dish : upload_queue;
  if (!target_queue || upload_queue_is_full()) {
    return false;
  }
  UploadJob job = {};
  job.job_id = job_id;
  if (mode) {
    strncpy(job.mode, mode, sizeof(job.mode) - 1);
    job.mode[sizeof(job.mode) - 1] = '\0';
  }
  if (expiry) {
    strncpy(job.expiry_date, expiry, sizeof(job.expiry_date) - 1);
    job.expiry_date[sizeof(job.expiry_date) - 1] = '\0';
  }
  job.quantity = (quantity < 1) ? 1 : quantity;
  job.add_to_shopping_list = add_to_shopping_list;
  job.image_buf = image_buf;
  job.image_len = image_len;
  job.retries = retries;
  job.created_ms = millis();
  job.created_epoch = created_epoch;
  job.from_persisted = from_persisted;
  if (camera_meta) {
    job.camera_meta = *camera_meta;
  } else {
    memset(&job.camera_meta, 0, sizeof(job.camera_meta));
  }
  BaseType_t ok = is_dish
                    ? xQueueSendToFront(target_queue, &job, pdMS_TO_TICKS(10))
                    : xQueueSend(target_queue, &job, pdMS_TO_TICKS(10));
  if (ok != pdTRUE) {
    return false;
  }
  if (is_dish) {
    Serial.printf("[UPLOAD_QUEUE] prioritized dish job_id=%lu\n", (unsigned long)job_id);
  }
  if (from_persisted) {
    Serial.printf("[UPLOAD_QUEUE] restored persisted job_id=%lu retries=%u\n",
                  (unsigned long)job_id,
                  (unsigned)retries);
  }
  return true;
}

static bool queue_voice_upload_job(uint32_t job_id,
                                   uint8_t* audio_buf,
                                   size_t audio_len,
                                   uint8_t retries,
                                   bool from_persisted,
                                   uint32_t created_epoch) {
  if (!audio_buf || audio_len == 0 || !upload_queue || upload_queue_is_full()) {
    return false;
  }
  UploadJob job = {};
  job.job_id = job_id;
  job.is_voice = true;
  strncpy(job.mode, "voice", sizeof(job.mode) - 1);
  job.mode[sizeof(job.mode) - 1] = '\0';
  job.image_buf = audio_buf;
  job.image_len = audio_len;
  job.retries = retries;
  job.from_persisted = from_persisted;
  job.created_ms = millis();
  job.created_epoch = created_epoch;
  BaseType_t ok = xQueueSend(upload_queue, &job, pdMS_TO_TICKS(10));
  if (ok != pdTRUE) {
    return false;
  }
  Serial.printf("[VOICE_QUEUE] queued job_id=%lu len=%u q=%lu\n",
                (unsigned long)job_id,
                (unsigned)audio_len,
                (unsigned long)upload_queue_count());
  if (from_persisted) {
    Serial.printf("[VOICE_QUEUE] restored persisted job_id=%lu retries=%u\n",
                  (unsigned long)job_id,
                  (unsigned)retries);
  }
  return true;
}

static void upload_worker_task(void *arg) {
  Serial.println("[UPLOAD] Background upload task started");
  for (;;) {
    UploadJob job = {};
    bool got_job = false;
    bool hold_normal = dish_upload_pending();
    if (upload_queue_dish != NULL &&
        xQueueReceive(upload_queue_dish, &job, pdMS_TO_TICKS(20)) == pdTRUE) {
      got_job = true;
    } else if (upload_worker_has_parked_job &&
               (scan_mode_is_dish(upload_worker_parked_job.mode) || !hold_normal)) {
      job = upload_worker_parked_job;
      upload_worker_has_parked_job = false;
      Serial.printf("[UPLOAD_QUEUE] resume_parked job_id=%lu mode=%s voice=%d stage=%s parked_ms=%lu q=%lu\n",
                    (unsigned long)job.job_id,
                    job.mode,
                    job.is_voice ? 1 : 0,
                    upload_worker_parked_stage ? upload_worker_parked_stage : "foreground",
                    upload_worker_parked_at_ms > 0 ? (unsigned long)(millis() - upload_worker_parked_at_ms) : 0UL,
                    (unsigned long)upload_queue_count());
      upload_worker_parked_stage = "idle";
      upload_worker_parked_at_ms = 0;
      got_job = true;
    } else if (!hold_normal && upload_queue != NULL &&
               xQueueReceive(upload_queue, &job, pdMS_TO_TICKS(200)) == pdTRUE) {
      got_job = true;
    } else if (hold_normal) {
      static unsigned long last_hold_log_ms = 0;
      unsigned long now_ms = millis();
      if (now_ms - last_hold_log_ms > 2000) {
        Serial.println("[UPLOAD_QUEUE] hold normal uploads (dish_scan_inflight)");
        last_hold_log_ms = now_ms;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (got_job) {
      if (!job.image_buf || job.image_len == 0) {
        Serial.println(job.is_voice ? "[VOICE_QUEUE] job missing audio buffer"
                                    : "[UPLOAD] job missing image buffer");
        diag_record_error(job.is_voice ? "voice_upload" : "upload", -1, "missing_buffer");
        if (scan_mode_is_dish(job.mode)) {
          clear_active_dish_job(job.job_id, "missing_buffer");
        }
        continue;
      }
      if (park_upload_job_if_foreground_active(job, "dequeued")) {
        vTaskDelay(pdMS_TO_TICKS(40));
        continue;
      }
      diag_record_action(job.is_voice ? "voice_upload" : "upload");
      upload_inflight = true;
      if (job.is_voice) {
        Serial.printf("[VOICE_QUEUE] start job_id=%lu len=%u q=%lu\n",
                      (unsigned long)job.job_id,
                      (unsigned)job.image_len,
                      (unsigned long)upload_queue_count());
        if (park_upload_job_if_foreground_active(job, "voice_post")) {
          upload_inflight = false;
          vTaskDelay(pdMS_TO_TICKS(40));
          continue;
        }
        bool accepted = false;
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("[VOICE_QUEUE] Wi-Fi not connected, connecting...");
          accepted = voice_ensure_wifi_connected() &&
                     voice_upload_and_parse(job.image_buf, job.image_len, job.job_id);
        } else {
          accepted = voice_upload_and_parse(job.image_buf, job.image_len, job.job_id);
        }
        if (accepted) {
          Serial.println("[VOICE_QUEUE] async accept complete; backend owns completion");
          if (job.from_persisted) {
            upload_persist_delete();
            upload_persist_note_event("retry_uploaded", job.mode, g_upload_persist_cached_count, job.retries);
            dump_system_truth("upload_retry_uploaded");
            Serial.printf("[UPLOAD_PERSIST] retry_uploaded job_id=%lu retries=%u cached=%u\n",
                          (unsigned long)job.job_id,
                          (unsigned)job.retries,
                          (unsigned)g_upload_persist_cached_count);
          }
        } else {
          Serial.println("[VOICE_QUEUE] upload failed or backend rejected request");
          upload_persist_handle_failure(job, "voice_post_fail");
        }
        free(job.image_buf);
        upload_inflight = false;
        continue;
      }
      uint32_t job_start_ms = millis();
      uint32_t job_deadline_ms = job_start_ms + ACTION_AWAKE_BUDGET_MS;
      bool budget_exhausted = false;
      const bool is_check = scan_mode_is_check(job.mode);
      const bool is_discard = scan_mode_is_discard(job.mode);
      const bool is_dish = scan_mode_is_dish(job.mode);
      uint32_t presign_deadline_ms = is_dish ? job_deadline_ms : (millis() + BACKGROUND_UPLOAD_PRESIGN_BUDGET_MS);
      if (is_dish) {
        g_dish_timing = {};
        g_dish_timing.sense_job_id = job.job_id;
        g_dish_timing.upload_start_ms = job_start_ms;
      }
      Serial.printf("[UPLOAD] start job_id=%lu mode=%s len=%u q=%lu\n",
                    (unsigned long)job.job_id,
                    job.mode,
                    (unsigned)job.image_len,
                    (unsigned long)upload_queue_count());
      size_t psram_free = 0;
#if (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM)
      psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
      Serial.printf("[UPLOAD] mem_free heap=%u psram=%u\n",
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)psram_free);
      bool deferred_for_dish = false;

      if (is_dish) {
        active_dish_job_id = job.job_id;
      }

      if (!is_dish && dish_upload_pending()) {
        bool requeued = false;
        if (upload_queue && xQueueSendToFront(upload_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
          requeued = true;
        }
        if (requeued) {
          Serial.println("[UPLOAD_QUEUE] preempt normal upload for dish");
          upload_inflight = false;
          vTaskDelay(pdMS_TO_TICKS(20));
          continue;
        }
        Serial.println("[UPLOAD_QUEUE] preempt failed, continuing normal upload");
      }

      const uint32_t backoff_ms[] = {500, 1500, 3500};
      const uint8_t max_retries = 3;
      PresignReply upload_presign;
      bool presign_success = false;
      for (uint8_t attempt = 0; attempt < max_retries; ++attempt) {
        if (!upload_wait_for_foreground_clear_in_place(job, "presign", presign_deadline_ms, &budget_exhausted)) {
          break;
        }
        if (is_dish && attempt == 0 && g_dish_timing.presign_start_ms == 0) {
          g_dish_timing.presign_start_ms = millis();
        }
        uint32_t remaining_ms = deadline_remaining_ms(presign_deadline_ms);
        if (remaining_ms < ACTION_MIN_REMAINING_MS) {
          presign_set_error_text("Upload timeout");
          Serial.printf("[UPLOAD] budget_exceeded phase=presign job_id=%lu remaining=%lu\n",
                        (unsigned long)job.job_id,
                        (unsigned long)remaining_ms);
          diag_record_error("upload_presign", -1, "timeout");
          budget_exhausted = true;
          break;
        }
        if (!is_dish && dish_upload_pending()) {
          bool requeued = false;
          if (upload_queue && xQueueSendToFront(upload_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
            requeued = true;
          }
          if (requeued) {
            Serial.println("[UPLOAD_QUEUE] preempt before presign for dish");
            upload_inflight = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            deferred_for_dish = true;
            break;
          }
          Serial.println("[UPLOAD_QUEUE] preempt failed, continuing presign");
        }
        uint32_t tls_timeout = clamp_timeout_ms(15000, presign_deadline_ms);
        if (tls_timeout < ACTION_MIN_REMAINING_MS) {
          presign_set_error_text("Upload timeout");
          diag_record_error("upload_presign", -1, "timeout");
          budget_exhausted = true;
          break;
        }
        if (!net_ready_for_tls("upload", tls_timeout, job.mode, job.job_id, NULL)) {
          Serial.printf("[UPLOAD] net_not_ready attempt=%u\n", (unsigned)attempt + 1);
          diag_record_error("net_ready", -1, "tls_not_ready");
          uint32_t backoff = backoff_ms[attempt];
          remaining_ms = deadline_remaining_ms(presign_deadline_ms);
          if (remaining_ms < (backoff + ACTION_MIN_REMAINING_MS)) {
            presign_set_error_text("Upload timeout");
            diag_record_error("upload_presign", -1, "timeout");
            budget_exhausted = true;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(backoff));
          continue;
        }
        const char* expiry = (job.expiry_date[0] != '\0') ? job.expiry_date : NULL;
        presign_success = is_check ? get_presign_checkin(upload_presign, expiry, job.quantity, &job.camera_meta, presign_deadline_ms)
                                   : get_presign(upload_presign, job.mode, expiry, job.add_to_shopping_list, &job.camera_meta, presign_deadline_ms);
        if (presign_success) {
          if (is_dish) {
            g_dish_timing.presign_end_ms = millis();
            Serial.printf("[TIMING][DISH] job_id=%lu presign_ms=%lu upload_start_to_presign_ms=%lu\n",
                          (unsigned long)job.job_id,
                          (unsigned long)(g_dish_timing.presign_end_ms - g_dish_timing.presign_start_ms),
                          (unsigned long)(g_dish_timing.presign_end_ms - g_dish_timing.upload_start_ms));
          }
          break;
        }
        Serial.printf("[UPLOAD] presign_failed attempt=%u\n", (unsigned)attempt + 1);
        uint32_t backoff = backoff_ms[attempt];
        remaining_ms = deadline_remaining_ms(presign_deadline_ms);
        if (remaining_ms < (backoff + ACTION_MIN_REMAINING_MS)) {
          presign_set_error_text("Upload timeout");
          budget_exhausted = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff));
      }
      if (deferred_for_dish) {
        continue;
      }
      if (budget_exhausted) {
        if (is_dish) {
          scan_ui_status_emit("ERROR", presign_error_text(), job.mode, job.job_id, true);
        }
        upload_persist_handle_failure(job, "presign_timeout");
        free(job.image_buf);
        if (is_dish) {
          clear_active_dish_job(job.job_id, "presign_timeout");
        }
        upload_inflight = false;
        continue;
      }
      if (!presign_success) {
        if (is_dish) {
          scan_ui_status_emit("ERROR", presign_error_text(), job.mode, job.job_id, true);
        }
        diag_record_action_event("upload", job.mode, "err", "presign", -1);
        upload_persist_handle_failure(job, "presign_fail");
        free(job.image_buf);
        if (is_dish) {
          clear_active_dish_job(job.job_id, "presign_fail");
        }
        upload_inflight = false;
        continue;
      }

      if (is_dish) {
        if (upload_presign.result_url.length() > 0) {
          Serial.printf("[RESULT_HTTP] result_url=%s\n", upload_presign.result_url.c_str());
        }
      }

      uint32_t put_deadline_ms = is_dish ? job_deadline_ms : (millis() + BACKGROUND_UPLOAD_PUT_BUDGET_MS);
      bool upload_success = false;
      for (uint8_t attempt = 0; attempt < max_retries; ++attempt) {
        if (!upload_wait_for_foreground_clear_in_place(job, "put", put_deadline_ms, &budget_exhausted)) {
          break;
        }
        uint32_t remaining_ms = deadline_remaining_ms(put_deadline_ms);
        if (remaining_ms < ACTION_MIN_REMAINING_MS) {
          presign_set_error_text("Upload timeout");
          Serial.printf("[UPLOAD] budget_exceeded phase=put job_id=%lu remaining=%lu\n",
                        (unsigned long)job.job_id,
                        (unsigned long)remaining_ms);
          diag_record_error("upload_put", -1, "timeout");
          budget_exhausted = true;
          break;
        }
        if (!is_dish && dish_upload_pending()) {
          bool requeued = false;
          if (upload_queue && xQueueSendToFront(upload_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
            requeued = true;
          }
          if (requeued) {
            Serial.println("[UPLOAD_QUEUE] preempt before PUT for dish");
            upload_inflight = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            deferred_for_dish = true;
            break;
          }
          Serial.println("[UPLOAD_QUEUE] preempt failed, continuing PUT");
        }
        uint32_t tls_timeout = clamp_timeout_ms(15000, put_deadline_ms);
        if (tls_timeout < ACTION_MIN_REMAINING_MS) {
          presign_set_error_text("Upload timeout");
          budget_exhausted = true;
          break;
        }
        if (!net_ready_for_tls("upload_put", tls_timeout, job.mode, job.job_id, NULL)) {
          Serial.printf("[UPLOAD] net_not_ready_put attempt=%u\n", (unsigned)attempt + 1);
          uint32_t backoff = backoff_ms[attempt];
          remaining_ms = deadline_remaining_ms(put_deadline_ms);
          if (remaining_ms < (backoff + ACTION_MIN_REMAINING_MS)) {
            presign_set_error_text("Upload timeout");
            budget_exhausted = true;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(backoff));
          continue;
        }
        bool aborted_for_dish = false;
        bool aborted_for_budget = false;
        diag_note_stage("upload_put", 0);
        if (is_dish && attempt == 0 && g_dish_timing.put_start_ms == 0) {
          g_dish_timing.put_start_ms = millis();
        }
        upload_success = put_to_presigned_url(upload_presign.put_url,
                                              job.image_buf,
                                              job.image_len,
                                              upload_presign.content_type.length() ? upload_presign.content_type.c_str() : "image/jpeg",
                                              job.job_id,
                                              !is_dish,
                                              &aborted_for_dish,
                                              put_deadline_ms,
                                              &aborted_for_budget);
        if (aborted_for_dish) {
          bool requeued = false;
          if (upload_queue && xQueueSendToFront(upload_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
            requeued = true;
          }
          if (requeued) {
            Serial.println("[UPLOAD_QUEUE] preempt mid-PUT for dish");
            upload_inflight = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            deferred_for_dish = true;
            break;
          }
          Serial.println("[UPLOAD_QUEUE] preempt mid-PUT failed, continuing");
        }
        if (aborted_for_budget) {
          presign_set_error_text("Upload timeout");
          budget_exhausted = true;
          break;
        }
        if (upload_success) {
          if (is_dish) {
            g_dish_timing.put_end_ms = millis();
            Serial.printf("[TIMING][DISH] job_id=%lu put_ms=%lu upload_total_ms=%lu\n",
                          (unsigned long)job.job_id,
                          (unsigned long)(g_dish_timing.put_end_ms - g_dish_timing.put_start_ms),
                          (unsigned long)(g_dish_timing.put_end_ms - g_dish_timing.upload_start_ms));
          }
          break;
        }
        presign_set_error_text("Network error. Tap to retry.");
        Serial.printf("[UPLOAD] put_failed attempt=%u\n", (unsigned)attempt + 1);
        uint32_t backoff = backoff_ms[attempt];
        remaining_ms = deadline_remaining_ms(put_deadline_ms);
        if (remaining_ms < (backoff + ACTION_MIN_REMAINING_MS)) {
          presign_set_error_text("Upload timeout");
          budget_exhausted = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff));
      }
      if (deferred_for_dish) {
        continue;
      }
      if (budget_exhausted) {
        if (is_dish) {
          scan_ui_status_emit("ERROR", presign_error_text(), job.mode, job.job_id, true);
          waiting_for_mqtt_result = false;
          mqtt_wait_deadline = 0;
          current_scan_job_id = "";
          mqtt_clear_result_subscription();
        }
        diag_record_action_event("upload", job.mode, "err", "timeout", -1);
        upload_persist_handle_failure(job, "put_timeout");
        free(job.image_buf);
        if (is_dish) {
          clear_active_dish_job(job.job_id, "put_timeout");
        }
        upload_inflight = false;
        continue;
      }
      if (!upload_success) {
        if (is_dish) {
          scan_ui_status_emit("ERROR", presign_error_text(), job.mode, job.job_id, true);
          waiting_for_mqtt_result = false;
          mqtt_wait_deadline = 0;
          current_scan_job_id = "";
          mqtt_clear_result_subscription();
        }
        diag_record_action_event("upload", job.mode, "err", "put_fail", -1);
        upload_persist_handle_failure(job, "put_fail");
        free(job.image_buf);
        if (is_dish) {
          clear_active_dish_job(job.job_id, "put_fail");
        }
        upload_inflight = false;
        continue;
      }

      diag_record_action_event("upload", job.mode, "ok", "put", 0);
      if (job.from_persisted) {
        upload_persist_delete();
        upload_persist_note_event("retry_uploaded", job.mode, g_upload_persist_cached_count, job.retries);
        dump_system_truth("upload_retry_uploaded");
        Serial.printf("[UPLOAD_PERSIST] retry_uploaded job_id=%lu retries=%u cached=%u\n",
                      (unsigned long)job.job_id,
                      (unsigned)job.retries,
                      (unsigned)g_upload_persist_cached_count);
      }

      if (is_dish) {
        waiting_for_mqtt_result = false;
        mqtt_wait_deadline = 0;
        current_scan_job_id = "";
        mqtt_clear_result_subscription();
        clear_active_dish_job(job.job_id, "upload_complete");
      }

      free(job.image_buf);
      size_t psram_after = 0;
#if (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM)
      psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
      Serial.printf("[UPLOAD_DONE] mode=%s job_id=%lu heap=%u psram=%u q=%lu\n",
                    job.mode,
                    (unsigned long)job.job_id,
                    (unsigned)ESP.getFreeHeap(),
                    (unsigned)psram_after,
                    (unsigned long)upload_queue_count());
      upload_inflight = false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// presign_set_error_text, presign_error_text → sense_upload.h

static bool net_ready_for_tls(const char* reason, uint32_t timeout_ms, const char* mode, uint32_t job_id, const char* ui_policy) {
  (void)ui_policy;
  if (!ensure_wifi_ready(reason, timeout_ms)) {
    presign_set_error_text("Wi-Fi not ready");
    return false;
  }
  time_t now = time(nullptr);
  if (now < 1700000000) {
    scan_ui_status_emit("UPLOAD_STARTING", "Syncing time…", mode, job_id, false);
  }
  if (!ensure_time_valid(reason, timeout_ms)) {
    presign_set_error_text("Time sync failed");
    return false;
  }
  return true;
}

// wifi_hard_reset_and_reconnect, wifi_recover_if_needed → sense_wifi.h
// http_queue_lock, http_queue_unlock, http_post_json_with_retries,
// http_get_with_retries, presign_set_error_text, presign_error_text,
// net_ready_for_tls(char*,size_t) → sense_upload.h

// http_queue_lock, http_queue_unlock → sense_upload.h

// http_post_json_with_retries, http_get_with_retries → sense_upload.h

static String build_dish_result_url(const char* user_id, const char* device_id, const char* job_id) {
  if (!user_id || user_id[0] == '\0' ||
      !device_id || device_id[0] == '\0' ||
      !job_id || job_id[0] == '\0') {
    return String();
  }
  return String(CHECKIN_API_BASE_URL) + "/dish/result?user_id=" + String(user_id) +
         "&device_id=" + String(device_id) +
         "&job_id=" + String(job_id);
}

static uint32_t dish_result_poll_delay_ms(const JsonDocument& doc, uint32_t fallback_ms) {
  uint32_t poll_after_ms = doc["poll_after_ms"] | fallback_ms;
  if (poll_after_ms < 250) {
    poll_after_ms = 250;
  }
  if (poll_after_ms > 5000) {
    poll_after_ms = 5000;
  }
  return poll_after_ms;
}

static bool wait_for_dish_result_http(const UploadJob& job,
                                      const PresignReply& presign,
                                      uint32_t job_deadline_ms) {
  auto clear_wait_state = [&]() {
    waiting_for_mqtt_result = false;
    mqtt_wait_deadline = 0;
    current_scan_job_id = "";
    mqtt_clear_result_subscription();
  };

  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));

  String result_url = presign.result_url;
  if (result_url == "null") {
    result_url = "";
  }
  if (result_url.length() == 0) {
    result_url = build_dish_result_url(owner_id, device_id, presign.job_id.c_str());
  }
  if (result_url.length() == 0) {
    Serial.println("[RESULT_HTTP] missing result_url");
    scan_ui_status_emit("ERROR", "Dish result URL missing", job.mode, job.job_id, true);
    diag_record_error("result_http", -1, "missing_result_url");
    diag_record_action_event("result", job.mode, "err", "missing_url", -1);
    clear_wait_state();
    clear_active_dish_job(job.job_id, "http_missing_result_url");
    return false;
  }

  uint32_t remaining_ms = deadline_remaining_ms(job_deadline_ms);
  uint32_t max_wait_ms = (remaining_ms < DISH_RESULT_TIMEOUT_MS) ? remaining_ms : DISH_RESULT_TIMEOUT_MS;
  if (max_wait_ms < ACTION_MIN_REMAINING_MS) {
    scan_ui_status_emit("ERROR", "Analysis timeout", job.mode, job.job_id, true);
    diag_record_error("result_http", -1, "timeout_budget");
    diag_record_action_event("result", job.mode, "err", "timeout_budget", -1);
    clear_wait_state();
    clear_active_dish_job(job.job_id, "http_timeout_budget");
    return false;
  }

  clear_wait_state();
  waiting_for_mqtt_result = true;  // Reused as generic dish-result wait state.
  mqtt_wait_deadline = millis() + max_wait_ms;
  scan_ui_status_emit("RESULT_WAITING", "AI working its magic", job.mode, job.job_id, false);
  flow_step(job.job_id, "RESULT_WAITING");
  g_dish_timing.mqtt_refresh_start_ms = millis();
  g_dish_timing.mqtt_refresh_end_ms = millis();
  g_dish_timing.result_wait_start_ms = millis();

  const uint32_t local_job_id = current_result_local_job_id ? current_result_local_job_id : job.job_id;
  const char* result_mode = current_result_mode[0] ? current_result_mode : job.mode;
  bool had_fast_result = false;
  bool fast_emitted = false;
  uint32_t wait_start = millis();

  Serial.printf("[RESULT_HTTP] start job_id=%s timeout_ms=%lu url=%s\n",
                presign.job_id.c_str(),
                (unsigned long)max_wait_ms,
                result_url.c_str());
  Serial.printf("[TIMING][DISH] job_id=%lu result_http_setup_ms=%lu upload_to_wait_ms=%lu wait_budget_ms=%lu\n",
                (unsigned long)job.job_id,
                (unsigned long)(g_dish_timing.mqtt_refresh_end_ms - g_dish_timing.mqtt_refresh_start_ms),
                (unsigned long)(g_dish_timing.result_wait_start_ms - g_dish_timing.put_end_ms),
                (unsigned long)max_wait_ms);

  while (waiting_for_mqtt_result &&
         !deadline_expired(job_deadline_ms) &&
         (millis() - wait_start) < max_wait_ms) {
    int http_code = 0;
    String resp_body;
    if (!http_get_with_retries(result_url.c_str(),
                               http_code,
                               resp_body,
                               "DISH_RESULT",
                               API_KEY,
                               BEARER_TOKEN,
                               job.job_id,
                               job_deadline_ms)) {
      if (http_code == 404) {
        scan_ui_status_emit("ERROR", "Unknown dish job", job.mode, job.job_id, true);
        diag_record_error("result_http", http_code, "unknown_job");
        diag_record_action_event("result", job.mode, "err", "http_404", http_code);
        clear_wait_state();
        clear_active_dish_job(job.job_id, "http_unknown_job");
        return false;
      }
      if (http_code == 401 || http_code == 403) {
        scan_ui_status_emit("ERROR", "Dish auth failed", job.mode, job.job_id, true);
        diag_record_error("result_http", http_code, "auth");
        diag_record_action_event("result", job.mode, "err", "http_auth", http_code);
        clear_wait_state();
        clear_active_dish_job(job.job_id, "http_auth_error");
        return false;
      }
      if (http_code >= 400 && http_code < 500) {
        scan_ui_status_emit("ERROR", "Dish result failed", job.mode, job.job_id, true);
        diag_record_error("result_http", http_code, "client_error");
        diag_record_action_event("result", job.mode, "err", "http_client", http_code);
        clear_wait_state();
        clear_active_dish_job(job.job_id, "http_client_error");
        return false;
      }

      uint32_t retry_delay_ms = clamp_timeout_ms(had_fast_result ? 1500UL : 1000UL, job_deadline_ms);
      Serial.printf("[RESULT_HTTP] transient_error code=%d delay_ms=%lu had_fast=%d\n",
                    http_code,
                    (unsigned long)retry_delay_ms,
                    had_fast_result ? 1 : 0);
      if (retry_delay_ms == 0) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
      continue;
    }

    DynamicJsonDocument doc(6144);
    DeserializationError err = deserializeJson(doc, resp_body);
    if (err) {
      uint32_t retry_delay_ms = clamp_timeout_ms(had_fast_result ? 1500UL : 1000UL, job_deadline_ms);
      Serial.printf("[RESULT_HTTP] parse_error=%s delay_ms=%lu\n",
                    err.c_str(),
                    (unsigned long)retry_delay_ms);
      if (retry_delay_ms == 0) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
      continue;
    }

    const char* state = doc["state"] | "";
    const char* phase = doc["phase"] | "";
    const char* latest_phase = doc["latest_phase"] | "";
    const char* pipeline_stage = doc["pipeline_stage"] | "";
    bool is_terminal = doc["is_terminal"] | false;
    bool is_pending = strcmp(state, "pending") == 0 || strcmp(phase, "pending") == 0;
    bool is_fast = strcmp(phase, "fast") == 0 ||
                   (!is_terminal && strcmp(latest_phase, "fast") == 0 && strcmp(state, "ready") == 0);
    bool is_final = strcmp(phase, "final") == 0 ||
                    is_terminal ||
                    strcmp(latest_phase, "final") == 0 ||
                    (strcmp(state, "ready") == 0 && strcmp(pipeline_stage, "DONE") == 0);
    uint32_t poll_after_ms = dish_result_poll_delay_ms(doc, is_fast ? 1500UL : 1000UL);

    Serial.printf("[RESULT_HTTP] job_id=%s state=%s phase=%s latest=%s terminal=%d stage=%s poll_after_ms=%lu\n",
                  presign.job_id.c_str(),
                  state,
                  phase,
                  latest_phase,
                  is_terminal ? 1 : 0,
                  pipeline_stage,
                  (unsigned long)poll_after_ms);

    if (is_pending) {
      vTaskDelay(pdMS_TO_TICKS(poll_after_ms));
      continue;
    }

    if (!is_fast && !is_final) {
      const char* msg = doc["message"] | "";
      if (msg && msg[0]) {
        uart_send_ui_toast(msg);
      }
      vTaskDelay(pdMS_TO_TICKS(poll_after_ms));
      continue;
    }

    const char* meal_summary = doc["meal_summary"] | "";
    const char* summary = doc["summary"] | "";
    const char* dish_name = doc["dish_name"] | "";
    String summary_text;
    if (meal_summary && meal_summary[0]) {
      summary_text = meal_summary;
    } else if (summary && summary[0]) {
      summary_text = summary;
    } else if (dish_name && dish_name[0]) {
      summary_text = dish_name;
    }

    int calories = doc["calories"] | 0;
    if (doc["calories"].isNull()) {
      calories = doc["kcal"] | 0;
    }
    float protein_g = doc["protein_g"] | 0.0f;
    float carbs_g = doc["carbs_g"] | 0.0f;
    float fat_g = doc["fat_g"] | 0.0f;
    float confidence = doc["confidence"] | 0.0f;
    int health_score = doc["health_score"] | 0;
    const char* recommendation = doc["recommendation"] | "";

    if (summary_text.length() > 0 && (is_final || !fast_emitted)) {
      uart_send_ui_meal_result(calories,
                               summary_text.c_str(),
                               health_score,
                               (recommendation && recommendation[0]) ? recommendation : NULL,
                               result_mode,
                               protein_g,
                               carbs_g,
                               fat_g,
                               confidence,
                               local_job_id);
    } else if (summary_text.length() == 0) {
      const char* msg = doc["message"] | "";
      if (msg && msg[0]) {
        uart_send_ui_toast(msg);
      }
    }

    if (is_fast) {
      had_fast_result = true;
      if (summary_text.length() > 0) {
        fast_emitted = true;
      }
      diag_record_action_event("result", job.mode, "ok", "http_fast", 0);
      uint32_t result_now_ms = millis();
      Serial.printf("[TIMING][DISH] job_id=%lu result_http_fast total_ms=%lu post_upload_to_result_ms=%lu result_wait_loop_ms=%lu ui_wait_ms=%lu\n",
                    (unsigned long)g_dish_timing.sense_job_id,
                    (unsigned long)(g_dish_timing.upload_start_ms ? (result_now_ms - g_dish_timing.upload_start_ms) : 0),
                    (unsigned long)(g_dish_timing.put_end_ms ? (result_now_ms - g_dish_timing.put_end_ms) : 0),
                    (unsigned long)(g_dish_timing.result_wait_start_ms ? (result_now_ms - g_dish_timing.result_wait_start_ms) : 0),
                    (unsigned long)(g_dish_timing.ui_wait_start_ms ? (result_now_ms - g_dish_timing.ui_wait_start_ms) : 0));
      clear_wait_state();
      clear_active_dish_job(job.job_id, "http_fast_result");
      Serial.println("[RESULT_HTTP] dish result processed (fast)");
      return true;
    }

    diag_record_action_event("result", job.mode, "ok", "http_final", 0);
    uint32_t result_now_ms = millis();
    Serial.printf("[TIMING][DISH] job_id=%lu result_http_final total_ms=%lu post_upload_to_result_ms=%lu result_wait_loop_ms=%lu ui_wait_ms=%lu\n",
                  (unsigned long)g_dish_timing.sense_job_id,
                  (unsigned long)(g_dish_timing.upload_start_ms ? (result_now_ms - g_dish_timing.upload_start_ms) : 0),
                  (unsigned long)(g_dish_timing.put_end_ms ? (result_now_ms - g_dish_timing.put_end_ms) : 0),
                  (unsigned long)(g_dish_timing.result_wait_start_ms ? (result_now_ms - g_dish_timing.result_wait_start_ms) : 0),
                  (unsigned long)(g_dish_timing.ui_wait_start_ms ? (result_now_ms - g_dish_timing.ui_wait_start_ms) : 0));
    clear_wait_state();
    clear_active_dish_job(job.job_id, "http_result_final");
    Serial.println("[RESULT_HTTP] dish result processed (final)");
    return true;
  }

  uint32_t timeout_now_ms = millis();
  Serial.printf("[TIMING][DISH] job_id=%lu timeout total_ms=%lu post_upload_wait_ms=%lu result_wait_loop_ms=%lu ui_wait_ms=%lu had_fast=%d\n",
                (unsigned long)job.job_id,
                (unsigned long)(timeout_now_ms - g_dish_timing.upload_start_ms),
                (unsigned long)(timeout_now_ms - g_dish_timing.put_end_ms),
                (unsigned long)(timeout_now_ms - g_dish_timing.result_wait_start_ms),
                (unsigned long)(g_dish_timing.ui_wait_start_ms ? (timeout_now_ms - g_dish_timing.ui_wait_start_ms) : 0),
                had_fast_result ? 1 : 0);
  clear_wait_state();
  if (had_fast_result) {
    diag_record_action_event("result", job.mode, "ok", "http_fast_timeout", 0);
    clear_active_dish_job(job.job_id, "http_fast_timeout");
    Serial.println("[RESULT_HTTP] final timeout -> keeping last fast result");
    return true;
  }

  scan_ui_status_emit("ERROR", "Analysis timeout", job.mode, job.job_id, true);
  diag_record_error("result_http", -1, "timeout");
  diag_record_action_event("result", job.mode, "err", "timeout", -1);
  clear_active_dish_job(job.job_id, "http_result_timeout");
  Serial.println("[RESULT_HTTP] dish result timeout");
  return false;
}

// ── Check-in Presign Function (Grocery Recognition API) ───────
static bool get_presign_checkin(PresignReply& out, const char* expiry_date, uint16_t quantity, const UploadJob::CameraUploadMeta* camera_meta, uint32_t deadline_ms) {
  // Sense_Minimal/Sense_Minimal.ino: get_presign_checkin
  String presign_url = String(CHECKIN_API_BASE_URL) + String(CHECKIN_PRESIGN_ENDPOINT);

  // Build request body for check-in API
  StaticJsonDocument<512> doc;
  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));
  doc["user_id"] = owner_id;
  doc["device_id"] = device_id;
  doc["owner"] = owner_id;
  doc["action"] = "IN";  // "IN" for adding items to inventory
  doc["type"] = "grocery";
  doc["content_type"] = "image/jpeg";
  
  // Add expiration date if provided
  if (expiry_date != NULL && strlen(expiry_date) > 0) {
    doc["product_expiration"] = expiry_date;
    Serial.printf("[CHECKIN_PRESIGN] Including expiration date: %s\n", expiry_date);
  }
  uint16_t safe_quantity = (quantity < 1) ? 1 : quantity;
  doc["quantity"] = safe_quantity;
  Serial.printf("[CHECKIN_PRESIGN] Including quantity: %u\n", (unsigned)safe_quantity);
  if (camera_meta) {
    append_camera_meta_json(doc, *camera_meta);
  }
  
  String body; 
  serializeJson(doc, body);
  
  log_camera_meta_for_presign("CHECKIN_PRESIGN", camera_meta);
  Serial.println("[CHECKIN_PRESIGN] POST " + presign_url);
  Serial.println("[CHECKIN_PRESIGN] Body: " + body);
  
  int http_code = 0;
  String resp_body;
  if (!http_post_json_with_retries(presign_url.c_str(),
                                   body,
                                   http_code,
                                   resp_body,
                                   "CHECKIN_PRESIGN",
                                   NULL,
                                   NULL,
                                   current_job.job_id,
                                   deadline_ms)) {
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
    diag_record_error("presign_parse", -1, "json_parse");
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
    diag_record_error("presign_parse", -1, "missing_fields");
  }
  return success;
}


static bool put_to_presigned_url(const String& url,
                                 const uint8_t* buf,
                                 size_t len,
                                 const char* contentType,
                                 uint32_t job_id,
                                 bool allow_abort,
                                 bool* aborted_for_dish,
                                 uint32_t deadline_ms,
                                 bool* aborted_for_budget) {
  Serial.printf("[UPLOAD] Starting PUT to S3, size: %u bytes\n", len);
  if (aborted_for_dish) {
    *aborted_for_dish = false;
  }
  if (aborted_for_budget) {
    *aborted_for_budget = false;
  }
  if (deadline_expired(deadline_ms)) {
    if (aborted_for_budget) {
      *aborted_for_budget = true;
    }
    presign_set_error_text("Upload timeout");
    diag_record_error("upload_put", -1, "deadline_expired");
    return false;
  }
  if (allow_abort && dish_upload_pending()) {
    if (aborted_for_dish) {
      *aborted_for_dish = true;
    }
    Serial.println("[UPLOAD] abort before connect (dish preempt)");
    return false;
  }
  uint32_t effective_job = job_id;
  if (effective_job == 0 && current_job.active) {
    effective_job = current_job.job_id;
  }
  http_queue_lock("UPLOAD_PUT", effective_job);

  bool https = false;
  String host;
  String path;
  uint16_t port = 0;
  if (!parse_url_parts(url, https, host, port, path)) {
    Serial.println("[UPLOAD] URL parse failed for PUT");
    diag_record_error("upload_put", -1, "bad_url");
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "bad_url");
    http_queue_unlock("UPLOAD_PUT", effective_job);
    return false;
  }

  if (deadline_expired(deadline_ms)) {
    if (aborted_for_budget) {
      *aborted_for_budget = true;
    }
    presign_set_error_text("Upload timeout");
    diag_record_error("upload_put", -1, "deadline_expired");
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "deadline_expired");
    http_queue_unlock("UPLOAD_PUT", effective_job);
    return false;
  }
  WiFiClientSecure tls; 
  tls.setInsecure();
  uint32_t tls_timeout = clamp_timeout_ms(60000, deadline_ms);
  if (tls_timeout < ACTION_MIN_REMAINING_MS) {
    if (aborted_for_budget) {
      *aborted_for_budget = true;
    }
    presign_set_error_text("Upload timeout");
    diag_record_error("upload_put", -1, "timeout");
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "timeout");
    http_queue_unlock("UPLOAD_PUT", effective_job);
    return false;
  }
  tls.setTimeout(tls_timeout);  // timeout for large uploads

  if (!tls.connect(host.c_str(), port)) {
    Serial.println("[UPLOAD] TLS connect failed for PUT");
    diag_record_error("upload_put", -1, "tls_connect");
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "tls_connect");
    http_queue_unlock("UPLOAD_PUT", effective_job);
    return false;
  }
  
  const char* resolved_ct = (contentType && *contentType && strcmp(contentType, "null") != 0)
                              ? contentType
                              : "image/jpeg";
  Serial.printf("[UPLOAD] Using Content-Type: %s\n", resolved_ct);

  tls.printf("PUT %s HTTP/1.1\r\n", path.c_str());
  tls.printf("Host: %s\r\n", host.c_str());
  tls.printf("Content-Type: %s\r\n", resolved_ct);
  tls.printf("Content-Length: %u\r\n", (unsigned)len);
  tls.print("Connection: close\r\n\r\n");

  Serial.println("[UPLOAD] Sending PUT request...");
  unsigned long upload_start = millis();

  size_t offset = 0;
  const size_t chunk_size = 2048;
  bool write_ok = true;
  while (offset < len) {
    if (deadline_expired(deadline_ms)) {
      if (aborted_for_budget) {
        *aborted_for_budget = true;
      }
      presign_set_error_text("Upload timeout");
      Serial.println("[UPLOAD] abort during PUT (budget)");
      diag_record_error("upload_put", -1, "timeout");
      uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "timeout");
      tls.stop();
      http_queue_unlock("UPLOAD_PUT", effective_job);
      return false;
    }
    if (allow_abort && dish_upload_pending()) {
      if (aborted_for_dish) {
        *aborted_for_dish = true;
      }
      Serial.println("[UPLOAD] abort during PUT (dish preempt)");
      diag_record_error("upload_put", -1, "dish_preempt");
      uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "dish_preempt");
      tls.stop();
      http_queue_unlock("UPLOAD_PUT", effective_job);
      return false;
    }
    size_t to_write = len - offset;
    if (to_write > chunk_size) {
      to_write = chunk_size;
    }
    int written = tls.write(buf + offset, to_write);
    if (written <= 0) {
      write_ok = false;
      break;
    }
    offset += (size_t)written;
  }
  
  unsigned long upload_duration = millis() - upload_start;
  Serial.printf("[UPLOAD] PUT completed in %lu ms\n", upload_duration);
  
  if (!write_ok) {
    Serial.println("[UPLOAD] PUT write failed");
    diag_record_error("upload_put", -1, "write_failed");
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", -1, "write_failed");
    tls.stop();
    http_queue_unlock("UPLOAD_PUT", effective_job);
    Serial.println("[UPLOAD] ✗ Upload failed");
    return false;
  }

  String status_line = tls.readStringUntil('\n');
  status_line.trim();
  int code = -1;
  if (status_line.startsWith("HTTP/")) {
    int space = status_line.indexOf(' ');
    if (space > 0) {
      code = status_line.substring(space + 1).toInt();
    }
  }

  String resp;
  const unsigned long resp_deadline = millis() + 2000;
  while (millis() < resp_deadline) {
    while (tls.available()) {
      char c = (char)tls.read();
      if (resp.length() < 1024) {
        resp += c;
      }
    }
    if (!tls.available()) {
      delay(10);
    }
  }
  tls.stop();
  http_queue_unlock("UPLOAD_PUT", effective_job);

  Serial.printf("[UPLOAD] PUT status: %d\n", code);
  if (resp.length()) {
    Serial.print("[UPLOAD] PUT response: ");
    Serial.println(resp);
  }
  
  bool success = (code >= 200 && code < 300);
  if (success) {
    Serial.println("[UPLOAD] ✓ Upload successful");
    uart_send_sense_diag("http", "success", "UPLOAD_PUT", (int32_t)code, "put");
  } else {
    Serial.println("[UPLOAD] ✗ Upload failed");
    diag_record_error("upload_put", code, "http_status");
    char detail[32];
    snprintf(detail, sizeof(detail), "http_status=%d", code);
    uart_send_sense_diag("http", "fail", "UPLOAD_PUT", (int32_t)code, detail);
  }
  
  return success;
}

// ── MQTT Functions → sense_mqtt.h ─────────────────────────────────

static void uart_send_ui_meal_result(int kcal, const char* meal_summary, int health_score, const char* recommendation, const char* mode, float protein_g, float carbs_g, float fat_g, float confidence, uint32_t job_id) {
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
  if (job_id != 0) {
    doc["job_id"] = job_id;
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
  uart_rx_count++;
  uart_note_rx_type(type);
  bool is_user_input_message = (strncmp(type, "INPUT_", 6) == 0) &&
                               strcmp(type, "INPUT_SLEEP") != 0 &&
                               strcmp(type, "INPUT_PING") != 0;
  if (is_user_input_message) {
    cancel_pending_sleep_for_user_action(type);
  }
  if (strcmp(type, "LCD_DIAG") == 0) {
    const char* wake = doc["wake"] | "";
    const char* screen = doc["screen"] | "";
    const char* input = doc["input"] | "";
    const char* last_tx = doc["last_tx"] | "";
    const char* last_rx = doc["last_rx"] | "";
    lcd_diag_input_age_ms = doc["input_age_ms"] | -1;
    lcd_diag_sense_rx_age_ms = doc["sense_rx_age_ms"] | -1;
    lcd_diag_uart_tx = doc["uart_tx"] | 0;
    lcd_diag_uart_rx = doc["uart_rx"] | 0;
    strncpy(lcd_diag_wake, wake, sizeof(lcd_diag_wake) - 1);
    lcd_diag_wake[sizeof(lcd_diag_wake) - 1] = '\0';
    strncpy(lcd_diag_screen, screen, sizeof(lcd_diag_screen) - 1);
    lcd_diag_screen[sizeof(lcd_diag_screen) - 1] = '\0';
    strncpy(lcd_diag_last_input, input, sizeof(lcd_diag_last_input) - 1);
    lcd_diag_last_input[sizeof(lcd_diag_last_input) - 1] = '\0';
    strncpy(lcd_diag_last_tx, last_tx, sizeof(lcd_diag_last_tx) - 1);
    lcd_diag_last_tx[sizeof(lcd_diag_last_tx) - 1] = '\0';
    strncpy(lcd_diag_last_rx, last_rx, sizeof(lcd_diag_last_rx) - 1);
    lcd_diag_last_rx[sizeof(lcd_diag_last_rx) - 1] = '\0';
    last_lcd_diag_ms = millis();
    return true;
  }
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
    Serial.println("[UART] LCD wake-up detected - list refresh disabled");
    last_wake_ms = now_ms;
  } else if (strcmp(type, "MAINT_WINDOW_ACK") == 0) {
    uint32_t remaining_s = doc["remaining_s"] | 0;
    uint32_t wake_in_s = doc["wake_in_s"] | 0;
    bool clear = doc["clear"] | false;
    const char* request_id = doc["request_id"] | "";
    const char* status = doc["status"] | "";
    bool persisted = doc["persisted"] | false;
    uint64_t start_epoch = doc["start_epoch"] | 0ULL;
    uint32_t duration_sec = doc["duration_sec"] | 0;
    uint32_t grace_before_sec = doc["grace_before_sec"] | 0;
    uint32_t grace_after_sec = doc["grace_after_sec"] | 0;
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_on_lcd_maint_ack(remaining_s,
                               wake_in_s,
                               clear,
                               request_id,
                               status,
                               persisted,
                               start_epoch,
                               duration_sec,
                               grace_before_sec,
                               grace_after_sec);
#endif
    Serial.printf("[UART] MAINT_WINDOW_ACK received remaining_s=%lu wake_in_s=%lu clear=%d request_id=%s status=%s persisted=%d\n",
                  (unsigned long)remaining_s,
                  (unsigned long)wake_in_s,
                  clear ? 1 : 0,
                  request_id && request_id[0] ? request_id : "-",
                  status && status[0] ? status : "-",
                  persisted ? 1 : 0);
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
    voice_finalize_requested = false;
    if (op_queue != NULL) {
      OpJob job = {OP_VOICE, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, "", ""};
      job.quantity = 1;
      if (enqueue_op_job(job, false, "voice_long_press_start")) {
        Serial.println("[OP] VOICE job queued");
      } else {
        Serial.println("[OP] Failed to queue VOICE job (queue full?)");
      }
    }
  } else if (strcmp(type, "INPUT_LONG_PRESS_END") == 0) {
    // Long press end event from LCD (user released after long press)
    Serial.println("[UART] Long press END received - signaling VOICE to finalize");
    voice_request_finalize("lcd_long_press_end");
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
      OpJob job = {OP_SCAN, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, "", ""};
      strncpy(job.mode, "dish", sizeof(job.mode) - 1);
      job.expiry_date[0] = '\0';
      job.quantity = 1;
      if (xQueueSend(op_queue, &job, pdMS_TO_TICKS(10)) == pdTRUE) {
        Serial.println("[OP] SCAN job queued (dish mode)");
      } else {
        Serial.println("[OP] Failed to queue SCAN job (queue full?)");
      }
    }
  } else if (strcmp(type, "INPUT_SLEEP") == 0) {
    // LCD is going to sleep - Sense should also sleep
    sleep_intent_pending = false;
    uint32_t msg_id = doc["msg_id"] | 0;
    unsigned long now_ms = millis();
    Serial.printf("[SLEEP_PROTO] rx INPUT_SLEEP -> evaluate msg_id=%u\n", (unsigned)msg_id);
    const char* deny_reason = NULL;
    if (!deny_reason) {
      wake_pin_configure_rtc_input_inactive_pull();
      int wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      if (wake_pin_is_active_level(wake_pin_level)) {
        Serial.printf("[SLEEP_SANITY] wake_pin_active_at_input_sleep level=%d wake_active=%d\n",
                      wake_pin_level,
                      WAKE_ACTIVE_LEVEL);
        deny_reason = "wake_pin_active";
      }
    }
    Serial.printf("[WAKE_PIN] gpio2=%d phase=input_sleep\n",
                  rtc_gpio_get_level(WAKE_GPIO));

    bool new_request = (sleep_sm_state == SLEEP_SM_IDLE);
    sleep_requested = true;
    sleep_coord_requested = true;
    if (new_request) {
      sleep_ready_sent_for_cycle = false;
      sleep_deny_sent_for_request = false;
      sleep_sm_msg_id = msg_id;
      sleep_sm_transition(SLEEP_SM_REQ_RX, "INPUT_SLEEP", msg_id);
    }
    if (sleep_request_ms == 0) {
      sleep_request_ms = now_ms;
    }
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
  } else if (strcmp(type, "SLEEP_DENY") == 0) {
    const char* reason = doc["reason"] | "unknown";
    uint32_t retry_ms = (uint32_t)(doc["retry_ms"] | SLEEP_DENY_RETRY_DEFAULT_MS);
    sleep_intent_pending = false;
    sleep_intent_cooldown_until_ms = millis() + retry_ms;
    Serial.printf("[SLEEP_INTENT] rx SLEEP_DENY reason=%s retry_ms=%lu\n",
                  reason,
                  (unsigned long)retry_ms);
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
  } else if (strcmp(type, "INPUT_FW_INFO") == 0) {
    unsigned long now_ms = millis();
    unsigned long wake_age_ms = last_input_wake_ms > 0 ? (now_ms - last_input_wake_ms) : 0xFFFFFFFFUL;
    unsigned long rx_age_ms = last_lcd_communication > 0 ? (now_ms - last_lcd_communication) : 0xFFFFFFFFUL;
    Serial.printf("[UART] INPUT_FW_INFO received - sending FW_INFO wake_age_ms=%lu rx_age_ms=%lu inflight=%d fg=%d\n",
                  wake_age_ms,
                  rx_age_ms,
                  wifi_connect_inflight ? 1 : 0,
                  foreground_active ? 1 : 0);
    uart_send_fw_info();
  } else if (strcmp(type, "INPUT_OTA_CHECK") == 0) {
    const char* reason = doc["reason"] | "manual";
    Serial.printf("[UART] INPUT_OTA_CHECK received reason=%s\n", reason);
    uart_send_ui_status("Starting OTA...");
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_request_manual_ota(reason);
#else
    Serial.println("[UART] INPUT_OTA_CHECK ignored (no prod wrapper)");
#endif
  } else if (strcmp(type, "INPUT_MENU_SELECT") == 0) {
    // Menu item selected on LCD
    const char* menu_item = doc["menu_item"] | "";
    int menu_index = doc["menu_index"] | -1;
    const char* requested_mode = "";
    Serial.printf("[UART] Menu item selected: %s (index %d)\n", menu_item, menu_index);
    bool is_scan_menu_item = (strcmp(menu_item, "Dish") == 0 ||
                              strcmp(menu_item, "Discard") == 0 ||
                              strcmp(menu_item, "Check-in") == 0);
    if (strcmp(menu_item, "Dish") == 0) {
      requested_mode = "dish";
    } else if (strcmp(menu_item, "Discard") == 0) {
      requested_mode = "discard";
    } else if (strcmp(menu_item, "Check-in") == 0) {
      requested_mode = "check-in";
    }
    if (is_scan_menu_item && scan_request_pending_for_mode(requested_mode)) {
      Serial.printf("[SCAN] ignore menu select item=%s requested_mode=%s current_mode=%s state=%d upload=%d q=%lu result=%d user_state=%s device_state=%s\n",
                    menu_item,
                    requested_mode,
                    current_job.mode,
                    (int)current_job.state,
                    upload_inflight ? 1 : 0,
                    (unsigned long)upload_queue_count(),
                    waiting_for_mqtt_result ? 1 : 0,
                    sense_user_state_name(),
                    sense_device_state_name());
      uart_send_ui_toast("Capture already in progress");
      return true;
    }
    
    // Handle "Dish" selection - trigger SCAN operation (meal nutrition)
    if (strcmp(menu_item, "Dish") == 0) {
      Serial.println("[UART] Dish selected - queuing SCAN operation for meal nutrition");
      if (dish_upload_pending()) {
        Serial.printf("[DISH] ignore new selection active_job_id=%lu\n",
                      (unsigned long)active_dish_job_id);
        uart_send_ui_toast("Dish already in progress");
        return true;
      }
      if (op_queue != NULL) {
        OpJob job = {OP_SCAN, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, "", ""};
        strncpy(job.mode, "dish", sizeof(job.mode) - 1);
        job.expiry_date[0] = '\0';
        job.quantity = 1;
        job.add_to_shopping_list = false;
        if (enqueue_op_job(job, true, "menu_select_dish")) {
          active_dish_job_id = job.job_id;
          Serial.printf("[DISH] active job_id=%lu source=menu_select\n", (unsigned long)job.job_id);
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
        job.quantity = 1;
        job.add_to_shopping_list = false;
        if (enqueue_op_job(job, true, "menu_select_discard")) {
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
        job.quantity = 1;
        job.add_to_shopping_list = false;
        if (enqueue_op_job(job, true, "menu_select_checkin")) {
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
    int quantity = doc["quantity"] | 1;
    if (quantity < 1) {
      quantity = 1;
    }
    pending_quantity = (uint16_t)quantity;
    Serial.printf("[UART] Expiration date received: '%s' (len=%d) quantity=%d\n",
                  expiry_date, strlen(expiry_date), quantity);
    
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
        active_checkin_job->quantity = (uint16_t)quantity;
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
  } else if (strcmp(type, "INPUT_DISCARD_OPTIONS") == 0) {
    bool add_to_shopping_list = (doc["add_to_shopping_list"] | false);
    pending_discard_add_to_shopping_list = add_to_shopping_list;
    discard_choice_response_received = true;
    Serial.printf("[UART] Discard choice received add_to_shopping_list=%d\n",
                  add_to_shopping_list ? 1 : 0);
    if (active_discard_job != NULL && strcmp(active_discard_job->mode, "discard") == 0) {
      active_discard_job->add_to_shopping_list = add_to_shopping_list;
      Serial.printf("[UART] Updated active discard job add_to_shopping_list=%d\n",
                    add_to_shopping_list ? 1 : 0);
    }
  } else if (strcmp(type, "SYNC") == 0) {
    Serial.println("[LNK] sync_rx -> reset_partial");
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
      g_lcd_ota_attempted_this_window = true;
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
    bool active_match = g_lcd_ota_request_active && (req_id == 0 || req_id == g_lcd_ota_request_id);
    bool late_match = (!g_lcd_ota_request_active &&
                       req_id != 0 &&
                       g_lcd_ota_recent_request_id != 0 &&
                       req_id == g_lcd_ota_recent_request_id);
    if (active_match || late_match) {
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
      g_lcd_ota_recent_request_id = 0;
      g_lcd_ota_attempted_this_window = true;
      if (late_match) {
        Serial.printf("[UART] OTA_CHECK_RESULT late request_id=%lu result=%s err=%s version=%s\n",
                      (unsigned long)req_id,
                      g_lcd_ota_result,
                      err_code ? err_code : "",
                      g_lcd_ota_version);
      } else {
        Serial.printf("[UART] OTA_CHECK_RESULT request_id=%lu result=%s err=%s version=%s\n",
                      (unsigned long)req_id,
                      g_lcd_ota_result,
                      err_code ? err_code : "",
                      g_lcd_ota_version);
      }
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
  } else if (strcmp(type, "INPUT_RETRY") == 0) {
    Serial.println("[UART] INPUT_RETRY ignored (queue-based upload in use)");
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
      diag_record_action(op_type_name(job.type));
      
      // If this is a check-in job, set it as the active job and apply pending expiration date
      if (strcmp(job.mode, "check-in") == 0) {
        active_checkin_job = &current_job;
        active_discard_job = NULL;
        // Clear expiry date first to ensure we start fresh
        current_job.expiry_date[0] = '\0';
        current_job.quantity = (pending_quantity < 1) ? 1 : pending_quantity;
        pending_quantity = 1;
        current_job.add_to_shopping_list = false;
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
      } else if (strcmp(job.mode, "discard") == 0) {
        active_discard_job = &current_job;
        active_checkin_job = NULL;
        current_job.add_to_shopping_list = pending_discard_add_to_shopping_list;
        discard_choice_response_received = false;
      } else {
        active_checkin_job = NULL;
        active_discard_job = NULL;
        current_job.add_to_shopping_list = false;
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
        voice_peak_abs = 0;
        voice_sum_abs = 0;
        voice_sample_count = 0;
        voice_nonzero_sample_count = 0;
        
        // Send status: RECORDING
        uart_send_ui_status_extended("VOICE", "RECORDING", "Listening…");
        
        // Start I2S audio recording (max 30 seconds, but will stop on INPUT_LONG_PRESS_END)
        Serial.println("[OP_WORKER] VOICE: Starting I2S audio recording...");
        audio_start_recording_ms(30000);
        voice_begin_wifi_preconnect();
        if (voice_finalize_requested) {
          current_job.state = OP_FINALIZE;
        }
        
        // Wait for recording to complete (will be signaled by INPUT_LONG_PRESS_END)
        // For now, record for max 10 seconds or until signaled
        uint32_t record_start = millis();
        const uint32_t MAX_RECORD_MS = 10000;
        
        while (current_job.state == OP_RECORDING && (millis() - record_start) < MAX_RECORD_MS) {
          vTaskDelay(pdMS_TO_TICKS(20));
          if (wifi_connect_inflight) {
            wifi_guard_poll();
          }
          // Check if we should finalize (signaled by INPUT_LONG_PRESS_END)
          if (voice_finalize_requested) {
            current_job.state = OP_FINALIZE;
          }
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
          unsigned long record_ms = millis() - record_start;
          size_t sample_count = voice_sample_count;
          unsigned long avg_abs = sample_count > 0
                                    ? (unsigned long)(voice_sum_abs / sample_count)
                                    : 0UL;
          unsigned long nonzero_pct = sample_count > 0
                                        ? (unsigned long)((voice_nonzero_sample_count * 100UL) / sample_count)
                                        : 0UL;
          Serial.printf("[VOICE_DIAG] record_ms=%lu bytes=%u samples=%u peak_abs=%u avg_abs=%lu nonzero_pct=%lu\n",
                        record_ms,
                        (unsigned)voice_audio_size,
                        (unsigned)sample_count,
                        (unsigned)voice_peak_abs,
                        avg_abs,
                        nonzero_pct);
          
          // Transition to UPLOAD
          current_job.state = OP_UPLOAD;
          uart_send_ui_status_extended("VOICE", "UPLOADING", "Transcribing…");

          if (current_job.state == OP_UPLOAD) {
            bool used_psram = false;
            uint8_t* voice_job_buf = allocate_upload_buffer(voice_audio_size, &used_psram);
            if (voice_job_buf == NULL) {
              Serial.println("[OP_WORKER] VOICE: Failed to allocate background upload buffer");
              uart_send_ui_status_extended("VOICE", "ERROR", "Upload failed");
              current_job.state = OP_DONE;
            } else {
              memcpy(voice_job_buf, voice_audio_buffer, voice_audio_size);
              if (queue_voice_upload_job(current_job.job_id, voice_job_buf, voice_audio_size)) {
                Serial.printf("[OP_WORKER] VOICE: upload queued in background bytes=%u psram=%d\n",
                              (unsigned)voice_audio_size,
                              used_psram ? 1 : 0);
                current_job.state = OP_DONE;
              } else {
                Serial.println("[OP_WORKER] VOICE: Failed to queue background upload");
                free(voice_job_buf);
                uart_send_ui_status_extended("VOICE", "ERROR", "Upload failed");
                current_job.state = OP_DONE;
              }
            }
          }
        }
        
        Serial.println("[OP_WORKER] VOICE operation complete");
      } else if (job.type == OP_SCAN) {
        // SCAN state machine: IDLE -> CAPTURE -> PRESIGN -> UPLOAD -> WAIT_MQTT -> DONE
        Serial.printf("[OP_WORKER] Starting SCAN operation state machine (mode: %s queue_age_ms=%lu wake_age_ms=%lu)\n",
                      job.mode,
                      (unsigned long)(millis() - job.created_ts),
                      last_input_wake_ms > 0 ? (unsigned long)(millis() - last_input_wake_ms) : 0xFFFFFFFFUL);
        scan_ui_inflight_set(true, "scan_start");
        diag_record_action_event("scan", job.mode, "start", "begin", 0);
        diag_note_stage("scan_start", 0);
        bool is_dish_mode = scan_mode_is_dish(job.mode);
        if (is_dish_mode) {
          dish_scan_inflight_set(true, "scan_start");
        }
        flow_plan_print(job.job_id, job.mode);
        bool skip_upload = false;
        bool is_check_mode = (strcmp(job.mode, "check-in") == 0 ||
                              strcmp(job.mode, "check-out") == 0 ||
                              strcmp(job.mode, "check_out") == 0);
        bool dish_handed_off_to_upload = false;
        uint8_t* job_buf = NULL;
        size_t job_len = 0;
        UploadJob::CameraUploadMeta capture_meta = {};
        sensor_t* s_check = NULL;
        camera_fb_t* fb = nullptr;
        scan_terminal_reset();
        if (upload_queue_is_full()) {
          diag_record_error("queue_full", -1, "upload_queue_full");
          diag_record_action_event("scan", job.mode, "err", "queue_full", -1);
          scan_send_terminal_status("ERROR", "Device busy, try again", job.mode, true);
          scan_ui_inflight_set(false, "queue_full");
          if (is_dish_mode) {
            dish_scan_inflight_set(false, "queue_full");
          }
          current_job.state = OP_DONE;
          goto scan_exit;
        }
        
        // State: CAPTURE - Initialize camera and capture image
        current_job.state = OP_RECORDING;  // Reuse RECORDING state for capture phase
        capture_fill_led_set(true, "scan_capturing");
        scan_ui_status_emit("CAPTURING", "Capturing image…", job.mode, job.job_id, false);
        flow_step(job.job_id, "CAPTURING");
        
        // Ensure camera is OFF before starting
        s_check = esp_camera_sensor_get();
        if (s_check != NULL) {
          Serial.println("[OP_WORKER] SCAN: Camera already initialized - deinitializing first...");
          deinit_camera();
          delay(100);
        }
        
        // Capture now, enqueue for background presign/upload
        if (is_check_mode) {
          // CHECK-IN MODE: Capture image first
          Serial.println("[OP_WORKER] SCAN: Check-in mode - capturing image first...");
          camera_timeline_reset(job.mode);
          camera_timeline_event("ui_delay", (int32_t)CAMERA_UI_CAPTURE_DELAY_MS);
          scan_ui_status_emit("CAPTURING", "Capturing image…", job.mode, job.job_id, false);
          if (CAMERA_UI_CAPTURE_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_UI_CAPTURE_DELAY_MS));
          }
          // Initialize camera right before capture
          Serial.println("[OP_WORKER] SCAN: Initializing camera...");
          diag_note_stage("camera_init", 0);
          if (!init_camera()) {
            Serial.println("[OP_WORKER] SCAN: Camera initialization FAILED");
            deinit_camera();
            camera_timeline_complete(false, nullptr, "init_fail");
            diag_record_error("camera_init", -1, "init_failed");
            uart_send_sense_diag("camera", "scan_init_fail", job.mode, -1, "op_worker");
            diag_record_action_event("scan", job.mode, "err", "camera_init", -1);
            scan_send_terminal_status("ERROR", "Camera init failed", job.mode, true);
            current_job.state = OP_DONE;
          } else {
            diag_note_stage("camera_capture", 0);
            if (!warmup_and_capture(fb, true)) {
              Serial.println("[OP_WORKER] SCAN: Camera capture FAILED");
              deinit_camera();
              diag_record_error("camera_capture", -1, "capture_failed");
              uart_send_sense_diag("camera", "scan_capture_fail", job.mode, -1, "op_worker");
              diag_record_action_event("scan", job.mode, "err", "camera_capture", -1);
              scan_send_terminal_status("ERROR", "Camera failed", job.mode, true);
              current_job.state = OP_DONE;
            }
          }
          if (current_job.state == OP_DONE) {
            // fall through to error handling below
          } else {
            capture_camera_meta_snapshot(&capture_meta, fb);
            Serial.printf("[OP_WORKER] SCAN: Captured %u bytes (%dx%d) - copying for later upload\n",
                          fb->len, fb->width, fb->height);
            bool used_psram = false;
            job_buf = allocate_upload_buffer(fb->len, &used_psram);
            if (job_buf == NULL) {
              Serial.println("[OP_WORKER] SCAN: ERROR - Failed to allocate memory for image copy!");
              esp_camera_fb_return(fb);
              deinit_camera();
              diag_record_error("alloc", -1, "image_buf");
              diag_record_action_event("scan", job.mode, "err", "alloc", -1);
              scan_send_terminal_status("ERROR", "Memory failed", job.mode, true);
              current_job.state = OP_DONE;
            } else {
              Serial.printf("[OP_WORKER] SCAN: buffer alloc ok psram=%d len=%u\n",
                            used_psram ? 1 : 0,
                            (unsigned)fb->len);
              memcpy(job_buf, fb->buf, fb->len);
              job_len = fb->len;
              Serial.printf("[OP_WORKER] SCAN: Copied %u bytes to buffer at %p\n", (unsigned)job_len, job_buf);
              esp_camera_fb_return(fb);
              fb = nullptr;
              Serial.println("[CAM_PWR] capture complete");
              Serial.println("[OP_WORKER] SCAN: Powering down camera after capture...");
              deinit_camera();
            }
          }
              
          // Wait for any required user choice before enqueue (only if we successfully copied the image)
          if (job_buf != NULL && job_len > 0) {
            if (strcmp(job.mode, "check-in") == 0) {
              Serial.println("[OP_WORKER] SCAN: Check-in mode - waiting for expiry date...");
              scan_ui_status_emit("WAITING_INPUT", "Waiting for expiry date...", job.mode, job.job_id, false);
              flow_step(job.job_id, "WAITING_INPUT");

              unsigned long wait_start = millis();
              const unsigned long MAX_EXPIRY_WAIT_MS = 30000;
              expiry_date_response_received = false;

              Serial.println("[OP_WORKER] SCAN: Waiting for expiry date (checking every 100ms)...");
              while (!expiry_date_response_received && (millis() - wait_start) < MAX_EXPIRY_WAIT_MS) {
                if (expiry_date_response_received) {
                  if (active_checkin_job != NULL) {
                    strncpy(current_job.expiry_date, active_checkin_job->expiry_date, sizeof(current_job.expiry_date) - 1);
                    current_job.expiry_date[sizeof(current_job.expiry_date) - 1] = '\0';
                    current_job.quantity = active_checkin_job->quantity > 0 ? active_checkin_job->quantity : 1;
                    if (current_job.expiry_date[0] != '\0') {
                      Serial.printf("[OP_WORKER] SCAN: Expiry date received: %s\n", current_job.expiry_date);
                    } else {
                      Serial.println("[OP_WORKER] SCAN: Empty expiry date received (user skipped) - proceeding immediately");
                    }
                  }
                  break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
              }

              if (!expiry_date_response_received) {
                Serial.println("[OP_WORKER] SCAN: Check-in mode - expiry date timeout, proceeding without it");
              } else if (current_job.expiry_date[0] == '\0') {
                Serial.println("[OP_WORKER] SCAN: Check-in mode - empty expiry date received, proceeding without it");
              }
            }
          }
          
          // UI completes here for check-in/out; background upload continues
          if (current_job.state == OP_DONE || job_buf == NULL || job_len == 0) {
            scan_ui_inflight_set(false, "scan_error");
            skip_upload = true;
          } else {
            scan_ui_status_emit("DONE", "Logged!", job.mode, job.job_id, false);
            flow_step(job.job_id, "DONE_UI");
            scan_ui_inflight_set(false, "ui_done");
            size_t psram_free = 0;
#if (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM)
            psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
            Serial.printf("[UPLOAD_ENQUEUE] mode=%s job_id=%lu len=%u heap=%u psram=%u q=%lu\n",
                          job.mode,
                          (unsigned long)job.job_id,
                          (unsigned)job_len,
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)psram_free,
                          (unsigned long)upload_queue_count());
            if (!queue_upload_job(job.job_id, job.mode, current_job.expiry_date, current_job.quantity, current_job.add_to_shopping_list, &capture_meta, job_buf, job_len, 0, false, 0)) {
              Serial.println("[UPLOAD] queue failed");
              presign_set_error_text("Upload queue failed");
              diag_record_action_event("scan", job.mode, "err", "upload_enqueue", -1);
              scan_send_terminal_status("ERROR", "Device busy, try again", job.mode, true);
              free(job_buf);
            } else {
              flow_step(job.job_id, "UPLOAD_ENQUEUED");
              diag_record_action_event("scan", job.mode, "queued", "upload_enqueue", 0);
            }
            current_job.state = OP_DONE;
            skip_upload = true;
          }
        } else {
          // DISH/DISCARD: capture, then enqueue for background upload/result
          Serial.printf("[OP_WORKER] SCAN: %s mode - capturing image...\n", job.mode);
          camera_timeline_reset(job.mode);
          camera_timeline_event("ui_delay", (int32_t)CAMERA_UI_CAPTURE_DELAY_MS);
          scan_ui_status_emit("CAPTURING", "Capturing image…", job.mode, job.job_id, false);
          if (CAMERA_UI_CAPTURE_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_UI_CAPTURE_DELAY_MS));
          }
          Serial.println("[OP_WORKER] SCAN: Initializing camera...");
          diag_note_stage("camera_init", 0);
          if (!init_camera()) {
            Serial.println("[OP_WORKER] SCAN: Camera initialization FAILED");
            deinit_camera();
            camera_timeline_complete(false, nullptr, "init_fail");
            diag_record_error("camera_init", -1, "init_failed");
            uart_send_sense_diag("camera", "scan_init_fail", job.mode, -1, "op_worker");
            diag_record_action_event("scan", job.mode, "err", "camera_init", -1);
            scan_send_terminal_status("ERROR", "Camera init failed", job.mode, true);
            current_job.state = OP_DONE;
          } else {
            diag_note_stage("camera_capture", 0);
            if (!warmup_and_capture(fb, true)) {
              Serial.println("[OP_WORKER] SCAN: Camera capture FAILED");
              deinit_camera();
              diag_record_error("camera_capture", -1, "capture_failed");
              uart_send_sense_diag("camera", "scan_capture_fail", job.mode, -1, "op_worker");
              diag_record_action_event("scan", job.mode, "err", "camera_capture", -1);
              scan_send_terminal_status("ERROR", "Camera failed", job.mode, true);
              current_job.state = OP_DONE;
            } else {
              capture_camera_meta_snapshot(&capture_meta, fb);
              Serial.printf("[OP_WORKER] SCAN: Captured %u bytes (%dx%d)\n", fb->len, fb->width, fb->height);
              bool used_psram = false;
              job_buf = allocate_upload_buffer(fb->len, &used_psram);
              if (job_buf == NULL) {
                Serial.println("[OP_WORKER] SCAN: ERROR - Failed to allocate memory for image copy!");
                esp_camera_fb_return(fb);
                deinit_camera();
                diag_record_error("alloc", -1, "image_buf");
                diag_record_action_event("scan", job.mode, "err", "alloc", -1);
                scan_send_terminal_status("ERROR", "Memory failed", job.mode, true);
                current_job.state = OP_DONE;
              } else {
                Serial.printf("[OP_WORKER] SCAN: buffer alloc ok psram=%d len=%u\n",
                              used_psram ? 1 : 0,
                              (unsigned)fb->len);
                memcpy(job_buf, fb->buf, fb->len);
                job_len = fb->len;
                Serial.printf("[OP_WORKER] SCAN: Copied %u bytes to buffer at %p\n", (unsigned)job_len, job_buf);
                esp_camera_fb_return(fb);
                fb = nullptr;
                Serial.println("[CAM_PWR] capture complete");
                Serial.println("[OP_WORKER] SCAN: Powering down camera after capture...");
                deinit_camera();
              }
            }
          }

          if (current_job.state != OP_DONE && job_buf != NULL && job_len > 0) {
            if (strcmp(job.mode, "discard") == 0) {
              Serial.println("[OP_WORKER] SCAN: Discard mode - waiting for add-to-shopping-list choice...");
              scan_ui_status_emit("WAITING_INPUT", "Add to shopping list?", job.mode, job.job_id, false);
              flow_step(job.job_id, "WAITING_INPUT");

              unsigned long wait_start = millis();
              const unsigned long MAX_DISCARD_WAIT_MS = 30000;
              discard_choice_response_received = false;
              current_job.add_to_shopping_list = false;

              while (!discard_choice_response_received && (millis() - wait_start) < MAX_DISCARD_WAIT_MS) {
                if (discard_choice_response_received) {
                  if (active_discard_job != NULL) {
                    current_job.add_to_shopping_list = active_discard_job->add_to_shopping_list;
                  }
                  break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
              }

              if (!discard_choice_response_received) {
                Serial.println("[OP_WORKER] SCAN: Discard choice timeout - proceeding without shopping-list add");
              } else {
                Serial.printf("[OP_WORKER] SCAN: Discard choice received add_to_shopping_list=%d\n",
                              current_job.add_to_shopping_list ? 1 : 0);
              }

              scan_ui_status_emit("DONE", "Logged!", job.mode, job.job_id, false);
              flow_step(job.job_id, "DONE_UI");
            } else if (is_dish_mode) {
              scan_ui_status_emit("DONE", "Logged!", job.mode, job.job_id, false);
              flow_step(job.job_id, "DONE_UI");
            }
            scan_ui_inflight_set(false, "ui_done");
            size_t psram_free = 0;
#if (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM)
            psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
            Serial.printf("[UPLOAD_ENQUEUE] mode=%s job_id=%lu len=%u heap=%u psram=%u q=%lu\n",
                          job.mode,
                          (unsigned long)job.job_id,
                          (unsigned)job_len,
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)psram_free,
                          (unsigned long)upload_queue_count());
            if (!queue_upload_job(job.job_id,
                                  job.mode,
                                  current_job.expiry_date[0] ? current_job.expiry_date : NULL,
                                  current_job.quantity,
                                  current_job.add_to_shopping_list,
                                  &capture_meta,
                                  job_buf,
                                  job_len,
                                  0,
                                  false,
                                  0)) {
              Serial.println("[UPLOAD] queue failed");
              diag_record_error("queue", -1, "upload_enqueue");
              diag_record_action_event("scan", job.mode, "err", "upload_enqueue", -1);
              scan_send_terminal_status("ERROR", "Device busy, try again", job.mode, true);
              free(job_buf);
            } else {
              if (is_dish_mode) {
                dish_handed_off_to_upload = true;
              }
              flow_step(job.job_id, "UPLOAD_ENQUEUED");
              diag_record_action_event("scan", job.mode, "queued", "upload_enqueue", 0);
            }
            current_job.state = OP_DONE;
            skip_upload = true;
          }
        }
          
        if (current_job.state == OP_DONE) {
          skip_upload = true;
        }
        if (skip_upload) {
          Serial.println("[OP_WORKER] SCAN: Upload deferred to background");
        }
scan_exit:
        if (is_dish_mode && !dish_handed_off_to_upload) {
          clear_active_dish_job(job.job_id, "scan_exit_no_upload");
        }
        scan_ui_inflight_set(false, "scan_complete");
        if (is_dish_mode) {
          dish_scan_inflight_set(false, "scan_complete");
        }
        // Sense_Minimal/Sense_Minimal.ino: op_worker_task (SCAN exit)
        Serial.println("[OP_WORKER] SCAN operation complete");
      } else if (job.type == OP_LIST_REFRESH) {
        Serial.println("[OP_WORKER] LIST_REFRESH ignored (shopping list disabled)");
        list_refresh_mark_complete("disabled");
      }
      
      // Job complete
      current_job.active = false;
      if (job.pri == PRI_USER) {
        foreground_active = false;
      }
      
      // Clear active scan job pointers and transient input state
      if (strcmp(current_job.mode, "check-in") == 0) {
        active_checkin_job = NULL;
        current_job.expiry_date[0] = '\0';  // Clear expiry date for next job
        current_job.quantity = 1;
      } else if (strcmp(current_job.mode, "discard") == 0) {
        active_discard_job = NULL;
        current_job.add_to_shopping_list = false;
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ── Sleep/Wake Functions → sense_sleep.h ────────────────────────────
#include "sense_sleep.h"


void setup() {
  Serial.printf("[BOOT_FLOW] stage=setup_enter t=%lu heap=%u min_heap=%u\n",
                millis(),
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
  print_wakeup_diagnostics(HALO_BOARD_NAME);
#ifdef HALO_SENSE_PROD_WRAPPER
  halo_prod_pre_setup();
#endif
  Serial.printf("[BOOT_FLOW] stage=after_pre_setup t=%lu heap=%u min_heap=%u\n",
                millis(),
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
  initUarts();
  camera_pwdn_gpio_init();
  g_sense_boot_count++;
  last_wake_ms = millis();
  guardian_awake_start_ms = last_wake_ms;
  guardian_force_sleep = false;
  
  // Check wake-up cause
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  esp_reset_reason_t reset_reason = esp_reset_reason();
  g_boot_reset_reason = reset_reason;
  int wake_gpio = gpio_get_level(WAKE_GPIO);
  int uart_rx = gpio_get_level((gpio_num_t)UART_RX_PIN);
  g_prev_clean_shutdown = g_rtc_clean_shutdown;
  g_rtc_clean_shutdown = 0;
  diag_record_crash_boot(reset_reason, cause);
  bool deep_sleep_reset = (reset_reason == ESP_RST_DEEPSLEEP);
  bool timer_override = deep_sleep_reset && g_timer_wake_armed &&
                        (cause == ESP_SLEEP_WAKEUP_UNDEFINED || cause == ESP_SLEEP_WAKEUP_EXT0);
  Serial.printf("[BOOT_DIAG] reset_reason=%d wake_cause=%d gpio2=%d uart_rx=%d\n",
                (int)reset_reason, (int)cause, wake_gpio, uart_rx);
  print_wake_cause(cause);
  if (reset_reason == ESP_RST_BROWNOUT ||
      reset_reason == ESP_RST_TASK_WDT ||
      reset_reason == ESP_RST_INT_WDT) {
    Serial.printf("[BOOT_DIAG] WARNING reset_reason=%d (brownout/wdt)\n", (int)reset_reason);
  }
  if (reset_reason == ESP_RST_PANIC ||
      reset_reason == ESP_RST_TASK_WDT ||
      reset_reason == ESP_RST_INT_WDT ||
      reset_reason == ESP_RST_WDT) {
    g_upload_persist_replay_not_before_ms = millis() + UPLOAD_PERSIST_VOICE_REPLAY_BACKOFF_MS;
    Serial.printf("[UPLOAD_PERSIST] panic_backoff until_ms=%lu reset_reason=%s\n",
                  g_upload_persist_replay_not_before_ms,
                  reset_reason_label(reset_reason));
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
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER || timer_override) {
    if (timer_override) {
      Serial.printf("[BOOT_DIAG] timer_wake_override cause=%d armed=%u\n",
                    (int)cause, (unsigned)g_timer_wake_armed);
    }
    ota_on_timer_wake();
  } else {
    Serial.println("[SENSE] Cold boot");
  }
  g_timer_wake_armed = 0;
  
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
  op_queue = xQueueCreate(OP_QUEUE_MAX, sizeof(OpJob));
  ui_event_queue = xQueueCreate(20, sizeof(UiEvent));
  upload_queue = xQueueCreate(UPLOAD_QUEUE_MAX, sizeof(UploadJob));
  upload_queue_dish = xQueueCreate(UPLOAD_QUEUE_DISH_MAX, sizeof(UploadJob));
  if (op_queue == NULL || ui_event_queue == NULL) {
    Serial.println("ERROR: Failed to create operation queues!");
  } else {
    Serial.println("[SETUP] Operation queues created");
  }
  if (upload_queue == NULL || upload_queue_dish == NULL) {
    Serial.println("ERROR: Failed to create upload queues!");
  }
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
  upload_persist_setup();
#endif
  http_mutex = xSemaphoreCreateMutex();
  if (http_mutex == NULL) {
    Serial.println("ERROR: Failed to create http_mutex!");
  }
  wifi_connect_mutex = xSemaphoreCreateMutex();
  if (wifi_connect_mutex == NULL) {
    Serial.println("ERROR: Failed to create wifi_connect_mutex!");
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

  // Create background upload worker task (lower priority)
  xTaskCreatePinnedToCore(upload_worker_task, "upload_worker", 12288, NULL, 1, &upload_worker_task_handle, 1);
  if (upload_worker_task_handle == NULL) {
    Serial.println("ERROR: Failed to create upload_worker_task!");
  } else {
    Serial.println("[SETUP] upload_worker_task created with handle");
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
    Serial.printf("[BOOT_FLOW] stage=awake_tx_begin t=%lu\n", millis());
    lcdSerial.println("AWAKE");
    lcdSerial.flush();
    Serial.printf("[BOOT_FLOW] stage=awake_tx_done t=%lu\n", millis());
  }

  // Register Wi-Fi event handler for connect guard state
  Serial.printf("[BOOT_FLOW] stage=wifi_event_register_begin t=%lu\n", millis());
  WiFi.onEvent(handle_wifi_event);
  Serial.printf("[BOOT_FLOW] stage=wifi_event_register_done t=%lu\n", millis());
  
  // Defer boot-time Wi-Fi connect so UART/user actions can be serviced first.
  boot_wifi_connect_pending = true;
  boot_wifi_connect_earliest_ms = millis() + 1500;
  Serial.printf("[BOOT_FLOW] stage=wifi_connect_deferred t=%lu earliest_t=%lu status=%d inflight=%d state=%d heap=%u min_heap=%u\n",
                millis(),
                boot_wifi_connect_earliest_ms,
                (int)WiFi.status(),
                wifi_connect_inflight ? 1 : 0,
                (int)wifi_state,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
  
  // Initialize MQTT (will connect when needed)
  Serial.println("[SETUP] MQTT system initialized (will connect on demand)");
  
  Serial.println("\nReady. Sense board initialized.");
  Serial.println("UART communication with LCD board active.");

#ifdef HALO_SENSE_PROD_WRAPPER
  Serial.printf("[BOOT_FLOW] stage=halo_prod_setup_begin t=%lu\n", millis());
  halo_prod_setup();
  Serial.printf("[BOOT_FLOW] stage=halo_prod_setup_done t=%lu\n", millis());
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

  // DEBUG: USB serial command injection — allows injecting UART JSON messages
  // via USB CDC (/dev/cu.usbmodem*) so they are processed as if sent by the LCD.
  {
    static char usb_rx_line[UART_RX_FRAME_MAX + 1];
    static size_t usb_rx_len = 0;
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      if (c == '\n') {
        usb_rx_line[usb_rx_len] = '\0';
        // Trim trailing \r if present
        if (usb_rx_len > 0 && usb_rx_line[usb_rx_len - 1] == '\r') {
          usb_rx_line[--usb_rx_len] = '\0';
        }
        if (usb_rx_len > 0 && usb_rx_line[0] == '{') {
          Serial.printf("[DEBUG_INJECT] Processing USB serial command: %.40s...\n", usb_rx_line);
          parse_input_message(usb_rx_line);
        }
        usb_rx_len = 0;
      } else if (usb_rx_len < UART_RX_FRAME_MAX) {
        usb_rx_line[usb_rx_len++] = c;
      }
      // else: overflow — silently discard until next newline
    }
  }

  service_boot_wifi_connect(millis());

  if (wifi_connect_inflight) {
    wifi_guard_poll();
  }
  if (wifi_is_connected() && wifi_state != WIFI_STATE_CONNECTED) {
    wifi_guard_set_inflight(false);
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "loop_status_connected", WL_CONNECTED);
  }
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
  upload_persist_maybe_replay();
#endif

  if (!guardian_force_sleep && GUARDIAN_FORCE_SLEEP_MS > 0) {
    unsigned long now_ms = millis();
    unsigned long awake_ms = now_ms - guardian_awake_start_ms;
    if (awake_ms >= GUARDIAN_FORCE_SLEEP_MS) {
      guardian_force_sleep = true;
      Serial.printf("[GUARDIAN] force_sleep elapsed_ms=%lu\n", awake_ms);
      sense_enter_sleep(SENSE_SLEEP_DEEP_IDLE);
      return;
    }
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
    refresh_requested = false;
    Serial.println("[LIST_REFRESH] ignored (shopping list disabled)");
    list_refresh_mark_complete("disabled");
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
    
    // Delete item from AWS. Shopping list refresh is disabled.
    if (WiFi.status() == WL_CONNECTED) {
      delete_item_from_api(delete_item_id);
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
  if (wake_pulse_seen) {
    wake_pulse_seen = false;
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
      Serial.println("[SLEEP] sleep_requested timeout -> deny");
      sleep_send_deny_and_clear("timeout", now_ms);
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
    pre_sleep_block_start_ms = 0;
#endif
#ifdef HALO_SENSE_PROD_WRAPPER
    if (halo_provisioning_active()) {
      Serial.println("[SENSE] coordinated deny reason=provisioning_active");
      sleep_send_deny_and_clear("op_inflight", now_ms);
      goto loop_end;
    }
#endif

    bool coordinated_request = sleep_coord_requested;
    sleep_ready_reason = coordinated_request ? "coordinated" : "requested";
    sleep_coord_pending_for_ready = coordinated_request;
    sleep_requested = false;  // Clear flag only when we can proceed
    sleep_request_ms = 0;
    sleep_request_logged = false;
    sleep_ack_defer_logged = false;
    sleep_ready_sent_for_cycle = false;
    sleep_coord_requested = false;
    sleep_sm_transition(SLEEP_SM_IDLE, "CLEAR", sleep_sm_msg_id);
    sleep_sm_msg_id = 0;
    
    // Sense_Minimal/Sense_Minimal.ino: loop (sleep block check)
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
    } else if (upload_inflight && scan_ui_inflight) {
      block_reason = "net_upload";
      block_op = "upload";
    } else if (current_job.state != OP_IDLE && current_job.state != OP_DONE) {
      block_reason = "op_inflight";
      block_op = op_type_name(current_job.type);
    }
    if (can_sleep) {
      const char* net_reason = NULL;
      if (!sleep_allowed_now("coord_request", &net_reason)) {
        block_reason = net_reason ? net_reason : "net_inflight";
        block_op = "net";
        can_sleep = false;
      }
    }
    if (block_reason) {
      if (sleep_reason_is_background_deferable(block_reason)) {
        if (sleep_background_force_ready(now_ms, block_reason, "coord_request")) {
          block_reason = NULL;
          block_op = "background";
          can_sleep = true;
        } else {
          sleep_requested = true;
          sleep_coord_requested = true;
          sleep_request_ms = now_ms;
          last_lcd_communication = now_ms;
          goto loop_end;
        }
      }
    }
    if (block_reason) {
      if (sleep_block_should_log(block_reason, "coord_request", block_op)) {
        Serial.printf("[SLEEP_BLOCK] reason=%s op=%s\n", block_reason, block_op);
      }
      can_sleep = false;
    }
    
    if (can_sleep) {
      sleep_background_force_reset();
      wake_pin_configure_rtc_input_inactive_pull();
      int wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      if (wake_pin_is_active_level(wake_pin_level)) {
        Serial.printf("[SLEEP] coordinated deny reason=wake_pin_active level=%d\n",
                      wake_pin_level);
        sleep_send_deny_and_clear("wake_pin_active", now_ms);
        goto loop_end;
      }
      if (!sleep_wait_wake_pin_deassert(WAKE_PIN_STABLE_WAIT_MS)) {
        Serial.println("[SLEEP] coordinated deny reason=wake_pin_active (unstable)");
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
      if (coordinated_request) {
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
        if (sleep_reason_is_background_deferable(block_reason)) {
          if (sleep_background_force_ready(now_ms, block_reason, "coord_inactivity")) {
            block_reason = NULL;
            block_op = "background";
            can_sleep = true;
          } else {
            last_lcd_communication = now_ms;
            goto loop_end;
          }
        }
      }
      if (block_reason) {
        if (sleep_block_should_log(block_reason, "coord_inactivity", block_op)) {
          Serial.printf("[SLEEP_BLOCK] reason=%s op=%s\n", block_reason, block_op);
        }
        can_sleep = false;
        // Reset timer to check again later
        if (lcd_timeout > 10000) {
          last_lcd_communication = millis() - (lcd_timeout - 10000);  // Check again in 10 seconds
        }
      }
      if (can_sleep) {
        const char* net_reason = NULL;
        if (!sleep_allowed_now("coord_inactivity", &net_reason)) {
          if (sleep_reason_is_background_deferable(net_reason)) {
            if (sleep_background_force_ready(now_ms, net_reason, "coord_inactivity")) {
              can_sleep = true;
            } else {
              last_lcd_communication = now_ms;
              goto loop_end;
            }
          } else {
            can_sleep = false;
          }
        }
      }
      
      if (can_sleep) {
        sleep_background_force_reset();
        bool allow_lcd_fallback_sleep = !link_synced || rx_age > 30000UL;
        if (!allow_lcd_fallback_sleep) {
          if (sleep_intent_cooldown_until_ms > now_ms) {
            if ((now_ms - last_sleep_coord_log_ms) > 2000) {
              unsigned long remaining_ms = sleep_intent_cooldown_until_ms - now_ms;
              Serial.printf("[SLEEP_INTENT] cooldown active remaining_ms=%lu\n", remaining_ms);
              last_sleep_coord_log_ms = now_ms;
            }
            last_lcd_communication = now_ms;
            goto loop_end;
          }
          if (!sleep_intent_pending) {
            uart_send_sleep_intent("inactivity");
            sleep_intent_pending = true;
            sleep_intent_sent_ms = now_ms;
            Serial.println("[SLEEP_INTENT] sent reason=inactivity");
            last_lcd_communication = now_ms;
            goto loop_end;
          }
          if ((now_ms - sleep_intent_sent_ms) < SENSE_SLEEP_INTENT_TIMEOUT_MS) {
            if ((now_ms - last_sleep_coord_log_ms) > 2000) {
              unsigned long wait_ms = SENSE_SLEEP_INTENT_TIMEOUT_MS - (now_ms - sleep_intent_sent_ms);
              Serial.printf("[SLEEP_INTENT] awaiting_lcd_response wait_ms=%lu\n", wait_ms);
              last_sleep_coord_log_ms = now_ms;
            }
            last_lcd_communication = now_ms;
            goto loop_end;
          }
        }
        sleep_intent_pending = false;
        Serial.printf("[SLEEP_COORD] allow_sleep reason=%s link_recent=%d rx_age=%lu hb_age=%lu synced=%d\n",
                      allow_lcd_fallback_sleep ? "stale_link_fallback" : "inactivity",
                      link_recent ? 1 : 0,
                      rx_age,
                      hb_age,
                      link_synced ? 1 : 0);
        Serial.printf("[SENSE] Idle timeout (%lu s) - entering sleep (lcd_optional=1)\n",
                      time_since_last_comm / 1000);
        sleep_ready_reason = "inactivity";
        sleep_coord_pending_for_ready = false;
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



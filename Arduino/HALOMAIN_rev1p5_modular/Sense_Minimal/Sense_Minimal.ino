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
#include <atomic>
#include <HardwareSerial.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_sleep.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_mac.h>
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

#ifdef HALO_SENSE_PROD_WRAPPER
// Provisioning integration (Sense prod wrapper)
#include "../halo_ota_demo/firmware/shared/ProvisioningState.h"
#include "../halo_ota_demo/firmware/shared/OtaIntent.h"
bool halo_provisioning_active();
extern "C" bool halo_provisioning_claim_worker_poll();
bool halo_get_provisioned_wifi(char* ssid, size_t ssid_sz, char* pass, size_t pass_sz);
void halo_prod_pre_setup();
void halo_prod_setup();
void halo_prod_loop();
void halo_prod_pre_sleep();
void halo_prod_reset_wifi();
void halo_prod_provision_diag();
void halo_prod_provision_restart_ap();
void halo_prod_provision_crypto_diag();
bool halo_prod_should_delay_sleep();
bool halo_prod_boot_ota_pending();
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
                                uint32_t grace_after_sec,
                                const char* coord_id, uint32_t peer_boot_id);
bool halo_prod_should_defer_sleep_ack(bool* ota_busy, bool* mqtt_busy, bool* time_invalid, bool* ota_check_busy);
void halo_prod_request_manual_ota(const char* reason);
void halo_prod_note_lcd_timer(const char* schedule_id, uint32_t boot_id, int wake, int reset, uint32_t epoch, bool resumed);
void halo_prod_request_maint_test(uint32_t duration_sec);
// True if any LCD/Sense OTA activity is in progress. Used to suppress
// non-OTA HTTP traffic (e.g. list refresh) that would starve the single
// net stack and stall the LCD OTA S3 download. Defined in the prod wrapper.
bool lcd_ota_in_progress();
#endif

#ifndef HALO_SENSE_PROD_WRAPPER
// Non-wrapper (dev) builds have no OTA orchestration; never suppress.
__attribute__((weak)) bool lcd_ota_in_progress() { return false; }
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
#define UART_RX_FRAME_MAX 1024

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
RTC_DATA_ATTR static uint8_t g_timer_wake_armed = 0;   // deep sleep only — .rtc.data is fine

// Crash forensics: RTC_NOINIT_ATTR, *not* RTC_DATA_ATTR (fixed 2026-08-20).
//
// These exist to describe a crash, and a crash is a full reset. `.rtc.data` is
// INITIALISED data — the startup code restores it from the image on every reset —
// so every one of these was wiped by the panic reboot before the code that reads
// them ever ran. Concretely, diag_record_crash_boot() runs on the boot AFTER the
// crash and copies g_rtc_last_stage into g_rtc_last_crash_stage; that breadcrumb
// was already back to "" by then, and g_rtc_crash_count++ started from 0 every
// time, so it could never exceed 1. The diagnostics were destroyed by the exact
// event they were written to explain.
//
// Proven empirically on the sibling case: g_cam_wedge_restarts had the same
// attribute and reported code=1 on eight consecutive self-heal restarts, never 2.
//
// `.rtc_noinit` is not initialised at all, so it carries across a reset. It holds
// garbage after a power cycle, hence the magic below.
#define RTC_DIAG_MAGIC 0x44494147u   // 'DIAG'
RTC_NOINIT_ATTR static uint32_t g_rtc_diag_magic;
RTC_NOINIT_ATTR static uint8_t  g_rtc_clean_shutdown;
RTC_NOINIT_ATTR static char     g_rtc_last_stage[24];
RTC_NOINIT_ATTR static int32_t  g_rtc_last_stage_code;
RTC_NOINIT_ATTR static uint32_t g_rtc_last_stage_uptime_ms;
RTC_NOINIT_ATTR static uint32_t g_rtc_crash_count;
RTC_NOINIT_ATTR static uint32_t g_rtc_last_crash_reason;
RTC_NOINIT_ATTR static uint32_t g_rtc_last_crash_wake_cause;
RTC_NOINIT_ATTR static char     g_rtc_last_crash_stage[24];
RTC_NOINIT_ATTR static int32_t  g_rtc_last_crash_stage_code;
RTC_NOINIT_ATTR static uint32_t g_rtc_last_crash_stage_uptime_ms;

// Must run before anything reads or writes the above. Power-on leaves
// `.rtc_noinit` as garbage; the magic is what separates "fresh device" from
// "rebooted after a crash, breadcrumbs intact".
static void rtc_diag_init() {
  if (g_rtc_diag_magic == RTC_DIAG_MAGIC) {
    return;   // carried across a reset — this is the case that was broken
  }
  g_rtc_diag_magic = RTC_DIAG_MAGIC;
  g_rtc_clean_shutdown = 0;
  g_rtc_last_stage[0] = '\0';
  g_rtc_last_stage_code = 0;
  g_rtc_last_stage_uptime_ms = 0;
  g_rtc_crash_count = 0;
  g_rtc_last_crash_reason = 0;
  g_rtc_last_crash_wake_cause = 0;
  g_rtc_last_crash_stage[0] = '\0';
  g_rtc_last_crash_stage_code = 0;
  g_rtc_last_crash_stage_uptime_ms = 0;
}
static const unsigned long WAKE_PIN_MITIGATION_MS = 20;
static const uint32_t WAKE_PIN_FAILSAFE_TIMER_S = 15;
static const uint32_t SENSE_MAX_SLEEP_TIMER_S = 0;  // Disabled — GPIO39 is now clean
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
static const unsigned long MIN_AWAKE_BEFORE_SLEEP_MS = 2000;  // Brief grace period (MQTT disabled)
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
static int JPEG_QUALITY = 5;  // Max text detail: minimal compression for label readability
static int CAMERA_XCLK_HZ = 20000000;  // 20 MHz (max OV2640 clock)
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
static std::atomic<uint8_t*> g_camera_dma_reserve{nullptr};
static portMUX_TYPE g_camera_dma_reserve_mux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> g_camera_dma_provisioning{false};
// While true, nothing may re-bank the 16KB camera reserve.
//
// The OTA path frees it deliberately to make room for the download, but other
// components restore it behind OTA's back -- the provisioning TLS RAII guard
// re-acquires on every scope exit, and it runs during an OTA check. Measured
// result: the applier's own heap gate rejected the update EVERY time,
//   [OTA_WRITE][ERROR] Insufficient heap for OTA: free_heap=37800, required>=40960
// which is only ~3KB short of the 16KB that had just been handed back. The Sense
// could update the LCD but could never update ITSELF -- a device in a home would
// have been permanently un-updatable.
static std::atomic<bool> g_dma_reserve_suppressed{false};
static const uint32_t CAMERA_UI_CAPTURE_DELAY_MS = 0;
static const uint32_t CAMERA_PREFLIGHT_SETTLE_MS = 40;
static const uint8_t CAMERA_PREFLIGHT_WARMUP_FRAMES = 2;
static const uint32_t CAMERA_PREFLIGHT_WARMUP_DELAY_MS = 80;
static const uint32_t CAMERA_PREFLIGHT_BUDGET_MS = 1000;
static const uint32_t CAMERA_INIT_SETTLE_DELAY_MS = 50;
static const uint8_t CAMERA_INIT_WARMUP_FRAMES = 3;
static const uint32_t CAMERA_INIT_WARMUP_DELAY_MS = 30;
// How long init_camera() waits for an in-flight HTTP request to finish before
// it touches WiFi. Tearing the interface down under a live socket frees lwIP's
// pbufs while the upload task is blocked in recv(), which panics the board
// (InstrFetchProhibited in esp_pbuf_free). 1.5s covers a normal request tail
// without making the user wait; past that we skip the teardown rather than
// crash. See init_camera() in sense_camera.h.
// Reverted to 1500 after measurement (2026-08-21). Raising it to 5000 was based
// on "a parked upload is holding the camera's DMA block" — which is FALSE:
// across a 12-capture soak the drain succeeded 5/5 ("HTTP drained — safe to free
// DMA"), WiFi was torn down, and dma_largest was STILL 15860 against a 16384
// need. The upload is not the holder, so a longer window buys nothing.
static const uint32_t CAMERA_HTTP_DRAIN_MAX_MS = 1500;
static const size_t CAMERA_DMA_LARGEST_BLOCK_MIN_BYTES = 24 * 1024;
static const size_t CAMERA_DMA_RESERVE_BYTES = 16384;
// How long to wait for contiguous DMA to come back before retrying camera init.
// The immediate retry was measured to be useless: dma_largest was IDENTICAL
// (15860) before and after the WiFi teardown, and 15860 < 16384 is precisely why
// init failed. The memory DOES return (steady state 17396) once the parked
// background upload finishes — it just is not back yet at retry time.
// 500ms, not 3000: a 3s wait was measured 0-for-3 (largest stayed 15348/15860
// against a 16384 need for the full 3 seconds). The block is held by a background
// upload that runs for ~30s, so waiting is not going to win — but a short check
// costs little and the log line it emits is the diagnostic that matters.
static const uint32_t CAMERA_DMA_RECOVER_MAX_MS = 500;

// Defer the post-capture camera deinit to the START of the next capture.
//
// MEASURED EFFECT: camera init failures 20-27 per soak -> 0. Same harness, same
// modes, 10 captures across 2 boots (i.e. genuinely multiple captures per boot,
// which is the case that used to fail ~60% of the time).
//
// WHY IT WORKS — and it is NOT the reason this flag was first written. The op
// worker already deinits at the start of a capture
// ("SCAN: Camera already initialized - deinitializing first"), so skipping the
// post-capture deinit does not keep the camera alive; it MOVES the teardown to
// immediately before the next init. That is the whole fix: the camera's 16KB DMA
// block is now freed and re-claimed back to back, with nothing in between. It used
// to be freed after capture N, then the upload's TLS handshake fragmented the
// region, and capture N+1 had to find a fresh contiguous 16KB in a heap that no
// longer had one (~40KB free, largest 15,860).
//
// The reuse path in init_camera() therefore never fires today. It is kept because
// it is correct if that pre-deinit is ever removed — but do not assume it runs.
//
// Root cause it addresses (measured 2026-08-21): the camera needs ONE contiguous
// 16,384-byte internal DMA block, and the first TLS handshake of a boot leaves
// the region permanently fragmented (~40KB free, largest 15,860). So the FIRST
// capture of a boot always worked (6/6) and later ones in the same session failed
// ~60% — because each capture tore the camera down and the next had to re-acquire
// that block out of a heap TLS had already carved up.
//
// Deinit-per-capture is not required for correctness: sense_sleep.h already
// deinits on the way to sleep ("sleep path camera still initialized - deinit
// first"), so the camera never survives into deep sleep either way. Holding it
// initialised for the ~10-40s awake window costs some current; failing every
// second capture costs the user their photo.
// OFF (2026-08-21). It fixed the camera and broke the device.
//
// With it on: camera init failures 20-27 -> 0 across two soaks. But the camera's
// 16KB DMA block is then still held during the upload, and the Sense ran out of
// heap — 3 aborts in 12 captures, decoded as:
//     console_write -> uart_write -> _lock_acquire_recursive
//                   -> lock_init_generic -> abort()
// i.e. it could not allocate a MUTEX for a printf. A failed capture reports an
// honest error and the user retries; an abort reboots the device mid-operation
// and loses whatever was in flight. The trade is strictly bad.
//
// This is the third cheap fix to fail, and they all fail the same way: the camera
// needs ~16KB contiguous internal DMA, a TLS handshake needs ~25-30KB, and there
// is roughly 40KB. They cannot overlap. No amount of re-ordering the teardown
// makes two things fit in the space for one — the only real fix is to stop them
// overlapping in TIME, which is exactly the spool-first design in lcd_sdspool.h.
#ifndef HALO_CAMERA_KEEP_INIT
#define HALO_CAMERA_KEEP_INIT 0
#endif  // Camera's largest single DMA allocation

// ── DMA reserve: release/re-acquire, instrumented ──────────────────────────
//
// The reserve exists to hold the ONE contiguous 16KB DMA block that
// esp_camera_init() needs, so WiFi/TLS cannot fragment it away between
// captures. Margin is thin: the largest free DMA block measures ~17,396 bytes
// against a 16,384-byte requirement, i.e. about 1KB. Lose the reserve and the
// next camera init is a coin flip.
//
// Five sites released it and four re-acquired it, each open-coded. THREE
// released with no log at all, and ALL FOUR discarded the re-acquire result --
// so a failed re-acquire left the region unprotected with zero evidence, and
// the next init failed with a bare 0xffffffff. On 2026-08-20 that presented as
// a ~1-in-12 capture failure whose log showed no "DMA reservation released"
// line before the failing init -- because the pointer was already null and the
// guarded release had silently done nothing.
//
// These helpers replace the open-coded blocks so every transition is logged and
// a failed re-acquire is loud. The retry matters as much as the log: TLS
// teardown frees internal SRAM slightly after the socket closes, so an
// immediate retry often succeeds where the first attempt did not.
static void camera_dma_reserve_release(const char* who) {
  uint8_t* held = g_camera_dma_reserve.exchange(nullptr);
  if (!held) return;
  heap_caps_free(held);
  Serial.printf("[DMA_RESERVE] released by=%s bytes=%u largest_now=%u\n",
                who ? who : "?", (unsigned)CAMERA_DMA_RESERVE_BYTES,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

static bool camera_dma_reserve_acquire(const char* who) {
  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (g_dma_reserve_suppressed.load() || g_camera_dma_provisioning.load()) return false;
    if (g_camera_dma_reserve.load()) return true;
    uint8_t* candidate = (uint8_t*)heap_caps_malloc(
        CAMERA_DMA_RESERVE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (candidate) {
      // Setup/OTA may suppress the reserve while allocation runs. No heap,
      // logging or delay work is allowed inside this publication gate.
      portENTER_CRITICAL(&g_camera_dma_reserve_mux);
      const bool allowed = !g_dma_reserve_suppressed.load() &&
                           !g_camera_dma_provisioning.load();
      bool published = false;
      if (allowed && !g_camera_dma_reserve.load()) {
        g_camera_dma_reserve.store(candidate);
        published = true;
      }
      portEXIT_CRITICAL(&g_camera_dma_reserve_mux);
      if (!published) heap_caps_free(candidate);
      if (published && attempt > 1) {
        Serial.printf("[DMA_RESERVE] acquired by=%s at=%p attempt=%d\n",
                      who ? who : "?", candidate, attempt);
      }
      return allowed;
    }
    if (attempt < 3) delay(20);
  }
  Serial.printf("[DMA_RESERVE][WARN] re-acquire FAILED by=%s largest=%u need=%u "
                "- next camera init is at risk\n",
                who ? who : "?",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                (unsigned)CAMERA_DMA_RESERVE_BYTES);
  return false;
}

static void camera_dma_reserve_provisioning(bool active) {
  const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  const size_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  portENTER_CRITICAL(&g_camera_dma_reserve_mux);
  g_camera_dma_provisioning.store(active);
  portEXIT_CRITICAL(&g_camera_dma_reserve_mux);
  if (active) camera_dma_reserve_release("provisioning");
  else camera_dma_reserve_acquire("provisioning_exit");
  Serial.printf("[DMA_RESERVE] provisioning=%u held=%u free_before=%u largest_before=%u free_after=%u largest_after=%u\n",
                active ? 1u : 0u, g_camera_dma_reserve.load() ? 1u : 0u,
                (unsigned)free_before, (unsigned)largest_before,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

static void camera_dma_reserve_suppress_for_ota() {
  portENTER_CRITICAL(&g_camera_dma_reserve_mux);
  g_dma_reserve_suppressed.store(true);
  portEXIT_CRITICAL(&g_camera_dma_reserve_mux);
  camera_dma_reserve_release("ota");
}


// Tear WiFi down before every camera init "just in case" DMA is short.
// OFF by default: the check that drove it reads free DMA while the 16KB reserve
// is still held, so it fired on every capture and starved uploads of a usable
// connection. init_camera() already retries with quiesce_network_for_camera()
// when a DMA shortage is real. See sense_camera.h for the measurement.
#ifndef CAMERA_PREEMPTIVE_WIFI_KILL
#define CAMERA_PREEMPTIVE_WIFI_KILL 0
#endif

// Defined HERE, not next to the deferral logic further down, because
// sense_can_sleep_now() tests it and sits ~400 lines above that code. Left where
// it was, `#if HALO_DEFER_UPLOADS_TO_SLEEP` in the sleep guard would expand to 0
// with no warning and silently restore the bug it is there to prevent.
#ifndef HALO_DEFER_UPLOADS_TO_SLEEP
#define HALO_DEFER_UPLOADS_TO_SLEEP 1
#endif

// Declared HERE rather than beside the deferral logic further down, because
// sense_op_queue.h is included ~800 lines before that and needs to test it: once
// the sleep flush is running there is no foreground left to yield to.
static volatile bool g_upload_flush_requested = false;
static const uint32_t CAMERA_NETWORK_QUIESCE_DELAY_MS = 50;
static const uint32_t CAMERA_CAPTURE_SETTLE_MS = 30;
static const uint32_t CAMERA_WARMUP_DELAY_FAST_MS = 30;
static const uint32_t CAMERA_WARMUP_DELAY_SLOW_MS = 30;
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
enum CameraProfile { CAM_PROFILE_NORMAL = 0, CAM_PROFILE_LOW_LIGHT = 1, CAM_PROFILE_FLASH = 2, CAM_PROFILE_LABEL = 3 };

// awsEndpoint is retained for DNS pre-resolution in sense_http.h only.
// The MQTT client, its result-topic subscription and the dish/nutrition
// result path were deleted 2026-08-21 -- the nutrition feature is gone and
// dish is now a plain capture-and-log like check-in and discard.
const char* awsEndpoint = "arq86ma48kw9j-ats.iot.us-east-1.amazonaws.com";

// ── Owner/Provisioning Helpers ─────────────────────────────────────
static void load_owner_id_or_default(char* out, size_t out_len) {
  if (!out || out_len == 0) {
    return;
  }
  out[0] = '\0';
  char owner_code[32] = {0};
  if (ProvisioningState::loadOwnerCode(owner_code, sizeof(owner_code))) {
    // Owner code exists -- check if owner_id was already set via /user-id fallback.
    char tmp_id[64] = {0};
    if (ProvisioningState::loadOwnerId(tmp_id, sizeof(tmp_id)) && tmp_id[0] != '\0') {
      // Both exist: claim effectively complete. Clear stale owner_code.
      ProvisioningState::clearOwnerCode();
      Serial.printf("[OWNER] owner_code stale, owner_id already set -> cleared owner_code, using owner_id=%s\n", tmp_id);
      strncpy(out, tmp_id, out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
    // owner_code pending, no owner_id yet -> genuine pending claim, suppress.
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

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_ETH);
  snprintf(out, out_len, "halo-%02x%02x-%02x%02x",
           mac[2], mac[3], mac[4], mac[5]);
}

static String build_runtime_ota_topic() {
  char device_id[32] = {0};
  load_runtime_device_id(device_id, sizeof(device_id));
  return String("trepo/halo/") + device_id + "/ota";
}

#ifndef HALO_SENSE_PROD_WRAPPER
// Amazon Root CA 1 -- public, used for HTTPS peer validation
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

// No device credentials live in this file, and none should ever be added.
//
// An AWS IoT client certificate and its RSA private key were once inlined here
// and reached a PUBLIC repo (branch lcd-backlight-binary, 2026-04-14). The cert
// has since been revoked and deleted. MQTT itself is gone as of 2026-08-21, so
// there is no longer any consumer for device credentials at all -- if one is
// ever reintroduced it must load them from MqttSecrets.local.cpp, which is
// git-ignored (.gitignore: **/MqttSecrets.local.*). tools/check_secrets.sh
// enforces this.
//
// rootCA above is retained on purpose -- it is the public Amazon Root CA 1 and
// sense_http.h uses it to validate HTTPS peers. It is not a secret.

#endif

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
#define AUDIO_BUFFER_SIZE (512 * 1024)  // 512KB buffer (~16 seconds at 16kHz, 16-bit mono)
static uint8_t* voice_audio_buffer = NULL;
static volatile size_t voice_audio_pos = 0;
static volatile size_t voice_audio_size = 0;
static volatile bool voice_recording_active = false;
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

// Forward declarations — functions still defined in this .ino
static void uart_send_ui_status(const char* text);
struct PresignReply {
  String job_id;
  String put_url;
  String s3_key;
  String content_type;
  String result_url;
  int    ttl_s;
  bool immutable_image = false;
  bool upload_stored = false;
  String checksum_sha256_b64;
  String operation_token;
  uint32_t expected_image_bytes = 0;

  PresignReply() : ttl_s(0) {}  // Constructor to initialize ttl_s
};
static bool net_ready_for_tls(const char* reason, uint32_t timeout_ms, const char* mode, uint32_t job_id, const char* ui_policy = NULL);
static const char* sense_device_state_name();
// Forward declarations — sense_upload_queue.h (late include)
static void sleep_defer_queued_background_uploads();
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
                             uint32_t created_epoch = 0,
                             const UploadJob::ImageEnvelope* image = nullptr,
                             bool from_image_sd = false);
static bool queue_voice_upload_job(uint32_t job_id,
                                   uint8_t* audio_buf,
                                   size_t audio_len,
                                   uint8_t retries = 0,
                                   bool from_persisted = false,
                                   uint32_t created_epoch = 0,
                                   const UploadJob::VoiceEnvelope* voice = nullptr,
                                   bool from_voice_sd = false);
// Forward declarations — sense_upload_exec.h (late include)
static bool get_presign_checkin(PresignReply& out, const char* expiry_date = NULL, uint16_t quantity = 1, const UploadJob::CameraUploadMeta* camera_meta = nullptr, uint32_t deadline_ms = 0);
static bool put_to_presigned_url(const String& url,
                                 const uint8_t* buf,
                                 size_t len,
                                 const char* contentType,
                                 uint32_t job_id = 0,
                                 uint32_t deadline_ms = 0,
                                 bool* aborted_for_budget = NULL,
                                 const PresignReply* durable = nullptr,
                                 int* response_code = nullptr);
// Forward declarations — sense_sleep.h (late include)
#ifdef HALO_SENSE_PROD_WRAPPER
// Implemented in halo_sense_prod.ino (needs SenseOtaPolicy + g_ota_check_requested,
// both of which are declared after this file is included).
static void halo_request_ota_check(const char* why, bool allow_reboot);
#endif
static void sleep_send_deny_and_clear(const char* reason, unsigned long now_ms);
static bool wake_pin_is_active_level(int level);
static void wake_pin_configure_rtc_input_inactive_pull();
static uint32_t sleep_deny_retry_ms(const char* reason, unsigned long now_ms);

enum UiEvtType { UI_EVT_STATUS, UI_EVT_LIST_UPDATE, UI_EVT_VOICE_ITEMS, UI_EVT_ERROR };

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
#if HALO_SPOOL_TEST
static uint8_t g_test_fail_uploads = 0;   // bench: force N upload failures
#endif
static OpJob current_job = {OP_LIST_REFRESH, PRI_BG, 0, 0, OP_IDLE, false, "", ""};
// ONE upload queue for every mode.
//
// Dish used to have its own queue, dequeued first and unconditionally -- ahead
// of the deferral gate. That was priority scaffolding for the nutrition result
// a user waited on, and it outlived the feature: measured 2026-08-21, a dish
// capture still ran a full presign+PUT mid-session and so still fragmented the
// internal DMA region that esp_camera_init() needs (SHIP_CHECKLIST §6). Dish is
// a plain capture-and-log now, so it queues and defers exactly like the rest.
static QueueHandle_t upload_queue = NULL;
static const uint8_t UPLOAD_QUEUE_MAX = 10;
static const uint8_t OP_QUEUE_MAX = 20;
static TaskHandle_t upload_worker_task_handle = NULL;
static bool scan_ui_inflight = false;
static volatile bool dish_scan_inflight = false;
static volatile bool upload_inflight = false;
// Publish before either dequeue, including the gap before upload_inflight.
// Only the upload worker owns this token; no lock spans processing/callbacks.
static std::atomic<bool> upload_worker_claim_active{false};
class UploadWorkerClaim {
 public:
  bool before_take() {
    held_ = true;
    upload_worker_claim_active.store(true);
    return true;
  }
  void release() {
    if (held_) { upload_worker_claim_active.store(false); held_ = false; }
  }
  ~UploadWorkerClaim() { release(); }
 private:
  bool held_ = false;
};
static SemaphoreHandle_t http_mutex = NULL;
static volatile bool http_inflight = false;
static bool wifi_recover_requested = false;
static TaskHandle_t op_worker_task_handle = NULL;  // Handle to suspend/resume task
static bool scan_terminal_sent = false;
static char presign_last_error_text[64] = "";
static CameraProfile g_camera_profile = CAM_PROFILE_LABEL;
static bool g_camera_preflight_force = false;
static int g_last_scene_luma = -1;
static int g_last_scene_green_ratio = -1;

// Camera module auto-detection (HD3FM-811 vs DCX-OV2640-v2)
// Detected by timing first warmup frame readout at 20MHz SXGA
enum CameraModule { CAM_MODULE_UNKNOWN = 0, CAM_MODULE_HD3FM = 1, CAM_MODULE_DCX = 2 };
static CameraModule g_camera_module = CAM_MODULE_UNKNOWN;
static uint32_t g_camera_first_frame_ms = 0;
static const uint32_t CAMERA_MODULE_DETECT_THRESHOLD_MS = 350;  // HD3FM < 350ms, DCX > 350ms

#ifndef HALO_SENSE_PROD_WRAPPER
static volatile bool g_lcd_ota_done = false;
static char g_lcd_ota_result[32] = "unknown";
static char g_lcd_ota_version[32] = "";
static volatile bool g_lcd_ota_task_running = false;
#endif

// ── Work State ─────────────────────────────────────────────────────
// Use work state pattern so UART RX never blocks on Wi-Fi
volatile bool refresh_requested = false;
volatile bool delete_requested = false;
volatile bool reset_wifi_requested = false;

static bool list_refresh_inflight = false;
// Set true while the user is on the LCD shopping-list screen. Pins the Sense
// awake + WiFi connected so list refresh/delete hit a live connection instantly.
// The LCD re-asserts LIST_ACTIVE(state=1) every few seconds; if it stops
// (e.g. LCD crash), the staleness watchdog auto-clears this after ~30s so a
// stuck flag can never pin the device awake forever.
static volatile bool g_list_screen_active = false;
static unsigned long last_list_active_ms = 0;
static const unsigned long LIST_ACTIVE_STALE_MS = 30000;
static std::atomic<bool> g_voice_list_refresh_pending{false};
static std::atomic<bool> g_voice_list_followup{false};
static std::atomic<uint32_t> g_voice_list_attempted_job{0};
static bool voice_list_pending();
static std::atomic<bool> list_transport_refresh_deferred{false};
static unsigned long list_refresh_start_ms = 0;
static unsigned long list_refresh_cooldown_until_ms = 0;
// Last SUCCESSFUL list fetch (set in parse_and_update_shopping_list). Used by
// request_list_refresh() to serve the cached list instantly when a user
// refresh lands inside the cooldown window instead of silently dropping it.
static unsigned long list_last_fetch_ok_ms = 0;
static const unsigned long LIST_REFRESH_TIMEOUT_MS = 20000;
// Max time an in-flight list refresh may block idle sleep in sense_can_sleep_now().
// After this the gate stops pinning the device awake so a stuck refresh (flaky
// WiFi) can never hold it past the idle timeout. Belt-and-suspenders alongside
// the coordinated-sleep background-force path and the 20s inflight watchdog.
static const unsigned long LIST_REFRESH_BLOCK_MAX_MS = 12000;
#ifndef HALO_SENSE_PROD_WRAPPER
static const unsigned long LIST_REFRESH_COOLDOWN_MS = 3000;
#endif
static char g_last_refresh_reason[32] = "unknown";
static bool link_synced = false;
static unsigned long last_user_activity_ms = 0;
#include "sense_user_activity.h"
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
  // household_item_uuid from the list API — required for delete: the iOS app
  // deletes by itemUUID (household-wide), not row id. 50x64B = +3.2KB RAM.
  char huuid[64];
  char store[48];   // store name e.g. "Whole Foods" (empty = no store)
};

static shopping_list_item_t g_shopping_list[MAX_LIST_ITEMS];
static int g_list_count = 0;
static int g_selected_index = -1;
static SemaphoreHandle_t g_list_mutex = NULL;

#include "sense_time.h"
#include "sense_captrace.h"
// True while the Sense is STREAMING an image out to the LCD. Suppresses this
// board's own JSON TX so a periodic SENSE_DIAG cannot land inside the COBS
// frames it is transmitting.
static std::atomic<bool> g_img_spool_tx_active{false};
// RAM custody includes admission waits between individual SD transactions.
static std::atomic<uint32_t> g_media_custody_waiters{0};
// setup() and loop() share Arduino's owner task. Background upload retries may
// collect bytes, but only this task may dispatch ordinary input callbacks.
static TaskHandle_t g_sense_main_task_handle = NULL;
static bool sense_uart_ordinary_tx_allowed();  // Defined with the transport guard below.
#include "sense_diag.h"
#include "sense_errlog.h"
// True while the SD-spool drain is streaming an image back from the LCD over
// COBS. Declared here rather than in sense_spool_drain.h because
// pump_uart_rx_once() (sense_uart.h, included next) must stand down while it is
// set, and that header comes long before the drain's.
static bool g_spool_owns_uart = false;
// True for the whole of a wake whose purpose is emptying the SD spool. Declared
// here rather than in sense_spool_drain.h for the same reason as the flag above:
// sense_can_sleep_now() has to test it and is defined long before that header.
static bool g_spool_drain_wake = false;
static uint32_t g_spool_drain_wake_start_ms = 0;
static uint32_t g_spool_drain_wake_ok_at_start = 0;

#include "sense_uart.h"
#include "sense_img_spool.h"  // spool captures to the LCD SD card (Step 4)
// After sense_uart.h: the relay uses uart_send_sense_diag().
#include "sense_wakelog.h"   // per-wake-cycle history that survives deep sleep

// Record error locally AND forward to LCD for NVS persistence.
// Defined here (not in sense_diag.h) because it depends on both
// sense_diag.h and sense_uart.h, and the former is included first.

// Forward declaration — defined in sense_upload_queue.h (included later).
static uint32_t upload_queue_count();

// Build compact device-state context string for error enrichment.
static int diag_snapshot_context(char* buf, size_t buf_size) {
    int32_t rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
    uint32_t heap_kb = esp_get_free_heap_size() / 1024;

    const char* op_str = "idle";
    const char* op_mode = "";
    if (current_job.active) {
        switch (current_job.type) {
            case OP_SCAN:  op_str = "scan"; op_mode = current_job.mode; break;
            case OP_VOICE: op_str = "voice"; break;
            case OP_LIST_REFRESH: op_str = "list"; break;
            default: op_str = "op"; break;
        }
    }

    return snprintf(buf, buf_size,
        "heap=%luK rssi=%ld op=%s%s%s fg=%d http=%d upl_q=%d boot=%lu",
        (unsigned long)heap_kb,
        (long)rssi,
        op_str,
        op_mode[0] ? "/" : "",
        op_mode,
        (int)foreground_active,
        (int)http_inflight,
        upload_queue_count(),
        (unsigned long)g_sense_boot_count);
}

static void diag_record_error_persistent(const char* stage, int32_t code, const char* text) {
  diag_record_error(stage, code, text);
  char enriched[192];
  char ctx[128];
  diag_snapshot_context(ctx, sizeof(ctx));
  snprintf(enriched, sizeof(enriched), "%s | %s", text ? text : "", ctx);
  uart_send_sense_diag_persist(stage, "ERROR", stage, code, enriched);
  sense_errlog_store(stage, code, enriched);
}

#include "sense_uart_msg.h"
#ifdef HALO_SENSE_PROD_WRAPPER
#include "sense_ota_lcd.h"
#include "sense_lcd_query_proof.h"
#endif
#include "sense_http.h"
#include "sense_wifi.h"
#include "sense_media_retry.h"
#include "sense_upload.h"
#include "sense_voice.h"
#include "sense_voice_spool.h"
#include "sense_image_identity.h"
#include "sense_image_spool.h"
#include "sense_list.h"
#include "sense_camera.h"
#include "sense_presign.h"
#include "sense_image_upload.h"
#include "sense_scan.h"
#include "sense_op_queue.h"
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
static const unsigned long SLEEP_HOLDOFF_MS = 2000;
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
  if (reason && reason[0]) {
    strncpy(g_last_refresh_reason, reason, sizeof(g_last_refresh_reason) - 1);
    g_last_refresh_reason[sizeof(g_last_refresh_reason) - 1] = '\0';
  }
  // Suppress non-OTA HTTP while an OTA proxy/transfer is active — a list
  // GET on the single net stack starves the LCD OTA S3 download and can
  // stall it to abort. Deferred refreshes resume after OTA completes.
  if (lcd_ota_in_progress()) {
    Serial.printf("[LIST_REFRESH] deferred (lcd_ota_in_progress) reason=%s — will resume after OTA\n",
                  reason ? reason : "unknown");
    return;
  }
  if (g_list_screen_active && (voice_list_pending() || g_media_voice_list_mode.load())) {
    // One deferred request, regardless of how often the user refreshes.
    g_voice_list_refresh_pending.store(true);
    return;
  }
  unsigned long now = millis();
  if (list_refresh_inflight) {
    unsigned long inflight_ms =
        (list_refresh_start_ms > 0) ? (now - list_refresh_start_ms) : 0;
    if (inflight_ms > LIST_REFRESH_TIMEOUT_MS) {
      // Self-heal: a wedged inflight flag (fetch path died without reaching
      // list_refresh_mark_complete) would otherwise block every refresh until
      // reboot. Clear it and accept this request.
      Serial.printf("[LIST_REFRESH] stuck inflight elapsed_ms=%lu -> self-heal, accepting reason=%s\n",
                    inflight_ms,
                    reason ? reason : "unknown");
      list_refresh_inflight = false;
      list_refresh_start_ms = 0;
    } else {
      // Safe debounce: a fetch is genuinely running and will answer the LCD
      // with UI_LIST + IDLE when it completes, so the LCD's refresh state
      // machine still terminates — nothing is silently dropped here.
      Serial.printf("[LIST_REFRESH] already inflight elapsed_ms=%lu, skipping reason=%s\n",
                    inflight_ms,
                    reason ? reason : "unknown");
      return;
    }
  }
  if (now < list_refresh_cooldown_until_ms) {
    // A user refresh must never be silently eaten by the cooldown — the LCD
    // would sit on "Refreshing..." until its 12s hard timeout. If the cached
    // list was fetched recently (within the cooldown window, +1s jitter
    // margin), re-send it so the LCD refresh completes instantly.
    unsigned long cache_age_ms =
        (list_last_fetch_ok_ms > 0) ? (now - list_last_fetch_ok_ms) : 0;
    if (list_last_fetch_ok_ms > 0 &&
        cache_age_ms <= LIST_REFRESH_COOLDOWN_MS + 1000) {
      Serial.printf("[LIST_REFRESH] cooldown_hit -> served_cached cache_age_ms=%lu reason=%s\n",
                    cache_age_ms,
                    reason ? reason : "unknown");
      // Awake-proof FIRST: the LCD refresh SM sits in REFRESH_WAKE_PENDING
      // until it sees PONG/SYNC_ACK/UI_STATUS. The cached UI_LIST lands
      // ~100ms after the wake pulse — before any of those — so the LCD
      // rendered the list but its SM never completed (stuck "Refreshing"
      // pill). Send a UI_STATUS first, give the LCD's Core-0 RX a beat to
      // process it, then the list, so the SM is INFLIGHT when UI_LIST lands.
      uart_send_ui_status("IDLE");
      Serial.println("[LIST_REFRESH] awake_proof_sent path=cached");
      delay(40);
      uart_send_ui_list();
      delay(50);
      uart_send_ui_status("IDLE");
      return;
    }
    // No fresh cache (last fetch failed or never ran) — accept the request
    // despite the cooldown rather than silently dropping a user refresh.
    Serial.printf("[LIST_REFRESH] cooldown_hit -> cache_stale, accepting reason=%s\n",
                  reason ? reason : "unknown");
  }
  list_refresh_inflight = true;
  list_refresh_start_ms = now;
  OpJob job = {OP_LIST_REFRESH, PRI_BG, get_next_msg_id(), now, OP_IDLE, false, "", ""};
  job.quantity = 0;
  job.add_to_shopping_list = false;
  if (!enqueue_op_job(job, false, "list_refresh")) {
    Serial.printf("[LIST_REFRESH] failed to enqueue reason=%s\n",
                  reason ? reason : "unknown");
    list_refresh_inflight = false;
    return;
  }
  if (send_status) {
    uart_send_ui_status("Refreshing...");
  }
  Serial.printf("[LIST_REFRESH] request accepted reason=%s\n",
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
  // Heap-allocated doc (freed on scope exit): 50 items x up to 64B id + 64B
  // text + 48B store + per-object overhead can exceed a 8KB doc, which silently
  // dropped items. Bumped to 12KB after adding the per-item store field. This
  // runs on a task with WiFi up; a brief ~12KB heap allocation is fine.
  DynamicJsonDocument doc(12288);
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "UI_LIST";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  int dropped = 0;
  int total = 0;
  JsonArray items = doc.createNestedArray("items");
  if (g_list_mutex == NULL || xSemaphoreTake(g_list_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println("[UI_LIST] skipped reason=list_lock_unavailable");
    return;  // A missing snapshot must never become a successful empty list.
  }
  {
    doc["selected_index"] = g_selected_index;
    total = g_list_count;
    for (int i = 0; i < g_list_count; i++) {
      JsonObject item = items.createNestedObject();
      if (item.isNull()) {  // doc out of memory — stop instead of silent drop
        dropped = g_list_count - i;
        break;
      }
      item["id"] = g_shopping_list[i].id;
      item["text"] = g_shopping_list[i].text;
      item["store"] = g_shopping_list[i].store;
    }
    xSemaphoreGive(g_list_mutex);
  }
  if (dropped > 0 || doc.overflowed()) {
    Serial.printf("[UI_LIST] skipped reason=allocation dropped=%d overflowed=%d total=%d\n",
                  dropped, doc.overflowed() ? 1 : 0, total);
    return;  // Preserve the LCD's last complete list instead of publishing a prefix.
  }

  String output;
  const size_t expected_bytes = measureJson(doc);
  if (!output.reserve(expected_bytes) || serializeJson(doc, output) != expected_bytes ||
      output.length() != expected_bytes) {
    Serial.println("[UI_LIST] skipped reason=serialization");
    return;
  }
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

static void uart_send_fw_info(bool do_lcd_query = true) {
  // FW_INFO carries the Sense fw version (known instantly) and the LCD fw.
  //
  // do_lcd_query == true  (INPUT_FW_INFO / diagnostic path, UNCHANGED):
  //   Trigger a FRESH LCD query round-trip so FW_INFO reports the REAL running
  //   LCD firmware + partition/state (not the cached OTA manifest value).
  //   This BLOCKS for up to several seconds. Automation depends on the fresh
  //   lcd_fw / running_state, so this behavior must not change.
  //
  // do_lcd_query == false (INPUT_SENSE_FW / fast Settings path):
  //   Skip the blocking sense_lcd_ota_query() entirely. Report sense_fw
  //   immediately and fill lcd_fw from the cached LCD version
  //   (g_lcd_ota_version, populated by the pre_sleep LCD query), or "unknown"
  //   if the cache is empty. Used by the LCD Settings screen, which only reads
  //   sense_fw and must not be held hostage for seconds.
  // Observability only — does not affect OTA control flow.
  char     lcd_fw_buf[32] = {0};
  uint32_t lcd_part_size  = 0;
  bool          lcd_ok        = false;
  unsigned long lcd_fw_age_s  = 0;

  if (do_lcd_query) {
    unsigned long query_start_ms = millis();
    lcd_ok = sense_lcd_ota_query(lcd_fw_buf, sizeof(lcd_fw_buf), &lcd_part_size);
    lcd_fw_age_s = (millis() - query_start_ms) / 1000UL;
  }

  // Include the Sense's LIVE OTA state on both paths. A reported version alone
  // cannot prove that the running image passed rollback validation. These
  // additive fields let the LCD USB diagnostics inspect it without attaching
  // Sense USB (which changes the reset/wake evidence).
  const esp_partition_t* sense_running = esp_ota_get_running_partition();
  const esp_partition_t* sense_boot = esp_ota_get_boot_partition();
  const char* sense_state = "UNKNOWN";
  esp_ota_img_states_t sense_ota_state;
  if (sense_running && esp_ota_get_state_partition(sense_running, &sense_ota_state) == ESP_OK) {
    switch (sense_ota_state) {
      case ESP_OTA_IMG_NEW: sense_state = "NEW"; break;
      case ESP_OTA_IMG_PENDING_VERIFY: sense_state = "PENDING_VERIFY"; break;
      case ESP_OTA_IMG_VALID: sense_state = "VALID"; break;
      case ESP_OTA_IMG_INVALID: sense_state = "INVALID"; break;
      case ESP_OTA_IMG_ABORTED: sense_state = "ABORTED"; break;
      case ESP_OTA_IMG_UNDEFINED: sense_state = "UNDEFINED"; break;
      default: break;
    }
  }

  StaticJsonDocument<640> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "FW_INFO";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["sense_fw"] = get_sense_fw_version();
#ifdef HALO_SENSE_PROD_WRAPPER
  doc["sense_build"] = kBuildId ? kBuildId : "unknown";
#else
  doc["sense_build"] = "unknown";
#endif
  doc["sense_running_part"] = sense_running ? sense_running->label : "?";
  doc["sense_running_state"] = sense_state;
  doc["sense_boot_part"] = sense_boot ? sense_boot->label : "?";
  doc["sense_wake_cause"] = (int)esp_sleep_get_wakeup_cause();
  doc["sense_reset_reason"] = (int)esp_reset_reason();
  if (do_lcd_query) {
    if (lcd_ok && lcd_fw_buf[0] != '\0') {
      doc["lcd_fw"]            = lcd_fw_buf;
      doc["lcd_running_part"]  = sense_lcd_last_running_part();
      doc["lcd_running_state"] = sense_lcd_last_running_state();
      doc["lcd_boot_part"]     = sense_lcd_last_boot_part();
      doc["lcd_fw_age_s"]      = (uint32_t)lcd_fw_age_s;
    } else {
      // Fresh query failed/timed out — still emit sense_fw, mark LCD unknown.
      doc["lcd_fw"] = "unknown";
    }
  } else {
    // Fast path: no query. Use the cached LCD version (from pre_sleep query).
    doc["lcd_fw"] = (g_lcd_ota_version[0] != '\0') ? g_lcd_ota_version : "unknown";
  }

  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  if (do_lcd_query) {
    Serial.printf("[UART_TX] FW_INFO sent (lcd_ok=%d lcd_fw=%s state=%s)\n",
                  lcd_ok ? 1 : 0,
                  lcd_ok ? lcd_fw_buf : "unknown",
                  lcd_ok ? sense_lcd_last_running_state() : "-");
  } else {
    Serial.printf("[UART_TX] FW_INFO sent (fast, cached lcd_fw=%s)\n",
                  (g_lcd_ota_version[0] != '\0') ? g_lcd_ota_version : "unknown");
  }
}

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
  if (job_id == 0 && current_job.active && current_job.job_id > 0 && op != NULL &&
      ((strcmp(op, "SCAN") == 0 && current_job.type == OP_SCAN) ||
       (strcmp(op, "VOICE") == 0 && current_job.type == OP_VOICE))) {
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

// (scan_screen_hint, scan_mode_is_* removed — see sense_scan.h)
// (op queue, foreground priority, park/requeue removed — see sense_op_queue.h)

static bool sense_can_sleep_now(const char** reason) {
  if (guardian_force_sleep) {
    return true;
  }
  // Ordinary sleep must leave the original bounded SNTP opportunity to the
  // owner loop. Pre-sleep DNS work would stop it before teardown could resume it.
  if (sense_ntp_attempt_pending()) {
    if (reason) *reason = "clock_sync_pending";
    return false;
  }
  // An SD-spool transfer is in flight — do not sleep through it.
  //
  // A 184KB drain takes ~24s at 115200, but a maintenance wake is only ~10-14s,
  // so the device slept mid-transfer EVERY time: four attempts each died at
  // exactly seq=155 / 79,360 bytes after ~14s. That looks data-dependent until
  // you check the rate (5,673 B/s x 14s = 79KB) and realise it is failing at a
  // TIME, not at a byte.
  //
  // Bounded by the drain's own deadlines (SPOOL_DRAIN_FRAME_TIMEOUT_MS per frame,
  // 60s for the whole transfer), and it yields immediately to any user action via
  // sense_spool_drain_yield_to_user(), so this can delay sleep but never hold it.
  if (g_media_custody_waiters.load()) {
    if (reason) *reason = "media_custody";
    return false;
  }
  if (upload_worker_claim_active.load()) {
    if (reason) *reason = "upload_worker_claim";
    return false;
  }
  if (g_spool_owns_uart || g_img_spool_tx_active) {
    if (reason) *reason = "spool_transfer";
    return false;
  }
  // A drain wake has to survive long enough to actually drain.
  //
  // The guard above only holds once a transfer has STARTED, and on a drain wake
  // nothing has started yet: WiFi is still associating and the depth probe is
  // still out. The inactivity timer won every time -- measured drain wakes were
  // 13-16s long and recovered nothing, while a single transfer needs ~16s on its
  // own, so the card sat at 35 images through repeated wakes.
  //
  // Bounded by SPOOL_DRAIN_WAKE_BUDGET_MS and released the moment a capture or
  // any foreground activity appears (see sense_spool_drain_wake_active), so this
  // delays sleep on a wake nobody is waiting on, and never on a user's.
  if (g_spool_drain_wake) {
    if (reason) *reason = "spool_drain_wake";
    return false;
  }
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_prod_boot_ota_pending()) {
    if (reason) *reason = "ota_boot_pending";
    return false;
  }
  if (g_lcd_ota_request_active) {
    if (reason) *reason = "lcd_ota_pending";
    return false;
  }
  if (g_ota_pending_verify_active) {
    if (reason) *reason = "ota_pending_verify";
    return false;
  }
#endif
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
#if HALO_DEFER_UPLOADS_TO_SLEEP
    // Deliberately NOT a sleep blocker under deferral.
    //
    // Queued uploads are the expected steady state here -- they are held on
    // purpose until sleep, and the PRE-SLEEP FLUSH is the only thing that drains
    // them. Blocking sleep on a non-empty queue therefore blocks the drain, and
    // background_force_defer() then fired after 10s and DESTROYED the capture
    // (`sleep_drop ... queue_not_persisted`). Measured 2026-08-21: one photo lost
    // this way, and only ever with EXACTLY ONE job queued, because the
    // force-defer path skips itself when count != 1 -- which is why the 4-deep
    // sessions all passed and hid it. Sleeping IS the drain.
#else
    if (reason) *reason = "upload_queue";
    return false;
#endif
  }
  // Keep the device awake while the user is on the LCD shopping-list screen so
  // list refresh/delete hit a live WiFi connection instantly (no cold reconnect).
  if (g_list_screen_active) {
    if (reason) *reason = "list_screen_active";
    return false;
  }
  // An in-flight list refresh blocks idle sleep so SLEEP_READY isn't sent +
  // WiFi torn down mid-fetch (which hangs the refresh). But this must be
  // FORCE-DEFERRABLE, not a permanent pin: a stuck refresh (e.g. flaky WiFi)
  // should never hold the device awake past the idle timeout. We only block
  // while the refresh is young (< LIST_REFRESH_BLOCK_MAX_MS); after that we
  // allow sleep. ("list_refresh_inflight" is also registered in
  // sleep_reason_is_background_deferable() so the 10s background-force path in
  // the coordinated-sleep loop applies too — belt-and-suspenders.)
  if (list_refresh_inflight) {
    if (background_sleep_bypass_active) {
      return true;
    }
    unsigned long refresh_age_ms =
        (list_refresh_start_ms > 0) ? (millis() - list_refresh_start_ms) : 0;
    if (refresh_age_ms < LIST_REFRESH_BLOCK_MAX_MS) {
      if (reason) *reason = "list_refresh_inflight";
      return false;
    }
    // Stale refresh — stop pinning; let the device sleep.
  }
  return true;
}

static void log_sleep_flags(const char* where) {
  Serial.printf("[SLEEP_FLAGS] where=%s http=%d upload=%d q=%lu\n",
                where ? where : "",
                http_inflight ? 1 : 0,
                upload_inflight ? 1 : 0,
                (unsigned long)upload_queue_count());
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
      Serial.printf("[SLEEP_BLOCK] reason=%s where=%s http=%d upload=%d q=%lu\n",
                    reason ? reason : "net_inflight",
                    where ? where : "",
                    http_inflight ? 1 : 0,
                    upload_inflight ? 1 : 0,
                    (unsigned long)upload_queue_count());
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
    Serial.printf("[SLEEP] background_force_defer reason=%s where=%s elapsed_ms=%lu upload=%d q=%lu http=%d\n",
                  reason,
                  where ? where : "",
                  elapsed_ms,
                  upload_inflight ? 1 : 0,
                  (unsigned long)upload_queue_count(),
                  http_inflight ? 1 : 0);
#if !HALO_DEFER_UPLOADS_TO_SLEEP
    sleep_defer_queued_background_uploads();
#endif
    // Production deferral keeps queue custody with the worker. The ordinary
    // pre-sleep flush opens that worker's gate after this background bypass.
    background_sleep_bypass_active = true;
  }
  return true;
}

// (uart_send_sync_ack, uart_send_ui_toast, uart_send_ui_voice_response,
//  uart_send_pong, uart_send_link_hb, uart_send_sleep_ready,
//  uart_send_sleep_intent, uart_send_sleep_deny, uart_send_release_wake
//  removed — see sense_uart_msg.h)

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
  // Deliberately NOT gated on link_synced.
  //
  // link_synced only becomes true when the LCD sends SYNC, and after a TIMER
  // wake the LCD does not know the Sense woke at all -- so it never sends one.
  // That made this predicate permanently false on the nightly maintenance path,
  // which is precisely where it is load-bearing: the PRE_SLEEP LCD firmware
  // query is gated on it, so the Sense could never learn the LCD's version and
  // therefore could never decide the LCD needed an update.
  //
  // Recent RX is strictly stronger evidence than the SYNC flag anyway -- the
  // flag can be minutes stale while rx_age answers the actual question, "have
  // we heard from the LCD lately". The sibling sense_link_recent() (sense_sleep.h)
  // already treats recent RX alone as sufficient; this now matches it.
  return last_uart_rx_ms > 0 && rx_age < limit_ms;
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
  if (sleep_requested) {
    return false;
  }
  return true;
}


static void service_boot_wifi_connect(unsigned long now_ms) {
  wifi_service_events();
  service_wifi_maintenance(now_ms);
  // Close/drain an expired primary attempt before admission and sleep checks,
  // so a queued capture's one secondary opportunity has no empty-window gap.
  sense_ntp_service();
  // First IP can arrive on the same iteration that inactivity becomes eligible.
  // Fresh captures may request the existing single secondary generation. This
  // stays asynchronous; it does not dequeue media, bypass the upload hold or
  // authorize TLS without a fresh reply.
  if (wifi_is_connected()) {
    UploadJob head = {};
    if (upload_queue && xQueuePeek(upload_queue, &head, 0) == pdTRUE &&
        !head.from_voice_sd && !head.from_image_sd && !head.from_persisted)
      sense_ntp_request_media_retry();
    sense_ntp_begin();
  }
  // Consume actual SNTP replies or expire the bounded attempt on the owner
  // task. Plausible retained time alone must not stop a fresh synchronization.
  sense_ntp_service();
#if defined(HALO_SENSE_PROD_WRAPPER) && HALO_DURABLE_OTA_POLICY
  // A newly fresh reply removes the SNTP sleep hold. Queue any existing durable
  // boot recovery before this loop can sleep; admission still requires fresh UTC.
  halo_policy_service_boot();
#endif
}

// (voice functions removed — see sense_voice.h)
// (shopping list API functions removed — see sense_list.h)

// ── Camera Functions → sense_camera.h ────────────────────────────────

// (camera functions removed — see sense_camera.h)



// (presign functions removed — see sense_presign.h)

// (scan/flow functions removed — see sense_scan.h)

static const char* sense_device_state_name() {
  if (sleep_requested || sleep_coord_requested || sleep_sm_state != SLEEP_SM_IDLE) {
    return "PRE_SLEEP";
  }
  if (upload_inflight || upload_queue_count() > 0) {
    return "BACKGROUND_UPLOAD";
  }
  if (wifi_connect_inflight) {
    return "WIFI_RECOVERY";
  }
  return "AWAKE_IDLE";
}

// (upload_queue_count, upload_queue_is_full removed — see sense_upload_queue.h)

// (upload persist system removed — see sense_upload_persist.h)
#include "sense_upload_persist.h"



// (sleep_defer_queued_background_uploads, allocate_upload_buffer,
//  queue_upload_job, queue_voice_upload_job removed — see sense_upload_queue.h)
#include "sense_upload_queue.h"
#include "sense_media_retry_transport.h"
// Must follow sense_upload_queue.h: the drain hands fetched images to
// queue_upload_job(). Pulls spooled captures back off the LCD's SD card so they
// actually reach the backend instead of sitting there durably and uselessly.
#include "sense_spool_drain.h"

// Direct USB diagnostic admission; no change to ordinary Wi-Fi/OTA policy.
static bool sense_backup_diagnostic_busy() {
  bool ota = g_lcd_ota_task_running || g_lcd_ota_proxy_owns_uart;
#ifdef HALO_SENSE_PROD_WRAPPER
  ota = ota || halo_provisioning_active() || halo_prod_boot_ota_pending();
#endif
  return ota || http_inflight || upload_inflight || current_job.active || foreground_active ||
      voice_recording_active || dish_scan_inflight || scan_ui_inflight ||
      g_img_spool_tx_active || g_img_spool_request_active.load() || g_spool_owns_uart ||
      sleep_requested || sleep_coord_requested || sleep_sm_state != SLEEP_SM_IDLE;
}

// ── Defer normal uploads until the device is going to sleep ──────────────
//
// WHY: esp_camera_init() needs ONE contiguous 16,384-byte internal DMA block; a
// TLS handshake needs ~25-30KB of the same memory; there is ~40KB. The first
// handshake of a boot fragments the region permanently (largest run drops to
// 15,860 — 524 bytes short), so every capture after the first in a session fails
// ~60%. Four narrower fixes were tried; two made it worse (SHIP_CHECKLIST §6).
// The only thing that works is to stop capture and TLS overlapping IN TIME.
//
// Capture already returns before any upload — the image sits in PSRAM and the UI
// says "Logged!" immediately — so nothing user-facing waits on this.
//
// NO MODE IS EXEMPT. Dish used to be, because a dish upload was followed by the
// AI nutrition result the user sat watching. That feature is gone, and the
// exemption outlived it: measured 2026-08-21, dish still bypassed this gate via
// its own priority queue and still ran presign+PUT mid-session, which is exactly
// the TLS-during-capture this flag exists to prevent. One queue, one gate, all
// modes.
// (HALO_DEFER_UPLOADS_TO_SLEEP is defined near the top of this file, above
// sense_can_sleep_now() -- that guard needs it and is ~400 lines earlier.)

// (g_upload_flush_requested is declared near the top of this file -- see there.)
static bool voice_list_job_eligible(const UploadJob& job) {
  if (!job.is_voice || job.from_voice_sd || job.from_image_sd || job.from_persisted ||
      !g_list_screen_active || job.job_id == g_voice_list_attempted_job.load() ||
      !wifi_is_connected() || !sense_time_has_fresh_sync() ||
      foreground_active || scan_ui_inflight || dish_scan_inflight || voice_recording_active ||
      g_camera_radio_off_owned.load() ||
      (current_job.active && current_job.pri == PRI_USER)) return false;
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active() || halo_prod_boot_ota_pending() || lcd_ota_in_progress()) return false;
#endif
  if (op_queue) {
    OpJob next = {};
    if (xQueuePeek(op_queue, &next, 0) == pdTRUE && next.pri == PRI_USER) return false;
  }
  return true;
}

static bool voice_list_pending() {
  UploadJob next = {};
  // Preserve the existing parked owner and FIFO order. Never bypass an image
  // or saved replay merely to find a later voice note.
  if (upload_worker_peek_parked_job(next)) return voice_list_job_eligible(next);
  return upload_queue && xQueuePeek(upload_queue, &next, 0) == pdTRUE &&
         voice_list_job_eligible(next);
}

static unsigned long g_upload_hold_since_ms = 0;

// Never hold so long, or so many, that we risk losing the user's captures:
// PSRAM does not survive power loss, so these bound the exposure.
static const unsigned long UPLOAD_HOLD_MAX_MS = 10UL * 60UL * 1000UL;  // 10 min
static const uint32_t UPLOAD_HOLD_HIGHWATER = 8;                       // of UPLOAD_QUEUE_MAX=10

static bool uploads_held_for_session(const char** why_out) {
#if !HALO_DEFER_UPLOADS_TO_SLEEP
  if (why_out) *why_out = "disabled";
  return false;
#else
  if (g_upload_flush_requested) { if (why_out) *why_out = "flush_requested"; return false; }
  if (g_list_screen_active) {
    UploadJob parked = {};
    if (upload_worker_peek_parked_job(parked) && parked.is_voice &&
        parked.job_id == g_voice_list_attempted_job.load()) {
      if (why_out) *why_out = "voice_list_attempt_used";
      return true;
    }
    if (voice_list_pending()) {
      if (why_out) *why_out = "fresh_voice_for_list";
      return http_inflight;
    }
  }
  const uint32_t n = upload_queue_count();
  if (n == 0) { g_upload_hold_since_ms = 0; if (why_out) *why_out = "empty"; return false; }
  // A previously durable replay is background recovery, not a new capture.
  // Release only that FIFO head and only outside foreground/user-input work;
  // subsequent fresh captures retain the existing defer-until-sleep policy.
  UploadJob head = {};
  if (upload_queue && xQueuePeek(upload_queue, &head, 0) == pdTRUE &&
      (head.from_voice_sd || head.from_image_sd || head.from_persisted) &&
      !g_media_retry_user_paused.load() &&
      !foreground_priority_active(millis(), nullptr)) {
    if (why_out) *why_out = "durable_replay";
    return false;
  }
  if (n >= UPLOAD_HOLD_HIGHWATER) { if (why_out) *why_out = "highwater"; return false; }
  if (g_upload_hold_since_ms == 0) g_upload_hold_since_ms = millis();
  if ((millis() - g_upload_hold_since_ms) > UPLOAD_HOLD_MAX_MS) {
    if (why_out) *why_out = "max_age";
    return false;
  }
  if (why_out) *why_out = "session_active";
  return true;
#endif
}

static void upload_worker_task(void *arg) {
  Serial.println("[UPLOAD] Background upload task started");
  for (;;) {
#ifdef HALO_SENSE_PROD_WRAPPER
    // Claim transport borrows this existing stack, ahead of media ownership
    // gates. It never enters the upload queue or takes custody of media.
    if (halo_provisioning_claim_worker_poll()) continue;
#endif
    UploadWorkerClaim worker_claim;
    UploadJob job = {};
    bool got_job = false;
    const char* parked_stage = "idle";
    unsigned long parked_at_ms = 0;
    if (!uploads_held_for_session(NULL) &&
        worker_claim.before_take() &&
        upload_worker_take_parked_job(job, &parked_stage, &parked_at_ms)) {
      Serial.printf("[UPLOAD_QUEUE] resume_parked job_id=%lu mode=%s voice=%d stage=%s parked_ms=%lu q=%lu\n",
                    (unsigned long)job.job_id,
                    job.mode,
                    job.is_voice ? 1 : 0,
                    parked_stage ? parked_stage : "foreground",
                    parked_at_ms > 0 ? (unsigned long)(millis() - parked_at_ms) : 0UL,
                    (unsigned long)upload_queue_count());
      got_job = true;
    } else if (!uploads_held_for_session(NULL) && upload_queue != NULL &&
               worker_claim.before_take() &&
               // NON-BLOCKING on purpose. This used to wait 200ms, which raced the
               // hold gate above it: the gate is evaluated while the queue is still
               // empty (so it opens), and then the blocking receive picks up the job
               // that arrives during the wait. That let the FIRST upload of every
               // session through — the one whose TLS handshake fragments the DMA
               // region and breaks every later capture. With a 0 timeout the gate and
               // the receive see the same queue state.
               xQueueReceive(upload_queue, &job, 0) == pdTRUE) {
      got_job = true;
    } else if (uploads_held_for_session(NULL)) {
      worker_claim.release();
      static unsigned long last_defer_log_ms = 0;
      const unsigned long now_ms = millis();
      if (now_ms - last_defer_log_ms > 5000) {
        const char* why = "session_active";
        (void)uploads_held_for_session(&why);
        Serial.printf("[UPLOAD_DEFER] holding %lu upload(s) until sleep (%s, held_ms=%lu)\n",
                      (unsigned long)upload_queue_count(), why,
                      g_upload_hold_since_ms ? (unsigned long)(now_ms - g_upload_hold_since_ms) : 0UL);
        last_defer_log_ms = now_ms;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    } else if (!got_job) {
      worker_claim.release();
      // The normal-queue receive no longer blocks, so pace the loop here.
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (got_job) {
      const bool list_voice = voice_list_job_eligible(job);
      MediaRetryNetworkScope retry_network(job, list_voice);
      if (list_voice) {
        g_voice_list_attempted_job.store(job.job_id);
        Serial.printf("[VOICE_LIST] start job=%lu budget_ms=%lu\n", (unsigned long)job.job_id,
                      (unsigned long)VOICE_LIST_UPLOAD_BUDGET_MS);
      }
      auto park_cancelled_fresh = [&](const char* stage) {
        if (job.from_voice_sd || job.from_image_sd || job.from_persisted ||
            !media_retry_network_cancelled() ||
            (media_voice_list_timed_out() && !media_voice_list_user_interrupted())) return false;
        // A new input ended this transport attempt, not the capture's custody.
        // Preserve its exact RAM envelope before storage-busy retries can hold
        // up that gesture. A full parked slot leaves the existing save path.
        if (!upload_worker_park_job(job, stage, "new_user_input")) return false;
        upload_inflight = false;
        vTaskDelay(pdMS_TO_TICKS(40));
        return true;
      };
      if (!job.image_buf || job.image_len == 0) {
        Serial.println(job.is_voice ? "[VOICE_QUEUE] job missing audio buffer"
                                    : "[UPLOAD] job missing image buffer");
        diag_record_error(job.is_voice ? "voice_upload" : "upload", -1, "missing_buffer");
        continue;
      }
      if (park_upload_job_if_foreground_active(job, "dequeued")) {
        vTaskDelay(pdMS_TO_TICKS(40));
        continue;
      }
      diag_record_action(job.is_voice ? "voice_upload" : "upload");
      upload_inflight = true;
      if ((job.from_voice_sd || job.from_image_sd || job.from_persisted) &&
          g_media_retry_user_paused.load()) {
        // Only the fetched RAM copy is released. The committed original keeps
        // its identity and will be retried on a later background wake.
        Serial.printf("[MEDIA_RETRY] yield_to_user job=%lu retained=1\n", (unsigned long)job.job_id);
        free(job.image_buf);
        upload_inflight = false;
        continue;
      }
      // The owner loop already retries association and fresh clock sync. Once
      // the user leaves, offline captures go straight to durable custody; do
      // not spend a separate multi-attempt Wi-Fi budget on each queued item.
      if (!wifi_is_connected() || !sense_time_has_fresh_sync()) {
        upload_persist_handle_failure(job, "network_or_clock_pending");
        free(job.image_buf);
        upload_inflight = false;
        continue;
      }
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
        // Association loss after the earlier gate is another deferred attempt,
        // not permission for a new synchronous multi-reset connection loop.
        const bool accepted = wifi_is_connected() && !media_retry_network_cancelled() &&
                              sense_voice_prepare_first_attempt(job) && voice_upload_and_parse(job);
        if (accepted) {
          if (list_voice) g_voice_list_followup.store(true);
          media_retry_delivered();
          Serial.println("[VOICE_QUEUE] async accept complete; backend owns completion");
          if (job.from_voice_sd) {
            const bool deleted = sense_voice_spool_delete(job);
            uart_send_sense_diag("voice", deleted ? "sd_delivered" : "sd_delete_pending", "voice",
                                 (int32_t)job.job_id, deleted ? "durable_backend_ack" : "original_slot_kept");
          } else if (job.from_persisted) {
            upload_persist_delete_voice(job);
            upload_persist_note_event("retry_uploaded", job.mode, g_upload_persist_cached_count, job.retries);
            dump_system_truth("upload_retry_uploaded");
            Serial.printf("[UPLOAD_PERSIST] retry_uploaded job_id=%lu retries=%u cached=%u\n",
                          (unsigned long)job.job_id,
                          (unsigned)job.retries,
                          (unsigned)g_upload_persist_cached_count);
          }
        } else {
          if (park_cancelled_fresh("voice_cancelled")) continue;
          Serial.println("[VOICE_QUEUE] upload failed or backend rejected request");
          upload_persist_handle_failure(job, "voice_post_fail");
        }
        free(job.image_buf);
        upload_inflight = false;
        continue;
      }
      if (!sense_image_prepare_first_attempt(job)) {
        upload_persist_handle_failure(job, "image_identity_or_time");
        free(job.image_buf); upload_inflight = false; continue;
      }
      uint32_t job_start_ms = millis();
      uint32_t job_deadline_ms = job_start_ms + ACTION_AWAKE_BUDGET_MS;
      bool budget_exhausted = false;
      const bool is_check = scan_mode_is_check(job.mode);
      const bool is_discard = scan_mode_is_discard(job.mode);
      const bool is_dish = scan_mode_is_dish(job.mode);
      // Same budget for every mode. Dish used to get the long foreground deadline
      // because a user was waiting on its nutrition result; that feature is gone.
      uint32_t presign_deadline_ms = millis() + BACKGROUND_UPLOAD_PRESIGN_BUDGET_MS;
      (void)is_dish;
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

      const uint32_t backoff_ms[] = {500, 1500, 3500};
      // A saved copy already has an independent future retry. Keep this wake
      // bounded; the HTTP helper retains its own bounded server retry policy.
      const uint8_t max_retries = media_retry_network_active() ? 1 : 3;
      PresignReply upload_presign;
      bool presign_success = false;
      for (uint8_t attempt = 0; attempt < max_retries; ++attempt) {
        if (media_retry_network_cancelled()) break;
        if (!upload_wait_for_foreground_clear_in_place(job, "presign", presign_deadline_ms, &budget_exhausted)) {
          break;
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
          if (!media_retry_network_wait(backoff)) break;
          continue;
        }
        const char* expiry = (job.expiry_date[0] != '\0') ? job.expiry_date : NULL;
        presign_success = sense_image_get_presign(job, upload_presign, presign_deadline_ms);
        if (presign_success) {
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
        if (!media_retry_network_wait(backoff)) break;
      }
      if ((!presign_success || budget_exhausted) && park_cancelled_fresh("presign_cancelled")) continue;
      if (budget_exhausted) {
        if (is_dish && !media_retry_network_active()) {
          scan_ui_status_emit("ERROR", presign_error_text(), job.mode, job.job_id, true);
        }
        upload_persist_handle_failure(job, "presign_timeout");
        free(job.image_buf);
        upload_inflight = false;
        continue;
      }
      if (!presign_success) {
        if (is_dish && !media_retry_network_active()) {
          scan_ui_status_emit("ERROR", presign_error_text(), job.mode, job.job_id, true);
        }
        diag_record_action_event("upload", job.mode, "err", "presign", -1);
        upload_persist_handle_failure(job, "presign_fail");
        free(job.image_buf);
        upload_inflight = false;
        continue;
      }

      if (is_dish) {
        if (upload_presign.result_url.length() > 0) {
          Serial.printf("[RESULT_HTTP] result_url=%s\n", upload_presign.result_url.c_str());
        }
      }

      // Gate: park before PUT if user action is active (presign URL has 300s TTL)
      if (park_upload_job_if_foreground_active(job, "pre_put")) {
        upload_inflight = false;
        vTaskDelay(pdMS_TO_TICKS(40));
        continue;
      }

      uint32_t put_deadline_ms = is_dish ? job_deadline_ms : (millis() + BACKGROUND_UPLOAD_PUT_BUDGET_MS);
      bool upload_success = false;
      for (uint8_t attempt = 0; attempt < max_retries; ++attempt) {
        if (media_retry_network_cancelled()) break;
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
          if (!media_retry_network_wait(backoff)) break;
          continue;
        }
        bool aborted_for_budget = false;
        diag_note_stage("upload_put", 0);
        int put_http_code = 0;
        upload_success = upload_presign.upload_stored || put_to_presigned_url(upload_presign.put_url,
                                              job.image_buf,
                                              job.image_len,
                                              upload_presign.content_type.length() ? upload_presign.content_type.c_str() : "image/jpeg",
                                              job.job_id,
                                              put_deadline_ms,
                                              &aborted_for_budget,
                                              &upload_presign,
                                              &put_http_code);
        if (!upload_success && put_http_code == 412 && upload_presign.immutable_image) {
          // A lost success reply is not a second capture. Ask the backend to
          // verify the original object's checksum and length; retain on doubt.
          upload_success = sense_image_reconcile_stored(job, upload_presign, put_deadline_ms);
          if (!upload_success) break;
        }
        if (aborted_for_budget) {
          presign_set_error_text("Upload timeout");
          budget_exhausted = true;
          break;
        }
        if (upload_success) {
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
        if (!media_retry_network_wait(backoff)) break;
      }
      if (!upload_success && park_cancelled_fresh("put_cancelled")) continue;
      if (budget_exhausted) {
        if (is_dish && !media_retry_network_active()) {
          scan_ui_status_emit("ERROR", presign_error_text(), job.mode, job.job_id, true);
        }
        diag_record_action_event("upload", job.mode, "err", "timeout", -1);
        upload_persist_handle_failure(job, "put_timeout");
        free(job.image_buf);
        upload_inflight = false;
        continue;
      }
#if HALO_SPOOL_TEST
      if (g_test_fail_uploads > 0 && upload_success) {
        g_test_fail_uploads--;
        upload_success = false;
        Serial.println("[FAILUPLOAD] forcing this upload to FAIL (bench)");
      }
#endif
      if (!upload_success) {
        if (is_dish && !media_retry_network_active()) {
          scan_ui_status_emit("ERROR", presign_error_text(), job.mode, job.job_id, true);
        }
        diag_record_action_event("upload", job.mode, "err", "put_fail", -1);
        // Tell the LCD. Upload outcome was previously Sense-local: only sleep-path
        // upload events crossed the link, so neither the LCD's error log nor
        // anything watching the link could tell whether a user's capture actually
        // reached the backend. That is the single most important fact about a
        // capture after the shutter, and it was invisible.
        uart_send_sense_diag("upload", "put_fail", job.mode, (int32_t)job.job_id,
                             "upload_failed");
        // If this came off the SD spool, KEEP the slot: the card still holds the
        // only copy of the image.
        sense_spool_on_upload_result(job.job_id, false);
        if (g_cycle_uploads_fail < 0xFFFF) g_cycle_uploads_fail++;
        // Try to KEEP the photo before dropping it.
        //
        // This path previously went straight to free() with no persistence
        // attempt whatsoever — not SPIFFS, not the SD card. A failed upload
        // therefore destroyed the user's capture outright, which is the exact
        // loss the SD spool was built to prevent; the spool was only ever wired
        // into the sleep path. Verified on the bench: a forced upload failure
        // left `upload=0 q=0` at sleep entry and an empty card.
        //
        // upload_persist_handle_failure() tries SPIFFS first and falls back to
        // the LCD's SD card when the partition cannot hold the image — which for
        // a real capture is always (173,441 B partition, ~150 KB images needing
        // roughly double to write).
        if (!job.is_voice) {
          upload_persist_handle_failure(job, "put_fail");
        }
        free(job.image_buf);
        upload_inflight = false;
        continue;
      }

      diag_record_action_event("upload", job.mode, "ok", "put", 0);
      // Success is reported for the same reason as failure above: it is what
      // makes "did this photo get there?" answerable from the LCD side, and it
      // is the completion signal the SD-spool drain needs before it may delete
      // the only remaining copy of an image.
      uart_send_sense_diag("upload", "put_ok", job.mode, (int32_t)job.job_id,
                           job.from_persisted ? "replayed" : "direct");
      // Confirmed at the backend — only NOW may the SD slot be deleted. Deleting
      // at queue time would regress straight back to losing photos, since SPIFFS
      // still cannot hold a real capture.
      sense_spool_on_upload_result(job.job_id, true);
      if (g_cycle_uploads_ok < 0xFFFF) g_cycle_uploads_ok++;
      if (job.from_image_sd) {
        const bool deleted = sense_image_spool_delete(job);
        media_retry_delivered();
        uart_send_sense_diag("upload", deleted ? "sd_delivered" : "sd_delete_pending", job.mode,
                            (int32_t)job.job_id, deleted ? "durable_backend_ack" : "original_slot_kept");
      } else if (job.from_persisted) {
        upload_persist_delete();
        upload_persist_note_event("retry_uploaded", job.mode, g_upload_persist_cached_count, job.retries);
        dump_system_truth("upload_retry_uploaded");
        Serial.printf("[UPLOAD_PERSIST] retry_uploaded job_id=%lu retries=%u cached=%u\n",
                      (unsigned long)job.job_id,
                      (unsigned)job.retries,
                      (unsigned)g_upload_persist_cached_count);
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
  // Saved recovery yields to the user and never starts a blocking radio reset.
  // The owner loop and next retry wake handle association/time recovery.
  if (media_retry_network_active()) {
    return !media_retry_network_cancelled() && wifi_is_connected() && sense_time_has_fresh_sync();
  }
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

// (build_dish_result_url, dish_result_poll_delay_ms removed — see sense_upload_exec.h)
#include "sense_upload_exec.h"

// (wait_for_dish_result_http, get_presign_checkin, put_to_presigned_url,
//  uart_send_ui_meal_result deleted 2026-08-21 with the nutrition feature)

// ── UART Message Parsing ───────────────────────────────────────────
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
static bool sense_diag_accept_uart(JsonDocument& doc);
#endif
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
static bool halo_policy_one_shot_command(JsonDocument& doc);
static bool halo_policy_bench_command(JsonDocument& doc);
#endif
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
  
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  if(sense_diag_accept_uart(doc))return true;
#endif
  // Update last communication time (any message from LCD resets the timeout)
  last_lcd_communication = millis();
  last_uart_rx_ms = last_lcd_communication;
  
  const char* type = doc["type"] | "";
  if (!strcmp(type, "MEDIA_RETRY_ARM_ACK")) {
    media_retry_arm_ack(doc);
    return true;
  }
  Serial.printf("[PROTO] RX: type=%s\n", type);
  uart_rx_count++;
  uart_note_rx_type(type);
  bool is_user_input_message = (strncmp(type, "INPUT_", 6) == 0) &&
                               strcmp(type, "INPUT_SLEEP") != 0 &&
                               strcmp(type, "INPUT_PING") != 0;
  if (is_user_input_message) {
    const uint32_t in_msg_id = (uint32_t)(doc["msg_id"] | 0);

    // Ack BEFORE doing any work. The LCD retransmits on a 400ms timer, and some
    // of these handlers queue jobs or touch the camera; acking afterwards would
    // race the retry and generate duplicate traffic for no reason.
    uart_send_input_ack(in_msg_id);

    // Suppress the repeat ACTION for a msg_id we have already handled. The LCD
    // replays the original bytes when an ACK is lost, so this is what stops one
    // tap becoming two captures. The re-ack above still goes out — the LCD is
    // retrying precisely because it did not hear us.
    if (sense_input_seen_recently(in_msg_id)) {
      g_input_dupes_suppressed++;
      Serial.printf("[UART] duplicate %s msg_id=%lu suppressed (retransmit, total=%lu)\n",
                    type, (unsigned long)in_msg_id,
                    (unsigned long)g_input_dupes_suppressed);
      return true;
    }
    sense_input_mark_seen(in_msg_id);
    if (sense_user_action_cancels_flush(type)) {
      sense_note_admitted_user_action(sense_voice_list_input_compatible(type, g_list_screen_active));
      g_media_retry_user_paused.store(true);
      cancel_pending_sleep_for_user_action(type);
      // Yield background work only for a new user action, not a repeated ACK
      // request or an automatic firmware/status query.
      sense_spool_drain_yield_to_user();
    }
  }
  // Drain replies from the LCD (what is spooled / ready to stream).
  if (strcmp(type, "SPOOL_LIST") == 0) {
    sense_spool_on_list(doc);
    return true;
  }
  if (strcmp(type, "SPOOL_FETCH_READY") == 0) {
    sense_spool_on_fetch_ready(doc);
    return true;
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
  if (strncmp(type, "INPUT_", 6) == 0 &&
      strcmp(type, "INPUT_SLEEP") != 0 &&
      strcmp(type, "INPUT_PING") != 0) {
    last_user_activity_ms = millis();
  }
#else
  if (strncmp(type, "INPUT_", 6) == 0 &&
      strcmp(type, "INPUT_SLEEP") != 0 &&
      strcmp(type, "INPUT_PING") != 0) {
    last_user_activity_ms = millis();
  }
#endif
#ifdef HALO_SENSE_PROD_WRAPPER
  halo_prod_on_lcd_message(type);
#endif
  
  if (strcmp(type, "INPUT_USER_ACTIVE") == 0) {
    // A real wake-only tap/turn during a dark media retry needs priority but
    // must not initiate a shopping-list refresh or change the current screen.
    last_input_wake_ms = millis();
    last_user_activity_ms = last_input_wake_ms;
    return true;
  } else if (strcmp(type, "INPUT_WAKE") == 0) {
    unsigned long now_ms = millis();
    if (last_input_wake_ms > 0 && (now_ms - last_input_wake_ms) < 1500) {
      Serial.printf("[INPUT_WAKE] ignored debounce age_ms=%lu\n",
                    (unsigned long)(now_ms - last_input_wake_ms));
      // Debounce only the wake/holdoff churn -- DO NOT drop the list-refresh
      // request, or the LCD's INFLIGHT refresh hangs until its 20s watchdog.
      // request_list_refresh() answers safely (cached during cooldown / skips a
      // live inflight that will answer / enqueues otherwise) and never re-wakes.
      if (!lcd_ota_in_progress()) {
        request_list_refresh("input_wake_debounce", false);
      }
      return true;
    }
    last_input_wake_ms = now_ms;
    sleep_holdoff_until_ms = now_ms + SLEEP_HOLDOFF_MS;
    Serial.printf("[SLEEP_HOLDOFF] set until=%lu reason=input_wake\n", sleep_holdoff_until_ms);
    sleep_grace_until_ms = now_ms + MIN_AWAKE_BEFORE_SLEEP_MS;
    wake_requested = true;
    Serial.printf("[WAKE_GRACE] set until=%lu reason=input_wake\n", sleep_grace_until_ms);
    // Inflight handling lives in request_list_refresh(): it debounces a
    // genuinely-running fetch (which will answer the LCD when it completes)
    // and self-heals a stuck inflight flag instead of silently ignoring the
    // wake until reboot.
    Serial.println("[INPUT_WAKE] accepted");
    if (lcd_ota_in_progress()) {
      Serial.println("[INPUT_WAKE] list refresh deferred (lcd_ota_in_progress)");
    } else {
      Serial.println("[UART] LCD wake-up detected - requesting list refresh");
      request_list_refresh("input_wake", false);
    }
    last_wake_ms = now_ms;
  } else if (strcmp(type, "MAINT_WINDOW_ACK") == 0) {
    if ((!doc["remaining_s"].isUnbound() && !doc["remaining_s"].is<uint32_t>()) ||
        (!doc["wake_in_s"].isUnbound() && !doc["wake_in_s"].is<uint32_t>()) ||
        (!doc["clear"].isUnbound() && !doc["clear"].is<bool>()) ||
        (!doc["persisted"].isUnbound() && !doc["persisted"].is<bool>()) ||
        (!doc["request_id"].isUnbound() && !doc["request_id"].is<const char*>()) ||
        (!doc["status"].isUnbound() && !doc["status"].is<const char*>()) ||
        (!doc["start_epoch"].isUnbound() && !doc["start_epoch"].is<uint32_t>()) ||
        (!doc["duration_sec"].isUnbound() && !doc["duration_sec"].is<uint32_t>()) ||
        (!doc["grace_before_sec"].isUnbound() && !doc["grace_before_sec"].is<uint32_t>()) ||
        (!doc["grace_after_sec"].isUnbound() && !doc["grace_after_sec"].is<uint32_t>()) ||
        (!doc["coord_id"].isUnbound() && !doc["coord_id"].is<const char*>()) ||
        (!doc["peer_boot_id"].isUnbound() && !doc["peer_boot_id"].is<uint32_t>())) return false;
    const char* arm_coord = doc["coord_id"] | "";
    const char* arm_id = doc["request_id"] | "";
    const char* arm_status = doc["status"] | "";
    if (strlen(arm_id) >= 64 || strlen(arm_status) >= 24 || strlen(arm_coord) >= 40 ||
        (!doc["coord_id"].isUnbound() && !arm_coord[0])) return false;
    if (strcmp(arm_status, "stored_verified") == 0 && arm_coord[0] &&
        (!doc["remaining_s"].is<uint32_t>() || !doc["wake_in_s"].is<uint32_t>() ||
         !doc["persisted"].is<bool>() || !doc["start_epoch"].is<uint32_t>() ||
         !doc["duration_sec"].is<uint32_t>() || !doc["grace_before_sec"].is<uint32_t>() ||
         !doc["grace_after_sec"].is<uint32_t>() || !doc["peer_boot_id"].is<uint32_t>())) return false;
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
                               grace_after_sec, arm_coord, doc["peer_boot_id"] | (uint32_t)0);
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
#ifdef STRESS_TEST_NO_SLEEP
    Serial.println("[SLEEP] STRESS_TEST_NO_SLEEP — ignoring INPUT_SLEEP");
    uart_send_sleep_deny("stress_test", 60000);
    return true;
#endif
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
  } else if (strcmp(type, "INPUT_PROVISION_DIAG") == 0) {
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_provision_diag();
#else
    Serial.println("[PROVISION_DIAG] unavailable (no prod wrapper)");
#endif
  } else if (strcmp(type, "INPUT_PROVISION_RESTART_AP") == 0) {
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_provision_restart_ap();
#else
    Serial.println("[PROVISION_AP_RESTART] unavailable (no prod wrapper)");
#endif
  } else if (strcmp(type, "INPUT_PROVISION_CRYPTO_DIAG") == 0) {
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_provision_crypto_diag();
#else
    Serial.println("[PROVISION_CRYPTO] unavailable (no prod wrapper)");
#endif
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
  } else if (strcmp(type, "INPUT_SENSE_FW") == 0) {
    // Fast path for the LCD Settings screen: it only needs the Sense fw version
    // (known instantly) and ignores lcd_fw. Reply IMMEDIATELY with the cached
    // LCD version — do NOT run the blocking sense_lcd_ota_query() that
    // INPUT_FW_INFO uses (which can stall for several seconds).
    const char* cached_lcd_fw = (g_lcd_ota_version[0] != '\0') ? g_lcd_ota_version : "unknown";
    Serial.printf("[UART] INPUT_SENSE_FW received - fast FW_INFO (cached lcd_fw=%s)\n",
                  cached_lcd_fw);
    uart_send_fw_info(false);
  } else if (strcmp(type, "INPUT_OTA_CHECK") == 0) {
    const char* reason = doc["reason"] | "manual";
    Serial.printf("[UART] INPUT_OTA_CHECK received reason=%s\n", reason);
    // LCD owns this action's persistent update overlay. A generic transient
    // UI_STATUS would remain queued behind it and replay after the result.
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_request_manual_ota(reason);
#else
    Serial.println("[UART] INPUT_OTA_CHECK ignored (no prod wrapper)");
#endif
  } else if (strcmp(type, "INPUT_MAINT_TEST") == 0) {
    uint32_t duration = doc["duration_sec"] | 600;
    Serial.printf("[UART] INPUT_MAINT_TEST received duration_sec=%lu\n", (unsigned long)duration);
#ifdef HALO_SENSE_PROD_WRAPPER
    halo_prod_request_maint_test(duration);
#else
    Serial.println("[UART] INPUT_MAINT_TEST ignored (no prod wrapper)");
#endif
  } else if (strcmp(type, "INPUT_WIFI_SCAN") == 0) {
    // WiFi network scan — reports all visible APs with RSSI via SENSE_DIAG
    Serial.println("[UART] INPUT_WIFI_SCAN received");
    uart_send_sense_diag("wifi", "scan_start", "scanning", 0, "starting_scan");

    // Disconnect temporarily if connected to get clean scan
    bool was_connected = wifi_is_connected();
    int8_t pre_rssi = was_connected ? (int8_t)WiFi.RSSI() : 0;

    int n = WiFi.scanNetworks(false, true);  // sync scan, show hidden
    char detail_buf[64];
    snprintf(detail_buf, sizeof(detail_buf), "found=%d was_connected=%d pre_rssi=%d", n, was_connected ? 1 : 0, pre_rssi);
    uart_send_sense_diag("wifi", "scan_result", "count", n, detail_buf);

    for (int i = 0; i < n && i < 20; i++) {
      int32_t rssi = WiFi.RSSI(i);
      char ap_detail[96];
      snprintf(ap_detail, sizeof(ap_detail), "ssid=%s rssi=%ld ch=%d enc=%d",
               WiFi.SSID(i).c_str(), (long)rssi, WiFi.channel(i), (int)WiFi.encryptionType(i));
      char label_buf[33];
      strncpy(label_buf, WiFi.SSID(i).c_str(), 32);
      label_buf[32] = '\0';
      uart_send_sense_diag("wifi", "scan_ap", label_buf, rssi, ap_detail);
    }
    WiFi.scanDelete();
    uart_send_sense_diag("wifi", "scan_done", "complete", n, "scan_complete");

  } else if (strcmp(type, "INPUT_WIFI_TEST") == 0) {
    SenseBackupWifiCall backup_call; if (!backup_call) return false;
    // WiFi cold-start test — disconnect, scan, reconnect with detailed timing
    Serial.println("[UART] INPUT_WIFI_TEST received");
    uart_send_sense_diag("wifi", "test_start", "starting", 0, "cold_start_test");

    // Step 1: Record current state
    bool was_connected = wifi_is_connected();
    int8_t pre_rssi = was_connected ? (int8_t)WiFi.RSSI() : 0;
    char state_detail[48];
    snprintf(state_detail, sizeof(state_detail), "connected=%d rssi=%d status=%d",
             was_connected ? 1 : 0, pre_rssi, (int)WiFi.status());
    uart_send_sense_diag("wifi", "test_pre_state", "initial", pre_rssi, state_detail);

    // Step 2: Disconnect
    unsigned long t0 = millis();
    WiFi.disconnect(true);  // disconnect and turn off radio
    delay(500);
    uart_send_sense_diag("wifi", "test_disconnected", "radio_off", 0, "wifi_off");

    // Step 3: Scan (with radio back on)
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true);
    unsigned long t_scan = millis() - t0;
    char scan_detail[64];
    snprintf(scan_detail, sizeof(scan_detail), "found=%d scan_ms=%lu", n, t_scan);
    uart_send_sense_diag("wifi", "test_scan", "scan_done", n, scan_detail);

    // Report target AP RSSI
    for (int i = 0; i < n; i++) {
      int32_t rssi = WiFi.RSSI(i);
      char ap_detail[96];
      snprintf(ap_detail, sizeof(ap_detail), "ssid=%s rssi=%ld ch=%d",
               WiFi.SSID(i).c_str(), (long)rssi, WiFi.channel(i));
      char label_buf[33];
      strncpy(label_buf, WiFi.SSID(i).c_str(), 32);
      label_buf[32] = '\0';
      uart_send_sense_diag("wifi", "test_scan_ap", label_buf, rssi, ap_detail);
    }
    WiFi.scanDelete();

    // Step 4: Reconnect with timing
    unsigned long t_begin = millis();
    uart_send_sense_diag("wifi", "test_connecting", "begin", 0, "calling_wifi_begin");
    WiFi.begin();  // Uses stored credentials

    // Poll for connection with 30s timeout, report progress every 2s
    bool connected = false;
    for (int attempt = 0; attempt < 60; attempt++) {  // 30s max (500ms * 60)
      delay(500);
      wl_status_t st = WiFi.status();
      unsigned long elapsed = millis() - t_begin;

      if (st == WL_CONNECTED) {
        connected = true;
        int8_t rssi = (int8_t)WiFi.RSSI();
        char conn_detail[80];
        snprintf(conn_detail, sizeof(conn_detail), "rssi=%d ip=%s connect_ms=%lu total_ms=%lu",
                 rssi, WiFi.localIP().toString().c_str(), elapsed, millis() - t0);
        uart_send_sense_diag("wifi", "test_connected", "success", rssi, conn_detail);
        break;
      }

      // Report progress every 2s
      if (attempt % 4 == 3) {
        char prog_detail[48];
        snprintf(prog_detail, sizeof(prog_detail), "status=%d elapsed_ms=%lu", (int)st, elapsed);
        uart_send_sense_diag("wifi", "test_progress", "waiting", (int)st, prog_detail);
      }
    }

    if (!connected) {
      unsigned long total_ms = millis() - t0;
      char fail_detail[48];
      snprintf(fail_detail, sizeof(fail_detail), "status=%d total_ms=%lu", (int)WiFi.status(), total_ms);
      uart_send_sense_diag("wifi", "test_failed", "timeout", (int)WiFi.status(), fail_detail);
    }

    uart_send_sense_diag("wifi", "test_done", connected ? "pass" : "fail", connected ? 1 : 0, "test_complete");

  } else if (strcmp(type, "INPUT_MENU_SELECT") == 0) {
    // Menu item selected on LCD
    const char* menu_item = doc["menu_item"] | "";
    int menu_index = doc["menu_index"] | -1;
    const char* requested_mode = "";
    Serial.printf("[UART] Menu item selected: %s (index %d)\n", menu_item, menu_index);
    // Start the Sense half of the capture trace, keyed on the LCD's msg_id so
    // both boards' logs correlate on the same id.
    sense_captrace_begin((uint32_t)(doc["msg_id"] | 0), menu_item);
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
      Serial.printf("[SCAN] ignore menu select item=%s requested_mode=%s current_mode=%s state=%d upload=%d q=%lu user_state=%s device_state=%s\n",
                    menu_item,
                    requested_mode,
                    current_job.mode,
                    (int)current_job.state,
                    upload_inflight ? 1 : 0,
                    (unsigned long)upload_queue_count(),
                    sense_user_state_name(),
                    sense_device_state_name());
      uart_send_ui_toast("Capture already in progress");
      return true;
    }
    
    // Handle "Dish" selection. Dish is a plain capture-and-log, identical to
    // check-in and discard -- the nutrition feature it used to feed is gone.
    if (strcmp(menu_item, "Dish") == 0) {
      Serial.println("[UART] Dish selected - queuing SCAN operation");
      if (op_queue != NULL) {
        OpJob job = {OP_SCAN, PRI_USER, get_next_msg_id(), millis(), OP_IDLE, false, "", ""};
        strncpy(job.mode, "dish", sizeof(job.mode) - 1);
        job.expiry_date[0] = '\0';
        job.quantity = 1;
        job.add_to_shopping_list = false;
        if (enqueue_op_job(job, true, "menu_select_dish")) {
          Serial.println("[OP] SCAN job queued (dish mode)");
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
    // Proactively push the Sense firmware version on every (re)sync. The LCD
    // re-syncs right after its OTA reboot, so this is what delivers the new
    // sense_fw to the LCD promptly (it never has to query). Fast, non-blocking
    // (do_lcd_query=false): reports sense_fw + cached lcd_fw, no LCD round-trip.
    uart_send_fw_info(false);
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
  } else if (strcmp(type, "INPUT_TEST_ERRORS") == 0) {
    Serial.println("[TEST] Injecting test errors via diag_record_error_persistent...");
    diag_record_error_persistent("camera", -1, "test: init failed after 2 attempts");
    delay(50);  // small delay between UART sends to avoid buffer overflow
    diag_record_error_persistent("wifi", -3, "test: connect timeout after 30s");
    delay(50);
    diag_record_error_persistent("upload", 403, "test: PUT rejected by server");
    delay(50);
    diag_record_error_persistent("voice", -2, "test: i2s recording failed");
    Serial.println("[TEST] 4 Sense test errors sent to LCD via UART");
  // ── LCD OTA proxy mailbox handlers ─────────────────────────────────
  // These messages are responses from the LCD board during OTA proxy
  // sessions. The proxy task polls the mailbox flags instead of reading
  // lcdSerial directly, avoiding UART contention with this main loop.
  } else if (strcmp(type, "OTA_PEER_READY") == 0) {
#ifdef HALO_SENSE_PROD_WRAPPER
    if (!doc["schedule_id"].is<const char*>() || !doc["peer_boot_id"].is<uint32_t>() ||
        !doc["origin_wake"].is<int>() || !doc["origin_reset"].is<int>() ||
        !doc["origin_epoch"].is<uint32_t>() || !doc["resumed"].is<bool>()) return false;
    halo_prod_note_lcd_timer(doc["schedule_id"] | "", doc["peer_boot_id"] | (uint32_t)0,
                             doc["origin_wake"] | -1, doc["origin_reset"] | -1,
                             doc["origin_epoch"] | (uint32_t)0, doc["resumed"] | false);
#endif
  } else if (strcmp(type, "LCD_OTA_QUERY_RESP") == 0) {
    // Clear acceptance first: malformed present fields cannot inherit a
    // previous reply or silently fall back to the uncorrelated legacy path.
    g_lcd_ota_query_resp_ready = false;
    g_lcd_query_boot_ready = false;
    g_lcd_query_recovery_idle = false;
    if (!doc["lcd_fw"].is<const char*>() || !doc["ota_part_size"].is<uint32_t>() ||
        (!doc["running_part"].isUnbound() && !doc["running_part"].is<const char*>()) ||
        (!doc["running_state"].isUnbound() && !doc["running_state"].is<const char*>()) ||
        (!doc["boot_part"].isUnbound() && !doc["boot_part"].is<const char*>()) ||
        (!doc["boot_ready"].isUnbound() && !doc["boot_ready"].is<bool>()) ||
        (!doc["coord_id"].isUnbound() && !doc["coord_id"].is<const char*>()) ||
        (!doc["peer_boot_id"].isUnbound() && !doc["peer_boot_id"].is<uint32_t>()) ||
        (!doc["coord_waiting"].isUnbound() && !doc["coord_waiting"].is<bool>()) ||
        (!doc["coord_owner"].isUnbound() && !doc["coord_owner"].is<const char*>()) ||
        (!doc["coord_lease_ms"].isUnbound() && !doc["coord_lease_ms"].is<uint32_t>())) return false;
    const char* fw = doc["lcd_fw"] | "";
    uint32_t part_size = doc["ota_part_size"] | (uint32_t)0;
    strncpy(g_lcd_ota_query_resp_fw, fw, sizeof(g_lcd_ota_query_resp_fw) - 1);
    g_lcd_ota_query_resp_fw[sizeof(g_lcd_ota_query_resp_fw) - 1] = '\0';
    g_lcd_ota_query_resp_part_size = part_size;
    // Fresh boot-proof fields. Older LCD firmware may omit them; defaults
    // must never borrow a previous response's VALID/ready state.
    const char* running_part  = doc["running_part"]  | "";
    const char* running_state = doc["running_state"] | "UNKNOWN";
    const char* boot_part     = doc["boot_part"]     | "";
    if (strlen(fw) >= sizeof(g_lcd_ota_query_resp_fw) ||
        strlen(running_part) >= sizeof(g_lcd_query_running_part) ||
        strlen(running_state) >= sizeof(g_lcd_query_running_state) ||
        strlen(boot_part) >= sizeof(g_lcd_query_boot_part)) return false;
    strncpy(g_lcd_query_running_part, running_part, sizeof(g_lcd_query_running_part) - 1);
    g_lcd_query_running_part[sizeof(g_lcd_query_running_part) - 1] = '\0';
    strncpy(g_lcd_query_running_state, running_state, sizeof(g_lcd_query_running_state) - 1);
    g_lcd_query_running_state[sizeof(g_lcd_query_running_state) - 1] = '\0';
    strncpy(g_lcd_query_boot_part, boot_part, sizeof(g_lcd_query_boot_part) - 1);
    g_lcd_query_boot_part[sizeof(g_lcd_query_boot_part) - 1] = '\0';
    g_lcd_query_boot_ready = doc["boot_ready"] | false;
    // Per-response freshness: an omitted field must not inherit an earlier
    // correlated reply or another LCD boot's readiness window.
    const char* coord_id = doc["coord_id"] | "";
    if (!doc["coord_id"].isUnbound() && !coord_id[0]) return false;
    if (strlen(coord_id) >= sizeof(g_lcd_query_coord_id)) coord_id = "!overlength";
    strncpy(g_lcd_query_coord_id, coord_id, sizeof(g_lcd_query_coord_id) - 1);
    g_lcd_query_coord_id[sizeof(g_lcd_query_coord_id) - 1] = '\0';
    g_lcd_query_peer_boot_id = doc["peer_boot_id"] | (uint32_t)0;
    g_lcd_query_coord_waiting = doc["coord_waiting"] | false;
    const char* owner = doc["coord_owner"] | "";
    if (strlen(owner) >= sizeof(g_lcd_query_coord_owner)) owner = "!overlength";
    strlcpy(g_lcd_query_coord_owner, owner, sizeof(g_lcd_query_coord_owner));
    g_lcd_query_coord_lease_ms = doc["coord_lease_ms"] | (uint32_t)0;
    g_lcd_query_recovery_idle = doc["recovery_idle"].is<bool>() && (doc["recovery_idle"] | false);
    g_lcd_ota_query_resp_ready = true;
    (void)sense_lcd_query_proof_emit(Serial, coord_id, g_lcd_query_peer_boot_id,
        fw, running_part, boot_part, running_state, g_lcd_query_boot_ready, part_size);
  } else if (strcmp(type, "LCD_OTA_BEGIN_ACK") == 0) {
    if (!doc["accepted"].is<bool>() ||
        (!doc["json_ready"].isUnbound() && !doc["json_ready"].is<bool>()) ||
        (!doc["session_id"].isUnbound() && !doc["session_id"].is<uint16_t>()) ||
        (!doc["resume_offset"].isUnbound() && !doc["resume_offset"].is<uint32_t>()) ||
        (!doc["ota_proto"].isUnbound() && (!doc["ota_proto"].is<uint8_t>() ||
          ((doc["ota_proto"] | 0) != 1 && (doc["ota_proto"] | 0) != OTA_UART_PROTOCOL_VERSION)))) return false;
    bool accepted = doc["accepted"] | false;
    const char* reason = doc["reason"] | "unknown";
    uint32_t resume_offset = doc["resume_offset"] | (uint32_t)0;
    g_lcd_ota_begin_ack_accepted = accepted;
    g_lcd_ota_begin_ack_json_ready = doc["json_ready"] | false;
    strncpy(g_lcd_ota_begin_ack_reason, reason, sizeof(g_lcd_ota_begin_ack_reason) - 1);
    g_lcd_ota_begin_ack_reason[sizeof(g_lcd_ota_begin_ack_reason) - 1] = '\0';
    g_lcd_ota_begin_ack_resume_offset = resume_offset;
    g_lcd_ota_begin_ack_proto = doc["ota_proto"] | 1;
    g_lcd_ota_begin_ack_session = doc["session_id"] | (uint16_t)0;
    g_lcd_ota_begin_ack_ready = true;
    Serial.printf("[UART] LCD_OTA_BEGIN_ACK accepted=%d reason=%s resume=%lu\n",
                  accepted ? 1 : 0, reason, (unsigned long)resume_offset);
  } else if (strcmp(type, "LCD_OTA_END_ACK") == 0) {
    bool sha_match = doc["sha_match"] | false;
    bool ota_ok    = doc["ota_ok"] | false;
    g_lcd_ota_end_ack_sha_match = sha_match;
    g_lcd_ota_end_ack_ota_ok    = ota_ok;
    g_lcd_ota_end_ack_ready     = true;
    Serial.printf("[UART] LCD_OTA_END_ACK sha_match=%d ota_ok=%d\n", sha_match ? 1 : 0, ota_ok ? 1 : 0);
  } else if (strcmp(type, "LCD_OTA_STATUS") == 0) {
    uint8_t pct = doc["progress"] | (uint8_t)0;
    const char* phase = doc["phase"] | "";
    Serial.printf("[UART] LCD_OTA_STATUS phase=%s progress=%u%%\n", phase, pct);
  } else if (strcmp(type, "LCD_OTA_ABORT") == 0) {
    const char* reason = doc["reason"] | "unknown";
    Serial.printf("[UART] LCD_OTA_ABORT reason=%s\n", reason);
  } else if (strcmp(type, "LIST_ACTIVE") == 0) {
    // LCD asserts this while the user is on the shopping-list screen. While
    // active we pin the Sense awake (sleep gate) and keep WiFi up so list
    // refresh/delete hit a live connection instantly. The LCD re-asserts
    // state=1 every few seconds; staleness watchdog in loop() clears it if the
    // LCD goes silent (e.g. crash) so a stuck flag can't pin us awake forever.
    int state = doc["state"] | 0;
    unsigned long now_ms = millis();
    if (state == 1) {
      // Rising edge = list entry (false -> true). The LCD re-asserts state=1
      // every ~3s; only nudge WiFi on the transition so we don't re-kick the
      // connection repeatedly (which would prolong wifi_connect_inflight / the
      // current op). While already list-active + awake, g_list_screen_active
      // keeps WiFi up by blocking the pre-sleep teardown — no re-kick needed.
      bool rising_edge = !g_list_screen_active;
      g_list_screen_active = true;
      last_list_active_ms = now_ms;
      // Extend awake grace/holdoff like INPUT_WAKE so idle sleep doesn't fire.
      sleep_holdoff_until_ms = now_ms + SLEEP_HOLDOFF_MS;
      sleep_grace_until_ms = now_ms + MIN_AWAKE_BEFORE_SLEEP_MS;
      if (rising_edge) {
        // Kick off WiFi bring-up non-blockingly on list entry only.
        // service_wifi_maintenance() in loop() services the connection; we just
        // nudge it here so a fetch has a live link ready. Skip the blocking
        // wait (timeout_ms=0) so this UART handler never stalls.
        if (WiFi.status() != WL_CONNECTED) {
          ensure_wifi_connected("list_active", 0);
        }
        Serial.println("[LIST_ACTIVE] state=1 (rising_edge)");
      } else {
        Serial.println("[LIST_ACTIVE] state=1 (refresh)");
      }
    } else {
      if (g_list_screen_active) sense_note_admitted_user_action();
      g_list_screen_active = false;
      Serial.println("[LIST_ACTIVE] state=0");
    }
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
  } else if (strcmp(type, "OTA_BENCH") == 0) {
    return halo_policy_bench_command(doc);
  } else if (strcmp(type, "OTA_ONE_SHOT") == 0) {
    return halo_policy_one_shot_command(doc);
#endif
  } else if (strcmp(type, "OTA_CHECK") == 0) {
    // On-demand OTA check.
    //
    // The LCD has forwarded this message for a long time and the Sense answered
    // "[PROTO] Unknown type: OTA_CHECK" -- it was never handled. That went
    // unnoticed because the on-demand path used to live in MQTT
    // (MQTT_CMD_OTA_CHECK), and MQTT was deleted in 6.2.0, so the ONLY remaining
    // trigger was the nightly timer wake. There was no way to ask a device to
    // check for an update, which matters for support and for testing.
    //
    // Policy still decides. allowOtaWorkNow() enforces the maintenance-only
    // design, so this REQUESTS a check rather than forcing one; it does not
    // become a way to start a download while the user is mid-capture.
#ifdef HALO_SENSE_PROD_WRAPPER
    // Routed through a wrapper-provided function: this file is #included BEFORE
    // SenseOtaPolicy.h and g_ota_check_requested exist, so it cannot touch them
    // directly.
    halo_request_ota_check(doc["reason"] | "uart_cmd", doc["allow_reboot"] | false);
#else
    Serial.println("[OTA_CHECK] ignored (not a prod build)");
#endif
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
        
        // Start I2S audio recording (max duration, but will stop on INPUT_LONG_PRESS_END)
        Serial.println("[OP_WORKER] VOICE: Starting I2S audio recording...");
        const uint32_t MAX_RECORD_MS = 10000;
        audio_start_recording_ms(MAX_RECORD_MS);
        voice_begin_wifi_preconnect();
        if (voice_finalize_requested) {
          current_job.state = OP_FINALIZE;
        }

        // Wait for recording to complete (will be signaled by INPUT_LONG_PRESS_END)
        // For now, record for max 10 seconds or until signaled
        uint32_t record_start = millis();
        
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
        
        // Brief yield to let any in-flight callback invocation complete
        vTaskDelay(pdMS_TO_TICKS(10));
        
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
          { // Bridge voice recording diagnostics to LCD
            char vdetail[128];
            snprintf(vdetail, sizeof(vdetail), "ms=%lu bytes=%u peak=%u avg=%lu nz=%lu%%",
                     record_ms, (unsigned)voice_audio_size,
                     (unsigned)voice_peak_abs, avg_abs, nonzero_pct);
            uart_send_sense_diag("voice", "record_done", "recording", (int32_t)voice_audio_size, vdetail);
          }
          
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
                last_user_activity_ms = millis();  // Extend foreground parking window for voice upload
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
            diag_record_error_persistent("camera_init", -1, "init_failed");
            uart_send_sense_diag("camera", "scan_init_fail", job.mode, -1, "op_worker");
            diag_record_action_event("scan", job.mode, "err", "camera_init", -1);
            scan_send_terminal_status("ERROR", "Camera init failed", job.mode, true);
            current_job.state = OP_DONE;
          } else {
            diag_note_stage("camera_capture", 0);
            if (!warmup_and_capture(fb, true)) {
              Serial.println("[OP_WORKER] SCAN: Camera capture FAILED");
              deinit_camera();
              diag_record_error_persistent("camera_capture", -1, "capture_failed");
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
#if HALO_CAMERA_KEEP_INIT
              Serial.println("[OP_WORKER] SCAN: camera KEPT initialised (deinit deferred to sleep)");
#else
              Serial.println("[OP_WORKER] SCAN: Powering down camera after capture...");
              deinit_camera();
#endif
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
              scan_ui_status_emit("DONE", "Logged!", job.mode, job.job_id, false);
              flow_step(job.job_id, "DONE_UI");
              scan_ui_inflight_set(false, "ui_done");
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
            diag_record_error_persistent("camera_init", -1, "init_failed");
            uart_send_sense_diag("camera", "scan_init_fail", job.mode, -1, "op_worker");
            diag_record_action_event("scan", job.mode, "err", "camera_init", -1);
            scan_send_terminal_status("ERROR", "Camera init failed", job.mode, true);
            current_job.state = OP_DONE;
          } else {
            diag_note_stage("camera_capture", 0);
            if (!warmup_and_capture(fb, true)) {
              Serial.println("[OP_WORKER] SCAN: Camera capture FAILED");
              deinit_camera();
              diag_record_error_persistent("camera_capture", -1, "capture_failed");
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
#if HALO_CAMERA_KEEP_INIT
                Serial.println("[OP_WORKER] SCAN: camera KEPT initialised (deinit deferred to sleep)");
#else
                Serial.println("[OP_WORKER] SCAN: Powering down camera after capture...");
                deinit_camera();
#endif
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

            }
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
              scan_ui_status_emit("DONE", "Logged!", job.mode, job.job_id, false);
              flow_step(job.job_id, "DONE_UI");
              scan_ui_inflight_set(false, "ui_done");
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
        scan_ui_inflight_set(false, "scan_complete");
        if (is_dish_mode) {
          dish_scan_inflight_set(false, "scan_complete");
        }
        // Sense_Minimal/Sense_Minimal.ino: op_worker_task (SCAN exit)
        Serial.println("[OP_WORKER] SCAN operation complete");
      } else if (job.type == OP_LIST_REFRESH) {
        unsigned long deq_ms = millis();
        Serial.printf("[LIST_REFRESH] dequeued queue_wait_ms=%lu\n",
                      (unsigned long)(deq_ms - job.created_ts));
        // Awake-proof before the fetch: the wake-path IDLE in setup() only
        // fires when the Sense was actually deep-sleeping, and PONG/SYNC_ACK
        // timing is LCD-dependent — nothing guarantees the LCD's refresh SM
        // left REFRESH_WAKE_PENDING before a warm-WiFi fetch (~400ms) lands
        // UI_LIST. One cheap UART line makes the ordering deterministic.
        uart_send_ui_status("IDLE");
        Serial.println("[LIST_REFRESH] awake_proof_sent path=fetch");
        // The parser publishes exactly once after a valid backend response.
        // Failed fetches retain the cache and must not turn boot count=0 into UI_LIST[].
        const auto fetch_result = voice_list_pending() || g_media_voice_list_mode.load()
            ? ListRequestResult::Deferred : fetch_shopping_list_from_api();
        const bool fetch_ok = fetch_result == ListRequestResult::Completed;
        Serial.printf("[LIST_REFRESH] total_ms=%lu fetch_ms=%lu ok=%d\n",
                      (unsigned long)(millis() - job.created_ts),
                      (unsigned long)(millis() - deq_ms), fetch_ok ? 1 : 0);
        if (fetch_result == ListRequestResult::Deferred) {
          list_transport_refresh_deferred = true;
          list_refresh_inflight = false;
          list_refresh_start_ms = 0;
        } else {
          list_refresh_mark_complete(fetch_ok ? "done" : "failed");
        }
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
  g_sense_main_task_handle = xTaskGetCurrentTaskHandle();
  // Must precede halo_prod_pre_setup(): that hook classifies timer boots for
  // OTA. Only an actually selected media timer has this separate purpose.
  g_media_retry_timer_boot = esp_reset_reason() == ESP_RST_DEEPSLEEP &&
                            esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER &&
                            g_media_retry_timer_selected;
  g_media_retry_timer_selected = false;
  Serial.printf("[BOOT_FLOW] stage=setup_enter t=%lu heap=%u min_heap=%u\n",
                millis(),
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap());
  print_wakeup_diagnostics(HALO_BOARD_NAME);
  // Set TZ before anything computes a local time. Nothing set it previously, so
  // the process TZ was whatever the runtime happened to leave — observed
  // flipping between PST8PDT and UTC0DST0 between two calls seconds apart. The
  // Validate the crash-forensics RTC block before anything reads or writes it.
  // Must precede diag_record_crash_boot() and wakelog_begin_cycle(), both of
  // which consume these values.
  rtc_diag_init();

  // nightly maintenance wake is defined in LOCAL time, so an unset TZ moves it
  // by whole hours. Backend-supplied zone will override this at owner-claim.
  // Prefer the zone the backend gave us at claim time; fall back to the compiled
  // default. Persisted in NVS, so it survives reboots and deep sleep and is
  // applied on EVERY boot — not just the one where the claim happened, which is
  // the only boot that would otherwise have the right local time.
  {
    char saved_tz[64];
    if (ProvisioningState::loadTimezone(saved_tz, sizeof(saved_tz)) && saved_tz[0]) {
      sense_set_timezone(saved_tz);
    } else {
      sense_set_timezone(nullptr);   // default until the backend supplies one
    }
  }
  // Open a wake-cycle record. Early, so a crash later in setup() still leaves an
  // un-closed entry that the next boot reports as "NEVER SLEPT".
  wakelog_begin_cycle(sense_now_epoch(), (uint8_t)esp_reset_reason());
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
  if (reset_reason == ESP_RST_BROWNOUT ||
      reset_reason == ESP_RST_INT_WDT ||
      reset_reason == ESP_RST_TASK_WDT ||
      reset_reason == ESP_RST_PANIC) {
    char detail[64];
    snprintf(detail, sizeof(detail), "reset=%d stage=%s", (int)reset_reason, g_rtc_last_stage);
    diag_record_error_persistent("boot", (int32_t)reset_reason, detail);
  }
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
  if (reset_reason == ESP_RST_PANIC ||
      reset_reason == ESP_RST_TASK_WDT ||
      reset_reason == ESP_RST_INT_WDT ||
      reset_reason == ESP_RST_WDT) {
    g_upload_persist_replay_not_before_ms = millis() + UPLOAD_PERSIST_VOICE_REPLAY_BACKOFF_MS;
    Serial.printf("[UPLOAD_PERSIST] panic_backoff until_ms=%lu reset_reason=%s\n",
                  g_upload_persist_replay_not_before_ms,
                  reset_reason_label(reset_reason));
  }
#endif
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
    // Init UART and announce to LCD — LCD may be waiting for us
    initUarts();
    uart_send_ui_status("IDLE");
    last_lcd_communication = millis();
    // Check if LCD is actively trying to talk to us
    unsigned long lcd_check_start = millis();
    bool lcd_active = false;
    while (millis() - lcd_check_start < 2000) {
      if (lcdSerial.available()) {
        lcd_active = true;
        Serial.println("[TIMER_WAKE] LCD active — staying awake");
        break;
      }
      delay(50);
    }
    if (!g_media_retry_timer_boot) ota_on_timer_wake();
    else Serial.println("[MEDIA_RETRY] timer_wake purpose=saved_uploads");
    // A timer wake is the one boot with no user waiting and no camera init, so
    // it is where the SD spool gets emptied. Whether there is anything to empty
    // is answered by the probe once the link and WiFi are up.
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
    // Deliberately NOT gated on lcd_active.
    //
    // It was, and that silently disabled the whole feature: lcd_active only
    // means the LCD said something within 2s of boot, which it does on virtually
    // every wake. Every 300s drain wake fired, stayed up 13-15s, drained nothing
    // and logged drain=- because the flag was never set. "The LCD is talking" is
    // not "a person is using the device" -- the real guard for that is a capture
    // or foreground activity, which sense_spool_drain_wake_active() checks and
    // which releases the wake immediately.
    // Gated on the feature actually being ON. It was not, and this still armed
    // on EVERY timer wake -- including the nightly 02:00 one -- so the device
    // held itself awake for the full 90s drain budget and recovered nothing:
    //   detail="awake=105070ms timer=150s cap=0 ok=0 spool=0 drain=budget"
    // Five for five on the 2026-09-02 unplugged test. On a mostly-off device
    // that is ~95s of pointless radio-on every single night, and it delays the
    // nightly update check that the wake actually exists to run.
#if SPOOL_DRAIN_WAKE_ENABLED
    g_spool_drain_wake = true;
    g_spool_drain_wake_start_ms = millis();
    g_spool_drain_wake_ok_at_start = g_spool_drained_ok;
    Serial.printf("[SPOOL_DRAIN] timer wake - eligible to drain the SD spool (lcd_active=%d)\n",
                  lcd_active ? 1 : 0);
#endif
#endif
  } else {
    Serial.println("[SENSE] Cold boot");
  }
  g_timer_wake_armed = 0;
  
  // Configure wake GPIO for polling (to detect wake pulses even when already awake)
  gpio_set_direction(WAKE_GPIO, GPIO_MODE_INPUT);
  gpio_pullup_en(WAKE_GPIO);
  gpio_pulldown_dis(WAKE_GPIO);
  delay(200);  // Grace period: let LCD GPIO39 stabilize after boot/restart
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
  if (op_queue == NULL || ui_event_queue == NULL) {
    Serial.println("ERROR: Failed to create operation queues!");
  } else {
    Serial.println("[SETUP] Operation queues created");
  }
  if (upload_queue == NULL) {
    Serial.println("ERROR: Failed to create upload queue!");
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
  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED && sense_uart_ordinary_tx_allowed()) {
    Serial.println("[SENSE] Initial boot - sending AWAKE to LCD");
    Serial.printf("[BOOT_FLOW] stage=awake_tx_begin t=%lu\n", millis());
    lcdSerial.println("AWAKE");
    lcdSerial.flush();
    Serial.printf("[BOOT_FLOW] stage=awake_tx_done t=%lu\n", millis());
  }

  // Register Wi-Fi event handler for connect guard state
  Serial.printf("[BOOT_FLOW] stage=wifi_event_register_begin t=%lu\n", millis());
  WiFi.onEvent(handle_wifi_event);
  wifi_diag_reset();
  Serial.printf("[BOOT_FLOW] stage=wifi_event_register_done t=%lu\n", millis());

  // Reserve a contiguous DMA block for camera before WiFi fragments the heap.
  // WiFi.begin() allocates ~30-50KB in internal DMA SRAM; those allocations can
  // split the heap so no contiguous 16KB block remains for esp_camera_init().
  // By reserving first, WiFi allocates *around* this block, not *through* it.
  // Released in init_camera(), re-reserved in deinit_camera().
  if (camera_dma_reserve_acquire("setup")) {
    Serial.printf("[SETUP] Camera DMA reserved %u bytes\n", (unsigned)CAMERA_DMA_RESERVE_BYTES);
  } else if (!g_camera_dma_provisioning.load()) {
    Serial.println("[SETUP] WARNING: Camera DMA reservation failed");
  }

  // Start Wi-Fi immediately (non-blocking). The ESP32 WiFi stack runs on
  // core 0 in a FreeRTOS task — calling WiFi.begin() doesn't block the
  // main loop or interfere with camera capture on core 1.
  // IMPORTANT: Skip if device needs provisioning — the ProvisioningManager
  // owns WiFi mode (AP_STA) during setup and we must not force STA-only.
  {
    bool skip_boot_wifi = false;
#ifdef HALO_SENSE_PROD_WRAPPER
    if (!ProvisioningState::isProvisioned()) {
      skip_boot_wifi = true;
      Serial.printf("[BOOT_FLOW] stage=wifi_skip_needs_provisioning t=%lu\n", millis());
    } else {
      // Provisioned but check if home WiFi creds actually exist
      char check_ssid[64];
      char check_pass[64];
      if (!ProvisioningState::loadHomeWifiCreds(check_ssid, sizeof(check_ssid),
                                                 check_pass, sizeof(check_pass))) {
        skip_boot_wifi = true;
        Serial.printf("[BOOT_FLOW] stage=wifi_skip_no_home_creds t=%lu\n", millis());
      }
    }
#endif
    if (!skip_boot_wifi) {
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
      if (ssid && ssid[0]) {
        WiFi.persistent(false);  // Don't cache WiFi config in NVS — we manage creds ourselves

        // After deep sleep, the WiFi driver has stale state from the pre-sleep
        // teardown (WiFi.disconnect + WiFi.mode(OFF) + esp_wifi_stop).
        // A hard reset clears the driver completely, matching cold-boot behavior.
        if (deep_sleep_reset) {
          Serial.println("[BOOT_WIFI] deep sleep wake - hard reset WiFi driver");
          hardResetSta();  // disconnect(true,true) + mode OFF + delay + mode STA
        }

        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(true);
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (wifi_guard_try_claim_connect("boot_setup")) {
          SenseBackupWifiCall backup_call;
          if (backup_call) WiFi.begin(ssid, pass);
          wifi_guard_set_state(WIFI_STATE_CONNECTING, "boot_begin", WiFi.status());
          Serial.printf("[BOOT_FLOW] stage=wifi_begin_immediate t=%lu ssid=%s status=%d heap=%u\n",
                        millis(),
                        ssid,
                        (int)WiFi.status(),
                        (unsigned)ESP.getFreeHeap());
        }

        // WiFi.begin() fired above — connection proceeds in background.
        // service_wifi_maintenance() in loop() handles retries.
        // Capture/voice work without WiFi; uploads wait for connectivity.
        Serial.printf("[BOOT_FLOW] stage=wifi_deferred t=%lu status=%d (connecting in background)\n",
                      millis(), (int)WiFi.status());
      } else {
        Serial.printf("[BOOT_FLOW] stage=wifi_skip_no_ssid t=%lu\n", millis());
      }
    }
  }
  
  // Initialize MQTT (will connect when needed)
  // MQTT DISABLED: using HTTPS for OTA scheduling, presign, uploads.
  // MQTT was only used for remote cmd/desired-version push.
  // Keeping it off prevents esp-aes TLS failures that block deep sleep.
  #define HALO_MQTT_DISABLED 1
  #if HALO_MQTT_DISABLED
  Serial.println("[SETUP] MQTT DISABLED (HALO_MQTT_DISABLED=1)");
  #else
  Serial.println("[SETUP] MQTT system initialized (will connect on demand)");
  #endif
  
  Serial.println("\nReady. Sense board initialized.");
  Serial.println("UART communication with LCD board active.");

#ifdef HALO_SENSE_PROD_WRAPPER
  Serial.printf("[BOOT_FLOW] stage=halo_prod_setup_begin t=%lu\n", millis());
  halo_prod_setup();
  Serial.printf("[BOOT_FLOW] stage=halo_prod_setup_done t=%lu\n", millis());
#endif
}

void loop() {
  sense_backup_diagnostic_tick();
  // Process incoming UART messages (line-based JSON protocol) - non-blocking.
  // Skip when the LCD OTA proxy task owns the serial port for binary
  // COBS framing — the proxy reads lcdSerial directly during that phase.
  pump_uart_rx_once();

  // DEBUG: USB serial command injection — allows injecting UART JSON messages
  // via USB CDC (/dev/cu.usbmodem*) so they are processed as if sent by the LCD.
  {
    static char usb_rx_line[UART_RX_FRAME_MAX + 1];
    static size_t usb_rx_len = 0;
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
    static bool usb_rx_overflow=false;
#endif
    while (Serial.available() > 0) {
      char c = (char)Serial.read();
      if (c == '\n') {
        usb_rx_line[usb_rx_len] = '\0';
        // Trim trailing \r if present
        if (usb_rx_len > 0 && usb_rx_line[usb_rx_len - 1] == '\r') {
          usb_rx_line[--usb_rx_len] = '\0';
        }
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
        if(usb_rx_overflow)Serial.printf("[OTA_DIAG_AUTH] USB line overflow; discarded\n");
        else
#endif
        if (usb_rx_len > 0) {
          if (sense_backup_usb_command(usb_rx_line)) {
            // Local USB only; never accepted by parse_input_message().
          } else if (usb_rx_line[0] == '{') {
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
            Serial.printf("[DEBUG_INJECT] USB JSON bytes=%u\n",unsigned(usb_rx_len));
            if(!halo_diag_auth_usb_line(usb_rx_line,usb_rx_len)) {
#if HALO_IDLE_NETWORK_PROBE && HALO_IDLE_NETWORK_RECOVERY && HALO_OTA_BENCH_PROFILE
              if(!halo_idle_probe_usb_line(usb_rx_line,usb_rx_len))
#endif
              parse_input_message(usb_rx_line);
            }
#else
            Serial.printf("[DEBUG_INJECT] Processing USB serial command: %.40s...\n", usb_rx_line);
            parse_input_message(usb_rx_line);
#endif
          } else if (strcmp(usb_rx_line, "errors") == 0) {
            sense_errlog_dump(Serial);
          } else if (strcmp(usb_rx_line, "lcdxfer") == 0) {
            sense_lcd_terminal_dump(Serial);
#if defined(HALO_OTA_BENCH_CASE)
          } else if (strcmp(usb_rx_line, "otabench") == 0) {
            halo_bench_dump(Serial, g_lcd_ota_proxy_owns_uart);
#endif
          } else if (strcmp(usb_rx_line, "wakelog") == 0) {
            wakelog_dump();
          } else if (strcmp(usb_rx_line, "clearerrors") == 0) {
            sense_errlog_clear();
#if HALO_SPOOL_TEST
          } else if (strcmp(usb_rx_line, "nextwake") == 0) {
            // Verify the nightly maintenance timer against the DEVICE's real
            // clock and TZ. The host tests prove the arithmetic (DST, month
            // ends, spring-forward); only hardware can prove the epoch actually
            // survives deep sleep and that TZ is what we think it is.
            const uint32_t now_epoch = sense_now_epoch();
            const uint32_t secs = halo_seconds_until_maintenance((time_t)now_epoch);
            time_t nowt = (time_t)now_epoch;
            struct tm lt; char nowbuf[32] = "<no clock>";
            if (now_epoch && localtime_r(&nowt, &lt)) {
              strftime(nowbuf, sizeof(nowbuf), "%Y-%m-%d %H:%M:%S", &lt);
            }
            char wakebuf[32] = "<fallback>";
            if (secs) {
              time_t w = nowt + (time_t)secs;
              struct tm wt;
              if (localtime_r(&w, &wt)) strftime(wakebuf, sizeof(wakebuf), "%Y-%m-%d %H:%M:%S", &wt);
            }
            Serial.printf("[NEXTWAKE] epoch=%lu local=\"%s\" TZ=%s -> in %lus (%.2fh) at \"%s\"%s\n",
                          (unsigned long)now_epoch, nowbuf,
                          getenv("TZ") ? getenv("TZ") : "<unset>",
                          (unsigned long)secs, secs / 3600.0, wakebuf,
                          secs ? "" : "  [would use fallback interval]");
          } else if (strcmp(usb_rx_line, "draintest") == 0) {
            // Fetch the oldest spooled image back and verify it byte-for-byte,
            // WITHOUT queueing an upload (the spooltest payload is synthetic and
            // would become a junk check-in in the real account).
            sense_spool_drain_force(true);
          } else if (strcmp(usb_rx_line, "drainreal") == 0) {
            // Full drain including the upload. Only for a slot holding a REAL
            // capture — this does write to the backend.
            sense_spool_drain_force(false);
          } else if (strncmp(usb_rx_line, "failupload", 10) == 0) {
            // Force the NEXT upload to fail so the spool-to-SD fallback in
            // sense_sleep.h runs for a REAL capture. Since the WiFi teardown fix,
            // uploads finish in ~2s and cannot be raced by forcing a sleep —
            // making the upload genuinely fail is the only way onto that path.
            {
              const char* a = usb_rx_line + 10;
              while (*a == ' ') a++;
              int n = atoi(a);
              g_test_fail_uploads = (uint8_t)(n > 0 ? n : 3);
            }
            // Default 3: one failure is not enough to reach the spool path,
            // because the retry succeeds and the job never survives to sleep.
            Serial.printf("[FAILUPLOAD] next %u upload(s) will be forced to fail\n",
                          (unsigned)g_test_fail_uploads);
#if HALO_CAM_STALL_TEST
          } else if (strncmp(usb_rx_line, "camstall", 8) == 0) {
            // "camstall N" — make the next N camera grabs overrun their bound.
            // N survives the self-heal restart (RTC), so N=1 tests recovery and
            // a large N tests the restart budget running out.
            {
              const char* a = usb_rx_line + 8;
              while (*a == ' ') a++;
              const int n = atoi(a);
              g_bench_cam_stall = (uint32_t)(n > 0 ? n : 1);
            }
            Serial.printf("[CAM_STALL_TEST] next %lu grab(s) will stall (restarts so far=%lu/%d)\n",
                          (unsigned long)g_bench_cam_stall,
                          (unsigned long)g_cam_wedge_restarts,
                          (int)CAMERA_GRAB_WEDGE_RESTART_MAX);
          } else if (strncmp(usb_rx_line, "camwedge", 8) == 0) {
            // "camwedge N" — seed the wedge-restart counter.
            //
            // The counter is RTC-backed so it survives the self-heal restart,
            // but NOT the reset that opening this very port causes — so letting
            // it climb naturally and then reading the result is self-defeating:
            // the act of observing zeroes it. Seeding it reaches the two
            // branches that only depend on its VALUE (budget exhausted, and
            // budget retired by a good frame) without needing it to survive
            // anything. The increment path is verified separately by a real
            // stall.
            {
              const char* a = usb_rx_line + 8;
              while (*a == ' ') a++;
              g_cam_wedge_restarts = (uint32_t)strtoul(a, NULL, 10);
            }
            Serial.printf("[CAM_STALL_TEST] wedge restart counter seeded to %lu/%d\n",
                          (unsigned long)g_cam_wedge_restarts,
                          (int)CAMERA_GRAB_WEDGE_RESTART_MAX);
#endif
          } else if (strcmp(usb_rx_line, "crashme") == 0) {
            // Induce a REAL panic so the crash-forensics path can be verified.
            //
            // diag_record_crash_boot() only fires for PANIC / INT_WDT / TASK_WDT /
            // WDT / BROWNOUT — an ordinary esp_restart() is ESP_RST_SW and is not
            // counted, so the camera self-heal restarts do NOT exercise this. There
            // was no way to produce a genuine crash on demand, which is exactly why
            // this path stayed unverified while the bug in it went unnoticed.
            diag_note_stage("crashme_test", 4242);
            Serial.println("[CRASHTEST] inducing a panic via abort() - expect ESP_RST_PANIC");
            Serial.flush();
            delay(150);   // let the line leave the UART before the chip goes down
            abort();
          } else if (strcmp(usb_rx_line, "crashinfo") == 0) {
            // Read the record back. These now live in .rtc_noinit, so unlike the
            // old RTC_DATA_ATTR versions they survive both the crash AND the reset
            // that opening this very port causes.
            Serial.printf("[CRASHINFO] count=%lu last_reason=%lu last_wake=%lu "
                          "last_stage=\"%s\" stage_code=%ld stage_uptime_ms=%lu "
                          "cur_stage=\"%s\"\n",
                          (unsigned long)g_rtc_crash_count,
                          (unsigned long)g_rtc_last_crash_reason,
                          (unsigned long)g_rtc_last_crash_wake_cause,
                          g_rtc_last_crash_stage,
                          (long)g_rtc_last_crash_stage_code,
                          (unsigned long)g_rtc_last_crash_stage_uptime_ms,
                          g_rtc_last_stage);
          } else if (strncmp(usb_rx_line, "settz", 5) == 0) {
            // "settz <POSIX TZ>" — stand in for the backend until it returns a
            // zone at claim time, so the CONSUMPTION path (persist -> reload on
            // boot -> apply -> nightly wake recomputed in the new local time) can
            // be tested for real rather than just compiled.
            // e.g.  settz EST5EDT,M3.2.0,M11.1.0
            {
              const char* a = usb_rx_line + 5;
              while (*a == ' ') a++;
              if (*a) {
                std::lock_guard<std::recursive_mutex> config_lock(ProvisioningState::timezoneMutex());
                if (ProvisioningState::saveTimezone(a)) {
                  sense_set_timezone(a);
                  Serial.printf("[SETTZ] saved+applied tz=%s\n", a);
                } else {
                  Serial.println("[SETTZ] persistence unverified; active timezone unchanged");
                }
              } else {
                char cur[64];
                const bool have = ProvisioningState::loadTimezone(cur, sizeof(cur));
                Serial.printf("[SETTZ] stored=%s active=%s\n",
                              have ? cur : "<none>", g_tz_current);
              }
            }
          } else if (strncmp(usb_rx_line, "dropacks", 8) == 0) {
            // "dropacks N" — swallow the next N INPUT_ACKs so the LCD retransmits
            // a message we already acted on, exercising duplicate suppression.
            const char* arg = usb_rx_line + 8;
            while (*arg == ' ') arg++;
            g_test_drop_acks = (uint32_t)atoi(arg);
            if (g_test_drop_acks == 0) g_test_drop_acks = 1;
            Serial.printf("[ACKTEST] will drop the next %lu INPUT_ACK(s)\n",
                          (unsigned long)g_test_drop_acks);
          } else if (strcmp(usb_rx_line, "spooltest") == 0) {
            // Bench-only: prove the SD image spool actually runs, without having
            // to engineer a WiFi failure plus a sleep to reach the real trigger
            // in sense_sleep.h. Uses the same send path the sleep handler calls.
            const size_t TEST_LEN = 180 * 1024;   // real captures are 177-189KB
            uint8_t* t = (uint8_t*)heap_caps_malloc(TEST_LEN, MALLOC_CAP_SPIRAM);
            if (!t) {
              Serial.println("[SPOOLTEST] alloc failed");
            } else {
              // Deterministic pattern so the received file can be verified
              // byte-for-byte, not merely by length.
              for (size_t i = 0; i < TEST_LEN; i++) t[i] = (uint8_t)((i * 31 + 7) & 0xFF);
              Serial.printf("[SPOOLTEST] sending %u bytes\n", (unsigned)TEST_LEN);
              // Synthesise a job so the test exercises the same metadata path
              // production uses — otherwise the sidecar would go untested.
              UploadJob tj = {};
              tj.job_id = 999999;
              snprintf(tj.mode, sizeof(tj.mode), "%s", "spooltest");
              tj.quantity = 1;
              tj.image_len = TEST_LEN;
              bool ok = sense_spool_image_to_lcd(tj, t, TEST_LEN);
              Serial.printf("[SPOOLTEST] RESULT=%s\n", ok ? "PASS" : "FAIL");
              free(t);
            }
#endif
          }
        }
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
        usb_rx_overflow=false;
        {volatile char* wipe=usb_rx_line;for(size_t i=0;i<sizeof(usb_rx_line);++i)wipe[i]=0;}
#endif
        usb_rx_len = 0;
      } else if (usb_rx_len < UART_RX_FRAME_MAX) {
        usb_rx_line[usb_rx_len++] = c;
      }
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
      else usb_rx_overflow=true;
#endif
      // Retain overflow until LF; a truncated valid prefix is never a command.
    }
  }

  service_boot_wifi_connect(millis());

  // LIST_ACTIVE staleness watchdog: the LCD re-asserts LIST_ACTIVE(state=1)
  // every few seconds while the user is on the list screen. If it goes silent
  // (e.g. LCD crash) we must not stay pinned awake forever — auto-clear after
  // ~30s of no refresh. Only clears on staleness; a live screen keeps it set.
  if (g_list_screen_active &&
      last_list_active_ms > 0 &&
      (millis() - last_list_active_ms) >= LIST_ACTIVE_STALE_MS) {
    g_list_screen_active = false;
    Serial.printf("[LIST_ACTIVE] auto-clear stale age_ms=%lu\n",
                  (unsigned long)(millis() - last_list_active_ms));
  }

  if (wifi_connect_inflight) {
    wifi_guard_poll();
  }
  if (wifi_is_connected() && wifi_state != WIFI_STATE_CONNECTED) {
    wifi_guard_set_inflight(false);
    wifi_guard_set_state(WIFI_STATE_CONNECTED, "loop_status_connected", WL_CONNECTED);
  }

  // Periodic WiFi RSSI reporter — sends status over UART every 2s for monitoring via LCD
  {
    static unsigned long last_wifi_report_ms = 0;
    unsigned long now = millis();
    if ((now - last_wifi_report_ms) >= 2000) {
      last_wifi_report_ms = now;
      wl_status_t st = WiFi.status();
      if (st == WL_CONNECTED) {
        int8_t rssi = (int8_t)WiFi.RSSI();
        char detail[48];
        snprintf(detail, sizeof(detail), "rssi=%d ip=%s", rssi, WiFi.localIP().toString().c_str());
        uart_send_sense_diag("wifi", "rssi_report", "connected", rssi, detail);
      } else {
        char detail[32];
        snprintf(detail, sizeof(detail), "status=%d", (int)st);
        uart_send_sense_diag("wifi", "rssi_report", "disconnected", (int)st, detail);
      }
    }
  }

#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
  upload_persist_maybe_replay();

  // SD-spool drain. Pump first (an in-flight transfer owns the UART and must be
  // serviced promptly), then consider starting a new one. Both are no-ops unless
  // the device is idle, WiFi is up and the queue is clear — background work must
  // never compete with someone standing at the device.
  sense_spool_receive_pump();
  // Learn how deep the card is, once per wake. Cheap, and it is what decides
  // whether we bother waking early to empty it.
  sense_spool_probe_tick();
  // On a wake whose whole purpose is draining, pull slots back to back.
  sense_spool_drain_wake_tick();
  // The opportunistic tick stays for the case where the device happens to be
  // idle with WiFi up anyway. It rarely fires; the drain wake is the real path.
  sense_spool_drain_tick();
  if (media_retry_image_first()) {
    sense_image_spool_replay_tick();
    sense_voice_spool_replay_tick();
  } else {
    sense_voice_spool_replay_tick();
    sense_image_spool_replay_tick();
  }

  // Relay the wake history once the link is up, exactly once per boot. Deferred
  // to here rather than setup() because the LCD may not be listening yet, and a
  // report nobody receives is worse than none — it looks like the feature works.
  {
    // Deliberately NOT gated on link_synced.
    //
    // link_synced only becomes true when the LCD sends SYNC, and after a TIMER
    // wake the LCD does not know the Sense woke at all — so it never sends one
    // and the gate could never open on the maintenance path this exists to
    // report. Two full test cycles produced no output for exactly that reason.
    // uart_send_sense_diag() does not require sync (the ota_sched and wifi diags
    // arrive fine without it), so a short settle delay is the only thing needed.
    static bool s_wakelog_relayed = false;
    if (!s_wakelog_relayed && millis() > 5000) {
      s_wakelog_relayed = true;
      wakelog_report_to_lcd();
    }
  }
#endif

  if (!guardian_force_sleep && GUARDIAN_FORCE_SLEEP_MS > 0) {
    unsigned long now_ms = millis();
#ifdef HALO_SENSE_PROD_WRAPPER
    if (halo_provisioning_active()) {
      // Never guardian-force-sleep while provisioning/SoftAP setup is active --
      // it tears down the SoftAP + QR mid-setup. Pause the guardian clock so it
      // also doesn't fire the instant provisioning ends after a long setup. The
      // idle + coordinated sleep paths already guard provisioning the same way.
      guardian_awake_start_ms = now_ms;
    } else
#endif
    {
      unsigned long awake_ms = now_ms - guardian_awake_start_ms;
      if (awake_ms >= GUARDIAN_FORCE_SLEEP_MS) {
        guardian_force_sleep = true;
        Serial.printf("[GUARDIAN] force_sleep elapsed_ms=%lu\n", awake_ms);
        sense_enter_sleep(SENSE_SLEEP_DEEP_IDLE);
        return;
      }
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
  if (g_voice_list_followup.exchange(false)) {
    list_last_fetch_ok_ms = 0;
    list_refresh_cooldown_until_ms = 0;
    g_voice_list_refresh_pending.store(true);
  }
  if ((g_voice_list_refresh_pending.load() || list_transport_refresh_deferred.load()) &&
      !g_media_voice_list_mode.load() && !voice_list_pending() && !http_inflight) {
    g_voice_list_refresh_pending.store(false);
    list_transport_refresh_deferred.store(false);
    if (g_list_screen_active) request_list_refresh("voice_or_transport_followup", false);
  }

  if (delete_requested && !http_inflight && !g_media_voice_list_mode.load()) {
    delete_requested = false;  // Clear flag immediately
    
    Serial.printf("[LOOP] Processing delete request for ID: %s\n", delete_item_id);
    
    // Ensure Wi-Fi is connected
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[LOOP] Wi-Fi not connected, connecting...");
      if (!wifi_connect()) {
        Serial.println("[LOOP] Wi-Fi connection failed!");
        Serial.println("[DELETE] wifi_failed -> retain item and report failure");
        shopping_list_delete_failed(delete_item_id, "wifi");
        // Don't return - continue processing UART
      }
    }
    
    // Delete item from AWS. Shopping list refresh is disabled.
    if (WiFi.status() == WL_CONNECTED) {
      if (delete_item_from_api(delete_item_id) == ListRequestResult::Deferred)
        delete_requested = true;
    }
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

    // Preserve the pending request, message ID and original 60-second timeout.
    // Returning to loop_end keeps UART, WiFi and SNTP service running; do not
    // clear/recreate the request or enter pre-sleep DNS while time is pending.
    if (!guardian_force_sleep && sense_ntp_attempt_pending()) {
      if (sleep_block_should_log("clock_sync_pending", "coord_request", "clock")) {
        Serial.println("[SLEEP_BLOCK] reason=clock_sync_pending where=coord_request");
      }
      goto loop_end;
    }
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
      if (g_lcd_ota_task_running) {
        sleep_block_reason = "lcd_ota_proxy";
      } else if (link_recent) {
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
          OtaIntent::clearDesired();
          pre_sleep_block_start_ms = 0;
        } else {
          Serial.println("[SENSE] Inactivity timeout ignored (OTA pending)");
          last_lcd_communication = now_ms;
          goto loop_end;
        }
      }
#endif
      // Check if there are any active operations that should prevent sleep
      bool can_sleep = true;
      const char* block_reason = NULL;
      const char* block_op = "none";
      if (g_lcd_ota_task_running) {
        block_reason = "lcd_ota_proxy";
        block_op = "lcd_ota";
      } else if (list_refresh_inflight) {
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
#ifdef HALO_SENSE_PROD_WRAPPER
        if (halo_provisioning_active()) {
          // Never idle-sleep while provisioning/SoftAP setup is active -- sleeping
          // tears down the SoftAP + QR and blocks the user from provisioning.
          // (The coordinated-sleep path already guards this; the idle path was missed.)
          goto loop_end;
        }
#endif
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

loop_end:

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

  // NO idle re-arm of the camera DMA reserve. This was tried on 2026-08-21 and
  // REVERTED the same day; do not reintroduce it without reading this.
  //
  // The idea was sound-looking: the reserve is absent at camera-init time in ~95%
  // of captures, so re-acquire it whenever the device looks idle. It did fix that
  // -- exposure went 95% -> 0% over 24 captures. It also broke uploads.
  //
  //   presign HTTP 200:  41/41 before  ->  6/25 after
  //
  // A TLS handshake needs ~25-30KB CONTIGUOUS internal RAM, which is the entire
  // reason the upload/presign guards RELEASE this 16KB block before handshaking.
  // "Idle" is not mutually exclusive with an upload in flight -- uploads run in
  // the background after current_job reports DONE -- so the re-arm raced those
  // guards and grabbed the block back mid-handshake. The logs interleave exactly
  // that: PRESIGN attempt= / re-armed while idle / HTTP_FAIL label=PRESIGN.
  //
  // Gating on http_inflight/upload_inflight is NOT sufficient: the presign retry
  // loop spans several seconds with gaps where those flags are clear.
  //
  // Trade accepted: a camera init that fails ~1 in 12 reports an honest error and
  // the next capture works. A failed upload can LOSE THE PHOTO outright (observed:
  // "SD spool FAILED ... saved=0"). Protecting the upload is worth more.

#ifdef HALO_SENSE_PROD_WRAPPER
  halo_prod_loop();
#endif

  delay(idle_mode ? 50 : 10);
}

/*
 * LCD_Minimal.ino
 * 
 * MINIMAL VERSION - Only essential functionality:
 * - LCD screen asleep by default
 * - Wake on touch or scroll
 * - On wake, wake Sense board (but don't auto-refresh - user must use pull-to-refresh)
 * - User can scroll through items
 * - Pull-to-refresh (scroll 5 ticks counter-clockwise beyond first item) triggers list refresh
 * 
 * REMOVED: Audio recording, provisioning, OTA, camera, voice features, all other UI features
 */

typedef struct app_event_t app_event_t;

#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "bidi_switch_knob.h"
#include "user_config.h"
#include "HardwareSerial.h"
#include "ArduinoJson.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "Preferences.h"
#include <string.h>
#include "src/provisioning/qr_display.h"
#include <string>
#ifdef HALO_LCD_PROD_WRAPPER
#include "../halo_ota_demo/firmware/shared/LcdWifiCreds.h"
#endif
#ifndef HALO_BOARD_LCD
#define HALO_BOARD_LCD 1
#endif
#include "../halo_common/BoardConfig.h"
#include "../halomain_assets/ui.h"
#include "../halomain_assets/ui_img_Frame_439_1_png.c"
#include "../halomain_assets/ui_img_Frame_439_2_png.c"
#include "../halomain_assets/ui_img_Frame_439_png.c"
#include "../halomain_assets/ui_img_Frame_440_png.c"
#include "../halomain_assets/ui_img_Frame_443_png.c"
#include "../halomain_assets/ui_img_Frame_443_1_png.c"

#ifdef HALO_LCD_PROD_WRAPPER
void halo_lcd_prod_setup();
void halo_lcd_prod_loop();
void halo_lcd_prod_on_wifi_creds(const char* ssid, const char* pass);
void halo_lcd_prod_on_wifi_on(uint32_t timeout_ms);
bool halo_lcd_prod_should_stay_awake();
void halo_lcd_prod_run_ota_check_once();
unsigned long halo_lcd_prod_wifi_budget_ms(unsigned long safety_margin_ms);
static bool lcd_has_wifi_creds();
static bool in_cold_boot_grace();
#endif

// ── Protocol Configuration ──────────────────────────────────────────
#define PROTOCOL_VERSION 1
#define MAX_LINE_LENGTH 2048  // Increased for UI_LIST messages (can be >1KB)

// ── UART Configuration ────────────────────────────────────────────────
#define UART_BAUD_RATE 115200
#ifndef UART_TX_PIN
#define UART_TX_PIN 38   // LCD transmits on GPIO38
#endif
#ifndef UART_RX_PIN
#define UART_RX_PIN 48   // LCD receives on GPIO48
#endif
#ifndef UART_RX_DEBUG
#define UART_RX_DEBUG 0
#endif
HardwareSerial senseSerial(1);  // Use UART1

// ── Wakeup Configuration ──────────────────────────────────────────
// Wake roles:
// - LCD drives INT_PIN low to wake Sense EXT0 (GPIO2, active low).
// - LCD wakes from touch INT (LCD_WAKE_GPIO GPIO9, active low).
#define INT_PIN 39   // LCD -> Sense wake line (GPIO39)
static const gpio_num_t PIN_TOUCH_INT = HALO_WAKE_GPIO;   // TP_INT
static const gpio_num_t PIN_EC1_A     = GPIO_NUM_8;   // EC1_A
static const gpio_num_t PIN_EC1_B     = GPIO_NUM_7;   // EC1_B
#define LCD_WAKE_GPIO PIN_TOUCH_INT
#define LCD_WAKE_LEVEL HALO_WAKE_LEVEL
#define WAKE_PIN_BOOT_WARN_MS 200
#define LCD_SLEEP_FALLBACK_TIMER_SEC 30
#define LCD_OTA_WAKE_INTERVAL_SEC (6 * 60 * 60)
#ifndef LCD_SHIP_MODE_OTA
#define LCD_SHIP_MODE_OTA true
#endif
/* Only enter SHIP_OTA wake window in factory/ship-test builds; normal builds use maintenance-only OTA */
#ifndef HALO_SHIP_TEST_MODE
#define HALO_SHIP_TEST_MODE 0
#endif
#ifndef SHIP_MENU_UI
#define SHIP_MENU_UI 0
#endif

// ── Shopping List State ─────────────────────────────────────────────
#define MAX_LIST_ITEMS 50

struct app_state_t {
  char items[MAX_LIST_ITEMS][64];
  char item_ids[MAX_LIST_ITEMS][64];
  int  count;
  int  selected_index;
};

typedef enum {
  MENU_MODE_MAIN = 0,
  MENU_MODE_SETTINGS
} menu_mode_t;

typedef enum {
  LCD_MODE_UI_ACTIVE = 0,
  LCD_MODE_MAINTENANCE
} lcd_mode_t;

typedef enum {
  SHIP_MENU_ACTION_CHECK_IN = 0,
  SHIP_MENU_ACTION_CHECK_OUT,
  SHIP_MENU_ACTION_LOG_DISH,
  SHIP_MENU_ACTION_MORE,
  SHIP_MENU_ACTION_HOME,
  SHIP_MENU_ACTION_SETTINGS,
  SHIP_MENU_ACTION_DEBUG,
  SHIP_MENU_ACTION_RESET_WIFI,
  SHIP_MENU_ACTION_BACK
} ship_menu_action_t;

typedef enum {
  SHIP_MENU_SCREEN_MAIN = 0,
  SHIP_MENU_SCREEN_SECOND,
  SHIP_MENU_SCREEN_SETTINGS
} ship_menu_screen_t;

typedef enum {
  SCREEN_HOME = 0,
  SCREEN_SECOND,
  SCREEN_SETTINGS,
  SCREEN_RESULT,
  SCREEN_DEBUG
} ui_screen_t;

// Forward declarations (needed when included as a wrapper)
static void stop_glowing_animation(void);
static void delete_item_btn_handler(lv_event_t * e);
static void menu_btn_event_handler(lv_event_t * e);
static void set_menu_mode(menu_mode_t mode);
static void update_menu_display();
static void resetActivityTimer();
static void user_activity_bump(const char* reason);
static void save_list_to_storage(const app_state_t *s);
static int load_list_from_storage(app_state_t *s);
static bool notify_sense_sleep();
static void init_touch_once();
static void init_knob_once();
static void init_ui_stack(int saved_count);
static void enter_ship_ota_sleep();
static bool wake_sense_for_request(const char* reason);
static uint64_t buildWakeMaskForSleep();
static bool sleep_blocked_for_ota();
static void ship_menu_send_menu_select(const char* menu_item, int menu_index, const char* label);
static void show_ship_debug_screen();
static void ui_apply_ship_ui_status();
static void wifi_on_run_deferred_if_ready(const char* reason);

// ── Double-Buffered List State ──────────────────────────────────────
// Active: UI reads this only (rendered on screen)
// Pending: UART RX writes here only (background updates)
static app_state_t g_active;   // UI reads this only
static app_state_t g_pending;  // UART RX writes here only
static volatile bool pending_ready = false;
static SemaphoreHandle_t app_state_mutex = NULL;

// ── Protocol State ──────────────────────────────────────────────────
static uint32_t lcd_msg_id_counter = 1;
static char uart_rx_line_buffer[MAX_LINE_LENGTH];
static int uart_rx_line_pos = 0;

// ── Sleep/Wake State ────────────────────────────────────────────────
static uint32_t boot_ms = 0;
static uint32_t g_boot_count = 0;
static const uint32_t COLD_BOOT_GRACE_MS = 0;
static bool cold_boot_grace_logged = false;
static volatile unsigned long touch_ignore_until = 0;
static volatile unsigned long scroll_ignore_until = 0;
/* Throttle scroll-driven redraws to avoid SPI "color failed" when scrolling fast.
 * We do NOT call lv_obj_invalidate on the whole screen: ui_refresh_from_state -> ui_update_list
 * only updates the 7 list row labels (text/style), so LVGL invalidates just those areas. */
static unsigned long last_scroll_refresh_ms = 0;
static bool scroll_pending_redraw = false;
#define SCROLL_REFRESH_MIN_MS 50
/* Minimal telemetry for freeze triage (no watchdog) */
static volatile unsigned long last_ui_tick_ms = 0;
static volatile unsigned long last_touch_or_input_ms = 0;
static volatile uint32_t ui_loop_counter = 0;
static unsigned long last_heartbeat_ms = 0;
#define UI_HEARTBEAT_INTERVAL_MS 2000
/* Display busy overlay when flush submit fails (soft fault) */
static lv_obj_t *display_busy_overlay = NULL;
static unsigned long display_busy_hide_at_ms = 0;
#define DISPLAY_BUSY_OVERLAY_MS 1000
static unsigned long sleep_entry_time = 0;
static unsigned long last_sleep_skip_log_ms = 0;
static char last_sleep_skip_reason[32] = "";
static const unsigned long WAKE_DEBOUNCE_MS = 200;
static volatile bool g_in_light_sleep = false;
extern "C" volatile bool g_sleep_transition = false;
static bool g_ui_initialized = false;
static bool g_lcd_initialized = false;
static bool g_backlight_initialized = false;
static bool g_touch_initialized = false;
static bool g_ship_ota_wake_window = false;
static bool g_ship_ota_user_input = false;
static bool g_ship_ota_ota_started = false;
static volatile lcd_mode_t lcd_mode = LCD_MODE_UI_ACTIVE;
static bool wifi_on_reject_logged = false;
static bool wifi_on_deferred = false;
static bool wifi_on_deferred_logged = false;
static unsigned long wifi_on_deferred_timeout_ms = 0;
static int g_saved_list_count = 0;
static unsigned long stay_awake_until_ms = 0;
static bool link_synced = false;
static bool link_sync_pending = false;
static bool g_lcd_maintenance_active = false;
static bool g_lcd_maintenance_started = false;
static unsigned long g_lcd_maintenance_deadline_ms = 0;
static volatile bool g_lcd_maintenance_aborted = false;
static char s_wake_cause_label[32] = "cold_boot";

bool lcd_maintenance_active(void) {
  return g_lcd_maintenance_active;
}
const char* lcd_wake_cause_label(void) {
  return s_wake_cause_label;
}

extern const char* kFirmwareVersion;
static bool sense_awake_confirmed = true;
static unsigned long last_sense_any_rx_ms = 0;
static unsigned long wake_retry_until_ms = 0;
static unsigned long last_wake_retry_ms = 0;
static const unsigned long WAKE_RETRY_INTERVAL_MS = 1200;
static const unsigned long WAKE_RETRY_WINDOW_MS = 30000;
static const unsigned long WAKE_PULSE_DURATION_MS = 80;
static const unsigned long WAKE_PULSE_SHORT_MS = 30;
static const unsigned long WAKE_LINE_STUCK_WARN_MS = 3000;
static const unsigned long INT_PULSE_COOLDOWN_MS = 5000;
static unsigned long last_int_pulse_ms = 0;
static volatile bool user_activity_since_sleep = false;
static volatile bool sense_status_sync_requested = false;
static volatile bool sense_ota_apply_required = false;
static unsigned int sleep_handshake_fail_count = 0;
static unsigned long sleep_retry_allowed_ms = 0;
static bool sleep_retry_requires_user = false;
static unsigned long last_sleep_retry_log_ms = 0;
static unsigned long last_user_activity_ms = 0;
static volatile bool sense_wake_explicit_request = false;
static volatile bool sense_awake_estimate = false;
enum SenseState {
  SENSE_UNKNOWN = 0,
  SENSE_AWAKE,
  SENSE_ASLEEP
};
static SenseState sense_state = SENSE_UNKNOWN;
static uint8_t sense_missed_pongs = 0;
static bool sense_pong_pending = false;
static unsigned long sense_pong_deadline_ms = 0;
static unsigned long last_sense_ping_ms = 0;
static const unsigned long SENSE_PROBE_INTERVAL_MS = 5000;
static const unsigned long SENSE_PONG_TIMEOUT_MS = 1500;
static const uint8_t SENSE_MISSED_PONGS_FOR_ASLEEP = 2;
static unsigned long last_sense_rx_ms = 0;
static const unsigned long SENSE_RX_STALE_MS = 8000;
static const unsigned long SENSE_UNKNOWN_STALE_MS = 12000;
static const unsigned long SENSE_UNKNOWN_STALE_EXTENDED_MS = 30000;
static const unsigned long SENSE_PING_MIN_INTERVAL_MS = 5000;
static uint32_t sense_awake_grace_until_ms = 0;
static const uint32_t SENSE_AWAKE_GRACE_MS = 10000;
static const uint32_t SENSE_RECENT_RX_FOR_SLEEP_MS = 10000;
static bool wake_ext0_enabled = false;
static bool wake_ext1_enabled = false;
static bool wake_timer_enabled = false;
static bool wake_uart_enabled = false;
static volatile bool sleep_ack_received = false;
static volatile bool sleep_ready_received = false;
static volatile bool sleep_busy_received = false;
static volatile bool sleep_deny_received = false;
static unsigned long sleep_deny_retry_ms = 0;
static unsigned long sleep_deny_received_ms = 0;
static char sleep_deny_reason[24] = "";
static bool sleep_deny_active = false;
static uint32_t sleep_fallback_timer_sec = 0;
static const unsigned long SENSE_SLEEP_RETRY_INTERVAL_MS = 1000;
static const unsigned long SLEEP_HANDSHAKE_RETRY_DELAY_MS = 800;
static const uint8_t SLEEP_HANDSHAKE_MAX_ATTEMPTS = 3;
static const uint32_t SLEEP_DENY_RETRY_DEFAULT_MS = 5000;
static const uint32_t SLEEP_FALLBACK_TIMER_SEC = 15;
static const int SLEEP_WAIT_DIM_DUTY = 20;
static volatile bool refresh_request_pending = false;
static volatile unsigned long refresh_request_last_ms = 0;
static volatile unsigned long refresh_request_start_ms = 0;
static volatile bool refresh_input_wake_sent = false;
static unsigned long refresh_request_retry_count = 0;
static bool refresh_grace_extended = false;
static bool lcd_refresh_inflight = false;
static unsigned long lcd_refresh_sent_ms = 0;
static unsigned long lcd_refresh_start_ms = 0;
static bool lcd_refresh_ack_seen = false;
static uint8_t lcd_refresh_retry_count = 0;
static unsigned long lcd_last_ui_list_complete_ms = 0;
static const unsigned long LCD_UI_LIST_DEDUPE_MS = 1000;
static const unsigned long REFRESH_REQUEST_RETRY_MS = 2500;
static const unsigned long REFRESH_REQUEST_MAX_WINDOW_MS = 20000;
enum RefreshState {
  REFRESH_IDLE = 0,
  REFRESH_WAKE_PENDING,
  REFRESH_INFLIGHT,
  REFRESH_COMPLETE,
  REFRESH_FAILED
};
static RefreshState refresh_state = REFRESH_IDLE;
static unsigned long refresh_last_proof_ms = 0;
static unsigned long refresh_last_pulse_ms = 0;
static unsigned long refresh_last_wake_send_ms = 0;
static uint8_t refresh_pulse_count = 0;
static bool refresh_rx_stale_suppressed = false;
static bool refresh_wake_sent = false;
static unsigned long refresh_done_ms = 0;
static unsigned long refresh_last_ui_pol_ms = 0;
static unsigned long refresh_failed_shown_ms = 0;
static unsigned long refresh_wake_pending_start_ms = 0;
static unsigned long refresh_wake_pending_last_ping_ms = 0;
static unsigned long refresh_wake_pending_next_pulse_ms = 0;
static uint8_t refresh_wake_pending_attempts = 0;
static bool refresh_requested_again = false;
static bool refresh_retry_pending = false;
static uint32_t refresh_success_count = 0;
static uint32_t refresh_timeout_count = 0;
static uint32_t refresh_retry_count = 0;
static const unsigned long REFRESH_TOTAL_TIMEOUT_MS = 12000;
static const unsigned long REFRESH_PROOF_OF_LIFE_TIMEOUT_MS = 6000;
static const unsigned long REFRESH_TOTAL_MAX_MS = 30000;
static const unsigned long REFRESH_NO_UI_POL_TIMEOUT_MS = 15000;
static const unsigned long REFRESH_MAX_MS = 8000;
static const unsigned long REFRESH_FAILED_SHOW_MS = 2000;
static const unsigned long REFRESH_COMPLETE_SHOW_MS = 500;
static const unsigned long REFRESH_PULSE_COOLDOWN_MS = 1500;
static const unsigned long REFRESH_PULSE_NO_POL_MS = 1500;
static const uint8_t REFRESH_PULSE_MAX = 2;
static const unsigned long REFRESH_WAKE_WAIT_MS = 4000;
static const unsigned long REFRESH_WAKE_PING_INTERVAL_MS = 400;
static const unsigned long REFRESH_WAKE_PULSE_BACKOFF_MS = 1500;
static const uint8_t REFRESH_WAKE_MAX_ATTEMPTS = 3;
static unsigned long last_sense_msg_ms = 0;
static unsigned long last_proof_of_life_ms = 0;
static bool sense_rx_stale_logged = false;
static int lcd_wake_pin_mode = INPUT;
static int lcd_wake_pin_pullup = 0;

static uint32_t safe_age_ms(uint32_t now_ms, uint32_t then_ms);
static volatile bool waiting_for_voice_response = false;  // Track if we're waiting for voice response
static const unsigned long VOICE_RESPONSE_TIMEOUT_MS = 30000;  // 30 seconds timeout when waiting for voice
static volatile bool waiting_for_scan_response = false;  // Track if we're waiting for scan response
static const unsigned long SCAN_RESPONSE_TIMEOUT_MS = 90000;  // 90 seconds timeout when waiting for scan (image processing can take time)
static const unsigned long PROVISIONING_TIMEOUT_MS = 1800000;  // 30 minutes during provisioning
static volatile bool waiting_for_list_response = false;  // Track if we're waiting for list/API response
static const unsigned long API_RESPONSE_TIMEOUT_MS = 60000;  // 60 seconds for API waits
static bool waiting_for_sense_cmds = true;
static bool waiting_for_sense_logged = false;
// Explicit phase and timestamps for sleep-safe backoff (never block sleep in BACKOFF).
enum WifiPhase {
  WIFI_PHASE_OFF = 0,
  WIFI_PHASE_CONNECTING = 1,
  WIFI_PHASE_CONNECTED = 2,
  WIFI_PHASE_BACKOFF = 3
};
static int wifi_phase = WIFI_PHASE_OFF;
static unsigned long wifi_attempt_deadline_ms = 0;  // 0 = none; when set, attempt is active until this time
static bool wifi_on_pending = false;
static bool wifi_on_pending_logged = false;
static bool wifi_pending_budget_logged = false;
static unsigned long wifi_pending_start_ms = 0;
static unsigned long last_sleep_diag_ms = 0;
static unsigned long last_sleep_decision_log_ms = 0;
static unsigned long last_sleep_coord_status_log_ms = 0;
static int wifi_last_result = 0;  // 0=unknown, 1=ok, 2=failed
enum WifiPendingReason {
  WIFI_REASON_IDLE = 0,
  WIFI_REASON_USER_ACTION = 1,
  WIFI_REASON_OTA = 2
};
static int wifi_pending_reason = WIFI_REASON_IDLE;
static unsigned long wifi_pending_budget_ms = 0;
static const unsigned long WIFI_BUDGET_IDLE_MS = 12000;
static const unsigned long WIFI_BUDGET_USER_MS = 45000;
static const unsigned long WIFI_BUDGET_OTA_MS = 45000;
static const unsigned long WIFI_BUDGET_SAFETY_MARGIN_MS = 5000;
// Cap "active attempt" window so backoff does not block sleep (backoff clears pending via wifi_pending_clear_for_backoff).
static const unsigned long WIFI_ATTEMPT_MAX_MS = 90000;
static bool ota_pending_logged = false;
static unsigned long api_error_shown_time = 0;
static const unsigned long API_ERROR_TIMEOUT_MS = 30000;
static unsigned long last_wake_time = 0;  // Track when we last woke from sleep
static bool just_woke_up = false;  // Flag to track if we just woke from sleep (for UI reset)
// last_user_activity_ms is the single source of activity timing
static const unsigned long USER_WAKE_HOLD_MS = 10000;
static unsigned long meal_result_shown_time = 0;  // Track when meal result screen was shown
static const unsigned long MEAL_RESULT_TIMEOUT_MS = 30000;  // 30 seconds timeout for meal result screen (gives user time to read)
static volatile bool provisioning_active = false;  // Suppress sleep while provisioning UI is active
static volatile bool provision_refresh_pending = false;  // Refresh list after provisioning completes
static volatile bool refresh_request_needs_send = false;  // Send INPUT_WAKE once Sense is awake

// ── Haptics (DRV2605) ───────────────────────────────────────────────
static const uint8_t HAPTIC_ADDR = 0x5A;
static const i2c_port_t HAPTIC_I2C_PORT = I2C_NUM_0;
static const uint8_t DRV2605_REG_STATUS = 0x00;
static const uint8_t DRV2605_REG_MODE = 0x01;
static const uint8_t DRV2605_REG_RTPIN = 0x02;
static const uint8_t DRV2605_REG_LIBRARY = 0x03;
static const uint8_t DRV2605_REG_WAVESEQ1 = 0x04;
static const uint8_t DRV2605_REG_WAVESEQ2 = 0x05;
static const uint8_t DRV2605_REG_GO = 0x0C;
static const uint8_t DRV2605_REG_OVERDRIVE = 0x0D;
static const uint8_t DRV2605_REG_SUSTAINPOS = 0x0E;
static const uint8_t DRV2605_REG_SUSTAINNEG = 0x0F;
static const uint8_t DRV2605_REG_BREAK = 0x10;
static const uint8_t DRV2605_REG_AUDIOMAX = 0x13;
static const uint8_t DRV2605_REG_RATEDV = 0x16;
static const uint8_t DRV2605_REG_CLAMPV = 0x17;
static const uint8_t DRV2605_REG_FEEDBACK = 0x1A;
static const uint8_t DRV2605_REG_CONTROL3 = 0x1D;
static const uint8_t DRV2605_MODE_INTTRIG = 0x00;
static const uint8_t DRV2605_EFFECT_CLICK = 4;  // Sharp Click - 100%
static const bool HAPTIC_USE_LRA = true;
static const uint8_t HAPTIC_RATEDV_FULL = 0xFF;
static const uint8_t HAPTIC_CLAMPV_FULL = 0xFF;
static const uint8_t HAPTIC_RATEDV_SCROLL = 0x10;
static const uint8_t HAPTIC_CLAMPV_SCROLL = 0x10;
static bool haptic_ready = false;
static unsigned long last_haptic_ms = 0;
static const unsigned long HAPTIC_MIN_INTERVAL_MS = 40;

static bool haptic_write(uint8_t reg, uint8_t value) {
  uint8_t data[2] = {reg, value};
  return i2c_master_write_to_device(HAPTIC_I2C_PORT, HAPTIC_ADDR, data, sizeof(data),
                                    pdMS_TO_TICKS(20)) == ESP_OK;
}

static bool haptic_read(uint8_t reg, uint8_t* value) {
  if (!value) {
    return false;
  }
  return i2c_master_write_read_device(HAPTIC_I2C_PORT, HAPTIC_ADDR, &reg, 1, value, 1,
                                      pdMS_TO_TICKS(20)) == ESP_OK;
}

static bool haptic_write_mask(uint8_t reg, uint8_t clear_mask, uint8_t set_mask) {
  uint8_t value = 0;
  if (!haptic_read(reg, &value)) {
    return false;
  }
  value &= clear_mask;
  value |= set_mask;
  return haptic_write(reg, value);
}

static void haptic_init() {
  if (haptic_ready) {
    return;
  }
  uint8_t status = 0;
  if (!haptic_read(DRV2605_REG_STATUS, &status)) {
    return;
  }
  uint8_t chip_id = (status >> 5) & 0x07;
  if (chip_id != 0x03 && chip_id != 0x04 && chip_id != 0x06 && chip_id != 0x07) {
    return;
  }
  // Internal trigger, ERM open-loop (mirrors reference init)
  if (!haptic_write(DRV2605_REG_MODE, DRV2605_MODE_INTTRIG)) {
    return;
  }
  haptic_write(DRV2605_REG_RTPIN, 0);
  haptic_write(DRV2605_REG_LIBRARY, HAPTIC_USE_LRA ? 6 : 1);
  haptic_write(DRV2605_REG_WAVESEQ1, DRV2605_EFFECT_CLICK);
  haptic_write(DRV2605_REG_WAVESEQ2, 0);
  haptic_write(DRV2605_REG_OVERDRIVE, 0);
  haptic_write(DRV2605_REG_SUSTAINPOS, 0);
  haptic_write(DRV2605_REG_SUSTAINNEG, 0);
  haptic_write(DRV2605_REG_BREAK, 0);
  haptic_write(DRV2605_REG_AUDIOMAX, 0x64);
  // Max drive voltages (strongest output)
  haptic_write(DRV2605_REG_RATEDV, HAPTIC_RATEDV_FULL);
  haptic_write(DRV2605_REG_CLAMPV, HAPTIC_CLAMPV_FULL);
  if (HAPTIC_USE_LRA) {
    // LRA mode: set LRA bit, clear ERM open-loop
    haptic_write_mask(DRV2605_REG_FEEDBACK, 0xFF, 0x80);
    haptic_write_mask(DRV2605_REG_CONTROL3, 0xDF, 0x00);
  } else {
    // ERM open-loop
    haptic_write_mask(DRV2605_REG_FEEDBACK, 0x7F, 0x00);
    haptic_write_mask(DRV2605_REG_CONTROL3, 0xFF, 0x20);
  }
  haptic_ready = true;
}

static void haptic_pulse_with_strength(uint8_t ratedv, uint8_t clampv) {
  unsigned long now = millis();
  if (now - last_haptic_ms < HAPTIC_MIN_INTERVAL_MS) {
    return;
  }
  if (!haptic_ready) {
    haptic_init();
  }
  if (!haptic_ready) {
    return;
  }
  if (ratedv != HAPTIC_RATEDV_FULL || clampv != HAPTIC_CLAMPV_FULL) {
    haptic_write(DRV2605_REG_RATEDV, ratedv);
    haptic_write(DRV2605_REG_CLAMPV, clampv);
  }
  haptic_write(DRV2605_REG_WAVESEQ1, DRV2605_EFFECT_CLICK);
  haptic_write(DRV2605_REG_WAVESEQ2, 0);
  haptic_write(DRV2605_REG_GO, 1);
  if (ratedv != HAPTIC_RATEDV_FULL || clampv != HAPTIC_CLAMPV_FULL) {
    haptic_write(DRV2605_REG_RATEDV, HAPTIC_RATEDV_FULL);
    haptic_write(DRV2605_REG_CLAMPV, HAPTIC_CLAMPV_FULL);
  }
  last_haptic_ms = now;
}

static void haptic_pulse() {
  haptic_pulse_with_strength(HAPTIC_RATEDV_FULL, HAPTIC_CLAMPV_FULL);
}

static void haptic_pulse_scroll() {
  haptic_pulse_with_strength(HAPTIC_RATEDV_SCROLL, HAPTIC_CLAMPV_SCROLL);
}

// ── OTA Lock (Sense-coordinated) ───────────────────────────────────────
static volatile bool ota_locked = false;
static unsigned long ota_lock_at_ms = 0;
static const unsigned long OTA_LOCK_TIMEOUT_MS = 600000;  // 10 min auto-unlock safety
static volatile bool ota_check_requested = false;
static volatile bool ota_check_pending = false;
static unsigned long ota_stay_awake_until_ms = 0;
static const unsigned long OTA_STAY_AWAKE_MS = 20000;
static bool lcd_ota_request_active = false;
static uint32_t lcd_ota_request_id = 0;
static bool lcd_ota_request_allow_reboot = true;
static char lcd_ota_request_reason[24] = "";
static unsigned long lcd_ota_request_start_ms = 0;
static const unsigned long LCD_OTA_CHECK_STAY_AWAKE_MS = 600000;  // 10 min
static const unsigned long LCD_OTA_USER_ACTIVE_GRACE_MS = 120000; // 2 min

// ── Scroll Position Tracking ──────────────────────────────────────────
// Track if user has scrolled since last list update (to prevent disrupting scroll)
static volatile bool user_has_scrolled = false;

// ── Pull-to-Refresh Tracking ──────────────────────────────────────────
// Track consecutive negative scrolls when at index 0 (pull-to-refresh)
static int pull_to_refresh_counter = 0;
static const int PULL_TO_REFRESH_THRESHOLD = 5;
// Defer glowing animation to next loop iteration to avoid SPI queue overflow (list redraw + animation in one frame)
static bool defer_glowing_after_refresh = false;
static char defer_glowing_reason[32] = {0};

// ── Deleted Items Tracking ────────────────────────────────────────────
// Track IDs of items that have been deleted locally but may not yet be processed by backend
// This prevents deleted items from reappearing if a list refresh happens before backend processes delete
#define MAX_DELETED_ITEMS 20
static char deleted_item_ids[MAX_DELETED_ITEMS][64];
static int deleted_item_count = 0;

// ── Touch Detection ────────────────────────────────────────────────────
static bool touch_pressed = false;
static unsigned long touch_press_time = 0;
static uint16_t touch_press_x = 0;  // Store X coordinate when touch is pressed
static uint16_t touch_press_y = 0;  // Store Y coordinate when touch is pressed
static const unsigned long LONG_PRESS_THRESHOLD_MS = 500;  // 500ms for long press
static bool long_press_sent = false;  // Track if long press event already sent
static bool touch_used_to_dismiss_meal = false;  // Track if touch was used to dismiss meal result screen

// ── UI Elements ────────────────────────────────────────────────────
static lv_obj_t *list_container = NULL;
static lv_obj_t *list_labels[MAX_LIST_ITEMS] = {NULL};
static lv_obj_t *empty_label = NULL;
static lv_obj_t *loading_screen = NULL;
// ── Ship Menu Screen ────────────────────────────────────────────────
static lv_obj_t *ship_menu_screen = NULL;
static lv_obj_t *ship_menu_label = NULL;
static lv_obj_t *ship_menu_second_screen = NULL;
static lv_obj_t *ship_menu_settings_screen = NULL;
static lv_obj_t *ship_menu_settings_title = NULL;
static lv_obj_t *ship_menu_settings_btn_debug = NULL;
static lv_obj_t *ship_menu_settings_label_debug = NULL;
static lv_obj_t *ship_menu_settings_btn_reset = NULL;
static lv_obj_t *ship_menu_settings_label_reset = NULL;
static lv_obj_t *ship_menu_settings_btn_back = NULL;
static lv_obj_t *ship_menu_settings_label_back = NULL;
static lv_obj_t *ship_menu_press_overlay = NULL;
static unsigned long ship_menu_press_hide_at_ms = 0;
static lv_obj_t *g_status_layer = NULL;
static lv_obj_t *g_status_label = NULL;
static lv_obj_t *g_status_spinner = NULL;
static unsigned long g_status_hide_at_ms = 0;
static ship_menu_screen_t ship_menu_screen_state = SHIP_MENU_SCREEN_MAIN;
static volatile bool ui_busy = false;
static volatile ui_screen_t ui_screen_state = SCREEN_HOME;

typedef struct {
  bool valid;
  char menu_item[16];
  int menu_index;
} last_action_t;

static last_action_t g_last_action = {false, "", -1};
static unsigned long g_retry_disable_until_ms = 0;

static lv_obj_t *result_root = NULL;
static lv_obj_t *result_icon_label = NULL;
static lv_obj_t *result_title_label = NULL;
static lv_obj_t *result_subtitle_label = NULL;
static lv_obj_t *result_btn_home = NULL;
static lv_obj_t *result_btn_home_label = NULL;
static lv_obj_t *result_btn_retry = NULL;
static lv_obj_t *result_btn_retry_label = NULL;

static lv_obj_t *debug_screen = NULL;
static lv_obj_t *debug_title = NULL;
static lv_obj_t *debug_label_status = NULL;
static lv_obj_t *debug_label_status2 = NULL;
static lv_obj_t *debug_label_sense = NULL;
static lv_obj_t *debug_label_hb = NULL;
static lv_obj_t *debug_label_wifi = NULL;
static lv_obj_t *debug_label_ui = NULL;
static lv_obj_t *debug_btn_back = NULL;
static lv_obj_t *debug_btn_back_label = NULL;
static unsigned long debug_last_update_ms = 0;
static char debug_buf_status[96];
static char debug_buf_status2[96];
static char debug_buf_sense[64];
static char debug_buf_hb[48];
static char debug_buf_wifi[64];
static char debug_buf_ui[32];

static bool g_ship_ui_busy = false;
static bool g_ship_ui_terminal = false;
static bool g_ship_ui_error = false;
static char g_ship_ui_op[16] = {0};
static char g_ship_ui_phase[16] = {0};
static char g_ship_ui_text[64] = {0};
static char g_ship_ui_mode[16] = {0};
static unsigned long g_ship_ui_last_ms = 0;

// ── Meal Result Screen ──────────────────────────────────────────────
static lv_obj_t *meal_result_screen = NULL;
static lv_obj_t *meal_calories_label = NULL;  // Large calories at top
static lv_obj_t *meal_description_label = NULL;  // Meal description
static lv_obj_t *meal_protein_value_label = NULL;  // Protein value (e.g., "30g")
static lv_obj_t *meal_protein_name_label = NULL;  // Protein name (e.g., "Protein")
static lv_obj_t *meal_carbs_value_label = NULL;  // Carbs value (e.g., "30g")
static lv_obj_t *meal_carbs_name_label = NULL;  // Carbs name (e.g., "Carbs")
static lv_obj_t *meal_fat_value_label = NULL;  // Fat value (e.g., "0g")
static lv_obj_t *meal_fat_name_label = NULL;  // Fat name (e.g., "Fat")
static lv_obj_t *meal_recommendation_label = NULL;  // Recommendation at bottom

// ── Menu Buttons ────────────────────────────────────────────────────
static lv_obj_t *delete_menu = NULL;  // Menu overlay for delete button
static lv_obj_t *delete_item_btn = NULL;  // Button to delete current item
static lv_obj_t *delete_item_label = NULL;  // Label inside delete button
static lv_obj_t *menu_menu = NULL;  // Menu overlay for menu button (top third)
static lv_obj_t *menu_item_btn = NULL;  // Button for menu
static lv_obj_t *menu_item_label = NULL;  // Label inside menu button
static bool buttons_visible = false;  // Track if buttons are currently visible
static unsigned long delete_cooldown_until = 0;  // Ignore touch toggles for a short period after delete
static unsigned long menu_cooldown_until = 0;  // Ignore menu item selection for a short period after showing menu

// ── Menu Screen State (must be before menu screen UI) ────────────────
#define MENU_MAIN_ITEM_COUNT 7
#define MENU_SETTINGS_ITEM_COUNT 2
#define MENU_MAX_ITEMS 7

static const char* menu_items_main[MENU_MAIN_ITEM_COUNT] = {
  "Discard",
  "Dish",
  "Check-in",
  "Kitchen",
  "Health",
  "Settings",
  "Home"
};

static const char* menu_items_settings[MENU_SETTINGS_ITEM_COUNT] = {
  "Reset Wi-Fi",
  "Back"
};

static menu_mode_t menu_mode = MENU_MODE_MAIN;
static const char** menu_items_current = menu_items_main;
static int menu_item_count = MENU_MAIN_ITEM_COUNT;
static int menu_selected_index = 0;  // Currently selected menu item
static bool menu_screen_visible = false;  // Track if menu screen is showing
// NOTE: discard_mode_active removed - we now use mode from UI_STATUS/UI_MEAL_RESULT messages

// ── Menu Screen ──────────────────────────────────────────────────────
static lv_obj_t *menu_screen = NULL;  // Full-screen menu overlay
static lv_obj_t *menu_list_container = NULL;  // Container for menu items
static lv_obj_t *menu_item_labels[MENU_MAX_ITEMS] = {NULL};  // Labels for each menu item

// ── Ship Menu Hitboxes (tune coords here) ────────────────────────────
#define SHIP_MENU_W EXAMPLE_LCD_H_RES
#define SHIP_MENU_H EXAMPLE_LCD_V_RES
#define SHIP_MENU_MID_X (SHIP_MENU_W / 2)
#define SHIP_MENU_MID_Y (SHIP_MENU_H / 2)
#define SHIP_MENU_PRESS_MS 150

// Main Menu icon bounds
#define SHIP_MENU_MAIN_ICON_W 120
#define SHIP_MENU_MAIN_ICON_H 120
#define SHIP_MENU_MAIN_PAD 10
#define SHIP_MENU_MAIN_TOP_X (SHIP_MENU_MID_X - (SHIP_MENU_MAIN_ICON_W / 2))
#define SHIP_MENU_MAIN_TOP_Y SHIP_MENU_MAIN_PAD
#define SHIP_MENU_MAIN_LEFT_X SHIP_MENU_MAIN_PAD
#define SHIP_MENU_MAIN_LEFT_Y (SHIP_MENU_MID_Y - (SHIP_MENU_MAIN_ICON_H / 2))
#define SHIP_MENU_MAIN_RIGHT_X (SHIP_MENU_W - SHIP_MENU_MAIN_PAD - SHIP_MENU_MAIN_ICON_W)
#define SHIP_MENU_MAIN_RIGHT_Y (SHIP_MENU_MID_Y - (SHIP_MENU_MAIN_ICON_H / 2))
#define SHIP_MENU_MAIN_BOTTOM_X (SHIP_MENU_MID_X - (SHIP_MENU_MAIN_ICON_W / 2))
#define SHIP_MENU_MAIN_BOTTOM_Y (SHIP_MENU_H - SHIP_MENU_MAIN_PAD - SHIP_MENU_MAIN_ICON_H)

// Second Menu icon bounds (HOME center, SETTINGS bottom)
#define SHIP_MENU_SECOND_HOME_W 130
#define SHIP_MENU_SECOND_HOME_H 130
#define SHIP_MENU_SECOND_HOME_X (SHIP_MENU_MID_X - (SHIP_MENU_SECOND_HOME_W / 2))
#define SHIP_MENU_SECOND_HOME_Y (SHIP_MENU_MID_Y - (SHIP_MENU_SECOND_HOME_H / 2))
#define SHIP_MENU_SECOND_SETTINGS_W SHIP_MENU_MAIN_ICON_W
#define SHIP_MENU_SECOND_SETTINGS_H SHIP_MENU_MAIN_ICON_H
#define SHIP_MENU_SECOND_SETTINGS_X SHIP_MENU_MAIN_BOTTOM_X
#define SHIP_MENU_SECOND_SETTINGS_Y SHIP_MENU_MAIN_BOTTOM_Y

// Settings screen button bounds
#define SHIP_MENU_SETTINGS_BTN_W 260
#define SHIP_MENU_SETTINGS_BTN_H 48
#define SHIP_MENU_SETTINGS_BTN_X ((SHIP_MENU_W - SHIP_MENU_SETTINGS_BTN_W) / 2)
#define SHIP_MENU_SETTINGS_DEBUG_Y 80
#define SHIP_MENU_SETTINGS_RESET_Y 140
#define SHIP_MENU_SETTINGS_BACK_Y 210

#define MENU_INDEX_DISCARD 0
#define MENU_INDEX_DISH 1
#define MENU_INDEX_CHECK_IN 2
#define MENU_INDEX_SETTINGS 5

typedef struct {
  ship_menu_action_t action;
  const char* action_name;   // For log: [MENU] tap=<ACTION>
  const char* menu_item;     // For INPUT_MENU_SELECT
  int menu_index;            // For INPUT_MENU_SELECT
  int x1;
  int y1;
  int x2;
  int y2;
} ship_menu_hitbox_t;

static const ship_menu_hitbox_t ship_menu_hitboxes_main[] = {
  {SHIP_MENU_ACTION_LOG_DISH,  "LOG_DISH",  "Dish",     MENU_INDEX_DISH,
   SHIP_MENU_MAIN_TOP_X, SHIP_MENU_MAIN_TOP_Y,
   SHIP_MENU_MAIN_TOP_X + SHIP_MENU_MAIN_ICON_W - 1, SHIP_MENU_MAIN_TOP_Y + SHIP_MENU_MAIN_ICON_H - 1},
  {SHIP_MENU_ACTION_CHECK_IN,  "CHECK_IN",  "Check-in", MENU_INDEX_CHECK_IN,
   SHIP_MENU_MAIN_LEFT_X, SHIP_MENU_MAIN_LEFT_Y,
   SHIP_MENU_MAIN_LEFT_X + SHIP_MENU_MAIN_ICON_W - 1, SHIP_MENU_MAIN_LEFT_Y + SHIP_MENU_MAIN_ICON_H - 1},
  {SHIP_MENU_ACTION_CHECK_OUT, "CHECK_OUT", "Discard",  MENU_INDEX_DISCARD,
   SHIP_MENU_MAIN_RIGHT_X, SHIP_MENU_MAIN_RIGHT_Y,
   SHIP_MENU_MAIN_RIGHT_X + SHIP_MENU_MAIN_ICON_W - 1, SHIP_MENU_MAIN_RIGHT_Y + SHIP_MENU_MAIN_ICON_H - 1},
  {SHIP_MENU_ACTION_MORE,      "MORE",      NULL,       -1,
   SHIP_MENU_MAIN_BOTTOM_X, SHIP_MENU_MAIN_BOTTOM_Y,
   SHIP_MENU_MAIN_BOTTOM_X + SHIP_MENU_MAIN_ICON_W - 1, SHIP_MENU_MAIN_BOTTOM_Y + SHIP_MENU_MAIN_ICON_H - 1}
};

static const ship_menu_hitbox_t ship_menu_hitboxes_second[] = {
  {SHIP_MENU_ACTION_HOME,     "HOME",     NULL, -1,
   SHIP_MENU_SECOND_HOME_X, SHIP_MENU_SECOND_HOME_Y,
   SHIP_MENU_SECOND_HOME_X + SHIP_MENU_SECOND_HOME_W - 1, SHIP_MENU_SECOND_HOME_Y + SHIP_MENU_SECOND_HOME_H - 1},
  {SHIP_MENU_ACTION_SETTINGS, "SETTINGS", NULL, -1,
   SHIP_MENU_SECOND_SETTINGS_X, SHIP_MENU_SECOND_SETTINGS_Y,
   SHIP_MENU_SECOND_SETTINGS_X + SHIP_MENU_SECOND_SETTINGS_W - 1, SHIP_MENU_SECOND_SETTINGS_Y + SHIP_MENU_SECOND_SETTINGS_H - 1}
};

static const ship_menu_hitbox_t ship_menu_hitboxes_settings[] = {
  {SHIP_MENU_ACTION_DEBUG, "DEBUG", NULL, -1,
   SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_DEBUG_Y,
   SHIP_MENU_SETTINGS_BTN_X + SHIP_MENU_SETTINGS_BTN_W - 1, SHIP_MENU_SETTINGS_DEBUG_Y + SHIP_MENU_SETTINGS_BTN_H - 1},
  {SHIP_MENU_ACTION_RESET_WIFI, "RESET_WIFI", NULL, -1,
   SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_RESET_Y,
   SHIP_MENU_SETTINGS_BTN_X + SHIP_MENU_SETTINGS_BTN_W - 1, SHIP_MENU_SETTINGS_RESET_Y + SHIP_MENU_SETTINGS_BTN_H - 1},
  {SHIP_MENU_ACTION_BACK, "BACK", NULL, -1,
   SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_BACK_Y,
   SHIP_MENU_SETTINGS_BTN_X + SHIP_MENU_SETTINGS_BTN_W - 1, SHIP_MENU_SETTINGS_BACK_Y + SHIP_MENU_SETTINGS_BTN_H - 1}
};

// ── Logged Screen (for Discard mode) ─────────────────────────────────
static lv_obj_t *logged_screen = NULL;  // Screen shown after discard image capture
static lv_obj_t *logged_label = NULL;  // "Logged!" text label
static unsigned long logged_screen_shown_time = 0;  // Track when logged screen was shown
static const unsigned long LOGGED_SCREEN_TIMEOUT_MS = 2000;  // 2 seconds timeout

// ── Expiration Date Entry Screen (for Check-in mode) ───────────────────
static lv_obj_t *expiry_screen = NULL;  // Screen for entering expiration date
static lv_obj_t *expiry_date_label = NULL;  // Label showing current date input (e.g., "2026-12-31")
static lv_obj_t *expiry_keypad_buttons[10] = {NULL};  // Number buttons 0-9
static lv_obj_t *expiry_check_button = NULL;  // Check button to submit
static lv_obj_t *expiry_backspace_button = NULL;  // Backspace button
static char expiry_date_buffer[11] = "__-__-____";  // Date buffer (MM-DD-YYYY format for display)
static int expiry_date_pos = 0;  // Current position in date (0-9, skipping dashes)
static bool expiry_screen_visible = false;  // Track if expiration date screen is showing
static unsigned long expiry_screen_shown_time = 0;  // Track when expiry screen was shown (for timeout)
static const unsigned long EXPIRY_SCREEN_TIMEOUT_MS = 30000;  // 30 seconds timeout (matches Sense board timeout)

// ── Long Press Halo ────────────────────────────────────────────────────
static lv_obj_t *recording_indicator = NULL;  // Solid halo/ring for long press recording

// ── Processing Halo (Glowing/Pulsing) ──────────────────────────────────────
static lv_obj_t *processing_indicator = NULL;  // Processing indicator (glowing halo)
static lv_anim_t *processing_anim = NULL;  // Animation for pulsing effect
static bool is_glowing_animation = false;  // Track if glowing animation is active
static unsigned long processing_start_ms = 0;
static char processing_op[24] = "none";
static const unsigned long PROCESSING_WATCHDOG_MS = 15000;
static volatile unsigned long lvgl_timer_calls = 0;
static unsigned long last_ui_heartbeat_ms = 0;
static volatile unsigned long ui_heartbeat_counter = 0;
static int g_backlight_duty = 255;
static bool g_panel_enabled = true;
static bool g_lvgl_running = true;
static TaskHandle_t ui_task_handle = NULL;
static bool g_ota_mode_active = false;

// ── Status Screen ──────────────────────────────────────────────────────────
static lv_obj_t *status_screen = NULL;  // Status screen for "On it!", "Hold still!", etc.
static lv_obj_t *status_label = NULL;  // Large text label for status messages
static lv_obj_t *status_reset_button = NULL;  // Reset Wi-Fi button on error screen
static lv_obj_t *status_reset_label = NULL;
static unsigned long status_screen_shown_time = 0;  // Track when status screen was shown
static const unsigned long STATUS_SCREEN_TIMEOUT_MS = 1000;  // 1 second for "On it!" message
static bool status_reset_visible = false;

// ── Provisioning Screen (QR) ─────────────────────────────────────────
static lv_obj_t *provision_screen = NULL;
static lv_obj_t *provision_qr = NULL;
static lv_obj_t *provision_title_label = NULL;
static lv_obj_t *provision_ssid_label = NULL;
static lv_obj_t *provision_url_label = NULL;
static lv_obj_t *provision_status_label = NULL;
static bool provision_screen_visible = false;

// ── Knob Handle ─────────────────────────────────────────────────────
static knob_handle_t s_knob = NULL;

// ── Event Queue ────────────────────────────────────────────────────
typedef enum {
  EVT_SCROLL_DELTA,
  EVT_LIST_REPLACED,
  EVT_RENDER_ACTIVE_LIST,
  EVT_TOGGLE_BUTTONS,
  EVT_VOICE_ITEMS_ADDED,  // Optimistic voice items added - refresh UI without swapping
  EVT_SHOW_MEAL_RESULT,  // Show meal result screen
  EVT_MENU_SELECTED,  // Menu item selected
  EVT_SHOW_PROVISION_QR,
  EVT_HIDE_PROVISION_QR,
  EVT_UPDATE_PROVISION_STATUS,
  EVT_RESET_UI,
  EVT_HAPTIC_TICK,
  EVT_STOP_GLOWING,   // From UART: UI task calls stop_glowing_animation + lv_timer_handler
  EVT_START_GLOWING,  // From UART: UI task calls start_glowing_animation(reason)
  EVT_UI_STATUS_IDLE, // From UART: UI task calls set_status_reset_visible(false), stop_glowing if needed
  EVT_SHIP_UI_STATUS, // From UART: Ship menu UI_STATUS -> update overlay/result
} app_event_type_t;

typedef struct app_event_t {
  app_event_type_t type;
  union {
    int8_t scroll_delta;
    int new_count;
    int menu_index;  // For EVT_MENU_SELECTED
    char glow_reason[32];  // For EVT_START_GLOWING
    struct {
      int calories;
      float protein_g;
      float carbs_g;
      float fat_g;
      char meal_summary[256];
      char recommendation[256];
    } meal_result;
    struct {
      char ssid[33];
      char password[65];
      char url[64];
    } provision;
    struct {
      char state[32];
    } provision_status;
  } data;
} app_event_t;

static QueueHandle_t app_event_queue = NULL;

// ── UART TX Queue ────────────────────────────────────────────────────
typedef struct {
  char type[24];
  int delta;
  char id[40];
  bool has_delta;
  bool has_id;
} tx_msg_t;

static QueueHandle_t uart_tx_queue = NULL;

#ifdef HALO_LCD_PROD_WRAPPER
bool halo_lcd_prod_wifi_ok();
bool halo_lcd_prod_wifi_connecting();
bool halo_lcd_prod_wifi_failed();
int halo_lcd_prod_wifi_status();
unsigned long halo_lcd_prod_next_wifi_retry_ms();
bool halo_lcd_prod_wifi_retry_ready();
void halo_lcd_prod_wifi_schedule_backoff(const char* reason);
void halo_lcd_prod_wifi_abort(const char* reason);
#endif

static bool lcd_wifi_connected() {
#ifdef HALO_LCD_PROD_WRAPPER
  return halo_lcd_prod_wifi_ok();
#else
  return false;
#endif
}

static bool lcd_wifi_connecting() {
#ifdef HALO_LCD_PROD_WRAPPER
  return halo_lcd_prod_wifi_connecting();
#else
  return false;
#endif
}

static unsigned long wifi_budget_for_reason(int reason) {
#ifdef HALO_LCD_PROD_WRAPPER
  unsigned long computed = halo_lcd_prod_wifi_budget_ms(WIFI_BUDGET_SAFETY_MARGIN_MS);
  if (computed > 0) {
    return computed;
  }
#endif
  switch (reason) {
    case WIFI_REASON_USER_ACTION:
      return WIFI_BUDGET_USER_MS;
    case WIFI_REASON_OTA:
      return WIFI_BUDGET_OTA_MS;
    case WIFI_REASON_IDLE:
    default:
      return WIFI_BUDGET_IDLE_MS;
  }
}

static int wifi_reason_priority(int reason) {
  switch (reason) {
    case WIFI_REASON_OTA:
      return 2;
    case WIFI_REASON_USER_ACTION:
      return 1;
    case WIFI_REASON_IDLE:
    default:
      return 0;
  }
}

static void wifi_pending_start(int reason) {
  unsigned long budget_ms = wifi_budget_for_reason(reason);
  if (wifi_on_pending) {
    if (reason == wifi_pending_reason) {
      return;
    }
    if (wifi_reason_priority(reason) <= wifi_reason_priority(wifi_pending_reason)) {
      return;
    }
  }
  wifi_on_pending = true;
  wifi_pending_reason = reason;
  wifi_pending_budget_ms = budget_ms;
  wifi_pending_start_ms = millis();
  wifi_on_pending_logged = false;
  wifi_pending_budget_logged = false;
  wifi_phase = WIFI_PHASE_CONNECTING;
  unsigned long cap = (budget_ms > WIFI_ATTEMPT_MAX_MS) ? WIFI_ATTEMPT_MAX_MS : budget_ms;
  wifi_attempt_deadline_ms = wifi_pending_start_ms + cap;
}

static bool wifi_on_ui_idle() {
#if SHIP_MENU_UI
  return (!ui_busy && ui_screen_state == SCREEN_HOME);
#else
  return (lcd_mode != LCD_MODE_UI_ACTIVE);
#endif
}

static void wifi_on_execute(uint32_t timeout_ms, const char* source) {
#ifdef HALO_LCD_PROD_WRAPPER
  if (!halo_lcd_prod_wifi_retry_ready()) {
    unsigned long next_ms = halo_lcd_prod_next_wifi_retry_ms();
    long wait_ms = next_ms > 0 ? (long)(next_ms - millis()) : -1;
    Serial.printf("[LCD_WIFI] backoff active (%ld ms) - skipping WIFI_ON (%s)\n",
                  wait_ms,
                  source ? source : "unknown");
    if (status_screen != NULL && status_label != NULL) {
      status_screen_use_text("Wi-Fi down");
      lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
      status_screen_shown_time = millis();
      api_error_shown_time = status_screen_shown_time;
      set_status_reset_visible(false);
      lv_timer_handler();
    }
  } else {
    halo_lcd_prod_on_wifi_on(timeout_ms);
    int reason = WIFI_REASON_IDLE;
    if (ota_check_requested || ota_check_pending) {
      reason = WIFI_REASON_OTA;
    } else if (refresh_request_pending || waiting_for_list_response ||
               provision_refresh_pending || provisioning_active ||
               user_activity_since_sleep) {
      reason = WIFI_REASON_USER_ACTION;
    }
    wifi_pending_start(reason);
    wifi_last_result = 0;
  }
#else
  (void)timeout_ms;
  (void)source;
  Serial.println("[UART][WIFI_ON] ignored (no prod wrapper)");
#endif
}

static void wifi_on_run_deferred_if_ready(const char* reason) {
  if (!wifi_on_deferred) {
    return;
  }
  if (!wifi_on_ui_idle() && !g_sleep_transition) {
    if (!wifi_on_deferred_logged) {
      Serial.println("[LCD_WIFI] deferred WIFI_ON waiting for idle");
      wifi_on_deferred_logged = true;
    }
    return;
  }
  uint32_t timeout_ms = (uint32_t)wifi_on_deferred_timeout_ms;
  wifi_on_deferred = false;
  wifi_on_deferred_logged = false;
  Serial.printf("[LCD_WIFI] running deferred WIFI_ON (%s)\n", reason ? reason : "idle");
  wifi_on_execute(timeout_ms, reason);
}

// Called by halo_lcd_prod when backoff is scheduled. Transitions to BACKOFF and allows sleep.
void wifi_pending_clear_for_backoff(void) {
  if (!wifi_on_pending && wifi_phase != WIFI_PHASE_CONNECTING) {
    return;
  }
  Serial.println("[LCD_WIFI] wifi_on_pending=0 (backoff scheduled)");
  wifi_on_pending = false;
  wifi_pending_start_ms = 0;
  wifi_pending_reason = WIFI_REASON_IDLE;
  wifi_pending_budget_ms = WIFI_BUDGET_IDLE_MS;
  wifi_pending_budget_logged = false;
  wifi_phase = WIFI_PHASE_BACKOFF;
  wifi_attempt_deadline_ms = 0;
}

static void update_wifi_on_pending_state() {
  if (!wifi_on_pending) {
    return;
  }
  unsigned long now = millis();
  if (wifi_pending_start_ms == 0) {
    wifi_pending_start_ms = now;
    wifi_pending_budget_logged = false;
  }
  if (lcd_wifi_connected()) {
    wifi_on_pending = false;
    wifi_pending_start_ms = 0;
    wifi_phase = WIFI_PHASE_CONNECTED;
    wifi_attempt_deadline_ms = 0;
    wifi_last_result = 1;
    return;
  }
#ifdef HALO_LCD_PROD_WRAPPER
  if (!lcd_has_wifi_creds()) {
    Serial.println("[LCD_WIFI] no creds; clearing wifi_on_pending");
    wifi_on_pending = false;
    wifi_pending_start_ms = 0;
    wifi_pending_reason = WIFI_REASON_IDLE;
    wifi_pending_budget_ms = WIFI_BUDGET_IDLE_MS;
    wifi_phase = WIFI_PHASE_OFF;
    wifi_attempt_deadline_ms = 0;
    wifi_last_result = 2;
    return;
  }
#endif
#ifdef HALO_LCD_PROD_WRAPPER
  if (halo_lcd_prod_wifi_failed()) {
    Serial.println("[LCD_WIFI] wifi_on_pending failed -> clearing");
    wifi_on_pending = false;
    wifi_pending_start_ms = 0;
    wifi_attempt_deadline_ms = 0;
    wifi_last_result = 2;
    // Leave wifi_phase CONNECTING so wifi_pending_clear_for_backoff() in schedule_wifi_backoff sets BACKOFF
    halo_lcd_prod_wifi_schedule_backoff("connect_failed");
    return;
  }
#endif
  unsigned long age_ms = now - wifi_pending_start_ms;
  unsigned long budget_ms = wifi_pending_budget_ms;
  if (budget_ms == 0) {
    budget_ms = wifi_budget_for_reason(wifi_pending_reason);
  }
  if (!wifi_pending_budget_logged) {
    unsigned long remaining_ms = (budget_ms > age_ms) ? (budget_ms - age_ms) : 0;
    Serial.printf("[LCD_WIFI] pending_budget reason=%d budget_ms=%lu remaining_ms=%lu\n",
                  wifi_pending_reason,
                  (unsigned long)budget_ms,
                  (unsigned long)remaining_ms);
    wifi_pending_budget_logged = true;
  }
  if (age_ms > budget_ms && !lcd_wifi_connected()) {
    Serial.println("[LCD_WIFI] wifi_on budget expired -> disabling wifi and clearing pending");
    wifi_on_pending = false;
    wifi_pending_start_ms = 0;
    wifi_pending_reason = WIFI_REASON_IDLE;
    wifi_pending_budget_ms = WIFI_BUDGET_IDLE_MS;
    wifi_attempt_deadline_ms = 0;
    wifi_last_result = 2;
#ifdef HALO_LCD_PROD_WRAPPER
    halo_lcd_prod_wifi_abort("budget_expired");
    // Leave wifi_phase CONNECTING so wifi_pending_clear_for_backoff() in schedule_wifi_backoff sets BACKOFF
    halo_lcd_prod_wifi_schedule_backoff("pending_timeout");
#endif
  }
#ifdef HALO_LCD_PROD_WRAPPER
  // In backoff (next retry in future): ensure we're not still "pending" so sleep is allowed.
  unsigned long next_retry = halo_lcd_prod_next_wifi_retry_ms();
  if (next_retry > 0 && now < next_retry) {
    if (wifi_on_pending || wifi_phase == WIFI_PHASE_CONNECTING) {
      Serial.println("[LCD_WIFI] wifi_on_pending=0 (in backoff)");
      wifi_on_pending = false;
      wifi_pending_start_ms = 0;
      wifi_pending_reason = WIFI_REASON_IDLE;
      wifi_pending_budget_ms = WIFI_BUDGET_IDLE_MS;
      wifi_phase = WIFI_PHASE_BACKOFF;
      wifi_attempt_deadline_ms = 0;
    }
  }
#endif
}

// ── UART Functions ──────────────────────────────────────────────────
static void init_uart() {
  Serial.println("Initializing UART to Sense board...");
  senseSerial.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.printf("UART initialized: TX=GPIO%d, RX=GPIO%d, Baud=%d\n", 
                UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
  uart_rx_line_pos = 0;
}

static uint32_t get_next_msg_id() {
  uint32_t id = lcd_msg_id_counter++;
  if (lcd_msg_id_counter == 0) {
    lcd_msg_id_counter = 1;
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

static void ship_menu_handle_ui_status(const JsonDocument& doc);

static void request_sense_wake(const char* reason);

// UART send function - ONLY called from uart_task, NEVER from ISR
static void uart_send_input_message(const char* type, int delta = 0, const char* id = NULL) {
  auto input_requires_sense = [](const char* msg_type) -> bool {
    if (!msg_type) return false;
    return strcmp(msg_type, "INPUT_WAKE") == 0 ||
           strcmp(msg_type, "INPUT_DELETE") == 0 ||
           strcmp(msg_type, "INPUT_LONG_PRESS_START") == 0 ||
           strcmp(msg_type, "INPUT_LONG_PRESS_END") == 0 ||
           strcmp(msg_type, "INPUT_MENU_SELECT") == 0 ||
           strcmp(msg_type, "INPUT_RESET_WIFI") == 0;
  };
  if (input_requires_sense(type)) {
    request_sense_wake(type);
  }
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = type;
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (delta != 0) {
    doc["delta"] = delta;
  }
  if (id != NULL && strlen(id) > 0) {
    doc["id"] = id;
  }
  
  String output;
  serializeJson(doc, output);
  senseSerial.print(output);
  senseSerial.print("\n");
  senseSerial.flush();
  Serial.printf("[PROTO] TX: %s\n", output.c_str());
}

static void uart_send_json(const char* json_str);
static void send_sense_ping();
static void start_sense_wake_handshake();

static void uart_send_ack(const char* type) {
  if (!type || !type[0]) {
    return;
  }
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = type;
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.printf("[PROTO] TX: type=%s\n", type);
}

static void uart_send_wifi_on_ack(const char* status, const char* reason) {
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "WIFI_ON_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (status && status[0]) {
    doc["status"] = status;
  }
  if (reason && reason[0]) {
    doc["reason"] = reason;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.printf("[PROTO] TX: type=WIFI_ON_ACK status=%s reason=%s\n",
                status ? status : "", reason ? reason : "");
}

static void uart_send_wifi_creds_ack(const char* status, int err_code) {
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "WIFI_CREDS_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["status"] = (status && status[0]) ? status : "ERR";
  doc["err_code"] = err_code;
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.printf("[PROTO] TX: type=WIFI_CREDS_ACK status=%s err_code=%d\n",
                doc["status"].as<const char*>(), err_code);
}

// UART send function for arbitrary JSON (e.g., expiration date)
// Can be called from loop() - sends directly to serial
static void uart_send_json(const char* json_str) {
  if (json_str == NULL || strlen(json_str) == 0) {
    return;
  }
  senseSerial.print(json_str);
  senseSerial.print("\n");
  senseSerial.flush();
  Serial.printf("[PROTO] TX: %s\n", json_str);
}

static void lcd_send_ota_done(const char* result, const char* version) {
  StaticJsonDocument<192> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "LCD_OTA_DONE";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (result && result[0]) {
    doc["result"] = result;
  }
  if (version && version[0]) {
    doc["version"] = version;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void lcd_send_ota_check_ack(uint32_t request_id) {
  StaticJsonDocument<160> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "OTA_CHECK_ACK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (request_id > 0) {
    doc["request_id"] = request_id;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void lcd_send_ota_check_result(uint32_t request_id,
                                      const char* result,
                                      const char* detail,
                                      const char* new_version,
                                      const char* err_code) {
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "OTA_CHECK_RESULT";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (request_id > 0) {
    doc["request_id"] = request_id;
  }
  if (result && result[0]) {
    doc["result"] = result;
  }
  if (detail && detail[0]) {
    doc["detail"] = detail;
  }
  if (new_version && new_version[0]) {
    doc["new_version"] = new_version;
  }
  if (err_code && err_code[0]) {
    doc["err_code"] = err_code;
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void lcd_ota_request_finish(const char* result,
                                   const char* detail,
                                   const char* new_version,
                                   const char* err_code) {
  if (!lcd_ota_request_active) {
    return;
  }
  lcd_send_ota_check_result(lcd_ota_request_id, result, detail, new_version, err_code);
  lcd_ota_request_active = false;
  lcd_ota_request_id = 0;
  lcd_ota_request_allow_reboot = true;
}

static void lcd_finish_maintenance(const char* result) {
  if (!g_lcd_maintenance_active) {
    return;
  }
  lcd_send_ota_done(result, kFirmwareVersion ? kFirmwareVersion : "");
  g_lcd_maintenance_active = false;
  g_lcd_maintenance_started = false;
  g_lcd_maintenance_aborted = false;
  lcd_mode = LCD_MODE_UI_ACTIVE;
}

static void lcd_uart_reset_rx_state() {
  uart_rx_line_pos = 0;
  uart_rx_line_buffer[0] = '\0';
  int dropped = 0;
  while (senseSerial.available() > 0 && dropped < 256) {
    (void)senseSerial.read();
    dropped++;
  }
}

// Convert MM-DD-YYYY format to YYYY-MM-DD format
// Input: "MM-DD-YYYY" (10 chars)
// Output: "YYYY-MM-DD" (10 chars)
static void convert_date_mmddyyyy_to_yyyymmdd(const char* input, char* output, size_t output_size) {
  if (input == NULL || output == NULL || output_size < 11) {
    return;
  }
  
  // Input format: MM-DD-YYYY (positions: 0-1=MM, 2=-, 3-4=DD, 5=-, 6-9=YYYY)
  // Output format: YYYY-MM-DD (positions: 0-3=YYYY, 4=-, 5-6=MM, 7=-, 8-9=DD)
  
  // Extract components
  char month[3] = {input[0], input[1], '\0'};
  char day[3] = {input[3], input[4], '\0'};
  char year[5] = {input[6], input[7], input[8], input[9], '\0'};
  
  // Build output: YYYY-MM-DD
  snprintf(output, output_size, "%s-%s-%s", year, month, day);
}

static void user_activity_bump(const char* reason) {
  last_user_activity_ms = millis();
  last_touch_or_input_ms = millis();
  user_activity_since_sleep = true;
  if (sleep_retry_requires_user) {
    sleep_retry_requires_user = false;
    sleep_handshake_fail_count = 0;
    sleep_retry_allowed_ms = 0;
    Serial.println("[SLEEP] retry_unblocked reason=user_activity");
  }
  if (g_lcd_maintenance_active) {
    g_lcd_maintenance_aborted = true;
  }
  Serial.printf("[USER_ACTIVITY] reason=%s t=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)last_user_activity_ms);
}

static const char* sense_state_name(SenseState state) {
  switch (state) {
    case SENSE_AWAKE:
      return "AWAKE";
    case SENSE_ASLEEP:
      return "ASLEEP";
    case SENSE_UNKNOWN:
    default:
      return "UNKNOWN";
  }
}

static void sense_state_set(SenseState next, const char* reason) {
  if (sense_state == next) {
    return;
  }
  sense_state = next;
  sense_awake_estimate = (sense_state == SENSE_AWAKE);
  Serial.printf("[SENSE_STATE] state=%s reason=%s missed=%u\n",
                sense_state_name(sense_state),
                reason ? reason : "unknown",
                (unsigned)sense_missed_pongs);
}

static void set_sense_awake_estimate(bool value, const char* reason) {
  if (value) {
    sense_missed_pongs = 0;
    sense_pong_pending = false;
    sense_pong_deadline_ms = 0;
  }
  sense_state_set(value ? SENSE_AWAKE : SENSE_UNKNOWN, reason);
}

static void note_sense_link_rx(const char* reason) {
  (void)reason;
  last_proof_of_life_ms = millis();
  sense_rx_stale_logged = false;
}

static void update_sense_awake_grace(const char* reason) {
  uint32_t now_ms = millis();
  uint32_t until = now_ms + SENSE_AWAKE_GRACE_MS;
  if (until > sense_awake_grace_until_ms) {
    sense_awake_grace_until_ms = until;
    Serial.printf("[SLEEP] grace_until=%lu reason=%s\n",
                  (unsigned long)sense_awake_grace_until_ms,
                  reason ? reason : "unknown");
  }
}

static bool should_extend_grace_reason(const char* reason) {
  if (!reason || !reason[0]) {
    return false;
  }
  if (strcmp(reason, "refresh_retry") == 0 ||
      strcmp(reason, "wake_retry") == 0 ||
      strcmp(reason, "status_sync") == 0) {
    return false;
  }
  if (strcmp(reason, "INPUT_WAKE") == 0 ||
      strcmp(reason, "refresh_request") == 0 ||
      strcmp(reason, "provision_refresh") == 0) {
    return !refresh_grace_extended;
  }
  return true;
}

static void maybe_extend_sense_awake_grace(const char* reason) {
  if (!should_extend_grace_reason(reason)) {
    return;
  }
  update_sense_awake_grace(reason);
  if (strcmp(reason, "INPUT_WAKE") == 0 ||
      strcmp(reason, "refresh_request") == 0 ||
      strcmp(reason, "provision_refresh") == 0) {
    refresh_grace_extended = true;
  }
}

static void refresh_sense_awake_estimate(unsigned long now_ms) {
  if (sense_state == SENSE_AWAKE) {
    if (refresh_state == REFRESH_WAKE_PENDING || refresh_state == REFRESH_INFLIGHT) {
      return;
    }
    uint32_t pol_age = last_proof_of_life_ms > 0
                           ? safe_age_ms((uint32_t)now_ms, (uint32_t)last_proof_of_life_ms)
                           : 0;
    uint32_t stale_threshold_ms = (last_proof_of_life_ms > 0)
                                    ? SENSE_UNKNOWN_STALE_EXTENDED_MS
                                    : SENSE_UNKNOWN_STALE_MS;
    if (pol_age >= stale_threshold_ms) {
      if (!sense_rx_stale_logged) {
        Serial.printf("[SENSE_LINK] rx_stale age_ms=%lu threshold_ms=%lu\n",
                      (unsigned long)pol_age,
                      (unsigned long)stale_threshold_ms);
        sense_rx_stale_logged = true;
      }
      sense_state_set(SENSE_UNKNOWN, "rx_stale");
    }
  }
}

// Forward declarations (used by refresh state machine)
static void status_screen_use_text(const char* text);
static void set_status_reset_visible(bool show);
static void start_glowing_animation(const char* op);
static void pulseWakeSenseShort();
static void send_wifi_status();

static uint32_t safe_age_ms(uint32_t now_ms, uint32_t then_ms) {
  return (now_ms >= then_ms) ? (now_ms - then_ms) : 0;
}

static const char* refresh_state_name(RefreshState state) {
  switch (state) {
    case REFRESH_IDLE:
      return "IDLE";
    case REFRESH_WAKE_PENDING:
      return "WAKE_PENDING";
    case REFRESH_INFLIGHT:
      return "REFRESH_INFLIGHT";
    case REFRESH_COMPLETE:
      return "REFRESH_COMPLETE";
    case REFRESH_FAILED:
      return "REFRESH_FAILED";
    default:
      return "UNKNOWN";
  }
}

static const unsigned long LCD_STATE_LOG_INTERVAL_MS = 1000;
static unsigned long last_lcd_state_log_ms = 0;
static char last_lcd_state[24] = "";
static char last_lcd_state_reason[32] = "";

static const char* lcd_state_for_log(char* reason,
                                     size_t reason_len,
                                     unsigned long now_ms,
                                     unsigned long idle_age_ms,
                                     unsigned long effective_timeout_ms) {
  if (!reason || reason_len == 0) {
    return "UNKNOWN";
  }
  reason[0] = '\0';
  if (in_cold_boot_grace()) {
    strncpy(reason, "cold_boot_grace", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "BOOT_GRACE";
  }
  if (provisioning_active || provision_screen_visible) {
    strncpy(reason, provisioning_active ? "provisioning" : "provision_screen", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "PROVISIONING";
  }
  if (ota_locked) {
    strncpy(reason, "ota_locked", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "OTA";
  }
  if (ota_check_requested || ota_check_pending) {
    strncpy(reason, "ota_pending", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "OTA";
  }
  if (now_ms < ota_stay_awake_until_ms) {
    strncpy(reason, "ota_stay_awake", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "OTA";
  }
  if (waiting_for_voice_response) {
    strncpy(reason, "voice_wait", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "VOICE_WAIT";
  }
  if (waiting_for_scan_response) {
    strncpy(reason, "scan_wait", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "SCAN_WAIT";
  }
  if (waiting_for_list_response ||
      refresh_request_pending ||
      lcd_refresh_inflight ||
      refresh_state != REFRESH_IDLE) {
    snprintf(reason, reason_len, "refresh_%s", refresh_state_name(refresh_state));
    return "LIST_REFRESH";
  }
  if (status_screen != NULL && status_label != NULL &&
      !lv_obj_has_flag(status_screen, LV_OBJ_FLAG_HIDDEN)) {
    const char* current_text = lv_label_get_text(status_label);
    if (current_text &&
        (strcmp(current_text, "Refreshing…") == 0 ||
         strcmp(current_text, "Refreshing...") == 0)) {
      strncpy(reason, "status_refreshing", reason_len - 1);
      reason[reason_len - 1] = '\0';
      return "PROCESSING";
    }
  }
  if (is_glowing_animation) {
    strncpy(reason, "glowing_anim", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "PROCESSING";
  }
  if (wifi_on_pending && !lcd_wifi_connected()) {
    strncpy(reason, "wifi_on_pending", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "WIFI_PENDING";
  }
  if (lcd_wifi_connecting() && lcd_has_wifi_creds()) {
    strncpy(reason, "wifi_connecting", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "WIFI_CONNECTING";
  }
  if (now_ms < sense_awake_grace_until_ms) {
    strncpy(reason, "sense_grace", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "SENSE_GRACE";
  }
  if (idle_age_ms < effective_timeout_ms) {
    strncpy(reason, "idle_wait", reason_len - 1);
    reason[reason_len - 1] = '\0';
    return "IDLE_WAIT";
  }
  strncpy(reason, "idle_eligible", reason_len - 1);
  reason[reason_len - 1] = '\0';
  return "IDLE_ELIGIBLE";
}

static void lcd_log_state(unsigned long now_ms,
                          unsigned long idle_age_ms,
                          unsigned long effective_timeout_ms) {
  char reason[32];
  const char* state = lcd_state_for_log(reason, sizeof(reason),
                                        now_ms, idle_age_ms, effective_timeout_ms);
  bool changed = (strcmp(state, last_lcd_state) != 0) ||
                 (strcmp(reason, last_lcd_state_reason) != 0);
  if (changed || (now_ms - last_lcd_state_log_ms) >= LCD_STATE_LOG_INTERVAL_MS) {
    Serial.printf("[LCD_STATE] now=%lu state=%s reason=%s idle_age_ms=%lu timeout_ms=%lu\n",
                  now_ms,
                  state,
                  reason[0] ? reason : "-",
                  idle_age_ms,
                  effective_timeout_ms);
    strncpy(last_lcd_state, state, sizeof(last_lcd_state) - 1);
    last_lcd_state[sizeof(last_lcd_state) - 1] = '\0';
    strncpy(last_lcd_state_reason, reason, sizeof(last_lcd_state_reason) - 1);
    last_lcd_state_reason[sizeof(last_lcd_state_reason) - 1] = '\0';
    last_lcd_state_log_ms = now_ms;
  }
}

static bool sleep_blocked_for_ota() {
  unsigned long now_ms = millis();
  if (ota_check_requested || ota_check_pending) {
    return true;
  }
  if (now_ms < ota_stay_awake_until_ms) {
    return true;
  }
  return false;
}

static void refresh_sm_log(const char* reason, unsigned long now_ms) {
  uint32_t pol_age = refresh_last_proof_ms > 0
                         ? safe_age_ms((uint32_t)now_ms, (uint32_t)refresh_last_proof_ms)
                         : 0;
  Serial.printf("[REFRESH_SM] state=%s reason=%s now=%lu start=%lu pol_age=%lu\n",
                refresh_state_name(refresh_state),
                reason ? reason : "unknown",
                now_ms,
                lcd_refresh_start_ms,
                pol_age);
}

static void refresh_sm_set_state(RefreshState state, const char* reason) {
  unsigned long now_ms = millis();
  refresh_state = state;
  if (state == REFRESH_INFLIGHT) {
    lcd_refresh_start_ms = now_ms;
    refresh_last_proof_ms = now_ms;
    refresh_last_ui_pol_ms = now_ms;
    refresh_last_pulse_ms = 0;
    refresh_pulse_count = 0;
    refresh_rx_stale_suppressed = false;
    refresh_wake_sent = false;
    refresh_last_wake_send_ms = 0;
    refresh_done_ms = 0;
    last_proof_of_life_ms = now_ms;
    if (!is_glowing_animation) {
      start_glowing_animation("refresh_sm");
      lv_timer_handler();
    }
  } else if (state == REFRESH_COMPLETE || state == REFRESH_FAILED) {
    refresh_done_ms = now_ms;
  }
  refresh_sm_log(reason, now_ms);
}

static void refresh_soft_fail(const char* reason) {
  unsigned long now_ms = millis();
  Serial.printf("[REFRESH_SM] soft_fail reason=%s\n", reason ? reason : "unknown");
  if (is_glowing_animation) {
    stop_glowing_animation();
    lv_timer_handler();
  }
  if (status_screen != NULL) {
    lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  }
  status_screen_shown_time = 0;
  api_error_shown_time = 0;
  refresh_failed_shown_ms = 0;
  set_status_reset_visible(false);

  refresh_state = REFRESH_IDLE;
  refresh_sm_log(reason, now_ms);

  lcd_refresh_inflight = false;
  waiting_for_list_response = false;
  refresh_request_pending = false;
  refresh_request_needs_send = false;
  refresh_input_wake_sent = false;
  refresh_request_retry_count = 0;
  refresh_grace_extended = false;
  lcd_refresh_ack_seen = false;
  lcd_refresh_retry_count = 0;
  refresh_retry_pending = false;
  refresh_requested_again = false;
  refresh_wake_pending_attempts = 0;
  refresh_wake_pending_last_ping_ms = 0;
  refresh_wake_pending_next_pulse_ms = 0;
  refresh_wake_pending_start_ms = 0;
  refresh_wake_sent = false;
  refresh_last_ui_pol_ms = 0;
  refresh_last_proof_ms = 0;
  refresh_pulse_count = 0;
  refresh_last_pulse_ms = 0;
  refresh_last_wake_send_ms = 0;
  refresh_done_ms = 0;
}

static void refresh_sm_set_wake_pending(const char* reason) {
  unsigned long now_ms = millis();
  refresh_state = REFRESH_WAKE_PENDING;
  refresh_wake_pending_start_ms = now_ms;
  refresh_wake_pending_last_ping_ms = 0;
  refresh_wake_pending_attempts = 1;
  refresh_wake_pending_next_pulse_ms = now_ms + REFRESH_WAKE_WAIT_MS;
  refresh_wake_sent = false;
  refresh_done_ms = 0;
  refresh_sm_log(reason, now_ms);
  Serial.printf("[REFRESH] state=WAKE_PENDING start=%lu attempt=%u\n",
                now_ms,
                (unsigned)refresh_wake_pending_attempts);
  pulseWakeSenseShort();
  send_sense_ping();
  refresh_wake_pending_last_ping_ms = now_ms;
}

static void refresh_sm_on_awake_proof(const char* source) {
  if (refresh_state != REFRESH_WAKE_PENDING) {
    return;
  }
  Serial.printf("[REFRESH] awake_proof=%s -> send_refresh\n", source ? source : "unknown");
  refresh_sm_set_state(REFRESH_INFLIGHT, "awake_proof");
  if (!refresh_wake_sent) {
    tx_msg_t tx_msg = {};
    strncpy(tx_msg.type, "INPUT_WAKE", sizeof(tx_msg.type) - 1);
    if (uart_tx_queue != NULL) {
      xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
    }
    refresh_wake_sent = true;
    refresh_last_wake_send_ms = millis();
    lcd_refresh_sent_ms = refresh_last_wake_send_ms;
    Serial.println("[REFRESH] INPUT_WAKE sent");
  }
}

static void refresh_sm_proof_of_life(const char* source) {
  if (refresh_state != REFRESH_INFLIGHT) {
    return;
  }
  unsigned long now_ms = millis();
  refresh_last_proof_ms = now_ms;
  refresh_rx_stale_suppressed = false;
  Serial.printf("[REFRESH_SM] proof_of_life source=%s now=%lu\n",
                source ? source : "unknown",
                now_ms);
}

static void note_sense_proof_of_life(const char* source) {
  last_proof_of_life_ms = millis();
  sense_rx_stale_logged = false;
  sense_missed_pongs = 0;
  sense_pong_pending = false;
  sense_pong_deadline_ms = 0;
  sense_state_set(SENSE_AWAKE, source);
  refresh_sm_proof_of_life(source);
}

static void refresh_note_ui_proof(const char* source) {
  if (refresh_state == REFRESH_INFLIGHT) {
    refresh_last_ui_pol_ms = millis();
    Serial.printf("[REFRESH_SM] proof_of_life source=%s now=%lu\n",
                  source ? source : "unknown",
                  (unsigned long)refresh_last_ui_pol_ms);
  }
}

static bool sense_rx_type_is_awake_proof(const char* type) {
  if (!type || !type[0]) {
    return false;
  }
  // PONG is accepted as awake proof to avoid rx_stale during coordinated sleep.
  return strcmp(type, "SLEEP_ACK") == 0 ||
         strcmp(type, "INPUT_SLEEP_ACK") == 0 ||
         strcmp(type, "SLEEP_READY") == 0 ||
         strcmp(type, "SLEEP_BUSY") == 0 ||
         strcmp(type, "SLEEP_DENY") == 0 ||
         strcmp(type, "SYNC_ACK") == 0 ||
         strcmp(type, "UI_STATUS") == 0 ||
         strcmp(type, "PONG") == 0 ||
         strcmp(type, "LINK_HB") == 0 ||
         strcmp(type, "WIFI_CREDS") == 0 ||
         strcmp(type, "WIFI_ON") == 0 ||
         strcmp(type, "UI_LIST") == 0 ||
         strcmp(type, "UI_MEAL_RESULT") == 0 ||
         strcmp(type, "UI_VOICE_ITEMS") == 0 ||
         strcmp(type, "PROVISION_QR") == 0 ||
         strcmp(type, "PROVISION_STATUS") == 0 ||
         strcmp(type, "OTA_LOCK") == 0 ||
         strcmp(type, "OTA_UNLOCK") == 0 ||
         strcmp(type, "OTA_CHECK") == 0;
}

static bool should_wake_sense() {
  return sense_wake_explicit_request ||
         sense_status_sync_requested ||
         sense_ota_apply_required ||
         refresh_request_pending ||
         refresh_request_needs_send ||
         waiting_for_list_response ||
         waiting_for_scan_response ||
         waiting_for_voice_response ||
         provision_refresh_pending;
}

static bool ota_apply_pending() {
  return sense_ota_apply_required;
}

static bool sense_recently_heard(uint32_t within_ms) {
  if (within_ms == 0 || last_sense_any_rx_ms == 0) {
    return false;
  }
  unsigned long now = millis();
  return (now - last_sense_any_rx_ms) <= within_ms;
}

static bool lcd_wake_pins_active() {
  return (digitalRead(PIN_TOUCH_INT) == 0) ||
         (digitalRead(PIN_EC1_A) == 0) ||
         (digitalRead(PIN_EC1_B) == 0);
}

static bool wake_pin_is_active() {
  return digitalRead(LCD_WAKE_GPIO) == LCD_WAKE_LEVEL;
}

static void lcd_wake_pin_set_mode(int pin, int mode) {
  pinMode(pin, mode);
  if (pin == INT_PIN) {
    lcd_wake_pin_mode = mode;
    lcd_wake_pin_pullup = (mode == INPUT_PULLUP) ? 1 : 0;
  }
}

static void configure_sleep_sources(bool enable_ext0, uint32_t timer_sec) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (timer_sec > 0) {
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_sec) * 1000000ULL);
  }
  if (enable_ext0) {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)LCD_WAKE_GPIO, LCD_WAKE_LEVEL);
  }
  uint64_t wakeMask = buildWakeMaskForSleep();
  if (wakeMask != 0) {
    esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    wake_ext1_enabled = true;
  } else {
    wake_ext1_enabled = false;
  }
}

static void wake_line_pulse_ms(unsigned long pulse_ms, const char* reason) {
  lcd_wake_pin_set_mode(INT_PIN, OUTPUT);
  digitalWrite(INT_PIN, HIGH);
  delay(2);
  Serial.printf("[WAKE_LINE] pulse_begin gpio=%d level=0 len_ms=%lu reason=%s\n",
                (int)INT_PIN,
                (unsigned long)pulse_ms,
                reason ? reason : "unknown");
  Serial.printf("[LCD_WAKE_PIN] mode=OUT pullup=0 level=%d phase=before_pulse\n",
                digitalRead(INT_PIN));
  digitalWrite(INT_PIN, LOW);
  delay(pulse_ms);
  digitalWrite(INT_PIN, HIGH);
  delay(2);
  // Release line so Sense sees deasserted wake pin.
  lcd_wake_pin_set_mode(INT_PIN, INPUT_PULLUP);
  int level = digitalRead(INT_PIN);
  Serial.printf("[WAKE_LINE] pulse_end gpio=%d level=%d\n", (int)INT_PIN, level);
  Serial.printf("[LCD_WAKE_PIN] mode=IN pullup=1 level=%d phase=after_pulse\n", level);
}

static void pulseWakeSense() {
  Serial.println("[LCD] Pulsing INT to wake Sense...");
  wake_line_pulse_ms(WAKE_PULSE_DURATION_MS, "default");
  Serial.println("[LCD] Wake pulse sent");
}

static void pulseWakeSenseShort() {
  wake_line_pulse_ms(WAKE_PULSE_SHORT_MS, "short");
}

static bool wake_sense_for_request(const char* reason) {
  if (sense_recently_heard(1500)) {
    return false;
  }
  unsigned long now = millis();
  if (wake_retry_until_ms == 0) {
    start_sense_wake_handshake();
  }
  maybe_extend_sense_awake_grace(reason);
  last_int_pulse_ms = now;
  last_wake_retry_ms = now;
  Serial.printf("[LCD_INT] pulse_sense_wake reason=%s len_ms=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)WAKE_PULSE_SHORT_MS);
  pulseWakeSenseShort();
  send_sense_ping();
  return true;
}

static bool lcd_maybe_pulse_sense_int(const char* reason) {
  if (!should_wake_sense()) {
    Serial.printf("[LCD_INT] skip_pulse reason=%s\n", "no_pending_ops");
    return false;
  }
  if (sense_awake_confirmed) {
    Serial.printf("[LCD_INT] skip_pulse reason=%s\n", "already_awake");
    sense_status_sync_requested = false;
    return false;
  }
  unsigned long now = millis();
  if (wake_retry_until_ms == 0) {
    start_sense_wake_handshake();
  }
  if (now - last_int_pulse_ms < INT_PULSE_COOLDOWN_MS) {
    Serial.printf("[LCD_INT] skip_pulse reason=%s\n", "cooldown");
    return false;
  }
  maybe_extend_sense_awake_grace(reason);
  last_int_pulse_ms = now;
  Serial.printf("[LCD_INT] pulse_sense reason=%s len_ms=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)WAKE_PULSE_DURATION_MS);
  pulseWakeSense();
  send_sense_ping();
  last_wake_retry_ms = now;
  sense_status_sync_requested = false;
  sense_ota_apply_required = false;
  sense_wake_explicit_request = false;
  return true;
}

static void request_sense_wake(const char* reason) {
  sense_wake_explicit_request = true;
  maybe_extend_sense_awake_grace(reason);
  lcd_maybe_pulse_sense_int(reason);
}

static void send_sense_ping() {
  if (link_sync_pending) {
    link_sync_pending = false;
    link_synced = false;
    uart_send_input_message("SYNC");
    send_wifi_status();
  }
  unsigned long now_ms = millis();
  if (last_sense_ping_ms > 0 &&
      (now_ms - last_sense_ping_ms) < SENSE_PING_MIN_INTERVAL_MS) {
    return;
  }
  tx_msg_t tx_msg = {};
  strncpy(tx_msg.type, "INPUT_PING", sizeof(tx_msg.type) - 1);
  if (uart_tx_queue != NULL) {
    xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
  }
  last_sense_ping_ms = now_ms;
  sense_pong_pending = true;
  sense_pong_deadline_ms = now_ms + SENSE_PONG_TIMEOUT_MS;
}

static void start_sense_wake_handshake() {
  sense_awake_confirmed = false;
  wake_retry_until_ms = millis() + WAKE_RETRY_WINDOW_MS;
  last_wake_retry_ms = millis() - WAKE_RETRY_INTERVAL_MS;
}

// ── Processing Animation Functions ────────────────────────────────────
// Animation callback for glowing/pulsing halo (fades opacity in and out).
// Only touches the halo object (processing_indicator) so LVGL invalidates minimal area, not whole screen.
static void processing_glow_anim_cb(void * var, int32_t value) {
  lv_obj_t * obj = (lv_obj_t *)var;
  lv_opa_t opacity = (lv_opa_t)value;
  if (opacity > LV_OPA_COVER) opacity = LV_OPA_COVER;
  if (opacity < LV_OPA_TRANSP) opacity = LV_OPA_TRANSP;
  lv_obj_set_style_border_opa(obj, opacity, LV_PART_MAIN);
}

// Start glowing animation for processing indicator
static void start_glowing_animation(const char* op) {
  if (processing_indicator == NULL) return;
  
  // Stop any existing animation first
  stop_glowing_animation();
  
  // Show the processing indicator with border
  lv_obj_clear_flag(processing_indicator, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_border_width(processing_indicator, 12, LV_PART_MAIN);
  lv_obj_set_style_border_color(processing_indicator, lv_color_hex(0x245DFF), LV_PART_MAIN);  // Halo Blue
  lv_obj_set_style_border_opa(processing_indicator, LV_OPA_COVER, LV_PART_MAIN);  // Start at max brightness
  
  // Create animation if it doesn't exist
  if (processing_anim == NULL) {
    processing_anim = (lv_anim_t *)malloc(sizeof(lv_anim_t));
    if (processing_anim == NULL) {
      Serial.println("[ANIM] malloc(processing_anim) failed - skipping glow");
      return;
    }
  }

  lv_anim_init(processing_anim);
  lv_anim_set_var(processing_anim, processing_indicator);
  lv_anim_set_exec_cb(processing_anim, processing_glow_anim_cb);
  lv_anim_set_time(processing_anim, 1000);  // 1 second to fade from max to min
  lv_anim_set_values(processing_anim, LV_OPA_COVER, LV_OPA_20);  // From max brightness (255) to min brightness (~51)
  lv_anim_set_repeat_count(processing_anim, LV_ANIM_REPEAT_INFINITE);  // Repeat forever
  lv_anim_set_playback_time(processing_anim, 1000);  // 1 second to fade back from min to max
  lv_anim_start(processing_anim);
  
  is_glowing_animation = true;
  if (op && op[0]) {
    strncpy(processing_op, op, sizeof(processing_op) - 1);
    processing_op[sizeof(processing_op) - 1] = '\0';
  } else {
    strncpy(processing_op, "unknown", sizeof(processing_op) - 1);
    processing_op[sizeof(processing_op) - 1] = '\0';
  }
  processing_start_ms = millis();
  Serial.println("[ANIM] Started glowing processing animation");
}

// Stop glowing animation for processing indicator
static void stop_glowing_animation(void) {
  if (processing_indicator != NULL) {
    // Stop animation
    if (processing_anim != NULL) {
      lv_anim_del(processing_indicator, processing_glow_anim_cb);
    }
    // Hide and reset the indicator
    lv_obj_add_flag(processing_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width(processing_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_border_opa(processing_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_invalidate(processing_indicator);  // Minimal: only the halo object, not whole screen
  }
  is_glowing_animation = false;
  processing_start_ms = 0;
  strncpy(processing_op, "none", sizeof(processing_op) - 1);
  processing_op[sizeof(processing_op) - 1] = '\0';
  Serial.println("[ANIM] Stopped glowing processing animation");
}

static inline void ui_lvgl_tick() {
  if (!g_lvgl_running || g_sleep_transition) {
    return;
  }
  lvgl_assert_locked();
  last_ui_tick_ms = millis();
  lv_timer_handler();
  last_ui_tick_ms = millis();
  lvgl_timer_calls++;
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

static bool lcd_load_wifi_creds(char* ssid, size_t ssid_sz, char* pass, size_t pass_sz) {
#ifdef HALO_LCD_PROD_WRAPPER
  if (!ssid || !pass || ssid_sz == 0 || pass_sz == 0) {
    return false;
  }
  ssid[0] = '\0';
  pass[0] = '\0';
  return LcdWifiCreds::loadCreds(ssid, ssid_sz, pass, pass_sz);
#else
  (void)ssid;
  (void)ssid_sz;
  (void)pass;
  (void)pass_sz;
  return false;
#endif
}

static bool lcd_has_wifi_creds() {
  char ssid[33] = {0};
  char pass[65] = {0};
  return lcd_load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));
}

static void send_wifi_status() {
  char ssid[33] = {0};
  char pass[65] = {0};
  bool has_creds = lcd_load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));
  uint32_t checksum = has_creds ? wifi_creds_checksum(ssid, pass) : 0;
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "WIFI_STATUS";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["has_creds"] = has_creds ? 1 : 0;
  doc["checksum"] = checksum;
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
}

static void log_lcd_wake_pin_tick() {
  static unsigned long last_log_ms = 0;
  unsigned long now_ms = millis();
  if (now_ms - last_log_ms < 1000) {
    return;
  }
  last_log_ms = now_ms;
  int level = digitalRead(INT_PIN);
  int pullup = lcd_wake_pin_pullup;
  int mode = lcd_wake_pin_mode;
  Serial.printf("[LCD_WAKE_PIN] mode=%s pullup=%d level=%d phase=tick\n",
                mode == OUTPUT ? "OUT" : "IN",
                pullup,
                level);
}

static bool wake_line_active_for_ms(unsigned long window_ms) {
  unsigned long start_ms = millis();
  while ((millis() - start_ms) < window_ms) {
    int level = digitalRead(LCD_WAKE_GPIO);
    if (level != LCD_WAKE_LEVEL) {
      return false;
    }
    delay(10);
  }
  return true;
}

static void log_wake_pin_boot_state(const char* phase) {
  int level = digitalRead(LCD_WAKE_GPIO);
  Serial.printf("[WAKE_PIN_BOOT] board=%s gpio=%d active=%d level=%d phase=%s\n",
                HALO_BOARD_NAME,
                (int)LCD_WAKE_GPIO,
                (int)LCD_WAKE_LEVEL,
                level,
                phase ? phase : "boot");
}

static void warn_wake_pin_active_at_boot(unsigned long window_ms) {
  unsigned long start_ms = millis();
  bool stuck_active = true;
  while ((millis() - start_ms) < window_ms) {
    int level = digitalRead(LCD_WAKE_GPIO);
    if (level != LCD_WAKE_LEVEL) {
      stuck_active = false;
      break;
    }
    delay(10);
  }
  if (stuck_active) {
    Serial.printf("[WAKE_PIN_WARN] board=%s gpio=%d active_level=%d held_active_ms=%lu\n",
                  HALO_BOARD_NAME,
                  (int)LCD_WAKE_GPIO,
                  (int)LCD_WAKE_LEVEL,
                  window_ms);
  }
}

static void ensure_awake_for_ui(const char* reason) {
  // Only call this on real user input (touch/scroll/pull-to-refresh).
  if (g_ota_mode_active) {
    Serial.printf("[WAKE_UI] ignored (ota_mode) reason=%s\n", reason ? reason : "unknown");
    return;
  }
  unsigned long now_ms = millis();
  bool need_wake = g_in_light_sleep || g_sleep_transition ||
                   (g_backlight_duty == 0) || !g_panel_enabled || !g_lvgl_running;
  if (need_wake) {
    if (g_sleep_transition) {
      g_sleep_transition = false;
    }
    if (g_in_light_sleep) {
      g_in_light_sleep = false;
    }
    if (g_backlight_duty == 0) {
      lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
      g_backlight_initialized = true;
      g_backlight_duty = 255;
    }
    if (!g_panel_enabled) {
      lcd_panel_set_power(true);
      g_panel_enabled = true;
    }
    g_lvgl_running = true;
    if (app_event_queue != NULL) {
      app_event_t evt = {};
      if (g_active.count > 0) {
        evt.type = EVT_RENDER_ACTIVE_LIST;
        evt.data.new_count = g_active.count;
      } else {
        evt.type = EVT_RESET_UI;
      }
      xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(10));
    }
  }
  Serial.printf("[WAKE_UI] reason=%s backlight=%d panel_on=%d lvgl_running=%d\n",
                reason ? reason : "unknown",
                g_backlight_duty,
                g_panel_enabled ? 1 : 0,
                g_lvgl_running ? 1 : 0);
}

static bool lcd_enter_ota_mode(uint32_t min_internal_free) {
  g_ota_mode_active = true;
  g_sleep_transition = true;
  g_lvgl_running = false;
  stop_glowing_animation();
  if (ui_task_handle != NULL) {
    vTaskSuspend(ui_task_handle);
  }
  lcd_lvgl_wait_tx_done(200);
  if (lv_is_initialized()) {
    lv_obj_clean(lv_scr_act());
    lv_deinit();
  }
  if (g_lcd_initialized) {
    lcd_panel_set_power(false);
    lcd_panel_deinit();
    g_lcd_initialized = false;
  }
  if (g_backlight_initialized) {
    setUpdutySubdivide(0);
  }
  g_backlight_duty = 0;
  g_panel_enabled = false;

  const uint32_t internal_free =
      (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t internal_largest =
      (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[LCD_OTA] ota_mode_heap internal_free=%u internal_largest=%u min_free=%u\n",
                (unsigned)internal_free, (unsigned)internal_largest,
                (unsigned)min_internal_free);
  bool ok = internal_free >= min_internal_free;
  if (!ok) {
    g_ota_mode_active = false;
    g_sleep_transition = false;
    if (!g_ship_ota_wake_window) {
      if (ui_task_handle != NULL) {
        vTaskDelete(ui_task_handle);
        ui_task_handle = NULL;
      }
      g_ui_initialized = false;
      init_ui_stack(g_saved_list_count);
    }
  }
  return ok;
}

static bool processing_ops_active() {
  return waiting_for_list_response ||
         waiting_for_voice_response ||
         waiting_for_scan_response ||
         lcd_refresh_inflight;
}

static void processing_watchdog_poll() {
  if (!is_glowing_animation || processing_start_ms == 0) {
    return;
  }
  if (processing_ops_active()) {
    return;
  }
  unsigned long now_ms = millis();
  if ((now_ms - processing_start_ms) < PROCESSING_WATCHDOG_MS) {
    return;
  }
  Serial.printf("[PROCESSING] watchdog_clear op=%s age_ms=%lu pending=%d needs_send=%d sent=%d retries=%lu\n",
                processing_op,
                (unsigned long)(now_ms - processing_start_ms),
                refresh_request_pending ? 1 : 0,
                refresh_request_needs_send ? 1 : 0,
                refresh_input_wake_sent ? 1 : 0,
                refresh_request_retry_count);
  stop_glowing_animation();
  lv_timer_handler();
}

static void log_sleep_inhibit_processing(unsigned long now_ms) {
  unsigned long age_ms = processing_start_ms > 0 ? (now_ms - processing_start_ms) : 0;
  unsigned long refresh_age_ms = refresh_request_start_ms > 0 ? (now_ms - refresh_request_start_ms) : 0;
  Serial.printf("[SLEEP] inhibited reason=processing op=%s age_ms=%lu refresh_pending=%d needs_send=%d sent=%d retries=%lu refresh_age_ms=%lu wait_list=%d wait_voice=%d wait_scan=%d\n",
                processing_op,
                age_ms,
                refresh_request_pending ? 1 : 0,
                refresh_request_needs_send ? 1 : 0,
                refresh_input_wake_sent ? 1 : 0,
                refresh_request_retry_count,
                refresh_age_ms,
                waiting_for_list_response ? 1 : 0,
                waiting_for_voice_response ? 1 : 0,
                waiting_for_scan_response ? 1 : 0);
}

static bool is_wifi_error_text(const char* text) {
  if (!text) return false;
  return (strstr(text, "Wi-Fi") != NULL) ||
         (strstr(text, "wifi") != NULL) ||
         (strstr(text, "Connect failed") != NULL) ||
         (strstr(text, "connect failed") != NULL);
}

static void set_status_reset_visible(bool show) {
  if (!status_reset_button) {
    status_reset_visible = false;
    return;
  }
  status_reset_visible = show;
  if (show) {
    lv_obj_clear_flag(status_reset_button, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(status_reset_button, LV_OBJ_FLAG_HIDDEN);
  }
}

static void release_wake_line(const char* reason) {
  lcd_wake_pin_set_mode(INT_PIN, OUTPUT);
  digitalWrite(INT_PIN, HIGH);
  delay(2);
  lcd_wake_pin_set_mode(INT_PIN, INPUT_PULLUP);
  Serial.printf("[SLEEP] release_wake_line reason=%s level=%d\n",
                reason ? reason : "unknown",
                digitalRead(INT_PIN));
  Serial.printf("[LCD_WAKE_PIN] mode=IN pullup=1 level=%d phase=release\n",
                digitalRead(INT_PIN));
}

static void status_screen_use_text(const char* text) {
  if (g_sleep_transition) {
    return;
  }
  if (!status_screen || !status_label) {
    return;
  }
  lv_obj_set_style_bg_img_src(status_screen, NULL, LV_PART_MAIN);
  lv_obj_set_style_bg_color(status_screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_label_set_text(status_label, text ? text : "");
  lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
}

static void status_screen_use_image(const lv_img_dsc_t* image) {
  if (!status_screen) {
    return;
  }
  lv_obj_set_style_bg_img_src(status_screen, image, LV_PART_MAIN);
  lv_obj_set_style_bg_color(status_screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_img_opa(status_screen, LV_OPA_COVER, LV_PART_MAIN);
  if (status_label) {
    lv_label_set_text(status_label, "");
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
  }
  set_status_reset_visible(false);
}

// ── Provisioning QR Screen ────────────────────────────────────────────
static void update_provision_status_label(const char* state) {
  if (!provision_status_label) {
    return;
  }
  const char* text = "Setup Mode";
  if (state && strcmp(state, "connecting") == 0) {
    text = "Connecting...";
  } else if (state && strcmp(state, "connected") == 0) {
    text = "Connecting...";
  } else if (state && strcmp(state, "failed") == 0) {
    text = "Failed - try again";
  } else if (state && strcmp(state, "ap_setup") == 0) {
    text = "Scan QR to join Wi-Fi";
  }
  lv_label_set_text(provision_status_label, text);
}

static void create_provision_screen_if_needed() {
  if (provision_screen != NULL) {
    return;
  }
  provision_screen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(provision_screen, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_bg_color(provision_screen, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_width(provision_screen, 0, 0);

  provision_title_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_title_label, "Halo Wi-Fi Setup");
  lv_obj_set_style_text_color(provision_title_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_title_label, LV_ALIGN_TOP_MID, 0, 16);

  provision_qr = NULL;

  provision_ssid_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_ssid_label, "SSID: -");
  lv_obj_set_style_text_color(provision_ssid_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_ssid_label, LV_ALIGN_BOTTOM_MID, 0, -64);

  provision_url_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_url_label, "Open: http://192.168.4.1");
  lv_obj_set_style_text_color(provision_url_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_url_label, LV_ALIGN_BOTTOM_MID, 0, -40);

  provision_status_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_status_label, "Setup Mode");
  lv_obj_set_style_text_color(provision_status_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_status_label, LV_ALIGN_BOTTOM_MID, 0, -16);

  lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
}

static void show_provisioning_screen(const char* ssid, const char* password, const char* url) {
  create_provision_screen_if_needed();
  if (!ssid) ssid = "";
  if (!password) password = "";
  if (!url || !url[0]) url = "http://192.168.4.1";

  // Hide other screens
  if (list_container) lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  if (menu_screen) lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  if (meal_result_screen) lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  if (logged_screen) lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
  if (expiry_screen) lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);

  std::string qr_data = QRDisplay::generateWifiQRData(ssid, password);
  if (provision_qr == NULL) {
    provision_qr = QRDisplay::createQRCodeWidget(provision_screen, qr_data, 200);
    lv_obj_align(provision_qr, LV_ALIGN_CENTER, 0, -10);
  } else {
    QRDisplay::updateQRCodeWidget(provision_qr, qr_data);
  }

  char ssid_line[64];
  snprintf(ssid_line, sizeof(ssid_line), "SSID: %s", ssid);
  lv_label_set_text(provision_ssid_label, ssid_line);
  char url_line[96];
  snprintf(url_line, sizeof(url_line), "Open: %s", url);
  lv_label_set_text(provision_url_label, url_line);
  update_provision_status_label("ap_setup");

  lv_obj_clear_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_screen_visible = true;
  lv_timer_handler();
}

static void hide_provisioning_screen() {
  if (!provision_screen) {
    return;
  }
  lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_screen_visible = false;

  // Restore list if available
  if (list_container && g_active.count > 0) {
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  lv_timer_handler();
}

// ── UI Functions ────────────────────────────────────────────────────
static void show_ship_main_menu() {
  if (!ship_menu_screen) {
    ship_menu_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(ship_menu_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_img_src(ship_menu_screen, &ui_img_Frame_443_png, LV_PART_MAIN);
    lv_obj_set_style_bg_img_opa(ship_menu_screen, LV_OPA_COVER, LV_PART_MAIN);
  }
  ship_menu_screen_state = SHIP_MENU_SCREEN_MAIN;
  ui_screen_state = SCREEN_HOME;
  lv_scr_load(ship_menu_screen);
  Serial.println("[SHIP_MENU] showing MAIN_MENU");
  lv_timer_handler();
  wifi_on_run_deferred_if_ready("main_menu");
}

static void show_ship_second_menu() {
  if (!ship_menu_second_screen) {
    ship_menu_second_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_second_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(ship_menu_second_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_img_src(ship_menu_second_screen, &ui_img_Frame_443_1_png, LV_PART_MAIN);
    lv_obj_set_style_bg_img_opa(ship_menu_second_screen, LV_OPA_COVER, LV_PART_MAIN);
  }
  ship_menu_screen_state = SHIP_MENU_SCREEN_SECOND;
  ui_screen_state = SCREEN_SECOND;
  lv_scr_load(ship_menu_second_screen);
  Serial.println("[MENU] screen=SECOND_MENU");
  lv_timer_handler();
}

static void show_ship_settings_screen() {
  if (!ship_menu_settings_screen) {
    ship_menu_settings_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_settings_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ship_menu_settings_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(ship_menu_settings_screen, LV_OBJ_FLAG_SCROLLABLE);

    ship_menu_settings_title = lv_label_create(ship_menu_settings_screen);
    lv_label_set_text(ship_menu_settings_title, "Settings");
    lv_obj_set_style_text_color(ship_menu_settings_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(ship_menu_settings_title, LV_ALIGN_TOP_MID, 0, 24);

    ship_menu_settings_btn_debug = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_debug, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_DEBUG_Y);
    lv_obj_set_size(ship_menu_settings_btn_debug, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
    lv_obj_set_style_bg_color(ship_menu_settings_btn_debug, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_btn_debug, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_menu_settings_btn_debug, 0, LV_PART_MAIN);

    ship_menu_settings_label_debug = lv_label_create(ship_menu_settings_btn_debug);
    lv_label_set_text(ship_menu_settings_label_debug, "Debug");
    lv_obj_set_style_text_color(ship_menu_settings_label_debug, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(ship_menu_settings_label_debug);

    ship_menu_settings_btn_reset = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_reset, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_RESET_Y);
    lv_obj_set_size(ship_menu_settings_btn_reset, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
    lv_obj_set_style_bg_color(ship_menu_settings_btn_reset, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_btn_reset, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_menu_settings_btn_reset, 0, LV_PART_MAIN);

    ship_menu_settings_label_reset = lv_label_create(ship_menu_settings_btn_reset);
    lv_label_set_text(ship_menu_settings_label_reset, "Reset Wi-Fi (Sense)");
    lv_obj_set_style_text_color(ship_menu_settings_label_reset, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(ship_menu_settings_label_reset);

    ship_menu_settings_btn_back = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_back, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_BACK_Y);
    lv_obj_set_size(ship_menu_settings_btn_back, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
    lv_obj_set_style_bg_color(ship_menu_settings_btn_back, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_btn_back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_menu_settings_btn_back, 0, LV_PART_MAIN);

    ship_menu_settings_label_back = lv_label_create(ship_menu_settings_btn_back);
    lv_label_set_text(ship_menu_settings_label_back, "Back");
    lv_obj_set_style_text_color(ship_menu_settings_label_back, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(ship_menu_settings_label_back);

    // Three-button settings screen (Debug + Reset Wi-Fi + Back)
  }
  ship_menu_screen_state = SHIP_MENU_SCREEN_SETTINGS;
  ui_screen_state = SCREEN_SETTINGS;
  lv_scr_load(ship_menu_settings_screen);
  Serial.println("[MENU] screen=SETTINGS");
  lv_timer_handler();
}

static void status_overlay_init() {
  if (g_status_layer) return;
  g_status_layer = lv_obj_create(lv_layer_top());
  lv_obj_set_size(g_status_layer, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(g_status_layer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(g_status_layer, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_status_layer, LV_OPA_60, LV_PART_MAIN);
  g_status_spinner = lv_spinner_create(g_status_layer, 1000, 60);
  lv_obj_set_size(g_status_spinner, 60, 60);
  lv_obj_set_style_arc_color(g_status_spinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(g_status_spinner, 4, LV_PART_INDICATOR);
  lv_obj_align(g_status_spinner, LV_ALIGN_CENTER, 0, -30);
  g_status_label = lv_label_create(g_status_layer);
  lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_width(g_status_label, LV_PCT(90));
  lv_obj_align(g_status_label, LV_ALIGN_CENTER, 0, 30);
  lv_obj_add_flag(g_status_layer, LV_OBJ_FLAG_HIDDEN);
}

static void status_overlay_show(const char* text) {
  status_overlay_init();
  lv_label_set_text(g_status_label, text ? text : "");
  lv_obj_clear_flag(g_status_layer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(g_status_layer);
}

static void status_overlay_hide() {
  if (g_status_layer) {
    lv_obj_add_flag(g_status_layer, LV_OBJ_FLAG_HIDDEN);
  }
  g_status_hide_at_ms = 0;
}

static void result_btn_home_event(lv_event_t * e);
static void result_btn_retry_event(lv_event_t * e);

static void result_screen_init() {
  if (result_root) {
    return;
  }
  result_root = lv_obj_create(NULL);
  lv_obj_set_size(result_root, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(result_root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(result_root, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(result_root, LV_OPA_COVER, LV_PART_MAIN);

  result_icon_label = lv_label_create(result_root);
  lv_obj_set_style_text_font(result_icon_label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(result_icon_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(result_icon_label, LV_ALIGN_TOP_MID, 0, 24);

  result_title_label = lv_label_create(result_root);
  lv_label_set_long_mode(result_title_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(result_title_label, LV_PCT(90));
  lv_obj_set_style_text_font(result_title_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(result_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(result_title_label, LV_ALIGN_TOP_MID, 0, 100);

  result_subtitle_label = lv_label_create(result_root);
  lv_label_set_long_mode(result_subtitle_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(result_subtitle_label, LV_PCT(90));
  lv_obj_set_style_text_font(result_subtitle_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(result_subtitle_label, lv_color_hex(0xA0A0A0), LV_PART_MAIN);
  lv_obj_align(result_subtitle_label, LV_ALIGN_TOP_MID, 0, 140);

  result_btn_home = lv_obj_create(result_root);
  lv_obj_set_size(result_btn_home, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
  lv_obj_align(result_btn_home, LV_ALIGN_BOTTOM_MID, 0, -72);
  lv_obj_set_style_bg_color(result_btn_home, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(result_btn_home, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(result_btn_home, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(result_btn_home, result_btn_home_event, LV_EVENT_CLICKED, NULL);

  result_btn_home_label = lv_label_create(result_btn_home);
  lv_label_set_text(result_btn_home_label, "Back to Home");
  lv_obj_set_style_text_color(result_btn_home_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(result_btn_home_label);

  result_btn_retry = lv_obj_create(result_root);
  lv_obj_set_size(result_btn_retry, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
  lv_obj_align(result_btn_retry, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_set_style_bg_color(result_btn_retry, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(result_btn_retry, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(result_btn_retry, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(result_btn_retry, result_btn_retry_event, LV_EVENT_CLICKED, NULL);

  result_btn_retry_label = lv_label_create(result_btn_retry);
  lv_label_set_text(result_btn_retry_label, "Retry");
  lv_obj_set_style_text_color(result_btn_retry_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(result_btn_retry_label);
}

static void ui_hide_result() {
  if (!result_root) {
    return;
  }
  lv_obj_add_flag(result_root, LV_OBJ_FLAG_HIDDEN);
}

static void ui_show_result(bool is_error, const char* title, const char* mode) {
  result_screen_init();
  const char* safe_title = (title && title[0]) ? title : (is_error ? "Something went wrong" : "Done");
  const char* safe_mode = (mode && mode[0]) ? mode : "—";
  char subtitle[48];
  snprintf(subtitle, sizeof(subtitle), "Mode: %s", safe_mode);

  lv_label_set_text(result_icon_label, is_error ? "⚠" : "✓");
  lv_label_set_text(result_title_label, safe_title);
  lv_label_set_text(result_subtitle_label, subtitle);

  if (is_error && g_last_action.valid) {
    lv_obj_clear_flag(result_btn_retry, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(result_btn_retry, LV_OBJ_FLAG_HIDDEN);
  }
  if (g_retry_disable_until_ms == 0) {
    lv_obj_clear_state(result_btn_retry, LV_STATE_DISABLED);
  }

  stop_glowing_animation();
  status_overlay_hide();
  ui_busy = false;
  ui_screen_state = SCREEN_RESULT;
  lv_obj_clear_flag(result_root, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(result_root);
  lv_timer_handler();
}

static void result_retry_tick() {
  if (!result_btn_retry) {
    return;
  }
  if (g_retry_disable_until_ms && millis() >= g_retry_disable_until_ms) {
    g_retry_disable_until_ms = 0;
    lv_obj_clear_state(result_btn_retry, LV_STATE_DISABLED);
  }
}

static void debug_screen_update();

static void debug_btn_back_event(lv_event_t * e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  Serial.println("[DEBUG] back -> settings");
  show_ship_settings_screen();
  debug_last_update_ms = 0;
}

static void debug_screen_init() {
  if (debug_screen) {
    return;
  }
  debug_screen = lv_obj_create(NULL);
  lv_obj_set_size(debug_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(debug_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(debug_screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(debug_screen, LV_OPA_COVER, LV_PART_MAIN);

  debug_title = lv_label_create(debug_screen);
  lv_label_set_text(debug_title, "Debug");
  lv_obj_set_style_text_color(debug_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(debug_title, LV_ALIGN_TOP_MID, 0, 16);

  debug_label_status = lv_label_create(debug_screen);
  lv_obj_set_style_text_color(debug_label_status, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(debug_label_status, LV_ALIGN_TOP_LEFT, 12, 52);

  debug_label_status2 = lv_label_create(debug_screen);
  lv_obj_set_style_text_color(debug_label_status2, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(debug_label_status2, LV_ALIGN_TOP_LEFT, 12, 76);

  debug_label_sense = lv_label_create(debug_screen);
  lv_obj_set_style_text_color(debug_label_sense, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(debug_label_sense, LV_ALIGN_TOP_LEFT, 12, 108);

  debug_label_hb = lv_label_create(debug_screen);
  lv_obj_set_style_text_color(debug_label_hb, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(debug_label_hb, LV_ALIGN_TOP_LEFT, 12, 132);

  debug_label_wifi = lv_label_create(debug_screen);
  lv_obj_set_style_text_color(debug_label_wifi, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(debug_label_wifi, LV_ALIGN_TOP_LEFT, 12, 156);

  debug_label_ui = lv_label_create(debug_screen);
  lv_obj_set_style_text_color(debug_label_ui, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(debug_label_ui, LV_ALIGN_TOP_LEFT, 12, 180);

  debug_btn_back = lv_obj_create(debug_screen);
  lv_obj_set_size(debug_btn_back, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
  lv_obj_align(debug_btn_back, LV_ALIGN_BOTTOM_MID, 0, -20);
  lv_obj_set_style_bg_color(debug_btn_back, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(debug_btn_back, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(debug_btn_back, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(debug_btn_back, debug_btn_back_event, LV_EVENT_CLICKED, NULL);

  debug_btn_back_label = lv_label_create(debug_btn_back);
  lv_label_set_text(debug_btn_back_label, "Back");
  lv_obj_set_style_text_color(debug_btn_back_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(debug_btn_back_label);
}

static void show_ship_debug_screen() {
  debug_screen_init();
  ui_screen_state = SCREEN_DEBUG;
  lv_scr_load(debug_screen);
  debug_last_update_ms = 0;
  debug_screen_update();
  Serial.println("[MENU] screen=DEBUG");
  lv_timer_handler();
}

static void debug_screen_update() {
  if (!debug_screen || ui_screen_state != SCREEN_DEBUG) {
    return;
  }
  unsigned long now = millis();
  if (debug_last_update_ms && (now - debug_last_update_ms) < 1000) {
    return;
  }
  debug_last_update_ms = now;

  snprintf(debug_buf_status, sizeof(debug_buf_status), "UI_STATUS op=%s phase=%s",
           g_ship_ui_op[0] ? g_ship_ui_op : "—",
           g_ship_ui_phase[0] ? g_ship_ui_phase : "—");
  snprintf(debug_buf_status2, sizeof(debug_buf_status2), "text=%.28s mode=%s",
           g_ship_ui_text[0] ? g_ship_ui_text : "—",
           g_ship_ui_mode[0] ? g_ship_ui_mode : "—");
  snprintf(debug_buf_sense, sizeof(debug_buf_sense), "sense_state=%s",
           sense_state_name(sense_state));
  unsigned long hb_age = (last_sense_rx_ms > 0) ? (now - last_sense_rx_ms) : 0;
  snprintf(debug_buf_hb, sizeof(debug_buf_hb), "link_hb_age_ms=%lu",
           (unsigned long)hb_age);

  char ssid[33] = {0};
  char pass[65] = {0};
  bool has_creds = lcd_load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));
  uint32_t checksum = has_creds ? wifi_creds_checksum(ssid, pass) : 0;
  snprintf(debug_buf_wifi, sizeof(debug_buf_wifi), "wifi_creds=%s crc=0x%08lX",
           has_creds ? "yes" : "no",
           (unsigned long)checksum);

  snprintf(debug_buf_ui, sizeof(debug_buf_ui), "ui_busy=%d",
           ui_busy ? 1 : 0);

  lv_label_set_text_static(debug_label_status, debug_buf_status);
  lv_label_set_text_static(debug_label_status2, debug_buf_status2);
  lv_label_set_text_static(debug_label_sense, debug_buf_sense);
  lv_label_set_text_static(debug_label_hb, debug_buf_hb);
  lv_label_set_text_static(debug_label_wifi, debug_buf_wifi);
  lv_label_set_text_static(debug_label_ui, debug_buf_ui);
}

static void result_btn_home_event(lv_event_t * e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  Serial.println("[RESULT] back -> home");
  ui_hide_result();
  ui_screen_state = SCREEN_HOME;
  ui_busy = false;
  status_overlay_hide();
  stop_glowing_animation();
  show_ship_main_menu();
  wifi_on_run_deferred_if_ready("result_home");
}

static void result_btn_retry_event(lv_event_t * e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  if (!g_last_action.valid) {
    Serial.println("[RESULT] retry ignored (no last action)");
    return;
  }
  unsigned long now = millis();
  if (g_retry_disable_until_ms && now < g_retry_disable_until_ms) {
    Serial.println("[RESULT] retry ignored (debounce)");
    return;
  }
  g_retry_disable_until_ms = now + 1000;
  if (result_btn_retry) {
    lv_obj_add_state(result_btn_retry, LV_STATE_DISABLED);
  }
  ui_busy = true;
  status_overlay_show("Retrying...");
  Serial.printf("[RESULT] retry menu_item=%s index=%d\n",
                g_last_action.menu_item,
                g_last_action.menu_index);
  ship_menu_send_menu_select(g_last_action.menu_item, g_last_action.menu_index, "RETRY");
}

static void ui_apply_ship_ui_status() {
  if (strcmp(g_ship_ui_op, "SCAN") != 0) {
    debug_screen_update();
    return;
  }
  const char* phase = g_ship_ui_phase;
  const char* text = g_ship_ui_text[0] ? g_ship_ui_text : (phase && phase[0]) ? phase : "";
  const char* mode = g_ship_ui_mode[0] ? g_ship_ui_mode : "—";

  if (g_ship_ui_busy) {
    ui_busy = true;
    status_overlay_show(text);
    return;
  }

  ui_busy = false;
  status_overlay_hide();

  if (g_ship_ui_terminal) {
    const char* title = (g_ship_ui_text[0]) ? g_ship_ui_text :
                        (g_ship_ui_error ? "Something went wrong" : "Done");
    ui_show_result(g_ship_ui_error, title, mode);
  } else if (strcmp(phase, "IDLE") == 0) {
    if (ui_screen_state != SCREEN_RESULT) {
      status_overlay_hide();
    }
  }

  debug_screen_update();
}

static const ship_menu_hitbox_t* ship_menu_hit_test(int x, int y) {
  const ship_menu_hitbox_t* hb = NULL;
  if (ship_menu_screen_state == SHIP_MENU_SCREEN_MAIN) {
    for (size_t i = 0; i < (sizeof(ship_menu_hitboxes_main) / sizeof(ship_menu_hitboxes_main[0])); i++) {
      hb = &ship_menu_hitboxes_main[i];
      if (x >= hb->x1 && x <= hb->x2 && y >= hb->y1 && y <= hb->y2) {
        return hb;
      }
    }
  } else if (ship_menu_screen_state == SHIP_MENU_SCREEN_SECOND) {
    for (size_t i = 0; i < (sizeof(ship_menu_hitboxes_second) / sizeof(ship_menu_hitboxes_second[0])); i++) {
      hb = &ship_menu_hitboxes_second[i];
      if (x >= hb->x1 && x <= hb->x2 && y >= hb->y1 && y <= hb->y2) {
        return hb;
      }
    }
  } else if (ship_menu_screen_state == SHIP_MENU_SCREEN_SETTINGS) {
    for (size_t i = 0; i < (sizeof(ship_menu_hitboxes_settings) / sizeof(ship_menu_hitboxes_settings[0])); i++) {
      hb = &ship_menu_hitboxes_settings[i];
      if (x >= hb->x1 && x <= hb->x2 && y >= hb->y1 && y <= hb->y2) {
        return hb;
      }
    }
  }
  return NULL;
}

static void ship_menu_show_press_overlay(const ship_menu_hitbox_t* hb) {
  if (!hb) {
    return;
  }
  if (!ship_menu_press_overlay) {
    ship_menu_press_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_style_bg_color(ship_menu_press_overlay, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_press_overlay, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_menu_press_overlay, 0, LV_PART_MAIN);
  }
  lv_obj_set_pos(ship_menu_press_overlay, hb->x1, hb->y1);
  lv_obj_set_size(ship_menu_press_overlay, (hb->x2 - hb->x1 + 1), (hb->y2 - hb->y1 + 1));
  lv_obj_clear_flag(ship_menu_press_overlay, LV_OBJ_FLAG_HIDDEN);
  ship_menu_press_hide_at_ms = millis() + SHIP_MENU_PRESS_MS;
}

static void ship_menu_record_last_action(const ship_menu_hitbox_t* hb) {
  if (!hb || !hb->menu_item || hb->menu_item[0] == '\0') {
    return;
  }
  g_last_action.valid = true;
  strncpy(g_last_action.menu_item, hb->menu_item, sizeof(g_last_action.menu_item) - 1);
  g_last_action.menu_item[sizeof(g_last_action.menu_item) - 1] = '\0';
  g_last_action.menu_index = hb->menu_index;
}

static void ship_menu_send_menu_select(const char* menu_item, int menu_index, const char* label) {
  if (!menu_item || menu_item[0] == '\0') {
    Serial.println("[MENU] menu_select missing item");
    return;
  }
  request_sense_wake("menu_select");
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_MENU_SELECT";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  doc["menu_item"] = menu_item;
  doc["menu_index"] = menu_index;
  String output;
  serializeJson(doc, output);
  senseSerial.println(output);
  if (label && label[0]) {
    Serial.printf("[MENU] tap=%s\n", label);
  } else {
    Serial.printf("[MENU] menu_item=%s index=%d\n", menu_item, menu_index);
  }
}

static void ship_menu_send_action(const ship_menu_hitbox_t* hb) {
  if (!hb) {
    return;
  }
  if (ui_busy || ui_screen_state == SCREEN_RESULT || ui_screen_state == SCREEN_DEBUG) {
    Serial.println("[UI_BUSY] tap ignored");
    return;
  }
  switch (hb->action) {
    case SHIP_MENU_ACTION_MORE:
      show_ship_second_menu();
      break;
    case SHIP_MENU_ACTION_HOME:
      Serial.println("[MENU] tap=HOME");
      show_ship_main_menu();
      break;
    case SHIP_MENU_ACTION_SETTINGS:
      Serial.println("[MENU] tap=SETTINGS");
      show_ship_settings_screen();
      break;
    case SHIP_MENU_ACTION_DEBUG:
      Serial.println("[MENU] tap=DEBUG");
      show_ship_debug_screen();
      break;
    case SHIP_MENU_ACTION_RESET_WIFI: {
      request_sense_wake("reset_wifi");
      StaticJsonDocument<128> doc;
      doc["ver"] = PROTOCOL_VERSION;
      doc["type"] = "INPUT_RESET_WIFI";
      doc["msg_id"] = get_next_msg_id();
      doc["ts"] = millis();
      String output;
      serializeJson(doc, output);
      senseSerial.println(output);
      break;
    }
    case SHIP_MENU_ACTION_BACK:
      show_ship_second_menu();
      break;
    case SHIP_MENU_ACTION_CHECK_IN:
    case SHIP_MENU_ACTION_CHECK_OUT:
    case SHIP_MENU_ACTION_LOG_DISH:
    default:
      ship_menu_record_last_action(hb);
      ship_menu_send_menu_select(hb->menu_item, hb->menu_index, hb->action_name);
      break;
  }
}

static void ship_menu_handle_ui_status(const JsonDocument& doc) {
  const char* op    = doc["op"]    | "";
  const char* phase = doc["phase"] | "";
  const char* text  = doc["text"]  | "";
  const char* mode  = doc["mode"]  | "";
  Serial.printf("[SHIP_UI_STATUS] op=%s phase=%s mode=%s text=%s\n", op, phase, mode, text);
  const char* text_raw = (text && text[0]) ? text : "";
  strncpy(g_ship_ui_op, op ? op : "", sizeof(g_ship_ui_op) - 1);
  g_ship_ui_op[sizeof(g_ship_ui_op) - 1] = '\0';
  strncpy(g_ship_ui_phase, phase ? phase : "", sizeof(g_ship_ui_phase) - 1);
  g_ship_ui_phase[sizeof(g_ship_ui_phase) - 1] = '\0';
  strncpy(g_ship_ui_text, text_raw, sizeof(g_ship_ui_text) - 1);
  g_ship_ui_text[sizeof(g_ship_ui_text) - 1] = '\0';
  strncpy(g_ship_ui_mode, mode ? mode : "", sizeof(g_ship_ui_mode) - 1);
  g_ship_ui_mode[sizeof(g_ship_ui_mode) - 1] = '\0';
  g_ship_ui_last_ms = millis();

  if (strcmp(op, "SCAN") == 0) {
    bool is_busy = (strcmp(phase, "CAPTURING") == 0 ||
                    strcmp(phase, "PREPARING") == 0 ||
                    strcmp(phase, "PROCESSING") == 0 ||
                    strcmp(phase, "WAITING") == 0);
    bool is_terminal = (strcmp(phase, "ERROR") == 0 ||
                        strcmp(phase, "DONE") == 0 ||
                        strcmp(phase, "SUCCESS") == 0 ||
                        strcmp(phase, "COMPLETE") == 0);
    g_ship_ui_busy = is_busy;
    g_ship_ui_terminal = is_terminal;
    g_ship_ui_error = (strcmp(phase, "ERROR") == 0);
  }

  if (app_event_queue != NULL) {
    app_event_t evt = {};
    evt.type = EVT_SHIP_UI_STATUS;
    if (xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20)) != pdTRUE) {
      Serial.println("[SHIP_UI_STATUS] event queue full");
    }
  }
}

static void create_custom_ui() {
  // Get default screen
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  
  // Create loading screen
  loading_screen = lv_obj_create(scr);
  lv_obj_set_size(loading_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(loading_screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(loading_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(loading_screen);
  
  lv_obj_t *loading_label = lv_label_create(loading_screen);
  lv_label_set_text(loading_label, "Loading...");
  lv_obj_set_style_text_color(loading_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(loading_label);
  
  // Create list container
  list_container = lv_obj_create(scr);
  lv_obj_set_size(list_container, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(list_container, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(list_container, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_img_src(list_container, &ui_img_Frame_439_png, LV_PART_MAIN);
  lv_obj_set_style_bg_img_opa(list_container, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(list_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(list_container, 0, LV_PART_MAIN);
  lv_obj_clear_flag(list_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  
  // Create empty label (will be created when needed)
  empty_label = NULL;
  
  // Create delete menu (hidden by default) - positioned at bottom third
  delete_menu = lv_obj_create(scr);
  lv_obj_set_size(delete_menu, 340, 120);  // Lower third height (120px), wide to fill bottom section
  lv_obj_align(delete_menu, LV_ALIGN_BOTTOM_MID, 0, 0);  // Positioned at bottom, centered
  lv_obj_set_style_bg_opa(delete_menu, LV_OPA_TRANSP, LV_PART_MAIN);  // Transparent background
  lv_obj_set_style_border_width(delete_menu, 0, LV_PART_MAIN);  // No border
  lv_obj_set_style_pad_all(delete_menu, 0, LV_PART_MAIN);  // No padding
  lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
  
  // Create "Delete" button - fills the lower third
  delete_item_btn = lv_btn_create(delete_menu);
  lv_obj_set_size(delete_item_btn, 340, 120);  // Match menu size
  lv_obj_align(delete_item_btn, LV_ALIGN_CENTER, 0, 0);  // Centered in menu
  lv_obj_set_style_bg_color(delete_item_btn, lv_color_hex(0xF0524D), LV_PART_MAIN);  // Alert Red
  lv_obj_set_style_bg_opa(delete_item_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(delete_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(delete_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(delete_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(delete_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(delete_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(delete_item_btn, lv_color_hex(0xF0524D), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(delete_item_btn, LV_OPA_COVER, LV_STATE_PRESSED);
  delete_item_label = lv_label_create(delete_item_btn);
  lv_label_set_text(delete_item_label, "Delete");
  lv_obj_set_style_text_font(delete_item_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(delete_item_label, lv_color_hex(0xF9FAFB), LV_PART_MAIN);
  lv_obj_set_style_text_opa(delete_item_label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(delete_item_label);
  // Attach delete handler
  lv_obj_add_event_cb(delete_item_btn, delete_item_btn_handler, LV_EVENT_CLICKED, NULL);
  
  // Create menu button at top third (same style as delete button but at top)
  menu_menu = lv_obj_create(scr);
  lv_obj_set_size(menu_menu, 340, 120);  // Top third height (120px), wide to fill top section
  lv_obj_align(menu_menu, LV_ALIGN_TOP_MID, 0, 0);  // Positioned at top, centered
  lv_obj_set_style_bg_opa(menu_menu, LV_OPA_TRANSP, LV_PART_MAIN);  // Transparent background
  lv_obj_set_style_border_width(menu_menu, 0, LV_PART_MAIN);  // No border
  lv_obj_set_style_pad_all(menu_menu, 0, LV_PART_MAIN);  // No padding
  lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);  // Hidden by default
  
  // Create "Menu" button - fills the top third
  menu_item_btn = lv_btn_create(menu_menu);
  lv_obj_set_size(menu_item_btn, 340, 120);  // Match menu size
  lv_obj_align(menu_item_btn, LV_ALIGN_CENTER, 0, 0);  // Centered in menu
  lv_obj_set_style_bg_color(menu_item_btn, lv_color_hex(0x245DFF), LV_PART_MAIN);  // Halo Blue
  lv_obj_set_style_bg_opa(menu_item_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(menu_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(menu_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(menu_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(menu_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(menu_item_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(menu_item_btn, lv_color_hex(0x245DFF), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(menu_item_btn, LV_OPA_COVER, LV_STATE_PRESSED);
  menu_item_label = lv_label_create(menu_item_btn);
  lv_label_set_text(menu_item_label, "Menu");
  lv_obj_set_style_text_font(menu_item_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(menu_item_label, lv_color_hex(0xF9FAFB), LV_PART_MAIN);
  lv_obj_set_style_text_opa(menu_item_label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(menu_item_label);
  // Attach menu handler
  lv_obj_add_event_cb(menu_item_btn, menu_btn_event_handler, LV_EVENT_CLICKED, NULL);
  
  // Create meal result screen (hidden by default)
  meal_result_screen = lv_obj_create(scr);
  lv_obj_set_size(meal_result_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(meal_result_screen, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(meal_result_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_img_src(meal_result_screen, &ui_img_Frame_439_png, LV_PART_MAIN);
  lv_obj_set_style_bg_img_opa(meal_result_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(meal_result_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(meal_result_screen, 20, LV_PART_MAIN);
  lv_obj_center(meal_result_screen);
  lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(meal_result_screen, LV_OBJ_FLAG_SCROLLABLE);
  
  // Calories label - large text at top
  meal_calories_label = lv_label_create(meal_result_screen);
  lv_label_set_text(meal_calories_label, "0cal");
  lv_obj_set_style_text_font(meal_calories_label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(meal_calories_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(meal_calories_label, LV_ALIGN_TOP_MID, 0, 20);
  
  // Meal description label - centered, medium text
  meal_description_label = lv_label_create(meal_result_screen);
  lv_label_set_text(meal_description_label, "");
  lv_obj_set_style_text_font(meal_description_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(meal_description_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(meal_description_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(meal_description_label, LV_LABEL_LONG_WRAP);  // Enable text wrapping
  lv_obj_set_width(meal_description_label, 280);  // Reduced from 300 to fit circular screen better
  lv_obj_set_style_pad_hor(meal_description_label, 10, LV_PART_MAIN);  // Add horizontal padding
  lv_obj_align(meal_description_label, LV_ALIGN_CENTER, 0, -40);
  
  // Macros container - horizontal layout with stacked labels
  lv_obj_t *macros_container = lv_obj_create(meal_result_screen);
  lv_obj_set_size(macros_container, 300, 80);  // Increased height for larger font and spacing
  lv_obj_set_style_bg_opa(macros_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(macros_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(macros_container, 0, LV_PART_MAIN);
  lv_obj_align(macros_container, LV_ALIGN_CENTER, 0, 50);  // Moved down (was 20, now 50)
  
  // Protein - value label (top) - doubled font size
  meal_protein_value_label = lv_label_create(macros_container);
  lv_label_set_text(meal_protein_value_label, "0g");
  lv_obj_set_style_text_font(meal_protein_value_label, &lv_font_montserrat_40, LV_PART_MAIN);  // Doubled font size (20 -> 40)
  lv_obj_set_style_text_color(meal_protein_value_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_opa(meal_protein_value_label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(meal_protein_value_label, LV_ALIGN_LEFT_MID, 0, -20);  // More spacing above center (was -10)
  
  // Protein - name label (bottom)
  meal_protein_name_label = lv_label_create(macros_container);
  lv_label_set_text(meal_protein_name_label, "Protein");
  lv_obj_set_style_text_font(meal_protein_name_label, &lv_font_montserrat_14, LV_PART_MAIN);  // Smaller font for label
  lv_obj_set_style_text_color(meal_protein_name_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(meal_protein_name_label, LV_ALIGN_LEFT_MID, 0, 20);  // More spacing below center (was 10)
  
  // Carbs - value label (top) - doubled font size
  meal_carbs_value_label = lv_label_create(macros_container);
  lv_label_set_text(meal_carbs_value_label, "0g");
  lv_obj_set_style_text_font(meal_carbs_value_label, &lv_font_montserrat_40, LV_PART_MAIN);  // Doubled font size (20 -> 40)
  lv_obj_set_style_text_color(meal_carbs_value_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_opa(meal_carbs_value_label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(meal_carbs_value_label, LV_ALIGN_CENTER, 0, -20);  // More spacing above center (was -10)
  
  // Carbs - name label (bottom)
  meal_carbs_name_label = lv_label_create(macros_container);
  lv_label_set_text(meal_carbs_name_label, "Carbs");
  lv_obj_set_style_text_font(meal_carbs_name_label, &lv_font_montserrat_14, LV_PART_MAIN);  // Smaller font for label
  lv_obj_set_style_text_color(meal_carbs_name_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(meal_carbs_name_label, LV_ALIGN_CENTER, 0, 20);  // More spacing below center (was 10)
  
  // Fat - value label (top) - doubled font size
  meal_fat_value_label = lv_label_create(macros_container);
  lv_label_set_text(meal_fat_value_label, "0g");
  lv_obj_set_style_text_font(meal_fat_value_label, &lv_font_montserrat_40, LV_PART_MAIN);  // Doubled font size (20 -> 40)
  lv_obj_set_style_text_color(meal_fat_value_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_opa(meal_fat_value_label, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(meal_fat_value_label, LV_ALIGN_RIGHT_MID, 0, -20);  // More spacing above center (was -10)
  
  // Fat - name label (bottom)
  meal_fat_name_label = lv_label_create(macros_container);
  lv_label_set_text(meal_fat_name_label, "Fat");
  lv_obj_set_style_text_font(meal_fat_name_label, &lv_font_montserrat_14, LV_PART_MAIN);  // Smaller font for label
  lv_obj_set_style_text_color(meal_fat_name_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(meal_fat_name_label, LV_ALIGN_RIGHT_MID, 0, 20);  // More spacing below center (was 10)
  
  // Recommendation label - small text at bottom
  // For circular screen (360x360), use narrower width to ensure text fits within circle bounds
  meal_recommendation_label = lv_label_create(meal_result_screen);
  lv_label_set_text(meal_recommendation_label, "");
  lv_obj_set_style_text_font(meal_recommendation_label, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(meal_recommendation_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(meal_recommendation_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(meal_recommendation_label, LV_LABEL_LONG_WRAP);  // Enable text wrapping
  lv_obj_set_width(meal_recommendation_label, 280);  // Reduced from 300 to fit circular screen better
  lv_obj_set_style_pad_hor(meal_recommendation_label, 10, LV_PART_MAIN);  // Add horizontal padding
  lv_obj_set_style_pad_ver(meal_recommendation_label, 5, LV_PART_MAIN);  // Add vertical padding
  lv_obj_align(meal_recommendation_label, LV_ALIGN_BOTTOM_MID, 0, -15);  // Slightly higher to give more room
  
  // Create recording indicator (solid halo/ring for long press)
  // Screen is 360x360px, create a circular border
  recording_indicator = lv_obj_create(scr);
  lv_obj_set_size(recording_indicator, 360, 360);  // Full screen size (360x360)
  lv_obj_align(recording_indicator, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(recording_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(recording_indicator, 12, LV_PART_MAIN);  // 12px thick border
  lv_obj_set_style_border_color(recording_indicator, lv_color_hex(0x245DFF), LV_PART_MAIN);  // Halo Blue
  lv_obj_set_style_radius(recording_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);  // Perfect circle
  lv_obj_set_style_border_opa(recording_indicator, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(recording_indicator, LV_OBJ_FLAG_HIDDEN);  // Hidden by default
  lv_obj_clear_flag(recording_indicator, LV_OBJ_FLAG_CLICKABLE);  // Not clickable - just visual
  
  // Create processing indicator (glowing/pulsing halo)
  // This will be shown during list refresh and voice processing
  processing_indicator = lv_obj_create(scr);
  lv_obj_set_size(processing_indicator, 360, 360);  // Full screen size (360x360)
  lv_obj_align(processing_indicator, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(processing_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(processing_indicator, 12, LV_PART_MAIN);  // 12px thick border
  lv_obj_set_style_border_color(processing_indicator, lv_color_hex(0x245DFF), LV_PART_MAIN);  // Halo Blue
  lv_obj_set_style_radius(processing_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);  // Perfect circle
  lv_obj_set_style_border_opa(processing_indicator, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_flag(processing_indicator, LV_OBJ_FLAG_HIDDEN);  // Hidden by default
  lv_obj_clear_flag(processing_indicator, LV_OBJ_FLAG_CLICKABLE);  // Not clickable - just visual
  
  // Create status screen (for "On it!", "Hold still!", etc.)
  status_screen = lv_obj_create(scr);
  lv_obj_set_size(status_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(status_screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White background
  lv_obj_set_style_bg_opa(status_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(status_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(status_screen, 0, LV_PART_MAIN);
  lv_obj_center(status_screen);
  lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_SCROLLABLE);
  
  // Create status label - large font, centered
  status_label = lv_label_create(status_screen);
  lv_label_set_text(status_label, "");
  lv_obj_set_style_text_font(status_label, &lv_font_montserrat_32, LV_PART_MAIN);  // Large font
  lv_obj_set_style_text_color(status_label, lv_color_hex(0x001A4D), LV_PART_MAIN);  // Dark blue text (#001A4D)
  lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);  // Enable text wrapping for multi-line
  lv_obj_set_width(status_label, 300);  // Width for circular screen
  lv_obj_center(status_label);

  // Reset Wi-Fi button (hidden by default)
  status_reset_button = lv_btn_create(status_screen);
  lv_obj_set_size(status_reset_button, 200, 50);
  lv_obj_set_style_bg_color(status_reset_button, lv_color_hex(0x2563EB), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_reset_button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(status_reset_button, 10, LV_PART_MAIN);
  lv_obj_align(status_reset_button, LV_ALIGN_BOTTOM_MID, 0, -30);
  lv_obj_add_flag(status_reset_button, LV_OBJ_FLAG_HIDDEN);
  status_reset_label = lv_label_create(status_reset_button);
  lv_label_set_text(status_reset_label, "Reset Wi-Fi");
  lv_obj_set_style_text_color(status_reset_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(status_reset_label);
  
  // Create logged screen (for Discard mode - shown after image capture)
  logged_screen = lv_obj_create(scr);
  lv_obj_set_size(logged_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(logged_screen, lv_color_hex(0x1D4509), LV_PART_MAIN);  // Green background (#1D4509)
  lv_obj_set_style_bg_opa(logged_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_img_src(logged_screen, &ui_img_Frame_440_png, LV_PART_MAIN);
  lv_obj_set_style_bg_img_opa(logged_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(logged_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(logged_screen, 0, LV_PART_MAIN);
  lv_obj_center(logged_screen);
  lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(logged_screen, LV_OBJ_FLAG_SCROLLABLE);
  
  // Create logged label - white text, large font, centered
  logged_label = lv_label_create(logged_screen);
  lv_label_set_text(logged_label, "");
  lv_obj_set_style_text_font(logged_label, &lv_font_montserrat_32, LV_PART_MAIN);  // Large font
  lv_obj_set_style_text_color(logged_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White text
  lv_obj_set_style_text_align(logged_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(logged_label);
  lv_obj_add_flag(logged_label, LV_OBJ_FLAG_HIDDEN);
  
  // Create menu screen (blue background with menu items)
  menu_screen = lv_obj_create(scr);
  lv_obj_set_size(menu_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(menu_screen, lv_color_hex(0x0056A5), LV_PART_MAIN);  // Blue background (#0056A5)
  lv_obj_set_style_bg_opa(menu_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(menu_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(menu_screen, 0, LV_PART_MAIN);
  lv_obj_center(menu_screen);
  lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(menu_screen, LV_OBJ_FLAG_SCROLLABLE);
  
  // Create container for menu items (vertical list, no border/background)
  menu_list_container = lv_obj_create(menu_screen);
  lv_obj_set_size(menu_list_container, 320, 320);  // Sized for circular screen (360x360)
  lv_obj_align(menu_list_container, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(menu_list_container, LV_OPA_TRANSP, LV_PART_MAIN);  // Transparent - no box
  lv_obj_set_style_border_width(menu_list_container, 0, LV_PART_MAIN);  // No border
  lv_obj_set_style_pad_all(menu_list_container, 0, LV_PART_MAIN);  // No padding
  lv_obj_set_flex_flow(menu_list_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(menu_list_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);  // Center items
  
  // Create labels for each menu item (max slots)
  for (int i = 0; i < MENU_MAX_ITEMS; i++) {
    menu_item_labels[i] = lv_label_create(menu_list_container);
    lv_label_set_text(menu_item_labels[i], "");
    lv_obj_set_style_text_font(menu_item_labels[i], &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(menu_item_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White text
    lv_obj_set_style_text_opa(menu_item_labels[i], LV_OPA_70, LV_PART_MAIN);  // Default to slightly transparent
    lv_obj_set_style_text_align(menu_item_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);  // Center align text
    lv_obj_set_style_pad_ver(menu_item_labels[i], 12, LV_PART_MAIN);  // Vertical spacing between items
    lv_obj_set_style_bg_opa(menu_item_labels[i], LV_OPA_TRANSP, LV_PART_MAIN);  // No background by default
    lv_obj_set_width(menu_item_labels[i], LV_PCT(100));
  }
  
  // Initialize menu mode and selection
  set_menu_mode(MENU_MODE_MAIN);
  menu_selected_index = 0;
  update_menu_display();
  
  // Create expiration date entry screen (for Check-in mode)
  expiry_screen = lv_obj_create(scr);
  lv_obj_set_size(expiry_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(expiry_screen, lv_color_hex(0x0056A5), LV_PART_MAIN);  // Blue background (same as menu)
  lv_obj_set_style_bg_opa(expiry_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(expiry_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(expiry_screen, 0, LV_PART_MAIN);
  lv_obj_center(expiry_screen);
  lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(expiry_screen, LV_OBJ_FLAG_SCROLLABLE);
  
  // Create date display label at top (shows current input like "12-31-2026")
  expiry_date_label = lv_label_create(expiry_screen);
  lv_label_set_text(expiry_date_label, "MM-DD-YYYY");
  lv_obj_set_style_text_font(expiry_date_label, &lv_font_montserrat_28, LV_PART_MAIN);  // Large font
  lv_obj_set_style_text_color(expiry_date_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White text
  lv_obj_set_style_text_align(expiry_date_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(expiry_date_label, LV_ALIGN_TOP_MID, 0, 30);  // Top center, 30px from top
  
  // Create keypad container (for all buttons: 1-9, backspace, 0, submit)
  lv_obj_t *keypad_container = lv_obj_create(expiry_screen);
  lv_obj_set_size(keypad_container, 340, 300);  // Larger size to extend to bottom
  lv_obj_align(keypad_container, LV_ALIGN_BOTTOM_MID, 0, -10);  // Align to bottom, 10px from edge
  lv_obj_set_style_bg_opa(keypad_container, LV_OPA_TRANSP, LV_PART_MAIN);  // Transparent
  lv_obj_set_style_border_width(keypad_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(keypad_container, 5, LV_PART_MAIN);
  lv_obj_set_flex_flow(keypad_container, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(keypad_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  
  // Create number buttons 1-9 in a 3x3 grid
  // Layout: 1 2 3
  //         4 5 6
  //         7 8 9
  const char* keypad_labels[9] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
  int digit_values[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  for (int i = 0; i < 9; i++) {
    expiry_keypad_buttons[i] = lv_btn_create(keypad_container);
    lv_obj_set_size(expiry_keypad_buttons[i], 80, 55);  // Button size
    lv_obj_set_style_bg_color(expiry_keypad_buttons[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White buttons
    lv_obj_set_style_bg_opa(expiry_keypad_buttons[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(expiry_keypad_buttons[i], 8, LV_PART_MAIN);  // Rounded corners
    
    lv_obj_t *btn_label = lv_label_create(expiry_keypad_buttons[i]);
    lv_label_set_text(btn_label, keypad_labels[i]);
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0x0056A5), LV_PART_MAIN);  // Blue text
    lv_obj_center(btn_label);
    
    // Store digit value in user data (for touch detection)
    lv_obj_set_user_data(expiry_keypad_buttons[i], (void*)(intptr_t)digit_values[i]);
  }
  
  // Create bottom row: backspace (left), 0 (center), submit (right) - all in same container
  // Backspace button
  expiry_backspace_button = lv_btn_create(keypad_container);
  lv_obj_set_size(expiry_backspace_button, 80, 50);
  lv_obj_set_style_bg_color(expiry_backspace_button, lv_color_hex(0xFF6B6B), LV_PART_MAIN);  // Red background
  lv_obj_set_style_bg_opa(expiry_backspace_button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(expiry_backspace_button, 8, LV_PART_MAIN);
  
  lv_obj_t *backspace_label = lv_label_create(expiry_backspace_button);
  lv_label_set_text(backspace_label, "DEL");  // Backspace text (ASCII)
  lv_obj_set_style_text_font(backspace_label, &lv_font_montserrat_20, LV_PART_MAIN);  // Slightly smaller font to fit
  lv_obj_set_style_text_color(backspace_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White text
  lv_obj_center(backspace_label);
  
  // Zero button
  expiry_keypad_buttons[9] = lv_btn_create(keypad_container);
  lv_obj_set_size(expiry_keypad_buttons[9], 80, 50);
  lv_obj_set_style_bg_color(expiry_keypad_buttons[9], lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White button
  lv_obj_set_style_bg_opa(expiry_keypad_buttons[9], LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(expiry_keypad_buttons[9], 8, LV_PART_MAIN);
  
  lv_obj_t *zero_label = lv_label_create(expiry_keypad_buttons[9]);
  lv_label_set_text(zero_label, "0");
  lv_obj_set_style_text_font(zero_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(zero_label, lv_color_hex(0x0056A5), LV_PART_MAIN);  // Blue text
  lv_obj_center(zero_label);
  lv_obj_set_user_data(expiry_keypad_buttons[9], (void*)(intptr_t)0);  // Store digit 0
  
  // Submit/Check button
  expiry_check_button = lv_btn_create(keypad_container);
  lv_obj_set_size(expiry_check_button, 80, 50);
  lv_obj_set_style_bg_color(expiry_check_button, lv_color_hex(0x1D4509), LV_PART_MAIN);  // Green background
  lv_obj_set_style_bg_opa(expiry_check_button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(expiry_check_button, 8, LV_PART_MAIN);
  
  lv_obj_t *check_label = lv_label_create(expiry_check_button);
  lv_label_set_text(check_label, "OK");  // Check text (ASCII)
  lv_obj_set_style_text_font(check_label, &lv_font_montserrat_20, LV_PART_MAIN);  // Slightly smaller font to fit
  lv_obj_set_style_text_color(check_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);  // White text
  lv_obj_center(check_label);
}

// ── Menu Display Functions ─────────────────────────────────────────
static void set_menu_mode(menu_mode_t mode) {
  menu_mode = mode;
  if (menu_mode == MENU_MODE_SETTINGS) {
    menu_items_current = menu_items_settings;
    menu_item_count = MENU_SETTINGS_ITEM_COUNT;
  } else {
    menu_items_current = menu_items_main;
    menu_item_count = MENU_MAIN_ITEM_COUNT;
  }
  
  for (int i = 0; i < MENU_MAX_ITEMS; i++) {
    if (menu_item_labels[i] != NULL) {
      if (i < menu_item_count) {
        lv_label_set_text(menu_item_labels[i], menu_items_current[i]);
        lv_obj_clear_flag(menu_item_labels[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_label_set_text(menu_item_labels[i], "");
        lv_obj_add_flag(menu_item_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

static void update_menu_display() {
  if (menu_list_container == NULL) return;
  
  // Safety check: ensure selected index is valid
  if (menu_selected_index < 0 || menu_selected_index >= menu_item_count) {
    Serial.printf("[MENU] Invalid selected_index: %d (max: %d) - clamping to 0\n", menu_selected_index, menu_item_count - 1);
    menu_selected_index = 0;
  }
  
  // Update all menu item labels with highlighting for selected item
  for (int i = 0; i < MENU_MAX_ITEMS; i++) {
    if (menu_item_labels[i] != NULL) {
      if (i >= menu_item_count) {
        continue;
      }
      if (i == menu_selected_index) {
        // Selected item - brighter white or bold
        lv_obj_set_style_text_color(menu_item_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_opa(menu_item_labels[i], LV_OPA_COVER, LV_PART_MAIN);
        // Could add background highlight or make font larger/bolder
        // For now, we'll add a subtle background highlight
        lv_obj_set_style_bg_color(menu_item_labels[i], lv_color_hex(0x003D7A), LV_PART_MAIN);  // Darker blue highlight
        lv_obj_set_style_bg_opa(menu_item_labels[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(menu_item_labels[i], 8, LV_PART_MAIN);  // Rounded corners
        lv_obj_set_style_pad_all(menu_item_labels[i], 8, LV_PART_MAIN);  // Padding for highlight
      } else {
        // Unselected item - white text, no background
        lv_obj_set_style_text_color(menu_item_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_opa(menu_item_labels[i], LV_OPA_70, LV_PART_MAIN);  // Slightly transparent
        lv_obj_set_style_bg_opa(menu_item_labels[i], LV_OPA_TRANSP, LV_PART_MAIN);  // No background
        lv_obj_set_style_pad_all(menu_item_labels[i], 0, LV_PART_MAIN);  // No padding
      }
    }
  }
}

static void show_menu_screen() {
  Serial.println("[MENU] Showing menu screen");
  
  // Safety check: ensure menu UI is initialized
  if (menu_screen == NULL || menu_list_container == NULL) {
    Serial.println("[MENU] ERROR: Menu UI not initialized! Cannot show menu.");
    return;
  }
  
  menu_screen_visible = true;
  set_menu_mode(MENU_MODE_MAIN);
  menu_selected_index = 0;  // Reset to first item
  
  // Clamp selected index to valid range
  if (menu_selected_index < 0 || menu_selected_index >= menu_item_count) {
    menu_selected_index = 0;
  }
  
  update_menu_display();
  
  // Set cooldown to prevent the touch that opened the menu from also selecting a menu item
  menu_cooldown_until = millis() + 300;  // 300ms cooldown
  
  // Hide all other screens
  if (list_container != NULL) {
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  if (meal_result_screen != NULL) {
    lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  }
  if (status_screen != NULL) {
    lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  }
  
  // Show menu screen
  lv_obj_clear_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  
  lv_timer_handler();  // Force immediate render
}

static void hide_menu_screen() {
  Serial.println("[MENU] Hiding menu screen");
  menu_screen_visible = false;
  menu_cooldown_until = 0;  // Reset cooldown when menu is hidden
  
  if (menu_screen != NULL) {
    lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  }
  
  // Show list again
  if (list_container != NULL && g_active.count > 0) {
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  
  lv_timer_handler();  // Force immediate render
}

// ── Menu Button Handler ────────────────────────────────────────────
static void menu_btn_event_handler(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    Serial.println("[MENU] Menu button clicked - showing menu screen");
    
    // Hide buttons immediately after click
    buttons_visible = false;
    if (delete_menu != NULL) {
      lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
    }
    if (menu_menu != NULL) {
      lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Show menu screen
    show_menu_screen();
    
    // Set cooldown to prevent touch toggle from re-showing buttons immediately
    delete_cooldown_until = millis() + 500;  // 500ms cooldown
    
    // Reset activity timer (user is interacting)
    resetActivityTimer();
  }
}

static void ui_update_list(const app_state_t *s) {
  if (list_container == NULL) {
    Serial.println("[UI] ERROR: list_container is NULL!");
    return;
  }
  
  // Ensure list container is visible
  lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  
  const int NUM_ROWS = 7;  // Number of visible rows
  const int CENTER_ROW = NUM_ROWS / 2;  // Middle row (index 3)
  const int ROW_HEIGHT = 42;  // Fixed height per row
  const int START_Y = -(ROW_HEIGHT * (NUM_ROWS - 1)) / 2;  // Center block vertically
  
  // Ensure we have enough labels (use first NUM_ROWS of list_labels array)
  static bool labels_created = false;
  if (!labels_created) {
    Serial.println("[UI] Creating list labels...");
    for (int row = 0; row < NUM_ROWS && row < MAX_LIST_ITEMS; row++) {
      if (list_labels[row] == NULL) {
        list_labels[row] = lv_label_create(list_container);
        lv_obj_set_width(list_labels[row], 280);  // Fixed width
        lv_label_set_long_mode(list_labels[row], LV_LABEL_LONG_WRAP);  // Let LVGL handle wrapping
        lv_obj_set_style_text_align(list_labels[row], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        int y = START_Y + row * ROW_HEIGHT;
        lv_obj_align(list_labels[row], LV_ALIGN_CENTER, 0, y);
        // Hide labels initially - they'll be shown when we have data
        lv_obj_add_flag(list_labels[row], LV_OBJ_FLAG_HIDDEN);
      }
    }
    labels_created = true;
    Serial.println("[UI] List labels created");
  }
  
  // Hide empty label if items exist
  if (s->count > 0 && empty_label != NULL) {
    lv_obj_del(empty_label);
    empty_label = NULL;
  }
  
  // Show empty message if no items
  if (s->count == 0) {
    for (int row = 0; row < NUM_ROWS; row++) {
      if (list_labels[row] != NULL) {
        lv_obj_add_flag(list_labels[row], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (empty_label == NULL) {
      empty_label = lv_label_create(list_container);
      lv_label_set_text(empty_label, "Shopping list is empty");
      lv_obj_set_style_text_color(empty_label, lv_color_hex(0x6B7280), LV_PART_MAIN);  // Text Muted Dark
      lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
    }
    return;
  }
  
  // Update each row - simple mapping: logical_index = selected_index + (row - CENTER_ROW)
  // When selected_index = 0, item 0 appears in center row (row 3), but we highlight it
  int visible_count = 0;
  for (int row = 0; row < NUM_ROWS; row++) {
    int logical_index = s->selected_index + (row - CENTER_ROW);
    
    if (logical_index < 0 || logical_index >= s->count) {
      // Out of bounds - hide this row
      if (list_labels[row] != NULL) {
        lv_obj_add_flag(list_labels[row], LV_OBJ_FLAG_HIDDEN);
      }
    } else {
      // Show this row with item
      if (list_labels[row] != NULL) {
        lv_obj_clear_flag(list_labels[row], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(list_labels[row], s->items[logical_index]);
        
        // Reset font styles
        lv_obj_set_style_text_letter_space(list_labels[row], 0, LV_PART_MAIN);
        
        // Highlight the selected item
        bool is_selected = (logical_index == s->selected_index);
        
        if (is_selected) {
          lv_obj_set_style_text_font(list_labels[row], &lv_font_montserrat_20, LV_PART_MAIN);
          lv_obj_set_style_text_color(list_labels[row], lv_color_hex(0xF9FAFB), LV_PART_MAIN);  // Text Primary Dark
        } else {
          lv_obj_set_style_text_font(list_labels[row], &lv_font_montserrat_16, LV_PART_MAIN);
          lv_obj_set_style_text_color(list_labels[row], lv_color_hex(0x9CA3AF), LV_PART_MAIN);  // Text Secondary Dark
        }
        visible_count++;
      }
    }
  }
  
  Serial.printf("[UI] Updated list display: count=%d, selected=%d, visible_rows=%d\n", 
                s->count, s->selected_index, visible_count);
}

// CRITICAL: This function must ONLY be called from UI task context!
// It does NOT take the mutex - caller must ensure mutex is held if needed
static void ui_refresh_from_state(const app_state_t *s) {
  if (g_sleep_transition) {
    return;
  }
  if (s == NULL) {
    Serial.println("[UI] ERROR: ui_refresh_from_state called with NULL pointer!");
    return;
  }
  
  // Safety check: validate count
  if (s->count < 0 || s->count > MAX_LIST_ITEMS) {
    Serial.printf("[UI] ERROR: Invalid count in ui_refresh_from_state: %d\n", s->count);
    return;
  }
  // Update list display
  ui_update_list(s);
  
  // Ensure list container is visible when we have items
  if (s->count > 0 && list_container != NULL) {
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  
  // Ensure loading screen is hidden when we have items
  if (s->count > 0 && loading_screen != NULL) {
    lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
  }
}

// ── Delete Button Handler ────────────────────────────────────────────
// CRITICAL: This runs from LVGL event context - must send event to UI task for state updates
static void delete_item_btn_handler(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    // Get selected item ID from g_active
    char item_id_to_delete[64] = {0};
    int local_selected = -1;
    int local_count = 0;
    
    if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      local_selected = g_active.selected_index;
      local_count = g_active.count;
      
      if (local_selected >= 0 && local_selected < local_count && local_selected < MAX_LIST_ITEMS) {
        // Copy the item ID (don't use pointer - it might become invalid)
        strncpy(item_id_to_delete, g_active.item_ids[local_selected], sizeof(item_id_to_delete) - 1);
        item_id_to_delete[sizeof(item_id_to_delete) - 1] = '\0';
        
        // Add to deleted items tracking (if ID exists and not already tracked)
        if (strlen(item_id_to_delete) > 0 && deleted_item_count < MAX_DELETED_ITEMS) {
          bool already_tracked = false;
          for (int i = 0; i < deleted_item_count; i++) {
            if (strcmp(item_id_to_delete, deleted_item_ids[i]) == 0) {
              already_tracked = true;
              break;
            }
          }
          if (!already_tracked) {
            strncpy(deleted_item_ids[deleted_item_count], item_id_to_delete, 63);
            deleted_item_ids[deleted_item_count][63] = '\0';
            deleted_item_count++;
            Serial.printf("[DELETE] Added ID to deleted tracking: %s (total tracked: %d)\n", 
                          item_id_to_delete, deleted_item_count);
          }
        }
        
        Serial.printf("[DELETE] Delete button pressed - item at index %d, ID: %s\n", 
                      local_selected, item_id_to_delete);
      } else {
        Serial.printf("[DELETE] Invalid selected index: %d (count: %d)\n", local_selected, local_count);
        xSemaphoreGive(app_state_mutex);
        return;
      }
      xSemaphoreGive(app_state_mutex);
    } else {
      Serial.println("[DELETE] Failed to acquire mutex");
      return;
    }
    
    // Optimistically remove item from g_active immediately
    if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (local_selected < g_active.count) {
        // Shift remaining items up
        for (int i = local_selected; i < g_active.count - 1; i++) {
          strncpy(g_active.items[i], g_active.items[i + 1], 64);
          g_active.items[i][63] = '\0';
          strncpy(g_active.item_ids[i], g_active.item_ids[i + 1], 64);
          g_active.item_ids[i][63] = '\0';
        }
        // Clear last item
        g_active.items[g_active.count - 1][0] = '\0';
        g_active.item_ids[g_active.count - 1][0] = '\0';
        g_active.count--;
        
        // Adjust selected index if needed
        if (g_active.selected_index >= g_active.count && g_active.count > 0) {
          g_active.selected_index = g_active.count - 1;
        } else if (g_active.count == 0) {
          g_active.selected_index = -1;
        }
        
        // CRITICAL: Update UI IMMEDIATELY for instant visual feedback
        // This is safe because we're in an LVGL event callback context
        ui_refresh_from_state(&g_active);
        
        // Hide buttons IMMEDIATELY after deletion (before any other processing)
        buttons_visible = false;
        if (delete_menu != NULL) {
          lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
        }
        if (menu_menu != NULL) {
          lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Set cooldown to prevent touch toggle from re-showing buttons immediately
        delete_cooldown_until = millis() + 500;  // 500ms cooldown
        
        // Force LVGL to render immediately (including button hiding)
        lv_timer_handler();
        
        // Save updated list to storage (non-blocking, happens after UI update)
        save_list_to_storage(&g_active);
      }
      xSemaphoreGive(app_state_mutex);
      
      // Also send event to UI task (backup, but UI is already updated above)
      if (app_event_queue != NULL) {
        app_event_t evt = {EVT_LIST_REPLACED, {.new_count = g_active.count}};
        xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(10));
      }
    }
    
    // Send delete request to Sense board
    tx_msg_t tx_msg = {};
    strncpy(tx_msg.type, "INPUT_DELETE", sizeof(tx_msg.type) - 1);
    strncpy(tx_msg.id, item_id_to_delete, sizeof(tx_msg.id) - 1);
    tx_msg.has_id = true;
    if (uart_tx_queue != NULL) {
      xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
      Serial.printf("[DELETE] Sent delete request to Sense board for ID: %s\n", item_id_to_delete);
    }
    
    Serial.println("[DELETE] Item removed from local state, buttons hidden, UI refreshed");
  }
}

// ── Apply Event to State (Pure Data, No LVGL) ──────────────────────
// Returns true if pull-to-refresh was triggered
static bool apply_event_to_state(app_state_t *s, const app_event_t *evt) {
  if (s == NULL || evt == NULL) return false;
  
  if (evt->type == EVT_SCROLL_DELTA) {
    // Mark that user has scrolled
    user_has_scrolled = true;
    
    if (s->count > 0) {
      int old_index = s->selected_index;
      s->selected_index += evt->data.scroll_delta;
      
      // Clamp to valid range
      if (s->selected_index < 0) {
        s->selected_index = 0;
      }
      if (s->selected_index >= s->count) {
        s->selected_index = s->count - 1;
      }
      
      // Pull-to-refresh: track consecutive negative scrolls when at index 0
      if (old_index == 0 && s->selected_index == 0 && evt->data.scroll_delta < 0) {
        if (lcd_refresh_inflight || waiting_for_list_response) {
          // Refresh already in progress; ignore additional pull attempts.
          pull_to_refresh_counter = 0;
          return false;
        }
        // User is at first item and scrolling counter-clockwise (negative)
        pull_to_refresh_counter++;
        Serial.printf("[PULL-TO-REFRESH] Counter: %d/%d\n", pull_to_refresh_counter, PULL_TO_REFRESH_THRESHOLD);
        
        if (pull_to_refresh_counter >= PULL_TO_REFRESH_THRESHOLD) {
          // Trigger refresh!
          pull_to_refresh_counter = 0;  // Reset counter
          Serial.println("[PULL-TO-REFRESH] Threshold reached - triggering refresh!");
          return true;  // Signal that refresh should be triggered
        }
      } else {
        // User scrolled away from index 0 or scrolled positive - reset counter
        if (pull_to_refresh_counter > 0) {
          pull_to_refresh_counter = 0;
          Serial.println("[PULL-TO-REFRESH] Counter reset (user scrolled away)");
        }
      }
    } else {
      // Empty list: allow pull-to-refresh with negative scrolls
      if (evt->data.scroll_delta < 0) {
        if (lcd_refresh_inflight || waiting_for_list_response) {
          pull_to_refresh_counter = 0;
          return false;
        }
        pull_to_refresh_counter++;
        Serial.printf("[PULL-TO-REFRESH] (empty) Counter: %d/%d\n",
                      pull_to_refresh_counter, PULL_TO_REFRESH_THRESHOLD);
        if (pull_to_refresh_counter >= PULL_TO_REFRESH_THRESHOLD) {
          pull_to_refresh_counter = 0;
          Serial.println("[PULL-TO-REFRESH] (empty) Threshold reached - triggering refresh!");
          return true;
        }
      } else if (pull_to_refresh_counter > 0) {
        pull_to_refresh_counter = 0;
        Serial.println("[PULL-TO-REFRESH] (empty) Counter reset");
      }
    }
  }
  
  return false;  // No refresh triggered
}

// ── Scroll Handling ────────────────────────────────────────────────
// CRITICAL: These callbacks run in interrupt context - NO LVGL, NO UART TX!
// Only post events to queues, tasks will handle LVGL and UART
static void knob_left_cb(void *arg, void *data) {
  if (g_ship_ota_wake_window && !g_ui_initialized) {
    g_ship_ota_user_input = true;
    return;
  }
  // Ignore scrolls during brief wake-up period (150ms)
  if (millis() < scroll_ignore_until) {
    return;
  }
  user_activity_since_sleep = true;
  
  // Post to UI event queue for immediate UI feedback (optimistic update)
  app_event_t evt = {EVT_SCROLL_DELTA, {.scroll_delta = -1}};
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (app_event_queue != NULL) {
    xQueueSendFromISR(app_event_queue, &evt, &xHigherPriorityTaskWoken);
  }

  // Haptic tick (queued to UI task)
  app_event_t h_evt = {};
  h_evt.type = EVT_HAPTIC_TICK;
  if (app_event_queue != NULL) {
    xQueueSendFromISR(app_event_queue, &h_evt, &xHigherPriorityTaskWoken);
  }
  
  // Queue UART TX message (NOT sent here - uart_task will send it)
  if (!refresh_request_pending && !ui_busy) {
    tx_msg_t tx_msg = {};
    strncpy(tx_msg.type, "INPUT_SCROLL", sizeof(tx_msg.type) - 1);
    tx_msg.delta = -1;
    tx_msg.has_delta = true;
    if (uart_tx_queue != NULL) {
      xQueueSendFromISR(uart_tx_queue, &tx_msg, &xHigherPriorityTaskWoken);
    }
  }
  
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

static void knob_right_cb(void *arg, void *data) {
  if (g_ship_ota_wake_window && !g_ui_initialized) {
    g_ship_ota_user_input = true;
    return;
  }
  // Ignore scrolls during brief wake-up period (150ms)
  if (millis() < scroll_ignore_until) {
    return;
  }
  user_activity_since_sleep = true;
  
  // Post to UI event queue for immediate UI feedback (optimistic update)
  app_event_t evt = {EVT_SCROLL_DELTA, {.scroll_delta = 1}};
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (app_event_queue != NULL) {
    xQueueSendFromISR(app_event_queue, &evt, &xHigherPriorityTaskWoken);
  }

  // Haptic tick (queued to UI task)
  app_event_t h_evt = {};
  h_evt.type = EVT_HAPTIC_TICK;
  if (app_event_queue != NULL) {
    xQueueSendFromISR(app_event_queue, &h_evt, &xHigherPriorityTaskWoken);
  }
  
  // Queue UART TX message (NOT sent here - uart_task will send it)
  if (!refresh_request_pending && !ui_busy) {
    tx_msg_t tx_msg = {};
    strncpy(tx_msg.type, "INPUT_SCROLL", sizeof(tx_msg.type) - 1);
    tx_msg.delta = 1;
    tx_msg.has_delta = true;
    if (uart_tx_queue != NULL) {
      xQueueSendFromISR(uart_tx_queue, &tx_msg, &xHigherPriorityTaskWoken);
    }
  }
  
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

// ── Sleep/Wake Functions ────────────────────────────────────────────
static uint64_t buildWakeMaskForSleep() {
  uint64_t wakeMask = 0;
 
  // Touch + encoder wake for scroll-to-wake in deep sleep.
  wakeMask |= (1ULL << LCD_WAKE_GPIO);
  wakeMask |= (1ULL << PIN_EC1_A);
  wakeMask |= (1ULL << PIN_EC1_B);
  return wakeMask;
}

static void enterLightSleep() {
  if (sleep_blocked_for_ota()) {
    Serial.println("[SLEEP] blocked (ota_pending)");
    resetActivityTimer();
    return;
  }
  Serial.println("========================================");
  Serial.println("Preparing for DEEP SLEEP...");
  Serial.println("========================================");
  uint16_t dummy_x = 0, dummy_y = 0;
  if (!notify_sense_sleep()) {
    if (sleep_deny_active) {
      if (millis() - last_sleep_retry_log_ms > 1000) {
        Serial.printf("[SLEEP] deny_wait reason=%s retry_ms=%lu\n",
                      sleep_deny_reason[0] ? sleep_deny_reason : "unknown",
                      sleep_deny_retry_ms > 0 ? sleep_deny_retry_ms : (unsigned long)SLEEP_DENY_RETRY_DEFAULT_MS);
        last_sleep_retry_log_ms = millis();
      }
      resetActivityTimer();
      return;
    }
    Serial.println("[SLEEP] Sense sleep not confirmed - staying awake");
    sleep_handshake_fail_count++;
    if (sleep_handshake_fail_count >= 3) {
      sleep_retry_requires_user = true;
    }
    unsigned long backoff_ms = sleep_retry_requires_user ? 60000UL
                          : (30000UL + (sleep_handshake_fail_count > 1 ? (sleep_handshake_fail_count - 1) * 10000UL : 0));
    if (backoff_ms > 60000UL) {
      backoff_ms = 60000UL;
    }
    sleep_retry_allowed_ms = millis() + backoff_ms;
    user_activity_since_sleep = false;
    if (millis() - last_sleep_retry_log_ms > 1000) {
      Serial.printf("[SLEEP] no_ready_timeout backoff_ms=%lu fail_count=%u require_user=%d\n",
                    backoff_ms,
                    sleep_handshake_fail_count,
                    sleep_retry_requires_user ? 1 : 0);
      last_sleep_retry_log_ms = millis();
    }
    resetActivityTimer();
    return;
  }

  sleep_handshake_fail_count = 0;
  sleep_retry_requires_user = false;
  sleep_retry_allowed_ms = 0;

  user_activity_since_sleep = false;
  sense_awake_confirmed = false;
  sense_state_set(SENSE_ASLEEP, "enter_sleep");
  g_in_light_sleep = true;
  
  // Reset UI to clean state before sleep (so it's ready on wake)
  Serial.println("[SLEEP] Resetting UI state for clean wake...");
  
  // Reset all UI state variables to defaults
  buttons_visible = false;
  meal_result_shown_time = 0;
  status_screen_shown_time = 0;
  delete_cooldown_until = 0;
  long_press_sent = false;
  touch_pressed = false;
  touch_press_time = 0;
  user_has_scrolled = false;
  pull_to_refresh_counter = 0;
  
  // If menu is visible, hide it and show the list before going to sleep
  if (menu_screen_visible) {
    Serial.println("[SLEEP] Menu is visible - hiding menu and showing list before sleep");
    hide_menu_screen();  // This function hides the menu and shows the list
  }
  
  menu_selected_index = 0;  // Reset menu selection
  menu_cooldown_until = 0;  // Reset menu cooldown on sleep
  logged_screen_shown_time = 0;  // Reset logged screen timeout on sleep
  
  // Stop any animations
  stop_glowing_animation();

  g_sleep_transition = true;
  Serial.println("[SLEEP] transition_begin");
  g_lvgl_running = false;
  
  // Save shopping list to persistent storage before sleep (with reset index)
  Serial.println("Saving shopping list to persistent storage...");
  if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    save_list_to_storage(&g_active);
    xSemaphoreGive(app_state_mutex);
  }

  // Hide all UI screens via UI task
  if (app_event_queue != NULL) {
    app_event_t reset_evt = {};
    reset_evt.type = EVT_RESET_UI;
    xQueueSend(app_event_queue, &reset_evt, pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  
  lcd_lvgl_wait_tx_done(200);
  Serial.println("[SLEEP] tx_idle");

  // Turn off LCD backlight + panel before deep sleep
  Serial.println("Turning off backlight for sleep...");
  setUpdutySubdivide(0);
  lcd_panel_set_power(false);
  g_backlight_duty = 0;
  g_panel_enabled = false;
  delay(50);
  
  lcd_wake_pin_set_mode(LCD_WAKE_GPIO, INPUT_PULLUP);
  Serial.printf("[WAKE_LINE] sleep_config board=%s wake_gpio=%d level=%d\n",
                HALO_BOARD_NAME,
                (int)LCD_WAKE_GPIO,
                digitalRead(LCD_WAKE_GPIO));
  // Ensure wake line to Sense is deasserted before sleeping.
  release_wake_line("pre_sleep");
  int sense_wake_level = digitalRead(INT_PIN);
  Serial.printf("[LCD_INT] before_sleep mode=INPUT_PULLUP level=%d\n",
                sense_wake_level);
  Serial.printf("[LCD_WAKE_PIN] mode=IN pullup=1 level=%d phase=pre_sleep\n",
                sense_wake_level);
  // Configure LCD wake pin (touch) for EXT0 sanity checks.
  lcd_wake_pin_set_mode(LCD_WAKE_GPIO, OUTPUT);
  digitalWrite(LCD_WAKE_GPIO, HIGH);
  delay(2);
  lcd_wake_pin_set_mode(LCD_WAKE_GPIO, INPUT_PULLUP);
  int wake_pin_level = digitalRead(LCD_WAKE_GPIO);
  Serial.printf("[LCD_SLEEP_CFG] ext0_gpio=%d ext0_level=%d pin_level_now=%d\n",
                (int)LCD_WAKE_GPIO, (int)LCD_WAKE_LEVEL, wake_pin_level);
  Serial.printf("[SLEEP_SANITY] wake_pin_level=%d wake_level=%d ext0_gpio=%d\n",
                wake_pin_level, LCD_WAKE_LEVEL, (int)LCD_WAKE_GPIO);
  if (wake_pin_level == LCD_WAKE_LEVEL) {
    if (wake_line_active_for_ms(WAKE_LINE_STUCK_WARN_MS)) {
      Serial.printf("[WAKE_LINE][WARN] stuck_active gpio=%d level=%d held_ms=%lu\n",
                    (int)LCD_WAKE_GPIO,
                    (int)LCD_WAKE_LEVEL,
                    (unsigned long)WAKE_LINE_STUCK_WARN_MS);
    }
    Serial.println("[SLEEP_SANITY] wake pin already at wake level; refusing_sleep");
    delay(250);
    return;
  }
  uint32_t sleep_timer_sec = (sleep_fallback_timer_sec > 0)
                               ? sleep_fallback_timer_sec
                               : LCD_OTA_WAKE_INTERVAL_SEC;
  configure_sleep_sources(true, sleep_timer_sec);
  if (sleep_fallback_timer_sec > 0) {
    Serial.printf("[SLEEP_PROTO] fallback_timer_active timer_s=%lu\n",
                  (unsigned long)sleep_fallback_timer_sec);
    sleep_fallback_timer_sec = 0;
  }

  Serial.printf("[SLEEP_STATE] entering_deep_sleep now_ms=%lu ext0_gpio=%d ext0_level=%d\n",
                (unsigned long)millis(),
                (int)LCD_WAKE_GPIO,
                (int)LCD_WAKE_LEVEL);
  Serial.println("[SLEEP] entering_deep_sleep");
  sleep_entry_time = millis();

  if (ui_task_handle != NULL) {
    vTaskDelete(ui_task_handle);
    ui_task_handle = NULL;
  }
  
  // Enter deep sleep (no return)
  esp_deep_sleep_start();
  
  // Execution resumes here after wake
  Serial.end();
  delay(10);
  Serial.begin(115200);
  delay(50);
  g_sleep_transition = false;
  g_lvgl_running = true;
  Serial.printf("[LCD_SLEEP_DIAG] woke_from_deep_sleep wake_cause=%d wake_pin=%d ms=%lu\n",
                (int)esp_sleep_get_wakeup_cause(),
                digitalRead(LCD_WAKE_GPIO),
                (unsigned long)millis());
  
  Serial.println("\n========================================");
  Serial.println("Woke from DEEP SLEEP!");
  Serial.println("========================================");
  
  // Check for false wakeup
  unsigned long time_since_sleep_entry = millis() - sleep_entry_time;
  if (time_since_sleep_entry < WAKE_DEBOUNCE_MS) {
    Serial.println("[SLEEP] False wakeup detected - going back to sleep...");
    dummy_x = 0;
    dummy_y = 0;
    getTouch(&dummy_x, &dummy_y);
    for (int i = 0; i < 5; i++) {
      digitalRead(PIN_EC1_A);
      digitalRead(PIN_EC1_B);
      vTaskDelay(pdMS_TO_TICKS(5));  // Use vTaskDelay to yield to other tasks
    }
    uint64_t wakeMask = buildWakeMaskForSleep();
    esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    sleep_entry_time = millis();
    esp_light_sleep_start();
  }

  if (lcd_wake_pins_active()) {
    user_activity_since_sleep = true;
  }
  
  // Log wake-up cause
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  int wake_level = digitalRead(LCD_WAKE_GPIO);
  const char* wake_cause_label = "unknown";
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    static char gpio_label[16];
    snprintf(gpio_label, sizeof(gpio_label), "gpio%d", (int)LCD_WAKE_GPIO);
    wake_cause_label = gpio_label;
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    wake_cause_label = "timer";
#if defined(ESP_SLEEP_WAKEUP_UART)
  } else if (cause == ESP_SLEEP_WAKEUP_UART) {
    wake_cause_label = "uart";
#endif
  }
  Serial.printf("[WAKE] cause=%s level=%d ts=%lu\n",
                wake_cause_label,
                wake_level,
                (unsigned long)millis());
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    unsigned long now_ms = millis();
    unsigned long until = now_ms + 8000;
    if (until > stay_awake_until_ms) {
      stay_awake_until_ms = until;
    }
  }
  link_sync_pending = true;
  bool user_ui_wake = (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1);
  Serial.print("Wake-up cause: ");
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("EXT1 (touch or encoder)");
      break;
    default:
      Serial.println("Unknown");
      break;
  }
  
  g_in_light_sleep = false;
  
  // Track wake time to prevent activity timer reset from background updates
  last_wake_time = millis();
  just_woke_up = true;  // Set flag to trigger UI reset on next list render
  
  // Clear touch interrupts
  dummy_x = 0;
  dummy_y = 0;
  getTouch(&dummy_x, &dummy_y);
  vTaskDelay(pdMS_TO_TICKS(10));  // Use vTaskDelay to yield to other tasks
  getTouch(&dummy_x, &dummy_y);
  
  // Clear encoder interrupts
  for (int i = 0; i < 5; i++) {
    digitalRead(PIN_EC1_A);
    digitalRead(PIN_EC1_B);
    vTaskDelay(pdMS_TO_TICKS(5));  // Use vTaskDelay to yield to other tasks
  }
  
  // Ignore touches for a short period after wake (prevent wake touch from triggering UI)
  touch_ignore_until = millis() + 300;
  // Minimal scroll ignore (150ms) - just enough to prevent wake scroll from scrolling
  scroll_ignore_until = millis() + 150;
  
  // Turn backlight back on
  Serial.println("[WAKE] Turning backlight ON after wake");
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  g_backlight_initialized = true;
  g_backlight_duty = 255;
  g_panel_enabled = true;
  vTaskDelay(pdMS_TO_TICKS(50));  // Use vTaskDelay to yield to other tasks
  
  // Set flag to trigger UI reset on next list render (state was already cleared before sleep)
  just_woke_up = true;
  
  // Load saved list (selected_index was already reset to 0 before sleep)
  Serial.println("[WAKE] Loading saved list from storage...");
  int saved_count = 0;
  // Reduce mutex timeout to prevent long blocking (500ms instead of 1000ms)
  if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    saved_count = load_list_from_storage(&g_active);
    
    // Ensure selected_index is 0 (should already be set from before sleep, but double-check)
    g_active.selected_index = (saved_count > 0) ? 0 : -1;
    
    Serial.printf("[WAKE] Loaded %d items, selected_index=%d\n", 
                  saved_count, g_active.selected_index);
    
    xSemaphoreGive(app_state_mutex);
    
    // Post event to UI task to render and reset UI (LVGL must be called from UI task)
    if (saved_count > 0 && app_event_queue != NULL) {
      app_event_t evt = {EVT_RENDER_ACTIVE_LIST, {.new_count = saved_count}};
      xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(100));
      Serial.println("[WAKE] Queued render event for saved list at index 0");
    } else {
      Serial.println("[WAKE] No saved list found");
    }
  } else {
    Serial.println("[WAKE] WARNING: Failed to acquire mutex for loading saved list");
  }
  
  // Wake Sense board (but don't request list refresh - user will do that via pull-to-refresh)
  Serial.println("[WAKE] Waking Sense board (no automatic refresh)...");
  if (should_wake_sense()) {
    lcd_maybe_pulse_sense_int("wake_from_sleep");
  } else if (user_ui_wake) {
    Serial.println("[LCD_INT] skip_pulse reason=user_ui_wake");
    if (last_sense_rx_ms == 0 || (millis() - last_sense_rx_ms) > SENSE_RX_STALE_MS) {
      sense_state_set(SENSE_UNKNOWN, "user_ui_wake");
    }
  } else {
    Serial.println("[LCD_INT] skip_pulse reason=no_pending_ops");
  }
  
  // NOTE: We do NOT send INPUT_WAKE here anymore - user must manually trigger refresh via pull-to-refresh
  
  vTaskDelay(pdMS_TO_TICKS(200));  // Use vTaskDelay to yield to other tasks
  
  Serial.println("LIGHT SLEEP wake handling complete.");
}

static uint32_t send_input_sleep_message() {
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_SLEEP";
  uint32_t msg_id = get_next_msg_id();
  doc["msg_id"] = msg_id;
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
  Serial.printf("[SLEEP_PROTO] tx INPUT_SLEEP msg_id=%u\n", (unsigned)msg_id);
  return msg_id;
}

static void sleep_enter_wait_low_power(const char* reason) {
  if (g_backlight_initialized && g_backlight_duty > SLEEP_WAIT_DIM_DUTY) {
    setUpdutySubdivide(SLEEP_WAIT_DIM_DUTY);
    g_backlight_duty = SLEEP_WAIT_DIM_DUTY;
  }
  if (g_lvgl_running) {
    g_lvgl_running = false;
  }
  Serial.printf("[SLEEP] wait_low_power reason=%s backlight=%d lvgl_running=%d\n",
                reason ? reason : "unknown",
                g_backlight_duty,
                g_lvgl_running ? 1 : 0);
}

// Send sleep signal to Sense board before LCD goes to sleep
static bool notify_sense_sleep() {
  Serial.println("[LCD] Notifying Sense board to sleep...");
  sleep_ack_received = false;
  sleep_ready_received = false;
  sleep_busy_received = false;
  sleep_deny_received = false;
  sleep_deny_retry_ms = 0;
  sleep_deny_reason[0] = '\0';
  sleep_deny_received_ms = 0;
  sleep_deny_active = false;
  sleep_fallback_timer_sec = 0;
  if (provisioning_active) {
    Serial.println("[LCD] Sleep suppressed (provisioning active)");
    return false;
  }
  if (sleep_blocked_for_ota()) {
    Serial.println("[SLEEP] abort handshake (ota_pending)");
    return false;
  }
  unsigned long now_ms = millis();
  refresh_sense_awake_estimate(now_ms);
  unsigned long age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0;
  Serial.printf("[SLEEP] pre_handshake awake_est=%d age_ms=%lu\n",
                sense_awake_estimate ? 1 : 0,
                age_ms);
  bool grace_active = now_ms < sense_awake_grace_until_ms;
  bool sense_recent = (last_sense_rx_ms > 0) &&
                      (now_ms - last_sense_rx_ms) < SENSE_RECENT_RX_FOR_SLEEP_MS;
  Serial.printf("[SLEEP] decision est=%d recent=%d grace=%d age_ms=%lu\n",
                sense_awake_estimate ? 1 : 0,
                sense_recent ? 1 : 0,
                grace_active ? 1 : 0,
                age_ms);

  if (sleep_ready_received) {
    Serial.println("[SLEEP_PROTO] got SLEEP_READY while waiting_for_sleep_ack -> success");
    return true;
  }

  if (!link_synced || sense_state != SENSE_AWAKE || age_ms > SENSE_RECENT_RX_FOR_SLEEP_MS) {
    Serial.printf("[SLEEP_PROTO] skip INPUT_SLEEP reason=sense_not_awake rx_age=%lu synced=%d state=%s\n",
                  age_ms,
                  link_synced ? 1 : 0,
                  sense_state_name(sense_state));
    return true;
  }

  const unsigned long ready_timeout_ms = 6000;
  for (uint8_t attempt = 1; attempt <= SLEEP_HANDSHAKE_MAX_ATTEMPTS; ++attempt) {
    if (sleep_ready_received) {
      Serial.println("[SLEEP_PROTO] got SLEEP_READY while waiting_for_sleep_ack -> success");
      return true;
    }
    sleep_ack_received = false;
    sleep_ready_received = false;
    sleep_busy_received = false;
    sleep_deny_received = false;
    send_input_sleep_message();
    unsigned long start = millis();
    unsigned long deadline_ms = start + ready_timeout_ms;
    Serial.printf("[SLEEP] sent INPUT_SLEEP attempt=%u timeout_ms=%lu\n",
                  (unsigned)attempt,
                  (unsigned long)ready_timeout_ms);
    Serial.printf("[SLEEP] req sent est=%d recent=%d grace=%d wait_ms=%u\n",
                  sense_awake_estimate ? 1 : 0,
                  sense_recent ? 1 : 0,
                  grace_active ? 1 : 0,
                  (unsigned)ready_timeout_ms);
    bool ack_logged = false;
    while (millis() < deadline_ms) {
      if (sleep_blocked_for_ota()) {
        Serial.println("[SLEEP] abort wait (ota_pending)");
        return false;
      }
      if (sleep_deny_received || sleep_busy_received) {
        const char* deny_reason = sleep_deny_received ? sleep_deny_reason : "op_inflight";
        uint32_t retry_ms = sleep_deny_retry_ms > 0 ? sleep_deny_retry_ms : SLEEP_DENY_RETRY_DEFAULT_MS;
        sleep_deny_active = true;
        sleep_retry_allowed_ms = millis() + retry_ms;
        sleep_handshake_fail_count = 0;
        sleep_retry_requires_user = false;
        sleep_enter_wait_low_power(deny_reason);
        Serial.printf("[SLEEP_PROTO] rx DENY reason=%s retry_ms=%lu\n",
                      deny_reason ? deny_reason : "unknown",
                      (unsigned long)retry_ms);
        return false;
      }
      if (sleep_ready_received) {
        Serial.println("[SLEEP_PROTO] got SLEEP_READY while waiting_for_sleep_ack -> success");
        Serial.println("[SLEEP] got_ready -> sleeping");
        Serial.println("[SLEEP_PROTO] decision coordinated reason=ready");
        return true;
      }
      if (sleep_ack_received && !ack_logged) {
        ack_logged = true;
        start = millis();
        deadline_ms = start + ready_timeout_ms;
        Serial.println("[SLEEP] got_ack during wait -> continue waiting_ready");
      }
      if (ota_stay_awake_until_ms > deadline_ms) {
        deadline_ms = ota_stay_awake_until_ms;
      }
      vTaskDelay(pdMS_TO_TICKS(40));
    }
    if (attempt < SLEEP_HANDSHAKE_MAX_ATTEMPTS) {
      Serial.printf("[SLEEP_PROTO] timeout attempt=%u -> retry\n", (unsigned)attempt);
      send_sense_ping();
      vTaskDelay(pdMS_TO_TICKS(SLEEP_HANDSHAKE_RETRY_DELAY_MS));
      continue;
    }
  }
  {
    unsigned long now_ms = millis();
    unsigned long rx_age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0xFFFFFFFFUL;
    bool allow_fallback = (!link_synced) || (rx_age_ms > 30000UL);
    if (allow_fallback) {
      sleep_fallback_timer_sec = SLEEP_FALLBACK_TIMER_SEC;
      Serial.printf("[SLEEP_PROTO][ERROR] no_response_to_INPUT_SLEEP synced=%d rx_age=%lu sense_state=%s attempts=%u -> fallback_timer_sleep\n",
                    link_synced ? 1 : 0,
                    rx_age_ms,
                    sense_state_name(sense_state),
                    (unsigned)SLEEP_HANDSHAKE_MAX_ATTEMPTS);
      return true;
    }
  }
  return false;
}

// ── UART Message Processing ─────────────────────────────────────────
// CRITICAL: This function runs from uart_task - NO LVGL calls allowed!
// Only writes to g_pending buffer and posts events to UI task
static void uart_process_received_message(const char* json_str) {
  // Skip empty messages
  if (json_str == NULL || strlen(json_str) == 0) {
    return;
  }
  const char* p = json_str;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (*p == '\0') {
    return;
  }
  if (strncmp(p, "ESP-ROM:", 8) == 0 ||
      strncmp(p, "rst:", 4) == 0 ||
      strncmp(p, "load:", 5) == 0 ||
      strncmp(p, "entry", 5) == 0 ||
      strncmp(p, "Build:", 6) == 0) {
    static unsigned long last_drop_log_ms = 0;
    if (millis() - last_drop_log_ms > 5000) {
      char preview[32];
      strncpy(preview, p, sizeof(preview) - 1);
      preview[sizeof(preview) - 1] = '\0';
      Serial.printf("[UART] drop_nonjson line_prefix=%s\n", preview);
      last_drop_log_ms = millis();
    }
    return;
  }
  if (*p != '{') {
    static unsigned long last_drop_log_ms = 0;
    if (millis() - last_drop_log_ms > 5000) {
      char preview[32];
      strncpy(preview, p, sizeof(preview) - 1);
      preview[sizeof(preview) - 1] = '\0';
      Serial.printf("[UART] drop_nonjson line_prefix=%s\n", preview);
      last_drop_log_ms = millis();
    }
    return;
  }

  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, p);
  
  if (error) {
    Serial.printf("[PROTO] JSON parse error: %s (len=%d)\n", error.c_str(), strlen(json_str));
    // Log first 100 chars for debugging
    if (strlen(json_str) > 0) {
      char preview[101];
      strncpy(preview, json_str, 100);
      preview[100] = '\0';
      Serial.printf("[PROTO] First 100 chars: %s\n", preview);
    }
    return;
  }
  
  if (!validate_protocol_message(doc)) {
    return;  // Validation failed, message dropped
  }

  const char* type = doc["type"] | "";
  unsigned long prev_rx_ms = last_sense_rx_ms;
  note_sense_link_rx(type);
  if (sense_rx_type_is_awake_proof(type)) {
    last_sense_rx_ms = millis();
    set_sense_awake_estimate(true, type);
  }
  last_sense_any_rx_ms = millis();
  last_sense_msg_ms = last_sense_any_rx_ms;
  Serial.printf("[PROTO] RX: type=%s\n", type);
#if SHIP_MENU_UI
  if (strcmp(type, "UI_STATUS") == 0) {
    ship_menu_handle_ui_status(doc);
    return;
  }
  if (strcmp(type, "UI_LIST") == 0 ||
      strcmp(type, "UI_MEAL_RESULT") == 0 ||
      strcmp(type, "UI_VOICE_ITEMS") == 0 ||
      strcmp(type, "PROVISION_QR") == 0 ||
      strcmp(type, "PROVISION_STATUS") == 0) {
    Serial.printf("[SHIP_MENU] ignore type=%s\n", type);
    return;
  }
#endif
  if (waiting_for_sense_cmds) {
    waiting_for_sense_cmds = false;
    waiting_for_sense_logged = false;
  }
  
  if (strcmp(type, "PONG") == 0) {
    unsigned long now_ms = millis();
    unsigned long age_ms = prev_rx_ms > 0 ? (now_ms - prev_rx_ms) : 0;
    last_sense_rx_ms = now_ms;
    set_sense_awake_estimate(true, "PONG");
    Serial.printf("[SLEEP] sense_awake_estimate -> 1 reason=PONG age_ms=%lu\n",
                  age_ms);
    sense_awake_confirmed = true;
    sense_wake_explicit_request = false;
    wake_retry_until_ms = 0;
    note_sense_proof_of_life("PONG");
    refresh_sm_on_awake_proof("PONG");
    return;
  }

  if (strcmp(type, "LINK_HB") == 0) {
    unsigned long now_ms = millis();
    unsigned long age_ms = prev_rx_ms > 0 ? (now_ms - prev_rx_ms) : 0;
    Serial.printf("[LINK_HB] rx age_ms=%lu\n", age_ms);
    return;
  }
  
  if (strcmp(type, "SLEEP_ACK") == 0 || strcmp(type, "INPUT_SLEEP_ACK") == 0) {
    sleep_ack_received = true;
    last_sense_rx_ms = millis();
    set_sense_awake_estimate(true, "SLEEP_ACK");
    Serial.printf("[SLEEP_PROTO] rx %s\n", type);
    return;
  }

  if (strcmp(type, "SLEEP_BUSY") == 0) {
    sleep_busy_received = true;
    last_sense_rx_ms = millis();
    set_sense_awake_estimate(true, "SLEEP_BUSY");
    Serial.println("[SLEEP_PROTO] rx SLEEP_BUSY");
    sleep_deny_received = true;
    sleep_deny_retry_ms = SLEEP_DENY_RETRY_DEFAULT_MS;
    strncpy(sleep_deny_reason, "op_inflight", sizeof(sleep_deny_reason) - 1);
    sleep_deny_reason[sizeof(sleep_deny_reason) - 1] = '\0';
    sleep_deny_received_ms = millis();
    return;
  }

  if (strcmp(type, "SLEEP_DENY") == 0) {
    sleep_deny_received = true;
    sleep_deny_retry_ms = (uint32_t)(doc["retry_ms"] | 0);
    const char* reason = doc["reason"] | "";
    strncpy(sleep_deny_reason, reason, sizeof(sleep_deny_reason) - 1);
    sleep_deny_reason[sizeof(sleep_deny_reason) - 1] = '\0';
    sleep_deny_received_ms = millis();
    last_sense_rx_ms = millis();
    set_sense_awake_estimate(true, "SLEEP_DENY");
    Serial.printf("[SLEEP_PROTO] rx SLEEP_DENY reason=%s retry_ms=%lu\n",
                  sleep_deny_reason,
                  (unsigned long)sleep_deny_retry_ms);
    return;
  }

  if (strcmp(type, "SLEEP_READY") == 0) {
    sleep_ready_received = true;
    last_sense_rx_ms = millis();
    sense_state_set(SENSE_ASLEEP, "SLEEP_READY");
    Serial.println("[SLEEP_PROTO] rx SLEEP_READY");
    Serial.println("[SLEEP] got SLEEP_READY -> release_wake_line");
    release_wake_line("sleep_ready");
    return;
  }

  if (strcmp(type, "RELEASE_WAKE") == 0) {
    Serial.println("[SLEEP_PROTO] rx RELEASE_WAKE");
    release_wake_line("release_wake");
    return;
  }

  if (strcmp(type, "STATUS_SYNC") == 0) {
    sense_status_sync_requested = true;
    Serial.println("[UART] STATUS_SYNC received");
    return;
  }

  if (strcmp(type, "MAINT_WINDOW") == 0) {
    uint32_t remaining_s = doc["remaining_s"] | 0;
    g_lcd_maintenance_active = true;
    g_lcd_maintenance_started = false;
    g_lcd_maintenance_aborted = false;
    lcd_mode = LCD_MODE_MAINTENANCE;
    g_lcd_maintenance_deadline_ms = millis() + (unsigned long)remaining_s * 1000UL;
    Serial.printf("[UART] MAINT_WINDOW received remaining_s=%lu\n",
                  (unsigned long)remaining_s);
#ifdef HALO_LCD_PROD_WRAPPER
    halo_lcd_prod_on_wifi_on(remaining_s * 1000UL);
#endif
    return;
  }

  if (strcmp(type, "SYNC") == 0) {
    Serial.println("[LNK] sync_rx -> reset_parser");
    lcd_uart_reset_rx_state();
    link_synced = true;
    Serial.println("[LNK] synced=1");
    uart_send_ack("SYNC_ACK");
    Serial.println("[LNK] sync_ack_tx");
    return;
  }

  if (strcmp(type, "SYNC_ACK") == 0) {
    link_synced = true;
    Serial.println("[LNK] synced=1");
    note_sense_proof_of_life("SYNC_ACK");
    refresh_sm_on_awake_proof("SYNC_ACK");
    return;
  }

  if (strcmp(type, "WIFI_CREDS") == 0) {
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["pass"] | "";
    bool has_pass = doc.containsKey("pass") && pass && pass[0];
    bool pass_present = (doc["pass_present"] | (has_pass ? 1 : 0)) != 0;
    if (ssid[0] == '\0') {
      Serial.println("[UART][WIFI_CREDS] reject empty ssid");
      uart_send_wifi_creds_ack("ERR", 1);
      return;
    }
#ifdef HALO_LCD_PROD_WRAPPER
    if (has_pass) {
      halo_lcd_prod_on_wifi_creds(ssid, pass);
    } else {
      Serial.printf("[UART][WIFI_CREDS] pass_omitted present=%d (keeping existing creds)\n",
                    pass_present ? 1 : 0);
    }
#else
    Serial.println("[UART][WIFI_CREDS] ignored (no prod wrapper)");
#endif
    uart_send_wifi_creds_ack(
#ifdef HALO_LCD_PROD_WRAPPER
      "OK", 0
#else
      "ERR", 2
#endif
    );
    return;
  }

  if (strcmp(type, "WIFI_ON") == 0) {
    uint32_t timeout_ms = doc["timeout_ms"] | 0;
#if SHIP_MENU_UI
    lcd_mode = LCD_MODE_UI_ACTIVE;
#endif
    bool ui_active = (lcd_mode == LCD_MODE_UI_ACTIVE);
#if SHIP_MENU_UI
    ui_active = !wifi_on_ui_idle();
#endif
    if (ui_active) {
      wifi_on_deferred = true;
      wifi_on_deferred_timeout_ms = timeout_ms;
      wifi_on_deferred_logged = false;
      if (!wifi_on_reject_logged) {
        Serial.println("[LCD_WIFI] DEFER WIFI_ON (UI_ACTIVE)");
        wifi_on_reject_logged = true;
      }
      uart_send_wifi_on_ack("DEFERRED", "UI_ACTIVE");
      return;
    }
    wifi_on_reject_logged = false;
    wifi_on_execute(timeout_ms, "uart");
    uart_send_wifi_on_ack("OK", ""); 
    return;
  }
  
  if (strcmp(type, "OTA_LOCK") == 0) {
    ota_locked = true;
    ota_lock_at_ms = millis();
    if (ota_check_requested) {
      ota_check_pending = true;
      ota_check_requested = false;
    }
    Serial.println("[OTA] lock received - blocking LCD OTA");
    return;
  } else if (strcmp(type, "OTA_UNLOCK") == 0) {
    ota_locked = false;
    Serial.println("[OTA] unlock received - LCD OTA may proceed");
    if (ota_check_pending) {
      ota_check_pending = false;
      ota_check_requested = true;
      unsigned long until = millis() + OTA_STAY_AWAKE_MS;
      if (until > ota_stay_awake_until_ms) {
        ota_stay_awake_until_ms = until;
      }
      Serial.println("[OTA] unlock -> queued deferred OTA check");
    }
    return;
  } else if (strcmp(type, "OTA_CHECK") == 0) {
    uint32_t request_id = (uint32_t)(doc["request_id"] | 0);
    const char* reason = doc["reason"] | "";
    bool allow_reboot = doc["allow_reboot"] | true;
    lcd_ota_request_active = true;
    lcd_ota_request_id = request_id;
    lcd_ota_request_allow_reboot = allow_reboot;
    lcd_ota_request_start_ms = millis();
    strncpy(lcd_ota_request_reason, reason ? reason : "", sizeof(lcd_ota_request_reason) - 1);
    lcd_ota_request_reason[sizeof(lcd_ota_request_reason) - 1] = '\0';
    lcd_send_ota_check_ack(request_id);

    unsigned long idle_ms = (last_user_activity_ms > 0) ? (millis() - last_user_activity_ms) : 0xFFFFFFFFUL;
    if (!allow_reboot && idle_ms < LCD_OTA_USER_ACTIVE_GRACE_MS) {
      Serial.printf("[OTA] OTA_CHECK skip user_active idle_ms=%lu\n", idle_ms);
      lcd_ota_request_finish("fail", "user_active", kFirmwareVersion, "user_active");
      return;
    }

    if (ota_locked) {
      ota_check_pending = true;
      Serial.println("[OTA] check received while locked - deferred");
    } else {
      Serial.println("[OTA] OTA_CHECK received");
      ota_check_requested = true;
      if (g_sleep_transition) {
        g_sleep_transition = false;
        Serial.println("[OTA] abort sleep transition (ota_check)");
      }
      if (g_in_light_sleep) {
        g_in_light_sleep = false;
      }
      resetActivityTimer();
      unsigned long until = millis() + LCD_OTA_CHECK_STAY_AWAKE_MS;
      if (until > ota_stay_awake_until_ms) {
        ota_stay_awake_until_ms = until;
      }
      Serial.printf("[OTA] extending awake window %lu ms\n", (unsigned long)LCD_OTA_CHECK_STAY_AWAKE_MS);
    }
    return;
  } else if (strcmp(type, "OTA_APPLY_REQUIRED") == 0) {
    sense_ota_apply_required = true;
    Serial.println("[OTA] apply required - will wake Sense if needed");
    return;
  } else if (strcmp(type, "PROVISION_QR") == 0) {
    provisioning_active = true;
    const char* ssid = doc["ssid"] | "";
    const char* password = doc["password"] | "";
    const char* url = doc["url"] | "http://192.168.4.1";
    if (app_event_queue != NULL) {
      app_event_t evt = {};
      evt.type = EVT_SHOW_PROVISION_QR;
      strncpy(evt.data.provision.ssid, ssid, sizeof(evt.data.provision.ssid) - 1);
      evt.data.provision.ssid[sizeof(evt.data.provision.ssid) - 1] = '\0';
      strncpy(evt.data.provision.password, password, sizeof(evt.data.provision.password) - 1);
      evt.data.provision.password[sizeof(evt.data.provision.password) - 1] = '\0';
      strncpy(evt.data.provision.url, url, sizeof(evt.data.provision.url) - 1);
      evt.data.provision.url[sizeof(evt.data.provision.url) - 1] = '\0';
      xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(50));
      Serial.println("[UART] PROVISION_QR received - queued UI update");
    }
    return;
  } else if (strcmp(type, "PROVISION_STATUS") == 0) {
    const char* state = doc["state"] | "";
    if (state != NULL && strlen(state) > 0) {
      bool active = (strcmp(state, "connected") != 0 && strcmp(state, "idle") != 0);
      provisioning_active = active;
      if (strcmp(state, "connected") == 0) {
        provision_refresh_pending = true;
      }
    }
    if (app_event_queue != NULL) {
      app_event_t evt = {};
      if (strcmp(state, "connected") == 0) {
        evt.type = EVT_HIDE_PROVISION_QR;
      } else {
        evt.type = EVT_UPDATE_PROVISION_STATUS;
        strncpy(evt.data.provision_status.state, state, sizeof(evt.data.provision_status.state) - 1);
        evt.data.provision_status.state[sizeof(evt.data.provision_status.state) - 1] = '\0';
      }
      xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(50));
      Serial.printf("[UART] PROVISION_STATUS received: %s\n", state);
    }
    return;
  }

  if (strcmp(type, "UI_LIST") == 0) {
    unsigned long now_ms = millis();
    if (!lcd_refresh_inflight &&
        lcd_last_ui_list_complete_ms > 0 &&
        (now_ms - lcd_last_ui_list_complete_ms) < LCD_UI_LIST_DEDUPE_MS) {
      Serial.println("[UART] UI_LIST deduped (recent completion)");
      return;
    }
    // Phase 0: UI_LIST (full list replacement)
    int selected_index = doc["selected_index"] | -1;
    
    // Update list from items array - write to g_pending buffer
    if (doc.containsKey("items") && doc["items"].is<JsonArray>()) {
      JsonArray items = doc["items"];
      int count = 0;
      
      // Acquire mutex to write to g_pending
      if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // CRITICAL: Preserve optimistic voice items (items with empty IDs at the top of g_active)
        // These are items that were added optimistically but haven't been processed by backend yet
        int optimistic_count = 0;
        for (int i = 0; i < g_active.count; i++) {
          // Items with empty IDs at the top are optimistic voice items
          if (g_active.item_ids[i][0] == '\0' && i < 10) {  // Check first 10 items for optimistic items
            optimistic_count++;
          } else {
            break;  // Once we hit an item with an ID, stop counting optimistic items
          }
        }
        
        // Start by copying optimistic items to pending (they go at the top)
        g_pending.count = 0;
        if (optimistic_count > 0) {
          Serial.printf("[UART] Preserving %d optimistic voice items at top of list\n", optimistic_count);
          for (int i = 0; i < optimistic_count && g_pending.count < MAX_LIST_ITEMS; i++) {
            strncpy(g_pending.items[g_pending.count], g_active.items[i], 63);
            g_pending.items[g_pending.count][63] = '\0';
            strncpy(g_pending.item_ids[g_pending.count], g_active.item_ids[i], 63);
            g_pending.item_ids[g_pending.count][63] = '\0';
            g_pending.count++;
          }
        }
        
        // Now process items from UI_LIST (backend-processed items)
        // For each item from backend, check if it matches an optimistic item by text
        // If it matches, replace the optimistic item (give it the real ID)
        // If it doesn't match, add it to the list
        for (JsonObject item : items) {
          const char* text = item["text"] | "";
          const char* id = item["id"] | "";
          
          // CRITICAL: Filter out deleted items - if this item's ID is in deleted_item_ids, skip it
          bool is_deleted = false;
          if (id != NULL && strlen(id) > 0) {
            for (int i = 0; i < deleted_item_count; i++) {
              if (strcmp(id, deleted_item_ids[i]) == 0) {
                is_deleted = true;
                Serial.printf("[UART] Filtering out deleted item: %s (ID: %s)\n", text, id);
                break;
              }
            }
          }
          
          // Skip deleted items
          if (is_deleted) {
            continue;
          }
          
          // Check if this item matches an optimistic item by text (case-insensitive match)
          bool found_in_optimistic = false;
          if (text != NULL && strlen(text) > 0) {
            for (int i = 0; i < optimistic_count && i < g_pending.count; i++) {
              // Match by text (case-insensitive)
              if (strcasecmp(g_pending.items[i], text) == 0) {
                // Replace optimistic item with real item (has ID now)
                strncpy(g_pending.items[i], text, 63);
                g_pending.items[i][63] = '\0';
                strncpy(g_pending.item_ids[i], id, 63);
                g_pending.item_ids[i][63] = '\0';
                found_in_optimistic = true;
                Serial.printf("[UART] Replaced optimistic item with real item: %s (ID: %s)\n", text, id);
                break;
              }
            }
          }
          
          // If not found in optimistic items, add it to the list (after optimistic items)
          if (!found_in_optimistic && g_pending.count < MAX_LIST_ITEMS) {
            strncpy(g_pending.items[g_pending.count], text, 63);
            g_pending.items[g_pending.count][63] = '\0';
            strncpy(g_pending.item_ids[g_pending.count], id, 63);
            g_pending.item_ids[g_pending.count][63] = '\0';
            g_pending.count++;
          }
          
          count++;  // Track total items processed (for logging)
        }
        
        // g_pending.count is already correct (incremented as we add items)
        // Store Sense's selected_index in pending (UI task will decide whether to use it)
        if (selected_index >= 0 && selected_index < g_pending.count) {
          g_pending.selected_index = selected_index;
        } else {
          g_pending.selected_index = (g_pending.count > 0) ? 0 : -1;
        }
        
        // Mark pending as ready
        pending_ready = true;
        refresh_request_needs_send = false;
        
        Serial.printf("[UART] Received UI_LIST: count=%d (after filtering deleted items), selected_index=%d (pending ready)\n", 
                      count, g_pending.selected_index);
        
        xSemaphoreGive(app_state_mutex);
        
        // UART task must NOT call LVGL - UI task will stop_glowing and refresh_sm_set_state when it handles EVT_LIST_REPLACED
        note_sense_proof_of_life("LIST");
        refresh_note_ui_proof("UI_LIST");
        if (refresh_state == REFRESH_INFLIGHT) {
          refresh_sm_set_state(REFRESH_COMPLETE, "list_received");
          refresh_success_count++;
        }
        waiting_for_list_response = false;
        refresh_request_pending = false;
        refresh_input_wake_sent = false;
        refresh_request_retry_count = 0;
        refresh_grace_extended = false;
        lcd_refresh_inflight = false;
        lcd_refresh_ack_seen = false;
        lcd_refresh_retry_count = 0;
        lcd_refresh_sent_ms = 0;
        lcd_last_ui_list_complete_ms = now_ms;
        
        // Post event to UI task to swap pending → active and render
        if (app_event_queue != NULL) {
          app_event_t evt = {EVT_LIST_REPLACED, {.new_count = count}};
          xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(100));
          Serial.println("[UART] Sent EVT_LIST_REPLACED event to UI task");
        }
        if (refresh_requested_again) {
          refresh_requested_again = false;
          Serial.println("[REFRESH] queued another refresh after completion");
          refresh_sm_set_wake_pending("refresh_again");
          lcd_refresh_inflight = true;
          lcd_refresh_ack_seen = false;
          lcd_refresh_retry_count = 0;
          lcd_refresh_sent_ms = lcd_refresh_start_ms;
          waiting_for_list_response = true;
          refresh_request_pending = true;
          refresh_request_needs_send = false;
          refresh_input_wake_sent = false;
          refresh_request_retry_count = 0;
          refresh_grace_extended = false;
          refresh_request_start_ms = lcd_refresh_start_ms;
          refresh_request_last_ms = refresh_request_start_ms;
        }
      }
    }
  } else if (strcmp(type, "UI_STATUS") == 0) {
    // Extended status message from Sense board
    const char* op = doc["op"] | "";
    const char* phase = doc["phase"] | "";
    const char* text = doc["text"] | "";
    Serial.printf("[UART] Status: op=%s, phase=%s, text=%s\n", op, phase, text);
    note_sense_proof_of_life("UI_STATUS");
    refresh_note_ui_proof("UI_STATUS");
    // Any UI_STATUS means Sense is awake and responding.
    sense_awake_confirmed = true;
    sense_wake_explicit_request = false;
    wake_retry_until_ms = 0;
    
    // If this is a generic status (no op), treat it as list/API wait
    if (op[0] == '\0') {
      if (strncmp(text, "ERROR", 5) == 0) {
        if (refresh_state == REFRESH_INFLIGHT) {
          refresh_soft_fail("ui_status_error");
        }
        waiting_for_list_response = false;
        refresh_request_pending = false;
        refresh_request_needs_send = false;
        refresh_input_wake_sent = false;
        refresh_request_retry_count = 0;
        refresh_grace_extended = false;
        lcd_refresh_inflight = false;
        lcd_refresh_ack_seen = false;
        lcd_refresh_retry_count = 0;
        resetActivityTimer();
      } else if (strcmp(text, "IDLE") == 0) {
        if (lcd_refresh_inflight && !lcd_refresh_ack_seen) {
          // Still waiting for refresh ack or list; do not clear.
        } else if (!refresh_request_pending) {
          waiting_for_list_response = false;
          refresh_input_wake_sent = false;
          refresh_request_retry_count = 0;
          refresh_grace_extended = false;
        }
        Serial.printf("[UI_STATUS] text=\"%s\" action=overlay_off screen_unchanged=1\n",
                      text ? text : "");
        if (app_event_queue != NULL) {
          app_event_t evt = {EVT_UI_STATUS_IDLE, {0}};
          xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
        }
      } else {
        waiting_for_list_response = true;
        resetActivityTimer();
        set_status_reset_visible(false);
      }
    }
    if (text && strstr(text, "Refreshing") != NULL) {
      if (app_event_queue != NULL) {
        app_event_t evt = {EVT_START_GLOWING, {0}};
        strncpy(evt.data.glow_reason, "ui_status_refresh", sizeof(evt.data.glow_reason) - 1);
        evt.data.glow_reason[sizeof(evt.data.glow_reason) - 1] = '\0';
        xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
      }
      Serial.printf("[UI_STATUS] text=\"%s\" action=overlay_on screen_unchanged=1\n",
                    text ? text : "");
      lcd_refresh_ack_seen = true;
      Serial.println("[REFRESH] ack_seen (UI_STATUS Refreshing)");
    }
    
    // Track voice operation state to prevent sleep during voice processing
    if (strcmp(op, "VOICE") == 0) {
      if (strcmp(phase, "RECORDING") == 0 || strcmp(phase, "UPLOADING") == 0 || strcmp(phase, "PROCESSING") == 0) {
        waiting_for_voice_response = true;
        Serial.println("[UART] Voice operation in progress - extending sleep timeout to 30s");
      } else if (strcmp(phase, "DONE") == 0 || strcmp(phase, "ERROR") == 0) {
        waiting_for_voice_response = false;
        Serial.println("[UART] Voice operation complete - restoring normal sleep timeout");
        if (app_event_queue != NULL) {
          app_event_t evt = {EVT_STOP_GLOWING, {0}};
          xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
        }
      }
    }
    
    // Track SCAN operation state to prevent sleep during scan processing
    // NOTE: The SCAN block below still contains LVGL calls (status_screen, expiry_screen, logged_screen, lv_timer_handler).
    // This is a known violation: uart_task must not call LVGL. Refactor: post EVT_UI_STATUS_SCAN with (op,phase,mode,text)
    // and have the UI task apply the same UI updates.
    if (strcmp(op, "SCAN") == 0) {
      // Get mode from message (if present) - "dish" or "discard"
      const char* mode = doc["mode"] | "";
      
      if (strcmp(phase, "CAPTURING") == 0 || strcmp(phase, "PREPARING") == 0 || 
          strcmp(phase, "UPLOADING") == 0 || strcmp(phase, "PROCESSING") == 0) {
        // Set flag on first status update (don't log every time)
        if (!waiting_for_scan_response) {
          waiting_for_scan_response = true;
          Serial.printf("[UART] SCAN operation in progress (mode: %s) - extending sleep timeout to 90s\n", mode);
        }
        // Reset activity timer on each status update to keep screen awake
        resetActivityTimer();
        
        // Check if this is a PREPARING phase with "Waiting for expiry date…" for check-in mode
        // This must be checked BEFORE the UPLOADING check
        if (strcmp(mode, "check-in") == 0 && strcmp(phase, "PREPARING") == 0 && 
            strstr(text, "Waiting for expiry date") != NULL) {
          // For check-in mode, show expiration date entry screen when waiting for expiry date
          if (status_screen != NULL) {
            lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
          }
          if (expiry_screen != NULL) {
            // Reset expiration date entry state
            strcpy(expiry_date_buffer, "__-__-____");
            expiry_date_pos = 0;
            expiry_screen_visible = true;
            expiry_screen_shown_time = millis();  // Track when screen was shown
            if (expiry_date_label != NULL) {
              lv_label_set_text(expiry_date_label, "MM-DD-YYYY");
            }
            lv_obj_clear_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
            Serial.println("[STATUS] Showing expiration date entry screen for check-in mode (PREPARING phase)");
            lv_timer_handler();  // Force immediate render
          }
        }
        // Update status screen text when UPLOADING phase is reached
        else if (strcmp(phase, "UPLOADING") == 0 && status_screen != NULL && status_label != NULL) {
          if (!lv_obj_has_flag(status_screen, LV_OBJ_FLAG_HIDDEN)) {
            if (strcmp(mode, "discard") == 0) {
              // For discard mode, hide status screen and show "Logged!" screen
              lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              if (logged_screen != NULL && logged_label != NULL) {
                lv_obj_clear_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
                logged_screen_shown_time = millis();
                Serial.println("[STATUS] Showing 'Logged!' screen for discard mode");
                lv_timer_handler();  // Force immediate render
              }
            } else if (strcmp(mode, "check-in") == 0 && strcmp(phase, "UPLOADING") == 0) {
              // For check-in mode, hide status screen and show expiration date entry screen (if not already shown)
              if (!expiry_screen_visible) {
                lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
                if (expiry_screen != NULL) {
                  // Reset expiration date entry state
                  strcpy(expiry_date_buffer, "__-__-____");
                  expiry_date_pos = 0;
                  expiry_screen_visible = true;
                  expiry_screen_shown_time = millis();  // Track when screen was shown
                  if (expiry_date_label != NULL) {
                    lv_label_set_text(expiry_date_label, "MM-DD-YYYY");
                  }
                  lv_obj_clear_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
                  Serial.println("[STATUS] Showing expiration date entry screen for check-in mode");
                  lv_timer_handler();  // Force immediate render
                }
              }
            } else {
              // For dish mode (or if mode not specified), show Frame_439_1
              status_screen_use_image(&ui_img_Frame_439_1_png);
              Serial.println("[STATUS] Showing Frame_439_1 for dish scan");
              ui_lvgl_tick();  // Force immediate render
            }
          }
        }
      } else if (strcmp(phase, "DONE") == 0 || strcmp(phase, "ERROR") == 0) {
        waiting_for_scan_response = false;
        Serial.printf("[UART] SCAN operation complete (mode: %s) - restoring normal sleep timeout\n", mode);
        // Reset activity timer so user can see the result before sleep
        resetActivityTimer();
      }
    }
    
    // TODO: Display status banner on LCD (e.g., "Listening...", "Transcribing...", "Capturing...")
    // Post event to UI task to show status overlay
  } else if (strcmp(type, "UI_VOICE_ITEMS") == 0) {
    // Voice items extracted from voice API - add immediately to top of list for instant feedback
    Serial.println("[UART] Received UI_VOICE_ITEMS - adding items optimistically to top of list");
    if (doc.containsKey("items") && doc["items"].is<JsonArray>()) {
      JsonArray items = doc["items"];
      int voice_item_count = items.size();
      Serial.printf("[UART] Voice extracted %d items - adding to top of list\n", voice_item_count);
      
      if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // Shift existing items down to make room at the top
        int shift_count = (voice_item_count < MAX_LIST_ITEMS - g_active.count) ? voice_item_count : (MAX_LIST_ITEMS - g_active.count);
        int items_to_add = shift_count;
        
        if (items_to_add > 0 && g_active.count + items_to_add <= MAX_LIST_ITEMS) {
          // Shift existing items down
          for (int i = g_active.count - 1; i >= 0; i--) {
            if (i + items_to_add < MAX_LIST_ITEMS) {
              strncpy(g_active.items[i + items_to_add], g_active.items[i], 63);
              g_active.items[i + items_to_add][63] = '\0';
              strncpy(g_active.item_ids[i + items_to_add], g_active.item_ids[i], 63);
              g_active.item_ids[i + items_to_add][63] = '\0';
            }
          }
          
          // Add new items at the top (index 0 to items_to_add-1)
          int added = 0;
          for (JsonObject item : items) {
            if (added >= items_to_add) break;
            
            const char* text = item["text"] | "";
            if (text != NULL && strlen(text) > 0) {
              strncpy(g_active.items[added], text, 63);
              g_active.items[added][63] = '\0';
              // Temporary ID (empty or placeholder) - will be replaced when full list refresh comes
              g_active.item_ids[added][0] = '\0';
              added++;
              Serial.printf("[UART] Added voice item to top: %s\n", text);
            }
          }
          
          g_active.count += added;
          // Set selected index to first new item (top of list)
          g_active.selected_index = 0;
          
          Serial.printf("[UART] Optimistically added %d items to top of list (total: %d)\n", added, g_active.count);
          
          xSemaphoreGive(app_state_mutex);
          
          // Post event to UI task to refresh display immediately (without swapping)
          if (app_event_queue != NULL) {
            app_event_t evt = {EVT_VOICE_ITEMS_ADDED, {.new_count = g_active.count}};
            xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(10));
            Serial.println("[UART] Sent EVT_VOICE_ITEMS_ADDED event for optimistic voice items");
          }
        } else {
          Serial.printf("[UART] Cannot add voice items - list full or would exceed MAX_LIST_ITEMS\n");
          xSemaphoreGive(app_state_mutex);
        }
      } else {
        Serial.println("[UART] Failed to acquire mutex for optimistic voice items");
      }
    }
  } else if (strcmp(type, "UI_MEAL_RESULT") == 0) {
    // Nutrition result from scan
    int calories = doc["calories"] | 0;
    float protein_g = doc["protein_g"] | 0.0f;
    float carbs_g = doc["carbs_g"] | 0.0f;
    float fat_g = doc["fat_g"] | 0.0f;
    float confidence = doc["confidence"] | 0.0f;
    const char* meal_summary = doc["meal_summary"] | "";
    const char* recommendation = doc["recommendation"] | "";
    const char* mode = doc["mode"] | "";  // Get mode from message
    
    Serial.printf("[UART] Meal result: %d cal, %.1fg protein, %.1fg carbs, %.1fg fat (confidence: %.2f, mode: %s)\n",
                  calories, protein_g, carbs_g, fat_g, confidence, mode);
    
    // For discard mode, don't show meal result - just ignore it
    if (strcmp(mode, "discard") == 0) {
      Serial.println("[UART] Discard mode - ignoring meal result, not displaying meal nutrition");
      // Ensure meal result screen is hidden
      if (meal_result_screen != NULL) {
        lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
      }
      // Show list again (in case it was hidden)
      if (list_container != NULL) {
        lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
      }
      return;  // Don't process meal result - exit early
    }
    
    // Hide status screen - meal result is coming (for dish mode)
    if (status_screen != NULL) {
      lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
      Serial.println("[STATUS] Hiding status screen - meal result received");
    }
    
    // Reset activity timer so screen doesn't go to sleep immediately after showing result
    resetActivityTimer();
    
    // Post event to UI task to show meal result screen
    if (app_event_queue != NULL) {
      app_event_t evt = {};
      evt.type = EVT_SHOW_MEAL_RESULT;
      evt.data.meal_result.calories = calories;
      evt.data.meal_result.protein_g = protein_g;
      evt.data.meal_result.carbs_g = carbs_g;
      evt.data.meal_result.fat_g = fat_g;
      strncpy(evt.data.meal_result.meal_summary, meal_summary, sizeof(evt.data.meal_result.meal_summary) - 1);
      evt.data.meal_result.meal_summary[sizeof(evt.data.meal_result.meal_summary) - 1] = '\0';
      strncpy(evt.data.meal_result.recommendation, recommendation, sizeof(evt.data.meal_result.recommendation) - 1);
      evt.data.meal_result.recommendation[sizeof(evt.data.meal_result.recommendation) - 1] = '\0';
      
      if (xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.println("[UART] Posted EVT_SHOW_MEAL_RESULT event to UI task");
      }
    }
  } else {
    Serial.printf("[PROTO] Unknown type: %s\n", type);
  }
}

// ── UI Task ────────────────────────────────────────────────────────
// ONLY the UI task (and loop() when it runs LVGL) may call LVGL; both hold example_lvgl_lock.
static void ui_task(void *arg) {
  Serial.println("[UI] UI task started");
  
  for (;;) {
    if (!example_lvgl_lock(50)) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    app_event_t evt;
    bool processed_anything = false;

    ui_loop_counter++;
    uint32_t submit_ok = 0, submit_fail = 0;
    int outstanding = 0, soft_fault = 0;
    lcd_bsp_get_flush_submit_stats(&submit_ok, &submit_fail, &outstanding, &soft_fault);

    if (soft_fault) {
      lcd_bsp_clear_flush_soft_fault();
      if (!display_busy_overlay) {
        display_busy_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(display_busy_overlay, 120, 40);
        lv_obj_center(display_busy_overlay);
        lv_obj_t *lbl = lv_label_create(display_busy_overlay);
        lv_label_set_text(lbl, "Display busy");
        lv_obj_center(lbl);
        lv_obj_set_style_bg_opa(display_busy_overlay, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(display_busy_overlay, lv_color_hex(0x404040), 0);
      }
      lv_obj_clear_flag(display_busy_overlay, LV_OBJ_FLAG_HIDDEN);
      display_busy_hide_at_ms = millis() + DISPLAY_BUSY_OVERLAY_MS;
    }
    if (display_busy_hide_at_ms && millis() >= display_busy_hide_at_ms) {
      if (display_busy_overlay) lv_obj_add_flag(display_busy_overlay, LV_OBJ_FLAG_HIDDEN);
      display_busy_hide_at_ms = 0;
    }

    {
      unsigned long now_hb = millis();
      if ((now_hb - last_heartbeat_ms) >= UI_HEARTBEAT_INTERVAL_MS) {
        last_heartbeat_ms = now_hb;
        size_t heap_internal = esp_get_free_heap_size();
        size_t heap_min = esp_get_minimum_free_heap_size();
        size_t heap_psram = 0;
#if (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM)
        heap_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
        Serial.printf("[UI_HB] heap=%u min=%u psram=%u hwm=%u loop=%lu tick_ms=%lu input_ms=%lu flush_ok=%lu fail=%lu out=%d soft_fault=%d\n",
                      (unsigned)heap_internal,
                      (unsigned)heap_min,
                      (unsigned)heap_psram,
                      (unsigned)uxTaskGetStackHighWaterMark(NULL),
                      (unsigned long)ui_loop_counter,
                      (unsigned long)last_ui_tick_ms,
                      (unsigned long)last_touch_or_input_ms,
                      (unsigned long)submit_ok,
                      (unsigned long)submit_fail,
                      outstanding,
                      soft_fault);
      }
    }

#if SHIP_MENU_UI
    if (app_event_queue != NULL) {
      while (xQueueReceive(app_event_queue, &evt, 0) == pdTRUE) {
        if (evt.type == EVT_SHIP_UI_STATUS) {
          ui_apply_ship_ui_status();
        } else if (evt.type == EVT_HAPTIC_TICK) {
          haptic_pulse();
        }
      }
    }
    debug_screen_update();
    result_retry_tick();
    wifi_on_run_deferred_if_ready("ui_tick");
    if (g_status_hide_at_ms && millis() >= g_status_hide_at_ms) {
      status_overlay_hide();
    }
    ui_lvgl_tick();
    example_lvgl_unlock();
    vTaskDelay(pdMS_TO_TICKS(5));
    continue;
#endif

    /* Deferred scroll redraw: avoid flooding SPI when we throttled the last scroll */
    if (scroll_pending_redraw && (millis() - last_scroll_refresh_ms >= SCROLL_REFRESH_MIN_MS)) {
      ui_refresh_from_state(&g_active);
      ui_lvgl_tick();
      last_scroll_refresh_ms = millis();
      scroll_pending_redraw = false;
    }
    /* Deferred glowing animation: run on next iteration so list redraw + one lv_timer_handler can drain before adding animation (avoids panel_io_spi_tx_color queue failed) */
    if (defer_glowing_after_refresh) {
      defer_glowing_after_refresh = false;
      start_glowing_animation(defer_glowing_reason[0] ? defer_glowing_reason : "list_refresh");
      defer_glowing_reason[0] = '\0';
      ui_lvgl_tick();
    }
    
    // CRITICAL: Process scroll events ONE AT A TIME for maximum responsiveness
    // Check for scroll events with 0 timeout (non-blocking, immediate)
    if (app_event_queue != NULL && 
        xQueueReceive(app_event_queue, &evt, 0) == pdTRUE) {
      
      if (evt.type == EVT_SCROLL_DELTA) {
        user_activity_bump("scroll");
        ensure_awake_for_ui("scroll_evt");
        Serial.printf("[UI] scroll_evt delta=%d\n", evt.data.scroll_delta);
        // Ignore scrolls during wake-up period
        if (millis() >= scroll_ignore_until) {
          // Check if expiry screen is visible - exit it on scroll
          if (expiry_screen_visible && expiry_screen != NULL && !lv_obj_has_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN)) {
            Serial.println("[EXPIRY] Scroll detected - exiting expiry date entry (no date provided)");
            
            // Send empty expiry date to Sense board to indicate user skipped it
            StaticJsonDocument<256> doc;
            doc["ver"] = PROTOCOL_VERSION;
            doc["type"] = "INPUT_EXPIRY_DATE";
            doc["msg_id"] = lcd_msg_id_counter++;
            doc["ts"] = millis();
            doc["expiry_date"] = "";  // Empty string indicates no expiry date
            
            String output;
            serializeJson(doc, output);
            request_sense_wake("expiry_skip");
            uart_send_json(output.c_str());
            Serial.printf("[EXPIRY] Sent empty expiration date to Sense (user skipped)\n");
            
            // Hide expiry screen and show list
            lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
            expiry_screen_visible = false;
            expiry_screen_shown_time = 0;
            expiry_date_pos = 0;
            strcpy(expiry_date_buffer, "__-__-____");
            if (expiry_date_label != NULL) {
              lv_label_set_text(expiry_date_label, "MM-DD-YYYY");
            }
            
            // Show list again
            if (list_container != NULL && g_active.count > 0) {
              lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
            }
            lv_timer_handler();
            resetActivityTimer();
            example_lvgl_unlock();
            continue;  // Skip normal scroll handling
          }
          
          // Check if menu screen is visible - handle menu scrolling
          if (menu_screen_visible) {
            // Menu scrolling
            int old_index = menu_selected_index;
            menu_selected_index += evt.data.scroll_delta;
            
            // Clamp to valid range
            if (menu_selected_index < 0) {
              menu_selected_index = 0;
            }
            if (menu_selected_index >= menu_item_count) {
              menu_selected_index = menu_item_count - 1;
            }
            
            // Update menu display if selection changed
            if (menu_selected_index != old_index) {
              update_menu_display();
              lv_timer_handler();  // Force immediate render
              resetActivityTimer();  // Reset activity timer on scroll
            }
          } else {
            // Normal list scrolling
            bool should_refresh = false;
            if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              // Apply this single scroll event (returns true if pull-to-refresh triggered)
              should_refresh = apply_event_to_state(&g_active, &evt);
              xSemaphoreGive(app_state_mutex);
              
              // Hide buttons on scroll (user is scrolling, not interacting with buttons)
              if (buttons_visible) {
                buttons_visible = false;
                if (delete_menu != NULL) {
                  lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
                }
                if (menu_menu != NULL) {
                  lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
                }
              }
            
            // Update UI: throttle redraws to avoid SPI queue overflow on fast scroll
            int sel_before = g_active.selected_index;
            unsigned long now_scroll = millis();
            if (now_scroll - last_scroll_refresh_ms >= SCROLL_REFRESH_MIN_MS) {
              ui_refresh_from_state(&g_active);
              ui_lvgl_tick();
              last_scroll_refresh_ms = now_scroll;
              scroll_pending_redraw = false;
            } else {
              scroll_pending_redraw = true;
            }
            int sel_after = g_active.selected_index;
            if (sel_after != sel_before) {
              Serial.printf("[UI] scroll_apply sel_before=%d sel_after=%d\n", sel_before, sel_after);
            }
              
              // Reset activity timer on user interaction
              resetActivityTimer();
              
              // Hide meal result screen if visible (user is scrolling, wants to see list)
              if (meal_result_screen != NULL && !lv_obj_has_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN)) {
                Serial.println("[UI] Meal result screen visible - hiding and showing list (scroll)");
                lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
                meal_result_shown_time = 0;  // Reset timeout
                // Show list again
                if (list_container != NULL && g_active.count > 0) {
                  lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
                }
              }
              
              // LVGL tick already done in throttle block when we refreshed
              
              // Trigger refresh if pull-to-refresh threshold reached
              if (should_refresh) {
                user_activity_bump("pull_refresh");
                ensure_awake_for_ui("pull_refresh");
                Serial.println("[UI] Pull-to-refresh triggered - requesting list refresh");
                if (refresh_state == REFRESH_WAKE_PENDING ||
                    refresh_state == REFRESH_INFLIGHT) {
                  Serial.printf("[REFRESH] ignored duplicate trigger state=%s\n",
                                refresh_state_name(refresh_state));
                  if (refresh_state == REFRESH_INFLIGHT) {
                    refresh_requested_again = true;
                    refresh_last_ui_pol_ms = millis();
                  }
                } else {
                  refresh_sm_set_wake_pending("pull_refresh");
                  lcd_refresh_inflight = true;
                  lcd_refresh_ack_seen = false;
                  lcd_refresh_retry_count = 0;
                  lcd_refresh_sent_ms = lcd_refresh_start_ms;
                  waiting_for_list_response = true;
                  refresh_request_pending = true;
                  refresh_request_needs_send = false;
                  refresh_input_wake_sent = false;
                  refresh_request_retry_count = 0;
                  refresh_grace_extended = false;
                  refresh_request_start_ms = lcd_refresh_start_ms;
                  refresh_request_last_ms = refresh_request_start_ms;
                  unsigned long refresh_now_ms = millis();
                  unsigned long refresh_age_ms =
                      last_sense_any_rx_ms > 0 ? (refresh_now_ms - last_sense_any_rx_ms) : 0;
                  Serial.printf("[REFRESH] sense_recent=%d age_ms=%lu\n",
                                sense_recently_heard(1500) ? 1 : 0,
                                refresh_age_ms);
                }
                // Reset pull-to-refresh counter
                pull_to_refresh_counter = 0;
                // Defer glowing animation to next loop iteration to avoid SPI queue overflow (list redraw already done this frame)
                defer_glowing_after_refresh = true;
                strncpy(defer_glowing_reason, "list_refresh", sizeof(defer_glowing_reason) - 1);
                defer_glowing_reason[sizeof(defer_glowing_reason) - 1] = '\0';
              }
              
              processed_anything = true;
            }
          }
        }
        // Continue immediately to check for next scroll event (no delay)
        example_lvgl_unlock();
        continue;
        
      } else if (evt.type == EVT_VOICE_ITEMS_ADDED) {
        // Optimistic voice items added - refresh UI directly without swapping
        Serial.printf("[UI] Voice items added event - refreshing UI (count=%d)\n", evt.data.new_count);
        if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          // Refresh UI directly from g_active (no swap needed)
          ui_refresh_from_state(&g_active);
          xSemaphoreGive(app_state_mutex);
          
          // Force LVGL to render
          lv_timer_handler();
          
          processed_anything = true;
        }
        
      } else if (evt.type == EVT_RENDER_ACTIVE_LIST) {
        // Render active list (e.g., restore after wake)
        Serial.printf("[UI] Render active list event - refreshing UI (count=%d)\n", evt.data.new_count);
        if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          ui_refresh_from_state(&g_active);
          xSemaphoreGive(app_state_mutex);
          lv_timer_handler();
          processed_anything = true;
        }
        
      } else if (evt.type == EVT_STOP_GLOWING) {
        stop_glowing_animation();
        lv_timer_handler();
        processed_anything = true;
      } else if (evt.type == EVT_START_GLOWING) {
        if (!is_glowing_animation) {
          start_glowing_animation(evt.data.glow_reason[0] ? evt.data.glow_reason : "uart");
          ui_lvgl_tick();
        }
        processed_anything = true;
      } else if (evt.type == EVT_UI_STATUS_IDLE) {
        set_status_reset_visible(false);
        if (is_glowing_animation) {
          stop_glowing_animation();
          ui_lvgl_tick();
        }
        processed_anything = true;
      } else if (evt.type == EVT_LIST_REPLACED) {
        // List was replaced (from UART task) - stop glowing, then swap pending → active and render
        stop_glowing_animation();
        Serial.printf("[UI] List replaced event - swapping pending → active (count=%d)\n", evt.data.new_count);
        if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          if (pending_ready) {
            // Save previous selection
            int prev_sel = g_active.selected_index;
            bool preserve = user_has_scrolled;
            
            // Copy pending → active (struct copy)
            // Note: deleted items have already been filtered out in uart_process_received_message
            g_active = g_pending;
            
            // Clean up deleted_item_ids: remove IDs that are no longer in the list (backend processed them)
            // This prevents the deleted list from growing indefinitely
            if (deleted_item_count > 0) {
              int new_deleted_count = 0;
              for (int i = 0; i < deleted_item_count; i++) {
                bool still_exists = false;
                // Check if this deleted ID still exists in the new list
                for (int j = 0; j < g_active.count; j++) {
                  if (strcmp(deleted_item_ids[i], g_active.item_ids[j]) == 0) {
                    still_exists = true;
                    break;
                  }
                }
                // If item no longer exists in list, backend has processed the delete - remove from tracking
                if (!still_exists) {
                  Serial.printf("[UI] Deleted item ID %s no longer in list - removing from tracking\n", deleted_item_ids[i]);
                  // Don't copy to new_deleted_count - effectively removes it
                } else {
                  // Still exists (shouldn't happen if filtering worked, but keep it just in case)
                  if (new_deleted_count < i) {
                    strncpy(deleted_item_ids[new_deleted_count], deleted_item_ids[i], 63);
                    deleted_item_ids[new_deleted_count][63] = '\0';
                  }
                  new_deleted_count++;
                }
              }
              deleted_item_count = new_deleted_count;
              if (new_deleted_count < deleted_item_count) {
                Serial.printf("[UI] Cleaned up deleted items tracking: %d remaining\n", deleted_item_count);
              }
            }
            
            // Reset UI to clean state if we just woke up
            bool was_just_woke = just_woke_up;
            if (was_just_woke) {
              Serial.println("[UI] Just woke up - resetting UI to clean state");
              
              // Reset selected_index to 0 for clean start
              g_active.selected_index = (g_active.count > 0) ? 0 : -1;
              
              // Hide all screens and show only the list
              if (status_screen != NULL) {
                lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              }
              if (meal_result_screen != NULL) {
                lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
              }
              if (delete_menu != NULL) {
                lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
              }
              if (menu_menu != NULL) {
                lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
              }
              if (recording_indicator != NULL) {
                lv_obj_add_flag(recording_indicator, LV_OBJ_FLAG_HIDDEN);
              }
              if (loading_screen != NULL) {
                lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
              }
              
              // Ensure list container is visible
              if (list_container != NULL && g_active.count > 0) {
                lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              
              // Clear the wake-up flag
              just_woke_up = false;
              
              Serial.println("[UI] UI reset complete - showing clean list at index 0");
            }
            
            // Preserve user's scroll position if they've scrolled (only if not just woke up)
            if (preserve && !was_just_woke) {
              if (g_active.count <= 0) {
                g_active.selected_index = -1;
              } else {
                if (prev_sel < 0) prev_sel = 0;
                if (prev_sel >= g_active.count) prev_sel = g_active.count - 1;
                g_active.selected_index = prev_sel;
                Serial.printf("[UI] Preserved user scroll position: %d\n", g_active.selected_index);
              }
            } else {
              Serial.printf("[UI] Using Sense selected_index: %d\n", g_active.selected_index);
            }
            
            pending_ready = false;
            
            // Ensure list container is visible
            if (g_active.count > 0 && list_container != NULL) {
              lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
            }
            if (loading_screen != NULL) {
              lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
            }
            
            // Render from g_active (LVGL call ONLY here)
            ui_refresh_from_state(&g_active);
            
            // Save to storage immediately
            save_list_to_storage(&g_active);
            
            // Reset pull-to-refresh counter when list is refreshed
            pull_to_refresh_counter = 0;
            
            // CRITICAL: Reset activity timer appropriately
            // If we just woke up, always reset the timer (user just woke the device)
            // If it's a background update after wake (more than 3 seconds), also reset it
            // Only skip reset if it's a background update within 3 seconds of wake (to prevent sleep loop)
            unsigned long time_since_wake = millis() - last_wake_time;
            if (was_just_woke) {
              // Just woke up - always reset activity timer so device doesn't immediately sleep
              resetActivityTimer();
              Serial.println("[UI] Just woke up - resetting activity timer");
            } else if (!g_in_light_sleep && time_since_wake > 3000) {
              // Background update after wake period (3+ seconds) - safe to reset
              resetActivityTimer();
              Serial.println("[UI] Background list update (3+ seconds after wake) - resetting activity timer");
            } else if (time_since_wake <= 3000) {
              // Background update within 3 seconds of wake - DON'T reset timer to prevent sleep loop
              Serial.printf("[UI] Background list update (%lu ms after wake) - NOT resetting activity timer to prevent sleep loop\n", time_since_wake);
            } else {
              // Normal background update (not in sleep, more than 3 seconds after wake)
              resetActivityTimer();
            }
            
            Serial.printf("[UI] Swapped pending → active: %d items, selected_index=%d\n", 
                          g_active.count, g_active.selected_index);
          }
          xSemaphoreGive(app_state_mutex);
        }
        processed_anything = true;
      } else if (evt.type == EVT_SHOW_PROVISION_QR) {
        show_provisioning_screen(evt.data.provision.ssid,
                                 evt.data.provision.password,
                                 evt.data.provision.url);
        processed_anything = true;
      } else if (evt.type == EVT_HIDE_PROVISION_QR) {
        hide_provisioning_screen();
        processed_anything = true;
      } else if (evt.type == EVT_UPDATE_PROVISION_STATUS) {
        update_provision_status_label(evt.data.provision_status.state);
        processed_anything = true;
      } else if (evt.type == EVT_RESET_UI) {
        // Force UI back to list state after sleep
        if (status_screen != NULL) {
          lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
        }
        set_status_reset_visible(false);
        if (meal_result_screen != NULL) {
          lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
        }
        if (logged_screen != NULL) {
          lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
        }
        if (expiry_screen != NULL) {
          lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
        }
        expiry_screen_visible = false;
        expiry_screen_shown_time = 0;
        if (provision_screen != NULL) {
          lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
        }
        provision_screen_visible = false;
        if (menu_screen != NULL) {
          lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
        }
        menu_screen_visible = false;
        set_menu_mode(MENU_MODE_MAIN);
        menu_selected_index = 0;
        if (loading_screen != NULL) {
          lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
        }
        if (delete_menu != NULL) {
          lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
        }
        if (menu_menu != NULL) {
          lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
        }
        buttons_visible = false;

        if (list_container != NULL && g_active.count > 0) {
          lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
        }
        status_screen_shown_time = 0;
        meal_result_shown_time = 0;
        lv_timer_handler();
        processed_anything = true;
      } else if (evt.type == EVT_HAPTIC_TICK) {
        haptic_pulse_scroll();
        processed_anything = true;
      } else if (evt.type == EVT_MENU_SELECTED) {
        // Menu item selected - send to Sense board and handle accordingly
        int selected_index = evt.data.menu_index;
        if (selected_index >= 0 && selected_index < menu_item_count && menu_items_current[selected_index] != NULL) {
          const char* selected_item = menu_items_current[selected_index];
          Serial.printf("[MENU] Menu item selected: %s (index %d)\n", selected_item, selected_index);
          
          if (menu_mode == MENU_MODE_SETTINGS) {
            if (strcmp(selected_item, "Back") == 0) {
              set_menu_mode(MENU_MODE_MAIN);
              menu_selected_index = 0;
              update_menu_display();
              resetActivityTimer();
              processed_anything = true;
              example_lvgl_unlock();
              continue;
            } else if (strcmp(selected_item, "Reset Wi-Fi") == 0) {
              request_sense_wake("reset_wifi");
              StaticJsonDocument<128> doc;
              doc["ver"] = PROTOCOL_VERSION;
              doc["type"] = "INPUT_RESET_WIFI";
              doc["msg_id"] = get_next_msg_id();
              doc["ts"] = millis();
              String output;
              serializeJson(doc, output);
              senseSerial.println(output);
              Serial.printf("[MENU] Sent reset Wi-Fi request to Sense: %s\n", output.c_str());
              
              hide_menu_screen();
              if (status_screen != NULL && status_label != NULL) {
                status_screen_use_text("Resetting\nWi-Fi...");
                lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              }
              ui_lvgl_tick();
              resetActivityTimer();
              processed_anything = true;
              example_lvgl_unlock();
              continue;
            }
          }
          
          // Handle "Settings" selection - open settings menu
          if (strcmp(selected_item, "Settings") == 0) {
            set_menu_mode(MENU_MODE_SETTINGS);
            menu_selected_index = 0;
            update_menu_display();
            resetActivityTimer();
            processed_anything = true;
            example_lvgl_unlock();
            continue;
          }
          
          // Handle "Home" selection - just go back to shopping list
          if (strcmp(selected_item, "Home") == 0) {
            hide_menu_screen();
            // Show list again
            if (list_container != NULL && g_active.count > 0) {
              lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
            }
            resetActivityTimer();
            processed_anything = true;
            example_lvgl_unlock();
            continue;  // Don't send message to Sense for Home
          }
          
          // Handle "Dish" selection - show status screen immediately
          if (strcmp(selected_item, "Dish") == 0) {
            // Show "Hold still!" status screen immediately (same as old menu button behavior)
            if (status_screen != NULL && status_label != NULL) {
              // Hide menu and list
              hide_menu_screen();
              if (list_container != NULL) {
                lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              // Show status screen with "Hold still!"
              status_screen_use_image(&ui_img_Frame_439_2_png);
              lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              status_screen_shown_time = millis();
              Serial.println("[STATUS] Showing Frame_439_2 for Dish scan");
                ui_lvgl_tick();  // Force immediate render
            }
          }
          
          // Handle "Discard" selection - show status screen
          else if (strcmp(selected_item, "Discard") == 0) {
            // Show "Hold still!" status screen immediately
            if (status_screen != NULL && status_label != NULL) {
              // Hide menu and list
              hide_menu_screen();
              if (list_container != NULL) {
                lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              // Show status screen with "Hold still!"
              status_screen_use_image(&ui_img_Frame_439_2_png);
              lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              status_screen_shown_time = millis();
              Serial.println("[STATUS] Showing Frame_439_2 for Discard scan");
              lv_timer_handler();  // Force immediate render
            }
          }
          
          // Handle "Check-in" selection - show status screen (same UI as discard)
          else if (strcmp(selected_item, "Check-in") == 0) {
            // Show "Hold still!" status screen immediately
            if (status_screen != NULL && status_label != NULL) {
              // Hide menu and list
              hide_menu_screen();
              if (list_container != NULL) {
                lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              // Show status screen with "Hold still!"
              status_screen_use_image(&ui_img_Frame_439_2_png);
              lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              status_screen_shown_time = millis();
              Serial.println("[STATUS] Showing Frame_439_2 for Check-in scan");
              lv_timer_handler();  // Force immediate render
            }
          }
          
          // Send menu selection to Sense board (for Dish, Discard, and Check-in)
          request_sense_wake("menu_select");
          StaticJsonDocument<256> doc;
          doc["ver"] = PROTOCOL_VERSION;
          doc["type"] = "INPUT_MENU_SELECT";
          doc["msg_id"] = get_next_msg_id();
          doc["ts"] = millis();
          doc["menu_item"] = selected_item;
          doc["menu_index"] = selected_index;
          
          String output;
          serializeJson(doc, output);
          senseSerial.println(output);
          Serial.printf("[MENU] Sent menu selection to Sense: %s\n", output.c_str());
          
          // Hide menu screen (if not already hidden for Dish/Discard)
          if (strcmp(selected_item, "Dish") != 0 && strcmp(selected_item, "Discard") != 0) {
            hide_menu_screen();
          }
          
          // Reset activity timer
          resetActivityTimer();
        }
        processed_anything = true;
      } else if (evt.type == EVT_TOGGLE_BUTTONS) {
        // Toggle menu buttons visibility
        buttons_visible = !buttons_visible;
        
        if (buttons_visible) {
          // Show buttons
          if (delete_menu != NULL && g_active.count > 0) {
            lv_obj_clear_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
          }
          if (menu_menu != NULL) {
            lv_obj_clear_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
          }
          Serial.println("[UI] Showing menu buttons");
        } else {
          // Hide buttons
          if (delete_menu != NULL) {
            lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
          }
          if (menu_menu != NULL) {
            lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
          }
          Serial.println("[UI] Hiding menu buttons");
        }
        
        processed_anything = true;
      } else if (evt.type == EVT_SHOW_MEAL_RESULT) {
        // NOTE: Mode check is now done in uart_process_received_message before posting event
        // This is just a safety check - should not be needed
        
        // Show meal result screen
        Serial.printf("[UI] Showing meal result: %d cal, %.1fg protein, %.1fg carbs, %.1fg fat\n",
                      evt.data.meal_result.calories, evt.data.meal_result.protein_g,
                      evt.data.meal_result.carbs_g, evt.data.meal_result.fat_g);
        
        // Hide list, loading, and status screens
        if (list_container != NULL) {
          lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
        }
        if (loading_screen != NULL) {
          lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_screen != NULL) {
          lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
          status_screen_shown_time = 0;  // Reset timeout
        }
        
        // Show meal result screen
        if (meal_result_screen != NULL) {
          lv_obj_clear_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
          
          // Update calories (large text at top)
          if (meal_calories_label != NULL) {
            char cal_text[32];
            snprintf(cal_text, sizeof(cal_text), "%dcal", evt.data.meal_result.calories);
            lv_label_set_text(meal_calories_label, cal_text);
          }
          
          // Update meal description
          if (meal_description_label != NULL) {
            lv_label_set_text(meal_description_label, evt.data.meal_result.meal_summary);
          }
          
          // Update macros (whole numbers only, no decimal)
          // Update value labels only (name labels are static)
          if (meal_protein_value_label != NULL) {
            char protein_text[32];
            snprintf(protein_text, sizeof(protein_text), "%.0fg", evt.data.meal_result.protein_g);
            lv_label_set_text(meal_protein_value_label, protein_text);
          }
          if (meal_carbs_value_label != NULL) {
            char carbs_text[32];
            snprintf(carbs_text, sizeof(carbs_text), "%.0fg", evt.data.meal_result.carbs_g);
            lv_label_set_text(meal_carbs_value_label, carbs_text);
          }
          if (meal_fat_value_label != NULL) {
            char fat_text[32];
            snprintf(fat_text, sizeof(fat_text), "%.0fg", evt.data.meal_result.fat_g);
            lv_label_set_text(meal_fat_value_label, fat_text);
          }
          
          // Update recommendation (small text at bottom)
          if (meal_recommendation_label != NULL) {
            lv_label_set_text(meal_recommendation_label, evt.data.meal_result.recommendation);
          }
          
          // Force render
          ui_lvgl_tick();
          
          // Record when meal result was shown (for 10-second timeout)
          meal_result_shown_time = millis();
        }
        
        processed_anything = true;
      }
    }
    
    // Only call lv_timer_handler() if we didn't just process a scroll event
    if (!processed_anything) {
      ui_lvgl_tick();
      example_lvgl_unlock();
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      example_lvgl_unlock();
    }
    unsigned long now_ms = millis();
    if ((now_ms - last_ui_heartbeat_ms) >= 500) {
      ui_heartbeat_counter++;
      Serial.printf("[UI_HEARTBEAT] now=%lu sleep=%d transition=%d hb=%lu lvgl_calls=%lu backlight=%d panel_on=%d refresh_ok=%lu refresh_timeout=%lu refresh_retry=%lu\n",
                    now_ms,
                    g_in_light_sleep ? 1 : 0,
                    g_sleep_transition ? 1 : 0,
                    (unsigned long)ui_heartbeat_counter,
                    (unsigned long)lvgl_timer_calls,
                    g_backlight_duty,
                    g_panel_enabled ? 1 : 0,
                    (unsigned long)refresh_success_count,
                    (unsigned long)refresh_timeout_count,
                    (unsigned long)refresh_retry_count);
      last_ui_heartbeat_ms = now_ms;
    }
  }
}

// ── UART Task (Core 0) ─────────────────────────────────────────────
// Dedicated task for UART TX/RX - never blocks UI, never calls LVGL
static void uart_task(void *arg) {
  Serial.println("[UART] UART task started");
  
  for (;;) {
    const int UART_TX_MAX_PER_LOOP = 16;
    const int UART_RX_MAX_BYTES_PER_LOOP = 512;
    bool yielded_early = false;

    if (lcd_refresh_inflight && !lcd_refresh_ack_seen && lcd_refresh_sent_ms == 0) {
      if (refresh_state != REFRESH_INFLIGHT) {
        Serial.printf("[REFRESH_SM] skip INPUT_WAKE reason=state state=%s\n",
                      refresh_state_name(refresh_state));
      } else if (!refresh_wake_sent) {
        wake_sense_for_request("refresh_send");
        uart_send_input_message("INPUT_WAKE");
        refresh_request_needs_send = false;
        lcd_refresh_sent_ms = millis();
        refresh_wake_sent = true;
        refresh_last_wake_send_ms = lcd_refresh_sent_ms;
        Serial.println("[REFRESH] INPUT_WAKE sent (uart_task)");
      }
    }

    // 1) TX drain - send queued messages
    tx_msg_t tx_msg;
    int tx_count = 0;
    while (uart_tx_queue != NULL && xQueueReceive(uart_tx_queue, &tx_msg, 0) == pdTRUE) {
      if (tx_msg.has_delta) {
        uart_send_input_message(tx_msg.type, tx_msg.delta);
      } else if (tx_msg.has_id) {
        uart_send_input_message(tx_msg.type, 0, tx_msg.id);
      } else {
        uart_send_input_message(tx_msg.type);
      }
      tx_count++;
      if (tx_count >= UART_TX_MAX_PER_LOOP) {
        yielded_early = true;
        break;
      }
    }
    
    // 2) RX read and parse lines
    int rx_bytes = 0;
    while (senseSerial.available() > 0) {
      char c = senseSerial.read();
      if (UART_RX_DEBUG) {
        Serial.printf("[UART_RAW] rx_byte=0x%02X\n", (uint8_t)c);
      }
      
      if (c == '\n' || c == '\r') {
        if (uart_rx_line_pos > 0) {
          uart_rx_line_buffer[uart_rx_line_pos] = '\0';
          uart_process_received_message(uart_rx_line_buffer);
          uart_rx_line_pos = 0;
        }
      } else if (uart_rx_line_pos < MAX_LINE_LENGTH - 1) {
        uart_rx_line_buffer[uart_rx_line_pos++] = c;
      } else {
        // Line too long, flush buffer
        Serial.printf("[PROTO] Line overflow (max=%d) - flushing buffer\n", MAX_LINE_LENGTH);
        uart_rx_line_pos = 0;
      }
      rx_bytes++;
      if (rx_bytes >= UART_RX_MAX_BYTES_PER_LOOP) {
        yielded_early = true;
        break;
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(yielded_early ? 1 : 5));
  }
}

// ── Activity Timer ────────────────────────────────────────────────
static const unsigned long INACTIVITY_TIMEOUT_MS = 10000;  // 10 seconds

static void resetActivityTimer() {
  last_user_activity_ms = millis();
  last_sleep_skip_log_ms = 0;
  last_sleep_skip_reason[0] = '\0';
}

static void init_touch_once() {
  if (g_touch_initialized) {
    return;
  }
  Touch_Init();
  g_touch_initialized = true;
}

static void init_knob_once() {
  if (s_knob != NULL) {
    return;
  }
  Serial.println("Initializing encoder...");
  knob_config_t knob_cfg = {
    .gpio_encoder_a = EXAMPLE_ENCODER_ECA_PIN,
    .gpio_encoder_b = EXAMPLE_ENCODER_ECB_PIN,
  };
  s_knob = iot_knob_create(&knob_cfg);
  if (s_knob == NULL) {
    Serial.println("ERROR: Knob create failed");
  } else {
    iot_knob_register_cb(s_knob, KNOB_LEFT, knob_left_cb, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, knob_right_cb, NULL);
    Serial.println("Knob initialized");
  }
}

static void init_ui_stack(int saved_count) {
  if (g_ui_initialized) {
    return;
  }
  // Initialize LCD and LVGL
  Serial.println("Initializing LCD...");
  init_touch_once();
  haptic_init();
  lcd_lvgl_Init();
  g_lcd_initialized = true;
  
  // Initialize backlight PWM (required before using setUpdutySubdivide)
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  g_backlight_initialized = true;
  g_backlight_duty = 255;
  g_panel_enabled = true;
  
  // Rotate display 180 degrees
  lv_disp_t *disp = lv_disp_get_default();
  lv_disp_set_rotation(disp, LV_DISP_ROT_180);
  
  // Create UI
  Serial.println("Creating UI...");
  create_custom_ui();
  
  // If we have a saved list, render it immediately (before background refresh)
  // Note: This is safe to call here because UI task hasn't started yet
  if (saved_count > 0 && list_container != NULL) {
    Serial.println("Rendering saved list immediately...");
    // Show list container
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
    if (loading_screen != NULL) {
      lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
    }
    // Refresh UI to show saved list (LVGL call - safe here before tasks start)
    // Safety check: ensure g_active is valid before calling
    if (g_active.count > 0 && g_active.count <= MAX_LIST_ITEMS) {
      ui_refresh_from_state(&g_active);
      // Force LVGL to render immediately (before UI task starts)
      for (int i = 0; i < 5; i++) {
        lv_timer_handler();
        delay(10);
      }
      Serial.println("Saved list rendered - UI ready");
    } else {
      Serial.printf("WARNING: Invalid g_active state (count=%d) - skipping render\n", g_active.count);
    }
  }

#if SHIP_MENU_UI
  show_ship_main_menu();
  status_overlay_init();
#endif
  
  // Initialize encoder (knob)
  init_knob_once();
  
  // Create UI task (Core 1 - owns LVGL)
  xTaskCreatePinnedToCore(ui_task, "ui_task", 12288, NULL, 3, &ui_task_handle, 1);
  
  // Small delay to ensure UART is stable
  delay(200);
  
  // Initialize activity timer BEFORE sending wake (so it doesn't go to sleep immediately)
  resetActivityTimer();
  
  // NOTE: We do NOT automatically request list refresh on boot anymore
  // User must manually trigger refresh via pull-to-refresh (scroll 5 ticks counter-clockwise beyond first item)
  
  // Reset activity timer (to prevent immediate sleep)
  resetActivityTimer();
  
  g_lvgl_running = true;
  g_ui_initialized = true;
  Serial.println("✓ Setup complete - device ready!");
}

static void enter_ship_ota_sleep() {
  g_sleep_transition = true;
  if (g_backlight_initialized) {
    setUpdutySubdivide(0);
  }
  if (g_lcd_initialized) {
    lcd_panel_set_power(false);
  }
  g_backlight_duty = 0;
  g_panel_enabled = false;
  delay(50);
  
  lcd_wake_pin_set_mode(LCD_WAKE_GPIO, INPUT_PULLUP);
  int wake_pin_level = digitalRead(LCD_WAKE_GPIO);
  Serial.printf("[LCD_SLEEP_CFG] ext0_gpio=%d ext0_level=%d pin_level_now=%d\n",
                (int)LCD_WAKE_GPIO, (int)LCD_WAKE_LEVEL, wake_pin_level);
  Serial.printf("[SLEEP_SANITY] wake_pin_level=%d wake_level=%d ext0_gpio=%d\n",
                wake_pin_level, LCD_WAKE_LEVEL, (int)LCD_WAKE_GPIO);
  if (wake_pin_level == LCD_WAKE_LEVEL) {
    Serial.println("[SLEEP_SANITY] wake pin already at wake level; refusing_sleep");
    delay(250);
    return;
  }
  configure_sleep_sources(true, LCD_OTA_WAKE_INTERVAL_SEC);
  
  Serial.printf("[SLEEP_STATE] entering_deep_sleep now_ms=%lu ext0_gpio=%d ext0_level=%d\n",
                (unsigned long)millis(),
                (int)LCD_WAKE_GPIO,
                (int)LCD_WAKE_LEVEL);
  Serial.println("[SLEEP] entering_deep_sleep");
  sleep_entry_time = millis();
  esp_deep_sleep_start();
}

static bool in_cold_boot_grace() {
  return (millis() - boot_ms) < COLD_BOOT_GRACE_MS;
}

// ── Persistent Storage ─────────────────────────────────────────────
static Preferences preferences;
static const char* PREF_NAMESPACE = "shopping_list";
static const char* PREF_KEY_COUNT = "count";
static const char* PREF_KEY_SELECTED = "selected";
static const char* PREF_KEY_ITEMS = "items";  // JSON string of items
static const char* PREF_KEY_IDS = "ids";      // JSON string of item IDs

// Save shopping list to persistent storage
static void save_list_to_storage(const app_state_t *s) {
  if (s == NULL) {
    Serial.println("✗ Cannot save list: app_state_t is NULL");
    return;
  }
  
  if (!preferences.begin(PREF_NAMESPACE, false)) {
    Serial.println("✗ Failed to open preferences for saving");
    return;
  }
  
  // Create JSON document to store items and IDs
  StaticJsonDocument<8192> doc;  // 8KB should be enough
  JsonArray items_array = doc.createNestedArray("items");
  JsonArray ids_array = doc.createNestedArray("ids");
  
  for (int i = 0; i < s->count && i < MAX_LIST_ITEMS; i++) {
    items_array.add(s->items[i]);
    ids_array.add(s->item_ids[i]);
  }
  
  // Serialize to string
  String json_str;
  serializeJson(doc, json_str);
  
  // Save count, selected index, and JSON string
  bool count_saved = preferences.putInt(PREF_KEY_COUNT, s->count);
  bool selected_saved = preferences.putInt(PREF_KEY_SELECTED, s->selected_index);
  bool items_saved = preferences.putString(PREF_KEY_ITEMS, json_str.c_str());
  
  preferences.end();
  
  if (count_saved && selected_saved && items_saved) {
    Serial.printf("✓ Saved %d items (selected_index=%d) to persistent storage\n", s->count, s->selected_index);
  } else {
    Serial.printf("✗ Failed to save list! count=%d, selected=%d, items=%d\n", 
                  count_saved, selected_saved, items_saved);
  }
}

// Load shopping list from persistent storage
static int load_list_from_storage(app_state_t *s) {
  if (!preferences.begin(PREF_NAMESPACE, true)) {  // Read-only mode
    Serial.println("✗ Failed to open preferences for loading");
    return -1;
  }
  
  int count = preferences.getInt(PREF_KEY_COUNT, -1);
  if (count < 0 || count > MAX_LIST_ITEMS) {
    preferences.end();
    Serial.println("✗ No valid saved list found");
    return -1;
  }
  
  int selected = preferences.getInt(PREF_KEY_SELECTED, 0);
  String json_str = preferences.getString(PREF_KEY_ITEMS, "");
  preferences.end();
  
  if (json_str.length() == 0) {
    Serial.println("✗ Saved list JSON is empty");
    return -1;
  }
  
  // Parse JSON (reduced size to prevent stack overflow)
  // Note: 4096 should be enough for most shopping lists (typically < 50 items)
  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, json_str);
  
  if (error) {
    Serial.printf("✗ Failed to parse saved list JSON: %s\n", error.c_str());
    return -1;
  }
  
  // Extract arrays
  if (!doc.containsKey("items") || !doc.containsKey("ids")) {
    Serial.println("✗ Saved list JSON missing items or ids");
    return -1;
  }
  
  JsonArray items_array = doc["items"].as<JsonArray>();
  JsonArray ids_array = doc["ids"].as<JsonArray>();
  
  // Clear old items
  for (int i = 0; i < MAX_LIST_ITEMS; i++) {
    s->items[i][0] = '\0';
    s->item_ids[i][0] = '\0';
  }
  
  // Copy items from JSON
  int actual_count = 0;
  for (int i = 0; i < count && i < MAX_LIST_ITEMS && i < (int)items_array.size(); i++) {
    const char* item_text = items_array[i] | "";
    const char* item_id = ids_array[i] | "";
    
    if (item_text != NULL && strlen(item_text) > 0) {
      strncpy(s->items[actual_count], item_text, 63);
      s->items[actual_count][63] = '\0';
      strncpy(s->item_ids[actual_count], item_id, 63);
      s->item_ids[actual_count][63] = '\0';
      actual_count++;
    }
  }
  
  s->count = actual_count;
  // Restore selected index (clamp to valid range)
  if (selected >= 0 && selected < actual_count) {
    s->selected_index = selected;
  } else {
    s->selected_index = (actual_count > 0) ? 0 : -1;
  }
  
  Serial.printf("✓ Loaded %d items (selected_index=%d) from persistent storage\n", 
                actual_count, s->selected_index);
  return actual_count;
}

// ── Arduino lifecycle ──────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  boot_ms = millis();
  g_boot_count++;
  cold_boot_grace_logged = false;
  waiting_for_sense_cmds = true;
  waiting_for_sense_logged = false;
  
  Serial.println("LCD ESP32-S3: booting...");
  Serial.println("[BOOT] safe_mode_timeout_flush_disabled=1");
  esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  Serial.printf("[WAKE_CAUSE] cause=%d\n", (int)wake_cause);
  const char* wake_cause_label = "OTHER";
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT0) {
    wake_cause_label = "EXT0";
  } else if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    wake_cause_label = "EXT1";
  } else if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
    wake_cause_label = "TIMER";
  } else if (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    wake_cause_label = "COLD_BOOT";
  }
  lcd_wake_pin_set_mode(LCD_WAKE_GPIO, INPUT_PULLUP);
  log_wake_pin_boot_state("boot");
  warn_wake_pin_active_at_boot(WAKE_PIN_BOOT_WARN_MS);
  Serial.printf("[WAKE_LINE] board=%s wake_out_gpio=%d pulse_ms=%lu wake_in_gpio=%d ext0_level=%d\n",
                HALO_BOARD_NAME,
                (int)INT_PIN,
                (unsigned long)WAKE_PULSE_DURATION_MS,
                (int)LCD_WAKE_GPIO,
                (int)LCD_WAKE_LEVEL);
  Serial.println("[SLEEP_DIAG] sleep_kind=DEEP");
  Serial.printf("[SLEEP_DIAG] wake_cause=%s\n", wake_cause_label);
  Serial.printf("[WAKE_STATE] cause=%s boot_count=%lu uptime_ms=%lu\n",
                wake_cause_label,
                (unsigned long)g_boot_count,
                (unsigned long)millis());
  snprintf(s_wake_cause_label, sizeof(s_wake_cause_label), "%s", wake_cause_label);
  s_wake_cause_label[sizeof(s_wake_cause_label) - 1] = '\0';
  g_ship_ota_wake_window = (HALO_SHIP_TEST_MODE != 0) && LCD_SHIP_MODE_OTA &&
                           (wake_cause == ESP_SLEEP_WAKEUP_TIMER ||
                            wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED);
  if (g_ship_ota_wake_window) {
    Serial.println("[SHIP_OTA] wake_window_start");
    g_lvgl_running = false;
    g_panel_enabled = false;
    g_backlight_duty = 0;
  }
  link_sync_pending = true;
  link_synced = false;
  
  // Phase 0: Print protocol validation message
  Serial.println("LCD ESP32-S3: booted, PROTO OK v1.");
  
  // Initialize UART
  init_uart();
  
  // Initialize INT pin for waking Sense
  lcd_wake_pin_set_mode(INT_PIN, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)INT_PIN, GPIO_DRIVE_CAP_3);
  digitalWrite(INT_PIN, HIGH);
  delay(2);
  lcd_wake_pin_set_mode(INT_PIN, INPUT_PULLUP);
  
  // Initialize mutex
  app_state_mutex = xSemaphoreCreateMutex();
  if (app_state_mutex == NULL) {
    Serial.println("ERROR: Failed to create app_state_mutex!");
  }
  
  // Initialize event queue (larger size for rapid scroll events)
  app_event_queue = xQueueCreate(64, sizeof(app_event_t));
  if (app_event_queue == NULL) {
    Serial.println("ERROR: Failed to create app_event_queue!");
  }
  
  // Initialize UART TX queue
  uart_tx_queue = xQueueCreate(20, sizeof(tx_msg_t));
  if (uart_tx_queue == NULL) {
    Serial.println("ERROR: Failed to create uart_tx_queue!");
  }

  // Create UART task (Core 0 - handles TX/RX) early
  xTaskCreatePinnedToCore(uart_task, "uart_task", 8192, NULL, 2, NULL, 0);
  
  // Initialize app state (double-buffered)
  g_active.count = 0;
  g_active.selected_index = -1;
  g_pending.count = 0;
  g_pending.selected_index = -1;
  pending_ready = false;
  user_has_scrolled = false;
  pull_to_refresh_counter = 0;
  touch_pressed = false;
  touch_press_time = 0;
  long_press_sent = false;
  buttons_visible = false;
  
  // Load saved list from persistent storage BEFORE creating UI
  Serial.println("\n=== Loading Saved Shopping List ===");
  int saved_count = load_list_from_storage(&g_active);
  g_saved_list_count = saved_count;
  if (saved_count > 0) {
    Serial.printf("Loaded %d items, selected_index=%d\n", saved_count, g_active.selected_index);
  } else {
    Serial.println("No saved list found - will fetch from Sense");
  }

  if (!g_ship_ota_wake_window) {
    init_ui_stack(g_saved_list_count);
  } else {
    init_touch_once();
    init_knob_once();
  }

#ifdef HALO_LCD_PROD_WRAPPER
  halo_lcd_prod_setup();
#endif
}

void loop() {
  // safe mode: disable timeout/force-ready flush path (LVGL finish only via SPI done)
  if (lcd_bsp_display_reset_requested()) {
    Serial.println("[LCD_FLUSH] display reset requested (flush failures exceeded threshold); consider reinit or power cycle");
  }
  if (!example_lvgl_lock(50)) {
    vTaskDelay(pdMS_TO_TICKS(10));
    return;
  }
  // UART TX/RX is now handled by uart_task - nothing to do here
  
  // Poll touch input
  // getTouch returns: 1 = touch detected, 0 = no touch
  uint16_t touch_x = 0, touch_y = 0;
  uint8_t touch_result = getTouch(&touch_x, &touch_y);
  bool touch_detected = (touch_result == 1);  // 1 means touch detected!
  
  // Ignore touches during wake-up period
  if (millis() < touch_ignore_until) {
    touch_detected = false;
  }
  if (touch_detected) {
    last_touch_or_input_ms = millis();
  }

  bool skip_main_loop = false;
  if (g_ship_ota_wake_window && !g_ui_initialized) {
    if (touch_detected) {
      g_ship_ota_user_input = true;
    }
    if (g_ship_ota_user_input) {
      Serial.println("[SHIP_OTA] user_input -> normal_ui");
      g_ship_ota_wake_window = false;
      g_ship_ota_user_input = false;
      init_ui_stack(g_saved_list_count);
    } else if (!g_ship_ota_ota_started) {
      g_ship_ota_ota_started = true;
#ifdef HALO_LCD_PROD_WRAPPER
      halo_lcd_prod_run_ota_check_once();
#endif
      Serial.println("[SHIP_OTA] wake_window_end -> sleeping");
      example_lvgl_unlock();
      enter_ship_ota_sleep();
      return;
    } else {
      skip_main_loop = true;
    }
  }
  if (g_lcd_maintenance_active && !g_lcd_maintenance_started) {
    g_lcd_maintenance_started = true;
    if (g_lcd_maintenance_aborted) {
      lcd_finish_maintenance("aborted");
    } else {
#ifdef HALO_LCD_PROD_WRAPPER
      halo_lcd_prod_run_ota_check_once();
#else
      lcd_finish_maintenance("failed");
#endif
    }
  }
  if (skip_main_loop) {
    example_lvgl_unlock();
#ifdef HALO_LCD_PROD_WRAPPER
    halo_lcd_prod_loop();
#endif
    vTaskDelay(pdMS_TO_TICKS(50));  // Poll touch every 50ms (use vTaskDelay to yield to IDLE task)
    return;
  }
  
  unsigned long now = millis();
  refresh_sense_awake_estimate(now);
  
  // Simple state machine: detect press -> wait for release -> determine brief vs long
  if (touch_detected && !touch_pressed) {
    ensure_awake_for_ui("touch_press");
    // Touch just pressed - store coordinates
    touch_pressed = true;
    user_activity_bump("touch_press");
    touch_press_time = now;
    touch_press_x = touch_x;  // Store coordinates for later use
    touch_press_y = touch_y;  // Store coordinates for later use
    long_press_sent = false;
    Serial.printf("[TOUCH] Touch pressed at (%d, %d) - stored as (%d, %d)\n", touch_x, touch_y, touch_press_x, touch_press_y);
    haptic_pulse();
    resetActivityTimer();
    
    // Hide meal result screen if visible (user touched, wants to go back to list)
    if (meal_result_screen != NULL && !lv_obj_has_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN)) {
      Serial.println("[TOUCH] Meal result screen visible - hiding and showing list");
      lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
      meal_result_shown_time = 0;  // Reset timeout
      // Show list again
      if (list_container != NULL && g_active.count > 0) {
        lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
      }
      lv_timer_handler();
      // Reset activity timer since user is interacting
      resetActivityTimer();
      // Mark that this touch was used to dismiss meal result (prevent button toggle)
      touch_used_to_dismiss_meal = true;
    } else {
      // Touch not used to dismiss meal result
      touch_used_to_dismiss_meal = false;
    }
  } else if (!touch_detected && touch_pressed) {
    // Touch just released
    unsigned long press_duration = now - touch_press_time;
    bool was_long_press = long_press_sent;
    touch_pressed = false;
    
    #if SHIP_MENU_UI
    if (!was_long_press && press_duration < LONG_PRESS_THRESHOLD_MS) {
      uint16_t check_x = 359 - touch_press_x;
      uint16_t check_y = 359 - touch_press_y;
      const ship_menu_hitbox_t* hb = ship_menu_hit_test(check_x, check_y);
      if (hb) {
        ship_menu_send_action(hb);
        ui_lvgl_tick();
      }
    }
    resetActivityTimer();
    #else
    if (!was_long_press && press_duration < LONG_PRESS_THRESHOLD_MS) {
      // Brief touch - check if menu is visible first
      if (menu_screen_visible) {
        // Menu is visible - check cooldown to prevent selection from the touch that opened the menu
        if (millis() < menu_cooldown_until) {
          Serial.printf("[TOUCH] Brief touch ignored (menu cooldown active - menu just opened)\n");
        } else {
          // Menu is visible and cooldown expired - select the current menu item
          // Safety check: ensure index is valid
          if (menu_selected_index >= 0 && menu_selected_index < menu_item_count && menu_items_current[menu_selected_index] != NULL) {
            Serial.printf("[TOUCH] Brief touch detected (%lums) - selecting menu item: %s (index %d)\n", 
                          press_duration, menu_items_current[menu_selected_index], menu_selected_index);
            if (app_event_queue != NULL) {
              app_event_t menu_evt = {EVT_MENU_SELECTED, {.menu_index = menu_selected_index}};
              xQueueSend(app_event_queue, &menu_evt, pdMS_TO_TICKS(10));
            }
          } else {
            Serial.printf("[TOUCH] Invalid menu index: %d (max: %d)\n", menu_selected_index, menu_item_count - 1);
          }
        }
      } else if (status_screen != NULL && status_reset_visible && status_reset_button != NULL &&
                 !lv_obj_has_flag(status_screen, LV_OBJ_FLAG_HIDDEN)) {
        uint16_t check_x = 359 - touch_press_x;
        uint16_t check_y = 359 - touch_press_y;
        lv_area_t btn_area;
        lv_obj_get_coords(status_reset_button, &btn_area);
        if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
            check_y >= btn_area.y1 && check_y <= btn_area.y2) {
          Serial.println("[STATUS] Reset Wi-Fi button pressed");
          set_status_reset_visible(false);
          status_screen_use_text("Resetting\nWi-Fi...");
          lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
          status_screen_shown_time = millis();
          lv_timer_handler();
          tx_msg_t tx_msg = {};
          strncpy(tx_msg.type, "INPUT_RESET_WIFI", sizeof(tx_msg.type) - 1);
          if (uart_tx_queue != NULL) {
            xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
          }
        }
      } else if (expiry_screen_visible && expiry_screen != NULL && !lv_obj_has_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN)) {
        // Expiration date entry screen is visible - handle keypad touches
        // Use stored touch coordinates from when touch was pressed
        // Flip both X and Y coordinates (360px screen, 0-indexed: 0-359)
        uint16_t check_x = 359 - touch_press_x;  // Flip X coordinate horizontally
        uint16_t check_y = 359 - touch_press_y;  // Flip Y coordinate vertically
        Serial.printf("[EXPIRY] Checking buttons with stored coordinates (%d, %d) -> flipped to (%d, %d)\n", 
                     touch_press_x, touch_press_y, check_x, check_y);
        
        // Check if touch is on a number button or check button
        bool button_pressed = false;
        
        // Check number buttons (0-9)
        for (int i = 0; i < 10; i++) {
          if (expiry_keypad_buttons[i] != NULL) {
            lv_area_t btn_area;
            lv_obj_get_coords(expiry_keypad_buttons[i], &btn_area);
            if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
                check_y >= btn_area.y1 && check_y <= btn_area.y2) {
              // Number button pressed
              int digit = (int)(intptr_t)lv_obj_get_user_data(expiry_keypad_buttons[i]);
              Serial.printf("[EXPIRY] Number button %d pressed\n", digit);
              
              // Add digit to date string (format: MM-DD-YYYY)
              // Buffer positions: 0-1 (month), 2 (dash), 3-4 (day), 5 (dash), 6-9 (year)
              // We track logical position (0-7 for 8 digits) and map to buffer positions
              // Logical positions: 0-1 (month), 2-3 (day), 4-7 (year)
              if (expiry_date_pos < 8) {
                int pos_in_buffer;
                if (expiry_date_pos < 2) {
                  // Month digits: positions 0-1
                  pos_in_buffer = expiry_date_pos;
                } else if (expiry_date_pos < 4) {
                  // Day digits: positions 3-4 (skip dash at 2)
                  pos_in_buffer = expiry_date_pos + 1;
                } else {
                  // Year digits: positions 6-9 (skip dashes at 2 and 5)
                  pos_in_buffer = expiry_date_pos + 2;
                }
                
                expiry_date_buffer[pos_in_buffer] = '0' + digit;
                expiry_date_pos++;
                
                // Ensure buffer is null-terminated
                expiry_date_buffer[10] = '\0';
                
                // Update display
                if (expiry_date_label != NULL) {
                  lv_label_set_text(expiry_date_label, expiry_date_buffer);
                  Serial.printf("[EXPIRY] Label text set to: %s\n", expiry_date_buffer);
                  lv_timer_handler();  // Force immediate render
                }
                
                Serial.printf("[EXPIRY] Date updated: %s (pos: %d, buffer pos: %d)\n", 
                             expiry_date_buffer, expiry_date_pos, pos_in_buffer);
              }
              button_pressed = true;
              break;
            }
          }
        }
        
        // Check backspace button
        if (!button_pressed && expiry_backspace_button != NULL) {
          lv_area_t btn_area;
          lv_obj_get_coords(expiry_backspace_button, &btn_area);
          if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
              check_y >= btn_area.y1 && check_y <= btn_area.y2) {
            // Backspace button pressed - remove last digit
            if (expiry_date_pos > 0) {
              expiry_date_pos--;
              
              // Calculate buffer position for the digit to remove
              // Logical positions: 0-1 (month), 2-3 (day), 4-7 (year)
              int pos_in_buffer;
              if (expiry_date_pos < 2) {
                // Month digits: positions 0-1
                pos_in_buffer = expiry_date_pos;
              } else if (expiry_date_pos < 4) {
                // Day digits: positions 3-4 (skip dash at 2)
                pos_in_buffer = expiry_date_pos + 1;
              } else {
                // Year digits: positions 6-9 (skip dashes at 2 and 5)
                pos_in_buffer = expiry_date_pos + 2;
              }
              
              // Restore underscore at that position
              expiry_date_buffer[pos_in_buffer] = '_';
              expiry_date_buffer[10] = '\0';
              
              // Update display
              if (expiry_date_label != NULL) {
                lv_label_set_text(expiry_date_label, expiry_date_buffer);
                lv_timer_handler();
              }
              
              Serial.printf("[EXPIRY] Backspace - Date now: %s (pos: %d)\n", expiry_date_buffer, expiry_date_pos);
            }
            button_pressed = true;
          }
        }
        
        // Check check button
        if (!button_pressed && expiry_check_button != NULL) {
          lv_area_t btn_area;
          lv_obj_get_coords(expiry_check_button, &btn_area);
          Serial.printf("[EXPIRY] Check button area: x1=%d, y1=%d, x2=%d, y2=%d, touch=(%d, %d)\n", 
                       btn_area.x1, btn_area.y1, btn_area.x2, btn_area.y2, check_x, check_y);
          if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
              check_y >= btn_area.y1 && check_y <= btn_area.y2) {
            // Check button pressed - submit expiration date
            Serial.printf("[EXPIRY] Check button pressed - submitting date: %s (pos: %d)\n", expiry_date_buffer, expiry_date_pos);
            
            // If user presses OK with no digits, treat as "no expiration"
            if (expiry_date_pos == 0) {
              StaticJsonDocument<256> doc;
              doc["ver"] = PROTOCOL_VERSION;
              doc["type"] = "INPUT_EXPIRY_DATE";
              doc["msg_id"] = lcd_msg_id_counter++;
              doc["ts"] = millis();
              doc["expiry_date"] = "";  // Empty string indicates no expiry date
              
              String output;
              serializeJson(doc, output);
              request_sense_wake("expiry_submit_empty");
              uart_send_json(output.c_str());
              Serial.printf("[EXPIRY] Sent empty expiration date to Sense (OK with no date)\n");
              
              // Hide expiration date screen and show list
              lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
              expiry_screen_visible = false;
              expiry_screen_shown_time = 0;  // Reset timeout
              expiry_date_pos = 0;
              strcpy(expiry_date_buffer, "__-__-____");
              if (expiry_date_label != NULL) {
                lv_label_set_text(expiry_date_label, "MM-DD-YYYY");
              }
              
              // Show list again
              if (list_container != NULL && g_active.count > 0) {
                lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              lv_timer_handler();
              resetActivityTimer();
            // Validate date is complete (all 8 digits entered: 2 for month, 2 for day, 4 for year)
            // Date format: MM-DD-YYYY (8 digits total, excluding dashes)
            } else if (expiry_date_pos == 8) {
              // Verify the buffer doesn't have any underscores (all digits filled)
              bool has_underscores = false;
              for (int i = 0; i < 10; i++) {
                if (expiry_date_buffer[i] == '_') {
                  has_underscores = true;
                  break;
                }
              }
              
              if (!has_underscores) {
                // Convert MM-DD-YYYY to YYYY-MM-DD format for sending to Sense board
                char converted_date[11];
                convert_date_mmddyyyy_to_yyyymmdd(expiry_date_buffer, converted_date, sizeof(converted_date));
                
                // Send expiration date to Sense board
                StaticJsonDocument<256> doc;
                doc["ver"] = PROTOCOL_VERSION;
                doc["type"] = "INPUT_EXPIRY_DATE";
                doc["msg_id"] = lcd_msg_id_counter++;
                doc["ts"] = millis();
                doc["expiry_date"] = converted_date;  // Format: YYYY-MM-DD (converted from MM-DD-YYYY)
                
                String output;
                serializeJson(doc, output);
                request_sense_wake("expiry_submit");
                uart_send_json(output.c_str());
                Serial.printf("[EXPIRY] Sent expiration date to Sense: %s (converted from %s)\n", converted_date, expiry_date_buffer);
                
                // Hide expiration date screen and show list
                lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
                expiry_screen_visible = false;
                expiry_screen_shown_time = 0;  // Reset timeout
                expiry_date_pos = 0;
                strcpy(expiry_date_buffer, "__-__-____");
                if (expiry_date_label != NULL) {
                  lv_label_set_text(expiry_date_label, "MM-DD-YYYY");
                }
                
                // Show list again
                if (list_container != NULL && g_active.count > 0) {
                  lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
                }
                lv_timer_handler();
                resetActivityTimer();
              } else {
                Serial.printf("[EXPIRY] Date buffer still contains underscores - cannot submit\n");
              }
            } else {
              Serial.printf("[EXPIRY] Date incomplete (%d/8 digits) - cannot submit\n", expiry_date_pos);
            }
            button_pressed = true;
          }
        }
        
        if (!button_pressed) {
          Serial.printf("[EXPIRY] Touch detected but not on any button (%d, %d)\n", check_x, check_y);
        }
      } else {
        // Menu not visible - handle normal touch behavior
        // Brief touch - send event to UI task to toggle buttons (LVGL must be called from UI task)
        // BUT: Ignore if we're in delete cooldown period (prevents re-showing buttons after delete)
        // BUT: Also ignore if touch was used to dismiss meal result screen (prevents unwanted button toggle)
        if (millis() < delete_cooldown_until) {
          Serial.printf("[TOUCH] Brief touch ignored (delete cooldown active)\n");
        } else if (touch_used_to_dismiss_meal) {
          Serial.printf("[TOUCH] Brief touch ignored (used to dismiss meal result)\n");
          touch_used_to_dismiss_meal = false;  // Reset flag
        } else {
          Serial.printf("[TOUCH] Brief touch detected (%lums) - toggling menu buttons\n", press_duration);
          if (app_event_queue != NULL) {
            app_event_t toggle_evt = {EVT_TOGGLE_BUTTONS, {.new_count = 0}};
            xQueueSend(app_event_queue, &toggle_evt, pdMS_TO_TICKS(10));
          }
        }
      }
      
      // Also send INPUT_TOUCH to Sense board
      tx_msg_t tx_msg = {};
      strncpy(tx_msg.type, "INPUT_TOUCH", sizeof(tx_msg.type) - 1);
      if (uart_tx_queue != NULL) {
        xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
      }
    } else if (was_long_press) {
      // Long press was active - send END message
      Serial.printf("[TOUCH] Long press END detected (%lums) - sending INPUT_LONG_PRESS_END\n", press_duration);
      tx_msg_t tx_msg = {};
      strncpy(tx_msg.type, "INPUT_LONG_PRESS_END", sizeof(tx_msg.type) - 1);
      if (uart_tx_queue != NULL) {
        xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
      }
      
      // Hide solid halo when long press ends
      if (recording_indicator != NULL) {
        lv_obj_add_flag(recording_indicator, LV_OBJ_FLAG_HIDDEN);
        Serial.println("[TOUCH] Hiding solid halo - long press ended");
      }
      
      // Show "On it!" status screen for 1 second
      if (status_screen != NULL && status_label != NULL) {
        // Hide list and other screens
        if (list_container != NULL) {
          lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
        }
        if (meal_result_screen != NULL) {
          lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
        }
        // Show status screen with "On it!"
        status_screen_use_text("On it!");
        lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
        status_screen_shown_time = now;
        Serial.println("[STATUS] Showing 'On it!' status screen");
        lv_timer_handler();  // Force immediate render
      }
      
      // Reset long press flag
      long_press_sent = false;
    } else {
      // Edge case: released before long press threshold but after we started tracking
      Serial.printf("[TOUCH] Touch released after %lums (not long press)\n", press_duration);
      // Make sure halo is hidden (shouldn't be visible, but just in case)
      if (recording_indicator != NULL && !lv_obj_has_flag(recording_indicator, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(recording_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_timer_handler();
      }
      // Reset long press flag
      long_press_sent = false;
    }
    resetActivityTimer();
    #endif
  }
  #if !SHIP_MENU_UI
  else if (touch_pressed && !long_press_sent) {
    // Touch is still pressed - check for long press
    unsigned long press_duration = now - touch_press_time;
    
    if (press_duration >= LONG_PRESS_THRESHOLD_MS) {
      // Long press detected! Send START message
      long_press_sent = true;
      Serial.printf("[TOUCH] Long press START detected (%lums) - sending INPUT_LONG_PRESS_START\n", press_duration);
      request_sense_wake("voice_start");
      tx_msg_t tx_msg = {};
      strncpy(tx_msg.type, "INPUT_LONG_PRESS_START", sizeof(tx_msg.type) - 1);
      if (uart_tx_queue != NULL) {
        xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
      }
      
      // Show solid halo for long press
      if (recording_indicator != NULL) {
        lv_obj_clear_flag(recording_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_timer_handler();  // Force immediate render
        Serial.println("[TOUCH] Showing solid halo for long press");
      }
      
      resetActivityTimer();
    }
  }
  #endif
  if (!g_in_light_sleep && sense_status_sync_requested) {
    lcd_maybe_pulse_sense_int("status_sync");
  }
  
  // Check if "On it!" status screen has been showing for 1 second - hide it and show list with glowing halo
  if (status_screen != NULL && !lv_obj_has_flag(status_screen, LV_OBJ_FLAG_HIDDEN) && 
      status_screen_shown_time > 0) {
    unsigned long status_elapsed = now - status_screen_shown_time;
    // Check if it's the "On it!" message (1 second timeout)
    const char* current_text = lv_label_get_text(status_label);
    if (current_text != NULL && strcmp(current_text, "On it!") == 0 && 
        status_elapsed >= STATUS_SCREEN_TIMEOUT_MS) {
      Serial.println("[LOOP] 'On it!' status screen timeout (1s) - hiding and showing list with glowing halo");
      lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
      status_screen_shown_time = 0;  // Reset timeout
      
      // Show list again
      if (list_container != NULL && g_active.count > 0) {
        lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
      }
      
      // Start glowing animation (voice processing in progress)
      start_glowing_animation("voice_processing");
      
      lv_timer_handler();
      // Reset activity timer
      resetActivityTimer();
    }
  }

  // Hide API error status after a grace period
  if (api_error_shown_time > 0) {
    unsigned long error_elapsed = now - api_error_shown_time;
    if (error_elapsed >= API_ERROR_TIMEOUT_MS && !status_reset_visible) {
      if (status_screen != NULL) {
        lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
      }
      set_status_reset_visible(false);
      api_error_shown_time = 0;
      status_screen_shown_time = 0;
      if (list_container != NULL && g_active.count > 0) {
        lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
      }
      lv_timer_handler();
      resetActivityTimer();
    }
  }
  
  // Check if logged screen has been showing for 2 seconds - auto-hide it and return to list
  if (logged_screen != NULL && !lv_obj_has_flag(logged_screen, LV_OBJ_FLAG_HIDDEN) && 
      logged_screen_shown_time > 0 && (millis() - logged_screen_shown_time) > LOGGED_SCREEN_TIMEOUT_MS) {
    Serial.println("[LOOP] Logged screen timeout (2s) - hiding and showing list");
    lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
    logged_screen_shown_time = 0;  // Reset timeout
    // NOTE: Mode is now tracked per-message, not via global flag
    // Meal results for discard mode are filtered in uart_process_received_message
    // Show list again
    if (list_container != NULL && g_active.count > 0) {
      lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_handler();
    // Reset activity timer so user has time to see the list before sleep
    resetActivityTimer();
  }
  
  // Check if meal result screen has been showing for 30 seconds - auto-hide it
  if (meal_result_screen != NULL && !lv_obj_has_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN) && 
      meal_result_shown_time > 0 && (millis() - meal_result_shown_time) > MEAL_RESULT_TIMEOUT_MS) {
    Serial.println("[LOOP] Meal result screen timeout (30s) - hiding and showing list");
    lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
    meal_result_shown_time = 0;  // Reset timeout
    // Show list again
    if (list_container != NULL && g_active.count > 0) {
      lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_handler();
    // Reset activity timer so user has time to see the list before sleep
    resetActivityTimer();
  }
  
  // Check if expiry screen has been showing for 30 seconds - auto-hide it and send empty date
  if (expiry_screen_visible && expiry_screen != NULL && !lv_obj_has_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN) && 
      expiry_screen_shown_time > 0 && (millis() - expiry_screen_shown_time) > EXPIRY_SCREEN_TIMEOUT_MS) {
    Serial.println("[LOOP] Expiry screen timeout (30s) - hiding and sending empty date to Sense");
    
    // Send empty expiry date to Sense board to indicate timeout
    StaticJsonDocument<256> doc;
    doc["ver"] = PROTOCOL_VERSION;
    doc["type"] = "INPUT_EXPIRY_DATE";
    doc["msg_id"] = lcd_msg_id_counter++;
    doc["ts"] = millis();
    doc["expiry_date"] = "";  // Empty string indicates no expiry date
    
    String output;
    serializeJson(doc, output);
    request_sense_wake("expiry_timeout");
    uart_send_json(output.c_str());
    Serial.printf("[EXPIRY] Sent empty expiration date to Sense (timeout)\n");
    
    // Hide expiry screen and show list
    lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
    expiry_screen_visible = false;
    expiry_screen_shown_time = 0;
    expiry_date_pos = 0;
    strcpy(expiry_date_buffer, "__-__-____");
    if (expiry_date_label != NULL) {
      lv_label_set_text(expiry_date_label, "MM-DD-YYYY");
    }
    
    // Show list again
    if (list_container != NULL && g_active.count > 0) {
      lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_handler();
  }

  // Probe Sense when needed (avoid continuous UART spam)
  if (!g_in_light_sleep) {
    unsigned long now_ms = millis();
    log_lcd_wake_pin_tick();
    if (sense_pong_pending && sense_pong_deadline_ms > 0 &&
        now_ms > sense_pong_deadline_ms) {
      sense_pong_pending = false;
      sense_pong_deadline_ms = 0;
      if (sense_missed_pongs < 255) {
        sense_missed_pongs++;
      }
      if (sense_missed_pongs >= SENSE_MISSED_PONGS_FOR_ASLEEP) {
        sense_state_set(SENSE_ASLEEP, "missed_pongs");
      } else {
        sense_state_set(SENSE_UNKNOWN, "missed_pongs");
      }
    }
    bool need_probe = (sense_state != SENSE_AWAKE);
    if (need_probe && (now_ms - last_sense_ping_ms) >= SENSE_PROBE_INTERVAL_MS) {
      send_sense_ping();
    }
  }

  // After provisioning completes, clear stale list and request refresh
  if (!g_in_light_sleep && provision_refresh_pending) {
    provision_refresh_pending = false;
    Serial.println("[UI] Provisioning connected - clearing list and requesting refresh");
    if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
      g_active.count = 0;
      g_active.selected_index = -1;
      for (int i = 0; i < MAX_LIST_ITEMS; i++) {
        g_active.items[i][0] = '\0';
        g_active.item_ids[i][0] = '\0';
      }
      save_list_to_storage(&g_active);
      xSemaphoreGive(app_state_mutex);
    }
    if (list_container != NULL) {
      lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (loading_screen != NULL) {
      lv_obj_clear_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
    }
    start_glowing_animation("provision_refresh");
    lv_timer_handler();
    if (!lcd_refresh_inflight) {
      refresh_sm_set_state(REFRESH_INFLIGHT, "provision_refresh");
      lcd_refresh_inflight = true;
      lcd_refresh_ack_seen = false;
      lcd_refresh_retry_count = 0;
      lcd_refresh_sent_ms = lcd_refresh_start_ms;
      waiting_for_list_response = true;
      refresh_request_pending = true;
      refresh_request_needs_send = false;
      refresh_input_wake_sent = false;
      refresh_request_retry_count = 0;
      refresh_grace_extended = false;
      refresh_request_start_ms = lcd_refresh_start_ms;
      refresh_request_last_ms = refresh_request_start_ms;
      request_sense_wake("provision_refresh");
      if (refresh_state != REFRESH_INFLIGHT) {
        Serial.printf("[REFRESH_SM] skip INPUT_WAKE reason=state state=%s\n",
                      refresh_state_name(refresh_state));
      } else if (!refresh_wake_sent) {
        wake_sense_for_request("provision_refresh");
        tx_msg_t tx_msg = {};
        strncpy(tx_msg.type, "INPUT_WAKE", sizeof(tx_msg.type) - 1);
        if (uart_tx_queue != NULL) {
          xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
        }
        refresh_wake_sent = true;
        refresh_last_wake_send_ms = millis();
        Serial.println("[REFRESH] INPUT_WAKE sent (provision)");
      }
    }
    resetActivityTimer();
  }

  update_wifi_on_pending_state();

  // Keepalive while Wi-Fi connecting: send INPUT_PING so Sense link stays recent and does not sleep during LCD connect attempt.
#ifdef HALO_LCD_PROD_WRAPPER
  {
    static unsigned long last_wifi_keepalive_ms = 0;
    unsigned long now_wk = millis();
    if (wifi_on_pending && wifi_pending_start_ms > 0 && !lcd_wifi_connected()) {
      unsigned long budget_wk = wifi_pending_budget_ms ? wifi_pending_budget_ms : wifi_budget_for_reason(wifi_pending_reason);
      unsigned long cap_wk = (budget_wk > WIFI_ATTEMPT_MAX_MS) ? WIFI_ATTEMPT_MAX_MS : budget_wk;
      unsigned long deadline_wk = wifi_pending_start_ms + cap_wk;
      if (now_wk < deadline_wk && (now_wk - last_wifi_keepalive_ms) >= 1500) {
        tx_msg_t tx_msg = {};
        strncpy(tx_msg.type, "INPUT_PING", sizeof(tx_msg.type) - 1);
        if (uart_tx_queue != NULL) {
          xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
        }
        last_wifi_keepalive_ms = now_wk;
      }
    }
  }
#endif

  // If Sense didn't acknowledge wake, retry wake pulses for a short window
  if (!g_in_light_sleep && !sense_awake_confirmed && wake_retry_until_ms > 0 &&
      refresh_state != REFRESH_INFLIGHT) {
    unsigned long now = millis();
    if (now > wake_retry_until_ms) {
      wake_retry_until_ms = 0;
    } else if (now - last_wake_retry_ms >= WAKE_RETRY_INTERVAL_MS) {
      Serial.println("[LCD] Wake retry pulse...");
      lcd_maybe_pulse_sense_int("wake_retry");
    }
  }

  // Refresh state machine: proof-of-life, pulses, timeout
  if (!g_in_light_sleep && refresh_state == REFRESH_WAKE_PENDING) {
    unsigned long now = millis();
    if (refresh_wake_pending_start_ms > 0 &&
        (now - refresh_wake_pending_start_ms) > REFRESH_MAX_MS) {
      Serial.println("[REFRESH_SM] max_ms exceeded in WAKE_PENDING -> soft_fail");
      refresh_soft_fail("wake_pending_max");
      example_lvgl_unlock();
      return;
    }
    if (refresh_wake_pending_last_ping_ms == 0 ||
        (now - refresh_wake_pending_last_ping_ms) >= REFRESH_WAKE_PING_INTERVAL_MS) {
      send_sense_ping();
      refresh_wake_pending_last_ping_ms = now;
    }
    if (now >= refresh_wake_pending_next_pulse_ms) {
      if (refresh_wake_pending_attempts < REFRESH_WAKE_MAX_ATTEMPTS) {
        refresh_wake_pending_attempts++;
        Serial.printf("[REFRESH] state=WAKE_PENDING start=%lu attempt=%u\n",
                      refresh_wake_pending_start_ms,
                      (unsigned)refresh_wake_pending_attempts);
        pulseWakeSenseShort();
        refresh_wake_pending_next_pulse_ms =
            now + REFRESH_WAKE_WAIT_MS +
            (refresh_wake_pending_attempts - 1) * REFRESH_WAKE_PULSE_BACKOFF_MS;
      } else {
        Serial.println("[REFRESH] wake_wait_timeout -> fail");
        refresh_soft_fail("wake_timeout");
      }
    }
  } else if (!g_in_light_sleep && refresh_state == REFRESH_INFLIGHT) {
    unsigned long now = millis();
    if (refresh_last_proof_ms == 0) {
      refresh_last_proof_ms = now;
    }
    if (lcd_refresh_start_ms == 0) {
      lcd_refresh_start_ms = now;
    }
    unsigned long total_ms = now - lcd_refresh_start_ms;
    if (total_ms > REFRESH_MAX_MS) {
      Serial.println("[REFRESH_SM] max_ms exceeded in INFLIGHT -> soft_fail");
      refresh_timeout_count++;
      refresh_soft_fail("inflight_max");
      example_lvgl_unlock();
      return;
    }
    uint32_t pol_age_ms = refresh_last_proof_ms > 0
                              ? safe_age_ms((uint32_t)now, (uint32_t)refresh_last_proof_ms)
                              : 0;

    unsigned long pol_gate_ms = last_proof_of_life_ms > 0
                                    ? (now - last_proof_of_life_ms)
                                    : (now - lcd_refresh_start_ms);
    if (sense_state == SENSE_ASLEEP &&
        pol_gate_ms > REFRESH_PULSE_NO_POL_MS &&
        refresh_pulse_count < REFRESH_PULSE_MAX &&
        (now - refresh_last_wake_send_ms) > REFRESH_PULSE_COOLDOWN_MS) {
      wake_sense_for_request("refresh_retry");
      tx_msg_t tx_msg = {};
      strncpy(tx_msg.type, "INPUT_WAKE", sizeof(tx_msg.type) - 1);
      if (uart_tx_queue != NULL) {
        xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
      }
      refresh_pulse_count++;
      refresh_last_pulse_ms = now;
      refresh_last_wake_send_ms = now;
      lcd_refresh_sent_ms = now;
      refresh_sm_log("pulse", now);
    }

    uint32_t ui_pol_age_ms = refresh_last_ui_pol_ms > 0
                                 ? safe_age_ms((uint32_t)now, (uint32_t)refresh_last_ui_pol_ms)
                                 : 0;
    bool timeout_due = false;
    if (ui_pol_age_ms > REFRESH_NO_UI_POL_TIMEOUT_MS) {
      timeout_due = true;
    } else if (total_ms > REFRESH_TOTAL_MAX_MS) {
      timeout_due = true;
    } else if (total_ms > REFRESH_TOTAL_TIMEOUT_MS &&
               pol_age_ms > REFRESH_PROOF_OF_LIFE_TIMEOUT_MS) {
      timeout_due = true;
    }
    if (timeout_due) {
      Serial.printf("[REFRESH_SM] timeout fail total_ms=%lu pol_ms=%lu ui_pol_ms=%lu\n",
                    total_ms, pol_age_ms, (unsigned long)ui_pol_age_ms);
      refresh_timeout_count++;
      refresh_soft_fail("timeout");
    }
  }

  // Periodic Sense link diagnostics
  {
    static unsigned long last_sense_link_log_ms = 0;
    unsigned long now_ms = millis();
    if (now_ms - last_sense_link_log_ms >= 5000) {
      last_sense_link_log_ms = now_ms;
      uint32_t pol_age = last_proof_of_life_ms > 0
                             ? safe_age_ms((uint32_t)now_ms, (uint32_t)last_proof_of_life_ms)
                             : 0;
      bool inflight = (refresh_state == REFRESH_WAKE_PENDING || refresh_state == REFRESH_INFLIGHT);
      Serial.printf("[SENSE_LINK] now=%lu state=%s last_pol_ms=%lu age_ms=%lu inflight=%d\n",
                    now_ms,
                    sense_state_name(sense_state),
                    (unsigned long)last_proof_of_life_ms,
                    (unsigned long)pol_age,
                    inflight ? 1 : 0);
    }
  }

  processing_watchdog_poll();

  if (!g_in_light_sleep && refresh_state == REFRESH_FAILED) {
    refresh_soft_fail("failed_state");
  }
  
  // Check for inactivity and enter sleep
  // Use extended timeout (30s) if waiting for voice response
  // Use extended timeout if waiting for voice or scan response
  unsigned long timeout_ms = INACTIVITY_TIMEOUT_MS;
  if (provision_screen_visible) {
    timeout_ms = PROVISIONING_TIMEOUT_MS;
  } else if (waiting_for_scan_response) {
    timeout_ms = SCAN_RESPONSE_TIMEOUT_MS;
  } else if (waiting_for_voice_response) {
    timeout_ms = VOICE_RESPONSE_TIMEOUT_MS;
  } else if (waiting_for_list_response) {
    timeout_ms = API_RESPONSE_TIMEOUT_MS;
  }

  unsigned long now_ms = millis();
  unsigned long age_ms = last_user_activity_ms > 0 ? (now_ms - last_user_activity_ms) : 0;
  unsigned long effective_timeout_ms = timeout_ms;
  if (USER_WAKE_HOLD_MS > effective_timeout_ms) {
    effective_timeout_ms = USER_WAKE_HOLD_MS;
  }

  if (!g_in_light_sleep && refresh_state == REFRESH_COMPLETE) {
    if (refresh_done_ms > 0 && (now_ms - refresh_done_ms) >= REFRESH_COMPLETE_SHOW_MS) {
      refresh_sm_set_state(REFRESH_IDLE, "complete_shown");
    }
  }

  if (in_cold_boot_grace()) {
    if (!cold_boot_grace_logged) {
      Serial.println("[SLEEP] cold_boot_grace active; deferring inactivity checks");
      cold_boot_grace_logged = true;
    }
    lcd_log_state(now_ms, age_ms, effective_timeout_ms);
    goto loop_continue;
  }

  if (!g_in_light_sleep) {
    lcd_log_state(now_ms, age_ms, effective_timeout_ms);
    if ((now_ms - last_sleep_coord_status_log_ms) >= 10000) {
      unsigned long rx_age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0;
      Serial.printf("[SLEEP_COORD] now=%lu lcd user_idle_age_ms=%lu user_idle_timeout_ms=%lu sense_state=%s rx_age_ms=%lu\n",
                    now_ms,
                    age_ms,
                    effective_timeout_ms,
                    sense_state_name(sense_state),
                    rx_age_ms);
      last_sleep_coord_status_log_ms = now_ms;
    }
    bool eligible = age_ms > effective_timeout_ms;
    const char* decision_reason = eligible ? "eligible" : "age_lt_timeout";
    if (!eligible) {
      if ((now_ms - last_sleep_decision_log_ms) > 1000) {
        Serial.printf("[SLEEP_DECISION] now=%lu last_act=%lu age=%lu eligible=%d reason=%s\n",
                      now_ms,
                      (unsigned long)last_user_activity_ms,
                      age_ms,
                      0,
                      decision_reason);
        last_sleep_decision_log_ms = now_ms;
      }
      goto loop_continue;
    }
    unsigned long wifi_age_ms = (wifi_on_pending && wifi_pending_start_ms > 0) ? (now_ms - wifi_pending_start_ms) : 0;
    long attempt_deadline_in_ms = -1;
    if (wifi_attempt_deadline_ms > 0 && now_ms < wifi_attempt_deadline_ms) {
      attempt_deadline_in_ms = (long)(wifi_attempt_deadline_ms - now_ms);
      if (attempt_deadline_in_ms < 0) {
        attempt_deadline_in_ms = 0;
      }
    }
    long next_retry_in_ms = 0;
#ifdef HALO_LCD_PROD_WRAPPER
    unsigned long next_wifi_retry_at_ms = halo_lcd_prod_next_wifi_retry_ms();
    if (next_wifi_retry_at_ms > 0) {
      if (now_ms < next_wifi_retry_at_ms) {
        next_retry_in_ms = (long)(next_wifi_retry_at_ms - now_ms);
        if (next_retry_in_ms < 0) {
          next_retry_in_ms = 0;
        }
      }
    }
    int wifi_status = halo_lcd_prod_wifi_status();
#else
    int wifi_status = -1;
#endif
    if ((now_ms - last_sleep_diag_ms) > 1000) {
      Serial.printf("[SLEEP_DIAG] wifi_on_pending=%d wifi_phase=%d attempt_deadline_in_ms=%ld next_retry_in_ms=%ld wifi_status=%d\n",
                    wifi_on_pending ? 1 : 0,
                    wifi_phase,
                    attempt_deadline_in_ms,
                    next_retry_in_ms,
                    wifi_status);
      last_sleep_diag_ms = now_ms;
    }
    const char* inhibit_reason = nullptr;
    if (millis() < sense_awake_grace_until_ms) {
      inhibit_reason = "grace";
    } else if (status_screen != NULL && status_label != NULL &&
               !lv_obj_has_flag(status_screen, LV_OBJ_FLAG_HIDDEN)) {
      const char* current_text = lv_label_get_text(status_label);
      if (current_text &&
          (strcmp(current_text, "Refreshing…") == 0 ||
           strcmp(current_text, "Refreshing...") == 0)) {
        inhibit_reason = "refreshing";
      }
    }
    if (!inhibit_reason && is_glowing_animation) {
      inhibit_reason = "processing";
    }
    if (!inhibit_reason && ota_locked) {
      inhibit_reason = "ota_locked";
    }
    if (inhibit_reason) {
      unsigned long now_ms = millis();
      decision_reason = inhibit_reason;
      if (strncmp(last_sleep_skip_reason, inhibit_reason, sizeof(last_sleep_skip_reason)) != 0 ||
          (now_ms - last_sleep_skip_log_ms) > 1000) {
        if (strcmp(inhibit_reason, "processing") == 0) {
          log_sleep_inhibit_processing(now_ms);
        } else {
          Serial.printf("[SLEEP] inhibited reason=%s\n", inhibit_reason);
        }
        strncpy(last_sleep_skip_reason, inhibit_reason, sizeof(last_sleep_skip_reason) - 1);
        last_sleep_skip_reason[sizeof(last_sleep_skip_reason) - 1] = '\0';
        last_sleep_skip_log_ms = now_ms;
      }
      if ((now_ms - last_sleep_decision_log_ms) > 1000) {
        Serial.printf("[SLEEP_DECISION] now=%lu last_act=%lu age=%lu eligible=%d reason=%s\n",
                      now_ms,
                      (unsigned long)last_user_activity_ms,
                      age_ms,
                      0,
                      decision_reason);
        last_sleep_decision_log_ms = now_ms;
      }
      goto loop_continue;
    }
    const char* skip_reason = nullptr;
    // Block sleep only when phase is CONNECTING (active attempt). BACKOFF and OFF allow sleep.
    if (wifi_phase == WIFI_PHASE_CONNECTING) {
      skip_reason = "wifi_connecting_in_progress";
      wifi_on_pending_logged = true;
    } else if (wifi_on_pending && !lcd_wifi_connected() && wifi_attempt_deadline_ms > 0 && now_ms < wifi_attempt_deadline_ms) {
      skip_reason = "wifi_connecting_in_progress";
      wifi_on_pending_logged = true;
    } else if (lcd_wifi_connecting() && lcd_has_wifi_creds()) {
      skip_reason = "wifi_connecting_in_progress";
    } else if (ota_check_requested || ota_check_pending) {
      skip_reason = "ota_pending";
      ota_pending_logged = true;
    }
    if (skip_reason) {
      unsigned long now_ms = millis();
      decision_reason = skip_reason;
      if (strncmp(last_sleep_skip_reason, skip_reason, sizeof(last_sleep_skip_reason)) != 0 ||
          (now_ms - last_sleep_skip_log_ms) > 1000) {
        Serial.printf("[SLEEP] skip reason=%s\n", skip_reason);
        strncpy(last_sleep_skip_reason, skip_reason, sizeof(last_sleep_skip_reason) - 1);
        last_sleep_skip_reason[sizeof(last_sleep_skip_reason) - 1] = '\0';
        last_sleep_skip_log_ms = now_ms;
      }
      if ((now_ms - last_sleep_decision_log_ms) > 1000) {
        Serial.printf("[SLEEP_DECISION] now=%lu last_act=%lu age=%lu eligible=%d reason=%s\n",
                      now_ms,
                      (unsigned long)last_user_activity_ms,
                      age_ms,
                      0,
                      decision_reason);
        last_sleep_decision_log_ms = now_ms;
      }
      goto loop_continue;
    }
    if (millis() < ota_stay_awake_until_ms) {
      static unsigned long last_ota_log_ms = 0;
      if (millis() - last_ota_log_ms > 5000) {
        Serial.println("[OTA] stay_awake window active - deferring sleep");
        last_ota_log_ms = millis();
      }
      if ((now_ms - last_sleep_decision_log_ms) > 1000) {
        Serial.printf("[SLEEP_DECISION] now=%lu last_act=%lu age=%lu eligible=%d reason=ota_stay_awake\n",
                      now_ms,
                      (unsigned long)last_user_activity_ms,
                      age_ms,
                      0);
        last_sleep_decision_log_ms = now_ms;
      }
      goto loop_continue;
    }
    if (millis() < stay_awake_until_ms) {
      static unsigned long last_stay_awake_log_ms = 0;
      unsigned long now_ms = millis();
      if (now_ms - last_stay_awake_log_ms > 1000) {
        unsigned long remaining_ms = stay_awake_until_ms - now_ms;
        Serial.printf("[SLEEP] blocked stay_awake_window remaining_ms=%lu\n",
                      remaining_ms);
        last_stay_awake_log_ms = now_ms;
      }
      goto loop_continue;
    }
#ifdef HALO_LCD_PROD_WRAPPER
    if (halo_lcd_prod_should_stay_awake()) {
      Serial.println("[LOOP] Sleep deferred (LCD OTA pending)");
      resetActivityTimer();
      goto loop_continue;
    }
#endif
    if (sleep_retry_requires_user && !user_activity_since_sleep) {
      if (now_ms - last_sleep_retry_log_ms > 5000) {
        Serial.println("[SLEEP] retry_requires_user -> skip_sleep");
        last_sleep_retry_log_ms = now_ms;
      }
      goto loop_continue;
    }
    if (sleep_retry_allowed_ms > 0 && now_ms < sleep_retry_allowed_ms) {
      if (now_ms - last_sleep_retry_log_ms > 5000) {
        unsigned long remaining_ms = sleep_retry_allowed_ms - now_ms;
        Serial.printf("[SLEEP] retry_backoff remaining_ms=%lu\n", remaining_ms);
        last_sleep_retry_log_ms = now_ms;
      }
      goto loop_continue;
    }
    if ((now_ms - last_sleep_decision_log_ms) > 1000) {
      Serial.printf("[SLEEP_DECISION] now=%lu last_act=%lu age=%lu eligible=%d reason=eligible\n",
                    now_ms,
                    (unsigned long)last_user_activity_ms,
                    age_ms,
                    1);
      last_sleep_decision_log_ms = now_ms;
    }
    if (waiting_for_voice_response) {
      Serial.println("[LOOP] Inactivity timeout (voice mode: 30s) - entering sleep...");
    } else if (waiting_for_scan_response) {
      Serial.println("[LOOP] Inactivity timeout (scan mode: 90s) - entering sleep...");
    } else {
      Serial.println("[LOOP] Inactivity timeout - entering sleep...");
    }
    wifi_on_run_deferred_if_ready("pre_sleep");
    enterLightSleep();
    resetActivityTimer();  // Reset after wake
  }

loop_continue:
  example_lvgl_unlock();

#ifdef HALO_LCD_PROD_WRAPPER
  halo_lcd_prod_loop();
#endif

  vTaskDelay(pdMS_TO_TICKS(50));  // Poll touch every 50ms (use vTaskDelay to yield to IDLE task)
}


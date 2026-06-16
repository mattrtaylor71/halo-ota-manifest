/*
 * LCD_Minimal.ino
 * 
 * MINIMAL VERSION - Only essential functionality:
 * - LCD screen asleep by default
 * - Wake on touch or scroll
 * - On wake, wake Sense board without list refresh behavior
 * - User can scroll through items
 * - Knob scroll is local-only; no list refresh behavior
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
#include "ui_screen_registry.h"
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
#include "driver/rtc_io.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "Preferences.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>  // settimeofday() for syncing clock from Sense MAINT_WINDOW now_epoch
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
// PNG frame images removed — replaced with programmatic LVGL rendering

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
#define MAX_LINE_LENGTH 4096  // Allows richer UI_VOICE_RESPONSE payloads over UART

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
#define UART_RX_DIAG_CAPTURE_LIMIT 32
HardwareSerial senseSerial(1);  // Use UART1

// ── Wakeup Configuration ──────────────────────────────────────────
// Wake roles:
// - LCD drives INT_PIN low to wake Sense EXT0 (GPIO2, active low).
// - LCD wakes from touch INT (LCD_WAKE_GPIO GPIO9, active low).
#define INT_PIN 39   // LCD -> Sense wake line (GPIO39)
#define MAINT_FORCE_WAKE_SENSE 1  // TEMP: allow INT wake during maintenance
static const gpio_num_t PIN_TOUCH_INT = HALO_WAKE_GPIO;   // TP_INT
static const gpio_num_t PIN_EC1_A     = GPIO_NUM_8;   // EC1_A
static const gpio_num_t PIN_EC1_B     = GPIO_NUM_7;   // EC1_B
#define LCD_WAKE_GPIO PIN_TOUCH_INT
#define WAKE_GPIO LCD_WAKE_GPIO
#define LCD_WAKE_LEVEL HALO_WAKE_LEVEL
#define WAKE_PIN_BOOT_WARN_MS 200
#define LCD_SLEEP_FALLBACK_TIMER_SEC 30
// DEV OVERRIDE: Wake every 60s for unattended OTA testing.
// Comment out HALO_DEV_WAKE_INTERVAL_SEC to restore production 6-hour interval.
// #define HALO_DEV_WAKE_INTERVAL_SEC 60

#ifdef HALO_DEV_WAKE_INTERVAL_SEC
#define LCD_OTA_WAKE_INTERVAL_SEC HALO_DEV_WAKE_INTERVAL_SEC
#else
#define LCD_OTA_WAKE_INTERVAL_SEC (6 * 60 * 60)
#endif
#define HALO_ALLOW_TIMER_WAKE 1
static const uint16_t LCD_OTA_SCHED_START_MIN = 120;  // 2:00 AM local
static const uint16_t LCD_OTA_SCHED_WINDOW_MIN = 30;
#ifndef LCD_OTA_SCHED_TEST_OFFSET_SEC
#define LCD_OTA_SCHED_TEST_OFFSET_SEC 0
#endif
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
  SHIP_MENU_ACTION_AI,
  SHIP_MENU_ACTION_HOME,
  SHIP_MENU_ACTION_SETTINGS,
  SHIP_MENU_ACTION_DEBUG,
  SHIP_MENU_ACTION_RESET_WIFI,
  SHIP_MENU_ACTION_MANUAL_OTA,
  SHIP_MENU_ACTION_BACKLIGHT,
  SHIP_MENU_ACTION_DEBUG_LOG,
  SHIP_MENU_ACTION_SHOPPING_LIST,
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
  SCREEN_BACKLIGHT,
  SCREEN_AI_LISTENING,
  SCREEN_VOICE_JSON,
  SCREEN_HOLD_STILL,
  SCREEN_VOICE_ACK,
  SCREEN_PROCESSING,
  SCREEN_LOGGED,
  SCREEN_EXPIRY_CHOICE,
  SCREEN_EXPIRY,
  SCREEN_RESULT,
  SCREEN_DEBUG,
  SCREEN_ERRLOG,
  SCREEN_ERRLOG_DETAIL,
  SCREEN_SHOPPING_LIST
} ui_screen_t;

typedef enum {
  SHIP_USER_STATE_ASLEEP = 0,
  SHIP_USER_STATE_MENU_READY,
  SHIP_USER_STATE_CAPTURE_COMMITTED,
  SHIP_USER_STATE_USER_WAITING_RESULT,
  SHIP_USER_STATE_USER_INPUT_REQUIRED,
  SHIP_USER_STATE_USER_FEEDBACK
} ship_user_state_t;

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
static bool lcd_sleep_intent_allowed(const char** reason_out);
static bool notify_sense_sleep();
static void init_touch_once();
static void init_knob_once();
static void init_ui_stack(int saved_count);
static void enter_ship_ota_sleep();
static void lcd_clear_maintenance_state(const char* reason, bool mark_completed);
static void lcd_exit_ota_mode(const char* reason);
static bool wake_sense_for_request(const char* reason);
static uint64_t buildWakeMaskForSleep();
static bool ui_is_sleep_eligible_menu_screen(ui_screen_t state);
static const char* ui_screen_state_name(ui_screen_t state);
static ship_user_state_t ship_user_state_current();
static const char* ship_user_state_name(ship_user_state_t state);
static void log_active_screen(const char* reason);
static void expiry_choice_update_quantity_label();
static void expiry_choice_adjust_quantity(int delta);
static bool ship_choice_mode_is_discard(void);
static void ship_configure_scan_choice_screen(void);
static void ship_send_discard_choice(bool add_to_shopping_list, const char* wake_reason);
static bool sleep_blocked_for_ota();
static void status_screen_use_text(const char* text);
static void status_screen_use_image(const lv_img_dsc_t* image);
static void status_screen_use_image_named(const lv_img_dsc_t* image, const char* asset_name, const char* reason);
static void set_status_reset_visible(bool visible);
static void uart_note_input_type(const char* type);
static void provision_qr_wait_begin(const char* reason);
static void provision_qr_wait_clear(const char* reason);
static void expiry_update_timeout_ring();
static void ui_reset_lvgl_objects();
static void ui_show_screen(ScreenId next, const char* reason, const char* file, int line, const char* func);
static void ui_show_screen_impl(ScreenId next);
static void ensure_awake_for_ui(const char* reason);
static void ship_show_error_impl();
static void ship_menu_send_menu_select(const char* menu_item, int menu_index, const char* label);
static void ship_menu_send_retry(const char* label);
static void ship_menu_update_versions_label();
static void ship_menu_request_fw_info();
static void ship_menu_service_fw_info_request(unsigned long now_ms);
static void show_ship_debug_screen();
static void show_ship_debug_screen_impl();
static void ui_show_result(bool is_error, const char* title, const char* mode);
static void ui_show_result_impl(bool is_error, const char* title, const char* mode);
static void ui_apply_ship_ui_status(const app_event_t* evt);
static void ship_show_voice_json_screen(const char* json_text);
static void ship_init_voice_json_screen();
static void wifi_on_run_deferred_if_ready(const char* reason);
static void ship_start_processing_animation();
static void ship_update_processing_progress();
static void ship_update_ai_listening_countdown();
static bool ship_mode_is_dish(const char* op, const char* mode);

#define UI_SHOW(next, reason) ui_show_screen(next, reason, __FILE__, __LINE__, __func__)

// ── Test Mode (automated testing, disables sleep) ──────────────────
static bool g_test_mode_active = false;
static unsigned long g_test_mode_expire_ms = 0;

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
static unsigned long uart_rx_raw_bytes_seen = 0;
static unsigned long uart_rx_completed_lines_seen = 0;
static unsigned long uart_rx_valid_msgs_seen = 0;
static unsigned long uart_rx_partial_started_ms = 0;
static unsigned long uart_rx_last_summary_ms = 0;
static unsigned long uart_rx_last_partial_log_ms = 0;
static unsigned long uart_rx_diag_raw_logged = 0;
static uint32_t uart_tx_count = 0;
static uint32_t uart_rx_count = 0;
static unsigned long last_uart_tx_type_ms = 0;
static unsigned long last_uart_rx_type_ms = 0;
static unsigned long last_input_type_ms = 0;
static char last_uart_tx_type[24] = "";
static char last_uart_rx_type[24] = "";
static char last_input_type[24] = "";

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
static bool g_lcd_maintenance_headless = false;
static unsigned long g_lcd_maintenance_deadline_ms = 0;
static volatile bool g_lcd_maintenance_aborted = false;
static unsigned long g_lcd_maintenance_completed_ms = 0;
RTC_DATA_ATTR static uint8_t g_lcd_maintenance_timer_armed = 0;
RTC_DATA_ATTR static uint32_t g_lcd_maintenance_wake_in_s = 0;
RTC_DATA_ATTR static uint32_t g_lcd_maintenance_remaining_s = 0;
RTC_DATA_ATTR static char g_lcd_maintenance_request_id[64] = "";
RTC_DATA_ATTR static uint64_t g_lcd_maintenance_start_epoch = 0;
RTC_DATA_ATTR static uint32_t g_lcd_maintenance_duration_sec = 0;
RTC_DATA_ATTR static uint32_t g_lcd_maintenance_grace_before_sec = 0;
RTC_DATA_ATTR static uint32_t g_lcd_maintenance_grace_after_sec = 0;
RTC_DATA_ATTR static uint8_t g_lcd_schedule_timer_armed = 0;
RTC_DATA_ATTR static uint32_t g_lcd_schedule_wake_in_s = 0;
RTC_DATA_ATTR static uint32_t g_lcd_schedule_next_epoch = 0;
static bool g_lcd_maintenance_wake_window = false;
static bool g_lcd_schedule_wake_window = false;
static unsigned long g_lcd_schedule_window_deadline_ms = 0;
static unsigned long g_lcd_maintenance_boot_grace_until_ms = 0;
static const unsigned long LCD_MAINT_BOOT_GRACE_MS = 120000;
static char s_wake_cause_label[32] = "cold_boot";
static const char* LCD_MAINT_PREF_NAMESPACE = "lcd_maint";
static const char* LCD_MAINT_PREF_KEY_VALID = "valid";
static const char* LCD_MAINT_PREF_KEY_ARMED = "armed";
static const char* LCD_MAINT_PREF_KEY_WAKE_S = "wake_s";
static const char* LCD_MAINT_PREF_KEY_REMAIN_S = "remain_s";
static const char* LCD_MAINT_PREF_KEY_REQ_ID = "req_id";
static const char* LCD_MAINT_PREF_KEY_START = "start_ep";
static const char* LCD_MAINT_PREF_KEY_DUR = "dur_s";
static const char* LCD_MAINT_PREF_KEY_GB = "grace_b";
static const char* LCD_MAINT_PREF_KEY_GA = "grace_a";

static void lcd_log_rtc_timer_state(const char* reason) {
  Serial.printf("[LCD_RTC] reason=%s maint_armed=%d maint_wake_in_s=%lu maint_remaining_s=%lu sched_armed=%d sched_wake_in_s=%lu sched_next_epoch=%lu wake_label=%s\n",
                reason ? reason : "unknown",
                g_lcd_maintenance_timer_armed ? 1 : 0,
                (unsigned long)g_lcd_maintenance_wake_in_s,
                (unsigned long)g_lcd_maintenance_remaining_s,
                g_lcd_schedule_timer_armed ? 1 : 0,
                (unsigned long)g_lcd_schedule_wake_in_s,
                (unsigned long)g_lcd_schedule_next_epoch,
                s_wake_cause_label[0] ? s_wake_cause_label : "-");
}

static inline void lcd_sleep_ts(const char* label) {
  Serial.printf("[SLEEP_TS] %s now_ms=%lu\n",
                label ? label : "event",
                (unsigned long)millis());
}

static bool lcd_time_valid() {
  time_t now = time(nullptr);
  return now > 1700000000;
}

// The LCD has no NTP/RTC time source of its own. The Sense carries its wall
// clock in MAINT_WINDOW (now_epoch); applying it here makes lcd_time_valid()
// true and lets the absolute-epoch maintenance machinery (window-current /
// remaining-s / self-wake target) run. ESP-IDF carries the set time across
// deep sleep via the RTC, so subsequent partial wakes keep an accurate clock.
// Cheap to re-apply on every MAINT_WINDOW (corrects drift). No-op for epoch<=0.
// Forward declaration (defined after lcd_activity.h). Declared early so the
// maintenance restore/sleep paths above the definition can emit diagnostic
// breadcrumbs (arm-time delivery race fix).
static void lcd_errlog_store_with_context(const char* board, const char* area,
                                           const char* event, int32_t code,
                                           const char* detail);

static void lcd_set_clock_from_sense(uint64_t now_epoch, const char* reason) {
  if (now_epoch <= 1700000000ULL) {
    return;  // invalid / absent (old Sense) — keep relative fallback behavior
  }
  struct timeval tv;
  tv.tv_sec = (time_t)now_epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.printf("[LCD_TIME] set from sense now_epoch=%llu reason=%s valid=%d\n",
                (unsigned long long)now_epoch,
                reason ? reason : "unknown",
                lcd_time_valid() ? 1 : 0);
}

// ── Shopping-list cache metadata ─────────────────────────────────────
// Epoch (seconds) when the list cache was last saved to NVS. 0 = unknown
// (no cache yet, or the LCD had no valid wall clock at save time). Written
// by save_list_to_storage()/load_list_from_storage() in lcd_persist.h.
static volatile uint32_t g_list_cache_fetched_epoch = 0;
// True once a UI_LIST has landed since boot. Gates the "No items on your
// list" empty state: an empty g_active is only genuinely empty after a
// refresh actually completed (otherwise it's just a cache miss).
static volatile bool g_list_refresh_completed_once = false;

// Age of the persisted list cache in seconds; -1 when unknown (no cache,
// or no valid wall clock at save or query time).
static int list_cache_age_s() {
  if (g_list_cache_fetched_epoch == 0 || !lcd_time_valid()) {
    return -1;
  }
  uint32_t now_epoch = (uint32_t)time(nullptr);
  return (now_epoch >= g_list_cache_fetched_epoch)
             ? (int)(now_epoch - g_list_cache_fetched_epoch)
             : -1;
}

static bool lcd_maintenance_context_present() {
  return g_lcd_maintenance_timer_armed ||
         g_lcd_maintenance_wake_in_s > 0 ||
         g_lcd_maintenance_remaining_s > 0 ||
         g_lcd_maintenance_start_epoch > 0 ||
         g_lcd_maintenance_request_id[0] != '\0';
}

static bool lcd_maintenance_window_is_current(uint64_t now_epoch) {
  if (g_lcd_maintenance_start_epoch == 0) {
    return false;
  }
  uint64_t start_epoch =
      (g_lcd_maintenance_start_epoch >= (uint64_t)g_lcd_maintenance_grace_before_sec)
          ? (g_lcd_maintenance_start_epoch - (uint64_t)g_lcd_maintenance_grace_before_sec)
          : 0;
  uint64_t end_epoch = g_lcd_maintenance_start_epoch +
                       (uint64_t)g_lcd_maintenance_duration_sec +
                       (uint64_t)g_lcd_maintenance_grace_after_sec;
  return now_epoch >= start_epoch && now_epoch <= end_epoch;
}

static uint32_t lcd_maintenance_window_remaining_s(uint64_t now_epoch) {
  if (g_lcd_maintenance_start_epoch == 0) {
    return 0;
  }
  uint64_t end_epoch = g_lcd_maintenance_start_epoch +
                       (uint64_t)g_lcd_maintenance_duration_sec +
                       (uint64_t)g_lcd_maintenance_grace_after_sec;
  if (now_epoch >= end_epoch) {
    return 0;
  }
  uint64_t remaining = end_epoch - now_epoch;
  if (remaining > 0xFFFFFFFFULL) {
    remaining = 0xFFFFFFFFULL;
  }
  return (uint32_t)remaining;
}

static bool lcd_should_resume_maintenance_on_boot(bool restored_from_nvs,
                                                  bool maintenance_in_window) {
  if (!lcd_maintenance_context_present()) {
    return false;
  }
  if (g_lcd_maintenance_timer_armed || maintenance_in_window) {
    return true;
  }
  if (!restored_from_nvs) {
    return false;
  }
  return g_lcd_maintenance_wake_in_s > 0 ||
         g_lcd_maintenance_remaining_s > 0 ||
         g_lcd_maintenance_start_epoch > 0 ||
         g_lcd_maintenance_request_id[0] != '\0';
}

static void lcd_clear_persisted_maintenance_state(const char* reason) {
  Preferences prefs;
  if (prefs.begin(LCD_MAINT_PREF_NAMESPACE, false)) {
    prefs.clear();
    prefs.end();
  }
  Serial.printf("[LCD_MAINT_NVS] clear reason=%s\n", reason ? reason : "unknown");
}

static void lcd_persist_maintenance_state(const char* reason) {
  bool valid = lcd_maintenance_context_present();
  if (!valid) {
    lcd_clear_persisted_maintenance_state(reason ? reason : "empty_state");
    return;
  }
  Preferences prefs;
  if (!prefs.begin(LCD_MAINT_PREF_NAMESPACE, false)) {
    Serial.printf("[LCD_MAINT_NVS] save_failed reason=%s\n", reason ? reason : "unknown");
    return;
  }
  prefs.putBool(LCD_MAINT_PREF_KEY_VALID, true);
  prefs.putBool(LCD_MAINT_PREF_KEY_ARMED, g_lcd_maintenance_timer_armed != 0);
  prefs.putUInt(LCD_MAINT_PREF_KEY_WAKE_S, g_lcd_maintenance_wake_in_s);
  prefs.putUInt(LCD_MAINT_PREF_KEY_REMAIN_S, g_lcd_maintenance_remaining_s);
  prefs.putString(LCD_MAINT_PREF_KEY_REQ_ID, g_lcd_maintenance_request_id);
  prefs.putUInt(LCD_MAINT_PREF_KEY_START, (uint32_t)g_lcd_maintenance_start_epoch);
  prefs.putUInt(LCD_MAINT_PREF_KEY_DUR, g_lcd_maintenance_duration_sec);
  prefs.putUInt(LCD_MAINT_PREF_KEY_GB, g_lcd_maintenance_grace_before_sec);
  prefs.putUInt(LCD_MAINT_PREF_KEY_GA, g_lcd_maintenance_grace_after_sec);
  prefs.end();
  Serial.printf("[LCD_MAINT_NVS] save reason=%s armed=%d wake_in_s=%lu remaining_s=%lu start_epoch=%lu request_id=%s\n",
                reason ? reason : "unknown",
                g_lcd_maintenance_timer_armed ? 1 : 0,
                (unsigned long)g_lcd_maintenance_wake_in_s,
                (unsigned long)g_lcd_maintenance_remaining_s,
                (unsigned long)g_lcd_maintenance_start_epoch,
                g_lcd_maintenance_request_id[0] ? g_lcd_maintenance_request_id : "-");
}

static bool lcd_restore_persisted_maintenance_state(const char* reason) {
  if (lcd_maintenance_context_present()) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(LCD_MAINT_PREF_NAMESPACE, true)) {
    return false;
  }
  bool valid = prefs.getBool(LCD_MAINT_PREF_KEY_VALID, false);
  if (!valid) {
    prefs.end();
    return false;
  }

  uint8_t armed = prefs.getBool(LCD_MAINT_PREF_KEY_ARMED, false) ? 1 : 0;
  uint32_t wake_in_s = prefs.getUInt(LCD_MAINT_PREF_KEY_WAKE_S, 0);
  uint32_t remaining_s = prefs.getUInt(LCD_MAINT_PREF_KEY_REMAIN_S, 0);
  String request_id = prefs.getString(LCD_MAINT_PREF_KEY_REQ_ID, "");
  uint32_t start_epoch = prefs.getUInt(LCD_MAINT_PREF_KEY_START, 0);
  uint32_t duration_sec = prefs.getUInt(LCD_MAINT_PREF_KEY_DUR, 0);
  uint32_t grace_before_sec = prefs.getUInt(LCD_MAINT_PREF_KEY_GB, 0);
  uint32_t grace_after_sec = prefs.getUInt(LCD_MAINT_PREF_KEY_GA, 0);
  prefs.end();

  if (lcd_time_valid() && start_epoch > 0) {
    uint64_t now_epoch = (uint64_t)time(nullptr);
    uint64_t window_end = (uint64_t)start_epoch +
                          (uint64_t)duration_sec +
                          (uint64_t)grace_after_sec;
    if (now_epoch > window_end && armed == 0 && remaining_s == 0) {
      lcd_clear_persisted_maintenance_state("expired_on_boot");
      return false;
    }
  }

  g_lcd_maintenance_timer_armed = armed;
  g_lcd_maintenance_wake_in_s = wake_in_s;
  g_lcd_maintenance_remaining_s = remaining_s;
  g_lcd_maintenance_start_epoch = start_epoch;
  g_lcd_maintenance_duration_sec = duration_sec;
  g_lcd_maintenance_grace_before_sec = grace_before_sec;
  g_lcd_maintenance_grace_after_sec = grace_after_sec;
  if (request_id.length() > 0) {
    strncpy(g_lcd_maintenance_request_id, request_id.c_str(), sizeof(g_lcd_maintenance_request_id) - 1);
    g_lcd_maintenance_request_id[sizeof(g_lcd_maintenance_request_id) - 1] = '\0';
  } else {
    g_lcd_maintenance_request_id[0] = '\0';
  }
  Serial.printf("[LCD_MAINT_NVS] restore reason=%s armed=%d wake_in_s=%lu remaining_s=%lu start_epoch=%lu request_id=%s\n",
                reason ? reason : "unknown",
                g_lcd_maintenance_timer_armed ? 1 : 0,
                (unsigned long)g_lcd_maintenance_wake_in_s,
                (unsigned long)g_lcd_maintenance_remaining_s,
                (unsigned long)g_lcd_maintenance_start_epoch,
                g_lcd_maintenance_request_id[0] ? g_lcd_maintenance_request_id : "-");
  // Arm-time delivery race fix (breadcrumb): capture the post-deep-sleep restore
  // of the maintenance timer + whether the clock was valid then. value=armed,
  // detail encodes clock_valid + request_id (clk=<0/1> rid=<...>).
  {
    char restore_detail[80];
    snprintf(restore_detail, sizeof(restore_detail), "clk=%d rid=%s",
             lcd_time_valid() ? 1 : 0,
             g_lcd_maintenance_request_id[0] ? g_lcd_maintenance_request_id : "-");
    lcd_errlog_store_with_context("lcd", "maint", "RESTORE",
                                  (int)g_lcd_maintenance_timer_armed, restore_detail);
  }
  return true;
}

static uint32_t lcd_sched_compute_next_epoch(time_t now) {
  if (now <= 0) {
    return 0;
  }
  if (LCD_OTA_SCHED_TEST_OFFSET_SEC > 0) {
    return (uint32_t)(now + (time_t)LCD_OTA_SCHED_TEST_OFFSET_SEC);
  }
  tm local = {};
  localtime_r(&now, &local);
  int cur_min = local.tm_hour * 60 + local.tm_min;
  time_t day_start = now - (local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
  time_t target = day_start + (LCD_OTA_SCHED_START_MIN * 60);
  if ((cur_min * 60 + local.tm_sec) >= (LCD_OTA_SCHED_START_MIN * 60)) {
    target += 86400;
  }
  return (uint32_t)target;
}

static void lcd_sched_update_next_epoch(time_t now) {
  if (!lcd_time_valid()) {
    return;
  }
  if (LCD_OTA_SCHED_TEST_OFFSET_SEC > 0) {
    Serial.printf("[LCD_SCHED] test_override offset_s=%d\n", LCD_OTA_SCHED_TEST_OFFSET_SEC);
  }
  if (g_lcd_schedule_next_epoch == 0 || g_lcd_schedule_next_epoch <= (uint32_t)now) {
    g_lcd_schedule_next_epoch = lcd_sched_compute_next_epoch(now);
    if (g_lcd_schedule_next_epoch > 0) {
      time_t target = (time_t)g_lcd_schedule_next_epoch;
      tm local = {};
      localtime_r(&target, &local);
      Serial.printf("[LCD_SCHED] next_epoch=%lu local=%02d:%02d\n",
                    (unsigned long)g_lcd_schedule_next_epoch,
                    local.tm_hour,
                    local.tm_min);
    }
  }
}

static bool lcd_schedule_window_active() {
  if (!g_lcd_schedule_wake_window) {
    return false;
  }
  if (g_lcd_schedule_window_deadline_ms == 0) {
    return true;
  }
  return millis() < g_lcd_schedule_window_deadline_ms;
}

bool lcd_maintenance_active(void) {
  return g_lcd_maintenance_active;
}

unsigned long lcd_maintenance_remaining_ms(void) {
  if (!g_lcd_maintenance_active || g_lcd_maintenance_deadline_ms == 0) {
    return 0;
  }
  unsigned long now_ms = millis();
  return (now_ms < g_lcd_maintenance_deadline_ms)
             ? (g_lcd_maintenance_deadline_ms - now_ms)
             : 0;
}

bool lcd_schedule_maintenance_retry(unsigned long delay_ms, const char* reason) {
  unsigned long remaining_ms = lcd_maintenance_remaining_ms();
  if (!g_lcd_maintenance_active || remaining_ms == 0) {
    Serial.printf("[LCD_MAINT] retry_not_scheduled reason=%s active=%d remaining_ms=%lu\n",
                  reason ? reason : "unknown",
                  g_lcd_maintenance_active ? 1 : 0,
                  (unsigned long)remaining_ms);
    return false;
  }

  unsigned long effective_delay_ms = delay_ms;
  if (effective_delay_ms == 0 || effective_delay_ms >= remaining_ms) {
    effective_delay_ms = (remaining_ms > 1000UL) ? (remaining_ms - 1000UL) : remaining_ms;
  }
  if (effective_delay_ms < 1000UL) {
    Serial.printf("[LCD_MAINT] retry_not_scheduled reason=%s remaining_ms=%lu delay_ms=%lu\n",
                  reason ? reason : "unknown",
                  (unsigned long)remaining_ms,
                  (unsigned long)effective_delay_ms);
    return false;
  }

  g_lcd_maintenance_timer_armed = 1;
  g_lcd_maintenance_wake_in_s = (uint32_t)((effective_delay_ms + 999UL) / 1000UL);
  g_lcd_maintenance_remaining_s = (uint32_t)((remaining_ms + 999UL) / 1000UL);
  Serial.printf("[LCD_MAINT] retry_scheduled reason=%s delay_ms=%lu wake_in_s=%lu remaining_ms=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)effective_delay_ms,
                (unsigned long)g_lcd_maintenance_wake_in_s,
                (unsigned long)remaining_ms);
  lcd_log_rtc_timer_state("maintenance_retry_schedule");
  return true;
}

const char* lcd_wake_cause_label(void) {
  return s_wake_cause_label;
}

extern const char* kFirmwareVersion;
static bool sense_awake_confirmed = false;
static unsigned long last_sense_any_rx_ms = 0;
static unsigned long wake_retry_until_ms = 0;
static unsigned long last_wake_retry_ms = 0;
static uint8_t wake_retry_attempts = 0;
static const unsigned long WAKE_RETRY_INTERVAL_MS = 1200;
static const unsigned long WAKE_RETRY_WINDOW_MS = 10000;
static const unsigned long WAKE_PULSE_DURATION_MS = 80;
static const unsigned long WAKE_PULSE_SHORT_MS = 30;
static bool wake_timer_wait_mode = false;
static unsigned long wake_timer_wait_start_ms = 0;
static const unsigned long WAKE_TIMER_WAIT_WINDOW_MS = 30000;  // 30s — covers 15s failsafe + boot + margin
static unsigned long maint_force_wake_last_ms = 0;
static const unsigned long MAINT_FORCE_WAKE_INTERVAL_MS = 2000;
static const unsigned long WAKE_LINE_STUCK_WARN_MS = 3000;
static const unsigned long INT_PULSE_COOLDOWN_MS = 5000;
static unsigned long last_int_pulse_ms = 0;
static unsigned long last_lcd_keepalive_ms = 0;
static const unsigned long LCD_SENSE_KEEPALIVE_MS = 2000;
static volatile bool user_activity_since_sleep = false;
static volatile bool sense_status_sync_requested = false;
static volatile bool sense_ota_apply_required = false;
static unsigned int sleep_handshake_fail_count = 0;
static unsigned long sleep_retry_allowed_ms = 0;
static bool sleep_retry_requires_user = false;
static unsigned long last_sleep_retry_log_ms = 0;
static unsigned long last_user_activity_ms = 0;
static volatile unsigned long last_scroll_activity_ms = 0;
static volatile bool scroll_activity_pending = false;
static bool g_idle_screen_dark = false;
static unsigned long last_scroll_bump_log_ms = 0;
#define SCROLL_ACTIVITY_LOG_MS 500
static unsigned long home_shown_ms = 0;
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
static unsigned long last_sense_sleep_ready_ms = 0;
static const unsigned long SENSE_RX_STALE_MS = 8000;
static const unsigned long SENSE_SLEEP_READY_GRACE_MS = 15000;
static const unsigned long SENSE_UNKNOWN_STALE_MS = 12000;
static const unsigned long SENSE_UNKNOWN_STALE_EXTENDED_MS = 30000;
static const unsigned long SENSE_PING_MIN_INTERVAL_MS = 5000;
static uint32_t sense_awake_grace_until_ms = 0;
static const uint32_t SENSE_AWAKE_GRACE_MS = 10000;
static const uint32_t SENSE_RECENT_RX_FOR_SLEEP_MS = 10000;
static const uint32_t SLEEP_LINK_RETRY_MS = 5000;
static bool wake_ext0_enabled = false;
static bool wake_ext1_enabled = false;
static bool wake_timer_enabled = false;
static bool wake_uart_enabled = false;
static volatile bool sleep_ready_received = false;
static volatile bool sleep_deny_received = false;
static bool sleep_handshake_fail_link = false;
static unsigned long sleep_deny_retry_ms = 0;
static unsigned long sleep_deny_received_ms = 0;
static char sleep_deny_reason[24] = "";
static bool sleep_deny_active = false;
static uint8_t sleep_deny_count = 0;
static const uint8_t SLEEP_DENY_MAX_COUNT = 10;
static bool sleep_wait_for_sense_idle = false;
static bool sleep_cancelled_by_user_input = false;
static uint32_t sleep_fallback_timer_sec = 0;
static bool sense_ota_active = false;
// Forward-declared; true when LCD OTA binary transfer is active (set by lcd_ota_uart.h).
// Used by sleep_blocked_for_ota() which is defined before the lcd_ota_uart.h include.
static bool g_lcd_ota_uart_receiving = false;
static volatile bool sense_sleep_intent_pending = false;
static unsigned long sense_sleep_intent_received_ms = 0;
static const unsigned long SENSE_SLEEP_RETRY_INTERVAL_MS = 1000;
static const unsigned long SLEEP_HANDSHAKE_RETRY_DELAY_MS = 800;
static const uint8_t SLEEP_HANDSHAKE_MAX_ATTEMPTS = 3;
static const uint32_t SLEEP_DENY_RETRY_DEFAULT_MS = 5000;
static const uint32_t SLEEP_FALLBACK_TIMER_SEC = 15;
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
// Soft-fail auto-retry budget (max 3 while the user is on the shopping list).
// File-scope (not local to refresh_soft_fail) so the UI_LIST handler in
// lcd_uart_rx.h can reset it whenever a fresh list actually lands.
static uint8_t s_list_auto_retry_count = 0;
static const unsigned long REFRESH_TOTAL_TIMEOUT_MS = 25000;    // 25s — Sense WiFi connect (~10s) + API (~3s) + margin
static const unsigned long REFRESH_PROOF_OF_LIFE_TIMEOUT_MS = 20000; // 20s — generous window for proof after INFLIGHT
static const unsigned long REFRESH_TOTAL_MAX_MS = 35000;        // 35s hard max
static const unsigned long REFRESH_NO_UI_POL_TIMEOUT_MS = 25000; // 25s — no UI poll timeout
static const unsigned long REFRESH_MAX_MS = 30000;  // 30s — Sense needs boot (~5s) + WiFi connect (~15s) + API call (~3s)
static const unsigned long REFRESH_HARD_TIMEOUT_MS = 20000;    // 20s unified hard cap on a refresh (WAKE_PENDING+INFLIGHT) — recover from stuck "Refreshing" without killing legit slow-WiFi fetches
static const unsigned long REFRESH_KEEPAWAKE_MAX_MS = 30000;    // 30s — keep the device awake while a refresh is in flight, capped from the refresh start
static const unsigned long REFRESH_FAILED_SHOW_MS = 2000;
static const unsigned long REFRESH_COMPLETE_SHOW_MS = 500;
static const unsigned long REFRESH_PULSE_COOLDOWN_MS = 1500;
static const unsigned long REFRESH_PULSE_NO_POL_MS = 1500;
static const uint8_t REFRESH_PULSE_MAX = 2;
static const unsigned long REFRESH_WAKE_WAIT_MS = 5000;         // 5s per attempt (Sense needs up to 12s to boot)
static const unsigned long REFRESH_WAKE_PING_INTERVAL_MS = 400;
static const unsigned long SENSE_CONTROL_READY_WINDOW_MS = 2000;
static const unsigned long REFRESH_WAKE_PULSE_BACKOFF_MS = 1500;
static const uint8_t REFRESH_WAKE_MAX_ATTEMPTS = 5;             // 5 attempts (total ~25s window for Sense boot)
static unsigned long last_sense_msg_ms = 0;
static unsigned long last_proof_of_life_ms = 0;
static bool sense_rx_stale_logged = false;
static int lcd_wake_pin_mode = INPUT;
static int lcd_wake_pin_pullup = 0;

static uint32_t safe_age_ms(uint32_t now_ms, uint32_t then_ms);
static volatile bool waiting_for_voice_response = false;  // Track if we're waiting for voice response
static const unsigned long VOICE_RESPONSE_TIMEOUT_MS = 30000;  // 30 seconds timeout when waiting for voice
static unsigned long voice_response_deadline_ms = 0;
// Voice is fire-and-forget on LCD: after the local acknowledgement, late
// voice-only UI responses are ignored so the user can continue using the UI.
static volatile bool g_voice_fire_and_forget_ignore_ui = false;
static volatile bool waiting_for_scan_response = false;  // Track if we're waiting for scan response
static const unsigned long SCAN_RESPONSE_TIMEOUT_MS = 90000;  // 90 seconds timeout when waiting for scan (image processing can take time)
static unsigned long scan_request_sent_ms = 0;
static const unsigned long SCAN_NO_RESPONSE_TIMEOUT_MS = 30000;  // 30s timeout if Sense never responds to capture request
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
static const unsigned long MEAL_RESULT_TIMEOUT_MS = 10000;  // 10 seconds timeout for meal result screen
static volatile bool provisioning_active = false;  // Suppress sleep while provisioning UI is active
static volatile bool provision_qr_waiting = false;
static volatile bool provision_qr_exit_headless = false;
static unsigned long provision_qr_wait_start_ms = 0;
static const unsigned long PROVISION_QR_WAIT_TIMEOUT_MS = 12000;
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
static volatile bool g_ota_screen_active = false;  // OTA status screen is showing — block UI overwrite
static unsigned long ota_lock_at_ms = 0;
static unsigned long ota_unlock_received_ms = 0;
static const unsigned long OTA_LOCK_TIMEOUT_MS = 1800000; // 30 min auto-unlock (covers sense OTA + reboot)
static const unsigned long OTA_UNLOCK_GRACE_MS = 45000;   // keep LCD awake after unlock
static volatile bool ota_check_requested = false;
static volatile bool ota_check_pending = false;
static unsigned long ota_stay_awake_until_ms = 0;
static const unsigned long OTA_STAY_AWAKE_MS = 20000;
// Set by the OTA_LOCK handler (= millis() + LCD_OTA_LOCK_STAY_AWAKE_MS). While
// millis() < this value a genuine dual-board OTA is in progress: the Sense sends
// OTA_UNLOCK *before* its self-OTA reboot, so during that ~15s reboot the LCD
// sees sense_state == SENSE_ASLEEP && !ota_locked and the "missed-OTA race
// guard" would otherwise cancel ota_stay_awake_until_ms and deep-sleep — making
// the LCD UART-unreachable for the post-reboot LCD_OTA_QUERY/proxy
// (lcd_query_fail). The guards check this window and KEEP the stay-awake when a
// fresh OTA_LOCK window is live. Cleared once the proxy actually starts
// (LCD_OTA_BEGIN) and on the post-OTA restore/reboot path. A stale stay-awake
// (no recent OTA_LOCK) leaves this at 0 and is still canceled as before.
static unsigned long g_ota_lock_window_until_ms = 0;
static bool lcd_ota_request_active = false;
static uint32_t lcd_ota_request_id = 0;
static bool lcd_ota_request_allow_reboot = true;
static char lcd_ota_request_reason[24] = "";
static unsigned long lcd_ota_request_start_ms = 0;
static const unsigned long LCD_OTA_CHECK_STAY_AWAKE_MS = 600000;  // 10 min
// Stay-awake window applied on OTA_LOCK. Must cover the entire dual-board OTA
// sequence: Sense self-OTA download (~40s) + reboot (~15s) + boot/wifi/proxy
// start (~20s). Keeps the LCD UART-responsive so the Sense's post-reboot
// LCD_OTA_QUERY gets a reply instead of timing out (lcd_query_fail). The LCD
// will sleep sooner once the proxy actually starts (transfer keeps it busy).
static const unsigned long LCD_OTA_LOCK_STAY_AWAKE_MS = 180000;   // 3 min
static const unsigned long LCD_OTA_USER_ACTIVE_GRACE_MS = 120000; // 2 min
static volatile bool g_manual_ota_override = false;
static unsigned long g_manual_ota_override_until_ms = 0;
static const unsigned long MANUAL_OTA_OVERRIDE_TTL_MS = 5UL * 60UL * 1000UL;

static void lcd_manual_ota_override_set(const char* reason) {
  g_manual_ota_override = true;
  g_manual_ota_override_until_ms = millis() + MANUAL_OTA_OVERRIDE_TTL_MS;
  Serial.printf("[OTA_MANUAL] override=1 reason=%s\n", reason ? reason : "unknown");
}

static bool lcd_ota_request_allows_outside_maintenance() {
  if (!lcd_ota_request_active) {
    return false;
  }
  return strcmp(lcd_ota_request_reason, "manual") == 0 ||
         strcmp(lcd_ota_request_reason, "scheduled_http") == 0 ||
         strcmp(lcd_ota_request_reason, "maintenance") == 0;
}

static bool lcd_manual_ota_override_active() {
  if (!g_manual_ota_override) {
    return false;
  }
  if (g_manual_ota_override_until_ms > 0 && millis() > g_manual_ota_override_until_ms) {
    g_manual_ota_override = false;
    g_manual_ota_override_until_ms = 0;
    Serial.println("[OTA_MANUAL] override expired");
    return false;
  }
  return true;
}

static void lcd_manual_ota_override_clear(const char* reason) {
  if (!g_manual_ota_override) {
    return;
  }
  g_manual_ota_override = false;
  g_manual_ota_override_until_ms = 0;
  Serial.printf("[OTA_MANUAL] override cleared reason=%s\n", reason ? reason : "unknown");
}

// ── Scroll Position Tracking ──────────────────────────────────────────
// Track if user has scrolled since last list update (to prevent disrupting scroll)
static volatile bool user_has_scrolled = false;

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
static bool touch_wake_only_pending = false;  // First tap on idle-dark screen wakes only

// ── UI Elements ────────────────────────────────────────────────────
static lv_obj_t *list_container = NULL;
static lv_obj_t *list_labels[MAX_LIST_ITEMS] = {NULL};
static lv_obj_t *empty_label = NULL;
static lv_obj_t *loading_screen = NULL;
static lv_obj_t *g_base_screen = NULL;
// ── Ship Menu Screen ────────────────────────────────────────────────
static lv_obj_t *ship_menu_screen = NULL;
static lv_obj_t *ship_menu_label = NULL;
static lv_obj_t *ship_main_menu_buttons[4] = {NULL};
static lv_obj_t *ship_main_menu_ai_button = NULL;
static lv_obj_t *ship_main_menu_ai_label = NULL;
static lv_obj_t *ship_menu_second_screen = NULL;
static lv_obj_t *ship_menu_settings_screen = NULL;
static lv_obj_t *ship_menu_settings_title = NULL;
static lv_obj_t *ship_menu_settings_versions = NULL;
static lv_obj_t *ship_menu_settings_btn_reset = NULL;
static lv_obj_t *ship_menu_settings_label_reset = NULL;
static lv_obj_t *ship_menu_settings_btn_backlight = NULL;
static lv_obj_t *ship_menu_settings_label_backlight = NULL;
static lv_obj_t *ship_menu_settings_btn_ota = NULL;
static lv_obj_t *ship_menu_settings_label_ota = NULL;
static lv_obj_t *ship_menu_settings_btn_back = NULL;
static lv_obj_t *ship_menu_settings_label_back = NULL;
static lv_obj_t *ship_menu_settings_status = NULL;
static unsigned long ship_menu_settings_status_hide_at_ms = 0;
static char g_sense_fw_version[32] = "--";
static char g_lcd_device_id[32] = "";
static unsigned long g_fw_info_request_ms = 0;
static unsigned long g_fw_info_last_attempt_ms = 0;
static unsigned long g_fw_info_retry_deadline_ms = 0;
static uint8_t g_fw_info_retry_count = 0;
static bool g_fw_info_response_received = false;
static const unsigned long FW_INFO_RETRY_INTERVAL_MS = 800;
static const unsigned long FW_INFO_RETRY_TIMEOUT_MS = 5000;
static lv_obj_t *ship_hold_screen = NULL;
static lv_obj_t *ship_processing_screen = NULL;
static lv_obj_t *ship_ai_listening_screen = NULL;
static lv_obj_t *ship_ai_listening_ring = NULL;
static lv_obj_t *ship_backlight_screen = NULL;
static lv_obj_t *ship_backlight_ring = NULL;
static lv_obj_t *ship_backlight_pct_label = NULL;
static lv_obj_t *ship_ai_listening_title = NULL;
static lv_obj_t *ship_ai_listening_hint = NULL;
static lv_obj_t *ship_ai_listening_mic_head = NULL;
static lv_obj_t *ship_ai_listening_mic_stem = NULL;
static lv_obj_t *ship_ai_listening_mic_base = NULL;
static lv_obj_t *ship_ai_listening_mic_disc = NULL;
static lv_obj_t *ship_ai_listening_pulse[3] = {NULL, NULL, NULL};
static unsigned long ship_ai_listening_countdown_start_ms = 0;
static const unsigned long SHIP_AI_LISTENING_COUNTDOWN_MS = 10000;
static lv_obj_t *ship_voice_json_screen = NULL;
static lv_obj_t *ship_voice_json_card = NULL;
static lv_obj_t *ship_voice_json_title = NULL;
static lv_obj_t *ship_voice_json_subtitle = NULL;
static lv_obj_t *ship_voice_json_label = NULL;
static lv_obj_t *ship_voice_json_footer = NULL;
static lv_obj_t *ship_voice_json_page_label = NULL;
static lv_obj_t *ship_voice_json_hint = NULL;
static lv_obj_t *ship_expiry_choice_screen = NULL;
static lv_obj_t *ship_expiry_choice_prompt = NULL;
static lv_obj_t *ship_expiry_choice_timeout_ring = NULL;
static lv_obj_t *ship_expiry_choice_qty_prefix = NULL;
static lv_obj_t *ship_expiry_choice_add_caption = NULL;
static lv_obj_t *ship_expiry_choice_skip_btn = NULL;
static lv_obj_t *ship_expiry_choice_skip_label = NULL;
static lv_obj_t *ship_expiry_choice_add_btn = NULL;
static lv_obj_t *ship_expiry_choice_add_label = NULL;
static lv_obj_t *ship_processing_fill = NULL;
static lv_obj_t *ship_processing_halo = NULL;
static lv_obj_t *ship_processing_spinner = NULL;
static lv_obj_t *ship_processing_subtitle = NULL;
static lv_obj_t *ship_processing_label = NULL;
static lv_obj_t *ship_hold_title = NULL;
static lv_obj_t *ship_hold_subtitle = NULL;
static lv_obj_t *ship_hold_ring = NULL;
static lv_obj_t *ship_hold_countdown_label = NULL;
static lv_obj_t *ship_hold_capture_icon = NULL;
static lv_obj_t *ship_voice_ack_screen = NULL;
static lv_obj_t *ship_voice_ack_icon = NULL;
static lv_obj_t *ship_voice_ack_label = NULL;
static lv_obj_t *ship_voice_ack_subtitle = NULL;
static lv_obj_t *ship_logged_screen = NULL;
static lv_obj_t *ship_logged_icon = NULL;
static lv_obj_t *ship_logged_label = NULL;
static lv_obj_t *ship_logged_subtitle = NULL;
static lv_obj_t *ship_error_screen = NULL;
static lv_obj_t *ship_error_label = NULL;
static unsigned long ship_hold_anim_start_ms = 0;
static int ship_hold_countdown_value = -1;
static int ship_hold_capture_frame = -1;
static bool ship_hold_capture_phase = false;
static const unsigned long SHIP_HOLD_COUNTDOWN_MS = 3000UL;
static const unsigned long SHIP_HOLD_CAPTURE_PULSE_MS = 1000UL;
static unsigned long ship_expiry_choice_shown_time = 0;
static unsigned long ship_voice_ack_hide_at_ms = 0;
static unsigned long ship_logged_hide_at_ms = 0;
static unsigned long ship_error_hide_at_ms = 0;
static lv_obj_t *ship_menu_press_overlay = NULL;
static unsigned long ship_menu_press_hide_at_ms = 0;
static lv_obj_t *g_status_layer = NULL;
static lv_obj_t *g_status_label = NULL;
static lv_obj_t *g_status_spinner = NULL;
static unsigned long g_status_hide_at_ms = 0;
static ship_menu_screen_t ship_menu_screen_state = SHIP_MENU_SCREEN_MAIN;
static bool ship_ai_touch_active = false;
static uint8_t ship_voice_end_resends_remaining = 0;
static unsigned long ship_voice_end_resend_due_ms = 0;
static char g_ship_voice_json_text[4096] = "";
static volatile bool g_ship_voice_json_pending = false;
static volatile bool ui_busy = false;
static volatile ui_screen_t ui_screen_state = SCREEN_HOME;

#define SHIP_VOICE_UI_MAX_PAGES 6
#define SHIP_VOICE_UI_MAX_ITEMS 5

static const int SHIP_CHOICE_VALUE_Y = 28;
static const int SHIP_CHOICE_PROMPT_Y = 62;
static const int SHIP_CHOICE_BUTTON_Y = 20;
static const int SHIP_CHOICE_BUTTON_HEIGHT = 164;

typedef struct {
  bool valid;
  char id[24];
  char template_name[24];
  char style[16];
  char title[64];
  char subtitle[64];
  char body[512];
  char footer[96];
  char items[SHIP_VOICE_UI_MAX_ITEMS][80];
  uint8_t item_count;
} ship_voice_ui_page_t;

static lv_obj_t *ship_voice_json_item_labels[SHIP_VOICE_UI_MAX_ITEMS] = {NULL};
static lv_obj_t *ship_voice_json_page_dots[SHIP_VOICE_UI_MAX_PAGES] = {NULL};
static ship_voice_ui_page_t g_ship_voice_ui_pages[SHIP_VOICE_UI_MAX_PAGES];
static uint8_t g_ship_voice_ui_page_count = 0;
static uint8_t g_ship_voice_ui_page_index = 0;
static bool g_ship_voice_ui_wrap = false;
static bool g_ship_voice_ui_show_page_dots = true;
static bool g_ship_voice_ui_show_page_count = false;
static bool g_ship_voice_ui_structured = false;

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
static char g_ship_ui_policy[16] = {0};
static unsigned long g_ship_ui_last_ms = 0;
static uint32_t g_ship_ui_job_id = 0;
static uint32_t g_ship_ui_finalized_job_id = 0;
static bool g_ship_ui_finalized = false;
static bool g_ship_ui_dirty = false;
static uint32_t g_ship_ui_applied_msg_id = 0;
static uint32_t g_last_ui_status_job_id = 0;
static char g_last_ui_status_phase[16] = {0};
static char g_last_ui_status_op[16] = {0};
static char g_last_ui_status_mode[16] = {0};
static uint32_t g_last_ui_status_msg_id = 0;
static const char* ui_screen_from_ptr(lv_obj_t* scr);

typedef struct {
  bool is_error;
  char title[64];
  char mode[16];
} result_args_t;

static result_args_t g_result_pending = {false, "", ""};
static ScreenId g_current_screen_id = SCREEN_UNKNOWN;

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
#define MENU_SETTINGS_ITEM_COUNT 3
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
  "Run OTA Update",
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
#define SHIP_MAIN_MENU_BTN_W 92
#define SHIP_MAIN_MENU_BTN_H 92
#define SHIP_MAIN_MENU_BTN_RADIUS 26
#define SHIP_MENU_MAIN_TOP_X (SHIP_MENU_MID_X - (SHIP_MENU_MAIN_ICON_W / 2))
#define SHIP_MENU_MAIN_TOP_Y SHIP_MENU_MAIN_PAD
#define SHIP_MENU_MAIN_LEFT_X SHIP_MENU_MAIN_PAD
#define SHIP_MENU_MAIN_LEFT_Y (SHIP_MENU_MID_Y - (SHIP_MENU_MAIN_ICON_H / 2))
#define SHIP_MENU_MAIN_RIGHT_X (SHIP_MENU_W - SHIP_MENU_MAIN_PAD - SHIP_MENU_MAIN_ICON_W)
#define SHIP_MENU_MAIN_RIGHT_Y (SHIP_MENU_MID_Y - (SHIP_MENU_MAIN_ICON_H / 2))
#define SHIP_MENU_MAIN_BOTTOM_X (SHIP_MENU_MID_X - (SHIP_MENU_MAIN_ICON_W / 2))
#define SHIP_MENU_MAIN_BOTTOM_Y (SHIP_MENU_H - SHIP_MENU_MAIN_PAD - SHIP_MENU_MAIN_ICON_H)
#define SHIP_MAIN_MENU_BTN_INSET_X ((SHIP_MENU_MAIN_ICON_W - SHIP_MAIN_MENU_BTN_W) / 2)
#define SHIP_MAIN_MENU_BTN_INSET_Y ((SHIP_MENU_MAIN_ICON_H - SHIP_MAIN_MENU_BTN_H) / 2)
#define SHIP_MAIN_MENU_TOP_BTN_X (SHIP_MENU_MAIN_TOP_X + SHIP_MAIN_MENU_BTN_INSET_X)
#define SHIP_MAIN_MENU_TOP_BTN_Y (SHIP_MENU_MAIN_TOP_Y + SHIP_MAIN_MENU_BTN_INSET_Y)
#define SHIP_MAIN_MENU_LEFT_BTN_X (SHIP_MENU_MAIN_LEFT_X + SHIP_MAIN_MENU_BTN_INSET_X)
#define SHIP_MAIN_MENU_LEFT_BTN_Y (SHIP_MENU_MAIN_LEFT_Y + SHIP_MAIN_MENU_BTN_INSET_Y)
#define SHIP_MAIN_MENU_RIGHT_BTN_X (SHIP_MENU_MAIN_RIGHT_X + SHIP_MAIN_MENU_BTN_INSET_X)
#define SHIP_MAIN_MENU_RIGHT_BTN_Y (SHIP_MENU_MAIN_RIGHT_Y + SHIP_MAIN_MENU_BTN_INSET_Y)
#define SHIP_MAIN_MENU_BOTTOM_BTN_X (SHIP_MENU_MAIN_BOTTOM_X + SHIP_MAIN_MENU_BTN_INSET_X)
#define SHIP_MAIN_MENU_BOTTOM_BTN_Y (SHIP_MENU_MAIN_BOTTOM_Y + SHIP_MAIN_MENU_BTN_INSET_Y)
#define SHIP_MAIN_MENU_CENTER_BTN_X (SHIP_MENU_MID_X - (SHIP_MAIN_MENU_BTN_W / 2))
#define SHIP_MAIN_MENU_CENTER_BTN_Y (SHIP_MENU_MID_Y - (SHIP_MAIN_MENU_BTN_H / 2))

#define SHIP_MENU_MAIN_CENTER_HITBOX_W SHIP_MAIN_MENU_BTN_W
#define SHIP_MENU_MAIN_CENTER_HITBOX_H SHIP_MAIN_MENU_BTN_H
#define SHIP_MENU_MAIN_CENTER_X SHIP_MAIN_MENU_CENTER_BTN_X
#define SHIP_MENU_MAIN_CENTER_Y SHIP_MAIN_MENU_CENTER_BTN_Y

// Second Menu icon bounds (HOME center, SETTINGS bottom)
#define SHIP_MENU_SECOND_HOME_W 130
#define SHIP_MENU_SECOND_HOME_H 130
#define SHIP_MENU_SECOND_HOME_X (SHIP_MENU_MID_X - (SHIP_MENU_SECOND_HOME_W / 2))
#define SHIP_MENU_SECOND_HOME_Y (SHIP_MENU_MID_Y - (SHIP_MENU_SECOND_HOME_H / 2))
#define SHIP_MENU_SECOND_SETTINGS_W SHIP_MENU_MAIN_ICON_W
#define SHIP_MENU_SECOND_SETTINGS_H SHIP_MENU_MAIN_ICON_H
#define SHIP_MENU_SECOND_SETTINGS_X SHIP_MENU_MAIN_BOTTOM_X
#define SHIP_MENU_SECOND_SETTINGS_Y SHIP_MENU_MAIN_BOTTOM_Y

// Second Menu — Shopping List button (TOP position)
#define SHIP_MENU_SECOND_LIST_W SHIP_MENU_MAIN_ICON_W
#define SHIP_MENU_SECOND_LIST_H SHIP_MENU_MAIN_ICON_H
#define SHIP_MENU_SECOND_LIST_X SHIP_MENU_MAIN_TOP_X
#define SHIP_MENU_SECOND_LIST_Y SHIP_MENU_MAIN_TOP_Y

// Settings screen button bounds (four buttons, fit within round 360x360 display)
#define SHIP_MENU_SETTINGS_BTN_W 220
#define SHIP_MENU_SETTINGS_BTN_H 54
#define SHIP_MENU_SETTINGS_BTN_X ((SHIP_MENU_W - SHIP_MENU_SETTINGS_BTN_W) / 2)
#define SHIP_MENU_SETTINGS_RESET_Y 38
#define SHIP_MENU_SETTINGS_BACKLIGHT_Y 108
#define SHIP_MENU_SETTINGS_OTA_Y 178
#define SHIP_MENU_SETTINGS_BACK_Y 248
#define SHIP_MENU_SETTINGS_VERSION_Y 324

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
   SHIP_MENU_MAIN_BOTTOM_X + SHIP_MENU_MAIN_ICON_W - 1, SHIP_MENU_MAIN_BOTTOM_Y + SHIP_MENU_MAIN_ICON_H - 1},
  {SHIP_MENU_ACTION_AI,        "AI",        NULL,       -1,
   SHIP_MENU_MAIN_CENTER_X, SHIP_MENU_MAIN_CENTER_Y,
   SHIP_MENU_MAIN_CENTER_X + SHIP_MENU_MAIN_CENTER_HITBOX_W - 1, SHIP_MENU_MAIN_CENTER_Y + SHIP_MENU_MAIN_CENTER_HITBOX_H - 1}
};

static const ship_menu_hitbox_t ship_menu_hitboxes_second[] = {
  {SHIP_MENU_ACTION_SHOPPING_LIST, "SHOPPING_LIST", NULL, -1,
   SHIP_MENU_SECOND_LIST_X, SHIP_MENU_SECOND_LIST_Y,
   SHIP_MENU_SECOND_LIST_X + SHIP_MENU_SECOND_LIST_W - 1, SHIP_MENU_SECOND_LIST_Y + SHIP_MENU_SECOND_LIST_H - 1},
  {SHIP_MENU_ACTION_HOME,     "HOME",     NULL, -1,
   SHIP_MENU_SECOND_HOME_X, SHIP_MENU_SECOND_HOME_Y,
   SHIP_MENU_SECOND_HOME_X + SHIP_MENU_SECOND_HOME_W - 1, SHIP_MENU_SECOND_HOME_Y + SHIP_MENU_SECOND_HOME_H - 1},
  {SHIP_MENU_ACTION_SETTINGS, "SETTINGS", NULL, -1,
   SHIP_MENU_SECOND_SETTINGS_X, SHIP_MENU_SECOND_SETTINGS_Y,
   SHIP_MENU_SECOND_SETTINGS_X + SHIP_MENU_SECOND_SETTINGS_W - 1, SHIP_MENU_SECOND_SETTINGS_Y + SHIP_MENU_SECOND_SETTINGS_H - 1}
};

static const ship_menu_hitbox_t ship_menu_hitboxes_settings[] = {
  {SHIP_MENU_ACTION_RESET_WIFI, "RESET_WIFI", NULL, -1,
   SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_RESET_Y,
   SHIP_MENU_SETTINGS_BTN_X + SHIP_MENU_SETTINGS_BTN_W - 1, SHIP_MENU_SETTINGS_RESET_Y + SHIP_MENU_SETTINGS_BTN_H - 1},
  {SHIP_MENU_ACTION_BACKLIGHT, "BACKLIGHT", NULL, -1,
   SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_BACKLIGHT_Y,
   SHIP_MENU_SETTINGS_BTN_X + SHIP_MENU_SETTINGS_BTN_W - 1, SHIP_MENU_SETTINGS_BACKLIGHT_Y + SHIP_MENU_SETTINGS_BTN_H - 1},
  {SHIP_MENU_ACTION_MANUAL_OTA, "MANUAL_OTA", NULL, -1,
   SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_OTA_Y,
   SHIP_MENU_SETTINGS_BTN_X + SHIP_MENU_SETTINGS_BTN_W - 1, SHIP_MENU_SETTINGS_OTA_Y + SHIP_MENU_SETTINGS_BTN_H - 1},
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
static lv_obj_t *expiry_title_label = NULL;
static lv_obj_t *expiry_date_label = NULL;  // Legacy alias for non-ship fallback paths
static lv_obj_t *expiry_month_button = NULL;
static lv_obj_t *expiry_month_label = NULL;
static lv_obj_t *expiry_day_button = NULL;
static lv_obj_t *expiry_day_label = NULL;
static lv_obj_t *expiry_year_button = NULL;
static lv_obj_t *expiry_year_label = NULL;
static lv_obj_t *expiry_hint_label = NULL;
static lv_obj_t *expiry_keypad_buttons[10] = {NULL};  // Number buttons 0-9
static lv_obj_t *expiry_check_button = NULL;  // Check button to submit
static lv_obj_t *expiry_backspace_button = NULL;  // Backspace button
static lv_obj_t *expiry_back_button = NULL;  // Back/cancel button
static lv_obj_t *expiry_back_label = NULL;
static lv_obj_t *expiry_timeout_ring = NULL;
static char expiry_date_buffer[11] = "__-__-____";  // Date buffer (MM-DD-YYYY format for display)
static int expiry_date_pos = 0;  // Current position in date (0-9, skipping dashes)
typedef enum {
  EXPIRY_SEGMENT_MONTH = 0,
  EXPIRY_SEGMENT_DAY = 1,
  EXPIRY_SEGMENT_YEAR = 2
} expiry_segment_t;
static expiry_segment_t expiry_active_segment = EXPIRY_SEGMENT_MONTH;
static int expiry_selected_year = 2026;
static int expiry_selected_month = 1;
static int expiry_selected_day = 1;
static bool expiry_submitted = false;
static bool expiry_screen_visible = false;  // Track if expiration date screen is showing
static unsigned long expiry_screen_shown_time = 0;  // Track when expiry screen was shown (for timeout)
static const unsigned long EXPIRY_SCREEN_TIMEOUT_MS = 30000;  // 30 seconds timeout (matches Sense board timeout)
static lv_obj_t *expiry_choice_quantity_label = NULL;
static int expiry_choice_quantity = 1;

// ── Long Press Halo ────────────────────────────────────────────────────
static lv_obj_t *recording_indicator = NULL;  // Solid halo/ring for long press recording

// ── Processing Halo (Glowing/Pulsing) ──────────────────────────────────────
static lv_obj_t *processing_indicator = NULL;  // Processing indicator (glowing halo)
static lv_anim_t *processing_anim = NULL;  // Animation for pulsing effect
static bool is_glowing_animation = false;  // Track if glowing animation is active
static unsigned long processing_start_ms = 0;
static char processing_op[24] = "none";
static const unsigned long PROCESSING_WATCHDOG_MS = 15000;
static bool dish_processing_active = false;
static unsigned long dish_processing_start_ms = 0;
static uint8_t dish_processing_progress_pct = 1;
// Failsafe only; Sense owns the normal dish-result timeout.
static const unsigned long DISH_PROCESSING_TIMEOUT_MS = 90000;
static const unsigned long DISH_PROCESSING_PROGRESS_TOTAL_MS = 15000;
static const unsigned long DISH_RESULT_LATE_GRACE_MS = 30000;
static unsigned long dish_timeout_at_ms = 0;
static uint32_t dish_timeout_job_id = 0;
static volatile unsigned long lvgl_timer_calls = 0;
static unsigned long last_ui_heartbeat_ms = 0;
static volatile unsigned long ui_heartbeat_counter = 0;
static int g_backlight_duty = 255;
static int g_user_brightness_duty = 255;  // 0..255, the "on" target (user-adjustable backlight level)
static bool g_panel_enabled = true;
static bool g_lvgl_running = true;
static TaskHandle_t ui_task_handle = NULL;
static volatile bool g_ui_task_exit_requested = false;
static bool g_ota_mode_active = false;

// ── Status Screen ──────────────────────────────────────────────────────────
static lv_obj_t *status_screen = NULL;  // Status screen for "On it!", "Hold still!", etc.
static lv_obj_t *status_label = NULL;  // Large text label for status messages
static lv_obj_t *status_reset_button = NULL;  // Reset Wi-Fi button on error screen
static lv_obj_t *status_reset_label = NULL;
static unsigned long status_screen_shown_time = 0;  // Track when status screen was shown
static unsigned long status_screen_auto_hide_at_ms = 0;
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
static lv_obj_t *provision_intro_screen = NULL;
static bool provision_intro_visible = false;
static bool provision_intro_pending = false;
static bool provision_intro_tapped = false;
static bool provision_qr_cached = false;
static bool provision_user_requested = false;
static bool provision_return_home_pending = false;
static char provision_qr_ssid[33] = {0};
static char provision_qr_password[65] = {0};
static char provision_qr_url[64] = {0};
static inline bool provisioning_input_locked() {
  return provisioning_active || provision_screen_visible || provision_intro_visible || provision_qr_waiting;
}

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
  EVT_SHOW_PROVISION_INTRO,
  EVT_HIDE_PROVISION_INTRO,
  EVT_RESET_UI,
  EVT_HAPTIC_TICK,
  EVT_STOP_GLOWING,   // From UART: UI task calls stop_glowing_animation + lv_timer_handler
  EVT_START_GLOWING,  // From UART: UI task calls start_glowing_animation(reason)
  EVT_UI_STATUS_IDLE, // From UART: UI task calls set_status_reset_visible(false), stop_glowing if needed
  EVT_REFRESH_TIMEOUT, // Refresh stuck inflight too long: stop glowing, show "Couldn't refresh", re-render existing list
  EVT_SHIP_UI_STATUS, // From UART: Ship menu UI_STATUS -> update overlay/result
  EVT_SHIP_UI_TOAST,
  EVT_SHIP_VOICE_JSON,
  // USB test-command injection (port 101). Posted by the Core-0 USB reader so the
  // real screen/list actions run on the UI task (Core 1) — never LVGL from Core 0.
  EVT_USB_ENTER_LIST,   // emulate tapping List on the second menu
  EVT_USB_REFRESH,      // emulate pull-to-refresh gesture on the list
  EVT_USB_PULL,         // emulate the touch pull-to-refresh path ("usb_pull" reason)
  EVT_USB_DELETE,       // emulate delete-touch on N-th visible item (data.usb_index)
  EVT_USB_DELTOUCH,     // full touch-path delete: open overlay, then tap the real Delete button
  EVT_USB_HOME,         // emulate returning to the main menu
  EVT_USB_LISTSTATE,    // print one [LISTSTATE] JSON line from the UI task (e2e harness)
} app_event_type_t;

typedef struct app_event_t {
  app_event_type_t type;
  union {
    int8_t scroll_delta;
    int new_count;
    int menu_index;  // For EVT_MENU_SELECTED
    int usb_index;   // For EVT_USB_DELETE (0-based visible-item index)
    char glow_reason[32];  // For EVT_START_GLOWING
    char ship_toast[64];
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
    struct {
      char op[16];
      char mode[16];
      char phase[16];
      char text[64];
      char ui_policy[16];
      uint32_t job_id;
      uint32_t msg_id;
    } ship_ui_status;
  } data;
} app_event_t;

static QueueHandle_t app_event_queue = NULL;

#include "lcd_uart.h"

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

static bool lcd_wifi_allowed_for_ota(void) {
  return (lcd_maintenance_active() || g_ota_mode_active);
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
  if (!lcd_wifi_allowed_for_ota()) {
    Serial.printf("[LCD_WIFI] WIFI_ON ignored (ota_only) source=%s\n",
                  source ? source : "unknown");
    return;
  }
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
               provision_qr_waiting || user_activity_since_sleep) {
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

// Forward declarations — functions defined later in this .ino
static void ship_menu_handle_ui_status(const JsonDocument& doc);
static void ui_apply_ship_meal_result(const JsonDocument& doc);
static void deferred_awake_tx_service();
static void cancel_pending_sleep_for_user_input(const char* reason);
static void send_sense_ping();
static void start_sense_wake_handshake();

static void lcd_send_diag_pre_sleep() {
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "LCD_DIAG";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  const char* wake = lcd_wake_cause_label();
  if (wake && wake[0]) {
    doc["wake"] = wake;
  }
  doc["screen"] = ui_screen_state_name(ui_screen_state);
  doc["uart_tx"] = uart_tx_count;
  doc["uart_rx"] = uart_rx_count;
  if (last_uart_tx_type[0]) {
    doc["last_tx"] = last_uart_tx_type;
  }
  if (last_uart_rx_type[0]) {
    doc["last_rx"] = last_uart_rx_type;
  }
  if (last_input_type[0]) {
    doc["input"] = last_input_type;
    unsigned long now = millis();
    doc["input_age_ms"] = (last_input_type_ms > 0) ? (long)(now - last_input_type_ms) : -1;
  }
  unsigned long now_ms = millis();
  if (last_sense_any_rx_ms > 0) {
    doc["sense_rx_age_ms"] = (long)(now_ms - last_sense_any_rx_ms);
  }
  String output;
  serializeJson(doc, output);
  uart_send_json(output.c_str());
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

extern "C" void lcd_ota_on_success(const char* version) {
  const char* reported = (version && version[0])
                             ? version
                             : (kFirmwareVersion ? kFirmwareVersion : "");
  lcd_send_ota_done("updated", reported);
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
  Serial.printf("[LCD_OTA_REQ] finish request_id=%lu result=%s detail=%s err=%s maint_active=%d remaining_ms=%lu\n",
                (unsigned long)lcd_ota_request_id,
                result ? result : "-",
                detail ? detail : "-",
                err_code ? err_code : "-",
                g_lcd_maintenance_active ? 1 : 0,
                (unsigned long)lcd_maintenance_remaining_ms());
  lcd_send_ota_check_result(lcd_ota_request_id, result, detail, new_version, err_code);
  ota_check_requested = false;
  ota_check_pending = false;
  ota_stay_awake_until_ms = 0;
  lcd_ota_request_active = false;
  lcd_ota_request_id = 0;
  lcd_ota_request_allow_reboot = true;
  lcd_ota_request_reason[0] = '\0';
  lcd_ota_request_start_ms = 0;
}

static void lcd_finish_maintenance(const char* result) {
  if (!g_lcd_maintenance_active) {
    return;
  }
  Serial.printf("[LCD_MAINT] finish result=%s wake_window=%d timer_armed=%d wake_in_s=%lu remaining_ms=%lu ota_mode=%d\n",
                result ? result : "unknown",
                g_lcd_maintenance_wake_window ? 1 : 0,
                g_lcd_maintenance_timer_armed ? 1 : 0,
                (unsigned long)g_lcd_maintenance_wake_in_s,
                (unsigned long)lcd_maintenance_remaining_ms(),
                g_ota_mode_active ? 1 : 0);
  lcd_send_ota_done(result, kFirmwareVersion ? kFirmwareVersion : "");
  g_lcd_maintenance_active = false;
  g_lcd_maintenance_started = false;
  g_lcd_maintenance_aborted = false;
  g_lcd_maintenance_headless = false;
  g_lcd_maintenance_deadline_ms = 0;
  g_lcd_maintenance_completed_ms = millis();
  ota_stay_awake_until_ms = 0;
  lcd_mode = LCD_MODE_UI_ACTIVE;
  lcd_clear_persisted_maintenance_state("maintenance_finish");
  if (g_ota_mode_active) {
    lcd_exit_ota_mode(result ? result : "maintenance_done");
  }
}

static void lcd_clear_maintenance_state(const char* reason, bool mark_completed) {
  g_lcd_maintenance_active = false;
  g_lcd_maintenance_started = false;
  g_lcd_maintenance_aborted = false;
  g_lcd_maintenance_headless = false;
  g_lcd_maintenance_deadline_ms = 0;
  g_lcd_maintenance_timer_armed = 0;
  g_lcd_maintenance_wake_in_s = 0;
  g_lcd_maintenance_remaining_s = 0;
  if (mark_completed) {
    g_lcd_maintenance_completed_ms = millis();
  }
  ota_stay_awake_until_ms = 0;
  lcd_mode = LCD_MODE_UI_ACTIVE;
  lcd_clear_persisted_maintenance_state(reason ? reason : "clear_state");
  Serial.printf("[LCD_MAINT] clear_state reason=%s completed=%d\n",
                reason ? reason : "unknown",
                mark_completed ? 1 : 0);
}

// Exit headless mode but PRESERVE the maintenance wake timer.
// Used when user touches screen during headless maintenance — screen comes
// back on, but the timer still fires so both boards wake for OTA.
static void lcd_exit_maintenance_headless_only(const char* reason) {
  g_lcd_maintenance_headless = false;
  g_lcd_maintenance_active = false;
  g_lcd_maintenance_started = false;
  g_lcd_maintenance_aborted = false;
  lcd_mode = LCD_MODE_UI_ACTIVE;
  ota_stay_awake_until_ms = 0;
  sleep_deny_received = false;
  // Mark completed so we don't re-enter headless on retry messages
  g_lcd_maintenance_completed_ms = millis();
  // DO NOT zero timer_armed, wake_in_s, remaining_s, or deadline_ms
  // DO NOT call lcd_clear_persisted_maintenance_state()
  Serial.printf("[LCD_MAINT] exit_headless_only reason=%s timer_armed=%d wake_in_s=%lu\n",
                reason ? reason : "unknown",
                g_lcd_maintenance_timer_armed,
                (unsigned long)g_lcd_maintenance_wake_in_s);
}

static void lcd_uart_reset_rx_state() {
  uart_rx_line_pos = 0;
  uart_rx_line_buffer[0] = '\0';
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

static bool expiry_is_leap_year(int year) {
  if ((year % 400) == 0) return true;
  if ((year % 100) == 0) return false;
  return (year % 4) == 0;
}

static int expiry_days_in_month(int year, int month) {
  static const int kDaysPerMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1) month = 1;
  if (month > 12) month = 12;
  if (month == 2 && expiry_is_leap_year(year)) {
    return 29;
  }
  return kDaysPerMonth[month - 1];
}

static void expiry_clamp_selected_day() {
  int max_day = expiry_days_in_month(expiry_selected_year, expiry_selected_month);
  if (expiry_selected_day < 1) {
    expiry_selected_day = 1;
  } else if (expiry_selected_day > max_day) {
    expiry_selected_day = max_day;
  }
}

static void expiry_step_day(int delta) {
  if (delta == 0) {
    return;
  }
  int step = (delta > 0) ? 1 : -1;
  int remaining = (delta > 0) ? delta : -delta;
  int max_day = expiry_days_in_month(expiry_selected_year, expiry_selected_month);
  while (remaining-- > 0) {
    expiry_selected_day += step;
    if (expiry_selected_day > max_day) {
      expiry_selected_day = 1;
    } else if (expiry_selected_day < 1) {
      expiry_selected_day = max_day;
    }
  }
}

static void expiry_step_month(int delta) {
  if (delta == 0) {
    return;
  }
  int next = expiry_selected_month + delta;
  while (next > 12) {
    next -= 12;
  }
  while (next < 1) {
    next += 12;
  }
  expiry_selected_month = next;
  expiry_clamp_selected_day();
}

static void expiry_step_year(int delta) {
  if (delta == 0) {
    return;
  }
  expiry_selected_year += delta;
  if (expiry_selected_year < 2020) {
    expiry_selected_year = 2020;
  } else if (expiry_selected_year > 2099) {
    expiry_selected_year = 2099;
  }
  expiry_clamp_selected_day();
}

static void expiry_format_selected_date(char* output, size_t output_size) {
  if (!output || output_size < 11) {
    return;
  }
  snprintf(output, output_size, "%04d-%02d-%02d",
           expiry_selected_year,
           expiry_selected_month,
           expiry_selected_day);
}

static void expiry_apply_segment_style(lv_obj_t* button, lv_obj_t* label, bool active) {
  if (!button || !label) {
    return;
  }
  lv_color_t bg = active ? lv_color_hex(0x1F4D2B) : lv_color_hex(0xFFFFFF);
  lv_color_t border = active ? lv_color_hex(0x1F4D2B) : lv_color_hex(0x1A1A1A);
  lv_color_t text = active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x1A1A1A);
  lv_obj_set_style_bg_color(button, bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, border, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, active ? 0 : 2, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, text, LV_PART_MAIN);
}

static void expiry_refresh_picker_ui() {
  static const char* kMonthNames[12] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (!expiry_month_label || !expiry_day_label || !expiry_year_label) {
    return;
  }
  const char* month_name = kMonthNames[(expiry_selected_month >= 1 && expiry_selected_month <= 12)
                                           ? (expiry_selected_month - 1)
                                           : 0];
  char day_text[12];
  char year_text[12];
  snprintf(day_text, sizeof(day_text), "%02d", expiry_selected_day);
  snprintf(year_text, sizeof(year_text), "%04d", expiry_selected_year);
  lv_label_set_text(expiry_month_label, month_name);
  lv_label_set_text(expiry_day_label, day_text);
  lv_label_set_text(expiry_year_label, year_text);
  expiry_apply_segment_style(expiry_month_button,
                             expiry_month_label,
                             expiry_active_segment == EXPIRY_SEGMENT_MONTH);
  expiry_apply_segment_style(expiry_day_button,
                             expiry_day_label,
                             expiry_active_segment == EXPIRY_SEGMENT_DAY);
  expiry_apply_segment_style(expiry_year_button,
                             expiry_year_label,
                             expiry_active_segment == EXPIRY_SEGMENT_YEAR);
}

static void expiry_reset_picker_default() {
  expiry_active_segment = EXPIRY_SEGMENT_MONTH;
  expiry_selected_year = 2026;
  expiry_selected_month = 1;
  expiry_selected_day = 1;
  if (lcd_time_valid()) {
    time_t now = time(nullptr);
    struct tm local_tm = {};
    if (localtime_r(&now, &local_tm) != NULL) {
      expiry_selected_year = local_tm.tm_year + 1900;
      expiry_selected_month = local_tm.tm_mon + 1;
      expiry_selected_day = local_tm.tm_mday;
      Serial.printf("[EXPIRY] picker_default source=clock date=%04d-%02d-%02d epoch=%ld\n",
                    expiry_selected_year,
                    expiry_selected_month,
                    expiry_selected_day,
                    (long)now);
    }
  } else {
    Serial.printf("[EXPIRY] picker_default source=fallback date=%04d-%02d-%02d epoch=%ld\n",
                  expiry_selected_year,
                  expiry_selected_month,
                  expiry_selected_day,
                  (long)time(nullptr));
  }
  expiry_clamp_selected_day();
}

static void expiry_prepare_picker_for_entry() {
  expiry_reset_picker_default();
  expiry_submitted = false;
  expiry_date_pos = 0;
  strcpy(expiry_date_buffer, "__-__-____");
  expiry_screen_visible = true;
  expiry_screen_shown_time = millis();
  expiry_update_timeout_ring();
  expiry_refresh_picker_ui();
}

static void expiry_submit_empty_date(const char* wake_reason, const char* ui_reason, const char* log_reason) {
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_EXPIRY_DATE";
  doc["msg_id"] = lcd_msg_id_counter++;
  doc["ts"] = millis();
  doc["expiry_date"] = "";
  doc["quantity"] = expiry_choice_quantity;

  String output;
  serializeJson(doc, output);
  request_sense_wake(wake_reason ? wake_reason : "expiry_empty");
  uart_send_json(output.c_str());
  Serial.printf("[EXPIRY] Sent empty expiration date to Sense reason=%s quantity=%d\n",
                log_reason ? log_reason : "unknown",
                expiry_choice_quantity);

  expiry_submitted = true;
  expiry_screen_shown_time = 0;
  g_ship_ui_finalized = true;
  g_ship_ui_finalized_job_id = g_ship_ui_job_id;
  UI_SHOW(SCREEN_SHIP_LOGGED, ui_reason ? ui_reason : "expiry_submit_empty");
}

static void expiry_submit_selected_date(const char* wake_reason, const char* ui_reason) {
  char expiry_date[11];
  expiry_format_selected_date(expiry_date, sizeof(expiry_date));

  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_EXPIRY_DATE";
  doc["msg_id"] = lcd_msg_id_counter++;
  doc["ts"] = millis();
  doc["expiry_date"] = expiry_date;
  doc["quantity"] = expiry_choice_quantity;

  String output;
  serializeJson(doc, output);
  request_sense_wake(wake_reason ? wake_reason : "expiry_submit");
  uart_send_json(output.c_str());
  Serial.printf("[EXPIRY] Sent expiration date to Sense: %s quantity=%d\n",
                expiry_date,
                expiry_choice_quantity);

  expiry_submitted = true;
  expiry_screen_shown_time = 0;
  g_ship_ui_finalized = true;
  g_ship_ui_finalized_job_id = g_ship_ui_job_id;
  UI_SHOW(SCREEN_SHIP_LOGGED, ui_reason ? ui_reason : "expiry_submit");
}

static void user_activity_bump_quiet(const char* reason, bool log_reason) {
  last_user_activity_ms = millis();
  last_touch_or_input_ms = millis();
  user_activity_since_sleep = true;
  if (ui_is_sleep_eligible_menu_screen(ui_screen_state)) {
    home_shown_ms = last_user_activity_ms;
  }
  if (sleep_retry_requires_user) {
    sleep_retry_requires_user = false;
    sleep_handshake_fail_count = 0;
    sleep_retry_allowed_ms = 0;
    Serial.println("[SLEEP] retry_unblocked reason=user_activity");
  }
  if (sleep_wait_for_sense_idle) {
    sleep_wait_for_sense_idle = false;
    Serial.println("[SLEEP] passive_wait_cleared reason=user_activity");
  }
  if (g_lcd_maintenance_active) {
    g_lcd_maintenance_aborted = true;
  }
  if (log_reason) {
    Serial.printf("[USER_ACTIVITY] reason=%s t=%lu\n",
                  reason ? reason : "unknown",
                  (unsigned long)last_user_activity_ms);
  }
}

static void user_activity_bump(const char* reason) {
  user_activity_bump_quiet(reason, true);
}

static void scroll_activity_bump_if_pending() {
  if (!scroll_activity_pending) {
    return;
  }
  scroll_activity_pending = false;
  unsigned long now_ms = millis();
  if (now_ms == 0) {
    return;
  }
  last_scroll_activity_ms = now_ms;
  bool log_reason = (now_ms - last_scroll_bump_log_ms) >= SCROLL_ACTIVITY_LOG_MS;
  if (log_reason) {
    last_scroll_bump_log_ms = now_ms;
  }
  user_activity_bump_quiet("scroll", log_reason);
  bool needs_ui_wake = g_idle_screen_dark || g_sleep_transition || g_in_light_sleep ||
                       (g_backlight_duty == 0) || !g_panel_enabled || !g_lvgl_running;
  if (needs_ui_wake) {
    ensure_awake_for_ui("scroll_pending");
    request_sense_wake("scroll_wake");
  }
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
  if (sense_state != SENSE_AWAKE) {
    sense_awake_confirmed = false;
  }
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
    last_sense_sleep_ready_ms = 0;
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
  if (provision_qr_waiting || provisioning_active || provision_screen_visible || provision_intro_visible) {
    const char* prov_reason = "provision_screen";
    if (provision_qr_waiting) {
      prov_reason = "provision_wait";
    } else if (provision_intro_visible) {
      prov_reason = "provision_intro";
    } else if (provisioning_active) {
      prov_reason = "provisioning";
    }
    strncpy(reason, prov_reason, reason_len - 1);
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

static void log_sleep_decision(unsigned long now_ms,
                               const char* screen,
                               unsigned long home_age_ms,
                               bool eligible,
                               const char* reason) {
  if ((now_ms - last_sleep_decision_log_ms) > 1000) {
    Serial.printf("[SLEEP_DECISION] screen=%s home_age=%lu eligible=%d reason=%s\n",
                  screen ? screen : "UNKNOWN",
                  home_age_ms,
                  eligible ? 1 : 0,
                  reason ? reason : "-");
    last_sleep_decision_log_ms = now_ms;
  }
}

static bool sleep_blocked_for_ota() {
  // LCD OTA over UART is actively receiving binary data
  if (g_lcd_ota_uart_receiving) {
    return true;
  }
  unsigned long now_ms = millis();
  if (lcd_maintenance_active() || g_ota_mode_active || sense_ota_apply_required || sense_ota_active) {
    // Safety: if Sense is asleep and OTA is not locked, these are stale flags — clear and allow sleep
    if (sense_state == SENSE_ASLEEP && !ota_locked && !g_lcd_ota_uart_receiving) {
      g_lcd_maintenance_active = false;
      g_lcd_maintenance_deadline_ms = 0;
      g_ota_mode_active = false;
      sense_ota_apply_required = false;
      sense_ota_active = false;
      Serial.println("[SLEEP] stale ota/maint flags cleared (sense_asleep)");
      return false;
    }
    return true;
  }
  if (ota_check_requested || ota_check_pending) {
    return true;
  }
  if (now_ms < ota_stay_awake_until_ms) {
    // If Sense went to sleep without OTA_LOCK, the OTA request was missed — don't block.
    // EXCEPTION: a fresh OTA_LOCK window means the Sense is mid self-OTA reboot and
    // will proxy the LCD afterward — keep the LCD awake + UART-reachable.
    if (sense_state == SENSE_ASLEEP && !ota_locked &&
        now_ms >= g_ota_lock_window_until_ms) {
      ota_stay_awake_until_ms = 0;
      ota_check_requested = false;
      return false;
    }
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

static void refresh_sm_set_wake_pending(const char* reason);  // forward declaration

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

  // Auto-retry if user is still on the shopping list screen.
  // The Sense WiFi may take 15-25s to connect on cold boot — the first
  // refresh attempt times out before WiFi is ready. Re-trigger so the
  // user doesn't have to manually retry. (s_list_auto_retry_count is
  // file-scope; the UI_LIST handler resets it when a list lands.)
  if (ui_screen_state == SCREEN_SHOPPING_LIST && s_list_auto_retry_count < 3) {
    s_list_auto_retry_count++;
    Serial.printf("[REFRESH] auto-retry %d/3 (user on shopping list)\n", s_list_auto_retry_count);
    request_sense_wake("list_retry");
    refresh_sm_set_wake_pending("list_auto_retry");
  } else {
    s_list_auto_retry_count = 0;
  }
}

// Hard-timeout recovery: clear all "refresh inflight" state WITHOUT auto-retrying
// (unlike refresh_soft_fail) so a stuck refresh stops spinning and the 10s idle
// sleep can engage. Surfaces a brief non-blocking "Couldn't refresh" notice on
// the list via the UI task (never touches LVGL from here / Core 0). Mirrors the
// flag-clearing done by the UI_LIST-received handler and refresh_soft_fail.
static void refresh_hard_timeout_clear(const char* reason) {
  Serial.printf("[REFRESH] timeout -> cleared (reason=%s)\n", reason ? reason : "unknown");

  refresh_state = REFRESH_IDLE;
  lcd_refresh_inflight = false;
  waiting_for_list_response = false;
  refresh_request_pending = false;
  refresh_request_needs_send = false;
  refresh_input_wake_sent = false;
  refresh_request_retry_count = 0;
  refresh_grace_extended = false;
  lcd_refresh_ack_seen = false;
  lcd_refresh_retry_count = 0;
  lcd_refresh_sent_ms = 0;
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

  // Stop the glow and surface a brief notice on the list — UI-task only.
  if (app_event_queue != NULL) {
    app_event_t evt = {EVT_REFRESH_TIMEOUT, {0}};
    xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
  }
}

// Abandon any pending/inflight refresh with no UI side effects. Used when the
// user leaves the shopping list screen (or any other moment the refresh SM
// would be left running with no consumer — the pill lives on the list screen).
// Mirrors the flag-clearing of refresh_hard_timeout_clear but never auto-
// retries and never posts a UI event. No LVGL — safe from any task.
static void refresh_sm_abandon(const char* reason) {
  if (refresh_state == REFRESH_IDLE &&
      !lcd_refresh_inflight && !waiting_for_list_response && !refresh_request_pending) {
    return;  // nothing running
  }
  Serial.printf("[REFRESH_SM] abandon reason=%s state=%s\n",
                reason ? reason : "unknown",
                refresh_state_name(refresh_state));

  refresh_state = REFRESH_IDLE;
  lcd_refresh_inflight = false;
  waiting_for_list_response = false;
  refresh_request_pending = false;
  refresh_request_needs_send = false;
  refresh_input_wake_sent = false;
  refresh_request_retry_count = 0;
  refresh_grace_extended = false;
  lcd_refresh_ack_seen = false;
  lcd_refresh_retry_count = 0;
  lcd_refresh_sent_ms = 0;
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
  s_list_auto_retry_count = 0;
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
  if (!sense_awake_confirmed) {
    pulseWakeSenseShort();
  }
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
         provision_refresh_pending ||
         (wake_retry_until_ms > 0);
}

static bool lcd_should_wake_sense() {
  return should_wake_sense() ||
         lcd_refresh_inflight ||
         refresh_state != REFRESH_IDLE ||
         provisioning_active ||
         provision_qr_waiting ||
         lcd_maintenance_active() ||
         sense_ota_active ||
         g_ota_mode_active;
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

static bool sense_ready_for_control_tx() {
  if (!sense_awake_confirmed || !link_synced) {
    return false;
  }
  unsigned long now = millis();
  bool rx_recent = (last_sense_rx_ms > 0) &&
                   ((now - last_sense_rx_ms) <= SENSE_CONTROL_READY_WINDOW_MS);
  bool proof_recent = (last_proof_of_life_ms > 0) &&
                      ((now - last_proof_of_life_ms) <= SENSE_CONTROL_READY_WINDOW_MS);
  return rx_recent || proof_recent;
}

static bool lcd_wake_pins_active() {
  return (digitalRead(PIN_TOUCH_INT) == 0) ||
         (digitalRead(PIN_EC1_A) == 0) ||
         (digitalRead(PIN_EC1_B) == 0);
}

static bool lcd_touch_wake_active() {
  return digitalRead(PIN_TOUCH_INT) == 0;
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

static void lcd_wake_pin_drive_level(int level, int mode) {
  // Preload the output latch before switching to OUTPUT so we do not
  // momentarily assert a stale level on the Sense EXT0 wake line.
  digitalWrite(INT_PIN, level);
  lcd_wake_pin_set_mode(INT_PIN, mode);
}

static void configure_sleep_sources(bool enable_ext0, uint32_t timer_sec) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#if HALO_ALLOW_TIMER_WAKE
  if (timer_sec > 0) {
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_sec) * 1000000ULL);
  }
#endif
  if (enable_ext0) {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)HALO_WAKE_GPIO, HALO_WAKE_LEVEL);
  }
  uint64_t wakeMask = buildWakeMaskForSleep();
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
  wake_ext1_enabled = true;
}

static void wake_line_pulse_ms(unsigned long pulse_ms, const char* reason) {
  // GPIO39 is JTAG MTDO on ESP32-S3. When USB-Serial/JTAG is active (CDCOnBoot=cdc),
  // the JTAG peripheral may claim this pin. Force-detach it via ESP-IDF GPIO API.
  gpio_reset_pin((gpio_num_t)INT_PIN);
  gpio_set_direction((gpio_num_t)INT_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)INT_PIN, 1);
  delay(2);
  int pre_level = gpio_get_level((gpio_num_t)INT_PIN);
  Serial.printf("[WAKE_LINE] pulse_begin gpio=%d pre_level=%d len_ms=%lu reason=%s\n",
                (int)INT_PIN, pre_level,
                (unsigned long)pulse_ms,
                reason ? reason : "unknown");
  gpio_set_level((gpio_num_t)INT_PIN, 0);
  delay(pulse_ms);
  int during_level = gpio_get_level((gpio_num_t)INT_PIN);
  gpio_set_level((gpio_num_t)INT_PIN, 1);
  delay(2);
  // Release line with pullup so Sense sees inactive level
  gpio_set_direction((gpio_num_t)INT_PIN, GPIO_MODE_INPUT);
  gpio_pullup_en((gpio_num_t)INT_PIN);
  int post_level = gpio_get_level((gpio_num_t)INT_PIN);
  Serial.printf("[WAKE_LINE] pulse_end gpio=%d during=%d post=%d\n",
                (int)INT_PIN, during_level, post_level);
  lcd_wake_pin_mode = INPUT_PULLUP;
  lcd_wake_pin_pullup = 1;
}

static void pulseWakeSense() {
  Serial.println("[LCD] Pulsing INT to wake Sense...");
  wake_line_pulse_ms(WAKE_PULSE_DURATION_MS, "default");
  Serial.println("[LCD] Wake pulse sent");
}

static void pulseWakeSenseShort() {
  wake_line_pulse_ms(WAKE_PULSE_SHORT_MS, "short");
}

static bool wake_reason_requires_immediate_pulse(const char* reason) {
  if (!reason || !reason[0]) {
    return false;
  }
  return strcmp(reason, "user_ui_wake") == 0 ||
         strcmp(reason, "pre_sleep_touch") == 0 ||
         strcmp(reason, "INPUT_WAKE") == 0 ||
         strcmp(reason, "scroll_wake") == 0 ||
         strcmp(reason, "menu_select") == 0 ||
         strcmp(reason, "voice_start") == 0 ||
         strcmp(reason, "discard_choice") == 0 ||
         strcmp(reason, "retry") == 0 ||
         strcmp(reason, "expiry_submit") == 0 ||
         strcmp(reason, "expiry_submit_empty") == 0;
}

static bool wake_sense_for_request(const char* reason) {
  bool immediate_user_pulse = wake_reason_requires_immediate_pulse(reason);
  if (g_in_light_sleep || g_sleep_transition) {
    return false;
  }
  // HARD GATE: No GPIO pulse when Sense is confirmed awake. UART only.
  if (sense_awake_confirmed) {
    send_sense_ping();
    return false;
  }
  if (sleep_ready_received) {
    if (!(immediate_user_pulse || sense_state == SENSE_ASLEEP)) {
      return false;
    }
    sleep_ready_received = false;
  }
  if (!immediate_user_pulse && sense_recently_heard(1500)) {
    return false;
  }
  if (!lcd_should_wake_sense()) {
    Serial.println("[LCD] skipping Sense wake");
    return false;
  }
  Serial.println("[LCD] waking Sense");
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
  if (g_in_light_sleep || g_sleep_transition) {
    return false;
  }
  // HARD GATE: No GPIO pulse when Sense is confirmed awake. UART only.
  if (sense_awake_confirmed) {
    send_sense_ping();
    return false;
  }
  bool immediate_user_pulse = wake_reason_requires_immediate_pulse(reason);
  if (sleep_ready_received) {
    if (!(immediate_user_pulse || sense_state == SENSE_ASLEEP)) {
      return false;
    }
    sleep_ready_received = false;
  }
  if (!lcd_should_wake_sense()) {
    Serial.println("[LCD] skipping Sense wake");
    return false;
  }
  unsigned long now = millis();
  if (wake_retry_until_ms == 0) {
    start_sense_wake_handshake();
  }
  if (immediate_user_pulse) {
    cancel_pending_sleep_for_user_input(reason);
  }
  maybe_extend_sense_awake_grace(reason);
  last_int_pulse_ms = now;
  Serial.printf("[LCD_INT] pulse_sense reason=%s len_ms=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)WAKE_PULSE_DURATION_MS);
  Serial.println("[LCD] waking Sense");
  pulseWakeSense();
  send_sense_ping();
  last_wake_retry_ms = now;
  sense_status_sync_requested = false;
  sense_ota_apply_required = false;
  sense_wake_explicit_request = false;
  return true;
}

static void request_sense_wake(const char* reason) {
  // HARD GATE: No GPIO pulse when Sense is confirmed awake. UART only.
  if (sense_awake_confirmed) {
    send_sense_ping();
    return;
  }
  sense_wake_explicit_request = true;
  maybe_extend_sense_awake_grace(reason);
  lcd_maybe_pulse_sense_int(reason);
}

static void deferred_awake_tx_service() {
  if (!deferred_awake_tx_valid || g_in_light_sleep || sense_ready_for_control_tx()) {
    return;
  }

  const char* reason = tx_msg_wake_reason(&deferred_awake_tx_msg);
  sense_wake_explicit_request = true;
  maybe_extend_sense_awake_grace(reason);
  lcd_maybe_pulse_sense_int(reason);

  unsigned long now_ms = millis();
  if (deferred_awake_tx_last_ping_ms == 0 ||
      (now_ms - deferred_awake_tx_last_ping_ms) >= REFRESH_WAKE_PING_INTERVAL_MS) {
    if (!link_synced) {
      link_sync_pending = true;
    }
    send_sense_ping();
    deferred_awake_tx_last_ping_ms = now_ms;
    unsigned long rx_age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0xFFFFFFFFUL;
    unsigned long proof_age_ms = last_proof_of_life_ms > 0 ? (now_ms - last_proof_of_life_ms) : 0xFFFFFFFFUL;
    Serial.printf("[UART] waiting_awake_proof type=%s reason=%s awake=%d synced=%d rx_age_ms=%lu proof_age_ms=%lu\n",
                  deferred_awake_tx_msg.type,
                  reason,
                  sense_awake_confirmed ? 1 : 0,
                  link_synced ? 1 : 0,
                  rx_age_ms,
                  proof_age_ms);
  }
}

static void cancel_pending_sleep_for_user_input(const char* reason) {
  bool cancelled = false;
  if (sense_sleep_intent_pending) {
    sense_sleep_intent_pending = false;
    cancelled = true;
  }
  if (sleep_deny_active) {
    sleep_deny_active = false;
    cancelled = true;
  }
  if (sleep_wait_for_sense_idle) {
    sleep_wait_for_sense_idle = false;
    cancelled = true;
  }
  if (g_sleep_transition) {
    g_sleep_transition = false;
    cancelled = true;
  }
  if (cancelled) {
    sleep_cancelled_by_user_input = true;
    sleep_handshake_fail_count = 0;
    sleep_retry_requires_user = false;
    sleep_retry_allowed_ms = 0;
    sleep_deny_retry_ms = 0;
    sleep_deny_reason[0] = '\0';
    sleep_ready_received = false;
    sleep_deny_received = false;
    Serial.printf("[SLEEP] cancel pending lcd sleep reason=%s user_state=%s\n",
                  reason ? reason : "user_input",
                  ship_user_state_name(ship_user_state_current()));
  }
}

static void send_sense_ping() {
  if (link_sync_pending) {
    link_sync_pending = false;
    link_synced = false;
    uart_send_input_message("SYNC");
    send_wifi_status();
  }
  unsigned long now_ms = millis();
  unsigned long min_ping_interval_ms = SENSE_PING_MIN_INTERVAL_MS;
  if (!sense_awake_confirmed ||
      deferred_awake_tx_valid ||
      wake_retry_until_ms > 0 ||
      refresh_state == REFRESH_WAKE_PENDING) {
    min_ping_interval_ms = REFRESH_WAKE_PING_INTERVAL_MS;
  }
  if (last_sense_ping_ms > 0 &&
      (now_ms - last_sense_ping_ms) < min_ping_interval_ms) {
    return;
  }
  uart_send_input_message("INPUT_PING");
  last_sense_ping_ms = now_ms;
  sense_pong_pending = true;
  sense_pong_deadline_ms = now_ms + SENSE_PONG_TIMEOUT_MS;
}

static void start_sense_wake_handshake() {
  sense_awake_confirmed = false;
  wake_retry_until_ms = millis() + WAKE_RETRY_WINDOW_MS;
  wake_retry_attempts = 0;
  last_wake_retry_ms = millis() - WAKE_RETRY_INTERVAL_MS;
  wake_timer_wait_mode = false;
}

static unsigned long wake_retry_interval_for_attempt(uint8_t attempt) {
  if (attempt == 0) {
    return WAKE_RETRY_INTERVAL_MS;
  }
  if (attempt == 1) {
    return 2200UL;
  }
  if (attempt == 2) {
    return 3200UL;
  }
  return 4500UL;
}


#include "lcd_anim.h"



#include "lcd_provision.h"

#include "lcd_errlog.h"

#include "lcd_ship_screens.h"


#include "lcd_ship_flow.h"

#include "lcd_ship_route.h"

#include "lcd_ship_action.h"


#include "lcd_menu.h"



#include "lcd_sleep.h"

// Forward declaration — defined after lcd_activity.h where all dependencies are available
static void lcd_errlog_store_with_context(const char* board, const char* area,
                                           const char* event, int32_t code,
                                           const char* detail);

#include "lcd_diag.h"
#include "lcd_ota_uart.h"

#include "lcd_uart_rx.h"


// ── UI Task ────────────────────────────────────────────────────────
// ONLY the UI task (and loop() when it runs LVGL) may call LVGL; both hold example_lvgl_lock.

#include "lcd_ui_task.h"


// ── UART Task (Core 0) ─────────────────────────────────────────────
// Dedicated task for UART TX/RX - never blocks UI, never calls LVGL

#include "lcd_uart_task.h"


// ── Activity Timer ────────────────────────────────────────────────

#include "lcd_activity.h"


#include "lcd_persist.h"


// ── Enriched Error Logging ────────────────────────────────────────
// Wraps errlog_store() with device context (heap, sense state, screen).
// Placed here because it needs sense_state, ui_screen_state_name(),
// and ESP.getFreeHeap(), all of which are available after lcd_activity.h.
static void lcd_errlog_store_with_context(const char* board, const char* area,
                                           const char* event, int32_t code,
                                           const char* detail) {
    char ctx[64];
    snprintf(ctx, sizeof(ctx), "heap=%luK sense=%s screen=%s",
             (unsigned long)(ESP.getFreeHeap() / 1024),
             sense_state == SENSE_AWAKE ? "AWAKE" :
             sense_state == SENSE_ASLEEP ? "ASLEEP" : "UNKNOWN",
             ui_screen_state_name(ui_screen_state));

    StaticJsonDocument<384> entry;
    entry["board"] = board;
    entry["area"] = area;
    entry["event"] = event;
    entry["code"] = code;
    char enriched_detail[192];
    snprintf(enriched_detail, sizeof(enriched_detail), "%s | %s",
             detail ? detail : "", ctx);
    entry["detail"] = enriched_detail;
    entry["uptime_ms"] = millis();
    String json;
    serializeJson(entry, json);
    errlog_store(json.c_str());
}


// ── Arduino lifecycle ──────────────────────────────────────────────
void setup() {
  print_wakeup_diagnostics(HALO_BOARD_NAME);
  Serial.begin(115200);
  delay(100);
  boot_ms = millis();
  guardian_awake_start_ms = boot_ms;
  guardian_sleep_triggered = false;
  g_boot_count++;
  cold_boot_grace_logged = false;
  waiting_for_sense_cmds = true;
  waiting_for_sense_logged = false;
  
  Serial.println("LCD ESP32-S3: booting...");
  Serial.println("[BOOT] safe_mode_timeout_flush_disabled=1");
  if (kFirmwareVersion && kFirmwareVersion[0]) {
    Serial.printf("[BUILD_DIAG] VERSION=%s\n", kFirmwareVersion);
  }
  // Cache device ID for Settings screen display (from base MAC, no WiFi needed)
  {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(g_lcd_device_id, sizeof(g_lcd_device_id),
             "halo-%02x%02x-%02x%02x", mac[4], mac[5], mac[2], mac[3]);
    Serial.printf("[BOOT] device_id=%s\n", g_lcd_device_id);
  }
  // Restore saved backlight brightness before the first backlight-on so the
  // "on" target reflects the user's setting (sets g_user_brightness_duty).
  {
    int saved_pct = backlight_load_pct_from_nvs();
    backlight_apply_pct(saved_pct);
    Serial.printf("[BOOT] backlight_restore pct=%d duty=%d\n", saved_pct, g_user_brightness_duty);
  }
  esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  esp_reset_reason_t reset_reason = esp_reset_reason();
  {
    if (reset_reason == ESP_RST_BROWNOUT || reset_reason == ESP_RST_INT_WDT ||
        reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_PANIC) {
      lcd_errlog_store_with_context("lcd", "boot", "ABNORMAL_RESET", (int)reset_reason, "watchdog/brownout/panic");
    }
  }
  bool restored_maint_state = lcd_restore_persisted_maintenance_state("boot");
  bool deep_sleep_reset = (reset_reason == ESP_RST_DEEPSLEEP);
  bool panic_reset = (reset_reason == ESP_RST_PANIC);
  uint64_t now_epoch = lcd_time_valid() ? (uint64_t)time(nullptr) : 0ULL;
  bool maintenance_in_window = (now_epoch > 0) ? lcd_maintenance_window_is_current(now_epoch) : false;
  bool maintenance_context = lcd_maintenance_context_present();
  bool maintenance_resume_hint =
      lcd_should_resume_maintenance_on_boot(restored_maint_state, maintenance_in_window);
  bool timer_override =
      (wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED &&
       (deep_sleep_reset || panic_reset) &&
       maintenance_context &&
       maintenance_resume_hint);
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
  if (timer_override) {
    wake_cause_label = "TIMER";
    Serial.printf("[LCD_MAINT] wake_cause_override=timer reason=%s restored=%d in_window=%d armed=%d resume_hint=%d\n",
                  panic_reset ? "panic_recover" : "rtc_armed",
                  restored_maint_state ? 1 : 0,
                  maintenance_in_window ? 1 : 0,
                  g_lcd_maintenance_timer_armed ? 1 : 0,
                  maintenance_resume_hint ? 1 : 0);
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
  lcd_log_rtc_timer_state(timer_override ? "boot_timer_override" : "boot_pre_eval");
  bool effective_timer_wake = (wake_cause == ESP_SLEEP_WAKEUP_TIMER) || timer_override;
  g_ship_ota_wake_window = (HALO_SHIP_TEST_MODE != 0) && LCD_SHIP_MODE_OTA &&
                           (wake_cause == ESP_SLEEP_WAKEUP_TIMER ||
                            wake_cause == ESP_SLEEP_WAKEUP_UNDEFINED);
  if (g_ship_ota_wake_window) {
    Serial.println("[SHIP_OTA] wake_window_start");
    g_lvgl_running = false;
    g_panel_enabled = false;
  }
  g_lcd_maintenance_wake_window =
      (effective_timer_wake && maintenance_context && maintenance_resume_hint);
  if (g_lcd_maintenance_wake_window) {
    Serial.println("[LCD_MAINT] wake_window_start");
    g_lcd_maintenance_timer_armed = 0;
    g_lcd_maintenance_active = true;
    g_lcd_maintenance_started = false;
    g_lcd_maintenance_aborted = false;
    lcd_mode = LCD_MODE_MAINTENANCE;
    if (g_lcd_maintenance_remaining_s == 0 && now_epoch > 0 && maintenance_in_window) {
      g_lcd_maintenance_remaining_s = lcd_maintenance_window_remaining_s(now_epoch);
    }
    if (g_lcd_maintenance_wake_in_s == 0 && g_lcd_maintenance_remaining_s > 0) {
      g_lcd_maintenance_wake_in_s = g_lcd_maintenance_remaining_s;
    }
    if (g_lcd_maintenance_remaining_s > 0) {
      g_lcd_maintenance_deadline_ms =
          millis() + (unsigned long)g_lcd_maintenance_remaining_s * 1000UL;
    }
    g_lvgl_running = false;
    g_panel_enabled = false;
    // Defer headless OTA-mode entry until the regular maintenance loop path.
    // Doing it here during early boot makes the wake path more fragile.
    if (!ota_check_requested && !ota_check_pending) {
      ota_check_requested = true;
      Serial.println("[OTA] maintenance_wake -> self OTA check");
    }
    lcd_persist_maintenance_state("maint_wake_window");
    lcd_log_rtc_timer_state("maint_wake_window");
  }
  g_lcd_schedule_wake_window =
      (effective_timer_wake && g_lcd_schedule_timer_armed);
  if (g_lcd_schedule_wake_window) {
    Serial.println("[LCD_SCHED] wake_window_start");
    g_lcd_schedule_timer_armed = 0;
    g_lcd_schedule_window_deadline_ms =
        millis() + (unsigned long)LCD_OTA_SCHED_WINDOW_MIN * 60000UL;
    ota_check_requested = true;
    lcd_log_rtc_timer_state("sched_wake_window");
  } else {
    g_lcd_schedule_window_deadline_ms = 0;
  }
#ifdef HALO_DEV_WAKE_INTERVAL_SEC
  // DEV: On timer wake, auto-trigger OTA check so we can test OTA unattended.
  // Skip on COLD_BOOT so the UI initializes normally and the device is usable.
  // lcd_enter_ota_mode() tears down LVGL before OTA, freeing enough RAM for TLS.
  if (!ota_check_requested && !ota_check_pending &&
      wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED /* COLD_BOOT */) {
    ota_check_requested = true;
    lcd_manual_ota_override_set("dev_auto_wake");
    g_lcd_maintenance_active = true;
    g_lcd_maintenance_started = false;
    g_lcd_maintenance_wake_window = true;
    Serial.printf("[DEV_OTA] %s -> auto OTA check\n", wake_cause_label);
  }
#endif
#if HALO_OTA_POLICY_MAINTENANCE_ONLY
  if (!g_lcd_maintenance_active) {
    g_lcd_maintenance_boot_grace_until_ms = millis() + LCD_MAINT_BOOT_GRACE_MS;
    Serial.printf("[LCD_MAINT] boot_grace_until=%lu reason=maint_only_wait\n",
                  (unsigned long)g_lcd_maintenance_boot_grace_until_ms);
  } else {
    g_lcd_maintenance_boot_grace_until_ms = 0;
  }
#endif
  link_sync_pending = true;
  link_synced = false;
  
  // Phase 0: Print protocol validation message
  Serial.println("LCD ESP32-S3: booted, PROTO OK v1.");
  
  // Initialize UART
  init_uart();
  
  // Release GPIO39 hold from previous deep sleep and detach JTAG
  gpio_hold_dis((gpio_num_t)INT_PIN);
  gpio_deep_sleep_hold_dis();
  gpio_reset_pin((gpio_num_t)INT_PIN);  // Permanently detach JTAG MTDO

  // Initialize INT pin for waking Sense
  lcd_wake_pin_set_mode(INT_PIN, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)INT_PIN, GPIO_DRIVE_CAP_3);
  digitalWrite(INT_PIN, HIGH);
  delay(2);
  lcd_wake_pin_set_mode(INT_PIN, INPUT_PULLUP);

  // Start Sense wake handshake on any boot — deep sleep wake, power-on, or
  // software reset (e.g. after direct flash). Sense may be in deep sleep and
  // needs a wake pulse to sync with LCD.
  {
    const char* boot_wake_reason = "boot_wake";
    if (wake_cause == ESP_SLEEP_WAKEUP_EXT0) {
      boot_wake_reason = "boot_ext0_wake";
    } else if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
      boot_wake_reason = "boot_ext1_wake";
    } else if (wake_cause == ESP_SLEEP_WAKEUP_TIMER) {
      boot_wake_reason = "boot_timer_wake";
    } else {
      boot_wake_reason = "boot_poweron_wake";
    }
    Serial.printf("[WAKE] Requesting Sense wake in setup reason=%s\n", boot_wake_reason);
    request_sense_wake(boot_wake_reason);
    sense_state_set(SENSE_UNKNOWN, boot_wake_reason);
  }
  
  // Restore test mode from NVS (survives OTA reboots)
  {
    Preferences prefs;
    if (prefs.begin("test_cfg", true)) {
      g_test_mode_active = prefs.getBool("test_mode", false);
      if (g_test_mode_active) {
        g_test_mode_expire_ms = millis() + 3600000; // 1 hour from boot
        Serial.println("[TEST_MODE] restored from NVS (1 hour window)");
      }
      prefs.end();
    }
  }

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

  Serial.printf("[BOOT_DIAG] ship_ota=%d maint_wake=%d eff_timer=%d maint_ctx=%d resume_hint=%d timer_ovr=%d reset=%d wake=%d restored_nvs=%d\n",
                g_ship_ota_wake_window ? 1 : 0,
                g_lcd_maintenance_wake_window ? 1 : 0,
                effective_timer_wake ? 1 : 0,
                maintenance_context ? 1 : 0,
                maintenance_resume_hint ? 1 : 0,
                timer_override ? 1 : 0,
                (int)reset_reason,
                (int)wake_cause,
                restored_maint_state ? 1 : 0);
  if (!g_ship_ota_wake_window && !g_lcd_maintenance_wake_window) {
    init_ui_stack(g_saved_list_count);
  } else {
    Serial.printf("[BOOT_DIAG] UI SKIPPED — clearing stale maintenance NVS and forcing UI init\n");
    lcd_clear_persisted_maintenance_state("stale_boot_override");
    g_lcd_maintenance_wake_window = false;
    g_ship_ota_wake_window = false;
    // Clear maintenance active state that was set before we detected stale NVS
    g_lcd_maintenance_active = false;
    g_lcd_maintenance_started = false;
    g_lcd_maintenance_headless = false;
    g_ota_mode_active = false;
    g_lcd_maintenance_timer_armed = 0;
    g_lcd_maintenance_wake_in_s = 0;
    g_lcd_maintenance_remaining_s = 0;
    g_lcd_maintenance_deadline_ms = 0;
    lcd_mode = LCD_MODE_UI_ACTIVE;
    g_lvgl_running = true;
    g_panel_enabled = true;
    init_ui_stack(g_saved_list_count);
  }
  // Force LVGL to flush the display buffer immediately after init
  if (g_ui_initialized) {
    lv_timer_handler();
    Serial.printf("[BOOT_DIAG] post_init backlight=%d panel=%d lvgl=%d ui_init=%d\n",
                  g_backlight_duty, g_panel_enabled ? 1 : 0, g_lvgl_running ? 1 : 0, g_ui_initialized ? 1 : 0);
  }
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT0 || wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    clear_input_wake_sources("boot_wake");
    touch_ignore_until = millis() + 300;
    scroll_ignore_until = millis() + 150;
  }

  lcd_ota_self_test();

#ifdef HALO_LCD_PROD_WRAPPER
  halo_lcd_prod_setup();
#endif
}

void loop() {
  // safe mode: disable timeout/force-ready flush path (LVGL finish only via SPI done)
  if (lcd_bsp_display_reset_requested()) {
      Serial.println("[LCD_FLUSH] display reset requested (flush failures exceeded threshold)");
      lcd_bsp_reset_flush_fail_count();
      lv_obj_t *scr = lv_scr_act();
      if (scr) {
          lv_obj_invalidate(scr);
          Serial.println("[LCD_FLUSH] fail count reset, full screen invalidated for recovery");
      }
      // Track persistent failure cycles -- if flush keeps failing with zero
      // successful renders, force sleep to get a clean boot
      static uint32_t s_flush_reset_cycles = 0;
      uint32_t flush_ok = 0, flush_fail_tmp = 0;
      int out_tmp = 0, sf_tmp = 0;
      lcd_bsp_get_flush_submit_stats(&flush_ok, &flush_fail_tmp, &out_tmp, &sf_tmp);
      s_flush_reset_cycles++;
      if (s_flush_reset_cycles >= 5 && flush_ok == 0) {
        Serial.printf("[LCD_FLUSH] persistent DMA failure (%u resets, 0 ok) -- forcing sleep\n",
                      (unsigned)s_flush_reset_cycles);
        lcd_sleep_ts("dma_persistent_fail");
        enterLightSleep();
      }
  }
  bool lvgl_locked = false;
  if (g_ui_initialized) {
    if (!example_lvgl_lock(50)) {
      vTaskDelay(pdMS_TO_TICKS(10));
      return;
    }
    lvgl_locked = true;
  }
  // Fallback: if provisioning intro was queued but UI task hasn't shown it yet, force it
  if (provision_intro_pending && !provision_intro_visible) {
    show_provision_intro_screen("force_from_loop");
  }
  // Fallback: if UI_STATUS arrived but UI task hasn't applied it, force route from loop
  if (g_ship_ui_dirty) {
    ui_apply_ship_ui_status(NULL);
  }
  // Fallback: UI task timer-driven updates (countdown, auto-hide, ring animations)
  if (ship_logged_hide_at_ms && millis() >= ship_logged_hide_at_ms) {
    ship_logged_hide_at_ms = 0;
    show_ship_main_menu();
  }
  if (ui_screen_state == SCREEN_HOLD_STILL) {
    ship_update_hold_still_countdown();
  }
  if (ui_screen_state == SCREEN_EXPIRY_CHOICE && ship_expiry_choice_shown_time > 0) {
    ship_update_expiry_choice_timeout_ring();
  }
  if (ui_screen_state == SCREEN_AI_LISTENING) {
    ship_update_ai_listening_countdown();
  }
  if (ship_error_hide_at_ms && millis() >= ship_error_hide_at_ms) {
    ship_error_hide_at_ms = 0;
    show_ship_main_menu();
  }
  if (ship_voice_ack_hide_at_ms && ui_screen_state == SCREEN_VOICE_ACK && millis() >= ship_voice_ack_hide_at_ms) {
    ship_voice_ack_hide_at_ms = 0;
    g_voice_fire_and_forget_ignore_ui = false;
    waiting_for_voice_response = false;
    voice_response_deadline_ms = 0;
    g_ship_voice_json_pending = false;
    stop_glowing_animation();
    Serial.println("[VOICE_FAF] ack_done -> main_menu (loop_fallback)");
    show_ship_main_menu();
  }
  // Fallback: if provisioning completed but UI task hasn't returned to home, force it
  if (provision_return_home_pending) {
    provision_return_home_pending = false;
    hide_provisioning_screen();
    hide_provision_intro_screen("return_home_from_loop");
    show_ship_main_menu();
    Serial.println("[PROVISION] return_home (force_from_loop)");
  }

  // Periodic resend of INPUT_OTA_CHECK while a manual OTA request is latched but
  // the OTA has not yet started. The single send from ship_menu_send_manual_ota()
  // can be dropped on the Sense during its wake/boot LCD_OTA_QUERY window; the
  // event-driven resends (SLEEP_READY/FW_INFO) depend on receiving those messages
  // back, so they can miss. This timer guarantees delivery. Stops immediately once
  // OTA starts (ota_locked) so it never spams during the actual OTA transfer, and
  // stops when the override TTL expires (lcd_manual_ota_override_active() == false).
  {
    static unsigned long last_resend_ms = 0;
    if (lcd_manual_ota_override_active() && !ota_locked) {
      unsigned long now_ms = millis();
      if (now_ms - last_resend_ms >= 1500) {
        last_resend_ms = now_ms;
        StaticJsonDocument<160> resendDoc;
        resendDoc["ver"] = PROTOCOL_VERSION;
        resendDoc["type"] = "INPUT_OTA_CHECK";
        resendDoc["msg_id"] = get_next_msg_id();
        resendDoc["ts"] = now_ms;
        resendDoc["reason"] = "timer_resend";
        String resendOut;
        serializeJson(resendDoc, resendOut);
        senseSerial.println(resendOut);
        Serial.println("[OTA_MANUAL] resend INPUT_OTA_CHECK reason=timer");
      }
    } else {
      last_resend_ms = 0;
    }
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
      if (lvgl_locked) {
        example_lvgl_unlock();
      }
      enter_ship_ota_sleep();
      return;
    } else {
      skip_main_loop = true;
    }
  }
  if (g_lcd_maintenance_headless && !g_ui_initialized) {
    if (touch_detected || g_lcd_maintenance_aborted || provision_qr_exit_headless) {
      const char* exit_reason = "user_input";
      if (provision_qr_exit_headless) {
        Serial.println("[PROVISION] exit_headless (qr_received)");
        provision_qr_exit_headless = false;
        exit_reason = "provision_qr";
      } else {
        Serial.println("[LCD_MAINT] user_input -> exit_headless");
      }
      lcd_exit_maintenance_headless_only(exit_reason);
      lcd_exit_ota_mode(exit_reason);
      g_lcd_maintenance_aborted = false;
      skip_main_loop = false;
    } else {
      if (MAINT_FORCE_WAKE_SENSE) {
        unsigned long now = millis();
        if (lcd_wake_pins_active()) {
          wake_sense_for_request("maint_user_input");
        }
        if (!sense_recently_heard(1500) &&
            (now - maint_force_wake_last_ms) >= MAINT_FORCE_WAKE_INTERVAL_MS) {
          maint_force_wake_last_ms = now;
          wake_sense_for_request("maint_force_wake");
        }
      }
      if (!g_lcd_maintenance_started) {
        g_lcd_maintenance_started = true;
        if (g_lcd_maintenance_aborted) {
          lcd_finish_maintenance("aborted");
        } else {
          if (!ota_check_requested && !ota_check_pending) {
            ota_check_requested = true;
            Serial.println("[OTA] maintenance_headless -> self OTA check");
          }
          Serial.println("[LCD_MAINT] headless waiting_for_ota_check");
        }
      }
      // Deadline check: exit headless if maintenance window expired or failsafe hit
      unsigned long headless_age_ms = millis() - guardian_awake_start_ms;
      bool deadline_expired = (g_lcd_maintenance_deadline_ms > 0 && millis() > g_lcd_maintenance_deadline_ms);
      bool failsafe_expired = (headless_age_ms >= GUARDIAN_FORCE_SLEEP_MS);
      if (deadline_expired || failsafe_expired) {
        Serial.printf("[LCD_MAINT] headless exit reason=%s age_ms=%lu\n",
                      deadline_expired ? "deadline_expired" : "failsafe_timeout",
                      headless_age_ms);
        lcd_finish_maintenance(deadline_expired ? "deadline_expired" : "failsafe_timeout");
        lcd_exit_ota_mode(deadline_expired ? "deadline_expired" : "failsafe_timeout");
        skip_main_loop = false;
      } else {
        skip_main_loop = true;
      }
    }
  }
  if (g_lcd_maintenance_wake_window && !g_ui_initialized) {
    if (!g_lcd_maintenance_started) {
      g_lcd_maintenance_started = true;
      if (g_lcd_maintenance_aborted) {
        lcd_finish_maintenance("aborted");
      } else {
        Serial.println("[LCD_MAINT] waiting_for_ota_check");
      }
    }
    skip_main_loop = true;
  } else if (g_lcd_maintenance_active && !g_lcd_maintenance_started) {
    g_lcd_maintenance_started = true;
    if (g_lcd_maintenance_aborted) {
      lcd_finish_maintenance("aborted");
    } else {
      if (!ota_check_requested && !ota_check_pending) {
        ota_check_requested = true;
        Serial.println("[OTA] maintenance_active -> self OTA check");
      }
      Serial.println("[LCD_MAINT] maintenance_active waiting_for_ota_check");
    }
  }
  if (!lvgl_locked && g_ui_initialized) {
    if (!example_lvgl_lock(50)) {
      vTaskDelay(pdMS_TO_TICKS(10));
      return;
    }
    lvgl_locked = true;
  }
  if (skip_main_loop) {
    if (lvgl_locked) {
      example_lvgl_unlock();
    }
#ifdef HALO_LCD_PROD_WRAPPER
    halo_lcd_prod_loop();
#endif
    if (g_lcd_maintenance_wake_window && !g_lcd_maintenance_active) {
      Serial.println("[LCD_MAINT] maintenance_done -> sleep");
      enter_maintenance_sleep();
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));  // Poll touch every 50ms (use vTaskDelay to yield to IDLE task)
    return;
  }
  if (!g_ui_initialized) {
#ifdef HALO_LCD_PROD_WRAPPER
    halo_lcd_prod_loop();
#endif
    vTaskDelay(pdMS_TO_TICKS(50));
    return;
  }
  
  unsigned long now = millis();
  ship_service_voice_end_resend(now);
  refresh_sense_awake_estimate(now);
  
  // Simple state machine: detect press -> wait for release -> determine brief vs long
  if (touch_detected && !touch_pressed) {
    bool wake_only_touch = g_idle_screen_dark || g_sleep_transition || g_in_light_sleep ||
                           (g_backlight_duty == 0) || !g_panel_enabled || !g_lvgl_running;
    ensure_awake_for_ui("touch_press");
    if (wake_only_touch) {
      // Consume the wake tap immediately instead of waiting for a release edge.
      // When the panel wakes during a passive sleep-deny window, the touch
      // controller can leave the press latched, which strands future input.
      touch_pressed = false;
      touch_wake_only_pending = false;
      touch_press_time = 0;
      touch_press_x = 0;
      touch_press_y = 0;
      long_press_sent = false;
      ship_ai_touch_active = false;
      touch_used_to_dismiss_meal = false;
      touch_ignore_until = now + 300;
      scroll_ignore_until = now + 150;
      user_activity_bump("touch_wake_only");
      Serial.printf("[TOUCH] wake_only consumed at (%d, %d)\n", touch_x, touch_y);
      resetActivityTimer();
      if (lvgl_locked) {
        example_lvgl_unlock();
      }
      return;
    }
    // Touch just pressed - store coordinates
    touch_pressed = true;
    touch_wake_only_pending = false;
    user_activity_bump("touch_press");
    touch_press_time = now;
    touch_press_x = touch_x;  // Store coordinates for later use
    touch_press_y = touch_y;  // Store coordinates for later use
    long_press_sent = false;
    Serial.printf("[TOUCH] Touch pressed at (%d, %d) - stored as (%d, %d)\n", touch_x, touch_y, touch_press_x, touch_press_y);
    haptic_pulse();
    resetActivityTimer();
    
    // Hide meal result screen if visible (user touched, return to main menu)
    if (meal_result_screen != NULL && !lv_obj_has_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN)) {
      Serial.println("[TOUCH] Meal result screen visible - returning to main menu");
      lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
      meal_result_shown_time = 0;  // Reset timeout
      show_ship_main_menu();
      lv_timer_handler();
      // Reset activity timer since user is interacting
      resetActivityTimer();
      // Mark that this touch was used to dismiss meal result (prevent button toggle)
      touch_used_to_dismiss_meal = true;
    } else {
      // Touch not used to dismiss meal result
      touch_used_to_dismiss_meal = false;
      if (ui_screen_state == SCREEN_HOME && ship_menu_screen_state == SHIP_MENU_SCREEN_MAIN) {
        uint16_t check_x = 359 - touch_press_x;
        uint16_t check_y = 359 - touch_press_y;
        const ship_menu_hitbox_t* hb = ship_menu_hit_test(check_x, check_y);
        if (hb != NULL) {
          if (ship_main_menu_is_ai_action(hb)) {
            ship_ai_touch_active = true;
            long_press_sent = false;
            ship_voice_end_resends_remaining = 0;
            ship_voice_end_resend_due_ms = 0;
            if (!provisioning_input_locked()) {
              ship_show_ai_listening_screen();
            }
          } else {
            ship_main_menu_play_tap_animation(hb);
          }
        }
      }
    }
  } else if (!touch_detected && touch_pressed) {
    // Touch just released
    unsigned long press_duration = now - touch_press_time;
    bool was_long_press = long_press_sent;
    touch_pressed = false;
    if (touch_wake_only_pending) {
      touch_wake_only_pending = false;
      long_press_sent = false;
      ship_ai_touch_active = false;
      touch_used_to_dismiss_meal = false;
      Serial.printf("[TOUCH] wake_only release duration_ms=%lu\n", press_duration);
      resetActivityTimer();
      example_lvgl_unlock();
      return;
    }
    
    #if SHIP_MENU_UI
    if (ship_ai_touch_active) {
      ship_ai_touch_active = false;
      if (!long_press_sent || press_duration < LONG_PRESS_THRESHOLD_MS) {
        long_press_sent = false;
        ship_ai_listening_countdown_start_ms = 0;
        ship_update_ai_listening_countdown();
        Serial.printf("[AI] short_press_ignored duration_ms=%lu\n", press_duration);
        if (ui_screen_state == SCREEN_AI_LISTENING) {
          show_ship_main_menu();
          ui_lvgl_tick();
        }
        resetActivityTimer();
        example_lvgl_unlock();
        return;
      }
      long_press_sent = false;
      ship_ai_listening_countdown_start_ms = 0;
      ship_update_ai_listening_countdown();
      g_voice_fire_and_forget_ignore_ui = true;
      waiting_for_voice_response = false;
      voice_response_deadline_ms = 0;
      g_ship_voice_json_pending = false;
      g_ship_voice_json_text[0] = '\0';
      ship_queue_voice_input("INPUT_LONG_PRESS_END", "voice_end");
      ship_voice_end_resends_remaining = 2;          // 2 backup resends at 220ms intervals
      ship_voice_end_resend_due_ms = millis() + 220; // first resend in 220ms
      ship_set_processing_text("Processing");
      ship_show_voice_ack();
      ui_lvgl_tick();
      resetActivityTimer();
      example_lvgl_unlock();
      return;
    }
    if (ui_screen_state == SCREEN_VOICE_JSON) {
      show_ship_main_menu();
      ui_lvgl_tick();
      resetActivityTimer();
      example_lvgl_unlock();
      return;
    }

    uint16_t check_x = 359 - touch_press_x;
    uint16_t check_y = 359 - touch_press_y;

    if (!was_long_press && press_duration < LONG_PRESS_THRESHOLD_MS) {
      if (provision_intro_visible) {
        provision_intro_tapped = true;
        provision_intro_pending = false;
        if (provision_qr_cached) {
          show_provisioning_screen(provision_qr_ssid, provision_qr_password, provision_qr_url);
        } else if (status_screen != NULL && status_label != NULL) {
          hide_provision_intro_screen("tap_wait_qr");
          status_screen_use_text("Preparing\nWi-Fi...");
          lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
          status_screen_shown_time = millis();
        }
        resetActivityTimer();
        ui_lvgl_tick();
        example_lvgl_unlock();
        return;
      }
      if (provisioning_input_locked()) {
        resetActivityTimer();
        ui_lvgl_tick();
        example_lvgl_unlock();
        return;
      }
      if (ui_screen_state == SCREEN_BACKLIGHT) {
        // Any tap saves the (already-applied) brightness and returns to Settings.
        backlight_save_to_nvs();
        show_ship_settings_screen();
        ui_lvgl_tick();
        resetActivityTimer();
        example_lvgl_unlock();
        return;
      }
      if (ui_screen_state == SCREEN_SHOPPING_LIST) {
        if (shopping_list_handle_touch(check_x, check_y)) {
          ui_lvgl_tick();
          resetActivityTimer();
          example_lvgl_unlock();
          return;
        }
      }
      if (expiry_choice_handle_touch(check_x, check_y)) {
        ui_lvgl_tick();
      } else if (expiry_handle_touch(check_x, check_y)) {
        ui_lvgl_tick();
      } else if (ui_screen_state == SCREEN_HOME ||
                 ui_screen_state == SCREEN_SECOND ||
                 ui_screen_state == SCREEN_SETTINGS) {
        const ship_menu_hitbox_t* hb = ship_menu_hit_test(check_x, check_y);
        if (hb) {
          ship_menu_send_action(hb);
          ui_lvgl_tick();
        }
      }
    }
    resetActivityTimer();
    #else
    if (provisioning_input_locked()) {
      if (long_press_sent) {
        long_press_sent = false;
        if (recording_indicator != NULL) {
          lv_obj_add_flag(recording_indicator, LV_OBJ_FLAG_HIDDEN);
        }
      }
      resetActivityTimer();
      ui_lvgl_tick();
      example_lvgl_unlock();
      return;
    }
    if (!was_long_press && press_duration < LONG_PRESS_THRESHOLD_MS) {
      uint16_t check_x = 359 - touch_press_x;
      uint16_t check_y = 359 - touch_press_y;
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
        lv_area_t btn_area;
        lv_obj_get_coords(status_reset_button, &btn_area);
        if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
            check_y >= btn_area.y1 && check_y <= btn_area.y2) {
          Serial.println("[STATUS] Reset Wi-Fi button pressed");
          provision_user_requested = true;
          set_status_reset_visible(false);
          status_screen_use_text("Resetting\nWi-Fi...");
          lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
          status_screen_shown_time = millis();
          lv_timer_handler();
          provision_qr_wait_begin("status_button");
          tx_msg_t tx_msg = {};
          strncpy(tx_msg.type, "INPUT_RESET_WIFI", sizeof(tx_msg.type) - 1);
          if (uart_tx_queue != NULL) {
            xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
          }
        }
      } else if (expiry_handle_touch(check_x, check_y)) {
        // handled by expiry keypad
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
              doc["quantity"] = expiry_choice_quantity;
              
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
                doc["quantity"] = expiry_choice_quantity;
                
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
  #if SHIP_MENU_UI
  else if (touch_pressed && ship_ai_touch_active && !long_press_sent) {
    if (provisioning_input_locked()) {
      return;
    }
    unsigned long press_duration = now - touch_press_time;
    if (press_duration >= LONG_PRESS_THRESHOLD_MS) {
      long_press_sent = true;
      ship_ai_listening_countdown_start_ms = now;
      ship_update_ai_listening_countdown();
      Serial.printf("[AI] long_press_start duration_ms=%lu\n", press_duration);
      ship_queue_voice_input("INPUT_LONG_PRESS_START", "voice_start");
      ui_lvgl_tick();
      resetActivityTimer();
    }
  }
  #endif
  #if !SHIP_MENU_UI
  else if (touch_pressed && !long_press_sent) {
    if (provisioning_input_locked()) {
      return;
    }
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
    if (sense_awake_confirmed) {
      send_sense_ping();
      sense_status_sync_requested = false;
    } else {
      lcd_maybe_pulse_sense_int("status_sync");
    }
  }
  
  // Check if "On it!" status screen has been showing for 1 second - hide it and show list with glowing halo
  if (status_screen != NULL && !lv_obj_has_flag(status_screen, LV_OBJ_FLAG_HIDDEN) && 
      status_screen_shown_time > 0) {
    unsigned long status_elapsed = now - status_screen_shown_time;
    if (status_screen_auto_hide_at_ms > 0 && now >= status_screen_auto_hide_at_ms) {
      lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
      status_screen_shown_time = 0;
      status_screen_auto_hide_at_ms = 0;
      lv_timer_handler();
      resetActivityTimer();
    }
    // Check if it's the "On it!" message (1 second timeout)
    else {
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
  }

  if (ship_menu_settings_status != NULL &&
      !lv_obj_has_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN) &&
      ship_menu_settings_status_hide_at_ms > 0 &&
      now >= ship_menu_settings_status_hide_at_ms) {
    lv_obj_add_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN);
    ship_menu_settings_status_hide_at_ms = 0;
    lv_timer_handler();
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
    Serial.println("[LOOP] Meal result screen timeout (10s) - returning home and sleeping");
    lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
    meal_result_shown_time = 0;  // Reset timeout
    show_ship_main_menu();
    enterLightSleep();
  }

  // If reset Wi-Fi was requested but QR never arrived, show error after timeout.
  if (provision_qr_waiting && !provision_screen_visible) {
    unsigned long wait_ms = millis() - provision_qr_wait_start_ms;
    if (wait_ms > PROVISION_QR_WAIT_TIMEOUT_MS) {
      provision_qr_waiting = false;
      provision_qr_wait_start_ms = 0;
      Serial.printf("[PROVISION] wait_for_qr timeout after %lu ms\n", wait_ms);
      if (status_screen != NULL && status_label != NULL) {
        status_screen_use_text("Reset Wi-Fi\nNo QR from Sense");
        lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
        status_screen_shown_time = millis();
        set_status_reset_visible(true);
        lv_timer_handler();
      }
    }
  }

  if (ui_screen_state == SCREEN_EXPIRY_CHOICE &&
      ship_expiry_choice_shown_time > 0 &&
      (millis() - ship_expiry_choice_shown_time) > EXPIRY_SCREEN_TIMEOUT_MS &&
      !expiry_submitted) {
    if (ship_choice_mode_is_discard()) {
      Serial.println("[LOOP] Discard choice timeout (30s) - defaulting to skip");
      ship_send_discard_choice(false, "discard_choice_timeout");
    } else {
      Serial.println("[LOOP] Expiry choice timeout (30s) - sending empty date to Sense");

      StaticJsonDocument<256> doc;
      doc["ver"] = PROTOCOL_VERSION;
      doc["type"] = "INPUT_EXPIRY_DATE";
      doc["msg_id"] = lcd_msg_id_counter++;
      doc["ts"] = millis();
      doc["expiry_date"] = "";
      doc["quantity"] = expiry_choice_quantity;

      String output;
      serializeJson(doc, output);
      request_sense_wake("expiry_choice_timeout");
      uart_send_json(output.c_str());
      Serial.printf("[EXPIRY_CHOICE] Sent empty expiration date to Sense (timeout) quantity=%d\n",
                    expiry_choice_quantity);
    }

    expiry_submitted = true;
    ship_expiry_choice_shown_time = 0;
    g_ship_ui_finalized = true;
    g_ship_ui_finalized_job_id = g_ship_ui_job_id;
    UI_SHOW(SCREEN_SHIP_LOGGED, "expiry_choice_timeout");
  }
  
  // Check if expiry screen has been showing for 30 seconds - auto-hide it and send empty date
  if (expiry_screen_visible && expiry_screen != NULL && !lv_obj_has_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN) && 
      expiry_screen_shown_time > 0 && (millis() - expiry_screen_shown_time) > EXPIRY_SCREEN_TIMEOUT_MS) {
    Serial.println("[LOOP] Expiry screen timeout (30s) - hiding and sending empty date to Sense");
    expiry_submit_empty_date("expiry_timeout", "expiry_timeout", "timeout");
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
        lcd_errlog_store_with_context("lcd", "sense_wake", "MISSED_PONGS", (int)sense_missed_pongs, "sense unresponsive");
      }
    }
    bool need_probe = (sense_state != SENSE_AWAKE) && !(sleep_deny_active && g_idle_screen_dark);
    if (need_probe && (now_ms - last_sense_ping_ms) >= SENSE_PROBE_INTERVAL_MS) {
      send_sense_ping();
    }
  }

  // After provisioning completes, skip the old shopping-list refresh flow.
  if (!g_in_light_sleep && provision_refresh_pending) {
    provision_refresh_pending = false;
    Serial.println("[UI] Provisioning connected - shopping list refresh disabled");
    resetActivityTimer();
  }

  update_wifi_on_pending_state();

  // Keepalive while Wi-Fi connecting: send INPUT_PING so Sense link stays recent and does not sleep during LCD connect attempt.
#ifdef HALO_LCD_PROD_WRAPPER
  {
    static unsigned long last_wifi_keepalive_ms = 0;
    unsigned long now_wk = millis();
    if (wifi_on_pending && wifi_pending_start_ms > 0 && !lcd_wifi_connected() &&
        lcd_wifi_allowed_for_ota()) {
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

  // Keep Sense link recent while LCD is awake to prevent desync sleep.
  if (!g_in_light_sleep && link_synced && !(sleep_deny_active && g_idle_screen_dark)) {
    unsigned long now_keepalive = millis();
    if (now_keepalive - last_lcd_keepalive_ms >= LCD_SENSE_KEEPALIVE_MS) {
      tx_msg_t tx_msg = {};
      strncpy(tx_msg.type, "INPUT_PING", sizeof(tx_msg.type) - 1);
      if (uart_tx_queue != NULL) {
        xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
      }
      last_lcd_keepalive_ms = now_keepalive;
    }
  }

  // Keep the Sense awake + WiFi-connected ONLY while the user is actively on the
  // shopping list. Re-assert LIST_ACTIVE(1) every ~3s during active use (the Sense
  // auto-clears after ~30s of silence). Once the list goes idle past the inactivity
  // timeout, send LIST_ACTIVE(0) once so the Sense releases its keep-awake flag and
  // the normal coordinated 10s idle-sleep can proceed (otherwise the re-assert pins
  // the Sense awake and the device never sleeps on the list). Plain UART (no LVGL).
  if (!g_in_light_sleep && ui_screen_state == SCREEN_SHOPPING_LIST) {
    static unsigned long last_list_active_tx_ms = 0;
    unsigned long now_la = millis();
    unsigned long last_act = last_user_activity_ms;
    if (last_scroll_activity_ms > last_act) last_act = last_scroll_activity_ms;
    unsigned long idle_age = (last_act > 0) ? (now_la - last_act) : 0;
    // Active use -> assert(1) (Sense stays awake + WiFi up). Idle past the timeout ->
    // assert(0) so the Sense releases and the coordinated 10s sleep can proceed.
    // Re-send the CURRENT desired state every ~3s (robust to a dropped UART message;
    // the Sense also auto-clears after ~30s as a backstop).
    if (last_list_active_tx_ms == 0 || (now_la - last_list_active_tx_ms) >= 3000) {
      // Also keep the Sense awake while a refresh is in flight so it doesn't
      // sleep mid-fetch (the user may be idle while the list refreshes).
      bool keep = (idle_age < INACTIVITY_TIMEOUT_MS) ||
                  refresh_state == REFRESH_WAKE_PENDING ||
                  refresh_state == REFRESH_INFLIGHT;
      uart_send_list_active(keep);
      last_list_active_tx_ms = now_la;
    }
  }

  // If Sense didn't acknowledge wake, retry wake pulses for a short window
  if (!g_in_light_sleep && !sense_awake_confirmed && wake_retry_until_ms > 0 &&
      refresh_state != REFRESH_INFLIGHT) {
    unsigned long now = millis();
    if (now > wake_retry_until_ms) {
      wake_retry_until_ms = 0;
      // Enter timer-wait mode — stop pulsing, wait for Sense's failsafe timer
      if (!wake_timer_wait_mode && !sense_awake_confirmed) {
        wake_timer_wait_mode = true;
        wake_timer_wait_start_ms = now;
        release_wake_line("timer_wait");
        Serial.println("[WAKE] pulse retries exhausted -> timer-wait mode (30s)");
      }
    } else if (now - last_wake_retry_ms >= wake_retry_interval_for_attempt(wake_retry_attempts)) {
      if (sense_recently_heard(1500UL)) {
        Serial.println("[LCD] Wake retry waiting for sync...");
        send_sense_ping();
        last_wake_retry_ms = now;
      } else {
        ++wake_retry_attempts;
        Serial.printf("[LCD] Wake retry pulse attempt=%u\n", (unsigned)wake_retry_attempts);
        lcd_maybe_pulse_sense_int("wake_retry");
      }
    }
  }

  // Timer-wait mode: Sense may be in timer-only sleep. Send UART pings until it wakes.
  if (wake_timer_wait_mode && !sense_awake_confirmed) {
    unsigned long now = millis();
    if (now - wake_timer_wait_start_ms > WAKE_TIMER_WAIT_WINDOW_MS) {
      wake_timer_wait_mode = false;
      Serial.println("[WAKE] timer-wait expired — Sense unreachable");
    } else if (now - last_sense_ping_ms > 2000) {
      uart_send_input_message("INPUT_PING");
      last_sense_ping_ms = now;
    }
  }

  // Refresh state machine: proof-of-life, pulses, timeout
  if (!g_in_light_sleep && refresh_state == REFRESH_WAKE_PENDING) {
    unsigned long now = millis();
    // Unified 20s hard watchdog: a stuck WAKE_PENDING (Sense never woke) is
    // cleared to IDLE just like a stuck INFLIGHT, so ANY non-IDLE refresh
    // resolves by REFRESH_HARD_TIMEOUT_MS and the 10s idle-sleep can engage.
    if (ui_screen_state == SCREEN_SHOPPING_LIST &&
        refresh_wake_pending_start_ms > 0 &&
        (now - refresh_wake_pending_start_ms) > REFRESH_HARD_TIMEOUT_MS) {
      refresh_timeout_count++;
      refresh_hard_timeout_clear("wake_pending_hard_timeout");
      example_lvgl_unlock();
      return;
    }
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
        if (!sense_awake_confirmed) {
          pulseWakeSenseShort();
        }
        send_sense_ping();
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
    // Hard timeout on the shopping list: if the Sense never returns UI_LIST,
    // clear the stuck state (no auto-retry) so the user isn't stuck on
    // "Refreshing..." forever and the 10s idle-sleep can engage.
    if (ui_screen_state == SCREEN_SHOPPING_LIST &&
        total_ms > REFRESH_HARD_TIMEOUT_MS) {
      refresh_timeout_count++;
      refresh_hard_timeout_clear("inflight_hard_timeout");
      example_lvgl_unlock();
      return;
    }
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
  scroll_activity_bump_if_pending();

  if (!g_in_light_sleep && refresh_state == REFRESH_FAILED) {
    refresh_soft_fail("failed_state");
  }
  
  // Check for inactivity and enter sleep
  // Use extended timeout (30s) if waiting for voice response
  // Use extended timeout if waiting for voice or scan response
  unsigned long timeout_ms = INACTIVITY_TIMEOUT_MS;
  if (provisioning_input_locked()) {
    timeout_ms = PROVISIONING_TIMEOUT_MS;
  } else if (waiting_for_scan_response) {
    timeout_ms = SCAN_RESPONSE_TIMEOUT_MS;
  } else if (waiting_for_voice_response) {
    timeout_ms = VOICE_RESPONSE_TIMEOUT_MS;
  } else if (waiting_for_list_response) {
    timeout_ms = API_RESPONSE_TIMEOUT_MS;
  }

  unsigned long now_ms = millis();
  unsigned long last_activity_ms = last_user_activity_ms;
  if (last_scroll_activity_ms > last_activity_ms) {
    last_activity_ms = last_scroll_activity_ms;
  }
  unsigned long age_ms = last_activity_ms > 0 ? (now_ms - last_activity_ms) : 0;
  unsigned long effective_timeout_ms = timeout_ms;
  if (USER_WAKE_HOLD_MS > effective_timeout_ms) {
    effective_timeout_ms = USER_WAKE_HOLD_MS;
  }

  if (!g_in_light_sleep && GUARDIAN_FORCE_SLEEP_MS > 0) {
    unsigned long awake_ms = now_ms - guardian_awake_start_ms;
    if (awake_ms >= GUARDIAN_FORCE_SLEEP_MS) {
      if (lcd_ota_uart_active()) {
        // Do not force-sleep during active LCD OTA
        static bool guardian_ota_defer_logged = false;
        if (!guardian_ota_defer_logged) {
          Serial.println("[GUARDIAN] force_sleep deferred (lcd_ota_uart_active)");
          guardian_ota_defer_logged = true;
        }
      } else {
        if (!guardian_sleep_triggered) {
          guardian_sleep_triggered = true;
          Serial.printf("[GUARDIAN] force_sleep elapsed_ms=%lu\n", awake_ms);
        }
        enterLightSleep();
        goto loop_continue;
      }
    }
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
    ship_menu_service_fw_info_request(now_ms);
    if (sense_sleep_intent_pending) {
      const char* deny_reason = NULL;
      if (!lcd_sleep_intent_allowed(&deny_reason)) {
        uint32_t retry_ms = SLEEP_DENY_RETRY_DEFAULT_MS;
        uart_send_sleep_deny(deny_reason ? deny_reason : "op_inflight", retry_ms);
        Serial.printf("[SLEEP_INTENT] deny reason=%s retry_ms=%lu\n",
                      deny_reason ? deny_reason : "op_inflight",
                      (unsigned long)retry_ms);
        sense_sleep_intent_pending = false;
        goto loop_continue;
      }
      Serial.println("[SLEEP_INTENT] enter_sleep");
      sense_sleep_intent_pending = false;
      enterLightSleep();
      goto loop_continue;
    }
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
    const char* screen_name = ui_screen_state_name(ui_screen_state);
    bool on_menu_screen = ui_is_sleep_eligible_menu_screen(ui_screen_state);
    if (on_menu_screen && home_shown_ms == 0) {
      home_shown_ms = now_ms;
    }
    unsigned long home_age_ms = (on_menu_screen && home_shown_ms > 0) ? (now_ms - home_shown_ms) : 0;
    bool dish_processing = false;
    if (ship_mode_is_dish(g_ship_ui_op, g_ship_ui_mode)) {
      const char* phase = g_ship_ui_phase;
      if (phase &&
          (strcmp(phase, "UPLOAD_STARTING") == 0 ||
           strcmp(phase, "UPLOADING") == 0 ||
           strcmp(phase, "RESULT_WAITING") == 0 ||
           strcmp(phase, "PROCESSING") == 0)) {
        dish_processing = true;
      }
    }
    if (!on_menu_screen) {
      if (g_idle_screen_dark) {
        lcd_set_idle_screen_dark(false, "screen_not_home");
      }
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, "not_home");
      goto loop_continue;
    }
    if (provisioning_input_locked()) {
      if (g_idle_screen_dark) {
        lcd_set_idle_screen_dark(false, "provisioning_active");
      }
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, "provisioning_active");
      goto loop_continue;
    }
    if (dish_processing) {
      if (g_idle_screen_dark) {
        lcd_set_idle_screen_dark(false, "dish_processing");
      }
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, "dish_processing");
      goto loop_continue;
    }
    // Test mode: block all sleep for automated testing
    if (g_test_mode_active && millis() < g_test_mode_expire_ms) {
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, "test_mode");
      goto loop_continue;
    }
    bool eligible = home_age_ms >= HOME_SLEEP_DELAY_MS;
    const char* decision_reason = eligible ? "eligible" : "home_age_lt_timeout";
    if (!eligible) {
      if (g_idle_screen_dark && !sleep_deny_active) {
        lcd_set_idle_screen_dark(false, "home_active");
      } else if (g_idle_screen_dark && sleep_deny_active) {
        log_sleep_decision(now_ms, screen_name, home_age_ms, false, "deny_wait_dark");
        goto loop_continue;
      }
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, decision_reason);
      goto loop_continue;
    }
    if (ota_locked) {
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, "ota_locked");
      goto loop_continue;
    }
    if (g_lcd_ota_uart_receiving) {
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, "lcd_ota_uart_receiving");
      goto loop_continue;
    }
    if (lcd_ota_uart_active()) {
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, "lcd_ota_uart");
      goto loop_continue;
    }
    lcd_set_idle_screen_dark(true, "idle_timeout");
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
#if HALO_OTA_POLICY_MAINTENANCE_ONLY
    if (!inhibit_reason && g_lcd_maintenance_boot_grace_until_ms > 0 &&
        millis() < g_lcd_maintenance_boot_grace_until_ms) {
      inhibit_reason = "maint_boot_grace";
    }
#endif
    if (!inhibit_reason && g_lcd_maintenance_active) {
      // Safety: if Sense is asleep and OTA not active, maintenance is stale — clear it
      if (sense_state == SENSE_ASLEEP && !ota_locked && !g_lcd_ota_uart_receiving) {
        g_lcd_maintenance_active = false;
        g_lcd_maintenance_deadline_ms = 0;
        g_ota_mode_active = false;
        Serial.println("[SLEEP] stale maintenance cleared (sense_asleep, no ota)");
      } else if (g_lcd_maintenance_deadline_ms == 0 || millis() < g_lcd_maintenance_deadline_ms) {
        inhibit_reason = "maintenance_active";
      }
    }
    if (!inhibit_reason && ota_locked) {
      inhibit_reason = "ota_locked";
    }
    if (!inhibit_reason && lcd_ota_uart_active()) {
      inhibit_reason = "lcd_ota_uart";
    }
    // Stay awake through the absolute maintenance window so the Sense's
    // LCD-first OTA proxy (which queries the LCD with retries over ~35s at
    // window entry) can reach us. The LCD has a real clock now (set from the
    // Sense's now_epoch in MAINT_WINDOW), so once we are inside the window we
    // must not idle-sleep until the OTA completes or the window+grace ends.
    // lcd_maintenance_window_is_current() is bounded by start_epoch +
    // duration + grace_after, so this self-terminates. Clock-gated so it never
    // affects normal (non-maintenance) idle-sleep or an old-Sense/no-clock LCD.
    if (!inhibit_reason && lcd_time_valid() &&
        g_lcd_maintenance_start_epoch > 0 &&
        lcd_maintenance_window_is_current((uint64_t)time(nullptr))) {
      static unsigned long last_maint_window_log_ms = 0;
      unsigned long mw_now = millis();
      if (mw_now - last_maint_window_log_ms > 5000) {
        Serial.printf("[LCD_MAINT] stay_awake in_window now_epoch=%llu remaining_s=%lu\n",
                      (unsigned long long)time(nullptr),
                      (unsigned long)lcd_maintenance_window_remaining_s((uint64_t)time(nullptr)));
        last_maint_window_log_ms = mw_now;
      }
      inhibit_reason = "maintenance_window";
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
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, decision_reason);
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
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, decision_reason);
      goto loop_continue;
    }
    if (millis() < ota_stay_awake_until_ms) {
      // If Sense went to sleep without OTA_LOCK, the OTA request was missed.
      // EXCEPTION: a fresh OTA_LOCK window means a real dual-OTA is pending and
      // the Sense is just mid self-OTA reboot — keep the LCD awake + reachable.
      if (sense_state == SENSE_ASLEEP && !ota_locked &&
          millis() >= g_ota_lock_window_until_ms) {
        Serial.println("[OTA] stay_awake cancelled (sense asleep, no ota_lock)");
        ota_stay_awake_until_ms = 0;
        ota_check_requested = false;
      } else if (sense_state == SENSE_ASLEEP && !ota_locked) {
        static unsigned long last_ota_lock_keep_log_ms = 0;
        if (millis() - last_ota_lock_keep_log_ms > 5000) {
          Serial.println("[OTA] keep stay_awake (ota_lock window active, sense rebooting)");
          last_ota_lock_keep_log_ms = millis();
        }
        log_sleep_decision(now_ms, screen_name, home_age_ms, false, "ota_stay_awake");
        goto loop_continue;
      } else {
        static unsigned long last_ota_log_ms = 0;
        if (millis() - last_ota_log_ms > 5000) {
          Serial.println("[OTA] stay_awake window active - deferring sleep");
          last_ota_log_ms = millis();
        }
        log_sleep_decision(now_ms, screen_name, home_age_ms, false, "ota_stay_awake");
        goto loop_continue;
      }
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
    if (!g_in_light_sleep &&
        !g_sleep_transition &&
        sleep_ready_received &&
        sense_state == SENSE_ASLEEP &&
        g_idle_screen_dark &&
        on_menu_screen) {
      Serial.println("[SLEEP] remote_sleep_ready_idle_dark -> local sleep");
      enterLightSleep();
      goto loop_continue;
    }
    if (sleep_wait_for_sense_idle) {
      unsigned long rx_age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0xFFFFFFFFUL;
      bool sense_recent = (last_sense_rx_ms > 0) &&
                          (rx_age_ms < SENSE_RECENT_RX_FOR_SLEEP_MS);
      if (sense_state == SENSE_ASLEEP || !sense_recent) {
        sleep_wait_for_sense_idle = false;
        Serial.printf("[SLEEP] passive_wait_released state=%s rx_age_ms=%lu\n",
                      sense_state_name(sense_state),
                      rx_age_ms);
      } else {
        if (now_ms - last_sleep_retry_log_ms > 5000) {
          Serial.printf("[SLEEP] waiting_for_sense_idle reason=%s rx_age_ms=%lu\n",
                        sleep_deny_reason[0] ? sleep_deny_reason : "op_inflight",
                        rx_age_ms);
          last_sleep_retry_log_ms = now_ms;
        }
        goto loop_continue;
      }
    }
    if (sleep_retry_allowed_ms > 0 && now_ms < sleep_retry_allowed_ms) {
      if (now_ms - last_sleep_retry_log_ms > 5000) {
        unsigned long remaining_ms = sleep_retry_allowed_ms - now_ms;
        Serial.printf("[SLEEP] retry_backoff remaining_ms=%lu\n", remaining_ms);
        last_sleep_retry_log_ms = now_ms;
      }
      goto loop_continue;
    }
    log_sleep_decision(now_ms, screen_name, home_age_ms, true, "eligible");
    if (waiting_for_voice_response) {
      Serial.println("[LOOP] Inactivity timeout (voice mode: 30s) - entering sleep...");
    } else if (waiting_for_scan_response) {
      Serial.println("[LOOP] Inactivity timeout (scan mode: 90s) - entering sleep...");
    } else {
      Serial.println("[LOOP] Inactivity timeout - entering sleep...");
    }
    lcd_sleep_ts("idle_timeout");
    wifi_on_run_deferred_if_ready("pre_sleep");
    enterLightSleep();
  }

loop_continue:
  if (lvgl_locked) {
    lv_timer_handler();  // Tick LVGL animations/timers every loop iteration
    example_lvgl_unlock();
  }

#ifdef HALO_LCD_PROD_WRAPPER
  halo_lcd_prod_loop();
#endif

  vTaskDelay(pdMS_TO_TICKS(5));  // Fast loop for smooth LVGL rendering (was 50ms, too slow for animations)
}


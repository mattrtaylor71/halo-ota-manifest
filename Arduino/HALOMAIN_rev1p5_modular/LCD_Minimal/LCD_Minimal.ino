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
#include "Preferences.h"
#include <string.h>
#include <time.h>
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
#include "../halomain_assets/ui_img_Frame_443_png.c"
#include "../halomain_assets/ui_img_Frame_443_1_png.c"
#include "../start_screen/ui_img_Frame_493_png.c"

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
#define LCD_OTA_WAKE_INTERVAL_SEC (6 * 60 * 60)
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
  SCREEN_AI_LISTENING,
  SCREEN_VOICE_JSON,
  SCREEN_HOLD_STILL,
  SCREEN_VOICE_ACK,
  SCREEN_PROCESSING,
  SCREEN_LOGGED,
  SCREEN_EXPIRY_CHOICE,
  SCREEN_EXPIRY,
  SCREEN_RESULT,
  SCREEN_DEBUG
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
static bool sleep_wait_for_sense_idle = false;
static bool sleep_cancelled_by_user_input = false;
static uint32_t sleep_fallback_timer_sec = 0;
static bool sense_ota_active = false;
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
static const unsigned long SENSE_CONTROL_READY_WINDOW_MS = 2000;
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
static unsigned long voice_response_deadline_ms = 0;
// Voice is fire-and-forget on LCD: after the local acknowledgement, late
// voice-only UI responses are ignored so the user can continue using the UI.
static volatile bool g_voice_fire_and_forget_ignore_ui = false;
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
static unsigned long ota_lock_at_ms = 0;
static const unsigned long OTA_LOCK_TIMEOUT_MS = 600000;  // 10 min auto-unlock safety
static const unsigned long OTA_UNLOCK_GRACE_MS = 45000;   // keep LCD awake after unlock
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
static lv_obj_t *ship_menu_settings_btn_ota = NULL;
static lv_obj_t *ship_menu_settings_label_ota = NULL;
static lv_obj_t *ship_menu_settings_btn_back = NULL;
static lv_obj_t *ship_menu_settings_label_back = NULL;
static lv_obj_t *ship_menu_settings_status = NULL;
static unsigned long ship_menu_settings_status_hide_at_ms = 0;
static char g_sense_fw_version[32] = "--";
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
static lv_obj_t *ship_ai_listening_title = NULL;
static lv_obj_t *ship_ai_listening_hint = NULL;
static lv_obj_t *ship_ai_listening_mic_head = NULL;
static lv_obj_t *ship_ai_listening_mic_stem = NULL;
static lv_obj_t *ship_ai_listening_mic_base = NULL;
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
static const unsigned long SHIP_HOLD_COUNTDOWN_MS = 5000UL;
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

// Settings screen button bounds
#define SHIP_MENU_SETTINGS_BTN_W 300
#define SHIP_MENU_SETTINGS_BTN_H 64
#define SHIP_MENU_SETTINGS_BTN_X ((SHIP_MENU_W - SHIP_MENU_SETTINGS_BTN_W) / 2)
#define SHIP_MENU_SETTINGS_VERSION_Y 52
#define SHIP_MENU_SETTINGS_STATUS_Y 72
#define SHIP_MENU_SETTINGS_RESET_Y 90
#define SHIP_MENU_SETTINGS_OTA_Y 170
#define SHIP_MENU_SETTINGS_BACK_Y 250

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
  EVT_SHIP_UI_STATUS, // From UART: Ship menu UI_STATUS -> update overlay/result
  EVT_SHIP_UI_TOAST,
  EVT_SHIP_VOICE_JSON,
} app_event_type_t;

typedef struct app_event_t {
  app_event_type_t type;
  union {
    int8_t scroll_delta;
    int new_count;
    int menu_index;  // For EVT_MENU_SELECTED
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
  lv_color_t bg = active ? lv_color_hex(0x6EE7B7) : lv_color_hex(0x16335E);
  lv_color_t border = active ? lv_color_hex(0x6EE7B7) : lv_color_hex(0x355D93);
  lv_color_t text = active ? lv_color_hex(0x0E2547) : lv_color_hex(0xFFFFFF);
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
  unsigned long now_ms = millis();
  if (lcd_maintenance_active() || g_ota_mode_active || sense_ota_apply_required || sense_ota_active) {
    return true;
  }
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
  // Release line so Sense sees a biased inactive wake pin.
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
  if (!immediate_user_pulse && sense_awake_confirmed && link_synced) {
    sense_status_sync_requested = false;
    return false;
  }
  unsigned long now = millis();
  if (wake_retry_until_ms == 0) {
    start_sense_wake_handshake();
  }
  if (immediate_user_pulse) {
    cancel_pending_sleep_for_user_input(reason);
  } else if (now - last_int_pulse_ms < INT_PULSE_COOLDOWN_MS) {
    return false;
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
  (void)op;
      return;
}

// Stop glowing animation for processing indicator
static void stop_glowing_animation(void) {
  if (lv_is_initialized() && processing_indicator != NULL) {
    lv_obj_add_flag(processing_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width(processing_indicator, 0, LV_PART_MAIN);
    lv_obj_set_style_border_opa(processing_indicator, LV_OPA_TRANSP, LV_PART_MAIN);
  }
  is_glowing_animation = false;
  processing_start_ms = 0;
  strncpy(processing_op, "none", sizeof(processing_op) - 1);
  processing_op[sizeof(processing_op) - 1] = '\0';
  return;
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

static const char* lcd_backlight_state_label() {
  if (g_sleep_transition) {
    return "transition";
  }
  if (g_in_light_sleep) {
    return "sleep";
  }
  if (!g_panel_enabled) {
    return "panel_off";
  }
  if (g_lvgl_running) {
    return "awake";
  }
  return "idle";
}

static void lcd_set_backlight_level(int level, const char* reason) {
  int target = (level > 0) ? 255 : 0;
  if (target > 0 && !g_backlight_initialized) {
    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
    g_backlight_initialized = true;
  }
  if (g_backlight_initialized) {
    setUpdutySubdivide(target);
  }
  g_backlight_duty = target;
  Serial.printf("[BL] set level=%d reason=%s state=%s\n",
                target,
                reason ? reason : "unknown",
                lcd_backlight_state_label());
}

static void lcd_set_backlight_binary(bool on, const char* reason) {
  lcd_set_backlight_level(on ? 255 : 0, reason);
}

static void lcd_set_idle_screen_dark(bool dark, const char* reason) {
  if (dark) {
    if (g_idle_screen_dark || g_sleep_transition || g_in_light_sleep || g_ota_mode_active) {
      return;
    }
    if (g_lvgl_running && lv_is_initialized()) {
      lcd_lvgl_wait_tx_done(100);
    }
    if (g_panel_enabled) {
      lcd_panel_set_power(false);
      g_panel_enabled = false;
    }
    lcd_set_backlight_binary(false, reason ? reason : "idle_dark");
    g_lvgl_running = false;
    g_idle_screen_dark = true;
    Serial.printf("[DISPLAY] idle_dark reason=%s backlight=%d panel_on=%d lvgl_running=%d\n",
                  reason ? reason : "idle_dark",
                  g_backlight_duty,
                  g_panel_enabled ? 1 : 0,
                  g_lvgl_running ? 1 : 0);
    return;
  }

  if (!g_idle_screen_dark) {
    return;
  }
  if (!g_panel_enabled) {
    lcd_panel_set_power(true);
    g_panel_enabled = true;
    delay(20);
  }
  if (g_backlight_duty == 0) {
    lcd_set_backlight_binary(true, reason ? reason : "idle_wake");
  }
  g_lvgl_running = true;
  g_idle_screen_dark = false;
  Serial.printf("[DISPLAY] idle_wake reason=%s backlight=%d panel_on=%d lvgl_running=%d\n",
                reason ? reason : "idle_wake",
                g_backlight_duty,
                g_panel_enabled ? 1 : 0,
                g_lvgl_running ? 1 : 0);
}

static void ensure_awake_for_ui(const char* reason) {
  // Only call this on real user input (touch/scroll/pull-to-refresh).
  cancel_pending_sleep_for_user_input(reason);
  if (g_ota_mode_active) {
    if (g_lcd_maintenance_headless) {
      Serial.printf("[WAKE_UI] exit headless ota_mode reason=%s\n",
                    reason ? reason : "unknown");
      lcd_clear_maintenance_state("user_input", true);
      lcd_exit_ota_mode("user_input");
    } else {
      Serial.printf("[WAKE_UI] ignored (ota_mode) reason=%s\n", reason ? reason : "unknown");
      return;
    }
  }
  if (g_in_light_sleep || g_sleep_transition || (g_backlight_duty == 0) || !g_panel_enabled || !g_lvgl_running) {
    g_idle_screen_dark = false;
    if (g_sleep_transition) {
      g_sleep_transition = false;
    }
    if (g_in_light_sleep) {
      g_in_light_sleep = false;
    }
    if (g_backlight_duty == 0) {
      lcd_set_backlight_binary(true, reason ? reason : "wake_ui");
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

static void abort_sleep_transition(const char* reason) {
  Serial.printf("[SLEEP_ABORT] reason=%s in_sleep=%d transition=%d backlight=%d panel_on=%d lvgl_running=%d\n",
                reason ? reason : "unknown",
                g_in_light_sleep ? 1 : 0,
                g_sleep_transition ? 1 : 0,
                g_backlight_duty,
                g_panel_enabled ? 1 : 0,
                g_lvgl_running ? 1 : 0);
  unsigned long now_ms = millis();
  bool need_wake = g_in_light_sleep || g_sleep_transition ||
                   (g_backlight_duty == 0) || !g_panel_enabled || !g_lvgl_running;
  if (need_wake) {
    g_idle_screen_dark = false;
    if (g_sleep_transition) {
      g_sleep_transition = false;
    }
    if (g_in_light_sleep) {
      g_in_light_sleep = false;
    }
    if (g_backlight_duty == 0) {
      lcd_set_backlight_binary(true, reason ? reason : "wake_ui");
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
  resetActivityTimer();
  Serial.printf("[SLEEP_ABORT] recovered reason=%s backlight=%d panel_on=%d lvgl_running=%d\n",
                reason ? reason : "unknown",
                g_backlight_duty,
                g_panel_enabled ? 1 : 0,
                g_lvgl_running ? 1 : 0);
}

static bool lcd_enter_ota_mode(uint32_t min_internal_free) {
  if (g_ota_mode_active) {
    const uint32_t internal_free =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internal_largest =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[LCD_OTA] ota_mode_heap internal_free=%u internal_largest=%u min_free=%u\n",
                  (unsigned)internal_free, (unsigned)internal_largest,
                  (unsigned)min_internal_free);
    return internal_free >= min_internal_free;
  }
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
  ui_reset_lvgl_objects();
  if (g_lcd_initialized) {
    lcd_panel_set_power(false);
    lcd_panel_deinit();
    g_lcd_initialized = false;
  }
  lcd_set_backlight_binary(true, "ota_mode");
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
    if (!g_ship_ota_wake_window && !g_lcd_maintenance_wake_window) {
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

static void lcd_enter_maintenance_headless(const char* reason) {
  if (g_lcd_maintenance_headless) {
    return;
  }
  g_lcd_maintenance_headless = true;
  bool ota_ok = lcd_enter_ota_mode(0);
  g_ui_initialized = false;
  g_lvgl_running = false;
  g_panel_enabled = false;
  lcd_set_backlight_binary(false, reason ? reason : "maintenance_headless");
  Serial.printf("[LCD_MAINT] headless_ui=1 reason=%s ota_mode=%d\n",
                reason ? reason : "unknown", ota_ok ? 1 : 0);
}

static void lcd_exit_ota_mode(const char* reason) {
  if (!g_ota_mode_active) {
    return;
  }
  g_ota_mode_active = false;
  g_sleep_transition = false;
  if (ui_task_handle != NULL) {
    vTaskDelete(ui_task_handle);
    ui_task_handle = NULL;
  }
  g_ui_initialized = false;
  g_lvgl_running = false;
  g_panel_enabled = false;
  lcd_set_backlight_binary(true, reason ? reason : "ota_exit");
  ui_reset_lvgl_objects();
  init_ui_stack(g_saved_list_count);
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

static void ui_log_asset(const char* reason, const char* screen, const char* asset_name) {
  Serial.printf("[ASSET] screen=%s reason=%s asset=\"%s\"\n",
                screen ? screen : "unknown",
                reason ? reason : "unknown",
                asset_name ? asset_name : "unknown");
}

typedef struct {
  const char* name;
  const lv_img_dsc_t* image;
} ui_asset_entry_t;

static const ui_asset_entry_t k_ui_assets[] = {
  {"ui_img_Frame_439_png", &ui_img_Frame_439_png},
  {"ui_img_Frame_439_1_png", &ui_img_Frame_439_1_png},
  {"ui_img_Frame_439_2_png", &ui_img_Frame_439_2_png},
  {"ui_img_Frame_443_png", &ui_img_Frame_443_png},
  {"ui_img_Frame_443_1_png", &ui_img_Frame_443_1_png},
  {"ui_img_Frame_493_png", &ui_img_Frame_493_png},
};

static void ui_log_asset_list_once() {
  static bool logged = false;
  if (logged) {
    return;
  }
  logged = true;
  for (size_t i = 0; i < (sizeof(k_ui_assets) / sizeof(k_ui_assets[0])); ++i) {
    Serial.printf("[ASSET_LIST] idx=%u name=\"%s\"\n",
                  (unsigned)i,
                  k_ui_assets[i].name ? k_ui_assets[i].name : "unknown");
  }
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
  bool hold_high = g_sleep_transition || g_in_light_sleep || sleep_ready_received;
  if (!hold_high) {
    lcd_wake_pin_set_mode(INT_PIN, INPUT_PULLUP);
  }
  Serial.printf("[SLEEP] release_wake_line reason=%s level=%d\n",
                reason ? reason : "unknown",
                digitalRead(INT_PIN));
  Serial.printf("[LCD_WAKE_PIN] mode=%s pullup=%d level=%d phase=release\n",
                hold_high ? "OUT" : "IN",
                hold_high ? 0 : 1,
                digitalRead(INT_PIN));
}

static void status_screen_use_text(const char* text) {
  if (g_sleep_transition) {
    return;
  }
  if (!status_screen || !status_label) {
    return;
  }
  status_screen_auto_hide_at_ms = 0;
  lv_obj_set_style_bg_img_src(status_screen, NULL, LV_PART_MAIN);
  lv_obj_set_style_bg_color(status_screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_label_set_text(status_label, text ? text : "");
  lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
}

static void show_auto_hiding_status_message(const char* text, unsigned long duration_ms) {
  if (!status_screen || !status_label) {
    return;
  }
  status_screen_use_text(text);
  lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  status_screen_shown_time = millis();
  status_screen_auto_hide_at_ms = status_screen_shown_time + duration_ms;
  set_status_reset_visible(false);
  lv_timer_handler();
}

static void status_screen_use_image(const lv_img_dsc_t* image) {
  status_screen_use_image_named(image, "status_image", "status");
}

static void status_screen_use_image_named(const lv_img_dsc_t* image, const char* asset_name, const char* reason) {
  if (!status_screen) {
    return;
  }
  if (!image) {
    ui_log_asset(reason, "STATUS", "missing->none");
    lv_obj_set_style_bg_img_src(status_screen, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(status_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_screen, LV_OPA_COVER, LV_PART_MAIN);
    set_status_reset_visible(false);
    return;
  }
  ui_log_asset(reason, "STATUS", asset_name);
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

static void provision_qr_wait_begin(const char* reason) {
  provision_qr_waiting = true;
  provision_qr_wait_start_ms = millis();
  Serial.printf("[PROVISION] wait_for_qr begin reason=%s\n", reason ? reason : "unknown");
}

static void provision_qr_wait_clear(const char* reason) {
  if (!provision_qr_waiting) {
    return;
  }
  provision_qr_waiting = false;
  provision_qr_wait_start_ms = 0;
  Serial.printf("[PROVISION] wait_for_qr clear reason=%s\n", reason ? reason : "unknown");
}

static void provision_cache_qr(const char* ssid, const char* password, const char* url) {
  if (!ssid) ssid = "";
  if (!password) password = "";
  if (!url || !url[0]) url = "http://192.168.4.1";
  strncpy(provision_qr_ssid, ssid, sizeof(provision_qr_ssid) - 1);
  provision_qr_ssid[sizeof(provision_qr_ssid) - 1] = '\0';
  strncpy(provision_qr_password, password, sizeof(provision_qr_password) - 1);
  provision_qr_password[sizeof(provision_qr_password) - 1] = '\0';
  strncpy(provision_qr_url, url, sizeof(provision_qr_url) - 1);
  provision_qr_url[sizeof(provision_qr_url) - 1] = '\0';
  provision_qr_cached = true;
}

static void create_provision_screen_if_needed() {
  if (provision_screen != NULL) {
    return;
  }
  provision_screen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(provision_screen, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_bg_color(provision_screen, lv_color_hex(0xDDF7EA), 0);
  lv_obj_set_style_bg_opa(provision_screen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(provision_screen, 0, 0);
  lv_obj_clear_flag(provision_screen, LV_OBJ_FLAG_SCROLLABLE);

  provision_title_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_title_label, "Halo Wi-Fi Setup");
  lv_obj_set_style_text_color(provision_title_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_title_label, LV_ALIGN_TOP_MID, 0, 16);
  lv_obj_add_flag(provision_title_label, LV_OBJ_FLAG_HIDDEN);

  provision_qr = NULL;

  provision_ssid_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_ssid_label, "SSID: -");
  lv_obj_set_style_text_color(provision_ssid_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_ssid_label, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_add_flag(provision_ssid_label, LV_OBJ_FLAG_HIDDEN);

  provision_url_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_url_label, "Open: http://192.168.4.1");
  lv_obj_set_style_text_color(provision_url_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_url_label, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_add_flag(provision_url_label, LV_OBJ_FLAG_HIDDEN);

  provision_status_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_status_label, "Setup Mode");
  lv_obj_set_style_text_color(provision_status_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_status_label, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_add_flag(provision_status_label, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
}

static void create_provision_intro_screen_if_needed() {
  if (provision_intro_screen != NULL) {
    return;
  }
  provision_intro_screen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(provision_intro_screen, LV_HOR_RES, LV_VER_RES);
  lv_obj_clear_flag(provision_intro_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_img_src(provision_intro_screen, &ui_img_Frame_493_png, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_img_opa(provision_intro_screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_add_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
}

static void show_provision_intro_screen(const char* reason) {
  create_provision_intro_screen_if_needed();
  if (provision_intro_visible) {
    return;
  }
  if (list_container) lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  if (menu_screen) lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  if (meal_result_screen) lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  if (logged_screen) lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
  if (expiry_screen) lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
  if (provision_screen) lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_screen_visible = false;
  lv_obj_clear_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
  provision_intro_visible = true;
  provision_intro_pending = false;
  ui_screen_state = SCREEN_SETTINGS;
  Serial.printf("[PROVISION] intro_show reason=%s\n", reason ? reason : "unknown");
  lv_timer_handler();
}

static void hide_provision_intro_screen(const char* reason) {
  if (!provision_intro_screen || !provision_intro_visible) {
    return;
  }
  lv_obj_add_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
  provision_intro_visible = false;
  provision_intro_pending = false;
  Serial.printf("[PROVISION] intro_hide reason=%s\n", reason ? reason : "unknown");
}

static void show_provisioning_screen(const char* ssid, const char* password, const char* url) {
  create_provision_screen_if_needed();
  if (!ssid) ssid = "";
  if (!password) password = "";
  if (!url || !url[0]) url = "http://192.168.4.1";
  hide_provision_intro_screen("qr_show");
  provision_qr_wait_clear("qr_shown");

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
    lv_obj_align(provision_qr, LV_ALIGN_CENTER, 0, 0);
  } else {
    QRDisplay::updateQRCodeWidget(provision_qr, qr_data);
  }

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
  provision_qr_wait_clear("qr_hidden");

  // Restore list if available
  if (list_container && g_active.count > 0) {
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  lv_timer_handler();
}

static void ship_style_plain_screen(lv_obj_t* screen, lv_color_t bg_color);
static void ship_anim_set_y(void* obj, int32_t v);
static void ship_anim_set_x(void* obj, int32_t v);
static void ship_anim_set_text_opa(void* obj, int32_t v);
static void ship_anim_set_obj_opa(void* obj, int32_t v);
static void ship_anim_set_zoom(void* obj, int32_t v);
static void ship_anim_set_border_opa(void* obj, int32_t v);

// ── UI Functions ────────────────────────────────────────────────────
static int ship_main_menu_button_index_for_action(ship_menu_action_t action) {
  switch (action) {
    case SHIP_MENU_ACTION_LOG_DISH: return 0;
    case SHIP_MENU_ACTION_CHECK_IN: return 1;
    case SHIP_MENU_ACTION_CHECK_OUT: return 2;
    case SHIP_MENU_ACTION_MORE: return 3;
    default: return -1;
  }
}

static bool ship_main_menu_is_ai_action(const ship_menu_hitbox_t* hb) {
  return hb != NULL && hb->action == SHIP_MENU_ACTION_AI;
}

static int ship_main_menu_button_target_y(int index) {
  switch (index) {
    case 0: return SHIP_MAIN_MENU_TOP_BTN_Y;
    case 1: return SHIP_MAIN_MENU_LEFT_BTN_Y;
    case 2: return SHIP_MAIN_MENU_RIGHT_BTN_Y;
    case 3: return SHIP_MAIN_MENU_BOTTOM_BTN_Y;
    default: return 0;
  }
}

static void ship_main_menu_style_button(lv_obj_t* btn, bool primary) {
  if (!btn) {
    return;
  }
  lv_obj_set_size(btn, SHIP_MAIN_MENU_BTN_W, SHIP_MAIN_MENU_BTN_H);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(btn, SHIP_MAIN_MENU_BTN_RADIUS, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn,
                            primary ? lv_color_hex(0x6EE7B7) : lv_color_hex(0x16335E),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, primary ? 0 : 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x355D93), LV_PART_MAIN);
  lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
}

static void ship_main_menu_set_ai_hold_active(bool active) {
  if (!ship_main_menu_ai_button || !ship_main_menu_ai_label) {
    return;
  }
  lv_obj_set_style_bg_color(ship_main_menu_ai_button,
                            active ? lv_color_hex(0x245DFF) : lv_color_hex(0x6EE7B7),
                            LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_main_menu_ai_button, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_main_menu_ai_button, active ? 18 : 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(ship_main_menu_ai_button, lv_color_hex(0x245DFF), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(ship_main_menu_ai_button, active ? LV_OPA_60 : LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_main_menu_ai_label,
                              active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x0E2547),
                              LV_PART_MAIN);
}

static void ship_init_ai_listening_screen() {
  if (ship_ai_listening_screen) {
    return;
  }

  ship_ai_listening_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_ai_listening_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_ai_listening_screen, LV_OBJ_FLAG_SCROLLABLE);
  ship_style_plain_screen(ship_ai_listening_screen, lv_color_hex(0x6EE7B7));
  lv_obj_set_style_border_width(ship_ai_listening_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_ai_listening_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_screen, 0, LV_PART_MAIN);

  ship_ai_listening_ring = lv_arc_create(ship_ai_listening_screen);
  lv_obj_remove_style(ship_ai_listening_ring, NULL, LV_PART_KNOB);
  lv_obj_set_size(ship_ai_listening_ring, 164, 164);
  lv_obj_align(ship_ai_listening_ring, LV_ALIGN_CENTER, 0, -8);
  lv_arc_set_range(ship_ai_listening_ring, 0, 1000);
  lv_arc_set_value(ship_ai_listening_ring, 1000);
  lv_arc_set_bg_angles(ship_ai_listening_ring, 0, 360);
  lv_arc_set_rotation(ship_ai_listening_ring, 270);
  lv_obj_set_style_arc_width(ship_ai_listening_ring, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_ai_listening_ring, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_ai_listening_ring, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ship_ai_listening_ring, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_ai_listening_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_outline_width(ship_ai_listening_ring, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_ai_listening_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  ship_ai_listening_mic_head = lv_obj_create(ship_ai_listening_screen);
  lv_obj_set_size(ship_ai_listening_mic_head, 48, 70);
  lv_obj_align(ship_ai_listening_mic_head, LV_ALIGN_CENTER, 0, -18);
  lv_obj_set_style_radius(ship_ai_listening_mic_head, 24, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_head, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_head, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_head, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_head, 0, LV_PART_MAIN);

  ship_ai_listening_mic_stem = lv_obj_create(ship_ai_listening_screen);
  lv_obj_set_size(ship_ai_listening_mic_stem, 8, 28);
  lv_obj_align(ship_ai_listening_mic_stem, LV_ALIGN_CENTER, 0, 36);
  lv_obj_set_style_radius(ship_ai_listening_mic_stem, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_stem, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_stem, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_stem, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_stem, 0, LV_PART_MAIN);

  ship_ai_listening_mic_base = lv_obj_create(ship_ai_listening_screen);
  lv_obj_set_size(ship_ai_listening_mic_base, 44, 6);
  lv_obj_align(ship_ai_listening_mic_base, LV_ALIGN_CENTER, 0, 56);
  lv_obj_set_style_radius(ship_ai_listening_mic_base, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_base, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_base, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_base, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_base, 0, LV_PART_MAIN);

  ship_ai_listening_title = lv_label_create(ship_ai_listening_screen);
  lv_label_set_text(ship_ai_listening_title, "Listening");
  lv_obj_set_style_text_font(ship_ai_listening_title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_ai_listening_title, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_ai_listening_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_ai_listening_title, LV_ALIGN_CENTER, 0, -136);

  ship_ai_listening_hint = lv_label_create(ship_ai_listening_screen);
  lv_label_set_text(ship_ai_listening_hint, "Release to return");
  lv_obj_set_style_text_font(ship_ai_listening_hint, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_ai_listening_hint, lv_color_hex(0x16335E), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_ai_listening_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_ai_listening_hint, LV_ALIGN_CENTER, 0, -102);
}

static void ship_start_ai_listening_animation() {
  if (!ship_ai_listening_ring || !ship_ai_listening_mic_head || !ship_ai_listening_mic_stem ||
      !ship_ai_listening_mic_base || !ship_ai_listening_title || !ship_ai_listening_hint) {
    return;
  }

  lv_anim_del(ship_ai_listening_ring, NULL);
  lv_anim_del(ship_ai_listening_mic_head, NULL);
  lv_anim_del(ship_ai_listening_mic_stem, NULL);
  lv_anim_del(ship_ai_listening_mic_base, NULL);
  lv_anim_del(ship_ai_listening_title, NULL);
  lv_anim_del(ship_ai_listening_hint, NULL);

  lv_obj_set_style_opa(ship_ai_listening_mic_head, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_opa(ship_ai_listening_mic_stem, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_opa(ship_ai_listening_mic_base, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_ai_listening_title, -118);
  lv_obj_set_style_text_opa(ship_ai_listening_title, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_ai_listening_hint, -84);
  lv_obj_set_style_text_opa(ship_ai_listening_hint, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t mic_head_fade;
  lv_anim_init(&mic_head_fade);
  lv_anim_set_var(&mic_head_fade, ship_ai_listening_mic_head);
  lv_anim_set_values(&mic_head_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&mic_head_fade, 220);
  lv_anim_set_delay(&mic_head_fade, 70);
  lv_anim_set_exec_cb(&mic_head_fade, ship_anim_set_obj_opa);
  lv_anim_start(&mic_head_fade);

  lv_anim_t mic_stem_fade;
  lv_anim_init(&mic_stem_fade);
  lv_anim_set_var(&mic_stem_fade, ship_ai_listening_mic_stem);
  lv_anim_set_values(&mic_stem_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&mic_stem_fade, 220);
  lv_anim_set_delay(&mic_stem_fade, 100);
  lv_anim_set_exec_cb(&mic_stem_fade, ship_anim_set_obj_opa);
  lv_anim_start(&mic_stem_fade);

  lv_anim_t mic_base_fade;
  lv_anim_init(&mic_base_fade);
  lv_anim_set_var(&mic_base_fade, ship_ai_listening_mic_base);
  lv_anim_set_values(&mic_base_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&mic_base_fade, 220);
  lv_anim_set_delay(&mic_base_fade, 130);
  lv_anim_set_exec_cb(&mic_base_fade, ship_anim_set_obj_opa);
  lv_anim_start(&mic_base_fade);

  lv_anim_t title_fade;
  lv_anim_init(&title_fade);
  lv_anim_set_var(&title_fade, ship_ai_listening_title);
  lv_anim_set_values(&title_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&title_fade, 240);
  lv_anim_set_delay(&title_fade, 60);
  lv_anim_set_exec_cb(&title_fade, ship_anim_set_text_opa);
  lv_anim_start(&title_fade);

  lv_anim_t title_y;
  lv_anim_init(&title_y);
  lv_anim_set_var(&title_y, ship_ai_listening_title);
  lv_anim_set_values(&title_y, -118, -136);
  lv_anim_set_time(&title_y, 300);
  lv_anim_set_delay(&title_y, 60);
  lv_anim_set_path_cb(&title_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&title_y, ship_anim_set_y);
  lv_anim_start(&title_y);

  lv_anim_t hint_fade;
  lv_anim_init(&hint_fade);
  lv_anim_set_var(&hint_fade, ship_ai_listening_hint);
  lv_anim_set_values(&hint_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&hint_fade, 220);
  lv_anim_set_delay(&hint_fade, 120);
  lv_anim_set_exec_cb(&hint_fade, ship_anim_set_text_opa);
  lv_anim_start(&hint_fade);

  lv_anim_t hint_y;
  lv_anim_init(&hint_y);
  lv_anim_set_var(&hint_y, ship_ai_listening_hint);
  lv_anim_set_values(&hint_y, -84, -102);
  lv_anim_set_time(&hint_y, 300);
  lv_anim_set_delay(&hint_y, 120);
  lv_anim_set_path_cb(&hint_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&hint_y, ship_anim_set_y);
  lv_anim_start(&hint_y);

}

static void ship_show_ai_listening_screen() {
  ship_init_ai_listening_screen();
  ship_ai_listening_countdown_start_ms = 0;
  ship_update_ai_listening_countdown();
  ui_screen_state = SCREEN_AI_LISTENING;
  ui_busy = false;
  lv_scr_load(ship_ai_listening_screen);
  ship_start_ai_listening_animation();
  Serial.println("[AI] screen=LISTENING");
  lv_timer_handler();
}

static void ship_queue_voice_input(const char* type, const char* wake_reason) {
  if (!type || !type[0]) {
    return;
  }
  if (strcmp(type, "INPUT_LONG_PRESS_START") == 0) {
    g_voice_fire_and_forget_ignore_ui = false;
    waiting_for_voice_response = false;
    voice_response_deadline_ms = 0;
    g_ship_voice_json_pending = false;
    g_ship_voice_json_text[0] = '\0';
    stop_glowing_animation();
  }
  if (wake_reason && wake_reason[0]) {
    request_sense_wake(wake_reason);
  }
  tx_msg_t tx_msg = {};
  strncpy(tx_msg.type, type, sizeof(tx_msg.type) - 1);
  if (uart_tx_queue != NULL) {
    xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
  }
  Serial.printf("[AI] queued type=%s wake_reason=%s\n",
                type,
                wake_reason ? wake_reason : "");
}

static void ship_service_voice_end_resend(unsigned long now) {
  if (ship_voice_end_resends_remaining == 0 || now < ship_voice_end_resend_due_ms) {
    return;
  }
  ship_queue_voice_input("INPUT_LONG_PRESS_END", NULL);
  ship_voice_end_resends_remaining--;
  ship_voice_end_resend_due_ms = now + 220;
  Serial.printf("[AI] resend INPUT_LONG_PRESS_END remaining=%u\n",
                (unsigned)ship_voice_end_resends_remaining);
}

static void ship_voice_ui_copy(char* dst, size_t dst_size, const char* src) {
  if (!dst || dst_size == 0) {
    return;
  }
  const unsigned char* s = (const unsigned char*)(src ? src : "");
  size_t out = 0;
  while (*s && out < dst_size - 1) {
    if (*s < 0x80) {
      char c = (char)(*s++);
      if (c == '\r') {
        continue;
      }
      if (c == '\t') {
        c = ' ';
      } else if (((unsigned char)c) < 0x20 && c != '\n') {
        c = ' ';
      }
      dst[out++] = c;
      continue;
    }

    if (s[0] == 0xC2 && s[1] == 0xA0) {  // non-breaking space
      dst[out++] = ' ';
      s += 2;
      continue;
    }
    if (s[0] == 0xE2 && s[1] == 0x80) {
      unsigned char third = s[2];
      if (third == 0x93 || third == 0x94) {  // en/em dash
        dst[out++] = '-';
        s += 3;
        continue;
      }
      if (third == 0xA6) {  // ellipsis
        if (out < dst_size - 3) {
          dst[out++] = '.';
          dst[out++] = '.';
          dst[out++] = '.';
        } else {
          dst[out++] = '.';
        }
        s += 3;
        continue;
      }
      if (third == 0x98 || third == 0x99) {  // curly apostrophes
        dst[out++] = '\'';
        s += 3;
        continue;
      }
      if (third == 0x9C || third == 0x9D) {  // curly quotes
        dst[out++] = '"';
        s += 3;
        continue;
      }
      if (third == 0xA2) {  // bullet
        dst[out++] = '-';
        s += 3;
        continue;
      }
    }

    int skip = 1;
    if ((*s & 0xE0) == 0xC0) {
      skip = 2;
    } else if ((*s & 0xF0) == 0xE0) {
      skip = 3;
    } else if ((*s & 0xF8) == 0xF0) {
      skip = 4;
    }
    if (out == 0 || dst[out - 1] != ' ') {
      dst[out++] = ' ';
    }
    s += skip;
  }
  dst[out] = '\0';
}

static bool ship_voice_ui_title_is_generic(const char* title) {
  if (!title || !title[0]) {
    return true;
  }
  if (strcmp(title, "Assistant") == 0 ||
      strcmp(title, "Assistant 2") == 0 ||
      strcmp(title, "Assistant 3") == 0 ||
      strcmp(title, "Assistant 4") == 0 ||
      strcmp(title, "Assistant 5") == 0 ||
      strcmp(title, "Assistant 6") == 0) {
    return true;
  }
  if (strcmp(title, "Voice Response") == 0) {
    return true;
  }
  return false;
}

static void ship_voice_ui_append(char* dst, size_t dst_size, const char* text, const char* separator) {
  if (!dst || dst_size == 0 || !text || !text[0]) {
    return;
  }
  size_t used = strlen(dst);
  if (used >= dst_size - 1) {
    return;
  }
  if (used > 0 && separator && separator[0]) {
    strncat(dst, separator, dst_size - strlen(dst) - 1);
  }
  strncat(dst, text, dst_size - strlen(dst) - 1);
}

static const char* ship_voice_ui_fallback_title(const char* response_type,
                                                bool error_present,
                                                size_t quick_item_count) {
  if (error_present) {
    return "Voice Error";
  }
  if (response_type && strcmp(response_type, "clarification_needed") == 0) {
    return "Question";
  }
  if (quick_item_count > 0 || (response_type && strcmp(response_type, "action_ack") == 0)) {
    return "Done";
  }
  if (response_type &&
      (strcmp(response_type, "info_answer") == 0 || strcmp(response_type, "paged_answer") == 0)) {
    return "Answer";
  }
  return "Voice Response";
}

static uint8_t ship_voice_ui_paginate_text(const char* text,
                                           const char* base_title,
                                           const char* style,
                                           const char* footer) {
  if (!text || !text[0]) {
    return 0;
  }

  const size_t kFallbackPageChars = 220;
  const size_t text_len = strlen(text);
  size_t start = 0;
  uint8_t page_count = 0;

  while (start < text_len && page_count < SHIP_VOICE_UI_MAX_PAGES) {
    while (start < text_len && (text[start] == '\n' || text[start] == ' ')) {
      start++;
    }
    if (start >= text_len) {
      break;
    }

    size_t end = start + kFallbackPageChars;
    if (end < text_len) {
      size_t best = end;
      for (size_t i = end; i > start + (kFallbackPageChars / 2); --i) {
        char c = text[i];
        if (c == '\n' || c == '.' || c == ';' || c == ',' || c == ' ') {
          best = (c == ' ') ? i : (i + 1);
          break;
        }
      }
      end = best;
    } else {
      end = text_len;
    }

    while (end > start && (text[end - 1] == '\n' || text[end - 1] == ' ')) {
      end--;
    }
    if (end <= start) {
      break;
    }

    ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[page_count];
    page->valid = true;
    ship_voice_ui_copy(page->template_name, sizeof(page->template_name), "title_body");
    ship_voice_ui_copy(page->style, sizeof(page->style), style && style[0] ? style : "default");
    ship_voice_ui_copy(page->title, sizeof(page->title),
                       page_count == 0 ? base_title : "More");

    char chunk[512];
    size_t chunk_len = end - start;
    if (chunk_len >= sizeof(chunk)) {
      chunk_len = sizeof(chunk) - 1;
    }
    memcpy(chunk, text + start, chunk_len);
    chunk[chunk_len] = '\0';
    ship_voice_ui_copy(page->body, sizeof(page->body), chunk);

    if (page_count == 0 && footer && footer[0]) {
      ship_voice_ui_copy(page->footer, sizeof(page->footer), footer);
    }

    page_count++;
    start = end;
  }

  return page_count;
}

static void ship_voice_ui_reset() {
  memset(g_ship_voice_ui_pages, 0, sizeof(g_ship_voice_ui_pages));
  g_ship_voice_ui_page_count = 0;
  g_ship_voice_ui_page_index = 0;
  g_ship_voice_ui_wrap = false;
  g_ship_voice_ui_show_page_dots = true;
  g_ship_voice_ui_show_page_count = false;
  g_ship_voice_ui_structured = false;
}

static bool ship_voice_ui_add_item(ship_voice_ui_page_t* page, const char* text) {
  if (!page || !text || !text[0] || page->item_count >= SHIP_VOICE_UI_MAX_ITEMS) {
    return false;
  }
  ship_voice_ui_copy(page->items[page->item_count], sizeof(page->items[page->item_count]), text);
  page->item_count++;
  return true;
}

static void ship_voice_ui_parse_items(JsonArray items, ship_voice_ui_page_t* page) {
  if (items.isNull() || !page) {
    return;
  }
  for (JsonVariant v : items) {
    if (page->item_count >= SHIP_VOICE_UI_MAX_ITEMS) {
      break;
    }
    if (v.is<const char*>()) {
      ship_voice_ui_add_item(page, v.as<const char*>());
    } else if (v.is<JsonObject>()) {
      JsonObject item = v.as<JsonObject>();
      const char* text = item["text"] | "";
      if (!text[0]) {
        text = item["item"] | "";
      }
      ship_voice_ui_add_item(page, text);
    }
  }
}

static void ship_voice_ui_parse_blocks(JsonArray blocks, ship_voice_ui_page_t* page) {
  if (blocks.isNull() || !page) {
    return;
  }
  for (JsonObject block : blocks) {
    const char* block_type = block["type"] | "";
    if (strcmp(block_type, "heading") == 0) {
      const char* text = block["text"] | "";
      if (!page->title[0]) {
        ship_voice_ui_copy(page->title, sizeof(page->title), text);
      } else {
        ship_voice_ui_append(page->body, sizeof(page->body), text, "\n\n");
      }
    } else if (strcmp(block_type, "paragraph") == 0) {
      ship_voice_ui_append(page->body, sizeof(page->body), block["text"] | "", "\n\n");
    } else if (strcmp(block_type, "footer") == 0) {
      ship_voice_ui_copy(page->footer, sizeof(page->footer), block["text"] | "");
    } else if (strcmp(block_type, "list") == 0) {
      ship_voice_ui_parse_items(block["items"].as<JsonArray>(), page);
    }
  }
}

static bool ship_voice_ui_footer_is_chrome(const char* footer) {
  if (!footer || !footer[0]) {
    return true;
  }
  if (strncmp(footer, "Page ", 5) == 0 && strstr(footer, " of ") != NULL) {
    return true;
  }
  if (strstr(footer, "Turn knob") != NULL ||
      strstr(footer, "Tap to") != NULL ||
      strstr(footer, "Swipe") != NULL) {
    return true;
  }
  return false;
}

static const char* ship_voice_ui_generic_error_text() {
  return "Sorry, something went wrong.";
}

static const char* ship_voice_ui_generic_format_text() {
  return "Sorry, I couldn't format that response.";
}

static void ship_voice_ui_parse_screen(JsonObject screen,
                                       ship_voice_ui_page_t* page,
                                       const char* fallback_title) {
  if (screen.isNull() || !page) {
    return;
  }
  page->valid = true;
  ship_voice_ui_copy(page->id, sizeof(page->id), screen["id"] | "");
  ship_voice_ui_copy(page->template_name, sizeof(page->template_name), screen["template"] | "title_body");
  ship_voice_ui_copy(page->style, sizeof(page->style), screen["style"] | "default");
  const char* screen_title = screen["title"] | "";
  const char* preferred_title = screen_title[0] ? screen_title : (fallback_title ? fallback_title : "");
  if (ship_voice_ui_title_is_generic(preferred_title)) {
    preferred_title = "Response";
  }
  ship_voice_ui_copy(page->title, sizeof(page->title), preferred_title);
  ship_voice_ui_copy(page->subtitle, sizeof(page->subtitle), screen["subtitle"] | "");
  ship_voice_ui_copy(page->body, sizeof(page->body), screen["body"] | "");
  ship_voice_ui_copy(page->footer, sizeof(page->footer), screen["footer"] | "");
  ship_voice_ui_parse_items(screen["items"].as<JsonArray>(), page);
  ship_voice_ui_parse_blocks(screen["blocks"].as<JsonArray>(), page);
}

static int ship_voice_ui_layout_label(lv_obj_t* label,
                                      const char* text,
                                      int y,
                                      int gap_after) {
  if (!label) {
    return y;
  }
  if (!text || !text[0]) {
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return y;
  }
  lv_label_set_text(label, text);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_update_layout(ship_voice_json_card);
  return y + lv_obj_get_height(label) + gap_after;
}

static void ship_voice_ui_build_fallback_page(JsonObject doc) {
  JsonVariant error = doc["error"];
  JsonArray quick_items = doc["quickItems"].as<JsonArray>();
  const char* response_type = doc["type"] | "";
  const char* text = doc["text"] | "";
  const char* transcript = doc["transcript"] | "";
  bool error_present = !error.isNull();
  const char* title = ship_voice_ui_fallback_title(response_type, error_present, quick_items.size());
  const char* style = error_present ? "error" : "default";
  const char* body_text = NULL;

  if (!error.isNull() && error.is<const char*>()) {
    body_text = error.as<const char*>();
  } else if (text[0]) {
    body_text = text;
  } else if (error_present) {
    body_text = ship_voice_ui_generic_error_text();
  } else if (transcript[0]) {
    body_text = transcript;
  } else {
    body_text = ship_voice_ui_generic_format_text();
  }

  char footer[96] = {0};
  if (transcript[0] && body_text && strcmp(transcript, body_text) != 0) {
    ship_voice_ui_copy(footer, sizeof(footer), transcript);
  }

  uint8_t page_count = ship_voice_ui_paginate_text(body_text, title, style, footer);
  if (page_count == 0) {
    page_count = ship_voice_ui_paginate_text(ship_voice_ui_generic_format_text(), title, style, footer);
  }

  if (page_count == 0) {
    ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[0];
    page->valid = true;
    ship_voice_ui_copy(page->template_name, sizeof(page->template_name), "title_body");
    ship_voice_ui_copy(page->style, sizeof(page->style), style);
    ship_voice_ui_copy(page->title, sizeof(page->title), title);
    ship_voice_ui_copy(page->body, sizeof(page->body), ship_voice_ui_generic_format_text());
    page_count = 1;
  }

  if (page_count > 0) {
    ship_voice_ui_parse_items(quick_items, &g_ship_voice_ui_pages[0]);
  }
  g_ship_voice_ui_page_count = page_count;
  g_ship_voice_ui_show_page_dots = page_count > 1;
  g_ship_voice_ui_show_page_count = false;
}

static bool ship_voice_ui_parse_response(const char* json_text) {
  ship_voice_ui_reset();
  if (!json_text || !json_text[0]) {
    return false;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, json_text);
  if (error) {
    Serial.printf("[VOICE_UI] parse error=%s len=%u\n", error.c_str(), (unsigned)strlen(json_text));
    ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[0];
    page->valid = true;
    ship_voice_ui_copy(page->template_name, sizeof(page->template_name), "title_body");
    ship_voice_ui_copy(page->style, sizeof(page->style), "error");
    ship_voice_ui_copy(page->title, sizeof(page->title), "Voice Error");
    ship_voice_ui_copy(page->body, sizeof(page->body), ship_voice_ui_generic_format_text());
    g_ship_voice_ui_page_count = 1;
    g_ship_voice_ui_show_page_dots = false;
    g_ship_voice_ui_show_page_count = false;
    return false;
  }

  JsonObject ui = doc["ui"].as<JsonObject>();
  JsonArray screens = ui["screens"].as<JsonArray>();
  Serial.printf("[VOICE_UI] parsed type=%s ui=%d screens=%u\n",
                doc["type"] | "",
                ui.isNull() ? 0 : 1,
                screens.isNull() ? 0 : (unsigned)screens.size());
  if (!ui.isNull() && !screens.isNull() && screens.size() > 0) {
    const char* ui_title = ui["title"] | "";
    JsonObject navigation = ui["navigation"].as<JsonObject>();
    g_ship_voice_ui_wrap = navigation["wrap"] | false;
    g_ship_voice_ui_show_page_dots = navigation["showPageDots"] | true;
    g_ship_voice_ui_show_page_count = false;

    uint8_t page_count = screens.size();
    if (page_count > SHIP_VOICE_UI_MAX_PAGES) {
      page_count = SHIP_VOICE_UI_MAX_PAGES;
    }
    for (uint8_t i = 0; i < page_count; ++i) {
      JsonObject screen = screens[i].as<JsonObject>();
      ship_voice_ui_parse_screen(screen, &g_ship_voice_ui_pages[i], ui_title);
    }
    g_ship_voice_ui_page_count = page_count;
    g_ship_voice_ui_structured = true;
    return true;
  }

  ship_voice_ui_build_fallback_page(doc.as<JsonObject>());
  return true;
}

static void ship_voice_ui_apply_style(const ship_voice_ui_page_t* page) {
  lv_color_t screen_bg = lv_color_hex(0x0E2547);
  lv_color_t card_bg = lv_color_hex(0x16345E);
  lv_color_t accent = lv_color_hex(0x7DD3FC);

  if (page) {
    if (strcmp(page->style, "warning") == 0) {
      accent = lv_color_hex(0xFBBF24);
      card_bg = lv_color_hex(0x3A2A12);
    } else if (strcmp(page->style, "success") == 0) {
      accent = lv_color_hex(0x6EE7B7);
      card_bg = lv_color_hex(0x153E38);
    } else if (strcmp(page->style, "error") == 0) {
      accent = lv_color_hex(0xFCA5A5);
      card_bg = lv_color_hex(0x451A25);
      screen_bg = lv_color_hex(0x2B1020);
    } else if (strcmp(page->style, "info") == 0) {
      accent = lv_color_hex(0x93C5FD);
      card_bg = lv_color_hex(0x142C52);
    }
  }

  ship_style_plain_screen(ship_voice_json_screen, screen_bg);
  lv_obj_set_style_bg_opa(ship_voice_json_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_voice_json_card, 0, LV_PART_MAIN);

  lv_obj_set_style_text_color(ship_voice_json_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_subtitle, lv_color_hex(0xD6E4FF), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_label, lv_color_hex(0xF5F9FF), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_footer, accent, LV_PART_MAIN);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    if (ship_voice_json_item_labels[i]) {
      lv_obj_set_style_text_color(ship_voice_json_item_labels[i], lv_color_hex(0xEAF4FF), LV_PART_MAIN);
    }
  }
  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    if (!ship_voice_json_page_dots[i]) {
      continue;
    }
    bool active = (i == g_ship_voice_ui_page_index);
    lv_obj_set_style_bg_color(ship_voice_json_page_dots[i], active ? accent : lv_color_hex(0x4B6387), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_voice_json_page_dots[i], active ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
  }
}

static void ship_voice_ui_render_current_page() {
  ship_init_voice_json_screen();
  if (g_ship_voice_ui_page_count == 0 || g_ship_voice_ui_page_index >= g_ship_voice_ui_page_count) {
    return;
  }

  ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[g_ship_voice_ui_page_index];
  ship_voice_ui_apply_style(page);

  int current_y = 10;
  current_y = ship_voice_ui_layout_label(ship_voice_json_title,
                                         page->title[0] ? page->title : "Response",
                                         current_y,
                                         10);
  current_y = ship_voice_ui_layout_label(ship_voice_json_subtitle,
                                         page->subtitle,
                                         current_y,
                                         12);
  current_y = ship_voice_ui_layout_label(ship_voice_json_label,
                                         page->body,
                                         current_y,
                                         page->item_count > 0 ? 14 : 10);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    if (!ship_voice_json_item_labels[i]) {
      continue;
    }
    if (i < page->item_count) {
      char line[96];
      snprintf(line, sizeof(line), "- %s", page->items[i]);
      current_y = ship_voice_ui_layout_label(ship_voice_json_item_labels[i], line, current_y, 10);
    } else {
      lv_obj_add_flag(ship_voice_json_item_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (page->footer[0] && !ship_voice_ui_footer_is_chrome(page->footer)) {
    lv_label_set_text(ship_voice_json_footer, page->footer);
    lv_obj_clear_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(ship_voice_json_card);
    if ((current_y + lv_obj_get_height(ship_voice_json_footer)) <= 212) {
      lv_obj_align(ship_voice_json_footer, LV_ALIGN_TOP_MID, 0, current_y);
    } else {
      lv_obj_add_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    lv_obj_add_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);
  }

  if (g_ship_voice_ui_page_count > 1 && g_ship_voice_ui_show_page_count) {
    char page_text[24];
    snprintf(page_text, sizeof(page_text), "%u/%u",
             (unsigned)(g_ship_voice_ui_page_index + 1),
             (unsigned)g_ship_voice_ui_page_count);
    lv_label_set_text(ship_voice_json_page_label, page_text);
    lv_obj_align(ship_voice_json_page_label, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_clear_flag(ship_voice_json_page_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(ship_voice_json_page_label, LV_OBJ_FLAG_HIDDEN);
  }

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    if (!ship_voice_json_page_dots[i]) {
      continue;
    }
    if (i < g_ship_voice_ui_page_count && g_ship_voice_ui_page_count > 1 && g_ship_voice_ui_show_page_dots) {
      int x = ((int)i - ((int)g_ship_voice_ui_page_count - 1) / 2) * 16;
      lv_obj_align(ship_voice_json_page_dots[i], LV_ALIGN_BOTTOM_MID, x, -42);
      lv_obj_clear_flag(ship_voice_json_page_dots[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ship_voice_json_page_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  lv_obj_add_flag(ship_voice_json_hint, LV_OBJ_FLAG_HIDDEN);
}

static void ship_voice_ui_handle_scroll(int delta) {
  if (g_ship_voice_ui_page_count <= 1 || delta == 0) {
    return;
  }

  int next = (int)g_ship_voice_ui_page_index + (delta > 0 ? 1 : -1);
  if (next < 0) {
    next = g_ship_voice_ui_wrap ? (int)g_ship_voice_ui_page_count - 1 : 0;
  } else if (next >= g_ship_voice_ui_page_count) {
    next = g_ship_voice_ui_wrap ? 0 : (int)g_ship_voice_ui_page_count - 1;
  }
  if (next == g_ship_voice_ui_page_index) {
    return;
  }

  g_ship_voice_ui_page_index = (uint8_t)next;
  ship_voice_ui_render_current_page();
  Serial.printf("[VOICE_UI] page=%u/%u\n",
                (unsigned)(g_ship_voice_ui_page_index + 1),
                (unsigned)g_ship_voice_ui_page_count);
}

static void ship_init_voice_json_screen() {
  if (ship_voice_json_screen) {
    return;
  }

  ship_voice_json_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_voice_json_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_voice_json_screen, LV_OBJ_FLAG_SCROLLABLE);
  ship_style_plain_screen(ship_voice_json_screen, lv_color_hex(0x0E2547));
  lv_obj_set_style_border_width(ship_voice_json_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ship_voice_json_screen, 0, LV_PART_MAIN);

  ship_voice_json_card = lv_obj_create(ship_voice_json_screen);
  lv_obj_set_size(ship_voice_json_card, 236, 248);
  lv_obj_align(ship_voice_json_card, LV_ALIGN_CENTER, 0, -6);
  lv_obj_clear_flag(ship_voice_json_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_voice_json_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_voice_json_card, 0, LV_PART_MAIN);

  ship_voice_json_title = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_title, "Voice Response");
  lv_obj_set_style_text_font(ship_voice_json_title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_width(ship_voice_json_title, 228);
  lv_label_set_long_mode(ship_voice_json_title, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(ship_voice_json_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(ship_voice_json_title, 4, LV_PART_MAIN);
  lv_obj_align(ship_voice_json_title, LV_ALIGN_TOP_MID, 0, 18);

  ship_voice_json_page_label = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_page_label, "");
  lv_obj_set_style_text_font(ship_voice_json_page_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_add_flag(ship_voice_json_page_label, LV_OBJ_FLAG_HIDDEN);

  ship_voice_json_subtitle = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_subtitle, "");
  lv_obj_set_width(ship_voice_json_subtitle, 220);
  lv_label_set_long_mode(ship_voice_json_subtitle, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ship_voice_json_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_json_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(ship_voice_json_subtitle, 5, LV_PART_MAIN);
  lv_obj_align(ship_voice_json_subtitle, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_add_flag(ship_voice_json_subtitle, LV_OBJ_FLAG_HIDDEN);

  ship_voice_json_label = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_label, "");
  lv_obj_set_width(ship_voice_json_label, 216);
  lv_label_set_long_mode(ship_voice_json_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ship_voice_json_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_json_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(ship_voice_json_label, 8, LV_PART_MAIN);
  lv_obj_align(ship_voice_json_label, LV_ALIGN_TOP_MID, 0, 70);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    ship_voice_json_item_labels[i] = lv_label_create(ship_voice_json_card);
    lv_label_set_text(ship_voice_json_item_labels[i], "");
    lv_obj_set_width(ship_voice_json_item_labels[i], 204);
    lv_label_set_long_mode(ship_voice_json_item_labels[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(ship_voice_json_item_labels[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(ship_voice_json_item_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(ship_voice_json_item_labels[i], 8, LV_PART_MAIN);
    lv_obj_add_flag(ship_voice_json_item_labels[i], LV_OBJ_FLAG_HIDDEN);
  }

  ship_voice_json_footer = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_footer, "");
  lv_obj_set_width(ship_voice_json_footer, 204);
  lv_label_set_long_mode(ship_voice_json_footer, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ship_voice_json_footer, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_json_footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(ship_voice_json_footer, 4, LV_PART_MAIN);
  lv_obj_align(ship_voice_json_footer, LV_ALIGN_BOTTOM_MID, 0, -72);
  lv_obj_add_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    ship_voice_json_page_dots[i] = lv_obj_create(ship_voice_json_screen);
    lv_obj_set_size(ship_voice_json_page_dots[i], 8, 8);
    lv_obj_set_style_radius(ship_voice_json_page_dots[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_voice_json_page_dots[i], 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_voice_json_page_dots[i], 0, LV_PART_MAIN);
    lv_obj_add_flag(ship_voice_json_page_dots[i], LV_OBJ_FLAG_HIDDEN);
  }

  ship_voice_json_hint = lv_label_create(ship_voice_json_screen);
  lv_label_set_text(ship_voice_json_hint, "Tap to exit");
  lv_obj_set_style_text_font(ship_voice_json_hint, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_json_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_flag(ship_voice_json_hint, LV_OBJ_FLAG_HIDDEN);
}

static void ship_show_voice_json_screen(const char* json_text) {
  ship_init_voice_json_screen();
  strncpy(g_ship_voice_json_text, json_text ? json_text : "", sizeof(g_ship_voice_json_text) - 1);
  g_ship_voice_json_text[sizeof(g_ship_voice_json_text) - 1] = '\0';
  g_ship_voice_json_pending = false;
  bool parsed = ship_voice_ui_parse_response(g_ship_voice_json_text[0] ? g_ship_voice_json_text : "{}");
  ship_voice_ui_render_current_page();
  ui_screen_state = SCREEN_VOICE_JSON;
  ui_busy = true;
  lv_scr_load(ship_voice_json_screen);
  Serial.printf("[VOICE] screen=RESPONSE parsed=%d pages=%u structured=%d\n",
                parsed ? 1 : 0,
                (unsigned)g_ship_voice_ui_page_count,
                g_ship_voice_ui_structured ? 1 : 0);
  lv_timer_handler();
}

static void ship_main_menu_add_caption(lv_obj_t* btn, const char* text, lv_color_t text_color) {
  if (!btn) {
    return;
  }
  lv_obj_t* caption = lv_label_create(btn);
  lv_label_set_text(caption, text);
  lv_obj_set_style_text_color(caption, text_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(caption, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(caption, SHIP_MAIN_MENU_BTN_W - 14);
  lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void ship_main_menu_add_symbol_icon(lv_obj_t* btn, const char* symbol, int y_offset, lv_color_t text_color) {
  if (!btn) {
    return;
  }
  lv_obj_t* icon = lv_label_create(btn);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_color(icon, text_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, y_offset);
}

static void ship_main_menu_add_dish_icon(lv_obj_t* btn, lv_color_t fg_color) {
  if (!btn) {
    return;
  }
  lv_obj_t* plate = lv_obj_create(btn);
  lv_obj_set_size(plate, 36, 36);
  lv_obj_align(plate, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_set_style_radius(plate, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(plate, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(plate, 3, LV_PART_MAIN);
  lv_obj_set_style_border_color(plate, fg_color, LV_PART_MAIN);
  lv_obj_set_style_outline_width(plate, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(plate, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(plate, 0, LV_PART_MAIN);

  lv_obj_t* fork = lv_obj_create(btn);
  lv_obj_set_size(fork, 4, 30);
  lv_obj_align(fork, LV_ALIGN_TOP_MID, -20, 17);
  lv_obj_set_style_radius(fork, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(fork, fg_color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(fork, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(fork, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(fork, 0, LV_PART_MAIN);

  lv_obj_t* knife = lv_obj_create(btn);
  lv_obj_set_size(knife, 4, 30);
  lv_obj_align(knife, LV_ALIGN_TOP_MID, 20, 17);
  lv_obj_set_style_radius(knife, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(knife, fg_color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(knife, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(knife, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(knife, 0, LV_PART_MAIN);
}

static void ship_main_menu_play_entry_animation() {
  static const uint16_t kEntryDelays[4] = {0, 45, 90, 135};
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* btn = ship_main_menu_buttons[i];
    if (!btn) {
      continue;
    }
    int target_y = ship_main_menu_button_target_y(i);
    lv_anim_del(btn, NULL);
    lv_obj_set_y(btn, target_y + 20);
    lv_obj_set_style_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_anim_t pos_anim;
    lv_anim_init(&pos_anim);
    lv_anim_set_var(&pos_anim, btn);
    lv_anim_set_values(&pos_anim, target_y + 20, target_y);
    lv_anim_set_time(&pos_anim, 260);
    lv_anim_set_delay(&pos_anim, kEntryDelays[i]);
    lv_anim_set_path_cb(&pos_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&pos_anim, ship_anim_set_y);
    lv_anim_start(&pos_anim);

    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, btn);
    lv_anim_set_values(&opa_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&opa_anim, 220);
    lv_anim_set_delay(&opa_anim, kEntryDelays[i]);
    lv_anim_set_exec_cb(&opa_anim, ship_anim_set_obj_opa);
    lv_anim_start(&opa_anim);
  }
}

static void ship_main_menu_reset_visual_state() {
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* btn = ship_main_menu_buttons[i];
    if (!btn) {
      continue;
    }
    lv_anim_del(btn, NULL);
    lv_obj_set_y(btn, ship_main_menu_button_target_y(i));
    lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  }
  if (ship_main_menu_ai_button) {
    lv_anim_del(ship_main_menu_ai_button, NULL);
    lv_obj_set_pos(ship_main_menu_ai_button, SHIP_MAIN_MENU_CENTER_BTN_X, SHIP_MAIN_MENU_CENTER_BTN_Y);
    lv_obj_set_style_opa(ship_main_menu_ai_button, LV_OPA_COVER, LV_PART_MAIN);
  }
  ship_main_menu_set_ai_hold_active(false);
}

static void ship_main_menu_play_tap_animation(const ship_menu_hitbox_t* hb) {
  if (!hb || ui_screen_state != SCREEN_HOME || ship_menu_screen_state != SHIP_MENU_SCREEN_MAIN) {
    return;
  }
  int index = ship_main_menu_button_index_for_action(hb->action);
  if (index < 0 || index >= 4) {
    return;
  }
  lv_obj_t* btn = ship_main_menu_buttons[index];
  if (!btn) {
    return;
  }
  int target_y = ship_main_menu_button_target_y(index);
  lv_anim_del(btn, NULL);
  lv_obj_set_y(btn, target_y);
  lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);

  lv_anim_t pos_anim;
  lv_anim_init(&pos_anim);
  lv_anim_set_var(&pos_anim, btn);
  lv_anim_set_values(&pos_anim, target_y, target_y + 6);
  lv_anim_set_time(&pos_anim, 70);
  lv_anim_set_playback_time(&pos_anim, 120);
  lv_anim_set_exec_cb(&pos_anim, ship_anim_set_y);
  lv_anim_start(&pos_anim);

  lv_anim_t opa_anim;
  lv_anim_init(&opa_anim);
  lv_anim_set_var(&opa_anim, btn);
  lv_anim_set_values(&opa_anim, LV_OPA_COVER, LV_OPA_80);
  lv_anim_set_time(&opa_anim, 70);
  lv_anim_set_playback_time(&opa_anim, 120);
  lv_anim_set_exec_cb(&opa_anim, ship_anim_set_obj_opa);
  lv_anim_start(&opa_anim);
}

static void show_ship_main_menu_impl() {
  if (!ship_menu_screen) {
    ship_menu_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_screen, LV_PCT(100), LV_PCT(100));
    ship_style_plain_screen(ship_menu_screen, lv_color_hex(0x0E2547));
    lv_obj_set_style_border_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ship_menu_screen, LV_OBJ_FLAG_SCROLLABLE);

    ship_main_menu_buttons[0] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[0], SHIP_MAIN_MENU_TOP_BTN_X, SHIP_MAIN_MENU_TOP_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[0], false);
    ship_main_menu_add_dish_icon(ship_main_menu_buttons[0], lv_color_hex(0xD6E4FF));
    ship_main_menu_add_caption(ship_main_menu_buttons[0], "Dish", lv_color_hex(0xFFFFFF));

    ship_main_menu_buttons[1] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[1], SHIP_MAIN_MENU_LEFT_BTN_X, SHIP_MAIN_MENU_LEFT_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[1], false);
    ship_main_menu_add_symbol_icon(ship_main_menu_buttons[1], "+", 12, lv_color_hex(0xD6E4FF));
    ship_main_menu_add_caption(ship_main_menu_buttons[1], "Check In", lv_color_hex(0xFFFFFF));

    ship_main_menu_buttons[2] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[2], SHIP_MAIN_MENU_RIGHT_BTN_X, SHIP_MAIN_MENU_RIGHT_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[2], false);
    ship_main_menu_add_symbol_icon(ship_main_menu_buttons[2], "-", 14, lv_color_hex(0xD6E4FF));
    ship_main_menu_add_caption(ship_main_menu_buttons[2], "Discard", lv_color_hex(0xFFFFFF));

    ship_main_menu_buttons[3] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[3], SHIP_MAIN_MENU_BOTTOM_BTN_X, SHIP_MAIN_MENU_BOTTOM_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[3], false);
    ship_main_menu_add_symbol_icon(ship_main_menu_buttons[3], "...", 14, lv_color_hex(0xD6E4FF));
    ship_main_menu_add_caption(ship_main_menu_buttons[3], "More", lv_color_hex(0xFFFFFF));

    ship_main_menu_ai_button = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_ai_button, SHIP_MAIN_MENU_CENTER_BTN_X, SHIP_MAIN_MENU_CENTER_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_ai_button, true);

    ship_main_menu_ai_label = lv_label_create(ship_main_menu_ai_button);
    lv_label_set_text(ship_main_menu_ai_label, "AI");
    lv_obj_set_style_text_font(ship_main_menu_ai_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(ship_main_menu_ai_label);
    ship_main_menu_set_ai_hold_active(false);
  }
  ui_log_asset("show_main_menu", "SHIP_MAIN_MENU", "custom_main_menu");
  ship_menu_screen_state = SHIP_MENU_SCREEN_MAIN;
  ui_screen_state = SCREEN_HOME;
  ui_busy = false;
  ship_ai_touch_active = false;
  home_shown_ms = millis();
  ship_logged_hide_at_ms = 0;
  ship_main_menu_reset_visual_state();
  lv_scr_load(ship_menu_screen);
  Serial.println("[SHIP_MENU] showing MAIN_MENU");
  lv_timer_handler();
  wifi_on_run_deferred_if_ready("main_menu");
}

static void show_ship_main_menu() {
  UI_SHOW(SCREEN_SHIP_MAIN_MENU, "show_main_menu");
}

static void show_ship_second_menu_impl() {
  if (!ship_menu_second_screen) {
    ship_menu_second_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_second_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(ship_menu_second_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_log_asset("show_second_menu", "SHIP_SECOND_MENU", "ui_img_Frame_443_1_png");
    lv_obj_set_style_bg_img_src(ship_menu_second_screen, &ui_img_Frame_443_1_png, LV_PART_MAIN);
    lv_obj_set_style_bg_img_opa(ship_menu_second_screen, LV_OPA_COVER, LV_PART_MAIN);
  }
  ui_log_asset("show_second_menu", "SHIP_SECOND_MENU", "ui_img_Frame_443_1_png");
  ship_menu_screen_state = SHIP_MENU_SCREEN_SECOND;
  ui_screen_state = SCREEN_SECOND;
  ui_busy = false;
  home_shown_ms = millis();
  lv_scr_load(ship_menu_second_screen);
  Serial.println("[MENU] screen=SECOND_MENU");
  lv_timer_handler();
}

static void show_ship_second_menu() {
  UI_SHOW(SCREEN_SHIP_SECOND_MENU, "show_second_menu");
}

static void show_ship_settings_screen_impl() {
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

    ship_menu_settings_versions = lv_label_create(ship_menu_settings_screen);
    lv_label_set_text(ship_menu_settings_versions, "");
    lv_obj_set_style_text_color(ship_menu_settings_versions, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(ship_menu_settings_versions, LV_ALIGN_TOP_MID, 0, SHIP_MENU_SETTINGS_VERSION_Y);

    ship_menu_settings_status = lv_label_create(ship_menu_settings_screen);
    lv_label_set_text(ship_menu_settings_status, "");
    lv_obj_set_style_text_color(ship_menu_settings_status, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(ship_menu_settings_status, LV_ALIGN_TOP_MID, 0, SHIP_MENU_SETTINGS_STATUS_Y);
    lv_obj_add_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN);

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

    ship_menu_settings_btn_ota = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_ota, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_OTA_Y);
    lv_obj_set_size(ship_menu_settings_btn_ota, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
    lv_obj_set_style_bg_color(ship_menu_settings_btn_ota, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_btn_ota, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_menu_settings_btn_ota, 0, LV_PART_MAIN);

    ship_menu_settings_label_ota = lv_label_create(ship_menu_settings_btn_ota);
    lv_label_set_text(ship_menu_settings_label_ota, "Run OTA Update");
    lv_obj_set_style_text_color(ship_menu_settings_label_ota, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(ship_menu_settings_label_ota);

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

    // Three-button settings screen (Reset Wi-Fi + OTA + Back)
  }
  ship_menu_screen_state = SHIP_MENU_SCREEN_SETTINGS;
  ui_screen_state = SCREEN_SETTINGS;
  ui_busy = false;
  home_shown_ms = millis();
  if (ship_menu_settings_status != NULL) {
    lv_obj_add_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN);
    ship_menu_settings_status_hide_at_ms = 0;
  }
  ship_menu_update_versions_label();
  request_sense_wake("settings_fw_info");
  ship_menu_request_fw_info();
  lv_scr_load(ship_menu_settings_screen);
  Serial.println("[MENU] screen=SETTINGS");
  lv_timer_handler();
}

static void show_ship_settings_screen() {
  UI_SHOW(SCREEN_SHIP_SETTINGS, "show_settings");
}

static void status_overlay_init() {
  return;
}

static void status_overlay_show(const char* text) {
  (void)text;
  return;
}

static void status_overlay_hide() {
  g_status_hide_at_ms = 0;
  return;
}

static void ship_hide_expiry_screen() {
  if (!expiry_screen) {
    return;
  }
  lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
  expiry_screen_visible = false;
  expiry_screen_shown_time = 0;
  if (expiry_timeout_ring) {
    lv_arc_set_value(expiry_timeout_ring, 1000);
  }
}

static void ship_hide_default_ui() {
  if (list_container) lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  if (menu_screen) lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  if (meal_result_screen) lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  if (loading_screen) lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
}

static void ship_style_plain_screen(lv_obj_t* screen, lv_color_t bg_color) {
  if (!screen) {
    return;
  }
  lv_obj_set_style_bg_img_src(screen, NULL, LV_PART_MAIN);
  lv_obj_set_style_bg_color(screen, bg_color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
}

static void expiry_update_timeout_ring() {
  if (!expiry_timeout_ring || expiry_screen_shown_time == 0) {
    return;
  }
  unsigned long elapsed_ms = millis() - expiry_screen_shown_time;
  uint32_t remaining_ms =
      (elapsed_ms >= EXPIRY_SCREEN_TIMEOUT_MS) ? 0UL : (EXPIRY_SCREEN_TIMEOUT_MS - elapsed_ms);
  uint16_t ring_value = (uint16_t)((remaining_ms * 1000UL) / EXPIRY_SCREEN_TIMEOUT_MS);
  lv_arc_set_value(expiry_timeout_ring, ring_value);
}

static void ship_update_expiry_choice_timeout_ring() {
  if (!ship_expiry_choice_timeout_ring || ship_expiry_choice_shown_time == 0) {
    return;
  }
  unsigned long elapsed_ms = millis() - ship_expiry_choice_shown_time;
  uint32_t remaining_ms =
      (elapsed_ms >= EXPIRY_SCREEN_TIMEOUT_MS) ? 0UL : (EXPIRY_SCREEN_TIMEOUT_MS - elapsed_ms);
  uint16_t ring_value = (uint16_t)((remaining_ms * 1000UL) / EXPIRY_SCREEN_TIMEOUT_MS);
  lv_arc_set_value(ship_expiry_choice_timeout_ring, ring_value);
}

static void ship_anim_set_y(void* obj, int32_t v) {
  if (obj) {
    lv_obj_set_y((lv_obj_t*)obj, v);
  }
}

static void ship_anim_set_x(void* obj, int32_t v) {
  if (obj) {
    lv_obj_set_x((lv_obj_t*)obj, v);
  }
}

static void ship_anim_set_text_opa(void* obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_text_opa((lv_obj_t*)obj, (lv_opa_t)v, LV_PART_MAIN);
  }
}

static void ship_anim_set_obj_opa(void* obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, LV_PART_MAIN);
  }
}

static void ship_anim_set_arc_opa(void* obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_arc_opa((lv_obj_t*)obj, (lv_opa_t)v, LV_PART_MAIN);
    lv_obj_set_style_arc_opa((lv_obj_t*)obj, (lv_opa_t)v, LV_PART_INDICATOR);
  }
}

static void ship_anim_set_zoom(void* obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_transform_zoom((lv_obj_t*)obj, (uint16_t)v, LV_PART_MAIN);
  }
}

static void ship_anim_set_border_opa(void* obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_border_opa((lv_obj_t*)obj, (lv_opa_t)v, LV_PART_MAIN);
  }
}

static void ship_update_ai_listening_countdown() {
  if (!ship_ai_listening_ring) {
    return;
  }

  uint16_t ring_value = 1000;
  if (long_press_sent && ship_ai_touch_active && ship_ai_listening_countdown_start_ms > 0) {
    unsigned long elapsed_ms = millis() - ship_ai_listening_countdown_start_ms;
    if (elapsed_ms >= SHIP_AI_LISTENING_COUNTDOWN_MS) {
      ring_value = 0;
    } else {
      uint32_t remaining_ms = SHIP_AI_LISTENING_COUNTDOWN_MS - elapsed_ms;
      ring_value = (uint16_t)((remaining_ms * 1000UL) / SHIP_AI_LISTENING_COUNTDOWN_MS);
    }
  }

  lv_arc_set_value((lv_obj_t*)ship_ai_listening_ring, ring_value);
}

static void ship_start_logged_success_animation() {
  if (!ship_logged_icon || !ship_logged_label || !ship_logged_subtitle) {
    return;
  }

  lv_obj_set_y(ship_logged_icon, -86);
  lv_obj_set_style_text_opa(ship_logged_icon, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_logged_label, 40);
  lv_obj_set_style_text_opa(ship_logged_label, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_logged_subtitle, 92);
  lv_obj_set_style_text_opa(ship_logged_subtitle, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t icon_fade;
  lv_anim_init(&icon_fade);
  lv_anim_set_var(&icon_fade, ship_logged_icon);
  lv_anim_set_values(&icon_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&icon_fade, 260);
  lv_anim_set_exec_cb(&icon_fade, ship_anim_set_text_opa);
  lv_anim_start(&icon_fade);

  lv_anim_t icon_y;
  lv_anim_init(&icon_y);
  lv_anim_set_var(&icon_y, ship_logged_icon);
  lv_anim_set_values(&icon_y, -86, -42);
  lv_anim_set_time(&icon_y, 320);
  lv_anim_set_path_cb(&icon_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&icon_y, ship_anim_set_y);
  lv_anim_start(&icon_y);

  lv_anim_t label_fade;
  lv_anim_init(&label_fade);
  lv_anim_set_var(&label_fade, ship_logged_label);
  lv_anim_set_values(&label_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&label_fade, 260);
  lv_anim_set_delay(&label_fade, 90);
  lv_anim_set_exec_cb(&label_fade, ship_anim_set_text_opa);
  lv_anim_start(&label_fade);

  lv_anim_t label_y;
  lv_anim_init(&label_y);
  lv_anim_set_var(&label_y, ship_logged_label);
  lv_anim_set_values(&label_y, 40, 8);
  lv_anim_set_time(&label_y, 320);
  lv_anim_set_delay(&label_y, 90);
  lv_anim_set_path_cb(&label_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&label_y, ship_anim_set_y);
  lv_anim_start(&label_y);

  lv_anim_t subtitle_fade;
  lv_anim_init(&subtitle_fade);
  lv_anim_set_var(&subtitle_fade, ship_logged_subtitle);
  lv_anim_set_values(&subtitle_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&subtitle_fade, 260);
  lv_anim_set_delay(&subtitle_fade, 150);
  lv_anim_set_exec_cb(&subtitle_fade, ship_anim_set_text_opa);
  lv_anim_start(&subtitle_fade);

  lv_anim_t subtitle_y;
  lv_anim_init(&subtitle_y);
  lv_anim_set_var(&subtitle_y, ship_logged_subtitle);
  lv_anim_set_values(&subtitle_y, 92, 52);
  lv_anim_set_time(&subtitle_y, 320);
  lv_anim_set_delay(&subtitle_y, 150);
  lv_anim_set_path_cb(&subtitle_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&subtitle_y, ship_anim_set_y);
  lv_anim_start(&subtitle_y);
}

static void ship_start_voice_ack_animation() {
  if (!ship_voice_ack_icon || !ship_voice_ack_label || !ship_voice_ack_subtitle) {
    return;
  }

  lv_anim_del(ship_voice_ack_icon, NULL);
  lv_anim_del(ship_voice_ack_label, NULL);
  lv_anim_del(ship_voice_ack_subtitle, NULL);

  lv_obj_set_y(ship_voice_ack_icon, -86);
  lv_obj_set_style_text_opa(ship_voice_ack_icon, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_voice_ack_label, 40);
  lv_obj_set_style_text_opa(ship_voice_ack_label, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_voice_ack_subtitle, 92);
  lv_obj_set_style_text_opa(ship_voice_ack_subtitle, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t icon_fade;
  lv_anim_init(&icon_fade);
  lv_anim_set_var(&icon_fade, ship_voice_ack_icon);
  lv_anim_set_values(&icon_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&icon_fade, 220);
  lv_anim_set_exec_cb(&icon_fade, ship_anim_set_text_opa);
  lv_anim_start(&icon_fade);

  lv_anim_t icon_y;
  lv_anim_init(&icon_y);
  lv_anim_set_var(&icon_y, ship_voice_ack_icon);
  lv_anim_set_values(&icon_y, -86, -42);
  lv_anim_set_time(&icon_y, 300);
  lv_anim_set_path_cb(&icon_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&icon_y, ship_anim_set_y);
  lv_anim_start(&icon_y);

  lv_anim_t label_fade;
  lv_anim_init(&label_fade);
  lv_anim_set_var(&label_fade, ship_voice_ack_label);
  lv_anim_set_values(&label_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&label_fade, 220);
  lv_anim_set_delay(&label_fade, 70);
  lv_anim_set_exec_cb(&label_fade, ship_anim_set_text_opa);
  lv_anim_start(&label_fade);

  lv_anim_t label_y;
  lv_anim_init(&label_y);
  lv_anim_set_var(&label_y, ship_voice_ack_label);
  lv_anim_set_values(&label_y, 40, 8);
  lv_anim_set_time(&label_y, 300);
  lv_anim_set_delay(&label_y, 70);
  lv_anim_set_path_cb(&label_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&label_y, ship_anim_set_y);
  lv_anim_start(&label_y);

  lv_anim_t subtitle_fade;
  lv_anim_init(&subtitle_fade);
  lv_anim_set_var(&subtitle_fade, ship_voice_ack_subtitle);
  lv_anim_set_values(&subtitle_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&subtitle_fade, 220);
  lv_anim_set_delay(&subtitle_fade, 130);
  lv_anim_set_exec_cb(&subtitle_fade, ship_anim_set_text_opa);
  lv_anim_start(&subtitle_fade);

  lv_anim_t subtitle_y;
  lv_anim_init(&subtitle_y);
  lv_anim_set_var(&subtitle_y, ship_voice_ack_subtitle);
  lv_anim_set_values(&subtitle_y, 92, 52);
  lv_anim_set_time(&subtitle_y, 300);
  lv_anim_set_delay(&subtitle_y, 130);
  lv_anim_set_path_cb(&subtitle_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&subtitle_y, ship_anim_set_y);
  lv_anim_start(&subtitle_y);
}

static void ship_update_hold_still_countdown() {
  if (!ship_hold_countdown_label || ship_hold_anim_start_ms == 0) {
    return;
  }

  unsigned long elapsed_ms = millis() - ship_hold_anim_start_ms;
  bool capture_phase = (elapsed_ms >= SHIP_HOLD_COUNTDOWN_MS);
  if (ship_hold_ring) {
    uint16_t ring_value = 0;
    if (!capture_phase) {
      uint32_t remaining_ms = SHIP_HOLD_COUNTDOWN_MS - elapsed_ms;
      ring_value = (uint16_t)((remaining_ms * 1000UL) / SHIP_HOLD_COUNTDOWN_MS);
    } else {
      uint32_t pulse_ms = (elapsed_ms - SHIP_HOLD_COUNTDOWN_MS) % SHIP_HOLD_CAPTURE_PULSE_MS;
      if (pulse_ms < (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL)) {
        ring_value = (uint16_t)(280UL + ((pulse_ms * 720UL) / (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL)));
      } else {
        ring_value = (uint16_t)(1000UL - (((pulse_ms - (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL)) * 720UL) /
                                          (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL)));
      }
    }
    lv_arc_set_value(ship_hold_ring, ring_value);
  }

  if (!capture_phase) {
    if (ship_hold_capture_phase) {
      ship_hold_capture_phase = false;
      ship_hold_capture_frame = -1;
      if (ship_hold_capture_icon) {
        lv_obj_add_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_opa(ship_hold_capture_icon, LV_OPA_0, LV_PART_MAIN);
        lv_obj_set_style_transform_zoom(ship_hold_capture_icon, 256, LV_PART_MAIN);
      }
      lv_obj_clear_flag(ship_hold_countdown_label, LV_OBJ_FLAG_HIDDEN);
      if (ship_hold_subtitle) {
        lv_label_set_text(ship_hold_subtitle, "Capturing your item/meal");
      }
    }

    int countdown = 5;
    countdown = 5 - (int)(elapsed_ms / 1000UL);
    if (countdown < 1) {
      countdown = 1;
    }

    if (countdown == ship_hold_countdown_value) {
      return;
    }

    ship_hold_countdown_value = countdown;
    char countdown_text[4];
    snprintf(countdown_text, sizeof(countdown_text), "%d", countdown);
    lv_label_set_text(ship_hold_countdown_label, countdown_text);
    return;
  }

  if (!ship_hold_capture_phase) {
    ship_hold_capture_phase = true;
    lv_obj_add_flag(ship_hold_countdown_label, LV_OBJ_FLAG_HIDDEN);
    if (ship_hold_capture_icon) {
      lv_obj_clear_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (ship_hold_subtitle) {
      lv_label_set_text(ship_hold_subtitle, "Capturing image...");
    }
  }

  int capture_frame = (int)(((elapsed_ms - SHIP_HOLD_COUNTDOWN_MS) / 125UL) % 8UL);
  if (capture_frame == ship_hold_capture_frame) {
    return;
  }

  ship_hold_capture_frame = capture_frame;
  ship_hold_countdown_value = -1;
  if (ship_hold_capture_icon) {
    uint32_t pulse_ms = (elapsed_ms - SHIP_HOLD_COUNTDOWN_MS) % SHIP_HOLD_CAPTURE_PULSE_MS;
    bool expanding = pulse_ms < (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL);
    uint32_t pulse_half_ms = SHIP_HOLD_CAPTURE_PULSE_MS / 2UL;
    uint32_t pulse_pos = expanding ? pulse_ms : (pulse_ms - pulse_half_ms);
    uint16_t zoom = expanding
                        ? (uint16_t)(256UL + ((pulse_pos * 96UL) / pulse_half_ms))
                        : (uint16_t)(352UL - ((pulse_pos * 96UL) / pulse_half_ms));
    lv_opa_t icon_opa = (lv_opa_t)(expanding
                                       ? (LV_OPA_70 + ((pulse_pos * (LV_OPA_COVER - LV_OPA_70)) / pulse_half_ms))
                                       : (LV_OPA_COVER - ((pulse_pos * (LV_OPA_COVER - LV_OPA_70)) / pulse_half_ms)));
    lv_obj_set_style_transform_zoom(ship_hold_capture_icon, zoom, LV_PART_MAIN);
    lv_obj_set_style_text_opa(ship_hold_capture_icon, icon_opa, LV_PART_MAIN);
    lv_obj_align(ship_hold_capture_icon, LV_ALIGN_CENTER, 0, 12);
  }
}

static void ship_start_hold_still_animation() {
  if (!ship_hold_title || !ship_hold_ring || !ship_hold_countdown_label || !ship_hold_subtitle || !ship_hold_capture_icon) {
    return;
  }

  ship_hold_anim_start_ms = millis();
  ship_hold_countdown_value = -1;
  ship_hold_capture_frame = -1;
  ship_hold_capture_phase = false;
  lv_label_set_text(ship_hold_subtitle, "Capturing your item/meal");
  lv_obj_clear_flag(ship_hold_countdown_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_opa(ship_hold_capture_icon, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_transform_zoom(ship_hold_capture_icon, 256, LV_PART_MAIN);
  ship_update_hold_still_countdown();

  lv_anim_del(ship_hold_title, NULL);
  lv_anim_del(ship_hold_ring, NULL);
  lv_anim_del(ship_hold_countdown_label, NULL);
  lv_anim_del(ship_hold_capture_icon, NULL);
  lv_anim_del(ship_hold_subtitle, NULL);

  lv_obj_set_y(ship_hold_title, -132);
  lv_obj_set_style_text_opa(ship_hold_title, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_hold_ring, 72);
  ship_anim_set_arc_opa(ship_hold_ring, LV_OPA_0);
  lv_obj_set_y(ship_hold_countdown_label, 24);
  lv_obj_set_style_text_opa(ship_hold_countdown_label, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_hold_capture_icon, 18);
  lv_obj_set_style_text_opa(ship_hold_capture_icon, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_hold_subtitle, 118);
  lv_obj_set_style_text_opa(ship_hold_subtitle, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t title_fade;
  lv_anim_init(&title_fade);
  lv_anim_set_var(&title_fade, ship_hold_title);
  lv_anim_set_values(&title_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&title_fade, 280);
  lv_anim_set_exec_cb(&title_fade, ship_anim_set_text_opa);
  lv_anim_start(&title_fade);

  lv_anim_t title_y;
  lv_anim_init(&title_y);
  lv_anim_set_var(&title_y, ship_hold_title);
  lv_anim_set_values(&title_y, -132, -82);
  lv_anim_set_time(&title_y, 340);
  lv_anim_set_path_cb(&title_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&title_y, ship_anim_set_y);
  lv_anim_start(&title_y);

  lv_anim_t ring_fade;
  lv_anim_init(&ring_fade);
  lv_anim_set_var(&ring_fade, ship_hold_ring);
  lv_anim_set_values(&ring_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&ring_fade, 300);
  lv_anim_set_delay(&ring_fade, 90);
  lv_anim_set_exec_cb(&ring_fade, ship_anim_set_arc_opa);
  lv_anim_start(&ring_fade);

  lv_anim_t ring_y;
  lv_anim_init(&ring_y);
  lv_anim_set_var(&ring_y, ship_hold_ring);
  lv_anim_set_values(&ring_y, 72, 12);
  lv_anim_set_time(&ring_y, 360);
  lv_anim_set_delay(&ring_y, 90);
  lv_anim_set_path_cb(&ring_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&ring_y, ship_anim_set_y);
  lv_anim_start(&ring_y);

  lv_anim_t count_fade;
  lv_anim_init(&count_fade);
  lv_anim_set_var(&count_fade, ship_hold_countdown_label);
  lv_anim_set_values(&count_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&count_fade, 260);
  lv_anim_set_delay(&count_fade, 150);
  lv_anim_set_exec_cb(&count_fade, ship_anim_set_text_opa);
  lv_anim_start(&count_fade);

  lv_anim_t count_y;
  lv_anim_init(&count_y);
  lv_anim_set_var(&count_y, ship_hold_countdown_label);
  lv_anim_set_values(&count_y, 24, 12);
  lv_anim_set_time(&count_y, 320);
  lv_anim_set_delay(&count_y, 150);
  lv_anim_set_path_cb(&count_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&count_y, ship_anim_set_y);
  lv_anim_start(&count_y);

  lv_anim_t subtitle_fade;
  lv_anim_init(&subtitle_fade);
  lv_anim_set_var(&subtitle_fade, ship_hold_subtitle);
  lv_anim_set_values(&subtitle_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&subtitle_fade, 240);
  lv_anim_set_delay(&subtitle_fade, 220);
  lv_anim_set_exec_cb(&subtitle_fade, ship_anim_set_text_opa);
  lv_anim_start(&subtitle_fade);

  lv_anim_t subtitle_y;
  lv_anim_init(&subtitle_y);
  lv_anim_set_var(&subtitle_y, ship_hold_subtitle);
  lv_anim_set_values(&subtitle_y, 118, 86);
  lv_anim_set_time(&subtitle_y, 320);
  lv_anim_set_delay(&subtitle_y, 220);
  lv_anim_set_path_cb(&subtitle_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&subtitle_y, ship_anim_set_y);
  lv_anim_start(&subtitle_y);
}

static void ship_start_expiry_choice_animation() {
  if (!expiry_choice_quantity_label || !ship_expiry_choice_skip_btn || !ship_expiry_choice_add_btn) {
    return;
  }

  lv_anim_del(expiry_choice_quantity_label, NULL);
  if (ship_expiry_choice_qty_prefix) {
    lv_anim_del(ship_expiry_choice_qty_prefix, NULL);
  }
  if (ship_expiry_choice_prompt) {
    lv_anim_del(ship_expiry_choice_prompt, NULL);
  }
  lv_anim_del(ship_expiry_choice_skip_btn, NULL);
  lv_anim_del(ship_expiry_choice_add_btn, NULL);

  lv_obj_set_y(expiry_choice_quantity_label, -36);
  lv_obj_set_style_text_opa(expiry_choice_quantity_label, LV_OPA_0, LV_PART_MAIN);
  if (ship_expiry_choice_qty_prefix) {
    lv_obj_set_y(ship_expiry_choice_qty_prefix, -28);
    lv_obj_set_style_text_opa(ship_expiry_choice_qty_prefix, LV_OPA_0, LV_PART_MAIN);
  }
  if (ship_expiry_choice_prompt) {
    lv_obj_set_y(ship_expiry_choice_prompt, 2);
    lv_obj_set_style_text_opa(ship_expiry_choice_prompt, LV_OPA_0, LV_PART_MAIN);
  }
  lv_obj_set_x(ship_expiry_choice_skip_btn, -220);
  lv_obj_set_style_opa(ship_expiry_choice_skip_btn, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_x(ship_expiry_choice_add_btn, 220);
  lv_obj_set_style_opa(ship_expiry_choice_add_btn, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t qty_fade;
  lv_anim_init(&qty_fade);
  lv_anim_set_var(&qty_fade, expiry_choice_quantity_label);
  lv_anim_set_values(&qty_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&qty_fade, 240);
  lv_anim_set_exec_cb(&qty_fade, ship_anim_set_text_opa);
  lv_anim_start(&qty_fade);

  lv_anim_t qty_y;
  lv_anim_init(&qty_y);
  lv_anim_set_var(&qty_y, expiry_choice_quantity_label);
  lv_anim_set_values(&qty_y, -36, SHIP_CHOICE_VALUE_Y);
  lv_anim_set_time(&qty_y, 320);
  lv_anim_set_path_cb(&qty_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&qty_y, ship_anim_set_y);
  lv_anim_start(&qty_y);

  if (ship_expiry_choice_qty_prefix) {
    lv_anim_t qty_prefix_fade;
    lv_anim_init(&qty_prefix_fade);
    lv_anim_set_var(&qty_prefix_fade, ship_expiry_choice_qty_prefix);
    lv_anim_set_values(&qty_prefix_fade, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&qty_prefix_fade, 220);
    lv_anim_set_exec_cb(&qty_prefix_fade, ship_anim_set_text_opa);
    lv_anim_start(&qty_prefix_fade);

    lv_anim_t qty_prefix_y;
    lv_anim_init(&qty_prefix_y);
    lv_anim_set_var(&qty_prefix_y, ship_expiry_choice_qty_prefix);
    lv_anim_set_values(&qty_prefix_y, -28, SHIP_CHOICE_VALUE_Y);
    lv_anim_set_time(&qty_prefix_y, 300);
    lv_anim_set_path_cb(&qty_prefix_y, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&qty_prefix_y, ship_anim_set_y);
    lv_anim_start(&qty_prefix_y);
  }

  if (ship_expiry_choice_prompt) {
    lv_anim_t prompt_fade;
    lv_anim_init(&prompt_fade);
    lv_anim_set_var(&prompt_fade, ship_expiry_choice_prompt);
    lv_anim_set_values(&prompt_fade, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&prompt_fade, 220);
    lv_anim_set_delay(&prompt_fade, 70);
    lv_anim_set_exec_cb(&prompt_fade, ship_anim_set_text_opa);
    lv_anim_start(&prompt_fade);

    lv_anim_t prompt_y;
    lv_anim_init(&prompt_y);
    lv_anim_set_var(&prompt_y, ship_expiry_choice_prompt);
    lv_anim_set_values(&prompt_y, 2, SHIP_CHOICE_PROMPT_Y);
    lv_anim_set_time(&prompt_y, 300);
    lv_anim_set_delay(&prompt_y, 70);
    lv_anim_set_path_cb(&prompt_y, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&prompt_y, ship_anim_set_y);
    lv_anim_start(&prompt_y);
  }

  lv_anim_t skip_fade;
  lv_anim_init(&skip_fade);
  lv_anim_set_var(&skip_fade, ship_expiry_choice_skip_btn);
  lv_anim_set_values(&skip_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&skip_fade, 240);
  lv_anim_set_delay(&skip_fade, 120);
  lv_anim_set_exec_cb(&skip_fade, ship_anim_set_obj_opa);
  lv_anim_start(&skip_fade);

  lv_anim_t skip_x;
  lv_anim_init(&skip_x);
  lv_anim_set_var(&skip_x, ship_expiry_choice_skip_btn);
  lv_anim_set_values(&skip_x, -220, -84);
  lv_anim_set_time(&skip_x, 340);
  lv_anim_set_delay(&skip_x, 120);
  lv_anim_set_path_cb(&skip_x, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&skip_x, ship_anim_set_x);
  lv_anim_start(&skip_x);

  lv_anim_t add_fade;
  lv_anim_init(&add_fade);
  lv_anim_set_var(&add_fade, ship_expiry_choice_add_btn);
  lv_anim_set_values(&add_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&add_fade, 240);
  lv_anim_set_delay(&add_fade, 180);
  lv_anim_set_exec_cb(&add_fade, ship_anim_set_obj_opa);
  lv_anim_start(&add_fade);

  lv_anim_t add_x;
  lv_anim_init(&add_x);
  lv_anim_set_var(&add_x, ship_expiry_choice_add_btn);
  lv_anim_set_values(&add_x, 220, 84);
  lv_anim_set_time(&add_x, 340);
  lv_anim_set_delay(&add_x, 180);
  lv_anim_set_path_cb(&add_x, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&add_x, ship_anim_set_x);
  lv_anim_start(&add_x);
}

static void ship_init_hold_still() {
  // LCD_Minimal/LCD_Minimal.ino: ship_init_hold_still
  if (ship_hold_screen) {
    return;
  }
  ship_hold_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_hold_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_hold_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_HOLD_STILL", "plain_screen");
  ship_style_plain_screen(ship_hold_screen, lv_color_hex(0x0E2547));

  ship_hold_title = lv_label_create(ship_hold_screen);
  lv_label_set_text(ship_hold_title, "Hold Still");
  lv_obj_set_style_text_font(ship_hold_title, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_hold_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_hold_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_hold_title, LV_ALIGN_CENTER, 0, -82);

  ship_hold_ring = lv_arc_create(ship_hold_screen);
  lv_obj_remove_style(ship_hold_ring, NULL, LV_PART_KNOB);
  lv_obj_set_size(ship_hold_ring, 104, 104);
  lv_arc_set_range(ship_hold_ring, 0, 1000);
  lv_arc_set_value(ship_hold_ring, 1000);
  lv_arc_set_bg_angles(ship_hold_ring, 0, 360);
  lv_arc_set_rotation(ship_hold_ring, 270);
  lv_obj_set_style_arc_width(ship_hold_ring, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_hold_ring, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ship_hold_ring, lv_color_hex(0x284A78), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ship_hold_ring, (lv_opa_t)110, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ship_hold_ring, lv_color_hex(0x6EE7B7), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_hold_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_outline_width(ship_hold_ring, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_hold_ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_hold_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(ship_hold_ring, LV_ALIGN_CENTER, 0, 12);

  ship_hold_countdown_label = lv_label_create(ship_hold_screen);
  lv_label_set_text(ship_hold_countdown_label, "5");
  lv_obj_set_style_text_font(ship_hold_countdown_label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_hold_countdown_label, lv_color_hex(0x6EE7B7), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_hold_countdown_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_hold_countdown_label, LV_ALIGN_CENTER, 0, 12);

  ship_hold_capture_icon = lv_label_create(ship_hold_screen);
  lv_label_set_text(ship_hold_capture_icon, LV_SYMBOL_IMAGE);
  lv_obj_set_style_text_font(ship_hold_capture_icon, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_hold_capture_icon, lv_color_hex(0x6EE7B7), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_hold_capture_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_transform_zoom(ship_hold_capture_icon, 256, LV_PART_MAIN);
  lv_obj_align(ship_hold_capture_icon, LV_ALIGN_CENTER, 0, 12);
  lv_obj_add_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);

  ship_hold_subtitle = lv_label_create(ship_hold_screen);
  lv_label_set_text(ship_hold_subtitle, "Capturing your item/meal");
  lv_obj_set_style_text_font(ship_hold_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_hold_subtitle, lv_color_hex(0xD6E4FF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_hold_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_hold_subtitle, LV_ALIGN_CENTER, 0, 86);
}

static void ship_init_expiry_choice() {
  if (ship_expiry_choice_screen) {
    return;
  }
  ship_expiry_choice_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_expiry_choice_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_expiry_choice_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_EXPIRY_CHOICE", "plain_screen");
  ship_style_plain_screen(ship_expiry_choice_screen, lv_color_hex(0x0E2547));
  lv_obj_set_style_border_width(ship_expiry_choice_screen, 0, LV_PART_MAIN);

  ship_expiry_choice_timeout_ring = lv_arc_create(ship_expiry_choice_screen);
  lv_obj_remove_style(ship_expiry_choice_timeout_ring, NULL, LV_PART_KNOB);
  lv_obj_set_size(ship_expiry_choice_timeout_ring, 356, 356);
  lv_arc_set_range(ship_expiry_choice_timeout_ring, 0, 1000);
  lv_arc_set_value(ship_expiry_choice_timeout_ring, 1000);
  lv_arc_set_bg_angles(ship_expiry_choice_timeout_ring, 0, 360);
  lv_arc_set_rotation(ship_expiry_choice_timeout_ring, 270);
  lv_obj_set_style_arc_width(ship_expiry_choice_timeout_ring, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_expiry_choice_timeout_ring, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_expiry_choice_timeout_ring, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ship_expiry_choice_timeout_ring, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_expiry_choice_timeout_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_outline_width(ship_expiry_choice_timeout_ring, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_expiry_choice_timeout_ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_expiry_choice_timeout_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(ship_expiry_choice_timeout_ring);

  expiry_choice_quantity_label = lv_label_create(ship_expiry_choice_screen);
  lv_label_set_text(expiry_choice_quantity_label, "1");
  lv_obj_set_style_text_font(expiry_choice_quantity_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(expiry_choice_quantity_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(expiry_choice_quantity_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(expiry_choice_quantity_label, LV_ALIGN_TOP_MID, 26, SHIP_CHOICE_VALUE_Y);

  ship_expiry_choice_qty_prefix = lv_label_create(ship_expiry_choice_screen);
  lv_label_set_text(ship_expiry_choice_qty_prefix, "QTY:");
  lv_obj_set_style_text_font(ship_expiry_choice_qty_prefix, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_expiry_choice_qty_prefix, lv_color_hex(0xD6E4FF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_expiry_choice_qty_prefix, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_expiry_choice_qty_prefix, LV_ALIGN_TOP_MID, -26, SHIP_CHOICE_VALUE_Y);

  ship_expiry_choice_prompt = lv_label_create(ship_expiry_choice_screen);
  lv_label_set_text(ship_expiry_choice_prompt, "Turn knob to change QTY");
  lv_obj_set_style_text_font(ship_expiry_choice_prompt, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_expiry_choice_prompt, lv_color_hex(0xD6E4FF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_expiry_choice_prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_expiry_choice_prompt, LV_ALIGN_TOP_MID, 0, SHIP_CHOICE_PROMPT_Y);

  ship_expiry_choice_skip_btn = lv_btn_create(ship_expiry_choice_screen);
  lv_obj_set_size(ship_expiry_choice_skip_btn, 135, SHIP_CHOICE_BUTTON_HEIGHT);
  lv_obj_set_style_radius(ship_expiry_choice_skip_btn, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_expiry_choice_skip_btn, lv_color_hex(0x16335E), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_expiry_choice_skip_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_expiry_choice_skip_btn, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(ship_expiry_choice_skip_btn, lv_color_hex(0x355D93), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_expiry_choice_skip_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_expiry_choice_skip_btn, 0, LV_PART_MAIN);
  lv_obj_align(ship_expiry_choice_skip_btn, LV_ALIGN_CENTER, -84, SHIP_CHOICE_BUTTON_Y);

  ship_expiry_choice_skip_label = lv_label_create(ship_expiry_choice_skip_btn);
  lv_label_set_text(ship_expiry_choice_skip_label, "Skip");
  lv_obj_set_style_text_font(ship_expiry_choice_skip_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_expiry_choice_skip_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_expiry_choice_skip_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(ship_expiry_choice_skip_label);

  ship_expiry_choice_add_btn = lv_btn_create(ship_expiry_choice_screen);
  lv_obj_set_size(ship_expiry_choice_add_btn, 135, SHIP_CHOICE_BUTTON_HEIGHT);
  lv_obj_set_style_radius(ship_expiry_choice_add_btn, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_expiry_choice_add_btn, lv_color_hex(0x6EE7B7), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_expiry_choice_add_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_expiry_choice_add_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_expiry_choice_add_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_expiry_choice_add_btn, 0, LV_PART_MAIN);
  lv_obj_align(ship_expiry_choice_add_btn, LV_ALIGN_CENTER, 84, SHIP_CHOICE_BUTTON_Y);

  ship_expiry_choice_add_label = lv_label_create(ship_expiry_choice_add_btn);
  lv_label_set_text(ship_expiry_choice_add_label, "Add\nExpiration");
  lv_obj_set_style_text_font(ship_expiry_choice_add_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_expiry_choice_add_label, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_expiry_choice_add_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(ship_expiry_choice_add_label, 110);
  lv_obj_center(ship_expiry_choice_add_label);
}

static void ship_init_processing() {
  // LCD_Minimal/LCD_Minimal.ino: ship_init_processing
  if (ship_processing_screen) {
    return;
  }
  ship_processing_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_processing_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_processing_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_PROCESSING", "plain_screen");
  ship_style_plain_screen(ship_processing_screen, lv_color_hex(0x0E2547));
  lv_obj_set_style_border_width(ship_processing_screen, 0, LV_PART_MAIN);

  ship_processing_fill = lv_obj_create(ship_processing_screen);
  lv_obj_set_size(ship_processing_fill, 360, 0);
  lv_obj_align(ship_processing_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_radius(ship_processing_fill, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_processing_fill, lv_color_hex(0x6EE7B7), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_processing_fill, LV_OPA_80, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_processing_fill, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_processing_fill, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_processing_fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ship_processing_fill, LV_OBJ_FLAG_HIDDEN);

  ship_processing_halo = lv_obj_create(ship_processing_screen);
  lv_obj_set_size(ship_processing_halo, 132, 132);
  lv_obj_align(ship_processing_halo, LV_ALIGN_CENTER, 0, -54);
  lv_obj_set_style_radius(ship_processing_halo, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_processing_halo, lv_color_hex(0x6EE7B7), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_processing_halo, 31, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_processing_halo, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_processing_halo, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_processing_halo, 34, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(ship_processing_halo, lv_color_hex(0x6EE7B7), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(ship_processing_halo, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_shadow_spread(ship_processing_halo, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_processing_halo, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  ship_processing_spinner = lv_spinner_create(ship_processing_screen, 1100, 90);
  lv_obj_set_size(ship_processing_spinner, 92, 92);
  lv_obj_align(ship_processing_spinner, LV_ALIGN_CENTER, 0, -54);
  lv_obj_set_style_bg_opa(ship_processing_spinner, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_processing_spinner, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ship_processing_spinner, lv_color_hex(0x24466F), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ship_processing_spinner, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_processing_spinner, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ship_processing_spinner, lv_color_hex(0x6EE7B7), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_processing_spinner, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ship_processing_spinner, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(ship_processing_spinner, true, LV_PART_INDICATOR);
  lv_obj_set_style_shadow_width(ship_processing_spinner, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_processing_spinner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  ship_processing_label = lv_label_create(ship_processing_screen);
  lv_label_set_text(ship_processing_label, "Processing");
  lv_obj_set_style_text_font(ship_processing_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_processing_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_processing_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_processing_label, LV_ALIGN_CENTER, 0, 44);

  ship_processing_subtitle = lv_label_create(ship_processing_screen);
  lv_label_set_text(ship_processing_subtitle, "Working on your request");
  lv_obj_set_style_text_font(ship_processing_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_processing_subtitle, lv_color_hex(0xD6E4FF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_processing_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_processing_subtitle, LV_ALIGN_CENTER, 0, 82);
}

static bool ship_processing_is_dish_mode() {
  return ship_mode_is_dish(g_ship_ui_op, g_ship_ui_mode);
}

static uint8_t ship_compute_dish_processing_pct(unsigned long age_ms) {
  if (age_ms >= DISH_PROCESSING_PROGRESS_TOTAL_MS) {
    return 100;
  }

  uint8_t pct = 1;
  if (age_ms < 1500UL) {
    pct = 1 + (uint8_t)((age_ms * 15UL) / 1500UL);  // 1 -> 16
  } else if (age_ms < 5000UL) {
    pct = 16 + (uint8_t)(((age_ms - 1500UL) * 24UL) / 3500UL);  // 16 -> 40
  } else if (age_ms < 9500UL) {
    pct = 40 + (uint8_t)(((age_ms - 5000UL) * 25UL) / 4500UL);  // 40 -> 65
  } else if (age_ms < 12500UL) {
    pct = 65 + (uint8_t)(((age_ms - 9500UL) * 20UL) / 3000UL);  // 65 -> 85
  } else {
    pct = 85 + (uint8_t)(((age_ms - 12500UL) * 14UL) / 2500UL);  // 85 -> 99
  }
  if (pct < 1) {
    pct = 1;
  } else if (pct > 99) {
    pct = 99;
  }
  return pct;
}

static void ship_set_processing_fill_pct(uint8_t pct) {
  if (!ship_processing_fill) {
    return;
  }
  if (pct > 100) {
    pct = 100;
  }
  uint16_t fill_h = (uint16_t)((360UL * (unsigned long)pct) / 100UL);
  if (pct > 0 && fill_h == 0) {
    fill_h = 1;
  }
  lv_obj_set_size(ship_processing_fill, 360, fill_h);
  lv_obj_align(ship_processing_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void ship_set_processing_text(const char* text) {
  if (!ship_processing_label) {
    return;
  }
  lv_label_set_text(ship_processing_label,
                    (text && text[0]) ? text : "Processing");
}

static void ship_configure_processing_layout(bool is_dish) {
  if (!ship_processing_fill || !ship_processing_halo || !ship_processing_spinner || !ship_processing_label || !ship_processing_subtitle) {
    return;
  }

  if (is_dish) {
    lv_obj_clear_flag(ship_processing_fill, LV_OBJ_FLAG_HIDDEN);
    ship_set_processing_fill_pct(dish_processing_progress_pct);
    lv_obj_add_flag(ship_processing_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(ship_processing_halo, 152, 152);
    lv_obj_align(ship_processing_halo, LV_ALIGN_CENTER, 0, -48);
    lv_obj_set_style_shadow_width(ship_processing_halo, 40, LV_PART_MAIN);
    lv_obj_set_style_text_font(ship_processing_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(ship_processing_label, LV_ALIGN_CENTER, 0, -48);
    lv_obj_set_style_text_font(ship_processing_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(ship_processing_subtitle, LV_ALIGN_CENTER, 0, 58);
  } else {
    lv_obj_add_flag(ship_processing_fill, LV_OBJ_FLAG_HIDDEN);
    ship_set_processing_fill_pct(0);
    lv_obj_clear_flag(ship_processing_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(ship_processing_halo, 132, 132);
    lv_obj_align(ship_processing_halo, LV_ALIGN_CENTER, 0, -54);
    lv_obj_set_style_shadow_width(ship_processing_halo, 34, LV_PART_MAIN);
    lv_obj_set_style_text_font(ship_processing_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(ship_processing_label, LV_ALIGN_CENTER, 0, 44);
    lv_obj_set_style_text_font(ship_processing_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(ship_processing_subtitle, LV_ALIGN_CENTER, 0, 82);
  }
}

static void ship_start_processing_animation() {
  if (!ship_processing_halo || !ship_processing_spinner || !ship_processing_label || !ship_processing_subtitle) {
    return;
  }
  bool is_dish = ship_processing_is_dish_mode();

  lv_anim_del(ship_processing_halo, NULL);
  lv_anim_del(ship_processing_label, NULL);
  lv_anim_del(ship_processing_subtitle, NULL);

  lv_obj_set_style_opa(ship_processing_halo, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_processing_halo, is_dish ? -38 : -44);
  lv_obj_set_style_opa(ship_processing_spinner, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_y(ship_processing_spinner, -54);
  lv_obj_set_style_bg_opa(ship_processing_halo, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_processing_label, is_dish ? -36 : 60);
  lv_obj_set_style_text_opa(ship_processing_label, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_processing_subtitle, is_dish ? 70 : 94);
  lv_obj_set_style_text_opa(ship_processing_subtitle, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t halo_in_opa;
  lv_anim_init(&halo_in_opa);
  lv_anim_set_var(&halo_in_opa, ship_processing_halo);
  lv_anim_set_values(&halo_in_opa, LV_OPA_0, LV_OPA_50);
  lv_anim_set_time(&halo_in_opa, 360);
  lv_anim_set_path_cb(&halo_in_opa, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&halo_in_opa, ship_anim_set_obj_opa);
  lv_anim_start(&halo_in_opa);

  lv_anim_t halo_in_y;
  lv_anim_init(&halo_in_y);
  lv_anim_set_var(&halo_in_y, ship_processing_halo);
  lv_anim_set_values(&halo_in_y, is_dish ? -38 : -44, is_dish ? -48 : -54);
  lv_anim_set_time(&halo_in_y, 360);
  lv_anim_set_path_cb(&halo_in_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&halo_in_y, ship_anim_set_y);
  lv_anim_start(&halo_in_y);

  lv_anim_t label_fade;
  lv_anim_init(&label_fade);
  lv_anim_set_var(&label_fade, ship_processing_label);
  lv_anim_set_values(&label_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&label_fade, 220);
  lv_anim_set_delay(&label_fade, 50);
  lv_anim_set_exec_cb(&label_fade, ship_anim_set_text_opa);
  lv_anim_start(&label_fade);

  lv_anim_t label_y;
  lv_anim_init(&label_y);
  lv_anim_set_var(&label_y, ship_processing_label);
  lv_anim_set_values(&label_y, is_dish ? -36 : 60, is_dish ? -48 : 44);
  lv_anim_set_time(&label_y, 280);
  lv_anim_set_delay(&label_y, 50);
  lv_anim_set_path_cb(&label_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&label_y, ship_anim_set_y);
  lv_anim_start(&label_y);

  lv_anim_t subtitle_fade;
  lv_anim_init(&subtitle_fade);
  lv_anim_set_var(&subtitle_fade, ship_processing_subtitle);
  lv_anim_set_values(&subtitle_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&subtitle_fade, 220);
  lv_anim_set_delay(&subtitle_fade, 110);
  lv_anim_set_exec_cb(&subtitle_fade, ship_anim_set_text_opa);
  lv_anim_start(&subtitle_fade);

  lv_anim_t subtitle_y;
  lv_anim_init(&subtitle_y);
  lv_anim_set_var(&subtitle_y, ship_processing_subtitle);
  lv_anim_set_values(&subtitle_y, is_dish ? 70 : 94, is_dish ? 58 : 82);
  lv_anim_set_time(&subtitle_y, 280);
  lv_anim_set_delay(&subtitle_y, 110);
  lv_anim_set_path_cb(&subtitle_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&subtitle_y, ship_anim_set_y);
  lv_anim_start(&subtitle_y);
}

static void ship_init_logged() {
  // LCD_Minimal/LCD_Minimal.ino: ship_init_logged
  if (ship_logged_screen) {
    return;
  }
  ship_logged_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_logged_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_logged_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_LOGGED", "plain_screen");
  ship_style_plain_screen(ship_logged_screen, lv_color_hex(0x0E2547));
  lv_obj_set_style_border_width(ship_logged_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_logged_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_logged_screen, 0, LV_PART_MAIN);

  ship_logged_icon = lv_label_create(ship_logged_screen);
  lv_label_set_text(ship_logged_icon, LV_SYMBOL_OK);
  lv_obj_set_style_text_font(ship_logged_icon, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_logged_icon, lv_color_hex(0x6EE7B7), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_logged_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_logged_icon, LV_ALIGN_CENTER, 0, -42);

  ship_logged_label = lv_label_create(ship_logged_screen);
  lv_label_set_text(ship_logged_label, "Logged");
  lv_obj_set_style_text_font(ship_logged_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_logged_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_logged_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_logged_label, LV_ALIGN_CENTER, 0, 8);

  ship_logged_subtitle = lv_label_create(ship_logged_screen);
  lv_label_set_text(ship_logged_subtitle, "Saved successfully");
  lv_obj_set_style_text_font(ship_logged_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_logged_subtitle, lv_color_hex(0xD6E4FF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_logged_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_logged_subtitle, LV_ALIGN_CENTER, 0, 52);
}

static void ship_init_voice_ack() {
  if (ship_voice_ack_screen) {
    return;
  }
  ship_voice_ack_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_voice_ack_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_voice_ack_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_VOICE_ACK", "plain_screen");
  ship_style_plain_screen(ship_voice_ack_screen, lv_color_hex(0x0E2547));
  lv_obj_set_style_border_width(ship_voice_ack_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_voice_ack_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_voice_ack_screen, 0, LV_PART_MAIN);

  ship_voice_ack_icon = lv_label_create(ship_voice_ack_screen);
  lv_label_set_text(ship_voice_ack_icon, LV_SYMBOL_OK);
  lv_obj_set_style_text_font(ship_voice_ack_icon, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_ack_icon, lv_color_hex(0x6EE7B7), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_ack_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_voice_ack_icon, LV_ALIGN_CENTER, 0, -42);

  ship_voice_ack_label = lv_label_create(ship_voice_ack_screen);
  lv_label_set_text(ship_voice_ack_label, "On it!");
  lv_obj_set_style_text_font(ship_voice_ack_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_ack_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_ack_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_voice_ack_label, LV_ALIGN_CENTER, 0, 8);

  ship_voice_ack_subtitle = lv_label_create(ship_voice_ack_screen);
  lv_label_set_text(ship_voice_ack_subtitle, "Processing your request");
  lv_obj_set_style_text_font(ship_voice_ack_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_ack_subtitle, lv_color_hex(0xD6E4FF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_ack_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_voice_ack_subtitle, LV_ALIGN_CENTER, 0, 52);
}

static void ship_init_error() {
  if (ship_error_screen) {
    return;
  }
  ship_error_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_error_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_error_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_ERROR", "plain_screen");
  ship_style_plain_screen(ship_error_screen, lv_color_hex(0x8B1E2D));

  ship_error_label = lv_label_create(ship_error_screen);
  lv_label_set_text(ship_error_label, "Try Again");
  lv_obj_set_style_text_font(ship_error_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_error_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_error_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(ship_error_label);
}

static void ship_show_hold_still_impl() {
  if (ui_screen_state == SCREEN_HOLD_STILL) {
    return;
  }
  ship_init_hold_still();
  ship_hide_expiry_screen();
  status_overlay_hide();
  if (is_glowing_animation) {
    stop_glowing_animation();
  }
  ui_screen_state = SCREEN_HOLD_STILL;
  ui_busy = true;
  ship_logged_hide_at_ms = 0;
  ship_error_hide_at_ms = 0;
  lv_scr_load(ship_hold_screen);
  ship_start_hold_still_animation();
  lv_timer_handler();
}

static void ship_show_hold_still() {
  UI_SHOW(SCREEN_SHIP_HOLD_STILL, "hold_still");
}

static void ship_update_processing_progress() {
  if (!dish_processing_active || ui_screen_state != SCREEN_PROCESSING || dish_processing_start_ms == 0) {
    return;
  }
  if (!ship_processing_is_dish_mode()) {
    return;
  }

  unsigned long age_ms = millis() - dish_processing_start_ms;
  uint8_t pct = ship_compute_dish_processing_pct(age_ms);
  if (pct == dish_processing_progress_pct && pct != 100) {
    return;
  }
  dish_processing_progress_pct = pct;
  ship_set_processing_fill_pct(pct);
  if (ship_processing_label) {
    char pct_text[8];
    snprintf(pct_text, sizeof(pct_text), "%u%%", (unsigned)pct);
    lv_label_set_text(ship_processing_label, pct_text);
  }
  if (ship_processing_subtitle) {
    lv_label_set_text(ship_processing_subtitle, "Analyzing your meal");
  }
}

static void ship_show_processing_impl() {
  if (ui_screen_state == SCREEN_PROCESSING) {
    return;
  }
  ship_init_processing();
  ship_hide_expiry_screen();
  status_overlay_hide();
  bool is_dish = ship_processing_is_dish_mode();
  ship_configure_processing_layout(is_dish);
  if (is_dish) {
    dish_processing_progress_pct = 1;
    if (ship_processing_label) {
      lv_label_set_text(ship_processing_label, "1%");
    }
    if (ship_processing_subtitle) {
      lv_label_set_text(ship_processing_subtitle, "Analyzing your meal");
    }
  } else {
    ship_set_processing_text(NULL);
    if (ship_processing_subtitle) {
      lv_label_set_text(ship_processing_subtitle, "Working on your request");
    }
  }
  ui_screen_state = SCREEN_PROCESSING;
  ui_busy = true;
  ship_voice_ack_hide_at_ms = 0;
  ship_logged_hide_at_ms = 0;
  ship_error_hide_at_ms = 0;
  if (waiting_for_voice_response && voice_response_deadline_ms == 0) {
    voice_response_deadline_ms = millis() + VOICE_RESPONSE_TIMEOUT_MS;
    Serial.printf("[VOICE_TIMEOUT] armed source=processing_screen timeout_ms=%lu\n",
                  (unsigned long)VOICE_RESPONSE_TIMEOUT_MS);
  }
  lv_scr_load(ship_processing_screen);
  ship_start_processing_animation();
  lv_timer_handler();
}

static void ship_show_processing() {
  UI_SHOW(SCREEN_SHIP_PROCESSING, "processing");
}

static void ship_show_voice_ack_impl() {
  ship_init_voice_ack();
  ship_hide_expiry_screen();
  status_overlay_hide();
  if (is_glowing_animation) {
    stop_glowing_animation();
  }
  ui_screen_state = SCREEN_VOICE_ACK;
  ui_busy = false;
  ship_voice_ack_hide_at_ms = millis() + LOGGED_SCREEN_TIMEOUT_MS;
  ship_logged_hide_at_ms = 0;
  ship_error_hide_at_ms = 0;
  lv_scr_load(ship_voice_ack_screen);
  ship_start_voice_ack_animation();
  lv_timer_handler();
}

static void ship_show_voice_ack() {
  UI_SHOW(SCREEN_SHIP_VOICE_ACK, "voice_ack");
}

static void ship_show_logged_impl() {
  ship_init_logged();
  ship_hide_expiry_screen();
  status_overlay_hide();
  if (is_glowing_animation) {
    stop_glowing_animation();
  }
  ui_screen_state = SCREEN_LOGGED;
  ui_busy = false;
  ship_logged_hide_at_ms = millis() + LOGGED_SCREEN_TIMEOUT_MS;
  ship_error_hide_at_ms = 0;
  lv_scr_load(ship_logged_screen);
  ship_start_logged_success_animation();
  lv_timer_handler();
}

static void ship_show_logged() {
  UI_SHOW(SCREEN_SHIP_LOGGED, "logged");
}

static void ship_show_expiry_choice_impl() {
  if (ui_screen_state == SCREEN_EXPIRY_CHOICE) {
    return;
  }
  ship_init_expiry_choice();
  ship_hide_expiry_screen();
  status_overlay_hide();
  ship_hide_default_ui();
  ui_screen_state = SCREEN_EXPIRY_CHOICE;
  ui_busy = true;
  ship_logged_hide_at_ms = 0;
  ship_error_hide_at_ms = 0;
  expiry_submitted = false;
  ship_expiry_choice_shown_time = millis();
  ship_update_expiry_choice_timeout_ring();
  if (!ship_choice_mode_is_discard()) {
    expiry_choice_quantity = 1;
    expiry_choice_update_quantity_label();
  }
  ship_configure_scan_choice_screen();
  lv_scr_load(ship_expiry_choice_screen);
  ship_start_expiry_choice_animation();
  lv_timer_handler();
}

static void ship_show_expiry_choice() {
  UI_SHOW(SCREEN_SHIP_EXPIRY_CHOICE, "expiry_choice");
}

static void ship_show_expiry_screen_impl() {
  // LCD_Minimal/LCD_Minimal.ino: ship_show_expiry_screen
  if (!expiry_screen || !g_base_screen) {
    return;
  }
  if (ui_screen_state == SCREEN_EXPIRY) {
    return;
  }
  ship_hide_default_ui();
  ship_hide_expiry_screen();
  status_overlay_hide();
  lv_scr_load(g_base_screen);
  ship_expiry_choice_shown_time = 0;
  expiry_prepare_picker_for_entry();
  lv_obj_clear_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
  ui_screen_state = SCREEN_EXPIRY;
  ui_busy = true;
  ship_logged_hide_at_ms = 0;
  ship_error_hide_at_ms = 0;
  expiry_submitted = false;
  Serial.println("[STATUS] Showing expiration date entry screen (ship)");
  lv_timer_handler();
}

static void ship_show_error_impl() {
  ship_init_error();
  ship_hide_expiry_screen();
  status_overlay_hide();
  ui_screen_state = SCREEN_RESULT;
  ui_busy = false;
  ship_error_hide_at_ms = millis() + 3000;
  ship_logged_hide_at_ms = 0;
  lv_scr_load(ship_error_screen);
  lv_timer_handler();
}

static void ship_show_expiry_screen() {
  UI_SHOW(SCREEN_SHIP_EXPIRY, "expiry");
}

static bool ship_text_contains_expiry(const char* text) {
  if (!text || text[0] == '\0') return false;
  return (strstr(text, "expiry") != NULL) || (strstr(text, "Expiry") != NULL);
}

static bool ship_text_contains_upload(const char* text) {
  if (!text || text[0] == '\0') return false;
  return (strstr(text, "Preparing upload") != NULL) ||
         (strstr(text, "Uploading") != NULL);
}

static bool ship_text_contains_wifi_error(const char* text) {
  if (!text || text[0] == '\0') return false;
  return (strstr(text, "Wi-Fi not ready") != NULL) ||
         (strstr(text, "wifi not ready") != NULL);
}

static const char* ship_infer_phase(const char* phase, const char* text) {
  const char* safe_phase = phase ? phase : "";
  if (safe_phase[0] == '\0' || strcmp(safe_phase, "PREPARING") == 0 ||
      strcmp(safe_phase, "PROCESSING") == 0 || strcmp(safe_phase, "WAITING") == 0) {
    if (ship_text_contains_expiry(text)) return "WAITING_INPUT";
    if (ship_text_contains_upload(text)) return "UPLOADING";
    if (ship_text_contains_wifi_error(text)) return "ERROR";
  }
  return safe_phase;
}

static void ui_show_screen(ScreenId next, const char* reason, const char* file, int line, const char* func) {
  const ScreenMeta* from_meta = screen_meta(g_current_screen_id);
  const ScreenMeta* to_meta = screen_meta(next);
  Serial.printf("[UI_SHOW] op=%s mode=%s phase=%s from=%s to=%s reason=%s caller=%s file=%s:%d prev_bitmap=%s next_bitmap=%s\n",
                g_ship_ui_op[0] ? g_ship_ui_op : "",
                g_ship_ui_mode[0] ? g_ship_ui_mode : "",
                g_ship_ui_phase[0] ? g_ship_ui_phase : "",
                from_meta && from_meta->name ? from_meta->name : "UNKNOWN",
                to_meta && to_meta->name ? to_meta->name : "UNKNOWN",
                reason ? reason : "",
                func ? func : "",
                file ? file : "",
                line,
                from_meta && from_meta->bitmap ? from_meta->bitmap : "(none)",
                to_meta && to_meta->bitmap ? to_meta->bitmap : "(none)");
  g_current_screen_id = next;
  ui_show_screen_impl(next);
}

static void ui_show_screen_impl(ScreenId next) {
  switch (next) {
    case SCREEN_SHIP_MAIN_MENU:
      show_ship_main_menu_impl();
      break;
    case SCREEN_SHIP_SECOND_MENU:
      show_ship_second_menu_impl();
      break;
    case SCREEN_SHIP_SETTINGS:
      show_ship_settings_screen_impl();
      break;
    case SCREEN_SHIP_HOLD_STILL:
      ship_show_hold_still_impl();
      break;
    case SCREEN_SHIP_VOICE_ACK:
      ship_show_voice_ack_impl();
      break;
    case SCREEN_SHIP_PROCESSING:
      ship_show_processing_impl();
      break;
    case SCREEN_SHIP_LOGGED:
      ship_show_logged_impl();
      break;
    case SCREEN_SHIP_ERROR:
      ship_show_error_impl();
      break;
    case SCREEN_SHIP_EXPIRY_CHOICE:
      ship_show_expiry_choice_impl();
      break;
    case SCREEN_SHIP_EXPIRY:
      ship_show_expiry_screen_impl();
      break;
    case SCREEN_SHIP_RESULT:
      ui_show_result_impl(g_result_pending.is_error, g_result_pending.title, g_result_pending.mode);
      break;
    case SCREEN_SHIP_DEBUG:
      show_ship_debug_screen_impl();
      break;
    default:
      break;
  }
}

static bool ship_mode_is_check(const char* mode) {
  return mode && (strcmp(mode, "check-in") == 0 ||
                  strcmp(mode, "check-out") == 0 ||
                  strcmp(mode, "check_out") == 0);
}

static bool ship_mode_is_discard(const char* mode) {
  return mode && (strcmp(mode, "discard") == 0);
}

static bool ship_choice_mode_is_discard(void) {
  return ship_mode_is_discard(g_ship_ui_mode);
}

static void ship_configure_scan_choice_screen(void) {
  if (!expiry_choice_quantity_label || !ship_expiry_choice_qty_prefix ||
      !ship_expiry_choice_prompt || !ship_expiry_choice_skip_label ||
      !ship_expiry_choice_add_label) {
    return;
  }

  if (ship_choice_mode_is_discard()) {
    lv_label_set_text(expiry_choice_quantity_label, "Shopping list");
    lv_obj_set_width(expiry_choice_quantity_label, 220);
    lv_obj_set_style_text_font(expiry_choice_quantity_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_align(expiry_choice_quantity_label, LV_ALIGN_TOP_MID, 0, SHIP_CHOICE_VALUE_Y);
    lv_obj_add_flag(ship_expiry_choice_qty_prefix, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(ship_expiry_choice_prompt, "Also add this item after discard?");
    lv_obj_set_width(ship_expiry_choice_prompt, 220);
    lv_obj_align(ship_expiry_choice_prompt, LV_ALIGN_TOP_MID, 0, SHIP_CHOICE_PROMPT_Y + 4);
    lv_obj_clear_flag(ship_expiry_choice_prompt, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(ship_expiry_choice_skip_label, "Skip");
    lv_label_set_text(ship_expiry_choice_add_label, "Add to\nShopping\nList");
    lv_obj_set_width(ship_expiry_choice_add_label, 110);
  } else {
    lv_obj_set_width(expiry_choice_quantity_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(expiry_choice_quantity_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(expiry_choice_quantity_label, LV_ALIGN_TOP_MID, 26, SHIP_CHOICE_VALUE_Y);
    lv_obj_clear_flag(ship_expiry_choice_qty_prefix, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ship_expiry_choice_qty_prefix, "QTY:");
    lv_obj_align(ship_expiry_choice_qty_prefix, LV_ALIGN_TOP_MID, -26, SHIP_CHOICE_VALUE_Y);

    expiry_choice_update_quantity_label();
    lv_label_set_text(ship_expiry_choice_prompt, "Turn knob to change QTY");
    lv_obj_set_width(ship_expiry_choice_prompt, LV_SIZE_CONTENT);
    lv_obj_align(ship_expiry_choice_prompt, LV_ALIGN_TOP_MID, 0, SHIP_CHOICE_PROMPT_Y);
    lv_obj_clear_flag(ship_expiry_choice_prompt, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(ship_expiry_choice_skip_label, "Skip");
    lv_label_set_text(ship_expiry_choice_add_label, "Add\nExpiration");
    lv_obj_set_width(ship_expiry_choice_add_label, 110);
  }
}

static void ship_send_discard_choice(bool add_to_shopping_list, const char* wake_reason) {
  StaticJsonDocument<256> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_DISCARD_OPTIONS";
  doc["msg_id"] = lcd_msg_id_counter++;
  doc["ts"] = millis();
  doc["add_to_shopping_list"] = add_to_shopping_list;

  String output;
  serializeJson(doc, output);
  request_sense_wake(wake_reason ? wake_reason : "discard_choice");
  uart_send_json(output.c_str());
  Serial.printf("[DISCARD_CHOICE] Sent add_to_shopping_list=%d\n",
                add_to_shopping_list ? 1 : 0);
}

static bool ship_mode_is_dish(const char* op, const char* mode) {
  if (op && strcmp(op, "DISH_LOG") == 0) {
    return true;
  }
  return mode && (strcmp(mode, "dish") == 0 ||
                  strcmp(mode, "dish-log") == 0 ||
                  strcmp(mode, "dish_log") == 0);
}

static void ship_route_log(const char* op, const char* mode, const char* phase, const char* target) {
  Serial.printf("[ROUTE] op=%s mode=%s phase=%s -> %s\n",
                op ? op : "",
                mode ? mode : "",
                phase ? phase : "",
                target ? target : "");
}

typedef struct {
  const char* op;
  const char* mode;
  const char* phase;
  const char* text;
  const char* ui_policy;
  uint32_t job_id;
} ui_status_t;

static ScreenId route_ship_ui(const ui_status_t* s) {
  if (!s || !s->op) {
    return SCREEN_UNKNOWN;
  }
  const char* safe_text = s->text ? s->text : "";
  const char* safe_mode = s->mode ? s->mode : "";
  const char* safe_phase = ship_infer_phase(s->phase, safe_text);
  if (strcmp(safe_phase, "CAPTURING") == 0 && s->job_id == 0) {
    g_ship_ui_finalized = false;
    g_ship_ui_finalized_job_id = 0;
  }

  if (s->ui_policy && strcmp(s->ui_policy, "TOAST_ONLY") == 0) {
    ship_route_log(s->op, safe_mode, safe_phase, "TOAST_ONLY");
    return SCREEN_UNKNOWN;
  }

  if (g_ship_ui_finalized &&
      (g_ship_ui_finalized_job_id == 0 || s->job_id == g_ship_ui_finalized_job_id)) {
    ship_route_log(s->op, safe_mode, safe_phase, "IGNORED_LATE");
    return SCREEN_UNKNOWN;
  }

  bool is_scan = (strcmp(s->op, "SCAN") == 0);
  bool is_voice = (strcmp(s->op, "VOICE") == 0);
  bool is_check = is_scan && ship_mode_is_check(safe_mode);
  bool is_discard = is_scan && ship_mode_is_discard(safe_mode);
  bool is_dish = ship_mode_is_dish(s->op, safe_mode);
  if (is_voice) {
    if (strcmp(safe_phase, "UPLOADING") == 0 ||
        strcmp(safe_phase, "PROCESSING") == 0 ||
        strcmp(safe_phase, "PARSE") == 0 ||
        strcmp(safe_phase, "APPLY") == 0) {
      ship_route_log(s->op, safe_mode, safe_phase, "VOICE_PROCESSING");
      return SCREEN_SHIP_PROCESSING;
    }
    if (strcmp(safe_phase, "ERROR") == 0) {
      ship_route_log(s->op, safe_mode, safe_phase, "VOICE_ERROR");
      return SCREEN_SHIP_ERROR;
    }
    ship_route_log(s->op, safe_mode, safe_phase, "VOICE_IGNORED");
    return SCREEN_UNKNOWN;
  }
  if (!is_scan && !is_dish) {
    return SCREEN_UNKNOWN;
  }

  if (is_check) {
    if (strcmp(safe_phase, "CAPTURING") == 0) {
      ship_route_log(s->op, safe_mode, safe_phase, "HOLD_STILL");
      return SCREEN_SHIP_HOLD_STILL;
    }
    if (strcmp(safe_phase, "WAITING_INPUT") == 0) {
      ship_route_log(s->op, safe_mode, safe_phase, "EXPIRY_CHOICE");
      return SCREEN_SHIP_EXPIRY_CHOICE;
    }
    if (strcmp(safe_phase, "DONE") == 0) {
      g_ship_ui_finalized_job_id = s->job_id;
      g_ship_ui_finalized = true;
      ship_route_log(s->op, safe_mode, safe_phase, "LOGGED_THEN_HOME");
      return SCREEN_SHIP_LOGGED;
    }
    if (strcmp(safe_phase, "ERROR") == 0) {
      g_ship_ui_finalized_job_id = s->job_id;
      g_ship_ui_finalized = true;
      ship_route_log(s->op, safe_mode, safe_phase, "ERROR_SCREEN");
      return SCREEN_SHIP_ERROR;
    }
    ship_route_log(s->op, safe_mode, safe_phase, "IGNORED");
    return SCREEN_UNKNOWN;
  }

  if (is_discard) {
    if (strcmp(safe_phase, "CAPTURING") == 0) {
      ship_route_log(s->op, safe_mode, safe_phase, "HOLD_STILL");
      return SCREEN_SHIP_HOLD_STILL;
    }
    if (strcmp(safe_phase, "WAITING_INPUT") == 0) {
      ship_route_log(s->op, safe_mode, safe_phase, "DISCARD_CHOICE");
      return SCREEN_SHIP_EXPIRY_CHOICE;
    }
    if (strcmp(safe_phase, "DONE") == 0 || strcmp(safe_phase, "UPLOADING") == 0) {
      g_ship_ui_finalized_job_id = s->job_id;
      g_ship_ui_finalized = true;
      ship_route_log(s->op, safe_mode, safe_phase, "LOGGED_THEN_HOME");
      return SCREEN_SHIP_LOGGED;
    }
    if (strcmp(safe_phase, "ERROR") == 0) {
      g_ship_ui_finalized_job_id = s->job_id;
      g_ship_ui_finalized = true;
      ship_route_log(s->op, safe_mode, safe_phase, "ERROR_SCREEN");
      return SCREEN_SHIP_ERROR;
    }
    ship_route_log(s->op, safe_mode, safe_phase, "IGNORED");
    return SCREEN_UNKNOWN;
  }

  if (is_dish) {
    if (strcmp(safe_phase, "CAPTURING") == 0) {
      ship_route_log(s->op, safe_mode, safe_phase, "HOLD_STILL");
      return SCREEN_SHIP_HOLD_STILL;
    }
    if (strcmp(safe_phase, "DONE") == 0 ||
        strcmp(safe_phase, "UPLOADING") == 0 ||
        strcmp(safe_phase, "UPLOAD_STARTING") == 0 ||
        strcmp(safe_phase, "RESULT_WAITING") == 0 ||
        strcmp(safe_phase, "PROCESSING") == 0) {
      g_ship_ui_finalized_job_id = s->job_id;
      g_ship_ui_finalized = true;
      ship_route_log(s->op, safe_mode, safe_phase, "LOGGED_THEN_HOME");
      return SCREEN_SHIP_LOGGED;
    }
    if (strcmp(safe_phase, "ERROR") == 0) {
      g_ship_ui_finalized_job_id = s->job_id;
      g_ship_ui_finalized = true;
      ship_route_log(s->op, safe_mode, safe_phase, "ERROR_SCREEN");
      return SCREEN_SHIP_ERROR;
    }
    ship_route_log(s->op, safe_mode, safe_phase, "IGNORED");
    return SCREEN_UNKNOWN;
  }

  if (strcmp(safe_phase, "CAPTURING") == 0) {
    ship_route_log(s->op, safe_mode, safe_phase, "HOLD_STILL");
    return SCREEN_SHIP_HOLD_STILL;
  }
  if (strcmp(safe_phase, "UPLOAD_STARTING") == 0 ||
      strcmp(safe_phase, "UPLOADING") == 0 ||
      strcmp(safe_phase, "RESULT_WAITING") == 0 ||
      strcmp(safe_phase, "PROCESSING") == 0) {
    ship_route_log(s->op, safe_mode, safe_phase, "PROCESSING");
    return SCREEN_SHIP_PROCESSING;
  }
  if (strcmp(safe_phase, "RESULT_READY") == 0) {
    ship_route_log(s->op, safe_mode, safe_phase, "RESULT_READY");
    return SCREEN_UNKNOWN;
  }
  if (strcmp(safe_phase, "ERROR") == 0) {
    g_ship_ui_finalized_job_id = s->job_id;
    g_ship_ui_finalized = true;
    ship_route_log(s->op, safe_mode, safe_phase, "ERROR_SCREEN");
    return SCREEN_SHIP_ERROR;
  }
  if (strcmp(safe_phase, "DONE") == 0) {
    g_ship_ui_finalized_job_id = s->job_id;
    g_ship_ui_finalized = true;
    ship_route_log(s->op, safe_mode, safe_phase, "LOGGED_THEN_HOME");
    return SCREEN_SHIP_LOGGED;
  }
  ship_route_log(s->op, safe_mode, safe_phase, "IGNORED");
  return SCREEN_UNKNOWN;
}

static bool ship_ui_phase_is_terminal(const char* phase) {
  if (!phase || !phase[0]) {
    return false;
  }
  return strcmp(phase, "DONE") == 0 ||
         strcmp(phase, "ERROR") == 0 ||
         strcmp(phase, "SUCCESS") == 0 ||
         strcmp(phase, "COMPLETE") == 0 ||
         strcmp(phase, "RESULT_READY") == 0;
}

static bool ship_ui_terminal_apply_pending() {
  if (!g_ship_ui_terminal || !ship_ui_phase_is_terminal(g_ship_ui_phase) || g_ship_ui_job_id == 0) {
    return false;
  }
  bool stale_scan_screen =
      (ui_screen_state == SCREEN_HOLD_STILL) ||
      (ui_screen_state == SCREEN_PROCESSING) ||
      (ui_screen_state == SCREEN_EXPIRY_CHOICE) ||
      (ui_screen_state == SCREEN_EXPIRY);
  if (!stale_scan_screen) {
    return false;
  }
  if (g_last_ui_status_msg_id != 0) {
    return g_last_ui_status_msg_id != g_ship_ui_applied_msg_id;
  }
  return true;
}

static void dump_route_table() {
  Serial.println("[ROUTE_TABLE]");
  Serial.println("SCAN check-in/check-out:");
  Serial.println("  CAPTURING      -> SHIP_HOLD_STILL");
  Serial.println("  WAITING_INPUT  -> SHIP_EXPIRY_CHOICE");
  Serial.println("  DONE           -> SHIP_LOGGED_THEN_HOME");
  Serial.println("  ERROR          -> SHIP_ERROR_THEN_HOME");
  Serial.println("SCAN discard:");
  Serial.println("  CAPTURING      -> SHIP_HOLD_STILL");
  Serial.println("  UPLOAD_STARTING-> IGNORED");
  Serial.println("  UPLOADING/DONE -> SHIP_LOGGED_THEN_HOME");
  Serial.println("  ERROR          -> SHIP_ERROR_THEN_HOME");
  Serial.println("SCAN dish:");
  Serial.println("  CAPTURING      -> SHIP_HOLD_STILL");
  Serial.println("  UPLOAD_STARTING-> SHIP_PROCESSING");
  Serial.println("  UPLOADING      -> SHIP_PROCESSING");
  Serial.println("  RESULT_WAITING -> SHIP_PROCESSING");
  Serial.println("  RESULT_READY   -> MEAL_RESULT");
  Serial.println("  ERROR          -> SHIP_ERROR_THEN_HOME");
}

static void expiry_choice_update_quantity_label() {
  if (!expiry_choice_quantity_label) {
    return;
  }
  if (expiry_choice_quantity < 1) {
    expiry_choice_quantity = 1;
  }
  char qty_text[12];
  snprintf(qty_text, sizeof(qty_text), "%d", expiry_choice_quantity);
  lv_label_set_text(expiry_choice_quantity_label, qty_text);
}

static void expiry_choice_adjust_quantity(int delta) {
  int next = expiry_choice_quantity + delta;
  if (next < 1) {
    next = 1;
  }
  if (next == expiry_choice_quantity) {
    return;
  }
  expiry_choice_quantity = next;
  Serial.printf("[EXPIRY_CHOICE] quantity=%d\n", expiry_choice_quantity);
  expiry_choice_update_quantity_label();
}

static bool expiry_choice_handle_touch(uint16_t check_x, uint16_t check_y) {
  if (ui_screen_state != SCREEN_EXPIRY_CHOICE) {
    return false;
  }
  if (expiry_submitted) {
    Serial.println("[EXPIRY_CHOICE] input ignored (submitted)");
    return true;
  }
  bool skip_pressed = false;
  bool add_pressed = false;
  if (ship_expiry_choice_skip_btn != NULL) {
    lv_area_t skip_area;
    lv_obj_get_coords(ship_expiry_choice_skip_btn, &skip_area);
    skip_pressed = (check_x >= skip_area.x1 && check_x <= skip_area.x2 &&
                    check_y >= skip_area.y1 && check_y <= skip_area.y2);
  }
  if (ship_expiry_choice_add_btn != NULL) {
    lv_area_t add_area;
    lv_obj_get_coords(ship_expiry_choice_add_btn, &add_area);
    add_pressed = (check_x >= add_area.x1 && check_x <= add_area.x2 &&
                   check_y >= add_area.y1 && check_y <= add_area.y2);
  }
  Serial.printf("[EXPIRY_CHOICE] tap (%d, %d) skip=%d add=%d\n",
                check_x,
                check_y,
                skip_pressed ? 1 : 0,
                add_pressed ? 1 : 0);
  if (ship_choice_mode_is_discard()) {
    if (skip_pressed) {
      ship_send_discard_choice(false, "discard_choice_skip");
      expiry_submitted = true;
      ship_expiry_choice_shown_time = 0;
      g_ship_ui_finalized = true;
      g_ship_ui_finalized_job_id = g_ship_ui_job_id;
      UI_SHOW(SCREEN_SHIP_LOGGED, "discard_choice_skip");
    } else if (add_pressed) {
      ship_send_discard_choice(true, "discard_choice_add");
      expiry_submitted = true;
      ship_expiry_choice_shown_time = 0;
      g_ship_ui_finalized = true;
      g_ship_ui_finalized_job_id = g_ship_ui_job_id;
      UI_SHOW(SCREEN_SHIP_LOGGED, "discard_choice_add");
    } else {
      Serial.println("[DISCARD_CHOICE] tap ignored (outside buttons)");
    }
    return true;
  }
  if (skip_pressed) {
    StaticJsonDocument<256> doc;
    doc["ver"] = PROTOCOL_VERSION;
    doc["type"] = "INPUT_EXPIRY_DATE";
    doc["msg_id"] = lcd_msg_id_counter++;
    doc["ts"] = millis();
    doc["expiry_date"] = "";  // Empty string indicates no expiry date
    doc["quantity"] = expiry_choice_quantity;

    String output;
    serializeJson(doc, output);
    request_sense_wake("expiry_choice_left");
    uart_send_json(output.c_str());
    Serial.printf("[EXPIRY_CHOICE] Sent empty expiration date to Sense (left tap) quantity=%d\n",
                  expiry_choice_quantity);

    expiry_submitted = true;
    ship_expiry_choice_shown_time = 0;
    g_ship_ui_finalized = true;
    g_ship_ui_finalized_job_id = g_ship_ui_job_id;
    UI_SHOW(SCREEN_SHIP_LOGGED, "expiry_choice_skip");
  } else if (add_pressed) {
    ship_expiry_choice_shown_time = 0;
    ship_show_expiry_screen();
  } else {
    Serial.println("[EXPIRY_CHOICE] tap ignored (outside buttons)");
  }
  return true;
}

static bool expiry_handle_touch(uint16_t check_x, uint16_t check_y) {
  if (!expiry_screen_visible || expiry_screen == NULL || lv_obj_has_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN)) {
    return false;
  }
  if (expiry_submitted) {
    Serial.println("[EXPIRY] input ignored (submitted)");
    return true;
  }
  Serial.printf("[EXPIRY] Checking buttons with stored coordinates (%d, %d) -> flipped to (%d, %d)\n", 
               touch_press_x, touch_press_y, check_x, check_y);

  bool button_pressed = false;

  if (expiry_back_button != NULL) {
    lv_area_t btn_area;
    lv_obj_get_coords(expiry_back_button, &btn_area);
    if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
        check_y >= btn_area.y1 && check_y <= btn_area.y2) {
      Serial.println("[EXPIRY] Back button pressed - canceling");
      ship_hide_expiry_screen();
      expiry_submitted = false;
#if SHIP_MENU_UI
      show_ship_main_menu();
#else
      if (list_container != NULL && g_active.count > 0) {
        lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
      }
      lv_timer_handler();
#endif
      return true;
    }
  }

  if (!button_pressed && expiry_month_button != NULL) {
    lv_area_t btn_area;
    lv_obj_get_coords(expiry_month_button, &btn_area);
    if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
        check_y >= btn_area.y1 && check_y <= btn_area.y2) {
      expiry_active_segment = EXPIRY_SEGMENT_MONTH;
      expiry_refresh_picker_ui();
      Serial.println("[EXPIRY] Active segment -> month");
      button_pressed = true;
    }
  }

  if (!button_pressed && expiry_day_button != NULL) {
    lv_area_t btn_area;
    lv_obj_get_coords(expiry_day_button, &btn_area);
    if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
        check_y >= btn_area.y1 && check_y <= btn_area.y2) {
      expiry_active_segment = EXPIRY_SEGMENT_DAY;
      expiry_refresh_picker_ui();
      Serial.println("[EXPIRY] Active segment -> day");
      button_pressed = true;
    }
  }

  if (!button_pressed && expiry_year_button != NULL) {
    lv_area_t btn_area;
    lv_obj_get_coords(expiry_year_button, &btn_area);
    if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
        check_y >= btn_area.y1 && check_y <= btn_area.y2) {
      expiry_active_segment = EXPIRY_SEGMENT_YEAR;
      expiry_refresh_picker_ui();
      Serial.println("[EXPIRY] Active segment -> year");
      button_pressed = true;
    }
  }

  if (!button_pressed && expiry_check_button != NULL) {
    lv_area_t btn_area;
    lv_obj_get_coords(expiry_check_button, &btn_area);
    if (check_x >= btn_area.x1 && check_x <= btn_area.x2 &&
        check_y >= btn_area.y1 && check_y <= btn_area.y2) {
      expiry_submit_selected_date("expiry_submit", "expiry_submit");
      button_pressed = true;
    }
  }
  
  if (!button_pressed) {
    Serial.printf("[EXPIRY] Touch detected but not on any button (%d, %d)\n", check_x, check_y);
  }
  return true;
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

static void ui_show_result_impl(bool is_error, const char* title, const char* mode) {
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

static void ui_show_result(bool is_error, const char* title, const char* mode) {
  g_result_pending.is_error = is_error;
  strncpy(g_result_pending.title, title ? title : "", sizeof(g_result_pending.title) - 1);
  g_result_pending.title[sizeof(g_result_pending.title) - 1] = '\0';
  strncpy(g_result_pending.mode, mode ? mode : "", sizeof(g_result_pending.mode) - 1);
  g_result_pending.mode[sizeof(g_result_pending.mode) - 1] = '\0';
  UI_SHOW(SCREEN_SHIP_RESULT, "result");
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

static void show_ship_debug_screen_impl() {
  debug_screen_init();
  ui_screen_state = SCREEN_DEBUG;
  lv_scr_load(debug_screen);
  debug_last_update_ms = 0;
  debug_screen_update();
  Serial.println("[MENU] screen=DEBUG");
  lv_timer_handler();
}

static void show_ship_debug_screen() {
  UI_SHOW(SCREEN_SHIP_DEBUG, "debug");
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
  ship_show_processing();
  Serial.printf("[RESULT] retry menu_item=%s index=%d\n",
                g_last_action.menu_item,
                g_last_action.menu_index);
  if (g_last_action.menu_item && strcmp(g_last_action.menu_item, "Check-in") == 0) {
    ship_menu_send_retry("check_in");
  } else {
    ship_menu_send_menu_select(g_last_action.menu_item, g_last_action.menu_index, "RETRY");
  }
}

static void ui_apply_ship_ui_status(const app_event_t* evt) {
  ui_status_t s = {};
  uint32_t msg_id = g_last_ui_status_msg_id;
  if (evt && evt->type == EVT_SHIP_UI_STATUS) {
    s.op = evt->data.ship_ui_status.op;
    s.mode = evt->data.ship_ui_status.mode;
    s.phase = evt->data.ship_ui_status.phase;
    s.text = evt->data.ship_ui_status.text;
    s.ui_policy = evt->data.ship_ui_status.ui_policy;
    s.job_id = evt->data.ship_ui_status.job_id;
    msg_id = evt->data.ship_ui_status.msg_id;
  } else {
    s.op = g_ship_ui_op;
    s.mode = g_ship_ui_mode;
    s.phase = g_ship_ui_phase;
    s.text = g_ship_ui_text;
    s.ui_policy = g_ship_ui_policy;
    s.job_id = g_ship_ui_job_id;
  }
  bool latest_latched_status = (!evt || evt->type != EVT_SHIP_UI_STATUS ||
                                msg_id == 0 || msg_id == g_last_ui_status_msg_id);
  if (s.op && strcmp(s.op, "VOICE") == 0 && ui_screen_state == SCREEN_VOICE_JSON) {
    if (msg_id != 0) {
      g_ship_ui_applied_msg_id = msg_id;
    }
    if (latest_latched_status) {
      g_ship_ui_dirty = false;
    }
    debug_screen_update();
    return;
  }
  bool was_processing = (ui_screen_state == SCREEN_PROCESSING);
  bool is_dish = ship_mode_is_dish(s.op, s.mode);
  ScreenId target = route_ship_ui(&s);
  bool hold_voice_ack = (
      ui_screen_state == SCREEN_VOICE_ACK &&
      ship_voice_ack_hide_at_ms != 0 &&
      millis() < ship_voice_ack_hide_at_ms &&
      s.op && strcmp(s.op, "VOICE") == 0 &&
      target == SCREEN_SHIP_PROCESSING);
  if (hold_voice_ack) {
    if (msg_id != 0) {
      g_ship_ui_applied_msg_id = msg_id;
    }
    if (latest_latched_status) {
      g_ship_ui_dirty = false;
    }
    Serial.printf("[VOICE_ACK] hold_processing_until_ack_done phase=%s job_id=%lu remaining_ms=%lu\n",
                  s.phase ? s.phase : "",
                  (unsigned long)s.job_id,
                  (unsigned long)(ship_voice_ack_hide_at_ms - millis()));
    debug_screen_update();
    return;
  }
  if (target != SCREEN_UNKNOWN) {
    if (target == SCREEN_SHIP_HOLD_STILL && was_processing) {
      Serial.printf("[UI_ROUTE][WARN] processing_to_holdstill op=%s mode=%s phase=%s job_id=%lu msg_id=%u prev_screen=%s\n",
                    s.op ? s.op : "",
                    s.mode ? s.mode : "",
                    s.phase ? s.phase : "",
                    (unsigned long)s.job_id,
                    (unsigned)msg_id,
                    ui_screen_state_name(ui_screen_state));
    }
    UI_SHOW(target, "route");
    if (target == SCREEN_SHIP_PROCESSING) {
      if (s.op && strcmp(s.op, "VOICE") == 0) {
        ship_set_processing_text("Processing");
      } else if (!is_dish && s.text && s.text[0]) {
        ship_set_processing_text(s.text);
      }
    }
    dish_processing_active = false;
    dish_processing_start_ms = 0;
    dish_processing_progress_pct = 1;
    dish_timeout_at_ms = 0;
    dish_timeout_job_id = 0;
    if (target == SCREEN_SHIP_ERROR) {
      ship_error_hide_at_ms = millis() + 3000;
    }
  }
  if (msg_id != 0) {
    g_ship_ui_applied_msg_id = msg_id;
  }
  if (latest_latched_status) {
    g_ship_ui_dirty = false;
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

static const char* ship_menu_mode_for_item(const char* menu_item) {
  if (!menu_item || menu_item[0] == '\0') {
    return NULL;
  }
  if (strcmp(menu_item, "Dish") == 0) {
    return "dish";
  }
  if (strcmp(menu_item, "Check-in") == 0) {
    return "check-in";
  }
  if (strcmp(menu_item, "Discard") == 0) {
    return "discard";
  }
  return NULL;
}

static bool ship_scan_request_is_local_only() {
  return waiting_for_scan_response &&
         strcmp(g_ship_ui_op, "SCAN") == 0 &&
         g_ship_ui_job_id == 0;
}

static void ship_cancel_local_scan_request(const char* reason, const char* message) {
  waiting_for_scan_response = false;
  dish_processing_active = false;
  dish_processing_start_ms = 0;
  dish_timeout_at_ms = 0;
  dish_timeout_job_id = 0;
  g_ship_ui_busy = false;
  g_ship_ui_terminal = false;
  g_ship_ui_error = false;
  g_ship_ui_finalized = false;
  g_ship_ui_dirty = false;
  g_ship_ui_job_id = 0;
  g_ship_ui_applied_msg_id = 0;
  g_ship_ui_op[0] = '\0';
  g_ship_ui_phase[0] = '\0';
  g_ship_ui_mode[0] = '\0';
  g_ship_ui_text[0] = '\0';
  g_ship_ui_policy[0] = '\0';
  Serial.printf("[SHIP_UI] local_scan_cancel reason=%s message=%s user_state=%s\n",
                reason ? reason : "unknown",
                message ? message : "",
                ship_user_state_name(ship_user_state_current()));
  show_ship_main_menu();
  if (message && message[0]) {
    show_auto_hiding_status_message(message, 1500);
  }
  resetActivityTimer();
}

static void ui_handle_ship_toast_event(const app_event_t* evt) {
  const char* text = (evt && evt->data.ship_toast[0]) ? evt->data.ship_toast : "";
  if (ship_scan_request_is_local_only()) {
    ship_cancel_local_scan_request("toast_reject", text);
    return;
  }
  if (text && text[0]) {
    Serial.printf("[SHIP_UI] toast=%s user_state=%s\n",
                  text,
                  ship_user_state_name(ship_user_state_current()));
    show_auto_hiding_status_message(text, 1500);
    resetActivityTimer();
  }
}

static void ship_menu_begin_local_scan_request(const char* menu_item) {
  const char* mode = ship_menu_mode_for_item(menu_item);
  if (!mode) {
    return;
  }
  strncpy(g_ship_ui_op, "SCAN", sizeof(g_ship_ui_op) - 1);
  g_ship_ui_op[sizeof(g_ship_ui_op) - 1] = '\0';
  strncpy(g_ship_ui_phase, "CAPTURING", sizeof(g_ship_ui_phase) - 1);
  g_ship_ui_phase[sizeof(g_ship_ui_phase) - 1] = '\0';
  strncpy(g_ship_ui_mode, mode, sizeof(g_ship_ui_mode) - 1);
  g_ship_ui_mode[sizeof(g_ship_ui_mode) - 1] = '\0';
  strncpy(g_ship_ui_text, "Capturing image...", sizeof(g_ship_ui_text) - 1);
  g_ship_ui_text[sizeof(g_ship_ui_text) - 1] = '\0';
  g_ship_ui_policy[0] = '\0';
  g_ship_ui_last_ms = millis();
  g_ship_ui_job_id = 0;
  g_ship_ui_busy = true;
  g_ship_ui_terminal = false;
  g_ship_ui_error = false;
  g_ship_ui_finalized = false;
  g_ship_ui_dirty = false;
  g_ship_ui_applied_msg_id = 0;
  g_ship_ui_finalized_job_id = 0;
  waiting_for_scan_response = true;
  dish_processing_active = false;
  dish_processing_start_ms = 0;
  dish_timeout_at_ms = 0;
  dish_timeout_job_id = 0;
  resetActivityTimer();
  Serial.printf("[SHIP_UI] local_scan_begin item=%s mode=%s user_state=%s\n",
                menu_item,
                mode,
                ship_user_state_name(ship_user_state_current()));
  ship_show_hold_still();
}

static void ship_menu_send_menu_select(const char* menu_item, int menu_index, const char* label) {
  if (!menu_item || menu_item[0] == '\0') {
    Serial.println("[MENU] menu_select missing item");
    return;
  }
  if (provisioning_input_locked()) {
    Serial.println("[PROVISION] menu_select ignored (provisioning_active)");
    return;
  }
  ship_menu_begin_local_scan_request(menu_item);
  tx_msg_t tx_msg = {};
  strncpy(tx_msg.type, "INPUT_MENU_SELECT", sizeof(tx_msg.type) - 1);
  tx_msg.delta = menu_index;
  tx_msg.has_delta = true;
  strncpy(tx_msg.id, menu_item, sizeof(tx_msg.id) - 1);
  tx_msg.has_id = true;
  if (uart_tx_queue != NULL) {
    xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
  }
  if (label && label[0]) {
    Serial.printf("[MENU] tap=%s\n", label);
  } else {
    Serial.printf("[MENU] menu_item=%s index=%d\n", menu_item, menu_index);
  }
}

static void ship_menu_send_retry(const char* label) {
  request_sense_wake("retry");
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_RETRY";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  senseSerial.println(output);
  Serial.printf("[MENU] retry=%s\n", label ? label : "unknown");
}

static void ship_menu_send_manual_ota(const char* reason) {
  request_sense_wake("manual_ota");
  lcd_manual_ota_override_set("manual_button");
  if (ship_menu_settings_status != NULL) {
    lv_label_set_text(ship_menu_settings_status, "Starting OTA...");
    lv_obj_clear_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN);
    ship_menu_settings_status_hide_at_ms = millis() + 3000;
    lv_timer_handler();
  }
  StaticJsonDocument<160> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_OTA_CHECK";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  if (reason && reason[0]) {
    doc["reason"] = reason;
  }
  String output;
  serializeJson(doc, output);
  senseSerial.println(output);
  Serial.printf("[OTA_MANUAL] tx INPUT_OTA_CHECK reason=%s\n", reason ? reason : "manual");
  unsigned long until = millis() + LCD_OTA_CHECK_STAY_AWAKE_MS;
  if (until > ota_stay_awake_until_ms) {
    ota_stay_awake_until_ms = until;
  }
  resetActivityTimer();
  if (status_screen != NULL && status_label != NULL) {
    status_screen_use_text("Starting OTA\nUpdate...");
    lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
    status_screen_shown_time = millis();
    set_status_reset_visible(false);
    lv_timer_handler();
  }
}

static void ship_menu_update_versions_label() {
  if (ship_menu_settings_versions == NULL) {
    return;
  }
  const char* lcd_fw = kFirmwareVersion ? kFirmwareVersion : "unknown";
  const char* sense_fw = (g_sense_fw_version[0] != '\0') ? g_sense_fw_version : "--";
  char buf[96];
  snprintf(buf, sizeof(buf), "LCD %s  Sense %s", lcd_fw, sense_fw);
  lv_label_set_text(ship_menu_settings_versions, buf);
}

static void ship_menu_request_fw_info() {
  unsigned long now_ms = millis();
  user_activity_bump("fw_info_request");
  g_fw_info_request_ms = now_ms;
  g_fw_info_last_attempt_ms = now_ms;
  g_fw_info_retry_deadline_ms = now_ms + FW_INFO_RETRY_TIMEOUT_MS;
  g_fw_info_retry_count = 0;
  g_fw_info_response_received = false;
  Serial.printf("[MENU] request_fw_info cached_sense_fw=%s awake=%d sync=%d recent=%d\n",
                g_sense_fw_version[0] ? g_sense_fw_version : "--",
                sense_awake_confirmed ? 1 : 0,
                link_synced ? 1 : 0,
                sense_recently_heard(1500) ? 1 : 0);
  tx_msg_t tx_msg = {};
  strncpy(tx_msg.type, "INPUT_FW_INFO", sizeof(tx_msg.type) - 1);
  if (uart_tx_queue != NULL) {
    xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
  }
}

static void ship_menu_service_fw_info_request(unsigned long now_ms) {
  if (ui_screen_state != SCREEN_SETTINGS) {
    if (deferred_awake_tx_valid &&
        strcmp(deferred_awake_tx_msg.type, "INPUT_FW_INFO") == 0) {
      deferred_awake_tx_valid = false;
      deferred_awake_tx_last_ping_ms = 0;
    }
    g_fw_info_request_ms = 0;
    g_fw_info_last_attempt_ms = 0;
    g_fw_info_retry_deadline_ms = 0;
    g_fw_info_retry_count = 0;
    g_fw_info_response_received = false;
    return;
  }
  if (g_fw_info_request_ms == 0 || g_fw_info_response_received) {
    return;
  }
  if (g_fw_info_retry_deadline_ms > 0 && now_ms >= g_fw_info_retry_deadline_ms) {
    return;
  }
  if (g_fw_info_last_attempt_ms > 0 &&
      (now_ms - g_fw_info_last_attempt_ms) < FW_INFO_RETRY_INTERVAL_MS) {
    return;
  }
  if (deferred_awake_tx_valid &&
      strcmp(deferred_awake_tx_msg.type, "INPUT_FW_INFO") == 0) {
    deferred_awake_tx_last_ping_ms = 0;
    user_activity_bump("fw_info_retry");
    deferred_awake_tx_service();
  } else {
    tx_msg_t tx_msg = {};
    strncpy(tx_msg.type, "INPUT_FW_INFO", sizeof(tx_msg.type) - 1);
    if (uart_tx_queue != NULL) {
      user_activity_bump("fw_info_retry");
      xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
    }
  }
  g_fw_info_last_attempt_ms = now_ms;
  g_fw_info_retry_count++;
  Serial.printf("[MENU] retry_fw_info attempt=%u age_ms=%lu awake=%d sync=%d recent=%d deferred=%d\n",
                (unsigned)g_fw_info_retry_count,
                (unsigned long)(now_ms - g_fw_info_request_ms),
                sense_awake_confirmed ? 1 : 0,
                link_synced ? 1 : 0,
                sense_recently_heard(1500) ? 1 : 0,
                (deferred_awake_tx_valid &&
                 strcmp(deferred_awake_tx_msg.type, "INPUT_FW_INFO") == 0) ? 1 : 0);
}

static void ship_menu_send_action(const ship_menu_hitbox_t* hb) {
  if (!hb) {
    return;
  }
  if (provisioning_input_locked()) {
    Serial.println("[PROVISION] tap ignored (provisioning_active)");
    return;
  }
  if (ui_screen_state != SCREEN_HOME &&
      ui_screen_state != SCREEN_SECOND &&
      ui_screen_state != SCREEN_SETTINGS) {
    Serial.println("[UI_BUSY] tap ignored (screen)");
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
    case SHIP_MENU_ACTION_RESET_WIFI: {
      request_sense_wake("reset_wifi");
      provision_user_requested = true;
      StaticJsonDocument<128> doc;
      doc["ver"] = PROTOCOL_VERSION;
      doc["type"] = "INPUT_RESET_WIFI";
      doc["msg_id"] = get_next_msg_id();
      doc["ts"] = millis();
      String output;
      serializeJson(doc, output);
      senseSerial.println(output);
      provision_qr_wait_begin("menu_action");
      if (status_screen != NULL && status_label != NULL) {
        status_screen_use_text("Resetting\nWi-Fi...");
        lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
        status_screen_shown_time = millis();
        set_status_reset_visible(false);
        lv_timer_handler();
      }
      break;
    }
    case SHIP_MENU_ACTION_MANUAL_OTA:
      ship_menu_send_manual_ota("menu_action");
      break;
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
  // LCD_Minimal/LCD_Minimal.ino: ship_menu_handle_ui_status
  const char* op    = doc["op"]    | "";
  const char* phase = doc["phase"] | "";
  const char* text  = doc["text"]  | "";
  const char* mode  = doc["mode"]  | "";
  const char* ui_policy = doc["ui_policy"] | "";
  uint32_t job_id = (uint32_t)(doc["job_id"] | (uint32_t)(doc["msg_id"] | 0));
  uint32_t msg_id = (uint32_t)(doc["msg_id"] | 0);
  char prev_phase[16] = {0};
  char prev_op[16] = {0};
  char prev_mode[16] = {0};
  strncpy(prev_phase, g_ship_ui_phase, sizeof(prev_phase) - 1);
  strncpy(prev_op, g_ship_ui_op, sizeof(prev_op) - 1);
  strncpy(prev_mode, g_ship_ui_mode, sizeof(prev_mode) - 1);
  uint32_t prev_job_id = g_ship_ui_job_id;
  const char* prev_screen = ui_screen_state_name(ui_screen_state);
  if (msg_id && msg_id == g_last_ui_status_msg_id) {
    Serial.printf("[UI_STATUS] dup msg_id=%u op=%s phase=%s mode=%s job_id=%lu ignored\n",
                  (unsigned)msg_id,
                  op,
                  phase,
                  mode,
                  (unsigned long)job_id);
    return;
  }
  if (msg_id) {
    g_last_ui_status_msg_id = msg_id;
  }
  Serial.printf("[SHIP_UI_STATUS] msg_id=%u job_id=%lu op=%s phase=%s mode=%s prev_phase=%s prev_job_id=%lu screen=%s text=%s\n",
                (unsigned)msg_id,
                (unsigned long)job_id,
                op,
                phase,
                mode,
                prev_phase,
                (unsigned long)prev_job_id,
                prev_screen ? prev_screen : "UNKNOWN",
                text);
  if (g_voice_fire_and_forget_ignore_ui && strcmp(op, "VOICE") == 0) {
    if (strcmp(phase, "DONE") == 0 || strcmp(phase, "ERROR") == 0) {
      g_voice_fire_and_forget_ignore_ui = false;
      waiting_for_voice_response = false;
      voice_response_deadline_ms = 0;
      g_ship_voice_json_pending = false;
      Serial.printf("[VOICE_FAF] backend_complete phase=%s job_id=%lu\n",
                    phase,
                    (unsigned long)job_id);
    } else {
      Serial.printf("[VOICE_FAF] ignore UI_STATUS phase=%s job_id=%lu\n",
                    phase,
                    (unsigned long)job_id);
    }
    return;
  }
  bool prev_processing_phase = (strcmp(prev_phase, "RESULT_WAITING") == 0 ||
                                strcmp(prev_phase, "UPLOAD_STARTING") == 0 ||
                                strcmp(prev_phase, "UPLOADING") == 0 ||
                                strcmp(prev_phase, "PROCESSING") == 0);
  bool new_capturing_phase = (strcmp(phase, "CAPTURING") == 0 ||
                              strcmp(phase, "PREPARING") == 0);
  if (strcmp(op, "SCAN") == 0 && prev_processing_phase && new_capturing_phase) {
    Serial.printf("[SHIP_UI_STATUS][REGRESS] prev_phase=%s -> %s prev_job_id=%lu new_job_id=%lu prev_screen=%s msg_id=%u\n",
                  prev_phase,
                  phase,
                  (unsigned long)prev_job_id,
                  (unsigned long)job_id,
                  prev_screen ? prev_screen : "UNKNOWN",
                  (unsigned)msg_id);
  }
  if (strcmp(op, "SCAN") == 0 && prev_job_id && job_id && job_id != prev_job_id) {
    Serial.printf("[SHIP_UI_STATUS][JOB_CHANGE] prev_job_id=%lu new_job_id=%lu prev_phase=%s new_phase=%s prev_screen=%s\n",
                  (unsigned long)prev_job_id,
                  (unsigned long)job_id,
                  prev_phase,
                  phase,
                  prev_screen ? prev_screen : "UNKNOWN");
  }
  const char* text_raw = (text && text[0]) ? text : "";
  strncpy(g_ship_ui_op, op ? op : "", sizeof(g_ship_ui_op) - 1);
  g_ship_ui_op[sizeof(g_ship_ui_op) - 1] = '\0';
  strncpy(g_ship_ui_phase, phase ? phase : "", sizeof(g_ship_ui_phase) - 1);
  g_ship_ui_phase[sizeof(g_ship_ui_phase) - 1] = '\0';
  strncpy(g_ship_ui_text, text_raw, sizeof(g_ship_ui_text) - 1);
  g_ship_ui_text[sizeof(g_ship_ui_text) - 1] = '\0';
  strncpy(g_ship_ui_mode, mode ? mode : "", sizeof(g_ship_ui_mode) - 1);
  g_ship_ui_mode[sizeof(g_ship_ui_mode) - 1] = '\0';
  strncpy(g_ship_ui_policy, ui_policy ? ui_policy : "", sizeof(g_ship_ui_policy) - 1);
  g_ship_ui_policy[sizeof(g_ship_ui_policy) - 1] = '\0';
  g_ship_ui_last_ms = millis();
  g_ship_ui_job_id = job_id;
  g_last_ui_status_job_id = job_id;
  strncpy(g_last_ui_status_phase, phase ? phase : "", sizeof(g_last_ui_status_phase) - 1);
  g_last_ui_status_phase[sizeof(g_last_ui_status_phase) - 1] = '\0';
  strncpy(g_last_ui_status_op, op ? op : "", sizeof(g_last_ui_status_op) - 1);
  g_last_ui_status_op[sizeof(g_last_ui_status_op) - 1] = '\0';
  strncpy(g_last_ui_status_mode, mode ? mode : "", sizeof(g_last_ui_status_mode) - 1);
  g_last_ui_status_mode[sizeof(g_last_ui_status_mode) - 1] = '\0';
  if (job_id && job_id != g_ship_ui_finalized_job_id) {
    g_ship_ui_finalized = false;
  }

  if (strcmp(op, "SCAN") == 0) {
    bool is_busy = (strcmp(phase, "CAPTURING") == 0 ||
                    strcmp(phase, "PREPARING") == 0 ||
                    strcmp(phase, "PROCESSING") == 0 ||
                    strcmp(phase, "WAITING") == 0 ||
                    strcmp(phase, "WAITING_INPUT") == 0 ||
                    strcmp(phase, "UPLOAD_STARTING") == 0 ||
                    strcmp(phase, "UPLOADING") == 0 ||
                    strcmp(phase, "RESULT_WAITING") == 0);
    bool is_terminal = (strcmp(phase, "ERROR") == 0 ||
                        strcmp(phase, "DONE") == 0 ||
                        strcmp(phase, "SUCCESS") == 0 ||
                        strcmp(phase, "COMPLETE") == 0 ||
                        strcmp(phase, "RESULT_READY") == 0);
    g_ship_ui_busy = is_busy;
    g_ship_ui_terminal = is_terminal;
    g_ship_ui_error = (strcmp(phase, "ERROR") == 0);
    if (is_terminal) {
      waiting_for_scan_response = false;
    }
  }
  g_ship_ui_dirty = true;

  if (app_event_queue != NULL) {
    app_event_t evt = {};
    evt.type = EVT_SHIP_UI_STATUS;
    strncpy(evt.data.ship_ui_status.op, op ? op : "", sizeof(evt.data.ship_ui_status.op) - 1);
    evt.data.ship_ui_status.op[sizeof(evt.data.ship_ui_status.op) - 1] = '\0';
    strncpy(evt.data.ship_ui_status.mode, mode ? mode : "", sizeof(evt.data.ship_ui_status.mode) - 1);
    evt.data.ship_ui_status.mode[sizeof(evt.data.ship_ui_status.mode) - 1] = '\0';
    strncpy(evt.data.ship_ui_status.phase, phase ? phase : "", sizeof(evt.data.ship_ui_status.phase) - 1);
    evt.data.ship_ui_status.phase[sizeof(evt.data.ship_ui_status.phase) - 1] = '\0';
    strncpy(evt.data.ship_ui_status.text, text_raw, sizeof(evt.data.ship_ui_status.text) - 1);
    evt.data.ship_ui_status.text[sizeof(evt.data.ship_ui_status.text) - 1] = '\0';
    strncpy(evt.data.ship_ui_status.ui_policy, ui_policy ? ui_policy : "", sizeof(evt.data.ship_ui_status.ui_policy) - 1);
    evt.data.ship_ui_status.ui_policy[sizeof(evt.data.ship_ui_status.ui_policy) - 1] = '\0';
    evt.data.ship_ui_status.job_id = job_id;
    evt.data.ship_ui_status.msg_id = msg_id;
    if (xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20)) != pdTRUE) {
      Serial.println("[SHIP_UI_STATUS] event queue full -> using latched apply");
    }
  }
}

static void ui_apply_ship_meal_result(const JsonDocument& doc) {
  // Nutrition result from scan
  int calories = doc["calories"] | 0;
  float protein_g = doc["protein_g"] | 0.0f;
  float carbs_g = doc["carbs_g"] | 0.0f;
  float fat_g = doc["fat_g"] | 0.0f;
  float confidence = doc["confidence"] | 0.0f;
  const char* meal_summary = doc["meal_summary"] | "";
  const char* recommendation = doc["recommendation"] | "";
  const char* mode = doc["mode"] | "";
  uint32_t job_id = (uint32_t)(doc["job_id"] | 0);

  if (job_id != 0 && g_ship_ui_job_id != 0 &&
      ui_screen_state == SCREEN_PROCESSING &&
      job_id != g_ship_ui_job_id) {
    Serial.printf("[SHIP_MEAL] ignoring stale result active_job_id=%lu result_job_id=%lu\n",
                  (unsigned long)g_ship_ui_job_id,
                  (unsigned long)job_id);
    return;
  }

  if (!dish_processing_active && ui_screen_state != SCREEN_PROCESSING) {
    bool allow_late = false;
    unsigned long now_ms = millis();
    if (dish_timeout_at_ms > 0 &&
        (now_ms - dish_timeout_at_ms) <= DISH_RESULT_LATE_GRACE_MS &&
        (dish_timeout_job_id == 0 || job_id == dish_timeout_job_id)) {
      allow_late = true;
      Serial.printf("[SHIP_MEAL] late_result_override age_ms=%lu job_id=%lu\n",
                    (unsigned long)(now_ms - dish_timeout_at_ms),
                    (unsigned long)job_id);
    }
    if (!allow_late) {
      Serial.println("[SHIP_MEAL] ignoring result (not waiting)");
      return;
    }
  }

  const char* safe_mode = (mode && mode[0]) ? mode : (g_ship_ui_mode[0] ? g_ship_ui_mode : "dish");
  if (strcmp(safe_mode, "discard") == 0 || ship_mode_is_dish("SCAN", safe_mode)) {
    Serial.printf("[UART] %s mode - ignoring meal result, not displaying meal nutrition\n",
                  safe_mode);
    dish_processing_active = false;
    dish_processing_start_ms = 0;
    dish_timeout_at_ms = 0;
    dish_timeout_job_id = 0;
    if (meal_result_screen != NULL) {
      lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
    }
    if (list_container != NULL) {
      lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }

  Serial.printf("[SHIP_MEAL] received result kcal=%d conf=%.2f mode=%s -> show MEAL_RESULT\n",
                calories, confidence, mode ? mode : "");

  strncpy(g_ship_ui_op, "SCAN", sizeof(g_ship_ui_op) - 1);
  g_ship_ui_op[sizeof(g_ship_ui_op) - 1] = '\0';
  strncpy(g_ship_ui_mode, safe_mode, sizeof(g_ship_ui_mode) - 1);
  g_ship_ui_mode[sizeof(g_ship_ui_mode) - 1] = '\0';
  strncpy(g_ship_ui_phase, "RESULT_READY", sizeof(g_ship_ui_phase) - 1);
  g_ship_ui_phase[sizeof(g_ship_ui_phase) - 1] = '\0';
  strncpy(g_ship_ui_text, meal_summary ? meal_summary : "", sizeof(g_ship_ui_text) - 1);
  g_ship_ui_text[sizeof(g_ship_ui_text) - 1] = '\0';
  g_ship_ui_job_id = job_id;
  g_ship_ui_last_ms = millis();
  g_ship_ui_busy = false;
  g_ship_ui_terminal = true;
  g_ship_ui_error = false;
  waiting_for_scan_response = false;
  if (job_id) {
    g_ship_ui_finalized = true;
    g_ship_ui_finalized_job_id = job_id;
  }
  dish_processing_active = false;
  dish_processing_start_ms = 0;
  dish_timeout_at_ms = 0;
  dish_timeout_job_id = 0;

  log_active_screen("meal_result_rx");
  if (status_screen != NULL) {
    lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
    Serial.println("[STATUS] Hiding status screen - meal result received");
  }

  resetActivityTimer();

  if (app_event_queue != NULL) {
    app_event_t evt = {};
    evt.type = EVT_SHOW_MEAL_RESULT;
    evt.data.meal_result.calories = calories;
    evt.data.meal_result.protein_g = protein_g;
    evt.data.meal_result.carbs_g = carbs_g;
    evt.data.meal_result.fat_g = fat_g;
    strncpy(evt.data.meal_result.meal_summary, meal_summary ? meal_summary : "",
            sizeof(evt.data.meal_result.meal_summary) - 1);
    evt.data.meal_result.meal_summary[sizeof(evt.data.meal_result.meal_summary) - 1] = '\0';
    strncpy(evt.data.meal_result.recommendation, recommendation ? recommendation : "",
            sizeof(evt.data.meal_result.recommendation) - 1);
    evt.data.meal_result.recommendation[sizeof(evt.data.meal_result.recommendation) - 1] = '\0';
    if (xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20)) != pdTRUE) {
      Serial.println("[UI_EVT][WARN] meal_result queue full, dropping oldest");
      app_event_t dropped = {};
      xQueueReceive(app_event_queue, &dropped, 0);
      if (xQueueSend(app_event_queue, &evt, 0) == pdTRUE) {
        Serial.println("[UART] Posted EVT_SHOW_MEAL_RESULT event to UI task (retry)");
      }
    } else {
      Serial.println("[UART] Posted EVT_SHOW_MEAL_RESULT event to UI task");
    }
  }
}

static void ui_handle_meal_result_event(const app_event_t* evt) {
  if (!evt) {
    return;
  }
  if (g_base_screen != NULL) {
    lv_scr_load(g_base_screen);
  }
  ui_screen_state = SCREEN_RESULT;
  ui_busy = false;
  dish_processing_active = false;
  dish_processing_start_ms = 0;

  log_active_screen("meal_result_show");
  Serial.printf("[UI] Showing meal result: %d cal, %.1fg protein, %.1fg carbs, %.1fg fat\n",
                evt->data.meal_result.calories, evt->data.meal_result.protein_g,
                evt->data.meal_result.carbs_g, evt->data.meal_result.fat_g);

  if (list_container != NULL) {
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  if (loading_screen != NULL) {
    lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
  }
  if (status_screen != NULL) {
    lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
    status_screen_shown_time = 0;
  }

  if (meal_result_screen != NULL) {
    lv_obj_clear_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
    if (meal_calories_label != NULL) {
      char cal_text[32];
      snprintf(cal_text, sizeof(cal_text), "%dcal", evt->data.meal_result.calories);
      lv_label_set_text(meal_calories_label, cal_text);
    }
    if (meal_description_label != NULL) {
      lv_label_set_text(meal_description_label, evt->data.meal_result.meal_summary);
    }
    if (meal_protein_value_label != NULL) {
      char protein_text[32];
      snprintf(protein_text, sizeof(protein_text), "%.0fg", evt->data.meal_result.protein_g);
      lv_label_set_text(meal_protein_value_label, protein_text);
    }
    if (meal_carbs_value_label != NULL) {
      char carbs_text[32];
      snprintf(carbs_text, sizeof(carbs_text), "%.0fg", evt->data.meal_result.carbs_g);
      lv_label_set_text(meal_carbs_value_label, carbs_text);
    }
    if (meal_fat_value_label != NULL) {
      char fat_text[32];
      snprintf(fat_text, sizeof(fat_text), "%.0fg", evt->data.meal_result.fat_g);
      lv_label_set_text(meal_fat_value_label, fat_text);
    }
    if (meal_recommendation_label != NULL) {
      lv_label_set_text(meal_recommendation_label, evt->data.meal_result.recommendation);
    }
    ui_lvgl_tick();
    meal_result_shown_time = millis();
  }
}

static void create_custom_ui() {
  // Get default screen
  lv_obj_t *scr = lv_scr_act();
  g_base_screen = scr;
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

  expiry_timeout_ring = lv_arc_create(expiry_screen);
  lv_obj_remove_style(expiry_timeout_ring, NULL, LV_PART_KNOB);
  lv_obj_set_size(expiry_timeout_ring, 352, 352);
  lv_arc_set_range(expiry_timeout_ring, 0, 1000);
  lv_arc_set_value(expiry_timeout_ring, 1000);
  lv_arc_set_bg_angles(expiry_timeout_ring, 0, 360);
  lv_arc_set_rotation(expiry_timeout_ring, 270);
  lv_obj_set_style_arc_width(expiry_timeout_ring, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(expiry_timeout_ring, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(expiry_timeout_ring, lv_color_hex(0x2C71BE), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(expiry_timeout_ring, (lv_opa_t)80, LV_PART_MAIN);
  lv_obj_set_style_arc_color(expiry_timeout_ring, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(expiry_timeout_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_outline_width(expiry_timeout_ring, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(expiry_timeout_ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(expiry_timeout_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(expiry_timeout_ring);
  
  expiry_title_label = lv_label_create(expiry_screen);
  lv_label_set_text(expiry_title_label, "Expiration");
  lv_obj_set_style_text_font(expiry_title_label, &lv_font_montserrat_26, LV_PART_MAIN);
  lv_obj_set_style_text_color(expiry_title_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(expiry_title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(expiry_title_label, LV_ALIGN_TOP_MID, 0, 32);
  
  expiry_back_button = NULL;
  expiry_back_label = NULL;

  expiry_month_button = lv_btn_create(expiry_screen);
  lv_obj_set_size(expiry_month_button, 122, 64);
  lv_obj_align(expiry_month_button, LV_ALIGN_TOP_MID, -74, 104);
  lv_obj_set_style_radius(expiry_month_button, 22, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(expiry_month_button, 0, LV_PART_MAIN);

  expiry_month_label = lv_label_create(expiry_month_button);
  lv_label_set_text(expiry_month_label, "Jan");
  lv_obj_set_style_text_font(expiry_month_label, &lv_font_montserrat_30, LV_PART_MAIN);
  lv_obj_set_style_text_align(expiry_month_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(expiry_month_label);
  expiry_date_label = expiry_month_label;

  expiry_day_button = lv_btn_create(expiry_screen);
  lv_obj_set_size(expiry_day_button, 122, 64);
  lv_obj_align(expiry_day_button, LV_ALIGN_TOP_MID, 74, 104);
  lv_obj_set_style_radius(expiry_day_button, 22, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(expiry_day_button, 0, LV_PART_MAIN);

  expiry_day_label = lv_label_create(expiry_day_button);
  lv_label_set_text(expiry_day_label, "01");
  lv_obj_set_style_text_font(expiry_day_label, &lv_font_montserrat_30, LV_PART_MAIN);
  lv_obj_set_style_text_align(expiry_day_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(expiry_day_label);

  expiry_year_button = lv_btn_create(expiry_screen);
  lv_obj_set_size(expiry_year_button, 168, 64);
  lv_obj_align(expiry_year_button, LV_ALIGN_TOP_MID, 0, 192);
  lv_obj_set_style_radius(expiry_year_button, 22, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(expiry_year_button, 0, LV_PART_MAIN);

  expiry_year_label = lv_label_create(expiry_year_button);
  lv_label_set_text(expiry_year_label, "2026");
  lv_obj_set_style_text_font(expiry_year_label, &lv_font_montserrat_30, LV_PART_MAIN);
  lv_obj_set_style_text_align(expiry_year_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(expiry_year_label);

  expiry_hint_label = NULL;

  expiry_check_button = lv_btn_create(expiry_screen);
  lv_obj_set_size(expiry_check_button, 132, 52);
  lv_obj_align(expiry_check_button, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_style_bg_color(expiry_check_button, lv_color_hex(0xFFB703), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(expiry_check_button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(expiry_check_button, 20, LV_PART_MAIN);
  lv_obj_set_style_border_width(expiry_check_button, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(expiry_check_button, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(expiry_check_button, 0, LV_PART_MAIN);

  lv_obj_t *check_label = lv_label_create(expiry_check_button);
  lv_label_set_text(check_label, "OK");
  lv_obj_set_style_text_font(check_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(check_label, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_center(check_label);

  expiry_backspace_button = NULL;
  for (int i = 0; i < 10; ++i) {
    expiry_keypad_buttons[i] = NULL;
  }
  expiry_refresh_picker_ui();

  dump_screen_registry();
  dump_route_table();
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

  // Shopping-list UI is retired for the ship flow.
  if (menu_screen != NULL) {
    lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  }
  if (list_container != NULL) {
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  menu_screen_visible = false;
  Serial.println("[MENU] legacy menu disabled");
  return;
  
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

  if (list_container != NULL) {
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  return;
  
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

  // Shopping-list UI is retired; keep the legacy list surface hidden.
  if (empty_label != NULL) {
    lv_obj_del(empty_label);
    empty_label = NULL;
  }
  lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  return;
  
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
  if (provisioning_input_locked()) {
    return;
  }
  // Ignore scrolls during brief wake-up period (150ms)
  if (millis() < scroll_ignore_until) {
    return;
  }
  user_activity_since_sleep = true;
  last_scroll_activity_ms = millis();
  scroll_activity_pending = true;
  
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
  if (provisioning_input_locked()) {
    return;
  }
  // Ignore scrolls during brief wake-up period (150ms)
  if (millis() < scroll_ignore_until) {
    return;
  }
  user_activity_since_sleep = true;
  last_scroll_activity_ms = millis();
  scroll_activity_pending = true;
  
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

  // Temporary hardening: deep-sleep wake is touch-only.
  // Leave encoder handling enabled while fully awake, but do not let
  // encoder A/B lines trigger EXT1 wake until the false-wake path is fixed.
  wakeMask |= (1ULL << LCD_WAKE_GPIO);
  Serial.printf("[EXT1_MASK] touch=%d enc_a_level=%d enc_b_level=%d mask_touch=%d mask_enc_a=%d mask_enc_b=%d\n",
                digitalRead(PIN_TOUCH_INT),
                digitalRead(PIN_EC1_A),
                digitalRead(PIN_EC1_B),
                (wakeMask & (1ULL << LCD_WAKE_GPIO)) ? 1 : 0,
                (wakeMask & (1ULL << PIN_EC1_A)) ? 1 : 0,
                (wakeMask & (1ULL << PIN_EC1_B)) ? 1 : 0);
  return wakeMask;
}

static void log_ext1_wakeup_status(const char* phase) {
  uint64_t status = esp_sleep_get_ext1_wakeup_status();
  Serial.printf("[EXT1_WAKE] phase=%s status=0x%llx touch=%d enc_a=%d enc_b=%d\n",
                phase ? phase : "unknown",
                (unsigned long long)status,
                (status & (1ULL << LCD_WAKE_GPIO)) ? 1 : 0,
                (status & (1ULL << PIN_EC1_A)) ? 1 : 0,
                (status & (1ULL << PIN_EC1_B)) ? 1 : 0);
}

static void clear_input_wake_sources(const char* reason) {
  uint16_t touch_x = 0;
  uint16_t touch_y = 0;
  uint8_t touch_first = 0;
  uint8_t touch_second = 0;
  if (g_touch_initialized) {
    touch_first = getTouch(&touch_x, &touch_y);
    delay(10);
    touch_second = getTouch(&touch_x, &touch_y);
  }

  for (int i = 0; i < 5; ++i) {
    digitalRead(PIN_EC1_A);
    digitalRead(PIN_EC1_B);
    delay(2);
  }

  int touch_level = digitalRead(PIN_TOUCH_INT);
  int enc_a_level = digitalRead(PIN_EC1_A);
  int enc_b_level = digitalRead(PIN_EC1_B);
  Serial.printf("[WAKE_CLEAR] reason=%s touch_first=%u touch_second=%u touch_level=%d enc_a=%d enc_b=%d touch_active=%d any_active=%d\n",
                reason ? reason : "unknown",
                (unsigned)touch_first,
                (unsigned)touch_second,
                touch_level,
                enc_a_level,
                enc_b_level,
                lcd_touch_wake_active() ? 1 : 0,
                lcd_wake_pins_active() ? 1 : 0);
}

static bool input_wake_sources_idle(const char* reason) {
  clear_input_wake_sources(reason);
  delay(10);
  bool touch_active = lcd_touch_wake_active();
  if (touch_active) {
    Serial.printf("[SLEEP_SANITY] touch_source_active reason=%s touch=%d enc_a=%d enc_b=%d\n",
                  reason ? reason : "unknown",
                  digitalRead(PIN_TOUCH_INT),
                  digitalRead(PIN_EC1_A),
                  digitalRead(PIN_EC1_B));
  }
  return !touch_active;
}

static void enterLightSleep() {
  if (sleep_blocked_for_ota()) {
    Serial.println("[SLEEP] blocked (ota_pending)");
    resetActivityTimer();
    return;
  }
  sleep_cancelled_by_user_input = false;
  Serial.println("========================================");
  Serial.println("Preparing for DEEP SLEEP...");
  Serial.println("========================================");
  uint16_t dummy_x = 0, dummy_y = 0;
  if (!notify_sense_sleep()) {
    if (sleep_cancelled_by_user_input) {
      Serial.println("[SLEEP] user_input cancelled pre_sleep");
      resetActivityTimer();
      return;
    }
    if (sleep_deny_active) {
      if (millis() - last_sleep_retry_log_ms > 1000) {
        Serial.printf("[SLEEP] deny_wait reason=%s retry_ms=%lu\n",
                      sleep_deny_reason[0] ? sleep_deny_reason : "unknown",
                      sleep_deny_retry_ms > 0 ? sleep_deny_retry_ms : (unsigned long)SLEEP_DENY_RETRY_DEFAULT_MS);
        last_sleep_retry_log_ms = millis();
      }
      return;
    }
    Serial.println("[SLEEP] Sense sleep not confirmed - staying awake");
    sleep_handshake_fail_count++;
    if (sleep_handshake_fail_link) {
      sleep_handshake_fail_count = 0;
      sleep_retry_requires_user = false;
    } else if (sleep_handshake_fail_count >= 3) {
      sleep_retry_requires_user = true;
    }
    unsigned long backoff_ms = sleep_handshake_fail_link ? SLEEP_LINK_RETRY_MS
                          : (sleep_retry_requires_user ? 60000UL
                          : (30000UL + (sleep_handshake_fail_count > 1 ? (sleep_handshake_fail_count - 1) * 10000UL : 0)));
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
    return;
  }

  sleep_handshake_fail_count = 0;
  sleep_retry_requires_user = false;
  sleep_retry_allowed_ms = 0;
  sleep_wait_for_sense_idle = false;

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
  // If menu is visible, hide it and show the list before going to sleep
  bool lvgl_ready = g_lvgl_running && lv_is_initialized();
  if (menu_screen_visible && lvgl_ready) {
    Serial.println("[SLEEP] Menu is visible - hiding menu and showing list before sleep");
    hide_menu_screen();  // This function hides the menu and shows the list
  }
  
  menu_selected_index = 0;  // Reset menu selection
  menu_cooldown_until = 0;  // Reset menu cooldown on sleep
  logged_screen_shown_time = 0;  // Reset logged screen timeout on sleep
  
  // Stop any animations (only if LVGL is active)
  if (lvgl_ready) {
  stop_glowing_animation();
  } else {
    Serial.println("[SLEEP] skip_ui_reset (lvgl_inactive)");
  }

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

  lcd_send_diag_pre_sleep();

  // Turn off LCD panel before deep sleep
  Serial.println("Turning off panel for sleep...");
  lcd_panel_set_power(false);
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
    abort_sleep_transition("ext0_active_pre_sleep");
    delay(250);
    return;
  }
  if (!input_wake_sources_idle("idle_pre_sleep")) {
    Serial.println("[SLEEP_SANITY] input wake sources still active; refusing_sleep");
    abort_sleep_transition("ext1_active_pre_sleep");
    delay(250);
    return;
  }
  g_lcd_schedule_timer_armed = 0;
  g_lcd_schedule_wake_in_s = 0;
  g_lcd_schedule_next_epoch = 0;
  uint32_t sleep_timer_sec = (sleep_fallback_timer_sec > 0)
                               ? sleep_fallback_timer_sec
                               : (g_lcd_maintenance_timer_armed && g_lcd_maintenance_wake_in_s > 0)
                                   ? g_lcd_maintenance_wake_in_s
                               : LCD_OTA_WAKE_INTERVAL_SEC;
  const char* timer_reason = (sleep_fallback_timer_sec > 0)
                               ? "fallback"
                               : (g_lcd_maintenance_timer_armed && g_lcd_maintenance_wake_in_s > 0)
                                   ? "maintenance"
                               : "periodic";
  Serial.printf("[SLEEP_TIMER] reason=%s timer_s=%lu maint_armed=%d wake_in_s=%lu\n",
                timer_reason,
                (unsigned long)sleep_timer_sec,
                g_lcd_maintenance_timer_armed ? 1 : 0,
                (unsigned long)g_lcd_maintenance_wake_in_s);
  configure_sleep_sources(true, sleep_timer_sec);
  if (sleep_fallback_timer_sec > 0) {
    Serial.printf("[SLEEP_PROTO] fallback_timer_active timer_s=%lu\n",
                  (unsigned long)sleep_fallback_timer_sec);
    sleep_fallback_timer_sec = 0;
  }
  if (g_lcd_maintenance_timer_armed && g_lcd_maintenance_wake_in_s > 0) {
    Serial.printf("[LCD_MAINT] sleep_timer_armed wake_in_s=%lu\n",
                  (unsigned long)g_lcd_maintenance_wake_in_s);
  }
  lcd_log_rtc_timer_state("pre_deep_sleep");

  Serial.printf("[SLEEP_STATE] entering_deep_sleep now_ms=%lu ext0_gpio=%d ext0_level=%d\n",
                (unsigned long)millis(),
                (int)LCD_WAKE_GPIO,
                (int)LCD_WAKE_LEVEL);
  Serial.println("[SLEEP] entering_deep_sleep");
  sleep_entry_time = millis();

  lcd_set_backlight_binary(false, "deep_sleep");
  if (ui_task_handle != NULL) {
    vTaskDelete(ui_task_handle);
    ui_task_handle = NULL;
  }
  
  // Enter deep sleep (no return)
  Serial.printf("[LCD_SLEEP] wake_sources=%s timer_s=%lu\n",
                sleep_timer_sec > 0 ? "EXT0_TIMER" : "EXT0_ONLY",
                (unsigned long)sleep_timer_sec);
  Serial.println("=================================");
  Serial.println("[SLEEP] entering deep sleep");
  Serial.printf("[SLEEP] wake_gpio=%d\n", WAKE_GPIO);
  Serial.printf("[SLEEP] wake_level=%d\n", HALO_WAKE_LEVEL);
  Serial.println("=================================");
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
    delay(20);
    int touch_level = digitalRead(HALO_WAKE_GPIO);
    if (touch_level == HIGH) {
      Serial.println("[TOUCH] false wake detected");
      Serial.println("[TOUCH] rejected_noise");
      uint32_t retry_timer_sec = (sleep_fallback_timer_sec > 0)
                                   ? sleep_fallback_timer_sec
                                   : (g_lcd_maintenance_timer_armed && g_lcd_maintenance_wake_in_s > 0)
                                       ? g_lcd_maintenance_wake_in_s
                                   : (g_lcd_schedule_timer_armed && g_lcd_schedule_wake_in_s > 0)
                                       ? g_lcd_schedule_wake_in_s
                                       : 0;
      configure_sleep_sources(true, retry_timer_sec);
      Serial.printf("[LCD_SLEEP] wake_sources=%s false_wake_retry_timer_s=%lu\n",
                    retry_timer_sec > 0 ? "EXT0_TIMER" : "EXT0_ONLY",
                    (unsigned long)retry_timer_sec);
      esp_deep_sleep_start();
      return;
    }
    Serial.println("[TOUCH] validated");
  }
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
  lcd_set_backlight_binary(true, "wake");
  g_panel_enabled = true;
  vTaskDelay(pdMS_TO_TICKS(50));  // Use vTaskDelay to yield to other tasks
  
  // Set flag to trigger UI reset on next list render (state was already cleared before sleep)
  just_woke_up = true;
  
  // Start the Sense wake handshake immediately on user wake so Sense boots
  // while the LCD restores its own UI state.
  const char* wake_reason = user_ui_wake ? "user_ui_wake" : "wake_from_sleep";
  Serial.printf("[WAKE] Requesting Sense wake reason=%s\n", wake_reason);
  request_sense_wake(wake_reason);
  if (last_sense_rx_ms == 0 || (millis() - last_sense_rx_ms) > SENSE_RX_STALE_MS) {
    sense_state_set(SENSE_UNKNOWN, wake_reason);
  }

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
  lcd_sleep_ts("tx_input_sleep");
  Serial.printf("[SLEEP_PROTO] tx INPUT_SLEEP msg_id=%u now_ms=%lu\n",
                (unsigned)msg_id,
                (unsigned long)millis());
  return msg_id;
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
  Serial.printf("[SLEEP_PROTO] tx SLEEP_DENY reason=%s retry_ms=%lu\n",
                reason ? reason : "unknown",
                (unsigned long)retry_ms);
}

static void sleep_enter_wait_low_power(const char* reason) {
  lcd_set_idle_screen_dark(true, reason ? reason : "sleep_wait_low_power");
  Serial.printf("[SLEEP] wait_low_power reason=%s backlight=%d panel_on=%d lvgl_running=%d idle_dark=%d\n",
                reason ? reason : "unknown",
                g_backlight_duty,
                g_panel_enabled ? 1 : 0,
                g_lvgl_running ? 1 : 0,
                g_idle_screen_dark ? 1 : 0);
}

static bool sleep_prepare_wake_line_for_request() {
  release_wake_line("pre_sleep_request");
  unsigned long start_ms = millis();
  while ((millis() - start_ms) < 40UL) {
    if (digitalRead(INT_PIN) != LCD_WAKE_LEVEL) {
      Serial.printf("[SLEEP] wake_line_released_local level=%d elapsed_ms=%lu\n",
                    digitalRead(INT_PIN),
                    (unsigned long)(millis() - start_ms));
      return true;
    }
    delay(5);
  }
  sleep_deny_active = true;
  sleep_deny_received = true;
  sleep_deny_retry_ms = 1500;
  strncpy(sleep_deny_reason, "wake_pin_active", sizeof(sleep_deny_reason) - 1);
  sleep_deny_reason[sizeof(sleep_deny_reason) - 1] = '\0';
  sleep_deny_received_ms = millis();
  sleep_enter_wait_low_power("wake_pin_active");
  Serial.printf("[SLEEP] local_wake_line_still_active level=%d\n", digitalRead(INT_PIN));
  return false;
}

// Send sleep signal to Sense board before LCD goes to sleep
static bool notify_sense_sleep() {
  Serial.println("[LCD] Notifying Sense board to sleep...");
  lcd_sleep_ts("notify_sense_sleep");
  sleep_ready_received = false;
  sleep_deny_received = false;
  sleep_deny_retry_ms = 0;
  sleep_deny_reason[0] = '\0';
  sleep_deny_received_ms = 0;
  sleep_deny_active = false;
  sleep_fallback_timer_sec = 0;
  sleep_handshake_fail_link = false;
  if (provisioning_active) {
    unsigned long now_ms = millis();
    unsigned long age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0;
    bool sense_stale = (last_sense_rx_ms > 0 && age_ms > SENSE_RX_STALE_MS);
    if (!(sense_state == SENSE_ASLEEP || sense_stale || !link_synced)) {
      Serial.println("[LCD] Sleep suppressed (provisioning active)");
      return false;
    }
    Serial.println("[LCD] provisioning_active but sense asleep/stale -> allow sleep");
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
  bool sleep_ready_recent = last_sense_sleep_ready_ms > 0 &&
                            (now_ms - last_sense_sleep_ready_ms) <= SENSE_SLEEP_READY_GRACE_MS;
  bool grace_active = now_ms < sense_awake_grace_until_ms;
  bool sense_recent = (last_sense_rx_ms > 0) &&
                      (now_ms - last_sense_rx_ms) < SENSE_RECENT_RX_FOR_SLEEP_MS;
  Serial.printf("[SLEEP] decision est=%d recent=%d grace=%d age_ms=%lu\n",
                sense_awake_estimate ? 1 : 0,
                sense_recent ? 1 : 0,
                grace_active ? 1 : 0,
                age_ms);

  if (sleep_ready_recent) {
    Serial.printf("[SLEEP] sense_ready_recent -> local sleep age_ms=%lu\n", age_ms);
    return true;
  }
  if (sense_state == SENSE_ASLEEP) {
    Serial.printf("[SLEEP] sense_asleep -> local sleep rx_age=%lu\n", age_ms);
    return true;
  }

  bool sense_probably_awake = (sense_state == SENSE_AWAKE) || sense_awake_estimate || sense_recent || grace_active;
  if (!sense_probably_awake || age_ms > SENSE_UNKNOWN_STALE_EXTENDED_MS) {
    if (!link_synced && age_ms > 60000UL) {
      sleep_fallback_timer_sec = SLEEP_FALLBACK_TIMER_SEC;
      Serial.printf("[SLEEP_PROTO] link_unsynced_stale rx_age=%lu -> fallback_timer_sleep\n",
                    age_ms);
      return true;
    }
    if (!link_synced) {
      link_sync_pending = true;
    }
    send_sense_ping();
    Serial.printf("[SLEEP_PROTO] skip INPUT_SLEEP reason=sense_not_awake rx_age=%lu synced=%d state=%s\n",
                  age_ms,
                  link_synced ? 1 : 0,
                  sense_state_name(sense_state));
    sleep_handshake_fail_link = true;
    return false;
  }
  if (!link_synced) {
    link_sync_pending = true;
    Serial.printf("[SLEEP_PROTO] proceed INPUT_SLEEP despite unsynced link rx_age=%lu state=%s est=%d recent=%d grace=%d\n",
                  age_ms,
                  sense_state_name(sense_state),
                  sense_awake_estimate ? 1 : 0,
                  sense_recent ? 1 : 0,
                  grace_active ? 1 : 0);
  }

  const unsigned long ready_timeout_ms = 25000;
  for (uint8_t attempt = 1; attempt <= SLEEP_HANDSHAKE_MAX_ATTEMPTS; ++attempt) {
    if (sleep_ready_received) {
      Serial.println("[SLEEP_PROTO] got SLEEP_READY while waiting_for_sleep_result -> success");
      return true;
    }
    sleep_ready_received = false;
    sleep_deny_received = false;
    if (!sleep_prepare_wake_line_for_request()) {
      return false;
    }
    send_input_sleep_message();
    unsigned long start = millis();
    unsigned long deadline_ms = start + ready_timeout_ms;
    unsigned long baseline_user_activity_ms = last_user_activity_ms;
    unsigned long baseline_scroll_activity_ms = last_scroll_activity_ms;
    Serial.printf("[SLEEP] sent INPUT_SLEEP attempt=%u timeout_ms=%lu\n",
                  (unsigned)attempt,
                  (unsigned long)ready_timeout_ms);
    Serial.printf("[SLEEP] req sent est=%d recent=%d grace=%d wait_ms=%u\n",
                  sense_awake_estimate ? 1 : 0,
                  sense_recent ? 1 : 0,
                  grace_active ? 1 : 0,
                  (unsigned)ready_timeout_ms);
    while (millis() < deadline_ms) {
      if (sleep_blocked_for_ota()) {
        Serial.println("[SLEEP] abort wait (ota_pending)");
        return false;
      }
      uint16_t touch_x = 0, touch_y = 0;
      bool fresh_touch = (millis() >= touch_ignore_until) && !touch_pressed &&
                         (getTouch(&touch_x, &touch_y) == 1);
      bool user_cancel = (last_user_activity_ms > baseline_user_activity_ms) ||
                         (last_scroll_activity_ms > baseline_scroll_activity_ms) ||
                         fresh_touch;
      if (user_cancel) {
        // Swallow the wake tap so it does not immediately trigger a UI action
        // once the panel is back on, and explicitly cancel Sense-side sleep.
        touch_pressed = false;
        touch_wake_only_pending = false;
        touch_press_time = 0;
        touch_press_x = 0;
        touch_press_y = 0;
        long_press_sent = false;
        ship_ai_touch_active = false;
        touch_used_to_dismiss_meal = false;
        touch_ignore_until = millis() + 450;
        scroll_ignore_until = millis() + 200;
        cancel_pending_sleep_for_user_input("pre_sleep_touch");
        abort_sleep_transition("pre_sleep_touch");
        uart_send_input_message("INPUT_WAKE");
        Serial.println("[SLEEP] sent INPUT_WAKE after user abort");
        Serial.println("[SLEEP] abort wait (user_input)");
        return false;
      }
      if (sleep_deny_received) {
        const char* deny_reason = sleep_deny_reason;
        uint32_t retry_ms = sleep_deny_retry_ms > 0 ? sleep_deny_retry_ms : SLEEP_DENY_RETRY_DEFAULT_MS;
        sleep_deny_active = true;
        bool passive_wait = (strcmp(deny_reason, "op_inflight") == 0 ||
                             strcmp(deny_reason, "pre_ready_block") == 0);
        sleep_wait_for_sense_idle = passive_wait;
        sleep_retry_allowed_ms = passive_wait ? 0 : (millis() + retry_ms);
        sleep_handshake_fail_count = 0;
        sleep_retry_requires_user = false;
        sleep_enter_wait_low_power(deny_reason);
        Serial.printf("[SLEEP_PROTO] rx DENY reason=%s retry_ms=%lu\n",
                      deny_reason ? deny_reason : "unknown",
                      (unsigned long)retry_ms);
        if (passive_wait) {
          Serial.printf("[SLEEP] passive_wait_for_sense_idle reason=%s\n",
                        deny_reason ? deny_reason : "unknown");
        }
        return false;
      }
      if (sleep_ready_received) {
        Serial.println("[SLEEP_PROTO] got SLEEP_READY while waiting_for_sleep_result -> success");
        Serial.println("[SLEEP] got_ready -> sleeping");
        Serial.println("[SLEEP_PROTO] decision coordinated reason=ready");
        return true;
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


#include "lcd_uart_rx.h"


// ── UI Task ────────────────────────────────────────────────────────
// ONLY the UI task (and loop() when it runs LVGL) may call LVGL; both hold example_lvgl_lock.

#include "lcd_ui_task.h"


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
    if (deferred_awake_tx_valid) {
      if (sense_ready_for_control_tx()) {
        tx_msg_send_now(&deferred_awake_tx_msg);
        deferred_awake_tx_valid = false;
        deferred_awake_tx_last_ping_ms = 0;
        tx_count++;
      } else {
        deferred_awake_tx_service();
        yielded_early = true;
      }
    }
    if (!deferred_awake_tx_valid) {
      while (uart_tx_queue != NULL && xQueueReceive(uart_tx_queue, &tx_msg, 0) == pdTRUE) {
        if (tx_msg_requires_awake_proof(&tx_msg) && !sense_ready_for_control_tx()) {
          deferred_awake_tx_msg = tx_msg;
          deferred_awake_tx_valid = true;
          deferred_awake_tx_last_ping_ms = 0;
          Serial.printf("[UART] deferred_until_awake type=%s awake=%d synced=%d recent=%d\n",
                        tx_msg.type,
                        sense_awake_confirmed ? 1 : 0,
                        link_synced ? 1 : 0,
                        sense_recently_heard(SENSE_CONTROL_READY_WINDOW_MS) ? 1 : 0);
          deferred_awake_tx_service();
          yielded_early = true;
          break;
        } else {
          tx_msg_send_now(&tx_msg);
        }
        tx_count++;
        if (tx_count >= UART_TX_MAX_PER_LOOP) {
          yielded_early = true;
          break;
        }
      }
    }
    
    // 2) RX read and parse lines
    int rx_bytes = 0;
    while (senseSerial.available() > 0) {
      char c = senseSerial.read();
      uart_rx_raw_bytes_seen++;
      if (UART_RX_DEBUG) {
        Serial.printf("[UART_RAW] rx_byte=0x%02X\n", (uint8_t)c);
      }
      if (uart_rx_diag_raw_logged < UART_RX_DIAG_CAPTURE_LIMIT) {
        char printable = (c >= 32 && c <= 126) ? c : '.';
        Serial.printf("[UART_RAW_DIAG] idx=%lu byte=0x%02X char=%c\n",
                      uart_rx_raw_bytes_seen,
                      (uint8_t)c,
                      printable);
        uart_rx_diag_raw_logged++;
      }
      
      if (c == '\n' || c == '\r') {
        if (uart_rx_line_pos > 0) {
          uart_rx_line_buffer[uart_rx_line_pos] = '\0';
          uart_rx_completed_lines_seen++;
          uart_process_received_message(uart_rx_line_buffer);
          uart_rx_line_pos = 0;
          uart_rx_partial_started_ms = 0;
        }
      } else if (uart_rx_line_pos < MAX_LINE_LENGTH - 1) {
        if (uart_rx_line_pos == 0) {
          uart_rx_partial_started_ms = millis();
        }
        uart_rx_line_buffer[uart_rx_line_pos++] = c;
      } else {
        // Line too long, flush buffer
        Serial.printf("[PROTO] Line overflow (max=%d) - flushing buffer\n", MAX_LINE_LENGTH);
        uart_rx_line_pos = 0;
        uart_rx_partial_started_ms = 0;
      }
      rx_bytes++;
      if (rx_bytes >= UART_RX_MAX_BYTES_PER_LOOP) {
        yielded_early = true;
        break;
      }
    }
    unsigned long now_ms = millis();
    if (uart_rx_line_pos > 0 &&
        uart_rx_partial_started_ms > 0 &&
        (now_ms - uart_rx_partial_started_ms) >= 250 &&
        (now_ms - uart_rx_last_partial_log_ms) >= 1000) {
      char preview[33];
      int preview_len = uart_rx_line_pos < (int)(sizeof(preview) - 1)
                          ? uart_rx_line_pos
                          : (int)(sizeof(preview) - 1);
      memcpy(preview, uart_rx_line_buffer, preview_len);
      preview[preview_len] = '\0';
      Serial.printf("[UART_DIAG] partial_line age_ms=%lu len=%d preview=%s\n",
                    now_ms - uart_rx_partial_started_ms,
                    uart_rx_line_pos,
                    preview);
      uart_rx_last_partial_log_ms = now_ms;
    }
    if ((deferred_awake_tx_valid || uart_rx_valid_msgs_seen == 0) &&
        (now_ms - uart_rx_last_summary_ms) >= 1000) {
      int available_now = senseSerial.available();
      unsigned long proof_age_ms = last_proof_of_life_ms > 0 ? (now_ms - last_proof_of_life_ms) : 0xFFFFFFFFUL;
      unsigned long rx_age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0xFFFFFFFFUL;
      Serial.printf("[UART_DIAG] summary avail=%d rx_gpio=%d raw_bytes=%lu lines=%lu valid=%lu line_pos=%d awake=%d synced=%d rx_age_ms=%lu proof_age_ms=%lu deferred=%d\n",
                    available_now,
                    (int)gpio_get_level((gpio_num_t)UART_RX_PIN),
                    uart_rx_raw_bytes_seen,
                    uart_rx_completed_lines_seen,
                    uart_rx_valid_msgs_seen,
                    uart_rx_line_pos,
                    sense_awake_confirmed ? 1 : 0,
                    link_synced ? 1 : 0,
                    rx_age_ms,
                    proof_age_ms,
                    deferred_awake_tx_valid ? 1 : 0);
      uart_rx_last_summary_ms = now_ms;
    }
    
    vTaskDelay(pdMS_TO_TICKS(yielded_early ? 1 : 5));
  }
}

// ── Activity Timer ────────────────────────────────────────────────
static const unsigned long INACTIVITY_TIMEOUT_MS = 10000;  // 10 seconds
static const unsigned long HOME_SLEEP_DELAY_MS = 10000;  // 10 seconds after HOME shown
// Guardian: hard upper bound for awake time (ms). Set 0 to disable.
static const unsigned long GUARDIAN_FORCE_SLEEP_MS = 5UL * 60UL * 1000UL;
static unsigned long guardian_awake_start_ms = 0;
static bool guardian_sleep_triggered = false;

static bool lcd_sleep_intent_allowed(const char** reason_out) {
  unsigned long now_ms = millis();
  if (reason_out) {
    *reason_out = NULL;
  }
  if (sleep_blocked_for_ota()) {
    if (reason_out) *reason_out = "ota_pending";
    return false;
  }
  if (provisioning_input_locked()) {
    if (reason_out) *reason_out = "provisioning";
    return false;
  }
  if (now_ms < sense_awake_grace_until_ms) {
    if (reason_out) *reason_out = "grace";
    return false;
  }
  if (g_lcd_maintenance_boot_grace_until_ms > 0 && now_ms < g_lcd_maintenance_boot_grace_until_ms) {
    if (reason_out) *reason_out = "maint_boot_grace";
    return false;
  }
  if (ota_locked) {
    if (reason_out) *reason_out = "ota_locked";
    return false;
  }
  if (ota_check_requested || ota_check_pending) {
    if (reason_out) *reason_out = "ota_check";
    return false;
  }
  if (now_ms < ota_stay_awake_until_ms) {
    if (reason_out) *reason_out = "ota_stay_awake";
    return false;
  }
  if (now_ms < stay_awake_until_ms) {
    if (reason_out) *reason_out = "stay_awake";
    return false;
  }
  if (is_glowing_animation) {
    if (reason_out) *reason_out = "processing";
    return false;
  }
  if (wifi_phase == WIFI_PHASE_CONNECTING || wifi_on_pending || lcd_wifi_connecting()) {
    if (reason_out) *reason_out = "wifi_connecting";
    return false;
  }

  unsigned long timeout_ms = INACTIVITY_TIMEOUT_MS;
  if (waiting_for_scan_response) {
    timeout_ms = SCAN_RESPONSE_TIMEOUT_MS;
  } else if (waiting_for_voice_response) {
    timeout_ms = VOICE_RESPONSE_TIMEOUT_MS;
  } else if (waiting_for_list_response) {
    timeout_ms = API_RESPONSE_TIMEOUT_MS;
  }
  unsigned long last_activity_ms = last_user_activity_ms;
  if (last_scroll_activity_ms > last_activity_ms) {
    last_activity_ms = last_scroll_activity_ms;
  }
  unsigned long idle_age_ms = last_activity_ms > 0 ? (now_ms - last_activity_ms) : 0;
  unsigned long effective_timeout_ms = timeout_ms;
  if (USER_WAKE_HOLD_MS > effective_timeout_ms) {
    effective_timeout_ms = USER_WAKE_HOLD_MS;
  }
  if (idle_age_ms < effective_timeout_ms) {
    if (reason_out) *reason_out = "user_active";
    return false;
  }
  if (!ui_is_sleep_eligible_menu_screen(ui_screen_state)) {
    if (reason_out) *reason_out = "not_home";
    return false;
  }
  if (home_shown_ms == 0) {
    home_shown_ms = now_ms;
  }
  unsigned long home_age_ms = (home_shown_ms > 0) ? (now_ms - home_shown_ms) : 0;
  if (home_age_ms < HOME_SLEEP_DELAY_MS) {
    if (reason_out) *reason_out = "home_delay";
    return false;
  }
  if (ship_mode_is_dish(g_ship_ui_op, g_ship_ui_mode)) {
    const char* phase = g_ship_ui_phase;
    if (phase &&
        (strcmp(phase, "UPLOAD_STARTING") == 0 ||
         strcmp(phase, "UPLOADING") == 0 ||
         strcmp(phase, "RESULT_WAITING") == 0 ||
         strcmp(phase, "PROCESSING") == 0)) {
      if (reason_out) *reason_out = "dish_processing";
      return false;
    }
  }
  return true;
}

static void resetActivityTimer() {
  last_user_activity_ms = millis();
  if (ui_screen_state == SCREEN_HOME) {
    home_shown_ms = last_user_activity_ms;
  }
  last_sleep_skip_log_ms = 0;
  last_sleep_skip_reason[0] = '\0';
}

static const char* ui_screen_state_name(ui_screen_t state) {
  switch (state) {
    case SCREEN_HOME: return "HOME";
    case SCREEN_SECOND: return "SECOND";
    case SCREEN_SETTINGS: return "SETTINGS";
    case SCREEN_AI_LISTENING: return "AI_LISTENING";
    case SCREEN_VOICE_JSON: return "VOICE_JSON";
    case SCREEN_HOLD_STILL: return "HOLD_STILL";
    case SCREEN_VOICE_ACK: return "VOICE_ACK";
    case SCREEN_PROCESSING: return "PROCESSING";
    case SCREEN_LOGGED: return "LOGGED";
    case SCREEN_EXPIRY_CHOICE: return "EXPIRY_CHOICE";
    case SCREEN_EXPIRY: return "EXPIRY";
    case SCREEN_RESULT: return "RESULT";
    case SCREEN_DEBUG: return "DEBUG";
    default: return "UNKNOWN";
  }
}

static bool ui_is_sleep_eligible_menu_screen(ui_screen_t state) {
  return state == SCREEN_HOME ||
         state == SCREEN_SECOND ||
         state == SCREEN_SETTINGS;
}

static ship_user_state_t ship_user_state_current() {
  if (g_in_light_sleep || g_sleep_transition) {
    return SHIP_USER_STATE_ASLEEP;
  }
  if (waiting_for_scan_response) {
    if (strcmp(g_ship_ui_phase, "WAITING_INPUT") == 0) {
      return SHIP_USER_STATE_USER_INPUT_REQUIRED;
    }
    if (strcmp(g_ship_ui_phase, "UPLOADING") == 0 ||
        strcmp(g_ship_ui_phase, "UPLOAD_STARTING") == 0 ||
        strcmp(g_ship_ui_phase, "RESULT_WAITING") == 0 ||
        strcmp(g_ship_ui_phase, "PROCESSING") == 0) {
      return SHIP_USER_STATE_USER_WAITING_RESULT;
    }
    return SHIP_USER_STATE_CAPTURE_COMMITTED;
  }
  if (ui_screen_state == SCREEN_VOICE_ACK || ui_screen_state == SCREEN_LOGGED || ui_screen_state == SCREEN_RESULT) {
    return SHIP_USER_STATE_USER_FEEDBACK;
  }
  return SHIP_USER_STATE_MENU_READY;
}

static const char* ship_user_state_name(ship_user_state_t state) {
  switch (state) {
    case SHIP_USER_STATE_ASLEEP: return "ASLEEP";
    case SHIP_USER_STATE_MENU_READY: return "MENU_READY";
    case SHIP_USER_STATE_CAPTURE_COMMITTED: return "CAPTURE_COMMITTED";
    case SHIP_USER_STATE_USER_WAITING_RESULT: return "USER_WAITING_RESULT";
    case SHIP_USER_STATE_USER_INPUT_REQUIRED: return "USER_INPUT_REQUIRED";
    case SHIP_USER_STATE_USER_FEEDBACK: return "USER_FEEDBACK";
    default: return "UNKNOWN";
  }
}

static const char* ui_screen_from_ptr(lv_obj_t* scr) {
  if (scr == ship_menu_screen) return "MAIN_MENU";
  if (scr == ship_menu_second_screen) return "SECOND_MENU";
  if (scr == ship_menu_settings_screen) return "SETTINGS_MENU";
  if (scr == ship_ai_listening_screen) return "AI_LISTENING";
  if (scr == ship_voice_json_screen) return "VOICE_JSON";
  if (scr == ship_hold_screen) return "HOLD_STILL";
  if (scr == ship_voice_ack_screen) return "VOICE_ACK";
  if (scr == ship_processing_screen) return "PROCESSING";
  if (scr == ship_logged_screen) return "LOGGED";
  if (scr == ship_error_screen) return "ERROR";
  if (scr == ship_expiry_choice_screen) return "EXPIRY_CHOICE";
  if (scr == g_base_screen) return "BASE";
  return "UNKNOWN";
}

static void log_active_screen(const char* reason) {
  lv_obj_t* active = lv_scr_act();
  Serial.printf("[UI_ACTIVE] reason=%s active=%s ptr=%p ui_state=%s user_state=%s\n",
                reason ? reason : "",
                ui_screen_from_ptr(active),
                (void*)active,
                ui_screen_state_name(ui_screen_state),
                ship_user_state_name(ship_user_state_current()));
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

static void ui_reset_lvgl_objects() {
  display_busy_overlay = NULL;
  list_container = NULL;
  for (int i = 0; i < MAX_LIST_ITEMS; i++) {
    list_labels[i] = NULL;
  }
  empty_label = NULL;
  loading_screen = NULL;
  g_base_screen = NULL;
  ship_menu_screen = NULL;
  ship_menu_label = NULL;
  for (int i = 0; i < 4; ++i) {
    ship_main_menu_buttons[i] = NULL;
  }
  ship_main_menu_ai_button = NULL;
  ship_main_menu_ai_label = NULL;
  ship_menu_second_screen = NULL;
  ship_menu_settings_screen = NULL;
  ship_menu_settings_title = NULL;
  ship_menu_settings_btn_reset = NULL;
  ship_menu_settings_label_reset = NULL;
  ship_menu_settings_btn_ota = NULL;
  ship_menu_settings_label_ota = NULL;
  ship_menu_settings_btn_back = NULL;
  ship_menu_settings_label_back = NULL;
  ship_menu_settings_status = NULL;
  ship_menu_settings_status_hide_at_ms = 0;
  ship_hold_screen = NULL;
  ship_voice_ack_screen = NULL;
  ship_voice_ack_icon = NULL;
  ship_voice_ack_label = NULL;
  ship_voice_ack_subtitle = NULL;
  ship_processing_screen = NULL;
  ship_ai_listening_screen = NULL;
  ship_ai_listening_ring = NULL;
  ship_ai_listening_title = NULL;
  ship_ai_listening_hint = NULL;
  ship_ai_listening_mic_head = NULL;
  ship_ai_listening_mic_stem = NULL;
  ship_ai_listening_mic_base = NULL;
  ship_voice_json_screen = NULL;
  ship_voice_json_card = NULL;
  ship_voice_json_title = NULL;
  ship_voice_json_subtitle = NULL;
  ship_voice_json_label = NULL;
  ship_voice_json_footer = NULL;
  ship_voice_json_page_label = NULL;
  ship_voice_json_hint = NULL;
  ship_processing_fill = NULL;
  ship_processing_halo = NULL;
  ship_processing_spinner = NULL;
  ship_processing_subtitle = NULL;
  ship_processing_label = NULL;
  ship_hold_title = NULL;
  ship_hold_subtitle = NULL;
  ship_hold_ring = NULL;
  ship_hold_countdown_label = NULL;
  ship_hold_capture_icon = NULL;
  ship_logged_screen = NULL;
  ship_logged_icon = NULL;
  ship_logged_label = NULL;
  ship_logged_subtitle = NULL;
  ship_error_screen = NULL;
  ship_error_label = NULL;
  ship_hold_anim_start_ms = 0;
  ship_hold_countdown_value = -1;
  ship_hold_capture_frame = -1;
  ship_hold_capture_phase = false;
  ship_expiry_choice_shown_time = 0;
  ship_menu_press_overlay = NULL;
  g_status_layer = NULL;
  g_status_label = NULL;
  g_status_spinner = NULL;
  ship_ai_touch_active = false;
  g_ship_voice_json_text[0] = '\0';
  voice_response_deadline_ms = 0;
  memset(ship_voice_json_item_labels, 0, sizeof(ship_voice_json_item_labels));
  memset(ship_voice_json_page_dots, 0, sizeof(ship_voice_json_page_dots));
  ship_voice_ui_reset();
  result_root = NULL;
  result_icon_label = NULL;
  result_title_label = NULL;
  result_subtitle_label = NULL;
  result_btn_home = NULL;
  result_btn_home_label = NULL;
  result_btn_retry = NULL;
  result_btn_retry_label = NULL;
  debug_screen = NULL;
  debug_title = NULL;
  debug_label_status = NULL;
  debug_label_status2 = NULL;
  debug_label_sense = NULL;
  debug_label_hb = NULL;
  debug_label_wifi = NULL;
  debug_label_ui = NULL;
  debug_btn_back = NULL;
  debug_btn_back_label = NULL;
  meal_result_screen = NULL;
  expiry_choice_quantity_label = NULL;
  ship_expiry_choice_qty_prefix = NULL;
  ship_expiry_choice_prompt = NULL;
  ship_expiry_choice_timeout_ring = NULL;
  ship_expiry_choice_add_caption = NULL;
  ship_expiry_choice_skip_btn = NULL;
  ship_expiry_choice_skip_label = NULL;
  ship_expiry_choice_add_btn = NULL;
  ship_expiry_choice_add_label = NULL;
  meal_calories_label = NULL;
  meal_description_label = NULL;
  meal_protein_value_label = NULL;
  meal_protein_name_label = NULL;
  meal_carbs_value_label = NULL;
  meal_carbs_name_label = NULL;
  meal_fat_value_label = NULL;
  meal_fat_name_label = NULL;
  meal_recommendation_label = NULL;
  delete_menu = NULL;
  delete_item_btn = NULL;
  delete_item_label = NULL;
  menu_menu = NULL;
  menu_item_btn = NULL;
  menu_item_label = NULL;
  menu_screen = NULL;
  menu_list_container = NULL;
  for (int i = 0; i < MENU_MAX_ITEMS; i++) {
    menu_item_labels[i] = NULL;
  }
  logged_screen = NULL;
  logged_label = NULL;
  expiry_screen = NULL;
  expiry_date_label = NULL;
  expiry_month_button = NULL;
  expiry_month_label = NULL;
  expiry_day_button = NULL;
  expiry_day_label = NULL;
  expiry_year_button = NULL;
  expiry_year_label = NULL;
  expiry_hint_label = NULL;
  for (int i = 0; i < 10; i++) {
    expiry_keypad_buttons[i] = NULL;
  }
  expiry_check_button = NULL;
  expiry_backspace_button = NULL;
  expiry_back_button = NULL;
  expiry_back_label = NULL;
  expiry_timeout_ring = NULL;
  recording_indicator = NULL;
  processing_indicator = NULL;
  status_screen = NULL;
  status_label = NULL;
  status_reset_button = NULL;
  status_reset_label = NULL;
  provision_screen = NULL;
  provision_qr = NULL;
  provision_title_label = NULL;
  provision_ssid_label = NULL;
  provision_url_label = NULL;
  provision_status_label = NULL;
  provision_intro_screen = NULL;

  menu_screen_visible = false;
  buttons_visible = false;
  status_reset_visible = false;
  touch_used_to_dismiss_meal = false;
  meal_result_shown_time = 0;
  status_screen_shown_time = 0;
  logged_screen_shown_time = 0;
  delete_cooldown_until = 0;
  menu_cooldown_until = 0;
  provision_qr_waiting = false;
  provision_qr_exit_headless = false;
  provision_qr_wait_start_ms = 0;
  provision_screen_visible = false;
  provision_intro_visible = false;
  provision_intro_pending = false;
  provision_intro_tapped = false;
  provision_qr_cached = false;
  provision_user_requested = false;
  provision_return_home_pending = false;
  provision_qr_ssid[0] = '\0';
  provision_qr_password[0] = '\0';
  provision_qr_url[0] = '\0';
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
  lcd_set_backlight_binary(true, "init_ui");
  g_panel_enabled = true;
  
  // Rotate display 180 degrees
  lv_disp_t *disp = lv_disp_get_default();
  lv_disp_set_rotation(disp, LV_DISP_ROT_180);
  
  // Create UI
  Serial.println("Creating UI...");
  create_custom_ui();
  
  // Ship menu owns the first visible screen. Do not pre-render the shopping list at boot.

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
  if (g_lcd_initialized) {
    lcd_panel_set_power(false);
  }
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
    abort_sleep_transition("ship_ota_ext0_active");
    delay(250);
    return;
  }
  if (!input_wake_sources_idle("ship_ota_pre_sleep")) {
    Serial.println("[SLEEP_SANITY] input wake sources still active; refusing_sleep");
    abort_sleep_transition("ship_ota_ext1_active");
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
  lcd_set_backlight_binary(false, "ship_ota_sleep");
  Serial.printf("[LCD_SLEEP] wake_sources=%s timer_s=%lu\n",
                LCD_OTA_WAKE_INTERVAL_SEC > 0 ? "EXT0_TIMER" : "EXT0_ONLY",
                (unsigned long)LCD_OTA_WAKE_INTERVAL_SEC);
  Serial.println("=================================");
  Serial.println("[SLEEP] entering deep sleep");
  Serial.printf("[SLEEP] wake_gpio=%d\n", WAKE_GPIO);
  Serial.printf("[SLEEP] wake_level=%d\n", HALO_WAKE_LEVEL);
  Serial.println("=================================");
  esp_deep_sleep_start();
}

static void enter_maintenance_sleep() {
  g_sleep_transition = true;
  if (g_lcd_initialized) {
    lcd_panel_set_power(false);
  }
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
    abort_sleep_transition("maint_ext0_active");
    delay(250);
    return;
  }
  if (!input_wake_sources_idle("maintenance_pre_sleep")) {
    Serial.println("[SLEEP_SANITY] input wake sources still active; refusing_sleep");
    abort_sleep_transition("maint_ext1_active");
    delay(250);
    return;
  }
  uint32_t timer_s = (g_lcd_maintenance_timer_armed && g_lcd_maintenance_wake_in_s > 0)
                         ? g_lcd_maintenance_wake_in_s
                         : 0;
  Serial.printf("[LCD_MAINT] sleep_entry timer_armed=%d wake_in_s=%lu remaining_s=%lu deadline_ms=%lu\n",
                g_lcd_maintenance_timer_armed ? 1 : 0,
                (unsigned long)g_lcd_maintenance_wake_in_s,
                (unsigned long)g_lcd_maintenance_remaining_s,
                (unsigned long)g_lcd_maintenance_deadline_ms);
  configure_sleep_sources(true, timer_s);

  Serial.printf("[SLEEP_STATE] entering_deep_sleep now_ms=%lu ext0_gpio=%d ext0_level=%d\n",
                (unsigned long)millis(),
                (int)LCD_WAKE_GPIO,
                (int)LCD_WAKE_LEVEL);
  Serial.println("[SLEEP] entering_deep_sleep");
  sleep_entry_time = millis();
  lcd_set_backlight_binary(false, "maintenance_sleep");
  Serial.printf("[LCD_SLEEP] wake_sources=%s timer_s=%lu\n",
                timer_s > 0 ? "EXT0_TIMER" : "EXT0_ONLY",
                (unsigned long)timer_s);
  Serial.println("=================================");
  Serial.println("[SLEEP] entering deep sleep");
  Serial.printf("[SLEEP] wake_gpio=%d\n", WAKE_GPIO);
  Serial.printf("[SLEEP] wake_level=%d\n", HALO_WAKE_LEVEL);
  Serial.println("=================================");
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

static const char* reset_reason_label(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "ESP_RST_POWERON";
    case ESP_RST_EXT:
      return "ESP_RST_EXT";
    case ESP_RST_SW:
      return "ESP_RST_SW";
    case ESP_RST_PANIC:
      return "ESP_RST_PANIC";
    case ESP_RST_INT_WDT:
      return "ESP_RST_INT_WDT";
    case ESP_RST_TASK_WDT:
      return "ESP_RST_TASK_WDT";
    case ESP_RST_WDT:
      return "ESP_RST_WDT";
    case ESP_RST_DEEPSLEEP:
      return "ESP_RST_DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "ESP_RST_BROWNOUT";
    case ESP_RST_SDIO:
      return "ESP_RST_SDIO";
    case ESP_RST_UNKNOWN:
    default:
      return "ESP_RST_UNKNOWN";
  }
}

static const char* wake_cause_label(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "UNDEFINED";
    default:
      return "OTHER";
  }
}

static void print_wakeup_diagnostics(const char* board_name) {
  Serial.begin(115200);
  delay(10);
  esp_reset_reason_t reset_reason = esp_reset_reason();
  esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  int wake_pin_level = gpio_get_level((gpio_num_t)HALO_WAKE_GPIO);
  Serial.printf("[BOOT] board=%s\n", board_name ? board_name : "unknown");
  Serial.printf("[BOOT] reset_reason=%s\n", reset_reason_label(reset_reason));
  Serial.printf("[BOOT] wake_cause=%s\n", wake_cause_label(wake_cause));
  Serial.printf("[BOOT] wake_pin_level=%d\n", wake_pin_level);
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    log_ext1_wakeup_status("boot");
  }
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
  esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  esp_reset_reason_t reset_reason = esp_reset_reason();
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
  
  // Initialize INT pin for waking Sense
  lcd_wake_pin_set_mode(INT_PIN, OUTPUT);
  gpio_set_drive_capability((gpio_num_t)INT_PIN, GPIO_DRIVE_CAP_3);
  digitalWrite(INT_PIN, HIGH);
  delay(2);
  lcd_wake_pin_set_mode(INT_PIN, INPUT_PULLUP);

  // On deep-sleep boot wakes, setup() is the active wake path. Start the
  // Sense wake handshake here so the Sense can boot alongside the LCD.
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT0 || wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    const char* boot_wake_reason =
        (wake_cause == ESP_SLEEP_WAKEUP_EXT0) ? "boot_ext0_wake" : "boot_ext1_wake";
    Serial.printf("[WAKE] Requesting Sense wake in setup reason=%s\n", boot_wake_reason);
    request_sense_wake(boot_wake_reason);
    sense_state_set(SENSE_UNKNOWN, boot_wake_reason);
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

  if (!g_ship_ota_wake_window && !g_lcd_maintenance_wake_window) {
    init_ui_stack(g_saved_list_count);
  } else {
    init_touch_once();
    init_knob_once();
  }
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT0 || wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    clear_input_wake_sources("boot_wake");
    touch_ignore_until = millis() + 300;
    scroll_ignore_until = millis() + 150;
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
  bool lvgl_locked = false;
  if (g_ui_initialized) {
    if (!example_lvgl_lock(50)) {
      vTaskDelay(pdMS_TO_TICKS(10));
      return;
    }
    lvgl_locked = true;
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
      lcd_clear_maintenance_state(exit_reason, true);
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
      skip_main_loop = true;
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
      ship_voice_end_resends_remaining = 0;
      ship_voice_end_resend_due_ms = 0;
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
    lcd_maybe_pulse_sense_int("status_sync");
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

  // If Sense didn't acknowledge wake, retry wake pulses for a short window
  if (!g_in_light_sleep && !sense_awake_confirmed && wake_retry_until_ms > 0 &&
      refresh_state != REFRESH_INFLIGHT) {
    unsigned long now = millis();
    if (now > wake_retry_until_ms) {
      wake_retry_until_ms = 0;
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
      if (!guardian_sleep_triggered) {
        guardian_sleep_triggered = true;
        Serial.printf("[GUARDIAN] force_sleep elapsed_ms=%lu\n", awake_ms);
      }
      enterLightSleep();
      goto loop_continue;
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
      if (g_lcd_maintenance_deadline_ms == 0 || millis() < g_lcd_maintenance_deadline_ms) {
        inhibit_reason = "maintenance_active";
      }
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
      static unsigned long last_ota_log_ms = 0;
      if (millis() - last_ota_log_ms > 5000) {
        Serial.println("[OTA] stay_awake window active - deferring sleep");
        last_ota_log_ms = millis();
      }
      log_sleep_decision(now_ms, screen_name, home_age_ms, false, "ota_stay_awake");
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
    example_lvgl_unlock();
  }

#ifdef HALO_LCD_PROD_WRAPPER
  halo_lcd_prod_loop();
#endif

  vTaskDelay(pdMS_TO_TICKS(50));  // Poll touch every 50ms (use vTaskDelay to yield to IDLE task)
}


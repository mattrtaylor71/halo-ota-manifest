/*
 * lcd_anim.h
 *
 * Processing animation functions: glowing halo animation, LVGL tick,
 * ship UI screen builders (hold-still, processing progress, AI
 * listening, voice UI, result screens), and status screen helpers.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 9.
 *
 * Prerequisites (must be declared before #include "lcd_anim.h"):
 *   - All LVGL UI object globals (processing_indicator, etc.)
 *   - All animation state globals (is_glowing_animation, etc.)
 *   - All ship UI state globals (ship_hold_*, ship_ai_*, etc.)
 *   - lcd_uart.h (uart_send_input_message)
 */

#ifndef LCD_ANIM_H
#define LCD_ANIM_H

extern "C" void lcd_stop_lvgl_tick_timer(void);

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
  // Request the UI task to exit cleanly (before we deinit LVGL).
  // The task checks g_ui_task_exit_requested before acquiring the LVGL lock,
  // so it will exit without holding any locks.
  if (ui_task_handle != NULL) {
    g_ui_task_exit_requested = true;
    unsigned long wait_start = millis();
    while (ui_task_handle != NULL && (millis() - wait_start) < 500) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ui_task_handle != NULL) {
      Serial.println("[LCD_OTA] WARN: UI task did not exit cleanly, force deleting");
      vTaskDelete(ui_task_handle);
      ui_task_handle = NULL;
    }
    g_ui_task_exit_requested = false;
  }
  lcd_lvgl_wait_tx_done(200);
  if (lv_is_initialized()) {
    lv_obj_clean(lv_scr_act());
    lv_deinit();
  }
  lcd_stop_lvgl_tick_timer();
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
    g_ui_task_exit_requested = true;
    unsigned long wait_start = millis();
    while (ui_task_handle != NULL && (millis() - wait_start) < 500) {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ui_task_handle != NULL) {
      Serial.println("[LCD_EXIT_OTA] WARN: UI task did not exit cleanly, force deleting");
      vTaskDelete(ui_task_handle);
      ui_task_handle = NULL;
    }
    g_ui_task_exit_requested = false;
  }
  g_ui_initialized = false;
  g_lvgl_running = false;
  g_panel_enabled = false;
  lcd_set_backlight_binary(true, reason ? reason : "ota_exit");
  ui_reset_lvgl_objects();
  Serial.printf("[LCD_EXIT_OTA] pre_init_ui_stack internal_free=%u largest=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  // Check if there's enough contiguous DMA memory for normal LVGL flush
  // Normal flush size: 360 x 36 x 2 = 25,920 bytes + overhead
  {
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t normal_flush = EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t);
    Serial.printf("[LCD_EXIT_OTA] dma_largest=%u normal_flush=%u\n",
                  (unsigned)dma_largest, (unsigned)normal_flush);
    if (dma_largest < normal_flush) {
      g_post_ota_recovery = true;
      Serial.println("[LCD_EXIT_OTA] recovery_mode=1 (reduced LVGL buffers)");
    }
  }
  init_ui_stack(g_saved_list_count);
  Serial.printf("[LCD_EXIT_OTA] post_init_ui_stack internal_free=%u largest=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
  {NULL, NULL},  // All PNG frames removed — using programmatic rendering
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
  lv_obj_set_style_bg_color(status_screen, lv_color_hex(0xF5E9D8), LV_PART_MAIN);
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

#endif // LCD_ANIM_H

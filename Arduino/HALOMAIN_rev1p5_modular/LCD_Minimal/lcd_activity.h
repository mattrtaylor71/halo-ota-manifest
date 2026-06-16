/*
 * lcd_activity.h
 *
 * Activity timer, guardian force-sleep, sleep coordination
 * logic, OTA/maintenance sleep entry, and loop()-level sleep
 * decision making.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 6.
 *
 * Prerequisites (must be declared before #include "lcd_activity.h"):
 *   - All sleep/wake state globals, lcd_sleep.h
 *   - All sense link management, UI state, OTA state functions
 *   - enterLightSleep (from lcd_sleep.h)
 */

#ifndef LCD_ACTIVITY_H
#define LCD_ACTIVITY_H

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
  // Test mode: block all sleep for automated testing
  if (g_test_mode_active) {
    if (millis() < g_test_mode_expire_ms) {
      if (reason_out) *reason_out = "test_mode";
      return false;
    } else {
      // Auto-expire
      g_test_mode_active = false;
      Preferences prefs;
      if (prefs.begin("test_cfg", false)) {
        prefs.remove("test_mode");
        prefs.end();
      }
      Serial.println("[TEST_MODE] auto-expired (1 hour elapsed)");
    }
  }
  if (wake_timer_wait_mode) {
    if (reason_out) *reason_out = "timer_wait";
    return false;
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
    // If Sense went to sleep without sending OTA_LOCK, the OTA request was missed
    // (race: LCD sent INPUT_OTA_CHECK after Sense already started sleep sequence).
    // Clear the stay_awake timer so LCD can sleep too.
    if (sense_state == SENSE_ASLEEP && !ota_locked &&
        now_ms >= g_ota_lock_window_until_ms) {
      Serial.println("[OTA] stay_awake cancelled (sense asleep, no ota_lock)");
      ota_stay_awake_until_ms = 0;
      ota_check_requested = false;
      // fall through — allow sleep
    } else if (sense_state == SENSE_ASLEEP && !ota_locked) {
      // Fresh OTA_LOCK window still live: the Sense is mid self-OTA reboot and
      // will proxy the LCD afterward. Keep the LCD awake + UART-reachable.
      Serial.println("[OTA] keep stay_awake (ota_lock window active, sense rebooting)");
      if (reason_out) *reason_out = "ota_stay_awake";
      return false;
    } else {
      if (reason_out) *reason_out = "ota_stay_awake";
      return false;
    }
  }
  if (now_ms < stay_awake_until_ms) {
    if (reason_out) *reason_out = "stay_awake";
    return false;
  }
  if (is_glowing_animation) {
    if (reason_out) *reason_out = "processing";
    return false;
  }
  // Keep the device awake while a shopping-list refresh is in flight so it
  // can complete (otherwise the 10s idle-sleep fires ~10s into a fetch that
  // takes longer on slow WiFi). Capped at REFRESH_KEEPAWAKE_MAX_MS from the
  // refresh start so a wedged refresh can't pin the device awake forever.
  if (refresh_state == REFRESH_WAKE_PENDING || refresh_state == REFRESH_INFLIGHT) {
    unsigned long refresh_start_ms = refresh_wake_pending_start_ms != 0
                                         ? refresh_wake_pending_start_ms
                                         : lcd_refresh_start_ms;
    if (refresh_start_ms == 0 ||
        (now_ms - refresh_start_ms) <= REFRESH_KEEPAWAKE_MAX_MS) {
      if (reason_out) *reason_out = "refresh_inflight";
      return false;
    }
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
    case SCREEN_ERRLOG: return "ERRLOG";
    case SCREEN_ERRLOG_DETAIL: return "ERRLOG_DETAIL";
    case SCREEN_SHOPPING_LIST: return "SHOPPING_LIST";
    default: return "UNKNOWN";
  }
}

static bool ui_is_sleep_eligible_menu_screen(ui_screen_t state) {
  return state == SCREEN_HOME ||
         state == SCREEN_SECOND ||
         state == SCREEN_SETTINGS ||
         state == SCREEN_SHOPPING_LIST;
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
  ship_menu_settings_btn_backlight = NULL;
  ship_menu_settings_label_backlight = NULL;
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
  ship_backlight_screen = NULL;
  ship_backlight_ring = NULL;
  ship_backlight_pct_label = NULL;
  ship_ai_listening_title = NULL;
  ship_ai_listening_hint = NULL;
  ship_ai_listening_mic_head = NULL;
  ship_ai_listening_mic_stem = NULL;
  ship_ai_listening_mic_base = NULL;
  ship_ai_listening_mic_disc = NULL;
  ship_ai_listening_pulse[0] = NULL;
  ship_ai_listening_pulse[1] = NULL;
  ship_ai_listening_pulse[2] = NULL;
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
  // Configure RTC pull-up on wake GPIO so the pin doesn't float during deep sleep
  rtc_gpio_init((gpio_num_t)LCD_WAKE_GPIO);
  rtc_gpio_set_direction((gpio_num_t)LCD_WAKE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)LCD_WAKE_GPIO);
  rtc_gpio_pulldown_dis((gpio_num_t)LCD_WAKE_GPIO);
  Serial.printf("[SLEEP_GPIO] gpio=%d rtc_pullup=1 pulldown=0 level_now=%d\n",
                (int)LCD_WAKE_GPIO, digitalRead(LCD_WAKE_GPIO));

  configure_sleep_sources(true, LCD_OTA_WAKE_INTERVAL_SEC);

  Serial.printf("[SLEEP_STATE] entering_deep_sleep now_ms=%lu ext0_gpio=%d ext0_level=%d\n",
                (unsigned long)millis(),
                (int)LCD_WAKE_GPIO,
                (int)LCD_WAKE_LEVEL);
  Serial.println("[SLEEP] entering_deep_sleep");
  sleep_entry_time = millis();

  // Put touch IC into standby mode so it generates INT on touch during deep sleep
  if (g_touch_initialized) {
    Touch_Standby();
    Serial.println("[TOUCH] standby mode set for deep sleep wake");
  }

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
  // Configure RTC pull-up on wake GPIO so the pin doesn't float during deep sleep
  rtc_gpio_init((gpio_num_t)LCD_WAKE_GPIO);
  rtc_gpio_set_direction((gpio_num_t)LCD_WAKE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)LCD_WAKE_GPIO);
  rtc_gpio_pulldown_dis((gpio_num_t)LCD_WAKE_GPIO);
  Serial.printf("[SLEEP_GPIO] gpio=%d rtc_pullup=1 pulldown=0 level_now=%d\n",
                (int)LCD_WAKE_GPIO, digitalRead(LCD_WAKE_GPIO));

  configure_sleep_sources(true, timer_s);

  Serial.printf("[SLEEP_STATE] entering_deep_sleep now_ms=%lu ext0_gpio=%d ext0_level=%d\n",
                (unsigned long)millis(),
                (int)LCD_WAKE_GPIO,
                (int)LCD_WAKE_LEVEL);
  Serial.println("[SLEEP] entering_deep_sleep");
  sleep_entry_time = millis();

  // Put touch IC into standby mode so it generates INT on touch during deep sleep
  if (g_touch_initialized) {
    Touch_Standby();
    Serial.println("[TOUCH] standby mode set for deep sleep wake");
  }

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


#endif // LCD_ACTIVITY_H

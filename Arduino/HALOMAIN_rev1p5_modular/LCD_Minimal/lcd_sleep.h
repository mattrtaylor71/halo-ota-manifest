/*
 * lcd_sleep.h
 *
 * Sleep/wake management: deep-sleep entry (enterLightSleep),
 * Sense sleep handshake (notify_sense_sleep), wake mask
 * configuration, sleep protocol TX helpers, and sleep state
 * machine coordination.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 5.
 *
 * Prerequisites (must be declared before #include "lcd_sleep.h"):
 *   - All sleep/wake state globals (sleep_ready_received, etc.)
 *   - All sense link management functions (request_sense_wake, etc.)
 *   - lcd_uart.h (uart_send_json, uart_send_sleep_deny, etc.)
 *   - LVGL, FreeRTOS, ESP-IDF sleep APIs
 */

#ifndef LCD_SLEEP_H
#define LCD_SLEEP_H

static uint64_t buildWakeMaskForSleep() {
  uint64_t wakeMask = 0;

  // Touch-only wake: encoder A/B excluded from EXT1 mask because
  // input_wake_sources_idle() only gates on touch, not encoder pins.
  // If enc_b happens to rest LOW, EXT1 ANY_LOW triggers instant wake.
  // TODO: add RTC pull-ups + idle gate for encoder before re-enabling.
  wakeMask |= (1ULL << LCD_WAKE_GPIO);   // GPIO9 - touch INT only
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

  bool force_sleep = false;
  if (sleep_deny_count >= SLEEP_DENY_MAX_COUNT) {
    Serial.printf("[SLEEP] deny_max_exceeded count=%u - forcing sleep\n", sleep_deny_count);
    sleep_deny_count = 0;
    sleep_deny_active = false;
    force_sleep = true;
  }

  Serial.println("========================================");
  Serial.println("Preparing for DEEP SLEEP...");
  Serial.println("========================================");
  uint16_t dummy_x = 0, dummy_y = 0;
  if (!force_sleep && !notify_sense_sleep()) {
    if (sleep_cancelled_by_user_input) {
      Serial.println("[SLEEP] user_input cancelled pre_sleep");
      resetActivityTimer();
      return;
    }
    if (sleep_deny_active) {
      sleep_deny_count++;
      if (millis() - last_sleep_retry_log_ms > 1000) {
        Serial.printf("[SLEEP] deny_wait reason=%s retry_ms=%lu count=%u/%u\n",
                      sleep_deny_reason[0] ? sleep_deny_reason : "unknown",
                      sleep_deny_retry_ms > 0 ? sleep_deny_retry_ms : (unsigned long)SLEEP_DENY_RETRY_DEFAULT_MS,
                      sleep_deny_count,
                      SLEEP_DENY_MAX_COUNT);
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
  sleep_deny_count = 0;
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
  // GPIO9 (touch INT) is open-drain from CST816 - never drive as OUTPUT
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
  // Configure RTC pull-up on wake GPIO so the pin doesn't float during deep sleep.
  // Digital pull-ups are disabled when the digital GPIO controller powers off.
  rtc_gpio_init((gpio_num_t)LCD_WAKE_GPIO);
  rtc_gpio_set_direction((gpio_num_t)LCD_WAKE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)LCD_WAKE_GPIO);
  rtc_gpio_pulldown_dis((gpio_num_t)LCD_WAKE_GPIO);
  Serial.printf("[SLEEP_GPIO] gpio=%d rtc_pullup=1 pulldown=0 level_now=%d\n",
                (int)LCD_WAKE_GPIO, digitalRead(LCD_WAKE_GPIO));

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

  // Put touch IC into standby mode so it generates INT on touch during deep sleep
  if (g_touch_initialized) {
    Touch_Standby();
    Serial.println("[TOUCH] standby mode set for deep sleep wake");
  }

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

    // Hold GPIO39 (INT_PIN) HIGH through deep sleep.
    // GPIO39 is NOT an RTC GPIO on ESP32-S3, so without hold, the output
    // driver turns off when the digital domain powers down, leaving it
    // floating. Floating GPIO39 → Sense GPIO2 may read LOW → disables EXT0.
    gpio_set_direction((gpio_num_t)INT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)INT_PIN, 1);
    gpio_hold_en((gpio_num_t)INT_PIN);
    gpio_deep_sleep_hold_en();

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
  // Don't call release_wake_line here — it toggles OUTPUT→INPUT which
  // Sense detects as "lcd_pulsing" and denies sleep. Just ensure the
  // pin is in INPUT_PULLUP (stable HIGH, no toggle).
  if (digitalRead(INT_PIN) == LCD_WAKE_LEVEL) {
    // Pin is at wake level — set to INPUT_PULLUP to let it float HIGH
    lcd_wake_pin_set_mode(INT_PIN, INPUT_PULLUP);
    delay(5);
  }
  Serial.printf("[SLEEP] wake_line_check level=%d\n", digitalRead(INT_PIN));
  unsigned long start_ms = millis();
  while ((millis() - start_ms) < 40UL) {
    if (digitalRead(INT_PIN) != LCD_WAKE_LEVEL) {
      Serial.printf("[SLEEP] wake_line_ready level=%d elapsed_ms=%lu\n",
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
        // once the panel is back on. Don't send INPUT_WAKE — it resets the
        // Sense cooldown timer and adds 10s penalty. Just abort locally and
        // let the Sense INPUT_SLEEP timeout naturally.
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
        Serial.println("[SLEEP] abort wait (user_input, no INPUT_WAKE sent)");
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


#endif // LCD_SLEEP_H

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
#if defined(HALO_OTA_BENCH_CASE)
#include "../halo_ota_demo/firmware/shared/HaloOtaBenchFaults.h"
#endif

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

// ── Touch during the sleep teardown ──────────────────────────────────
//
// Sleep entry is not instantaneous. Between `transition_begin` and
// esp_deep_sleep_start() the LCD saves the list to NVS, tears down the UI,
// powers the panel off and writes an errlog entry. MEASURED on hardware:
// `[SLEEP_WATCH] teardown_ms=166`. A tap landing inside those 166ms is seen by
// nobody — the UI task has stopped treating touches as input and ext1 is not
// armed until the last instruction — so it is silently lost and the user taps
// again.
//
// A held touch is NOT affected: the INT stays asserted, so ext1 ANY_LOW fires
// the moment sleep starts. Only a tap that both starts AND ends inside the
// window is lost.
//
// Polling cannot close this — the tap is a transient pulse and the teardown is
// busy doing I2C, NVS and serial work. An edge-triggered ISR can: it fires
// regardless of what the main flow is doing, and the flag is checked at the last
// point where sleep can still be abandoned CLEANLY, i.e. above Touch_Standby(),
// the backlight going off and vTaskDelete(ui_task_handle) — abort_sleep_transition()
// restores the panel, backlight and LVGL flag, but it cannot recreate a deleted
// UI task.
//
// HISTORY: this was built and reverted on 08-19 because the defect it was
// written for (a "~4s dead zone") turned out to be a measurement artifact. The
// 166ms window is real and measured; this reinstates the fix for THAT, and it is
// testable via HALO_TEARDOWN_DELAY_MS below.
static volatile bool s_touch_during_sleep = false;
static volatile uint32_t s_touch_isr_count = 0;
static uint32_t s_sleep_transition_ms = 0;

// Bench-only: widen the teardown window so a tap can actually be landed inside
// it. The actuator's stroke is ~1.7s end to end, so a 166ms target is not
// hittable — without this the fix is untestable, which is exactly why it was
// left unbuilt for a day. Production keeps the real 166ms window.
#ifndef HALO_TEARDOWN_DELAY_MS
#define HALO_TEARDOWN_DELAY_MS 0
#endif

static void IRAM_ATTR lcd_sleep_touch_isr() {
  s_touch_during_sleep = true;
  s_touch_isr_count++;
}

// GPIO9 is open-drain from the CST816 and idles high (lcd_touch_wake_active()
// is `digitalRead(PIN_TOUCH_INT) == 0`), so a touch is a FALLING edge. Nothing
// else in the tree attaches an interrupt to this pin — it is otherwise only
// polled — so there is no handler to displace.
static void lcd_sleep_touch_watch_begin() {
  s_touch_during_sleep = false;
  s_touch_isr_count = 0;
  s_sleep_transition_ms = millis();
  attachInterrupt(digitalPinToInterrupt(LCD_WAKE_GPIO), lcd_sleep_touch_isr, FALLING);
}

static void lcd_sleep_touch_watch_end() {
  detachInterrupt(digitalPinToInterrupt(LCD_WAKE_GPIO));
}

static bool lcd_sleep_touch_fired() { return s_touch_during_sleep; }

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

// Active transfer custody is distinct from JSON quarantine: a closed replay
// retires its sleep custody at the original transfer deadline, while retaining
// JSON suppression until bound cleanup or reboot. Never renew that deadline.
static bool s_sleep_media_deferred = false;
static bool sleep_defer_for_media() {
  if (!g_img_rx_active && !g_img_rx_binary_mode &&
      !g_spool_tx_pending && !g_spool_tx_active) return false;
  s_sleep_media_deferred = true;
  Serial.println("[SLEEP] defer active_media_custody");
  return true;
}

// A live setup session owns the display even if SYNC was missed or cached
// sleep state is stale. Apply this at the sleep funnel, before the guardian's
// denial-count escape, and throughout the handshake. Connecting/claiming owns
// the display until its existing absolute progress deadline, even if Sense is
// busy in network code. Repeated statuses do not extend that deadline. A guide
// without active progress still uses the existing short RX-stale bound.
static bool s_sleep_provision_deferred = false;
static bool sleep_defer_for_provisioning() {
  const uint32_t now = (uint32_t)millis();
  if (!provision_flow.progress_sleep_pending(now) &&
      (!provisioning_active || !last_sense_rx_ms ||
       uint32_t(now - (uint32_t)last_sense_rx_ms) > SENSE_RX_STALE_MS)) return false;
  s_sleep_provision_deferred = true;
  return true;
}

static bool s_sleep_maintenance_deferred = false;
static bool sleep_defer_for_maintenance() {
  if (!lcd_maintenance_awake_active()) return false;
  s_sleep_maintenance_deferred = true;
  return true;
}

static void enterLightSleep() {
#if HALO_DEV_NO_SLEEP
  // Bench builds never idle-sleep, so USB-CDC stays up and reflashing is instant.
  // This has to sit HERE, not only at lcd_sleep_intent_allowed(): this function is
  // the real funnel for idle sleep (6 callers in LCD_Minimal.ino) and it does not
  // consult that gate at all. Guarding only the gate let the device sleep anyway.
  // It also has to be before the sleep_deny_count logic below, which force-sleeps
  // once denials pile up — a gate that always says "no" would trip exactly that.
  static uint32_t suppressed = 0;
  if ((suppressed++ % 20) == 0) {
    Serial.printf("[DEV] idle sleep suppressed x%lu (HALO_DEV_NO_SLEEP=1)\n",
                  (unsigned long)suppressed);
  }
  resetActivityTimer();
  return;
#endif
  s_sleep_maintenance_deferred = false;
  if (sleep_defer_for_maintenance()) return;
  if (sleep_blocked_for_ota()) {
    Serial.println("[SLEEP] blocked (ota_pending)");
    resetActivityTimer();
    return;
  }
  s_sleep_media_deferred = false;
  if (sleep_defer_for_media()) return;
  s_sleep_provision_deferred = false;
  if (sleep_defer_for_provisioning()) return;
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
    // Active ownership is not a failed handshake or Sense denial.
    if (s_sleep_media_deferred || s_sleep_provision_deferred || s_sleep_maintenance_deferred) return;
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

  // BEGIN admission uses the same storage guard and publishes its custody
  // flags before releasing it. Recheck while serialized, before any UI reset,
  // and retain this guard through the existing final sleep commit below.
  LcdMaintenanceStorageGuard sleep_arm_guard;
  if (sleep_defer_for_media()) return;
  if (sleep_defer_for_provisioning()) return;
  if (sleep_defer_for_maintenance()) return;
  if (sleep_blocked_for_ota()) { resetActivityTimer(); return; }

  // Commit before mutating UI/Sense state. A renewal between the storage
  // precheck and this lock defers sleep without masquerading as user activity.
  // Once committed, the same lock makes later background renewals decline.
  struct SleepCommitGate {
    bool acquired = false;
    SleepCommitGate() {
      LcdCoordCriticalGuard guard;
      if (lcd_maintenance_awake_active()) return;
      g_lcd_sleep_commit_gate.store(true);
      g_sleep_transition = true;
      acquired = true;
    }
    ~SleepCommitGate() {
      if (!acquired) return;
      LcdCoordCriticalGuard guard;
      g_lcd_sleep_commit_gate.store(false);
    }
  } sleep_commit_guard;
  if (!sleep_commit_guard.acquired) return;
  Serial.println("[SLEEP] transition_begin");
  lcd_sleep_touch_watch_begin();

  sleep_handshake_fail_count = 0;
  sleep_deny_count = 0;
  sleep_retry_requires_user = false;
  sleep_retry_allowed_ms = 0;
  sleep_wait_for_sense_idle = false;

  // If the LCD is idle-sleeping while still on the shopping list, tell the
  // Sense the list is no longer active so it may sleep + drop WiFi too.
  // Plain UART send (no LVGL) — safe from this Core-0 sleep path.
  if (ui_screen_state == SCREEN_SHOPPING_LIST) {
    uart_send_list_active(false);
  }

  user_activity_since_sleep = false;
  sense_awake_confirmed = false;
  sense_state_set(SENSE_ASLEEP, "enter_sleep");
  g_in_light_sleep = true;
  
  // Reset UI to clean state before sleep (so it's ready on wake)
  Serial.println("[SLEEP] Resetting UI state for clean wake...");
  
  // Reset all UI state variables to defaults
  buttons_visible = false;
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

  if (sleep_blocked_for_ota()) {
    lcd_sleep_touch_watch_end();
    abort_sleep_transition("ota_before_teardown", lcd_sleep_touch_fired());
    return;
  }
#ifdef HALO_LCD_PROD_WRAPPER
  // Wait expiry may have set the clear flag in this same sleep evaluation.
  // Service it before deep sleep can bypass the bottom-of-loop housekeeping.
  lcd_coord_service_clear();
#endif
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
    lcd_sleep_touch_watch_end();   // every path out of here releases the ISR
    abort_sleep_transition("ext0_active_pre_sleep");
    delay(250);
    return;
  }
  if (!input_wake_sources_idle("idle_pre_sleep")) {
    Serial.println("[SLEEP_SANITY] input wake sources still active; refusing_sleep");
    lcd_sleep_touch_watch_end();
    abort_sleep_transition("ext1_active_pre_sleep");
    delay(250);
    return;
  }
  g_lcd_schedule_timer_armed = 0;
  g_lcd_schedule_wake_in_s = 0;
  g_lcd_schedule_next_epoch = 0;

  // Lead before the Sense enters the window so the LCD is booted + UART-listening
  // when the Sense's LCD-first OTA proxy queries it. Matches the Sense's
  // nextWakeEpochForSleep(now,15,5) 15s lead.
  static const uint32_t LCD_MAINT_WAKE_LEAD_S = 15;

  // Absolute-window self-wake: when the clock is valid we recompute the remaining
  // time to the wake target from the absolute start_epoch on EVERY sleep, so any
  // intermediate touch/timer wake before the window is harmless (the stale
  // relative g_lcd_maintenance_wake_in_s would otherwise re-arm the full original
  // offset and miss the window). Falls back to the relative offset only when the
  // clock is invalid (preserves legacy behavior on an old Sense / no time set).
  uint32_t sleep_timer_sec;
  const char* timer_reason;
  bool maint_abs = (g_lcd_maintenance_timer_armed &&
                    lcd_time_valid() &&
                    g_lcd_maintenance_start_epoch > 0);
  uint64_t maint_now_epoch = 0;
  uint64_t maint_target_epoch = 0;
  // Bound consecutive fallback sleeps across actual deep-sleep resets. Consume
  // a retry only at the final sleep commit below, after the touch-abort check.
  // An exhausted episode stays exhausted until coordinated sleep or user input;
  // choosing the long timer must not itself reset and restart the quick loop.
  const bool fallback_requested = (sleep_fallback_timer_sec > 0);
  if (fallback_requested && sleep_fallback_consecutive >= SLEEP_FALLBACK_MAX_CONSECUTIVE) {
    Serial.printf("[SLEEP] fallback_loop_bounded consecutive=%u max=%u -> long_timer\n",
                  (unsigned)sleep_fallback_consecutive,
                  (unsigned)SLEEP_FALLBACK_MAX_CONSECUTIVE);
    sleep_fallback_timer_sec = 0;
  }
  if (sleep_fallback_timer_sec > 0) {
    sleep_timer_sec = sleep_fallback_timer_sec;
    timer_reason = "fallback";
  } else if (g_lcd_arm_storage_fault) {
    sleep_timer_sec = LCD_OTA_WAKE_INTERVAL_SEC;
    timer_reason = "maintenance_store_failed";
  } else if (maint_abs) {
    maint_now_epoch = (uint64_t)time(nullptr);
    uint64_t lead = (uint64_t)g_lcd_maintenance_grace_before_sec + (uint64_t)LCD_MAINT_WAKE_LEAD_S;
    maint_target_epoch = (g_lcd_maintenance_start_epoch > lead)
                           ? (g_lcd_maintenance_start_epoch - lead)
                           : 0;
    if (maint_target_epoch > maint_now_epoch) {
      uint64_t delta = maint_target_epoch - maint_now_epoch;
      if (delta > 0xFFFFFFFFULL) delta = 0xFFFFFFFFULL;
      sleep_timer_sec = (uint32_t)delta;
      timer_reason = "maintenance_abs";
    } else if (maint_now_epoch > g_lcd_maintenance_start_epoch +
                   (uint64_t)g_lcd_maintenance_duration_sec + g_lcd_maintenance_grace_after_sec) {
      // An expired absolute arm must not produce a five-second reset loop,
      // including when the attempted durable disarm fails.
      lcd_clear_persisted_maintenance_state("absolute_expired");
      sleep_timer_sec = LCD_OTA_WAKE_INTERVAL_SEC;
      timer_reason = "maintenance_expired";
    } else {
      // At/inside the wake band or window already: do not deep-sleep for the
      // stale relative value. Part D keeps the LCD awake through the window, but
      // if we still reach here, clamp to a short timer as a safety net so we
      // re-evaluate quickly rather than sleeping past the window.
      sleep_timer_sec = 5;
      timer_reason = "maintenance_abs_imminent";
    }
  } else if (g_lcd_maintenance_timer_armed && g_lcd_maintenance_wake_in_s > 0) {
    // Relative fallback (clock invalid): legacy behavior.
    sleep_timer_sec = g_lcd_maintenance_wake_in_s;
    timer_reason = "maintenance_rel";
  } else {
    sleep_timer_sec = LCD_OTA_WAKE_INTERVAL_SEC;
    timer_reason = "periodic";
  }
  bool media_timer_selected = false;
#if HALO_ALLOW_TIMER_WAKE
  sleep_timer_sec = lcd_media_retry_choose_timer(sleep_timer_sec, &media_timer_selected);
  if (media_timer_selected) timer_reason = "media_retry";
#endif
  Serial.printf("[SLEEP_TIMER] reason=%s timer_s=%lu maint_armed=%d wake_in_s=%lu clock_valid=%d now_epoch=%llu target_epoch=%llu start_epoch=%llu\n",
                timer_reason,
                (unsigned long)sleep_timer_sec,
                g_lcd_maintenance_timer_armed ? 1 : 0,
                (unsigned long)g_lcd_maintenance_wake_in_s,
                lcd_time_valid() ? 1 : 0,
                (unsigned long long)maint_now_epoch,
                (unsigned long long)maint_target_epoch,
                (unsigned long long)g_lcd_maintenance_start_epoch);
  // Arm-time delivery race fix (breadcrumb): when a maintenance timer is armed,
  // record what self-wake timer we chose (maintenance_abs vs maintenance_rel vs
  // periodic). value=sleep_timer_sec, detail=timer_reason. Gated on armed to
  // avoid noise on ordinary periodic sleeps.
  if (g_lcd_maintenance_timer_armed) {
    lcd_errlog_store_with_context("lcd", "maint", "SLEEP",
                                  (int)sleep_timer_sec, timer_reason);
  }
  // Configure RTC pull-up on wake GPIO so the pin doesn't float during deep sleep.
  // Digital pull-ups are disabled when the digital GPIO controller powers off.
  rtc_gpio_init((gpio_num_t)LCD_WAKE_GPIO);
  rtc_gpio_set_direction((gpio_num_t)LCD_WAKE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)LCD_WAKE_GPIO);
  rtc_gpio_pulldown_dis((gpio_num_t)LCD_WAKE_GPIO);
  Serial.printf("[SLEEP_GPIO] gpio=%d rtc_pullup=1 pulldown=0 level_now=%d\n",
                (int)LCD_WAKE_GPIO, digitalRead(LCD_WAKE_GPIO));

#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS && HALO_LCD_SLEEP_WITNESS
  lcd_sleep_witness_prepare(timer_reason, maint_now_epoch, sleep_timer_sec, LCD_MAINT_WAKE_LEAD_S);
#endif
  const bool sleep_timer_ok = configure_sleep_sources(true, sleep_timer_sec);
  if (media_timer_selected && !sleep_timer_ok) {
    lcd_sleep_touch_watch_end();
    abort_sleep_transition("media_timer_arm_failed", false);
    return;
  }
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

#if HALO_TEARDOWN_DELAY_MS
  // Bench: hold the window open long enough for a real tap to land in it.
  Serial.printf("[SLEEP_WATCH] teardown window widened to %dms for testing\n",
                (int)HALO_TEARDOWN_DELAY_MS);
  delay(HALO_TEARDOWN_DELAY_MS);
#endif

  // Report the window every time, not just on abort: without this, "the tap was
  // lost" and "no tap ever happened" look identical from the log.
  Serial.printf("[SLEEP_WATCH] teardown_ms=%lu isr_count=%lu fired=%d level=%d\n",
                (unsigned long)(millis() - s_sleep_transition_ms),
                (unsigned long)s_touch_isr_count,
                lcd_sleep_touch_fired() ? 1 : 0,
                digitalRead(LCD_WAKE_GPIO));

  // LAST CHANCE TO ABANDON. Must stay ABOVE Touch_Standby()/backlight-off/
  // vTaskDelete: once the UI task is gone there is no clean way back.
  if (!g_sleep_transition || sleep_blocked_for_ota()) {
    lcd_sleep_touch_watch_end();
    abort_sleep_transition("ota_or_cancel_during_teardown", lcd_sleep_touch_fired());
    return;
  }
  if (lcd_sleep_touch_fired()) {
    lcd_sleep_touch_watch_end();
    Serial.println("[SLEEP_ABORT] touch arrived during teardown - abandoning sleep");
    abort_sleep_transition("touch_during_teardown");
    return;
  }
  lcd_sleep_touch_watch_end();

#if defined(HALO_OTA_BENCH_CASE)
  if (!halo_bench_timer_commit(sleep_timer_sec, HALO_ALLOW_TIMER_WAKE && g_lcd_maintenance_timer_armed &&
                              !g_lcd_arm_storage_fault &&
                              strcmp(timer_reason, "maintenance_abs") == 0,
                              g_lcd_maintenance_request_id)) {
    abort_sleep_transition("bench_timer_arm_failed", false);
    return;
  }
#endif
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

  // Commit the retry episode only once sleep can no longer be aborted. Saturate
  // so repeated long fallback sleeps cannot wrap and re-enable short retries.
  if (fallback_requested) {
    if (sleep_fallback_consecutive < SLEEP_FALLBACK_MAX_CONSECUTIVE) {
      ++sleep_fallback_consecutive;
    }
  } else {
    sleep_fallback_reset("coordinated_sleep");
  }
  sleep_fallback_magic = SLEEP_FALLBACK_MAGIC;
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS && HALO_LCD_SLEEP_WITNESS
  lcd_sleep_witness_enter();
#endif
  // Only a committed sleep with a successful SDK timer arm may label the next
  // actual TIMER boot as media. Aborted teardown never creates that identity.
  lcd_media_retry_commit_sleep(sleep_timer_sec, media_timer_selected, sleep_timer_ok);
  esp_deep_sleep_start();
  
  // Deep sleep never returns. Wake policy and UI restoration live in setup().
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

struct LcdSleepHandshakeScope {
  LcdSleepHandshakeScope() { g_lcd_sleep_handshake_active.store(true); }
  ~LcdSleepHandshakeScope() { g_lcd_sleep_handshake_active.store(false); }
};

// Send sleep signal to Sense board before LCD goes to sleep
static bool notify_sense_sleep() {
  LcdSleepHandshakeScope handshake_scope;
  s_sleep_maintenance_deferred = false;
  if (sleep_defer_for_maintenance()) return false;
  s_sleep_media_deferred = false;
  if (sleep_defer_for_media()) return false;
  s_sleep_provision_deferred = false;
  if (sleep_defer_for_provisioning()) return false;
  Serial.println("[LCD] Notifying Sense board to sleep...");
  // Send diagnostics BEFORE the sleep handshake. After SLEEP_READY the Sense is
  // already asleep and anything sent then is lost (measured: 1 of 19 delivered).
  lcd_send_diag_pre_sleep();
  lcd_sleep_ts("notify_sense_sleep");
  sleep_ready_received = false;
  sleep_deny_received = false;
  sleep_deny_retry_ms = 0;
  sleep_deny_reason[0] = '\0';
  sleep_deny_received_ms = 0;
  sleep_deny_active = false;
  sleep_fallback_timer_sec = 0;
  sleep_handshake_fail_link = false;
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
    return !sleep_defer_for_media() && !sleep_defer_for_provisioning();
  }
  if (sense_state == SENSE_ASLEEP) {
    Serial.printf("[SLEEP] sense_asleep -> local sleep rx_age=%lu\n", age_ms);
    return !sleep_defer_for_media() && !sleep_defer_for_provisioning();
  }

  bool sense_probably_awake = (sense_state == SENSE_AWAKE) || sense_awake_estimate || sense_recent || grace_active;
  if (!sense_probably_awake || age_ms > SENSE_UNKNOWN_STALE_EXTENDED_MS) {
    if (!link_synced && age_ms > 60000UL) {
      sleep_fallback_timer_sec = SLEEP_FALLBACK_TIMER_SEC;
      Serial.printf("[SLEEP_PROTO] link_unsynced_stale rx_age=%lu -> fallback_timer_sleep\n",
                    age_ms);
      return !sleep_defer_for_media() && !sleep_defer_for_provisioning();
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
    if (sleep_defer_for_media()) return false;
    if (sleep_defer_for_provisioning()) return false;
    if (sleep_ready_received) {
      Serial.println("[SLEEP_PROTO] got SLEEP_READY while waiting_for_sleep_result -> success");
      return !sleep_defer_for_media() && !sleep_defer_for_provisioning();
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
      if (sleep_defer_for_media()) return false;
      if (sleep_defer_for_provisioning()) return false;
      if (sleep_defer_for_maintenance()) return false;
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
        touch_ignore_until = millis() + 450;
        scroll_ignore_until = millis() + 200;
        cancel_pending_sleep_for_user_input("pre_sleep_touch");
        abort_sleep_transition("pre_sleep_touch",
                               fresh_touch || last_scroll_activity_ms > baseline_scroll_activity_ms);
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
        return !sleep_defer_for_media() && !sleep_defer_for_provisioning();
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
      return !sleep_defer_for_media() && !sleep_defer_for_provisioning();
    }
  }
  return false;
}


#endif // LCD_SLEEP_H

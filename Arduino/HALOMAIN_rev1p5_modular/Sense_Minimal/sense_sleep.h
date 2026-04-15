/*
 * sense_sleep.h
 *
 * Sleep/wake pin management, deep sleep entry, and wake diagnostics
 * for Sense_Minimal.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 9.
 *
 * NOTE: This header is included late in the .ino (just before the
 * Sleep/Wake Functions section) so that all its dependencies —
 * globals, UART senders, sleep state machine, OTA helpers,
 * camera functions — are already defined. No forward declarations
 * are needed.
 *
 * Prerequisites (all satisfied by position in .ino):
 *   - WAKE_GPIO, WAKE_LEVEL, WAKE_ACTIVE_LEVEL, WAKE_INACTIVE_LEVEL,
 *     WAKE_PIN_* constants, SLEEP_DENY_* constants,
 *     MIN_AWAKE_BEFORE_SLEEP_MS, HALO_BOARD_NAME
 *   - SenseSleepKind enum, SleepSmState enum
 *   - Sleep state globals: sleep_requested, sleep_request_ms,
 *     sleep_request_logged, sleep_ready_sent_for_cycle, sleep_ready_reason,
 *     sleep_coord_requested, sleep_coord_pending_for_ready,
 *     sleep_deny_sent_for_request, sleep_sm_msg_id, sleep_grace_until_ms,
 *     sleep_holdoff_until_ms, uart_wake_enabled_state, wake_pin_abort_*,
 *     sleep_abort_active_pin_count, sleep_retry_deassert_count,
 *     last_sleep_abort_reason, g_sense_boot_count, g_rtc_clean_shutdown
 *   - Link state: last_uart_rx_ms, last_link_hb_rx_ms, link_synced, LINK_RECENT_MS
 *   - lcdSerial, LCD_UART_PORT
 *   - sleep_allowed_now(), sleep_sm_transition(), log_sleep_flags(),
 *     uart_send_sleep_deny(), uart_send_sleep_ready(),
 *     voice_session_reset(), diag_note_stage()
 *   - ota_configure_timer_wakeup(), ota_get_timer_delta_s()
 *   - deinit_camera(), camera_power_disable(), camera_power_hold_enable()
 *     from sense_camera.h
 *   - mqttClient, wifiClient, current_job, op_worker_task_handle
 */

#ifndef SENSE_SLEEP_H
#define SENSE_SLEEP_H

// ── Wake pin utilities ──────────────────────────────────────────────

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

// ── Sleep deny / retry ──────────────────────────────────────────────

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

// ── Deep sleep wakeup configuration ─────────────────────────────────

static void sense_config_deep_sleep_wakeup(bool enable_ext0, uint32_t fallback_timer_s) {
  // Configure WAKE_GPIO for deep sleep wake on LOW (LCD pulses LOW)
  wake_pin_configure_rtc_input_inactive_pull();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (enable_ext0) {
    rtc_gpio_init(WAKE_GPIO);
    rtc_gpio_set_direction(WAKE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(WAKE_GPIO);
    rtc_gpio_pulldown_dis(WAKE_GPIO);
    Serial.printf("[SENSE] RTC wake pin configured GPIO%d\n", WAKE_GPIO);
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

// ── Wake pin deassert wait ──────────────────────────────────────────

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

// ── Sleep deny and clear ────────────────────────────────────────────

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
  sleep_ready_sent_for_cycle = false;
  sleep_coord_pending_for_ready = false;
  sleep_sm_transition(SLEEP_SM_IDLE, "DENY", sleep_sm_msg_id);
  sleep_sm_msg_id = 0;
}

static void sleep_notify_late_block(const char* reason) {
  if (!sleep_coord_pending_for_ready) {
    return;
  }
  unsigned long now_ms = millis();
  sleep_send_deny_and_clear(reason ? reason : "late_block", now_ms);
  sleep_coord_pending_for_ready = false;
}

// ── Deep sleep entry ────────────────────────────────────────────────

static void sense_enter_deep_sleep(SenseSleepKind kind) {
  Serial.println("========================================");
  Serial.println("[SENSE] Preparing for DEEP SLEEP...");
  Serial.println("========================================");
  voice_session_reset("deep_sleep");
  unsigned long now_ms = millis();
  Serial.printf("[SLEEP_TS] enter_deep_sleep now_ms=%lu\n", now_ms);
  diag_note_stage("sleep_enter", 0);
  if (sleep_holdoff_until_ms > 0 && now_ms < sleep_holdoff_until_ms) {
    Serial.println("[SLEEP] inhibited reason=holdoff");
    sleep_notify_late_block("holdoff");
    return;
  }
  if (!sleep_allowed_now("pre_ready", NULL)) {
    sleep_notify_late_block("pre_ready_block");
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
  // 3) Verify SLEEP_READY log appears before deep sleep.

  // Check current job state
  Serial.printf("[SENSE] Current job: type=%d, state=%d\n", current_job.type, current_job.state);

#ifdef HALO_SENSE_PROD_WRAPPER
  Serial.printf("[SLEEP_TS] pre_sleep_begin now_ms=%lu\n", (unsigned long)millis());
  halo_prod_pre_sleep();
  Serial.printf("[SLEEP_TS] pre_sleep_end now_ms=%lu\n", (unsigned long)millis());
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

  delay(100);
  if (ext0_allowed) {
    if (wake_pin_is_active_level(rtc_gpio_get_level(WAKE_GPIO))) {
      Serial.println("[SLEEP_SANITY] wake_pin_active_after_ready");
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
  if (!sleep_allowed_now("pre_wifi_off", NULL)) {
    sleep_notify_late_block("pre_wifi_off");
    return;
  }
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

  // Keep the hardware PWDN line asserted through deep sleep.
  if (esp_camera_sensor_get() != NULL) {
    Serial.println("[CAM_PWR] sleep path camera still initialized - deinit first");
    deinit_camera();
  } else {
    camera_power_disable();
  }
  camera_power_hold_enable();
  Serial.printf("[CAM_PWR] level before sleep gpio=%d level=%d hold=%d\n",
                (int)CAM_PWDN_GPIO,
                gpio_get_level(CAM_PWDN_GPIO),
                g_cam_pwdn_hold_enabled ? 1 : 0);

  // Preserve the OTA/scheduled timer even if EXT0 is disabled late in sleep entry.
  uint32_t ota_timer_delta_s = ota_get_timer_delta_s();
  uint32_t timer_delta_s = ota_timer_delta_s > 0
                               ? ota_timer_delta_s
                               : (wake_pin_stuck ? WAKE_PIN_FAILSAFE_TIMER_S : 0);
  sense_config_deep_sleep_wakeup(ext0_allowed, timer_delta_s);
  Serial.printf("[SLEEP_DIAG] wake_sources ext0_gpio=%d ext0_level=%d ext0_enabled=%d timer_delta_s=%lu ota_timer_delta_s=%lu wake_pin_stuck=%d kind=%d\n",
                WAKE_GPIO,
                WAKE_LEVEL,
                ext0_allowed ? 1 : 0,
                (unsigned long)timer_delta_s,
                (unsigned long)ota_timer_delta_s,
                wake_pin_stuck ? 1 : 0,
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
  Serial.printf("[SLEEP_STATE] entering_deep_sleep now_ms=%lu wake_gpio=%d wake_level=%d\n",
                (unsigned long)millis(),
                (int)WAKE_GPIO,
                WAKE_LEVEL);
  log_sleep_flags("pre_deep_sleep");
  if (!sleep_allowed_now("pre_deep_sleep", NULL)) {
    sleep_notify_late_block("pre_deep_sleep");
    return;
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
  sleep_coord_pending_for_ready = false;
  uart_drain_tx(150);
  Serial.println("[SENSE] Entering DEEP SLEEP (will wake on EXT0/timer)...");
  Serial.flush();  // Final flush before sleep

  // Record time before sleep to verify sleep duration
  unsigned long sleep_start_time = millis();

  // 7. Enter deep sleep (no return)
  sleep_sm_transition(SLEEP_SM_SLEEPING, "ENTER_SLEEP", sleep_sm_msg_id);
  Serial.printf("[SLEEP_TS] deep_sleep_start now_ms=%lu\n", (unsigned long)millis());
  diag_note_stage("sleep_start", 0);
  g_rtc_clean_shutdown = 1;
  Serial.println("=================================");
  Serial.println("[SLEEP] entering deep sleep");
  Serial.println("[CAM_PWR] entering deep sleep");
  Serial.printf("[SLEEP] wake_gpio=%d\n", WAKE_GPIO);
  Serial.printf("[SLEEP] wake_level=%d\n", HALO_WAKE_LEVEL);
  Serial.println("=================================");
  esp_deep_sleep_start();
  (void)sleep_start_time;
}

static void sense_enter_sleep(SenseSleepKind kind) {
  sense_enter_deep_sleep(kind);
}

// ── Wake cause diagnostics ──────────────────────────────────────────

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

#endif // SENSE_SLEEP_H

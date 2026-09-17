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
 *   - current_job, op_worker_task_handle
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

static void uart_send_release_wake_request() {
  Serial.println("[SLEEP] requesting LCD release wake line via UART");
  StaticJsonDocument<96> doc;
  doc["ver"] = 1;
  doc["type"] = "RELEASE_WAKE";
  doc["msg_id"] = 0;
  doc["ts"] = millis();
  String out;
  serializeJson(doc, out);
  uart_send_json(out.c_str());
  // Flush TX and wait for LCD to process
  Serial.flush();
  delay(250);
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

// Arm the wake sources. `timer_s` is now applied in BOTH branches.
//
// It previously applied only when ext0 was DISABLED; the enabled branch (the
// normal case) called ota_configure_timer_wakeup() instead and ignored the
// caller's value entirely. So a timer computed by the caller — including the
// nightly 02:00 maintenance wake — silently never armed on the path the device
// actually takes. ESP32 supports multiple simultaneous wake sources, so ext0
// and the timer coexist: tap OR timer, whichever comes first.
//
// The caller guarantees timer_s > 0 (see the wake-timer block in
// sense_enter_deep_sleep), so "no wake source at all" is no longer reachable.
static bool sense_config_deep_sleep_wakeup(bool enable_ext0, uint32_t& timer_s, uint32_t normal_or_user_s) {
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
    Serial.printf("[SENSE] Wake EXT0 configured: GPIO%d level=%d\n", WAKE_GPIO, WAKE_LEVEL);
  }

  if (timer_s > 0) {
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
    const esp_err_t actual_timer_result=halo_policy_arm_timer(timer_s,normal_or_user_s);
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
    sense_diag_note_timer_sdk((uint64_t)timer_s*1000000ULL,actual_timer_result);
#endif
    if(actual_timer_result!=ESP_OK){
      Serial.printf("[SLEEP] timer configuration failed sdk=%ld; sleep refused\n",(long)actual_timer_result);
      return false;
    }
#elif defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
    const uint64_t actual_timer_us=(uint64_t)timer_s*1000000ULL;
    const esp_err_t actual_timer_result=esp_sleep_enable_timer_wakeup(actual_timer_us);
    sense_diag_note_timer_sdk(actual_timer_us,actual_timer_result);
#else
    esp_sleep_enable_timer_wakeup((uint64_t)timer_s * 1000000ULL);
#endif
    Serial.printf("[SENSE] Wake timer armed: %lus (%.2fh) ext0=%d\n",
                  (unsigned long)timer_s, timer_s / 3600.0, enable_ext0 ? 1 : 0);
  } else {
    // Should be unreachable — the caller always supplies a timer. Loud because
    // sleeping with no wake source bricks the device until someone taps it.
    Serial.println("[SENSE] WARNING: no timer armed - device wakes only on tap");
  }
  return timer_s>0;
}

// ── Wake pin quick-check for LCD pulsing ────────────────────────────

static bool wake_pin_check_pulsing(unsigned long window_ms) {
  int toggles = 0;
  int prev = -1;
  unsigned long start = millis();
  while ((millis() - start) < window_ms) {
    int level = gpio_get_level(WAKE_GPIO);
    int active = wake_pin_is_active_level(level) ? 1 : 0;
    if (prev >= 0 && active != prev) {
      toggles++;
      // Any single transition means LCD is actively driving the pin.
      // The pullup holds the line inactive — noise can't cause a
      // transition visible at 10ms polling intervals.
      Serial.printf("[WAKE_PIN_CHECK] pulsing_detected toggles=%d window_ms=%lu elapsed=%lu\n",
                    toggles, window_ms, millis() - start);
      return true;
    }
    prev = active;
    delay(10);
  }
  return false;
}

// ── Wake pin deassert wait ──────────────────────────────────────────

static uint16_t sleep_sanity_toggle_count = 0;

static bool sleep_wait_wake_pin_deassert(unsigned long wait_ms) {
  unsigned long start_ms = millis();
  unsigned long next_log_ms = start_ms;
  unsigned long stable_start_ms = 0;
  int prev_active = -1;  // -1 = unknown
  sleep_sanity_toggle_count = 0;
  while ((millis() - start_ms) < wait_ms) {
    unsigned long now_ms = millis();
    int level = rtc_gpio_get_level(WAKE_GPIO);
    int is_active = wake_pin_is_active_level(level) ? 1 : 0;
    // Count transitions — early exit if LCD is clearly pulsing
    if (prev_active >= 0 && is_active != prev_active) {
      sleep_sanity_toggle_count++;
      if (sleep_sanity_toggle_count >= 3) {
        Serial.printf("[SLEEP_SANITY] early_exit toggles=%u elapsed_ms=%lu\n",
                      (unsigned)sleep_sanity_toggle_count,
                      millis() - start_ms);
        return false;
      }
    }
    prev_active = is_active;
    if (is_active) {
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
      Serial.printf("[SLEEP_SANITY] wake_pin_level=%d wake_active=%d wait_left_ms=%lu stable_ms=%lu toggles=%u\n",
                    level,
                    WAKE_ACTIVE_LEVEL,
                    (unsigned long)(wait_ms - (now_ms - start_ms)),
                    stable_ms,
                    (unsigned)sleep_sanity_toggle_count);
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

// Optional hook: co-schedule the LCD to wake alongside the Sense's next timer
// wake. Set by the prod wrapper (halo_sense_prod.ino) at setup; left null in the
// plain Sense_Minimal build. Called with the FINAL wake delta (after the
// HALO_MAINT_TEST_S override and any OTA/spool shortening), so the LCD arms the
// same instant the Sense does. Without this the LCD sleeps on its own unrelated
// 6h periodic timer and is not awake for the Sense's LCD-OTA proxy on the
// unattended nightly path -- so a nightly OTA updates only the Sense.
static void (*g_lcd_maint_coschedule_hook)(uint32_t wake_in_s) = nullptr;
// Reports the actual selected interval on the next available cloud report.
static void (*g_sleep_timer_selected_hook)(uint32_t seconds) = nullptr;

// Called only by the main task at a boundary with no locally dequeued photo.
// The UART pump owns/leases raw RX and releases it before invoking callbacks.
// Only a new, deduplicated user action changes this generation; link traffic
// must not renew a sleep/flush deadline. A guardian-forced teardown stays owned.
static bool sleep_upload_flush_yield_to_user(uint32_t user_generation,
                                           unsigned long started_ms,
                                           unsigned long budget_ms) {
  if (guardian_force_sleep) return false;
  pump_uart_rx_once();
  if (sense_user_action_generation() == user_generation) return false;
  g_upload_flush_requested = false;
  char detail[96];
  snprintf(detail, sizeof(detail), "queued=%lu elapsed_ms=%lu budget_ms=%lu",
           (unsigned long)upload_queue_count(), millis() - started_ms, budget_ms);
  uart_send_sense_diag("sleep", "flush_cancel", "user_activity", 0, detail);
  sleep_notify_late_block("user_activity");
  return true;
}

static void sense_enter_deep_sleep(SenseSleepKind kind) {
#ifdef STRESS_TEST_NO_SLEEP
  // Bench builds must stay on the USB bus. The only previous guard was in the
  // INPUT_SLEEP handler (Sense_Minimal.ino), which left every other path to
  // this function — idle timeout, sleep coordinator, post-op — free to sleep
  // and drop the Sense's USB CDC mid-test. That is what silently killed the
  // third spool run: the transfer had not failed, the port had gone away.
  // Gating the single chokepoint covers all callers.
  static uint32_t s_no_sleep_suppressed = 0;
  Serial.printf("[DEV] deep sleep suppressed x%lu kind=%d (STRESS_TEST_NO_SLEEP=1)\n",
                (unsigned long)(++s_no_sleep_suppressed), (int)kind);
  return;
#endif
  const uint32_t sleep_user_generation = sense_user_action_generation();
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

  // If LCD is pulsing the wake pin, abort sleep — user action pending
  if (wake_pin_check_pulsing(200)) {
    Serial.println("[SLEEP] abort (lcd_pulsing after pre_sleep)");
    sleep_notify_late_block("lcd_pulsing");
    return;
  }

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
      // If pin toggled multiple times, LCD is actively pulsing — abort sleep
      // entirely so Sense stays awake to handle the user's request.
      if (sleep_sanity_toggle_count >= 3) {
        Serial.printf("[SLEEP_SANITY] lcd_pulsing_detected toggles=%u -> abort_sleep\n",
                      (unsigned)sleep_sanity_toggle_count);
        sleep_notify_late_block("lcd_pulsing");
        return;
      }
      wake_pin_apply_mitigation("pre_sleep");
      wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      // Last resort: ask LCD to release wake line
      if (wake_pin_is_active_level(wake_pin_level)) {
        uart_send_release_wake_request();
        wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      }
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
      if (sleep_sanity_toggle_count >= 3) {
        Serial.printf("[SLEEP_SANITY] lcd_pulsing_detected toggles=%u -> abort_sleep\n",
                      (unsigned)sleep_sanity_toggle_count);
        sleep_notify_late_block("lcd_pulsing");
        return;
      }
      wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      // Last resort: ask LCD to release wake line
      if (wake_pin_is_active_level(wake_pin_level)) {
        uart_send_release_wake_request();
        wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      }
      if (wake_pin_is_active_level(wake_pin_level)) {
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
  }

  delay(100);
  if (ext0_allowed) {
    if (wake_pin_is_active_level(rtc_gpio_get_level(WAKE_GPIO))) {
      Serial.println("[SLEEP_SANITY] wake_pin_active_after_ready");
    }
    if (!sleep_wait_wake_pin_deassert(500)) {
      if (sleep_sanity_toggle_count >= 3) {
        Serial.printf("[SLEEP_SANITY] lcd_pulsing_detected toggles=%u -> abort_sleep\n",
                      (unsigned)sleep_sanity_toggle_count);
        sleep_notify_late_block("lcd_pulsing");
        return;
      }
      wake_pin_apply_mitigation("post_ready");
      wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      // Last resort: ask LCD to release wake line
      if (wake_pin_is_active_level(wake_pin_level)) {
        uart_send_release_wake_request();
        wake_pin_level = rtc_gpio_get_level(WAKE_GPIO);
      }
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

  // Stop synchronously before the flush releases worker DNS. A plausible
  // retained clock is not fresh proof; no new attempt starts during teardown.
  sense_ntp_quiesce_for_sleep();

  // --- Upload flush window: drain pending uploads before sleep ---
  // User may have done captures during this wake cycle. The upload_worker_task
  // runs on Core 1 and processes the queue independently. Give it time to
  // finish before we tear down WiFi.
  {
    // Open the deferred-upload gate. Normal (non-dish) uploads are held for the
    // whole awake session so their TLS handshakes cannot fragment the DMA region
    // the camera needs — see uploads_held_for_session() in Sense_Minimal.ino.
    // THIS is the moment they are released: the user has stopped, the camera is
    // about to be torn down, and the full internal heap is available.
    // Close the temporary worker gate on every exit, including cancellation
    // and a later sleep admission refusal. Queue/parked ownership is unchanged.
    struct FlushGate {
      ~FlushGate() { g_upload_flush_requested = false; }
    } flush_gate;

    // Budget must scale with what is actually queued.
    //
    // A flat 30s was fine when uploads went out during the session and at most
    // one was ever pending here. Deferring them to sleep means the whole
    // session's captures arrive at once — measured 2026-08-21: 5 captures, each
    // upload ~6s, flush hit 30s with one still in flight, the device slept, and
    // that photo was LOST (up_ok=4, spool=0). Deferral created that path, so the
    // budget has to cover it.
    const uint32_t pending_now = upload_queue_count() +
                                 (upload_inflight ? 1 : 0) +
                                 (upload_worker_parked_pending() ? 1 : 0);
    const unsigned long UPLOAD_FLUSH_TIMEOUT_MS =
        (pending_now <= 1) ? 30000UL
                           : (15000UL * (unsigned long)pending_now > 180000UL
                                  ? 180000UL                       // hard ceiling: 3 min
                                  : 15000UL * (unsigned long)pending_now);
    const unsigned long UPLOAD_FLUSH_LOG_INTERVAL_MS = 2000;
    unsigned long flush_start = millis();
    unsigned long last_log = 0;
    uint32_t initial_count = upload_queue_count();
    bool has_parked = upload_worker_parked_pending();
    bool had_pending = (initial_count > 0 || upload_inflight || has_parked ||
                        upload_worker_claim_active.load() ||
                        upload_worker_holding_in_place);

    if (sleep_upload_flush_yield_to_user(sleep_user_generation, flush_start,
                                        UPLOAD_FLUSH_TIMEOUT_MS)) return;
    char flush_detail[96];
    snprintf(flush_detail, sizeof(flush_detail),
             "queued=%lu inflight=%u parked=%u budget_ms=%lu",
             (unsigned long)initial_count, upload_inflight ? 1 : 0,
             has_parked ? 1 : 0, UPLOAD_FLUSH_TIMEOUT_MS);
    uart_send_sense_diag("sleep", "flush_start", "upload", 0, flush_detail);
    g_upload_flush_requested = true;

    if (had_pending) {
      Serial.printf("[SLEEP_UPLOAD_FLUSH] start pending=%lu inflight=%d parked=%d\n",
                    (unsigned long)initial_count, upload_inflight ? 1 : 0, has_parked ? 1 : 0);
    }

    while (had_pending &&
           (upload_queue_count() > 0 || upload_inflight || upload_worker_parked_pending() ||
            upload_worker_claim_active.load() ||
            upload_worker_holding_in_place) &&
           (millis() - flush_start) < UPLOAD_FLUSH_TIMEOUT_MS) {
      if (sleep_upload_flush_yield_to_user(sleep_user_generation, flush_start,
                                          UPLOAD_FLUSH_TIMEOUT_MS)) return;
      // Log progress periodically
      if ((millis() - last_log) >= UPLOAD_FLUSH_LOG_INTERVAL_MS) {
        last_log = millis();
        Serial.printf("[SLEEP_UPLOAD_FLUSH] waiting queue=%lu inflight=%d parked=%d held=%d elapsed=%lums\n",
                      (unsigned long)upload_queue_count(),
                      upload_inflight ? 1 : 0,
                      upload_worker_parked_pending() ? 1 : 0,
                      upload_worker_holding_in_place ? 1 : 0,
                      millis() - flush_start);
      }
      delay(100);  // Yield to upload_worker_task on Core 1
    }

    if (sleep_upload_flush_yield_to_user(sleep_user_generation, flush_start,
                                        UPLOAD_FLUSH_TIMEOUT_MS)) return;
    // The upload worker remains the only consumer of queued/parked media.
    // A long SD save can outlive this flush window. Main-task rescue used to
    // dequeue the next job while that save owned UART, count local admission
    // refusal as a storage failure, and free the only RAM copy. Defer ordinary
    // teardown instead; closing FlushGate preserves ownership, and the next
    // normal sleep attempt reopens the same bounded worker drain.
    const uint32_t remaining_after_flush = upload_queue_count();
    const bool pending_after_flush = remaining_after_flush || upload_inflight ||
        upload_worker_parked_pending() || upload_worker_holding_in_place ||
        upload_worker_claim_active.load();
    if (pending_after_flush) {
      snprintf(flush_detail, sizeof(flush_detail),
               "queued=%lu inflight=%u elapsed_ms=%lu budget_ms=%lu",
               (unsigned long)remaining_after_flush, upload_inflight ? 1 : 0,
               millis() - flush_start, UPLOAD_FLUSH_TIMEOUT_MS);
      if (!guardian_force_sleep) {
        Serial.printf("[SLEEP_UPLOAD_FLUSH] deferred pending_worker queued=%lu inflight=%u\n",
                      (unsigned long)remaining_after_flush, upload_inflight ? 1 : 0);
        uart_send_sense_diag("sleep", "flush_defer", "pending_worker", 0, flush_detail);
        sleep_notify_late_block("upload_flush_pending");
        return;
      }
      // Preserve the existing guardian hard stop. RAM-only work is not durable
      // at that boundary; do not report it saved or claim lossless forced sleep.
      Serial.println("[SLEEP_UPLOAD_FLUSH] guardian hard stop: pending RAM media at risk");
      uart_send_sense_diag("sleep", "flush_forced", "guardian", 0, flush_detail);
    }

    if (had_pending) {
      uint32_t remaining = upload_queue_count();
      Serial.printf("[SLEEP_UPLOAD_FLUSH] done remaining=%lu inflight=%d elapsed=%lums %s\n",
                    (unsigned long)remaining,
                    upload_inflight ? 1 : 0,
                    millis() - flush_start,
                    (remaining == 0 && !upload_inflight) ? "DRAINED" : "TIMEOUT");
    }
    if (sleep_upload_flush_yield_to_user(sleep_user_generation, flush_start,
                                        UPLOAD_FLUSH_TIMEOUT_MS)) return;
    snprintf(flush_detail, sizeof(flush_detail),
             "queued=%lu inflight=%u elapsed_ms=%lu budget_ms=%lu",
             (unsigned long)upload_queue_count(), upload_inflight ? 1 : 0,
             millis() - flush_start, UPLOAD_FLUSH_TIMEOUT_MS);
    uart_send_sense_diag("sleep", "flush_end", "upload", 0, flush_detail);
  }

  // 1. Shut down Wi-Fi + BT before deep sleep
  if (!sleep_allowed_now("pre_wifi_off", NULL)) {
    sleep_notify_late_block("pre_wifi_off");
    return;
  }
  // Independent saved-media arm while normal receive dispatch is still live.
  // Relative time works without SNTP; no OTA arm, allowance or debt is changed.
  const uint32_t media_retry_s = media_retry_interval();
  const uint32_t media_arm_at_ms = millis();
  media_retry_arm_lcd(media_retry_s, sleep_user_generation);
  if (sleep_upload_flush_yield_to_user(sleep_user_generation, media_arm_at_ms, 1800)) return;
  if (!sleep_allowed_now("media_arm_complete", NULL)) {
    sleep_notify_late_block("media_arm_complete");
    return;
  }
  // Belt-and-suspenders: never tear down WiFi while the user is on the LCD
  // shopping-list screen. Normally the sleep gate (sense_can_sleep_now) blocks
  // us from ever reaching here while list-active, but if we somehow do, keep
  // WiFi up so a list refresh/delete still hits a live connection.
  if (g_list_screen_active) {
    Serial.println("[SLEEP] WiFi teardown SKIPPED reason=list_screen_active");
    btStop();
  } else {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[SENSE] Disconnecting Wi-Fi before deep sleep...");
    }
    WiFi.disconnect(true);
    delay(50);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    btStop();
  }

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

  // ── Wake timer: ALWAYS arm one ────────────────────────────────────────
  //
  // THE RULE: a device must never sleep without a next wake armed. The previous
  // form fell through to 0 whenever the OTA orchestrator had nothing scheduled
  // and the wake pin was healthy — i.e. the normal case — so the device slept
  // with NO timer and could only be revived by a physical tap. On a mostly-off
  // device in someone's kitchen there is no human who knows to do that, and a
  // missed nightly window could never self-heal.
  //
  // Baseline is now the 02:00 local maintenance wake (NightlySchedule.h, which
  // handles DST properly — "tomorrow" is not now+86400, and 02:00 does not
  // exist on spring-forward day). Anything sooner takes precedence.
  uint32_t ota_timer_delta_s = ota_get_timer_delta_s();
  uint32_t nightly_s = sense_time_has_fresh_sync()
                           ? halo_seconds_until_maintenance((time_t)sense_now_epoch()) : 0;
  if (nightly_s == 0) {
    // No usable wall clock. Wake on a plain interval anyway so the device gets
    // a chance to re-sync time and try again, rather than sleeping forever.
    nightly_s = HALO_MAINTENANCE_FALLBACK_S;
    Serial.println("[SLEEP] no fresh confirmed clock - arming fallback interval");
  }
#ifdef HALO_MAINT_TEST_S
  // Bench override: collapse the nightly wake to a few seconds so the ARM ->
  // SLEEP -> TIMER-FIRES -> RE-ARM cycle can be exercised in a minute instead of
  // a day. Deliberately independent of STRESS_TEST_NO_SLEEP, because this test
  // needs the device to ACTUALLY sleep. The local-hour arithmetic it bypasses is
  // verified separately (host tests + the on-device `nextwake` command).
  if (HALO_MAINT_TEST_S > 0) {
    Serial.printf("[SLEEP] HALO_MAINT_TEST_S override: %ds instead of %lus\n",
                  (int)HALO_MAINT_TEST_S, (unsigned long)nightly_s);
    nightly_s = (uint32_t)HALO_MAINT_TEST_S;
  }
#endif
  uint32_t timer_delta_s = nightly_s;
  uint32_t normal_or_user_s = nightly_s;
  if (ota_timer_delta_s > 0 && ota_timer_delta_s < timer_delta_s) {
    timer_delta_s = ota_timer_delta_s;
  }
  // Captures still on the SD card earn a short wake to come back and send them.
  // Reads cached state only -- deliberately no UART traffic on this path.
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
  // A drain wake that is still open at sleep time (nothing ended it) is closed
  // here so its outcome and backoff are still recorded.
  if (g_spool_drain_wake) {
    sense_spool_drain_wake_finish(WAKE_DRAIN_ACTIVE);
  }
  if (sense_spool_wants_early_wake() && SPOOL_DRAIN_WAKE_S < timer_delta_s) {
    timer_delta_s = SPOOL_DRAIN_WAKE_S;
    Serial.printf("[SPOOL_DRAIN] arming a %us drain wake (depth=%u barren=%u)\n",
                  (unsigned)SPOOL_DRAIN_WAKE_S, (unsigned)g_spool_last_known_depth,
                  (unsigned)g_spool_barren_wakes);
  }
  if (sense_spool_wants_early_wake() && SPOOL_DRAIN_WAKE_S < normal_or_user_s)
    normal_or_user_s = SPOOL_DRAIN_WAKE_S;
#endif
  if (wake_pin_stuck && WAKE_PIN_FAILSAFE_TIMER_S < timer_delta_s) {
    timer_delta_s = WAKE_PIN_FAILSAFE_TIMER_S;
  }
  if (wake_pin_stuck && WAKE_PIN_FAILSAFE_TIMER_S < normal_or_user_s)
    normal_or_user_s = WAKE_PIN_FAILSAFE_TIMER_S;
  const uint32_t before_media_s = timer_delta_s;
  const uint32_t media_elapsed_s = (uint32_t)(millis() - media_arm_at_ms + 999U) / 1000U;
  const uint32_t media_remaining_s = !media_retry_s ? 0 :
      (media_retry_s > media_elapsed_s ? media_retry_s - media_elapsed_s : 1);
  if (media_remaining_s && media_remaining_s < timer_delta_s) timer_delta_s = media_remaining_s;
  if (media_remaining_s && media_remaining_s < normal_or_user_s) normal_or_user_s = media_remaining_s;
  Serial.printf("[SLEEP] wake timer: nightly=%lus ota=%lus chosen=%lus\n",
                (unsigned long)nightly_s, (unsigned long)ota_timer_delta_s,
                (unsigned long)timer_delta_s);
  // Tell the LCD to wake at the same instant so it is up for the LCD-OTA proxy
  // on the unattended nightly path. Sent here (before the SLEEP_READY handshake,
  // while the LCD is still awake) using the FINAL delta so both boards align;
  // the LCD applies its own ~15s lead. No-op in the plain Sense build (hook null).
  if (g_lcd_maint_coschedule_hook && timer_delta_s > 0) {
    g_lcd_maint_coschedule_hook(timer_delta_s);
  }
  // Close the wake-cycle record before actually sleeping. An entry left open on
  // the next boot means the device never got here — panic, hang or brownout.
  wakelog_end_cycle(timer_delta_s, ext0_allowed, (uint16_t)g_spool_last_known_depth);
  if(!sense_config_deep_sleep_wakeup(ext0_allowed, timer_delta_s, normal_or_user_s)){
    sleep_notify_late_block("timer_configuration");return;
  }
  g_media_retry_timer_selected = halo_media_retry::selected(timer_delta_s, media_remaining_s, before_media_s);
  Serial.printf("[MEDIA_RETRY] sleep seconds=%lu selected=%u peer_ack=%u\n",
                (unsigned long)media_remaining_s, g_media_retry_timer_selected ? 1 : 0,
                g_media_retry_arm_acked ? 1 : 0);
  if (g_sleep_timer_selected_hook) g_sleep_timer_selected_hook(timer_delta_s);
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
  media_retry_commit_sleep();
  if (!sleep_ready_sent_for_cycle) {
    Serial.printf("[SLEEP] sending SLEEP_READY reason=%s\n",
                  sleep_ready_reason ? sleep_ready_reason : "unknown");
    Serial.printf("[SLEEP_PROTO] tx SLEEP_READY reason=%s\n",
                  sleep_ready_reason ? sleep_ready_reason : "unknown");
#if HALO_SENSE_LCD_DIAG_BRIDGE
    wifi_diag_finalize();
#endif
    // The LCD may still be persisting MAINT_WINDOW with flash cache disabled.
    // Keep this final UART tail to the small SLEEP_READY frame: the optional
    // Wi-Fi summary plus READY could exceed its hardware RX FIFO during that
    // pause. Finalize local timing above; do not send diagnostics here.
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
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
  halo_sleep_witness_enter();
#endif
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

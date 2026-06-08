/*
 * lcd_uart_rx.h
 *
 * UART RX message processing: parses all incoming JSON messages from
 * Sense board and dispatches to state updates / event queue.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 2.
 *
 * CRITICAL: uart_process_received_message runs from uart_task on Core 0.
 * NO LVGL calls allowed — only writes to g_pending and posts events.
 *
 * Prerequisites (must be declared before #include "lcd_uart_rx.h"):
 *   - All globals, structs, enums, and functions used by the message
 *     handlers (this is a late include — all deps are satisfied by
 *     its position in the .ino)
 */

#ifndef LCD_UART_RX_H
#define LCD_UART_RX_H

static void ui_handle_ship_toast(const JsonDocument& doc) {
  const char* text = doc["text"] | "";
  if (app_event_queue != NULL) {
    app_event_t evt = {};
    evt.type = EVT_SHIP_UI_TOAST;
    strncpy(evt.data.ship_toast, text ? text : "", sizeof(evt.data.ship_toast) - 1);
    evt.data.ship_toast[sizeof(evt.data.ship_toast) - 1] = '\0';
    xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
  }
}

// ── UART Message Processing ─────────────────────────────────────────
// CRITICAL: This function runs from uart_task - NO LVGL calls allowed!
// Only writes to g_pending buffer and posts events to UI task
static void uart_process_received_message(const char* json_str) {
  // LCD_Minimal/LCD_Minimal.ino: uart_process_received_message
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
  const char* json_start = strchr(p, '{');
  if (json_start == NULL) {
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
  if (json_start != p) {
    static unsigned long last_skip_log_ms = 0;
    if (millis() - last_skip_log_ms > 5000) {
      char preview[32];
      strncpy(preview, p, sizeof(preview) - 1);
      preview[sizeof(preview) - 1] = '\0';
      Serial.printf("[UART] nonjson_prefix skipped line_prefix=%s\n", preview);
      last_skip_log_ms = millis();
    }
    p = json_start;
  }

  DynamicJsonDocument doc(8192);
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
  uart_rx_valid_msgs_seen++;
  unsigned long prev_rx_ms = last_sense_rx_ms;
  note_sense_link_rx(type);
  uart_rx_count++;
  uart_note_rx_type(type);
  if (sense_rx_type_is_awake_proof(type)) {
    last_sense_rx_ms = millis();
    set_sense_awake_estimate(true, type);
  }
  last_sense_any_rx_ms = millis();
  last_sense_msg_ms = last_sense_any_rx_ms;
  Serial.printf("[PROTO] RX: type=%s\n", type);
#if SHIP_MENU_UI
  if (provisioning_input_locked()) {
    if (strcmp(type, "UI_STATUS") == 0 ||
        strcmp(type, "UI_MEAL_RESULT") == 0) {
      Serial.printf("[PROVISION] ignore type=%s (provisioning_active)\n", type);
      return;
    }
  }
  if (strcmp(type, "UI_STATUS") == 0) {
    ship_menu_handle_ui_status(doc);
    return;
  }
  if (strcmp(type, "SENSE_DIAG") == 0) {
    const char* area = doc["area"] | "";
    const char* event = doc["event"] | "";
    const char* label = doc["label"] | "";
    const char* detail = doc["detail"] | "";
    int32_t code = doc["code"] | 0;
    bool persist = doc["persist"] | false;
    Serial.printf("[SENSE_DIAG][%s] event=%s label=%s code=%ld detail=%s\n",
                  area, event, label, (long)code, detail);
    if (persist) {
        StaticJsonDocument<256> entry;
        entry["board"] = "sense";
        entry["area"] = area;
        entry["event"] = event;
        entry["code"] = code;
        if (detail[0]) entry["detail"] = detail;
        entry["uptime_ms"] = doc["ts"] | 0;
        String json;
        serializeJson(entry, json);
        errlog_store(json.c_str());
    }
    return;
  }
  if (strcmp(type, "WIFI_DIAG_SUMMARY") == 0) {
    String summary;
    serializeJson(doc, summary);
    Serial.printf("[WIFI_DIAG_SUMMARY] %s\n", summary.c_str());
    diag_store_wifi_summary(summary.c_str());
    return;
  }
  if (strcmp(type, "UI_MEAL_RESULT") == 0) {
    ui_apply_ship_meal_result(doc);
    return;
  }
  if (strcmp(type, "UI_TOAST") == 0) {
    ui_handle_ship_toast(doc);
    return;
  }
  if (strcmp(type, "UI_VOICE_RESPONSE") == 0) {
    if (g_voice_fire_and_forget_ignore_ui) {
      Serial.println("[VOICE_FAF] ignoring UI_VOICE_RESPONSE");
      return;
    }
    const char* json_text = doc["json"] | "{}";
    strncpy(g_ship_voice_json_text, json_text, sizeof(g_ship_voice_json_text) - 1);
    g_ship_voice_json_text[sizeof(g_ship_voice_json_text) - 1] = '\0';
    g_ship_voice_json_pending = true;
    waiting_for_voice_response = false;
    voice_response_deadline_ms = 0;
    Serial.println("[VOICE_TIMEOUT] cleared source=ui_voice_response");
    if (app_event_queue != NULL) {
      app_event_t evt = {};
      evt.type = EVT_SHIP_VOICE_JSON;
      if (xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20)) != pdTRUE) {
        Serial.println("[VOICE_JSON] queue_send_failed; pending_direct_apply=1");
      }
    }
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
    wake_timer_wait_mode = false;
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
  
  if (strcmp(type, "SLEEP_ACK") == 0 ||
      strcmp(type, "INPUT_SLEEP_ACK") == 0 ||
      strcmp(type, "SLEEP_BUSY") == 0) {
    last_sense_rx_ms = millis();
    set_sense_awake_estimate(true, "SLEEP_LEGACY");
    Serial.printf("[SLEEP_PROTO] ignoring legacy sleep status type=%s now_ms=%lu\n",
                  type,
                  (unsigned long)millis());
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
    if (g_lcd_maintenance_active) {
      Serial.println("[SLEEP_PROTO] SLEEP_READY received (maintenance_active)");
    }
    sleep_ready_received = true;
    sleep_wait_for_sense_idle = false;
    last_sense_sleep_ready_ms = millis();
    last_sense_rx_ms = millis();
    sense_state_set(SENSE_ASLEEP, "SLEEP_READY");
    lcd_sleep_ts("rx_sleep_ready");
    Serial.printf("[SLEEP_PROTO] rx SLEEP_READY now_ms=%lu\n",
                  (unsigned long)millis());
    Serial.println("[SLEEP] got SLEEP_READY -> release_wake_line");
    release_wake_line("sleep_ready");
    sense_wake_explicit_request = false;
    sense_status_sync_requested = false;
    wake_retry_until_ms = 0;

    // If LCD has a pending manual OTA request that Sense missed (race condition:
    // LCD sent INPUT_OTA_CHECK after Sense already started its sleep sequence),
    // re-wake Sense so it can process the request on the next boot cycle.
    if (g_manual_ota_override && millis() < ota_stay_awake_until_ms && !ota_locked) {
      Serial.println("[OTA_MANUAL] sense slept before seeing INPUT_OTA_CHECK — re-waking");
      delay(500);  // let Sense fully enter deep sleep before pulling wake pin
      request_sense_wake("manual_ota_retry");
      // Resend INPUT_OTA_CHECK so it's in UART buffer when Sense boots
      StaticJsonDocument<128> retryDoc;
      retryDoc["ver"] = PROTOCOL_VERSION;
      retryDoc["type"] = "INPUT_OTA_CHECK";
      retryDoc["msg_id"] = get_next_msg_id();
      retryDoc["ts"] = millis();
      retryDoc["reason"] = "manual_retry";
      String retryOut;
      serializeJson(retryDoc, retryOut);
      senseSerial.println(retryOut);
      Serial.printf("[OTA_MANUAL] resent INPUT_OTA_CHECK msg_id=%d\n",
                    (int)retryDoc["msg_id"]);
    }

    return;
  }

  if (strcmp(type, "SENSE_SLEEP_INTENT") == 0) {
    const char* deny_reason = NULL;
    if (!lcd_sleep_intent_allowed(&deny_reason)) {
      uint32_t retry_ms = SLEEP_DENY_RETRY_DEFAULT_MS;
      uart_send_sleep_deny(deny_reason ? deny_reason : "op_inflight", retry_ms);
      Serial.printf("[SLEEP_INTENT] deny reason=%s retry_ms=%lu\n",
                    deny_reason ? deny_reason : "op_inflight",
                    (unsigned long)retry_ms);
      return;
    }
    sleep_wait_for_sense_idle = false;
    sense_sleep_intent_pending = true;
    sense_sleep_intent_received_ms = millis();
    Serial.println("[SLEEP_INTENT] accepted -> pending sleep");
    return;
  }

  if (strcmp(type, "SENSE_OTA_ACTIVE") == 0) {
    if (!ota_locked) {
      Serial.println("[OTA] sense_ota_active ignored (not locked)");
      return;
    }
    sense_ota_active = true;
    Serial.println("[OTA] sense_ota_active=1");
    return;
  }

  if (strcmp(type, "SENSE_OTA_IDLE") == 0) {
    sense_ota_active = false;
    Serial.println("[OTA] sense_ota_active=0");
    return;
  }

  if (strcmp(type, "FW_INFO") == 0) {
    const char* sense_fw = doc["sense_fw"] | "";
    if (sense_fw && sense_fw[0]) {
      strncpy(g_sense_fw_version, sense_fw, sizeof(g_sense_fw_version) - 1);
      g_sense_fw_version[sizeof(g_sense_fw_version) - 1] = '\0';
    } else {
      strncpy(g_sense_fw_version, "--", sizeof(g_sense_fw_version) - 1);
      g_sense_fw_version[sizeof(g_sense_fw_version) - 1] = '\0';
    }
    g_fw_info_response_received = true;
    g_fw_info_last_attempt_ms = 0;
    g_fw_info_retry_deadline_ms = 0;
    ship_menu_update_versions_label();
    unsigned long age_ms = (g_fw_info_request_ms > 0) ? (millis() - g_fw_info_request_ms) : 0;
    Serial.printf("[UART] FW_INFO received sense_fw=%s age_ms=%lu awake=%d sync=%d\n",
                  g_sense_fw_version,
                  age_ms,
                  sense_awake_confirmed ? 1 : 0,
                  link_synced ? 1 : 0);
    // Re-send INPUT_OTA_CHECK if LCD manual override is still active
    // (original send was lost because Sense was in deep sleep)
    if (lcd_manual_ota_override_active()) {
      StaticJsonDocument<160> doc2;
      doc2["ver"] = PROTOCOL_VERSION;
      doc2["type"] = "INPUT_OTA_CHECK";
      doc2["msg_id"] = get_next_msg_id();
      doc2["ts"] = millis();
      doc2["reason"] = "resend_on_wake";
      String output;
      serializeJson(doc2, output);
      senseSerial.println(output);
      Serial.println("[OTA_MANUAL] resend INPUT_OTA_CHECK reason=sense_woke");
    }
    return;
  }

  if (strcmp(type, "RELEASE_WAKE") == 0) {
    Serial.println("[UART_RX] RELEASE_WAKE received -> releasing wake line");
    release_wake_line("sense_request");
    return;
  }

  if (strcmp(type, "STATUS_SYNC") == 0) {
    sense_status_sync_requested = true;
    Serial.println("[UART] STATUS_SYNC received");
    return;
  }

  if (strcmp(type, "MAINT_WINDOW") == 0) {
    uint32_t remaining_s = doc["remaining_s"] | 0;
    uint32_t wake_in_s = doc["wake_in_s"] | 0;
    bool clear = doc["clear"] | false;
    const char* request_id = doc["request_id"] | "";
    uint64_t start_epoch = doc["start_epoch"] | 0ULL;
    uint32_t duration_sec = doc["duration_sec"] | 0;
    uint32_t grace_before_sec = doc["grace_before_sec"] | 0;
    uint32_t grace_after_sec = doc["grace_after_sec"] | 0;
    uint64_t now_epoch = doc["now_epoch"] | 0ULL;
    // Set the LCD's wall clock from the Sense epoch BEFORE arming the timer or
    // persisting, so the absolute-window self-wake / window-current logic sees a
    // valid clock. Carried across deep sleep by the RTC. No-op when absent (old
    // Sense) — relative wake_in_s fallback then applies.
    lcd_set_clock_from_sense(now_epoch, "maint_window");
    if (g_lcd_maintenance_boot_grace_until_ms > 0) {
      g_lcd_maintenance_boot_grace_until_ms = 0;
      Serial.println("[LCD_MAINT] boot_grace_clear (maint_window)");
    }
    Serial.printf("[UART][MAINT_WINDOW] rx remaining_s=%lu wake_in_s=%lu clear=%d sleep_transition=%d maint_active=%d\n",
                  (unsigned long)remaining_s,
                  (unsigned long)wake_in_s,
                  clear ? 1 : 0,
                  g_sleep_transition ? 1 : 0,
                  g_lcd_maintenance_active ? 1 : 0);
    if (clear) {
      g_lcd_maintenance_active = false;
      g_lcd_maintenance_started = false;
      g_lcd_maintenance_aborted = false;
      g_lcd_maintenance_headless = false;
      g_lcd_maintenance_deadline_ms = 0;
      g_lcd_maintenance_timer_armed = 0;
      g_lcd_maintenance_wake_in_s = 0;
      g_lcd_maintenance_remaining_s = 0;
      g_lcd_maintenance_request_id[0] = '\0';
      g_lcd_maintenance_start_epoch = 0;
      g_lcd_maintenance_duration_sec = 0;
      g_lcd_maintenance_grace_before_sec = 0;
      g_lcd_maintenance_grace_after_sec = 0;
      lcd_clear_persisted_maintenance_state("maint_window_clear");
      // Exit OTA mode so UI task restarts, panel powers on, and sleep unblocks.
      // lcd_enter_maintenance_headless() called lcd_enter_ota_mode() which killed the
      // UI task and set g_ota_mode_active=true; we must undo that on clear.
      if (g_ota_mode_active) {
        lcd_exit_ota_mode("maint_window_clear");
      }
      Serial.println("[UART] MAINT_WINDOW clear");
      Serial.printf("[LCD_MAINT] state active=%d timer_armed=%d wake_in_s=%lu remaining_s=%lu deadline_ms=%lu\n",
                    g_lcd_maintenance_active ? 1 : 0,
                    g_lcd_maintenance_timer_armed ? 1 : 0,
                    (unsigned long)g_lcd_maintenance_wake_in_s,
                    (unsigned long)g_lcd_maintenance_remaining_s,
                    (unsigned long)g_lcd_maintenance_deadline_ms);
      lcd_log_rtc_timer_state("maint_window_clear");
      uart_send_maint_window_ack(remaining_s,
                                 wake_in_s,
                                 true,
                                 request_id,
                                 "cleared",
                                 true,
                                 start_epoch,
                                 duration_sec,
                                 grace_before_sec,
                                 grace_after_sec);
      return;
    }
    g_lcd_maintenance_remaining_s = remaining_s;
    if (request_id && request_id[0]) {
      strncpy(g_lcd_maintenance_request_id, request_id, sizeof(g_lcd_maintenance_request_id) - 1);
      g_lcd_maintenance_request_id[sizeof(g_lcd_maintenance_request_id) - 1] = '\0';
    } else {
      g_lcd_maintenance_request_id[0] = '\0';
    }
    g_lcd_maintenance_start_epoch = start_epoch;
    g_lcd_maintenance_duration_sec = duration_sec;
    g_lcd_maintenance_grace_before_sec = grace_before_sec;
    g_lcd_maintenance_grace_after_sec = grace_after_sec;
    // ALWAYS arm wake timer when we have a valid window — this must
    // survive even if headless mode is exited by touch
    if (remaining_s > 0) {
      g_lcd_maintenance_timer_armed = 1;
      g_lcd_maintenance_wake_in_s = (wake_in_s > 0) ? wake_in_s : remaining_s;
      g_lcd_maintenance_remaining_s = remaining_s;
      g_lcd_maintenance_deadline_ms = millis() + (unsigned long)remaining_s * 1000UL;
    }
    if (wake_in_s > 0) {
      g_lcd_maintenance_timer_armed = 1;
      g_lcd_maintenance_wake_in_s = wake_in_s;
    }
    // Persist to NVS BEFORE headless entry so timer survives touch exit
    lcd_persist_maintenance_state("maint_window_store");
    lcd_log_rtc_timer_state("maint_window_store");
    // Conditional headless entry — only if not already completed this cycle
    const char* ack_status = "stored";
    if (remaining_s > 0 && wake_in_s == 0) {
      // Skip headless if maintenance already completed this cycle, OR if
      // we just received OTA_UNLOCK (Sense confirmed LCD is up to date).
      // The Sense sends MAINT_WINDOW during pre_sleep sync even after OTA
      // is done — entering headless at that point kills the UI for no reason.
      unsigned long unlock_age = (ota_unlock_received_ms > 0) ? (millis() - ota_unlock_received_ms) : 0xFFFFFFFF;
      bool recently_unlocked = (unlock_age < 10000);  // within 10s of OTA_UNLOCK
      if (!g_lcd_maintenance_active && (g_lcd_maintenance_completed_ms > 0 || recently_unlocked)) {
        // Timer is armed above, just don't re-enter headless mode
        Serial.printf("[UART] MAINT_WINDOW skip headless (completed=%lu unlock_age=%lu)\n",
                      g_lcd_maintenance_completed_ms, unlock_age);
        ack_status = "timer_armed";
      } else {
        // Full activation + headless entry
        g_lcd_maintenance_active = true;
        g_lcd_maintenance_started = false;
        g_lcd_maintenance_aborted = false;
        lcd_mode = LCD_MODE_MAINTENANCE;
        // g_lcd_maintenance_deadline_ms already set above
        // g_lcd_maintenance_timer_armed already set above
        // g_lcd_maintenance_wake_in_s already set above
        if (g_sleep_transition) {
          g_sleep_transition = false;
          Serial.println("[LCD_MAINT] abort sleep transition (maint_window)");
        }
        if (g_in_light_sleep) {
          g_in_light_sleep = false;
        }
        resetActivityTimer();
        unsigned long until = millis() + (unsigned long)remaining_s * 1000UL;
        if (until > ota_stay_awake_until_ms) {
          ota_stay_awake_until_ms = until;
        }
#if HALO_OTA_POLICY_MAINTENANCE_ONLY
        if (!ota_check_requested && !ota_check_pending) {
          ota_check_requested = true;
          Serial.println("[OTA] maintenance_active -> self OTA check");
        }
#endif
#if HALO_OTA_POLICY_MAINTENANCE_ONLY
        if (ota_check_pending && !ota_locked) {
          ota_check_pending = false;
          ota_check_requested = true;
          Serial.println("[OTA] maintenance_active -> run deferred OTA check");
        }
#endif
        sleep_deny_received = true;
        sleep_deny_retry_ms = SLEEP_DENY_RETRY_DEFAULT_MS;
        strncpy(sleep_deny_reason, "maintenance", sizeof(sleep_deny_reason) - 1);
        sleep_deny_reason[sizeof(sleep_deny_reason) - 1] = '\0';
        sleep_deny_received_ms = millis();
#ifdef HALO_LCD_PROD_WRAPPER
        // Do NOT enter headless mode. LCD OTA is proxied by Sense over UART —
        // the LCD doesn't need WiFi/TLS RAM, so keep the UI running. Headless
        // mode kills the UI task and turns off the display, causing a cascade
        // of recovery bugs (stale NVS, black screen, failed reboots).
        // The UART binary transfer works fine with the UI running.
        Serial.println("[LCD_MAINT] staying in UI mode (OTA proxied by Sense)");
        touch_ignore_until = millis() + 2000;
#endif
        ack_status = "active";
      }
    }
    Serial.printf("[UART] MAINT_WINDOW received remaining_s=%lu wake_in_s=%lu\n",
                  (unsigned long)remaining_s,
                  (unsigned long)wake_in_s);
    Serial.printf("[LCD_MAINT] state active=%d timer_armed=%d wake_in_s=%lu remaining_s=%lu deadline_ms=%lu\n",
                  g_lcd_maintenance_active ? 1 : 0,
                  g_lcd_maintenance_timer_armed ? 1 : 0,
                  (unsigned long)g_lcd_maintenance_wake_in_s,
                  (unsigned long)g_lcd_maintenance_remaining_s,
                  (unsigned long)g_lcd_maintenance_deadline_ms);
    uart_send_maint_window_ack(remaining_s,
                               wake_in_s,
                               clear,
                               request_id,
                               ack_status,
                               true,
                               start_epoch,
                               duration_sec,
                               grace_before_sec,
                               grace_after_sec);
#ifdef HALO_LCD_PROD_WRAPPER
    if (g_lcd_maintenance_active && remaining_s > 0) {
      halo_lcd_prod_on_wifi_on(remaining_s * 1000UL);
    }
#endif
    return;
  }

  if (strcmp(type, "SYNC") == 0) {
    Serial.println("[LNK] sync_rx -> reset_partial");
    lcd_uart_reset_rx_state();
    link_synced = true;
    Serial.println("[LNK] synced=1");
    uart_send_ack("SYNC_ACK");
    Serial.println("[LNK] sync_ack_tx");
    return;
  }

  if (strcmp(type, "SYNC_ACK") == 0) {
    link_synced = true;
    sense_awake_confirmed = true;
    wake_timer_wait_mode = false;
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
#ifdef HALO_LCD_PROD_WRAPPER
    if (!lcd_wifi_allowed_for_ota()) {
      wifi_on_deferred = false;
      wifi_on_deferred_logged = false;
      wifi_on_deferred_timeout_ms = 0;
      wifi_on_reject_logged = false;
      Serial.println("[LCD_WIFI] WIFI_ON ignored (ota_only)");
      uart_send_wifi_on_ack("IGNORED", "OTA_ONLY");
      return;
    }
#endif
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
    if (g_lcd_maintenance_boot_grace_until_ms > 0) {
      g_lcd_maintenance_boot_grace_until_ms = 0;
      Serial.println("[LCD_MAINT] boot_grace_clear (ota_lock)");
    }
    // Extend maintenance deadline while Sense downloads first
    if (g_lcd_maintenance_active) {
      g_lcd_maintenance_deadline_ms = millis() + OTA_LOCK_TIMEOUT_MS;
      Serial.printf("[LCD_MAINT] deadline extended %lu ms (ota_lock during maintenance)\n",
                    (unsigned long)OTA_LOCK_TIMEOUT_MS);
    }
    // Extend the stay-awake window to cover the WHOLE dual-board OTA sequence:
    // Sense self-OTA (~40s) + reboot (~15s) + boot/wifi/proxy start (~20s).
    // The Sense sends OTA_UNLOCK before its self-OTA reboot (to let the LCD
    // sleep), which zeroes ota_stay_awake_until_ms — so OTA_UNLOCK must NOT
    // clear it while a recent OTA_LOCK extension is still live, otherwise the
    // LCD deep-sleeps and the post-reboot LCD_OTA_QUERY gets no UART reply
    // (lcd_query_fail). Only extend (never shorten), like ship_menu_send_manual_ota.
    {
      unsigned long ota_lock_awake_until = millis() + LCD_OTA_LOCK_STAY_AWAKE_MS;
      if (ota_lock_awake_until > ota_stay_awake_until_ms) {
        ota_stay_awake_until_ms = ota_lock_awake_until;
      }
      // Mark a FRESH dual-OTA-in-progress window. The SENSE_ASLEEP "missed-OTA
      // race guard" must NOT cancel ota_stay_awake_until_ms while this is live —
      // the Sense is mid self-OTA reboot (offline ~15s) and will proxy the LCD
      // afterward. A stale stay-awake with no recent OTA_LOCK leaves this at 0
      // and is still canceled normally. Cleared on LCD_OTA_BEGIN / post-OTA.
      g_ota_lock_window_until_ms = ota_lock_awake_until;
      Serial.printf("[OTA] ota_stay_awake extended %lums (ota_lock)\n",
                    (unsigned long)LCD_OTA_LOCK_STAY_AWAKE_MS);
    }
    // Wake display from idle-dark if needed
    if (g_idle_screen_dark) {
      lcd_set_idle_screen_dark(false, "ota_lock");
    }
    Serial.println("[OTA] lock received - blocking LCD OTA");
    // Do NOT touch LVGL here — this runs on Core 0 (UART task).
    // Calling lv_obj_clean/lv_label_create corrupts LVGL state and
    // prevents show_ship_main_menu() from working after OTA.
    // The current screen stays visible during OTA. After OTA,
    // restore_ui sets provision_return_home_pending which the UI task
    // picks up and shows HOME safely on Core 1.
    g_ota_screen_active = true;
    return;
  } else if (strcmp(type, "OTA_UNLOCK") == 0) {
    ota_locked = false;
    ota_check_pending = false;
    ota_check_requested = false;
    sense_ota_active = false;
    sense_ota_apply_required = false;
    // Do NOT blindly clear ota_stay_awake_until_ms here. The Sense sends
    // OTA_UNLOCK *before* its self-OTA reboot so the LCD is allowed to sleep,
    // but the LCD must stay awake (and UART-responsive) through the Sense
    // reboot + the post-reboot LCD_OTA_QUERY/proxy. The OTA_LOCK handler set a
    // long stay-awake window for exactly this; preserve it so the normal
    // sleep-decision keeps the LCD awake until the window naturally expires.
    if (millis() < ota_stay_awake_until_ms) {
      Serial.printf("[OTA] unlock - keep ota_stay_awake remaining %lums (dual-OTA)\n",
                    (unsigned long)(ota_stay_awake_until_ms - millis()));
    } else {
      ota_stay_awake_until_ms = 0;
    }
    g_ota_screen_active = false;
    g_lcd_maintenance_active = false;
    g_lcd_maintenance_deadline_ms = 0;
    g_ota_mode_active = false;
    ota_unlock_received_ms = millis();
    // Clear the manual-OTA override so the INPUT_OTA_CHECK resend loop in loop()
    // stops. Without this, an already-up-to-date manual OTA leaves the override
    // set; the resend condition (override_active() && !ota_locked) keeps firing,
    // re-locking/unlocking the Sense and looping the "Software Update" screen
    // (black-flash) until the 5-min override TTL. OTA_UNLOCK is the single
    // termination point for all Sense "nothing to do" exits (up_to_date,
    // downgrade blocked, rollout skip, apply blocked) — they all go through
    // release_waiting_lcd_ota -> OTA_UNLOCK. Safe/idempotent: early-returns if
    // the override is not set.
    lcd_manual_ota_override_clear("ota_unlock");
    provision_return_home_pending = true;   // UI task (Core 1) tears down the OTA UI and shows HOME, so sleep/wake doesn't redraw "Software Update"
    Serial.println("[OTA] return_home_pending=1 (ota_unlock)");
    Serial.println("[OTA] unlock received - all OTA flags cleared");
    return;
  } else if (strcmp(type, "OTA_CHECK") == 0) {
    uint32_t request_id = (uint32_t)(doc["request_id"] | 0);
    const char* reason = doc["reason"] | "";
    bool allow_reboot = doc["allow_reboot"] | true;
    // Treat scheduled OTA checks like manual so LCD does not defer on a stale
    // maintenance flag right at wake/entry time.
    bool manual_override =
        (reason && strcmp(reason, "manual") == 0) ||
        (reason && strcmp(reason, "scheduled_http") == 0) ||
        (reason && strcmp(reason, "maintenance") == 0);
    if (manual_override) {
      lcd_manual_ota_override_set("ota_check");
    }
    if (g_lcd_maintenance_boot_grace_until_ms > 0) {
      g_lcd_maintenance_boot_grace_until_ms = 0;
      Serial.println("[LCD_MAINT] boot_grace_clear (ota_check)");
    }
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

#if HALO_OTA_POLICY_MAINTENANCE_ONLY
    if (!manual_override && !lcd_maintenance_active()) {
      ota_check_pending = true;
      Serial.println("[OTA] OTA_CHECK deferred (maintenance_not_active)");
      return;
    }
#endif

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
    provision_qr_wait_clear("qr_uart");
    provision_cache_qr(ssid, password, url);
    if (g_lcd_maintenance_headless || g_ota_mode_active || !g_ui_initialized) {
      provision_qr_exit_headless = true;
      Serial.println("[PROVISION] qr_received -> exit_headless");
    }
    if (!provision_intro_tapped && !provision_intro_visible && !provision_intro_pending) {
      provision_intro_pending = true;
      if (app_event_queue != NULL) {
        app_event_t intro_evt = {};
        intro_evt.type = EVT_SHOW_PROVISION_INTRO;
        xQueueSend(app_event_queue, &intro_evt, pdMS_TO_TICKS(50));
      }
    }
    if ((provision_intro_visible || provision_intro_pending) && !provision_intro_tapped) {
      Serial.println("[PROVISION] qr_received -> deferred (intro_pending)");
      return;
    }
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
      bool unprovisioned = (strcmp(state, "unprovisioned") == 0 || strcmp(state, "ap_setup") == 0);
      if (strcmp(state, "connected") == 0) {
        provision_refresh_pending = true;
      }
      if (strcmp(state, "connected") == 0 || strcmp(state, "idle") == 0) {
        provision_qr_wait_clear("status_complete");
        provision_user_requested = false;
        provision_intro_tapped = false;
        provision_qr_cached = false;
        provision_return_home_pending = true;
        sleep_retry_requires_user = false;
        sleep_retry_allowed_ms = 0;
        sleep_handshake_fail_count = 0;
      }
      if (unprovisioned) {
        provision_intro_pending = true;
        provision_intro_tapped = false;
        if (app_event_queue != NULL) {
          app_event_t intro_evt = {};
          intro_evt.type = EVT_SHOW_PROVISION_INTRO;
          xQueueSend(app_event_queue, &intro_evt, pdMS_TO_TICKS(50));
        }
      } else if (!unprovisioned) {
        provision_intro_pending = false;
        if (app_event_queue != NULL) {
          app_event_t intro_evt = {};
          intro_evt.type = EVT_HIDE_PROVISION_INTRO;
          xQueueSend(app_event_queue, &intro_evt, pdMS_TO_TICKS(50));
        }
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
    if (provisioning_input_locked()) {
      Serial.println("[PROVISION] ignore UI_STATUS (provisioning_active)");
      return;
    }
    // Extended status message from Sense board
    const char* op = doc["op"] | "";
    const char* phase = doc["phase"] | "";
    const char* text = doc["text"] | "";
    Serial.printf("[UART] Status: op=%s, phase=%s, text=%s\n", op, phase, text);
    note_sense_proof_of_life("UI_STATUS");
    refresh_note_ui_proof("UI_STATUS");
    // Any UI_STATUS means Sense is awake and responding.
    sense_awake_confirmed = true;
    wake_timer_wait_mode = false;
    sense_wake_explicit_request = false;
    wake_retry_until_ms = 0;
    if (g_voice_fire_and_forget_ignore_ui && strcmp(op, "VOICE") == 0) {
      if (strcmp(phase, "DONE") == 0 || strcmp(phase, "ERROR") == 0) {
        g_voice_fire_and_forget_ignore_ui = false;
        waiting_for_voice_response = false;
        voice_response_deadline_ms = 0;
        g_ship_voice_json_pending = false;
        Serial.printf("[VOICE_FAF] backend_complete_raw phase=%s\n", phase);
        if (strcmp(phase, "ERROR") == 0) {
          // Let ERROR fall through to normal UI_STATUS handler so user sees it
          Serial.println("[VOICE_FAF] allowing ERROR to display");
        } else {
          return;
        }
      } else {
        Serial.printf("[VOICE_FAF] ignore raw UI_STATUS phase=%s\n", phase);
        return;
      }
    }
    
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
        if ((strcmp(phase, "UPLOADING") == 0 || strcmp(phase, "PROCESSING") == 0) &&
            voice_response_deadline_ms == 0) {
          voice_response_deadline_ms = millis() + VOICE_RESPONSE_TIMEOUT_MS;
          Serial.printf("[VOICE_TIMEOUT] armed source=ui_status phase=%s timeout_ms=%lu\n",
                        phase,
                        (unsigned long)VOICE_RESPONSE_TIMEOUT_MS);
        }
      } else if (strcmp(phase, "DONE") == 0 || strcmp(phase, "ERROR") == 0) {
        waiting_for_voice_response = false;
        voice_response_deadline_ms = 0;
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
        scan_request_sent_ms = millis();  // reset timeout — Sense is responding
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
            expiry_prepare_picker_for_entry();
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
                  expiry_prepare_picker_for_entry();
                  lv_obj_clear_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
                  Serial.println("[STATUS] Showing expiration date entry screen for check-in mode");
                  lv_timer_handler();  // Force immediate render
                }
              }
            } else {
              // For dish mode (or if mode not specified), show Frame_439_1
              status_screen_use_text("");
              Serial.println("[STATUS] Showing Frame_439_1 for dish scan");
              ui_lvgl_tick();  // Force immediate render
            }
          }
        }
      } else if (strcmp(phase, "DONE") == 0 || strcmp(phase, "ERROR") == 0) {
        waiting_for_scan_response = false;
        scan_request_sent_ms = 0;
        Serial.printf("[UART] SCAN operation complete (mode: %s) - restoring normal sleep timeout\n", mode);
        // Reset activity timer so user can see the result before sleep
        resetActivityTimer();
      }
    }
    
    // TODO: Display status banner on LCD (e.g., "Listening...", "Transcribing...", "Capturing...")
    // Post event to UI task to show status overlay
  } else if (strcmp(type, "UI_VOICE_ITEMS") == 0) {
    // List feature disabled
    Serial.println("[UART] UI_VOICE_ITEMS ignored (list feature disabled)");
    return;
    if (g_voice_fire_and_forget_ignore_ui) {
      Serial.println("[VOICE_FAF] ignoring UI_VOICE_ITEMS");
      return;
    }
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
    ui_apply_ship_meal_result(doc);
    return;
  } else if (strcmp(type, "LCD_OTA_QUERY") == 0) {
    lcd_ota_handle_query();
    return;
  } else if (strcmp(type, "LCD_OTA_BEGIN") == 0) {
    JsonObject obj = doc.as<JsonObject>();
    lcd_ota_handle_begin(obj);
    return;
  } else if (strcmp(type, "LCD_OTA_END") == 0) {
    JsonObject obj = doc.as<JsonObject>();
    lcd_ota_handle_end(obj);
    return;
  } else if (strcmp(type, "LCD_OTA_ABORT") == 0) {
    JsonObject obj = doc.as<JsonObject>();
    lcd_ota_handle_abort(obj);
    return;
  } else {
    Serial.printf("[PROTO] Unknown type: %s\n", type);
  }
}

#endif // LCD_UART_RX_H

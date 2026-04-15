/*
 * lcd_ship_action.h
 *
 * Menu hit-test, action dispatch, meal result handling,
 * toast events, OTA menu, fw-info polling, and create_custom_ui.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 14.
 *
 * Prerequisites: all globals, LVGL, lcd_ship_route.h
 *   must be included before this header.
 */

#ifndef LCD_SHIP_ACTION_H
#define LCD_SHIP_ACTION_H

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

#endif // LCD_SHIP_ACTION_H

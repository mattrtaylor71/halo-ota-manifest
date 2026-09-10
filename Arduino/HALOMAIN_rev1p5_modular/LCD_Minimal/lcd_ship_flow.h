/*
 * lcd_ship_flow.h
 *
 * Animations, timeout rings, screen init/show functions,
 * processing progress, and phase inference helpers.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 12.
 *
 * Prerequisites: all globals, LVGL, lcd_ship_screens.h
 *   must be included before this header.
 */

#ifndef LCD_SHIP_FLOW_H
#define LCD_SHIP_FLOW_H

static lv_obj_t* ship_hold_badge = NULL;
static lv_obj_t* ship_choice_badge = NULL;
static lv_obj_t* ship_choice_question = NULL;
static bool ship_choice_returning_from_date = false;

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
  (void)obj;
  (void)v; // No layer-backed transformations in the production UI.
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
  if (ship_ai_listening_hint)
    lv_label_set_text(ship_ai_listening_hint,
                      long_press_sent ? "Release to send" : "Keep holding to speak");
}

static void ship_start_logged_success_animation() {
  halo_ui_reward_start(ship_logged_icon);
}

static void ship_start_voice_ack_animation() {
  halo_ui_reward_start(ship_voice_ack_icon);
}

static void ship_update_hold_still_countdown() {
  if (!ship_hold_countdown_label || ship_hold_anim_start_ms == 0)
    return;
  unsigned long elapsed = millis() - ship_hold_anim_start_ms;
  bool capturing = elapsed >= SHIP_HOLD_COUNTDOWN_MS;
  if (ship_hold_ring) {
    uint32_t value;
    if (!capturing)
      value = (SHIP_HOLD_COUNTDOWN_MS - elapsed) * 1000UL / SHIP_HOLD_COUNTDOWN_MS;
    else {
      uint32_t p = (elapsed - SHIP_HOLD_COUNTDOWN_MS) % SHIP_HOLD_CAPTURE_PULSE_MS;
      uint32_t h = SHIP_HOLD_CAPTURE_PULSE_MS / 2;
      value = p < h ? 280 + p * 720 / h : 1000 - (p - h) * 720 / h;
    }
    lv_arc_set_value(ship_hold_ring, value);
  }
  if (!capturing) {
    int count = 3 - (int)(elapsed / 1000UL);
    if (count < 1)
      count = 1;
    if (count != ship_hold_countdown_value) {
      ship_hold_countdown_value = count;
      char text[4];
      snprintf(text, sizeof(text), "%d", count);
      lv_label_set_text(ship_hold_countdown_label, text);
      halo_ui_digit_pulse(ship_hold_countdown_label);
    }
    return;
  }
  if (!ship_hold_capture_phase) {
    ship_hold_capture_phase = true;
    ship_hold_countdown_value = -1;
    lv_obj_add_flag(ship_hold_countdown_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ship_hold_title, "Taking photo");
    lv_label_set_text(ship_hold_subtitle, "Capturing your item...");
  }
  // Actual capture-phase pulse is the ring. The vector stays crisp and static.
}

static void ship_start_hold_still_animation() {
  if (!ship_hold_screen)
    return;
  halo_ui_motion_stop(ship_hold_screen);
  ship_hold_anim_start_ms = millis();
  ship_hold_countdown_value = -1;
  ship_hold_capture_frame = -1;
  ship_hold_capture_phase = false;
  lv_label_set_text(ship_hold_title, "Hold still");
  lv_label_set_text(ship_hold_subtitle, "Keep your item in place");
  lv_obj_clear_flag(ship_hold_countdown_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
  if (ship_hold_badge) {
    const char* task = ship_mode_is_dish(g_ship_ui_op, g_ship_ui_mode) ? "LOG MEAL"
                       : ship_choice_mode_is_discard()                 ? "DISCARD"
                                                                       : "ADD FOOD";
    lv_label_set_text(lv_obj_get_child(ship_hold_badge, 0), task);
  }
  ship_update_hold_still_countdown();
}

static void ship_start_expiry_choice_animation() {
  // Keep the original input deadline and immediate, stable hit targets.
  // No container opacity or sliding controls while a user is selecting.
}

static void ship_init_hold_still() {
  if (ship_hold_screen)
    return;
  ship_hold_screen = halo_ui_page();
  ship_hold_badge = halo_ui_badge(ship_hold_screen, "ADD FOOD", 20);
  ship_hold_title =
      halo_ui_label(ship_hold_screen, "Hold still", &nunito_28, COL_DARK, 45, 60, 270);
  ship_hold_ring = halo_ui_ring(ship_hold_screen, 114, 112, 132, 7, COL_GREEN);
  halo_ui_card(ship_hold_screen, 126, 124, 108, 108, COL_WHITE, 54);
  ship_hold_countdown_label =
      halo_ui_label(ship_hold_screen, "3", &nunito_60, COL_GREEN, 130, 156, 100);
  ship_hold_capture_icon =
      halo_ui_icon(ship_hold_screen, HALO_ICON_CAMERA, 159, 157, 42, COL_GREEN);
  lv_obj_add_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
  ship_hold_subtitle = halo_ui_label(ship_hold_screen, "Keep your item in place",
                                     &lv_font_montserrat_16, COL_TEXT2, 40, 272, 280);
}

static void ship_init_expiry_choice() {
  if (ship_expiry_choice_screen)
    return;
  ship_expiry_choice_screen = halo_ui_page();
  ship_expiry_choice_timeout_ring = halo_ui_ring(ship_expiry_choice_screen, 6, 6, 348, 4, COL_GOLD);
  lv_obj_set_style_arc_opa(ship_expiry_choice_timeout_ring, LV_OPA_TRANSP, LV_PART_MAIN);
  ship_choice_badge = halo_ui_badge(ship_expiry_choice_screen, "ADD FOOD", 22, 110);
  ship_expiry_choice_qty_prefix =
      halo_ui_label(ship_expiry_choice_screen, "QUANTITY", &nunito_12, COL_MUTED, 70, 64, 220);
  expiry_choice_quantity_label =
      halo_ui_label(ship_expiry_choice_screen, "1", &nunito_60, COL_GREEN, 60, 92, 240);
  ship_expiry_choice_prompt = halo_ui_label(ship_expiry_choice_screen, "Turn the dial to adjust",
                                            &lv_font_montserrat_14, COL_TEXT2, 40, 160, 280);
  ship_choice_question = halo_ui_label(ship_expiry_choice_screen, "Add an expiry date?", &nunito_18,
                                       COL_DARK, 40, 192, 280);
  ship_expiry_choice_skip_btn =
      halo_ui_button(ship_expiry_choice_screen, 54, 232, 118, 54, "NO DATE", COL_WHITE, COL_DARK);
  ship_expiry_choice_skip_label = lv_obj_get_child(ship_expiry_choice_skip_btn, 0);
  ship_expiry_choice_add_btn =
      halo_ui_button(ship_expiry_choice_screen, 188, 232, 118, 54, "ADD DATE");
  ship_expiry_choice_add_label = lv_obj_get_child(ship_expiry_choice_add_btn, 0);
}

static void ship_init_processing() {
  if (ship_processing_screen)
    return;
  ship_processing_screen = halo_ui_page();
  ship_processing_fill = NULL;
  ship_processing_halo = NULL;
  ship_processing_spinner = halo_ui_spinner(ship_processing_screen, 64);
  ship_processing_label =
      halo_ui_label(ship_processing_screen, "Working on it", &nunito_24, COL_DARK, 35, 172, 290);
  ship_processing_subtitle = halo_ui_label(ship_processing_screen, "Sending your request to Trepo.",
                                           &lv_font_montserrat_16, COL_TEXT2, 45, 212, 270);
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
  (void)pct; // Elapsed time is not backend progress; show an indeterminate arc.
}

static void ship_set_processing_text(const char* text) {
  if (!ship_processing_label)
    return;
  lv_label_set_text(ship_processing_label,
                    (text && text[0] && strcmp(text, "Processing") != 0) ? text : "Working on it");
}

static void ship_configure_processing_layout(bool is_dish) {
  if (!ship_processing_screen)
    return;
  lv_label_set_text(ship_processing_label, is_dish ? "Analyzing your meal" : "Working on it");
  lv_label_set_text(ship_processing_subtitle,
                    is_dish ? "Finding what's on the plate." : "Sending your request to Trepo.");
}

static void ship_start_processing_animation() {
  if (ship_processing_spinner)
    halo_ui_spinner_start(ship_processing_spinner);
}

static void ship_init_logged() {
  if (ship_logged_screen)
    return;
  ship_logged_screen = halo_ui_page();
  ship_logged_icon = halo_ui_seal(ship_logged_screen);
  ship_logged_label =
      halo_ui_label(ship_logged_screen, "Got it!", &nunito_28, COL_DARK, 40, 166, 280);
  ship_logged_subtitle = halo_ui_label(ship_logged_screen, "Sending to your kitchen...",
                                       &lv_font_montserrat_16, COL_TEXT2, 36, 210, 288);
  halo_ui_back(ship_logged_screen);
}

static void ship_init_voice_ack() {
  if (ship_voice_ack_screen)
    return;
  ship_voice_ack_screen = halo_ui_page();
  ship_voice_ack_icon = halo_ui_seal(ship_voice_ack_screen);
  ship_voice_ack_label =
      halo_ui_label(ship_voice_ack_screen, "On it!", &nunito_28, COL_DARK, 40, 166, 280);
  ship_voice_ack_subtitle = halo_ui_label(ship_voice_ack_screen, "Working on your request.",
                                          &lv_font_montserrat_16, COL_TEXT2, 36, 210, 288);
  halo_ui_back(ship_voice_ack_screen);
}

static void ship_init_error() {
  if (ship_error_screen)
    return;
  ship_error_screen = halo_ui_page();
  lv_obj_t* seal = halo_ui_card(ship_error_screen, 142, 44, 76, 76, COL_RED, 38);
  halo_ui_icon(seal, HALO_ICON_WARNING, 16, 16, 40, COL_WHITE);
  ship_error_label =
      halo_ui_label(ship_error_screen, "Something went wrong", &nunito_22, COL_DARK, 38, 152, 284);
  halo_ui_label(ship_error_screen, "Please try again.", &lv_font_montserrat_16, COL_TEXT2, 45, 190,
                270);
  halo_ui_button(ship_error_screen, 96, 240, 168, 54, "HOME");
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
  // Preserve the operation/watchdog clocks elsewhere. This presentation never
  // converts an elapsed timer into a fabricated backend percentage.
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
  // Sense has responded; we're on a user-choice screen with its own 30s timeout.
  // Clear the capture/processing watchdogs so they can't race this screen's
  // graceful timeout and throw an error screen.
  waiting_for_scan_response = false;
  scan_request_sent_ms = 0;
  dish_processing_active = false;
  dish_processing_start_ms = 0;
  expiry_submitted = false;
  if (!ship_choice_returning_from_date)
    ship_expiry_choice_shown_time = millis();
  ship_update_expiry_choice_timeout_ring();
  if (!ship_choice_mode_is_discard() && !ship_choice_returning_from_date) {
    expiry_choice_quantity = 1;
    expiry_choice_update_quantity_label();
  }
  ship_choice_returning_from_date = false;
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
  unsigned long original_choice_start = ship_expiry_choice_shown_time;
  ship_expiry_choice_shown_time = 0;
  expiry_prepare_picker_for_entry();
  if (original_choice_start)
    expiry_screen_shown_time = original_choice_start;
  lv_obj_clear_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
  ui_screen_state = SCREEN_EXPIRY;
  ui_busy = true;
  ship_logged_hide_at_ms = 0;
  ship_error_hide_at_ms = 0;
  // Sense has responded; we're on a user-choice screen with its own 30s timeout.
  // Clear the capture/processing watchdogs so they can't race this screen's
  // graceful timeout and throw an error screen.
  waiting_for_scan_response = false;
  scan_request_sent_ms = 0;
  dish_processing_active = false;
  dish_processing_start_ms = 0;
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

#endif // LCD_SHIP_FLOW_H

/*
 * lcd_ship_route.h
 *
 * Screen routing (ui_show_screen), touch handlers, result/debug
 * screens, expiry choice logic, and ui_apply_ship_ui_status.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 13.
 *
 * Prerequisites: all globals, LVGL, lcd_ship_flow.h
 *   must be included before this header.
 */

#ifndef LCD_SHIP_ROUTE_H
#define LCD_SHIP_ROUTE_H

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
    case SCREEN_SHIP_ERRLOG:
      show_errlog_screen();
      break;
    case SCREEN_SHIP_SHOPPING_LIST:
      show_shopping_list_screen_impl();
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

#endif // LCD_SHIP_ROUTE_H

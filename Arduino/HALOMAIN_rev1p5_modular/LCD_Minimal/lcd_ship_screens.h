/*
 * lcd_ship_screens.h
 *
 * Ship main-menu, second-menu, settings, voice/AI UI,
 * and overlay screen builders.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 11.
 *
 * Prerequisites: all globals, LVGL, ship types, lcd_anim.h,
 *   lcd_provision.h must be included before this header.
 */

#ifndef LCD_SHIP_SCREENS_H
#define LCD_SHIP_SCREENS_H

static int ship_main_menu_button_index_for_action(ship_menu_action_t action) {
  switch (action) {
    case SHIP_MENU_ACTION_LOG_DISH: return 0;
    case SHIP_MENU_ACTION_CHECK_IN: return 1;
    case SHIP_MENU_ACTION_CHECK_OUT: return 2;
    case SHIP_MENU_ACTION_MORE: return 3;
    default: return -1;
  }
}

static bool ship_main_menu_is_ai_action(const ship_menu_hitbox_t* hb) {
  return hb != NULL && hb->action == SHIP_MENU_ACTION_AI;
}

static int ship_main_menu_button_target_y(int index) {
  switch (index) {
    case 0: return SHIP_MAIN_MENU_TOP_BTN_Y;
    case 1: return SHIP_MAIN_MENU_LEFT_BTN_Y;
    case 2: return SHIP_MAIN_MENU_RIGHT_BTN_Y;
    case 3: return SHIP_MAIN_MENU_BOTTOM_BTN_Y;
    default: return 0;
  }
}

static void ship_main_menu_style_button(lv_obj_t* btn, bool primary) {
  if (!btn) {
    return;
  }
  lv_obj_set_size(btn, SHIP_MAIN_MENU_BTN_W, SHIP_MAIN_MENU_BTN_H);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(btn, SHIP_MAIN_MENU_BTN_RADIUS, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn,
                            primary ? lv_color_hex(0x6EE7B7) : lv_color_hex(0x16335E),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, primary ? 0 : 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x355D93), LV_PART_MAIN);
  lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
}

static void ship_main_menu_set_ai_hold_active(bool active) {
  if (!ship_main_menu_ai_button || !ship_main_menu_ai_label) {
    return;
  }
  lv_obj_set_style_bg_color(ship_main_menu_ai_button,
                            active ? lv_color_hex(0x245DFF) : lv_color_hex(0x6EE7B7),
                            LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_main_menu_ai_button, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_main_menu_ai_button, active ? 18 : 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(ship_main_menu_ai_button, lv_color_hex(0x245DFF), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(ship_main_menu_ai_button, active ? LV_OPA_60 : LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_main_menu_ai_label,
                              active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x0E2547),
                              LV_PART_MAIN);
}

static void ship_init_ai_listening_screen() {
  if (ship_ai_listening_screen) {
    return;
  }

  ship_ai_listening_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_ai_listening_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_ai_listening_screen, LV_OBJ_FLAG_SCROLLABLE);
  ship_style_plain_screen(ship_ai_listening_screen, lv_color_hex(0x6EE7B7));
  lv_obj_set_style_border_width(ship_ai_listening_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_ai_listening_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_screen, 0, LV_PART_MAIN);

  ship_ai_listening_ring = lv_arc_create(ship_ai_listening_screen);
  lv_obj_remove_style(ship_ai_listening_ring, NULL, LV_PART_KNOB);
  lv_obj_set_size(ship_ai_listening_ring, 164, 164);
  lv_obj_align(ship_ai_listening_ring, LV_ALIGN_CENTER, 0, -8);
  lv_arc_set_range(ship_ai_listening_ring, 0, 1000);
  lv_arc_set_value(ship_ai_listening_ring, 1000);
  lv_arc_set_bg_angles(ship_ai_listening_ring, 0, 360);
  lv_arc_set_rotation(ship_ai_listening_ring, 270);
  lv_obj_set_style_arc_width(ship_ai_listening_ring, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_ai_listening_ring, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_ai_listening_ring, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ship_ai_listening_ring, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_ai_listening_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_outline_width(ship_ai_listening_ring, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_ai_listening_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  ship_ai_listening_mic_head = lv_obj_create(ship_ai_listening_screen);
  lv_obj_set_size(ship_ai_listening_mic_head, 48, 70);
  lv_obj_align(ship_ai_listening_mic_head, LV_ALIGN_CENTER, 0, -18);
  lv_obj_set_style_radius(ship_ai_listening_mic_head, 24, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_head, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_head, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_head, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_head, 0, LV_PART_MAIN);

  ship_ai_listening_mic_stem = lv_obj_create(ship_ai_listening_screen);
  lv_obj_set_size(ship_ai_listening_mic_stem, 8, 28);
  lv_obj_align(ship_ai_listening_mic_stem, LV_ALIGN_CENTER, 0, 36);
  lv_obj_set_style_radius(ship_ai_listening_mic_stem, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_stem, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_stem, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_stem, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_stem, 0, LV_PART_MAIN);

  ship_ai_listening_mic_base = lv_obj_create(ship_ai_listening_screen);
  lv_obj_set_size(ship_ai_listening_mic_base, 44, 6);
  lv_obj_align(ship_ai_listening_mic_base, LV_ALIGN_CENTER, 0, 56);
  lv_obj_set_style_radius(ship_ai_listening_mic_base, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_base, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_base, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_base, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_base, 0, LV_PART_MAIN);

  ship_ai_listening_title = lv_label_create(ship_ai_listening_screen);
  lv_label_set_text(ship_ai_listening_title, "Listening");
  lv_obj_set_style_text_font(ship_ai_listening_title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_ai_listening_title, lv_color_hex(0x0E2547), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_ai_listening_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_ai_listening_title, LV_ALIGN_CENTER, 0, -136);

  ship_ai_listening_hint = lv_label_create(ship_ai_listening_screen);
  lv_label_set_text(ship_ai_listening_hint, "Release to return");
  lv_obj_set_style_text_font(ship_ai_listening_hint, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_ai_listening_hint, lv_color_hex(0x16335E), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_ai_listening_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_ai_listening_hint, LV_ALIGN_CENTER, 0, -102);
}

static void ship_start_ai_listening_animation() {
  if (!ship_ai_listening_ring || !ship_ai_listening_mic_head || !ship_ai_listening_mic_stem ||
      !ship_ai_listening_mic_base || !ship_ai_listening_title || !ship_ai_listening_hint) {
    return;
  }

  lv_anim_del(ship_ai_listening_ring, NULL);
  lv_anim_del(ship_ai_listening_mic_head, NULL);
  lv_anim_del(ship_ai_listening_mic_stem, NULL);
  lv_anim_del(ship_ai_listening_mic_base, NULL);
  lv_anim_del(ship_ai_listening_title, NULL);
  lv_anim_del(ship_ai_listening_hint, NULL);

  lv_obj_set_style_opa(ship_ai_listening_mic_head, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_opa(ship_ai_listening_mic_stem, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_opa(ship_ai_listening_mic_base, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_ai_listening_title, -118);
  lv_obj_set_style_text_opa(ship_ai_listening_title, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_ai_listening_hint, -84);
  lv_obj_set_style_text_opa(ship_ai_listening_hint, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t mic_head_fade;
  lv_anim_init(&mic_head_fade);
  lv_anim_set_var(&mic_head_fade, ship_ai_listening_mic_head);
  lv_anim_set_values(&mic_head_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&mic_head_fade, 220);
  lv_anim_set_delay(&mic_head_fade, 70);
  lv_anim_set_exec_cb(&mic_head_fade, ship_anim_set_obj_opa);
  lv_anim_start(&mic_head_fade);

  lv_anim_t mic_stem_fade;
  lv_anim_init(&mic_stem_fade);
  lv_anim_set_var(&mic_stem_fade, ship_ai_listening_mic_stem);
  lv_anim_set_values(&mic_stem_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&mic_stem_fade, 220);
  lv_anim_set_delay(&mic_stem_fade, 100);
  lv_anim_set_exec_cb(&mic_stem_fade, ship_anim_set_obj_opa);
  lv_anim_start(&mic_stem_fade);

  lv_anim_t mic_base_fade;
  lv_anim_init(&mic_base_fade);
  lv_anim_set_var(&mic_base_fade, ship_ai_listening_mic_base);
  lv_anim_set_values(&mic_base_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&mic_base_fade, 220);
  lv_anim_set_delay(&mic_base_fade, 130);
  lv_anim_set_exec_cb(&mic_base_fade, ship_anim_set_obj_opa);
  lv_anim_start(&mic_base_fade);

  lv_anim_t title_fade;
  lv_anim_init(&title_fade);
  lv_anim_set_var(&title_fade, ship_ai_listening_title);
  lv_anim_set_values(&title_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&title_fade, 240);
  lv_anim_set_delay(&title_fade, 60);
  lv_anim_set_exec_cb(&title_fade, ship_anim_set_text_opa);
  lv_anim_start(&title_fade);

  lv_anim_t title_y;
  lv_anim_init(&title_y);
  lv_anim_set_var(&title_y, ship_ai_listening_title);
  lv_anim_set_values(&title_y, -118, -136);
  lv_anim_set_time(&title_y, 300);
  lv_anim_set_delay(&title_y, 60);
  lv_anim_set_path_cb(&title_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&title_y, ship_anim_set_y);
  lv_anim_start(&title_y);

  lv_anim_t hint_fade;
  lv_anim_init(&hint_fade);
  lv_anim_set_var(&hint_fade, ship_ai_listening_hint);
  lv_anim_set_values(&hint_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&hint_fade, 220);
  lv_anim_set_delay(&hint_fade, 120);
  lv_anim_set_exec_cb(&hint_fade, ship_anim_set_text_opa);
  lv_anim_start(&hint_fade);

  lv_anim_t hint_y;
  lv_anim_init(&hint_y);
  lv_anim_set_var(&hint_y, ship_ai_listening_hint);
  lv_anim_set_values(&hint_y, -84, -102);
  lv_anim_set_time(&hint_y, 300);
  lv_anim_set_delay(&hint_y, 120);
  lv_anim_set_path_cb(&hint_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&hint_y, ship_anim_set_y);
  lv_anim_start(&hint_y);

}

static void ship_show_ai_listening_screen() {
  ship_init_ai_listening_screen();
  ship_ai_listening_countdown_start_ms = 0;
  ship_update_ai_listening_countdown();
  ui_screen_state = SCREEN_AI_LISTENING;
  ui_busy = false;
  lv_scr_load(ship_ai_listening_screen);
  ship_start_ai_listening_animation();
  Serial.println("[AI] screen=LISTENING");
  lv_timer_handler();
}

static void ship_queue_voice_input(const char* type, const char* wake_reason) {
  if (!type || !type[0]) {
    return;
  }
  if (strcmp(type, "INPUT_LONG_PRESS_START") == 0) {
    g_voice_fire_and_forget_ignore_ui = false;
    waiting_for_voice_response = false;
    voice_response_deadline_ms = 0;
    g_ship_voice_json_pending = false;
    g_ship_voice_json_text[0] = '\0';
    stop_glowing_animation();
  }
  if (wake_reason && wake_reason[0]) {
    request_sense_wake(wake_reason);
  }
  tx_msg_t tx_msg = {};
  strncpy(tx_msg.type, type, sizeof(tx_msg.type) - 1);
  if (uart_tx_queue != NULL) {
    xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
  }
  Serial.printf("[AI] queued type=%s wake_reason=%s\n",
                type,
                wake_reason ? wake_reason : "");
}

static void ship_service_voice_end_resend(unsigned long now) {
  if (ship_voice_end_resends_remaining == 0 || now < ship_voice_end_resend_due_ms) {
    return;
  }
  ship_queue_voice_input("INPUT_LONG_PRESS_END", NULL);
  ship_voice_end_resends_remaining--;
  ship_voice_end_resend_due_ms = now + 220;
  Serial.printf("[AI] resend INPUT_LONG_PRESS_END remaining=%u\n",
                (unsigned)ship_voice_end_resends_remaining);
}

static void ship_voice_ui_copy(char* dst, size_t dst_size, const char* src) {
  if (!dst || dst_size == 0) {
    return;
  }
  const unsigned char* s = (const unsigned char*)(src ? src : "");
  size_t out = 0;
  while (*s && out < dst_size - 1) {
    if (*s < 0x80) {
      char c = (char)(*s++);
      if (c == '\r') {
        continue;
      }
      if (c == '\t') {
        c = ' ';
      } else if (((unsigned char)c) < 0x20 && c != '\n') {
        c = ' ';
      }
      dst[out++] = c;
      continue;
    }

    if (s[0] == 0xC2 && s[1] == 0xA0) {  // non-breaking space
      dst[out++] = ' ';
      s += 2;
      continue;
    }
    if (s[0] == 0xE2 && s[1] == 0x80) {
      unsigned char third = s[2];
      if (third == 0x93 || third == 0x94) {  // en/em dash
        dst[out++] = '-';
        s += 3;
        continue;
      }
      if (third == 0xA6) {  // ellipsis
        if (out < dst_size - 3) {
          dst[out++] = '.';
          dst[out++] = '.';
          dst[out++] = '.';
        } else {
          dst[out++] = '.';
        }
        s += 3;
        continue;
      }
      if (third == 0x98 || third == 0x99) {  // curly apostrophes
        dst[out++] = '\'';
        s += 3;
        continue;
      }
      if (third == 0x9C || third == 0x9D) {  // curly quotes
        dst[out++] = '"';
        s += 3;
        continue;
      }
      if (third == 0xA2) {  // bullet
        dst[out++] = '-';
        s += 3;
        continue;
      }
    }

    int skip = 1;
    if ((*s & 0xE0) == 0xC0) {
      skip = 2;
    } else if ((*s & 0xF0) == 0xE0) {
      skip = 3;
    } else if ((*s & 0xF8) == 0xF0) {
      skip = 4;
    }
    if (out == 0 || dst[out - 1] != ' ') {
      dst[out++] = ' ';
    }
    s += skip;
  }
  dst[out] = '\0';
}

static bool ship_voice_ui_title_is_generic(const char* title) {
  if (!title || !title[0]) {
    return true;
  }
  if (strcmp(title, "Assistant") == 0 ||
      strcmp(title, "Assistant 2") == 0 ||
      strcmp(title, "Assistant 3") == 0 ||
      strcmp(title, "Assistant 4") == 0 ||
      strcmp(title, "Assistant 5") == 0 ||
      strcmp(title, "Assistant 6") == 0) {
    return true;
  }
  if (strcmp(title, "Voice Response") == 0) {
    return true;
  }
  return false;
}

static void ship_voice_ui_append(char* dst, size_t dst_size, const char* text, const char* separator) {
  if (!dst || dst_size == 0 || !text || !text[0]) {
    return;
  }
  size_t used = strlen(dst);
  if (used >= dst_size - 1) {
    return;
  }
  if (used > 0 && separator && separator[0]) {
    strncat(dst, separator, dst_size - strlen(dst) - 1);
  }
  strncat(dst, text, dst_size - strlen(dst) - 1);
}

static const char* ship_voice_ui_fallback_title(const char* response_type,
                                                bool error_present,
                                                size_t quick_item_count) {
  if (error_present) {
    return "Voice Error";
  }
  if (response_type && strcmp(response_type, "clarification_needed") == 0) {
    return "Question";
  }
  if (quick_item_count > 0 || (response_type && strcmp(response_type, "action_ack") == 0)) {
    return "Done";
  }
  if (response_type &&
      (strcmp(response_type, "info_answer") == 0 || strcmp(response_type, "paged_answer") == 0)) {
    return "Answer";
  }
  return "Voice Response";
}

static uint8_t ship_voice_ui_paginate_text(const char* text,
                                           const char* base_title,
                                           const char* style,
                                           const char* footer) {
  if (!text || !text[0]) {
    return 0;
  }

  const size_t kFallbackPageChars = 220;
  const size_t text_len = strlen(text);
  size_t start = 0;
  uint8_t page_count = 0;

  while (start < text_len && page_count < SHIP_VOICE_UI_MAX_PAGES) {
    while (start < text_len && (text[start] == '\n' || text[start] == ' ')) {
      start++;
    }
    if (start >= text_len) {
      break;
    }

    size_t end = start + kFallbackPageChars;
    if (end < text_len) {
      size_t best = end;
      for (size_t i = end; i > start + (kFallbackPageChars / 2); --i) {
        char c = text[i];
        if (c == '\n' || c == '.' || c == ';' || c == ',' || c == ' ') {
          best = (c == ' ') ? i : (i + 1);
          break;
        }
      }
      end = best;
    } else {
      end = text_len;
    }

    while (end > start && (text[end - 1] == '\n' || text[end - 1] == ' ')) {
      end--;
    }
    if (end <= start) {
      break;
    }

    ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[page_count];
    page->valid = true;
    ship_voice_ui_copy(page->template_name, sizeof(page->template_name), "title_body");
    ship_voice_ui_copy(page->style, sizeof(page->style), style && style[0] ? style : "default");
    ship_voice_ui_copy(page->title, sizeof(page->title),
                       page_count == 0 ? base_title : "More");

    char chunk[512];
    size_t chunk_len = end - start;
    if (chunk_len >= sizeof(chunk)) {
      chunk_len = sizeof(chunk) - 1;
    }
    memcpy(chunk, text + start, chunk_len);
    chunk[chunk_len] = '\0';
    ship_voice_ui_copy(page->body, sizeof(page->body), chunk);

    if (page_count == 0 && footer && footer[0]) {
      ship_voice_ui_copy(page->footer, sizeof(page->footer), footer);
    }

    page_count++;
    start = end;
  }

  return page_count;
}

static void ship_voice_ui_reset() {
  memset(g_ship_voice_ui_pages, 0, sizeof(g_ship_voice_ui_pages));
  g_ship_voice_ui_page_count = 0;
  g_ship_voice_ui_page_index = 0;
  g_ship_voice_ui_wrap = false;
  g_ship_voice_ui_show_page_dots = true;
  g_ship_voice_ui_show_page_count = false;
  g_ship_voice_ui_structured = false;
}

static bool ship_voice_ui_add_item(ship_voice_ui_page_t* page, const char* text) {
  if (!page || !text || !text[0] || page->item_count >= SHIP_VOICE_UI_MAX_ITEMS) {
    return false;
  }
  ship_voice_ui_copy(page->items[page->item_count], sizeof(page->items[page->item_count]), text);
  page->item_count++;
  return true;
}

static void ship_voice_ui_parse_items(JsonArray items, ship_voice_ui_page_t* page) {
  if (items.isNull() || !page) {
    return;
  }
  for (JsonVariant v : items) {
    if (page->item_count >= SHIP_VOICE_UI_MAX_ITEMS) {
      break;
    }
    if (v.is<const char*>()) {
      ship_voice_ui_add_item(page, v.as<const char*>());
    } else if (v.is<JsonObject>()) {
      JsonObject item = v.as<JsonObject>();
      const char* text = item["text"] | "";
      if (!text[0]) {
        text = item["item"] | "";
      }
      ship_voice_ui_add_item(page, text);
    }
  }
}

static void ship_voice_ui_parse_blocks(JsonArray blocks, ship_voice_ui_page_t* page) {
  if (blocks.isNull() || !page) {
    return;
  }
  for (JsonObject block : blocks) {
    const char* block_type = block["type"] | "";
    if (strcmp(block_type, "heading") == 0) {
      const char* text = block["text"] | "";
      if (!page->title[0]) {
        ship_voice_ui_copy(page->title, sizeof(page->title), text);
      } else {
        ship_voice_ui_append(page->body, sizeof(page->body), text, "\n\n");
      }
    } else if (strcmp(block_type, "paragraph") == 0) {
      ship_voice_ui_append(page->body, sizeof(page->body), block["text"] | "", "\n\n");
    } else if (strcmp(block_type, "footer") == 0) {
      ship_voice_ui_copy(page->footer, sizeof(page->footer), block["text"] | "");
    } else if (strcmp(block_type, "list") == 0) {
      ship_voice_ui_parse_items(block["items"].as<JsonArray>(), page);
    }
  }
}

static bool ship_voice_ui_footer_is_chrome(const char* footer) {
  if (!footer || !footer[0]) {
    return true;
  }
  if (strncmp(footer, "Page ", 5) == 0 && strstr(footer, " of ") != NULL) {
    return true;
  }
  if (strstr(footer, "Turn knob") != NULL ||
      strstr(footer, "Tap to") != NULL ||
      strstr(footer, "Swipe") != NULL) {
    return true;
  }
  return false;
}

static const char* ship_voice_ui_generic_error_text() {
  return "Sorry, something went wrong.";
}

static const char* ship_voice_ui_generic_format_text() {
  return "Sorry, I couldn't format that response.";
}

static void ship_voice_ui_parse_screen(JsonObject screen,
                                       ship_voice_ui_page_t* page,
                                       const char* fallback_title) {
  if (screen.isNull() || !page) {
    return;
  }
  page->valid = true;
  ship_voice_ui_copy(page->id, sizeof(page->id), screen["id"] | "");
  ship_voice_ui_copy(page->template_name, sizeof(page->template_name), screen["template"] | "title_body");
  ship_voice_ui_copy(page->style, sizeof(page->style), screen["style"] | "default");
  const char* screen_title = screen["title"] | "";
  const char* preferred_title = screen_title[0] ? screen_title : (fallback_title ? fallback_title : "");
  if (ship_voice_ui_title_is_generic(preferred_title)) {
    preferred_title = "Response";
  }
  ship_voice_ui_copy(page->title, sizeof(page->title), preferred_title);
  ship_voice_ui_copy(page->subtitle, sizeof(page->subtitle), screen["subtitle"] | "");
  ship_voice_ui_copy(page->body, sizeof(page->body), screen["body"] | "");
  ship_voice_ui_copy(page->footer, sizeof(page->footer), screen["footer"] | "");
  ship_voice_ui_parse_items(screen["items"].as<JsonArray>(), page);
  ship_voice_ui_parse_blocks(screen["blocks"].as<JsonArray>(), page);
}

static int ship_voice_ui_layout_label(lv_obj_t* label,
                                      const char* text,
                                      int y,
                                      int gap_after) {
  if (!label) {
    return y;
  }
  if (!text || !text[0]) {
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return y;
  }
  lv_label_set_text(label, text);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_update_layout(ship_voice_json_card);
  return y + lv_obj_get_height(label) + gap_after;
}

static void ship_voice_ui_build_fallback_page(JsonObject doc) {
  JsonVariant error = doc["error"];
  JsonArray quick_items = doc["quickItems"].as<JsonArray>();
  const char* response_type = doc["type"] | "";
  const char* text = doc["text"] | "";
  const char* transcript = doc["transcript"] | "";
  bool error_present = !error.isNull();
  const char* title = ship_voice_ui_fallback_title(response_type, error_present, quick_items.size());
  const char* style = error_present ? "error" : "default";
  const char* body_text = NULL;

  if (!error.isNull() && error.is<const char*>()) {
    body_text = error.as<const char*>();
  } else if (text[0]) {
    body_text = text;
  } else if (error_present) {
    body_text = ship_voice_ui_generic_error_text();
  } else if (transcript[0]) {
    body_text = transcript;
  } else {
    body_text = ship_voice_ui_generic_format_text();
  }

  char footer[96] = {0};
  if (transcript[0] && body_text && strcmp(transcript, body_text) != 0) {
    ship_voice_ui_copy(footer, sizeof(footer), transcript);
  }

  uint8_t page_count = ship_voice_ui_paginate_text(body_text, title, style, footer);
  if (page_count == 0) {
    page_count = ship_voice_ui_paginate_text(ship_voice_ui_generic_format_text(), title, style, footer);
  }

  if (page_count == 0) {
    ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[0];
    page->valid = true;
    ship_voice_ui_copy(page->template_name, sizeof(page->template_name), "title_body");
    ship_voice_ui_copy(page->style, sizeof(page->style), style);
    ship_voice_ui_copy(page->title, sizeof(page->title), title);
    ship_voice_ui_copy(page->body, sizeof(page->body), ship_voice_ui_generic_format_text());
    page_count = 1;
  }

  if (page_count > 0) {
    ship_voice_ui_parse_items(quick_items, &g_ship_voice_ui_pages[0]);
  }
  g_ship_voice_ui_page_count = page_count;
  g_ship_voice_ui_show_page_dots = page_count > 1;
  g_ship_voice_ui_show_page_count = false;
}

static bool ship_voice_ui_parse_response(const char* json_text) {
  ship_voice_ui_reset();
  if (!json_text || !json_text[0]) {
    return false;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, json_text);
  if (error) {
    Serial.printf("[VOICE_UI] parse error=%s len=%u\n", error.c_str(), (unsigned)strlen(json_text));
    ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[0];
    page->valid = true;
    ship_voice_ui_copy(page->template_name, sizeof(page->template_name), "title_body");
    ship_voice_ui_copy(page->style, sizeof(page->style), "error");
    ship_voice_ui_copy(page->title, sizeof(page->title), "Voice Error");
    ship_voice_ui_copy(page->body, sizeof(page->body), ship_voice_ui_generic_format_text());
    g_ship_voice_ui_page_count = 1;
    g_ship_voice_ui_show_page_dots = false;
    g_ship_voice_ui_show_page_count = false;
    return false;
  }

  JsonObject ui = doc["ui"].as<JsonObject>();
  JsonArray screens = ui["screens"].as<JsonArray>();
  Serial.printf("[VOICE_UI] parsed type=%s ui=%d screens=%u\n",
                doc["type"] | "",
                ui.isNull() ? 0 : 1,
                screens.isNull() ? 0 : (unsigned)screens.size());
  if (!ui.isNull() && !screens.isNull() && screens.size() > 0) {
    const char* ui_title = ui["title"] | "";
    JsonObject navigation = ui["navigation"].as<JsonObject>();
    g_ship_voice_ui_wrap = navigation["wrap"] | false;
    g_ship_voice_ui_show_page_dots = navigation["showPageDots"] | true;
    g_ship_voice_ui_show_page_count = false;

    uint8_t page_count = screens.size();
    if (page_count > SHIP_VOICE_UI_MAX_PAGES) {
      page_count = SHIP_VOICE_UI_MAX_PAGES;
    }
    for (uint8_t i = 0; i < page_count; ++i) {
      JsonObject screen = screens[i].as<JsonObject>();
      ship_voice_ui_parse_screen(screen, &g_ship_voice_ui_pages[i], ui_title);
    }
    g_ship_voice_ui_page_count = page_count;
    g_ship_voice_ui_structured = true;
    return true;
  }

  ship_voice_ui_build_fallback_page(doc.as<JsonObject>());
  return true;
}

static void ship_voice_ui_apply_style(const ship_voice_ui_page_t* page) {
  lv_color_t screen_bg = lv_color_hex(0x0E2547);
  lv_color_t card_bg = lv_color_hex(0x16345E);
  lv_color_t accent = lv_color_hex(0x7DD3FC);

  if (page) {
    if (strcmp(page->style, "warning") == 0) {
      accent = lv_color_hex(0xFBBF24);
      card_bg = lv_color_hex(0x3A2A12);
    } else if (strcmp(page->style, "success") == 0) {
      accent = lv_color_hex(0x6EE7B7);
      card_bg = lv_color_hex(0x153E38);
    } else if (strcmp(page->style, "error") == 0) {
      accent = lv_color_hex(0xFCA5A5);
      card_bg = lv_color_hex(0x451A25);
      screen_bg = lv_color_hex(0x2B1020);
    } else if (strcmp(page->style, "info") == 0) {
      accent = lv_color_hex(0x93C5FD);
      card_bg = lv_color_hex(0x142C52);
    }
  }

  ship_style_plain_screen(ship_voice_json_screen, screen_bg);
  lv_obj_set_style_bg_opa(ship_voice_json_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_voice_json_card, 0, LV_PART_MAIN);

  lv_obj_set_style_text_color(ship_voice_json_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_subtitle, lv_color_hex(0xD6E4FF), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_label, lv_color_hex(0xF5F9FF), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_footer, accent, LV_PART_MAIN);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    if (ship_voice_json_item_labels[i]) {
      lv_obj_set_style_text_color(ship_voice_json_item_labels[i], lv_color_hex(0xEAF4FF), LV_PART_MAIN);
    }
  }
  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    if (!ship_voice_json_page_dots[i]) {
      continue;
    }
    bool active = (i == g_ship_voice_ui_page_index);
    lv_obj_set_style_bg_color(ship_voice_json_page_dots[i], active ? accent : lv_color_hex(0x4B6387), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_voice_json_page_dots[i], active ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
  }
}

static void ship_voice_ui_render_current_page() {
  ship_init_voice_json_screen();
  if (g_ship_voice_ui_page_count == 0 || g_ship_voice_ui_page_index >= g_ship_voice_ui_page_count) {
    return;
  }

  ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[g_ship_voice_ui_page_index];
  ship_voice_ui_apply_style(page);

  int current_y = 10;
  current_y = ship_voice_ui_layout_label(ship_voice_json_title,
                                         page->title[0] ? page->title : "Response",
                                         current_y,
                                         10);
  current_y = ship_voice_ui_layout_label(ship_voice_json_subtitle,
                                         page->subtitle,
                                         current_y,
                                         12);
  current_y = ship_voice_ui_layout_label(ship_voice_json_label,
                                         page->body,
                                         current_y,
                                         page->item_count > 0 ? 14 : 10);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    if (!ship_voice_json_item_labels[i]) {
      continue;
    }
    if (i < page->item_count) {
      char line[96];
      snprintf(line, sizeof(line), "- %s", page->items[i]);
      current_y = ship_voice_ui_layout_label(ship_voice_json_item_labels[i], line, current_y, 10);
    } else {
      lv_obj_add_flag(ship_voice_json_item_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (page->footer[0] && !ship_voice_ui_footer_is_chrome(page->footer)) {
    lv_label_set_text(ship_voice_json_footer, page->footer);
    lv_obj_clear_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(ship_voice_json_card);
    if ((current_y + lv_obj_get_height(ship_voice_json_footer)) <= 212) {
      lv_obj_align(ship_voice_json_footer, LV_ALIGN_TOP_MID, 0, current_y);
    } else {
      lv_obj_add_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    lv_obj_add_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);
  }

  if (g_ship_voice_ui_page_count > 1 && g_ship_voice_ui_show_page_count) {
    char page_text[24];
    snprintf(page_text, sizeof(page_text), "%u/%u",
             (unsigned)(g_ship_voice_ui_page_index + 1),
             (unsigned)g_ship_voice_ui_page_count);
    lv_label_set_text(ship_voice_json_page_label, page_text);
    lv_obj_align(ship_voice_json_page_label, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_clear_flag(ship_voice_json_page_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(ship_voice_json_page_label, LV_OBJ_FLAG_HIDDEN);
  }

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    if (!ship_voice_json_page_dots[i]) {
      continue;
    }
    if (i < g_ship_voice_ui_page_count && g_ship_voice_ui_page_count > 1 && g_ship_voice_ui_show_page_dots) {
      int x = ((int)i - ((int)g_ship_voice_ui_page_count - 1) / 2) * 16;
      lv_obj_align(ship_voice_json_page_dots[i], LV_ALIGN_BOTTOM_MID, x, -42);
      lv_obj_clear_flag(ship_voice_json_page_dots[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ship_voice_json_page_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  lv_obj_add_flag(ship_voice_json_hint, LV_OBJ_FLAG_HIDDEN);
}

static void ship_voice_ui_handle_scroll(int delta) {
  if (g_ship_voice_ui_page_count <= 1 || delta == 0) {
    return;
  }

  int next = (int)g_ship_voice_ui_page_index + (delta > 0 ? 1 : -1);
  if (next < 0) {
    next = g_ship_voice_ui_wrap ? (int)g_ship_voice_ui_page_count - 1 : 0;
  } else if (next >= g_ship_voice_ui_page_count) {
    next = g_ship_voice_ui_wrap ? 0 : (int)g_ship_voice_ui_page_count - 1;
  }
  if (next == g_ship_voice_ui_page_index) {
    return;
  }

  g_ship_voice_ui_page_index = (uint8_t)next;
  ship_voice_ui_render_current_page();
  Serial.printf("[VOICE_UI] page=%u/%u\n",
                (unsigned)(g_ship_voice_ui_page_index + 1),
                (unsigned)g_ship_voice_ui_page_count);
}

static void ship_init_voice_json_screen() {
  if (ship_voice_json_screen) {
    return;
  }

  ship_voice_json_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_voice_json_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_voice_json_screen, LV_OBJ_FLAG_SCROLLABLE);
  ship_style_plain_screen(ship_voice_json_screen, lv_color_hex(0x0E2547));
  lv_obj_set_style_border_width(ship_voice_json_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ship_voice_json_screen, 0, LV_PART_MAIN);

  ship_voice_json_card = lv_obj_create(ship_voice_json_screen);
  lv_obj_set_size(ship_voice_json_card, 236, 248);
  lv_obj_align(ship_voice_json_card, LV_ALIGN_CENTER, 0, -6);
  lv_obj_clear_flag(ship_voice_json_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_voice_json_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_voice_json_card, 0, LV_PART_MAIN);

  ship_voice_json_title = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_title, "Voice Response");
  lv_obj_set_style_text_font(ship_voice_json_title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_width(ship_voice_json_title, 228);
  lv_label_set_long_mode(ship_voice_json_title, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(ship_voice_json_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(ship_voice_json_title, 4, LV_PART_MAIN);
  lv_obj_align(ship_voice_json_title, LV_ALIGN_TOP_MID, 0, 18);

  ship_voice_json_page_label = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_page_label, "");
  lv_obj_set_style_text_font(ship_voice_json_page_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_add_flag(ship_voice_json_page_label, LV_OBJ_FLAG_HIDDEN);

  ship_voice_json_subtitle = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_subtitle, "");
  lv_obj_set_width(ship_voice_json_subtitle, 220);
  lv_label_set_long_mode(ship_voice_json_subtitle, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ship_voice_json_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_json_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(ship_voice_json_subtitle, 5, LV_PART_MAIN);
  lv_obj_align(ship_voice_json_subtitle, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_add_flag(ship_voice_json_subtitle, LV_OBJ_FLAG_HIDDEN);

  ship_voice_json_label = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_label, "");
  lv_obj_set_width(ship_voice_json_label, 216);
  lv_label_set_long_mode(ship_voice_json_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ship_voice_json_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_json_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(ship_voice_json_label, 8, LV_PART_MAIN);
  lv_obj_align(ship_voice_json_label, LV_ALIGN_TOP_MID, 0, 70);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    ship_voice_json_item_labels[i] = lv_label_create(ship_voice_json_card);
    lv_label_set_text(ship_voice_json_item_labels[i], "");
    lv_obj_set_width(ship_voice_json_item_labels[i], 204);
    lv_label_set_long_mode(ship_voice_json_item_labels[i], LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(ship_voice_json_item_labels[i], &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(ship_voice_json_item_labels[i], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(ship_voice_json_item_labels[i], 8, LV_PART_MAIN);
    lv_obj_add_flag(ship_voice_json_item_labels[i], LV_OBJ_FLAG_HIDDEN);
  }

  ship_voice_json_footer = lv_label_create(ship_voice_json_card);
  lv_label_set_text(ship_voice_json_footer, "");
  lv_obj_set_width(ship_voice_json_footer, 204);
  lv_label_set_long_mode(ship_voice_json_footer, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(ship_voice_json_footer, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_json_footer, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_line_space(ship_voice_json_footer, 4, LV_PART_MAIN);
  lv_obj_align(ship_voice_json_footer, LV_ALIGN_BOTTOM_MID, 0, -72);
  lv_obj_add_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    ship_voice_json_page_dots[i] = lv_obj_create(ship_voice_json_screen);
    lv_obj_set_size(ship_voice_json_page_dots[i], 8, 8);
    lv_obj_set_style_radius(ship_voice_json_page_dots[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_voice_json_page_dots[i], 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_voice_json_page_dots[i], 0, LV_PART_MAIN);
    lv_obj_add_flag(ship_voice_json_page_dots[i], LV_OBJ_FLAG_HIDDEN);
  }

  ship_voice_json_hint = lv_label_create(ship_voice_json_screen);
  lv_label_set_text(ship_voice_json_hint, "Tap to exit");
  lv_obj_set_style_text_font(ship_voice_json_hint, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_json_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_add_flag(ship_voice_json_hint, LV_OBJ_FLAG_HIDDEN);
}

static void ship_show_voice_json_screen(const char* json_text) {
  ship_init_voice_json_screen();
  strncpy(g_ship_voice_json_text, json_text ? json_text : "", sizeof(g_ship_voice_json_text) - 1);
  g_ship_voice_json_text[sizeof(g_ship_voice_json_text) - 1] = '\0';
  g_ship_voice_json_pending = false;
  bool parsed = ship_voice_ui_parse_response(g_ship_voice_json_text[0] ? g_ship_voice_json_text : "{}");
  ship_voice_ui_render_current_page();
  ui_screen_state = SCREEN_VOICE_JSON;
  ui_busy = true;
  lv_scr_load(ship_voice_json_screen);
  Serial.printf("[VOICE] screen=RESPONSE parsed=%d pages=%u structured=%d\n",
                parsed ? 1 : 0,
                (unsigned)g_ship_voice_ui_page_count,
                g_ship_voice_ui_structured ? 1 : 0);
  lv_timer_handler();
}

static void ship_main_menu_add_caption(lv_obj_t* btn, const char* text, lv_color_t text_color) {
  if (!btn) {
    return;
  }
  lv_obj_t* caption = lv_label_create(btn);
  lv_label_set_text(caption, text);
  lv_obj_set_style_text_color(caption, text_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(caption, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(caption, SHIP_MAIN_MENU_BTN_W - 14);
  lv_obj_align(caption, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void ship_main_menu_add_symbol_icon(lv_obj_t* btn, const char* symbol, int y_offset, lv_color_t text_color) {
  if (!btn) {
    return;
  }
  lv_obj_t* icon = lv_label_create(btn);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_color(icon, text_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, y_offset);
}

static void ship_main_menu_add_dish_icon(lv_obj_t* btn, lv_color_t fg_color) {
  if (!btn) {
    return;
  }
  lv_obj_t* plate = lv_obj_create(btn);
  lv_obj_set_size(plate, 36, 36);
  lv_obj_align(plate, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_set_style_radius(plate, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(plate, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(plate, 3, LV_PART_MAIN);
  lv_obj_set_style_border_color(plate, fg_color, LV_PART_MAIN);
  lv_obj_set_style_outline_width(plate, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(plate, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(plate, 0, LV_PART_MAIN);

  lv_obj_t* fork = lv_obj_create(btn);
  lv_obj_set_size(fork, 4, 30);
  lv_obj_align(fork, LV_ALIGN_TOP_MID, -20, 17);
  lv_obj_set_style_radius(fork, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(fork, fg_color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(fork, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(fork, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(fork, 0, LV_PART_MAIN);

  lv_obj_t* knife = lv_obj_create(btn);
  lv_obj_set_size(knife, 4, 30);
  lv_obj_align(knife, LV_ALIGN_TOP_MID, 20, 17);
  lv_obj_set_style_radius(knife, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(knife, fg_color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(knife, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(knife, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(knife, 0, LV_PART_MAIN);
}

static void ship_main_menu_play_entry_animation() {
  static const uint16_t kEntryDelays[4] = {0, 45, 90, 135};
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* btn = ship_main_menu_buttons[i];
    if (!btn) {
      continue;
    }
    int target_y = ship_main_menu_button_target_y(i);
    lv_anim_del(btn, NULL);
    lv_obj_set_y(btn, target_y + 20);
    lv_obj_set_style_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_anim_t pos_anim;
    lv_anim_init(&pos_anim);
    lv_anim_set_var(&pos_anim, btn);
    lv_anim_set_values(&pos_anim, target_y + 20, target_y);
    lv_anim_set_time(&pos_anim, 260);
    lv_anim_set_delay(&pos_anim, kEntryDelays[i]);
    lv_anim_set_path_cb(&pos_anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&pos_anim, ship_anim_set_y);
    lv_anim_start(&pos_anim);

    lv_anim_t opa_anim;
    lv_anim_init(&opa_anim);
    lv_anim_set_var(&opa_anim, btn);
    lv_anim_set_values(&opa_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&opa_anim, 220);
    lv_anim_set_delay(&opa_anim, kEntryDelays[i]);
    lv_anim_set_exec_cb(&opa_anim, ship_anim_set_obj_opa);
    lv_anim_start(&opa_anim);
  }
}

static void ship_main_menu_reset_visual_state() {
  for (int i = 0; i < 4; ++i) {
    lv_obj_t* btn = ship_main_menu_buttons[i];
    if (!btn) {
      continue;
    }
    lv_anim_del(btn, NULL);
    lv_obj_set_y(btn, ship_main_menu_button_target_y(i));
    lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  }
  if (ship_main_menu_ai_button) {
    lv_anim_del(ship_main_menu_ai_button, NULL);
    lv_obj_set_pos(ship_main_menu_ai_button, SHIP_MAIN_MENU_CENTER_BTN_X, SHIP_MAIN_MENU_CENTER_BTN_Y);
    lv_obj_set_style_opa(ship_main_menu_ai_button, LV_OPA_COVER, LV_PART_MAIN);
  }
  ship_main_menu_set_ai_hold_active(false);
}

static void ship_main_menu_play_tap_animation(const ship_menu_hitbox_t* hb) {
  if (!hb || ui_screen_state != SCREEN_HOME || ship_menu_screen_state != SHIP_MENU_SCREEN_MAIN) {
    return;
  }
  int index = ship_main_menu_button_index_for_action(hb->action);
  if (index < 0 || index >= 4) {
    return;
  }
  lv_obj_t* btn = ship_main_menu_buttons[index];
  if (!btn) {
    return;
  }
  int target_y = ship_main_menu_button_target_y(index);
  lv_anim_del(btn, NULL);
  lv_obj_set_y(btn, target_y);
  lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);

  lv_anim_t pos_anim;
  lv_anim_init(&pos_anim);
  lv_anim_set_var(&pos_anim, btn);
  lv_anim_set_values(&pos_anim, target_y, target_y + 6);
  lv_anim_set_time(&pos_anim, 70);
  lv_anim_set_playback_time(&pos_anim, 120);
  lv_anim_set_exec_cb(&pos_anim, ship_anim_set_y);
  lv_anim_start(&pos_anim);

  lv_anim_t opa_anim;
  lv_anim_init(&opa_anim);
  lv_anim_set_var(&opa_anim, btn);
  lv_anim_set_values(&opa_anim, LV_OPA_COVER, LV_OPA_80);
  lv_anim_set_time(&opa_anim, 70);
  lv_anim_set_playback_time(&opa_anim, 120);
  lv_anim_set_exec_cb(&opa_anim, ship_anim_set_obj_opa);
  lv_anim_start(&opa_anim);
}

static void show_ship_main_menu_impl() {
  if (!ship_menu_screen) {
    ship_menu_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_screen, LV_PCT(100), LV_PCT(100));
    ship_style_plain_screen(ship_menu_screen, lv_color_hex(0x0E2547));
    lv_obj_set_style_border_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ship_menu_screen, LV_OBJ_FLAG_SCROLLABLE);

    ship_main_menu_buttons[0] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[0], SHIP_MAIN_MENU_TOP_BTN_X, SHIP_MAIN_MENU_TOP_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[0], false);
    ship_main_menu_add_dish_icon(ship_main_menu_buttons[0], lv_color_hex(0xD6E4FF));
    ship_main_menu_add_caption(ship_main_menu_buttons[0], "Dish", lv_color_hex(0xFFFFFF));

    ship_main_menu_buttons[1] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[1], SHIP_MAIN_MENU_LEFT_BTN_X, SHIP_MAIN_MENU_LEFT_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[1], false);
    ship_main_menu_add_symbol_icon(ship_main_menu_buttons[1], "+", 12, lv_color_hex(0xD6E4FF));
    ship_main_menu_add_caption(ship_main_menu_buttons[1], "Check In", lv_color_hex(0xFFFFFF));

    ship_main_menu_buttons[2] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[2], SHIP_MAIN_MENU_RIGHT_BTN_X, SHIP_MAIN_MENU_RIGHT_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[2], false);
    ship_main_menu_add_symbol_icon(ship_main_menu_buttons[2], "-", 14, lv_color_hex(0xD6E4FF));
    ship_main_menu_add_caption(ship_main_menu_buttons[2], "Discard", lv_color_hex(0xFFFFFF));

    ship_main_menu_buttons[3] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[3], SHIP_MAIN_MENU_BOTTOM_BTN_X, SHIP_MAIN_MENU_BOTTOM_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[3], false);
    ship_main_menu_add_symbol_icon(ship_main_menu_buttons[3], "...", 14, lv_color_hex(0xD6E4FF));
    ship_main_menu_add_caption(ship_main_menu_buttons[3], "More", lv_color_hex(0xFFFFFF));

    ship_main_menu_ai_button = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_ai_button, SHIP_MAIN_MENU_CENTER_BTN_X, SHIP_MAIN_MENU_CENTER_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_ai_button, true);

    ship_main_menu_ai_label = lv_label_create(ship_main_menu_ai_button);
    lv_label_set_text(ship_main_menu_ai_label, "AI");
    lv_obj_set_style_text_font(ship_main_menu_ai_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(ship_main_menu_ai_label);
    ship_main_menu_set_ai_hold_active(false);
  }
  ui_log_asset("show_main_menu", "SHIP_MAIN_MENU", "custom_main_menu");
  ship_menu_screen_state = SHIP_MENU_SCREEN_MAIN;
  ui_screen_state = SCREEN_HOME;
  ui_busy = false;
  ship_ai_touch_active = false;
  home_shown_ms = millis();
  ship_logged_hide_at_ms = 0;
  ship_main_menu_reset_visual_state();
  lv_scr_load(ship_menu_screen);
  Serial.println("[SHIP_MENU] showing MAIN_MENU");
  lv_timer_handler();
  wifi_on_run_deferred_if_ready("main_menu");
}

static void show_ship_main_menu() {
  UI_SHOW(SCREEN_SHIP_MAIN_MENU, "show_main_menu");
}

static void show_ship_second_menu_impl() {
  if (!ship_menu_second_screen) {
    ship_menu_second_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_second_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(ship_menu_second_screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_log_asset("show_second_menu", "SHIP_SECOND_MENU", "ui_img_Frame_443_1_png");
    lv_obj_set_style_bg_img_src(ship_menu_second_screen, &ui_img_Frame_443_1_png, LV_PART_MAIN);
    lv_obj_set_style_bg_img_opa(ship_menu_second_screen, LV_OPA_COVER, LV_PART_MAIN);
  }
  ui_log_asset("show_second_menu", "SHIP_SECOND_MENU", "ui_img_Frame_443_1_png");
  ship_menu_screen_state = SHIP_MENU_SCREEN_SECOND;
  ui_screen_state = SCREEN_SECOND;
  ui_busy = false;
  home_shown_ms = millis();
  lv_scr_load(ship_menu_second_screen);
  Serial.println("[MENU] screen=SECOND_MENU");
  lv_timer_handler();
}

static void show_ship_second_menu() {
  UI_SHOW(SCREEN_SHIP_SECOND_MENU, "show_second_menu");
}

static void show_ship_settings_screen_impl() {
  if (!ship_menu_settings_screen) {
    ship_menu_settings_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_settings_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ship_menu_settings_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(ship_menu_settings_screen, LV_OBJ_FLAG_SCROLLABLE);

    ship_menu_settings_title = lv_label_create(ship_menu_settings_screen);
    lv_label_set_text(ship_menu_settings_title, "Settings");
    lv_obj_set_style_text_color(ship_menu_settings_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(ship_menu_settings_title, LV_ALIGN_TOP_MID, 0, 24);

    ship_menu_settings_versions = lv_label_create(ship_menu_settings_screen);
    lv_label_set_text(ship_menu_settings_versions, "");
    lv_obj_set_style_text_color(ship_menu_settings_versions, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(ship_menu_settings_versions, LV_ALIGN_TOP_MID, 0, SHIP_MENU_SETTINGS_VERSION_Y);

    ship_menu_settings_status = lv_label_create(ship_menu_settings_screen);
    lv_label_set_text(ship_menu_settings_status, "");
    lv_obj_set_style_text_color(ship_menu_settings_status, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(ship_menu_settings_status, LV_ALIGN_TOP_MID, 0, SHIP_MENU_SETTINGS_STATUS_Y);
    lv_obj_add_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN);

    ship_menu_settings_btn_reset = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_reset, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_RESET_Y);
    lv_obj_set_size(ship_menu_settings_btn_reset, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
    lv_obj_set_style_bg_color(ship_menu_settings_btn_reset, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_btn_reset, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_menu_settings_btn_reset, 0, LV_PART_MAIN);

    ship_menu_settings_label_reset = lv_label_create(ship_menu_settings_btn_reset);
    lv_label_set_text(ship_menu_settings_label_reset, "Reset Wi-Fi (Sense)");
    lv_obj_set_style_text_color(ship_menu_settings_label_reset, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(ship_menu_settings_label_reset);

    ship_menu_settings_btn_ota = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_ota, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_OTA_Y);
    lv_obj_set_size(ship_menu_settings_btn_ota, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
    lv_obj_set_style_bg_color(ship_menu_settings_btn_ota, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_btn_ota, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_menu_settings_btn_ota, 0, LV_PART_MAIN);

    ship_menu_settings_label_ota = lv_label_create(ship_menu_settings_btn_ota);
    lv_label_set_text(ship_menu_settings_label_ota, "Run OTA Update");
    lv_obj_set_style_text_color(ship_menu_settings_label_ota, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(ship_menu_settings_label_ota);

    ship_menu_settings_btn_back = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_back, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_BACK_Y);
    lv_obj_set_size(ship_menu_settings_btn_back, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
    lv_obj_set_style_bg_color(ship_menu_settings_btn_back, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_btn_back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ship_menu_settings_btn_back, 0, LV_PART_MAIN);

    ship_menu_settings_label_back = lv_label_create(ship_menu_settings_btn_back);
    lv_label_set_text(ship_menu_settings_label_back, "Back");
    lv_obj_set_style_text_color(ship_menu_settings_label_back, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(ship_menu_settings_label_back);

    // Three-button settings screen (Reset Wi-Fi + OTA + Back)
  }
  ship_menu_screen_state = SHIP_MENU_SCREEN_SETTINGS;
  ui_screen_state = SCREEN_SETTINGS;
  ui_busy = false;
  home_shown_ms = millis();
  if (ship_menu_settings_status != NULL) {
    lv_obj_add_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN);
    ship_menu_settings_status_hide_at_ms = 0;
  }
  ship_menu_update_versions_label();
  request_sense_wake("settings_fw_info");
  ship_menu_request_fw_info();
  lv_scr_load(ship_menu_settings_screen);
  Serial.println("[MENU] screen=SETTINGS");
  lv_timer_handler();
}

static void show_ship_settings_screen() {
  UI_SHOW(SCREEN_SHIP_SETTINGS, "show_settings");
}

static void status_overlay_init() {
  return;
}

static void status_overlay_show(const char* text) {
  (void)text;
  return;
}

static void status_overlay_hide() {
  g_status_hide_at_ms = 0;
  return;
}

static void ship_hide_expiry_screen() {
  if (!expiry_screen) {
    return;
  }
  lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
  expiry_screen_visible = false;
  expiry_screen_shown_time = 0;
  if (expiry_timeout_ring) {
    lv_arc_set_value(expiry_timeout_ring, 1000);
  }
}

static void ship_hide_default_ui() {
  if (list_container) lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  if (menu_screen) lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  if (meal_result_screen) lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  if (loading_screen) lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
}

static void ship_style_plain_screen(lv_obj_t* screen, lv_color_t bg_color) {
  if (!screen) {
    return;
  }
  lv_obj_set_style_bg_img_src(screen, NULL, LV_PART_MAIN);
  lv_obj_set_style_bg_color(screen, bg_color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
}

#endif // LCD_SHIP_SCREENS_H

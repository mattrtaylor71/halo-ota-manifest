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

LV_IMG_DECLARE(dish_icon_img);
LV_IMG_DECLARE(mic_icon_img);

// Home-only amber retains gold coding with readable contrast on white cards.
static const uint32_t SHIP_HOME_SYMBOL_GOLD = 0xA66A00;

static int ship_main_menu_button_index_for_action(ship_menu_action_t action) {
  switch (action) {
  case SHIP_MENU_ACTION_LOG_DISH:
    return 0;
  case SHIP_MENU_ACTION_CHECK_IN:
    return 1;
  case SHIP_MENU_ACTION_CHECK_OUT:
    return 2;
  case SHIP_MENU_ACTION_MORE:
    return 3;
  default:
    return -1;
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

// Design-system card. Replaces the hand-rolled styling that had drifted off spec:
// the shadow was 8px wide at 50% offset 5/7, i.e. blurred and soft. The system
// calls for a HARD shadow — 4px right/down, no blur, ink at 85% — which is what
// trepo_card() applies. Colours now come from the palette instead of literals.
static void ship_main_menu_style_button(lv_obj_t* btn, bool primary) {
  if (!btn) {
    return;
  }
  lv_obj_set_size(btn, SHIP_MAIN_MENU_BTN_W, SHIP_MAIN_MENU_BTN_H);
  trepo_card(btn, primary ? COL_TEAL : COL_WHITE, SHIP_MAIN_MENU_BTN_RADIUS);
  quiet_clickable(btn);
}

static void ship_main_menu_set_ai_hold_active(bool active) {
  if (!ship_main_menu_ai_button || !ship_main_menu_ai_label) {
    return;
  }
  // All Home cards stay white; a held gesture accents only the center border gold.
  lv_obj_set_style_bg_color(ship_main_menu_ai_button,
                            lv_color_hex(COL_WHITE),
                            LV_PART_MAIN);
  lv_obj_set_style_border_color(ship_main_menu_ai_button,
                            active ? lv_color_hex(COL_GOLD) : lv_color_hex(COL_DARK),
                            LV_PART_MAIN);
}

static void ship_main_menu_add_plus(lv_obj_t* btn, lv_color_t color);
static void ship_main_menu_add_minus(lv_obj_t* btn, lv_color_t color);
static void ship_main_menu_add_dots(lv_obj_t* btn, lv_color_t color);

static const uint8_t SHIP_VOICE_WAVE_BAR_COUNT = 13;
static lv_obj_t* ship_voice_wave_bars[SHIP_VOICE_WAVE_BAR_COUNT] = {NULL};
static lv_obj_t* ship_voice_contact_ring = NULL;
static bool ship_voice_wave_animating = false;
static unsigned long ship_voice_wave_last_frame_ms = 0;

// Decorative motion stays above the finger. Only ordinary rounded bars change
// size; there is no audio transport, draw layer, per-frame allocation or timer.
static void ship_voice_wave_draw(int32_t phase) {
  static const uint8_t peak_height[SHIP_VOICE_WAVE_BAR_COUNT] = {
      16, 23, 29, 35, 40, 44, 46, 44, 40, 35, 29, 23, 16};
  for (uint8_t i = 0; i < SHIP_VOICE_WAVE_BAR_COUNT; ++i) {
    lv_obj_t* bar = ship_voice_wave_bars[i];
    if (!bar)
      continue;
    uint32_t level = 450;
    if (phase >= 0) {
      // LVGL's fixed-point lookup avoids trigonometric floating-point work.
      int angle = (phase * 2 + i * 39) % 360;
      level = ((int32_t)lv_trigo_sin(angle) + 32767L) * 1000L / 65534L;
    }
    int height = 7 + ((peak_height[i] - 7) * level) / 1000;
    lv_obj_set_height(bar, height);
    lv_obj_set_y(bar, 70 - height / 2);
  }
}

static bool ship_voice_wave_hold_active() {
  return ui_screen_state == SCREEN_AI_LISTENING && ship_ai_touch_active &&
         long_press_sent && ship_ai_listening_countdown_start_ms > 0 &&
         millis() - ship_ai_listening_countdown_start_ms < SHIP_AI_LISTENING_COUNTDOWN_MS;
}

static void ship_voice_wave_anim_cb(void* obj, int32_t phase) {
  if (obj != ship_ai_listening_screen || !ship_voice_wave_hold_active())
    return;
  unsigned long now = millis();
  if (now - ship_voice_wave_last_frame_ms < 50)
    return;
  ship_voice_wave_last_frame_ms = now;
  ship_voice_wave_draw(phase);
}

static void ship_voice_wave_stop() {
  if (ship_ai_listening_screen)
    lv_anim_del(ship_ai_listening_screen, ship_voice_wave_anim_cb);
  ship_voice_wave_animating = false;
  ship_voice_wave_last_frame_ms = 0;
}

static void ship_voice_wave_screen_event(lv_event_t* event) {
  if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOAD_START)
    ship_voice_wave_stop();
}

static void ship_sync_ai_listening_animation() {
  bool active = ship_voice_wave_hold_active();
  if (ship_voice_contact_ring) {
    if (active)
      lv_obj_clear_flag(ship_voice_contact_ring, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(ship_voice_contact_ring, LV_OBJ_FLAG_HIDDEN);
  }
  if (!active) {
    if (ship_voice_wave_animating) {
      ship_voice_wave_stop();
      ship_voice_wave_draw(-1);
    }
    return;
  }
  if (ship_voice_wave_animating)
    return;
  ship_voice_wave_animating = true;
  ship_voice_wave_last_frame_ms = 0;
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, ship_ai_listening_screen);
  lv_anim_set_values(&animation, 0, 360);
  lv_anim_set_time(&animation, 2400);
  lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&animation, ship_voice_wave_anim_cb);
  lv_anim_start(&animation);
}

static lv_color_t ship_voice_muted_color(uint32_t color) {
  return lv_color_mix(lv_color_hex(color), lv_color_hex(COL_CREAM), 71);
}

static lv_obj_t* ship_voice_muted_card(int x, int y) {
  lv_obj_t* card = lv_obj_create(ship_ai_listening_screen);
  ship_main_menu_style_button(card, false);
  lv_obj_set_pos(card, x, y);
  // Preblend the dormant Home cards instead of fading their containers.
  lv_obj_set_style_bg_color(card, ship_voice_muted_color(COL_WHITE), 0);
  lv_obj_set_style_border_color(card, ship_voice_muted_color(COL_DARK), 0);
  lv_obj_set_style_shadow_color(card, ship_voice_muted_color(COL_DARK), 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
  return card;
}

static void ship_init_ai_listening_screen() {
  if (ship_ai_listening_screen)
    return;
  ship_ai_listening_screen = halo_ui_page();
  lv_obj_add_event_cb(ship_ai_listening_screen, ship_voice_wave_screen_event,
                      LV_EVENT_SCREEN_UNLOAD_START, NULL);
  ship_ai_listening_title = NULL;
  ship_ai_listening_hint = NULL;
  ship_ai_listening_ring = NULL;

  lv_obj_t* left = ship_voice_muted_card(SHIP_MAIN_MENU_LEFT_BTN_X, SHIP_MAIN_MENU_LEFT_BTN_Y);
  ship_main_menu_add_plus(left, ship_voice_muted_color(COL_GREEN));
  lv_obj_t* right = ship_voice_muted_card(SHIP_MAIN_MENU_RIGHT_BTN_X, SHIP_MAIN_MENU_RIGHT_BTN_Y);
  ship_main_menu_add_minus(right, ship_voice_muted_color(COL_RED));
  lv_obj_t* bottom = ship_voice_muted_card(SHIP_MAIN_MENU_BOTTOM_BTN_X, SHIP_MAIN_MENU_BOTTOM_BTN_Y);
  ship_main_menu_add_dots(bottom, ship_voice_muted_color(SHIP_HOME_SYMBOL_GOLD));

  ship_voice_contact_ring = lv_obj_create(ship_ai_listening_screen);
  lv_obj_remove_style_all(ship_voice_contact_ring);
  lv_obj_set_pos(ship_voice_contact_ring, 123, 123);
  lv_obj_set_size(ship_voice_contact_ring, 114, 114);
  lv_obj_set_style_radius(ship_voice_contact_ring, 33, 0);
  lv_obj_set_style_border_width(ship_voice_contact_ring, 2, 0);
  lv_obj_set_style_border_color(ship_voice_contact_ring, lv_color_hex(COL_TEAL), 0);
  lv_obj_set_style_border_opa(ship_voice_contact_ring, LV_OPA_60, 0);
  lv_obj_clear_flag(ship_voice_contact_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ship_voice_contact_ring, LV_OBJ_FLAG_HIDDEN);

  // The held target stays exactly where the user's finger pressed on Home.
  ship_ai_listening_mic_disc = lv_obj_create(ship_ai_listening_screen);
  ship_main_menu_style_button(ship_ai_listening_mic_disc, false);
  lv_obj_set_pos(ship_ai_listening_mic_disc, SHIP_MAIN_MENU_CENTER_BTN_X,
                 SHIP_MAIN_MENU_CENTER_BTN_Y);
  lv_obj_clear_flag(ship_ai_listening_mic_disc, LV_OBJ_FLAG_CLICKABLE);
  ship_ai_listening_mic_head =
      halo_ui_icon(ship_ai_listening_mic_disc, HALO_ICON_MIC, 0, 0, 54, SHIP_HOME_SYMBOL_GOLD);
  lv_obj_center(ship_ai_listening_mic_head);
  for (uint8_t i = 0; i < SHIP_VOICE_WAVE_BAR_COUNT; ++i) {
    lv_obj_t* bar = lv_obj_create(ship_ai_listening_screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 98 + i * 13, 66);
    lv_obj_set_size(bar, 7, 8);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_TEAL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ship_voice_wave_bars[i] = bar;
  }
  ship_voice_wave_draw(-1);
  // Compatibility handles are not separate animated objects.
  ship_ai_listening_mic_stem = NULL;
  ship_ai_listening_mic_base = NULL;
  for (int i = 0; i < 3; ++i)
    ship_ai_listening_pulse[i] = NULL;
}

static void ship_start_ai_listening_animation() {
  ship_voice_wave_stop();
  ship_voice_wave_draw(-1);
  ship_sync_ai_listening_animation();
}

static void ship_show_ai_listening_screen() {
  halo_ui_motion_stop(lv_scr_act());
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

static bool ship_voice_gesture_begin() {
  if(lcd_media_voice_begin())return true;
  show_auto_hiding_status_message("Finishing sync - try again",1800);
  resetActivityTimer();
  Serial.println("[MEDIA_BUSY] voice gesture rejected until release");
  return false;
}

static void ship_voice_status_begin() {
  // A new local recording retires the previous voice's UI identity. Wait for
  // Sense's RECORDING status to bind the new job before accepting its failure.
  snprintf(g_ship_ui_op, sizeof(g_ship_ui_op), "VOICE");
  snprintf(g_ship_ui_phase, sizeof(g_ship_ui_phase), "RECORDING");
  g_ship_ui_job_id = 0;
  g_ship_ui_finalized = false;
  g_ship_ui_dirty = false;
}

static bool ship_queue_voice_input(const char* type, const char* wake_reason) {
  if (!type || !type[0]) {
    return false;
  }
  if (strcmp(type, "INPUT_LONG_PRESS_START") == 0) {
    ship_voice_status_begin();
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
  if (!uart_tx_enqueue(&tx_msg, "ship_screens")) return false;
  Serial.printf("[AI] queued type=%s wake_reason=%s\n",
                type,
                wake_reason ? wake_reason : "");
  return true;
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

static lv_obj_t* ship_voice_badge = NULL;
static void ship_voice_ui_apply_style(const ship_voice_ui_page_t* page) {
  bool warning = page && (strcmp(page->style, "warning") == 0 || strcmp(page->style, "error") == 0);
  if (ship_voice_badge) {
    lv_obj_set_style_bg_color(ship_voice_badge, lv_color_hex(warning ? COL_RED : COL_GOLD), 0);
    lv_label_set_text(lv_obj_get_child(ship_voice_badge, 0), warning ? "HEADS UP" : "THYME SAYS");
  }
  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i)
    if (ship_voice_json_page_dots[i]) {
      lv_obj_set_style_bg_color(ship_voice_json_page_dots[i],
                                lv_color_hex(i == g_ship_voice_ui_page_index ? COL_TEAL : COL_TERT),
                                0);
    }
}

static void ship_voice_ui_render_current_page() {
  ship_init_voice_json_screen();
  if (g_ship_voice_ui_page_count == 0 || g_ship_voice_ui_page_index >= g_ship_voice_ui_page_count)
    return;
  ship_voice_ui_page_t* page = &g_ship_voice_ui_pages[g_ship_voice_ui_page_index];
  ship_voice_ui_apply_style(page);
  lv_label_set_text(ship_voice_json_title, page->title[0] ? page->title : "Response");
  int y = 10;
  y = ship_voice_ui_layout_label(ship_voice_json_subtitle, page->subtitle, y, 5);
  y = ship_voice_ui_layout_label(ship_voice_json_label, page->body, y, 7);
  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    if (i < page->item_count) {
      char line[96];
      snprintf(line, sizeof(line), "- %s", page->items[i]);
      y = ship_voice_ui_layout_label(ship_voice_json_item_labels[i], line, y, 5);
    } else
      lv_obj_add_flag(ship_voice_json_item_labels[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (page->footer[0] && !ship_voice_ui_footer_is_chrome(page->footer))
    y = ship_voice_ui_layout_label(ship_voice_json_footer, page->footer, y, 0);
  else
    lv_obj_add_flag(ship_voice_json_footer, LV_OBJ_FLAG_HIDDEN);
  // Preserve all supplied text in the card, with existing page navigation. A
  // long individual page uses the existing LVGL vertical scroll, never sample text.
  lv_obj_set_scroll_dir(ship_voice_json_card, LV_DIR_VER);
  if (y > 138)
    lv_obj_add_flag(ship_voice_json_card, LV_OBJ_FLAG_SCROLLABLE);
  else
    lv_obj_clear_flag(ship_voice_json_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_scroll_to_y(ship_voice_json_card, 0, LV_ANIM_OFF);
  lv_obj_add_flag(ship_voice_json_page_label, LV_OBJ_FLAG_HIDDEN);
  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    lv_obj_t* dot = ship_voice_json_page_dots[i];
    if (i < g_ship_voice_ui_page_count && g_ship_voice_ui_page_count > 1 &&
        g_ship_voice_ui_show_page_dots) {
      lv_obj_set_pos(dot, 177 + ((int)i * 14) - ((int)g_ship_voice_ui_page_count - 1) * 7, 256);
      lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    } else
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
  }
  if (g_ship_voice_ui_page_count > 1)
    lv_obj_clear_flag(ship_voice_json_hint, LV_OBJ_FLAG_HIDDEN);
  else
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
  if (ship_voice_json_screen)
    return;
  ship_voice_json_screen = halo_ui_page();
  ship_voice_badge = halo_ui_badge(ship_voice_json_screen, "THYME SAYS", 20);
  ship_voice_json_title =
      halo_ui_label(ship_voice_json_screen, "Response", &nunito_22, COL_DARK, 40, 54, 280);
  lv_obj_set_height(ship_voice_json_title, 32);
  lv_label_set_long_mode(ship_voice_json_title, LV_LABEL_LONG_DOT);
  ship_voice_json_card = halo_ui_card(ship_voice_json_screen, 58, 94, 244, 150);
  ship_voice_json_subtitle =
      halo_ui_label(ship_voice_json_card, "", &lv_font_montserrat_14, COL_TEXT2, 14, 12, 212);
  ship_voice_json_label =
      halo_ui_label(ship_voice_json_card, "", &lv_font_montserrat_16, COL_DARK, 14, 12, 212);
  lv_obj_set_style_text_line_space(ship_voice_json_label, 3, 0);
  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    ship_voice_json_item_labels[i] =
        halo_ui_label(ship_voice_json_card, "", &nunito_18, COL_DARK, 14, 12, 212);
    lv_obj_set_style_text_align(ship_voice_json_item_labels[i], LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_flag(ship_voice_json_item_labels[i], LV_OBJ_FLAG_HIDDEN);
  }
  ship_voice_json_footer =
      halo_ui_label(ship_voice_json_card, "", &lv_font_montserrat_12, COL_TEXT2, 14, 12, 212);
  ship_voice_json_page_label =
      halo_ui_label(ship_voice_json_screen, "", &nunito_12, COL_MUTED, 144, 251, 72);
  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    lv_obj_t* dot = lv_obj_create(ship_voice_json_screen);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, 3, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    ship_voice_json_page_dots[i] = dot;
    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
  }
  ship_voice_json_hint = halo_ui_label(ship_voice_json_screen, "Turn the dial for pages",
                                       &nunito_12, COL_MUTED, 55, 274, 250);
  halo_ui_back(ship_voice_json_screen);
}

static void ship_show_voice_json_screen(const char* json_text) {
  halo_ui_motion_stop(lv_scr_act());
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
  lv_obj_t* icon = lv_img_create(btn);
  lv_img_set_src(icon, &dish_icon_img);
  // Keep the original alpha/shape; recolor its opaque pixels for the filled card.
  lv_obj_set_style_img_recolor(icon, fg_color, LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_center(icon);
}

static void ship_main_menu_add_mic_icon(lv_obj_t* btn) {
  if (!btn) {
    return;
  }
  // Use the same clean vector as the listening screen. The bitmap's faint
  // nonzero background alpha otherwise leaves a rectangle on the white card.
  lv_obj_t* icon = halo_ui_icon(btn, HALO_ICON_MIC, 0, 0, 54, SHIP_HOME_SYMBOL_GOLD);
  lv_obj_center(icon);
}

// solid rounded bar / dot used to draw +, -, ... crisply (no image, zero flash)
static lv_obj_t* ship_menu_bar(lv_obj_t* parent, int w, int h, lv_color_t color) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, (h < w ? h : w) / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  return b;
}

static void ship_main_menu_add_plus(lv_obj_t* btn, lv_color_t color) {
  if (!btn) return;
  lv_obj_center(ship_menu_bar(btn, 46, 10, color));
  lv_obj_center(ship_menu_bar(btn, 10, 46, color));
}

static void ship_main_menu_add_minus(lv_obj_t* btn, lv_color_t color) {
  if (!btn) return;
  lv_obj_center(ship_menu_bar(btn, 46, 10, color));
}

static void ship_main_menu_add_dots(lv_obj_t* btn, lv_color_t color) {
  if (!btn) return;
  const int d = 12, step = 21;
  for (int i = -1; i <= 1; ++i) {
    lv_obj_align(ship_menu_bar(btn, d, d, color), LV_ALIGN_CENTER, i * step, 0);
  }
}

static void ship_menu_add_symbol_centered(lv_obj_t* btn, const char* symbol, lv_color_t color) {
  if (!btn) return;
  lv_obj_t* icon = lv_label_create(btn);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_color(icon, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_center(icon);
}

static void ship_main_menu_play_entry_animation() {
  // Home appears immediately; no screen/container opacity layer.
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
  if (!hb || ui_screen_state != SCREEN_HOME)
    return;
  int index = ship_main_menu_button_index_for_action(hb->action);
  if (index < 0 || index >= 4 || !ship_main_menu_buttons[index])
    return;
  lv_obj_t* btn = ship_main_menu_buttons[index];
  lv_anim_del(btn, NULL);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, btn);
  int y = ship_main_menu_button_target_y(index);
  lv_anim_set_values(&a, y, y + 3);
  lv_anim_set_time(&a, 60);
  lv_anim_set_playback_time(&a, 100);
  lv_anim_set_exec_cb(&a, ship_anim_set_y);
  lv_anim_start(&a);
}

static void show_ship_main_menu_impl() {
  if (!ship_menu_screen) {
    ship_menu_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_screen, LV_PCT(100), LV_PCT(100));
    ship_style_plain_screen(ship_menu_screen, lv_color_hex(COL_CREAM));  // was 0xFDF2DE (off-palette)
    lv_obj_set_style_border_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ship_menu_screen, LV_OBJ_FLAG_SCROLLABLE);

    ship_main_menu_buttons[0] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[0], SHIP_MAIN_MENU_TOP_BTN_X, SHIP_MAIN_MENU_TOP_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[0], false);
    ship_main_menu_add_dish_icon(ship_main_menu_buttons[0], lv_color_hex(COL_TEAL));

    ship_main_menu_buttons[1] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[1], SHIP_MAIN_MENU_LEFT_BTN_X, SHIP_MAIN_MENU_LEFT_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[1], false);
    ship_main_menu_add_plus(ship_main_menu_buttons[1], lv_color_hex(COL_GREEN));

    ship_main_menu_buttons[2] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[2], SHIP_MAIN_MENU_RIGHT_BTN_X, SHIP_MAIN_MENU_RIGHT_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[2], false);
    ship_main_menu_add_minus(ship_main_menu_buttons[2], lv_color_hex(COL_RED));

    ship_main_menu_buttons[3] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[3], SHIP_MAIN_MENU_BOTTOM_BTN_X, SHIP_MAIN_MENU_BOTTOM_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[3], false);
    ship_main_menu_add_dots(ship_main_menu_buttons[3], lv_color_hex(SHIP_HOME_SYMBOL_GOLD));

    ship_main_menu_ai_button = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_ai_button, SHIP_MAIN_MENU_CENTER_BTN_X, SHIP_MAIN_MENU_CENTER_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_ai_button, false);  // white like the outer cards

    // keep a hidden label so ship_main_menu_set_ai_hold_active()'s guard passes
    ship_main_menu_ai_label = lv_label_create(ship_main_menu_ai_button);
    lv_label_set_text(ship_main_menu_ai_label, "AI");
    lv_obj_center(ship_main_menu_ai_label);
    lv_obj_add_flag(ship_main_menu_ai_label, LV_OBJ_FLAG_HIDDEN);
    ship_main_menu_add_mic_icon(ship_main_menu_ai_button);         // amber mic on white
    ship_main_menu_set_ai_hold_active(false);
  }
  ui_log_asset("show_main_menu", "SHIP_MAIN_MENU", "custom_main_menu");
  ship_menu_screen_state = SHIP_MENU_SCREEN_MAIN;
  ui_screen_state = SCREEN_HOME;
  ui_busy = false;
  ship_ai_touch_active = false;
  home_shown_ms = millis();
  touch_ignore_until = millis() + 280;  // guard: ignore bleed-through touch from the navigation tap so it can't trigger a button on the just-loaded screen (e.g. tapping Settings auto-firing Run OTA Update)
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
    ship_menu_second_screen = halo_ui_page();
    halo_ui_label(ship_menu_second_screen, "More", &nunito_28, COL_DARK, 60, 42, 240);
    lv_obj_t* list = halo_ui_card(ship_menu_second_screen, 58, 104, 244, 56);
    halo_ui_icon(list, HALO_ICON_LIST, 16, 15, 22, COL_TEAL);
    halo_ui_label(list, "Shopping list", &nunito_18, COL_DARK, 48, 16, 182);
    lv_obj_t* settings = halo_ui_card(ship_menu_second_screen, 58, 176, 244, 56);
    halo_ui_icon(settings, HALO_ICON_SETTINGS, 16, 15, 22, COL_TEAL);
    halo_ui_label(settings, "Settings", &nunito_18, COL_DARK, 48, 16, 182);
    halo_ui_back(ship_menu_second_screen);
  }
  ui_log_asset("show_second_menu", "SHIP_SECOND_MENU", "dynamic_second_menu");
  ship_menu_screen_state = SHIP_MENU_SCREEN_SECOND;
  ui_screen_state = SCREEN_SECOND;
  ui_busy = false;
  home_shown_ms = millis();
  touch_ignore_until = millis() + 280;  // guard: ignore bleed-through touch from the navigation tap so it can't trigger a button on the just-loaded screen (e.g. tapping Settings auto-firing Run OTA Update)
  lv_scr_load(ship_menu_second_screen);
  Serial.println("[MENU] screen=SECOND_MENU");
  lv_timer_handler();
}

static void show_ship_second_menu() {
  UI_SHOW(SCREEN_SHIP_SECOND_MENU, "show_second_menu");
}

// forward declaration (defined later in this header)
static void show_ship_settings_screen();

// ── Shopping List Screen ──────────────────────────────────────────────
static lv_obj_t* shopping_list_screen = NULL;
static lv_obj_t* shopping_list_scroll = NULL;
static lv_obj_t* shopping_list_title_label = NULL;
static lv_obj_t* shopping_list_count_label = NULL;
static lv_obj_t* shopping_list_refresh_btn = NULL;
static int shopping_list_scroll_idx = 0;
static int shopping_list_overscroll_ticks = 0;  // counts CCW ticks at top for pull-to-refresh
static const int SHOPPING_LIST_REFRESH_TICKS = 3;
static unsigned long shopping_list_last_ccw_tick_ms = 0;  // last CCW overscroll tick (for stale-tick expiry)
static const unsigned long SHOPPING_LIST_OVERSCROLL_WINDOW_MS = 1500;  // CCW ticks older than this don't count toward refresh
static lv_obj_t* shopping_list_items[50] = {NULL};
// Parallel to shopping_list_items[]: for each item that is first-in-its-store-
// group, holds the store-header label object positioned directly above its
// card (NULL otherwise). Selection scroll targets this header instead of the
// card so scrolling onto a group's first item reveals the header rather than
// clipping it off the top. NOT an item — never counted, never deleted.
static lv_obj_t* shopping_list_item_headers[50] = {NULL};
static int shopping_list_rendered_count = 0;
static lv_obj_t* shopping_list_overlay = NULL;  // Delete/Back overlay
static bool shopping_list_overlay_visible = false;
static char shopping_list_overlay_item_id[64] = {0};
static char shopping_list_pending_delete_id[64] = {0};
static unsigned long shopping_list_pending_delete_ms = 0;
// Overlay hitbox sources — the touch handler derives its hit areas from these
// rendered objects (lv_obj_get_coords + slop) instead of magic coordinates,
// so layout tweaks can never desync the hitboxes from the pixels.
static lv_obj_t* shopping_list_overlay_card = NULL;        // center card
static lv_obj_t* shopping_list_overlay_delete_btn = NULL;  // red Delete button
static lv_obj_t* shopping_list_overlay_back_btn = NULL;    // green Back button
static lv_obj_t* shopping_list_back_btn_obj = NULL;  // Bottom back button

// Border refresh ring — a full-screen lv_arc hugging the display edge.
// Replaces the old floating refresh pill: the list stays fully usable while
// refreshing (the ring is input-transparent and covers no content).
// States (driven by shopping_list_refresh_indicator_sync + the pull gesture):
//   pull drag   — fills clockwise from 12 o'clock with pull progress (0-360°
//                 at the 45px threshold); armed = full ring + width 5→7 bump
//   refreshing  — ~70° segment sweeping continuously (~1.2s/rev, linear)
//   complete    — segment closes into a full 360° ring, then fades out
//   failed      — full ring flashes twice in the error red + transient
//                 "Couldn't refresh" toast (errors are the only text)
#include "lcd_shopping_scroll_asset.h"
static lv_obj_t* shopping_list_refresh_ring = NULL;
static lv_obj_t* shopping_list_scroll_cue = NULL;

// One small flash-backed alpha mask avoids rotating labels or allocating a
// full-screen layer. Keep it clear of the active refresh ring and Delete dialog.
static void shopping_list_scroll_cue_sync() {
  if (!shopping_list_scroll_cue || !shopping_list_scroll) return;
  const bool overflowing = g_active.count > 0 &&
      (lv_obj_get_scroll_top(shopping_list_scroll) > 0 ||
       lv_obj_get_scroll_bottom(shopping_list_scroll) > 0);
  const bool ring_visible = shopping_list_refresh_ring &&
      !lv_obj_has_flag(shopping_list_refresh_ring, LV_OBJ_FLAG_HIDDEN);
  if (overflowing && !ring_visible && !shopping_list_overlay_visible)
    lv_obj_clear_flag(shopping_list_scroll_cue, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(shopping_list_scroll_cue, LV_OBJ_FLAG_HIDDEN);
}

static void shopping_list_scroll_cue_deleted(lv_event_t* event) {
  if (lv_event_get_target(event) == shopping_list_scroll_cue)
    shopping_list_scroll_cue = NULL;
}

static void shopping_list_build_scroll_cue(lv_obj_t* parent) {
  shopping_list_scroll_cue = lv_img_create(parent);
  lv_img_set_src(shopping_list_scroll_cue, &shopping_scroll_cue_img);
  lv_obj_set_pos(shopping_list_scroll_cue, SHOPPING_SCROLL_CUE_X, SHOPPING_SCROLL_CUE_Y);
  lv_obj_set_style_img_recolor(shopping_list_scroll_cue, lv_color_hex(COL_TEAL), 0);
  lv_obj_set_style_img_recolor_opa(shopping_list_scroll_cue, LV_OPA_COVER, 0);
  lv_obj_clear_flag(shopping_list_scroll_cue, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(shopping_list_scroll_cue, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(shopping_list_scroll_cue, shopping_list_scroll_cue_deleted, LV_EVENT_DELETE, NULL);
}
// How many refresh timeouts with NO successful refresh ever before the list
// stops looping the skeleton and states plainly that the Sense is unreachable.
#define LIST_HONEST_FAIL_TIMEOUTS 2

static lv_obj_t* shopping_list_error_toast = NULL;
static int shopping_list_refresh_ui_state = -1;   // last refresh SM state synced to the ring (-1 = force re-sync)
static bool shopping_list_ring_hiding = false;    // ring fade-out in progress (liststate "pill_hiding")
static bool shopping_list_ring_sweeping = false;  // sweep anim currently running
static bool shopping_list_ring_autohide = false;  // finish/error sequence owns the hide — plain hide() must not interrupt
// Whether the current refresh should show the spinning ring. TRUE for a user
// pull-to-refresh; FALSE for the silent auto entry-revalidate on list open
// (so opening the list is clean — no spinner unless the user pulls).
static bool shopping_list_refresh_show_ring = false;
static int32_t shopping_list_ring_sweep_base = 0; // current sweep start angle (deg from 12 o'clock)
static const int SHOPPING_LIST_RING_SIZE = 352;          // hugs the 360px round edge (4px margin)
static const int SHOPPING_LIST_RING_WIDTH = 5;           // indicator arc width
static const int SHOPPING_LIST_RING_WIDTH_ARMED = 7;     // emphasis when the pull passes the threshold
static const int SHOPPING_LIST_RING_SWEEP_DEG = 70;      // sweeping segment length
static const uint32_t SHOPPING_LIST_RING_SWEEP_MS = 1200;  // one full revolution

// Round-screen card layout
static const int SHOPPING_LIST_CARD_W = 248; // clears the circle at all visible heights
static const int SHOPPING_LIST_CARD_H = 50;
static const int SHOPPING_LIST_CARD_RADIUS = 16;
static const int SHOPPING_LIST_CARD_GAP = 6;

// Signature of the list content currently rendered in shopping_list_scroll.
// 0xFFFFFFFF = placeholder content (skeleton / hint label) that never matches
// real content, so the next real list always triggers a render.
static uint32_t shopping_list_rendered_sig = 0xFFFFFFFFu;

// Touch pull-to-refresh state (LVGL scroll events on shopping_list_scroll)
static bool shopping_list_touch_pull_armed = false;     // pull passed threshold while pressed
static bool shopping_list_touch_pull_consumed = false;  // refresh already fired for this gesture
static bool shopping_list_touch_in_press = false;       // finger currently down on the scroll container
static bool shopping_list_touch_was_scrolled = false;   // gesture scrolled — suppress the tap-overlay on release
static lv_point_t shopping_list_touch_start_pt = {0, 0};  // where the finger landed this gesture
// How far the finger may travel and still count as a tap. Below this, release
// opens the item overlay; above it the gesture is a scroll and the release is
// swallowed. 10px sits above a deliberate tap's jitter and well below any real
// scroll on a 360px panel.
static const int SHOPPING_LIST_TAP_SLOP_PX = 10;
static const int SHOPPING_LIST_PULL_THRESHOLD_PX = 45;  // pull-down distance past top to arm refresh
static const int SHOPPING_LIST_PULL_HINT_PX = 12;       // pull distance at which the hint cue appears

static bool shopping_list_reveal_pending = false;  // staggered fade-in on next populate (fresh UI_LIST)

// Re-arm the touch pull-to-refresh latch. shopping_list_screen_populate()
// already does this after lv_obj_clean(), but a refresh that RESOLVES without
// repopulating (hard-timeout, or an unchanged list that skips the rebuild)
// must still re-arm so the next pull-to-refresh gesture can fire. LVGL-safe:
// call only from the UI task / LVGL-locked context.
static inline void shopping_list_reset_pull_latch() {
  shopping_list_touch_pull_consumed = false;
  shopping_list_touch_pull_armed = false;
}

// Forward declarations
static void show_shopping_list_screen();
static void shopping_list_screen_populate();
static void shopping_list_dismiss_overlay();
static void shopping_list_trigger_refresh(const char* reason);

// A queued delete changes only the presentation until its matching result.
// Keep the source rows/cache intact so failure or timeout can restore the item.
static int shopping_list_pending_delete_index() {
  if (!shopping_list_pending_delete_id[0]) return -1;
  for (int i = 0; i < g_active.count; ++i)
    if (strcmp(g_active.item_ids[i], shopping_list_pending_delete_id) == 0) return i;
  return -1;
}

static int shopping_list_visible_count() {
  return g_active.count - (shopping_list_pending_delete_index() >= 0 ? 1 : 0);
}

static int shopping_list_visible_index(int idx, int direction) {
  if (g_active.count <= 0) return 0;
  if (idx < 0) idx = 0;
  if (idx >= g_active.count) idx = g_active.count - 1;
  if (idx != shopping_list_pending_delete_index()) return idx;
  const int next = idx + (direction < 0 ? -1 : 1);
  if (next >= 0 && next < g_active.count) return next;
  const int previous = idx - (direction < 0 ? -1 : 1);
  return previous >= 0 && previous < g_active.count ? previous : idx;
}

static void shopping_list_refresh_delete_view() {
  if (ui_screen_state != SCREEN_SHOPPING_LIST) return;
  shopping_list_reveal_pending = false;
  shopping_list_screen_populate();
}

// Keep the Delete/Back overlay above everything else on the list screen.
// The refresh ring and error toast call lv_obj_move_foreground on themselves
// while animating — without this they'd stack over a visible overlay.
static void shopping_list_keep_overlay_on_top() {
  shopping_list_scroll_cue_sync();
  if (shopping_list_overlay_visible && shopping_list_overlay) {
    lv_obj_move_foreground(shopping_list_overlay);
  }
}

// ── Border refresh ring helpers ──────────────────────────────────────
// One lv_arc hugging the screen edge, input-transparent, on top of all list
// content. Wired to the same refresh-SM state source the old pill used
// (shopping_list_refresh_indicator_sync, state-diffing, polled from the UI
// task); the pull gesture drives the ring angle directly during the drag.
// All LVGL — UI-task only.

static void shopping_list_ring_anim_opa_cb(void* var, int32_t value) {
  lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)value, 0);
}

static void shopping_list_ring_sweep_anim_cb(void* var, int32_t value) {
  shopping_list_ring_sweep_base = value % 360;
  lv_arc_set_angles((lv_obj_t*)var,
                    (uint16_t)shopping_list_ring_sweep_base,
                    (uint16_t)((shopping_list_ring_sweep_base + SHOPPING_LIST_RING_SWEEP_DEG) % 360));
}

// Finish: grow the segment from the sweep's last position until it closes
// into a full 360° ring (lv_arc draws start==end as empty, so snap to 0..360
// at the end instead of wrapping back onto itself).
static void shopping_list_ring_close_anim_cb(void* var, int32_t len) {
  if (len >= 359) {
    lv_arc_set_angles((lv_obj_t*)var, 0, 360);
  } else {
    lv_arc_set_angles((lv_obj_t*)var,
                      (uint16_t)shopping_list_ring_sweep_base,
                      (uint16_t)((shopping_list_ring_sweep_base + len) % 360));
  }
}

static void shopping_list_ring_hide_anim_ready(lv_anim_t* a) {
  lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa((lv_obj_t*)a->var, LV_OPA_COVER, 0);
  shopping_list_ring_hiding = false;
  shopping_list_ring_autohide = false;
  shopping_list_scroll_cue_sync();
}

// Kill every ring animation and restore a sane base state (full opacity).
static void shopping_list_ring_anim_reset(lv_obj_t* ring) {
  lv_anim_del(ring, shopping_list_ring_sweep_anim_cb);
  lv_anim_del(ring, shopping_list_ring_close_anim_cb);
  lv_anim_del(ring, shopping_list_ring_anim_opa_cb);
  shopping_list_ring_sweeping = false;
  shopping_list_ring_hiding = false;
  shopping_list_ring_autohide = false;
  lv_obj_set_style_opa(ring, LV_OPA_COVER, 0);
}

static void shopping_list_ring_set_style(lv_obj_t* ring, uint32_t color, int width) {
  lv_obj_set_style_arc_color(ring, lv_color_hex(color), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ring, width, LV_PART_INDICATOR);
}

static void shopping_list_ring_fade_out(uint32_t delay_ms, uint32_t time_ms) {
  lv_obj_t* ring = shopping_list_refresh_ring;
  if (!ring) return;
  lv_anim_del(ring, shopping_list_ring_anim_opa_cb);
  shopping_list_ring_hiding = true;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, ring);
  lv_anim_set_exec_cb(&a, shopping_list_ring_anim_opa_cb);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_time(&a, time_ms);
  lv_anim_set_delay(&a, delay_ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, shopping_list_ring_hide_anim_ready);
  lv_anim_start(&a);
}

static void shopping_list_ring_close_anim_ready(lv_anim_t* a) {
  (void)a;
  shopping_list_ring_fade_out(120, 300);  // brief full-ring hold, then fade
}

static void shopping_list_ring_flash_anim_ready(lv_anim_t* a) {
  (void)a;
  shopping_list_ring_fade_out(0, 300);
}

static void shopping_list_toast_hide_anim_ready(lv_anim_t* a) {
  lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa((lv_obj_t*)a->var, LV_OPA_COVER, 0);
}

// Transient toast near the top arc, auto-hides after ~2.5s. Errors only —
// the normal refresh states are text-free by design.
static void shopping_list_toast_show(const char* text) {
  lv_obj_t* toast = shopping_list_error_toast;
  if (!toast) return;
  lv_label_set_text(toast, text);
  lv_anim_del(toast, shopping_list_ring_anim_opa_cb);
  lv_obj_set_style_opa(toast, LV_OPA_COVER, 0);
  lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(toast);
  shopping_list_keep_overlay_on_top();
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, toast);
  lv_anim_set_exec_cb(&a, shopping_list_ring_anim_opa_cb);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_time(&a, 300);
  lv_anim_set_delay(&a, 2200);
  lv_anim_set_ready_cb(&a, shopping_list_toast_hide_anim_ready);
  lv_anim_start(&a);
}

// Pull drag: the ring fills clockwise from 12 o'clock proportional to pull
// progress (0→360° at the 45px threshold). Armed = full ring + width bump.
// Called only from drag events — no continuous animation.
static void shopping_list_ring_show_pull(int progress_pct, bool armed) {
  lv_obj_t* ring = shopping_list_refresh_ring;
  if (!ring) return;
  shopping_list_ring_anim_reset(ring);
  shopping_list_ring_set_style(ring, 0x1F4D2B,
                               armed ? SHOPPING_LIST_RING_WIDTH_ARMED : SHOPPING_LIST_RING_WIDTH);
  if (progress_pct < 0) progress_pct = 0;
  if (progress_pct > 100) progress_pct = 100;
  int angle = armed ? 360 : (360 * progress_pct) / 100;
  if (angle >= 360) {
    lv_arc_set_angles(ring, 0, 360);
  } else {
    lv_arc_set_angles(ring, 0, (uint16_t)angle);
  }
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(ring);
  shopping_list_keep_overlay_on_top();
}

// Refreshing (WAKE_PENDING + INFLIGHT): ~70° segment sweeping continuously
// around the border. One lv_anim on one small arc — cheap. No-op when the
// sweep is already running (so WAKE_PENDING -> INFLIGHT doesn't jump it).
static void shopping_list_ring_show_sweep() {
  lv_obj_t* ring = shopping_list_refresh_ring;
  if (!ring) return;
  bool visible = !lv_obj_has_flag(ring, LV_OBJ_FLAG_HIDDEN);
  if (shopping_list_ring_sweeping && visible && !shopping_list_ring_hiding) return;
  shopping_list_ring_anim_reset(ring);
  shopping_list_ring_set_style(ring, 0x1F4D2B, SHOPPING_LIST_RING_WIDTH);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(ring);
  shopping_list_keep_overlay_on_top();
  shopping_list_ring_sweeping = true;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, ring);
  lv_anim_set_exec_cb(&a, shopping_list_ring_sweep_anim_cb);
  lv_anim_set_values(&a, 0, 360);
  lv_anim_set_time(&a, SHOPPING_LIST_RING_SWEEP_MS);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_linear);
  lv_anim_start(&a);
}

// Complete: close the sweep into a full 360° ring (~260ms ease-out), hold a
// beat, fade out (~300ms).
static void shopping_list_ring_finish() {
  lv_obj_t* ring = shopping_list_refresh_ring;
  if (!ring) return;
  if (lv_obj_has_flag(ring, LV_OBJ_FLAG_HIDDEN) || shopping_list_ring_hiding) return;  // nothing visible to close
  shopping_list_ring_anim_reset(ring);
  shopping_list_ring_autohide = true;  // hide() must not cut the finish short
  shopping_list_ring_set_style(ring, 0x1F4D2B, SHOPPING_LIST_RING_WIDTH);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, ring);
  lv_anim_set_exec_cb(&a, shopping_list_ring_close_anim_cb);
  lv_anim_set_values(&a, SHOPPING_LIST_RING_SWEEP_DEG, 360);
  lv_anim_set_time(&a, 260);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, shopping_list_ring_close_anim_ready);
  lv_anim_start(&a);
}

// Failed/timeout: full ring flashes twice (~500ms) in the family error red
// (0x8B1E2D, the SHIP_ERROR background from the style guide), fades, plus a
// transient "Couldn't refresh" toast near the top arc.
static void shopping_list_ring_show_error() {
  lv_obj_t* ring = shopping_list_refresh_ring;
  if (!ring) return;
  shopping_list_ring_anim_reset(ring);
  shopping_list_ring_autohide = true;
  shopping_list_ring_set_style(ring, 0x8B1E2D, SHOPPING_LIST_RING_WIDTH);
  lv_arc_set_angles(ring, 0, 360);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(ring);
  shopping_list_keep_overlay_on_top();
  lv_anim_t a;  // two ~250ms flashes (fade down + playback up), then fade out
  lv_anim_init(&a);
  lv_anim_set_var(&a, ring);
  lv_anim_set_exec_cb(&a, shopping_list_ring_anim_opa_cb);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
  lv_anim_set_time(&a, 125);
  lv_anim_set_playback_time(&a, 125);
  lv_anim_set_repeat_count(&a, 2);
  lv_anim_set_ready_cb(&a, shopping_list_ring_flash_anim_ready);
  lv_anim_start(&a);
  shopping_list_toast_show("Couldn't refresh");
}

// Quick fade-hide for IDLE / cue-cancel. No-ops while a finish/error
// sequence is running (those own their fade-out via ring_autohide).
static void shopping_list_ring_hide() {
  lv_obj_t* ring = shopping_list_refresh_ring;
  if (!ring) return;
  if (lv_obj_has_flag(ring, LV_OBJ_FLAG_HIDDEN)) return;
  if (shopping_list_ring_hiding || shopping_list_ring_autohide) return;
  lv_anim_del(ring, shopping_list_ring_sweep_anim_cb);
  lv_anim_del(ring, shopping_list_ring_close_anim_cb);
  shopping_list_ring_sweeping = false;
  shopping_list_ring_fade_out(0, 150);
}

// Sync the ring with the refresh SM state. Cheap when nothing changed, so it
// is safe to call every UI-task iteration (the WAKE_PENDING -> INFLIGHT
// transition happens on the UART task, so the UI task polls for it here).
// Same state source / function name as the old pill — only the body changed.
static void shopping_list_refresh_indicator_sync(bool force) {
  shopping_list_scroll_cue_sync();
  if (!shopping_list_refresh_ring) return;
  int st = (int)refresh_state;
  if (!force && st == shopping_list_refresh_ui_state) return;
  shopping_list_refresh_ui_state = st;
  if (st == REFRESH_WAKE_PENDING || st == REFRESH_INFLIGHT) {
    if (shopping_list_refresh_show_ring) {
      shopping_list_ring_show_sweep();
    }
    // entry-revalidate (flag false): silent background refresh, ring stays hidden
  } else if (st == REFRESH_COMPLETE) {
    shopping_list_ring_finish();
  } else if (st == REFRESH_FAILED) {
    // show_error() does NOT early-return on a hidden ring (it force-clears the
    // hidden flag), so gate it too — a silent entry-revalidate that fails must
    // not surface a ring flash / toast.
    if (shopping_list_refresh_show_ring) shopping_list_ring_show_error();
  } else if (shopping_list_touch_in_press) {
    // IDLE mid-press: the gesture events own the ring during a pull drag
    // (the UI-task poll would otherwise fight the pull-progress fill with
    // fade-outs). Leave the cache unconsumed so the post-release sync still
    // sees the IDLE transition and hides a stale cue.
    shopping_list_refresh_ui_state = -1;
  } else {
    shopping_list_ring_hide();
  }
  shopping_list_scroll_cue_sync();
}

// Build the border ring + error toast (called from show_shopping_list_screen_impl;
// the screen is rebuilt fresh on each entry so these are too). Created as the
// LAST children of the screen so they render above all list content; both are
// input-transparent (CLICKABLE cleared) so they never block touches.
static void shopping_list_build_refresh_ring(lv_obj_t* parent) {
  shopping_list_refresh_ring = lv_arc_create(parent);
  lv_obj_set_size(shopping_list_refresh_ring, SHOPPING_LIST_RING_SIZE, SHOPPING_LIST_RING_SIZE);
  lv_obj_align(shopping_list_refresh_ring, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_rotation(shopping_list_refresh_ring, 270);  // angles measured from 12 o'clock
  lv_arc_set_bg_angles(shopping_list_refresh_ring, 0, 360);
  lv_arc_set_angles(shopping_list_refresh_ring, 0, 0);
  lv_obj_set_style_bg_opa(shopping_list_refresh_ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_opa(shopping_list_refresh_ring, LV_OPA_TRANSP, LV_PART_MAIN);  // track invisible — only the indicator shows
  lv_obj_set_style_arc_width(shopping_list_refresh_ring, SHOPPING_LIST_RING_WIDTH, LV_PART_MAIN);
  lv_obj_set_style_arc_color(shopping_list_refresh_ring, lv_color_hex(COL_GREEN), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(shopping_list_refresh_ring, SHOPPING_LIST_RING_WIDTH, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(shopping_list_refresh_ring, true, LV_PART_INDICATOR);  // rounded ends
  lv_obj_remove_style(shopping_list_refresh_ring, NULL, LV_PART_KNOB);  // no knob
  lv_obj_clear_flag(shopping_list_refresh_ring, LV_OBJ_FLAG_CLICKABLE);  // input-transparent
  lv_obj_add_flag(shopping_list_refresh_ring, LV_OBJ_FLAG_HIDDEN);

  // Error toast — small label card near the top arc, errors only
  shopping_list_error_toast = lv_label_create(parent);
  lv_label_set_text(shopping_list_error_toast, "");
  lv_obj_align(shopping_list_error_toast, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_set_style_text_font(shopping_list_error_toast, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(shopping_list_error_toast, lv_color_hex(COL_RED), LV_PART_MAIN);
  lv_obj_set_style_bg_color(shopping_list_error_toast, lv_color_hex(COL_WHITE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(shopping_list_error_toast, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(shopping_list_error_toast, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_left(shopping_list_error_toast, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_right(shopping_list_error_toast, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_top(shopping_list_error_toast, 5, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(shopping_list_error_toast, 5, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(shopping_list_error_toast, 6, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(shopping_list_error_toast, lv_color_hex(COL_DARK), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(shopping_list_error_toast, LV_OPA_30, LV_PART_MAIN);
  lv_obj_clear_flag(shopping_list_error_toast, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(shopping_list_error_toast, LV_OBJ_FLAG_HIDDEN);

  shopping_list_refresh_ui_state = -1;  // force first sync
  shopping_list_ring_hiding = false;
  shopping_list_ring_sweeping = false;
  shopping_list_ring_autohide = false;
  shopping_list_ring_sweep_base = 0;
}

static void shopping_list_show_overlay() {
  if (!shopping_list_screen || shopping_list_overlay_visible || g_active.count == 0 ||
      shopping_list_scroll_idx < 0 || shopping_list_scroll_idx >= g_active.count ||
      shopping_list_scroll_idx == shopping_list_pending_delete_index())
    return;
  strncpy(shopping_list_overlay_item_id, g_active.item_ids[shopping_list_scroll_idx],
          sizeof(shopping_list_overlay_item_id) - 1);
  shopping_list_overlay_item_id[sizeof(shopping_list_overlay_item_id) - 1] = '\0';
  shopping_list_overlay_visible = true;
  shopping_list_scroll_cue_sync();
  shopping_list_overlay = lv_obj_create(shopping_list_screen);
  lv_obj_remove_style_all(shopping_list_overlay);
  lv_obj_set_size(shopping_list_overlay, 360, 360);
  lv_obj_set_pos(shopping_list_overlay, 0, 0);
  lv_obj_set_style_bg_color(shopping_list_overlay, lv_color_hex(COL_DARK), 0);
  lv_obj_set_style_bg_opa(shopping_list_overlay, LV_OPA_80, 0);
  lv_obj_clear_flag(shopping_list_overlay, LV_OBJ_FLAG_SCROLLABLE);
  shopping_list_overlay_card = halo_ui_card(shopping_list_overlay, 52, 84, 256, 196, COL_WHITE, 20);
  lv_obj_set_style_shadow_ofs_x(shopping_list_overlay_card, 5, 0);
  lv_obj_set_style_shadow_ofs_y(shopping_list_overlay_card, 5, 0);
  halo_ui_label(shopping_list_overlay, "Delete", &lv_font_montserrat_14,
                COL_TEXT2, 65, 104, 230);
  char title[100];
  snprintf(title, sizeof(title), "%s?", g_active.items[shopping_list_scroll_idx]);
  lv_obj_t* name = halo_ui_label(shopping_list_overlay, title, &nunito_22, COL_RED, 65, 130, 230);
  lv_obj_set_height(name, 52);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  shopping_list_overlay_delete_btn =
      halo_ui_button(shopping_list_overlay, 76, 190, 208, 48, "DELETE", COL_RED, COL_WHITE);
  shopping_list_overlay_back_btn = halo_ui_button(shopping_list_overlay, 130, 242, 100, 34,
                                                  "CANCEL", COL_WHITE, COL_TEXT2, true);
  lv_obj_update_layout(shopping_list_overlay);
  lv_obj_move_foreground(shopping_list_overlay);
  shopping_list_touch_was_scrolled = false;
  Serial.printf("[SHOP_LIST] overlay shown for item %d: %s\n", shopping_list_scroll_idx,
                g_active.items[shopping_list_scroll_idx]);
}

static void shopping_list_dismiss_overlay() {
  if (!shopping_list_overlay_visible || !shopping_list_overlay) return;
  lv_obj_del(shopping_list_overlay);
  shopping_list_overlay = NULL;
  shopping_list_overlay_card = NULL;
  shopping_list_overlay_delete_btn = NULL;
  shopping_list_overlay_back_btn = NULL;
  shopping_list_overlay_visible = false;
  shopping_list_overlay_item_id[0] = '\0';
  shopping_list_scroll_cue_sync();
  Serial.println("[SHOP_LIST] overlay dismissed");
}

// Queue one displayed item; retain it in RAM and NVS until the backend confirms.
static const char* shopping_list_delete_index(int idx, const char* expected_id = NULL) {
  static char requested_id[64];
  requested_id[0] = '\0';
  if (shopping_list_pending_delete_id[0]) {
    return requested_id;
  }
  if (!app_state_mutex || xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    shopping_list_toast_show("Couldn't delete. Try again");
    return requested_id;
  }
  tx_msg_t tx_msg = {};
  if (idx < 0 || idx >= g_active.count || !g_active.item_ids[idx][0] ||
      strlen(g_active.item_ids[idx]) >= sizeof(tx_msg.id) ||
      (expected_id && strcmp(expected_id, g_active.item_ids[idx]) != 0) ||
      deleted_item_count >= MAX_DELETED_ITEMS) {
    xSemaphoreGive(app_state_mutex);
    shopping_list_toast_show("Couldn't delete. Try again");
    return requested_id;
  }
  strncpy(tx_msg.type, "INPUT_DELETE", sizeof(tx_msg.type) - 1);
  strncpy(tx_msg.id, g_active.item_ids[idx], sizeof(tx_msg.id) - 1);
  tx_msg.has_id = true;
  if (!uart_tx_enqueue(&tx_msg, "ship_screens")) {
    xSemaphoreGive(app_state_mutex);
    shopping_list_toast_show("Couldn't delete. Try again");
    return requested_id;
  }
  strncpy(requested_id, tx_msg.id, sizeof(requested_id) - 1);
  strcpy(shopping_list_pending_delete_id, requested_id);
  shopping_list_pending_delete_ms = millis();
  xSemaphoreGive(app_state_mutex);
  shopping_list_refresh_delete_view();
  Serial.printf("[SHOP_LIST] Sent INPUT_DELETE for ID: %s; awaiting result\n", requested_id);
  return requested_id;
}

static void shopping_list_delete_failure_toast() {
  shopping_list_refresh_delete_view();
  if (ui_screen_state == SCREEN_SHOPPING_LIST) shopping_list_toast_show("Couldn't delete. Try again");
  else show_auto_hiding_status_message("Couldn't delete. Try again", 2500);
}

// UI-task deadline: restore the provisional view if the backend result is lost.
static void shopping_list_expire_pending_delete() {
  if (shopping_list_pending_delete_id[0] &&
      (unsigned long)(millis() - shopping_list_pending_delete_ms) >= 60000UL) {
    Serial.printf("[SHOP_LIST] delete result timeout id=%s\n", shopping_list_pending_delete_id);
    shopping_list_pending_delete_id[0] = '\0';
    shopping_list_delete_failure_toast();
  }
}

// UI task only. Return the removed index; unmatched/late results
// and all failures leave the list and persistent cache unchanged.
static int shopping_list_apply_delete_result(const char* id, bool ok) {
  shopping_list_expire_pending_delete();
  if (!id || !id[0] || strcmp(id, shopping_list_pending_delete_id) != 0) return -1;
  shopping_list_pending_delete_id[0] = '\0';
  if (!ok) {
    shopping_list_delete_failure_toast();
    return -1;
  }
  if (!app_state_mutex || xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    shopping_list_delete_failure_toast();
    return -1;
  }
  int idx = -1;
  for (int i = 0; i < g_active.count; ++i) {
    if (strcmp(g_active.item_ids[i], id) == 0) { idx = i; break; }
  }
  if (idx >= 0) {
    if (deleted_item_count < MAX_DELETED_ITEMS) {
      strncpy(deleted_item_ids[deleted_item_count], id, 63);
      deleted_item_ids[deleted_item_count++][63] = '\0';
    }
    for (int j = idx; j < g_active.count - 1; ++j) {
      memcpy(g_active.items[j], g_active.items[j+1], sizeof(g_active.items[j]));
      memcpy(g_active.item_ids[j], g_active.item_ids[j+1], sizeof(g_active.item_ids[j]));
      memcpy(g_active.stores[j], g_active.stores[j+1], sizeof(g_active.stores[j]));
    }
    --g_active.count;
    g_active.items[g_active.count][0] = '\0';
    g_active.item_ids[g_active.count][0] = '\0';
    g_active.stores[g_active.count][0] = '\0';
    save_list_to_storage(&g_active);
  }
  xSemaphoreGive(app_state_mutex);
  if (shopping_list_scroll_idx >= g_active.count) shopping_list_scroll_idx = g_active.count ? g_active.count - 1 : 0;
  Serial.printf("[SHOP_LIST] confirmed delete id=%s removed_index=%d\n", id, idx);
  return idx;
}

// Kick the list-refresh state machine exactly like the pull-to-refresh gesture
// (touch pull-down past the top, or 3 CCW knob ticks at top): wake Sense, arm
// the refresh, and start the border-ring sweep.
// Touches LVGL — UI-task only. `reason` is for logging/wake tagging.
static void shopping_list_trigger_refresh(const char* reason) {
  const char* r = reason ? reason : "list_refresh";
  const bool show_ring = !(reason && strcmp(reason, "entry_revalidate") == 0);
  // Guard: never stack a second refresh while one is pending/inflight
  if (refresh_state == REFRESH_WAKE_PENDING || refresh_state == REFRESH_INFLIGHT) {
    // A user can join the silent refresh started on list entry. Promote only
    // its feedback; retain the existing request, deadline and animation phase.
    if (show_ring && !shopping_list_refresh_show_ring) {
      shopping_list_refresh_show_ring = true;
      shopping_list_refresh_indicator_sync(true);
    }
    Serial.printf("[SHOPPING_LIST] refresh joined (%s) — already %s\n",
                  r, refresh_state == REFRESH_INFLIGHT ? "inflight" : "wake_pending");
    return;
  }
  Serial.printf("[SHOPPING_LIST] refresh triggered (%s)\n", r);
  // Gate the spinning ring on the trigger reason: entry-revalidate is the only
  // silent one (clean list-open); touch_pull / encoder / usb all show the ring.
  // Set fresh on EVERY trigger so each refresh's ring visibility is correct.
  shopping_list_refresh_show_ring = show_ring;
  request_sense_wake(r);
  refresh_sm_set_wake_pending(r);
  // Show refreshing feedback (border-ring sweep)
  shopping_list_refresh_indicator_sync(true);
  ui_lvgl_tick();
}

// LVGL scroll-event callback on shopping_list_scroll: touch pull-to-refresh.
// Runs inside lv_timer_handler on the UI task, so direct LVGL calls are fine
// (do NOT call ui_lvgl_tick here — we're already inside the timer handler).
// Pull DOWN past the top (scroll_y < 0 thanks to LV_OBJ_FLAG_SCROLL_ELASTIC):
// past SHOPPING_LIST_PULL_HINT_PX a hint cue appears, past
// SHOPPING_LIST_PULL_THRESHOLD_PX the gesture is armed, and the refresh fires
// exactly once on release. Re-arms only after the scroll settles back to rest.
static void shopping_list_scroll_event_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* obj = lv_event_get_target(e);
  if (obj != shopping_list_scroll || ui_screen_state != SCREEN_SHOPPING_LIST) return;
  if (shopping_list_overlay_visible) return;

  bool refresh_busy = (refresh_state == REFRESH_WAKE_PENDING || refresh_state == REFRESH_INFLIGHT);
  lv_coord_t sy = lv_obj_get_scroll_y(obj);  // negative = pulled below the top edge

  if (code == LV_EVENT_PRESSED) {
    // New gesture begins
    shopping_list_touch_pull_armed = false;
    shopping_list_touch_in_press = true;
    shopping_list_touch_was_scrolled = false;
    // Remember where the finger landed so PRESSING can measure real movement.
    {
      lv_indev_t* indev = lv_indev_get_act();
      if (indev) lv_indev_get_point(indev, &shopping_list_touch_start_pt);
      else shopping_list_touch_start_pt.x = shopping_list_touch_start_pt.y = 0;
    }
    return;
  }

  // Movement-based drag detection. LV_EVENT_SCROLL alone was not enough: LVGL
  // only emits it when the container ACTUALLY scrolls, so a short drag, or a
  // drag while already pinned at the top/bottom, produced no SCROLL at all —
  // and the release was then treated as a tap, popping the Delete/Back overlay.
  // Measuring the finger instead catches every one of those.
  if (code == LV_EVENT_PRESSING && shopping_list_touch_in_press &&
      !shopping_list_touch_was_scrolled) {
    lv_indev_t* indev = lv_indev_get_act();
    if (indev) {
      lv_point_t p;
      lv_indev_get_point(indev, &p);
      int dx = (int)p.x - (int)shopping_list_touch_start_pt.x;
      int dy = (int)p.y - (int)shopping_list_touch_start_pt.y;
      if ((dx * dx + dy * dy) > (SHOPPING_LIST_TAP_SLOP_PX * SHOPPING_LIST_TAP_SLOP_PX)) {
        shopping_list_touch_was_scrolled = true;
      }
    }
    return;
  }

  if (code == LV_EVENT_SCROLL) {
    if (shopping_list_touch_in_press) {
      shopping_list_touch_was_scrolled = true;  // drag, not a tap — see shopping_list_handle_touch
    }
    if (refresh_busy || shopping_list_touch_pull_consumed) return;  // one fire per gesture, never stack
    if (sy <= -SHOPPING_LIST_PULL_HINT_PX) {
      // Progressive cue: the border ring fills clockwise from 12 o'clock as
      // the pull approaches the arm threshold (full ring + width bump = armed).
      int progress = (int)(((-sy) * 100) / SHOPPING_LIST_PULL_THRESHOLD_PX);
      shopping_list_touch_pull_armed = (sy <= -SHOPPING_LIST_PULL_THRESHOLD_PX);
      shopping_list_ring_show_pull(progress, shopping_list_touch_pull_armed);
      shopping_list_refresh_ui_state = -1;  // force re-sync once the SM takes over
    } else if (sy >= 0) {
      // Back at/above rest with no pull — drop the cue (no-op if none shown)
      shopping_list_touch_pull_armed = false;
      shopping_list_refresh_indicator_sync(false);
    }
    return;
  }

  if (code == LV_EVENT_RELEASED) {
    shopping_list_touch_in_press = false;
    if (shopping_list_touch_pull_armed && !shopping_list_touch_pull_consumed) {
      shopping_list_touch_pull_armed = false;
      shopping_list_touch_pull_consumed = true;  // until the scroll settles back to rest
      shopping_list_trigger_refresh("touch_pull");
    } else {
      shopping_list_refresh_indicator_sync(false);  // hide a stale cue (no-op otherwise)
    }
    return;
  }

  if (code == LV_EVENT_SCROLL_END) {
    if (sy >= 0) {
      // Gesture fully settled — re-arm for the next pull
      shopping_list_touch_pull_consumed = false;
      shopping_list_touch_pull_armed = false;
      shopping_list_refresh_indicator_sync(false);  // restores SM text or hides cue
    }
    return;
  }
}

// Handle a touch on the shopping list screen. Returns true if handled.
// x, y are screen coordinates (0-359).
static void shopping_list_style_card(lv_obj_t* card, int idx);
static bool shopping_list_handle_touch(int x, int y) {
  if (ui_screen_state != SCREEN_SHOPPING_LIST) return false;

  // The touch path has no drag filter — a quick scroll/pull would otherwise be
  // treated as a tap at the press point and pop the item overlay (or hit the
  // back button). If LVGL scrolled the list during this press, consume the
  // "tap" without acting on it.
  if (!shopping_list_overlay_visible) {
    // Two independent ways to recognise a drag, because neither alone is
    // reliable here:
    //  (a) the LVGL callback flagged it (SCROLL fired, or PRESSING saw motion);
    //  (b) this release point is simply far from where the finger landed.
    // (b) needs only LV_EVENT_PRESSED — which is definitely delivered — so it
    // still works if the finer-grained events never arrive. This touch path is
    // driven by raw coordinates, not LVGL's event tree, so it must do its own
    // check rather than trust a flag set elsewhere.
    // touch_move_max_d2 is the furthest the finger actually strayed from its
    // press point, sampled by the raw touch poll while the finger was down.
    // This is the ONLY reliable signal here: the coordinates handed to this
    // function are the PRESS point (the release handler dispatches with
    // touch_press_x/y), so comparing them to anything measures zero by
    // construction — which is why comparing against an LVGL press point, and
    // relying on LV_EVENT_SCROLL, both failed to catch real scrolls.
    const uint32_t slop2 =
        (uint32_t)SHOPPING_LIST_TAP_SLOP_PX * (uint32_t)SHOPPING_LIST_TAP_SLOP_PX;
    bool moved_far = (touch_move_max_d2 > slop2);
    if (shopping_list_touch_was_scrolled || moved_far) {
      Serial.printf("[SHOP_LIST] touch consumed (drag: flag=%d moved=%d max_d2=%lu slop2=%lu)\n",
                    shopping_list_touch_was_scrolled ? 1 : 0, moved_far ? 1 : 0,
                    (unsigned long)touch_move_max_d2, (unsigned long)slop2);
      shopping_list_touch_was_scrolled = false;
      return true;
    }
  }

  if (!shopping_list_overlay_visible && y >= 277 && y <= 325 && x >= 97 && x <= 145) {
    Serial.println("[SHOP_LIST] back button tapped");
    show_ship_main_menu();
    return true;
  }
  if (!shopping_list_overlay_visible && x >= 153 && x <= 263 && y >= 276 && y <= 321) {
    shopping_list_trigger_refresh("refresh_button");
    return true;
  }

  if (shopping_list_overlay_visible) {
    // Hitboxes derived from the RENDERED objects (lv_obj_get_coords on the
    // actual buttons/card, inflated by a few px of touch slop) — correct by
    // construction, regardless of label wrap or future layout tweaks.
    // Priority: delete btn -> back btn -> inside card (no-op) -> outside
    // card (dismiss). show_overlay calls lv_obj_update_layout so coords are
    // final before the first tap.
    const int SLOP = 8; // Cancel only; destructive bounds never extend into Cancel
    lv_area_t a;

    if (shopping_list_overlay_delete_btn) {
      lv_obj_get_coords(shopping_list_overlay_delete_btn, &a);
      if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
        Serial.printf("[SHOPPING_LIST] overlay tap (%d,%d) -> delete\n", x, y);
        int del_idx = -1;
        for (int i = 0; i < g_active.count; ++i) {
          if (shopping_list_overlay_item_id[0] &&
              strcmp(g_active.item_ids[i], shopping_list_overlay_item_id) == 0) {
            del_idx = i;
            break;
          }
        }
        shopping_list_delete_index(del_idx, shopping_list_overlay_item_id);

        // The queued row is hidden; its cached data waits for backend success.
        shopping_list_dismiss_overlay();
        return true;
      }
    }

    if (shopping_list_overlay_back_btn) {
      lv_obj_get_coords(shopping_list_overlay_back_btn, &a);
      if (x >= a.x1 - SLOP && x <= a.x2 + SLOP && y >= a.y1 - SLOP && y <= a.y2 + SLOP) {
        Serial.printf("[SHOPPING_LIST] overlay tap (%d,%d) -> dismiss (stay on list)\n", x, y);
        shopping_list_dismiss_overlay();
        return true;
      }
    }

    if (shopping_list_overlay_card) {
      lv_obj_get_coords(shopping_list_overlay_card, &a);
      if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
        // Card body (item name / dead space) — deliberate no-op so a slightly
        // missed button never dismisses the overlay out from under the user.
        Serial.printf("[SHOPPING_LIST] overlay tap (%d,%d) -> card\n", x, y);
        return true;
      }
    }

    // Tap outside card — dismiss overlay
    Serial.printf("[SHOPPING_LIST] overlay tap (%d,%d) -> dismiss\n", x, y);
    shopping_list_dismiss_overlay();
    return true;
  }

  // Touch selects the actual rendered row; empty space cannot delete a
  // dial-selected item that the user did not tap.
  if (y >= 82 && y <= 262) {
    for (int i = 0; i < shopping_list_rendered_count; ++i) {
      lv_obj_t* row = shopping_list_items[i];
      if (!row)
        continue;
      lv_area_t a;
      lv_obj_get_coords(row, &a);
      if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
        int old = shopping_list_scroll_idx;
        shopping_list_scroll_idx = i;
        shopping_list_style_card(shopping_list_items[old], old);
        shopping_list_style_card(row, i);
        shopping_list_show_overlay();
        break;
      }
    }
  }
  return true;
}

// All tappable rows share one appearance; the dial index only guides scrolling.
static void shopping_list_style_card(lv_obj_t* card, int idx) {
  if (!card)
    return;
  trepo_card(card, COL_WHITE, 18);
  lv_obj_set_style_shadow_ofs_x(card, 3, 0);
  lv_obj_set_style_shadow_ofs_y(card, 3, 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_border_side(card, LV_BORDER_SIDE_FULL, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(COL_DARK), 0);
  lv_obj_t* label = lv_obj_get_child(card, 0);
  if (label && idx >= 0 && idx < g_active.count) {
    lv_label_set_text(label, g_active.items[idx]);
    lv_obj_set_style_text_font(label, &nunito_18, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_DARK), 0);
    // Equal gutters keep item names centered within their pills.
    lv_obj_set_size(label, SHOPPING_LIST_CARD_W - 48, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
  }
  lv_obj_invalidate(card);
}

// scroll_to_view refuses a non-SCROLLABLE parent in LVGL 8. Keep touch scrolling
// disabled, but move the viewport explicitly for knob selection and rebuilds.
static void shopping_list_reveal_selection(int idx, lv_anim_enable_t anim) {
  if (!shopping_list_scroll || idx < 0 || idx >= shopping_list_rendered_count ||
      !shopping_list_items[idx]) return;
  lv_obj_update_layout(shopping_list_scroll);
  lv_area_t row, top, view;
  lv_obj_get_coords(shopping_list_items[idx], &row);
  lv_obj_get_coords(shopping_list_item_headers[idx] ? shopping_list_item_headers[idx]
                                                   : shopping_list_items[idx], &top);
  lv_obj_get_content_coords(shopping_list_scroll, &view);
  lv_coord_t y = lv_obj_get_scroll_y(shopping_list_scroll);
  if (top.y1 < view.y1) y -= view.y1 - top.y1;
  else if (row.y2 > view.y2) y += row.y2 - view.y2;
  lv_obj_scroll_to_y(shopping_list_scroll, y, anim);
}

// Lightweight scroll path: restyle just the two affected cards and keep the
// selection visible. Used by the encoder EVT_SCROLL_DELTA handler instead of
// a full repopulate (the rebuild-everything path made scrolling laggy).
static void shopping_list_update_selection(int old_idx, int new_idx) {
  if (!shopping_list_scroll) return;
  if (old_idx >= 0 && old_idx < shopping_list_rendered_count && shopping_list_items[old_idx]) {
    shopping_list_style_card(shopping_list_items[old_idx], old_idx);
  }
  if (new_idx >= 0 && new_idx < shopping_list_rendered_count && shopping_list_items[new_idx]) {
    shopping_list_style_card(shopping_list_items[new_idx], new_idx);
    shopping_list_reveal_selection(new_idx, LV_ANIM_ON);
  }
}

// lv_anim exec callback for the staggered row fade-in reveal
static void shopping_list_card_opa_anim_cb(void* var, int32_t value) {
  lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)value, 0);
}

// ── Content signature ────────────────────────────────────────────────
// FNV-1a over count + item texts + ids. Used to skip the rebuild (and the
// staggered reveal) entirely when a revalidate returns identical content —
// the common case — which also kills the flicker it used to cause.
static uint32_t shopping_list_content_sig(const app_state_t* s) {
  uint32_t h = 2166136261u;
  h = (h ^ (uint32_t)s->count) * 16777619u;
  for (int i = 0; i < s->count && i < MAX_LIST_ITEMS; i++) {
    for (const char* p = s->items[i]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0x1Fu) * 16777619u;  // field separator
    for (const char* p = s->item_ids[i]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0x1Du) * 16777619u;  // field separator (store)
    for (const char* p = s->stores[i]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0x1Eu) * 16777619u;  // record separator
  }
  if (h == 0xFFFFFFFFu) h = 0xFFFFFFFEu;  // 0xFFFFFFFF is the placeholder sentinel
  return h;
}

// ── Skeleton loader ──────────────────────────────────────────────────
// Four placeholder cards with a gentle opacity pulse, shown while the very
// first fetch runs and there is no cached data yet (never "No items" before
// a refresh has actually completed). The next populate() starts with
// lv_obj_clean(), which deletes the placeholders and kills their anims.
static void shopping_list_render_skeleton() {
  for (int i = 0; i < 4; i++) {
    lv_obj_t* ph = lv_obj_create(shopping_list_scroll);
    lv_obj_set_size(ph, SHOPPING_LIST_CARD_W, SHOPPING_LIST_CARD_H);
    lv_obj_set_style_radius(ph, SHOPPING_LIST_CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ph, lv_color_hex(COL_TERT), LV_PART_MAIN);  // light tan placeholder between the bg and the 0xD4C4AE cards
    lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ph, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ph, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ph);
    lv_anim_set_exec_cb(&a, shopping_list_card_opa_anim_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_40);
    lv_anim_set_time(&a, 450);
    lv_anim_set_playback_time(&a, 450);  // ~900ms full pulse
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_delay(&a, i * 110);  // soft stagger down the column
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
  }
}

// Store-group header row — a small, dim, NON-selectable label inserted above
// each contiguous group of same-store items. It is a separate LVGL object and
// is deliberately NOT stored in shopping_list_items[] nor counted as an item:
// selection/delete stay index-based on items only. The flex column auto-
// positions it between groups. Input-transparent so touches pass through.
// Returns the created header label so the populate loop can track it in
// shopping_list_item_headers[] (parallel to the item it precedes) for
// header-aware selection scrolling.
static lv_obj_t* shopping_list_add_store_header(const char* store) {
  lv_obj_t* hdr = lv_label_create(shopping_list_scroll);
  // Uppercase the store name into a small buffer for a quieter, "section" look.
  char up[48];
  size_t n = 0;
  for (; store[n] != '\0' && n < sizeof(up) - 1; n++) {
    up[n] = (char)toupper((unsigned char)store[n]);
  }
  up[n] = '\0';
  lv_label_set_text(hdr, up);
  lv_obj_set_width(hdr, SHOPPING_LIST_CARD_W - 8);
  lv_obj_set_style_text_font(hdr, &nunito_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(hdr, lv_color_hex(COL_MUTED),
                              LV_PART_MAIN); // dim tan, lighter than cards
  lv_obj_set_style_text_align(hdr, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_obj_set_style_pad_left(hdr, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_top(hdr, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(hdr, 0, LV_PART_MAIN);
  // Not tappable / not selectable — never participates in touch hit-testing.
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
  return hdr;
}

// Full rebuild path — used ONLY when content changes (new UI_LIST arrived,
// delete, screen entry, refresh timeout revert). Scroll-only selection moves
// go through shopping_list_update_selection() instead.
static void shopping_list_screen_populate() {
  if (!shopping_list_scroll) return;

  // Clear existing items
  lv_obj_clean(shopping_list_scroll);
  // Re-arm the touch pull-to-refresh latch. lv_obj_clean() can interrupt
  // LVGL's scroll-end delivery: when a fast refresh completes mid-elastic-
  // snap-back, the emptied container never receives LV_EVENT_SCROLL_END,
  // so the once-per-gesture latch in shopping_list_scroll_event_cb would
  // stay consumed forever and silently reject every subsequent pull.
  shopping_list_touch_pull_consumed = false;
  shopping_list_touch_pull_armed = false;
  // Belt-and-braces: if the rebuild lands mid-pull the container's scroll_y
  // can still be negative with no snap-back coming — force it to rest BEFORE
  // rows are re-added (the selection reveal logic below then starts
  // from a clean origin).
  lv_obj_scroll_to_y(shopping_list_scroll, 0, LV_ANIM_OFF);
  shopping_list_rendered_count = 0;
  for (int i = 0; i < 50; i++) shopping_list_items[i] = NULL;  // no stale card pointers
  for (int k = 0; k < 50; k++) shopping_list_item_headers[k] = NULL;  // no stale header pointers
  bool reveal = shopping_list_reveal_pending;
  shopping_list_reveal_pending = false;
  const int pending_delete_index = shopping_list_pending_delete_index();
  const int visible_count = shopping_list_visible_count();

  if (shopping_list_title_label)
    lv_label_set_text(shopping_list_title_label, "Shopping list");
  if (shopping_list_count_label) {
    char count[28];
    if (g_active.count > 0 || g_list_refresh_completed_once)
      snprintf(count, sizeof(count), "%d ITEMS", visible_count);
    else
      snprintf(count, sizeof(count), "%s",
               refresh_state == REFRESH_FAILED ? "NOT CONNECTED" : "LOADING YOUR LIST");
    lv_label_set_text(shopping_list_count_label, count);
  }

  if (visible_count == 0) {
    bool refresh_running = (refresh_state == REFRESH_WAKE_PENDING ||
                            refresh_state == REFRESH_INFLIGHT);
    // Honest failure beats an endless loader. If a refresh has NEVER completed
    // and we have already timed out repeatedly, the Sense isn't answering — say
    // so, instead of cycling the skeleton against a hint forever. Previously
    // this alternated skeleton (while retrying) and "Tap Refresh to update"
    // (between retries), which reads as a flashing, broken screen.
    // Design rule: "Show an honest error instead of a fabricated result."
    bool sense_unreachable = (!g_list_refresh_completed_once &&
                              refresh_timeout_count >= LIST_HONEST_FAIL_TIMEOUTS);
    if (!g_list_refresh_completed_once && refresh_running && !sense_unreachable) {
      // No data yet (first boot / cache miss) and a refresh is underway —
      // show the pulsing skeleton instead of a premature "No items".
      shopping_list_render_skeleton();
      shopping_list_rendered_sig = 0xFFFFFFFFu;  // placeholder, never matches real content
      return;
    }
    if (sense_unreachable) {
      lv_obj_t* glyph = lv_label_create(shopping_list_scroll);
      lv_label_set_text(glyph, LV_SYMBOL_WARNING);   // FontAwesome -> Montserrat, not Nunito
      lv_obj_set_style_text_font(glyph, &lv_font_montserrat_32, LV_PART_MAIN);
      lv_obj_set_style_text_color(glyph, lv_color_hex(COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_pad_top(glyph, 34, LV_PART_MAIN);

      lv_obj_t* msg = lv_label_create(shopping_list_scroll);
      lv_label_set_text(msg, "Can't reach your kitchen");
      lv_obj_set_style_text_font(msg, &nunito_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(msg, lv_color_hex(COL_DARK), LV_PART_MAIN);
      lv_obj_set_style_pad_top(msg, 8, LV_PART_MAIN);
      lv_obj_set_width(msg, 240);
      lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

      lv_obj_t* hint = lv_label_create(shopping_list_scroll);
      lv_label_set_text(hint, "Tap Refresh to try again");
      lv_obj_set_style_text_font(hint, &nunito_12, LV_PART_MAIN);
      lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_pad_top(hint, 2, LV_PART_MAIN);
      lv_obj_set_width(hint, 240);
      lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

      shopping_list_rendered_sig = 0xFFFFFFFFu;
      return;
    }
    // Empty state. The friendly glyph + "No items on your list" only when a
    // refresh has actually completed (the emptiness is genuine); otherwise a
    // neutral hint.
    if (g_list_refresh_completed_once) {
      lv_obj_t* glyph = lv_label_create(shopping_list_scroll);
      lv_label_set_text(glyph, LV_SYMBOL_LIST);
      lv_obj_set_style_text_font(glyph, &lv_font_montserrat_32, LV_PART_MAIN);
      lv_obj_set_style_text_color(glyph, lv_color_hex(COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_pad_top(glyph, 34, LV_PART_MAIN);

      lv_obj_t* empty = lv_label_create(shopping_list_scroll);
      lv_label_set_text(empty, "No items on your list");
      lv_obj_set_style_text_font(empty, &nunito_22, LV_PART_MAIN);
      lv_obj_set_style_text_color(empty, lv_color_hex(COL_DARK), LV_PART_MAIN);
      lv_obj_set_style_pad_top(empty, 8, LV_PART_MAIN);
      lv_obj_set_width(empty, 240);
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

      lv_obj_t* hint = lv_label_create(shopping_list_scroll);
      lv_label_set_text(hint, "Tap Refresh to update");
      lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_pad_top(hint, 2, LV_PART_MAIN);
      lv_obj_set_width(hint, 240);
      lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
      lv_obj_t* empty = lv_label_create(shopping_list_scroll);
      lv_label_set_text(empty, "Tap Refresh to update");
      lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, LV_PART_MAIN);
      lv_obj_set_style_text_color(empty, lv_color_hex(COL_MUTED), LV_PART_MAIN);
      lv_obj_set_style_pad_top(empty, 64, LV_PART_MAIN);
      lv_obj_set_width(empty, 240);
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    shopping_list_rendered_sig = g_list_refresh_completed_once
                                     ? shopping_list_content_sig(&g_active)
                                     : 0xFFFFFFFFu;
    return;
  }

  // Clamp scroll index
  if (shopping_list_scroll_idx >= g_active.count) shopping_list_scroll_idx = g_active.count - 1;
  if (shopping_list_scroll_idx < 0) shopping_list_scroll_idx = 0;
  shopping_list_scroll_idx = shopping_list_visible_index(shopping_list_scroll_idx, 1);

  // Create item cards — fixed-height rounded rows sized for the round
  // display (272px clears the circle at every height the cards reach)
  int previous_visible = -1;
  for (int i = 0; i < g_active.count && i < 50; i++) {
    if (i == pending_delete_index) continue;
    // Store-group header: items arrive pre-sorted by store (empty store last),
    // so a store change from the previous item starts a new group. The header
    // is a separate, non-selectable object — it does NOT consume an item index.
    if (previous_visible < 0 || strcmp(g_active.stores[i], g_active.stores[previous_visible]) != 0) {
      const char* store = g_active.stores[i];
      // Empty-store items sort last; label that trailing group "Other".
      // Track the header parallel to its item so selection scroll can reveal it.
      shopping_list_item_headers[i] =
          shopping_list_add_store_header((store && store[0] != '\0') ? store : "Other");
    }

    lv_obj_t* card = lv_obj_create(shopping_list_scroll);
    lv_obj_set_size(card, SHOPPING_LIST_CARD_W, SHOPPING_LIST_CARD_H);
    lv_obj_set_style_radius(card, SHOPPING_LIST_CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_left(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_top(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, lv_color_hex(COL_DARK), LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Label first (style_card rewrites its text/font/color) — single line,
    // vertically centered, ellipsized
    lv_obj_t* label = lv_label_create(card);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    shopping_list_style_card(card, i);

    shopping_list_items[i] = card;
    previous_visible = i;
  }
  // Slots retain their source indices, including the hole for a pending delete.
  shopping_list_rendered_count = g_active.count < 50 ? g_active.count : 50;

  // Reveal the selected card together with its store header, when present.
  shopping_list_reveal_selection(shopping_list_scroll_idx, LV_ANIM_OFF);

  // Remember what's rendered so a revalidate with identical content can skip
  // the rebuild (and its reveal/flicker) entirely.
  shopping_list_rendered_sig = shopping_list_content_sig(&g_active);

  // Fresh-list reveal: quick staggered fade-in of the first rows so a new
  // UI_LIST landing feels alive. One-shot — no continuous animations after.
  if (reveal && shopping_list_rendered_count > 0) {
    int reveal_n = shopping_list_rendered_count < 10 ? shopping_list_rendered_count : 10;
    for (int i = 0; i < reveal_n; i++) {
      lv_obj_t* card = shopping_list_items[i];
      if (!card) continue;
      lv_anim_del(card, shopping_list_card_opa_anim_cb);
      lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, card);
      lv_anim_set_exec_cb(&a, shopping_list_card_opa_anim_cb);
      lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
      lv_anim_set_time(&a, 180);
      lv_anim_set_delay(&a, i * 30);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_start(&a);
    }
    // Rows beyond the first ~10 appear instantly (created at full opacity)
  }
}

// Thin stack of screen-bg strips fading out toward the list so rows dissolve
// at the circle's narrow zones instead of hard-clipping. LVGL 8 styles have
// no opacity gradients, so a 4-step strip stack approximates one. The strips
// are input-transparent (CLICKABLE cleared) — touches pass to the list below.
// Edge fade: strips of the page ground laid over the list, stepping down in
// opacity so rows dissolve into the bezel instead of being cut off.
// Was 4 strips x 8px with a steep {255,165,95,40} ramp — few enough steps and
// large enough jumps that the banding was visible. Now 12 strips x 3px over the
// same 36px, on a quadratic ramp (fast at the covered end, long gentle tail),
// which is what reads as smooth. Still plain property styling, no gradients or
// transforms — cheap, and it respects the no-transform-animation rule.
#define LIST_FADE_STEPS  12
#define LIST_FADE_PX      3
static void shopping_list_add_edge_fade(lv_obj_t* parent, int x, int y, int w, bool top) {
  for (int i = 0; i < LIST_FADE_STEPS; i++) {
    // t: 0 at the fully-covered edge -> 1 at the transparent end
    int t = (i * 100) / (LIST_FADE_STEPS - 1);
    int opa = (255 * (100 - t) * (100 - t)) / 10000;   // quadratic ease-out
    if (opa > 255) opa = 255;
    if (opa < 0) opa = 0;
    lv_obj_t* strip = lv_obj_create(parent);
    lv_obj_set_size(strip, w, LIST_FADE_PX);
    lv_obj_set_pos(strip, x, top ? (y + i * LIST_FADE_PX)
                                 : (y - (i + 1) * LIST_FADE_PX));
    lv_obj_set_style_bg_color(strip, lv_color_hex(COL_CREAM), LV_PART_MAIN);  // screen bg → transparent steps
    lv_obj_set_style_bg_opa(strip, (lv_opa_t)opa, LV_PART_MAIN);
    lv_obj_set_style_border_width(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(strip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);
  }
}

static void show_shopping_list_screen_impl() {
  // Always rebuild the screen fresh
  if (shopping_list_screen) {
    if (lv_scr_act() == shopping_list_screen)
      lv_scr_load(g_base_screen);
    lv_obj_del(shopping_list_screen);
    shopping_list_screen = NULL;
  }
  shopping_list_overlay = NULL;
  shopping_list_overlay_card = NULL;
  shopping_list_overlay_delete_btn = NULL;
  shopping_list_overlay_back_btn = NULL;
  shopping_list_overlay_visible = false;
  shopping_list_refresh_ring = NULL;  // children of the deleted screen
  shopping_list_error_toast = NULL;
  shopping_list_ring_hiding = false;
  shopping_list_ring_sweeping = false;
  shopping_list_ring_autohide = false;
  shopping_list_rendered_sig = 0xFFFFFFFFu;  // fresh screen — force a real render

  shopping_list_screen = lv_obj_create(NULL);
  lv_obj_set_size(shopping_list_screen, LV_PCT(100), LV_PCT(100));
  ship_style_plain_screen(shopping_list_screen, lv_color_hex(COL_CREAM));
  lv_obj_set_style_border_width(shopping_list_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(shopping_list_screen, 0, LV_PART_MAIN);
  lv_obj_clear_flag(shopping_list_screen, LV_OBJ_FLAG_SCROLLABLE);

  shopping_list_title_label =
      halo_ui_label(shopping_list_screen, "Shopping list", &nunito_22, COL_DARK, 50, 26, 260);
  shopping_list_count_label =
      halo_ui_label(shopping_list_screen, "", &nunito_12, COL_MUTED, 70, 58, 220);

  // ── Scrollable item list (y 60..290 — clears the title and back button) ──
  shopping_list_scroll = lv_obj_create(shopping_list_screen);
  lv_obj_set_size(shopping_list_scroll, 260, 182);
  lv_obj_set_pos(shopping_list_scroll, 50, 82);
  lv_obj_set_style_bg_opa(shopping_list_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(shopping_list_scroll, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(shopping_list_scroll, 0, LV_PART_MAIN);
  // Generous top/bottom pads so the first and last rows can rest in the
  // comfortable middle band, clear of the edge fades.
  lv_obj_set_style_pad_top(shopping_list_scroll, 3, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(shopping_list_scroll, 5, LV_PART_MAIN);
  lv_obj_set_style_pad_left(shopping_list_scroll, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_right(shopping_list_scroll, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(shopping_list_scroll, SHOPPING_LIST_CARD_GAP, LV_PART_MAIN);
  lv_obj_set_flex_flow(shopping_list_scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(shopping_list_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(shopping_list_scroll, LV_SCROLLBAR_MODE_OFF);  // a straight scrollbar fights the round bezel
  lv_obj_set_scroll_dir(shopping_list_scroll, LV_DIR_VER);
  // KNOB-ONLY SCROLLING. Explicit scroll_to_y in reveal_selection works with
  // this flag cleared; LVGL's scroll_to_view does not.
  // Rationale: on a 360px round panel a swipe and a tap are hard to tell apart,
  // and a misread swipe fired the item overlay. The knob is unambiguous.
  // Refresh is NOT lost with the touch pull — the knob keeps its own gesture
  // (CCW ticks at the top of the list), which is the documented path anyway.
  lv_obj_clear_flag(shopping_list_scroll, LV_OBJ_FLAG_SCROLLABLE);
  // Touch pull-to-refresh: elastic overscroll past the top + momentum flicks,
  // with scroll events driving the pull gesture detector.
  lv_obj_add_flag(shopping_list_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_add_flag(shopping_list_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_PRESSED, NULL);
  // PRESSING is what carries the finger's live position. Without it the
  // movement-based drag filter in the callback never runs at all, and a scroll
  // that LVGL doesn't turn into a SCROLL event still lands as a tap.
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_SCROLL, NULL);
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_SCROLL_END, NULL);

  // The bounded central viewport needs no layered fade strips.
  shopping_list_back_btn_obj = halo_ui_back(shopping_list_screen, 97, 277);
  shopping_list_refresh_btn =
      halo_ui_button(shopping_list_screen, 153, 276, 110, 45, "REFRESH", COL_WHITE, COL_DARK);

  shopping_list_build_scroll_cue(shopping_list_screen);

  // ── Border refresh ring + error toast (last children — top layer) ──
  shopping_list_build_refresh_ring(shopping_list_screen);

  // Populate
  shopping_list_scroll_idx = 0;
  shopping_list_overscroll_ticks = 0;
  shopping_list_last_ccw_tick_ms = 0;
  shopping_list_touch_pull_armed = false;
  shopping_list_touch_pull_consumed = false;
  shopping_list_touch_in_press = false;
  shopping_list_touch_was_scrolled = false;

  // Stale-while-revalidate: the cached list renders instantly below and stays
  // fully interactive; ALWAYS kick a background refresh on entry unless one
  // is already pending/inflight (the trigger guard handles that). Triggered
  // BEFORE populate so a cache-miss entry shows the skeleton, not "No items".
  if (refresh_state != REFRESH_WAKE_PENDING && refresh_state != REFRESH_INFLIGHT) {
    shopping_list_trigger_refresh("entry_revalidate");
  }

  shopping_list_screen_populate();

  // If a refresh is already pending/inflight when the user enters the list,
  // surface the border ring sweep immediately.
  shopping_list_refresh_indicator_sync(true);

  ui_screen_state = SCREEN_SHOPPING_LIST;
  resetActivityTimer();  // Give every entry a fresh viewing interval.
  ui_busy = false;
  lv_scr_load(shopping_list_screen);
  Serial.println("[MENU] screen=SHOPPING_LIST");
  lv_timer_handler();
}

static void show_shopping_list_screen() {
  UI_SHOW(SCREEN_SHIP_SHOPPING_LIST, "show_shopping_list");
}

// ── Error Log Screen ─────────────────────────────────────────────────
static lv_obj_t* errlog_screen = NULL;
static lv_obj_t* errlog_scroll_container = NULL;
static lv_obj_t* errlog_count_label = NULL;
static lv_obj_t* errlog_btn_back = NULL;
static lv_obj_t* errlog_btn_clear = NULL;

// ── Error Log Detail Screen ──────────────────────────────────────────
static lv_obj_t* errlog_detail_screen = NULL;
static lv_obj_t* errlog_detail_scroll = NULL;
static lv_obj_t* errlog_detail_btn_back = NULL;

// Forward declarations
static void show_errlog_detail(int index);
static void show_errlog_screen();

// ── Detail screen helpers ────────────────────────────────────────────

static lv_obj_t* errlog_detail_add_section(lv_obj_t* parent, const char* text) {
  lv_obj_t* lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_width(lbl, 210);
  lv_obj_set_style_text_color(lbl, lv_color_hex(COL_GOLD), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_pad_top(lbl, 6, LV_PART_MAIN);
  return lbl;
}

static lv_obj_t* errlog_detail_add_field(lv_obj_t* parent, const char* name, const char* value) {
  char line[128];
  snprintf(line, sizeof(line), "%s: %s", name, value);
  lv_obj_t* lbl = lv_label_create(parent);
  lv_label_set_text(lbl, line);
  lv_obj_set_width(lbl, 210);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, LV_PART_MAIN);
  return lbl;
}

static lv_obj_t* errlog_detail_add_separator(lv_obj_t* parent) {
  lv_obj_t* sep = lv_obj_create(parent);
  lv_obj_set_size(sep, 208, 1);
  lv_obj_set_style_bg_color(sep, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(sep, 0, LV_PART_MAIN);
  return sep;
}

// Map context field keys to human-readable names
static const char* errlog_context_label(const char* key) {
  if (strcmp(key, "heap") == 0) return "Heap";
  if (strcmp(key, "rssi") == 0) return "RSSI";
  if (strcmp(key, "op") == 0) return "Operation";
  if (strcmp(key, "fg") == 0) return "Foreground";
  if (strcmp(key, "http") == 0) return "HTTP Active";
  if (strcmp(key, "upl_q") == 0) return "Upload Queue";
  if (strcmp(key, "boot") == 0) return "Boot Count";
  if (strcmp(key, "sense") == 0) return "Sense State";
  if (strcmp(key, "screen") == 0) return "Screen";
  return key; // fallback: use raw key
}

// Format context field value with units/labels
static void errlog_format_context_value(const char* key, const char* raw, char* out, size_t out_size) {
  if (strcmp(key, "rssi") == 0) {
    snprintf(out, out_size, "%s dBm", raw);
  } else if (strcmp(key, "fg") == 0) {
    snprintf(out, out_size, "%s", strcmp(raw, "1") == 0 ? "yes" : "no");
  } else if (strcmp(key, "http") == 0) {
    snprintf(out, out_size, "%s", strcmp(raw, "1") == 0 ? "yes" : "no");
  } else {
    strlcpy(out, raw, out_size);
  }
}

static void errlog_detail_btn_back_event(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  Serial.println("[ERRLOG] detail back -> errlog list");
  show_errlog_screen();
}

static void errlog_detail_screen_init() {
  if (errlog_detail_screen)
    return;
  errlog_detail_screen = halo_ui_page(COL_DARK);
  halo_ui_label(errlog_detail_screen, "SUPPORT", &nunito_12, COL_GOLD, 70, 30, 220);
  halo_ui_label(errlog_detail_screen, "Log detail", &nunito_22, COL_WHITE, 50, 52, 260);
  errlog_detail_scroll = halo_ui_card(errlog_detail_screen, 62, 96, 236, 160, 0x303030, 18);
  lv_obj_set_style_border_color(errlog_detail_scroll, lv_color_hex(0x555555), 0);
  lv_obj_set_style_pad_all(errlog_detail_scroll, 10, 0);
  lv_obj_set_flex_flow(errlog_detail_scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(errlog_detail_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_add_flag(errlog_detail_scroll, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(errlog_detail_scroll, LV_SCROLLBAR_MODE_AUTO);
  errlog_detail_btn_back = halo_ui_back(errlog_detail_screen);
  lv_obj_add_event_cb(errlog_detail_btn_back, errlog_detail_btn_back_event, LV_EVENT_CLICKED, NULL);
}

static void show_errlog_detail(int index) {
  errlog_detail_screen_init();
  lv_obj_clean(errlog_detail_scroll);

  char buf[256];
  if (!errlog_read_entry(index, buf, sizeof(buf))) {
    lv_obj_t* err = lv_label_create(errlog_detail_scroll);
    lv_label_set_text(err, "Failed to read entry");
    lv_obj_set_style_text_color(err, lv_color_hex(COL_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(err, &lv_font_montserrat_14, LV_PART_MAIN);
  } else {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) {
      lv_obj_t* err = lv_label_create(errlog_detail_scroll);
      lv_label_set_text(err, "Parse error");
      lv_obj_set_style_text_color(err, lv_color_hex(COL_RED), LV_PART_MAIN);
      lv_obj_set_style_text_font(err, &lv_font_montserrat_14, LV_PART_MAIN);
    } else {
      const char* board  = doc["board"]  | "?";
      const char* area   = doc["area"]   | "?";
      const char* event  = doc["event"]  | "?";
      int32_t code       = doc["code"]   | 0;
      const char* detail = doc["detail"] | "";
      float uptime       = doc["up"]     | 0.0f;

      // Header fields
      errlog_detail_add_field(errlog_detail_scroll, "Board", board);
      errlog_detail_add_field(errlog_detail_scroll, "Area", area);
      errlog_detail_add_field(errlog_detail_scroll, "Event", event);
      char code_str[16];
      snprintf(code_str, sizeof(code_str), "%ld", (long)code);
      errlog_detail_add_field(errlog_detail_scroll, "Code", code_str);
      if (uptime > 0) {
        char up_str[16];
        snprintf(up_str, sizeof(up_str), "%.1fs", uptime);
        errlog_detail_add_field(errlog_detail_scroll, "Uptime", up_str);
      }

      // Split detail on " | " to separate error text from context
      if (detail[0]) {
        errlog_detail_add_separator(errlog_detail_scroll);
        errlog_detail_add_section(errlog_detail_scroll, "Detail");

        // Make a mutable copy for splitting
        char detail_copy[160];
        strlcpy(detail_copy, detail, sizeof(detail_copy));

        char* pipe_pos = strstr(detail_copy, " | ");
        if (pipe_pos) {
          *pipe_pos = '\0';
          const char* error_text = detail_copy;
          const char* context_str = pipe_pos + 3;

          // Show the error text
          lv_obj_t* err_lbl = lv_label_create(errlog_detail_scroll);
          lv_label_set_text(err_lbl, error_text);
          lv_obj_set_width(err_lbl, 210);
          lv_label_set_long_mode(err_lbl, LV_LABEL_LONG_WRAP);
          lv_obj_set_style_text_color(err_lbl, lv_color_hex(0xf0883e), LV_PART_MAIN);
          lv_obj_set_style_text_font(err_lbl, &lv_font_montserrat_12, LV_PART_MAIN);

          // Parse context fields
          errlog_detail_add_separator(errlog_detail_scroll);
          errlog_detail_add_section(errlog_detail_scroll, "Context");

          char ctx_copy[128];
          strlcpy(ctx_copy, context_str, sizeof(ctx_copy));
          char* token = strtok(ctx_copy, " ");
          while (token) {
            char* eq = strchr(token, '=');
            if (eq) {
              *eq = '\0';
              const char* key = token;
              const char* val = eq + 1;
              const char* label = errlog_context_label(key);
              char formatted[48];
              errlog_format_context_value(key, val, formatted, sizeof(formatted));
              errlog_detail_add_field(errlog_detail_scroll, label, formatted);
            }
            token = strtok(NULL, " ");
          }
        } else {
          // No pipe separator -- show full detail as error text
          lv_obj_t* err_lbl = lv_label_create(errlog_detail_scroll);
          lv_label_set_text(err_lbl, detail_copy);
          lv_obj_set_width(err_lbl, 210);
          lv_label_set_long_mode(err_lbl, LV_LABEL_LONG_WRAP);
          lv_obj_set_style_text_color(err_lbl, lv_color_hex(0xf0883e), LV_PART_MAIN);
          lv_obj_set_style_text_font(err_lbl, &lv_font_montserrat_12, LV_PART_MAIN);
        }
      }
    }
  }

  ui_screen_state = SCREEN_ERRLOG_DETAIL;
  ui_busy = false;
  lv_scr_load(errlog_detail_screen);
  Serial.printf("[MENU] screen=ERRLOG_DETAIL (entry %d)\n", index);
  lv_timer_handler();
}

// ── Error Log List Screen ────────────────────────────────────────────

static void errlog_entry_click_event(lv_event_t* e) {
  int index = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  Serial.printf("[ERRLOG] tapped entry %d\n", index);
  show_errlog_detail(index);
}

static void errlog_screen_populate() {
  lv_obj_clean(errlog_scroll_container);

  int count = errlog_count();
  char count_text[32];
  snprintf(count_text, sizeof(count_text), "(%d entries)", count);
  lv_label_set_text(errlog_count_label, count_text);

  if (count == 0) {
    lv_obj_t* empty = lv_label_create(errlog_scroll_container);
    lv_label_set_text(empty, "No errors recorded");
    lv_obj_set_style_text_color(empty, lv_color_hex(0x8b949e), LV_PART_MAIN);
    lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, LV_PART_MAIN);
    return;
  }

  char buf[256];
  for (int i = 0; i < count && i < 20; i++) {
    if (!errlog_read_entry(i, buf, sizeof(buf))) continue;

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) continue;

    const char* board = doc["board"] | "?";
    const char* area = doc["area"] | "?";
    const char* event = doc["event"] | "?";
    int32_t code = doc["code"] | 0;

    // Short summary line only (detail shown on tap)
    char display[80];
    snprintf(display, sizeof(display), "[%s] %s: %s (%ld)", board, area, event, (long)code);

    // Tappable container
    lv_obj_t* entry = lv_obj_create(errlog_scroll_container);
    lv_obj_set_width(entry, 212);
    lv_obj_set_height(entry, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(entry, lv_color_hex(0x1c2128), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(entry, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(entry, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_border_width(entry, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(entry, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(entry, 8, LV_PART_MAIN);
    lv_obj_clear_flag(entry, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(entry, (void*)(intptr_t)i);
    lv_obj_add_event_cb(entry, errlog_entry_click_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl = lv_label_create(entry);
    lv_label_set_text(lbl, display);
    lv_obj_set_width(lbl, 190);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_WHITE), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, LV_PART_MAIN);
  }
}

static void errlog_btn_back_event(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  Serial.println("[ERRLOG] back -> settings");
  show_ship_settings_screen();
}

static void errlog_btn_clear_event(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
    return;
  }
  Serial.println("[ERRLOG] clearing all error logs");
  errlog_clear();
  errlog_screen_populate();
}

static void errlog_screen_init() {
  if (errlog_screen)
    return;
  errlog_screen = halo_ui_page(COL_DARK);
  halo_ui_label(errlog_screen, "SUPPORT", &nunito_12, COL_GOLD, 70, 30, 220);
  halo_ui_label(errlog_screen, "Error log", &nunito_22, COL_WHITE, 50, 52, 260);
  errlog_count_label = halo_ui_label(errlog_screen, "", &nunito_12, COL_MUTED, 70, 78, 220);
  errlog_scroll_container = halo_ui_card(errlog_screen, 62, 100, 236, 144, 0x303030, 18);
  lv_obj_set_style_border_color(errlog_scroll_container, lv_color_hex(0x555555), 0);
  lv_obj_set_style_pad_all(errlog_scroll_container, 8, 0);
  lv_obj_set_flex_flow(errlog_scroll_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(errlog_scroll_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(errlog_scroll_container, 6, 0);
  lv_obj_add_flag(errlog_scroll_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(errlog_scroll_container, LV_SCROLLBAR_MODE_AUTO);
  errlog_btn_clear =
      halo_ui_button(errlog_screen, 120, 248, 120, 36, "CLEAR LOGS", COL_DARK, COL_GOLD, true);
  lv_obj_add_event_cb(errlog_btn_clear, errlog_btn_clear_event, LV_EVENT_CLICKED, NULL);
  errlog_btn_back = halo_ui_back(errlog_screen);
  lv_obj_add_event_cb(errlog_btn_back, errlog_btn_back_event, LV_EVENT_CLICKED, NULL);
}

static void show_errlog_screen() {
  errlog_screen_init();
  errlog_screen_populate();
  ui_screen_state = SCREEN_ERRLOG;
  ui_busy = false;
  lv_scr_load(errlog_screen);
  Serial.println("[MENU] screen=ERRLOG");
  lv_timer_handler();
}

// Style a settings list button with the shared design language
// (white card, 2px dark border, hard drop shadow, radius 18).
// Design-system card. Was hand-rolled with a soft 8px/40% shadow offset 4/6;
// trepo_card() applies the system's hard 1px shadow at 4/4, ink at 85%.
// Label is Nunito with the system's tracking. Callers pass text ALREADY
// uppercased — LVGL has no text-transform.
static void ship_settings_style_button(lv_obj_t* btn, lv_obj_t* label, const char* text) {
  lv_obj_set_size(btn, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
  trepo_card(btn, COL_WHITE, 18);
  quiet_clickable(btn);
  lv_label_set_text(label, text);
  // The Nunito cuts are ASCII-only. LV_SYMBOL_* are FontAwesome codepoints in
  // the UTF-8 private use area and render as TOFU in them, so any label that is
  // an icon must stay on Montserrat. Detect by high-bit bytes.
  bool is_symbol = false;
  for (const char* p = text; p && *p; ++p) {
    if ((unsigned char)*p >= 0x80) { is_symbol = true; break; }
  }
  lv_obj_set_style_text_font(label,
                             is_symbol ? &lv_font_montserrat_20 : &nunito_16,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(COL_DARK), LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(label, is_symbol ? 0 : 1, LV_PART_MAIN);
  lv_obj_center(label);
}

static void show_ship_settings_screen_impl() {
  if (!ship_menu_settings_screen) {
    ship_menu_settings_screen = halo_ui_page();
    halo_ui_label(ship_menu_settings_screen, "Settings", &nunito_24, COL_DARK, 60, 30, 240);
    auto row = [&](int y, const char* text, HaloUiIcon icon, lv_obj_t** label) {
      lv_obj_t* b = halo_ui_card(ship_menu_settings_screen, 58, y, 244, 52);
      halo_ui_icon(b, icon, 14, 13, 21, COL_TEAL);
      *label = halo_ui_label(b, text, &nunito_16, COL_DARK, 42, 15, 171);
      lv_obj_set_style_text_align(*label, LV_TEXT_ALIGN_LEFT, 0);
      halo_ui_icon(b, HALO_ICON_CHEVRON, 215, 16, 16, COL_MUTED);
      return b;
    };
    ship_menu_settings_btn_backlight =
        row(70, "Brightness", HALO_ICON_SUN, &ship_menu_settings_label_backlight);
    ship_menu_settings_btn_ota =
        row(132, "Software update", HALO_ICON_DOWNLOAD, &ship_menu_settings_label_ota);
    ship_menu_settings_btn_reset =
        row(194, "Change Wi-Fi", HALO_ICON_WIFI, &ship_menu_settings_label_reset);
    ship_menu_settings_btn_back = halo_ui_back(ship_menu_settings_screen);
    ship_menu_settings_label_back = NULL;
    ship_menu_settings_versions =
        halo_ui_label(ship_menu_settings_screen, "", &nunito_12, COL_MUTED, 62, 262, 236);
    ship_menu_settings_status =
        halo_ui_label(ship_menu_settings_screen, "", &nunito_12, COL_TEAL, 75, 258, 210);
    lv_obj_add_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN);
  }
  ship_menu_screen_state = SHIP_MENU_SCREEN_SETTINGS;
  ui_screen_state = SCREEN_SETTINGS;
  ui_busy = false;
  home_shown_ms = millis();
  touch_ignore_until = millis() + 280;  // guard: ignore bleed-through touch from the navigation tap so it can't trigger a button on the just-loaded screen (e.g. tapping Settings auto-firing Run OTA Update)
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

// ── Backlight brightness screen ──────────────────────────────────────────
static void show_ship_backlight_screen_impl() {
  if (!ship_backlight_screen) {
    ship_backlight_screen = halo_ui_page();
    halo_ui_label(ship_backlight_screen, "Brightness", &nunito_24, COL_DARK, 55, 32, 250);
    halo_ui_label(ship_backlight_screen, "Turn the dial to adjust", &lv_font_montserrat_14,
                  COL_TEXT2, 45, 66, 270);
    ship_backlight_ring = halo_ui_ring(ship_backlight_screen, 102, 94, 156, 10, COL_GOLD);
    ship_backlight_pct_label =
        halo_ui_label(ship_backlight_screen, "100%", &nunito_44, COL_DARK, 105, 148, 150);
    halo_ui_button(ship_backlight_screen, 110, 264, 140, 48, "SAVE");
  }
  // Seed arc + label from current brightness
  int pct = backlight_get_pct();
  if (pct < 5) pct = 5;
  if (pct > 100) pct = 100;
  if (ship_backlight_ring != NULL) {
    lv_arc_set_value(ship_backlight_ring, pct * 10);
  }
  if (ship_backlight_pct_label != NULL) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(ship_backlight_pct_label, buf);
  }

  ui_screen_state = SCREEN_BACKLIGHT;
  ui_busy = false;
  home_shown_ms = millis();
  touch_ignore_until = millis() + 280;  // guard: ignore bleed-through tap from the navigation tap
  lv_scr_load(ship_backlight_screen);
  Serial.println("[MENU] screen=BACKLIGHT");
  lv_timer_handler();
}

static void show_ship_backlight_screen() {
  UI_SHOW(SCREEN_SHIP_BACKLIGHT, "show_backlight");
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

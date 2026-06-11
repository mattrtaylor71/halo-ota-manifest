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
                            primary ? lv_color_hex(0x296065) : lv_color_hex(0xFFFFFF),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
  // hard drop shadow, offset bottom-right (design look)
  lv_obj_set_style_shadow_width(btn, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_spread(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_ofs_x(btn, 5, LV_PART_MAIN);
  lv_obj_set_style_shadow_ofs_y(btn, 7, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(btn, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(btn, LV_OPA_50, LV_PART_MAIN);
}

static void ship_main_menu_set_ai_hold_active(bool active) {
  if (!ship_main_menu_ai_button || !ship_main_menu_ai_label) {
    return;
  }
  // Center mic button stays a teal box; press feedback lightens it + gold border (drop shadow untouched)
  lv_obj_set_style_bg_color(ship_main_menu_ai_button,
                            active ? lv_color_hex(0x347C82) : lv_color_hex(0x296065),
                            LV_PART_MAIN);
  lv_obj_set_style_border_color(ship_main_menu_ai_button,
                            active ? lv_color_hex(0xF6BF41) : lv_color_hex(0x1A1A1A),
                            LV_PART_MAIN);
}

// Expanding "sound pulse" ripple: v = 0..1000 -> grow outward + fade, looped.
static void ship_listen_pulse_cb(void* obj, int32_t v) {
  lv_obj_t* o = (lv_obj_t*)obj;
  int sz = 120 + (int)((v * 104L) / 1000);   // emerge at the disc edge, expand to ~224 px
  lv_obj_set_size(o, sz, sz);
  lv_obj_set_style_radius(o, sz / 2, LV_PART_MAIN);
  lv_obj_align(o, LV_ALIGN_CENTER, 0, -8);
  lv_obj_set_style_border_opa(o, (lv_opa_t)((LV_OPA_50 * (1000 - v)) / 1000), LV_PART_MAIN);
}

// Gentle "breathing" zoom for the teal mic disc (256 = 100%).
static void ship_listen_zoom_cb(void* obj, int32_t v) {
  lv_obj_set_style_transform_zoom((lv_obj_t*)obj, (uint16_t)v, LV_PART_MAIN);
}

static void ship_init_ai_listening_screen() {
  if (ship_ai_listening_screen) {
    return;
  }

  ship_ai_listening_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_ai_listening_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_ai_listening_screen, LV_OBJ_FLAG_SCROLLABLE);
  ship_style_plain_screen(ship_ai_listening_screen, lv_color_hex(0xFDF2DE));
  lv_obj_set_style_border_width(ship_ai_listening_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_ai_listening_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_screen, 0, LV_PART_MAIN);

  // looping "sound pulse" ripples (created first = drawn behind the disc/ring)
  for (int i = 0; i < 3; ++i) {
    lv_obj_t* p = lv_obj_create(ship_ai_listening_screen);
    ship_ai_listening_pulse[i] = p;
    lv_obj_set_size(p, 120, 120);
    lv_obj_align(p, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 4, LV_PART_MAIN);
    lv_obj_set_style_border_color(p, lv_color_hex(0x296065), LV_PART_MAIN);
    lv_obj_set_style_border_opa(p, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(p, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(p, 0, LV_PART_MAIN);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }

  // gold countdown ring with rounded caps over a faint teal track
  ship_ai_listening_ring = lv_arc_create(ship_ai_listening_screen);
  lv_obj_remove_style(ship_ai_listening_ring, NULL, LV_PART_KNOB);
  lv_obj_set_size(ship_ai_listening_ring, 176, 176);
  lv_obj_align(ship_ai_listening_ring, LV_ALIGN_CENTER, 0, -8);
  lv_arc_set_range(ship_ai_listening_ring, 0, 1000);
  lv_arc_set_value(ship_ai_listening_ring, 1000);
  lv_arc_set_bg_angles(ship_ai_listening_ring, 0, 360);
  lv_arc_set_rotation(ship_ai_listening_ring, 270);
  lv_obj_set_style_arc_width(ship_ai_listening_ring, 11, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_ai_listening_ring, 11, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ship_ai_listening_ring, lv_color_hex(0x296065), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ship_ai_listening_ring, LV_OPA_20, LV_PART_MAIN);   // faint teal track
  lv_obj_set_style_arc_color(ship_ai_listening_ring, lv_color_hex(0xF6BF41), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_ai_listening_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ship_ai_listening_ring, true, LV_PART_INDICATOR);
  lv_obj_set_style_outline_width(ship_ai_listening_ring, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_ai_listening_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  // teal disc that holds the mic (mirrors the menu's centre button), breathes gently
  ship_ai_listening_mic_disc = lv_obj_create(ship_ai_listening_screen);
  lv_obj_set_size(ship_ai_listening_mic_disc, 128, 128);
  lv_obj_align(ship_ai_listening_mic_disc, LV_ALIGN_CENTER, 0, -8);
  lv_obj_set_style_radius(ship_ai_listening_mic_disc, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_disc, lv_color_hex(0x296065), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_disc, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_disc, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_ai_listening_mic_disc, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_disc, 0, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_x(ship_ai_listening_mic_disc, 64, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(ship_ai_listening_mic_disc, 64, LV_PART_MAIN);
  lv_obj_clear_flag(ship_ai_listening_mic_disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  // gold mic, parented to the disc so it scales with the breathing animation
  ship_ai_listening_mic_head = lv_obj_create(ship_ai_listening_mic_disc);
  lv_obj_set_size(ship_ai_listening_mic_head, 48, 70);
  lv_obj_align(ship_ai_listening_mic_head, LV_ALIGN_CENTER, 0, -16);
  lv_obj_set_style_radius(ship_ai_listening_mic_head, 24, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_head, lv_color_hex(0xF6BF41), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_head, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_head, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_head, 0, LV_PART_MAIN);

  ship_ai_listening_mic_stem = lv_obj_create(ship_ai_listening_mic_disc);
  lv_obj_set_size(ship_ai_listening_mic_stem, 8, 28);
  lv_obj_align(ship_ai_listening_mic_stem, LV_ALIGN_CENTER, 0, 30);
  lv_obj_set_style_radius(ship_ai_listening_mic_stem, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_stem, lv_color_hex(0xF6BF41), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_stem, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_stem, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_stem, 0, LV_PART_MAIN);

  ship_ai_listening_mic_base = lv_obj_create(ship_ai_listening_mic_disc);
  lv_obj_set_size(ship_ai_listening_mic_base, 44, 6);
  lv_obj_align(ship_ai_listening_mic_base, LV_ALIGN_CENTER, 0, 48);
  lv_obj_set_style_radius(ship_ai_listening_mic_base, 3, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_ai_listening_mic_base, lv_color_hex(0xF6BF41), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_ai_listening_mic_base, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_ai_listening_mic_base, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_ai_listening_mic_base, 0, LV_PART_MAIN);

  ship_ai_listening_title = lv_label_create(ship_ai_listening_screen);
  lv_label_set_text(ship_ai_listening_title, "Listening");
  lv_obj_set_style_text_font(ship_ai_listening_title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_ai_listening_title, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_ai_listening_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_ai_listening_title, LV_ALIGN_CENTER, 0, -136);

  ship_ai_listening_hint = lv_label_create(ship_ai_listening_screen);
  lv_label_set_text(ship_ai_listening_hint, "Release to return");
  lv_obj_set_style_text_font(ship_ai_listening_hint, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_ai_listening_hint, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
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
  lv_anim_del(ship_ai_listening_mic_disc, NULL);
  for (int i = 0; i < 3; ++i) {
    if (ship_ai_listening_pulse[i]) lv_anim_del(ship_ai_listening_pulse[i], NULL);
  }

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

  // looping "sound pulse" ripples — staggered so a wave is always travelling outward
  for (int i = 0; i < 3; ++i) {
    if (!ship_ai_listening_pulse[i]) continue;
    lv_obj_set_style_border_opa(ship_ai_listening_pulse[i], LV_OPA_0, LV_PART_MAIN);
    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, ship_ai_listening_pulse[i]);
    lv_anim_set_values(&pulse, 0, 1000);
    lv_anim_set_time(&pulse, 1700);
    lv_anim_set_delay(&pulse, i * 560);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&pulse, ship_listen_pulse_cb);
    lv_anim_start(&pulse);
  }

  // gentle breathing of the teal disc (and the gold mic riding on it)
  if (ship_ai_listening_mic_disc) {
    lv_obj_set_style_transform_zoom(ship_ai_listening_mic_disc, 256, LV_PART_MAIN);
    lv_anim_t breathe;
    lv_anim_init(&breathe);
    lv_anim_set_var(&breathe, ship_ai_listening_mic_disc);
    lv_anim_set_values(&breathe, 256, 273);
    lv_anim_set_time(&breathe, 850);
    lv_anim_set_playback_time(&breathe, 850);
    lv_anim_set_repeat_count(&breathe, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&breathe, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&breathe, ship_listen_zoom_cb);
    lv_anim_start(&breathe);
  }
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
  lv_color_t screen_bg = lv_color_hex(0xF5E9D8);
  lv_color_t card_bg = lv_color_hex(0xFFFFFF);
  lv_color_t accent = lv_color_hex(0x1F4D2B);

  if (page) {
    if (strcmp(page->style, "warning") == 0) {
      accent = lv_color_hex(0xFBBF24);
      card_bg = lv_color_hex(0xFFFFFF);
    } else if (strcmp(page->style, "success") == 0) {
      accent = lv_color_hex(0x1F4D2B);
      card_bg = lv_color_hex(0xFFFFFF);
    } else if (strcmp(page->style, "error") == 0) {
      accent = lv_color_hex(0xFCA5A5);
      card_bg = lv_color_hex(0xFFFFFF);
      screen_bg = lv_color_hex(0xF5E9D8);
    } else if (strcmp(page->style, "info") == 0) {
      accent = lv_color_hex(0x93C5FD);
      card_bg = lv_color_hex(0xFFFFFF);
    }
  }

  ship_style_plain_screen(ship_voice_json_screen, screen_bg);
  lv_obj_set_style_bg_opa(ship_voice_json_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_voice_json_card, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_voice_json_card, 0, LV_PART_MAIN);

  lv_obj_set_style_text_color(ship_voice_json_title, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_subtitle, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_label, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_json_footer, accent, LV_PART_MAIN);

  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_ITEMS; ++i) {
    if (ship_voice_json_item_labels[i]) {
      lv_obj_set_style_text_color(ship_voice_json_item_labels[i], lv_color_hex(0x4A4A4A), LV_PART_MAIN);
    }
  }
  for (uint8_t i = 0; i < SHIP_VOICE_UI_MAX_PAGES; ++i) {
    if (!ship_voice_json_page_dots[i]) {
      continue;
    }
    bool active = (i == g_ship_voice_ui_page_index);
    lv_obj_set_style_bg_color(ship_voice_json_page_dots[i], active ? accent : lv_color_hex(0x8A7E72), LV_PART_MAIN);
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
  ship_style_plain_screen(ship_voice_json_screen, lv_color_hex(0xF5E9D8));
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
  (void)fg_color;   // icon color is baked into the dish_icon_img asset
  lv_obj_t* icon = lv_img_create(btn);
  lv_img_set_src(icon, &dish_icon_img);
  lv_obj_center(icon);
}

static void ship_main_menu_add_mic_icon(lv_obj_t* btn) {
  if (!btn) {
    return;
  }
  lv_obj_t* icon = lv_img_create(btn);
  lv_img_set_src(icon, &mic_icon_img);
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
    ship_style_plain_screen(ship_menu_screen, lv_color_hex(0xFDF2DE));
    lv_obj_set_style_border_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ship_menu_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ship_menu_screen, LV_OBJ_FLAG_SCROLLABLE);

    ship_main_menu_buttons[0] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[0], SHIP_MAIN_MENU_TOP_BTN_X, SHIP_MAIN_MENU_TOP_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[0], false);
    ship_main_menu_add_dish_icon(ship_main_menu_buttons[0], lv_color_hex(0x1A1A1A));

    ship_main_menu_buttons[1] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[1], SHIP_MAIN_MENU_LEFT_BTN_X, SHIP_MAIN_MENU_LEFT_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[1], false);
    ship_main_menu_add_plus(ship_main_menu_buttons[1], lv_color_hex(0x1A1A1A));

    ship_main_menu_buttons[2] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[2], SHIP_MAIN_MENU_RIGHT_BTN_X, SHIP_MAIN_MENU_RIGHT_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[2], false);
    ship_main_menu_add_minus(ship_main_menu_buttons[2], lv_color_hex(0x1A1A1A));

    ship_main_menu_buttons[3] = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_buttons[3], SHIP_MAIN_MENU_BOTTOM_BTN_X, SHIP_MAIN_MENU_BOTTOM_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_buttons[3], false);
    ship_main_menu_add_dots(ship_main_menu_buttons[3], lv_color_hex(0x1A1A1A));

    ship_main_menu_ai_button = lv_obj_create(ship_menu_screen);
    lv_obj_set_pos(ship_main_menu_ai_button, SHIP_MAIN_MENU_CENTER_BTN_X, SHIP_MAIN_MENU_CENTER_BTN_Y);
    ship_main_menu_style_button(ship_main_menu_ai_button, true);   // teal box

    // keep a hidden label so ship_main_menu_set_ai_hold_active()'s guard passes
    ship_main_menu_ai_label = lv_label_create(ship_main_menu_ai_button);
    lv_label_set_text(ship_main_menu_ai_label, "AI");
    lv_obj_center(ship_main_menu_ai_label);
    lv_obj_add_flag(ship_main_menu_ai_label, LV_OBJ_FLAG_HIDDEN);
    ship_main_menu_add_mic_icon(ship_main_menu_ai_button);         // gold mic on teal
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
    ship_menu_second_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_second_screen, LV_PCT(100), LV_PCT(100));
    ship_style_plain_screen(ship_menu_second_screen, lv_color_hex(0xFDF2DE));
    lv_obj_set_style_border_width(ship_menu_second_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ship_menu_second_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_menu_second_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ship_menu_second_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ship_menu_second_screen, LV_OBJ_FLAG_SCROLLABLE);

    // SHOPPING LIST button — top of screen
    lv_obj_t* list_btn = lv_obj_create(ship_menu_second_screen);
    lv_obj_set_pos(list_btn, SHIP_MAIN_MENU_TOP_BTN_X, SHIP_MAIN_MENU_TOP_BTN_Y);
    ship_main_menu_style_button(list_btn, false);
    ship_menu_add_symbol_centered(list_btn, LV_SYMBOL_LIST, lv_color_hex(0x1A1A1A));

    // HOME button — center of screen (teal, mirrors the main-menu center accent)
    lv_obj_t* home_btn = lv_obj_create(ship_menu_second_screen);
    lv_obj_set_pos(home_btn, SHIP_MAIN_MENU_CENTER_BTN_X, SHIP_MAIN_MENU_CENTER_BTN_Y);
    ship_main_menu_style_button(home_btn, true);
    ship_menu_add_symbol_centered(home_btn, LV_SYMBOL_LEFT, lv_color_hex(0xF6BF41));

    // SETTINGS button — bottom of screen (same position as "More" on main menu)
    lv_obj_t* settings_btn = lv_obj_create(ship_menu_second_screen);
    lv_obj_set_pos(settings_btn, SHIP_MAIN_MENU_BOTTOM_BTN_X, SHIP_MAIN_MENU_BOTTOM_BTN_Y);
    ship_main_menu_style_button(settings_btn, false);
    ship_menu_add_symbol_centered(settings_btn, LV_SYMBOL_SETTINGS, lv_color_hex(0x1A1A1A));
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
static int shopping_list_scroll_idx = 0;
static int shopping_list_overscroll_ticks = 0;  // counts CCW ticks at top for pull-to-refresh
static const int SHOPPING_LIST_REFRESH_TICKS = 3;
static unsigned long shopping_list_last_ccw_tick_ms = 0;  // last CCW overscroll tick (for stale-tick expiry)
static const unsigned long SHOPPING_LIST_OVERSCROLL_WINDOW_MS = 1500;  // CCW ticks older than this don't count toward refresh
static lv_obj_t* shopping_list_items[50] = {NULL};
static int shopping_list_rendered_count = 0;
static lv_obj_t* shopping_list_overlay = NULL;  // Delete/Back overlay
static bool shopping_list_overlay_visible = false;
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
static lv_obj_t* shopping_list_refresh_ring = NULL;
static lv_obj_t* shopping_list_error_toast = NULL;
static int shopping_list_refresh_ui_state = -1;   // last refresh SM state synced to the ring (-1 = force re-sync)
static bool shopping_list_ring_hiding = false;    // ring fade-out in progress (liststate "pill_hiding")
static bool shopping_list_ring_sweeping = false;  // sweep anim currently running
static bool shopping_list_ring_autohide = false;  // finish/error sequence owns the hide — plain hide() must not interrupt
static int32_t shopping_list_ring_sweep_base = 0; // current sweep start angle (deg from 12 o'clock)
static const int SHOPPING_LIST_RING_SIZE = 352;          // hugs the 360px round edge (4px margin)
static const int SHOPPING_LIST_RING_WIDTH = 5;           // indicator arc width
static const int SHOPPING_LIST_RING_WIDTH_ARMED = 7;     // emphasis when the pull passes the threshold
static const int SHOPPING_LIST_RING_SWEEP_DEG = 70;      // sweeping segment length
static const uint32_t SHOPPING_LIST_RING_SWEEP_MS = 1200;  // one full revolution

// Round-screen card layout
static const int SHOPPING_LIST_CARD_W = 272;   // clears the circle at all visible heights
static const int SHOPPING_LIST_CARD_H = 60;
static const int SHOPPING_LIST_CARD_RADIUS = 16;
static const int SHOPPING_LIST_CARD_GAP = 10;

// Signature of the list content currently rendered in shopping_list_scroll.
// 0xFFFFFFFF = placeholder content (skeleton / hint label) that never matches
// real content, so the next real list always triggers a render.
static uint32_t shopping_list_rendered_sig = 0xFFFFFFFFu;

// Touch pull-to-refresh state (LVGL scroll events on shopping_list_scroll)
static bool shopping_list_touch_pull_armed = false;     // pull passed threshold while pressed
static bool shopping_list_touch_pull_consumed = false;  // refresh already fired for this gesture
static bool shopping_list_touch_in_press = false;       // finger currently down on the scroll container
static bool shopping_list_touch_was_scrolled = false;   // gesture scrolled — suppress the tap-overlay on release
static const int SHOPPING_LIST_PULL_THRESHOLD_PX = 45;  // pull-down distance past top to arm refresh
static const int SHOPPING_LIST_PULL_HINT_PX = 12;       // pull distance at which the hint cue appears

static bool shopping_list_reveal_pending = false;  // staggered fade-in on next populate (fresh UI_LIST)

// Forward declarations
static void show_shopping_list_screen();
static void shopping_list_screen_populate();
static void shopping_list_dismiss_overlay();
static void shopping_list_trigger_refresh(const char* reason);
static void shopping_list_animate_card_removal(int idx);

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
  if (!shopping_list_refresh_ring) return;
  int st = (int)refresh_state;
  if (!force && st == shopping_list_refresh_ui_state) return;
  shopping_list_refresh_ui_state = st;
  if (st == REFRESH_WAKE_PENDING || st == REFRESH_INFLIGHT) {
    shopping_list_ring_show_sweep();
  } else if (st == REFRESH_COMPLETE) {
    shopping_list_ring_finish();
  } else if (st == REFRESH_FAILED) {
    shopping_list_ring_show_error();
  } else if (shopping_list_touch_in_press) {
    // IDLE mid-press: the gesture events own the ring during a pull drag
    // (the UI-task poll would otherwise fight the pull-progress fill with
    // fade-outs). Leave the cache unconsumed so the post-release sync still
    // sees the IDLE transition and hides a stale cue.
    shopping_list_refresh_ui_state = -1;
  } else {
    shopping_list_ring_hide();
  }
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
  lv_obj_set_style_arc_color(shopping_list_refresh_ring, lv_color_hex(0x1F4D2B), LV_PART_INDICATOR);
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
  lv_obj_set_style_text_color(shopping_list_error_toast, lv_color_hex(0x8B1E2D), LV_PART_MAIN);
  lv_obj_set_style_bg_color(shopping_list_error_toast, lv_color_hex(0xFAF6F0), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(shopping_list_error_toast, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(shopping_list_error_toast, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_left(shopping_list_error_toast, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_right(shopping_list_error_toast, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_top(shopping_list_error_toast, 5, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(shopping_list_error_toast, 5, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(shopping_list_error_toast, 6, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(shopping_list_error_toast, lv_color_hex(0xD4C4AE), LV_PART_MAIN);
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
  if (!shopping_list_screen || shopping_list_overlay_visible) return;
  if (g_active.count == 0 || shopping_list_scroll_idx < 0 || shopping_list_scroll_idx >= g_active.count) return;

  shopping_list_overlay_visible = true;

  // Semi-transparent dark backdrop covering the whole screen
  shopping_list_overlay = lv_obj_create(shopping_list_screen);
  lv_obj_set_size(shopping_list_overlay, 360, 360);
  lv_obj_set_pos(shopping_list_overlay, 0, 0);
  lv_obj_set_style_bg_color(shopping_list_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(shopping_list_overlay, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_border_width(shopping_list_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(shopping_list_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(shopping_list_overlay, 0, LV_PART_MAIN);
  lv_obj_clear_flag(shopping_list_overlay, LV_OBJ_FLAG_SCROLLABLE);

  // Center card showing item name + buttons
  lv_obj_t* card = lv_obj_create(shopping_list_overlay);
  lv_obj_set_size(card, 280, LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(card, 140, LV_PART_MAIN);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, -10);
  lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(card, 20, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(card, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
  lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  // Item name
  lv_obj_t* name_label = lv_label_create(card);
  lv_label_set_text(name_label, g_active.items[shopping_list_scroll_idx]);
  lv_obj_set_style_text_font(name_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(name_label, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_width(name_label, 240);
  lv_obj_set_style_text_align(name_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);

  // Button row container
  lv_obj_t* btn_row = lv_obj_create(card);
  lv_obj_set_size(btn_row, 240, 44);
  lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

  // Delete button — red
  lv_obj_t* del_btn = lv_btn_create(btn_row);
  lv_obj_set_size(del_btn, 110, 40);
  lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xE53935), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(del_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(del_btn, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(del_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(del_btn, 0, LV_PART_MAIN);
  lv_obj_t* del_label = lv_label_create(del_btn);
  lv_label_set_text(del_label, "Delete");
  lv_obj_set_style_text_font(del_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(del_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(del_label);

  // Back button — green
  lv_obj_t* back_btn = lv_btn_create(btn_row);
  lv_obj_set_size(back_btn, 110, 40);
  lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(back_btn, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
  lv_obj_t* back_label = lv_label_create(back_btn);
  lv_label_set_text(back_label, "Back");
  lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(back_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(back_label);

  Serial.printf("[SHOP_LIST] overlay shown for item %d: %s\n",
                shopping_list_scroll_idx, g_active.items[shopping_list_scroll_idx]);
}

static void shopping_list_dismiss_overlay() {
  if (!shopping_list_overlay_visible || !shopping_list_overlay) return;
  lv_obj_del(shopping_list_overlay);
  shopping_list_overlay = NULL;
  shopping_list_overlay_visible = false;
  Serial.println("[SHOP_LIST] overlay dismissed");
}

// Delete the item at the given index from the active shopping list.
// Mirrors the DELETE-touch path: track the deleted id, send INPUT_DELETE to
// Sense, remove locally under the state mutex, and fix up the scroll index.
// Does NOT dismiss any overlay or re-render — callers handle that. Returns the
// deleted item id (empty string on failure / out-of-range).
static const char* shopping_list_delete_index(int idx) {
  static char deleted_id[64];
  deleted_id[0] = '\0';
  if (idx < 0 || idx >= g_active.count) return deleted_id;

  Serial.printf("[SHOP_LIST] DELETE index %d: %s (id=%s)\n",
                idx, g_active.items[idx], g_active.item_ids[idx]);

  // Track deleted item ID for filtering
  const char* del_id = g_active.item_ids[idx];
  strncpy(deleted_id, del_id, sizeof(deleted_id) - 1);
  deleted_id[sizeof(deleted_id) - 1] = '\0';
  if (del_id[0] != '\0' && deleted_item_count < MAX_DELETED_ITEMS) {
    strncpy(deleted_item_ids[deleted_item_count], del_id, 63);
    deleted_item_ids[deleted_item_count][63] = '\0';
    deleted_item_count++;
  }

  // Send INPUT_DELETE to Sense
  tx_msg_t tx_msg = {};
  strncpy(tx_msg.type, "INPUT_DELETE", sizeof(tx_msg.type) - 1);
  strncpy(tx_msg.id, del_id, sizeof(tx_msg.id) - 1);
  tx_msg.has_id = true;
  if (uart_tx_queue != NULL) {
    xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
    Serial.printf("[SHOP_LIST] Sent INPUT_DELETE for ID: %s\n", del_id);
  }

  // Remove from g_active locally
  if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int j = idx; j < g_active.count - 1; j++) {
      strncpy(g_active.items[j], g_active.items[j+1], 63);
      g_active.items[j][63] = '\0';
      strncpy(g_active.item_ids[j], g_active.item_ids[j+1], 63);
      g_active.item_ids[j][63] = '\0';
    }
    if (g_active.count > 0) {
      g_active.items[g_active.count - 1][0] = '\0';
      g_active.item_ids[g_active.count - 1][0] = '\0';
      g_active.count--;
    }
    // Persist the deletion immediately — LCD deep sleep is a full reset, so an
    // unsaved delete would resurrect the item from the NVS cache on next boot.
    save_list_to_storage(&g_active);
    xSemaphoreGive(app_state_mutex);
  }

  // Adjust scroll index
  if (shopping_list_scroll_idx >= g_active.count && g_active.count > 0) {
    shopping_list_scroll_idx = g_active.count - 1;
  } else if (g_active.count == 0) {
    shopping_list_scroll_idx = 0;
  }

  return deleted_id;
}

// Kick the list-refresh state machine exactly like the pull-to-refresh gesture
// (touch pull-down past the top, or 3 CCW knob ticks at top): wake Sense, arm
// the refresh, and start the border-ring sweep.
// Touches LVGL — UI-task only. `reason` is for logging/wake tagging.
static void shopping_list_trigger_refresh(const char* reason) {
  const char* r = reason ? reason : "list_refresh";
  // Guard: never stack a second refresh while one is pending/inflight
  if (refresh_state == REFRESH_WAKE_PENDING || refresh_state == REFRESH_INFLIGHT) {
    Serial.printf("[SHOPPING_LIST] refresh ignored (%s) — already %s\n",
                  r, refresh_state == REFRESH_INFLIGHT ? "inflight" : "wake_pending");
    return;
  }
  Serial.printf("[SHOPPING_LIST] refresh triggered (%s)\n", r);
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
static bool shopping_list_handle_touch(int x, int y) {
  if (ui_screen_state != SCREEN_SHOPPING_LIST) return false;

  // The touch path has no drag filter — a quick scroll/pull would otherwise be
  // treated as a tap at the press point and pop the item overlay (or hit the
  // back button). If LVGL scrolled the list during this press, consume the
  // "tap" without acting on it.
  if (!shopping_list_overlay_visible && shopping_list_touch_was_scrolled) {
    shopping_list_touch_was_scrolled = false;
    Serial.println("[SHOP_LIST] touch consumed (scroll gesture, not a tap)");
    return true;
  }

  // Check circular back button at bottom-center (visual: 56px circle at
  // y≈294-350; hitbox kept generous around it)
  if (!shopping_list_overlay_visible && y >= 288 && x >= 130 && x <= 230) {
    Serial.println("[SHOP_LIST] back button tapped");
    show_ship_second_menu();
    return true;
  }

  if (shopping_list_overlay_visible) {
    // Check if tap is on the center card area (roughly 40..320 x, 90..270 y)
    // The card is 280px wide centered at 180, so x=[40..320], and vertically centered around 170
    int card_x1 = 40, card_x2 = 320;
    int card_y1 = 80, card_y2 = 270;

    if (x >= card_x1 && x <= card_x2 && y >= card_y1 && y <= card_y2) {
      // Check which button was tapped
      // Button row is at the bottom of the card, roughly y=210..250
      // Delete button: left half (~55..165), Back button: right half (~175..285)
      if (y >= 195) {
        int btn_mid = 180;
        if (x < btn_mid) {
          // DELETE tapped
          Serial.printf("[SHOP_LIST] DELETE tapped for item %d\n", shopping_list_scroll_idx);
          int del_idx = shopping_list_scroll_idx;
          shopping_list_delete_index(del_idx);

          // Dismiss overlay, animate the card out, rebuild when it lands
          shopping_list_dismiss_overlay();
          shopping_list_animate_card_removal(del_idx);
          return true;

        } else {
          // BACK tapped
          Serial.println("[SHOP_LIST] BACK tapped from overlay");
          shopping_list_dismiss_overlay();
          show_ship_second_menu();
          return true;
        }
      }
    }

    // Tap outside card — dismiss overlay
    shopping_list_dismiss_overlay();
    return true;
  }

  // No overlay visible — show the overlay (item action popup)
  if (g_active.count > 0) {
    shopping_list_show_overlay();
  } else {
    // No items, tap goes back
    show_ship_second_menu();
  }
  return true;
}

// Restyle a single list card as selected/unselected — O(1), no rebuild.
// Selected = white bg + hero-green left accent bar + arrow; unselected = tan.
static void shopping_list_style_card(lv_obj_t* card, int idx, bool selected) {
  if (!card) return;

  if (selected) {
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 4, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_color(card, lv_color_hex(0xD4C4AE), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
  }

  // Arrow + text (label is the card's only child)
  lv_obj_t* label = lv_obj_get_child(card, 0);
  if (label && idx >= 0 && idx < g_active.count) {
    char item_text[80];
    if (selected) {
      snprintf(item_text, sizeof(item_text), LV_SYMBOL_RIGHT "  %s", g_active.items[idx]);
    } else {
      snprintf(item_text, sizeof(item_text), "%s", g_active.items[idx]);
    }
    lv_label_set_text(label, item_text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(selected ? 0x1A1A1A : 0x444444), LV_PART_MAIN);
  }
}

// Lightweight scroll path: restyle just the two affected cards and keep the
// selection visible. Used by the encoder EVT_SCROLL_DELTA handler instead of
// a full repopulate (the rebuild-everything path made scrolling laggy).
static void shopping_list_update_selection(int old_idx, int new_idx) {
  if (!shopping_list_scroll) return;
  if (old_idx >= 0 && old_idx < shopping_list_rendered_count && shopping_list_items[old_idx]) {
    shopping_list_style_card(shopping_list_items[old_idx], old_idx, false);
  }
  if (new_idx >= 0 && new_idx < shopping_list_rendered_count && shopping_list_items[new_idx]) {
    shopping_list_style_card(shopping_list_items[new_idx], new_idx, true);
    lv_obj_scroll_to_view(shopping_list_items[new_idx], LV_ANIM_ON);
  }
}

// lv_anim exec callback for the staggered row fade-in reveal
static void shopping_list_card_opa_anim_cb(void* var, int32_t value) {
  lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)value, 0);
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
    lv_obj_set_style_bg_color(ph, lv_color_hex(0xE7D9C3), LV_PART_MAIN);  // light tan placeholder between the bg and the 0xD4C4AE cards
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

// ── Delete animation ─────────────────────────────────────────────────
// Slide the removed card left + fade (~150ms), then collapse its height
// (~120ms), then rebuild the list — deletion feels physical instead of an
// instant rebuild. The data (g_active) is already updated by the caller; this
// is purely the visual exit. If a fresh UI_LIST rebuilds the list mid-
// animation, lv_obj_clean() deletes the card and its anims (the ready
// callback simply never fires — the rebuild already happened).
static void shopping_list_card_x_anim_cb(void* var, int32_t value) {
  lv_obj_set_style_translate_x((lv_obj_t*)var, (lv_coord_t)value, 0);
}

static void shopping_list_card_h_anim_cb(void* var, int32_t value) {
  lv_obj_set_height((lv_obj_t*)var, (lv_coord_t)value);
}

static void shopping_list_delete_anim_done(lv_anim_t* a) {
  (void)a;
  shopping_list_screen_populate();
}

static void shopping_list_animate_card_removal(int idx) {
  lv_obj_t* card = (idx >= 0 && idx < shopping_list_rendered_count)
                       ? shopping_list_items[idx]
                       : NULL;
  if (!card) {
    shopping_list_screen_populate();  // nothing to animate — rebuild now
    return;
  }
  shopping_list_items[idx] = NULL;  // selection restyle must not touch it anymore
  lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_coord_t h = lv_obj_get_height(card);
  lv_obj_set_style_min_height(card, 0, LV_PART_MAIN);  // allow the collapse below 38px

  lv_anim_t a;
  // Phase 1: slide left + fade (simultaneous, ~150ms)
  lv_anim_init(&a);
  lv_anim_set_var(&a, card);
  lv_anim_set_exec_cb(&a, shopping_list_card_x_anim_cb);
  lv_anim_set_values(&a, 0, -320);
  lv_anim_set_time(&a, 150);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_start(&a);
  lv_anim_init(&a);
  lv_anim_set_var(&a, card);
  lv_anim_set_exec_cb(&a, shopping_list_card_opa_anim_cb);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_time(&a, 150);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_start(&a);
  // Phase 2: collapse the row height, then rebuild from g_active
  lv_anim_init(&a);
  lv_anim_set_var(&a, card);
  lv_anim_set_exec_cb(&a, shopping_list_card_h_anim_cb);
  lv_anim_set_values(&a, h, 0);
  lv_anim_set_time(&a, 120);
  lv_anim_set_delay(&a, 150);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, shopping_list_delete_anim_done);
  lv_anim_start(&a);
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
  // rows are re-added (the scroll_to_view selection logic below then starts
  // from a clean origin).
  lv_obj_scroll_to_y(shopping_list_scroll, 0, LV_ANIM_OFF);
  shopping_list_rendered_count = 0;
  for (int i = 0; i < 50; i++) shopping_list_items[i] = NULL;  // no stale card pointers
  bool reveal = shopping_list_reveal_pending;
  shopping_list_reveal_pending = false;

  // Update title with the item count integrated ("Shopping List • 6").
  // Separator is the bullet glyph (U+2022) — the only mid-dot the built-in
  // Montserrat fonts ship with (U+00B7 is not in the default range).
  if (shopping_list_title_label) {
    char title[40];
    if (g_active.count > 0) {
      snprintf(title, sizeof(title), "Shopping List \xE2\x80\xA2 %d", g_active.count);
    } else {
      snprintf(title, sizeof(title), "Shopping List");
    }
    lv_label_set_text(shopping_list_title_label, title);
  }

  if (g_active.count == 0) {
    bool refresh_running = (refresh_state == REFRESH_WAKE_PENDING ||
                            refresh_state == REFRESH_INFLIGHT);
    if (!g_list_refresh_completed_once && refresh_running) {
      // No data yet (first boot / cache miss) and a refresh is underway —
      // show the pulsing skeleton instead of a premature "No items".
      shopping_list_render_skeleton();
      shopping_list_rendered_sig = 0xFFFFFFFFu;  // placeholder, never matches real content
      return;
    }
    // Empty state. The friendly glyph + "No items on your list" only when a
    // refresh has actually completed (the emptiness is genuine); otherwise a
    // neutral hint.
    if (g_list_refresh_completed_once) {
      lv_obj_t* glyph = lv_label_create(shopping_list_scroll);
      lv_label_set_text(glyph, LV_SYMBOL_LIST);
      lv_obj_set_style_text_font(glyph, &lv_font_montserrat_32, LV_PART_MAIN);
      lv_obj_set_style_text_color(glyph, lv_color_hex(0xD4C4AE), LV_PART_MAIN);
      lv_obj_set_style_pad_top(glyph, 34, LV_PART_MAIN);

      lv_obj_t* empty = lv_label_create(shopping_list_scroll);
      lv_label_set_text(empty, "No items on your list");
      lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, LV_PART_MAIN);
      lv_obj_set_style_text_color(empty, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
      lv_obj_set_style_pad_top(empty, 8, LV_PART_MAIN);
      lv_obj_set_width(empty, 240);
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

      lv_obj_t* hint = lv_label_create(shopping_list_scroll);
      lv_label_set_text(hint, "Pull down to refresh");
      lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), LV_PART_MAIN);
      lv_obj_set_style_pad_top(hint, 2, LV_PART_MAIN);
      lv_obj_set_width(hint, 240);
      lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else {
      lv_obj_t* empty = lv_label_create(shopping_list_scroll);
      lv_label_set_text(empty, "Pull down to refresh");
      lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, LV_PART_MAIN);
      lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), LV_PART_MAIN);
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

  // Create item cards — fixed-height rounded rows sized for the round
  // display (272px clears the circle at every height the cards reach)
  for (int i = 0; i < g_active.count && i < 50; i++) {
    lv_obj_t* card = lv_obj_create(shopping_list_scroll);
    lv_obj_set_size(card, SHOPPING_LIST_CARD_W, SHOPPING_LIST_CARD_H);
    lv_obj_set_style_radius(card, SHOPPING_LIST_CARD_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_pad_left(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_top(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0xD4C4AE), LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Label first (style_card rewrites its text/font/color) — single line,
    // vertically centered, ellipsized
    lv_obj_t* label = lv_label_create(card);
    lv_obj_set_width(label, SHOPPING_LIST_CARD_W - 32);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    shopping_list_style_card(card, i, i == shopping_list_scroll_idx);

    shopping_list_items[i] = card;
    shopping_list_rendered_count++;
  }

  // Scroll selected item into view
  if (shopping_list_scroll_idx < shopping_list_rendered_count && shopping_list_items[shopping_list_scroll_idx]) {
    lv_obj_scroll_to_view(shopping_list_items[shopping_list_scroll_idx], LV_ANIM_OFF);
  }

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
      lv_obj_set_style_opa(card, LV_OPA_TRANSP, 0);
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
static void shopping_list_add_edge_fade(lv_obj_t* parent, int x, int y, int w, bool top) {
  static const lv_opa_t fade_opa[4] = {LV_OPA_COVER, 165, 95, 40};
  for (int i = 0; i < 4; i++) {
    lv_obj_t* strip = lv_obj_create(parent);
    lv_obj_set_size(strip, w, 8);
    lv_obj_set_pos(strip, x, top ? (y + i * 8) : (y - (i + 1) * 8));
    lv_obj_set_style_bg_color(strip, lv_color_hex(0xF5E9D8), LV_PART_MAIN);  // screen bg → transparent steps
    lv_obj_set_style_bg_opa(strip, fade_opa[i], LV_PART_MAIN);
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
    lv_obj_del(shopping_list_screen);
    shopping_list_screen = NULL;
  }
  shopping_list_overlay = NULL;
  shopping_list_overlay_visible = false;
  shopping_list_refresh_ring = NULL;  // children of the deleted screen
  shopping_list_error_toast = NULL;
  shopping_list_ring_hiding = false;
  shopping_list_ring_sweeping = false;
  shopping_list_ring_autohide = false;
  shopping_list_rendered_sig = 0xFFFFFFFFu;  // fresh screen — force a real render

  shopping_list_screen = lv_obj_create(NULL);
  lv_obj_set_size(shopping_list_screen, LV_PCT(100), LV_PCT(100));
  ship_style_plain_screen(shopping_list_screen, lv_color_hex(0xF5E9D8));
  lv_obj_set_style_border_width(shopping_list_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(shopping_list_screen, 0, LV_PART_MAIN);
  lv_obj_clear_flag(shopping_list_screen, LV_OBJ_FLAG_SCROLLABLE);

  // ── Title — centered at the top arc, count integrated by populate() ──
  shopping_list_title_label = lv_label_create(shopping_list_screen);
  lv_label_set_text(shopping_list_title_label, "Shopping List");
  lv_obj_set_style_text_font(shopping_list_title_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(shopping_list_title_label, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_align(shopping_list_title_label, LV_ALIGN_TOP_MID, 0, 34);

  // ── Scrollable item list (y 60..290 — clears the title and back button) ──
  shopping_list_scroll = lv_obj_create(shopping_list_screen);
  lv_obj_set_size(shopping_list_scroll, 300, 230);
  lv_obj_set_pos(shopping_list_scroll, 30, 60);
  lv_obj_set_style_bg_opa(shopping_list_scroll, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(shopping_list_scroll, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(shopping_list_scroll, 0, LV_PART_MAIN);
  // Generous top/bottom pads so the first and last rows can rest in the
  // comfortable middle band, clear of the edge fades.
  lv_obj_set_style_pad_top(shopping_list_scroll, 24, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(shopping_list_scroll, 40, LV_PART_MAIN);
  lv_obj_set_style_pad_left(shopping_list_scroll, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_right(shopping_list_scroll, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(shopping_list_scroll, SHOPPING_LIST_CARD_GAP, LV_PART_MAIN);
  lv_obj_set_flex_flow(shopping_list_scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(shopping_list_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(shopping_list_scroll, LV_SCROLLBAR_MODE_OFF);  // a straight scrollbar fights the round bezel
  lv_obj_set_scroll_dir(shopping_list_scroll, LV_DIR_VER);
  // Touch pull-to-refresh: elastic overscroll past the top + momentum flicks,
  // with scroll events driving the pull gesture detector.
  lv_obj_add_flag(shopping_list_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_add_flag(shopping_list_scroll, LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_SCROLL, NULL);
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(shopping_list_scroll, shopping_list_scroll_event_cb, LV_EVENT_SCROLL_END, NULL);

  // ── Edge fades over the scroll viewport (top + bottom, never block input) ──
  shopping_list_add_edge_fade(shopping_list_screen, 30, 60, 300, true);    // top of viewport
  shopping_list_add_edge_fade(shopping_list_screen, 30, 290, 300, false);  // bottom of viewport

  // ── Back control — circular button, bottom-center inside the circle ──
  shopping_list_back_btn_obj = lv_obj_create(shopping_list_screen);
  lv_obj_set_size(shopping_list_back_btn_obj, 56, 56);
  lv_obj_align(shopping_list_back_btn_obj, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_radius(shopping_list_back_btn_obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(shopping_list_back_btn_obj, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(shopping_list_back_btn_obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(shopping_list_back_btn_obj, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(shopping_list_back_btn_obj, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(shopping_list_back_btn_obj, lv_color_hex(0xD4C4AE), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(shopping_list_back_btn_obj, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_pad_all(shopping_list_back_btn_obj, 0, LV_PART_MAIN);
  lv_obj_clear_flag(shopping_list_back_btn_obj, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* back_lbl = lv_label_create(shopping_list_back_btn_obj);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(back_lbl);

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
  lv_obj_set_width(lbl, 290);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x58a6ff), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_pad_top(lbl, 6, LV_PART_MAIN);
  return lbl;
}

static lv_obj_t* errlog_detail_add_field(lv_obj_t* parent, const char* name, const char* value) {
  char line[128];
  snprintf(line, sizeof(line), "%s: %s", name, value);
  lv_obj_t* lbl = lv_label_create(parent);
  lv_label_set_text(lbl, line);
  lv_obj_set_width(lbl, 290);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xc9d1d9), LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, LV_PART_MAIN);
  return lbl;
}

static lv_obj_t* errlog_detail_add_separator(lv_obj_t* parent) {
  lv_obj_t* sep = lv_obj_create(parent);
  lv_obj_set_size(sep, 270, 1);
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
  if (errlog_detail_screen) return;

  errlog_detail_screen = lv_obj_create(NULL);
  lv_obj_set_size(errlog_detail_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(errlog_detail_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(errlog_detail_screen, lv_color_hex(0x0d1117), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(errlog_detail_screen, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(errlog_detail_screen);
  lv_label_set_text(title, "Error Detail");
  lv_obj_set_style_text_color(title, lv_color_hex(0xf85149), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  errlog_detail_scroll = lv_obj_create(errlog_detail_screen);
  lv_obj_set_size(errlog_detail_scroll, 320, 240);
  lv_obj_align(errlog_detail_scroll, LV_ALIGN_TOP_MID, 0, 48);
  lv_obj_set_style_bg_color(errlog_detail_scroll, lv_color_hex(0x161b22), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(errlog_detail_scroll, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(errlog_detail_scroll, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_border_width(errlog_detail_scroll, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(errlog_detail_scroll, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_all(errlog_detail_scroll, 10, LV_PART_MAIN);
  lv_obj_set_flex_flow(errlog_detail_scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(errlog_detail_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_scrollbar_mode(errlog_detail_scroll, LV_SCROLLBAR_MODE_AUTO);

  errlog_detail_btn_back = lv_obj_create(errlog_detail_screen);
  lv_obj_set_size(errlog_detail_btn_back, 120, 40);
  lv_obj_align(errlog_detail_btn_back, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_style_bg_color(errlog_detail_btn_back, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(errlog_detail_btn_back, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(errlog_detail_btn_back, lv_color_hex(0x484f58), LV_PART_MAIN);
  lv_obj_set_style_border_width(errlog_detail_btn_back, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(errlog_detail_btn_back, 6, LV_PART_MAIN);
  lv_obj_clear_flag(errlog_detail_btn_back, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* back_lbl = lv_label_create(errlog_detail_btn_back);
  lv_label_set_text(back_lbl, "Back");
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xc9d1d9), LV_PART_MAIN);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(back_lbl);

  lv_obj_add_event_cb(errlog_detail_btn_back, errlog_detail_btn_back_event, LV_EVENT_CLICKED, NULL);
}

static void show_errlog_detail(int index) {
  errlog_detail_screen_init();
  lv_obj_clean(errlog_detail_scroll);

  char buf[256];
  if (!errlog_read_entry(index, buf, sizeof(buf))) {
    lv_obj_t* err = lv_label_create(errlog_detail_scroll);
    lv_label_set_text(err, "Failed to read entry");
    lv_obj_set_style_text_color(err, lv_color_hex(0xf85149), LV_PART_MAIN);
    lv_obj_set_style_text_font(err, &lv_font_montserrat_14, LV_PART_MAIN);
  } else {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, buf) != DeserializationError::Ok) {
      lv_obj_t* err = lv_label_create(errlog_detail_scroll);
      lv_label_set_text(err, "Parse error");
      lv_obj_set_style_text_color(err, lv_color_hex(0xf85149), LV_PART_MAIN);
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
          lv_obj_set_width(err_lbl, 290);
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
          lv_obj_set_width(err_lbl, 290);
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
    lv_obj_set_width(entry, 296);
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
    lv_obj_set_width(lbl, 276);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xc9d1d9), LV_PART_MAIN);
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
  if (errlog_screen) return;

  errlog_screen = lv_obj_create(NULL);
  lv_obj_set_size(errlog_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(errlog_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(errlog_screen, lv_color_hex(0x0d1117), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(errlog_screen, LV_OPA_COVER, LV_PART_MAIN);

  lv_obj_t* title = lv_label_create(errlog_screen);
  lv_label_set_text(title, "Error Log");
  lv_obj_set_style_text_color(title, lv_color_hex(0xf85149), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  errlog_count_label = lv_label_create(errlog_screen);
  lv_obj_set_style_text_color(errlog_count_label, lv_color_hex(0x8b949e), LV_PART_MAIN);
  lv_obj_set_style_text_font(errlog_count_label, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_align(errlog_count_label, LV_ALIGN_TOP_MID, 0, 40);

  errlog_scroll_container = lv_obj_create(errlog_screen);
  lv_obj_set_size(errlog_scroll_container, 320, 220);
  lv_obj_align(errlog_scroll_container, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_style_bg_color(errlog_scroll_container, lv_color_hex(0x161b22), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(errlog_scroll_container, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(errlog_scroll_container, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_border_width(errlog_scroll_container, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(errlog_scroll_container, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_all(errlog_scroll_container, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(errlog_scroll_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(errlog_scroll_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_scrollbar_mode(errlog_scroll_container, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_row(errlog_scroll_container, 6, LV_PART_MAIN);

  errlog_btn_back = lv_obj_create(errlog_screen);
  lv_obj_set_size(errlog_btn_back, 100, 36);
  lv_obj_align(errlog_btn_back, LV_ALIGN_BOTTOM_MID, -60, -16);
  lv_obj_set_style_bg_color(errlog_btn_back, lv_color_hex(0x30363d), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(errlog_btn_back, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(errlog_btn_back, lv_color_hex(0x484f58), LV_PART_MAIN);
  lv_obj_set_style_border_width(errlog_btn_back, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(errlog_btn_back, 6, LV_PART_MAIN);
  lv_obj_clear_flag(errlog_btn_back, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* back_lbl = lv_label_create(errlog_btn_back);
  lv_label_set_text(back_lbl, "Back");
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xc9d1d9), LV_PART_MAIN);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(back_lbl);

  lv_obj_add_event_cb(errlog_btn_back, errlog_btn_back_event, LV_EVENT_CLICKED, NULL);

  errlog_btn_clear = lv_obj_create(errlog_screen);
  lv_obj_set_size(errlog_btn_clear, 100, 36);
  lv_obj_align(errlog_btn_clear, LV_ALIGN_BOTTOM_MID, 60, -16);
  lv_obj_set_style_bg_color(errlog_btn_clear, lv_color_hex(0xE53935), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(errlog_btn_clear, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(errlog_btn_clear, lv_color_hex(0xEF5350), LV_PART_MAIN);
  lv_obj_set_style_border_width(errlog_btn_clear, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(errlog_btn_clear, 6, LV_PART_MAIN);
  lv_obj_clear_flag(errlog_btn_clear, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* clear_lbl = lv_label_create(errlog_btn_clear);
  lv_label_set_text(clear_lbl, "Clear Logs");
  lv_obj_set_style_text_color(clear_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(clear_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(clear_lbl);

  lv_obj_add_event_cb(errlog_btn_clear, errlog_btn_clear_event, LV_EVENT_CLICKED, NULL);
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
static void ship_settings_style_button(lv_obj_t* btn, lv_obj_t* label, const char* text) {
  lv_obj_set_size(btn, SHIP_MENU_SETTINGS_BTN_W, SHIP_MENU_SETTINGS_BTN_H);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(btn, 18, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_border_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_spread(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_ofs_x(btn, 4, LV_PART_MAIN);
  lv_obj_set_style_shadow_ofs_y(btn, 6, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(btn, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(btn, LV_OPA_40, LV_PART_MAIN);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_center(label);
}

static void show_ship_settings_screen_impl() {
  if (!ship_menu_settings_screen) {
    ship_menu_settings_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_menu_settings_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ship_menu_settings_screen, lv_color_hex(0xFDF2DE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ship_menu_settings_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(ship_menu_settings_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Reset Wi-Fi
    ship_menu_settings_btn_reset = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_reset, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_RESET_Y);
    ship_menu_settings_label_reset = lv_label_create(ship_menu_settings_btn_reset);
    ship_settings_style_button(ship_menu_settings_btn_reset, ship_menu_settings_label_reset, "Reset Wi-Fi");

    // Backlight
    ship_menu_settings_btn_backlight = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_backlight, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_BACKLIGHT_Y);
    ship_menu_settings_label_backlight = lv_label_create(ship_menu_settings_btn_backlight);
    ship_settings_style_button(ship_menu_settings_btn_backlight, ship_menu_settings_label_backlight, "Backlight");

    // Run OTA Update
    ship_menu_settings_btn_ota = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_ota, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_OTA_Y);
    ship_menu_settings_label_ota = lv_label_create(ship_menu_settings_btn_ota);
    ship_settings_style_button(ship_menu_settings_btn_ota, ship_menu_settings_label_ota, "Run OTA Update");

    // Back
    ship_menu_settings_btn_back = lv_obj_create(ship_menu_settings_screen);
    lv_obj_set_pos(ship_menu_settings_btn_back, SHIP_MENU_SETTINGS_BTN_X, SHIP_MENU_SETTINGS_BACK_Y);
    ship_menu_settings_label_back = lv_label_create(ship_menu_settings_btn_back);
    ship_settings_style_button(ship_menu_settings_btn_back, ship_menu_settings_label_back, "Back");

    // Firmware version (compact, very bottom, inside the circle)
    ship_menu_settings_versions = lv_label_create(ship_menu_settings_screen);
    lv_label_set_text(ship_menu_settings_versions, "");
    lv_obj_set_style_text_font(ship_menu_settings_versions, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(ship_menu_settings_versions, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
    lv_obj_set_style_text_align(ship_menu_settings_versions, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ship_menu_settings_versions, LV_ALIGN_TOP_MID, 0, SHIP_MENU_SETTINGS_VERSION_Y);

    // Status overlay label (hidden; reused for "Starting OTA..." feedback)
    ship_menu_settings_status = lv_label_create(ship_menu_settings_screen);
    lv_label_set_text(ship_menu_settings_status, "");
    lv_obj_set_style_text_color(ship_menu_settings_status, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_align(ship_menu_settings_status, LV_ALIGN_TOP_MID, 0, SHIP_MENU_SETTINGS_VERSION_Y);
    lv_obj_add_flag(ship_menu_settings_status, LV_OBJ_FLAG_HIDDEN);

    // Four-button settings screen (Reset Wi-Fi + Backlight + OTA + Back)
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
    ship_backlight_screen = lv_obj_create(NULL);
    lv_obj_set_size(ship_backlight_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(ship_backlight_screen, LV_OBJ_FLAG_SCROLLABLE);
    ship_style_plain_screen(ship_backlight_screen, lv_color_hex(0xFDF2DE));
    lv_obj_set_style_border_width(ship_backlight_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ship_backlight_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_backlight_screen, 0, LV_PART_MAIN);

    // Title above the ring
    lv_obj_t* title = lv_label_create(ship_backlight_screen);
    lv_label_set_text(title, "Backlight");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -110);

    // Gold brightness ring over faint teal track (mirrors listening ring)
    ship_backlight_ring = lv_arc_create(ship_backlight_screen);
    lv_obj_remove_style(ship_backlight_ring, NULL, LV_PART_KNOB);
    lv_obj_set_size(ship_backlight_ring, 190, 190);
    lv_obj_align(ship_backlight_ring, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_range(ship_backlight_ring, 0, 1000);
    lv_arc_set_value(ship_backlight_ring, 1000);
    lv_arc_set_bg_angles(ship_backlight_ring, 0, 360);
    lv_arc_set_rotation(ship_backlight_ring, 270);
    lv_obj_set_style_arc_width(ship_backlight_ring, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ship_backlight_ring, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ship_backlight_ring, lv_color_hex(0x296065), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(ship_backlight_ring, LV_OPA_20, LV_PART_MAIN);   // faint teal track
    lv_obj_set_style_arc_color(ship_backlight_ring, lv_color_hex(0xF6BF41), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ship_backlight_ring, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ship_backlight_ring, true, LV_PART_INDICATOR);
    lv_obj_set_style_outline_width(ship_backlight_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(ship_backlight_ring, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ship_backlight_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // Big centered percentage label
    ship_backlight_pct_label = lv_label_create(ship_backlight_screen);
    lv_label_set_text(ship_backlight_pct_label, "100%");
    lv_obj_set_style_text_font(ship_backlight_pct_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(ship_backlight_pct_label, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_align(ship_backlight_pct_label, LV_ALIGN_CENTER, 0, 0);

    // Hint below the ring
    lv_obj_t* hint = lv_label_create(ship_backlight_screen);
    lv_label_set_text(hint, "Scroll to adjust \xC2\xB7 Tap to save");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 110);
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

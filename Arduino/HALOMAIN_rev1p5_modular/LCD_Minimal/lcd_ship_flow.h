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
  if (obj) {
    lv_obj_set_style_transform_zoom((lv_obj_t*)obj, (uint16_t)v, LV_PART_MAIN);
  }
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
}

static void ship_start_logged_success_animation() {
  if (!ship_logged_icon || !ship_logged_label || !ship_logged_subtitle) {
    return;
  }

  lv_obj_set_y(ship_logged_icon, -86);
  lv_obj_set_style_text_opa(ship_logged_icon, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_logged_label, 40);
  lv_obj_set_style_text_opa(ship_logged_label, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_logged_subtitle, 92);
  lv_obj_set_style_text_opa(ship_logged_subtitle, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t icon_fade;
  lv_anim_init(&icon_fade);
  lv_anim_set_var(&icon_fade, ship_logged_icon);
  lv_anim_set_values(&icon_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&icon_fade, 260);
  lv_anim_set_exec_cb(&icon_fade, ship_anim_set_text_opa);
  lv_anim_start(&icon_fade);

  lv_anim_t icon_y;
  lv_anim_init(&icon_y);
  lv_anim_set_var(&icon_y, ship_logged_icon);
  lv_anim_set_values(&icon_y, -86, -42);
  lv_anim_set_time(&icon_y, 320);
  lv_anim_set_path_cb(&icon_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&icon_y, ship_anim_set_y);
  lv_anim_start(&icon_y);

  lv_anim_t label_fade;
  lv_anim_init(&label_fade);
  lv_anim_set_var(&label_fade, ship_logged_label);
  lv_anim_set_values(&label_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&label_fade, 260);
  lv_anim_set_delay(&label_fade, 90);
  lv_anim_set_exec_cb(&label_fade, ship_anim_set_text_opa);
  lv_anim_start(&label_fade);

  lv_anim_t label_y;
  lv_anim_init(&label_y);
  lv_anim_set_var(&label_y, ship_logged_label);
  lv_anim_set_values(&label_y, 40, 8);
  lv_anim_set_time(&label_y, 320);
  lv_anim_set_delay(&label_y, 90);
  lv_anim_set_path_cb(&label_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&label_y, ship_anim_set_y);
  lv_anim_start(&label_y);

  lv_anim_t subtitle_fade;
  lv_anim_init(&subtitle_fade);
  lv_anim_set_var(&subtitle_fade, ship_logged_subtitle);
  lv_anim_set_values(&subtitle_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&subtitle_fade, 260);
  lv_anim_set_delay(&subtitle_fade, 150);
  lv_anim_set_exec_cb(&subtitle_fade, ship_anim_set_text_opa);
  lv_anim_start(&subtitle_fade);

  lv_anim_t subtitle_y;
  lv_anim_init(&subtitle_y);
  lv_anim_set_var(&subtitle_y, ship_logged_subtitle);
  lv_anim_set_values(&subtitle_y, 92, 52);
  lv_anim_set_time(&subtitle_y, 320);
  lv_anim_set_delay(&subtitle_y, 150);
  lv_anim_set_path_cb(&subtitle_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&subtitle_y, ship_anim_set_y);
  lv_anim_start(&subtitle_y);
}

static void ship_start_voice_ack_animation() {
  if (!ship_voice_ack_icon || !ship_voice_ack_label || !ship_voice_ack_subtitle) {
    return;
  }

  lv_anim_del(ship_voice_ack_icon, NULL);
  lv_anim_del(ship_voice_ack_label, NULL);
  lv_anim_del(ship_voice_ack_subtitle, NULL);

  lv_obj_set_y(ship_voice_ack_icon, -86);
  lv_obj_set_style_text_opa(ship_voice_ack_icon, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_voice_ack_label, 40);
  lv_obj_set_style_text_opa(ship_voice_ack_label, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_voice_ack_subtitle, 92);
  lv_obj_set_style_text_opa(ship_voice_ack_subtitle, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t icon_fade;
  lv_anim_init(&icon_fade);
  lv_anim_set_var(&icon_fade, ship_voice_ack_icon);
  lv_anim_set_values(&icon_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&icon_fade, 220);
  lv_anim_set_exec_cb(&icon_fade, ship_anim_set_text_opa);
  lv_anim_start(&icon_fade);

  lv_anim_t icon_y;
  lv_anim_init(&icon_y);
  lv_anim_set_var(&icon_y, ship_voice_ack_icon);
  lv_anim_set_values(&icon_y, -86, -42);
  lv_anim_set_time(&icon_y, 300);
  lv_anim_set_path_cb(&icon_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&icon_y, ship_anim_set_y);
  lv_anim_start(&icon_y);

  lv_anim_t label_fade;
  lv_anim_init(&label_fade);
  lv_anim_set_var(&label_fade, ship_voice_ack_label);
  lv_anim_set_values(&label_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&label_fade, 220);
  lv_anim_set_delay(&label_fade, 70);
  lv_anim_set_exec_cb(&label_fade, ship_anim_set_text_opa);
  lv_anim_start(&label_fade);

  lv_anim_t label_y;
  lv_anim_init(&label_y);
  lv_anim_set_var(&label_y, ship_voice_ack_label);
  lv_anim_set_values(&label_y, 40, 8);
  lv_anim_set_time(&label_y, 300);
  lv_anim_set_delay(&label_y, 70);
  lv_anim_set_path_cb(&label_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&label_y, ship_anim_set_y);
  lv_anim_start(&label_y);

  lv_anim_t subtitle_fade;
  lv_anim_init(&subtitle_fade);
  lv_anim_set_var(&subtitle_fade, ship_voice_ack_subtitle);
  lv_anim_set_values(&subtitle_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&subtitle_fade, 220);
  lv_anim_set_delay(&subtitle_fade, 130);
  lv_anim_set_exec_cb(&subtitle_fade, ship_anim_set_text_opa);
  lv_anim_start(&subtitle_fade);

  lv_anim_t subtitle_y;
  lv_anim_init(&subtitle_y);
  lv_anim_set_var(&subtitle_y, ship_voice_ack_subtitle);
  lv_anim_set_values(&subtitle_y, 92, 52);
  lv_anim_set_time(&subtitle_y, 300);
  lv_anim_set_delay(&subtitle_y, 130);
  lv_anim_set_path_cb(&subtitle_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&subtitle_y, ship_anim_set_y);
  lv_anim_start(&subtitle_y);
}

static void ship_update_hold_still_countdown() {
  if (!ship_hold_countdown_label || ship_hold_anim_start_ms == 0) {
    return;
  }

  unsigned long elapsed_ms = millis() - ship_hold_anim_start_ms;
  bool capture_phase = (elapsed_ms >= SHIP_HOLD_COUNTDOWN_MS);
  if (ship_hold_ring) {
    uint16_t ring_value = 0;
    if (!capture_phase) {
      uint32_t remaining_ms = SHIP_HOLD_COUNTDOWN_MS - elapsed_ms;
      ring_value = (uint16_t)((remaining_ms * 1000UL) / SHIP_HOLD_COUNTDOWN_MS);
    } else {
      uint32_t pulse_ms = (elapsed_ms - SHIP_HOLD_COUNTDOWN_MS) % SHIP_HOLD_CAPTURE_PULSE_MS;
      if (pulse_ms < (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL)) {
        ring_value = (uint16_t)(280UL + ((pulse_ms * 720UL) / (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL)));
      } else {
        ring_value = (uint16_t)(1000UL - (((pulse_ms - (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL)) * 720UL) /
                                          (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL)));
      }
    }
    lv_arc_set_value(ship_hold_ring, ring_value);
  }

  if (!capture_phase) {
    if (ship_hold_capture_phase) {
      ship_hold_capture_phase = false;
      ship_hold_capture_frame = -1;
      if (ship_hold_capture_icon) {
        lv_obj_add_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_opa(ship_hold_capture_icon, LV_OPA_0, LV_PART_MAIN);
        lv_obj_set_style_transform_zoom(ship_hold_capture_icon, 256, LV_PART_MAIN);
      }
      lv_obj_clear_flag(ship_hold_countdown_label, LV_OBJ_FLAG_HIDDEN);
      if (ship_hold_subtitle) {
        lv_label_set_text(ship_hold_subtitle, "Capturing your item/meal");
      }
    }

    int countdown = 3;
    countdown = 3 - (int)(elapsed_ms / 1000UL);
    if (countdown < 1) {
      countdown = 1;
    }

    if (countdown == ship_hold_countdown_value) {
      return;
    }

    ship_hold_countdown_value = countdown;
    char countdown_text[4];
    snprintf(countdown_text, sizeof(countdown_text), "%d", countdown);
    lv_label_set_text(ship_hold_countdown_label, countdown_text);
    return;
  }

  if (!ship_hold_capture_phase) {
    ship_hold_capture_phase = true;
    lv_obj_add_flag(ship_hold_countdown_label, LV_OBJ_FLAG_HIDDEN);
    if (ship_hold_capture_icon) {
      lv_obj_clear_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
    }
    if (ship_hold_subtitle) {
      lv_label_set_text(ship_hold_subtitle, "Capturing image...");
    }
  }

  int capture_frame = (int)(((elapsed_ms - SHIP_HOLD_COUNTDOWN_MS) / 125UL) % 8UL);
  if (capture_frame == ship_hold_capture_frame) {
    return;
  }

  ship_hold_capture_frame = capture_frame;
  ship_hold_countdown_value = -1;
  if (ship_hold_capture_icon) {
    uint32_t pulse_ms = (elapsed_ms - SHIP_HOLD_COUNTDOWN_MS) % SHIP_HOLD_CAPTURE_PULSE_MS;
    bool expanding = pulse_ms < (SHIP_HOLD_CAPTURE_PULSE_MS / 2UL);
    uint32_t pulse_half_ms = SHIP_HOLD_CAPTURE_PULSE_MS / 2UL;
    uint32_t pulse_pos = expanding ? pulse_ms : (pulse_ms - pulse_half_ms);
    uint16_t zoom = expanding
                        ? (uint16_t)(256UL + ((pulse_pos * 96UL) / pulse_half_ms))
                        : (uint16_t)(352UL - ((pulse_pos * 96UL) / pulse_half_ms));
    lv_opa_t icon_opa = (lv_opa_t)(expanding
                                       ? (LV_OPA_70 + ((pulse_pos * (LV_OPA_COVER - LV_OPA_70)) / pulse_half_ms))
                                       : (LV_OPA_COVER - ((pulse_pos * (LV_OPA_COVER - LV_OPA_70)) / pulse_half_ms)));
    lv_obj_set_style_transform_zoom(ship_hold_capture_icon, zoom, LV_PART_MAIN);
    lv_obj_set_style_text_opa(ship_hold_capture_icon, icon_opa, LV_PART_MAIN);
    lv_obj_align(ship_hold_capture_icon, LV_ALIGN_CENTER, 0, 12);
  }
}

static void ship_start_hold_still_animation() {
  if (!ship_hold_title || !ship_hold_ring || !ship_hold_countdown_label || !ship_hold_subtitle || !ship_hold_capture_icon) {
    return;
  }

  ship_hold_anim_start_ms = millis();
  ship_hold_countdown_value = -1;
  ship_hold_capture_frame = -1;
  ship_hold_capture_phase = false;
  lv_label_set_text(ship_hold_subtitle, "Capturing your item/meal");
  lv_obj_clear_flag(ship_hold_countdown_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_opa(ship_hold_capture_icon, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_transform_zoom(ship_hold_capture_icon, 256, LV_PART_MAIN);
  ship_update_hold_still_countdown();

  lv_anim_del(ship_hold_title, NULL);
  lv_anim_del(ship_hold_ring, NULL);
  lv_anim_del(ship_hold_countdown_label, NULL);
  lv_anim_del(ship_hold_capture_icon, NULL);
  lv_anim_del(ship_hold_subtitle, NULL);

  lv_obj_set_y(ship_hold_title, -132);
  lv_obj_set_style_text_opa(ship_hold_title, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_hold_ring, 72);
  ship_anim_set_arc_opa(ship_hold_ring, LV_OPA_0);
  lv_obj_set_y(ship_hold_countdown_label, 24);
  lv_obj_set_style_text_opa(ship_hold_countdown_label, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_hold_capture_icon, 18);
  lv_obj_set_style_text_opa(ship_hold_capture_icon, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_hold_subtitle, 118);
  lv_obj_set_style_text_opa(ship_hold_subtitle, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t title_fade;
  lv_anim_init(&title_fade);
  lv_anim_set_var(&title_fade, ship_hold_title);
  lv_anim_set_values(&title_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&title_fade, 280);
  lv_anim_set_exec_cb(&title_fade, ship_anim_set_text_opa);
  lv_anim_start(&title_fade);

  lv_anim_t title_y;
  lv_anim_init(&title_y);
  lv_anim_set_var(&title_y, ship_hold_title);
  lv_anim_set_values(&title_y, -132, -82);
  lv_anim_set_time(&title_y, 340);
  lv_anim_set_path_cb(&title_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&title_y, ship_anim_set_y);
  lv_anim_start(&title_y);

  lv_anim_t ring_fade;
  lv_anim_init(&ring_fade);
  lv_anim_set_var(&ring_fade, ship_hold_ring);
  lv_anim_set_values(&ring_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&ring_fade, 300);
  lv_anim_set_delay(&ring_fade, 90);
  lv_anim_set_exec_cb(&ring_fade, ship_anim_set_arc_opa);
  lv_anim_start(&ring_fade);

  lv_anim_t ring_y;
  lv_anim_init(&ring_y);
  lv_anim_set_var(&ring_y, ship_hold_ring);
  lv_anim_set_values(&ring_y, 72, 12);
  lv_anim_set_time(&ring_y, 360);
  lv_anim_set_delay(&ring_y, 90);
  lv_anim_set_path_cb(&ring_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&ring_y, ship_anim_set_y);
  lv_anim_start(&ring_y);

  lv_anim_t count_fade;
  lv_anim_init(&count_fade);
  lv_anim_set_var(&count_fade, ship_hold_countdown_label);
  lv_anim_set_values(&count_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&count_fade, 260);
  lv_anim_set_delay(&count_fade, 150);
  lv_anim_set_exec_cb(&count_fade, ship_anim_set_text_opa);
  lv_anim_start(&count_fade);

  lv_anim_t count_y;
  lv_anim_init(&count_y);
  lv_anim_set_var(&count_y, ship_hold_countdown_label);
  lv_anim_set_values(&count_y, 24, 12);
  lv_anim_set_time(&count_y, 320);
  lv_anim_set_delay(&count_y, 150);
  lv_anim_set_path_cb(&count_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&count_y, ship_anim_set_y);
  lv_anim_start(&count_y);

  lv_anim_t subtitle_fade;
  lv_anim_init(&subtitle_fade);
  lv_anim_set_var(&subtitle_fade, ship_hold_subtitle);
  lv_anim_set_values(&subtitle_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&subtitle_fade, 240);
  lv_anim_set_delay(&subtitle_fade, 220);
  lv_anim_set_exec_cb(&subtitle_fade, ship_anim_set_text_opa);
  lv_anim_start(&subtitle_fade);

  lv_anim_t subtitle_y;
  lv_anim_init(&subtitle_y);
  lv_anim_set_var(&subtitle_y, ship_hold_subtitle);
  lv_anim_set_values(&subtitle_y, 118, 86);
  lv_anim_set_time(&subtitle_y, 320);
  lv_anim_set_delay(&subtitle_y, 220);
  lv_anim_set_path_cb(&subtitle_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&subtitle_y, ship_anim_set_y);
  lv_anim_start(&subtitle_y);
}

static void ship_start_expiry_choice_animation() {
  if (!expiry_choice_quantity_label || !ship_expiry_choice_skip_btn || !ship_expiry_choice_add_btn) {
    return;
  }

  lv_anim_del(expiry_choice_quantity_label, NULL);
  if (ship_expiry_choice_qty_prefix) {
    lv_anim_del(ship_expiry_choice_qty_prefix, NULL);
  }
  if (ship_expiry_choice_prompt) {
    lv_anim_del(ship_expiry_choice_prompt, NULL);
  }
  lv_anim_del(ship_expiry_choice_skip_btn, NULL);
  lv_anim_del(ship_expiry_choice_add_btn, NULL);

  lv_obj_set_y(expiry_choice_quantity_label, -36);
  lv_obj_set_style_text_opa(expiry_choice_quantity_label, LV_OPA_0, LV_PART_MAIN);
  if (ship_expiry_choice_qty_prefix) {
    lv_obj_set_y(ship_expiry_choice_qty_prefix, -28);
    lv_obj_set_style_text_opa(ship_expiry_choice_qty_prefix, LV_OPA_0, LV_PART_MAIN);
  }
  if (ship_expiry_choice_prompt) {
    lv_obj_set_y(ship_expiry_choice_prompt, 2);
    lv_obj_set_style_text_opa(ship_expiry_choice_prompt, LV_OPA_0, LV_PART_MAIN);
  }
  lv_obj_set_x(ship_expiry_choice_skip_btn, -220);
  lv_obj_set_style_opa(ship_expiry_choice_skip_btn, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_x(ship_expiry_choice_add_btn, 220);
  lv_obj_set_style_opa(ship_expiry_choice_add_btn, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t qty_fade;
  lv_anim_init(&qty_fade);
  lv_anim_set_var(&qty_fade, expiry_choice_quantity_label);
  lv_anim_set_values(&qty_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&qty_fade, 240);
  lv_anim_set_exec_cb(&qty_fade, ship_anim_set_text_opa);
  lv_anim_start(&qty_fade);

  lv_anim_t qty_y;
  lv_anim_init(&qty_y);
  lv_anim_set_var(&qty_y, expiry_choice_quantity_label);
  lv_anim_set_values(&qty_y, -36, SHIP_CHOICE_VALUE_Y);
  lv_anim_set_time(&qty_y, 320);
  lv_anim_set_path_cb(&qty_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&qty_y, ship_anim_set_y);
  lv_anim_start(&qty_y);

  if (ship_expiry_choice_qty_prefix) {
    lv_anim_t qty_prefix_fade;
    lv_anim_init(&qty_prefix_fade);
    lv_anim_set_var(&qty_prefix_fade, ship_expiry_choice_qty_prefix);
    lv_anim_set_values(&qty_prefix_fade, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&qty_prefix_fade, 220);
    lv_anim_set_exec_cb(&qty_prefix_fade, ship_anim_set_text_opa);
    lv_anim_start(&qty_prefix_fade);

    lv_anim_t qty_prefix_y;
    lv_anim_init(&qty_prefix_y);
    lv_anim_set_var(&qty_prefix_y, ship_expiry_choice_qty_prefix);
    lv_anim_set_values(&qty_prefix_y, -28, SHIP_CHOICE_VALUE_Y);
    lv_anim_set_time(&qty_prefix_y, 300);
    lv_anim_set_path_cb(&qty_prefix_y, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&qty_prefix_y, ship_anim_set_y);
    lv_anim_start(&qty_prefix_y);
  }

  if (ship_expiry_choice_prompt) {
    lv_anim_t prompt_fade;
    lv_anim_init(&prompt_fade);
    lv_anim_set_var(&prompt_fade, ship_expiry_choice_prompt);
    lv_anim_set_values(&prompt_fade, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&prompt_fade, 220);
    lv_anim_set_delay(&prompt_fade, 70);
    lv_anim_set_exec_cb(&prompt_fade, ship_anim_set_text_opa);
    lv_anim_start(&prompt_fade);

    lv_anim_t prompt_y;
    lv_anim_init(&prompt_y);
    lv_anim_set_var(&prompt_y, ship_expiry_choice_prompt);
    lv_anim_set_values(&prompt_y, 2, SHIP_CHOICE_PROMPT_Y);
    lv_anim_set_time(&prompt_y, 300);
    lv_anim_set_delay(&prompt_y, 70);
    lv_anim_set_path_cb(&prompt_y, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&prompt_y, ship_anim_set_y);
    lv_anim_start(&prompt_y);
  }

  lv_anim_t skip_fade;
  lv_anim_init(&skip_fade);
  lv_anim_set_var(&skip_fade, ship_expiry_choice_skip_btn);
  lv_anim_set_values(&skip_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&skip_fade, 240);
  lv_anim_set_delay(&skip_fade, 120);
  lv_anim_set_exec_cb(&skip_fade, ship_anim_set_obj_opa);
  lv_anim_start(&skip_fade);

  lv_anim_t skip_x;
  lv_anim_init(&skip_x);
  lv_anim_set_var(&skip_x, ship_expiry_choice_skip_btn);
  lv_anim_set_values(&skip_x, -220, -84);
  lv_anim_set_time(&skip_x, 340);
  lv_anim_set_delay(&skip_x, 120);
  lv_anim_set_path_cb(&skip_x, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&skip_x, ship_anim_set_x);
  lv_anim_start(&skip_x);

  lv_anim_t add_fade;
  lv_anim_init(&add_fade);
  lv_anim_set_var(&add_fade, ship_expiry_choice_add_btn);
  lv_anim_set_values(&add_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&add_fade, 240);
  lv_anim_set_delay(&add_fade, 180);
  lv_anim_set_exec_cb(&add_fade, ship_anim_set_obj_opa);
  lv_anim_start(&add_fade);

  lv_anim_t add_x;
  lv_anim_init(&add_x);
  lv_anim_set_var(&add_x, ship_expiry_choice_add_btn);
  lv_anim_set_values(&add_x, 220, 84);
  lv_anim_set_time(&add_x, 340);
  lv_anim_set_delay(&add_x, 180);
  lv_anim_set_path_cb(&add_x, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&add_x, ship_anim_set_x);
  lv_anim_start(&add_x);
}

static void ship_init_hold_still() {
  // LCD_Minimal/LCD_Minimal.ino: ship_init_hold_still
  if (ship_hold_screen) {
    return;
  }
  ship_hold_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_hold_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_hold_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_HOLD_STILL", "plain_screen");
  ship_style_plain_screen(ship_hold_screen, lv_color_hex(0xF5E9D8));

  ship_hold_title = lv_label_create(ship_hold_screen);
  lv_label_set_text(ship_hold_title, "Hold Still");
  lv_obj_set_style_text_font(ship_hold_title, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_hold_title, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_hold_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_hold_title, LV_ALIGN_CENTER, 0, -82);

  ship_hold_ring = lv_arc_create(ship_hold_screen);
  lv_obj_remove_style(ship_hold_ring, NULL, LV_PART_KNOB);
  lv_obj_set_size(ship_hold_ring, 104, 104);
  lv_arc_set_range(ship_hold_ring, 0, 1000);
  lv_arc_set_value(ship_hold_ring, 1000);
  lv_arc_set_bg_angles(ship_hold_ring, 0, 360);
  lv_arc_set_rotation(ship_hold_ring, 270);
  lv_obj_set_style_arc_width(ship_hold_ring, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_hold_ring, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ship_hold_ring, lv_color_hex(0xD4C8B8), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ship_hold_ring, (lv_opa_t)110, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ship_hold_ring, lv_color_hex(0x1F4D2B), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_hold_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_outline_width(ship_hold_ring, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_hold_ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_hold_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(ship_hold_ring, LV_ALIGN_CENTER, 0, 12);

  ship_hold_countdown_label = lv_label_create(ship_hold_screen);
  lv_label_set_text(ship_hold_countdown_label, "5");
  lv_obj_set_style_text_font(ship_hold_countdown_label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_hold_countdown_label, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_hold_countdown_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_hold_countdown_label, LV_ALIGN_CENTER, 0, 12);

  ship_hold_capture_icon = lv_label_create(ship_hold_screen);
  lv_label_set_text(ship_hold_capture_icon, LV_SYMBOL_IMAGE);
  lv_obj_set_style_text_font(ship_hold_capture_icon, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_hold_capture_icon, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_hold_capture_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_transform_zoom(ship_hold_capture_icon, 256, LV_PART_MAIN);
  lv_obj_align(ship_hold_capture_icon, LV_ALIGN_CENTER, 0, 12);
  lv_obj_add_flag(ship_hold_capture_icon, LV_OBJ_FLAG_HIDDEN);

  ship_hold_subtitle = lv_label_create(ship_hold_screen);
  lv_label_set_text(ship_hold_subtitle, "Capturing your item/meal");
  lv_obj_set_style_text_font(ship_hold_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_hold_subtitle, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_hold_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_hold_subtitle, LV_ALIGN_CENTER, 0, 86);
}

static void ship_init_expiry_choice() {
  if (ship_expiry_choice_screen) {
    return;
  }
  ship_expiry_choice_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_expiry_choice_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_expiry_choice_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_EXPIRY_CHOICE", "plain_screen");
  ship_style_plain_screen(ship_expiry_choice_screen, lv_color_hex(0xF5E9D8));
  lv_obj_set_style_border_width(ship_expiry_choice_screen, 0, LV_PART_MAIN);

  ship_expiry_choice_timeout_ring = lv_arc_create(ship_expiry_choice_screen);
  lv_obj_remove_style(ship_expiry_choice_timeout_ring, NULL, LV_PART_KNOB);
  lv_obj_set_size(ship_expiry_choice_timeout_ring, 356, 356);
  lv_arc_set_range(ship_expiry_choice_timeout_ring, 0, 1000);
  lv_arc_set_value(ship_expiry_choice_timeout_ring, 1000);
  lv_arc_set_bg_angles(ship_expiry_choice_timeout_ring, 0, 360);
  lv_arc_set_rotation(ship_expiry_choice_timeout_ring, 270);
  lv_obj_set_style_arc_width(ship_expiry_choice_timeout_ring, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_expiry_choice_timeout_ring, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_expiry_choice_timeout_ring, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ship_expiry_choice_timeout_ring, lv_color_hex(0x1A1A1A), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_expiry_choice_timeout_ring, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_outline_width(ship_expiry_choice_timeout_ring, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_expiry_choice_timeout_ring, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_expiry_choice_timeout_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(ship_expiry_choice_timeout_ring);

  expiry_choice_quantity_label = lv_label_create(ship_expiry_choice_screen);
  lv_label_set_text(expiry_choice_quantity_label, "1");
  lv_obj_set_style_text_font(expiry_choice_quantity_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(expiry_choice_quantity_label, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_text_align(expiry_choice_quantity_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(expiry_choice_quantity_label, LV_ALIGN_TOP_MID, 26, SHIP_CHOICE_VALUE_Y);

  ship_expiry_choice_qty_prefix = lv_label_create(ship_expiry_choice_screen);
  lv_label_set_text(ship_expiry_choice_qty_prefix, "QTY:");
  lv_obj_set_style_text_font(ship_expiry_choice_qty_prefix, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_expiry_choice_qty_prefix, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_expiry_choice_qty_prefix, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_expiry_choice_qty_prefix, LV_ALIGN_TOP_MID, -26, SHIP_CHOICE_VALUE_Y);

  ship_expiry_choice_prompt = lv_label_create(ship_expiry_choice_screen);
  lv_label_set_text(ship_expiry_choice_prompt, "Turn knob to change QTY");
  lv_obj_set_style_text_font(ship_expiry_choice_prompt, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_expiry_choice_prompt, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_expiry_choice_prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_expiry_choice_prompt, LV_ALIGN_TOP_MID, 0, SHIP_CHOICE_PROMPT_Y);

  ship_expiry_choice_skip_btn = lv_btn_create(ship_expiry_choice_screen);
  lv_obj_set_size(ship_expiry_choice_skip_btn, 135, SHIP_CHOICE_BUTTON_HEIGHT);
  lv_obj_set_style_radius(ship_expiry_choice_skip_btn, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_expiry_choice_skip_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_expiry_choice_skip_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_expiry_choice_skip_btn, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(ship_expiry_choice_skip_btn, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_expiry_choice_skip_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_expiry_choice_skip_btn, 0, LV_PART_MAIN);
  lv_obj_align(ship_expiry_choice_skip_btn, LV_ALIGN_CENTER, -84, SHIP_CHOICE_BUTTON_Y);

  ship_expiry_choice_skip_label = lv_label_create(ship_expiry_choice_skip_btn);
  lv_label_set_text(ship_expiry_choice_skip_label, "Skip");
  lv_obj_set_style_text_font(ship_expiry_choice_skip_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_expiry_choice_skip_label, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_expiry_choice_skip_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(ship_expiry_choice_skip_label);

  ship_expiry_choice_add_btn = lv_btn_create(ship_expiry_choice_screen);
  lv_obj_set_size(ship_expiry_choice_add_btn, 135, SHIP_CHOICE_BUTTON_HEIGHT);
  lv_obj_set_style_radius(ship_expiry_choice_add_btn, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_expiry_choice_add_btn, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_expiry_choice_add_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_expiry_choice_add_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_expiry_choice_add_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_expiry_choice_add_btn, 0, LV_PART_MAIN);
  lv_obj_align(ship_expiry_choice_add_btn, LV_ALIGN_CENTER, 84, SHIP_CHOICE_BUTTON_Y);

  ship_expiry_choice_add_label = lv_label_create(ship_expiry_choice_add_btn);
  lv_label_set_text(ship_expiry_choice_add_label, "Add\nExpiration");
  lv_obj_set_style_text_font(ship_expiry_choice_add_label, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_expiry_choice_add_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_expiry_choice_add_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_width(ship_expiry_choice_add_label, 110);
  lv_obj_center(ship_expiry_choice_add_label);
}

static void ship_init_processing() {
  // LCD_Minimal/LCD_Minimal.ino: ship_init_processing
  if (ship_processing_screen) {
    return;
  }
  ship_processing_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_processing_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_processing_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_PROCESSING", "plain_screen");
  ship_style_plain_screen(ship_processing_screen, lv_color_hex(0xF5E9D8));
  lv_obj_set_style_border_width(ship_processing_screen, 0, LV_PART_MAIN);

  ship_processing_fill = lv_obj_create(ship_processing_screen);
  lv_obj_set_size(ship_processing_fill, 360, 0);
  lv_obj_align(ship_processing_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_radius(ship_processing_fill, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_processing_fill, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_processing_fill, LV_OPA_80, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_processing_fill, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_processing_fill, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_processing_fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ship_processing_fill, LV_OBJ_FLAG_HIDDEN);

  ship_processing_halo = lv_obj_create(ship_processing_screen);
  lv_obj_set_size(ship_processing_halo, 132, 132);
  lv_obj_align(ship_processing_halo, LV_ALIGN_CENTER, 0, -54);
  lv_obj_set_style_radius(ship_processing_halo, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ship_processing_halo, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ship_processing_halo, 31, LV_PART_MAIN);
  lv_obj_set_style_border_width(ship_processing_halo, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_processing_halo, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_processing_halo, 34, LV_PART_MAIN);
  lv_obj_set_style_shadow_color(ship_processing_halo, lv_color_hex(0x1F4D2B), LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(ship_processing_halo, LV_OPA_30, LV_PART_MAIN);
  lv_obj_set_style_shadow_spread(ship_processing_halo, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_processing_halo, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  ship_processing_spinner = lv_spinner_create(ship_processing_screen, 1100, 90);
  lv_obj_set_size(ship_processing_spinner, 92, 92);
  lv_obj_align(ship_processing_spinner, LV_ALIGN_CENTER, 0, -54);
  lv_obj_set_style_bg_opa(ship_processing_spinner, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_processing_spinner, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ship_processing_spinner, lv_color_hex(0xD4C8B8), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(ship_processing_spinner, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ship_processing_spinner, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ship_processing_spinner, lv_color_hex(0x1F4D2B), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(ship_processing_spinner, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ship_processing_spinner, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(ship_processing_spinner, true, LV_PART_INDICATOR);
  lv_obj_set_style_shadow_width(ship_processing_spinner, 0, LV_PART_MAIN);
  lv_obj_clear_flag(ship_processing_spinner, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  ship_processing_label = lv_label_create(ship_processing_screen);
  lv_label_set_text(ship_processing_label, "Processing");
  lv_obj_set_style_text_font(ship_processing_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_processing_label, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_processing_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_processing_label, LV_ALIGN_CENTER, 0, 44);

  ship_processing_subtitle = lv_label_create(ship_processing_screen);
  lv_label_set_text(ship_processing_subtitle, "Working on your request");
  lv_obj_set_style_text_font(ship_processing_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_processing_subtitle, lv_color_hex(0x4A4A4A), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_processing_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_processing_subtitle, LV_ALIGN_CENTER, 0, 82);
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
  if (!ship_processing_fill) {
    return;
  }
  if (pct > 100) {
    pct = 100;
  }
  uint16_t fill_h = (uint16_t)((360UL * (unsigned long)pct) / 100UL);
  if (pct > 0 && fill_h == 0) {
    fill_h = 1;
  }
  lv_obj_set_size(ship_processing_fill, 360, fill_h);
  lv_obj_align(ship_processing_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void ship_set_processing_text(const char* text) {
  if (!ship_processing_label) {
    return;
  }
  lv_label_set_text(ship_processing_label,
                    (text && text[0]) ? text : "Processing");
}

static void ship_configure_processing_layout(bool is_dish) {
  if (!ship_processing_fill || !ship_processing_halo || !ship_processing_spinner || !ship_processing_label || !ship_processing_subtitle) {
    return;
  }

  if (is_dish) {
    lv_obj_clear_flag(ship_processing_fill, LV_OBJ_FLAG_HIDDEN);
    ship_set_processing_fill_pct(dish_processing_progress_pct);
    lv_obj_add_flag(ship_processing_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(ship_processing_halo, 152, 152);
    lv_obj_align(ship_processing_halo, LV_ALIGN_CENTER, 0, -48);
    lv_obj_set_style_shadow_width(ship_processing_halo, 40, LV_PART_MAIN);
    lv_obj_set_style_text_font(ship_processing_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(ship_processing_label, LV_ALIGN_CENTER, 0, -48);
    lv_obj_set_style_text_font(ship_processing_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(ship_processing_subtitle, LV_ALIGN_CENTER, 0, 58);
  } else {
    lv_obj_add_flag(ship_processing_fill, LV_OBJ_FLAG_HIDDEN);
    ship_set_processing_fill_pct(0);
    lv_obj_clear_flag(ship_processing_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(ship_processing_halo, 132, 132);
    lv_obj_align(ship_processing_halo, LV_ALIGN_CENTER, 0, -54);
    lv_obj_set_style_shadow_width(ship_processing_halo, 34, LV_PART_MAIN);
    lv_obj_set_style_text_font(ship_processing_label, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(ship_processing_label, LV_ALIGN_CENTER, 0, 44);
    lv_obj_set_style_text_font(ship_processing_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(ship_processing_subtitle, LV_ALIGN_CENTER, 0, 82);
  }
}

static void ship_start_processing_animation() {
  if (!ship_processing_halo || !ship_processing_spinner || !ship_processing_label || !ship_processing_subtitle) {
    return;
  }
  bool is_dish = ship_processing_is_dish_mode();

  lv_anim_del(ship_processing_halo, NULL);
  lv_anim_del(ship_processing_label, NULL);
  lv_anim_del(ship_processing_subtitle, NULL);

  lv_obj_set_style_opa(ship_processing_halo, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_processing_halo, is_dish ? -38 : -44);
  lv_obj_set_style_opa(ship_processing_spinner, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_y(ship_processing_spinner, -54);
  lv_obj_set_style_bg_opa(ship_processing_halo, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_processing_label, is_dish ? -36 : 60);
  lv_obj_set_style_text_opa(ship_processing_label, LV_OPA_0, LV_PART_MAIN);
  lv_obj_set_y(ship_processing_subtitle, is_dish ? 70 : 94);
  lv_obj_set_style_text_opa(ship_processing_subtitle, LV_OPA_0, LV_PART_MAIN);

  lv_anim_t halo_in_opa;
  lv_anim_init(&halo_in_opa);
  lv_anim_set_var(&halo_in_opa, ship_processing_halo);
  lv_anim_set_values(&halo_in_opa, LV_OPA_0, LV_OPA_50);
  lv_anim_set_time(&halo_in_opa, 360);
  lv_anim_set_path_cb(&halo_in_opa, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&halo_in_opa, ship_anim_set_obj_opa);
  lv_anim_start(&halo_in_opa);

  lv_anim_t halo_in_y;
  lv_anim_init(&halo_in_y);
  lv_anim_set_var(&halo_in_y, ship_processing_halo);
  lv_anim_set_values(&halo_in_y, is_dish ? -38 : -44, is_dish ? -48 : -54);
  lv_anim_set_time(&halo_in_y, 360);
  lv_anim_set_path_cb(&halo_in_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&halo_in_y, ship_anim_set_y);
  lv_anim_start(&halo_in_y);

  lv_anim_t label_fade;
  lv_anim_init(&label_fade);
  lv_anim_set_var(&label_fade, ship_processing_label);
  lv_anim_set_values(&label_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&label_fade, 220);
  lv_anim_set_delay(&label_fade, 50);
  lv_anim_set_exec_cb(&label_fade, ship_anim_set_text_opa);
  lv_anim_start(&label_fade);

  lv_anim_t label_y;
  lv_anim_init(&label_y);
  lv_anim_set_var(&label_y, ship_processing_label);
  lv_anim_set_values(&label_y, is_dish ? -36 : 60, is_dish ? -48 : 44);
  lv_anim_set_time(&label_y, 280);
  lv_anim_set_delay(&label_y, 50);
  lv_anim_set_path_cb(&label_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&label_y, ship_anim_set_y);
  lv_anim_start(&label_y);

  lv_anim_t subtitle_fade;
  lv_anim_init(&subtitle_fade);
  lv_anim_set_var(&subtitle_fade, ship_processing_subtitle);
  lv_anim_set_values(&subtitle_fade, LV_OPA_0, LV_OPA_COVER);
  lv_anim_set_time(&subtitle_fade, 220);
  lv_anim_set_delay(&subtitle_fade, 110);
  lv_anim_set_exec_cb(&subtitle_fade, ship_anim_set_text_opa);
  lv_anim_start(&subtitle_fade);

  lv_anim_t subtitle_y;
  lv_anim_init(&subtitle_y);
  lv_anim_set_var(&subtitle_y, ship_processing_subtitle);
  lv_anim_set_values(&subtitle_y, is_dish ? 70 : 94, is_dish ? 58 : 82);
  lv_anim_set_time(&subtitle_y, 280);
  lv_anim_set_delay(&subtitle_y, 110);
  lv_anim_set_path_cb(&subtitle_y, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&subtitle_y, ship_anim_set_y);
  lv_anim_start(&subtitle_y);
}

static void ship_init_logged() {
  // LCD_Minimal/LCD_Minimal.ino: ship_init_logged
  if (ship_logged_screen) {
    return;
  }
  ship_logged_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_logged_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_logged_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_LOGGED", "plain_screen");
  ship_style_plain_screen(ship_logged_screen, lv_color_hex(0x1F4D2B));
  lv_obj_set_style_border_width(ship_logged_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_logged_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_logged_screen, 0, LV_PART_MAIN);

  ship_logged_icon = lv_label_create(ship_logged_screen);
  lv_label_set_text(ship_logged_icon, LV_SYMBOL_OK);
  lv_obj_set_style_text_font(ship_logged_icon, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_logged_icon, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_logged_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_logged_icon, LV_ALIGN_CENTER, 0, -42);

  ship_logged_label = lv_label_create(ship_logged_screen);
  lv_label_set_text(ship_logged_label, "Logged");
  lv_obj_set_style_text_font(ship_logged_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_logged_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_logged_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_logged_label, LV_ALIGN_CENTER, 0, 8);

  ship_logged_subtitle = lv_label_create(ship_logged_screen);
  lv_label_set_text(ship_logged_subtitle, "Saved successfully");
  lv_obj_set_style_text_font(ship_logged_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_logged_subtitle, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_logged_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_logged_subtitle, LV_ALIGN_CENTER, 0, 52);
}

static void ship_init_voice_ack() {
  if (ship_voice_ack_screen) {
    return;
  }
  ship_voice_ack_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_voice_ack_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_voice_ack_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_VOICE_ACK", "plain_screen");
  ship_style_plain_screen(ship_voice_ack_screen, lv_color_hex(0x1F4D2B));
  lv_obj_set_style_border_width(ship_voice_ack_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_outline_width(ship_voice_ack_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(ship_voice_ack_screen, 0, LV_PART_MAIN);

  ship_voice_ack_icon = lv_label_create(ship_voice_ack_screen);
  lv_label_set_text(ship_voice_ack_icon, LV_SYMBOL_OK);
  lv_obj_set_style_text_font(ship_voice_ack_icon, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_ack_icon, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_ack_icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_voice_ack_icon, LV_ALIGN_CENTER, 0, -42);

  ship_voice_ack_label = lv_label_create(ship_voice_ack_screen);
  lv_label_set_text(ship_voice_ack_label, "On it!");
  lv_obj_set_style_text_font(ship_voice_ack_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_ack_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_ack_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_voice_ack_label, LV_ALIGN_CENTER, 0, 8);

  ship_voice_ack_subtitle = lv_label_create(ship_voice_ack_screen);
  lv_label_set_text(ship_voice_ack_subtitle, "Processing your request");
  lv_obj_set_style_text_font(ship_voice_ack_subtitle, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_voice_ack_subtitle, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_voice_ack_subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ship_voice_ack_subtitle, LV_ALIGN_CENTER, 0, 52);
}

static void ship_init_error() {
  if (ship_error_screen) {
    return;
  }
  ship_error_screen = lv_obj_create(NULL);
  lv_obj_set_size(ship_error_screen, LV_PCT(100), LV_PCT(100));
  lv_obj_clear_flag(ship_error_screen, LV_OBJ_FLAG_SCROLLABLE);
  ui_log_asset("init", "SHIP_ERROR", "plain_screen");
  ship_style_plain_screen(ship_error_screen, lv_color_hex(0xE53935));

  ship_error_label = lv_label_create(ship_error_screen);
  lv_label_set_text(ship_error_label, "Try Again");
  lv_obj_set_style_text_font(ship_error_label, &lv_font_montserrat_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(ship_error_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_align(ship_error_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(ship_error_label);
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
  if (!dish_processing_active || ui_screen_state != SCREEN_PROCESSING || dish_processing_start_ms == 0) {
    return;
  }
  if (!ship_processing_is_dish_mode()) {
    return;
  }

  unsigned long age_ms = millis() - dish_processing_start_ms;
  uint8_t pct = ship_compute_dish_processing_pct(age_ms);
  if (pct == dish_processing_progress_pct && pct != 100) {
    return;
  }
  dish_processing_progress_pct = pct;
  ship_set_processing_fill_pct(pct);
  if (ship_processing_label) {
    char pct_text[8];
    snprintf(pct_text, sizeof(pct_text), "%u%%", (unsigned)pct);
    lv_label_set_text(ship_processing_label, pct_text);
  }
  if (ship_processing_subtitle) {
    lv_label_set_text(ship_processing_subtitle, "Analyzing your meal");
  }
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
  if (is_dish) {
    dish_processing_progress_pct = 1;
    if (ship_processing_label) {
      lv_label_set_text(ship_processing_label, "1%");
    }
    if (ship_processing_subtitle) {
      lv_label_set_text(ship_processing_subtitle, "Analyzing your meal");
    }
  } else {
    ship_set_processing_text(NULL);
    if (ship_processing_subtitle) {
      lv_label_set_text(ship_processing_subtitle, "Working on your request");
    }
  }
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
  ship_expiry_choice_shown_time = millis();
  ship_update_expiry_choice_timeout_ring();
  if (!ship_choice_mode_is_discard()) {
    expiry_choice_quantity = 1;
    expiry_choice_update_quantity_label();
  }
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
  ship_expiry_choice_shown_time = 0;
  expiry_prepare_picker_for_entry();
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

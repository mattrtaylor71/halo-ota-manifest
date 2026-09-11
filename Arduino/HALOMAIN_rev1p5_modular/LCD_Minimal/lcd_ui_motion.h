#pragma once

// UI-thread-only presentation motion. No operation dispatch, timers, heap-sized
// layers, transform_zoom, screen fades or changes to capture/ack deadlines.
static void halo_ui_motion_stop(lv_obj_t* root) {
  if (!root)
    return;
  lv_anim_del(root, NULL);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(root); ++i)
    halo_ui_motion_stop(lv_obj_get_child(root, i));
}

static void halo_ui_seal_pop(void* p, int32_t v) {
  lv_obj_t* seal = (lv_obj_t*)p;
  // 500ms spring: 0.30 -> 1.06 at 65% -> 1.00. Resize the small
  // circle and vector check directly; no raster scaling/layer allocation.
  int size = v < 650 ? 25 + (64 * v) / 650 : 89 - (5 * (v - 650)) / 350;
  lv_obj_set_size(seal, size, size);
  lv_obj_set_pos(seal, 180 - size / 2, 100 - size / 2);
  lv_obj_t* check = lv_obj_get_child(seal, 0);
  if (check) {
    int n = size * 58 / 100;
    lv_obj_set_size(check, n, n);
    lv_obj_center(check);
    lv_obj_set_style_text_opa(check, (lv_opa_t)(v < 200 ? v * 255 / 200 : 255), 0);
  }
}
static lv_obj_t* halo_ui_seal(lv_obj_t* page, uint32_t color = COL_GREEN,
                              HaloUiIcon icon = HALO_ICON_CHECK) {
  lv_obj_t* seal = halo_ui_card(page, 138, 58, 84, 84, color, LV_RADIUS_CIRCLE);
  lv_obj_set_style_shadow_ofs_x(seal, 4, 0);
  lv_obj_set_style_shadow_ofs_y(seal, 4, 0);
  halo_ui_icon(seal, icon, 15, 15, 50, COL_WHITE);
  return seal;
}
static void halo_ui_reward_start(lv_obj_t* seal) {
  if (!seal)
    return;
  lv_anim_del(seal, NULL);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, seal);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, 500);
  lv_anim_set_exec_cb(&a, halo_ui_seal_pop);
  lv_anim_start(&a);
}

static void halo_ui_voice_ack_pop(void* p, int32_t v) {
  lv_obj_t* seal = (lv_obj_t*)p;
  // Keep the existing seal, with a shorter 70 -> 89 -> 84px pop. The check
  // stays at least 40px wide, avoiding its renderer's 2px -> 4px stroke jump.
  const int size = v < 650 ? 70 + (19 * v) / 650 : 89 - (5 * (v - 650)) / 350;
  if (lv_obj_get_width(seal) != size || lv_obj_get_height(seal) != size)
    lv_obj_set_size(seal, size, size);
  const int x = 180 - size / 2;
  const int y = 100 - size / 2;
  if (lv_obj_get_x(seal) != x || lv_obj_get_y(seal) != y)
    lv_obj_set_pos(seal, x, y);
  lv_obj_t* check = lv_obj_get_child(seal, 0);
  if (check) {
    const int n = size * 58 / 100;
    if (lv_obj_get_width(check) != n || lv_obj_get_height(check) != n)
      lv_obj_set_size(check, n, n);
    const lv_opa_t opacity = (lv_opa_t)(v < 200 ? v * 255 / 200 : 255);
    if (lv_obj_get_style_text_opa(check, 0) != opacity)
      lv_obj_set_style_text_opa(check, opacity, 0);
  }
}

static void halo_ui_voice_ack_start(lv_obj_t* seal) {
  if (!seal)
    return;
  lv_anim_del(seal, NULL);
  lv_obj_t* check = lv_obj_get_child(seal, 0);
  if (check)
    lv_obj_center(check); // Persistent alignment; no per-frame layout reset.
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, seal);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, 260);
  lv_anim_set_exec_cb(&a, halo_ui_voice_ack_pop);
  lv_anim_start(&a);
}

static void halo_ui_digit_settle(void* p, int32_t v) {
  // A gentle 8px settle of the actual numeral. Ring time never depends on it.
  lv_obj_set_y((lv_obj_t*)p, 156 + v);
}
static void halo_ui_digit_pulse(lv_obj_t* digit) {
  lv_anim_del(digit, NULL);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, digit);
  lv_anim_set_values(&a, -8, 0);
  lv_anim_set_time(&a, 220);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, halo_ui_digit_settle);
  lv_anim_start(&a);
}

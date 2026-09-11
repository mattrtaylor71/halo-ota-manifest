#pragma once
#include <math.h>

// September 2026 designer presentation. All geometry is logical 360 x 360.
// These helpers allocate ordinary small LVGL objects, never a canvas/layer.
static lv_obj_t* halo_ui_label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                               uint32_t color, int x, int y, int w) {
  lv_obj_t* o = lv_label_create(parent);
  lv_obj_set_style_text_font(o, font, 0);
  lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(o, w);
  lv_obj_set_pos(o, x, y);
  lv_label_set_text(o, text);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* halo_ui_card(lv_obj_t* parent, int x, int y, int w, int h,
                              uint32_t fill = COL_WHITE, int radius = 18) {
  lv_obj_t* o = lv_obj_create(parent);
  trepo_card(o, fill, radius);
  lv_obj_set_style_shadow_ofs_x(o, 3, 0);
  lv_obj_set_style_shadow_ofs_y(o, 3, 0);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  quiet_clickable(o);
  return o;
}

static lv_obj_t* halo_ui_button(lv_obj_t* parent, int x, int y, int w, int h, const char* text,
                                uint32_t fill = COL_GREEN, uint32_t ink = COL_WHITE,
                                bool ghost = false) {
  lv_obj_t* o = halo_ui_card(parent, x, y, w, h, fill, 16);
  if (ghost) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
  }
  lv_obj_t* l = halo_ui_label(o, text, &nunito_16, ink, 0, 0, w - 8);
  lv_obj_center(l);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  return o;
}

static lv_obj_t* halo_ui_page(uint32_t color = COL_CREAM) {
  lv_obj_t* o = lv_obj_create(NULL);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 360, 360);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* halo_ui_ring(lv_obj_t* parent, int x, int y, int size, int stroke,
                              uint32_t color) {
  lv_obj_t* o = lv_arc_create(parent);
  lv_obj_remove_style(o, NULL, LV_PART_KNOB);
  lv_obj_set_size(o, size, size);
  lv_obj_set_pos(o, x, y);
  lv_arc_set_range(o, 0, 1000);
  lv_arc_set_value(o, 1000);
  lv_arc_set_bg_angles(o, 0, 360);
  lv_arc_set_rotation(o, 270);
  lv_obj_set_style_arc_width(o, stroke, LV_PART_MAIN);
  lv_obj_set_style_arc_width(o, stroke, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(o, lv_color_hex(0xE6D7BF), LV_PART_MAIN);
  lv_obj_set_style_arc_color(o, lv_color_hex(color), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(o, true, LV_PART_INDICATOR);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* halo_ui_badge(lv_obj_t* parent, const char* text, int y, int w = 120,
                               uint32_t fill = COL_GOLD) {
  lv_obj_t* o = halo_ui_card(parent, (360 - w) / 2, y, w, 25, fill, 12);
  lv_obj_set_style_shadow_ofs_x(o, 2, 0);
  lv_obj_set_style_shadow_ofs_y(o, 2, 0);
  lv_obj_t* l =
      halo_ui_label(o, text, &nunito_12, fill == COL_TEAL ? COL_WHITE : COL_DARK, 0, 0, w - 8);
  lv_obj_center(l);
  return o;
}

// Vector icons are drawn directly into LVGL's existing draw buffer. One small
// object per icon, no bitmap, font substitution, per-frame allocation or zoom.
enum HaloUiIcon {
  HALO_ICON_PLUS,
  HALO_ICON_PLATE,
  HALO_ICON_TRASH,
  HALO_ICON_LIST,
  HALO_ICON_SETTINGS,
  HALO_ICON_BACK,
  HALO_ICON_MIC,
  HALO_ICON_SUN,
  HALO_ICON_DOWNLOAD,
  HALO_ICON_WIFI,
  HALO_ICON_CAMERA,
  HALO_ICON_CHECK,
  HALO_ICON_CHEVRON,
  HALO_ICON_WARNING,
  HALO_ICON_REFRESH
};

static void halo_ui_icon_draw(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target(e);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const int n = lv_obj_get_width(o);
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = lv_obj_get_style_text_color(o, 0);
  line.opa = lv_obj_get_style_text_opa(o, 0);
  line.width = n >= 40 ? 4 : 2;
  line.round_start = line.round_end = 1;
  auto seg = [&](int x1, int y1, int x2, int y2) {
    lv_point_t p = {(lv_coord_t)(a.x1 + x1 * n / 24), (lv_coord_t)(a.y1 + y1 * n / 24)};
    lv_point_t q = {(lv_coord_t)(a.x1 + x2 * n / 24), (lv_coord_t)(a.y1 + y2 * n / 24)};
    lv_draw_line(ctx, &line, &p, &q);
  };
  auto box = [&](int x, int y, int w, int h, int r) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = line.color;
    d.border_opa = line.opa;
    d.border_width = line.width;
    d.radius = r * n / 24;
    lv_area_t b = {(lv_coord_t)(a.x1 + x * n / 24), (lv_coord_t)(a.y1 + y * n / 24),
                   (lv_coord_t)(a.x1 + (x + w) * n / 24), (lv_coord_t)(a.y1 + (y + h) * n / 24)};
    lv_draw_rect(ctx, &d, &b);
  };
  auto arc = [&](int x, int y, int r, int start, int end) {
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = line.color;
    d.opa = line.opa;
    d.width = line.width;
    d.rounded = 1;
    lv_point_t p = {(lv_coord_t)(a.x1 + x * n / 24), (lv_coord_t)(a.y1 + y * n / 24)};
    lv_draw_arc(ctx, &d, &p, r * n / 24, start, end);
  };
  switch ((HaloUiIcon)(uintptr_t)lv_event_get_user_data(e)) {
  case HALO_ICON_PLUS:
    seg(12, 4, 12, 20);
    seg(4, 12, 20, 12);
    break;
  case HALO_ICON_PLATE:
    arc(13, 12, 8, 0, 360);
    arc(13, 12, 5, 0, 360);
    seg(2, 3, 2, 21);
    break;
  case HALO_ICON_TRASH:
    seg(3, 6, 21, 6);
    box(6, 6, 12, 15, 2);
    seg(9, 3, 15, 3);
    seg(10, 10, 10, 17);
    seg(14, 10, 14, 17);
    break;
  case HALO_ICON_LIST:
    box(4, 3, 16, 18, 2);
    for (int y = 7; y <= 17; y += 5) {
      seg(7, y, 8, y);
      seg(11, y, 17, y);
    }
    break;
  case HALO_ICON_SETTINGS:
    arc(12, 12, 7, 0, 360);
    arc(12, 12, 3, 0, 360);
    for (int k = 0; k < 8; k++) {
      float t = k * 0.785398f;
      seg(12 + cosf(t) * 8, 12 + sinf(t) * 8, 12 + cosf(t) * 10, 12 + sinf(t) * 10);
    }
    break;
  case HALO_ICON_BACK:
    seg(15, 5, 8, 12);
    seg(8, 12, 15, 19);
    break;
  case HALO_ICON_CHEVRON:
    seg(9, 5, 16, 12);
    seg(16, 12, 9, 19);
    break;
  case HALO_ICON_MIC:
    box(9, 2, 6, 12, 3);
    arc(12, 11, 7, 0, 180);
    seg(12, 18, 12, 22);
    seg(8, 22, 16, 22);
    break;
  case HALO_ICON_SUN:
    arc(12, 12, 4, 0, 360);
    for (int k = 0; k < 8; k++) {
      float t = k * 0.785398f;
      seg(12 + cosf(t) * 7, 12 + sinf(t) * 7, 12 + cosf(t) * 10, 12 + sinf(t) * 10);
    }
    break;
  case HALO_ICON_DOWNLOAD:
    seg(12, 3, 12, 15);
    seg(7, 10, 12, 15);
    seg(12, 15, 17, 10);
    seg(4, 16, 4, 21);
    seg(4, 21, 20, 21);
    seg(20, 21, 20, 16);
    break;
  case HALO_ICON_WIFI:
    arc(12, 21, 17, 225, 315);
    arc(12, 21, 11, 225, 315);
    arc(12, 21, 5, 225, 315);
    seg(12, 21, 12, 21);
    break;
  case HALO_ICON_CAMERA:
    box(2, 6, 20, 15, 3);
    box(8, 3, 8, 3, 1);
    arc(12, 13, 4, 0, 360);
    break;
  case HALO_ICON_CHECK:
    seg(5, 12, 10, 17);
    seg(10, 17, 20, 6);
    break;
  case HALO_ICON_WARNING:
    seg(12, 6, 12, 14);
    seg(12, 18, 12, 18);
    break;
  case HALO_ICON_REFRESH:
    arc(12, 12, 8, 45, 310);
    seg(18, 3, 19, 9);
    seg(19, 9, 13, 8);
    break;
  }
}

static lv_obj_t* halo_ui_icon(lv_obj_t* parent, HaloUiIcon icon, int x, int y, int size,
                              uint32_t color = COL_DARK) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, size, size);
  lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_text_opa(o, LV_OPA_COVER, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(o, halo_ui_icon_draw, LV_EVENT_DRAW_MAIN, (void*)(uintptr_t)icon);
  return o;
}

static lv_obj_t* halo_ui_back(lv_obj_t* parent, int x = 156, int y = 291) {
  lv_obj_t* o = halo_ui_card(parent, x, y, 48, 48, COL_WHITE, 24);
  halo_ui_icon(o, HALO_ICON_BACK, 11, 11, 22);
  return o;
}

static void halo_ui_spin(void* p, int32_t rotation) {
  lv_arc_set_rotation((lv_obj_t*)p, rotation);
}
static void halo_ui_spinner_start(lv_obj_t* o) {
  if (!o)
    return;
  lv_anim_del(o, NULL);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, 0, 360);
  lv_anim_set_time(&a, 1100);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&a, halo_ui_spin);
  lv_anim_start(&a);
}
static lv_obj_t* halo_ui_spinner(lv_obj_t* parent, int y, uint32_t color = COL_TEAL) {
  lv_obj_t* o = halo_ui_ring(parent, 138, y, 84, 7, color);
  lv_arc_set_value(o, 300);
  halo_ui_spinner_start(o);
  return o;
}

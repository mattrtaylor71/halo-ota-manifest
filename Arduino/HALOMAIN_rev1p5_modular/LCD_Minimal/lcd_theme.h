// lcd_theme.h — Trepo design system for the HALO round display.
//
// Palette inherited from Trepo. Current screen composition follows the accepted
// September 7 designer HTML; Shopping_Demo is historical. Full rationale, screen
// inventory and trap list live in docs/HALO_UI_DESIGN.md.
//
// Target: 360x360 round SH8601, LVGL 8.3, LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 1.
// No GPU, ~48KB LVGL heap (LV_MEM_SIZE in Arduino/libraries/lv_conf.h — shared by
// every sketch on this machine, so it is NOT ours to raise unilaterally).
//
// Use the primitives rather than restyling by hand. Hand-restyling is how the 43
// distinct colours and 798 inline style calls in the pre-redesign UI happened.

#pragma once

#include <lvgl.h>

// ─────────────────────────── 1. Palette ───────────────────────────
// Hero colours ONLY. Never introduce a colour outside this table.
// The brand accent is green-blue #296065 — the royal blue #2D4FFF is RETIRED.
#define COL_TEAL   0x296065   // brand accent (thymeBlue): headers, secondary actions
#define COL_GREEN  0x1F4D2B   // positive/primary: Buy Now, Add, Check In
#define COL_RED    0xE53935   // in-progress, alerts, errors, the "loud" payoff frame
#define COL_GOLD   0xFFD54F   // totals, badges, attention. Never a large ground
#define COL_CREAM  0xF5E9D8   // EVERY page ground
#define COL_WHITE  0xFFFFFF   // card surfaces ONLY — never a page ground
#define COL_DARK   0x1A1A1A   // ink: text, borders, shadows
#define COL_TERT   0xEDE0D0   // inset areas, progress-bar tracks
#define COL_TEXT2  0x4A4A4A   // secondary text
#define COL_MUTED  0x8A7E72   // hints, eyebrow labels

// ─────────────────────────── 2. Type ───────────────────────────
// Nunito Black (wght=900), --bpp 4 --no-compress. ~19.3KB of flash for all six.
LV_FONT_DECLARE(nunito_12);
LV_FONT_DECLARE(nunito_16);
LV_FONT_DECLARE(nunito_18);
LV_FONT_DECLARE(nunito_22);
LV_FONT_DECLARE(nunito_24);
LV_FONT_DECLARE(nunito_28);
LV_FONT_DECLARE(nunito_32);
LV_FONT_DECLARE(nunito_44);
LV_FONT_DECLARE(nunito_60);

// Real line_height values read out of the generated .c files. Use THESE for any
// vertical arithmetic — browser/mock metrics disagree and that mismatch is what
// produced the original 5-20px overlaps.
#define LH_NUNITO_12  13
#define LH_NUNITO_16  18
#define LH_NUNITO_18 19
#define LH_NUNITO_22  24
#define LH_NUNITO_24 25
#define LH_NUNITO_28 30
#define LH_NUNITO_32  31
#define LH_NUNITO_44  42
#define LH_NUNITO_60  44

// Coverage, so you don't reach for a glyph a cut doesn't carry:
//   12/16/18/22/24/28 → full ASCII (0x20-0x7E)
//   32/44    → digits plus $ % .
//   60       → digits only (0x30-0x39)
// The Nunito cuts are ASCII-only: LV_SYMBOL_* are FontAwesome codepoints and
// render as TOFU in them. Icon glyphs must stay on lv_font_montserrat_20/34/48.
// Montserrat itself covers ASCII + degree + bullet only — middle dots and
// en/em dashes are tofu, so substitute bullet and plain hyphens.

// ─────────────────────── 3. Round-display geometry ───────────────────────
// The panel is a circle of radius 180 centred at (180,180). The real constraint
// is the chord at an element's OWN height, not the panel width — something that
// fits at the centre will clip near the top or bottom.
//
// Full-bleed bands are exempt: a header spanning the whole panel is MEANT to be
// clipped by the bezel and reads as an arc.
//
// Do NOT shape-follow the bezel. A barrel-shaped keyboard where every row hugged
// the circle was geometrically optimal and was rejected outright. Use simple
// centred rectangles sized to the chord at their position.
#define HALO_R       180
#define HALO_CX      180
#define HALO_CY      180

// Max width for an element spanning y1..y2, already minus the 4px shadow that
// extends right and down. Returns a full width (not a half-width).
static inline lv_coord_t halo_chord_max_w(lv_coord_t y1, lv_coord_t y2) {
  int32_t d1 = (int32_t)y1 - HALO_CY; if (d1 < 0) d1 = -d1;
  int32_t d2 = (int32_t)y2 - HALO_CY; if (d2 < 0) d2 = -d2;
  int32_t dy = (d1 > d2) ? d1 : d2;
  int32_t rr = (int32_t)HALO_R * HALO_R - dy * dy;
  if (rr <= 0) return 0;
  // integer sqrt
  int32_t h = 0, bit = 1 << 15;
  while (bit > rr) bit >>= 2;
  int32_t n = rr;
  while (bit) {
    if (n >= h + bit) { n -= h + bit; h = (h >> 1) + bit; }
    else              { h >>= 1; }
    bit >>= 2;
  }
  int32_t w = 2 * h - 4;              // shadow allowance
  return (lv_coord_t)(w > 0 ? w : 0);
}

// ─────────────────────────── 4. Primitives ───────────────────────────

// Make a touch target visually inert: no focus state, no theme outline flash.
// The default theme flashes a focus outline on click; this kills it.
static inline void quiet_clickable(lv_obj_t* obj) {
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_set_style_outline_width(obj, 0, 0);
  lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_width(obj, 0, LV_STATE_PRESSED);
}

// The signature treatment: 4px right/down, ZERO blur, ink at 85%.
// LVGL 8.3 skips drawing a shadow ENTIRELY when shadow_width == 0
// (lv_draw_sw_rect.c), so 1 is the smallest width it will actually render — a
// 1px feather that reads as hard at this density. Never pass 0.
static inline void trepo_shadow(lv_obj_t* o) {
  lv_obj_set_style_shadow_width(o, 1, 0);
  lv_obj_set_style_shadow_spread(o, 0, 0);
  lv_obj_set_style_shadow_ofs_x(o, 4, 0);
  lv_obj_set_style_shadow_ofs_y(o, 4, 0);
  lv_obj_set_style_shadow_color(o, lv_color_hex(COL_DARK), 0);
  lv_obj_set_style_shadow_opa(o, 217, 0);   // 85%
}

// Card / button body: solid fill, 18px radius, 2px ink border, hard shadow.
// radius < 0 keeps whatever radius the caller already set (circles, pills).
//
// Note: lv_obj_align on a CHILD is relative to the parent's *content* area,
// which LVGL insets by border_width — even when border_side is BOTTOM only. The
// 2px border here pushes every child down 2px. Account for it.
static inline void trepo_card(lv_obj_t* o, uint32_t fill, int radius) {
  lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  if (radius >= 0) lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(COL_DARK), 0);
  lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  trepo_shadow(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

// Uppercase tracked label. LVGL has no text-transform, so callers pass strings
// ALREADY uppercased.
static inline lv_obj_t* trepo_caps(lv_obj_t* parent, const char* txt, const lv_font_t* f,
                                   uint32_t color, lv_coord_t track) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_letter_space(l, track, 0);
  lv_label_set_text(l, txt);
  return l;
}

// Full-width primary button: fill + ink border + hard shadow + caps label.
static inline lv_obj_t* trepo_button(lv_obj_t* parent, lv_coord_t w, lv_coord_t h, lv_coord_t y,
                                     uint32_t fill, const char* txt, uint32_t txt_col,
                                     lv_event_cb_t cb) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
  trepo_card(b, fill, 18);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  quiet_clickable(b);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = trepo_caps(b, txt, &nunito_16, txt_col, 1);
  lv_obj_center(l);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  return b;
}

// General button. subtle = the cancel style: no fill, no border, no shadow.
static inline lv_obj_t* make_button(lv_obj_t* parent, lv_coord_t w, lv_coord_t h, lv_coord_t y,
                                    uint32_t bg, const char* text, const lv_font_t* font,
                                    uint32_t txt, bool subtle, lv_event_cb_t cb) {
  lv_obj_t* b = lv_obj_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
  if (subtle) {
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  } else {
    trepo_card(b, bg, 18);
  }
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  quiet_clickable(b);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* l = trepo_caps(b, text, font, txt, 1);
  lv_obj_center(l);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  return b;
}

// There is NO microphone symbol in LVGL — LV_SYMBOL_AUDIO is a music note. Draw
// it from three rounded rects instead.
static inline void mic_glyph(lv_obj_t* parent, uint32_t color) {
  struct { lv_coord_t w, h, y, r; } part[3] = {
    {15, 23, -8, 7},   // capsule
    { 4,  7, 10, 2},   // stem
    {21,  4, 16, 2},   // base
  };
  for (int i = 0; i < 3; i++) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, part[i].w, part[i].h);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, part[i].y);
    lv_obj_set_style_radius(o, part[i].r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
  }
}

// ──────────────────── 5. Layout audit (dev tool, compile-gated) ────────────────────
// Verify on the panel, never from a mock. Set LAYOUT_AUDIT 1, flash, capture
// serial, then: python3 tools/layout_audit.py dump.log
// Rebuild with LAYOUT_AUDIT 0 before shipping. Run it after ANY layout change.
#ifndef LAYOUT_AUDIT
#define LAYOUT_AUDIT 0
#endif

#if LAYOUT_AUDIT
static void audit_dump_tree(lv_obj_t* o, const char* screen, int depth) {
  uint32_t n = lv_obj_get_child_cnt(o);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t* c = lv_obj_get_child(o, i);
    lv_area_t a;
    lv_obj_get_coords(c, &a);
    const char* txt = "";
    char kind = 'O';
    if (lv_obj_check_type(c, &lv_label_class)) {
      kind = 'L';
      txt = lv_label_get_text(c);
    } else if (lv_obj_check_type(c, &lv_img_class)) {
      kind = 'I';
      // An lv_img keeps its SOURCE size as its object box but draws zoomed about
      // its centre, so the box is not what you see. Report the visible extent.
      uint16_t z = lv_img_get_zoom(c);
      lv_coord_t w = a.x2 - a.x1 + 1, h = a.y2 - a.y1 + 1;
      lv_coord_t vw = (lv_coord_t)(((int32_t)w * z) / 256);
      lv_coord_t vh = (lv_coord_t)(((int32_t)h * z) / 256);
      lv_coord_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
      a.x1 = cx - vw / 2; a.x2 = a.x1 + vw - 1;
      a.y1 = cy - vh / 2; a.y2 = a.y1 + vh - 1;
    }
    Serial.printf("[LAY] %s|%c|%d|%d|%d|%d|%d|%d|%d|%d|%s\n",
                  screen, kind, depth, (int)a.x1, (int)a.y1,
                  (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1),
                  lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) ? 1 : 0,
                  (int)lv_obj_get_style_bg_opa(c, LV_PART_MAIN),
                  (int)lv_obj_get_style_text_opa(c, LV_PART_MAIN),
                  txt);
    audit_dump_tree(c, screen, depth + 1);
  }
}
static const char* s_audit_name = "?";
static void audit_dump_cb(lv_timer_t* t) {
  (void)t;
  lv_obj_t* scr = lv_scr_act();
  lv_obj_update_layout(scr);
  Serial.printf("[LAYSTART] %s\n", s_audit_name);
  audit_dump_tree(scr, s_audit_name, 0);
  Serial.printf("[LAYEND] %s\n", s_audit_name);
}
// Dump 1500ms after a screen appears so entrance anims have fully settled.
// lv_timer_create DEFAULTS TO REPEATING — the repeat_count(1) is load-bearing.
// A fired one-shot auto-deletes; never lv_timer_del it afterwards.
static inline void audit_capture(const char* name) {
  s_audit_name = name;
  lv_timer_t* t = lv_timer_create(audit_dump_cb, 1500, NULL);
  lv_timer_set_repeat_count(t, 1);
}
#else
#define audit_capture(name) ((void)0)
#endif

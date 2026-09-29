#pragma once

#include "../halo_ota_demo/firmware/shared/HomeBattery.h"

// The ADC belongs to loop(). Home only borrows its synchronized cached view.
static halo_power::View lcd_power_view();

static bool home_battery_usb_attached() {
#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE == 1 && ARDUINO_USB_CDC_ON_BOOT
  // SOF presence is contradictory evidence for a low-rail battery estimate;
  // it does not establish a 5 V supply or that the cell is charging.
  return HWCDC::isPlugged();
#else
  return false;
#endif
}

static home_battery::Model s_home_power_model;
static home_battery::Display s_home_power_display = {
    home_battery::Mode::Unknown, home_battery::Band::Neutral, 0};
static uint32_t s_home_power_last_check_ms = 0;
static bool s_home_power_checked = false;

// Reference-directed footer teal; existing menu icon colors remain untouched.
static constexpr uint32_t HOME_POWER_TEAL = 0x2E8C8E;
static constexpr uint32_t HOME_POWER_BOLT_GREEN = 0x2EA86A;

static uint32_t lcd_home_power_color(home_battery::Band band) {
  switch (band) {
    case home_battery::Band::Green: return HOME_POWER_TEAL;
    case home_battery::Band::Yellow: return SHIP_HOME_SYMBOL_GOLD;
    case home_battery::Band::Red: return COL_RED;
    default: return COL_MUTED;
  }
}

// A crisp bright-green lightning silhouette with a restrained fixed glow. Two overlapping convex triangles avoid LVGL's unsupported
// concave-polygon path. No animation, canvas or extra widget; LVGL may borrow
// its existing scratch buffers to rasterize these bounded primitives.
static void lcd_home_power_bolt(lv_draw_ctx_t* ctx, const lv_area_t& screen) {
  const lv_point_t outline[] = {
      {lv_coord_t(screen.x1 + 185), lv_coord_t(screen.y1 + 322)},
      {lv_coord_t(screen.x1 + 171), lv_coord_t(screen.y1 + 338)},
      {lv_coord_t(screen.x1 + 181), lv_coord_t(screen.y1 + 338)},
      {lv_coord_t(screen.x1 + 175), lv_coord_t(screen.y1 + 348)},
      {lv_coord_t(screen.x1 + 189), lv_coord_t(screen.y1 + 331)},
      {lv_coord_t(screen.x1 + 180), lv_coord_t(screen.y1 + 331)}};
  const uint8_t widths[] = {3, 1};
  const lv_opa_t opacities[] = {18, 30};
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = lv_color_hex(HOME_POWER_BOLT_GREEN);
  line.round_start = line.round_end = 1;
  for (unsigned pass = 0; pass < 2; ++pass) {
    line.width = widths[pass];
    line.opa = opacities[pass];
    for (unsigned segment = 0; segment < 6; ++segment)
      lv_draw_line(ctx, &line, &outline[segment], &outline[(segment + 1) % 6]);
  }
  lv_draw_rect_dsc_t fill;
  lv_draw_rect_dsc_init(&fill);
  fill.bg_color = lv_color_hex(HOME_POWER_BOLT_GREEN);
  const lv_point_t upper[] = {outline[0], outline[1], outline[2]};
  const lv_point_t lower[] = {outline[5], outline[3], outline[4]};
  lv_draw_triangle(ctx, &fill, upper);
  lv_draw_triangle(ctx, &fill, lower);
}

// Pure drawing, called under the existing LVGL owner lock. No ADC, clock,
// USB, UART, activity, power or networking operation belongs in this callback.
static void lcd_home_power_draw(lv_event_t* event) {
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(event);
  lv_area_t screen;
  lv_obj_get_coords(lv_event_get_target(event), &screen);
  if (s_home_power_display.mode == home_battery::Mode::ExternalPower) {
    lcd_home_power_bolt(ctx, screen);
    return;
  }

  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.width = 6;
  arc.rounded = 1;
  arc.color = s_home_power_display.mode == home_battery::Mode::Battery ?
      lv_color_mix(lv_color_hex(HOME_POWER_TEAL), lv_color_hex(COL_CREAM), 51) : lv_color_hex(COL_TERT);
  const lv_point_t center = {lv_coord_t(screen.x1 + 180), lv_coord_t(screen.y1 + 180)};
  // A broad rounded lower arc, with 12px of cream inside the round screen edge.
  lv_draw_arc(ctx, &arc, &center, 168, 55, 125);

  char text[8] = "--";
  uint32_t ink = COL_MUTED;
  if (s_home_power_display.mode == home_battery::Mode::Battery) {
    const unsigned percent = s_home_power_display.percent <= 100 ? s_home_power_display.percent : 100;
    arc.color = lv_color_hex(lcd_home_power_color(s_home_power_display.band));
    if (percent) {
      const unsigned start = 125 - (70 * percent + 50) / 100;
      lv_draw_arc(ctx, &arc, &center, 168, start, 125);
    }
    // The system-rail curve is a provisional estimate, not a calibrated SOC.
    snprintf(text, sizeof(text), "%u%%", percent);
    ink = s_home_power_display.band == home_battery::Band::Yellow ?
        SHIP_HOME_SYMBOL_GOLD : lcd_home_power_color(s_home_power_display.band);
  }
  lv_draw_label_dsc_t label;
  lv_draw_label_dsc_init(&label);
  label.font = &nunito_22;
  label.color = lv_color_hex(ink);
  label.align = LV_TEXT_ALIGN_CENTER;
  // The bold percentage sits below More's shadow, with a gap before the arc.
  const lv_area_t label_area = {lv_coord_t(screen.x1 + 144), lv_coord_t(screen.y1 + 317),
                               lv_coord_t(screen.x1 + 215), lv_coord_t(screen.y1 + 340)};
  lv_draw_label(ctx, &label, &label_area, text, nullptr);
}

static void lcd_home_power_register(lv_obj_t* screen) {
  // The event descriptor is owned/freed by Home; no new widget or touch target.
  lv_obj_add_event_cb(screen, lcd_home_power_draw, LV_EVENT_DRAW_MAIN_END, nullptr);
}

static void lcd_home_power_reset() {
  s_home_power_model = home_battery::Model{};
  s_home_power_display = {home_battery::Mode::Unknown, home_battery::Band::Neutral, 0};
  s_home_power_last_check_ms = 0;
  s_home_power_checked = false;
}

// Called only by existing LVGL owners. The cheap cadence check adds no timer;
// neither a changed sample nor a glow may turn on the panel or renew activity.
static void lcd_home_power_service() {
  if (!ship_menu_screen || lv_scr_act() != ship_menu_screen || ui_screen_state != SCREEN_HOME ||
      !g_ui_initialized || !g_panel_enabled || !g_lvgl_running || g_backlight_duty <= 0 ||
      g_idle_screen_dark || g_sleep_transition || g_in_light_sleep ||
      g_lcd_maintenance_headless || g_background_wake_dark || g_ota_screen_active || ota_locked ||
      provision_intro_visible || (provision_screen && !lv_obj_has_flag(provision_screen, LV_OBJ_FLAG_HIDDEN))) {
    s_home_power_checked = false;
    return;
  }
  const uint32_t now = millis();
  if (s_home_power_checked && uint32_t(now - s_home_power_last_check_ms) < 250) return;
  s_home_power_last_check_ms = now;
  s_home_power_checked = true;
  const home_battery::Display next = s_home_power_model.update(lcd_power_view(), home_battery_usb_attached());
  if (next.mode == s_home_power_display.mode && next.band == s_home_power_display.band &&
      next.percent == s_home_power_display.percent) return;
  s_home_power_display = next;
  lv_area_t screen;
  lv_obj_get_coords(ship_menu_screen, &screen);
  const lv_area_t changed = {lv_coord_t(screen.x1 + 81), lv_coord_t(screen.y1 + 311),
                             lv_coord_t(screen.x1 + 279), lv_coord_t(screen.y1 + 351)};
  lv_obj_invalidate_area(ship_menu_screen, &changed);
}

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

static uint32_t lcd_home_power_color(home_battery::Band band) {
  switch (band) {
    case home_battery::Band::Green: return COL_GREEN;
    case home_battery::Band::Yellow: return COL_GOLD;
    case home_battery::Band::Red: return COL_RED;
    default: return COL_MUTED;
  }
}

// Fixed vector glow: three bounded strokes, no animation, canvas or extra
// widget. LVGL may borrow its existing scratch buffers to rasterize primitives.
static void lcd_home_power_bolt(lv_draw_ctx_t* ctx, const lv_area_t& screen) {
  const lv_point_t points[] = {
      {lv_coord_t(screen.x1 + 183), lv_coord_t(screen.y1 + 343)},
      {lv_coord_t(screen.x1 + 176), lv_coord_t(screen.y1 + 350)},
      {lv_coord_t(screen.x1 + 183), lv_coord_t(screen.y1 + 349)},
      {lv_coord_t(screen.x1 + 177), lv_coord_t(screen.y1 + 356)}};
  const uint8_t widths[] = {7, 5, 2};
  const lv_opa_t opacities[] = {28, 68, LV_OPA_COVER};
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = lv_color_hex(0x20DE63);
  line.round_start = line.round_end = 1;
  for (unsigned pass = 0; pass < 3; ++pass) {
    line.width = widths[pass];
    line.opa = opacities[pass];
    for (unsigned segment = 0; segment < 3; ++segment)
      lv_draw_line(ctx, &line, &points[segment], &points[segment + 1]);
  }
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
  arc.color = lv_color_hex(COL_TERT);
  const lv_point_t center = {lv_coord_t(screen.x1 + 180), lv_coord_t(screen.y1 + 180)};
  lv_draw_arc(ctx, &arc, &center, 177, 30, 150);

  char text[8] = "--";
  uint32_t ink = COL_MUTED;
  if (s_home_power_display.mode == home_battery::Mode::Battery) {
    const unsigned percent = s_home_power_display.percent <= 100 ? s_home_power_display.percent : 100;
    arc.color = lv_color_hex(lcd_home_power_color(s_home_power_display.band));
    if (percent) {
      const unsigned start = 150 - (120 * percent + 50) / 100;
      lv_draw_arc(ctx, &arc, &center, 177, start, 150);
    }
    // The system-rail curve is a provisional estimate, not a calibrated SOC.
    snprintf(text, sizeof(text), "~%u%%", percent);
    ink = s_home_power_display.band == home_battery::Band::Yellow ?
        SHIP_HOME_SYMBOL_GOLD : lcd_home_power_color(s_home_power_display.band);
  }
  lv_draw_label_dsc_t label;
  lv_draw_label_dsc_init(&label);
  label.font = &nunito_12;
  label.color = lv_color_hex(ink);
  label.align = LV_TEXT_ALIGN_CENTER;
  // Actual Nunito glyph ink occupies y340..349; More's shadow ends at y339.
  const lv_area_t label_area = {lv_coord_t(screen.x1 + 150), lv_coord_t(screen.y1 + 339),
                               lv_coord_t(screen.x1 + 209), lv_coord_t(screen.y1 + 351)};
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
  const lv_area_t changed = {lv_coord_t(screen.x1 + 20), lv_coord_t(screen.y1 + 260),
                             lv_coord_t(screen.x1 + 339), lv_coord_t(screen.y1 + 359)};
  lv_obj_invalidate_area(ship_menu_screen, &changed);
}

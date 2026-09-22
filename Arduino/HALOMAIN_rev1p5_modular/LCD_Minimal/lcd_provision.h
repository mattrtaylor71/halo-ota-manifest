/* Guided provisioning overlays. All LVGL operations run under the existing UI lock. */
#ifndef LCD_PROVISION_H
#define LCD_PROVISION_H
#include "lcd_provision_flow.h"
#include "lcd_provision_scroll_asset.h"

// The bundled QR implementation shares a guard with the disabled SDK header.
extern "C" lv_obj_t* lv_qrcode_create(lv_obj_t*, lv_coord_t, lv_color_t, lv_color_t);
extern "C" lv_res_t lv_qrcode_update(lv_obj_t*, const void*, uint32_t);
static void show_ship_settings_screen();
static void hide_provision_intro_screen(const char* reason);
static void show_provisioning_screen(const char* ssid, const char* password, const char* url);
static void hide_provisioning_screen();
static void provision_ui_render();
static void provision_ui_reset_start();

static LcdProvisionFlow provision_flow;
static bool provision_ui_deferred = false;
static bool provision_ui_reset_confirmation = false;
static bool provision_ui_reset_confirmation_pending = false;
static bool provision_ui_qr_failed = false;
static lv_obj_t* provision_ui_back_btn = NULL;
static lv_obj_t* provision_ui_retry_btn = NULL;
static lv_obj_t* provision_ui_intro_title = NULL;
static lv_obj_t* provision_ui_intro_copy = NULL;
static lv_obj_t* provision_ui_intro_cancel = NULL;
static lv_obj_t* provision_ui_intro_start = NULL;
// Read only from knob ISR; actual state and rendering belong to the UI owner.
static volatile bool provision_ui_can_scroll = false;
static bool provision_ui_preview_active = false;
static uint32_t provision_ui_preview_at_ms = 0;
static void provision_ui_preview(int step);

static lv_obj_t* provision_ui_label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                                    uint32_t color, int y, int width) {
  lv_obj_t* label = halo_ui_label(parent, text, font, color, (360 - width) / 2, y, width);
  lv_obj_set_style_text_line_space(label, 3, 0);
  return label;
}
static void provision_ui_card(lv_obj_t* object, uint32_t color, int radius) {
  trepo_card(object, color, radius);
  lv_obj_set_style_shadow_ofs_x(object, 3, 0);
  lv_obj_set_style_shadow_ofs_y(object, 3, 0);
  quiet_clickable(object);
}
static lv_obj_t* provision_ui_button(lv_obj_t* parent, int x, int y, const char* text, bool primary) {
  return halo_ui_button(parent, x, y, 118, 52, text,
                        primary ? COL_GREEN : COL_WHITE, primary ? COL_WHITE : COL_DARK);
}
static bool provision_ui_hit(lv_obj_t* object, uint16_t x, uint16_t y) {
  if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
  lv_area_t a;
  lv_obj_get_coords(object, &a);
  return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}
static void provision_ui_overlay(lv_obj_t* object) {
  lv_obj_set_size(object, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_pos(object, 0, 0);
  lv_obj_set_style_bg_color(object, lv_color_hex(COL_CREAM), 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_set_style_radius(object, 0, 0);
  lv_obj_set_style_shadow_width(object, 0, 0);
  lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
static void provision_ui_step_label(int step) {
  char text[20];
  snprintf(text, sizeof(text), "STEP %d OF 6", step);
  provision_status_label = provision_ui_label(provision_screen, text, &nunito_12,
                                               COL_MUTED, 34, 160);
  lv_obj_set_style_text_letter_space(provision_status_label, 1, 0);
}
static void provision_ui_app_pill() {
  lv_obj_t* pill = halo_ui_card(provision_screen, 109, 64, 142, 29, COL_GOLD, 15);
  lv_obj_set_style_shadow_ofs_x(pill, 2, 0);
  lv_obj_set_style_shadow_ofs_y(pill, 2, 0);
  lv_obj_t* text = halo_ui_label(pill, "On Trepo App", &nunito_16, COL_DARK, 0, 0, 132);
  lv_obj_center(text);
}
static void provision_ui_scroll_cue() {
  lv_obj_t* cue = lv_img_create(provision_screen);
  lv_img_set_src(cue, &provision_scroll_cue_img);
  lv_obj_set_pos(cue, PROVISION_SCROLL_CUE_X, PROVISION_SCROLL_CUE_Y);
  lv_obj_set_style_img_recolor(cue, lv_color_hex(COL_TEAL), 0);
  lv_obj_set_style_img_recolor_opa(cue, LV_OPA_COVER, 0);
  lv_obj_clear_flag(cue, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}
static bool provision_ui_make_qr(const std::string& data, int y) {
  lv_obj_t* card = halo_ui_card(provision_screen, 94, y, 172, 172, COL_WHITE, 18);
  provision_qr = lv_qrcode_create(card, 148, lv_color_black(), lv_color_white());
  if (!provision_qr || lv_qrcode_update(provision_qr, data.data(), data.size()) != LV_RES_OK) {
    lv_obj_del(card);
    provision_qr = NULL;
    return false;
  }
  lv_obj_center(provision_qr);
  return true;
}
static void provision_ui_progress() {
  lv_obj_t* spinner = lv_spinner_create(provision_screen, 1100, 105);
  lv_obj_set_size(spinner, 128, 128);
  lv_obj_set_pos(spinner, 116, 86);
  lv_obj_set_style_arc_opa(spinner, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(COL_TEAL), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(spinner, true, LV_PART_INDICATOR);
  lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* center = halo_ui_card(provision_screen, 140, 110, 80, 80, COL_WHITE, 40);
  halo_ui_icon(center, HALO_ICON_WIFI, 16, 16, 44, COL_TEAL);
}
static void provision_ui_phone() {
  lv_obj_t* seal = halo_ui_card(provision_screen, 123, 112, 114, 114, COL_WHITE, 57);
  lv_obj_t* phone = halo_ui_card(seal, 36, 20, 38, 66, COL_CREAM, 9);
  lv_obj_set_style_shadow_width(phone, 0, 0);
  lv_obj_t* dot = lv_obj_create(phone);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 5, 5);
  lv_obj_set_pos(dot, 14, 51);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(COL_DARK), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  halo_ui_icon(phone, HALO_ICON_WIFI, 7, 17, 20, COL_TEAL);
}
static void provision_ui_profile_draw(lv_event_t* e) {
  // Draw into the existing badge, using the same ink as the other vector icons.
  // No symbol font, extra object or image buffer is needed for this small avatar.
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target(e), &a);
  lv_draw_rect_dsc_t ink;
  lv_draw_rect_dsc_init(&ink);
  ink.bg_color = lv_color_hex(COL_DARK);
  ink.bg_opa = LV_OPA_COVER;
  ink.radius = LV_RADIUS_CIRCLE;
  lv_area_t head = {(lv_coord_t)(a.x1 + 9), (lv_coord_t)(a.y1 + 5),
                    (lv_coord_t)(a.x1 + 15), (lv_coord_t)(a.y1 + 11)};
  lv_area_t shoulders = {(lv_coord_t)(a.x1 + 6), (lv_coord_t)(a.y1 + 13),
                         (lv_coord_t)(a.x1 + 18), (lv_coord_t)(a.y1 + 19)};
  lv_draw_rect(lv_event_get_draw_ctx(e), &ink, &head);
  lv_draw_rect(lv_event_get_draw_ctx(e), &ink, &shoulders);
}
static void provision_ui_assistant_draw(lv_event_t* e) {
  // Match iOS HaloDeviceIcon's 34x48 vector in its 24x34 profile-row footprint.
  // Draw directly into the existing buffer; no bitmap, symbol font or child objects.
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target(e), &a);
  auto scaled = [](int value) { return (value * 24 + 17) / 34; };
  auto shape = [&](int x, int y, int w, int h, int radius, uint32_t color, int border) {
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_hex(color);
    ink.bg_opa = LV_OPA_COVER;
    ink.radius = radius < 0 ? LV_RADIUS_CIRCLE : scaled(radius);
    ink.border_color = lv_color_hex(COL_DARK);
    ink.border_width = border;
    const lv_coord_t left = a.x1 + scaled(x), top = a.y1 + scaled(y);
    lv_area_t area = {left, top, (lv_coord_t)(left + scaled(w) - 1),
                                 (lv_coord_t)(top + scaled(h) - 1)};
    lv_draw_rect(lv_event_get_draw_ctx(e), &ink, &area);
  };
  shape(5, 14, 24, 33, 5, COL_DARK, 0);
  shape(1, 1, 32, 32, -1, COL_TEAL, 1);
  shape(5, 5, 24, 24, -1, COL_CREAM, 1);
}
static void provision_ui_success_frame(void* object, int32_t value) {
  lv_obj_t* page = (lv_obj_t*)object;
  const lv_opa_t opacity = (lv_opa_t)value;
  lv_obj_set_style_bg_color(page,
      lv_color_mix(lv_color_hex(COL_GREEN), lv_color_hex(COL_CREAM), opacity), 0);
  // Animate existing primitives, not object opacity or transformed layers.
  // The two cards retain their original fill, border and 85% hard shadow.
  const int offset = 8 * (255 - value) / 255;
  const int final_y[] = {52, 113, 225};
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(page) && i < 3; ++i) {
    lv_obj_t* child = lv_obj_get_child(page, i);
    lv_obj_set_y(child, final_y[i] + offset);
    if (i < 2) {
      lv_obj_set_style_bg_opa(child, opacity, 0);
      lv_obj_set_style_border_opa(child, opacity, 0);
      lv_obj_set_style_shadow_opa(child, (lv_opa_t)(217 * value / 255), 0);
      lv_obj_t* ink = lv_obj_get_child(child, 0);
      if (ink) lv_obj_set_style_text_opa(ink, opacity, 0);
    } else {
      lv_obj_set_style_text_opa(child, opacity, 0);
    }
  }
}
static void provision_ui_success_stop() {
  if (provision_screen) lv_anim_del(provision_screen, provision_ui_success_frame);
}
static void provision_ui_success_start() {
  provision_ui_success_stop();
  // Install the final primitive styles before allocating the animation. If
  // memory is tight, keep this fully visible final page without any motion.
  provision_ui_success_frame(provision_screen, LV_OPA_COVER);
  lv_mem_monitor_t memory;
  lv_mem_monitor(&memory);
  if (memory.free_biggest_size < 1024) return;
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, provision_screen);
  lv_anim_set_exec_cb(&anim, provision_ui_success_frame);
  lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&anim, 360);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
  if (!lv_anim_start(&anim)) provision_ui_success_frame(provision_screen, LV_OPA_COVER);
}
static void provision_ui_render() {
  if (g_ota_screen_active) { provision_ui_deferred = true; return; }
  if (!provision_flow.active()) return;
  if (!provision_screen) {
    provision_screen = lv_obj_create(lv_layer_top());
    provision_ui_overlay(provision_screen);
  }
  // Delete the previous page's small objects and its owned spinner together.
  // No full-screen canvas or animated transformed layer is allocated.
  provision_ui_success_stop();
  lv_obj_clean(provision_screen);
  provision_qr = provision_status_label = provision_title_label = NULL;
  provision_ssid_label = provision_url_label = NULL;
  provision_ui_back_btn = provision_ui_retry_btn = NULL;
  lv_obj_set_style_bg_color(provision_screen, lv_color_hex(COL_CREAM), 0);
  provision_ui_can_scroll = provision_flow.scrollable();
  const auto step = provision_flow.step;
  if (step >= LcdProvisionFlow::GetApp && step <= LcdProvisionFlow::Connecting)
    provision_ui_step_label((int)step);
  if (step == LcdProvisionFlow::GetApp) {
    // Two fonts on one centered line preserve the approved emphasis.
    lv_point_t light_size, bold_size;
    lv_txt_get_size(&light_size, "Get the", &lv_font_montserrat_22, 0, 0, 360, LV_TEXT_FLAG_NONE);
    lv_txt_get_size(&bold_size, "Trepo app", &nunito_24, 0, 0, 360, LV_TEXT_FLAG_NONE);
    const int left = (360 - light_size.x - bold_size.x - 7) / 2;
    halo_ui_label(provision_screen, "Get the", &lv_font_montserrat_22, COL_DARK,
                  left, 66, light_size.x + 1);
    halo_ui_label(provision_screen, "Trepo app", &nunito_24, COL_DARK,
                  left + light_size.x + 7, 65, bold_size.x + 1);
    if (!provision_ui_make_qr("https://hellotrepo.com/app-signup", 116)) {
      provision_ui_qr_failed = true;
      provision_flow.step = LcdProvisionFlow::Failed;
      provision_ui_render();
      return;
    }
    provision_ui_scroll_cue();
  } else if (step == LcdProvisionFlow::AppRoute) {
    provision_ui_app_pill();
    lv_obj_t* profile = halo_ui_card(provision_screen, 81, 119, 198, 55, COL_WHITE, 18);
    // Match the app's Profile button: dark filled person in a white outlined circle.
    lv_obj_t* avatar = halo_ui_card(profile, 16, 14, 25, 25, COL_WHITE, 13);
    lv_obj_set_style_shadow_width(avatar, 0, 0);
    lv_obj_add_event_cb(avatar, provision_ui_profile_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t* title = halo_ui_label(profile, "Profile", &nunito_22, COL_DARK, 48, 13, 131);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    halo_ui_icon(provision_screen, HALO_ICON_CHEVRON_DOWN, 169, 182, 22, COL_TEAL);
    lv_obj_t* route = halo_ui_card(provision_screen, 81, 214, 198, 67, COL_WHITE, 18);
    lv_obj_t* assistant = lv_obj_create(route);
    lv_obj_remove_style_all(assistant);
    lv_obj_set_pos(assistant, 16, 15);
    lv_obj_set_size(assistant, 24, 34);
    lv_obj_clear_flag(assistant, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(assistant, provision_ui_assistant_draw, LV_EVENT_DRAW_MAIN, NULL);
    title = halo_ui_label(route, "Setup kitchen\nassistant", &nunito_18, COL_DARK, 48, 12, 142);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    provision_ui_scroll_cue();
  } else if (step == LcdProvisionFlow::PairQr) {
    provision_ui_label(provision_screen, "Scan to connect", &nunito_24, COL_DARK, 65, 280);
    if (provision_qr_cached || provision_ui_preview_active) {
      const std::string data = provision_ui_preview_active
          ? QRDisplay::generateWifiQRData("Trepo-Preview", "PreviewOnly")
          : QRDisplay::generateWifiQRData(provision_qr_ssid, provision_qr_password);
      if (!provision_ui_make_qr(data, 108)) {
        provision_ui_qr_failed = true;
        provision_flow.step = LcdProvisionFlow::Failed;
        provision_ui_render();
        return;
      }
    } else {
      provision_ui_progress();
    }
  } else if (step == LcdProvisionFlow::SelectWifi) {
    provision_ui_app_pill();
    provision_ui_phone();
    provision_ui_label(provision_screen, "Select home Wi-Fi", &nunito_24, COL_DARK, 254, 306);
  } else if (step == LcdProvisionFlow::Connecting) {
    provision_ui_progress();
    provision_ui_label(provision_screen,
        provision_flow.claiming ? "Finishing setup..." : "Connecting to\nhome Wi-Fi...",
        &nunito_24, COL_DARK, 244, 290);
  } else if (step == LcdProvisionFlow::Complete) {
    lv_obj_set_style_bg_color(provision_screen, lv_color_hex(COL_GREEN), 0);
    halo_ui_badge(provision_screen, "SETUP COMPLETE", 52, 156);
    lv_obj_t* seal = halo_ui_card(provision_screen, 140, 113, 80, 80, COL_GOLD, 40);
    halo_ui_icon(seal, HALO_ICON_CHECK, 14, 14, 48, COL_GREEN);
    provision_ui_label(provision_screen, "Welcome to\nyour assistant", &nunito_28, COL_WHITE, 225, 300);
    provision_ui_success_start();
  } else if (step == LcdProvisionFlow::Failed) {
    lv_obj_t* seal = halo_ui_card(provision_screen, 144, 48, 72, 72, COL_RED, 36);
    halo_ui_icon(seal, HALO_ICON_WARNING, 14, 14, 40, COL_WHITE);
    provision_ui_label(provision_screen, provision_ui_qr_failed ? "No setup code yet" : "Couldn't connect",
                       &nunito_24, COL_DARK, 147, 294);
    provision_ui_label(provision_screen, provision_ui_qr_failed
        ? "Please try setup again."
        : "Check the Wi-Fi details in the app, then try again.",
        &lv_font_montserrat_14, COL_TEXT2, 190, 260);
    provision_ui_back_btn = provision_ui_button(provision_screen, 54, 249, "BACK", false);
    provision_ui_retry_btn = provision_ui_button(provision_screen, 188, 249, "TRY AGAIN", true);
  }
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(provision_screen);
  provision_screen_visible = true;
  if (!provision_ui_preview_active) provisioning_active = true;
  Serial.printf("[PROVISION_UI] step=%d app_connected=%d claiming=%d\n",
                (int)step, provision_flow.app_connected, provision_flow.claiming);
}
static void update_provision_status_label(const char* state) {
  if (!state || !state[0]) return;
  if (provision_ui_preview_active) return;
  if (!strcmp(state, "idle")) { hide_provisioning_screen(); return; }
  // These are setup-only peer states. Recover a missed first QR/intro event,
  // while leaving ordinary already-provisioned "connected" heartbeats inert.
  if (!strcmp(state, "app_connected") || !strcmp(state, "claiming")) provision_flow.begin();
  if (provision_flow.status(state, millis())) {
    hide_provision_intro_screen("status_advance");
    provision_ui_qr_failed = false;
    provision_ui_render();
  }
}
static void provision_ui_service() {
  if (g_ota_screen_active) return;
  if (provision_ui_deferred) { provision_ui_deferred = false; provision_ui_render(); }
  if (provision_ui_preview_active) {
    if (uint32_t(millis() - provision_ui_preview_at_ms) >= 60000 ||
        provision_flow.completion_due(millis())) provision_ui_preview(0);
    return;
  }
  if (provision_flow.check_timeout(millis())) {
    Serial.println("[PROVISION_UI] connection_timeout_ms=240000");
    provision_ui_render();
  }
  if (provision_flow.completion_due(millis())) {
    // Publish the fresh foreground interval before dropping the completion
    // lease: the sleep loop runs on the other core and may already be overdue.
    lcd_guardian_begin_foreground((uint32_t)millis());
    provision_flow.close();
    provision_ui_can_scroll = false;
    provisioning_active = false;
    provision_user_requested = provision_intro_tapped = provision_qr_cached = false;
    provision_return_home_pending = true;
    resetActivityTimer();
  }
}
static void provision_ui_resume_deferred() { provision_ui_service(); }
static bool provision_ui_handle_scroll(int delta) {
  if (!provision_flow.active()) return false;
  if (provision_flow.scroll(delta)) provision_ui_render();
  return true;
}
static void provision_qr_wait_begin(const char* reason) {
  provision_qr_waiting = true;
  provision_qr_wait_start_ms = millis();
  Serial.printf("[PROVISION] wait_for_qr begin reason=%s\n", reason ? reason : "unknown");
  provision_ui_qr_failed = false;
  hide_provision_intro_screen("wait_for_qr");
  provision_flow.begin();
  provision_ui_render();
}
static void provision_qr_wait_clear(const char* reason) {
  if (!provision_qr_waiting) return;
  provision_qr_waiting = false;
  provision_qr_wait_start_ms = 0;
  Serial.printf("[PROVISION] wait_for_qr clear reason=%s\n", reason ? reason : "unknown");
}
static void provision_cache_qr(const char* ssid, const char* password, const char* url) {
  if (!ssid) ssid = "";
  if (!password) password = "";
  if (!url || !url[0]) url = "http://192.168.4.1";
  strncpy(provision_qr_ssid, ssid, sizeof(provision_qr_ssid) - 1);
  provision_qr_ssid[sizeof(provision_qr_ssid) - 1] = '\0';
  strncpy(provision_qr_password, password, sizeof(provision_qr_password) - 1);
  provision_qr_password[sizeof(provision_qr_password) - 1] = '\0';
  strncpy(provision_qr_url, url, sizeof(provision_qr_url) - 1);
  provision_qr_url[sizeof(provision_qr_url) - 1] = '\0';
  provision_qr_cached = true;
}
static void create_provision_intro_screen_if_needed() {
  if (provision_intro_screen) return;
  provision_intro_screen = lv_obj_create(lv_layer_top());
  provision_ui_overlay(provision_intro_screen);
  lv_obj_t* seal = halo_ui_card(provision_intro_screen, 150, 36, 60, 60, COL_GOLD, 30);
  halo_ui_icon(seal, HALO_ICON_WIFI, 10, 10, 36, COL_DARK);
  provision_ui_intro_title = provision_ui_label(provision_intro_screen, "Change Wi-Fi", &nunito_24, COL_DARK, 116, 280);
  provision_ui_intro_copy = provision_ui_label(provision_intro_screen,
      "Forget this network and reconnect with your phone?", &lv_font_montserrat_14, COL_TEXT2, 154, 264);
  provision_ui_intro_cancel = provision_ui_button(provision_intro_screen, 54, 232, "CANCEL", false);
  provision_ui_intro_start = provision_ui_button(provision_intro_screen, 188, 232, "START", true);
}
static bool show_provision_intro_screen(const char* reason) {
  if (g_ota_screen_active) { provision_intro_pending = true; return false; }
  if (!provision_ui_reset_confirmation_pending && !provision_ui_reset_confirmation) {
    provision_intro_pending = false;
    provision_intro_tapped = true;
    provision_flow.begin();
    provision_ui_render();
    return true;
  }
  create_provision_intro_screen_if_needed();
  provision_ui_reset_confirmation = true;
  provision_ui_reset_confirmation_pending = false;
  provision_ui_can_scroll = false;
  lv_obj_clear_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(provision_intro_screen);
  provision_intro_visible = true;
  provision_intro_pending = false;
  Serial.printf("[PROVISION] intro_show reason=%s\n", reason ? reason : "unknown");
  return true;
}
static void show_provision_reset_confirm() {
  provision_ui_reset_confirmation_pending = true;
  show_provision_intro_screen("reset_confirm");
}
static void hide_provision_intro_screen(const char* reason) {
  provision_ui_reset_confirmation_pending = false;
  if (provision_intro_screen) lv_obj_add_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
  provision_intro_visible = false;
  provision_intro_pending = false;
  provision_ui_reset_confirmation = false;
  (void)reason;
}
static void show_provisioning_screen(const char* ssid, const char* password, const char* url) {
  // Real setup always supersedes a USB-only preview. The peer's QR remains
  // real state and is cached only after the preview overlay has been removed.
  if (provision_ui_preview_active) provision_ui_preview(0);
  provision_cache_qr(ssid, password, url);
  provision_qr_wait_clear("qr_shown");
  hide_provision_intro_screen("qr_show");
  const bool was_active = provision_flow.active();
  provision_flow.begin();
  provision_intro_tapped = true;
  // Repeated QR messages refresh only the QR page, never reset earlier/later pages.
  if (!was_active || provision_flow.step == LcdProvisionFlow::PairQr) provision_ui_render();
}
static void hide_provisioning_screen() {
  provision_ui_deferred = false;
  provision_ui_can_scroll = false;
  provision_ui_preview_active = false;
  provision_ui_preview_at_ms = 0;
  provision_flow.close();
  if (provision_screen) {
    provision_ui_success_stop();
    lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(provision_screen);
  }
  provision_qr = provision_status_label = provision_title_label = NULL;
  provision_ssid_label = provision_url_label = NULL;
  provision_ui_back_btn = provision_ui_retry_btn = NULL;
  provision_screen_visible = false;
  provision_qr_wait_clear("qr_hidden");
}
static void provision_ui_suspend_for_ota() {
  if (!provision_flow.active()) return;
  // Keep logical progress but release presentation and every owned animation.
  // The UI owner redraws the same step after the OTA overlay is removed.
  provision_ui_deferred = true;
  provision_ui_can_scroll = false;
  if (provision_screen) {
    provision_ui_success_stop();
    lv_obj_clean(provision_screen);
    lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  }
  provision_qr = provision_status_label = provision_title_label = NULL;
  provision_ssid_label = provision_url_label = NULL;
  provision_ui_back_btn = provision_ui_retry_btn = NULL;
  provision_screen_visible = false;
}
static void provision_ui_preview(int step) {
#if defined(HALO_UI_REVIEW) && HALO_UI_REVIEW
  // Reject while real setup is active. Preview never changes credentials,
  // sends UART messages, or touches the peer's provisioning/owner state.
  if (step < 0 || step > 6 || g_ota_screen_active ||
      (!provision_ui_preview_active &&
       (provisioning_active || provision_flow.active() || provision_intro_visible || provision_qr_waiting ||
        (ui_screen_state != SCREEN_HOME && ui_screen_state != SCREEN_SETTINGS)))) {
    Serial.println("[PROVISION_PREVIEW] rejected=busy_or_invalid");
    return;
  }
  if (!step) {
    if (!provision_ui_preview_active) return;
    provision_ui_preview_active = false;
    hide_provisioning_screen();
    resetActivityTimer();
    Serial.println("[PROVISION_PREVIEW] off (no credentials changed)");
    return;
  }
  provision_ui_preview_active = true;
  provision_ui_preview_at_ms = millis();
  provision_flow.close();
  provision_flow.step = (LcdProvisionFlow::Step)step;
  provision_flow.app_connected = step >= 4;
  provision_flow.complete_at_ms = millis();
  provision_ui_render();
  resetActivityTimer();
  Serial.printf("[PROVISION_PREVIEW] step=%d no_peer_commands=1\n", step);
#else
  (void)step;
#endif
}
static void provision_ui_qr_timeout() {
  provision_ui_qr_failed = true;
  provision_flow.begin();
  provision_flow.clear_progress_sleep();
  provision_flow.step = LcdProvisionFlow::Failed;
  provision_ui_render();
}
static void provision_ui_reset_start() {
  if (provision_ui_preview_active) return;
  lcd_force_wake_sense("reset_wifi");
  provision_user_requested = true;
  provision_intro_tapped = true;
  provision_intro_pending = false;
  provision_qr_cached = false;
  provision_flow.close();
  StaticJsonDocument<128> doc;
  doc["ver"] = PROTOCOL_VERSION;
  doc["type"] = "INPUT_RESET_WIFI";
  doc["msg_id"] = get_next_msg_id();
  doc["ts"] = millis();
  String output;
  serializeJson(doc, output);
  senseSerial.println(output);
  provision_qr_wait_begin("menu_action");
}
static bool provision_ui_handle_touch(uint16_t x, uint16_t y) {
  if (provision_ui_preview_active) return true;
  if (provision_intro_visible) {
    lv_obj_update_layout(provision_intro_screen);
    if (provision_ui_hit(provision_ui_intro_cancel, x, y)) {
      hide_provision_intro_screen("cancel_reset");
      show_ship_settings_screen();
    } else if (provision_ui_hit(provision_ui_intro_start, x, y)) {
      hide_provision_intro_screen("confirm_reset");
      provision_ui_reset_start();
    }
    return true;
  }
  if (provision_flow.active()) {
    if (provision_flow.step == LcdProvisionFlow::Failed) {
      lv_obj_update_layout(provision_screen);
      if (provision_ui_hit(provision_ui_back_btn, x, y)) {
        provision_flow.close();
        provision_flow.begin();
        provision_ui_render();
      } else if (provision_ui_hit(provision_ui_retry_btn, x, y)) {
        // Claim failure may have stopped the AP. A cached QR is not evidence
        // that its network still exists: explicitly restart setup on retry.
        provision_ui_reset_start();
      }
    }
    return true;
  }
  return false;
}

static void ship_style_plain_screen(lv_obj_t* screen, lv_color_t bg_color);
static void ship_anim_set_y(void* obj, int32_t v);
static void ship_anim_set_x(void* obj, int32_t v);
static void ship_anim_set_text_opa(void* obj, int32_t v);
static void ship_anim_set_obj_opa(void* obj, int32_t v);
static void ship_anim_set_zoom(void* obj, int32_t v);
static void ship_anim_set_border_opa(void* obj, int32_t v);
#endif

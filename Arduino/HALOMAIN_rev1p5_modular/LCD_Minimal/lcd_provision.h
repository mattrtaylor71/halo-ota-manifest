/*
 * lcd_provision.h
 *
 * Provisioning QR screen: WiFi QR code display, provision status
 * label updates, QR caching, and provision intro screen helpers.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 10.
 *
 * Prerequisites (must be declared before #include "lcd_provision.h"):
 *   - LVGL, QR display headers
 *   - Provisioning state globals
 */

#ifndef LCD_PROVISION_H
#define LCD_PROVISION_H

// Exact API from the bundled third-party QR implementation. Its header shares
// a guard with the disabled SDK QR header, so declare the existing C ABI here.
extern "C" lv_obj_t* lv_qrcode_create(lv_obj_t*, lv_coord_t, lv_color_t, lv_color_t);
extern "C" lv_res_t lv_qrcode_update(lv_obj_t*, const void*, uint32_t);

// These screens remain overlays: provisioning can change independently of the
// underlying menu, but only the existing UI-lock owner creates or styles them.
static void show_ship_settings_screen();
static void hide_provision_intro_screen(const char* reason);
static void show_provisioning_screen(const char* ssid, const char* password, const char* url);
static void hide_provisioning_screen();

enum provision_ui_view_t {
  PROVISION_UI_PREPARING,
  PROVISION_UI_QR,
  PROVISION_UI_CONNECTING,
  PROVISION_UI_FAILED
};
static provision_ui_view_t provision_ui_view = PROVISION_UI_PREPARING;
static int provision_ui_deferred_view = -1;
static bool provision_ui_reset_confirmation = false;
static bool provision_ui_reset_confirmation_pending = false;
static bool provision_ui_qr_failed = false;
static lv_obj_t* provision_ui_qr_card = NULL;
static lv_obj_t* provision_ui_spinner = NULL;
static lv_obj_t* provision_ui_badge = NULL;
static lv_obj_t* provision_ui_failure_seal = NULL;
static lv_obj_t* provision_ui_back_btn = NULL;
static lv_obj_t* provision_ui_retry_btn = NULL;
static lv_obj_t* provision_ui_intro_title = NULL;
static lv_obj_t* provision_ui_intro_copy = NULL;
static lv_obj_t* provision_ui_intro_cancel = NULL;
static lv_obj_t* provision_ui_intro_start = NULL;

static lv_obj_t* provision_ui_label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                                    uint32_t color, int y, int width) {
  lv_obj_t* label = lv_label_create(parent);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(label, 3, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_label_set_text(label, text);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
  return label;
}

static void provision_ui_card(lv_obj_t* object, uint32_t color, int radius) {
  trepo_card(object, color, radius);
  lv_obj_set_style_shadow_ofs_x(object, 3, 0);
  lv_obj_set_style_shadow_ofs_y(object, 3, 0);
  quiet_clickable(object);
}

static lv_obj_t* provision_ui_button(lv_obj_t* parent, int x, int y, const char* text,
                                     bool primary) {
  lv_obj_t* button = lv_obj_create(parent);
  lv_obj_set_size(button, 118, 52);
  lv_obj_set_pos(button, x, y);
  provision_ui_card(button, primary ? COL_GREEN : COL_WHITE, 16);
  lv_obj_t* label =
      provision_ui_label(button, text, &nunito_16, primary ? COL_WHITE : COL_DARK, 0, 110);
  lv_obj_set_style_text_letter_space(label, 1, 0);
  lv_obj_center(label);
  return button;
}

static bool provision_ui_hit(lv_obj_t* object, uint16_t x, uint16_t y) {
  if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
    return false;
  lv_area_t area;
  lv_obj_get_coords(object, &area);
  return x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2;
}

static void provision_ui_stop_spinner() {
  if (provision_ui_spinner) {
    lv_obj_del(provision_ui_spinner);
    provision_ui_spinner = NULL;
  }
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

static void create_provision_screen_if_needed() {
  if (provision_screen)
    return;
  provision_screen = lv_obj_create(lv_layer_top());
  provision_ui_overlay(provision_screen);

  provision_ui_badge = lv_obj_create(provision_screen);
  lv_obj_set_size(provision_ui_badge, 112, 25);
  lv_obj_set_pos(provision_ui_badge, 124, 18);
  provision_ui_card(provision_ui_badge, COL_GOLD, LV_RADIUS_CIRCLE);
  provision_status_label =
      provision_ui_label(provision_ui_badge, "STEP 1 OF 2", &nunito_12, COL_DARK, 0, 106);
  lv_obj_center(provision_status_label);

  provision_ui_qr_card = lv_obj_create(provision_screen);
  lv_obj_set_size(provision_ui_qr_card, 172, 172);
  lv_obj_set_pos(provision_ui_qr_card, 94, 54);
  provision_ui_card(provision_ui_qr_card, COL_WHITE, 18);
  provision_qr = NULL;

  provision_ui_failure_seal = lv_obj_create(provision_screen);
  lv_obj_set_size(provision_ui_failure_seal, 72, 72);
  lv_obj_set_pos(provision_ui_failure_seal, 144, 46);
  provision_ui_card(provision_ui_failure_seal, COL_RED, LV_RADIUS_CIRCLE);
  lv_obj_t* warning = provision_ui_label(provision_ui_failure_seal, LV_SYMBOL_WARNING,
                                         &lv_font_montserrat_32, COL_WHITE, 0, 52);
  lv_obj_center(warning);

  provision_title_label =
      provision_ui_label(provision_screen, "Scan with your phone", &nunito_22, COL_DARK, 238, 292);
  provision_ssid_label = provision_ui_label(
      provision_screen, "It joins HALO's Wi-Fi, then setup finishes in your browser.",
      &lv_font_montserrat_14, COL_TEXT2, 266, 250);
  provision_url_label =
      provision_ui_label(provision_screen, "", &lv_font_montserrat_12, COL_MUTED, 312, 230);
  provision_ui_back_btn = provision_ui_button(provision_screen, 54, 240, "BACK", false);
  provision_ui_retry_btn = provision_ui_button(provision_screen, 188, 240, "TRY AGAIN", true);
}

static void provision_ui_show_view(provision_ui_view_t view) {
  if (g_ota_screen_active) {
    provision_ui_deferred_view = (int)view;
    return;
  }
  create_provision_screen_if_needed();
  provision_ui_view = view;
  provision_ui_stop_spinner();
  lv_obj_add_flag(provision_ui_badge, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(provision_ui_qr_card, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(provision_ui_failure_seal, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(provision_ui_back_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(provision_ui_retry_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(provision_url_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_font(provision_title_label, &nunito_22, 0);

  if (view == PROVISION_UI_QR) {
    lv_label_set_text(provision_status_label, "STEP 1 OF 2");
    lv_obj_clear_flag(provision_ui_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(provision_ui_qr_card, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(provision_title_label, "Scan with your phone");
    lv_obj_set_style_text_font(provision_title_label, &nunito_18, 0);
    lv_obj_align(provision_title_label, LV_ALIGN_TOP_MID, 0, 238);
    lv_label_set_text(provision_ssid_label, "Join HALO's Wi-Fi.\nFinish setup in your browser.");
    lv_obj_align(provision_ssid_label, LV_ALIGN_TOP_MID, 0, 266);
  } else if (view == PROVISION_UI_FAILED) {
    lv_obj_clear_flag(provision_ui_failure_seal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(provision_ui_back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(provision_ui_retry_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(provision_title_label,
                      provision_ui_qr_failed ? "No setup code yet" : "Couldn't connect");
    lv_obj_align(provision_title_label, LV_ALIGN_TOP_MID, 0, 150);
    lv_label_set_text(provision_ssid_label,
                      provision_ui_qr_failed
                          ? "Keep both parts connected, then try again."
                          : "Check the Wi-Fi details on your phone. Setup is still required.");
    lv_obj_align(provision_ssid_label, LV_ALIGN_TOP_MID, 0, 190);
  } else {
    if (view == PROVISION_UI_CONNECTING) {
      lv_label_set_text(provision_status_label, "STEP 2 OF 2");
      lv_obj_clear_flag(provision_ui_badge, LV_OBJ_FLAG_HIDDEN);
    }
    provision_ui_spinner = lv_spinner_create(provision_screen, 1100, 108);
    lv_obj_set_size(provision_ui_spinner, 84, 84);
    lv_obj_set_pos(provision_ui_spinner, 138, 80);
    lv_obj_set_style_arc_width(provision_ui_spinner, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(provision_ui_spinner, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(provision_ui_spinner, lv_color_hex(0xE6D7BF), LV_PART_MAIN);
    lv_obj_set_style_arc_color(provision_ui_spinner, lv_color_hex(COL_TEAL), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(provision_ui_spinner, true, LV_PART_INDICATOR);
    lv_obj_clear_flag(provision_ui_spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(provision_title_label,
                      view == PROVISION_UI_CONNECTING ? "Connecting..." : "Getting ready...");
    lv_obj_align(provision_title_label, LV_ALIGN_TOP_MID, 0, 188);
    lv_label_set_text(provision_ssid_label, view == PROVISION_UI_CONNECTING
                                                ? "Finish the steps on your phone."
                                                : "Creating HALO's setup network.");
    lv_obj_align(provision_ssid_label, LV_ALIGN_TOP_MID, 0, 226);
  }
  if (status_screen)
    lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(provision_screen);
  // The original QR-arrival timeout tests !provision_screen_visible. Preparing
  // is visible artwork, but it must not masquerade as an arrived QR.
  provision_screen_visible = view != PROVISION_UI_PREPARING;
}

static void update_provision_status_label(const char* state) {
  if (!state || !state[0])
    return;
  if (strcmp(state, "connected") == 0 || strcmp(state, "idle") == 0) {
    hide_provisioning_screen();
    return; // Existing receiver code owns success, scheduling and Home return.
  }
  if (strcmp(state, "connecting") == 0) {
    hide_provision_intro_screen("connecting");
    provision_ui_show_view(PROVISION_UI_CONNECTING);
  } else if (strcmp(state, "failed") == 0) {
    provision_ui_qr_failed = false;
    hide_provision_intro_screen("failed");
    provision_ui_show_view(PROVISION_UI_FAILED);
  } else if (strcmp(state, "ap_setup") == 0 && provision_intro_tapped && provision_qr_cached) {
    show_provisioning_screen(provision_qr_ssid, provision_qr_password, provision_qr_url);
  }
}

static void provision_qr_wait_begin(const char* reason) {
  provision_qr_waiting = true;
  provision_qr_wait_start_ms = millis();
  Serial.printf("[PROVISION] wait_for_qr begin reason=%s\n", reason ? reason : "unknown");
  provision_ui_qr_failed = false;
  hide_provision_intro_screen("wait_for_qr");
  provision_ui_show_view(PROVISION_UI_PREPARING);
}

static void provision_qr_wait_clear(const char* reason) {
  if (!provision_qr_waiting)
    return;
  provision_qr_waiting = false;
  provision_qr_wait_start_ms = 0;
  Serial.printf("[PROVISION] wait_for_qr clear reason=%s\n", reason ? reason : "unknown");
  // Called from the UART owner too: no LVGL work here.
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
  if (provision_intro_screen)
    return;
  provision_intro_screen = lv_obj_create(lv_layer_top());
  provision_ui_overlay(provision_intro_screen);
  lv_obj_t* seal = lv_obj_create(provision_intro_screen);
  lv_obj_set_size(seal, 60, 60);
  lv_obj_set_pos(seal, 150, 36);
  provision_ui_card(seal, COL_GOLD, LV_RADIUS_CIRCLE);
  lv_obj_t* wifi =
      provision_ui_label(seal, LV_SYMBOL_WIFI, &lv_font_montserrat_32, COL_DARK, 0, 46);
  lv_obj_center(wifi);
  provision_ui_intro_title =
      provision_ui_label(provision_intro_screen, "Change Wi-Fi", &nunito_24, COL_DARK, 116, 280);
  provision_ui_intro_copy =
      provision_ui_label(provision_intro_screen, "", &lv_font_montserrat_14, COL_TEXT2, 154, 264);
  provision_ui_intro_cancel = provision_ui_button(provision_intro_screen, 54, 232, "CANCEL", false);
  provision_ui_intro_start = provision_ui_button(provision_intro_screen, 188, 232, "START", true);
}

static bool show_provision_intro_screen(const char* reason) {
  if (g_ota_screen_active) {
    provision_intro_pending = true;
    return false;
  }
  create_provision_intro_screen_if_needed();
  if (!provision_intro_screen || !provision_ui_intro_title || !provision_ui_intro_copy ||
      !provision_ui_intro_start || !provision_ui_intro_cancel) {
    provision_intro_pending = true;
    return false;
  }
  if (provision_intro_visible && !provision_ui_reset_confirmation_pending)
    return true;
  provision_ui_reset_confirmation = provision_ui_reset_confirmation_pending;
  provision_ui_reset_confirmation_pending = false;
  lv_label_set_text(provision_ui_intro_title,
                    provision_ui_reset_confirmation ? "Change Wi-Fi" : "Wi-Fi setup");
  lv_label_set_text(provision_ui_intro_copy,
                    provision_ui_reset_confirmation
                        ? "HALO forgets this network and restarts setup. Reconnect with your phone."
                        : "Connect HALO with your phone to finish Wi-Fi setup.");
  // Cancel is offered only before the reset command. Active setup cannot roll
  // back credentials, including when an OTA overlay temporarily deferred it.
  if (provision_ui_reset_confirmation) {
    lv_obj_clear_flag(provision_ui_intro_cancel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(provision_ui_intro_start, 188, 232);
  } else {
    lv_obj_add_flag(provision_ui_intro_cancel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(provision_ui_intro_start, 121, 232);
  }
  if (list_container) lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  if (menu_screen) lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  if (logged_screen) lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
  if (expiry_screen) lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
  if (provision_screen) lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_ui_stop_spinner();
  provision_screen_visible = false;
  lv_obj_clear_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(provision_intro_screen);
  provision_intro_visible = true;
  provision_intro_pending = false;
  ui_screen_state = SCREEN_SETTINGS;
  Serial.printf("[PROVISION] intro_show reason=%s\n", reason ? reason : "unknown");
  lv_timer_handler();
  return true;
}

static void show_provision_reset_confirm() {
  // Preserve the requested confirmation if intro presentation is deferred.
  // No labels are touched unless the UI owner actually created the intro.
  provision_ui_reset_confirmation_pending = true;
  if (!show_provision_intro_screen("reset_confirm"))
    return;
  lv_obj_update_layout(provision_intro_screen);
}

static void hide_provision_intro_screen(const char* reason) {
  provision_ui_reset_confirmation_pending = false;
  if (!provision_intro_screen || !provision_intro_visible)
    return;
  lv_obj_add_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
  provision_intro_visible = false;
  provision_intro_pending = false;
  provision_ui_reset_confirmation = false;
  Serial.printf("[PROVISION] intro_hide reason=%s\n", reason ? reason : "unknown");
}

static void show_provisioning_screen(const char* ssid, const char* password, const char* url) {
  if (g_ota_screen_active) {
    provision_cache_qr(ssid, password, url);
    provision_ui_deferred_view = PROVISION_UI_QR;
    return;
  }
  create_provision_screen_if_needed();
  if (!ssid) ssid = "";
  if (!password) password = "";
  if (!url || !url[0]) url = "http://192.168.4.1";
  hide_provision_intro_screen("qr_show");
  if (list_container) lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  if (menu_screen) lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  if (logged_screen) lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
  if (expiry_screen) lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);

  // The QR implementation still generates the real credential payload. The
  // white card adds an external quiet zone; never ship the HTML sample pattern.
  std::string qr_data = QRDisplay::generateWifiQRData(ssid, password);
  if (!provision_qr)
    provision_qr = lv_qrcode_create(provision_ui_qr_card, 148, lv_color_black(), lv_color_white());
  if (!provision_qr ||
      lv_qrcode_update(provision_qr, qr_data.data(), qr_data.size()) != LV_RES_OK) {
    provision_ui_qr_failed = true;
    provision_ui_show_view(PROVISION_UI_FAILED);
    provision_screen_visible = false;
    lv_timer_handler();
    return;
  }
  lv_obj_center(provision_qr);
  provision_qr_wait_clear("qr_shown");
  provision_ui_show_view(PROVISION_UI_QR);
  lv_timer_handler();
}

static void hide_provisioning_screen() {
  provision_ui_deferred_view = -1;
  const bool was_visible =
      provision_screen && !lv_obj_has_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_ui_stop_spinner();
  if (provision_screen)
    lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_screen_visible = false;
  provision_qr_wait_clear("qr_hidden");
  if (was_visible && list_container && g_active.count > 0 &&
      ui_screen_state == SCREEN_SHOPPING_LIST)
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  lv_timer_handler();
}

static void provision_ui_resume_deferred() {
  if (g_ota_screen_active || provision_ui_deferred_view < 0)
    return;
  provision_ui_view_t view = (provision_ui_view_t)provision_ui_deferred_view;
  provision_ui_deferred_view = -1;
  if (view == PROVISION_UI_QR && provision_qr_cached)
    show_provisioning_screen(provision_qr_ssid, provision_qr_password, provision_qr_url);
  else
    provision_ui_show_view(view);
}

static void provision_ui_qr_timeout() {
  // Called at the existing 12-second timeout; it grants no extra opportunity.
  provision_ui_qr_failed = true;
  hide_provision_intro_screen("qr_timeout");
  provision_ui_show_view(PROVISION_UI_FAILED);
}

static void provision_ui_reset_start() {
  // Exactly the existing RESET_WIFI action, moved behind the visible Start.
  lcd_force_wake_sense("reset_wifi");
  provision_user_requested = true;
  provision_intro_tapped = true;
  provision_intro_pending = false;
  provision_qr_cached = false;
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
  if (provision_intro_visible) {
    lv_obj_update_layout(provision_intro_screen);
    if (provision_ui_reset_confirmation && provision_ui_hit(provision_ui_intro_cancel, x, y)) {
      hide_provision_intro_screen("cancel_reset");
      show_ship_settings_screen();
    } else if (provision_ui_hit(provision_ui_intro_start, x, y)) {
      if (provision_ui_reset_confirmation) {
        hide_provision_intro_screen("confirm_reset");
        provision_ui_reset_start();
      } else {
        provision_intro_tapped = true;
        provision_intro_pending = false;
        if (provision_qr_cached) {
          show_provisioning_screen(provision_qr_ssid, provision_qr_password, provision_qr_url);
        } else {
          provision_qr_wait_begin("intro_start");
        }
      }
    }
    return true;
  }
  if (provision_screen && !lv_obj_has_flag(provision_screen, LV_OBJ_FLAG_HIDDEN)) {
    if (provision_ui_view == PROVISION_UI_FAILED) {
      lv_obj_update_layout(provision_screen);
      if (provision_ui_hit(provision_ui_back_btn, x, y)) {
        show_provision_intro_screen("setup_back");
      } else if (provision_ui_hit(provision_ui_retry_btn, x, y)) {
        provision_intro_tapped = true;
        if (provision_qr_cached) {
          show_provisioning_screen(provision_qr_ssid, provision_qr_password, provision_qr_url);
        } else {
          provision_ui_reset_start();
        }
      }
    }
    return true; // Preparing/QR/connecting never activate the screen underneath.
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


#endif // LCD_PROVISION_H

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

static void update_provision_status_label(const char* state) {
  if (!provision_status_label) {
    return;
  }
  const char* text = "Setup Mode";
  if (state && strcmp(state, "connecting") == 0) {
    text = "Connecting...";
  } else if (state && strcmp(state, "connected") == 0) {
    text = "Connecting...";
  } else if (state && strcmp(state, "failed") == 0) {
    text = "Failed - try again";
  } else if (state && strcmp(state, "ap_setup") == 0) {
    text = "Scan QR to join Wi-Fi";
  }
  lv_label_set_text(provision_status_label, text);
}

static void provision_qr_wait_begin(const char* reason) {
  provision_qr_waiting = true;
  provision_qr_wait_start_ms = millis();
  Serial.printf("[PROVISION] wait_for_qr begin reason=%s\n", reason ? reason : "unknown");
}

static void provision_qr_wait_clear(const char* reason) {
  if (!provision_qr_waiting) {
    return;
  }
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

static void create_provision_screen_if_needed() {
  if (provision_screen != NULL) {
    return;
  }
  provision_screen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(provision_screen, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_style_bg_color(provision_screen, lv_color_hex(0xDDF7EA), 0);
  lv_obj_set_style_bg_opa(provision_screen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(provision_screen, 0, 0);
  lv_obj_clear_flag(provision_screen, LV_OBJ_FLAG_SCROLLABLE);

  provision_title_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_title_label, "Halo Wi-Fi Setup");
  lv_obj_set_style_text_color(provision_title_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_title_label, LV_ALIGN_TOP_MID, 0, 16);
  lv_obj_add_flag(provision_title_label, LV_OBJ_FLAG_HIDDEN);

  provision_qr = NULL;

  provision_ssid_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_ssid_label, "SSID: -");
  lv_obj_set_style_text_color(provision_ssid_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_ssid_label, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_add_flag(provision_ssid_label, LV_OBJ_FLAG_HIDDEN);

  provision_url_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_url_label, "Open: http://192.168.4.1");
  lv_obj_set_style_text_color(provision_url_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_url_label, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_add_flag(provision_url_label, LV_OBJ_FLAG_HIDDEN);

  provision_status_label = lv_label_create(provision_screen);
  lv_label_set_text(provision_status_label, "Setup Mode");
  lv_obj_set_style_text_color(provision_status_label, lv_color_hex(0x000000), 0);
  lv_obj_align(provision_status_label, LV_ALIGN_BOTTOM_MID, 0, -16);
  lv_obj_add_flag(provision_status_label, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
}

static void create_provision_intro_screen_if_needed() {
  if (provision_intro_screen != NULL) {
    return;
  }
  provision_intro_screen = lv_obj_create(lv_scr_act());
  lv_obj_set_size(provision_intro_screen, LV_HOR_RES, LV_VER_RES);
  lv_obj_clear_flag(provision_intro_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(provision_intro_screen, lv_color_hex(0xF5E9D8), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(provision_intro_screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_border_width(provision_intro_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(provision_intro_screen, 40, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_shadow_color(provision_intro_screen, lv_color_hex(0xD4C4AE), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_shadow_spread(provision_intro_screen, -20, LV_PART_MAIN | LV_STATE_DEFAULT);

  // WiFi icon
  lv_obj_t* wifi_icon = lv_label_create(provision_intro_screen);
  lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(wifi_icon, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_align(wifi_icon, LV_ALIGN_CENTER, 0, -40);

  // Title
  lv_obj_t* title = lv_label_create(provision_intro_screen);
  lv_label_set_text(title, "Wi-Fi Setup");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 20);

  // Tap prompt
  lv_obj_t* prompt = lv_label_create(provision_intro_screen);
  lv_label_set_text(prompt, "Tap to Start");
  lv_obj_set_style_text_font(prompt, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(prompt, lv_color_hex(0x8A7A6A), LV_PART_MAIN);
  lv_obj_align(prompt, LV_ALIGN_CENTER, 0, 55);

  lv_obj_add_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
}

static void show_provision_intro_screen(const char* reason) {
  create_provision_intro_screen_if_needed();
  if (provision_intro_visible) {
    return;
  }
  if (list_container) lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  if (menu_screen) lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  if (meal_result_screen) lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  if (logged_screen) lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
  if (expiry_screen) lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
  if (provision_screen) lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_screen_visible = false;
  lv_obj_clear_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
  provision_intro_visible = true;
  provision_intro_pending = false;
  ui_screen_state = SCREEN_SETTINGS;
  Serial.printf("[PROVISION] intro_show reason=%s\n", reason ? reason : "unknown");
  lv_timer_handler();
}

static void hide_provision_intro_screen(const char* reason) {
  if (!provision_intro_screen || !provision_intro_visible) {
    return;
  }
  lv_obj_add_flag(provision_intro_screen, LV_OBJ_FLAG_HIDDEN);
  provision_intro_visible = false;
  provision_intro_pending = false;
  Serial.printf("[PROVISION] intro_hide reason=%s\n", reason ? reason : "unknown");
}

static void show_provisioning_screen(const char* ssid, const char* password, const char* url) {
  create_provision_screen_if_needed();
  if (!ssid) ssid = "";
  if (!password) password = "";
  if (!url || !url[0]) url = "http://192.168.4.1";
  hide_provision_intro_screen("qr_show");
  provision_qr_wait_clear("qr_shown");

  // Hide other screens
  if (list_container) lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  if (status_screen) lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  if (menu_screen) lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  if (meal_result_screen) lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  if (logged_screen) lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
  if (expiry_screen) lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);

  std::string qr_data = QRDisplay::generateWifiQRData(ssid, password);
  if (provision_qr == NULL) {
    provision_qr = QRDisplay::createQRCodeWidget(provision_screen, qr_data, 200);
    lv_obj_align(provision_qr, LV_ALIGN_CENTER, 0, 0);
  } else {
    QRDisplay::updateQRCodeWidget(provision_qr, qr_data);
  }

  lv_obj_clear_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_screen_visible = true;
  lv_timer_handler();
}

static void hide_provisioning_screen() {
  if (!provision_screen) {
    return;
  }
  lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
  provision_screen_visible = false;
  provision_qr_wait_clear("qr_hidden");

  // Restore list if available
  if (list_container && g_active.count > 0) {
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  lv_timer_handler();
}

static void ship_style_plain_screen(lv_obj_t* screen, lv_color_t bg_color);
static void ship_anim_set_y(void* obj, int32_t v);
static void ship_anim_set_x(void* obj, int32_t v);
static void ship_anim_set_text_opa(void* obj, int32_t v);
static void ship_anim_set_obj_opa(void* obj, int32_t v);
static void ship_anim_set_zoom(void* obj, int32_t v);
static void ship_anim_set_border_opa(void* obj, int32_t v);


#endif // LCD_PROVISION_H

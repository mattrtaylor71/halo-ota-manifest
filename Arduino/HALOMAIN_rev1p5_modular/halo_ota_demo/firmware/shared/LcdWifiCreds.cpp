#include "LcdWifiCreds.h"
#include "Log.h"
#include <Preferences.h>
#include <string.h>

#define NVS_NAMESPACE "lcd_wifi"
#define KEY_SSID     "ssid"
#define KEY_PASS     "pass"

#define LOG_TAG_LCD_WIFI "LCD_WIFI"

static void log_no_nvs_creds_once() {
  static bool logged = false;
  if (logged) {
    return;
  }
  logged = true;
  Serial.println("[LCD_WIFI] no_nvs_creds");
}

bool LcdWifiCreds::hasCreds() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return false;
  }
  char ssid[33];
  size_t len = prefs.getString(KEY_SSID, ssid, sizeof(ssid));
  prefs.end();
  return (len > 0);
}

bool LcdWifiCreds::loadCreds(char* ssid_out, size_t ssid_cap, char* pass_out, size_t pass_cap) {
  if (!ssid_out || ssid_cap < 1 || !pass_out || pass_cap < 1) {
    return false;
  }
  ssid_out[0] = '\0';
  pass_out[0] = '\0';
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    log_no_nvs_creds_once();
    return false;
  }
  size_t ssid_len = prefs.getString(KEY_SSID, ssid_out, ssid_cap);
  size_t pass_len = prefs.getString(KEY_PASS, pass_out, pass_cap);
  prefs.end();
  if (ssid_len == 0 || ssid_len >= ssid_cap) {
    log_no_nvs_creds_once();
    return false;
  }
  // Log at most once per boot (first successful load) to avoid spam while WIFI_PENDING
  static bool load_creds_logged = false;
  if (!load_creds_logged) {
    Serial.printf("[LCD_WIFI] loaded creds from NVS ssid=%s\n", ssid_out);
    load_creds_logged = true;
  }
  return true;
}

bool LcdWifiCreds::saveCreds(const char* ssid, const char* pass) {
  if (!ssid) {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    Serial.println("[LCD_WIFI] FAILED to save creds to NVS");
    return false;
  }
  size_t ssid_len = strlen(ssid);
  size_t pass_len = pass ? strlen(pass) : 0;
  if (ssid_len > 32) ssid_len = 32;
  if (pass_len > 63) pass_len = 63;
  prefs.putString(KEY_SSID, ssid);
  prefs.putString(KEY_PASS, pass ? pass : "");
  prefs.end();
  Serial.printf("[LCD_WIFI] saved creds to NVS ssid_len=%u pass_len=%u\n", (unsigned)ssid_len, (unsigned)pass_len);
  return true;
}

void LcdWifiCreds::clearCreds() {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.remove(KEY_SSID);
    prefs.remove(KEY_PASS);
    prefs.end();
  }
}

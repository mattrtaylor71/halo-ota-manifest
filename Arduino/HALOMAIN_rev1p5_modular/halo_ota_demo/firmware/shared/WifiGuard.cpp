#include "WifiGuard.h"
#include "BuildInfo.h"  // For kFirmwareVersion in boot log
#include "Log.h"
#include <WiFi.h>
#include <esp_system.h>

// Global flag: tracks whether WiFi.begin() was called this boot
// CRITICAL: Prevents "STA not started" errors on post-OTA reboot
static bool g_sta_begin_called = false;

// Reboot kill switch: set true immediately before ESP.restart(); loops must exit early when true
static volatile bool g_rebooting = false;

void halo_sta_mark_begin_called() {
  g_sta_begin_called = true;
  Serial.println("[WIFI_GUARD] WiFi.begin called - STA marked as started");
}

void halo_sta_mark_off() {
  g_sta_begin_called = false;
  Serial.println("[WIFI_GUARD] WiFi turned off - STA marked as stopped");
}

bool halo_sta_begin_called() {
  return g_sta_begin_called;
}

void halo_wifi_guard_boot_log() {
  Serial.printf("[WIFI_GUARD] present version=%s\n", kFirmwareVersion);
}

bool halo_rebooting(void) {
  return g_rebooting;
}

void halo_reboot(const char* reason) {
  g_rebooting = true;
  Serial.printf("[REBOOT] reason=%s g_rebooting=1\n", reason ? reason : "(null)");
  delay(50);  // Allow log to flush
  ESP.restart();
}

void halo_safe_disconnect(const char* reason, bool wifioff, const char* file, int line) {
  // Skip if STA was never started OR WiFi mode is OFF (no STA) - prevents [STA.cpp:524] disconnect(): STA not started!
  wifi_mode_t mode = WiFi.getMode();
  bool sta_active = (mode == WIFI_STA || mode == WIFI_AP_STA);
  if (!g_sta_begin_called || mode == WIFI_OFF || !sta_active) {
    Serial.printf("[WIFI_GUARD] skip_disconnect_sta_not_started reason=%s file=%s line=%d begin_called=%d mode=%d\n",
                  reason ? reason : "(null)", file ? file : "(null)", line,
                  g_sta_begin_called ? 1 : 0, (int)mode);
    return;
  }
  Serial.printf("[WIFI_GUARD] disconnect reason=%s file=%s line=%d begin_called=%d wifioff=%d\n",
                reason ? reason : "(null)", file ? file : "(null)", line,
                g_sta_begin_called ? 1 : 0, wifioff ? 1 : 0);
  WiFi.disconnect(wifioff, false);
  if (wifioff) {
    g_sta_begin_called = false;
  }
}

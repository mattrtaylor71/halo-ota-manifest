#ifndef WIFI_GUARD_H
#define WIFI_GUARD_H

#include <WiFi.h>

/**
 * WifiGuard - Prevents "STA not started" errors after OTA reboot
 * 
 * CRITICAL: Tracks whether WiFi.begin() was called this boot.
 * Only allows WiFi.disconnect() if STA was actually started.
 * 
 * This prevents: [STA.cpp:524] disconnect(): STA not started!
 */

/**
 * Mark that WiFi.begin() was called this boot
 * Call this immediately after every WiFi.begin() call
 */
void halo_sta_mark_begin_called();

/**
 * Mark that WiFi was turned off (STA no longer active)
 * Call this when explicitly setting WiFi.mode(WIFI_OFF) or similar
 */
void halo_sta_mark_off();

/**
 * Safe WiFi disconnect - only disconnects if STA was started this boot
 * 
 * @param reason Short reason string (e.g. "ota_cleanup", "wifi_disconnect")
 * @param wifioff If true, also calls WiFi.mode(WIFI_OFF) after disconnect
 * @param file __FILE__ (use HALO_SAFE_DISCONNECT macro)
 * @param line __LINE__ (use HALO_SAFE_DISCONNECT macro)
 * 
 * Logs: [WIFI_GUARD] disconnect reason=... file=... line=... begin_called=...
 */
void halo_safe_disconnect(const char* reason, bool wifioff, const char* file, int line);

/** Macro to call halo_safe_disconnect with __FILE__ and __LINE__ */
#define HALO_SAFE_DISCONNECT(reason, wifioff) halo_safe_disconnect(reason, wifioff, __FILE__, __LINE__)

/**
 * Print boot log to prove WifiGuard is present in this image
 * Call once early in setup()
 */
void halo_wifi_guard_boot_log();

/**
 * Check if STA begin was called this boot
 */
bool halo_sta_begin_called();

/**
 * Reboot kill switch - set true immediately before ESP.restart().
 * All periodic loops (provisioning, WiFi, OTA) should check this and exit early.
 */
bool halo_rebooting(void);

/**
 * Reboot with reason breadcrumb. Sets g_rebooting=true, logs [REBOOT] reason=... g_rebooting=1, then ESP.restart().
 * Call this instead of ESP.restart() so no network work runs after teardown.
 */
void halo_reboot(const char* reason);

#endif // WIFI_GUARD_H

#ifndef WIFI_UTILS_H
#define WIFI_UTILS_H

#include <WiFi.h>

/**
 * WifiUtils - Safe WiFi operations to prevent post-OTA reboot errors
 * 
 * CRITICAL: These helpers prevent:
 * - [STA.cpp:524] disconnect(): STA not started!
 * - [NetworkClient.cpp:319] setSocketOption errno 9 Bad file number
 * - "wifi:sta is connecting, return error" (driver race when begin() called while still connecting)
 * 
 * Use these instead of direct WiFi.disconnect() calls.
 */

/**
 * Full STA reset to clear driver state before a new connection attempt.
 * Prevents "sta is connecting" race: driver can reject begin() if STA is still in connecting state.
 * Call before WiFi.begin() on each retry so each attempt starts from a clean STA state.
 */
static inline void hardResetSta() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(250);
  WiFi.mode(WIFI_STA);
  delay(250);
}

/**
 * Human-readable string for ESP32 disconnect reason (ARDUINO_EVENT_WIFI_STA_DISCONNECTED info.reason).
 * Common codes: 2=AUTH_EXPIRE, 4=HANDSHAKE_TIMEOUT, 15=4WAY_HANDSHAKE_TIMEOUT, 201=NO_AP_FOUND, 202=AUTH_FAIL.
 */
static inline const char* wifiDisconnectReasonString(uint8_t reason) {
  switch (reason) {
    case 1:  return "UNSPECIFIED";
    case 2:  return "AUTH_EXPIRE";
    case 3:  return "AUTH_LEAVE";
    case 4:  return "ASSOC_EXPIRE";
    case 5:  return "ASSOC_TOOMANY";
    case 6:  return "NOT_AUTHED";
    case 7:  return "NOT_ASSOCED";
    case 8:  return "ASSOC_LEAVE";
    case 9:  return "ASSOC_NOT_AUTHED";
    case 10: return "DISASSOC_PWRCAP_BAD";
    case 11: return "DISASSOC_SUPCHAN_BAD";
    case 13: return "IE_INVALID";
    case 14: return "MIC_FAILURE";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17: return "IE_IN_4WAY_DIFFERS";
    case 18: return "GROUP_CIPHER_INVALID";
    case 19: return "PAIRWISE_CIPHER_INVALID";
    case 20: return "AKMP_INVALID";
    case 21: return "UNSUPP_RSN_IE_VERSION";
    case 22: return "INVALID_RSN_IE_CAP";
    case 23: return "AUTH_8021X_FAIL";
    case 24: return "CIPHER_SUITE_REJECTED";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    default: return "UNKNOWN";
  }
}

/**
 * Check if STA is started (WiFi.begin() was called or mode includes STA)
 * Arduino-ESP32: STA is "started" when mode includes WIFI_STA
 */
static inline bool halo_sta_started() {
  wifi_mode_t m = WiFi.getMode();
  return (m == WIFI_STA) || (m == WIFI_AP_STA);
}

/**
 * Safe WiFi disconnect - only disconnects if STA was started
 * Avoids "STA not started" error on post-OTA reboot
 * 
 * @param wifioff If true, also calls WiFi.mode(WIFI_OFF) after disconnect
 * @param erase If true, erases stored credentials (default: false)
 */
static inline void halo_safe_wifi_disconnect(bool wifioff = false, bool erase = false) {
  // Only disconnect if STA started; avoids STA.cpp:524
  if (halo_sta_started()) {
    // true,erase: wifioff + erase. Use erase=false unless you intend to clear creds.
    WiFi.disconnect(wifioff, erase);
  }
}

/**
 * Safe WiFiClient stop - only stops if client is valid/connected
 * Avoids "Bad file number" socket errors
 * 
 * @param c Pointer to WiFiClient (can be null)
 */
static inline void halo_safe_client_stop(WiFiClient* c) {
  if (!c) return;
  if (*c) {  // Only stop if connected/valid
    c->stop();
  }
}

#endif // WIFI_UTILS_H

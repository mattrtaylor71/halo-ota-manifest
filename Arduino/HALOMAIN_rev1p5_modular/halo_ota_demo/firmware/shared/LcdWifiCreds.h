#ifndef LCD_WIFI_CREDS_H
#define LCD_WIFI_CREDS_H

#include <stddef.h>

/**
 * LcdWifiCreds: Wi-Fi credential storage in NVS for LCD.
 * Namespace: "lcd_wifi", keys: "ssid", "pass".
 * Creds are valid if ssid exists and length > 0.
 */
class LcdWifiCreds {
public:
  /** Returns true if NVS contains valid creds (ssid present and non-empty). */
  static bool hasCreds();

  /**
   * Load SSID and password into provided buffers (null-terminated).
   * Returns true if creds were loaded; false if missing or error.
   */
  static bool loadCreds(char* ssid_out, size_t ssid_cap, char* pass_out, size_t pass_cap);

  /**
   * Save SSID and password to NVS.
   * Returns true on success.
   */
  static bool saveCreds(const char* ssid, const char* pass);

  /** Clear stored creds from NVS. */
  static void clearCreds();
};

#endif // LCD_WIFI_CREDS_H

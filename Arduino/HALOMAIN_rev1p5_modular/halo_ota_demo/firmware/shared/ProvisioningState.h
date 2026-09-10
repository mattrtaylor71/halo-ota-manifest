#include <mutex>
#ifndef PROVISIONING_STATE_H
#define PROVISIONING_STATE_H

#include <Arduino.h>

/**
 * ProvisioningState: Manages provisioning state machine and NVS storage
 * 
 * States:
 * - UNPROVISIONED: Initial state, no home Wi-Fi credentials
 * - AP_SETUP: SoftAP running, waiting for credentials
 * - CONNECTING_HOME_WIFI: STA connecting to home Wi-Fi
 * - CONNECTED: STA connected, provisioning complete
 * - ERROR: Provisioning error, retry needed
 */
namespace ProvisioningState {
  enum State {
    STATE_UNPROVISIONED = 0,
    STATE_AP_SETUP = 1,
    STATE_CONNECTING_HOME_WIFI = 2,
    STATE_CONNECTED = 3,
    STATE_ERROR = 4
  };
  
  // Initialize (call once at startup)
  void init();
  
  // Get current state
  State getState();
  
  // Set state (also updates NVS if needed)
  void setState(State state);
  
  // Get state as string (for logging/API)
  const char* getStateString(State state);
  const char* getStateString();  // Current state
  
  // NVS: Home Wi-Fi credentials
  bool loadHomeWifiCreds(char* ssid, size_t ssid_sz, char* password, size_t password_sz);
  void saveHomeWifiCreds(const char* ssid, const char* password);
  void clearHomeWifiCreds();
  
  // NVS: AP credentials (SoftAP SSID/password)
  bool loadApCreds(char* ssid, size_t ssid_sz, char* password, size_t password_sz);
  void saveApCreds(const char* ssid, const char* password);
  void clearApCreds();
  
  // NVS: Provisioning flag
  bool isProvisioned();
  void setProvisioned(bool value);
  
  // NVS: Owner ID
  bool loadOwnerId(char* owner_id, size_t owner_id_sz);
  void saveOwnerId(const char* owner_id);
  void clearOwnerId();
  
  // NVS: Owner setup code (short code entered in portal)
  // POSIX TZ string supplied by the backend at claim time. Absent until the
  // backend sends one; callers fall back to HALO_DEFAULT_TZ.
  enum class TimezoneStatus { Present, Absent, Unknown };
  // Hold this lock across configuration proof and dependent time/credit work.
  // Acquire any time mutex after this one. Unknown persists until normal boot.
  std::recursive_mutex& timezoneMutex();
  TimezoneStatus loadTimezoneStatus(char* tz, size_t tz_sz);
  bool loadTimezone(char* tz, size_t tz_sz);
  bool saveTimezone(const char* tz);

  bool loadOwnerCode(char* owner_code, size_t owner_code_sz);
  void saveOwnerCode(const char* owner_code);
  void clearOwnerCode();
  
  // Generate device ID (MAC-based)
  void getDeviceId(char* device_id, size_t device_id_sz);
  
  // Generate AP SSID (Trepo-Halo-XXXX-YYYY where YYYY is session-unique)
  void generateApSsid(char* ap_ssid, size_t ap_ssid_sz);
  
  // Generate random password (10 chars alphanumeric)
  void generateRandomPassword(char* password, size_t password_sz);
}

#endif // PROVISIONING_STATE_H

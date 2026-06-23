#ifndef PROVISIONING_MANAGER_H
#define PROVISIONING_MANAGER_H

#include <Arduino.h>
#include <string.h>  // For strncpy
#include "ProvisioningState.h"

class UartProto;  // Forward declaration

/**
 * ProvisioningManager: Handles SoftAP and HTTP provisioning server
 * 
 * Responsibilities:
 * - Start/stop SoftAP
 * - HTTP server endpoints (/info, /scan, /wifi, /status, /user-id)
 * - Provisioning state machine
 * - Home Wi-Fi connection
 * - UART status messages to LCD
 */
class ProvisioningManager {
public:
  ProvisioningManager();
  ~ProvisioningManager();
  
  // Initialize (call once at startup)
  void init(UartProto* uart_proto);
  
  // Update state machine (call in loop())
  void update();
  
  // Start Setup Mode (SoftAP + HTTP server)
  bool startSetupMode();
  
  // Stop Setup Mode (stop SoftAP, stop HTTP server)
  void stopSetupMode();
  
  // Check if Setup Mode is active
  bool isSetupModeActive() const { return setup_mode_active; }
  
  
  // Get current AP SSID (for QR code)
  const char* getApSsid() const { return ap_ssid; }
  
  // Get current AP password (for QR code)
  const char* getApPassword() const { return ap_password; }
  
  // Get device ID
  const char* getDeviceId() const { return device_id; }
  
  // Set target home SSID (for /status reporting, called when credentials loaded)
  void setTargetHomeSsid(const char* ssid) {
    if (ssid) {
      strncpy(target_home_ssid, ssid, sizeof(target_home_ssid) - 1);
      target_home_ssid[sizeof(target_home_ssid) - 1] = '\0';
    } else {
      target_home_ssid[0] = '\0';
    }
  }
  
  // Get last error (for /status endpoint)
  const char* getLastError() const { return last_error; }
  
  // Set last error (for /status endpoint) - used by static handlers
  void setLastError(const char* error);
  
  // Get/set owner_id_set_ms (for grace period extension)
  unsigned long getOwnerIdSetMs() const { return owner_id_set_ms; }
  void setOwnerIdSetMs(unsigned long ms) { owner_id_set_ms = ms; }

  // Reset claim state to allow retry (e.g., after SoftAP shutdown frees memory)
  void resetClaimForRetry();

  // App-driven provisioning session tracking
  void noteProvisionClientRequest(const char* client_type,
                                  const char* app_version,
                                  bool app_session);
  bool isAppSessionActive(unsigned long now_ms = 0) const;
  const char* getLastProvisionClient() const { return last_provision_client; }
  const char* getLastAppVersion() const { return last_app_version; }
  
  // Start home Wi-Fi connection (called by static handlers)
  void startHomeWifiConnectPublic(const char* ssid, const char* password) {
    startHomeWifiConnect(ssid, password);
  }
  
  // Friend declarations for static handlers
  friend void handleInfo();
  friend void handleScan();
  friend void handleWifiPost();
  friend void handleStatus();
  friend void handleUserIdPost();
  friend void handleRoot();
  
private:
  UartProto* uart_proto;
  bool setup_mode_active;
  char ap_ssid[64];
  char ap_password[16];
  char device_id[32];
  char target_home_ssid[64];  // Current target/home SSID (for /status reporting)
  
  // HTTP server (ESPAsyncWebServer or WebServer)
  void* http_server;  // Opaque pointer to avoid including headers here
  
  // State machine
  unsigned long connecting_start_ms;
  unsigned long connected_verified_ms;  // When STA connection was verified stable (first WL_CONNECTED seen)
  unsigned long last_disconnect_ms;     // First moment WiFi left WL_CONNECTED after connected_verified_ms was set (0 = currently connected). Used for AP_STA flap tolerance so a brief STA drop doesn't restart the verify/fail path.
  unsigned long connected_state_set_ms;  // When STATE_CONNECTED was set (for minimum delay before SoftAP shutdown)
  unsigned long owner_id_set_ms;  // When owner_id was set (extends SoftAP grace period)
  // Connection attempt constants (single place for timeout + retry)
  static const unsigned long CONNECTING_TIMEOUT_MS = 45000;   // 45 seconds per attempt
  static const unsigned int CONNECT_MAX_RETRIES = 1;          // One retry = 2 attempts total (attempt 1/2, 2/2)
  static const unsigned long CONNECTION_VERIFY_DELAY_MS = 2000;  // 2 seconds to verify connection stable
  static const unsigned long FLAP_TOLERANCE_MS = 5000;  // Once connected_verified_ms is set, ignore brief STA drops (AP_STA radio sharing) for this long; only treat as a real drop after continuous disconnect > this window.
  static const unsigned long SOFTAP_GRACE_PERIOD_MS = 150000;  // 150s after connect (or until owner_id set). Covers the app's ~97.5s /status poll window plus connect+claim time so the app can always read the terminal /status (success or failed) before the server tears down.
  static const unsigned long STA_FAILURE_THRESHOLD = 3;       // Re-enter Setup Mode after N consecutive failures (if used)
  unsigned int sta_failure_count;   // Consecutive STA connection failures (for logging)
  unsigned int connect_attempt;     // Current attempt within this submit (0 = first, 1 = retry; max CONNECT_MAX_RETRIES)
  
  // Error tracking
  char last_error[128];  // Last error message for /status endpoint

  // App-driven provisioning session state
  unsigned long last_app_request_ms;
  char last_provision_client[32];
  char last_app_version[32];
  static const unsigned long APP_SESSION_ACTIVE_MS = 45000;
  
  // Owner claim state (setup code -> owner_id)
  unsigned long last_claim_attempt_ms;
  unsigned int claim_attempts;
  bool claim_in_progress;
  bool claim_completed;
  
  // Internal helpers
  bool startSoftAP();
  void stopSoftAP();
  bool startHttpServer();
  void stopHttpServer();
  void startHomeWifiConnect(const char* ssid, const char* password);
  void sendProvisionStatus(const char* state_str);
  void resetOwnerClaimState();
  bool tryClaimOwnerId();
};

#endif // PROVISIONING_MANAGER_H

#ifndef HEALTH_GATE_H
#define HEALTH_GATE_H

#include <Arduino.h>

/**
 * HealthGate: State machine for firmware health validation.
 * 
 * Phase 1: Split rollback cancellation from operational readiness.
 * 
 * Rollback cancellation (rollback_eligible):
 *   - Local-only criteria, does NOT depend on WiFi/manifest
 *   - uptime >= 10s (configurable)
 *   - main loop is running
 *   - no crash reset reason (panic/wdt)
 *   - actual local UART driver initialized
 * 
 * Operational readiness (cloud_ready):
 *   - Separate flag for cloud connectivity
 *   - WiFi connected AND manifest fetched
 * 
 * State machine:
 *   - INIT: Gate closed, checking criteria
 *   - ELIGIBLE: Rollback criteria met, eligible to mark valid
 *   - VALID: Firmware marked valid (rollback cancelled)
 *   - FAILED: Health check failed (not used in Phase 1)
 */
class HealthGate {
public:
  enum State {
    STATE_INIT,
    STATE_ELIGIBLE,
    STATE_VALID,
    STATE_FAILED
  };
  
  HealthGate();
  
  // Update health gate (call from loop())
  void update();
  
  // Get current state
  State getState() const { return state; }
  
  // Check if gate is open (eligible to mark valid)
  bool isEligible() const { return state == STATE_ELIGIBLE; }
  
  // Check if firmware is marked valid
  bool isValid() const { return state == STATE_VALID; }
  
  // Get uptime when gate became eligible (0 if not eligible)
  unsigned long getEligibleUptime() const { return eligible_uptime_ms; }
  
  // Check if rollback eligible (local criteria met)
  bool isRollbackEligible() const { return rollback_eligible; }
  
  // Check if cloud ready (WiFi + manifest)
  bool isCloudReady() const { return cloud_ready; }
  
  // Mark Wi-Fi as connected (call when Wi-Fi connects)
  void markWifiConnected();
  
  // Mark Wi-Fi as disconnected (call when Wi-Fi disconnects; keeps HEALTH wifi= in sync with live state)
  void markWifiDisconnected();
  
  // Mark manifest fetch as successful (call when manifest fetch succeeds)
  void markManifestFetched();
  
  // Mark manifest as disabled/placeholder (call when manifest URL is placeholder/disabled)
  void markManifestDisabled();
  
  // Publish current local UART driver readiness; peer sync is separate
  void markUartInitialized(bool ready = true);
  
  // Mark UART sync as established (call when SYNC handshake completes)
  void markUartSyncEstablished();
  
  // Check if we can mark valid (expose for logging/debugging)
  bool canMarkValid() const;
  // Local runtime health evidence, independent of connectivity and OTA state.
  bool hasRuntimeReadiness() const;
  
  // OTA verify state for [OTA_VERIFY] boot line
  bool getPendingVerify() const { return pending_verify; }
  bool getMarkedValid() const { return marked_valid; }

  // OTA test hook: skip mark-valid (test builds only)
  void setSkipMarkValid(bool value) { skip_mark_valid = value; }
  bool getSkipMarkValid() const { return skip_mark_valid; }
  
  // Reset gate (for testing)
  void reset();

private:
  State state;
  unsigned long boot_time_ms;
  unsigned long eligible_uptime_ms;
  
  // Rollback eligibility (local criteria only)
  bool rollback_eligible;
  bool uart_initialized;
  bool uart_sync_established;
  
  // Cloud readiness (WiFi + manifest)
  bool wifi_connected_once;
  bool manifest_fetched_once;
  bool manifest_disabled;  // True if manifest URL is placeholder/disabled
  bool cloud_ready;
  
  // Periodic status logging
  unsigned long last_status_log_ms;
  static const unsigned long STATUS_LOG_INTERVAL_MS = 5000;  // Log every 5 seconds
  
  // Phase 1 rollback criteria: uptime >= OTA_VERIFY_MIN_UPTIME_MS AND main loop running AND no crash AND (actual local UART initialized)
  static const unsigned long ELIGIBILITY_UPTIME_MS = 10000;
  
  // Two-phase OTA verify: min uptime before we can mark valid (tunable)
  static const unsigned long OTA_VERIFY_MIN_UPTIME_MS = 10000;   // 10 seconds
  static const unsigned long OTA_VERIFY_LOCAL_TIMEOUT_MS = 30000; // local failure only; never Wi-Fi timeout
  
  // Legacy alias for code that referenced STABILITY_MS
  static const unsigned long STABILITY_MS = OTA_VERIFY_MIN_UPTIME_MS;
  
  // OTA state tracking (exposed via getPendingVerify/getMarkedValid)
  bool pending_verify;
  bool marked_valid;
  bool validation_attempted;
  bool skip_mark_valid;
  
  void checkRollbackCriteria();
  void checkCloudReadiness();
  void updateOtaState();  // Check if running partition is in PENDING_VERIFY
  void markValid();
  bool confirmValid();
  void logState();
  void logPeriodicStatus();
  
  // Helper: Compute rollback eligibility (only true when pending_verify and all criteria met)
  bool computeRollbackEligible(unsigned long uptime_ms) const;
};

#endif // HEALTH_GATE_H

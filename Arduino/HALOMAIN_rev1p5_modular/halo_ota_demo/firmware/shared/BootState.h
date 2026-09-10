#ifndef BOOT_STATE_H
#define BOOT_STATE_H

#include <Arduino.h>
#include <Preferences.h>

/**
 * BootState: Manages boot count and OTA apply tracking in NVS (Non-Volatile Storage)
 * 
 * Replaces RTC_DATA_ATTR which doesn't persist across ESP.restart() on ESP32-S3.
 * Uses Arduino Preferences library (NVS backend) for reliable persistence.
 */
namespace BootState {
  // Initialize BootState (call once at startup)
  void init();
  
  // End BootState (call to close NVS, optional)
  void end();
  
  // Boot count management
  uint32_t getBootCount();
  uint32_t nextBootCount();  // Increments boot count in NVS, closes/reopens to verify, returns verified count
  
  // OTA apply tracking
  bool getLastAppliedVersion(char* out, size_t out_sz);
  int64_t getLastOtaApplyTimestamp();  // Returns timestamp in milliseconds (0 if not set)
  void setLastOtaApply(const char* version, int64_t timestamp_ms);
  void clearLastOtaApply();
  
  // Reboot loop detection (field safety)
  // Returns true if reboot loop detected (too many reboots in short time)
  // Returns false if dev override is enabled (dev_disable_reset_guard=1 in NVS)
  bool checkRebootLoop(uint32_t max_reboots, uint32_t window_ms);
  // Record current boot timestamp
  void recordBootTimestamp();
  // Get reboot count in last window_ms milliseconds
  uint32_t getRebootCountInWindow(uint32_t window_ms);
  // Clear reboot history (call after stable uptime)
  void clearRebootHistory();
  // Check if dev override is enabled (dev_disable_reset_guard=1)
  bool isDevOverrideEnabled();
}

#endif // BOOT_STATE_H

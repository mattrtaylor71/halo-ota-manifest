#include "BootState.h"
#include <Preferences.h>
#include <string.h>
#include <esp_timer.h>

namespace BootState {
  static const char* NVS_NAMESPACE = "halo";
  static const char* KEY_BOOT_COUNT = "boot_count_u32";
  static const char* KEY_LAST_APPLIED_VERSION = "last_applied_ver";
  static const char* KEY_LAST_OTA_TIMESTAMP = "last_apply_epoch_ms";
  static const char* KEY_BOOT_TIMESTAMPS = "boot_ts_array";  // Array of boot timestamps (circular buffer)
  static const char* KEY_BOOT_TS_INDEX = "boot_ts_idx";      // Current index in circular buffer
  static const char* KEY_DEV_DISABLE_RESET_GUARD = "dev_dis_rstgrd";  // Dev override: disable reboot loop guard
  
  static Preferences prefs;
  static bool initialized = false;
  
  // Reboot loop detection: circular buffer of boot sequence numbers
  // Store last N boot_count values (monotonically increasing NVS counter)
  // We use a circular buffer stored as bytes in NVS
  static const uint32_t MAX_BOOT_TIMESTAMPS = 10;  // Track last 10 boots
  static const size_t BOOT_TS_ARRAY_SIZE = MAX_BOOT_TIMESTAMPS * sizeof(int64_t);
  
  void init() {
    if (initialized) return;
    
    bool success = prefs.begin(NVS_NAMESPACE, false);  // false = read-write mode
    if (!success) {
      // Log error if Serial is available (may not be at early init)
      if (Serial) {
        Serial.println("[BOOT_STATE] ERROR: Failed to open NVS namespace 'halo'");
      }
      return;  // Will retry on next call
    }
    initialized = true;
  }
  
  void end() {
    if (initialized) {
      prefs.end();
      initialized = false;
    }
  }
  
  uint32_t getBootCount() {
    if (!initialized) init();
    return prefs.getUInt(KEY_BOOT_COUNT, 0);
  }
  
  uint32_t nextBootCount() {
    // Open NVS namespace
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {
      Serial.println("[BOOTCOUNT] ERROR: Failed to open NVS namespace 'halo'");
      return 0;
    }
    
    // Check if key existed before increment
    bool exists = prefs.isKey(KEY_BOOT_COUNT);
    
    // Read previous value
    uint32_t prev = prefs.getUInt(KEY_BOOT_COUNT, 0);
    
    // Increment
    uint32_t next = prev + 1;
    
    // Write new value
    bool write_ok = prefs.putUInt(KEY_BOOT_COUNT, next);
    
    // Close NVS to ensure commit
    prefs.end();
    
    if (!write_ok) {
      Serial.print("[BOOTCOUNT] ERROR: Failed to write boot_count to NVS (prev=");
      Serial.print(prev);
      Serial.print(", next=");
      Serial.print(next);
      Serial.println(")");
      return next;  // Return incremented value even if write failed
    }
    
    // Re-open NVS and verify the write persisted
    if (!prefs.begin(NVS_NAMESPACE, true)) {  // true = read-only for verification
      Serial.println("[BOOTCOUNT] ERROR: Failed to reopen NVS for verification");
      return next;  // Return what we wrote even if verification failed
    }
    
    uint32_t verify = prefs.getUInt(KEY_BOOT_COUNT, 0);
    prefs.end();
    
    // Log the operation
    Serial.print("[BOOTCOUNT] existed=");
    Serial.print(exists ? 1 : 0);
    Serial.print(" prev=");
    Serial.print(prev);
    Serial.print(" next=");
    Serial.print(next);
    Serial.print(" verify=");
    Serial.println(verify);
    
    // Verify the readback matches what we wrote
    if (verify != next) {
      Serial.print("[BOOTCOUNT] WARN: Verification mismatch: wrote=");
      Serial.print(next);
      Serial.print(", readback=");
      Serial.println(verify);
    }
    
    return verify;
  }
  
  // Legacy alias for backward compatibility (deprecated - use nextBootCount)
  uint32_t incrementBootCount() {
    return nextBootCount();
  }
  
  bool getLastAppliedVersion(char* out, size_t out_sz) {
    if (!initialized) init();
    if (!out || out_sz == 0) return false;
    
    size_t len = prefs.getString(KEY_LAST_APPLIED_VERSION, out, out_sz);
    return (len > 0);
  }
  
  int64_t getLastOtaApplyTimestamp() {
    if (!initialized) init();
    // Use getBytes to read int64_t (Preferences doesn't have getInt64)
    int64_t timestamp = 0;
    size_t len = prefs.getBytesLength(KEY_LAST_OTA_TIMESTAMP);
    if (len == sizeof(int64_t)) {
      prefs.getBytes(KEY_LAST_OTA_TIMESTAMP, &timestamp, sizeof(int64_t));
    }
    return timestamp;
  }
  
  void setLastOtaApply(const char* version, int64_t timestamp_ms) {
    if (!initialized) init();
    if (version) {
      prefs.putString(KEY_LAST_APPLIED_VERSION, version);
    }
    prefs.putBytes(KEY_LAST_OTA_TIMESTAMP, &timestamp_ms, sizeof(int64_t));
  }
  
  void clearLastOtaApply() {
    if (!initialized) init();
    prefs.remove(KEY_LAST_APPLIED_VERSION);
    prefs.remove(KEY_LAST_OTA_TIMESTAMP);
    // Note: We do NOT remove boot_count_u32 - it should persist across all operations
  }
  
  void recordBootTimestamp() {
    if (!initialized) init();
    
    // Use NVS boot_count as a monotonically increasing sequence number.
    // esp_timer_get_time() resets to 0 on every boot, making all previous
    // timestamps appear within any detection window (false positive).
    int64_t current_ts = (int64_t)prefs.getUInt(KEY_BOOT_COUNT, 0);
    
    // Read existing timestamps array
    uint8_t ts_array[BOOT_TS_ARRAY_SIZE];
    size_t array_len = prefs.getBytesLength(KEY_BOOT_TIMESTAMPS);
    if (array_len == BOOT_TS_ARRAY_SIZE) {
      prefs.getBytes(KEY_BOOT_TIMESTAMPS, ts_array, BOOT_TS_ARRAY_SIZE);
    } else {
      // Initialize array to zeros
      memset(ts_array, 0, BOOT_TS_ARRAY_SIZE);
    }
    
    // Read current index
    uint32_t idx = prefs.getUInt(KEY_BOOT_TS_INDEX, 0);
    
    // Write current timestamp at current index
    memcpy(&ts_array[idx * sizeof(int64_t)], &current_ts, sizeof(int64_t));
    
    // Advance index (circular buffer)
    idx = (idx + 1) % MAX_BOOT_TIMESTAMPS;
    
    // Write back to NVS
    prefs.putBytes(KEY_BOOT_TIMESTAMPS, ts_array, BOOT_TS_ARRAY_SIZE);
    prefs.putUInt(KEY_BOOT_TS_INDEX, idx);
  }
  
  uint32_t getRebootCountInWindow(uint32_t window_ms) {
    if (!initialized) init();
    
    // Use boot_count sequence numbers. Reinterpret window_ms as: each boot
    // cycle takes ~1 second minimum, so window_ms/1000 gives the maximum
    // number of boot cycles in the detection window.
    // With checkRebootLoop(3, 30000): detect 3+ reboots within 30 boot cycles.
    int64_t current_ts = (int64_t)prefs.getUInt(KEY_BOOT_COUNT, 0);
    int64_t window_start = current_ts - (int64_t)(window_ms / 1000);
    
    // Read timestamps array
    uint8_t ts_array[BOOT_TS_ARRAY_SIZE];
    size_t array_len = prefs.getBytesLength(KEY_BOOT_TIMESTAMPS);
    if (array_len != BOOT_TS_ARRAY_SIZE) {
      return 0;  // No timestamps recorded yet
    }
    prefs.getBytes(KEY_BOOT_TIMESTAMPS, ts_array, BOOT_TS_ARRAY_SIZE);
    
    // Count timestamps within window
    uint32_t count = 0;
    for (uint32_t i = 0; i < MAX_BOOT_TIMESTAMPS; i++) {
      int64_t ts;
      memcpy(&ts, &ts_array[i * sizeof(int64_t)], sizeof(int64_t));
      if (ts > 0 && ts >= window_start && ts <= current_ts) {
        count++;
      }
    }
    
    return count;
  }
  
  bool isDevOverrideEnabled() {
    if (!initialized) init();
    // Check NVS for dev override flag (default: false/0 = guard enabled)
    return (prefs.getUInt(KEY_DEV_DISABLE_RESET_GUARD, 0) == 1);
  }
  
  bool checkRebootLoop(uint32_t max_reboots, uint32_t window_ms) {
    // Dev override: if dev_disable_reset_guard=1, skip reboot loop detection
    if (isDevOverrideEnabled()) {
      return false;  // Guard disabled - always return false (no loop detected)
    }
    
    uint32_t reboot_count = getRebootCountInWindow(window_ms);
    return (reboot_count >= max_reboots);
  }
  
  void clearRebootHistory() {
    if (!initialized) init();
    
    // Clear the timestamps array and reset index
    uint8_t ts_array[BOOT_TS_ARRAY_SIZE];
    memset(ts_array, 0, BOOT_TS_ARRAY_SIZE);
    
    // Write cleared array back to NVS
    bool success = prefs.putBytes(KEY_BOOT_TIMESTAMPS, ts_array, BOOT_TS_ARRAY_SIZE);
    if (success) {
      prefs.putUInt(KEY_BOOT_TS_INDEX, 0);
    }
    // Note: If NVS write fails, we silently continue (safe failure)
  }
}

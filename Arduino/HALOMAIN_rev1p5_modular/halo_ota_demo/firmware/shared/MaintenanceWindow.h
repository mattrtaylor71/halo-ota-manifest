#ifndef MAINTENANCE_WINDOW_H
#define MAINTENANCE_WINDOW_H

#include <Arduino.h>
#include <ArduinoJson.h>

/**
 * MaintenanceWindow - Scheduled maintenance window (OTA only during window).
 * Shared by Sense and LCD. Persisted in NVS namespace "mw".
 */
struct MaintenanceWindow {
  bool scheduled;
  uint64_t start_epoch;      // seconds UTC
  uint32_t duration_sec;     // default 600 (10 min)
  char request_id[64];
  uint32_t grace_before_sec; // e.g. 60
  uint32_t grace_after_sec;  // e.g. 600

  MaintenanceWindow();

  /** Load from NVS. Returns true if loaded and scheduled. */
  bool loadFromNvs();
  /** Save to NVS. */
  void saveToNvs() const;
  /** Clear and remove from NVS. */
  void clear();

  /** True if (start_epoch - grace_before_sec) <= now <= (start_epoch + duration_sec + grace_after_sec). */
  bool isWithinWindow(uint64_t now_epoch) const;
  /** Epoch to wake: start_epoch - grace_before_sec, clamped to >= now_epoch + 5. Returns 0 if no schedule or already past. */
  uint64_t nextWakeEpoch(uint64_t now_epoch) const;
  /** Wake epoch for deep sleep: max(now + MIN_WAKE_LEAD_SEC, start_epoch - WAKE_EARLY_SEC), clamped to >= now + 5. Returns 0 if no schedule or expired. */
  uint64_t nextWakeEpochForSleep(uint64_t now_epoch, uint32_t wake_early_sec = 15, uint32_t min_wake_lead_sec = 5) const;
  /** True iff we are within the active window (same as isWithinWindow). */
  bool shouldEnterMaintenanceMode(uint64_t now_epoch) const;
  /** True if now_epoch > start_epoch + duration_sec + grace_after_sec. */
  bool hasExpired(uint64_t now_epoch) const;

  /**
   * Parse JSON object with keys: start_epoch, duration_sec, request_id, enabled, grace_before_sec, grace_after_sec.
   * If enabled and start_epoch >= (now_epoch + min_future_sec), fills this struct and returns true.
   */
  bool setFromJson(const char* json, size_t json_len, uint64_t now_epoch, uint32_t min_future_sec = 60);

  /** Overload for JsonObject (e.g. from MQTT desired["maintenance"]). */
  bool setFromJson(JsonObject& obj, uint64_t now_epoch, uint32_t min_future_sec = 60);

  /** Parse and store from wire (e.g. UART MAINT_WINDOW_SET). No time validation. Returns true if valid JSON. */
  bool storeFromWire(const char* json, size_t json_len);
};

/** Global flag: set when schedule was updated from MQTT and should be sent to LCD. Cleared after send. */
bool maintenance_schedule_pending_sync_to_lcd();

void set_maintenance_schedule_pending_sync_to_lcd(bool value);

#endif // MAINTENANCE_WINDOW_H

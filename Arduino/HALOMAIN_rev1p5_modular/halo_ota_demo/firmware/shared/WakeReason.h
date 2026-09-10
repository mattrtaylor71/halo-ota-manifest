#ifndef WAKE_REASON_H
#define WAKE_REASON_H

#include <Arduino.h>

enum class WakeReason {
  COLD_BOOT,
  USER_INTERACTION,
  MAINTENANCE,
  OTHER_TIMER,
  UNKNOWN
};

/** Return string for logging. */
const char* wakeReasonToString(WakeReason r);

/**
 * Determine wake reason (Sense).
 * Uses esp_sleep_get_wakeup_cause(), MaintenanceWindow, and current epoch.
 * Call after MaintenanceWindow::loadFromNvs() if using maintenance.
 */
WakeReason senseWakeReason(int wakeup_cause, uint64_t now_epoch, bool maintenance_window_scheduled, bool within_maintenance_window);

#endif // WAKE_REASON_H

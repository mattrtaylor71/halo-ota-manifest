#include "WakeReason.h"
#include <esp_sleep.h>

const char* wakeReasonToString(WakeReason r) {
  switch (r) {
    case WakeReason::COLD_BOOT: return "COLD_BOOT";
    case WakeReason::USER_INTERACTION: return "USER_INTERACTION";
    case WakeReason::MAINTENANCE: return "MAINTENANCE";
    case WakeReason::OTHER_TIMER: return "OTHER_TIMER";
    case WakeReason::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

WakeReason senseWakeReason(int wakeup_cause, uint64_t now_epoch, bool maintenance_window_scheduled, bool within_maintenance_window) {
  if (wakeup_cause == ESP_SLEEP_WAKEUP_UNDEFINED || wakeup_cause == ESP_RST_POWERON) {
    return WakeReason::COLD_BOOT;
  }
  if (wakeup_cause == ESP_SLEEP_WAKEUP_TIMER) {
    if (maintenance_window_scheduled && within_maintenance_window) {
      return WakeReason::MAINTENANCE;
    }
    return WakeReason::OTHER_TIMER;
  }
  if (wakeup_cause == ESP_SLEEP_WAKEUP_EXT0 || wakeup_cause == ESP_SLEEP_WAKEUP_EXT1 ||
      wakeup_cause == ESP_SLEEP_WAKEUP_GPIO) {
    return WakeReason::USER_INTERACTION;
  }
  return WakeReason::UNKNOWN;
}

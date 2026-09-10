#ifndef AWAKE_SUPERVISOR_H
#define AWAKE_SUPERVISOR_H

#include <Arduino.h>

/**
 * AwakeSupervisor - State-aware awake reason and idle timeout.
 * If no allowed reason is active for IDLE_TIMEOUT_MS, trigger sleep.
 * Any reason held longer than MAX_REASON_MS forces teardown + sleep.
 */
enum AwakeReason {
  AWAKE_USER_ACTIVE   = (1 << 0),
  AWAKE_OP_OTA        = (1 << 1),
  AWAKE_OP_PROVISION  = (1 << 2),
  AWAKE_OP_CAPTURE    = (1 << 3),
  AWAKE_OP_API        = (1 << 4),
  AWAKE_OP_MAINTENANCE = (1 << 5),
  AWAKE_DEBUG_OVERRIDE = (1 << 6),
};

class AwakeSupervisor {
public:
  /** Idle timeout: if no reason active for this long, trigger sleep (ms). */
  static const unsigned long DEFAULT_IDLE_TIMEOUT_MS = 30000;
  /** Max duration for OTA (ms); then force teardown. */
  static const unsigned long MAX_OTA_MS = 15 * 60 * 1000;
  /** Max duration for provisioning (ms). */
  static const unsigned long MAX_PROVISION_MS = 5 * 60 * 1000;
  /** Max duration for API (ms). */
  static const unsigned long MAX_API_MS = 60 * 1000;
  /** Log interval for [AWAKE] line (ms). */
  static const unsigned long LOG_INTERVAL_MS = 5000;

  AwakeSupervisor(unsigned long idle_timeout_ms = DEFAULT_IDLE_TIMEOUT_MS);

  void setReason(AwakeReason reason, bool on);
  void setReasonMask(uint32_t mask);  // replace active set
  bool hasReason(AwakeReason reason) const { return (reasons_ & (uint32_t)reason) != 0; }
  uint32_t getReasons() const { return reasons_; }

  /**
   * Call from main loop. Returns true if sleep was requested (caller should perform sleep).
   * now_ms: current millis().
   * wake_reason_str: for logging (e.g. "MAINTENANCE", "COLD_BOOT").
   */
  bool tick(unsigned long now_ms, const char* wake_reason_str = nullptr);

  /** Call when entering maintenance mode (sets OP_MAINTENANCE, records start). */
  void enterMaintenance();
  /** Call when leaving maintenance (clears OP_MAINTENANCE). */
  void exitMaintenance();

  /** Sleep was requested by supervisor (idle or max duration). */
  bool sleepRequested() const { return sleep_requested_; }
  void clearSleepRequested() { sleep_requested_ = false; }

  void setIdleTimeoutMs(unsigned long ms) { idle_timeout_ms_ = ms; }
  unsigned long getIdleTimeoutMs() const { return idle_timeout_ms_; }

private:
  uint32_t reasons_;
  unsigned long idle_timeout_ms_;
  unsigned long last_reason_change_ms_;
  unsigned long last_log_ms_;
  bool sleep_requested_;
  unsigned long reason_start_ms_[7];  // one per bit up to DEBUG_OVERRIDE
  unsigned long idle_since_ms_;
};

#endif // AWAKE_SUPERVISOR_H

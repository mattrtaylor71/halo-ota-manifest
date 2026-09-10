#include "AwakeSupervisor.h"
#include "Log.h"
#include <string.h>
#include <stdio.h>

AwakeSupervisor::AwakeSupervisor(unsigned long idle_timeout_ms)
  : reasons_(0),
    idle_timeout_ms_(idle_timeout_ms),
    last_reason_change_ms_(0),
    last_log_ms_(0),
    sleep_requested_(false),
    idle_since_ms_(0) {
  memset(reason_start_ms_, 0, sizeof(reason_start_ms_));
}

void AwakeSupervisor::setReason(AwakeReason reason, bool on) {
  uint32_t r = (uint32_t)reason;
  unsigned long now = millis();
  if (on) {
    if ((reasons_ & r) == 0) {
      reasons_ |= r;
      int bit = 0;
      for (; bit < 7 && (r != (1u << bit)); bit++) { }
      if (bit < 7) {
        reason_start_ms_[bit] = now;
      }
      last_reason_change_ms_ = now;
    }
  } else {
    if (reasons_ & r) {
      reasons_ &= ~r;
      last_reason_change_ms_ = now;
      if (reasons_ == 0) {
        idle_since_ms_ = now;
      }
    }
  }
  if (reasons_ != 0) {
    idle_since_ms_ = now;
  }
}

void AwakeSupervisor::setReasonMask(uint32_t mask) {
  reasons_ = mask;
  unsigned long now = millis();
  last_reason_change_ms_ = now;
  if (reasons_ != 0) {
    idle_since_ms_ = now;
  }
}

void AwakeSupervisor::enterMaintenance() {
  setReason(AWAKE_OP_MAINTENANCE, true);
}

void AwakeSupervisor::exitMaintenance() {
  setReason(AWAKE_OP_MAINTENANCE, false);
}

static void reason_str(uint32_t r, char* buf, size_t buf_len) {
  size_t n = 0;
  buf[0] = '\0';
#define APPEND(s) do { size_t len = strlen(s); if (n + len + 2 <= buf_len) { if (n) { buf[n++] = ','; buf[n] = '\0'; } n += snprintf(buf + n, buf_len - n, "%s", s); } } while(0)
  if (r & AWAKE_USER_ACTIVE) APPEND("USER");
  if (r & AWAKE_OP_OTA) APPEND("OTA");
  if (r & AWAKE_OP_PROVISION) APPEND("PROV");
  if (r & AWAKE_OP_CAPTURE) APPEND("CAPTURE");
  if (r & AWAKE_OP_API) APPEND("API");
  if (r & AWAKE_OP_MAINTENANCE) APPEND("MAINT");
  if (r & AWAKE_DEBUG_OVERRIDE) APPEND("DEBUG");
#undef APPEND
  if (buf[0] == '\0' && buf_len > 0) snprintf(buf, buf_len, "none");
}

static char s_reason_buf[80];

bool AwakeSupervisor::tick(unsigned long now_ms, const char* wake_reason_str) {
  sleep_requested_ = false;

  if (idle_since_ms_ == 0) {
    idle_since_ms_ = now_ms;
  }
  if (reasons_ != 0) {
    idle_since_ms_ = now_ms;
  }
  unsigned long idle_ms = (reasons_ != 0) ? 0 : (now_ms - idle_since_ms_);

  // Max duration guard
  for (int bit = 0; bit < 7; bit++) {
    if (!(reasons_ & (1u << bit))) continue;
    unsigned long elapsed = now_ms - reason_start_ms_[bit];
    unsigned long max_ms = 0;
    if (bit == 1) max_ms = MAX_OTA_MS;
    else if (bit == 2) max_ms = MAX_PROVISION_MS;
    else if (bit == 4) max_ms = MAX_API_MS;
    if (max_ms > 0 && elapsed > max_ms) {
      LOG_WARN("[AWAKE] reason held too long bit=%d elapsed=%lu max=%lu - forcing teardown",
               bit, (unsigned long)elapsed, (unsigned long)max_ms);
      reasons_ &= ~(1u << bit);
      sleep_requested_ = true;
      return true;
    }
  }

  // Idle timeout
  if (reasons_ == 0 && idle_ms >= idle_timeout_ms_) {
    LOG_INFO("[AWAKE] idle_ms=%lu >= timeout=%lu - requesting sleep wake_reason=%s",
             (unsigned long)idle_ms, (unsigned long)idle_timeout_ms_,
             wake_reason_str ? wake_reason_str : "-");
    sleep_requested_ = true;
    return true;
  }

  // Periodic log
  if (wake_reason_str && (now_ms - last_log_ms_) >= LOG_INTERVAL_MS) {
    last_log_ms_ = now_ms;
    reason_str(reasons_, s_reason_buf, sizeof(s_reason_buf));
    LOG_INFO("[AWAKE] reasons=%s idle_ms=%lu wake_reason=%s",
             s_reason_buf, (unsigned long)idle_ms, wake_reason_str);
  }

  return false;
}

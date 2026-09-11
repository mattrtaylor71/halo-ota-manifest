#ifndef SENSE_ACTION_SUMMARY_H
#define SENSE_ACTION_SUMMARY_H

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// RAM-only, current-boot totals. A fetch execution includes its existing HTTP
// retry; cached responses and ignored/unqueued refresh requests are not counted.
namespace sense_action_summary {
enum class Result : uint8_t {
  None, Pending, Ok, WifiTimeout, WifiUnavailable, Provisioning, Host,
  Network, Heap, Parse, Http, Dns, Timeout, Failed
};
struct Snapshot {
  uint32_t attempts = 0, successes = 0, failures = 0, duration_ms = 0;
  Result result = Result::None, pending_failure = Result::Failed;
  bool active = false;
};
static_assert(sizeof(Snapshot) <= 24, "Keep action counters small");
static Snapshot state;
static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
// Report construction normally belongs to the loop. A nonblocking guard also
// makes the fixed formatter buffer safe if a second caller is introduced.
static std::atomic_flag format_owner = ATOMIC_FLAG_INIT;
static constexpr size_t GROUP_BYTES = 256;
static char group_buffer[GROUP_BYTES];

inline void increment(uint32_t& value) { if (value != UINT32_MAX) ++value; }
inline Snapshot snapshot() {
  portENTER_CRITICAL(&state_mux);
  const Snapshot copy = state;
  portEXIT_CRITICAL(&state_mux);
  return copy;
}
inline const char* name(Result value) {
  switch (value) {
    case Result::Pending: return "pending";
    case Result::Ok: return "ok";
    case Result::WifiTimeout: return "wifi_timeout";
    case Result::WifiUnavailable: return "wifi_unavailable";
    case Result::Provisioning: return "provisioning";
    case Result::Host: return "host";
    case Result::Network: return "network";
    case Result::Heap: return "heap";
    case Result::Parse: return "parse";
    case Result::Http: return "http";
    case Result::Dns: return "dns";
    case Result::Timeout: return "timeout";
    default: return "failed";
  }
}
inline void note_failure(const char* reason) {
  Result result = Result::Failed;
  if (reason) {
    if (!strcmp(reason, "wifi_connect_timeout")) result = Result::WifiTimeout;
    else if (!strcmp(reason, "wifi_not_connected")) result = Result::WifiUnavailable;
    else if (!strcmp(reason, "wifi_provisioning_active")) result = Result::Provisioning;
    else if (!strcmp(reason, "host_parse_fail")) result = Result::Host;
    else if (!strcmp(reason, "net_not_ready") || !strcmp(reason, "network_fail")) result = Result::Network;
    else if (!strcmp(reason, "low_internal_heap")) result = Result::Heap;
    else if (!strcmp(reason, "json_parse")) result = Result::Parse;
    else if (!strcmp(reason, "http_error")) result = Result::Http;
    else if (!strcmp(reason, "dns_error")) result = Result::Dns;
    else if (!strcmp(reason, "timeout")) result = Result::Timeout;
  }
  portENTER_CRITICAL(&state_mux);
  if (state.active) state.pending_failure = result;
  portEXIT_CRITICAL(&state_mux);
}
class ListAttempt {
  const uint32_t started_ms_;
  bool ok_ = false;
public:
  ListAttempt() : started_ms_(millis()) {
    portENTER_CRITICAL(&state_mux);
    increment(state.attempts);
    state.active = true;
    state.result = Result::Pending;
    state.pending_failure = Result::Failed;
    portEXIT_CRITICAL(&state_mux);
  }
  ListAttempt(const ListAttempt&) = delete;
  ListAttempt& operator=(const ListAttempt&) = delete;
  void complete(bool ok) { ok_ = ok; }
  ~ListAttempt() {
    const uint32_t duration_ms = (uint32_t)(millis() - started_ms_);
    portENTER_CRITICAL(&state_mux);
    increment(ok_ ? state.successes : state.failures);
    state.duration_ms = duration_ms;
    state.result = ok_ ? Result::Ok : state.pending_failure;
    state.active = false;
    portEXIT_CRITICAL(&state_mux);
  }
};

// Only numeric fields and fixed enum strings enter this suffix. No user text,
// owner, network credentials, media, item names or request payload is copied.
inline size_t format_group(const Snapshot& value, uint32_t boot_id, bool empty,
                           char* buffer, size_t capacity) {
  if (!boot_id || !buffer || !capacity) return 0;
  int count = snprintf(buffer, capacity,
      "%s\"action_scope\":\"boot_v1\",\"action_boot_id\":%lu,"
      "\"list_attempts\":%lu,\"list_successes\":%lu,\"list_failures\":%lu",
      empty ? "" : ",", (unsigned long)boot_id,
      (unsigned long)value.attempts, (unsigned long)value.successes,
      (unsigned long)value.failures);
  if (count < 0 || (size_t)count >= capacity) return 0;
  size_t used = (size_t)count;
  if (value.attempts) {
    count = snprintf(buffer + used, capacity - used,
                     ",\"list_last_result\":\"%s\"", name(value.result));
    if (count < 0 || (size_t)count >= capacity - used) return 0;
    used += (size_t)count;
    if (!value.active) {
      count = snprintf(buffer + used, capacity - used,
                       ",\"list_last_duration_ms\":%lu", (unsigned long)value.duration_ms);
      if (count < 0 || (size_t)count >= capacity - used) return 0;
      used += (size_t)count;
    }
  }
  if (capacity - used < 2) return 0;
  buffer[used++] = '}';
  buffer[used] = '\0';
  return used;
}

template<class Text> bool append(Text& body, uint32_t boot_id, size_t limit) {
  const size_t original = body.length();
  if (!boot_id || original < 2 || body[0] != '{' || body[original - 1] != '}' ||
      original > limit || format_owner.test_and_set(std::memory_order_acquire)) return false;
  struct Release {
    ~Release() { format_owner.clear(std::memory_order_release); }
  } release;
  const size_t count = format_group(snapshot(), boot_id, original == 2,
                                    group_buffer, sizeof(group_buffer));
  // Replace only the original closing brace, and only after all allocation and
  // append work succeeds. Failure leaves the existing report byte-for-byte intact.
  if (!count || count - 1 > limit - original ||
      !body.reserve(original + count - 1)) return false;
  if (!body.concat(group_buffer + 1, count - 1) || body.length() != original + count - 1) {
    body.remove(original);
    return false;
  }
  body.setCharAt(original - 1, group_buffer[0]);
  return true;
}
} // namespace sense_action_summary
#endif

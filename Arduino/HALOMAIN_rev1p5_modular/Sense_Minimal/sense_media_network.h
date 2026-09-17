#ifndef SENSE_MEDIA_NETWORK_H
#define SENSE_MEDIA_NETWORK_H
#include <stdint.h>

namespace sense_media_network {
static constexpr uint32_t TLS_CONNECT_MAX_MS = 8000;
static constexpr uint32_t WIFI_RESET_SETUP_MS = 500;  // hardResetSta's two delays

static inline bool transient_http_status(int code) {
  return code == 408 || code == 429 || (code >= 500 && code <= 599);
}

static inline uint32_t connect_timeout_ms(uint32_t remaining_ms) {
  return remaining_ms < TLS_CONNECT_MAX_MS ? remaining_ms : TLS_CONNECT_MAX_MS;
}

// SDK accepts whole seconds. Round down so a short budget is not extended.
// Callers must decline a handshake when the result is zero.
static inline uint32_t handshake_timeout_seconds(uint32_t remaining_ms) {
  return connect_timeout_ms(remaining_ms) / 1000;
}

static inline uint32_t reset_wait_timeout_ms(uint32_t remaining_ms) {
  if (remaining_ms <= WIFI_RESET_SETUP_MS) return 0;
  const uint32_t available = remaining_ms - WIFI_RESET_SETUP_MS;
  return available < 15000 ? available : 15000;
}
}  // namespace sense_media_network
#endif

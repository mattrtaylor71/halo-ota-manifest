#pragma once
#include <stddef.h>
#include <string.h>
#include "SystemPower.h"

// Sense supplies one nonblocking copy of the LCD measurement cache. Shared
// provisioning/host-test builds may omit it; absence is explicit unknown data.
#if defined(ARDUINO_ARCH_ESP32)
extern "C" bool halo_system_power_json(char*, size_t) __attribute__((weak));
#elif defined(HALO_SYSTEM_POWER_HAS_HOOK)
// Host fixtures opt in to a real hook explicitly. Darwin's executable linker
// rejects an unresolved ELF-style weak function, unlike the target toolchain.
extern "C" bool halo_system_power_json(char*, size_t);
#endif

namespace halo_power_transport {
static constexpr size_t JSON_BYTES = halo_power::kJsonCapacity;  // Includes NUL.
static constexpr char HEADER_NAME[] = "X-Halo-System-Power";
static constexpr char HEADER_PREFIX[] = "X-Halo-System-Power: ";

// This is a transport guard, not a second measurement/parser implementation.
// Only the trusted fixed-schema formatter supplies values. Never allow a bad
// hook or unterminated/truncated result to create another HTTP header.
inline bool header_safe(const char* value, size_t capacity) {
  if (!value || !capacity) return false;
  const size_t length = strnlen(value, capacity);
  if (length < 2 || length == capacity || value[0] != '{' || value[length - 1] != '}')
    return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 32 || c > 126) return false;
  }
  return true;
}

inline size_t snapshot(char* out, size_t capacity) {
  if (!out || !capacity) return 0;
  if (capacity > JSON_BYTES) capacity = JSON_BYTES;
  memset(out, 0, capacity);
#if defined(ARDUINO_ARCH_ESP32) || defined(HALO_SYSTEM_POWER_HAS_HOOK)
  if (halo_system_power_json && halo_system_power_json(out, capacity) &&
      header_safe(out, capacity)) return strlen(out);
#endif
  // A cache or formatting failure must not become zero volts or valid 0%.
  out[0] = 0;
  if (!halo_power::json(halo_power::View{}, out, capacity)) return 0;
  return strlen(out);
}

// HTTPClient copies the header into its request-owned String. Keep the fixed
// snapshot off each caller's longer-lived upload/list/claim stack frame.
template <class Http>
__attribute__((noinline)) bool add_header(Http& http) {
  char value[JSON_BYTES];
  if (!snapshot(value, sizeof(value))) return false;
  http.addHeader(HEADER_NAME, value);
  return true;
}
}  // namespace halo_power_transport

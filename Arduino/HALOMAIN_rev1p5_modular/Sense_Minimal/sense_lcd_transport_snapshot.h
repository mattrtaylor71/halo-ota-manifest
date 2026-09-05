#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

// Terminal observations only. No SSL reads, client polling, logging, allocation,
// or NVS here. SO_ERROR clears the socket error and must never run before the
// caller has already decided to abandon this HTTP connection.
struct LcdOtaTransportSnapshot {
  uint32_t captured_ms = 0;
  int32_t fd = -1;
  int32_t ioctl_result = -2;  // -2 means not sampled (invalid descriptor).
  int32_t ioctl_errno = 0;
  int32_t queued_bytes = 0;
  int32_t peek_result = -2;
  int32_t peek_errno = 0;
  int32_t so_result = -2;
  int32_t so_error = 0;       // SO_ERROR value on success; syscall errno on failure.
  uint8_t wifi_status = 255;
  int32_t rssi = 0;
  uint32_t internal_free = 0;
  uint32_t internal_largest = 0;
  uint32_t psram_free = 0;
  uint32_t psram_largest = 0;
  uint32_t duration_ms = 0;
};

template <typename SecureClient>
static LcdOtaTransportSnapshot sense_lcd_transport_snapshot(const SecureClient& client) {
  const int caller_errno = errno;
  LcdOtaTransportSnapshot s;
  s.captured_ms = (uint32_t)millis();
  s.fd = client.fd();  // Never use NetworkClientSecure::socket(): it changes fd.
  if (s.fd >= 0) {
    int queued = 0;
    errno = 0;
    s.ioctl_result = lwip_ioctl(s.fd, FIONREAD, &queued);
    s.ioctl_errno = s.ioctl_result == 0 ? 0 : errno;
    if (s.ioctl_result == 0) s.queued_bytes = queued;

    uint8_t ignored = 0;  // Ciphertext is neither consumed nor retained.
    errno = 0;
    s.peek_result = lwip_recv(s.fd, &ignored, 1, MSG_PEEK | MSG_DONTWAIT);
    s.peek_errno = s.peek_result < 0 ? errno : 0;

    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    errno = 0;
    s.so_result = lwip_getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &socket_error, &length);
    s.so_error = s.so_result == 0 ? socket_error : errno;
    if (s.so_result == 0 && length != sizeof(socket_error)) {
      s.so_result = -2;  // Unexpected SDK result length: not a valid SO_ERROR.
      s.so_error = EINVAL;
    }
  }
  s.wifi_status = (uint8_t)WiFi.status();
  s.rssi = WiFi.RSSI();
  s.internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s.internal_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s.psram_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  s.duration_ms = (uint32_t)((uint32_t)millis() - s.captured_ms);
  errno = caller_errno;
  return s;
}

static bool sense_lcd_transport_format(uint16_t session, const LcdOtaTransportSnapshot& s,
                                       char* out, size_t capacity) {
  // At all int32/uint32 extrema this is <=219 bytes, below lcdxfer's 224-byte
  // detail limit. m order: internal free/largest, PSRAM free/largest (hex bytes).
  // TLS is explicitly unknown: public lastError() is a connection-era value.
  const int n = snprintf(out, capacity,
      "N1 s=%u t=%lu fd=%ld io=%ld ie=%ld q=%ld pk=%ld pe=%ld so=%ld se=%ld w=%u r=%ld m=%08lx/%08lx/%08lx/%08lx d=%lu tls=?",
      (unsigned)session, (unsigned long)s.captured_ms, (long)s.fd,
      (long)s.ioctl_result, (long)s.ioctl_errno, (long)s.queued_bytes,
      (long)s.peek_result, (long)s.peek_errno, (long)s.so_result, (long)s.so_error,
      (unsigned)s.wifi_status, (long)s.rssi,
      (unsigned long)s.internal_free, (unsigned long)s.internal_largest,
      (unsigned long)s.psram_free, (unsigned long)s.psram_largest,
      (unsigned long)s.duration_ms);
  return n >= 0 && (size_t)n < capacity && n < 224;
}

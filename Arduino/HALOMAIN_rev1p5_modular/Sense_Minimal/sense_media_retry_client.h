#pragma once
#include "sense_media_network.h"

// The saved-media scope and atomic user pause are owned by sense_media_retry.h.
// Only the upload worker calls this client's methods/stop(). No other task
// closes a socket or tears down Wi-Fi underneath an active SDK operation.
class SenseMediaRetryClient : public WiFiClientSecure {
 public:
  SenseMediaRetryClient() : background_(media_retry_network_active()) {
    if (background_) setHandshakeTimeout(8);
  }
  using WiFiClientSecure::connect;

  int connect(const char* host, uint16_t port, int32_t timeout_ms) override {
    if (cancel_now()) return 0;
    if (background_ && (timeout_ms <= 0 || timeout_ms > 8000)) timeout_ms = 8000;
    const int connected = WiFiClientSecure::connect(host, port, timeout_ms);
    // DNS/TCP/handshake are synchronous SDK phases. They cannot be stopped
    // safely by another task; close here before sending any request bytes.
    return cancel_now() ? 0 : connected;
  }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    if (cancel_now()) return 0;
    if (!background_) return WiFiClientSecure::write(bytes, count);
    size_t sent = 0;
    while (sent < count) {
      if (cancel_now()) return 0;
      const size_t remaining = count - sent;
      const size_t chunk = remaining < 512 ? remaining : 512;
      const size_t written = WiFiClientSecure::write(bytes + sent, chunk);
      if (cancel_now()) return 0;
      if (!written) return sent;
      sent += written;
    }
    return sent;
  }
  int read() override {
    if (cancel_now()) return -1;
    const int result = WiFiClientSecure::read();
    return cancel_now() ? -1 : result;
  }
  int read(uint8_t* bytes, size_t count) override {
    if (cancel_now()) return -1;
    const int result = WiFiClientSecure::read(bytes, count);
    return cancel_now() ? -1 : result;
  }
  int available() override {
    if (cancel_now()) return 0;
    const int result = WiFiClientSecure::available();
    return cancel_now() ? 0 : result;
  }
  uint8_t connected() override {
    return cancel_now() ? 0 : WiFiClientSecure::connected();
  }
  int peek() override { return cancel_now() ? -1 : WiFiClientSecure::peek(); }

 private:
  bool cancel_now() {
    if (cancelled_) return true;
    if (!background_ || !media_retry_network_cancelled()) return false;
    cancelled_ = true;
    WiFiClientSecure::stop();
    // Stream::readStringUntil() retries read() until its own timeout even
    // after stop(). Shorten that owner-local loop when cancellation is latched.
    Stream::setTimeout(0);
    return true;
  }
  const bool background_;
  bool cancelled_ = false;
};

static bool media_retry_network_wait(uint32_t delay_ms) {
  if (!media_retry_network_active()) { delay(delay_ms); return true; }
  const uint32_t started = millis();
  while (uint32_t(millis() - started) < delay_ms) {
    if (media_retry_network_cancelled()) return false;
    const uint32_t left = delay_ms - uint32_t(millis() - started);
    delay(left < 20 ? left : 20);
  }
  return !media_retry_network_cancelled();
}

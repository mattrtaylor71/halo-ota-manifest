#pragma once
#include "ProvisioningClaimJob.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace provision_claim {
// Only the transport worker touches this client, including stop(). The SDK's
// synchronous DNS/TCP/TLS connect must return before cancellation can close it;
// explicit TCP/TLS limits replace Arduino's 120-second handshake default.
class Client : public WiFiClientSecure {
 public:
  Client(Job& job, const Request& request) : job_(job), request_(request) {
    setHandshakeTimeout(8);
  }
  using WiFiClientSecure::connect;
  int connect(const char* host, uint16_t port, int32_t timeout) override {
    if (cancel_now()) return 0;
    if (timeout <= 0 || timeout > 5000) timeout = 5000;
    const int rc = WiFiClientSecure::connect(host, port, timeout);
    return cancel_now() ? 0 : rc;
  }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (cancel_now()) return 0;
    const size_t n = WiFiClientSecure::write(data, size);
    return cancel_now() ? 0 : n;
  }
  int read() override {
    if (cancel_now()) return -1;
    const int n = WiFiClientSecure::read();
    return cancel_now() ? -1 : n;
  }
  int read(uint8_t* data, size_t size) override {
    if (cancel_now()) return -1;
    const int n = WiFiClientSecure::read(data, size);
    return cancel_now() ? -1 : n;
  }
  int available() override {
    if (cancel_now()) return 0;
    const int n = WiFiClientSecure::available();
    return cancel_now() ? 0 : n;
  }
  int peek() override { return cancel_now() ? -1 : WiFiClientSecure::peek(); }
  uint8_t connected() override { return cancel_now() ? 0 : WiFiClientSecure::connected(); }
 private:
  bool cancel_now() {
    if (cancelled_) return true;
    if (!job_.cancelled(request_, millis())) return false;
    cancelled_ = true;
    WiFiClientSecure::stop();
    Stream::setTimeout(0);
    return true;
  }
  Job& job_;
  const Request& request_;
  bool cancelled_ = false;
};

class ResponseSink : public Stream {
 public:
  explicit ResponseSink(Result& result) : result_(result) {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (size > kResponseBytes - used_) {
      result_.overflow = true;
      setWriteError();
      return 0;
    }
    memcpy(result_.response + used_, data, size);
    used_ += size;
    result_.response[used_] = 0;
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
 private:
  Result& result_;
  size_t used_ = 0;
};

inline Result transport(Job& job, const Request& request, const char* url,
                        const char* ca, bool insecure) {
  Result result;
  Client client(job, request);
  HTTPClient http;
  if (insecure) client.setInsecure();
  else client.setCACert(ca);
  if (!job.cancelled(request, millis()) && http.begin(client, url)) {
    http.setConnectTimeout(5000);
    http.setTimeout(3000);
    http.setReuse(false);
    http.addHeader("Content-Type", "application/json");
    result.http_code = http.POST(String(request.body));
    if (result.http_code > 0 && !job.cancelled(request, millis())) {
      if (http.getSize() > int(kResponseBytes)) result.overflow = true;
      else {
        ResponseSink sink(result);
        result.complete = http.writeToStream(&sink) >= 0 && !result.overflow &&
                          !job.cancelled(request, millis());
      }
    }
  }
  http.end();
  client.stop();
  result.elapsed_ms = uint32_t(millis() - request.queued_ms);
  return result;
}
}

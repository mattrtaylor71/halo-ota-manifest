#pragma once

// Deferred means no request was attempted. The caller retains its refresh or
// exact delete identity and retries admission without reporting network failure.
enum class ListRequestResult : uint8_t { Completed, Failed, Deferred };

// Construct before the complete Wi-Fi call, DMA guard, and TLS/response objects.
// Their destructors must finish before this lease releases the shared mutex.
// In particular, a main-loop delete never waits here behind an upload.
class SenseListTransportLease {
 public:
  explicit SenseListTransportLease(const char* label) : label_(label) {
#ifdef HALO_SENSE_PROD_WRAPPER
    if (halo_provisioning_active()) return;
#endif
    if (!http_mutex || xSemaphoreTake(http_mutex, 0) != pdTRUE) return;
    if (http_inflight) {
      xSemaphoreGive(http_mutex);
      return;
    }
    held_ = true;
    http_inflight = true;
    Serial.printf("[LIST_TRANSPORT] start kind=%s\n", label_);
  }
  ~SenseListTransportLease() {
    if (!held_) return;
    http_inflight = false;
    Serial.printf("[LIST_TRANSPORT] done kind=%s\n", label_);
    xSemaphoreGive(http_mutex);
  }
  explicit operator bool() const { return held_; }
  SenseListTransportLease(const SenseListTransportLease&) = delete;
  SenseListTransportLease& operator=(const SenseListTransportLease&) = delete;

 private:
  const char* label_;
  bool held_ = false;
};

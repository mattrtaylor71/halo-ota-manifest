#pragma once

#include <atomic>
#include <stdint.h>

// UART owns request/cancellation. UI owns the submitted-frame acknowledgement.
// Tokens distinguish repeated BEGINs even when their wire session is identical.
// An acknowledgement is NOT flash permission: the existing owner/DMA barrier
// must still pass after UART publishes receive inhibition.
class LcdOtaInstallFrame {
 public:
  uint32_t request(uint16_t session, uint32_t now, uint32_t budget) {
    token_.store(0);
    const uint32_t token = ++generation_ ? generation_ : ++generation_;
    session_.store(session);
    started_.store(now);
    budget_.store(budget);
    acknowledged_.store(0);
    token_.store(token);
    return token;
  }
  bool active(uint32_t token, uint32_t now) const {
    return token && token_.load() == token &&
        (uint32_t)(now - started_.load()) < budget_.load() && token_.load() == token;
  }
  uint32_t pending(uint32_t now) const {
    const uint32_t token = token_.load();
    return active(token, now) ? token : 0;
  }
  bool acknowledge(uint32_t token, uint32_t now) {
    if (!active(token, now)) return false;
    acknowledged_.store(token);
    return active(token, now);
  }
  bool acknowledged(uint32_t token, uint16_t session, uint32_t now) const {
    return active(token, now) && session_.load() == session && acknowledged_.load() == token;
  }
  void cancel(uint32_t token) { token_.compare_exchange_strong(token, 0); }

 private:
  uint32_t generation_ = 0; // UART owner only
  std::atomic<uint32_t> token_{0}, acknowledged_{0}, started_{0}, budget_{0};
  std::atomic<uint16_t> session_{0};
};

static LcdOtaInstallFrame g_lcd_ota_install_frame;

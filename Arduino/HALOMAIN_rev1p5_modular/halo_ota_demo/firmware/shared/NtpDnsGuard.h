#pragma once

// Shared network code also compiles for LCD. With no Sense time service these
// hooks are absent and the guard is a no-op; Sense supplies the implementation.
extern "C" {
#ifdef HALO_SENSE_PROD_WRAPPER
void halo_sntp_dns_acquire();
void halo_sntp_dns_release();
bool halo_sntp_sync_pending();
#else
void halo_sntp_dns_acquire() __attribute__((weak));
void halo_sntp_dns_release() __attribute__((weak));
bool halo_sntp_sync_pending() __attribute__((weak));
#endif
}

class HaloNtpDnsGuard {
 public:
  HaloNtpDnsGuard() : active_(halo_sntp_dns_acquire && halo_sntp_dns_release) {
    if (active_) halo_sntp_dns_acquire();
  }
  ~HaloNtpDnsGuard() { release(); }
  // FreeRTOS task deletion does not unwind C++ stack objects. Such exits must
  // release explicitly; a later ordinary scope exit remains harmless.
  void release() {
    if (!active_) return;
    active_ = false;
    halo_sntp_dns_release();
  }
  HaloNtpDnsGuard(const HaloNtpDnsGuard&) = delete;
  HaloNtpDnsGuard& operator=(const HaloNtpDnsGuard&) = delete;
 private:
  bool active_;
};

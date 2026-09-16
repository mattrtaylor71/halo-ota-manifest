#pragma once
#include <stdint.h>
#include <string.h>

// UI-owner state only. UART producers enqueue statuses rather than manipulating
// this object, so a late QR/heartbeat cannot unwind an app-owned setup step.
struct LcdProvisionFlow {
  enum Step { Closed = 0, GetApp = 1, AppRoute, PairQr, SelectWifi, Connecting,
              Complete, Failed };
  Step step = Closed;
  bool app_connected = false;
  bool claiming = false;
  uint32_t complete_at_ms = 0;
  uint32_t connecting_at_ms = 0;
  bool progress_timed_out = false;

  bool active() const { return step != Closed; }
  bool scrollable() const { return step >= GetApp && step <= PairQr && !app_connected; }
  void begin() { if (!active()) { step = GetApp; app_connected = claiming = progress_timed_out = false; } }
  void close() { step = Closed; app_connected = claiming = progress_timed_out = false; complete_at_ms = 0; }
  void retry(bool has_qr) {
    step = has_qr ? PairQr : GetApp;
    app_connected = claiming = false;
    complete_at_ms = 0;
    progress_timed_out = false;
  }
  bool scroll(int delta) {
    if (!scrollable() || !delta) return false;
    const Step next = delta > 0 ? (step == PairQr ? PairQr : Step(step + 1))
                                : (step == GetApp ? GetApp : Step(step - 1));
    if (next == step) return false;
    step = next;
    return true;
  }
  bool status(const char* value, uint32_t now) {
    if (!active() || !value || step == Complete) return false;
    const Step old = step;
    const bool was_claiming = claiming;
    if (!strcmp(value, "app_connected")) {
      app_connected = true;
      if (step <= SelectWifi) step = SelectWifi;
    } else if (!strcmp(value, "connecting") || !strcmp(value, "claiming")) {
      if (progress_timed_out) return false;
      app_connected = true;
      if (step != Connecting) connecting_at_ms = now;
      step = Connecting;
      claiming = !strcmp(value, "claiming");
    } else if (!strcmp(value, "connected")) {
      // A queued heartbeat from the old network can arrive just after Change
      // Wi-Fi begins. Require progress from this guide before showing success.
      if (!app_connected) return false;
      app_connected = true;
      step = Complete;
      complete_at_ms = now;
    } else if (!strcmp(value, "failed") || !strcmp(value, "error")) {
      step = Failed;
    }
    return step != old || claiming != was_claiming;
  }
  bool completion_due(uint32_t now) const {
    return step == Complete && uint32_t(now - complete_at_ms) >= 4500;
  }
  bool check_timeout(uint32_t now) {
    if (step != Connecting || uint32_t(now - connecting_at_ms) < 240000) return false;
    step = Failed;
    progress_timed_out = true;
    return true;
  }
};

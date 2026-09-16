#pragma once
#include <string.h>

// Presentation only: Wi-Fi, claim retries and persisted provisioning state keep
// their existing owners. An app session remains detected even if HTTP pauses.
class ProvisioningDisplayStatus {
public:
  void reset() { setup_was_active_ = guided_ = app_seen_ = false; }
  bool tracking() const { return guided_; }
  const char* observe(const char* state, bool setup_active, bool app_active,
                      bool owner_set, bool claim_failed) {
    if (setup_active && !setup_was_active_) {
      guided_ = true;
      app_seen_ = false;
    }
    setup_was_active_ = setup_active;
    if (setup_active && app_active) app_seen_ = true;
    if (!state) state = "unknown";
    const char* result = state;
    if (strcmp(state, "error") == 0) result = "failed";
    else if (guided_ && strcmp(state, "ap_setup") == 0)
      result = app_seen_ ? "app_connected" : "ap_setup";
    else if (guided_ && strcmp(state, "connected") == 0)
      result = owner_set ? "connected" : (claim_failed ? "failed" : "claiming");
    // Keep observing account recovery after AP shutdown, but stop NVS polling
    // once there is a terminal result. Normal provisioned wakes stay normal.
    if (!setup_active && strcmp(state, "connected") == 0 &&
        (owner_set || claim_failed)) guided_ = false;
    return result;
  }
private:
  bool setup_was_active_ = false;
  bool guided_ = false;
  bool app_seen_ = false;
};

#include "SenseOtaPolicy.h"
#include "BuildFlags.h"
#include "Truth.h"
#include "Log.h"
#include <time.h>

__attribute__((weak)) bool halo_ota_manual_override_active() {
  return false;
}

namespace SenseOtaPolicy {

// Always allow OTA work.
//
// This used to be `manual_override || !maintenance_only || in_window`, with
// `in_window` set by the maintenance-window machinery. Under the nightly design
// that machinery is gone, so `in_window` can never become true — and with
// HALO_OTA_POLICY_MAINTENANCE_ONLY=1 the expression collapses to "always refuse".
//
// That is not hypothetical. On the first nightly-only build the wake fired, called
// maybeRunOtaCheck("nightly"), and the policy silently skipped it:
//     [OTA_POLICY] maintenance_only skip ota_check (not in window)
// The device would have woken at 02:00 every night and never checked for an
// update, reporting success the whole time.
//
// The gate is now redundant rather than wrong: OTA work is only ever *invoked*
// from the nightly maintenance wake, so "are we in the window?" is answered by
// construction — being here means we are. The manual override is preserved
// because a user-triggered update must still work outside that path.
bool allowOtaWorkNow(const char* why) {
  const bool manual_override = halo_ota_manual_override_active();
  Serial.printf("[OTA_POLICY] allow=1 manual=%d why=%s (nightly design: the wake IS the window)\n",
                manual_override ? 1 : 0,
                why ? why : "");
  return true;
}

}  // namespace SenseOtaPolicy

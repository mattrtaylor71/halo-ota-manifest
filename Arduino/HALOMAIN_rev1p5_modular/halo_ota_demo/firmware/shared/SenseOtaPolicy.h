#ifndef SENSE_OTA_POLICY_H
#define SENSE_OTA_POLICY_H

/**
 * Sense OTA policy: single gate for "may we do OTA work now?".
 * Used to enforce maintenance-only: no manifest fetch or OTA intent unless allowed.
 */
namespace SenseOtaPolicy {

/** Return true iff OTA work (intent, manifest fetch, runOtaCheckOnce) is allowed now.
 *  true when: HALO_OTA_POLICY_MAINTENANCE_ONLY==0 OR maintenance_in_window OR maintenance_mode.
 *  Logs [OTA_POLICY] allow=... when called (caller may rate-limit). */
bool allowOtaWorkNow(const char* why);

}  // namespace SenseOtaPolicy

#endif  // SENSE_OTA_POLICY_H

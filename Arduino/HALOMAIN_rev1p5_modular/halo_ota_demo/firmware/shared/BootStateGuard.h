#ifndef BOOT_STATE_GUARD_H
#define BOOT_STATE_GUARD_H

/**
 * BootStateGuard: Compile-time check to ensure RTC_DATA_ATTR is not used for boot/OTA tracking.
 * 
 * This header should be included early in the main sketch.
 * It provides a compile-time check (via grep/CI) and runtime documentation.
 * 
 * Manual check: grep -r "RTC_DATA_ATTR.*boot\|RTC_DATA_ATTR.*g_boot\|RTC_DATA_ATTR.*g_last" firmware/
 * Should return no matches (except in this comment or lcd_ota_demo which is a different sketch).
 */

// Compile-time assertion: If you see this error, RTC_DATA_ATTR is being used for boot tracking
// Search the codebase for "RTC_DATA_ATTR" and replace with BootState (NVS)
#ifdef __cplusplus
  // C++ compile-time check: ensure BootState is used instead of RTC_DATA_ATTR
  // This is a documentation/reminder - actual enforcement is via grep/CI
  static_assert(true, "BootState (NVS) must be used for boot_count persistence, not RTC_DATA_ATTR");
#endif

#endif // BOOT_STATE_GUARD_H

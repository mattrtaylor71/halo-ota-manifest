// BuildFlags.h - Single source of truth for compile-time build flags
// 
// This header ensures consistent defaults across all compilation units.
// Build flags can override via -DOTA_ENABLED=0 or -DSHIP_TEST_MODE=1

#ifndef BUILD_FLAGS_H
#define BUILD_FLAGS_H

// OTA enable/disable (default ON for normal builds)
// OTA_ENABLED=0: log "OTA disabled", do NOT emit errors.
// OTA_ENABLED=1: config must resolve; empty base/url => hard fail with reason.
#ifndef OTA_ENABLED
#define OTA_ENABLED 1
#endif

// SHIP_TEST_MODE: If true, always skip OTA apply (for provisioning testing)
// Default OFF (0) to allow OTA apply for testing
#ifndef SHIP_TEST_MODE
#define SHIP_TEST_MODE 0
#endif

// Simple OTA proof (no IDF rollback): after OTA reboot, prove UART sync + WiFi (if provisioned)
// within 15s; on pass clear pending + setLastSuccessTs; on fail setLastError, no reboot.
#ifndef HALO_SIMPLE_OTA_PROOF
#define HALO_SIMPLE_OTA_PROOF 1
#endif

// OTA_TEST_BUILD: enable OTA gauntlet test hooks (serial commands)
#ifndef OTA_TEST_BUILD
#define OTA_TEST_BUILD 0
#endif

// Watchdog enable/disable (default ON)
#ifndef WATCHDOG_ENABLED
#define WATCHDOG_ENABLED 1
#endif

// TLS insecure debug (default OFF). Set to 1 ONLY for temporary diagnostics.
#ifndef OTA_TLS_INSECURE_DEBUG
#define OTA_TLS_INSECURE_DEBUG 0
#endif

// Local dev override: disable OTA unless runtime override set.
#ifndef HALO_DEV_NO_OTA
#define HALO_DEV_NO_OTA 0
#endif
#ifndef LOCAL_DEV_BUILD
#define LOCAL_DEV_BUILD 0
#endif

// HALO_* macros: canonical names for use across compilation units
#define HALO_OTA_ENABLED (OTA_ENABLED)
#define HALO_SHIP_TEST_MODE (SHIP_TEST_MODE)

// OTA only during scheduled maintenance window; no opportunistic OTA on MQTT connect.
#ifndef HALO_OTA_POLICY_MAINTENANCE_ONLY
#define HALO_OTA_POLICY_MAINTENANCE_ONLY 1
#endif

// Maintenance self-test: if 1 and no schedule in NVS at pre_sleep, inject a schedule (start=now+60, duration=120) for timer-wake validation.
#ifndef HALO_MAINT_SELFTEST
#define HALO_MAINT_SELFTEST 0
#endif

// ── Demo-unit mode ────────────────────────────────────────────────────────────
// HALO_DEMO_MODE=1 builds a showroom/demo unit: every user-facing flow
// (check-in, check-out, discard, dish, voice, shopping list) runs the REAL
// production UI to completion, but nothing touches WiFi, TLS, MQTT, S3, the
// cloud, or OTA. The shopping list is preloaded from a canned table.
//
// Implementation lives in Sense_Minimal/sense_demo.h. The LCD firmware is
// unchanged: it is already driven by the Sense's UI_STATUS phase machine, so
// scripting those phases reproduces the shipped UX exactly.
//
// Default 0 — production builds are bit-for-bit unaffected. Build a demo unit
// with tools/build_demo_unit.sh (passes -DHALO_DEMO_MODE=1 to both targets).
#ifndef HALO_DEMO_MODE
#define HALO_DEMO_MODE 0
#endif

// Demo mode: also fire the real camera during a capture (photo is discarded
// immediately, never uploaded). Default OFF — camera init fails ~1-in-12 on
// current hardware, and a demo unit must not show an error. The fill LED still
// flashes either way, so the capture looks real to anyone watching.
#ifndef HALO_DEMO_CAPTURE
#define HALO_DEMO_CAPTURE 0
#endif

// A demo unit must stay awake: it keeps USB-CDC enumerated (so reflashing is
// instant, no tap-to-wake) and a showroom device that has gone dark reads as
// broken. Both flags below are the pre-existing, hardware-proven no-sleep
// gates — HALO_DEV_NO_SLEEP guards the LCD (lcd_activity.h gate AND the
// enterLightSleep funnel), STRESS_TEST_NO_SLEEP guards the Sense's single
// deep-sleep chokepoint. Implied here so a demo build cannot forget one.
//
// NOTE: this only takes effect where BuildFlags.h is included before the flag's
// own #ifndef default. That holds for the Sense (Sense_Minimal.ino includes it
// first) but NOT for LCD_Minimal.ino, which defaults HALO_DEV_NO_SLEEP=0 at its
// top. tools/build_demo_unit.sh therefore passes both flags on the command line
// as the authoritative path; this block is the belt-and-braces.
#if HALO_DEMO_MODE
#ifndef HALO_DEV_NO_SLEEP
#define HALO_DEV_NO_SLEEP 1
#endif
#ifndef STRESS_TEST_NO_SLEEP
#define STRESS_TEST_NO_SLEEP 1
#endif
#endif

#endif // BUILD_FLAGS_H

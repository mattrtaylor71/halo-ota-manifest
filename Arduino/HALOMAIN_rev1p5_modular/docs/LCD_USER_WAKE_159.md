# 6.4.159 — published; LCD wake fix installed and tested

Use artifact source `da3057b4ed59ad49804daa9974bb73084b5b5ac9` or a reviewed descendant for further firmware work. Build ID: `6.4.159-20260914T230156Z-da3057b4ed59`. Keep the frozen158 tag/package as the recovery and historical scheduled-OTA qualification reference; do not drop this159 LCD fix when preparing the next version. Both public latest manifests and complete binary readbacks match the checked159 pair.

The assembled unit currently runs **LCD159/app1 SDK VALID and Sense158/app0 SDK VALID**. Since Sense's real discovery budget is exhausted and its USB is inaccessible, the fixed LCD application was installed through LCD USB. Both previous LCD banks were backed up; the selected158 fallback, NVS, partition table and Sense firmware were preserved. This is a controlled LCD service, not a manual firmware transfer. The Sense runtime source and executable sections are unchanged in159; its published image carries the new release identity.

Validation completed September14:

- Six focused host suites pass, including16 actual-source wake regressions. The same regression reproduces failures on frozen158.
- Three manual requests completed: one with Sense awake and two after real SLEEP_READY/unanswered pings. Actual control-request delivery was about0.04s,1.19s and1.30s; observed pulse counts0,1,2 matched readiness and bounded retry cadence. Every cycle returned to Home naturally about8s after its terminal result, then entered deep sleep with the original Pacific timer.
- Cloud reports corroborate the real `policy_daily_limit` terminal for all three cycles: two network windows, zero applies/begins/reservation, no target. No allowance was reset and no new download happened.
- Two shopping-list responses applied all seven items; the unit returned to Home and natural sleep. The final35-sample passive USB observation saw no LCD reappearance. This is not an enclosure RF or USB-free test.
- Independent build review confirms only the two LCD wake runtime files changed. LCD adds704 bytes flash and16 bytes static RAM; Sense resources, RTC, partitions, shipping flags, SDK correction and96KiB LVGL configuration are preserved.

The original reported extra wake did not recur in these captured cycles and remains unexplained. Full paired159 installation, a new scheduled159 transfer, power interruption, physical cold boot and comprehensive product qualification remain outside this result. The next normal Sense allowance opportunity is **02:00 Pacific on September15**; schedule and ledger are unchanged. No unattended controller was started.

Receipt: `/Users/MattTaylor/halo-device-analytics-2026-09-10/hardware-validation159/ACCEPTANCE.json`; linked build, installation, manual, shopping, cloud and publication receipts contain exact source and capture hashes. The earlier source-candidate notes below preserve their original checkpoint.

---

# 6.4.159 candidate: deliver user requests after Sense sleeps

## Observed failure

The assembled158 LCD capture on September14 showed Sense sending SLEEP_READY, then the LCD changing ASLEEP to UNKNOWN after a missed PONG. Settings and Manual Update were not recognized as immediate user wake reasons. The retained sleep-ready flag therefore blocked the GPIO wake pulse. Repeated400ms UART pings moved the1500ms response deadline, preventing recovery until the queued manual action failed after12seconds. This failure occurred before Wi-Fi or any firmware download.

Evidence: `/Users/MattTaylor/halo-device-analytics-2026-09-10/assembled158-lcd-only-20260914/FINDINGS-20260914.md` and its original raw capture. The separately reported extra wake after check-in did not recur in that capture and is not explained by this fix.

## Scoped correction

- Honor explicit Settings/manual-update wake reasons while retaining the fresh-awake no-pulse guard.
- Preserve explicit sleep-ready evidence after an unanswered ping. The first unanswered PONG deadline stays fixed across retransmissions.
- Share one bounded pulse cadence between request producers; do not let deferred retries renew the ten-second episode. A new real gesture or explicit manual action can retry after exhaustion.
- Serialize the short response-deadline and wake-episode state transitions. Keep GPIO pulses and logging outside critical sections.

Sense runtime, OTA allowance/debt rules, firmware routes, production02:00Pacific schedule, and UI appearance are unchanged. Frozen158 tag, package and exact bytes remain the rollback reference.

## Validation and installation status

Actual-source host tests exercise the captured ordering, Settings/manual dispatch, fresh PONG, already-awake controls, deadline wrap, pulse cadence/exhaustion and new-user-action rearm. Existing maintenance sleep, manual OTA, clock and panel-ownership regressions are included. One old presentation expectation was independently reproduced as failing on frozen158: UART intentionally defers relighting to the UI owner. Only that stale expectation was corrected.

The concrete release workspace is `/Users/MattTaylor/halo-device-analytics-2026-09-10/hardware-validation159`. Build, artifact and device outcomes must be added from completed receipts; this document does not claim those pending steps passed.

The assembled unit exposes only LCD USB. Its158 Sense has consumed the two discovery network windows; DISCOVERY is not an immediately refillable manual-update state. LCD forwarding does not expose a supported quota reset. Preserve the real ledger and schedule. A manual action can be tested for wake/delivery/terminal UI without proving an image transfer. Any LCD-only USB application service must be explicitly recorded separately from a paired/manual OTA, preserving NVS, current158 fallback and image selectors outside the new candidate selection.

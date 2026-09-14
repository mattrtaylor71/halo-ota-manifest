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

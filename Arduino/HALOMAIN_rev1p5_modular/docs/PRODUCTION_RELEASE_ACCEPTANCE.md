# Released 6.4.114 — September 10, 2026

Manual and normal-calendar scheduled installations of the exact production pair passed. Both boards are SDK VALID and idle; 2 a.m. Pacific and paired natural sleep are restored. Production latest is 6.4.114. Scheduled accounting resolved on the subsequent verification wake; the opening wake transition and earlier LCD sleep boundary were unobserved. See RELEASE_BASELINE.json for the actual acceptance/publication and checkout records. Extended fault and soak testing is deferred.

# Production OTA acceptance — 6.4.114

**Final disposition: accepted and published.** The compiled source is commit `1224f28ab2d1307d54cf71956b30da2bbccead3d`, firmware tree `83c178bde2020c4e2fff800caf9e33be33a4ef97`. `RELEASE_BASELINE.json` contains the actual artifact hashes and saved test receipts. Publication, release tagging and original-checkout adoption are not yet complete.

## Change and scope

The latest correction addresses the observed retry cancellation at 12 seconds before its due time when the separate LCD timer-origin notice was absent. A persisted shipping ARMED record and current correlated peer may now wait within the existing 15-second lead interval. The function still returns false before due, preserves the missing-origin diagnostic and retains the original deadlines, target checks, busy guards and accounting.

The native regression reproduces the old failure and passes on this source. It exercises the actual extracted readiness code and canonical policy codec, including missing origin, exact-due reservation, no early or duplicate debit, invalid clock/storage/peer state, deadlines and caller busy guards. Earlier transfer-cleanup and retry-arm corrections remain in the compiled source.

The user directed a one-hour release effort on September 10. The remaining essential device checks are one manual 113→114 installation and one separate normal-calendar 113→114 installation of those same production artifacts. Additional repeats, deliberate faults, power cuts, USB-free operation and later-day recovery are deferred, not counted as passed.

## Actual acceptance

| Check | Result |
|---|---|
| Focused native readiness regression | PASS on actual source; observed old-source failure reproduced. |
| Exact 113 private and 114 production builds | PASS. Canonical shipping flags, source provenance, image hashes, partition fit and resource comparison verified. 114 uses production endpoints; bench, one-shot and fault controls are disabled. |
| Portable 114 recovery package | PASS. Exact application images, ELF/maps, bootloaders, partitions, flash arguments and provenance included; package and ZIP contents independently verified. |
| Controlled 113 manual baseline | PASS. Both boards booted exact 113/app1, SDK VALID, with a current Home/idle observation and later natural paired USB absence. Provisioning and valid 109 fallback preserved. The short Sense log did not prove the terminal sleep marker or current policy counters. |
| Manual 113→114 OTA | PASS. One supported LCD serial request at 16:20:55.588 PDT, followed by natural paired sleep. Post-case USB health confirms exact 114/app0 SDK VALID on both boards and Home/idle. No physical menu-button coverage is claimed. |
| Normal-calendar 113→114 OTA | PASS: exact114/nightly telemetry before verification, followed by paired SDK VALID/Home and settled native accounting. |
| Manual paired 114 health and accounting | PASS. The native RESOLVED phase 8/generation 6 record predates the post-case USB health check: admission at 16:21:00, resolution at 16:25:59 (299 seconds), reserved 0, one network/apply attempt and one begin per board. Remaining allowance is 14 ms, without refund. Final scheduled health/accounting passed on the verification wake. |
| Pacific restoration and production publication | PASS; final commit/tag and checkout bindings are in RELEASE_BASELINE.json. |

## Test conditions and limits

The unit has an 8 MiB Sense and a physically 16 MiB LCD with the recorded production partition layouts. Tests use Garage Member Wi-Fi with USB cables connected. Opening a USB diagnostic port can reset a board, so post-case queries are separate from the autonomous observation. USB disappearance alone is not proof of a successful update.

Nearby scheduled tests use an explicitly recorded temporary POSIX timezone to place normal local 02:00 shortly ahead while keeping genuine UTC. They exercise the calendar path; they are not an observed Pacific overnight test. Final service must restore `PST8PDT,M3.2.0,M11.1.0` and verify the next 02:00 Pacific timer.

New-case fixtures are permitted only between closed, archived cases. They preserve ownership, Wi-Fi/authentication, unrelated NVS and the valid fallback. There is no in-case quota refund. Shipping retains its 40-minute daily work allowance, 120-second preflight reservation, network/apply/begin caps and strictly later-UTC-day refill rule.

Controlled USB setup is not an electrical cold boot. Static RAM/RTC and stack-frame comparisons do not prove worst-case TLS/library/RTOS stack margin. An offline factory package does not qualify a production flashing station. Historical bench passes do not establish acceptance of these final bytes.

## Earlier failure retained

The 109→112 scheduled run reached 835,080 LCD bytes before an HTTP no-data failure, then 304,405 bytes before a disconnect with Wi-Fi connection loss. Its armed retry woke at due−12 seconds with `origin:false` and cancelled without a second network reservation. The canonical record was DEFERRED generation 8. The missing-origin readiness rejection is reproduced and corrected in 114; the cause of the first network stall is still unconfirmed. The evidence does not establish USB or heap as its cause.

Version 112 was not promoted, tagged or adopted. Earlier 102→103 and 105→106 failures, the historical 98→99→100→101 bench passes, and the separate Pacific restoration are retained in `RELEASE_BASELINE.json`. They are not reclassified as current acceptance.

## Completion

After actual acceptance, restore Pacific scheduling, promote the exact 114 pair with served-byte readback and previous-pointer archival, commit/tag accurate release records, and safely adopt the release in the original checkout. Close temporary hardware/cloud owners and the finite awake assertion. Keep the old recurring soak automation paused. No unrelated remote Git push is part of this release.

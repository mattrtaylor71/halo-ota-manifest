# Production OTA acceptance — released 6.4.114

**Accepted and published on September 10, 2026.** Manual and normal-calendar installations reached the same exact production 114 pair. Both boards finished SDK VALID and idle, with Pacific scheduling and paired natural sleep restored. Production latest advanced to 114 at **16:56 PDT**.

The compiled source is commit `1224f28ab2d1307d54cf71956b30da2bbccead3d`, firmware tree `83c178bde2020c4e2fff800caf9e33be33a4ef97`. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) binds the exact artifacts, acceptance, publication, recovery package and checkout records. Later release-documentation commits do not change the compiled source identity.

## Accepted scope and results

The user directed a one-hour release effort covering a manual 113→114 update and a separate normal-calendar 113→114 update, followed by final health, accounting, Pacific restoration and production publication. Version 113 was a private-route baseline with shipping policy; both updates installed the exact production 114 bytes. Production promotion completed at 16:56 PDT, after the internal 16:52 target and within one hour of the user’s 15:57 instruction; final Git handoff followed. Extended fault and soak testing was deferred.

| Check | Verified result |
|---|---|
| Native readiness regression | PASS. The observed old-source failure was reproduced; the corrected real-source readiness code and canonical codec passed the focused cases. |
| Exact 113 and 114 builds | PASS. Canonical shipping flags, source provenance, hashes, partition fit and resource comparison verified. Version 114 uses production endpoints; bench, one-shot and fault controls are disabled. |
| Portable 114 recovery package | PASS. Exact application images, ELF/maps, bootloaders, partitions, flash arguments and provenance included; package and ZIP contents verified. |
| Controlled manual baseline | PASS. Both boards booted exact 113/app1, SDK VALID, with Home/idle and later paired natural USB absence. Provisioning and the valid 109 fallback were preserved. |
| Manual 113→114 installation | PASS. One supported LCD serial request at 16:20:55.588 PDT was followed by paired natural sleep. Subsequent health confirmed exact 114/app0, SDK VALID and Home/idle on both boards. Physical menu-button coverage is not claimed. |
| Manual durable accounting | PASS before the verification query. The archived native record is RESOLVED, phase 8/generation 6, with admission at 16:21:00 and resolution at 16:25:59. It records reserved 0, one network window, one apply attempt and one begin per board, with 14 ms remaining. The record predates the post-case USB health intervention. |
| Normal-calendar 113→114 installation | PASS. The separate case was scheduled for 16:40:45 PDT. Before verification, telemetry reported exact 114 on both boards with `nightly_20260910` origin. The final query confirmed both boards SDK VALID and Home/idle. |
| Scheduled durable accounting | PASS on the verification wake. The final native record is RESOLVED, phase 8/generation 6, reserved 0, one network window, one apply attempt and one begin per board, with 13 ms remaining. Its 16:50:35 timestamp follows the health intervention; pre-query durable resolution is not proven. |
| Final Pacific service restoration | PASS. Only `halo_prov/tz` changed to `PST8PDT,M3.2.0,M11.1.0`; all 768 canonical policy bytes and unrelated NVS values were preserved. Both boards returned SDK VALID/idle and entered paired natural sleep. Saved timers select September 11 at 02:00 Pacific for Sense, with the normal 15-second LCD lead. |
| Production publication | PASS. The canonical LCD and Sense latest manifests were promoted and verified against the exact 114 artifacts, with previous-pointer protection. Hardware and cloud owners closed. |

The scheduled observer began after the opening wake transition. Before the final query, Sense was naturally asleep and both boards had reported 114, while LCD remained enumerated. The opening transition and that earlier LCD sleep boundary remain unobserved. Final paired natural sleep was verified after Pacific restoration; it does not retroactively fill those gaps.

## Runtime change

The final correction addresses retry cancellation at 12 seconds before due when the separate LCD timer-origin notice is absent. A persisted shipping ARMED record and current correlated peer may wait within the existing 15-second lead interval. The function still returns false before due, preserves the missing-origin diagnostic and retains the original deadlines, target checks, busy guards and accounting.

The native regression covers missing origin, exact-due reservation, no early or duplicate debit, invalid clock/storage/peer state, deadlines and caller busy guards. Earlier transfer-cleanup and retry-arm corrections remain in the compiled source. See the [readiness investigation](OTA_RETRY_NEXT_FIX.md) for the observed failure and source rationale.

## Conditions and limits

The tested unit has an 8 MiB Sense and a physically 16 MiB LCD with the recorded production layouts. Tests used Garage Member Wi-Fi with USB cables connected. Opening a diagnostic port can reset a board, so post-case queries are distinct from autonomous observations. USB disappearance alone is not update-success proof.

The scheduled case used an explicitly recorded temporary POSIX timezone to place normal local 02:00 nearby while retaining genuine UTC. This exercised the calendar path, not an observed Pacific overnight interval. Final Pacific scheduling was restored and its next timer verified.

The two cases used separately authorized, archived fixtures between closed runs. These preserved ownership, Wi-Fi/authentication, unrelated NVS and valid fallback images. No in-case quota refund occurred. Shipping retains its 40-minute daily work allowance, 120-second preflight reservation, network/apply/begin caps and strictly later-UTC-day refill rule.

Extended repeated soak, deliberate integrity faults, controlled network interruption, physical power cuts, USB-free operation and observed later-day recovery are deferred, not passed. Controlled USB setup is not an electrical cold boot. Static RAM/RTC and stack-frame comparisons do not prove worst-case TLS/library/RTOS stack margin. The offline recovery package does not qualify a production flashing station.

## Historical evidence and handoff

The earlier 109→112 scheduled run reached 835,080 LCD bytes before HTTP no-data failure, then 304,405 bytes before a disconnect with Wi-Fi connection loss. Its armed retry woke at due−12 seconds with `origin:false` and cancelled without a second network reservation; the canonical record became DEFERRED generation 8. The readiness rejection is reproduced and corrected in 114. The initial stream-stall cause remains unconfirmed; the evidence does not establish USB or heap as its cause.

Version 112 was not promoted, tagged or adopted. Earlier 102→103 and 105→106 failures and historical bench passes remain historical evidence in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) and the [changelog](../CHANGELOG.md). They are not reclassified as acceptance of 114.

Use the [release baseline workflow](RELEASE_BASELINE.md) for future source, build, tag and checkout work, and the [factory and recovery guide](PRODUCTION_FACTORY_RECOVERY.md) for the exact portable package and preservation rules. Release/tag/adoption receipts are recorded separately from artifact source identity. The recurring soak automation remains paused; no unrelated remote Git push is part of this release.

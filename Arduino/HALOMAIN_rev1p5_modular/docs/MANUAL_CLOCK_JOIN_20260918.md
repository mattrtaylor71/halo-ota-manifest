# Manual update waiting for fresh time — September 18, 2026

The user reported a persistent “Checking for updates” screen while the unit still ran Sense190/app1 and LCD191/app1, both SDK VALID. The published194 limit fix had not been installed.

USB capture showed live UI and repeated valid, correlated LCD readiness responses. Sense was associated with Wi-Fi, with observed signal roughly −76 to −85 dBm. The readiness window expired at Sense uptime120,955ms; Sense sent `OTA_UNLOCK` with `peer_unavailable`, the LCD returned Home eight seconds later, and both boards slept. The final Sense log explicitly reported `no fresh confirmed clock`. No transfer was observed in the captured interval. Capture began about65 seconds after boot, so the initial time-sync failure and exact manual trigger were not captured.

The production ordering explains why the check waited: a manual tap joined an already pending recovery, but `nightly_maintenance_tick()` called `self_retry_boot_admit()` before the helper that requests the manual time-sync retry. The policy readiness check requires a fresh clock, so it could not reach the retry helper. The ordinary awake alternative also depended on MQTT connectivity. A valid LCD response could not satisfy the clock gate, and the generic deadline result incorrectly blamed the peer.

The scoped correction moves the existing manual clock helper ahead of policy readiness for a joined manual request, after the existing foreground, provisioning, peer and Wi-Fi guards. It retains the original readiness deadlines, the single bounded secondary time-sync opportunity, and all policy/transfer checks. Deadline reporting identifies a missing fresh clock only when the manual request has a current healthy peer proof; genuinely missing/stale peer cases retain their existing result.

This is separate from194’s manual allowance and completed-calendar handoff changes. It does not change Wi-Fi credentials, scheduling, saved media or the OTA record format. Build, test and installation results must be recorded separately; this diagnosis alone is not a device-fix claim.

- [Captured failure, recovery and limits](/Users/MattTaylor/halo-manual194-20260918/manual-check001/REVIEW.json).
- The late diagnostic request after the boards slept was refused by the capture owner. Its error receipt is retained, and all descriptors closed.
- The subsequent193 USB service chain failed its wake/identity stage: the actuator reported a completed stroke, but no board enumerated and zero board descriptors opened. No firmware or NVS writes occurred. See [the stopped chain](/Users/MattTaylor/halo-manual193-20260918/installation-001/RESULT.json).

The integrated readiness test now executes the actual full maintenance tick, manual request, clock helper, policy readiness and terminal cleanup with modeled network responses. Its440 checks pass, alongside the actual SNTP/terminal suite and postboot settlement tests. A negative control using the prior committed maintenance tick fails at the missing secondary retry, reproducing the ordering bug. These are focused host results, not a full release gate or hardware acceptance.

The user has been asked to wake the unit to Home for USB access. Do not reuse the failed193 case directory or claim193 installed. Unused195/196 were inventoried for a private Sense bootstrap and later paired public bump respectively; these are candidate roles until actual build and publication receipts exist.

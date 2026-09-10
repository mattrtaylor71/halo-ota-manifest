The bounded-wait change described below is implemented in source commit `1224f28ab2d1307d54cf71956b30da2bbccead3d`. Its focused native regression and manual installation of production 114 have passed. Current scheduled acceptance and release status are recorded in `RELEASE_BASELINE.json` and `docs/PRODUCTION_RELEASE_ACCEPTANCE.md`; this historical design note is not a separate acceptance result.

# Next focused retry-readiness fix

The 109 → 112 scheduled case did not complete. The first window reported `lcd_proxy_failed_defer`, then ARMED phase 5/generation 7 for retry at **1789078722 (15:18:42 PDT)**. Its second wake reported `not_due`, `accepted_origin=false` at **1789078710 (due − 12 seconds)**, with fresh time and 116733 ms left in the existing readiness opportunity. The subsequent report at **1789078726 (due + 4 seconds)** still had one network window, one APPLY attempt, two LCD begins and zero Sense begins. This report precedes final sleep and does not establish the final canonical phase.

Saved evidence: `retry-window-diagnostic-analysis001/RESULT.json` and its immutable inventory excerpt; full private input is the saved `calendar112-observer-cloud-062/events-response.raw` in the established final-QA directory. The earlier D3 record proves ProxyCleaned, but supplies no transfer error code, accepted byte count or socket diagnostic. Do not infer the first transfer failure's cause from that milestone.

## Minimal proposed behavior

For a valid persisted shipping ARMED record, fresh clock at or above its high-water mark, current correlated paired readiness, and a due time within the existing 15-second LCD lead, preserve the current readiness opportunity while waiting until due even if the timer-origin latch is absent. Keep the latch and report its actual value; do not fabricate accepted origin or relabel the wake.

Before due, **return false unconditionally**: no transfer entry, preflight reservation, policy mutation or network charge. Keep both original deadlines, require the due time to fit strictly within their remaining interval, and preserve the normal user-busy, provisioning, transport ownership, current peer and local VALID checks. At or after due, use the existing admission/reservation path, target binding and budget caps. Expired, malformed, stale-clock, unavailable-peer and out-of-window cases retain their refusals. No extra wake, lease renewal, budget refill or target change is needed.

This wrapper guard is essential: `DurableOtaPolicy.h:349` currently allows `reserve_preflight` when `now + kPeerLead >= fast_due`. The codec itself therefore does **not** prove that reservation waits until the exact due epoch.

## Real-source regression recipe

Extend the existing native `tools/test_retry_wake_ready.py` approach, extracting the actual `halo_policy_boot_ready`, observation and accepted-origin functions and using the real codec/arm/reservation functions. Build a valid modeled ARMED target112 record with due1789078722, preserve its full encoded preimage, and start with **no accepted timer origin**. Do not seed the latch true as the prior positive test did. Keep actual current paired readiness and an unchanged original deadline with 116733 ms remaining.

1. At1789078710, the current b6 code must reproduce `not_due`, origin false and cancellation. The proposed behavior must instead keep pending, return false, make no reservation/cancellation, and preserve the canonical bytes and deadlines.
2. At due−1, still return false with identical bytes/counters. At due, permit existing readiness admission; invoke the real reservation only then. Verify exactly one additional network-window charge, unchanged target/day allowance and no refunded work. At due+4, the same pending sequence must have reached its admitted path rather than retained the cancellation recorded by the failed case.
3. Exercise no notice at all, a late notice, and an exact pending retry notice whose due predicate was evaluated before fresh SNTP was serviced. Cover changed queued reasons without assuming an accepted origin. Keep separate negative cases for stale clock/high-water, due−16, expiry, invalid record, non-VALID/local storage, absent/legacy/not-ready peer, active user/transport ownership, and due equal to or beyond remaining readiness. Test caller guards at their actual boundary rather than pretending the policy helper alone checks user activity.
4. Disable native core dumps and reap the bounded native child. No firmware build or hardware run is part of this proposed regression.

## Why origin may be missing: source possibilities, not observed cause

References below are relative to the frozen `production-release109-source001/source` snapshot, committed from b6ff853e4e3aa4f014a45a48230351bbed802d7c.

- `halo_sense_prod.ino:5972–5974` loads/services canonical policy before normal peer service. The unloaded-cache hypothesis is therefore not established for the normal loop; `ota_peer_ready` also invokes peer service again at3938.
- Existing coordinator debt queues `coord_recovery` at6760. That path can obtain a correlated peer query before an `OTA_PEER_READY` notice has populated the origin. The notice is a separate UART message (`Sense_Minimal.ino:2954–2962`); its bounded mailbox rejects malformed/non-TIMER/zero-boot input or a new message while one is pending (`halo_sense_prod.ino:3261–3271`).
- LCD notice emission requires its committed active origin, boot-ready state and receiver-wait interval, and stops while `ota_locked` (`halo_lcd_prod.ino:293–324`). OTA_LOCK ends the receiver-wait interval while retaining the stored origin (`LCD_Minimal.ino:557–565`). Thus a missed initial notice need not be repaired by later notice repetition after the lock. A query alone is not proof that this separate origin notice was accepted.
- `nightly_maintenance_tick` calls `ota_peer_ready` before its first `halo_prod_kick_time_sync` (`halo_sense_prod.ino:4272–4288`). The kick services SNTP (`2817–2820`), and `Sense_Minimal/sense_time.h:307–325` can change fresh=false to true at that point. A notice can fail its due/clock admission during peer service, then `halo_policy_boot_ready` sees fresh ARMED state and origin false in the same tick, cancelling before a subsequent service can accept the pending notice. This is a viable ordering, not a captured event.
- Accepted-origin additionally requires matching notice/seen/current queried boot, exact arm ID, TIMER cause, active ready nonlegacy peer (`SenseDurablePolicyRuntime.h:511–523`). b6 removed only the queue-label condition. The actual readiness telemetry does not identify which remaining conjunct was false.

No autonomous retry UART payload or exact notice/query/SNTP ordering was captured. The best discriminating evidence is the complete existing `OTA_PEER_READY` payload, correlated query replies, OTA_LOCK ordering and SNTP-fresh line from the same early retry wake. A later USB query resets/perturbs that sequence and cannot supply historical proof. The bounded wait proposal addresses the demonstrated premature cancellation without claiming any of these unobserved sequences as the root cause.

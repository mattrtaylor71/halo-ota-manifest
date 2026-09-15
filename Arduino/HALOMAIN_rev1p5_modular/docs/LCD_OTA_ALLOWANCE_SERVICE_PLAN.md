# LCD-accessible OTA check allowance: proposed design

Status: design only. No firmware, defaults, ledger, device, or publication changed. Camera162 remains the already-built and published candidate. Sense USB is now available for its separate installation; this proposal is for future serviceability.

## Recommendation

Add a small **separate service-check ledger** on Sense and an LCD service interface. Keep ordinary daily discovery at its present default of two windows. Expose three clearly different operations:

- **Status:** ordinary checks used/limit, extra service checks used/limit, UTC budget day, next eligible normal wake, remaining work, apply/BEGIN limits, target/debt, and any reason an extra check cannot proceed.
- **Set daily check ceiling:** persistent, bounded ceiling for ordinary plus explicitly requested service checks. Proposed initial range 2–8, default2; the maximum is a product choice, not existing firmware behavior. Changing it does not run OTA, reset counters, move the schedule, or increase work/apply budgets.
- **Check once with a temporary grant:** authorize exactly one named manual service request, with a short expiry and a compiled hard ceiling. It does not permanently raise the setting or guarantee installation. It may still refuse for insufficient work, unfinished/blocked target state, apply/BEGIN limits, freshness, or peer readiness.

The additional allowance should initially apply only to closed ordinary DISCOVERY or RESOLVED records already eligible for explicit manual discovery. Preserve the original DISCOVERY identity; replace a completed campaign only through its existing checked transition. Refuse active reservations, legacy debt, deferred/armed retries, quarantine, and bench/one-shot states. This solves the reproduced “two unsuccessful/no-update checks used, no target or BEGIN” service problem without turning an administrative check into a retry-debt override. Broader servicing of an unresolved target is a separate decision.

Scheduled OTA retains its current behavior and never automatically spends service grants. Existing 40-minute work budget, preflight and request deadlines, fast-opportunity cap2, apply cap2, BEGIN cap4 per board, slow-path limits, selected VALID proof and exact immutable target validation remain authoritative. More checks can consume the remaining work and leave insufficient time to install; the LCD must say so.

## Why a separate setting alone is insufficient

`DurableOtaPolicy.h:185–213` validates the actual record and rejects ordinary `network_windows>2`. Admission also has literal ordinary/slow-path caps in `DurableOtaDiscovery.h:77` and `DurableOtaPolicy.h:347–351`. Merely changing a setting or `network_cap()` does not produce a correct implementation.

The canonical record is768 bytes with a whole-record CRC. Versions1–3 reject unknown versions and nonzero unused bytes (`DurableOtaPolicy.h:480–544`). The nominal140-byte tail is fully assigned in version3:112 bytes one-shot plus28 bytes bench. Silent reserved-byte reuse is invalid. Even a deliberately new shipping-only format would be rejected by fallback158/159 firmware. Raising the existing stored count above2 would also be rejected by those readers.

## Proposed storage and transaction

Use one bounded versioned/CRC-protected service blob in the existing `ota_coord` namespace, alongside unchanged `retry_v1`. The blob holds persistent ceiling and revision, control sequence, relevant core budget day, service checks consumed, and the last request ID/digest/outcome plus its bound core campaign/generation/hash. Include explicit size/schema/range checks and commit/readback under the existing writer/capacity lease. Reserve both initial allocation and replacement peaks while preserving the96-entry essential floor. Missing service state means ordinary defaults and **no automatic extra authority**; initialization requires a fresh explicit authorized operation. Unreadable/corrupt service state disables service operations; never erase or reset it as recovery.

Keep `retry_v1.network_windows` honest as the count of ordinary reservations. Record extra administrative reservations only in the service ledger and expose both counts and their total; never decrement, refund, or silently saturate the ordinary count. Add one narrow service-reservation path that changes the core phase/work reservation using the existing rules while leaving its ordinary network counter unchanged. It must not call an ordinary reservation and undo the increment afterward.

For each extra check:

1. Under the shared writer/admission lease, load exact current core and service state; validate fresh clock, current boot/peer proof, idle/no active ownership, request authorization/expiry, identity, usage and remaining work. Recheck both after any blocking operation.
2. Commit and exactly read back the **consumed service grant** first. Bind it to the actual core preimage, operation, request and budget day. A duplicate cannot debit or execute again.
3. Commit/read back the core work reservation against that same preimage. Only then fetch a manifest. The service path retains the original absolute readiness/work deadlines.
4. If step3 fails or power disappears between steps, the grant may be lost. Report consumed/no work or unknown as evidence warrants; never automatically refund or infer a completed reservation. Recovery requires a newly authorized request. If the core reservation committed, its existing reset reconciliation retains the charge.

This is deliberately conservative two-record ordering, not an atomic multi-key transaction claim. A fallback to old158 can still decode/reconcile `retry_v1`, cannot spend service allowance, and naturally refuses more ordinary checks at cap2. It ignores the new key. Returning to newer firmware must reconcile the service record against the actual core day/identity; an old fallback can have advanced the core meanwhile. Never reset service usage solely because wall time crossed UTC midnight: bind rollover to the core's legitimate later budget day and preserve control sequence/replay history. Lowering a ceiling below usage blocks further checks without making historical state corrupt.

## Control authorization and replay

LCD is a relay; Sense decides and persists. Message IDs, CRC, a claimed `manual` reason and a nonce alone are not authentication. Bind every mutating command to a fresh Sense challenge, device identity, current paired boot identities, core generation/full hash, service revision/sequence, exact requested operation/value, UTC day and short expiry. Return a request-correlated receipt only after readback. Persist replay sequence even when a campaign or day changes; repeated delivery returns the same receipt or a refusal, never a second grant.

The recommended first scope is local service through physical LCD USB, which matches the accessible connector in the assembled unit. Dashboard control is a later, separately authenticated transport. A local LCD service panel/physical confirmation can authorize local service, but this trusts the LCD and physical access and is not remote authentication. For commands submitted through a service utility, use a device-verified signed authorization with an explicitly managed service authority. The current B1 HMAC machinery authenticates outbound diagnostic POSTs (`DiagnosticAdmissionAuth.h:5,27–33`); diagnostic observations explicitly are not policy authority. It is not an existing inbound quota-control protocol, and optional lab/provisioned diagnostic secrets must not silently become a production dependency. A new service command domain and authorization/provisioning contract are required if that cryptographic route is chosen.

## Alternative: versioned core ledger

A new explicit codec could place service counters/grants inside one768-byte shipping record, atomically consuming a grant with the work reservation. This gives cleaner accounting but creates migration and rollback work: preserve every v1–3 field, reject incompatible bench/one-shot conversion, update shape/codec/capacity/readback consumers, and first install/validate readers capable of the new format in all applicable fallback images. Keep writing the old format until that compatibility is actually established. Do not assume one new active image makes the old fallback reader safe. Choose this later if service functionality grows; the separate ledger is the pragmatic first implementation for the limited extra-discovery operation.

## Finite validation before activation

1. Ordinary default behavior and existing scheduled/manual regressions remain identical with absent service state.
2. Exhausted ordinary count2 plus a valid service grant reserves once, keeps count2, increments service usage, charges real work and can install only within existing apply/BEGIN limits.
3. Active/deferred/armed/quarantined/legacy or mismatched target states refuse without replacing debt; wrong boot/identity, stale clock and insufficient work also refuse.
4. Duplicate/reordered/expired/wrong-signature controls cannot create extra authority, including across reboot and budget rollover; setting reductions preserve already-used counts.
5. Fault injection before/after each set/commit/readback, between service debit and core reservation, and after reservation: no network before both proofs, no reused grant, no invented refund.
6. Corrupt/truncated/unknown service schema and NVS-capacity failure preserve ordinary core/history and fail service closed.
7. Exact old158 decode/reconcile of every emitted core postimage; boot fallback and return with changed core day/campaign remain conservative.
8. Actual LCD-only status/configure/one-grant round trip, duplicate UART delivery, interrupted reply and natural sleep; separately verify physical update and unchanged daily Pacific scheduling.

Source reviewed: camera162 worktree `Arduino/HALOMAIN_rev1p5_modular/halo_ota_demo/firmware/shared`. Core policy, discovery, runtime and control files byte-match frozen158. Key references: `DurableOtaPolicy.h:16–20,116–118,185–230,331–404,480–564`; `DurableOtaDiscovery.h:38–85`; `DurableOtaPolicyNvs.h:22–46,61–65,83–117`; `SenseDurablePolicyRuntime.h:181–203,279–299`; `DiagnosticAdmission.h:4–5`. This is a proposal, not a completed compatibility or security qualification.

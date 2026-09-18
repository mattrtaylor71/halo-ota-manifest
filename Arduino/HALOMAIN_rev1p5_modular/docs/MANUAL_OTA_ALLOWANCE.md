# Manual updates without a daily tap limit

A deliberate Manual Update request authorizes a new, bounded OTA attempt even when the previous daily allowance is exhausted. Each attempt still uses the existing durable reservation, checked storage, clock and board-readiness gates. Repeated taps while an update is active do not start overlapping transfers.

This is an explicit user grant, not unlimited background work. Scheduled wakes do not grant fresh allowance through the manual path. Automatic retry timing, per-attempt transfer limits, the daily 02:00 Pacific schedule, image validation and rollback protections remain in place. A manual retry of unfinished work must retain its exact target; it cannot replace quarantined, armed or active work.

The grant uses the existing policy record format so retained firmware can still decode it after rollback. Manual attempts continue to use ordinary accounting after their explicit grant; they are not a separate uncharged ledger. A failed or interrupted attempt preserves its reservation/debt for normal recovery.

The first update from older firmware still runs that older firmware's admission rules. A quota-only service grant is insufficient if an older completed policy record coexists with a different unfinished calendar check. The manual recovery path now hands that due check into fresh discovery only after proving current board health, ownership, storage and clock readiness. It retains the pending origin and obtains new manifests before reporting completion; it does not mark the pending check successful merely because older firmware once completed.

That old-state combination requires a separately recorded Sense USB bootstrap before this unit can download the fix. Publication, the bootstrap and the user's actual OTA result must each be recorded from their own evidence. A later public version than the private bootstrap lets the user test a real transfer on both boards.

The unchanged 1,106-case media custody suite previously passed in 173.6 seconds under its 180-second aggregate timeout. Two candidate192 runs exceeded that bound without reporting assertion failures. Its aggregate limit is now 300 seconds; individual child bounds, test cases and assertions are unchanged. Both failed192 receipts are retained; a complete passing gate on the final snapshot is still required.

Release and validation receipts will be added after the new paired build and publication complete. This design note does not claim a completed device OTA.

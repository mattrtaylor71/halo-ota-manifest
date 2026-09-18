# Manual updates without a daily tap limit

A deliberate Manual Update request authorizes a new, bounded OTA attempt even when the previous daily allowance is exhausted. Each attempt still uses the existing durable reservation, checked storage, clock and board-readiness gates. Repeated taps while an update is active do not start overlapping transfers.

This is an explicit user grant, not unlimited background work. Scheduled wakes do not grant fresh allowance through the manual path. Automatic retry timing, per-attempt transfer limits, the daily 02:00 Pacific schedule, image validation and rollback protections remain in place. A manual retry of unfinished work must retain its exact target; it cannot replace quarantined, armed or active work.

The grant uses the existing policy record format so retained firmware can still decode it after rollback. Manual attempts continue to use ordinary accounting after their explicit grant; they are not a separate uncharged ledger. A failed or interrupted attempt preserves its reservation/debt for normal recovery.

The first update from older firmware still runs that older firmware's admission rules. If its allowance is exhausted, a separately recorded one-time service grant is needed before installing this change. Publication, that service grant and the user's actual OTA result must each be recorded from their own evidence.

Release and validation receipts will be added after the new paired build and publication complete. This design note does not claim a completed device OTA.

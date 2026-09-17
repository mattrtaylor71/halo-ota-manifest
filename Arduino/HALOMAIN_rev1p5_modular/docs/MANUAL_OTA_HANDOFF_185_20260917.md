# Manual OTA loses the display lease after menu navigation

On September 17, the user physically navigated to Settings and pressed Manual
Update on Sense183/LCD180. The display stayed on Checking for update until the
existing two-minute readiness deadline. Both boards subsequently slept normally.

## Captured cause

Sense acquired an automatic preflight lease on the LCD. The user's navigation
touch correctly released that lease, preserving foreground input priority.
Sense still held `g_peer_gate.locked=true`. The explicit manual request joined
the existing readiness episode without refreshing this stale ownership state.
The LCD repeatedly returned a correlated, SDK VALID, ready reply with an empty
owner and zero remaining lease. Sense rejected these replies until its original
deadline and returned `peer_unavailable`.

This attempt connected to Wi-Fi at about -50 dBm and obtained fresh SNTP time.
It never reached manifest discovery or either firmware download. A separate
DNS/SNTP cancellation finding is outside this correction. The original earlier
user-reported failure was not captured from its start and is not assigned this
cause retrospectively.

Raw evidence: `/Users/MattTaylor/halo-manual184-allowance-20260917/manual-repro001`.
The passive capture closed all descriptors with no errors or injected commands.
Manual request: Unix 1789680205.934870; fresh clock: 1789680208.764065;
peer deadline failure: 1789680323.166594; both boards slept by 1789680346.285998.

## Narrow correction

The first explicit manual attachment to an unentered readiness episode clears
only the cached ready/locked state and advances the nonzero lock sequence.
Sense then sends a fresh lock and requires the LCD's ownership echo. An explicit
manual join can reacquire a timer-origin lease after touch cleared its waiting
notice; the original correlated LCD boot identity is still required.

Repeated requests do not renew the manual latch or either readiness deadline.
Entered work, update application, UART binary ownership, ongoing continuation,
and expired episodes remain excluded. Daily allowance, retained campaign debt,
work accounting, OTA manifests, user-input priority, media custody, and the
02:00 Pacific production schedule are unchanged.

## Validation status

The composed host regression executes the production Sense request/readiness
tail, JSON lock encoder, LCD lock/cancel handlers and durable policy entry.
It passes 308 checks, also under AddressSanitizer/UndefinedBehaviorSanitizer.
The frozen183 negative control reproduces the stale lock and original-deadline
failure through 250 empty-owner replies. The surrounding readiness, calendar,
policy and discovery suites are required again on the committed snapshot.
UART delivery, task timing and storage transport are host doubles.

Candidate185 build, installation, and hardware acceptance remain pending.
This document does not claim a successful transfer. Public184 remains immutable
and contains the previous runtime. Frozen158 remains the retained historical
scheduled-OTA qualification.

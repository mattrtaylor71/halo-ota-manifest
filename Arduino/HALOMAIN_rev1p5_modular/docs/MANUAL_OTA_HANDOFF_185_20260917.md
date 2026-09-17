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
failure through 250 empty-owner replies. All seven required suites passed on the
canonical185 snapshot: manual touch handoff, manual readiness join, manual OTA,
future calendar wait, scheduled OTA clock, postboot policy settlement and OTA
discovery recovery. UART delivery, task timing and storage transport are host
doubles; these results do not establish physical transfer acceptance.

## Release and installed state

Source `5ce4b9615c8fdfa48a5a3f927e581c3ec1073c88`, build
`6.4.185-20260917T213753Z-5ce4b9615c8f`, is sealed as pair SHA256
`01bce790d85da7bcbb79f410c899d33434aa1d91d4ea8678df137414fc9427b9`.
Both canonical builds and artifact checks passed. Public185 manifests and complete
binary readbacks match that pair. Public184 remains immutable history.

Controlled USB service installed only Sense185 in app1 and preserved Sense183
app0, both full-bank backups, NVS/allowance/debt, partition table, bootloader and
LCD180 app0. The original released Sense boot logged the exact185 build/app1 and
marked SDK VALID with current readback at10,131ms. The service receipt's conservative
SDK-health-pending status is supplemented by that separate raw observation.
That service initially left **Sense185/LCD180**. The subsequent manual003 case
updated LCD180 to185 over OTA. Both boards now run185 app1 / SDK VALID.

The earlier explicit one-check allowance grant was not consumed by the captured183
reproduction: its cloud report received at21:25:45UTC retained generation9,
network_windows1, empty target, zero reservation and zero apply/write begins.
Accounting headroom does not independently prove future clock/peer admission.
The185 service and publication performed no allowance or schedule mutation.

The first two185 controller cases, `manual001` and `manual002`, stopped on
interleaved console identity/setup JSON before sending any manual OTA request.
Both captures closed cleanly. They are setup-only harness failures, not firmware
failures or successful OTA attempts.

## Successful manual003 acceptance

One actuator wake was followed by the ordinary LCD encoder and manual-update
handlers through the USB console. One manual request reacquired a fresh LCD lease
with sequence2 and progressed to manifest discovery in3.545 seconds. LCD session4243
began in6.647 seconds and accepted transfer to app1 in13.231 seconds.

The first transfer attempt delivered all2,029,184 bytes. Its SHA256 matched
`50133df54df745c80a1c6a3a542294548753fb95fa029a6d7931bb0b6b3924ea`
at266.726 seconds; LCD marked SDK VALID at268.637 seconds. Fresh cross-board
identity confirmed both185 app1 / SDK VALID. Firmware readback verified
`lcd_ota_due=0` and `done_ids`; the terminal result arrived at274.635 seconds.
Home returned, its overlay cleared and its LVGL heartbeat advanced. Both boards
slept by289.413 seconds. Passive capture remained quiet for15.032 seconds after
sleep, with no USB reopening; all descriptors closed cleanly.

The final cloud event received at22:00:59UTC corroborates native policy
**RESOLVED**, generation14, network_windows2, apply_attempts1, LCDbegins1,
Sensebegins0, zero reserved work and zero retry times. Its retained target hash
belongs to the already-installed Sense185 image, not the LCD transfer. Cloud does
not expose SDK state or every hidden NVS key; direct serial evidence supplies
SDK state and completion bookkeeping. No post-run raw NVS dump was performed.

The next normal wake remains September18 at**02:00 Pacific** (epoch1789722000),
with the LCD timer15 seconds earlier. No quota or schedule change was made for185.
This qualifies the repaired manual handoff and one real LCD OTA after USB-installed
Sense185. It does not qualify a Sense OTA, scheduled transfer, physical navigation
geometry, USB-free operation, power-cut recovery or a whole-product regression.
Original lock/deadline bytes were partly missed; preservation of the original
deadline is host-tested. Historical diagnostic-store warnings remain outside
this correction. Frozen158 retains its historical scheduled-OTA qualification.

## Evidence

- [Sealed pair](/Users/MattTaylor/halo-manual-handoff185-20260917/candidate185-001/RELEASE-PAIR.json), [seven snapshot suites](/Users/MattTaylor/halo-manual-handoff185-20260917/candidate185-001/host-final001/RESULT.json), [publication readback](/Users/MattTaylor/halo-manual-handoff185-20260917/PUBLISHED185.json).
- [Closed Sense service](/Users/MattTaylor/halo-manual-handoff185-20260917/service001/result.json) and [original released SDK-health log](/Users/MattTaylor/halo-manual-handoff185-20260917/service001/sense-released.raw:127).
- [Captured183 reproduction closure](/Users/MattTaylor/halo-manual184-allowance-20260917/manual-repro001/RESULT.json), [explicit grant](/Users/MattTaylor/halo-manual184-allowance-20260917/grant001/result.json) and [subsequent cloud ledger](/Users/MattTaylor/halo-device-analytics-2026-09-10/manual184-20260917/cloud-diagnostics003/CORRELATION.json).
- [Setup-only manual001 controller result](/Users/MattTaylor/halo-manual-handoff185-20260917/manual001/controller-result.json).
- [Setup-only manual002 controller result](/Users/MattTaylor/halo-manual-handoff185-20260917/manual002/controller-result.json).
- [Final manual185 acceptance](/Users/MattTaylor/halo-manual-handoff185-20260917/MANUAL185-ACCEPTANCE.json), SHA256 `b869a20e1faa03d2d14fca1c19a3cedfa37b7c3059d8704a42b7e0cd017eaba8`.
- [Independent device review](/Users/MattTaylor/halo-manual-handoff185-20260917/manual003/INDEPENDENT-DEVICE-REVIEW.json), SHA256 `e1f6de9b4d26c8333ed5fa705edf43a38a35b77fac7b430224acb270c5894c66`.
- [Final cloud correlation](/Users/MattTaylor/halo-device-analytics-2026-09-10/manual184-20260917/cloud-manual185-final001/CORRELATION.json), SHA256 `ef766ee03cc705ca54a4d1fd06874d3f3a6d09efade559e3829f43e3fc7f4b90`.

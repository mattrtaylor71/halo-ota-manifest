# Background maintenance must not renew display inactivity

September 28, 2026. Matt observed the main menu remaining lit for about 30
seconds after completing the private 226 manual shopping-list test. He asked
to stop and review the capture and fix this delay. This is private development;
public OTA is not authorized by this request.

## Observed cause on 226

The closed passive capture contains real touch/encoder input, no host device
commands or actuator taps. It records:

- Last Refresh accepted by Sense at host epoch 1790640134.966413. This updates
  Sense `last_wake_ms`, even though the Sense did not reboot.
- Home shown at 1790640135.975856.
- Background `MAINT_KEEPALIVE` messages every two seconds while the refreshed
  25-second schedule-delivery window remained active. The last arrived at
  1790640158.033262; request ID was absent, not `img_spool`.
- LCD panel/backlight off at 1790640168.047531: 32.072 seconds after Home, but
  exactly 10.014 seconds after the final keepalive.
- LCD deep sleep about 3.080 seconds later, after normal sleep coordination.

The LCD keepalive handler calls `resetActivityTimer()`, updating both user
activity and Home's idle origin, and extends the shared OTA awake deadline by
eight seconds. That deadline also blocks panel darkening. Thus removing only
the activity reset would leave another incorrect keep-lit condition. No actual
manual OTA, firmware download or apply was observed. The generic `OTA` log state
is not proof of an active firmware update.

The future `MAINT_WINDOW` arm path has the same nonvisual schedule-delivery
purpose and must receive the same separation. Active-now maintenance and real
OTA ownership retain their existing behavior.

## Manual 226 test review

The capture contains four Settings taps, four Shopping List taps, eight complete
refresh/idle transitions, six refresh joins (two physical Refresh presses during
an actual voice POST) and three acknowledged user-item deletions. It contains
no captured panic, reboot, watchdog or allocation-error signature. Pixel motion
and physical vibration are not measured by serial logs.

The user's voice job 185, 106,496 bytes, returned read timeout -11. The device
committed the recording durably and armed a 300-second retry; no device 202 was
recorded before capture stopped. The stop was requested by Matt and occurred
just before the expected retry. The observed quiet interval is 288.925 seconds,
not six complete minutes; subsequent recovery must not be inferred.

A later bounded cloud lookup matched session `halo-d45c-d3f1-1-41473` to one
completed 106,496-byte job and one worker attempt. The backend recorded a second
receipt for the same request after this native capture had closed. Thus the
recording reached processing, but this cloud evidence does not prove the
device received its acknowledgement or removed its saved recording. The initial
native timeout remains a failed attempt, not a successful upload assertion.

Six intact memory phase samples from that failed voice attempt reached minima
of 41,332 B internal free, 33,668 B DMA free and 17,396 B largest DMA block.
Internal and DMA views overlap and must not be added. Qualified upload-worker
unused stack was 5,564 B. The scoped allocator reported no external allocation
failures or default fallbacks. These are bounded samples of one failed attempt,
not continuous minima or a full resource qualification.

## Correction and validation status

The implementation, host validation, canonical paired build and paired bench
installation are complete. Device logs measure the panel darkening at 10.011
seconds despite five natural keepalives. The extra physical touch during the
background hold remains untested: its guard refused movement after Sense had
already slept. This is a qualified timing result, not a complete pass of that
two-part test. The correction gives
`MAINT_KEEPALIVE` and the future `MAINT_WINDOW` arm grace their own renewable
eight-second CPU/UART lease. They no longer renew the user's display timer or
qualify as a real OTA keep-lit hold. The existing foreground idle path may darken
the panel while the lease prevents sleep teardown. Schedule receipt/storage/ACK,
active-now maintenance and real OTA protection retain their existing paths.
Real input still makes the UI visible and starts a new foreground interval.

Sleep intent, guardian, normal sleep handshake, final teardown admission and
headless maintenance sleep must honor the same lease. Expiry and renewal use
atomic state; final admission shares the existing short coordinator lock with
keepalive acceptance. A late maintenance renewal must decline final sleep
admission before any UI state is changed, without treating that message as
user input. This is specifically
tested alongside a coincident real touch. Neither the Sense sender, 02:00
schedule, upload policy, retained media nor production manifests are changed.

### Resource ownership

The new state is one 32-bit atomic deadline and one boolean sleep-defer flag on
the LCD. The compiled LCD static DRAM increase is eight bytes including layout
effects; Sense static DRAM and both boards' IRAM are unchanged.
There is no dynamic buffer, PSRAM migration, new task, larger stack or queue.
The UART receiver renews the deadline; main-loop/headless sleep admission reads
and expires it. A short existing coordinator critical section serializes final
sleep admission and renewal; it performs no I/O, NVS access or allocation.
Actual OTA retains its own hold and resource ownership. This fixes visible
idle time, not Wi-Fi connection or cloud-response time. CPU activity may still
outlast the dark display while existing communication work completes.

All 125 working-tree offline suites passed, including 20 new production-function
cases. The exact 226 source fails the repeated-keepalive and future-arm negative
controls at the unchanged-user-timer assertions. Dependent tests cover real
touch, actual OTA display ownership, media custody and provisioning sleep.
An independent read-only source review found no blocking issue in the final
diff. These host tests do not prove an on-device timing result or a new OTA.

Working gate: `/Users/MattTaylor/halo-background-display-20260928/working-gate227/RESULT.json`,
SHA256 `24f4dcd76def48190f62e872b8e0e70d7c5e28a7e9cd0f85bc82e3cca75de6cf`.
Review: `/Users/MattTaylor/halo-shopping-incident-20260928/keepalive227-readiness/INDEPENDENT-SOURCE-REVIEW.json`,
SHA256 `2541214a05d76e12372d5c91463ead29bf988ca5bbd751e99d712e2eba2764be`.
Only this documentation was updated after the working gate before the source
commit. The canonical candidate's own exact-snapshot gate also passed all 125
suites, with unchanged tested source. SHA256 of that gate is
`e48a319a1d92f932e464a91ebc0ca172cae25da9e5567c93cbcf038f5fd50194`.

The built private candidate is 6.4.227, after a fresh complete inventory
found no occupied 227+ version and live manifest hashes matched the original
224 publication receipt. Different new bytes now require an unused 228+ version
after fresh inventory. Preserve
226 as the compiled baseline and rollback; retain the approved 225 RAM and
226 refresh/haptics changes. No factory erase or saved-media removal is needed.

## Built private candidate

- Source: `4602b6b105f7dca43ea4a7de2f2a2823029da8f5` on `codex/ram-qualification`.
- Build: `6.4.227-20260929T002631Z-4602b6b105f7`, epoch `1790641591`.
- Mini candidate: `/Volumes/Trepo-Work/Workspaces/halo-firmware-bench/candidates/6.4.227`.
- Pipeline `20260928T172854-35127be1` passed the full snapshot gate, paired
  canonical builds, locked-environment recheck, actual artifact checks and
  candidate verification. No hardware actions or cloud writes occurred.
- Readiness receipt SHA256:
  `c2fbe25a6111d915649c80ea29c2a7daf8301ebf88ada31e376421912b7b9a53`.

| Board | Application bytes | Change vs 226 | OTA slot margin | SHA256 |
| --- | ---: | ---: | ---: | --- |
| Sense | 1,876,336 | -16 | 89,744 | `768795ddc46cbba5f120770bbb1c42025b251abcca63c697e601fb1afac80f22` |
| LCD | 2,033,632 | +352 | 587,808 | `e5e23741aef9cb5d393f99aab62b64b6c83f6228fa8af2518f2017354cb305f7` |

LCD static DRAM is 204,356 bytes (+8); IRAM remains 85,248 bytes. Sense static
DRAM/IRAM is unchanged. Existing unambiguous selected compiler stack frames
are unchanged. The new helper frames are 48/32/32 bytes and the defer helper
32 bytes individually; these are not complete call-chain or runtime peaks.
No task stack was resized. The current static artifact report passes; the
historical resource model still says `REVIEW_REQUIRED` and does not grant
production approval. No recalibration or new runtime RAM qualification is claimed.

Root independently rehashed all 48 files in the compact receipt mirror,
including both binary images and the exact snapshot gate. Build, static-memory,
frame comparison, environment and verification receipts are under
`/Users/MattTaylor/halo-background-display-20260928/build227/receipt-bundle/`.

## September 28 bench results

Both boards now run the exact private 227 build from app0, SDK VALID. Sense
`1C:DB:D4:5C:D3:F0` and LCD `20:6E:F1:A1:2B:74` were identified freshly before
each installation. Each inactive-bank installation verified the complete image,
preserved 226/app1 and its selector, and verified unchanged NVS, partition table
and filesystem ranges. No factory erase, queue deletion, provisioning, schedule,
allowance or public manifest change occurred.

Closed health case 33 independently verified the exact paired build with a fresh
nonce and CRC, settled Home, both mounted image/voice inventories at zero, and
normal paired sleep. LCD LVGL free memory was 79,560 B with a largest free block
of 78,900 B. These are a point-in-time UI allocator reading, not a new full RAM
qualification. The normal September 29 02:00 Pacific maintenance start was armed;
this does not prove scheduled execution. Both ports remained absent for 75.267
seconds after the last close. The current empty stores do not retroactively
prove the off-window acknowledgement/deletion of manual voice 185.

The separate passive timing case 34 sent no board commands. Ten native Home-age
samples maintained a stable idle origin (0.215 ms spread). Five natural
keepalives arrived before the panel/backlight darkened at **10.010845 seconds**.
Fresh heartbeat and sleep-defer rows then confirmed the LCD remained awake for
background maintenance with its backlight and panel off. This directly addresses
the old 32.072-second symptom. It measures display state from firmware telemetry,
not from a camera or light sensor.

The planned additional dark-held physical tap was **not executed**. Sense
entered normal sleep at epoch 1790644482.128179 and its USB closed at
1790644482.312314 while the Uno was starting. The guard then refused with
`Board closed during dark admission`, before any second PUSH. STOP was
acknowledged, motor PWM returned zero, stylus was off, the worker was reaped and
descriptors closed. LCD subsequently entered normal sleep at 1790644490.292450.
Thus the composite case remains `FAIL_OR_INCOMPLETE`; its first timing assertion
is qualified, while touch-during-hold and the second idle interval remain gaps.
Normal physical wake from sleep did pass in the health and timing cases. No
repeat or stronger stroke was used to bypass the refusal.

### Retained setup failures and corrections

- Case 29, before Matt repositioned the device, completed one stroke but found
  neither board within 40 seconds. It opened neither board or sent a command.
  After Matt confirmed placement, case 30 independently admitted paired 226,
  mounted empty inventories and normal sleep. Case 29 remains a failure.
- Separate wake 35 followed by service 31 left a 20.12-second host gap before
  capture. The recorder caught only the LCD's final four seconds; Sense was not
  observed, and cached readiness indicates capture likely missed its awake
  interval. No command, ROM access or flash occurred. Corrected Sense
  case 36 and LCD case 32 used the existing sealed capture-before-tap option,
  retaining every admission and write guard; no controller or firmware changes
  were needed.
- The initial timing launch requested a 65-second cleanup grace beyond the job
  runner's 60-second maximum. It was rejected before a worker or hardware action.
  Only that outer argument was corrected to 60; the sealed finite controller,
  330-second job bound and closed health receipt stayed unchanged.

Raw cases remain outside Git under
`/Users/MattTaylor/halo-ram-campaign-20260928/bench-private/`.

| Closed case | Result | RESULT.json SHA256 |
| --- | --- | --- |
| `followup227-sense-service-20260928-36` | Inactive-bank installation verified; later SDK health proved in 33 | `66d0b471b9b4a257ae61bdcee84c40c9dc05e26a2080c84289572e19b1e40f4d` |
| `followup227-lcd-service-20260928-32` | Inactive-bank installation verified; later SDK health proved in 33 | `c5db16009d1007e732b568db9e61209c623b6870a90435d7cf24b2333e6dd810` |
| `followup227-health-20260928-33` | Paired 227/app0 VALID, empty stores, sleep and 75-second quiet | `a9da9f09f3b0fa6ff3775ff61fdd30737928b7faffd069e87f4000dc7eca8c15` |
| `followup227-idle-timing-20260928-34` | First dim qualified; composite incomplete because extra tap was refused | `800dee8d0e173d0425e1e02e0b8691e970fd20ec5f7550e0e50d18f409f746f4` |

This is a private bench correction, not a new full-product, upload, OTA-transfer
or scheduled-execution qualification. Public paired 224 remains unchanged.
Root receipt readback is saved at
`/Users/MattTaylor/halo-background-display-20260928/ROOT-HARDWARE-READBACK.json`.
Independent review is saved at
`/Users/MattTaylor/halo-ram-campaign-20260928/service227-prep/INDEPENDENT-SOURCE-RESULTS-REVIEW.json`,
SHA256 `861f18b4eca845bbdb10cfcfed6375df8a19b933bc2d3a60df38b309504aed21`.

## Evidence

- Closed manual capture:
  `/Users/MattTaylor/halo-ram-campaign-20260928/bench-private/manual226-observe-20260928-28/`.
- Its `OBSERVATIONS.json` SHA256:
  `8fbc907941795877b8cb5a32f2457febba46edd7ede3a9847f9ff056db814848`.
- Continuation evidence:
  `/Users/MattTaylor/halo-background-display-20260928/`.
- Exact-session cloud findings:
  `/Users/MattTaylor/halo-shopping-incident-20260928/cloud226-manual28/FINDINGS.md`.
- Retained 225 panic and 226 correction:
  [shopping-list investigation](SHOPPING_LIST_PANIC_225_20260928.md).

Shared source consulted: `engineering/halo-resource-testing`. Its dated
calibration does not replace current source, artifact or device evidence.
Raw device logs remain private and outside Git; no company-brain write is made.

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

The implementation, host validation and canonical paired build are complete;
installation and device timing validation remain pending. The correction gives
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

## Hardware still pending

A bounded 226 readiness observation (case 29) completed one actuator stroke,
but neither exact board appeared over USB within 40 seconds. The recorder
opened neither board and sent no board command; the capture and actuator closed
cleanly and released the hardware lease. No flash or retry was attempted.
Physical contact and saved-recording cleanup remain unverified; Matt was asked
for one manual wake. A later single passive inventory still found neither Halo
USB port. Neither board has been flashed to 227. The last verified installed
pair is 226/app1 SDK VALID. The next step is fresh paired identity and mounted
queue admission, then state-preserving inactive-bank installation and a finite
natural keepalive/dim/user-tap test. Keep the failed case 29; do not repeatedly
stroke or erase pending media to bypass it. The offline build is complete.

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

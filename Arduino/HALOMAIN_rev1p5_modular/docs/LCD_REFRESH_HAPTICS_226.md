# Private 226: restore refresh feedback and disable haptics

September 28, 2026. Requested by Matt after testing private 225. Production OTA
remains 224. Version 226 was inventoried unused before preparation; publication
requires a separate explicit release request. The paired build is now verified
and ready for bench testing. It is **not installed or published**; neither Halo
USB interface appeared during the two bounded wake attempts described below.

## Why these fixes were missing

Private 225 was deliberately based on published 224 plus the reviewed Sense RAM
changes. The earlier private LCD fixes were not ancestors of that source:

- `26b9a5ae2f8676ef5d804eae422218cd760f78ff` (216) removed LVGL rendering
  from the UART-owned refresh-state transition.
- `0ff079c0afba6ffe931ee1095e52589b8274c830` (217) revealed feedback when an
  explicit refresh joined the silent refresh started on list entry.
- `567eb2a955c7d4cbd483fccae513931f989eb2b0` disabled LCD haptics.

This was an omitted integration, not evidence that those fixes stopped working.
The 217 device results are historical, documented in commit `bae9b7b1a42b84aba3a487573398a6adfa3c4213`;
they do not qualify 226. Preserve these restored LCD changes alongside the 225
RAM work in future private candidates; do not rebuild from production alone and
silently omit them. Do not merge unrelated private 216–222 changes wholesale.

## Changes and resource ownership

When a silent list refresh is pending or in flight, a user refresh now reveals
the existing border sweep. It keeps the original request, deadline and animation
phase, so repeated taps do not start duplicate network work or extend a stuck
request indefinitely. It does not promise faster Wi-Fi or voice processing.
The UART transition only publishes refresh state; existing UI-task rendering
under the LVGL owner lock draws it. No new task, queue, UI object or payload
buffer is introduced. The existing animation allocator and cleanup remain in use.

Touch and encoder haptic playback routes and their queued haptic event are
removed. After existing I2C0 initialization, the driver receives standby first,
then GO=0 and RTP=0, preventing a retained prior effect from continuing across an
MCU reset. Each of the three writes and three readback transactions is bounded
at 20 ms; missing or inconsistent readback is explicitly unverified. There is
no allocation or retry loop. The `haptics` USB diagnostic only reads registers;
it never configures playback. Driver address, pins and peripheral power control
are unchanged. Standby readback is evidence of driver state, not a vibration
sensor measurement.

Sense runtime, RAM optimizations, upload custody/retries, OTA implementation,
production 02:00 Pacific schedule and account behavior remain unchanged from225.
Both versions will be stamped together by the canonical candidate builder.

## Validation and evidence

The actual 225 functions fail the restored regressions for the expected defects;
the restored code passes the focused tests. Coverage includes joining both wake
pending and in-flight refreshes, repeated taps, completion/error, silent entry,
recreated UI objects, no nested rendering, active-driver shutdown and partial
I2C failures. These are behavior tests using production functions, not proof of
physical animation smoothness or radio timing.

The full working-source gate, exact-snapshot gate, paired canonical build,
artifact/resource comparison and finite affected-device checks are required
before declaring this private candidate ready. Retain all original failures.
Bench scope: real voice upload with list refresh, repeated refresh coalescing,
read-only standby verification after interaction, responsive UI and bounded
sleep/wake observation. Broader 225 RAM results retain their original limits.

Evidence roots:

- `/Users/MattTaylor/halo-refresh-haptics-20260928/source-audit/`
- `/Users/MattTaylor/halo-ram-campaign-20260928/haptics-fix-20260928/`
- `/Users/MattTaylor/halo-ram-campaign-20260928/followup226-prep/`

The existing border design is retained. Company source
`brand/trepo-brand-guidelines` was consulted; this change introduces no new
branding, icon, typography or copy and requires no brand exception. Shared
resource guidance: `engineering/halo-resource-testing`; live Git/build/device
receipts take precedence over dated shared snapshots for deployment state.

## Built candidate and current qualification

- Compiled source: `1ce0d04e95b1b2a039b27364a616a535642cd10b` on
  `codex/ram-qualification`; build `6.4.226-20260928T221036Z-1ce0d04e95b1`.
- Mini candidate:
  `/Volumes/Trepo-Work/Workspaces/halo-firmware-bench/candidates/6.4.226-recovery01`.
- Full working-source gate: all124 suites pass; result SHA256
  `a3736afe54f7319fa01426f36ad80db7d5fba5c7e8a0cf74483ee9dc0aab47ab`.
- Full immutable-snapshot gate: all124 suites pass; result SHA256
  `f3a93762678621e7eab2459d0195ed894c11432231811174282ece900b311194`.
- Canonical paired compilation, unchanged locked environment, artifact checker
  and candidate verification pass. This qualifies build integrity, not hardware.

| Board | Application bytes | SHA256 | OTA slot margin |
| --- | ---: | --- | ---: |
| Sense | 1,876,352 | `ccb96ab2242959572ebdd8a552073296bf97b514e3524f62ee66b047555eb823` | 89,728 |
| LCD | 2,033,280 | `6a4282f3a818595a87eda90a64aa344c60732daaccf88a686bf598440d907aaf` | 588,160 |

Compared with225, Sense static RAM/IRAM/RTC sections are unchanged and its image
is16 bytes larger. LCD static DRAM is8 bytes smaller and its image128 bytes
smaller; IRAM/RTC sections are unchanged. The compiled LCD loop, UART task, UI
task and refresh-trigger frames remain320,1648,800 and32 bytes respectively.
These are individual compiler frames, not call-chain peaks or runtime free
memory. No226 device resource measurement has been made. The historical resource
model's source/calibration drift remains separate; it was not recalibrated to
manufacture a passing result.

The first build job `20260928T151217-4371082a` was interrupted before compilation
when the user moved the USB-C hub carrying the SSD. Matching macOS SIGBUS crash
reports prove force-unmounted executable backing files. Original candidate/run
and partial gate evidence remain intact. After complete storage, toolchain,
source and snapshot checks, fresh job `20260928T152837-5989e913` built the same
source/version/epoch in the separate recovery directory. See the
[Mini recovery notes](MAC_MINI_BENCH.md).

Physical limits are explicit: baseline attempt14 completed one actuator stroke
but saw no Halo USB arrival; it opened no board ports and made no media requests.
After the hub move, only the proven Uno/relay location fields were rebound.
Recovery15 received controller READY and completed one stroke, but again saw no
Halo interface during40 seconds. All actuator workers closed and the lease was
released. No firmware write, device command, NVS change or media capture occurred.
The last qualified installed pair remains225/app0; a new live identity check is
required before the prepared inactive-app1 service can run. Preserve225/app0.

Remaining acceptance: manual wake/USB access, exact paired installation, actual
voice/list refresh feedback, driver standby readback after interaction and
bounded sleep/wake. There is no226 physical refresh, vibration, upload, OTA or
full-functional pass. Production manifests were freshly read as paired224;
no cloud writes were performed.

Compact build receipts and static/frame reports:
`/Users/MattTaylor/halo-refresh-haptics-20260928/build226/recovery01/receipt-bundle/`.
Current continuation checkpoint:
`/Users/MattTaylor/halo-refresh-haptics-20260928/STATUS.json`.

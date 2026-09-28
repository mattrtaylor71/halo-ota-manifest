# Private 226: restore refresh feedback and disable haptics

September 28, 2026. Requested by Matt after testing private 225. Production OTA
remains 224. Version 226 was inventoried unused before preparation; publication
requires a separate explicit release request. Build and device results are
pending at this source checkpoint and must be recorded from actual receipts.

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

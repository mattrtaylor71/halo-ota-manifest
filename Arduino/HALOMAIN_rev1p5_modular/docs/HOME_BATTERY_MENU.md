# Home battery indicator — implementation and historical bench evidence

**Publication update, September 29:** exact233 is now published for both boards and fully read back. See [RELEASE_233.md](RELEASE_233.md) for current identity and qualification limits. The dated private-candidate narrative below records its pre-publication state; it does not override the current release.

September 28, 2026. Matt requested a bottom-edge battery arc and percentage on
the existing five-card Home menu, with a bright green glowing lightning bolt
replacing the arc and number when external power is detected. This work is a
private candidate after 230, not a production publication. Public OTA remains
224. Private paired 233 is now installed on the identified Mini bench unit;
the focused build/navigation/power-read/sleep checks passed September 29.
It removes transient dashes and widens the 100% display range at Matt's request.
The previous 232 results below are retained as historical evidence. Physical
unplug/transition review and electrical percentage calibration remain separate.

## Display behavior

The latest reference-matched preview restores the Dish card to y24 and puts
Check-in/Mic/Discard at y130. The More card is 92×80 at y232, making space for a
large bold percentage while preserving all five original icon artworks. Other
cards remain 92×92. Actual touch bounds and voice hold feedback share those
coordinates and dimensions. The More card's shorter height is a local layout
change explicitly accepted for matching Matt's latest reference, IMG_5858.

A **70-degree** rounded lower arc is inset **12 pixels** from the round display
edge. A pale teal remainder sits behind its filled segment. The centered
**Nunito22** number appears as `78%`, matching the photo; it remains an estimate,
not a calibrated measurement, and the review page/documents say so explicitly.
The lightning symbol means **external power**, not verified charging current,
a full cell or remaining battery percentage.

| Display state | Appearance |
| --- | --- |
| Estimated 26–100% | Teal-green arc and number matching the supplied reference |
| Estimated 11–25% | Amber arc and number matching the existing microphone/dots |
| Estimated 0–10% | Red arc and number |
| External power | Bright green lightning bolt with a static layered glow; no arc or percentage |
| Unknown or stale | Blank on startup; a previously confirmed indicator may bridge a transient gap for at most 1.5 seconds, then disappears |

The exact overlapping user boundaries are resolved as **25% amber, 10% red**.
The filled lightning silhouette uses bright local green `#2EA86A`, with a
thin, low-opacity static glow. Two overlapping convex triangles avoid LVGL's unsupported concave-polygon
path. The glow is drawn from fixed vector strokes, not a new animation, image buffer
or periodic wake. The indicator cannot renew the user's activity timer, turn on
the panel, keep the device awake or interfere with foreground input.

## Measurement and estimate policy

The existing `lcd_power.h` service reads GPIO1 / ADC1 channel 0 through the
Waveshare board's nominal 10kΩ/10kΩ divider. It calibrates 32 readings and reports
**system supply mV**, not direct cell voltage. `HomeBattery.h` is a presentation
model consuming that cached observation. It does not alter ADC ownership or
acquisition, `SystemPower.h`, cloud fields, OTA admission, charging or shutdown.
The cloud still honestly reports `battery_percent:null`, `cell_mv:null` and
`power_source:unknown` under the existing system-supply schema.

The model requires a received, valid 32-sample measurement, known age no greater
than five seconds, and a rail between 2500 and 5500 mV. That broad plausibility
window is not a cell-safety range or calibrated battery range. Missing, invalid,
stale or implausible input produces Unknown and resets the model history; the
LCD-only bounded visual hold described below does not change that raw result.
Older sequence numbers or regressing sample uptime within one boot are refused.
A new boot or a gap longer than five seconds resets filtering and source state.
Duplicate reads do not advance the filter.

External power starts at **4400 mV** and remains selected until the rail reaches
**4300 mV or lower**. These are provisional thresholds with hysteresis. A USB data
connection can veto a low-rail battery estimate but never assert 5 V or active
charging. A non-external reading above 4250 mV is also Unknown. After rejecting
an ambiguous sample, removal of the USB data flag cannot reinterpret that same
cached sample as battery power: the model waits for a new ADC sample.

The private lookup is deliberately explicit and bounded:

| System rail mV | Estimated display percentage |
| ---: | ---: |
| 3300 or lower | 0 |
| 3500 | 5 |
| 3600 | 10 |
| 3700 | 25 |
| 3800 | 50 |
| 3900 | 75 |
| 4000 | 90 |
| 4100 | 97 |
| 4150 or higher, when otherwise admitted as battery | 100 |

Intermediate values use rounded linear interpolation between adjacent anchors,
clamped to 0–100. Below 4150 mV the result is capped at 99, so rounding cannot
move the full-display threshold. The top anchor was lowered from 4200 mV at
Matt's request: the old curve displayed 99% over 4150–4183 mV, so this small
plateau includes that entire former 99% range. Lower anchors through 4100 mV
are unchanged. This is display rounding, not proof of a fully charged cell.
A causal median of up to five distinct valid samples feeds a
fixed-point low-pass with approximately ten-second settling scale. Source
changes clear that filter rather than blending USB voltage into battery history.
This is a **provisional system-rail estimate, not a calibrated state-of-charge
curve or remaining-runtime prediction**. The anchors do not establish a battery
warning/shutdown policy and must not become OTA or power-safety thresholds.

## September 29 display transition revision

Unknown on initial Home entry is blank. A previously confirmed bolt or battery
reading may bridge a temporary unknown observation for at most 1,500 ms from
the **first** unknown observation; subsequent unknown observations cannot
extend that deadline. A fresh battery reading crossfades from a confirmed bolt
over 240 ms. A reading arriving near the hold deadline shortens the fade so
the old bolt cannot outlive the original 1,500 ms budget. Reconnecting confirmed
external power replaces an in-progress fade immediately.

Home unload, dark/headless/sleep/OTA/provisioning states, a new ADC boot or an
owner-loop observation gap over 350 ms reset transition history. A later visit
cannot replay an old bolt. Persistent unknown readings become blank, never an
invented percentage or an unconfirmed lightning bolt. Only primitive draw
opacity changes; neither user activity nor the underlying validity/power state
is modified.

Matt reported 99% after unplugging installed 232. That visual observation is
consistent with the old lookup's top range, but the finite USB capture received
no bytes, so no exact unplug voltage is claimed. New electrical observations
remain separate from the exhaustive estimator and native renderer tests.

## Evidence and remaining calibration gap

The closed September 28 discharge handoff for a different LCD contains 216 USB
samples at 4614–4656 mV and 18,276 battery-only samples at 2728–3944 mV. The proposed
4400 mV threshold separates those recorded segments, but this is not fleet-wide
or cable-drop validation. The initial 3944 mV was not a confirmed full battery;
the final low-voltage reboots are not an approved empty endpoint. No battery
current, cell-to-rail reference measurement or validated charge-complete signal
was captured.

The provisional table may be substantially pessimistic for this particular
power path. In that high-load experiment the rail reached 3700 mV after about
1 h 12 min, with about 3 h 59 min remaining to the first reboot, while this table
would display approximately 25%. Those times are experiment-specific and do not
establish true battery state of charge either. The plain percentage follows
the requested photo; removing the tilde does not strengthen its accuracy claim.

The current bench has also reported about 3056 mV while USB was attached.
Historical notes attribute roughly 3.3 V with the LCD switch off to Sense
regulator/backfeed power. The intended unit's physical LCD power-switch state
and direct-USB/battery/backfeed behavior still need confirmation. This is why a
USB data connection vetoes a low-rail percentage. It does not detect all possible
power-only USB or Sense-only backfeed arrangements.

Smallest remaining physical check: observe the intended unit with its normal
LCD switch on under LCD USB power, battery-only power and Sense-USB-only power,
then compare the battery connector and system rail with a meter at ordinary
charge levels. Do not repeat the diagnostic deep-discharge tail as a UI test.
A qualified percentage later requires confirmed full/usable-empty anchors and
repeat observations of this pack and power path under normal sleep/wake loads.

Closed source evidence:
`/Users/MattTaylor/halo-voltage-probe-20260928/handoff-20260928/FIRMWARE_HANDOFF.md`
and its immutable `data/readings.csv` (SHA256
`b95c035811b7cc7c1646c5f9bdb2a57ff1bcf6079fa59fa7c6a1cafb75f9e8ec`).
The separate voltage probe and live recorder are protected; this feature does
not operate, reset, flash or change their collection services. See also
[SYSTEM_POWER_TELEMETRY.md](SYSTEM_POWER_TELEMETRY.md) and the local audit at
`/Users/MattTaylor/halo-battery-menu-20260928/measurement-audit.md`.

## Resource and ownership review

`HomeBattery.h` owns a fixed five-value ring and scalar presentation state,
with a compile-time bound of 40 bytes for the model. It has no heap allocation,
clock read, task, queue, network request or persistent write. Its median sort
uses five `uint16_t` values on the stack; arithmetic is bounded and integer based.

`lcd_home_power.h` borrows the existing synchronized power snapshot. Existing
LVGL owners check the observation at most every 250 ms while Home is visible,
initialized, lit and outside sleep, provisioning, headless maintenance and OTA
modes. A 240 ms transition uses the existing owner loop to redraw only the
footer at most every 40 ms, plus its completion frame. It introduces no timer,
task, animation object, widget, layer or canvas. A fixed 28-byte motion record
tracks presentation continuity separately from the unchanged 40-byte model. The draw callback does no sampling, USB inspection,
clock, UART, activity or power work. Home owns its event descriptor; there is
no new widget or touch target. LVGL may still use its existing rasterization
scratch allocation for drawing primitives, so "no new canvas" does not imply
zero renderer memory use.

The indicator redraws its lower-screen region when mode, band or displayed
percentage changes, or for those bounded transition frames. Final paired artifact deltas, compiler frames and actual
Home resource samples must accompany the private build. The separate completed
230 voice/list results do not qualify this new renderer or its resource usage.

## Validation status

The root's focused model run reports 23,252 assertions passing after adding the
same-sample USB detach regression. Those are host results, not battery accuracy
or physical UI acceptance. Coverage includes exhaustive monotonic percentage
mapping, 25/10 boundaries, stale/invalid values, external hysteresis, USB veto,
source/filter reset, duplicate samples, boot/uptime/sequence handling, sustained
change and preservation of the unchanged cloud power schema.

Independent source review identified the pre-detach cached-sample problem; it
was corrected to await the next existing ADC sample. No further material pure-
model issue was found in the reviewed version. The full host gate, canonical
paired build/resource checks and finite bench sleep/wake/input checks remain
separate acceptance steps. Do not call this
production-ready or electrically calibrated based on a host model pass.

The revised focused actual-LVGL renderer run passed **364,912 pixel/lifecycle checks**
with the production LVGL configuration, actual Home creation block, existing
theme/icon helpers, dish bitmap and Nunito22 font. Hardware observations and
clock inputs are host doubles. Native 360×360 PNGs for 100%, 78%, 25%, 10%, 0%,
external power, ambiguous low USB rail and stale input were generated in
`/Users/MattTaylor/halo-battery-menu-20260928/render10/`. The renderer owner
visually reviewed the reference composition, teal/amber states and external-
power silhouette. User review of this latest revision is pending.
`RESULT.json` in that directory records source hashes and the exact scope.

- All 15 existing Home objects remain; every pixel in the five relocated
  card/icon/shadow rectangles matches the relocated reference without an
  indicator. The entire menu remains within the round display aperture.
  All new indicator pixels, including the glow, lie within the compact inset
  footer region and the physical circle.
- Percentage text uses Nunito22 at line y317; its glyph ink sits below the
  More-card shadow and above the arc. The arc has center(180,180), outer
  radius168, width6 and angles55–125. The footer teal is `#2E8C8E`; its track
  blends that color into cream at 20%. Visible geometry and actual touch bounds
  move together. The external-power bolt is 18×26 before its restrained glow.
- Three hundred display-state transitions retain exactly stable LVGL free
  memory. Twenty successive normal Home rebuilds each retain exactly stable
  free memory, unchanged object count and intact allocator state. No animation
  or additional rendering layer is created.
- The 64-bit host measured **24 bytes** for the Home event descriptor,
  **40 bytes** for the fixed model and **8 bytes** across presentation scalars.
  Warmed host LVGL free memory was **83,040 bytes**. These are focused host
  measurements, not ESP32 descriptor size, full-product peak or device headroom.
- The baseline fixture temporarily removes and reattaches the callback after
  card allocation to obtain a clean reference image. Its allocation order
  differs from normal Home construction by eight bytes. The lifecycle check
  normalizes once to the source's callback-before-cards order, reports that
  eight-byte difference, then requires exact equality on every rebuild;
  no heap-growth tolerance is allowed.
- A missing-callback negative control fails the named `battery arc visible`
  assertion. Same-value updates cause no extra refresh, and dark, hidden,
  headless, sleep, provisioning and OTA states do not borrow a sample or render.

The original visual candidate is preserved separately in the immutable 231
source/artifact snapshot; it is not installed by this revision. Earlier failed
render receipts remain in `render01` through `render05`, with the original
compact design's passing run in `render06`. `render07` preserves the rejected
30-degree design. `render08` and `render09` preserve the reference layout and
its focused teal refinement before the final bolt refinement. The
native comparison caught a real 14-pixel glow overlap with More's translucent
shadow, corrected by moving only the bolt two pixels lower. Other failures
were isolated fixture issues: probing inside the valid glow, clearing the
presentation cache without destroying its screen, and the allocation-order
comparison described above. The final checks retain exact pixel and heap
comparisons rather than accepting those failures with wider tolerances.

An independent host harness compiled the actual geometry, hitbox arrays,
hit-test function, action mappings and tap/reset origins. **304,107 checks
passed for the final geometry**, including every logical/raw coordinate,
inclusive edges, gaps, the now-inert old More lower strip, unchanged secondary/
settings actions, and the fixed actuator center remaining inside the mic.
Both Home and muted voice More cards use the same 80px height as their touch
bounds. Restoring the old mic Y only in a generated harness fails the expected
bounds check. Evidence and source hashes are in
`/Users/MattTaylor/halo-battery-menu-20260928/menu-route-review/run-2-final/`.
That external harness is separate evidence, not yet a registered release-gate
suite. Physical touch timing, hold/release behavior and actual-panel acceptance
remain pending; command injection cannot prove them.

## Follow-up focused validation before private 233 build

The revised estimator passed **24,210 assertions**, including the exact
4,149/4,150 mV boundary and exhaustive top-range plateau checks. The actual-LVGL
renderer passed **459,730 pixel/lifecycle checks** and its missing-callback
negative control. Evidence is in
`/Users/MattTaylor/halo-battery-menu-20260928/render12-smooth-bound/RESULT.json`.
It covers blank cold/expired Unknown, a bounded confirmed-state hold, the
crossfade and clipped late-arrival deadline, new boots and interrupted/hidden
Home. All 15 objects, the warmed 83,040-byte host LVGL free pool and repeated
state/rebuild allocation equality are retained. The motion record adds 28 fixed
bytes; the model stays 40 bytes. These replace the focused follow-up figures,
not the historical 232 results below. Canonical build and device acceptance
remain separate pending steps.

## Private 232 installation and focused device result

Completed September 29, 2026, on Sense `1C:DB:D4:5C:D3:F0` and LCD
`20:6E:F1:A1:2B:74` only. Exact source
`e396c8e2cf2f7bc044a86b7b768316d8aa8a4fe9`, build
`6.4.232-20260929T070436Z-e396c8e2cf2f`. Both boards freshly reported app0,
boot app0 and SDK VALID. The previous 230/app1 banks, NVS, partition tables
and filesystems were verified unchanged during the inactive-bank service.
The superseded 231 candidate remains uninstalled. There was no public OTA
publication, factory erase, setup reset or policy change.

All **130** exact-snapshot regression suites and both canonical artifact checks
passed. Compared with 230, Sense section sizes and application size are
unchanged; LCD adds **56 bytes DRAM** and **2,720 bytes application**, with no
IRAM increase. Remaining application-slot margins are 85,104 bytes for Sense
and 573,088 bytes for LCD. Actual individual compiler frames are 192 bytes
for footer drawing, 96 for its service and 48 for the model; the existing UI
task frame remains 800 bytes. These are not whole-call-chain or runtime stack
measurements. The older resource guard remains REVIEW_REQUIRED, not a public
release approval.

One finite device case passed (`PASS_HOME_POWER_NAVIGATION_PANEL_SLEEP`):

- Fresh paired 232 identity and mounted, empty voice/image queues.
- Home → shopping list (11 items) → Home using native console handlers.
- Two cached ADC observations, 3,196 and 3,160 mV, each valid/fresh with 32
  samples. USB was attached; this low system rail is ambiguous, so the expected
  display is Unknown (`--`), not a fabricated percentage or external-power bolt.
- LVGL pool free/largest: 79,544/78,884 bytes initially and 70,800/70,712 after
  list creation and Home return, above the existing 32,768/16,384 review floors.
  The two snapshots include retained list allocations; they do not establish
  a leak trend or continuous minimum.
- Native panel-off at exactly 10,000 ms idle (10.17 seconds observed after the
  Home command), paired deep sleep, zero media-retry arms, then 75 seconds with
  neither USB interface reopening. All capture workers exited and the shared
  hardware lease was free at closure.

The first service attempt stopped **before ROM entry or any write** because an
empty-queue reply was interleaved with startup logging. A separate v2 host
controller permits one additional fresh read only after an unreadable-reply
timeout, retaining exact required fields, new offsets, existing two-query caps
and immediate rejection of an unsafe inventory. Nine unit tests and 29 queue
checks passed; the firmware did not change. The failed receipt is preserved.
One later queue reply had valid required zero counts with an interleaved
optional diagnostic tail; this is not claimed as a wholly clean log line.

Evidence on the laptop is under
`/Users/MattTaylor/halo-battery-menu-20260928/build232/` and `device232/`.
The complete canonical build is on the Mini at
`/Volumes/Trepo-Work/Workspaces/halo-firmware-bench/candidates/6.4.232`.
Closed device receipts are `evidence/home232-sense-service-v2-20260929`,
`evidence/home232-lcd-service-v2-20260929` and
`evidence/home232-navigation-sleep-20260929` under that bench root.

This case does not certify physical pixels/touch, cell percentage accuracy,
every user journey, an OTA transfer or the strict 2 am calendar proof (that
field was unqualified). User power-switch/visual review is a separate capture.

## Private 233 build qualification

Completed September 29 from clean source
`730bdd4cedbfe27e1708dc9e986900c8349024f2`, exact build
`6.4.233-20260929T075757Z-730bdd4cedbf`. All 130 exact-snapshot suites,
locked-toolchain checks and both canonical artifact verifications passed.
The estimator passed 24,210 assertions and the actual-LVGL renderer passed
459,730 checks, including its missing-callback negative control.

Compared with installed 232, Sense's section sizes/application bytes are
unchanged. LCD adds **32 bytes static DRAM** (the motion record itself is 28
bytes), **608 application bytes**, and no IRAM. The LCD slot retains 572,480
bytes; Sense retains 85,104 bytes. Actual service frame grows from 96 to 112
bytes. New event/bolt/reset frames are 128/176/64 bytes; the former draw frame
was inlined/restructured and its absence is not counted as a stack saving.
The model frame stays 48 bytes, UI task 800, voice upload 1,232 and upload
worker 2,064. These are individual compiler frames, not call-chain or device
high-water marks. The historical resource guard remains REVIEW_REQUIRED.

The hash-verified 453-file build mirror, exact binaries and detailed comparisons
are under `/Users/MattTaylor/halo-battery-menu-20260928/build233/`.
`SUMMARY.json` records all artifact/source hashes and paths. This is a private
candidate only. Public OTA remains 224; no production upload or promotion ran.
The validated service plan preserves installed 232/app0 and writes 233/app1
only after fresh exact paired identities and empty mounted queues are observed.
It does not erase NVS, storage, provisioning or the fallback application.
Installation and physical animation/unplug acceptance must be recorded
separately; passing this build does not establish them.

## Private 233 device acceptance

Both intended boards freshly reported exact 233/app1, boot app1 and SDK VALID
in `home233-navigation-sleep-20260929`. Each installation preserved its entire
232/app0 fallback, NVS, partition table and filesystems; only inactive app1 and
the alternate OTA selector were written. No factory erase or public OTA ran.
Three fixed actuator wakes were sufficient for the two services and final case;
no relay reset or user intervention was needed.

The finite Home case passed: Home → shopping list (11 items) → Home, followed
by panel-off at **10,004 ms** idle (**10.09 seconds** observed after Home), native
paired deep sleep and **75 seconds quiet** with neither USB reopening. Both
mounted media queues were empty and retry arms cleared. This run also recorded
a qualified next 02:00 Pacific maintenance arm for September 29, with LCD's
15-second lead; it is timer-arm evidence, not an executed scheduled OTA.
One unreadable initial voice-queue observation required the already reviewed
bounded fresh query; it did not trigger media action or broaden the controller.

The two fresh same-boot 32-sample ADC snapshots were **4,908 and 4,900 mV**,
consistent with the external-power display condition. They do not measure cell
charge or prove charging current. UI free/largest bytes were **79,544/78,884**
at initial Home and **70,800/70,712** after list creation and Home return, above
the unchanged floors and equal to the prior 232 observations. Retained list
allocations explain the two snapshots' difference; they are not continuous
minimums or a leak trend. The captures closed cleanly. Full closed results and
worker/lease closure are retained under
`/Users/MattTaylor/halo-battery-menu-20260928/device233/`.

The host native renderer verifies the visual transition and exact threshold;
this finite device case confirms actual power samples, normal navigation and
sleep. A physical USB unplug/percentage transition remains Matt's visual check.
No new camera/voice/provisioning journey or OTA transfer was claimed from this
UI-focused case. The plain percentage remains an uncalibrated display estimate.

## Design provenance

The current Home screen and the supplied photo govern layout continuity. Shared
sources read through `trepo-company` were `brand/trepo-brand-guidelines`
(original corporate PDF, modified September 15; imported September 23) and
`brand/trepo-app-design-system` (September 23 implementation reference). A search
for relevant brand amendments did not identify a battery-specific amendment.

The established firmware cream background, white cards, existing icon artwork
and Nunito assets are retained. Matt's latest direction to match IMG_5858
supersedes the earlier 30-degree arc and uniformly raised 92×92 cross. The local
footer teal `#2E8C8E` and brighter bolt green `#2EA86A` follow that reference and
the explicit request for a bright lightning indicator. Low states still use
the existing microphone/dots amber and discard red. Those colors affect only
this new footer; no global palette or brand amendment is introduced. No wordmark
is recreated and no company source, logo or shared brand rule is overwritten.
Brand references do not certify the electrical estimate or authorize public
OTA publication.

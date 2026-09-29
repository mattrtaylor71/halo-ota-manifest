# Home battery indicator — private implementation

September 28, 2026. Matt requested a bottom-edge battery arc and percentage on
the existing five-card Home menu, with a bright green glowing lightning bolt
replacing the arc and number when external power is detected. This work is a
private candidate after 230, not a production publication. Public OTA remains
224. Hardware qualification and final compiled resource measurements are pending.

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
| Unknown or stale | Neutral track and `--`; no fabricated 0%/100% or charging claim |

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
stale or implausible input produces Unknown and resets presentation history.
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
| 4200 or higher, when otherwise admitted as battery | 100 |

Intermediate values use rounded linear interpolation between adjacent anchors,
clamped to 0–100. A causal median of up to five distinct valid samples feeds a
fixed-point low-pass with approximately ten-second settling scale. Source
changes clear that filter rather than blending USB voltage into battery history.
This is a **provisional system-rail estimate, not a calibrated state-of-charge
curve or remaining-runtime prediction**. The anchors do not establish a battery
warning/shutdown policy and must not become OTA or power-safety thresholds.

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
LVGL owners call its service with a 250 ms maximum check cadence only while
Home is visible, initialized, lit and outside sleep, provisioning, headless
maintenance and OTA modes. The draw callback does no sampling, USB inspection,
clock, UART, activity or power work. Home owns its event descriptor; there is
no new widget or touch target. LVGL may still use its existing rasterization
scratch allocation for drawing primitives, so "no new canvas" does not imply
zero renderer memory use.

The indicator redraws its lower-screen region only when mode, band or displayed
percentage changes. Final paired artifact deltas, compiler frames and actual
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

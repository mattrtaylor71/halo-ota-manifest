# Idle display visibility correction — 6.4.198

## Published identity and current source

Published September 20 for both boards, with complete public manifest/binary readback. Source is `3826d4ad1858922a245558634cb0d6881acbfc5e`, firmware tree `51357035951277ee3dd1967848261774a07f98f8`, build `6.4.198-20260920T195720Z-3826d4ad1858`. Local annotated tag `halo-v6.4.198` identifies the artifact source; no Git remote push was performed. Future firmware must descend from this source and use unused **6.4.199 or later**. The canonical branch remains `codex/halo-production-baseline-197`; its name does not select old firmware.

| Board | Application bytes | SHA256 |
| --- | ---: | --- |
| Sense | 1,868,224 | `2026acf678b95458c1ff0ca9c290e3e0d429bb2824ca5c71cf11999fe38247f6` |
| LCD | 2,030,064 | `8e6a88cad058586b4f9e4ac3951b689320831b5ea518904321acaecb513b318a` |

Both complete 102-suite gates passed with no skips and unchanged source. Both canonical compilers closed successfully; artifact checks and paired staging/promotion/readback passed. Static RAM and RTC RAM are unchanged from197. The only non-generated production runtime changes from197 are `LCD_Minimal/lcd_anim.h` and `LCD_Minimal/lcd_ui_task.h`.

Receipts: `/Users/MattTaylor/halo-idle-dark198-20260920/RELEASE-RECORD.json`, `working-host001/RESULT.json`, `regression/RESULT.json`, `build/artifact-result-v2-sense-lcd.json`, and `promote001/result.json`. The exact source/artifacts and selected test/publication evidence are checksum-verified in `/Users/MattTaylor/halo-releases/6.4.198` and its sibling tarball/`FREEZE-VERIFIED.json`. Original compiler caches and private device data are excluded. Existing158/196/197 packages are unchanged.

**Device acceptance remains pending.** The user will perform Manual Update and repeat voice capture, list browsing/refresh, idle timeout and a later touch. Confirm the update completes, background activity leaves the screen dark, and touch restores a responsive UI. A scheduled OTA, full-product test, cold-power test or interrupted-transfer hardware pass is not claimed. EOL factory selection remains197; the configured bench is last verified196.

## Problem and scope

After a normal user session timed out, background UART activity could turn the LCD back on without another touch. Maintenance keepalives, future wake arms and late list responses refreshed the same activity clock used by Home; its automatic restoration path did not preserve `g_idle_screen_dark`. Separately, a non-user sleep-transition abort could restore an already dark ordinary session. The native timer-boot guard alone did not cover either case.

The correction keeps an ordinary idle-dark display dark through automatic activity and non-user sleep aborts. Actual touch/scroll still restores the UI immediately through `ensure_awake_for_ui`. Actual OTA presentation explicitly owns visibility. There is no new state, and no change to Sense, Wi-Fi, upload custody/retries, scheduling, allowances or transfer behavior. Preserve the 197 factory-startup correction and all previous production fixes.

## Regression evidence

`tools/test_lcd_idle_visibility.py` executes the actual production activity, UART, list-completion, Home, touch and OTA visibility bodies with observed panel/PWM state. Its 15 cases cover ordinary idle, background keepalive/future-arm/list events, timer-dark controls, denial/transition/handshake controls, touch takeover and actual OTA visibility. `tools/test_lcd_maintenance_sleep.py` adds three ordinary-session abort cases to its existing timer/maintenance cases.

Against frozen 197, the new visibility test reproduces six failures and the ordinary idle-dark non-user abort also fails. All new cases pass with the correction; seven focused related suites also pass. Baseline and focused receipts are under `/Users/MattTaylor/halo-wake-investigation-20260920/code-audit-repro/fixed-regression/`. The handshake-keepalive Home case checks a visibility branch invariant, not evidence that the main loop runs concurrently inside its own blocking handshake.

The working-tree gate and exact-snapshot gate each passed all102 suites. Canonical paired builds, artifact checks and publication passed; actual receipts are recorded above. Device installation and the user's repeat voice/list-to-idle check are separate, pending acceptance.

## Cloud wake reports are not backlight measurements

September 20 cloud reports for the bench Sense show approximate boots at 12:48:31 and 12:49:47 Pacific, matching the previous saved-media sleep intervals of 299 and 59 seconds. The first reports a voice upload, the second no new action, and neither an active OTA. These are actual processor wake cycles, distinct from the reproduced same-boot relight. Retry processors should run with the LCD dark; Sense EXT0 and cloud wake reports do not establish the LCD's wake source or physical backlight state. Do not claim that this patch proves or fixes those particular timer-boot events.

## Release workflow

198 is now immutable. For future work use a reviewed descendant, a freshly inventoried version199+, the canonical materializer/builder, the complete host gate and the verified paired publisher described in `BUILD_AND_RELEASE.md`. Keep the EOL factory selector on197 unless that separate deployment is explicitly performed. The user will install the published pair with Manual Update; no assisted flash or new scheduled-OTA pass is implied.

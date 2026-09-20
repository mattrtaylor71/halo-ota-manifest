# Idle display visibility correction — 6.4.198

## Problem and scope

After a normal user session timed out, background UART activity could turn the LCD back on without another touch. Maintenance keepalives, future wake arms and late list responses refreshed the same activity clock used by Home; its automatic restoration path did not preserve `g_idle_screen_dark`. Separately, a non-user sleep-transition abort could restore an already dark ordinary session. The native timer-boot guard alone did not cover either case.

The correction keeps an ordinary idle-dark display dark through automatic activity and non-user sleep aborts. Actual touch/scroll still restores the UI immediately through `ensure_awake_for_ui`. Actual OTA presentation explicitly owns visibility. There is no new state, and no change to Sense, Wi-Fi, upload custody/retries, scheduling, allowances or transfer behavior. Preserve the 197 factory-startup correction and all previous production fixes.

## Regression evidence

`tools/test_lcd_idle_visibility.py` executes the actual production activity, UART, list-completion, Home, touch and OTA visibility bodies with observed panel/PWM state. Its 15 cases cover ordinary idle, background keepalive/future-arm/list events, timer-dark controls, denial/transition/handshake controls, touch takeover and actual OTA visibility. `tools/test_lcd_maintenance_sleep.py` adds three ordinary-session abort cases to its existing timer/maintenance cases.

Against frozen 197, the new visibility test reproduces six failures and the ordinary idle-dark non-user abort also fails. All new cases pass with the correction; seven focused related suites also pass. Baseline and focused receipts are under `/Users/MattTaylor/halo-wake-investigation-20260920/code-audit-repro/fixed-regression/`. The handshake-keepalive Home case checks a visibility branch invariant, not evidence that the main loop runs concurrently inside its own blocking handshake.

The complete working-tree gate, exact-snapshot gate, canonical paired build and publication are pending at this source checkpoint. Record their actual receipts before calling the release published. Device installation and the user's repeat voice/list-to-idle check are separate, pending acceptance.

## Cloud wake reports are not backlight measurements

September 20 cloud reports for the bench Sense show approximate boots at 12:48:31 and 12:49:47 Pacific, matching the previous saved-media sleep intervals of 299 and 59 seconds. The first reports a voice upload, the second no new action, and neither an active OTA. These are actual processor wake cycles, distinct from the reproduced same-boot relight. Retry processors should run with the LCD dark; Sense EXT0 and cloud wake reports do not establish the LCD's wake source or physical backlight state. Do not claim that this patch proves or fixes those particular timer-boot events.

## Release workflow

Use unused 6.4.198, the canonical materializer/builder, all 102 host suites and the verified paired publisher described in `BUILD_AND_RELEASE.md`. Keep the EOL factory selector on 197. Preserve immutable 158/196/197 artifacts. The user will install the published pair with Manual Update; no assisted flash or new scheduled-OTA pass is implied.

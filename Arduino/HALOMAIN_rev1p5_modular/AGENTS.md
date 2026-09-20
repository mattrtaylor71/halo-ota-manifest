> **Current published firmware: 6.4.198 (September 20).** Use source `3826d4ad1858922a245558634cb0d6881acbfc5e` or reviewed descendants for every future firmware/OTA change. Read `PRODUCTION_BASELINE.json` and [the 198 handoff](docs/LCD_IDLE_DARK_198.md). Both 102-suite gates, paired canonical builds and public binary readback passed; the user's manual OTA and physical screen verification remain pending. EOL factory selection stays 197; historical 158 is recovery only. New builds require unused 199+. The 197/196 records below retain their historical scope.

> Factory startup correction (September18): candidate197 fixes the erased-NVS/setup UART deadlock while retaining quarantine until fresh healthy/idle proof. Both99-suite gates and paired canonical builds passed; the EOL pair received full factory images/readbacks, then passed automatic startup and fresh SDKVALID communication after reviewed reset-only harness recovery. Physical power-cycle/Settings/sleep-wake remain pending; public196 is unchanged. Continue from source `7f87340d7c271bfdfc9b888aa6a0fcbf43822a8e` or reviewed descendants. See [factory evidence](docs/FACTORY_UART_STARTUP_197.md).

> September 18 release: **196 is published and read back for Sense and LCD.** Both canonical builds/artifact checks and all 97 exact-snapshot suites passed. The user’s paired manual OTA passed: Sense 195/app0 → 196/app1 and LCD 191/app1 → 196/app0, both SDK VALID, with verified image hashes, observed target resolution, Home and paired sleep. Capture closed after 60.438 seconds without USB reopening. A further no-update request is untested; no new scheduled-OTA or full-product pass is claimed. See [release evidence and remaining steps](docs/MANUAL_OTA_196_20260918.md).

# HALO Firmware Agent Guide

Current development source is `3826d4ad1858922a245558634cb0d6881acbfc5e`, firmware tree `51357035951277ee3dd1967848261774a07f98f8`, build `6.4.198-20260920T195720Z-3826d4ad1858`. Public paired OTA is 198, tagged `halo-v6.4.198`. Continue from this source or reviewed descendants. The only runtime change from197 is the LCD idle-dark correction; touch/scroll and real OTA visibility remain enabled. User installation and physical acceptance are pending. EOL factory selection stays197 and the configured bench is last verified196. Use an unused199+ after a fresh inventory check. Earlier records below are historical acceptance and limitations.

The finite190 device checks passed: voice/list browsing with8refreshes and28scroll events, then camera interruption of an actual voice POST, exact-job resume to202, successful camera PUT200 and paired sleep. Do not rerun the historical189-to190 installer. Spoken-item semantics, physical gesture geometry and another same-wake camera after the recorded DMA-reserve warning remain unqualified. The same warning exists in188 evidence; it is not newly proved to be a190 regression. Public188, production02:00Pacific configuration, quota and debt are unchanged.

A later physical voice request, "Add gummy worms to my shopping list," was matched to backend insertion, LCD list display and the user's deletion. Its automatic saved retry received an accepted response with one cloud worker execution and dark LCD logs. See `/Users/MattTaylor/halo-voice-list189-20260917/MANUAL-USER190-FIRST-VOICE-REVIEW.json`; this is one manual case, not three repeated cases.

The following178/179 notes are historical qualification limits.178 fixes the observed camera-cancelled Wi-Fi association delay and enables narrowly proved, charged recovery of closed OTA discovery. Its rapid camera/voice checks and two saved image/voice deliveries passed within recorded limits; cloud custody was independently confirmed and retry wakes stayed dark. 179 retains early manual intent and waits for transient list/HTTP ownership inside the original deadline. Two real 179 manual requests fetched both older public 162 manifests and completed checked-pair bookkeeping. Cross-board logs support cleanup; strict test controllers stopped on truncated USB diagnostic lines, and their failures remain recorded. The first check's paired sleep is directly captured. A final ordinary wake confirmed normal unlocked Home, persisted the next 02:00 Pacific LCD arm, and returned both boards to sleep.

At that earlier178/179 checkpoint, public manifests remained162. The older-manifest `downgrade_blocked` result still maps to generic failure wording; this does not qualify equal-version UpToDate UI or an OTA transfer. Early-startup preservation is host-tested; no dedicated physical early-window assertion passed. Preserve provisioning, memory-reserve, USB-connected and full-product qualification limits. The old179 timed installation harness can tap an awake foreground screen and must not be reused.180 used a separate Home identity gate and one initial actuator stroke only when LCD USB was absent. Retain exact private artifacts and receipts; no public release is claimed.

## Release Baseline

- Start future OTA work from `RELEASE_BASELINE.json.current_working_source`, retaining162 camera input recovery,159 wake handling and the158 fixes. `current_baseline` and tag `halo-v6.4.158` identify the immutable recovery/previous scheduled qualification. Read `docs/FROZEN_RELEASE_158.md`, `docs/LCD_USER_WAKE_159.md` and `docs/BUILD_AND_RELEASE.md`. Historical117 qualification does not select the current source.
- Run `python3 -B tools/verify_frozen_baseline.py` before preparing new firmware. Preserve the immutable 158 tag and binaries; new changes need an unused version at least 6.4.199 after a fresh inventory check. The guard verifies committed ancestry and retained bytes, not uncommitted edits or device acceptance.
- Use `docs/PRODUCTION_RELEASE_ACCEPTANCE.md` for finite shipping acceptance. Preserve the distinction between bench evidence, shipping build evidence and actual production acceptance.
- Prepare release metadata from clean committed source with `tools/prepare_production_release.py`, then use the external snapshot's `tools/build_ota_policy_production.py`. Follow `docs/OTA_POLICY_PRODUCTION_BUILD.md`; do not publish repository precommit metadata or substitute a flag-free Arduino build. `publish_both.sh` consumes prebuilt artifacts only.
- Keep `RELEASE_BASELINE.json` and `CHANGELOG.md` synchronized with actual source, artifact, acceptance and publication receipts. Never fill release commit/tag or success fields prospectively.

## Production Targets

- Sense production entry: `halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino`
- LCD production entry: `halo_ota_demo/firmware/halo_lcd_prod/halo_lcd_prod.ino`
- Core device logic still lives in:
  - `Sense_Minimal/Sense_Minimal.ino`
  - `LCD_Minimal/LCD_Minimal.ino`
- Shared production modules live in `halo_ota_demo/firmware/shared/`

## Board Roles

- Sense: Wi-Fi, provisioning, MQTT, OTA orchestration, camera, audio, sleep coordination.
- LCD: LVGL UI, touch/knob input, local sleep handling, LCD OTA client.
- Cross-board link: UART1 at 115200 baud plus GPIO wake signaling.
- Production UART behavior is newline-delimited JSON, not the legacy framed protocol described in `firmware/shared/UartProto.h`.

## Production Folders

- Treat these as source of truth for current production work:
  - `halo_ota_demo/`
  - `Sense_Minimal/`
  - `LCD_Minimal/`
  - `halo_common/`
  - `halomain_assets/`
- Treat demo/reference folders as non-production unless a task explicitly says otherwise.

## Safe Edit Rules

- Change one subsystem at a time.
- Preserve current behavior unless the task explicitly requires a fix.
- Do not casually change pins, baud rates, wake timing, sleep sequencing, OTA URLs, or message names.
- Prefer small extractions behind existing interfaces over rewrites.
- Keep wrapper sketches thin, but do not do big-bang migrations.
- Avoid repo-wide formatting or unrelated cleanup during firmware refactors.

## Module Boundaries

- `firmware/shared/`: shared Wi-Fi, OTA, provisioning, build info, and coordination helpers.
- `Sense_Minimal/`: Sense runtime, camera/audio flows, sleep path, UART receive handling.
- `LCD_Minimal/`: LCD runtime, LVGL/UI flow, UART receive dispatch, sleep/wake behavior.
- `halo_common/`: board identity and cross-board protocol/pin headers.

## Do Not Touch Casually

- Sense sleep entry and wake-source logic.
- LCD LVGL lock ownership between `loop()` and `ui_task`.
- UART JSON schema and coordination messages.
- OTA host allowlist and manifest path logic.
- Camera power, deinit, and hold behavior.

## Validation

- Run the complete offline regression gate after each firmware fix: `python3 -B tools/run_regression_suite.py --out /absolute/path/to/new-results`. See `docs/REGRESSION_TESTING.md` for prerequisites, coverage and finite device acceptance. Focused tests alone are not the release gate; skips and source changes fail it.
- For a new release, rerun the gate using the materialized snapshot's own runner. The paired publisher requires its exact `--host-result` and revalidates it before stage/promote. Do not reuse working-tree or older-source evidence, edit old sealed releases, or bypass the gate.
- Build the production OTA implementation with the canonical entry, which supplies and records the durable policy/diagnostic flags and verifies actual FQBN/partition/UI settings:
  - `python3 tools/build_ota_policy_production.py --out /absolute/path/to/new-build`
  - Add `--board sense` or `--board lcd` for one target; `--plan` prints the commands without compiling.
- See `docs/OTA_POLICY_PRODUCTION_BUILD.md` for feature defaults and compatibility-build limitations.
- Run focused hardware checks for the subsystem you touched.
- Minimum post-change smoke check:
  - both boards cold boot
  - UART handshake succeeds
  - one user-visible action on the affected board still works
  - sleep/wake still works if the change touched power, UART, OTA, or maintenance logic

## Refactor Approach

- First extract pure helpers and duplicated constants.
- Next isolate protocol helpers and state-machine logic.
- Leave hardware-heavy paths for later, smaller passes with explicit hardware validation.

## Actuator bench control

Use the bounded exact-device `tools/tapctl.py` for the current replacement Uno; preserve `PUSH:500,200,500` at speed128. `--action probe` is nonmoving; `--action wake` requires a fresh Sense USB transition. Inspect receipts and stop on uncertain worker ownership. The relay is absent from the current wiring; the old relay helper pins a different Uno. Three physical cycles plus a final idle/reopen passed after a full USB/12V reset, but the original intermittent fault is not proved permanently cured. See [current evidence](docs/ACTUATOR_INVESTIGATION_20260918.md).

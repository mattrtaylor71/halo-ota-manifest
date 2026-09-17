> September 17 release update: **6.4.185 is public and installed on both boards**, from `5ce4b9615c8f`, build `6.4.185-20260917T213753Z-5ce4b9615c8f`. Both canonical builds/artifact checks and seven exact-snapshot suites passed. Sense185 was installed by controlled USB service; the subsequent **manual LCD180 to185 OTA passed on its first transfer attempt**. Both boards reached app1 / SDK VALID, returned Home and slept. Native cloud policy reported RESOLVED with zero reservation and retry times. The production **02:00 Pacific** schedule is unchanged; this is not a new scheduled or Sense OTA qualification. Continue from185 or reviewed descendants, using unused version **186 or later** after fresh inventory. Frozen158 is unchanged. See `RELEASE_BASELINE.json.current_working_source` and `published_manual_test_release_185`.

# HALO Firmware Agent Guide

Current code starting point is `5ce4b9615c8fdfa48a5a3f927e581c3ec1073c88` (185), firmware tree `d9dabd3599764b3c3088a2492c5eda17411e3b1d`. The only runtime delta from sealed183 is the Sense production wrapper: an explicit manual OTA request can reacquire the LCD preflight lease after navigation touch released it, with a newer correlated sequence and fresh ownership echo inside the original deadline. Allowance, retained debt, touch priority and the production schedule are unchanged. Seven exact-snapshot suites and both canonical build/artifact checks passed; the sealed pair is `01bce790d85d`. Public185 readback passed. The full183 working-source record is retained as `previous_working_source_183`.

The installed unit is **Sense185 app1 / LCD185 app1**, both SDK VALID. Sense-only USB service preserved Sense183 app0 and protected storage; manual003 then updated the LCD from180 to185 using the production OTA path. One request reached manifest discovery in3.545s, accepted the LCD transfer in13.231s, verified the exact image hash and restored Home; both boards slept by289.413s, followed by15 seconds without USB reopening. Cloud reported generation14, RESOLVED, network_windows2, apply_attempts1, LCDbegins1, Sensebegins0, reserved0 and next normal wake September18 at02:00 Pacific.185 performed no quota reset. Earlier manual001/002 stopped on console parsing before any OTA request; preserve them as setup failures. This does not qualify a Sense OTA, scheduled update, physical menu geometry, USB-free operation or power-cut recovery. Final receipt: `/Users/MattTaylor/halo-manual-handoff185-20260917/MANUAL185-ACCEPTANCE.json` (SHA256 `b869a20e1faa03d2d14fca1c19a3cedfa37b7c3059d8704a42b7e0cd017eaba8`). Read `docs/MANUAL_OTA_HANDOFF_185_20260917.md` for the captured cause and limits.

The following178/179 notes are historical qualification limits.178 fixes the observed camera-cancelled Wi-Fi association delay and enables narrowly proved, charged recovery of closed OTA discovery. Its rapid camera/voice checks and two saved image/voice deliveries passed within recorded limits; cloud custody was independently confirmed and retry wakes stayed dark. 179 retains early manual intent and waits for transient list/HTTP ownership inside the original deadline. Two real 179 manual requests fetched both older public 162 manifests and completed checked-pair bookkeeping. Cross-board logs support cleanup; strict test controllers stopped on truncated USB diagnostic lines, and their failures remain recorded. The first check's paired sleep is directly captured. A final ordinary wake confirmed normal unlocked Home, persisted the next 02:00 Pacific LCD arm, and returned both boards to sleep.

At that earlier178/179 checkpoint, public manifests remained162. The older-manifest `downgrade_blocked` result still maps to generic failure wording; this does not qualify equal-version UpToDate UI or an OTA transfer. Early-startup preservation is host-tested; no dedicated physical early-window assertion passed. Preserve provisioning, memory-reserve, USB-connected and full-product qualification limits. The old179 timed installation harness can tap an awake foreground screen and must not be reused.180 used a separate Home identity gate and one initial actuator stroke only when LCD USB was absent. Retain exact private artifacts and receipts; no public release is claimed.

## Release Baseline

- Start future OTA work from `RELEASE_BASELINE.json.current_working_source`, retaining162 camera input recovery,159 wake handling and the158 fixes. `current_baseline` and tag `halo-v6.4.158` identify the immutable recovery/previous scheduled qualification. Read `docs/FROZEN_RELEASE_158.md`, `docs/LCD_USER_WAKE_159.md` and `docs/BUILD_AND_RELEASE.md`. Historical117 qualification does not select the current source.
- Run `python3 -B tools/verify_frozen_baseline.py` before preparing new firmware. Preserve the immutable 158 tag and binaries; new changes need an unused version at least 6.4.186 after a fresh inventory check. The guard verifies committed ancestry and retained bytes, not uncommitted edits or device acceptance.
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

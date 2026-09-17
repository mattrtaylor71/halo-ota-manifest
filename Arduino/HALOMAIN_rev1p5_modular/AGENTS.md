> September 17 publication update: **6.4.184 is now public for both boards**, built from clean `fd3fd3947242` with unchanged 183 runtime. Full public manifest/binary readback passed. User manual OTA acceptance is pending; publication did not reset the unit’s spent allowance. Installed state was last verified as Sense183/LCD180. Continue from the same reviewed runtime and use an unused version **185 or later** after fresh inventory. See `RELEASE_BASELINE.json.published_manual_test_release_184`. Earlier public162 statements below describe the preceding checkpoint.

# HALO Firmware Agent Guide

Current code starting point is `ad9eae59596131f501520c06cad7f3b4d98e9bfb` (183), firmware tree `4a3d346feb41cd80d75ab9ea27b0f03b63b79617`. It gates camera allocation on complete upload cleanup, retaining the 181 upload-priority changes and 182 clock-boundary correction, and adds the bounded host actuator-relay recovery tool. All 25 exact-snapshot host suites pass. Both183 canonical builds/artifact checks pass and the pair is sealed (`ca4060d1627c`); controlled Sense-only installation and fresh paired SDK health passed. The183 camera-overlap and corrected visible-list tests passed; all three cloud metadata checks and independent device review are complete. The approved external adapters used unchanged canonical `run()`: a 2.5 GiB reserve correctly stopped before LCD, then the separately approved LCD-only 2.0 GiB build passed. Preserve both exception receipts.

The installed unit is **Sense183 app0 / LCD180 app0**. Controlled Sense-only service verified protected ranges/backups and closed cleanly, preserving Sense182 app1 and LCD179 app1 fallback banks, with no NVS, bootloader, current-bank or LCD writes. The released Sense183 boot reached SDK VALID with current-state readback at10,077ms. The183/LCD180 pair is SDK VALID. In the183 camera-overlap test, voice22 parked after actual POST interruption, full transport cleanup drained before camera allocation, and camera48 captured127,193bytes in1,825ms; both fresh jobs were accepted before paired sleep plus15seconds quiet. Inputs used the real console handlers, not physical menu navigation. Both prior Sense182/LCD180 were observed SDK VALID before service. After relay recovery, the 182 first-wake image test passed, including actual secondary-clock success, exact cloud delivery and paired sleep. Voice interruption by a live list refresh passed. Camera interruption failed because HTTP unlocked before the full upload lease/DMA cleanup; the original voice was saved to SD and subsequently accepted during a dark automatic retry. That recovery does not pass the failed camera capture. Earlier actuator-open failures remain recorded. The corrected183 visible-list case woke the LCD through an injected encoder event, delivered the live list in0.879s and verified a lit responsive list at0.960s on serial timestamps (controller observation0.952s/1.061s); the interrupted voice resumed before paired sleep. Exact cloud custody and completed processing are confirmed for all three voice jobs and the camera image. These are injected handler tests, not physical touch-geometry or broad product qualification. No public release is claimed. Read `docs/UPLOAD_LATENCY_181_20260917.md` and `RELEASE_BASELINE.json.current_working_source`; continue from reviewed 183 descendants. The 180 cosmetic release and prior runtime evidence remain historical records.

178 fixes the observed camera-cancelled Wi-Fi association delay and enables narrowly proved, charged recovery of closed OTA discovery. Its rapid camera/voice checks and two saved image/voice deliveries passed within recorded limits; cloud custody was independently confirmed and retry wakes stayed dark. 179 retains early manual intent and waits for transient list/HTTP ownership inside the original deadline. Two real 179 manual requests fetched both older public 162 manifests and completed checked-pair bookkeeping. Cross-board logs support cleanup; strict test controllers stopped on truncated USB diagnostic lines, and their failures remain recorded. The first check's paired sleep is directly captured. A final ordinary wake confirmed normal unlocked Home, persisted the next 02:00 Pacific LCD arm, and returned both boards to sleep.

Public manifests remain 162. The older-manifest `downgrade_blocked` result still maps to generic failure wording; this does not qualify equal-version UpToDate UI or an OTA transfer. Early-startup preservation is host-tested; no dedicated physical early-window assertion passed. Preserve provisioning, memory-reserve, USB-connected and full-product qualification limits. The old179 timed installation harness can tap an awake foreground screen and must not be reused.180 used a separate Home identity gate and one initial actuator stroke only when LCD USB was absent. Retain exact private artifacts and receipts; no public release is claimed.

## Release Baseline

- Start future OTA work from `RELEASE_BASELINE.json.current_working_source`, retaining162 camera input recovery,159 wake handling and the158 fixes. `current_baseline` and tag `halo-v6.4.158` identify the immutable recovery/previous scheduled qualification. Read `docs/FROZEN_RELEASE_158.md`, `docs/LCD_USER_WAKE_159.md` and `docs/BUILD_AND_RELEASE.md`. Historical117 qualification does not select the current source.
- Run `python3 -B tools/verify_frozen_baseline.py` before preparing new firmware. Preserve the immutable 158 tag and binaries; new changes need an unused version at least 6.4.184 after a fresh inventory check. The guard verifies committed ancestry and retained bytes, not uncommitted edits or device acceptance.
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

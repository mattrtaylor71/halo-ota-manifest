> September 18 installed checkpoint: **LCD191/app1 and unchanged Sense190/app1 are SDK VALID.** The LCD-only service preserved settings and the old LCD188/app0 bank. Fresh identity, list/scroll, Home and paired sleep with 20 seconds quiet passed; the smoke check made zero deletions. Public OTA remains 188. 191 delete latency/rollback and paired191 OTA qualification remain untested.

# HALO Firmware Agent Guide

Current code starting point is `6f3bc970385d6905f1f9723c8d89cb7669c4c148`, firmware tree `cbcec189d1b3d5652020746542ff35db313ba7a5`, build `6.4.191-20260918T192530Z-6f3bc970385d`. It adds immediate view-only delete feedback with rollback; backend confirmation still controls cache removal. See `docs/SHOPPING_DELETE_RESPONSE.md`. The full190 source/acceptance record is retained as `previous_working_source_190`; preserve its voice/list transport and LCD `INPUT_USER_ACTIVE` fixes.

The finite190 device checks passed: voice/list browsing with8refreshes and28scroll events, then camera interruption of an actual voice POST, exact-job resume to202, successful camera PUT200 and paired sleep. Do not rerun the historical189-to190 installer. Spoken-item semantics, physical gesture geometry and another same-wake camera after the recorded DMA-reserve warning remain unqualified. The same warning exists in188 evidence; it is not newly proved to be a190 regression. Public188, production02:00Pacific configuration, quota and debt are unchanged.

A later physical voice request, "Add gummy worms to my shopping list," was matched to backend insertion, LCD list display and the user's deletion. Its automatic saved retry received an accepted response with one cloud worker execution and dark LCD logs. See `/Users/MattTaylor/halo-voice-list189-20260917/MANUAL-USER190-FIRST-VOICE-REVIEW.json`; this is one manual case, not three repeated cases.

The following178/179 notes are historical qualification limits.178 fixes the observed camera-cancelled Wi-Fi association delay and enables narrowly proved, charged recovery of closed OTA discovery. Its rapid camera/voice checks and two saved image/voice deliveries passed within recorded limits; cloud custody was independently confirmed and retry wakes stayed dark. 179 retains early manual intent and waits for transient list/HTTP ownership inside the original deadline. Two real 179 manual requests fetched both older public 162 manifests and completed checked-pair bookkeeping. Cross-board logs support cleanup; strict test controllers stopped on truncated USB diagnostic lines, and their failures remain recorded. The first check's paired sleep is directly captured. A final ordinary wake confirmed normal unlocked Home, persisted the next 02:00 Pacific LCD arm, and returned both boards to sleep.

At that earlier178/179 checkpoint, public manifests remained162. The older-manifest `downgrade_blocked` result still maps to generic failure wording; this does not qualify equal-version UpToDate UI or an OTA transfer. Early-startup preservation is host-tested; no dedicated physical early-window assertion passed. Preserve provisioning, memory-reserve, USB-connected and full-product qualification limits. The old179 timed installation harness can tap an awake foreground screen and must not be reused.180 used a separate Home identity gate and one initial actuator stroke only when LCD USB was absent. Retain exact private artifacts and receipts; no public release is claimed.

## Release Baseline

- Start future OTA work from `RELEASE_BASELINE.json.current_working_source`, retaining162 camera input recovery,159 wake handling and the158 fixes. `current_baseline` and tag `halo-v6.4.158` identify the immutable recovery/previous scheduled qualification. Read `docs/FROZEN_RELEASE_158.md`, `docs/LCD_USER_WAKE_159.md` and `docs/BUILD_AND_RELEASE.md`. Historical117 qualification does not select the current source.
- Run `python3 -B tools/verify_frozen_baseline.py` before preparing new firmware. Preserve the immutable 158 tag and binaries; new changes need an unused version at least 6.4.192 after a fresh inventory check. The guard verifies committed ancestry and retained bytes, not uncommitted edits or device acceptance.
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

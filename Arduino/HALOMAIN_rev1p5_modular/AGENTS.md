> **Current private development checkpoint, September 28: 227 is installed on the bench, not published.** Continue from compiled source `4602b6b105f7dca43ea4a7de2f2a2823029da8f5` or reviewed descendants on `codex/ram-qualification`. This retains 225 RAM and 226 refresh/haptics fixes and separates background maintenance holds from visible inactivity. Both 125-suite gates and canonical paired builds passed; LCD static DRAM increases 8 bytes. Both boards are 227/app0 SDK VALID with 226/app1 and data preserved. Empty mounted stores, normal physical wake, paired sleep and 75s quiet passed. Passive device logs measured panel-off at 10.011s despite five natural keepalives. The extra dark-held touch was refused before movement because Sense slept during actuator startup; retain case34 as incomplete, not a full pass. Public OTA remains 224; different firmware bytes require freshly inventoried unused 228+. Do not infer prior media acknowledgement from current empty queues. [Current private handoff](docs/BACKGROUND_MAINTENANCE_DISPLAY_20260928.md). The [functional follow-up](docs/FUNCTIONAL_227_20260928.md) is closed with findings: four camera records reached cloud; two voice/list first attempts timed out then recovered without duplicate actions; visible refresh joins, viewport movement, exact test-item deletion, haptic standby and final healthy empty-state sleep/75s quiet were observed. Preserve failed strict observers and physical/OTA-transfer gaps; this is not blanket release approval.

> **Historical 226 private checkpoint, September 28:** Compiled source `1ce0d04e95b1b2a039b27364a616a535642cd10b` restored 216/217 refresh ownership/feedback and haptics-off while retaining 225 Sense RAM changes. Both 124-suite gates and canonical paired builds passed. Both boards were verified 226/app1 SDK VALID, with protected 225/app0 and device data. Three native Home/List rebuild cycles and haptic standby readbacks passed. Two refresh joins during an actual voice POST showed the indicator; that attempt timed out, and a separate passive observation captured native retry acknowledgement, saved-file deletion, cleared retry state and dark LCD recovery. A later manual capture had another voice timeout whose cloud completion is verified, but later native cleanup remains unknown. Retain each case's distinct limits. A retained 225 LCD panic was preserved and decoded before installation. [226 handoff](docs/LCD_REFRESH_HAPTICS_226.md).

> **Current published firmware: 6.4.224 (September 28).** Continue from source `518691c8d3bd609272b438ec31b82c4de1b6b053` or reviewed descendants on `codex/halo-retry-release` in `/Users/MattTaylor/halo-retry-release-20260928`; tag `halo-v6.4.224`. Both 119-suite gates, paired canonical artifacts and complete public readbacks passed. One installed 224/app1 SDK VALID pair has composed limited acceptance: exact OTA transfers, real saved-media delivery/delete and fresh inventory 0, durable retry/peer-arm 0, normal timer evidence and a later 75.219 s passive USB-absence interval. Original OTA/media/health observer failures remain recorded; no continuous first Sense 224 boot, new scheduled 02:00 execution or full-product pass is claimed. Only the three retry fixes and approved 212 reminder are added to 211 behavior; private 213–222 and battery WIP remain separate. Future versions require freshly inventoried unused 225+. Factory 197/frozen 158 remain unchanged. [Current release and limits](docs/RELEASE_224.md). Older notes below are historical.

> **Historical211 publication checkpoint:6.4.211; device testing pending at that checkpoint.** Discard and dish now use their requested Got it subtitles; 210 automatic check-in is retained. Paired canonical artifacts, all 114 exact-snapshot host suites and complete public downloads passed. The last verified installed pair remains 209 (Sense app1/LCD app0 SDK VALID). Source `731e4897c845e6bb891f277f0e82e210c380150c`, tag `halo-v6.4.211`; future versions require unused 212+ after inventory. Camera, network, OTA and storage behavior are unchanged; prior network/UI lease limits remain open. Factory 197/frozen 158 stay separate. [Release handoff](docs/RELEASE_211.md).

> **Historical 212 hold (September 22); superseded by published 224.** Both builds and all 115 snapshot suites passed; immutable files were staged while production latest remained paired 211. Do not promote 212 or schedule its publication without a new explicit user command. Keep the exact candidate bytes; version 212 is occupied and must not be reused for other changes. [Candidate handoff](docs/RELEASE_CANDIDATE_212.md).

> **Historical 210 publication checkpoint: 6.4.210; device testing pending.** Check-in now skips the quantity page and proceeds to the existing Got it screen after queue acceptance. Paired canonical artifacts, all 114 exact-snapshot host suites and complete public downloads passed. The last verified installed pair remains 209 (Sense app1/LCD app0 SDK VALID). Source `6eb91c7bed8bc89eed83ca785108b1f0a53deac3`, tag `halo-v6.4.210`; future versions require unused 211+ after inventory. Camera, network, OTA and storage behavior are unchanged; prior network/UI lease limits remain open. Factory 197/frozen 158 stay separate. [Release handoff](docs/RELEASE_210.md).

> **Historical 209 checkpoint: 6.4.209 (September 22).** The user’s second manual attempt updated both boards from 208: Sense app1 and LCD app0, exact image hashes and SDK VALID, native observed-pair settlement, fresh Home and paired sleep. The earlier weak-signal attempt failed and exposed a preflight UI lease expiry issue that remains open. The September 23 02:00 PDT schedule is stored and its sleep timer armed; scheduled execution is not yet proved. Version 209 changes only metadata over 208. Source `cd2b84bc072a8f287c4f93bed99df091e04610ba`, tag `halo-v6.4.209`; future versions need unused 210+ after inventory. Factory 197/frozen 158 unchanged. [Release handoff](docs/RELEASE_209.md). Earlier dated notes below are historical.

> **Historical207 functional follow-up: OTA blocker reproduced.** Camera/check-in/dish/discard uploads, list refreshes and normal sleep passed on Sense207/LCD206. Both a manual request and the genuine 02:00 timer wake refused OTA before download because retained closed DISCOVERY/LCD-due state did not acquire required calendar ownership. Voice reached cloud but the acoustic fixture had an empty transcript; list addition/deletion is unqualified. Final wake recheck was blocked by actuator USB timeout before a stroke. Firmware, schedule and debt were not changed. See [test report](docs/FUNCTIONAL_207_20260922.md). Earlier release-checkpoint wording below is historical.

> **Historical207 release checkpoint (September22).** Continue from source `fcf6fb220f1f4983557669e8e3b552f910e079c1` or reviewed descendants. [Release handoff](docs/RELEASE_207.md). Both canonical builds, all 113 exact-snapshot suites and complete public binary readbacks passed. The bench remains Sense207/app1 and LCD206/app0, both SDK VALID; one phone provisioning and immediate Check-in passed on first attempts, with cloud image verification and paired sleep. Paired 207 OTA and broader reliability checks remain untested. New versions must be unused 208+ after fresh inventory. EOL 197 and frozen 158 recovery remain unchanged. Earlier dated entries below are historical checkpoints.

> Historical factory startup correction (September18): candidate197 fixes the erased-NVS/setup UART deadlock while retaining quarantine until fresh healthy/idle proof. Both99-suite gates and paired canonical builds passed; the EOL pair received full factory images/readbacks, then passed automatic startup and fresh SDKVALID communication after reviewed reset-only harness recovery. Physical power-cycle/Settings/sleep-wake remain pending; public196 is unchanged. Continue from source `7f87340d7c271bfdfc9b888aa6a0fcbf43822a8e` or reviewed descendants. See [factory evidence](docs/FACTORY_UART_STARTUP_197.md).

> Historical September 18 release: **196 is published and read back for Sense and LCD.** Both canonical builds/artifact checks and all 97 exact-snapshot suites passed. The user’s paired manual OTA passed: Sense 195/app0 → 196/app1 and LCD 191/app1 → 196/app0, both SDK VALID, with verified image hashes, observed target resolution, Home and paired sleep. Capture closed after 60.438 seconds without USB reopening. A further no-update request is untested; no new scheduled-OTA or full-product pass is claimed. See [release evidence and remaining steps](docs/MANUAL_OTA_196_20260918.md).

# HALO Firmware Agent Guide

> **Historical225 RAM qualification, September 28:** `codex/ram-qualification` contains
> built candidate **6.4.225**, exact build source
> `245e8b4cd64ec1633e0a85fbb3a9487f4f80d71e`. The Mini bench pair is now
> 225 / app0 / SDK VALID, with 224 retained in app1. Public production is still
> 224. Version 225 is occupied; different firmware bytes require a freshly
> inventoried unused 226+ version. Read [RAM qualification](docs/RAM_QUALIFICATION_20260928.md)
> for acceptance limits. Earlier production/bench checkpoints below are historical
> where they differ. No 225 publication follows from testing it.

Published224 metadata (separate from private development): source `518691c8d3bd609272b438ec31b82c4de1b6b053`, firmware tree `ae7726fdad7edea9d51ed4191da845deaef31455`, build `6.4.224-20260928T164640Z-518691c8d3bd`, tag `halo-v6.4.224`, release branch `codex/halo-retry-release`. At publication, the bench pair was224/app1 SDKVALID with the limited acceptance in `docs/RELEASE_224.md`. The current private development branch is `codex/ram-qualification`; use the private checkpoint at the top of this file for new private work. Do not restart from published224 and silently drop the retained RAM, refresh or haptics fixes. Version227 is occupied; different firmware bytes require a freshly inventoried unused228+ version. Factory197/frozen158 remain separate.

## Historical device notes

The finite190 device checks passed: voice/list browsing with8refreshes and28scroll events, then camera interruption of an actual voice POST, exact-job resume to202, successful camera PUT200 and paired sleep. Do not rerun the historical189-to190 installer. Spoken-item semantics, physical gesture geometry and another same-wake camera after the recorded DMA-reserve warning remain unqualified. The same warning exists in188 evidence; it is not newly proved to be a190 regression. Public188, production02:00Pacific configuration, quota and debt are unchanged.

A later physical voice request, "Add gummy worms to my shopping list," was matched to backend insertion, LCD list display and the user's deletion. Its automatic saved retry received an accepted response with one cloud worker execution and dark LCD logs. See `/Users/MattTaylor/halo-voice-list189-20260917/MANUAL-USER190-FIRST-VOICE-REVIEW.json`; this is one manual case, not three repeated cases.

The following178/179 notes are historical qualification limits.178 fixes the observed camera-cancelled Wi-Fi association delay and enables narrowly proved, charged recovery of closed OTA discovery. Its rapid camera/voice checks and two saved image/voice deliveries passed within recorded limits; cloud custody was independently confirmed and retry wakes stayed dark. 179 retains early manual intent and waits for transient list/HTTP ownership inside the original deadline. Two real 179 manual requests fetched both older public 162 manifests and completed checked-pair bookkeeping. Cross-board logs support cleanup; strict test controllers stopped on truncated USB diagnostic lines, and their failures remain recorded. The first check's paired sleep is directly captured. A final ordinary wake confirmed normal unlocked Home, persisted the next 02:00 Pacific LCD arm, and returned both boards to sleep.

At that earlier178/179 checkpoint, public manifests remained162. The older-manifest `downgrade_blocked` result still maps to generic failure wording; this does not qualify equal-version UpToDate UI or an OTA transfer. Early-startup preservation is host-tested; no dedicated physical early-window assertion passed. Preserve provisioning, memory-reserve, USB-connected and full-product qualification limits. The old179 timed installation harness can tap an awake foreground screen and must not be reused.180 used a separate Home identity gate and one initial actuator stroke only when LCD USB was absent. Retain exact private artifacts and receipts; no public release is claimed.

## Release Baseline

- For an explicitly approved production release, follow [Nightly OTA publication](docs/NIGHTLY_OTA_RELEASE.md). Promotion makes the pair available for each eligible 02:00 check; it does not change timers, force an immediate fleet update, or prove installation. Fresh provisioning/manual checks may update earlier.
- **Default to local development.** Read [Development and release](docs/DEVELOPMENT_AND_RELEASE.md). Build, flash, test, commit, and push-to-device requests do not authorize public OTA. Keep the current production latest unchanged until the user explicitly approves that candidate for production; historical publication authorization does not carry forward.
- Prepare local candidates with `tools/firmware_candidate.py`; record bench evidence separately. Only after explicit release approval use the current paired publisher with `--approve-production-version` for each production remote phase. Do not use historical publisher copies, the retired single-board CLIs, or direct cloud writes to bypass this boundary.
- Continue private firmware work from the current private checkpoint above or reviewed descendants. `PRODUCTION_BASELINE.json` and `RELEASE_BASELINE.json.current_working_source` verify the public release identity; they do not supersede approved private RAM, refresh or haptics fixes. `current_baseline` and tag `halo-v6.4.158` identify immutable recovery and previous scheduled qualification. Read `docs/RELEASE_224.md` and `docs/BUILD_AND_RELEASE.md`; historical qualification does not select the current development source.
- Run `python3 -B tools/verify_frozen_baseline.py` before preparing new firmware. Preserve the immutable 158 tag and binaries; new changes need an unused version at least6.4.225 after a fresh inventory check. The guard verifies committed ancestry and retained bytes, not uncommitted edits or device acceptance.
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

- Review resources for every firmware change using [Resource review](docs/RESOURCE_REVIEW.md). Record maximum allocation/capabilities, lifetime/owner, overlap, failure cleanup and foreground responsiveness. Copy-only changes may record unchanged ownership and build-size delta; resource-affecting changes require before/after artifact reports and relevant runtime evidence. Read the [both-board audit](docs/RESOURCE_AUDIT_20260922.md) for proposed opportunities and restrictions. Do not turn proposals into untested PSRAM/stack/DMA changes or alter held 212 artifacts.
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

The active September 28 campaign uses the commissioned **Mac mini** bench at
`/Volumes/Trepo-Work/Workspaces/halo-firmware-bench`, its exact-device identity
configuration and common hardware lease. Its USB relay resets only the Uno's
5 V supply; it does not switch Halo or the 12 V motor supply. Use the qualified
Mini control/runbooks in `/Users/MattTaylor/halo-mac-mini-bench-20260925`, not the
historical MacBook-only wiring description below. Preserve the fixed calibrated
stroke and one hardware operator. Evidence is in
`/Users/MattTaylor/halo-ram-campaign-20260928/bench-private/`.

Historical MacBook September 18 arrangement:

Use the bounded exact-device `tools/tapctl.py` for the current replacement Uno; preserve `PUSH:500,200,500` at speed128. `--action probe` is nonmoving; `--action wake` requires a fresh Sense USB transition. Inspect receipts and stop on uncertain worker ownership. The relay is absent from the current wiring; the old relay helper pins a different Uno. Three physical cycles plus a final idle/reopen passed after a full USB/12V reset, but the original intermittent fault is not proved permanently cured. See [current evidence](docs/ACTUATOR_INVESTIGATION_20260918.md).

# Current release 207

Paired 207 is published and fully read back. Continue from source `fcf6fb220f1f4983557669e8e3b552f910e079c1` or reviewed descendants, selected by [PRODUCTION_BASELINE.json](../PRODUCTION_BASELINE.json) and `RELEASE_BASELINE.json.current_working_source`. Use an unused 208+ version after fresh inventory. [Release identity, evidence and limits](RELEASE_207.md).

The bench remains Sense207/app1 and LCD206/app0, both SDK VALID. One phone provisioning and immediate Check-in passed on first attempts, with cloud image verification and paired sleep. Exact paired 207 installation and broader acceptance remain untested. EOL 197 and frozen 158 recovery are unchanged. The dated sections below preserve historical checkpoints and do not select new development.

## Historical 196 checkpoint

> September 18 release: **196 is published and read back for Sense and LCD.** Both canonical builds/artifact checks and all 97 exact-snapshot suites passed. The user’s paired manual OTA passed: Sense 195/app0 → 196/app1 and LCD 191/app1 → 196/app0, both SDK VALID, with verified image hashes, observed target resolution, Home and paired sleep. Capture closed after 60.438 seconds without USB reopening. A further no-update request is untested; no new scheduled-OTA or full-product pass is claimed. See [release evidence and remaining steps](MANUAL_OTA_196_20260918.md).

Current source is `26c5d713475dc0390c375a9b1e77d2e2ffaaa82a`, firmware tree `672353630dab3650e7f233a331eb4b989d8772b0`, build `6.4.196-20260918T213712Z-26c5d713475d`. Continue from this release or reviewed descendants. It retains 190 voice/list and 191 immediate-delete behavior, explicit manual grants and the completed-calendar handoff, and adds the bounded clock retry for a manual request joined to recovery. Use an unused 197+ after fresh inventory. The following 191/190 paragraphs preserve historical acceptance and limits. The working checkout also contains unpublished UI-copy change `4a52ac3` (“Keep Halo powered on”); that wording is not in published or installed196.

`RELEASE_BASELINE.json.current_working_source` now selects 196; `previous_working_source_194` preserves the prior publication and `previous_working_source_191` retains the LCD-only installed checkpoint. [Publication, bootstrap and completed manual OTA](MANUAL_OTA_196_20260918.md).

## Previous191 installed checkpoint

> September 18 installed checkpoint: **LCD191/app1 and unchanged Sense190/app1 are SDK VALID.** The LCD-only service preserved settings and the old LCD188/app0 bank. Fresh identity, list/scroll, Home and paired sleep with 20 seconds quiet passed; the smoke check made zero deletions. Public OTA remains 188. 191 delete latency/rollback and paired191 OTA qualification remain untested.

Current source is191: continue from `6f3bc970385d6905f1f9723c8d89cb7669c4c148` or reviewed descendants. The exact materialization uses firmware tree `cbcec189d1b3d5652020746542ff35db313ba7a5` and build `6.4.191-20260918T192530Z-6f3bc970385d`. [Delete response validation](SHOPPING_DELETE_RESPONSE.md) records the 97-suite host pass, LCD-only canonical artifact checks and closed USB installation/list/Home/sleep smoke check. Delete latency and rollback are still untested. Sense was not rebuilt for191. Use an unused192+ for subsequent firmware changes after fresh inventory.

`RELEASE_BASELINE.json.current_working_source` selects191; `previous_working_source_190` preserves the original [scoped190 acceptance](VOICE_LIST_UPLOAD_VALIDATION_190.md), including its manual and cloud receipts. The installed configuration is Sense190/app1 plus LCD191/app1 SDK VALID; LCD188/app0 is retained and public188 is unchanged. Speech/list mutation, physical gesture geometry and another same-wake camera after the DMA-reserve warning remain unqualified. `current_baseline` retains frozen158 recovery. The checkpoints below are historical, not current source selection.

## Historical release checkpoints

> September17 user test: **both boards completed the physical185→186 manual OTA and run app0 / SDK VALID**. Exact hashes passed; native policy resolved, Home returned and both boards slept. **OTA presentation failed:** the user saw a frozen Checking for updates screen during LCD transfer because the flash-safety guard suppresses rendering. This remains unfixed; do not call186 full UI acceptance. The final02:00 Pacific schedule is verified unchanged. See `docs/MANUAL_OTA_186_20260917.md` and the current release record.

> September 17 release update: **6.4.186 is public; the last verified installed pair remains185 app1 SDK VALID.**186 is a version-only bump from `62c6864b4c15`, build `6.4.186-20260917T224600Z-62c6864b4c15`. All 1,138 non-generated runtime files match sealed 185; both canonical builds/artifact checks and seven exact-snapshot suites passed. Public paired readback passed. The user will perform the physical manual OTA;186 installation and acceptance are pending. One external test allowance is granted and verified; the schedule is unchanged. Continue from186 or reviewed descendants; use unused **187 or later** after fresh inventory. Frozen158 remains unchanged.

**Continue from186 source `62c6864b4c15` or reviewed descendants**, selected by `RELEASE_BASELINE.json.current_working_source`. The complete185 record is preserved as `previous_working_source_185`;183 and158 history remain unchanged. See [186 publication and pending user test](MANUAL_OTA_186_20260917.md). The prior [185 manual LCD OTA](MANUAL_OTA_HANDOFF_185_20260917.md) is separate acceptance, not evidence of186 installation.

The following177 campaign remains historical evidence. The [September 17 functional campaign](FUNCTIONAL_TEST_177_20260917.md) adds 60 distinct passing host suites, four independently verified saved-upload recoveries, dark retry and physical user-priority evidence. It also records two open findings: a 25-second reconnect delay and inherited OTA coordinator state blocking the manual check. Installed 177 and public 162 are unchanged; this is not new release acceptance.

For historical 158 recovery, [the frozen release handoff](FROZEN_RELEASE_158.md) binds exact hashes, private archive, recovery limits, schedule, acceptance scope, and retention requirements. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) → `current_baseline` is the historical recovery record.

- [Build and publish a future version](BUILD_AND_RELEASE.md)
- [Actual OTA evidence and repeatable test procedure](OTA_158_VALIDATION.md)
- [Acceptance plan and historical qualification](PRODUCTION_RELEASE_ACCEPTANCE.md)
- [Archived release checkpoints](RELEASE_BASELINE_HISTORY.md)

The scheduled 157→158 confirmation passed, both boards reached SDK VALID, native policy resolved, and daily 02:00 Pacific was restored. This supports a small monitored rollout; USB-free, power-interruption, factory-station, and complete product regression on 158 remain outside that acceptance. Historical 117 qualification is retained under `historical_product_qualification_117`; it does not select the current source.

Before preparing new source, run `python3 -B tools/verify_frozen_baseline.py` from the firmware directory. Select new development through `PRODUCTION_BASELINE.json` and allocate an unused version at least 6.4.208 after fresh inventory. Neither documentation nor source preservation is a new firmware build, OTA publication, hardware test, or remote Git push.

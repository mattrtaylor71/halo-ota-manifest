> September 18 acceptance: **Sense190/app1 SDK VALID passed voice/list transport and urgent-camera priority with preserved voice custody and paired sleep.** LCD188/app0 SDK VALID is unchanged; public OTA remains188 and190 is not published. See `docs/VOICE_LIST_UPLOAD_VALIDATION_190.md` for the closed receipt and limits.

Current source is190; continue from ffe52bf27865645ec0ff2a000b51049b40bcd5b1 or reviewed descendants. Use unused191+ for new firmware changes after inventory. RELEASE_BASELINE.json.current_working_source selects this code; current_baseline retains frozen158 recovery. The closed scoped acceptance is [recorded here](VOICE_LIST_UPLOAD_VALIDATION_190.md). Speech/list mutation, physical gesture geometry and another same-wake camera after the DMA-reserve warning remain unqualified. The checkpoints below are historical, not current source selection.

## Historical release checkpoints

> September17 user test: **both boards completed the physical185→186 manual OTA and run app0 / SDK VALID**. Exact hashes passed; native policy resolved, Home returned and both boards slept. **OTA presentation failed:** the user saw a frozen Checking for updates screen during LCD transfer because the flash-safety guard suppresses rendering. This remains unfixed; do not call186 full UI acceptance. The final02:00 Pacific schedule is verified unchanged. See `docs/MANUAL_OTA_186_20260917.md` and the current release record.

> September 17 release update: **6.4.186 is public; the last verified installed pair remains185 app1 SDK VALID.**186 is a version-only bump from `62c6864b4c15`, build `6.4.186-20260917T224600Z-62c6864b4c15`. All 1,138 non-generated runtime files match sealed 185; both canonical builds/artifact checks and seven exact-snapshot suites passed. Public paired readback passed. The user will perform the physical manual OTA;186 installation and acceptance are pending. One external test allowance is granted and verified; the schedule is unchanged. Continue from186 or reviewed descendants; use unused **187 or later** after fresh inventory. Frozen158 remains unchanged.

**Continue from186 source `62c6864b4c15` or reviewed descendants**, selected by `RELEASE_BASELINE.json.current_working_source`. The complete185 record is preserved as `previous_working_source_185`;183 and158 history remain unchanged. See [186 publication and pending user test](MANUAL_OTA_186_20260917.md). The prior [185 manual LCD OTA](MANUAL_OTA_HANDOFF_185_20260917.md) is separate acceptance, not evidence of186 installation.

The following177 campaign remains historical evidence. The [September 17 functional campaign](FUNCTIONAL_TEST_177_20260917.md) adds 60 distinct passing host suites, four independently verified saved-upload recoveries, dark retry and physical user-priority evidence. It also records two open findings: a 25-second reconnect delay and inherited OTA coordinator state blocking the manual check. Installed 177 and public 162 are unchanged; this is not new release acceptance.

Start with [the frozen release handoff](FROZEN_RELEASE_158.md). It binds exact hashes, private archive, recovery limits, schedule, acceptance scope, and retention requirements. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) → `current_baseline` is the machine-readable authority.

- [Build and publish a future version](BUILD_AND_RELEASE.md)
- [Actual OTA evidence and repeatable test procedure](OTA_158_VALIDATION.md)
- [Acceptance plan and historical qualification](PRODUCTION_RELEASE_ACCEPTANCE.md)
- [Archived release checkpoints](RELEASE_BASELINE_HISTORY.md)

The scheduled 157→158 confirmation passed, both boards reached SDK VALID, native policy resolved, and daily 02:00 Pacific was restored. This supports a small monitored rollout; USB-free, power-interruption, factory-station, and complete product regression on 158 remain outside that acceptance. Historical 117 qualification is retained under `historical_product_qualification_117`; it does not select the current source.

Before preparing new source, run `python3 -B tools/verify_frozen_baseline.py` from the firmware directory. Allocate an unused version at least 6.4.187 for new changes. Neither documentation nor source preservation is a new firmware build, OTA publication, hardware test, or remote Git push.

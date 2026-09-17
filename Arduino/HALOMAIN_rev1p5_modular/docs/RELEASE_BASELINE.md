# Current source:6.4.185; installed Sense185/LCD185 app1 SDK VALID; frozen recovery:6.4.158

> September 17 release update: **6.4.185 is public and both boards now run185 app1 SDK VALID**, build `6.4.185-20260917T213753Z-5ce4b9615c8f`. Both canonical builds/artifact checks and seven exact-snapshot suites passed. Sense185 was installed by controlled USB service; `manual003` then completed the actual **LCD180→185 manual OTA on its first transfer attempt**. The exact LCD hash, reboot/SDK validation, completion bookkeeping, return to Home and paired sleep plus15seconds quiet were independently verified. This is not a Sense OTA or new scheduled-OTA qualification. Continue from185 or reviewed descendants and use an unused version **186 or later** after fresh inventory. Frozen158 remains unchanged.

**Continue from185 source `5ce4b9615c8f` or reviewed descendants**, selected by `RELEASE_BASELINE.json.current_working_source`. The complete183 record remains under `previous_working_source_183`. See [manual OTA handoff185](MANUAL_OTA_HANDOFF_185_20260917.md) for the captured183 failure and bounded correction. The completed scope is Sense USB installation followed by first-attempt LCD manual OTA, not a new Sense or scheduled paired transfer.

Cloud received at22:00:59UTC corroborates **phase8 RESOLVED**, generation14, network_windows2, apply_attempts1, LCDbegins1, Sensebegins0 and reserved_ms0; retry due/expiry are zero. The next normal02:00 Pacific wake remains unchanged (September18, epoch1789722000). Fresh SDK state and `lcd_ota_due=0`/`done_ids` verification come from direct device logs; the cloud receipt does not establish absence of hidden NVS keys. See [independent device review](/Users/MattTaylor/halo-manual-handoff185-20260917/manual003/INDEPENDENT-DEVICE-REVIEW.json) and [cloud ledger correlation](/Users/MattTaylor/halo-device-analytics-2026-09-10/manual184-20260917/cloud-manual185-final001/CORRELATION.json).

The following177 campaign remains historical evidence. The [September 17 functional campaign](FUNCTIONAL_TEST_177_20260917.md) adds 60 distinct passing host suites, four independently verified saved-upload recoveries, dark retry and physical user-priority evidence. It also records two open findings: a 25-second reconnect delay and inherited OTA coordinator state blocking the manual check. Installed 177 and public 162 are unchanged; this is not new release acceptance.

Start with [the frozen release handoff](FROZEN_RELEASE_158.md). It binds exact hashes, private archive, recovery limits, schedule, acceptance scope, and retention requirements. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) → `current_baseline` is the machine-readable authority.

- [Build and publish a future version](BUILD_AND_RELEASE.md)
- [Actual OTA evidence and repeatable test procedure](OTA_158_VALIDATION.md)
- [Acceptance plan and historical qualification](PRODUCTION_RELEASE_ACCEPTANCE.md)
- [Archived release checkpoints](RELEASE_BASELINE_HISTORY.md)

The scheduled 157→158 confirmation passed, both boards reached SDK VALID, native policy resolved, and daily 02:00 Pacific was restored. This supports a small monitored rollout; USB-free, power-interruption, factory-station, and complete product regression on 158 remain outside that acceptance. Historical 117 qualification is retained under `historical_product_qualification_117`; it does not select the current source.

Before preparing new source, run `python3 -B tools/verify_frozen_baseline.py` from the firmware directory. Allocate an unused version at least 6.4.186 for new changes. Neither documentation nor source preservation is a new firmware build, OTA publication, hardware test, or remote Git push.

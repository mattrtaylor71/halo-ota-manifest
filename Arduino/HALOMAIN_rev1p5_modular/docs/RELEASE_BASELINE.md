# Current working source: private 6.4.172; frozen recovery: 6.4.158

**Continue from private172 source `c90050f` or reviewed descendants, selected by `RELEASE_BASELINE.json.current_working_source`.** Its sealed pair and49 host suites pass; exact172 is installed with paired SDK VALID health. Phone association and functional acceptance remain pending. See [the172 handoff](PROVISIONING_JOIN_INVESTIGATION_20260916.md). Frozen158 remains the immutable recovery reference. The source tag is `halo-v6.4.158`, at `b6d06da5997e2252a3472697f30dc8111ab90367`. The current checkout includes later acceptance and handoff documentation. Preserve the tag and published binaries unchanged.

Start with [the frozen release handoff](FROZEN_RELEASE_158.md). It binds exact hashes, private archive, recovery limits, schedule, acceptance scope, and retention requirements. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) → `current_baseline` is the machine-readable authority.

- [Build and publish a future version](BUILD_AND_RELEASE.md)
- [Actual OTA evidence and repeatable test procedure](OTA_158_VALIDATION.md)
- [Acceptance plan and historical qualification](PRODUCTION_RELEASE_ACCEPTANCE.md)
- [Archived release checkpoints](RELEASE_BASELINE_HISTORY.md)

The scheduled 157→158 confirmation passed, both boards reached SDK VALID, native policy resolved, and daily 02:00 Pacific was restored. This supports a small monitored rollout; USB-free, power-interruption, factory-station, and complete product regression on 158 remain outside that acceptance. Historical 117 qualification is retained under `historical_product_qualification_117`; it does not select the current source.

Before preparing new source, run `python3 -B tools/verify_frozen_baseline.py` from the firmware directory. Allocate an unused version at least 6.4.173 for new changes. Neither documentation nor source preservation is a new firmware build, OTA publication, hardware test, or remote Git push.

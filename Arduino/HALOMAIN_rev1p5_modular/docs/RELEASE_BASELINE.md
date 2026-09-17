# Current working source: private6.4.174; frozen recovery:6.4.158

**Continue from private174 source `75debe8` or reviewed descendants, selected by `RELEASE_BASELINE.json.current_working_source`.** Both canonical builds, nine focused suites and paired SDK VALID health pass. Phone authentication still fails;197 SDK AES allocation errors were captured despite passing public HMAC/PBKDF vectors. A175 camera DMA reserve change is pending, with no confirmed fix or full functional acceptance. See [the current handoff](PROVISIONING_JOIN_INVESTIGATION_20260916.md). Frozen158 remains the immutable recovery reference: tag `halo-v6.4.158`, source `b6d06da5997e2252a3472697f30dc8111ab90367`. Preserve its tag and published bytes unchanged.

Start with [the frozen release handoff](FROZEN_RELEASE_158.md). It binds exact hashes, private archive, recovery limits, schedule, acceptance scope, and retention requirements. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) → `current_baseline` is the machine-readable authority.

- [Build and publish a future version](BUILD_AND_RELEASE.md)
- [Actual OTA evidence and repeatable test procedure](OTA_158_VALIDATION.md)
- [Acceptance plan and historical qualification](PRODUCTION_RELEASE_ACCEPTANCE.md)
- [Archived release checkpoints](RELEASE_BASELINE_HISTORY.md)

The scheduled 157→158 confirmation passed, both boards reached SDK VALID, native policy resolved, and daily 02:00 Pacific was restored. This supports a small monitored rollout; USB-free, power-interruption, factory-station, and complete product regression on 158 remain outside that acceptance. Historical 117 qualification is retained under `historical_product_qualification_117`; it does not select the current source.

Before preparing new source, run `python3 -B tools/verify_frozen_baseline.py` from the firmware directory. Allocate an unused version at least 6.4.175 for new changes. Neither documentation nor source preservation is a new firmware build, OTA publication, hardware test, or remote Git push.

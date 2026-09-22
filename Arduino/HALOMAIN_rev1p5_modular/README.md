# Halo production firmware

Public paired OTA is **6.4.207**. Start with [the release handoff](docs/RELEASE_207.md) and [PRODUCTION_BASELINE.json](PRODUCTION_BASELINE.json). New firmware must descend from `fcf6fb220f1f4983557669e8e3b552f910e079c1`, preserving 207 and all earlier fixes.

Use this canonical checkout and branch `codex/halo-production-baseline-197`, then follow [build and release](docs/BUILD_AND_RELEASE.md). New releases need an unused 208+ identity after fresh inventory, committed source, the complete snapshot regression gate, paired canonical builds and relevant device checks.

The bench remains Sense207/app1 and LCD206/app0, both SDK VALID. One phone provisioning and immediate Check-in passed on first attempts, with cloud image verification and paired sleep. Exact paired 207 installation, repeated reliability, voice and input interruption/resume remain untested.

EOL factory selection stays 197; frozen 158 recovery and earlier private packages remain unchanged. [RELEASE_BASELINE.json](RELEASE_BASELINE.json) preserves their evidence. Historical entries named “current” do not override the compact production selector.

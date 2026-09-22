# HALO production firmware

**6.4.207 is the current paired public release.** Start in [Arduino/HALOMAIN_rev1p5_modular](Arduino/HALOMAIN_rev1p5_modular), continuing from source `fcf6fb220f1f4983557669e8e3b552f910e079c1` or reviewed descendants. Use an unused 208+ version after fresh inventory.

- [Current release, evidence and limits](Arduino/HALOMAIN_rev1p5_modular/docs/RELEASE_207.md)
- [Authoritative production selector](Arduino/HALOMAIN_rev1p5_modular/PRODUCTION_BASELINE.json)
- [Build and publish the next version](Arduino/HALOMAIN_rev1p5_modular/docs/BUILD_AND_RELEASE.md)
- [Frozen 158 recovery and retention](Arduino/HALOMAIN_rev1p5_modular/docs/FROZEN_RELEASE_158.md)

The bench remains Sense207/app1 and LCD206/app0, both SDK VALID. One phone provisioning and immediate Check-in passed on first attempts, with cloud image verification and paired sleep. Exact paired 207 installation and broader reliability checks remain untested; publication does not expand that acceptance.

EOL factory selection remains 197. The immutable `halo-v6.4.158` tag and package retain their historical scheduled-OTA qualification. [RELEASE_BASELINE.json](Arduino/HALOMAIN_rev1p5_modular/RELEASE_BASELINE.json) preserves that history separately from current development. The `Arduino/HALOMAIN_rev1` folder is legacy reference material.

# Halo production firmware

Start with [the production baseline](docs/PRODUCTION_BASELINE_197.md) and [PRODUCTION_BASELINE.json](PRODUCTION_BASELINE.json). New firmware must preserve source 197, including the factory-startup fix and all preceding functionality. Public OTA currently remains 196; these are distinct release states.

Use this canonical checkout and branch `codex/halo-production-baseline-197`, then follow [build and release](docs/BUILD_AND_RELEASE.md). New releases need an unused 198+ identity, committed source, the complete snapshot regression gate, paired canonical builds and applicable device checks. Never reuse an old worktree or historical build script to bypass the source guard.

Exact private recovery packages are retained in `/Users/MattTaylor/halo-releases/6.4.197` and `/Users/MattTaylor/halo-releases/6.4.196`. Full evidence and limitations are linked from the handoff. [RELEASE_BASELINE.json](RELEASE_BASELINE.json) preserves history; old entries named “current” do not override the compact production selector.

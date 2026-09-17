# HALO workspace baseline

Current source includes unbuilt provisioning fixes in `e58a4fb` (profile emblem and stale STA guard). Read `docs/PROVISIONING_JOIN_INVESTIGATION_20260916.md`.171 artifacts are unchanged; both boards still169. The phone join failure is unresolved. Do not substitute171 artifacts for this newer source; allocate172+ when built.


- Current development candidate is private171, artifact source `aa221c2`. 45 host suites and both canonical production builds/artifact checks pass. **Not installed or published**; both boards remain169 SDK VALID and LCD is at real provisioning step3, app_connected=0. Read `docs/WIFI_MEDIA_CORNER_TESTING_171.md` and `RELEASE_BASELINE.json.current_working_source`. Three corner fixes cover Wi-Fi clock zero, accepted-delete recovery and SD replay yielding to input. Complete real setup before physical media/sleep/retry qualification; rebind171 installation tooling and preserve169 fallback banks. Frozen158/public162 remain separate references. Continue from171 or reviewed descendants; allocate unused172+.

- The authoritative production project is `Arduino/HALOMAIN_rev1p5_modular`; read its `AGENTS.md` and `docs/FROZEN_RELEASE_158.md` before changing firmware.
- Use `RELEASE_BASELINE.json.current_working_source` for new changes and `current_baseline` for the retained frozen158 recovery artifacts. Work from this checkout or reviewed descendants preserving158 OTA fixes,159 LCD wake handling and162 camera input recovery. Historical records and `Arduino/HALOMAIN_rev1` are not alternative current baselines.
- Run `python3 -B tools/verify_frozen_baseline.py` from the production firmware directory before preparing new firmware. This read-only check verifies ancestry and retained artifact identity, not the correctness of uncommitted changes.
- Preserve the immutable tag and published 158 bytes. Future changes use an unused version 6.4.172 or later, clean committed source, the canonical snapshot/builder, and the documented paired publication workflow.
- Keep the recorded acceptance limits. Do not call an assisted installation a scheduled pass, clear unresolved OTA debt to force a passing test, or treat a factory image as an ordinary update.
- Keep current release metadata synchronized with actual receipts. The private release archive contains source history and is not a public download asset. Do not push, upload, or delete archives merely as documentation cleanup.

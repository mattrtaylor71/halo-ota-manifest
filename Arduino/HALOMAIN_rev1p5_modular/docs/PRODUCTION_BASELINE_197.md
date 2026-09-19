# Production firmware baseline — September 19, 2026

**Start all further firmware and OTA work from the 197 source, preserving every preceding fix.** The compact machine-readable selector is [PRODUCTION_BASELINE.json](../PRODUCTION_BASELINE.json). The full historical ledger remains [RELEASE_BASELINE.json](../RELEASE_BASELINE.json); its old `current_baseline=158` means retained recovery evidence, not today's development source.

| Purpose | Version and exact identity |
| --- | --- |
| Required source for future development | 197, `7f87340d7c271bfdfc9b888aa6a0fcbf43822a8e` or a reviewed descendant |
| Exact source tag | `halo-v6.4.197` |
| 197 build | `6.4.197-20260918T231600Z-7f87340d7c27` |
| Factory package selected by EOL | 197; automatic blank-NVS handshake fix included |
| Current published OTA and configured bench | 196; exact public manifests and complete binaries reverified September 19 |
| Historical scheduled-OTA recovery package | 158, immutable; not a starting point for new work |
| Next changed build | Unused 198 or higher, after fresh release inventory |

The canonical checkout is `/Users/MattTaylor/halo-camera-recovery-2026-09-15`, branch `codex/halo-production-baseline-197`; firmware is under `Arduino/HALOMAIN_rev1p5_modular`. The older Documents/Arduino checkout and September 9 worktrees are historical and must not produce new releases. Their unrelated local edits are preserved.

This freeze changes release tooling and documentation only. It neither rebuilds existing version numbers nor changes installed devices, OTA manifests, wake schedules, allowances, provisioning or runtime behavior. The user-approved source designation is separate from an OTA publication or a factory unit's shipping acceptance.

## What this source preserves

- White menu cards with colored symbols, shopping-list scrolling and immediate delete feedback, revised capture/voice screens, and guided provisioning.
- Foreground user actions take priority over background work; the bounded voice/list coexistence fix remains included.
- Image and voice storage/retry paths, conservative pending-inventory handling, and dark background timer wakes with touch takeover.
- Explicit manual OTA requests without a daily tap limit; bounded scheduled/recovery policy, clock recovery and the existing 02:00 Pacific production schedule.
- Guarded automatic factory UART startup: absent/unsafe state stays quarantined until fresh healthy and idle LCD proof. Interrupted transfers remain protected.
- The next installation subtitle is “Keep Halo powered on,” included in 197, not retroactively in published 196.

## Preserved packages

`/Users/MattTaylor/halo-releases/6.4.197` contains exact application/factory artifacts, build-source and selected acceptance evidence. `/Users/MattTaylor/halo-releases/6.4.196` preserves the published applications and their manual-OTA evidence. Each has `MANIFEST.json`, `SHA256SUMS`, a README and a compressed archive alongside it. The release ledger pins package hashes after verification. The original build directories and their sealed receipts remain unchanged.

197 application SHA-256 values:

- Sense: `50f11891252244cb478cf86d308eb1d53abfb323988a266628c00faccf4b9504`
- LCD: `fd650cfc6521bb5fa133837bb09c91a6b629c53c423f9dd764fed36cbab330bb`

Use application images for ordinary OTA. Factory images erase NVS and provisioning; they are for an explicitly assigned fresh unit or approved recovery procedure. Do not apply one device's private flash backup to another unit. Packages are private engineering assets and are not uploaded by this freeze. Original absolute evidence/toolchain dependencies remain documented; relocating a package does not automatically make every build or acceptance tool portable.

## Required workflow

1. Read this handoff and the compact selector. Work from the canonical checkout or a reviewed descendant of the required source.
2. Run `python3 -B tools/verify_frozen_baseline.py`. It verifies current source ancestry as well as the retained 158 recovery bytes. For the archived 158 byte check, its `--package` option remains available.
3. Make only the requested change. Run the complete regression gate, review the diff and commit clean source.
4. Allocate a genuinely unused version of at least 198, then use `tools/prepare_production_release.py` to materialize that committed source. The preparer rejects older/unrelated lineage and old version numbers before creating output.
5. Run the snapshot's regression gate and canonical paired builder. The builder verifies materialized source and its original Git ancestry. Preserve the original repository/history for that check. Never use plain Arduino compilation to substitute a release profile.
6. Follow [BUILD_AND_RELEASE.md](BUILD_AND_RELEASE.md) for exact artifact checks, applicable finite device tests, paired publication/readback and release records. Source lineage is necessary, not proof that new changes are correct.

Older archived scripts cannot be retroactively guarded. Do not invoke them for new releases. The paired publisher still permits exact previously sealed artifacts under its existing proof rules; this freeze does not change its transport or retrospectively alter historical receipts.

## Evidence and limits

197 passed 99 host regression suites, canonical paired builds and artifact checks. The EOL pair passed full factory write/readback, automatic Sense identity before commands, fresh bidirectional production traffic, and paired app0/SDK VALID. The live EOL station selected 197 and passed its 73 host tests. Its unit remained at `functional_review`; that is not a physical shipping pass. [Factory evidence](FACTORY_UART_STARTUP_197.md).

196 passed the user's paired manual OTA, exact installed identities, policy settlement, Home and paired sleep. On September 19, two additional real Dish captures in one wake uploaded successfully and rendered at 1280 × 1024 in the gallery; byte counts matched cloud originals. Subsequent cloud records showed completed voice/check-in/dish/discard activity from the user's tests. USB-triggered captures do not establish physical button geometry or food recognition accuracy. [196 OTA evidence](MANUAL_OTA_196_20260918.md).

The 14:53 wake is consistent with the saved-media retry timer: 59 seconds selected at 14:52:31, LCD five-second lead, and Sense EXT0 wake. Current HTTP telemetry does not include the precise LCD trigger or pending-store mask, so a stale pending hint cannot be ruled out. The old diagnostic uploaded then was not evidence of a new crash. No new screen-lighting observation was made.

Retain these limits: no new 197 paired OTA/scheduled-OTA qualification; physical cold power, visible Settings and configured sleep/wake on the EOL unit were not established by these receipts; interrupted-update coverage is host-tested rather than a newly forced physical power cut. The existing post-upload DMA-reserve warning recurred on the configured 196 bench, without preventing the two deliveries; another capture after that warning in the same wake remains unqualified. The actuator recovered after reset and passed two calibrated taps plus a final probe, but its intermittent failure is not proved permanently cured.

September 19 evidence is retained under `/Users/MattTaylor/halo-image-review-20260919` and `/Users/MattTaylor/halo-production-baseline-20260919`. Baseline designations and documentation do not silently convert these limits into passes.

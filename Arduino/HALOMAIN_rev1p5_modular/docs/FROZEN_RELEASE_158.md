# Frozen HALO 6.4.158

**Historical recovery record: 158 is not the current development baseline.** Use [the 197 production handoff](PRODUCTION_BASELINE_197.md) for new work. This freeze preserves the published bytes, exact source, evidence, and operating procedure. It makes no runtime change, schedules no new test, and does not republish the OTA. Future firmware must also preserve the newer required source floor in that handoff.

## Exact identity

| Field | Frozen value |
| --- | --- |
| Paired version | `6.4.158` |
| Immutable local source tag | `halo-v6.4.158` |
| Artifact source commit | `b6d06da5997e2252a3472697f30dc8111ab90367` |
| Firmware subtree | `6e1a6b0bf8e8d5739075fa96e680667483312fd9` |
| Build ID | `6.4.158-20260914T145324Z-b6d06da5997e` |
| Build epoch | `1789397604` |
| Runtime fix ancestor | `df8cab35ef8128bdb3d312bd8c016817ad064a85` |
| Closed acceptance records commit | `f589a1148580ec7e563325e29b1e251deb30622d` |

The tag identifies the source that produced the binaries. Later documentation commits are expected descendants; they are not a different firmware build. The tag is local; neither tagging nor this freeze constitutes a remote Git push. Production OTA publication was already completed and verified separately.

| Application | Bytes | SHA-256 |
| --- | ---: | --- |
| Sense | 1,812,640 | `017795e6fc298c93d8ebeb7f4f3bcd1d3683d6db4902d07b19ba50ec0b2f5d56` |
| LCD | 1,891,872 | `d2d3e6eb38e09c5b699dd2cb7b25abdd139bf7649b7deb047bab1f51a228ee91` |

[RELEASE_BASELINE.json](../RELEASE_BASELINE.json) → `current_baseline` is authoritative only for these historical158 package hashes, publication and acceptance receipts. `PRODUCTION_BASELINE.json` selects current development. `historical_product_qualification_117` and the versioned campaign records preserve earlier evidence; their former “current” wording does not select the development baseline.

## Preserved files and verification

The private package is `/Users/MattTaylor/halo-releases/6.4.158`; its archive is `/Users/MattTaylor/halo-releases/halo-6.4.158-frozen.tar.gz`. The package includes board applications, ELF/map debug symbols, boot components, partition definitions, existing merged factory images, materialized build source, source history, and selected acceptance/build/publication evidence. Read its `README.md`, `MANIFEST.json`, and `SHA256SUMS` for exact inventory and declared exclusions.

The source export omits historical non-build raw logs with their omission hashes recorded. The private Git bundle retains exact tracked history, including historical tracked logs. New device NVS dumps, current raw serial captures, and private diagnostic journals are kept outside this release package. This archive is a private engineering recovery asset, not a public website attachment.

From the production firmware directory:

```sh
python3 -B tools/verify_frozen_baseline.py
python3 -B tools/verify_frozen_baseline.py --package /Users/MattTaylor/halo-releases/6.4.158
```

The first checks the exact source tag/tree, candidate ancestry, and retained application bytes. The second additionally checks the pinned package inventory and every payload checksum, and can use the archived applications if the original build directory has moved. Neither command opens USB, contacts a device/cloud service, changes Git state, or compiles firmware. It does not validate uncommitted firmware changes; the canonical source preparer separately requires committed clean source.

Verify the archive against its SHA-256 in `current_baseline.frozen_package.archive` before extraction. Extract into a new private directory, never over a live checkout. Follow the package README to restore its Git bundle into a separate repository. Retained binaries are the authority for byte-for-byte rollback: changed absolute build paths can change embedded `__FILE__` strings even with the same source and build epoch.

## What passed and what remains

The scheduled **157→158** update due September 14, 2026 at **09:10:36 PDT** passed without manual OTA or taps during its automatic window. Both exact images installed, both boards became app0 / SDK VALID, native policy reached RESOLVED before coordinator completion, and both boards slept. Three LCD packet retries recovered. There was no observed terminal transfer failure or watchdog reset.

After the case closed, a separate audited cleanup restored **daily 02:00 Pacific**, with the LCD timer 15 seconds earlier. It retired completed test bookkeeping after verifying resolution, preserved legitimate history and found no owed work. Next captured timers were September 15, 02:00:00 / 01:59:45 PDT. All case owners were closed and the previous overnight automation was paused.

This supports the previously agreed **small monitored rollout**, not a new exhaustive product qualification. Still unqualified on 158: USB-free operation, power interruption during OTA, a complete UI/product regression, and a paired factory station. The exact positive early-calendar WAIT branch has host tests, but that branch was not exercised in the successful hardware case. Initial raw serial prefixes were missed. Historical results and their limitations remain intact.

[OTA_158_VALIDATION.md](OTA_158_VALIDATION.md) explains the root causes, actual timeline, evidence, pass criteria, and how to run a bounded new scheduled test without destroying failure evidence. A download or a new version on both screens is insufficient: require exact hashes, SDK VALID, native durable settlement, no owed work, and sleep/schedule closure.

## Recovery and retained state

For an existing provisioned unit, prefer the supported paired OTA path. Preserve Wi-Fi/ownership, NVS, policy debt, diagnostics, selectors, and the prior VALID bank. A failed OTA retains an obligation for its exact target; a different newer image or an allowance reset does not prove recovery. Diagnose and retain the real failure before any declared service intervention.

The package's merged images are **factory/reset payloads**. Their component offsets and erased areas were checked offline; they have not been newly qualified through a factory station on this unit. They overwrite provisioning and selector state. Do not use them to perform an ordinary update or infer that a frozen tag authorizes erasing a provisioned device. Application-only USB service requires a board-specific plan based on fresh identity, real partition/selector state, both banks, and unresolved policy, with readback before coordinated release.

[PRODUCTION_FACTORY_RECOVERY.md](PRODUCTION_FACTORY_RECOVERY.md) retains the earlier 114 service procedure and explains the hazards. Its 114 payload names and old qualification are historical; use the exact 158 inventory for any new 158-specific service plan. Keep raw backups private and bound to their original board; never put one device's NVS on another device.

## Future work and safe retention

Use [BUILD_AND_RELEASE.md](BUILD_AND_RELEASE.md) for the next change. Run the baseline preflight, make one scoped change, verify it, commit clean source, allocate an unused version at or above the current production selector's minimum, prepare a snapshot, build both production targets, check the real artifacts, then stage/promote those same bytes with fresh paired predecessor receipts. Keep the reviewed ESP32 3.3.8 DNS patch, production flags, 96 KiB LVGL heap, and exact partition profiles. Changes to those dependencies need their own validation.

Never move `halo-v6.4.158`, overwrite its frozen payloads, or rebuild different bytes under version 158. The archive is also immutable; amendments go in new documentation commits or a separately named addendum. To compare or explore the old source, use a separate checkout; do not discard the current handoff by resetting this working directory.

Keep the frozen package/archive, the current repository, `hardware-validation157`, `hardware-validation158`, and `scheduled158-confirmation-20260914`. Keep the evidence paths explicitly referenced by the release records and the external artifact checker/toolchain closure. Original raw evidence is still needed for re-audit; the portable package does not copy every absolute dependency. Large old compile caches may eventually be expendable after a separate inventory, but a successful archive checksum alone does not authorize deleting historical failed-case evidence or unique backups. Maintain an independently verified backup before disk cleanup; this freeze is currently local to this Mac.

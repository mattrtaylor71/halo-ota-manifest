# Release baseline and future OTA development

**Production candidate: 6.4.108. Release status: pending.** Reserved canaries6.4.105–107 will validate the narrow post-LCD-failure retry correction under shipping limits. Versions102/103/104 remain preserved historical artifacts;104 is superseded and has not been published to production. The actual102-to103 scheduled case exposed an LCD HTTP failure followed by a retry-readiness rejection. This checkout remains the release preparation workspace on `codex/production-release-2026-09-09`. `RELEASE_BASELINE.json` binds actual results; unbuilt and unaccepted successor fields remain pending. Do not use the older original checkout for future firmware work until verified release adoption completes.

The source foundation is the tested M8 snapshot, checkpoint SHA256 `a74915125f46d8765de2027d698a5ca8cee901e3eadfa1b16a2d3c14765c4b54`. The [integration receipt](</Users/MattTaylor/halo-ui-implementation-2026-09-07/investigations/ota-recovery-20260908/production-source-integration001/MATERIALIZATION.json>) records its 1,111 non-cache files in this workspace. Release changes, including version headers and canonical build settings, require their own final diff and evidence. Optional M9 report-wake and observation/outbox work is excluded unless separately reviewed.

Before marking this baseline released, fill these fields from actual receipts:

| Required release binding | Current value |
|---|---|
| Source commit/tree and reviewed integration diff | Pending |
| Sense and LCD immutable binaries, versions, build IDs, sizes and SHA256 | Pending |
| Canonical compiler arguments, toolchain/SDK and partition hashes | Pending |
| Finite shipping acceptance and documented limitations | [Acceptance plan](PRODUCTION_RELEASE_ACCEPTANCE.md); results pending |
| Production manifests and exact served readback | Pending |
| Previous production pointer, rollback/factory instructions and cleanup | Pending |

The release becomes the default development baseline through the original checkout, not merely through these documents. After the verified commit exists, archive the original task-owned source/index and inspect a fresh working-tree/index diff. Advance the original development branch/checkout to that commit only after confirming ancestry and preserving unrelated dirty files. Do not use hard reset, clean or a wholesale copy to force adoption. If overlap prevents a safe fast-forward, reconcile those files explicitly and record the resulting adoption receipt. This operation does not change a remote default branch or another developer's branch. Fill `default_checkout_adoption` only after the actual result is verified.

Avoid self-referential commit hashes: the immutable artifact manifest names the exact source commit used for compilation. A subsequent documentation commit may record that source commit/tag and the publication receipts; it must not relabel different source or rebuild bytes as the already released artifact. Preserve both provenance links.

For later OTA changes:

1. Branch from the recorded release commit. Preserve its paired artifacts and manifests; never reuse a published version for changed bytes. Allocate the next unused version above the latest release.
2. Commit the reviewed source, then create a new external snapshot with `python3 -B tools/prepare_production_release.py --version <allocated-version> --epoch <explicit-UTC-seconds> --out <new-outside-git-path>`. Build both targets using that snapshot's `source/tools/build_ota_policy_production.py --out <new-build-path>`. Add `--private-canary` only for allocated canary versions; omit it for the final production route. Read the recorded actual arguments, commit/tree and linked results. Repository Version headers are source-only/precommit; the publisher rejects their provisional builds. `publish_both.sh` now delegates to the prebuilt-artifact publisher.
3. Keep private/test profiles explicit and separate. Tests on such profiles provide supporting evidence; shipping-specific admission, routes and limits need the exact shipping build. A runtime bench stop does not remove compiled capabilities.
4. Review source/resource changes and run the smallest relevant native and physical checks. Preserve prior campaign debt, reset attribution and provisioning in ordinary upgrades. Factory-reset testing is a separate recorded case.
5. Stage immutable paired artifacts, verify manifests and served hashes, then promote the intended production pointers using the reviewed publisher. Retain the previous pointer and exact recovery package. Updating a global latest manifest may affect every eligible device on that route.
6. Update this baseline with the actual new commit, artifacts, acceptance and publication receipts. Record unresolved limitations plainly; do not carry forward an old successful release label onto new bytes.

Factory instructions must specify board identity, actual partition offsets, bootloader/partition/application components, NVS preservation versus deliberate reset, provisioning steps and recovery verification. OTA rollback must use the supported version/rollback behavior; replacing a latest manifest with older bytes alone is not proof that devices will downgrade.

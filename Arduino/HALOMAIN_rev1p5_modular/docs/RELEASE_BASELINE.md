# Production baseline — released 6.4.114

**Latest published version: 6.4.115**, a version-only rebuild for the user’s manual OTA test (September 10, 2026, 17:20 PDT). Its runtime matches 6.4.114; 6.4.114 remains the last device-qualified pair. Exact115 build/publication records are under `published_manual_test_bumps` in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json). Publication did not reset the unit’s saved test allowance.

**Production 6.4.114 is live as of September 10, 2026, 16:56 PDT.** The same exact production pair passed manual and normal-calendar installation. Both boards finished SDK VALID and idle, with settled accounting, Pacific 02:00 scheduling and paired natural sleep restored.

The artifact source is commit `1224f28ab2d1307d54cf71956b30da2bbccead3d`, firmware tree `83c178bde2020c4e2fff800caf9e33be33a4ef97`. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) records the exact binaries, build proofs, package, acceptance, publication and release/tag/adoption bindings. Documentation and handoff commits do not change the identity of already compiled artifacts.

Version 113 supplied the private-route shipping baseline. Version 114 uses production endpoints. The manual case's native RESOLVED record predates the verification query. The scheduled case reported both new images before verification, while its durable accounting settled on the verification wake. The opening scheduled wake transition and earlier LCD sleep boundary were unobserved; final paired sleep after Pacific restoration passed. See [production acceptance](PRODUCTION_RELEASE_ACCEPTANCE.md) for the exact evidence and limits.

Extended fault and soak testing was deferred under the user's bounded release scope. Production promotion completed at 16:56 PDT, after the internal 16:52 target and within one hour of the user’s 15:57 instruction; final Git handoff followed. Earlier failed candidates remain in the [changelog](../CHANGELOG.md) and baseline JSON; they are not the default source for future releases.

## Future firmware work

1. Start from the accepted release commit/tag recorded in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json). Verify the checkout's adoption receipt and Git state before modifying firmware, and preserve unrelated user changes.
2. Allocate a new unused version above the released version. Commit the intended source, then prepare an external snapshot with `python3 -B tools/prepare_production_release.py --version <allocated-version> --epoch <explicit-UTC-seconds> --out <new-outside-git-path>`.
3. Build using that snapshot's `source/tools/build_ota_policy_production.py --out <new-build-path>`. Use `--private-canary` only for allocated private test versions; omit it for final production artifacts. Review the recorded arguments, source commit/tree, partitions, image sizes and resource checks. See [Production OTA build](OTA_POLICY_PRODUCTION_BUILD.md).
4. Repository Version headers are source-only/precommit metadata. The preparer generates the final three headers in the external snapshot; the publisher rejects provisional builds. Use the canonical builder rather than a direct Arduino build or an older checkout. `publish_both.sh` consumes prebuilt artifacts through the canonical publisher.
5. Run focused native and device checks on the changed behavior. Preserve provisioning, valid fallback images and campaign accounting during ordinary upgrades. A separately archived test fixture is not customer recovery or an in-case quota refund.
6. Stage immutable paired artifacts and verify served hashes before promoting production pointers. Never replace published versioned bytes. Preserve previous pointers and the exact recovery package; global latest promotion affects eligible devices on that route.
7. Record actual acceptance and publication, then commit/tag the release and safely advance the original checkout. Keep the artifact source commit separate from release-record commits. Bind completed facts without prospective hashes or success claims.

## Recovery and checkout handoff

The [factory and recovery guide](PRODUCTION_FACTORY_RECOVERY.md) identifies the exact 114 package, board layouts and retained-descriptor release rule. It separates preservation of an existing unit's provisioning from deliberate factory erasure. A rollback manifest alone does not prove that firmware will accept a downgrade. Physical cold-boot and factory-station qualification require their own evidence.

The default development checkout is `/Users/MattTaylor/Documents/Arduino/HALOMAIN_rev1p5_modular`. Its recorded adoption procedure archives the selected preimages and index, updates only release-owned files, retires only the exact archived local MQTT overrides, and preserves unrelated tracked and untracked changes. Verify the recorded adoption receipt before relying on that checkout's baseline. Hard reset, clean and remote branch replacement are outside this handoff.

Keep historical failed-case receipts and immutable artifact packages available through the baseline JSON. Follow the accepted tag and subsequent documented release commits for future work; do not relabel or rebuild old artifact bytes under a new source identity. The recurring soak automation remains paused. This release includes local Git handoff, not an unrelated remote repository push.

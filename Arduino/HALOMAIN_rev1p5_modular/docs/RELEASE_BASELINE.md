# Released 6.4.114 — September 10, 2026

Manual and normal-calendar scheduled installations of the exact production pair passed. Both boards are SDK VALID and idle; 2 a.m. Pacific and paired natural sleep are restored. Production latest is 6.4.114. Scheduled accounting resolved on the subsequent verification wake; the opening wake transition and earlier LCD sleep boundary were unobserved. See RELEASE_BASELINE.json for the actual acceptance/publication and checkout records. Extended fault and soak testing is deferred.

# Production baseline — 6.4.114

The exact production artifacts are built and packaged. Manual installation and paired health/accounting have passed. Scheduled acceptance and production promotion have passed. The artifact source is commit `1224f28ab2d1307d54cf71956b30da2bbccead3d`, firmware tree `83c178bde2020c4e2fff800caf9e33be33a4ef97`. `RELEASE_BASELINE.json` binds the actual paired binaries, build records, recovery package and acceptance receipts. Later documentation commits do not change this compiled source identity.

Version 6.4.113 is the private-route test baseline, using shipping policy. Version 6.4.114 uses the production route. Final acceptance covers a manual 113→114 update and a separate normal-calendar 113→114 update of the same exact production bytes, followed by paired health/accounting checks and restoration of 2 a.m. Pacific. The extended fault matrix is deferred under the user's one-hour shipping limit. Failed earlier candidates remain recorded in the changelog and baseline JSON; none is a released source to rebuild by default.

## Future firmware work

1. Start from the accepted release commit/tag recorded in `RELEASE_BASELINE.json`. The original development checkout must adopt that exact baseline while preserving unrelated user changes. Until adoption is recorded, use this release workspace rather than the older original checkout.
2. Allocate a new unused version above the released version. Commit the intended source, then prepare an external snapshot with `python3 -B tools/prepare_production_release.py --version <allocated-version> --epoch <explicit-UTC-seconds> --out <new-outside-git-path>`.
3. Build using the snapshot's `source/tools/build_ota_policy_production.py --out <new-build-path>`. Use `--private-canary` only for allocated private test versions. Omit it for the final production artifacts. Review the actual recorded arguments, source commit/tree, partitions and image sizes.
4. Repository Version headers are source-only/precommit metadata. The preparer generates the final three headers in the external snapshot; the publisher rejects provisional builds. Do not substitute a direct Arduino build or rebuild an older checkout. `publish_both.sh` consumes prebuilt artifacts through the canonical publisher.
5. Run focused native and device checks on the changed behavior. Preserve provisioning, valid fallback images and prior campaign accounting during ordinary upgrades. A separately archived test fixture is not customer recovery or an in-case quota refund.
6. Stage immutable paired artifacts and verify served hashes before promoting production pointers. Never replace already published versioned bytes. Preserve the previous pointers and exact recovery package; global latest promotion affects eligible devices on that route.
7. Record the actual acceptance and publication receipts, then commit/tag the release and safely advance the original checkout. Keep the artifact source commit separate from release-record commits; do not fill prospective hashes or success claims.

## Recovery and handoff

`docs/PRODUCTION_FACTORY_RECOVERY.md` binds the exact 114 package and board layouts. It distinguishes preserving an existing unit's provisioning from a deliberate factory erase. A rollback manifest alone does not prove that firmware will accept a downgrade. Physical cold-boot and factory-station qualification require their own evidence.

The default checkout is `/Users/MattTaylor/Documents/Arduino/HALOMAIN_rev1p5_modular`. Adoption archives the task-owned preimage/index, updates only the selected release files, retires only the exact archived local MQTT overrides and preserves unrelated tracked and untracked changes. No hard reset, clean or remote branch replacement is part of this operation. See the actual adoption receipt before assuming it is complete.

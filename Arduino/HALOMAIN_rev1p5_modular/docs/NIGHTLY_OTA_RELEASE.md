# Publish a release for the nightly OTA check

Promoting the approved paired production release makes it discoverable at the
device's next eligible OTA check. **No schedule change or fleet command is
required.** The ordinary nightly opportunity is 02:00 in each device's configured
timezone; publication does not wake devices or prove they installed the release.

## Prepare and publish the approved pair

1. Obtain explicit approval for the exact release version. Development builds,
   USB installs and commits do not publish anything. Follow
   [Build and release](BUILD_AND_RELEASE.md) for a fresh unused-version inventory,
   clean committed source, immutable snapshot, complete snapshot host gate,
   paired canonical builds and both artifact proofs. Use normal production
   routing, not `--private-canary` or a shortened bench schedule.
2. Save fresh Sense and LCD production latest bodies as predecessors. Reconcile
   any unexpected version/hash first. Run the documented local `prepare` with
   `--route production`, both exact proofs and the matching full `--host-result`.
   Keep the returned `release.json` and SHA unchanged.
3. Run **stage**, inspect its successful result and process closure, then run
   **promote** separately. For the explicitly approved 6.4.212 release, the
   remote commands are:

   ```sh
   # Use the qualified Python, snapshot publisher, prepared plan/SHA, installed
   # AWS CLI v2 Python and new evidence directories from BUILD_AND_RELEASE.md.
   "$PY" -B "$PUBLISHER" stage \
     --release "$OUT/publication/release.json" --release-sha256 "$RELEASE_SHA" \
     --profile trepo --aws-cli-python "$AWS_CLI_PYTHON" \
     --approve-production-version 6.4.212 --out "$OUT/stage001"

   # Run only after stage has closed successfully and its receipts are reviewed.
   "$PY" -B "$PUBLISHER" promote \
     --release "$OUT/publication/release.json" --release-sha256 "$RELEASE_SHA" \
     --profile trepo --aws-cli-python "$AWS_CLI_PYTHON" \
     --approve-production-version 6.4.212 --out "$OUT/promote001"
   ```

   This is a version-specific example, not a statement that 212 has published.
   Future releases need a fresh explicit approval and their own exact version
   on both commands. Missing or mismatched approval refuses before remote work.
4. Retain the successful stage/promote results and closed child-process receipts.
   Verify both latest manifests and full public BIN downloads against the exact
   artifact proofs; record source/tag, hashes and qualification limits. Update
   selectors and the release handoff only from actual receipts. Preserve the
   previous installed-device evidence separately.

Stage writes the four immutable Sense/LCD resources and leaves both production
latest pointers unchanged. Promote rechecks those resources and exact predecessor
bodies, then conditionally changes LCD latest followed by Sense latest. Devices
discover through these two public endpoints:

- [Sense production latest](https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/manifest_latest.json)
- [LCD production latest](https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/lcd/manifest_latest.json)

The two-pointer promotion is sequential, not atomic. On interruption, partial
promotion or uncertain readback, preserve the receipts and inspect actual served
objects before any separately reviewed recovery; never blindly rerun or roll back.
The [paired publisher](../halo_ota_demo/tools/ota/PAIRED_RELEASE.md) specifies these
checks. Historical publishers and direct cloud writes must not bypass approval.

## What happens at 02:00

Sense computes the next local 02:00 using the persisted timezone supplied at
owner claim. Only definite absence selects the US Pacific default
(`PST8PDT,M3.2.0,M11.1.0`); an unknown configuration is not a trustworthy schedule.
The calendar calculation accounts for daylight-saving transitions; a nonexistent
spring-forward 02:00 normalizes to the following hour. With no fresh confirmed
clock, sleep uses a six-hour relative recovery timer instead of promising 02:00.
The ordinary coordinated LCD arm prepares up to 15 seconds before its target.
Earlier media/OTA retry wakes can occur without granting a new nightly campaign.

Leave units powered, provisioned and able to reach working Wi-Fi, DNS, time sync
and the HTTPS OTA endpoints. A timed sleeping unit can wake; an unpowered unit
cannot. Both boards must establish valid running images and fresh peer readiness.
Foreground work, unsafe transport, uncertain storage, retained target/debt,
cooldowns and exhausted retry/work budgets can defer a check or installation.
Do not erase debt, change the clock, shorten schedules or enable test overrides
to make a release appear successful.

Keep the shipping defaults and canonical flags: OTA enabled, durable OTA policy
enabled, legacy orchestrator disabled, and bench/one-shot/ship-test/local-OTA-disable
controls off. A new published version is compared with each board's installed
version. A current Sense still checks/proxies a behind LCD; equal versions do not
reinstall merely because a new object was uploaded. Ordinary publication is not
a downgrade mechanism.

## Checks outside the nightly opportunity

02:00 is not the only allowed trigger. Software Update requests, bounded retained
recovery and provisioning completion can request a check. The provisioning path
waits until setup mode ends and then applies the same readiness, connectivity,
fresh-time and durable-policy guards. A genuinely absent policy record with no
legacy debt can start its first discovery without a 02:00 condition; existing
closed/deferred campaigns retain their due-time and budget rules. This path is
present in both 197/198 and current source, and can explain a fresh older unit
updating just after provisioning. The exact cause on a particular unit still
requires its logs/state; a cold boot or re-provision alone is not an unconditional
force-update command.

Source references: [nightly calendar](../halo_ota_demo/firmware/shared/NightlySchedule.h),
[Sense sleep timer](../Sense_Minimal/sense_sleep.h),
[provisioning trigger](../halo_ota_demo/firmware/shared/ProvisioningManager.cpp),
[coordinator and OTA entry](../halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino),
[durable admission](../halo_ota_demo/firmware/shared/SenseDurablePolicyRuntime.h),
and [first-discovery rules](../halo_ota_demo/firmware/shared/DurableOtaDiscovery.h).

## Record publication and installation separately

A public readback proves availability, not that every unit has a verified timer
arm, fetched the manifests or completed the update. For a scheduled-device pass,
retain its actual arm/timezone, timer-origin check, paired version/hash outcome,
SDK validation and normal return to sleep. Keep network failures and unobserved
units unqualified. No fleet-wide success is implied by one bench result.

The installed LCD firmware draws the update screen while it receives new
firmware. Consequently, a unit installing 212 can still display its older update
copy during that transfer; 212's new copy is available once 212 is running and
will be used by later OTA interactions. Publishing 212 cannot replace the UI
already executing on an older unit.

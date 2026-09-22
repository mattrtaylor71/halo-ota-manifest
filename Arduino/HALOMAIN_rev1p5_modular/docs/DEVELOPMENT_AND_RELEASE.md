# Develop privately, release deliberately

**Production is 6.4.211.** Its paired public OTA manifests, tagged source and
archived images stay unchanged while we develop the next version. A user tapping
Software Update on a normal production unit sees the production manifest, not
files we compile or flash over USB.

The normal process is:

1. Make and commit the requested change on a `codex/` development branch based
   on the current production source.
2. Allocate an unused version, prepare an immutable local candidate, and run the
   host tests and canonical builds. The candidate is **not published**.
3. Flash only the identified bench unit by USB; test the affected functions and
   relevant regressions. Save the exact build hashes and device results.
4. Review the result with Matt. **Only an explicit request to release that
   candidate to production authorizes publication.** “Build”, “flash”, “test”,
   “commit”, “push to the device”, and “looks good” do not authorize public OTA.
   Previous requests to publish other versions do not carry forward.
5. Publish the exact approved Sense/LCD files, verify both public downloads,
   then update the production baseline, release tag, archive and handoff.

This changes host tooling and workflow, not the firmware's OTA, Wi-Fi, sleep,
provisioning, quota or partition behavior. No firmware release is needed to start
following this process. The retained 211 physical-test limits remain documented
in [its handoff](RELEASE_211.md); choosing it as production does not invent tests.

## Local candidate workspace

Use `/Users/MattTaylor/halo-firmware-candidates/` for candidates and evidence.
Each candidate gets a new directory; never overwrite a tested build. Keep this
outside Git. Source belongs in Git; build artifacts and private device logs do
not. `PRODUCTION_BASELINE.json` continues to identify production; do not move it
to an unapproved candidate or confuse it with a candidate's `CANDIDATE.json`.

Use the qualified Python, with inherited virtual-environment overrides removed:

```sh
PY=/Users/MattTaylor/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3.12
unset __PYVENV_LAUNCHER__ PYTHONPATH
```

From the firmware root, inspect production with a read-only command:

```sh
"$PY" -B tools/firmware_candidate.py production-status
```

It compares both public latest manifests with the exact recorded production
release. A mismatch stops the workflow for reconciliation; it never repairs or
reverts a manifest automatically. Network unavailability is an inconclusive
check, not evidence that production changed.

Before allocating a version, check current baseline, local candidates/tags and
immutable cloud artifacts as described in [Build and release](BUILD_AND_RELEASE.md).
The next available starting range is 6.4.212+. An example version is not a
reservation. Use normal numeric versions because the device's version comparison
expects them; do not invent `-dev` suffixes. Once a version has been installed
or remotely staged, new changed firmware gets a new version. Skipped production
version numbers are fine.

For a reviewed version and explicit UTC build epoch:

```sh
"$PY" -B tools/firmware_candidate.py plan \
  --version "$VERSION" --epoch "$BUILD_EPOCH" --out "$CANDIDATE"
"$PY" -B tools/firmware_candidate.py prepare \
  --version "$VERSION" --epoch "$BUILD_EPOCH" --out "$CANDIDATE"
```

`plan` only prints commands. `prepare` requires clean committed firmware,
preserves production metadata, and creates the external source snapshot,
`CANDIDATE.json` and `NEXT_STEPS.md`. It does not build, access a device, or
publish. Run the separate commands in `NEXT_STEPS.md` for the exact-snapshot
host suite, paired canonical build, pinned artifact checker and candidate
verification. The helper contains no cloud-write or flash command. The pinned
checker currently depends on this Mac's qualified toolchain; relocation requires
reviewed setup rather than silently choosing another compiler/checker.

The default uses **production routing** in the locally stored binary. That lets
us bench-test the exact files we may later release, without rebuilding them at
release time. Compiling a production URL into a local image does not publish it.
If the candidate changes after testing, repeat the applicable tests on the new
candidate; never rebuild different bytes under the approved artifact identity.

`verify --candidate "$CANDIDATE"` rechecks source identity, paired artifact
proofs, full host results and the qualified checker. Its result is
`READY_FOR_BENCH_TEST_NOT_PRODUCTION_APPROVAL`, not a claim of device acceptance.
Candidates remain bound to their recorded checkout baseline and toolchain paths.
If production advances while a candidate is waiting, verification refuses the
stale baseline: rebase/review it against the new production source and prepare
a fresh candidate. Retained historical release receipts remain historical evidence.

## Bench installation and evidence

Follow [Bench candidate flashing](BENCH_CANDIDATE_FLASH.md). Reuse the existing
reviewed inactive-bank service implementation, binding the plan to the actual
unit, running firmware and slot state. Historical per-version installers need
those bindings updated; they are not general-purpose commands to rerun blindly.
There is deliberately no unrestricted “flash whatever is on this USB port” shortcut.

Use both USB connections for routine development when available. If the
assembled Sense is inaccessible, USB flashing only the LCD is not a paired
installation; arrange Sense access or use the isolated OTA test path below.

Record a `DEVICE_TEST.md` beside the candidate with the board MACs, actual
running versions/build hashes, SDK validation, installation method, relevant
test steps/results, log references and unresolved limitations. A practical
functional change check includes the changed interaction, its failure/retry
path where relevant, fresh Home, and normal sleep/wake. Network, storage or OTA
changes require corresponding tests; cosmetic copy does not justify a new soak.
Do not erase Wi-Fi, ownership, saved uploads or OTA debt just to get a passing
test. A USB install is not a successful OTA test.

## Optional bench-only OTA testing

USB is the default and requires no remote publication. When the feature under
test is OTA itself, use the existing isolated canary manifest route with an
explicitly prepared bench unit. Normal production units keep following the
production route and cannot discover the canary latest manifest through their
update button.

The canonical builder's `--private-canary` option changes compiled routing.
Those images are not byte-identical to production-route candidates and must
not be relabeled as production artifacts. The existing
[paired canary bridge](../halo_ota_demo/tools/ota/PAIRED_RELEASE.md) can test the
exact final production bytes: stage immutable candidate objects, leave
production latest unchanged, and point only the canary manifest at those bytes.
This requires a separately authorized isolated OTA experiment, fresh inventory
of the shared canary namespace, and an already canary-routed bench. After it
installs production-route firmware, it follows production again.

“Canary” means discovery isolation, not confidential storage or a separate
application backend. Uploads and telemetry can still reach real accounts. No
canary, S3 policy or device routing is changed by setting up this workflow.

## Explicit production release

For nightly discovery, use [Publish for the 02:00 OTA check](NIGHTLY_OTA_RELEASE.md);
promotion needs no device schedule change and does not prove fleet installation.

Only after the user explicitly approves the candidate for production, follow
the existing [release procedure](BUILD_AND_RELEASE.md) against the exact tested
pair. Local manifest `prepare` remains non-publishing. Both remote production
phases now require an exact additional argument:

```sh
"$PY" -B "$PUBLISHER" stage --release "$PLAN" --release-sha256 "$PLAN_SHA" \
  --profile "$AWS_PROFILE_NAME" --aws-cli-python "$AWS_CLI_PYTHON" \
  --approve-production-version "$VERSION" --out "$CANDIDATE/stage001"
"$PY" -B "$PUBLISHER" promote --release "$PLAN" --release-sha256 "$PLAN_SHA" \
  --profile "$AWS_PROFILE_NAME" --aws-cli-python "$AWS_CLI_PYTHON" \
  --approve-production-version "$VERSION" --out "$CANDIDATE/promote001"
```

Missing/wrong approval refuses before remote activity. The flag records the
operator's intent; it is not a substitute for the user's instruction. Stage
uploads immutable objects; **promote** changes the production discovery pointers.
Neither build nor commit runs these commands. Existing artifact, full-test,
predecessor/ETag, paired-order and readback checks remain enforced. On uncertain
publication, inspect actual receipts and served objects; never blindly retry.

The legacy single-board publishing CLIs refuse non-dry-run calls and direct
users to this paired workflow. These are accidental-operation safeguards in the
current tools, not an IAM security boundary: someone holding cloud write
credentials could bypass them with old scripts or direct AWS calls. Historical
release snapshots remain immutable; never use their old publishers for a new
release. Access-control separation can be added later if another host/team needs
an independently restricted publishing role.

Changing public latest does not forcibly downgrade units that already installed
a candidate. Keep the previous production artifacts for recovery; handle a
necessary rollback as a reviewed device recovery or a newer corrective release.

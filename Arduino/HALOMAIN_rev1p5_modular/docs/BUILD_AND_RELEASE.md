# Build and release from the current production source

**Current published firmware: 6.4.211; device testing pending.** Discard and dish now use their requested Got it subtitles; 210 automatic check-in is retained. Paired canonical artifacts, all 114 exact-snapshot host suites and complete public downloads passed. The last verified installed pair remains 209 (Sense app1/LCD app0 SDK VALID). Source `731e4897c845e6bb891f277f0e82e210c380150c`, tag `halo-v6.4.211`; future versions require unused 212+ after inventory. Camera, network, OTA and storage behavior are unchanged; prior network/UI lease limits remain open. Factory 197/frozen 158 stay separate. [Current handoff](RELEASE_211.md).

The retained `RELEASE_BASELINE.json.current_baseline` and `development_baseline` identify historical158 recovery/scheduled qualification. They do not select new development. Its tag, package and previous acceptance stay immutable; see [historical158 recovery](FROZEN_RELEASE_158.md). Retain the immutable158/196/197/198 packages under `/Users/MattTaylor/halo-releases`, with their hashes and original evidence. Never rebuild/relabel existing versions.

## Development is local by default

Follow [Development and release](DEVELOPMENT_AND_RELEASE.md) for the candidate workspace, selected-unit USB testing and explicit release boundary. Building the production profile does not publish it. Stop after local build/test/bench work unless the user explicitly asks to release that candidate. Prior publication requests are not blanket approval for later versions.

## Before changing anything

Work on a clean, reviewed descendant of the required 211 source. Preserve unrelated user work. Run the read-only baseline guard from the firmware root:

```sh
python3 -B tools/verify_frozen_baseline.py
```

Resolve any ancestry or hash mismatch before proceeding. Make only the requested change, run the complete [host regression gate](REGRESSION_TESTING.md), and commit the reviewed source before preparing release artifacts. The guard verifies current-source ancestry and historical recovery bytes; it does not establish device acceptance of new code. The source preparer independently enforces the same current source floor and minimum new version.

Allocate an unused version **6.4.212 or later** from the current release record and actual immutable artifact inventory. A version written in an example is not a reservation. The source preparer validates metadata but does not allocate versions or compare against production latest. Never recycle a published version or change a frozen payload in place.

For the commands below, set actual reviewed values. Use Python 3.12, absolute output paths outside Git, and fresh directories. Avoid spaces in the snapshot path because the LCD LVGL configuration path is a compiler macro.

```sh
PY=/Users/MattTaylor/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3.12
RELEASE_VERSION='<allocated-unused-version-at-least-6.4.212>'
BUILD_EPOCH='<explicit-UTC-Unix-second>'
OUT='/absolute/path/to/new-release-workspace'
mkdir "$OUT"
```

## 1. Prepare committed source

From the clean firmware root:

```sh
"$PY" -B tools/prepare_production_release.py \
  --version "$RELEASE_VERSION" --epoch "$BUILD_EPOCH" --out "$OUT/snapshot"
```

The preparer rejects old/unrelated source and reused baseline version numbers, then extracts only the tracked firmware subtree, verifies required dependencies, rejects dirty/untracked source and local MQTT credential overrides, and generates exactly three version headers. Retain `snapshot/materialization.json`: it records the full source commit/tree, explicit build inputs, original source hashes and materialized hashes. Do not hand-edit generated headers or substitute repository source-only metadata.

Run the complete offline gate from that immutable snapshot before publication:

```sh
env -u __PYVENV_LAUNCHER__ "$PY" -B \
  "$OUT/snapshot/source/tools/run_regression_suite.py" --out "$OUT/regression"
```

Require `regression/RESULT.json` to pass with no skips and unchanged source. The publisher binds this result and its logs to both exact board proofs and rechecks it before remote writes. A working-tree pass cannot replace the snapshot result. This adds no device interaction; perform the finite physical checks relevant to the change separately.

## 2. Build the production profile

```sh
"$PY" -B "$OUT/snapshot/source/tools/build_ota_policy_production.py" \
  --out "$OUT/build"
```

The default builds both boards. `--board sense` or `--board lcd` is available for an explicitly scoped rebuild; a release still needs both exact proofs. Use `--plan` to inspect commands without compiling. **Do not add `--private-canary` for a normal production release.** That option selects the historical fixed private route and creates different routing bytes.

Preserve Arduino ESP32 **3.3.8**, its exact reviewed DNS-cache lock patch, the qualified toolchain/libraries, the owned LCD `lv_conf.h`, and both partition tables. The canonical builder checks the SDK correction and actual compiled NetworkManager object, records full flags/FQBN, and refuses unknown partition hashes. Follow [the build profile](OTA_POLICY_PRODUCTION_BUILD.md) if setting up a new host; SDK patching is an explicit separate action, never an automatic upgrade during compilation.

Keep durable policy/diagnostics enabled, bench/one-shot/probe/credential-provisioning controls disabled, and production routing/telemetry labels intact. Optional B1 admission export requires an existing credential and is not an OTA dependency. The builder defaults to an 8 GiB free-space floor; a documented `--min-free-gib` override must reflect the measured host budget. Never delete backups or qualification evidence to make a build fit.

## 3. Check the actual artifacts

The established artifact checker is currently external to the repository:

```sh
CHECKER=/Users/MattTaylor/halo-provision-memory202-20260921/service203-prep/check_release_artifacts_203.py
"$PY" -B "$CHECKER" --build-root "$OUT/build" \
  --materialization "$OUT/snapshot/materialization.json" --boards sense lcd
```

Its reviewed SHA256 is `4e3791533f26fcf04b2d458322bb39ed0582e34c4d547b0f6bc1f9feddd8331d`. This camera-aware descendant is required for203+ Sense builds: it verifies both canonical build properties and the pinned camera-driver/wrapper receipts. The historical v2 checker expects only one build property and refuses the legitimate camera wrapper flag; do not remove that flag or relax the check. Preserve its pinned dependency closure and installed toolchain paths; this command is not a claim that the checker is independently portable. A relocated host needs a reviewed packaging adaptation, not fabricated proof files.

Retain both `artifacts/verified.json` files, BIN/ELF/partition/map/stack evidence, compiler results, SDK patch/object receipts, and artifact-check result. Review resource changes against 158. Compilation and artifact checks establish package identity and layout; perform the relevant finite device acceptance separately. Preserve failed debt and real history. Do not reuse old campaign fixtures to manufacture a clean baseline or infer completion from a successful download alone.

## 4. Prepare, stage, and publish exact bytes

See [Publish for the 02:00 OTA check](NIGHTLY_OTA_RELEASE.md) for scheduled-device
eligibility, other update triggers and the distinction between publication and installation.

First save fresh, read-only paired production latest bodies in a new directory:

```sh
mkdir "$OUT/predecessor"
curl --fail --silent --show-error --retry 0 \
  https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/manifest_latest.json \
  --output "$OUT/predecessor/sense.json"
curl --fail --silent --show-error --retry 0 \
  https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/lcd/manifest_latest.json \
  --output "$OUT/predecessor/lcd.json"
```

Compare both board/version/hash identities with the recorded current publication. Unexpected or mixed latest versions require reconciliation. Preserve the exact bodies as conditional publication predecessors; historical copies are not a fresh preflight.

```sh
PUBLISHER="$OUT/snapshot/source/halo_ota_demo/tools/ota/publish_pair.py"
"$PY" -B "$PUBLISHER" prepare --route production --version "$RELEASE_VERSION" \
  --sense-proof "$OUT/build/sense/artifacts/verified.json" \
  --lcd-proof "$OUT/build/lcd/artifacts/verified.json" \
  --host-result "$OUT/regression/RESULT.json" \
  --baseline-sense "$OUT/predecessor/sense.json" \
  --baseline-lcd "$OUT/predecessor/lcd.json" --out "$OUT/publication"
```

`prepare` is local: it validates proofs and copies the exact images. Record its returned release SHA. Only after explicit user approval to publish this candidate, run the two separate remote phases using actual configured AWS values. Both require `--approve-production-version` equal to the pinned release version; the flag never supplies user authorization by itself:

```sh
RELEASE_SHA='<SHA-returned-by-prepare>'
AWS_PROFILE_NAME='<configured-profile>'
AWS_CLI_PYTHON='<absolute-installed-AWS-CLI-v2-Python>'
"$PY" -B "$PUBLISHER" stage --release "$OUT/publication/release.json" \
  --release-sha256 "$RELEASE_SHA" --profile "$AWS_PROFILE_NAME" \
  --aws-cli-python "$AWS_CLI_PYTHON" --approve-production-version "$RELEASE_VERSION" --out "$OUT/stage001"
"$PY" -B "$PUBLISHER" promote --release "$OUT/publication/release.json" \
  --release-sha256 "$RELEASE_SHA" --profile "$AWS_PROFILE_NAME" \
  --aws-cli-python "$AWS_CLI_PYTHON" --approve-production-version "$RELEASE_VERSION" --out "$OUT/promote001"
```

Stage must close successfully before promotion. Immutable writes require absence; promotion rechecks served artifacts and exact predecessors, changes LCD latest then Sense latest, and performs full readback. Preserve uncertain/partial results; there is no blind automatic retry or rollback. Reverting latest does not force firmware downgrade.

Finish by recording actual source, paired hashes, acceptance scope, publication/readback, process closure and tag in `current_working_source`, the compact production selector and the changelog. Preserve the separate historical recovery records; do not overwrite them with a prospective release. Archive the complete release package and distinguish local Git tagging from any separately requested remote push. The new baseline becomes authoritative only when those actual results exist.

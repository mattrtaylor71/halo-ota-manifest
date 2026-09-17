> September17 user test: **both boards completed the physical185→186 manual OTA and run app0 / SDK VALID**. Exact hashes passed; native policy resolved, Home returned and both boards slept. **OTA presentation failed:** the user saw a frozen Checking for updates screen during LCD transfer because the flash-safety guard suppresses rendering. This remains unfixed; do not call186 full UI acceptance. The final02:00 Pacific schedule is verified unchanged. See `docs/MANUAL_OTA_186_20260917.md` and the current release record.

> Current source:186 retains185 runtime unchanged. Preserve the full185 acceptance under `previous_working_source_185`; the installed pair is still185. See [186 publication and pending manual test](MANUAL_OTA_186_20260917.md). Preparing a separate external allowance is not evidence that a grant or OTA has occurred.

# Build and release from the frozen 6.4.158 baseline

Use `RELEASE_BASELINE.json` → `current_working_source` as the authoritative source starting point (159 or a reviewed descendant). The retained `current_baseline` identifies frozen158 recovery artifacts and the previous paired scheduled-OTA qualification; it does not select an older working checkout. The following paragraph records that immutable158 reference. Its artifact source is commit `b6d06da5997e2252a3472697f30dc8111ab90367`, firmware tree `6e1a6b0bf8e8d5739075fa96e680667483312fd9`, build epoch `1789397604`, and build ID `6.4.158-20260914T145324Z-b6d06da5997e`. The local annotated tag is `halo-v6.4.158`. Creating that tag did not push Git history or tags remotely; S3 firmware publication is a separate, completed operation.

The frozen package belongs at `/Users/MattTaylor/halo-releases/6.4.158`, with its portable archive alongside it. Use the package inventory/checksums and release receipts to verify the actual files. Keep the published BINs authoritative. A rebuild under a different absolute snapshot path can change embedded `__FILE__` strings, even with identical source/version/time. Do not rebuild and republish 158 under its existing immutable identity.

The qualification is specific: one scheduled 157→158 paired OTA, exact image hashes, selected/running SDK VALID, native RESOLVED with zero reservation and no separate debt, followed by verified restoration of daily 02:00 Pacific and genuine history. Three LCD packet retries recovered. Initial serial prefixes were missed; the positive early-calendar WAIT branch was covered by host tests, but was not exercised in that device run. Historical 117 product/UI qualification remains historical; it is not a claim that every product feature was retested on 158.

## Before changing anything

Work on a clean, reviewed descendant of the frozen 158 source. Preserve unrelated user work. Run the read-only baseline guard from the firmware root:

```sh
python3 -B tools/verify_frozen_baseline.py
```

Resolve any ancestry or hash mismatch before proceeding. Make only the requested change, run relevant host regressions, and commit the reviewed source before preparing release artifacts. The baseline guard verifies identity; it does not establish device acceptance of new code.

Allocate an unused version **6.4.187 or later** from the current release record and actual immutable artifact inventory. A version written in an example is not a reservation. The source preparer validates metadata but does not allocate versions or compare against production latest. Never recycle a published version or change a frozen payload in place.

For the commands below, set actual reviewed values. Use Python 3.12, absolute output paths outside Git, and fresh directories. Avoid spaces in the snapshot path because the LCD LVGL configuration path is a compiler macro.

```sh
PY=/Users/MattTaylor/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3.12
RELEASE_VERSION='<allocated-unused-version-at-least-6.4.187>'
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

The preparer extracts only the tracked firmware subtree, verifies required dependencies, rejects dirty/untracked source and local MQTT credential overrides, and generates exactly three version headers. Retain `snapshot/materialization.json`: it records the full source commit/tree, explicit build inputs, original source hashes and materialized hashes. Do not hand-edit generated headers or substitute repository source-only metadata.

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
CHECKER=/Users/MattTaylor/halo-ui-implementation-2026-09-07/investigations/ota-recovery-20260908/production-source-integration001/check_release_artifacts_v2.py
"$PY" -B "$CHECKER" --build-root "$OUT/build" \
  --materialization "$OUT/snapshot/materialization.json" --boards sense lcd
```

Its reviewed SHA256 is `a042bdb8d6e9e3fe050b6a4301a5880dd9a6feeee72801a186d2a335cba3f1bd`. Preserve its pinned dependency closure and installed toolchain paths; this command is not a claim that the checker is independently portable. A relocated host needs a reviewed packaging adaptation, not fabricated proof files.

Retain both `artifacts/verified.json` files, BIN/ELF/partition/map/stack evidence, compiler results, SDK patch/object receipts, and artifact-check result. Review resource changes against 158. Compilation and artifact checks establish package identity and layout; perform the relevant finite device acceptance separately. Preserve failed debt and real history. Do not reuse old campaign fixtures to manufacture a clean baseline or infer completion from a successful download alone.

## 4. Prepare, stage, and publish exact bytes

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
  --baseline-sense "$OUT/predecessor/sense.json" \
  --baseline-lcd "$OUT/predecessor/lcd.json" --out "$OUT/publication"
```

`prepare` is local: it validates proofs and copies the exact images. Record its returned release SHA. For an authorized deployment, run the two separate remote phases using actual configured AWS values:

```sh
RELEASE_SHA='<SHA-returned-by-prepare>'
AWS_PROFILE_NAME='<configured-profile>'
AWS_CLI_PYTHON='<absolute-installed-AWS-CLI-v2-Python>'
"$PY" -B "$PUBLISHER" stage --release "$OUT/publication/release.json" \
  --release-sha256 "$RELEASE_SHA" --profile "$AWS_PROFILE_NAME" \
  --aws-cli-python "$AWS_CLI_PYTHON" --out "$OUT/stage001"
"$PY" -B "$PUBLISHER" promote --release "$OUT/publication/release.json" \
  --release-sha256 "$RELEASE_SHA" --profile "$AWS_PROFILE_NAME" \
  --aws-cli-python "$AWS_CLI_PYTHON" --out "$OUT/promote001"
```

Stage must close successfully before promotion. Immutable writes require absence; promotion rechecks served artifacts and exact predecessors, changes LCD latest then Sense latest, and performs full readback. Preserve uncertain/partial results; there is no blind automatic retry or rollback. Reverting latest does not force firmware downgrade.

Finish by recording actual source, paired hashes, acceptance scope, publication/readback, process closure and tag in `current_baseline` and the changelog. Archive the complete release package and distinguish local Git tagging from any separately requested remote push. The new baseline becomes authoritative only when those actual results exist.

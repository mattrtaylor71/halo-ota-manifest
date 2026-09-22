# Prebuilt paired release

Development builds and USB bench tests are local by default; see
[Development and release](../../../docs/DEVELOPMENT_AND_RELEASE.md). Production
stage and promote require a separate explicit user release decision and
`--approve-production-version` matching the pinned plan version. Missing or
wrong approval is refused before remote access. Canary phases do not require
that production flag. The legacy single-board CLIs are dry-run only. Historical
version numbers below illustrate the original campaign, not the current release.

`../../publish_both.sh` now packages and publishes verified prebuilt images. It never compiles or changes version headers. Build separately with the canonical `tools/build_ota_policy_production.py`; supply the actual artifact checker's `verified.json` for both boards. Provisional or explicitly held proofs are refused.

Local preparation checks the exact canonical compiler command, actual flag proof, source partition table, compiled partition evidence, app capacity, and unique version/build/board marker. It copies exact BIN bytes, writes deterministic manifests, and pins all provenance, publisher, guard, and builder inputs. The package retains absolute provenance paths; preserve those files unchanged until publication completes.

There are two fixed destinations:

| Route | Bucket | Prefix | Required build |
| --- | --- | --- | --- |
| `production` | `halo-ota-prod` | `halo/ota/prod/` | Shipping policy, normal production route |
| `private-canary` | `halo-ota-dev` | `halo/ota/canary/production-release-20260909/dev/` | Same shipping policy, canonical `--private-canary` route |

Prepare locally:

```sh
./publish_both.sh prepare --route production --version 6.4.104 \
  --sense-proof /absolute/final104/sense/artifacts/verified.json \
  --lcd-proof /absolute/final104/lcd/artifacts/verified.json \
  --host-result /absolute/final104/regression/RESULT.json \
  --baseline-sense /absolute/saved-production-sense-latest.json \
  --baseline-lcd /absolute/saved-production-lcd-latest.json \
  --out /absolute/release104
```

Use `--route private-canary` for 102/103. The first empty canary namespace may omit both baseline arguments; authenticated absence is then required at staging and promotion. Production always requires exact previous latest bodies.

Run `stage`, then separately `promote`, each with `--release /absolute/release104/release.json`, its returned `--release-sha256`, `--profile`, `--aws-cli-python` pointing to the installed AWS CLI v2 Python interpreter, `--approve-production-version 6.4.104` for this historical production example, and a fresh `--out` evidence directory. Staging writes only the four immutable objects. It refuses existing versioned manifests and uses `If-None-Match: *` for every immutable PUT. Promotion first checks all served objects and exact previous latest bodies, then changes LCD latest followed by Sense latest using their observed ETags. Every write gets a full public GET/hash check.

Each phase has a 600-second bound. Each command reserves five seconds for owned process-group cleanup. The S3 before-send guard permits one transmission per operation, including redirects; there is no automatic retry or rollback. A promotion-attempt receipt is fsynced before each latest PUT. An interrupted or uncertain operation remains `ATTENTION_NO_AUTOMATIC_RETRY`, with per-board attempted and verified states. Inspect those receipts and current objects before deciding any separate recovery action. Restoring old manifest bytes does not establish that firmware downgrade policy will accept an older image.

## Exact production-byte bridge

Stage the production104 package first, leaving production latest unchanged. Prepare a separate canary bridge:

```sh
./publish_both.sh prepare-bridge \
  --release /absolute/release104/release.json --release-sha256 EXACT_RETURNED_SHA256 \
  --baseline-sense /absolute/saved-canary103-sense-latest.json \
  --baseline-lcd /absolute/saved-canary103-lcd-latest.json \
  --out /absolute/canary104-bridge
```

Stage and promote that bridge using the same two remote phases. It writes only the two canary manifest files and their latest references. Both manifests remain byte-identical to production versioned104 and retain the canonical production BIN URLs. Before each bridge phase, all four production resources must be served with exact hashes. No BIN is copied or renamed into the canary prefix. After the declared device qualification of the exact104 images, separately promote the original production104 package.

Offline checks:

```sh
python3 -B -m unittest discover -s halo_ota_demo/tools/ota -p test_publish_pair.py
```

These tests exercise local identity checks, conditional write ordering, failed/partial promotion, the S3 transmission guard, byte-identical bridge, and process cleanup. They make no remote requests and do not establish device or fleet qualification.

# HALO 6.4.207 production release

**Published for Sense and LCD on September 22, 2026.** This is the source for
future firmware work. Continue from `fcf6fb220f1f4983557669e8e3b552f910e079c1`
or reviewed descendants on `codex/halo-production-baseline-197`. The branch
name is historical; it does not select firmware 197. Allocate an unused
208+ version after checking the actual inventory.

## What changed since public 201

- Free the provisioning scan cache when setup finishes, and add bounded
  allocation diagnostics for voice, account linking and image uploads.
- Recover camera initialization when internal DMA memory is fragmented using
  the guarded, pinned-driver PSRAM fallback. Keep allocation ownership in
  internal memory and preserve camera cleanup and user-priority behavior.
- Reduce image PUT header memory and pace body writes to reduce transient
  DMA pressure. Preserve upload identity, saved-media custody and retries.
- Keep account-linking and image TLS allocations in external RAM within
  exclusive task-owned scopes. Other tasks and voice/OTA retain their prior
  allocation policy; hardware DMA allocations remain with the SDK.
- Fix the LCD guardian's immediate blackout after prolonged provisioning or
  fresh user input. Retain the setup success transition and normal idle sleep.

The 201 completed-OTA-hint repair and all earlier fixes are retained. This
release does not change production 02:00 Pacific scheduling, manual-update
allowances, network credentials or unresolved OTA debt.

## Exact release

| Field | Value |
| --- | --- |
| Version | `6.4.207`, both boards |
| Source tag | `halo-v6.4.207` (local annotated tag) |
| Artifact source | `fcf6fb220f1f4983557669e8e3b552f910e079c1` |
| Firmware tree | `2489f338c55c9a9af14f49c4408ea70fc8d50f24` |
| Build ID | `6.4.207-20260922T073628Z-fcf6fb220f1f` |
| Sense | 1,874,752 B; SHA256 `a6a46832b39dd7b05b677657b7af1e17818b5a234909b7357f1c77e120076e0e` |
| LCD | 2,033,104 B; SHA256 `ffb78b24586709e570c9a6647e5dd4f0462f80897ac2388173fd35312d147338` |

The Sense image is the exact image tested on the bench, without rebuilding it.
The matching LCD image comes from the same immutable source snapshot. Among
the previous LCD206 build's 168 actual source dependencies, only the generated
version header differs. LCD207's exact bytes have not been installed on the
bench; do not relabel the observed Sense207/LCD206 case as paired207 acceptance.

## Verification and limits

Both working-tree and exact-snapshot gates passed all **113 suites**. Both
canonical production builds and artifact checks passed. The publisher
revalidated the exact source, test logs and board proofs, staged immutable
resources, conditionally promoted both latest manifests from 201, and read
them back. A separate public download of each complete binary matched its
expected size and SHA256. No test-route or accelerated-schedule build was used.

One actual phone provisioning followed by immediate Check-in on Sense207 and
LCD206 passed account linking in 1.04 seconds and image PUT on the first attempt.
Complete claim/upload allocation-failure windows recorded zero failures. The
cloud JPEG passed checksum verification and full decode. Internal heap after
PUT connection was 27,672 B versus 16,764 B in the previous failing case. These
are checkpoint measurements across two cases, not a worst-case memory bound.
The LCD completed setup, accepted Check-in/Confirm, darkened normally while
upload finished, and both boards slept. See the
[memory audit](POST_PROVISION_RESOURCE_AUDIT_207.md) for the exact observations.

This is a scoped production release with explicit remaining work:

- Repeated provisioning, voice and input-interruption/resume acceptance have
  not been repeated on this release. There is no new paired207 OTA transfer,
  scheduled OTA, cold-power, USB-free or full-product qualification.
- Camera fragmentation/reserve warnings remain; successful fallback was
  observed. The claim allocator's final fallback counter was truncated in
  serial output, although its failure trace was complete.
- Exhausted account-claim retry bookkeeping and the permanent 64 KB internal
  OTA marker buffer remain separate issues. They were not changed to make
  this release pass.
- Cloud image storage was verified, not downstream recognition/inventory
  semantics. One successful case is not a reliability guarantee.

## Evidence and continuing work

Private release evidence is in
`/Users/MattTaylor/halo-provision-memory202-20260921/release207`:

- `RELEASE-RECORD.json`, `PAIR-ARTIFACTS.json`, `SOURCE-TAG.json`.
- `snapshot/materialization.json`, `regression/RESULT.json`, both board proofs.
- `publication/release.json`, `stage001/result.json`, `promote001/result.json`,
  `PUBLIC-READBACK.json` and independent `release-review/READINESS.json`.
- The sibling `reprovision-memory207-01/CASE-RESULT.json` and cloud receipt;
  raw device evidence stays private, outside the release package.

The earlier `QUALIFICATION.json` records a pre-install checkpoint and is
preserved as history. The release record and current selectors supersede its
pending wording. Documentation commits are separate from the artifact source.
OTA publication does not imply a remote Git push or a device installation.

The verified private archive is `/Users/MattTaylor/halo-releases/6.4.207.tar.gz`
(40,584,242 B, SHA256
`984dafb10e11b049daad344efe958fd24f40a70106a0e651e8a19be69091f964`).
Its 1,754 members were rehashed from the archive, including the exact source
bundle, materialized source, board artifacts, host logs and publication
receipts. The adjacent `6.4.207.ARCHIVE-RECORD.json` binds the inventory. Device
NVS, current raw serial captures and user media are excluded; historical Git
content means the package remains private. It is a retained OTA/debug package,
not a newly qualified factory-flash image. The final release record adds the
archive reference outside the archive to avoid circular hashes.

[PRODUCTION_BASELINE.json](../PRODUCTION_BASELINE.json) is the authoritative
source selector. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) preserves
the current release and historical evidence. Follow
[BUILD_AND_RELEASE.md](BUILD_AND_RELEASE.md) and retain the pinned SDK,
production flags and full snapshot regression gate. EOL factory selection
remains 197; frozen158 remains the historical recovery package.

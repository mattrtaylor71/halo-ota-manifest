# Candidate 6.4.212 — ready, held from production promotion

**Do not promote 212 until a new explicit publish command.** The user withdrew
publication and wants to observe existing devices for several days.
[Production remains 211](RELEASE_211.md); earlier approval does not carry forward.

Workspace: `/Users/MattTaylor/halo-firmware-candidates/6.4.212`.
The authoritative [HOLD.json](/Users/MattTaylor/halo-firmware-candidates/6.4.212/HOLD.json)
records the pinned evidence and publication boundary.

- Source: `ee3fd65bd15cb7206e5e4493faa8fbcc3a1693a1`.
- Firmware tree: `2eab1b218209d96e722d330a313f7f3e4024c348`.
- Build: `6.4.212-20260922T204419Z-ee3fd65bd15c`.
- Prepared plan SHA256: `33dc1379275eae2b7941efa780cc8ad696409252478fed01ea21d75f6443cf24`.

The only firmware change is the LCD OTA instruction “Keep your Kitchen Assistant
powered on”, rendered on two lines. Camera, network, OTA admission/transfer,
storage, provisioning and sleep behavior are unchanged. The 211 confirmation
subtitles, 210 automatic check-in and reviewed development/release safeguards remain.

Both working-source and exact-snapshot gates passed all **115 suites**. Both
canonical shipping builds and artifact proofs passed; scope comparison confirmed
the text-only runtime change plus generated version/build headers. Actual LVGL
font/text calculations passed copy fit, without physical panel testing. Evidence:
[snapshot gate](/Users/MattTaylor/halo-firmware-candidates/6.4.212/regression/RESULT.json),
[paired artifacts](/Users/MattTaylor/halo-firmware-candidates/6.4.212/build/artifact-result-v2-sense-lcd.json),
[scope](/Users/MattTaylor/halo-firmware-candidates/6.4.212/SCOPE-REVIEW.json),
[copy fit](/Users/MattTaylor/halo-firmware-candidates/6.4.212/copy-layout/RESULT.json).

[Stage001](/Users/MattTaylor/halo-firmware-candidates/6.4.212/stage001/result.json)
completed four immutable writes and readbacks, with **zero latest-pointer writes**
and all 12 children reaped. Promotion was never invoked. Staged URLs exist, but
ordinary production discovery still serves 211; the
[fresh latest check](/Users/MattTaylor/halo-firmware-candidates/6.4.212/HOLD-PRODUCTION-STATUS.json)
confirmed both recorded 211 identities.

**212 device acceptance is pending.** Last verified installed bench firmware
remains 209 (Sense app1/LCD app0, SDK VALID), with no new hardware test claimed.
Prior network/UI lease limits remain open. An older LCD draws its existing update
screen while installing 212; the new reminder appears during later OTA interactions
once LCD 212 runs.

After a new explicit publish command, recheck current latest against the exact
saved predecessors and revalidate source, tests, proofs, plan hash and served
immutable bytes. If production or pinned inputs changed, reconcile first; allocate
a new version if needed. **Do not rerun staging, overwrite immutable objects or
rebuild different bytes as 212.** With intact evidence, use the reviewed,
plan-pinned paired publisher for **promote only** in a new evidence directory,
with `--approve-production-version 6.4.212`. Resolve publisher-pin mismatches
without bypassing guards; verify closure and complete public readbacks before
recording publication.

The [nightly playbook](NIGHTLY_OTA_RELEASE.md) describes unchanged 02:00 local
checks and eligibility. Fresh 197/198 units can also check after provisioning
when first-discovery policy permits; they currently discover 211, not held 212.
Candidate 212 retains this behavior. Observation requires no schedule or debt changes.

> Execution checkpoint: corrected190 source and both images are built; all97 snapshot suites pass.190 installation is pending because the actuator no longer answers. Follow [the190 results and resume sequence](VOICE_LIST_UPLOAD_VALIDATION_190.md), which supersede prospective189 version/identity examples below. Public188 is unchanged. Service requires the pinned esptool virtual environment, not the host test Python.

# Fresh voice → list refresh: bounded device acceptance plan

Prepared September17,2026; this is a plan, not completed candidate acceptance.
The hardware owner alone runs the commands below. Keep each failed receipt and
distinguish console input, physical input, backend custody and semantic completion.

## Implemented scope awaiting candidate acceptance

The Sense-only fix gives a fresh voice job priority only at the FIFO or parked
head; it does not bypass an older image or saved-owner work. It keeps the
original12s budget and one attempt, with new camera/voice input and OTA or
provisioning exits interrupting that work. A validated202 queues one coalesced
list fetch; user refresh retains the normal3s cooldown. This is not an automatic
claim that backend processing or the list mutation has completed. LCD runtime
is unchanged. The complete97-suite host gate, canonical build and finite device
checks remain separate evidence requirements.

## Starting evidence and ownership

The last completed manual OTA installed188 on both boards: Sense app1 and LCD
app0, SDK VALID. Exact pair:
`/Users/MattTaylor/halo-manual188-20260917/candidate188-001/RELEASE-PAIR.json`,
SHA256 `76e8a29f0ebae795f9f19922e5b5609782ade7d863ee29ba773d93c0cfaa107c`.
The actual acceptance and independent review are `USER-MANUAL188-RESULT.json`
and `USER-MANUAL188-INDEPENDENT-REVIEW.json` in the same release workspace.
That case reported RESOLVED, no reservation/retry, and17ms remaining work; it
does not promise another immediate OTA allowance. Do not reset quotas or debt
for this voice/list test.

Read-only inspection around September18 01:58–02:00UTC found no matching live
capture/identity/service/tap process and no `lsof` owner of USB modem ports.
The visible callout leaves were `/dev/cu.usbmodem213301` and
`/dev/cu.usbserial-21320`; no Halo USB leaf was present. Names alone do not prove
identity, sleep or connectivity. Recheck ownership immediately before action.
Do not open ports merely to inventory them.

The hardware owner's completed189-workspace setup is recorded separately from
candidate acceptance. `relay001/RESULT.json` at
`/Users/MattTaylor/halo-voice-list189-20260917` records actuator BSD disappearance
and return, restore acknowledgement, relay closure and worker reaping (SHA256
`0fd186ae5e1e61e8e23b12de735a2c395be193b27e12ff7ccb02bd822b5a8ef4`).
It does not measure the power rail or itself prove the actuator sketch alive.
The subsequent `identity188-001/wake.json` contains HELP with PUSH, one completed
stroke and descriptor closure (SHA256
`3188643d8e834d747f774ad0f3128e49058a4f21a8be44ca273799c08e7cc040`).
`identity188-001/identity.json` binds actual188 Sense app1/LCD app0 SDK VALID,
Home and idle at epoch1789697486.499877 (SHA256
`222d747dc6c7efe8be52ef1a89cd9ffe11810a90b7d71efb8bd570f3dce80ccb`).
This is historical setup evidence; service must collect a new identity when
its120s admission age expires. The earlier failed HELP attempt remains retained.

## Qualified primitives and adaptations still required

| Purpose | Existing resource | Restriction |
|---|---|---|
| Passive paired capture and explicit command inbox | `/Users/MattTaylor/halo-upload181-20260917/tools/capture.py` | SHA256 `4311f960bea98d7614b0e43d9c66b05d2ea43122c27da23452335459fbe781cf`; new output, finite maximum1800s, no automatic commands or modem-line calls |
| One bounded calibrated wake | `/Users/MattTaylor/halo-upload181-20260917/tools/tap_once_trace.py` | SHA256 `76b9f273a9bd178db522a816a0a5991909027c8649ee01fa0676609077416d69`; checks HELP, one `PUSH:500,200,500`, Complete and descriptor closure; caller must supervise25s |
| Relay recovery | `tools/actuator_relay_reset.py` | SHA256 `ae14e5fc7a756dadf60406a7e215ebadec78c4a12b85598a3cfa28ddae173368`; confirmed COM/NC wiring, idle actuator only; no stroke |
| Fresh paired Home/idle identity | `/Users/MattTaylor/halo-voice-list189-20260917/tools/identity188.py` | Exact188 pair, Sense app1/LCD app0; SHA256 `8ea7b74a61fb5fcf6c5ddd5d8fc0179cd489013679f7b5e47ea56edd61c325a7`; actual setup receipt above |
| Sense-only application service | `/Users/MattTaylor/halo-voice-list189-20260917/tools/install_sense189_only.py` | Prepared from185 owner with unchanged physical workflow; exact188 current proof/slots; target189 remains HELD until the actual sealed pair is reviewed |
| Voice/list interruption and custody parser | `/Users/MattTaylor/halo-upload181-20260917/tools/timing183_ui_priority.py` | Old version/slot gates must be adapted. Reuse `voice_acceptance`, `live_list_evidence`, fresh identity and cleanup; do not weaken gates |

Identity admission must use a fresh closed capture, nonce/CRC-verified LCD ID1,
exact paired builds, running=selected SDK VALID, unlocked Home, no OTA/binary
activity, and empty coordinator owner/lease. Service admits identity at most120s
old. Preserve actual raw proofs, never fill identity fields from expectations.
The188 pair schema differs from older service inputs: validate its actual
`boards.*.verified` files and their artifact/partition references when adapting;
do not manufacture missing shipping-profile assertions.

For a Sense-only runtime change, retain LCD188 and both original selected banks.
Service requires full bank backups, exact MAC/table/selector checks, unchanged
NVS/bootloader/policy and readback before selecting only the inactive Sense bank.
An install/release receipt is not SDK-health proof: wait for original postboot
mark-valid/current readback before opening any new Sense reader. If the patch
changes LCD runtime too, this Sense-only plan is insufficient.

## Commands for the hardware owner

Set new external directories and reviewed values; these are command templates,
not evidence that a candidate, adapter or inventory check already exists.

```sh
FW=/Users/MattTaylor/halo-camera-recovery-2026-09-15/Arduino/HALOMAIN_rev1p5_modular
PY=/Users/MattTaylor/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3.12
HW_PY=/Library/Frameworks/Python.framework/Versions/3.8/bin/python3
OUT='<new absolute case workspace>'

# Only when actuator HELP/open recovery is needed, with confirmed COM/NC wiring:
"$HW_PY" -B "$FW/tools/actuator_relay_reset.py" --out "$OUT/relay001"
# Add --degraded-start only if the exact actuator BSD serial is already absent.

# Start once in its own supervised task; do not run concurrently with service.
"$HW_PY" -B /Users/MattTaylor/halo-upload181-20260917/tools/capture.py \
  --out "$OUT/voice-list001" --seconds 420

# Separate supervised task, only if Halo is asleep/USB absent; maximum25s:
"$HW_PY" -B /Users/MattTaylor/halo-upload181-20260917/tools/tap_once_trace.py \
  "$OUT/wake001.json"
```

If wake fails or ownership is ambiguous, close/reap that owner before recovery;
do not loop strokes or relay cycles. Capture uses original descriptors while
awake, but can reopen after disappearance. USB opens remain an intervention:
never start/reconnect a reader during an active update or PENDING_VERIFY boot.
The historical serial warning remains relevant;188's successful original-handle
capture does not prove arbitrary reconnects harmless.

The future reviewed Sense adapter must keep this interface. Run input validation
first, then the identical arguments with `--execute` only after review:

```sh
"/Users/MattTaylor/halo-ui-implementation-2026-09-07/investigations/ota-recovery-20260908/bench-recovery-install68-001/esptool53-env/bin/python" -B "$SENSE_SERVICE_ADAPTER" \
  --current-sense-pair "$CURRENT188_PAIR" --current-lcd-pair "$CURRENT188_PAIR" \
  --target-pair "$NEW_PAIR" --target-pair-sha256 "$NEW_PAIR_SHA256" \
  --identity "$FRESH_CLOSED_IDENTITY" --out "$OUT/service001"
```

## Version and build evidence before service

189 is only the next candidate number, not a reservation. The hardware owner
must retain authenticated HeadObject and full ListObjectVersions results for
both exact keys below.403 is not absence; any prior version/delete marker needs
review. Re-read both latest manifests separately and preserve their exact bytes.

```sh
AWS=/opt/homebrew/Cellar/awscli/2.31.7/bin/aws
"$AWS" --profile trepo-dev --region us-east-1 s3api head-object \
  --bucket halo-ota-prod --key halo/ota/prod/manifest_6.4.189.json
"$AWS" --profile trepo-dev --region us-east-1 s3api list-object-versions \
  --bucket halo-ota-prod --prefix halo/ota/prod/manifest_6.4.189.json
"$AWS" --profile trepo-dev --region us-east-1 s3api head-object \
  --bucket halo-ota-prod --key halo/ota/prod/lcd/manifest_6.4.189.json
"$AWS" --profile trepo-dev --region us-east-1 s3api list-object-versions \
  --bucket halo-ota-prod --prefix halo/ota/prod/lcd/manifest_6.4.189.json

# Clean reviewed committed source, after actual version inventory:
"$PY" -B "$FW/tools/verify_frozen_baseline.py"
"$PY" -B "$FW/tools/prepare_production_release.py" \
  --version "$RELEASE_VERSION" --epoch "$BUILD_EPOCH" --out "$OUT/snapshot"
"$PY" -B "$OUT/snapshot/source/tools/run_regression_suite.py" \
  --source-root "$OUT/snapshot/source" --materialization "$OUT/snapshot/materialization.json" \
  --history-repo /Users/MattTaylor/halo-camera-recovery-2026-09-15 \
  --out "$OUT/host001" --jobs 2
"$PY" -B "$OUT/snapshot/source/tools/build_ota_policy_production.py" \
  --board both --min-free-gib 4 --out "$OUT/build"
```

For this build only, the hardware/build owner selected the canonical
`--min-free-gib 4` reserve with approximately5.1GiB free, based on the prior188
paired build's measured355MiB footprint. The default8GiB reserve is not silently
changed. No caches, artifacts or evidence were deleted. This is a resource
planning decision, not proof that the new build has completed or cannot run out
of disk; retain actual build exit/proofs.

Require actual complete regression results, canonical board `verified.json`
proofs, artifact checker and sealed pair. Compare actual resources and runtime
scope with188. No publication or candidate installation is implied by this plan.

## Finite acceptance sequence

1. On the current baseline or installed reviewed candidate, prove fresh exact
   identities and Home/idle; record radio/time state and existing media queues.
   Never erase a pre-existing queue. A missing or busy admission is a failed or
   blocked case, not permission to inject input through an OTA lock.
2. Capture one actual spoken, unique shopping-list fixture phrase using physical
   voice input. If console `voicestart`/`voicestop` is used, label it explicitly;
   these record the microphone, they do not inject a transcript/audio file. The
   old3s ambient fixture proves capture/transport only. Record START/END, job,
   frozen owner/device/session/request, byte count and SHA without content dumps.
3. In the same wake, enter/refresh the list through the intended physical input
   as soon as recording ends. Allow one additional refresh only if the backend
   is still processing. Record user-input→HTTP start, HTTP200/parse completion,
   UI_LIST receipt/application and visible active list. A cached UI_LIST or
   cumulative counter is not proof of a new GET or uploaded voice.
4. Require the exact voice's validated HTTP202 receipt within the existing
   bounded case. Independently verify that request's cloud custody/processing
   and that the unique fixture appears once in the intended list.202 is custody,
   not semantic completion. Record latency; do not invent a passing threshold
   after observing results. Use420s as a host case cap, not a device deadline.
5. Run one separate overlap case: request list refresh while this exact voice
   upload is active, requiring preservation/resumption of the same job and a
   live list result. If acceptance already completed before input, record that
   overlap was missed. A single optional offline/restart replay case may use the
   existing RAM-only `backupoffline <32hexnonce> 180` diagnostic only when root
   includes that fault; it is not RF loss. Require typed durable custody before
   sleep, same-identity replay, and never clear or refill policy budgets.
6. Let Home and natural coordinated paired sleep occur, then observe at least20s
   quiet. Verify actual health, zero active transfer/owner flags, preserved02:00
   schedule and unchanged provisioning. Close/reap capture, verify final RESULT
   and descriptor closure. Remove only the unique test-list fixture if its exact
   identity is confirmed and cleanup is authorized; retain all original items.

The final receipt must keep image/voice durability, backend semantic outcome,
list freshness, responsiveness, SDK health and sleep as separate checks. An
offline save is not online-delivery success; a host/console pass is not a
physical-input, weak-radio, USB-free or full-product qualification.

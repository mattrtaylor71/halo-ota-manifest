# Production retry bookkeeping correction

Status: implementation and release preparation, 28 September 2026. Publication,
paired OTA and device acceptance must be recorded from actual receipts below;
none is implied by this document.

Matt explicitly requested publication of firmware addressing unnecessary media
retry wakes, after a quick test. Investigation is retained locally at
`/Users/MattTaylor/halo-wake-audit-20260928/WAKE_INVESTIGATION.md`.

## Scope and ancestry

This isolated release branch starts at `01c90cb70ecba06bc77fb505a939a999015926d5`:
production 211 behavior plus the already-reviewed 212 power-reminder wording and
host release/resource tooling. It deliberately excludes private 213–222 runtime
changes that require broader qualification. Those branches and artifacts remain
intact, as does unrelated uncommitted battery-monitor work.

Runtime corrections are confined to three Sense headers:

1. After SPIFFS mount failure, clear only the VoiceFlash retry hint if a bounded
   read proves the entire matching, nonencrypted partition is erased. Never
   format, remove media, or treat unknown storage as empty.
2. After confirmed saved-voice deletion, request one fresh owner/device/probe-bound
   inventory within the original four-second UART lease. Update only VoiceSd;
   clear the cursor when the authoritative count is zero.
3. Apply the same reconciliation to saved images, updating only ImageSd.

Both post-delete paths preserve the successful deletion outcome if the optional
inventory fails. A lost/malformed/mismatched/late inventory or foreground takeover
retains pending state. No retry cadence, clock, nightly schedule, OTA admission,
partition, credentials, queue format or LCD transport behavior changes.

The source includes the approved reminder “Keep your Kitchen Assistant powered
on.” Held 212's immutable files are unchanged and will not be repurposed.

## Versions and exact test path

Fresh local/tag/production and canary artifact inventory found 223 and 224 unused.
The preparation allocates **6.4.223** as a private-canary baseline, using the
unchanged 211 upload/retry runtime. **6.4.224** is reserved for the fixed production
candidate. New changes after either becomes immutable require a new version.

The proposed test is source-211 behavior in canary 223 → exact production 224
bytes. It is not literally an installed version-211 device, and the baseline's
canary discovery route is intentionally different from production. This allows
private OTA validation without publishing an untested production latest pointer.

Use the current reviewed paired publisher and bridge. Production immutable files
may be staged before the test; production latest must remain 211 until the tests
pass. Freshly inventory the shared canary route and retain predecessor bytes.
Do not publish canary-baseline 223 to production.

## Finite acceptance

- Actual-function host tests: erased/unknown/nonblank/encrypted/read-error flash,
  durable-write failure/reboot, preservation of every other store bit; voice/image
  successful last deletion, remaining records, bad/missing/wrong-owner/device/probe
  replies, failure/deadline/user takeover. Retain production-source negative controls.
- Full immutable-snapshot regression gate and canonical paired builds; verify
  source hashes, feature flags, slots, static sections and stack-frame changes.
  Use the resource review; unknown dynamic measurements remain unknown.
- Exact bench identity and fresh running/boot/SDK state before any protected
  inactive-bank service; preserve NVS, saved uploads and valid fallback images.
- Private OTA of both exact candidate images, with recorded trigger, transfer,
  post-update hashes/versions, SDK validation, policy settlement and normal sleep.
- One finite RAM-only offline episode producing actual image and voice SD custody,
  followed by native recovery. Match exact stored payload identities to cloud
  completion and local deletion/inventory. Prove both inventories empty, pending
  state cleared, zero peer media timer, and at least 75 seconds of uninterrupted
  sleep after the final successful-delivery cycle. This targets the old 60-second
  stale follow-up; initial genuine retry on the preserved 211 policy may take five
  minutes and must not be shortened to make testing faster.
- Fresh Home, list refresh, ordinary action and normal next-2-a.m. arm. Known
  unrelated network failures must be reported and evaluated, not relabeled passes.

The separate early-timer/paired-board wake behavior is not fixed here. A cloud
completed job is not proof of local receipt or deletion. Customer-specific queue
state remains unknown until observed; this release addresses the reproduced
bookkeeping defects without discarding legitimate backlog.

## Evidence and publication

Candidate workspaces: `/Users/MattTaylor/halo-firmware-candidates/6.4.223` and
`/Users/MattTaylor/halo-firmware-candidates/6.4.224`. Each retains its own snapshot,
tests, build proofs and device receipts. Update this section after those operations
actually complete. Current public manifest verification still reports paired 211.

Resource review: [retry reconciliation resource review](RETRY_RECONCILIATION_RESOURCE_REVIEW_20260928.md).
Publication procedure: [nightly release](NIGHTLY_OTA_RELEASE.md).

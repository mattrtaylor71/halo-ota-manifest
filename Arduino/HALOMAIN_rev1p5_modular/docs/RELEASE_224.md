# Release 6.4.224

**Status: published for Sense and LCD on September 28, 2026.** Both production
latest manifests and complete public application downloads match the tested
224 artifacts. One bench pair is installed at 224/app1, SDK VALID. Acceptance
combines exact private OTA transfers, subsequent fresh paired identity, real
saved-media recovery, cloud custody and a later passive quiet interval. The
original OTA, media and short health controllers retain their
**FAIL_OR_INCOMPLETE** results and the limits below.

## Change and source

224 corrects stale media retry hints that can schedule another wake after saved
work has already been delivered. Source is
`518691c8d3bd609272b438ec31b82c4de1b6b053`, firmware tree
`ae7726fdad7edea9d51ed4191da845deaef31455`, build
`6.4.224-20260928T164640Z-518691c8d3bd`.

Continue from this source or reviewed descendants in
`/Users/MattTaylor/halo-retry-release-20260928`, branch
`codex/halo-retry-release`. Annotated tag `halo-v6.4.224`, object
`0806a73ecb256cf541cf1df04c77ee1685f32013`, selects this exact commit. Both the
source branch and release tag were pushed; the remote tag was independently
verified. The original source-tag receipt records local creation, with a separate
remote verification receipt. New versions require freshly inventoried unused
225 or later.

The release preserves production 211 runtime behavior and includes the approved
212 reminder, “Keep your Kitchen Assistant powered on.” It adds only three
retry corrections:

- After a failed VoiceFlash mount, clear that store's hint only when a bounded
  read proves the entire matching unencrypted partition is erased. Unknown,
  nonblank, unreadable or late results retain pending state; no format or media
  erasure is performed.
- After a confirmed saved-voice deletion, request fresh owner/device/probe-bound
  inventory within the existing four-second UART lease. Update only VoiceSd and
  clear its cursor when the authoritative count is zero.
- Apply the same reconciliation after saved-image deletion, updating only
  ImageSd. Both paths retain pending state on uncertainty or foreground takeover
  while preserving the already-confirmed deletion result.

Retry intervals, clocks, nightly schedules, OTA admission, partition geometry,
credentials and queue formats retain their 211 behavior. Private 213–222 runtime
changes and unrelated battery-monitor work are excluded. Held 212 artifacts and
frozen 158 recovery remain separate and unchanged. See the
[scope](RETRY_RECONCILIATION_RELEASE_20260928.md) and
[resource review](RETRY_RECONCILIATION_RESOURCE_REVIEW_20260928.md).

## Builds, host tests and resources

All **119 working-source suites** and all **119 exact 224 snapshot suites**
passed, with source hashes unchanged during each gate. The working-source gate
preceded the final source commit; the immutable snapshot gate binds the committed
source and build above. Both canonical production builds and the paired artifact
checker passed. The private 223 baseline separately passed all 116 suites.

| Board | Application bytes | OTA slot bytes | Remaining margin |
|---|---:|---:|---:|
| Sense | 1,875,504 | 1,966,080 | 90,576 (4.61%) |
| LCD | 2,033,088 | 2,621,440 | 588,352 (22.44%) |

Exact application SHA-256:

```text
Sense 62d73b1f3724c64bd3664fb265ecd667392166d7be2e321a131d33a3f4a07edb
LCD   fdf0e90cafcf4a5c407dec33eac5f577923c506228d8731779591a53a307a3dd
```

Static RAM sections are unchanged from the exact 211 artifacts. Sense DRAM is
159,524 bytes, IRAM 81,152, and RTC fast/slow 136/2,528. LCD DRAM is 204,356 bytes,
IRAM 85,248, and RTC fast/slow 136/268. Both have zero static PSRAM sections.
Application growth is 2,096 bytes for Sense and 48 for LCD; both retain the 64 KiB
OTA-margin review floor. These section measurements are not runtime free-memory
or stack measurements. The optional inventory uses heap-backed ArduinoJson;
unchanged static RAM does not imply zero added dynamic demand.

The external resource guard reports compiled artifacts **PASS** and overall
**REVIEW_REQUIRED** because its historical 213 calibration does not establish a
211-derived runtime envelope. Whole-call-chain stack use, dynamic peaks and
worst-case foreground latency remain unqualified.

Receipts: [working gate](/Users/MattTaylor/halo-retry-release-evidence-20260928/working-regression/RESULT.json),
[snapshot gate](/Users/MattTaylor/halo-firmware-candidates/6.4.224/regression/RESULT.json),
[paired artifacts](/Users/MattTaylor/halo-firmware-candidates/6.4.224/build/artifact-result-v2-sense-lcd.json),
[static comparison](/Users/MattTaylor/halo-retry-release-evidence-20260928/resource224/REPORT.md),
[resource guard](/Users/MattTaylor/halo-retry-release-evidence-20260928/resource-guard224/REPORT.md).

## Private paired OTA evidence

Private 223 uses the source-211 upload/retry behavior with a canary discovery
route. The qualification is **private 223 → exact production 224**, rather than
a literal installed-version-211 test. Protected inactive-bank service installed
223/app0 on both boards, preserving the prior Sense 221 and LCD 222 app1 banks
and their backups. NVS, partition table and filesystem checks passed. Ordinary
OTA then selected app1, retaining 223/app0 as the fallback.

One normal Settings update request transferred both exact 224 application
lengths and hashes above. LCD reboot and 224/app1 SDK VALID were captured; LCD
also forwarded Sense 224/app1 while it was PENDING_VERIFY. The Sense USB stream
did not provide its subsequent first-224-boot trace. Software reboots did not
produce the descriptor reopen events expected by the original observer, so its
result remains **FAIL_OR_INCOMPLETE**. It was stopped after native sleep; a
separate closure receipt verifies closed descriptors and no remaining owned
process group. No second OTA request or policy/debt reset was used.

The next ordinary wake, captured before the media fault, independently observed
both exact 224 builds, running and boot partitions app1, SDK VALID, fresh nonce
and CRC-bound LCD identity, post-command Sense FW_INFO, idle Home and no
coordinator lease. Observed boot work was settled, current-wake network/clock
admission passed, and both SD inventories were mounted, successful and empty.
The final closed media receipt retains these observations before its fault.

These observations compose transfer evidence with fresh subsequent paired
health. They do not turn the original observer into a pass or reconstruct the
missing first Sense boot, its erased-flash probe timing, or an uninterrupted
first-boot policy/sleep trace. Native OTA-policy `RESOLVED` was not captured;
later SDK VALID does not establish that separate policy state. See the
[bench execution record](/Users/MattTaylor/halo-wake-audit-20260928/bench-release-execution.md),
[original OTA result](/Users/MattTaylor/halo-wake-audit-20260928/bench-private/release-runs/wake224-ota-20260928-01/RESULT.json)
and [separate observer closure](/Users/MattTaylor/halo-wake-audit-20260928/bench-private/release-runs/wake224-ota-20260928-01/stop-closure.json).

## Saved-media recovery and final observations

One real image and one voice recording were saved to SD during a bounded,
RAM-only network fault. Native sleep armed the unchanged five-minute media
retry. Voice was delivered on the first native retry wake; the real remaining
image was delivered on the subsequent 60-second retry wake. Both exact saved
jobs received delivery acknowledgments and were deleted. Fresh matching
post-delete inventories reported zero;
durable pending and backoff reached zero, and zero peer media arms were
acknowledged. The test did not erase queues, NVS, schedule or OTA debt to obtain
these results.

Sense entered native sleep with a 56,345-second nightly timer. LCD entered
native sleep with a 56,320-second `maintenance_abs` timer and SDK timer
56,320,000,000 microseconds. Its complete current epoch, 1790616065, plus that
timer equals the earlier recorded preparation target, 1790672385, for the
1790672400 nightly start. The final LCD target field was interleaved with an
ERRLOG diagnostic and truncated. The strict media parser therefore stopped;
its original result remains **FAIL_OR_INCOMPLETE**, and it did not observe the
planned uninterrupted 75 seconds after delivery. The arithmetic and earlier
calendar record support the composed timer assessment, not a complete final
calendar line.

Independent read-only cloud evidence matched the image's exact 152,484 bytes
and SHA-256, and the voice job's exact 106,496 bytes and SHA-256. The voice job
completed with one ingestion, worker start and worker completion. Image
recognition, voice transcription semantics and list insertion were not assessed.
Cloud custody corroborates the local delivery/delete observations; it does not
replace them.

A final ordinary wake again proved both exact 224/app1 builds SDK VALID. List
entry and one refresh each returned seven items with settled UI state. The
controller queried UI immediately after requesting Home and received the
previous list screen, then stopped. Later native logs show the Home command
processed and repeated Home sleep decisions. This health result also remains
**FAIL_OR_INCOMPLETE**; its planned strict timer and quiet checks were not reached.

A separate, subsequent passive USB inventory observed both boards absent for
75.219 seconds across 198 samples, with no port opens or hardware jobs. This
supports a later quiet interval. It is not an uninterrupted continuation of the
media or health capture and does not establish USB-free electrical sleep.

| Record | Result and scope |
|---|---|
| [Original media controller](/Users/MattTaylor/halo-wake-audit-20260928/bench-private/release-runs/wake224-media-20260928-01/RESULT.json) | Incomplete final calendar parser; exact delivery/delete, fresh empty inventories, zero retry state and native sleep retained. |
| [Independent cloud custody](/Users/MattTaylor/halo-wake-audit-20260928/bench-private/release224-cloud001/RESULT.json) | PASS for exact image custody and completed voice job; no semantic qualification. |
| [Original short health controller](/Users/MattTaylor/halo-wake-audit-20260928/bench-private/release-runs/wake224-health-20260928-01/RESULT.json) | Incomplete Home observation race; fresh identity and settled list/refresh retained. |
| [Subsequent passive interval](/Users/MattTaylor/halo-wake-audit-20260928/bench-private/release-runs/wake224-passive-quiet-20260928-01/RESULT.json) | PASS for later 75.219-second USB absence only. |

## Publication

The canonical paired publisher promoted LCD and Sense production latest to 224.
All ten publisher child processes closed successfully. Complete public latest
and versioned manifest checks and both complete binary readbacks passed with
the exact hashes and lengths above. Publication makes 224 available to eligible
devices; it does not establish fleet installation or force an immediate wake.

Receipts: [promotion](/Users/MattTaylor/halo-wake-audit-20260928/release224-publication/production-promote001/result.json),
[complete public readback](/Users/MattTaylor/halo-wake-audit-20260928/release224-publication/PUBLIC-READBACK.json),
[local source tag](/Users/MattTaylor/halo-wake-audit-20260928/release224-publication/SOURCE-TAG.json),
[remote source tag](/Users/MattTaylor/halo-wake-audit-20260928/release224-publication/SOURCE-TAG-REMOTE.json),
[composed device acceptance](/Users/MattTaylor/halo-wake-audit-20260928/DEVICE-ACCEPTANCE-224.json).

Early or drifting paired timer wakes are a separate unresolved issue; this
release changes retry bookkeeping, not clock calibration. Normal next-2-a.m.
arming does not prove a new scheduled 2-a.m. execution. No full
product, fleet installation, continuous resource, or physical gesture pass is
claimed. The observed pair and finite media case do not establish repeated or
fleet-wide reliability.

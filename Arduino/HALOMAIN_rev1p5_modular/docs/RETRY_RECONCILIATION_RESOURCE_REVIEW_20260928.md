# Retry reconciliation resource review — September 28, 2026

Status: source review and validation plan, not a new-build or hardware pass. The
release implementation and evidence are being prepared separately. This review
changes no firmware, calibration, resource floor or existing artifact.

## Scope and dependency boundary

Use the isolated `01c90cb70ecba06bc77fb505a939a999015926d5` lineage: production211
runtime plus the existing212 OTA-screen copy and release/resource reporting
tools. Add only these three runtime changes:

- `Sense_Minimal/sense_upload_persist.h`:220's
  `upload_persist_blank_probe_result()`, `upload_persist_partition_is_erased()`
  and failed-mount branch of `upload_persist_setup()`.
- `Sense_Minimal/sense_voice_spool.h`:221's optional post-delete inventory in
  `sense_voice_spool_delete()`.
- `Sense_Minimal/sense_image_spool.h`:the equivalent independently tested image
  post-delete inventory in `sense_image_spool_delete()`; this is absent from222.

The211 entry already includes `esp_ota_ops.h`, which supplies the partition API;
it already has owner guards, durable `media_retry_inventory()`, UART leases,
foreground flags, probed owner/device inventory, and zero-second peer-arm
cancellation. LCD211 supports both typed inventory requests and probe echoes
(`lcd_voice_spool.h:170–205`, `lcd_image_spool.h:186–221`). No dependency on218's
retry cadence,221's account/cache protocol or222's startup discovery is needed.
Keep211's policy and timer behavior. Do not cherry-pick the whole221 commit.

The canonical checkout was at `e9ad3605b9aa345798ec471929d841ded6e706d4`, with
uncommitted LCD voltage work in `lcd_uart_task.h`, `lcd_voltage_sense.h`, its test,
catalog entry and diagnostic document. This work is separate from this release.

## Allocation, lifetime and failure review

| Change | New explicit storage / lifetime | Overlap and failure behavior |
|---|---|---|
| Erased-flash proof | One256-byte automatic buffer, setup-owned; scalar locals and compiler/SDK call frames additional. No new application heap allocator, task, client, DMA or PSRAM request in this helper. | Setup invokes persistence before operation/upload task creation in211 (`Sense_Minimal.ino:4071,4106,4114`). Read only the selected unencrypted SPIFFS partition, at most1MiB, entirely0xff, within500ms checked before and after each read. Yield every4KiB. Individual SDK calls are not preempted by this elapsed-time limit. Every uncertain outcome retains the hint and bytes. |
| Voice post-delete | One17-byte automatic probe array and scalar locals; reuses the existing JSON document and four-second lease. | Runs in the existing upload worker after backend acceptance and durable deletion, while the original payload remains allocated (`Sense_Minimal.ino:2098–2123`). Successful inventory may allocate more JSON storage than the shorter delete acknowledgment. Unknown/error/foreground/owner-change results retain pending state; confirmed deletion still returns success. |
| Image post-delete | Same17-byte explicit probe array, existing document and lease. | Same worker; payload remains live until after this call (`Sense_Minimal.ino:2383–2398`). Voice and image branches do not execute concurrently in that worker, so the two probe arrays are not simply added as simultaneous demand. Retain image hint on uncertainty or takeover. |

**ArduinoJson qualification:** the linked library is7.2.0. Its
`compatibility.hpp:63` makes `StaticJsonDocument<N>` inherit heap-backed
`JsonDocument`; `N=512` is neither a hard allocation bound nor a512-byte stack
pool. Do not claim that these changes cost only17 bytes or have zero heap effect.
The extra request/response uses the existing document, existing UART frame limit
and parser. A normal inventory reply contains owner/device/probe, count and
summary fields, plus optional next request/epoch; it does not include the full
media metadata. The existing LCD storage/list path is exercised once more after
a delete, with its existing independent storage budget. Sense's four-second
deadline does not preempt a blocked LCD SD operation. Validate actual elapsed
time, takeover behavior and cleanup on both boards.

No persistent buffer, new task, task-stack size, camera reserve, allocator
placement, storage schema or media data mutation is introduced by the intended
three-file scope. Hint reconciliation can cause an existing NVS persistence
operation; failed persistence may repeat discovery after reboot and must not
erase media. Preserve these failures in tests.

## Exact artifact baseline and stack limits

Production baseline is build `6.4.211-20260922T194351Z-731e4897c845`, from
`/Users/MattTaylor/halo-confirmation211-20260922/build/{sense,lcd}/artifacts/verified.json`.
The pinned link-memory section inputs and artifact sizes were read and verified
against their recorded hashes for this review:

| Board | DRAM sections | IRAM sections | RTC fast / slow | App / OTA slot | Margin |
|---|---:|---:|---:|---:|---:|
| Sense211 |159524B|81152B|136 /2528B|1873408 /1966080B|92672B|
| LCD211 |204356B|85248B|136 /268B|2033040 /2621440B|588400B|

The exact new deltas remain pending paired canonical builds. Keep the existing
64KiB OTA-margin review floor and review any DRAM/IRAM/RTC change; do not infer
the delta from source buffer arithmetic. Copy changes alter flash text, not the
ownership model.

211's emitted frames are setup1584B and upload worker1968B; the worker has a
12288-byte allocated stack. These are individual compiler frames, not measured
remaining stack or whole-call-chain bounds.220 emitted setup1776B and221 emitted
voice-delete240B/worker2016B, but those builds contain other changes and optimizer
inlining differences: their deltas cannot substitute for the new build's frames.
The new setup and upload worker paths require exact compiler-frame comparison
and available runtime stack/heap observations. Historical220 main-task minimum
6060B and221 saved-voice success qualify those cases only. Upload-worker runtime
high-water remains unknown; do not invent a safe margin.

## External resource guard without changing calibration

The current guard's calibration is Sense213/LCD211, source
`202802d5e4d3a70c1fe271b46eb813ae1b92e3a6`. Its reviewed source lock later tracks222.
On September28, a read-only `check(root=<isolated firmware root>)` invocation
returned `REVIEW_REQUIRED` for expected source drift, `artifacts=NOT_CHECKED`,
and `HASH_AND_VALUES_VERIFIED` for the checked-in observations. No result files,
lock updates or altered calibration were produced by that check. Rerun after
the three-file implementation and final snapshot exist.

Sense211 still requests2584B plus allocator overhead for the unused queue/mutex
removed in213. Therefore the213 measured dynamic headroom cannot be silently
credited to211. In particular, a passing no-additional-allocation simulation is
not a qualified211 measurement. Keep the source mismatch and this coverage gap;
use fresh affected-path samples and the211 artifact baseline. Do not refresh
the lock or copy later measurements merely to obtain a green result.

The current CLI has no `--root`. Invoke its existing Python API externally with
an explicit source root, while retaining the original model and lock. The
following is a plan to run after building; replace the candidate and fresh
output paths with the actual release workspace. The output parent must exist.

```python
import json, sys
from pathlib import Path
tool_root = Path('/Users/MattTaylor/halo-camera-recovery-2026-09-15/Arduino/HALOMAIN_rev1p5_modular')
sys.path.insert(0, str(tool_root / 'tools'))
import resource_guard as guard
candidate = Path('/absolute/path/to/new-immutable-candidate')
source = candidate / 'snapshot/source'
out = guard.artifacts.reserve_output(
    Path('/absolute/path/to/new-resource-results'), [tool_root, candidate])
result = guard.check(
    root=source,
    proof_paths={board: candidate / 'build' / board / 'artifacts/verified.json'
                 for board in ('sense', 'lcd')},
    out=out)
(out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
(out / 'REPORT.md').write_text(guard.markdown(result))
print(result['status'])
```

Retain `REVIEW_REQUIRED` for the expected lineage mismatch. A genuine artifact
budget failure is a separate failure, not something this document overrides.
The three LCD scenarios also lack internal/DMA measurements. Neither a modeled
pass nor this review is full-product or production acceptance.

Generate an ordinary static resource report from the exact211 proof pair into
a new external output directory, then generate the candidate's report with
`--compare <211-report>/REPORT.json`. This separates actual211-to-candidate
artifact deltas from the guard's213 calibration. Use the current
`tools/resource_report.py`; its arguments are `--sense-proof`, `--lcd-proof`,
`--out`, and optionally `--compare`.

Tool/model fingerprints at review:

```text
resource_guard.py f36a182cb68ee8c5f75de2c46f16b29283216f1f6babdc356a030eb0ef418e35
resource_report.py c0068aecd76bcc3e471631ab5eb1bdb73adda1e186e7fdf8957167395f64f73f
baseline.json de46b17a5a6737571953abd957af9631364155f7f00cbb90e57b02ee4ce12cf7
observations.json 67f3fe595a57d67b2b00f40cf83d4eb43d0717390ebea6c903fd332be1987d29
sources.lock.json 710bf96f6d32cc24ccfd36d41e46bf959ff3e571b081f1ae12c0527792fd8b67
```

## Required finite validation

1. Import the raw-partition and real-retry-policy test harness hunks with the
   erased-flash actual-header tests. Adapt their218-policy `backoff=5` fixture
   to211's maximum `backoff=3`, retaining the21600-second expectation. Run a
   negative control on the immutable211 header. Cover partial/nonblank reads,
   every uncertainty, independent store bits, failed NVS durability/reboot and
   successful mounted recording replay; no format/write/delete during probing.
2. Run the actual voice and image delete functions against empty/nonempty and
   malformed/lost/wrong owner/device/probe responses, owner changes, lease and
   deadline failures, foreground takeover and allocation/parser failures.
   Assert original deadline/lease release, no cursor-relative query, no false
   clear, other-store preservation and successful-delete result preservation.
3. Complete the full working-source and immutable-snapshot host gates, paired
   canonical builds, exact artifact verification,211-relative static reports
   and external guard review. Record exact new compiler frames and remaining
   unknown runtime stack coverage.
4. On the private exact candidate, prove erased-flash hint reconciliation without
   formatting; normal cold boot/handshake; one forced-offline saved voice and one
   saved image, followed by native dark retry, exact cloud acknowledgment,
   durable deletion and fresh post-delete inventory. Finish with zero media
   hints/peer arm and normal maintenance sleep. Observe beyond the obsolete
  60-second post-progress retry deadline without host wake commands. Include
   one remaining-item or failed-inventory case that keeps retries, and one
   foreground takeover. Check resource/latency samples, a normal list/camera
   action and fresh sleep/wake; preserve every failed attempt.

This scope removes proven stale bookkeeping. It does not promise elimination of
legitimate media retries, all timer drift, old-version behavior or every wake.
The source choice deliberately leaves the separately unqualified213–222 runtime
work for its own acceptance: resource changes, list UI/haptics, faster cadence,
owner transaction/cache migration and account-discovery recovery.

Provenance: canonical and immutable211/220/221/222 sources and receipts; shared
`engineering/halo-resource-testing` (September24 historical resource guidance)
recalled before review. Current local evidence takes precedence for current
candidate state. No company-brain write or hardware/cloud action was performed.

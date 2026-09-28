# RAM qualification candidate on published 224

September 28, 2026. Production remains paired **6.4.224**, runtime source
`518691c8d3bd609272b438ec31b82c4de1b6b053`; this branch starts from documentation
descendant `88641024daa765b5197d54dc8ee232a00cd88296`. This is a private, unbuilt
qualification candidate. Version 225 was freshly inventoried and reserved by
the campaign; no version header, installed device or production route is changed
by this source commit. Full host gates, canonical paired builds and physical
RAM/stack measurements remain pending. See [224 acceptance limits](RELEASE_224.md).

## Exact scope

Apply only the previously implemented resource changes from private 213–215:

| Source | Runtime scope |
| --- | --- |
| `202802d5e4d3a70c1fe271b46eb813ae1b92e3a6` | Remove the unused Sense UI event queue and microphone mutex. Operation/upload queues are unchanged. |
| `d8fe385eb73fe43f54ecb18ec4e5fc50a4bbafab` | Destroy four presign request JSON documents before transport, retaining the owning serialized String and existing request/response contracts. |
| `9711348c94e31ac4d682585344e1d4ddb2e545c1` | Apply the existing task-owned, nonblocking TLS PSRAM preference to each voice attempt; add PSRAM phase samples and qualified voice allocator counters. |

Only six Sense runtime files change. No private 216–222 account, LCD list,
retry-timing or haptics change is included. Battery WIP in the other checkout
is untouched. The existing TLS allocator implementation is unchanged. The
published 224 retry runtime files are byte-identical to the starting source:

| File | SHA256 |
| --- | --- |
| `Sense_Minimal/sense_image_spool.h` | `235d8c974b758a52c91f009a1b906c16ab4d6d25aecd817bd0630038be97473b` |
| `Sense_Minimal/sense_upload_persist.h` | `7e3158fcd271df86eb4af2b0bf660705c384ed20e117a9f3b4bf005940ce3a87` |
| `Sense_Minimal/sense_voice_spool.h` | `12f737098ae84b97cd248ef54e322c3d8cb1bdfe390610ad2cb0de29589ad1ec` |

The current catalog retains all 119 existing suites and adds the presign lifetime
and voice TLS suites, for 121 suites before separate host portability work.
The 215 allocator-aware network harness changes and SDK-boundary doubles in
the shared diagnostic/image/claim tests accompany the runtime changes. The
historically calibrated resource guard is not a runtime dependency and is not
imported or silently recalibrated here.

## Upload-worker stack observation

The only additional runtime behavior is a small diagnostic closing a known
measurement gap. At startup the upload worker binds its current task handle,
before claim or media dispatch. Shared Trace cleanup emits one `WORKER_STACK`
row after client cleanup, with owner, job/attempt (or claim timestamp/generation),
task identity and qualification. Only the bound current worker reads
`uxTaskGetStackHighWaterMark(nullptr)`; unbound or foreign callers report
`qualified=0` without a numeric watermark. No stack is moved or resized.

The pinned Arduino ESP32 3.3.8 / ESP-IDF 5.5.4 SDK's
`esp32s3-libs/3.3.8/include/freertos/FreeRTOS-Kernel/include/freertos/task.h`,
lines 1495–1511, explicitly defines the result as **minimum unused bytes since
task creation**, unlike standard FreeRTOS word units. No multiplication is used.
Its SHA256 is `8b908b964d2c87377838762478253eec04312b985e6bb680bacd3f03ab253897`.
The pinned Xtensa `portmacro.h` defines `StackType_t` as `uint8_t`; SHA256
`80ff24959d8a900b7896d4fd3991344a2e100f3b8aacfbc787fe31df7bef8f77`.

The reported value includes earlier work on that task. It is not a resettable
per-attempt minimum, instantaneous free stack, proof of a whole-call-chain bound,
or a watermark for the loop/operation/audio tasks. Pair every sample with exact
boot/build evidence. A missing/truncated row remains unknown. Claim, image and
voice share this trace; presign/camera transitions do not gain new heap traces.

## Resource review

- Allocation and placement: 213 removed 2,584 requested internal bytes plus
  allocator overhead. JSON scopes shorten overlap without introducing buffers.
  Actual ArduinoJson 7 allocation depends on content; compatibility capacities
  are not fixed pools. Voice uses existing external 8-bit preference and the
  existing default `calloc` fallback, which is not guaranteed internal-only.
- Ownership: declaration order is Scope, Trace, TLS client, HTTP client. Reverse
  destruction retires clients before logging and scope release. No payload copy,
  job identity, retry deadline, storage acknowledgment or cancellation rule changes.
- Diagnostic cost: PSRAM fields add 56 bytes to the shared seven-point table.
  Worker binding adds one atomic task-handle object (4-byte target pointer), with
  no dynamic allocation. Stack/frame alignment, helper calls, printing and actual
  linked storage must be measured in the new canonical build. Historical 215's
  +64-byte voice/worker/claim compiler frames do not predict this candidate.
- Failure/responsiveness: unavailable PSRAM, external allocation failure,
  foreign/nested allocator ownership and user cancellation retain existing
  behavior. The new watermark scans once per completed trace and prints after
  client cleanup; it does not add work to the allocation-failure hook or a wait
  to foreground input. Its actual timing/stack overhead remains unmeasured.
- Static/build evidence: pending. Use exact 224 and candidate paired proofs for
  DRAM, IRAM, RTC, binary and OTA-slot deltas. Do not add overlapping internal/DMA
  heap views or credit historical measured savings as guaranteed new headroom.
- Runtime evidence: pending. Historical 213 before-attempt samples improved
  2,644–3,068 B; historical 215 matched voice networking samples improved
  8,628–8,632 B, but its interruption-image sample was 6,244 B lower. These were
  different finite workloads, not controlled predictions for 224 plus this patch.
  Original response-timeout and telemetry failures remain recorded.

## Focused host evidence

Campaign root: `/Users/MattTaylor/halo-ram-campaign-20260928/`.
`focused01/`, `focused02/` and `focused-final/` retain command logs and structured results.
`focused-final/RESULT.json` pins the final firmware/test source hashes for the
diagnostic, voice, image and claim reruns plus the three voice negative controls.

- Frozen-baseline preflight passed on the starting 224 descendant.
- Presign: 241 actual-function lifetime/contract checks pass. Retained private
  213 is the negative control: the same 241 checks fail exactly 30 HTTP-boundary
  lifetime assertions while retaining payload/error contract behavior.
- Voice TLS: 45 cases pass, including missing PSRAM/default fallback, injected
  external failure, foreign/nested ownership, retries, cancellation, parked
  resume and durable receipt/retirement. Three declaration-order negative
  controls fail at their expected lifetime assertions.
- Diagnostic: 88 checks pass, including unbound/foreign task rejection, exact
  byte values and identity, claim key names, hook isolation, owner lifetime,
  concurrency/loss and actual PUT declaration ordering. The target primitive
  probe passes; linked production atomic dispatch still needs artifact review.
- Whole PUT integration: 103 cases / 1,756 assertions pass; claim integration:
  1,175 checks pass; fresh upload priority: 561 checks pass. Shared client cleanup
  assertions now cover the worker watermark output too.
- Media retry/corner suites and the retained 224 erased-VoiceFlash,
  image/voice post-delete inventory and voice persistence suites pass.
- The first fresh-priority invocation omitted its required output argument and
  exited before running tests. Its log remains in `focused01`; the corrected
  invocation passes in `focused02`. This was a test invocation error.

These tests double RTOS, heaps, sockets and storage boundaries. They establish
host behavior, not physical RAM margin, real response latency or acceptance.

## Remaining gates and finite measurement plan

After separate host-path preparation, run the complete working-tree gate on
unchanged source. Then commit/review final source, materialize the reserved
private version, run the snapshot's complete gate, and build/check both canonical
artifacts. Keep before/after resource reports and actual compiler frame metadata.
Do not claim stack qualification from a 12,288-byte task allocation.

On exact installed candidate identities, measure successful, failed, cancelled
and resumed voice, image PUT and claim with worker watermarks; matched internal
8-bit free, DMA free/largest, PSRAM free/largest, allocator qualification and
failure completeness; final reserve ownership, custody and natural sleep.
Prioritize full-length voice immediately after fresh provisioning, repeated
maximum voice and bounded three-then-five captures with pending uploads. Retain
latency/semantic failures even when durable recovery later succeeds. Any wider
OTA or storage-fault case remains separately scoped and authorized.

Shared source read: `engineering/halo-resource-testing`, revision
`59409ca6-1b35-498a-9981-298565d95014`, dated September 23–24. Its production 211
wording is historical. Local 224 source/release evidence controls the current
baseline. No shared-memory save, device action or production publication is
performed by this source step.

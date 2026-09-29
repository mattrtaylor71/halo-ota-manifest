# Voice acknowledgment while browsing the list — September 28, 2026

Status: private 230 is installed on both bench boards, app1 / SDK VALID, with
229/app0 preserved. Compiled source is
`13a0060ad672a1178158887206032e5031b28bf0`; build
`6.4.230-20260929T054206Z-13a0060ad672`. The complete host gate, paired builds,
two finite device cases and scoped cloud readback are complete. Overall resource
reports retain missing/interleaved telemetry gaps. Public OTA remains 224; this
document does not authorize publication or version reuse. See the
[dated functional report](FUNCTIONAL_230_20260928.md) for exact evidence and limits.

## Observed cause

The [229 campaign](FUNCTIONAL_229_20260928.md) captured a fresh 102,400-byte
recording while the list was open. The first POST ended with HTTP −11 at
1790656238.397; the cloud accepted that same request at 1790656238.743 and
completed one job. The native saved retry later obtained acknowledgment and
deleted the recording. The original timeout remains a failed first attempt,
even though durable recovery worked. Similar failures occurred on 227.

`voice_upload_and_parse()` shortened both Stream and HTTP response timeouts to
1,500 ms in list mode. That is distinct from the existing 12-second cooperative
workflow budget. It can abandon a successful request while budget remains,
leaving an unnecessary saved copy and later retry wake.

The correction uses `media_voice_list_remaining_ms()` for those two response
timeouts. Normal fresh uploads and saved retries retain their existing timeout
values. The list attempt count remains one. No radio reset, additional retry,
new task, request identity, queue ordering or receipt acceptance rule is added.

## Foreground priority and SDK boundaries

The pinned ESP32 Arduino 3.3.8 `HTTPClient::handleHeaderResponse()` polls the
client's `connected()` and `available()` methods, sleeping 10 ms between empty
polls. `SenseMediaRetryClient` checks urgent input and the original scope's
deadline at those boundaries. Partial header reads also call its overridden
`read()`; cancellation closes the socket from its owning worker and sets the
Stream timeout to zero. Compatible list scrolling/refreshing does not restart
the deadline; urgent capture still cancels and parks the exact request.

In this SDK, `Stream::_timeout` and `NetworkClient::_timeout` are **different
members**. `setTimeout()` is inherited from Stream; changing it and HTTPClient's
response timer does not increase NetworkClient's socket timeout. The existing
connect/handshake limits and 512-byte write chunks remain unchanged.

DNS, TCP, TLS handshake and an individual SDK write are synchronous and cannot
be safely interrupted from another task. The 12-second budget is checked at
cooperative boundaries; it is not a proved hard wall-clock bound across those
SDK calls. This change does not claim instantaneous takeover inside them.

## Resource and custody review

- Sense only; LCD behavior and OTA implementation are unchanged.
- No additional buffers, tasks, queue slots, persistent records or global
  state. Existing audio payload and task-owned PSRAM-preferred TLS scope remain.
- The same serialized HTTP lease and camera DMA-reserve ownership span the
  request. A healthy delayed response may keep those objects alive longer than
  the former 1.5-second idle response wait, within the existing cooperative
  workflow budget. The changed-path runtime observations and remaining telemetry
  gaps are recorded in the 230 functional report; no complete resource pass is claimed.
- Urgent input closes the owned transport and parks the original descriptor;
  timeout/failure retains durable custody under the existing worker rules.
  Only the validated identity-bound HTTP 202 acknowledgment retires the media.
- Compatible refreshes remain coalesced until transport cleanup. No parallel
  list TLS request, OTA path change, backlight change or retry-schedule change.
- Prior 229 resource observations remain historical measurements. No model
  floors, calibration or source locks are relaxed to claim a new pass.

## Completed finite validation and retained limits

1. The delayed-response regression passes with remaining-budget waits, and its
   negative control fails with the former 1.5-second setting. Host coverage
   includes interrupted/partial replies, exhausted/consumed budget and rollover.
2. Existing request identity, custody, foreground cancellation, refresh
   coalescing and scoped TLS allocation coverage remains in the full host gate.
3. Private 230 was built from committed source using the Mini's locked canonical
   paired builder. Actual static resources were compared with 229.
4. The exact commissioned pair completed one ordinary voice/list/refresh case
   and one urgent capture interruption/resume case, each with natural sleep.
   Cloud custody was reviewed separately. Queues and bookkeeping were not cleared
   to force a successful result.

Host mocks do not establish RF timing or physical user response. Device
acceptance, public release and electrical battery calibration remain separate.

Initial focused checks passed: 16,312 assertions across 91 voice/list scenarios,
561 fresh-upload priority checks and 45 scoped TLS-memory cases. The expanded
test extracts the pinned SDK's actual Stream read loops and HTTP idle-timeout
branch; full HTTP parsing, TLS, scheduling and storage remain doubles. The
former 1.5-second setting has a named negative control requiring failure at
`delayed_response_accepted`. The Mini subsequently passed all 128 exact-snapshot
suites and all three named negative controls. Both canonical builds and artifact
checks passed; static DRAM/IRAM and the measured compiler frames are unchanged
from 229. The MacBook run stalled in an earlier intentional-abort fixture and
remains incomplete, not a passing result.

On private 230, the first voice/list/refresh case obtained its first POST 202 in
4.797 s. The separate Check-in takeover case deliberately cancelled/parked the
voice, then resumed it to 202 and delivered the image with PUT 200. Each ended in
native paired sleep and 75 s quiet. Scoped cloud readback confirms one completed
worker for each exact voice session and a uniquely correlated completed image.
The ambient recordings did not exercise named-item list addition. Original
controller receipts remain unchanged; later cloud evidence is separate.

The first voice window has complete resource phases above the unchanged floors.
Both overall strict resource reports remain `INCOMPLETE_OR_REVIEW_REQUIRED`
because LCD/Sense rows were interleaved or missing; later successes do not repair
those gaps. All hardware workers/descriptors are closed and the lease is free.
No provisioning, physical-gesture or new OTA-transfer qualification is added.
Evidence root: `/Users/MattTaylor/halo-voice-response-20260928/`.

Company context consulted: `engineering/halo-resource-testing` (September
23–24). Current repository source, pinned SDK and dated 229 receipts govern this
investigation. No shared-brain write was requested or made.

# Voice acknowledgment while browsing the list — September 28, 2026

Status: source correction under validation. The last installed bench pair is
private 229; public OTA remains 224. This document does not authorize publication
or reuse version 229 for changed bytes.

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
  workflow budget. Therefore changed-path runtime measurements are still needed.
- Urgent input closes the owned transport and parks the original descriptor;
  timeout/failure retains durable custody under the existing worker rules.
  Only the validated identity-bound HTTP 202 acknowledgment retires the media.
- Compatible refreshes remain coalesced until transport cleanup. No parallel
  list TLS request, OTA path change, backlight change or retry-schedule change.
- Prior 229 resource observations remain historical measurements. No model
  floors, calibration or source locks are relaxed to claim a new pass.

## Finite validation plan

1. Delayed-response regression must fail with the former 1.5-second setting and
   pass with remaining-budget waits. Cover interrupted/partial replies,
   deadline exhaustion, previously consumed budget and millisecond rollover.
2. Retain actual request identity, custody, foreground cancellation, refresh
   coalescing and scoped TLS allocation tests; run the complete host gate.
3. Compile an unused private version from committed source using the Mini's
   locked canonical paired build. Compare actual static resources with 229.
4. On the exact commissioned pair: one ordinary voice/list/refresh attempt,
   an urgent capture interruption/resume case, and natural dark sleep. Preserve
   failures and confirm exact cloud custody separately. Do not clear queues or
   bookkeeping to force a successful result.

Host mocks do not establish RF timing or physical user response. Device
acceptance, public release and electrical battery calibration remain separate.

Initial focused checks passed: 16,312 assertions across 91 voice/list scenarios,
561 fresh-upload priority checks and 45 scoped TLS-memory cases. The expanded
test extracts the pinned SDK's actual Stream read loops and HTTP idle-timeout
branch; full HTTP parsing, TLS, scheduling and storage remain doubles. The
former 1.5-second setting has a named negative control requiring failure at
`delayed_response_accepted`. All negative controls still require Mini validation:
the MacBook run stalled in the earlier intentional-abort fixture and is retained
as incomplete, not a passing result. Evidence root:
`/Users/MattTaylor/halo-voice-response-20260928/`.

Company context consulted: `engineering/halo-resource-testing` (September
23–24). Current repository source, pinned SDK and dated 229 receipts govern this
investigation. No shared-brain write was requested or made.

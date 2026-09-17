# Wi-Fi and saved-media recovery candidate 170

This change starts from committed private169 (`98e26ef`). It is a development
candidate, not a publication or a claim of installed-device qualification.
Keep169 available for rollback and preserve the frozen158 release.

## Completed candidate validation — September16 Pacific

Artifact source is `bf2e5e2cf6a9c219404f29b948ff7e4a377864ec`, firmware tree
`d7d70fcd613b2462cc0d3e0b5e252b45fba24191`, build
`6.4.170-20260917T001602Z-bf2e5e2cf6a9`.

All40 explicit host suites passed against the unchanged canonical snapshot.
These execute production policy, timer, NVS, UART/ACK, storage/replay, Wi-Fi,
network cancellation, user-input, display, sleep, provisioning and OTA code
with stated host boundaries. They include dropped/stale replies, failed NVS
writes, byte-identical retransmission, SD cancellation, retained originals,
no positive backlight/panel enable on a media boot, and passive aborts staying
dark. The frozen baseline ancestry/retained-byte guard also passed.

Both production builds passed with pinned ESP32 SDK3.3.8 and shipping flags;
the independent artifact checker and publisher's local shipping-proof checks
passed. Compiler process groups were reaped and closed. Partition bytes match169.

| Board | Application bytes | Slot bytes | Static RAM change vs169 | RTC change |
| --- | ---: | ---: | ---: | ---: |
| Sense | 1,846,368 | 1,966,080 | +280 | 0 |
| LCD | 2,028,160 | 2,621,440 | +32 | +40 |

These are compiler/linker measurements, not runtime heap/stack qualification.
The Sense upload worker still has its existing12,288-byte allocation. Its
compiler frame grows1,328→1,760 bytes; `uploads_held_for_session` grows32→576
because of the FIFO descriptor peek. Image replay has a newly separately
emitted1,760-byte frame; that is not a measured whole-call-chain delta. Matching
LCD frames do not grow. Physical stack high-water remains part of qualification.
Actual receipts live under
`/Users/MattTaylor/halo-wifi-recovery-2026-09-16/device170/candidate170-001`:

- `RELEASE-PAIR.json`: SHA256 `998bf8ebb384516d4de0af227c909026c0263ac438d62b164e430758c283d4fa`.
- `host-final001/RESULT.json`: SHA256 `850a9602271cf5e48a01cd88e9c1951eb2a6409472f70dbb55d52c6b62db3d26`.
- `build/artifact-result-v2-sense-lcd.json`: SHA256 `823e11fa1e5d0198528d79e96695da295be701d0eafac68d60fcbbcee77abbe7`.
- `SOURCE-SCOPE.json`, `STACK-REVIEW.json`, snapshot materialization, full test
  logs, actual BIN/ELF/map and compiler/SDK receipts are retained alongside them.

No170 installation or publication has run. The local application-only tools
are bound to that exact pair (`device170/tools/BINDING170.json`). They preserve
LCD169/app0 while targeting LCDapp1, then preserve Sense169/app1 while targeting
Senseapp0. Never run a previous version's slot assumptions on this unit.

A fresh non-resetting LCD capture at00:08UTC confirmed169/app0 SDK VALID and
continued real provisioning. Both read-only capture descriptors closed. Real
Wi-Fi setup in the Trepo app is the remaining prerequisite for physical media
qualification; no credentials, SD files or OTA accounting were reset. See
`/Users/MattTaylor/halo-wifi-recovery-2026-09-16/STATUS170.json` for closure and
the next finite device cases. All physical tests below remain pending.

## Behavior

- The Wi-Fi event callback uses a bounded mailbox. The owner loop handles its
  diagnostics; callbacks never reset the radio or cancel a newer connection.
  Ordinary connection timeouts now wait from the failure, not the previous
  attempt's start. Background reconnect continues with a capped cooldown.
- Offline or not-yet-time-synchronized queued captures go directly to existing
  durable backup instead of repeating a lengthy Wi-Fi connection budget for
  every capture. Association and time synchronization remain owner-loop work.
- Voice retries a transient HTTP408/429/5xx once with the same frozen request
  identity and payload. These server responses do not reset the radio. Explicit
  TLS handshake/connect limits replace SDK defaults; PUT recovery waits respect
  the remaining phase budget. This is not a hard wall-clock guarantee for every
  SDK read/write call.
- A compact Sense NVS hint records which stores may still contain work. It is
  scheduling metadata, never proof of custody. Confirmed SD commits, uncertain
  SD transactions, and the internal voice fallback retain the hint. Only a
  successful request-bound inventory may clear an SD store. A timeout, malformed
  reply or partial/failed inventory cannot claim that storage is empty.
- Both boards receive an independent relative media retry timer. First retry is
  5 minutes after sleep, then 15 minutes, 1 hour and 6 hours on continued failure.
  Progress shortens the next retry to 1 minute to drain remaining work. Empty
  stores stop these additional wakes. Intervals do not require SNTP; actual cloud
  replay still requires fresh time for TLS/request-age checks.
- The LCD wakes five seconds before Sense's fallback media timer, stays dark,
  and uses the existing wake line to start Sense. The LCD's media arm is separate
  from its existing OTA arm. Earlier OTA/safety timers win, including ties. A
  media timer cannot supply an OTA schedule identity or allowance. Real OTA debt
  and due schedule admission continue through their existing policy.
- One typed SD item may be queued per wake. Voice/photo priority alternates and
  the within-store cursor advances after a queued attempt, including a failed
  upload; the original record stays intact. The separate one-slot internal voice
  fallback keeps its own bounded attempt and waits for fresh time before using
  that attempt. Its failure cannot consume the SD voice turn.
- A previously durable FIFO head can upload once foreground work is idle.
  Newly captured RAM jobs retain the established camera-safe sleep-flush policy.
- Retry boots initialize the panel/backlight dark. A real touch or encoder
  gesture reveals the normal UI, ends the receiver wait, cancels an SD replay
  and pauses saved-media work for the rest of that wake. An SD save is allowed
  to finish because it may be the only durable copy. The user can still make
  fresh captures, which keep the existing safe sleep-flush behavior.
- The saved upload's own worker checks user cancellation between I/O calls and
  512-byte writes. It closes its own socket and leaves the original saved record
  intact; other tasks never close an active TLS client. Saved transport failures
  defer without resetting Wi-Fi. Backoff waits poll cancellation every20ms.
- Image “Logged!” now follows successful queue admission. Voice errors use a
  stable job identity, allowing a matching failure through the acknowledgment
  animation without displacing a newer action.

## Custody and retained limits

Request IDs, owner/device binding, payload CRC/SHA, commit-before-ACK storage,
cloud receipt checks, and receipt-before-deletion rules are unchanged. Media
is not deleted merely because an attempt failed. Corrupt, wrong-owner and
expired records remain held; after seven days they are not silently replayed as
fresh commands. Finite storage, loss of power before durable backup, and the
five-minute guardian remain limitations. Queue admission alone is RAM custody,
not a durable-save receipt. Missing/unbound old voice errors remain diagnostic
instead of replacing an unrelated current screen.

UI wake does not wait for network cleanup. Network cancellation is not an
instantaneous interrupt of the SDK: an already-running connection includes DNS,
an up-to8s TCP phase and an up-to8s TLS handshake. A currently executing SDK
write also reaches its safe return before cancellation; progress-based timeouts
are not a whole-operation wall-clock bound. Physical touch-to-action latency
under these faults must be measured before claiming immediate capture service.

Internal retry hints survive a reset. The LCD relative arm is RTC state: a full
power cycle loses that arm and ordinary paired startup rediscovers/rearms from
the stores. No SD/NVS erase, credential reset, OTA allowance reset, legacy spool
enablement, schedule change or fleet publication is part of this change.

The durable image backend was last recorded as test-owner-only. Fleet rollout
still needs its own verified backend admission/IAM expansion; source changes do
not establish cloud rollout state.

## Required validation

Run focused executable host tests for Wi-Fi callbacks/backoff, HTTP retry
classification and identity, media policy, both timer adapters, ACK loss/stale
replies, media custody, sleep/worker ownership, provisioning and OTA UI. Build
the canonical paired production profile from clean committed source, retain the
artifact checker receipts, then validate on the actual unit:

1. Online voice and photo still complete with exact cloud custody evidence.
2. Disconnect Wi-Fi with the bounded diagnostic command; capture voice/photo,
   verify durable local receipts and ordinary sleep.
3. Leave the unit untouched: verify actual timer wake, dark LCD, no unintended
   OTA/checking screen or allowance change, exact request replay and retirement.
4. Repeat a failed retry and later successful recovery; check pending hints,
   fair selection, bounded backoff, and no extra media timer after empty proof.
5. Confirm a real existing OTA timer/obligation is not consumed or moved by a
   media wake; verify ordinary touch, provisioning and coordinated sleep.
6. Tap/turn during SD replay and HTTP upload, including a stalled response:
   verify immediate visible menu, queued user intent, cancelled background
   activity, retained original, and recovery on a later autonomous wake.

Host doubles do not establish weak-RF/fridge performance, physical SD power-loss
resilience or USB-free qualification. Record each physical case and its limits
separately. Do not label a helper-driven tap as an autonomous retry wake.

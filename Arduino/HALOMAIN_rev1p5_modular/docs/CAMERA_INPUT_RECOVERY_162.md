# Camera request recovery candidate, September 15

This candidate repairs the reproduced pre-sleep input starvation on Sense158. It starts from the reviewed159 production runtime, retaining the158 OTA fixes and159 LCD wake handling. The separately preserved, uninstalled voice161 candidate is not included. Build162 is a candidate until exact artifact and physical acceptance receipts are recorded; published160 and frozen158 remain unchanged.

## Reproduced failure

Four actual camera captures succeeded. A subsequent Discard request arrived while Sense was flushing offline uploads before sleep. All five LCD transmissions went unacknowledged. Heartbeat/PONG resumed roughly47seconds later without a reboot. The45second upload wait did not pump UART. The old photo-backup READY reader could also discard unrelated command lines. This demonstrates a request-service defect, not a captured camera-driver hang.

Original evidence: `/Users/MattTaylor/halo-device-analytics-2026-09-10/camera-repro-20260915/FINDINGS.json` and its raw captures. The user's first reported acquisition was not captured, so attribution of that exact earlier incident remains qualified.

## Changes

- Pump UART at safe boundaries during upload cleanup, including the100ms wait loop. A new validated, deduplicated user action cancels cleanup before teardown. Duplicated transmissions are re-ACKed without repeated cancellation; passive link/status traffic does not advance the cancellation generation.
- Keep the temporary upload gate scoped to cleanup. Preserve the existing upload budgets, guardian behavior, queue ownership and sleep admission checks.
- Replace the private READY reader with a typed, matching-job mailbox and shared raw-RX ownership. Ordinary complete lines remain queued, and application callbacks run outside the raw-RX lock on the main task. Preserve fragmented post-wake frames and complete queued commands across SYNC.
- Coordinate photo and LCD-query transport admission. An accepted binary transfer suppresses ordinary JSON. No OTA policy, quota, schedule, credentials, partition, UI, or camera-driver change is intended.
- Relay `sleep/flush_start`, `sleep/flush_cancel`, and `sleep/flush_end` through LCD diagnostics, including actual queue/budget/elapsed values.

## Validation and limits

The focused host tests execute actual production source with explicit hardware/RTOS doubles. They cover offline pending uploads, cancellation, duplicate/passive input, custody boundaries, unchanged deadlines, guardian sleep, fragmented messages, READY refusal/malformed/wrong-job/timeout, RX contention and LCD query ownership. Negative controls reproduce the old starvation and dropped-command behavior. These tests do not establish physical timing or Wi-Fi recovery.

The existing backup rescue still uses its bounded READY retries. During an unresolved READY handshake, ordinary commands remain queued because the peer may already be in binary mode; dispatching actions while their ACK/UI replies are suppressed would lose visible outcomes. If READY never arrives, main-owned rescue dispatches preserved input after that photo's existing custody decision, while worker-owned rescue allows dispatch after request closure. Delivery within the LCD's2second ACK window is not guaranteed in these cases. A positive binary transfer is also noninterruptible until its safe boundary. Do not report these cases as prompt-ACK passes.

The legacy LCD photo SD fallback remains disabled. This patch does not make offline photos durable, fix RF reception, or claim every underlying camera-driver call is bounded. No fake successful upload or new durable-storage qualification should be inferred from UI completion.

## Finite physical acceptance

1. Verify fresh exact candidate Sense identity and SDK VALID through LCD, with the actual LCD159 or162 identity. Preserve native OTA admission/debt and both existing firmware banks.
2. With a deliberately unavailable network, perform Discard, Check-in and Dish captures; record each request ACK, acquisition, options and terminal UI independently of upload success.
3. At forwarded `flush_start`, request a new capture. Require timely ACK, `flush_cancel`, actual camera completion and no teardown during the action. Repeat twice.
4. Leave cleanup untouched once; verify its recorded original budget, terminal behavior, natural sleep and actuator wake. Stop on a crash or exhausted request rather than stacking more requests.
5. Restore the original network and require a genuine successful upload/cloud record for a fresh capture. A UI DONE alone is insufficient.

Sense USB is inaccessible in the enclosure. Installation requires restored Wi-Fi and genuine OTA eligibility, or temporary Sense USB access for a guarded application-only service. LCD USB forwards diagnostic/application commands but cannot install a Sense image. Record any assisted installation separately from OTA acceptance.

Build, test and service-tool evidence workspace: `/Users/MattTaylor/halo-device-analytics-2026-09-10/camera-fix162-20260915`. No physical acceptance is claimed by this source document.

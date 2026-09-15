# Camera request recovery162: scoped acceptance, September15

This release repairs the reproduced pre-sleep input starvation on Sense158. It starts from the reviewed159 production runtime, retaining the158 OTA fixes and159 LCD wake handling. The separately preserved, uninstalled voice161 candidate is not included. Build162 is published and installed on both boards with SDK VALID; its scoped camera/input/upload-recovery acceptance passed. Frozen158 remains the recovery and previous scheduled qualification reference.

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

The user provided temporary access to both USB connectors for guarded application-only service. This completed installation without changing native OTA allowance/debt. LCD USB forwards diagnostic/application commands but cannot install a Sense image; when the unit is assembled, that access limit still applies. Record USB service separately from OTA acceptance.

Build, test and service-tool evidence workspace: `/Users/MattTaylor/halo-device-analytics-2026-09-10/camera-fix162-20260915`. The final scoped physical results are recorded below; the broader limitations remain.

## Built candidate checkpoint

Source `b78efd38ed3ffc795a9fab086a04302c89f0c922`, firmware tree `7b2586bee5f75b7cf1a5c94798ad60d8f8d5dd4a`, build `6.4.162-20260915T182022Z-b78efd38ed3f`. Both canonical builds and actual artifact checks passed. Sealed pair SHA256 `52314e60a74b1ec4a44906d05294679bcc0580ed557fcaf0cee7f9ed2fa5034e`.17 regression groups and3 expected-failure controls passed; the old flush fails the compiled new-user starvation assertion. Source review and all runtime hashes are pinned in the evidence workspace.

Sense image/link size grows3952bytes and static RAM112bytes; RTC is unchanged. LCD footprints are unchanged. Per-function compiler stack metadata is retained separately; it is not a physical high-water measurement. Both exact images were staged and public latest pointers promoted to162 on September15 at18:35UTC, with full manifest and binary readbacks. Publication receipts are pinned in `PUBLISHED.json`; the immutable build-time `RELEASE-PAIR.json` retains its original checkpoint fields. The later USB installation checkpoint is recorded below; it does not change these immutable artifact/source identities.

The user confirmed the Wi-Fi trigger: Halo was provisioned for the home network while physically in the garage. Two subsequent actuator wakes confirmed live old-firmware diagnostics with Wi-Fi disconnected. Garage reprovisioning was requested so genuine OTA installation and on-device fault acceptance can proceed. Do not label these old-firmware connectivity observations as candidate tests.

## Earlier September15 garage provisioning and blocked native attempts

Garage reprovisioning restored actual Sense Wi-Fi connectivity, confirmed through LCD-forwarded RSSI/IP diagnostics and a fresh cloud boot report. Three native Manual Update requests reached Sense and ended with terminal OTA_UNLOCK before any image transfer. An earlier software attempt occurred under the initial boot lock and never queued a native request; it is not counted.

The fresh cloud policy reports DISCOVERY with two discovery windows used, no target, no BEGIN/apply and no reserved work. Production manual discovery still enforces the two-window cap, consistent with the refusal. The exact manual terminal result string was not captured: USB output interleaved, and requested UI layout dumps executed after the terminal overlay expired. Do not substitute the background `not_due` report for that missing manual result.

At this earlier checkpoint, all six observation captures were closed and the installed pair was Sense158/LCD159. No allowance/debt reset, schedule change, firmware-bank service or image transfer had occurred during those attempts. Installation was blocked pending an eligible native window or temporary Sense USB access. Preserve `INSTALLATION-OBSERVATION.json` (SHA256 `71938c7a0e254f9c6a3ae1bc3355a0d8223deeb742d68dc12aa7fdaa3d03f3e4`) and the individual capture closures as dated history; this is no longer the current installation state.


## Installed checkpoint: September15 19:42:01UTC

Both boards now run exact `6.4.162-20260915T182022Z-b78efd38ed3f`: Sense/app1 and LCD/app0, each selected for boot and SDK VALID. The user supplied both USB connections. Scoped application service preserved all verified NVS and partition tables, kept the selected Sense158 and LCD159 fallback banks, and archived both complete banks on each serviced board before changing the inactive image and alternate selector. No quota/debt reset or OTA pass is claimed.

`INSTALLED.json` SHA256 `395782cfefd007bccf72a43fa3a5e8ef368ce5011a98667bbe2fb4b5f5e3ca19` joins the closed Sense service receipt `edddb9ca4496033911f25ca5af6cc35cc8815a2c34067b8c61239c8715ed18fc`, closed LCD service receipt `b8f7cb6842d922f6b3b5512cd8a866e5aba62a58521aa8822986427432754cdb`, and the later paired live-health trace `bd1f271e7d103ee04bb1400c63c2641b1d240a08fe8fbdc3c51966239c995a56`. The service receipts themselves stop at release; the separate live trace supplies SDK-health evidence. All installation owners were reaped.

Zero candidate camera actions had run at this immutable installation checkpoint; camera/flush acceptance was pending then. Installation and SDK VALID alone did not qualify those behaviors. The later acceptance below supersedes that pending status without changing `INSTALLED.json`. Frozen158 and the separately preserved voice161 candidate remain unchanged.

## Final scoped acceptance

`ACCEPTANCE.json` SHA256 `f6a7bc58d8e6bace35632ae201ff656eab008cdf1c2f00427a85c73aa33af52f` records `PASS_SCOPED_CAMERA_RECOVERY_162`, completed September15 at19:56:43UTC. Six actual captures completed: two Discard, two Check-in and two Dish. Two requests interrupted populated upload cleanup: observed request-to-ACK times were62.57/73.377ms and request-to-`flush_cancel` times93.042/133.462ms. Six exact stored image objects were independently verified by S3 HEAD. A fresh actuator wake established exact paired SDK VALID identity, and the final Dish case ended in normal coordinated sleep. All hardware owners are reaped; both boards were left asleep.

The long interruption case selected the unchanged LCD denial-ceiling sleep fallback while Sense drained; only the separate fresh Dish case qualifies normal coordinated sleep. One interrupted camera acquisition took3070ms, exceeding the nominal3000ms target. Commands used the normal LCD USB application path, with USB connected; the actuator exercised wake. No USB-free, power-loss, RF-reception or other camera-driver failure-mode qualification is claimed. Offline photo SD fallback remains disabled. No quota/debt reset, allowance-service implementation, new OTA transfer or full-product qualification occurred.

Use the exact162 artifact source and reviewed descendants for future work, retaining the158 OTA and159 wake fixes. `RELEASE_BASELINE.json.current_working_source` selects this checkout; `current_baseline` continues to pin the frozen158 recovery artifacts.

# Offline media stress 167 — current checkpoint

Current result: both boards run the sealed167 build with SDK VALID. The September16 actuator reset restored the test fixture. Sense-only service completed, followed by six device episodes: baseline inventory, one10.5s voice/Dish/Discard offline burst, three replay wakes and final inventory. All3 payloads saved, reached exact cloud custody and retired from SD. One recoverable LCD sleep-handshake failure remains. No publication or full shipping acceptance. Historical checkpoints below are retained; the current closure at the end supersedes their pending-installation wording.

## What the physical stress tests found

| Candidate | Observed outcome | Evidence boundary |
| --- | --- | --- |
| 165 | The worker retained sole capture ownership beyond the 45 s flush window, but LCD exhausted its three 25 s sleep waits and slept during the third SD transfer. Voice and Dish saved and later reached cloud custody. Discard job 61 reported `saved=0` for 117,605 bytes during bounded recovery. | Two saved captures do not qualify the three-capture burst. The lost Discard was not recovered by a later empty queue. |
| 166 | The new media sleep guard held LCD awake throughout the 320,000-byte voice save. Binary traffic did not refresh peer liveness; JSON silence caused UNKNOWN, then ASLEEP. Immediately after voice commit released custody, LCD took the `sense_asleep -> local sleep` path. | Voice request `f264a12205dab21c14013fc2fed66e5d` has an exact durable-save receipt. Dish started next and Discard had been queued, but their terminal outcomes are absent from the capture. Preserve both image outcomes as gaps. |
| 166 recovery | The retained voice reached cloud custody and its worker completed once. LCD slept before the deletion request; Sense reported `sd_delete_pending` / `original_slot_kept`. | Cloud delivery is proven for this voice; SD retirement is not. The recovery controller failed paired readiness, so this episode is not a passing health/replay run. |

The actuator was recovered and both 165 and 166 were installed and tested. Repository wording that still names actuator failure as the current 165 blocker is stale. Preserve that historical blocker separately rather than overwriting the subsequent failed stress outcomes.

166 timing, UTC September 16: LCD active-custody defer at 03:48:28.854596; its BEGIN log at 03:48:28.893366; stale JSON diagnosis at 03:48:58.841926; missed-PONG ASLEEP at 03:49:05.346716; voice commit at 03:49:05.561328; local-sleep decision at 03:49:05.563501; deep sleep at 03:49:05.714498. Later cloud acceptance was 03:52:19.379 and worker completion 03:52:22.055; SD deletion remained pending at 03:52:23.842253.

## Intended 167 correction

Runtime commit `143fc11706a345fd35d6c7f3a7540f5de0e2c65f` treats accepted typed media frames, exact replay acknowledgments and bound cleanup as peer liveness before releasing transfer custody. Missed-PONG and stale-RX state decisions commit under the same existing mutex, so an older main-loop decision cannot overwrite newer binary proof. Clock reads and logging remain outside that mutex. Invalid or expired frames do not supply proof. Media deadlines, user-activity timeout, OTA policy, UI and wire schemas are unchanged.

Commit `3955e6c` changes only the two host transport fixtures to recognize retained same-media cleanup ownership after the existing 90 s active-custody retirement; the frozen-source conditional returns false where that state does not exist. It does not change device behavior. Bind the actual sealed 167 build to its full source commit rather than assuming a draft output directory is selected.

Retain the 164 directory-enumeration fix, 165 worker-custody fix and 166 active-transfer sleep guard. The existing **7,363 host stress cases** are 2,755 storage cases plus 4,608 UART/custody cases; they are not physical device runs. Additional overlapping focused assertions must not be added to that count as independent scenarios.

## Finite acceptance still required

1. Verify the exact sealed 167 pair installed and both boards selected/running SDKVALID. Capture a typed pre-burst inventory and recover the previously accepted 166 voice through its normal replay/delete acknowledgment before adding new work.
2. Run the same bounded diagnostic disconnect and long voice, Dish, Discard sequence. Require three distinct exact request identities, all three durable saves, no rescue/loss markers, and continued custody through the 45 s flush boundary and inter-transfer gaps. A runner counting three `saved=1` lines alone is insufficient.
3. On subsequent normal wakes, correlate each original request and payload length/hash to cloud custody, worker state and its device delivery/deletion evidence. No assumed transcription or shopping-list semantic effect.
4. Capture final typed inventories, live UI, fresh paired SDKVALID and explicit paired sleep. Record fallback or guardian events separately from clean handshake success. All capture owners must close and be reaped.

Pre-burst 166 health proved voice pending/incomplete/corrupt 0/0/0 and image 0/1/0. The image incomplete count of one was already present after the failed 165 save. Pending 0 with incomplete 1 means committed deliverables empty with held residue, not clean SD and not recovery of the lost photo. Matching counts alone do not prove the same incomplete filename. Do not erase evidence to obtain an empty result.

Use the reviewed `tools/summarize_media_regression_r2.py --version 6.4.167`. Its 29 host controls pass. It corrects the original summarizer's ordering requirement: the observed 38.77 ms reversal is possible because admission sets custody before printing BEGIN. The original summarizer and raw timestamps are preserved; no admission instant is inferred.

Public OTA was last verified at 162; obtain fresh manifest evidence before documenting its current state. This draft authorizes no publication. Preserve frozen 158 baseline values and all previous candidate receipts. Existing repository `last_verified_device_health` must not be relabeled as 165/166/167 without the actual corresponding closed evidence.

This is a finite USB-connected diagnostic-disconnect regression. Physical RF/enclosure behavior, USB-free operation, arbitrary power cuts, guardian loss of RAM-only work, full-capacity LIST latency, new OTA transfers and all product effects remain outside this acceptance.

## Closed evidence references

- [INSTALLATION165-AND-HEALTH-REVIEW.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/INSTALLATION165-AND-HEALTH-REVIEW.json) — SHA-256 `42d57b5e5214aa94e9f02e274f1e162792871a06ad82dc4022fb941463f287e9`.
- [hardware165-burst001/CUSTODY-OBSERVATIONS.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/hardware165-burst001/CUSTODY-OBSERVATIONS.json) — SHA-256 `394b9737471cc0c74eb98cc3067ecf5e6f31b6b2f8a0aeadfaf2c52600ebfac9`.
- [hardware165-recovery001/SOURCE-REVIEW.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/hardware165-recovery001/SOURCE-REVIEW.json) — SHA-256 `0590195da4fb6a183418f4d33f12e2e9d94981deb86a6a8731d499a5527dde0e`.
- [hardware165-final001/RESULT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/hardware165-final001/RESULT.json) — SHA-256 `b4378a9f4a01ac973fd1bfe02d764242d8be5a7d37b2d150e0708d2eca424087`.
- [cloud-burst165-001/voice/RESULT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/cloud-burst165-001/voice/RESULT.json) — SHA-256 `97c4e41abddde4983edc397e53ffe7c41fdf32b5b4148197c242931d2b8220b5`.
- [cloud-burst165-001/dish/RESULT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/cloud-burst165-001/dish/RESULT.json) — SHA-256 `a04b08bb659c25c9c5a2515ea1d4496e942636b5cebb9f41e96f8375284f05bb`.
- [INSTALLATION166-AND-HEALTH-REVIEW.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/INSTALLATION166-AND-HEALTH-REVIEW.json) — SHA-256 `57087416bfe661f75ac531d5ec420997ce18ed42ec0b1461e15bef7beb13cf7e`.
- [hardware166-burst001/RESULT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/hardware166-burst001/RESULT.json) — SHA-256 `bd922bce0a81c883ed5081489fc383e4fe49209afa457d71bee9189e33cd8356`.
- [hardware166-recovery001/RESULT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/hardware166-recovery001/RESULT.json) — SHA-256 `f86590c2016fcd24e5b8e5f1c7bb18498582a99ef08be2beab10b10f65571056`.
- [cloud-burst166-001/voice/RESULT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/cloud-burst166-001/voice/RESULT.json) — SHA-256 `7fc2a8ae2634783f9477b97d61e8e78969d6685ea8ee0e611f62978b8614dfb3`.
- [r5-passive-tail-review001/RESULT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/r5-passive-tail-review001/RESULT.json) — SHA-256 `57ebde675ad8129cceb4099b4ef6d6f908613f073edf18d064bae6181ce633c7`.

## Exact 167 continuation

- Pair: `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/candidate167-002/RELEASE-PAIR.json` (SHA256 `ffe0f6e7a72ee1121ee7b5a91781834e90b6eeb241ac01045250aa3c895d4972`).
- LCD completed service: `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/lcd167-service001/install/result.json`. Do not repeat this completed write.
- Sense failed-before-install attempt: `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/sense167-service001/RUNNER-RESULT.json`. Start a new output directory after actuator response is restored; retain failed evidence.
- Use `tools/install167_timed_identity.py --board sense`, exact167-002 pair/hash, and the completed LCD result. It obtains a new mixed-pair identity before writing.
- Capture with `run_hardware_r5.py` and `CAPTURE-BINDING.167.json`; use `tools/summarize_media_regression_r2.py --version 6.4.167` for reviewed defer-log correlation. Original summarizer is retained.
- Both target binaries fit existing slots; static RAM/RTC deltas are 0. LCD binary grows 752 bytes versus 166; Sense size is unchanged. Compiler source-frame totals are not measured hardware stack headroom.
- Candidate167-001 was never installed: its newly added test stub rejected legitimate late cleanup.3955e6c fixes only two fixtures;167-002 reran all 17 suites successfully. Preserve001 partial hostlogs and follow-up receipt.

Closed partial installation review: [INSTALLATION167-PARTIAL-REVIEW.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/INSTALLATION167-PARTIAL-REVIEW.json). The LCD readback and unchanged NVS/table bytes are verified; no Sense flash operation occurred. All captured service/recovery processes are closed or confirmed absent. [STRESS167-CHECKPOINT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/STRESS167-CHECKPOINT.json) records the remaining actuator dependency and continuation.

## September 16 remote wake continuation

A fresh, bounded signature-only actuator probe timed out without a bootloader response. One diagnostic `PUSH:500,200,500` command was written, but there was no actuator reply or Halo USB appearance; serial write success does not prove a physical tap. Both probe owners and the parallel passive capture were closed and reaped. No actuator or Halo firmware was written in this remote-wake attempt.

The last LCD sleep log armed its timer for approximately 01:59:46 Pacific. This is an expected wake, not an observed one. A sole bounded USB observer was started (owner PID 24567, exec session 48042), with a one-shot thread follow-up at 01:57 Pacific and a 02:10 Pacific deadline on September 16. This startup record is now historical: both observed wakes are closed and the follow-up is paused; see the episode result below. Read [RTC-HANDOFF.md](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/remote-wake167-001/RTC-HANDOFF.md) and the live `rtc-capture001/STATE.json` before hardware access. The immutable [wake checkpoint](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/remote-wake167-001/WAKE-CHECKPOINT.json) records startup; the live state and eventual RESULT control ownership thereafter. Do not open a second LCD descriptor or run the actuator alongside this observer.

The observer sends health reads only; it does not automatically tap, reset, flash or request OTA. A future Sense service must first close/reap the observer and obtain fresh mixed-pair identity with all existing idle, SDK and artifact checks. The ordinary timed installer requires the actuator, so its wake stage cannot be blindly reused while that controller is unresponsive. LCD service is already complete; do not repeat it. Continue the finite physical regression only after a valid Sense167 installation and fresh paired health. If no safe handoff is possible by 02:10, close the episode and report the remaining physical blocker.

## RTC episode closed: September 16, 02:01 Pacific

[EPISODE-RESULT.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/remote-wake167-001/EPISODE-RESULT.json) records two observed wakes and closed capture owners. The second LCD wake occurred at01:59:46 Pacific with timer-origin metadata; both USB identities were present at02:00:09.979. LCD167/app0 and Sense166/app0 reported SDK VALID. OTA coordination unlocked at02:00:06.032, inactivity sleep began at02:00:14.981, and SLEEP_READY led to LCD deep sleep at02:00:20.859. The next LCD timer was86365 seconds. These logs establish wake and coordinated sleep, not a complete manifest verdict or new paired OTA transfer.

The host missed the roughly9second idle interval while checking inventory and the service handoff. No HOME, WAKE, actuator command, installer, firmware write, media stress run or publication occurred in this follow-up. This is a missed host service window; it is not evidence that OTA failed. Sense167 service and its regression remain outstanding. The prepared no-actuator identity exporter must still receive fresh, closed, idle proof before the existing Sense-only installer can execute. Never reuse this sleeping capture as fresh service authorization.

The first watcher's repeated FWINFO query unexpectedly invoked the firmware's Sense wake pulse; its observation is diagnostic-assisted. Root created a separate second watcher polling local ID1/UI only and preserved the original scripts and captures. The second capture reported an empty voice list, but no deletion acknowledgment tied to the retained166 request; exact retirement remains unproven. Both watcher/child pairs are absent and both campaign locks were verified free in [CLOSURE.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/remote-wake167-001/CLOSURE.json). The one-shot automation is paused.

## September16 actuator-reset continuation — six device episodes closed

[STRESS167-CLOSURE.json](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/STRESS167-CLOSURE.json) is the current acceptance boundary. Sense167 installation verified both bank backups, target readback, protected NVS/partition bytes and release; LCD was not reflashed. Both boards then reported exact167 build and SDK VALID.

The offline burst saved voice320000bytes, Dish118156bytes and Discard118074bytes. LCD stayed awake across the45second flush boundary and inter-transfer gaps that failed165/166. Three subsequent wakes uploaded all three original identities; exact cloud records and device deletion acknowledgments correlate. Final typed inventories are voice0/0/0 and image0/1/0 (pending/incomplete/corrupt), matching the baseline counts. The old incomplete image is retained evidence; filename identity and historical166 voice deletion are not inferred. Voice worker completed in1attempt; image jobs areDONE. Product semantics and duplicate-request retries were not tested.

The burst did not have a clean sleep handshake: LCD dropped a SLEEP_READY suffix, retained a five-byte partial JSON prefix, retried3times, and used fallback sleep about77seconds after Sense slept. Its15second fallback wake recovered; all five other episodes showed normal paired sleep. UART task diagnostics continued throughout, so this was not a demonstrated permanent hang. Synchronous LCD maintenance-window NVS persistence overlapped Sense's next frames; exact byte-loss cause needs confirmation. [The sleep review](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/BURST167-SLEEP-HANDSHAKE-REVIEW.json) separates evidence from hypothesis. Do not mark the full campaign flawless from the custody-component PASS.

All captures/installers are closed and both campaign locks were verified free. No source runtime edits were made during this continuation. The next sleep-fix candidate must use unused168+, preserve this tested167 pair, and verify delayed/missing/stale ACKs and concurrent user input before repeating the same burst. Disk is below the builder's8GiB default reserve; the user was asked to free roughly10GB. No new build or publication was attempted.

[Next-candidate implementation and test plan](/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-stress-20260915/sleep-ack167-review001/PLAN.md) defines the bounded acknowledgment barrier and the required user-input/timeout checks. It is a plan, not an implemented168 fix.

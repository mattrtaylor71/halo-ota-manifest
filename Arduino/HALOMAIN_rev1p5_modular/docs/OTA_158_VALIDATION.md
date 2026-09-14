# OTA 158: engineering validation handoff

Version **6.4.158** is published and installed on both boards. One scheduled **157→158** update passed through exact image verification, SDK validation, native persistent-state settlement and natural sleep. The production schedule is restored to **2 a.m. Pacific**. This document records that evidence and a repeatable next-test procedure; it does not claim that every failure mode is qualified.

Use local release tag **`halo-v6.4.158`**, targeting artifact source `b6d06da5997e2252a3472697f30dc8111ab90367`, or reviewed descendants preserving its fixes. The early-calendar correction is commit `df8cab35ef8128bdb3d312bd8c016817ad064a85`. Version 158 has the same runtime and resource footprints as 157; only three generated identity headers and four pinned release records differ. No remote Git push was performed when the tag was created.

[Release baseline](../RELEASE_BASELINE.json) is the machine-readable source for exact artifacts and receipts. [Production acceptance](PRODUCTION_RELEASE_ACCEPTANCE.md) preserves the separate historical version 117 full-product qualification. The 158 result qualifies this OTA case, not a fresh full-product regression.

## What actually happened on September 14

Times below are **PDT (UTC−7)**, taken from captured event timestamps, not review-file creation times. The intended Sense calendar time was 09:10:36; the LCD was armed 15 seconds earlier. Admission during this existing lead window is expected.

| Time | Observed event |
|---|---|
| Before the case | Controlled USB installation put both boards on 157/app1, SDK VALID. Setup changed only the Sense timezone; LCD NVS, legitimate history and existing accounting were preserved. |
| 09:09:06.039 | Original passive capture started, before either scheduled timer. |
| 09:10:19.909–22.438 | LCD reported TIMER wake and the exact stored origin epoch `1789402221`; Sense recorded `reason=lcd_timer` and correlated the live peer identity. Host receipt time and device clock are distinct measurements. |
| 09:10:24.474–25.078 | Fresh SNTP was confirmed; Sense admitted `allow=1 manual=0 why=lcd_timer`. |
| 09:10:28.296–34.773 | LCD BEGIN for 158; rendering drained in **22 ms** within the 500 ms limit. One session was accepted from offset zero. |
| 09:14:27.243–34.801 | All **1,891,872 LCD bytes** matched SHA-256; LCD selected app0, rebooted, marked it valid and answered a fresh correlated SDK VALID query. |
| 09:15:23.196–24.660 | Sense completed its exact-image hash/write path, selected app0 and rebooted into 158. Its first boot report correctly showed PENDING_VERIFY rather than prematurely claiming VALID. |
| 09:15:35.658–36.063 | Native `observed_pair` resolution with unchanged credit, verified `done_ids`, and coordinator completion were captured. Fresh postcase identity subsequently confirmed both 158/app0 SDK VALID. |
| 09:15:53.556 / 53.750 | Sense and LCD directly logged deep-sleep entry. |
| 09:19:06.011 | Original capture closed naturally after **599.969 seconds**. It issued no UART commands, resets, taps or writes and never reopened either descriptor. |

The automatic episode had **one accepted LCD session, three recovered packet retries, zero terminal transfer failures and zero observed watchdog resets**. These packet retries are not three complete OTA attempts.

A separate postcase read of native NVS proved exact-target **RESOLVED, phase 8/generation 6**, one network window, one daily attempt and zero reserved work. Only then did scoped restoration archive the resolved test policy and retire completed test bookkeeping. Three NVS sectors were rewritten; no firmware bank or selector was written. Both genuine history entries were retained, and verified readback contained no owed work. The boards then naturally slept with Sense timer `1789462800` (**September 15, 02:00 PDT**) and LCD timer `1789462785` (15-second lead, SDK return 0). Bootstrap, audit and restoration were declared interventions outside the automatic episode.

## Faults found and the fixes retained in 158

There was no single proven “USB bug.” USB enumeration and actuator failures complicated observation, and USB-free behavior remains a separate acceptance gap. Neither a successful host download nor a lost USB descriptor proves the device's Wi-Fi or power behavior.

| Evidence | Retained correction or limitation |
|---|---|
| 22:40 target 140: LCD `ui_task` watchdog during transfer; Sense exhausted chunk retries. | 142 bounded LVGL/DMA quiescence before flashing and preserved watchdog feeding. Sense recovery accepts an exactly identified rebooted LCD within the existing cleanup deadline. The original blocked UI call and USB causation were not captured. |
| Midnight target 143: scheduled admission occurred, but fresh-clock verification failed before manifests. | 144 added one qualified secondary SNTP opportunity within the existing readiness deadlines. This addresses the local clock-acquisition path; it does not prove the remote NTP/network cause. |
| 00:45 target 145: unsafe panel wake raced UART BEGIN, with initial refusals and LCD watchdog. Native retry later installed both images. | 147 corrected UART panel-wake ownership; see [LCD receive handling](../LCD_Minimal/lcd_uart_rx.h) and [quiescence tests](../tools/test_lcd_flash_quiescence.py). |
| Both 145 images became VALID, but durable policy remained APPLY while legacy completion/history proceeded. | 149 ordered exact-target durable settlement before legacy completion; see [runtime settlement](../halo_ota_demo/firmware/shared/SenseDurablePolicyRuntime.h) and [postboot tests](../tools/test_postboot_policy_settlement.py). An installed pair alone is insufficient acceptance. |
| 02:15 target 150: three accepted LCD transfers failed after 86,536 / 69,128 / 138,760 bytes; no Sense BEGIN. | 151 added raw Wi-Fi reason diagnostics in [sense_wifi.h](../Sense_Minimal/sense_wifi.h). This was instrumentation, not a demonstrated RF/network repair. A correct host download was only a host control. |
| 07:00 target 156: early calendar cancellation discarded the pending request before work admission. | 157 retained the matching future LCD notice only when the boundary fits strictly within original boot/peer deadlines; cancellation and mailbox disposal share that guard. See the [Sense production entry](../halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino) and [four future-calendar tests](../tools/test_future_calendar_wait.py), within the 13-test focused regression suite. |

The separate 22:00 miss remains a failed observation; do not retrospectively assign the later proven calendar root cause to it.

## Persistent state and retry limits

The [pure durable policy](../halo_ota_demo/firmware/shared/DurableOtaPolicy.h) identifies exact target/peer images, phase, generation, deadlines, daily accounting, reservations and retry-arm identity. [Storage](../halo_ota_demo/firmware/shared/SenseDurablePolicyStorage.h) and runtime checks must prove writes by readback before granting work. Preserve coordinator pending/history, LCD transfer continuation, peer expectations and durable policy together.

Production constants allow 40 minutes of daily work, a 120-second preflight ceiling and a 20-minute Sense apply ceiling, subject to remaining original deadlines. The fast path allows two network/apply opportunities, at most two BEGINs per board per attempt and four per board in that path. A temporary failure can schedule a **300-second** retry inside a **1,800-second** fast lifetime, with the LCD 15 seconds ahead, only after peer arm/storage proof. Exhaustion defers to normal maintenance; the deferred path has stricter one-attempt/two-BEGIN limits. Invalid targets, rollback or repeated exact-validation faults quarantine rather than retry indefinitely.

These are caps, not promised grants. UTC budget-day rollover requires qualified time and native policy conditions; it differs from Pacific wall-clock scheduling. Reboot, another target or another manual tap must not refund consumed/reserved work. Never clear allowances or unresolved debt to manufacture a test pass.

## How to repeat without changing the result being measured

1. Start from the frozen tag. Use [canonical release preparation/build](OTA_POLICY_PRODUCTION_BUILD.md) from clean committed source. Seal both artifacts, version/build identities, flags, partition/UI configuration and full hashes; publish immutable objects before verified latest promotion.
2. Establish fresh paired SDK VALID identity and a closed native-state audit. If debt, quarantine, missing identity or insufficient native capacity exists, stop setup and diagnose it. Preserve the refusal and let a valid native recovery/defer path resolve it.
3. Pin a new case's exact current/target pair, manifest bytes and future calendar due. Reuse reviewed timezone-only test preparation only against its proven clean baseline; record the prior timezone and every changed byte. Do not reset credits/history, restore an old NVS image or erase a failed obligation.
4. Verify actual Sense/LCD timer arithmetic, SDK return and paired natural sleep. Start a single passive observer before the LCD lead. Record epoch-tagged raw offsets, reset/wake causes, SNTP freshness, manifests, BEGIN/ACK offsets, hashes, boot partitions, native settlement and sleep. Capture cloud events as corroboration, not installation proof.
5. On failure, retain all evidence and the native retry/defer decision. If sleep destroys capture handles before a short retry, close and reap that observer before a separate passive retry capture; document the gap. Do not tap or command inside the automatic case.
6. Close the capture, then independently verify exact paired SDK identity and full native state. A pass needs scheduled lineage, exact images, native RESOLVED, no owed work and sleep—not just “100%,” VALID or `done_ids`. Only after success may scoped test bookkeeping be archived/retired and Pacific scheduling restored with verified readback.

Report admission misses, terminal failures, recovered packet retries, full retries, intervention counts, reset causes and trace gaps separately. Do not keep testing without regard to the native energy/erase budget.

## Prior overnight results and remaining gaps

| Scheduled/attempted PDT slot | Target | Closed outcome |
|---|---:|---|
| Sep 13 22:00 | 140 | Missed admission |
| 22:40 diagnostic | 140 | Transfer/watchdog failure |
| Sep 14 00:00 | 143 | Clock failure before download |
| 00:45 and native retry | 145 | Recovered paired installation, unresolved durable policy; not a full pass |
| 02:15 and native retry | 150 | Network transfer failure |
| 03:20 | 152 | Audited pass; late startup capture, four packet retries |
| 04:00 | 153 | Audited pass; two packet retries |
| 05:00 | 154 | Audited pass; LCD post-reboot trace gap, two packet retries |
| 06:00 | 155 | Audited pass; three packet retries |
| 07:00 | 156 | Missed target before work admission |

That campaign remains **four full passes, five failures and four skipped nominal slots** (23:00, 01:00, 02:00, 03:00); the separate recovered 145 installation is not promoted to a pass. This 158 confirmation adds one independent pass.

Still unqualified by this case: USB-free operation, power interruption during either image/settlement, a fresh full UI/action regression, and the **positive early-calendar WAIT branch**. Fresh SNTP in this run was already three seconds beyond the LCD boundary, so normal scheduling success cannot prove that branch. Initial capture prefixes were incomplete despite the captured lineage and both post-update sleep traces. The 18 ERRLOG and one DIAG `verified=0` notices were intentionally disabled legacy writers under durable diagnostics, not attempted NVS-write failures. No new hardware test was run to write this handoff.

## Immutable evidence appendix

All paths below are relative to:

`/Users/MattTaylor/halo-device-analytics-2026-09-10/scheduled158-confirmation-20260914/`

| Receipt | SHA-256 |
|---|---|
| `confirmation001/VERDICT.json` | `9471c0b06e61c2e3dc0c7553162956c2b191e37080f0720821eae4b25903120a` |
| `confirmation001/AUTOMATIC-UPDATE-REVIEW.json` | `0e45b5b81acdf30edddda32c10256e508f879347f43d1e63f954bd6168cf61be` |
| `postcase158-identity001/identity.json` | `90337f905e34b962e2c5d4858ee94286615810f902ef0a4c12296aa0eacce286` |
| `restore001/RESTORE-PREPARATION.json` | `e43fee71cf12ceab31860c2711a01d94bddb217afab0d9a43e946c8d23f94e07` |
| `PRODUCTION-RESTORATION-REVIEW.json` | `3baf3cd5938c93dc1508c30d84133bda12df739719e5237b65812b02bb6b7bd9` |
| `restore001/result.json` | `332c099f428b354dcce4cb6cee50e73fd6020150c51a7548b9b74ae34727bb43` |
| `RELEASE-TAG.json` | `e4d6cb659ed99080c7a547ad4ad65271a95868735202ff4276300336015d1976` |

The automatic review pins the closed capture, byte spans and timestamp mapping; the verdict pins exact published binaries. The baseline retains the previous campaign's individual verdicts. Keep private NVS, credentials and raw logs in their controlled evidence locations; this handoff intentionally contains none of their payloads.

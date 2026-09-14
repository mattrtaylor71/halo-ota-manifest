# Recovery closed; 151-to-152 scheduled acceptance pending

**Last verified paired firmware is 151, SDK VALID; production latest is 152.** Version 151 was explicitly installed by USB, preserving 150 fallback and NVS; a separate fresh identity confirms paired SDK VALID, idle and Home. Both builds use committed production source `550e5039ee30e56dde57a0c6ecc57d425457214a`. The only runtime addition is raw Wi-Fi reason logging, not a network behavior fix. Version 152 changes generated version metadata only; both latest manifests and full binaries have been verified. Continue development from this source and subsequent reviewed commits.

The earlier assisted 150 recovery is now closed with a full no-debt audit: **RESOLVED phase 8, generation 12**, counters still 2 network windows/2 day attempts, 2,225,642 ms remaining and zero reserved work. USB repair and one manual observed-pair resolution preceded the exact fulfilled LCD-due cleanup; native `done_ids` verification, `policy_target_valid` and paired sleep were observed. No allowance was reset. These interventions do not turn the failed scheduled case into a pass; earlier guard refusals remain recorded.

Case05 **151 → 152 was armed for 03:20 PDT on September 14**: Sense epoch 1789381200 and LCD epoch 1789381185, with explicit LCD SDK return 0 and paired natural sleep. Closed setup and independent review prove arming only. Completed production bookkeeping was archived before the declared test reset; no failed debt was erased. The planned pre-due observer was missed during context compaction. Root started passive capture around 03:22, ending at 03:28:30, so the scheduled wake and initial activity are not fully covered. The outcome, postcase firmware state and native settlement remain pending; no success is claimed.

The 02:15 case remains failed: scheduled and native retry wakes worked, but three LCD transfers stopped after 86,536, 69,128 and 138,760 acknowledged bytes because of HTTP disconnect/no-data failures; no Sense update began. No watchdog or render-quiescence failure was observed. Captures retain their incomplete classifications and actual durations of 219.937 and 375.645 seconds. The Mac download is only a host control; the device's disconnect reason and USB causation remain unproven. Campaign totals remain **0 fully settled scheduled passes, 0 clean passes and 1 automatic paired installation**. Exact receipts are bound in `RELEASE_BASELINE.json`; following sections are historical checkpoints.

---

# Recovered paired installation 1; fully settled scheduled passes 0

The 00:45 scheduled case automatically recovered and installed both 145 images; both boards reported SDK VALID and completed native coordinator reporting/sleep. However, Sense NVS captured before the later 147 USB bootstrap still held **DurableOtaPolicy APPLY (phase 3), generation 12**, not RESOLVED. Coordinator history and valid firmware do not prove policy settlement. Correct accounting is **1 recovered paired installation, 0 fully settled scheduled passes, 0 clean passes**. The original and v2 verdicts and prior peer review are preserved; `VERDICT-v3.json` binds the frozen native policy decoding and source review. These NVS captures precede 147 application execution and are not a fresh postboot policy read. No credits or debt are being reset, and 148 must not be staged while settlement remains unresolved.

Source review confirms an ordering gap: legacy coordinator completion can write done IDs and cancel boot work before durable exact-target reconciliation runs. The preserved obligation is for exact 145 images; newer 147 cannot satisfy it. The focused correction must commit and read back durable RESOLVED before legacy completion, retaining existing readiness and policy budgets. Native recovery and this correction remain unvalidated.

147 is committed at `06650a1685c660eaf149bd275625e9bbb9a059b4`, built and explicitly installed by USB, preserving 145 fallback and all NVS. Fresh Home/idle identity separately confirms both 147 SDK VALID; the installer's earlier SDK-not-yet-observed summary is retained. 147 contains only the reviewed UART panel-wake ownership fix relative to 144; host tests and canonical checks passed. 148 is built/checked/sealed from identical 147 source with version metadata only and unchanged resource footprints. It remains unpublished, uninstalled and withheld. **Latest published firmware remains 145.**

The three original failed cases remain unchanged (22:00 admission miss, 22:40 transfer failure, midnight clock failure); 23:00 and 01:00 slots were skipped during repair/validation. 147 scheduled acceptance, native policy settlement, secondary SNTP recovery and the six-hour clock-failure fallback gap remain open. Continue from this 147 production source and preserve historical 117/139/143 evidence, both failed initial capture receipts and closed native-retry evidence. `RELEASE_BASELINE.json` pins actual build/install/identity/NVS references. Earlier sections below are historical checkpoints.

---

# Fixed 142 → 143 scheduled acceptance — midnight armed, outcome pending

The midnight capture is closed: the actual scheduled LCD timer triggered OTA, Wi-Fi connected, but the 15-second network-time attempt timed out before any manifest request. Both boards armed six-hour fallback wakes. No transfer or short automatic retry occurred. A post-window actuator wake and read-only ROM audit verified paired 142 health, absent policy and no partial-transfer debt; those resets are declared interventions. The failing NTP server/packet path and USB causation remain unproven.

Version 144 is the next focused candidate, not yet built or published: one isolated secondary SNTP attempt for the qualified scheduled path, capped at 40 seconds and the original readiness deadlines. Cross-sleep short retry remains outstanding. Manual admission, active-policy budgets, LCD behavior and the existing watchdog/reboot-recovery fixes are preserved.

Fixed source `7eb99f1d194179571b2ba204b2f86f053b600781` is now built as 142/143. Canonical builds, artifact checks and focused host regressions passed. Both boards run 142/app1 SDK VALID after controlled USB installation; Home after actuator wake and natural paired deep sleep were observed. 143 has the same runtime with generated version metadata only, and both latest manifests and complete binaries were verified. **143 on-device acceptance remains pending.** Historical 117 qualification and 139 manual-policy evidence are preserved.

The two focused fixes bound LCD LVGL/DMA quiescence before flash writes while retaining watchdog feeds, and let Sense prove clean recovery from an exactly identified rebooted LCD within the existing 36-second cleanup budget. No allowance refund or watchdog timeout increase. The 22:40 LCD `ui_task` watchdog and unconfirmed cleanup motivated these changes; the exact blocked UI call and a USB cause were not captured. The proposed early-notice cause of the separate 22:00 missed admission remains unconfirmed and unchanged.

The midnight September 14 case is armed for **142 → 143**: Sense epoch 1789369200 (00:00 PDT), LCD epoch 1789369185 (23:59:45 PDT). Only Sense timezone changed; absent policy, original history and LCD NVS were preserved. Native timers and paired sleep were observed. Existing observer 41574/PID 47530 captures 23:58:30–00:08:30; cloud observer 65610 remains the single cloud owner. No further commands, taps, resets, schedule edits or publications belong inside this armed window. Admission was confirmed, but this case closed before download. Exact paired 143 installation and transfer-fix acceptance remain unproven.

Campaign accounting: **0 full scheduled passes; 1 missed admission (22:00); 1 proven transfer failure (22:40); 1 pre-download clock failure (00:00); 1 skipped slot (23:00); 0 native retries.** The failed 140 state was archived before explicit exact 140 USB repair. Native `target_valid resolution=observed_pair credit=unchanged` then resolved phase 8/gen6. Scoped retirement 003 restored original history/Pacific before 142 installation. Two earlier host cleanup attempts refused before any writes (capacity, then computed NVS span). Repair, retirement and 142 bootstrap are not scheduled successes. Final 2 a.m. Pacific/history restoration remains pending after this campaign, using the exact retirement 003/clean142 receipt chain and preserving any new unresolved debt.

Future firmware work must use this fixed production source and subsequent committed records. Later version-only hourly packages use `hourly-scheduled140-20260913/tools/prepare_next_release142.py`; the older helper anchors unchanged 139. Unused 141 remains unpublished. `RELEASE_BASELINE.json` pins the closed build, publication, health, recovery and arming receipts. **All sections below are historical checkpoints, not current publication or device state.**

---

# Hourly scheduled OTA acceptance — closed failed diagnostic

The September 13 campaign has **zero scheduled passes, one missed admission and one actual failed transfer**. The 22:00 target had no observed admission or transfer; its proposed early-notice cancellation remains unconfirmed. In the separate 22:40 timezone-only diagnostic, native LCD TIMER/reset 8 woke Sense through EXT0/reset 8. The automatic path fetched both exact published 140 manifests and started LCD session 32929. After progress was persisted through offset 262172, the LCD `ui_task` watchdog reset it. Sense ended `chunk_retry_exhausted`, preserved the original 140 debt and deferred its own update to avoid a split pair.

The observer is closed and reaped. UART reports DEFERRED phase 6 / generation 5, no short arm, then `policy_not_due`; both boards entered next-day sleep at Sense epoch 1789450800 and LCD epoch 1789450785 under the temporary diagnostic timezone. No full 140 update or fresh complete paired postfailure health qualification is claimed. The capture observes Sense 139 and LCD 139/app1 VALID after the watchdog; the separate pre-diagnostic paired 139 health receipt retains its original scope.

Only two focused fixes are in development: LCD OTA quiescence/watchdog handling and Sense abort recovery using verified LCD reboot evidence. They are not built, installed, published or physically accepted. Existing local141 is an unused, unpublished version-only package of the unchanged139 runtime and must not be treated as the fix. Public latest remains140; source139 remains the verified development baseline. Preserve failed140 debt and all original case/history receipts; closed-case/no-attempt reset helpers do not admit this failure. Pacific 2 a.m. scheduling and legitimate history restoration remain pending through a debt-preserving path.

The [closed diagnostic verdict](/Users/MattTaylor/halo-device-analytics-2026-09-10/hourly-scheduled140-20260913/DIAGNOSTIC-2240-VERDICT.json) and [runbook addendum](/Users/MattTaylor/halo-device-analytics-2026-09-10/hourly-scheduled140-20260913/RUNBOOK-DIAGNOSTIC-CLOSED.md) bind the final evidence and recovery constraints. Original pending-observation records are preserved. Further hourly tests await focused recovery and validation; no extra pass or skipped-slot result is invented.

# Version 139 manual-policy verification — September 13, 2026

| Check | Actual evidence |
|---|---|
| Build/publication/install | Source `97c45f6a20cadff2ae444b99c3928457e7b204e4`; canonical builds, native regressions and public binary verification passed. Both 139/app1 SDK VALID; 138/app0 and all NVS preserved. |
| Actual no-update | Both complete parsed 139 manifest identities match the sealed pair; terminal is `up_to_date`. Cloud confirms two network windows and generation 4. Independent serial review verifies Home/dark/paired shutdown. |
| Budget refusal | Admission 5 (BUDGET), terminal `policy_daily_limit`, no manifest fetch. Independent serial review verifies Home/dark/paired shutdown. No allowance reset in either case. |
| Capture limits | Both original harness results remain INCOMPLETE. First capture lacks a redundant Sense summary line. Second lacks the full ACK prefix and LCD target suffix: stored-arm fields and SDK timers support local arming and an inferred 15-second lead, but not complete ACK delivery to Sense. |
| Telemetry limit | First case corroborated by cloud. Second pre-sleep upload failed (code 0, stage 4, result 9); no second cloud outcome claimed. |
| Scope | Two manual-policy cases verified by separate offline reviews; no new firmware-transfer, scheduled 2am execution, physical navigation, power-cut, USB-free or extended soak qualification. |

Original captures and independent reviews are pinned under `acceptance_results.139_manual_discovery_and_budget_refusal` in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json). All earlier acceptance remains preserved below.

---

# Version 138 sleep-handshake verification — September 13, 2026

| Check | Actual evidence |
|---|---|
| Build/publication | Source `e77c5acf7246d474fada668fbaf38ac59d2a1a16`; both canonical builds, artifact checks, 13 native regressions and full public manifest/binary readbacks passed. |
| Installation | Both 138/app0 SDK VALID; 137/app1 preserved. NVS, bootloader and current-bank writes were zero. |
| Three closed cycles | Each manual-result cycle stored and acknowledged the future schedule, stayed dark through the sleep handshake, and logged paired deep sleep without a relight or reboot. No firmware writes, allowance changes or diagnostic sleep override. |
| Existing policy | Actual terminal remained `policy_deferred` / “Update postponed”. Policy, allowance and schedule logic are unchanged. |
| Limits | Sleep-handshake functional acceptance only; no 138 firmware-transfer, scheduled OTA, physical navigation, power-cut, USB-free or extended soak qualification. |

Receipts are pinned under `acceptance_results.138_sleep_handshake` in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json). Earlier 137, 136 and qualified 117 evidence remains unchanged below.

---

# Version 137 manual UI memory verification — September 13, 2026

| Check | Actual evidence |
|---|---|
| Source/build/publication | Committed source `a8ab44b851b438c57ad7ddda11c392d1e9af8bd3`; both canonical shipping builds, artifact checks and full public manifest/binary readbacks passed. |
| Controlled installation | Both 137/app1 SDK VALID; original-handle nonce/CRC LCD identity and fresh Sense FW_INFO. Version 136/app0 preserved; NVS, bootloader and current-bank writes were zero. |
| Repeated manual UI | Ten requests through the existing Settings action returned Home naturally, with no forced exit. Cloud confirmed the actual `policy_daily_limit` terminal. The stress test changed no allowance. |
| UI memory | After all ten cycles: 98,304-byte pool, 73,204 bytes free and 73,120-byte largest block. No decline between cycles. |
| Same-version no-update | One actual manifest comparison against published 137 returned Sense `up_to_date`, LCD already current and the `up_to_date` terminal, then Home naturally. Free UI memory returned to 73,204 bytes with a 73,120-byte largest block. |
| No-update bench fixture | Archived the completed version 136 policy; one Sense NVS sector changed only its retry bookkeeping after verifying no unresolved debt. Both firmware banks, selectors, other NVS, LCD NVS, schedule and completed history preserved. This is not natural quota renewal. |
| Limits | No full Settings navigation, new firmware transfer, scheduled, power-cut, USB-free, physical cold-boot or extended soak qualification. Normal sleep was not observed in the bounded no-update capture. Version 136's scheduled result and the historical qualified 117 release retain their own scope. |

Receipts are pinned under `acceptance_results.137_manual_UI_memory_recovery` in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json).

---

# Scheduled 136 verification — September 13, 2026

The actual 135→136 scheduled transfer is reported by the passive cloud observer. The distinct checks below must retain their own evidence and timing.

| Check | Current evidence |
|---|---|
| Build/publication | Canonical shipping 136 source/artifacts and full public manifest/binary readbacks passed. |
| Scheduled setup | Native calendar due 18:55:53 UTC; LCD lead 18:55:38 UTC. Both timers logged, LCD timer SDK returned success, setup handles closed. |
| Autonomous transfer | Cloud reports both 136, LCD updated, Sense running/selected app0; one BEGIN per board. No device command or USB open by the observer. |
| Direct SDK health | PASS. Both 136/app0 SDK VALID, exact build and LCD Home/OTA flags clear. Independently extracted complete fresh boot FW_INFO and nonce/CRC ID1 from the original LCD capture; its later duplicate-response timeout remains recorded. |
| Durable settlement | PASS on ordinary verification wake: the same campaign reports `policy_target_valid`, RESOLVED/generation 6 / reserved 0 at 19:11:15 UTC, with one BEGIN per board. No manual OTA request. The subsequent LCD USB health read reset the already-valid LCD; this observation is not a USB-free control. Earlier pre-query APPLY / generation 5 is preserved; completion before this wake is not claimed. |
| Pacific restoration | PASS. Pacific 02:00 restored; both actual timer settings captured, policy/history unchanged. First night uses an uncredited absolute arm. |
| Limits | One accelerated calendar case, connected USB; old 134 failure separately archived. No soak, fault-recovery, power-cut, USB-free or factory qualification. |

Version 117 remains the preserved qualified release; its accepted scope and historical114 results below are unchanged.

---

# Production 117 acceptance — scoped five-fix validation

The actual acceptance receipt is `halo-ui-fixes-2026-09-10/ACCEPTANCE.json`, pinned in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json). Compiled source is `1e624b5e8d2f44f8f1c91cbe455df5f2f8d7a66f`; 116/117 are canonical production pairs with identical runtime outside generated version headers. Production 117 publication passed. The short same-version response follow-up also passed. Release tag `halo-v6.4.117`, commit and completed checkout-adoption bindings are recorded in `RELEASE_BASELINE.json`.

| Check | Actual scope/result |
|---|---|
| Icons/Settings | Existing vector microphone replaces the bitmap background; ASCII separator removes the unsupported glyph. Installed source verified; no new physical pixel photograph claimed. |
| Shopping layout/navigation | Actual LVGL/Nunito geometry within 1 px; on-device real seven-item list selected 0/scroll 0→selected 6/scroll 235→selected 0/scroll 0. |
| Native manual/retry behavior | Real policy/codec, one-request action, ACK replay/deduplication, readable refusal/timeout, and existing retry/wake regressions passed. |
| Controlled 116 installation | Candidate/protected ranges verified; NVS, bootloader and current-bank writes 0; paired 116 SDK VALID observed afterward. |
| Manual 116→117 | Exactly one request through the LCD Settings action using existing USB `ota`; stable sampled update overlay; both full image hashes match 117; both app0 SDK VALID; terminal unlock, Home/OTA flags 0, logged deep sleep and 30 seconds paired USB absence. Capture elapsed 365.78 seconds. |
| Same-version response | One further Settings-action request correctly returned `policy_daily_limit`. Tested source title: “Daily update limit”; detail: “Please try again tomorrow.” Overlay sampled on through 8.444 seconds and off by 8.944 seconds after request, matching an eight-second terminal hold. Fresh paired 117 SDK VALID and LCD nonce/build proof passed; Home/OTA flags 0, coordinated sleep and 15 seconds paired USB absence followed. |
| Limits | USB-connected test. No new scheduled, fault, soak, physical power-cut or USB-free qualification. No physical finger-hit or new screenshot claim. Capture status alone was not promoted to success: The acceptance record combines the actual raw evidence. |

Two local deployment starts stopped safely: a host `BytesIO.name` format error before flash transfer (attempted-write intent retained), then an absent port while asleep. Descriptors/locks closed; the third invocation completed. These are installer conditions, not failed firmware OTA results. Provisioning and retained campaign allowance were not reset.

The 114 evidence below is historical and retains its original scope. Its scheduled/Pacific/package qualification is not relabeled as 117 testing.

---

# Historical production OTA acceptance — 6.4.114

**Accepted and published on September 10, 2026.** Manual and normal-calendar installations reached the same exact production 114 pair. Both boards finished SDK VALID and idle, with Pacific scheduling and paired natural sleep restored. Production latest advanced to 114 at **16:56 PDT**.

The compiled source is commit `1224f28ab2d1307d54cf71956b30da2bbccead3d`, firmware tree `83c178bde2020c4e2fff800caf9e33be33a4ef97`. [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) binds the exact artifacts, acceptance, publication, recovery package and checkout records. Later release-documentation commits do not change the compiled source identity.

## Accepted scope and results

The user directed a one-hour release effort covering a manual 113→114 update and a separate normal-calendar 113→114 update, followed by final health, accounting, Pacific restoration and production publication. Version 113 was a private-route baseline with shipping policy; both updates installed the exact production 114 bytes. Production promotion completed at 16:56 PDT, after the internal 16:52 target and within one hour of the user’s 15:57 instruction; final Git handoff followed. Extended fault and soak testing was deferred.

| Check | Verified result |
|---|---|
| Native readiness regression | PASS. The observed old-source failure was reproduced; the corrected real-source readiness code and canonical codec passed the focused cases. |
| Exact 113 and 114 builds | PASS. Canonical shipping flags, source provenance, hashes, partition fit and resource comparison verified. Version 114 uses production endpoints; bench, one-shot and fault controls are disabled. |
| Portable 114 recovery package | PASS. Exact application images, ELF/maps, bootloaders, partitions, flash arguments and provenance included; package and ZIP contents verified. |
| Controlled manual baseline | PASS. Both boards booted exact 113/app1, SDK VALID, with Home/idle and later paired natural USB absence. Provisioning and the valid 109 fallback were preserved. |
| Manual 113→114 installation | PASS. One supported LCD serial request at 16:20:55.588 PDT was followed by paired natural sleep. Subsequent health confirmed exact 114/app0, SDK VALID and Home/idle on both boards. Physical menu-button coverage is not claimed. |
| Manual durable accounting | PASS before the verification query. The archived native record is RESOLVED, phase 8/generation 6, with admission at 16:21:00 and resolution at 16:25:59. It records reserved 0, one network window, one apply attempt and one begin per board, with 14 ms remaining. The record predates the post-case USB health intervention. |
| Normal-calendar 113→114 installation | PASS. The separate case was scheduled for 16:40:45 PDT. Before verification, telemetry reported exact 114 on both boards with `nightly_20260910` origin. The final query confirmed both boards SDK VALID and Home/idle. |
| Scheduled durable accounting | PASS on the verification wake. The final native record is RESOLVED, phase 8/generation 6, reserved 0, one network window, one apply attempt and one begin per board, with 13 ms remaining. Its 16:50:35 timestamp follows the health intervention; pre-query durable resolution is not proven. |
| Final Pacific service restoration | PASS. Only `halo_prov/tz` changed to `PST8PDT,M3.2.0,M11.1.0`; all 768 canonical policy bytes and unrelated NVS values were preserved. Both boards returned SDK VALID/idle and entered paired natural sleep. Saved timers select September 11 at 02:00 Pacific for Sense, with the normal 15-second LCD lead. |
| Production publication | PASS. The canonical LCD and Sense latest manifests were promoted and verified against the exact 114 artifacts, with previous-pointer protection. Hardware and cloud owners closed. |

The scheduled observer began after the opening wake transition. Before the final query, Sense was naturally asleep and both boards had reported 114, while LCD remained enumerated. The opening transition and that earlier LCD sleep boundary remain unobserved. Final paired natural sleep was verified after Pacific restoration; it does not retroactively fill those gaps.

## Runtime change

The final correction addresses retry cancellation at 12 seconds before due when the separate LCD timer-origin notice is absent. A persisted shipping ARMED record and current correlated peer may wait within the existing 15-second lead interval. The function still returns false before due, preserves the missing-origin diagnostic and retains the original deadlines, target checks, busy guards and accounting.

The native regression covers missing origin, exact-due reservation, no early or duplicate debit, invalid clock/storage/peer state, deadlines and caller busy guards. Earlier transfer-cleanup and retry-arm corrections remain in the compiled source. See the [readiness investigation](OTA_RETRY_NEXT_FIX.md) for the observed failure and source rationale.

## Conditions and limits

The tested unit has an 8 MiB Sense and a physically 16 MiB LCD with the recorded production layouts. Tests used Garage Member Wi-Fi with USB cables connected. Opening a diagnostic port can reset a board, so post-case queries are distinct from autonomous observations. USB disappearance alone is not update-success proof.

The scheduled case used an explicitly recorded temporary POSIX timezone to place normal local 02:00 nearby while retaining genuine UTC. This exercised the calendar path, not an observed Pacific overnight interval. Final Pacific scheduling was restored and its next timer verified.

The two cases used separately authorized, archived fixtures between closed runs. These preserved ownership, Wi-Fi/authentication, unrelated NVS and valid fallback images. No in-case quota refund occurred. Shipping retains its 40-minute daily work allowance, 120-second preflight reservation, network/apply/begin caps and strictly later-UTC-day refill rule.

Extended repeated soak, deliberate integrity faults, controlled network interruption, physical power cuts, USB-free operation and observed later-day recovery are deferred, not passed. Controlled USB setup is not an electrical cold boot. Static RAM/RTC and stack-frame comparisons do not prove worst-case TLS/library/RTOS stack margin. The offline recovery package does not qualify a production flashing station.

## Historical evidence and handoff

The earlier 109→112 scheduled run reached 835,080 LCD bytes before HTTP no-data failure, then 304,405 bytes before a disconnect with Wi-Fi connection loss. Its armed retry woke at due−12 seconds with `origin:false` and cancelled without a second network reservation; the canonical record became DEFERRED generation 8. The readiness rejection is reproduced and corrected in 114. The initial stream-stall cause remains unconfirmed; the evidence does not establish USB or heap as its cause.

Version 112 was not promoted, tagged or adopted. Earlier 102→103 and 105→106 failures and historical bench passes remain historical evidence in [RELEASE_BASELINE.json](../RELEASE_BASELINE.json) and the [changelog](../CHANGELOG.md). They are not reclassified as acceptance of 114.

Use the [release baseline workflow](RELEASE_BASELINE.md) for future source, build, tag and checkout work, and the [factory and recovery guide](PRODUCTION_FACTORY_RECOVERY.md) for the exact portable package and preservation rules. Release/tag/adoption receipts are recorded separately from artifact source identity. The recurring soak automation remains paused; no unrelated remote Git push is part of this release.

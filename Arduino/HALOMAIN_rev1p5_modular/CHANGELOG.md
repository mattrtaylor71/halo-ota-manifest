## Unreleased — offline photo and voice backup integration

- Integrate corrected voice SD custody onto accepted162 and add a distinct checked photo queue with frozen request identity; preserve current OTA/camera/wake fixes.
- Preserve UART input while transferring saved media; use full-length typed replies and verified backend custody before retirement.
- Add bounded direct-USB offline diagnostics for device testing. Candidate only: build, installation and physical acceptance are not yet complete. See `docs/OFFLINE_MEDIA_BACKUP.md`.

# September 15: camera input recovery162 accepted

Fix Sense input starvation during upload cleanup before sleep and preserve unrelated commands during photo-backup READY waits. Retain the158 OTA safeguards,159 LCD wake fix, existing budgets and photo-storage policy. Both exact162 images are published and installed; both boards report SDK VALID. USB installation preserved NVS and the previous selected firmware banks, without clearing OTA allowance or recovery debt.

Validation:17 source regression groups and3 negative controls passed. Six actual captures completed (two each Discard, Check-in and Dish), with all six stored images independently verified through S3. Two captures interrupted populated upload cleanup: ACK63/73ms and cancellation93/133ms. A fresh final Dish cycle completed coordinated sleep. One acquisition took3070ms against the nominal3000ms target; the longer interruption run used the existing LCD denial-limit sleep fallback.

Current working source is162 (`b78efd38ed3ffc795a9fab086a04302c89f0c922`), retaining159 and frozen158 as prior references. Sense image size grows3952bytes and static RAM112bytes; LCD footprints are unchanged. Tests used USB and application command injection, with the actuator for wake. Offline photo SD fallback remains disabled. LCD allowance controls are designed but not implemented; no new OTA transfer or full-product qualification is claimed.

See [the camera recovery handoff](docs/CAMERA_INPUT_RECOVERY_162.md) and its pinned `ACCEPTANCE.json` for raw evidence, installation history and remaining limits.

---

# September14: publish160 for the user’s manual OTA test

Version-only production pair from `957ca816aaf1e90a5fe67b75f891e9d70e44f486`, build `6.4.160-20260914T233149Z-957ca816aaf1`. Runtime matches tested159 except the three generated identity headers. Both canonical builds, artifact checks, independent review, public manifests and complete binary readbacks passed; executable sections and image/static/RTC footprints match159. Local tag `halo-v6.4.160` identifies the exact artifact source; no Git remote push.

No device command, installation, allowance reset or schedule change was performed for this bump. The user will test manually; the last observed Sense daily limit remains a separate possible blocker. Recorded device health remains LCD159/Sense158. Exact receipts are under `hardware-validation160` and pinned in RELEASE_BASELINE.json. Keep the159 wake fixes and158 recovery archive for future work.

---

# September 14: publish159 and verify the scoped LCD wake fix

Published exact159 pair from `da3057b4ed59ad49804daa9974bb73084b5b5ac9`. Installed LCD159/app1 SDK VALID through LCD USB, retaining the158 fallback and all NVS; Sense remains158/app0 SDK VALID with unchanged executable code. Six host suites, three manual-request/wake/terminal/sleep cycles and two seven-item shopping-list refreshes passed. Publication and complete binary readbacks passed; final passive observation found no extra USB wake. See [the159 handoff](docs/LCD_USER_WAKE_159.md) and its exact acceptance receipts.

The real daily discovery allowance blocked all firmware downloads. No quota reset, new paired159 OTA, scheduled159 transfer, physical power-cut or USB-free pass is claimed. Continue development from this159 source or reviewed descendants; keep frozen158 as the preserved recovery and earlier scheduled qualification reference. Daily02:00Pacific scheduling is unchanged.

---

# September 14: 6.4.159 scoped LCD wake candidate

Fix delivery of Settings and Manual Update requests after Sense announces sleep and a PONG goes unanswered. Retain the original outstanding PONG deadline, preserve explicit sleep evidence and bound shared GPIO retries. New user actions can retry an exhausted wake episode; background retries cannot renew it. Short state transitions are serialized without holding a lock across GPIO or logging.

Only LCD wake handling changes. Sense runtime, OTA policy, daily schedule and UI designs remain unchanged. The assembled158 failure predates networking and is separate from the unreproduced extra-wake report. See [the focused record](docs/LCD_USER_WAKE_159.md). Frozen158 remains the accepted baseline until candidate build and device results are recorded. No159 installation or firmware-transfer pass is claimed at this source checkpoint.

---

# September 14: freeze 6.4.158 as the default baseline

Preserved exact published 158 source, paired applications, debug symbols, factory components and selected provenance in a private checksummed archive. Added the current handoff, build/release workflow, OTA validation and recovery/retention guidance. Repository instructions now explicitly select158; prior117 top-level metadata moved intact to historical qualification and older source-selection notes are archived. Added a read-only ancestry/artifact/package preflight for future development.

This is a documentation and host-tooling freeze only. No firmware/runtime bytes, device state, production manifests, schedule or immutable release tag changed. Existing scoped OTA acceptance and remaining USB-free, power-interruption, full-product and factory-station gaps are unchanged. No remote Git push or independent off-machine backup is claimed.

---

# September 14: release 158 scheduled OTA confirmed; Pacific schedule restored

**Version 158 is published and installed on both boards. The single scheduled 157→158 confirmation passed**, including exact native settlement and restoration of the production schedule. The September 14, 09:10:36 PDT update completed automatically: both boards reached app0/SDK VALID; policy was RESOLVED (phase 8, generation 6), with one network window, one daily attempt and no reserved work. Three packet retries recovered; no terminal transfer failure or watchdog reset was observed.

The fixed 157 pair was built from `df8cab35ef8128bdb3d312bd8c016817ad064a85` and installed through a controlled USB bootstrap before the test. Published 158 was built from `b6d06da5997e2252a3472697f30dc8111ab90367`, with identical runtime and resource footprints to 157; only generated identity headers and four pinned release records differ. Both public manifests and complete binaries are verified. Local release tag `halo-v6.4.158` points to that exact artifact source; no remote Git push was performed. Continue development from this tag or reviewed descendants preserving the fix.

**The daily wake is restored to 2 a.m. Pacific:** September 15 at epoch `1789462800`, with the LCD 15 seconds earlier (`1789462785`, SDK timer return 0). Both boards naturally returned to sleep on 158. Restoration rewrote three scoped NVS sectors, archived the already-RESOLVED policy before retiring completed test bookkeeping, and retained both legitimate history entries. Verified readback showed no owed work. No firmware bank or selector was written during restoration. The USB bootstrap and this cleanup were outside the automatic update.

This is OTA subsystem qualification, separate from the historical version 117 full-product qualification. The positive early-calendar WAIT branch was not exercised on hardware; its 13 host tests remain separate evidence. Initial raw prefixes were missed, but scheduled origin/admission and both post-update reboot-to-sleep traces were captured. The 18 ERRLOG and one DIAG `verified=0` notices came from intentionally disabled legacy writers, not attempted NVS-write failures. USB-free operation and power-interruption acceptance are not claimed by this case.

The earlier overnight campaign remains unchanged: four audited passes, five failed cases and four skipped nominal slots, with its recorded retries and capture gaps. This separate confirmation adds one successful scheduled case. All case owners are closed. Exact final verdict, native qualification, restoration, identity, publication and local-tag receipts are pinned in `RELEASE_BASELINE.json`.

---

# September 14: 147 installed; 148 withheld pending 145 policy settlement

The 00:45 scheduled case automatically recovered and installed both 145 images; both boards reported SDK VALID and completed native coordinator reporting/sleep. However, Sense NVS captured before the later 147 USB bootstrap still held **DurableOtaPolicy APPLY (phase 3), generation 12**, not RESOLVED. Coordinator history and valid firmware do not prove policy settlement. Correct accounting is **1 recovered paired installation, 0 fully settled scheduled passes, 0 clean passes**. The original and v2 verdicts and prior peer review are preserved; `VERDICT-v3.json` binds the frozen native policy decoding and source review. These NVS captures precede 147 application execution and are not a fresh postboot policy read. No credits or debt are being reset, and 148 must not be staged while settlement remains unresolved.

Source review confirms an ordering gap: legacy coordinator completion can write done IDs and cancel boot work before durable exact-target reconciliation runs. The preserved obligation is for exact 145 images; newer 147 cannot satisfy it. The focused correction must commit and read back durable RESOLVED before legacy completion, retaining existing readiness and policy budgets. Native recovery and this correction remain unvalidated.

147 is committed at `06650a1685c660eaf149bd275625e9bbb9a059b4`, built and explicitly installed by USB, preserving 145 fallback and all NVS. Fresh Home/idle identity separately confirms both 147 SDK VALID; the installer's earlier SDK-not-yet-observed summary is retained. 147 contains only the reviewed UART panel-wake ownership fix relative to 144; host tests and canonical checks passed. 148 is built/checked/sealed from identical 147 source with version metadata only and unchanged resource footprints. It remains unpublished, uninstalled and withheld. **Latest published firmware remains 145.**

The three original failed cases remain unchanged (22:00 admission miss, 22:40 transfer failure, midnight clock failure); 23:00 and 01:00 slots were skipped during repair/validation. 147 scheduled acceptance, native policy settlement, secondary SNTP recovery and the six-hour clock-failure fallback gap remain open. Continue from this 147 production source and preserve historical 117/139/143 evidence, both failed initial capture receipts and closed native-retry evidence. `RELEASE_BASELINE.json` pins actual build/install/identity/NVS references. Earlier sections below are historical checkpoints.

---

# September 13–14: focused 142 fixes installed; 143 published; scheduled validation pending

The midnight capture is closed: the actual scheduled LCD timer triggered OTA, Wi-Fi connected, but the 15-second network-time attempt timed out before any manifest request. Both boards armed six-hour fallback wakes. No transfer or short automatic retry occurred. A post-window actuator wake and read-only ROM audit verified paired 142 health, absent policy and no partial-transfer debt; those resets are declared interventions. The failing NTP server/packet path and USB causation remain unproven.

Version 144 is the next focused candidate, not yet built or published: one isolated secondary SNTP attempt for the qualified scheduled path, capped at 40 seconds and the original readiness deadlines. Cross-sleep short retry remains outstanding. Manual admission, active-policy budgets, LCD behavior and the existing watchdog/reboot-recovery fixes are preserved.

Fixed source `7eb99f1d194179571b2ba204b2f86f053b600781` is now built as 142/143. Canonical builds, artifact checks and focused host regressions passed. Both boards run 142/app1 SDK VALID after controlled USB installation; Home after actuator wake and natural paired deep sleep were observed. 143 has the same runtime with generated version metadata only, and both latest manifests and complete binaries were verified. **143 on-device acceptance remains pending.** Historical 117 qualification and 139 manual-policy evidence are preserved.

The two focused fixes bound LCD LVGL/DMA quiescence before flash writes while retaining watchdog feeds, and let Sense prove clean recovery from an exactly identified rebooted LCD within the existing 36-second cleanup budget. No allowance refund or watchdog timeout increase. The 22:40 LCD `ui_task` watchdog and unconfirmed cleanup motivated these changes; the exact blocked UI call and a USB cause were not captured. The proposed early-notice cause of the separate 22:00 missed admission remains unconfirmed and unchanged.

The midnight September 14 case is armed for **142 → 143**: Sense epoch 1789369200 (00:00 PDT), LCD epoch 1789369185 (23:59:45 PDT). Only Sense timezone changed; absent policy, original history and LCD NVS were preserved. Native timers and paired sleep were observed. Existing observer 41574/PID 47530 captures 23:58:30–00:08:30; cloud observer 65610 remains the single cloud owner. No further commands, taps, resets, schedule edits or publications belong inside this armed window. Admission was confirmed, but this case closed before download. Exact paired 143 installation and transfer-fix acceptance remain unproven.

Campaign accounting: **0 full scheduled passes; 1 missed admission (22:00); 1 proven transfer failure (22:40); 1 pre-download clock failure (00:00); 1 skipped slot (23:00); 0 native retries.** The failed 140 state was archived before explicit exact 140 USB repair. Native `target_valid resolution=observed_pair credit=unchanged` then resolved phase 8/gen6. Scoped retirement 003 restored original history/Pacific before 142 installation. Two earlier host cleanup attempts refused before any writes (capacity, then computed NVS span). Repair, retirement and 142 bootstrap are not scheduled successes. Final 2 a.m. Pacific/history restoration remains pending after this campaign, using the exact retirement 003/clean142 receipt chain and preserving any new unresolved debt.

Future firmware work must use this fixed production source and subsequent committed records. Later version-only hourly packages use `hourly-scheduled140-20260913/tools/prepare_next_release142.py`; the older helper anchors unchanged 139. Unused 141 remains unpublished. `RELEASE_BASELINE.json` pins the closed build, publication, health, recovery and arming receipts. **All sections below are historical checkpoints, not current publication or device state.**

---

# September 13–14: hourly scheduled OTA diagnostic closed with failed transfer

The September 13 campaign has **zero scheduled passes, one missed admission and one actual failed transfer**. The 22:00 target had no observed admission or transfer; its proposed early-notice cancellation remains unconfirmed. In the separate 22:40 timezone-only diagnostic, native LCD TIMER/reset 8 woke Sense through EXT0/reset 8. The automatic path fetched both exact published 140 manifests and started LCD session 32929. After progress was persisted through offset 262172, the LCD `ui_task` watchdog reset it. Sense ended `chunk_retry_exhausted`, preserved the original 140 debt and deferred its own update to avoid a split pair.

The observer is closed and reaped. UART reports DEFERRED phase 6 / generation 5, no short arm, then `policy_not_due`; both boards entered next-day sleep at Sense epoch 1789450800 and LCD epoch 1789450785 under the temporary diagnostic timezone. No full 140 update or fresh complete paired postfailure health qualification is claimed. The capture observes Sense 139 and LCD 139/app1 VALID after the watchdog; the separate pre-diagnostic paired 139 health receipt retains its original scope.

Only two focused fixes are in development: LCD OTA quiescence/watchdog handling and Sense abort recovery using verified LCD reboot evidence. They are not built, installed, published or physically accepted. Existing local141 is an unused, unpublished version-only package of the unchanged139 runtime and must not be treated as the fix. Public latest remains140; source139 remains the verified development baseline. Preserve failed140 debt and all original case/history receipts; closed-case/no-attempt reset helpers do not admit this failure. Pacific 2 a.m. scheduling and legitimate history restoration remain pending through a debt-preserving path.

The [closed diagnostic verdict](/Users/MattTaylor/halo-device-analytics-2026-09-10/hourly-scheduled140-20260913/DIAGNOSTIC-2240-VERDICT.json) and [runbook addendum](/Users/MattTaylor/halo-device-analytics-2026-09-10/hourly-scheduled140-20260913/RUNBOOK-DIAGNOSTIC-CLOSED.md) bind the final evidence and recovery constraints. Original pending-observation records are preserved. Further hourly tests await focused recovery and validation; no extra pass or skipped-slot result is invented.

# Firmware changelog

## 6.4.139 — published; manual checks use remaining allowance

An explicit manual request can spend the remaining same-day network and work allowance after a closed discovery, including a previous read-only failure. Automatic cooldowns, next-day rollover, legacy/deferred debt, active operations and transfer limits are unchanged. Preserve the actual admission reason so exhausted discovery reports “Daily update limit” rather than “Update postponed”. Version 138 display sleep handling and UI are unchanged.

Canonical builds, native regressions, artifact checks and full public binary readbacks passed from source `97c45f6a20cadff2ae444b99c3928457e7b204e4`, build `6.4.139-20260913T213006Z-97c45f6a20ca`. Both boards are 139/app1 SDK VALID; installation preserved 138/app0 and all NVS. Sense grows by 240 bytes; LCD and static/RTC memory footprints are unchanged.

Without resetting allowance, one manual request checked both published 139 manifests and returned `up_to_date`, consuming the remaining network window. The next request returned `policy_daily_limit` with BUDGET admission and no manifest fetch. Independent review verifies natural Home, darkness and paired sleep in both unchanged captures. Both original harnesses remain INCOMPLETE because USB logging omitted expected text. The quota ACK frame and LCD target suffix were incomplete; local arm payload and SDK timers support the inferred 15-second lead, not proven peer ACK delivery. Cloud confirms the first result and two used windows; the second report upload failed. No new firmware-transfer or scheduled OTA qualification is claimed. Version 139 is the development baseline.

## 6.4.138 — published; coordinated sleep stays dark

A future wake-time message from Sense was counted as user activity while LCD waited for SLEEP_READY. This cancelled sleep, relit the display, and restarted the Home idle timer. Scope the sleep handshake explicitly and keep future schedule storage/acknowledgment active without extending user activity or the OTA awake hold during that handshake. Preserve normal arm-sync grace, genuine touch/scroll cancellation, and current-window OTA handling.

Canonical production builds, artifact checks and full public manifest/binary readbacks passed from source `e77c5acf7246d474fada668fbaf38ac59d2a1a16`, build `6.4.138-20260913T202809Z-e77c5acf7246`. All 13 native regressions passed. Both boards are installed as 138/app0, SDK VALID, preserving 137/app1 and all NVS. LCD binary size increases by 304 bytes; static RAM, RTC use and the 96 KiB graphics configuration are unchanged.

Three manual-result cycles stored and acknowledged the future schedule, stayed dark through the sleep handshake and entered deep sleep on both boards without a relight or reboot. The existing `policy_deferred` / “Update postponed” result and allowance behavior are unchanged. No firmware or allowance changes occurred during these cycles. This verifies the sleep handshake, not a new firmware transfer or scheduled OTA. At this checkpoint, version 138 became the development baseline; earlier acceptance remains preserved.

## 6.4.137 — published; manual update screen memory recovery

A captured 136 freeze stopped in LVGL's out-of-memory assertion while drawing a 324-byte temporary image buffer. The manual daily-limit result had arrived, but the render task held the UI mutex forever. The default assertion handler spins and the UI task kept feeding its watchdog while waiting for that mutex.

Use a repository-owned LCD configuration with a 96 KiB graphics heap, preserving all other LVGL settings. Fatal LVGL assertions now call the platform panic handler instead of spinning forever. The canonical builder applies the same configuration to the LCD sketch and library; Sense and the machine-wide Arduino configuration are unchanged. A queued `uimem` readout measures free space, largest block and peak use under the UI lock. No OTA budgets, schedule, display design or transfer logic changes.

Canonical shipping pair from source `a8ab44b851b438c57ad7ddda11c392d1e9af8bd3`, build `6.4.137-20260913T194121Z-a8ab44b851b4`. Both builds, artifact checks and full public manifest/binary readbacks passed. LCD static RAM grows by 49,152 bytes and its binary by 160 bytes; Sense resource usage is unchanged. Controlled USB installation placed both boards in 137/app1, SDK VALID, preserving 136/app0 and all NVS.

Ten manual requests through the existing Settings action each returned Home naturally. Free UI memory returned to exactly 73,204 bytes and the largest block to 73,120 bytes after every cycle. Cloud confirmed `policy_daily_limit`; the stress test changed no allowance. A separate actual check against published version 137 returned `up_to_date` for Sense, found LCD already current, and returned Home naturally with the same free memory. Before that check, the fully completed version 136 policy was archived and only its Sense retry bookkeeping was cleared in one NVS sector; no unresolved debt, firmware banks, selectors, other NVS or schedule were changed. This was a declared bench allowance fixture, not natural quota renewal. No full Settings navigation, new firmware-transfer, scheduled, sleep-after-no-update or fault-recovery pass is claimed. At this checkpoint, version 137 became the development baseline; the previous 136 scheduled result and qualified 117 reference remain preserved.

## 6.4.136 — published; scheduled transfer and paired health verified

Canonical shipping pair from source `e2d85c003cc280edf9d66c697eaf0d608a881002`, build `6.4.136-20260913T184416Z-e2d85c003cc2`. Production builds, focused host regressions, artifact checks and full public manifest/binary verification passed. Version 135 contains the same runtime and was installed before the scheduled test.

Pause both display rendering paths during UART firmware reception and finalization. Preserve the LCD calendar notice under its original preflight lease, handle its existing 15-second early wake, and retain bounded same-session abort cleanup. Daily budgets, counters, credit rules and user interface designs are unchanged.

The September 13 scheduled 135→136 case used accurate UTC and a recorded temporary timezone to exercise the ordinary 02:00 calendar path. Both boards woke for the 18:55:53 UTC start. Cloud reported both on 136 and LCD updated at 19:00:49 UTC. No taps or USB opens occurred during the autonomous transfer. The initial policy record remained APPLY. On the next ordinary actuator wake, direct health evidence confirmed both 136/app0 SDK VALID and Home; the same campaign then resolved natively at 19:11:15 UTC, with no manual OTA or accounting reset. The LCD USB health read reset the already-valid LCD, so this later observation is not a USB-free control. A duplicate diagnostic reply was truncated in USB logging; the original complete boot FW_INFO and nonce/CRC ID1 were independently verified.

Restored 02:00 Pacific and captured both actual sleep timers. The first night uses an uncredited absolute arm to preserve existing calendar history; normal daily Pacific scheduling remains configured. Final receipts are bound in RELEASE_BASELINE.json.

The earlier 129 and 134 failures remain failures. The 132→134 display transfer stopped at chunk 545 and LCD boot identity changed; its precise reset cause is unproven. Failed cases were archived before the fresh test, not automatically recovered. This is one successful final-build scheduled transfer, not extended soak or fault-recovery qualification. At this checkpoint, version 136 became the development baseline; 117 remained the preserved qualified release and recovery reference.

## 6.4.129 — published; scheduled transfer failed

Version-only shipping pair from source `bbcf3deb854de72db929da53fa078f1c732acb28`, build `6.4.129-20260912T215437Z-bbcf3deb854d`. Canonical builds, artifact checks and full public manifest/binary readbacks passed. Runtime and resource footprints are unchanged from128;128 remains the installed development baseline.

The September12 accelerated scheduled wake/admission succeeded, but LCD chunk646 received no ACK after five sends. Sense downloaded330248bytes and acknowledged329736bytes; the failure snapshot still showed queued TCP data and no socket error. Both boards remain128/app0 SDK VALID. The policy ended DEFERRED and no quick retry wake followed during the closed case. Receiver-side cause remains unresolved; no full129 OTA qualification is claimed. Both actual wake timers were restored to 2am Pacific, with the LCD 15 seconds early, using a one-night uncredited arm that preserves failed-update debt and credit history. This does not promise an OTA at the first restored wake.

Fixed the separate backend diagnostic receipt incompatibility in commit `e0c20c912a009c458f555994ce78a11149b0e147`: omit the optional24-character analytics field only from validated diagnostic acknowledgements. All17 tests and unchanged-firmware D3/H4 parser checks passed. Real historical journals then uploaded handoffs;128 boot1649 reported LCD handoff ACK committed. No firmware runtime change or display-transfer fix is implied. `RELEASE_BASELINE.json` binds publication, failed-case, terminal, health and live ACK evidence.

## 6.4.128 — installed; focused functional fixes verified

Retry manual clock synchronization when the initial window obtained no server address, using a separate static DNS generation and the same bounded recovery budget. Serialize Sense JSON messages across producers so a shopping-list reply cannot merge into a concurrent startup diagnostic. Both canonical production builds and artifact checks passed from source `7025f07cc9efd3b9128c23df6d7eb67586e287d0`. Installed128/app0, preserving127/app1 and provisioning/accounting. Six real HTTP200 refreshes reached the LCD, scrolling0→67→0 passed twice, and both manual checks exited to Home then paired sleep. A fresh boot reproduced the zero-address clock failure; the new retry synchronized in3751ms and cloud telemetry confirmed fresh clock and3/3 successful fetches. The truthful clock result is now allowed in deployed analytics, committed separately as `e283150a2ef1c389cf30e5a80f9925776c662691`. Daily allowance still blocked manifest comparison, so this is not a no-update or firmware-transfer qualification. Initial missing-USB-log capture and earlier127 failures are preserved. At that checkpoint public OTA remained126;128 itself was not published. The later129 publication/test is recorded above.

## 6.4.127 — local functional bug-fix candidate

Give each provisioning accessor its own Preferences handle so concurrent reads cannot falsely report an absent owner. Reject empty-owner shopping fetch/delete requests before HTTP.

Give explicit manual OTA one bounded extra40-second clock-sync opportunity after the ordinary15-second window, using previously resolved numeric servers and preserving DNS/late-callback guards. Save the accurate clock-failure result before releasing the LCD; do not promise a newly scheduled retry after exhaustion. Daily allowance, durable debt, firmware routes and2am schedule are unchanged.

Canonical builds and artifact checks passed from source `3fb0f1019040cfabcbb1dc370cdb8c3f8186ea63`. Installed127/app1 and preserved126/app0, provisioning and accounting. Both SDK VALID, four HTTP200 fetches, three LCD list applications, scrolling, truthful clock-failure terminal, automatic Home8.125s and paired sleep were observed. One startup UI_LIST was lost despite HTTP200, consistent with the unchanged split-write JSON sender; a later manual request lacked any resolved time server and therefore did not start the numeric-only retry. Both gaps are retained as failed acceptance, to be addressed in128. Public production remains126.

## 6.4.126 — published for manual OTA testing

Replace check-in expiration choices with the approved white quantity picker and one large green Confirm button. Small chevrons bracket the number, and dial changes settle in 140 ms. Confirm sends the selected quantity with an empty expiration date through the existing protocol; the old Add Date touch route is removed. The original 30-second default submission remains unchanged.

Remove the discard subtitle and enlarge Add to List and Not Now into side-by-side 126 × 144 px buttons. Shared-screen transitions reset geometry, visibility and quantity motion; LVGL recovery clears the choice screen and its new pointers.

Both canonical production builds and artifact checks passed from source `67a61d68319144822088b8d493d8932a99883a0a`, build `6.4.126-20260911T233117Z-67a61d683191`. Seven focused native scan-choice groups and six OTA-presentation groups passed. Actual generated font/kerning checks verified nine label cases and quantity-animation bounds. Only four LCD runtime headers changed; Sense/shared runtime and OTA behavior remain unchanged. Paired public manifests and full firmware downloads match the sealed release. One calibrated actuator tap completed, but neither USB board appeared. No board firmware installation or allowance reset was performed; 126 physical appearance and OTA acceptance remain pending.

## 6.4.125 — published September 11, 2026 for manual OTA testing

Change the transfer heading to "Something new is coming" and remove "Display transfer only." "Keep HALO connected." remains removed. Progress, checking/result details and OTA behavior are unchanged. Only LCD_Minimal/lcd_ui_task.h changes in runtime source. Version 124 was already published, so these new bytes use 125.

Both canonical production builds and artifact checks passed from source `91fb4716005653ce43ac484b0e2c3e28d1a455e9`, build `6.4.125-20260911T225448Z-91fb47160056`. The new heading measures 270 px in the existing 280 px label using the actual font and kerning. Compiled LCD checks confirm the new title and absence of all three old phrases. Paired production manifests and full firmware downloads were verified after publication. No device installation or allowance reset was performed; physical appearance and 125 OTA acceptance remain pending.

## 6.4.124 — published September 11, 2026 for manual OTA testing

Remove the "Keep HALO connected." footer and the "HALO may restart itself." finishing description. Software Update titles, progress, checking text and result/error details remain intact.

Give only the "On it!" voice acknowledgement a 260ms checkmark pop instead of 500ms, with a smaller 70→89→84px size range. Logged rewards retain their existing animation; the two-second acknowledgement dwell, UART messages, capture and upload behavior remain unchanged. Focused host checks were reported passing for animation geometry, repeated-frame updates, restart and unchanged logged timing. Device visual validation remains pending.

Both canonical production builds and separate artifact checks passed from source `2b40fc651b15ef652ab5561a703e550826839518`, build `6.4.124-20260911T221452Z-2b40fc651b15`. Sense runtime and resources are unchanged from 123 apart from generated version metadata. Exactly three LCD headers changed; the LCD binary increased by 400 bytes with no static RAM or RTC increase. The two removed sentences are absent from the compiled LCD image. Both immutable paired resources and production latest manifests were published and verified by full public downloads. The earlier LCD-only candidate receipt remains archived as historical evidence. At publication, manual OTA and device acceptance were pending; subsequent 123 health is recorded below.

Subsequent bench preparation verified 123 installed on both boards with exact firmware hashes and SDK VALID. The completed 123 campaign had 13 ms left; its native policy was archived and only its Sense retry_v1 bitmap entries retired in one NVS sector. Current 123, fallback 122, selectors, provisioning, future calendar state and all LCD NVS remained intact. Fresh post-clear 123 health passed; the user’s 124 OTA remains pending. This separate bench setup does not change production allowance policy.

## 6.4.123 — published September 11, 2026 for manual OTA testing

Version-only rebuild of the current 122 runtime from committed source `eb767ea73cf6fdde7886b2f05ca892d08e6cfc57`, build `6.4.123-20260911T213344Z-eb767ea73cf6`. Retains the latest shopping-list deletion, right-rim scroll hint, wordless voice waveform and OTA fixes. Only the three generated version/build headers differ in executable source; production flags, partitions, image sizes, static RAM and RTC usage are unchanged from 122.

Both canonical production builds and artifact checks passed. Immutable paired resources were staged and verified, then both production latest manifests were promoted to 123. Fresh full downloads matched both binary hashes, sizes and embedded identities. No device was flashed, awakened or asked to update, and no quota/accounting state was changed. The last verified installed build remains 122; the user's manual 122→123 OTA test is pending. `RELEASE_BASELINE.json` binds exact publication and source-equivalence receipts. Future development starts from 123 source and subsequent commits.

The first user retry hit the completed 119 campaign’s same-day allowance, with 12ms remaining. Its native record was archived, and only allowlisted bench bookkeeping was initialized after verifying no active update debt. Four NVS sector writes preserved both firmware banks, selectors, unrelated values and timezone; both boards returned to exact 122/app1/SDK VALID. The user’s manual 123 OTA remains pending. This separate test setup leaves production policy unchanged.

Later hardware readback confirmed the 123 images in app0 on both boards, with 122 retained in app1; both reported SDK VALID. This supersedes the earlier pending installed-identity status while preserving the original publication and bench preparation records. No new full transfer trace or124 installation is claimed.

## 6.4.122 — local shopping and voice feedback polish, September 11, 2026

Removed the transient "Deleting..." toast. Items still wait for backend confirmation before removal; duplicate-request protection, failure feedback and timeout handling remain intact. Added the approved curved "TURN TO SCROLL" hint on the shopping list's right rim, clear of the bottom buttons. It hides when content fits, during visible refresh feedback, and under the Delete dialog. The pre-rendered alpha mask needs no rotated labels or full-screen buffer.

The voice hold screen now uses a wordless, decorative teal waveform above the user's finger, with the white microphone card kept at its Home position. Only small bar heights change, at up to 20 frames per second during an active hold. The existing 500ms activation, 10-second limit, Sense audio/UART handling, and release response remain intact.

Canonical paired builds and artifact checks passed from source `d2362a7`. Installed 122/app1 by USB with exact 121/app0 preserved and no installer NVS, quota, bootloader or fallback-bank writes. A normal actuator wake confirmed both 122/SDK VALID. Three fresh HTTP 200 six-item fetches, viewport scrolling 0→67→0, visible right-rim cue, zero sampled display flush faults, Home and natural paired sleep passed. Initial Wi-Fi timeout and cache-cooldown harness limitations remain recorded; the passing final capture changed only host refresh spacing. Voice host/source checks were reported passing; physical waveform appearance and audio upload were not exercised. `RELEASE_BASELINE.json` binds actual receipts. Public OTA remains 119.

## 6.4.121 — local device shopping deletion fix, September 11, 2026

Read the API's `itemUUID` field (with the older alias retained), send the shared UUID on removal and require a positive `affectedRows` count before reporting success. The LCD keeps the row until the backend confirms deletion, then removes and animates the matching row; failures show a retry message. The Delete dialog pins the displayed item ID, and requests use the existing Sense awake-proof queue. OTA and network configuration are unchanged.

The native regression executes the production parser and delete path with mocked HTTP/UART: 234 checks pass on the fix; the 120 source fails 60 checks. Delayed LCD deletion and existing shopping-idle regressions pass. Both canonical builds/artifact checks passed from source `cbf5332`. Installed 121/app0 by USB with 120/app1 preserved and no installer NVS/quota/bootloader writes. One Delete touch-handler action removed Olives: backend `affectedRows: 1`, all six other records unchanged. Fresh refresh and a subsequent wake show six correct labels; both boards exact 121/SDK VALID and naturally asleep. Two harness limitations (completion-animation timing and a truncated USB prefix) remain recorded; the final read-only wake capture passed. Public OTA remains 119; RELEASE_BASELINE.json binds the receipts.

## 6.4.120 — local device UI update, September 11, 2026

Removed shopping-list selection arrows and green borders while retaining dial scrolling and direct item taps. Simplified Delete confirmation: removed the explanatory sentence, used a regular small Delete heading and a larger red item name; button geometry and deletion logic are unchanged.

Canonical paired builds and artifact checks passed. Installed on the unit by USB with119 retained in app0 and no NVS, quota or bootloader writes. Fresh exact120 SDK VALID identity, seven-item list fetch, scrolling0→4→0/y0→123→0 and paired natural sleep passed. Dialog layout was reviewed in source; no backend delete or new dialog screenshot was performed. Public OTA remains119. Source3633698 and receipts are recorded in `RELEASE_BASELINE.json`.

## 6.4.119 — published September 11, 2026 for manual OTA installation

Published the exact canonical pair from source `09f1d525fd82dc54eacbe4fe348054102462774a`, build `6.4.119-20260911T162403Z-09f1d525fd82`. Background coordinator preflight no longer takes over the display; unlock and lease expiry return Home only when OTA owned the presentation. Shopping entry, refresh completion and activity receive a fresh idle interval, and loading does not consume it. Includes the118 startup shopping Wi-Fi wait, boot-scoped action telemetry and pinned SDK DNS cache-lock correction.

Both production builds, artifact checks, focused host regressions and live publication/hash verification passed. One paired manual118→119 OTA then passed with exact hashes and SDK VALID on both boards, confirmed in the cloud. Focused USB checks passed three shopping fetches, scrolling, list visibility, natural Home and logged sleep. Existing keepalives extended the idle interval; isolated timing is covered by host regression tests. The completed117 bench allowance was archived and test bookkeeping initialized before this request; production quota logic was unchanged. External117 recovery artifacts remain preserved;119 now occupies app0 and118 is the prior on-device image. Continue development from119 source. `RELEASE_BASELINE.json` binds exact receipts, earlier partial captures and validation limits.

## 6.4.117 — scoped UI/manual OTA fixes, September 10, 2026

Built 116/117 from committed source `1e624b5e8d2f44f8f1c91cbe455df5f2f8d7a66f`. Replace the microphone bitmap background with the existing vector icon and the unsupported Settings separator with ASCII; center shopping labels and reveal the selected row while turning the knob. Manual Software update uses one acknowledged request, a stable checking/result screen, and no sleep/identity/timer re-dispatch. Explicit manual discovery can replenish a later UTC day only after RESOLVED work; failed debt and same-day bounds remain intact. USB `ota` exercises the same Settings action.

Canonical 116/117 builds and focused native/action/LVGL checks passed. The real 116 seven-item list scrolled 0→6/235→0. One Settings-action manual request installed exact 117 on both boards; saved full hashes, SDK VALID, Home, cleared OTA flags and natural sleep passed. Production 117 pointers were promoted and verified. The final same-version request correctly produced `policy_daily_limit`, held the result for eight seconds, returned Home without re-dispatch, and naturally slept. Fresh paired 117 SDK VALID/build evidence passed. Release tag `halo-v6.4.117` and the normal development checkout identify this accepted release; exact bindings are in `RELEASE_BASELINE.json`. No new scheduled or extended-soak qualification is claimed.

## 6.4.115 — published September 10, 2026 for manual OTA testing

Version-only rebuild of the released 6.4.114 runtime from committed production baseline `79ee6ee8695738dc90513da3cc797007edc07d11`. Only generated version/build metadata differs in executable source; production flags, partition layouts, static RAM and RTC usage are unchanged. Both canonical builds and uploaded binary/manifest checks passed; both production latest pointers now serve 6.4.115. The user will perform the manual installation, which has not yet been qualified for these new bytes. No device firmware, scheduling or retained allowance was changed by publication. See `published_manual_test_bumps` in `RELEASE_BASELINE.json` for exact receipts and the saved test-allowance limitation.

## 6.4.114 — released September 10, 2026

A retry could wake within its15-second lead interval while the separate LCD timer-origin notice was still unavailable. The existing fresh clock and peer handshake were ready, but policy cancelled the entire opportunity before its due time. The correction lets a valid persisted shipping ARMED record wait inside that lead interval with a current correlated peer. It preserves the actual missing-origin diagnostic, original deadlines and all target/accounting gates. It returns false before due, so this wait grants no early transfer or reservation.

The native test now starts from the observed due−12s timing with an absent origin rather than assuming that notice was accepted. Exact 113 private and 114 production builds and the portable 114 package have passed verification. Manual 113→114 installation passed, with both exact 114 images SDK VALID, Home/idle and resolved accounting. Scheduled114 installation also passed; final paired health, accounting, Pacific timers and natural sleep were verified and production latest promoted. See RELEASE_BASELINE.json for scope and exact receipts. Version112 was not released; its on-device transfer failures and missed retry remain recorded below.

## 6.4.112 — superseded candidate, never released

A verified early LCD timer notice could be rejected because retained OTA debt had already queued the boot check as `coord_recovery`. The queue preserves its first reason and deadline, but the retry readiness check incorrectly required that reason to equal `lcd_timer`. The correction uses the accepted timer origin and currently queried peer identity, while preserving the original deadline, stored arm identity and all retry accounting.

The native regression fails on the preceding source and passes with the correction. It covers the five queue reasons, ten invalid-origin conditions, clock/storage/expiry/deadline boundaries, and the actual canonical reservation/codec operations. The earlier post-transfer cleanup/arm regression also passes. Exact production112 build and one clean scheduled109→112 installation are the user-directed release gate. Additional fault/repeat runs and a separate final manual reinstall are deferred under the one-hour shipping scope. Candidate108 was never released.

The actual September10 scheduled109→112 run failed during LCD transfer. Its armed retry woke but returned to sleep on109 without a second reservation, with an early readiness snapshot reporting `origin:false`. The queue-label fix is therefore insufficient for hardware recovery. The one-hour deadline was missed; production latest is unchanged, and this candidate has no release tag or production acceptance. Exact cause of the transfer failure remains unconfirmed.

Deadline closure restored Pacific scheduling while preserving valid109 firmware, provisioning and failed-campaign accounting. Saved ring records identify an HTTP no-data abort at835,080 bytes and a later connection-loss abort at304,405 bytes; final policy is DEFERRED/gen8 with no second reservation. The recurring soak is paused. The bounded-readiness correction was subsequently implemented and natively tested in candidate 114.

## 6.4.108 — superseded candidate, never released

The shipping102-to103 calendar case downloaded and acknowledged16924LCD bytes, then stalled; its immediate second HTTP request failed. Both boards retained valid102 images. The prepared five-minute retry closed without a verified arm and was deferred until the next daily wake despite remaining budget. The failed run did not capture the fresh query payload, so its exact rejected predicate is unknown. Source review found two deterministic gates that reject a healthy old LCD: a changed image-begin count and the requirement for a live coordinator lease that ordinary OTA_LOCK clears.

The committed correction preserves confirmed transfer cleanup, fresh nonce/current-boot proof, exact invocation-local old-image identity, selected/running VALID state, competing-owner rejection, user-work guards and existing deadlines/accounting. It permits an exactly empty owner/zero-lease tuple after cleanup rather than treating it as evidence of a failed image. No TLS/network root cause is claimed from the saved socket snapshot.

Reserved validation:105 bootstrap,106 isolated LCD integrity-failure plus autonomous retry,107 clean scheduled repeat,108 exact production manual bridge and Pacific scheduling. All four pairs now pass canonical artifact and independent resource checks from source commit9d38091f2b23341f5a12f423efa5fa71cf0f6978. Controlled 105 setup passed, but the 105→106 scheduled case failed: the transfer stalled at 1,383,690 bytes, a five-minute retry was confirmed and woke both boards, then policy deferred without a second network reservation. Further source correction is required; later acceptance, publication, release tag and original-checkout adoption remain pending. Existing102/103/104 bytes are unchanged;104 is not a production release.

## 6.4.104 — superseded candidate, never released

Versions 6.4.102/103 are staging canaries with shipping policy and a fixed private OTA route. The final 6.4.104 image uses the normal production route; canary validation and exact final-image validation remain separately recorded.

Prepared from the tested M8 source instead of rebuilding the older development checkout without its fixes. The integration includes the bounded shared Ordinary/Diagnostic/Admission POST transport, reduced diagnostic/report stack use, and one checked watchdog enrolment acknowledgement while preserving the global watchdog policy. Durable OTA accounting, clock provenance, paired target checks and normal calendar scheduling remain part of the release source.

The canonical production builder now selects the previously compiled M8 shipping feature profile explicitly and rejects local MQTT credential overrides. The tracked MQTT-disabled configuration contains the existing public CA and empty client credentials; it does not enable MQTT. Bench, one-shot, local diagnostic credential provisioning, idle-network probe and intentional fault controls are excluded from the shipping profile.

Evidence already available: three earlier **bench** scheduled paired installs completed in 318, 303 and 309 seconds, each subsequently SDKVALID/Home with full resolved policy. Shipping 98 and default-off shipping 99 compiled. These do not establish shipping 6.4.104 acceptance. The 101 pair's later supported bench stop and restored Pacific calendar timers are separately archived.

Pending: actual 6.4.104 source commit/tag, both binaries and served manifests, finite shipping acceptance, factory/rollback instructions, production promotion and adoption by the standard development checkout. See `RELEASE_BASELINE.json` and `docs/PRODUCTION_RELEASE_ACCEPTANCE.md`. Optional M9 report-expiry wake and durable outbox work is not included.

For every later version, record the concrete behavior changed, the source commit/tag, exact paired artifact manifest, tests performed and material limitations. Change this entry to released only from actual completion/publication evidence; do not rename a candidate or reuse an immutable version to conceal changed bytes.

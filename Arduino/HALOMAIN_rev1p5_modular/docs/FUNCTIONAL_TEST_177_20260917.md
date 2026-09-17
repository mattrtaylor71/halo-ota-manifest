# Firmware 177 functional campaign — September 17, 2026

Installed Sense and LCD: `6.4.177-20260917T063649Z-ebb006726a81`, source `ebb006726a81b947f13fc2dc500c3de9ffd66444`, both app1 SDK VALID. This campaign exercised the installed firmware without flashing firmware, directly editing credentials/NVS/partitions, or switching Mac Wi-Fi. It found a reproducible reconnect defect; the passing cases below do not qualify every edge case or a new public release.

Private evidence root: `/Users/MattTaylor/halo-functional177-20260917`. Raw captures and cloud receipts can contain private identifiers, network details, and signed URLs; this report intentionally records no such contents.

## Host regression results

**60 distinct host suites passed** (63 grouped executions, with three suites shared between groups), after repairing one test-fixture compilation boundary:

| Area | Suites | Private receipt beneath evidence root |
| --- | ---: | --- |
| Network, camera, retry policy | 19 | `network-host001/RESULT.json` |
| Voice, storage, transport, durable custody | 12 | `voice-host001/SCOPE_ASSESSMENT.json` |
| UI, shopping, provisioning, sleep, OTA policy | 32 | `ui-ota-host001/FINAL-RESULT.json` |

`tools/test_lcd_calendar_notice.py` needed only the missing no-op `lcd_media_retry_wait_release(const char*)` extraction-boundary stub. The original compile-failure log remains retained; the official rerun passed five scenarios. No firmware implementation changed. The broad voice input manifest noticed this unrelated fixture edit; its scope assessment confirms the 12 executed suites and their runtime inputs remained unchanged.

The suites include actual production functions and software LVGL rendering with controlled SDK/clock/transport boundaries. Storage fault campaigns cover 3,035 stress cases and 1,106 custody cases; these overlap other suite coverage and are not additional unique suites. Host filesystem fault injection does not establish physical SD power-loss safety. OTA host policy checks do not establish a real transfer or installation.

## Installed-device observations

`health001/RESULT.json` records an assisted wake, fresh paired identity, awake/unlocked HOME, and paired sleep. `online003/RESULT.json`, `UI-OBSERVED.json`, and `voice-observed001/online003-observed.json` cover the subsequent shopping, camera, voice, and missed-upload sequence.

Shopping entry, refresh, and pull completed three refreshes with seven rows; two forward scroll commands advanced selection 0→2. Reverse scrolling was commanded but its final selection was not separately sampled. No shopping row was deleted. Check-in, Dish, and Discard reached LCD `DONE`; voice reached RECORDING/UPLOADING and local worker completion. These are local action/UI outcomes, not backend delivery acknowledgments.

The main actions used LCD console injection, not physical touch or knob operation. The initial wake used the actuator. The controller issued Check-in confirmation too early after matching instructional text; `online003/CONFIRM-INTERVENTION.json` records the explicit correction after actual job55 WAITING_INPUT. That wait is not firmware capture latency. One list-state sample was interleaved and unreadable.

UI review found 211 complete heartbeat samples and 52 complete health samples with zero reported flush failures or soft faults. The LCD entered idle-dark, stayed dark during media custody waits, received coordinated `SLEEP_READY`, and logged deep-sleep entry with Sense. This is serial evidence, not an independent screen recording or power measurement.

Four real captured payloads missed their immediate uploads and were committed to LCD SD:

| Payload | Local job | Bytes | Current observed outcome |
| --- | ---: | ---: | --- |
| Check-in image | 55 | 130,530 | Natural retry; cloud custody independently confirmed |
| Dish image | 90 | 130,506 | Natural retry; cloud custody independently confirmed |
| Discard image | 102 | 130,000 | Fourth retry interrupted by physical input; fifth natural retry delivered and cloud custody confirmed |
| Voice | 116 | 75,776 | Natural retry; cloud custody independently confirmed |

The three images were 1280×1024 and captured in 1,194 ms, 1,659 ms, and 1,074 ms respectively. Voice recorded 2,370 ms. The sleep flush took 55,763 ms, emptied the RAM queue, and armed an acknowledged 300-second media retry. RAM `DRAINED` did not mean backend delivery: durable records remained for subsequent wakes.

Natural recovery delivered all four original payloads across four successful replay wakes; one additional wake was deliberately interrupted. Read-only cloud verification matched request binding and captured byte counts; all three image object SHA-256 values matched. Voice metadata SHA matched, but object HEAD did not provide an independent object SHA-256. Backend processing reported voice completed and image jobs DONE; this does not establish scene recognition, transcript accuracy, or intended shopping semantics. Evidence: `cloud001/RESULT.json` and its referenced individual receipts. The checker initially used an incorrect Check-in admission key and Discard bucket expectation. Both read receipts are preserved and excluded; corrected expectations match the actual backend contract, with one justified follow-up read each. The final result binds those corrections.

On the fourth retry, physical actuator input immediately produced LCD `touch_press`; Sense yielded with `retained=1` and scheduled another 300-second retry. This tests one real user-priority interruption, not all touch interactions. Evidence: `cloud001/DISCARD-INTERRUPTION.json`.

**Final Discard recovery passed:** the fifth natural wake fetched the retained 130,000-byte item, obtained HTTP200 and the durable backend acknowledgment, then acknowledged SD deletion. Independent admission/job/object verification matched the original image. The LCD remained dark during this recovery. A sixth native wake encountered fresh-SNTP failure while RSSI varied down to -88 dBm. It remained responsive/dark, expired the original 120-second readiness deadline, entered paired sleep at about126 seconds and armed a300-second relative retry. This was a bounded clock/readiness failure, not a freeze; it did not reconcile the empty inventory that boot. The following declared actuator wake obtained fresh time and reconciled the queues.

**Manual OTA did not reach the no-update check.** Production manifest readback showed both public images at162, below installed 177. One supported LCD Settings action (`ota`) was acknowledged, then refused before any manifest request: `phase=9` (DISCOVERY), `admission=7` (LEGACY), terminal `policy_deferred`. The LCD held its result for approximately eight seconds, returned to HOME with both OTA flags clear, and slept normally. This is a passing refusal/exit path but an open manual-OTA acceptance blocker; it is not an up-to-date result or a daily-limit result. No quota, policy debt or coordinator record was cleared to force success. See the policy finding below.

**Final inventory and sleep passed within scope:** following an ordinary actuator wake, typed voice inventory reported pending0/incomplete0/corrupt0; image inventory reported pending0/incomplete1/corrupt0. The image residue existed before this campaign and was preserved. The Sense independently reconciled empty lists, persisted pending0, and obtained acknowledgment for clearing the media retry timer. The shopping list moved selected0/scroll0 → selected3/scroll67 → selected0/scroll0. Fresh paired177/app1 SDK VALID identity, responsive UI, coordinated sleep and15.75 seconds without USB re-enumeration were recorded. This is an assisted final inspection, separate from the command-free natural recovery wakes.

Private final evidence: `final001/RESULT.json`, `final001/controller-result.json`, `voice-observed001/recovery-user-priority-observed.json`, and `ota-manifests001/RESULT.json`. The recovery receipt binds raw logs and distinguishes the deliberate fourth touch from unassisted dark wakes. Logs start after USB enumeration, so they do not prove the very earliest boot pixels or electrical backlight behavior.


## Open defect: obsolete asynchronous Wi-Fi attempt

The real sequence exposed an asynchronous reconnect bookkeeping defect. After Check-in queued its upload, maintenance began STA association. The next camera operation intentionally stopped Wi-Fi for DMA, but left `wifi_connect_inflight` set. Voice preconnect then treated the cancelled attempt as active. Maintenance waited roughly 25 seconds, timed it out, and added four seconds of backoff; the sleep flush preserved the jobs on SD instead of completing immediate uploads.

`voice-observed001/async-connect-reproducer002/RESULT.json` reproduces this using the actual `service_wifi_maintenance`, `camera_quiesce_wifi_for_dma`, `deinit_camera`, and `SenseBackupWifiCall` functions with controlled clock/radio boundaries. It demonstrates no new association until 25,001 ms plus backoff, and an unnecessary hard-reset charge. Its expected-failure status is evidence of an installed 177 defect, not a passing product invariant. The flag is RAM-only; a fresh-boot control starts a new association. **The defect is not fixed by this documentation or the calendar test stub.**

## Open acceptance blocker: retained OTA coordinator state

The manual check was blocked by legacy work, not connectivity: fresh SNTP and connected Wi-Fi were already present. `Admission::LEGACY` is returned by `reserve_discovery` when its caller passes retained legacy work. The boot was already running `coord_recovery`; the source only queues that path for a nonempty, unfinished `g_coord_pending` record. That existing coordinator record is sufficient to trigger refusal. The identical phase9/admission7 refusal already appears in the retained175 setup capture (`/Users/MattTaylor/halo-provisioning-review-2026-09-16/device175/app-provision001/sense.raw`, line418), so it predates177 and this functional campaign. `storage=0` means READY; neither this value nor `normal=0` is a reported storage fault. Its exact durable identity/debt and legitimate closure still need a separate read-only diagnosis; this campaign did not read or rewrite NVS to invent a completed update. The previously retained shipping policy/history must remain intact.

The user-facing result recovers correctly, but manual OTA acceptance remains blocked on this unit. Actual new paired transfers and scheduled OTA were not run in this campaign.

## Remaining qualification limits

No electrical power-cut recovery, USB-free behavior, controlled weak-RF campaign, or new paired OTA transfer/install was established. The observed RSSI drop was uncontrolled; it does not prove the cause of the SNTP failure. Console-driven UI coverage does not qualify every physical touch/encoder flow, panel appearance, or shopping deletion. Bench voice/scene semantics and audio quality were not assessed. The user reported that 177 provisioning worked well; a separately instrumented fresh provisioning/confirmation-animation run followed immediately by a same-boot camera capture remains unqualified. Preserve the existing camera DMA reserve warning and avoid a broad memory-resolution claim.

Initial `online001` stopped before media actions because its startup gate waited too briefly; `online002` failed in controller startup before device descriptors or commands. The first final controller was stopped before hardware ownership because its delete matcher expected job102 while the control response logs job0; the corrected controller binds the following exact sd_delivered102 evidence and preserves existing incomplete residue. These harness failures/corrections remain recorded and are not device failures or passing action cases.

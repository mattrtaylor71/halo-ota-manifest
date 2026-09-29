# 6.4.233 production release

Published September 29, 2026 at Matt’s explicit request. Both production latest manifests, both immutable versioned manifests and complete Sense/LCD binaries were read back and matched the approved release plan. This releases the accumulated 225–233 work over production 224. Future changes start from the source below or reviewed descendants on `codex/ram-qualification`, using a freshly inventoried unused 234+ identity. Factory 197 and frozen 158 recovery are unchanged.

Exact compiled source: `730bdd4cedbfe27e1708dc9e986900c8349024f2`.
Exact paired build: `6.4.233-20260929T075757Z-730bdd4cedbf`.
Compared against production tag `halo-v6.4.224`, source `518691c8d3bd609272b438ec31b82c4de1b6b053`. This is the accumulated 225–233 work, not merely the last battery animation change.

## Publication identity and receipts

| Board | Bytes | SHA-256 | Public image |
| --- | ---: | --- | --- |
| Sense | 1,880,976 | `da41ed5a83e31d3b3407deddbe36eae21bf1b98ed8a40284158ee45f4e06ddb9` | [Immutable Sense233](https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/artifacts/sense_6.4.233_da41ed5a83e31d3b.bin) |
| LCD | 2,048,960 | `1e25ff9122c9368a45e0e131014967bb41e7c5697d354c1a83cd03ff9e940d8c` | [Immutable LCD233](https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/lcd/artifacts/lcd_6.4.233_1e25ff9122c9368a.bin) |

Source firmware tree: `7a9150de710151c876393370e6ff5c14b34093f6`. Annotated tag `halo-v6.4.233` object `5ebacf8b959fe2f3dc0f7f74e0ad44bab90376b1` points to the exact compiled source. The tag object and peeled compiled commit were independently verified on the remote; the subsequent documentation commit is separate from the compiled release.

| Evidence | SHA-256 |
| --- | --- |
| [Release plan](/Users/MattTaylor/halo-battery-menu-20260928/publish233/release.json) | `a510625748f6d8c2c5afae89e3d23759aadfc7aafeb24d088175b711ec0ba7af` |
| [Immutable staging](/Users/MattTaylor/halo-battery-menu-20260928/publish233/stage001/result.json) | `756392bb725614be534c26c71f51d81c878985797c87d2203bcdac45a2247491` |
| [Paired promotion](/Users/MattTaylor/halo-battery-menu-20260928/publish233/promote001/result.json) | `caa824cebe776d990092a2ac59f63e8bbc751b81fab0f273df75597283d24811` |
| [Public full readback](/Users/MattTaylor/halo-battery-menu-20260928/publish233/PUBLIC-READBACK.json) | `ec56509f2db59234ce1dff49cc2e4eeab1b74673f0ecce0db154f5ce8169c941` |
| [Exact local publisher closure](/Users/MattTaylor/halo-battery-menu-20260928/publish233/LOCAL-CLOSURE-VERIFIED.json) | `91db00ae815de19acb2ee80716b2f0328caf31bb7b2a3d3ca7ed5c082e2e570e` |
| [Source tag](/Users/MattTaylor/halo-battery-menu-20260928/publish233/SOURCE-TAG.json) | `181e18a09b0a4693c4841e57eaeeed5766dc69e8420ea7056afd92961567e3f7` |
| [Remote source tag](/Users/MattTaylor/halo-battery-menu-20260928/publish233/SOURCE-TAG-REMOTE.json) | `575e1f0b735899756cecbb3933516c960a01b60d5f96e4f883cb4624b7904eff` |

Publication used the unchanged canonical paired publisher and exact 1,659-file hash-verified closure on the MacBook, with original paths preserved on a task-owned APFS volume. It performed no compilation and changed no IAM policy. The earlier restricted-session issuance was denied before any cloud writes; it is not a successful staging attempt. The current immutable release plan and closed stage/promotion/readback receipts are the publication evidence. No new archive is claimed by this documentation update.

## Changes included

| Area | What changes for the user or device |
| --- | --- |
| Memory headroom | Voice upload attempts now use the existing task-owned TLS preference for external RAM, with the original fallback and cancellation behavior. Presign JSON documents are destroyed before network transport while their serialized payload remains owned. An unused Sense UI queue and microphone mutex are removed. Resource diagnostics now include PSRAM and qualified upload-worker stack margin. |
| Shopping-list refresh and stability | A Refresh press now reveals the border animation when it joins an already pending/in-flight silent refresh. It reuses the request and original deadline rather than creating duplicate network work. Refresh-state handling no longer renders LVGL from the UART task; the owning UI task performs rendering. These restore previously tested private fixes that were absent from 224/225. |
| Voice while browsing the list | List-mode voice response waits now use the remaining existing 12-second cooperative workflow budget instead of prematurely giving up after 1.5 seconds. This prevents some successful cloud uploads from being unnecessarily retained and retried when their acknowledgement arrives later. Urgent capture still cancels/parks the owned request and resumes it later; list refresh still shares the transport rather than opening a parallel TLS request. |
| Haptics | Touch and scrolling vibration routes are disabled. Startup places the haptic driver in standby, stops playback and clears RTP, with bounded readback diagnostics. |
| Display inactivity | Background schedule-delivery keepalives keep CPU/UART coordination alive without refreshing the user's screen timer. Home can darken after its normal 10-second idle interval instead of staying lit roughly 32 seconds. Actual user activity and real OTA presentation retain their existing visibility rules. |
| Voltage diagnostics | The LCD samples its calibrated system-supply ADC and forwards a bounded cached observation to an already-awake Sense. Ordinary reports carry `system_power`; eligible existing API requests also attach a bounded power header. Freshness, missing data and read errors remain explicit. No new cloud transaction, wake or retry is added. |
| Home battery display | Adds the reference-matched bottom arc and percentage while retaining the existing menu icons and synchronized touch/voice geometry. Estimated 26–100% is teal-green, 11–25% amber and 0–10% red. A confirmed high external rail shows a bright green lightning bolt. Cold/persistent unknown is blank; a previously confirmed indicator can bridge a transient unknown for at most 1.5 seconds, with a bounded 240 ms bolt-to-percentage fade. The display reaches 100% at 4150 mV when admitted as battery. |

The published 224 stale-media bookkeeping corrections remain included. The image/voice spool and upload-persistence files have no diff against 224. This release does not change the configured 2 am Pacific schedule, retry cadence, request identity or durable acknowledgement rules, and does not introduce a new OTA download/apply algorithm. The power header adds observational metadata to existing request transports; it does not change saved image/voice bodies or their immutable media identities.

## Evidence supporting the release

- **Exact 233 build:** all 130 canonical snapshot suites passed, including the existing 128 suites plus estimator/actual-LVGL renderer coverage and negative controls. Both paired artifacts and locked build environment verified. Estimator: 24,210 assertions; renderer: 459,730 checks. These are host results, not physical visual qualification.
- **Exact 233 bench:** both intended boards freshly reported 233/app1, boot app1, SDK VALID. The inactive-bank installs preserved each 232/app0 fallback, NVS, partition table and filesystems. Native Home → List (11 items) → Home, power readback, UI memory checks, 10.004-second panel-off, paired sleep and 75 seconds without reopening all passed. Both media retry arms cleared; the next 2 am Pacific maintenance timer was armed. All three scoped hardware jobs, workers, captures and lease handles closed.
- **233 resource snapshot:** supply readings were 4908 and 4900 mV. LCD UI free/largest block was 79,544/78,884 B initially and 70,800/70,712 B after List/Home, with 1% fragmentation; both exceed the unchanged 32,768/16,384 B review floors. Remaining firmware-slot space is 85,104 B Sense and 572,480 B LCD. The last 232→233 change adds 32 B static LCD DRAM and 608 B application bytes; Sense sizes are unchanged.
- **Earlier retained path tests:** the 225 RAM campaign measured image, voice, burst, interruption and saved-media recovery paths above unchanged resource floors. On 227, finite capture/cloud delivery, refresh during voice POST, scrolling, item deletion and haptic standby were observed. On 229, a native saved-voice retry acknowledged and removed the recording while the LCD stayed dark. An exact 229 ordinary report retained the full system-power observation in the cloud, correctly marked stale.
- **230 voice-budget correction:** one real voice/List/Refresh case received its first acknowledgement in 4.797 seconds. A separate Check-in interruption parked/resumed the voice and delivered both voice and image. Scoped cloud readback found one completed worker per voice. Each case ended in paired sleep and 75 seconds quiet. These are prior-version affected-path evidence preserved in 233, not fresh 233 repetitions.

## Important qualification limits

1. **The percentage is provisional.** It estimates from system-rail voltage, not a calibrated cell state-of-charge curve or remaining-runtime measurement. The full-range plateau and 4400/4300 mV external-power hysteresis are presentation choices. The lightning bolt means external power, not verified charging/full charge. A low rail with USB data attached is withheld as ambiguous. Cloud cell voltage/percentage/source stay unknown; no battery safety or OTA-admission threshold is introduced.
2. **Physical unplug/reconnect and visual transition review remain separate.** Native renderer coverage and serial state do not prove on-device pixels, perceived smoothness or every physical gesture.
3. **No new 233 OTA transfer or scheduled 2 am execution was performed.** The private install used verified inactive-bank service. Timer arming is proved; later OTA installation must be observed separately. Publication makes the pair available to eligible manual/nightly checks and does not force immediate fleet installation. Existing post-provisioning recovery checks may also encounter the published update earlier than 2 am.
4. **Resource qualification is finite.** Earlier 230 aggregate resource reports remain incomplete because some diagnostics interleaved; valid samples pass unchanged floors. Two 233 UI snapshots are not continuous minima, leak testing or whole-task stack bounds. The historical resource model remains `REVIEW_REQUIRED` rather than being recalibrated to force a pass.
5. **Earlier failures stay recorded.** The prior voice first-attempt timeouts and observer failures are not relabeled successes by later recovery. The recordings qualify transport/custody, not a named shopping-list phrase. The current tests are not a repeated full-product, provisioning, weak-RF or cold-power campaign.
6. **Cloud telemetry scope is limited.** Ordinary report retention is verified historically. Most individual request backends still ignore the new optional power header; this release does not deploy backend retention or a battery analytics projection.

## Source and evidence references

Reviewed Git diff: `halo-v6.4.224..730bdd4cedbfe27e1708dc9e986900c8349024f2`.
Firmware documents are under `/Users/MattTaylor/halo-retry-release-20260928/Arduino/HALOMAIN_rev1p5_modular/docs/`:

- `RAM_QUALIFICATION_20260928.md`
- `LCD_REFRESH_HAPTICS_226.md`, `SHOPPING_LIST_PANIC_225_20260928.md`
- `BACKGROUND_MAINTENANCE_DISPLAY_20260928.md`
- `SYSTEM_POWER_TELEMETRY.md`, `POWER_TELEMETRY_229.md`
- `VOICE_LIST_RESPONSE_BUDGET_20260928.md`, `FUNCTIONAL_230_20260928.md`
- `HOME_BATTERY_MENU.md`

Exact current build: `/Users/MattTaylor/halo-battery-menu-20260928/build233/POST-VERIFY.stdout.json`.
Closed current bench: `/Users/MattTaylor/halo-battery-menu-20260928/device233/FINAL-BENCH-CLOSURE.json`, SHA-256 `133d6a69ec665cc7126b4a3e8c1c6056a955f7d6bf398b7fad716c52362e7854`.

Shared context consulted: `engineering/halo-resource-testing` (dated September 23, historical evidence/measurement rules). Current Git, artifact and closed-device receipts take precedence over that page's older deployment checkpoint. The release documentation itself adds no shared-brain write, rebuild or hardware action. Publication was performed separately and is supported by the receipts above.

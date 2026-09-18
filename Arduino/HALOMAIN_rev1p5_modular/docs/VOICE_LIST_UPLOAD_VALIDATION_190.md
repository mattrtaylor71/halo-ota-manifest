# Voice while browsing the shopping list — candidate 190

Current status, September18: **Sense190/app1 SDK VALID passed the scoped voice/list and urgent-camera device checks.** LCD188/app0 SDK VALID is unchanged. Public OTA remains188;190 is not published. Spoken-item semantics and physical gesture geometry remain unqualified.

## September18 focused device acceptance

The original source and sealed binaries below were used without new firmware edits. Fresh identity, inactive Sense-only service/readback and LCD-forwarded SDK health passed. Sense189 fallback, LCD188, NVS, quota/debt and02:00Pacific configuration were preserved.

The successful case captured196,608 bytes from the real microphone with no speech playback. Its single POST returned202 in5.408seconds while the LCD list stayed lit and accepted five scroll events during the POST. Over30seconds it accepted8refreshes and28scroll events. A fresh post-accept list response returned200 in642ms. Both boards then slept, followed by20seconds quiet. Exact session/device/time/byte cloud correlation independently confirmed acceptance and processing completion.

This tests transport/list scheduling using injected UI/console events, not physical gesture geometry or spoken-item insertion. The separate speaker-based attempt stopped when Mac afplay timed out before list entry; recording finalized and its empty transcription is retained as a failed semantic test. Original Mac volume19/unmuted was subsequently restored and verified.

The later `camera-interrupt190-manual001` case passed after a manual wake. Camera input interrupted the actual POST of voice job28 (98,304 bytes); the same job was parked with `new_user_input`, resumed and received a validated202. Camera job54 captured141,508 JPEG bytes in1,648ms and returned PUT200. Both boards slept, followed by20seconds quiet and clean capture closure. Independent `cloud-camera190-manual001-discovery/RESULT.json` matched the exact voice job, reported completed. This does not establish spoken-item insertion or a semantic food effect.

After the camera upload, `[DMA_RESERVE][WARN] re-acquire FAILED by=presign_post` reported a9,716-byte largest block against the16,384-byte reserve. A further camera in that wake was not tested. [The same warning on188](/Users/MattTaylor/halo-manual188-20260917/functional-user001/sense.raw:428) reported10,228 bytes; this is a retained limitation, not a newly demonstrated190 regression.

[Closed scoped acceptance](/Users/MattTaylor/halo-voice-list189-20260917/MVP190-DEVICE-ACCEPTANCE-20260918.json), SHA256 `2e7180e8cd8377cc48ad597f91fe7ac3dd4eb74882d1104c42885dab14dca093`, binds the camera overlap, exact-job custody, image result, sleep and limitations. The earlier [transport checkpoint](/Users/MattTaylor/halo-voice-list189-20260917/MVP190-CHECKPOINT-20260918.json), SHA256 `37585b7915c07d0dce770d70e957c75599ee8c7b05c686b1870d415ad8d4148f`, preserves installation, the successful list case and failed fixture/actuator attempts. The initial camera attempt opened neither board; it is not the passing case.

**Do not rerun the historical189-to190 service chain below on this updated unit.** No further firmware changes are needed for the tested handoff. Public190 publication remains separate; no new scheduled OTA campaign is implied by this narrow acceptance.

## Intended behavior and limits

A fresh voice at the FIFO head, or the existing parked head, may upload while the shopping list remains open. List browsing does not repeatedly cancel that upload. List fetch/delete and voice transport serialize their complete HTTP/DMA ownership; refresh requests coalesce into one pending refresh. There is no new queue, queue reordering or bypass of an older image. Saved retries retain their existing foreground priority rules.

The special fresh-voice attempt requires connected Wi-Fi, fresh clock and no capture, recording, foreground job, provisioning or OTA owner. It gets one attempt and an original 12-second logical budget, including cancellable HTTP-lock acquisition. Scrolling cannot renew that budget. SDK blocking boundaries mean this is not a proved 12-second wall-clock maximum. Urgent camera selection, new recording, manual OTA and Wi-Fi/reset actions still cancel; the exact unaccepted job is parked for later recovery. Timeout-only/offline failure uses existing durable custody. A validated positive acceptance wins a late interruption and must not duplicate completed custody.

HTTP 202 acknowledges asynchronous cloud custody; it does not prove transcription, a shopping-row mutation or immediate list visibility. A later refresh may still be needed. Purely local menu navigation is not itself a guaranteed upload-cancellation signal; the subsequent resource-using action is.

## Why 189 needed correction

The closed physical `voice-list002` capture on Sense 189 showed fresh voice job 11 (194,560 bytes) starting its list-mode POST, then receiving the LCD's actual `INPUT_USER_ACTIVE` notification at Sense uptime 12,201ms. HTTP returned `-3` and the same job was parked. It later resumed and received 202; the intended uninterrupted upload while browsing had failed.

The earlier compatibility list covered `INPUT_SCROLL`, `INPUT_WAKE` and `INPUT_TOUCH`, but omitted the common activity notice actually emitted by the LCD scroll/wake path. 190 adds `INPUT_USER_ACTIVE` **only while the list is active**. Ordinary activity generation, saved-retry pause and sleep cancellation remain intact. Explicit urgent actions still increment the interrupt generation even if the list-active flag is stale. See private `voice-list002/sense.raw` lines 131–177 and 504–547; no successful shopping mutation is inferred from those transport logs.

## Exact source and verification

All private paths below are relative to `/Users/MattTaylor/halo-voice-list189-20260917/`.

- Source: `ffe52bf27865645ec0ff2a000b51049b40bcd5b1`; firmware tree: `94b08341ceccc8e7d05b78281d712cdd531cbf90`.
- Build ID: `6.4.190-20260918T025500Z-ffe52bf27865`.
- Sealed pair: `candidate190-001/RELEASE-PAIR.json`, SHA256 `4bf26cdba58a51bf954b57e28b7f4a89521e44516c91ba6b89818fef8196a060`.
- Full snapshot gate: `candidate190-001/host001/RESULT.json`, **97/97 PASS**, source unchanged, SHA256 `75a4182206d719b2a985929336662271774319077a7e993f3a20572dd53c37ad`.
- Actual-function workflow regression: **496 assertions / 73 scenarios**, including the compiled LCD scroll/wake producer, actual Sense admission and urgent capture after compatible activity. Repeated scroll, refresh coalescing, FIFO/parked precedence, mutex contention, deadline rollover, accepted-response races and durable fallback are covered. ASan/UBSan passed. Both mutation controls fail the specific `lcd_activity_preserved_voice` assertion; unrelated failures do not count. Snapshot receipt: `candidate190-001/host001/test_voice_list_workflow/RESULT.json`, SHA256 `f5f9c810fb8f22527b03f5036a844cc6f062b0891ea6c8faf25696f2090e8ded`.
- The frozen 189 source separately reproduces that same assertion: `wire-activity190-old189-negative001/RESULT.json`, SHA256 `49df97e0f0ff112c932b6a222c798dafc28cd8ef44a73d2d609a50087b7af907`.

`candidate190-001/SOURCE-COMPARISON.json` verifies exactly nine Sense runtime files changed from 188: `Sense_Minimal.ino`, `sense_list.h`, `sense_list_transport.h` (new), `sense_media_retry.h`, `sense_media_retry_client.h`, `sense_op_queue.h`, `sense_upload.h`, `sense_user_activity.h` and `sense_voice.h`, all under `Sense_Minimal/`. The other three production-source differences are generated version headers. LCD and OTA runtime are unchanged. Host SDK/network/storage doubles do not establish physical radio, flash, display, power or cloud behavior.

## Build and resource evidence

Both canonical builder executions completed and reaped successfully: Sense 68.885s, LCD 78.467s. Exact commands, compiler logs and results remain in `candidate190-001/build/{sense,lcd}/`; artifact checks passed in `build/artifact-result-v2-sense-lcd.json`.

| Board | Binary bytes | Verified OTA slot | Slot remaining | Compiler static globals |
| --- | ---: | ---: | ---: | ---: |
| Sense 190 | 1,866,544 | 1,966,080 | 99,536 | 159,368 bytes |
| LCD 190 | 2,030,288 | 2,621,440 | 591,152 | 204,356 bytes |

Sense static globals increased 24 bytes from 188; LCD globals are unchanged. Compiler globals and reported local-variable remainder are not measured runtime free heap, contiguous DMA capacity or task-stack headroom. Use the verified partition slots above, not the compiler's generic program-storage maximum.

The initial 3 GiB CLI request was rejected because the canonical CLI requires an integer reserve of at least 4 GiB. Root explicitly approved a measured **2.5 GiB reserve** through an external adapter calling unchanged canonical `run()`. No source, SDK, flags, build timeout or cleanup behavior was changed. `candidate190-001/BUILD-BUDGET-EXCEPTION.json` records this exception, its pinned launcher/builder and per-board free space; SHA256 `ce9e1e814e31719593748cc9c069cfaab165892b321c54af136b9d8eb23d629e`. The earlier `DISK-PLAN.json` is retained as the superseded proposal, not evidence of an ordinary CLI build.

Space recovery is separately recorded in `object-cache-compression001/RESULT.json` and `object-cache-compression002/RESULT.json`: historical compiler `.o/.d` bytes were archived and verified before removal (673,624,080→188,451,657 bytes and 1,258,305,717→367,022,334 bytes). Sources, binaries, ELF/map/static-library/stack-usage files, logs, device backups and receipts were preserved. These were reversible cache compression operations, not deletion of release evidence.

## Historical pre-installation checkpoint and resume

`CHECKPOINT190.json`, SHA256 `7f4809a1ecf1ae7af4b14d0b7ca052830e935f866356fc93d920a9eb08539421`, closes three fresh-identity attempts (`identity189mixed-001` through `003`). All had zero Halo USB opens and zero actuator strokes; the actuator did not answer HELP. **No190 service ran.** Last confirmed installation remains Sense189 app0 and LCD188 app0, both SDK VALID. All hardware owners are absent; no automatic retry loop is running.

Relay009/010 proved detach/return but subsequent HELP failed. A targeted actuator USB hub-port off/on returned successfully without observed BSD disappearance. The first DTR diagnostic hung on its initial clear ioctl; its parent could not reap within three seconds, and later process inspection confirmed the child absent. The second complete DTR reset/release closed cleanly but received no HELP. A bounded, non-writing AVR bootloader probe also failed to obtain a signature. There was no actuator reflash. See [the recovery procedure and limitations](ACTUATOR_RELAY_RECOVERY_20260917.md); do not confuse USB enumeration with sketch readiness.

The earlier exact test PCM was independently matched to cloud bytes/hash: after the initial transient its AC signal was −61.2dBFS and the transcriber returned no speech. `golden-audio-audit001/RESULT.json` supports an acoustic/capture limitation, without deciding whether speaker placement, microphone or capture code caused it. No fixture shopping row was created; no cleanup mutation was needed.190's prepared test increases temporary speaker volume to80 and restores the original setting, but still needs actual semantic verification.

After Halo is awake (or the actuator is recovered), use the prepared immediate identity→service chain with new output paths. It validates exact189/188 builds and idle Home, then runs the pinned esptool environment, inactive Sense-only write/readback and release. It preserves current189 fallback and LCD188/NVS. Never reuse stale identity or open a new Sense reader before190 SDK VALID.

```sh
cd /Users/MattTaylor/halo-voice-list189-20260917
/Library/Frameworks/Python.framework/Versions/3.8/bin/python3 -B tools/identity_then_service190.py \
  --target-pair candidate190-001/RELEASE-PAIR.json \
  --target-pair-sha256 4bf26cdba58a51bf954b57e28b7f4a89521e44516c91ba6b89818fef8196a060 \
  --installer-sha256 5ed58690162cf4d1632688d53818a9c1468d91a56228839ea3ea1c38945e343a \
  --identity-out /Users/MattTaylor/halo-voice-list189-20260917/identity189mixed-004 \
  --service-out /Users/MattTaylor/halo-voice-list189-20260917/service190-004 \
  --inputs-receipt /Users/MattTaylor/halo-voice-list189-20260917/service190-inputs004.json
```

Only after actual successful service, `tools/health190_then_main.py` takes `--service-result <actual service/result.json> --out <new health directory> --main-out <new voice-list directory>`. It requires SDK VALID using the original boot receipt or LCD-only diagnostics, closes that owner, and immediately starts the bound main test. Its SHA256 is `dce8af1675b0bc3f761554e526784a182b8ace2cd864bdddd5e4e3925f958740`; main controller SHA256 is `f67802126b26ffeba2baae09a18f23c78399da317f71d9bb8eb385d25c1c4b59`. Verify these and review their receipts before execution. Run the separate prepared camera-interruption case after the main case closes. No existing case gets overwritten or silently rerun.

## Device acceptance status

- Controlled installation and exact running partition/SDK VALID identities: **passed September18**.
- Voice/list transport with visible browsing/refresh, exact voice acceptance and independent cloud evidence: **passed September18**; spoken-item insertion remains unqualified.
- Urgent camera interruption, exact voice custody/resume, camera acceptance and return to paired sleep: **passed September18**, with the DMA-reserve/next-camera limitation above.

No full-product, physical touch-geometry, new provisioning, paired OTA transfer or scheduled-OTA qualification is claimed by this scoped device acceptance.

## Subsequent physical user voice test

A later physical voice request, "Add gummy worms to my shopping list," was matched to backend insertion, LCD list display and the user's deletion. Its automatic saved retry received an accepted response with one cloud worker execution and dark LCD logs. See `/Users/MattTaylor/halo-voice-list189-20260917/MANUAL-USER190-FIRST-VOICE-REVIEW.json`; this is one manual case, not three repeated cases.

The LCD acknowledgement initially timed out, but the exact request reached the backend. The request was preserved on the LCD and replayed automatically; the accepted retry then cleared the saved copy. Voice release to list render was 26.323 seconds. The exact cloud row remained removed after replay, and the worker execution count remained one. This supplements the earlier quiet-fixture acceptance without replacing its immutable receipt or claiming three manual repetitions.

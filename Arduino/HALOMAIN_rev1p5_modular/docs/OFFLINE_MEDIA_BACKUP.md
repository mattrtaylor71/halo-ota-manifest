> Current follow-up: [OFFLINE_MEDIA_STRESS_167.md](OFFLINE_MEDIA_STRESS_167.md). Continue development from the committed167 source; its physical stress acceptance is pending. The163 acceptance below is historical scoped evidence.

# Offline media backup163 — scoped accepted working source

The accepted working source is exact private candidate003, commit `4cdefb2b413930dfb68624640272154aeaf0b2fc`, build `6.4.163-20260915T215252Z-4cdefb2b4139`. It retains162 camera recovery,159 LCD wake handling and158 OTA safeguards. Canonical artifacts and22 host regression groups passed; scoped physical/cloud acceptance and final paired SDK VALID health are pinned below. Start future development here or from reviewed descendants. Public OTA remains162; no163 publication is claimed.

## Intended behavior

Failed uploads and pending jobs at sleep transfer to the LCD SD card. Voice uses `/voice-spool-v1`; new photos use `/image-spool-v1`. Legacy `/spool` files remain untouched and its unsafe replay remains disabled. Metadata and payload checks, commit-last writes and readback precede custody acknowledgement. Capacity is40 request stems per namespace,512KiB per item, with no silent eviction. Corrupt, expired or differently owned files remain held.

A connected idle wake can queue at most one saved media item for retry; there are no added timer wakes. New actions and ongoing OTA exclude replay. The original SD copy remains until verified backend custody, and a failed deletion is retried idempotently on a later wake. Cloud custody is not a claim that food recognition or transcription/list processing succeeded.

Voice retains owner/device/session/request identity. Photo identity binds owner/device/request, full SHA256, byte count and action options. Photo presign is create-only;412 requires backend HEAD proof of the exact admitted object. An uncertain response never allocates a replacement identity. The voice idempotency update is deployed; durable image activation is restricted to the verified test owner as described under Rollout dependencies.

## Local diagnostic

Direct Sense USB only: `backupoffline <32 lowercase hex nonce> <seconds>` accepts1–180 seconds while idle. Matching nonce with0 resumes. It disconnects only this device, changes no stored credentials, refuses renewal/busy admission and restores auto-reconnect on expiry; all state is RAM-only and reboot clears it. This exercises a deliberate local disconnection, not a claim of fridge/RF-loss qualification. LCD USB read-only `voicequeue`/`imagequeue` report actual card inventory without media contents. Existing actuator performs wake taps only.

## Acceptance status

Current scoped acceptance: `PASS_SCOPED_OFFLINE_MEDIA_BACKUP_163`.

On exact candidate003, a 309,248-byte voice note and a 117,633-byte Dish photo were captured with Sense Wi-Fi deliberately disconnected, saved to LCD SD, retained across both-board deep-sleep resets, delivered with original identity, and retired after cloud custody. Independent exact cloud records confirm both; the photo HEAD confirms SHA256 and length, and the voice job confirms audio SHA with HEAD length/identity. Voice encountered a real TLS failure and recovered through the existing Wi-Fi reset/retry. Photo has a complete captured save/restart/fetch/delivery trace. Voice lacks the initial LCD sleep-entry and fetch prefix; subsequent reset8/changed boot identities, cloud custody and deletion are observed. The final fresh wake found both queues empty with no incomplete/corrupt records and both boards SDK VALID, followed by paired sleep.

Foreground touch arbitration is covered by actual-source host tests; physical captures used LCD application commands, and the actuator provided wake taps. These receipts distinguish those checks and preserve the voice trace gaps.

- `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-backup-20260915/ACCEPTANCE.json` — SHA256 `13156739178aeee20d4c506cd451513c94c7f4433c18fd0deddd1d6f5112d926`.
- `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-backup-20260915/INSTALLATION-r2.json` — SHA256 `9276bcf13aa54abdaaccf4e2a14a65ec9e939cd44b63eadea3bb6736a26e4637`.
- `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-backup-20260915/FINAL-HEALTH.json` — SHA256 `7d9f3bb05e0a7ac7fd66d710da1a35050dd91ee76803a364a63a3ba0c6581ccb`.

### Earlier candidate002 evidence (historical)

The initial private163 build `6.4.163-20260915T205850Z-12921914559e` passed19 host regression groups and canonical artifact checks. Real tests disconnected only Sense Wi-Fi and captured two voice notes (133120 and311296 bytes) plus one116801-byte Dish photo. Each committed to LCD SD, survived ordinary deep sleep/reboot, uploaded on the next connected wake with its original identity, and was deleted only after backend custody. Exact cloud records match all three; photo S3 HEAD includes the matching SHA256, while voice SHA256 is bound by the strong job record and HEAD independently confirms length/identity. Both voice jobs completed once and the photo job reached DONE. A final wake found both typed namespaces mounted with pending=0, incomplete=0 and corrupt=0, followed by paired sleep.

The longer note took36.6 seconds to save. At the30-second sleep-flush deadline, the active transfer correctly blocked sleep until SD commit. This is ordinary deep-sleep/reboot durability, not an abrupt power-cut or USB-free test. Actuator provided wake taps; captures used LCD application commands.

Final review found a separate interaction problem: real camera requests remain queued during the binary transfer, but voice press/release edges can reach Sense too late while the LCD has already shown capture feedback. Direct diagnostic sends can also be suppressed. The follow-up arbitrates UI admission before showing voice feedback, interrupts background replay through a bound cleanup handshake, and retains a busy-refused RAM capture while main-task input continues. An atomic parked-job claim prevents worker and sleep rescue from owning the same buffer. Missing replay cleanup proof retains UART quarantine but releases transfer sleep custody at the original90-second deadline. Candidate003 adds22 passing host regression groups; its separate installation and scoped physical acceptance are the current receipts above. The initial candidate002 backup passes remain historical evidence, not substitute qualification of the follow-up.

Evidence directory: `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-backup-20260915`. Initial artifacts are `candidate163-002/RELEASE-PAIR.json`; installation is `installations/INSTALLATION.json`; physical receipts are `physical-voice001`, `physical-image001`, `physical-voice002`; independent cloud receipts are `CLOUD-VOICE001`, `CLOUD-IMAGE001`, `CLOUD-VOICE002`; final inventory is `final-empty-wake001`.

## Rollout dependencies

The async voice backend fixes are deployed. Durable photo admission and checksum/length reconciliation are currently enabled only for the verified test owner, with corresponding owner-prefix S3 read permissions. Other owners receive a retriable503 and retain their backup. Expand and validate that backend rollout before publicly publishing this firmware; see the evidence directory's `ROLLOUT-DEPENDENCIES.md`. Public manifests were still162 at21:24:57UTC on September15.

Legacy `SD_HEALTH` warnings from the disabled photo implementation do not measure the new typed stores. Actual typed inventory/commit results above are the relevant SD evidence. Legacy files remain untouched.

# Offline media backup integration (unqualified candidate)

Starts from accepted162 (`519e7d0`), retaining the camera input recovery,159 LCD wake handling and158 OTA policy. Integrates corrected voice candidate153e553 plus a separate checked image queue. The first private163 candidate is installed on both boards and SDK VALID. Public OTA remains162; no163 publication has occurred. The accepted source record remains162 until the foreground-interaction follow-up below is qualified.

## Intended behavior

Failed uploads and pending jobs at sleep transfer to the LCD SD card. Voice uses `/voice-spool-v1`; new photos use `/image-spool-v1`. Legacy `/spool` files remain untouched and its unsafe replay remains disabled. Metadata and payload checks, commit-last writes and readback precede custody acknowledgement. Capacity is40 request stems per namespace,512KiB per item, with no silent eviction. Corrupt, expired or differently owned files remain held.

A connected idle wake can queue at most one saved media item for retry; there are no added timer wakes. New actions and ongoing OTA exclude replay. The original SD copy remains until verified backend custody, and a failed deletion is retried idempotently on a later wake. Cloud custody is not a claim that food recognition or transcription/list processing succeeded.

Voice retains owner/device/session/request identity. Photo identity binds owner/device/request, full SHA256, byte count and action options. Photo presign is create-only;412 requires backend HEAD proof of the exact admitted object. An uncertain response never allocates a replacement identity. Image backend activation and the voice idempotency update must be deployed before enabling this candidate.

## Local diagnostic

Direct Sense USB only: `backupoffline <32 lowercase hex nonce> <seconds>` accepts1–180 seconds while idle. Matching nonce with0 resumes. It disconnects only this device, changes no stored credentials, refuses renewal/busy admission and restores auto-reconnect on expiry; all state is RAM-only and reboot clears it. This exercises a deliberate local disconnection, not a claim of fridge/RF-loss qualification. LCD USB read-only `voicequeue`/`imagequeue` report actual card inventory without media contents. Existing actuator performs wake taps only.

## Acceptance status

The initial private163 build `6.4.163-20260915T205850Z-12921914559e` passed19 host regression groups and canonical artifact checks. Real tests disconnected only Sense Wi-Fi and captured two voice notes (133120 and311296 bytes) plus one116801-byte Dish photo. Each committed to LCD SD, survived ordinary deep sleep/reboot, uploaded on the next connected wake with its original identity, and was deleted only after backend custody. Exact cloud records match all three; photo S3 HEAD includes the matching SHA256, while voice SHA256 is bound by the strong job record and HEAD independently confirms length/identity. Both voice jobs completed once and the photo job reached DONE. A final wake found both typed namespaces mounted with pending=0, incomplete=0 and corrupt=0, followed by paired sleep.

The longer note took36.6 seconds to save. At the30-second sleep-flush deadline, the active transfer correctly blocked sleep until SD commit. This is ordinary deep-sleep/reboot durability, not an abrupt power-cut or USB-free test. Actuator provided wake taps; captures used LCD application commands.

Final review found a separate interaction problem: real camera requests remain queued during the binary transfer, but voice press/release edges can reach Sense too late while the LCD has already shown capture feedback. Direct diagnostic sends can also be suppressed. The follow-up arbitrates UI admission before showing voice feedback, interrupts background replay through a bound cleanup handshake, and retains a busy-refused RAM capture while main-task input continues. An atomic parked-job claim prevents worker and sleep rescue from owning the same buffer. Missing replay cleanup proof retains UART quarantine but releases transfer sleep custody at the original90-second deadline. This correction requires its own focused regression and installation evidence before accepting163 as the current working source. Do not mistake the initial backup passes for qualification of that follow-up.

Evidence directory: `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-backup-20260915`. Initial artifacts are `candidate163-002/RELEASE-PAIR.json`; installation is `installations/INSTALLATION.json`; physical receipts are `physical-voice001`, `physical-image001`, `physical-voice002`; independent cloud receipts are `CLOUD-VOICE001`, `CLOUD-IMAGE001`, `CLOUD-VOICE002`; final inventory is `final-empty-wake001`.

## Rollout dependencies

The async voice backend fixes are deployed. Durable photo admission and checksum/length reconciliation are currently enabled only for the verified test owner, with corresponding owner-prefix S3 read permissions. Other owners receive a retriable503 and retain their backup. Expand and validate that backend rollout before publicly publishing this firmware; see the evidence directory's `ROLLOUT-DEPENDENCIES.md`. Public manifests were still162 at21:24:57UTC on September15.

Legacy `SD_HEALTH` warnings from the disabled photo implementation do not measure the new typed stores. Actual typed inventory/commit results above are the relevant SD evidence. Legacy files remain untouched.

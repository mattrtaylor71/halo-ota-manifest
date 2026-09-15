# Offline media backup integration (unqualified candidate)

Starts from accepted162 (`519e7d0`), retaining the camera input recovery,159 LCD wake handling and158 OTA policy. Integrates corrected voice candidate153e553 plus a separate checked image queue. Current installed/public firmware remains162 until installation and publication receipts say otherwise.

## Intended behavior

Failed uploads and pending jobs at sleep transfer to the LCD SD card. Voice uses `/voice-spool-v1`; new photos use `/image-spool-v1`. Legacy `/spool` files remain untouched and its unsafe replay remains disabled. Metadata and payload checks, commit-last writes and readback precede custody acknowledgement. Capacity is40 request stems per namespace,512KiB per item, with no silent eviction. Corrupt, expired or differently owned files remain held.

A connected idle wake can queue at most one saved media item for retry; there are no added timer wakes. New actions and ongoing OTA exclude replay. The original SD copy remains until verified backend custody, and a failed deletion is retried idempotently on a later wake. Cloud custody is not a claim that food recognition or transcription/list processing succeeded.

Voice retains owner/device/session/request identity. Photo identity binds owner/device/request, full SHA256, byte count and action options. Photo presign is create-only;412 requires backend HEAD proof of the exact admitted object. An uncertain response never allocates a replacement identity. Image backend activation and the voice idempotency update must be deployed before enabling this candidate.

## Local diagnostic

Direct Sense USB only: `backupoffline <32 lowercase hex nonce> <seconds>` accepts1–180 seconds while idle. Matching nonce with0 resumes. It disconnects only this device, changes no stored credentials, refuses renewal/busy admission and restores auto-reconnect on expiry; all state is RAM-only and reboot clears it. This exercises a deliberate local disconnection, not a claim of fridge/RF-loss qualification. LCD USB read-only `voicequeue`/`imagequeue` report actual card inventory without media contents. Existing actuator performs wake taps only.

## Acceptance status

Host tests are in progress. Canonical build, real SD mount, offline voice/photo custody, restart survival, actual cloud receipt, queue retirement and final sleep remain required. Do not label this candidate accepted or replace the current working-source record from host results alone. Evidence directory: `/Users/MattTaylor/halo-device-analytics-2026-09-10/offline-backup-20260915`.

# Voice SD queue candidate

This adds an isolated voice queue to the reviewed 159 firmware source. Frozen 158
and published 159/160 artifacts remain unchanged. Building this source is not
evidence that the assembled unit's SD card works; physical mount, store/reboot,
replay and sleep validation are required before claiming device durability.

The new `LCD_VOICE_SPOOL_ENABLED` candidate default is 1. The legacy
`LCD_SD_SPOOL_ENABLED` photo fallback stays 0. Voice operations reuse the reviewed
task-level SD lease and mount lifecycle, refuse OTA/sleep-commit overlap and
never format the card. They do not scan, replay or clear the old photo namespace.

`halo_common/VoiceSpoolStore.h` is the Arduino-free storage implementation.
`LCD_Minimal/lcd_voice_spool.h` adds the typed UART controls and SD lease/mount
wrapper. Existing image/spool ownership flags cover voice transfers too, so
ordinary UART JSON, diagnostics and both sleep routes remain excluded while a
binary transfer owns the link. No panel or LVGL operation is added to UART.

## Stored records and commit

The private namespace is `/sdcard/voice-spool-v1`. Each 32-character lowercase
hex request ID names its PCM file and metadata. The metadata has an explicit
273-byte little-endian encoding, version magic and its own CRC32. It binds
owner, device, session, request, job, byte count, whole-audio CRC32, 16 kHz signed
16-bit mono format, epoch, retry count and LCD-local 64-bit commit ordinal.

An inbound recording first creates synced `.meta.part` and `.part` files. Only
after exact length, sequence and CRC validation does it sync/close the payload,
read back its CRC, rename it to `.pcm`, assign the next committed ordinal and
sync/close metadata. Renaming metadata to `.meta` commits the record last. A
final reopen and complete CRC verification precedes the storage ACK. A same
bound request can restart its own interrupted partial; an existing committed or
conflicting record cannot be overwritten. Partial/corrupt files remain unlisted
and available for diagnosis. There is no automatic clear, format or eviction.

The cap is 40 occupied request stems, each at most 512 KiB. Invalid/partial stems
also consume capacity. Full storage rejects the new recording explicitly.
Committed records replay in local commit order rather than random request-ID
order. A cursor can skip a held record without deleting it or changing order.

For a recording created with epoch 0, `VOICE_SPOOL_ATTEMPT` can commit its first
trusted attempt epoch in a separate `.attempt` record. This avoids relying on
rename-over-existing behavior in FATFS. The original metadata stays immutable;
the marker is CRC checked and bound to the same request, owner and payload.
Repeating its same epoch is idempotent; replacing it with a different epoch is
refused. The Sense owns trusted-time and replay-age admission. The LCD does not
delete old unacknowledged recordings.

## Wire and diagnostic controls

All voice JSON controls require `voice_schema:1`. `VOICE_XFER_BEGIN/READY` and
`VOICE_SPOOL_LIST_REQ/LIST`, `FETCH/FETCH_READY`, `ATTEMPT/ATTEMPT_ACK`, and
`DELETE/DELETE_ACK` are separate from legacy photo controls. Original firmware
cannot acknowledge these capabilities or discover PCM through its photo list.

Binary payloads use existing IMG COBS/CRC16 frames only after a typed correlated
READY. The final committed ACK and replay END contain 40 bytes: little-endian
length, little-endian audio CRC32 and the 32-byte request ID. Sequence, content
proof and request correlation are all required. A duplicate committed BEGIN
returns `stored:1` and does not switch to binary mode.

Matching `VOICE_XFER_ABORT` is accepted through the binary/JSON demultiplexer
and in ordinary mode after a lost response. Its typed `json_ready` response
follows file closure and ownership release. It cannot clear another voice
request or an OTA owner. SD scans have a 12-second work budget; the receiver has
a 90-second absolute transfer limit and 4-second idle limit. Limits do not
establish physical SD performance or power-loss guarantees on an untested card.
The work budgets are checked between storage operations; they cannot preempt a
blocked SDK mount, FATFS call or existing maintenance lock. The existing freeze
watchdog remains the last resort for a stalled task. Card behavior still needs
the finite physical validation above.

The LCD USB command `voicequeue` performs a guarded read-only mount/inventory
and reports feature/schema, mount result, committed/partial/corrupt counts and
elapsed time. It prints no owner identifiers or recording content, creates no
test jobs and exposes no deletion command.


## Sense retry policy and cloud custody

The Sense freezes account, device, session, random request ID and the original
PCM checksum before the first HTTP request. Failed uploads offer that exact
recording to the LCD queue. The existing single-record SPIFFS store is a backup
when the recording fits; voice cannot replace another pending recording or
photo. Its V3 metadata has a whole-record checksum, checked before replay
identity or time is used, as well as the PCM checksum. Legacy voice records without account/request identity remain retained
for diagnosis rather than being replayed into a potentially different account.

Retries run on an eligible connected, idle wake. This feature adds no timer
wake and changes neither the daily 2 a.m. schedule nor OTA allowance. A queued
recording keeps its original identity when its acknowledgment is lost. A
recording with no trusted time gets a durable first-attempt marker before it
can be uploaded. The marker cannot be renewed on later retries. The retry
horizon is seven days; expired, corrupt or differently owned records remain
held. Storage exhaustion is an explicit failure, never silent eviction.

HTTP success alone is insufficient to remove the local copy. The response must
be HTTP 202, contain true `accepted` and `async` booleans, and return the expected
SHA256 job ID derived from the frozen owner/device/session/request tuple.
Duplicate receipts also need an accepted, enqueued, processing or completed
state. This proves server custody; it does not prove that the assistant has
successfully changed the shopping list.

The accompanying backend candidate prevents overlapping worker execution,
checks immutable audio, and repairs acceptance-before-delivery failures. It
retains failed or ambiguous processing for review because a crash may occur
after a list change. It does not guarantee exactly-once SQL changes across
arbitrary crashes. Deploying and verifying that reviewed backend candidate is
an acceptance prerequisite for automatic replay. Its preparation and tests do
not imply that it has been deployed.

## Finite acceptance checklist

- Host storage tests: commit/restart, maximum recording, partial writes,
  disk/full and I/O failures, corruption, duplicate acknowledgments, immutable
  attempt markers and FIFO cursor behavior.
- Host protocol tests: exact receipt identity, bounded metadata, wrong request,
  stale frame, CRC mismatch, abort, timeout and exclusive UART ownership.
- Existing OTA, user-wake, flash/sleep exclusion and production-profile tests.
- Canonical builds of both boards from committed source, preserving the
  partition layout, OTA policy flags and previous recovery firmware.
- On this unit: guarded SD mount, failed-network voice capture, confirmed local
  custody, restart, reconnect, one cloud job for the original request, removal
  only after acceptance, and normal sleep/wake afterward.

Host tests and successful compilation do not substitute for the last item.
Release evidence must record which of these checks actually ran and preserve
any pending physical or backend acceptance separately.

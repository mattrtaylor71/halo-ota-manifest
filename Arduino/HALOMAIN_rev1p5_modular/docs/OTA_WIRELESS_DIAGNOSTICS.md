# Optional wireless OTA diagnostics

This candidate adds an admission breadcrumb and a retained LCD sleep witness. It is based on the canonical production source and its real user-work guards. Installation, live credentials and private backend deployment require separate qualification; a compile alone is not a physical result. The accelerated 60–66 runtime remains a separate, older source lineage.

## Build boundaries

The breadcrumb and LCD witness flags default off. The volatile exporter trace and SHA detail use their existing diagnostic/error paths. An explicitly qualified wireless diagnostic build enables `HALO_DIAGNOSTIC_ADMISSION=1` on Sense and `HALO_LCD_SLEEP_WITNESS=1` on LCD, in addition to the canonical durable policy/diagnostic flags. Both shipping (`HALO_OTA_BENCH_PROFILE=0`, `HALO_OTA_ONE_SHOT=0`) and bench configurations are compiled separately. `HALO_DIAG_AUTH_PROVISIONING=1` additionally enables the local Sense setup transaction; leave it off when provisioning is not needed. Reading an already installed diagnostic key does not depend on that setup switch.

`HALO_DIAG_B1_URL` defaults empty. A configured URL must be HTTPS with the exact `/v1/private/ota/admission` suffix. It contains no credential. Private export requires an installed key and verified TLS. The existing canonical build command keeps its documented defaults and supplies no private endpoint or credentials.

The UI, pins, partition tables, policy record format, deadlines, quotas, target identity, rollback and scheduling decisions are unchanged. The optional witness cannot veto a fresh health proof, and neither breadcrumb availability nor export success is an OTA admission requirement.

## Admission breadcrumb

`ota_diag/admit_v1` is one 64-byte record. It is captured at an actual checked PREFLIGHT admission only when the ordinary journal State is busy because an exact prior-boot State is still unacknowledged. The hook rechecks the complete stored policy and retained Context/State/checkpoint authority under the existing NVS lease. It never overwrites State or either Failure slot. A missing authority, occupied/corrupt breadcrumb, unavailable storage, user work or exhausted original diagnostic budget declines the optional capture.

The record stores the full 32-byte Context SHA, actual uint32 Sense boot, observed epoch, immutable due, policy generation/ordinal, optional proven prior LCD boot, reset/wake and quality bits, format and CRC. It is its own `b1` admission type; it is not a fabricated historical d3 State. Context binds the target/campaign/device/owner but does not identify the captured base firmware. Readers must not fill a historical build identity from a later target boot.

The scheduled bit requires the actual accepted episode identity and fresh clock. The LCD quality bit additionally requires a validated typed peer witness and matching full window digest. Manual, unsupported, late, missing or inconsistent origins remain unproved. Record fields and unsigned epoch bounds are checked by the codec.

A matching pending breadcrumb delays only optional Context retirement, allowing later export with the same authority. It cannot delay device health, rollback, target progress or sleep. An older reader may retire that Context; a remaining unmatched breadcrumb is then diagnostic orphan/unproved evidence. It must never be used to prohibit rollback or silently acquire a new identity. An occupied orphan declines subsequent breadcrumb captures until a separately reviewed diagnostic cleanup.

## LCD sleep evidence

The existing accepted `MAINT_WINDOW` may carry an optional typed `sense_boot_id`. Absence or malformed content changes only witness quality. The ordinary acceptance, schedule and health reply remain compatible with old peers.

LCD retains 72 bytes in RTC memory and Sense retains a 64-byte exact-sent-window cache. The digest covers the real durable arm fields, actual request bytes, both prior boots and a fixed domain. No wire request is invented for calendar maintenance. Only the supported absolute maintenance timer selector can qualify; fallback, relative, imminent and unmatched selectors remain unknown.

The LCD capture records the actual timer SDK return before the final sleep boundary. A cancellation generation invalidates a pending entry without racing an RTC buffer write. Boot consumption clears eligibility on every boot. Only the actual deep-sleep timer predecessor, exact matching selected time/timer/due and changed boot can qualify. RTC is not evidence of continuity through power loss or cold/software reset.

One optional `OTA_DIAG op=sleep` read carries the original 72-byte witness, fresh nonce/current peer and typed reset/wake/quality. It does not alter the health reply. The worst measured escaped reply is 440 bytes, below the 512-byte UART bound. Sense reads once within the remaining original deadline (nominal maximum 250 ms, no extension). A late pump/reply, unsupported peer or malformed tuple remains unknown. The validated quality is persisted in the admission breadcrumb so later OTA software restarts do not erase the proof. The optional exchange is not an admission prerequisite.

## Storage and local setup

All capacity decisions use live `nvs_get_stats` under the same writer lease. The historical available-entry count from an earlier image is not current capacity. Sense retains a 96-entry floor and the full missing diagnostic profile plus a 27-entry policy replacement peak (54 entries when an allocated typed policy is not proved).

| Optional key | Bytes | New-blob entries |
|---|---:|---:|
| `admit_v1` | 64 | 4 |
| `auth_v1` | 32 | 3 |

Setup reserves any missing breadcrumb; breadcrumb capture reserves a missing auth key. The SDK split-page allocation rule and the larger serialized policy replacement peak are accounted for. Available entries do not guarantee flash allocation: an actual failed set/commit/readback remains a refusal or uncertain result. The setup transaction creates no namespace and changes no user, Wi-Fi, owner or legacy key.

The strict USB-only transaction has exactly `type`, `op`, `key_id`, `key_hex`, `nonce`, `sense_boot`, and `peer_boot`. Type is `OTA_DIAG_AUTH`, operation `install`. Key ID is `b1-` plus the full 12 lowercase hex bytes of the actual Ethernet MAC. The key is 32 bytes encoded as 64 lowercase hex characters; the nonce is 32 lowercase hex characters and both boots are nonzero uint32 values. Duplicate fields, escaping, coercion, extra fields, zero key, malformed and truncated frames refuse. The maximum supported command measured with both maximum-width boots is 234 bytes including LF.

A fresh nonce-correlated peer query must prove current READY/SDK VALID/selected-running/idle and the exact supplied peer boot; the local boot must match and remain SDK VALID/idle. The one transaction uses its original 10-second window. The key must be absent. There is one set/commit/readback, no overwrite, erase or automatic retry after uncertainty. A committed key remains durable if its acknowledgement is lost.

`OTA_DIAG_AUTH_ACK` reports only nonce, public key ID, status (`INSTALLED`, `DECLINED`, `UNKNOWN`), and actual capacity metadata: `stats_known`, `free_entries`, `available_entries`, `missing_profile`, `policy_reserve`, `breadcrumb_reserve`, `auth_entries`, `floor_entries`, `required_entries`. Counts are the prewrite held-lease sample; unknown counts are explicitly marked unknown. `INSTALLED` additionally requires exact key readback and the HMAC proof over `HALO_B1_PROVISION_V1\n` + nonce + `\n` + key ID, without a NUL. The successful tested reply is 379 bytes and uses bounded partial writes of at most 64 bytes, rather than requiring whole-message USB capacity.

With the feature enabled, every USB JSON payload preview is length-only. The secret frame is intercepted before the ordinary parser/logger. All malformed USB JSON is consumed before the ordinary head/tail error logger. Temporary key/raw buffers are wiped; the key is never echoed, placed in an OTA binary, or logged as an unsalted digest. Provisioning uses a separately reviewed non-recording original-handle host adapter and a local owner-only staging file. No live key or provisioning is part of the source/test packet.

## Later private export

The optional `b1` POST runs only after the existing State/Failure exporter, within the same original pre-sleep deadline and ordinary-report reserve. It does not introduce a pre-OTA network drain, wake, retry or quota grant. The fixed envelope is at most 1024 bytes (795 in the tested fixture); the response is at most 256 bytes. It requires exact Context/breadcrumb identity and an exact authenticated backend receipt before deleting only `admit_v1`. Failed, late, truncated, conflicting or unconfirmed responses leave the original record. The auth key is not deleted by journal retirement.

HMAC-SHA256 uses the exact prefix `HALO_B1_POST_V1\nPOST\n/v1/private/ota/admission\n` without a NUL followed by the exact envelope body. Headers are `X-Halo-B1-Key-Id` and `X-Halo-B1-Signature` (64 lowercase hex). Backend authentication and append-only receipt validation are separate private-route requirements; ordinary/d3/H4 schemas and latest-target metadata are unaffected.

The synchronous SDK DNS/TCP/TLS/header APIs do not provide a preemptible aggregate two-second bound. Existing deadline checks remain truthful and do not claim one. No synchronous network operation is added before apply. Empty endpoint, absent key, unavailable storage or export failure disables/defers only the optional diagnostics.

## Ordinary exporter trace and integrity detail

The original exporter now keeps a 48-byte volatile last-decision trace. Ordinary pre-sleep reports include `diag_export_decision`, `diag_export_attempted`, and remaining budget. A selected record adds the exact journal/sequence/CRC/slot and handoff flag. POST result appears only after a returned transport call; HTTP status only after an actual returned HTTP call; ACK is `committed` or `not_committed` only after the checked ACK opportunity. Absent fields mean unobserved, not zero success/failure. The trace distinguishes pre-sleep-budget, deadline, pending-boot, other busy, Wi-Fi, local journal, no-item, owner, envelope, POST and ACK decisions. It makes no inference about older boots and adds no NVS key or network request.

The self-applier SHA mismatch branch now snapshots the already accepted byte count and existing heap metrics as `SHA_CHECK` (stage 13) before abort/logging. The error result remains `FAILED_SHA256_MISMATCH`; no `esp_ota_end` or boot selection occurs. This is the existing 28-byte RAM failure detail and existing Failure capsule, with no new allocation or key. Startup 32768 and periodic 8192 heap guards compare internal free heap; largest block is a separate fragmentation observation.

## Qualification still required

Focused codec, authority, persistence, exact ACK, parser, lifecycle, actual handler and generated SDK syntax checks are saved in the isolated candidate packet. The full rich report fixture is 7,220 bytes and rejects allocation/serialization truncation. Source-owned frame checks do not establish SDK tail stack usage or physical low-water memory.

Before installation: review the complete canonical merge, private backend contract and secret setup; link both board/profile builds and assess actual static/RTC/stack deltas. Then qualify a fresh baseline for both SDK VALID/READY/Home, sleep/wake and fallback with private export unavailable. Install no credential without separate authorization. A later controlled failure/recovery must validate wireless admission/sleep evidence after software restarts, with missing/offline evidence still classified honestly. Shipping-calendar qualification remains a separate gate after explicit safe retirement of any active bench record.

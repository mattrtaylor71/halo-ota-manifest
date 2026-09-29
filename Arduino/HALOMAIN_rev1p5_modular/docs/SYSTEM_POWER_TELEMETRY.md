# System-rail cloud metadata coverage

September 28, 2026. Status: local firmware implementation and read-only serving-code audit. No backend deployment, live ingestion, media download, physical qualification or public OTA publication is established by this record.

The source measurement is the LCD's calibrated GPIO1/ADC1-channel0 system supply rail. It is not cell voltage, a charge detector or a qualified battery percentage. The measurement evidence and limitations are in `/Users/MattTaylor/halo-voltage-probe-20260928/handoff-20260928/FIRMWARE_HANDOFF.md`. Company context consulted: `projects/halo-firmware` and `engineering/halo-resource-testing`, historical resource/workflow evidence rather than current deployment state.

This telemetry does not implement a production percentage curve or a low-voltage shutdown threshold. The test handoff's proposed bench thresholds and discharge endpoints are not approved production thresholds.

## Contract

`X-Halo-System-Power` carries one compact JSON object, bounded to 511 value bytes plus NUL. `SystemPower.h` defines the schema and unknown/stale representation. The optional Sense hook copies an existing cached observation without requesting ADC work, UART work, storage, another wake or another cloud transaction. Missing or invalid formatter output falls back to the same model's explicit `peer_missing`/null object. Control characters and unterminated/truncated output cannot enter a header. Only fixed schema keys, integers, booleans, null and fixed enum strings belong in it; no credentials, owner IDs, media, list contents or claim codes are added.

This is a **transmission-time** snapshot. Saved-media replay may have a new reading; it does not pretend to measure the old capture. `sample_epoch_s` may be null. LCD-local age uses its own monotonic clock. Sense uses the greater of sender age and a conservative paired-clock epoch age (including one second of quantization), then advances that age monotonically. This includes serial queue delay. When paired clock evidence is missing or inconsistent, `age_ms:null`, `age_known:false` and `fresh:false` explicitly refuse a freshness claim; the measured value and sample identity remain available. The cell-voltage and percentage fields remain null; `power_source` remains unknown. A successful HTTP receipt does not verify voltage accuracy, charge state or physical battery safety.

Schema1 reports `measurement:"lcd_system_supply"`, integer/null `system_supply_mv`, `status`, `valid`, `fresh`, integer/null `age_ms` and `sample_epoch_s`, boolean `age_known`, LCD `sample_uptime_ms`, `sample_boot_id`, `sample_sequence`, conversion `samples`, and `raw_min`/`raw_max`. Failed/unavailable measurement produces null voltage; a previously valid but stale observation retains its measured voltage with `fresh:false` and explicit age. Status distinguishes not-sampled, ADC-unavailable, calibration-unavailable, read-error, saturation and missing-peer states. No percentage or charge state is inferred from this rail.

Ordinary device reports also carry the object as `system_power`, because the existing report backend retains unknown object fields. Header and body are sampled separately during their respective existing construction/send phases; their age/sample sequence may differ. Neither is silently relabeled as capture-time evidence.

## Active coverage and preservation

| Request | Firmware adapter | Existing body limit | Serving backend today |
| --- | --- | --- | --- |
| Image admission: check-in/grocery, dish, discard | `sense_upload.h::http_post_json_with_retries` | 2048 JSON bytes | Header ignored. Unknown top-level body fields ignored. |
| Image reconciliation after complete PUT412 | Same helper | 2048 JSON bytes | Header ignored; immutable request stays unchanged. |
| Fresh and saved voice | `sense_voice.h::voice_upload_and_parse` on each actual POST | Original raw PCM envelope and media bounds unchanged | Explicit header allowlist omits power; job and S3 metadata omit it. |
| Shopping-list view | `sense_list.h::fetch_shopping_list_from_api` on each POST | Existing String request | Header and extra body metadata ignored. |
| Shopping-list removal | `sense_list.h::delete_item_from_api` before POST | Existing String request | Header and extra body metadata ignored. |
| Provisioning claim | `ProvisioningClaimTransport.h::transport` before POST | 256-byte request slot; 768-byte response | Header ignored; only explicit device/owner/firmware/IP fields retained. |
| Ordinary pre-sleep report | `ota_report_build_payload` plus `SenseBoundedPost::write_request` | 8192-byte payload; 256-byte response | Nested `system_power` retained in event `payload` and ordinary `last_payload`; no power analytics projection or typed validator. Header ignored. |
| Retained diagnostic d3 / handoff h4 | `SenseBoundedPost::write_request` | 1536-byte envelope; 256-byte receipt | Header ignored. Frozen evidence body unchanged. |
| Conditional private admission b1 | Same writer | Firmware1024-byte body; server1536; exact schema | Header ignored, outside body HMAC. Frozen body/signature and duplicate identity unchanged. |

Both shopping-list operations are POST. The active image builder is `sense_image_contract_request` in `sense_image_upload.h`; legacy `sense_presign.h`/`get_presign_checkin` do not serve the active worker. They share the same JSON transport if used by a compatibility build. Every current image/voice replay eventually uses the active senders above, so no media record migration is needed for transmission-time telemetry.

Excluded: signed S3 JPEG PUT, OTA manifest/binary GET and Range, DNS/SNTP, TLS control traffic, inbound local provisioning responses and ordinary UART coordination. MQTT remains disabled; dormant truth/diag/pong code is not enabled for power telemetry. Power does not justify another cloud transaction or longer retry/sleep deadline.

## Identity and backend work boundary

The live presign function copies arbitrary `camera_meta` fields, but `upload_admission.py::admission_identity` hashes the complete normalized request, **including camera_meta**. Putting a changing voltage there causes HTTP409 when retry/reconciliation reuses an admitted operation. No power field is added to image bodies, camera metadata, upload-operation identity or raw media. A future body field requires an explicitly reviewed exclusion from the immutable admission hash or a frozen, versioned capture-time record.

Voice also binds retained audio to explicit owner/device/session/request/hash identity. The new header changes none of those fields. The current ingest can return early for already-processing/completed jobs, so a future requirement to retain every retry's header must run before that branch. Define whether retention means first observation, latest observation or attempt history before implementing it.

The private b1 server rejects any body key outside its exact `ENVELOPE_FIELDS` set and compares duplicate payloads with the first saved payload. Header metadata is not part of its HMAC attestation and must not be presented as authenticated historical evidence. Any backend retention extension must preserve this distinction.

Backend parsing/preservation for the ignored headers is **not implemented or deployed** here. Ordinary report retention already works by source inspection, but a live ingestion/readback test was not performed. A future bounded backend parser should validate the exact schema, numeric types/ranges, enum values and total size before storing observational metadata. Current Python/DynamoDB ingestion does not convert arbitrary nested floats to Decimal; this firmware uses integers/null.

The application API Gateway routes have `AuthorizationType: NONE`. Voice/list firmware currently uses `setInsecure()`. These existing limits mean power is observational telemetry, not authenticated device identity. No new authorization claim follows from adding a header.

## Read-only serving evidence

AWS route/integration and claim function-URL readbacks verified target mapping. Function ZIPs were downloaded into memory and checked against AWS CodeSha256 before inspecting source; environment variables were not printed or saved. No handler was invoked.

| Serving function | CodeSha256 | Last modified |
| --- | --- | --- |
| `trepo-grocery-backend-dev-OtaReportApiFunction-cyUvBswGXwDb` | `mg3OS74Of/onaqn/ozaBTmF/BGNNMcK46/PkmCK3iVg=` | 2026-09-12 |
| `trepo-grocery-backend-dev-PresignFunction-EOb8DRx0IhAC` | `UHOCo+aIbKfSiAsKtejcYGSsTZWX5Z0/GCZ6ATFnmQk=` | 2026-09-20 |
| `trepo-quick-ack-async-ingest-dev` | `yV29zd9pgV/6WMWmp48WJbRSH2s5wen2kgS5OYEkK0U=` | 2026-09-25 |
| `trepo-list-handler` | `7fKNHlQkrDbC0OWqaIJY+GMyp93jk6PSv7DxjVd/exU=` | 2026-09-27 |
| `device_claim` | `AL3+C/4e+RtNQYAC3zXVn6VV3FHi4qIflB2Pr4papO0=` | 2026-04-01 |
| `halo-private-admission-20260908-v2-b1` | `g/mbH+aQ0QNv5fKbYafDFUPiuloQ4YwQ2YuwZNaK92o=` | 2026-09-09 |

The current ordinary report `app.py` SHA256 is `891ed191022f10f17393e3d2dc08763c09a4666d6c889e6378d2afea243e0523`, matching the existing local `halo-cloud-protocols-20260920/telemetry-serving/app.py`. It preserves every normalized body field in event `payload`, updates ordinary `last_payload`, and excludes diagnostic exports from latest-state updates.

`verify_backend_scope.py` currently pins older voice ingest/worker hashes. Its prior passing inventory is not evidence that the newly serving September25 voice code is identical. This audit does not change that independent contract gate.

## Resource and verification scope

The HTTPClient adapter uses one 512-byte automatic buffer in a non-inlined short formatter frame. HTTPClient then owns one bounded header String through its original request lifetime, adding up to 511 value bytes plus header name/framing and allocator overhead. It adds no task, queue, persistent record or global mutable buffer. Cache copying must remain short and synchronized with its owner.

The bounded report writer keeps its original 768-byte header buffer and adds a separate 512-byte power buffer plus three additional pointer/size entries. It writes the header as separate fragments under the original request deadline, chunk limit and cleanup custody. Body Content-Length, diagnostic envelope, HMAC, response parsing and receipt retirement are unchanged. The ordinary nested report additionally owns one bounded object in its existing ArduinoJson document; allocation/serialization failure still refuses a partial report.

`tools/test_system_power_transport.py` compiles the actual formatter/adapter and actual bounded writer with partial/WANT writes and cancellation/timeout faults. It checks missing-hook fallback, control/truncation guards, a maximum-length header, stale output, exact body preservation across different readings, b1 signature preservation, Content-Length, 8192-byte body handling and required/excluded insertion points. Existing voice/media, list and provisioning suites exercise the affected callers. These are host tests with I/O doubles: no SDK/hardware responsiveness, cloud retention or battery measurement claim. Full source/snapshot regression gates, paired builds, resource deltas and finite device tests remain the release workflow's responsibility.

The LCD's local `GET_POWER` / `power` diagnostic emits a compact, versioned `[POWER1]` JSON line. Keys are `v` (schema 1), `b` (LCD boot ID), `q` (sample sequence), `s` (numeric `halo_power::Status`), `mv` (valid system-supply mV or null), `a` (known age in ms or null), `f` (fresh boolean), `ok` (valid boolean), `lo`/`hi` (raw extrema), and `n` (sample count). Status values are 0 not sampled, 1 OK, 2 ADC unavailable, 3 calibration unavailable, 4 read error, 5 saturated, 6 peer missing. Unknown age always has `a:null` and `f:false`; validity is independent of freshness. This compact schema does not contain a cell voltage, source classification or battery percentage. The measurement remains the LCD system rail; the full cloud JSON schema is unchanged.

The diagnostic reads only the cached snapshot. Its 192-byte automatic line buffer replaces the earlier 512-byte cloud-JSON readout buffer, and one `Serial.write` replaces the long `Serial.printf` allocation path. A maximum-width valid-snapshot fixture is 135 bytes including prefix and newline. If the entire line does not currently fit `availableForWrite()`, the observation is dropped without sampling, waiting or retrying. Concurrent console writers can still consume capacity or cause a short write, so delivery is not guaranteed. USB buffers and timeouts, persistent RAM, tasks and queues are unchanged.

Optional UART forwarding requires an awake Sense plus a real PONG or SYNC_ACK from the preceding two seconds. It does not additionally require the startup `link_synced` latch: a missed initial SYNC can leave that latch false even after a valid PONG. Binary ownership, queued/deferred user-work priority, sleep and OTA guards, three-second coalescing and no-extra-wake behavior remain unchanged. The LCD host test compiles the actual production task guard expression and exercises PONG-only unsynchronized admission, absent/expired proof, asleep peer, binary ownership, user queues, sleep and OTA refusals, plus bounded compact diagnostic formatting and capacity refusal.

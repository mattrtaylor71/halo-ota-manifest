# HALO device → backend: handoff

**For:** the agent working on the Trepo backend
**From:** the HALO firmware side
**Date:** 2026-08-24
**Firmware:** 6.2.0 (`BUILD_ID 6.2.0-Aug 22 2026-21:54:06-0f52537`)

The firmware is finished and verified. Two things on the backend/infra side are
blocking ship, and both are yours. This document gives you the exact contract the
device depends on so you can build the production side without breaking it.

**The single most important rule:** the device is a shipped appliance. When you
change the backend, firmware in the field cannot change with you. Every contract
below must keep working, or units go dark until someone re-flashes them by hand.

---

## 1. Blocker A — there is no production backend

### What exists today

| Thing | Value | Note |
|---|---|---|
| Presign API | `https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com` | API name is **`trepo-grocery-backend-dev`** |
| Stage | `$default` (AutoDeploy on) | |
| Check-in / dish bucket | `trepo-grocery-uploads-dev` | |
| Discard bucket | `trepo-grocery-discards-dev` | |
| Production equivalents | **none — searched the whole account** | |

Every grocery bucket in account `566667681926` ends in `-dev`. A shipped unit
would file real customer photos into the scratch environment we wipe freely.

### What's needed

A production presign endpoint plus production buckets, and a decision about how
the device is told which to use. See §4 — that decision has firmware
consequences.

### Verified working today (do not regress this)

The whole chain works end-to-end against dev. Measured 2026-08-24:

```
tap → LCD → UART → Sense → camera → PSRAM → deferred upload
    → presign → S3 PUT → backend resize
```

* **294** consecutive captures since 2026-08-21 delivered **and** processed
* every original in `images/…` gets a `resized-images/…` twin within ~8s
* the only originals ever missing a twin are 4 from 2026-08-18 (pre-fix)

---

## 2. The wire contract (authoritative — from firmware source)

### 2.1 Auth

```
x-api-key:      <API_KEY>        // only sent if non-empty; currently ""
Authorization:  <BEARER_TOKEN>   // only sent if non-empty; currently ""
```

Both are **empty in shipping firmware**. The endpoint is effectively
unauthenticated today. If you add auth you must coordinate — the device cannot
learn a new key on its own. See §4.

### 2.2 `POST /presign` — check-in (grocery)

Request:

```json
{
  "user_id": "<owner uuid>",
  "device_id": "halo-XXXX-YYYY",
  "owner": "<owner uuid>",
  "action": "IN",
  "type": "grocery",
  "content_type": "image/jpeg",
  "product_expiration": "2026-12-31",   // omitted when the user skipped it
  "quantity": 1,
  "camera_meta": { ... },               // see 2.5
  "errors": [ ... ]                     // see 2.6, only when present
}
```

### 2.3 `POST /presign` — dish and discard

Same endpoint, different body. `type` is `dish` or `discard`.

```json
{
  "user_id": "<owner uuid>",
  "device_id": "halo-XXXX-YYYY",
  "owner": "<owner uuid>",
  "action": "IN",
  "type": "discard",
  "content_type": "image/jpeg",
  "add_to_shopping_list": false,        // discard only
  "camera_meta": { ... },
  "errors": [ ... ]
}
```

`POST /presign/discard` also exists as a **legacy fallback** the firmware tries
if the primary call fails. Keep it alive or keep it returning the same shape.

### 2.4 Presign response — REQUIRED FIELDS

```json
{
  "job_id":       "73f0f9c6425c4ea09b9e7cbae56f03e0",
  "put_url":      "https://<bucket>.s3.amazonaws.com/images/...?X-Amz-...",
  "s3_key":       "images/<user>/<device>/2026/08/24/<uuid>.jpg",
  "content_type": "image/jpeg",
  "ttl_s":        900,
  "result_url":   ""
}
```

Firmware behaviour you must not break:

* **`job_id` and `put_url` are mandatory.** If either is missing or empty the
  firmware logs `presign_parse / missing_fields` and the capture **fails**.
* `content_type` empty or the string `"null"` → firmware substitutes
  `image/jpeg`. Safe to omit.
* `result_url` is parsed but **no longer used** — the nutrition feature was
  deleted in 6.2.0. Send `""` or omit. Do not resurrect a result-polling flow;
  nothing consumes it.
* `ttl_s` is read but not enforced by the device. The PUT usually happens
  within seconds of the presign, but see §3 — it can be **minutes** later.
* Response is parsed into a `StaticJsonDocument<768>`. **Keep the response under
  ~700 bytes.** A larger body silently fails to parse and the capture is lost.

### 2.5 `camera_meta` (diagnostic, sent on every presign)

```json
"camera_meta": {
  "profile": "label", "profile_code": 3, "flash_enabled": true,
  "jpeg_quality": 5, "capture_width": 1280, "capture_height": 1024,
  "configured_framesize": 14, "scene_luma": -1,
  "scene_green_ratio": -1, "xclk_hz": 20000000
}
```

Store it if useful; ignore it if not. Never reject a request for its contents.

### 2.6 `errors` — piggybacked device telemetry

The device has no other channel for error reporting, so it appends up to **3**
undelivered error records to the presign body:

```json
"errors": [ {"seq":12,"area":"upload_put","code":-1,"detail":"..."} ]
```

The device marks them delivered **only when the presign returns 2xx**. If you
drop or 4xx these requests you are also throwing away the only fleet error
telemetry that exists. Accept unknown fields; never validate strictly.

### 2.7 The S3 PUT

* Plain `PUT` to `put_url` with `Content-Type` from the response.
* Body is a JPEG, typically **130–165 KB**.
* Success is **HTTP 200**. Any non-200 is treated as failure and retried.
* TLS is validated against the real CA bundle (`tls_configure`) — **the
  certificate must be valid.** Do not put a self-signed or intercepting proxy in
  front of this.

### 2.8 Key layout the device produces

```
images/<user_id>/<device_id>/YYYY/MM/DD/<uuid>.jpg     ← original (device writes)
resized-images/<user_id>/<device_id>/<uuid>.jpg        ← you generate
product-images/<user_id>/<device_id>/<uuid>.jpg        ← you generate
```

Bench device: `device_id=halo-16f8-1a6d`,
`user_id=6f5d7535-2d6f-4251-9ed2-eb56fcf56400`.

---

## 3. Timing — read this before you touch latency

Uploads are **deliberately deferred**. This is not incidental; it is the fix for
a hardware constraint and it changes your assumptions.

The camera needs one contiguous 16 KB internal-DMA block. A TLS handshake needs
~25–30 KB of the same memory. There is ~40 KB. The first TLS handshake of a boot
permanently fragments the region, and every capture after it fails ~60%. So the
firmware holds **all** uploads until the user has stopped and the device is going
to sleep, then drains them in one burst.

Consequences for you:

* A photo may be presigned **minutes** after it was taken. Do not assume presign
  time ≈ capture time. `ttl_s` must comfortably exceed the gap; 900s is fine.
* Uploads arrive **in bursts** at sleep, several within ~10s, not spread out.
* The device budgets **30 s** for presign (`BACKGROUND_UPLOAD_PRESIGN_BUDGET_MS`)
  with 3 attempts and 500/1500/3500 ms backoff. **If you are slower than ~30 s
  the capture is dropped.** Keep p99 well under that.
* The device gives up a phase entirely with under 1 s left
  (`ACTION_MIN_REMAINING_MS`).

**A slow presign does not degrade gracefully — it loses the photo.**

---

## 4. How the device should learn the prod endpoint (needs a decision)

The base URL is a **compile-time constant**:

```c
// Sense_Minimal/Sense_Minimal.ino:261
const char* CHECKIN_API_BASE_URL = "https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com";
```

Options, in order of preference from the firmware side:

1. **Keep the hostname, switch what it points at.** Repoint the existing API (or
   put prod behind the same custom domain) and route by `device_id`/`user_id`
   server-side. **No firmware change, no re-test, works for units already in the
   field.** Strongly preferred.
2. **New hostname baked into firmware.** Requires a firmware change, a rebuild, a
   re-run of ship gates §0–§5 and the soak, and re-flashing every existing unit
   by hand — OTA cannot deliver it while blocker B stands (§5).
3. **Runtime-configurable endpoint.** Deliverable via provisioning, but it is new
   firmware work and new failure modes. Only worth it if you expect to move again.

If you pick 2 or 3, tell the firmware side early — it invalidates the current
verified build.

---

## 5. Blocker B — OTA is dead in production (may be yours)

Every device wakes at **02:00 local** and asks one question. It currently gets
`403`.

```
GET https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/manifest_latest.json
→ HTTP 403
```

The bucket has Block Public Access fully on (all four flags `true`) and was
empty. **6.2.0 is now staged there** and is correct:

```
sense_6.2.0.bin        1,650,912 bytes
sha256                 1003f27ca3cddde741bec71e3ce3007b6d6cc3a9417836d7e9ba18d089b6704f
manifest_latest.json   331 bytes, points at the above
```

Awaiting Matt's decision: public-read on the OTA prefix (works immediately, no
firmware change) vs CloudFront (bucket stays private, but the URL changes → new
firmware → same re-test/re-flash problem as §4 option 2).

Manifest schema the firmware parses (`ManifestClient.cpp`):

```json
{
  "version": "6.2.0",
  "bin_url": "https://.../sense_6.2.0.bin",
  "sha256": "<64 hex>",
  "size": 1650912,
  "min_version": "0.0.0",
  "artifact_fw_version": "6.2.0",
  "build_id": "6.2.0-Aug 22 2026-21:54:06-0f52537"
}
```

`bin_url`, `sha256` and `build_id` are required. `url` is accepted as a legacy
alias for `bin_url`. SHA-256 is verified before flashing — a mismatch aborts.

**Until this is resolved, anything we ship is frozen forever.** Any bug found
after a unit reaches a kitchen is permanent for that unit.

---

## 6. Things the firmware no longer does

Deleted in 6.2.0. If backend code still serves these, it is dead weight:

* **MQTT / AWS IoT entirely.** `sense_mqtt.h` is gone. MQTT existed only to carry
  dish nutrition results. All IoT certificates in the account are now **INACTIVE**
  and the leaked one is deleted. Nothing connects to `…iot.us-east-1.amazonaws.com`
  (the endpoint string survives only for a DNS warm-up).
* **Dish nutrition results.** No `/dish/result` polling, no `UI_MEAL_RESULT`, no
  result screen. **Dish is now a plain capture-and-log, identical to check-in and
  discard.** Do not send nutrition payloads; nothing will render them.
* **`result_url`** is parsed for compatibility but unused.

---

## 7. Rules of engagement

* **Never break the presign response shape.** Missing `job_id`/`put_url` = lost
  capture, and the user was already told "Logged!".
* **Keep the response under ~700 bytes** (`StaticJsonDocument<768>`).
* **Accept unknown request fields.** The device adds diagnostics over time.
* **Stay under 30 s** for presign, p99.
* **Never 4xx a request just for its `errors` array** — that is the only fleet
  telemetry channel.
* **Valid TLS certificate, always.** The device validates properly.
* **Assume the device cannot be updated** until blocker B is fixed.

---

## 8. Verification

To prove a backend change end-to-end, confirm the object *and* its processed
twin, not just an HTTP 200:

```bash
aws s3api list-objects-v2 --bucket <bucket> --profile trepo \
  --query "Contents[?contains(Key,'halo-16f8-1a6d')].[LastModified,Key,Size]" \
  --output text | sort | tail -20
```

A capture is only genuinely delivered when both exist:

```
images/<user>/<device>/YYYY/MM/DD/<uuid>.jpg      ← device wrote it
resized-images/<user>/<device>/<uuid>.jpg          ← you processed it
```

Counting `PUT status: 200` in device logs is **not** sufficient — that measure
has been wrong twice this month.

---

## 9. Open questions for you

1. Prod endpoint strategy — §4 option 1, 2 or 3? Option 1 needs nothing from us.
2. Will prod require auth? If so, how does a shipped device get the key?
3. Who owns the OTA bucket decision — you or infra?
4. Do the 4 unprocessed originals from 2026-08-18 matter, or can they be dropped?
5. Should prod buckets keep the same key layout (§2.8)? Changing it is a
   backend-only change if the device keeps writing to the presigned URL you give it.

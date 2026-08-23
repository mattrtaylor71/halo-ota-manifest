# Deferred uploads — plan

## Why

`esp_camera_init()` needs one contiguous **16,384**-byte internal DMA block. A TLS
handshake needs ~25–30KB of the same memory. There is ~40KB. They cannot both
hold their block, so the firmware releases the camera's reserve for TLS — and
after the first handshake of a boot the region is fragmented for good (~40KB
free, largest run **15,860**, i.e. 524 bytes short). Every capture after the first
in a session then fails ~60%.

Four narrower fixes were tried and all failed; two made things worse. See
SHIP_CHECKLIST §6. The only thing that works is to stop capture and TLS
overlapping **in time**.

## The change

Capture already returns immediately — the image lands in PSRAM and the UI shows
"Logged!" before any upload. So the upload does not need to run during the
session at all. Hold it until the device is going to sleep, i.e. until the user
has demonstrably stopped.

**Nothing new is invented.** All three pieces exist:

| piece | already there |
|---|---|
| image held in PSRAM | `job.image_buf`, `buffer alloc ok psram=1` |
| drain before sleep | `sense_sleep.h:546` "Upload flush window", 30s budget |
| defer while user active | `foreground_priority_active()` |

The only missing behaviour: the worker currently starts uploading ~2.5s after the
last input. It needs to hold until the sleep flush opens.

## Design

Gate the **normal** upload queue only:

- **Dish is NOT deferred.** Dish uploads and then waits on the AI nutrition result
  which the user is watching. Deferring it would break the feature. Dish sessions
  are single-capture by nature, so they do not need this.
- **Release the hold** when any of:
  1. the sleep path requests a flush (the normal case),
  2. the queue reaches a high-water mark — never drop a capture to hold a policy,
  3. a max hold age expires — bounds how long an image sits unuploaded.

## Capacity

`UPLOAD_QUEUE_MAX = 10`, images ~175KB, PSRAM free ~7.6MB. Worst case 1.75MB of
7.6MB. Not a constraint.

## The trade

PSRAM does not survive power loss. An image now sits in RAM for minutes rather
than ~30s, so an abrupt power cut loses more. Mitigations: the high-water and age
caps bound it, and the SD spool (built and verified end-to-end) remains the
failure fallback.

## Rollback

Behind `HALO_DEFER_UPLOADS_TO_SLEEP`. Set to 0 to restore upload-immediately.

## Success criteria

1. Multi-capture: several captures in ONE boot, **0 camera init failures**
   (baseline: 20–45).
2. No regression: 0 panics, 0 aborts, 0 stack canary.
3. Uploads still complete — `PUT status: 200` for every capture, via the sleep
   flush.
4. `[SLEEP_UPLOAD_FLUSH]` shows the queue draining, not timing out.
5. Ship gates §0–§4 still pass.

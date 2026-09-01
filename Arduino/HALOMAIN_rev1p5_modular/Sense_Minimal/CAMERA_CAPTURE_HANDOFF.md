# Camera Capture Handoff

This document explains how camera capture currently works in `Sense_Minimal/Sense_Minimal.ino`, what timing buckets exist, how retries and fallbacks behave, and what tradeoffs have driven the current implementation.

It is intended as an engineering handoff, not a product spec.

## Goals

The camera path is trying to balance several competing goals:

- Keep user-perceived capture latency as low as possible
- Still produce acceptable low-light images
- Avoid OV2640 init/capture failures
- Avoid JPEG encoder instability in dim scenes
- Keep power reasonable by deinitializing the camera between actions
- Avoid blocking foreground UI more than necessary

In practice, the current firmware is biased toward reliability and low-light tolerance more than raw speed.

## Current Entry Points

The main capture flow lives in the `OP_SCAN` path inside `op_worker_task()`.

There are two main capture variants:

- `check-in` uses the faster path: `warmup_and_capture(fb, true)`
- `discard` and `dish` use the slower path: `warmup_and_capture(fb, false)`

That means not all scan modes are equivalent from a timing standpoint.

## Current Shared Camera Configuration

Current camera constants:

- `CAPTURE_SIZE = FRAMESIZE_SXGA` (`1280x1024`)
- `JPEG_QUALITY = 10`
- `CAMERA_XCLK_HZ = 10000000`
- `CAMERA_UI_CAPTURE_DELAY_MS = 0`
- `CAMERA_PREFLIGHT_MODE = CAMERA_PREFLIGHT_OFF`
- `CAMERA_CAPTURE_TARGET_MS = 3000`
- `CAMERA_CAPTURE_BUDGET_FAST_MS = 4000`
- `CAMERA_CAPTURE_BUDGET_SLOW_MS = 8000`
- `CAMERA_ENABLE_REINIT_FALLBACK = false`

Important implication:

- Preflight scene analysis exists in code, but is currently disabled
- Default profile selection therefore matters a lot

## Current Profile Model

There are two distinct concepts:

### 1. Sensor Profiles

These tune the OV2640 registers:

- `CAM_PROFILE_NORMAL`
- `CAM_PROFILE_LOW_LIGHT`

`apply_sensor_profile_normal()` is more conservative:

- `aec2 = 0`
- `ae_level = 1`
- `gainceiling = 32X`
- `denoise = 0`

`apply_sensor_profile_low_light()` is more aggressive:

- `aec2 = 1`
- `ae_level = 2`
- `gainceiling = 64X`
- `brightness = 2`
- `contrast = -2`
- `denoise = 1`

Both currently use:

- `wb_mode = 2` (`Cloudy`)
- `lenc = 0`

### 2. Capture Modes

These are the `fast_profile` boolean passed into `warmup_and_capture()`:

- `true` = fast path
- `false` = slow path

This controls:

- warmup frame count
- number of final-capture attempts
- quality strictness
- capture time budget

It does not by itself decide the sensor tuning profile; it decides how aggressive the capture routine is.

## Current High-Level Capture Flow

For a normal scan action:

1. Sense receives menu action over UART
2. `OP_SCAN` starts
3. UI emits `CAPTURING`
4. If camera already exists, deinit it first
5. `init_camera()` powers and initializes the OV2640
6. Selected sensor profile is applied
7. Initial settle/discard is performed
8. `warmup_and_capture()` runs the main capture routine
9. Captured JPEG is copied to PSRAM-backed upload buffer
10. Camera is deinitialized to save power
11. Upload is deferred to background queue

## What `init_camera()` Actually Does

`init_camera()` is more than just `esp_camera_init()`.

It performs:

- camera power enable
- driver init, with up to 2 init attempts
- memory logging
- sensor reset
- optional scene preflight if enabled
- profile application
- framesize / JPEG quality reapplication
- `CAMERA_INIT_SETTLE_DELAY_MS`
- `camera_settle_discard(CAMERA_INIT_WARMUP_FRAMES, CAMERA_INIT_WARMUP_DELAY_MS)`

So the "camera init" bucket includes both hardware bring-up and some convergence work.

## What `warmup_and_capture()` Actually Does

This is the main capture engine.

Common behavior:

- starts a timing budget
- applies `CAMERA_CAPTURE_SETTLE_MS`
- performs warmup frames
- attempts final capture
- optionally rejects the frame on quality grounds
- may retry / switch profile / fall back to smaller framesize if budget remains

### Fast Path

Used by `check-in`.

Parameters:

- `warmup_frames = 1`
- `max_attempts = 1`
- `min_warmup_success = 1`
- `strict_quality = false`
- budget `= 4000 ms`
- warmup delay `= CAMERA_WARMUP_DELAY_FAST_MS`

### Slow Path

Used by `discard` and `dish`.

Parameters:

- `warmup_frames = 3`
- `max_attempts = 2`
- `min_warmup_success = 2`
- `strict_quality = true`
- budget `= 8000 ms`
- warmup delay `= CAMERA_WARMUP_DELAY_SLOW_MS`

This slow path is the main reason discard/dish can take materially longer than check-in.

## Retry and Fallback Order

If the first capture attempt does not succeed and budget remains, the code tries these in order:

1. Retry once with extra settle on the same profile
2. Switch sensor profile and retry
3. Fall back to `SVGA`
4. Optional full camera reinit + retry

However:

- `CAMERA_ENABLE_REINIT_FALLBACK` is currently `false`
- so the full reinit recovery path is compiled in but disabled at runtime

## Quality Gate

`is_frame_quality_ok()` can reject a frame based on:

- JPEG length too small for frame area
- luma too low/high when strict mode is active
- green ratio too high when strict mode is active

Today this matters more on the slow path because:

- `strict_quality = !fast_profile`

So discard/dish are stricter than check-in.

## Why Capture Can Be Slow Even With `CAMERA_UI_CAPTURE_DELAY_MS = 0`

Even with the explicit front-end UI delay removed, there are still several built-in timing buckets:

- camera power / init
- sensor reset and profile application
- init settle delay
- init discard frames
- capture settle delay
- warmup frames
- final capture attempts
- fallback settle delays if retries happen

The user-visible "capture time" is therefore not just one delay; it is the sum of several smaller phases plus the sensor's own convergence time.

## Example Real-World Slow Run

Observed run:

- mode: `discard`
- profile: `low_light`
- total capture time: about `11.1 s`

Timeline interpretation:

- about `0.5 s` for init and profile setup
- about `2.5 s` additional setup/convergence before the slow capture phase is fully underway
- about `7.9 s` inside the slow warmup/final-capture path

Key contributing factors in that run:

- slow path was used (`cap_slow`, `8000 ms` budget)
- profile was `low_light`
- warmup frame 1 failed
- final capture did succeed, but only after long convergence

This is why a capture can succeed while still being far outside the `3 s` target.

## Current Low-Light Bias

Recent changes moved the system toward default low-light behavior:

- the explicit `1.5 s` UI delay was removed
- the default active profile was set to `CAM_PROFILE_LOW_LIGHT`

This improves odds of getting a usable image in dim conditions, but it also increases risk of:

- longer convergence time
- more sensor noise / grain
- more color speckling from high analog gain

## Why Low-Light Images Look Grainy

The current low-light profile intentionally pushes the OV2640 harder:

- `GAINCEILING_64X`
- `aec2 = 1`
- brighter AE
- denoise enabled, but still limited

That combination helps produce an image in dark scenes, but it also amplifies:

- luminance noise
- chroma noise
- JPEG texture / artifacts

The grainy red-blue speckle seen in dark captures is primarily amplified sensor noise, not a UI or upload issue.

Contributing factors:

- high gain ceiling
- aggressive low-light exposure behavior
- JPEG compression of noisy source data

## Why Preflight Exists But Is Disabled

The code supports an optional preflight pass that:

- temporarily switches to a tiny framesize
- estimates scene luma / green ratio
- chooses `normal` or `low_light`

That is useful in principle, but it costs time.

Current setting:

- `CAMERA_PREFLIGHT_MODE = CAMERA_PREFLIGHT_OFF`

So today:

- no automatic scene-based profile choice occurs
- the default profile and retry logic do the work instead

## Why The Camera Is Deinitialized Between Actions

The current architecture deinitializes after each capture to:

- reduce power draw
- release camera resources
- avoid keeping the sensor active while idle

Tradeoff:

- every user action pays the full camera bring-up cost again

Alternative architectures such as keeping the camera warm after wake may reduce latency, but were intentionally not adopted in the current implementation.

## Foreground vs Background Responsibilities

Foreground capture path:

- UI `CAPTURING`
- camera init
- warmup and capture
- buffer copy
- user choice screen if needed

Background path:

- Wi-Fi connect
- presign
- upload
- result wait / backend work

This means slow image capture is distinct from slow upload; they are separate timing domains.

## Recent Practical Challenges Faced

The camera work has repeatedly had to trade off:

- `3 s` UX target
- low-light usability
- JPEG reliability
- consistency across kitchen-like lighting
- avoiding regressions in wake / sleep / upload behavior

Some notable tensions:

- Lower latency often meant darker or less stable captures
- More convergence time improved low-light output but made capture feel sluggish
- Aggressive low-light tuning increased noise
- Higher JPEG aggressiveness could destabilize encoding in dim scenes

## Production Recommendations Already Reflected

The separate `CAMERA_PRODUCTION_RECOMMENDATIONS.md` note documents the main production-oriented tuning work that informed current settings.

Current implementation already reflects some of those ideas, including:

- `JPEG_QUALITY = 10`
- `lenc = 0`
- `wb_mode = 2`

But not all recommended timing / architecture options are enabled.

## Things That Are Intentionally Not Enabled Right Now

Current firmware does not do these by default:

- boot-time camera initialization
- background camera prewarming
- always-on scene preflight
- full reinit fallback during normal runtime
- dual capture workflow
- fill light usage (`FILL_LED_PIN = -1`)

These were either rejected for complexity / power reasons or left disabled to keep behavior simpler.

## What To Watch In Logs

Useful markers:

- `[CAMERA_TIMING]`
- `init_begin`
- `init_ok`
- `preflight_skip` or `preflight_begin`
- `profile_set`
- `init_warmup`
- `cap_fast` or `cap_slow`
- `warmup_begin`
- `warmup_done`
- `final_try`
- `retry_same`
- `retry_profile`
- `retry_svga`
- `capture_ok`
- `target_miss`

Interpretation:

- If time is lost before `cap_fast` / `cap_slow`, it is mostly startup / settle cost
- If time is lost between `warmup_begin` and `warmup_done`, it is convergence / warmup cost
- If retries appear, the camera path is spending time recovering from quality or capture failure

## Current Bottom Line

The current implementation is a reliability-biased, deinit-between-actions camera pipeline with:

- SXGA capture
- JPEG Q10
- 10 MHz XCLK
- default low-light profile
- fast path for check-in
- slow path for discard/dish
- multiple retry / fallback stages
- no preflight enabled

Its main challenge is that low-light robustness and image usability are currently competing directly with the desired `~3 s` capture time.

That is the core engineering tension in this system.

# HALO Sense Board Firmware Documentation

## Overview

The Sense board is an **XIAO ESP32-S3** with PSRAM, OV2640 camera, I2S microphone, and UART connection to the LCD board. It is the network-connected brain of the HALO device, responsible for:

- **Camera capture** with adaptive scene analysis (preflight brightness/color detection)
- **S3 image uploads** via presigned URLs with retry logic
- **Voice recording** and upload to an async quick-ack API
- **WiFi management** with guard state machine, background maintenance, and hard-reset recovery
- **Sleep coordination** with the LCD board via a handshake protocol
- **OTA self-update** (via production wrapper) and LCD OTA proxy over UART
- **Shopping list** fetch and display via UART to LCD

### Hardware

| Component | Detail |
|---|---|
| MCU | ESP32-S3 (XIAO form factor) |
| Camera | OV2640, GPIO1 PWDN, GPIO4 fill LED |
| Microphone | I2S PDM, 16 kHz 16-bit mono |
| UART to LCD | UART1, 115200 baud, JSON line protocol |
| Wake input | GPIO2 (EXT0, active LOW, driven by LCD INT_PIN) |
| Flash | 8 MB (must compile with FlashSize=8M) |
| PSRAM | OPI PSRAM |

### FreeRTOS Tasks

| Task | Core | Stack | Priority | Purpose |
|---|---|---|---|---|
| `op_worker` | 1 | 16384 | 2 | Executes SCAN/VOICE/LIST_REFRESH operations as state machines |
| `upload_worker` | 1 | 12288 | 1 | Background presign + S3 PUT upload pipeline |
| Arduino `loop()` | 1 | default | 1 | UART RX, WiFi maintenance, sleep coordination, GPIO polling |

---

### USB console backpressure

`initUarts()` sets `Serial.setTxTimeoutMs(1)` once; no production code overrides it. The installed Arduino core 3.3.8 `HWCDC::write()` uses an unsigned no-progress retry counter. With timeout 0 and a connected host that stops draining a full byte ring, the zero-byte ring send succeeds and the counter underflows before its zero test. The positive 1 ms SDK timeout avoids that path. It also bounds no-progress `Serial.flush()` retry; mutex/ring waits use the SDK tick conversion. This is a no-progress timeout, not a 1 ms wall-clock guarantee for a whole write with partial progress. Console logs may be dropped. The inter-board UART1 and OTA transfer timeouts are unchanged; the installed SDK is not modified.

This conditional driver defect is reproduced with actual-source host tests. Passive USB enumeration after closing a reader did not establish that it caused the observed bench stall; physical validation remains separate.

## Module Reference

### 1. Sense_Minimal.ino -- Main File

**Path:** `Sense_Minimal/Sense_Minimal.ino` (~4400 lines)

This is the main compilation unit. All `.h` modules are `#include`d from here in dependency order. It contains:

#### Constants and Configuration

| Constant | Value | Purpose |
|---|---|---|
| `PROTOCOL_VERSION` | 1 | UART JSON protocol version field |
| `MAX_LINE_LENGTH` | 4096 | Max UART line length (for voice response payloads) |
| `UART_BAUD_RATE` | 115200 | UART1 baud rate |
| `WAKE_GPIO` | GPIO2 | EXT0 deep sleep wake pin (active LOW) |
| `WAKE_LEVEL` | 0 (LOW) | EXT0 trigger level |
| `WAKE_PIN_FAILSAFE_TIMER_S` | 15 | Timer wake fallback when wake pin stuck |
| `SENSE_MAX_SLEEP_TIMER_S` | 0 | Disabled (was 300s/5-min safety net; disabled after hard gate cleanup made the safety timer unnecessary) |
| `MIN_AWAKE_BEFORE_SLEEP_MS` | 2000 | Grace period before allowing sleep |
| `GUARDIAN_FORCE_SLEEP_MS` | 300000 | 5-min max awake time (guardian force sleep); clock is paused (reset each loop) while `halo_provisioning_active()` so it never tears down SoftAP/QR mid-setup |
| `ACTION_AWAKE_BUDGET_MS` | 60000 | Per-action time budget (1 min) |
| `ACTION_MIN_REMAINING_MS` | 1000 | Minimum budget remaining to start a step |
| `LCD_INACTIVITY_TIMEOUT_MS` | 10000 | Sleep if no LCD message for 10s |
| `LINK_HB_INTERVAL_MS` | 4000 | Sense-to-LCD heartbeat interval |
| `DISH_RESULT_TIMEOUT_MS` | 60000 | Max wait for dish HTTP result |
| `CAPTURE_SIZE` | FRAMESIZE_SXGA | 1280x1024 default capture resolution |
| `JPEG_QUALITY` | 8 | JPEG compression quality (lower = less compression) |
| `CAMERA_XCLK_HZ` | 10000000 | Camera clock 10 MHz |
| `CAMERA_DMA_RESERVE_BYTES` | 16384 | DMA block reserved before WiFi init |
| `AUDIO_BUFFER_SIZE` | 524288 | 512 KB voice buffer (~16s at 16 kHz) |

#### Device ID Generation

```c
static void load_runtime_device_id(char* out, size_t out_len)
```
Generates a device ID from the ESP32 eFuse MAC address in the format `halo-XXYY-ZZWW`. This is used for API calls, MQTT client IDs, and voice session IDs.

#### Owner ID Loading

```c
static void load_owner_id_or_default(char* out, size_t out_len)
```
Loads the owner UUID from NVS provisioning state. If an `owner_code` is pending (claim not yet completed), suppresses the owner ID to prevent API calls with stale identity.

#### `setup()`

Boot flow:
1. Print wakeup diagnostics and reset reason
2. Initialize UARTs (USB CDC + UART1 to LCD)
3. Init camera PWDN GPIO (hold camera off by default)
4. Increment boot counter (`g_sense_boot_count`, RTC-persistent)
5. Check wake cause:
   - **EXT0**: Re-init UARTs, send `UI_STATUS IDLE` to LCD
   - **TIMER**: Init UARTs, send status, check if LCD is active (2s window), call `ota_on_timer_wake()`
   - **Cold boot**: Standard init
6. Configure wake GPIO for runtime polling (pullup, input)
7. Create FreeRTOS queues: `op_queue` (20 slots), `upload_queue` (10)
8. Init upload persistence (SPIFFS) if enabled
9. Create mutexes: `http_mutex`, `wifi_connect_mutex`, `mic_mutex`, `g_list_mutex`
10. Allocate voice audio buffer in PSRAM (512 KB)
11. Init I2S audio system and register `voice_audio_callback`
12. Create `op_worker_task` (Core 1, priority 2, 16 KB stack)
13. Create `upload_worker_task` (Core 1, priority 1, 12 KB stack)
14. Reserve 16 KB DMA block for camera before WiFi fragments heap
15. Start WiFi.begin() non-blocking (skipped if provisioning needed)
16. Register WiFi event handler
17. Call `halo_prod_setup()` (production wrapper)

##### Reboot-loop guard feeds on crash resets only

The production wrapper's early init increments `BootState::nextBootCount()` (monotonic
counter, used elsewhere) and then decides how to treat the reboot-loop history based on
`esp_reset_reason()`:

- **Crash reset** (`ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT`, `ESP_RST_WDT`,
  `ESP_RST_BROWNOUT`): calls `BootState::recordBootTimestamp()` so the boot feeds the
  loop detector.
- **Clean boot** (deep-sleep wake / power-on / SW restart, e.g. post-OTA): calls
  `BootState::clearRebootHistory()` instead, so normal wakes and scheduled-OTA wakes can
  never trip the guard.

`g_reboot_loop_detected = BootState::checkRebootLoop(3, 30000)` then only fires on 3
genuine crashes with no clean boot between. **Why this matters:** a scheduled OTA wakes
the device from deep sleep (timer wake + in-window retries), and the detector used
`boot_count` as a pseudo-timestamp, so a few normal wakes used to trip the guard and latch
`ota_en=0` (`why=reboot_loop_guard`) — permanently disabling scheduled OTA. Manual OTA
bypasses the guard via `halo_ota_manual_override_active`, which is why manual worked but
scheduled did not. `reset_reason_to_str()` (sketch ~line 547) maps the enums for logging.

#### `loop()`

Main loop responsibilities:
1. **UART RX processing**: Read `lcdSerial`, push to ring buffer, extract frames, call `parse_input_message()`. Skipped when LCD OTA proxy owns UART.
2. **USB serial injection**: Accepts JSON commands via USB CDC for debugging.
3. **WiFi maintenance**: `service_wifi_maintenance()`, guard poll, RSSI reporting (every 2s).
4. **Upload persistence replay**: Attempt to re-queue failed uploads from SPIFFS.
5. **Guardian force sleep**: If awake > 5 minutes, force deep sleep.
6. **Work request processing**: Handle `reset_wifi_requested`, `refresh_requested`, `delete_requested`.
8. **Wake pin polling**: Detect LCD wake pulses (falling edge on GPIO2) every 50ms.
9. **Sleep coordination**: Evaluate `sleep_requested`, check cooldown, holdoff, background work, then enter deep sleep.

#### `parse_input_message(const char* json_str)`

Central UART message dispatcher. Parses JSON, validates protocol fields, routes by `type`:

| Message Type | Action |
|---|---|
| `INPUT_WAKE` | Reset sleep timers, request list refresh, debounce 1.5s |
| `INPUT_SLEEP` | Start coordinated sleep handshake, check wake pin |
| `INPUT_LONG_PRESS_START` | Queue `OP_VOICE` job |
| `INPUT_LONG_PRESS_END` | Signal voice finalize |
| `INPUT_MENU_SELECT` | Queue `OP_SCAN` with mode (dish/discard/check-in) |
| `INPUT_EXPIRY_DATE` | Store expiry + quantity for check-in job |
| `INPUT_DISCARD_OPTIONS` | Store add-to-shopping-list flag |
| `INPUT_DELETE` | Queue item delete from shopping list API |
| `LIST_ACTIVE` | `state=1`: user on the list screen → set `g_list_screen_active` (keep awake + WiFi up), extend grace, ensure WiFi **on the rising edge only**; `state=0`: clear it (allow sleep). 30s staleness auto-clear watchdog in `loop()` |
| `INPUT_PING` | Reply with PONG, update heartbeat timestamp |
| `INPUT_RESET_WIFI` | Flag WiFi credential reset |
| `INPUT_FW_INFO` | Trigger fresh LCD query (BLOCKING), then reply with `FW_INFO` carrying BOTH board versions and live partition/state. Diagnostic/automation path |
| `INPUT_SENSE_FW` | **Fast path** for the LCD Settings screen: reply IMMEDIATELY with `FW_INFO` carrying live Sense version/build/partition/state + CACHED `lcd_fw` (`g_lcd_ota_version`, or `"unknown"`). Calls `uart_send_fw_info(false)` — NO blocking `sense_lcd_ota_query()`. Settings only reads `sense_fw` |
| `INPUT_OTA_CHECK` | Trigger manual OTA check |
| `INPUT_WIFI_SCAN` | Scan and report visible APs |
| `INPUT_WIFI_TEST` | Full WiFi cold-start diagnostic |
| `SYNC` / `SYNC_ACK` | Link synchronization. After replying `SYNC_ACK`, the Sense also **pushes `FW_INFO`** via `uart_send_fw_info(false)` (fast, non-blocking) so whenever the LCD (re)connects — including right after its OTA reboot — it immediately receives the current `sense_fw` without having to query |
| `LINK_HB` | LCD heartbeat received |
| `LCD_DIAG` | Store LCD diagnostic state |
| `WIFI_STATUS` | Sync WiFi credentials with LCD |
| `WIFI_CREDS_ACK` / `WIFI_ON_ACK` | Provisioning credential flow |
| `MAINT_WINDOW_ACK` | Maintenance window scheduling |
| `SLEEP_DENY` | LCD denied Sense sleep intent |
| `OTA_CHECK_ACK` / `OTA_CHECK_RESULT` | OTA status from LCD |
| `LCD_OTA_QUERY_RESP` | LCD running fw version + partition/state (OTA proxy mailbox). Extended fields: `running_part`, `running_state`, `boot_part`, `boot_ready` |
| `LCD_OTA_BEGIN_ACK` | LCD accepted OTA session (mailbox) |
| `LCD_OTA_END_ACK` | LCD SHA verification result (mailbox) |
| `LCD_OTA_STATUS` | LCD OTA progress (informational) |

#### `upload_worker_task(void *arg)`

Background upload pipeline running in an infinite loop:

1. **Dequeue priority**: Dish queue first, then parked job (if no dish pending), then normal queue.
2. **Foreground yield**: If user action active, park the job and wait.
3. **Voice uploads**: Connect WiFi, call `voice_upload_and_parse()`, handle persistence on failure.
4. **Image uploads**:
   a. **Presign** (up to 3 retries with backoff): Get presigned S3 URL via `get_presign()` or `get_presign_checkin()`.
   b. **PUT** (up to 3 retries): Upload image to S3 via `put_to_presigned_url()`.
   c. **Dish result wait**: For dish mode, poll HTTP for AI analysis result.
   d. **Persistence**: On failure, save to SPIFFS for retry on next boot.
5. Dish uploads preempt normal uploads at every stage.

#### `op_worker_task(void *arg)`

Processes operation jobs from `op_queue`:

- **OP_SCAN**: Camera init, preflight, capture, queue upload job, handle check-in/discard expiry input wait.
- **OP_VOICE**: Start I2S recording, wait for finalize signal, upload PCM audio.
- **OP_LIST_REFRESH**: Fetch shopping list from API, send to LCD.

#### Sleep State Machine

States: `SLEEP_SM_IDLE` -> `SLEEP_SM_REQ_RX` -> `SLEEP_SM_READY_SENT` -> `SLEEP_SM_SLEEPING`

Sleep flow:
1. LCD sends `INPUT_SLEEP`
2. Sense checks wake pin, pending operations, cooldown
3. If blocked, sends `SLEEP_DENY` with retry_ms
4. If clear, drains upload queue (budget scales with queue depth), disconnects WiFi, deinits camera
5. Configures EXT0 + timer wake, sends `SLEEP_READY` + WiFi diag summary
6. Calls `esp_deep_sleep_start()` (no return)

**Shopping-list keep-awake + hardening (v6.1.759).** `sense_can_sleep_now()` blocks idle sleep while
`g_list_screen_active` (user on the list — hard block) OR `list_refresh_inflight` (a fetch in flight —
the original root-cause check; **force-deferrable after `LIST_REFRESH_BLOCK_MAX_MS`≈12s** so a stuck
fetch can't pin awake forever). The pre-deep-sleep WiFi teardown (`sense_sleep.h`) is **skipped while
`g_list_screen_active`** so WiFi stays up for instant refresh/delete. The list fetch
(`fetch_shopping_list_from_api`, sense_list.h) uses a **bounded ~6s WiFi-connect budget with no
hard-reset escalation** (`LIST_FETCH_WIFI_BUDGET_MS`) so flaky WiFi fails fast (`list_refresh_fail`)
and `op_inflight` releases (was the ~40s hard-reset chain that pinned `SLEEP_DENY reason=op_inflight`).
See memory `project_halo_list_feature`.

**Never-silently-drop refresh (`request_list_refresh()`, Sense_Minimal.ino).** A user refresh
(INPUT_WAKE) is never silently eaten — a silent drop left the LCD on "Refreshing..." until its 12s
hard timeout. Behavior by gate:
- **Cooldown hit** (`now < list_refresh_cooldown_until_ms`, cooldown = `LIST_REFRESH_COOLDOWN_MS` = 3s
  after each refresh completes): if the cached list was fetched successfully within the cooldown window
  (+1s jitter margin; tracked by `list_last_fetch_ok_ms`, set in `parse_and_update_shopping_list`),
  the Sense immediately re-sends the cached list so the LCD refresh completes instantly
  (`[LIST_REFRESH] cooldown_hit -> served_cached`). If there is no
  fresh cache (last fetch failed), the request is **accepted despite the cooldown**
  (`cooldown_hit -> cache_stale, accepting`).
- **Awake-proof ordering (both paths).** The LCD's refresh state machine sits in
  `REFRESH_WAKE_PENDING` until it sees an awake-proof message (PONG, SYNC_ACK, or UI_STATUS); a
  UI_LIST that arrives first renders the list but leaves the SM stuck ("Refreshing" pill never
  clears). The Sense therefore sends `UI_STATUS "IDLE"` **before** the list on both paths:
  - **Served-cached**: `UI_STATUS "IDLE"` → `delay(40)` (lets the LCD's Core-0 RX process it) →
    `uart_send_ui_list()` → `delay(50)` → trailing `UI_STATUS "IDLE"`. Logged
    `[LIST_REFRESH] awake_proof_sent path=cached`.
  - **Normal fetch**: `UI_STATUS "IDLE"` sent at OP_LIST_REFRESH dequeue in the op worker, before
    `fetch_shopping_list_from_api()` — needed because the wake-path IDLE in setup() only fires when
    the Sense was actually deep-sleeping, and a warm-WiFi fetch (~400ms) could land UI_LIST before
    any PONG/SYNC_ACK. Logged `[LIST_REFRESH] awake_proof_sent path=fetch`.
- **Already inflight**: safe debounce — the running fetch will answer the LCD with UI_LIST + IDLE.
  **Stuck-inflight self-heal**: if `list_refresh_inflight` has been set for > `LIST_REFRESH_TIMEOUT_MS`
  (20s, same constant as the loop() watchdog), the flag is cleared in place and the new request is
  accepted (`stuck inflight ... -> self-heal`), so a wedged flag can't break refresh until reboot.
  The INPUT_WAKE handler no longer early-returns on inflight; `request_list_refresh()` owns that logic.
- **Rapid-refresh debounce still services the list (2026-06-17).** The INPUT_WAKE 1.5s debounce
  now only suppresses the wake/holdoff/grace churn — it no longer `return true`s before requesting a
  refresh. The debounced path calls `request_list_refresh("input_wake_debounce", false)` (gated by
  `lcd_ota_in_progress()`, same as the accepted path), which answers safely (cached during cooldown /
  skips a live inflight that will answer / enqueues otherwise) and never re-wakes. Previously the
  debounce silently dropped the request, so the LCD's INFLIGHT refresh hung until its 20s watchdog
  on back-to-back pulls (log: `[INPUT_WAKE] ignored debounce age_ms=...` with no following UI_LIST).
- **LCD OTA in progress**: still deferred (unchanged), logged clearly.

**UI_LIST buffer.** `uart_send_ui_list()` uses a heap `DynamicJsonDocument(12288)` (freed on scope
exit) instead of `StaticJsonDocument<4096>` — 50 items x 64B id + 64B text + 48B store could overflow
a smaller doc and silently drop items. If items still don't fit, it logs `[UI_LIST] truncated
dropped=N overflowed=...` instead of truncating silently. The `huuid` field added to
`shopping_list_item_t` is **not** serialized into UI_LIST (the LCD doesn't need it). The `store`
field **is** serialized (`item["store"]`) so the LCD can render store-group headers; the doc was
bumped 8192→12288 to absorb the extra ~48B/item.

**Refresh latency instrumentation.** One-line millis() deltas across the path: `[LIST_REFRESH]
dequeued queue_wait_ms=` (enqueue→op-worker dequeue), `[NET_DIAG] list_wifi_connect_ms=` (bounded
list-fetch WiFi connect), `[NET_DIAG] fetch_ok code=200 http_ms=... resp_len=... attempt=` (HTTP
duration on success — was failure-only), `[LIST_REFRESH] parse_ms=`, and `[LIST_REFRESH] total_ms=...
fetch_ms=... (request->UI_LIST)`.

---

### 2. sense_camera.h -- Camera Hardware Control

**Purpose:** Camera init/deinit, sensor profile management, preflight scene analysis, warmup, capture, and metadata helpers.

#### Camera Profiles

| Profile | Use Case | Key Settings |
|---|---|---|
| `CAM_PROFILE_NORMAL` | Standard lighting | AEC on, AEC2 off, gain ceil 32x, brightness +1 |
| `CAM_PROFILE_LOW_LIGHT` | Dark scenes (luma < 60) | AEC2 on (longer exposure), gain ceil 64x, brightness +2, contrast -2 |
| `CAM_PROFILE_FLASH` | Fill LED active | AEC2 off, gain ceil 16x, contrast +1, sharpness +2 |
| `CAM_PROFILE_LABEL` | Text/barcode (default) | AEC2 off, sharpness 3 (max), lens correction ON |

#### Key Functions

**`init_camera()`** -- Camera initialization sequence:
1. Check DMA availability. If DMA < 24 KB: with an HTTP request **inflight**, wait up to `CAMERA_HTTP_DRAIN_MAX_MS` (1500) for it to finish, then tear WiFi down — and if it has *not* drained, **skip the teardown entirely**. Tearing WiFi down under an inflight request frees lwIP pbufs while the upload task is blocked in `recv()`, which panicked the board (`lwip_recvfrom → lwip_recv_tcp → pbuf_free`) at a measured 2 panics / 8 captures. Losing the DMA headroom for one capture is strictly better than a panic.
2. Release DMA reservation (16 KB) so esp_camera_init can use it
3. Configure camera hardware (XCLK, pins, JPEG, 2 framebuffers in PSRAM)
4. Two init attempts; on first failure, deinit + quiesce network + retry
5. Run preflight scene analysis if enabled (compute luma/green ratio)
6. Select camera profile based on scene or default to LABEL
7. Apply profile, settle, discard warmup frames

**`deinit_camera()`** -- Cleanup and DMA re-reservation:
1. Turn off fill LED
2. `esp_camera_deinit()` (suppress GDMA disconnect error)
3. Re-acquire 16 KB DMA reservation to protect from WiFi fragmentation
4. Stop XCLK, set camera pins high-Z, disable power
5. Kick background WiFi reconnect (camera init kills WiFi)

**`warmup_and_capture(camera_fb_t*& fb, bool fast_profile)`** -- Multi-stage capture with fallbacks:
1. Warmup frames (3 frames, discard)
2. Final capture with retry
3. Frame quality check (min size, luma bounds, green ratio)
4. On failure: retry same profile, switch to alternate profile, fallback to SVGA, optionally reinit camera

**`compute_scene_brightness_preflight(sensor_t* s, int* green_ratio_out)`** -- Scene analysis:
- Switches to RGB565 at QQVGA (160x120)
- Computes average luma and green/RB ratio
- Detects overly green scenes (common camera fault)
- Budget-limited to `CAMERA_PREFLIGHT_BUDGET_MS` (1000ms)

**`capture_camera_meta_snapshot()`** -- Captures metadata (profile, flash, JPEG quality, dimensions, luma, green ratio, XCLK) into an `UploadJob::CameraUploadMeta` struct for upload with presign request.

#### Camera Power Control

- `camera_power_enable()`: PWDN LOW, delay 15ms
- `camera_power_disable()`: PWDN HIGH, delay 2ms
- `camera_power_hold_enable()`: GPIO hold + deep sleep hold (keeps camera off during sleep)
- `camera_stop_xclk()`: Detach LEDC channel from XCLK pin

#### Upload failure must never free the photo

A failed upload used to destroy the user's capture outright. The `put_fail` path
went straight from the diagnostic to `free(job.image_buf)` with **no persistence
attempt at all** — not SPIFFS, not the SD card. The SD spool built to prevent
exactly this was only wired into the *sleep* path (a job parked because the
foreground was busy), never into the failure path it exists for.

Order now, in `upload_persist_handle_failure()` so every failure reason is
covered (`put_fail`, `put_timeout`, `presign_fail`, `presign_timeout`,
`voice_post_fail`) rather than patching call sites one at a time:

1. `upload_persist_save()` → SPIFFS. For a real capture this **always** fails:
   the partition is 173,441 B and writing needs roughly twice the image size.
   Measured: `skip_save len=156779 avail=173441 required=222391`.
2. Fall back to `sense_spool_image_to_lcd()` → the LCD's SD card.
3. Only if both fail is the photo lost, and that emits a `SENSE_DIAG` marked
   `PHOTO_LOST` so the unrecoverable case is at least visible.

**The image buffer may only be freed after this returns.** Any new failure path
that frees earlier reintroduces silent data loss.

> **SD spool-DRAIN is DISABLED (2026-09-04).** `SPOOL_DRAIN_WAKE_ENABLED` in
> `sense_spool_drain.h` is set to **0**. That macro now gates **every**
> `SPOOL_LIST_REQ` emission on the Sense — `sense_spool_drain_tick()`,
> `sense_spool_probe_tick()`, and the bench `sense_spool_drain_force()`
> (`draintest`/`drainreal`) all early-return when it is 0 — not just the
> early-wake arming. Reason: any spool op makes the LCD mount its SD card
> (`lcd_sd_init()` → `sd_card_Init()` → `esp_vfs_fat_sdmmc_mount()`), which
> asserts on a NULL SDMMC semaphore (`assert failed: xQueueSemaphoreTake
> queue.c:1709`) and **panics the LCD**. The Sense still *attempts* the step-2
> fallback (`sense_spool_image_to_lcd()`), but the LCD now refuses it: its
> `IMG_XFER_BEGIN` handler calls `lcd_img_rx_begin()` → `lcd_sd_init()`, which
> is hard-disabled and returns `false`, so the LCD replies `IMG_XFER_READY ok=0`
> and the Sense keeps the image in RAM (no mount, no crash). Net effect: while
> this is disabled the SD card is fully out of the loop on both boards, so a
> failed upload can lose the photo (the pre-spool behaviour) — the accepted
> tradeoff vs. crashing the LCD. Re-enable only after `sd_card_Init`/SDMMC init
> is root-caused and fixed.

#### Bounded Frame Grab (`sense_camera_grab.h`)

`esp_camera_fb_get()` takes no timeout — the installed driver exposes only `camera_fb_t* esp_camera_fb_get(void)`, with no timeout variant. Every budget check in this module (`CAMERA_PREFLIGHT_BUDGET_MS`, `camera_timeline_event`, the `elapsed_ms` comparisons) runs *after* the call returns, so none of them can fire on the one failure they exist for: a sensor that never delivers a frame. That hung the capture task and stranded the LCD on the capturing screen.

The grab now runs on a dedicated worker task; callers wait on a semaphore with a deadline.

| Symbol | Meaning |
|---|---|
| `sense_camera_fb_get_bounded(timeout_ms)` | The only way frames are pulled. Returns `nullptr` on expiry. |
| `sense_camera_grab_start()` | Starts the worker. Called from `init_camera()` immediately after `esp_camera_init()` succeeds, so even init warmup frames are covered. |
| `sense_camera_grab_stalled()` | True once a grab has overrun; further grabs are refused rather than queued behind a task that will never wake. |
| `sense_camera_grab_recover()` | Clears the latch if the frame merely arrived late. Returns false if the worker is genuinely wedged. |
| `CAMERA_GRAB_TIMEOUT_MS` (5000) | Final capture bound (typical grab is ~300–800 ms). |
| `CAMERA_GRAB_WARMUP_TIMEOUT_MS` (2500) | Warmup/preflight bound. |

**Rules:**
- **Every** grab must go through `sense_camera_fb_get_bounded()`. Two contexts calling `esp_camera_fb_get()` concurrently would race inside the driver, so a direct call left anywhere reintroduces the bug in a harder-to-find form. All five former call sites in this module are converted; the only live raw calls are inside the worker and its no-worker fallback.
- A stall **latches**. The worker stays blocked inside the driver — FreeRTOS cannot cancel a blocked task and `esp_camera_deinit()` contends on the same state — so there is no in-process way to reclaim it.
- A late frame is returned to the pool by the worker, not the caller, so a caller that gave up cannot leak one of the driver's small framebuffer pool.
- Capture failure now reports `camera_stall` distinctly from `final_capture_null`: a null is retryable, a stall is not, and retrying a latched stall only burns the user's time on the capturing screen.

#### DMA Guard Pattern

> **2026-08-20 — the guards were failing silently, and it cost ~1 capture in 12.**
> The margin here is about **1 KB**: `esp_camera_init()` needs one contiguous
> **16,384**-byte DMA block and the largest free block measures **17,396**. The
> reserve exists to hold exactly that block so WiFi/TLS cannot fragment it.
>
> Six sites released it and five re-acquired it, each open-coded. **Three released
> with no log at all, and every re-acquire discarded its result** (`halo_tls_restore_dma_reserve()`
> logged `"restored reserve, ptr=%p"` unconditionally, printing `ptr=0x0` on failure
> and still reading as success). So a failed re-acquire left the region unprotected
> with no evidence, and the *next* camera init failed with a bare `0xffffffff`.
> The signature was a failed init with **no preceding "DMA reservation released"
> line** — because the pointer was already null, so the guarded release did nothing.
> That is indistinguishable from a dropped log line, which is how it survived.
>
> Measured: the reserve was **NOT held at camera-init time in 37 of 39 captures**.
> The safety net was essentially never deployed; inits were succeeding on luck.
>
> All sites now delegate to `camera_dma_reserve_release(who)` /
> `camera_dma_reserve_acquire(who)` (`Sense_Minimal.ino`, just after
> `CAMERA_DMA_RESERVE_BYTES`), which log every transition with its caller, retry
> the acquire 3× (TLS frees internal SRAM slightly *after* the socket closes, so
> the destructor is the worst possible moment to try), and emit
> `[DMA_RESERVE][WARN] re-acquire FAILED ... next camera init is at risk` when
> exhausted. `init_camera()` additionally logs when the reserve was **not** held,
> so the exact signature that hid this now self-reports.
>
> **The exposure itself is still open, and one fix was tried and REVERTED.**
> An idle re-arm (re-acquire the reserve whenever the device looks idle) drove
> exposure 95% → 0% over 24 captures — and broke uploads: presign `HTTP 200` went
> **41/41 → 6/25**, with one capture losing its photo outright
> (`SD spool FAILED ... saved=0`). A TLS handshake needs ~25–30KB **contiguous**
> internal RAM, which is the whole reason these guards release the block; "idle" is
> not mutually exclusive with an upload in flight (uploads continue in the
> background after `current_job` reports DONE), so the re-arm raced the guards and
> took the block back mid-handshake. The logs interleave exactly that:
> `PRESIGN attempt=` / `re-armed while idle` / `HTTP_FAIL label=PRESIGN`.
> Gating on `http_inflight`/`upload_inflight` is **not** sufficient — the presign
> retry loop spans seconds with gaps where those flags are clear.
>
> Current state: diagnostics + retry only. Camera init fails ~1 in 12 but reports
> an honest error and the next capture succeeds; a failed upload can lose the
> photo. Protecting the upload is worth more. Any future attempt at this must be
> measured against **presign success rate**, not just camera exposure.

Before WiFi/TLS operations, the 16 KB DMA reservation is freed to give TLS enough contiguous internal SRAM. An RAII `DmaGuard` struct re-acquires it on function exit. This pattern appears in:
- `http_post_json_with_retries()`
- `http_get_with_retries()`
- `put_to_presigned_url()`
- `voice_upload_and_parse()`
- `ProvisioningManager::tryClaimOwnerId()` (owner-claim HTTPS POST) — runs during provisioning when internal heap is fragmented by AP_STA; the claim TLS handshake (-1/http fail) needs the reserve freed. Because `ProvisioningManager.cpp` is a separate translation unit that cannot see the `static g_camera_dma_reserve`, it calls the external `extern "C"` hooks `halo_tls_free_dma_reserve()` / `halo_tls_restore_dma_reserve()` (implemented in `halo_sense_prod.ino`, where the include of `Sense_Minimal.ino` puts the globals in scope) via a local RAII `DmaReserveTlsGuard`. Kept in sync across both `shared/` and `halo_sense_prod/` copies of `ProvisioningManager.cpp`.

---

### 3. sense_sleep.h -- Sleep/Wake Management

**Purpose:** Deep sleep entry, wake pin management, EXT0 configuration, pulsing detection, and coordinated sleep handshake.

#### Key Constants

| Constant | Value | Purpose |
|---|---|---|
| `WAKE_PIN_DEASSERT_WAIT_MS` | 1500 | Max wait for wake pin to go inactive |
| `WAKE_PIN_INACTIVE_STABLE_MS` | 50 | Pin must be inactive this long to be "stable" |
| `WAKE_PIN_MITIGATION_MS` | 20 | RTC input reconfiguration delay |
| `WAKE_PIN_FAILSAFE_TIMER_S` | 15 | Timer fallback if wake pin stuck |

#### Key Functions

**`sense_enter_deep_sleep(SenseSleepKind kind)`** -- Full sleep entry sequence:
1. Reset voice session
2. Check holdoff and `sleep_allowed_now()`
3. Check for LCD pulsing (200ms window) -- abort if detected
4. Configure wake pin as RTC input with inactive pull
5. If wake pin active: wait for deassert, apply mitigation, send RELEASE_WAKE request
6. If still stuck: disable EXT0, use timer fallback
7. **Upload flush window** (30s max): Wait for pending uploads to drain
8. Disconnect WiFi; stop BT
9. Flush UART, suspend op_worker_task
10. Deinit camera, enable PWDN hold through sleep
11. Configure wake sources (EXT0 + OTA timer)
12. Send WiFi diagnostic summary and SLEEP_READY to LCD
13. `esp_deep_sleep_start()` (no return)

**`wake_pin_check_pulsing(unsigned long window_ms)`** -- Detects if LCD is actively pulsing the wake pin (any transition within window). Returns true on first toggle detected.

**`sleep_wait_wake_pin_deassert(unsigned long wait_ms)`** -- Waits for wake pin to stabilize at inactive level. Counts transitions; aborts early if 3+ toggles (LCD pulsing).

**`uart_send_release_wake_request()`** -- Sends `RELEASE_WAKE` JSON message to LCD, asking it to stop driving the wake pin. Used as last resort before disabling EXT0.

**`sense_config_deep_sleep_wakeup(bool enable_ext0, uint32_t fallback_timer_s)`** -- Configures EXT0 wake on GPIO2 LOW with pullup, plus `ota_configure_timer_wakeup()` for scheduled OTA.

#### Wake Cause Diagnostics

**`print_wake_cause(esp_sleep_wakeup_cause_t cause)`** -- Logs wake source (EXT0, TIMER, COLD_BOOT) with boot count and uptime.

---

### 4. sense_wifi.h -- WiFi Connection Management

**Purpose:** WiFi connection/disconnection, guard state machine, background maintenance, time sync, and recovery.

#### WiFi Guard State Machine

States: `DISCONNECTED` -> `CONNECTING` -> `CONNECTED` | `FAILED` | `FAILED_TIMEOUT`

The guard prevents concurrent connection attempts using a claim/release pattern with `wifi_connect_mutex`.

#### Key Functions

**`ensure_wifi_connected(const char* reason, uint32_t timeout_ms)`** -- Primary connection function:
- Returns immediately if already connected
- Respects cooldown (`WIFI_BEGIN_COOLDOWN_MS` = 2s) unless bypass reasons (maintenance, pre_sleep, voice_upload)
- Claims connect ownership via mutex
- Calls `WiFi.begin()` with provisioned or hardcoded credentials
- Polls for connection with retry on failure

**`service_wifi_maintenance(unsigned long now_ms)`** -- Called from `loop()`:
- Monitors inflight connections for timeout
- Starts new connection attempts with exponential backoff (2s, 4s, 8s, capped 16s)
- Hard resets WiFi after `WIFI_MAINT_MAX_FAILS_BEFORE_RESET` (3) consecutive failures

**`wifi_hard_reset_and_reconnect(const char* reason, uint32_t timeout_ms)`** -- Nuclear recovery:
- Calls `hardResetSta()` (disconnect + mode OFF + mode STA)
- Reconnects with fresh credentials

**`ensure_time_valid(const char* reason, uint32_t timeout_ms)`** -- NTP time sync:
- Checks system time >= `TIME_VALID_MIN_EPOCH` (1700000000, ~Nov 2023)
- Tries RTC/NVS time cache bootstrap first
- Falls back to SNTP (pool.ntp.org, time.nist.gov, time.google.com)
- Polls for up to 15s

#### WiFi Diagnostic Accumulator (`WifiDiagAccum`)

Tracks per-wake-cycle WiFi stats:
- Connect attempts, successes, failures
- RSSI min/max/last
- Total connected/disconnected time
- Last fail reason and status
- Finalized and sent to LCD via `WIFI_DIAG_SUMMARY` before sleep

---

### 5. sense_upload.h -- HTTP Request Core

**Purpose:** HTTP POST/GET with retry logic, DMA guard, queue serialization, and network readiness checks.

#### Key Functions

**`http_post_json_with_retries()`** -- POST with up to 3 retries:
- Releases DMA reservation (RAII guard for re-acquire)
- Yields to foreground user actions (scan/voice)
- Ensures WiFi + time valid before each attempt
- Exponential backoff: 500ms, 1500ms, 3500ms
- Serializes via `http_mutex` (`http_queue_lock/unlock`)
- On unlock: processes deferred WiFi recovery if needed

**`http_get_with_retries()`** -- GET with same retry/DMA/foreground logic.

**`net_ready_for_tls(char* why, size_t why_len)`** -- Quick readiness check:
- WiFi connected?
- Has IP address?
- System time >= 1700000000?

**`http_queue_lock/unlock()`** -- Serializes HTTP requests:
- Takes `http_mutex` (portMAX_DELAY)
- Sets `http_inflight = true` (checked by camera init and sleep)
- On unlock: triggers deferred WiFi recovery if `wifi_recover_requested`

---

### 6. sense_upload_exec.h -- Upload Execution

**Purpose:** Presign URL request and S3 PUT upload with chunked write.

**Nutrition removal (2026-08-21):** the dish-result HTTP poller (`wait_for_dish_result_http()`), `build_dish_result_url()` and `uart_send_ui_meal_result()` were deleted with the nutrition feature. Dish is a plain capture-and-log.

**TLS on the photo PUT (2026-08-20):** the S3 PUT used `tls.setInsecure()` while presign, MQTT and
the OTA paths all validated — the one leg carrying user photo data was the one not checking who it
was talking to. It now calls the same `tls_configure()` helper as the rest of the HTTP path (cert
bundle when available, else the pinned Amazon Root CA 1). The presigned URL is fetched over a
validated channel, so this was never wide open, but "the URL is secret" is not peer authentication.

This was left unfixed for a while because the path has a heap-exhaustion panic history (the AES-DMA
fault fixed in 6.1.815) and validation costs more heap than `setInsecure`. Measured on hardware,
that fear did not materialise: `[UPLOAD_TLS] validated heap_before=35956 after=36168 delta=+212
dma_largest=17396`, `PUT status: 200`, 4/4 captures. Heap is logged either side of the handshake
on every PUT so a future regression is visible rather than inferred; if free heap or
`dma_largest` collapses here, revert.

#### Key Functions

**`get_presign_checkin(PresignReply& out, ...)`** -- Gets presigned URL for check-in uploads:
- POST to `CHECKIN_API_BASE_URL/presign`
- Includes user_id, device_id, content_type, expiry, quantity, camera metadata
- Returns job_id, put_url, s3_key, content_type, ttl_s

**`put_to_presigned_url()`** -- Raw TLS PUT to S3:
- Releases DMA reservation (RAII guard)
- Parses URL into host/port/path
- TLS connect with 60s timeout, 2 retry attempts
- Chunked write (2048 bytes per chunk)
- **Foreground preemption**: Checks `foreground_active` and `dish_upload_pending()` during write loop; aborts immediately
- On TLS connect failure: hard WiFi reset before retry
- Reads HTTP response status line

**`wait_for_dish_result_http()`** -- Polls for AI analysis result:
- Uses result_url from presign response (or builds from user/device/job IDs)
- Polls with `http_get_with_retries()` in a loop
- Handles states: pending, fast (intermediate result), final (complete)
- Respects `DISH_RESULT_TIMEOUT_MS` (60s) and job deadline


---

### 7. sense_upload_queue.h -- Upload Queue Management

**Purpose:** FreeRTOS queue wrappers for image and voice upload jobs.

#### Key Functions

**`upload_queue_count()`** -- Total items across both queues (normal + dish).

**`upload_queue_is_full()`** -- True if total >= `UPLOAD_QUEUE_MAX` (10).

**`queue_upload_job()`** -- Enqueue image upload:
- All modes go to `upload_queue` via `xQueueSend` (plain FIFO; the dish priority queue was removed 2026-08-21)
- Normal jobs go to `upload_queue` via `xQueueSend`
- Copies mode, expiry, quantity, camera metadata, image buffer pointer

**`queue_voice_upload_job()`** -- Enqueue voice upload:
- Always goes to `upload_queue` (normal priority)
- Sets `is_voice = true`, mode = "voice"

**`sleep_defer_queued_background_uploads()`** -- Pre-sleep queue drain:
- Saves first eligible job to SPIFFS persistence
- Frees image buffers for remaining jobs
- Clears active dish job references

**`allocate_upload_buffer(size_t len, bool* used_psram)`** -- PSRAM-first allocation for upload image copies.

---

### 8. sense_upload_persist.h -- Upload NVS Persistence

**Purpose:** SPIFFS-backed retry storage for failed uploads. Saves one upload job (image + metadata) to flash for replay on next boot.

**Guard:** Entire module requires `HALO_SENSE_PROD_WRAPPER && HALO_SENSE_UPLOAD_PERSISTENCE`.

#### Storage Format

Two files on SPIFFS:
- `/upload_retry.meta` -- `PersistedUploadMeta` struct (magic `0x48555031` "HUP1", version 2)
- `/upload_retry.bin` -- Raw image/audio data

#### Key Constants

| Constant | Value | Purpose |
|---|---|---|
| `UPLOAD_PERSIST_MAX_RETRIES` | 5 | Max retry attempts before dropping |
| `UPLOAD_PERSIST_MAX_AGE_S` | 86400 | 24h max age for persisted uploads |
| `UPLOAD_PERSIST_MAX_IMAGE_BYTES` | 1048576 | 1 MB max image size |
| `UPLOAD_PERSIST_FREE_RESERVE_BYTES` | 65536 | 64 KB free space reserve on SPIFFS |
| `UPLOAD_PERSIST_VOICE_REPLAY_BACKOFF_MS` | 60000 | 60s backoff for voice replay |

#### Key Functions

**`upload_persist_save(const UploadJob& job, uint8_t next_retries)`** -- Save job to SPIFFS.

**`upload_persist_load(UploadJob* job)`** -- Load job from SPIFFS with validation (magic, version, retries, staleness).

**`upload_persist_maybe_replay()`** -- Called from `loop()`:
- Checks WiFi connected, no inflight operations, queue empty
- Loads persisted job and re-queues it
- One attempt per boot (`g_upload_persist_attempted_this_boot`)
- Deferred after panic/WDT resets (60s backoff)

**`upload_persist_handle_failure(const UploadJob& job, const char* reason)`** -- Called on upload failure to persist for retry.

---

### 9. sense_voice.h -- Voice Recording and Upload

**Purpose:** Audio capture via I2S callback, WiFi connection for voice, session management, and PCM upload to quick-ack API.

#### Voice Session Management

Sessions track conversation context across multiple voice turns:
- Session ID format: `{device_id}-{boot_count}-{millis}`
- Idle timeout: `VOICE_SESSION_IDLE_TIMEOUT_MS` (90s)
- Backend can override session ID via response metadata

#### Key Functions

**`voice_audio_callback(const int16_t* samples, size_t num_samples)`** -- I2S recording callback:
- Copies PCM samples to `voice_audio_buffer` (512 KB in PSRAM)
- Tracks peak amplitude, sum, sample count, nonzero count
- Auto-stops when buffer full and sets `voice_finalize_requested`

**`voice_upload_and_parse()`** -- Upload raw PCM to async API:
- Releases DMA reservation (RAII guard)
- POST to `QUICK_ACK_BASE_URL/voice-ack-async`
- Headers: `Content-Type: audio/pcm`, `x-audio-sample-rate: 16000`, `x-audio-format: pcm_s16le_mono`, `x-session-id`, `x-owner-id`, `x-device-id`, `x-client-surface: halo`
- 2 retry attempts with WiFi hard reset between
- Success means backend durably accepted the work; no result wait

**`voice_ensure_wifi_connected()`** -- Aggressive WiFi connect for voice (3 attempts with full reset between).

**`voice_begin_wifi_preconnect()`** -- Non-blocking WiFi.begin() when voice recording starts.

---

### 10. sense_mqtt.h -- DELETED (2026-08-21)

Removed with the nutrition feature. MQTT existed solely to receive dish
analysis results; `on_mqtt_message()` handled nothing else. All of
`waiting_for_mqtt_result`, `mqtt_wait_deadline`, `current_result_local_job_id`,
`current_result_mode`, `active_dish_job_id`, `clear_active_dish_job()` and
`g_dish_timing` went with it.


### 11. sense_presign.h -- Presign URL Generation

**Purpose:** Request presigned S3 upload URLs from the backend API.

#### Key Functions

**`get_presign(PresignReply& out, const char* mode, ...)`** -- Mode-aware dispatch:
- **Dish mode**: POST to `DISH_PRESIGN_URL` with type="dish"
- **Discard mode**: POST to `CHECKIN_API_BASE_URL/presign` with type="discard", fallback to `/presign/discard`
- Returns `PresignReply`: job_id, put_url, s3_key, content_type, result_url, ttl_s

**`do_presign_request()`** -- Full presign POST with all fields (user_id, device_id, owner, action, type, content_type, expiry, add_to_shopping_list, camera_meta).

**`do_presign_request_simple()`** -- Simpler presign for basic operations.

---

### 12. sense_scan.h -- Scan Operation Pipeline

**Purpose:** Helpers for the SCAN operation flow: screen hints, mode classification, UI status emission, inflight tracking, and result context management.

#### Mode Classification

| Function | Matches |
|---|---|
| `scan_mode_is_dish()` | "dish", "dish-log", "dish_log" |
| `scan_mode_is_discard()` | "discard" |
| `scan_mode_is_check()` | "check-in", "check-out", "check_out" |
| `scan_mode_is_quiet()` | check-in, check-out, discard (suppress most UI) |

#### Scan Flow (by mode)

**Dish mode:**
1. LCD shows HOLD_STILL (CAPTURING)
2. Sense captures image
3. LCD shows LOGGED for 2s (DONE)
4. Background: presign + PUT upload, deferred to the sleep flush like every other mode

**Check-in mode:**
1. LCD shows HOLD_STILL (CAPTURING)
2. Sense captures image
3. LCD shows EXPIRY input (WAITING_INPUT)
4. User enters expiry date/quantity
5. LCD shows LOGGED for 2s (DONE)
6. Background: presign + upload

**Discard mode:** Same as check-in but with add-to-shopping-list option.

#### Key Functions

**`scan_ui_status_emit()`** -- Sends `UI_STATUS` to LCD with phase, mode, job_id, and auto-generated screen hint. Suppresses most phases for quiet modes.

**`dish_upload_pending()`** -- Deleted 2026-08-21. Dish has no special upload handling.

**`foreground_scan_pending()`** -- True if a scan is actively in progress (for sleep blocking).

---

### 13. sense_op_queue.h -- Operation Job Queue

**Purpose:** Op-queue enqueue, foreground priority detection, upload job parking/requeueing.

#### Key Functions

**`enqueue_op_job(const OpJob& job, bool prioritize_front, const char* source)`** -- Queue an operation job. `prioritize_front` uses `xQueueSendToFront` for user-initiated actions.

**`foreground_priority_active(unsigned long now_ms, const char** reason_out)`** -- Detects if a foreground user action should block background work:
- Active foreground job or scan/voice recording
- Queued user-priority job
- Recent input wake (< 2.5s)
- Recent user activity (< 1.8s)
- Recent LCD communication (< 1s)

**`park_upload_job_if_foreground_active()`** -- Parks an upload job when foreground is active. The upload worker resumes it when foreground clears.

**`upload_wait_for_foreground_clear_in_place()`** -- Blocks the upload worker until foreground clears, with deadline awareness.

---

### 14. sense_uart_msg.h -- UART Message Sending Helpers

**Purpose:** Formatted UART JSON message construction and transmission for various protocol messages.

#### Messages

| Function | Type Field | Purpose |
|---|---|---|
| `uart_send_sync_ack()` | `SYNC_ACK` | Acknowledge link sync |
| `uart_send_ui_toast(msg)` | `UI_TOAST` | Display transient message on LCD |
| `uart_send_ui_voice_response(json)` | `UI_VOICE_RESPONSE` | Voice assistant response with progressive truncation |
| `uart_send_pong()` | `PONG` | Reply to INPUT_PING |
| `uart_send_link_hb()` | `LINK_HB` | Periodic heartbeat |
| `uart_send_sleep_ready()` | `SLEEP_READY` | Sense ready for deep sleep |
| `uart_send_sleep_intent(reason)` | `SENSE_SLEEP_INTENT` | Sense wants to sleep |
| `uart_send_sleep_deny(reason, retry_ms)` | `SLEEP_DENY` | Sense cannot sleep now |
| `uart_send_release_wake()` | `RELEASE_WAKE` | Ask LCD to release wake pin |

#### Voice Response Truncation

`uart_send_ui_voice_response()` progressively removes fields to fit within `kMaxVoiceJsonChars` (2400):
1. Full response (UI screens + quick items + transcript)
2. Remove UI screens
3. Remove quick items
4. Remove transcript
5. Truncate text to 1600, then 1000 chars
6. Fallback error message

---

### 15. sense_list.h -- Shopping List API

**Purpose:** Fetch, parse, and manage the shopping list from the Trepo backend.

#### Key Functions

**`fetch_shopping_list_from_api()`** -- Fetches list via HTTPS POST:
- POST to `TREPO_API_BASE_URL/v1/list` with operation="view"
- 2 retry attempts with backoff on code=-1
- DNS readiness check before request
- Parses response into `g_shopping_list[]` array

**`parse_and_update_shopping_list(const String& json)`** -- Parses JSON items:
- Skips items with action="CHECKED"
- Extracts product_name, id, household_item_uuid (stored in `huuid[64]`, empty if missing), and `store` (stored in `store[48]`, empty if missing/null)
- After the final count is set, `qsort`s the in-RAM list via `shopping_list_cmp_by_store()` to group items by store name (empty store sorts last) so the LCD can render store-group headers. Safe to reorder: delete is by id, and `g_selected_index` is reset to 0 afterward (not preserved by position)
- Thread-safe via `g_list_mutex`
- Records `list_last_fetch_ok_ms` on success (powers the cooldown served-cached path in `request_list_refresh()`)
- Sends `UI_LIST` to LCD after update

**`delete_item_from_api(const char* item_id)`** -- Removes item via POST with operation="remove",
matching the iOS app's swipe-delete exactly:
- Looks up the item's `household_item_uuid` by id in `g_shopping_list[]` (under `g_list_mutex`) and
  sends `{"operation":"remove","ownerId":...,"device":TREPO_DEVICE_ID,"itemUUID":<huuid>}`. The
  backend deletes by **itemUUID** (the household-wide key, propagates to ALL household members'
  tables) — NOT the per-table row id.
- If no huuid is cached (empty), falls back to the legacy `{"id":...}` body and logs a warning.
- **On HTTP 200**: also removes the item from the RAM cache `g_shopping_list[]` (under mutex via
  `remove_item_from_ram_list_locked()` — compacts array, fixes `g_list_count`/`g_selected_index`)
  so a cached serve (`request_list_refresh()` cooldown path) can't resurrect the deleted item. No
  `UI_LIST` push — the LCD already removed it locally. Logs `[DELETE] ok itemUUID=...`.
- **On failure**: RAM list is left unchanged; sends `uart_send_ui_list()` so the LCD's optimistic
  removal reconverges with reality (the LCD's `deleted_item_ids` RAM filter may still hide the item
  this boot — acceptable, logged). Logs `[DELETE] fail code=...`.

**`remove_item_from_ram_list_locked(int list_index)`** -- Compacting removal helper; caller must
hold `g_list_mutex`.

---

### RTC memory: `RTC_DATA_ATTR` does NOT survive `esp_restart()`

A trap that cost a real bug, found 2026-08-20. The two attributes are not interchangeable:

| Attribute | Section | Deep-sleep wake | `esp_restart()` | Power-on |
|---|---|---|---|---|
| `RTC_DATA_ATTR` | `.rtc.data` (**initialised**) | survives | **re-initialised from the image** | re-initialised |
| `RTC_NOINIT_ATTR` | `.rtc_noinit` (not initialised) | survives | **survives** | garbage |

`g_cam_wedge_restarts` — the counter bounding the camera self-heal restart budget — was
`RTC_DATA_ATTR`, so it was zeroed by the very `esp_restart()` it was counting. Measured on
hardware: eight consecutive self-heal restarts all reported `wedge_restart code=1`, never 2, and
`restart_budget_exhausted` never fired. A genuinely faulty camera would have rebooted the device
forever — exactly the boot loop the cap exists to prevent. The code comment claiming it "survives
deep sleep and SW reset" was simply wrong.

Fixed to `RTC_NOINIT_ATTR` plus a magic word (`CAM_WEDGE_MAGIC`), because `.rtc_noinit` holds
garbage after a power cycle and needs an explicit validity check to distinguish "fresh device" from
"restarted mid-self-heal".

**Audit rule:** any counter that must survive a self-heal restart needs `RTC_NOINIT_ATTR` + a
magic. Any state that only needs to survive deep sleep can stay `RTC_DATA_ATTR`. Check which one
you actually need — the failure is silent, and it hides behind seeded tests that write the value
directly.

### 16. sense_time.h -- NTP/RTC Time Cache

**Purpose:** Persist last-known epoch to RTC memory and NVS so TLS can bootstrap time after deep sleep without waiting for NTP.

**Owner timezone (2026-08-20).** The nightly maintenance wake is scheduled at **02:00 local**, so
the zone is not cosmetic. `sense_set_timezone()` applies a POSIX TZ string; `HALO_DEFAULT_TZ` is
`PST8PDT,M3.2.0,M11.1.0`.

The real defect was in the prod wrapper, not here: `ensure_timezone_pt()` hardcoded
`setenv("TZ","PST8PDT,…")` and runs on **WiFi connect** — i.e. *after* `setup()` applies the zone
loaded from NVS — so it silently overwrote it and forced US Pacific on every device in the fleet.
**A backend change alone would never have fixed this**; the firmware discarded whatever arrived,
while `nextwake` reported the wrong zone convincingly. It now reads NVS and applies the owner's
zone, logging `[TZ] set=<tz> source=nvs|default reason=<why>`.

Plumbing: `ProvisioningState::load/saveTimezone` (NVS key `tz`); the claim-response parser accepts
`timezone` or `tz` (response doc grown 256 → 384 so a TZ string cannot silently truncate the
parse); absent/empty leaves the existing value alone rather than resetting it to Pacific.
Verified end to end on hardware: stored `EST5EDT,M3.2.0,M11.1.0` → reboot → applied from NVS →
wake target `2026-08-21 02:00:00` Eastern, 48,202 s out — correct to the second.

**Backend still owes** the field in the claim 200 response. It must be a POSIX string WITH the DST
rule, not an IANA name (`America/New_York` will not work — there is no tz database on the ESP32),
and already-claimed devices never re-claim, so moving the existing fleet off Pacific needs a
backfill path.

#### Storage

- **RTC memory**: `g_time_cache_epoch` (survives deep sleep, lost on power cycle)
- **NVS (Preferences)**: Namespace `time_cache`, key `epoch` (survives power cycle)

#### Key Functions

**`time_cache_store(time_t now)`** -- Saves epoch to RTC + NVS. Rate-limited: skips if < 1 hour since last store.

**`time_cache_bootstrap(const char* reason)`** -- Restores time from cache:
1. Check RTC memory first (fastest)
2. Fall back to NVS Preferences
3. Set system time via `settimeofday()`
4. Returns true if time is valid (>= 1700000000)

**`time_cache_load()`** -- Loads from RTC memory, falls back to NVS.

---

### 17. sense_ota_lcd.h -- LCD OTA Proxy

**Purpose:** Sense board downloads LCD firmware from S3 over HTTPS, then streams it to the LCD board via UART using COBS-framed `UartOtaProtocol`. The LCD cannot do TLS directly due to insufficient DMA RAM.

#### Constants

| Constant | Value | Purpose |
|---|---|---|
| `LCD_OTA_PROXY_CHUNK_SIZE` | 512 | Bytes per COBS frame |
| `LCD_OTA_PROXY_MAX_RETRIES` | 5 | Per-chunk retry limit |
| `LCD_OTA_PROXY_ACK_TIMEOUT_MS` | 3000 | Wait for chunk ACK |
| `LCD_OTA_PROXY_QUERY_TIMEOUT_MS` | 7000 | Wait for LCD version query (per attempt) |
| `LCD_OTA_PROXY_BEGIN_TIMEOUT_MS` | 45000 | Wait for OTA partition erase and begin ACK |
| `LCD_OTA_PROXY_END_TIMEOUT_MS` | 60000 | Wait for SHA verify + reboot |
| `LCD_OTA_PROXY_DOWNLOAD_TIMEOUT_MS` | 2400000 | 40 min overall download limit |
| `LCD_OTA_PROXY_STALL_WINDOW_MS` | 30000 | No-data interval ends the current session |

**Cold-wake LCD query budget = 35s.** `sense_lcd_ota_query()` retries the version query
`LCD_OTA_QUERY_ATTEMPTS = 5` times at `LCD_OTA_PROXY_QUERY_TIMEOUT_MS = 7000` ms each
(5 x 7s = up to 35s, raised from the old 3 x 5s = 15s). The scheduled-OTA case wakes the
LCD from its own deep-sleep timer at the maintenance window, so the LCD may still be
running LVGL init when the Sense's first `LCD_OTA_QUERY` arrives; the larger budget gives
the LCD time to boot and answer before the proxy gives up.

#### Mailbox Pattern

The proxy task cannot read `lcdSerial` directly (main loop owns it). Instead, `parse_input_message()` in the main loop dispatches LCD OTA responses into mailbox variables:
- `g_lcd_ota_query_resp_ready/fw/part_size` -- LCD_OTA_QUERY_RESP
- `g_lcd_query_running_part[16]` / `g_lcd_query_running_state[20]` / `g_lcd_query_boot_part[16]` / `g_lcd_query_boot_ready` -- fresh LCD partition/state/readiness fields captured from LCD_OTA_QUERY_RESP. Exposed via `sense_lcd_last_running_part()`, `sense_lcd_last_running_state()`, `sense_lcd_last_boot_part()`, `sense_lcd_last_boot_ready()` getters. Every response resets missing fields to empty/UNKNOWN/false.
- `g_lcd_ota_begin_ack_ready/accepted/reason/resume_offset` -- LCD_OTA_BEGIN_ACK
- `g_lcd_ota_end_ack_ready/sha_match/ota_ok` -- LCD_OTA_END_ACK. `sha_match` is the LCD's SHA256 verification result; `ota_ok` reflects whether `esp_ota_set_boot_partition()` actually succeeded on the LCD. `parse_input_message()` stores both data flags **before** setting `ready=true` so the waiting proxy never observes `ready` with a stale `ota_ok`.

During binary streaming, `g_lcd_ota_proxy_owns_uart = true` prevents the main loop from reading `lcdSerial`.

#### OTA Proxy Flow

**`sense_lcd_ota_proxy(const OtaManifest& manifest, const char* lcd_fw_version)`**:

1. **Version check**: Compare manifest version against LCD current version. Skip if up-to-date.
2. **Begin session**: Send `LCD_OTA_BEGIN` with a fresh session_id, image_size, sha256, version. Wait for `LCD_OTA_BEGIN_ACK`; require `resume_offset=0` because the LCD cannot restore its SHA context.
3. **Download + stream**: HTTP GET firmware binary from S3. For each 512-byte chunk:
   - Send COBS frame (`MSG_CHUNK`) with sequence number
   - Wait for `MSG_ACK` with matching sequence
   - Retry up to 5 times on timeout/NACK
   - Progress logged every 10%
   - Stream timeout reduced to 50ms for throughput
   - **Bounded fresh-session recovery:** disconnect or 30s without data ends this session with `"timeout"`. The wait uses time since the last data, including the boundary at exactly 30s. There is one HTTP GET per session; no Range reopen or in-session reconnect remains. `http.setReuse(false)` and `close_stream()` (`http.end()` plus `tls_client.stop()`) discard the incomplete body on every exit. The old forced-stall/reconnect test hook was removed. Network calls can block beyond the loop's timer checks; the 40-minute check is not a hard wall-clock limit around DNS/TLS/HTTP calls.
4. **End session**: Release UART ownership. Send `LCD_OTA_END` with image_size and sha256. Wait for `LCD_OTA_END_ACK`, which carries both `sha_match` (SHA verification result) and `ota_ok` (`esp_ota_set_boot_partition()` result on the LCD).
5. **Result**: Returns "success", "up_to_date", "sha_mismatch", "lcd_boot_part_fail", "chunk_retry_exhausted", "timeout", etc. **Success requires `sha_match && ota_ok`.** The proxy checks `sha_match` first (→ `"sha_mismatch"` on failure), then `ota_ok`: if the LCD verified the SHA but failed to set its boot partition (`ota_ok=0`), the proxy returns the new verdict `"lcd_boot_part_fail"` (recorded via `diag_record_error_persistent("lcd_ota", -1, "lcd_boot_part_fail")`) rather than falsely reporting success — a transfer that hashes correctly but won't boot the new image no longer masquerades as a success.

**Inline proxy pumps its own UART for JSON-mode handshake waits:** the proxy now runs **inline on the main task** (called from `maybeRunOtaCheck()`), so `loop()` is blocked and its concurrent UART RX drain does NOT run while the proxy waits. The three JSON-mode mailbox waits therefore each call `pump_uart_rx_once(); delay(10);` inside their wait loops so the proxy drains+parses incoming UART itself and the LCD's ACK actually reaches the mailbox: the `LCD_OTA_QUERY_RESP` wait (in `sense_lcd_ota_query()`), the `LCD_OTA_BEGIN_ACK` wait (step 2), and the `LCD_OTA_END_ACK` wait (step 4). At both the BEGIN_ACK wait (before `g_lcd_ota_proxy_owns_uart` is ever set true) and the END_ACK wait (after it has been set back to false), `g_lcd_ota_proxy_owns_uart == false`, so `pump_uart_rx_once()` (which early-returns when that flag is true) actually runs. The binary chunk-streaming loop (step 3) deliberately does NOT pump — it owns the UART (`g_lcd_ota_proxy_owns_uart = true`) and uses the COBS protocol's own RX path.

**Non-OTA network suppression:** while any OTA activity is in flight, `request_list_refresh()` is short-circuited (`[LIST_REFRESH] deferred (lcd_ota_in_progress)`) and the `INPUT_WAKE` handler skips its list-refresh trigger. This prevents an HTTP GET to `/v1/list` from competing with the S3 download on the single net stack (which previously starved the download to a stall/abort). The gate is `lcd_ota_in_progress()`, defined in `halo_sense_prod.ino` and forward-declared in `Sense_Minimal.ino` (weak `return false` fallback for non-wrapper builds). It returns true if any of `g_lcd_ota_proxy_owns_uart`, `g_lcd_ota_request_active`, `g_lcd_ota_task_running`, `g_ota_apply_in_progress`, or `g_ota_check_in_progress` is set. OTA's own HTTP (manifest fetch, S3 download, schedule/report) is NOT gated.

**`sense_lcd_ota_query()`** -- Query LCD firmware version and OTA partition size. The dispatch also captures fresh `running_part`, `running_state`, `boot_part`, and `boot_ready`. The optional fourth argument `expected_boot_fw` enables strict postboot confirmation in the existing 35s budget: the first 7s slot pumps RX without transmitting, allowing END finalization/reboot to settle; up to four fresh query slots remain. A response must match the expected version, report VALID, identify equal nonempty/non-`?` running/boot partitions, and set `boot_ready=true`. A fast not-ready response waits out its slot before the next query; all waits share the absolute budget. This mode sends only queries, with no LOCK, UNLOCK or BEGIN during proof. Default three-argument queries retain their existing behavior.

**LCD-only resolved transfers require postboot proof.** `prod_proxy_lcd_inline()` uses that mode after a successful byte transfer or an at/above-target retry response. Only a confirmed boot persists `updated`/`noop`, the actual queried version and a fresh cache, then clears LCD debt. Failure records `lcd_postboot_unconfirmed`, clears the version cache, leaves `lcd_ota_due=1`, and stops the transfer loop so a second version-only attempt cannot bypass proof. The caller clears check/force state and sends its final unlock to end the current check; this is deferral, not confirmed application, and does not clear LCD continuation NVS. Initial both-current checks and the paired LCD→Sense path remain unchanged. Receivers predating `boot_ready` can still answer ordinary/pretransfer queries, but LCD-only postboot proof safely defers after the bounded wait; use a target implementing the new field.

**`prepare_lcd_ota_proxy_retry()`** (production wrapper) sends a fresh `OTA_LOCK`, then requires a new LCD query response before the second attempt. Both paired-update and LCD-only inline paths use it. A failed query leaves LCD unresolved without sending another BEGIN; a response at or above the target version resolves the update without another transfer, preserving `noop` and the actual queried version. In the paired path a completed transfer records `updated` with the target version and invalidates the cache. LCD-only resolution additionally requires the postboot proof above and keeps that fresh observed version. The receiver allows its existing 10s Home idle interval after failed cleanup for this exchange, then sleeps if no new lock arrives. At most one fresh-session retry runs, starting at byte zero; final failure keeps `lcd_ota_due=1`, unlocks, and defers Sense self-OTA.

**Terminal unlock:** `send_ota_uart_message("OTA_UNLOCK", true)` adds `terminal:true` only when the synchronous production check is finished (including the final both-current path, unresolved-LCD deferral, proven final error/policy returns and verified post-OTA boot). The LCD then clears generic/recovery stay-awake holds and resumes normal idle behavior. The pre-inline `release_waiting_lcd_ota()` unlock remains unmarked because a query/transfer may follow. Async task/proof and apply-failure handoffs retain legacy semantics; older LCD firmware ignores the optional field.

**Stale-RX flush (hardening):** Inside the per-attempt loop, immediately after clearing the mailbox (`g_lcd_ota_query_resp_ready = false`) and before sending the query JSON, the function drains the hardware FIFO (`while (lcdSerial.available() > 0) lcdSerial.read();`) and resets the RX ring / partial-frame state (`uart_reset_rx_state()`). During the Sense's HTTPS-blocking self-OTA window the main-loop UART drain is starved, so a backlog/overflow can accumulate on `lcdSerial` and desync parsing of the fresh `LCD_OTA_QUERY_RESP` (the no-response failure mode). Ordering is mailbox-clear → raw-flush → send fresh query, so an already-parsed response cannot be dropped.

**`uart_send_fw_info(bool do_lcd_query = true)`** -- Emits a `FW_INFO` JSON reporting the Sense firmware version plus the LCD firmware. Both paths also include additive live Sense diagnostics: `sense_build`, `sense_running_part`, `sense_running_state`, `sense_boot_part`, and numeric ESP-IDF enums `sense_wake_cause` / `sense_reset_reason`. The running/boot partition APIs are read at reply time; `sense_running_state` is `NEW|PENDING_VERIFY|VALID|INVALID|ABORTED|UNDEFINED|UNKNOWN`. Missing partition pointers report `"?"`; a failed state read reports `"UNKNOWN"`, never an assumed valid state. Build identity is `kBuildId` (`"unknown"` in non-production builds). The JSON capacity is 640 bytes; message type, required protocol fields, and existing fields are unchanged. These fields let LCD USB diagnostics verify the Sense running image without opening Sense USB and changing its wake/reset evidence. The `do_lcd_query` parameter selects between two paths:

**`do_lcd_query = true` (default; INPUT_FW_INFO / diagnostic path):** Triggers a FRESH `sense_lcd_ota_query()` round-trip (BLOCKING — up to 7000ms × 5 attempts), then reports the REAL running firmware of BOTH boards plus LCD partition/state. Fields:
- `sense_fw` -- live running Sense version (`kFirmwareVersion`)
- `lcd_fw` -- freshly queried LCD running version (NOT the cached OTA manifest value); `"unknown"` if the query fails/times out
- `lcd_running_part` -- LCD running partition label (omitted on query failure)
- `lcd_running_state` -- `NEW|PENDING_VERIFY|VALID|INVALID|ABORTED|UNDEFINED|UNKNOWN` (omitted on query failure)
- `lcd_boot_part` -- LCD boot partition label (omitted on query failure)
- `lcd_fw_age_s` -- duration of the fresh query in seconds (omitted on query failure)

A failed/timed-out query does not block: `sense_fw` is always emitted. This is the behavior automation depends on (fresh `lcd_fw` / `running_state`).

**`do_lcd_query = false` (INPUT_SENSE_FW / fast Settings path):** Skips `sense_lcd_ota_query()` entirely and replies IMMEDIATELY. Fields:
- `sense_fw` -- live running Sense version (`kFirmwareVersion`), known instantly
- `lcd_fw` -- CACHED LCD version (`g_lcd_ota_version`, populated by the pre_sleep LCD query); `"unknown"` if the cache is empty

The LCD partition/state fields are omitted; the live Sense fields above remain present. The LCD Settings screen uses this path because it only reads `sense_fw` and must not be held hostage for seconds by the blocking LCD query.

Observability only; does not affect OTA control flow.

**Unplugged automatic-trigger evidence.** `nightly_maintenance_tick()` captures the original persistent `SENSE_DIAG` begin record in RAM: event (`ota/nightly_begin` or `ota/lcd_recovery_begin`), reason, raw wake code, and the original wake/reset/epoch detail. It sends that record when the episode begins, then repeats the same payload once after a fresh LCD query succeeds during that synchronous automatic check, before any binary BEGIN or UART quiet period. This covers both paired-update and Sense-current/LCD-only paths; retries cannot repeat it again. Only the envelope timestamp/message ID are new. A manual check, including one arriving while an earlier automatic episode is pending after a guard skip, cannot replay the automatic record. Queue/finish and reboot clear replay eligibility. The unchanged 96-byte detail capacity fits both production reasons even with maximum 32-bit numeric fields (66 bytes plus NUL).

The LCD's existing `persist=true` handler stores received copies in its error ring for later USB `errors` retrieval. This corroborates the original automatic trigger; it does not prove an image was applied or marked valid. Use fresh `FW_INFO` plus LCD `fw` for resulting versions and states. At most one corroborating repeat is added per automatic episode, with no per-loop NVS writes or scheduling changes. Delivery still has no diagnostic acknowledgment, and the bounded ring may overwrite old evidence.

**Proactive `FW_INFO` push (boot + every SYNC).** Beyond the two query-triggered paths above, the Sense now PUSHES `FW_INFO` unsolicited via `uart_send_fw_info(false)` in two places, so the LCD learns the Sense version promptly (especially right after a manual OTA, when the LCD used to fail with "couldn't find the sensor" because the Sense only answered queries and the query landed mid-reboot):
- **On boot:** `halo_prod_setup()` calls it immediately after `handle_pending_ota_expectation()`. Fast/non-blocking so it never stalls boot.
- **On every (re)sync:** the `SYNC` handler calls it right after `uart_send_sync_ack()`. This is the key fix — the LCD re-syncs after its OTA reboot, and that SYNC now delivers the new `sense_fw`. Both use `do_lcd_query=false` (never the blocking `true`).

**`sense_lcd_ota_fetch_manifest()`** -- Fetch and parse LCD manifest JSON from S3.

**Cloud-reported `lcd_fw` is the REAL running version (not the manifest):** `truth_get_lcd_fw_version()` returns `g_lcd_ota_version`, which is sourced ONLY from an actual `LCD_OTA_QUERY_RESP` (via `sense_lcd_ota_query()`), never from the OTA manifest. On a successful proxy, the orchestrator clears the cached value (`g_lcd_ota_version[0]='\0'`, `g_lcd_fw_query_ms=0`) instead of writing the assumed manifest version — this previously masked failures by always reporting the target version. The pre-sleep path re-queries the LCD and overwrites the cache with the real booted version when the cache is empty OR stale (`>LCD_FW_QUERY_STALE_MS` = 5 min), provided the UART link is recent and no proxy task is running; `g_lcd_fw_query_ms` (millis) timestamps the last real query.

**Successful inline LCD OTA result survives the Sense self-OTA reboot via NVS.** Because the inline LCD proxy is followed immediately by the Sense self-OTA + reboot, the RAM truth globals (`g_lcd_ota_result`/`g_lcd_ota_version`) are wiped before the post-reboot pre_sleep cloud OTA report is built — so a real success used to surface as `last_lcd_ota_result=unknown` / `last_lcd_fw=unknown`. On `"success"` the orchestrator now sets `g_lcd_ota_result="updated"` and calls `set_lcd_ota_result_nvs("updated", lcd_manifest.version)`, persisting to Preferences namespace `"halo"` keys `lcd_ota_res` (11 chars) and `lcd_ota_ver` (11 chars; both <=15 so they don't silently fail). On the next boot, `halo_prod_setup()` calls `load_lcd_ota_result_nvs()` exactly once (co-located with the `get_lcd_ota_due_nvs()` boot check, before any report is built) which repopulates the globals and then **consumes** (removes) the keys — one-shot, so the success is reported once. The post-reboot cloud report therefore shows `last_lcd_ota_result=updated` + `last_lcd_fw=<target>`; the live pre_sleep LCD query then confirms/corrects `lcd_fw` with the real booted version once the LCD is reachable again.

**Dual-board OTA order — LCD proxy FIRST, then Sense self-OTA (`maybeRunOtaCheck()` in `halo_sense_prod.ino`):** When a Sense update is found and ready to apply, the LCD is proxied **inline, before** the Sense self-OTA, while the LCD is still awake from the button press:
1. Inline (main task, blocking the loop so there is no UART-drain race): `sense_lcd_ota_query()` → `sense_lcd_ota_fetch_manifest(cfg->base_dir, cfg->channel, …)` (via `ota_get_config()`) → `ManifestClient::compareVersions(lcd_manifest.version, lcd_fw) > 0` → `send_ota_uart_message("OTA_LOCK")` + `sense_lcd_ota_proxy(lcd_manifest, lcd_fw)`. Logged as `[OTA_ORCH] lcd proxy result=<res> (attempt=<n>)`. MQTT is already stopped (`mqtt_stop_for_ota()` earlier in the function) and camera DMA already released; the manifest-client connection is released (`g_manifest_client.releaseConnection()`) for TLS headroom. `sense_lcd_ota_proxy()` manages `g_lcd_ota_proxy_owns_uart` itself — not double-managed here. On `"success"` the cached LCD version is invalidated (`g_lcd_ota_version[0]='\0'`, `g_lcd_fw_query_ms=0`). **The update-needed branch proxies with ONE retry** (`for attempt=1..2`): a transient DMA/UART failure on the first attempt does not immediately give up.
2. **Split guard — Sense self-OTA requires positively resolved LCD state.** Before `applyToOtaPartition`, any `!lcd_proxy_succeeded` defers Sense: failed LCD query, failed LCD manifest fetch, or failed transfer after the existing retry. A current LCD or successful proxy sets the flag true. On deferral, `lcd_ota_due=1` remains persisted, the result stays `lcd_proxy_failed_defer`, MQTT is restored, the request is cleared, and `OTA_UNLOCK` releases the finished attempt's update screen. A later boot/manual/nightly check retries. The breadcrumb `sense_apply_deferred lcd_split_guard` records the decision; no image-transfer or reboot code changes.
3. **Otherwise** `g_ota_applier.applyToOtaPartition(...)` + reboot (success never returns; failure keeps the existing MQTT-reconnect / `OTA_UNLOCK` / `recordOtaResult` path).

**`OTA_UNLOCK` is guaranteed on every non-reboot completion/early-return path of `maybeRunOtaCheck()`.** A manual OTA sends `OTA_LOCK` up front (`halo_prod_request_manual_ota()`), so any path that ends WITHOUT a Sense self-apply reboot must release the LCD or it stays stranded on the "Updating…" screen. The pre-existing `release_waiting_lcd_ota()` helper is gated on `g_lcd_ota_request_active`, which is never set true, so it does not actually unlock — explicit `send_ota_uart_message("OTA_UNLOCK")` calls were added:
- **Manifest-error paths (no LCD proxy ever runs):** `manifest_url_invalid`, `manifest_fetch_fail`, `board_mismatch`, `bin_url_disallowed` — send `OTA_UNLOCK` unconditionally before returning.
- **Gate/no-update paths that call `maybe_trigger_lcd_ota_check()`:** `downgrade_blocked`, `rollout_min_version`, `rollout_skip`, `apply_blocked` — send `OTA_UNLOCK` **inside** the existing `if (!g_lcd_ota_task_running)` block. When a proxy task DID start, that task sends its own `OTA_UNLOCK` at completion (and the proxy does NOT re-lock, so an early unlock here would strand the stream); guarding on `!g_lcd_ota_task_running` avoids the double-unlock/premature-unlock. A redundant `OTA_UNLOCK` on the LCD is idempotent/harmless.
- **`up_to_date` path — inline LCD proxy (FIX B), NOT the background task.** When the Sense is already current and only the LCD may be behind, the `version_cmp==0` branch now calls the new static helper **`prod_proxy_lcd_inline()`** instead of `maybe_trigger_lcd_ota_check()`. The background `lcd_ota_proxy_task` DMA-starves and has many skip paths (window/ack gates, `g_lcd_ota_attempted_this_window`), so it frequently never ran — leaving a Sense-ahead/LCD-behind split un-healed. `prod_proxy_lcd_inline()` runs synchronously on the main task with the LCD held awake by `OTA_LOCK`: it does `g_manifest_client.releaseConnection()` → `sense_lcd_ota_query()` → `sense_lcd_ota_fetch_manifest()`; if the LCD is behind it sends `OTA_LOCK` and runs `sense_lcd_ota_proxy()` with ONE retry. On success it clears `lcd_ota_due` (and persists `lcd_ota_result="updated"`); on failure it sets `lcd_ota_due=1` and logs. It does NOT send the final `OTA_UNLOCK` — the caller's existing `if (!g_lcd_ota_task_running)` block does (always taken now, since no background task is spawned). This path heals a Sense-ahead/LCD-behind split on a manual, nightly, or bounded boot recovery check. `maybe_trigger_lcd_ota_check()` is still defined for the nightly/other callers.
- **Untouched:** the successful Sense self-apply path (reboots; `handle_pending_ota_expectation()` sends `OTA_UNLOCK` post-reboot on version match, unless `lcd_ota_due` is still pending) and the existing apply-failure `OTA_UNLOCK`.

**Automatic nightly and LCD recovery checks (`nightly_maintenance_tick()`).** The production path no longer calls `run_maintenance_if_needed()`. A raw `ESP_SLEEP_WAKEUP_TIMER` queues one automatic episode with reason `nightly`; `lcd_ota_due` at any boot queues reason `lcd_due` unless the timer episode is already queued. The existing 120-second bound covers readiness, not a transfer already in progress. No wake cause is inferred from USB reset or manual command.

Normal provisioning, time and health service run before the tick; the obsolete `g_maintenance_mode && !g_maintenance_handled` early return is removed. An old schedule-enable flag cannot suppress servicing a real timer request. Readiness is polled at most once per second. Automatic work waits for user operations, foreground/voice/list activity, queued operations, provisioning SoftAP, or another OTA to finish, then requires provisioning state CONNECTED, WiFi/IP/DNS, valid time, and cooldown permission. It kicks SNTP once without a blocking wait. `maybeRunOtaCheck(reason, true)` skips only the initial boot delay; it does not create a forced/manual intent or bypass OTA guards.

A readiness skip leaves the episode pending. Only a check that actually started (`g_ota_check_done`, set after the OTA guards) or the readiness deadline consumes it; a manifest failure counts as an attempted check. The final sleep guard and OTA sleep-ack guard both honor `halo_prod_boot_ota_pending()` while the deadline is live. Completion clears the pending flag and legacy mode and marks the old mode handled, so later manual work and health validation remain serviceable. `lcd_ota_due` persists until the LCD is positively current; the next boot earns a new bounded recovery opportunity. The same inline LCD-first / Sense-second path above handles manual, nightly, and recovery checks.

**Cooldown scope.** `OtaIntent` tracks the last attempt only in RAM for the current boot. A new boot gets a fresh discovery opportunity; within one boot the ten-minute cooldown and explicit-force bypass remain. Unsigned elapsed-time arithmetic handles `millis()` wrap, and an attempt at uptime zero still counts. Only the result string remains persisted. The removed `last_ota_attempt_ms` key was longer than ESP-IDF's 15-character NVS limit, so the old cross-boot uptime design was invalid but is not evidence that that particular key ever persisted on hardware.

**Persistent OTA-orchestration breadcrumbs (black box, area `ota_orch`):** `maybeRunOtaCheck()` writes concise breadcrumbs to the persistent error-log black box via `diag_record_error_persistent("ota_orch", code, detail)` (forwards to `sense_errlog_store()` + `uart_send_sense_diag_persist()`, so they survive reboots and are readable later via the LCD error log). Because the API is `(stage, code, text)` rather than printf-style, each `detail` string is built with `snprintf` into a local `char crumb[96]` and is prefixed with the event name. Events:
- Check entry (after readiness guards in `maybeRunOtaCheck`) -- `reason=<reason> manual=<override> t=<millis>`
- `lcd_query_tx` (before the inline `sense_lcd_ota_query`) -- `t=<millis> link_recent=<halo_uart_link_recent(3000)>`
- `lcd_query_fail` (code `-1`; inline query returned false) -- `t=<millis>`
- `lcd_query_ok` (query succeeded; emitted on the manifest-fail, update-needed, and up-to-date branches) -- `lcd_fw=<lcd_fw> t=<millis>`
- `lcd_proxy_start` (before each `sense_lcd_ota_proxy` attempt) -- `ver=<lcd_manifest.version> attempt=<n> t=<millis>`
- `lcd_proxy_done` (after each `sense_lcd_ota_proxy` attempt) -- `res=<lcd_res> attempt=<n> t=<millis>`
- `sense_apply_deferred` (code `-1`; split guard — LCD query/manifest/transfer unresolved, Sense self-OTA skipped) -- `lcd_split_guard ver=<manifest.version> t=<millis>`
- `sense_apply_start` (before `applyToOtaPartition`) -- `ver=<manifest.version> lcd_due=<!lcd_proxy_succeeded> t=<millis>`
- `lcd_inline_query_tx` / `lcd_inline_proxy_start` (FIX B `prod_proxy_lcd_inline()` on the `up_to_date` path) -- query TX and per-attempt proxy start for the inline-only-LCD-behind path

These breadcrumbs make the manual-OTA LCD-rendezvous decision/handshake trail visible in the black box even though the Sense reboots on a successful self-OTA.

**`lcd_ota_due` is a fallback only.** The inline-proxy outcome clears it on success/current and sets it on unresolved query, manifest, or transfer. `halo_prod_setup()` queues the bounded `lcd_due` recovery episode above; it does not enter a dead legacy maintenance branch. On a current Sense the inline LCD-only path heals the split; on an older Sense the same fail-closed split guard requires the LCD to resolve before Sense advances.

### MAINT_WINDOW carries the Sense wall clock (`now_epoch`)

`send_maint_window()` (the single sender for every `MAINT_WINDOW` TX — used by
`keep_lcd_awake_for_maintenance()`, `sync_pending_maintenance_to_lcd()`, and the clear/
window-delta paths) adds a **`now_epoch`** field = `time(nullptr)` whenever `is_time_valid()`
is true (omitted otherwise). The LCD has no NTP/RTC clock of its own, so before this it could
not run its absolute-epoch maintenance machinery; with `now_epoch` it `settimeofday()`s its
clock on receipt and computes its maintenance self-wake from the absolute window
(`start_epoch − grace_before − 15s` lead, matching the Sense's `nextWakeEpochForSleep(now,15,5)`)
and stays awake through the window so the LCD-first OTA proxy can reach it. Backward
compatible: when `now_epoch` is absent (old Sense / invalid clock) the LCD falls back to the
relative `wake_in_s`/`remaining_s` offsets. See LCD_FIRMWARE.md ("LCD clock + absolute-window
self-wake") and ARCHITECTURE.md. The `[MAINT_TX]` log line now includes `now_epoch=`.

### Arm-time keep-awake (`MAINT_KEEPALIVE`) — delivery race fix

After a tap the LCD idle-sleeps at ~10s, but the Sense needs ~10–15s for WiFi+NTP+
schedule-fetch before it can send the real `MAINT_WINDOW` (which needs `now_epoch`). The
crucial subtlety: `set_maintenance_schedule_pending_sync_to_lcd(true)` is called **only AFTER
the HTTPS `/ota/schedule` fetch** (`[OTA_HTTP_SCHED] saved`), and that fetch already requires
valid time for TLS — so by the time the pending-sync flag is set, `is_time_valid()` is
**already true** and the LCD has **already idle-slept during the fetch**. A
"pending && !time_valid" trigger therefore never fires on a fresh arm, and the window (and
hence the absolute self-wake) never lands. The Sense cannot wake a sleeping LCD (GPIO39 is
LCD→Sense only).

Fix — keep the LCD awake through the **entire post-wake connect/fetch phase**, before the
pending flag is even set (arm-time only — untouched: the at-window proxy, self-OTA,
`esp_restart`, `run_maintenance_if_needed()`). New function `keep_lcd_awake_during_maint_arm()`
runs every awake loop, **just before** `sync_pending_maintenance_to_lcd("awake")`, and sends a
lightweight **`MAINT_KEEPALIVE`** (`send_maint_keepalive()`) on a `MAINT_KEEPALIVE_RESEND_MS =
2000` cadence while **both**:
- we are still early in the wake — `(millis() − last_wake_ms) < KEEPALIVE_ARM_WINDOW_MS`
  (25s). `last_wake_ms` is reset in `setup()` on every wake (the Sense reboots on deep-sleep
  wake, so `millis()` and this static reset to ~0), giving a reliable per-wake start reference.
- **AND** `( !g_ota_check_done  ||  (pending && !acked) )`. `g_ota_check_done` is `false` from
  wake until `maybeRunOtaCheck()` begins (it sets it `true`), so `!g_ota_check_done` spans the
  WiFi+NTP+schedule-fetch phase; the `pending && !acked` part then covers delivery until the
  LCD acks. "acked" mirrors the `ack_ok` logic (`g_lcd_maint_ack_received` matched against
  `g_maint_pending_request_id`).

It is **NOT** gated on `halo_uart_link_recent()`: that measures LCD→Sense traffic, which goes
stale a few seconds after the tap even while the LCD is still awake, so it would cut keepalives
off too early. Sending a keepalive to an already-asleep LCD is a harmless no-op; the 25s
wake-window + the gate conditions are the bound. The keepalive carries only `request_id`
(empty until the window is loaded); the LCD just resets its activity timer + nudges stay-awake
on receipt. Stops naturally once acked (the `pending && !acked` gate goes false) and the OTA
check has run, or when the 25s wake window elapses. The cadence timer
`g_maint_keepalive_last_tx_ms` is owned solely by this function and is per-wake-fresh (static,
zero at boot) — `sync_pending_maintenance_to_lcd()` no longer touches it. New statics/constants:
`g_maint_keepalive_last_tx_ms`, `MAINT_KEEPALIVE_RESEND_MS`, `KEEPALIVE_ARM_WINDOW_MS`. New
senders: `send_maint_keepalive()`, `keep_lcd_awake_during_maint_arm()`. The
`deferred_time_invalid` branch of `sync_pending_maintenance_to_lcd()` is restored to log-only.

### Window-Start Schedule Re-Validation (Cancel-Safety)

`ota_sched_revalidate(const MaintenanceWindow& cached, uint64_t now_epoch, uint32_t timeout_ms)` returns a `SchedRevalidate` enum (`REVAL_VALID` / `REVAL_CANCELLED` / `REVAL_REPLACED` / `REVAL_FETCH_FAILED`). It performs a dedicated GET of `ota_sched_http_build_url()` (`/ota/schedule`) using the same HTTPS/TLS setup as `ota_sched_http_fetch_window()`, but inspects the response directly:

- **204** -> `REVAL_FETCH_FAILED` (**fail-open**). The schedule GET only returns enabled windows whose `start_epoch` is in the **future**, so a 204 means "no future window" — which includes a live, enabled window whose start has already passed (the normal case, since the device wakes **at** the window) as well as a truly deleted row. Treating 204 as cancelled would falsely abort legitimate scheduled OTAs, so the device proceeds with its cached window.
- **200, `enabled=false`** -> `REVAL_CANCELLED` (disabled). This is the **only** reliable cancel signal: an `enabled=false` row returns HTTP 200 at any wake time regardless of `start_epoch`.
- **200, `enabled=true`, `request_id` differs from cached** -> `REVAL_REPLACED`
- **200, `enabled=true`, `request_id` matches (or empty)** -> `REVAL_VALID`
- **not configured / no WiFi / time invalid / begin fail / empty body / parse error / any other HTTP code / network error** -> `REVAL_FETCH_FAILED` (**fail-open**)

> **Operational rule:** to pull a scheduled release, set `enabled=false` on the device's schedule row — do **not** delete it. A deleted row returns 204, indistinguishable from a live past-start window, and is fail-open (the device will proceed with its cached window).

`run_maintenance_if_needed()` calls it (only when `has_mw`) **after the in-window + idle checks pass and before `send_ota_uart_message("OTA_LOCK")`**, with timeout `max(2000UL, OTA_SCHED_HTTP_TIMEOUT_MS)`. On `REVAL_CANCELLED`/`REVAL_REPLACED` it aborts the OTA: records `ota_set_last_result("schedule_cancelled"/"schedule_replaced")` (reported to cloud at pre-sleep), `sched_event_note(res, request_id)` (request_id captured before clearing), `maintenance_followup_retry_clear(res)`, `maintenance_window_consumed_clear(res)`, then `mw.clear()` to **wipe the cached NVS `mw` window** so the next pre-sleep won't re-arm it, sets `g_maintenance_in_window=false` / `g_maintenance_mode=false`, and enters `sense_enter_sleep(SENSE_SLEEP_DEEP_MAINT)`. `OTA_LOCK` was never sent on this path, so no `OTA_UNLOCK` is needed. `REVAL_VALID` and `REVAL_FETCH_FAILED` both proceed with the cached window (fail-open).

**Outside-window handling (MaintenanceWindow path) — before vs. after.** In `run_maintenance_if_needed()`, the `MaintenanceWindow` outside-window branch (`now_epoch < window_start || now_epoch > window_end`) now distinguishes two cases:
- **Before-window** (`now_epoch < window_start && !mw.hasExpired(now_epoch)`): the device woke too early — the clock was off at arm-time and NTP has just corrected it (`run_maintenance_if_needed()` re-syncs NTP before this check), or a prior fixed-delay followup retry landed early. Instead of scheduling another fixed-delay retry (120/300/600s) that is misaligned with the real window and could miss again — ultimately exhausting the 3-attempt retry machinery and **abandoning the scheduled OTA** — it calls `maintenance_followup_retry_clear("woke_before_window")`. With no followup retry pending, the pre-sleep timer re-arm (`ota_sched_configure_timer_wakeup`) re-targets the actual window start using `maintenance_window_wake_delta_s` (wakes at `start-15`). Because the clock is now NTP-synced, this converges in a single cycle (next wake lands inside the window).
- **After-window** (`now_epoch > window_end`): unchanged — the existing expire/retry logic (`mw.hasExpired` clear + fixed-delay followup retry) is correct.

This fixes the case where an early wake (clock skew corrected by NTP) burned all followup retries and abandoned the scheduled OTA. The separate `ota_sched_in_window` outside-window branch (the `else`/no-`has_mw` schedule path) is independent and was not changed.

### Awake-Path Schedule Fetch (Sustained-Activity Arm-Flake Fix)

The schedule fetch (`ota_sched_http_fetch_window()`) historically ran **only** in `halo_prod_pre_sleep()`. A device kept continuously active (never reaching a clean idle-sleep) therefore never fetched a freshly-posted maintenance window before that window's `start_epoch` passed — and because the `/ota/schedule` GET is **future-only** (it returns 204 once `start_epoch` is in the past), the window could never be captured into NVS afterward. With no NVS window, the sleep-entry timer-arm (`sense_config_deep_sleep_wakeup` → `ota_configure_timer_wakeup`, in `sense_sleep.h`) had nothing to arm, and the device slept through the window.

To fix this, `halo_prod_loop()` now performs a **throttled awake-path schedule fetch**, inserted immediately after `keep_lcd_awake_during_maint_arm()` / `sync_pending_maintenance_to_lcd("awake")` and before the OTA-intent / `maybeRunOtaCheck()` logic. The timer-arm itself was **not** changed — it still runs at sleep entry — so once the awake-path fetch populates the NVS window while the window is still future, the existing sleep-entry arm handles delivery.

**Gates (all must hold to fetch):**
- `awake_long_enough` — current wake has lasted `>= AWAKE_SCHED_MIN_AWAKE_MS` (20s), measured against `last_wake_ms` (so only sustained wakes, not brief tap-and-sleep cycles, pay the HTTPS cost).
- `attempt_throttle_ok` — at least `AWAKE_SCHED_RETRY_MS` (30s) since the last awake-path attempt (per-`millis()` static `s_last_awake_sched_attempt_ms`, set on every attempt).
- `fetch_overdue` — `truth_get_sched_fetch_age_s()` is `< 0` (never fetched) or `>= AWAKE_SCHED_AGE_S` (90s since last successful fetch); keeps the NVS window fresh without re-fetching every loop.
- `ota_sched_http_configured() && wifi_is_connected() && is_time_valid()` — schedule endpoint usable and clock valid (the GET needs valid time).
- `!sense_action_inflight() && !g_lcd_ota_task_running && !g_maintenance_mode` — don't contend with a live capture/upload, an in-progress LCD OTA proxy, or maintenance handling.
- `!halo_provisioning_active()` — **never run schedule-fetch (or the `window_armed` report) TLS during provisioning/setup.** During provisioning the SoftAP is still up (AP+STA fragments internal RAM) AND the 16KB `g_camera_dma_reserve` is held, so the schedule GET's TLS handshake starves `esp_aes_process_dma` → `abort()` → `ESP_RST_PANIC` → the device reboots back to `ap_setup` **before** it marks itself provisioned or claims the owner (app shows "something went wrong", QR stays up). This is the same esp-aes DMA hazard commit 809 fixed for owner-claim TLS; the awake-sched path was previously unguarded and shouldn't run during setup at all. Reproduced on fw 6.1.814. Once provisioned and out of setup mode, the fetch runs normally.

On fire it stamps the throttle, computes `to_ms = min(OTA_SCHED_HTTP_TIMEOUT_MS, 5000)` (a tighter awake-path budget than the pre-sleep path), logs `[AWAKE_SCHED] fetch awake_ms=… fetch_age_s=…`, calls `ota_sched_http_fetch_window(to_ms)` (same call style as pre-sleep — no caller-side `http_inflight` pre-set), and if `maintenance_schedule_pending_sync_to_lcd()` then went true, immediately pushes the new window to the LCD via `sync_pending_maintenance_to_lcd("awake_sched_fetch")`.

**Constants/statics (all function-local to `halo_prod_loop()`):** `s_last_awake_sched_attempt_ms`, `AWAKE_SCHED_MIN_AWAKE_MS = 20000`, `AWAKE_SCHED_RETRY_MS = 30000`, `AWAKE_SCHED_AGE_S = 90`.

**`window_armed` cloud report (telemetry):** right after this awake-path fetch (and its LCD sync), the loop reloads the NVS window and, if it now holds a `request_id` not yet reported, POSTs a `window_armed` report via `ota_report_post("window_armed", nullptr, ...)`. This records in the cloud that the device picked up a maintenance window — the standard payload already carries `request_id`, `maint_start_epoch`, `fw`, and `lcd_fw`. De-dupe is a function-local `static char s_last_armed_report_request_id[64]` updated only on a successful POST, so repeated awake fetches of the same window don't re-report (a new/replaced `request_id` re-arms it). WiFi is already up on this path.

**`ScopedTlsDmaReserve` — shared TLS DMA guard (defense in depth):** a file-scope RAII struct in `halo_sense_prod.ino` (just after the `halo_tls_free_dma_reserve()` / `halo_tls_restore_dma_reserve()` `extern "C"` hooks) frees the 16KB camera DMA reserve in its ctor and restores it in its dtor (on **every** scope exit, early returns included). It takes a `bool active` flag so a call site arms it **only on its HTTPS branch** (the plain-HTTP branch doesn't free/restore). It mirrors commit 809's `DmaReserveTlsGuard` (used in `ProvisioningManager.cpp` for owner-claim TLS, a separate call path — no nesting/conflict). It is instantiated in two outbound-TLS functions so their handshakes can't panic when the camera reserve is held (e.g. during a maintenance window, or — as a backstop to the provisioning gate above — if a schedule fetch ever ran with the SoftAP up):
- **`ota_sched_http_fetch_window(timeout_ms)`** — guard armed (on `sched_is_https`) right before the `HTTPClient http;` declaration, covering the whole `http.GET()` / `http.end()` exchange and all early returns.
- **`ota_report_post(report_type, extras, timeout_ms)`** — guard armed (on `report_is_https`) right before the `HTTPClient http;` declaration, covering the `http.POST()` / `http.end()` exchange and all early returns. This protects the `ota_complete` report (which fires during a maintenance window with the camera reserve held) and the `window_armed` report.

The free/restore hooks are idempotent (free checks non-null, restore checks null), so the guard is safe even if the reserve is already released.

---

## Cross-Module Dependencies

```
Sense_Minimal.ino (main)
  |-- sense_time.h          (time cache, no deps)
  |-- sense_diag.h          (diagnostics, depends on time)
  |-- sense_uart.h          (UART TX/RX, ring buffer, COBS flag)
  |-- sense_uart_msg.h      (message formatters, depends on uart)
  |-- sense_ota_lcd.h       (LCD OTA proxy, depends on uart)
  |-- sense_http.h          (TLS config, DNS, deadlines)
  |-- sense_wifi.h          (WiFi state machine, depends on http)
  |-- sense_upload.h        (HTTP retry core, depends on wifi+http)
  |-- sense_mqtt.h          (MQTT, depends on wifi, dev only)
  |-- sense_voice.h         (voice capture+upload, depends on wifi+upload)
  |-- sense_list.h          (shopping list API, depends on http)
  |-- sense_camera.h        (camera hardware, depends on diag+uart)
  |-- sense_presign.h       (presign URLs, depends on upload+camera)
  |-- sense_scan.h          (scan helpers, depends on presign+camera)
  |-- sense_op_queue.h      (op queue, depends on scan)
  |-- sense_upload_exec.h   (upload exec, depends on everything above)
  |-- sense_upload_queue.h  (upload queue mgmt)
  |-- sense_upload_persist.h (SPIFFS persistence)
  |-- sense_sleep.h         (sleep, depends on camera+uart+ota)
```

## Key Design Patterns

### DMA Guard (RAII)
Camera DMA reservation (16 KB) is released before TLS operations and re-acquired on function exit via C++ destructor. This prevents `esp-aes` allocation failures during TLS handshake.

### Foreground Preemption
User actions (scan, voice) preempt background uploads at every stage:
- `foreground_active` flag checked during PUT write loop
- `dish_upload_pending()` checked before presign and PUT
- Upload worker parks jobs and resumes when foreground clears

### Coordinated Sleep
Neither board sleeps independently. The LCD initiates sleep via `INPUT_SLEEP`. Sense evaluates readiness, drains uploads, then sends `SLEEP_READY`. If blocked, sends `SLEEP_DENY` with retry delay. Wake pin stuck detection disables EXT0 and falls back to timer wake.

### MQTT Disabled in Production
MQTT (PubSubClient) is compiled out in production builds (`HALO_SENSE_PROD_WRAPPER`). Dish results use HTTP polling instead (`wait_for_dish_result_http()`). This saves ~30 KB internal SRAM and eliminates TLS conflicts.

### Upload Persistence
Failed uploads are saved to SPIFFS (one job at a time) and retried on next boot. Voice replays have 60s backoff. Stale uploads (> 24h) are dropped. Max 5 retries.

# HALO Sense Board Firmware Documentation

## Overview

The Sense board is an **XIAO ESP32-S3** with PSRAM, OV2640 camera, I2S microphone, and UART connection to the LCD board. It is the network-connected brain of the HALO device, responsible for:

- **Camera capture** with adaptive scene analysis (preflight brightness/color detection)
- **S3 image uploads** via presigned URLs with retry logic
- **Voice recording** and upload to an async quick-ack API
- **WiFi management** with guard state machine, background maintenance, and hard-reset recovery
- **Sleep coordination** with the LCD board via a handshake protocol
- **OTA self-update** (via production wrapper) and LCD OTA proxy over UART
- **MQTT** dish result subscription (dev builds; disabled in production)
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
| `GUARDIAN_FORCE_SLEEP_MS` | 300000 | 5-min max awake time (guardian force sleep) |
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
7. Create FreeRTOS queues: `op_queue` (20 slots), `upload_queue` (10), `upload_queue_dish` (10)
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

#### `loop()`

Main loop responsibilities:
1. **UART RX processing**: Read `lcdSerial`, push to ring buffer, extract frames, call `parse_input_message()`. Skipped when LCD OTA proxy owns UART.
2. **USB serial injection**: Accepts JSON commands via USB CDC for debugging.
3. **WiFi maintenance**: `service_wifi_maintenance()`, guard poll, RSSI reporting (every 2s).
4. **Upload persistence replay**: Attempt to re-queue failed uploads from SPIFFS.
5. **Guardian force sleep**: If awake > 5 minutes, force deep sleep.
6. **Work request processing**: Handle `reset_wifi_requested`, `refresh_requested`, `delete_requested`.
7. **MQTT loop** (dev builds only).
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
| `INPUT_PING` | Reply with PONG, update heartbeat timestamp |
| `INPUT_RESET_WIFI` | Flag WiFi credential reset |
| `INPUT_FW_INFO` | Trigger fresh LCD query, then reply with `FW_INFO` carrying BOTH board versions + LCD partition/state |
| `INPUT_OTA_CHECK` | Trigger manual OTA check |
| `INPUT_WIFI_SCAN` | Scan and report visible APs |
| `INPUT_WIFI_TEST` | Full WiFi cold-start diagnostic |
| `SYNC` / `SYNC_ACK` | Link synchronization |
| `LINK_HB` | LCD heartbeat received |
| `LCD_DIAG` | Store LCD diagnostic state |
| `WIFI_STATUS` | Sync WiFi credentials with LCD |
| `WIFI_CREDS_ACK` / `WIFI_ON_ACK` | Provisioning credential flow |
| `MAINT_WINDOW_ACK` | Maintenance window scheduling |
| `SLEEP_DENY` | LCD denied Sense sleep intent |
| `OTA_CHECK_ACK` / `OTA_CHECK_RESULT` | OTA status from LCD |
| `LCD_OTA_QUERY_RESP` | LCD running fw version + partition/state (OTA proxy mailbox). Extended fields: `running_part`, `running_state`, `boot_part` |
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
4. If clear, drains upload queue (30s window), disconnects WiFi/MQTT, deinits camera
5. Configures EXT0 + timer wake, sends `SLEEP_READY` + WiFi diag summary
6. Calls `esp_deep_sleep_start()` (no return)

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
1. Check DMA availability; kill WiFi if DMA < 24 KB or HTTP inflight
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

#### DMA Guard Pattern

Before WiFi/TLS operations, the 16 KB DMA reservation is freed to give TLS enough contiguous internal SRAM. An RAII `DmaGuard` struct re-acquires it on function exit. This pattern appears in:
- `http_post_json_with_retries()`
- `http_get_with_retries()`
- `put_to_presigned_url()`
- `voice_upload_and_parse()`

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
8. Disconnect MQTT and WiFi; stop BT
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

**Purpose:** Presign URL request, S3 PUT upload with chunked write, dish result HTTP polling, and UI meal result message.

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
- Emits `UI_MEAL_RESULT` UART message to LCD with calories, macros, health score
- Respects `DISH_RESULT_TIMEOUT_MS` (60s) and job deadline

**`uart_send_ui_meal_result()`** -- Sends structured nutrition data to LCD:
- JSON message type `UI_MEAL_RESULT`
- Fields: calories, protein_g, carbs_g, fat_g, confidence, meal_summary, recommendation, mode, job_id

---

### 7. sense_upload_queue.h -- Upload Queue Management

**Purpose:** FreeRTOS queue wrappers for image and voice upload jobs.

#### Key Functions

**`upload_queue_count()`** -- Total items across both queues (normal + dish).

**`upload_queue_is_full()`** -- True if total >= `UPLOAD_QUEUE_MAX` (10).

**`queue_upload_job()`** -- Enqueue image upload:
- Dish jobs go to `upload_queue_dish` via `xQueueSendToFront` (priority)
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

### 10. sense_mqtt.h -- MQTT (Dev Builds Only)

**Purpose:** AWS IoT MQTT connection for receiving dish analysis results. **Disabled in production** (`HALO_SENSE_PROD_WRAPPER` uses HTTP polling instead).

In production builds, all MQTT variables are stubbed:
- `mqtt_clear_result_subscription()` and `mqtt_subscribe_result_topic_if_needed()` are no-ops
- `waiting_for_mqtt_result` is still used as a generic "waiting for dish result" flag

#### Key Functions (Dev Builds)

**`connect_to_mqtt()`** -- Connects to AWS IoT with mTLS (root CA + device cert + private key).

**`on_mqtt_message()`** -- Callback for dish result messages:
- Matches job_id against `current_scan_job_id`
- Handles fast (intermediate) and final results
- Sends `UI_MEAL_RESULT` to LCD
- Clears wait state on receipt

**`mqtt_ensure_connected()`** -- Reconnect with retry (250ms backoff, 4 attempts before reset).

---

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
4. Background: presign + PUT upload (no nutrition wait in foreground)

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

**`dish_upload_pending()`** -- True if any dish work is in progress (active job, dish queue, dish scan inflight, or waiting for result).

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
- Extracts product_name and id
- Thread-safe via `g_list_mutex`
- Sends `UI_LIST` to LCD after update

**`delete_item_from_api(const char* item_id)`** -- Removes item via POST with operation="remove".

---

### 16. sense_time.h -- NTP/RTC Time Cache

**Purpose:** Persist last-known epoch to RTC memory and NVS so TLS can bootstrap time after deep sleep without waiting for NTP.

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
| `LCD_OTA_PROXY_QUERY_TIMEOUT_MS` | 5000 | Wait for LCD version query |
| `LCD_OTA_PROXY_BEGIN_TIMEOUT_MS` | 10000 | Wait for OTA begin ACK |
| `LCD_OTA_PROXY_END_TIMEOUT_MS` | 60000 | Wait for SHA verify + reboot |
| `LCD_OTA_PROXY_DOWNLOAD_TIMEOUT_MS` | 2400000 | 40 min overall download limit |

#### Mailbox Pattern

The proxy task cannot read `lcdSerial` directly (main loop owns it). Instead, `parse_input_message()` in the main loop dispatches LCD OTA responses into mailbox variables:
- `g_lcd_ota_query_resp_ready/fw/part_size` -- LCD_OTA_QUERY_RESP
- `g_lcd_query_running_part[16]` / `g_lcd_query_running_state[20]` / `g_lcd_query_boot_part[16]` -- extended LCD partition/state observability fields captured from LCD_OTA_QUERY_RESP. Exposed via `sense_lcd_last_running_part()`, `sense_lcd_last_running_state()`, `sense_lcd_last_boot_part()` getters.
- `g_lcd_ota_begin_ack_ready/accepted/reason/resume_offset` -- LCD_OTA_BEGIN_ACK
- `g_lcd_ota_end_ack_ready/sha_match` -- LCD_OTA_END_ACK

During binary streaming, `g_lcd_ota_proxy_owns_uart = true` prevents the main loop from reading `lcdSerial`.

#### OTA Proxy Flow

**`sense_lcd_ota_proxy(const OtaManifest& manifest, const char* lcd_fw_version)`**:

1. **Version check**: Compare manifest version against LCD current version. Skip if up-to-date.
2. **Begin session**: Send `LCD_OTA_BEGIN` with session_id, image_size, sha256, version. Wait for `LCD_OTA_BEGIN_ACK` (supports resume via `resume_offset`).
3. **Download + stream**: HTTP GET firmware binary from S3. For each 512-byte chunk:
   - Send COBS frame (`MSG_CHUNK`) with sequence number
   - Wait for `MSG_ACK` with matching sequence
   - Retry up to 5 times on timeout/NACK
   - Progress logged every 10%
   - Stream timeout reduced to 50ms for throughput
   - **Stall-tolerant (reconnect/resume):** the HTTP GET-and-acquire-stream step is factored into an `open_stream(offset)` lambda used by both the initial fetch and recovery. If the stream goes `>30s` with no data (`LCD_OTA_PROXY_STALL_WINDOW_MS`) or disconnects before completion, the proxy does NOT abort. It closes the socket and re-opens the GET with a `Range: bytes=<bytes_sent>-` header (200/206 expected), re-acquires the stream, and continues from `bytes_sent`. Up to `LCD_OTA_PROXY_MAX_RECONNECTS` (5) **consecutive** stalls are tolerated; the counter resets on any data progress. Only after the budget is exhausted does it abort with `"timeout"`. The overall `LCD_OTA_PROXY_DOWNLOAD_TIMEOUT_MS` (40 min) guard still applies. Logs: `stall -> reconnect attempt N/5 at offset <x>`, `reconnect ok code=<c>`, `reconnect failed code=<c>`. This makes the download survive transient WiFi hiccups / brief net contention instead of failing the whole OTA.
4. **End session**: Release UART ownership. Send `LCD_OTA_END` with image_size and sha256. Wait for `LCD_OTA_END_ACK` with SHA verification result.
5. **Result**: Returns "success", "up_to_date", "sha_mismatch", "chunk_retry_exhausted", "timeout", etc.

**Non-OTA network suppression:** while any OTA activity is in flight, `request_list_refresh()` is short-circuited (`[LIST_REFRESH] deferred (lcd_ota_in_progress)`) and the `INPUT_WAKE` handler skips its list-refresh trigger. This prevents an HTTP GET to `/v1/list` from competing with the S3 download on the single net stack (which previously starved the download to a stall/abort). The gate is `lcd_ota_in_progress()`, defined in `halo_sense_prod.ino` and forward-declared in `Sense_Minimal.ino` (weak `return false` fallback for non-wrapper builds). It returns true if any of `g_lcd_ota_proxy_owns_uart`, `g_lcd_ota_request_active`, `g_lcd_ota_task_running`, `g_ota_apply_in_progress`, or `g_ota_check_in_progress` is set. OTA's own HTTP (manifest fetch, S3 download, schedule/report) is NOT gated.

**`sense_lcd_ota_query()`** -- Query LCD firmware version and OTA partition size. The dispatch of `LCD_OTA_QUERY_RESP` also captures extended observability fields (`running_part`, `running_state`, `boot_part`) into static buffers exposed by `sense_lcd_last_running_part/state/boot_part()`.

**`uart_send_fw_info()`** -- Triggers a FRESH `sense_lcd_ota_query()` round-trip, then emits a `FW_INFO` JSON reporting the REAL running firmware of BOTH boards plus LCD partition/state. Fields:
- `sense_fw` -- live running Sense version (`kFirmwareVersion`)
- `lcd_fw` -- freshly queried LCD running version (NOT the cached OTA manifest value); `"unknown"` if the query fails/times out
- `lcd_running_part` -- LCD running partition label (omitted on query failure)
- `lcd_running_state` -- `NEW|PENDING_VERIFY|VALID|INVALID|ABORTED|UNDEFINED|UNKNOWN` (omitted on query failure)
- `lcd_boot_part` -- LCD boot partition label (omitted on query failure)
- `lcd_fw_age_s` -- seconds since the query completed (omitted on query failure)

Observability only; does not affect OTA control flow. A failed/timed-out query does not block: `sense_fw` is always emitted.

**`sense_lcd_ota_fetch_manifest()`** -- Fetch and parse LCD manifest JSON from S3.

**Cloud-reported `lcd_fw` is the REAL running version (not the manifest):** `truth_get_lcd_fw_version()` returns `g_lcd_ota_version`, which is sourced ONLY from an actual `LCD_OTA_QUERY_RESP` (via `sense_lcd_ota_query()`), never from the OTA manifest. On a successful proxy, the orchestrator clears the cached value (`g_lcd_ota_version[0]='\0'`, `g_lcd_fw_query_ms=0`) instead of writing the assumed manifest version — this previously masked failures by always reporting the target version. The pre-sleep path re-queries the LCD and overwrites the cache with the real booted version when the cache is empty OR stale (`>LCD_FW_QUERY_STALE_MS` = 5 min), provided the UART link is recent and no proxy task is running; `g_lcd_fw_query_ms` (millis) timestamps the last real query.

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

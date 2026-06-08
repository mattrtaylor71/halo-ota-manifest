# HALO Firmware Architecture

Last updated: 2026-05-12

---

## 1. System Overview

HALO is a kitchen-mounted food intelligence device that captures images of food (groceries, meals, pantry items), uploads them to AWS for AI processing, and displays results on a round LCD screen. It also supports voice commands via a built-in microphone.

The device uses a **two-board ESP32-S3 architecture**:

- **Sense board** (XIAO ESP32S3 Sense) -- Camera, WiFi, microphone, all network operations
- **LCD board** (custom ESP32-S3 with round display) -- 1.28" round LCD, touch, rotary encoder, all UI rendering

The boards communicate over a dedicated **UART link at 115200 baud**. The LCD board is the **sleep leader** -- it decides when the system sleeps based on user inactivity. The Sense board is the **network leader** -- it owns all WiFi, HTTPS, MQTT, and OTA downloads.

### Physical Architecture

```
                    UART (115200 baud)
  [Sense Board] <========================> [LCD Board]
   - OV2640 Camera                          - 1.28" Round LCD (SH8601)
   - WiFi (WPA2)                            - CST816 Touch Controller
   - I2S Microphone                         - Rotary Encoder (EC1)
   - Flash LED (GPIO4)                      - INT_PIN (GPIO39) -> Sense wake
   - GPIO2 (wake input from LCD)            - GPIO9 (touch wake from sleep)
   - GPIO1 (camera PWDN)
```

### Repository Layout

```
HALOMAIN_rev1p5_modular/
  Sense_Minimal/           # Sense board firmware (.ino + 23 headers)
  LCD_Minimal/             # LCD board firmware (.ino + 30 headers)
  halo_common/             # Shared board config (BoardConfig.h)
  halo_ota_demo/
    firmware/
      halo_sense_prod/     # Production Sense wrapper (OTA + MQTT + provisioning)
      halo_lcd_prod/       # Production LCD wrapper (OTA receiver)
      shared/              # Shared OTA/MQTT/provisioning libraries (~30 files)
    publish_both.sh        # OTA build + publish pipeline
    tools/                 # OTA publishing scripts
  halomain_assets/         # UI image assets
  tap_implementation/      # Physical actuator for automated testing
  tools/                   # Test infrastructure (dashboards, capture tools)
  docs/                    # This document
```

---

## 2. Hardware

### ESP32-S3 Variants

| Property | Sense Board | LCD Board |
|---|---|---|
| Module | XIAO ESP32S3 Sense | Custom ESP32-S3 |
| Flash | 8 MB | 8 MB |
| PSRAM | 8 MB OPI | 8 MB OPI |
| USB | USB CDC (HWCDC) | USB CDC (HWCDC) |
| USB Port (Mac) | /dev/cu.usbmodem**1101** | /dev/cu.usbmodem**101** |

**Critical:** Both boards have 8MB flash. The LCD must be compiled with `FlashSize=8M`, not 16M.

### Pin Assignments

#### Sense Board (XIAO ESP32S3)

| Pin | Function | Notes |
|---|---|---|
| GPIO2 | Wake input (EXT0) | Driven LOW by LCD INT_PIN to wake Sense |
| GPIO43 (D6) | UART TX to LCD | `HaloPins::kSenseUartTxPin` |
| GPIO44 (D7) | UART RX from LCD | `HaloPins::kSenseUartRxPin` |
| GPIO1 | Camera PWDN | Active low; held during sleep to save power |
| GPIO4 (D3) | Flash LED | Fill light for captures |
| GPIO41 | Shared: UART TX (disabled during voice) + I2S microphone | Must float + attenuate for voice recording |
| GPIO10 | Camera XCLK | 10 MHz clock to OV2640 |
| GPIO40/39 | Camera I2C (SDA/SCL) | SCCB interface |
| GPIO48,11,12,14,16,18,17,15 | Camera data Y2-Y9 | Parallel 8-bit DVP |
| GPIO38 | Camera VSYNC | |
| GPIO47 | Camera HREF | |
| GPIO13 | Camera PCLK | |

#### LCD Board

| Pin | Function | Notes |
|---|---|---|
| GPIO9 | Touch INT (wake from deep sleep) | EXT1 wake source, active LOW |
| GPIO39 | INT_PIN (wake Sense) | Output: pulsed LOW to wake Sense via EXT0 |
| GPIO38 | UART TX to Sense | `HaloPins::kLcdUartTxPin` |
| GPIO48 | UART RX from Sense | `HaloPins::kLcdUartRxPin` |
| GPIO8 | Encoder A (EC1_A) | Rotary encoder input |
| GPIO7 | Encoder B (EC1_B) | Rotary encoder input |

**Encoder wake mask caveat:** Encoder pins (GPIO7/8) must NOT be included in the EXT1 deep sleep wake mask. If `enc_b` rests LOW, EXT1 ANY_LOW triggers an instant wake loop. Only `GPIO9` (touch INT) is in the wake mask.

---

## 3. Communication Protocol

### UART JSON Protocol

All normal inter-board communication uses **newline-delimited JSON** at 115200 baud over UART1. Every message has four required fields:

```json
{
  "ver": 1,
  "type": "MESSAGE_TYPE",
  "msg_id": 42,
  "ts": 12345
}
```

- `ver` -- Protocol version (always 1)
- `type` -- Message type string
- `msg_id` -- Monotonically increasing per-board counter
- `ts` -- Sender's `millis()` timestamp

Maximum line length: 4096 bytes (allows rich voice response payloads).

### Message Types (LCD -> Sense)

| Type | Purpose |
|---|---|
| `INPUT_WAKE` | LCD woke up, requesting Sense wake/sync |
| `INPUT_SLEEP` | LCD wants to sleep; initiates sleep handshake |
| `INPUT_SCAN` | User selected a menu action (dish, check-in, discard, etc.) |
| `INPUT_DELETE` | Delete item from shopping list |
| `INPUT_SCROLL` | User scrolled the list (encoder) |
| `INPUT_LONG_PRESS` | Long press detected (voice recording trigger) |
| `INPUT_LONG_PRESS_END` | Long press released (stop voice recording) |
| `INPUT_EXPIRY` | Expiry date response for check-in flow |
| `INPUT_DISCARD_CHOICE` | User chose discard options (add to shopping list?) |
| `LINK_HB` | Heartbeat (sent every 4s, confirms LCD is alive) |
| `SYNC` | Request link synchronization |
| `PONG` | Response to PING |
| `LCD_OTA_QUERY_RESP` | LCD running fw + partition size + real partition/state: `running_part`, `running_state` (`NEW`/`PENDING_VERIFY`/`VALID`/`INVALID`/`ABORTED`/`UNDEFINED`/`UNKNOWN`), `boot_part`, optional `last_ota_result` |
| `LCD_OTA_BEGIN_ACK` | Acknowledge OTA begin (accepted/rejected, resume offset) |
| `LCD_OTA_END_ACK` | Acknowledge OTA end: `sha_match` (SHA256 verify) + `ota_ok` (`esp_ota_set_boot_partition()` result). Sense proxy requires **both** true for success |
| `RELEASE_WAKE_ACK` | Acknowledged wake pin release |
| `MAINT_WINDOW` | Maintenance window acknowledgment with timing details |

### Message Types (Sense -> LCD)

| Type | Purpose |
|---|---|
| `UI_STATUS` | Status update with op/phase/text/screen_hint for UI routing |
| `UI_LIST` | Shopping list data (items array) |
| `UI_MEAL_RESULT` | Dish analysis result (kcal, macros, health score) |
| `UI_VOICE_RESPONSE` | Voice assistant response (text, items, transcript) |
| `UI_TOAST` | Ephemeral notification overlay |
| `SYNC_ACK` | Link sync confirmed |
| `SLEEP_READY` | Sense is ready for deep sleep |
| `SLEEP_DENY` | Sense cannot sleep yet (reason + retry_ms) |
| `LINK_HB` | Heartbeat (sent every 4s) |
| `PING` | Keepalive probe |
| `RELEASE_WAKE` | Request LCD release the wake GPIO line |
| `SENSE_DIAG` | Diagnostic data forwarded to LCD for display/storage |
| `LCD_OTA_QUERY` | Query LCD for current firmware version |
| `LCD_OTA_BEGIN` | Start OTA transfer (version, size, SHA256) |
| `LCD_OTA_ABORT` | Abort OTA transfer |
| `WIFI_CREDS` | Send WiFi credentials to LCD for NVS storage |
| `WIFI_ON` | Request LCD enable WiFi (legacy, now no-op) |
| `MAINT_WINDOW` | Maintenance window schedule from cloud API. Fields: `remaining_s`, `wake_in_s`, `start_epoch`, `duration_sec`, `grace_before_sec`, `grace_after_sec`, `request_id`, `clear`, and `now_epoch` (Sense wall clock, present only when the Sense clock is valid — LCD uses it to set its own clock) |
| `PROVISION_QR` | QR code data for provisioning display |
| `FW_INFO` | Real running firmware of BOTH boards + LCD partition/state: `sense_fw`, `lcd_fw` (freshly queried, not cached manifest), `lcd_running_part`, `lcd_running_state`, `lcd_boot_part`, `lcd_fw_age_s`. `lcd_fw="unknown"` if the fresh LCD query fails |

### COBS Binary Protocol (OTA Only)

During LCD OTA updates, the UART switches from JSON to **COBS-framed binary** for firmware chunk transfer. This uses `UartOtaProtocol` (shared between both boards).

Frame structure (before COBS encoding):

```
[type:1] [seq:2 LE] [len:2 LE] [data:0-512] [crc16:2 LE]
```

- COBS encoding wraps the frame with 0x00 delimiters
- CRC16-CCITT (polynomial 0x1021, initial 0xFFFF)
- Max chunk size: 512 bytes
- Frame timeout: 5000ms
- Max retries: 5

Message types:
- `MSG_BEGIN (1)` -- Start OTA session (contains version, size, SHA256)
- `MSG_CHUNK (2)` -- Firmware data chunk
- `MSG_END (3)` -- Final frame (triggers SHA256 verify + boot partition swap)
- `MSG_ACK (4)` -- Acknowledge receipt
- `MSG_NACK (5)` -- Negative acknowledge (with error code)
- `MSG_ERROR (6)` -- Error notification

---

## 4. Boot and Wake Flow

### Sense Board Boot Sequence

1. **Reset reason check** -- Classify: deep sleep, brownout, WDT, panic, power-on
2. **RTC crash diagnostics** -- Record crash info from RTC_DATA_ATTR variables
3. **`halo_prod_pre_setup()`** -- Production wrapper early init (boot count, reboot loop guard)
   - **Reboot-loop guard counts crash resets ONLY.** The loop detector is fed
     (`BootState::recordBootTimestamp()`) only when `esp_reset_reason()` is a genuine
     crash: `ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT`, `ESP_RST_WDT`,
     `ESP_RST_BROWNOUT`. Any clean boot -- deep-sleep wake, power-on, or SW restart
     (e.g. post-OTA) -- instead **clears** the reboot history
     (`BootState::clearRebootHistory()`). This fixes scheduled OTA being permanently
     disabled: a scheduled OTA wakes the device from deep sleep (timer wake + in-window
     retries), and the detector used `boot_count` as a pseudo-timestamp, so a few normal
     wakes used to trip the guard and latch `ota_en=0` (`why=reboot_loop_guard`). Manual
     OTA bypassed the guard via `halo_ota_manual_override_active`, which is why manual
     worked but scheduled did not. A real crash-loop (3 crashes with no clean boot
     between) still trips the guard.
4. **UART init** (`initUarts`) -- Configure UART1 to LCD
5. **Camera PWDN init** -- Configure GPIO1 for camera power control
6. **Wake cause dispatch:**
   - `EXT0` (GPIO2 LOW from LCD): Re-init UART, send `UI_STATUS:IDLE`, update communication timer
   - `TIMER`: Init UART, check if LCD is active, call `ota_on_timer_wake()`
   - `UNDEFINED` (cold boot): Full initialization
7. **GPIO2 configure** -- Set as input with pullup for wake pulse polling
8. **FreeRTOS queues** -- Create op_queue (20 slots), upload_queue (10), upload_queue_dish (10)
9. **Voice buffer** -- Allocate 512KB in PSRAM for audio recording
10. **I2S audio init** -- Configure microphone with callback
11. **Worker tasks:**
    - `op_worker` -- Core 1, priority 2, 16KB stack (foreground operations)
    - `upload_worker` -- Core 1, priority 1, 12KB stack (background uploads)
12. **WiFi connect** -- Start connection using provisioned or default credentials
13. **`halo_prod_setup()`** -- MQTT connect, OTA schedule check, provisioning state machine

### LCD Board Boot Sequence

1. **Serial init** at 115200
2. **Wake cause classification** -- EXT0 (touch), EXT1 (touch mask), TIMER, cold boot
3. **Maintenance state restore** -- Check NVS for persisted maintenance window
4. **Timer override detection** -- Panic recovery or RTC-armed maintenance
5. **Wake pin init** -- Configure GPIO9 (touch INT) as input with pullup
6. **INT_PIN init** -- Configure GPIO39 as output, pulse HIGH, then input pullup
7. **Wake Sense** -- Send wake pulse to Sense board via INT_PIN
8. **Test mode restore** -- Check NVS for automated test mode flag
9. **FreeRTOS mutexes** -- app_state_mutex
10. **Touch + encoder init** -- CST816 I2C touch, rotary encoder
11. **LVGL + display init** -- SH8601 display driver, DMA framebuffer
12. **UI stack init** -- Create LVGL screens, load list from NVS
13. **UART task** -- Core 0, priority 3 (dedicated UART polling)
14. **UI task** -- Core 1 (event-driven screen updates)
15. **Prod wrapper setup** -- `halo_lcd_prod_setup()` (OTA fail state, timezone)

### Wake Causes

| Board | Source | GPIO/Method | Purpose |
|---|---|---|---|
| Sense | EXT0 | GPIO2 LOW | LCD pulsed INT_PIN to wake Sense |
| Sense | Timer | RTC timer | OTA maintenance window check |
| LCD | EXT1 | GPIO9 LOW (touch) | User touched the screen |
| LCD | Timer | RTC timer | OTA schedule window (2:00 AM default) |

---

## 5. Sleep Coordination

The sleep flow is a coordinated handshake between LCD (leader) and Sense (follower).

### Full Sleep Sequence

```
LCD (idle timeout)            Sense
    |                           |
    |-- INPUT_SLEEP ----------->|  LCD wants to sleep
    |                           |  Sense checks: operations inflight?
    |                           |  WiFi busy? OTA pending? Provisioning?
    |                           |
    |<--- SLEEP_READY ---------|  (if clear) Sense agrees to sleep
    |    or                     |
    |<--- SLEEP_DENY ----------|  (if busy) reason + retry_ms
    |                           |
    [If SLEEP_READY:]           |
    |  Disable backlight        |
    |  Reset UI state           |
    |  Configure EXT1 wake mask |  Configure EXT0 on GPIO2
    |  esp_deep_sleep_start()   |  esp_deep_sleep_start()
```

### Sleep Deny Reasons

- `cooldown` -- Sense just woke up (MIN_AWAKE_BEFORE_SLEEP_MS = 2000ms)
- `op_inflight` -- Scan/upload/voice operation in progress
- `ota_pending` -- OTA check or apply in progress
- `mqtt_pending` -- MQTT connection or message inflight
- `provisioning_active` -- Device provisioning in progress
- `time_invalid` -- RTC time not yet synced (needed for TLS)

### Sleep Guards

- **Guardian timer** (Sense): Force sleep after 5 minutes awake regardless of state
- **LCD inactivity timeout**: 10 seconds of no user interaction triggers sleep intent
- **Sleep deny max count** (LCD): After N consecutive denials, force sleep
- **Test mode**: Disables sleep entirely (for automated testing)
- **Diagnostic mode**: Keeps device awake for serial monitoring

### Stale Flag Handling

RTC_DATA_ATTR variables persist across deep sleep. On unclean shutdown (brownout, WDT, panic), stale flags can cause issues:
- `g_rtc_clean_shutdown` -- Set to 1 before clean sleep; checked on boot
- `g_rtc_crash_count` -- Incremented on non-clean boot
- `g_timer_wake_armed` -- Cleared immediately after boot to prevent stale timer wake classification

---

## 6. Capture Pipeline

The capture pipeline handles food image capture for dish logging, check-in, check-out, and discard operations.

### Sequence

```
1. Menu Select (LCD)
   User taps "Log Dish" / "Check In" / etc.
   LCD sends INPUT_SCAN {mode: "dish"/"check-in"/"discard"}

2. Camera Init (Sense)
   - Power on camera (GPIO1 PWDN LOW, 15ms settle)
   - esp_camera_init() with SXGA (1280x1024) config
   - 3 warmup frames discarded (100ms delay each)
   - Profile: LABEL (low compression, Q=8, 10MHz XCLK)

3. DMA Guard (Sense)
   - Reserve 16KB PSRAM block (g_camera_dma_reserve)
   - Check largest free block >= 24KB
   - If insufficient: abort with error

4. Capture (Sense)
   - Send UI_STATUS phase=CAPTURING, screen_hint=SHIP_HOLD_STILL
   - esp_camera_fb_get() -- get JPEG frame
   - Quality validation (size > minimum, not corrupt)
   - Copy to PSRAM upload buffer
   - esp_camera_fb_return()
   - Camera deinit + power off

5. Presign (Sense)
   - POST to /presign endpoint with mode, owner_id, device_id
   - Receive: presigned S3 PUT URL, job_id, content_type, result_url

6. S3 PUT Upload (Sense)
   - Release DMA reservation (16KB freed for TLS)
   - PUT JPEG to presigned S3 URL
   - Track timing (presign_start -> put_end)
   - On PUT abort for new dish: re-queue as background upload

7. Result Wait (Sense)
   - Send UI_STATUS phase=RESULT_WAITING
   - Poll result_url or wait for MQTT notification
   - Timeout: 60s

8. Display Result (LCD)
   - Receive UI_MEAL_RESULT with kcal, macros, health score
   - Show result screen with nutritional breakdown
```

### DMA Guard Pattern

Camera DMA and TLS both need large contiguous PSRAM blocks. The guard pattern prevents conflicts:

```cpp
// Before capture: reserve DMA memory
g_camera_dma_reserve = (uint8_t*)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);

// After capture, before upload: release reservation
free(g_camera_dma_reserve);
g_camera_dma_reserve = nullptr;
// Now TLS can allocate for HTTPS PUT
```

### Camera Profiles

| Profile | Resolution | Quality | XCLK | Use Case |
|---|---|---|---|---|
| NORMAL | SXGA 1280x1024 | Q=10 | 10MHz | General |
| LOW_LIGHT | SXGA 1280x1024 | Q=8 | 10MHz | Dark environments |
| FLASH | SXGA 1280x1024 | Q=8 | 10MHz | With fill light |
| LABEL | SXGA 1280x1024 | Q=8 | 10MHz | Text-optimized (default) |

---

## 7. Upload Queue

The Sense board manages two priority-separated upload queues:

### Queue Architecture

```
upload_queue_dish  (10 slots, high priority)  -- Dish/meal uploads
upload_queue       (10 slots, lower priority) -- Check-in, discard, voice uploads
```

The `upload_worker` FreeRTOS task (Core 1, priority 1) drains both queues. Dish queue is checked first for responsiveness.

### Upload Job Structure

Each `UploadJob` contains:
- `job_id` -- Unique ID for tracking
- `mode` -- Operation type (dish, check-in, check-out, discard, voice)
- `image_buf` / `image_len` -- PSRAM-allocated image data
- `expiry` -- Expiration date (check-in mode)
- `quantity` -- Item quantity
- `retries` -- Retry count
- `camera_meta` -- Capture metadata (luma, profile, resolution)

### Persistence (Production Only)

Under `HALO_SENSE_UPLOAD_PERSISTENCE`, one queued upload can be saved to SPIFFS before sleep:
- On sleep, `sleep_defer_queued_background_uploads()` saves the first valid job
- On next boot, `upload_persist_maybe_replay()` restores and retries
- Voice uploads have a panic backoff to prevent crash loops

### Upload Abort for Dish Priority

Background uploads (check-in, voice) can be aborted mid-PUT when a new dish scan arrives:
- `allow_abort=true` flag on PUT requests
- PUT monitors `dish_scan_inflight` flag
- Aborted upload is re-queued with incremented retry count

---

## 8. OTA System

### Dual OTA Architecture

HALO uses a dual OTA system because the LCD board cannot perform TLS directly (insufficient DMA RAM for WiFi + TLS + LVGL):

```
Cloud (S3)                    Sense                         LCD
  |                             |                             |
  |<-- HTTPS GET manifest -----|                             |
  |--- manifest.json --------->|                             |
  |                             |-- LCD_OTA_QUERY ---------->|
  |                             |<- LCD_OTA_QUERY_RESP ------|
  |                             |  (current FW version)      |
  |<-- HTTPS GET firmware.bin --|                             |
  |--- binary stream --------->|                             |
  |                             |-- LCD_OTA_BEGIN ---------->|
  |                             |<- LCD_OTA_BEGIN_ACK -------|
  |                             |                             |
  |                             |== COBS binary frames =====>|
  |                             |  (512B chunks + CRC16)     |
  |                             |<= MSG_ACK per chunk ======|
  |                             |                             |
  |                             |-- LCD_OTA_END ------------>|
  |                             |<- LCD_OTA_END_ACK ---------|
  |                             |  (sha_match + ota_ok;       |
  |                             |   success needs BOTH true)  |
  |                             |                      [reboot]
```

### Sense Self-OTA

1. Fetch manifest from S3: `s3://{bucket}/halo/ota/{channel}/sense/manifest.json`
2. Compare version using semantic versioning
3. Download firmware binary
4. Write to OTA partition via `esp_ota_begin/write/end`
5. Set boot partition and reboot
6. Health gate validates new firmware on next boot

### LCD OTA Proxy (via Sense UART)

The Sense board acts as a TLS proxy for LCD OTA:

1. **Query**: Sense sends `LCD_OTA_QUERY` over JSON UART
2. **Manifest**: Sense fetches LCD manifest from S3 (`lcd/manifest.json`)
3. **Version compare**: Skip if LCD already running latest
4. **Download**: Sense downloads LCD binary over HTTPS. **Stall-tolerant**: if the S3 stream stalls (>30s no data) or drops before completion, Sense re-opens the GET with a `Range: bytes=<offset>-` header and resumes from the current offset rather than aborting — up to 5 consecutive reconnects (counter resets on data progress). The overall 40-min download timeout still bounds the transfer.
5. **UART switch**: `g_lcd_ota_proxy_owns_uart = true` (blocks JSON RX)
6. **Stream**: COBS-framed binary chunks (512B each) with CRC16 and ACK/NACK
7. **Resume**: NVS progress saved every 64KB; can resume after power loss
8. **Verify**: LCD computes SHA256 over entire received binary
9. **Apply**: LCD calls `esp_ota_set_boot_partition()` and reboots

**Single net stack contention:** Sense has one network stack shared by OTA HTTPS and normal app traffic. While any OTA is in flight (`lcd_ota_in_progress()`), non-OTA HTTP — specifically `request_list_refresh()` / the `INPUT_WAKE`-triggered `/v1/list` GET — is suppressed so it cannot starve the S3 download. OTA's own HTTP (manifest, download, schedule/report) is never gated.

**Cloud `lcd_fw` is the REAL running version:** the cloud-reported LCD firmware version (`truth_get_lcd_fw_version()`) is sourced exclusively from an actual `LCD_OTA_QUERY_RESP`, never from the OTA manifest. On proxy success the cached value is cleared and re-queried (pre-sleep / periodic, refreshed when empty or >5min stale), so a transfer that completes but never boots the new image no longer masquerades as success in the dashboard.

### OTA Orchestration Order (LCD proxy FIRST, then Sense self-OTA)

When a manual/button-triggered OTA check (`maybeRunOtaCheck()` in `halo_sense_prod.ino`) finds a Sense update to apply, the dual-board update is ordered **LCD proxy first, Sense self-OTA second**:

1. **LCD proxy (inline, while the LCD is awake from the button press):** Before applying the Sense image, Sense runs the LCD proxy inline on the main task — `sense_lcd_ota_query()` → `sense_lcd_ota_fetch_manifest(cfg->base_dir, cfg->channel, …)` → version compare → `OTA_LOCK` + `sense_lcd_ota_proxy()`. This blocks the main loop (so there is no UART-drain race), and `sense_lcd_ota_proxy()` manages `g_lcd_ota_proxy_owns_uart` itself during COBS streaming.
2. **Sense self-OTA + reboot:** Only after the LCD proxy completes does Sense call `applyToOtaPartition()` and reboot.

**Why:** Previously the Sense applied its own image and **rebooted FIRST**, deferring the LCD proxy to the next boot via the `lcd_ota_due` NVS flag. By the time the rebooted Sense queried the LCD, the LCD had often gone back to sleep → intermittent `lcd_query_fail`. Doing the LCD proxy first, while the LCD is still awake, eliminates that race.

**`lcd_ota_due` is now a fallback only:** `set_lcd_ota_due_nvs()` is set based on the inline proxy outcome — **cleared on success / already-up-to-date**, **set when the proxy is skipped or fails** (query fail, manifest fetch fail, or a non-`"success"` proxy result). The boot-time `lcd_ota_due` handler in `run_maintenance_if_needed()` retries the LCD OTA on the next boot in the failure case. The post-apply code no longer unconditionally clears `lcd_ota_due`, so a transient LCD failure followed by a successful Sense apply still leaves the next-boot retry armed.

**Scheduled maintenance OTA uses the same LCD-proxy-first order (`run_maintenance_if_needed()`):** The in-window dual-board sequence is **LCD proxy first, Sense self-OTA second**, for an even stronger reason than the manual path. A scheduled window wakes both boards from their own deep-sleep timers; the Sense self-OTA (download + apply + reboot, ~1–3 min) outlasts the LCD's OTA_LOCK stay-awake budget, and after a Sense reboot the Sense **cannot wake the LCD** (GPIO39 is LCD→Sense only). If the Sense rebooted first, the LCD would have idle-slept and be stranded on old firmware (`lcd_fw=unknown` / `lcd_ota_result=unknown` after the window). New order:
1. `OTA_LOCK` (keep LCD awake/listening).
2. LCD proxy retry loop (`run_lcd_maintenance_ota_attempt` × `LCD_MAINT_OTA_MAX_ATTEMPTS`, window-budget guarded); success tracked via `lcd_maintenance_result_successful()`.
3. `OTA_LOCK` re-asserted; Sense self-OTA (`maybeRunOtaCheck`, which now no-ops its own inline LCD proxy because the LCD is already current). May `esp_restart()` on success — fine, the LCD is already updated.
4. `OTA_UNLOCK` (only if the Sense did not reboot).
5. **Conditional** `set_lcd_ota_due_nvs(!lcd_proxy_succeeded)` — clear on LCD success, set on LCD failure so the boot-time `lcd_ota_due` fallback retries next boot/window. (Was an unconditional clear before.)

This eliminated the prior scheduled-window failure mode where the LCD was deferred to the post-reboot `lcd_ota_due` branch but had already idle-slept and could not be woken.

### S3 Bucket Layout

```
halo-ota-dev/
  halo/ota/
    dev/
      sense/
        manifest.json       # {version, url, sha256, size}
        firmware-X.Y.Z.bin
      lcd/
        manifest.json
        firmware-X.Y.Z.bin
```

### Schedule API

The OTA schedule is managed via a cloud API:

- **Endpoint**: `https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com/ota/schedule`
- **Method**: GET with `device_id` parameter
- **Response**: Maintenance window with `start_epoch`, `duration_sec`, `grace_before/after_sec`, `request_id`
- **Report**: POST to `/ota/report` with result status

### Window-Start Cancel-Safety (Schedule Re-Validation)

A scheduled window is fetched and armed (RTC timer + NVS) hours before it fires. If the operator **disables or replaces** the schedule in the cloud after the device has armed it, the device would otherwise act on its stale cached copy. To prevent this, `run_maintenance_if_needed()` re-GETs `/ota/schedule` at window-start — after the in-window and idle checks pass, but **before** sending `OTA_LOCK` — via `ota_sched_revalidate()`.

The cancel is honored via `enabled=false` on the schedule row (HTTP 200), **not** a row delete. The schedule GET only returns enabled windows whose `start_epoch` is in the **future**, so a 204 means "no future window" — which includes a live, enabled window whose start has already passed (the normal case, since the device wakes **at** the window) as well as a deleted row. A 204 is therefore **fail-open** so legitimate scheduled OTAs are never falsely aborted; an `enabled=false` row returns HTTP 200 at any wake time, making it the only reliable cancel signal.

- **HTTP 204** (no future window — includes a live past-start window, or a deleted row) -> **fail-open**, proceed with the cached window
- **HTTP 200 `enabled=false`** (disabled) -> abort, result `schedule_cancelled`
- **HTTP 200 `enabled=true`** with a `request_id` different from the cached window -> abort, result `schedule_replaced`
- **HTTP 200 `enabled=true`** with matching `request_id` -> proceed with OTA
- **Network error / not-ready (no WiFi, time invalid, parse error, other HTTP code)** -> **fail-open**, proceed with the cached window

> **Operational rule:** to pull a scheduled release, set `enabled=false` on the device's schedule row — do **not** delete it (a delete returns 204, which is fail-open).

On abort the device records the result (reported to cloud at pre-sleep), clears the consumed/follow-up retry state, and calls `MaintenanceWindow::clear()` to **wipe the cached NVS window** so the next pre-sleep does not re-arm it. Because `OTA_LOCK` has not been sent yet on the abort path, no `OTA_UNLOCK` is required; the device simply enters `SENSE_SLEEP_DEEP_MAINT`.

### Cloud Truth Payload (Observability)

`build_truth_json()` (Sense `Truth.cpp`) emits the device ground-truth state to the cloud. For LCD observability it now carries, in addition to the existing `lcd_fw` / `lcd_fw_age_s`:
- `lcd_running_state` -- the REAL LCD running partition state (`NEW`/`PENDING_VERIFY`/`VALID`/`INVALID`/`ABORTED`/`UNDEFINED`/`UNKNOWN`), captured from the most recent `LCD_OTA_QUERY_RESP` via `truth_get_lcd_running_state()` (strong getter in `halo_sense_prod.ino` forwarding `sense_lcd_last_running_state()`; weak default `"UNKNOWN"` in `Truth.cpp`)
- `lcd_ota_result` -- last LCD OTA result string (`truth_get_lcd_ota_result()`)

`lcd_fw` is now always a real-queried running version (never the assumed manifest version — see "Cloud `lcd_fw` is the REAL running version" above), so it can be cross-checked against `lcd_running_state` for ground-truth LCD partition health. Observability only; OTA control flow and success/failure determination are unchanged.

### Maintenance Window Handshake (MAINT_WINDOW)

When Sense receives a maintenance schedule from the cloud:

1. Sense sends `MAINT_WINDOW` to LCD with timing details
2. LCD persists window to NVS and arms RTC timer wake
3. Both boards sleep
4. Timer wake: LCD enters maintenance mode (headless, no UI init)
5. LCD wakes Sense via INT_PIN
6. Sense performs OTA checks for both boards
7. LCD receives OTA via UART COBS if update available
8. Both boards sleep after maintenance completes

#### MAINT_WINDOW carries the Sense wall clock (`now_epoch`) — LCD self-wake from the absolute window

The LCD has **no NTP/RTC time source of its own** (no `settimeofday`/`configTime` anywhere
in its firmware), so on its own `time(nullptr)≈0` and `lcd_time_valid()` is false — which
left its absolute-epoch maintenance machinery dormant. To fix scheduled-OTA wake timing:

- **Sense:** `send_maint_window()` (`halo_sense_prod.ino`) adds a `now_epoch` field carrying
  `time(nullptr)` whenever `is_time_valid()`. Omitted (and the LCD falls back to the relative
  `wake_in_s`/`remaining_s` offsets) when the Sense clock is invalid — fully backward
  compatible.
- **LCD:** the `MAINT_WINDOW` handler (`lcd_uart_rx.h`) parses `now_epoch` and calls
  `lcd_set_clock_from_sense()` (`LCD_Minimal.ino`) which `settimeofday()`s the LCD clock
  **before** arming the timer / persisting NVS. This makes `lcd_time_valid()` true (it just
  checks `time(nullptr) > 1700000000`), and ESP-IDF carries the set time across deep sleep via
  the RTC, so subsequent partial (touch/timer) wakes keep an accurate clock. Re-applied on
  every MAINT_WINDOW (cheap drift correction).
- **LCD self-wake (`lcd_sleep.h`):** when maintenance is armed AND the clock is valid AND
  `g_lcd_maintenance_start_epoch > 0`, the deep-sleep timer is computed from the **absolute
  window** every sleep: `target = start_epoch − grace_before_sec − LCD_MAINT_WAKE_LEAD_S`
  (15s, matching the Sense's `nextWakeEpochForSleep(now,15,5)` lead); `sleep_timer_sec =
  target − now_epoch`. Recomputing each sleep makes intermediate pre-window wakes harmless
  (the old code re-armed the full stale relative offset and missed the window). If already
  at/inside the band, it clamps to a 5s safety timer instead of sleeping past the window.
  Relative `g_lcd_maintenance_wake_in_s` is the fallback only when the clock is invalid (legacy).
- **LCD stays awake through the window (`LCD_Minimal.ino` sleep gate):** when the clock is
  valid and `lcd_maintenance_window_is_current(time(nullptr))`, sleep is inhibited
  (`reason=maintenance_window`) so the Sense's LCD-first OTA proxy (queries the LCD with
  retries over ~35s at window entry) can reach it. Bounded by `start − grace_before` ..
  `start + duration + grace_after`, so it self-terminates; clock-gated so normal idle-sleep
  is never affected.

Key constants:
- `MAINT_SYNC_RESEND_MS = 1500` -- Resend MAINT_WINDOW if no ACK
- `MAINT_SYNC_MAX_SENDS = 3` -- Max retransmissions
- `LCD_OTA_SCHED_START_MIN = 120` -- Default schedule: 2:00 AM local
- `LCD_OTA_SCHED_WINDOW_MIN = 30` -- 30-minute window

---

## 9. Re-Wake System

A five-layer defense prevents the device from getting stuck in deep sleep or failing to wake:

### Layer 1: Safety Timer (Disabled)

```cpp
static const uint32_t SENSE_MAX_SLEEP_TIMER_S = 0;  // disabled
```

Previously a 5-minute fallback timer, this is now disabled (`0`). The Hard Gate architecture (see below) makes it unnecessary -- GPIO39 pulses are blocked once Sense is confirmed awake, eliminating the spurious re-wake scenarios this timer was designed to catch. Keeping it at zero avoids battery-wasting timer wakes.

### Hard Gate (`sense_awake_confirmed`)

The Hard Gate is the "**Pulse Once, UART Forever**" paradigm: GPIO39 is only used for the initial wake from deep sleep, then all subsequent communication uses UART.

**Mechanism:**

- `sense_awake_confirmed` is a boolean flag on the LCD board that blocks ALL GPIO39 pulses once Sense is confirmed awake
- **Set to `true`** when LCD receives any of: `PONG`, `SYNC_ACK`, or `UI_STATUS` from Sense via UART -- these prove Sense is alive and listening
- **While the gate is active:** `lcd_maybe_pulse_sense_int()`, `wake_sense_for_request()`, and `request_sense_wake()` all skip the GPIO39 pulse and send a UART ping instead
- **Cleared** when Sense goes to sleep (`SLEEP_READY` received), re-enabling GPIO39 for the next wake cycle

This eliminates an entire class of bugs where redundant GPIO39 pulses during normal operation could cause EXT0 re-wake on the next sleep entry.

### Layer 2: RELEASE_WAKE

Before entering deep sleep, Sense sends `RELEASE_WAKE` over UART asking LCD to deassert GPIO39 (INT_PIN). This prevents the wake GPIO from being stuck active, which would cause an immediate re-wake from EXT0.

### Layer 3: Timer Wait for Wake Pin

```cpp
static const unsigned long WAKE_PIN_STABLE_WAIT_MS = 200;
```

Before deep sleep entry, Sense polls the wake GPIO and waits for it to go inactive (HIGH). If it remains active after the wait window, Sense applies mitigation (pull-up, delay, re-check).

### Layer 4: GPIO39 JTAG Fix

GPIO39 on some ESP32-S3 modules is a JTAG pin that can float or remain driven. The firmware explicitly configures RTC pull direction to match the inactive level before sleep:

```cpp
if (WAKE_INACTIVE_LEVEL == 1) {
    rtc_gpio_pullup_en(WAKE_GPIO);
    rtc_gpio_pulldown_dis(WAKE_GPIO);
}
```

### Layer 5: Boot-All-Wake

On every boot (including cold boot), LCD sends a wake pulse to Sense via INT_PIN. This ensures Sense is always wakened when LCD is active, regardless of how LCD woke up.

---

## 10. Voice Recording

### Flow

1. **Long press** on LCD touch screen detected
2. LCD sends `INPUT_LONG_PRESS` to Sense
3. Sense starts I2S recording via `audio_bsp` into 512KB PSRAM buffer
4. LCD shows "AI Listening" screen with countdown
5. User releases touch -- LCD sends `INPUT_LONG_PRESS_END`
6. Sense finalizes recording, sends to Quick-Ack API (OpenAI Realtime API proxy)
7. API returns text response (and optionally item modifications)
8. Sense sends `UI_VOICE_RESPONSE` to LCD for display

### GPIO41 Shared Pin Fix

GPIO41 is shared between UART TX and the I2S microphone data line:

1. **Before recording**: Disable UART TX on GPIO41, set pin to float mode
2. **Configure attenuation**: Apply 4x attenuation to reduce noise
3. **During recording**: I2S reads microphone data on GPIO41
4. **After recording**: Re-enable UART TX on GPIO41

### Fire-and-Forget Mode

Voice uploads use a fire-and-forget pattern:
- After recording ends, Sense uploads audio in the background
- LCD shows a brief acknowledgment and returns to the main menu
- Voice response arrives asynchronously and is displayed if still relevant
- Session timeout: 90 seconds of idle before session ID resets

### Voice Session

Voice sessions allow multi-turn conversations:
- Session ID generated on first voice interaction
- Subsequent voice commands within 90 seconds reuse the session
- Session includes conversation context for follow-up questions

---

## 11. Provisioning

### QR Code Flow

1. **Unprovisioned state**: Device has no `owner_id` in NVS
2. **Sense generates QR data**: Contains device_id + claim code
3. **QR display**: Sense sends `PROVISION_QR` to LCD, which renders QR code
4. **User scans QR**: Mobile app sends claim request to backend API
5. **Backend claim**: Associates device_id with user's owner_id
6. **Device polls**: Sense periodically checks claim status
7. **Claim complete**: owner_id written to NVS, QR screen dismissed
8. **WiFi credentials**: Provisioned via separate flow (WIFI_CREDS message)

### Device ID Generation

Device ID is derived from the ESP32 eFuse MAC address:

```cpp
uint64_t mac = ESP.getEfuseMac();
snprintf(out, out_len, "halo-%02x%02x-%02x%02x",
         (mac >> 24) & 0xFF, (mac >> 16) & 0xFF,
         (mac >> 8) & 0xFF, mac & 0xFF);
```

**Important:** Sense and LCD boards have **different MAC addresses** and thus different device IDs. The Sense device_id is used for all API calls. The LCD device_id appears in LCD OTA manifests.

### Owner ID

- Stored in NVS via `ProvisioningState::saveOwnerId()`
- Used in all API calls (presign, list, voice)
- `owner_code` is a transient claim token; cleared after successful claim
- If `owner_code` exists but `owner_id` is already set, the stale code is cleared

---

## 12. Error Handling

### NVS Error Log (lcd_errlog.h)

The LCD board maintains a persistent error ring buffer in NVS:

- **Namespace**: `err_log`
- **Capacity**: 20 entries (ring buffer)
- **Persistence**: Survives reboots and deep sleep
- **Storage**: JSON strings per entry

Operations:
- `errlog_store(json_str)` -- Write new entry, advance head pointer
- `errlog_dump(Print&)` -- Print all entries (newest first)
- `errlog_read_entry(index, buf, size)` -- Read single entry by reverse index
- `errlog_count()` -- Get stored entry count

Errors are viewable via:
- **Debug screen** on LCD (navigate to Settings > Debug > Error Log)
- **USB serial** `errors` command

### SENSE_DIAG Forwarding

Sense forwards diagnostic events to LCD via `SENSE_DIAG` messages:

```json
{
  "type": "SENSE_DIAG",
  "category": "wifi",
  "event": "rssi_report",
  "label": "connected",
  "code": -65,
  "detail": "rssi=-65 ip=192.168.1.42"
}
```

Categories include: `wifi`, `ota_sched`, `camera`, `upload`, `boot`.

The LCD stores critical diagnostics in its error log for post-mortem analysis.

### Crash Recovery

RTC_DATA_ATTR variables persist crash context across resets:

| Variable | Purpose |
|---|---|
| `g_rtc_clean_shutdown` | 1 if last sleep was clean; 0 on crash |
| `g_rtc_crash_count` | Consecutive unclean boot counter |
| `g_rtc_last_stage` | Last noted stage before crash |
| `g_rtc_last_stage_code` | Error code at crash stage |
| `g_rtc_last_crash_reason` | ESP reset reason of last crash |

---

## 13. Build and Deploy

### Compile Commands

```bash
# LCD (always compile via the prod wrapper -- it includes LCD_Minimal.ino)
cd /Users/MattTaylor/Documents/Arduino/HALOMAIN_rev1p5_modular
arduino-cli compile \
  --fqbn "esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --build-path "/tmp/halo_lcd_check" \
  halo_ota_demo/firmware/halo_lcd_prod/halo_lcd_prod.ino

# Sense (uses XIAO ESP32S3 board definition)
arduino-cli compile \
  --fqbn "esp32:esp32:XIAO_ESP32S3:PSRAM=opi" \
  --build-path "/tmp/halo_sense_check" \
  halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino
```

**Never compile LCD_Minimal.ino directly** -- always use the `halo_lcd_prod` wrapper.

### Flash Commands

```bash
# Sense (port 1101) -- app-only flash preserves NVS/WiFi credentials
arduino-cli upload --fqbn "esp32:esp32:XIAO_ESP32S3:PSRAM=opi" \
  --port /dev/cu.usbmodem1101 \
  --input-dir /tmp/halo_sense_check

# LCD (port 101) -- app-only flash preserves NVS
arduino-cli upload \
  --fqbn "esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi" \
  --port /dev/cu.usbmodem101 \
  --input-dir /tmp/halo_lcd_check
```

**Critical port mapping:**
- Sense = `/dev/cu.usbmodem1101`
- LCD = `/dev/cu.usbmodem101`

Use app-only flash (offset 0x10000) to preserve WiFi credentials and NVS state. Only use merged.bin for factory reset.

### OTA Publish Pipeline (publish_both.sh)

```bash
./publish_both.sh --version 6.1.315 --channel dev --profile trepo-dev --bucket halo-ota-dev
```

Steps:
1. Generate `Version.h` for Sense with `generate_version_header.py`
2. Copy `Version.h` to `halo_sense_prod/`
3. Publish Sense OTA via `publish_ota.py` (compile + upload to S3)
4. Generate `Version.h` for LCD
5. Copy `Version.h` to `halo_lcd_prod/`
6. Compile LCD with `arduino-cli`
7. Publish LCD OTA via `publish_lcd_ota.py` (upload binary to S3)

### OTA Channels

- `dev` -- Development channel (S3 bucket: `halo-ota-dev`)
- `prod` -- Production channel (S3 bucket: `halo-ota-prod`)

Both use the same S3 prefix: `halo/ota/{channel}/{board}/`

---

## 14. Test Infrastructure

All tools are in `/Users/MattTaylor/Documents/Arduino/HALOMAIN_rev1p5_modular/tools/`.

### LVGL Image Converter (lvgl_image_converter/)

Turns any image into an LVGL `lv_img_dsc_t` C asset for the LCD UI. Web UI (`python3 tools/lvgl_image_converter/server.py` → http://localhost:9097, drag-drop + live preview on checkerboard/teal/white/black) and a CLI (`convert.py`). Defaults are tuned for this project: RGB565 with `LV_COLOR_16_SWAP=1` byte-swap ON (the easy-to-get-wrong part). Formats: `true_color_alpha` (colour+alpha), `alpha_8bit` (recolourable mono mask), `true_color`. Options: crop-to-content, recolour to a single colour, alpha modes (source / white-key / opaque). Drop the generated `.c` in `halo_ota_demo/firmware/halo_lcd_prod/`, `LV_IMG_DECLARE(name)`, and `lv_img_set_src`. Used to generate the menu/listening icon assets (`dish_icon.c`, `mic_icon.c`). Needs Pillow.

### TAP Actuator (tap_implementation/tap.py)

Physical servo actuator that taps the device touch screen to wake it from deep sleep. Used for automated testing when the device is sealed in its enclosure.

- Commands: `TAP` (brief press), `PUSH:<ms>` (timed hold), `STOP`
- Default port: `/dev/cu.usbmodem21201`
- 3-second boot wait for Arduino actuator firmware

### Serial Capture Server (serial_capture.py)

Dual-port web-based serial monitor at `localhost:9091`:
- Captures from both Sense and LCD USB serial simultaneously
- Live SSE (Server-Sent Events) stream
- Start/stop recording
- Color-coded log output (per-board)
- Web UI for real-time monitoring

### Upload Monitor Dashboard (upload_monitor.py)

Real-time upload viewer at `localhost:9092`:
- Polls CloudWatch logs and S3 for upload events
- SSE stream for live updates
- Tracks presign, PUT, and result events

### Automated Test System (halo_test.py)

End-to-end device test orchestrator:
- Configured via `halo_test_config.json`
- Uses TAP actuator to wake device
- Monitors serial output for expected events
- 60-second wake timer for autonomous cycles
- Supports unattended OTA testing

### Stress Capture (stress_capture.sh)

Automated rapid capture test:
- Injects dish/check-in scans via USB serial JSON commands
- Monitors for capture success/failure
- Tests camera DMA resilience under rapid-fire captures
- Auto-dismisses result screens

### Other Tools

| Tool | Purpose |
|---|---|
| `diag_dashboard.py` | Diagnostic visualization dashboard |
| `image_dashboard.py` | Captured image browser/viewer |
| `ota_test.py` | OTA-specific test automation |
| `serial_simple.py` | Lightweight single-port serial monitor |
| `test_dashboard.py` | Test result visualization |
| `wifi_coldstart_test.py` | WiFi connection cold-start timing tests |

### Serial Action Injection

USB serial commands can inject actions on the Sense board (useful for testing without touch):

```json
{"ver":1,"type":"INPUT_SCAN","msg_id":1,"ts":0,"mode":"dish","menu_item":"Log Dish","menu_index":2}
{"ver":1,"type":"INPUT_SCAN","msg_id":2,"ts":0,"mode":"check-in","menu_item":"Check In","menu_index":0}
{"ver":1,"type":"INPUT_SCAN","msg_id":3,"ts":0,"mode":"discard","menu_item":"Discard","menu_index":1}
```

---

## 15. Device IDs and API

### Device IDs

Each ESP32-S3 has a unique eFuse MAC. Since Sense and LCD are separate chips, they have **different device IDs**:

- **Sense device_id**: Used for all Trepo API calls (presign, list, voice, OTA schedule)
- **LCD device_id**: Used only in LCD OTA manifest checks

Format: `halo-XXXX-XXXX` (derived from MAC bytes 3-6)

### API Endpoints

| Endpoint | Base URL | Purpose |
|---|---|---|
| `/v1/list` | `1zc0nh8x48.execute-api.us-east-1.amazonaws.com` | Shopping list CRUD |
| `/v1/voice` | same | Voice command processing |
| `/presign` | `7tn3gvwvh7.execute-api.us-east-1.amazonaws.com` | S3 presigned URL for image upload |
| `/presign/discard` | same | Presign for discard operations |
| `/ota/schedule` | same | OTA maintenance window schedule |
| `/ota/report` | same | OTA result reporting |
| `/voice-ack-async` | `ivwu7ls6p8.execute-api.us-east-1.amazonaws.com` | Quick-ack voice API (OpenAI Realtime) |

### S3 Upload Path

Images are uploaded to S3 via presigned PUT URLs. The presign API returns a URL pointing to:
```
s3://trepo-uploads/{owner_id}/halo/{device_id}/{job_id}.jpg
```

### MQTT Topics

- `trepo/halo/{device_id}/ota` -- OTA intent notifications
- `trepo/{owner_id}/{device_id}/jobs/{job_id}/result` -- Dish analysis results (dev builds only; production polls HTTP)

---

## Appendix: Modular File Maps

### Sense Headers (21 files)

| Header | Content |
|---|---|
| `sense_wifi.h` | WiFi connect/disconnect, recovery, guard state machine |
| `sense_upload.h` | S3 upload core, HTTP client management |
| `sense_mqtt.h` | MQTT connect, subscribe, command dispatch (dev only) |
| `sense_camera.h` | Camera init, capture, quality validation, profiles |
| `sense_sleep.h` | Sleep entry, wake sources, handshake, GPIO management |
| `sense_voice.h` | Audio recording, voice upload, session management |
| `sense_list.h` | List fetch, shopping list, delete |
| `sense_presign.h` | Presign URL generation, check-in/dish operations |
| `sense_uart_msg.h` | UART message formatting (sync, toast, voice, sleep) |
| `sense_scan.h` | Scan flow helpers, screen hints, mode classification |
| `sense_op_queue.h` | Operation job queue, worker task |
| `sense_upload_exec.h` | Upload execution, retry logic |
| `sense_upload_queue.h` | Upload queue management, sleep defer |
| `sense_upload_persist.h` | Upload NVS persistence (SPIFFS) |
| `sense_time.h` | NTP/RTC time cache for TLS bootstrap |
| `sense_ota_lcd.h` | LCD OTA proxy: manifest fetch, binary download, UART COBS stream |
| `sense_uart.h` | UART init, ring buffer, JSON send |
| `sense_http.h` | HTTP client helpers, TLS configuration |
| `sense_diag.h` | Diagnostic recording, stage tracking |
| `sense_ops.h` | Operation types, job structs, priority enum |
| `audio_bsp.h` | I2S microphone driver |

### LCD Headers (15 primary files)

| Header | Content |
|---|---|
| `lcd_uart.h` | UART TX: init, send helpers, TX queue |
| `lcd_anim.h` | Glow/processing animation, LVGL tick, status screen |
| `lcd_provision.h` | Provisioning QR screen, status labels |
| `lcd_ship_screens.h` | Main menu, voice/AI UI, second menu, settings |
| `lcd_ship_flow.h` | Animations, timeout rings, screen init/show |
| `lcd_ship_route.h` | Screen routing, touch handling, result/debug screens |
| `lcd_ship_action.h` | Menu actions, meal results, custom UI |
| `lcd_menu.h` | Menu display, button handlers, scroll |
| `lcd_sleep.h` | Sleep/wake, INT_PIN pulse, sleep handshake |
| `lcd_ota_uart.h` | OTA-over-UART receiver: state machine, esp_ota, SHA256, NVS resume |
| `lcd_uart_rx.h` | UART RX message processing (runs on Core 0) |
| `lcd_ui_task.h` | FreeRTOS UI task, app_event_queue processing |
| `lcd_uart_task.h` | FreeRTOS UART task (Core 0) |
| `lcd_activity.h` | Activity timer, guardian force-sleep, sleep coordination |
| `lcd_persist.h` | NVS storage, wake diagnostics |

### Shared Library (halo_ota_demo/firmware/shared/)

Key files: `UartOtaProtocol.cpp/h`, `ManifestClient.cpp/h`, `ProvisioningState.cpp/h`, `ProvisioningManager.cpp/h`, `MqttClient.cpp/h`, `SenseOtaApplier.cpp/h`, `MaintenanceWindow.cpp/h`, `OtaIntent.cpp/h`, `HealthGate.cpp/h`, `WifiGuard.cpp/h`, `HaloPins.h`, `BoardConfig.h`, `BuildInfo.cpp/h`, `Version.h`

---

## Key Design Constraints

1. **No LVGL from Core 0** -- LVGL is not thread-safe. All display updates must happen from the UI task on Core 1. Cross-core updates use flags and FreeRTOS event queues.

2. **No light sleep on Sense** -- Compile-time guard prevents `esp_light_sleep_start()`. Only deep sleep is allowed (light sleep breaks WiFi/camera state).

3. **LCD has no WiFi** -- All network operations are proxied through Sense over UART. LCD OTA is streamed via COBS binary protocol.

4. **8MB flash limit** -- Both boards. Partition table is custom. OTA requires app partition < 50% of usable flash.

5. **PSRAM contention** -- Camera DMA and TLS both need large contiguous PSRAM blocks. The DMA guard pattern (reserve/release) prevents allocation failures.

6. **GPIO41 shared pin** -- Microphone data and UART TX share GPIO41 on Sense. Must disable TX and float pin before voice recording.

7. **Encoder wake mask** -- Encoder pins must not be in EXT1 wake mask (enc_b can rest LOW, causing instant wake loop).

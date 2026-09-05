# HALO Firmware Architecture

Last updated: 2026-05-12

---

## 1. System Overview

HALO is a kitchen-mounted food intelligence device that captures images of food (groceries, meals, pantry items), uploads them to AWS for AI processing, and displays results on a round LCD screen. It also supports voice commands via a built-in microphone.

The device uses a **two-board ESP32-S3 architecture**:

- **Sense board** (XIAO ESP32S3 Sense) -- Camera, WiFi, microphone, all network operations
- **LCD board** (custom ESP32-S3 with round display) -- 1.28" round LCD, touch, rotary encoder, all UI rendering

The boards communicate over a dedicated **UART link at 115200 baud**. The LCD board is the **sleep leader** -- it decides when the system sleeps based on user inactivity. The Sense board is the **network leader** -- it owns all WiFi, HTTPS and OTA downloads.

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
      halo_sense_prod/     # Production Sense wrapper (OTA + provisioning)
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

> **Two TX paths, and they are not equivalent.** UI actions go through
> `uart_tx_enqueue()` → `uart_tx_queue` → the drain in `uart_task`, which is where awake-proof
> checking and the deferred ring live. USB-injected commands call `uart_send_json()` **directly**
> and bypass all of that. This matters when testing: injecting
> `{"type":"INPUT_MENU_SELECT"}` over USB will never exercise the deferral path, because the
> drain never sees it. Use the bench `enq` command (gated by `HALO_SPOOL_TEST`), which goes
> through the real enqueue call — only the trigger is synthetic.

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
| `INPUT_FW_INFO` | Request `FW_INFO` via the **BLOCKING diagnostic path**: Sense runs a fresh `sense_lcd_ota_query()` (up to 7000ms × 5) so the reply carries the REAL running LCD fw + partition/state. Used by automation/diagnostics that need fresh `lcd_fw` |
| `INPUT_SENSE_FW` | Request `FW_INFO` via the **FAST path**: Sense replies IMMEDIATELY with live `sense_fw` + CACHED `lcd_fw` (`g_lcd_ota_version`), with NO blocking LCD query. Used by the LCD Settings screen to display the Sense version instantly. The Settings handler reads only `sense_fw` (it ignores `lcd_fw`), so the multi-second diagnostic query is wasted there — hence this fast path |
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
| `MAINT_KEEPALIVE` | Arm-time keep-awake (race fix). Sent throughout the post-wake WiFi+NTP+schedule-fetch phase (bounded ~25s after wake, gated by `!g_ota_check_done` or pending-unacked) so the tapped LCD doesn't idle-sleep before the real `MAINT_WINDOW` (with `now_epoch`) can be delivered. Carries only `request_id`; LCD just resets its activity timer + nudges stay-awake |
| `PROVISION_QR` | QR code data for provisioning display |
| `FW_INFO` | Firmware versions reply to `INPUT_FW_INFO` or `INPUT_SENSE_FW`. **Diagnostic reply (INPUT_FW_INFO):** real running firmware of BOTH boards + LCD partition/state: `sense_fw`, `lcd_fw` (freshly queried, not cached manifest), `lcd_running_part`, `lcd_running_state`, `lcd_boot_part`, `lcd_fw_age_s`; `lcd_fw="unknown"` if the fresh LCD query fails. **Fast reply (INPUT_SENSE_FW):** `sense_fw` (live) + `lcd_fw` (CACHED `g_lcd_ota_version`, or `"unknown"`); partition/state fields omitted, no blocking query |

### Delivery Guarantee for User-Intent Messages

`INPUT_*` messages that carry user intent are **acknowledged and retransmitted**; everything else remains fire-and-forget.

- The LCD holds each tracked message (`lcd_link_ack.h`) until the Sense returns `INPUT_ACK` with a matching `ack_id`, retransmitting every 400 ms up to 5 attempts.
- After the budget is spent it posts `EVT_LINK_SEND_FAILED`, and the UI resolves with "Couldn't reach sensor" instead of stranding on the capturing screen. A lost `INPUT_MENU_SELECT` was the single best explanation for the reported capture hang.
- A retransmit replays the **original bytes with the original `msg_id`**. Re-serializing would mint a new `msg_id` (`get_next_msg_id()` increments) and the Sense could not tell a retry from a second button press.
- The Sense therefore **must** dedupe: it acks *before* doing any work, then suppresses the repeat action for any `msg_id` in its 8-deep seen-ring. Retransmission without dedupe turns one tap into two captures — a worse bug than the one being fixed. A duplicate is still re-acked, because the LCD is retrying precisely because it did not hear the first ack.
- The LCD does not sleep while a tracked message is unacked (bounded: a slot is retired after ~2 s regardless).

Tracked types mirror `input_requires_sense()`. `INPUT_SCROLL` and `INPUT_PING` are deliberately excluded — high-frequency and near-idempotent, and retransmitting them would add load exactly when the link is already struggling.

Verified on hardware: happy path `tracked=3 acked=3 retries=0` (RTT 17 ms) with the Sense seeing exactly 3; induced loss gave `retry 2/5..5/5` then `GIVE_UP` at 2011 ms plus the UI error; dropped acks gave 3 receptions of one `msg_id`, 2 suppressed, exactly 1 scan job queued.

### COBS Binary Protocol (OTA and image spool)

During LCD OTA updates **and capture-image spooling**, the UART switches from JSON to **COBS-framed binary**. This uses `UartOtaProtocol` (shared between both boards).

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

Image-spool types are **deliberately distinct** from the firmware types above. Both transfers share this link and this framing, and the consequences are asymmetric: a firmware frame written into a `.jpg` is a corrupt photo, but an image frame written to an OTA partition is a bricked board.
- `MSG_IMG_BEGIN (0x11)`, `MSG_IMG_CHUNK (0x12)`, `MSG_IMG_END (0x13)`, `MSG_IMG_ACK (0x14)`, `MSG_IMG_NACK (0x15)`

**Two hard-won rules for anything that hands off from JSON to COBS on this link:**

1. **Never `println()` the handshake.** It emits `"\r\n"`; the peer's line parser terminates on the `\r` and stops reading the instant it enters binary mode, leaving the `\n` in the FIFO to be consumed as the first COBS byte. One stray byte shifts the entire decode. Reproduced exactly on the host (`tools/cobs_probe.c`): a valid 519-byte frame parses as `decoded_len=520, data_len=59649, expected=59656` — byte-for-byte what the device reported. It is fully deterministic, so it reads like a framing bug and survives unrelated "fixes". Send a bare `\n`, and drain until quiet on the receiving side.
2. **Suppress JSON TX for the duration of the transfer.** The LCD's TX-queue drain runs before its binary-mode branch, so status lines were being emitted into the middle of the COBS stream; the Sense saw `data_len=8818` (`0x2272` = `"r`). Messages queue and drain after the transfer instead.

### Rule: harden BOTH directions, or neither works

The link carries binary in both directions now (firmware LCD-ward, capture images both ways). Every hazard fixed in one direction has an exact twin in the other, and fixing only one produces failures that look unrelated. All three of these were found *after* the outbound direction was working:

**The rule, stated once so it need not be rediscovered a fourth time: during any
binary transfer, BOTH boards suppress JSON, regardless of which one is sending.**
Three separate debugging cycles were spent fixing whichever side had just been
observed to break. The flags are `g_img_rx_binary_mode` / `g_lcd_ota_binary_mode`
/ `g_spool_tx_pending` / `g_spool_tx_active` on the LCD, and
`g_lcd_ota_proxy_owns_uart` / `g_spool_owns_uart` / `g_img_spool_tx_active` on
the Sense.

The third instance hid the longest because the synthetic `spooltest` payload
passed byte-exact twice (184,320 B, both directions) — but `spooltest` runs on an
**idle** device. The real path runs during an upload failure with WiFi up, where
the Sense emits `SENSE_DIAG` rssi every ~2s, and those bytes land inside its own
outbound COBS stream: `[IMG_SPOOL] ack timeout at seq=5`. A test that cannot
produce the interference cannot catch it.

| Hazard | Outbound fix | Inbound twin (initially missed) |
|---|---|---|
| RX buffer too small for a ~521B frame | LCD `setRxBufferSize(1024)` | Sense had **none** (256B default) → `decoded_len=391` with a *correct* header |
| Own JSON interleaved into own COBS stream | Suppress TX while `g_img_rx_binary_mode` | `binary_xfer_active` covered only RECEIVE states, so the LCD corrupted the stream it was TRANSMITTING |
| Peer's periodic chatter corrupts frame reads | — | Sense `SENSE_DIAG` rssi (~2s) landed inside the LCD's ACK reads |

**Diagnostic signature:** a *correct* header (`data_len=512`) with a short `decoded_len` means bytes were dropped — look at buffer sizes. A *garbage* `data_len` whose hex spells ASCII (`8818` = `0x2272` = `"r`) means JSON leaked into the stream — look at TX suppression on both boards.

### COBS parser note

`recv_frame()` treats `*data_len` as **IN/OUT** — the caller must pass the buffer capacity in, or every chunk is rejected as "payload too large".

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
8. **FreeRTOS queues** -- Create op_queue (20 slots), upload_queue (10)
9. **Voice buffer** -- Allocate 512KB in PSRAM for audio recording
10. **I2S audio init** -- Configure microphone with callback
11. **Worker tasks:**
    - `op_worker` -- Core 1, priority 2, 16KB stack (foreground operations)
    - `upload_worker` -- Core 1, priority 1, 12KB stack (background uploads)
12. **WiFi connect** -- Start connection using provisioned or default credentials
13. **`halo_prod_setup()`** -- OTA schedule check, provisioning state machine

### LCD Board Boot Sequence

**Timer visibility (2026-09-04):** deep sleep restarts `setup()`. TIMER/effective
timer boots initialize the UI and inputs with PWM zero and panel DISPLAY_OFF,
and keep a background-dark latch until a real touch/encoder event or OTA takes
over. Cold/touch boots remain visible. The latch also gates later Home/status
relight paths; keeping the panel dark does not stop UART or input polling. OTA
and post-OTA continuation explicitly light the display. Short failed-handshake
fallback retries are retained in validated RTC no-init memory across timer
boots, capped at three, then use the maintenance/periodic timer. An actual
non-fallback sleep commit or real user input resets the retry episode. See
`LCD_FIRMWARE.md` for diagnostics and required unplugged validation.

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

**Sleep entry is not instantaneous — measured `teardown_ms=166` on the LCD** between
`transition_begin` and `esp_deep_sleep_start()` (NVS list save, UI teardown, panel off, errlog
write). A tap landing inside that window used to be lost outright: the UI task has already stopped
treating touches as input and ext1 is not armed until the final instruction, so nobody saw it and
the user tapped again. A *held* touch was never affected — the INT stays asserted, so ext1
`ANY_LOW` fires as soon as sleep begins.

Closed 2026-08-20 with an edge-triggered ISR on the touch INT, armed at `transition_begin` and
checked at the last point sleep can be abandoned cleanly (above `Touch_Standby()`, backlight-off
and `vTaskDelete(ui_task_handle)` — `abort_sleep_transition()` restores panel/backlight/LVGL but
cannot recreate a deleted UI task). Verified both directions on hardware, including 3/3 undisturbed
cycles sleeping normally with `isr_count=0`, because a spurious abort would mean the device never
sleeps — worse than the bug being fixed. See LCD_FIRMWARE.md for the test method.

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
   - (no result poll -- nutrition was removed 2026-08-21)
   - Timeout: 60s

8. Display Result (LCD)
   - LCD shows "Logged!" and returns home, same as check-in and discard
```

### DMA Guard Pattern

Camera DMA and TLS both need large contiguous blocks of **internal, DMA-capable** RAM — not PSRAM. That pool is the scarcest resource on the Sense and is where most capture/upload faults originate:

```cpp
// Reserved up front so WiFi/TLS fragmentation cannot eat the contiguous region
g_camera_dma_reserve = (uint8_t*)heap_caps_malloc(
    CAMERA_DMA_RESERVE_BYTES /*16384*/, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

// Released immediately before esp_camera_init(), re-acquired in deinit_camera()
```

**Do not pre-emptively tear WiFi down before camera init.** The old guard compared `heap_caps_get_largest_free_block()` against a 24576 threshold *while the 16KB reserve was still held*, so it understated the available block by the reserve size and fired on **every capture**:

```
[CAMERA] DMA low (18420 < 24576) — killing WiFi     x11 in an 11-capture run
```

Killing the connection on every capture is what starves uploads. A strong AP hides it; a weak one never reconnects before the next capture kills it again, producing "uploads missed or very late". A measured run had **zero uploads complete across 11 successful captures** for this reason. It is also redundant — `init_camera()` already makes two attempts with `quiesce_network_for_camera("retry_after_init_fail")` between them, so a real shortage costs one failed attempt instead of one lost connection. Restore the old behaviour with `CAMERA_PREEMPTIVE_WIFI_KILL=1` only with measurements in hand.

**Never tear WiFi down with an HTTP request inflight.** Doing so frees lwIP pbufs while the upload task is blocked in `recv()` and panics the board (`lwip_recvfrom -> lwip_recv_tcp -> pbuf_free`), measured at 2 panics / 8 captures. `init_camera()` now drains for up to `CAMERA_HTTP_DRAIN_MAX_MS` (1500) and **skips** the teardown entirely if the request has not finished. Losing DMA headroom for one capture beats a panic.

**Anything holding internal RAM across a camera re-init must release it first.** The bounded-grab worker's 4KB stack (5,120 B with TCB) took `dma_largest` from 19444 to 14324 — below the ~16KB `esp_camera_init()` needs — and caused 15/30 captures to fail. It is now released in `deinit_camera()` before the re-reservation, with a short yield because `vTaskDelete` defers freeing the stack to the IDLE task.

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
upload_queue       (10 slots, lower priority) -- Check-in, discard, voice uploads
```

The `upload_worker` FreeRTOS task (Core 1, priority 1) drains both queues. Dish queue is checked first for responsiveness.

**TLS:** every leg validates the peer. The S3 photo PUT was the exception — it used
`setInsecure()` while presign, MQTT and OTA all validated — and was switched to the shared
`tls_configure()` helper on 2026-08-20. Measured cost on hardware: `heap delta +212 B`,
`dma_largest 17396`, `PUT status: 200`. Heap is now logged either side of that handshake on every
PUT, because this path has a heap-exhaustion panic history (the AES-DMA fault fixed in 6.1.815) and
a regression there should be visible rather than inferred.

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
4. **Download**: Sense makes one HTTPS GET per session. A disconnect or 30s without data ends the attempt; HTTP and TLS are explicitly closed. There is no in-session Range reconnect that can outlive the LCD's matching 30s idle timeout. The overall 40-minute loop check does not interrupt blocking DNS/TLS/HTTP calls.
5. **UART switch**: `g_lcd_ota_proxy_owns_uart = true` (blocks JSON RX)
6. **Stream**: COBS-framed binary chunks (512B each) with CRC16 and ACK/NACK
7. **Progress**: NVS progress is saved every 64KB, but SHA context cannot be restored; a new session starts from byte zero.
8. **Verify**: LCD computes SHA256 over entire received binary
9. **Apply**: LCD calls `esp_ota_set_boot_partition()` and reboots

**Single net stack contention:** Sense has one network stack shared by OTA HTTPS and normal app traffic. While any OTA is in flight (`lcd_ota_in_progress()`), non-OTA HTTP — specifically `request_list_refresh()` / the `INPUT_WAKE`-triggered `/v1/list` GET — is suppressed so it cannot starve the S3 download. OTA's own HTTP (manifest, download, schedule/report) is never gated.

**Retry ownership:** after an abort or failed END, the LCD keeps its atomic sleep guards through cleanup and grants the existing 10s Home idle interval before sleeping. The Sense's second attempt reacquires `OTA_LOCK` and queries the LCD afresh before BEGIN in both paired-update and LCD-only inline paths. If the LCD is unavailable, Sense retains `lcd_ota_due`, unlocks, and defers its own update. Duplicate/late LCD abort commands cannot clear an idle receiver's new lock or a different active session. The guardian limit and GPIO wake contract are unchanged.

**Final check completion:** proven terminal production exits send `OTA_UNLOCK` with optional `terminal:true`, releasing the LCD's generic and recovery stay-awake timers. A both-current check therefore returns to normal Home idle sleep instead of keeping the panel lit for the 180s update lease. Unmarked unlocks preserve the continuation hold, including the intermediate unlock before the LCD-only query/transfer. Old receivers safely ignore the optional field.

**Cloud `lcd_fw` is the REAL running version:** the cloud-reported LCD firmware version (`truth_get_lcd_fw_version()`) is sourced exclusively from an actual `LCD_OTA_QUERY_RESP`, never from the OTA manifest. On proxy success the cached value is cleared and re-queried (pre-sleep / periodic, refreshed when empty or >5min stale), so a transfer that completes but never boots the new image no longer masquerades as success in the dashboard.

### OTA Orchestration Order (LCD proxy FIRST, then Sense self-OTA)

When a manual/button-triggered OTA check (`maybeRunOtaCheck()` in `halo_sense_prod.ino`) finds a Sense update to apply, the dual-board update is ordered **LCD proxy first, Sense self-OTA second**:

1. **LCD proxy (inline, while the LCD is awake from the button press):** Before applying the Sense image, Sense runs the LCD proxy inline on the main task — `sense_lcd_ota_query()` → `sense_lcd_ota_fetch_manifest(cfg->base_dir, cfg->channel, …)` → version compare → `OTA_LOCK` + `sense_lcd_ota_proxy()`. This blocks the main loop (so there is no UART-drain race), and `sense_lcd_ota_proxy()` manages `g_lcd_ota_proxy_owns_uart` itself during COBS streaming.
2. **Sense self-OTA + reboot:** Only after the LCD proxy completes does Sense call `applyToOtaPartition()` and reboot.

**Why:** Previously the Sense applied its own image and **rebooted FIRST**, deferring the LCD proxy to the next boot via the `lcd_ota_due` NVS flag. By the time the rebooted Sense queried the LCD, the LCD had often gone back to sleep → intermittent `lcd_query_fail`. Doing the LCD proxy first, while the LCD is still awake, eliminates that race.

**Persistent LCD recovery:** `lcd_ota_due` is cleared after a successful LCD transfer or a query proving it is already current, and retained on an unknown/failed LCD result. The strict guard then defers Sense self-OTA. On a later boot, the debt queues a bounded recovery check through `nightly_maintenance_tick()`; normal health, provisioning and user service continue while readiness is checked. If a retry query finds the target or a newer LCD image, no new BEGIN is sent: reporting preserves `noop` and the actual queried version rather than claiming a transfer to the manifest version.

**Scheduled maintenance uses the same production path:** a real TIMER boot queues a check with a 120s readiness deadline. Once connectivity, time, cooldown and foreground-work guards permit, `nightly_maintenance_tick()` invokes `maybeRunOtaCheck(reason, true)`. The shared path resolves the LCD first, then permits Sense self-OTA. Failed LCD coordination retains the debt and returns to normal service; it does not strand a new Sense image ahead of an unresolved LCD. Sense cannot wake LCD through GPIO39, which is LCD→Sense only, so timer co-wake and a fresh LCD query remain required.

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
- **Report**: POST to `/ota/report` with result status. All device→cloud reports are built by a single Sense helper (`ota_report_post(report_type, extras, timeout_ms)` in `halo_sense_prod.ino`; `ota_report_post_pre_sleep()` is now a thin wrapper). The Lambda (`aws/src/report/index.py`) stores `report_type` verbatim (no whitelist). Report types:
  - `pre_sleep` — the historical rich device-state snapshot emitted before deep sleep.
  - `window_armed` — emitted once per newly-armed `request_id` from the awake-path schedule fetch in `halo_prod_loop()`, so the cloud can see the device picked up a maintenance window (carries `request_id`, `maint_start_epoch`, `fw`, `lcd_fw`). De-duped on `request_id`.
  - `ota_complete` — emitted once per scheduled-maintenance run in `run_maintenance_if_needed()`, AFTER the LCD proxy phase and BEFORE the Sense self-OTA can `esp_restart()`. Captures the Sense-ahead/LCD-behind asymmetry: `fw` (current Sense, pre-self-update), `lcd_fw`, `lcd_ota_result` (e.g. `end_ack_timeout`/`updated`/`noop`), plus extras `sense_fw_target` and `lcd_ota_succeeded`. Without it, a Sense self-OTA reboot would leave the maintenance outcome invisible in the cloud.

The schedule is fetched on **two** paths: the original pre-sleep fetch (in `halo_prod_pre_sleep()`), and — to survive sustained activity — a **throttled awake-path fetch in `halo_prod_loop()`**. Because the schedule GET is **future-only** (it returns 204 once `start_epoch` has passed), a device kept continuously active never reached a clean idle-sleep and so never captured a freshly-posted window before its start passed → no NVS window → the sleep-entry timer-arm had nothing to arm → it slept through the window. The awake-path fetch refreshes the NVS window **while the window is still future** (gated: only after ~20s sustained awake, throttled to ~30s between attempts, re-fetching when the last fetch is >90s old, and only when WiFi+time are valid and no capture/LCD-OTA/maintenance is in flight). The sleep-entry arm (`sense_config_deep_sleep_wakeup`) is unchanged and still does the actual arming once the window is in NVS. See `docs/SENSE_FIRMWARE.md` → "Awake-Path Schedule Fetch".

**Provisioning gate (`!halo_provisioning_active()`):** the awake-path fetch (and its `window_armed` report) is also gated **off during provisioning/setup**. During provisioning the SoftAP is up (AP+STA fragments internal RAM) and the 16KB camera DMA reserve is held, so the schedule GET's TLS handshake would starve `esp_aes_process_dma` → `abort()` → `ESP_RST_PANIC`, rebooting back to `ap_setup` before the device marks itself provisioned or claims the owner (reproduced on fw 6.1.814 — same esp-aes DMA hazard commit 809 fixed for owner-claim TLS). As defense in depth, both outbound-TLS helpers also wrap their HTTPS handshake in a shared `ScopedTlsDmaReserve` RAII guard (frees the camera DMA reserve via `halo_tls_free_dma_reserve()` for the handshake, restores on every exit): `ota_sched_http_fetch_window()` around its GET and `ota_report_post()` around its POST (the latter protects `ota_complete`, which fires during a maintenance window with the reserve held). This mirrors commit 809's `DmaReserveTlsGuard` but on the Sense-local TLS paths (a different call path from the owner-claim guard in `ProvisioningManager.cpp` — no nesting).

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

#### Arm-time delivery race fix — keep the LCD awake until the window lands (`MAINT_KEEPALIVE`)

The `now_epoch` self-wake above only works if the LCD actually **receives** the
`MAINT_WINDOW` (with `now_epoch`) before it idle-sleeps. After a tap wakes both boards
the LCD idle-sleeps at ~10s, but the Sense needs ~10–15s for WiFi+NTP+schedule-fetch
before it can send the real window. Critically, the pending-sync flag is set **only AFTER**
the HTTPS `/ota/schedule` fetch (which already needs valid time), so by then the clock is
valid AND the LCD has already idle-slept during the fetch — a "pending && !time_valid"
trigger never fires on a fresh arm. The Sense **cannot wake a sleeping LCD** (GPIO39 is
LCD→Sense only), so the absolute self-wake never armed and the LCD missed the window.

Fix (three parts, arm-time only — does NOT touch the at-window proxy / self-OTA / `esp_restart`):
- **Sense (`keep_lcd_awake_during_maint_arm()`, run every awake loop just before
  `sync_pending_maintenance_to_lcd("awake")`):** keep the LCD awake through the **whole
  post-wake connect/fetch phase**, before pending is even set. Sends `MAINT_KEEPALIVE`
  (`send_maint_keepalive()`) on a `MAINT_KEEPALIVE_RESEND_MS = 2000` cadence while early in the
  wake (`millis() − last_wake_ms < KEEPALIVE_ARM_WINDOW_MS` = 25s; `last_wake_ms` is reset in
  `setup()` each wake) AND (`!g_ota_check_done` — spans the fetch — OR `pending && !acked` —
  spans delivery). Carries only `request_id`. **Not** gated on `halo_uart_link_recent()` (that
  tracks LCD→Sense traffic, which goes stale seconds after the tap even while the LCD is awake,
  cutting keepalives too early); sending to an already-asleep LCD is a harmless no-op, and the
  25s window + gates bound it. The `deferred_time_invalid` branch is now log-only.
- **LCD (`lcd_uart_rx.h`):** a `MAINT_KEEPALIVE` handler calls `resetActivityTimer()` and
  nudges `ota_stay_awake_until_ms` (+8s) — it does **not** touch maintenance state. The
  future-window arm branch (`wake_in_s>0`) of the `MAINT_WINDOW` handler now also calls
  `resetActivityTimer()` + nudges stay-awake so repeated arm-syncs hold the LCD awake.
  (Since the Sense is awake during arm-time, `ota_stay_awake_until_ms` is honored.)
- **Breadcrumbs (LCD error-log black box, area `maint`):** `MW_RX` (value=`lcd_time_valid()`,
  detail=request_id) on window receipt; `SLEEP` (value=`sleep_timer_sec`, detail=`timer_reason`,
  emitted only when a maintenance timer is armed) at sleep; `RESTORE` (value=`armed`,
  detail=`clk=<0/1> rid=<...>`) on post-deep-sleep NVS restore. These make the next on-site
  scheduled-OTA test self-diagnosing.

Key constants:
- `MAINT_SYNC_RESEND_MS = 1500` -- Resend MAINT_WINDOW if no ACK
- `MAINT_KEEPALIVE_RESEND_MS = 2000` -- Arm-time keep-awake cadence during post-wake connect/fetch
- `KEEPALIVE_ARM_WINDOW_MS = 25000` -- Bounds the arm-time keep-awake to early in a wake
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

**The flag is not the only encoding of "awake", and they had drifted (fixed 2026-08-20).**
There is also a `sense_state` enum (`SENSE_AWAKE` / `SENSE_ASLEEP` / `SENSE_UNKNOWN`), and two
consumers act on *it* rather than the flag: the wake-probe decision (`need_probe` in
`LCD_Minimal.ino`) and scroll handling (`lcd_ui_task.h`). Of the three setters above, only `PONG`
went through `set_sense_awake_estimate()` → `sense_state_set()`; **`SYNC_ACK` and `UI_STATUS` set
the flag directly and never touched the enum**. `sense_state_set()` compounded it by early-returning
on an unchanged state while clearing the flag only *after* that return, so the common case (already
`ASLEEP`) never reconciled the two. The device therefore behaved differently — redundant probes,
altered scroll handling — while every log looked healthy.

Both sources are closed: all three setters now go through `set_sense_awake_estimate()`, and the
invariant (`not awake` ⇒ flag false) is enforced *before* the early return. A standing
`[STATE_DISAGREE]` line in the 5s `[SENSE_LINK]` diagnostic reports any future violation from the
field rather than leaving it to be inferred from behaviour months later.

Note this is a different fix from `SENSE_AWAKE_TRUST_MS` (2026-06-17), which declines to *trust* a
stale flag. That treats the symptom; this closes the source. Both are in place.

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

**Production slot guard (2026-09-04):** `publish_lcd_ota.py` now reads
`firmware/halo_lcd_prod/partitions.csv`, requires both OTA app slots, and checks
the binary against the smaller slot. The current limit is **2,621,440 bytes**;
the same value goes into `max_slot_bytes` in the manifest. Missing, malformed,
or incomplete tables fail before upload. The previous publisher used the legacy
LCD demo table and incorrectly advertised a 6 MiB slot. Offline boundary and
manifest checks: `python3 -m unittest discover -s halo_ota_demo/tools/ota -p
'test_publish_lcd_ota.py' -v` (run from the firmware source root).

For a reviewed pair, generate and sync each board's version header immediately
before its build, finish both production-wrapper builds and artifact checks,
then publish the explicit prebuilt LCD binary first and Sense second. Both
publishers accept `--bin`; each uploads its artifact and versioned manifest
before switching its own latest manifest, then downloads and verifies them.
There is no atomic two-board manifest switch. This order avoids advertising a
new Sense release before its LCD target exists. Use `--channel prod --bucket
halo-ota-prod --region us-east-1 --profile trepo-dev` for the current production
target. Any `HALO_MAINT_TEST_S` accelerated timer build stays local and must not
be supplied to either publisher. The wrapper below remains the legacy combined
compile/publish sequence; it publishes Sense before building LCD.

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

Sense uses `halo/ota/{channel}/manifest_latest.json`; LCD uses
`halo/ota/{channel}/lcd/manifest_latest.json`.

For device readback without disturbing Sense USB wake behavior, the LCD logs
the optional `FW_INFO` fields `sense_build`, `sense_running_part`,
`sense_running_state`, `sense_boot_part`, `sense_wake_cause`, and
`sense_reset_reason`. The first four identify the running/selected image and
validation state; the last two report raw ESP-IDF values from that boot. Sense
also persists one `nightly_begin` wake/reset/epoch breadcrumb through the LCD
diagnostic ring when timer maintenance starts.

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
| `lcd_ship_action.h` | Menu actions, custom UI |
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

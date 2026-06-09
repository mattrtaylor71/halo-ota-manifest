# HALO LCD Board Firmware Documentation

## Hardware Overview

- **MCU:** ESP32-S3 with 8MB flash, OPI PSRAM
- **Display:** 360x360 round TFT (I80 bus), driven via LVGL v8
- **Touch:** CST816 capacitive touch controller (I2C), interrupt on GPIO9 (active LOW)
- **Encoder:** Rotary encoder on GPIO8 (EC1_A) and GPIO7 (EC1_B)
- **UART to Sense:** TX=GPIO38, RX=GPIO48, 115200 baud (UART1)
- **Wake line:** GPIO39 (INT_PIN) -- LCD drives LOW to wake Sense board (Sense EXT0 on GPIO2)
- **Backlight:** PWM-controlled. "On" maps to a user-adjustable level `g_user_brightness_duty` (0..255, 5% floor); off is 0. Set via the Settings → Backlight screen (knob to adjust, tap to save), persisted in NVS (`lcd_ui`/`brightness`), restored at boot. `lcd_set_backlight_level()` uses `(level>0) ? g_user_brightness_duty : 0`. `backlight_apply_pct(pct)`/`backlight_get_pct()` in lcd_anim.h apply/read it; `backlight_save_to_nvs()`/`backlight_load_pct_from_nvs()` in lcd_persist.h persist it.

## Architecture Overview

The firmware runs two FreeRTOS tasks plus the Arduino `loop()`:

| Task | Core | Priority | Role |
|------|------|----------|------|
| `ui_task` | 1 | 3 | Owns LVGL lock, processes `app_event_queue`, renders all screens |
| `uart_task` | 0 | 2 | TX queue drain, UART RX parsing, USB serial commands, OTA binary receive |
| `loop()` | 1 | 1 | Activity timer, sleep decision, sense link management, maintenance polling |

**CRITICAL RULE: Never call LVGL from Core 0 (uart_task).** All display updates must be posted as events to `app_event_queue` and processed by the UI task on Core 1. Violating this causes LVGL state corruption, display hangs, and ROM download mode entries.

## Module Reference

### 1. LCD_Minimal.ino (Main File, ~4800 lines)

**Purpose:** Entry point containing `setup()`, `loop()`, all global state declarations, constants, forward declarations, and the header include chain.

#### Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `PROTOCOL_VERSION` | 1 | JSON protocol version for UART messages |
| `MAX_LINE_LENGTH` | 4096 | Max UART RX line buffer (supports rich voice payloads) |
| `UART_BAUD_RATE` | 115200 | Sense UART baud rate |
| `INT_PIN` | 39 | LCD -> Sense wake line GPIO |
| `LCD_WAKE_GPIO` | GPIO9 | Touch INT pin, EXT0 wake source |
| `LCD_WAKE_LEVEL` | LOW | Active level for touch wake |
| `LCD_OTA_WAKE_INTERVAL_SEC` | 21600 (6h) | Periodic timer wake for OTA checks |
| `LCD_OTA_SCHED_START_MIN` | 120 | OTA schedule window start (2:00 AM) |
| `INACTIVITY_TIMEOUT_MS` | 10000 | Sleep after 10s inactivity |
| `GUARDIAN_FORCE_SLEEP_MS` | 300000 | Hard 5-minute awake cap |
| `MAX_LIST_ITEMS` | 50 | Shopping list capacity |

#### Global State Structures

```c
struct app_state_t {
  char items[50][64];      // Item text
  char item_ids[50][64];   // Backend IDs
  int count;               // Number of items
  int selected_index;      // Currently selected item
};
```

**Double-buffered state:** `g_active` (UI reads) and `g_pending` (UART writes). Protected by `app_state_mutex`. The UI task swaps pending->active on `EVT_LIST_REPLACED`.

#### Enumerations

- **`ui_screen_t`**: SCREEN_HOME, SCREEN_SECOND, SCREEN_SETTINGS, SCREEN_BACKLIGHT, SCREEN_AI_LISTENING, SCREEN_VOICE_JSON, SCREEN_HOLD_STILL, SCREEN_VOICE_ACK, SCREEN_PROCESSING, SCREEN_LOGGED, SCREEN_EXPIRY_CHOICE, SCREEN_EXPIRY, SCREEN_RESULT, SCREEN_DEBUG, SCREEN_ERRLOG, SCREEN_ERRLOG_DETAIL, SCREEN_SHOPPING_LIST (plus `ScreenId` SCREEN_SHIP_BACKLIGHT in ui_screen_registry.h)
- **`ship_menu_action_t`**: CHECK_IN, CHECK_OUT, LOG_DISH, MORE, AI, HOME, SETTINGS, DEBUG, RESET_WIFI, MANUAL_OTA, BACKLIGHT, DEBUG_LOG, SHOPPING_LIST, BACK
- **`ship_user_state_t`**: ASLEEP, MENU_READY, CAPTURE_COMMITTED, USER_WAITING_RESULT, USER_INPUT_REQUIRED, USER_FEEDBACK
- **`lcd_mode_t`**: LCD_MODE_UI_ACTIVE, LCD_MODE_MAINTENANCE
- **`SenseState`**: SENSE_UNKNOWN, SENSE_AWAKE, SENSE_ASLEEP
- **`RefreshState`**: REFRESH_IDLE, REFRESH_WAKE_PENDING, REFRESH_INFLIGHT, REFRESH_COMPLETE, REFRESH_FAILED
  - On the shopping list, a hard 12s cap (`REFRESH_HARD_TIMEOUT_MS`) on REFRESH_INFLIGHT calls `refresh_hard_timeout_clear()` — clears all inflight flags with **no auto-retry** (unlike `refresh_soft_fail`), posts `EVT_REFRESH_TIMEOUT` so the UI task stops the glow, re-renders the cached list, and shows a brief "Couldn't refresh" title. Lets the 10s idle-sleep engage instead of spinning forever.
- **Shopping-list keep-awake (`LIST_ACTIVE`)**: `uart_send_list_active(bool)` in lcd_uart.h tells the Sense to stay awake + keep WiFi up. Sent `true` on entering the list and re-asserted ~every 3s from `loop()`; sent `false` on leaving (both via `ui_show_screen` enter/leave detection) and on LCD idle-sleep entry (`enterLightSleep`) if still on the list.

#### Sleep/Wake State Variables (RTC_DATA_ATTR -- survive deep sleep)

- `g_lcd_maintenance_timer_armed` -- maintenance timer armed flag
- `g_lcd_maintenance_wake_in_s` -- seconds until next maintenance wake
- `g_lcd_maintenance_remaining_s` -- seconds remaining in maintenance window
- `g_lcd_maintenance_request_id[64]` -- cloud request ID
- `g_lcd_maintenance_start_epoch` -- window start time (unix epoch)
- `g_lcd_schedule_timer_armed`, `g_lcd_schedule_wake_in_s`, `g_lcd_schedule_next_epoch` -- schedule timer state

#### Wake Pin Management

The LCD uses GPIO39 (INT_PIN) to wake the Sense board. Pin mode tracking (`lcd_wake_pin_mode`, `lcd_wake_pin_pullup`) prevents toggling that Sense misinterprets as "lcd_pulsing."

- `request_sense_wake(reason)` -- Drives INT_PIN LOW for `WAKE_PULSE_DURATION_MS` (80ms), then releases
- `release_wake_line(reason)` -- Sets INT_PIN HIGH, then INPUT_PULLUP (unless sleeping)
- `sleep_prepare_wake_line_for_request()` -- Ensures wake line is HIGH before sending INPUT_SLEEP; if stuck LOW, enters deny-wait

#### Test Mode

- `g_test_mode_active` -- disables all sleep for automated testing
- `g_test_mode_expire_ms` -- auto-expires after 1 hour
- Persisted in NVS namespace `test_cfg` key `test_mode`
- Activated via USB `testmode` command, cleared via `testmodeoff`

#### setup() Flow

1. Print wakeup diagnostics (reset reason, wake cause)
2. Determine wake cause label (`s_wake_cause_label`)
3. Restore maintenance state from NVS if timer wake
4. Initialize UART to Sense board
5. Create FreeRTOS queues (`app_event_queue`, `uart_tx_queue`)
6. Create `app_state_mutex`
7. Load NVS test mode flag
8. Handle boot wake sequence (maintenance resume, schedule wake, ship OTA)
9. Initialize UI stack (`init_ui_stack`)
10. Start UART task on Core 0
11. Run OTA self-test
12. Log boot count, GPIO states

#### loop() Flow (runs every iteration on Core 1)

1. Log LCD wake pin tick (1s interval)
2. Service deferred WiFi ON
3. Check stale maintenance flags (safety override after 10 min)
4. Handle provisioning QR exit from headless
5. Service firmware info retry
6. Process maintenance deadlines
7. Handle sleep deny retry timers
8. Service Sense sleep intent
9. Process OTA check requests
10. Guard against LVGL screen state mismatches
11. Camera countdown hold-still timer
12. Sleep eligibility check -> `enterLightSleep()` or `enter_maintenance_sleep()`
13. Guardian force-sleep (5-minute hard cap)

---

### 2. lcd_uart.h -- UART TX Protocol Layer

**Purpose:** UART initialization, protocol validation, message sending, and TX queue management.

#### Key Functions

| Function | Description |
|----------|-------------|
| `init_uart()` | Configures UART1 with 1024-byte RX buffer at 115200 baud |
| `get_next_msg_id()` | Returns incrementing message ID (wraps at 0 -> 1) |
| `validate_protocol_message(doc)` | Checks `ver`, `type`, `msg_id`, `ts` fields |
| `uart_send_json(json_str)` | Base send: prints JSON + newline, flushes. Suppressed when `g_suppress_uart_json_tx` is true (during OTA binary mode) |
| `uart_send_input_message(type, delta, id)` | Builds protocol-compliant JSON message with optional delta/id fields. Suppresses INPUT_SCROLL and INPUT_PING during OTA lock |
| `uart_send_ack(type)` | Sends simple ACK message |
| `uart_send_maint_window_ack(...)` | Sends MAINT_WINDOW_ACK with full window context |
| `uart_send_wifi_on_ack(status, reason)` | Acknowledges WIFI_ON request |
| `uart_send_wifi_creds_ack(status, err_code)` | Acknowledges WIFI_CREDS storage |
| `tx_msg_send_now(tx_msg)` | Sends queued TX message immediately |

#### TX Queue

Messages are queued via `uart_tx_queue` (FreeRTOS queue of `tx_msg_t`) from ISR context (knob callbacks) or UI task. The `uart_task` drains the queue.

**Deferred TX:** Messages requiring awake proof (INPUT_MENU_SELECT, INPUT_FW_INFO) are held in `deferred_awake_tx_msg` until `sense_ready_for_control_tx()` returns true. The deferred service pings Sense periodically to wake it.

#### Important Flag

`g_suppress_uart_json_tx` -- Set true during LCD OTA binary mode to prevent JSON from corrupting COBS-framed binary data on the shared UART.

---

### 3. lcd_anim.h -- Animation, Display, and Wake Management

**Purpose:** Processing animation, LVGL tick wrapper, backlight control, display power management, OTA mode entry/exit, and wake pin utilities.

#### Key Functions

| Function | Description |
|----------|-------------|
| `ui_lvgl_tick()` | Guarded `lv_timer_handler()` call; skips if `!g_lvgl_running` or `g_sleep_transition` |
| `lcd_set_backlight_binary(on, reason)` | PWM backlight on (`g_user_brightness_duty`) or off (0) |
| `lcd_set_backlight_level(level, reason)` | "On" target = `g_user_brightness_duty`; off = 0 |
| `backlight_apply_pct(pct)` / `backlight_get_pct()` | Set/read user brightness (5..100%); applies live only when backlight already initialized (boot just seeds the global) |
| `lcd_set_idle_screen_dark(dark, reason)` | Powers off panel + backlight but keeps firmware running. Used during sleep wait and maintenance headless |
| `ensure_awake_for_ui(reason)` | Restores display from any low-power state (idle dark, sleep transition, panel off). Exits headless OTA if user touches |
| `abort_sleep_transition(reason)` | Recovers from aborted sleep -- restores panel, backlight, LVGL, resets activity timer |
| `lcd_enter_ota_mode(min_internal_free)` | Kills UI task, deinits LVGL, powers off panel to free RAM for OTA. Returns true if enough internal heap available |
| `lcd_exit_ota_mode(reason)` | Restores full UI stack after OTA. Checks DMA memory for reduced LVGL buffers if fragmented |
| `lcd_enter_maintenance_headless(reason)` | Enters OTA mode + turns off backlight for unattended maintenance |
| `release_wake_line(reason)` | Sets INT_PIN HIGH, then INPUT_PULLUP (unless in sleep transition) |
| `status_screen_use_text(text)` | Sets status screen to show plain text |
| `show_auto_hiding_status_message(text, duration_ms)` | Shows a toast that auto-hides after `duration_ms` |
| `wifi_creds_checksum(ssid, pass)` | FNV-1a hash for WiFi credential dedup |
| `send_wifi_status()` | Sends WIFI_STATUS with has_creds and checksum to Sense |

#### Display States

```
awake -> idle_dark -> sleep_transition -> deep_sleep
                  \-> ota_mode (LVGL deinit, panel off, UI task killed)
```

---

### 4. lcd_provision.h -- WiFi Provisioning UI

**Purpose:** QR code screen for WiFi setup, provisioning intro screen, status label updates.

#### Key Functions

| Function | Description |
|----------|-------------|
| `show_provision_intro_screen(reason)` | Shows "Wi-Fi Setup / Tap to Start" screen |
| `show_provisioning_screen(ssid, password, url)` | Shows QR code for WiFi credentials. Uses `QRDisplay::generateWifiQRData()` |
| `hide_provisioning_screen()` | Hides QR screen, restores list if available |
| `update_provision_status_label(state)` | Updates label: "Setup Mode", "Connecting...", "Failed - try again" |
| `provision_cache_qr(ssid, password, url)` | Caches QR data so it can be shown after intro tap |
| `provision_qr_wait_begin/clear(reason)` | Tracks waiting-for-QR state |

#### Provisioning Flow

1. Sense sends PROVISION_STATUS with state="unprovisioned" or "ap_setup"
2. LCD shows intro screen (WiFi icon + "Tap to Start")
3. User taps -> `provision_intro_tapped = true`
4. Sense sends PROVISION_QR with ssid/password/url
5. LCD shows QR code
6. User scans QR, connects, enters credentials
7. Sense sends PROVISION_STATUS state="connected"
8. LCD hides QR, shows main menu

---

### 5. lcd_ship_screens.h -- Screen Builders (~1500 lines)

**Purpose:** Creates all LVGL screen objects: main menu, second menu, settings, AI listening, voice response viewer, shopping list, expiry date picker, and the legacy list/menu/status overlays.

#### UI Design System (redesigned 2026-06-05)

The ship UI uses a consistent design language across menus and screens:
- **Background:** cream `0xFDF2DE` (fills the round 360×360 panel)
- **Buttons:** white `0xFFFFFF` rounded rects, 2px dark `0x1A1A1A` border, hard drop shadow (shadow_width 8, ofs 4–5/6–7, opa ~40–50)
- **Accents:** teal `0x296065` (primary/center), gold `0xF6BF41` (mic/icons on teal), dark `0x1A1A1A` text/icons
- Icon assets are RGB565+alpha LVGL images generated by `tools/lvgl_image_converter/` (byte-swapped for `LV_COLOR_16_SWAP=1`); simple glyphs (+, −, ⋯) are drawn natively as rounded `lv_obj` bars/dots (zero flash). See memory `reference_lvgl_image_converter`.

#### Main Menu (Home Screen)

Cream background, four white shadowed buttons in a cross layout + a teal center button. No text captions (icon-only):
- **Top:** Dish — `dish_icon.c` (plate + fork/knife image, dark), `ship_main_menu_add_dish_icon`
- **Left:** Check In — drawn **+** (`ship_main_menu_add_plus`)
- **Right:** Discard — drawn **−** (`ship_main_menu_add_minus`)
- **Bottom:** More — drawn **⋯** (`ship_main_menu_add_dots`)
- **Center:** AI/voice (hold-to-talk) — teal `0x296065` box (`ship_main_menu_ai_button`) holding the gold mic image `mic_icon.c` (`ship_main_menu_add_mic_icon`)

`ship_main_menu_ai_label` ("AI") is still created but hidden (`LV_OBJ_FLAG_HIDDEN`) so other code can reference it. `ship_main_menu_set_ai_hold_active()` gives press feedback by lightening the teal bg + turning the border gold (it no longer touches the drop shadow). Buttons styled via `ship_main_menu_style_button(btn, primary)`. The old `sparkles_ai.c` / `ai_bird.c` assets were removed. Buttons use hitbox-based touch detection (not LVGL events) for the round display.

#### Second Menu

Same design language. Three buttons: Shopping List (top, white), **Home** (center, **teal** with a gold back-arrow — mirrors the main-menu center accent), Settings (bottom, white).

#### Settings Screen

Cream `0xFDF2DE` background. Four white rounded buttons (220x54, X=70, radius 18, 2px dark border + hard drop shadow), in order: **Reset Wi-Fi** (Y=38), **Backlight** (Y=108), **Run OTA Update** (Y=178), **Back** (Y=248). Compact firmware-version line (`LCD x · Sense y`, montserrat_14) centered at the very bottom (Y=324); the hidden status-overlay label reuses that position for transient "Starting OTA..." text. Geometry constants `SHIP_MENU_SETTINGS_*` and the matching 4-entry `ship_menu_hitboxes_settings[]` (actions RESET_WIFI, BACKLIGHT, MANUAL_OTA, BACK) live in LCD_Minimal.ino. Buttons styled via `ship_settings_style_button()` in lcd_ship_screens.h.

#### Backlight Screen (`SCREEN_BACKLIGHT` / `SCREEN_SHIP_BACKLIGHT`)

Opened from Settings → Backlight (`SHIP_MENU_ACTION_BACKLIGHT` → `show_ship_backlight_screen()`). Cream bg, gold brightness `lv_arc` (190px, faint teal track + gold rounded indicator, range 0..1000, rotation 270) with a "Backlight" title above, a big centered "%" label (montserrat_28, `ship_backlight_pct_label`), and a "Scroll to adjust · Tap to save" hint below. The knob handler in lcd_ui_task.h adjusts in 5% steps (5..100), calls `backlight_apply_pct()` for live brightness, and updates the arc (`pct*10`) + % label on the UI task. Any tap calls `backlight_save_to_nvs()` then returns to Settings (brightness already applied live). Builder `show_ship_backlight_screen_impl()` in lcd_ship_screens.h seeds the arc/label from `backlight_get_pct()` on show.

#### Menu transition touch-ignore guard (bleed-through fix)

When a menu screen loads, the show function sets `touch_ignore_until = millis() + 280` (right after `home_shown_ms = millis();`). This guards against touch *bleed-through*: a single physical press during the slow screen transition can register a residual touch ~200ms after the new screen appears, landing on whatever button is now under the finger. Without the guard, tapping **Settings** on the second menu could auto-fire the **Run OTA Update** button (which loads under the finger at y≈156), triggering `[OTA_MANUAL] override=1 reason=manual_button` and the Software Update overlay. The 280ms window still covers the observed ~210ms bleed-through but is much snappier than the old 500ms (which made menu changes feel sluggish). Applied to the four menu/settings/backlight show functions (main menu, second menu, settings, backlight) in lcd_ship_screens.h. The touch handler enforces this via `if (millis() < touch_ignore_until)` (LCD_Minimal.ino:3733). NOTE: other touch guards are intentionally left longer — sleep wake (~300/450ms in lcd_sleep.h), the 2000ms UART-RX guard (lcd_uart_rx.h), and the loop()-level guard.

#### AI Listening Screen (redesigned 2026-06-05)

Active while the user holds the AI button (recording). `ship_ai_listening_countdown_start_ms` tracks recording duration; `ship_update_ai_listening_countdown()` drives the ring value. Design-system styling (`ship_init_ai_listening_screen`):
- Cream `0xFDF2DE` background
- **Gold countdown ring** (`ship_ai_listening_ring`, `lv_arc` 176px, width 11, rounded caps) — gold `0xF6BF41` indicator over a faint teal track; depletes as the recording counts down
- **Gold mic on a teal disc** (`ship_ai_listening_mic_disc` 128px teal circle; mic head/stem/base recolored gold, parented to the disc so they scale together) — mirrors the menu center button
- **Looping "sound-pulse" ripples** (`ship_ai_listening_pulse[3]`, teal ring outlines) expand outward, staggered (`ship_listen_pulse_cb`)
- **Breathing** — the disc gently zooms via `transform_zoom` (`ship_listen_zoom_cb`)
- "Listening" title + "Release to return" hint, with the staggered entry fade-ins
All animation runs on the UI task (Core 1). New globals are nulled in `ui_reset_lvgl_objects()` (lcd_activity.h).

#### Voice Response Screen (JSON Viewer)

Multi-page card UI parsed from `UI_VOICE_RESPONSE` JSON:
- Supports structured pages (`ui.screens[]`) and fallback text pagination
- Page dots for multi-page navigation via encoder scroll
- Styles: default, warning, success, error, info
- Items rendered as bulleted lists
- Maximum `SHIP_VOICE_UI_MAX_PAGES` pages, `SHIP_VOICE_UI_MAX_ITEMS` items per page
- UTF-8 text sanitization (curly quotes, em dashes, ellipsis -> ASCII)

#### Shopping List Screen

Scrollable card list with:
- Pull-to-refresh (5 counter-clockwise ticks at top)
- Tap to show delete/back overlay
- Knob scrolling with highlight
- Item count in title

#### Expiry Date Picker

Three-segment picker (Month/Day/Year) with:
- Tap to select segment, knob to adjust value
- Timeout ring (arc countdown)
- OK button to submit, Back button to cancel

#### create_custom_ui()

Master UI builder called from `init_ui_stack()`. Creates the base screen hierarchy:
- `g_base_screen` (root)
- `loading_screen`, `list_container`, `status_screen`, `logged_screen`
- `meal_result_screen` with calorie/protein/carbs/fat labels
- `recording_indicator`, `processing_indicator` (full-screen border rings)
- `menu_screen` with flex-column menu items
- `expiry_screen` with date picker
- `delete_menu`, `menu_menu` (overlay action buttons)

---

### 6. lcd_ship_flow.h -- Animations and Screen Transitions

**Purpose:** LVGL animations for screen transitions, timeout ring updates, processing progress, phase inference.

#### Key Functions

| Function | Description |
|----------|-------------|
| `ship_start_hold_still_animation()` | Fade-in title, countdown ring, label. 3-second countdown, then capture phase pulse |
| `ship_update_hold_still_countdown()` | Updates countdown number (3, 2, 1) then capture icon pulse |
| `ship_start_processing_animation()` | Fade-in halo, spinner, label, subtitle |
| `ship_update_processing_progress()` | Updates dish processing fill bar (1% to 99% over ~15s) |
| `ship_start_logged_success_animation()` | Checkmark + "Logged" fade-in slide |
| `ship_start_voice_ack_animation()` | Similar to logged but "On it!" text |
| `ship_start_expiry_choice_animation()` | Quantity label + Skip/Add buttons slide in from sides |
| `ship_update_ai_listening_countdown()` | Updates listening ring based on hold duration |
| `ship_infer_phase(phase, text)` | Infers UI phase from text content when explicit phase is missing |

#### Hold-Still Countdown Constants

| Constant | Value |
|----------|-------|
| `SHIP_HOLD_COUNTDOWN_MS` | 3000ms |
| `SHIP_HOLD_CAPTURE_PULSE_MS` | 1000ms |

#### Dish Processing Progress

Non-linear progress curve:
- 0-1.5s: 1% -> 16%
- 1.5-5s: 16% -> 40%
- 5-9.5s: 40% -> 65%
- 9.5-12.5s: 65% -> 85%
- 12.5-15s: 85% -> 99%

---

### 7. lcd_ship_route.h -- Screen Routing and UI Status Application

**Purpose:** Central routing logic that maps `(op, mode, phase)` tuples to screen transitions. Handles touch on expiry choice/expiry screens, result/debug screens.

#### Screen Routing (`route_ship_ui`)

Maps UI_STATUS messages to ScreenId:

**SCAN check-in/check-out:**
- CAPTURING -> HOLD_STILL
- WAITING_INPUT -> EXPIRY_CHOICE
- DONE -> LOGGED
- ERROR -> ERROR

**SCAN discard:**
- CAPTURING -> HOLD_STILL
- WAITING_INPUT -> EXPIRY_CHOICE (discard adds "Add to Shopping List?" option)
- DONE/UPLOADING -> LOGGED
- ERROR -> ERROR

**SCAN dish (DISH_LOG):**
- CAPTURING -> HOLD_STILL
- DONE/UPLOADING/UPLOAD_STARTING/RESULT_WAITING/PROCESSING -> LOGGED
- ERROR -> ERROR

**VOICE:**
- UPLOADING/PROCESSING/PARSE/APPLY -> PROCESSING
- ERROR -> ERROR

#### UI_SHOW Macro

```c
#define UI_SHOW(next, reason) ui_show_screen(next, reason, __FILE__, __LINE__, __func__)
```

Logs caller file/line for every screen transition for debugging.

#### Key Functions

| Function | Description |
|----------|-------------|
| `ui_show_screen(next, reason, file, line, func)` | Logs and dispatches screen transition |
| `ui_apply_ship_ui_status(evt)` | Applies UI_STATUS to current state; routes to correct screen |
| `expiry_choice_handle_touch(x, y)` | Hit-tests Skip/Add buttons on expiry choice screen |
| `expiry_handle_touch(x, y)` | Hit-tests month/day/year/OK/back buttons on expiry picker |
| `ui_show_result(is_error, title, mode)` | Shows result screen with Home + optional Retry buttons |
| `show_ship_debug_screen()` | Debug info: UI_STATUS op/phase, sense_state, heartbeat age, wifi creds, ui_busy |
| `ship_send_discard_choice(add_to_shopping_list, reason)` | Sends INPUT_DISCARD_OPTIONS to Sense |

#### Finalization Guard

`g_ship_ui_finalized` + `g_ship_ui_finalized_job_id` prevent late UI_STATUS messages from overriding terminal screens (LOGGED/ERROR) with stale CAPTURING phases.

---

### 8. lcd_ship_action.h -- Menu Actions and Meal Results

**Purpose:** Hit-test dispatch, menu action handlers, meal result display, toast events, firmware info polling.

#### Key Functions

| Function | Description |
|----------|-------------|
| `ship_menu_hit_test(x, y)` | Tests touch coordinates against hitbox tables for current menu screen |
| `ship_menu_send_action(hb)` | Dispatches menu button tap: MORE, HOME, SETTINGS, RESET_WIFI, MANUAL_OTA, DEBUG_LOG, SHOPPING_LIST, BACK, or scan actions |
| `ship_menu_send_menu_select(menu_item, index, label)` | Sends INPUT_MENU_SELECT to Sense, begins local scan request immediately |
| `ship_menu_begin_local_scan_request(menu_item)` | Sets up local UI state for SCAN operation before Sense confirms |
| `ship_cancel_local_scan_request(reason, message)` | Cancels a pending scan if rejected by toast |
| `ui_apply_ship_meal_result(doc)` | Processes nutrition result (calories, protein, carbs, fat, recommendation) |
| `ship_menu_request_fw_info()` | Sends INPUT_FW_INFO to get Sense firmware version |
| `ship_menu_service_fw_info_request(now_ms)` | Retries fw info requests with backoff |
| `ship_menu_send_manual_ota(reason)` | Sends INPUT_OTA_CHECK, sets manual override, shows status |
| `ship_menu_handle_ui_status(doc)` | Parses UI_STATUS from Sense, updates g_ship_ui_* globals, posts EVT_SHIP_UI_STATUS |

#### Meal Result Screen

Displays: calories (large), meal description, protein/carbs/fat values (large), recommendation text.

#### Last Action Tracking

`g_last_action` stores the most recent menu selection for retry support on error screens.

---

### 9. lcd_menu.h -- Legacy Menu and List Display

**Purpose:** Menu display (retired for ship flow), list rendering, delete handler, knob scroll callbacks.

**Note:** The legacy shopping-list UI (`list_container`, `ui_update_list`) is retired. Functions exist but return early. The ship menu (`lcd_ship_screens.h`) is the active UI.

#### Key Functions

| Function | Description |
|----------|-------------|
| `knob_left_cb(arg, data)` | ISR: posts EVT_SCROLL_DELTA(-1) + EVT_HAPTIC_TICK + INPUT_SCROLL to queues |
| `knob_right_cb(arg, data)` | ISR: posts EVT_SCROLL_DELTA(+1) + EVT_HAPTIC_TICK + INPUT_SCROLL to queues |
| `delete_item_btn_handler(e)` | LVGL event: optimistically removes item, sends INPUT_DELETE, updates UI immediately |
| `apply_event_to_state(s, evt)` | Pure data: adjusts selected_index for scroll events, clamps to bounds |
| `ui_refresh_from_state(s)` | Refreshes list display from state (calls ui_update_list). MUST be called from UI task only |

#### Scroll Throttling

`SCROLL_REFRESH_MIN_MS` (50ms) prevents SPI queue overflow during fast scrolling. `scroll_pending_redraw` defers the redraw to the next UI tick if throttled.

#### Critical ISR Safety

Knob callbacks run in interrupt context. They must NOT call LVGL or UART TX. They only post to FreeRTOS queues using `xQueueSendFromISR`.

---

### 10. lcd_sleep.h -- Sleep/Wake Coordination

**Purpose:** Deep sleep entry, Sense sleep handshake, wake mask configuration, sleep denial handling.

#### Deep Sleep Entry (`enterLightSleep`)

Despite the name, this function enters **deep sleep** (not light sleep). Flow:

1. Check `sleep_blocked_for_ota()` -- abort if OTA pending
2. Check sleep deny count (force sleep after 10 denies)
3. Call `notify_sense_sleep()` for handshake
4. Reset all UI state (buttons, menus, animations)
5. Save shopping list to NVS
6. Post EVT_RESET_UI to hide all screens
7. Wait for I80 bus TX idle (`lcd_lvgl_wait_tx_done`)
8. Power off LCD panel
9. Release wake line to Sense
10. Configure EXT0 wake on GPIO9 (touch), RTC pull-up
11. Configure timer wake (maintenance, schedule, or periodic 6h)
12. Put touch IC in standby mode
13. Turn off backlight
14. Delete UI task
15. `esp_deep_sleep_start()` -- no return

#### Wake Handling (post deep sleep)

1. Debounce check (ignore wakes < 200ms)
2. Validate EXT0 touch wake (reject if GPIO9 already HIGH)
3. Set `stay_awake_until_ms` for 8s on touch wake
4. Clear touch/encoder interrupts
5. Set ignore periods: touch 300ms, scroll 150ms
6. Restore backlight and panel
7. Request Sense wake
8. Load saved list from NVS
9. Post EVT_RENDER_ACTIVE_LIST to UI task

#### Sense Sleep Handshake (`notify_sense_sleep`)

1. Check if Sense is probably awake (state tracking, recent RX, grace period)
2. If Sense already asleep or sleep-ready recent, allow local sleep
3. Send `INPUT_SLEEP` message (up to 3 attempts)
4. Wait for SLEEP_READY or SLEEP_DENY response (25s timeout)
5. Handle user input cancellation during wait
6. On SLEEP_DENY: enter low-power wait, track retry
7. On timeout with no link: fallback timer sleep (15s)

#### Wake Mask

Only GPIO9 (touch) is in the EXT1 wake mask. Encoder pins are **excluded** because EC1_B resting LOW causes instant wake via ANY_LOW trigger.

See ARCHITECTURE.md Re-Wake System for the Hard Gate architecture that governs when GPIO39 pulses are allowed vs UART-only communication.

#### LCD clock + absolute-window self-wake (scheduled maintenance OTA)

The LCD has **no NTP/RTC clock of its own**, so `time(nullptr)≈0` and `lcd_time_valid()`
(which is just `time(nullptr) > 1700000000`) used to be false — leaving the absolute-epoch
maintenance machinery dormant and the deep-sleep timer armed with a stale relative offset
(`g_lcd_maintenance_wake_in_s`, never decremented for elapsed time → missed the window after
any intermediate wake). Fix:

- **Clock set:** the Sense now sends `now_epoch` in `MAINT_WINDOW`; the handler
  (`lcd_uart_rx.h`) calls `lcd_set_clock_from_sense()` (in `LCD_Minimal.ino`) which
  `settimeofday()`s the LCD clock before arming/persisting. ESP-IDF carries the set time
  across deep sleep via the RTC, so partial (touch/timer) wakes keep an accurate clock.
  Re-applied on each MAINT_WINDOW (drift correction). No-op when `now_epoch<=0` (old Sense).
- **Self-wake timer (`enterLightSleep` in `lcd_sleep.h`):** when maintenance is armed AND
  `lcd_time_valid()` AND `g_lcd_maintenance_start_epoch>0`, the deep-sleep timer is computed
  from the **absolute window** every sleep: `target = start_epoch − grace_before_sec −
  LCD_MAINT_WAKE_LEAD_S` (15s lead, matching the Sense's `nextWakeEpochForSleep(now,15,5)`);
  `sleep_timer_sec = target − now_epoch` (reasons `maintenance_abs` /
  `maintenance_abs_imminent`). Recomputing each sleep makes intermediate pre-window wakes
  harmless. If already at/inside the band, clamps to a 5s safety timer rather than sleeping
  past the window. Falls back to the relative `g_lcd_maintenance_wake_in_s` (`maintenance_rel`)
  ONLY when the clock is invalid. The `[SLEEP_TIMER]` log now shows `clock_valid`,
  `now_epoch`, `target_epoch`, `start_epoch`.
- **Stay awake through the window (sleep gate in `LCD_Minimal.ino` `loop()`):** when the clock
  is valid and `lcd_maintenance_window_is_current(time(nullptr))`, sleep is inhibited
  (`reason=maintenance_window`, logged `[LCD_MAINT] stay_awake in_window`) so the Sense's
  LCD-first OTA proxy (queries the LCD with retries over ~35s at window entry) can reach it.
  Bounded by `start − grace_before` .. `start + duration + grace_after` so it self-terminates;
  clock-gated and independent of `g_lcd_maintenance_active`, so it survives stale-flag clears
  and never affects normal (non-maintenance) idle-sleep.
- **Arm-time keep-awake (delivery race fix):** the clock-set above only works if the LCD
  actually receives the `MAINT_WINDOW` before idle-sleeping. After a tap the LCD sleeps at
  ~10s but the Sense needs ~10–15s for WiFi+NTP+schedule-fetch (and only sets its pending-sync
  flag AFTER that fetch), so the Sense sends a lightweight **`MAINT_KEEPALIVE`** throughout the
  post-wake connect/fetch phase (driven by `keep_lcd_awake_during_maint_arm()` on the Sense,
  bounded ~25s after wake, gated by `!g_ota_check_done` / pending-unacked) instead of the
  not-yet-sendable window. The `MAINT_KEEPALIVE`
  handler (`lcd_uart_rx.h`) just calls `resetActivityTimer()` + nudges `ota_stay_awake_until_ms`
  (+8s) — no maintenance state touched. Additionally, the future-window arm branch
  (`wake_in_s>0`) of the `MAINT_WINDOW` handler now also calls `resetActivityTimer()` + nudges
  stay-awake so repeated arm-syncs hold the LCD awake until `now_epoch` lands. Because the
  Sense is awake during arm-time, `ota_stay_awake_until_ms` is honored by the sleep gate.
- **Diagnostic breadcrumbs (error-log black box, area `maint`):** `MW_RX` (value=`lcd_time_valid()`,
  detail=request_id) on window receipt in `lcd_uart_rx.h`; `SLEEP` (value=`sleep_timer_sec`,
  detail=`timer_reason`, only when `g_lcd_maintenance_timer_armed`) in `lcd_sleep.h`; `RESTORE`
  (value=`g_lcd_maintenance_timer_armed`, detail=`clk=<0/1> rid=<...>`) in
  `lcd_restore_persisted_maintenance_state()`. Viewable via Debug screen / USB `errors`. The
  errlog forward declaration was moved earlier in `LCD_Minimal.ino` so the restore/sleep paths
  (above its definition) can emit these.

#### Key Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `SLEEP_HANDSHAKE_MAX_ATTEMPTS` | 3 | Max INPUT_SLEEP retries |
| `SLEEP_DENY_MAX_COUNT` | 10 | Force sleep after N denies |
| `SLEEP_DENY_RETRY_DEFAULT_MS` | 5000 | Default retry interval after deny |
| `SLEEP_FALLBACK_TIMER_SEC` | 15 | Timer wake if handshake fails |
| `LCD_SLEEP_FALLBACK_TIMER_SEC` | 30 | Fallback timer for certain paths |
| `LCD_MAINT_WAKE_LEAD_S` | 15 | Lead before window start for absolute self-wake (matches Sense lead) |

---

### 11. lcd_ota_uart.h -- OTA-over-UART Receiver

**Purpose:** Receives LCD firmware updates streamed from Sense board over UART using COBS-framed binary protocol.

#### State Machine

```
LCD_OTA_IDLE -> LCD_OTA_RECEIVING -> LCD_OTA_FINALIZING -> LCD_OTA_IDLE
                       |
                       v
               LCD_OTA_ABORTING -> LCD_OTA_IDLE
```

#### Protocol Flow

1. Sense sends `LCD_OTA_QUERY` -> LCD responds with `LCD_OTA_QUERY_RESP` (current fw version, OTA partition info, enriched partition/state fields -- see below)
2. Sense sends `LCD_OTA_BEGIN` with session_id, image_size, sha256, version
3. LCD validates (partition exists, image fits), sends `LCD_OTA_BEGIN_ACK`
4. LCD switches to binary RX mode (`g_lcd_ota_binary_mode = true`)
5. Sense sends COBS-framed chunks (512 bytes each, `UartOtaProtocol`)
6. LCD writes each chunk to OTA partition via `esp_ota_write`, sends ACK/NACK
7. LCD updates SHA256 hash, saves NVS progress every 64KB
8. Sense sends `LCD_OTA_END` JSON (detected by peeking for `{` in binary stream)
9. LCD finalizes: SHA256 verify, `esp_ota_end`, `esp_ota_set_boot_partition`
10. LCD sends `LCD_OTA_END_ACK` with sha_match and ota_ok
11. On success: restore UI, sleep naturally, boot new firmware on next wake
12. On failure: restore UI, log error

#### Enriched `LCD_OTA_QUERY_RESP` Fields

`lcd_ota_handle_query()` reports both legacy and enriched fields. The enriched partition/state fields are built by the shared helper `lcd_build_fw_status_json(JsonDocument&)` (defined in `lcd_ota_uart.h`), which is reused by the USB `fw`/`ver` command so the two reporting paths never diverge. The state-string mapping is `lcd_ota_img_state_str()`.

| Field | Source | Notes |
|-------|--------|-------|
| `lcd_fw` | `kFirmwareVersion` | Running image version |
| `ota_part_label` / `ota_part_size` | `esp_ota_get_next_update_partition(NULL)` | Legacy, retained for back-compat |
| `running_part` | `esp_ota_get_running_partition()->label` | `?` if NULL |
| `running_state` | `esp_ota_get_state_partition(running)` | One of `NEW`, `PENDING_VERIFY`, `VALID`, `INVALID`, `ABORTED`, `UNDEFINED`, `UNKNOWN` |
| `boot_part` | `esp_ota_get_boot_partition()->label` | `?` if NULL |
| `next_part` | `esp_ota_get_next_update_partition(NULL)->label` | Slot a future OTA would write; `?` if NULL |
| `last_ota_result` | NVS errlog black box | Best-effort newest `area=="ota"` event; field omitted when none found |

#### Key Design Decisions

- **No ESP.restart():** On ESP32-S3 with USB-Serial/JTAG, software reset enters download mode. Instead, the device sleeps naturally and boots the new partition on wake.
- **esp_restart() on OTA completion:** On OTA completion, `esp_restart()` is called instead of `esp_deep_sleep()`. Deep sleep preserves RTC memory, causing the bootloader to cache the old boot partition. `esp_restart()` clears RTC state and forces a fresh otadata read, ensuring the new partition boots.
- **SHA context not resumable:** Saving/restoring mbedtls SHA256 context caused heap corruption. On resume, OTA restarts from offset 0.
- **UI task stays alive:** Set `g_ota_screen_active` flag so UI task just ticks LVGL without processing events, avoiding need to restart UI task post-OTA (which caused crashes).
- **JSON TX suppressed:** `g_suppress_uart_json_tx = true` prevents JSON messages from corrupting COBS binary frames.

#### Post-OTA Self Test

Called from `setup()` on boot. If running partition is `ESP_OTA_IMG_PENDING_VERIFY`:
1. Check LVGL initialized
2. Check senseSerial ready
3. Check internal heap >= 32KB
4. Check firmware version string exists
5. Pass -> `esp_ota_mark_app_valid_cancel_rollback()`
6. Fail -> `esp_ota_mark_app_invalid_rollback_and_reboot()`

#### UI Restore (`lcd_ota_uart_restore_ui`)

Clears all OTA/maintenance flags, sets `provision_return_home_pending = true` so the UI task navigates to HOME on Core 1 (safe LVGL access).

#### Key Constants

| Constant | Value |
|----------|-------|
| `LCD_OTA_CHUNK_SIZE` | 512 bytes |
| `LCD_OTA_NVS_SAVE_INTERVAL` | 64KB |
| `LCD_OTA_IDLE_TIMEOUT_MS` | 30s |
| `LCD_OTA_CHUNK_TIMEOUT_MS` | 5s |

---

### 12. lcd_uart_rx.h -- UART RX Message Processing

**Purpose:** Parses all incoming JSON messages from Sense board. Runs on Core 0 (uart_task). **NO LVGL CALLS.**

#### Message Handlers

| Message Type | Handler Description |
|-------------|---------------------|
| `PONG` | Confirms Sense awake, clears wake retry, exits timer-wait mode |
| `LINK_HB` | Logs heartbeat age |
| `SLEEP_READY` | Sets `sleep_ready_received`, releases wake line, marks Sense as ASLEEP |
| `SLEEP_DENY` | Records deny reason and retry interval, enters wait state |
| `SENSE_SLEEP_INTENT` | Checks if LCD allows sleep (via `lcd_sleep_intent_allowed`), sends SLEEP_DENY if not |
| `UI_STATUS` | Routes to `ship_menu_handle_ui_status()` for ship UI; tracks voice/scan state |
| `UI_MEAL_RESULT` | Routes to `ui_apply_ship_meal_result()` for nutrition display |
| `UI_TOAST` | Posts `EVT_SHIP_UI_TOAST` to event queue |
| `UI_VOICE_RESPONSE` | Stores JSON text, posts `EVT_SHIP_VOICE_JSON` to event queue |
| `UI_LIST` | **Disabled** (returns early). Would update shopping list with deleted-item filtering |
| `FW_INFO` | Stores Sense firmware version, updates settings screen label |
| `RELEASE_WAKE` | Releases INT_PIN wake line on Sense request |
| `SYNC` / `SYNC_ACK` | Link synchronization protocol |
| `MAINT_WINDOW` | Maintenance window management -- arms timer, enters headless mode, or clears state. Parses the Sense's `now_epoch` and calls `lcd_set_clock_from_sense()` (sets the LCD wall clock via `settimeofday()`) BEFORE arming/persisting, so the absolute-window self-wake + window-current logic has a valid clock. See "LCD clock + absolute-window self-wake" below |
| `OTA_LOCK` / `OTA_UNLOCK` | Sets/clears OTA lock flags. **OTA_LOCK** extends `ota_stay_awake_until_ms` by `LCD_OTA_LOCK_STAY_AWAKE_MS` (180000 ms = 3 min, only-extend like `ship_menu_send_manual_ota`) to keep the LCD awake/UART-responsive through the entire dual-board OTA: Sense self-OTA (~40s) + reboot (~15s) + boot/wifi/proxy start (~20s). It **also sets `g_ota_lock_window_until_ms = millis() + LCD_OTA_LOCK_STAY_AWAKE_MS`** (see "Dual-OTA stay-awake survives Sense reboot" below). Logs `[OTA] ota_stay_awake extended <ms>ms (ota_lock)`. It also still extends the maintenance deadline by `OTA_LOCK_TIMEOUT_MS` when maintenance is active. **OTA_UNLOCK** clears the OTA flags but **does NOT zero `ota_stay_awake_until_ms` while a live OTA_LOCK window remains** (`millis() < ota_stay_awake_until_ms`): the Sense sends OTA_UNLOCK *before* its self-OTA reboot, so clearing the window would let the LCD deep-sleep and miss the post-reboot `LCD_OTA_QUERY` (`lcd_query_fail`). UNLOCK also calls `lcd_manual_ota_override_clear("ota_unlock")` to clear the manual-OTA override (so the periodic INPUT_OTA_CHECK resend loop in `loop()` stops — without this, an already-up-to-date manual OTA leaves the override set and the resend condition `override_active() && !ota_locked` keeps firing, re-locking/unlocking the Sense and looping the "Software Update" screen with a ~1s black-flash until the override's 5-min TTL). OTA_UNLOCK is the single termination point for ALL Sense "nothing to do" exits (up_to_date, downgrade blocked, rollout skip, apply blocked) since they all go through `release_waiting_lcd_ota -> OTA_UNLOCK`. The clear is idempotent (early-returns if the override is not set). UNLOCK then sets `provision_return_home_pending = true` (logged `[OTA] return_home_pending=1 (ota_unlock)`) so the UI task on Core 1 calls `show_ship_main_menu()` — this navigates `ui_screen_state` back to `SCREEN_HOME` and `lv_scr_load(ship_menu_screen)` (a different screen object) so the OTA "Software Update" overlay (child of the old active screen) is no longer visible. Without this, an already-up-to-date / no-op manual OTA cleared the flags but left the OTA overlay as the active screen, so the LCD slept and redrew "Software Update" on wake. Cross-core-safe (plain `bool`, same pattern used after provisioning). 2-min auto-unlock timeout in `loop()` as safety net. |
| `OTA_CHECK` | Initiates OTA check, sends ACK, respects maintenance-only policy |
| `OTA_APPLY_REQUIRED` | Flags that Sense needs wake for OTA apply |
| `PROVISION_QR` | Caches QR data, shows provisioning screen |
| `PROVISION_STATUS` | Updates provisioning state (connecting, connected, idle, unprovisioned, ap_setup) |
| `WIFI_CREDS` | Stores WiFi credentials via LcdWifiCreds (prod wrapper) |
| `WIFI_ON` | Starts WiFi (deferred if UI active, executed if idle) |
| `SENSE_OTA_ACTIVE/IDLE` | Tracks Sense OTA state |
| `SENSE_DIAG` | Logs diagnostic events; persists if `persist: true` |
| `WIFI_DIAG_SUMMARY` | Stores WiFi diagnostic summary |
| `LCD_OTA_QUERY` | Handled by `lcd_ota_handle_query()` |
| `LCD_OTA_BEGIN` | Handled by `lcd_ota_handle_begin()` |

#### Wake Timer Wait Mode

When woken by timer, LCD enters `wake_timer_wait_mode`: waits up to 30s for Sense to respond to pings before sleeping again. Cleared when PONG or SYNC_ACK received.

#### Manual OTA Retry on SLEEP_READY

If LCD sent INPUT_OTA_CHECK but Sense goes to sleep before processing it, LCD re-wakes Sense and resends the request.

#### Manual OTA Periodic Resend (timer)

`loop()` resends INPUT_OTA_CHECK (`reason=timer_resend`) every ~1500 ms while `lcd_manual_ota_override_active()` is true and `ota_locked` is false. This guarantees delivery if the original single send from `ship_menu_send_manual_ota()` is dropped during the Sense's wake/boot LCD_OTA_QUERY window (the event-driven resends on SLEEP_READY/FW_INFO depend on receiving those messages back, so they can miss). The resend stops immediately once `ota_locked` becomes true (so it never spams during the actual OTA transfer), when the override is cleared by the `OTA_UNLOCK` handler at the end of an OTA session (the normal termination — including the already-up-to-date case, which previously looped the "Software Update" screen because the override was never cleared), or when the override TTL expires (5-min safety net). Uses a `static unsigned long last_resend_ms` gate reset to 0 when inactive. Logs `[OTA_MANUAL] resend INPUT_OTA_CHECK reason=timer`.

---

### 13. lcd_ui_task.h -- FreeRTOS UI Task

**Purpose:** Processes `app_event_queue` events under the LVGL lock. Runs all screen transitions, animations, and rendering on Core 1.

#### OTA Progress Screen Rendering

- `g_lcd_ota_progress_pct` (volatile int) is updated every 10% during LCD OTA binary frame reception in `lcd_ota_uart.h`
- `g_lcd_ota_show_progress` (volatile bool) controls visibility
- Two-phase overlay: "Software Update" text during Sense self-OTA, "Updating XX%" during LCD binary transfer
- Rendered with black background, white text, lv_font_montserrat_28
- All rendering happens on Core 1 (UI task) — Core 0 only sets the volatile flags
- **Overlay lifecycle / stale-overlay fix:** The overlay (`ota_overlay`) and its label are `lv_obj_create(lv_scr_act())` children of whatever screen was active when OTA began. Since manual OTA is launched from the **Settings** screen, the overlay becomes a child of the settings screen. The four overlay statics (`ota_overlay`, `ota_label`, `last_shown_pct`, `last_was_transfer`) are now declared at the top of the loop body (just before the `if (g_ota_screen_active)` block) instead of inside it, so a teardown immediately **after** that block can see them. When `g_ota_screen_active` clears, the UI task deletes the overlay (`lv_obj_del(ota_overlay)`, which also frees the child label) and resets the statics, logging `[OTA] overlay torn down (screen inactive)`. Without this, the overlay lingered as a permanent child of the settings screen and the "Software Update" text reappeared on top every time Settings was reopened (even with all OTA flags clear). The teardown runs while the LVGL lock acquired at the top of the loop is still held — it does **not** re-lock or unlock (the normal path below releases the lock exactly once).

#### Task Loop Structure

```
for (;;) {
  1. Check g_ui_task_exit_requested -> self-delete if true
  2. Acquire LVGL lock (50ms timeout)
  3. If g_ota_screen_active: just tick LVGL, release, delay 50ms (overlay statics hoisted to loop top)
  3b. Else (OTA inactive): if ota_overlay still exists, lv_obj_del it + reset statics (lock still held)
  4. Drain ship UI events (EVT_SHIP_UI_STATUS, EVT_SHIP_UI_TOAST, EVT_SHIP_VOICE_JSON)
  5. Handle provision_return_home_pending -> show_ship_main_menu()
  6. Service timed screen transitions (voice_ack hide, logged hide, error hide)
  7. Update live animations (hold-still countdown, processing progress, expiry rings, AI listening)
  8. Check voice response timeout
  9. Apply dirty UI status
  10. Handle dish processing timeout (DISH_PROCESSING_TIMEOUT_MS)
  11. Process deferred scroll redraws
  12. Process single event from queue:
      - EVT_SCROLL_DELTA: Route to appropriate screen handler
      - EVT_LIST_REPLACED: Swap pending->active, render, save to NVS
      - EVT_SHOW_MEAL_RESULT: Display nutrition card
      - EVT_SHOW/HIDE_PROVISION_QR: Provisioning screens
      - EVT_RESET_UI: Hide all screens for sleep
      - EVT_HAPTIC_TICK: Pulse haptic motor
      - EVT_TOGGLE_BUTTONS: Show/hide delete/menu overlays
      - EVT_MENU_SELECTED: Handle legacy menu selection
  13. Call ui_lvgl_tick() if no events processed
  14. Release LVGL lock
  15. Log heartbeat every 500ms
}
```

#### Event Types

| Event | Data | Description |
|-------|------|-------------|
| `EVT_SCROLL_DELTA` | `int scroll_delta` | Knob rotation (+1/-1) |
| `EVT_LIST_REPLACED` | `int new_count` | Pending list ready for swap |
| `EVT_SHIP_UI_STATUS` | op, mode, phase, text, ui_policy, job_id, msg_id | Sense status update |
| `EVT_SHIP_UI_TOAST` | `char ship_toast[64]` | Toast message |
| `EVT_SHIP_VOICE_JSON` | (none, reads g_ship_voice_json_text) | Voice response ready |
| `EVT_SHOW_MEAL_RESULT` | calories, protein, carbs, fat, summary, recommendation | Nutrition result |
| `EVT_SHOW/HIDE_PROVISION_QR` | ssid, password, url | Provisioning QR |
| `EVT_UPDATE_PROVISION_STATUS` | state string | Provisioning state change |
| `EVT_SHOW/HIDE_PROVISION_INTRO` | (none) | Intro screen control |
| `EVT_RESET_UI` | (none) | Hide all screens for sleep |
| `EVT_HAPTIC_TICK` | (none) | Single haptic pulse |
| `EVT_START/STOP_GLOWING` | glow_reason | Processing animation |
| `EVT_TOGGLE_BUTTONS` | (none) | Delete/menu button visibility |
| `EVT_MENU_SELECTED` | menu_index | Legacy menu item selection |
| `EVT_RENDER_ACTIVE_LIST` | new_count | Render g_active list |
| `EVT_VOICE_ITEMS_ADDED` | new_count | Optimistic voice items |
| `EVT_UI_STATUS_IDLE` | (none) | Clear status overlay |
| `EVT_USB_ENTER_LIST` | (none) | USB `list`: show shopping list + arm refresh (UI task) |
| `EVT_USB_REFRESH` | (none) | USB `refresh`: pull-to-refresh path (UI task) |
| `EVT_USB_DELETE` | `int usb_index` | USB `del N`: delete N-th visible item (UI task) |
| `EVT_USB_HOME` | (none) | USB `home`: return to main menu (UI task) |

#### Scroll Routing by Screen

| Screen | Scroll Behavior |
|--------|----------------|
| SCREEN_VOICE_JSON | Page between voice response pages |
| SCREEN_SHOPPING_LIST | Scroll item selection; overscroll = pull-to-refresh |
| SCREEN_HOME (MAIN) | Ignored (touch-first UI) |
| SCREEN_EXPIRY_CHOICE | Adjust quantity (if not discard mode) |
| SCREEN_EXPIRY | Step month/day/year based on active segment |
| Legacy list | Scroll selected item index |

#### UI Task Stack

12288 bytes, pinned to Core 1, priority 3.

---

### 14. lcd_uart_task.h -- UART Task and USB Serial Handler

**Purpose:** FreeRTOS task for UART communication. Drains TX queue, assembles RX lines, handles USB serial commands, manages OTA binary mode.

#### Task Loop Structure

```
for (;;) {
  1. Service pending refresh (INPUT_WAKE send)
  2. Drain TX queue (max 16 per loop), handle deferred-awake messages
  3. If g_lcd_ota_binary_mode: delegate to lcd_ota_receive_loop(), continue
  4. Read UART RX bytes (max 512 per loop), assemble lines, dispatch to uart_process_received_message
  5. Log partial line diagnostics (>250ms stale)
  6. Read USB Serial commands
  7. Service diagnostic mode (keepalive pings every 2s)
  8. Yield (1ms if busy, 5ms if idle)
}
```

#### USB Serial Commands

| Command | Action |
|---------|--------|
| `fw` / `ver` | Print `[FW] {json}` line (lcd_fw, running_part, running_state, boot_part, next_part) via `lcd_build_fw_status_json()`, then legacy human-readable `[FW]` lines. Case-insensitive. |
| `ota` | Send INPUT_OTA_CHECK |
| `list` | Emulate tapping List on the second menu. Sends INPUT_WAKE, posts `EVT_USB_ENTER_LIST` so the UI task runs `show_shopping_list_screen()` + arms a list refresh. Prints `[USB] list`. |
| `refresh` | Emulate the pull-to-refresh gesture (5 CCW ticks at top). Posts `EVT_USB_REFRESH`; UI task calls `shopping_list_trigger_refresh()` (only on the list screen). Prints `[USB] refresh`. |
| `del N` | Emulate the DELETE touch on the N-th visible list item (0-based). Posts `EVT_USB_DELETE` (index N); UI task calls `shopping_list_delete_index(N)` (sends INPUT_DELETE + local removal) then re-renders. Guarded against `g_active.count`. Prints `[USB] del N -> <id>`. |
| `home` | Return to the main menu. Posts `EVT_USB_HOME`; UI task calls `show_ship_main_menu()`. Prints `[USB] home`. |
| `wake` | Force-wake Sense board |
| `sleep` | Send INPUT_SLEEP |
| `ping` | Send INPUT_PING |
| `wifi` | Dump WiFi diagnostic summaries |
| `scan` | Request WiFi network scan |
| `wifitest` | WiFi cold-start test |
| `diag` | Enable diagnostic mode (5 min, no sleep, continuous pings) |
| `diagoff` | Disable diagnostic mode |
| `errors` | Dump NVS error log |
| `clearerrors` | Clear NVS error log |
| `testerrors` | Inject 7 test errors (3 LCD + 4 via Sense) |
| `testmode` | Disable sleep for 1 hour (NVS persisted) |
| `testmodeoff` | Re-enable sleep, clear NVS flag |
| `ui` | Dump UI state (screen, OTA flags, panel, task status) |
| `help` | Show command list |
| `{...}` | Forward JSON command to Sense (INPUT_*, OTA_CHECK) |

#### JSON Command Forwarding

INPUT_MENU_SELECT, INPUT_DISCARD_OPTIONS, and INPUT_EXPIRY_DATE get full JSON forwarding with all payload fields preserved. Other INPUT_* types use simple `uart_send_input_message`.

#### UART Task Stack

Runs on Core 0, priority 2.

---

### 15. lcd_activity.h -- Activity Timer and Sleep Decision

**Purpose:** Inactivity timeout management, guardian force-sleep, sleep eligibility checks, UI state initialization.

#### Sleep Eligibility (`lcd_sleep_intent_allowed`)

Returns false (blocks sleep) if any of these are true:
1. Test mode active
2. OTA blocked (`sleep_blocked_for_ota`)
3. Provisioning active
4. In awake grace period
5. Maintenance boot grace period
6. OTA locked
7. OTA check requested/pending
8. OTA stay-awake timer active
9. Stay-awake timer active
10. Processing animation running
11. WiFi connecting
12. User activity within timeout period
13. Not on a sleep-eligible screen (HOME, SECOND, SETTINGS only)
14. HOME screen shown less than `HOME_SLEEP_DELAY_MS` (10s) ago
15. Dish processing in progress

#### Dual-OTA stay-awake survives Sense reboot (missed-OTA race guard)

There are **three** SENSE_ASLEEP "missed-OTA race guards" that cancel `ota_stay_awake_until_ms` when `sense_state == SENSE_ASLEEP && !ota_locked`:
- `lcd_activity.h` (`lcd_sleep_intent_allowed`, the sleep gate, ~line 79)
- `LCD_Minimal.ino` `sleep_blocked_for_ota()` (~line 2666)
- `LCD_Minimal.ino` `loop()` sleep decision (~line 5076)

Their real purpose is to drop a **stale** stay-awake set by an `INPUT_OTA_CHECK` that the Sense missed (it slept before sending `OTA_LOCK`), so the LCD doesn't stay awake forever. But they fired incorrectly during a **genuine** dual-board OTA: the Sense sends `OTA_UNLOCK` (clears `ota_locked`) then *reboots* for its self-OTA, going offline ~15s. During that window the LCD sees `SENSE_ASLEEP && !ota_locked`, so the guard canceled the stay-awake and the LCD deep-slept — becoming UART-unreachable (the LCD can't wake on UART), so the Sense's post-reboot `LCD_OTA_QUERY` failed (`lcd_query_fail`) and the LCD never updated.

**Fix:** `g_ota_lock_window_until_ms` (file-scope in `LCD_Minimal.ino`) is set by the `OTA_LOCK` handler to `millis() + LCD_OTA_LOCK_STAY_AWAKE_MS` (180s). Each guard now only cancels the stay-awake when `millis() >= g_ota_lock_window_until_ms` (i.e. no *fresh* OTA_LOCK). While the window is live the guards keep the stay-awake and log `[OTA] keep stay_awake (ota_lock window active, sense rebooting)`. The window is cleared when the proxy actually starts (`lcd_ota_handle_begin` in `lcd_ota_uart.h`, alongside `g_ota_screen_active = true`) and on the post-OTA restore/reboot path (`lcd_ota_uart_restore_ui`, alongside `ota_stay_awake_until_ms = 0`). A stale stay-awake (no recent OTA_LOCK) leaves the window at 0 and is still canceled as before; after 180s with no proxy, both windows expire and the LCD sleeps normally.

#### Activity Timeout Values

| Context | Timeout |
|---------|---------|
| Normal | 10s (`INACTIVITY_TIMEOUT_MS`) |
| Waiting for scan | 90s (`SCAN_RESPONSE_TIMEOUT_MS`) |
| Waiting for voice | 30s (`VOICE_RESPONSE_TIMEOUT_MS`) |
| Waiting for list | Configurable (`API_RESPONSE_TIMEOUT_MS`) |

#### Guardian Force-Sleep

`GUARDIAN_FORCE_SLEEP_MS` (5 minutes) is a hard upper bound. After 5 minutes awake, the device enters sleep regardless of activity. Tracked by `guardian_awake_start_ms`.

#### Key Functions

| Function | Description |
|----------|-------------|
| `resetActivityTimer()` | Sets `last_user_activity_ms = millis()`, resets sleep skip logging |
| `user_activity_bump(reason)` | Resets activity timer with log reason |
| `ui_is_sleep_eligible_menu_screen(state)` | Returns true for HOME, SECOND, SETTINGS, SHOPPING_LIST (list honors the normal 10s inactivity timeout) |
| `ship_user_state_current()` | Returns current user state enum based on UI and operation state |
| `init_ui_stack(saved_count)` | Full UI initialization: LCD, LVGL, backlight, rotation, create_custom_ui, knob, UI task |
| `enter_ship_ota_sleep()` | Enters deep sleep for ship OTA wake window |
| `enter_maintenance_sleep()` | Enters deep sleep with maintenance timer |
| `ui_reset_lvgl_objects()` | Nulls all LVGL object pointers for clean restart |
| `init_touch_once()` | One-time touch controller init |
| `init_knob_once()` | One-time encoder init with left/right callbacks |

#### Stale Maintenance Flag Safety

In `loop()`, if maintenance flags are stale (active for >10 minutes past deadline), they are force-cleared:
```c
if (g_lcd_maintenance_active && g_lcd_maintenance_deadline_ms > 0 &&
    millis() > g_lcd_maintenance_deadline_ms + 600000) {
  lcd_clear_maintenance_state("stale_safety", true);
}
```

---

### 16. lcd_persist.h -- NVS Storage and Boot Diagnostics

**Purpose:** Persistent shopping list storage, reset reason labels, wakeup diagnostics.

#### Shopping List Persistence

NVS namespace: `shopping_list`

| Key | Type | Description |
|-----|------|-------------|
| `count` | int | Number of items |
| `selected` | int | Selected index |
| `items` | String | JSON with items[] and ids[] arrays |

#### Key Functions

| Function | Description |
|----------|-------------|
| `save_list_to_storage(s)` | Serializes `app_state_t` to JSON, saves to NVS |
| `load_list_from_storage(s)` | Loads from NVS, parses JSON, populates `app_state_t` |
| `reset_reason_label(reason)` | Maps `esp_reset_reason_t` to human-readable string |
| `wake_cause_label(cause)` | Maps `esp_sleep_wakeup_cause_t` to "EXT0", "EXT1", "TIMER", etc. |
| `print_wakeup_diagnostics(board_name)` | Logs board name, reset reason, wake cause, wake pin level |

#### Maintenance State Persistence

NVS namespace: `lcd_maint`

Stores timer_armed, wake_in_s, remaining_s, request_id, start_epoch, duration, grace periods. Restored on boot to resume maintenance windows across deep sleep cycles.

---

## Inter-Module Dependencies

```
LCD_Minimal.ino (globals, setup, loop)
  |-- lcd_uart.h (TX protocol)
  |-- lcd_anim.h (display, animation, OTA mode)
  |-- lcd_provision.h (WiFi setup screens)
  |-- lcd_ship_screens.h (all screen builders)
  |-- lcd_ship_flow.h (animations, countdown, progress)
  |-- lcd_ship_route.h (screen routing, touch handlers)
  |-- lcd_ship_action.h (menu actions, meal results)
  |-- lcd_menu.h (legacy menu, knob ISR callbacks)
  |-- lcd_sleep.h (deep sleep, sense handshake)
  |-- lcd_ota_uart.h (OTA receiver state machine)
  |-- lcd_uart_rx.h (UART message handlers)
  |-- lcd_ui_task.h (UI task event loop)
  |-- lcd_uart_task.h (UART task, USB serial)
  |-- lcd_activity.h (timers, sleep decision, UI init)
  |-- lcd_persist.h (NVS storage, boot diagnostics)
```

Headers are included in this order and each depends on declarations from all preceding includes.

## Critical Rules

### 1. NO LVGL FROM CORE 0

The `uart_task` runs on Core 0. LVGL is not thread-safe and all LVGL objects are owned by the UI task on Core 1. Any LVGL call from Core 0 causes:
- Heap corruption
- Display hangs
- ESP32-S3 entering ROM download mode

**Correct pattern:** Post an event to `app_event_queue`, let the UI task handle it.

**Known violations:** The `UI_STATUS` handler for legacy SCAN mode still has LVGL calls (status_screen, expiry_screen). These should be refactored to use events.

### 2. Stale Maintenance Flag Clearing

Maintenance flags (`g_lcd_maintenance_active`, `g_lcd_maintenance_deadline_ms`) can become stale if the maintenance window ends while the device is awake but the clear message is lost. The 10-minute safety override in `loop()` force-clears these flags.

### 3. Encoder Pins Not in Wake Mask

GPIO7 (EC1_B) must stay out of the EXT1 wake mask. If EC1_B rests at 0, EXT1 ANY_LOW triggers instant wake, creating a wake loop. Only GPIO9 (touch INT) is in the wake mask.

### 4. OTA Must Not Restart

After OTA completion, do NOT call `ESP.restart()`. On ESP32-S3 with USB-Serial/JTAG, software reset enters download mode. Instead, restore UI and let the device sleep naturally. It boots the new partition on next wake.

### 5. Wake Line Management

The INT_PIN (GPIO39) drives Sense EXT0. Toggling between OUTPUT and INPUT modes is detected by Sense as "lcd_pulsing" and causes sleep deny. Use `sleep_prepare_wake_line_for_request()` before sending INPUT_SLEEP -- it ensures the pin is stable HIGH without toggling.

### 6. OTA Binary Mode UART Exclusivity

During LCD OTA (`g_lcd_ota_binary_mode = true`), all JSON TX is suppressed (`g_suppress_uart_json_tx = true`) and the UART task delegates all RX to `lcd_ota_receive_loop()`. The JSON/binary boundary is detected by peeking for `{` in the byte stream.

### 7. Double-Buffered List State

UART task writes to `g_pending`, UI task reads from `g_active`. Protected by `app_state_mutex`. The swap happens atomically in the UI task when processing `EVT_LIST_REPLACED`. Never read `g_pending` from the UI task or write `g_active` from the UART task without the mutex.

# HALO OTA — Fixes, Architecture & Test Setup (reference)

Single reference for the OTA work on branch `lcd-backlight-binary` (2026-06-01 → 06-03).
Covers how dual-board OTA works now, every fix made (with root cause), the test harnesses,
and the gotchas. Device validated end-to-end: manual + scheduled OTA both working, both
boards (Sense + LCD) from ~6.1.642 → 6.1.712.

---

## 1. System & how OTA works now

Two ESP32-S3 boards, joined by a newline-JSON UART link @115200 (+ COBS binary frames for
LCD firmware streaming):
- **Sense** (`/dev/cu.usbmodem1101`): WiFi/MQTT/TLS, camera, the only board that talks to the cloud.
- **LCD** (`/dev/cu.usbmodem101`): display/UI, **no WiFi**. Gets its firmware *proxied* by the Sense.
- **Actuator** (`/dev/cu.usbmodem21201`): an Arduino that physically taps the LCD touchscreen
  (the only way to wake the LCD — it cannot wake on UART).

**Dual-board OTA sequence (current, post-reorder):**
1. Trigger (button `INPUT_OTA_CHECK`, or scheduled-window wake, or USB-injected `INPUT_OTA_CHECK`).
2. Sense runs `maybeRunOtaCheck()`. **Proxies the LCD FIRST** while it's awake:
   `sense_lcd_ota_query` → `sense_lcd_ota_fetch_manifest` → `OTA_LOCK` → `sense_lcd_ota_proxy`
   (download LCD bin from S3, stream over UART via 512-byte COBS frames; LCD verifies SHA, reboots).
3. **Then** the Sense self-OTAs (`applyToOtaPartition`) and reboots.
4. `lcd_ota_due` (NVS) is a fallback: set only if the inline LCD proxy was skipped/failed, so a
   later boot retries the LCD half.
5. On `pre_sleep` the Sense HTTP-reports `fw`/`lcd_fw`/results to the cloud.

**Why proxy LCD first:** the old order self-OTA'd + rebooted the Sense first, deferring the LCD
proxy to the next boot — by which point the LCD had slept and couldn't be woken over UART
(`lcd_query_fail`). Proxying while the LCD is still awake from the trigger avoids that race.

**Manual vs scheduled:**
- **Manual** (button / USB inject): both boards are warm/awake → rock-solid.
- **Scheduled** (maintenance window): both boards wake independently from deep sleep (cold link),
  so the LCD query can transiently fail and the cloud LCD field lags — but it self-corrects, and
  the Sense applies reliably. **Scheduled OTA REQUIRES the reboot-loop-guard fix (≥6.1.711)** or it
  is permanently disabled (see §2, the guard fix).

---

## 2. The fix chain (branch `lcd-backlight-binary`)

Newest → oldest. Each line: `commit` — what broke / what fixed it.

**LCD manual-OTA UI bugs (live serial-capture debugging, v706–709):**
- `d9e1c60` — **Stale overlay**: the "Software\nUpdate" black overlay (`lcd_ui_task.h`) was created
  once as a child of `lv_scr_act()` (the Settings screen, since OTA launches there) and **never
  deleted** when `g_ota_screen_active` cleared → it lingered and re-appeared every time Settings was
  opened. Fix: hoist the overlay statics to loop scope and `lv_obj_del()` them when the OTA screen
  goes inactive (UI task / Core 1, LVGL lock already held).
- `39f0162` — **Touch bleed-through**: tapping "Settings" loaded the settings screen, then a residual
  touch (~210 ms later, same press) landed on the "Run OTA Update" button now under the finger →
  auto-fired the OTA. Fix: set the existing `touch_ignore_until = millis()+500` on menu screen
  transitions (main/second/settings) so a bleed-through tap can't hit a button on the just-loaded screen.
- `3b8d294` — After an up-to-date manual OTA, `OTA_UNLOCK` hid the overlay flag but didn't navigate
  back, so the LCD slept on the OTA screen and redrew "Software Update" on wake. Fix: `OTA_UNLOCK`
  handler sets `provision_return_home_pending` (UI task shows HOME).
- `2e5381a` — Tapping OTA when already up-to-date looped the "Software Update" screen (black-flash)
  for 5 min. Root cause: `lcd_manual_ota_override_clear()` was defined but **never called**, so the
  `INPUT_OTA_CHECK` resend loop kept firing. Fix: call it in the `OTA_UNLOCK` handler (the single
  termination point for all Sense "nothing to do" exits).

**Cloud-reporting fix:**
- `c1bd29a` — Cloud showed `last_lcd_ota_result=unknown` after a successful LCD OTA because the Sense
  self-OTA'd+rebooted before reporting. Fix: persist `lcd_ota_result=updated` + target version to NVS
  (`lcd_ota_res`/`lcd_ota_ver`) on inline-proxy success; one-shot load on boot so the post-reboot
  report carries it. (Also: this commit added the overnight soak harness.)

**Dual-board OTA core fixes:**
- `3ae6d68` — **THE root cause of "LCD won't update"**: the inline proxy's `LCD_OTA_BEGIN_ACK` and
  `LCD_OTA_END_ACK` waits spun on a mailbox flag with only `delay(10)` and **no UART pump**. Worked
  in the old task-based design (loop() drained UART concurrently); the inline reorder removed that
  drain → the LCD's ACK was never read → `begin_ack_timeout` (~10 s). Fix: `pump_uart_rx_once()`
  inside both wait loops (`g_lcd_ota_proxy_owns_uart` is false there, so the pump runs).
- `afc8c1a` — Stale-RX flush before each `LCD_OTA_QUERY` (`lcdSerial` drain + `uart_reset_rx_state()`)
  so a UART backlog/overflow can't desync the `QUERY_RESP`. Also added the black-box breadcrumbs
  (area `ota_orch`: `otachk_enter`, `lcd_query_tx/ok/fail`, `lcd_proxy_start/done`, `sense_apply_start`).
- `2dd09ec` — Reorder: proxy LCD first (while awake), then Sense self-OTA (see §1).

**Reboot-loop guard (THE scheduled-OTA fix):**
- `034a2e5` — **Scheduled OTA was permanently disabled** (`ota_en=0`, `reboot_loop=1`). The guard fed
  `recordBootTimestamp()` on every boot, and **deep-sleep wakes count as boots**; the detector uses
  `boot_count` as a pseudo-timestamp (last few boots always "in window"), so after a few boots it
  latched off. Scheduled OTA *requires* deep-sleep wakes, so it tripped its own guard. Manual OTA
  bypasses the guard (`halo_ota_manual_override_active`) — the reason manual always worked.
  Fix: only **crash** resets (`PANIC/INT_WDT/TASK_WDT/WDT/BROWNOUT`) feed the guard; **clean** boots
  (deep-sleep wake / power-on / `esp_restart` from OTA) **clear** the history. A real post-OTA
  crash-loop (3 crashes, no clean boot between) still trips it → rollback intact. Self-heals the
  stuck `ota_en=0` on the first clean boot. Also bumped cold-wake LCD query budget 15 s → 35 s
  (`LCD_OTA_QUERY_ATTEMPTS` 3→5, `LCD_OTA_PROXY_QUERY_TIMEOUT_MS` 5000→7000).

**Earlier (pre-reorder, ~v642):** `5221e42` manual OTA bypasses guard + NVS key `dev_dis_rstgrd`;
`572f352` manual-OTA handshake race; `f1674be` documented the serial-reset gotcha.

---

## 3. Test setup

### Hardware / ports (this device)
| role | port | notes |
|---|---|---|
| Sense | `/dev/cu.usbmodem1101` | opening it RESETS the board (see gotcha) |
| LCD | `/dev/cu.usbmodem101` | same; only wakes on touch/encoder/timer |
| Actuator (tap) | `/dev/cu.usbmodem21201` | `tap_implementation/tap.py --command PUSH:700 --port <act>` |

Device id `halo-d45b-8295`; owner id `7d7df434-d942-4037-b054-2d3005ea6abc`.

### Cloud (AWS profile `trepo-dev`, region `us-east-1`)
- **Device report:** DynamoDB `TrepoOtaDeviceLatest-dev`, key `device_id` → `last_fw`, `last_lcd_fw`,
  `last_ota_result`, `last_lcd_ota_result`, `updated_at`. (Written by `OtaReportApiFunction`.)
- **Schedule store:** DynamoDB `TrepoOtaSchedules-dev`, key (`device_id`, `request_id`) →
  `enabled`/`start_epoch`/`duration_sec`/`grace_before_sec`/`grace_after_sec`/`min_idle_min`/`owner_id`/`channel`.
- **Device-facing schedule GET:** `https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com/ota/schedule`
  (returns the soonest future enabled window; nothing for a past window). Lambda log group
  `OtaScheduleApiFunction-uTNwBpku0zPC`.
- **Publish:** `bash halo_ota_demo/publish_both.sh --version X.Y.Z --channel dev --profile trepo-dev`
  (compiles + uploads both boards; NEVER hand-roll manifests). Manifests at
  `s3://halo-ota-dev/halo/ota/dev/{,lcd/}manifest_latest.json`.

### Manual-OTA soak — `automation/overnight_ota_soak.py`
Per cycle: publish bumped version → actuator wake → **inject `INPUT_OTA_CHECK` over the Sense USB**
(== the physical button path; do NOT send `INPUT_WAKE`, it fires a contending `/v1/list`) → poll the
cloud for the Sense self-OTA → verify the LCD via the cloud report (NOT serial). Validated 50+ cycles,
zero real firmware OTA failures. Verify is cloud-only because the per-cycle LCD serial read resets the
LCD and garbles the `[FW]` line. STOP sentinel `automation/overnight_STOP`; results in
`automation/overnight_results.jsonl`.

### Scheduled-OTA soak — `automation/scheduled_ota_soak.py`
Per cycle: publish → clear stale schedule rows + write a fresh window (`start=now+300`, dur 600) to
`TrepoOtaSchedules-dev` → actuator tap (Sense fetches schedule + arms RTC timer; verify via
`OtaScheduleApiFunction` log hit + fresh report) → wait for the window (device auto-wakes ~grace_before
before `start_epoch`) → poll cloud for `last_fw==target` → tap to wake (so the LCD is reachable and the
Sense re-queries) → poll for `last_lcd_fw==target`. Run: `python3 automation/scheduled_ota_soak.py <base_ver> <n_cycles>`.
STOP sentinel `automation/sched_STOP`; results in `automation/scheduled_ota_results.jsonl`.
**A new window per cycle (new `request_id`) is never seen as "consumed", so back-to-back works.**

### Reading device state / breadcrumbs (only when NOT mid-OTA)
- Sense USB `errors` → `sense_errlog_dump` (the `ota_orch` breadcrumb trail, NVS black box).
- Sense USB `fw` / LCD USB `fw` → real running version + partition/state.
- The `[TRUTH]` line dumps `ota_en`, `reboot_loop`, `why`, `next_ota_epoch`, etc.

---

## 4. Critical gotchas
- **Opening a USB-CDC port RESETS the ESP32-S3** (`rst:0x15 USB_UART_CHIP_RESET`), even with
  dtr/rts=False, and it re-enumerates on every deep-sleep wake. **Never live-monitor serial during an
  OTA** — verify via the cloud report. Safe to read serial only when no OTA is in flight.
- **The LCD can only wake via touch/encoder/timer, never UART** — hence the actuator and the cold-link
  rendezvous issue for scheduled OTA.
- **Cloud LCD field lag:** right after a scheduled OTA the cloud may show `last_lcd_fw=unknown` /
  `last_lcd_ota_result=lcd_query_fail` (Sense couldn't re-query the asleep LCD); it **self-corrects**
  to the real version on the next wake-with-both-awake. Ground-truth with LCD USB `fw` if needed.
- Always `publish_both.sh`; always fresh-compile; flash LCD app-only (`esptool write_flash 0x10000`)
  to preserve NVS/WiFi creds; Sense via `arduino-cli upload --input-dir` (also NVS-safe).
- A device stuck at `ota_en=0` (pre-fix) can only be updated via **manual OTA** (bypasses the guard);
  the fix then self-heals it on first clean boot.

---

## 5. Known follow-up (optional)
`docs/OTA_HARDENING_TODO.md` — the cold-link rendezvous hardening (idempotent LCD `BEGIN` re-ACK +
Sense `BEGIN`/query retry). Would make the LCD half of a *scheduled* OTA succeed on the first wake
instead of occasionally needing the `lcd_ota_due` catch-up / cloud self-correct. Not required (it
converges without it) — a coordinated two-board change for a supervised pass.

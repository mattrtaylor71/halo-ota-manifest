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

**Session 2026-06-03 (v720–754, device recovered to 754):**
- `ef12156` — **REVERT of `f939b2d` (#5 cold-link wake-gate).** The #5 attempt did NOT work and was
  HARMFUL, so it was reverted and the device USB-reflashed. See the "#5 — what NOT to do" note in §5.
- `f939b2d` — *(reverted)* attempted #5: a Sense-side "wake-gate" (wait ≤90 s for the LCD UART link
  before the proxy query) + `lcd_ota_due` self-heal. **Failed** (the Sense cannot wake the LCD — GPIO39
  is LCD→Sense only — so waiting for a self-wake the gate can't cause doesn't help: 1/4 cold cycles),
  **and regressed the proxy** (when the LCD *was* awake the gate disrupted the transfer → `timeout`,
  which BLOCKS the Sense self-OTA), **and deadlocked** (`lcd_ota_due` stuck true → its consume-path
  hijacks every maintenance wake to retry the broken proxy then sleeps, never doing the Sense self-OTA).
  Net: device couldn't OTA out by any path; recovered only by USB reflash (see §4 recovery).
- `db45fd5` — **Soak harness: read the LCD via the Sense-UART `INPUT_FW_INFO` query, NEVER open port 101.**
  Opening the LCD's own port (even DTR/RTS-deasserted) STILL intermittently resets it → rolls back an
  in-flight `PENDING_VERIFY` image → false "one behind" reads + collateral Sense misses next cycle. A
  3 h soak's "8/13" was entirely this artifact (gold-standard showed the LCD tracked the Sense the whole
  run). `read_lcd_version()` now queries the LCD *through the Sense* and only passes on `running_state=VALID`.
- `8a69921` — **#4: early-wake `maintenance_outside_window`** abandoned scheduled OTAs. When the device
  woke BEFORE the window (clock fast at arm-time, NTP corrects at wake), it scheduled fixed-delay retries
  (120/300/600 s) that misalign with the window and after 3 misses abandoned it. Fix: the before-window
  case clears the followup retry so the pre-sleep timer re-arm re-targets the real window start (start-15)
  with the now-synced clock; converges in one cycle. (After-window + the separate `g_next_ota_epoch`
  path unchanged.)
- `821645c` — **#1: the Sense LCD-OTA proxy now requires `sha_match && ota_ok` for success** (was
  `sha_match` alone). Rare gap: SHA verifies but the LCD's `esp_ota_set_boot_partition` fails → LCD stays
  on old fw (doesn't reboot) yet the Sense reported success + self-OTA'd → silent board divergence. The
  LCD already sent `ota_ok` in `LCD_OTA_END_ACK`; the Sense just ignored it. New verdict `lcd_boot_part_fail`.
- `171b03f`/`db45fd5` — soak harness: arm-tap retry + wall-clock deadline + the Sense-UART LCD read above.
- `b9cc200` + `0ef7e39` — **Scheduled-OTA cancel safety.** The device re-validates the live schedule at
  window-start (`ota_sched_revalidate()`) before locking the OTA and aborts if pulled. The `/ota/schedule`
  GET is FUTURE-only, so a live enabled window whose start already passed returns `204` (same as deleted)
  → **204 is FAIL-OPEN** (proceed), else a legit scheduled OTA would falsely abort. **Cancel = set
  `enabled=false`** (returns HTTP 200 `{enabled:false}` at any wake time → reliably aborts as
  `schedule_cancelled`); a `request_id` change → `schedule_replaced`. Operational rule: to pull a release,
  set `enabled=false`; do NOT delete the row.

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

**Device-id uniqueness:** generated from the Sense board's hardware MAC — `load_runtime_device_id()`
(`Sense_Minimal.ino:347`) does `esp_read_mac(mac, ESP_MAC_ETH)` → `halo-%02x%02x-%02x%02x` over
`mac[2..5]`. Unique per chip by construction (each device's Sense has a distinct factory MAC), so devices
never share an id; always use the **Sense** id for cloud ops (the LCD derives its own from its MAC and is
provisioning-only). **Pre-scale caveat:** it uses only 4 MAC bytes = 32-bit space → birthday-bound ~50%
collision chance across a fleet around ~77k units. Fine now; widen the id (include `mac[1]` or hash all 6
bytes) before scaling to tens of thousands.

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
`TrepoOtaSchedules-dev` → actuator tap, **retried until the Sense actually fetches** (`OtaScheduleApiFunction`
log hit) → wait for the window (device auto-wakes ~grace_before before `start_epoch`) → poll cloud for
`last_fw==target` (Sense) → **verify the LCD via `read_lcd_version()` = the Sense-UART `INPUT_FW_INFO`
gold-standard read, passing only on `running_state=VALID`** (NEVER opens port 101 — see §4). Run:
`python3 automation/scheduled_ota_soak.py <base_ver> <n_cycles> [deadline_min]`. STOP sentinel
`automation/sched_STOP`; results in `automation/scheduled_ota_results.jsonl`.
- **CONSUMED-WINDOW COOLDOWN (~20 min):** after a successful OTA the device declines to re-OTA for ~20 min
  (`ota=up_to_date`, `sched_api_hits=1`, never wakes at the too-soon window). The soak fires windows every
  ~15 min, so **every other cycle is cooldown-blocked** — this shows up as a FAIL but is the device behaving
  correctly (real schedules are hours/days apart). For a clean pass-rate, **space windows >20 min apart**.
  A clean 3 h run climbed the device through 5 consecutive versions, both boards VALID, no skips — every
  OTA the cooldown permitted succeeded.

### Reading device state / breadcrumbs (only when NOT mid-OTA)
- Sense USB `errors` → `sense_errlog_dump` (the `ota_orch` breadcrumb trail, NVS black box).
- Sense USB `fw` / LCD USB `fw` → real running version + partition/state.
- The `[TRUTH]` line dumps `ota_en`, `reboot_loop`, `why`, `next_ota_epoch`, etc.

---

## 4. Critical gotchas
- **A DEFAULT-open USB-CDC port RESETS the ESP32-S3** (`rst:0x15 USB_UART_CHIP_RESET`) — `serial.Serial(port,baud)`
  opens before you can deassert the lines, so it asserts DTR → reset. **Never default-open / live-monitor
  serial during an OTA.**
- **CORRECTED (2026-06-03): you CAN open non-destructively** with a DEFERRED open and DTR/RTS off first:
  `s=serial.Serial(); s.port=p; s.baudrate=115200; s.dtr=False; s.rts=False; s.open()`. Verified on BOTH
  boards — reads the live version without resetting. **BUT** the LCD's OWN port (`usbmodem101`) STILL
  resets intermittently even this way (rolls back an in-flight `PENDING_VERIFY` image). **Gold-standard
  LCD read = query it THROUGH the Sense, never open port 101:** send `{"type":"INPUT_FW_INFO"}` to the
  Sense (deferred/DTR-off open) → Sense UART-queries the LCD → returns `sense_fw`, `lcd_fw`,
  `lcd_running_part`, `lcd_running_state` (NEW/PENDING_VERIFY/VALID/…). LCD must be awake (tap first).
  This is the only trustworthy way to confirm the LCD version/finalize state. (`/tmp/fwinfo4.py` pattern.)
- **LCD finalize is correct & prompt** (proven via the running_state probe, both manual + scheduled):
  `mark_valid` runs unconditionally in LCD `setup()` (`LCD_Minimal.ino:3610`, after LVGL init, before
  loop/sleep), so the LCD reaches VALID within ~seconds of its post-OTA boot. The only rollback trigger
  is an external reset during that brief PENDING_VERIFY window — which only a USB-port open causes; nothing
  in the field does. So a "one behind" LCD reading from a port-101 tool is a MEASUREMENT artifact.
- **The LCD can only wake via touch/encoder/timer, never UART** — hence the actuator and the cold-link
  rendezvous issue for scheduled OTA. A proxy `lcd_query_fail` (LCD asleep, quick) lets the Sense self-OTA
  proceed; a proxy `timeout` (LCD awake but transfer fails) BLOCKS the Sense self-OTA.
- **No-OTA recovery (USB reflash):** if a device can't OTA out of bad firmware, app-only reflash the Sense:
  `python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 write_flash <off> <bin>`.
  **Flash BOTH partitions** — `ota_0`=`0x10000`, `ota_1`=`0x1F0000` (Sense `partitions.csv`) — because the
  device may be booting from `ota_1`; a `0x10000`-only flash won't change the running fw. App-only preserves
  NVS/WiFi (no re-provision). Tap to wake first so the port enumerates. Then a manual OTA syncs the LCD.
- **Cloud LCD field lag:** right after a scheduled OTA the cloud may show `last_lcd_fw=unknown` /
  `last_lcd_ota_result=lcd_query_fail` (Sense couldn't re-query the asleep LCD); it **self-corrects**
  to the real version on the next wake-with-both-awake. Ground-truth with LCD USB `fw` if needed.
- Always `publish_both.sh`; always fresh-compile; flash LCD app-only (`esptool write_flash 0x10000`)
  to preserve NVS/WiFi creds; Sense via `arduino-cli upload --input-dir` (also NVS-safe).
- A device stuck at `ota_en=0` (pre-fix) can only be updated via **manual OTA** (bypasses the guard);
  the fix then self-heals it on first clean boot.

---

## 5. Known follow-up (optional) + #5 "what NOT to do"

**#5 cold-link rendezvous — UNSOLVED, and the obvious fix is WRONG.** A *scheduled* OTA can leave the
boards split for one cycle: the Sense wakes on its timer, the LCD is asleep, the proxy `lcd_query_fail`s,
the Sense self-OTAs alone → LCD one version behind. It **self-corrects on the next OTA / any user
interaction** (both boards awake), so it's the *least* important gap.

**Do NOT "fix" it with a Sense-side wake-gate** (we tried — `f939b2d`, reverted `ef12156`). Root reason:
**the Sense physically cannot wake the LCD** (GPIO39 is LCD→Sense only; the LCD ignores UART in deep
sleep). The LCD self-wakes only via its own MAINT_WINDOW RTC timer, which is the unreliable part — and
no amount of Sense-side waiting/retrying can cause a self-wake. Worse, the wake-gate disrupted the
working proxy (`timeout` → blocks Sense self-OTA) and the `lcd_ota_due` self-heal deadlocked the device
(every maintenance wake hijacked to retry the broken proxy, never self-OTAing). Recovery required a USB
reflash of both Sense partitions. **Any real fix must be LCD-side** (make the LCD's MAINT_WINDOW self-wake
reliable + stay awake through the window) and must be proven on a bench/test unit BEFORE deploying to the
only device. `docs/OTA_HARDENING_TODO.md` has the older idempotent-BEGIN-re-ACK idea — also a coordinated
two-board change; same "prove it first" rule applies.

**HARD RULE learned this session:** never deploy an unproven OTA-PATH firmware change to the only device.
The OTA path is the recovery path — break it and you can't roll back over the air.

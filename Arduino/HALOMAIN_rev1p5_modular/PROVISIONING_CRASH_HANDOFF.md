# Provisioning Crash Handoff — Sense PANICs on home-WiFi connect (TLS AES-DMA abort)

**Status:** ✅ RESOLVED 2026-06-24 — fixed in commit `86e13eb`, published **v6.1.815** (channel-latest). Fix: (1) gate the AWAKE_SCHED fetch with `!halo_provisioning_active()`; (2) `ScopedTlsDmaReserve` RAII around the TLS in `ota_sched_http_fetch_window()` + `ota_report_post()`. Validated on `halo-d45b-7fd9`: provisioned with 0 aborts, owner claimed (http=200), `provisioned:true`, QR cleared. NOTE: the OTA-schedule fetch predates 814 (since 6.1.776); 814/`c323e80` added a 2nd unguarded TLS call (the ota_complete report) in the same block, now guarded too. The detail below is kept for history.
**Found:** 2026-06-24, captured live on device `halo-d45b-7fd9`, fw **6.1.814** (`6.1.814-Jun 24 2026-14:49:34-c323e80`).
**Full serial capture:** `/tmp/prov_capture.log` (dual Sense+LCD, dtr/rts off).

---

## TL;DR

During provisioning, the moment the Sense connects to the home WiFi it fires an **OTA-schedule HTTPS fetch**. The TLS handshake needs hardware **AES-DMA**, but DMA-capable internal RAM is exhausted (SoftAP still up **+** the camera's 16 KB DMA reservation is held), so `esp_aes_process_dma` hits an `abort()` → **ESP_RST_PANIC** → reboot. The device reboots back to `ap_setup` **before** it can mark itself provisioned or claim the owner, so:

- the iOS app shows **"something went wrong"**,
- the device keeps **showing the QR** (still unprovisioned),
- `owner_id` is never set.

This is the same **esp-aes DMA conflict** family that commit **809** ("free camera DMA reserve around owner-claim TLS so it succeeds") fixed for the **owner-claim** TLS — but the **OTA-schedule fetch that runs during provisioning is NOT guarded**.

---

## User-facing symptom

App scans QR → joins device SoftAP → scans home networks → user submits home SSID + password → app: **"something went wrong"** within a few seconds → device is still showing the QR (provisioning never completes). `owner_id` stays empty, so later uploads would fail "Owner not set".

> Note: an *earlier* "can't join the device WiFi in ~3s" symptom was a separate stale-QR/password issue (the AP password regenerates each session) and is NOT this bug. This crash is the next failure downstream, after the join + scan succeed.

---

## Reproduction

1. Unprovisioned device in setup mode (SoftAP `Trepo-Halo-XXXX`, QR on LCD).
2. App joins SoftAP, `GET /info`, `GET /scan`, then `POST /wifi {ssid, password, owner_code}`.
3. Device connects to home WiFi → **panics within ~1 s** → reboots to `ap_setup`.

Reliable. (The reboot is a genuine `ESP_RST_PANIC` from `abort()`, NOT a USB-CDC-open reset.)

---

## Evidence (from `/tmp/prov_capture.log`, Sense)

```
[HTTP] POST /wifi client=halo_app app_session=1 app_ver=1.6
[PROVISION] Saved owner_code=4GTQ
[HTTP] Received credentials: ssid=Garage Member, password_len=8
[PROVISION] Starting home Wi-Fi connection: ssid=Garage Member
[PROVISION] State changed to: connecting
[WIFI_GUARD] WiFi.begin called - STA marked as started
...
[WIFI_EVENT] event=115 status=3            <- WL_CONNECTED
[WIFI_GUARD] state=connected truth=connected status=3 reason=loop_status_connected
[PROVISION] Home Wi-Fi connected: IP=192.168.1.22 RSSI=-67 mode=3 (verifying stability...)
[TLS_GUARD] SNTP init (early on connect) reason=sntp_start
[AWAKE_SCHED] fetch awake_ms=213073 fetch_age_s=-1
[OTA_HTTP_SCHED] fetch url=https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com/ota/schedule?device_id=halo-d45b-7fd9&board=sense&fw=6.1.814&...&channel=dev&device_type=sense timeout_ms=5000
[OTA_HTTP_SCHED] using crt bundle
abort() was called at PC 0x4037b6cf on core 1
Backtrace: 0x40381c6d ... 0x4037b6cf ...
Rebooting...
rst:0xc (RTC_SW_CPU_RST)
[BOOT] reset_reason=ESP_RST_PANIC
[PROVISION] NVS init: provisioned=0, state=connecting   <- back to square one
[PROVISION] Starting Setup Mode...
[UPLOAD_PERSIST] panic_backoff until_ms=61647 reset_reason=ESP_RST_PANIC
[BOOT] crash reset=PANIC recorded for reboot-loop guard
```

### Symbolized backtrace (addr2line, ELF c323e80)

```
esp_aes_crypt_ecb            (esp-idf .../mbedtls/port/aes/dma/esp_aes.c:173)
  -> esp_aes_process_dma_ext_ram (.../aes/dma/esp_aes_dma_core.c:295)
     -> esp_aes_process_dma      (.../aes/dma/esp_aes_dma_core.c:1077)  -> abort()
```

The abort is the **esp-aes DMA path failing to obtain its DMA buffer** (DMA-capable internal RAM exhausted) during the TLS handshake of the OTA-schedule fetch.

---

## Root cause

- The **awake-path OTA-schedule HTTPS fetch** (`ota_sched_http_fetch_window()`, invoked from the awake-path block in `halo_prod_loop()` in `halo_sense_prod.ino`, and the SNTP/`[TLS_GUARD]` + `[OTA_HTTP_SCHED]` path) runs the **instant WiFi connects** — and it runs **during provisioning**, while:
  - the **SoftAP is still up** (AP+STA, `mode=3`), consuming DMA/heap, and
  - the **camera 16 KB DMA reservation** is held.
- mbedTLS AES hardware acceleration (`esp_aes_*_dma`) can't allocate its DMA descriptor/buffer in that pressured state and `abort()`s (ESP_ERROR_CHECK), panicking the chip.
- This is the same hazard commit **809** fixed for the **owner-claim** TLS by releasing the camera DMA reserve (the `DmaGuard` / free-camera-DMA-reserve guard). The **OTA-schedule fetch path was not given the same guard**, and additionally it should not run at all mid-provisioning.

**Likely-suspect commit:** `c323e80` (the 6.1.814 update — "add window_armed + ota_complete cloud telemetry") touches the OTA schedule/telemetry path; confirm whether it introduced or exposed this awake-path fetch firing during provisioning.

---

## Fix direction (pick one; (a) is simplest/safest)

**(a) Don't run the awake-path OTA-schedule fetch while provisioning/setup is active.**
The awake-path fetch in `halo_prod_loop()` already has a gate list (been-awake>20s, throttle, `wifi_is_connected`, `is_time_valid`, `!sense_action_inflight`, `!g_lcd_ota_task_running`, `!g_maintenance_mode`). Add a provisioning guard, e.g. require `provisioned && !setup_mode` (or `prov_state == connected`, i.e. NOT `connecting`/`ap_setup`). During provisioning there is no reason to fetch an OTA schedule.

**(b) Wrap the OTA-schedule TLS fetch in the same DMA guard as owner-claim.**
Apply the `DmaGuard` / free-camera-16KB-DMA-reserve used on the other TLS paths (per the "esp-aes DMA conflict — RAII DmaGuard on all 4 TLS paths" work and commit 809) around `ota_sched_http_fetch_window()` so AES-DMA can allocate.

Recommended: do **(a)** (eliminates the work entirely during provisioning) and audit that **all** TLS call sites reachable during provisioning (owner-claim, SNTP, OTA-schedule, any MQTT/TLS) are DMA-guarded — option (b) — so a stray TLS call during the SoftAP-up window can't panic again.

### Also
- The panic trips the **reboot-loop guard** (`panic_backoff`, ~60 s). Repeated provisioning attempts compound it — verify the guard doesn't latch the device into a bad state after a few failed provisioning attempts.

---

## Verify the fix

Re-run provisioning with a serial capture on the Sense (`/dev/cu.usbmodem1101`, 115200). Expected after fix:
- `Home Wi-Fi connected … (verifying stability...)` → stays up (no abort/reboot),
- `[CLAIM]` owner-code claim runs and sets `owner_id` (claim Lambda `POST …/v1/provisioning/claim`),
- `/status` eventually returns `wifi_state:connected`, `provisioned:true`, `owner_id_set:true`,
- device LEAVES setup mode (QR disappears), app shows success.

> Capture gotcha: opening the ESP32-S3 USB-CDC port triggers `USB_UART_CHIP_RESET` once on open. Open the monitor BEFORE starting provisioning and keep it open — it won't reset again while held.

---

## Pointers

- Provisioning protocol/state machine: `PROVISIONING_HANDOFF.md`
- Provisioning impl: `halo_ota_demo/firmware/halo_sense_prod/ProvisioningManager.{cpp,h}` (+ `shared/`)
- Awake-path OTA-schedule fetch + `ota_sched_http_fetch_window()`: `halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino` (search `AWAKE_SCHED` / `OTA_HTTP_SCHED` / `ota_sched_http_fetch_window`)
- DMA guard precedent: commit `809` (bc2b759) "free camera DMA reserve around owner-claim TLS"; search `DmaGuard` / camera DMA reserve.
- Full crash log: `/tmp/prov_capture.log`

# ⚠️ Serial monitoring resets the boards — never live-monitor during an OTA

## The problem
Opening or reconnecting a USB-CDC serial port on a HALO ESP32-S3 board (Sense `1101`,
LCD `101`) triggers a **hardware reset**:

```
rst:0x15 (USB_UART_CHIP_RESET)
```

This happens **even with `dtr=False/rts=False`** set before open — the host re-attaching to
the ESP32-S3 USB-Serial/JTAG peripheral resets the chip. Critically, a board's port
**re-enumerates every time it wakes from deep sleep**, so an auto-reconnecting monitor will
reset the board on each wake.

## Why it breaks OTAs
If a reconnect-reset lands during an active OTA, it corrupts it:
- Resetting the **Sense** mid-OTA-decision drops the manual request / aborts the proxy.
- Resetting the **LCD** when the Sense queries it (post-self-OTA) → `lcd_query_fail`, so the
  LCD half never updates (Sense ends on the new version, LCD stuck on the old one).

Observed twice (2026-06-01): a real OTA-button press failed both when the Sense serial was
monitored (Sense reset on wake) and when only the LCD was monitored (LCD reset →
`lcd_query_fail`). **With no serial monitoring, the same button OTA updated both boards
cleanly.**

## Why the test-harness soak still worked (9/10)
`automation/ota_loop_test.py` cold-wakes with a tap (so the reset happens **then**, before
triggering) and then stays **continuously connected** through a clean OTA — the device stays
awake during the transfer, so there's no reconnect-reset mid-OTA. The danger is reconnect-
resets that **coincide** with the OTA (a user-initiated OTA where wake + reconnect + press
interleave).

## How to verify an OTA WITHOUT touching serial
Read the cloud OTA report (the Sense HTTP-reports `fw`/`lcd_fw` on `pre_sleep`):

```bash
aws dynamodb get-item --table-name TrepoOtaDeviceLatest-dev \
  --key '{"device_id":{"S":"halo-d45b-8295"}}' \
  --profile trepo-dev --region us-east-1
# -> last_fw, last_lcd_fw, last_lcd_ota_result
```

For live device-side detail, use the on-device **NVS error log read AFTER** the OTA, or a
**passive UART tap / logic analyzer** on the Sense↔LCD line. The USB-CDC monitor fundamentally
cannot observe an OTA without resetting the board.

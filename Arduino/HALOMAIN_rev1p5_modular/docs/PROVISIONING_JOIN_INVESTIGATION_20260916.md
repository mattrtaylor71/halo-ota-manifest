# Provisioning join investigation — September 16, 2026

The installed pair is still private169. The source changes below are descendants
of the built but uninstalled171 candidate; they are not built, installed or
published. Use unused172+ for new firmware bytes. Preserve all171 media-retry
changes and the169 fallback banks.

## Confirmed icon defect and source fix

The user's IMG_8787 photo matches the source: step2 created the gold profile badge
but never drew a person in it. The badge now draws a head and shoulders using the
existing dark ink, without another object, font, image buffer or layout change.
The actual LVGL8.3.6 software-render test passes: visible silhouette, unchanged
border/layout, zero child objects and no retained allocation over50 redraws.
The old empty-badge negative control fails. This is not panel qualification.

## Current phone-join evidence

The user saw the exact setup SSID advertised by Sense169 and re-scanned the
current QR. The iOS system alert named that same network and said it could not
join. A direct iPhone Settings join using the password captured at this session's
startup returned "Incorrect password." This bypasses the app but does not prove
which side rejected authentication or that the live driver configuration still
matches the startup password.

Passive capture003 spans the reproduction. Sense continued reporting ap_setup;
no AP-client connect/IP event was captured. Normal STA disconnected diagnostics
are not evidence that the setup AP itself is down. No board reset, firmware
installation, Wi-Fi scan or radio test command was used for that capture.

The installed169 sender writes the original JSON to UART before redacting a
separate copy for USB logging. LCD password buffers are65 bytes; generated
passwords are12 alphanumeric characters. No sender/cache/parser mismatch was
found. A separate host campaign exercised the real generator and148px LVGL QR:
Apple Vision decoded32/32 exactly and the actual Swift parser preserved all32
SSID/password pairs. OpenCV missed one code but decoded no incorrect payload.
These are synthetic software pixels, not a decode of the actual device screen.

After preserving the failed session, one INPUT_RESET_WIFI was sent through the
LCD, using the existing setup-reset path on the already-unprovisioned unit.
Capture004 records AP teardown, fresh credentials and successful AP startup.
This also clears setup identity fields by the existing reset contract; they were
already unset. It is a recovery experiment, not proof of a fix. Phone association
and full setup after this fresh session remain to be established.

## Separate reproduced ownership gap

An old normal STA connection attempt could survive entry into provisioning.
When its guard deadline expired, wifi_guard_poll could scan or force STA-only,
disrupting the setup AP. The poll now retires that obsolete owner and returns
while provisioning owns the radio. It does not change credentials, radio mode,
failure counters or provisioning's own connection attempt.

The actual-function regression passes1543 checks across status/deadline/rollover
and ownership boundaries; frozen171 fails553 of them. Four existing Wi-Fi/media
network suites pass. This conditional defect has not been established as the
cause of the reported join failure: observed169 logs show inflight=0.

## Private evidence

Raw captures and startup logs can contain credentials; keep them local and out
of release archives/publication. Evidence root:
`/Users/MattTaylor/halo-provisioning-review-2026-09-16/join-failure-20260916`.

- `profile-icon-render001/RESULT.json`: actual badge render and negative control.
- `qr-roundtrip001/run002/RESULT.json`:32-case QR/Apple decoder/Swift parser proof.
- `capture001` and `capture002`: pre-retry diagnostics; the guide bypasses the old
  QR widget's plaintext logging, so these do not recover the actual QR password.
- `capture003`: original phone/direct-join failure capture, closed normally.
- `capture004`: fresh setup-network recovery experiment.
- `/Users/MattTaylor/halo-wifi-recovery-2026-09-16/provisioning169-review/OWNER-FIX-REVIEW.json`:
  ownership reproducer, negative control and four unchanged regression suites.

Root also reran test_lcd_provision_flow.py and
test_provisioning_display_status.py successfully. No new device acceptance or
public OTA release is claimed.

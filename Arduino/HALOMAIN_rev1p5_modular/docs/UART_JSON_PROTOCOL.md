# HALO Production UART JSON Protocol

## Status

- This document describes the actual production Sense↔LCD UART contract in the current firmware.
- Transport is newline-delimited JSON over UART1 at 115200 baud.
- `halo_ota_demo/firmware/shared/UartJsonProtocol.h` is the shared source of truth for the production envelope fields and message-name constants.
- `halo_ota_demo/firmware/shared/UartProto.h` is legacy framed-protocol documentation and is not the production wire format.

## Required Envelope

Every production message must contain:

- `ver`: protocol version, currently `1`
- `type`: message name string
- `msg_id`: sender-generated message ID
- `ts`: sender timestamp from `millis()`

Additional fields are message-specific payload.

## Transport

- One JSON object per UART line
- Sender appends `\n`
- Receiver accumulates until newline, then deserializes JSON
- Production firmware validates the envelope before acting on the message

## Common Boot / Link Handshake

### LCD -> Sense

- `SYNC`
- `INPUT_PING`

### Sense -> LCD

- `SYNC_ACK`
- `PONG`
- `LINK_HB`

Current behavior:

1. LCD sends `SYNC` when link sync is pending.
2. Sense marks link synced and replies with `SYNC_ACK`.
3. LCD uses `INPUT_PING` and Sense replies with `PONG`.
4. Sense also emits `LINK_HB` periodically as a proof-of-life heartbeat.

## Common Coordinated Sleep Messages

### LCD -> Sense

- `INPUT_SLEEP`
- `SLEEP_DENY`

### Sense -> LCD

- `SLEEP_READY`
- `SENSE_SLEEP_INTENT`
- `RELEASE_WAKE`
- legacy-accepted by LCD: `SLEEP_ACK`, `INPUT_SLEEP_ACK`, `SLEEP_BUSY`

## Common Operational Message Families

### LCD -> Sense

- Input and command messages:
  - `INPUT_WAKE`
  - `INPUT_SCROLL`
  - `INPUT_TOUCH`
  - `INPUT_LONG_PRESS_START`
  - `INPUT_LONG_PRESS_END`
  - `INPUT_DELETE`
  - `INPUT_MENU_PRESS`
  - `INPUT_MENU_SELECT`
  - `INPUT_EXPIRY_DATE`
  - `INPUT_DISCARD_OPTIONS`
  - `INPUT_RETRY`
  - `INPUT_RESET_WIFI`
  - `INPUT_FW_INFO`
  - `INPUT_OTA_CHECK`
  - `LIST_ACTIVE` — `{"state":1|0}`. Sent while the user is on the shopping-list
    screen: `state=1` = keep Sense awake + WiFi up; `state=0` = user left / allow
    sleep. Sent on enter/leave (via `ui_show_screen`), on LCD idle-sleep entry if
    still on the list, and re-asserted ~every 3s from `loop()` (Sense auto-clears
    the flag after ~30s of silence).
- Link / status / ack messages:
  - `MAINT_WINDOW_ACK`
  - `WIFI_STATUS`
  - `WIFI_CREDS_ACK`
  - `WIFI_ON_ACK`
  - `OTA_CHECK_ACK`
  - `OTA_CHECK_RESULT`
  - `LCD_OTA_DONE`
  - `LCD_DIAG`

### Sense -> LCD

- UI / status messages:
  - `UI_LIST`
  - `UI_STATUS`
  - `UI_MEAL_RESULT`
  - `UI_TOAST`
  - `UI_VOICE_RESPONSE`
  - legacy/compatibility still handled in LCD code:
    - `UI_VOICE_ITEMS`
- Device / maintenance / provisioning messages:
  - `SENSE_DIAG`
  - `FW_INFO`
  - `STATUS_SYNC`
  - `MAINT_WINDOW`
  - `PROVISION_QR`
  - `PROVISION_STATUS`
- Wi-Fi / OTA coordination:
  - `WIFI_CREDS`
  - `WIFI_ON`
  - `OTA_LOCK`
  - `OTA_UNLOCK`
  - `OTA_CHECK`
  - `SENSE_OTA_ACTIVE`
  - `SENSE_OTA_IDLE`
  - legacy/compatibility still handled in LCD code:
    - `OTA_APPLY_REQUIRED`

## Notes

- This contract is intentionally conservative. It documents what production code currently sends and accepts, not an idealized future protocol.
- Any future protocol cleanup should preserve these message names and envelope fields until both firmware targets are migrated together.

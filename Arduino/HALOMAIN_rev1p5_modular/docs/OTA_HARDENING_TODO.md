# OTA hardening recommendation (vetted, for SUPERVISED implementation)

Status: NOT YET IMPLEMENTED. Surfaced by the overnight soak (2026-06-02). Needs a
coordinated two-board change + attended OTA test — do not land unsupervised.

> ⚠️ **2026-06-03: a Sense-side "wake-gate" attempt at this FAILED and bricked OTA (commit `f939b2d`,
> reverted `ef12156`; device recovered by USB reflash).** Root reason: **the Sense cannot wake the LCD**
> (GPIO39 is LCD→Sense only). Waiting/retrying on the Sense for an LCD self-wake it can't cause does not
> help, and it regressed the working proxy (`timeout` → blocks Sense self-OTA) and deadlocked via a stuck
> `lcd_ota_due` retry loop. **Any real fix must be LCD-SIDE** (reliable MAINT_WINDOW self-wake + stay
> awake through the window) and MUST be proven on a bench/test unit first. Do NOT deploy unproven OTA-path
> changes to the only device. See `docs/OTA_FIXES_AND_TEST_SETUP.md` §5.

## Observation
Overnight soak (button-free USB-inject trigger) passes ~8/9 dual-board OTA cycles.
The occasional failure is always the same signature:

    lcd_query_ok lcd_fw=<old>          (LCD reachable — query succeeds, retried 3x)
    lcd_proxy_start ver=<new>
    lcd_proxy_done res=timeout          (~10s == LCD_OTA_PROXY_BEGIN_TIMEOUT_MS)

i.e. the proxy's single `LCD_OTA_BEGIN` is missed (or its `LCD_OTA_BEGIN_ACK` lost)
and the BEGIN wait times out. Context on the failures: `link_recent=0` (the
Sense<->LCD UART link was cold at OTA start). This is induced by the soak's trigger,
which opens the Sense USB port (USB_UART_CHIP_RESET -> fresh Sense boot) every cycle;
the real "Software Update" button does NOT reset the Sense, so the link stays warm.
The failure self-corrects: the next cycle re-OTAs the lagging LCD.

So: not a field bug, but the BEGIN handshake is less resilient than the QUERY (which
is retried 3x). Worth hardening.

## Why a naive BEGIN-resend is WRONG
LCD `lcd_ota_handle_begin()` (LCD_Minimal/lcd_ota_uart.h:421-432): if the LCD already
accepted a BEGIN (s_lcd_ota_state != LCD_OTA_IDLE) a second BEGIN is REJECTED with
`accepted=false, reason="already_active"` (no resume_offset). If the Sense resends
BEGIN after the LCD accepted the first (but the ACK was lost), the Sense would see the
reject and abort -> regression.

## Recommended fix (coordinated, both boards)
1. LCD `lcd_ota_handle_begin()`: make BEGIN idempotent. If a BEGIN arrives with the
   SAME session_id as the active session, RE-ACK with `accepted=true` and the current
   `s_lcd_ota_bytes_written` as `resume_offset` (instead of rejecting "already_active").
   Only reject when a DIFFERENT session_id arrives mid-session.
2. Sense `sense_lcd_ota_proxy()` (Sense_Minimal/sense_ota_lcd.h ~286-324): wrap the
   BEGIN send + BEGIN_ACK wait in a retry loop (e.g. 3 attempts, ~3-4s each, same
   session_id each time, same ~10-12s total budget). On accepted=true proceed (honor
   resume_offset); only treat a DIFFERENT-session reject as fatal. The wait already
   pumps UART (BEGIN_ACK pump fix, commit 3ae6d68) so it works inline.
3. Optionally send OTA_LOCK to the LCD immediately on the manual request AND keep it
   re-armed until BEGIN, so the LCD can't sleep between QUERY and BEGIN.

## Test
Re-run the overnight soak (automation/overnight_ota_soak.py) and confirm the
link_recent=0 cycles now pass on the first attempt (no res=timeout). Attended, with
cloud + breadcrumb verification, per the OTA test discipline.

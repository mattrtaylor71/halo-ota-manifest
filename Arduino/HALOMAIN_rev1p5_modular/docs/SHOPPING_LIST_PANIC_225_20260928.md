# Retained LCD 225 shopping-list panic — September 28, 2026

Matt reported ineffective shopping-list refresh feedback, followed by a freeze,
black screen and Home after Home → Settings → Shopping List. The observed unit
was still on private 225. All original evidence was preserved before installing
the already-built private 226 correction. Production remains 224.

## Recovered evidence

The exact bench device `halo-d45c-d3f1` reported one successful Sense-side list
fetch in 1,771 ms during boot 242, received by the server at 16:16:23 PDT. There
were zero reported list-fetch failures. This establishes a reported network
fetch outcome, not LCD receipt or rendering. Device time was 115 seconds ahead
of receipt; use host/server times for correlation.

A later actuator wake captured both 225/app0 images with valid running state.
LCD Home was responsive and held nine cached list items. Its current LVGL pool
had 79,560 B free/largest 78,900 B. Those are recovery measurements, not original
crash-time headroom. Sense boot 243 and its 16:20:46 cloud report came from this
diagnostic wake; they are not evidence of an unexplained wake.

The complete 64 KiB LCD coredump partition was then read through the qualified
physical transport. Exact MAC, chip/security state and canonical partition
table were checked. Only the partition table and coredump region were read;
there were no flash writes or clears. The official release returned 225/Home.
Its resulting USB-reset boot belongs to this diagnostic action, not the incident.

| Evidence | Value |
| --- | --- |
| Raw partition SHA256 | `d4bbfd527044e21e7f93e48b5fd5dce6aaf4e08fa93bf23b10b15d7138ab1af1` |
| Encoded dump length / CRC32 | 18,436 B / `0x3b7f6139`, verified |
| Actual ELF prefix stored in dump | `e1116f376` (nine hexadecimal characters) |
| Verified analysis ELF SHA256 | `e1116f3761663a62bec1796d19b23e17d0c80465d380e7d576bf9143d5661061` |
| Task / exception | `loopTask` / `LoadProhibited` (28) |
| PC / fault address | `0x4203acce` / `0x84` |

The recorded PC lies 14 bytes into `_lv_event_mark_deleted`, independently
verified against the exact ELF symbol table. The saved call chain is:

`loop → ship_menu_send_action → show_shopping_list_screen → ui_show_screen → show_shopping_list_screen_impl → lv_obj_del → obj_del_core → _lv_obj_destruct → lv_obj_destructor → _lv_event_mark_deleted`.

The exact 225 source binds this to the physical touch-release path at
`LCD_Minimal.ino:5097`, Shopping List menu action at `lcd_ship_action.h:364`, and
deletion of the previous list screen at `lcd_ship_screens.h:2341`. GDB and the
static symbol range agree; a conflicting ArduinoJson mapping from addr2line is
retained but is not used for attribution.

This establishes a real retained 225 LCD event-deletion panic on the reported
navigation path. The dump has no trustworthy event timestamp. It does not prove
its exact age, the preceding thread interleaving, a double-delete, memory
exhaustion or an upload failure.

## Correction and diagnostic limits

225 retained an unlocked call to `lv_timer_handler()` from the UART-owned
refresh transition. Concurrent event dispatch can corrupt LVGL's event chain;
the earlier September 24 incident failed in the same deletion path. 226 removes
that unauthorized rendering entry and retains drawing under the UI owner lock.
It also reveals refresh feedback when a user joins an already-running silent
refresh, and disables touch/encoder haptics. See
[the 226 source/build handoff](LCD_REFRESH_HAPTICS_226.md).

Legacy LCD error-log, Sense error-log and wake-log NVS writers are retired when
`HALO_DURABLE_DIAGNOSTICS=1`. Their read commands can return stale records,
missing indices or no response; none establishes absence of a new crash.
`resetreason` describes only the current boot. The durable OTA journal is not a
general application crash history. In contrast, the actual compiled 225 SDK
enables flash ELF core dumps with CRC32 at LCD offset `0x510000`, size `0x10000`.
Preserve that entire region before intentionally reproducing a crash or changing
firmware. Decode privately against the matching ELF; raw memory can contain
credentials or user data and must not be committed.

## Private 226 device acceptance

Both boards are now installed on private 226/app1, with fresh valid-image
identity checks. The installation verified the new images and preserved the
225/app0 fallback, NVS, partition tables and filesystem regions. This was a
private USB installation, not an OTA transfer test.

Case 25 passed three native Home/List handler rebuilds, with fresh UI-alive and
list-state responses each time, no observed panic or reboot, and normal paired
sleep. Haptic register readbacks before and after were MODE=64, GO=0, RTP=0,
standby_verified=1. This verifies disabled driver state, not a vibration sensor.

Case 18 stopped before media or refresh actions because its capture missed the
early Wi-Fi connection event required by the admission check. Its failure is
retained. Case 26 started capture before the wake and passed admission. During
one actual 104,448-byte voice POST, two user-refresh joins changed the observed
indicator state from hidden to visible without restarting the existing request.
The list subsequently reported REFRESH_COMPLETE and IDLE, with no observed LCD
panic. However, the voice POST returned read timeout -11 after about five
seconds. The device saved the audio and armed a 300-second retry. Case 26 remains
FAIL_OR_INCOMPLETE: it did not observe the required voice 202 acknowledgement.
Its final haptics and ring-hidden queries were never sent; do not infer them.
A separate command-free case 27 observed the native timer wake and retry of
the same saved job/session/byte count. The retry received HTTP 202, deleted the
saved file successfully, reported an empty spool, and cleared the retry hint
and peer arm. Both boards returned to sleep. LCD startup and heartbeat logs
kept backlight/panel off throughout this retry; this is telemetry, not a camera
measurement. Case 26 remains failed despite subsequent recovery.

Read-only cloud correlation found the initial request completed successfully:
acceptance was about 305 ms after the device timeout. The cloud and device
receipts explain why a retry was needed even though processing had completed.
The cloud snapshot preceded the retry and does not qualify post-retry worker
counts. No media content or semantic correctness was evaluated.

The original 225 capture and dump remain intact. USB Home/List commands exercise
the native UI handler and screen rebuild, but bypass the physical Settings
hit-test and `loopTask` touch handler. Automated navigation does not establish
physical gesture or pixel/animation correctness. No full-product or scheduled
OTA pass is claimed.

## Evidence locations

- Investigation: `/Users/MattTaylor/halo-shopping-incident-20260928/`.
- Original USB capture: `/Users/MattTaylor/halo-ram-campaign-20260928/bench-private/incident225-freeze-20260928-19/`.
- Original dump: `/Users/MattTaylor/halo-ram-campaign-20260928/bench-private/incident225-coredump-20260928-20/`.
- Private installation and tests: cases `followup226-sense-service-20260928-16`,
  `followup226-lcd-service-20260928-17`, `followup226-reentry-20260928-25` and
  `followup226-refresh-completion-20260928-26` under the same bench-private root.
- Parent readback: `/Users/MattTaylor/halo-shopping-incident-20260928/PRIVATE226-READBACK.json`.
- Exact-session cloud correlation: `/Users/MattTaylor/halo-shopping-incident-20260928/cloud226-voice26/`.
- Independent source and crash attribution: `/Users/MattTaylor/halo-refresh-haptics-20260928/source-audit/INCIDENT225-CRASH-ATTRIBUTION.md`.

Shared context consulted: `engineering/halo-resource-testing`. Its dated
measurements are not substituted for this incident's evidence. No company-brain
save or production publication was performed.

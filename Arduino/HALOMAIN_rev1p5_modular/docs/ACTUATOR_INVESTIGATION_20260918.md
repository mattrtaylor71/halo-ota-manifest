# Actuator investigation — 18 September 2026

The actuator is not yet qualified as reliable. One normal physical tap woke both
Halo boards, but the following attempt stalled while opening the Uno's serial
port, before any HELP or movement command. This is a reproduced USB/control
failure, distinct from older completed strokes that did not prove screen contact.
Halo firmware, OTA configuration and calibrated stroke were not changed.

## Current controller and observed failure

Replacement Uno serial `03536373232351608112`, VID/PID `2341:0043`, was observed
at `/dev/cu.usbmodem21301`, USB path `2-1.3`. Live inventory shows a Fresco USB2
hub on that path, despite the earlier description of a direct connection. The
USB relay is absent. Never operate the old relay helper against this replacement
without verifying the actual relay, wiring and identities again.

Evidence root: `/Users/MattTaylor/halo-actuator-investigation-20260918`.

| Case | Observation |
| --- | --- |
| `probe001/RESULT.json` | Fresh HELP and calibrated stopped STATUS; no movement; descriptor closed. |
| `tap001/TAP-RESULT.json`, `tap001/REVIEW.json` | One unchanged `PUSH:500,200,500` completed and physically woke both boards to lit Home. Both later slept. A late diagnostic query caused an additional Sense wake; do not call that spontaneous. |
| `repeat001/FAILURE-REVIEW.json` | Next round hung at raw nonblocking `os.open`, before a command. Its old supervisor also failed cleanup after TERM/KILL timeouts and a permission error. Original incomplete receipt is retained. Root stopped the orphan passive capture; later process inventory found no tap child. Normal child reap is not claimed. |
| `hub001/RESULT.json` | Only the exact observed hub port was switched off/on. Commands succeeded, but Uno BSD serial never disappeared. This does not prove a power cut. |
| `probe-after-hub001.json` | Open/configure/write/close returned, but HELP and STOP received no replies. No stroke. |
| `usb001/worker.log` | Fresh standard USB device descriptor, serial and status requests succeeded on the exact Uno. Its USB control endpoint was alive. |
| `usb-reset001/worker.log`, `probe-after-usbreset001.json` | Targeted libusb reset succeeded; subsequent HELP was still silent. No whole-hub reset or kernel-driver detachment. |
| `cdc-dtr001/worker.log`, `probe-after-cdcdtr001.json` | Actual line coding was already115200/8N1. Direct CDC DTR clear/assert/clear requests succeeded; subsequent HELP still failed. This is not proof that the328P rebooted. |
| `cdc-uart001/worker.log`, `probe-after-cdcuart001.json` | Explicit115200/8N1 UART reinitialization and exact readback succeeded; HELP remained silent. |
| `usb-only-probe001.json` | After the user removed USB and12V for10seconds and restored USB only, the startup banner, HELP and calibrated stopped STATUS returned. Descriptor closed and child reaped. |
| `usb-only-idle001/RESULT.json`, `usb-only-reopen001.json` | With motor power reported disconnected, nine STATUS checks across120seconds passed on one descriptor. It then closed; after60seconds idle a fresh HELP/STATUS session passed and closed/reaped. No motion commands. |

The small C USB probes use the host supervisor only for process containment;
`WORKER_RECEIPT_MISSING` in those supervisor receipts is expected because the C
program writes a transcript rather than a Tapper worker JSON. Their actual USB
request results, process exit code, group absence and reap result are recorded
separately. They are not successful Tapper/readiness receipts.

The current evidence does not isolate the remaining cause among the Mac/USB
serial path, bridge/data channel, or actuator power/wiring. Correct baud, a
responsive control endpoint, and failed software recovery narrow the fault;
they do not prove a motor-noise or firmware-corruption explanation. The cold-start USB-only baseline passed; the powered-idle comparison and
post-motion check are pending confirmation that motor power is restored. Do not replace failed cases with later passes.

## Host control correction

`tools/tapctl.py` now supervises a separate raw-fd worker. The parent bounds
serial open, configuration, writes, reads and close, including a kernel stall.
It discovers the exact controller identity instead of trusting a fixed port,
clears inherited hardware flow-control flags, and avoids modem-line ioctls.
Every movement requires fresh HELP and the calibrated stopped STATUS, uses
`PUSH:500,200,500` at target speed128, then checks stopped STATUS and STOP.
No stronger/longer movement, reflash or automatic reset is added.

```sh
python3 -B tools/tapctl.py --action probe
python3 -B tools/tapctl.py --action wake
```

`probe` is nonmoving. `wake` requires both known Halo USB interfaces initially
absent and a new Sense USB appearance after the stroke. It refuses the partial
LCD-only starting state. `ALREADY_AWAKE` is separate from `WAKE_OBSERVED`.
A reported stroke completion alone never proves contact. Pair the USB transition
with actual Halo Home/sleep logs for physical acceptance. These checks describe
this USB-connected bench, not arbitrary unplugged units.

Each operation saves a private receipt and distinguishes protocol silence,
missing completion, no Halo wake, controller timeout and uncertain cleanup.
TERM/KILL/wait errors cannot skip other cleanup. The receipt distinguishes
explicit descriptor closure, worker reaping and process-group absence. After a
controller timeout or uncertain closure, noninventory operations in that
controller process require review; restarting blindly is not recovery.

The soak caller exits on a retained review latch instead of repeatedly creating
controllers and waiting for hours after uncertain USB-worker cleanup.

The controller requires pyserial for enumeration and uses the same public
`Tapper` methods as the existing bench callers. Future replacement identities
must be reviewed explicitly. The separate relay helper remains historical and
is not automatically called by Tapper.

## Validation scope

All24 inert host tests passed, including the actual soak caller stopping on a retained review latch. The frozen158 guard and diff check also passed. No full firmware regression gate was run for this host-tool-only change. The tests exercise actual worker decisions and subprocess containment:
no movement without readiness, unchanged calibration, false-completion rejection,
identity refusal, flow flags, lost acknowledgements, close failures, timeout,
TERM-resistant child cleanup and uncertain/restricted cleanup. Registration in
the offline gate keeps future releases from omitting this test. These tests do
not prove motor movement, contact, electrical power removal or a USB cure.

No new Halo build, flash, firmware publication, schedule change or allowance
change was made for this actuator investigation. Installed/public196 and the
separate pending OTA wording change retain their previous acceptance scope.

CDC request interpretation follows the [Arduino USB bridge source](https://raw.githubusercontent.com/arduino/ArduinoCore-avr/master/firmwares/atmegaxxu2/arduino-usbserial/Arduino-usbserial.c) and [LUFA CDC definitions](https://raw.githubusercontent.com/abcminiuser/lufa/master/LUFA/Drivers/USB/Class/Common/CDCClassCommon.h). The installed bridge image was not read back; source review is not firmware identity proof.

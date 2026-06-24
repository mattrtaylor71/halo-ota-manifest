# HALO Factory / EOL Test — USB Port Topology

This rig flashes and talks to a HALO device pair (Sense + LCD) over USB.
The LCD board exposes **two** ESP32 chips and which one enumerates depends on
the **USB-C cable orientation** (the cable is reversible; flipping it swaps
which LCD chip is reachable). The board stays powered through the flip.

## Port table

| Port                         | Role                                                       | Notes |
|------------------------------|------------------------------------------------------------|-------|
| `/dev/cu.usbmodem1101`       | **Sense** (XIAO ESP32-S3)                                  | Camera + WiFi board. Flash + communicate here. |
| `/dev/cu.usbmodem21201`      | **LCD main chip** (ESP32-S3)                               | The board we flash + communicate with for the LCD. Reachable in the "main" cable orientation. |
| `/dev/cu.usbmodem2120x`      | **LCD U4WDH secondary chip** (deep-sleep target)          | Only appears when the USB-C cable is **FLIPPED** to the other orientation. The trailing digit varies (e.g. `usbmodem2120`, `usbmodem21202`, `usbserial-2120`) — enter it manually in the UI. |
| `/dev/cu.usbmodem101`        | ⛔ **FORBIDDEN — owned by a DIFFERENT process**            | The tool will NEVER open, flash, or read this port. It is excluded from detected-ports and any attempt to use it raises a hard error. |
| `/dev/cu.usbmodem21301`      | Not part of this rig                                       | Ignore (legacy/other jig). |

### Why usbmodem101 is blocked

A separate process owns `/dev/cu.usbmodem101`. Opening it from this tool would
collide with that process. The server enforces this with an **exact-match**
guard (`assert_port_allowed`): it blocks only the literal device
`usbmodem101` (and the bare basename), and deliberately does **NOT** match
substrings — so `usbmodem1101`, `usbmodem21201`, `usbmodem21301`, etc. are all
allowed.

## USB-C cable-flip behavior

The LCD's USB-C connector is reversible and each orientation routes to a
different chip:

- **Main orientation** → LCD **main** chip enumerates on `usbmodem21201`.
  Use this for flashing/communicating with the LCD and for the EOL test.
- **Flipped orientation** → LCD **U4WDH** secondary chip enumerates on
  `usbmodem2120x`. Use this only to put U4WDH into deep sleep.

The board stays powered while you flip the cable, so once U4WDH has been put to
sleep it **stays asleep** after you flip back to the main orientation.

## Why U4WDH must be asleep for the EOL wire tests

The U4WDH secondary chip, when awake, contends on shared GPIO/UART lines and
will interfere with the inter-board wire tests (INT line, UART round-trip,
Sense→LCD heartbeat). Put it to deep sleep first so those tests measure the
real Sense↔LCD wiring. The camera/PWDN steps only exercise the **Sense** board
(`usbmodem1101`) and do not depend on U4WDH state.

## EOL run procedure

1. **Sleep U4WDH** — flip the LCD USB-C cable to the **U4WDH** orientation so
   `usbmodem2120x` appears, enter that port in the **U4WDH Port** field, and
   click **Sleep U4WDH**. Success = the chip prints `@@SLEEP_START` or goes
   silent (both mean asleep).
2. **Run EOL Hardware Test** — flip the cable **back** to the **main**
   orientation so `usbmodem21201` (LCD main) is reachable again. Confirm the
   port fields:
   - Sense: `/dev/cu.usbmodem1101`
   - LCD (main): `/dev/cu.usbmodem21201`
   Then click **Run EOL Hardware Test**.

The wire-test steps are labeled "(needs U4WDH asleep)" and the camera/PWDN
steps are labeled "(Sense only)" so a failure is self-explanatory.

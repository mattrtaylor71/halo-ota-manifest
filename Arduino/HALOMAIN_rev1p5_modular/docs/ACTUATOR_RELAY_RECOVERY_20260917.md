# Actuator USB relay recovery, 17 September 2026

The user wired only the actuator's USB power through COM/NC on a DSD TECH SH-UR01A. The relay's separate USB connection remains powered. The [manufacturer's command reference](https://www.deshide.com/product-details_SH-UR01A.html) specifies9600 baud,8N1 and AT/CH1 commands. In this confirmed NC wiring, `AT+CH1=1` interrupts actuator power and `AT+CH1=0` restores it. Do not reuse this mapping with different wiring or attached loads.

The first private three-second test received exact relay replies and the actuator BSD serial disappeared, but did not return within12seconds. Its `NEEDS_REVIEW` receipt remains at `/Users/MattTaylor/halo-relay-20260917/cycle001/RESULT.json`. An Arduino USB registry node alone was insufficient evidence that its serial driver recovered.

The second test started with the actuator BSD serial already absent. A ten-second off interval followed by exact `OK+CH1=0` restored the exact Arduino serial/VID/PID. The new USB node and subsequent actuator helper open/configure were observed by the hardware owner. The receipt is `/Users/MattTaylor/halo-relay-20260917/cycle002/RESULT.json`, SHA256 `c8a707acfbd212973a71bec8b417bc3f5af01272478c93c72bff3c5a9fcb7ef7`. Its original broad status text does not prove a new disappearance from an initially present BSD port; it proves return after a degraded start. This single recovery does not establish that power cycling fixes every USB fault.

## Reusable host helper

`tools/actuator_relay_reset.py` controls only the pinned relay. It never opens the actuator serial port, sends a movement command or operates either Halo board. Use it only with the same confirmed COM/NC wiring and an idle actuator:

```sh
python3 -B tools/actuator_relay_reset.py --out /absolute/new/private/relay-result
```

If the exact actuator BSD serial is already absent, explicitly add `--degraded-start`. The default is a ten-second off hold after the acknowledged command and at most30seconds to observe serial return. Two successive enumeration samples must contain the unique expected serial/VID/PID. The ordinary mode also requires observed disappearance. Return proves BSD enumeration only; a separate caller may make a bounded nonmoving `HELP` query to prove the actuator sketch is responsive. Do not infer a successful stroke from either proof.

The helper checks callout and dial-in ownership for the known devices, uses only the three approved relay commands, and requires complete exact acknowledgement lines. Once an off write is attempted, `finally` sends the idempotent restore command, with at most two bounded attempts. SIGINT/SIGTERM request cleanup; repeated signals do not interrupt that restore section. A supervisor bounds a stuck kernel call and allows eight seconds for cleanup before a forced stop. Every normal failure is reported, including unconfirmed restoration or descriptor closure. Do not treat command acknowledgement as a voltage measurement.

Software cannot guarantee restoration after relay disconnection, an uninterruptible kernel call, SIGKILL or host power loss. A forced stop, missing worker receipt or missing exact restore acknowledgement is a review condition; check the physical power state before another action. Do not loop power cycles or add an automatic stroke. The reusable helper was subsequently exercised in `cycle003`: the initially present actuator disappeared during a ten-second cut, returned after exact restore acknowledgement, and the worker closed/reaped cleanly. A separate bounded `HELP` query then proved its sketch responsive without movement. See `/Users/MattTaylor/halo-relay-20260917/cycle003/RESULT.json` and `actuator-help.json`. The earlier recovered actuator also completed a calibrated stroke and woke Halo for `image182-004`. A later small change tightened degraded-start and forced-stop reporting; those changed branches are host-tested rather than newly exercised on hardware.

Validation: `python3 -B tools/test_actuator_relay_reset.py` passes70 inert checks of the actual cycle logic, including stale/wrong acknowledgements, partial off writes, ownership refusal, missing serial/degraded admission, restore I/O errors, repeated restore signals, descriptor failure and absent serial return. Tests use fake clocks, inventories and transports; they do not import pyserial or access hardware.

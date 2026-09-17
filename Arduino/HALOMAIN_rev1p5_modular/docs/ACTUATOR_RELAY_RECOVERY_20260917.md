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


## Later recurrence before183 service

The next identity attempt, `service-identity183-001`, opened/configured the
actuator but its bounded `HELP` query returned no reply. It closed without
sending `PUSH` or opening either Halo capture port. The subsequent current-helper
`cycle004` observed49 absent serial samples during a10-second off interval,
received exact restore acknowledgement, and observed the exact actuator BSD
serial return. The relay descriptor closed and its worker exited/reaped cleanly;
there were zero actuator commands. Receipt:
`/Users/MattTaylor/halo-relay-20260917/cycle004/RESULT.json`, SHA256
`061219c92565bac2bc100e21a82b9ab27dc09556584fbf876ab00448e28ee23d`.
This proves another bounded BSD recovery following a real recurrence.
`service-identity183-002` then received the actuator's HELP response and its one
`PUSH:500,200,500` woke both Halo boards. The old blocking helper nevertheless
timed out after25seconds without a stroke-completion/descriptor-closure receipt.
The collector captured normal boot and sleep and closed cleanly; no firmware was
written. Preserve that failed helper result: actual wake is not proof that the
helper received `Complete` or completed its cleanup.

`cycle005` again proved the initially present exact BSD port disappeared and
returned after a10-second cut; its worker closed/reaped without actuator commands.
Receipt `/Users/MattTaylor/halo-relay-20260917/cycle005/RESULT.json`, SHA256
`b48846cf6fcf3bd5ddccd788b338fb65bd9e6d1215ec098194827e391fbca020`.
The separate traced helper in `service-identity183-003` received `Complete`,
closed its descriptor and exited/reaped successfully. Its concurrent collector
observed the one actual paired wake and then fresh unlocked Home with Sense182
app1/LCD180 app0, both SDK VALID. The identity was collected after helper closure;
no second stroke was sent. Pins under `/Users/MattTaylor/halo-upload181-20260917/`:

- `service-identity183-003/wake-owner.json`: SHA256 `bdf373771dd4f8dfd548e24c39a5fc71f597d315f10ba34e119ce8162140e47d`.
- `service-identity183-003/identity.json`: SHA256 `36840eeb0fc1eca88bb4a6493e18dc7fd1276b3aeb901cb34c7e2cfe71b52f23`.

This establishes usable recovery for this recurrence. It does not prove the
relay cures every USB failure, and does not turn the cycle004 helper timeout
into a completed-stroke pass.

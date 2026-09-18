# Actuator USB relay recovery, 17 September 2026

## Current direct connection — 18 September

The replacement Uno, serial `03536373232351608112`, is directly connected; the relay is absent from this wiring. Its existing sketch answered HELP and reported the calibrated stopped state, so no reflash was needed. [Nonmoving probe001](/Users/MattTaylor/halo-actuator-direct-20260918/probe001/RESULT.json) passed. [Tap001](/Users/MattTaylor/halo-actuator-direct-20260918/tap001/tap.json) completed one `PUSH:500,200,500`; the [closed paired capture](/Users/MattTaylor/halo-actuator-direct-20260918/tap001/RESULT.json) showed lit Home followed by both boards entering deep sleep.

[Probe002](/Users/MattTaylor/halo-actuator-direct-20260918/probe002/RESULT.json) then reopened the actuator after **186.4 seconds idle**, received HELP and calibrated stopped STATUS, and closed/reaped successfully without movement or firmware writes. This is a finite recovery check, not proof of a permanent cure: both the Uno and USB path changed, so a hub root cause is not established.

The existing relay helper targets the **old** Uno serial `0353637333235110A2A3`. Do not silently reuse it for the replacement or the current direct wiring. The relay/reflash results below remain historical evidence, including their failures; current190 device acceptance is recorded separately in [the voice/list validation record](VOICE_LIST_UPLOAD_VALIDATION_190.md).

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

## Voice/list189 campaign: recovery and recurring failures

The eight later relay runs below used the same helper, confirmed COM/NC wiring
and ten-second off hold. Every run received exact `OK+CH1=0`, closed the relay
descriptor and reaped its worker; none sent an actuator command. Their receipts
are under `/Users/MattTaylor/halo-voice-list189-20260917/`:

| Receipts | Initial actuator BSD port | Observed result |
| --- | --- | --- |
| [relay001](</Users/MattTaylor/halo-voice-list189-20260917/relay001/RESULT.json>), [relay002](</Users/MattTaylor/halo-voice-list189-20260917/relay002/RESULT.json>) | Present | Disappearance and exact serial return proved; `BSD_DISAPPEAR_RETURN_PROVED`. |
| [relay003](</Users/MattTaylor/halo-voice-list189-20260917/relay003/RESULT.json>), [relay005](</Users/MattTaylor/halo-voice-list189-20260917/relay005/RESULT.json>), [relay007](</Users/MattTaylor/halo-voice-list189-20260917/relay007/RESULT.json>) | Present | Disappeared, but exact serial did not return within the observation bound despite restore acknowledgement. Worker `USB_RETURN_NOT_PROVED`; supervisor `STOPPED_FOR_REVIEW`. |
| [relay004](</Users/MattTaylor/halo-voice-list189-20260917/relay004/RESULT.json>), [relay006](</Users/MattTaylor/halo-voice-list189-20260917/relay006/RESULT.json>), [relay008](</Users/MattTaylor/halo-voice-list189-20260917/relay008/RESULT.json>) | Already absent; explicit degraded start | Exact serial returned after restore; `BSD_SERIAL_RETURNED_AFTER_RESTORE`. These prove recovery from absence, not a newly observed disappearance. |

Separate subsequent strokes completed and closed cleanly in
`identity188-005/wake.json` after relay004, `voice-list001/wake.json` after
relay006, and `voice-list002/wake.json` after relay008, all under that campaign.
Recovery was not permanent: `health189-001/wake.json`, between relay004 and
relay005, records `No stroke completion` with `complete=false` and
`descriptor_closed=false`. A recovered serial port can still be followed by a
tap/helper hang. The relay receipts do not prove sketch responsiveness, a stroke,
or measured power removal; these observations establish no electrical root cause.

For another recurrence, close and reap existing owners before using the helper
once with a new output directory. If it reports `STOPPED_FOR_REVIEW`, inspect
`worker_status`, restore acknowledgement and exact serial inventory; do not
advance to a stroke merely because restoration was acknowledged. After review,
if the exact actuator BSD port remains absent, use a separately recorded
`--degraded-start` recovery. Require its exact return result, then a bounded
nonmoving `HELP` check before any separately authorized stroke. Bound the stroke
helper to at most25seconds and retain its completion and closure result; an
observed Halo wake does not erase a helper timeout. Do not automate repeated
power cycles or infer that a successful degraded recovery fixes the recurrence.

## 190 installation blocker: return is not command readiness

In `/Users/MattTaylor/halo-voice-list189-20260917/`, relay009 and relay010 both proved exact actuator BSD disappearance/return and acknowledged power restore, but the subsequent bounded HELP checks failed. `identity189mixed-001`/`002` hit their25-second owner bounds with no stroke; `003`, after a targeted hub-port off/on, closed cleanly but still had no HELP. All three captures opened neither Halo board.190 was therefore not installed.

The hub intervention targeted only the freshly observed actuator port2-1.3.3 using exact-location `uhubctl`, with no whole-hub or USB3-companion reset. Off/on commands returned0, but the exact BSD node did not disappear, so it is not proof of a physical power cut. This location is session-specific, not a reusable hard-coded command.

A first actuator-only DTR diagnostic blocked on its initial clear ioctl; its parent failed to reap within three seconds after kill. Later process inspection confirmed no remaining child; clean descriptor closure is not claimed for that attempt. After the hub intervention, one complete DTR clear/assert150ms/release with hardware flow control disabled closed cleanly, but HELP remained empty. A20-second, non-writing `avrdude` signature probe (normal Arduino reset handshake, `-n -u -D`, no erase/upload/force flags) also failed and was killed/reaped. No actuator firmware was reflashed and none of these attempts sent a stroke.

The final state and closed case references are in `CHECKPOINT190.json`. The current practical blocker is command/bootloader communication despite enumeration; its electrical or USB root cause is not established. Do not run an unbounded reset loop or infer that another firmware flash is possible. A manual Halo tap can bypass the actuator for the pending firmware install/test. Relay reset remains the documented first recovery attempt, not a guarantee.


## User-requested actuator reflash: verified, recurrence remains

At the user's request on 17 September, a fresh ten-second relay reset restored
the exact Uno identity. A bounded avrdude probe read the ATmega328P signature
(0x1e950f) and backed up all 32768 flash bytes. The backup was copied to the Mac
mini and its SHA256 read back before flashing. The existing 9036-byte application
already matched the saved calibrated image; no application corruption was found.

The same saved application was rewritten with automatic verification enabled.
All 9036 bytes verified, a full flash readback matched the application, and the
bootloader region remained identical to the backup. No fuse, EEPROM, bootloader,
calibration or Halo firmware change was made.

Two separately recorded calibrated taps subsequently completed and woke Halo.
The first helper closed successfully; a later nonmoving HELP session stalled
and was killed/reaped. After another successful relay reset, the second tap
passed HELP, initial STATUS, PUSH completion and immediate stopped STATUS.
Keeping its descriptor open through 30 seconds of inactivity did not prevent
failure: the next STATUS and STOP were unanswered, then close took 17.26 seconds.
Thus a serial close/reopen alone does not explain the recurrence. The paired
Halo captures showed normal Home and intentional sleep, with Sense189/LCD188.

The last relay reset received exact power-restore acknowledgement and closed
cleanly, but the exact Uno serial did not return within 30 seconds. Subsequent
enumeration exposed Arduino VID/PID with a missing serial. A prepared keepalive
test was therefore **not run**. No command was sent to that incomplete identity.
The electrical/USB cause remains unresolved; neither repeated reflash nor a
persistent connection is established as a fix. Candidate190 remains uninstalled.

Evidence is archived and independently SHA256-read back on mac-mini:

- Root: /Users/mikehunt/halo-bench-receipts/actuator-reflash-1789709068/
- Original backup: original-flash.bin, SHA256
  dd7271d5b00dfe0e16775cfc0db4c7259f49f5e939c4890a10e90e28f1f568f0.
- Case: evidence/VERDICT.json; all 51 case files are indexed by
  evidence/MANIFEST.json, SHA256
  c1dbee357d869eebda6a4bce50869a552bb1f97d5df11ec1634faf9ec8e1e90e.
- Saved Intel HEX SHA256:
  49a0fa47a9f8670bf7f65526da6d2f11bd6df55202af75a8ac7954eac684c2df.

The local Mac had only about 329 MiB available and had already produced ENOSPC;
this case used a temporary RAM disk, archived before removal. Archive paths
above belong to the Mac mini, not the laptop. All capture/actuator child processes
were closed or killed and reaped. Failed cases are preserved alongside passes.

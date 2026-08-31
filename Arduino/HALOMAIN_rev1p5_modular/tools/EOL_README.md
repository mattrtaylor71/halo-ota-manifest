# HALO EOL test station

## Flashing units for CUSTOMERS

```bash
./tools/eol --factory --serial <unit-serial>
```

Use this for anything going to a home. It does a full **erase** of both boards
and writes the complete image, so the unit ships **virgin** — no owner, no WiFi
credentials, no OTA state — and the customer provisions it themselves.

`--factory` is not the same as a normal flash. `arduino-cli upload` deliberately
PRESERVES NVS, so a normal flash leaves the previous owner's identity on the
device. Only an erase clears it.

What it checks (18): both boards identified by flash size, erase confirmed per
board (MAC reported), full image written with hash verified, boots at the right
version, LCD<->Sense link alive, all three capture modes work, **unit is
UNPROVISIONED with the setup AP broadcasting**, no panics, no camera failures.

S3 verification is skipped on purpose: a virgin unit has no owner, so presign
cannot succeed and nothing should reach the cloud. The capture check still proves
the sensor, the DMA reserve and the LCD flow. Likewise the "UI reached Logged"
check is skipped — a virgin unit sits on the provisioning QR screen, so the
capture UI never runs and asserting it would fail a perfectly good unit.

## Running it (already-provisioned units)

**Double-click `HALO EOL Test.command` on the Desktop.** That is the whole job.

A Terminal window opens and asks for a serial number. Plug a unit in, type the
serial (or just press Enter), and wait ~3-4 minutes. You get a big green
**UNIT PASS** or red **UNIT FAIL**, and the photo the unit actually took opens
on screen. Then it asks for the next unit. Type `q` to finish.

From a terminal instead:

```bash
./tools/eol --loop        # station mode: unit after unit  (what the .command does)
./tools/eol               # one unit, then exit
./tools/eol --no-flash    # test a unit without reflashing it
./tools/eol --build       # rebuild firmware — run once after a code change
```

## Look at the photo

The test can only check that a photo is sharp-ish and the right size. It cannot
tell you the camera is pointed at the wall, tinted green, or out of focus.
**Look at the image it opens.** That is why it opens.

## What PASS actually means

21 checks. The important ones:

- both boards identified **by flash size**, not port number
- firmware version and device ID reported
- LCD <-> Sense link alive
- all three capture flows (Check-in, Discard, Dish) reach "Logged"
- the unit sleeps, and its deferred uploads drain at sleep
- no panics, no camera-init failures, no dropped captures
- **every photo is a real object in S3**, with a backend-processed twin

That last one is the point. A device-side "upload OK" log has been wrong twice:
once a truncated serial line hid a good upload, once a capture was destroyed
while every counter still read green. PASS means the photo is in the cloud.

## If it fails

Every failing check prints its reason. Common ones:

| Message | Meaning |
|---|---|
| `Sense/LCD detected — not found` | Unit asleep or unplugged. It auto-taps; if there is no actuator, tap the screen or replug. |
| `reports None, expected 8MB — NOT writing` | Could not confirm which board is which, so it refused to flash. Replug and retry. |
| `not within 420s` | Post-flash maintenance cycle ran long. Retry with `--no-flash`. |
| `LCD write failed` | The LCD slept mid-test. It retries automatically; if persistent, replug. |
| `Captures present in S3 — 0 new` | Photos never reached the cloud. Check WiFi and the AWS profile. If the device log shows `PUT status: 200`, the photos DID upload and the tester is looking in the wrong bucket — check which bucket the presign returned. |

Reports and images: `tools/eol_results/` (one JSON + serial log per unit).

## Notes

- Firmware is **not** rebuilt automatically. After changing code run
  `./tools/eol --build` once, then test units normally.
- A factory-fresh unit boots awake. A previously-provisioned unit deep-sleeps
  and needs a tap; the station handles both, but the fresh-unit path has not yet
  been exercised on a real first article.
- `tools/factory_test/` (localhost:9095) is the deeper bench rig — per-wire
  INT/UART checks, PWDN heat safety, EOL test firmware. Use it when
  investigating a hardware fault; use this for production flow.

## Backend went to production 2026-08-24

The presign API now hands back `trepo-grocery-{uploads,discards}-prod`. The
station checks prod **and** dev, so a unit on older firmware still verifies.

This bit once: a healthy unit reported FAIL with "0 new originals" while its own
log showed three `PUT status: 200`. The uploads were fine — they had gone to the
new prod buckets and the tester was still watching dev. **If the device says it
uploaded, check which bucket the presign returned before suspecting the unit.**

Related: identity is read from the SENSE only. The LCD reports its own
`device_id`, and S3 keys are built from the Sense's — mixing them made a run
report the LCD's id and then find nothing filed under it.

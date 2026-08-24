# HALO EOL test station

## Running it

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
| `Captures present in S3 — 0 new` | Photos never reached the cloud. Check WiFi and the AWS profile. |

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

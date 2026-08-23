# Ship checklist — HALO firmware

Everything here is verifiable from a built binary. Do not take a build command's
word for what is in it; check the binary.

## 0. No secrets reachable by git

```bash
./tools/check_secrets.sh    # must exit 0
```

**This section exists because the leak already happened.** An AWS IoT client
certificate and its RSA private key were inlined in `Sense_Minimal.ino` and pushed
to the **public** repo `mattrtaylor71/halo-ota-manifest` on branch
`lcd-backlight-binary`, where they sat from 2026-04-14 until found on 2026-08-20 —
about four months.

The instructive part is *why* nobody noticed. `MqttSecrets.local.cpp` was correctly
git-ignored the entire time. That ignore rule was real protection for the file it
named, and it created the impression the credential was handled — while a second
copy of the same key sat in a tracked `.ino` that nobody thought of as a secrets
file. **A gitignore protects a path, not a secret.**

So the scanner deliberately checks **tracked *and* untracked-but-not-ignored**
files (`git ls-files --cached --others --exclude-standard`). A plain `git grep`
sees only tracked files and would have found 3 of the 5 source copies.

Two traps worth keeping, both of which produced a *silently passing* gate:
- The PEM patterns start with `-----`, so `grep` parses them as options unless you
  pass `-e`. Without it the scan matches nothing and reports OK.
- A PEM header alone is not a secret — docs show the marker with the body
  redacted. Requiring a base64 body after the marker is what keeps the gate from
  failing on documentation and then being switched off.

Build outputs are now git-ignored (`**/dist/`, `**/artifacts/*.bin`, `*.ino.bin`):
a `.bin` embeds whatever `MqttSecrets.local.cpp` held at build time, and 2,514 such
images were sitting untracked-but-addable — one `git add -A` from the same leak.

## 1. Bench flags must be absent

There are seven. **None is defined in source** — each is only reachable via
`--build-property`, so a plain `arduino-cli compile` is ship-safe by default.
That is the design, and it is why the list below is a verification step rather
than a "remember to turn these off" step.

**The design is not self-enforcing, and it has already failed once.** On 08-18
the LCD's `spoolfill`, `spoolclear` and `spoolcaps` commands were found in the
**shipping** binary with no guard at all. `spoolfill` forges spool slots that the
next drain uploads as real check-ins into the owner's kitchen, and `spoolclear`
deletes captures that have not been uploaded yet. They looked bench-only, so
nobody checked. That is exactly why this section greps the binary instead of
reading the source: a command that can fabricate or destroy a user's food log
must be *absent*, not merely hard to reach.

The important property is not that they add test affordances. It is that they
**remove real code paths**, so any result measured on a bench build is evidence
about the bench build and nothing else. This bit three times in one session:

| Flag | Board | What it REMOVES |
|---|---|---|
| `HALO_DEV_NO_SLEEP=1` | LCD | idle sleep — the LCD never initiates sleep coordination, so the **Sense never sleeps either** |
| `STRESS_TEST_NO_SLEEP` | Sense | the entire body of `sense_enter_deep_sleep()` (~22KB) — including the spool-to-SD fallback |
| `HALO_SPOOL_TEST=1` | Sense | (adds) `spooltest`, `draintest`, `drainreal`, `dropacks`, `failupload`, `nextwake` |
| `HALO_MAINT_TEST_S=N` | Sense | replaces the nightly 02:00 wake with an N-second timer |
| `HALO_QUIET_SOAK=1` | LCD | log volume |
| `HALO_FREEZE_TEST=1` | LCD | (adds) `freeze`, `freezeui` — hang a watchdog-subscribed task on purpose |
| `HALO_CAM_STALL_TEST=1` | Sense | (adds) `camstall`, `camwedge`, and the stall injection inside the grab worker |
| `HALO_TEARDOWN_DELAY_MS=N` | LCD | widens the sleep teardown window from its real ~166ms so a tap can be landed inside it. Ships as 0. |

`HALO_SPOOL_TEST=1` also gates the three **LCD** spool commands added 08-18
(`spoolfill`, `spoolclear`, `spoolcaps`) — see the warning above — and the Sense's
`settz`, which writes the owner timezone straight to NVS as a stand-in for the
backend. None of them belongs in a shipped build.

Verify against the binary:

```bash
S=/tmp/halo_sense_ship/halo_sense_prod.ino.bin
L=/tmp/halo_lcd_ship/halo_lcd_prod.ino.bin
for p in SPOOLTEST ACKTEST DRAINTEST FAILUPLOAD NEXTWAKE \
         "deep sleep suppressed" "HALO_MAINT_TEST_S override"; do
  echo "$p sense=$(strings $S | grep -c "$p")"
done
echo "idle sleep suppressed lcd=$(strings $L | grep -c 'idle sleep suppressed')"

# LCD bench commands. SPOOLFILL/SPOOLCLEAR/SPOOLCAPS shipped unguarded once --
# these four greps are the regression test for that.
for p in SPOOLFILL SPOOLCLEAR SPOOLCAPS FREEZE_TEST RINGSTAT "\[ENQ\]" \
         "teardown window widened"; do
  echo "$p lcd=$(strings $L | grep -c "$p")"
done
# Sense camera-stall injection and the timezone override.
echo "CAM_STALL_TEST sense=$(strings $S | grep -c CAM_STALL_TEST)"
echo "SETTZ sense=$(strings $S | grep -c SETTZ)"

# TLS must validate the peer, not skip validation. OTA_TLS_INSECURE_DEBUG=1 swaps
# setCACert(kAmazonRootCa1) for setInsecure() on the claim, manifest and OTA paths
# -- silently, with no runtime log. Verify the pinned CA is actually IN the binary
# rather than trusting the default, since a --build-property can flip it:
echo "amazon_root_ca1 sense=$(strings $S | grep -c 'MIIDQTCCAimgAwIBAgITBmyfz5m')"   # must be >=1
```

All must be `0`. Run this against the **ship** build paths, not whatever was
built last — and confirm the binary is newer than the sources it came from
(`flash_nightly.sh`/`flash_ship.sh` now abort if it is not, because both scripts
upload a prebuilt image with `--input-dir` and spent a session silently flashing
a stale one).

### 1a. OTA channel must be PROD

`OTA_DEFAULT_ENV` defaults to **`"prod"`** (changed 2026-08-21). A plain
`arduino-cli compile` is therefore ship-safe, the same principle as the bench
flags above. Previously it defaulted to `"dev"`, which meant **every device built
from this tree fetched firmware from `halo-ota-dev` — the bucket we push test
builds to.** A stray dev push would have gone straight to customer hardware.

Bench/dev builds opt IN:

```bash
--build-property "compiler.cpp.extra_flags=-DOTA_DEFAULT_ENV=\"dev\""
```

`publish_both.sh` now derives the flag from `--channel`, so the channel published
to and the env compiled in can no longer disagree.

Verify on-device (§5), not from the source — the URL is assembled at runtime:

```
[OTA_CFG] resolved_manifest_url=https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/manifest_latest.json
```

The DNS sanity check in `ManifestClient.cpp` now resolves the host it is actually
fetching; it used to probe the literal dev host, so on a prod build it validated
a host the device never contacts.

## 2. These must be PRESENT

Absence means a protection silently did not compile in.

```bash
strings $S | grep -c 'SPIFFS full'   # upload-failure -> SD fallback   (>=1)
strings $S | grep -c WAKELOG         # wake history                    (>=1)
strings $L | grep -c FREEZE_WDT      # LCD freeze watchdog             (>=1)
strings $L | grep -c LINK_ACK        # ack/retransmit layer            (>=1)
strings $L | grep -c SPOOL_CAP       # SD spool count/age caps         (>=1)
strings $S | grep -c wedge_restart   # camera stall self-heal          (>=1)
strings $S | grep -c UPLOAD_TLS      # photo PUT validates the peer    (>=1)
strings $L | grep -c deferred_until_awake            # deferred-TX ring          (>=1)
strings $L | grep -c "touch arrived during teardown" # teardown tap abort        (>=1)
strings $L | grep -c STATE_DISAGREE  # awake-state invariant guard     (>=1)
```

## 3. No duplicate copies of shared sources

```bash
./tools/check_shared_dupes.sh    # must exit 0
```

Arduino only compiles `.cpp` files inside the sketch directory, so each shared
source needs a same-named file there. The safe form is a one-line stub that
includes `../shared/<file>`. A full copy looks identical and works — until
`shared/` is fixed and the copy is not. A UB fix in the frame parser once went
into `shared/`, the Sense picked it up and the LCD did not, and a valid 512-byte
chunk kept decoding as `data_len=59649` after the fix was written, built and
flashed.

## 4. Host tests

```bash
cc  -I halo_ota_demo/firmware/shared -o /tmp/tns  tools/test_nightly_schedule.c && /tmp/tns
cc  -o /tmp/cobs tools/cobs_probe.c && /tmp/cobs
c++ -I LCD_Minimal -o /tmp/tdr tools/test_deferred_ring.cpp && /tmp/tdr
```

`test_deferred_ring` must report `ALL TESTS PASSED (0 failures)` across 24 checks.
It includes the REAL `LCD_Minimal/lcd_deferred_ring.h` rather than a copy, and
covers the three properties that actually matter: two user actions never swap
order, an overflow drops the OLDEST and logs it rather than losing input
silently, and `remove_type` does not take unrelated messages with it.

The nightly scheduler must report `ALL TESTS PASSED (15 checks)` with
`max_wait=90000s (25.0h)` — 25h is the autumn DST maximum and a larger number
means the DST handling has regressed.

## 5. On-device sanity

| Check | Expected |
|---|---|
| `nextwake` | epoch advancing, `TZ=PST8PDT,…`, target at the next 02:00 local |
| boot log | `[FREEZE_WDT] armed` **plus both** `subscribe uart_task` and `subscribe ui_task` |
| 12+ captures | 0 panics, 0 `Stack canary`, 0 camera init failures, `dma_largest` stable |
| capture | `killing WiFi` count **0** — a non-zero count starves uploads |
| capture | each one ends back on `SHIP_MAIN_MENU` (see §5a) |
| `linkstats` | `failed=0`, `overflow=0` |
| `[OTA_CFG] resolved_manifest_url` | must contain **`halo-ota-prod`** — a dev-pointing ship build is a hard fail |

Only `nextwake` needs a bench build (`HALO_SPOOL_TEST=1`); `linkstats` is ungated
and everything else runs on the ship binaries. Prefer the ship build — bench flags
REMOVE code paths, so a bench result is evidence about the bench build only.

The watchdog line matters: it once armed *after* `uart_task` was created, so only
`ui_task` subscribed and the boot log still looked healthy. Count both lines. Note
these lines only appear on a genuine boot, so capture them across a real
sleep→wake; a run that starts with the device already awake shows `uart_task=0`
and looks like the exact regression it is not.

### 5a. Do not fire captures back-to-back

`scratchpad/ship_sanity.py`. Score each capture on **UI recovery**, not
`phase=DONE` — a capture that reaches DONE can still strand the screen, and a
stranded non-menu screen blocks sleep entirely (see §5b).

Wait for the UI to be back on `SHIP_MAIN_MENU` before injecting the next one.
Discard and Check-in both end in `WAITING_INPUT` (add-to-list / expiry choice), and
a request arriving then is **correctly refused**:

```
[OP_WORKER] SCAN: Discard mode - waiting for add-to-shopping-list choice...
[SCAN] ignore menu select item=Discard ... state=1
```

A first version of this test fired 12 requests back-to-back, scored those refusals
as capture failures, and produced "1/12 passed" plus 29 camera-init errors from
the resulting thrash. That is a harness bug reported as a firmware defect — worse
than running no test at all. `dma_largest` being FLAT through the failures was the
tell: real DMA exhaustion falls, it does not hold steady.

### 5b. Sleep eligibility

Only `SCREEN_HOME`, `SECOND`, `SETTINGS`, `SHOPPING_LIST` are sleep-eligible
(`ui_is_sleep_eligible_menu_screen`). Any other screen left without a dismiss
deadline pins a lit AMOLED awake until the 5-minute guardian force-sleep. Confirm
the run ends with `[SLEEP] entering deep sleep` and both USB ports disappearing —
the Sense port vanishing is the only honest sleep signal.

## 6. Known-open at time of writing

- **§5 on-device sanity re-run on 6.2.0 (2026-08-22): PASS on every fault gate.**
  Device reports `fw=6.2.0`. 9 captures / 9 enqueued / device `up_ok` 9 /
  `up_fail` 0 / dropped 0 / refusals 0 / UI strand events 0, every flush
  `DRAINED`. 0 panics, 0 stack canary, 0 camera-init failures, 0 `killing WiFi`,
  0 aborts. `linkstats failed=0 overflow=0`. OTA channel resolves to
  **halo-ota-prod**.

  `ship_sanity.py` reported "7/12 returned to menu", which is a **harness
  scoring artifact, not a UI fault**: the LCD logged 14 `to=SHIP_MAIN_MENU`
  transitions and zero `UI_STRAND` events, and several failures carry
  `done_at=0.3s` — the harness matching a stale DONE left in its buffer rather
  than the capture it just injected. Score §5 against the Sense's own
  `enq`/`up_ok` per boot and the LCD's strand counter; treat the 12/12 tally as
  advisory until the harness is fixed.

- **`[FREEZE_WDT] armed` and `subscribe uart_task` never appear in the boot log**,
  while `subscribe ui_task: ok` does. This is a LOGGING gap, not a missing
  watchdog: `subscribe` returns silently unless `g_freeze_wdt_ready` is true, so
  the ui_task line proves `init()` succeeded — and `uart_task` is created AFTER
  `lcd_freeze_wdt_init()`, so it cannot have hit the not-ready path. The two
  missing lines are printed before USB CDC enumerates and are dropped by
  `Serial.setTxTimeoutMs(0)`. Worth re-emitting the watchdog state once USB is up
  so §5 can verify it instead of inferring it.

- **After a flash, both boards stay busy for 5–10 minutes** and the Sense does
  **not enumerate USB** while it does. The LCD sits in `state=OTA
  reason=ota_stay_awake` receiving `MAINT_KEEPALIVE`. Taps in this window look
  like they "do nothing" — they do not: the LCD logs `[TOUCH] Touch pressed` and
  `sense_state=AWAKE`, so there is simply nothing to wake. **Judging awake/asleep
  by the USB port is wrong in this state.** Wait for both ports to disappear
  before starting any harness.

- **Full 30-cycle soak PASSES on the current build (2026-08-22).**
  `captures 30 / device up_ok 30 / up_fail 0 / dropped 0 / hold_in_place 0 /
  panics 0 / camera-init failures 0 / flushes DRAINED 30 / slept 30`.
  No erosion: heap +0.8%, psram -0.0%, `dma_largest` flat. Mean awake 49s/cycle.

- **lwIP/SNTP panic during upload — FIXED 2026-08-22.** One capture in 30 was
  destroyed by a hard reset mid-presign:

  ```
  assert failed: udp_new_ip_type udp.c:1278 (Required to lock TCPIP core functionality!)
  ```

  `configTime()` leaves SNTP running for the whole boot. When it cannot reach a
  server — routine right after a WiFi hard reset — it sits on a **pending DNS
  request**. lwIP then runs that callback INLINE on whichever task next resolves
  a name, which here was `upload_worker_task`:

  `hostByName -> dns_gethostbyname -> dns_clear_cache -> dns_call_found ->
  sntp_dns_found -> sntp_try_next_server -> sntp_request -> dns_gethostbyname ->
  udp_new_ip_type` — raw lwIP from a task with no TCPIP core lock. Board panics,
  PSRAM goes, capture is gone. The clock was **already valid**, so every one of
  those retries was pure liability.

  Fixed with `sense_ntp_stop_if_time_valid()`, called from the **main task only**
  (stopping SNTP is itself a raw-lwIP call, so it must not run on the worker):
  once from the WiFi service path and once immediately before the pre-sleep
  flush. Post-fix: 30/30 resets are `DEEPSLEEP`, zero asserts.

- **Two harness lessons that each turned a clean run into a false verdict.**
  1. *Do not detect panics by their text.* The board can reset before
     `Guru Meditation` clears the serial buffer — that panic above was scored as
     `0 panics`. Detect the NEXT boot's `reset_reason=ESP_RST_PANIC` /
     `panic_backoff` / `assert failed:` instead.
  2. *Do not count uploads by grepping `PUT status: 200`.* That depends on the
     exact text surviving the UART; one line arrived as `[UPLOAD] PUT :57:05 GMT`,
     truncated mid-line, and failed an otherwise perfect 30-cycle soak. The Sense
     writes its own `up_ok`/`up_fail` into the WAKELOG at cycle close — use that.

- **WiFi maintenance times out on essentially every cycle** (31 `connect_timeout`
  in 30 cycles, each `elapsed_ms=25000 -> hard_reset`) yet uploads still succeed
  100%. Not data loss, but it is ~25s of radio-on time per wake on a device whose
  whole design is being off. **Open — worth investigating before shipping.**

- **THREE silent photo-loss paths found by the accelerated soak (2026-08-21).**
  All pre-existing. Each destroyed a user's capture while every gate read green.

  1. *Force-defer freed the image.* Sleep blocked on `reason=upload_queue`, and
     after 10s `background_force_defer` failed to persist and `free()`d the
     buffer (`sleep_drop ... queue_not_persisted`). Under deferral a queued
     upload is the EXPECTED state and the pre-sleep flush is the only thing that
     drains it, so blocking sleep blocked the drain. Fired only with **exactly
     one** job queued (force-defer skips itself when `count != 1`), which is why
     every 4-deep session passed and hid it. Fixed: a queued upload no longer
     blocks sleep, and the drop path tries the SD spool first.

  2. *The image lived in a worker stack local.* `upload_wait_for_foreground_window()`
     holds a **dequeued** job in its own stack frame -- out of the queue, not
     parked, not inflight, invisible to every check. The flush logged `DRAINED`
     in **100ms** and the device deep-slept, wiping the PSRAM. Trigger was
     `recent_lcd_link`, which is guaranteed true during pre-sleep **because the
     sleep handshake is itself LCD traffic**. Measured: 15 captures, 12 uploads,
     3 destroyed. Fixed with an `upload_worker_holding_in_place` flag in the
     flush's exit condition, and the hold now yields immediately once
     `g_upload_flush_requested` is set.

  3. *LCD could never sleep.* `sleep_blocked_for_ota()` returned unconditionally
     on `ota_check_requested || ota_check_pending` -- the only branch there
     without the stale-flag escape its neighbours have. Sense sets it then
     sleeps, LCD spins `[SLEEP] blocked (ota_pending)` forever with a lit AMOLED.
     Fixed with the same `sense_state == SENSE_ASLEEP` escape plus a 120s cap.

  **The gate that matters is `captures == PUT200 + spooled`.** `drop=0`,
  `PHOTO_LOST=0`, per-cycle counters and `DRAINED` were ALL green through every
  one of these. The first soak returned PASS while bug 2 was destroying photos;
  the only signal was 12 uploads against 15 captures. `session_accounting.py` and
  `cycle_soak.py` now gate on the invariant, and the firmware drop line emits
  `PHOTO_LOST` so greps catch it.

  Post-fix result: 10 consecutive cycles, **10/10 accounted**, `hold_in_place`
  events 3-of-15 -> **0**, 10/10 slept, 10/10 DRAINED, 0 panics, 0 camera-init
  failures, no heap/psram/dma erosion. A full 15-cycle run and a §5 re-run on this
  build are still owed -- the tap actuator died mid-run (see §5c).

### 5c. The tap actuator fails silently and looks fine

`sketch_alive: False` / `stroke_once: False` while `/dev/cu.usbmodem21301` still
enumerates as `Arduino Generic CDC` -- the USB chip is alive, the ATmega328P is
not. It answers neither the sketch protocol nor the bootloader: a DTR toggle
produces no boot banner and `arduino-cli upload` hangs in avrdude sync with no
output. **Only a physical power cycle clears it.**

This ends soak runs as `NO_WAKE`, which reads exactly like a firmware wake
regression. Check `sketch_alive()` BEFORE blaming wake code -- and note that
`tap.py` reports success even when nothing is listening.

- Nutrition code removal + dish unification: DONE (2026-08-21), verified on
  hardware. Deleted `sense_mqtt.h` in full (MQTT existed only to receive dish
  results), the dish-result HTTP poller, `uart_send_ui_meal_result()`, the LCD
  meal-result screen, and the state behind them (`waiting_for_mqtt_result`,
  `g_dish_timing`, `active_dish_job_id`, `current_result_*`). Sense -1,896 bytes
  flash; **LCD -74,252 bytes** (`lv_font_montserrat_48` was only the calories
  label).

  **The important part was not the deletion.** Removing the dish special-casing
  exposed that `upload_queue_dish` was dequeued FIRST and UNCONDITIONALLY, ahead
  of the deferral gate, and that dish also jumped the queue via
  `xQueueSendToFront`. So dish uploads still ran mid-session and still fragmented
  the DMA region -- the exact failure HALO_DEFER_UPLOADS_TO_SLEEP exists to
  prevent. Re-reading the 2026-08-21 "6/6 PASS" log confirmed it: the defer
  counter never rose above 4 of 6, and presign ran in-session for both dish
  captures. That PASS was true about data and false about deferral.

  Fixed by merging to ONE queue with one gate for every mode. Re-verified:
  6 captures in a single boot, defer depth reaches `holding 6 upload(s)`, first
  TLS of the session is inside the sleep flush, `SLEEP_UPLOAD_FLUSH ... DRAINED`,
  `up_ok=6 up_fail=0 spool=0`, 0 camera init failures, 0 panics.

  **Regression caught in the same pass:** removing an argument from two
  `Serial.printf` calls without removing the matching `%d` shifted every later
  `%s` and made the `[SCAN] ignore menu select` printf dereference NULL --
  a hard `LoadProhibited` panic on every Discard. Arduino builds do not surface
  `-Wformat`, so this compiled clean and only appeared on hardware. When deleting
  a printf argument, delete its specifier in the same edit.

- OTA orchestration teardown: DONE. The 407-line orchestrator plus 24 further
  dead functions are gone (halo_sense_prod.ino 5,507 -> 4,696 lines), removed
  compiler-driven (-Wunused-function) and iterated to a fixed point. The binary is
  byte-identical, so this was hygiene rather than a size win. The nightly path was
  re-verified on hardware afterwards.
- Spool transfer during a *real* upload failure: verified. A forced upload failure
  spools the photo to SD (`saved=1`) instead of destroying it, and a later drain
  uploads and deletes it.
- Spool size/age caps: IMPLEMENTED and verified on hardware 2026-08-18. Count cap
  evicts to make room for the arriving image; age is checked BEFORE position, so a
  stale image is dropped in preference to a fresh one.
- Owner timezone: firmware side DONE 2026-08-20 (stored in NVS, applied at boot,
  verified end to end). Still Pacific in the field until the BACKEND returns a
  POSIX TZ at owner-claim — and note existing devices never re-claim, so the
  installed fleet needs a backfill path.
- Photo upload TLS: the S3 PUT now validates the peer via `tls_configure()` like
  presign/MQTT/OTA (was `setInsecure()`). Verified: PUT 200, heap delta +212 B,
  `dma_largest` 17396 — no regression on a path with a heap-panic history.
- After a **timer** wake the Sense never re-syncs with the LCD (`link_synced` is
  only set on receipt of `SYNC`). FIXED 2026-08-20: `halo_uart_link_recent()` no
  longer requires it. Most call sites were benign (one drives only a log line,
  another has a send-count override), but the PRE_SLEEP **LCD firmware version
  query** was hard-gated on it — so on the nightly path the Sense could never
  learn the LCD's version and therefore could never decide the LCD needed an
  update. Recent RX is stronger evidence than a possibly-stale SYNC flag, and the
  sibling `sense_link_recent()` already treated it that way.

- **Camera init: ROOT-CAUSED 2026-08-21. You get ONE reliable capture per boot.**

  Not intermittent, not random — deterministic. Every boot traces identically:

  ```
  BOOT n
    release <- camera_init        camera OK      <- 1st capture always works
    release <- presign_post
    REACQ FAIL <- presign_post                   <- reserve lost HERE
    !! not held at camera_init    camera FAIL    <- never recovered this boot
    REACQ FAIL <- camera_deinit
    REACQ FAIL <- provisioning_tls
  ```

  `esp_camera_init()` needs ONE contiguous **16,384**-byte internal DMA block.
  `g_camera_dma_reserve` banks it at boot. The first TLS handshake requires that
  block released (esp-aes needs ~25-30KB contiguous), and **after that first TLS
  the region is permanently fragmented for the rest of the boot** — ~40KB free but
  the largest block sits at 15,860, a mere 524 bytes short. Every re-acquire from
  every call site then fails until reboot.

  This is why sleeping between captures hides it (each wake is a fresh boot, so
  each is a "first capture") and why a mixed soak with several captures per wake
  looks terrible.

  **Four fixes were tried and all are DISPROVEN — do not repeat them:**
  - *Wait for memory to recover.* 3s wait was 0-for-3; `largest` never moved.
    Cut to 500ms and kept only as a diagnostic.
  - *Blame capture cadence / the harness.* Dish-only was 0 failures at BOTH
    aggressive and settled cadence. Cadence is not the variable.
  - *Blame a parked upload holding the block.* Measured false: the pre-camera
    drain succeeded **5/5** ("HTTP drained — safe to free DMA"), WiFi was torn
    down, and `dma_largest` was still 15,860. Raising
    `CAMERA_HTTP_DRAIN_MAX_MS` 1500->5000 was reverted as inert.

  - *Keep the camera initialised across captures* (`HALO_CAMERA_KEEP_INIT`). This
    one WORKED on its own terms — camera init failures 20-27 -> **0** across two
    soaks with genuine multi-capture-per-boot. It also caused **3 aborts in 12
    captures**: holding the camera's 16KB through the upload window pushed the
    Sense out of heap, decoded as
    `console_write -> uart_write -> _lock_acquire_recursive -> lock_init_generic
    -> abort()` — it could not allocate a MUTEX for a printf. A failed capture is
    an honest error; an abort reboots mid-operation. Reverted; flag left at 0 with
    the measurements, so nobody re-enables it hopefully.

  **Why they all fail the same way:** the camera needs ~16KB contiguous internal
  DMA, a TLS handshake needs ~25-30KB, and there is ~40KB. They cannot coexist.
  Re-ordering *when* the teardown happens does not create memory — it only moves
  which side loses.

  **The real fix is already designed, in `lcd_sdspool.h`'s own header:** capture
  writes to SD and returns; the upload drains later when the user has walked away.
  "Decoupled, they never contend." The spool path is built and now verified
  end-to-end — it simply is not the default. Making it the default removes the
  contention by construction instead of by timing.

  Failure mode is safe meanwhile: two attempts, honest
  `phase=ERROR "Camera init failed"`, next boot's capture works, no data loss.

- **LCD `ipc1` stack-canary panic at sleep: FIXED 2026-08-21.** Decoded backtrace:
  `ipc_task -> gpio_isr_register_on_core_static -> esp_intr_alloc ->
  heap_caps_malloc -> _xt_context_save -> STACK CANARY`. The Arduino core installs
  the GPIO ISR service lazily on the first `attachInterrupt()`, and the only one on
  this board arms the teardown touch watch *during the sleep transition* — so every
  boot, that install ran mid-sleep and ipc1's small stack overflowed when an
  interrupt landed inside its malloc. Now installed at boot, in a quiet interrupt
  environment. **Pre-existing, not new** — at ~8%/cycle the earlier "0 panics"
  soaks (~10 sleeps) were luck. Verified: 1/12 before (twice, including with the
  SD probe compiled out), **0 across 63 cycles + two full soaks** after.

- **SD spool: FIXED and VERIFIED end-to-end (2026-08-21).** It had never worked
  for a real capture, and the reason was not the card.

  The chase: `SD spool FAILED ... saved=0 / PHOTO_LOST`. `sdtest` then failed with
  `sdmmc_init_ocr: send_op_cond returned 0x107` (ESP_ERR_TIMEOUT) — but that was a
  **stuck card state that only a power cycle cleared**; an esptool hard reset did
  NOT clear it. An isolated sketch using the production `sd_card_bsp.cpp` verbatim
  mounted the card perfectly (`APPSD, SDSC, 480MB, bus_width=4`), proving card,
  wiring and driver were all fine.

  **Real root cause: the LCD slept through the transfer.** A ~175KB photo needs
  ~18-22s over the 115200 UART (512-byte chunks, one ACK each) but the LCD idles
  out in 10s. The Sense already blocked its own sleep
  (`[SLEEP_BLOCK] reason=spool_transfer`); the LCD had **no equivalent guard in
  either direction**, so it stopped servicing the link mid-transfer and both ends
  timed out:

  ```
  RX  : [IMG_RX] abort reason=frame_timeout      -> PHOTO_LOST
  drain: [SPOOL_DRAIN] reset state=3 reason=frame_timeout
  ```

  Two sleep paths needed guarding, not one — `lcd_sleep_intent_allowed()` in
  `lcd_activity.h` **and** the inactivity timeout in `loop()`. Guarding only the
  first is insufficient: `[SLEEP_DECISION] eligible=0` was logged while
  `[LOOP] Inactivity timeout - entering sleep...` slept anyway. Both now check
  `g_img_rx_active || g_spool_tx_active`.

  Timeouts were NOT the problem — raising them to 15s made it worse (392 B/s), and
  the final fix passes with the original 4000/3000 values.

  **Verified end to end:** capture -> forced upload failure -> `[IMG_SPOOL]
  result=OK bytes=175294 rate_Bps=9867` -> `SD spool OK saved=1` -> `drainreal` ->
  `[SPOOL_TX] result=OK` -> `[UPLOAD] PUT status: 200` -> `SPOOL_DELETE` ->
  `[SPOOL_TX] deleted slot=1`. No `PHOTO_LOST`.

  **SD health is now surfaced (2026-08-21).** `[SD_HEALTH] ok=N probe_ms=N
  writes=N fails=N`, plus `sd` / `sd_ms` / `sd_w` / `sd_wf` in `LCD_DIAG`. Probed
  once per boot (~85ms on a healthy card) and reported when the Sense is confirmed
  AWAKE. On failure it says so explicitly: *"SD spool is UNAVAILABLE; a failed
  upload will LOSE the photo."*

  Two things that had to be got right, both measured:
  - **Report on the way UP, not down.** The Sense sleeps on its own schedule and
    usually beats the LCD to it, so `LCD_DIAG` sent during the LCD's sleep
    sequence is lost: **19 sent / 1 received**, and still 2/0 after moving it
    earlier *within* that sequence. Sending once the Sense is confirmed awake:
    **4 sent / 4 received.**
  - **Drive it from `loop()`, not `sense_state_set()`** — that runs on
    `uart_task`, and an 85ms probe (or a multi-second timeout on a dead card)
    would stall UART RX, which has only ~89ms of headroom at 115200.
  - Do NOT reset the once-per-boot flag from `resetActivityTimer()`: it fires on
    every inbound UI_STATUS and turned one report into 18 sends across 3 cycles.

  `sdtest` on the LCD USB console remains the manual probe. Note a *stuck* card
  needs a power cycle — an esptool reset does not clear it.

- **AWS IoT certificate revocation (see §0) is still outstanding** and is a hard
  blocker: the key in the current ship binary is the one that was public.

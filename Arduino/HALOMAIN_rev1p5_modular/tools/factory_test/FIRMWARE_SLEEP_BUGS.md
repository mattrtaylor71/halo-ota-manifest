# HALO LCD Sleep-Coordinator Bugs — Handoff (2026-06-25)

Two LCD sleep-coordinator bugs surfaced while validating provisioning + the
factory test on device **`halo-16f8-2ca9`** (later re-flashed; provisioned as
`Trepo-Halo-0995-5D8C`, owner OK). Both are the sleep logic being **too
aggressive**: a force-sleep backstop overrides legitimate "stay awake" signals.
Firmware = `lcd-backlight-binary`, prod **6.1.815**. Relevant module:
`LCD_Minimal/lcd_sleep.h` + `lcd_activity.h` (guardian / sleep-coordination).

> **Are these "just factory testing"?** Partly — the factory test's timing
> (slow manual provisioning, rapid back-to-back captures, 45s passive upload
> waits) reliably *triggers* them. But the underlying logic is wrong for real
> use too (see each bug's "Real-world impact"). Notably, **both override
> `testmode`** — the test's own 1-hour keep-awake — so they aren't purely a test
> artifact; they defeat an explicit "do not sleep" instruction.

---

## Bug 1 — Guardian force-sleep WEDGE during provisioning (tight loop)

**Symptom:** If the unit sits on the provisioning screen for ~5 minutes, the LCD
drops into a tight retry loop that spams the same line **thousands of times/sec**
and wedges the UI — the QR never (re)appears and provisioning can't proceed. The
loop hit **`fail_count=36231`** in this capture; it ballooned a run log to 120k
lines / 7 MB in ~3 minutes.

**Trigger:** 5-minute guardian idle watchdog fires *while provisioning is active*.

**Evidence** (`logs/run_full_20260625-185440.log`, ~line 2855):
```
[19:01:20][LCD] [GUARDIAN] force_sleep elapsed_ms=300000     ← 5-min watchdog
[19:01:20][LCD] Preparing for DEEP SLEEP...
[19:01:20][LCD] [LCD] Sleep suppressed (provisioning active) ← correctly refuses
[19:01:20][LCD] [SLEEP] Sense sleep not confirmed - staying awake
[19:01:20][LCD] [SLEEP] no_ready_timeout backoff_ms=30000 fail_count=1 require_user=0
[19:01:20][LCD] Preparing for DEEP SLEEP...                  ← immediately re-fires
   ... loops thousands/sec, fail_count climbs to 36231+ ...
```

**Root-cause hypothesis:** When the guardian force-sleep is *suppressed* because
provisioning is active, the guardian's elapsed/attempt timer is **not reset** and
the computed `backoff_ms` (30000/60000) is **not actually applied as a delay** —
so the "prepare to sleep → suppressed → retry" path runs every loop iteration
instead of once per backoff window. It's a busy-loop, not a paced retry.

**Likely fix:** when sleep is suppressed (provisioning_active / require_user), (a)
reset the guardian elapsed timer so it doesn't re-fire immediately, and (b)
honor `backoff_ms` as a real wait before the next attempt. Provisioning should
simply *defer* the guardian, not spin it.

**Real-world impact:** a user who scans the QR slowly, gets interrupted, or sets
the device down on the pairing screen for 5 min will wedge it and have to
power-cycle. Medium-high severity.

**Workaround today:** provision within ~5 min of the QR appearing (we succeeded
in ~1 min on the clean run). If wedged: power-cycle, then provision promptly.

---

## Bug 2 — `deny_max_exceeded` force-sleep KILLS an in-flight upload

**Symptom:** During a capture's S3 upload, the LCD force-sleeps even though the
Sense is mid-upload and explicitly denying sleep — killing the upload and
dropping the USB/link. In the factory run this failed the Discard upload and
cascaded (`[Errno 6]`) into Check-in + Voice.

**Trigger:** LCD goes idle (no user activity) during the upload wait → wants to
sleep → asks Sense → Sense denies with `op_inflight` → after **10 denials** the
LCD's `deny_max_exceeded` backstop **forces sleep anyway**.

**Evidence** (`logs/run_full_20260625-190818.log`, ~line 3113):
```
[19:15:15][LCD] [SLEEP_PROTO] rx SLEEP_DENY reason=op_inflight retry_ms=5000  ← Sense: upload running
[19:15:15][LCD] [DISPLAY] idle_dark reason=op_inflight ...                    ← LCD idle (no activity)
[19:15:15][LCD] [SLEEP] deny_wait reason=op_inflight retry_ms=5000 count=1/10
   ... count climbs 1/10 → 10/10 ...
[19:15:15][LCD] [SLEEP] deny_max_exceeded count=10 - forcing sleep            ← overrides the deny
[19:15:15][LCD] [SENSE_STATE] state=ASLEEP reason=enter_sleep
[19:15:52]      STEP 25 FAIL Discard Upload: No UPLOAD_PUT within 45s         ← upload killed
```

**Root-cause hypothesis:** `deny_max_exceeded` exists to stop a buggy/hung peer
from blocking sleep forever. But `reason=op_inflight` is a *legitimate* "real
work in progress" denial — it should **never** be overridden by the force-sleep
backstop. The backstop is treating a valid busy signal like a stuck peer.

**Likely fix:** exempt `op_inflight` (and other known-good "real op" reasons)
from `deny_max_exceeded` — keep waiting (or extend the cap) while an operation is
genuinely in flight. Only force-sleep on denials with no backing work. Optionally
have the Sense send op progress so the LCD can distinguish "working" from "stuck."

**Real-world impact:** a single capture whose upload is slow (weak WiFi, large
image) while the user isn't touching the screen could be force-slept mid-upload,
silently losing the capture. The Dish upload here succeeded; only the later one,
after the LCD had gone idle-dark, was killed. Medium severity (data loss).

---

## Cross-cutting: both override `testmode`

The factory test sends `testmode` (1-hour keep-awake) before captures, yet both
force-sleep paths fired anyway. **`testmode` should be authoritative** — if it's
meant to inhibit sleep, the guardian and `deny_max_exceeded` backstops must check
it too. (Today they don't, which is why the device also "stayed awake" after a
*failed* run — the test's `testmodeoff` cleanup never ran — and then still
force-slept despite testmode being nominally active.)

## What's NOT broken
- Camera (clean EOL capture, `captures/eol_1782414539.jpg`), PWDN/heat, all
  inter-board wiring (INT/UART/HB), flashing, provisioning (WiFi join + creds
  ACK + owner claim), and the **first** Dish capture→S3 upload (PUT 200) all
  pass. These are sleep-coordination bugs only.

## Log references
- Bug 1 (guardian wedge): `tools/factory_test/logs/run_full_20260625-185440.log`
- Bug 2 (deny-max mid-upload): `tools/factory_test/logs/run_full_20260625-190818.log`
- Clean provisioning (for contrast): same 190818 log, ~19:10–19:13.

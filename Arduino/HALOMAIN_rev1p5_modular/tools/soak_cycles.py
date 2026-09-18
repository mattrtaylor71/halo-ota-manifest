#!/usr/bin/env python3
"""HALO duty-cycle soak: tap -> capture -> sleep -> deferred flush, repeated.

    python3 tools/soak_cycles.py 30

Lives in the repo, not the scratchpad, because /tmp gets cleared and this has now
been rewritten from scratch twice.

WHAT THIS MEASURES, AND WHY IT IS SHAPED THIS WAY
Each deep sleep is a FULL RESET, so RAM erosion cannot accumulate across cycles.
What CAN accumulate is anything that survives a reset: NVS, the SD spool,
RTC_NOINIT. So the value here is cycle COUNT plus the accounting invariant.

Three hard-won rules are encoded below. Each one cost a false verdict:

1. ACCOUNTING IS THE GATE: captures == device up_ok + spooled.
   Per-cycle counters, drop=0, PHOTO_LOST=0 and "DRAINED" were ALL green on
   2026-08-21 while three captures were being destroyed. The only signal was
   captures != uploads.

2. DO NOT COUNT UPLOADS BY GREPPING "PUT status: 200".
   That depends on the exact text surviving the UART. One line arrived as
   "[UPLOAD] PUT :57:05 GMT" -- truncated mid-line -- and failed an otherwise
   perfect 30-cycle run. The Sense writes its own up_ok/up_fail into the WAKELOG
   at cycle close; that is the source of truth.

3. DO NOT DETECT PANICS BY THEIR TEXT.
   The board can reset before "Guru Meditation" clears the serial buffer. A real
   lwIP/SNTP panic was scored as 0 panics that way. Detect the NEXT boot's
   reset_reason=ESP_RST_PANIC / panic_backoff / "assert failed:" instead.

Plus: the tap actuator dies silently (port stays enumerated, sketch stops
answering). That reads exactly like a firmware wake regression and ended two
runs early. On a wake failure this WAITS for the rig rather than quitting, so a
power-cycle resumes the run.
"""
import os, re, sys, threading, time
import serial

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import tapctl  # noqa: E402

SENSE = "/dev/cu.usbmodem1101"
CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 else 30
MODES = [("Check-in", 2), ("Discard", 1), ("Dish", 0)]
OUT = os.path.join(REPO, "tools", "eol_results")

buf = [""]
ser, stop, lock = {}, threading.Event(), threading.Lock()
rig_stalls = [0]


def reader():
    s = None
    while not stop.is_set():
        if s is None:
            if os.path.exists(SENSE):
                try:
                    s = serial.Serial(); s.port = SENSE; s.baudrate = 115200
                    s.timeout = 0.2; s.dtr = False; s.rts = False
                    s.open(); ser['s'] = s
                except Exception:
                    s = None; time.sleep(0.2); continue
            else:
                ser.pop('s', None); time.sleep(0.05); continue
        try:
            n = s.in_waiting
            if n:
                with lock: buf[0] += s.read(n).decode(errors="replace")
            else: time.sleep(0.02)
        except Exception:
            try: s.close()
            except Exception: pass
            s = None; ser.pop('s', None)


threading.Thread(target=reader, daemon=True).start()
def snap():
    with lock: return buf[0]

_tap = [tapctl.Tapper(verbose=False)]


def wait_for_rig(max_wait=36000):
    """Block until the actuator answers again.

    It has died mid-run more than once, each time leaving its port enumerated
    while the sketch answers nothing -- so it LOOKS healthy. Only a physical
    power cycle revives it. Ordinary no-reply failures may resume after recovery.
    Uncertain USB-worker ownership stays latched: stop for receipt review and
    restart the controller afterward instead of silently clearing that state.
    """
    t0 = time.time(); said = False
    while time.time() - t0 < max_wait:
        try:
            if _tap[0].sketch_alive():
                if said:
                    print(f"  [RIG] actuator back after {time.time()-t0:.0f}s", flush=True)
                return True
        except Exception:
            pass
        result = getattr(_tap[0], "last_result", None) or {}
        if result.get("status") == "REVIEW_REQUIRED":
            print("  [RIG] Stopped: review actuator ownership/cleanup receipt before "
                  "restarting this controller: " + result.get("receipt_dir", "unavailable"),
                  flush=True)
            return False
        if not said:
            print("  [RIG] *** TAP ACTUATOR NOT ANSWERING ***", flush=True)
            print("  [RIG] its port stays enumerated when this happens; this is NOT firmware.", flush=True)
            print("  [RIG] POWER-CYCLE THE ARDUINO UNO. Waiting...", flush=True)
            said = True; rig_stalls[0] += 1
        time.sleep(20)
        try:
            _tap[0].close()
        except Exception:
            pass
        try:
            _tap[0] = tapctl.Tapper(verbose=False)
        except Exception:
            pass
    return False


def wake(timeout=90):
    if os.path.exists(SENSE): return True
    t0 = time.time()
    while time.time() - t0 < timeout:
        ok = False
        try: ok = _tap[0].stroke_once()
        except Exception: ok = False
        if not ok:
            if wait_for_rig(): t0 = time.time(); continue
            return False
        w = time.time()
        while time.time() - w < 12:
            if os.path.exists(SENSE): return True
            time.sleep(0.25)
    try:
        if not _tap[0].sketch_alive() and wait_for_rig():
            return wake(timeout)
    except Exception:
        pass
    return False


def wait_asleep(timeout=240):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not os.path.exists(SENSE): return True
        time.sleep(0.5)
    return False


print(f"=== HALO soak: {CYCLES} duty cycles ===", flush=True)
print("waiting for a clean sleep to start from...", flush=True)
wait_asleep(240); time.sleep(3)

rows = []
for i in range(CYCLES):
    mode, idx = MODES[i % len(MODES)]
    with lock: mark = len(buf[0])
    t_cycle = time.time()

    if not wake():
        rows.append(dict(n=i+1, mode=mode, verdict="NO_WAKE")); break
    time.sleep(2)

    verdict = "NO_CAPTURE"
    for attempt in range(6):
        s = ser.get('s')
        if not s:
            time.sleep(2)
            if not wake(): break
            continue
        with lock: m2 = len(buf[0])
        try:
            s.write(('{"ver":1,"type":"INPUT_MENU_SELECT","menu_item":"%s","menu_index":%d,'
                     '"msg_id":%d,"ts":1000}\n' % (mode, idx, 9000 + i*10 + attempt)).encode())
            s.flush()
        except Exception:
            time.sleep(2); continue
        t1 = time.time()
        while time.time() - t1 < 95:
            new = snap()[m2:]
            if "ignore menu select" in new: time.sleep(12); break
            if "CAMERA] SUCCESS: Captured" in new: verdict = "OK"; break
            if re.search(r"Camera init failed: 0x[0-9a-f]+ attempt=2", new):
                verdict = "CAMERA_FAIL"; break
            if not os.path.exists(SENSE): verdict = "SLEPT_EARLY"; break
            time.sleep(0.2)
        if verdict != "NO_CAPTURE": break

    slept = wait_asleep(240)
    seg = snap()[mark:]

    def last_int(pat):
        m = re.findall(pat, seg)
        return int(m[-1]) if m else None

    rows.append(dict(
        n=i+1, mode=mode, verdict=verdict, slept=slept,
        secs=round(time.time() - t_cycle),
        heap=last_int(r"mem_free heap=(\d+)"),
        dma=last_int(r"dma_largest[=: ]+(\d+)"),
        upok=sum(int(x) for x in re.findall(r"up_ok=(\d+)", seg)),
        upfail=sum(int(x) for x in re.findall(r"up_fail=(\d+)", seg)),
        spool=len(re.findall(r"deferred_upload_spooled|spooled_to_sd", seg)),
        drop=sum(int(n) for n in re.findall(r"upload_defer_summary saved=\d+ dropped=(\d+)", seg)),
        panic=len(re.findall(r"Guru Meditation|Stack canary|abort\(\) was called|"
                             r"reset_reason=ESP_RST_PANIC|panic_backoff|assert failed:", seg)),
        camfail=len(re.findall(r"Camera initialization FAILED", seg)),
        drained=1 if "DRAINED" in seg else 0,
    ))
    r = rows[-1]
    print(f"  {r['n']:>3}/{CYCLES} {r['mode']:<9} {r['verdict']:<12} "
          f"slept={'Y' if r['slept'] else 'N'} {r['secs']:>3}s "
          f"up_ok={r['upok']} fail={r['upfail']} drop={r['drop']} drain={r['drained']} "
          f"heap={r['heap']} panic={r['panic']} camfail={r['camfail']}", flush=True)
    if r['verdict'] == "NO_WAKE" and not wait_for_rig():
        break
    time.sleep(2)

stop.set(); time.sleep(0.5)
try: _tap[0].close()
except Exception: pass
out = snap()
os.makedirs(OUT, exist_ok=True)
logp = os.path.join(OUT, "soak_cycles.log")
open(logp, "w").write(out)

caps = len(re.findall(r"CAMERA\] SUCCESS: Captured", out))
upok = sum(int(x) for x in re.findall(r"up_ok=(\d+)", out))
upfail = sum(int(x) for x in re.findall(r"up_fail=(\d+)", out))
spool = len(re.findall(r"deferred_upload_spooled|spooled_to_sd", out))
drop = sum(int(n) for n in re.findall(r"upload_defer_summary saved=\d+ dropped=(\d+)", out))
panic = len(re.findall(r"Guru Meditation|Stack canary|abort\(\) was called|"
                       r"reset_reason=ESP_RST_PANIC|panic_backoff|assert failed:", out))
camfail = len(re.findall(r"Camera initialization FAILED", out))
drained = len(re.findall(r"DRAINED", out))
put_lines = len(re.findall(r"PUT status: 200", out))

print("\n===== SOAK SUMMARY =====")
print(f"  cycles run            : {len(rows)}")
print(f"  rig stalls (actuator) : {rig_stalls[0]}")
print(f"  captures              : {caps}")
print(f"  uploaded (device up_ok): {upok}   (PUT-200 lines seen: {put_lines})")
print(f"  upload failures       : {upfail}")
print(f"  spooled to SD         : {spool}")
print(f"  deferred DROPPED      : {drop}")
print(f"  panics/asserts        : {panic}")
print(f"  camera init failures  : {camfail}")
print(f"  flushes DRAINED       : {drained}")
print(f"  cycles that slept     : {sum(1 for r in rows if r.get('slept'))}/{len(rows)}")


def trend(key, label):
    v = [r[key] for r in rows if r.get(key) is not None]
    if len(v) < 4:
        print(f"  {label:<21}: n={len(v)} (too few)"); return
    half = len(v)//2
    a, b = sum(v[:half])/half, sum(v[half:])/(len(v)-half)
    d = (b-a)/a*100 if a else 0
    print(f"  {label:<21}: {a:,.0f} -> {b:,.0f} ({d:+.1f}%)"
          + ("   <-- EROSION?" if d < -3 else ""))


trend('heap', 'free heap')
trend('dma', 'dma_largest')

accounted = upok + spool
print(f"\n  ACCOUNTING: {accounted}/{caps}   (captures must == up_ok + spooled)")
ok = (caps > 0 and accounted >= caps and upfail == 0 and drop == 0
      and panic == 0 and camfail == 0)
print(f"\n  ==> {'PASS' if ok else 'FAIL'}")
print(f"  log: {logp}")

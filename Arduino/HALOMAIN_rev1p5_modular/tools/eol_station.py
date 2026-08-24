#!/usr/bin/env python3
"""HALO end-of-line station — plug a unit in, get a verdict.

    python3 tools/eol_station.py                 # flash + full test
    python3 tools/eol_station.py --no-flash      # test what is already on it
    python3 tools/eol_station.py --serial A17    # label the unit in the report

WHY THIS EXISTS ALONGSIDE tools/factory_test/
  The web tester (localhost:9095) is the richer rig-bench tool: EOL test firmware,
  per-wire INT/UART line checks, PWDN heat safety. It also hard-codes the June
  rig's port map (LCD on usbmodem21201, usbmodem101 marked forbidden) and needs
  a browser. This is the hands-off production path: plug in, run one command,
  read PASS/FAIL.

BOARDS ARE IDENTIFIED BY FLASH SIZE, NEVER BY PORT NUMBER.
  Sense (XIAO S3) = 8MB, LCD = 16MB. Port numbers reassign whenever a board
  re-enumerates -- 101 and 1101 have already swapped once on this bench -- and
  the LCD image would FIT in the Sense's app partition, so a wrong guess would
  not fail safely. esptool flash-id is asked inside the same job that writes.

WHAT PASS MEANS
  Every capture the unit took is accounted for in S3 as a real object, not as a
  hopeful log line. Counting "PUT status: 200" has been wrong twice: once a
  truncated serial line hid a good upload, once a capture was destroyed while
  every device-side counter still read green.
"""
import argparse, glob, json, os, re, shutil, subprocess, sys, threading, time
import serial

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ET = os.path.expanduser("~/Library/Arduino15/packages/esp32/tools/esptool_py/5.2.0/esptool")
SENSE_FQBN = "esp32:esp32:XIAO_ESP32S3:PSRAM=opi"
LCD_FQBN = ("esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M,"
            "USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi")
SENSE_SKETCH = f"{REPO}/halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino"
LCD_SKETCH = f"{REPO}/halo_ota_demo/firmware/halo_lcd_prod/halo_lcd_prod.ino"
SENSE_BUILD, LCD_BUILD = "/tmp/halo_sense_ship", "/tmp/halo_lcd_ship"
AWS_PROFILE = "trepo"
BUCKETS = ["trepo-grocery-uploads-dev", "trepo-grocery-discards-dev"]
OUT = os.path.join(REPO, "tools", "eol_results")

steps = []
def step(name, ok, detail=""):
    steps.append((name, ok, detail))
    mark = {True: "PASS", False: "FAIL", None: "info"}[ok]
    print(f"  [{mark:>4}] {name}" + (f"  — {detail}" if detail else ""), flush=True)

# ───────────────────────── board identification ─────────────────────────
def flash_size(port):
    """Ask the chip its flash size. Also parks it in download mode, where it
    physically cannot deep-sleep -- which is why identification and writing must
    happen in the same job."""
    try:
        out = subprocess.run([ET, "--port", port, "--no-stub", "flash-id"],
                             capture_output=True, text=True, timeout=40).stdout
        m = re.search(r"Detected flash size:\s*(\S+)", out)
        return m.group(1) if m else None
    except Exception:
        return None

def identify(exclude=()):
    """Map ports -> {'sense': port, 'lcd': port} by flash size."""
    found = {}
    for p in sorted(glob.glob("/dev/cu.usbmodem*")):
        if p in exclude:
            continue
        sz = flash_size(p)
        if sz == "8MB" and "sense" not in found:
            found["sense"] = p
        elif sz == "16MB" and "lcd" not in found:
            found["lcd"] = p
    return found

# ───────────────────────── serial plumbing ─────────────────────────
class Tap:
    """Non-resetting reader. DTR/RTS are deasserted BEFORE open: asserting them
    resets an ESP32-S3 and throws away the state we came to observe."""
    def __init__(self, port):
        self.port, self.buf, self.stop = port, "", threading.Event()
        self.lock = threading.Lock()
        self.s = None          # live handle; writes MUST reuse it (see send())
        self.io = threading.Lock()   # pyserial handles are NOT thread-safe
        self.last_err = ''
        threading.Thread(target=self._run, daemon=True).start()
    def _run(self):
        s = None
        while not self.stop.is_set():
            if s is None:
                if os.path.exists(self.port):
                    try:
                        s = serial.Serial(); s.port = self.port; s.baudrate = 115200
                        s.timeout = 0.2; s.dtr = False; s.rts = False; s.open()
                        self.s = s
                    except Exception:
                        s = None; self.s = None; time.sleep(0.2); continue
                else:
                    time.sleep(0.1); continue
            try:
                with self.io:
                    n = s.in_waiting
                    data = s.read(n) if n else b""
                if data:
                    with self.lock: self.buf += data.decode(errors="replace")
                else: time.sleep(0.02)
            except Exception:
                try: s.close()
                except Exception: pass
                s = None; self.s = None
    def text(self):
        with self.lock: return self.buf
    def mark(self):
        with self.lock: return len(self.buf)
    def wait(self, pattern, timeout, since=0):
        rx, t0 = re.compile(pattern), time.time()
        while time.time() - t0 < timeout:
            if rx.search(self.text()[since:]): return True
            time.sleep(0.15)
        return False
    def send(self, obj):
        """Write through the reader's OWN handle.

        Opening a second connection to the same port while the reader thread
        holds it does not work -- the write silently goes nowhere, and every
        capture then times out looking like a firmware fault. Reuse the handle.
        """
        h = self.s
        if h is None:
            self.last_err = "no open handle"
            return False
        try:
            with self.io:
                h.write((json.dumps(obj) + "\n").encode()); h.flush()
            return True
        except Exception as e:
            self.last_err = repr(e)
            return False

# ───────────────────────── S3 verification ─────────────────────────
def s3_keys(device_id=None):
    keys = set()
    for b in BUCKETS:
        q = "Contents[].[Key]" if not device_id else \
            f"Contents[?contains(Key,'{device_id}')].[Key]"
        try:
            out = subprocess.run(["aws", "s3api", "list-objects-v2", "--bucket", b,
                                  "--query", q, "--output", "text",
                                  "--profile", AWS_PROFILE],
                                 capture_output=True, text=True, timeout=240).stdout
            for l in out.strip().split("\n"):
                if l.strip(): keys.add((b, l.strip()))
        except Exception:
            pass
    return keys

def s3_fetch(bucket, key, dest):
    try:
        subprocess.run(["aws", "s3", "cp", f"s3://{bucket}/{key}", dest,
                        "--profile", AWS_PROFILE, "--no-progress"],
                       capture_output=True, text=True, timeout=180)
        return os.path.exists(dest)
    except Exception:
        return False

# ───────────────────────── main ─────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-flash", action="store_true")
    ap.add_argument("--serial", default="")
    ap.add_argument("--timeout-wake", type=int, default=420)
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    label = a.serial or time.strftime("%H%M%S")

    print("=" * 68)
    print(f"HALO EOL STATION{'  — unit ' + a.serial if a.serial else ''}")
    print("=" * 68)

    # 0/1. wake + identify ------------------------------------------------
    # Counting /dev/cu.usbmodem* is NOT a wake check: the tap actuator is itself
    # a usbmodem device, so a sleeping unit plus an actuator looks like "two
    # boards present". Identify by flash size instead and let the RESULT decide
    # whether we still need to wake something.
    #
    # A factory-FRESH unit boots awake and is found on the first pass. A unit
    # that has been provisioned deep-sleeps, and deep sleep drops USB entirely,
    # so there is no port to probe until it is tapped.
    actuator = None
    try:
        sys.path.insert(0, os.path.join(REPO, "tools"))
        import tapctl
        actuator = getattr(tapctl, "ACT_PORT", None) or "/dev/cu.usbmodem21301"
    except Exception:
        pass

    print("\n[1] identify boards (by flash size, not port number)")
    ports = identify(exclude=(actuator,) if actuator else ())
    if "sense" not in ports or "lcd" not in ports:
        print("     incomplete — tapping to wake and retrying")
        try:
            import tapctl
            tp = tapctl.Tapper(verbose=False)
            if tp.sketch_alive():
                for _ in range(6):
                    tp.stroke_once(); time.sleep(8)
                    ports = identify(exclude=(actuator,) if actuator else ())
                    if "sense" in ports and "lcd" in ports: break
            tp.close()
        except Exception as e:
            print(f"     no actuator available ({e}) — tap the screen or replug")
    step("Sense detected (8MB)", "sense" in ports, ports.get("sense", "not found"))
    step("LCD detected (16MB)", "lcd" in ports, ports.get("lcd", "not found"))
    if "sense" not in ports or "lcd" not in ports:
        print("\n  Cannot continue without both boards.")
        return summarise(label, None)
    sense_port, lcd_port = ports["sense"], ports["lcd"]

    # 2. flash -----------------------------------------------------------
    if not a.no_flash:
        print("\n[2] flash  (verify size + write in ONE job, both boards in parallel)")
        # Sequential flashing loses a race: an idle board re-enumerates and
        # deep-sleeps in the gap between being identified and being written, and
        # the size probe then returns None so the safety guard (correctly)
        # refuses. esptool's FIRST contact parks the chip in download mode where
        # it physically cannot sleep, so identify-and-write must be one job, and
        # both boards must go at once.
        results = {}
        def verify_and_flash(board, port, fqbn, build, sketch, want):
            b = os.path.join(build, os.path.basename(sketch).replace(".ino", ".ino.bin"))
            if not os.path.exists(b):
                results[board] = (False, f"no prebuilt image at {build}"); return
            sz = None
            for _ in range(3):                     # port may churn as it resets
                sz = flash_size(port)
                if sz: break
                time.sleep(2)
            if sz != want:
                results[board] = (False, f"{port} reports {sz}, expected {want} — NOT writing")
                return
            r = subprocess.run(["arduino-cli", "upload", "-p", port, "--fqbn", fqbn,
                                "--input-dir", build, sketch],
                               capture_output=True, text=True, timeout=420, cwd=REPO)
            ok = "Hash of data verified" in (r.stdout + r.stderr) or r.returncode == 0
            results[board] = (ok, f"{port} ({sz})")
        ts = [threading.Thread(target=verify_and_flash, args=x) for x in [
            ("sense", sense_port, SENSE_FQBN, SENSE_BUILD, SENSE_SKETCH, "8MB"),
            ("lcd", lcd_port, LCD_FQBN, LCD_BUILD, LCD_SKETCH, "16MB")]]
        for t in ts: t.start()
        for t in ts: t.join()
        for board in ("sense", "lcd"):
            ok, d = results.get(board, (False, "did not run"))
            step(f"Flash {board}", ok, d)
        print("\n    settling after flash (the unit runs a maintenance cycle;")
        print("     the Sense stays awake but does NOT enumerate during it)")
        time.sleep(45)
    else:
        print("\n[2] flash — skipped (--no-flash)")

    # 3. wake / boot -----------------------------------------------------
    print("\n[3] boot")
    # Post-flash the unit runs a maintenance/OTA cycle for up to ~10 minutes. In
    # that window the Sense is AWAKE and talking over UART but does NOT enumerate
    # USB, and the LCD is pinned by `ota_stay_awake`. So:
    #   - waiting for the Sense's port to appear does not work, and
    #   - tapping does not help, because there is nothing to wake.
    # The reliable sequence is: wait for the unit to SETTLE (both boards drop
    # their ports = genuinely asleep), then tap it awake normally.
    def unit_ports():
        return [q for q in glob.glob("/dev/cu.usbmodem*") if q != actuator]

    up = os.path.exists(sense_port)
    if not up:
        print("     waiting for the post-flash maintenance cycle to finish...")
        t0 = time.time()
        settled = False
        while time.time() - t0 < a.timeout_wake:
            if os.path.exists(sense_port):
                up = True; break                      # came back on its own
            if not unit_ports():
                settled = True; break                 # asleep -> tappable
            time.sleep(2)
        if settled:
            print(f"     unit settled after {time.time()-t0:.0f}s — tapping to wake")
            try:
                import tapctl
                tp = tapctl.Tapper(verbose=False)
                for _ in range(6):
                    tp.stroke_once()
                    w = time.time()
                    while time.time() - w < 12:
                        if os.path.exists(sense_port): up = True; break
                        time.sleep(0.25)
                    if up: break
                tp.close()
            except Exception:
                pass
    step("Sense enumerated", up, sense_port if up else f"not within {a.timeout_wake}s")
    if not up:
        return summarise(label, None)
    sen, lcd = Tap(sense_port), Tap(lcd_port)
    time.sleep(6)

    both = sen.text() + lcd.text()
    fw = sorted(set(re.findall(r"fw=([0-9]+\.[0-9]+\.[0-9]+)", both)))
    step("Firmware version reported", bool(fw), ", ".join(fw) or "not seen")
    dev = re.search(r"device_id=(halo-[0-9a-z-]+)", both)
    device_id = dev.group(1) if dev else None
    step("Device ID", bool(device_id), device_id or "not seen yet")

    # 4. link ------------------------------------------------------------
    print("\n[4] inter-board link")
    m = lcd.mark()
    alive = lcd.wait(r"SENSE_LINK|LINK_HB|PROTO\] RX", 25, m)
    step("LCD <-> Sense UART alive", alive)

    # 5. captures --------------------------------------------------------
    print("\n[5] capture flows")
    before = s3_keys(device_id) if device_id else s3_keys()
    def ensure_awake():
        """The LCD sleeps on its own schedule, and deep sleep drops its USB port
        -- taking our write handle with it. Earlier this surfaced as three
        identical 'capture failed' lines that looked like a firmware fault and
        were really a sleeping bench unit. Tap it back before driving the UI."""
        if os.path.exists(lcd_port) and lcd.s is not None:
            return True
        try:
            import tapctl
            tp = tapctl.Tapper(verbose=False)
            for _ in range(6):
                tp.stroke_once()
                w = time.time()
                while time.time() - w < 12:
                    if os.path.exists(lcd_port) and lcd.s is not None:
                        tp.close(); time.sleep(2); return True
                    time.sleep(0.25)
            tp.close()
        except Exception:
            pass
        return os.path.exists(lcd_port) and lcd.s is not None

    modes = [("Check-in", 2), ("Discard", 1), ("Dish", 0)]
    taken = 0
    for i, (mode, idx) in enumerate(modes):
        if not ensure_awake():
            step(f"{mode} capture", False, "unit asleep and could not be tapped awake"); continue
        ms = sen.mark(); ml = lcd.mark()
        sent = lcd.send({"ver": 1, "type": "INPUT_MENU_SELECT", "menu_item": mode,
                         "menu_index": idx, "msg_id": 7700 + i, "ts": 1000})
        if not sent:
            step(f"{mode} capture", False, f"LCD write failed: {lcd.last_err}"); continue
        got = sen.wait(r"CAMERA\] SUCCESS: Captured", 90, ms)
        if got: taken += 1
        step(f"{mode} capture", got)
        if mode == "Check-in":
            time.sleep(2)
            lcd.send({"ver": 1, "type": "INPUT_EXPIRY_DATE", "expiry_date": "2026-12-31",
                      "quantity": 1, "msg_id": 7800 + i, "ts": 1000})
        elif mode == "Discard":
            time.sleep(2)
            lcd.send({"ver": 1, "type": "INPUT_DISCARD_OPTIONS",
                      "add_to_shopping_list": False, "msg_id": 7900 + i, "ts": 1000})
        step(f"{mode} UI reached Logged", lcd.wait(r"SHIP_LOGGED|phase=DONE", 60, ml), "")
        time.sleep(5)

    # 6. sleep + deferred flush -----------------------------------------
    print("\n[6] sleep + deferred upload flush")
    print("     (6.2.0 holds uploads until sleep — this is where they are sent)")
    t0 = time.time()
    while time.time() - t0 < 300:
        if not os.path.exists(sense_port) and "DRAINED" in sen.text(): break
        time.sleep(1)
    txt = sen.text()
    step("Device slept", not os.path.exists(sense_port))
    step("Flush drained", "DRAINED" in txt)
    upok = sum(int(x) for x in re.findall(r"up_ok=(\d+)", txt))
    upfail = sum(int(x) for x in re.findall(r"up_fail=(\d+)", txt))
    dropped = sum(int(n) for n in re.findall(r"upload_defer_summary saved=\d+ dropped=(\d+)", txt))
    panic = len(re.findall(r"Guru Meditation|Stack canary|abort\(\) was called|"
                           r"reset_reason=ESP_RST_PANIC|panic_backoff|assert failed:", txt))
    camfail = len(re.findall(r"Camera initialization FAILED", txt))
    step("No panics", panic == 0, f"{panic} found" if panic else "")
    step("No camera init failures", camfail == 0, f"{camfail} found" if camfail else "")
    step("No dropped captures", dropped == 0, f"{dropped} dropped" if dropped else "")
    step("Device upload count", upfail == 0, f"up_ok={upok} up_fail={upfail}")

    if not device_id:
        m = re.search(r"device_id=(halo-[0-9a-z-]+)", txt)
        device_id = m.group(1) if m else None

    # 7. S3 truth --------------------------------------------------------
    print("\n[7] verify in S3 (the real end of end-to-end)")
    time.sleep(12)
    after = s3_keys(device_id) if device_id else s3_keys()
    new = after - before
    originals = sorted(k for k in new if k[1].startswith("images/"))
    resized = [k for k in new if k[1].startswith("resized-images/")]
    step("Captures present in S3", len(originals) >= taken,
         f"{len(originals)} new original(s) for {taken} capture(s)")
    step("Backend processed them", len(resized) > 0,
         f"{len(resized)} resized derivative(s)")

    # 8. pull one image so a human can look at it ------------------------
    print("\n[8] retrieve a captured image")
    shot = None
    if originals:
        b, k = originals[-1]
        shot = os.path.join(OUT, f"eol_{label}_{os.path.basename(k)}")
        if s3_fetch(b, k, shot):
            step("Image downloaded", True, shot)
        else:
            step("Image downloaded", False, "fetch failed"); shot = None
    else:
        step("Image downloaded", False, "no new object to fetch")

    sen.stop.set(); lcd.stop.set()
    with open(os.path.join(OUT, f"eol_{label}_sense.log"), "w") as f: f.write(txt)
    return summarise(label, shot, device_id)

def summarise(label, shot, device_id=None):
    print("\n" + "=" * 68)
    print("EOL VERDICT" + (f" — {device_id}" if device_id else ""))
    print("=" * 68)
    hard = [s for s in steps if s[1] is False]
    for n, ok, d in steps:
        if ok is False: print(f"  FAIL  {n}  {d}")
    print(f"\n  {sum(1 for s in steps if s[1] is True)} passed, {len(hard)} failed")
    verdict = "PASS" if not hard else "FAIL"
    print(f"\n  ==> UNIT {verdict}")
    if shot: print(f"  image: {shot}")
    with open(os.path.join(OUT, f"eol_{label}.json"), "w") as f:
        json.dump({"label": label, "device_id": device_id, "verdict": verdict,
                   "image": shot,
                   "steps": [{"name": n, "ok": o, "detail": d} for n, o, d in steps]}, f, indent=2)
    return 0 if verdict == "PASS" else 1

if __name__ == "__main__":
    sys.exit(main())

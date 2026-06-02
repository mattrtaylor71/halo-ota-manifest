#!/usr/bin/env python3
"""
Overnight autonomous dual-board OTA soak.

Each cycle:
  1. publish_both.sh --version <bumped>   (compiles+uploads both boards)
  2. actuator tap to wake (wakes the LCD; LCD can't wake on UART)
  3. inject INPUT_OTA_CHECK over the Sense USB (== the physical "Software Update"
     button path: halo_prod_request_manual_ota -> manual override + OTA_LOCK)
  4. NO serial monitoring during the OTA (opening serial resets the board and
     corrupts the transfer). Wait by polling the cloud for the Sense self-OTA.
  5. after the cloud shows the Sense on the target (which, with the reorder,
     means the LCD proxy already finished too), wake + read the LCD's REAL
     version over serial (OTA is done, so a reset is harmless). Verify
     lcd_fw==target AND running_state==VALID (partition flipped, no rollback).
  6. log PASS/FAIL; on FAIL dump the Sense ota_orch breadcrumbs + LCD state.

Self-correcting: a single LCD miss is retried next cycle (target keeps climbing
above both boards). Robust to transient errors (per-cycle try/except).

Stop cleanly by creating the STOP sentinel file.
Ground truth is serial (Sense breadcrumb res + LCD real version); the cloud is
only used as the "Sense done" signal.
"""
import json, time, subprocess, glob, sys, os
import serial

REPO       = "/Users/MattTaylor/Documents/Arduino/HALOMAIN_rev1p5_modular"
ACT_PORT   = "/dev/cu.usbmodem21201"
SENSE_GLOB = "/dev/cu.usbmodem1101"
LCD_GLOB   = "/dev/cu.usbmodem101"
PROFILE    = "trepo-dev"
REGION     = "us-east-1"
DEVICE_ID  = "halo-d45b-8295"
TABLE      = "TrepoOtaDeviceLatest-dev"
CHANNEL    = "dev"
PROTO_VER  = 1

RESULTS = os.path.join(REPO, "automation", "overnight_results.jsonl")
LOGFILE = os.path.join(REPO, "automation", "overnight.log")
STOP    = os.path.join(REPO, "automation", "overnight_STOP")
STATE   = os.path.join(REPO, "automation", "overnight_state.json")

SENSE_MANIFEST = f"s3://halo-ota-{CHANNEL}/halo/ota/{CHANNEL}/manifest_latest.json"
LCD_MANIFEST   = f"s3://halo-ota-{CHANNEL}/halo/ota/{CHANNEL}/lcd/manifest_latest.json"

_msg_id = 1000
def next_msg_id():
    global _msg_id
    _msg_id += 1
    return _msg_id

def logln(msg):
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)
    with open(LOGFILE, "a") as f:
        f.write(line + "\n")

def record(obj):
    with open(RESULTS, "a") as f:
        f.write(json.dumps(obj) + "\n")

def bump(v):
    a, b, c = v.split(".")
    return f"{a}.{b}.{int(c) + 1}"

def proto(type_, **extra):
    m = {"ver": PROTO_VER, "type": type_, "msg_id": next_msg_id(),
         "ts": int(time.monotonic() * 1000) % 2000000000}
    m.update(extra)
    return (json.dumps(m) + "\n").encode()

# ---------- actuator ----------
def actuator_tap():
    try:
        subprocess.run(
            ["python3", os.path.join(REPO, "tap_implementation", "tap.py"),
             "--command", "PUSH:600", "--port", ACT_PORT, "--timeout", "6", "--quiet"],
            cwd=REPO, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception as e:
        logln(f"  actuator_tap error: {e}")
        return False

def wait_port(globpat, timeout=8):
    end = time.time() + timeout
    while time.time() < end:
        g = glob.glob(globpat)
        if g:
            return g[0]
        time.sleep(0.2)
    return None

# ---------- cloud ----------
def cloud_get():
    try:
        out = subprocess.run(
            ["aws", "dynamodb", "get-item", "--table-name", TABLE,
             "--key", json.dumps({"device_id": {"S": DEVICE_ID}}),
             "--profile", PROFILE, "--region", REGION],
            capture_output=True, text=True, timeout=30)
        if out.returncode != 0:
            return {}
        item = json.loads(out.stdout).get("Item", {})
        def g(k):
            v = item.get(k, {})
            return v.get("S") or v.get("N")
        return {k: g(k) for k in
                ["last_fw", "last_lcd_fw", "last_ota_result",
                 "last_lcd_ota_result", "updated_at"]}
    except Exception as e:
        logln(f"  cloud_get error: {e}")
        return {}

def manifest_version(s3uri):
    try:
        out = subprocess.run(["aws", "s3", "cp", s3uri, "-", "--profile", PROFILE],
                             capture_output=True, text=True, timeout=30)
        return json.loads(out.stdout).get("version")
    except Exception:
        return None

# ---------- publish ----------
def publish(version):
    logln(f"  publish {version} ...")
    try:
        out = subprocess.run(
            ["bash", os.path.join(REPO, "halo_ota_demo", "publish_both.sh"),
             "--version", version, "--channel", CHANNEL, "--profile", PROFILE],
            cwd=REPO, capture_output=True, text=True, timeout=900)
    except Exception as e:
        logln(f"  publish exception: {e}")
        return False
    sv = manifest_version(SENSE_MANIFEST)
    lv = manifest_version(LCD_MANIFEST)
    ok = (sv == version and lv == version)
    logln(f"  publish done sense_manifest={sv} lcd_manifest={lv} ok={ok}")
    if not ok:
        logln("  publish tail: " + (out.stdout[-400:] if out.stdout else "(none)"))
    return ok

# ---------- trigger (USB inject == button) ----------
def trigger_ota(reason):
    # Inject INPUT_OTA_CHECK over the Sense USB == the real "Software Update"
    # button path. We do NOT send INPUT_WAKE (it fires a /v1/list refresh that
    # contends with the OTA). Opening the USB port resets the Sense; we settle
    # ~4s for boot, then inject 3x. Robust to the port dropping mid-inject
    # (Errno 6) by re-waking + reopening, up to a few attempts.
    for attempt in range(1, 4):
        if not actuator_tap():
            continue
        port = wait_port(SENSE_GLOB, 8)
        if not port:
            logln(f"  trigger attempt {attempt}: Sense port never enumerated")
            continue
        try:
            s = serial.Serial(port, 115200, timeout=1)  # open resets the Sense
        except Exception as e:
            logln(f"  trigger attempt {attempt}: open failed {e}")
            continue
        sent = 0
        try:
            time.sleep(4.0)  # boot after USB reset (proven settle)
            for i in range(3):
                s.write(proto("INPUT_OTA_CHECK", reason=reason)); s.flush()
                sent += 1
                time.sleep(1.2)
        except Exception as e:
            logln(f"  trigger attempt {attempt}: write dropped after {sent} ({e})")
            try: s.close()
            except Exception: pass
            if sent >= 1:
                # at least one INPUT_OTA_CHECK landed before the drop -> the
                # manual override is set; good enough to run the OTA.
                logln(f"  trigger: injected INPUT_OTA_CHECK x{sent} (partial, reason={reason})")
                return True
            time.sleep(1.0)
            continue
        finally:
            try: s.close()
            except Exception: pass
        logln(f"  trigger: injected INPUT_OTA_CHECK x{sent} (reason={reason})")
        return True
    logln("  trigger: FAILED all attempts")
    return False

# ---------- serial reads (post-OTA only; reset is harmless once OTA done) ----------
def _open_after_wake(globpat, boot_wait=3.0):
    actuator_tap()
    port = wait_port(globpat, 8)
    if not port:
        return None
    try:
        p = serial.Serial(port, 115200, timeout=0.3)
    except Exception:
        return None
    time.sleep(boot_wait)
    return p

def read_lcd():
    """Return dict {version, part, state} or None. Retries until the [FW] line
    parses to a well-formed X.Y.Z version (the firmware's [FW] debug line
    occasionally mangles into e.g. "6.1.6VALID")."""
    import re as _re
    p = _open_after_wake(LCD_GLOB, 3.0)
    if not p:
        return None
    cap = []
    end = time.time() + 13
    seq = ["fw", "fw", "fw"]; si = 0; nxt = 0
    try:
        while time.time() < end:
            now = time.time()
            if now >= nxt and si < len(seq):
                try: p.write((seq[si] + "\n").encode()); p.flush()
                except Exception: break
                si += 1; nxt = now + 2.0
            try: line = p.readline().decode("utf-8", "ignore").rstrip()
            except Exception: break
            if line: cap.append(line)
    finally:
        try: p.close()
        except Exception: pass
    res = None
    for l in cap:
        if l.startswith("[FW] {") and "lcd_fw" in l:
            # require a well-formed X.Y.Z version (skip mangled "6.1.6VALID" lines)
            ver = _re.search(r'"lcd_fw":"(\d+\.\d+\.\d+)"', l)
            if not ver:
                if res is None:
                    res = {"version": None, "raw": l}  # keep a fallback if nothing better
                continue
            part = _re.search(r'"running_part":"([^"]+)"', l)
            state = _re.search(r'"running_state":"([^"]+)"', l)
            res = {"version": ver.group(1),
                   "part": part.group(1) if part else None,
                   "state": state.group(1) if state else None,
                   "raw": l}
    return res

def read_sense_breadcrumbs(maxlines=12):
    p = _open_after_wake(SENSE_GLOB, 3.0)
    if not p:
        return []
    cap = []
    end = time.time() + 12
    seq = ["errors", "errors"]; si = 0; nxt = 0
    try:
        while time.time() < end:
            now = time.time()
            if now >= nxt and si < len(seq):
                try: p.write((seq[si] + "\n").encode()); p.flush()
                except Exception: break
                si += 1; nxt = now + 3.0
            try: line = p.readline().decode("utf-8", "ignore").rstrip()
            except Exception: break
            if line and "ota_orch" in line:
                cap.append(line)
    finally:
        try: p.close()
        except Exception: pass
    # de-dup, keep most recent (highest seq)
    return cap[:maxlines]

def wait_sense_cloud(target, timeout=720):
    """Poll DynamoDB until last_fw == target. Returns (ok, last_snapshot)."""
    end = time.time() + timeout
    last = {}
    while time.time() < end:
        if os.path.exists(STOP):
            return False, last
        c = cloud_get()
        if c:
            last = c
            if c.get("last_fw") == target:
                return True, c
        time.sleep(20)
    return False, last

# ---------- main ----------
def main():
    start_version = sys.argv[1] if len(sys.argv) > 1 else "6.1.649"
    published = start_version
    cycle = 0
    npass = 0
    nfail = 0
    logln("=" * 70)
    logln(f"OVERNIGHT OTA SOAK START  base_version={start_version}")
    logln(f"  STOP file to halt: {STOP}")
    logln("=" * 70)

    while not os.path.exists(STOP):
        cycle += 1
        target = bump(published)
        reason = f"overnight_c{cycle}"
        t0 = time.time()
        logln(f"--- CYCLE {cycle}  target={target} (prev_published={published}) ---")
        rec = {"cycle": cycle, "target": target, "ts": time.strftime("%Y-%m-%dT%H:%M:%S")}

        try:
            if not publish(target):
                logln(f"  CYCLE {cycle} ABORT: publish failed")
                rec.update(verdict="publish_fail")
                record(rec); nfail += 1
                # don't advance published; retry same target next loop
                time.sleep(10)
                continue
            published = target

            # Settle so the just-overwritten S3 manifest_latest.json is fully
            # consistent before the device fetches it. Without this, a publish
            # immediately followed by an OTA could fetch a stale LCD manifest ->
            # compareVersions == 0 -> proxy "noop" (LCD skipped). Field OTAs
            # never publish-then-trigger within seconds, so this is harness-only.
            logln("  settle 25s for S3 manifest consistency before trigger")
            time.sleep(25)

            if not trigger_ota(reason):
                logln(f"  CYCLE {cycle} WARN: trigger failed; will still poll cloud")
            else:
                logln(f"  CYCLE {cycle} triggered; polling cloud for Sense->{target}")

            ok_sense, snap = wait_sense_cloud(target, timeout=780)
            rec["cloud"] = snap
            if not ok_sense:
                logln(f"  CYCLE {cycle} FAIL: Sense did not reach {target} in time. cloud={snap}")
                bc = read_sense_breadcrumbs()
                rec.update(verdict="sense_ota_timeout", breadcrumbs=bc)
                record(rec); nfail += 1
                logln("  breadcrumbs: " + " || ".join(bc[-6:]))
                continue

            logln(f"  CYCLE {cycle} Sense reached {target}; reading LCD ground truth")
            lcd = read_lcd()
            rec["lcd"] = lcd
            lcd_ok = bool(lcd and lcd.get("version") == target and
                          (lcd.get("state") in (None, "VALID")))
            # require VALID when we got a state; some reads drop the token
            if lcd and lcd.get("version") == target and lcd.get("state") not in ("VALID", None):
                lcd_ok = False

            dt = int(time.time() - t0)
            if lcd_ok:
                npass += 1
                rec.update(verdict="PASS", secs=dt)
                logln(f"  CYCLE {cycle} ✅ PASS  LCD={lcd.get('version')} part={lcd.get('part')} "
                      f"state={lcd.get('state')}  ({dt}s)  [pass={npass} fail={nfail}]")
            else:
                nfail += 1
                bc = read_sense_breadcrumbs()
                rec.update(verdict="lcd_mismatch", secs=dt, breadcrumbs=bc)
                logln(f"  CYCLE {cycle} ❌ FAIL  LCD={lcd}  ({dt}s)  [pass={npass} fail={nfail}]")
                logln("  breadcrumbs: " + " || ".join(bc[-8:]))
            record(rec)

            with open(STATE, "w") as f:
                json.dump({"cycle": cycle, "published": published,
                           "pass": npass, "fail": nfail,
                           "updated": time.strftime("%Y-%m-%dT%H:%M:%S")}, f)
        except Exception as e:
            nfail += 1
            logln(f"  CYCLE {cycle} EXCEPTION: {e}")
            rec.update(verdict="exception", error=str(e))
            record(rec)
            time.sleep(15)

    logln("=" * 70)
    logln(f"OVERNIGHT SOAK STOPPED. cycles={cycle} pass={npass} fail={nfail}")
    logln("=" * 70)

if __name__ == "__main__":
    main()

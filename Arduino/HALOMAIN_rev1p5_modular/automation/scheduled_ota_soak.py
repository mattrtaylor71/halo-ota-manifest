#!/usr/bin/env python3
"""
End-to-end SCHEDULED OTA soak — N cycles, each via the maintenance-window path.

Per cycle:
  1. publish_both.sh --version <bumped>
  2. delete stale schedule rows, write a fresh window (new request_id, start=now+LEAD)
     to DynamoDB TrepoOtaSchedules-dev
  3. actuator tap -> Sense fetches the schedule + arms its RTC wake timer (verify via
     the OtaScheduleApi log + a fresh device report)
  4. wait for the window: device auto-wakes at the window and OTAs; poll the cloud for
     last_fw == target (Sense applied on schedule)
  5. tap to wake (so the LCD is reachable) and poll for last_lcd_fw == target — the
     cloud LCD field self-corrects once both boards are awake
  6. verdict PASS iff BOTH boards reach target via the scheduled path

Validates the reboot-loop-guard fix (>=6.1.711): scheduled OTA must run repeatedly
without the guard re-tripping (deep-sleep wakes are clean boots that clear history).

STOP sentinel: automation/sched_STOP
"""
import json, time, subprocess, os, sys, glob
import serial

REPO    = "/Users/MattTaylor/Documents/Arduino/HALOMAIN_rev1p5_modular"
DEV     = "halo-d45b-8295"
OWNER   = "7d7df434-d942-4037-b054-2d3005ea6abc"
PROFILE = "trepo-dev"; REGION = "us-east-1"
REPORT_TBL = "TrepoOtaDeviceLatest-dev"
SCHED_TBL  = "TrepoOtaSchedules-dev"
SCHED_GET  = "https://7tn3gvwvh7.execute-api.us-east-1.amazonaws.com/ota/schedule"
SCHED_LG   = "/aws/lambda/trepo-grocery-backend-dev-OtaScheduleApiFunction-uTNwBpku0zPC"
ACT_PORT   = "/dev/cu.usbmodem21201"
LEAD_SEC   = 300      # window opens 5 min after we set it (time for fetch+arm+sleep)
DURATION   = 600      # 10 min window
N_CYCLES   = int(sys.argv[2]) if len(sys.argv) > 2 else 5

LOG  = os.path.join(REPO, "automation", "scheduled_ota.log")
RES  = os.path.join(REPO, "automation", "scheduled_ota_results.jsonl")
STOP = os.path.join(REPO, "automation", "sched_STOP")

def logln(m):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), m)
    print(line, flush=True)
    open(LOG, "a").write(line + "\n")

def rec(o): open(RES, "a").write(json.dumps(o) + "\n")

def bump(v):
    a, b, c = v.split("."); return "%s.%s.%d" % (a, b, int(c) + 1)

def aws(args, timeout=40):
    return subprocess.run(["aws"] + args + ["--profile", PROFILE, "--region", REGION],
                          capture_output=True, text=True, timeout=timeout)

def cloud():
    try:
        o = aws(["dynamodb", "get-item", "--table-name", REPORT_TBL,
                 "--key", json.dumps({"device_id": {"S": DEV}})])
        d = json.loads(o.stdout).get("Item", {})
        g = lambda k: (d.get(k, {}).get("S") or d.get(k, {}).get("N"))
        return {k: g(k) for k in ["last_fw", "last_lcd_fw", "last_ota_result",
                                  "last_lcd_ota_result", "updated_at"]}
    except Exception as e:
        return {"err": str(e)[:40]}

def manifest_ver(sub=""):
    uri = "s3://halo-ota-dev/halo/ota/dev/%smanifest_latest.json" % sub
    try:
        o = subprocess.run(["aws", "s3", "cp", uri, "-", "--profile", PROFILE],
                           capture_output=True, text=True, timeout=30)
        return json.loads(o.stdout).get("version")
    except Exception:
        return None

def publish(ver):
    logln("  publish %s ..." % ver)
    subprocess.run(["bash", os.path.join(REPO, "halo_ota_demo", "publish_both.sh"),
                    "--version", ver, "--channel", "dev", "--profile", PROFILE],
                   cwd=REPO, capture_output=True, text=True, timeout=900)
    ok = (manifest_ver() == ver and manifest_ver("lcd/") == ver)
    logln("  publish done ok=%s" % ok); return ok

def clear_schedules():
    try:
        o = aws(["dynamodb", "query", "--table-name", SCHED_TBL,
                 "--key-condition-expression", "device_id = :d",
                 "--expression-attribute-values", json.dumps({":d": {"S": DEV}})])
        for it in json.loads(o.stdout).get("Items", []):
            rid = it["request_id"]["S"]
            aws(["dynamodb", "delete-item", "--table-name", SCHED_TBL,
                 "--key", json.dumps({"device_id": {"S": DEV}, "request_id": {"S": rid}})])
    except Exception as e:
        logln("  clear_schedules err %s" % e)

def set_schedule(now):
    start = now + LEAD_SEC
    rid = "sched-%d-soak" % now
    iso = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))
    item = {"device_id": {"S": DEV}, "request_id": {"S": rid}, "owner_id": {"S": OWNER},
            "channel": {"S": "dev"}, "enabled": {"BOOL": True}, "status": {"S": "scheduled"},
            "start_epoch": {"N": str(start)}, "duration_sec": {"N": str(DURATION)},
            "grace_before_sec": {"N": "30"}, "grace_after_sec": {"N": "60"},
            "min_idle_min": {"N": "0"}, "created_at": {"S": iso}, "updated_at": {"S": iso}}
    aws(["dynamodb", "put-item", "--table-name", SCHED_TBL, "--item", json.dumps(item)])
    return start, rid

def sched_get_ok(fw):
    import urllib.request, urllib.parse
    q = urllib.parse.urlencode({"device_id": DEV, "owner_id": OWNER, "board": "sense",
                                "fw": fw, "channel": "dev", "device_type": "sense"})
    try:
        with urllib.request.urlopen("%s?%s" % (SCHED_GET, q), timeout=15) as r:
            d = json.loads(r.read().decode())
            return bool(d.get("enabled"))
    except Exception:
        return False

def tap():
    """Single actuator tap — one tap wakes the device."""
    try:
        subprocess.run(["python3", os.path.join(REPO, "tap_implementation", "tap.py"),
                        "--command", "PUSH:700", "--port", ACT_PORT, "--timeout", "7", "--quiet"],
                       cwd=REPO, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as e:
        logln("  tap err %s" % e)

def read_lcd_version():
    """Ground-truth the LCD version over serial (the cloud last_lcd_fw field lags
    after a scheduled OTA because the Sense can't re-query the asleep LCD). One tap
    to wake; a second only if the port doesn't enumerate."""
    import re as _re
    for attempt in range(2):
        tap()
        port = None
        for _ in range(20):
            g = glob.glob("/dev/cu.usbmodem101")
            if g: port = g[0]; break
            time.sleep(0.3)
        if not port:
            continue  # port didn't come up — tap again
        try:
            p = serial.Serial(port, 115200, timeout=0.3)
        except Exception:
            continue
        time.sleep(3.5)  # LCD boot after the USB-open reset
        ver = None; endt = time.time() + 9; nxt = 0; n = 0
        while time.time() < endt:
            if time.time() >= nxt and n < 3:
                try: p.write(b"fw\n"); p.flush()
                except Exception: break
                n += 1; nxt = time.time() + 2.0
            try: line = p.readline().decode("utf-8", "ignore").rstrip()
            except Exception: break
            if line and "[FW]" in line:
                m = _re.search(r'"lcd_fw":"(\d+\.\d+\.\d+)"', line)
                if m: ver = m.group(1)
        try: p.close()
        except Exception: pass
        if ver: return ver
    return None

def sched_api_hits(since_epoch):
    try:
        o = aws(["logs", "filter-log-events", "--log-group-name", SCHED_LG,
                 "--start-time", str(since_epoch * 1000)], timeout=40)
        return sum(1 for e in json.loads(o.stdout).get("events", []) if "START RequestId" in e["message"])
    except Exception:
        return -1

def poll_until(field, target, timeout_s, stop_check=True):
    end = time.time() + timeout_s; last = {}
    while time.time() < end:
        if stop_check and os.path.exists(STOP): return False, last
        c = cloud(); last = c
        if c.get(field) == target: return True, c
        time.sleep(30)
    return False, last

def main():
    cur = sys.argv[1] if len(sys.argv) > 1 else "6.1.712"
    npass = nfail = 0
    logln("=" * 64)
    logln("SCHEDULED OTA SOAK START base=%s cycles=%d lead=%ds" % (cur, N_CYCLES, LEAD_SEC))
    logln("=" * 64)
    # PREFLIGHT: don't start on a device that isn't settled on the expected base
    # version (e.g. mid-OTA, or split, or carrying a leftover/cached maintenance
    # window from a prior mid-window STOP). The operator must also confirm via a
    # Sense [TRUTH] read that maintenance_in_window=0 / next_ota_epoch=0 before a
    # restart-after-stop — a cloud schedule delete does NOT cancel a window the
    # device already fetched+armed.
    clear_schedules()
    pf = cloud()
    if pf.get("last_fw") != cur:
        logln("PREFLIGHT FAIL: cloud last_fw=%s != base %s — device not at clean baseline. ABORT."
              % (pf.get("last_fw"), cur)); return
    if pf.get("last_ota_result") == "pending" or pf.get("last_lcd_ota_result") == "pending":
        logln("PREFLIGHT FAIL: device shows OTA pending — not settled. ABORT."); return
    logln("preflight ok: device settled on %s (lcd=%s); 0 schedules pending" % (cur, pf.get("last_lcd_fw")))
    for cyc in range(1, N_CYCLES + 1):
        if os.path.exists(STOP): logln("STOP sentinel — halting"); break
        target = bump(cur); t0 = time.time()
        logln("--- SCHED CYCLE %d/%d  target=%s (cur=%s) ---" % (cyc, N_CYCLES, target, cur))
        r = {"cycle": cyc, "target": target, "ts": time.strftime("%Y-%m-%dT%H:%M:%S")}
        try:
            if not publish(target):
                logln("  CYCLE %d publish_fail" % cyc); r["verdict"] = "publish_fail"
                rec(r); nfail += 1; continue
            clear_schedules()
            now = int(time.time()); start, rid = set_schedule(now)
            logln("  schedule set: start=%s (+%ds) rid=%s" %
                  (time.strftime("%H:%M:%SZ", time.gmtime(start)), LEAD_SEC, rid))
            getok = sched_get_ok(cur)
            logln("  GET endpoint enabled=%s" % getok)
            tap()  # wake -> fetch + arm
            time.sleep(20)
            hits = sched_api_hits(now); snap = cloud()
            ack = (hits >= 1)
            logln("  ack: sched_api_hits=%s report_updated=%s -> acknowledged=%s" %
                  (hits, snap.get("updated_at"), ack))
            r["acknowledged"] = ack
            # wait for the window + scheduled OTA: Sense reaches target
            logln("  waiting for window (%s) + scheduled OTA -> sense %s ..." %
                  (time.strftime("%H:%M:%SZ", time.gmtime(start)), target))
            ok_s, snap = poll_until("last_fw", target, timeout_s=max(120, start - int(time.time())) + 900)
            r["cloud"] = snap
            if not ok_s:
                logln("  CYCLE %d FAIL: sense did not reach %s (scheduled OTA did not apply). cloud=%s"
                      % (cyc, target, snap))
                r["verdict"] = "sched_sense_fail"; rec(r); nfail += 1; cur = snap.get("last_fw") or cur; continue
            logln("  sense reached %s on schedule. verifying LCD (cloud, then serial ground-truth) ..." % target)
            # The cloud last_lcd_fw lags after a scheduled OTA (cold-link). Trust it if it
            # already shows target; otherwise read the LCD's REAL version over serial.
            c = cloud(); r["cloud"] = c
            if c.get("last_lcd_fw") == target:
                lcd_real = target; src = "cloud"
            else:
                lcd_real = read_lcd_version(); src = "serial"
            r["lcd_real"] = lcd_real; dt = int(time.time() - t0)
            if lcd_real == target:
                npass += 1; r["verdict"] = "PASS"; r["secs"] = dt
                logln("  CYCLE %d ✅ PASS both=%s (cloud_lcd=%s lcd_real=%s via %s) (%ds) [pass=%d fail=%d]"
                      % (cyc, target, c.get("last_lcd_fw"), lcd_real, src, dt, npass, nfail))
            else:
                # Sense updated on schedule but the LCD genuinely did NOT reach target.
                nfail += 1; r["verdict"] = "lcd_fail"; r["secs"] = dt
                logln("  CYCLE %d ❌ LCD_FAIL: sense=%s lcd_real=%s (GENUINE miss) (%ds) [pass=%d fail=%d]"
                      % (cyc, target, lcd_real, dt, npass, nfail))
            rec(r); cur = target
        except Exception as e:
            nfail += 1; logln("  CYCLE %d EXCEPTION %s" % (cyc, e))
            r["verdict"] = "exception"; r["error"] = str(e); rec(r); time.sleep(15)
    logln("=" * 64)
    logln("SCHEDULED OTA SOAK DONE pass=%d fail=%d (of %d)" % (npass, nfail, cyc))
    logln("=" * 64)

if __name__ == "__main__":
    main()

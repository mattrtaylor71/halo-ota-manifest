#!/usr/bin/env python3
"""
ota_loop_test.py — Bounded, attended LCD-OTA root-cause loop.

Per cycle:
  1. Read REAL baseline LCD version/partition/state via the `fw` USB command (port 101).
  2. Build + publish an LCD firmware at an incrementing version (LCD manifest ONLY,
     so the Sense never self-OTAs — isolates the LCD OTA path).
  3. Capture BOTH serial ports (Sense 1101 + LCD 101) to per-cycle log files,
     INCLUDING across the post-proxy LCD reboot (monitors auto-reconnect).
  4. Trigger manual OTA via the Sense USB command (INPUT_OTA_CHECK).
  5. Wait for the LCD to reboot (port drop+reappear) or timeout.
  6. Tap to wake, read REAL LCD version/partition/state again.
  7. Verdict: PASS iff running version == target AND running_state == VALID AND
     the running partition flipped. PENDING_VERIFY or old-version => FAIL/PARTIAL.
  8. Loop N cycles. Print a summary + keep all logs for FAIL cycles.

This is a MEASUREMENT tool (Phase 2 of the OTA root-cause plan). It does not change
firmware. The whole point is to capture the LCD's own boot log across the restart,
which past testing never did, and to confirm the REAL running version (not the
manifest-assumed value the Sense used to report).

NOTE: automation/config.json in this repo is stale (old repo path, wrong actuator
port, FlashSize=16M, the 900000 overflow hold) — so constants are hardcoded here
from the verified 2026-05-31 session instead.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from collections import deque

import serial

# ---- Verified constants (this device/session) ------------------------------
REPO = "/Users/MattTaylor/Documents/Arduino/HALOMAIN_rev1p5_modular"
HALO_OTA = os.path.join(REPO, "halo_ota_demo")
SENSE_PORT = "/dev/cu.usbmodem1101"
LCD_PORT = "/dev/cu.usbmodem101"
ACT_PORT = "/dev/cu.usbmodem21201"          # actuator (Arduino Uno) — NOT 21101/21301
CHANNEL = "dev"
PROFILE = "trepo-dev"
LCD_FQBN = ("esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M,"
            "USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi")
TAP_PY = os.path.join(REPO, "tap_implementation", "tap.py")
GEN_VER = os.path.join(HALO_OTA, "tools", "generate_version_header.py")
PUB_LCD = os.path.join(HALO_OTA, "tools", "ota", "publish_lcd_ota.py")
SHARED_VER = os.path.join(HALO_OTA, "firmware", "shared", "Version.h")
LCD_PROD_VER = os.path.join(HALO_OTA, "firmware", "halo_lcd_prod", "Version.h")
LCD_PROD_INO = os.path.join(HALO_OTA, "firmware", "halo_lcd_prod", "halo_lcd_prod.ino")
LOGDIR = os.path.join(REPO, "automation", "ota_loop_logs")

# The Sense drops any UART/USB message that fails validate_protocol_message()
# (sense_uart.h:266) — it REQUIRES ver==PROTOCOL_VERSION, type, msg_id, AND ts.
# A bare {"type":...} is silently rejected before the type dispatch.
PROTOCOL_VERSION = 1
_msg_id = [2000]


def next_msg_id():
    _msg_id[0] += 1
    return _msg_id[0]


def proto_msg(type_, **extra):
    """Build a well-formed protocol message the Sense will actually accept."""
    m = {"ver": PROTOCOL_VERSION, "type": type_,
         "msg_id": next_msg_id(), "ts": int(time.monotonic() * 1000) % 2000000000}
    m.update(extra)
    return (json.dumps(m) + "\n").encode()


def ts():
    return time.strftime("%H:%M:%S")


def log(msg):
    print(f"[{ts()}] {msg}", flush=True)


# ---- Serial monitor: threaded reader with auto-reconnect --------------------
class SerialMonitor:
    """Owns one serial port. A reader thread appends (mono_ts, line) to an
    in-memory deque and a logfile. Reconnects automatically when the port drops
    (LCD esp_restart, deep sleep, etc.). Thread-safe write()."""

    def __init__(self, port, name, logpath):
        self.port = port
        self.name = name
        self.logpath = logpath
        self.lines = deque(maxlen=8000)
        self._ser = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thr = None
        self.is_open = False
        self.connect_count = 0
        self.disconnect_count = 0
        self._fh = open(logpath, "a", buffering=1)

    def _open(self):
        s = serial.Serial()
        s.port = self.port
        s.baudrate = 115200
        s.timeout = 1
        s.dtr = False          # do NOT reset the board on connect
        s.rts = False
        s.open()
        return s

    def _record(self, line):
        t = time.monotonic()
        self.lines.append((t, line))
        self._fh.write(f"{time.strftime('%H:%M:%S')} | {line}\n")

    def _run(self):
        while not self._stop.is_set():
            try:
                with self._lock:
                    self._ser = self._open()
                    self.is_open = True
                    self.connect_count += 1
                self._record(f"<<< MONITOR connected #{self.connect_count} >>>")
                while not self._stop.is_set():
                    raw = self._ser.readline()
                    if raw == b"":
                        continue
                    line = raw.decode("utf-8", "ignore").rstrip("\r\n")
                    if line:
                        self._record(line)
            except Exception:
                if self.is_open:
                    self.disconnect_count += 1
                    self._record(f"<<< MONITOR disconnected #{self.disconnect_count} >>>")
                self.is_open = False
                with self._lock:
                    try:
                        if self._ser:
                            self._ser.close()
                    except Exception:
                        pass
                    self._ser = None
                time.sleep(0.5)

    def start(self):
        self._thr = threading.Thread(target=self._run, daemon=True)
        self._thr.start()

    def write(self, data):
        with self._lock:
            if self._ser and self.is_open:
                try:
                    self._ser.write(data)
                    self._ser.flush()
                    return True
                except Exception:
                    return False
        return False

    def wait_open(self, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.is_open:
                return True
            time.sleep(0.2)
        return self.is_open

    def wait_for(self, pattern, timeout, since):
        """Wait until a captured line at/after `since` matches `pattern` (regex).
        Returns the re.Match or None."""
        rx = re.compile(pattern)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for t, line in list(self.lines):
                if t >= since:
                    m = rx.search(line)
                    if m:
                        return m
            time.sleep(0.2)
        return None

    def stop(self):
        self._stop.set()
        if self._thr:
            self._thr.join(timeout=3)
        try:
            self._fh.close()
        except Exception:
            pass


# ---- Hardware helpers -------------------------------------------------------
def tap():
    try:
        subprocess.run(["python3", TAP_PY, "--command", "TAP", "--port", ACT_PORT, "--quiet"],
                       cwd=REPO, timeout=20, capture_output=True)
    except Exception as e:
        log(f"  tap error: {e}")


_VER_RX = re.compile(r"^\d+\.\d+\.\d+$")
_FW_RX = re.compile(r"\[FW\]\s*(\{.*\})")


_TRUTH_FW_RX = re.compile(r"\[TRUTH\].*?\bfw=(\d+\.\d+\.\d+)\b")


def _lcd_fw_once(lcd_mon, timeout=7):
    """Read the LCD `fw` status assuming the LCD is already awake (no tap).
    Returns parsed dict or None. Only accepts a fully-JSON line with a sane
    version + running_part (discards torn/interleaved serial lines)."""
    if not lcd_mon.wait_open(6):
        return None
    since = time.monotonic()
    lcd_mon.write(b"fw\n")
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for t, line in list(lcd_mon.lines):
            if t < since:
                continue
            m = _FW_RX.search(line)
            if not m:
                continue
            try:
                d = json.loads(m.group(1))
            except Exception:
                continue  # torn line — discard
            if (_VER_RX.match(str(d.get("lcd_fw", ""))) and d.get("running_part")
                    and d.get("running_state")):
                return d  # require a COMPLETE read (version+part+state) — reject torn lines
        time.sleep(0.2)
    return None


def read_lcd_fw(lcd_mon, taps=4, timeout=7):
    """Tap to (cold-)wake, then read the LCD's real running status. Returns dict or None."""
    for attempt in range(taps):
        tap()
        time.sleep(1.8)
        d = _lcd_fw_once(lcd_mon, timeout)
        if d:
            return d
    return None


def _sense_fw_since(sense_mon, since, timeout=6):
    """Read the Sense's running version from its [TRUTH] telemetry (fw=X.Y.Z),
    assuming it was just woken. Returns version string or None."""
    end = time.monotonic() + timeout
    latest = None
    while time.monotonic() < end:
        for t, line in list(sense_mon.lines):
            if t >= since:
                m = _TRUTH_FW_RX.search(line)
                if m:
                    latest = m.group(1)
        if latest:
            return latest
        time.sleep(0.3)
    return latest


def read_both(lcd_mon, sense_mon, taps=4):
    """Cold-wake once and read BOTH boards: LCD via `fw`, Sense via [TRUTH].
    Returns {lcd_fw, lcd_state, lcd_part, sense_fw} (values may be None)."""
    lcd = None
    sense_fw = None
    for attempt in range(taps):
        t_tap = time.monotonic()
        tap()
        time.sleep(1.8)
        lcd = _lcd_fw_once(lcd_mon, timeout=7)
        sense_fw = _sense_fw_since(sense_mon, t_tap, timeout=4)
        if lcd and sense_fw:
            break
    return {
        "lcd_fw": lcd.get("lcd_fw") if lcd else None,
        "lcd_state": lcd.get("running_state") if lcd else None,
        "lcd_part": lcd.get("running_part") if lcd else None,
        "sense_fw": sense_fw,
    }


def sense_wake(sense_mon):
    sense_mon.write(proto_msg("INPUT_WAKE"))


def wait_for_sleep(mons, timeout=90, stable=6.0):
    """Wait until ALL monitored ports are gone (boards deep-asleep / de-enumerated)
    and stay gone for `stable` seconds. A stylus tap only pulses the Sense wake GPIO
    when the LCD itself wakes FROM sleep — so we must let both boards fully sleep,
    then a single tap cold-wakes both. Returns True if sleep detected, else False."""
    end = time.monotonic() + timeout
    stable_since = None
    while time.monotonic() < end:
        if all(not m.is_open for m in mons):
            if stable_since is None:
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= stable:
                return True
        else:
            stable_since = None
        time.sleep(0.5)
    return False


def trigger_ota(sense_mon, reason):
    sense_mon.wait_open(8)
    sense_mon.write(proto_msg("INPUT_WAKE"))
    time.sleep(0.4)
    sense_mon.write(proto_msg("INPUT_OTA_CHECK", reason=reason))


def build_lcd(version):
    """Build the LCD firmware at `version` (mirrors publish_both.sh LCD half).
    Returns path to the .bin."""
    subprocess.run(["python3", GEN_VER, "--version", version, "--board", "lcd"],
                   cwd=HALO_OTA, check=True, capture_output=True)
    shutil.copy(SHARED_VER, LCD_PROD_VER)
    bp = f"/tmp/halo_lcd_loop_{version.replace('.', '_')}"
    subprocess.run(["arduino-cli", "compile", "--fqbn", LCD_FQBN,
                    "--build-path", bp, LCD_PROD_INO],
                   cwd=HALO_OTA, check=True)
    return os.path.join(bp, "halo_lcd_prod.ino.bin")


def publish_lcd(version, binpath):
    """Publish ONLY the LCD manifest (manifest_latest.json) — Sense untouched."""
    subprocess.run(["python3", PUB_LCD, "--channel", CHANNEL, "--version", version,
                    "--bin", binpath, "--profile", PROFILE],
                   cwd=HALO_OTA, check=True)


def publish_both(version):
    """Build + publish BOTH Sense and LCD manifests at `version` (publish_both.sh).
    The manual OTA will then self-update the Sense AND proxy the LCD."""
    subprocess.run(["bash", os.path.join(HALO_OTA, "publish_both.sh"),
                    "--version", version, "--channel", CHANNEL, "--profile", PROFILE],
                   cwd=HALO_OTA, check=True)


# ---- Version helpers --------------------------------------------------------
def parse_ver(v):
    try:
        a, b, c = (v or "").strip().split(".")
        return (int(a), int(b), int(c))
    except Exception:
        return None


def bump(v, by=1):
    a, b, c = parse_ver(v)
    return f"{a}.{b}.{c + by}"


# ---- Main loop --------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=5, help="number of OTA cycles")
    ap.add_argument("--start", default="auto",
                    help="first target version, or 'auto' = (running LCD version)+1")
    ap.add_argument("--ota-timeout", type=int, default=300,
                    help="seconds to wait for the OTA+reboot per cycle")
    ap.add_argument("--prebuild", action="store_true",
                    help="build all cycle binaries up front (tight loop) vs per-cycle")
    ap.add_argument("--stress-contention", action="store_true",
                    help="pump INPUT_WAKE during the transfer to induce the network "
                         "contention that previously stalled the OTA — proves the Fix-1 "
                         "list-refresh suppression holds (OTA should still PASS).")
    ap.add_argument("--stress-alt", action="store_true",
                    help="alternate: odd cycles run WITH contention stress, even cycles "
                         "run clean — confirms both paths pass (no regression).")
    ap.add_argument("--both-boards", action="store_true",
                    help="REALISTIC mode: publish BOTH Sense+LCD at the bumped version, "
                         "so the manual OTA self-updates the Sense AND proxies the LCD. "
                         "After each OTA: verify clean sleep -> tap-wake -> BOTH boards on target.")
    args = ap.parse_args()

    os.makedirs(LOGDIR, exist_ok=True)
    run_id = time.strftime("%Y%m%d_%H%M%S")
    log(f"=== OTA loop run {run_id}  cycles={args.cycles} ota_timeout={args.ota_timeout}s ===")

    # Persistent monitors on both ports for the whole run.
    sense_mon = SerialMonitor(SENSE_PORT, "sense",
                              os.path.join(LOGDIR, f"{run_id}_sense.log"))
    lcd_mon = SerialMonitor(LCD_PORT, "lcd",
                            os.path.join(LOGDIR, f"{run_id}_lcd.log"))
    sense_mon.start()
    lcd_mon.start()

    results = []
    try:
        # Baseline read
        if args.both_boards:
            log("Reading baseline (both boards)...")
            b = read_both(lcd_mon, sense_mon)
            if not b["lcd_fw"] or not b["sense_fw"]:
                log(f"!! baseline read failed: {b} — aborting.")
                return 2
            base_ver = max([b["lcd_fw"], b["sense_fw"]], key=parse_ver)
            log(f"Baseline: lcd={b['lcd_fw']} sense={b['sense_fw']} -> base={base_ver}")
            base = b
        else:
            log("Reading baseline LCD status...")
            base = read_lcd_fw(lcd_mon)
            if not base:
                log("!! Could not read baseline LCD fw — aborting.")
                return 2
            base_ver = base.get("lcd_fw")
            log(f"Baseline LCD: fw={base_ver} part={base.get('running_part')} "
                f"state={base.get('running_state')}")

        if args.start == "auto":
            target0 = bump(base_ver, 1)
        else:
            target0 = args.start
        targets = [bump(target0, i) for i in range(args.cycles)]
        log(f"Target versions: {targets}")

        prebuilt = {}
        if args.prebuild:
            for v in targets:
                log(f"Pre-building LCD {v} ...")
                prebuilt[v] = build_lcd(v)

        prev = base
        for i, target in enumerate(targets, 1):
            cycle_stress = args.stress_contention or (args.stress_alt and (i % 2 == 1))
            log(f"\n----- CYCLE {i}/{args.cycles}  target={target}  "
                f"mode={'STRESS' if cycle_stress else 'clean'} -----")
            cyc_start = time.monotonic()

            # ---- REALISTIC both-boards mode --------------------------------
            if args.both_boards:
                log(f"  publishing BOTH boards {target}")
                publish_both(target)
                pre = read_both(lcd_mon, sense_mon)
                log(f"  pre-OTA: lcd={pre['lcd_fw']}({pre['lcd_part']}/{pre['lcd_state']}) "
                    f"sense={pre['sense_fw']}")

                # deep sleep -> user taps to wake -> user "presses OTA button"
                log("  waiting for coordinated sleep before trigger...")
                wait_for_sleep([lcd_mon, sense_mon], timeout=90)
                t0 = time.monotonic()
                reg = None
                for attempt in range(4):
                    tap()
                    time.sleep(1.5)
                    t_send = time.monotonic()
                    trigger_ota(sense_mon, f"loop_{run_id}_c{i}")
                    reg = sense_mon.wait_for(
                        r"OTA_MANUAL\] request accepted|INPUT_OTA_CHECK received|OTA_POLICY\] allow=1",
                        12, t_send)
                    if reg:
                        break
                    log(f"  trigger not registered (attempt {attempt+1}/4) — re-waking")
                log(f"  trigger registered: {reg.group(0) if reg else 'NO'}")

                # Wait for the FULL dual OTA. Firmware sequence (observed): the Sense
                # self-OTAs and REBOOTS first (sets lcd_ota_due in NVS), then the
                # freshly-updated Sense proxies the LCD. The definitive completion is
                # proxy_result / OTA_UNLOCK. The mid-way Sense reboot is expected — the
                # monitor reconnects across it, so wait_for spanning it is fine.
                done = sense_mon.wait_for(r"proxy_result|OTA_UNLOCK", args.ota_timeout, t0)
                log(f"  OTA complete: {done.group(0) if done else 'TIMEOUT (no proxy_result)'}")
                time.sleep(8)  # let the LCD reboot to apply its new image

                # Verify it returns to sleep cleanly, then tap-wake and read both.
                log("  verifying clean post-OTA sleep...")
                slept = wait_for_sleep([lcd_mon, sense_mon], timeout=120)
                log(f"  post-OTA sleep: {'OK' if slept else 'NOT detected (120s)'}")
                post = read_both(lcd_mon, sense_mon)
                log(f"  post-OTA: lcd={post['lcd_fw']}({post['lcd_part']}/{post['lcd_state']}) "
                    f"sense={post['sense_fw']}")

                lcd_ok = post["lcd_fw"] == target and post["lcd_state"] == "VALID"
                sense_ok = post["sense_fw"] == target
                if lcd_ok and sense_ok:
                    verdict = "PASS"
                elif post["lcd_fw"] is None or post["sense_fw"] is None:
                    verdict = f"ERROR_NO_READ(lcd={post['lcd_fw']} sense={post['sense_fw']})"
                else:
                    verdict = (f"FAIL(lcd={post['lcd_fw']}/{post['lcd_state']} "
                               f"sense={post['sense_fw']} slept={slept})")

                dur = int(time.monotonic() - cyc_start)
                log(f"  CYCLE {i} VERDICT: {verdict}   (lcd={post['lcd_fw']} "
                    f"sense={post['sense_fw']}, {dur}s)")
                results.append({"cycle": i, "target": target, "verdict": verdict,
                                "pre": pre, "post": post, "registered": bool(reg),
                                "slept_after": slept, "dur_s": dur})
                prev = post
                continue
            # ---- end both-boards mode --------------------------------------

            # Build (if not prebuilt) + publish LCD-only manifest
            binpath = prebuilt.get(target) or build_lcd(target)
            log(f"  publishing LCD {target} -> channel={CHANNEL}")
            publish_lcd(target, binpath)

            pre = read_lcd_fw(lcd_mon) or prev
            pre_part = pre.get("running_part")
            pre_ver = pre.get("lcd_fw")
            log(f"  pre-OTA: fw={pre_ver} part={pre_part} state={pre.get('running_state')}")

            # Let BOTH boards fully sleep first, then the trigger's tap cold-wakes
            # both (LCD wake-from-sleep pulses the Sense wake GPIO). Without this,
            # a post-read-awake LCD swallows the tap and the Sense never wakes.
            log("  waiting for coordinated sleep before trigger...")
            if wait_for_sleep([lcd_mon, sense_mon], timeout=90):
                log("  both boards asleep -> cold-waking via tap")
            else:
                log("  sleep not detected within 90s -> proceeding (tap-retry fallback)")

            # Trigger manual OTA. Tap to (cold-)wake right before each send, and
            # retry until the trigger REGISTERS (request accepted / allow=1).
            log("  triggering manual OTA (INPUT_OTA_CHECK)")
            t0 = time.monotonic()
            reg = None
            for attempt in range(4):
                tap()                       # wake Sense + LCD
                time.sleep(1.5)
                t_send = time.monotonic()
                trigger_ota(sense_mon, f"loop_{run_id}_c{i}")
                reg = sense_mon.wait_for(
                    r"OTA_MANUAL\] request accepted|INPUT_OTA_CHECK received|OTA_POLICY\] allow=1",
                    12, t_send)
                if reg:
                    break
                log(f"  trigger not registered (attempt {attempt+1}/4) — re-waking + retrying")
            log(f"  trigger registered: {reg.group(0) if reg else 'NO (gave up after 4 tries)'}")

            # 2) Did the transfer actually START? (BEGIN/chunks/update-available)
            began = sense_mon.wait_for(
                r"LCD_OTA_BEGIN|update available|[Cc]hunk|TX MSG_CHUNK", 30, t0)
            log(f"  transfer started: {began.group(0) if began else 'NO (never began)'}")

            # 3) Wait for completion: a real LCD reboot (only meaningful if transfer
            #    began) OR a Sense proxy result.
            #    IMPORTANT: do NOT pump INPUT_WAKE here. The Sense already blocks
            #    sleep during OTA (sleep_block_reason=lcd_ota_proxy), and every
            #    INPUT_WAKE triggers a /v1/list HTTP refresh that contends with the
            #    OTA's S3 download on the Sense's single net stack — which starves
            #    the download and causes "stream stall (30s no data)" -> abort.
            dc0 = lcd_mon.disconnect_count
            done = None
            end = time.monotonic() + args.ota_timeout
            last_note = time.monotonic()
            last_wake = time.monotonic()
            while time.monotonic() < end:
                # Stress mode: re-induce the contention that USED to stall the OTA.
                # With Fix 1, the Sense defers /v1/list while OTA is active, so this
                # should NO LONGER starve the download -> OTA still completes.
                if cycle_stress and began and (time.monotonic() - last_wake > 2.0):
                    sense_wake(sense_mon)
                    last_wake = time.monotonic()
                if began and lcd_mon.disconnect_count > dc0:
                    done = "lcd_reboot"
                    break
                m = sense_mon.wait_for(
                    r"OTA_UNLOCK|sha_mismatch|chunk_retry_exhausted|lcd_ota.*(updated|success|fail)"
                    r"|Boot partition set|BOOT_PART_FAIL|Marking OTA partition", 2, t0)
                if m:
                    done = f"sense:{m.group(0)[:40]}"
                    break
                if time.monotonic() - last_note > 20:
                    last_note = time.monotonic()
                    log(f"    ... waiting ({int(time.monotonic()-t0)}s)")
            log(f"  OTA wait result: {done or 'timeout'}")

            # Let the new image boot + run self-test (mark_valid after display init)
            time.sleep(8)
            post = read_lcd_fw(lcd_mon)
            if post and post.get("running_state") == "PENDING_VERIFY":
                log("  state=PENDING_VERIFY — re-reading after settle for mark_valid")
                time.sleep(6)
                post = read_lcd_fw(lcd_mon) or post

            # Verdict
            if not post:
                verdict = "ERROR_NO_READ"
            else:
                pv = post.get("lcd_fw")
                pst = post.get("running_state")
                ppart = post.get("running_part")
                if pv == target and pst == "VALID" and ppart != pre_part:
                    verdict = "PASS"
                elif pv == target and ppart != pre_part:
                    verdict = f"PARTIAL(state={pst})"
                elif pv == pre_ver and not began:
                    verdict = "FAIL_NO_TRANSFER(OTA never started)"
                elif pv == pre_ver:
                    verdict = "FAIL_OLD_FW(transferred but not running — no apply / rollback)"
                else:
                    verdict = f"FAIL(fw={pv} state={pst} part={ppart})"

            dur = int(time.monotonic() - cyc_start)
            log(f"  CYCLE {i} VERDICT: {verdict}   "
                f"(post fw={post.get('lcd_fw') if post else '?'} "
                f"state={post.get('running_state') if post else '?'} "
                f"part={post.get('running_part') if post else '?'}, {dur}s)")
            results.append({
                "cycle": i, "target": target, "verdict": verdict,
                "pre": pre, "post": post, "began": bool(began),
                "registered": bool(reg), "done": done, "dur_s": dur,
            })
            prev = post or pre

        # Summary
        log("\n========== SUMMARY ==========")
        npass = sum(1 for r in results if r["verdict"] == "PASS")
        for r in results:
            log(f"  cycle {r['cycle']}: target={r['target']:<8} -> {r['verdict']}")
        log(f"  PASS {npass}/{len(results)}")
        log(f"  logs: {LOGDIR}/{run_id}_sense.log  +  {run_id}_lcd.log")
        summ = os.path.join(LOGDIR, f"{run_id}_summary.json")
        with open(summ, "w") as f:
            json.dump(results, f, indent=2, default=str)
        log(f"  summary: {summ}")
    finally:
        sense_mon.stop()
        lcd_mon.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())

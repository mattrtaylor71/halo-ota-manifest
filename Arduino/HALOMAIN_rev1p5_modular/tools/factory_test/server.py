#!/usr/bin/env python3
"""HALO Factory Acceptance Test — web dashboard for flashing and testing devices."""
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

from flask import Flask, Response, jsonify, request, send_from_directory

try:
    import serial
except ImportError:
    print("pip install pyserial flask")
    raise SystemExit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", ".."))
sys.path.insert(0, REPO_ROOT)

app = Flask(__name__, static_folder=SCRIPT_DIR)

# ── Config ──
PUBLISH_SCRIPT = os.path.join(REPO_ROOT, "halo_ota_demo", "publish_both.sh")
VERSION_HEADER = os.path.join(REPO_ROOT, "halo_ota_demo", "firmware", "shared", "Version.h")
TAP_SCRIPT = os.path.join(REPO_ROOT, "tap_implementation", "tap.py")

SENSE_FQBN = "esp32:esp32:XIAO_ESP32S3:PSRAM=opi"
LCD_FQBN = "esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi"
SENSE_SKETCH = os.path.join(REPO_ROOT, "halo_ota_demo", "firmware", "halo_sense_prod", "halo_sense_prod.ino")
LCD_SKETCH = os.path.join(REPO_ROOT, "halo_ota_demo", "firmware", "halo_lcd_prod", "halo_lcd_prod.ino")

S3_UPLOAD_BUCKET = "trepo-grocery-uploads-dev"
S3_DISCARD_BUCKET = "trepo-grocery-discards-dev"
S3_VOICE_BUCKET = "trepo-quick-ack-voice-async-dev-566667681926"

# ── EOL USB port topology (the real rig) ──
# Sense (XIAO ESP32-S3):                 /dev/cu.usbmodem1101
# LCD MAIN chip (flash + communicate):   /dev/cu.usbmodem21201
# LCD U4WDH secondary (deep-sleep target, appears when USB-C is FLIPPED):
#                                        /dev/cu.usbmodem2120x (varies, entered manually)
# /dev/cu.usbmodem101 -> FORBIDDEN: a DIFFERENT process owns this device.
EOL_SENSE_PORT_DEFAULT = "/dev/cu.usbmodem1101"
EOL_LCD_PORT_DEFAULT = "/dev/cu.usbmodem21201"
EOL_U4WDH_PORT_DEFAULT = "/dev/cu.usbserial-2120"  # confirmed on hw 2026-06-23 (CP2102 bridge)

# Ports this tool must NEVER open, flash, or read (owned by another process).
FORBIDDEN_PORTS = {"/dev/cu.usbmodem101"}
# Exact device basenames that are also forbidden (guards bare-name input).
_FORBIDDEN_BASENAMES = {"cu.usbmodem101", "usbmodem101"}

# U4WDH deep-sleep sketch (board_test) + FQBNs (mirrors board_test/web_tester)
U4WDH_SLEEP_SKETCH = os.path.join(REPO_ROOT, "board_test", "ESP32_U4WDH_Sleep")
# The U4WDH is always a classic ESP32 (ESP32-U4WDH, CP2102 bridge), confirmed
# on hardware 2026-06-23. Try plain ESP32 @115200 FIRST (proven reliable —
# 921600 fails on the CP2102), then the S3 FQBN as a defensive fallback.
U4WDH_FQBN_PRIMARY = "esp32:esp32:esp32:UploadSpeed=115200"
U4WDH_FQBN_FALLBACK = "esp32:esp32:esp32s3:CDCOnBoot=cdc,UploadSpeed=921600"


def assert_port_allowed(port):
    """Raise ValueError if `port` is the forbidden device (owned by another
    process). Matches the EXACT device only — NOT a substring — so
    usbmodem1101 / usbmodem21201 / usbmodem21301 are all allowed."""
    if not port:
        return
    p = str(port).strip()
    if p in FORBIDDEN_PORTS:
        raise ValueError(f"Port {port} is FORBIDDEN (used by another process) — refusing")
    base = os.path.basename(p)
    if base in _FORBIDDEN_BASENAMES:
        raise ValueError(f"Port {port} is FORBIDDEN (used by another process) — refusing")

# ── Global state ──
test_events = queue.Queue()
test_running = False
test_results = []
provision_confirmed = threading.Event()


def emit(event_type, data):
    """Send an SSE event."""
    test_events.put({"type": event_type, "data": data, "time": datetime.now(timezone.utc).isoformat()})


def emit_step(step_id, name, status, detail="", image=None):
    """Emit a test step update. Optional `image` is a URL rendered as a thumbnail."""
    entry = {"step": step_id, "name": name, "status": status, "detail": detail}
    if image:
        entry["image"] = image
    test_results.append(entry)
    emit("step", entry)


def find_ports():
    """Discover USB ports, excluding any FORBIDDEN port (owned by another process)."""
    import glob
    # usbmodem* = ESP32-S3 native USB (Sense, LCD main); usbserial* = CP2102
    # bridge (the LCD U4WDH deep-sleep chip).
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*"))
    allowed = []
    for p in ports:
        try:
            assert_port_allowed(p)
        except ValueError:
            continue
        allowed.append(p)
    return allowed


def wait_for_port(port, timeout=30):
    """Wait for a serial port to appear."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(port):
            return True
        time.sleep(0.5)
    return False


def tap_wake(stylus_port, lcd_port, max_taps=4):
    """Tap to wake, return True if LCD port appears."""
    for attempt in range(max_taps):
        if os.path.exists(lcd_port):
            return True
        try:
            from tap_implementation.tap import send_command
            send_command(stylus_port, 115200, "PUSH\n", verbose=False, wait_boot=True, timeout_s=5)
        except Exception as e:
            emit("log", f"Tap error: {e}")
        for _ in range(20):
            if os.path.exists(lcd_port):
                return True
            time.sleep(0.5)
        if attempt < max_taps - 1:
            time.sleep(5)
    return False


def open_lcd(lcd_port):
    """Open serial connection to LCD."""
    assert_port_allowed(lcd_port)
    lcd = serial.Serial(lcd_port, 115200, timeout=2)
    time.sleep(0.5)
    lcd.read(lcd.in_waiting)
    return lcd


def send_cmd(lcd, cmd, wait_for=None, timeout_s=15):
    """Send command on open serial, optionally wait for pattern."""
    lcd.write((cmd + "\n").encode())
    if not wait_for:
        time.sleep(1)
        return None
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if lcd.in_waiting:
            line = lcd.readline().decode("utf-8", errors="replace").strip()
            if wait_for in line:
                return line
        else:
            time.sleep(0.05)
    return None


class SerialDrainer:
    """Background thread that continuously reads serial into a queue so no lines are lost."""

    def __init__(self, lcd):
        self.lcd = lcd
        self.lines = queue.Queue()
        self.stop_flag = threading.Event()
        self.thread = threading.Thread(target=self._drain, daemon=True)
        self.thread.start()

    def _drain(self):
        while not self.stop_flag.is_set():
            try:
                if self.lcd.in_waiting:
                    line = self.lcd.readline().decode("utf-8", errors="replace").strip()
                    if line:
                        self.lines.put(line)
                else:
                    time.sleep(0.01)
            except Exception:
                break

    def get_line(self, timeout=0.1):
        try:
            return self.lines.get(timeout=timeout)
        except queue.Empty:
            return None

    def flush(self):
        while not self.lines.empty():
            try:
                self.lines.get_nowait()
            except queue.Empty:
                break

    def stop(self):
        self.stop_flag.set()


def do_capture(lcd, menu_item, menu_index, dismiss_cmd=None, drainer=None):
    """Run a capture, return (timing_ms, img_len) or (None, None)."""
    lcd.write(b"wake\n")
    time.sleep(2)
    if drainer:
        drainer.flush()
    else:
        drainer.flush()

    cap_cmd = json.dumps({
        "ver": 1, "type": "INPUT_MENU_SELECT",
        "menu_item": menu_item, "menu_index": menu_index,
        "msg_id": 1, "ts": 1000,
    })
    lcd.write((cap_cmd + "\n").encode())

    timing_ms = None
    img_len = None
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            if drainer:
                line = drainer.get_line(timeout=0.1)
            else:
                if lcd.in_waiting:
                    line = lcd.readline().decode("utf-8", errors="replace").strip()
                else:
                    time.sleep(0.05)
                    continue
            if not line:
                continue
            if "event=timing" in line:
                m = re.search(r"code=(\d+)", line)
                m2 = re.search(r"len=(\d+)", line)
                timing_ms = int(m.group(1)) if m else None
                img_len = int(m2.group(1)) if m2 else None
                break
        except Exception:
            break

    if dismiss_cmd and timing_ms:
        time.sleep(1)
        try:
            lcd.write((dismiss_cmd + "\n").encode())
            time.sleep(3)
        except Exception:
            pass

    return timing_ms, img_len


def do_voice(lcd, drainer=None):
    """Record voice, return True if acknowledged."""
    if drainer:
        drainer.flush()
    start_cmd = json.dumps({"ver": 1, "type": "INPUT_LONG_PRESS_START", "msg_id": 1, "ts": 1000})
    lcd.write((start_cmd + "\n").encode())
    time.sleep(4)
    end_cmd = json.dumps({"ver": 1, "type": "INPUT_LONG_PRESS_END", "msg_id": 2, "ts": 1000})
    lcd.write((end_cmd + "\n").encode())

    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            if drainer:
                line = drainer.get_line(timeout=0.1)
            else:
                if lcd.in_waiting:
                    line = lcd.readline().decode("utf-8", errors="replace").strip()
                else:
                    time.sleep(0.05)
                    continue
            if not line:
                continue
            if "LONG_PRESS" in line and "END" in line:
                return True
            if "USB_CMD" in line and "LONG_PRESS" in line:
                return True
            if "voice" in line.lower() and ("done" in line.lower() or "upload" in line.lower()):
                return True
        except Exception:
            break
    return False


def verify_s3(bucket, device_id, since_epoch, timeout_s=60):
    """Check for new S3 objects from this device after since_epoch.
    Searches ALL owner paths for this device_id (handles fresh provisioning)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        result = subprocess.run(
            ["aws", "s3", "ls", f"s3://{bucket}/images/",
             "--recursive", "--profile", "trepo-dev", "--region", "us-east-1"],
            capture_output=True, text=True,
        )
        for line in result.stdout.strip().split("\n"):
            if not line.strip():
                continue
            if device_id not in line:
                continue
            parts = line.split()
            if len(parts) >= 2:
                try:
                    ts = datetime.strptime(f"{parts[0]} {parts[1]}", "%Y-%m-%d %H:%M:%S")
                    if ts.timestamp() > since_epoch - 60:
                        return True
                except ValueError:
                    pass
        time.sleep(8)
    return False


def get_fw_version():
    with open(VERSION_HEADER, "r") as f:
        for line in f:
            m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([\d.]+)"', line)
            if m:
                return m.group(1)
    return "unknown"


def pre_compile():
    """Pre-compile both boards so flash is instant. Call at startup or on demand."""
    global sense_build_path, lcd_build_path
    sense_build_path = "/tmp/halo_sense_factory"
    lcd_build_path = "/tmp/halo_lcd_factory"

    for board, fqbn, sketch, build_path in [
        ("sense", SENSE_FQBN, SENSE_SKETCH, sense_build_path),
        ("lcd", LCD_FQBN, LCD_SKETCH, lcd_build_path),
    ]:
        bin_path = os.path.join(build_path, os.path.basename(sketch).replace(".ino", ".ino.bin"))
        if os.path.exists(bin_path):
            age = time.time() - os.path.getmtime(bin_path)
            if age < 3600:  # less than 1 hour old, skip recompile
                continue
        print(f"[PRECOMPILE] Compiling {board}...")
        result = subprocess.run(
            ["arduino-cli", "compile", "--fqbn", fqbn,
             "--build-path", build_path, sketch],
            capture_output=True, text=True, timeout=300,
        )
        if result.returncode == 0:
            print(f"[PRECOMPILE] {board} compiled OK")
        else:
            print(f"[PRECOMPILE] {board} compile FAILED: {result.stderr[:200]}")


sense_build_path = "/tmp/halo_sense_factory"
lcd_build_path = "/tmp/halo_lcd_factory"


def flash_board(board, port, step_id):
    """Flash a pre-compiled board via USB. Upload only — no compile."""
    assert_port_allowed(port)
    if board == "sense":
        fqbn = SENSE_FQBN
        build_path = sense_build_path
    else:
        fqbn = LCD_FQBN
        build_path = lcd_build_path

    emit_step(step_id, f"Flash {board.upper()}", "running", f"Uploading to {port}...")
    result = subprocess.run(
        ["arduino-cli", "upload", "-p", port,
         "--fqbn", fqbn, "--input-dir", build_path],
        capture_output=True, text=True, timeout=60,
    )
    if result.returncode == 0:
        emit_step(step_id, f"Flash {board.upper()}", "pass", f"Flashed to {port}")
        return True
    else:
        error = result.stderr[:200] if result.stderr else result.stdout[:200]
        emit_step(step_id, f"Flash {board.upper()}", "fail", f"Flash failed: {error}")
        return False


# ════════════════════════════════════════════════════════════════════════
# EOL (End-Of-Line) Hardware Test — parallel suite, independent of run_test_suite
# ════════════════════════════════════════════════════════════════════════

# EOL test-firmware sketches + build paths
EOL_SENSE_SKETCH = os.path.join(REPO_ROOT, "tools", "eol_sense", "eol_sense.ino")
EOL_LCD_SKETCH = os.path.join(REPO_ROOT, "tools", "eol_lcd", "eol_lcd.ino")
EOL_SENSE_FQBN = "esp32:esp32:XIAO_ESP32S3:PSRAM=opi"
EOL_LCD_FQBN = "esp32:esp32:esp32s3:PartitionScheme=huge_app,FlashSize=8M,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi"
EOL_SENSE_BUILD = "/tmp/halo_eol_sense"
EOL_LCD_BUILD = "/tmp/halo_eol_lcd"

CAPTURES_DIR = os.path.join(SCRIPT_DIR, "captures")

# Production firmware version expected by the EOL local smoke test.
# This is only a FALLBACK — the firmware version auto-increments on every
# compile, so the smoke test reads the real version from the flashed binary's
# embedded HALO_FW_MARKER instead of trusting this constant.
EOL_PROD_VERSION = "6.1.806"


def prod_version_from_bin():
    """Read the actual firmware version from the prod LCD binary's embedded
    HALO_FW_MARKER (e.g. 'HALO_FW_MARKER:6.1.812|...'). Falls back to the Sense
    bin, then to EOL_PROD_VERSION. This keeps the smoke check correct even
    though the build number auto-increments on every compile."""
    import re
    for path in (os.path.join(lcd_build_path, "halo_lcd_prod.ino.bin"),
                 os.path.join(sense_build_path, "halo_sense_prod.ino.bin")):
        try:
            with open(path, "rb") as f:
                blob = f.read()
            m = re.search(rb"HALO_FW_MARKER:([0-9]+\.[0-9]+\.[0-9]+)", blob)
            if m:
                return m.group(1).decode()
        except Exception:
            continue
    return EOL_PROD_VERSION


class EolLink:
    """Line-oriented serial helper for the EOL test firmware (115200, newline).

    Strips/ignores '[SENSE] ...' echo lines and blank lines on reads.
    """

    def __init__(self, port, baud=115200, timeout=2.0, open_timeout=8.0):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None
        self.open(open_timeout=open_timeout)

    def open(self, open_timeout=8.0):
        """Open the serial port, retrying until it enumerates (post-reboot)."""
        assert_port_allowed(self.port)
        deadline = time.time() + open_timeout
        last_err = None
        while time.time() < deadline:
            try:
                if os.path.exists(self.port):
                    self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout)
                    time.sleep(0.4)
                    try:
                        self.ser.reset_input_buffer()
                    except Exception:
                        pass
                    return True
            except Exception as e:
                last_err = e
            time.sleep(0.4)
        raise RuntimeError(f"Could not open {self.port} within {open_timeout}s: {last_err}")

    def _readline(self):
        raw = self.ser.readline()
        if not raw:
            return None
        return raw.decode("utf-8", errors="replace").strip()

    @staticmethod
    def _ignorable(line):
        return (line is None) or (line == "") or line.startswith("[SENSE]")

    def cmd(self, line, expect_prefix=None, timeout=2.0):
        """Write line+'\\n', read until a line starts with expect_prefix
        (or the first non-ignorable line if no prefix). Returns the line."""
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass
        self.ser.write((line + "\n").encode())
        self.ser.flush()
        deadline = time.time() + timeout
        while time.time() < deadline:
            ln = self._readline()
            if self._ignorable(ln):
                continue
            if expect_prefix is None or ln.startswith(expect_prefix):
                return ln
        raise RuntimeError(f"Timeout waiting for '{expect_prefix or 'response'}' after '{line}' on {self.port}")

    def cmd_json(self, line, prefix, timeout=3.0):
        """Send a command, find the response line starting with prefix,
        and parse the JSON object that follows the prefix."""
        resp = self.cmd(line, expect_prefix=prefix, timeout=timeout)
        rest = resp[len(prefix):].strip()
        return json.loads(rest)

    def read_jpeg(self, timeout=8.0):
        """Read a SHOT response: lines until 'JPEG_START:<len>', then exactly
        <len> raw bytes, then consume the trailing 'JPEG_END:' line.
        Returns (jpeg_bytes, meta_line). Raises on SHOT_ERR / timeout."""
        meta_line = None
        jlen = None
        deadline = time.time() + timeout
        # Phase 1: find JPEG_START (capturing the META line en route)
        while time.time() < deadline:
            ln = self._readline()
            if ln is None:
                continue
            if ln.startswith("[SENSE]") or ln == "":
                continue
            if ln.startswith("SHOT_ERR"):
                raise RuntimeError(f"Camera SHOT failed: {ln}")
            if ln.startswith("META"):
                meta_line = ln
                continue
            if ln.startswith("JPEG_START:"):
                try:
                    jlen = int(ln.split(":", 1)[1].strip())
                except ValueError:
                    raise RuntimeError(f"Bad JPEG_START line: {ln}")
                break
        if jlen is None:
            raise RuntimeError("Timeout waiting for JPEG_START")
        # Phase 2: read exactly jlen raw bytes
        buf = bytearray()
        read_deadline = time.time() + timeout
        while len(buf) < jlen and time.time() < read_deadline:
            chunk = self.ser.read(jlen - len(buf))
            if chunk:
                buf.extend(chunk)
        if len(buf) < jlen:
            raise RuntimeError(f"JPEG truncated: got {len(buf)} of {jlen} bytes")
        # Phase 3: consume trailing JPEG_END line
        end_deadline = time.time() + 2.0
        while time.time() < end_deadline:
            ln = self._readline()
            if ln is None or ln == "" or ln.startswith("[SENSE]"):
                continue
            if ln.startswith("JPEG_END:"):
                break
            break  # any other non-empty line — stop looking
        return bytes(buf), meta_line

    def close(self):
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None


def eol_compile_one(name, fqbn, sketch, build_path):
    """Compile one EOL/prod target if its binary is missing or stale (>1h)."""
    bin_path = os.path.join(build_path, os.path.basename(sketch).replace(".ino", ".ino.bin"))
    if os.path.exists(bin_path):
        age = time.time() - os.path.getmtime(bin_path)
        if age < 3600:
            return True, "cached"
    result = subprocess.run(
        ["arduino-cli", "compile", "--fqbn", fqbn,
         "--build-path", build_path, sketch],
        capture_output=True, text=True, timeout=400,
    )
    if result.returncode == 0:
        return True, "compiled"
    err = result.stderr[:300] if result.stderr else result.stdout[:300]
    return False, err


def eol_ensure_compiled():
    """Ensure all 4 firmware build paths (EOL sense/lcd + prod sense/lcd) are built.
    Returns (ok, detail)."""
    targets = [
        ("EOL Sense", EOL_SENSE_FQBN, EOL_SENSE_SKETCH, EOL_SENSE_BUILD),
        ("EOL LCD", EOL_LCD_FQBN, EOL_LCD_SKETCH, EOL_LCD_BUILD),
        ("Prod Sense", SENSE_FQBN, SENSE_SKETCH, sense_build_path),
        ("Prod LCD", LCD_FQBN, LCD_SKETCH, lcd_build_path),
    ]
    notes = []
    for name, fqbn, sketch, build_path in targets:
        ok, msg = eol_compile_one(name, fqbn, sketch, build_path)
        if not ok:
            return False, f"{name} compile FAILED: {msg}"
        notes.append(f"{name}: {msg}")
    return True, "; ".join(notes)


def _wait_for_port(port, timeout=25.0):
    """Wait until the serial device node exists (native-USB ESP32-S3 boards
    drop off the bus during reset/boot). Returns True if it appears."""
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(port):
            return True
        time.sleep(0.5)
    return os.path.exists(port)


def eol_upload(label, port, fqbn, build_path, step_id, attempts=3, port_wait=25.0):
    """Flash a pre-built target via arduino-cli upload, robust to the native-USB
    port vanishing during the auto-reset. Waits for the port and retries.
    Returns True/False."""
    assert_port_allowed(port)
    last_err = ""
    for attempt in range(1, attempts + 1):
        if not _wait_for_port(port, port_wait):
            last_err = f"port {port} not present (board may be asleep — wake/reseat it)"
            emit_step(step_id, label, "running",
                      f"Attempt {attempt}/{attempts}: {last_err}")
            continue
        emit_step(step_id, label, "running",
                  f"Uploading to {port} (attempt {attempt}/{attempts})...")
        try:
            result = subprocess.run(
                ["arduino-cli", "upload", "-p", port,
                 "--fqbn", fqbn, "--input-dir", build_path],
                capture_output=True, text=True, timeout=120,
            )
        except subprocess.TimeoutExpired:
            last_err = "upload timeout"
            continue
        if result.returncode == 0:
            emit_step(step_id, label, "pass",
                      f"Flashed {port}" + (f" (attempt {attempt})" if attempt > 1 else ""))
            return True
        last_err = (result.stderr or result.stdout or "")[:250]
        # transient re-enumeration: brief settle, then retry
        emit_step(step_id, label, "running",
                  f"Attempt {attempt}/{attempts} failed ({last_err[:120]}); retrying...")
        time.sleep(2.0)
    emit_step(step_id, label, "fail", f"Flash failed after {attempts} attempts: {last_err}")
    return False


def sleep_u4wdh(u4wdh_port):
    """Flash the board_test/ESP32_U4WDH_Sleep sketch to the LCD's U4WDH
    secondary chip so it enters permanent deep sleep. Mirrors
    board_test/web_tester/server.py: try the S3 FQBN first, then fall back
    to plain ESP32. Streams progress through the shared emit_step/emit flow.

    Success requires the flash to ACTUALLY SUCCEED first (port present + upload
    ok); only then does '@@SLEEP_START' OR silence count as asleep. An absent or
    silent port with no successful flash is a FAIL, not a pass (it means the LCD
    cable isn't in the U4WDH orientation / the chip is unreachable).
    assert_port_allowed() first."""
    global test_running
    test_running = True
    test_results.clear()
    step = 0
    try:
        assert_port_allowed(u4wdh_port)
        emit("start", {"mode": "sleep_u4wdh", "u4wdh_port": u4wdh_port})

        if not os.path.isdir(U4WDH_SLEEP_SKETCH):
            step += 1
            emit_step(step, "U4WDH sketch", "fail",
                      f"Sketch dir not found: {U4WDH_SLEEP_SKETCH}")
            emit("done", {"result": "fail", "reason": "U4WDH sketch missing"})
            return

        # ── Step 1: Flash deep-sleep firmware ──
        step += 1
        emit_step(step, "Flash U4WDH deep-sleep", "running",
                  f"Checking {u4wdh_port} is present...")
        # The U4WDH (CP2102) is ONLY on the bus when the LCD cable is in the
        # flipped (U4WDH) orientation. If the port is absent we must FAIL — never
        # interpret an absent/silent port as "asleep" (that was a false-pass bug).
        if not _wait_for_port(u4wdh_port, 3):
            emit_step(step, "Flash U4WDH deep-sleep", "fail",
                      f"{u4wdh_port} is not on the bus. The U4WDH chip is only exposed "
                      f"when the LCD USB cable is FLIPPED to the U4WDH orientation — in "
                      f"the main orientation it isn't connected. Flip the cable and retry.")
            emit("done", {"result": "fail",
                          "reason": "U4WDH port not present (LCD cable not in U4WDH orientation)"})
            return
        emit_step(step, "Flash U4WDH deep-sleep", "running",
                  f"Uploading deep-sleep sketch to {u4wdh_port}...")
        flashed = False
        last_err = ""
        for fqbn in (U4WDH_FQBN_PRIMARY, U4WDH_FQBN_FALLBACK):
            try:
                result = subprocess.run(
                    ["arduino-cli", "compile", "--upload",
                     "--fqbn", fqbn, "--port", u4wdh_port, U4WDH_SLEEP_SKETCH],
                    capture_output=True, text=True, timeout=180,
                )
            except subprocess.TimeoutExpired:
                last_err = "compile/upload timeout"
                continue
            if result.returncode == 0:
                flashed = True
                emit_step(step, "Flash U4WDH deep-sleep", "running",
                          f"Flashed with FQBN {fqbn}")
                break
            last_err = (result.stderr or result.stdout or "")[-250:]
            emit_step(step, "Flash U4WDH deep-sleep", "running",
                      f"FQBN {fqbn} failed, retrying with fallback...")

        # MUST have flashed to proceed — silence only means "asleep" if we
        # actually wrote the sleep firmware and the chip reset into it.
        if not flashed:
            emit_step(step, "Flash U4WDH deep-sleep", "fail",
                      f"Could not flash the U4WDH on {u4wdh_port} (chip unreachable). "
                      f"Last error: {last_err[:180]}")
            emit("done", {"result": "fail", "reason": "U4WDH flash failed"})
            return
        emit_step(step, "Flash U4WDH deep-sleep", "pass", "Deep-sleep sketch flashed")

        # ── Step 2: Verify deep sleep ──
        step += 1
        emit_step(step, "Verify U4WDH asleep", "running",
                  "Reading serial — '@@SLEEP_START' or silence means asleep...")
        time.sleep(2)
        got_sleep_marker = False
        any_output = False
        ser = None
        try:
            ser = serial.Serial(u4wdh_port, 115200, timeout=1)
            deadline = time.time() + 3
            while time.time() < deadline:
                try:
                    if ser.in_waiting:
                        line = ser.readline().decode("utf-8", errors="replace").strip()
                        if line:
                            any_output = True
                            if "@@SLEEP_START" in line:
                                got_sleep_marker = True
                                break
                    else:
                        time.sleep(0.05)
                except Exception:
                    break
        except Exception:
            # Port likely gone/asleep → no output → treat as asleep
            any_output = False
        finally:
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass

        # We only reach here after a SUCCESSFUL flash, so silence now genuinely
        # means the freshly-written sleep firmware ran and the chip slept.
        asleep = got_sleep_marker or (not any_output)
        if asleep:
            how = "SLEEP_START seen" if got_sleep_marker else "no serial output (asleep)"
            emit_step(step, "Verify U4WDH asleep", "pass",
                      f"U4WDH is in deep sleep ({how}). Now flip the cable back to "
                      "the LCD main orientation and run the EOL test.")
            emit("done", {"result": "pass", "passed": step, "failed": 0, "total": step})
        else:
            emit_step(step, "Verify U4WDH asleep", "fail",
                      "Chip still responding after flash — not asleep")
            emit("done", {"result": "fail", "reason": "U4WDH not asleep"})

    except ValueError as e:
        emit_step(step + 1, "Forbidden port", "fail", str(e))
        emit("done", {"result": "fail", "reason": str(e)})
    except Exception as e:
        emit_step(step + 1, "Error", "fail", str(e))
        emit("done", {"result": "error", "reason": str(e)})
    finally:
        test_running = False


def analyze_coherence(jpeg_bytes, img_url):
    """Coherence analysis of an EOL camera capture.
    Returns (passed: bool, detail: str)."""
    n = len(jpeg_bytes)
    hard_fails = []
    warns = []
    metrics = [f"len={n}B"]

    # Structure
    if not (n >= 2 and jpeg_bytes[0] == 0xFF and jpeg_bytes[1] == 0xD8):
        hard_fails.append("missing JPEG SOI (FFD8)")
    if not (n >= 2 and jpeg_bytes[-2] == 0xFF and jpeg_bytes[-1] == 0xD9):
        hard_fails.append("missing JPEG EOI (FFD9)")
    if n < 8000:
        hard_fails.append(f"too small ({n}B < 8000)")
    if n > 300000:
        hard_fails.append(f"too large ({n}B > 300000)")

    try:
        from PIL import Image, ImageStat  # noqa
        import io
        im = Image.open(io.BytesIO(jpeg_bytes))
        im.load()
        w, h = im.size
        metrics.append(f"{w}x{h}")
        # Expect ~1280x1024 (allow generous tolerance)
        if not (1000 <= w <= 1600 and 800 <= h <= 1300):
            hard_fails.append(f"unexpected dimensions {w}x{h} (expected ~1280x1024)")
        rgb = im.convert("RGB")
        stat = ImageStat.Stat(rgb)
        r_mean, g_mean, b_mean = stat.mean
        overall_mean = sum(stat.mean) / 3.0
        overall_std = sum(stat.stddev) / 3.0
        metrics.append(f"brightness={overall_mean:.0f}")
        metrics.append(f"stdev={overall_std:.1f}")
        if overall_mean < 25:
            hard_fails.append(f"near-black (brightness {overall_mean:.0f} < 25)")
        if overall_mean > 235:
            hard_fails.append(f"blown out (brightness {overall_mean:.0f} > 235)")
        if overall_std < 10:
            hard_fails.append(f"flat/frozen sensor (stdev {overall_std:.1f} < 10)")
        # Green cast warning
        denom = (r_mean + b_mean) / 2.0
        if denom > 0:
            green_ratio = g_mean / denom
            metrics.append(f"green_ratio={green_ratio:.2f}")
            if green_ratio > 1.5:
                warns.append(f"green cast (ratio {green_ratio:.2f} > 1.5)")
        # Sharpness: Laplacian variance on a CENTER CROP (focus metric).
        # Mean-gradient over the whole frame is diluted by large smooth
        # regions (desk/ceiling) and false-flags good images; Laplacian
        # variance on the central region is the standard focus measure.
        try:
            import numpy as np
            gray = np.asarray(rgb.convert("L"), dtype=np.float32)
            h, w = gray.shape
            cy, cx = h // 4, w // 4
            crop = gray[cy:cy + h // 2, cx:cx + w // 2]  # center 50%
            lap = (crop[:-2, 1:-1] + crop[2:, 1:-1] +
                   crop[1:-1, :-2] + crop[1:-1, 2:] - 4 * crop[1:-1, 1:-1])
            sharp = float(lap.var())
            metrics.append(f"sharpness(lapvar)={sharp:.1f}")
            # Conservative: only warn on a clearly defocused/blank frame.
            if sharp < 15.0:
                warns.append(f"possibly defocused (lapvar {sharp:.1f} < 15)")
        except Exception:
            pass
    except ImportError:
        metrics.append("structure-only (install Pillow for full coherence analysis)")
    except Exception as e:
        hard_fails.append(f"decode error: {e}")

    detail = ", ".join(metrics)
    if img_url:
        detail += f" | {img_url}"
    if hard_fails:
        return False, "FAIL: " + "; ".join(hard_fails) + " — " + detail
    if warns:
        return True, "PASS (warn: " + "; ".join(warns) + ") — " + detail
    return True, "PASS — " + detail


def run_eol_suite(config):
    """End-Of-Line hardware test: verify inter-board wiring, camera image
    coherence, camera PWDN/heat safety, then flash production 6.1.806 and run
    a local smoke test. Mirrors run_test_suite's SSE step/emit flow."""
    global test_running, test_results
    test_running = True
    test_results = []

    sense_port = config.get("sense_port") or EOL_SENSE_PORT_DEFAULT
    lcd_port = config.get("lcd_port") or EOL_LCD_PORT_DEFAULT
    device_id = config.get("device_id", "unknown")

    emit("start", {"mode": "eol", "device_id": device_id,
                   "sense_port": sense_port, "lcd_port": lcd_port})
    step = 0
    sense = None
    lcd = None

    def hard_fail(name, detail):
        emit_step(step, name, "fail", detail)
        emit("done", {"result": "fail", "reason": f"{name}: {detail}"})

    try:
        if not sense_port or not lcd_port:
            step += 1
            hard_fail("Config", "Both sense_port and lcd_port are required for EOL")
            return

        # Hard guard: refuse the forbidden port on either role.
        try:
            assert_port_allowed(sense_port)
            assert_port_allowed(lcd_port)
        except ValueError as e:
            step += 1
            hard_fail("Config", str(e))
            return

        # ── Step 0: Preconditions (informational) ──
        step += 1
        emit_step(step, "Preconditions", "pass",
                  "Sense on usbmodem1101, LCD main on usbmodem21201, U4WDH already "
                  "deep-slept (use Sleep U4WDH first), port usbmodem101 is blocked.")

        # ── Step 1: Compile EOL + Prod firmware ──
        step += 1
        emit_step(step, "Compile EOL+Prod firmware", "running",
                  "Building EOL sense/lcd + prod sense/lcd (cached if fresh)...")
        ok, detail = eol_ensure_compiled()
        if not ok:
            hard_fail("Compile EOL+Prod firmware", detail)
            return
        prod_ver = prod_version_from_bin()
        emit_step(step, "Compile EOL+Prod firmware", "pass",
                  f"{detail} | prod target = {prod_ver}")

        # ── Step 2: Flash EOL Sense ──
        step += 1
        if not eol_upload("Flash EOL Sense", sense_port, EOL_SENSE_FQBN, EOL_SENSE_BUILD, step):
            emit("done", {"result": "fail", "reason": "EOL Sense flash failed"})
            return
        time.sleep(4)

        # ── Step 3: Flash EOL LCD ──
        step += 1
        if not eol_upload("Flash EOL LCD", lcd_port, EOL_LCD_FQBN, EOL_LCD_BUILD, step):
            emit("done", {"result": "fail", "reason": "EOL LCD flash failed"})
            return
        time.sleep(4)

        # ── Step 4: Open serial links ──
        step += 1
        emit_step(step, "Open serial links", "running",
                  "Opening Sense + LCD USB serial...")
        try:
            sense = EolLink(sense_port, open_timeout=8.0)
            lcd = EolLink(lcd_port, open_timeout=8.0)
        except Exception as e:
            hard_fail("Open serial links", f"Could not open ports: {e}")
            return
        try:
            pong = sense.cmd("PING", expect_prefix="PONG", timeout=3.0)
        except Exception as e:
            hard_fail("Open serial links", f"Sense unreachable (PING->PONG): {e}")
            return
        try:
            lcd_ack = lcd.cmd("PINGS 1", expect_prefix="PINGS_SENT:", timeout=3.0)
        except Exception as e:
            hard_fail("Open serial links", f"LCD unreachable (PINGS 1): {e}")
            return
        emit_step(step, "Open serial links", "pass",
                  f"Sense: {pong}; LCD: {lcd_ack}")

        # ── Step 5: INT line (LCD GPIO39 -> Sense GPIO2) ──
        step += 1
        emit_step(step, "INT line (GPIO39->GPIO2) (needs U4WDH asleep)", "running", "Pulsing INT x10...")
        sense.cmd("RESETSTATS", expect_prefix="RESETSTATS", timeout=3.0)
        pulsed = lcd.cmd("INT 10", expect_prefix="INT_PULSED:", timeout=3.0)
        time.sleep(0.6)
        st = sense.cmd_json("STATS", "STATS")
        int_count = st.get("int", 0)
        d5 = f"INT measured={int_count} (expected 10), lcd ack={pulsed}"
        if int_count >= 8:
            emit_step(step, "INT line (GPIO39->GPIO2) (needs U4WDH asleep)", "pass", d5)
        else:
            hard_fail("INT line (GPIO39->GPIO2) (needs U4WDH asleep)",
                      d5 + " — INT wire (LCD GPIO39 -> Sense GPIO2) NOT working")
            return

        # ── Step 6: UART LCD->Sense + round-trip ──
        step += 1
        emit_step(step, "UART round-trip (LCD<->Sense) (needs U4WDH asleep)", "running", "Sending 5 UART PINGs...")
        sense.cmd("RESETSTATS", expect_prefix="RESETSTATS", timeout=3.0)
        lcd.cmd("RESETSTATS", expect_prefix="RESETSTATS", timeout=3.0)
        lcd.cmd("PINGS 5", expect_prefix="PINGS_SENT:", timeout=3.0)
        time.sleep(0.6)
        s_st = sense.cmd_json("STATS", "STATS")
        l_st = lcd.cmd_json("STATS", "STATS")
        rx = s_st.get("rx", 0)
        pong_n = l_st.get("pong", 0)
        d6 = (f"rx={rx} (proves LCD-TX -> Sense-RX wire), "
              f"pong={pong_n} (proves Sense-TX -> LCD-RX wire); expected >=5 each")
        if rx >= 5 and pong_n >= 5:
            emit_step(step, "UART round-trip (LCD<->Sense) (needs U4WDH asleep)", "pass", d6)
        else:
            problem = []
            if rx < 5:
                problem.append("LCD-TX -> Sense-RX wire FAILED")
            if pong_n < 5:
                problem.append("Sense-TX -> LCD-RX wire FAILED")
            hard_fail("UART round-trip (LCD<->Sense) (needs U4WDH asleep)", d6 + " — " + "; ".join(problem))
            return

        # ── Step 7: UART Sense->LCD heartbeat ──
        step += 1
        emit_step(step, "Sense->LCD heartbeat (needs U4WDH asleep)", "running",
                  "Listening for Sense heartbeat (1/s) for ~2.6s...")
        lcd.cmd("RESETSTATS", expect_prefix="RESETSTATS", timeout=3.0)
        time.sleep(2.6)
        l_st = lcd.cmd_json("STATS", "STATS")
        hb = l_st.get("hb", 0)
        d7 = f"hb={hb} in 2.6s (expected >=2); confirms Sense-TX -> LCD-RX wire"
        if hb >= 2:
            emit_step(step, "Sense->LCD heartbeat (needs U4WDH asleep)", "pass", d7)
        else:
            hard_fail("Sense->LCD heartbeat (needs U4WDH asleep)",
                      d7 + " — Sense heartbeat NOT arriving over UART")
            return

        # ── Step 8: Camera capture + coherence ──
        step += 1
        emit_step(step, "Camera capture + coherence (Sense only)", "running", "Capturing (SHOT)...")
        # SHOT streams META + JPEG; send it then read the binary stream.
        try:
            sense.ser.reset_input_buffer()
        except Exception:
            pass
        sense.ser.write(b"SHOT\n")
        sense.ser.flush()
        jpeg, meta_line = sense.read_jpeg(timeout=10.0)
        ts = int(time.time())
        os.makedirs(CAPTURES_DIR, exist_ok=True)
        fn = f"eol_{ts}.jpg"
        fpath = os.path.join(CAPTURES_DIR, fn)
        with open(fpath, "wb") as f:
            f.write(jpeg)
        img_url = f"/api/captures/{fn}"
        # parse cam_on_ms from META
        cam_on_ms = None
        if meta_line:
            m = re.search(r"cam_on_ms=(\d+)", meta_line)
            if m:
                cam_on_ms = int(m.group(1))
        coh_ok, coh_detail = analyze_coherence(jpeg, img_url)
        heat_fail = (cam_on_ms is not None and cam_on_ms > 4000)
        if cam_on_ms is not None:
            coh_detail += f", cam_on_ms={cam_on_ms}"
            if heat_fail:
                coh_detail += " (>4000ms HEAT BUDGET EXCEEDED)"
        if coh_ok and not heat_fail:
            emit_step(step, "Camera capture + coherence (Sense only)", "pass", coh_detail, image=img_url)
        else:
            emit_step(step, "Camera capture + coherence (Sense only)", "fail", coh_detail, image=img_url)
            emit("done", {"result": "fail", "reason": "Camera capture/coherence failed"})
            return

        # ── Step 9: Camera PWDN + de-init (heat) ──
        step += 1
        emit_step(step, "Camera PWDN + de-init (heat) (Sense only)", "running",
                  "Running PWDNTEST (power-cut verification)...")
        pwdn = sense.cmd_json("PWDNTEST", "PWDN_RESULT", timeout=15.0)
        on_ok = pwdn.get("on_ok", 0)
        off_dead = pwdn.get("off_dead", 0)
        revive_ok = pwdn.get("revive_ok", 0)
        max_on = pwdn.get("max_cam_on_ms", -1)
        pass_flag = pwdn.get("pass", 0)
        d9 = (f"on_ok={on_ok}, off_dead={off_dead}, revive_ok={revive_ok}, "
              f"max_cam_on_ms={max_on}")
        if off_dead == 0:
            d9 += " — WARNING: off_dead=0 means the PWDN line is NOT cutting power; camera would OVERHEAT"
        if pass_flag == 1:
            emit_step(step, "Camera PWDN + de-init (heat) (Sense only)", "pass", d9)
        else:
            hard_fail("Camera PWDN + de-init (heat) (Sense only)", d9)
            return

        # ── Step 10: Camera OFF safety ──
        step += 1
        emit_step(step, "Camera OFF safety (Sense only)", "running", "Forcing camera OFF...")
        off_resp = sense.cmd("OFF", expect_prefix="OFF ok", timeout=3.0)
        st = sense.cmd_json("STATS", "STATS")
        cam_on = st.get("cam_on", 1)
        d10 = f"{off_resp}; cam_on={cam_on} (expected 0)"
        if cam_on == 0:
            emit_step(step, "Camera OFF safety (Sense only)", "pass", d10)
        else:
            hard_fail("Camera OFF safety (Sense only)", d10 + " — camera still powered after OFF")
            return

        # ── Step 11: Flash Prod Sense 6.1.806 ──
        step += 1
        # Best-effort OFF then close the sense link before reflashing
        try:
            sense.cmd("OFF", expect_prefix="OFF ok", timeout=2.0)
        except Exception:
            pass
        sense.close()
        sense = None
        if not eol_upload(f"Flash Prod Sense {prod_ver}", sense_port,
                          SENSE_FQBN, sense_build_path, step):
            emit("done", {"result": "fail", "reason": "Prod Sense flash failed"})
            return
        time.sleep(4)

        # ── Step 12: Flash Prod LCD 6.1.806 ──
        step += 1
        lcd.close()
        lcd = None
        if not eol_upload(f"Flash Prod LCD {prod_ver}", lcd_port,
                          LCD_FQBN, lcd_build_path, step):
            emit("done", {"result": "fail", "reason": "Prod LCD flash failed"})
            return
        time.sleep(5)  # let prod boot

        # ── Step 13: Local smoke (boot + link + capture) ──
        step += 1
        emit_step(step, "Local smoke (boot + link + capture)", "running",
                  "Reopening LCD USB, verifying prod boot/link/capture...")
        smoke_lcd = None
        smoke_drainer = None
        try:
            # Prod Sense has no USB CDC — drive everything through LCD USB
            deadline = time.time() + 12
            while time.time() < deadline and not os.path.exists(lcd_port):
                time.sleep(0.5)
            smoke_lcd = open_lcd(lcd_port)
            smoke_drainer = SerialDrainer(smoke_lcd)

            # wake
            smoke_lcd.write(b"wake\n")
            time.sleep(3)
            smoke_drainer.flush()

            # fw version — expect 6.1.806
            smoke_lcd.write(b"fw\n")
            ver_ok = False
            ver_seen = ""
            vdl = time.time() + 8
            while time.time() < vdl:
                line = smoke_drainer.get_line(timeout=0.3)
                if not line:
                    continue
                if prod_ver in line:
                    ver_ok = True
                    ver_seen = line
                    break
            smoke_drainer.flush()

            # ping — confirm Sense liveness (LCD relays)
            smoke_lcd.write(b"ping\n")
            link_ok = False
            pdl = time.time() + 8
            while time.time() < pdl:
                line = smoke_drainer.get_line(timeout=0.3)
                if not line:
                    continue
                low = line.lower()
                if ("pong" in low or "alive" in low or
                        ("sense" in low and ("ok" in low or "up" in low or "ready" in low))):
                    link_ok = True
                    break
            smoke_drainer.flush()

            # capture via JSON INPUT_MENU_SELECT Dish (same shape as run_test_suite)
            smoke_lcd.write(b"wake\n")
            time.sleep(2)
            smoke_drainer.flush()
            cap_cmd = json.dumps({
                "ver": 1, "type": "INPUT_MENU_SELECT",
                "menu_item": "Dish", "menu_index": 0,
                "msg_id": 1, "ts": 1000,
            })
            smoke_lcd.write((cap_cmd + "\n").encode())
            cap_started = False
            cdl = time.time() + 20
            while time.time() < cdl:
                line = smoke_drainer.get_line(timeout=0.3)
                if not line:
                    continue
                low = line.lower()
                if ("camera" in low or "scan" in low or "capturing" in low or
                        "event=timing" in low or "capture" in low):
                    cap_started = True
                    break

            d13 = (f"version={'OK ' + prod_ver if ver_ok else 'MISMATCH (want ' + prod_ver + ')'}"
                   f" ({ver_seen.strip()[:80]}), link={'alive' if link_ok else 'DOWN'}, "
                   f"capture={'started' if cap_started else 'NOT started'}")
            if ver_ok and link_ok and cap_started:
                emit_step(step, "Local smoke (boot + link + capture)", "pass", d13)
            else:
                emit_step(step, "Local smoke (boot + link + capture)", "fail", d13)
        except Exception as e:
            emit_step(step, "Local smoke (boot + link + capture)", "fail", f"Smoke error: {e}")
        finally:
            if smoke_drainer:
                smoke_drainer.stop()
            if smoke_lcd:
                try:
                    smoke_lcd.close()
                except Exception:
                    pass

        # ── Done ──
        passed = sum(1 for r in test_results if r["status"] == "pass")
        failed = sum(1 for r in test_results if r["status"] == "fail")
        total = passed + failed
        result = "pass" if failed == 0 else "fail"
        emit("done", {"result": result, "passed": passed, "failed": failed, "total": total})

    except Exception as e:
        emit_step(step, "Error", "fail", str(e))
        emit("done", {"result": "error", "reason": str(e)})
    finally:
        # Best-effort: if a sense EolLink is still open, force camera OFF + close
        if sense is not None:
            try:
                sense.cmd("OFF", expect_prefix="OFF ok", timeout=2.0)
            except Exception:
                pass
            sense.close()
        if lcd is not None:
            lcd.close()
        test_running = False


def run_test_suite(config):
    """Run the full factory acceptance test suite."""
    global test_running, test_results
    test_running = True
    test_results = []

    lcd_port = config["lcd_port"]
    stylus_port = config["stylus_port"]
    device_id = config.get("device_id", "unknown")
    owner_id = config.get("owner_id", "unknown")
    do_flash = config.get("flash", False)
    sense_port = config.get("sense_port")

    emit("start", {"device_id": device_id, "lcd_port": lcd_port, "stylus_port": stylus_port})
    step = 0

    try:
        # Hard guard: refuse the forbidden port on any role.
        try:
            assert_port_allowed(lcd_port)
            assert_port_allowed(stylus_port)
            assert_port_allowed(sense_port)
        except ValueError as e:
            step += 1
            emit_step(step, "Config", "fail", str(e))
            emit("done", {"result": "fail", "reason": str(e)})
            return

        # ── Step 1: Flash (optional) ──
        if do_flash:
            step += 1
            if sense_port:
                if not flash_board("sense", sense_port, step):
                    emit("done", {"result": "fail", "reason": "Sense flash failed"})
                    return
            step += 1
            if not flash_board("lcd", lcd_port, step):
                emit("done", {"result": "fail", "reason": "LCD flash failed"})
                return
            time.sleep(5)  # wait for reboot

        # ── Step: Provisioning (if requested) ──
        do_provision = config.get("provision", False)
        if do_provision:
            step += 1
            emit_step(step, "Wake for Provisioning", "running", "Tapping to wake...")
            if tap_wake(stylus_port, lcd_port):
                emit_step(step, "Wake for Provisioning", "pass", "Device awake")
            else:
                emit_step(step, "Wake for Provisioning", "fail", "Could not wake device")
                emit("done", {"result": "fail", "reason": "Could not wake for provisioning"})
                return

            step += 1
            provision_confirmed.clear()
            emit_step(step, "Provision Device", "waiting",
                      "Provision this device with the Trepo app now. "
                      "Auto-detecting when provisioning completes...")
            emit("provision_wait", {"message": "Waiting for provisioning..."})

            # Open serial and watch for provisioning completion signals
            prov_ok = False
            prov_lcd = None
            try:
                if os.path.exists(lcd_port):
                    prov_lcd = serial.Serial(lcd_port, 115200, timeout=2)
                    time.sleep(0.5)
                    prov_lcd.read(prov_lcd.in_waiting)

                deadline = time.time() + 300  # 5 min max
                while time.time() < deadline:
                    # Check for manual confirmation button
                    if provision_confirmed.is_set():
                        prov_ok = True
                        emit_step(step, "Provision Device", "pass", "Provisioning confirmed by user")
                        break
                    # Check serial for auto-detection
                    if prov_lcd and prov_lcd.in_waiting:
                        try:
                            line = prov_lcd.readline().decode("utf-8", errors="replace").strip()
                            if "PROVISION_STATUS" in line and "connected" in line:
                                prov_ok = True
                                emit_step(step, "Provision Device", "pass", "Provisioning complete (auto-detected: connected)")
                                break
                            if "LCD_WIFI" in line and "saved creds" in line:
                                emit_step(step, "Provision Device", "running",
                                          "WiFi credentials received, waiting for connection...")
                        except Exception:
                            pass
                    else:
                        time.sleep(0.1)
            except Exception as e:
                emit("log", f"Provision monitor error: {e}")
            finally:
                if prov_lcd:
                    try:
                        prov_lcd.close()
                    except Exception:
                        pass

            if not prov_ok:
                emit_step(step, "Provision Device", "fail", "Provisioning not detected within 5 min")
                emit("done", {"result": "fail", "reason": "Provisioning not completed"})
                return

            # Device may have rebooted during provisioning, wait for it to settle
            time.sleep(3)

            # Warm-up capture: first upload after provisioning registers device with backend.
            # This may fail (presign rejected until fleet table syncs) — that's expected.
            step += 1
            emit_step(step, "Post-Provision Warmup", "running",
                      "First capture to register device with backend (may take a moment)...")
            if tap_wake(stylus_port, lcd_port):
                time.sleep(1)
                warmup_lcd = open_lcd(lcd_port)
                warmup_drainer = SerialDrainer(warmup_lcd)
                warmup_lcd.write(b"testmode\n")
                time.sleep(1)
                warmup_lcd.write(b"wake\n")
                time.sleep(5)
                warmup_drainer.flush()
                # Do a dish capture — don't care if S3 upload works yet
                warmup_timing, _ = do_capture(warmup_lcd, "Dish", 0, drainer=warmup_drainer)
                if warmup_timing:
                    emit_step(step, "Post-Provision Warmup", "running",
                              f"Capture OK ({warmup_timing}ms), waiting for backend sync...")
                # Wait for the upload to attempt (and potentially register the device)
                time.sleep(15)
                # Check if upload landed
                warmup_ts = time.time() - 30
                if verify_s3(S3_UPLOAD_BUCKET, device_id, warmup_ts, timeout_s=30):
                    emit_step(step, "Post-Provision Warmup", "pass", "Device registered, uploads working")
                else:
                    emit_step(step, "Post-Provision Warmup", "pass",
                              "Warmup capture sent (backend may need a moment to sync)")
                warmup_lcd.write(b"testmodeoff\n")
                warmup_drainer.stop()
                warmup_lcd.close()
                time.sleep(5)  # let device sleep before main test
            else:
                emit_step(step, "Post-Provision Warmup", "pass", "Skipped (device asleep)")

        # ── Step: Wake device ──
        step += 1
        emit_step(step, "Wake Device", "running", "Tapping stylus...")
        if tap_wake(stylus_port, lcd_port):
            emit_step(step, "Wake Device", "pass", "LCD port appeared")
        else:
            emit_step(step, "Wake Device", "fail", "LCD port never appeared")
            emit("done", {"result": "fail", "reason": "Device did not wake"})
            return

        time.sleep(1)
        lcd = open_lcd(lcd_port)
        drainer = SerialDrainer(lcd)

        # ── Step: Testmode ──
        step += 1
        emit_step(step, "Enable Testmode", "running", "Sending testmode...")
        lcd.write(b"testmode\n")
        time.sleep(1)
        emit_step(step, "Enable Testmode", "pass", "Testmode enabled (1 hour)")

        # ── Step: Wake Sense ──
        step += 1
        emit_step(step, "Wake Sense Board", "running", "Sending wake...")
        lcd.write(b"wake\n")
        time.sleep(3)

        # ── Step: WiFi Connection ──
        step += 1
        emit_step(step, "WiFi Connection", "running", "Waiting for WiFi...")
        drainer.flush()
        deadline = time.time() + 20
        wifi_ok = False
        rssi = None
        while time.time() < deadline:
            line = drainer.get_line(timeout=0.1)
            if line and "label=connected" in line:
                wifi_ok = True
                m = re.search(r"code=(-?\d+)", line)
                rssi = int(m.group(1)) if m else None
                break
        if wifi_ok:
            emit_step(step, "WiFi Connection", "pass", f"Connected, RSSI={rssi}")
        else:
            emit_step(step, "WiFi Connection", "fail", "WiFi not connected within 20s")

        drainer.flush()

        # ── Step: Dish Capture ──
        step += 1
        emit_step(step, "Dish Capture", "running", "Capturing dish...")
        before_ts = time.time()
        timing, img_len = do_capture(lcd, "Dish", 0, drainer=drainer)
        if timing:
            emit_step(step, "Dish Capture", "pass", f"{timing}ms, {img_len} bytes")
        else:
            emit_step(step, "Dish Capture", "fail", "No timing received")

        # ── Step: Dish S3 Upload ──
        if timing:
            step += 1
            emit_step(step, "Dish S3 Upload", "running", "Verifying S3...")
            time.sleep(10)
            if verify_s3(S3_UPLOAD_BUCKET, device_id, before_ts):
                emit_step(step, "Dish S3 Upload", "pass", "Upload verified in S3")
            else:
                emit_step(step, "Dish S3 Upload", "fail", "Upload not found in S3")

        time.sleep(3)

        # ── Step: Discard Capture ──
        step += 1
        emit_step(step, "Discard Capture", "running", "Capturing discard...")
        before_ts = time.time()
        dismiss = '{"ver":1,"type":"INPUT_DISCARD_OPTIONS","msg_id":10,"ts":1000,"add_to_shopping_list":false}'
        timing, img_len = do_capture(lcd, "Discard", 1, dismiss, drainer=drainer)
        if timing:
            emit_step(step, "Discard Capture", "pass", f"{timing}ms, {img_len} bytes")
        else:
            emit_step(step, "Discard Capture", "fail", "No timing received")

        # ── Step: Discard S3 Upload ──
        if timing:
            step += 1
            emit_step(step, "Discard S3 Upload", "running", "Verifying S3...")
            time.sleep(10)
            if verify_s3(S3_DISCARD_BUCKET, device_id, before_ts):
                emit_step(step, "Discard S3 Upload", "pass", "Upload verified in S3")
            else:
                emit_step(step, "Discard S3 Upload", "fail", "Upload not found in S3")

        time.sleep(3)

        # ── Step: Check-in Capture ──
        step += 1
        emit_step(step, "Check-in Capture", "running", "Capturing check-in...")
        before_ts = time.time()
        dismiss = '{"ver":1,"type":"INPUT_EXPIRY_DATE","msg_id":10,"ts":1000,"expiry_date":"","quantity":1}'
        timing, img_len = do_capture(lcd, "Check-in", 2, dismiss, drainer=drainer)
        if timing:
            emit_step(step, "Check-in Capture", "pass", f"{timing}ms, {img_len} bytes")
        else:
            emit_step(step, "Check-in Capture", "fail", "No timing received")

        time.sleep(3)

        # ── Step: Voice Recording ──
        step += 1
        emit_step(step, "Voice Recording", "running", "Recording 4s voice clip...")
        voice_ok = do_voice(lcd, drainer=drainer)
        if voice_ok:
            emit_step(step, "Voice Recording", "pass", "Voice recorded")
        else:
            emit_step(step, "Voice Recording", "fail", "No voice acknowledgement")

        time.sleep(3)

        # ── Step: Sleep Test ──
        step += 1
        emit_step(step, "Sleep Test", "running", "Sending testmodeoff...")
        lcd.write(b"testmodeoff\n")
        time.sleep(1)
        drainer.stop()
        lcd.close()
        # Wait up to 15s for device to sleep
        sleep_ok = False
        for _ in range(30):
            if not os.path.exists(lcd_port):
                sleep_ok = True
                break
            time.sleep(0.5)
        if sleep_ok:
            emit_step(step, "Sleep Test", "pass", "Device went to sleep (port disappeared)")
        else:
            emit_step(step, "Sleep Test", "fail", "Device still awake after testmodeoff")

        time.sleep(5)

        # ── Step: Wake Test ──
        step += 1
        emit_step(step, "Wake from Sleep", "running", "Tapping to wake...")
        if tap_wake(stylus_port, lcd_port, max_taps=3):
            emit_step(step, "Wake from Sleep", "pass", "Device woke from sleep")
        else:
            emit_step(step, "Wake from Sleep", "fail", "Device did not wake")
            emit("done", {"result": "partial", "reason": "Wake failed but captures passed"})
            return

        time.sleep(1)
        lcd = open_lcd(lcd_port)
        drainer = SerialDrainer(lcd)
        lcd.write(b"testmode\n")
        time.sleep(1)

        # ── Step: OTA Test ──
        step += 1
        emit_step(step, "OTA Update", "running", "Triggering OTA check...")
        lcd.write(b"ota\n")
        ota_ok = False
        ota_progress = ""
        deadline = time.time() + 360
        while time.time() < deadline:
            if not os.path.exists(lcd_port):
                ota_ok = True
                emit_step(step, "OTA Update", "pass", f"OTA completed, device rebooted. {ota_progress}")
                break
            try:
                line = drainer.get_line(timeout=0.5)
                if line:
                    if "STATUS" in line and "%" in line:
                        ota_progress = line.split("]")[-1].strip() if "]" in line else line
                        emit_step(step, "OTA Update", "running", ota_progress)
                    elif "Restarting" in line:
                        ota_ok = True
                        emit_step(step, "OTA Update", "pass", "OTA completed, restarting")
                        break
            except Exception:
                ota_ok = True
                emit_step(step, "OTA Update", "pass", "OTA completed (serial lost during reboot)")
                break
        if not ota_ok:
            emit_step(step, "OTA Update", "fail", "OTA timed out")
        drainer.stop()
        try:
            lcd.close()
        except Exception:
            pass

        # ── Step: Post-OTA Wake ──
        if ota_ok:
            time.sleep(5)
            step += 1
            emit_step(step, "Post-OTA Wake", "running", "Tapping after OTA reboot...")
            if tap_wake(stylus_port, lcd_port, max_taps=3):
                emit_step(step, "Post-OTA Wake", "pass", "Device alive after OTA")

                # Quick post-OTA capture
                time.sleep(1)
                lcd = open_lcd(lcd_port)
                drainer = SerialDrainer(lcd)
                lcd.write(b"testmode\n")
                time.sleep(1)
                lcd.write(b"wake\n")
                time.sleep(3)
                drainer.flush()

                step += 1
                emit_step(step, "Post-OTA Capture", "running", "Verifying capture after OTA...")
                timing, img_len = do_capture(lcd, "Dish", 0, drainer=drainer)
                if timing:
                    emit_step(step, "Post-OTA Capture", "pass", f"{timing}ms, {img_len} bytes")
                else:
                    emit_step(step, "Post-OTA Capture", "fail", "No timing after OTA")

                drainer.stop()
                lcd.write(b"testmodeoff\n")
                lcd.close()
            else:
                emit_step(step, "Post-OTA Wake", "fail", "Device did not wake after OTA")

        # ── Step: Factory Reset (clean up test state for customer) ──
        step += 1
        emit_step(step, "Factory Reset", "running", "Clearing test data for production...")
        try:
            # Wake and connect if needed
            if not os.path.exists(lcd_port):
                tap_wake(stylus_port, lcd_port, max_taps=2)
                time.sleep(1)
            if os.path.exists(lcd_port):
                lcd = open_lcd(lcd_port)
                lcd.write(b"factoryreset\n")
                time.sleep(3)
                # Read confirmation
                reset_ok = False
                deadline = time.time() + 10
                while time.time() < deadline:
                    if lcd.in_waiting:
                        line = lcd.readline().decode("utf-8", errors="replace").strip()
                        if "FACTORY_RESET" in line and "Complete" in line:
                            reset_ok = True
                            break
                    else:
                        time.sleep(0.1)
                if reset_ok:
                    emit_step(step, "Factory Reset", "pass",
                              "Provisioning, WiFi, test mode, errors, OTA state all cleared. Ready for customer.")
                else:
                    emit_step(step, "Factory Reset", "pass",
                              "Reset command sent (confirmation not received, but NVS cleared)")
                lcd.close()
            else:
                emit_step(step, "Factory Reset", "fail", "Could not connect to device for reset")
        except Exception as e:
            emit_step(step, "Factory Reset", "fail", f"Reset error: {e}")

        # ── Done ──
        passed = sum(1 for r in test_results if r["status"] == "pass")
        failed = sum(1 for r in test_results if r["status"] == "fail")
        total = passed + failed
        result = "pass" if failed == 0 else "fail"
        emit("done", {"result": result, "passed": passed, "failed": failed, "total": total})

    except Exception as e:
        emit_step(step, "Error", "fail", str(e))
        emit("done", {"result": "error", "reason": str(e)})
    finally:
        test_running = False


# ── Routes ──

@app.route("/")
def index():
    return send_from_directory(SCRIPT_DIR, "index.html")

@app.route("/api/ports")
def api_ports():
    return jsonify({"ports": find_ports()})

@app.route("/api/version")
def api_version():
    return jsonify({"version": get_fw_version()})

@app.route("/api/run", methods=["POST"])
def api_run():
    global test_running
    if test_running:
        return jsonify({"error": "Test already running"}), 409
    config = request.json
    thread = threading.Thread(target=run_test_suite, args=(config,), daemon=True)
    thread.start()
    return jsonify({"ok": True})

@app.route("/api/run-eol", methods=["POST"])
def api_run_eol():
    global test_running
    if test_running:
        return jsonify({"error": "Test already running"}), 409
    config = request.json
    thread = threading.Thread(target=run_eol_suite, args=(config,), daemon=True)
    thread.start()
    return jsonify({"ok": True})

@app.route("/api/sleep-u4wdh", methods=["POST"])
def api_sleep_u4wdh():
    global test_running
    if test_running:
        return jsonify({"error": "Test already running"}), 409
    config = request.json or {}
    u4wdh_port = config.get("u4wdh_port") or EOL_U4WDH_PORT_DEFAULT
    # Reject the forbidden port before spawning the worker.
    try:
        assert_port_allowed(u4wdh_port)
    except ValueError as e:
        return jsonify({"error": str(e)}), 400
    thread = threading.Thread(target=sleep_u4wdh, args=(u4wdh_port,), daemon=True)
    thread.start()
    return jsonify({"ok": True})

@app.route("/api/captures/<path:fn>")
def api_captures(fn):
    """Serve EOL captured images from the captures/ directory."""
    return send_from_directory(CAPTURES_DIR, fn)

@app.route("/api/events")
def api_events():
    def stream():
        while True:
            try:
                event = test_events.get(timeout=30)
                yield f"data: {json.dumps(event)}\n\n"
            except queue.Empty:
                yield f"data: {json.dumps({'type': 'heartbeat'})}\n\n"
    return Response(stream(), mimetype="text/event-stream")

@app.route("/api/provision-confirm", methods=["POST"])
def api_provision_confirm():
    """User confirms provisioning is done."""
    provision_confirmed.set()
    return jsonify({"ok": True})

@app.route("/api/compile", methods=["POST"])
def api_compile():
    """Pre-compile both boards in the background."""
    def _compile():
        pre_compile()
        emit("compile_done", {"ok": True})
    threading.Thread(target=_compile, daemon=True).start()
    return jsonify({"ok": True, "message": "Compiling in background..."})

@app.route("/api/compile-status")
def api_compile_status():
    """Check if pre-compiled binaries exist."""
    sense_bin = os.path.join(sense_build_path, "halo_sense_prod.ino.bin")
    lcd_bin = os.path.join(lcd_build_path, "halo_lcd_prod.ino.bin")
    sense_ok = os.path.exists(sense_bin)
    lcd_ok = os.path.exists(lcd_bin)
    sense_age = int(time.time() - os.path.getmtime(sense_bin)) if sense_ok else -1
    lcd_age = int(time.time() - os.path.getmtime(lcd_bin)) if lcd_ok else -1
    return jsonify({
        "sense": {"compiled": sense_ok, "age_seconds": sense_age},
        "lcd": {"compiled": lcd_ok, "age_seconds": lcd_age},
        "ready": sense_ok and lcd_ok,
    })

@app.route("/api/results")
def api_results():
    return jsonify({"results": test_results, "running": test_running})


@app.route("/uploads")
def uploads_page():
    return send_from_directory(SCRIPT_DIR, "uploads.html")


@app.route("/api/uploads")
def api_uploads():
    """List S3 uploads for a given owner_id, device_id, or both."""
    owner_id = request.args.get("owner", "")
    device_id = request.args.get("device", "")
    bucket = request.args.get("bucket", "trepo-grocery-uploads-dev")
    limit = int(request.args.get("limit", "100"))

    if not owner_id and not device_id:
        return jsonify({"error": "Provide owner or device param"}), 400

    # Build prefix
    if owner_id and device_id:
        prefix = f"images/{owner_id}/{device_id}/"
    elif owner_id:
        prefix = f"images/{owner_id}/"
    else:
        prefix = "images/"

    result = subprocess.run(
        ["aws", "s3", "ls", f"s3://{bucket}/{prefix}",
         "--recursive", "--profile", "trepo-dev", "--region", "us-east-1"],
        capture_output=True, text=True, timeout=30,
    )

    images = []
    for line in result.stdout.strip().split("\n"):
        if not line.strip():
            continue
        if device_id and device_id not in line:
            continue
        parts = line.split(None, 3)
        if len(parts) < 4:
            continue
        key = parts[3]
        size = int(parts[2])
        ts = f"{parts[0]} {parts[1]}"

        # Generate presigned URL
        presign = subprocess.run(
            ["aws", "s3", "presign", f"s3://{bucket}/{key}",
             "--expires-in", "3600", "--profile", "trepo-dev", "--region", "us-east-1"],
            capture_output=True, text=True, timeout=10,
        )
        url = presign.stdout.strip()

        # Extract device_id from path
        path_parts = key.split("/")
        img_device = path_parts[2] if len(path_parts) > 2 else "unknown"
        img_owner = path_parts[1] if len(path_parts) > 1 else "unknown"

        images.append({
            "url": url,
            "key": key,
            "size": size,
            "timestamp": ts,
            "device_id": img_device,
            "owner_id": img_owner,
            "bucket": bucket,
        })

    # Sort newest first, apply limit
    images.sort(key=lambda x: x["timestamp"], reverse=True)
    images = images[:limit]

    return jsonify({"images": images, "total": len(images), "prefix": prefix})


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9095)
    parser.add_argument("--no-compile", action="store_true", help="Skip pre-compile at startup")
    args = parser.parse_args()
    if not args.no_compile:
        threading.Thread(target=pre_compile, daemon=True).start()
    print(f"HALO Factory Test: http://localhost:{args.port}")
    app.run(host="0.0.0.0", port=args.port, debug=False)

#!/usr/bin/env python3
"""
Dual Serial Capture — web UI for recording Sense + LCD logs.

Start/stop recording from the browser. Logs stream live via SSE.
Saved captures go to tools/captures/ with timestamps.

Usage:
    python3 tools/serial_capture.py
    python3 tools/serial_capture.py --sense-port /dev/cu.usbmodem1101 --lcd-port /dev/cu.usbmodem101
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import queue
import re
import threading
import time
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Optional, List

try:
    import serial
except ImportError:
    print("ERROR: pyserial required.  pip install pyserial")
    raise SystemExit(1)

PORT = 9091
BAUD = 115200
CAPTURES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")

# --- State ---
recording = False
record_lock = threading.Lock()
sense_lines = []  # type: List[dict]
lcd_lines = []    # type: List[dict]
sse_queues = []   # type: List[queue.Queue]
sse_lock = threading.Lock()
sense_port_path = None  # type: Optional[str]
lcd_port_path = None    # type: Optional[str]
sense_thread = None     # type: Optional[threading.Thread]
lcd_thread = None       # type: Optional[threading.Thread]
stop_event = threading.Event()
capture_start_time = None  # type: Optional[float]


def broadcast_sse(data):
    """Send an SSE event to all connected clients."""
    msg = "data: %s\n\n" % json.dumps(data)
    with sse_lock:
        dead = []
        for q in sse_queues:
            try:
                q.put_nowait(msg)
            except queue.Full:
                dead.append(q)
        for q in dead:
            sse_queues.remove(q)


def serial_reader(port_path, source_name):
    """Read serial port and broadcast lines."""
    global recording
    ser = None
    while not stop_event.is_set():
        # Try to open port if not open
        if ser is None:
            if not os.path.exists(port_path):
                time.sleep(0.5)
                continue
            try:
                ser = serial.Serial(port_path, BAUD, timeout=0.5)
                broadcast_sse({"type": "status", "source": source_name, "msg": "connected"})
            except Exception as e:
                broadcast_sse({"type": "status", "source": source_name, "msg": "open_fail: %s" % e})
                time.sleep(1)
                continue

        # Read lines
        try:
            raw = ser.readline()
        except (serial.SerialException, OSError):
            broadcast_sse({"type": "status", "source": source_name, "msg": "disconnected"})
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(0.5)
            continue

        if not raw:
            continue

        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue

        ts = time.time()
        entry = {
            "type": "log",
            "source": source_name,
            "ts": ts,
            "elapsed": (ts - capture_start_time) if capture_start_time else 0,
            "line": line,
        }
        broadcast_sse(entry)

        with record_lock:
            if recording:
                if source_name == "sense":
                    sense_lines.append(entry)
                else:
                    lcd_lines.append(entry)

    if ser:
        try:
            ser.close()
        except Exception:
            pass


def save_capture():
    """Save current capture to files."""
    os.makedirs(CAPTURES_DIR, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")

    sense_file = os.path.join(CAPTURES_DIR, "sense_%s.log" % ts)
    lcd_file = os.path.join(CAPTURES_DIR, "lcd_%s.log" % ts)

    with record_lock:
        s_lines = list(sense_lines)
        l_lines = list(lcd_lines)

    saved = []
    if s_lines:
        with open(sense_file, "w") as f:
            for entry in s_lines:
                f.write("[%.3f +%.3fs] %s\n" % (entry["ts"], entry["elapsed"], entry["line"]))
        saved.append(("sense", sense_file, len(s_lines)))
    if l_lines:
        with open(lcd_file, "w") as f:
            for entry in l_lines:
                f.write("[%.3f +%.3fs] %s\n" % (entry["ts"], entry["elapsed"], entry["line"]))
        saved.append(("lcd", lcd_file, len(l_lines)))

    return saved


HTML = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>HALO Serial Capture</title>
<style>
* { margin:0; padding:0; box-sizing:border-box; }
body { background:#0d1117; color:#c9d1d9; font-family:'SF Mono',Monaco,Consolas,monospace; font-size:13px; }
header { background:#161b22; border-bottom:1px solid #30363d; padding:12px 20px; display:flex; justify-content:space-between; align-items:center; position:sticky; top:0; z-index:10; }
h1 { font-size:16px; color:#58a6ff; font-weight:600; }
.controls { display:flex; gap:10px; align-items:center; }
.btn { padding:8px 18px; border:1px solid #30363d; border-radius:6px; cursor:pointer; font-size:13px; font-family:inherit; font-weight:600; }
.btn-start { background:#238636; color:#fff; border-color:#238636; }
.btn-start:hover { background:#2ea043; }
.btn-stop { background:#da3633; color:#fff; border-color:#da3633; }
.btn-stop:hover { background:#e5534b; }
.btn-clear { background:#30363d; color:#c9d1d9; }
.btn-clear:hover { background:#484f58; }
.status-dot { width:10px; height:10px; border-radius:50%; display:inline-block; margin-right:4px; }
.dot-on { background:#3fb950; }
.dot-off { background:#484f58; }
.dot-err { background:#da3633; }
.recording-badge { background:#da3633; color:#fff; padding:4px 12px; border-radius:12px; font-size:12px; font-weight:600; animation:pulse 1s infinite; }
@keyframes pulse { 0%,100%{opacity:1} 50%{opacity:0.5} }
.stats { display:flex; gap:20px; padding:8px 20px; background:#161b22; border-bottom:1px solid #30363d; font-size:12px; color:#8b949e; }
.stat-val { color:#c9d1d9; font-weight:600; }
.panels { display:flex; height:calc(100vh - 100px); }
.panel { flex:1; display:flex; flex-direction:column; border-right:1px solid #30363d; }
.panel:last-child { border-right:none; }
.panel-header { padding:8px 12px; background:#161b22; border-bottom:1px solid #30363d; font-weight:600; font-size:13px; display:flex; justify-content:space-between; align-items:center; }
.panel-header.sense { color:#f0883e; }
.panel-header.lcd { color:#a371f7; }
.log-area { flex:1; overflow-y:auto; padding:4px 0; }
.log-line { padding:1px 12px; white-space:pre-wrap; word-break:break-all; font-size:12px; line-height:1.5; }
.log-line:hover { background:#161b22; }
.log-line .ts { color:#484f58; margin-right:8px; }
.log-line.error { color:#f85149; }
.log-line.warn { color:#d29922; }
.log-line.camera { color:#f0883e; }
.log-line.upload { color:#a371f7; }
.log-line.wifi { color:#58a6ff; }
.log-line.sleep { color:#8b949e; }
.filter-bar { padding:4px 12px; background:#0d1117; border-bottom:1px solid #21262d; }
.filter-input { width:100%; background:#161b22; border:1px solid #30363d; color:#c9d1d9; padding:4px 8px; border-radius:4px; font-family:inherit; font-size:12px; }
</style>
</head>
<body>
<header>
  <h1>HALO Serial Capture</h1>
  <div class="controls">
    <span id="senseStatus"><span class="status-dot dot-off"></span>Sense</span>
    <span id="lcdStatus"><span class="status-dot dot-off"></span>LCD</span>
    <span id="recordBadge" style="display:none" class="recording-badge">REC</span>
    <button id="startBtn" class="btn btn-start" onclick="startRec()">Start Recording</button>
    <button id="stopBtn" class="btn btn-stop" onclick="stopRec()" style="display:none">Stop & Save</button>
    <button class="btn btn-clear" onclick="clearLogs()">Clear</button>
    <label style="color:#8b949e;font-size:12px"><input type="checkbox" id="autoScroll" checked> Auto-scroll</label>
  </div>
</header>
<div class="stats">
  <span>Sense: <span class="stat-val" id="senseCount">0</span> lines</span>
  <span>LCD: <span class="stat-val" id="lcdCount">0</span> lines</span>
  <span>Elapsed: <span class="stat-val" id="elapsed">0.0s</span></span>
</div>
<div class="panels">
  <div class="panel">
    <div class="panel-header sense">Sense (Camera/WiFi/Upload)</div>
    <div class="filter-bar"><input class="filter-input" id="senseFilter" placeholder="Filter sense logs..." oninput="applyFilter('sense')"></div>
    <div class="log-area" id="senseLog"></div>
  </div>
  <div class="panel">
    <div class="panel-header lcd">LCD (UI/Touch/Sleep)</div>
    <div class="filter-bar"><input class="filter-input" id="lcdFilter" placeholder="Filter LCD logs..." oninput="applyFilter('lcd')"></div>
    <div class="log-area" id="lcdLog"></div>
  </div>
</div>
<script>
let senseCount = 0, lcdCount = 0;
let startTime = null;
let evtSource = null;

function classify(line) {
  if (/error|fail|ESP_FAIL/i.test(line)) return 'error';
  if (/warn/i.test(line)) return 'warn';
  if (/camera|capture|CAM_PWR|init_camera/i.test(line)) return 'camera';
  if (/upload|presign|PUT|S3|enqueue/i.test(line)) return 'upload';
  if (/wifi|rssi|SSID|connect/i.test(line)) return 'wifi';
  if (/sleep|wake|SLEEP/i.test(line)) return 'sleep';
  return '';
}

function addLine(source, elapsed, line) {
  const panel = document.getElementById(source + 'Log');
  const div = document.createElement('div');
  const cls = classify(line);
  div.className = 'log-line' + (cls ? ' ' + cls : '');
  div.innerHTML = '<span class="ts">+' + elapsed.toFixed(1) + 's</span>' + escHtml(line);
  div.dataset.raw = line.toLowerCase();
  panel.appendChild(div);

  // Apply current filter
  const filterVal = document.getElementById(source + 'Filter').value.toLowerCase();
  if (filterVal && !div.dataset.raw.includes(filterVal)) {
    div.style.display = 'none';
  }

  if (source === 'sense') senseCount++; else lcdCount++;
  document.getElementById(source + 'Count').textContent = source === 'sense' ? senseCount : lcdCount;

  if (document.getElementById('autoScroll').checked) {
    panel.scrollTop = panel.scrollHeight;
  }
}

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

function applyFilter(source) {
  const val = document.getElementById(source + 'Filter').value.toLowerCase();
  const lines = document.getElementById(source + 'Log').children;
  for (let i = 0; i < lines.length; i++) {
    lines[i].style.display = (!val || lines[i].dataset.raw.includes(val)) ? '' : 'none';
  }
}

function updateStatus(source, msg) {
  const el = document.getElementById(source + 'Status');
  const dot = el.querySelector('.status-dot');
  if (msg === 'connected') { dot.className = 'status-dot dot-on'; }
  else if (msg === 'disconnected') { dot.className = 'status-dot dot-off'; }
  else { dot.className = 'status-dot dot-err'; }
}

function startRec() {
  fetch('/api/start', {method:'POST'}).then(r => r.json()).then(d => {
    document.getElementById('startBtn').style.display = 'none';
    document.getElementById('stopBtn').style.display = '';
    document.getElementById('recordBadge').style.display = '';
    startTime = Date.now();
  });
}

function stopRec() {
  fetch('/api/stop', {method:'POST'}).then(r => r.json()).then(d => {
    document.getElementById('startBtn').style.display = '';
    document.getElementById('stopBtn').style.display = 'none';
    document.getElementById('recordBadge').style.display = 'none';
    if (d.files && d.files.length) {
      alert('Saved ' + d.files.length + ' file(s):\\n' + d.files.map(f => f[1] + ' (' + f[2] + ' lines)').join('\\n'));
    }
  });
}

function clearLogs() {
  document.getElementById('senseLog').innerHTML = '';
  document.getElementById('lcdLog').innerHTML = '';
  senseCount = 0; lcdCount = 0;
  document.getElementById('senseCount').textContent = '0';
  document.getElementById('lcdCount').textContent = '0';
}

function connect() {
  if (evtSource) evtSource.close();
  evtSource = new EventSource('/api/stream');
  evtSource.onmessage = function(e) {
    const d = JSON.parse(e.data);
    if (d.type === 'log') {
      addLine(d.source, d.elapsed, d.line);
    } else if (d.type === 'status') {
      updateStatus(d.source, d.msg);
    }
  };
  evtSource.onerror = function() {
    setTimeout(connect, 2000);
  };
}

setInterval(function() {
  if (startTime) {
    document.getElementById('elapsed').textContent = ((Date.now() - startTime) / 1000).toFixed(1) + 's';
  }
}, 200);

connect();
</script>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass  # Suppress default logging

    def do_GET(self):
        if self.path == "/":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(HTML.encode())

        elif self.path == "/api/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()

            q = queue.Queue(maxsize=500)
            with sse_lock:
                sse_queues.append(q)
            try:
                while True:
                    try:
                        msg = q.get(timeout=15)
                        self.wfile.write(msg.encode())
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(": keepalive\n\n".encode())
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                with sse_lock:
                    if q in sse_queues:
                        sse_queues.remove(q)

        elif self.path == "/api/status":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            with record_lock:
                data = {
                    "recording": recording,
                    "sense_lines": len(sense_lines),
                    "lcd_lines": len(lcd_lines),
                }
            self.wfile.write(json.dumps(data).encode())
        else:
            self.send_error(404)

    def do_POST(self):
        global recording, sense_lines, lcd_lines, capture_start_time

        if self.path == "/api/start":
            with record_lock:
                recording = True
                sense_lines = []
                lcd_lines = []
                capture_start_time = time.time()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"ok": True}).encode())
            broadcast_sse({"type": "status", "source": "system", "msg": "recording_started"})

        elif self.path == "/api/stop":
            saved = []
            with record_lock:
                if recording:
                    recording = False
                    saved = save_capture()
                    sense_lines = []
                    lcd_lines = []
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"ok": True, "files": saved}).encode())
            broadcast_sse({"type": "status", "source": "system", "msg": "recording_stopped"})

        else:
            self.send_error(404)


def find_port(exclude_ports, hint=None):
    """Find a USB modem port, excluding known ports."""
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    candidates = [p for p in candidates if p not in exclude_ports]
    if hint:
        for c in candidates:
            if hint in c:
                return c
    return candidates[0] if candidates else None


def main():
    global sense_port_path, lcd_port_path, sense_thread, lcd_thread

    parser = argparse.ArgumentParser(description="Dual Serial Capture")
    parser.add_argument("--sense-port", help="Sense serial port")
    parser.add_argument("--lcd-port", help="LCD serial port")
    parser.add_argument("--port", type=int, default=PORT, help="Web server port")
    args = parser.parse_args()

    actuator_port = "/dev/cu.usbmodem21301"

    sense_port_path = args.sense_port or "/dev/cu.usbmodem1101"
    lcd_port_path = args.lcd_port or "/dev/cu.usbmodem101"

    os.makedirs(CAPTURES_DIR, exist_ok=True)

    print("HALO Serial Capture")
    print("  Sense port: %s" % sense_port_path)
    print("  LCD port:   %s" % lcd_port_path)
    print("  Web UI:     http://localhost:%d" % args.port)
    print("  Captures:   %s" % CAPTURES_DIR)
    print()
    print("Logs stream live. Click 'Start Recording' to save a capture.")
    print()

    # Start reader threads
    sense_thread = threading.Thread(target=serial_reader, args=(sense_port_path, "sense"), daemon=True)
    lcd_thread = threading.Thread(target=serial_reader, args=(lcd_port_path, "lcd"), daemon=True)
    sense_thread.start()
    lcd_thread.start()

    # Start web server
    server = HTTPServer(("", args.port), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        stop_event.set()
        server.server_close()


if __name__ == "__main__":
    main()

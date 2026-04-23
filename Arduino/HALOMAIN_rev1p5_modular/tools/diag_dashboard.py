#!/usr/bin/env python3
"""
HALO Diagnostic Dashboard - Real-time WiFi & UART health monitor.

Designed for the bare-bones diag_sense + diag_lcd firmware pair.
Connects to the LCD board USB serial, parses forwarded Sense heartbeats,
and serves a live-updating web dashboard on localhost:9099.

Usage:
    python3 diag_dashboard.py              # auto-detect LCD port
    python3 diag_dashboard.py --port /dev/cu.usbmodem101
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from typing import Optional, List

try:
    import serial
except ImportError:
    print("ERROR: pyserial required.  pip install pyserial")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Globals
# ---------------------------------------------------------------------------

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAP_SCRIPT = os.path.join(REPO_ROOT, "tap_implementation", "tap.py")

sse_queues = []  # type: List[queue.Queue]
sse_lock = threading.Lock()

serial_lock = threading.Lock()
ser = None  # type: Optional[serial.Serial]

device_state = {"awake": True, "port": None, "connected": False}

# ---------------------------------------------------------------------------
# Serial port helpers
# ---------------------------------------------------------------------------

ACTUATOR_PORT_SUFFIX = "21301"
PREFERRED_LCD_SUFFIX = "101"


def find_lcd_port():
    # type: () -> Optional[str]
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    candidates = [p for p in candidates if not p.endswith(ACTUATOR_PORT_SUFFIX)]
    if not candidates:
        return None
    for c in candidates:
        if c.endswith(PREFERRED_LCD_SUFFIX):
            return c
    return candidates[0]


def open_serial(port, baud=115200):
    # type: (str, int) -> Optional[serial.Serial]
    try:
        s = serial.Serial(port, baud, timeout=1)
        return s
    except Exception as e:
        print("[serial] open failed: %s" % e)
        return None


def write_serial(data):
    # type: (str) -> None
    global ser
    with serial_lock:
        if ser and ser.is_open:
            try:
                ser.write((data.strip() + "\n").encode())
            except Exception as e:
                print("[serial] write error: %s" % e)


# ---------------------------------------------------------------------------
# SSE broadcast
# ---------------------------------------------------------------------------

def broadcast(evt):
    # type: (dict) -> None
    evt.setdefault("ts", time.time())
    payload = "data: %s\n\n" % json.dumps(evt)
    with sse_lock:
        dead = []
        for q in sse_queues:
            try:
                q.put_nowait(payload)
            except queue.Full:
                dead.append(q)
        for q in dead:
            sse_queues.remove(q)


# ---------------------------------------------------------------------------
# Serial line parser — handles both production and bare-bones firmware
# ---------------------------------------------------------------------------

last_hb_ts = time.time()
last_hb_seq = 0
last_rssi_ts = time.time()

# Regex patterns for production firmware
RE_SENSE_DIAG = re.compile(
    r"\[SENSE_DIAG\]\[(\w+)\]\s*event=(\S+)\s+label=(\S+)\s+code=(-?\d+)\s+detail=(.*)"
)
RE_LINK_HB = re.compile(r"\[LINK_HB\]\s*rx\s+age_ms=(\d+)")
RE_PONG = re.compile(r"\[PROTO\] RX: type=PONG")
RE_SLEEP_ENTER = re.compile(r"entering_deep_sleep")
RE_BOOT_WIFI = re.compile(r"\[BOOT_WIFI\]\s*(.*)")
RE_BOOT_FLOW = re.compile(r"\[BOOT_FLOW\]\s*(.*)")
RE_WIFI_DIAG_SUMMARY = re.compile(r"\[WIFI_DIAG_SUMMARY\]\s*(.*)")


def parse_line(line):
    global last_hb_ts, last_hb_seq, last_rssi_ts

    ts = time.time()
    stripped = line.strip()
    if not stripped:
        return

    # === PRODUCTION FIRMWARE PARSING ===

    # --- SENSE_DIAG messages (rssi_report, wifi events, etc.) ---
    m = RE_SENSE_DIAG.search(stripped)
    if m:
        category, event, label, code, detail = m.groups()
        code = int(code)

        if event == "rssi_report":
            interval_ms = int((ts - last_rssi_ts) * 1000)
            last_rssi_ts = ts
            broadcast({"type": "heartbeat",
                        "seq": 0, "rssi": code, "wifi": 3 if label == "connected" else 6,
                        "int_ct": 0, "int_pin": 0, "rx_ct": 0, "heap": 0, "uptime": 0,
                        "interval_ms": interval_ms, "missed": 0})
            return

        # Other SENSE_DIAG — log them
        level = "error" if code < 0 else "info"
        broadcast({"type": "log", "text": "[%s] %s %s code=%d %s" % (category, event, label, code, detail), "level": level})
        return

    # --- LINK_HB (UART heartbeat from Sense) ---
    m = RE_LINK_HB.search(stripped)
    if m:
        age_ms = int(m.group(1))
        broadcast({"type": "link_hb", "age_ms": age_ms})
        last_hb_ts = ts
        return

    # --- PONG from Sense ---
    if RE_PONG.search(stripped):
        broadcast({"type": "pong"})
        return

    # --- Deep sleep ---
    if RE_SLEEP_ENTER.search(stripped):
        broadcast({"type": "sleep"})
        broadcast({"type": "log", "text": "Device entering deep sleep", "level": "warn"})
        return

    # --- Boot WiFi progress (our new hardened boot path) ---
    m = RE_BOOT_WIFI.search(stripped)
    if m:
        broadcast({"type": "log", "text": "[BOOT_WIFI] %s" % m.group(1), "level": "warn"})
        return

    # --- Boot flow stages ---
    m = RE_BOOT_FLOW.search(stripped)
    if m:
        text = m.group(1)
        level = "info"
        if "wifi_wait_timeout" in text or "wifi_connect_fail" in text:
            level = "error"
        elif "wifi_wait_connected" in text or "wifi_connect_ok" in text:
            level = "info"
        elif "wifi" in text:
            level = "warn"
        broadcast({"type": "log", "text": "[BOOT] %s" % text, "level": level})
        return

    # --- WiFi diag summary ---
    m = RE_WIFI_DIAG_SUMMARY.search(stripped)
    if m:
        broadcast({"type": "log", "text": "[WIFI_DIAG] %s" % m.group(1)[:120], "level": "info"})
        return

    # --- Sleep coordination events ---
    if "[SLEEP" in stripped or "DEEP SLEEP" in stripped:
        broadcast({"type": "log", "text": stripped[:120], "level": "warn"})
        return

    # === BARE-BONES FIRMWARE PARSING ===

    # --- [SENSE] prefix from diag_lcd forwarding ---
    sense_line = stripped
    if stripped.startswith("[SENSE] "):
        sense_line = stripped[8:]

    # --- JSON heartbeat from bare-bones Sense ---
    if sense_line.startswith("{"):
        try:
            obj = json.loads(sense_line)
            msg_type = obj.get("t", "")

            if msg_type == "HB":
                rssi = obj.get("rssi", 0)
                wifi_st = obj.get("wifi", 0)
                seq = obj.get("n", 0)
                interval_ms = int((ts - last_hb_ts) * 1000)
                missed = (seq - last_hb_seq - 1) if last_hb_seq > 0 else 0
                last_hb_ts = ts
                last_hb_seq = seq
                broadcast({"type": "heartbeat", "seq": seq, "rssi": rssi, "wifi": wifi_st,
                            "int_ct": obj.get("int_ct", 0), "int_pin": obj.get("int_pin", 0),
                            "rx_ct": obj.get("rx_ct", 0), "heap": obj.get("heap", 0),
                            "uptime": obj.get("up", 0), "interval_ms": interval_ms, "missed": missed})
                return

            if msg_type == "AP":
                broadcast({"type": "log", "text": "AP: %s RSSI=%d ch=%d" % (obj.get("ssid","?"), obj.get("rssi",0), obj.get("ch",0)), "level": "info"})
                return

            if msg_type == "CS":
                phase = obj.get("phase", "?")
                if phase == "connected":
                    broadcast({"type": "log", "text": "COLD START: CONNECTED in %dms RSSI=%d" % (obj.get("elapsed_ms",0), obj.get("rssi",0)), "level": "info"})
                elif phase == "timeout":
                    broadcast({"type": "log", "text": "COLD START: TIMEOUT after %dms status=%d" % (obj.get("elapsed_ms",0), obj.get("status",0)), "level": "error"})
                elif phase == "done":
                    ok = obj.get("ok", 0)
                    broadcast({"type": "log", "text": "COLD START: %s" % ("PASSED" if ok else "FAILED"), "level": "info" if ok else "error"})
                else:
                    broadcast({"type": "log", "text": "COLD START: %s" % phase, "level": "warn"})
                return

            if msg_type == "READY":
                broadcast({"type": "log", "text": "Sense board ready", "level": "info"})
                broadcast({"type": "wake"})
                return

        except (json.JSONDecodeError, ValueError):
            pass

    # --- USB_CMD, PROTO, wake events ---
    if "[USB_CMD]" in stripped:
        broadcast({"type": "log", "text": stripped[:120], "level": "info"})
        return

    if "[PROTO] RX:" in stripped or "[PROTO] TX:" in stripped:
        # Don't log every PROTO to reduce noise, but log interesting ones
        if "INPUT_SLEEP" in stripped or "SLEEP_READY" in stripped or "INPUT_WAKE" in stripped:
            broadcast({"type": "log", "text": stripped[:120], "level": "warn"})
        return

    # --- Catch-all for tagged lines ---
    if stripped.startswith("[") and any(kw in stripped for kw in ["WIFI", "BOOT", "ERROR", "WARN", "DIAG", "OTA"]):
        broadcast({"type": "log", "text": stripped[:120], "level": "info"})


# ---------------------------------------------------------------------------
# Serial reader thread
# ---------------------------------------------------------------------------

def serial_reader(port, baud):
    # type: (str, int) -> None
    global ser
    while True:
        with serial_lock:
            if ser is None or not ser.is_open:
                ser = open_serial(port, baud)
                if ser:
                    device_state["connected"] = True
                    device_state["port"] = port
                    broadcast({"type": "log", "text": "Serial connected: %s" % port, "level": "info"})
                    broadcast({"type": "wake"})
                else:
                    device_state["connected"] = False

        if not device_state["connected"]:
            time.sleep(2)
            continue

        try:
            raw = ser.readline()
            if raw:
                line = raw.decode("utf-8", errors="replace")
                parse_line(line)
        except serial.SerialException:
            device_state["connected"] = False
            broadcast({"type": "log", "text": "Serial disconnected", "level": "warn"})
            with serial_lock:
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
            time.sleep(2)
        except Exception as e:
            print("[reader] %s" % e)
            time.sleep(0.1)


# ---------------------------------------------------------------------------
# Watchdog thread
# ---------------------------------------------------------------------------

def watchdog_thread():
    while True:
        time.sleep(1)
        age = time.time() - last_hb_ts
        broadcast({"type": "hb_age", "age_s": round(age, 1)})


# ---------------------------------------------------------------------------
# HTTP server + Dashboard HTML
# ---------------------------------------------------------------------------

DASHBOARD_HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>HALO Diagnostic Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4"></script>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,monospace;background:#111;color:#e0e0e0;overflow-x:hidden}
#topbar{display:flex;align-items:center;gap:12px;padding:8px 16px;background:#1a1a2e;border-bottom:1px solid #333;flex-wrap:wrap}
#topbar .status{display:flex;align-items:center;gap:6px;font-size:13px}
#topbar .dot{width:10px;height:10px;border-radius:50%;display:inline-block}
.dot-green{background:#4caf50}.dot-red{background:#f44336}.dot-yellow{background:#ff9800}
.btn{padding:5px 12px;border:1px solid #555;border-radius:4px;background:#222;color:#ddd;cursor:pointer;font-size:12px;font-family:monospace}
.btn:hover{background:#333}
.btn-warn{border-color:#ff9800;color:#ff9800}
#panels{display:grid;grid-template-columns:1fr 1fr;gap:10px;padding:10px;height:calc(100vh - 46px)}
.panel{background:#1a1a2e;border:1px solid #333;border-radius:8px;padding:12px;display:flex;flex-direction:column;overflow:hidden}
.panel h2{font-size:13px;color:#888;margin-bottom:6px;text-transform:uppercase;letter-spacing:1px}
.big-row{display:flex;gap:20px;margin-bottom:6px;flex-wrap:wrap}
.big-stat{text-align:center}
.big-stat .val{font-size:36px;font-weight:700}
.big-stat .lbl{font-size:11px;color:#888;margin-top:2px}
.chart-wrap{flex:1;min-height:0;position:relative}
.chart-wrap canvas{width:100%!important;height:100%!important}
#event-log{flex:1;overflow-y:auto;font-size:11px;line-height:1.5;padding:4px 0}
#event-log .entry{padding:1px 6px;border-radius:3px}
.entry.info{color:#90caf9}.entry.warn{color:#ffb74d;background:rgba(255,183,77,0.07)}.entry.error{color:#ef5350;background:rgba(239,83,80,0.07)}
.info-bar{display:flex;gap:16px;font-size:12px;color:#888;padding:4px 0;flex-wrap:wrap}
.info-bar span{white-space:nowrap}
@media(max-width:900px){#panels{grid-template-columns:1fr;height:auto}.panel{min-height:280px}}
</style>
</head>
<body>
<div id="topbar">
  <div class="status"><span class="dot dot-red" id="conn-dot"></span><span id="conn-text">Connecting...</span></div>
  <div class="status"><span class="dot dot-red" id="sense-dot"></span><span id="sense-text">Sense: --</span></div>
  <div style="flex:1"></div>
  <button class="btn" onclick="sendCmd('ping')">Ping</button>
  <button class="btn" onclick="sendCmd('scan')">WiFi Scan</button>
  <button class="btn" onclick="sendCmd('wifitest')">WiFi Test</button>
  <button class="btn" onclick="sendCmd('wake')">Send Wake</button>
  <button class="btn" onclick="sendCmd('diag')">Diag On</button>
  <button class="btn" onclick="sendCmd('diagoff')">Diag Off</button>
  <button class="btn btn-warn" onclick="sendWake()">Tap Wake</button>
</div>
<div id="panels">
  <!-- WiFi RSSI panel -->
  <div class="panel">
    <h2>WiFi RSSI</h2>
    <div class="big-row">
      <div class="big-stat"><div class="val" id="rssi-val" style="color:#888">--</div><div class="lbl">dBm</div></div>
      <div class="big-stat"><div class="val" id="wifi-st" style="color:#888">--</div><div class="lbl">WiFi Status</div></div>
    </div>
    <div class="chart-wrap"><canvas id="rssiChart"></canvas></div>
  </div>

  <!-- UART Health panel -->
  <div class="panel">
    <h2>UART Link Health</h2>
    <div class="big-row">
      <div class="big-stat"><div class="val" id="hb-interval" style="color:#888">--</div><div class="lbl">HB Interval (ms)</div></div>
      <div class="big-stat"><div class="val" id="hb-missed" style="color:#4caf50">0</div><div class="lbl">Missed</div></div>
      <div class="big-stat"><div class="val" id="int-count" style="color:#888">--</div><div class="lbl">INT Count</div></div>
    </div>
    <div class="chart-wrap"><canvas id="hbChart"></canvas></div>
    <div class="info-bar">
      <span id="info-seq">Seq: --</span>
      <span id="info-rxct">Sense RX: --</span>
      <span id="info-heap">Heap: --</span>
      <span id="info-up">Up: --</span>
      <span id="info-intpin">INT pin: --</span>
    </div>
  </div>

  <!-- Event Log panel (full width) -->
  <div class="panel" style="grid-column: 1 / -1;">
    <h2>Event Log</h2>
    <div id="event-log"></div>
  </div>
</div>

<script>
const MAX_PTS = 180;
const MAX_LOG = 80;

const WIFI_STATUS_NAMES = {0:'IDLE',1:'NO_SSID',2:'SCAN_DONE',3:'CONNECTED',4:'CONNECT_FAILED',5:'CONNECTION_LOST',6:'DISCONNECTED',255:'NO_SHIELD'};

// --- RSSI chart ---
const rssiCtx = document.getElementById('rssiChart').getContext('2d');
const rssiData = {labels:[], datasets:[{
  label:'RSSI (dBm)', data:[], borderWidth:2, pointRadius:0, tension:0.3,
  segment:{borderColor:ctx=>{const v=ctx.p1.parsed.y; return v>-60?'#4caf50':v>-75?'#ff9800':'#f44336'}},
  borderColor:'#4caf50', fill:false
}]};
const rssiChart = new Chart(rssiCtx, {type:'line', data:rssiData, options:{
  responsive:true, maintainAspectRatio:false, animation:false,
  scales:{y:{min:-100,max:-30,grid:{color:'#333'},ticks:{color:'#888'}},x:{display:false}},
  plugins:{legend:{display:false}}
}});

// --- HB interval chart ---
const hbCtx = document.getElementById('hbChart').getContext('2d');
const hbData = {labels:[], datasets:[{
  label:'HB Interval (ms)', data:[], borderWidth:2, pointRadius:0, tension:0.3,
  segment:{borderColor:ctx=>{const v=ctx.p1.parsed.y; return v<1500?'#4caf50':v<3000?'#ff9800':'#f44336'}},
  borderColor:'#42a5f5', fill:false
}]};
const hbChart = new Chart(hbCtx, {type:'line', data:hbData, options:{
  responsive:true, maintainAspectRatio:false, animation:false,
  scales:{y:{min:0, suggestedMax:3000, grid:{color:'#333'}, ticks:{color:'#888'}}, x:{display:false}},
  plugins:{legend:{display:false}}
}});

function pushChart(chart, ds, val) {
  ds.data.push(val);
  chart.data.labels.push('');
  if (ds.data.length > MAX_PTS) { ds.data.shift(); chart.data.labels.shift(); }
  chart.update();
}

function addLog(text, level) {
  const el = document.getElementById('event-log');
  const ts = new Date().toLocaleTimeString();
  const div = document.createElement('div');
  div.className = 'entry ' + level;
  div.textContent = ts + '  ' + text;
  el.appendChild(div);
  while (el.children.length > MAX_LOG) el.removeChild(el.firstChild);
  el.scrollTop = el.scrollHeight;
}

function sendCmd(cmd) {
  fetch('/cmd/' + cmd, {method:'POST'}).then(r=>r.json()).then(d=>{
    addLog('Sent: ' + cmd, d.ok ? 'info' : 'error');
  }).catch(()=>addLog('Send failed: '+cmd,'error'));
}
function sendWake() {
  addLog('Sending wake tap...', 'warn');
  fetch('/wake', {method:'POST'}).then(r=>r.json()).then(d=>{
    addLog(d.ok ? 'Wake tap sent' : 'Wake failed: '+d.error, d.ok?'info':'error');
  }).catch(()=>addLog('Wake request failed','error'));
}

function rssiColor(v) { return v > -60 ? '#4caf50' : v > -75 ? '#ff9800' : '#f44336'; }
function wifiStatusName(code) { return WIFI_STATUS_NAMES[code] || ('?'+code); }

// --- SSE ---
function connectSSE() {
  const es = new EventSource('/events');
  es.onmessage = function(e) {
    const d = JSON.parse(e.data);
    switch(d.type) {
      case 'heartbeat': {
        // RSSI
        const rssi = d.rssi;
        const wst = d.wifi;
        if (wst === 3 && rssi !== 0) {
          document.getElementById('rssi-val').textContent = rssi;
          document.getElementById('rssi-val').style.color = rssiColor(rssi);
          pushChart(rssiChart, rssiData.datasets[0], rssi);
        } else {
          document.getElementById('rssi-val').textContent = '--';
          document.getElementById('rssi-val').style.color = '#f44336';
        }
        document.getElementById('wifi-st').textContent = wifiStatusName(wst);
        document.getElementById('wifi-st').style.color = wst===3 ? '#4caf50' : '#f44336';

        // HB interval
        const iv = d.interval_ms;
        document.getElementById('hb-interval').textContent = iv;
        document.getElementById('hb-interval').style.color = iv < 1500 ? '#4caf50' : iv < 3000 ? '#ff9800' : '#f44336';
        pushChart(hbChart, hbData.datasets[0], iv);

        // Missed
        const missed = d.missed;
        document.getElementById('hb-missed').textContent = missed;
        document.getElementById('hb-missed').style.color = missed === 0 ? '#4caf50' : '#f44336';

        // INT count
        document.getElementById('int-count').textContent = d.int_ct;
        document.getElementById('int-count').style.color = '#42a5f5';

        // Info bar
        document.getElementById('info-seq').textContent = 'Seq: ' + d.seq;
        document.getElementById('info-rxct').textContent = 'Sense RX: ' + d.rx_ct;
        document.getElementById('info-heap').textContent = 'Heap: ' + (d.heap/1024).toFixed(0) + 'KB';
        document.getElementById('info-up').textContent = 'Up: ' + (d.uptime/1000).toFixed(0) + 's';
        document.getElementById('info-intpin').textContent = 'INT pin: ' + d.int_pin;

        // Status indicators
        document.getElementById('sense-dot').className = 'dot dot-green';
        document.getElementById('sense-text').textContent = 'Sense: alive (seq ' + d.seq + ')';
        break;
      }
      case 'hb_age':
        if (d.age_s > 3) {
          document.getElementById('sense-dot').className = 'dot dot-red';
          document.getElementById('sense-text').textContent = 'Sense: silent ' + d.age_s.toFixed(0) + 's';
        }
        break;
      case 'link_hb': {
        // UART heartbeat from production firmware
        pushChart(hbChart, hbData.datasets[0], d.age_ms);
        document.getElementById('hb-interval').textContent = d.age_ms;
        document.getElementById('hb-interval').style.color = d.age_ms < 3000 ? '#4caf50' : d.age_ms < 5000 ? '#ff9800' : '#f44336';
        document.getElementById('sense-dot').className = 'dot dot-green';
        document.getElementById('sense-text').textContent = 'Sense: alive';
        break;
      }
      case 'pong':
        document.getElementById('sense-dot').className = 'dot dot-green';
        document.getElementById('sense-text').textContent = 'Sense: alive (pong)';
        break;
      case 'sleep':
        document.getElementById('sense-dot').className = 'dot dot-yellow';
        document.getElementById('sense-text').textContent = 'Sense: sleeping';
        document.getElementById('rssi-val').textContent = '--';
        document.getElementById('rssi-val').style.color = '#888';
        document.getElementById('wifi-st').textContent = 'ASLEEP';
        document.getElementById('wifi-st').style.color = '#ff9800';
        break;
      case 'wake':
        document.getElementById('conn-dot').className = 'dot dot-green';
        document.getElementById('conn-text').textContent = 'Connected';
        break;
      case 'scan_ap':
        addLog('AP: ' + d.ssid + '  RSSI=' + d.rssi + '  ch=' + d.channel, 'info');
        break;
      case 'log':
        addLog(d.text, d.level || 'info');
        if (d.text.includes('disconnected') || d.text.includes('Disconnected')) {
          document.getElementById('conn-dot').className = 'dot dot-red';
          document.getElementById('conn-text').textContent = 'Disconnected';
        }
        if (d.text.includes('connected') && d.text.includes('Serial')) {
          document.getElementById('conn-dot').className = 'dot dot-green';
          document.getElementById('conn-text').textContent = 'Connected';
        }
        break;
    }
  };
  es.onerror = function() {
    es.close();
    document.getElementById('conn-dot').className = 'dot dot-red';
    document.getElementById('conn-text').textContent = 'SSE lost - reconnecting...';
    setTimeout(connectSSE, 3000);
  };
}
connectSSE();
</script>
</body>
</html>"""


class DashboardHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        pass  # suppress access logs

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self._serve_html()
        elif self.path == "/events":
            self._serve_sse()
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path.startswith("/cmd/"):
            cmd = self.path[5:]
            allowed = {"ping", "scan", "int", "status", "help", "coldstart", "wake", "sleep", "diag", "diagoff", "wifitest", "ota", "wifi"}
            if cmd in allowed:
                write_serial(cmd)
                self._json_response({"ok": True, "cmd": cmd})
            else:
                self._json_response({"ok": False, "error": "unknown command: %s" % cmd}, 400)
        elif self.path == "/wake":
            self._handle_wake()
        else:
            self.send_error(404)

    def _serve_html(self):
        body = DASHBOARD_HTML.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        q = queue.Queue(maxsize=256)
        with sse_lock:
            sse_queues.append(q)

        init = {
            "type": "log",
            "text": "Dashboard connected. Port: %s" % device_state.get("port", "none"),
            "level": "info",
            "ts": time.time(),
        }
        try:
            self.wfile.write(("data: %s\n\n" % json.dumps(init)).encode())
            self.wfile.flush()
        except Exception:
            return

        try:
            while True:
                try:
                    payload = q.get(timeout=15)
                    self.wfile.write(payload.encode())
                    self.wfile.flush()
                except queue.Empty:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            with sse_lock:
                if q in sse_queues:
                    sse_queues.remove(q)

    def _handle_wake(self):
        if not os.path.exists(TAP_SCRIPT):
            self._json_response({"ok": False, "error": "tap.py not found at %s" % TAP_SCRIPT}, 404)
            return
        try:
            broadcast({"type": "log", "text": "Running tap.py wake actuator...", "level": "warn"})
            result = subprocess.run(
                [sys.executable, TAP_SCRIPT, "--port", "/dev/cu.usbmodem21301"],
                capture_output=True, text=True, timeout=15
            )
            if result.returncode == 0:
                broadcast({"type": "log", "text": "Wake tap completed", "level": "info"})
                broadcast({"type": "wake"})
                self._json_response({"ok": True})
            else:
                err = result.stderr.strip() or result.stdout.strip() or "unknown error"
                broadcast({"type": "log", "text": "Wake tap failed: %s" % err, "level": "error"})
                self._json_response({"ok": False, "error": err})
        except subprocess.TimeoutExpired:
            self._json_response({"ok": False, "error": "tap.py timed out"})
        except Exception as e:
            self._json_response({"ok": False, "error": str(e)})

    def _json_response(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="HALO Diagnostic Dashboard")
    parser.add_argument("--port", help="Serial port (auto-detect if omitted)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200)")
    parser.add_argument("--http-port", type=int, default=9099, help="HTTP port (default 9099)")
    args = parser.parse_args()

    port = args.port or find_lcd_port()
    if not port:
        print("ERROR: No LCD serial port found. Plug in device or use --port.")
        sys.exit(1)

    print("[dashboard] Serial port : %s" % port)
    print("[dashboard] Baud rate   : %d" % args.baud)
    print("[dashboard] Dashboard   : http://localhost:%d" % args.http_port)
    print("[dashboard] Tap script  : %s (%s)" % (TAP_SCRIPT, "found" if os.path.exists(TAP_SCRIPT) else "NOT FOUND"))
    print()

    device_state["port"] = port

    reader = threading.Thread(target=serial_reader, args=(port, args.baud), daemon=True)
    reader.start()

    wd = threading.Thread(target=watchdog_thread, daemon=True)
    wd.start()

    class ThreadedHTTPServer(HTTPServer):
        daemon_threads = True
        allow_reuse_address = True
        def process_request(self, request, client_address):
            t = threading.Thread(target=self._handle, args=(request, client_address), daemon=True)
            t.start()
        def _handle(self, request, client_address):
            try:
                self.finish_request(request, client_address)
            except Exception:
                self.handle_error(request, client_address)
            finally:
                self.shutdown_request(request)

    server = ThreadedHTTPServer(("0.0.0.0", args.http_port), DashboardHandler)
    print("[dashboard] Serving on http://localhost:%d -- press Ctrl+C to stop" % args.http_port)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[dashboard] Shutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()

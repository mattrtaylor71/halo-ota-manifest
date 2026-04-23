#!/usr/bin/env python3
"""
HALO Device Activity Monitor
Serves a live dashboard showing S3 uploads, presign Lambda logs, and image previews.
Browse all images by owner or view recent activity.
Usage: python3 device_monitor.py
Then open http://localhost:9090
"""

import json
import re
import time
import threading
from datetime import datetime, timedelta, timezone
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

import boto3

AWS_PROFILE = "trepo-dev"
AWS_REGION = "us-east-1"

# S3 buckets to monitor — each has owner-based prefixes
BUCKETS = [
    {"name": "trepo-grocery-uploads-dev", "prefix": "dish-images/", "label": "Dish"},
    {"name": "trepo-grocery-uploads-dev", "prefix": "images/", "label": "Check-in"},
    {"name": "trepo-grocery-discards-dev", "prefix": "images/", "label": "Discard"},
]

# CloudWatch log groups
LOG_GROUPS = [
    "/aws/lambda/trepo-grocery-backend-dev-PresignFunction-EOb8DRx0IhAC",
    "/aws/lambda/grocery-identifier-dev-IdentifyFastFunction-zCKXYCgkic9H",
    "/aws/lambda/grocery-identifier-dev-IdentifyFunction-85ovEoqMtgu6",
]

PORT = 9090

# Shared state
session = boto3.Session(profile_name=AWS_PROFILE, region_name=AWS_REGION)
s3 = session.client("s3")
logs_client = session.client("logs")

cache_lock = threading.Lock()
cached_data = {"owners": [], "logs": [], "last_refresh": None}

# Per-owner image cache: { owner_id: { "images": [...], "fetched_at": float } }
owner_image_cache = {}
owner_image_lock = threading.Lock()
OWNER_CACHE_TTL = 30  # seconds


def discover_owners():
    """Discover all owner IDs across both S3 buckets."""
    owner_set = set()
    for bucket_info in BUCKETS:
        bucket = bucket_info["name"]
        prefix = bucket_info["prefix"]
        try:
            resp = s3.list_objects_v2(Bucket=bucket, Prefix=prefix, Delimiter="/")
            for cp in resp.get("CommonPrefixes", []):
                p = cp["Prefix"]  # e.g. "images/7d7df434-.../""
                owner_id = p[len(prefix):].rstrip("/")
                if owner_id and "resized" not in owner_id:
                    owner_set.add(owner_id)
        except Exception as e:
            print(f"[WARN] Error listing owners in {bucket}: {e}")
    owners = sorted(owner_set)
    return owners


def fetch_owner_images(owner_id, max_images=200):
    """Fetch all images for a specific owner across both buckets."""
    all_objects = []
    for bucket_info in BUCKETS:
        bucket = bucket_info["name"]
        prefix = bucket_info["prefix"] + owner_id + "/"
        label = bucket_info["label"]
        try:
            paginator = s3.get_paginator("list_objects_v2")
            count = 0
            for page in paginator.paginate(Bucket=bucket, Prefix=prefix):
                for obj in page.get("Contents", []):
                    key = obj["Key"]
                    if not key.endswith(".jpg"):
                        continue
                    if "/resized/" in key:
                        continue
                    presigned = s3.generate_presigned_url(
                        "get_object",
                        Params={"Bucket": bucket, "Key": key},
                        ExpiresIn=3600,
                    )
                    all_objects.append({
                        "bucket": bucket,
                        "source": label,
                        "key": key,
                        "size": obj["Size"],
                        "timestamp": obj["LastModified"].isoformat(),
                        "ts_epoch": obj["LastModified"].timestamp(),
                        "url": presigned,
                    })
                    count += 1
                if count >= max_images:
                    break
        except Exception as e:
            print(f"[WARN] Error listing {bucket}/{prefix}: {e}")
    all_objects.sort(key=lambda x: x["ts_epoch"], reverse=True)
    return all_objects[:max_images]


def fetch_recent_logs(minutes=30):
    """Fetch recent CloudWatch log events from monitored Lambda functions."""
    start_ms = int((time.time() - minutes * 60) * 1000)
    all_events = []
    for group in LOG_GROUPS:
        short_name = group.split("/")[-1]
        if "-" in short_name:
            parts = short_name.split("-")
            for i, p in enumerate(parts):
                if p == "dev" and i + 1 < len(parts):
                    short_name = parts[i + 1]
                    break
        try:
            resp = logs_client.filter_log_events(
                logGroupName=group,
                startTime=start_ms,
                limit=50,
                interleaved=True,
            )
            for event in resp.get("events", []):
                msg = event.get("message", "").strip()
                if msg.startswith("START ") or msg.startswith("END ") or msg.startswith("REPORT "):
                    continue
                if not msg:
                    continue
                all_events.append({
                    "function": short_name,
                    "timestamp": datetime.fromtimestamp(
                        event["timestamp"] / 1000, tz=timezone.utc
                    ).isoformat(),
                    "ts_epoch": event["timestamp"] / 1000,
                    "message": msg[:500],
                })
        except Exception as e:
            print(f"[WARN] Error fetching logs from {group}: {e}")
    all_events.sort(key=lambda x: x["ts_epoch"], reverse=True)
    return all_events[:100]


def refresh_cache():
    """Background refresh of owner list + logs."""
    while True:
        try:
            owners = discover_owners()
            log_events = fetch_recent_logs(minutes=60)
            with cache_lock:
                cached_data["owners"] = owners
                cached_data["logs"] = log_events
                cached_data["last_refresh"] = datetime.now(timezone.utc).isoformat()
            print(f"[REFRESH] {len(owners)} owners, {len(log_events)} log events")
        except Exception as e:
            print(f"[ERROR] Refresh failed: {e}")
        time.sleep(30)


def get_owner_images_cached(owner_id):
    """Get images for an owner, using cache if fresh."""
    with owner_image_lock:
        entry = owner_image_cache.get(owner_id)
        if entry and (time.time() - entry["fetched_at"]) < OWNER_CACHE_TTL:
            return entry["images"]

    images = fetch_owner_images(owner_id)
    with owner_image_lock:
        owner_image_cache[owner_id] = {"images": images, "fetched_at": time.time()}
    return images


DASHBOARD_HTML = r"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HALO Device Monitor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#c9d1d9;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,monospace;padding:0}
header{background:#161b22;border-bottom:1px solid #30363d;padding:14px 20px;display:flex;justify-content:space-between;align-items:center;position:sticky;top:0;z-index:10}
header h1{font-size:18px;color:#58a6ff}
header .status{font-size:12px;color:#8b949e}
header .status span{color:#3fb950}

.owner-bar{background:#161b22;border-bottom:1px solid #30363d;padding:10px 20px;display:flex;align-items:center;gap:12px;position:sticky;top:52px;z-index:9}
.owner-bar label{font-size:12px;color:#8b949e;font-weight:600}
.owner-bar select{background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:6px 12px;font-size:13px;font-family:monospace;cursor:pointer;min-width:300px}
.owner-bar select:focus{border-color:#58a6ff;outline:none}
.owner-bar .owner-count{font-size:11px;color:#484f58}
.owner-bar .loading{font-size:11px;color:#d29922;display:none}

.tabs{display:flex;gap:0;background:#161b22;border-bottom:1px solid #30363d;position:sticky;top:94px;z-index:8}
.tab{padding:10px 24px;font-size:13px;color:#8b949e;cursor:pointer;border-bottom:2px solid transparent;font-weight:600}
.tab:hover{color:#c9d1d9}
.tab.active{color:#58a6ff;border-bottom-color:#58a6ff}
.content{padding:16px 20px}
.badge{display:inline-block;font-size:11px;padding:2px 8px;border-radius:12px;font-weight:600;margin-left:6px}
.badge-dish{background:#1f6feb33;color:#58a6ff;border:1px solid #1f6feb}
.badge-discard{background:#da363333;color:#f85149;border:1px solid #da3633}
.badge-checkin{background:#23863633;color:#3fb950;border:1px solid #238636}

.img-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:16px}
.img-card{background:#161b22;border:1px solid #30363d;border-radius:8px;overflow:hidden;transition:border-color .2s}
.img-card:hover{border-color:#58a6ff}
.img-card img{width:100%;aspect-ratio:4/3;object-fit:cover;display:block;background:#0d1117;cursor:pointer}
.img-card .meta{padding:10px 12px;font-size:11px;line-height:1.6;border-top:1px solid #21262d}
.img-card .meta .time{color:#58a6ff;font-weight:600;font-size:12px}
.img-card .meta .size{color:#f0883e}
.img-card .meta .key{color:#8b949e;word-break:break-all}

.log-table{width:100%;border-collapse:collapse;font-size:12px}
.log-table th{text-align:left;padding:8px 12px;background:#161b22;border-bottom:1px solid #30363d;color:#8b949e;font-weight:600;position:sticky;top:146px}
.log-table td{padding:6px 12px;border-bottom:1px solid #21262d;vertical-align:top}
.log-table tr:hover{background:#161b22}
.log-fn{color:#d2a8ff;white-space:nowrap}
.log-time{color:#8b949e;white-space:nowrap;font-size:11px}
.log-msg{color:#c9d1d9;word-break:break-all;max-width:800px;white-space:pre-wrap;font-family:monospace;font-size:11px}
.log-msg .json{color:#7ee787}

.stats{display:flex;gap:20px;margin-bottom:16px;flex-wrap:wrap}
.stat{background:#161b22;border:1px solid #30363d;border-radius:8px;padding:12px 20px;min-width:160px}
.stat .label{font-size:11px;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px}
.stat .value{font-size:24px;font-weight:700;color:#58a6ff;margin-top:2px}
.stat .sub{font-size:11px;color:#8b949e;margin-top:2px}

.lightbox{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.9);z-index:100;align-items:center;justify-content:center;cursor:pointer}
.lightbox.show{display:flex}
.lightbox img{max-width:95vw;max-height:95vh;object-fit:contain;border-radius:4px}

.empty{text-align:center;padding:60px 20px;color:#484f58;font-size:14px}
.auto-badge{font-size:10px;background:#238636;color:#fff;padding:2px 6px;border-radius:4px;margin-left:8px}
</style>
</head>
<body>
<header>
  <h1>HALO Device Monitor</h1>
  <div class="status">
    Last refresh: <span id="refreshTime">--</span>
    <span class="auto-badge" id="autoLabel">AUTO 30s</span>
  </div>
</header>

<div class="owner-bar">
  <label>Owner:</label>
  <select id="ownerSelect" onchange="onOwnerChange()">
    <option value="">Select an owner...</option>
  </select>
  <span class="owner-count" id="ownerCount"></span>
  <span class="loading" id="loadingLabel">Loading images...</span>
</div>

<div class="tabs">
  <div class="tab active" onclick="showTab('uploads')">Images <span id="uploadCount" class="badge badge-dish">0</span></div>
  <div class="tab" onclick="showTab('logs')">Lambda Logs <span id="logCount" class="badge badge-discard">0</span></div>
</div>

<div id="tab-uploads" class="content">
  <div class="stats" id="statsBar"></div>
  <div class="img-grid" id="imageGrid">
    <div class="empty">Select an owner above to browse their images.</div>
  </div>
</div>

<div id="tab-logs" class="content" style="display:none">
  <table class="log-table">
    <thead><tr><th>Time</th><th>Function</th><th>Message</th></tr></thead>
    <tbody id="logBody"></tbody>
  </table>
</div>

<div class="lightbox" id="lightbox" onclick="this.classList.remove('show')">
  <img id="lightboxImg" src="">
</div>

<script>
let currentTab = 'uploads';
let allOwners = [];
let currentOwner = '';
let currentImages = [];

function showTab(tab) {
  currentTab = tab;
  document.querySelectorAll('.tab').forEach(function(t) { t.classList.remove('active'); });
  document.querySelectorAll('.content').forEach(function(c) { c.style.display = 'none'; });
  event.target.closest('.tab').classList.add('active');
  document.getElementById('tab-' + tab).style.display = 'block';
}

function formatBytes(b) {
  if (b < 1024) return b + ' B';
  if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
  return (b / 1048576).toFixed(1) + ' MB';
}

function formatTime(iso) {
  var d = new Date(iso);
  var now = new Date();
  var diff = (now - d) / 1000;
  if (diff < 60) return Math.floor(diff) + 's ago';
  if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
  if (diff < 86400) return Math.floor(diff / 3600) + 'h ago';
  return d.toLocaleDateString() + ' ' + d.toLocaleTimeString();
}

function openLightbox(idx) {
  if (idx >= 0 && idx < currentImages.length) {
    document.getElementById('lightboxImg').src = currentImages[idx].url;
    document.getElementById('lightbox').classList.add('show');
  }
}

function populateOwners(owners) {
  allOwners = owners;
  var sel = document.getElementById('ownerSelect');
  // Keep first placeholder option, remove rest
  while (sel.options.length > 1) sel.remove(1);
  owners.forEach(function(o) {
    var opt = document.createElement('option');
    opt.value = o;
    // Shorten UUID-style owners for display
    if (/^[0-9a-f]{8}-/.test(o)) {
      opt.textContent = o.substring(0, 8) + '...' + o.substring(o.length - 4);
    } else {
      opt.textContent = o;
    }
    opt.title = o;
    sel.appendChild(opt);
  });
  document.getElementById('ownerCount').textContent = owners.length + ' owners';
  // Restore selection if previously set
  if (currentOwner) sel.value = currentOwner;
}

function onOwnerChange() {
  var sel = document.getElementById('ownerSelect');
  currentOwner = sel.value;
  if (!currentOwner) {
    currentImages = [];
    renderImages([]);
    return;
  }
  fetchOwnerImages(currentOwner);
}

function fetchOwnerImages(ownerId) {
  var loading = document.getElementById('loadingLabel');
  loading.style.display = 'inline';
  fetch('/api/images?owner=' + encodeURIComponent(ownerId))
    .then(function(r) { return r.json(); })
    .then(function(data) {
      loading.style.display = 'none';
      currentImages = data.images || [];
      renderImages(currentImages);
    })
    .catch(function(err) {
      loading.style.display = 'none';
      console.error('Fetch images error:', err);
    });
}

function renderImages(uploads) {
  var grid = document.getElementById('imageGrid');
  var stats = document.getElementById('statsBar');

  document.getElementById('uploadCount').textContent = uploads.length;

  if (uploads.length === 0) {
    stats.innerHTML = '';
    if (!currentOwner) {
      grid.innerHTML = '<div class="empty">Select an owner above to browse their images.</div>';
    } else {
      grid.innerHTML = '<div class="empty">No images found for this owner.</div>';
    }
    return;
  }

  var today = uploads.filter(function(u) {
    return new Date(u.timestamp).toDateString() === new Date().toDateString();
  });
  var totalSize = uploads.reduce(function(s, u) { return s + u.size; }, 0);
  var dishes = uploads.filter(function(u) { return u.source === 'Dish'; }).length;
  var checkins = uploads.filter(function(u) { return u.source === 'Check-in'; }).length;
  var discards = uploads.filter(function(u) { return u.source === 'Discard'; }).length;

  stats.innerHTML =
    '<div class="stat"><div class="label">Total</div><div class="value">' + uploads.length + '</div><div class="sub">' + formatBytes(totalSize) + '</div></div>' +
    '<div class="stat"><div class="label">Today</div><div class="value">' + today.length + '</div></div>' +
    '<div class="stat"><div class="label">Dishes</div><div class="value">' + dishes + '</div></div>' +
    '<div class="stat"><div class="label">Check-ins</div><div class="value">' + checkins + '</div></div>' +
    '<div class="stat"><div class="label">Discards</div><div class="value">' + discards + '</div></div>';

  grid.innerHTML = uploads.map(function(u, idx) {
    var badge;
    if (u.source === 'Discard') badge = '<span class="badge badge-discard">DISCARD</span>';
    else if (u.source === 'Check-in') badge = '<span class="badge badge-checkin">CHECK-IN</span>';
    else badge = '<span class="badge badge-dish">DISH</span>';
    var filename = u.key.split('/').pop();
    return '<div class="img-card">' +
      '<img src="' + u.url + '" loading="lazy" onclick="openLightbox(' + idx + ')" alt="capture">' +
      '<div class="meta">' +
        '<div class="time">' + formatTime(u.timestamp) + ' ' + badge + '</div>' +
        '<div class="size">' + formatBytes(u.size) + '</div>' +
        '<div class="key">' + filename + '</div>' +
      '</div></div>';
  }).join('');
}

function renderLogs(logs) {
  var body = document.getElementById('logBody');
  document.getElementById('logCount').textContent = logs.length;
  if (logs.length === 0) {
    body.innerHTML = '<tr><td colspan="3" class="empty">No log events in the last 60 minutes.</td></tr>';
    return;
  }
  body.innerHTML = logs.map(function(l) {
    var msg = l.message.replace(/</g, '&lt;');
    msg = msg.replace(/(\{[^}]+\})/g, '<span class="json">$1</span>');
    return '<tr>' +
      '<td class="log-time">' + formatTime(l.timestamp) + '</td>' +
      '<td class="log-fn">' + l.function + '</td>' +
      '<td class="log-msg">' + msg + '</td>' +
    '</tr>';
  }).join('');
}

function refreshMeta() {
  fetch('/api/data')
    .then(function(r) { return r.json(); })
    .then(function(data) {
      populateOwners(data.owners || []);
      renderLogs(data.logs || []);
      document.getElementById('refreshTime').textContent = data.last_refresh
        ? formatTime(data.last_refresh) : '--';
    })
    .catch(function(err) { console.error('Refresh error:', err); });
}

// INITIAL_DATA_PLACEHOLDER
refreshMeta();
setInterval(refreshMeta, 30000);
</script>
</body>
</html>"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Suppress default access log

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/" or parsed.path == "":
            with cache_lock:
                inline_json = json.dumps(cached_data).replace("</", "<\\/")
            html = DASHBOARD_HTML.replace(
                "// INITIAL_DATA_PLACEHOLDER",
                "var _initData = " + inline_json + ";\n"
                "populateOwners(_initData.owners || []);\n"
                "renderLogs(_initData.logs || []);\n"
                "document.getElementById('refreshTime').textContent = _initData.last_refresh ? formatTime(_initData.last_refresh) : '--';\n"
            )
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(html.encode())

        elif parsed.path == "/api/data":
            with cache_lock:
                data = json.dumps(cached_data)
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data.encode())

        elif parsed.path == "/api/images":
            qs = parse_qs(parsed.query)
            owner_id = qs.get("owner", [""])[0]
            if not owner_id or not re.match(r'^[a-zA-Z0-9_-]+(?:[a-zA-Z0-9_.-]*[a-zA-Z0-9_-])?$', owner_id):
                self.send_response(400)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"error":"invalid owner_id"}')
                return
            images = get_owner_images_cached(owner_id)
            data = json.dumps({"owner": owner_id, "images": images, "count": len(images)})
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data.encode())

        else:
            self.send_response(404)
            self.end_headers()


if __name__ == "__main__":
    print("[MONITOR] Loading initial data...")
    try:
        owners = discover_owners()
        log_events = fetch_recent_logs(minutes=60)
        with cache_lock:
            cached_data["owners"] = owners
            cached_data["logs"] = log_events
            cached_data["last_refresh"] = datetime.now(timezone.utc).isoformat()
        print(f"[MONITOR] Initial load: {len(owners)} owners, {len(log_events)} log events")
    except Exception as e:
        print(f"[WARN] Initial load failed ({e}), will retry in background")

    t = threading.Thread(target=refresh_cache, daemon=True)
    t.start()

    server = HTTPServer(("", PORT), Handler)
    print(f"[MONITOR] Dashboard running at http://localhost:{PORT}")
    print(f"[MONITOR] Monitoring {len(BUCKETS)} S3 buckets, {len(LOG_GROUPS)} log groups")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[MONITOR] Shutting down")
        server.server_close()

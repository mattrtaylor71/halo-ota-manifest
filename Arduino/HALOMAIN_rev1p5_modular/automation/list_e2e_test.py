#!/usr/bin/env python3
"""End-to-end test harness for the HALO shopping-list feature (LCD USB driven).

Drives the LCD board over its USB-CDC port (101) using the list-feature USB
commands (`list`, `refresh`, `pull`, `scroll <±n>`, `del N`, `home`, `fw`,
`liststate`) and asserts behavior from the single-line machine-readable
`[LISTSTATE] {...}` snapshots.

HARDWARE PREREQUISITES
  - LCD board on /dev/cu.usbmodem101 (port only enumerates while awake; the
    board deep-sleeps after ~10s idle and the port VANISHES).
  - The port MUST be opened with DTR/RTS deasserted BEFORE open() or the
    open resets the board (same proven pattern as overnight_combined_soak.py).
  - Tap actuator on /dev/cu.usbmodem21201 (tap_implementation/tap.py) is used
    to wake the device. After a tap, the port takes ~5-7s to appear.
  - NEVER open the Sense port (1101) — not needed, usually doesn't enumerate.
  - Firmware build with the new `pull` / `scroll` / `liststate` USB commands
    (not yet deployed at time of writing — see ASSUMPTIONS below).

STRICT REFRESH CRITERIA (default — see --lenient)
  A refresh cycle only PASSES if a FRESH UI_LIST was actually received by the
  LCD since the trigger. The LCD's 12s pill hard-timeout makes a timed-out
  refresh end in a clean-looking IDLE state, so "pill cleared + IDLE" alone is
  NOT success. Fresh-list evidence (any one suffices):
    (a) a serial line containing "Received UI_LIST" or "RX: type=UI_LIST"
        since the trigger,
    (b) liststate dedupe_age_ms (ms since last UI_LIST completion; -1 = never
        this boot) dropping BELOW the elapsed time since the trigger, or
    (c) liststate cache_age_s resetting to <=2 (and not predating the trigger).
  A cycle whose pill clears with NO such evidence FAILS with detail
  "refresh_timed_out_no_list". --lenient restores the old (pill-clear-only)
  behavior for debugging.

SCENARIOS (run all by default; --only NAME for one)
  1. cold_entry_cache        Let device deep-sleep, tap-wake, `list`; PASS if
                             liststate count>0 within 2s of entry (NVS cache
                             rendered across deep sleep / boot-load).
  2. entry_revalidate        Entry auto-triggers a background refresh; PASS if
                             pill 1->0, refresh_state ends IDLE/COMPLETE with
                             count>0 within 20s, AND a fresh UI_LIST was
                             received (strict criteria above).
  3. pull_refresh            `pull` (touch pull-to-refresh); PASS if pill shows
                             then clears within 15s, refresh_state ends
                             IDLE/COMPLETE, AND a fresh UI_LIST was received.
                             Records time-to-clear and time-to-list.
  4. refresh_spam            `pull` x5, 1s apart; PASS if final pill=0 +
                             refresh_state IDLE/COMPLETE within 25s, a fresh
                             UI_LIST arrived, no snapshot ever shows
                             auto_retry>=3 with pill stuck, AND each answered
                             cycle lands within budget: WARN >5s (non-fatal),
                             FAIL if any answer >20s or if ALL answers >10s
                             (systemic slowness).
  5. double_refresh          `pull` -> completion -> ASSERT latch==0 -> 1s ->
                             `pull` -> completion -> ASSERT latch==0 again.
                             Each cycle must receive a fresh UI_LIST (strict)
                             and is budget-checked like scenario 4. Regression
                             guard for the gesture-latch wedge (fast refresh
                             rebuild eats SCROLL_END; latch never re-arms).
                             FAIL detail "latch_wedged".
  6. encoder_overscroll_refresh  `scroll -1` x3 @300ms (3 CCW ticks at top =
                             refresh trigger, SHOPPING_LIST_REFRESH_TICKS=3);
                             PASS like scenario 3 (strict fresh-list check).
  7. scroll_responsiveness   `scroll 1` x10 @150ms; PASS if selected index
                             advanced and the port stayed alive (liveness
                             only — render timing isn't measurable over USB).
  8. delete_item             DATA-SAFE delete + restore. Snapshots the user's
                             REAL backend list via the list API ("view"),
                             then `del 0`, confirms count dropped by 1, parses
                             the deleted item's id from the `[USB] del 0 ->
                             <uuid>` serial line, RESTORES the item via the
                             list API "add" operation, triggers one device
                             refresh, and asserts count returned to the
                             pre-delete value. Skips (non-destructively) if
                             the pre-delete API view fails — never deletes an
                             item it cannot restore.
  9. sleep_wake_cache        `home`, wait for port to vanish (deep sleep),
                             tap-wake, reopen, `list`+`liststate`; PASS if
                             count survives (validates persist-on-update,
                             not just boot-load).

FLAGS
  --only NAME          Run a single scenario.
  --keep-awake         Skip the sleep-dependent scenarios (1, 9).
  --lenient            Restore the OLD refresh pass criteria (pill clear +
                       IDLE/COMPLETE only, no fresh-UI_LIST requirement) and
                       skip the cycle-budget failures. Debugging only.
  --owner-id UUID      Backend ownerId for scenario 8's API view/add calls.
  --list-api-url URL   Backend list endpoint override.
  --jsonl PATH         Machine log (default /tmp/list_e2e.jsonl), one record
                       per scenario + a final summary record.
  --lcd-port / --tap-port / --tap-command   Hardware overrides.
  --verbose            Echo every serial RX line (default: only interesting
                       [SHOPPING_LIST]/[LISTSTATE]/[USB]/UI_LIST/[LIST lines).

Exit code 0 only if every executed scenario passed.

BACKEND LIST API (used by scenario 8 only; stdlib urllib, no extra deps):
  POST {list-api-url} with JSON:
    view:   {"operation":"view","ownerId":...,"device":"halo"}
            -> {"items":[{"id":...,"product_name":...,"_device":...,...}]}
    add:    {"operation":"add","ownerId":...,"device":...,"product_name":...,
             [product_brand, product_barcode, store]}  (action dflt "ADDED")
  Shape VERIFIED against trepo-esp32-list-voice-api.md §2.2 and the deployed
  Lambda source (trepov2/APP/Lambdas/listHandler.js, operation === 'add').
  NOTE: restore creates a NEW backend row id — the item's name/brand/barcode/
  store are preserved but its id changes.

ASSUMPTIONS TO VERIFY ON THE FIRST LIVE RUN (search "ASSUMPTION" below):
  - refresh_state enum order — VERIFIED against LCD_Minimal.ino:787-791
    (REFRESH_IDLE=0, WAKE_PENDING=1, INFLIGHT=2, COMPLETE=3, FAILED=4).
  - `del N` is 0-based, and the LCD prints `[USB] del 0 -> <uuid>` — VERIFIED
    against lcd_ui_task.h:1040; the uuid is the backend item id as a string.
  - `dedupe_age_ms` in [LISTSTATE]: ms since last UI_LIST completion, capped
    at 99999, -1 if never this boot — VERIFIED against lcd_ui_task.h:1004.
  - "Received UI_LIST" serial marker — VERIFIED against lcd_uart_rx.h:1019
    ("[UART] Received UI_LIST: count=..."). The "UI_LIST deduped" line does
    NOT count as fresh-list evidence (it is a drop, not an apply).
  - `scroll 1` (positive / CW) advances `selected` downward; `scroll -1`
    (CCW) at the top contributes to the overscroll-refresh trigger.
  - `[LISTSTATE]` is printed as exactly one line of JSON after the tag.
  - The `screen` field's enum mapping is unknown, so it is logged but never
    asserted on.
  - Scenarios 2/3/6 soft-pass if the pill=1 phase was too fast to observe
    but a fresh UI_LIST (or busy/COMPLETE state) was seen (detail notes it).
  - Scenario 8 matches the deleted uuid against the API view's `id` (str
    compare); device row 0 need not be API row 0 (CHECKED items filtered).
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

import serial

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(SCRIPT_DIR)

LCD_GLOB = "/dev/cu.usbmodem101"
ACT_PORT = "/dev/cu.usbmodem21201"
TAP_SCRIPT = os.path.join(REPO, "tap_implementation", "tap.py")
TAP_COMMAND = "TAP"
BAUD = 115200
JSONL_DEFAULT = "/tmp/list_e2e.jsonl"

# Backend list API (scenario 8: data-safe delete + restore).
# Operation shapes verified against trepo-esp32-list-voice-api.md and
# trepov2/APP/Lambdas/listHandler.js.
LIST_API_URL_DEFAULT = "https://1zc0nh8x48.execute-api.us-east-1.amazonaws.com/v1/list"
OWNER_ID_DEFAULT = "7d7df434-d942-4037-b054-2d3005ea6abc"
API_DEVICE = "halo"
API_TIMEOUT_S = 15

# Serial RX prefixes worth echoing to the console / attaching to results.
INTERESTING_PREFIXES = ("[SHOPPING_LIST]", "[LISTSTATE]", "[USB]", "UI_LIST", "[LIST")

# Serial markers proving the LCD actually RECEIVED + APPLIED a fresh UI_LIST.
# "[UART] UI_LIST deduped ..." intentionally does NOT match (it's a drop).
UI_LIST_RX_MARKERS = ("Received UI_LIST", "RX: type=UI_LIST")


def line_is_ui_list_rx(line):
    return any(m in line for m in UI_LIST_RX_MARKERS)


def line_is_interesting(line):
    return line.startswith(INTERESTING_PREFIXES) or line_is_ui_list_rx(line)

# RefreshState enum — VERIFIED against LCD_Minimal/LCD_Minimal.ino:787-791:
#   enum RefreshState { REFRESH_IDLE = 0, REFRESH_WAKE_PENDING,
#                       REFRESH_INFLIGHT, REFRESH_COMPLETE, REFRESH_FAILED };
REFRESH_IDLE = 0
REFRESH_WAKE_PENDING = 1
REFRESH_INFLIGHT = 2
REFRESH_COMPLETE = 3
REFRESH_FAILED = 4
REFRESH_OK_FINAL = (REFRESH_IDLE, REFRESH_COMPLETE)
REFRESH_NAMES = {0: "IDLE", 1: "WAKE_PENDING", 2: "INFLIGHT", 3: "COMPLETE", 4: "FAILED"}

# Timing knobs (seconds unless noted)
PORT_APPEAR_AFTER_TAP_S = 8.0   # spec: ~5-7s for port 101 to appear post-tap
SLEEP_VANISH_TIMEOUT_S = 60.0   # scenario 1: wait for port to vanish (home-screen idle->sleep takes ~32s)
SLEEP_VANISH_TIMEOUT_LONG_S = 60.0  # scenario 9 (post-OTA ota_stay_awake delays sleep past the ~32s norm)
COLD_CACHE_WINDOW_S = 2.0       # scenario 1: count>0 within 2s of `list`
REVALIDATE_TIMEOUT_S = 20.0     # scenario 2
PULL_CLEAR_TIMEOUT_S = 15.0     # scenarios 3, 5, 6
SPAM_SETTLE_TIMEOUT_S = 25.0    # scenario 4
LISTSTATE_POLL_S = 0.5
LISTSTATE_REPLY_TIMEOUT_S = 2.0
STRICT_LIST_GRACE_S = 3.0       # after pill clears, grace for late UI_LIST evidence
DEL_LINE_TIMEOUT_S = 4.0        # scenario 8: wait for `[USB] del 0 -> <uuid>`

# Refresh-cycle answer budgets (scenarios 4 & 5). The Sense-side dedup fix
# makes fast answers the norm; single slow cycles only WARN (real WiFi
# varies), but ALL cycles slow = systemic = FAIL.
REFRESH_CYCLE_WARN_S = 5.0      # warn (non-fatal) if an answer takes longer
REFRESH_CYCLE_FAIL_S = 20.0     # any single answer slower than this = FAIL
REFRESH_CYCLE_SYSTEMIC_S = 10.0  # ALL answers slower than this = FAIL


class PortDropped(Exception):
    """LCD USB port vanished / write failed mid-scenario (unexpected sleep)."""


# --------------------------------------------------------------------------
# logging
# --------------------------------------------------------------------------

JSONL_PATH = JSONL_DEFAULT


def logln(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def record(obj):
    obj["t"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    try:
        with open(JSONL_PATH, "a") as f:
            f.write(json.dumps(obj) + "\n")
    except Exception:
        pass


# --------------------------------------------------------------------------
# backend list API (scenario 8 only — stdlib urllib, no extra deps)
# --------------------------------------------------------------------------

def api_list_post(url, payload, timeout=API_TIMEOUT_S):
    """POST a JSON payload to the list API. Returns (status, parsed_body);
    (None, None) on any network/HTTP/parse failure (logged, never raises)."""
    try:
        req = urllib.request.Request(
            url, data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status = resp.getcode()
            body = json.loads(resp.read().decode("utf-8", "ignore"))
            return status, body
    except urllib.error.HTTPError as e:
        logln("  API HTTP %s for operation=%s" % (e.code, payload.get("operation")))
        return None, None
    except Exception as e:
        logln("  API error for operation=%s: %s" % (payload.get("operation"), e))
        return None, None


def api_list_view(url, owner_id):
    """Fetch the full backend list. Returns the items list or None on failure."""
    status, body = api_list_post(url, {
        "operation": "view", "ownerId": owner_id, "device": API_DEVICE})
    if status != 200 or not isinstance(body, dict):
        return None
    items = body.get("items")
    return items if isinstance(items, list) else None


def api_list_add(url, owner_id, item):
    """Restore one item (from a prior view snapshot) via the add operation.
    Shape verified against trepo-esp32-list-voice-api.md §2.2 +
    trepov2/APP/Lambdas/listHandler.js. Returns True on HTTP 200.
    NOTE: the restored row gets a NEW backend id."""
    payload = {
        "operation": "add",
        "ownerId": owner_id,
        "device": item.get("_device") or API_DEVICE,
        "product_name": item.get("product_name"),
    }
    for k in ("product_brand", "product_barcode", "store"):
        v = item.get(k)
        if v:
            payload[k] = v
    status, body = api_list_post(url, payload)
    ok = (status == 200)
    logln("  API add %r -> %s" % (payload.get("product_name"),
                                  "OK" if ok else "FAILED (status=%s)" % status))
    return ok


# --------------------------------------------------------------------------
# port / actuator plumbing
# --------------------------------------------------------------------------

def port_present():
    return bool(glob.glob(LCD_GLOB))


def wait_for_port(present, timeout):
    """Wait until the LCD port is present (True) or absent (False)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if port_present() == present:
            return True
        time.sleep(0.3)
    return port_present() == present


def tap(tap_port, tap_command):
    logln("  tap (%s on %s)" % (tap_command, tap_port))
    try:
        subprocess.run(
            ["python3", TAP_SCRIPT, "--command", tap_command,
             "--port", tap_port, "--timeout", "7", "--quiet"],
            cwd=REPO, timeout=30,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception as e:
        logln("  tap err %s" % e)


class LcdLink:
    """LCD USB serial wrapper: dtr/rts-safe open + background reader thread.

    The reader accumulates (timestamp, line) tuples; liststate() and
    wait_for_line() scan the accumulated buffer so nothing is lost between
    a send and the matching reply.
    """

    def __init__(self, verbose=False):
        self.ser = None
        self.verbose = verbose
        self.lines = []                 # [(ts, line), ...]
        self._lock = threading.Lock()
        self._reader = None
        self._stop = threading.Event()
        self._dropped = threading.Event()

    @property
    def is_open(self):
        return self.ser is not None and not self._dropped.is_set()

    def open(self):
        """Open port 101 with DTR/RTS deasserted BEFORE open (proven pattern
        from overnight_combined_soak.py — anything else resets the board)."""
        g = glob.glob(LCD_GLOB)
        if not g:
            raise PortDropped("port_absent")
        s = serial.Serial()
        s.port = g[0]
        s.baudrate = BAUD
        s.timeout = 0.15
        s.dtr = False
        s.rts = False
        s.open()
        self.ser = s
        self._stop.clear()
        self._dropped.clear()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        # let boot/echo chatter drain so the first query reads clean
        time.sleep(0.3)

    def close(self):
        self._stop.set()
        if self._reader:
            self._reader.join(timeout=2.0)
            self._reader = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def _read_loop(self):
        while not self._stop.is_set():
            try:
                raw = self.ser.readline()
            except Exception:
                self._dropped.set()
                return
            if not raw:
                continue
            line = raw.decode("utf-8", "ignore").strip()
            if not line:
                continue
            with self._lock:
                self.lines.append((time.time(), line))
            if self.verbose or line_is_interesting(line):
                logln("    RX %s" % line)

    def mark(self):
        """Return the current buffer index; pass to scan helpers to only
        consider lines that arrive after this point."""
        with self._lock:
            return len(self.lines)

    def lines_since(self, mark):
        with self._lock:
            return list(self.lines[mark:])

    def interesting_since(self, mark, limit=30):
        out = [ln for _, ln in self.lines_since(mark)
               if line_is_interesting(ln)]
        return out[-limit:]

    def send(self, cmd):
        """Send one newline-terminated command. Raises PortDropped on failure
        (port vanished == the LCD went to sleep under us)."""
        if not self.is_open:
            raise PortDropped("port_not_open")
        try:
            self.ser.write((cmd + "\n").encode())
            self.ser.flush()
        except Exception as e:
            self._dropped.set()
            raise PortDropped("write_failed:%s" % e)
        logln("  TX %s" % cmd)

    def wait_for_line(self, predicate, timeout, mark=None):
        """Wait for the first buffered line (at/after mark) matching
        predicate(line). Returns the line or None on timeout."""
        idx = self.mark() if mark is None else mark
        deadline = time.time() + timeout
        while time.time() < deadline:
            new = self.lines_since(idx)
            idx += len(new)
            for _, ln in new:
                if predicate(ln):
                    return ln
            if self._dropped.is_set():
                raise PortDropped("port_dropped_while_waiting")
            time.sleep(0.05)
        return None

    def liststate(self, timeout=LISTSTATE_REPLY_TIMEOUT_S):
        """Send `liststate`, parse the one-line [LISTSTATE] {...} reply.

        Returns the parsed dict (keys: screen, refresh_state, pill, count,
        cache_age_s, auto_retry, selected, latch, armed, scroll_y,
        pill_hiding) or None on timeout/parse failure.
        """
        m = self.mark()
        self.send("liststate")
        ln = self.wait_for_line(lambda l: "[LISTSTATE]" in l, timeout, mark=m)
        if ln is None:
            return None
        try:
            payload = ln[ln.index("{"):]
            return json.loads(payload)
        except Exception:
            logln("  WARN unparseable liststate line: %r" % ln)
            return None


# --------------------------------------------------------------------------
# harness
# --------------------------------------------------------------------------

class Harness:
    def __init__(self, args):
        self.args = args
        self.link = LcdLink(verbose=args.verbose)
        self.ctx = {}  # cross-scenario context (e.g. list entry timestamp)

    # -- lifecycle ---------------------------------------------------------

    def tap(self):
        tap(self.args.tap_port, self.args.tap_command)

    def wake_and_open(self, attempts=3):
        """Ensure port 101 exists and is open: tap-wake as needed."""
        if self.link.is_open:
            return
        self.link.close()
        for i in range(attempts):
            if not port_present():
                self.tap()
                if not wait_for_port(True, PORT_APPEAR_AFTER_TAP_S):
                    logln("  port did not appear after tap (try %d/%d)" % (i + 1, attempts))
                    continue
            try:
                self.link.open()
                return
            except Exception as e:
                logln("  open failed: %s (try %d/%d)" % (e, i + 1, attempts))
                self.link.close()
                time.sleep(1.0)
        raise PortDropped("could_not_open_lcd_port")

    def let_sleep(self, timeout):
        """Send `home`, release the port, and wait for it to vanish (sleep).
        Returns True if the device slept within timeout."""
        if self.link.is_open:
            try:
                self.link.send("home")
            except PortDropped:
                pass
        self.link.close()
        # NOTE: closing our handle is required — a held-open CDC port can keep
        # the device's USB session alive on some hosts.
        return wait_for_port(False, timeout)

    def recover_after_drop(self):
        """Defensive: port vanished mid-scenario. Tap + reopen so the next
        scenario can run."""
        logln("  recovering: tap + reopen after port drop")
        self.link.close()
        try:
            self.wake_and_open()
        except PortDropped:
            logln("  WARN recovery failed — next scenario will retry")

    # -- shared assertion helpers ------------------------------------------

    def enter_list(self):
        """Send `list`, record entry time + serial mark in ctx (the mark/time
        pair is the refresh trigger reference for entry_revalidate)."""
        self.ctx["list_entry_mark"] = self.link.mark()
        self.link.send("list")
        self.ctx["list_entered_at"] = time.time()

    def poll_refresh_cycle(self, timeout, require_count=False,
                           require_pill_seen=True, poll_s=LISTSTATE_POLL_S,
                           trigger_mark=None, trigger_time=None):
        """Poll `liststate` until the refresh cycle completes (or timeout).
        Returns a dict describing the observed cycle.

        STRICT (default): completion = pill==0 + refresh_state in
        {IDLE,COMPLETE} AND fresh-UI_LIST evidence since the trigger:
          (a) a serial line matching UI_LIST_RX_MARKERS at/after trigger_mark,
          (b) dedupe_age_ms < elapsed-since-trigger (last UI_LIST completion
              happened AFTER the trigger; -1/absent = no evidence), or
          (c) cache_age_s in [0,2] and not predating the trigger.
        A pill that clears with no evidence is the LCD's 12s hard-timeout
        path — the cycle FAILS with note "refresh_timed_out_no_list" (after a
        short grace for late-arriving serial evidence). --lenient restores
        the old pill-clear-only criteria.

        trigger_mark/trigger_time default to the buffer index / wall clock at
        call entry; callers should pass values captured BEFORE sending the
        trigger command so no evidence is missed.

        soft-pass: if the pill=1 phase was never observed but the cycle ended
        COMPLETE / busy was seen / a fresh list arrived, the refresh clearly
        ran but was too fast to sample — counted as a pass with a note.
        """
        lenient = bool(getattr(self.args, "lenient", False))
        t0 = time.time()
        if trigger_time is None:
            trigger_time = t0
        scan_idx = self.link.mark() if trigger_mark is None else trigger_mark
        pill_seen = False
        busy_state_seen = False   # WAKE_PENDING/INFLIGHT observed
        complete_seen = False
        stuck_retry = False       # any snapshot with auto_retry>=3 while pill=1
        list_received = False     # fresh UI_LIST evidence since trigger
        list_evidence = []
        time_to_list = None       # s from trigger to first fresh-list evidence
        snapshots = 0
        final = None
        time_to_clear = None
        cleared_at = None
        while time.time() - t0 < timeout:
            # (a) serial evidence: fresh UI_LIST received since the trigger
            new = self.link.lines_since(scan_idx)
            scan_idx += len(new)
            for ts, ln in new:
                if line_is_ui_list_rx(ln) and not list_received:
                    list_received = True
                    list_evidence.append("serial_ui_list")
                    time_to_list = round(max(0.0, ts - trigger_time), 2)
            st = self.link.liststate()
            if st is not None:
                snapshots += 1
                final = st
                now = time.time()
                elapsed_s = now - trigger_time
                rs = int(st.get("refresh_state", -1))
                pill = int(st.get("pill", 0))
                if pill == 1:
                    pill_seen = True
                    if int(st.get("auto_retry", 0)) >= 3:
                        stuck_retry = True
                if rs in (REFRESH_WAKE_PENDING, REFRESH_INFLIGHT):
                    busy_state_seen = True
                if rs == REFRESH_COMPLETE:
                    complete_seen = True
                # (b) dedupe_age_ms: last UI_LIST completion AFTER the trigger
                da = st.get("dedupe_age_ms")
                if da is not None and not list_received:
                    da = int(da)
                    if 0 <= da < elapsed_s * 1000.0:
                        list_received = True
                        list_evidence.append("dedupe_age_ms")
                        time_to_list = round(max(0.0, elapsed_s - da / 1000.0), 2)
                # (c) cache_age_s reset to ~0 (and the save postdates the trigger)
                ca = st.get("cache_age_s")
                if ca is not None and not list_received:
                    ca = int(ca)
                    if 0 <= ca <= 2 and ca <= elapsed_s + 0.5:
                        list_received = True
                        list_evidence.append("cache_age_s")
                        time_to_list = round(max(0.0, elapsed_s - ca), 2)
                activity = (pill_seen or busy_state_seen or complete_seen
                            or list_received)
                cleared = (pill == 0 and rs in REFRESH_OK_FINAL)
                if cleared and activity:
                    if time_to_clear is None:
                        time_to_clear = round(time.time() - t0, 2)
                        cleared_at = time.time()
                    if lenient or list_received:
                        break
                    # strict: pill cleared but no list yet — allow a short
                    # grace for late serial evidence, then give up (this is
                    # the 12s-hard-timeout signature).
                    if time.time() - cleared_at > STRICT_LIST_GRACE_S:
                        break
            time.sleep(poll_s)
        ok = final is not None
        notes = []
        if ok:
            rs = int(final.get("refresh_state", -1))
            pill = int(final.get("pill", 1))
            ok = (pill == 0 and rs in REFRESH_OK_FINAL)
            if ok and not lenient and not list_received:
                ok = False
                notes.append("refresh_timed_out_no_list")
            if ok and require_pill_seen and not pill_seen:
                if busy_state_seen or complete_seen or list_received:
                    notes.append("pill_transition_missed_fast_refresh")
                else:
                    ok = False
                    notes.append("no_refresh_activity_observed")
            if ok and require_count and int(final.get("count", 0)) <= 0:
                ok = False
                notes.append("count_zero_after_refresh")
        else:
            notes.append("no_liststate_reply")
        return {
            "ok": ok,
            "strict": not lenient,
            "pill_seen": pill_seen,
            "busy_state_seen": busy_state_seen,
            "complete_seen": complete_seen,
            "list_received": list_received,
            "list_evidence": list_evidence,
            "time_to_list_s": time_to_list,
            "stuck_retry": stuck_retry,
            "snapshots": snapshots,
            "time_to_clear_s": time_to_clear,
            "final": final,
            "final_refresh_name": REFRESH_NAMES.get(
                int(final.get("refresh_state", -1)), "?") if final else None,
            "notes": notes,
        }


def assess_cycle_budget(latencies):
    """Budget-check the answered refresh cycles of a scenario (scenarios 4/5).

    latencies: seconds from trigger to the answering UI_LIST, one per
    answered cycle. Returns (ok, notes):
      - WARN (non-fatal note) for any cycle > REFRESH_CYCLE_WARN_S,
      - FAIL for any single cycle > REFRESH_CYCLE_FAIL_S,
      - FAIL if ALL cycles > REFRESH_CYCLE_SYSTEMIC_S (systemic slowness).
    An empty latencies list returns ok (the strict fresh-list criteria
    already fail no-answer scenarios)."""
    notes = []
    ok = True
    if not latencies:
        return ok, notes
    for s in latencies:
        if s > REFRESH_CYCLE_FAIL_S:
            ok = False
            notes.append("cycle_%.1fs_exceeds_%.0fs_budget" % (s, REFRESH_CYCLE_FAIL_S))
        elif s > REFRESH_CYCLE_WARN_S:
            notes.append("WARN_cycle_%.1fs_over_%.0fs" % (s, REFRESH_CYCLE_WARN_S))
    if all(s > REFRESH_CYCLE_SYSTEMIC_S for s in latencies):
        ok = False
        notes.append("all_cycles_over_%.0fs_systemic" % REFRESH_CYCLE_SYSTEMIC_S)
    return ok, notes


# --------------------------------------------------------------------------
# version check (always-run first step; warns, never fails the run)
# --------------------------------------------------------------------------

def version_check(h):
    t0 = time.time()
    try:
        h.wake_and_open()
        m = h.link.mark()
        h.link.send("fw")
        # `fw` output format varies; grab the first line that mentions a
        # version-looking token or fw/version keyword.
        ln = h.link.wait_for_line(
            lambda l: ("fw" in l.lower() or "version" in l.lower()
                       or "6.1." in l), 3.0, mark=m)
        if ln:
            logln("FW VERSION: %s" % ln)
            record({"kind": "version", "raw": ln})
        else:
            logln("WARN: could not parse fw version from `fw` output (continuing)")
            record({"kind": "version", "raw": None, "warn": "unparsed"})
    except PortDropped as e:
        logln("WARN: version check skipped (%s)" % e)
        record({"kind": "version", "raw": None, "warn": str(e)})
    logln("version check took %.1fs" % (time.time() - t0))


# --------------------------------------------------------------------------
# scenarios — each returns {name, pass, detail, timings}
# --------------------------------------------------------------------------

def scen_cold_entry_cache(h):
    name = "cold_entry_cache"
    timings = {}
    # 1) ensure asleep
    t0 = time.time()
    slept = h.let_sleep(SLEEP_VANISH_TIMEOUT_S)
    timings["sleep_wait_s"] = round(time.time() - t0, 1)
    if not slept:
        return {"name": name, "pass": False,
                "detail": "device_did_not_sleep_within_%ds" % int(SLEEP_VANISH_TIMEOUT_S),
                "timings": timings}
    # 2) tap to wake, open, enter list
    t0 = time.time()
    h.wake_and_open()
    timings["wake_open_s"] = round(time.time() - t0, 1)
    mark = h.link.mark()
    h.enter_list()
    # 3) count>0 within 2s of entry (cached list rendered from NVS)
    entered = h.ctx["list_entered_at"]
    count = -1
    final = None
    while time.time() - entered < COLD_CACHE_WINDOW_S:
        st = h.link.liststate(timeout=0.8)
        if st is not None:
            final = st
            count = int(st.get("count", 0))
            if count > 0:
                break
        time.sleep(0.1)
    timings["entry_to_count_s"] = round(time.time() - entered, 2)
    ok = count > 0
    h.ctx["cold_entry_count"] = count
    h.ctx["entry_revalidate_armed"] = ok  # scenario 2 rides this entry
    return {"name": name, "pass": ok,
            "detail": ("cached_count=%d" % count) if ok else
                      ("no_cached_list (liststate=%s)" % json.dumps(final)),
            "timings": timings,
            "serial": h.link.interesting_since(mark)}


def scen_entry_revalidate(h):
    """Entry auto-refresh must deliver a FRESH UI_LIST (strict criteria) —
    a pill that clears via the LCD 12s hard-timeout fails."""
    name = "entry_revalidate"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()
    if not h.ctx.get("entry_revalidate_armed"):
        # Scenario 1 didn't run (e.g. --keep-awake): trigger a fresh entry
        # ourselves so the entry-revalidate path fires.
        h.link.send("home")
        time.sleep(1.0)
        h.enter_list()
        time.sleep(0.3)
    # the list entry IS the refresh trigger (mark/time recorded by enter_list)
    trig_mark = h.ctx.get("list_entry_mark", mark)
    trig_time = h.ctx.get("list_entered_at")
    t0 = time.time()
    cyc = h.poll_refresh_cycle(REVALIDATE_TIMEOUT_S, require_count=True,
                               trigger_mark=trig_mark, trigger_time=trig_time)
    timings["revalidate_s"] = round(time.time() - t0, 1)
    timings["time_to_clear_s"] = cyc["time_to_clear_s"]
    timings["time_to_list_s"] = cyc["time_to_list_s"]
    h.ctx["entry_revalidate_armed"] = False
    detail = ("pill_seen=%s list=%s(%s) final=%s %s" %
              (cyc["pill_seen"], cyc["list_received"],
               ",".join(cyc["list_evidence"]) or "-",
               cyc["final_refresh_name"],
               ",".join(cyc["notes"]) or "clean"))
    return {"name": name, "pass": cyc["ok"], "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_pull_refresh(h):
    """`pull` must complete AND receive a fresh UI_LIST (strict criteria)."""
    name = "pull_refresh"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()
    h.link.send("pull")
    t0 = time.time()
    cyc = h.poll_refresh_cycle(PULL_CLEAR_TIMEOUT_S,
                               trigger_mark=mark, trigger_time=t0)
    timings["time_to_clear_s"] = cyc["time_to_clear_s"]
    timings["time_to_list_s"] = cyc["time_to_list_s"]
    timings["total_s"] = round(time.time() - t0, 1)
    detail = ("pill_seen=%s list=%s(%s) final=%s ttc=%s ttl=%s %s" %
              (cyc["pill_seen"], cyc["list_received"],
               ",".join(cyc["list_evidence"]) or "-",
               cyc["final_refresh_name"], cyc["time_to_clear_s"],
               cyc["time_to_list_s"], ",".join(cyc["notes"]) or "clean"))
    return {"name": name, "pass": cyc["ok"], "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_refresh_spam(h):
    """5 rapid pulls; the tail-check requires a fresh UI_LIST (strict), and
    every ANSWERED cycle (a received UI_LIST, measured against the most
    recent preceding pull) is budget-checked: WARN >5s, FAIL >20s, FAIL if
    ALL answers >10s (systemic). Also guards the stuck-auto_retry bug."""
    name = "refresh_spam"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()
    stuck_retry = False
    pull_times = []
    t0 = time.time()
    for i in range(5):
        h.link.send("pull")
        pull_times.append(time.time())
        # sample state between pulls too — the stuck-pill bug shows up as
        # auto_retry climbing while pill never clears
        st = h.link.liststate(timeout=0.8)
        if st and int(st.get("auto_retry", 0)) >= 3 and int(st.get("pill", 0)) == 1:
            stuck_retry = True
        if i < 4:
            time.sleep(max(0.0, 1.0 - 0.8))  # ~1s spacing incl. the liststate query
    cyc = h.poll_refresh_cycle(SPAM_SETTLE_TIMEOUT_S, require_pill_seen=False,
                               trigger_mark=mark, trigger_time=pull_times[0])
    stuck_retry = stuck_retry or cyc["stuck_retry"]
    timings["settle_s"] = round(time.time() - t0, 1)
    timings["time_to_clear_s"] = cyc["time_to_clear_s"]
    # per-cycle answer latencies: each received UI_LIST vs the most recent
    # pull sent before it (the device coalesces spam, so <=5 answers is fine)
    latencies = []
    for ts, ln in h.link.lines_since(mark):
        if line_is_ui_list_rx(ln):
            prior = [p for p in pull_times if p <= ts]
            base = prior[-1] if prior else pull_times[0]
            latencies.append(round(ts - base, 2))
    timings["cycle_latencies_s"] = latencies
    budget_ok, budget_notes = assess_cycle_budget(latencies)
    if getattr(h.args, "lenient", False):
        budget_ok = True  # budget failures are advisory in lenient mode
    for n in budget_notes:
        logln("  budget: %s" % n)
    ok = cyc["ok"] and not stuck_retry and budget_ok
    detail = ("final=%s list=%s stuck_retry=%s latencies=%s %s" %
              (cyc["final_refresh_name"], cyc["list_received"], stuck_retry,
               latencies, ",".join(cyc["notes"] + budget_notes) or "clean"))
    return {"name": name, "pass": ok, "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_double_refresh(h):
    """Two back-to-back refresh cycles with a latch assertion after each.

    Regression guard for the gesture-latch wedge: a fast refresh completion
    rebuilds the list (lv_obj_clean) mid-elastic-snap-back, LVGL 8.4 never
    delivers SCROLL_END to the emptied container, and the once-per-gesture
    latch (liststate "latch") stays consumed — every later touch pull is
    silently rejected. NOTE: the harness `pull` is the USB path, which
    bypasses the touch gesture (and the latch) entirely, so the second pull
    completing proves little by itself — the REAL regression guard here is
    the latch==0 assertion after each completed refresh cycle.

    Each cycle must receive a FRESH UI_LIST (strict criteria), and both
    answer times are budget-checked: WARN >5s (non-fatal), FAIL >20s, FAIL
    if BOTH cycles exceed 10s (systemic).
    """
    name = "double_refresh"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()

    def latch_after_cycle(which):
        """Returns (ok, detail_or_None). Reads liststate post-cycle and
        asserts the gesture latch re-armed (latch==0)."""
        st = h.link.liststate()
        if st is None:
            return False, "no_liststate_after_%s_refresh" % which
        if int(st.get("latch", 0)) == 1:
            return False, ("latch_wedged after %s refresh (liststate=%s)" %
                           (which, json.dumps(st)))
        return True, None

    # 1) first pull -> completion (with fresh UI_LIST) -> latch re-armed
    h.link.send("pull")
    t0 = time.time()
    cyc1 = h.poll_refresh_cycle(PULL_CLEAR_TIMEOUT_S,
                                trigger_mark=mark, trigger_time=t0)
    timings["first_time_to_clear_s"] = cyc1["time_to_clear_s"]
    timings["first_time_to_list_s"] = cyc1["time_to_list_s"]
    if not cyc1["ok"]:
        return {"name": name, "pass": False,
                "detail": "first_refresh_did_not_clear (%s)" %
                          (",".join(cyc1["notes"]) or cyc1["final_refresh_name"]),
                "timings": timings, "serial": h.link.interesting_since(mark)}
    ok, fail_detail = latch_after_cycle("first")
    if not ok:
        return {"name": name, "pass": False, "detail": fail_detail,
                "timings": timings, "serial": h.link.interesting_since(mark)}

    # 2) brief gap, then second pull -> completion -> latch re-armed again
    time.sleep(1.0)
    mark2 = h.link.mark()
    h.link.send("pull")
    t1 = time.time()
    cyc2 = h.poll_refresh_cycle(PULL_CLEAR_TIMEOUT_S,
                                trigger_mark=mark2, trigger_time=t1)
    timings["second_time_to_clear_s"] = cyc2["time_to_clear_s"]
    timings["second_time_to_list_s"] = cyc2["time_to_list_s"]
    timings["total_s"] = round(time.time() - t0, 1)
    if not cyc2["ok"]:
        return {"name": name, "pass": False,
                "detail": "second_refresh_did_not_clear (%s)" %
                          (",".join(cyc2["notes"]) or cyc2["final_refresh_name"]),
                "timings": timings, "serial": h.link.interesting_since(mark)}
    ok, fail_detail = latch_after_cycle("second")
    if not ok:
        return {"name": name, "pass": False, "detail": fail_detail,
                "timings": timings, "serial": h.link.interesting_since(mark)}

    # 3) per-cycle answer budget (time from pull to fresh UI_LIST)
    latencies = [t for t in (cyc1["time_to_list_s"], cyc2["time_to_list_s"])
                 if t is not None]
    budget_ok, budget_notes = assess_cycle_budget(latencies)
    if getattr(h.args, "lenient", False):
        budget_ok = True  # budget failures are advisory in lenient mode
    for n in budget_notes:
        logln("  budget: %s" % n)
    detail = ("both cycles clean, latch re-armed after each "
              "(ttc1=%s ttc2=%s ttl1=%s ttl2=%s final=%s %s)" %
              (cyc1["time_to_clear_s"], cyc2["time_to_clear_s"],
               cyc1["time_to_list_s"], cyc2["time_to_list_s"],
               cyc2["final_refresh_name"],
               ",".join(cyc1["notes"] + cyc2["notes"] + budget_notes) or "clean"))
    if not budget_ok:
        return {"name": name, "pass": False,
                "detail": "cycle_budget_exceeded (%s)" % detail,
                "timings": timings, "serial": h.link.interesting_since(mark)}
    return {"name": name, "pass": True, "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_encoder_overscroll_refresh(h):
    """3 CCW ticks at the top trigger a refresh which must deliver a fresh
    UI_LIST (strict criteria)."""
    name = "encoder_overscroll_refresh"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()
    # 3 CCW ticks at the top triggers refresh (SHOPPING_LIST_REFRESH_TICKS=3)
    trig_time = None
    for _ in range(3):
        h.link.send("scroll -1")
        trig_time = time.time()  # refresh fires on the 3rd tick
        time.sleep(0.3)
    t0 = time.time()
    cyc = h.poll_refresh_cycle(PULL_CLEAR_TIMEOUT_S,
                               trigger_mark=mark, trigger_time=trig_time)
    timings["time_to_clear_s"] = cyc["time_to_clear_s"]
    timings["time_to_list_s"] = cyc["time_to_list_s"]
    timings["total_s"] = round(time.time() - t0, 1)
    detail = ("pill_seen=%s list=%s(%s) final=%s ttc=%s %s" %
              (cyc["pill_seen"], cyc["list_received"],
               ",".join(cyc["list_evidence"]) or "-",
               cyc["final_refresh_name"], cyc["time_to_clear_s"],
               ",".join(cyc["notes"]) or "clean"))
    return {"name": name, "pass": cyc["ok"], "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_scroll_responsiveness(h):
    name = "scroll_responsiveness"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()
    before = h.link.liststate()
    if before is None:
        return {"name": name, "pass": False, "detail": "no_liststate_before",
                "timings": timings}
    sel_before = int(before.get("selected", -1))
    count = int(before.get("count", 0))
    if count <= 1:
        return {"name": name, "pass": True,
                "detail": "skipped_count_%d_nothing_to_scroll" % count,
                "timings": timings, "skipped": True}
    t0 = time.time()
    for _ in range(10):
        h.link.send("scroll 1")
        time.sleep(0.15)
    timings["scroll_burst_s"] = round(time.time() - t0, 2)
    after = h.link.liststate()
    if after is None:
        return {"name": name, "pass": False,
                "detail": "no_liststate_after_scroll (possible crash/hang)",
                "timings": timings, "serial": h.link.interesting_since(mark)}
    sel_after = int(after.get("selected", -1))
    # selection may clamp at count-1; "advanced" = moved forward at all
    ok = sel_after > sel_before
    return {"name": name, "pass": ok,
            "detail": "selected %d -> %d (count=%d)" % (sel_before, sel_after, count),
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_delete_item(h):
    """DATA-SAFE delete + restore — `del 0` removes one of the user's REAL
    backend shopping-list items, so this scenario:
      1. snapshots the backend list via API "view" (skips, non-destructively,
         if that fails — never delete what we cannot restore),
      2. sends `del 0` and confirms count dropped by exactly 1,
      3. parses the deleted item's id from `[USB] del 0 -> <uuid>`,
      4. looks the item up in the pre-delete snapshot and restores it via
         the API "add" operation (name/brand/barcode/store preserved; the
         backend assigns a NEW id),
      5. triggers one device refresh and asserts count returned to the
         pre-delete value.
    Any failure after step 2 that leaves the item unrestored is flagged
    loudly with DATA_LOSS in the detail."""
    name = "delete_item"
    timings = {}
    owner = h.args.owner_id
    api_url = h.args.list_api_url

    # 0) pre-delete backend snapshot — REQUIRED before any destructive step
    t0 = time.time()
    api_items = api_list_view(api_url, owner)
    timings["api_view_s"] = round(time.time() - t0, 1)
    if api_items is None:
        logln("  WARN delete_item skipped: pre-delete API view failed "
              "(refusing to delete without a restore snapshot)")
        return {"name": name, "pass": True, "skipped": True,
                "detail": "skipped_api_view_failed_no_restore_snapshot",
                "timings": timings}
    logln("  pre-delete backend snapshot: %d items" % len(api_items))

    h.wake_and_open()
    mark = h.link.mark()
    before = h.link.liststate()
    if before is None:
        return {"name": name, "pass": False, "detail": "no_liststate_before",
                "timings": timings}
    count_before = int(before.get("count", 0))
    if count_before <= 0:
        return {"name": name, "pass": True, "detail": "skipped_count_0",
                "timings": timings, "skipped": True}

    # 1) delete device row 0, capture the deleted item's id from serial
    #    (lcd_ui_task.h prints `[USB] del 0 -> <uuid>`; uuid == backend id)
    t0 = time.time()
    h.link.send("del 0")  # 0-based item index (verified in lcd_uart_task.h)
    del_line = h.link.wait_for_line(
        lambda l: "[USB] del 0 ->" in l, DEL_LINE_TIMEOUT_S, mark=mark)
    deleted_id = None
    if del_line and "out of range" not in del_line:
        deleted_id = del_line.split("->", 1)[1].strip()
        if deleted_id in ("(no id)", ""):
            deleted_id = None
    time.sleep(3.0)       # let the delete animation + state update complete
    after = h.link.liststate()
    timings["delete_s"] = round(time.time() - t0, 1)
    if after is None:
        return {"name": name, "pass": False, "detail": "no_liststate_after_del",
                "timings": timings, "serial": h.link.interesting_since(mark)}
    count_after = int(after.get("count", -1))
    drop_ok = count_after == count_before - 1

    # 2) restore the deleted item via the API add operation
    item = None
    if deleted_id is not None:
        for it in api_items:
            if str(it.get("id")) == deleted_id:
                item = it
                break
    restored = False
    if item is None:
        logln("  WARNING: cannot identify deleted item (id=%s) in the "
              "pre-delete snapshot — item NOT restored!" % deleted_id)
    elif not item.get("product_name"):
        logln("  WARNING: deleted item id=%s has no product_name — "
              "NOT restored!" % deleted_id)
    else:
        t0 = time.time()
        restored = api_list_add(api_url, owner, item)
        timings["api_restore_s"] = round(time.time() - t0, 1)

    # 3) one device refresh; count must return to the pre-delete value
    count_final = -1
    cyc = None
    if restored:
        rmark = h.link.mark()
        h.link.send("pull")
        rt = time.time()
        cyc = h.poll_refresh_cycle(PULL_CLEAR_TIMEOUT_S,
                                   trigger_mark=rmark, trigger_time=rt)
        timings["restore_refresh_s"] = round(time.time() - rt, 1)
        final = h.link.liststate()
        if final is not None:
            count_final = int(final.get("count", -1))

    ok = drop_ok and restored and (count_final == count_before)
    detail = ("count %d -> %d -> %d id=%s restored=%s refresh_list=%s" %
              (count_before, count_after, count_final, deleted_id, restored,
               cyc["list_received"] if cyc else None))
    if not restored:
        if drop_ok or deleted_id is not None:
            detail += " DATA_LOSS_ITEM_NOT_RESTORED"
        else:
            detail += " no_confirmed_delete_nothing_to_restore"
    return {"name": name, "pass": ok, "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_sleep_wake_cache(h):
    name = "sleep_wake_cache"
    timings = {}
    expected = h.ctx.get("last_known_count")  # may be None
    t0 = time.time()
    slept = h.let_sleep(SLEEP_VANISH_TIMEOUT_LONG_S)
    timings["sleep_wait_s"] = round(time.time() - t0, 1)
    if not slept:
        return {"name": name, "pass": False,
                "detail": "device_did_not_sleep_within_%ds" % int(SLEEP_VANISH_TIMEOUT_LONG_S),
                "timings": timings}
    t0 = time.time()
    h.wake_and_open()
    timings["wake_open_s"] = round(time.time() - t0, 1)
    mark = h.link.mark()
    h.enter_list()
    st = None
    t0 = time.time()
    while time.time() - t0 < COLD_CACHE_WINDOW_S:
        st = h.link.liststate(timeout=0.8)
        if st and int(st.get("count", 0)) > 0:
            break
        time.sleep(0.1)
    timings["entry_to_count_s"] = round(time.time() - t0, 2)
    count = int(st.get("count", 0)) if st else 0
    ok = count > 0
    detail = "count_after_wake=%d" % count
    if expected is not None:
        detail += " (pre-sleep=%s)" % expected
        # informational only: count may legitimately differ if scenario 8
        # deleted an item and the backend re-synced during revalidate
    return {"name": name, "pass": ok, "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


SCENARIOS = [
    ("cold_entry_cache", scen_cold_entry_cache, True),    # (name, fn, needs_sleep)
    ("entry_revalidate", scen_entry_revalidate, False),
    ("pull_refresh", scen_pull_refresh, False),
    ("refresh_spam", scen_refresh_spam, False),
    ("double_refresh", scen_double_refresh, False),
    ("encoder_overscroll_refresh", scen_encoder_overscroll_refresh, False),
    ("scroll_responsiveness", scen_scroll_responsiveness, False),
    ("delete_item", scen_delete_item, False),
    ("sleep_wake_cache", scen_sleep_wake_cache, True),
]


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def run_scenario(h, name, fn):
    logln("--- SCENARIO %s ---" % name)
    t0 = time.time()
    try:
        res = fn(h)
    except PortDropped as e:
        res = {"name": name, "pass": False, "detail": "port_dropped",
               "timings": {}, "error": str(e)}
        h.recover_after_drop()
    except Exception as e:
        res = {"name": name, "pass": False, "detail": "exception:%s" % e,
               "timings": {}}
    res.setdefault("timings", {})
    res["timings"]["scenario_total_s"] = round(time.time() - t0, 1)
    res["kind"] = "scenario"

    # track last known count for sleep_wake_cache's informational compare
    try:
        if h.link.is_open:
            st = h.link.liststate(timeout=1.0)
            if st is not None:
                h.ctx["last_known_count"] = int(st.get("count", 0))
    except PortDropped:
        h.recover_after_drop()

    logln("RESULT %-28s %s  (%s)" %
          (name, "PASS" if res["pass"] else "FAIL", res.get("detail", "")))
    record(res)
    return res


def main():
    global JSONL_PATH, LCD_GLOB

    p = argparse.ArgumentParser(description="HALO shopping-list e2e test harness")
    p.add_argument("--only", help="run a single scenario by name")
    p.add_argument("--keep-awake", action="store_true",
                   help="skip sleep-dependent scenarios (cold_entry_cache, sleep_wake_cache)")
    p.add_argument("--lenient", action="store_true",
                   help="restore OLD refresh criteria (pill-clear only, no "
                        "fresh-UI_LIST requirement, no cycle-budget failures) "
                        "— debugging only")
    p.add_argument("--owner-id", default=OWNER_ID_DEFAULT,
                   help="backend ownerId for delete_item's view/add calls")
    p.add_argument("--list-api-url", default=LIST_API_URL_DEFAULT,
                   help="backend list API endpoint")
    p.add_argument("--jsonl", default=JSONL_DEFAULT, help="JSONL output path")
    p.add_argument("--lcd-port", default=LCD_GLOB, help="LCD USB port glob")
    p.add_argument("--tap-port", default=ACT_PORT, help="actuator serial port")
    p.add_argument("--tap-command", default=TAP_COMMAND, help="actuator command (TAP / PUSH:800)")
    p.add_argument("--verbose", action="store_true", help="echo all serial RX lines")
    p.add_argument("--list", action="store_true", help="list scenario names and exit")
    args = p.parse_args()

    if args.list:
        for name, _, needs_sleep in SCENARIOS:
            print("%-28s%s" % (name, "  (sleep-dependent)" if needs_sleep else ""))
        return 0

    JSONL_PATH = args.jsonl
    LCD_GLOB = args.lcd_port

    valid = {n for n, _, _ in SCENARIOS}
    if args.only and args.only not in valid:
        print("Unknown scenario %r. Valid: %s" % (args.only, ", ".join(sorted(valid))))
        return 2

    selected = []
    for name, fn, needs_sleep in SCENARIOS:
        if args.only and name != args.only:
            continue
        if args.keep_awake and needs_sleep and not args.only:
            logln("SKIP %s (--keep-awake)" % name)
            record({"kind": "scenario", "name": name, "pass": None,
                    "detail": "skipped_keep_awake", "timings": {}})
            continue
        selected.append((name, fn))

    logln("=== LIST E2E TEST ===")
    logln("lcd=%s tap=%s jsonl=%s refresh_criteria=%s scenarios=%s" %
          (LCD_GLOB, args.tap_port, JSONL_PATH,
           "LENIENT" if args.lenient else "STRICT",
           [n for n, _ in selected]))
    record({"kind": "plan", "scenarios": [n for n, _ in selected],
            "keep_awake": args.keep_awake, "only": args.only,
            "lenient": args.lenient, "owner_id": args.owner_id})

    h = Harness(args)
    results = []
    try:
        version_check(h)
        # entry_revalidate normally rides cold_entry_cache's entry; if it runs
        # standalone it self-arms (see scen_entry_revalidate).
        for name, fn in selected:
            results.append(run_scenario(h, name, fn))
    finally:
        h.link.close()

    n_pass = sum(1 for r in results if r["pass"])
    n_fail = len(results) - n_pass
    logln("=== DONE: %d pass / %d fail ===" % (n_pass, n_fail))
    for r in results:
        logln("  %-28s %s  %s" %
              (r["name"], "PASS" if r["pass"] else "FAIL", r.get("detail", "")))
    record({"kind": "summary", "pass": n_fail == 0, "n_pass": n_pass,
            "n_fail": n_fail,
            "results": [{"name": r["name"], "pass": r["pass"],
                         "detail": r.get("detail")} for r in results]})
    return 0 if (results and n_fail == 0) else 1


if __name__ == "__main__":
    sys.exit(main())

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

SCENARIOS (run all by default; --only NAME for one)
  1. cold_entry_cache        Let device deep-sleep, tap-wake, `list`; PASS if
                             liststate count>0 within 2s of entry (NVS cache
                             rendered across deep sleep / boot-load).
  2. entry_revalidate        Entry auto-triggers a background refresh; PASS if
                             pill 1->0 and refresh_state ends IDLE/COMPLETE
                             with count>0 within 20s.
  3. pull_refresh            `pull` (touch pull-to-refresh); PASS if pill shows
                             then clears within 15s and refresh_state ends
                             IDLE/COMPLETE. Records time-to-clear.
  4. refresh_spam            `pull` x5, 1s apart; PASS if final pill=0 +
                             refresh_state IDLE/COMPLETE within 25s AND no
                             snapshot ever shows auto_retry>=3 with pill stuck
                             (regression test for the stuck-"Refreshing" bug).
  5. encoder_overscroll_refresh  `scroll -1` x3 @300ms (3 CCW ticks at top =
                             refresh trigger, SHOPPING_LIST_REFRESH_TICKS=3);
                             PASS like scenario 3.
  6. scroll_responsiveness   `scroll 1` x10 @150ms; PASS if selected index
                             advanced and the port stayed alive (liveness
                             only — render timing isn't measurable over USB).
  7. delete_item             `del 0` when count>0; PASS if count decreased by
                             exactly 1 after 3s (exercises delete + animation
                             path without asserting visuals).
  8. sleep_wake_cache        `home`, wait for port to vanish (deep sleep),
                             tap-wake, reopen, `list`+`liststate`; PASS if
                             count survives (validates persist-on-update,
                             not just boot-load).

FLAGS
  --only NAME          Run a single scenario.
  --keep-awake         Skip the sleep-dependent scenarios (1, 8).
  --jsonl PATH         Machine log (default /tmp/list_e2e.jsonl), one record
                       per scenario + a final summary record.
  --lcd-port / --tap-port / --tap-command   Hardware overrides.
  --verbose            Echo every serial RX line (default: only interesting
                       [SHOPPING_LIST]/[LISTSTATE]/[USB]/UI_LIST/[LIST lines).

Exit code 0 only if every executed scenario passed.

ASSUMPTIONS TO VERIFY ONCE THE FIRMWARE LANDS (search "ASSUMPTION" below):
  - refresh_state enum order — VERIFIED against LCD_Minimal.ino:787-791
    (REFRESH_IDLE=0, WAKE_PENDING=1, INFLIGHT=2, COMPLETE=3, FAILED=4).
  - `del N` is 0-based (spec says `del 0` deletes the first item).
  - `scroll 1` (positive / CW) advances `selected` downward; `scroll -1`
    (CCW) at the top contributes to the overscroll-refresh trigger.
  - `[LISTSTATE]` is printed as exactly one line of JSON after the tag.
  - The `screen` field's enum mapping is unknown, so it is logged but never
    asserted on.
  - Scenarios 2/3/5 soft-pass if the pill=1 phase was too fast to observe
    but refresh_state was seen/ended COMPLETE (detail notes it).
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import threading
import time

import serial

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(SCRIPT_DIR)

LCD_GLOB = "/dev/cu.usbmodem101"
ACT_PORT = "/dev/cu.usbmodem21201"
TAP_SCRIPT = os.path.join(REPO, "tap_implementation", "tap.py")
TAP_COMMAND = "TAP"
BAUD = 115200
JSONL_DEFAULT = "/tmp/list_e2e.jsonl"

# Serial RX prefixes worth echoing to the console / attaching to results.
INTERESTING_PREFIXES = ("[SHOPPING_LIST]", "[LISTSTATE]", "[USB]", "UI_LIST", "[LIST")

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
SLEEP_VANISH_TIMEOUT_S = 50.0   # scenario 1: wait for port to vanish (home-screen idle->sleep takes ~32s)
SLEEP_VANISH_TIMEOUT_LONG_S = 40.0  # scenario 8
COLD_CACHE_WINDOW_S = 2.0       # scenario 1: count>0 within 2s of `list`
REVALIDATE_TIMEOUT_S = 20.0     # scenario 2
PULL_CLEAR_TIMEOUT_S = 15.0     # scenarios 3, 5
SPAM_SETTLE_TIMEOUT_S = 25.0    # scenario 4
LISTSTATE_POLL_S = 0.5
LISTSTATE_REPLY_TIMEOUT_S = 2.0


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
            if self.verbose or line.startswith(INTERESTING_PREFIXES):
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
               if ln.startswith(INTERESTING_PREFIXES)]
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
        cache_age_s, auto_retry, selected) or None on timeout/parse failure.
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
        """Send `list`, record entry time in ctx."""
        self.link.send("list")
        self.ctx["list_entered_at"] = time.time()

    def poll_refresh_cycle(self, timeout, require_count=False,
                           require_pill_seen=True, poll_s=LISTSTATE_POLL_S):
        """Poll `liststate` until pill==0 and refresh_state in {IDLE,COMPLETE}
        (or timeout). Returns a dict describing the observed cycle.

        soft-pass: if the pill=1 phase was never observed but the cycle ended
        COMPLETE (or a non-IDLE refresh_state was seen), the refresh clearly
        ran but was too fast to sample — counted as a pass with a note.
        """
        t0 = time.time()
        pill_seen = False
        busy_state_seen = False   # WAKE_PENDING/INFLIGHT observed
        complete_seen = False
        stuck_retry = False       # any snapshot with auto_retry>=3 while pill=1
        snapshots = 0
        final = None
        time_to_clear = None
        while time.time() - t0 < timeout:
            st = self.link.liststate()
            if st is not None:
                snapshots += 1
                final = st
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
                cleared = (pill == 0 and rs in REFRESH_OK_FINAL)
                if cleared and (pill_seen or busy_state_seen or complete_seen):
                    time_to_clear = round(time.time() - t0, 2)
                    break
            time.sleep(poll_s)
        ok = final is not None
        notes = []
        if ok:
            rs = int(final.get("refresh_state", -1))
            pill = int(final.get("pill", 1))
            ok = (pill == 0 and rs in REFRESH_OK_FINAL)
            if ok and require_pill_seen and not pill_seen:
                if busy_state_seen or complete_seen:
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
            "pill_seen": pill_seen,
            "busy_state_seen": busy_state_seen,
            "complete_seen": complete_seen,
            "stuck_retry": stuck_retry,
            "snapshots": snapshots,
            "time_to_clear_s": time_to_clear,
            "final": final,
            "final_refresh_name": REFRESH_NAMES.get(
                int(final.get("refresh_state", -1)), "?") if final else None,
            "notes": notes,
        }


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
    t0 = time.time()
    cyc = h.poll_refresh_cycle(REVALIDATE_TIMEOUT_S, require_count=True)
    timings["revalidate_s"] = round(time.time() - t0, 1)
    timings["time_to_clear_s"] = cyc["time_to_clear_s"]
    h.ctx["entry_revalidate_armed"] = False
    detail = ("pill_seen=%s final=%s %s" %
              (cyc["pill_seen"], cyc["final_refresh_name"],
               ",".join(cyc["notes"]) or "clean"))
    return {"name": name, "pass": cyc["ok"], "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_pull_refresh(h):
    name = "pull_refresh"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()
    h.link.send("pull")
    t0 = time.time()
    cyc = h.poll_refresh_cycle(PULL_CLEAR_TIMEOUT_S)
    timings["time_to_clear_s"] = cyc["time_to_clear_s"]
    timings["total_s"] = round(time.time() - t0, 1)
    detail = ("pill_seen=%s final=%s ttc=%s %s" %
              (cyc["pill_seen"], cyc["final_refresh_name"],
               cyc["time_to_clear_s"], ",".join(cyc["notes"]) or "clean"))
    return {"name": name, "pass": cyc["ok"], "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_refresh_spam(h):
    name = "refresh_spam"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()
    stuck_retry = False
    t0 = time.time()
    for i in range(5):
        h.link.send("pull")
        # sample state between pulls too — the stuck-pill bug shows up as
        # auto_retry climbing while pill never clears
        st = h.link.liststate(timeout=0.8)
        if st and int(st.get("auto_retry", 0)) >= 3 and int(st.get("pill", 0)) == 1:
            stuck_retry = True
        if i < 4:
            time.sleep(max(0.0, 1.0 - 0.8))  # ~1s spacing incl. the liststate query
    cyc = h.poll_refresh_cycle(SPAM_SETTLE_TIMEOUT_S, require_pill_seen=False)
    stuck_retry = stuck_retry or cyc["stuck_retry"]
    timings["settle_s"] = round(time.time() - t0, 1)
    timings["time_to_clear_s"] = cyc["time_to_clear_s"]
    ok = cyc["ok"] and not stuck_retry
    detail = ("final=%s stuck_retry=%s %s" %
              (cyc["final_refresh_name"], stuck_retry,
               ",".join(cyc["notes"]) or "clean"))
    return {"name": name, "pass": ok, "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


def scen_encoder_overscroll_refresh(h):
    name = "encoder_overscroll_refresh"
    timings = {}
    h.wake_and_open()
    mark = h.link.mark()
    # 3 CCW ticks at the top triggers refresh (SHOPPING_LIST_REFRESH_TICKS=3)
    for _ in range(3):
        h.link.send("scroll -1")
        time.sleep(0.3)
    t0 = time.time()
    cyc = h.poll_refresh_cycle(PULL_CLEAR_TIMEOUT_S)
    timings["time_to_clear_s"] = cyc["time_to_clear_s"]
    timings["total_s"] = round(time.time() - t0, 1)
    detail = ("pill_seen=%s final=%s ttc=%s %s" %
              (cyc["pill_seen"], cyc["final_refresh_name"],
               cyc["time_to_clear_s"], ",".join(cyc["notes"]) or "clean"))
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
    name = "delete_item"
    timings = {}
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
    t0 = time.time()
    h.link.send("del 0")  # ASSUMPTION: 0-based item index
    time.sleep(3.0)       # let the delete animation + state update complete
    after = h.link.liststate()
    timings["delete_s"] = round(time.time() - t0, 1)
    if after is None:
        return {"name": name, "pass": False, "detail": "no_liststate_after_del",
                "timings": timings, "serial": h.link.interesting_since(mark)}
    count_after = int(after.get("count", -1))
    ok = count_after == count_before - 1
    return {"name": name, "pass": ok,
            "detail": "count %d -> %d" % (count_before, count_after),
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
        # informational only: count may legitimately differ if scenario 7
        # deleted an item and the backend re-synced during revalidate
    return {"name": name, "pass": ok, "detail": detail,
            "timings": timings, "serial": h.link.interesting_since(mark)}


SCENARIOS = [
    ("cold_entry_cache", scen_cold_entry_cache, True),    # (name, fn, needs_sleep)
    ("entry_revalidate", scen_entry_revalidate, False),
    ("pull_refresh", scen_pull_refresh, False),
    ("refresh_spam", scen_refresh_spam, False),
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
    logln("lcd=%s tap=%s jsonl=%s scenarios=%s" %
          (LCD_GLOB, args.tap_port, JSONL_PATH, [n for n, _ in selected]))
    record({"kind": "plan", "scenarios": [n for n, _ in selected],
            "keep_awake": args.keep_awake, "only": args.only})

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

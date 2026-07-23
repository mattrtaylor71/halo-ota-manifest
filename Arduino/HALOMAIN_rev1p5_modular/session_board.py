#!/usr/bin/env python3
"""
session_board.py — Mac-side daemon for the Halo LCD "Claude Session Board".

Every N seconds it enumerates active Claude Code sessions on this Mac, works out
each one's status (working / done / idle), extracts the latest assistant reply
text, and emits a newline-delimited-JSON snapshot either to stdout (--once) or to
an ESP32 over serial (default).

Python 3 stdlib only, plus pyserial for the serial daemon mode (imported lazily so
--once / --fake work without it).
"""

import argparse
import glob
import hashlib
import json
import os
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import unicodedata
import wave

HOME = os.path.expanduser("~")
SESSIONS_DIR = os.path.join(HOME, ".claude", "sessions")
LUXAFOR_DIR = os.path.join(HOME, ".claude", "luxafor-state")
PROJECTS_DIR = os.path.join(HOME, ".claude", "projects")
SCRATCH_ROOT = "/private/tmp/claude-501"   # per-session task-output scratch area
PAUSE_FILE = "/tmp/session_board.pause"
CMD_FILE = "/tmp/session_board_cmd.jsonl"   # test cmds to forward to the port
DEFAULT_TCP_PORT = 8383            # TCP snapshot/protocol server (standalone-over-WiFi)
BEACON_PORT = 8384                 # UDP discovery beacon
BEACON_MAGIC = "SBHALO1"           # beacon payload: "SBHALO1 <tcp_port>"
BEACON_INTERVAL = 3.0
# Configured Wi-Fi creds the board should join (env overrides the default).
# Blank either one to fall back to the current SSID / Keychain lookup.
WIFI_SSID = os.environ.get("SESSION_BOARD_WIFI_SSID", "Garage Member")
WIFI_PWD = os.environ.get("SESSION_BOARD_WIFI_PWD", "build00!")
SUMMARY_CACHE_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), ".session_board_summaries.json")
SECRET_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), ".session_board_secret")
NAMES_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), ".session_board_names.json")
POSITIONS_FILE = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), ".session_board_positions.json")
CUSTOM_NAME_MAX = 30               # Matt-assigned custom name cap
AUTH_TIMEOUT = 15.0                # a new TCP link must auth within this or be dropped
                                   # (tunnel-tolerant: a real internet round-trip via
                                   #  ngrok needs more than a LAN-tuned 5s)
NGROK_BIN = os.environ.get("SESSION_BOARD_NGROK_BIN", "ngrok")
NGROK_API = "http://127.0.0.1:4040/api/tunnels"
# Nebula.gg second fleet view (read-only). The CLI reuses the persisted ~/.nebula
# session non-interactively; we poll ONLY while the Nebula view is active (on-demand,
# never in the background) and no faster than NEBULA_POLL_SECS.
NEBULA_CLI = os.environ.get("SESSION_BOARD_NEBULA_CLI", "nebula-ai")  # run via npx
NEBULA_POLL_SECS = 8.0
NEBULA_TIMEOUT = 20
# Pin the workspace EXPLICITLY on every call (global --workspace flag "overrides
# stored") so we never inherit the CLI's ambient/drifting active-workspace state -
# that drift once silently pointed `channels list` at an empty workspace (looked
# like "0 agents" but was really "wrong workspace"). slug "matt-taylor" = Matt's
# Personal ws (id ws_0xbd309ffb...); override via env if the account/ws changes.
NEBULA_WORKSPACE = os.environ.get("SESSION_BOARD_NEBULA_WORKSPACE", "matt-taylor")
# Nebula write-actions (v2): voice -> `chat` into a channel, and new-channel create.
# `chat` is SYNCHRONOUS (waits for the agent's full reply even with --no-stream), so
# these ALWAYS run fire-and-forget on a bg thread the daemon never waits on - the
# reply surfaces naturally in the next channels-list poll. Default agent for new
# channels = "Nebula" (the workspace's default user agent).
NEBULA_DEFAULT_AGENT = os.environ.get("SESSION_BOARD_NEBULA_AGENT",
                                      "agt_06978f6b9b717fba80006458ccc6df20")
NEBULA_CHAT_TIMEOUT = 300   # bg-thread zombie-safety cap (a reply can be slow)
# Capture heartbeat cadence (s). Default 5s is fine for normal operation; set
# SESSION_BOARD_CAPTURE_HB=1 for a diagnostic upload run so the daemon's per-tick
# receive view (bytes-since-last, inst KB/s, gap-since-last-growth) can be lined up
# against the board's per-second EAGAIN/RSSI/TX-error log.
try:
    CAPTURE_HB_SECS = max(0.2, float(os.environ.get("SESSION_BOARD_CAPTURE_HB", "5")))
except ValueError:
    CAPTURE_HB_SECS = 5.0

MAX_SESSIONS = 12
STALE_UPDATED_SECS = 24 * 3600      # skip sessions not active in 24h
DORMANT_SECS = 2 * 3600             # updatedAt AND transcript stale this long => dormant
VOICE_CONSUME_TIMEOUT = 12          # poll the target inbox this long for consumption
VOICE_CONSUME_INTERVAL = 1.0
DONE_WINDOW_SECS = 30 * 60          # 'd' (delivered, awaiting user) window; older => 'i'
WORKING_MTIME_SECS = 45            # transcript written this recently => 'working'
BUSY_MTIME_SECS = 30 * 60          # registry busy trusted only if transcript < this cold
BG_AGENT_SECS = 60                 # bg task-output written this recently => 'working'
DELEGATE_TASK_SECS = 90            # fresh bg task output overrides an end_turn => 'w'
AGENT_CPU_DELTA = 0.15             # CPU-secs/cycle a teammate agent burns to count active
AGENT_GRACE_SAMPLES = 2            # a newly-seen agent pid is active for its first N samples
AGENT_LOW_SAMPLES = 2              # consecutive low-delta samples required to go inactive
TAIL_CHUNK = 64 * 1024              # backward read chunk size
TAIL_CEILING = 1024 * 1024          # never read more than 1MB from the tail
MSG_MAX = 500
NAME_MAX = 26
PROJ_MAX = 22

# --- LLM summarization ---
LLM_MODEL = "claude-haiku-4-5-20251001"
LLM_BIN = os.environ.get("SESSION_BOARD_CLAUDE_BIN", "claude")
# The summarizer's `claude -p` calls run from this dedicated marker dir so the
# board can exclude its own in-flight worker processes (which self-register in
# ~/.claude/sessions for ~3s and would otherwise show as blank ghost cards).
LLM_WORKER_DIR = "/tmp/sb_llm_worker"
_LLM_WORKER_PREFIXES = (LLM_WORKER_DIR, os.path.realpath(LLM_WORKER_DIR))
_TMP_GHOST_NAME_RE = re.compile(r"^tmp-[0-9a-f]{2}$")
LLM_TIMEOUT = 30                   # seconds per claude -p call
LLM_MAX_CONCURRENT = 2
LLM_TEXT_CAP = 3500                # assistant text chars fed to the model
LLM_USER_CAP = 500                 # user text chars fed to the model
SUMMARY_CACHE_MAX = 200            # LRU cap on persisted summaries
SUMMARY_FAIL_COOLDOWN = 120        # don't retry a failed key for this long
SUMMARY_CACHE_VERSION = 7          # bump to bust the persisted cache on meaning changes
TITLE_MAX = 24
STATUS_MAX = 260
DETAIL_MAX = 700                   # richer detail-view text
STATUS_RANK = {"d": 0, "w": 1, "i": 2}   # board order: done, working, idle
# The detail field ends with one classified closer (the agent's actual point).
CLOSER_PREFIXES = ("**Asking you:**", "**Answer:**", "**Bottom line:**")

# --- Voice capture / transcription / delivery ---
VOICE_DIR = "/tmp/session_board_voice"
VOICE_CAP_SECS = 200               # hard cap on a single capture (firmware ceiling ~180s)
VOICE_STALL_SECS = 8               # no PCM growth this long => abandon (bounds frozen-screen)
VOICE_MIN_SECS = 0.5               # shorter than this = "empty audio"
VOICE_KEEP = 20                    # keep the last N .wav recordings
VOICE_RATE = 44100                 # int16 mono PCM, as streamed by firmware
VOICE_WIDTH = 2
VOICE_CHANNELS = 1
VOICE_NORM_MAX_GAIN = 8.0          # cap on peak-normalization gain. Beyond ~8x a
                                   # "quiet" clip is almost always low-SNR (noise, not
                                   # a quiet talker); over-amplifying it makes whisper
                                   # hallucinate repeated tokens ("okay okay okay").
                                   # Capping still boosts a genuinely quiet talker while
                                   # leaving a noise clip quiet enough that whisper
                                   # honestly returns "no speech".
WAV_BEGIN_MARK = b"-- WAV_BEGIN --"
WAV_END_MARK = b"-- WAV_END --"
NEW_SESSION_SENTINEL = "NEWSESS0"  # rec-start id => spawn a fresh claude session
WHISPER_PY = os.path.expanduser("~/.whisper-venv/bin/python")
WHISPER_TIMEOUT = 60
# Default mlx-whisper was 'whisper-tiny' — measurably poor on short, noisy rename-
# length utterances (word confusion + repetition-loop hallucinations). base.en is
# already cached locally and, paired with the decode params below, fails gracefully
# (near-misses, not garbage) on degraded audio. Override with SESSION_BOARD_WHISPER_MODEL
# (e.g. a small.en repo) if real-world clips need more headroom.
WHISPER_MODEL = os.environ.get("SESSION_BOARD_WHISPER_MODEL",
                               "mlx-community/whisper-base.en-mlx")
# Rename is a constrained use case (a short label, not a sentence) — a vocabulary/style
# hint biases whisper toward a terse name and away from wandering into a garbled phrase.
WHISPER_RENAME_PROMPT = "A short agent name or label."
TEAMS_DIR = os.path.join(HOME, ".claude", "teams")

# ---------------------------------------------------------------------------
# Text cleaning
# ---------------------------------------------------------------------------

_WS_RE = re.compile(r"\s+")
_TRANSLATE = {
    ord("‘"): "'", ord("’"): "'",   # curly single quotes
    ord("“"): '"', ord("”"): '"',   # curly double quotes
    ord("–"): "-", ord("—"): "-",   # en / em dash
    ord("…"): "...",                       # ellipsis
}


def clean(text, limit=MSG_MAX):
    """Collapse whitespace, ASCII-fold, and truncate for the device fonts."""
    if not text:
        return ""
    s = text.translate(_TRANSLATE)
    s = _WS_RE.sub(" ", s).strip()
    s = unicodedata.normalize("NFKD", s)
    s = s.encode("ascii", "ignore").decode("ascii")
    if len(s) > limit:
        s = s[: limit - 3].rstrip() + "..."
    return s


def strip_bold(s):
    """Remove all ** markers (title/status never carry bold)."""
    return s.replace("**", "") if s else s


def balance_bold(s):
    """Keep ** bold markers only if balanced; an odd count -> strip them all."""
    if s and s.count("**") % 2:
        return s.replace("**", "")
    return s


def _split_closer(text):
    """Split detail into (overview, closer). The closer is the trailing sentence
    beginning at the last '**Asking you:**' / '**Answer:**' / '**Bottom line:**'
    marker; ('' closer) if none present."""
    ci = max((text.rfind(p) for p in CLOSER_PREFIXES), default=-1)
    if ci < 0:
        return text, ""
    return text[:ci].rstrip(), text[ci:].strip()


def _safe_trim(s, budget):
    """Trim s to <= budget chars without leaving an unbalanced ** span."""
    if len(s) <= budget:
        return s
    t = s[:budget]
    if t.count("**") % 2:            # cut landed inside a bold span -> back off
        t = t[:t.rfind("**")]
    return t.rstrip()


def fit_detail(text, limit=DETAIL_MAX):
    """Fit detail into `limit` chars while PRESERVING the closer (the agent's
    point is the highest-value text): trim the overview, never the closer."""
    if len(text) <= limit:
        return text
    overview, closer = _split_closer(text)
    if not closer:                                  # no closer -> plain tail trim
        return _safe_trim(text, limit - 3) + "..."
    budget = limit - len(closer) - 5                # room for " ... " joiner
    if budget <= 0:                                 # closer alone fills the budget
        return closer[:limit]
    ov = _safe_trim(overview, budget)
    return (ov + " ... " + closer) if ov else closer


# ---------------------------------------------------------------------------
# Transcript tail-reading (latest assistant text)
# ---------------------------------------------------------------------------

# sessionId -> (st_size, st_mtime, text)
_transcript_cache = {}
# sessionId -> resolved transcript path (avoids re-globbing every cycle)
_transcript_path_cache = {}


def _resolve_transcript_path(session_id, cwd):
    """Resolve (and cache) a session's transcript .jsonl path, or None."""
    p = _transcript_path_cache.get(session_id)
    if p and os.path.isfile(p):
        return p
    p = _find_transcript(session_id, cwd)
    if p:
        _transcript_path_cache[session_id] = p
    return p


def transcript_mtime(session_id, cwd):
    """st_mtime (epoch secs) of the session's transcript, or None if absent."""
    p = _resolve_transcript_path(session_id, cwd)
    if not p:
        return None
    try:
        return os.stat(p).st_mtime
    except OSError:
        return None


def _sanitize_cwd(cwd):
    """cwd -> project dir name: every non-alphanumeric char becomes '-'."""
    return re.sub(r"[^A-Za-z0-9]", "-", cwd)


def _find_transcript(session_id, cwd):
    """Locate the transcript .jsonl for a session, or None."""
    if cwd:
        cand = os.path.join(PROJECTS_DIR, _sanitize_cwd(cwd), session_id + ".jsonl")
        if os.path.isfile(cand):
            return cand
    # fallback: glob every project dir
    hits = glob.glob(os.path.join(PROJECTS_DIR, "*", session_id + ".jsonl"))
    return hits[0] if hits else None


def _msg_text(obj, want_type):
    """Extract joined text blocks from a transcript line of the given type."""
    if obj.get("isSidechain") or obj.get("type") != want_type:
        return None
    if want_type == "user" and obj.get("toolUseResult") is not None:
        return None  # tool-result echo, not a real user prompt
    content = (obj.get("message") or {}).get("content")
    if isinstance(content, str):
        return content.strip() or None
    if isinstance(content, list):
        texts = [
            c["text"]
            for c in content
            if isinstance(c, dict) and c.get("type") == "text" and c.get("text")
        ]
        if texts:
            return "\n".join(texts)
    return None


def _classify_entry(obj):
    """Classify one transcript line for the status verdict. Returns:
      ("done", ts) - assistant finished its turn (end_turn / natural stop)
      ("w",    ts) - mid-turn (assistant tool_use, or a user/tool_result entry)
      ("skip", None) - meta/system/streaming chunk: keep scanning backward
    Only 'assistant' and 'user' entries are meaningful; every other type
    (system, mode, permission-mode, ai-title, last-prompt, queue-operation,
    bridge-session, attachment, agent-name, ...) and sidechains are skipped."""
    if obj.get("isSidechain"):
        return "skip", None
    t = obj.get("type")
    if t not in ("assistant", "user"):
        return "skip", None
    msg = obj.get("message")
    if not isinstance(msg, dict):
        return "skip", None
    ts = _to_epoch_seconds(obj.get("timestamp"))
    if t == "user":
        if obj.get("isMeta"):
            return "skip", None
        if not msg.get("content"):
            return "skip", None
        # A user entry newer than any assistant end_turn means the agent owes a
        # response (tool_result mid-turn, or a fresh user message) -> working.
        return "w", ts
    # assistant
    sr = msg.get("stop_reason")
    content = msg.get("content") or []
    has_tool_use = isinstance(content, list) and any(
        isinstance(c, dict) and c.get("type") == "tool_use" for c in content)
    if sr == "end_turn":
        return "done", ts
    if sr == "tool_use":
        return "w", ts
    if sr in ("max_tokens", "stop_sequence"):
        return "done", ts
    if sr is None:
        # Intermediate streaming chunk. A tool_use block is definitive; a pure
        # thinking/text chunk with no stop_reason is not -> keep scanning back.
        if has_tool_use:
            return "w", ts
        return "skip", None
    return "skip", None


def _analyze_tail(path):
    """One backward pass -> (assistant_text, user_text, verdict).
    verdict is ("w"|"done", ts_epoch_or_None) from the newest meaningful entry,
    or None if nothing classifiable was found in the scanned tail."""
    try:
        size = os.path.getsize(path)
    except OSError:
        return "", "", None
    a_text = None
    u_text = None
    verdict = None
    remainder = b""            # bytes carried over from the front of a chunk
    read_total = 0
    with open(path, "rb") as f:
        pos = size
        while pos > 0 and read_total < TAIL_CEILING:
            step = min(TAIL_CHUNK, pos)
            pos -= step
            read_total += step
            f.seek(pos)
            chunk = f.read(step) + remainder
            parts = chunk.split(b"\n")
            # If there's still data before pos, the first element is a partial
            # line whose head lives in the not-yet-read region; hold it back.
            if pos > 0:
                remainder = parts[0]
                parts = parts[1:]
            else:
                remainder = b""
            for raw in reversed(parts):
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    obj = json.loads(raw)
                except (ValueError, TypeError):
                    continue
                if verdict is None:
                    cls, ts = _classify_entry(obj)
                    if cls != "skip":
                        verdict = (cls, ts)
                if a_text is None:
                    t = _msg_text(obj, "assistant")
                    if t:
                        a_text = t
                if u_text is None:
                    t = _msg_text(obj, "user")
                    if t:
                        u_text = t
                if a_text is not None and u_text is not None and verdict is not None:
                    return a_text, u_text, verdict
    return a_text or "", u_text or "", verdict


def analyze_session(session_id, cwd):
    """Cached (assistant_text, user_text, verdict); unchanged files -> one stat()."""
    path = _resolve_transcript_path(session_id, cwd)
    if not path:
        return "(no transcript)", "", None
    try:
        st = os.stat(path)
    except OSError:
        return "(no transcript)", "", None
    key = (st.st_size, st.st_mtime)
    cached = _transcript_cache.get(session_id)
    if cached and cached[0] == key:
        return cached[1]
    a_text, u_text, verdict = _analyze_tail(path)
    val = (a_text or "(no transcript)", u_text or "", verdict)
    _transcript_cache[session_id] = (key, val)
    return val


# ---------------------------------------------------------------------------
# Session enumeration
# ---------------------------------------------------------------------------


def _iter_tail_objs(path):
    """Yield parsed JSON objects from the tail, newest-first (bounded read)."""
    try:
        size = os.path.getsize(path)
    except OSError:
        return
    remainder = b""
    read_total = 0
    with open(path, "rb") as f:
        pos = size
        while pos > 0 and read_total < TAIL_CEILING:
            step = min(TAIL_CHUNK, pos)
            pos -= step
            read_total += step
            f.seek(pos)
            chunk = f.read(step) + remainder
            parts = chunk.split(b"\n")
            if pos > 0:
                remainder = parts[0]
                parts = parts[1:]
            else:
                remainder = b""
            for raw in reversed(parts):
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    yield json.loads(raw)
                except (ValueError, TypeError):
                    continue


def _count_meaningful(sid, cwd, cap=2):
    """Count assistant/user (non-sidechain) transcript entries, up to `cap`."""
    path = _resolve_transcript_path(sid, cwd)
    if not path:
        return 0
    n = 0
    for o in _iter_tail_objs(path):
        if (not o.get("isSidechain") and o.get("type") in ("assistant", "user")
                and isinstance(o.get("message"), dict)):
            n += 1
            if n >= cap:
                return n
    return n


def _is_llm_worker(cwd):
    """True if cwd is (under) the summarizer's dedicated worker dir - the precise,
    primary exclusion for the board watching its own `claude -p` calls."""
    return any(cwd == p or cwd.startswith(p + "/") for p in _LLM_WORKER_PREFIXES)


def _is_tmp_ghost(data, sid, cwd):
    """Belt-and-braces: a bare-/tmp claude process named tmp-XX with an almost
    empty transcript. Secondary to the marker-dir exclusion."""
    if os.path.normpath(cwd) not in ("/tmp", "/private/tmp"):
        return False
    if not _TMP_GHOST_NAME_RE.match(data.get("name") or ""):
        return False
    return _count_meaningful(sid, cwd, cap=2) < 2


def _to_epoch_seconds(val):
    """Accept epoch-ms ints, epoch-seconds ints, or ISO strings -> float secs."""
    if val is None:
        return None
    if isinstance(val, (int, float)):
        # Heuristic: values above ~ year 2001 in ms are > 1e12.
        return val / 1000.0 if val > 1e11 else float(val)
    if isinstance(val, str):
        s = val.strip()
        # numeric string?
        try:
            num = float(s)
            return num / 1000.0 if num > 1e11 else num
        except ValueError:
            pass
        # ISO-8601
        try:
            iso = s.replace("Z", "+00:00")
            import datetime

            return datetime.datetime.fromisoformat(iso).timestamp()
        except (ValueError, TypeError):
            return None
    return None


def _pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except (OSError, TypeError, ValueError):
        return False
    return True


def read_sessions(now_wall):
    """Return list of live, non-stale session dicts from the sessions dir."""
    out = []
    try:
        names = os.listdir(SESSIONS_DIR)
    except OSError:
        return out
    for fn in names:
        if not fn.endswith(".json"):
            continue
        fpath = os.path.join(SESSIONS_DIR, fn)
        try:
            with open(fpath, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        pid = data.get("pid")
        sid = data.get("sessionId")
        if not pid or not sid:
            continue
        if not _pid_alive(pid):
            continue
        cwd = data.get("cwd", "")
        # Exclude the summarizer's own in-flight `claude -p` worker sessions so
        # the board never charts itself thinking (blank tmp-XX / sb-llm-worker
        # ghost cards). Marker-dir match is primary; tmp-ghost is belt-and-braces.
        if _is_llm_worker(cwd) or _is_tmp_ghost(data, sid, cwd):
            continue
        updated = _to_epoch_seconds(data.get("updatedAt")) or 0.0
        # Transcript mtime is the reliable activity signal: sessions/<PID>.json
        # updatedAt freezes at turn start (stale on active sessions) and its
        # status:"busy" is sticky, so use max(updatedAt, transcript mtime) for
        # both the freshness filter and age.
        tmtime = transcript_mtime(sid, cwd)
        last_activity = max(updated, tmtime or 0.0)
        if last_activity and (now_wall - last_activity) > STALE_UPDATED_SECS:
            continue
        data["_updated_secs"] = updated
        data["_transcript_mtime"] = tmtime
        data["_last_activity"] = last_activity
        data["_started_secs"] = _to_epoch_seconds(data.get("startedAt")) or 0.0
        out.append(data)
    return out


# ---------------------------------------------------------------------------
# Status logic
# ---------------------------------------------------------------------------


def _luxafor_working(session_id):
    return os.path.exists(os.path.join(LUXAFOR_DIR, "session-" + session_id))


# (path, inode) -> is this file the session-board daemon's own log?
_sb_log_cache = {}


def _is_sb_daemon_log(path):
    """True if `path` is the session-board daemon's own stdout/stderr log (its
    content starts with a '[SB] ' line). The daemon runs as a background TASK of
    its owner session and rewrites this log every ~2s, which would otherwise pin
    the owner at WORKING forever via the fresh-task-output signal. Cached per
    (path, inode) so each candidate file is read at most once."""
    try:
        ino = os.stat(path).st_ino
    except OSError:
        return False
    key = (path, ino)
    hit = _sb_log_cache.get(key)
    if hit is not None:
        return hit
    try:
        with open(path, "rb") as f:
            head = f.read(200)
    except OSError:
        return False
    verdict = b"[SB] " in head
    _sb_log_cache[key] = verdict
    return verdict


def _fresh_task_output(tasks_dir, window, now_wall):
    """True if any file in tasks_dir was written within `window` AND is not the
    session-board daemon's own log. Content is only read for fresh candidates."""
    try:
        names = os.listdir(tasks_dir)
    except OSError:
        return False
    for name in names:
        p = os.path.join(tasks_dir, name)
        try:
            if (now_wall - os.stat(p).st_mtime) > window:
                continue
        except OSError:
            continue
        if _is_sb_daemon_log(p):
            continue
        return True
    return False


def _bg_agents_active(sid, cwd, now_wall):
    """True if a session's background task output was written in the last
    BG_AGENT_SECS (its own daemon log excluded). Covers an orchestrating session
    whose transcript is quiet while its subagents/teammates produce output."""
    if not cwd:
        return False
    tasks = os.path.join(SCRATCH_ROOT, _sanitize_cwd(cwd), sid, "tasks")
    return _fresh_task_output(tasks, BG_AGENT_SECS, now_wall)


def is_working(sid, status, cwd, tmtime, now_wall):
    """A session is 'working' if ANY of:
      - its luxafor state file exists (user-prompt turns), OR
      - its transcript was written in the last WORKING_MTIME_SECS (covers
        teammate-message / background-wakeup turns with no luxafor file), OR
      - a background task-output was written in the last BG_AGENT_SECS (covers
        an orchestrating session whose own transcript is quiet while its
        subagents run), OR
      - the registry status is "busy" AND its transcript is < BUSY_MTIME_SECS
        cold (covers a session blocked on subagents/teammates that writes
        nothing itself for minutes but is genuinely mid-turn).
    The registry busy flag is recency-gated because it wedges sticky-busy for
    hours/days; once the transcript goes >30min cold we stop trusting it."""
    if _luxafor_working(sid):
        return True
    if tmtime is not None and (now_wall - tmtime) <= WORKING_MTIME_SECS:
        return True
    if _bg_agents_active(sid, cwd, now_wall):
        return True
    if tmtime is None:
        return False
    return status == "busy" and (now_wall - tmtime) <= BUSY_MTIME_SECS


def delegated_work_active(sid, cwd, now_wall):
    """True if a session that ended its own turn is nonetheless coordinating live
    delegated work, detected ONLY by fresh background task output
    (<=DELEGATE_TASK_SECS, its own daemon log excluded). This OVERRIDES an
    end_turn verdict back to 'working'.

    Team inbox/config mtimes are deliberately NOT used: idle-ping notifications
    and SendMessage replies keep those files perpetually fresh, so they can't
    distinguish real work from chatter and would pin the owner at WORKING.
    Tradeoff: teammates grinding without writing task outputs may briefly read
    done between the owner's turns - better than a permanent false WORKING."""
    if cwd:
        tasks = os.path.join(SCRATCH_ROOT, _sanitize_cwd(cwd), sid, "tasks")
        if _fresh_task_output(tasks, DELEGATE_TASK_SECS, now_wall):  # skips own log
            return True
    return False


def derive_status(session, verdict, tmtime, now_wall, agent_active=False):
    """v5 status. Returns (st, sclass) where st is the device status ('w'/'d'/'i')
    and sclass is the summary framing ('w' present, 'wait' waiting-on-delegated,
    'done' past).
      verdict ("w", _)     -> mid-turn -> ('w','w')
      verdict ("done", ts) -> if delegated work is live (fresh task output OR an
                              actively-CPU-burning teammate agent) -> ('w','wait')
                              OVERRIDE; else 'd' within DONE_WINDOW_SECS / 'i'
      verdict None         -> fall back to v3 activity signals
    The transcript verdict is the primary truth about the TURN; the override
    corrects for the WORK: a session can end its turn while its teammates /
    background agents keep building."""
    sid = session.get("sessionId", "")
    cwd = session.get("cwd", "")
    if verdict is not None:
        cls, ts = verdict
        if cls == "w":
            return "w", "w"
        # done tail: is delegated/background work still live?
        if agent_active or delegated_work_active(sid, cwd, now_wall):
            return "w", "wait"
        ref = ts if ts is not None else (tmtime if tmtime is not None else now_wall)
        return ("d" if (now_wall - ref) <= DONE_WINDOW_SECS else "i"), "done"
    # No transcript / unclassifiable: fall back to the v3 signal logic.
    if agent_active or is_working(sid, session.get("status"), cwd, tmtime, now_wall):
        return "w", "w"
    return "i", "done"


# ---------------------------------------------------------------------------
# LLM summarization (titles + glanceable status via `claude` CLI, headless)
# ---------------------------------------------------------------------------

_LLM_PROMPT = (
    "You label a developer's live coding sessions for a tiny 1-inch status "
    "display. Given a session's project and its most recent activity, output a "
    "short human title and a glanceable status.\n\n"
    "Return ONLY a compact JSON object, no markdown, exactly:\n"
    '{"title":"...","status":"...","detail":"..."}\n\n'
    "title: 2-4 words, Title Case, at most 24 characters, naming what the "
    "session is ABOUT (e.g. \"LCD Session Board\", \"Kitchen Wiring Fixes\"). "
    "No punctuation, no file names.\n"
    "status: at most 260 characters, plain English, dumbed down for a quick "
    "glance. No file paths, no code, no jargon, no markdown, NO asterisks. "
    "@@TENSE@@\n"
    "detail: at most 700 characters, plain English but meatier. FIRST give a "
    "2-4 sentence overview - what the agent has done so far, the key findings or "
    "decisions, and what it is doing next or waiting on (same tense rule as "
    "status). Wrap the 2-5 MOST important words or short phrases in double "
    "asterisks for bold, e.g. \"created a new identity (**trepo-bot-readonly**) "
    "with **scoped permissions**\". THEN end with EXACTLY ONE closing sentence "
    "that states what the agent is actually telling the user, taken from its "
    "latest message (do NOT invent it). Prefix that closer with exactly one of "
    "these bolded classifiers, whichever fits best:\n"
    "  **Asking you:** - the agent is asking the user a question or needs input.\n"
    "  **Answer:** - the agent delivered an answer or conclusion.\n"
    "  **Bottom line:** - a statement, direction, or next step.\n"
    "The closer is the single most important part - always include exactly one. "
    "No file paths, no code, no jargon. Balance every pair of asterisks; use "
    "bold ONLY in detail, never in title or status.\n\n"
    "Project: @@PROJECT@@\n"
    "Latest user request: @@USER@@\n"
    "Latest assistant reply: @@ASSISTANT@@\n"
)

_TENSE_WORKING = (
    "The agent is still WORKING: use PRESENT tense, saying what "
    "it is doing right now and what it is still expected to deliver."
)
_TENSE_WAIT = (
    "The agent finished its own reply but is actively coordinating DELEGATED "
    "work - its teammates or background agents are still running. Use PRESENT "
    "tense and make clear it is WAITING ON that delegated/background work "
    "(e.g. \"Waiting on its build agent to compile the firmware.\"). Do not "
    "imply the overall job is finished."
)
_TENSE_DONE = (
    "The agent has FINISHED and is waiting on the user: use PAST tense, "
    "saying what it accomplished or delivered. Do not imply it is still working."
)

_TENSE_BY_CLASS = {"w": _TENSE_WORKING, "wait": _TENSE_WAIT, "done": _TENSE_DONE}


def _strip_json(raw):
    """Pull a JSON object out of an LLM reply that may be fenced or chatty."""
    s = raw.strip()
    if s.startswith("```"):
        # drop opening fence (``` or ```json) and trailing fence
        s = re.sub(r"^```[a-zA-Z0-9]*\s*", "", s)
        s = re.sub(r"\s*```$", "", s.strip())
    a, b = s.find("{"), s.rfind("}")
    if a != -1 and b != -1 and b > a:
        s = s[a:b + 1]
    return s


def _llm_summarize(project, assistant_text, user_text, sclass):
    """Call `claude -p` headless and return {"title","status","detail"} or None.
    sclass is "w" (present), "wait" (waiting on delegated work), or "done" (past)."""
    tense = _TENSE_BY_CLASS.get(sclass, _TENSE_DONE)
    # Plain .replace (not str.format): the prompt contains literal { } braces.
    prompt = (_LLM_PROMPT
              .replace("@@TENSE@@", tense)
              .replace("@@PROJECT@@", project or "(unknown)")
              .replace("@@USER@@", (user_text or "(none)")[:LLM_USER_CAP])
              .replace("@@ASSISTANT@@", (assistant_text or "(none)")[:LLM_TEXT_CAP]))
    try:
        os.makedirs(LLM_WORKER_DIR, exist_ok=True)
    except OSError:
        pass
    try:
        proc = subprocess.run(
            [LLM_BIN, "-p", prompt, "--model", LLM_MODEL],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=LLM_TIMEOUT,
            # Dedicated marker dir (not bare /tmp): keeps CLAUDE.md/context out AND
            # lets read_sessions exclude these self-registered worker sessions.
            cwd=LLM_WORKER_DIR,
        )
    except (subprocess.TimeoutExpired, OSError):
        return None
    if proc.returncode != 0:
        return None
    try:
        obj = json.loads(_strip_json(proc.stdout))
    except (ValueError, TypeError):
        return None
    title = obj.get("title")
    status = obj.get("status")
    if not isinstance(title, str) or not isinstance(status, str):
        return None
    title = strip_bold(clean(title, TITLE_MAX))     # no bold in title/status
    status = strip_bold(clean(status, STATUS_MAX))
    if not title or not status:
        return None
    detail_raw = obj.get("detail")
    if isinstance(detail_raw, str) and detail_raw.strip():
        # clean without truncating, then closer-aware fit (protects the closer).
        detail = balance_bold(fit_detail(clean(detail_raw, 10 ** 9), DETAIL_MAX))
    else:
        detail = status  # degrade gracefully if the model omitted detail
    return {"title": title, "status": status, "detail": detail}


def _text_hash(text):
    return hashlib.sha1((text or "").encode("utf-8", "ignore")).hexdigest()[:16]


class SummaryManager:
    """Async, cached LLM summaries with a SPLIT cache and fair scheduling.

    - `title` is keyed (sid8, texthash) ONLY - it is tense/class-independent, so
      it never regresses when a session's status class flips.
    - `status`/`detail` are keyed (sid8, texthash, class).
    One LLM call fills both. display() never blocks and never dispatches; it
    registers a NEED. pump() then dispatches the highest-priority needs into the
    free worker slots, so a never-summarized session is never starved by a churny
    one. On a class miss with a same-text entry under another class, the stale
    class's status/detail is shown as interim (better than raw text)."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self._lock = threading.Lock()
        self._sem = threading.Semaphore(LLM_MAX_CONCURRENT)
        self._titles = {}        # (sid8, th) -> {"title","ts"}
        self._title_by_sid = {}  # sid8 -> {"title","ts"}  (latest, any th)
        self._status = {}        # (sid8, th) -> {class: {"status","detail","ts"}}
        self._inflight = set()   # (sid8, th, class) keys being computed
        self._failed = {}        # key -> monotonic time of last failure
        self._pending = {}       # key -> (session, raw_a, raw_u, sclass, priority)
        self._load()

    # ---- persistence ----
    def _load(self):
        try:
            with open(SUMMARY_CACHE_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return
        if not isinstance(data, dict) or data.get("version") != SUMMARY_CACHE_VERSION:
            return
        for r in data.get("titles", []):
            if isinstance(r, dict) and r.get("title"):
                key = (r.get("sid8"), r.get("th"))
                ent = {"title": r["title"], "ts": r.get("ts", 0)}
                self._titles[key] = ent
                cur = self._title_by_sid.get(r.get("sid8"))
                if cur is None or ent["ts"] >= cur["ts"]:
                    self._title_by_sid[r.get("sid8")] = ent
        for r in data.get("status", []):
            if isinstance(r, dict) and r.get("status"):
                self._status.setdefault((r.get("sid8"), r.get("th")), {})[r.get("cls")] = {
                    "status": r["status"], "detail": r.get("detail") or r["status"],
                    "ts": r.get("ts", 0)}

    def _persist_locked(self):
        titles = [{"sid8": s, "th": t, "title": v["title"], "ts": v["ts"]}
                  for (s, t), v in self._titles.items()]
        status = [{"sid8": s, "th": t, "cls": c, "status": e["status"],
                   "detail": e["detail"], "ts": e["ts"]}
                  for (s, t), cls in self._status.items() for c, e in cls.items()]
        titles.sort(key=lambda r: r["ts"])
        status.sort(key=lambda r: r["ts"])
        titles = titles[-SUMMARY_CACHE_MAX:]
        status = status[-SUMMARY_CACHE_MAX:]
        tmp = SUMMARY_CACHE_PATH + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({"version": SUMMARY_CACHE_VERSION,
                           "titles": titles, "status": status}, f)
            os.replace(tmp, SUMMARY_CACHE_PATH)
        except OSError:
            pass

    def _fallback_title(self, session):
        pj = clean(_project_label(session.get("cwd", "")), PROJ_MAX)
        return clean(session.get("name") or "", TITLE_MAX) or pj

    def _compute(self, session, raw_a, raw_u, sclass, sid8, th, key):
        """Run one summarization (holds a slot); fills title + status caches."""
        res = None
        try:
            with self._sem:
                project = _project_label(session.get("cwd", ""))
                if self.verbose:
                    sys.stderr.write("[SB] llm summarize %s:%s (%s)\n" % (sid8, sclass, project))
                res = _llm_summarize(project, raw_a, raw_u, sclass)
        except Exception as e:
            if self.verbose:
                sys.stderr.write("[SB] llm error %s: %r\n" % (sid8, e))
            res = None
        with self._lock:
            self._inflight.discard(key)
            if res:
                ts = time.time()
                tent = {"title": res["title"], "ts": ts}
                self._titles[(sid8, th)] = tent           # title: class-independent
                self._title_by_sid[sid8] = tent
                self._status.setdefault((sid8, th), {})[sclass] = {
                    "status": res["status"], "detail": res.get("detail") or res["status"],
                    "ts": ts}
                self._persist_locked()
            else:
                self._failed[key] = time.monotonic()

    def _dispatch(self, session, raw_a, raw_u, sclass, sid8, th, key):
        t = threading.Thread(target=self._compute,
                             args=(session, raw_a, raw_u, sclass, sid8, th, key),
                             daemon=True)
        t.start()
        return t

    def display(self, session, raw_a, raw_u, sclass):
        """Return {"title","status","detail"} to render now, and register a NEED
        (dispatched later by pump()). Never blocks, never dispatches."""
        sid8 = session.get("sessionId", "")[:8]
        th = _text_hash(raw_a)
        key = (sid8, th, sclass)
        meaningful = raw_a and raw_a != "(no transcript)"
        with self._lock:
            # title: current th, else any prior title for this session, else name
            tent = self._titles.get((sid8, th)) or self._title_by_sid.get(sid8)
            title = tent["title"] if tent else self._fallback_title(session)
            # status/detail: exact class, else a stale sibling class, else raw text
            byth = self._status.get((sid8, th), {})
            sent = byth.get(sclass)
            if sent is None and byth:
                sent = max(byth.values(), key=lambda e: e["ts"])   # newest stale class
            if sent is not None:
                status, detail = sent["status"], sent["detail"]
            else:
                status = clean(raw_a, STATUS_MAX)
                detail = clean(raw_a, DETAIL_MAX)
            # register the need (unless satisfied for this exact class / inflight)
            need = meaningful and byth.get(sclass) is None and key not in self._inflight
            if need:
                fa = self._failed.get(key)
                if fa is None or (time.monotonic() - fa) > SUMMARY_FAIL_COOLDOWN:
                    # priority: 0 never-titled session, 1 missing this class, 2 refresh
                    if sid8 not in self._title_by_sid:
                        pri = 0
                    elif byth.get(sclass) is None:
                        pri = 1
                    else:
                        pri = 2
                    self._pending[key] = (session, raw_a, raw_u, sclass, pri)
        return {"title": title, "status": status, "detail": detail}

    def pump(self):
        """Dispatch the highest-priority pending needs into free worker slots.
        Called once per snapshot; keeps churny sessions from starving quiet ones.
        Unserved needs re-register on the next cycle's display()."""
        with self._lock:
            free = LLM_MAX_CONCURRENT - len(self._inflight)
            pending = self._pending
            self._pending = {}
            if free <= 0 or not pending:
                return
            # priority (0 highest) then registration order (dict is insertion-ordered)
            items = sorted(pending.items(), key=lambda kv: kv[1][4])
            to_start = []
            for key, (session, raw_a, raw_u, sclass, pri) in items:
                if len(to_start) >= free:
                    break
                if key in self._inflight:
                    continue
                self._inflight.add(key)
                to_start.append((session, raw_a, raw_u, sclass, key))
        for session, raw_a, raw_u, sclass, key in to_start:
            self._dispatch(session, raw_a, raw_u, sclass, key[0], key[1], key)

    def prime_sync(self, jobs):
        """Blocking: compute all missing (title,class) summaries (for --once)."""
        threads = []
        for session, raw_a, raw_u, sclass in jobs:
            if not raw_a or raw_a == "(no transcript)":
                continue
            sid8 = session.get("sessionId", "")[:8]
            th = _text_hash(raw_a)
            key = (sid8, th, sclass)
            with self._lock:
                if self._status.get((sid8, th), {}).get(sclass) or key in self._inflight:
                    continue
                self._inflight.add(key)
            threads.append(self._dispatch(session, raw_a, raw_u, sclass, sid8, th, key))
        for t in threads:
            t.join(LLM_TIMEOUT + 5)


# ---------------------------------------------------------------------------
# Snapshot building
# ---------------------------------------------------------------------------


def _project_label(cwd):
    if not cwd:
        return ""
    if os.path.normpath(cwd) == os.path.normpath(HOME):
        return "~"
    return os.path.basename(cwd.rstrip("/")) or "~"


def _parse_cputime(s):
    """Parse a `ps` CPU-time field ('MM:SS.ss', 'HH:MM:SS', 'DD-HH:MM:SS') -> secs."""
    s = s.strip()
    days = 0
    if "-" in s:
        d, s = s.split("-", 1)
        try:
            days = int(d)
        except ValueError:
            days = 0
    try:
        parts = [float(p) for p in s.split(":")]
    except ValueError:
        return None
    if len(parts) == 3:
        secs = parts[0] * 3600 + parts[1] * 60 + parts[2]
    elif len(parts) == 2:
        secs = parts[0] * 60 + parts[1]
    elif len(parts) == 1:
        secs = parts[0]
    else:
        return None
    return days * 86400 + secs


def _scan_agent_procs():
    """One `ps` pass. Return list of (pid, cpu_secs, owner_full_sid, agent_id) for
    every teammate agent process (identified by --agent-id)."""
    try:
        out = subprocess.run(["ps", "-axo", "pid,cputime,command"],
                             capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    procs = []
    for line in out.splitlines():
        if "--agent-id " not in line:
            continue
        bits = line.strip().split(None, 2)
        if len(bits) < 3:
            continue
        pid_s, cpu_s, cmd = bits
        try:
            pid = int(pid_s)
        except ValueError:
            continue
        cpu = _parse_cputime(cpu_s)
        if cpu is None:
            continue
        m = re.search(r"--agent-id (\S+)", cmd)
        agent_id = m.group(1) if m else None
        if not agent_id:
            continue
        # Owner = the full parent-session-id (direct & correct even when the team
        # name differs from the lead's sid8, e.g. session-56233906 -> 1765e702...).
        pm = re.search(r"--parent-session-id (\S+)", cmd)
        owner = pm.group(1) if pm else None
        if owner is None:                       # fallback: @session-<sid8> suffix
            sm = re.search(r"@session-([0-9a-f]{8})\b", agent_id)
            owner = sm.group(1) if sm else None
        if owner:
            procs.append((pid, cpu, owner, agent_id))
    return procs


class AgentActivity:
    """Detects teammate agents actively working via per-pid CPU-time deltas across
    ~2s cycles, so a session that delegated work to tmux teammates reads WORKING
    even between its own turns. Per-session counts (agn total / aga active) are
    also surfaced to the device. In-memory, stateful across cycles."""

    def __init__(self):
        self._pid = {}    # pid -> {"cpu","samples","low","active","owner","agent"}

    def scan(self):
        """Take one sample. Returns {owner_full_sid: (agn, aga)}."""
        procs = _scan_agent_procs()
        seen = set()
        for pid, cpu, owner, agent in procs:
            seen.add(pid)
            prev = self._pid.get(pid)
            if prev is None:                          # newly seen -> startup grace
                self._pid[pid] = {"cpu": cpu, "samples": 1, "low": 0,
                                  "active": True, "owner": owner, "agent": agent}
                continue
            delta = cpu - prev["cpu"]
            samples = prev["samples"] + 1
            if samples <= AGENT_GRACE_SAMPLES:
                active, low = True, 0
            elif delta > AGENT_CPU_DELTA:
                active, low = True, 0
            else:
                low = prev["low"] + 1
                active = prev["active"] if low < AGENT_LOW_SAMPLES else False
            self._pid[pid] = {"cpu": cpu, "samples": samples, "low": low,
                              "active": active, "owner": owner, "agent": agent}
        for pid in [p for p in self._pid if p not in seen]:   # gone -> forget
            del self._pid[pid]

        # Aggregate per owner: distinct agents (an agent has a bash + claude pid
        # sharing one agent-id; it is active if ANY of its pids is active).
        agents = {}   # owner -> {agent_id -> active}
        for st in self._pid.values():
            a = agents.setdefault(st["owner"], {})
            a[st["agent"]] = a.get(st["agent"], False) or st["active"]
        return {owner: (len(a), sum(1 for v in a.values() if v))
                for owner, a in agents.items()}


class NameStore:
    """Persistent sid8 -> Matt-assigned custom name. Separate from the auto-title
    (which keeps updating); a sibling file that is NEVER version-busted, since
    these are precious user-set names, not regenerable summaries."""

    def __init__(self):
        self._lock = threading.Lock()
        self._names = {}
        try:
            with open(NAMES_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict):
                self._names = {k: v for k, v in data.items() if isinstance(v, str) and v}
        except (OSError, ValueError):
            pass

    def get(self, sid8):
        return self._names.get(sid8)

    def set(self, sid8, name):
        with self._lock:
            self._names[sid8] = name
            tmp = NAMES_FILE + ".tmp"
            try:
                with open(tmp, "w", encoding="utf-8") as f:
                    json.dump(self._names, f)
                os.replace(tmp, NAMES_FILE)
            except OSError:
                pass


class PositionStore:
    """Persistent sid8 -> [index, seq] manual pin (A1: absolute-index reordering).
    A pinned agent holds a fixed board row regardless of status; unpinned agents
    flow through the calm order in the gaps. seq is a monotonic set-counter used
    only to break placement ties (the most-recently-moved agent wins its exact
    slot). Sibling file, NEVER version-busted (user-set, precious like custom
    names)."""

    def __init__(self):
        self._lock = threading.Lock()
        self._pins = {}       # sid8 -> [idx, seq]
        self._seq = 0
        try:
            with open(POSITIONS_FILE, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict):
                for k, v in data.items():
                    if (isinstance(v, list) and len(v) == 2
                            and all(isinstance(x, int) and not isinstance(x, bool)
                                    for x in v)):
                        self._pins[k] = [v[0], v[1]]
                        self._seq = max(self._seq, v[1])
        except (OSError, ValueError):
            pass

    def any(self):
        return bool(self._pins)

    def get(self, sid8):
        """(idx, seq) for a pinned agent, or None if unpinned."""
        p = self._pins.get(sid8)
        return (p[0], p[1]) if p else None

    def set(self, sid8, idx):
        with self._lock:
            self._seq += 1
            self._pins[sid8] = [int(idx), self._seq]
            self._save()

    def clear(self, sid8):
        with self._lock:
            if self._pins.pop(sid8, None) is not None:
                self._save()

    def _save(self):
        tmp = POSITIONS_FILE + ".tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self._pins, f)
            os.replace(tmp, POSITIONS_FILE)
        except OSError:
            pass


def parse_pos(line_bytes):
    """{"t":"pos","id":<sid8>,"idx":<int>} -> (sid8, idx); idx == -1 means unpin.
    Returns (sid8, idx) or None. sid trimmed to 8 chars (board identity)."""
    if not line_bytes.startswith(b"{"):
        return None
    try:
        o = json.loads(line_bytes)
    except (ValueError, TypeError):
        return None
    if o.get("t") == "pos":
        sid = o.get("id")
        idx = o.get("idx")
        if (isinstance(sid, str) and sid
                and isinstance(idx, int) and not isinstance(idx, bool)):
            return (sid[:8], idx)
    return None


def parse_view(line_bytes):
    """{"t":"view","fleet":"claude"|"nebula"} -> the fleet name; else None."""
    if not line_bytes.startswith(b"{"):
        return None
    try:
        o = json.loads(line_bytes)
    except (ValueError, TypeError):
        return None
    if o.get("t") == "view" and o.get("fleet") in ("claude", "nebula"):
        return o.get("fleet")
    return None


def apply_pins(ordered_ids, positions, max_slots):
    """Apply A1 manual pins to a calm-ordered id list AND cap to max_slots in one
    pass. Returns the final visible id list (length min(len(ordered_ids),max_slots)).
    Pinned agents are placed at their stored index (clamped into the visible
    window so a pin is never truncated by the cap); collisions resolve by
    (index asc, most-recent-set first) spilling to the next free slot; unpinned
    agents fill the remaining slots in calm order."""
    n = len(ordered_ids)
    limit = min(n, max_slots)
    if positions is None or not positions.any() or limit == 0:
        return ordered_ids[:limit]                 # no pins: calm order, capped

    def clamp(idx):
        return 0 if idx < 0 else (limit - 1 if idx > limit - 1 else idx)

    pinned, unpinned = [], []
    for sid in ordered_ids:
        p = positions.get(sid[:8])
        if p is None:
            unpinned.append(sid)
        else:
            pinned.append((sid, p[0], p[1]))       # (sid, idx, seq)
    if not pinned:
        return ordered_ids[:limit]
    # index asc, then most-recently-set first (so the just-moved agent wins ties)
    pinned.sort(key=lambda t: (clamp(t[1]), -t[2]))
    slots = [None] * limit
    for sid, idx, _seq in pinned[:limit]:          # can't seat more pins than slots
        target = clamp(idx)
        j = target
        while j < limit and slots[j] is not None:  # first free slot at/after target
            j += 1
        if j >= limit:                             # none after -> search backward
            j = target - 1
            while j >= 0 and slots[j] is not None:
                j -= 1
        if 0 <= j < limit:
            slots[j] = sid
    ui = 0
    for k in range(limit):                         # unpinned fill the gaps, calm order
        if slots[k] is None and ui < len(unpinned):
            slots[k] = unpinned[ui]
            ui += 1
    return [s for s in slots if s is not None]


def _clean_custom_name(text):
    """Lightly clean a spoken rename into a custom name: strip a leading
    'call it'/'name it'/'rename to' phrase, trim, cap to CUSTOM_NAME_MAX."""
    s = clean(text, 10 ** 9)          # ASCII-fold + collapse whitespace, no truncate
    s = re.sub(r"^(?:please\s+)?(?:you can\s+)?"
               r"(?:call it|call this|name it|name this|rename(?: it| this)?"
               r"(?: to)?|set (?:the )?name to|the name is)\s+",
               "", s, flags=re.I).strip()
    s = s.strip(" .,!?:;-\"'")
    if len(s) > CUSTOM_NAME_MAX:
        s = s[:CUSTOM_NAME_MAX].rstrip()
    return s


class OrderTracker:
    """Calm board ordering. Sort key is the status GROUP only (d=0, w=1, i=2);
    ages never participate. Within a group the PREVIOUS emitted relative order is
    preserved, so a card moves only when its status group changes - and then to
    the END of its new group (least disruptive). New sessions append to the end
    of their group; vanished sessions drop out; survivors keep relative order.
    In-memory only (a restart may reorder once)."""

    def __init__(self):
        self._order = []      # last-emitted session ids, flat, in emit order
        self._group = {}      # id -> last-emitted status rank

    def order(self, ids_ranks):
        """ids_ranks: list of (id, rank) for the current sessions.
        Returns the ids in the new stable board order."""
        cur = dict(ids_ranks)                       # id -> current rank
        # Survivors in their previous emitted order, then brand-new ids appended.
        base = [i for i in self._order if i in cur]
        seen = set(base)
        for i, _ in ids_ranks:
            if i not in seen:                       # brand new -> end of list
                base.append(i)
                seen.add(i)
        # A group change moves that id to the end (so the stable sort below places
        # it last within its NEW group); everyone else keeps relative order.
        changed = set(i for i in base
                      if i in self._group and self._group[i] != cur[i])
        if changed:
            base = [i for i in base if i not in changed] + \
                   [i for i in base if i in changed]
        base.sort(key=lambda i: cur[i])             # stable: group only
        self._order = base
        self._group = cur
        return base


class FirstSeenOrder:
    """Append-on-first-appearance ordering: an id keeps its slot as long as it
    exists, newcomers append to the end, vanished ids drop out. This is the stable
    'spatial map' base for the FLEET list (manual pins layer on top) - nothing moves
    unless Matt pins it. Decoupled from the carousel, which stays calm status-group.
    In-memory (a restart re-seeds from the first cycle)."""

    def __init__(self):
        self._seen = []

    def order(self, ids):
        cur = set(ids)
        seen = [i for i in self._seen if i in cur]      # survivors keep their slot
        known = set(seen)
        for i in ids:                                   # newcomers append, stable
            if i not in known:
                seen.append(i)
                known.add(i)
        self._seen = seen
        return list(seen)


def build_snapshot(sessions, seq, now_wall, summaries=None, order=None,
                   agents=None, names=None, positions=None, firstseen=None,
                   log_warn=None):
    """Return a list of protocol JSON strings (hdr + s/x per session + end).

    Status comes from the transcript-tail verdict (derive_status). Sessions are
    ordered done -> working -> idle, freshest first within each group. Each 's'
    line is followed by an optional 'x' detail line (old firmware ignores it)."""
    # One ps sample per snapshot: owner_full_sid -> (agn total, aga active).
    counts = agents.scan() if agents is not None else {}
    # Phase 1: derive status + age for every session (analyze_session is cached).
    recs = []
    for s in sessions:
        sid = s.get("sessionId", "")
        cwd = s.get("cwd", "")
        tmtime = s.get("_transcript_mtime")
        agn, aga = counts.get(sid, (0, 0))
        raw_a, raw_u, verdict = analyze_session(sid, cwd)
        st, sclass = derive_status(s, verdict, tmtime, now_wall, agent_active=aga > 0)
        activity = tmtime if tmtime is not None else s.get("_updated_secs", now_wall)
        age = max(0, min(int(now_wall - activity), 359999))
        # Dormant = registry updatedAt AND transcript both stale > DORMANT_SECS
        # (transcript-recent avoids false-flagging a genuinely idle-but-recent one).
        updated = s.get("_updated_secs", 0.0)
        dormant = ((now_wall - updated) > DORMANT_SECS
                   and (tmtime is None or (now_wall - tmtime) > DORMANT_SECS))
        recs.append({"s": s, "sid": sid, "cwd": cwd, "raw_a": raw_a, "raw_u": raw_u,
                     "st": st, "sclass": sclass, "age": age, "agn": agn, "aga": aga,
                     "drm": dormant})

    # Phase 2: TWO decoupled orderings (Matt's choice (c)).
    #  - CAROUSEL (the emitted s-line 'i' order): calm status-group (done->working->
    #    idle), within-group previous-emit-stable. NO pins - unchanged glance order.
    #  - FLEET (the 'fi' field): first-seen base + manual pins - a fully stable
    #    spatial map where nothing moves unless Matt pinned it.
    # The visible set is the fleet's top MAX_SESSIONS (pins clamped into view); the
    # carousel orders that same set.
    ranks = {r["sid"]: STATUS_RANK.get(r["st"], 9) for r in recs}
    all_sids = [r["sid"] for r in recs]
    if order is not None:
        calm_ids = order.order([(r["sid"], ranks[r["sid"]]) for r in recs])
    else:                                          # stable: group only, no persistence
        calm_ids = [r["sid"] for r in sorted(recs, key=lambda r: ranks[r["sid"]])]
    base = firstseen.order(all_sids) if firstseen is not None else list(calm_ids)
    fleet_ids = apply_pins(base, positions, MAX_SESSIONS)   # visible set + fleet order
    if log_warn and len(all_sids) > MAX_SESSIONS:
        log_warn("more than %d sessions (%d); capping" % (MAX_SESSIONS, len(all_sids)))
    visible = set(fleet_ids)
    fleet_pos = {sid: k for k, sid in enumerate(fleet_ids)}
    emit_ids = [i for i in calm_ids if i in visible]       # visible set, carousel order
    by_sid = {r["sid"]: r for r in recs}
    recs = [by_sid[i] for i in emit_ids if i in by_sid]

    # Phase 3: emit. Summaries are resolved only for the survivors (so dropped
    # sessions never trigger an LLM dispatch).
    n = len(recs)
    lines = [_dump({"t": "hdr", "seq": seq, "n": n, "fleet": "claude"})]
    for i, r in enumerate(recs):
        s, pj = r["s"], clean(_project_label(r["cwd"]), PROJ_MAX)
        if summaries is not None:
            summ = summaries.display(s, r["raw_a"], r["raw_u"], r["sclass"])
            nm = clean(summ["title"], NAME_MAX) or pj
            msg = clean(summ["status"], MSG_MAX)
            dtl = fit_detail(clean(summ["detail"], 10 ** 9), DETAIL_MAX)
        else:
            nm = clean(s.get("name") or "", NAME_MAX) or pj
            msg = clean(r["raw_a"], MSG_MAX)
            dtl = fit_detail(clean(r["raw_a"], 10 ** 9), DETAIL_MAX)  # no closer -> plain trim
        # Bold only in detail; strip from title/status and rebalance dtl in case
        # a trim split a ** span (fit_detail keeps the closer intact).
        nm, msg, dtl = strip_bold(nm), strip_bold(msg), balance_bold(dtl)
        srec = {"t": "s", "i": i, "id": r["sid"][:8], "nm": nm, "pj": pj,
                "st": r["st"], "age": r["age"], "msg": msg,
                "fi": fleet_pos.get(r["sid"], i)}   # fleet-list row (decoupled from i)
        if r["drm"]:                       # dormant: grey-out hint; omit when not
            srec["drm"] = 1
        cnm = names.get(r["sid"][:8]) if names is not None else None
        if cnm:                            # Matt-assigned custom name; omit when unset
            srec["cnm"] = strip_bold(clean(cnm, CUSTOM_NAME_MAX))
        if positions is not None and positions.get(r["sid"][:8]) is not None:
            srec["pn"] = 1                 # manually pinned; firmware shows a pin badge
        lines.append(_dump_s(srec))
        xrec = {"t": "x", "i": i, "dtl": dtl}
        if r["agn"]:                       # omit the fields when no agents
            xrec["agn"] = r["agn"]
            xrec["aga"] = r["aga"]
        lines.append(_dump_x(xrec))
    if summaries is not None:
        summaries.pump()   # fair, priority-ordered dispatch of this cycle's needs
    lines.append(_dump({"t": "end", "seq": seq, "n": n}))
    return lines


def _dump(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=True)


# The firmware reads each line into a fixed buffer and drops overlong lines.
# JSON escaping (\" \\ etc.) can inflate a 500-char msg past that, so cap the
# *serialized* byte length here by iteratively trimming msg. One place, so both
# --once and serial mode inherit the guarantee.
SERIAL_LINE_MAX = 1000


def _dump_capped(rec, field):
    """Serialize rec, trimming rec[field] until the line is <= SERIAL_LINE_MAX bytes."""
    line = _dump(rec)
    if len(line.encode("utf-8")) <= SERIAL_LINE_MAX:
        return line  # fast path: already fits
    base = rec.get(field, "")  # the un-truncated text we shrink
    while base:
        overshoot = len(line.encode("utf-8")) - SERIAL_LINE_MAX
        if overshoot <= 0:
            break
        # Drop `overshoot` chars (conservative: an escaped char can be 2 bytes)
        # plus 3 for the "..." marker we re-append. Always shrinks -> converges.
        base = base[: max(0, len(base) - overshoot - 3)].rstrip()
        rec = dict(rec, **{field: (base + "...") if base else "..."})
        line = _dump(rec)
    return line


def _dump_s(rec):
    """Serialize an 's' record, trimming msg to fit the serialized-line cap."""
    return _dump_capped(rec, "msg")


def _dump_x(rec):
    """Serialize an 'x' (detail) record, trimming dtl to fit the cap."""
    return _dump_capped(rec, "dtl")


# ---------------------------------------------------------------------------
# Nebula.gg second fleet (read-only status board via the nebula-ai CLI)
# ---------------------------------------------------------------------------

def _nebula_sid8(cid):
    """Stable 8-char board id from a Nebula thread id. HASHED, not a prefix slice:
    Nebula thread ids can share leading hex ("thrd_06978f70..." x3), which would
    collide on a prefix and merge distinct channels on the board."""
    return hashlib.sha1(cid.encode("utf-8")).hexdigest()[:8] if cid else "nebula00"


def _map_nebula_channel(c, now_wall):
    """Map one Nebula channel dict -> a board record. Status: open_turns non-empty
    => working; else done/idle by last_activity recency (mirrors derive_status).
    Summary: thread_summary, else last_message.preview_text (no Haiku needed)."""
    turns = (c.get("state") or {}).get("turns") or {}
    working = bool(turns.get("open_turns"))
    last_ms = c.get("last_activity_at")
    last_s = (last_ms / 1000.0) if isinstance(last_ms, (int, float)) else 0.0
    if working:
        st = "w"
    elif last_s and (now_wall - last_s) <= DONE_WINDOW_SECS:
        st = "d"
    else:
        st = "i"
    age = max(0, min(int(now_wall - last_s), 359999)) if last_s else 359999
    label = c.get("channel_label") or c.get("title") or "channel"
    summ = (c.get("thread_summary") or "").strip()
    if not summ:
        summ = ((c.get("last_message") or {}).get("preview_text") or "").strip()
    return {"sid8": _nebula_sid8(str(c.get("id") or "")), "nm": label, "st": st,
            "age": age, "msg": summ, "pinned": bool(c.get("is_pinned"))}


class NebulaScanner:
    """Polls the Nebula CLI (`npx nebula-ai --json channels list`) for the user's
    agent channels, ON DEMAND only (while the Nebula view is active) and no faster
    than NEBULA_POLL_SECS. Non-interactive (reuses the persisted ~/.nebula session).
    Caches the last good mapped records so a transient CLI error never blanks the
    board."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self._records = []
        self._last_poll = -1e9
        self._err = None
        self._validated = False   # one-time workspace-existence check done?
        self._id_map = {}         # board sid8 -> full "thrd_..." channel id (for sends)

    def poll(self):
        """Return mapped records, re-fetching only if the throttle has elapsed."""
        now = _now_mono()
        if self._records and (now - self._last_poll) < NEBULA_POLL_SECS:
            return self._records
        self._last_poll = now
        self._validate_workspace()   # one-shot defensive check (self-guarded)
        raw = self._fetch()
        if raw is not None:
            self._records = [_map_nebula_channel(c, _now_wall()) for c in raw]
            # sid8 -> full channel id, so a voice send can target the real "thrd_..."
            self._id_map = {_nebula_sid8(str(c.get("id"))): str(c.get("id"))
                            for c in raw if c.get("id")}
            self._err = None
        return self._records

    def channel_id_for(self, sid8):
        """Full Nebula channel id for a board sid8 (from the last poll), or None."""
        return self._id_map.get(sid8)

    def _fetch(self):
        try:
            # --workspace is a GLOBAL flag (before the subcommand) that overrides
            # the CLI's stored active workspace -> immune to that state drifting.
            p = subprocess.run(["npx", NEBULA_CLI, "--json",
                                "--workspace", NEBULA_WORKSPACE, "channels", "list"],
                               stdin=subprocess.DEVNULL, capture_output=True,
                               text=True, timeout=NEBULA_TIMEOUT)
        except (subprocess.TimeoutExpired, OSError) as e:
            self._err = str(e)
            if self.verbose:
                print("[SB] nebula poll error: %s" % e, file=sys.stderr)
            return None
        if p.returncode != 0:
            self._err = "exit %d" % p.returncode
            if self.verbose:
                print("[SB] nebula exit %d: %s"
                      % (p.returncode, (p.stderr or "")[:200]), file=sys.stderr)
            return None
        try:
            d = json.loads(p.stdout)
        except (ValueError, TypeError):
            self._err = "bad json"
            return None
        if isinstance(d, list):
            return d
        return d.get("channels") if isinstance(d, dict) else None

    def _validate_workspace(self):
        """One-time defensive check: confirm the pinned workspace actually exists
        in the account, so a bad SESSION_BOARD_NEBULA_WORKSPACE or auth drift is
        LOGGED (distinguishing 'wrong/absent workspace' from 'genuinely 0 agents')
        rather than silently looking like an empty fleet. Never blocks polling."""
        if self._validated:
            return
        self._validated = True
        try:
            p = subprocess.run(["npx", NEBULA_CLI, "--json", "workspace", "list"],
                               stdin=subprocess.DEVNULL, capture_output=True,
                               text=True, timeout=NEBULA_TIMEOUT)
            wss = json.loads(p.stdout) if p.returncode == 0 else None
        except (subprocess.TimeoutExpired, OSError, ValueError, TypeError):
            return   # non-fatal; the pinned --workspace still applies to the fetch
        if not isinstance(wss, list):
            return
        hit = next((w for w in wss if isinstance(w, dict) and NEBULA_WORKSPACE
                    in (w.get("slug"), w.get("id"), w.get("name"))), None)
        if hit is None:
            avail = ", ".join(str(w.get("slug")) for w in wss if isinstance(w, dict))
            print("[SB] nebula WARNING: pinned workspace %r not found (available: "
                  "%s) - channels list may be empty/wrong"
                  % (NEBULA_WORKSPACE, avail), file=sys.stderr)
        elif self.verbose:
            print("[SB] nebula workspace pinned: %s (%s)"
                  % (hit.get("slug"), hit.get("id")), file=sys.stderr)


def _nebula_cli(args, timeout=NEBULA_TIMEOUT):
    """Run a nebula-ai subcommand (pinned workspace, non-interactive). Returns the
    CompletedProcess, or None on spawn/timeout failure."""
    try:
        return subprocess.run(["npx", NEBULA_CLI, "--json", "--workspace",
                               NEBULA_WORKSPACE] + list(args),
                              stdin=subprocess.DEVNULL, capture_output=True,
                              text=True, timeout=timeout)
    except (subprocess.TimeoutExpired, OSError):
        return None


def _nebula_title(text):
    """Short channel title from spoken text (first few words, capped)."""
    words = clean(text, 60).split()
    return " ".join(words[:6])[:40] or "New channel"


def nebula_send(channel_id, text):
    """Fire-and-forget: post text into a Nebula channel on a background thread.
    `chat` blocks until the agent's full reply (even --no-stream), so the daemon
    NEVER waits on it - the reply surfaces in the next channels-list poll."""
    def _run():
        p = _nebula_cli(["chat", text, "-c", channel_id, "--no-stream"],
                        timeout=NEBULA_CHAT_TIMEOUT)
        if p is None or p.returncode != 0:
            print("[SB] nebula send FAILED (chan %s): %s"
                  % (channel_id, (p.stderr[:150] if p else "spawn/timeout")),
                  file=sys.stderr)
        else:
            print("[SB] nebula sent -> %s" % channel_id, file=sys.stderr)
    threading.Thread(target=_run, daemon=True).start()


def nebula_new_channel(text):
    """Create a new Nebula channel (titled from the spoken text, default agent),
    then seed it with the message. All on a background thread, fire-and-forget."""
    def _run():
        title = _nebula_title(text)
        p = _nebula_cli(["channels", "create", "-t", title, "-a", NEBULA_DEFAULT_AGENT])
        cid = None
        if p is not None and p.returncode == 0:
            try:
                o = json.loads(p.stdout)
                cid = o.get("id") if isinstance(o, dict) else None
            except (ValueError, TypeError):
                cid = None
        if not cid:
            print("[SB] nebula new-channel create FAILED: %s"
                  % (p.stderr[:150] if p else "spawn/timeout"), file=sys.stderr)
            return
        print("[SB] nebula created channel %s (%r); seeding" % (cid, title),
              file=sys.stderr)
        p2 = _nebula_cli(["chat", text, "-c", cid, "--no-stream"],
                         timeout=NEBULA_CHAT_TIMEOUT)
        if p2 is None or p2.returncode != 0:
            print("[SB] nebula seed-send FAILED (chan %s)" % cid, file=sys.stderr)
    threading.Thread(target=_run, daemon=True).start()


def build_nebula_snapshot(records, seq):
    """Nebula fleet snapshot in the SAME hdr/s/x/end schema the firmware renders.
    Read-only: no summaries/agents/custom-names/pins/voice. Order = Nebula-pinned
    first, then status group (d/w/i), then freshest."""
    ordered = sorted(records, key=lambda r: (0 if r.get("pinned") else 1,
                                             STATUS_RANK.get(r["st"], 9), r["age"]))
    if len(ordered) > MAX_SESSIONS:
        ordered = ordered[:MAX_SESSIONS]
    n = len(ordered)
    lines = [_dump({"t": "hdr", "seq": seq, "n": n, "fleet": "nebula"})]
    for i, r in enumerate(ordered):
        nm = clean(r["nm"], NAME_MAX) or "channel"
        msg = clean(r["msg"], MSG_MAX)
        dtl = fit_detail(clean(r["msg"], 10 ** 9), DETAIL_MAX)
        srec = {"t": "s", "i": i, "id": r["sid8"], "nm": nm, "pj": "nebula",
                "st": r["st"], "age": r["age"], "msg": msg, "fi": i}  # fleet == carousel
        lines.append(_dump_s(srec))
        lines.append(_dump_x({"t": "x", "i": i, "dtl": dtl}))
    lines.append(_dump({"t": "end", "seq": seq, "n": n}))
    return lines


# ---------------------------------------------------------------------------
# Fake data
# ---------------------------------------------------------------------------


def build_fake_snapshot(seq):
    long_msg = ("Working through the migration now. " * 20).strip()
    long_msg = clean(long_msg + " " + "X" * 600)  # force a 500-char truncation
    long_dtl = clean(("Tracing the schema migration and rechecking each table. " * 20)
                     + "Z" * 400)                 # force a 700-char detail truncation
    fakes = [
        {"id": "aaaaaaaa", "nm": "migrate-db-schema", "pj": "trepov2", "st": "w", "age": 3, "msg": long_msg, "dtl": long_dtl},
        {"id": "bbbbbbbb", "nm": "recall-check-fix", "pj": "trepo-ios", "st": "d", "age": 47, "msg": "Deployed and verified live. All 33 owners scanned clean.", "dtl": "Deployed the recall-check fix and verified it live across all 33 owners with a clean scan. Confirmed the megaphone flow warms in under a second."},
        {"id": "cccccccc", "nm": "idle-scratch", "pj": "Documents", "st": "i", "age": 320, "msg": "(no transcript)", "dtl": "(no transcript)"},
        {"id": "dddddddd", "nm": "halo-firmware", "pj": "HALOMAIN-rev1p5-modular", "st": "i", "age": 1805, "msg": "Flashed rev1p5 to the ESP32; LCD renders the board.", "dtl": "Flashed rev1p5 to the ESP32 and confirmed the LCD renders the board. Left it idle after a clean end-to-end pass."},
        {"id": "eeeeeeee", "nm": "memory-index", "pj": "~", "st": "i", "age": 86000, "msg": "Updated MEMORY.md with the new session-board note.", "dtl": "Updated MEMORY.md with the new session-board note and closed out the task. Nothing pending."},
    ]
    # Same board ordering as live: done, working, idle (group only; stable input).
    fakes.sort(key=lambda f: STATUS_RANK.get(f["st"], 9))
    n = len(fakes)
    lines = [_dump({"t": "hdr", "seq": seq, "n": n, "fleet": "claude"})]
    for i, fk in enumerate(fakes):
        lines.append(_dump_s({"t": "s", "i": i, "id": fk["id"], "nm": fk["nm"],
                              "pj": fk["pj"], "st": fk["st"], "age": fk["age"], "msg": fk["msg"]}))
        lines.append(_dump_x({"t": "x", "i": i, "dtl": fk["dtl"]}))
    lines.append(_dump({"t": "end", "seq": seq, "n": n}))
    return lines


# ---------------------------------------------------------------------------
# Voice: capture -> transcribe -> deliver to the target session's mailbox
# ---------------------------------------------------------------------------


def parse_rec_start(line_bytes):
    """If line is a voice rec-start, return (sid8, mode, nbytes, rate); else None.
    mode = optional "mode" ("rename" or None). nbytes = optional "bytes" total PCM
    byte count for a spooled (post-recording) upload, letting the daemon finalize
    on byte count even if the trailing WAV_END is lost; None on the live path.
    rate = optional "rate" (PCM sample rate, e.g. 16000 when the board downsamples
    to Whisper's native rate to cut bytes); None => the legacy 44100 default."""
    if not line_bytes.startswith(b"{"):
        return None
    try:
        o = json.loads(line_bytes)
    except (ValueError, TypeError):
        return None
    if o.get("t") == "rec" and o.get("state") == "start":
        sid = o.get("id")
        if isinstance(sid, str) and sid:
            mode = o.get("mode")
            nbytes = o.get("bytes")
            nbytes = nbytes if isinstance(nbytes, int) and nbytes > 0 else None
            rate = o.get("rate")
            # only trust a sane telephony..studio rate; else fall back to default
            rate = rate if isinstance(rate, int) and 8000 <= rate <= 48000 else None
            return (sid, mode if isinstance(mode, str) else None, nbytes, rate)
    return None


def is_rec_stop(line_bytes):
    if not line_bytes.startswith(b"{"):
        return False
    try:
        o = json.loads(line_bytes)
    except (ValueError, TypeError):
        return False
    return o.get("t") == "rec" and o.get("state") == "stop"


def _pcm_start(buf):
    """Index where PCM begins (after WAV_BEGIN marker + newline + 44-byte header),
    or None if WAV_BEGIN hasn't fully arrived yet."""
    begin = buf.find(WAV_BEGIN_MARK)
    if begin == -1:
        return None
    p = begin + len(WAV_BEGIN_MARK)
    while buf[p:p + 1] in (b"\r", b"\n"):   # skip the newline(s) after the marker
        p += 1
    return p + 44                            # skip the (ignored) 44-byte header


def _frame_trim(pcm):
    return pcm[:-(len(pcm) % VOICE_WIDTH)] if len(pcm) % VOICE_WIDTH else pcm


def extract_pcm(buf, end_idx):
    """WAV_END-delimited PCM: from PCM start up to the WAV_END marker index,
    dropping the trailing newline delimiter. b"" if the framing is malformed."""
    p = _pcm_start(buf)
    if p is None or p > end_idx:
        return b""
    pcm = buf[p:end_idx]
    while pcm.endswith(b"\n") or pcm.endswith(b"\r"):  # drop the WAV_END delimiter
        pcm = pcm[:-1]
    return _frame_trim(pcm)


def extract_pcm_len(buf, nbytes):
    """Exactly `nbytes` of PCM from PCM start (spooled upload w/ a known length).
    No trailing-newline strip - `nbytes` is the exact PCM size, and a real PCM
    sample could legitimately end in 0x0A/0x0D."""
    p = _pcm_start(buf)
    if p is None:
        return b""
    return _frame_trim(buf[p:p + nbytes])


def write_wav(pcm, path, rate=VOICE_RATE):
    """Rebuild a correct 16-bit/mono WAV from raw PCM at the given sample rate
    (default 44.1kHz; the board may send 16kHz to cut bytes - Whisper resamples
    to 16kHz internally either way, so the WAV rate must just match the PCM)."""
    with wave.open(path, "wb") as w:
        w.setnchannels(VOICE_CHANNELS)
        w.setsampwidth(VOICE_WIDTH)
        w.setframerate(rate)
        w.writeframes(pcm)


def _prune_voice_dir():
    try:
        wavs = [os.path.join(VOICE_DIR, f) for f in os.listdir(VOICE_DIR)
                if f.endswith(".wav")]
    except OSError:
        return
    if len(wavs) <= VOICE_KEEP:
        return
    wavs.sort(key=lambda p: os.path.getmtime(p))
    for p in wavs[:len(wavs) - VOICE_KEEP]:
        try:
            os.remove(p)
        except OSError:
            pass


def _openai_key():
    """OPENAI_API_KEY from the environment or ~/.bashrc (never printed)."""
    k = os.environ.get("OPENAI_API_KEY")
    if k:
        return k.strip()
    try:
        with open(os.path.join(HOME, ".bashrc"), "r", encoding="utf-8") as f:
            for ln in f:
                m = re.search(r'OPENAI_API_KEY\s*=\s*["\']?([^"\'\s]+)', ln)
                if m:
                    return m.group(1).strip()
    except OSError:
        pass
    return None


def _transcribe_mlx(wav_path, initial_prompt=None):
    # kwargs passed as JSON argv so the model + decode profile are explicit.
    # condition_on_previous_text=False stops the repetition/hallucination loops
    # ("Olid Olid Olid...") that the default True produces on short/noisy clips.
    kw = {"path_or_hf_repo": WHISPER_MODEL,
          "condition_on_previous_text": False,
          "verbose": False}
    if initial_prompt:
        kw["initial_prompt"] = initial_prompt
    try:
        proc = subprocess.run(
            [WHISPER_PY, "-c",
             "import mlx_whisper,json,sys;"
             "print(json.dumps(mlx_whisper.transcribe(sys.argv[1], **json.loads(sys.argv[2]))))",
             wav_path, json.dumps(kw)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=WHISPER_TIMEOUT,
        )
    except (subprocess.TimeoutExpired, OSError):
        return None
    if proc.returncode != 0:
        return None
    try:
        return (json.loads(proc.stdout).get("text") or "").strip()
    except (ValueError, TypeError):
        return None


def _transcribe_openai(wav_path, initial_prompt=None):
    key = _openai_key()
    if not key:
        return None
    import urllib.request
    boundary = "----haloVoiceBoundary"
    try:
        with open(wav_path, "rb") as f:
            audio = f.read()
    except OSError:
        return None
    parts = []
    parts.append(("--" + boundary).encode())
    parts.append(b'Content-Disposition: form-data; name="model"')
    parts.append(b"")
    parts.append(b"whisper-1")
    if initial_prompt:                        # bias the fallback the same way
        parts.append(("--" + boundary).encode())
        parts.append(b'Content-Disposition: form-data; name="prompt"')
        parts.append(b"")
        parts.append(initial_prompt.encode("utf-8"))
    parts.append(("--" + boundary).encode())
    parts.append(b'Content-Disposition: form-data; name="file"; filename="rec.wav"')
    parts.append(b"Content-Type: audio/wav")
    parts.append(b"")
    body = b"\r\n".join(parts) + b"\r\n" + audio + \
        ("\r\n--" + boundary + "--\r\n").encode()
    req = urllib.request.Request(
        "https://api.openai.com/v1/audio/transcriptions", data=body,
        headers={"Authorization": "Bearer " + key,
                 "Content-Type": "multipart/form-data; boundary=" + boundary})
    try:
        with urllib.request.urlopen(req, timeout=WHISPER_TIMEOUT) as resp:
            return (json.loads(resp.read().decode()).get("text") or "").strip()
    except Exception:
        return None


def transcribe(wav_path, mode=None):
    """Local mlx-whisper first, OpenAI whisper-1 as fallback.
    mode selects a decode profile: "rename" biases toward a short label with a
    vocabulary hint; normal voice uses no hint. Both use base.en +
    condition_on_previous_text=False (see _transcribe_mlx). Returns the transcript
    str (may be "" if the model RAN but heard no speech), or None if every
    transcriber CRASHED (so the caller can tell "no speech" from "transcribe failed")."""
    prompt = WHISPER_RENAME_PROMPT if mode == "rename" else None
    r = _transcribe_mlx(wav_path, initial_prompt=prompt)
    if r is not None:          # mlx ran (text or empty)
        return r
    return _transcribe_openai(wav_path, initial_prompt=prompt)   # None only if it also crashed


def normalize_pcm(frames):
    """Peak-normalize 16-bit PCM so a quiet talker still clears whisper's VAD.
    Returns (frames, gain): gain 1.0 = unchanged (already loud), a factor > 1 =
    scaled toward ~28000 peak but CAPPED at VOICE_NORM_MAX_GAIN (so a low-SNR clip
    isn't over-amplified into whisper hallucinations), or 0.0 = genuinely silent
    (peak <= 200)."""
    try:
        import audioop
    except ImportError:
        return frames, 1.0
    try:
        peak = audioop.max(frames, VOICE_WIDTH)
    except audioop.error:
        return frames, 1.0
    if peak <= 200:
        return frames, 0.0            # silent -> caller uses the empty-audio path
    if peak >= 16000:
        return frames, 1.0            # already loud enough
    # Cap the boost: a clip needing >8x is almost always noise, not a quiet talker;
    # amplifying it to full scale turns that noise into hallucinated tokens. Capped,
    # a real quiet talker still gets audible while a noise clip stays quiet enough
    # that whisper honestly returns no speech.
    factor = min(28000.0 / peak, VOICE_NORM_MAX_GAIN)
    try:
        return audioop.mul(frames, VOICE_WIDTH, factor), factor
    except audioop.error:
        return frames, 1.0


def _inbox_path(sid8):
    return os.path.join(TEAMS_DIR, "session-" + sid8, "inboxes", "team-lead.json")


def _applescript_escape(s):
    """Escape a string for embedding inside an AppleScript "..." literal."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def spawn_new_session(text):
    """Open a NEW Terminal.app window running `claude <prompt>` - an interactive
    session with the transcript as its first prompt (claude starts interactive by
    default with a positional prompt). Returns (ok, err). The prompt is shell-
    quoted (arbitrary speech) then AppleScript-escaped."""
    import shlex
    import shutil
    claude = shutil.which(LLM_BIN) or shutil.which("claude")
    if not claude:
        return False, "claude not found"
    workdir = os.path.expanduser("~")
    # Shell layer: cd <dir> && <claude> <quoted-prompt>  (shlex.quote => injection-safe)
    shell_cmd = "cd %s && %s %s" % (shlex.quote(workdir), shlex.quote(claude),
                                    shlex.quote(text))
    # AppleScript layer: escape the shell command for the "..." string literal.
    applescript = ('tell application "Terminal"\n'
                   '  do script "%s"\n'
                   '  activate\n'
                   'end tell') % _applescript_escape(shell_cmd)
    try:
        r = subprocess.run(["osascript", "-e", applescript],
                           capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return False, "osascript error"
    if r.returncode != 0:
        return False, "osascript error"
    print('[SB] voice: new session launched, prompt=%r' % (text[:60],))
    return True, None


def deliver_voice(sid8, text):
    """Append a voice message to session <sid8>'s team-lead inbox.
    Returns (ok, err, msg) - msg is the written dict (for consumption polling),
    None on failure. Fails gracefully if that session has no team dir."""
    team_dir = os.path.join(TEAMS_DIR, "session-" + sid8)
    if not os.path.isdir(team_dir):
        return False, "no mailbox", None
    inbox_dir = os.path.join(team_dir, "inboxes")
    try:
        os.makedirs(inbox_dir, exist_ok=True)
    except OSError:
        return False, "no mailbox", None
    inbox = os.path.join(inbox_dir, "team-lead.json")
    msg = {
        "from": "matt-halo-voice",
        "text": "Voice message from Matt (spoken into the Halo session board): " + text,
        "summary": "Voice message from Matt via Halo",
        "timestamp": _iso_now(),
        "color": "magenta",
        "type": "message",
    }
    try:
        try:
            with open(inbox, "r", encoding="utf-8") as f:
                arr = json.load(f)
            if not isinstance(arr, list):
                arr = []
        except (OSError, ValueError):
            arr = []
        arr.append(msg)
        tmp = inbox + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(arr, f)
        os.replace(tmp, inbox)
    except OSError:
        return False, "write failed", None
    return True, None, msg


def poll_consumption(sid8, msg, timeout=None, interval=None):
    """Poll the target inbox up to `timeout`s: True once OUR message (matched by
    timestamp+text) is gone (the agent polled and consumed it), False if it is
    still sitting there unread after the window. Runs on a background thread.
    Defaults resolve the module constants at CALL time (not def time)."""
    if timeout is None:
        timeout = VOICE_CONSUME_TIMEOUT
    if interval is None:
        interval = VOICE_CONSUME_INTERVAL
    inbox = _inbox_path(sid8)
    ts, txt = msg.get("timestamp"), msg.get("text")
    deadline = time.monotonic() + timeout
    while True:
        try:
            with open(inbox, "r", encoding="utf-8") as f:
                arr = json.load(f)
        except (OSError, ValueError):
            arr = []
        present = isinstance(arr, list) and any(
            isinstance(m, dict) and m.get("timestamp") == ts and m.get("text") == txt
            for m in arr)
        if not present:
            return True                          # consumed (or inbox gone)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False                         # still unread after the window
        time.sleep(min(interval, remaining))


def _iso_now():
    import datetime
    return datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S") + "Z"


def _voice_fail(sid8, err, text="", wav=None, words=0):
    # Outcomes are ALWAYS visible (not verbose-gated) so the e2e can assert and
    # a silent failure is debuggable.
    print("[SB] voice FAILED for %s: %s" % (sid8, err))
    return {"ok": False, "words": words, "err": err, "text": text, "wav": wav}


def process_capture(buf, end_idx, sid8, verbose=False, mode=None, names=None,
                    pcm_len=None, rate=None, nebula=None):
    """Finalize a capture: write wav, transcribe, then route by mode/sid8 - spawn
    a new session (NEWSESS0), set a custom name (mode="rename"), send into a Nebula
    channel (mode="nebula_send") or start a new one (mode="nebula_new"), or deliver
    to the target Claude mailbox (normal). PCM is either WAV_END-delimited (end_idx)
    or an exact byte count (pcm_len). rate is the PCM sample rate (None => legacy
    44100). nebula = the NebulaScanner (for channel-id lookup). Logged unconditionally."""
    eff_rate = rate or VOICE_RATE
    pcm = extract_pcm_len(buf, pcm_len) if pcm_len is not None else extract_pcm(buf, end_idx)
    secs = (len(pcm) // VOICE_WIDTH) / float(eff_rate)
    if secs < VOICE_MIN_SECS:
        return _voice_fail(sid8, "empty audio")
    # Peak-normalize quiet audio so whisper's VAD doesn't drop it.
    pcm, gain = normalize_pcm(pcm)
    if gain == 0.0:
        return _voice_fail(sid8, "empty audio")   # genuinely silent
    try:
        os.makedirs(VOICE_DIR, exist_ok=True)
        wav_path = os.path.join(VOICE_DIR, "rec_%d.wav" % int(time.time() * 1000))
        write_wav(pcm, wav_path, eff_rate)        # save the normalized artifact
    except OSError:
        return _voice_fail(sid8, "wav failed")
    _prune_voice_dir()
    print("[SB] voice wav saved %s (%.1fs, gain x%.1f)" % (wav_path, secs, gain))

    text = transcribe(wav_path, mode=mode)
    if text is None:
        return _voice_fail(sid8, "transcribe failed", wav=wav_path)   # whisper crashed
    if not text.strip():
        return _voice_fail(sid8, "no speech", wav=wav_path)           # ran, heard nothing
    text = text.strip()
    if verbose:
        sys.stderr.write("[SB] voice(%s) %.1fs -> %r\n" % (sid8, secs, text))

    words = len(text.split())
    # Mode-based routes are checked FIRST so an explicit mode wins over the id: the
    # firmware reuses id="NEWSESS0" as a placeholder for nebula_new, which would
    # otherwise fall into the Claude-spawn sentinel below and spawn a Claude session.

    # Nebula new-channel gesture (fleet-background long-press): create + seed a new
    # channel. Fire-and-forget (bg thread) - reply surfaces in the next poll.
    if mode == "nebula_new":
        nebula_new_channel(text)
        print("[SB] voice: new nebula channel from %r" % (text[:40],))
        return {"ok": True, "words": words, "err": None, "text": text, "wav": wav_path,
                "sid8": sid8, "msg": None}

    # Nebula send gesture (hold-speak on a Nebula channel card): post into that
    # channel. sid8 is the board's hashed id; resolve it to the real "thrd_..." id.
    if mode == "nebula_send":
        cid = nebula.channel_id_for(sid8) if nebula is not None else None
        if not cid:
            return _voice_fail(sid8, "nebula channel not found",
                               text=text, wav=wav_path, words=words)
        nebula_send(cid, text)
        print("[SB] voice: sent to nebula channel %s" % cid)
        return {"ok": True, "words": words, "err": None, "text": text, "wav": wav_path,
                "sid8": sid8, "msg": None}

    # Rename gesture: set the persistent custom name; do NOT deliver to a mailbox.
    if mode == "rename":
        name = _clean_custom_name(text)
        if not name:
            return _voice_fail(sid8, "empty name", text=text, wav=wav_path, words=words)
        if names is not None:
            names.set(sid8, name)
        print("[SB] voice: renamed %s -> %r" % (sid8, name))
        return {"ok": True, "words": words, "err": None, "text": text, "wav": wav_path,
                "sid8": sid8, "msg": None}

    # Claude fleet-background gesture: spawn a brand-new claude session instead of
    # delivering to an existing agent's mailbox. msg stays None (no consumption
    # poll) so the caller downlinks immediately on launch.
    if sid8 == NEW_SESSION_SENTINEL:
        ok, err = spawn_new_session(text)
        if not ok:
            return _voice_fail(sid8, err, text=text, wav=wav_path, words=words)
        return {"ok": True, "words": words, "err": None, "text": text, "wav": wav_path,
                "sid8": sid8, "msg": None}

    dv = deliver_voice(sid8, text)
    ok, err, msg = dv if len(dv) == 3 else (dv[0], dv[1], None)   # tolerate old 2-tuple
    if not ok:
        return _voice_fail(sid8, err, text=text, wav=wav_path, words=words)
    print("[SB] voice delivered to %s (%d words)" % (sid8, words))
    # msg + sid8 let the caller verify the target actually CONSUMES it (vs. writing
    # to a dormant inbox that never gets polled).
    return {"ok": True, "words": words, "err": None, "text": text, "wav": wav_path,
            "sid8": sid8, "msg": msg}


# ---------------------------------------------------------------------------
# Main loop / modes
# ---------------------------------------------------------------------------


def _count_s(lines):
    """Number of session ('s') lines in a snapshot (for verbose logging)."""
    return sum(1 for ln in lines if ln.startswith('{"t":"s"'))


def _now_wall():
    return time.time()


def _now_mono():
    return time.monotonic()


def _snapshot_lines(args, seq, summaries, order, agents, names, positions=None,
                    view_state=None, nebula=None, firstseen=None):
    """Build one snapshot's protocol lines for the current session state."""
    if args.fake:
        return build_fake_snapshot(seq)
    fleet = view_state["fleet"] if view_state else "claude"
    if fleet == "nebula" and nebula is not None:
        return build_nebula_snapshot(nebula.poll(), seq)   # on-demand poll, throttled
    sessions = read_sessions(_now_wall())
    return build_snapshot(
        sessions, seq, _now_wall(),
        summaries=summaries, order=order, agents=agents, names=names,
        positions=positions, firstseen=firstseen,
        log_warn=lambda m: print("[SB] warn:", m, file=sys.stderr),
    )


def run_once(args, summaries, order, agents, names, positions, firstseen, seq):
    want_nebula = getattr(args, "nebula", False)
    view_state = {"fleet": "nebula" if want_nebula else "claude"}
    nebula = NebulaScanner(args.verbose) if want_nebula else None
    if not args.fake and summaries is not None and not want_nebula:
        # Prime summaries synchronously so the single snapshot has LLM titles.
        now = _now_wall()
        sessions = read_sessions(now)
        counts = agents.scan() if agents is not None else {}
        jobs = []
        for s in sessions:
            ra, ru, verdict = analyze_session(s.get("sessionId", ""), s.get("cwd", ""))
            aga = counts.get(s.get("sessionId", ""), (0, 0))[1]
            st, sclass = derive_status(s, verdict, s.get("_transcript_mtime"), now,
                                       agent_active=aga > 0)
            jobs.append((s, ra, ru, sclass))
        summaries.prime_sync(jobs)
    lines = _snapshot_lines(args, 1 if args.fake else seq, summaries, order, agents,
                            names, positions, view_state, nebula, firstseen)
    sys.stdout.write("\n".join(lines) + "\n")
    sys.stdout.flush()
    if args.verbose:
        print("[SB] snapshot seq=%d n=%d" % (seq, _count_s(lines)), file=sys.stderr)


def parse_refresh(line_bytes):
    """True if an incoming line is a firmware refresh request."""
    if not line_bytes.startswith(b"{"):
        return False
    try:
        return json.loads(line_bytes).get("t") == "refresh"
    except (ValueError, TypeError):
        return False


def is_wificreds(line_bytes):
    """True if an incoming line requests Wi-Fi credentials (serial-only path)."""
    if not line_bytes.startswith(b"{"):
        return False
    try:
        return json.loads(line_bytes).get("t") == "wificreds"
    except (ValueError, TypeError):
        return False


# ---------------------------------------------------------------------------
# Network: Wi-Fi provisioning, firewall check, UDP discovery beacon
# ---------------------------------------------------------------------------


def _current_ssid():
    """Current Wi-Fi SSID; '' if unknown. Tries networksetup's specific
    'Current Wi-Fi Network:' line, then `ipconfig getsummary` (which still works
    when newer macOS blocks networksetup behind Location Services)."""
    for iface in ("en0", "en1"):
        try:
            out = subprocess.run(["networksetup", "-getairportnetwork", iface],
                                 capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            out = ""
        m = re.search(r"Current (?:Wi-Fi|AirPort) Network:\s*(.+?)\s*$", out, re.M)
        if m:
            return m.group(1).strip()
        try:
            out = subprocess.run(["ipconfig", "getsummary", iface],
                                 capture_output=True, text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            out = ""
        m = re.search(r"^\s*SSID\s*:\s*(.+?)\s*$", out, re.M)
        if m:
            return m.group(1).strip()
    return ""


def _wifi_password(ssid):
    """Wi-Fi password from Keychain, or '' on timeout/prompt/failure. Never logged."""
    if not ssid:
        return ""
    try:
        r = subprocess.run(
            ["security", "find-generic-password", "-D", "AirPort network password",
             "-a", ssid, "-w"],
            capture_output=True, text=True, timeout=10)
    except (subprocess.TimeoutExpired, OSError, subprocess.SubprocessError):
        return ""            # a Keychain GUI prompt hung past 10s
    if r.returncode != 0:
        return ""
    return r.stdout.strip()


def wifi_creds():
    """{"t":"wifi","ssid":..,"pwd":..} for the board to join. Serves the configured
    creds (WIFI_SSID/WIFI_PWD, env-overridable); falls back to the current SSID and
    the Keychain password only when the configured value is blank. Never logged."""
    ssid = WIFI_SSID or _current_ssid()
    pwd = WIFI_PWD or _wifi_password(ssid)
    return {"t": "wifi", "ssid": ssid, "pwd": pwd}


def firewall_state():
    """macOS application-firewall global state (0 off, 1 on, 2 block-all), or None.
    Tries the classic alf plist, then socketfilterfw (newer macOS drops the plist)."""
    try:
        out = subprocess.run(
            ["defaults", "read", "/Library/Preferences/com.apple.alf", "globalstate"],
            capture_output=True, text=True, timeout=5)
        if out.returncode == 0:
            return int(out.stdout.strip())
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    try:
        out = subprocess.run(
            ["/usr/libexec/ApplicationFirewall/socketfilterfw", "--getglobalstate"],
            capture_output=True, text=True, timeout=5).stdout
        m = re.search(r"State\s*=\s*(\d+)", out)   # "Firewall is enabled. (State = 1)"
        if m:
            return int(m.group(1))
    except (OSError, subprocess.SubprocessError):
        pass
    return None


def check_firewall_and_warn(tcp_port):
    st = firewall_state()
    if st and st != 0:
        print("[SB] *** WARNING: macOS application firewall is ON (globalstate=%s). "
              "It can block the incoming TCP %s device connection, and a permission "
              "prompt left unclicked overnight will silently block us. Allow "
              "'python'/'Python' in System Settings > Network > Firewall, or turn the "
              "firewall off for the LAN test. ***" % (st, tcp_port), file=sys.stderr)
    return st


def load_or_create_token():
    """Read the persisted auth token, or create one (secrets.token_hex(16), 0600)."""
    try:
        with open(SECRET_FILE, "r", encoding="utf-8") as f:
            tok = f.read().strip()
        if tok:
            return tok
    except OSError:
        pass
    import secrets
    tok = secrets.token_hex(16)
    try:
        with open(SECRET_FILE, "w", encoding="utf-8") as f:
            f.write(tok)
        os.chmod(SECRET_FILE, 0o600)
    except OSError:
        pass
    return tok


class NgrokTunnel:
    """Manages an `ngrok tcp <port>` subprocess and its public host:port, so the
    board can reach the daemon from a different network. Degrades to LAN-only if
    ngrok is missing/fails; restarts once if it dies; killed on daemon exit."""

    def __init__(self, port):
        self.port = port
        self.proc = None
        self.public = None        # (host, port) or None
        self._restarted = False

    def _spawn(self):
        binp = shutil.which(NGROK_BIN)
        if not binp:
            print("[SB] ngrok not found; remote access OFF, LAN-only", file=sys.stderr)
            return False
        try:
            self.proc = subprocess.Popen(
                [binp, "tcp", str(self.port)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            print("[SB] ngrok launch failed (%r); LAN-only" % e, file=sys.stderr)
            self.proc = None
            return False
        return True

    def _query_public(self):
        try:
            import urllib.request
            with urllib.request.urlopen(NGROK_API, timeout=2) as r:
                data = json.load(r)
        except Exception:
            return None
        for t in data.get("tunnels", []):
            url = t.get("public_url", "")
            if url.startswith("tcp://"):
                hp = url[len("tcp://"):]
                if ":" in hp:
                    host, p = hp.rsplit(":", 1)
                    try:
                        return (host, int(p))
                    except ValueError:
                        pass
        return None

    def start_and_resolve(self, timeout=25):
        """Spawn ngrok and poll its local API until the public address is up.
        Returns (host, port) or None. Runs on a background thread at startup."""
        if not self._spawn():
            return None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            hp = self._query_public()
            if hp:
                self.public = hp
                print("[SB] ngrok tunnel: %s:%d" % hp, file=sys.stderr)
                return hp
            if self.proc is not None and self.proc.poll() is not None:
                print("[SB] ngrok exited before publishing a tunnel; LAN-only",
                      file=sys.stderr)
                return None
            time.sleep(0.5)
        print("[SB] ngrok tunnel did not come up in %ds; LAN-only" % timeout,
              file=sys.stderr)
        return None

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.proc is not None:
            try:
                self.proc.terminate()
                self.proc.wait(timeout=3)
            except Exception:
                try:
                    self.proc.kill()
                except Exception:
                    pass
            self.proc = None


def beacon_loop(tcp_port, stop):
    """Broadcast 'SBHALO1 <tcp_port>' on UDP BEACON_PORT every BEACON_INTERVAL.
    The device learns the Mac's IP from the packet source address."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    except OSError as e:
        print("[SB] beacon socket failed: %r" % e, file=sys.stderr)
        return
    payload = ("%s %d" % (BEACON_MAGIC, tcp_port)).encode("ascii")
    while not stop["flag"]:
        try:
            s.sendto(payload, ("255.255.255.255", BEACON_PORT))
        except OSError:
            pass
        for _ in range(int(BEACON_INTERVAL * 10)):
            if stop["flag"]:
                break
            time.sleep(0.1)
    try:
        s.close()
    except OSError:
        pass


# ---------------------------------------------------------------------------
# Transports: identical line/binary protocol over pyserial OR a TCP socket
# ---------------------------------------------------------------------------


class SocketTransport:
    kind = "tcp"

    def __init__(self, sock, peer):
        self.sock = sock
        self.peer = peer
        sock.setblocking(False)
        for opt in ((socket.IPPROTO_TCP, socket.TCP_NODELAY),
                    (socket.SOL_SOCKET, socket.SO_KEEPALIVE)):
            try:
                sock.setsockopt(opt[0], opt[1], 1)
            except OSError:
                pass
        try:                              # roomy recv buffer to absorb PCM bursts
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
        except OSError:
            pass

    def fileno(self):
        return self.sock.fileno()

    def read(self):
        """Drain the socket COMPLETELY (loop recv until EAGAIN) so a fast PCM
        stream never backs up the kernel buffer / closes the TCP window."""
        chunks = []
        while True:
            try:
                data = self.sock.recv(262144)
            except BlockingIOError:
                break                     # kernel buffer emptied for now
            except (ConnectionError, OSError):
                if chunks:
                    break                 # return what we have; next read raises
                raise
            if data == b"":               # peer closed
                if not chunks:
                    raise ConnectionError("peer closed")
                break
            chunks.append(data)
        return b"".join(chunks) if chunks else b""

    def write(self, data):
        # small payloads; send fully then restore non-blocking for select-based reads
        self.sock.setblocking(True)
        try:
            self.sock.sendall(data)
        finally:
            try:
                self.sock.setblocking(False)
            except OSError:
                pass

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class SerialTransport:
    kind = "serial"

    def __init__(self, ser):
        self.ser = ser

    def fileno(self):
        return self.ser.fileno()

    def read(self):
        n = self.ser.in_waiting
        return self.ser.read(n if n else 1)

    def write(self, data):
        self.ser.write(data)
        self.ser.flush()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass


class ProtocolLink:
    """One protocol connection over a transport: partial-line rx buffering plus the
    binary voice-capture state machine, identical across serial and TCP."""

    def __init__(self, transport, verbose=False, token=None, auth_required=False,
                 names=None, positions=None, view_state=None, nebula=None):
        self.tp = transport
        self.verbose = verbose
        self.names = names        # NameStore for the rename gesture
        self.positions = positions  # PositionStore for the manual-reorder gesture
        self.view_state = view_state  # {"fleet": "claude"|"nebula"} shared w/ main loop
        self.nebula = nebula          # NebulaScanner (for voice-to-channel sends)
        self.rx = b""
        self.capture = None       # dict while in binary voice-capture mode
        self._wlock = threading.Lock()   # main loop + async verifier share the transport
        self._token = token
        self.auth_required = auth_required
        self.authed = not auth_required   # serial is trusted; TCP must present the token
        self._conn_at = _now_mono()
        self._cfg_pushed = -1     # last remotecfg version pushed to this link
        self._bootstrap = False   # grace-accepted a tokenless LAN device (first boot)
        # A remote (ngrok) connection is forwarded from localhost; a genuine LAN
        # device shows its own private IP. Only LAN peers may bootstrap tokenless.
        peer = getattr(transport, "peer", None)
        host = peer[0] if isinstance(peer, tuple) and peer else None
        self._lan_peer = bool(host) and host not in ("127.0.0.1", "::1", "localhost")

    def fileno(self):
        return self.tp.fileno()

    @property
    def kind(self):
        return self.tp.kind

    def capturing(self):
        return self.capture is not None

    def ready(self):
        """True once the link may receive pushes/snapshots (authed if required)."""
        return self.authed

    def auth_expired(self):
        return (self.auth_required and not self.authed
                and (_now_mono() - self._conn_at) > AUTH_TIMEOUT)

    def _peer(self):
        return getattr(self.tp, "peer", self.kind)

    def send_snapshot(self, lines):
        """Write a snapshot. Returns True if sent, False if paused (capture) or
        not yet authed. Snapshots pause on THIS link during its capture so
        outbound frames never interleave with the device's inbound PCM upload."""
        if self.capture is not None or not self.authed:
            return False
        with self._wlock:
            self.tp.write(("\n".join(lines) + "\n").encode("utf-8"))
        return True

    def send_obj(self, obj):
        with self._wlock:
            self.tp.write((_dump(obj) + "\n").encode("utf-8"))

    def _end_capture(self, reason):
        """Clear capture state, logging the reason + bytes UNCONDITIONALLY. Every
        exit from capture mode goes through here so none is ever silent."""
        got = len(self.capture["buf"]) if self.capture is not None else 0
        print("[SB] voice capture end (%s): %s got=%d" % (self.kind, reason, got),
              file=sys.stderr)
        self.capture = None

    def capture_tick(self):
        """Per-loop capture maintenance (called every ~0.2s regardless of whether
        data arrived, so a STUCK capture is never silent): a progress-STALL
        watchdog, a 5s heartbeat, and the VOICE_CAP_SECS hard cap. Returns True if
        the capture was aborted (so the loop resumes snapshots on this link now)."""
        if self.capture is None:
            return False
        now = _now_mono()
        # Growth tracking for the stall watchdog.
        cur = len(self.capture["buf"])
        if cur != self.capture.get("last_len"):
            self.capture["last_len"] = cur
            self.capture["grow"] = now
        # Progress-STALL: WAV_END was likely dropped by the RF path; abandon fast
        # so the link doesn't sit frozen (snapshots paused) until the 200s cap.
        if (now - self.capture.get("grow", self.capture["start"])) > VOICE_STALL_SECS:
            self.send_obj({"t": "sent", "ok": False, "err": "link stalled"})
            self._end_capture("stalled")
            self.rx = b""
            return True
        if (now - self.capture["start"]) > VOICE_CAP_SECS:
            self.send_obj({"t": "sent", "ok": False, "err": "timeout"})
            self._end_capture("timeout")
            self.rx = b""
            return True
        last_log = self.capture.get("log", self.capture["start"])
        if now - last_log >= CAPTURE_HB_SECS:
            self.capture["log"] = now
            got = len(self.capture["buf"])
            dt = now - last_log
            dbytes = got - self.capture.get("log_len", 0)
            self.capture["log_len"] = got
            # gap = seconds since the buffer last grew (a live receive would be ~0;
            # a rising gap under an active upload is the tunnel/uplink stalling).
            gap = now - self.capture.get("grow", self.capture["start"])
            print("[SB] voice capturing (%s): got=%d bytes, %.0fs elapsed "
                  "(+%d B, %.1f KB/s, gap=%.1fs)"
                  % (self.kind, got, now - self.capture["start"],
                     dbytes, (dbytes / dt / 1024.0) if dt > 0 else 0.0, gap),
                  file=sys.stderr)
        return False

    def _scan_capture(self):
        """Finalize the capture when the clip is complete - by WAV_END marker, OR
        (spooled upload with a known length) once `want` PCM bytes have arrived,
        so a lost/corrupted trailing WAV_END over the tunnel can't strand it.
        Whichever completes first wins. Returns "resend" on completion, else None."""
        buf = self.capture["buf"]
        idx = buf.find(WAV_END_MARK, self.capture["scan"])
        want = self.capture.get("want")
        by_len = False
        if idx == -1:
            p = _pcm_start(buf) if want else None      # length-based completion?
            if p is not None and (len(buf) - p) >= want:
                by_len = True
            else:
                # progress/timeout handled by capture_tick() on the loop timer
                self.capture["scan"] = max(0, len(buf) - len(WAV_END_MARK))
                return None
        if by_len:
            p = _pcm_start(buf)
            res = process_capture(buf, 0, self.capture["sid8"], verbose=self.verbose,
                                  mode=self.capture.get("mode"), names=self.names,
                                  pcm_len=want, rate=self.capture.get("rate"),
                                  nebula=self.nebula)
            trailing = bytes(buf[p + want:])
            reason = "bytes=%d ok=%s" % (want, res["ok"])
        else:
            res = process_capture(buf, idx, self.capture["sid8"], verbose=self.verbose,
                                  mode=self.capture.get("mode"), names=self.names,
                                  rate=self.capture.get("rate"), nebula=self.nebula)
            trailing = bytes(buf[idx + len(WAV_END_MARK):])
            reason = "wav_end ok=%s" % res["ok"]
        if res["ok"] and res.get("msg") is not None:
            # Delivered to the inbox; confirm the target actually CONSUMES it
            # (dormant terminals never poll). Verify async -> downlink later.
            self._verify_consumption_async(res["sid8"], res["msg"], res["words"])
        else:
            self.send_obj({"t": "sent", "ok": res["ok"], "words": res["words"]}
                          if res["ok"] else
                          {"t": "sent", "ok": False, "err": res["err"]})
        self.rx = trailing
        self._end_capture(reason)
        return "resend"

    def _auth_result(self, line):
        """"ok" (token matches), "bootstrap" (empty token from a LAN device on
        first boot - grace-accept so we can push it the token), or "reject"."""
        try:
            o = json.loads(line)
        except (ValueError, TypeError):
            return "reject"
        if o.get("t") != "auth":
            return "reject"
        tok = o.get("tok")
        if tok and tok == self._token:
            return "ok"
        if (not tok) and self._lan_peer:        # tokenless LAN first-boot bootstrap
            return "bootstrap"
        return "reject"                         # wrong token, or tokenless remote

    def feed(self, data):
        """Process incoming bytes. Returns "reject" to drop the link (bad auth),
        "resend" to request a fresh snapshot, else None. On an auth-required (TCP)
        link the FIRST line must be a matching {"t":"auth","tok":...}; nothing else
        is processed until then."""
        # ---- auth gate (TCP): first line must present the token ----
        if self.auth_required and not self.authed:
            self.rx += data
            if b"\n" not in self.rx:
                if len(self.rx) > 4096:            # no newline + huge => abusive
                    print("[SB] auth failed from %s, closing" % (self._peer(),),
                          file=sys.stderr)
                    return "reject"
                return None
            line, self.rx = self.rx.split(b"\n", 1)
            res = self._auth_result(line.strip())
            if res == "reject":
                print("[SB] auth failed from %s, closing" % (self._peer(),),
                      file=sys.stderr)
                return "reject"
            self.authed = True
            if res == "bootstrap":
                self._bootstrap = True
                print("[SB] auth BOOTSTRAP (tokenless LAN) from %s; provisioning "
                      "token via remotecfg" % (self._peer(),), file=sys.stderr)
            elif self.verbose:
                print("[SB] auth ok from %s" % (self._peer(),), file=sys.stderr)
            data = b""                             # remainder already in self.rx
            resend_after_auth = "resend"
        else:
            resend_after_auth = None

        if self.capture is not None:
            self.capture["buf"] += data     # bytearray in-place extend (O(1))
            return self._scan_capture() or resend_after_auth

        self.rx += data
        resend = resend_after_auth
        while b"\n" in self.rx:
            line, rest = self.rx.split(b"\n", 1)
            rec = parse_rec_start(line.strip())
            if rec:
                sid8, mode, nbytes, rate = rec
                # UNCONDITIONAL (not verbose-gated): capture start must always be
                # visible so its matching end line can be found in the log.
                print("[SB] voice capture start (%s) sid=%s mode=%s bytes=%s rate=%s; "
                      "snapshots paused on this link"
                      % (self.kind, sid8, mode or "normal", nbytes or "-",
                         rate or VOICE_RATE),
                      file=sys.stderr)
                _t = _now_mono()
                self.capture = {"buf": bytearray(rest), "scan": 0, "start": _t,
                                "sid8": sid8, "mode": mode, "want": nbytes,
                                "rate": rate, "grow": _t, "last_len": len(rest)}
                self.rx = b""
                # Scan immediately: WAV_END may already be in `rest` (whole
                # recording arrived in one read).
                return self._scan_capture() or resend
            self.rx = rest
            line = line.strip()
            if not line:
                continue
            if parse_refresh(line):
                resend = "resend"
                if self.verbose:
                    print("[SB] refresh requested (%s)" % self.kind, file=sys.stderr)
            elif parse_pos(line) is not None:
                sid8, idx = parse_pos(line)
                if self.positions is not None:
                    if idx < 0:
                        self.positions.clear(sid8)
                        print("[SB] unpinned %s" % sid8, file=sys.stderr)
                    else:
                        self.positions.set(sid8, idx)
                        print("[SB] pinned %s -> row %d" % (sid8, idx), file=sys.stderr)
                    self.send_obj({"t": "pos", "ok": True, "id": sid8, "idx": idx})
                    resend = "resend"       # reflect the new order immediately
            elif parse_view(line) is not None:
                fleet = parse_view(line)
                if self.view_state is not None:
                    self.view_state["fleet"] = fleet
                    print("[SB] view -> %s fleet" % fleet, file=sys.stderr)
                    self.send_obj({"t": "view", "ok": True, "fleet": fleet})
                    resend = "resend"       # switch the board to the new fleet now
            elif is_rec_stop(line):
                pass                    # stray stop (already finalized)
            elif line == WAV_END_MARK:
                pass                    # trailing WAV_END after a byte-count finalize
            elif is_wificreds(line) and self.kind == "serial":
                self._reply_wifi()
            else:
                print("[FW] " + line.decode("utf-8", "replace"))
        if len(self.rx) > 8192:          # guard against a never-terminated line
            self.rx = self.rx[-8192:]
        return resend

    def _reply_wifi(self):
        creds = wifi_creds()
        self.send_obj(creds)             # password goes to the device, NEVER to logs
        print("[SB] sent wifi creds for ssid %r (pwd %s)" % (
            creds.get("ssid") or "?",
            "set" if creds.get("pwd") else "EMPTY - Matt must provision manually"),
            file=sys.stderr)

    def _verify_consumption_async(self, sid8, msg, words):
        """Background: poll the target inbox for real consumption, then downlink
        {ok:true} if the agent picked it up, or {ok:true, unread:true} if it wrote
        fine but the (dormant) session never polled. Never blocks the loop."""
        def worker():
            consumed = poll_consumption(sid8, msg)
            dl = {"t": "sent", "ok": True, "words": words}
            if consumed:
                print("[SB] voice consumed by %s" % sid8, file=sys.stderr)
            else:
                dl["unread"] = True
                print("[SB] voice UNREAD by %s after %ds (dormant?)"
                      % (sid8, VOICE_CONSUME_TIMEOUT), file=sys.stderr)
            try:
                self.send_obj(dl)        # link may be gone; harmless if it fails
            except Exception:
                pass
        threading.Thread(target=worker, daemon=True).start()


def run_daemon(args, summaries, order, agents, names, positions, view_state, nebula,
               firstseen):
    """Dual-transport controller: serial (pause-gated) AND TCP server live at once,
    plus the UDP discovery beacon. One snapshot per interval fans out to every
    connected transport; input (acks/refresh/voice) is processed from each."""
    import serial  # lazy: only needed here

    stop = {"flag": False}

    def _sigint(signum, frame):
        stop["flag"] = True
    try:
        signal.signal(signal.SIGINT, _sigint)
        signal.signal(signal.SIGTERM, _sigint)
    except ValueError:
        pass

    seq = {"n": 0}
    last_send = [-1e9]
    serial_link = [None]
    serial_retry_at = [0.0]
    tcp_link = [None]     # the active (authed, or awaiting-first-auth) TCP device
    tcp_pend = [None]     # a NEWCOMER held aside while an authed link is active; it
                          # only replaces the active link once IT authenticates, so a
                          # stray ngrok forward can't knock the working device offline
    token = load_or_create_token()
    remote = {"host": None, "port": None, "ver": 0}   # ngrok addr + push version
    tunnel = None
    tunnel_check_at = [0.0]
    print("[SB] auth token loaded from %s (TCP requires it)" % SECRET_FILE, file=sys.stderr)

    if args.listen:
        check_firewall_and_warn(args.listen)

    tcp_srv = None
    if args.listen:
        try:
            tcp_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            tcp_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            tcp_srv.bind(("0.0.0.0", args.listen))
            tcp_srv.listen(1)
            tcp_srv.setblocking(False)
            print("[SB] TCP listening on 0.0.0.0:%d" % args.listen, file=sys.stderr)
        except OSError as e:
            print("[SB] TCP listen failed on %d: %r" % (args.listen, e), file=sys.stderr)
            tcp_srv = None

    if args.listen and args.beacon:
        threading.Thread(target=beacon_loop, args=(args.listen, stop), daemon=True).start()
        print("[SB] UDP beacon '%s %d' on :%d every %.0fs"
              % (BEACON_MAGIC, args.listen, BEACON_PORT, BEACON_INTERVAL), file=sys.stderr)

    # ngrok TCP tunnel (cross-network reach). Resolve in the background so the
    # daemon serves LAN immediately; the public address pushes to firmware later.
    if args.listen and tcp_srv is not None and args.tunnel:
        tunnel = NgrokTunnel(args.listen)

        def _tunnel_up():
            hp = tunnel.start_and_resolve()
            if hp:
                remote["host"], remote["port"] = hp
                remote["ver"] += 1
        threading.Thread(target=_tunnel_up, daemon=True).start()

    def links():
        # "Active" links that receive snapshots/pushes: serial + the active TCP.
        return [l for l in (serial_link[0], tcp_link[0]) if l is not None]

    def conns():
        # ALL live connections to poll/feed/expire (includes the pending newcomer).
        return [l for l in (serial_link[0], tcp_link[0], tcp_pend[0]) if l is not None]

    def remotecfg():
        c = {"t": "remotecfg", "tok": token}     # token always (needed for LAN auth too)
        if remote["host"]:
            c["host"], c["port"] = remote["host"], remote["port"]
        return c

    def drop(l):
        if l is not None and l.capturing():      # never let a capture end silently
            l._end_capture("link_dropped")
        try:
            l.tp.close()
        except Exception:
            pass
        if l is serial_link[0]:
            serial_link[0] = None
            serial_retry_at[0] = _now_mono() + 2.0
        if l is tcp_link[0]:
            tcp_link[0] = None
        if l is tcp_pend[0]:
            tcp_pend[0] = None

    def send_all():
        seq["n"] += 1
        lines = _snapshot_lines(args, seq["n"], summaries, order, agents, names,
                                positions, view_state, nebula, firstseen)
        sent = 0
        for l in list(links()):
            try:
                if l.send_snapshot(lines):   # False == paused mid-capture
                    sent += 1
            except (OSError, serial.SerialException, ConnectionError):
                print("[SB] %s link lost on send" % l.kind, file=sys.stderr)
                drop(l)
        last_send[0] = _now_mono()
        if args.verbose:
            # count links actually written (a capturing link is paused, not counted)
            print("[SB] sent seq=%d n=%d -> %d/%d link(s) (rest paused capturing)"
                  % (seq["n"], _count_s(lines), sent, len(links())), file=sys.stderr)

    def forward_cmds():
        ls = links()
        if not ls:
            return
        try:
            with open(CMD_FILE, "r", encoding="utf-8") as f:
                content = f.read()
        except OSError:
            return
        if not content.strip():
            return
        items = [ln.strip() for ln in content.split("\n") if ln.strip()]
        capturing = [l for l in ls if l.capturing()]
        if capturing:
            # mid-capture: only recstop may pass (to the capturing link); hold rest
            fwd = [ln for ln in items if "recstop" in ln]
            if not fwd:
                return
            hold = [ln for ln in items if "recstop" not in ln]
            targets = capturing
        else:
            fwd, hold, targets = items, [], ls
        for ln in fwd:
            for l in targets:
                try:
                    l.tp.write((ln + "\n").encode("utf-8"))
                except (OSError, serial.SerialException, ConnectionError):
                    drop(l)
            print("[SB] fwd: " + ln, file=sys.stderr)
        try:
            with open(CMD_FILE, "w", encoding="utf-8") as f:
                if hold:
                    f.write("\n".join(hold) + "\n")
        except OSError:
            pass

    def push_cfg():
        """Push the current remotecfg (token + ngrok addr) to any ready link that
        hasn't seen this version - a newly-ready link, or all links after the
        tunnel address changes. Serial gets it on connect; TCP after it auths."""
        ver = remote["ver"]
        for l in list(links()):
            if l.ready() and l._cfg_pushed < ver:
                try:
                    l.send_obj(remotecfg())
                    l._cfg_pushed = ver
                    print("[SB] pushed remotecfg to %s (host=%s port=%s)"
                          % (l.kind, remote["host"], remote["port"]), file=sys.stderr)
                except (OSError, serial.SerialException, ConnectionError):
                    drop(l)

    def monitor_tunnel():
        if tunnel is None or _now_mono() < tunnel_check_at[0]:
            return
        tunnel_check_at[0] = _now_mono() + 5.0
        if not tunnel.alive() and not tunnel._restarted:
            tunnel._restarted = True
            print("[SB] ngrok died; attempting one restart", file=sys.stderr)

            def _restart():
                hp = tunnel.start_and_resolve()
                if hp:
                    remote["host"], remote["port"] = hp
                    remote["ver"] += 1     # re-push new addr to firmware
                else:
                    print("[SB] ngrok restart failed; degraded to LAN-only",
                          file=sys.stderr)
            threading.Thread(target=_restart, daemon=True).start()

    try:
        while not stop["flag"]:
            paused = os.path.exists(PAUSE_FILE)
            # Serial lifecycle - pause-file gates ONLY serial (flashing); TCP lives on.
            if paused:
                if serial_link[0] is not None:
                    print("[SB] serial paused (pause file); TCP unaffected", file=sys.stderr)
                    drop(serial_link[0])
            elif serial_link[0] is None and args.serial and _now_mono() >= serial_retry_at[0]:
                try:
                    ser = serial.Serial(args.port, 115200, timeout=0.1, write_timeout=2)
                    ser.reset_input_buffer()
                    serial_link[0] = ProtocolLink(SerialTransport(ser), args.verbose,
                                                  token=token, auth_required=False,
                                                  names=names, positions=positions,
                                                  view_state=view_state, nebula=nebula)
                    last_send[0] = -1e9
                    print("[SB] serial connected %s" % args.port, file=sys.stderr)
                except (serial.SerialException, OSError):
                    serial_retry_at[0] = _now_mono() + 2.0   # quiet retry

            # Build the readable set: TCP listener + every live connection.
            watch = ([tcp_srv] if tcp_srv is not None else []) + conns()
            try:
                ready, _, _ = select.select(watch, [], [], 0.2)
            except (OSError, ValueError):
                ready = []                 # a closed fd slipped in; rebuild next loop

            resend = False
            for obj in ready:
                if obj is tcp_srv:
                    try:
                        conn, peer = tcp_srv.accept()
                    except OSError:
                        continue
                    pl = ProtocolLink(SocketTransport(conn, peer), args.verbose,
                                      token=token, auth_required=True, names=names,
                                      positions=positions, view_state=view_state,
                                      nebula=nebula)
                    if tcp_link[0] is not None and tcp_link[0].authed:
                        # A device is authed and working. Hold the newcomer aside; it
                        # only takes over once IT authenticates. This stops a stray
                        # ngrok forward from knocking the live device offline.
                        if tcp_pend[0] is not None:
                            drop(tcp_pend[0])       # at most one candidate
                        tcp_pend[0] = pl
                        print("[SB] TCP connect from %s (awaiting auth; active link "
                              "kept)" % (peer,), file=sys.stderr)
                    else:
                        # No authed active link -> the newcomer becomes the active one.
                        if tcp_link[0] is not None:
                            drop(tcp_link[0])
                        tcp_link[0] = pl
                        last_send[0] = -1e9          # fresh snapshot once it authenticates
                        print("[SB] TCP connect from %s (awaiting auth)" % (peer,),
                              file=sys.stderr)
                    continue
                l = obj
                try:
                    data = l.tp.read()
                except (OSError, serial.SerialException, ConnectionError):
                    print("[SB] %s link lost" % l.kind, file=sys.stderr)
                    drop(l)
                    continue
                if data:
                    sig = l.feed(data)
                    if sig == "reject":         # bad/absent auth -> drop (already logged)
                        drop(l)
                        continue
                    if sig == "resend":
                        resend = True
                    # A pending newcomer that just authed promotes to the active link.
                    if l is tcp_pend[0] and l.authed:
                        if tcp_link[0] is not None:
                            print("[SB] TCP promoting newly-authed %s (dropping old link)"
                                  % (l._peer(),), file=sys.stderr)
                            drop(tcp_link[0])
                        tcp_link[0] = tcp_pend[0]
                        tcp_pend[0] = None
                        last_send[0] = -1e9

            forward_cmds()
            push_cfg()                          # token/remote addr to ready links
            monitor_tunnel()
            capturing_any = False
            for l in conns():
                if l.auth_expired():
                    print("[SB] auth timeout from %s, closing" % (l._peer(),), file=sys.stderr)
                    drop(l)
                    continue
                if l in links() and l.capturing():
                    capturing_any = True
                if l.capture_tick():    # heartbeat + timeout, every loop tick
                    resend = True

            # While ANY link is capturing, do NOT build a snapshot: build_snapshot
            # reads ~10 transcripts + runs `ps`, blocking the read loop for 100s of
            # ms - long enough to fill the socket buffer and drop PCM. Defer it; the
            # capture-end "resend" fires a fresh snapshot the moment capture ends.
            if resend or (not capturing_any and (_now_mono() - last_send[0]) >= args.interval):
                send_all()
            elif capturing_any:
                last_send[0] = _now_mono()   # keep the timer fresh across the capture
    finally:
        for l in list(conns()):
            try:
                l.tp.close()
            except Exception:
                pass
        if tcp_srv is not None:
            try:
                tcp_srv.close()
            except OSError:
                pass
        if tunnel is not None:               # never orphan the ngrok subprocess
            tunnel.stop()
        print("[SB] exit", file=sys.stderr)


def parse_args(argv):
    default_port = os.environ.get("SESSION_BOARD_PORT", "/dev/cu.usbmodem101")
    p = argparse.ArgumentParser(description="Halo LCD Claude Session Board daemon")
    p.add_argument("--port", default=default_port, help="serial port (env SESSION_BOARD_PORT)")
    p.add_argument("--interval", type=float, default=2.0, help="snapshot interval seconds")
    p.add_argument("--once", action="store_true", help="print one snapshot to stdout and exit")
    p.add_argument("--fake", action="store_true", help="emit 5 synthetic sessions")
    p.add_argument("--no-llm", action="store_true",
                   help="skip LLM summaries; use raw registry name + reply text")
    p.add_argument("--listen", type=int, default=DEFAULT_TCP_PORT,
                   help="TCP server port for standalone-over-WiFi (0 disables)")
    p.add_argument("--no-serial", dest="serial", action="store_false",
                   help="disable the serial transport (TCP only)")
    p.add_argument("--no-beacon", dest="beacon", action="store_false",
                   help="disable the UDP discovery beacon")
    p.add_argument("--no-tunnel", dest="tunnel", action="store_false",
                   help="disable the ngrok cross-network tunnel (LAN-only)")
    p.add_argument("--verbose", action="store_true", help="per-cycle summary to stderr")
    p.add_argument("--nebula", action="store_true",
                   help="with --once: emit the Nebula.gg fleet snapshot instead of Claude")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])
    # Fake mode carries its own text; --no-llm disables summarization entirely.
    summaries = None if (args.fake or args.no_llm) else SummaryManager(verbose=args.verbose)
    try:
        os.makedirs(LLM_WORKER_DIR, exist_ok=True)   # summarizer worker cwd
    except OSError:
        pass
    order = OrderTracker()   # calm carousel ordering (persists across cycles)
    firstseen = FirstSeenOrder()   # stable first-seen base for the fleet (pins on top)
    agents = None if args.fake else AgentActivity()   # teammate CPU-delta tracker
    names = NameStore()      # persistent Matt-assigned custom names
    positions = PositionStore()   # persistent manual pins (A1 absolute-index reorder)
    view_state = {"fleet": "claude"}   # active board fleet (toggled by firmware)
    nebula = NebulaScanner(args.verbose)   # 2nd fleet; polled only while view active
    if args.once:
        run_once(args, summaries, order, agents, names, positions, firstseen, seq=1)
        return 0
    run_daemon(args, summaries, order, agents, names, positions, view_state, nebula,
               firstseen)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
WiFi Cold-Start Test — automated wake/sleep cycle analyzer.

Wakes the device via tap actuator, captures boot logs through LCD USB serial,
extracts WiFi cold-start timing, waits for sleep, then repeats.

Usage:
    python3 wifi_coldstart_test.py                    # 5 cycles
    python3 wifi_coldstart_test.py --cycles 10        # 10 cycles
    python3 wifi_coldstart_test.py --port /dev/cu.usbmodem101
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time
from typing import Optional

try:
    import serial
except ImportError:
    print("ERROR: pyserial required.  pip install pyserial")
    sys.exit(1)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAP_SCRIPT = os.path.join(REPO_ROOT, "tap_implementation", "tap.py")
ACTUATOR_PORT = "/dev/cu.usbmodem21301"


def find_lcd_port():
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    candidates = [p for p in candidates if "21301" not in p]
    for c in candidates:
        if c.endswith("101"):
            return c
    return candidates[0] if candidates else None


def wait_for_port(port, timeout=30):
    start = time.time()
    while time.time() - start < timeout:
        if os.path.exists(port):
            return True
        time.sleep(0.5)
    return False


def tap_wake():
    try:
        result = subprocess.run(
            [sys.executable, TAP_SCRIPT, "--port", ACTUATOR_PORT],
            capture_output=True, text=True, timeout=15
        )
        return result.returncode == 0
    except Exception as e:
        print("  Tap failed: %s" % e)
        return False


def run_cycle(port, cycle_num):
    """Run one wake cycle: tap, capture boot logs, wait for sleep."""
    print("\n" + "=" * 60)
    print("CYCLE %d" % cycle_num)
    print("=" * 60)

    # Step 1: Wake device
    print("[%d] Waking device..." % cycle_num)
    if not tap_wake():
        print("[%d] WARNING: tap may have failed" % cycle_num)
    time.sleep(2)

    # Step 2: Wait for LCD port
    print("[%d] Waiting for LCD port..." % cycle_num)
    if not wait_for_port(port, timeout=15):
        print("[%d] ERROR: LCD port did not appear after 15s" % cycle_num)
        return {"cycle": cycle_num, "error": "no_port"}

    time.sleep(1)  # Let serial settle

    # Step 3: Open serial and capture lines until sleep
    result = {
        "cycle": cycle_num,
        "boot_wifi_logs": [],
        "rssi_reports": [],
        "wifi_connected": False,
        "wifi_connect_time_ms": None,
        "wifi_retries": 0,
        "sense_diag_count": 0,
        "link_hb_count": 0,
        "pong_count": 0,
        "sleep_seen": False,
        "total_lines": 0,
        "uptime_at_sleep_ms": None,
        "error": None,
    }

    try:
        s = serial.Serial(port, 115200, timeout=1)
    except Exception as e:
        print("[%d] ERROR: could not open serial: %s" % (cycle_num, e))
        result["error"] = "serial_open_fail"
        return result

    start_time = time.time()
    max_wait = 120  # 2 minutes max per cycle

    try:
        while time.time() - start_time < max_wait:
            try:
                raw = s.readline()
            except serial.SerialException:
                # Port disappeared (device sleeping)
                print("[%d] Serial disconnected (sleep)" % cycle_num)
                result["sleep_seen"] = True
                break

            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            result["total_lines"] += 1

            # --- Extract WiFi boot flow (Sense USB logs, if Sense cable connected) ---
            if "[BOOT_WIFI]" in line:
                result["boot_wifi_logs"].append(line)
                print("  %s" % line)
                if "retry" in line.lower():
                    result["wifi_retries"] += 1

            if "[BOOT_FLOW]" in line and "wifi" in line.lower():
                result["boot_wifi_logs"].append(line)
                print("  %s" % line)
                if "wifi_wait_connected" in line:
                    result["wifi_connected"] = True
                    m = re.search(r"elapsed=(\d+)ms", line)
                    if m:
                        result["wifi_connect_time_ms"] = int(m.group(1))
                elif "wifi_wait_timeout" in line:
                    result["wifi_connected"] = False
                elif "wifi_connect_ok" in line:
                    result["wifi_connected"] = True
                    m = re.search(r"elapsed_ms=(\d+)", line)
                    if m:
                        result["wifi_connect_time_ms"] = int(m.group(1))

            # --- RSSI reports (proof WiFi is connected — Sense only sends
            #     rssi_report with label=connected when WL_CONNECTED) ---
            if "rssi_report" in line and "label=connected" in line:
                m = re.search(r"code=(-?\d+)", line)
                if m:
                    rssi = int(m.group(1))
                    result["rssi_reports"].append(rssi)
                    if not result["wifi_connected"]:
                        result["wifi_connected"] = True
                        # Estimate connect time from boot: first rssi_report timestamp
                        m2 = re.search(r"ts=(\d+)", line)
                        if not result["wifi_connect_time_ms"]:
                            # Use time since cycle start as rough estimate
                            elapsed_s = time.time() - start_time
                            result["wifi_connect_time_ms"] = int(elapsed_s * 1000)

            # --- WIFI_DIAG_SUMMARY (sent before sleep) ---
            if "WIFI_DIAG_SUMMARY" in line:
                result["boot_wifi_logs"].append(line)
                m = re.search(r'"attempts":(\d+)', line)
                if m:
                    result["wifi_attempts"] = int(m.group(1))
                m = re.search(r'"fails":(\d+)', line)
                if m:
                    result["wifi_retries"] = int(m.group(1))

            # --- Count UART health indicators ---
            if "[LINK_HB]" in line:
                result["link_hb_count"] += 1

            if "type=PONG" in line:
                result["pong_count"] += 1

            if "[SENSE_DIAG]" in line:
                result["sense_diag_count"] += 1

            # --- Detect sleep ---
            if "entering_deep_sleep" in line:
                result["sleep_seen"] = True
                m = re.search(r"now_ms=(\d+)", line)
                if m:
                    result["uptime_at_sleep_ms"] = int(m.group(1))
                print("[%d] Device entering sleep (uptime=%s ms)" % (
                    cycle_num, result["uptime_at_sleep_ms"]))
                # Read a few more lines then break
                time.sleep(0.5)
                break

    except Exception as e:
        result["error"] = str(e)
        print("[%d] ERROR: %s" % (cycle_num, e))
    finally:
        try:
            s.close()
        except Exception:
            pass

    # Summary
    rssi_avg = sum(result["rssi_reports"]) / len(result["rssi_reports"]) if result["rssi_reports"] else 0
    print("\n[%d] RESULT: wifi=%s connect_ms=%s retries=%d rssi_avg=%.0f hb=%d pong=%d lines=%d" % (
        cycle_num,
        "OK" if result["wifi_connected"] else "FAIL",
        result["wifi_connect_time_ms"],
        result["wifi_retries"],
        rssi_avg,
        result["link_hb_count"],
        result["pong_count"],
        result["total_lines"],
    ))

    # Wait for port to disappear (device fully asleep)
    if result["sleep_seen"]:
        time.sleep(3)

    return result


def main():
    parser = argparse.ArgumentParser(description="WiFi Cold-Start Test")
    parser.add_argument("--port", help="LCD serial port (auto-detect if omitted)")
    parser.add_argument("--cycles", type=int, default=5, help="Number of wake cycles (default 5)")
    args = parser.parse_args()

    port = args.port or find_lcd_port() or "/dev/cu.usbmodem101"
    print("WiFi Cold-Start Test")
    print("  LCD port:    %s" % port)
    print("  Cycles:      %d" % args.cycles)
    print("  Tap script:  %s" % TAP_SCRIPT)
    print()

    results = []
    for i in range(1, args.cycles + 1):
        r = run_cycle(port, i)
        results.append(r)
        if i < args.cycles:
            print("\n[wait] Pausing 5s before next cycle...")
            time.sleep(5)

    # Final report
    print("\n" + "=" * 60)
    print("FINAL REPORT — %d cycles" % len(results))
    print("=" * 60)
    print("%-6s %-8s %-12s %-8s %-10s %-6s %-6s" % (
        "Cycle", "WiFi", "Connect(ms)", "Retries", "RSSI(avg)", "HBs", "PONGs"))
    print("-" * 60)

    ok_count = 0
    connect_times = []
    retry_counts = []
    for r in results:
        if r.get("error") and not r.get("wifi_connected"):
            print("%-6d %-8s %-12s %-8s %-10s %-6s %-6s" % (
                r["cycle"], "ERROR", r.get("error", "?")[:12], "-", "-", "-", "-"))
            continue

        wifi_str = "OK" if r["wifi_connected"] else "FAIL"
        ct = r["wifi_connect_time_ms"]
        ct_str = str(ct) if ct is not None else "-"
        rssi_vals = r["rssi_reports"]
        rssi_str = "%.0f" % (sum(rssi_vals) / len(rssi_vals)) if rssi_vals else "-"

        print("%-6d %-8s %-12s %-8d %-10s %-6d %-6d" % (
            r["cycle"], wifi_str, ct_str, r["wifi_retries"],
            rssi_str, r["link_hb_count"], r["pong_count"]))

        if r["wifi_connected"]:
            ok_count += 1
            if ct is not None:
                connect_times.append(ct)
        retry_counts.append(r["wifi_retries"])

    print("-" * 60)
    print("Success rate: %d/%d (%.0f%%)" % (ok_count, len(results), 100.0 * ok_count / len(results) if results else 0))
    if connect_times:
        print("Connect time: min=%dms avg=%dms max=%dms" % (min(connect_times), sum(connect_times)//len(connect_times), max(connect_times)))
    total_retries = sum(retry_counts)
    if total_retries > 0:
        print("Total retries: %d (across %d cycles)" % (total_retries, len(results)))
    print()


if __name__ == "__main__":
    main()

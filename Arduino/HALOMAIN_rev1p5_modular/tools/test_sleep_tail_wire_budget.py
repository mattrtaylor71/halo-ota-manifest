#!/usr/bin/env python3
"""Compile the real sleep-ready serializer and final handoff; model a RX stall.

The production SLEEP_READY JSON builder and final once-only sleep block run
against real ArduinoJson. UART and MCU state are explicit host doubles. The
128-byte FIFO/drop-new model is deliberately hypothetical: it establishes the
wire budget and reproduces a possible overload, not the observed chip's exact
interrupt, FIFO-overrun, or byte-reordering mechanism. No hardware/network I/O.
"""
import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]
ARDUINO_JSON = Path.home() / "Documents/Arduino/libraries/ArduinoJson/src"
FIFO_BYTES = 128
BAUD = 115200


def harness(root):
    sleep = (root / "Sense_Minimal/sense_sleep.h").read_text()
    sender = (root / "Sense_Minimal/sense_uart_msg.h").read_text()
    deep = definition(sleep, "static void sense_enter_deep_sleep(")
    final = definition(deep, "if (!sleep_ready_sent_for_cycle)")
    ready = definition(sender, "static void uart_send_sleep_ready(")
    assert "uart_send_wifi_diag_summary(" not in deep, "Optional summary returned to sleep path"
    # Keep the transport double honest about the production frame delimiter.
    uart = (root / "Sense_Minimal/sense_uart.h").read_text()
    tx = definition(uart, "static bool uart_send_json(" if "static bool uart_send_json(" in uart else "static void uart_send_json(")
    assert 'lcdSerial.print(json_str);' in tx
    assert 'lcdSerial.print("\\n");' in tx
    assert tx.index('lcdSerial.print(json_str);') < tx.index('lcdSerial.print("\\n");')
    return r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
using String = std::string;
static constexpr unsigned PROTOCOL_VERSION = 1;
#define HALO_SENSE_LCD_DIAG_BRIDGE 1
static uint32_t tick_ms=119683, next_msg=152;
static uint32_t millis(){return tick_ms;}
static uint32_t get_next_msg_id(){return next_msg;}
static std::string wire;
static void uart_send_json(const char* s){wire += s;wire += '\n';}
static struct {
 template<class... A> void printf(const char*, A...){}
 void println(const char*){}
} Serial;
static unsigned finalized=0;
static void wifi_diag_finalize(){++finalized;}
static bool sleep_ready_sent_for_cycle=false;
static const char* sleep_ready_reason="coordinated";
static uint32_t sleep_sm_msg_id=69;
static constexpr int SLEEP_SM_READY_SENT=3;
static unsigned transitions=0;
static void sleep_sm_transition(int state,const char* event,uint32_t msg){
 assert(state==SLEEP_SM_READY_SENT && !strcmp(event,"READY_SENT") && msg==69);
 ++transitions;
}
''' + ready + "\nstatic void final_handoff(){\n" + final + r'''
}
static std::vector<std::string> types(const std::string& input){
 std::vector<std::string> out;size_t start=0,end;
 while((end=input.find('\n',start))!=std::string::npos){
  JsonDocument d;assert(deserializeJson(d,input.substr(start,end-start))==DeserializationError::Ok);
  assert(d["ver"].as<unsigned>()==PROTOCOL_VERSION);
  out.emplace_back(d["type"].as<const char*>());start=end+1;
 }
 assert(start==input.size());return out;
}
int main(){
 // Historical schema fixture, not a production serializer. Its representative
 // counters model the removed optional frame; do not infer exact hardware loss.
 const char* summary=R"({"ver":1,"type":"WIFI_DIAG_SUMMARY","msg_id":151,"ts":119650,"attempts":1,"successes":1,"fails":0,"disconnects":2,"hard_resets":0,"rssi_min":-83,"rssi_max":-75,"rssi_last":-80,"connected_ms":6176,"disconnected_ms":113507,"fail_status":0,"uptime_ms":119650})";
 wire=std::string(summary)+"\n";
 assert(types(wire)==std::vector<std::string>{"WIFI_DIAG_SUMMARY"});
 std::printf("SUMMARY %s",wire.c_str());
 size_t maximum=0;
 const uint32_t cases[]={0,1,9,10,99,100,152,UINT32_MAX};
 for(uint32_t id:cases)for(uint32_t tick:cases){
  wire.clear();next_msg=id;tick_ms=tick;uart_send_sleep_ready();
  assert(types(wire)==std::vector<std::string>{"SLEEP_READY"});
  JsonDocument d;assert(deserializeJson(d,wire)==DeserializationError::Ok);
  assert(d["msg_id"].as<uint32_t>()==id && d["ts"].as<uint32_t>()==tick);
  assert(wire.size()<=128);if(wire.size()>maximum)maximum=wire.size();
 }
 // The largest possible numeric fields in this production schema.
 next_msg=UINT32_MAX;tick_ms=UINT32_MAX;wire.clear();uart_send_sleep_ready();
 assert(wire.size()==maximum);
 std::printf("MAX_READY %s",wire.c_str());
 // Execute the actual production once-only handoff, including its state flag.
 for(const char* reason:{"coordinated","inactivity",static_cast<const char*>(nullptr)}){
  wire.clear();transitions=0;sleep_ready_sent_for_cycle=false;sleep_ready_reason=reason;
  final_handoff();
  assert(types(wire)==std::vector<std::string>{"SLEEP_READY"});
  assert(sleep_ready_sent_for_cycle && transitions==1);
  const std::string first=wire;final_handoff();
  assert(wire==first && transitions==1);
 }
 std::printf("PASS actual final handoff sends only SLEEP_READY once; maximum_wire_bytes=%zu\n",maximum);
}
'''


@dataclass(frozen=True)
class StalledResult:
    received: bytes
    dropped: int
    peak_fifo: int


def stalled_rx(wire, stall_us, capacity=FIFO_BYTES):
    """8N1, ISR fully paused from byte0; discard new bytes when FIFO is full.

    At the first byte after the pause, or at end of input, service empties the
    FIFO; later bytes are serviced immediately. Software ring capacity is not
    the limiting resource. This is an explicit model, not ESP-IDF emulation.
    """
    fifo = bytearray()
    received = bytearray()
    dropped = peak = 0
    for index, byte in enumerate(wire):
        arrival_us = (index + 1) * 10 * 1_000_000 / BAUD
        if arrival_us >= stall_us:
            received.extend(fifo)
            fifo.clear()
            received.append(byte)
        elif len(fifo) < capacity:
            fifo.append(byte)
            peak = max(peak, len(fifo))
        else:
            dropped += 1
    received.extend(fifo)
    return StalledResult(bytes(received), dropped, peak)


def valid_types(wire):
    result = []
    for line in wire.splitlines(keepends=True):
        if not line.endswith(b"\n"):
            continue
        try:
            result.append(json.loads(line)["type"])
        except (ValueError, KeyError, TypeError):
            pass
    return result


class WireBudgetTests(unittest.TestCase):
    summary = ready = b""

    def test_production_ready_is_lossless_across_bounded_stalls(self):
        self.assertLessEqual(len(self.ready), FIFO_BYTES)
        # Include zero service delay, the FIFO-fill interval, the observed
        # approximately56ms handler window, and a deliberately longer pause.
        for stall in (0, 1000, 11000, 25000, 56000, 1000000):
            with self.subTest(stall_us=stall):
                result = stalled_rx(self.ready, stall)
                self.assertEqual(result.dropped, 0)
                self.assertEqual(result.received, self.ready)
                self.assertEqual(valid_types(result.received), ["SLEEP_READY"])

    def test_old_diagnostic_burst_can_overflow_before_final_ready(self):
        burst = self.summary + self.ready
        self.assertGreater(len(self.summary), FIFO_BYTES)
        # Both complete frames arrive while ISR service is paused; loss of the
        # tail is the consequence of this specified drop-new FIFO model.
        for stall in (56000, 1000000):
            with self.subTest(stall_us=stall):
                self.assertLess(len(burst) * 10 * 1_000_000 / BAUD, stall)
                result = stalled_rx(burst, stall)
                self.assertEqual(result.peak_fifo, FIFO_BYTES)
                self.assertEqual(result.dropped, len(burst) - FIFO_BYTES)
                self.assertNotIn("SLEEP_READY", valid_types(result.received))

    def test_model_does_not_invent_loss_without_stall(self):
        burst = self.summary + self.ready
        result = stalled_rx(burst, 0)
        self.assertEqual(result.received, burst)
        self.assertEqual(result.dropped, 0)
        self.assertEqual(valid_types(result.received), ["WIFI_DIAG_SUMMARY", "SLEEP_READY"])

    def test_hardware_capacity_boundary(self):
        self.assertEqual(stalled_rx(b"x" * 128, 1000000).dropped, 0)
        self.assertEqual(stalled_rx(b"x" * 129, 1000000).dropped, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    compiler = shutil.which("c++")
    assert compiler, "Host C++ compiler required"
    assert (ARDUINO_JSON / "ArduinoJson.h").is_file(), "Canonical ArduinoJson required"
    with tempfile.TemporaryDirectory(prefix="halo-sleep-tail-") as temp:
        directory = Path(temp)
        cpp, binary = directory / "check.cpp", directory / "check"
        cpp.write_text(harness(args.source_root))
        subprocess.run([compiler, "-std=c++17", "-Wno-deprecated-declarations", "-I", str(ARDUINO_JSON),
                        str(cpp), "-o", str(binary)], check=True, timeout=30)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
    for line in result.stdout.splitlines():
        if line.startswith("SUMMARY "):
            WireBudgetTests.summary = line.removeprefix("SUMMARY ").encode() + b"\n"
        elif line.startswith("MAX_READY "):
            WireBudgetTests.ready = line.removeprefix("MAX_READY ").encode() + b"\n"
        else:
            print(line, flush=True)
    assert WireBudgetTests.summary and WireBudgetTests.ready, "Native serializer receipts missing"
    print(f"Wire bytes: summary={len(WireBudgetTests.summary)}, ready_max={len(WireBudgetTests.ready)}, FIFO={FIFO_BYTES}", flush=True)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(WireBudgetTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    print("LIMIT: host stall model only; no claim of the observed silicon failure mechanism.")


if __name__ == "__main__":
    main()

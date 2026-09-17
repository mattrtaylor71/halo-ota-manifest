#!/usr/bin/env python3
"""Run the actual provisioning display reducer and Sense bridge with fake I/O.

Host-only: compiles the production header, state-string accessor and complete
service_provision_display function. Tests drive time, AP/app activity and owner
storage independently so transitions that do not change ProvisioningState are
covered. No firmware build, serial, network or device state is touched.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = "halo_ota_demo/firmware/shared/ProvisioningDisplayStatus.h"
SENSE = "halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino"
STATE = "halo_ota_demo/firmware/shared/ProvisioningState.cpp"


def definition(text, signature):
    start = text.index(signature)
    pos = text.index("{", start) + 1
    depth = 1
    while depth:
        if text.startswith("//", pos):
            pos = text.index("\n", pos)
            continue
        if text.startswith("/*", pos):
            pos = text.index("*/", pos) + 2
            continue
        if text[pos] in ('"', "'"):
            quote = text[pos]
            pos += 1
            while text[pos] != quote:
                pos += 2 if text[pos] == "\\" else 1
            pos += 1
            continue
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos]


PREFIX = r'''
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
static uint32_t clock_ms = 0;
static uint32_t millis() { return clock_ms; }
static std::string owner_id;
static bool owner_read_ok = true;
static unsigned owner_reads = 0, app_queries = 0;
static std::vector<std::string> reset_calls;
namespace ProvisioningState {
  enum State { STATE_UNPROVISIONED, STATE_AP_SETUP,
               STATE_CONNECTING_HOME_WIFI, STATE_CONNECTED, STATE_ERROR };
  static bool loadOwnerId(char* out, size_t len) {
    ++owner_reads;
    if (!owner_read_ok) return false;
    snprintf(out, len, "%s", owner_id.c_str());
    return true;
  }
  const char* getStateString(State state);
  static State current_state=STATE_UNPROVISIONED;
  static State getState() { return current_state; }
  static void setState(State state) { current_state=state; reset_calls.emplace_back("state"); }
  static void clearHomeWifiCreds() { reset_calls.emplace_back("clear_home"); }
  static void clearApCreds() { reset_calls.emplace_back("clear_ap"); }
  static void clearOwnerId() { owner_id.clear(); reset_calls.emplace_back("clear_owner"); }
  static void clearOwnerCode() { reset_calls.emplace_back("clear_code"); }
  static void setProvisioned(bool) { reset_calls.emplace_back("provisioned"); }
}
struct FakeProvisioningManager {
  bool setup = false, exhausted = false;
  uint32_t last_app_request_ms = 0;
  bool isSetupModeActive() const { return setup; }
  void stopSetupMode() { reset_calls.emplace_back("stop"); setup=false; }
  bool startSetupMode() {
    reset_calls.emplace_back("start");
    // Match the production manager's idempotent active-session admission.
    if (setup) return true;
    setup=true; last_app_request_ms=0;
    ProvisioningState::current_state=ProvisioningState::STATE_AP_SETUP;
    return true;
  }
  bool ownerClaimExhausted() const { return exhausted; }
  bool claimTransportBusy() const { return false; }
  void cancelOwnerClaim() {}
  void resetClaimForRetry() { if (owner_id.empty()) exhausted=false; }
  bool isAppSessionActive(uint32_t now) const {
    ++app_queries;
    return last_app_request_ms && uint32_t(now-last_app_request_ms) <= 45000;
  }
} g_provisioning_manager;
static ProvisioningDisplayStatus g_provision_display_status;
static std::atomic<bool> g_provision_reset_pending{false};
static char g_last_provision_display_status[24] = "";
static uint32_t g_provision_display_poll_ms = 0;
static uint32_t g_provision_display_send_ms = 0;
static bool s_post_ap_claim_retry_pending = false;
static bool s_last_setup_mode_active = false;
static unsigned long s_post_ap_shutdown_ms = 0;
static bool g_pending_provision_qr=false;
static unsigned long g_last_provision_qr_ms=0;
static ProvisioningState::State g_last_prov_state=ProvisioningState::STATE_UNPROVISIONED;
static std::vector<std::string> sent;
static void send_provision_status(const char* state) { sent.emplace_back(state); }
'''

TESTS = r'''
using namespace ProvisioningState;
static unsigned failed = 0;
static void check(const char* name, bool ok) {
  printf("{\"test\":\"%s\",\"pass\":%s}\n", name, ok?"true":"false");
  if (!ok) ++failed;
}
static std::string latest() { return sent.empty()?"":sent.back(); }
static void tick(uint32_t now, State state, bool changed=false) {
  clock_ms=now; service_provision_display(state, changed);
}
static void reset() {
  clock_ms=0; owner_id.clear(); owner_read_ok=true;
  owner_reads=app_queries=0; sent.clear();
  g_provisioning_manager={}; g_provision_display_status.reset();
  g_last_provision_display_status[0]=0;
  g_provision_display_poll_ms=g_provision_display_send_ms=0;
  s_post_ap_claim_retry_pending=s_last_setup_mode_active=false;
  s_post_ap_shutdown_ms=0;
  reset_calls.clear(); g_pending_provision_qr=false; g_last_provision_qr_ms=0;
  ProvisioningState::current_state=STATE_UNPROVISIONED;
  g_last_prov_state=STATE_UNPROVISIONED;
}
static void start() {
  reset(); g_provisioning_manager.setup=true;
  tick(1000, STATE_AP_SETUP, true);
}
int main() {
  reset(); owner_id="existing-owner";
  tick(1000, STATE_CONNECTED, true);
  for (uint32_t t=1001; t<10000; ++t) tick(t, STATE_CONNECTED);
  check("normal_boot_never_enters_guide_or_polls_owner",
        sent.size()==1 && latest()=="connected" && owner_reads==0 &&
        app_queries==1 && !g_provision_display_status.tracking());

  start();
  check("new_setup_starts_at_qr_without_owner_reads",
        latest()=="ap_setup" && owner_reads==0);
  g_provisioning_manager.last_app_request_ms=1100;
  tick(1499, STATE_AP_SETUP);
  check("unchanged_state_poll_is_throttled_to_500ms",
        app_queries==1 && sent.size()==1);
  tick(1500, STATE_AP_SETUP);
  check("app_detected_while_provisioning_state_stays_ap_setup",
        latest()=="app_connected" && sent.size()==2 && app_queries==2);
  tick(46501, STATE_AP_SETUP);
  check("detected_app_remains_latched_after_45s_http_pause",
        latest()=="app_connected" && sent.size()==3 && owner_reads==0);
  tick(47000, STATE_AP_SETUP);
  tick(47001, STATE_AP_SETUP);
  tick(49500, STATE_AP_SETUP);
  const size_t before_resend=sent.size();
  tick(50000, STATE_AP_SETUP);
  check("active_status_repeats_after_3s_without_spamming",
        before_resend==3 && sent.size()==4 && latest()=="app_connected");

  tick(50001, STATE_CONNECTING_HOME_WIFI, true);
  check("state_change_bypasses_poll_delay", latest()=="connecting");
  tick(50100, STATE_CONNECTED, true);
  check("wifi_without_owner_reports_claiming_not_success",
        latest()=="claiming" && owner_reads==1);
  owner_read_ok=false;
  tick(50600, STATE_CONNECTED);
  check("owner_read_failure_cannot_report_success", latest()=="claiming");
  owner_read_ok=true;
  g_provisioning_manager.exhausted=true;
  tick(51100, STATE_CONNECTED);
  check("exhausted_claim_reports_failed", latest()=="failed");
  g_provisioning_manager.exhausted=false;
  tick(51600, STATE_CONNECTED);
  check("in_session_retry_returns_to_claiming", latest()=="claiming");
  owner_id="new-owner";
  g_provisioning_manager.exhausted=true;
  tick(52100, STATE_CONNECTED);
  check("owner_confirmation_wins_over_stale_failure", latest()=="connected");
  g_provisioning_manager.setup=false;
  tick(52600, STATE_CONNECTED);
  const unsigned settled_reads=owner_reads, settled_queries=app_queries;
  const size_t settled_sends=sent.size();
  for(uint32_t t=53000;t<70000;t+=100) tick(t,STATE_CONNECTED);
  check("completed_setup_stops_storage_polling_and_status_sends",
        owner_reads==settled_reads && app_queries==settled_queries &&
        sent.size()==settled_sends && !g_provision_display_status.tracking());
  g_provisioning_manager.setup=true;
  g_provisioning_manager.last_app_request_ms=0;
  tick(71000, STATE_AP_SETUP, true);
  check("new_session_clears_previous_app_detection", latest()=="ap_setup");
  tick(71001, STATE_ERROR, true);
  check("production_error_string_is_forwarded_as_lcd_failed", latest()=="failed");
  tick(71002, STATE_AP_SETUP, true);
  check("wifi_error_can_return_to_qr_retry", latest()=="ap_setup");

  // Exercise the actual post-AP recovery block followed by the display bridge,
  // with no underlying provisioning-state transition during claim recovery.
  start();
  tick(1500, STATE_CONNECTED, true);
  g_provisioning_manager.exhausted=true;
  tick(2000, STATE_CONNECTED);
  s_last_setup_mode_active=true;
  g_provisioning_manager.setup=false;
  clock_ms=2500; production_post_ap_slice(STATE_CONNECTED, false);
  check("post_ap_recovery_window_retains_guide_after_exhaustion",
        latest()=="claiming" && s_post_ap_claim_retry_pending &&
        g_provision_display_status.tracking());
  clock_ms=3000; production_post_ap_slice(STATE_CONNECTED, false);
  owner_id="recovered-owner";
  clock_ms=3500; production_post_ap_slice(STATE_CONNECTED, false);
  check("post_ap_claim_retry_owner_success_reaches_lcd",
        latest()=="connected" && !s_post_ap_claim_retry_pending &&
        !g_provision_display_status.tracking());

  start(); tick(1500, STATE_CONNECTED, true);
  s_last_setup_mode_active=true;
  g_provisioning_manager.setup=false;
  clock_ms=2000; production_post_ap_slice(STATE_CONNECTED, false);
  clock_ms=32501; production_post_ap_slice(STATE_CONNECTED, false);
  check("post_ap_timeout_fails_even_before_four_claim_attempts",
        latest()=="failed" && !s_post_ap_claim_retry_pending &&
        !g_provision_display_status.tracking());

  start();
  tick(UINT32_MAX-255, STATE_AP_SETUP);
  const size_t wrap_sends=sent.size();
  const unsigned wrap_queries=app_queries;
  tick(243, STATE_AP_SETUP);
  const bool before_wrap_poll=app_queries==wrap_queries;
  tick(244, STATE_AP_SETUP);
  tick(2744, STATE_AP_SETUP);
  check("poll_and_repeat_intervals_survive_millis_wrap",
        before_wrap_poll && app_queries==wrap_queries+2 &&
        sent.size()==wrap_sends+1 && latest()=="ap_setup");

  start(); g_provisioning_manager.last_app_request_ms=1100;
  tick(1500,STATE_AP_SETUP);
  owner_id="old-owner"; g_last_provision_qr_ms=1000;
  halo_prod_reset_wifi();
  const std::vector<std::string> active_reset_order={
    "stop","clear_home","clear_ap","clear_owner","clear_code","provisioned","state","start"};
  check("retry_stops_existing_ap_before_clearing_and_starting_fresh_session",
        reset_calls==active_reset_order && current_state==STATE_AP_SETUP &&
        g_last_prov_state==STATE_AP_SETUP && g_pending_provision_qr &&
        g_last_provision_qr_ms==0 && owner_id.empty());
  tick(1501, STATE_AP_SETUP);
  check("retry_clears_detected_app_and_emits_fresh_qr_status",
        latest()=="ap_setup" && g_provisioning_manager.last_app_request_ms==0);
  reset(); halo_prod_reset_wifi();
  const std::vector<std::string> inactive_reset_order={
    "clear_home","clear_ap","clear_owner","clear_code","provisioned","state","start"};
  check("inactive_reset_starts_setup_without_unnecessary_ap_teardown",
        reset_calls==inactive_reset_order && current_state==STATE_AP_SETUP &&
        g_last_prov_state==STATE_AP_SETUP && g_pending_provision_qr);
  return failed?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--output", type=Path, help="Optional JSON receipt")
    args = parser.parse_args()
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        parser.error("A native C++ compiler is required")
    root = args.source_root.resolve()
    sense = (root / SENSE).read_text()
    bridge = definition(sense, "static void service_provision_display(")
    reset_wifi = definition(sense, "void halo_prod_reset_wifi()")
    state = definition((root / STATE).read_text(),
                       "const char* ProvisioningState::getStateString(State state)")
    # Also pin the bridge into the real loop: a helper that is never called is
    # not enough to cover app detection while the underlying state is unchanged.
    loop = definition(sense, "void halo_prod_loop()")
    call = "service_provision_display(prov_state, prov_state_changed);"
    changed = definition(loop, "if (prov_state_changed)")
    post_ap = definition(loop, "{\n    bool current_setup_active =")
    if call not in loop or call in changed or loop.index(call) < loop.index(post_ap):
        raise AssertionError("Display service must run independently of state-change handling")
    recovery_slice = ("static void production_post_ap_slice(ProvisioningState::State prov_state, "
                      "bool prov_state_changed) {\n" + post_ap + "\n" + call + "\n}")
    harness = '\n'.join([
        '#include "' + str(root / HEADER) + '"', PREFIX, state, bridge, recovery_slice, reset_wifi, TESTS])
    with tempfile.TemporaryDirectory(prefix="halo-provisioning-display-") as directory:
        cpp = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        cpp.write_text(harness)
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        str(cpp), "-o", str(binary)], check=True, timeout=30)
        run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)
    rows = [json.loads(line) for line in run.stdout.splitlines()]
    if run.stderr:
        print(run.stderr, end="")
    passed = bool(rows) and run.returncode == 0 and all(row["pass"] for row in rows)
    receipt = {
        "status": "PASS" if passed else "FAIL",
        "scope": "Host-only actual reducer header, state-string accessor and full Sense bridge with mocked time, manager activity, NVS owner reads and UART output; no device or network.",
        "source_root": str(root),
        "source_sha256": {name: hashlib.sha256((root/name).read_bytes()).hexdigest()
                          for name in (HEADER, SENSE, STATE)},
        "harness_sha256": hashlib.sha256(harness.encode()).hexdigest(),
        "tests": rows,
    }
    if args.output:
        args.output.write_text(json.dumps(receipt, indent=2) + "\n")
    for row in rows:
        print(("PASS" if row["pass"] else "FAIL") + " " + row["test"])
    print(receipt["status"] + " provisioning display bridge")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

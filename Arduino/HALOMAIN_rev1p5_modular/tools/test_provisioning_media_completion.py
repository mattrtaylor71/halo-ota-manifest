#!/usr/bin/env python3
"""Execute production setup finalization and media-loop admission with fake I/O.

Reproduces a queued voice/image captured during the owner grace. Runs the actual
cleanup, teardown, foreground guard, provisioning-active and upload-hold functions,
plus the loop's actual early-return and post-AP bookkeeping slices. No hardware,
network, firmware flashing, or real FreeRTOS scheduling is exercised.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]
MANAGER = "halo_ota_demo/firmware/shared/ProvisioningManager.cpp"
HEADER = "halo_ota_demo/firmware/shared/ProvisioningManager.h"
WRAPPER = "halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino"
SENSE = "Sense_Minimal/Sense_Minimal.ino"

PREFIX = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define HALO_DEFER_UPLOADS_TO_SLEEP 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
static uint32_t clock_ms=1;
static unsigned stops=0,http_stops=0,dns_stops=0,ap_stops=0,dma_releases=0;
static unsigned manager_updates=0,claim_resets=0,diag_calls=0;
static unsigned active_ops=0,qcount=0,claim_reads=0;
static unsigned scan_deletes=0;
static int scan_status=-1;
static unsigned long g_scan_started_ms=0,g_scan_cached_ms=0;
static std::string g_scan_cached_response;
static bool rebooting=false,claim_busy=false,claim_busy_at_stop=false,g_scan_inflight=false;
static bool owner_read_ok=true;
static std::string owner="test-owner",diag;
static unsigned long millis(){return clock_ms;}
static bool halo_rebooting(){return rebooting;}
static void delay(unsigned ms){clock_ms+=ms;}
static unsigned heap_caps_get_largest_free_block(int){return 100000;}
static void dump_system_truth(const char*){}
static void halo_provisioning_dma_reserve(bool active){if(!active)++dma_releases;}
using wifi_mode_t=int;using wl_status_t=int;
static constexpr int WIFI_AP_STA=3,WIFI_STA=1,WL_CONNECTED=3;
static constexpr int WIFI_SCAN_RUNNING=-1,WIFI_SCAN_FAILED=-2;
static struct Radio {
 int getMode(){return WIFI_AP_STA;}int status(){return WL_CONNECTED;}
 int scanComplete(){return scan_status;}void scanDelete(){++scan_deletes;}
 void mode(int){}
} WiFi;
static struct Dns {void stop(){++dns_stops;}} *dns_server=nullptr;
namespace ProvisioningState {
 enum State {STATE_AP_SETUP,STATE_CONNECTING_HOME_WIFI,STATE_CONNECTED,STATE_ERROR};
 static State state=STATE_CONNECTED;
 static State getState(){return state;}
 static bool loadOwnerId(char* out,size_t n){
   if(!owner_read_ok)return false;
   snprintf(out,n,"%s",owner.c_str());return !owner.empty();
 }
}
class ProvisioningManager {public:
 bool setup_mode_active=true,claim_in_progress=false;
 unsigned long connected_verified_ms=1000,connected_state_set_ms=1000,owner_id_set_ms=1000;
 static const unsigned long MIN_CONNECTED_DELAY_MS=1500,OWNER_SUCCESS_GRACE_MS=15000;
 bool finishCompletedSetup();void stopSetupMode();
 void stopHttpServer(){++http_stops;}
 void stopSoftAP(){++ap_stops;++stops;}
 bool claimTransportBusy() const {++claim_reads;return claim_busy||(claim_busy_at_stop&&claim_reads>1);}
 bool isSetupModeActive()const{return setup_mode_active;}
 unsigned long getOwnerIdSetMs()const{return owner_id_set_ms;}
 void resetClaimForRetry(){++claim_resets;}
} g_provisioning_manager;
static bool http_inflight=false,upload_inflight=false,scan_ui_inflight=false;
static bool voice_recording_active=false,foreground_active=false,dish_scan_inflight=false;
static bool g_list_screen_active=false,g_ota_check_in_progress=false,g_ota_apply_in_progress=false;
static bool g_lcd_ota_task_running=false,g_lcd_ota_proxy_owns_uart=false;
static std::atomic<bool> upload_worker_claim_active{false},g_provision_reset_pending{false};
static bool s_post_ap_claim_retry_pending=false,s_last_setup_mode_active=true;
static unsigned long s_post_ap_shutdown_ms=0;
enum {OP_IDLE,OP_DONE,OP_CAPTURE};
static struct Job {int state=OP_IDLE;bool active=false;} current_job;
static void* op_queue=(void*)1;
static unsigned uxQueueMessagesWaiting(void*){return active_ops;}
static uint32_t upload_queue_count(){return qcount;}
static void mqtt_set_allowed(bool){}
static void uart_send_sense_diag(const char* area,const char* event,const char* reason,int,const char* detail){
 ++diag_calls;diag=std::string(area)+":"+event+":"+reason+":"+detail;
}
static bool g_upload_flush_requested=false;
static unsigned long g_upload_hold_since_ms=0;
static const unsigned long UPLOAD_HOLD_MAX_MS=600000;
static const uint32_t UPLOAD_HOLD_HIGHWATER=8;
struct UploadJob {bool is_voice=true;uint32_t job_id=129;bool from_voice_sd=false,from_image_sd=false,from_persisted=false;};
static UploadJob queue_head;
static bool upload_worker_peek_parked_job(UploadJob&){return false;}
static bool voice_list_pending(){return false;}
static std::atomic<uint32_t>g_voice_list_attempted_job{0};
static void* upload_queue=(void*)1;
static std::atomic<bool>g_media_retry_user_paused{false};
static bool foreground_priority_active(unsigned long,void*){return foreground_active;}
static int xQueuePeek(void*,UploadJob* out,int){*out=queue_head;return qcount?pdTRUE:0;}
'''

TESTS = r'''
static unsigned checks=0,failures=0;
static void check(const char* name,bool ok){
 ++checks;printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;
}
static void reset(){
 clock_ms=16000;stops=http_stops=dns_stops=ap_stops=dma_releases=0;
 manager_updates=claim_resets=diag_calls=active_ops=claim_reads=0;qcount=1;
 scan_deletes=0;scan_status=WIFI_SCAN_RUNNING;g_scan_started_ms=g_scan_cached_ms=0;
 g_scan_cached_response.clear();
 rebooting=claim_busy=claim_busy_at_stop=g_scan_inflight=false;
 owner_read_ok=true;owner="test-owner";diag.clear();
 g_provisioning_manager={};ProvisioningState::state=ProvisioningState::STATE_CONNECTED;
 http_inflight=upload_inflight=scan_ui_inflight=false;
 voice_recording_active=foreground_active=dish_scan_inflight=false;
 g_list_screen_active=g_ota_check_in_progress=g_ota_apply_in_progress=false;
 g_lcd_ota_task_running=g_lcd_ota_proxy_owns_uart=false;
 upload_worker_claim_active=false;g_provision_reset_pending=false;
 s_post_ap_claim_retry_pending=false;s_last_setup_mode_active=true;s_post_ap_shutdown_ms=0;
 current_job={};op_queue=(void*)1;
 g_upload_flush_requested=false;g_upload_hold_since_ms=0;queue_head={};
}
static void expect_held(const char* name){
 production_loop_slice();
 check(name,halo_provisioning_active()&&stops==0&&http_stops==0&&ap_stops==0&&
       diag_calls==0&&s_last_setup_mode_active&&!s_post_ap_shutdown_ms&&manager_updates==0);
}
int main(){
 for(bool voice:{true,false}){
  reset();queue_head.is_voice=voice;clock_ms=10000;
  const char* why=nullptr;
  check(voice?"fresh_voice_held":"fresh_image_held",uploads_held_for_session(&why)&&std::string(why)=="session_active");
  for(clock_ms=10000;clock_ms<16000;++clock_ms)production_loop_slice();
  check("queued_media_keeps_full_15_second_app_grace",stops==0&&diag_calls==0&&manager_updates==0);
  production_loop_slice();
  check(voice?"voice_during_grace_finishes_setup":"image_during_grace_finishes_setup",
        !halo_provisioning_active()&&stops==1&&http_stops==1&&ap_stops==1&&dma_releases==1&&qcount==1);
  check("queued_media_still_blocks_general_update",manager_updates==0&&claim_resets==0);
  check("completed_owner_has_no_claim_sleep_blocker",!s_post_ap_claim_retry_pending&&!s_last_setup_mode_active&&s_post_ap_shutdown_ms);
  check("completion_diag_is_bounded_and_contains_no_identity",diag_calls==1&&
        diag=="provision:setup_complete:media_flush_ready:setup=0 queued=1 grace_ms=15000");
  // The ordinary sleep path can now request flushing: setup is no longer its
  // blocker. Network/storage outcomes are outside this host test's scope.
  g_upload_flush_requested=true;
  check("ordinary_sleep_flush_releases_fresh_media",!uploads_held_for_session(&why)&&std::string(why)=="flush_requested");
  production_loop_slice();check("completion_not_repeated",stops==1&&diag_calls==1);
  qcount=0;production_loop_slice();
  check("later_owner_loop_does_not_recreate_claim_retry",manager_updates==1&&claim_resets==0&&!s_post_ap_claim_retry_pending);
 }
 reset();qcount=0;production_loop_slice();qcount=1;production_loop_slice();
 check("capture_after_grace_control",stops==1&&!halo_provisioning_active()&&manager_updates==1);

 reset();http_inflight=true;expect_held("active_http_retains_priority");
 reset();upload_inflight=true;expect_held("active_upload_retains_priority");
 reset();upload_worker_claim_active=true;expect_held("dequeue_owner_retains_priority");
 reset();current_job.active=true;expect_held("active_capture_retains_priority");
 reset();current_job.state=OP_CAPTURE;expect_held("capture_state_retains_priority");
 reset();voice_recording_active=true;expect_held("active_voice_retains_priority");
 reset();foreground_active=true;expect_held("foreground_retains_priority");
 reset();scan_ui_inflight=true;expect_held("scan_ui_retains_priority");
 reset();dish_scan_inflight=true;expect_held("dish_capture_retains_priority");
 reset();g_list_screen_active=true;expect_held("shopping_list_retains_priority");
 reset();active_ops=1;expect_held("new_user_operation_retains_priority");
 reset();g_provision_reset_pending=true;expect_held("reset_owner_retains_priority");
 reset();g_ota_check_in_progress=true;expect_held("ota_check_retains_priority");
 reset();g_ota_apply_in_progress=true;expect_held("ota_apply_retains_priority");
 reset();g_lcd_ota_task_running=true;expect_held("lcd_ota_task_retains_priority");
 reset();g_lcd_ota_proxy_owns_uart=true;expect_held("lcd_proxy_retains_priority");

 reset();claim_busy=true;expect_held("owner_claim_transport_is_not_interrupted");
 reset();g_provisioning_manager.claim_in_progress=true;expect_held("claim_result_not_applied_by_cleanup");
 reset();g_scan_inflight=true;expect_held("setup_radio_scan_is_not_interrupted");
 scan_status=3;production_loop_slice();
 check("completed_sdk_scan_retires_without_general_update",stops==1&&scan_deletes==1&&!g_scan_inflight&&manager_updates==0);
 reset();g_scan_inflight=true;scan_status=WIFI_SCAN_FAILED;production_loop_slice();
 check("failed_sdk_scan_retires_without_general_update",stops==1&&scan_deletes==1&&!g_scan_inflight&&manager_updates==0);
 reset();g_scan_inflight=true;scan_status=0;clock_ms=15999;expect_held("finished_scan_is_kept_until_app_grace_finishes");
 check("grace_does_not_discard_scan_cache",scan_deletes==0&&g_scan_inflight);
 reset();rebooting=true;expect_held("reboot_owner_retains_priority");
 reset();owner_read_ok=false;expect_held("unreadable_owner_keeps_setup");
 reset();owner.clear();expect_held("missing_owner_keeps_setup_even_with_elapsed_grace");
 reset();g_provisioning_manager.owner_id_set_ms=0;expect_held("missing_owner_timestamp_does_not_use_unclaimed_timeout");
 reset();g_provisioning_manager.connected_verified_ms=0;expect_held("unverified_connection_keeps_setup");
 reset();g_provisioning_manager.connected_state_set_ms=0;expect_held("missing_connected_timestamp_keeps_setup");
 for(auto state:{ProvisioningState::STATE_AP_SETUP,ProvisioningState::STATE_CONNECTING_HOME_WIFI,ProvisioningState::STATE_ERROR}){
  reset();ProvisioningState::state=state;expect_held("only_successful_setup_can_finish");
 }
 reset();claim_busy_at_stop=true;expect_held("teardown_rechecks_claim_before_closing_transports");
 reset();g_provisioning_manager.connected_state_set_ms=15000;
 expect_held("minimum_connected_poll_window_preserved");
 clock_ms=16500;production_loop_slice();check("minimum_poll_window_releases_at_boundary",stops==1);
 reset();voice_recording_active=true;clock_ms=30000;expect_held("recording_across_grace_preserves_capture");
 voice_recording_active=false;production_loop_slice();check("recording_completion_allows_cleanup_next_tick",stops==1);
 reset();g_provisioning_manager.owner_id_set_ms=UINT32_MAX-10000;
 g_provisioning_manager.connected_state_set_ms=UINT32_MAX-11000;
 g_provisioning_manager.connected_verified_ms=UINT32_MAX-12000;
 clock_ms=4998;expect_held("grace_before_millis_wrap_boundary");
 clock_ms=4999;production_loop_slice();check("grace_survives_millis_wrap",stops==1);
 reset();g_provisioning_manager.setup_mode_active=false;s_last_setup_mode_active=false;
 production_loop_slice();check("already_closed_setup_is_noop",stops==0&&diag_calls==0);
 printf("%s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);
 return failures?1:0;
}
'''


def harness(root):
    texts = {name: (root/name).read_text() for name in (MANAGER, HEADER, WRAPPER, SENSE)}
    manager, wrapper = texts[MANAGER], texts[WRAPPER]
    loop = definition(wrapper, "void halo_prod_loop()")
    call = "service_completed_provisioning();"
    guard = definition(loop, "if (sense_action_inflight())")
    assert loop.index(call) < loop.index(guard) < loop.index("g_provisioning_manager.update();")
    assert "bool finishCompletedSetup();" in texts[HEADER]
    for declaration in ("MIN_CONNECTED_DELAY_MS = 1500", "OWNER_SUCCESS_GRACE_MS = 15000"):
        assert declaration in texts[HEADER]
    cleanup = definition(manager, "bool ProvisioningManager::finishCompletedSetup()")
    for forbidden in ("handleClient(", "tryClaimOwnerId(", "applyClaimResult(", "startSetupMode(",
                      "WiFi.begin(", "maybeRunOtaCheck(", "clearOwner", "saveOwner"):
        assert forbidden not in cleanup, forbidden
    post_ap = definition(loop, "{\n    bool current_setup_active =")
    actual = "\n".join([
        definition(manager, "static void clearProvisionScanCache()"),
        definition(manager, "void ProvisioningManager::stopSetupMode()"), cleanup,
        definition(wrapper, "static bool sense_action_inflight()"),
        definition(wrapper, "bool halo_provisioning_active()"),
        definition(wrapper, "static void service_completed_provisioning()"),
        definition(texts[SENSE], "static bool uploads_held_for_session("),
        "static void production_loop_slice(){\n" + call + "\n" + guard +
        "\n++manager_updates;\n" + post_ap + "\n}",
    ])
    return PREFIX + actual + TESTS, texts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    code, texts = harness(args.source_root.resolve())
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        parser.error("Native C++ compiler required")
    outcomes = []
    for name, source in (("fixed", code), ("missing_loop_call", code.replace(
            "static void production_loop_slice(){\nservice_completed_provisioning();", "static void production_loop_slice(){"))):
        cpp, binary = args.out/(name+".cpp"), args.out/name
        cpp.write_text(source)
        build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-function",
                                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                str(cpp), "-o", str(binary)], capture_output=True, text=True, timeout=40)
        (args.out/(name+"-compile.log")).write_text(build.stdout+build.stderr)
        if build.returncode:
            raise RuntimeError("Compile failed: " + str(args.out/(name+"-compile.log")))
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        (args.out/(name+".log")).write_text(run.stdout+run.stderr)
        outcomes.append({"name": name, "returncode": run.returncode, "output": run.stdout+run.stderr})
    passed = (outcomes[0]["returncode"] == 0 and outcomes[1]["returncode"] == 1 and
              "FAIL voice_during_grace_finishes_setup" in outcomes[1]["output"] and
              "FAIL image_during_grace_finishes_setup" in outcomes[1]["output"])
    result = {
        "status": "PASS" if passed else "FAIL",
        "scope": __doc__,
        "source_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in texts.items()},
        "harness_sha256": hashlib.sha256(code.encode()).hexdigest(),
        "results": outcomes,
        "limits": "Host fake I/O; no actual upload, device timing, NVS, AP teardown or preemptive task qualification.",
    }
    (args.out/"RESULT.json").write_text(json.dumps(result, indent=2)+"\n")
    print(outcomes[0]["output"], end="")
    print(("PASS" if passed else "FAIL") + " queued-media setup completion; missing-call negative control")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compose actual camera ownership and asynchronous Wi-Fi maintenance functions.

Regression for firmware177: WiFi.begin() has returned (no SDK call owns the
counter), but camera radio-off leaves its asynchronous association in flight.
The old source must fail this test. Clock, radio and RTOS boundaries are doubles;
this does not measure physical reconnection latency or ESP32 scheduling.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_voice_persistence import definition
from test_wifi_recovery import harness as wifi_harness, no_core

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    wifi = (root / "Sense_Minimal/sense_wifi.h").read_text()
    camera = (root / "Sense_Minimal/sense_camera.h").read_text()
    backup = (root / "Sense_Minimal/sense_backup_diagnostic.h").read_text()
    code = wifi_harness(root / "Sense_Minimal/sense_wifi.h")
    code = code[:code.index("int main(){")]

    def replace(old, new):
        nonlocal code
        assert code.count(old) == 1, old
        code = code.replace(old, new)

    replace("WL_CONNECT_FAILED=4, WL_CONNECTION_LOST=5, WL_DISCONNECTED=6",
            "WL_CONNECT_FAILED=4, WL_CONNECTION_LOST=5, WL_DISCONNECTED=6, WL_NO_SHIELD=255")
    replace("static constexpr int WIFI_STA=1, WIFI_PS_NONE=0;", r'''
static constexpr int WIFI_OFF=0, WIFI_STA=1, WIFI_PS_NONE=0;
static uint32_t pdMS_TO_TICKS(uint32_t ms){return ms;}
static int radio_mode;
static bool fail_radio_stop, inject_http_on_lock, reserve_restored;
static unsigned stop_calls, disconnect_calls, checks;
static void (*on_radio_probe)();
static void (*on_admission_probe)();
static std::atomic<bool> g_camera_radio_off_owned{false};
namespace sense_backup_diag {
static std::atomic<unsigned> wifi_calls{0};
static constexpr unsigned Idle=0;
static std::atomic<unsigned> phase{Idle};
}
static void check(bool ok,const char* why){
 ++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::abort();}
}
static bool camera_quiesce_wifi_for_dma(const char*);
''')
    replace("static SemaphoreHandle_t http_mutex;",
            "static SemaphoreHandle_t http_mutex, wifi_connect_mutex;")
    replace("mutex_held=true;return pdTRUE;", "mutex_held=true;if(inject_http_on_lock)http_inflight=true;return pdTRUE;")
    replace("static uint32_t wifi_inflight_start_ms, wifi_last_begin_ms;",
            "static uint32_t wifi_inflight_start_ms, wifi_last_begin_ms;\nstatic char wifi_connect_owner[24];")
    replace("  template<class... A> void printf", "  void println(const char*){}\n  template<class... A> void printf")
    replace("  void mode(int){assert(!in_callback);assert_radio_idle();}", r'''
  int getMode(){
    if(on_radio_probe){auto hook=on_radio_probe;on_radio_probe=nullptr;hook();}
    return radio_mode;
  }
  void disconnect(bool off){
    check(off&&!http_inflight&&g_camera_radio_off_owned.load(),"disconnect has camera ownership and no HTTP");
    check(sense_backup_diag::wifi_calls.load()==0,"disconnect has no executing Wi-Fi call");
    ++disconnect_calls;live_status=(wl_status_t)254;
  }
  bool mode(int next){
    assert(!in_callback);
    if(next==WIFI_STA){assert_radio_idle();check(!g_camera_radio_off_owned.load(),"STA only after camera release");}
    else {
      check(!http_inflight&&g_camera_radio_off_owned.load(),"OFF remains owned and HTTP drained");
      ++stop_calls;if(fail_radio_stop)return false;
    }
    radio_mode=next;return true;
  }
''')
    replace("struct SenseBackupWifiCall {explicit operator bool()const{return backup_allowed;}};", r'''
static bool sense_backup_offline_active(){
 if(on_admission_probe){auto hook=on_admission_probe;on_admission_probe=nullptr;hook();}
 return !backup_allowed;
}
''' + definition(backup, "class SenseBackupWifiCall") + ";")
    replace(definition(code, "static void wifi_guard_set_inflight(bool value)"),
            definition(wifi, "static void wifi_guard_set_inflight(bool inflight)"))
    replace(definition(code, "static bool wifi_guard_try_claim_connect(const char*)"),
            definition(wifi, "static bool wifi_guard_try_claim_connect(const char* owner)"))
    code += r'''
static void capture_fill_led_set(bool,const char*){}
static constexpr int ESP_LOG_NONE=0,ESP_LOG_ERROR=1;
static void esp_log_level_set(const char*,int){}
static void esp_camera_deinit(){}
static void sense_camera_grab_stop(){}
static void camera_dma_reserve_acquire(const char*){
 check(g_camera_radio_off_owned.load(),"reserve reacquired before camera ownership release");
 check(begin_calls==1,"no reconnect before reserve reacquisition");
 reserve_restored=true;
}
static void camera_stop_xclk(){}
static void camera_set_pins_high_z(){}
static void camera_power_disable(){}
static struct {unsigned getFreeHeap(){return 40000;}} ESP;
'''
    code += definition(camera, "static void deinit_camera()") + "\n"
    code += definition(camera, "static bool camera_quiesce_wifi_for_dma(")
    return code + r'''
static void setup_attempt(){
 reset();radio_mode=WIFI_OFF;reserve_restored=fail_radio_stop=inject_http_on_lock=false;
 on_radio_probe=on_admission_probe=nullptr;stop_calls=disconnect_calls=0;
 g_camera_radio_off_owned=false;sense_backup_diag::wifi_calls=0;sense_backup_diag::phase=0;
 wifi_connect_owner[0]='\0';wifi_connect_mutex=nullptr;http_mutex=(void*)1;
 service_wifi_maintenance(millis());
 check(begin_calls==1&&wifi_connect_inflight&&sense_backup_diag::wifi_calls.load()==0,
       "async begin returns while association, not SDK call, remains active");
 check(!std::strcmp(wifi_connect_owner,"wifi_maint"),"actual connection claim records owner");
 now_ms+=700;foreground_active=current_job.active=true;
}
static void check_attempt_unchanged(uint32_t started){
 check(wifi_connect_inflight&&wifi_inflight_start_ms==started,"refused stop preserves async association");
 check(!std::strcmp(wifi_connect_owner,"wifi_maint"),"refused stop preserves association owner");
 check(!wifi_maint_retry_pending&&!wifi_maint_consecutive_fails,"refused stop adds no artificial failure");
 check(!mutex_held,"HTTP lease always returned");
}
int main(){
 setup_attempt();const uint32_t began=wifi_inflight_start_ms;
 check(camera_quiesce_wifi_for_dma("async_interruption"),"camera successfully stops radio");
 check(!wifi_connect_inflight,"successful camera OFF must retire obsolete asynchronous association");
 check(wifi_inflight_start_ms==0&&wifi_connect_owner[0]=='\0',"retirement clears start and owner");
 check(!wifi_maint_retry_pending&&!wifi_maint_consecutive_fails&&last_reason.empty(),"intentional cancellation is not failure");
 check(wifi_last_begin_ms==began&&wifi_maint_last_attempt_ms==began,"normal begin cooldown history preserved");
 foreground_active=current_job.active=false;now_ms=began+WIFI_CONNECT_TIMEOUT_MS+1;
 service_wifi_maintenance(millis());
 check(begin_calls==1&&!wifi_connect_inflight&&radio_mode==WIFI_OFF,"camera ownership blocks reconnect past old timeout");
 check(!wifi_maint_retry_pending&&!wifi_maint_consecutive_fails,"held camera is not charged a connection timeout");
 foreground_active=current_job.active=true;deinit_camera();
 check(reserve_restored&&!g_camera_radio_off_owned.load()&&begin_calls==1,"teardown restores reserve and respects foreground");
 foreground_active=current_job.active=false;service_wifi_maintenance(millis());
 check(begin_calls==2&&wifi_connect_inflight&&reset_calls==0,"release reconnects without false timeout, failure backoff or hard reset");

 // An interrupted attempt still respects the preexisting ordinary begin cooldown.
 setup_attempt();const uint32_t recent=wifi_maint_last_attempt_ms;
 check(camera_quiesce_wifi_for_dma("cooldown"),"cooldown radio stop");
 deinit_camera();foreground_active=current_job.active=false;
 now_ms=recent+WIFI_BEGIN_COOLDOWN_MS-1;service_wifi_maintenance(millis());
 check(begin_calls==1&&!wifi_connect_inflight,"ordinary cooldown not erased by cancellation");
 ++now_ms;service_wifi_maintenance(millis());
 check(begin_calls==2&&reset_calls==0,"reconnect at ordinary cooldown boundary");

 // Real historical failure/backoff is untouched, even with an old async claim.
 setup_attempt();wifi_maint_consecutive_fails=2;wifi_maint_retry_pending=true;
 wifi_maint_last_failure_ms=now_ms;last_reason="real_network_failure";
 const uint32_t failure=wifi_maint_last_failure_ms;
 check(camera_quiesce_wifi_for_dma("prior_failure"),"prior failure camera stop");
 check(wifi_maint_consecutive_fails==2&&wifi_maint_retry_pending&&wifi_maint_last_failure_ms==failure,
       "real failure count, pending recovery and failure timestamp survive");
 check(last_reason=="real_network_failure","real failure diagnostic survives");
 deinit_camera();foreground_active=current_job.active=false;
 now_ms=failure+7999;service_wifi_maintenance(millis());
 check(begin_calls==1&&reset_calls==0,"real 8s cooldown retained");
 ++now_ms;service_wifi_maintenance(millis());
 check(begin_calls==2&&reset_calls==1,"existing real-failure reset policy still runs");

 // Refused/uncertain shutdown cannot retire a potentially live association.
 for(unsigned fault=0;fault<5;++fault){
   setup_attempt();const uint32_t start=wifi_inflight_start_ms;
   if(fault==0)http_inflight=true;
   if(fault==1)mutex_busy=true;
   if(fault==2)inject_http_on_lock=true;
   if(fault==3)fail_radio_stop=true;
   if(fault==4)sense_backup_diag::wifi_calls=1;
   check(!camera_quiesce_wifi_for_dma("refused"),"unsafe or failed radio stop refused");
   check_attempt_unchanged(start);
   check(!g_camera_radio_off_owned.load(),"failed new owner released");
   check(stop_calls==(fault==3?1U:0U),"radio touched only for actual SDK-stop failure");
 }

 // An executing admitted SDK call wins the race; camera waits for its destructor.
 setup_attempt();const uint32_t before_call=wifi_inflight_start_ms;
 {
   SenseBackupWifiCall executing;
   check(static_cast<bool>(executing)&&sense_backup_diag::wifi_calls.load()==1,"complete SDK call owns admission");
   check(!camera_quiesce_wifi_for_dma("executing_call"),"camera refuses existing SDK call");
   check_attempt_unchanged(before_call);
 }
 check(camera_quiesce_wifi_for_dma("call_drained")&&!wifi_connect_inflight,"retire only after executing call drains");

 // Camera wins between the entrant's first owner check and counter publication.
 setup_attempt();on_admission_probe=[](){
   check(camera_quiesce_wifi_for_dma("admission_race"),"camera wins admission publication gap");
 };
 {
   SenseBackupWifiCall losing_entrant;
   check(!static_cast<bool>(losing_entrant),"racing entrant rechecks published camera owner");
 }
 check(sense_backup_diag::wifi_calls.load()==0&&!wifi_connect_inflight,"losing entrant balances count without new claim");

 // An independent task arriving after owner publication is rejected as well.
 setup_attempt();on_radio_probe=[](){
   std::thread other([](){
     SenseBackupWifiCall blocked;
     check(!static_cast<bool>(blocked),"other task cannot admit Wi-Fi during radio stop");
   });other.join();
 };
 check(camera_quiesce_wifi_for_dma("other_task")&&!wifi_connect_inflight,"camera retains exclusive retirement boundary");
 check(sense_backup_diag::wifi_calls.load()==0,"other-task rejected admission balanced");

 setup_attempt();radio_mode=WIFI_OFF;
 check(camera_quiesce_wifi_for_dma("already_off")&&!wifi_connect_inflight,"verified already-OFF radio retires stale state");
 check(stop_calls==0&&disconnect_calls==0,"already OFF requires no radio operation");
 check(camera_quiesce_wifi_for_dma("repeat")&&!wifi_connect_inflight,"repeated quiescence is idempotent");
 check(g_camera_radio_off_owned.load(),"repeat preserves camera ownership");
 std::printf("PASS %u camera/Wi-Fi interruption checks: async retirement, admission races, stop refusal, teardown order, real backoff preservation\n",checks);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="halo-camera-wifi-") as temporary:
        out = args.out or Path(temporary)
        out.mkdir(parents=True, exist_ok=True)
        source, binary = out / "test.cpp", out / "test"
        source.write_text(harness(args.source_root))
        subprocess.run([shutil.which("clang++") or "c++", "-std=c++17", "-pthread",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-I", str(args.source_root / "Sense_Minimal"), str(source),
                        "-o", str(binary)], check=True, timeout=30, preexec_fn=no_core)
        subprocess.run([str(binary)], check=True, timeout=15, preexec_fn=no_core)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Exercise actual camera init/deinit with deterministic DMA/radio boundaries.

The host model makes the 16 KiB camera allocation unavailable while Wi-Fi owns
its buffers. This proves call ordering and cleanup, not ESP heap topology,
physical camera operation, or Wi-Fi reconnection latency.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_voice_persistence import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    camera = (root / "Sense_Minimal/sense_camera.h").read_text()
    main = (root / "Sense_Minimal/Sense_Minimal.ino").read_text()
    backup = (root / "Sense_Minimal/sense_backup_diagnostic.h").read_text()
    admission = definition(backup, "class SenseBackupWifiCall") + ";\n"
    functions = "\n".join(definition(camera, signature) for signature in (
        "static void deinit_camera()",
        "static bool camera_quiesce_wifi_for_dma(",
        "static bool init_camera()"))
    constants = []
    for name in ("CAMERA_HTTP_DRAIN_MAX_MS", "CAMERA_DMA_LARGEST_BLOCK_MIN_BYTES",
                 "CAMERA_DMA_RESERVE_BYTES", "CAMERA_DMA_RECOVER_MAX_MS",
                 "CAMERA_NETWORK_QUIESCE_DELAY_MS", "CAMERA_INIT_SETTLE_DELAY_MS",
                 "CAMERA_INIT_WARMUP_FRAMES", "CAMERA_INIT_WARMUP_DELAY_MS"):
        match = re.search(r"static const (?:size_t|uint32_t|uint8_t) " + name + r" = ([^;]+);", main)
        assert match, name
        constants.append(f"static constexpr uint32_t {name} = {match[1]};")
    return r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <climits>
#define HALO_CAMERA_KEEP_INIT 0
#define CAMERA_PREEMPTIVE_WIFI_KILL 0
static unsigned checks;
static void check(bool ok,const char* why){++checks;if(!ok){fprintf(stderr,"FAIL %s\n",why);abort();}}
static std::vector<std::string> events;
static uint32_t now_ms, release_http_at;
static bool http_inflight, foreground_active, mutex_busy, inject_http_on_lock;
static bool camera_allocated, grab_worker, fail_stop, fail_reserve;
static bool fragment_without_wifi;
static unsigned fail_init_count, init_count, radio_off_count, radio_on_count;
static unsigned reconnect_requests, reconnects;
static int radio_mode;
static size_t connected_largest;
static std::atomic<void*> g_camera_dma_reserve{nullptr};
static std::atomic<bool> g_camera_radio_off_owned{false};
namespace sense_backup_diag {
static constexpr uint8_t Idle=0;
static std::atomic<uint8_t> phase{Idle};
static std::atomic<uint32_t> wifi_calls{0};
}
static bool sense_backup_offline_active(){return false;}
''' + admission + r'''
struct Mutex {bool held=false;} mutex;
using SemaphoreHandle_t=Mutex*;
static SemaphoreHandle_t http_mutex=&mutex;
static constexpr int pdTRUE=1, WIFI_OFF=0, WIFI_STA=1, WL_CONNECTED=3;
static int xSemaphoreTake(SemaphoreHandle_t h,unsigned){
 if(mutex_busy||h->held)return 0;
 h->held=true;
 if(inject_http_on_lock)http_inflight=true;
 return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t h){check(h->held,"mutex balanced");h->held=false;}
static uint32_t millis(){return now_ms;}
static uint32_t pdMS_TO_TICKS(uint32_t n){return n;}
static void delay(uint32_t n){now_ms+=n;if(release_http_at!=UINT32_MAX&&now_ms>=release_http_at)http_inflight=false;}
static void vTaskDelay(uint32_t n){delay(n);}
static struct {
 int getMode(){return radio_mode;}
 int status(){return radio_mode==WIFI_STA?WL_CONNECTED:6;}
 void disconnect(bool off){check(off&&!http_inflight,"disconnect requires drained HTTP");events.push_back("disconnect");}
 bool mode(int m){
  check(!http_inflight,"mode change requires drained HTTP");
  if(m==WIFI_OFF){++radio_off_count;events.push_back("off");if(fail_stop)return false;}
  else {++radio_on_count;events.push_back("sta");}
  radio_mode=m;return true;
 }
} WiFi;
static void wifi_guard_set_inflight(bool inflight){
 check(!inflight&&radio_mode==WIFI_OFF&&g_camera_radio_off_owned.load(),
       "association retired only after owned radio stop");
}
static constexpr int MALLOC_CAP_DMA=1,MALLOC_CAP_INTERNAL=2;
static size_t heap_caps_get_largest_free_block(int){
 return radio_mode==WIFI_OFF&&!fragment_without_wifi?32768:connected_largest;
}
static struct {
 unsigned getFreeHeap(){return 60000;}
 unsigned getMaxAllocHeap(){return heap_caps_get_largest_free_block(3);}
 unsigned getFreePsram(){return 7000000;}
 unsigned getPsramSize(){return 8000000;}
} ESP;
static struct {
 template<class...T>void printf(const char*,T...){}
 void println(const char*){}
} Serial;
static constexpr int ESP_LOG_NONE=0,ESP_LOG_ERROR=1,ESP_OK=0;
using esp_err_t=int;
static const char* esp_err_to_name(int){return "injected";}
static void esp_log_level_set(const char*,int){}
static void capture_fill_led_set(bool,const char*){}
static void camera_stop_xclk(){events.push_back("xclk_off");}
static void camera_set_pins_high_z(){}
static void camera_power_disable(){events.push_back("power_off");}
static void camera_power_enable(){events.push_back("power_on");}
static void sense_camera_grab_stop(){grab_worker=false;events.push_back("grab_stop");}
static void sense_camera_grab_start(){grab_worker=true;events.push_back("grab_start");}
static int esp_camera_deinit(){camera_allocated=false;events.push_back("camera_free");return 0;}
static void camera_dma_reserve_release(const char*){g_camera_dma_reserve=nullptr;events.push_back("reserve_free");}
static bool camera_dma_reserve_acquire(const char*){
 check(!camera_allocated&&!grab_worker,"reserve after camera and grab worker cleanup");
 events.push_back("reserve");g_camera_dma_reserve=fail_reserve?nullptr:&events;return g_camera_dma_reserve!=nullptr;
}
static void service_wifi_maintenance(unsigned long){
 SenseBackupWifiCall admission;if(!admission)return;
 ++reconnect_requests;events.push_back("maint");
 if(foreground_active||http_inflight)return;
 check(!camera_allocated&&!grab_worker,"reconnect after camera release");
 check(std::find(events.begin(),events.end(),"reserve")!=events.end(),"reserve attempted before reconnect");
 WiFi.mode(WIFI_STA);++reconnects;
}
static int g_camera_last_init_err,g_camera_profile,g_last_scene_luma,g_last_scene_green_ratio;
static int g_camera_module;
static bool g_camera_preflight_force;
static uint32_t g_camera_first_frame_ms;
static constexpr int CAM_PROFILE_LABEL=0,CAM_PROFILE_FLASH=1,CAM_PROFILE_LOW_LIGHT=2,CAM_PROFILE_NORMAL=3;
static constexpr int CAMERA_PREFLIGHT_MODE=0,CAMERA_PREFLIGHT_ALWAYS=1,CAMERA_PREFLIGHT_ON_FAIL=2;
static constexpr int CAMERA_PREFLIGHT_LUMA_LOW=20,CAMERA_DIAG=0;
static constexpr int CAM_MODULE_UNKNOWN=0,CAM_MODULE_DCX=1,CAM_MODULE_HD3FM=2;
static constexpr int CAMERA_MODULE_DETECT_THRESHOLD_MS=500,GAINCEILING_64X=1;
static constexpr int CAMERA_GRAB_WARMUP_TIMEOUT_MS=300;
static constexpr int FILL_LED_PIN=-1,OUTPUT=1;
static constexpr int LEDC_CHANNEL_0=0,LEDC_TIMER_0=0,PIXFORMAT_JPEG=1,CAPTURE_SIZE=1;
static constexpr int JPEG_QUALITY=10,CAMERA_FB_IN_PSRAM=1,CAMERA_GRAB_LATEST=1,CAMERA_XCLK_HZ=20000000;
static constexpr int Y2_GPIO_NUM=0,Y3_GPIO_NUM=1,Y4_GPIO_NUM=2,Y5_GPIO_NUM=3,Y6_GPIO_NUM=4;
static constexpr int Y7_GPIO_NUM=5,Y8_GPIO_NUM=6,Y9_GPIO_NUM=7,XCLK_GPIO_NUM=8,PCLK_GPIO_NUM=9;
static constexpr int VSYNC_GPIO_NUM=10,HREF_GPIO_NUM=11,SIOD_GPIO_NUM=12,SIOC_GPIO_NUM=13,PWDN_GPIO_NUM=14,RESET_GPIO_NUM=15;
struct camera_config_t {
 int ledc_channel,ledc_timer,pin_d0,pin_d1,pin_d2,pin_d3,pin_d4,pin_d5,pin_d6,pin_d7;
 int pin_xclk,pin_pclk,pin_vsync,pin_href,pin_sccb_sda,pin_sccb_scl,pin_pwdn,pin_reset;
 int xclk_freq_hz,pixel_format,frame_size,jpeg_quality,fb_count,fb_location,grab_mode;
};
static int esp_camera_init(camera_config_t*){
 ++init_count;events.push_back("camera_init");
 check(!g_camera_dma_reserve,"camera reserve released before SDK allocation");
 if(fail_init_count){--fail_init_count;return -1;}
 if(heap_caps_get_largest_free_block(3)<16384)return -1;
 camera_allocated=true;return 0;
}
struct sensor_t {
 int(*reset)(sensor_t*);
 int(*set_framesize)(sensor_t*,int);
 int(*set_quality)(sensor_t*,int);
 int(*set_gainceiling)(sensor_t*,int);
};
static sensor_t* esp_camera_sensor_get(){return nullptr;}
struct camera_fb_t{size_t len;};
static camera_fb_t* sense_camera_fb_get_bounded(unsigned){return nullptr;}
static void esp_camera_fb_return(camera_fb_t*){}
static void camera_timeline_event(const char*,int32_t){}
static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){}
static void diag_record_error_persistent(const char*,int32_t,const char*){}
static const char* camera_diag_label(){return "test";}
static const char* camera_profile_name(int){return "test";}
static int compute_scene_brightness_preflight(sensor_t*,int*){return 30;}
static void apply_camera_profile(sensor_t*,int){}
static void camera_settle_discard(int,int){}
static void pinMode(int,int){}
''' + "\n".join(constants) + "\n" + functions + r'''
static void reset(){
 events.clear();now_ms=10000;release_http_at=UINT32_MAX;
 http_inflight=false;foreground_active=true;mutex_busy=false;inject_http_on_lock=false;
 mutex.held=false;http_mutex=&mutex;
 g_camera_radio_off_owned.store(false);sense_backup_diag::wifi_calls.store(0);
 camera_allocated=false;grab_worker=false;fail_stop=false;fail_reserve=false;fragment_without_wifi=false;
 fail_init_count=init_count=radio_off_count=radio_on_count=reconnect_requests=reconnects=0;
 radio_mode=WIFI_STA;connected_largest=11764;g_camera_dma_reserve=nullptr;
}
static void finish_and_reconnect(){
 deinit_camera();
 check(!camera_allocated&&!grab_worker,"deinit frees camera and worker");
 check(bool(g_camera_dma_reserve.load())!=fail_reserve,"reserve result retained");
 check(!reconnects,"foreground still prevents reconnect during deinit");
 check(radio_mode==WIFI_OFF,"radio remains off through reserve restoration");
 foreground_active=false;service_wifi_maintenance(millis());
 check(reconnects==1&&radio_mode==WIFI_STA,"maintenance reconnect after foreground release");
}
int main(){
 reset();check(init_camera(),"post-provision fragmented heap recovers");
 check(g_camera_radio_off_owned.load(),"camera publishes radio ownership");
 {SenseBackupWifiCall bypass;check(!bypass,"LIST_ACTIVE and direct reconnect admission denied while camera owns radio");}
 check(init_count==1&&radio_off_count==1&&!radio_on_count,"no STA restart before camera allocation");
 check(camera_allocated&&radio_mode==WIFI_OFF,"radio off throughout camera ownership");finish_and_reconnect();
 reset();fail_init_count=1;check(init_camera(),"retry succeeds while radio remains off");
 check(init_count==2&&radio_off_count==1&&!radio_on_count,"retry does not cycle WiFi back on");finish_and_reconnect();
 reset();fail_init_count=2;check(!init_camera(),"double init failure propagates");finish_and_reconnect();
 reset();fragment_without_wifi=true;check(!init_camera(),"unrecoverable fragmentation stays failure");finish_and_reconnect();
 reset();radio_mode=WIFI_OFF;check(init_camera(),"already-off radio supports capture");
 check(!radio_off_count&&!radio_on_count,"already-off path does not toggle radio");finish_and_reconnect();
 reset();connected_largest=40000;g_camera_dma_reserve=&events;
 check(init_camera(),"adequate-DMA normal path succeeds");
 check(!radio_off_count&&!radio_on_count,"adequate-DMA path leaves connected WiFi alone");
 deinit_camera();check(g_camera_dma_reserve&&reconnect_requests==0,"connected normal cleanup restores reserve without reconnect");
 reset();http_inflight=true;release_http_at=now_ms+40;
 check(init_camera(),"drained HTTP permits camera recovery");
 check(radio_off_count==1&&!radio_on_count,"drained request frees WiFi exactly once");finish_and_reconnect();
 reset();http_inflight=true;check(!init_camera(),"live HTTP prevents low-DMA recovery");
 check(!radio_off_count&&!radio_on_count,"HTTP timeout cannot fall through to pre-init or retry teardown");
 check(foreground_active,"drain restores previous foreground state");
 reset();mutex_busy=true;check(!init_camera(),"contended HTTP mutex preserves radio");
 check(!radio_off_count&&!radio_on_count,"mutex contention never tears down WiFi");
 reset();inject_http_on_lock=true;check(!init_camera(),"HTTP admission race preserves radio");
 check(!radio_off_count&&!radio_on_count&&!mutex.held,"race recheck releases lease without radio mutation");
 reset();fail_stop=true;check(!init_camera(),"driver stop failure is not treated as reclaimed DMA");
 check(!radio_on_count&&!mutex.held&&!g_camera_radio_off_owned.load(),"stop failure cleans mutex and owner without restarting driver");
 reset();fail_reserve=true;check(init_camera(),"capture before reserve restoration failure");finish_and_reconnect();
 reset();http_mutex=nullptr;check(init_camera(),"legacy missing-mutex configuration remains supported");finish_and_reconnect();
 reset();{SenseBackupWifiCall active;
 check(bool(active),"existing Wi-Fi call admitted before camera");
 check(!camera_quiesce_wifi_for_dma("race"),"camera waits for complete admitted Wi-Fi call");
 check(!g_camera_radio_off_owned.load()&&!radio_off_count&&!mutex.held,"failed camera admission leaves no ownership or radio side effects");}
 check(camera_quiesce_wifi_for_dma("drained"),"camera admitted after Wi-Fi call drains");
 deinit_camera();check(!g_camera_radio_off_owned.load(),"cleanup releases owner");
 reset();fail_init_count=2;check(!init_camera(),"diagnostic caller init fails");
 check(!g_camera_radio_off_owned.load()&&!camera_allocated&&!grab_worker,"init failure itself cleans owner and camera without caller deinit");
 std::printf("PASS %u checks across 15 actual camera init/deinit DMA ownership scenarios\n",checks);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="halo-camera-dma-") as temp:
        source, binary = Path(temp) / "test.cpp", Path(temp) / "test"
        actual = harness(args.source_root)
        retained = actual[:actual.index("int main(){")] + r'''
int main(){
 reset();check(init_camera(),"retained-camera initialization succeeds");
 check(camera_allocated&&!g_camera_radio_off_owned.load(),"retained camera buffers replace radio hold");
 {SenseBackupWifiCall admission;check(bool(admission),"retained-camera mode allows later upload reconnect");}
 deinit_camera();check(!camera_allocated&&!g_camera_radio_off_owned.load(),"retained-camera teardown releases all ownership");
 reset();fail_init_count=2;check(!init_camera(),"retained-camera failure propagates");
 check(!camera_allocated&&!g_camera_radio_off_owned.load(),"retained-camera failure leaves no sticky owner");
 std::printf("PASS %u optional KEEP_INIT ownership/cleanup checks\n",checks);
}
'''
        retained = retained.replace("#define HALO_CAMERA_KEEP_INIT 0", "#define HALO_CAMERA_KEEP_INIT 1")
        for program in (actual, retained):
            source.write_text(program)
            subprocess.run([shutil.which("clang++") or "c++", "-std=c++17",
                            "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()

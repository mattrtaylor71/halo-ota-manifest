#!/usr/bin/env python3
"""Run the actual camera transport-admission helper against task timing.

This verifies the foreground handoff's bounded wait and radio safety, not camera
DMA allocation, real TLS cancellation latency, or physical capture quality.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_voice_persistence import definition

ROOT = Path(__file__).resolve().parents[1]


def harness():
    camera = (ROOT / "Sense_Minimal/sense_camera.h").read_text()
    drain = definition(camera, "static bool camera_claim_network_dma(")
    limit = re.search(r"const uint32_t drain_limit_ms = (\d+);", drain)
    assert limit, "Camera drain limit must come from the actual firmware"
    return r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <climits>
static uint32_t now_ms, started_ms, release_ms, release_call_ms;
static bool http_inflight, foreground_active;
static unsigned waits, disconnects, modes, diagnostics;
static std::atomic<bool> g_camera_radio_off_owned{false};
namespace sense_backup_diag {static std::atomic<unsigned> wifi_calls{0};}
static void* http_mutex=nullptr;
static constexpr int pdTRUE=1;
static int xSemaphoreTake(void*,unsigned){return pdTRUE;}
static void xSemaphoreGive(void*){}
static constexpr unsigned WIFI_OFF=0, WIFI_STA=1;
static uint32_t millis(){return now_ms;}
static uint32_t pdMS_TO_TICKS(uint32_t value){return value;}
static void vTaskDelay(uint32_t value){
  if(value==20){assert(foreground_active);++waits;}
  now_ms+=value;
  if(release_ms!=UINT32_MAX && uint32_t(now_ms-started_ms)>=release_ms)http_inflight=false;
  if(release_call_ms!=UINT32_MAX && uint32_t(now_ms-started_ms)>=release_call_ms)sense_backup_diag::wifi_calls.store(0);
}
static void delay(uint32_t value){vTaskDelay(value);}
static struct {
  unsigned getMode(){return modes?WIFI_OFF:WIFI_STA;}
  void disconnect(bool wifioff){assert(wifioff&&!http_inflight);++disconnects;}
  bool mode(unsigned mode){assert(!http_inflight);assert(mode==WIFI_OFF);++modes;return true;}
} WiFi;
static void wifi_guard_set_inflight(bool inflight){
 assert(!inflight&&WiFi.getMode()==WIFI_OFF&&g_camera_radio_off_owned.load());
}
static struct {
  template<class... A> void printf(const char*,A...){}
  void println(const char*){}
} Serial;
static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){++diagnostics;}
''' + f"static constexpr uint32_t CAMERA_TRANSPORT_DRAIN_MAX_MS={limit.group(1)};\n" + drain + r'''
int main(){
  unsigned cases=0;
  assert(CAMERA_TRANSPORT_DRAIN_MAX_MS==20000);
  for(bool prior_foreground:{false,true})
  for(uint32_t start:{0U,1U,1000U,UINT32_MAX-1500U,UINT32_MAX-20U,UINT32_MAX})
  for(uint32_t release:{1U,19U,20U,21U,1499U,1500U,8000U,19980U,UINT32_MAX})
  for(uint32_t tail:{0U,40U,UINT32_MAX}){
    now_ms=started_ms=start;release_ms=release;http_inflight=true;
    release_call_ms=release==UINT32_MAX||tail==UINT32_MAX?UINT32_MAX:release+tail;
    foreground_active=prior_foreground;waits=disconnects=modes=diagnostics=0;
    g_camera_radio_off_owned.store(false);
    sense_backup_diag::wifi_calls.store(1);
    const bool admitted=camera_claim_network_dma();
    assert(foreground_active==prior_foreground);
    const bool drained=release_call_ms<20000;
    const uint32_t waited=drained?((release_call_ms+19)/20)*20:20000;
    assert(admitted==drained);
    assert(waits==waited/20);
    assert(uint32_t(now_ms-started_ms)==waited);
    assert(disconnects==0&&modes==0);
    assert(g_camera_radio_off_owned.load()==drained);
    assert(diagnostics==(drained?0U:1U));
    ++cases;
  }
  std::printf("PASS %u actual camera drain cases: complete transport scope required, foreground restored, no radio changes, fixed20s bound including wrap\n",cases);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="halo-camera-priority-") as temp:
        source, binary = Path(temp) / "test.cpp", Path(temp) / "test"
        source.write_text(harness())
        subprocess.run([shutil.which("clang++") or "c++", "-std=c++17",
                        "-fsanitize=address,undefined", str(source), "-o", str(binary)],
                       check=True, timeout=30)
        subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()

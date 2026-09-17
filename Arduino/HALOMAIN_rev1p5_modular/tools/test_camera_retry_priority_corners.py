#!/usr/bin/env python3
"""Run the actual camera HTTP-drain branch against injected task timing.

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
    main = (ROOT / "Sense_Minimal/Sense_Minimal.ino").read_text()
    init = definition(camera, "static bool init_camera()")
    drain = definition(init, "  if (http_inflight) {")
    limit = re.search(r"static const uint32_t CAMERA_HTTP_DRAIN_MAX_MS = (\d+);", main)
    assert limit, "Camera drain limit must come from the actual firmware"
    return r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <climits>
static uint32_t now_ms, started_ms, release_ms;
static bool http_inflight, foreground_active;
static unsigned waits, disconnects, modes, diagnostics;
static constexpr unsigned WIFI_OFF=0, WIFI_STA=1;
static uint32_t millis(){return now_ms;}
static uint32_t pdMS_TO_TICKS(uint32_t value){return value;}
static void vTaskDelay(uint32_t value){
  if(value==20){assert(foreground_active);++waits;}
  now_ms+=value;
  if(release_ms!=UINT32_MAX && uint32_t(now_ms-started_ms)>=release_ms)http_inflight=false;
}
static struct {
  void disconnect(bool wifioff){assert(wifioff&&!http_inflight);++disconnects;}
  void mode(unsigned mode){assert(!http_inflight);assert(mode==(modes?WIFI_STA:WIFI_OFF));++modes;}
} WiFi;
static struct {
  template<class... A> void printf(const char*,A...){}
  void println(const char*){}
} Serial;
static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){++diagnostics;}
''' + f"static constexpr uint32_t CAMERA_HTTP_DRAIN_MAX_MS={limit.group(1)};\n" + r'''
static void actual_camera_drain(){
''' + drain + r'''
}
int main(){
  unsigned cases=0;
  assert(CAMERA_HTTP_DRAIN_MAX_MS==1500);
  for(bool prior_foreground:{false,true})
  for(uint32_t start:{0U,1U,1000U,UINT32_MAX-1500U,UINT32_MAX-20U,UINT32_MAX})
  for(uint32_t release:{1U,19U,20U,21U,1499U,1500U,1501U,8000U,UINT32_MAX}){
    now_ms=started_ms=start;release_ms=release;http_inflight=true;
    foreground_active=prior_foreground;waits=disconnects=modes=diagnostics=0;
    actual_camera_drain();
    assert(foreground_active==prior_foreground);
    const bool drained=release<=1500;
    const uint32_t waited=drained?((release+19)/20)*20:1500;
    assert(waits==waited/20);
    assert(uint32_t(now_ms-started_ms)==waited+(drained?100:0));
    assert(disconnects==(drained?1U:0U));
    assert(modes==(drained?2U:0U));
    assert(diagnostics==(drained?0U:1U));
    ++cases;
  }
  std::printf("PASS %u actual camera drain cases: foreground state restored, live HTTP never torn down, 1500ms drain bound including clock wrap\n",cases);
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

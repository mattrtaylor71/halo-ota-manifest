#!/usr/bin/env python3
"""Execute actual Wi-Fi maintenance/event functions with faulted SDK boundaries.

No Arduino build or device/network access. The host clock uses ESP32's 32-bit
unsigned long width so rollover assertions exercise the production arithmetic.
"""
import argparse
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def section(text, first, after):
    return text[text.index(first):text.index(after, text.index(first))]


def harness(source):
    text = source.read_text()
    actual = section(text, 'static bool wifi_maintenance_work_busy()',
                     '// ── WiFi event handler')
    actual += section(text, 'static constexpr size_t WIFI_EVENT_QUEUE_CAPACITY',
                     '// ── WiFi connection functions')
    actual += section(text, 'static unsigned long wifi_maint_last_attempt_ms',
                      '// ── Time synchronization')
    constants = section(text, 'static const unsigned long WIFI_BEGIN_COOLDOWN_MS',
                        'static const unsigned long WIFI_FAIL_COOLDOWN_MS')
    actual = (constants + actual).replace('unsigned long', 'uint32_t')
    return r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include "sense_wifi_events.h"
#define HALO_SENSE_PROD_WRAPPER 1
#define portMUX_TYPE std::mutex
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
// Deliberately enum names, matching Arduino-ESP32 3.3.8 (not #defines).
enum WiFiEvent_t {ARDUINO_EVENT_WIFI_STA_START=110,
                 ARDUINO_EVENT_WIFI_STA_DISCONNECTED=113,
                 ARDUINO_EVENT_WIFI_STA_GOT_IP=115};
struct WiFiEventInfo_t {struct {uint8_t reason;} wifi_sta_disconnected;};
enum wl_status_t {WL_IDLE_STATUS=0, WL_NO_SSID_AVAIL=1, WL_CONNECTED=3,
                 WL_CONNECT_FAILED=4, WL_CONNECTION_LOST=5, WL_DISCONNECTED=6};
enum WifiGuardState {WIFI_STATE_DISCONNECTED, WIFI_STATE_CONNECTING,
                     WIFI_STATE_CONNECTED, WIFI_STATE_FAILED_TIMEOUT};
using String=std::string;
static constexpr int WIFI_STA=1, WIFI_PS_NONE=0;
static uint32_t now_ms;
static bool in_callback, provisioning, claim_busy, connect_on_claim;
static bool http_inflight, upload_inflight, foreground_active, voice_recording_active;
static bool scan_ui_inflight, dish_scan_inflight, busy_on_claim;
static struct {bool active;} current_job;
static std::atomic<bool> upload_worker_claim_active{false};
using SemaphoreHandle_t=void*;
static SemaphoreHandle_t http_mutex;
static bool mutex_busy, mutex_held;
static unsigned mutex_takes, mutex_gives;
static constexpr int pdTRUE=1;
static int xSemaphoreTake(SemaphoreHandle_t mutex,uint32_t ticks){
  assert(mutex&&ticks==0&&!mutex_held);++mutex_takes;
  if(mutex_busy)return 0;
  mutex_held=true;return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex){
  assert(mutex&&mutex_held);mutex_held=false;++mutex_gives;
}
static void assert_radio_idle(){
  assert(!http_inflight&&!upload_inflight&&!upload_worker_claim_active.load());
  assert(!foreground_active&&!current_job.active&&!voice_recording_active);
  assert(!scan_ui_inflight&&!dish_scan_inflight);
  assert(!http_mutex||mutex_held);
}
static bool backup_allowed=true;
static unsigned begin_calls, reset_calls, uart_calls, logs;
static unsigned successes, disconnections, reset_notes;
static unsigned poll_failures, poll_timeouts;
static wl_status_t live_status;
static bool wifi_connect_inflight;
static uint32_t wifi_inflight_start_ms, wifi_last_begin_ms;
static uint32_t wifi_last_poll_ms, wifi_connected_ms;
static WifiGuardState wifi_state;
static const char* WIFI_SSID="test-fallback";
static const char* WIFI_PASS="test-only";
static bool saved_credentials=true;
static std::string begun_ssid, last_reason;
static uint32_t millis(){return now_ms;}
static void delay(uint32_t ms){assert(!in_callback);now_ms+=ms;}
static struct {
  template<class... A> void printf(const char*,A...){assert(!in_callback);++logs;}
} Serial;
struct Ip {String toString(){assert(!in_callback);return "192.0.2.2";}};
static struct {
  wl_status_t status(){assert(!in_callback);return live_status;}
  int RSSI(){assert(!in_callback);return -60;}
  Ip localIP(){assert(!in_callback);return {};}
  void mode(int){assert(!in_callback);assert_radio_idle();}
  void setAutoReconnect(bool enabled){assert(!in_callback&&enabled);}
  void setSleep(bool enabled){assert(!in_callback&&!enabled);}
  void begin(const char* ssid,const char*){
    assert(!in_callback);assert_radio_idle();++begin_calls;begun_ssid=ssid;
  }
} WiFi;
static void esp_wifi_set_ps(int){assert(!in_callback);}
static void hardResetSta(){assert(!in_callback);assert_radio_idle();++reset_calls;delay(500);}
static bool halo_provisioning_active(){return provisioning;}
static bool halo_get_provisioned_wifi(char* ssid,size_t ssid_len,char* pass,size_t pass_len){
  if(!saved_credentials)return false;
  std::snprintf(ssid,ssid_len,"saved-home");std::snprintf(pass,pass_len,"saved-only");
  return true;
}
struct SenseBackupWifiCall {explicit operator bool()const{return backup_allowed;}};
static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){
  assert(!in_callback);++uart_calls;
}
static void wifi_diag_note_success(int){assert(!in_callback);++successes;}
static void wifi_diag_note_disconnect(){assert(!in_callback);++disconnections;}
static void wifi_diag_note_hard_reset(){assert(!in_callback);++reset_notes;}
static void wifi_guard_set_inflight(bool value){assert(!in_callback);wifi_connect_inflight=value;}
static void wifi_guard_set_state(WifiGuardState state,const char*,wl_status_t){
  assert(!in_callback);wifi_state=state;
}
static void wifi_guard_note_fail(const char* reason){assert(!in_callback);last_reason=reason;}
static void wifi_guard_mark_failed(wl_status_t,const char*){
  assert_radio_idle();++poll_failures;wifi_guard_set_inflight(false);
}
static void wifi_guard_handle_timeout(uint32_t){
  assert_radio_idle();++poll_timeouts;wifi_guard_set_inflight(false);
}
static bool wifi_guard_try_claim_connect(const char*){
  assert(!in_callback);
  if(claim_busy||wifi_connect_inflight)return false;
  wifi_connect_inflight=true;wifi_inflight_start_ms=millis();
  if(connect_on_claim)live_status=WL_CONNECTED;
  if(busy_on_claim)http_inflight=true;
  return true;
}
''' + actual + r'''
static void reset(){
  now_ms=1000;in_callback=provisioning=claim_busy=connect_on_claim=false;
  http_inflight=upload_inflight=foreground_active=voice_recording_active=false;
  scan_ui_inflight=dish_scan_inflight=busy_on_claim=current_job.active=false;
  upload_worker_claim_active=false;
  http_mutex=nullptr;mutex_busy=mutex_held=false;mutex_takes=mutex_gives=0;
  backup_allowed=saved_credentials=true;
  begin_calls=reset_calls=uart_calls=logs=successes=disconnections=reset_notes=0;
  poll_failures=poll_timeouts=wifi_last_poll_ms=wifi_connected_ms=0;
  live_status=WL_DISCONNECTED;wifi_connect_inflight=false;
  wifi_inflight_start_ms=wifi_last_begin_ms=0;wifi_state=WIFI_STATE_DISCONNECTED;
  wifi_maint_last_attempt_ms=wifi_maint_last_log_ms=wifi_maint_last_failure_ms=0;
  wifi_maint_retry_pending=false;wifi_maint_consecutive_fails=0;
  begun_ssid.clear();last_reason.clear();
  wifi_events={};
}
static void emit(WiFiEvent_t id,uint8_t reason=0){
  in_callback=true;handle_wifi_event(id,{{reason}});in_callback=false;
}
static void tick(uint32_t advance=0){now_ms+=advance;service_wifi_maintenance(now_ms);}
static void fail_attempt(){
  assert(wifi_connect_inflight);
  now_ms=wifi_inflight_start_ms+WIFI_CONNECT_TIMEOUT_MS+1;
  service_wifi_maintenance(now_ms);
  assert(!wifi_connect_inflight&&wifi_maint_retry_pending);
}
int main(){
  reset();tick();assert(begin_calls==1&&begun_ssid=="saved-home");
  const uint32_t original_start=wifi_inflight_start_ms;
  emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,201);
  assert(logs==1&&disconnections==0&&reset_calls==0&&wifi_connect_inflight);
  wifi_service_events();
  assert(disconnections==1&&reset_calls==0&&wifi_connect_inflight);
  assert(wifi_inflight_start_ms==original_start);
  tick(1000);assert(begin_calls==1);
  std::puts("PASS enum event callback has no radio, DNS, UART, log or wait work; transient drop retains attempt");

  // Late events from a prior reset cannot terminate/replace a newer attempt.
  emit(ARDUINO_EVENT_WIFI_STA_GOT_IP);emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,8);
  wifi_service_events();assert(successes==0&&wifi_connect_inflight&&begin_calls==1);
  live_status=WL_CONNECTED;emit(ARDUINO_EVENT_WIFI_STA_GOT_IP);wifi_service_events();
  assert(successes==1&&uart_calls==1&&reset_calls==0);
  tick();assert(wifi_maint_consecutive_fails==0&&!wifi_maint_retry_pending);
  std::puts("PASS live status rejects stale success events; association cancels recovery");

  reset();tick();
  for(unsigned failure=1;failure<=5;++failure){
    const unsigned previous_begins=begin_calls, previous_resets=reset_calls;
    fail_attempt();
    assert(wifi_maint_consecutive_fails==failure);
    assert(reset_calls==previous_resets&&begin_calls==previous_begins);
    const uint32_t cooldown=failure==1?4000:failure==2?8000:16000;
    tick(cooldown-1);assert(begin_calls==previous_begins&&reset_calls==previous_resets);
    tick(1);assert(begin_calls==previous_begins+1&&wifi_connect_inflight);
    assert(reset_calls==previous_resets+(failure<=3));
    // A reset does not consume the next 25-second association allowance.
    assert(now_ms==wifi_inflight_start_ms);
  }
  assert(reset_calls==3&&reset_notes==3);
  std::puts("PASS failure-anchored 4/8/16s delays; first three resets only; later attempts continue");

  // A success during backoff, including the claim boundary, must not reset it.
  reset();tick();fail_attempt();live_status=WL_CONNECTED;tick(4000);
  assert(begin_calls==1&&reset_calls==0&&!wifi_maint_retry_pending);
  reset();tick();fail_attempt();connect_on_claim=true;tick(4000);
  assert(begin_calls==1&&reset_calls==0&&!wifi_maint_retry_pending&&!wifi_connect_inflight);
  reset();tick();fail_attempt();claim_busy=true;tick(4000);
  assert(begin_calls==1&&reset_calls==0&&wifi_maint_retry_pending);
  claim_busy=false;tick();assert(begin_calls==2&&reset_calls==1);
  std::puts("PASS success and competing owners cancel/defer reset before radio mutation");

  // Every foreground/HTTP owner suppresses first connection, timeout changes,
  // and an already eligible retry without sleeping or consuming the backoff.
  bool* busy_flags[]={&http_inflight,&upload_inflight,&foreground_active,
                     &current_job.active,&voice_recording_active,
                     &scan_ui_inflight,&dish_scan_inflight};
  for(unsigned flag=0;flag<=sizeof(busy_flags)/sizeof(*busy_flags);++flag){
    auto busy=[&](bool value){
      if(flag==sizeof(busy_flags)/sizeof(*busy_flags))upload_worker_claim_active=value;
      else *busy_flags[flag]=value;
    };
    reset();busy(true);const auto first_now=now_ms;tick();
    assert(now_ms==first_now&&begin_calls==0&&reset_calls==0&&!wifi_connect_inflight);
    busy(false);tick();assert(begin_calls==1);
    busy(true);now_ms=wifi_inflight_start_ms+WIFI_CONNECT_TIMEOUT_MS+1;
    const auto timeout_now=now_ms;tick();
    assert(now_ms==timeout_now&&wifi_connect_inflight&&!wifi_maint_retry_pending);
    busy(false);fail_attempt();const auto failure_at=wifi_maint_last_failure_ms;
    busy(true);now_ms+=4000;const auto retry_now=now_ms;tick();
    assert(now_ms==retry_now&&reset_calls==0&&begin_calls==1);
    assert(wifi_maint_retry_pending&&wifi_maint_last_failure_ms==failure_at);
    busy(false);tick();assert(reset_calls==1&&begin_calls==2&&!wifi_maint_retry_pending);
  }
  std::puts("PASS all HTTP, upload, worker, foreground, camera and voice owners defer maintenance without waiting");

  reset();http_mutex=(void*)1;tick();fail_attempt();
  assert(mutex_takes==1&&mutex_gives==1&&!mutex_held);
  mutex_busy=true;now_ms+=4000;const auto mutex_now=now_ms;tick();
  assert(now_ms==mutex_now&&begin_calls==1&&reset_calls==0&&wifi_maint_retry_pending);
  assert(mutex_takes==2&&mutex_gives==1&&!mutex_held&&!wifi_connect_inflight);
  mutex_busy=false;busy_on_claim=true;tick();
  assert(now_ms==mutex_now&&begin_calls==1&&reset_calls==0&&wifi_maint_retry_pending);
  assert(!wifi_connect_inflight&&!mutex_held&&mutex_gives==2);
  busy_on_claim=http_inflight=false;tick();
  assert(begin_calls==2&&reset_calls==1&&!mutex_held&&mutex_gives==3);
  std::puts("PASS nonblocking HTTP lease and post-claim busy race retain pending retry, then resume once idle");

  for(const auto status:{WL_CONNECT_FAILED,WL_NO_SSID_AVAIL,WL_DISCONNECTED,
                         WL_CONNECTION_LOST,WL_IDLE_STATUS}){
    for(unsigned flag=0;flag<=sizeof(busy_flags)/sizeof(*busy_flags);++flag){
      reset();http_mutex=(void*)1;tick();live_status=status;
      now_ms=wifi_inflight_start_ms+WIFI_CONNECT_TIMEOUT_MS+1;
      if(flag==sizeof(busy_flags)/sizeof(*busy_flags))upload_worker_claim_active=true;
      else *busy_flags[flag]=true;
      const auto poll_now=now_ms;wifi_guard_poll();
      assert(now_ms==poll_now&&wifi_connect_inflight&&poll_failures==0&&poll_timeouts==0);
      if(flag==sizeof(busy_flags)/sizeof(*busy_flags))upload_worker_claim_active=false;
      else *busy_flags[flag]=false;
      mutex_busy=true;now_ms+=WIFI_POLL_INTERVAL_MS;wifi_guard_poll();
      assert(wifi_connect_inflight&&poll_failures==0&&poll_timeouts==0&&!mutex_held);
      mutex_busy=false;now_ms+=WIFI_POLL_INTERVAL_MS;wifi_guard_poll();
      assert(!wifi_connect_inflight&&!mutex_held);
      assert(poll_failures==(status!=WL_IDLE_STATUS)&&poll_timeouts==(status==WL_IDLE_STATUS));
    }
  }
  reset();tick();foreground_active=http_inflight=mutex_busy=true;http_mutex=(void*)1;
  live_status=WL_CONNECTED;wifi_guard_poll();
  assert(!wifi_connect_inflight&&wifi_state==WIFI_STATE_CONNECTED&&mutex_takes==0);
  std::puts("PASS actual poll yields all failure/timeout mutations to busy owners but accepts connected success");

  reset();tick();fail_attempt();provisioning=true;tick(100000);
  assert(begin_calls==1&&reset_calls==0);
  provisioning=false;backup_allowed=false;tick(100000);
  assert(begin_calls==1&&reset_calls==0);
  backup_allowed=true;tick();assert(begin_calls==2&&reset_calls==1);
  std::puts("PASS setup and offline diagnostic ownership suppress maintenance");

  reset();now_ms=UINT32_MAX-1000;tick();fail_attempt();
  tick(3999);assert(begin_calls==1&&reset_calls==0);
  tick(1);assert(begin_calls==2&&reset_calls==1);
  // Failure exactly at clock zero must still enforce cooldown.
  reset();tick();wifi_inflight_start_ms=UINT32_MAX-25000;now_ms=0;
  service_wifi_maintenance(now_ms);assert(wifi_maint_retry_pending);
  tick(3999);assert(begin_calls==1);tick(1);assert(begin_calls==2);
  reset();tick();wifi_maint_consecutive_fails=UINT8_MAX;
  fail_attempt();assert(wifi_maint_consecutive_fails==UINT8_MAX);
  tick(15999);assert(begin_calls==1);tick(1);assert(begin_calls==2&&reset_calls==0);
  std::puts("PASS clock rollover, zero-time failure and saturated failure counter");

  reset();
  for(unsigned i=0;i<WIFI_EVENT_QUEUE_CAPACITY+5;++i)
    emit(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,(uint8_t)i);
  assert(logs==0);
  wifi_service_events();assert(disconnections==WIFI_EVENT_QUEUE_CAPACITY);
  assert(logs==WIFI_EVENT_QUEUE_CAPACITY+1&&reset_calls==0&&begin_calls==0);
  const unsigned previous_logs=logs;wifi_service_events();assert(logs==previous_logs);
  // Queue lifetime is allocation-free; concurrent producer/consumer use the
  // exact same lock discipline as callback and owner, including overflow.
  sense_wifi_events::Queue<16> concurrent;
  std::mutex lock;std::atomic<bool> done{false};
  unsigned consumed=0;uint32_t dropped=0,last=0;bool seen=false;
  std::thread producer([&]{for(uint32_t i=0;i<100000;++i){
    std::lock_guard<std::mutex> hold(lock);concurrent.push({113,i,201});
  }done=true;});
  while(!done){
    sense_wifi_events::Event event;bool found;
    {std::lock_guard<std::mutex> hold(lock);found=concurrent.pop(event);dropped+=concurrent.take_dropped();}
    if(found){assert(!seen||event.at_ms>last);seen=true;last=event.at_ms;++consumed;}
  }
  producer.join();sense_wifi_events::Event event;
  while(concurrent.pop(event)){assert(!seen||event.at_ms>last);seen=true;last=event.at_ms;++consumed;}
  dropped+=concurrent.take_dropped();assert(consumed+dropped==100000);
  std::puts("PASS bounded event overflow reports loss; concurrent delivery keeps FIFO/accounting");
}
'''


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'Sense_Minimal/sense_wifi.h')
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('C++ compiler unavailable')
    with tempfile.TemporaryDirectory(prefix='halo-wifi-recovery-') as directory:
        source, executable = Path(directory) / 'test.cpp', Path(directory) / 'test'
        source.write_text(harness(args.source))
        subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra', '-pthread',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-I', str(ROOT / 'Sense_Minimal'), '-o', str(executable)],
                       check=True, timeout=30, preexec_fn=no_core)
        subprocess.run([str(executable)], check=True, timeout=10, preexec_fn=no_core)


if __name__ == '__main__':
    main()

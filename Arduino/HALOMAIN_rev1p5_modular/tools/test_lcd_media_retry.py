#!/usr/bin/env python3
"""Execute the real LCD timer, adapter, UART admission and SDK-arm code on host.

Only clock, RTC annotation, UART/RTOS and ESP SDK boundaries are doubles. No
hardware, network, persistent device state or media payloads are accessed.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def definition(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def harness(root):
    main = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    uart = (root / 'LCD_Minimal/lcd_uart_rx.h').read_text()
    sleep = (root / 'LCD_Minimal/lcd_sleep.h').read_text()
    # Ensure the executed helper is wired to the actual production boundaries.
    assert 'if (lcd_media_retry_uart(doc)) return;' in uart
    assert 'lcd_media_retry_wait_release("sleep_ready");' in uart
    assert 'lcd_media_retry_wait_release(reason);' in main
    assert 'if (lcd_media_retry_wait_active()) return true;' in main
    assert '(effective_timer_wake && !g_lcd_media_retry_boot && maintenance_context && maintenance_resume_hint)' in main
    assert '(effective_timer_wake && !g_lcd_media_retry_boot && g_lcd_schedule_timer_armed)' in main
    assert main.index('lcd_media_retry_note_boot(reset_reason') < main.index('bool restored_maint_state =')
    assert 'g_background_wake_dark = effective_timer_wake;' in main
    assert sleep.index('lcd_media_retry_choose_timer(') < sleep.index('configure_sleep_sources(true, sleep_timer_sec)')
    assert sleep.index('if (lcd_sleep_touch_fired())') < sleep.index('lcd_media_retry_commit_sleep(') < sleep.index('esp_deep_sleep_start();')
    assert 'if (media_timer_selected && !sleep_timer_ok)' in sleep
    actual = definition(uart, 'static bool lcd_media_retry_uart(')
    actual += '\n' + definition(main, 'static bool configure_sleep_sources(')
    return r'''
#include <ArduinoJson.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
static unsigned checks=0;
static void check(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
static unsigned critical_depth=0,storage_depth=0;
static uint64_t clock_us=0;
static int64_t esp_timer_get_time(){check(!critical_depth,"clock outside critical section");return int64_t(clock_us);}
static uint32_t millis(){check(!critical_depth,"millis outside critical section");return uint32_t(clock_us/1000);}
#define RTC_DATA_ATTR
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
static void portENTER_CRITICAL(portMUX_TYPE*){check(!critical_depth,"no recursive state lock");++critical_depth;}
static void portEXIT_CRITICAL(portMUX_TYPE*){--critical_depth;}
struct SerialMock {
  std::string output;
  size_t write(uint8_t c){output+=char(c);return 1;}
  size_t write(const uint8_t* p,size_t n){output.append(reinterpret_cast<const char*>(p),n);return n;}
  void print(char c){output+=c;}
  void flush(){}
  template<class... T> void printf(const char*,T...){check(!critical_depth,"logging outside critical section");}
} Serial,senseSerial;
#include "LCD_Minimal/lcd_media_retry.h"
static constexpr unsigned PROTOCOL_VERSION=1;
static std::atomic<bool> g_lcd_sleep_commit_gate{false};
static bool g_suppress_uart_json_tx=false,g_img_rx_active=false,g_img_rx_binary_mode=false;
static bool g_spool_tx_pending=false,g_spool_tx_active=false,ota_busy=false,provisioning=false,media_free=true;
struct LcdMaintenanceStorageGuard {LcdMaintenanceStorageGuard(){++storage_depth;}~LcdMaintenanceStorageGuard(){--storage_depth;}};
static bool lcd_ota_in_progress_for_sd_guard(){return ota_busy;}
static bool provisioning_input_locked(){return provisioning;}
static bool lcd_media_deferred_intent_pending(){return false;}
static bool lcd_media_try_claim(bool,bool){check(storage_depth==1,"admission owns sleep storage guard");return media_free;}
static void lcd_media_release(){}
static uint32_t get_next_msg_id(){return 1;}
#define HALO_ALLOW_TIMER_WAKE 1
#define HALO_WAKE_GPIO 9
#define HALO_WAKE_LEVEL 0
using esp_err_t=int;
using gpio_num_t=int;
static constexpr int ESP_OK=0,ESP_SLEEP_WAKEUP_ALL=0,ESP_EXT1_WAKEUP_ANY_LOW=0;
static bool wake_ext1_enabled=false;
static int timer_result=ESP_OK;
static uint64_t sdk_timer_us=0;
static void esp_sleep_disable_wakeup_source(int){}
static esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us){sdk_timer_us=us;return timer_result;}
static void esp_sleep_enable_ext0_wakeup(int,int){}
static void esp_sleep_enable_ext1_wakeup(uint64_t,int){}
static uint64_t buildWakeMaskForSleep(){return 128;}
''' + actual + r'''
static constexpr const char* A="0123456789abcdef";
static constexpr const char* B="fedcba9876543210";
static void reset(){
  g_lcd_media_retry_timer={};g_lcd_media_retry_checkpoint_us=0;clock_us=0;
  g_lcd_media_retry_boot=false;g_lcd_media_retry_wait_until_ms=0;
  g_lcd_sleep_commit_gate=false;g_suppress_uart_json_tx=false;g_img_rx_active=false;g_img_rx_binary_mode=false;
  g_spool_tx_pending=false;g_spool_tx_active=false;ota_busy=false;provisioning=false;media_free=true;
  timer_result=ESP_OK;sdk_timer_us=0;senseSerial.output.clear();
}
static void request(const char* token,uint32_t seconds){
  StaticJsonDocument<256> d;d["ver"]=1;d["type"]="MEDIA_RETRY_ARM";
  d["token"]=token;d["wake_in_s"]=seconds;senseSerial.output.clear();
  check(lcd_media_retry_uart(d),"typed request consumed");
}
static bool acknowledged(bool ok){
  StaticJsonDocument<256> d;
  return !deserializeJson(d,senseSerial.output) &&
      !strcmp(d["type"]|"","MEDIA_RETRY_ARM_ACK") &&
      d["ok"].is<unsigned>() && d["ok"].as<unsigned>()==unsigned(ok);
}
static uint32_t choose(uint32_t original,bool& media){return lcd_media_retry_choose_timer(original,&media);}
int main(){
  bool media=false;
  reset();request(A,300);check(acknowledged(true),"arm acknowledgment");
  check(choose(21600,media)==295 && media,"five second lead");
  clock_us=30000000;request(A,300);check(acknowledged(true),"duplicate acknowledgment");
  check(choose(21600,media)==265,"duplicate never extends deadline");
  request(A,900);check(acknowledged(false),"identity cannot change interval");
  check(choose(21600,media)==265,"conflict leaves original timer");
  request(B,900);check(acknowledged(true) && choose(21600,media)==895,"new identity replaces hint");
  request(B,0);check(acknowledged(true) && choose(200,media)==200 && !media,"explicit clear");

  reset();for(auto seconds:{1U,59U,21601U,0xffffffffU}){request(A,seconds);check(senseSerial.output.empty(),"out of bounds rejected");}
  for(const char* token:{"0123456789abcde","0123456789abcdef0","0123456789abcdeF","../3456789abcdef"}){
    request(token,300);check(senseSerial.output.empty(),"malformed token rejected");
  }
  StaticJsonDocument<256> invalid;invalid["type"]="MEDIA_RETRY_ARM";invalid["ver"]=1;
  invalid["token"]=A;invalid["wake_in_s"]=true;
  check(lcd_media_retry_uart(invalid) && senseSerial.output.empty(),"boolean interval rejected");
  invalid["wake_in_s"]=300;invalid["ver"]=2;
  check(lcd_media_retry_uart(invalid) && senseSerial.output.empty(),"wrong protocol rejected");

  reset();g_img_rx_active=true;request(A,300);check(senseSerial.output.empty() && !g_lcd_media_retry_timer.armed,"binary owner not interrupted");
  g_img_rx_active=false;g_lcd_sleep_commit_gate=true;request(A,300);check(acknowledged(false),"sleep commit refusal");
  g_lcd_sleep_commit_gate=false;ota_busy=true;request(A,300);check(acknowledged(false),"OTA ownership refusal");
  ota_busy=false;provisioning=true;request(A,300);check(acknowledged(false),"provisioning refusal");
  provisioning=false;media_free=false;request(A,300);check(acknowledged(false),"user gesture refusal");

  reset();request(A,60);
  check(choose(54,media)==54 && !media,"earlier OTA wins");
  check(choose(55,media)==55 && !media,"OTA tie wins");
  check(choose(56,media)==55 && media,"earlier media wins");
  check(choose(0,media)==55 && media,"independent no-clock timer");
  clock_us=100000000;
  check(choose(21600,media)==5 && media,"overdue media bounded away from zero");

  reset();request(A,60);uint32_t seconds=choose(21600,media);
  check(configure_sleep_sources(true,seconds) && sdk_timer_us==55000000,"real SDK argument");
  // No commit before an aborted teardown: an unrelated later timer is not media.
  clock_us=1000;lcd_media_retry_note_boot(true,true);
  check(!g_lcd_media_retry_boot && g_lcd_media_retry_timer.armed,"aborted sleep has no provenance");
  reset();request(A,60);seconds=choose(21600,media);
  timer_result=-1;
  lcd_media_retry_commit_sleep(seconds,media,configure_sleep_sources(true,seconds));
  clock_us=1000;lcd_media_retry_note_boot(true,true);
  check(!g_lcd_media_retry_boot,"failed SDK arm has no provenance");

  reset();request(A,60);seconds=choose(21600,media);
  lcd_media_retry_commit_sleep(seconds,media,configure_sleep_sources(true,seconds));
  clock_us=1000;lcd_media_retry_note_boot(true,true);
  check(g_lcd_media_retry_boot && !g_lcd_media_retry_timer.armed,"committed actual TIMER consumes media hint");
  check(lcd_media_retry_wait_active(),"bounded receiver rendezvous begins");
  request(A,60);check(acknowledged(true) && !g_lcd_media_retry_timer.armed,"late duplicate cannot rearm consumed hint");
  clock_us+=120001000ULL;check(!lcd_media_retry_wait_active(),"receiver grace cannot renew itself");
  lcd_media_retry_wait_release("sleep_ready");check(!g_lcd_media_retry_wait_until_ms.load(),"sleep completion releases grace");

  reset();request(A,60);seconds=choose(21600,media);
  lcd_media_retry_commit_sleep(seconds,media,true);
  clock_us=1000;lcd_media_retry_note_boot(true,false);
  check(!g_lcd_media_retry_boot && g_lcd_media_retry_timer.armed,"touch wake is not media");
  check(choose(21600,media)==55,"unknown touch sleep elapsed retained conservatively");
  lcd_media_retry_note_boot(false,true);
  check(!g_lcd_media_retry_boot && !g_lcd_media_retry_timer.armed,"cold/reset timer marker discarded");

  reset();request(A,60);seconds=choose(10,media);
  lcd_media_retry_commit_sleep(seconds,media,true);
  clock_us=1000;lcd_media_retry_note_boot(true,true);
  check(!g_lcd_media_retry_boot && choose(21600,media)==45,"earlier OTA timer accounts elapsed without becoming media");
  request(B,300);lcd_media_retry_note_boot(false,false);
  check(!g_lcd_media_retry_timer.armed,"cold boot relies on Sense SD rediscovery");
  check(!critical_depth && !storage_depth,"all locks released");
  std::printf("PASS LCD media retry: %u checks\n",checks);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.source_root.resolve()
    compiler = shutil.which('c++') or shutil.which('g++')
    arduino_json = root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    if not compiler or not (arduino_json / 'ArduinoJson.h').is_file():
        raise SystemExit('C++ compiler and canonical ArduinoJson source are required')
    with tempfile.TemporaryDirectory(prefix='halo-lcd-media-retry-') as directory:
        source = Path(directory) / 'test.cpp'
        executable = Path(directory) / 'test'
        source.write_text(harness(root))
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-I', str(root),
                        '-I', str(arduino_json), str(source), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Run the production OTA receive branches and Home consumer with native stubs.

Uses the installed ArduinoJson parser; only hardware, clock, and rendering are
stubbed. --uart-rx can select an older header to reproduce the navigation bug.
No device, network, firmware build, or source mutation occurs.
"""
import argparse
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def definition(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def harness(source):
    receive = source.read_text()
    main = (ROOT / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    ui = (ROOT / 'LCD_Minimal/lcd_ui_task.h').read_text()
    # Include complete production LOCK/UNLOCK branches, including validation,
    # lease bookkeeping, all early returns, and manual-result dispatch.
    start = receive.index('  if (strcmp(type, "OTA_LOCK") == 0) {')
    end = receive.index(' else if (strcmp(type, "OTA_CHECK") == 0)', start)
    handler = 'static void receive(JsonObject doc, const char* type){\n' + receive[start:end] + '\n}'
    consumer = 'static void ui_consume(){\n' + definition(ui, '    if (provision_return_home_pending)') + '\n}'
    helpers = '\n'.join(definition(main, signature) for signature in (
        'static uint32_t halo_lcd_coord_lease_ms()',
        'static void lcd_manual_ota_override_clear',
        'static void lcd_manual_ota_finish',
    ))
    return r'''
#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
// Avoid macOS crash-report collection for an expected reproducer failure.
#undef assert
#define assert(condition) do { if (!(condition)) { \
  fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition);std::exit(1); \
} } while (false)
static uint32_t now_ms=1000;
static uint32_t millis(){return now_ms;}
static struct {template<class... A>void printf(const char*,A...){} void println(const char*){}} Serial;
struct LcdMaintenanceStorageGuard {};
struct LcdCoordCriticalGuard {};
static char g_lcd_coord_owner[40];
static uint32_t g_lcd_coord_sense_boot_id, g_lcd_coord_sequence, g_lcd_coord_boot_id=77;
static std::atomic<uint32_t> g_lcd_coord_lease_until_ms{0};
static std::atomic<bool> g_lcd_sleep_commit_gate{false};
static bool ota_locked,ota_check_requested,ota_check_pending,g_lcd_ota_recovery_grace;
static bool g_ota_screen_active,g_idle_screen_dark,g_lcd_maintenance_active;
static bool sense_ota_active,sense_ota_apply_required,g_ota_mode_active;
static bool g_manual_ota_ui_active,g_manual_ota_override,provision_return_home_pending;
static std::atomic<uint8_t> g_manual_ota_result{0};
static uint32_t ota_lock_at_ms,g_lcd_maintenance_boot_grace_until_ms,ota_stay_awake_until_ms;
static uint32_t g_ota_lock_window_until_ms,g_lcd_maintenance_deadline_ms;
static uint32_t g_ota_continuation_hold_start_ms,ota_unlock_received_ms;
static uint32_t g_manual_ota_override_until_ms,g_manual_ota_result_until_ms;
static constexpr uint32_t OTA_LOCK_TIMEOUT_MS=1800000,LCD_OTA_LOCK_STAY_AWAKE_MS=180000;
static unsigned visible_calls,home_calls,hide_calls,release_calls;
static std::string screen="shopping",release_reason;
static void lcd_timer_receiver_wait_release(const char* reason){++release_calls;release_reason=reason;}
static void lcd_allow_visible_ui(const char*){++visible_calls;}
static void lcd_set_idle_screen_dark(bool value,const char*){g_idle_screen_dark=value;}
static void hide_provisioning_screen(){++hide_calls;}
static void hide_provision_intro_screen(const char*){++hide_calls;}
static void show_ship_main_menu(){++home_calls;screen="home";}
static size_t test_strlcpy(char* out,const char* in,size_t size){
  const size_t n=strlen(in);if(size)snprintf(out,size,"%s",in);return n;
}
#define strlcpy test_strlcpy
''' + helpers + '\n' + handler + '\n' + consumer + r'''
static void wire(const char* json){
  StaticJsonDocument<512> doc;assert(!deserializeJson(doc,json));
  const char* type=doc["type"];assert(type);receive(doc.as<JsonObject>(),type);
}
static void lock(){wire(R"({"type":"OTA_LOCK","coord_id":"owner","lease_ms":10000,"sense_boot_id":12,"coord_seq":1,"peer_boot_id":77})");}
static void unlock(){wire(R"({"type":"OTA_UNLOCK","coord_id":"owner","terminal":true,"result":"policy_target_valid"})");}
static void shopping(){ui_consume();assert(screen=="shopping"&&home_calls==0&&hide_calls==0);}
int main(int argc,char** argv){
  assert(argc==2);const std::string test=argv[1];
  if(test=="background"){
    g_idle_screen_dark=true;ota_check_requested=true;lock();
    assert(ota_locked&&ota_check_pending&&!ota_check_requested);
    assert(g_lcd_coord_lease_until_ms==11000&&g_ota_lock_window_until_ms==11000);
    assert(!strcmp(g_lcd_coord_owner,"owner")&&g_lcd_coord_sequence==1);
    assert(release_calls==1&&release_reason=="ota_lock");
    assert(!g_ota_screen_active&&visible_calls==0&&g_idle_screen_dark);shopping();
    now_ms=6000;unlock();assert(!ota_locked&&!g_ota_screen_active);
    assert(g_lcd_coord_lease_until_ms==0&&ota_stay_awake_until_ms==0);
    assert(release_reason=="terminal_unlock");shopping();
    // User may be browsing when a delayed duplicate arrives.
    unlock();shopping();
  }else if(test=="unsolicited"){
    unlock();shopping();
    wire(R"({"type":"OTA_UNLOCK","terminal":true})");shopping();
    wire(R"({"type":"OTA_UNLOCK"})");shopping();
    // An unrelated provisioning request must not be erased by OTA cleanup.
    provision_return_home_pending=true;unlock();assert(provision_return_home_pending);
  }else if(test=="invalid"){
    lock();g_ota_screen_active=true;const auto until=g_lcd_coord_lease_until_ms.load();
    for(const char* payload:{
        R"({"type":"OTA_UNLOCK","coord_id":"other","terminal":true})",
        R"({"type":"OTA_UNLOCK","coord_id":"","terminal":true})",
        R"({"type":"OTA_UNLOCK","coord_id":3,"terminal":true})",
        R"({"type":"OTA_UNLOCK","coord_id":"owner","terminal":"true"})"}){
      wire(payload);assert(ota_locked&&g_ota_screen_active&&g_lcd_coord_lease_until_ms==until);
      assert(!provision_return_home_pending&&release_calls==1);shopping();
    }
  }else if(test=="manual"){
    g_manual_ota_ui_active=g_manual_ota_override=g_ota_screen_active=true;
    lock();assert(g_ota_screen_active&&ota_locked);
    wire(R"({"type":"OTA_UNLOCK","coord_id":"owner"})");
    assert(g_ota_screen_active&&g_manual_ota_result==0);shopping();
    wire(R"({"type":"OTA_UNLOCK","coord_id":"owner","terminal":true,"result":"up_to_date"})");
    assert(g_ota_screen_active&&g_manual_ota_result==1&&!g_manual_ota_override);
    assert(g_manual_ota_result_until_ms==9000&&ota_stay_awake_until_ms==9000);shopping();
    now_ms=2000;unlock();assert(g_manual_ota_result==1&&g_manual_ota_result_until_ms==9000);
  }else if(test=="legacy"){
    g_idle_screen_dark=true;wire(R"({"type":"OTA_LOCK"})");
    assert(ota_locked&&g_ota_screen_active&&visible_calls==1&&!g_idle_screen_dark);
    assert(ota_stay_awake_until_ms==181000&&release_reason=="ota_lock");
    wire(R"({"type":"OTA_UNLOCK"})");
    assert(!ota_locked&&!g_ota_screen_active&&provision_return_home_pending);
    assert(ota_stay_awake_until_ms==181000);ui_consume();assert(home_calls==1&&screen=="home");
    screen="shopping";home_calls=hide_calls=0;
    wire(R"({"type":"OTA_UNLOCK","terminal":true})");shopping();
  }else if(test=="continuation"){
    g_ota_screen_active=true;g_ota_continuation_hold_start_ms=500;ota_stay_awake_until_ms=180000;
    unlock();assert(g_ota_continuation_hold_start_ms==0&&!g_ota_screen_active);
    assert(provision_return_home_pending&&ota_stay_awake_until_ms==0);
    ui_consume();assert(screen=="home"&&home_calls==1);
  }else return 2;
  printf("PASS %s\n",test.c_str());
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uart-rx', type=Path, default=ROOT / 'LCD_Minimal/lcd_uart_rx.h')
    parser.add_argument('--arduino-json', type=Path,
                        default=Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src')
    args = parser.parse_args()
    if not (args.arduino_json / 'ArduinoJson.h').is_file():
        parser.error('Provide the canonical ArduinoJson include directory with --arduino-json')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix='halo-lcd-presentation-') as directory:
        path = Path(directory); source = path / 'check.cpp'; binary = path / 'check'
        source.write_text(harness(args.uart_rx))
        subprocess.run([shutil.which('c++'), '-std=c++17', '-I', str(args.arduino_json),
                        str(source), '-o', str(binary)], check=True, timeout=30)
        failures = 0
        for case in ('background', 'unsolicited', 'invalid', 'manual', 'legacy', 'continuation'):
            result = subprocess.run([str(binary), case], timeout=5)
            failures += result.returncode != 0
        print(f'{6-failures}/6 production receive/navigation scenarios passed', flush=True)
        return int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())

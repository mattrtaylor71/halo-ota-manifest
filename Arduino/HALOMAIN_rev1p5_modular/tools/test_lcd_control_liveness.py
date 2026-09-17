#!/usr/bin/env python3
"""Actual field validation, atomic peer proof, lease expiry/unlock and handshake.

Only RTOS, clock, rendering and UART transport boundaries are controlled. This
reproduces the stale-clock peer-age decision without hardware or a cloud claim.
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def harness():
    spec = importlib.util.spec_from_file_location('maintenance', ROOT / 'tools/test_lcd_maintenance_sleep.py')
    maintenance = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(maintenance)
    define = maintenance.definition
    main = (ROOT / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    uart = (ROOT / 'LCD_Minimal/lcd_uart_rx.h').read_text()
    assert uart.index('if (!validate_protocol_message(doc))') < uart.index('lcd_note_control_awake_proof(doc);')
    assert uart.index('lcd_note_control_awake_proof(doc);') < uart.index('if (lcd_media_retry_uart(doc))')
    text = maintenance.harness(ROOT).split('int main(int argc,char** argv){', 1)[0]
    text = '#include <ArduinoJson.h>\n#include "halo_common/MediaRetryTimer.h"\n' + text
    text = text.replace('static void refresh_sense_awake_estimate(unsigned long){}', '''
using SenseState=int;
static bool sense_awake_confirmed=false,sense_rx_stale_logged=false,sense_pong_pending=false;
static uint8_t sense_missed_pongs=0;
static unsigned long sense_pong_deadline_ms=0,last_sense_any_rx_ms=0,last_sense_msg_ms=0,last_proof_of_life_ms=0;
static constexpr unsigned SENSE_MISSED_PONGS_FOR_ASLEEP=2,SENSE_UNKNOWN_STALE_MS=12000;
enum{REFRESH_IDLE,REFRESH_WAKE_PENDING,REFRESH_INFLIGHT};
static int refresh_state=REFRESH_IDLE;
static uint32_t safe_age_ms(uint32_t n,uint32_t t){return n>=t?n-t:0;}
static unsigned critical_depth=0;
static void(*after_peer_unlock)()=nullptr;
using portMUX_TYPE=int;
static int s_sense_pong_mux=0;
static void portENTER_CRITICAL(int*){assert(!critical_depth);++critical_depth;}
static void portEXIT_CRITICAL(int*){--critical_depth;if(after_peer_unlock){auto h=after_peer_unlock;after_peer_unlock=nullptr;h();}}
static void lcd_errlog_store_with_context(const char*,const char*,const char*,int,const char*){}
''' + '\n'.join(define(main, signature) for signature in (
        'static bool sense_pong_take_expired(',
        'static void note_sense_binary_media_rx(',
        'static void refresh_sense_awake_estimate(')) + '''
static constexpr uint32_t PROTOCOL_VERSION=1;
#include "LCD_Minimal/lcd_control_liveness.h"
''')
    text = text.replace('static void vTaskDelay(unsigned long);', '''
static void vTaskDelay(unsigned long);
static void(*delay_event)()=nullptr;
''')
    text = text.replace('now_ms+=delay;++delay_calls;', 'now_ms+=delay;++delay_calls;if(delay_event){delay_event();return;}')
    unlock = define(uart, 'if (strcmp(type, "OTA_UNLOCK") == 0)')
    text += '''
static char g_lcd_coord_owner[40]="retained-origin";
static std::atomic<uint32_t> g_lcd_coord_lease_until_ms{0};
static bool g_lcd_ota_recovery_grace=false,g_manual_ota_ui_active=false;
static unsigned long ota_unlock_received_ms=0;
static void lcd_manual_ota_finish(const char*){}
static void lcd_manual_ota_override_clear(const char*){}
static void actual_unlock(JsonDocument& doc){const char* type="OTA_UNLOCK";
''' + unlock + '\n}\n'
    return text + r'''
static unsigned proof_checks=0;
static JsonDocument message(const char* type){
  JsonDocument d;d["type"]=type;d["ver"]=1;d["msg_id"]=7;d["ts"]=0;return d;
}
static void prove(JsonDocument& d,bool expected){
  ++proof_checks;last_sense_rx_ms=1;sense_state=SENSE_ASLEEP;sense_awake_estimate=false;
  sleep_ready_received=true;last_sense_sleep_ready_ms=1;sense_pong_pending=true;
  const auto idle=last_user_activity_ms,lease=ota_stay_awake_until_ms;
  lcd_note_control_awake_proof(d);
  assert(lcd_control_awake_proof(d)==expected);
  assert(last_user_activity_ms==idle&&ota_stay_awake_until_ms==lease);
  if(expected){assert(sense_state==SENSE_AWAKE&&last_sense_rx_ms==now_ms&&!sleep_ready_received&&!sense_pong_pending);}
  else{assert(sense_state==SENSE_ASLEEP&&last_sense_rx_ms==1&&sleep_ready_received&&sense_pong_pending);}
}
static void validation(){
  now_ms=120000;
  auto d=message("FW_INFO");prove(d,false);d["sense_fw"]="";prove(d,false);
  d["sense_fw"]=0;prove(d,false);d["sense_fw"]="6.4.170";prove(d,true);
  d["msg_id"]=0;prove(d,false);d["msg_id"]=7;d["ts"]="0";prove(d,false);
  d["ts"]=0;d["ver"]=true;prove(d,false);d["ver"]=1;prove(d,true);
  d=message("LCD_OTA_QUERY");prove(d,true);d.remove("msg_id");prove(d,false);
  d["msg_id"]=0;prove(d,false);d["msg_id"]=7;d["ts"]=true;prove(d,false);
  d["ts"]=0;prove(d,true);d["coord_id"]=0;prove(d,false);
  d["coord_id"]="";prove(d,false);d["coord_id"]="retained-origin";prove(d,true);
  d=message("SENSE_DIAG");d["area"]="wifi";d["event"]="rssi_report";
  d["label"]="connected";d["code"]=-60;d["detail"]="rssi=-60 ip=192.0.2.1";prove(d,true);
  d["code"]=0;prove(d,false);d["code"]=true;prove(d,false);
  d["label"]="disconnected";d["code"]=6;prove(d,true);d["code"]=0;prove(d,true);
  d["code"]=256;prove(d,false);d["code"]=6;d["detail"]="";prove(d,false);
  d["detail"]="status=6";d["event"]="other";prove(d,false);
  d=message("MEDIA_RETRY_ARM");d["token"]="0123456789abcdef";d["wake_in_s"]=300;prove(d,true);
  d["wake_in_s"]=0;prove(d,true);d["wake_in_s"]=59;prove(d,false);d["wake_in_s"]=21601;prove(d,false);
  d["wake_in_s"]=300;d["token"]="0123456789abcdeg";prove(d,false);
  d=message("UNRECOGNIZED");prove(d,false);
}
static void fresh_query(){auto d=message("LCD_OTA_QUERY");lcd_note_control_awake_proof(d);}
static void pong_ordering(){
  for(bool proof_first:{false,true}){
    now_ms=120000;sense_state=SENSE_ASLEEP;sense_missed_pongs=2;sense_pong_pending=true;
    sense_pong_deadline_ms=now_ms;sleep_ready_received=true;last_sense_sleep_ready_ms=1;
    if(proof_first)fresh_query();else after_peer_unlock=fresh_query;
    sense_pong_take_expired(now_ms);
    assert(sense_state==SENSE_AWAKE&&last_sense_rx_ms==now_ms&&!sense_pong_pending&&!sleep_ready_received);
  }
}
static unsigned long handshake_started;
static bool unlock_sent;
static void receive_unlock(){auto d=message("OTA_UNLOCK");d["terminal"]=true;actual_unlock(d);unlock_sent=true;}
static void handshake_events(){
  // Recorded168 race: unlock arrived11.281ms after stale decision, final READY
  //14.835s later. The40ms controlled task service observes the first event once.
  if(!unlock_sent&&now_ms-handshake_started>=12)receive_unlock();
  if(now_ms-handshake_started>=14835)sleep_ready_received=true;
}
static void clock_timeout_ordering(bool unlock_first){
  now_ms=120000;last_sense_rx_ms=1026;last_proof_of_life_ms=1026;
  sense_state=SENSE_UNKNOWN;sense_awake_estimate=false;sleep_ready_received=false;last_sense_sleep_ready_ms=0;
  link_synced=false;provisioning_active=false;sleep_messages=0;
  ota_locked=true;ota_stay_awake_until_ms=119999;g_ota_lock_window_until_ms=0;
  g_ota_continuation_hold_start_ms=0;g_ota_screen_active=false;g_lcd_validation_pending=false;
  g_lcd_maintenance_timer_armed=1;g_lcd_maintenance_wake_in_s=3500;g_lcd_maintenance_remaining_s=3600;
  last_user_activity_ms=last_scroll_activity_ms=9000;home_shown_ms=9000;
  unlock_sent=false;fresh_query();assert(ota_locked&&ota_stay_awake_until_ms==119999);
  if(unlock_first)receive_unlock();
  handshake_started=now_ms;delay_event=handshake_events;scenario="control_race";
  const bool result=notify_sense_sleep();delay_event=nullptr;
  assert(result&&unlock_sent&&sleep_messages==1&&sleep_fallback_timer_sec==0);
  assert(now_ms-handshake_started>=14835&&now_ms-handshake_started<15000);
  assert(!ota_locked&&!ota_stay_awake_until_ms&&!g_ota_lock_window_until_ms);
  assert(last_user_activity_ms==9000&&last_scroll_activity_ms==9000&&home_shown_ms==9000);
  assert(g_lcd_maintenance_timer_armed==1&&g_lcd_maintenance_wake_in_s==3500&&g_lcd_maintenance_remaining_s==3600);
  assert(!link_synced&&!g_panel_enabled&&relights==0);
}
int main(){
  validation();pong_ordering();clock_timeout_ordering(false);clock_timeout_ordering(true);
  printf("PASS validated control liveness: %u payload cases, both PONG orderings, both lease-expiry/unlock handshakes\n",proof_checks);
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='halo-control-life-') as directory:
        source = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        source.write_text(harness())
        arduino_json = ROOT / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
        subprocess.run([shutil.which('c++'), '-std=c++17', '-Wno-deprecated-declarations', '-I', str(ROOT),
                        '-I', str(arduino_json), str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()

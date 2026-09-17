"""Actual LCD service/OTA_LOCK/release regression; fake I/O, no device access."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
JSON=Path.home()/'Documents/Arduino/libraries/ArduinoJson/src'


def definition(source, signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


def harness(baseline=False):
    wrapper=ROOT/'halo_ota_demo/firmware/halo_lcd_prod/halo_lcd_prod.ino'
    source=wrapper.read_text()
    if baseline:
        source=subprocess.check_output(['git','show','4e549d454dc7e8f9a38734881e7ba3cfc5e85c23:Arduino/HALOMAIN_rev1p5_modular/halo_ota_demo/firmware/halo_lcd_prod/halo_lcd_prod.ino'],cwd=ROOT,text=True)
    lcd=(ROOT/'LCD_Minimal/LCD_Minimal.ino').read_text()
    rx=(ROOT/'LCD_Minimal/lcd_uart_rx.h').read_text()
    lock_start=rx.index('  if (strcmp(type, "OTA_LOCK") == 0) {')
    lock_end=rx.index('    } else {\n      LcdCoordCriticalGuard guard;',lock_start)
    # Execute the complete typed/correlated branch, not the unrelated plain UI lock.
    lock='static void actual_OTA_LOCK(JsonObject& doc){const char*type="OTA_LOCK";\n'+rx[lock_start:lock_end]+'\n}}}\n'
    prefix=r'''
#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using String=std::string;
static constexpr bool baseline=BASELINE;
static constexpr int ESP_SLEEP_WAKEUP_TIMER=4,PROTOCOL_VERSION=1;
static constexpr unsigned SENSE_RECENT_RX_FOR_SLEEP_MS=5000;
static uint32_t now_ms=1000;
static uint32_t millis(){return now_ms;}
static std::atomic<uint32_t> g_lcd_coord_lease_until_ms{0},g_lcd_timer_receiver_wait_until_ms{0};
static std::atomic<bool> g_lcd_boot_ready{true},g_lcd_coord_notice_clear{false},g_lcd_sleep_commit_gate{false};
static uint32_t g_lcd_coord_boot_id=77,g_lcd_coord_sense_boot_id,g_lcd_coord_sequence;
static char g_lcd_coord_owner[40];
static bool ota_locked,g_lcd_ota_uart_receiving,g_lcd_ota_binary_mode,g_ota_mode_active,g_img_rx_binary_mode,g_spool_tx_pending,g_spool_tx_active,g_suppress_uart_json_tx;
static bool g_lcd_ota_recovery_grace,ota_check_requested,ota_check_pending;
static uint32_t ota_lock_at_ms,g_lcd_maintenance_boot_grace_until_ms,ota_stay_awake_until_ms,g_ota_lock_window_until_ms;
static bool sense_awake_confirmed=true,link_synced=true,link_sync_pending;
static uint32_t last_sense_rx_ms=1000,g_lcd_coord_notice_next_ms;
static struct {char schedule[64];uint32_t epoch;int wake,reset;uint8_t resumes;bool active;} g_lcd_coord_notice;
static unsigned wake_calls,cancel_calls,lock_depth;static bool begin_on_lock;
struct LcdMaintenanceStorageGuard {LcdMaintenanceStorageGuard(){++lock_depth;if(begin_on_lock)g_lcd_ota_uart_receiving=true;}~LcdMaintenanceStorageGuard(){--lock_depth;}};
struct LcdCoordCriticalGuard {};
static struct {template<class...A>void printf(const char*,A...){}void println(const char*){}} Serial;
static uint32_t get_next_msg_id(){static uint32_t n=0;return ++n;}
static void request_sense_wake(const char*){++wake_calls;}
static void lcd_media_retry_wait_release(const char*){}
static void lcd_coord_cancel_preflight(bool only_expired){
 if(g_lcd_coord_lease_until_ms&&(!only_expired||int32_t(now_ms-g_lcd_coord_lease_until_ms)>=0)){
  ++cancel_calls;g_lcd_coord_lease_until_ms=0;ota_locked=false;
 }
}
static void lcd_coord_service_clear(){if(g_lcd_coord_notice_clear&&!ota_locked&&!g_lcd_timer_receiver_wait_until_ms&&!g_lcd_coord_lease_until_ms)g_lcd_coord_notice.active=false;}
static std::vector<std::string> notices;
static void uart_send_json(const char*text){assert(!g_lcd_ota_uart_receiving&&!g_lcd_ota_binary_mode&&!g_img_rx_binary_mode&&!g_spool_tx_pending&&!g_spool_tx_active);if(!baseline)assert(lock_depth>0);notices.emplace_back(text);}
'''.replace('BASELINE',str(baseline).lower())
    functions='\n'.join(definition(lcd,x) for x in ('static uint32_t halo_lcd_coord_lease_ms()', 'static void lcd_timer_receiver_wait_release(', 'static bool lcd_timer_receiver_wait_active()'))
    if not baseline:functions+='\n'+definition(source,'static bool lcd_coord_preflight_notice_allowed(')
    functions+='\n'+lock+'\n'+definition(source,'static void lcd_coord_service()')
    cases=r'''
static void setup(){
 now_ms=1000;g_lcd_coord_lease_until_ms=0;g_lcd_timer_receiver_wait_until_ms=121000;
 g_lcd_coord_notice={};strcpy(g_lcd_coord_notice.schedule,"nightly_20260914");g_lcd_coord_notice.epoch=1789426800;g_lcd_coord_notice.wake=4;g_lcd_coord_notice.reset=8;g_lcd_coord_notice.active=true;
 g_lcd_coord_notice_clear=false;g_lcd_boot_ready=true;g_lcd_sleep_commit_gate=false;
 ota_locked=g_lcd_ota_uart_receiving=g_lcd_ota_binary_mode=g_ota_mode_active=g_img_rx_binary_mode=g_spool_tx_pending=g_spool_tx_active=g_suppress_uart_json_tx=false;
 g_lcd_coord_sense_boot_id=g_lcd_coord_sequence=0;g_lcd_coord_owner[0]=0;g_lcd_coord_notice_next_ms=0;
 sense_awake_confirmed=link_synced=true;last_sense_rx_ms=1000;wake_calls=cancel_calls=0;begin_on_lock=false;notices.clear();
}
static void lock(unsigned sequence=1){
 JsonDocument d;d["coord_id"]="current-proof-owner";d["lease_ms"]=12000u;d["sense_boot_id"]=800u;d["coord_seq"]=sequence;d["peer_boot_id"]=77u;
 auto obj=d.as<JsonObject>();actual_OTA_LOCK(obj);
}
int main(int argc,char**argv){
 setup();lcd_coord_service();assert(notices.size()==1);const std::string first=notices[0];notices.clear(); // lose first notice
 now_ms=1100;lock();const uint32_t deadline=g_lcd_coord_lease_until_ms;
 assert(ota_locked&&deadline==13100&&g_lcd_timer_receiver_wait_until_ms==0&&g_lcd_coord_notice.active&&!g_lcd_coord_notice_clear);
 now_ms=3000;lcd_coord_service();
 if(baseline){assert(notices.empty());puts("PASS baseline reproduces lost first notice permanently suppressed by accepted OTA_LOCK");return 0;}
 assert(notices.size()==1&&g_lcd_coord_lease_until_ms==deadline&&g_lcd_timer_receiver_wait_until_ms==0&&wake_calls==0);
 JsonDocument a,b;assert(deserializeJson(a,first)==DeserializationError::Ok&&deserializeJson(b,notices[0])==DeserializationError::Ok);
 for(const char*k:{"type","schedule_id"})assert(!strcmp(a[k].as<const char*>(),b[k].as<const char*>()));
 for(const char*k:{"peer_boot_id","origin_wake","origin_reset","origin_epoch"})assert(a[k].as<uint32_t>()==b[k].as<uint32_t>());
 assert(b["resumed"].as<bool>()==false);
 if(argc>1){FILE*f=fopen(argv[1],"wb");assert(f);fwrite(notices[0].data(),1,notices[0].size(),f);fputc('\n',f);fclose(f);}
 puts("PASS lost first notice + actual correlated OTA_LOCK/release still retransmits identical TIMER origin within original lease");
 now_ms=4000;lock(2);assert(g_lcd_coord_lease_until_ms==deadline&&g_lcd_timer_receiver_wait_until_ms==0);
 now_ms=5000;sense_awake_confirmed=link_synced=false;last_sense_rx_ms=0;lcd_coord_service();assert(notices.size()==2&&wake_calls==0);
 now_ms=deadline;lcd_coord_service();assert(notices.size()==2&&g_lcd_coord_lease_until_ms==0&&g_lcd_coord_notice_clear);
 puts("PASS lock replay never extends deadline; no extra wake after handoff; announcement stops at original expiry");
 for(unsigned which=0;which<12;++which){
  setup();lock();now_ms=4000;
  switch(which){case0:g_lcd_ota_uart_receiving=true;break;case1:g_lcd_ota_binary_mode=true;break;case2:g_ota_mode_active=true;break;case3:g_img_rx_binary_mode=true;break;case4:g_spool_tx_pending=true;break;case5:g_spool_tx_active=true;break;case6:g_suppress_uart_json_tx=true;break;case7:g_lcd_coord_notice_clear=true;break;case8:g_lcd_coord_notice.wake=0;break;case9:strcpy(g_lcd_coord_notice.schedule,"relative_1789426800_133");break;case10:strcpy(g_lcd_coord_notice.schedule,"nightly_bad_date_");break;case11:g_lcd_coord_lease_until_ms=0;break;}
  lcd_coord_service();assert(notices.empty()&&wake_calls==0);
 }
 puts("PASS binary/image/spool/preparation/suppression/cleared/non-TIMER/non-calendar/unowned guards prevent late notice");
 setup();lock();now_ms=4000;begin_on_lock=true;lcd_coord_service();assert(notices.empty());
 puts("PASS BEGIN taking ownership before final send recheck prevents JSON crossing into OTA reception");
 setup();sense_awake_confirmed=link_synced=false;last_sense_rx_ms=0;lcd_coord_service();assert(wake_calls==1&&notices.size()==1);
 puts("PASS original receiver-wait handshake remains available before lock handoff");
}
'''.replace('case0:','case 0:').replace('case1:','case 1:').replace('case2:','case 2:').replace('case3:','case 3:').replace('case4:','case 4:').replace('case5:','case 5:').replace('case6:','case 6:').replace('case7:','case 7:').replace('case8:','case 8:').replace('case9:','case 9:').replace('case10:','case 10:').replace('case11:','case 11:')
    return prefix+functions+cases


def main():
    ap=argparse.ArgumentParser();ap.add_argument('--baseline',action='store_true');ap.add_argument('--notice-output',type=Path);a=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-calendar-notice-') as tmp:
        cpp=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';cpp.write_text(harness(a.baseline))
        subprocess.run(['clang++','-std=c++17','-Wno-deprecated-declarations','-I',str(JSON),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)]+([str(a.notice_output)] if a.notice_output else []),check=True)


if __name__=='__main__':main()

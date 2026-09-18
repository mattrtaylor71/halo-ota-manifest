#!/usr/bin/env python3
"""Compose production NVS restore, guarded TX, startup probe and FW_INFO.

Actual response validation, async mailbox ownership and firmware serialization
run against ArduinoJson. Preferences, RTOS, UART and clock are explicit doubles;
no hardware/cloud. A seeded negative control restores the former setup gate.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_sense_uart_json_tx import definition

ROOT = Path(__file__).resolve().parents[1]


def harness():
    uart = (ROOT / 'Sense_Minimal/sense_uart.h').read_text()
    ota = (ROOT / 'Sense_Minimal/sense_ota_lcd.h').read_text()
    main = (ROOT / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    prod = (ROOT / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino').read_text()
    guard = uart[uart.index('// LCD OTA proxy UART ownership flag'):
                 uart.index('// ── UART ring buffer & protocol state')]
    locks = uart[uart.index('static StaticSemaphore_t uart_json_tx_mutex_storage;'):
                 uart.index('// ── TX/RX type tracking')]
    mailbox = ota[ota.index('static volatile bool g_lcd_ota_query_resp_ready'):
                  ota.index('static volatile bool g_lcd_ota_begin_ack_ready')]
    query = ota[ota.index('struct LcdOtaQuerySnapshot {'):
                ota.index('// Reset destroys the old receiver session/handle.')]
    response = definition(main, '} else if (strcmp(type, "LCD_OTA_QUERY_RESP") == 0)')[len('} else '):]
    service = definition(prod, 'static void ota_control_probe_service()')
    needle = 'if (g_lcd_ota_proxy_owns_uart || sense_action_inflight() || foreground_active ||'
    assert needle in service, 'Review startup admission before updating seeded negative'
    service = service.replace(needle, '#ifdef RESTORE_SETUP_GATE\n'
                              '  if (g_provisioning_manager.isSetupModeActive()) return;\n'
                              '#endif\n  ' + needle)
    return r'''
#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
using String=std::string;
#define HALO_SENSE_PROD_WRAPPER 1
#define HALO_DEBUG_SENSITIVE 0
static unsigned checks;
static void check(bool yes,const char* message){++checks;if(!yes){fprintf(stderr,"FAIL %s\n",message);abort();}}
static uint32_t now=1000,lock_delay=0,next_random=1;
static bool fail_lock=false,short_write=false;
static uint32_t millis(){return now;}
static void delay(uint32_t n){now+=n;}
static uint32_t esp_random(){return next_random++;}
static unsigned pdMS_TO_TICKS(unsigned n){return n;}
static const int pdTRUE=1;
struct StaticSemaphore_t{bool held=false;};
using SemaphoreHandle_t=StaticSemaphore_t*;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s){return s;}
static std::function<void()> on_lock;
static int xSemaphoreTake(SemaphoreHandle_t s,unsigned){
  now+=lock_delay;lock_delay=0;if(on_lock){auto call=on_lock;on_lock=nullptr;call();}
  if(fail_lock||s->held)return 0;s->held=true;return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t s){assert(s->held);s->held=false;}
static std::string logs,wire;
static struct {
  void println(const char* text){logs+=text;logs+='\n';}
  template<class...A> void printf(const char* format,A...args){char text[2048];snprintf(text,sizeof(text),format,args...);logs+=text;}
}Serial;
static bool namespace_present=false,marker_present=false,saved_unsafe=true;
static bool read_failure=false,write_failure=false;
static unsigned marker_writes=0,due_writes=0;
static bool durable_due=false;
struct Preferences {
  bool begin(const char* name,bool readonly){check(!strcmp(name,"lcd_xfer"),"only transport namespace opened");
    if(readonly)return namespace_present&&!read_failure;return !write_failure;}
  bool isKey(const char*){return marker_present;}
  bool getBool(const char*,bool fallback){return marker_present&&!read_failure?saved_unsafe:fallback;}
  size_t putBool(const char*,bool value){++marker_writes;if(write_failure)return 0;
    namespace_present=marker_present=true;saved_unsafe=value;return 1;}
  void end(){}
};
static bool g_spool_owns_uart=false;
static std::atomic<bool> g_img_spool_tx_active{false},g_img_spool_request_active{false};
static std::atomic<int> s_img_ready_result{-2};
static bool sense_img_spool_binary_pending(){return s_img_ready_result.load()==1;}
static bool sense_img_spool_begin_matches(const char*){return false;}
''' + guard + '\n' + locks + r'''
static size_t uart_rx_ring_count=0,uart_rx_frame_len=0;
static bool uart_rx_frame_overflow=false;
static unsigned uart_tx_count=0,last_uart_tx_ms=0;
static void uart_note_tx_type(const char*){}
static struct {
  int available(){return 0;}
  int read(){return -1;}
  size_t print(const char* text){check(uart_json_tx_mutex->held,"wire write holds TX lock");
    const size_t n=strlen(text);if(short_write)return 0;wire+=text;return n;}
  void flush(){check(uart_json_tx_mutex->held,"wire flush holds TX lock");}
}lcdSerial;
static const unsigned PROTOCOL_VERSION=1,LCD_OTA_PROXY_QUERY_TIMEOUT_MS=7000;
static uint32_t get_next_msg_id(){static uint32_t id=1;return id++;}
''' + definition(uart, 'static bool uart_send_json(') + '\n' + mailbox + '\n' + query + r'''
static void uart_reset_rx_state(){uart_rx_ring_count=uart_rx_frame_len=0;uart_rx_frame_overflow=false;}
static void pump_uart_rx_once(){}
static void sense_lcd_terminal_flush(){}
''' + definition(ota, 'static bool sense_lcd_ota_query(char*') + r'''
template<class...A>static bool sense_lcd_query_proof_emit(A&&...){return true;}
''' + definition(uart, 'static bool validate_protocol_message(') + r'''
static bool receive(const char* text){
  JsonDocument doc;if(deserializeJson(doc,text)||!validate_protocol_message(doc))return false;
  const char* type=doc["type"]|"";
''' + response + r'''
  return true;
}
static const char* kFirmwareVersion="6.4.host",*kBuildId="factory-startup-fixture";
struct esp_partition_t{const char* label;};
static esp_partition_t running{"app0"};
static const esp_partition_t* esp_ota_get_running_partition(){return &running;}
static const esp_partition_t* esp_ota_get_boot_partition(){return &running;}
enum esp_ota_img_states_t{ESP_OTA_IMG_NEW,ESP_OTA_IMG_PENDING_VERIFY,ESP_OTA_IMG_VALID,ESP_OTA_IMG_INVALID,ESP_OTA_IMG_ABORTED,ESP_OTA_IMG_UNDEFINED};
static const int ESP_OK=0;
static int esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t* state){*state=ESP_OTA_IMG_VALID;return ESP_OK;}
static int esp_sleep_get_wakeup_cause(){return 0;}
static int esp_reset_reason(){return 1;}
static char g_lcd_ota_version[32]={0};
''' + definition(main, 'static const char* get_sense_fw_version()') + '\n' + definition(main, 'static void uart_send_fw_info(') + r'''
static struct {bool active=false;}g_peer_gate;
static bool g_boot_ota_pending=false,g_ota_check_requested=false,g_ota_check_in_progress=false;
static bool g_ota_apply_in_progress=false,g_lcd_ota_task_running=false,g_peer_episode_finished=false;
static bool foreground_active=false,voice_recording_active=false,g_list_screen_active=false,action_inflight=false;
static bool sense_action_inflight(){return action_inflight;}
static struct {bool setup=true;bool isSetupModeActive(){return setup;}}g_provisioning_manager;
static void set_lcd_ota_due_nvs(bool due){++due_writes;durable_due=due;}
''' + prod[prod.index('static bool g_control_probe_active ='):prod.index('static void ota_peer_service();')] + '\n' + definition(prod, 'static bool ota_control_probe_stop(') + '\n' + service + r'''
static void reset(bool existing_due=false){
  check(!uart_json_tx_mutex->held&&!uart_rx_mutex->held,"all leases released");
  now=1000;lock_delay=0;fail_lock=short_write=false;on_lock=nullptr;wire.clear();logs.clear();
  namespace_present=marker_present=false;saved_unsafe=true;read_failure=write_failure=false;marker_writes=due_writes=0;
  durable_due=existing_due;g_lcd_ota_proxy_owns_uart=g_spool_owns_uart=false;
  g_img_spool_tx_active=false;g_img_spool_request_active=false;s_img_ready_result=-2;
  s_lcd_query_pending=false;s_lcd_query_sync_active=false;s_lcd_query_proof_held=false;
  g_lcd_ota_query_resp_ready=false;g_lcd_query_coord_id[0]=0;s_lcd_query_requested_id[0]=0;
  g_control_probe_active=g_control_probe_finished=g_control_probe_querying=false;
  g_control_probe_deadline_ms=g_control_probe_next_ms=0;g_control_probe_challenge[0]=0;
  g_peer_gate.active=g_boot_ota_pending=g_ota_check_requested=g_ota_check_in_progress=false;
  g_ota_apply_in_progress=g_lcd_ota_task_running=g_peer_episode_finished=false;
  foreground_active=voice_recording_active=g_list_screen_active=action_inflight=false;g_provisioning_manager.setup=true;
  uart_rx_ring_count=uart_rx_frame_len=0;uart_rx_frame_overflow=false;
  sense_lcd_mode_restore();
}
static JsonDocument reply(){
  JsonDocument d;d["ver"]=1;d["type"]="LCD_OTA_QUERY_RESP";d["msg_id"]=123;d["ts"]=now;
  d["lcd_fw"]="6.4.host";d["ota_part_size"]=2621440;d["running_part"]="app0";
  d["running_state"]="VALID";d["boot_part"]="app0";d["boot_ready"]=true;
  d["coord_id"]=g_control_probe_challenge;d["peer_boot_id"]=456;d["recovery_idle"]=true;
  d["coord_waiting"]=false;d["coord_owner"]="";d["coord_lease_ms"]=0;return d;
}
static void deliver(JsonDocument& d){std::string text;serializeJson(d,text);receive(text.c_str());}
static void begin(){ota_control_probe_service();check(g_control_probe_querying,"setup mode admits actual guarded query");
  check(wire.find("LCD_OTA_QUERY")!=std::string::npos&&wire.back()=='\n',"query frame reaches wire");}
static void complete(){auto d=reply();deliver(d);ota_control_probe_service();}
int main(){
  uart_json_tx_init();reset();
#ifdef RESTORE_SETUP_GATE
  uart_send_fw_info(false);ota_control_probe_service();
  check(wire.empty()&&g_lcd_ota_mode_unconfirmed&&!g_control_probe_querying,"negative control reproduces setup quarantine deadlock");
  puts("PASS seeded negative: restored setup gate strands blank-NVS FW_INFO and probe");return 0;
#endif
  for(unsigned kind=0;kind<4;++kind){
    reset(kind%2);namespace_present=kind!=0;marker_present=kind>=2;read_failure=kind==3;saved_unsafe=true;
    sense_lcd_mode_restore();check(g_lcd_ota_mode_unconfirmed&&s_lcd_mode_marker_pending,"unsafe restore stays fail closed");
    uart_send_fw_info(false);check(wire.empty()&&logs.find("FW_INFO skipped")!=std::string::npos&&logs.find("FW_INFO sent")==std::string::npos,"fast FW_INFO log reflects rejected write");
    const uint32_t before=now;uart_send_fw_info(true);
    check(now==before&&wire.empty()&&logs.find("LCD_OTA_QUERY skipped")!=std::string::npos,"ordinary diagnostic query fails promptly without bypass");
    begin();check(g_lcd_ota_mode_unconfirmed,"query emission alone does not confirm mode");complete();
    check(!g_lcd_ota_mode_unconfirmed&&!s_lcd_query_proof_held&&!s_lcd_mode_marker_pending,"safe reply releases proof and persists marker");
    check(wire.find("FW_INFO")!=std::string::npos&&logs.find("FW_INFO sent")!=std::string::npos,"startup delivers actual FW_INFO after proof");
    check(due_writes==0&&durable_due==bool(kind%2),"startup preserves preexisting debt exactly");
  }
  reset();namespace_present=marker_present=true;saved_unsafe=false;sense_lcd_mode_restore();
  ota_control_probe_service();check(!g_control_probe_active&&wire.empty(),"saved-safe boot needs no probe");
  unsigned rejected=0;
#define REJECT(change) do{reset(true);begin();auto d=reply();deliver(d);change;deliver(d);ota_control_probe_service();\
    check(g_lcd_ota_mode_unconfirmed&&!s_lcd_query_proof_held&&marker_writes==0,"unsafe reply retains quarantine without proof leak");\
    check(durable_due&&due_writes==0,"bad proof leaves stored obligation unchanged");\
    wire.clear();uart_send_fw_info(false);check(wire.empty(),"bad proof cannot emit ordinary identity");++rejected;}while(0)
  REJECT(d["coord_id"]="stale");REJECT(d.remove("coord_id"));REJECT(d["coord_id"]=123);
  REJECT(d["recovery_idle"]=false);REJECT(d.remove("recovery_idle"));REJECT(d["recovery_idle"]="true");
  REJECT(d["peer_boot_id"]=0);REJECT(d["peer_boot_id"]="456");REJECT(d["coord_owner"]="other");
  REJECT(d["coord_lease_ms"]=1);REJECT(d["coord_waiting"]=true);
  REJECT(d.remove("coord_waiting"));REJECT(d.remove("coord_owner"));REJECT(d.remove("coord_lease_ms"));
  REJECT(d["boot_ready"]=false);REJECT(d["running_state"]="PENDING_VERIFY");REJECT(d["running_state"]="INVALID");
  REJECT(d["running_state"]="UNKNOWN");REJECT(d["boot_part"]="app1");REJECT(d["ota_part_size"]=0);
  REJECT(d["running_part"]="?");REJECT(d["lcd_fw"]="");
#undef REJECT
  reset();begin();auto d=reply();d["recovery_idle"]=false;deliver(d);ota_control_probe_service();
  now+=200;begin();d=reply();deliver(d);ota_control_probe_service();check(!g_lcd_ota_mode_unconfirmed,"rejected strong proof releases admission for retry");
  reset();begin();d=reply();deliver(d);now+=3000;ota_control_probe_service();
  check(g_lcd_ota_mode_unconfirmed&&!s_lcd_query_pending&&!s_lcd_query_proof_held,"exact per-query deadline rejects response");
  for(bool existing : {false,true}){
    reset(existing);begin();now=g_control_probe_deadline_ms;complete();
    check(g_lcd_ota_mode_unconfirmed&&g_control_probe_finished&&!s_lcd_query_pending&&due_writes==0&&durable_due==existing,"total timeout cannot invent or erase debt");
    wire.clear();now+=200000;ota_control_probe_service();check(wire.empty(),"finished startup budget cannot renew itself");
  }
  reset();now=0xfffffff0U;begin();complete();check(!g_lcd_ota_mode_unconfirmed,"startup deadline survives millis wrap");
  for(bool* owner : {&g_peer_gate.active,&g_boot_ota_pending,&g_ota_check_requested,&g_ota_check_in_progress,&g_ota_apply_in_progress,&g_lcd_ota_task_running,
                    &g_spool_owns_uart,&foreground_active,&voice_recording_active,&g_list_screen_active,&action_inflight}){
    reset();*owner=true;ota_control_probe_service();check(wire.empty()&&g_lcd_ota_mode_unconfirmed,"existing work blocks new startup query");*owner=false;
  }
  reset();g_lcd_ota_proxy_owns_uart=true;ota_control_probe_service();check(wire.empty()&&g_lcd_ota_mode_unconfirmed,"OTA owner blocks startup query");
  for(unsigned owner=0;owner<3;++owner){reset();if(owner==0)g_img_spool_tx_active=true;if(owner==1)g_img_spool_request_active=true;if(owner==2)s_img_ready_result=1;
    ota_control_probe_service();check(wire.empty()&&g_lcd_ota_mode_unconfirmed,"media owner blocks actual probe TX");}
  reset();begin();d=reply();deliver(d);g_img_spool_tx_active=true;ota_control_probe_service();
  check(g_lcd_ota_mode_unconfirmed&&!s_lcd_query_proof_held,"new media owner rejects held proof");
  reset();begin();g_ota_check_requested=true;ota_control_probe_service();
  check(!s_lcd_query_pending&&!g_control_probe_querying&&g_control_probe_finished,"real request cancels startup mailbox");
  reset();begin();now+=3000;check(sense_lcd_ota_query_start("replacement",1000),"another caller replaces expired startup query");
  d=reply();d["coord_id"]="replacement";deliver(d);ota_control_probe_service();
  check(s_lcd_query_pending&&g_lcd_ota_query_resp_ready&&!strcmp(s_lcd_query_requested_id,"replacement"),"old poll preserves replacement response");
  g_ota_check_requested=true;g_control_probe_querying=true;ota_control_probe_service();
  check(s_lcd_query_pending&&g_lcd_ota_query_resp_ready,"startup cancellation preserves replacement owner");
  reset();begin();d=reply();deliver(d);on_lock=[] {on_lock=[] {now+=3000;};};ota_control_probe_service();
  check(g_lcd_ota_mode_unconfirmed&&!s_lcd_query_proof_held,"proof admission cannot extend per-query deadline");
  reset();begin();d=reply();deliver(d);g_control_probe_deadline_ms=now+1;lock_delay=1;ota_control_probe_service();
  check(g_lcd_ota_mode_unconfirmed&&!s_lcd_query_proof_held,"lock wait cannot admit proof at overall deadline");
  reset();begin();write_failure=true;complete();
  check(!g_lcd_ota_mode_unconfirmed&&s_lcd_mode_marker_pending,"live proof permits RAM recovery when marker clear fails");
  sense_lcd_mode_restore();check(g_lcd_ota_mode_unconfirmed,"marker clear failure requires new proof next boot");
  reset();short_write=true;ota_control_probe_service();
  check(!g_control_probe_querying&&!s_lcd_query_pending&&g_lcd_ota_mode_unconfirmed,"partial TX cancels own pending query");
  logs.clear();uart_send_fw_info(false);check(logs.find("FW_INFO sent")==std::string::npos,"rejected send cannot log FW_INFO sent");
  reset();fail_lock=true;ota_control_probe_service();check(wire.empty()&&!g_control_probe_querying,"TX lock failure cannot claim a sent query");fail_lock=false;
  check(!uart_json_tx_mutex->held&&!uart_rx_mutex->held&&!s_lcd_query_proof_held,"all terminal paths release ownership");
  printf("PASS %u composed factory UART checks, %u unsafe response cases; no hardware/cloud\n",checks,rejected);
}
'''


def main():
    include = ROOT / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    with tempfile.TemporaryDirectory(prefix='halo-factory-uart-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        cpp.write_text(harness())
        for negative in (False, True):
            binary = Path(tmp) / ('negative' if negative else 'test')
            command = [shutil.which('clang++') or 'c++', '-std=c++17', '-O1',
                       '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                       '-I', str(include), str(cpp), '-o', str(binary)]
            if negative:
                command.append('-DRESTORE_SETUP_GATE=1')
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()

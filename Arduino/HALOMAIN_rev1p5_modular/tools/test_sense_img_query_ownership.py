#!/usr/bin/env python3
"""Execute actual Sense query/photo admission and RX-owner composition.

The production classes, async query start/poll, complete synchronous query and
query functions are compiled unchanged; raw collection is tested separately. Deterministic semaphore, serial, clock
and mailbox doubles inject the ownership handoffs; this is not hardware proof.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def definition(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def harness(root):
    uart = (root / 'Sense_Minimal/sense_uart.h').read_text()
    ota = (root / 'Sense_Minimal/sense_ota_lcd.h').read_text()
    photo = (root / 'Sense_Minimal/sense_img_spool.h').read_text()
    tx = uart[uart.index('static StaticSemaphore_t uart_json_tx_mutex_storage;'):
              uart.index('// ── TX/RX type tracking')]
    query = ota[ota.index('struct LcdOtaQuerySnapshot {'):
                ota.index('// Reset destroys the old receiver session/handle.')]
    return r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <ArduinoJson.h>
using String=std::string;
#define HALO_SENSE_PROD_WRAPPER 1
static unsigned checks;
static void check(bool b,const char* n){++checks;if(!b){fprintf(stderr,"FAIL %s\n",n);abort();}}
static uint32_t now=1000;
static uint32_t millis(){return now;}
static void delay(uint32_t n){now+=n;}
static uint32_t pdMS_TO_TICKS(uint32_t n){return n;}
static const int pdTRUE=1;
struct StaticSemaphore_t {bool held=false;};
using SemaphoreHandle_t=StaticSemaphore_t*;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s){return s;}
static int xSemaphoreTake(SemaphoreHandle_t s,uint32_t n){assert(s&&n<=2000);if(s->held)return 0;s->held=true;return pdTRUE;}
static void xSemaphoreGive(SemaphoreHandle_t s){assert(s&&s->held);s->held=false;}
static bool g_spool_owns_uart=false,g_lcd_ota_proxy_owns_uart=false;
static std::atomic<bool> g_img_spool_request_active{false};
static uint32_t s_img_ready_job=0;
static std::atomic<int> s_img_ready_result{-2};
static std::atomic<bool> g_img_spool_tx_active{false},g_lcd_ota_mode_unconfirmed{false};
''' + definition(uart, 'static bool sense_uart_ordinary_tx_allowed()') + '\n' + tx + r'''
static std::function<void()> on_read,on_send,on_confirm,on_dispatch;
static unsigned reads=0,sends=0,confirms=0,dispatches=0,resets=0;
static struct SerialPort {
  std::string bytes;
  int available(){return (int)bytes.size();}
  int read(){assert(uart_rx_mutex->held);++reads;if(on_read)on_read();
    if(bytes.empty())return -1;int c=(unsigned char)bytes.front();bytes.erase(0,1);return c;}
}lcdSerial;
static struct Log {template<class...T>void printf(const char*,T...){}void println(const char*){}}Serial;
static size_t uart_rx_ring_count=0,uart_rx_frame_len=0;
static bool uart_rx_frame_overflow=false;
static unsigned long uart_rx_dropped_since_frame=0;
static const bool UART_RX_DEBUG=false;
static bool uart_rx_is_printable(char c){return c>=32&&c<127;}
static void uart_ring_push(char){assert(uart_rx_mutex->held);++uart_rx_ring_count;}
static void uart_reset_rx_state(){assert(uart_rx_mutex->held);++resets;uart_rx_ring_count=uart_rx_frame_len=0;uart_rx_frame_overflow=false;}
static void uart_collect_rx_once(){}
static void uart_process_rx_ring(){assert(!uart_rx_mutex->held);++dispatches;if(on_dispatch)on_dispatch();uart_rx_ring_count=0;}
static const unsigned PROTOCOL_VERSION=1,LCD_OTA_PROXY_QUERY_TIMEOUT_MS=7000;
static uint32_t get_next_msg_id(){static uint32_t id=0;return ++id;}
static bool uart_send_json(const char*,bool){assert(!uart_json_tx_mutex->held&&!uart_rx_mutex->held);++sends;if(on_send)on_send();return true;}
static void sense_lcd_mode_confirm(){++confirms;if(on_confirm)on_confirm();g_lcd_ota_mode_unconfirmed=false;}
static bool g_lcd_ota_query_resp_ready=false,g_lcd_query_boot_ready=true;
static char g_lcd_ota_query_resp_fw[32]="6.4.host",g_lcd_query_running_part[16]="app0";
static char g_lcd_query_running_state[20]="VALID",g_lcd_query_boot_part[16]="app0";
static uint32_t g_lcd_ota_query_resp_part_size=0x200000;
''' + query + '\n' + definition(photo, 'class SenseImgSpoolLease {') + ';\n' + definition(uart, 'static void pump_uart_rx_once()') + r'''
static void sense_lcd_terminal_flush(){}
''' + definition(ota, 'static bool sense_lcd_ota_query(char*') + r'''
static void reset(){
 check(!uart_json_tx_mutex->held&&!uart_rx_mutex->held,"prior leases released");
 now=1000;on_read=on_send=on_confirm=on_dispatch=nullptr;reads=sends=confirms=dispatches=resets=0;
 s_lcd_query_pending=false;s_lcd_query_sync_active=false;s_lcd_query_proof_held=false;
 g_img_spool_tx_active=false;g_spool_owns_uart=g_lcd_ota_proxy_owns_uart=false;g_lcd_ota_mode_unconfirmed=false;
 g_lcd_ota_query_resp_ready=false;lcdSerial.bytes.clear();uart_rx_ring_count=uart_rx_frame_len=0;uart_rx_frame_overflow=false;
}
static void response(const char* id){strlcpy(g_lcd_query_coord_id,id,sizeof(g_lcd_query_coord_id));g_lcd_query_peer_boot_id=123;g_lcd_ota_query_resp_ready=true;}
int main(){
 uart_json_tx_init();reset();LcdOtaQuerySnapshot out{};
 on_send=[] {SenseImgSpoolLease photo(42);check(!photo.held(),"pending published before TX handoff blocks photo");};
 check(sense_lcd_ota_query_start("one",1000),"async query admitted");
 check(sends==1&&!uart_rx_mutex->held,"async admission neither retains RX nor nests sender lock");
 {SenseImgSpoolLease photo(42);check(!photo.held(),"unexpired async query excludes photo");}
 response("wrong");check(sense_lcd_ota_query_poll(out)==LCD_QUERY_WAITING&&confirms==0,"wrong nonce cannot confirm mode");
 response("one");on_confirm=[] {SenseImgSpoolLease photo(42);check(!photo.held(),"mode confirmation and photo claim serialized");};
 check(sense_lcd_ota_query_poll(out)==LCD_QUERY_READY&&confirms==1&&out.correlated,"actual correlated readiness confirms once");
 {SenseImgSpoolLease photo(42);check(photo.held()&&g_img_spool_request_active&&!uart_rx_mutex->held,"photo reserves request after completed query");
  check(!sense_lcd_ota_query_start("two",1000),"photo excludes async query");
  SenseLcdSyncQueryLease query;check(!query.held(),"photo excludes sync query");
  lcdSerial.bytes="binary";pump_uart_rx_once();check(reads==0&&lcdSerial.bytes=="binary","modeled pump does not read while admission tested");}
 check(!g_img_spool_tx_active&&!uart_rx_mutex->held,"photo releases only own custody");
 reset();check(sense_lcd_ota_query_start("old",50),"prepare abandoned async query");now+=50;response("old");
 {SenseImgSpoolLease photo(42);check(photo.held(),"original async deadline releases abandoned admission");
  g_lcd_ota_mode_unconfirmed=true;
  check(sense_lcd_ota_query_poll(out)==LCD_QUERY_TIMEOUT&&confirms==0&&g_lcd_ota_mode_unconfirmed,
        "late old poll cannot clear new photo quarantine");}
 reset();check(sense_lcd_ota_query_start("proof",1000),"prepare stronger proof");response("proof");
 check(sense_lcd_ota_query_poll(out,false)==LCD_QUERY_READY&&s_lcd_query_proof_held&&confirms==0,"unconfirmed proof keeps exclusive admission");
 {SenseImgSpoolLease photo(42);check(!photo.held(),"ready mailbox cannot release custody before strong proof validation");}
 // The production reboot-probe's scoped ProofLease releases this flag after
 // strong validation/failure; that full callsite has its separate existing test.
 s_lcd_query_proof_held=false;
 {SenseImgSpoolLease photo(42);check(photo.held(),"photo can claim after proof scope closes");}
 reset();char fw[32];uint32_t size=0;lcdSerial.bytes="stale bytes";
 on_read=[] {check(s_lcd_query_sync_active,"raw sync drain retains query admission");SenseImgSpoolLease photo(42);check(!photo.held(),"raw-reader lease excludes photo during stale FIFO drain");};
 on_send=[] {SenseImgSpoolLease photo(42);check(!photo.held(),"sync sender handoff retains query admission");g_lcd_ota_query_resp_ready=true;};
 check(sense_lcd_ota_query(fw,sizeof(fw),&size,nullptr,1000)&&!strcmp(fw,"6.4.host")&&size==0x200000,
       "actual synchronous query succeeds with serialized drain and original reply semantics");
 check(!s_lcd_query_sync_active&&!uart_rx_mutex->held&&reads==11&&resets==1,"successful sync query releases both leases");
 reset();lcdSerial.bytes.assign(1025,'x');
 check(!sense_lcd_ota_query(fw,sizeof(fw),&size,nullptr,1000)&&reads==1024&&sends==0,
       "actual raw sync drain refuses over-cap input before send");
 check(!s_lcd_query_sync_active&&!uart_rx_mutex->held,"drain refusal cannot strand photo admission");
 reset();check(!sense_lcd_ota_query(fw,sizeof(fw),&size,nullptr,50)&&now==1050,"sync query obeys original total timeout");
 {SenseImgSpoolLease photo(42);check(photo.held(),"sync timeout releases photo admission");}
 reset();lcdSerial.bytes="{}\n";on_dispatch=[] {check(!uart_rx_mutex->held,"normal callbacks execute outside RX lease");SenseImgSpoolLease photo(42);check(photo.held(),"callback can enter separately owned photo transaction");};
 pump_uart_rx_once();check(reads==0&&dispatches==1,"query callback fixture releases RX; actual collection tested separately");
 reset();g_lcd_ota_mode_unconfirmed=true;{SenseImgSpoolLease photo(42);check(!photo.held(),"quarantine still blocks new photo");}
 reset();g_lcd_ota_proxy_owns_uart=true;{SenseImgSpoolLease photo(42);check(!photo.held(),"OTA ownership still blocks new photo");}
 reset();g_spool_owns_uart=true;{SenseImgSpoolLease photo(42);check(!photo.held(),"photo ownership still blocks new photo");}
 reset();{SenseImgSpoolLease photo(42);check(photo.held(),"request admission independent of unrelated clock expiry");}
 check(!uart_json_tx_mutex->held&&!uart_rx_mutex->held,"all terminal paths close both locks");
 printf("PASS %u actual-source query/photo ownership checks\n",checks);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', default=Path(__file__).resolve().parents[1], type=Path)
    a = p.parse_args()
    json_include = a.source_root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    with tempfile.TemporaryDirectory(prefix='halo-photo-uart-') as temp:
        cpp = Path(temp) / 'test.cpp'
        exe = Path(temp) / 'test'
        cpp.write_text(harness(a.source_root))
        subprocess.run([shutil.which('clang++') or 'c++', '-std=c++17', '-O1', '-Wno-deprecated-declarations',
                        '-I', str(json_include), str(cpp), '-o', str(exe)], check=True, timeout=30)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Execute actual Sense query/voice admission and RX-owner composition.

The production classes, async query start/poll, complete synchronous query and
normal RX pump are compiled unchanged. Deterministic semaphore, serial, clock
and mailbox doubles inject the ownership handoffs; this is not hardware proof.
"""
import argparse
import re
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


def harness(root, negative_reader_root=None):
    uart = (root / 'Sense_Minimal/sense_uart.h').read_text()
    ino = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    frame_max = int(re.search(r'^#define UART_RX_FRAME_MAX (\d+)', ino, re.M)[1])
    ota = (root / 'Sense_Minimal/sense_ota_lcd.h').read_text()
    voice = (root / 'Sense_Minimal/sense_voice_spool.h').read_text()
    reader = ((negative_reader_root / 'Sense_Minimal/sense_voice_spool.h').read_text()
              if negative_reader_root else voice)
    photo = (root / 'Sense_Minimal/sense_img_spool.h').read_text()
    funcs = '\n'.join(definition(uart, name) for name in (
        'static bool uart_rx_is_printable(', 'static void uart_reset_rx_state()',
        'static void uart_ring_push(', 'static bool uart_ring_pop(',
        'static void uart_process_rx_ring()', 'static void uart_collect_rx_once()'))
    tx = uart[uart.index('static uint32_t sense_msg_id_counter'):
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
#include <vector>
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
static std::function<void(SemaphoreHandle_t)> on_unlock;
static void xSemaphoreGive(SemaphoreHandle_t s){assert(s&&s->held);s->held=false;auto callback=on_unlock;if(callback)callback(s);}
static bool g_spool_owns_uart=false,g_lcd_ota_proxy_owns_uart=false;
static std::atomic<bool> g_img_spool_request_active{false};
static uint32_t s_img_ready_job=0;
static std::atomic<int> s_img_ready_result{-2};
static bool sense_img_spool_binary_pending(){return s_img_ready_result.load()==1;}
static bool sense_img_spool_on_json(const char*){return false;} // photo mailbox has a separate actual-source suite
static constexpr size_t UART_RX_RING_SIZE=2048,UART_RX_FRAME_MAX=''' + str(frame_max) + r''';
static bool parse_input_message(const char*);
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
static std::vector<std::string> dispatched;
static bool parse_input_message(const char* line){
  check(!uart_rx_mutex->held&&!uart_json_tx_mutex->held,"callbacks hold neither RX nor TX");
  check(uart_dispatch_depth.load()>0,"callback admission spans unlocked dispatch");
  ++dispatches;dispatched.emplace_back(line);if(on_dispatch)on_dispatch();return true;
}
''' + funcs + r'''
static const unsigned PROTOCOL_VERSION=1,LCD_OTA_PROXY_QUERY_TIMEOUT_MS=7000;
static uint32_t get_next_msg_id(){static uint32_t id=0;return ++id;}
static void uart_send_json(const char*,bool){assert(!uart_json_tx_mutex->held&&!uart_rx_mutex->held);++sends;if(on_send)on_send();}
static void sense_lcd_mode_confirm(){++confirms;if(on_confirm)on_confirm();g_lcd_ota_mode_unconfirmed=false;}
static bool g_lcd_ota_query_resp_ready=false,g_lcd_query_boot_ready=true;
static char g_lcd_ota_query_resp_fw[32]="6.4.host",g_lcd_query_running_part[16]="app0";
static char g_lcd_query_running_state[20]="VALID",g_lcd_query_boot_part[16]="app0";
static uint32_t g_lcd_ota_query_resp_part_size=0x200000;
''' + query + '\n' + definition(voice, 'static uint32_t sense_voice_spool_remaining(') + '\n' + definition(voice, 'class SenseVoiceUartLease {') + ';\n' + definition(photo, 'class SenseImgSpoolLease {') + ';\n' + definition(reader, 'static bool sense_voice_spool_read(') + '\n' + definition(uart, 'static void pump_uart_rx_once()') + r'''
static void sense_lcd_terminal_flush(){}
''' + definition(ota, 'static bool sense_lcd_ota_query(char*') + r'''
static void reset(){
 check(!uart_json_tx_mutex->held&&!uart_rx_mutex->held,"prior leases released");
 now=1000;on_read=on_send=on_confirm=on_dispatch=nullptr;on_unlock=nullptr;reads=sends=confirms=dispatches=resets=0;dispatched.clear();
 s_lcd_query_pending=false;s_lcd_query_sync_active=false;s_lcd_query_proof_held=false;
 g_img_spool_request_active=false;s_img_ready_result=-2;uart_dispatch_depth=0;g_img_spool_tx_active=false;g_spool_owns_uart=g_lcd_ota_proxy_owns_uart=false;g_lcd_ota_mode_unconfirmed=false;
 g_lcd_ota_query_resp_ready=false;lcdSerial.bytes.clear();uart_reset_rx_state();
}
static void response(const char* id){strlcpy(g_lcd_query_coord_id,id,sizeof(g_lcd_query_coord_id));g_lcd_query_peer_boot_id=123;g_lcd_ota_query_resp_ready=true;}
int main(){
 uart_json_tx_init();reset();LcdOtaQuerySnapshot out{};
 on_send=[] {SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"pending published before TX handoff blocks voice");};
 check(sense_lcd_ota_query_start("one",1000),"async query admitted");
 check(sends==1&&!uart_rx_mutex->held,"async admission neither retains RX nor nests sender lock");
 {SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"unexpired async query excludes voice");}
 response("wrong");check(sense_lcd_ota_query_poll(out)==LCD_QUERY_WAITING&&confirms==0,"wrong nonce cannot confirm mode");
 response("one");on_confirm=[] {SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"mode confirmation and voice claim serialized");};
 check(sense_lcd_ota_query_poll(out)==LCD_QUERY_READY&&confirms==1&&out.correlated,"actual correlated readiness confirms once");
 {SenseVoiceUartLease voice(millis()+1000);check(voice.held()&&g_img_spool_tx_active&&uart_rx_mutex->held,"voice owns RX after completed query");
  check(!sense_lcd_ota_query_start("two",1000),"voice excludes async query");
  SenseLcdSyncQueryLease query;check(!query.held(),"voice excludes sync query");
  lcdSerial.bytes="binary";pump_uart_rx_once();check(reads==0&&lcdSerial.bytes=="binary","normal pump cannot consume voice COBS");}
 check(!g_img_spool_tx_active&&!uart_rx_mutex->held,"voice releases only own custody");
 reset();check(sense_lcd_ota_query_start("old",50),"prepare abandoned async query");now+=50;response("old");
 {SenseVoiceUartLease voice(millis()+1000);check(voice.held(),"original async deadline releases abandoned admission");
  g_lcd_ota_mode_unconfirmed=true;
  check(sense_lcd_ota_query_poll(out)==LCD_QUERY_TIMEOUT&&confirms==0&&g_lcd_ota_mode_unconfirmed,
        "late old poll cannot clear new voice quarantine");}
 reset();check(sense_lcd_ota_query_start("proof",1000),"prepare stronger proof");response("proof");
 check(sense_lcd_ota_query_poll(out,false)==LCD_QUERY_READY&&s_lcd_query_proof_held&&confirms==0,"unconfirmed proof keeps exclusive admission");
 {SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"ready mailbox cannot release custody before strong proof validation");}
 // The production reboot-probe's scoped ProofLease releases this flag after
 // strong validation/failure; that full callsite has its separate existing test.
 s_lcd_query_proof_held=false;
 {SenseVoiceUartLease voice(millis()+1000);check(voice.held(),"voice can claim after proof scope closes");}
 reset();char fw[32];uint32_t size=0;lcdSerial.bytes="stale bytes";
 on_read=[] {check(s_lcd_query_sync_active,"raw sync drain retains query admission");SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"raw-reader lease excludes voice during stale FIFO drain");};
 on_send=[] {SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"sync sender handoff retains query admission");g_lcd_ota_query_resp_ready=true;};
 check(sense_lcd_ota_query(fw,sizeof(fw),&size,nullptr,1000)&&!strcmp(fw,"6.4.host")&&size==0x200000,
       "actual synchronous query succeeds with serialized drain and original reply semantics");
 check(!s_lcd_query_sync_active&&!uart_rx_mutex->held&&reads==11&&uart_rx_frame_len==0&&uart_rx_ring_count==0,"successful sync query releases both leases");
 reset();lcdSerial.bytes.assign(1025,'x');
 check(!sense_lcd_ota_query(fw,sizeof(fw),&size,nullptr,1000)&&reads==1024&&sends==0,
       "actual raw sync drain refuses over-cap input before send");
 check(!s_lcd_query_sync_active&&!uart_rx_mutex->held,"drain refusal cannot strand voice admission");
 reset();check(!sense_lcd_ota_query(fw,sizeof(fw),&size,nullptr,50)&&now==1050,"sync query obeys original total timeout");
 {SenseVoiceUartLease voice(millis()+1000);check(voice.held(),"sync timeout releases voice admission");}
 reset();lcdSerial.bytes="{}\n";on_dispatch=[] {check(!uart_rx_mutex->held,"normal callbacks execute outside RX lease");SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"callback admission refuses a concurrent voice BEGIN");};
 pump_uart_rx_once();check(reads==3&&dispatches==1,"actual normal pump dispatches complete read batch");
 reset();g_lcd_ota_mode_unconfirmed=true;{SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"quarantine still blocks new voice");}
 reset();g_lcd_ota_proxy_owns_uart=true;{SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"OTA ownership still blocks new voice");}
 reset();g_spool_owns_uart=true;{SenseVoiceUartLease voice(millis()+1000);check(!voice.held(),"photo ownership still blocks new voice");}
 reset();{SenseVoiceUartLease voice(millis());check(!voice.held(),"expired transaction cannot claim UART");}


 reset();JsonDocument full;full["ver"]=1;full["type"]="VOICE_SPOOL_FETCH_READY";full["voice_schema"]=1;
 const std::string request(32,'a');full["request_id"]=request;full["job_id"]=42;full["len"]=320000;full["crc32"]=4294967295u;full["ok"]=1;
 JsonObject meta=full["meta"].to<JsonObject>();meta["voice_schema"]=1;meta["kind"]="voice";meta["pcm_fmt"]="s16le_mono";meta["rate"]=16000;
 meta["owner_id"]=std::string(63,'o');meta["device_id"]=std::string(31,'d');meta["session_id"]=std::string(95,'s');meta["request_id"]=request;
 meta["job_id"]=42;meta["len"]=320000;meta["crc32"]=4294967295u;meta["epoch"]=1789500000;meta["retries"]=1;
 std::string full_wire;serializeJson(full,full_wire);check(full_wire.size()>512&&full_wire.size()<1024,"valid maximum-identity FETCH_READY exceeds old512 frame");
 lcdSerial.bytes=full_wire+"\n";
 {SenseVoiceUartLease lease(millis()+100);JsonDocument reply;
  check(sense_voice_spool_read(reply,"VOICE_SPOOL_FETCH_READY",request.c_str(),millis()+100),"actual shipping frame capacity accepts full voice FETCH_READY metadata");}
 // The old candidate002 private reader recognized its reply but silently lost
 // these whole ordinary frames. Use exactly the production successor reader.
 const std::string input="{\"ver\":1,\"type\":\"INPUT_MENU_SELECT\",\"menu_item\":\"Dish\",\"msg_id\":17,\"ts\":1}";
 const std::string good="{\"ver\":1,\"type\":\"VOICE_XFER_READY\",\"voice_schema\":1,\"request_id\":\"wanted\",\"ok\":1}";
 const std::string wrong="{\"ver\":1,\"type\":\"VOICE_XFER_READY\",\"voice_schema\":1,\"request_id\":\"old\",\"ok\":1}";
 reset();lcdSerial.bytes=input+"\n"+wrong+"\n"+good+"\n"+std::string("\0COBS",5);
 {SenseVoiceUartLease lease(millis()+1000);JsonDocument d;check(lease.held(),"reader owns shared raw RX");
  check(sense_voice_spool_read(d,"VOICE_XFER_READY","wanted",millis()+1000),"only exact typed correlated response consumed");
  check(uart_rx_ring_count==input.size()+wrong.size()+2&&dispatches==0,"unrelated input retained until voice ownership closes");
  check(lcdSerial.bytes==std::string("\0COBS",5),"reader stops exactly before following binary payload");
  pump_uart_rx_once();check(dispatches==0,"owned voice reader cannot run ordinary callbacks");}
 lcdSerial.bytes.clear();pump_uart_rx_once();
 check(dispatched.size()==2&&dispatched[0]==input&&dispatched[1]==wrong,"held ordinary frames dispatch once in original order");
 reset();const size_t cut=21;lcdSerial.bytes=input.substr(0,cut);pump_uart_rx_once();
 check(uart_rx_frame_len==cut&&dispatches==0,"ordinary collector retains preexisting partial line");
 lcdSerial.bytes=input.substr(cut)+"\n"+good+"\n";
 {SenseVoiceUartLease lease(millis()+100);JsonDocument d;
  check(sense_voice_spool_read(d,"VOICE_XFER_READY","wanted",millis()+100),"voice reader completes shared partial line before own response");}
 pump_uart_rx_once();check(dispatched.size()==1&&dispatched[0]==input,"partial frame survives collector-to-voice handoff byte exactly");
 reset();lcdSerial.bytes=input.substr(0,cut);
 {SenseVoiceUartLease lease(millis()+10);JsonDocument d;
  check(!sense_voice_spool_read(d,"VOICE_XFER_READY","wanted",millis()+10),"missing response retains bounded timeout");
  check(uart_rx_frame_len==cut&&uart_rx_ring_count==0,"timeout keeps unfinished frame for next owner");}
 lcdSerial.bytes=input.substr(cut)+"\n";pump_uart_rx_once();
 check(dispatched.size()==1&&dispatched[0]==input,"normal collector completes partial voice-timeout frame once");
 reset();lcdSerial.bytes=std::string(UART_RX_FRAME_MAX+20,'x')+"\n"+input+"\n"+good+"\n";
 {SenseVoiceUartLease lease(millis()+100);JsonDocument d;
  check(sense_voice_spool_read(d,"VOICE_XFER_READY","wanted",millis()+100),"oversized line cannot hide later valid response");}
 pump_uart_rx_once();check(dispatched.size()==1&&dispatched[0]==input,"oversize discarded whole; next ordinary frame intact");
 reset();std::string retained;
 while(retained.size()+input.size()+1<=UART_RX_RING_SIZE){retained+=input+"\n";}
 {UartRxLock rx;for(char c:retained)uart_ring_push(c);}
 lcdSerial.bytes=wrong+"\n"+good+"\n";
 {SenseVoiceUartLease lease(millis()+100);JsonDocument d;
  check(sense_voice_spool_read(d,"VOICE_XFER_READY","wanted",millis()+100),"full ring still recognizes owned reply");
  check(uart_rx_ring_count==retained.size()&&uart_rx_dropped_since_frame==wrong.size()+1,"capacity refusal drops whole new frame without overwriting old input");}
 pump_uart_rx_once();check(dispatched.size()==retained.size()/(input.size()+1),"ring capacity preserves every prior whole frame");
 reset();lcdSerial.bytes.assign(10000,'x');on_read=[] {++now;};
 {SenseVoiceUartLease lease(millis()+10);JsonDocument d;
  check(!sense_voice_spool_read(d,"VOICE_XFER_READY","wanted",millis()+10)&&reads==10&&lcdSerial.bytes.size()==9990,"continuous traffic cannot extend total reader deadline");}
 reset();
 {SenseImgSpoolLease photo(42);check(photo.held(),"photo request admitted before voice");
  SenseVoiceUartLease voice(millis()+100);check(!voice.held(),"pending photo request excludes voice without losing photo ownership");
  check(g_img_spool_request_active&&s_img_ready_result==-1,"voice refusal preserves pending photo mailbox");}
 {SenseVoiceUartLease voice(millis()+100);check(voice.held(),"voice admitted after photo closes");
  SenseImgSpoolLease photo(43);check(!photo.held()&&g_img_spool_tx_active,"photo request cannot preempt committed voice RX owner");}
 reset();lcdSerial.bytes=input+"\n";bool boundary=false;
 on_unlock=[&](SemaphoreHandle_t sem){if(sem!=uart_rx_mutex||uart_dispatch_depth.load()==0)return;
  on_unlock=nullptr;boundary=true;
  SenseVoiceUartLease voice(millis()+100);check(!voice.held(),"voice refuses exact RX-release-before-callback interleaving");
  SenseImgSpoolLease photo(42);check(!photo.held(),"photo also refuses admitted callback boundary");};
 pump_uart_rx_once();check(boundary&&dispatches==1&&uart_dispatch_depth==0,"callback dispatch reservation closes after the real callback");
 check(!uart_json_tx_mutex->held&&!uart_rx_mutex->held,"all terminal paths close both locks");
 printf("PASS %u actual-source query/voice ownership checks\n",checks);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--negative-reader-root', type=Path)
    a = p.parse_args()
    json_include = a.source_root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    with tempfile.TemporaryDirectory(prefix='halo-voice-uart-') as temp:
        cpp = Path(temp) / 'test.cpp'
        exe = Path(temp) / 'test'
        cpp.write_text(harness(a.source_root, a.negative_reader_root))
        subprocess.run([shutil.which('clang++') or 'c++', '-std=c++17', '-O1', '-Wno-deprecated-declarations',
                        '-I', str(json_include), str(cpp), '-o', str(exe)], check=True, timeout=30)
        if a.negative_reader_root:
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            expected = 'FAIL unrelated input retained until voice ownership closes'
            if result.returncode == 0 or expected not in result.stderr:
                raise RuntimeError('Old reader did not reproduce the targeted lost-input failure: ' + result.stderr)
            print('PASS negative control: actual candidate002 reader loses ordinary input before its typed reply')
        else:
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()

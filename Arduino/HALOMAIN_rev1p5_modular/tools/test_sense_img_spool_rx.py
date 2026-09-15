#!/usr/bin/env python3
"""Actual-source legacy photo READY/RX ownership controls; host serial/RTOS doubles.

No flash, SD durability, or physical concurrency claim. --negative-control swaps
only the old deployed159 READY reader into the new callsite and must lose input.
"""
import argparse
import re
from pathlib import Path
import shutil
import subprocess
import tempfile


def definition(text, signature):
    start = text.index(signature)
    masked = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                    lambda m: ' ' * len(m.group()), text, flags=re.S)
    end = masked.index('{', start) + 1
    depth = 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return text[start:end]



def harness(root, negative, baseline):
    uart = (root / 'Sense_Minimal/sense_uart.h').read_text()
    spool = (root / 'Sense_Minimal/sense_img_spool.h').read_text()
    state = uart[uart.index('static uint32_t sense_msg_id_counter'):
                 uart.index('// ── TX/RX type tracking')]
    funcs = '\n'.join(definition(uart, x) for x in (
        'static bool uart_rx_is_printable(', 'static void uart_reset_rx_state()',
        'static void uart_ring_push(', 'static bool uart_ring_pop(',
        'static void uart_process_rx_ring()', 'static void uart_collect_rx_once()',
        'static void pump_uart_rx_once()', 'static void wake_rx_sanitize()', 'static void link_reset_parser_state()'))
    photo = spool[spool.index('static uint32_t g_img_spool_sent'):]
    if negative:
        start = photo.index('  // Collection does not dispatch')
        end = photo.index('  if (!ready) {', start)
        old = (baseline / 'Sense_Minimal/sense_img_spool.h').read_text()
        old_start = old.index('  bool ready = false;')
        old_end = old.index('  if (!ready) {', old_start)
        legacy = old[old_start:old_end]
        # Mechanical Arduino String -> host std::string surface adapter only.
        legacy = legacy.replace('String line;', 'std::string line;').replace('.indexOf(', '.find(').replace(' >= 0', ' != std::string::npos')
        photo = photo[:start] + legacy + photo[end:]
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
static unsigned checks=0;
static void check(bool b,const char* n){++checks;if(!b){fprintf(stderr,"FAIL %s\n",n);exit(2);}}
static uint32_t now=1000;
static uint32_t millis(){return now;}
static std::function<void()> on_delay;
static void delay(uint32_t n){now+=n;if(on_delay)on_delay();}
static const int pdTRUE=1;
static uint32_t pdMS_TO_TICKS(uint32_t n){return n;}
struct StaticSemaphore_t{bool held=false;};
using SemaphoreHandle_t=StaticSemaphore_t*;
static std::function<void(SemaphoreHandle_t)> on_unlock;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s){return s;}
static int xSemaphoreTake(SemaphoreHandle_t s,uint32_t){if(!s||s->held)return 0;s->held=true;return 1;}
static void xSemaphoreGive(SemaphoreHandle_t s){assert(s&&s->held);s->held=false;auto cb=on_unlock;if(cb)cb(s);}
static bool g_lcd_ota_proxy_owns_uart=false,g_spool_owns_uart=false;
static std::atomic<bool>g_img_spool_tx_active{false},g_img_spool_request_active{false},g_lcd_ota_mode_unconfirmed{false};
static std::atomic<int> s_img_ready_result{-2};
static bool sense_img_spool_binary_pending(){return s_img_ready_result.load()==1;}
static bool query_busy=false;
static bool sense_lcd_query_busy(){return query_busy;}
static bool parse_input_message(const char*);
static bool sense_img_spool_on_json(const char*);
static constexpr size_t UART_RX_RING_SIZE=2048,UART_RX_FRAME_MAX=512;
''' + definition(uart, 'static bool sense_uart_ordinary_tx_allowed()') + '\n' + state + r'''
static std::vector<std::string> dispatched;
static std::function<void()> on_dispatch_hook;
static struct {template<class...T>void printf(const char*,T...){}void println(const char*){}}Serial;
static struct Port {
 std::string bytes; unsigned reads=0;
 int available(){return (int)bytes.size();}
 int read(){++reads;if(bytes.empty())return -1;int c=(unsigned char)bytes.front();bytes.erase(0,1);return c;}
}lcdSerial;
static bool parse_input_message(const char* s){check(!uart_rx_mutex->held,"no protocol callback under RX lease");check(!g_img_spool_request_active||s_img_ready_result==0,"no action admission while its ACK/UI TX is suppressed");StaticJsonDocument<512> doc;check(!deserializeJson(doc,s),"only whole JSON reaches ordinary dispatch");dispatched.emplace_back(s);check(uart_dispatch_depth>0,"callback admission retained without raw lock");auto cb=on_dispatch_hook;if(cb)cb();return true;}
''' + funcs + r'''
static constexpr unsigned PROTOCOL_VERSION=1,SENSE_IMG_SPOOL_READY_TIMEOUT_MS=3000,SENSE_IMG_SPOOL_ACK_TIMEOUT_MS=4000;
static constexpr unsigned MAX_CHUNK_SIZE=512,MAX_FRAME_SIZE=600;
static constexpr uint8_t MSG_IMG_CHUNK=0x12,MSG_IMG_END=0x13,MSG_IMG_ACK=0x14;
struct UploadJob {
 uint32_t job_id=42;const char*mode="discard";bool is_voice=false;const char*expiry_date="";
 unsigned quantity=1,retries=0,created_epoch=0;bool add_to_shopping_list=false;
 struct {const char*profile="test";bool flash_enabled=false;unsigned jpeg_quality=12,actual_width=0,actual_height=0,configured_framesize=0,scene_luma=0,scene_green_ratio=0,xclk_hz=0;}camera_meta;
};
static std::string reply;
static unsigned begins=0,frames=0;
static bool send_ok=true,receive_ok=true;
static uint16_t last_seq=0,ack_offset=0;
static void uart_send_json(const char* s,bool=false,bool=false){
 check(!uart_rx_mutex->held&&!uart_json_tx_mutex->held,"JSON writes outside RX/admission locks");
 if(strstr(s,"IMG_XFER_BEGIN")){++begins;lcdSerial.bytes+=reply;}
}
struct UartOtaProtocol {
 bool quiet=false;explicit UartOtaProtocol(Port*){}
 bool send_frame(uint8_t,uint16_t seq,const uint8_t*,size_t){check(uart_rx_mutex->held&&g_img_spool_tx_active,"COBS requires actual exclusive RX and binary flag");++frames;last_seq=seq;return send_ok;}
 bool recv_frame(uint8_t*t,uint16_t*s,uint8_t*,size_t*n,uint32_t){check(uart_rx_mutex->held,"COBS ACK owns RX");*t=MSG_IMG_ACK;*s=last_seq+ack_offset;*n=0;return receive_ok;}
};
''' + photo + r'''
static const std::string input="{\"ver\":1,\"type\":\"INPUT_MENU_SELECT\",\"msg_id\":24,\"ts\":1000,\"menu_item\":\"Discard\"}\n";
static const std::string no="{\"ver\":1,\"type\":\"IMG_XFER_READY\",\"job_id\":42,\"ok\":0}\n";
static const std::string yes="{\"ver\":1,\"type\":\"IMG_XFER_READY\",\"job_id\":42,\"ok\":1}\n";
static void reset(){check(!uart_rx_mutex->held&&!uart_json_tx_mutex->held,"prior transaction released");
 now=1000;uart_reset_rx_state();lcdSerial.bytes.clear();lcdSerial.reads=0;dispatched.clear();reply.clear();
 on_delay=nullptr;on_unlock=nullptr;on_dispatch_hook=nullptr;g_img_spool_tx_active=false;g_img_spool_request_active=false;g_lcd_ota_mode_unconfirmed=false;
 g_spool_owns_uart=g_lcd_ota_proxy_owns_uart=query_busy=false;s_img_ready_result=-1;frames=begins=0;send_ok=receive_ok=true;ack_offset=0;}
int main(){uart_json_tx_init();reset();UploadJob job;uint8_t bytes[512]={};
 reply=input+no+input;
 check(!sense_spool_image_to_lcd_once(job,bytes,sizeof(bytes)),"negative READY refuses binary");
 check(dispatched.empty(),"main rescue does not dispatch while photo custody is live");
 pump_uart_rx_once();check(dispatched.size()==2&&dispatched[0]==dispatched[1],"both ordinary retries survive legacy READY reader");
 check(frames==0&&now<1100,"explicit SD refusal has no transfer or full timeout");
 reset();reply=input+no;bool serviced=false;
 on_delay=[&]{if(!serviced){serviced=true;pump_uart_rx_once();}};
 check(!sense_spool_image_to_lcd_once(job,bytes,sizeof(bytes))&&dispatched.size()==1,"main pump services input after explicit refusal during worker cleanup");
 check(now<1400,"explicit refusal restores input before LCD first retry");
 reset();{SenseImgSpoolLease request(42);check(request.held(),"prepare pending request dispatcher test");
 lcdSerial.bytes=input;pump_uart_rx_once();check(dispatched.empty()&&uart_rx_ring_count>0,"pending READY preserves INPUT without executing or marking seen");
 lcdSerial.bytes=no;pump_uart_rx_once();check(dispatched.size()==1,"explicit refusal resumes admission when ACK TX is safe");}
 reset();bool interleaved=false;lcdSerial.bytes=input;
 on_unlock=[&](SemaphoreHandle_t s){if(s==uart_rx_mutex&&uart_dispatch_depth>0&&!interleaved){interleaved=true;SenseImgSpoolLease raced(42);check(!raced.held(),"worker cannot BEGIN between RX release and ordinary callback");}};
 pump_uart_rx_once();check(interleaved&&dispatched.size()==1&&uart_dispatch_depth==0,"exact RX-release callback race closes admission then releases depth");
 reset();lcdSerial.bytes=input;on_dispatch_hook=[] {on_dispatch_hook=nullptr;lcdSerial.bytes=input;pump_uart_rx_once();check(uart_dispatch_depth==1,"nested pump retains outer dispatch admission");};
 pump_uart_rx_once();check(dispatched.size()==2&&uart_dispatch_depth==0,"recursive dispatch depth returns to zero");
 {SenseImgSpoolLease after(42);check(after.held(),"spool resumes after all callbacks close");}
 reset();reply=yes;check(sense_spool_image_to_lcd_once(job,bytes,sizeof(bytes))&&frames==2,"matching legacy positive READY enters exclusive binary and exact END ACK");
 check(!g_img_spool_request_active&&!g_img_spool_tx_active,"success closes request and binary ownership");
 reset();reply=yes;ack_offset=1;check(!sense_spool_image_to_lcd_once(job,bytes,sizeof(bytes)),"wrong ACK sequence refuses success");
 reset();reply="{\"ver\":1,\"type\":\"IMG_XFER_READY\",\"job_id\":41,\"ok\":1}\n"+input+no;
 check(!sense_spool_image_to_lcd_once(job,bytes,sizeof(bytes))&&frames==0,"other job READY cannot arm binary");pump_uart_rx_once();check(dispatched.size()==1,"unmatched READY preserves surrounding INPUT");
 reset();reply="{\"ver\":1,\"type\":\"IMG_XFER_READY\",\"job_id\":42,\"ok\":\"1\"}\n";
 check(!sense_spool_image_to_lcd_once(job,bytes,sizeof(bytes))&&now==4000&&frames==0,"wrong typed READY times out at original deadline");
 reset();reply=input;check(!sense_spool_image_to_lcd_once(job,bytes,sizeof(bytes))&&now==4000,"missing READY bounded");check(dispatched.empty()&&now-1000>2000,"main-owned missing READY cannot claim sub2s delivery before safe custody boundary");pump_uart_rx_once();check(dispatched.size()==1,"timeout preserves ordinary request");
 reset();reply=input;
 check(!sense_spool_image_to_lcd(job,bytes,sizeof(bytes))&&begins==3,"full missingREADY wrapper keeps original finite attempt count");
 check(now-1000==9980&&dispatched.empty(),"full main rescue missingREADY delay is9980ms, not a sub2s ACK guarantee");
 pump_uart_rx_once();check(dispatched.size()==3,"all ordinary inputs survive three missingREADY attempts");
 reset();lcdSerial.bytes=input.substr(0,20);uart_collect_rx_once();check(dispatched.empty(),"partial frame not dispatched");lcdSerial.bytes+=input.substr(20);pump_uart_rx_once();check(dispatched.size()==1,"partial ordinary frame completes exactly once");
 reset();lcdSerial.bytes="junk{";wake_rx_sanitize();
 check(dispatched.empty()&&uart_rx_ring_count==0&&uart_rx_frame_len==1,"sanitizer retains first brace as partial wire frame");
 lcdSerial.bytes=input.substr(1,15);pump_uart_rx_once();check(dispatched.empty(),"sanitized fragmented first input remains incomplete");
 lcdSerial.bytes=input.substr(16);pump_uart_rx_once();check(dispatched.size()==1,"sanitized first input dispatched exactly once after newline");
 reset();lcdSerial.bytes=input+input;uart_collect_rx_once();link_reset_parser_state();pump_uart_rx_once();check(dispatched.size()==2,"SYNC partial reset preserves complete queued inputs");
 reset();{SenseImgSpoolLease request(42);check(request.held(),"prepare destructor contention");uart_rx_mutex->held=true;}
 check(!g_img_spool_request_active&&s_img_ready_result==-2,"request releases without waiting for another raw owner");
 check(sense_img_spool_on_json(yes.c_str())&&!sense_img_spool_binary_pending(),"late READY cannot resurrect cancelled request");uart_rx_mutex->held=false;
 reset();lcdSerial.bytes="{"+std::string(514,'x')+"}\n"+input;pump_uart_rx_once();check(dispatched.size()==1,"oversize line drops wholly and following INPUT survives");
 reset();for(int i=0;i<30;++i)lcdSerial.bytes+=input;
 for(int i=0;i<4;++i)uart_collect_rx_once();pump_uart_rx_once();check(!dispatched.empty()&&dispatched.size()<30,"full queue never overwrites or emits truncated JSON");
 reset();lcdSerial.bytes.assign(4096,'x');uart_collect_rx_once();check(lcdSerial.reads==1024,"raw collection has finite byte budget");
 reset();lcdSerial.bytes=input;{UartRxLock raw;check(raw.held(),"test binary reader claims RX");pump_uart_rx_once();check(lcdSerial.reads==0&&dispatched.empty(),"competing reader neither consumes nor dispatches");}
 pump_uart_rx_once();check(dispatched.size()==1,"ordinary reader resumes after raw owner");
 for(int which=0;which<3;++which){reset();if(which==0)query_busy=true;if(which==1)g_lcd_ota_proxy_owns_uart=true;if(which==2)g_lcd_ota_mode_unconfirmed=true;
  check(!sense_spool_image_to_lcd_once(job,bytes,sizeof(bytes))&&begins==0,"query OTA or quarantine prevents BEGIN");}
 reset();{SenseImgSpoolLease first(42);check(first.held(),"first request admitted");SenseImgSpoolLease second(43);check(!second.held(),"second spool cannot overwrite active mailbox");}
 check(!uart_rx_mutex->held&&!uart_json_tx_mutex->held,"all locks closed");printf("PASS %u actual-source photo READY/RX checks\n",checks);
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--negative-control',action='store_true')
    p.add_argument('--baseline-root',type=Path,default=Path('/Users/MattTaylor/halo-device-analytics-2026-09-10/hardware-validation159/snapshot/source'))
    a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-photo-rx-') as t:
        cpp=Path(t)/'test.cpp';exe=Path(t)/'test';cpp.write_text(harness(a.source_root,a.negative_control,a.baseline_root))
        subprocess.run([shutil.which('clang++') or 'c++','-std=c++17','-O1','-Wno-deprecated-declarations','-I',str(a.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),str(cpp),'-o',str(exe)],check=True,timeout=30)
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
        print(r.stdout,end='');print(r.stderr,end='')
        if a.negative_control:
            if r.returncode==0 or 'both ordinary retries survive legacy READY reader' not in r.stderr:raise SystemExit('Negative control did not reproduce expected loss')
            print('PASS deployed legacy-reader negative control reproduces lost INPUT')
        elif r.returncode:raise SystemExit(r.returncode)
if __name__=='__main__':main()

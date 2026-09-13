"""Execute the production LCD line parser and ABORT handler with fake I/O.

No board access. --baseline reads pinned, committed pre-fix source and reproduces
both defects. Default tests current source, exact JSON parsing/session guards,
and the unchanged durable failure codec with compact receiver counters.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
JSON = Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src'


def definition(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--baseline-ref', default='91c16957879b3eab3d821d6746c8457bf4752b89')
    args = parser.parse_args()

    def read(name):
        if not args.baseline:
            return (ROOT / name).read_text()
        rel = (ROOT / name).relative_to(ROOT.parents[1])
        return subprocess.check_output(['git', 'show', f'{args.baseline_ref}:{rel}'], cwd=ROOT, text=True)

    task = read('LCD_Minimal/lcd_uart_task.h')
    ota = read('LCD_Minimal/lcd_ota_uart.h')
    rx = task[task.index('    int rx_bytes = 0;'):task.index('    unsigned long now_ms = millis();')]
    abort = definition(ota, 'static void lcd_ota_handle_abort(JsonObject& doc) {')
    extra = ''
    if not args.baseline:
        extra = '\n'.join(definition(ota, signature) for signature in (
            'static uint8_t lcd_ota_abort_reason_code(',
            'static uint32_t lcd_ota_abort_transport_flags(',
            'static void lcd_ota_record_abort('))
        diag = read('LCD_Minimal/lcd_diagnostic_integration.h')
        capture = definition(diag, 'static void lcd_diag_capture_failure(')
        assert capture.index('v.transport_flags=transport_flags') < capture.index('g_lcd_diag_failure_pending.store(true')
        assert '|v.transport_flags' in diag
        assert abort.count('lcd_ota_record_abort(') == 1
        assert definition(ota, 'static void lcd_ota_abort_internal(').count('lcd_ota_record_abort(') == 1

    pre = r'''
#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include "DiagnosticCapsule.h"
static constexpr bool baseline=BASELINE;
static constexpr int LCD_OTA_IDLE=0,LCD_OTA_RECEIVING=1,LCD_OTA_ABORTING=3;
static std::atomic<int> s_lcd_ota_state{LCD_OTA_IDLE};
static bool g_img_rx_binary_mode,g_lcd_ota_binary_mode,g_lcd_ota_uart_receiving,g_suppress_uart_json_tx;
static uint16_t s_lcd_ota_session_id=48089,s_lcd_ota_last_aborted_session=48089,s_lcd_ota_last_seq=646,s_lcd_ota_expected_seq=647;
static unsigned s_lcd_ota_started_ms=1,s_lcd_ota_budget_ms=2400000,s_lcd_ota_handle=1;
static unsigned s_lcd_ota_bytes_written=330248,s_lcd_ota_image_size=1889504,s_lcd_ota_last_nvs_offset,s_lcd_ota_last_chunk_ms=100;
static int s_lcd_ota_last_progress_pct,s_lcd_ota_sha_ctx;
static void* s_lcd_ota_partition=(void*)1;
static char s_lcd_ota_expected_sha256[65],s_lcd_ota_target_version[32];
static unsigned millis(){return 200;}
static struct {template<class...A>void printf(const char*,A...){} void println(const char*){}} Serial;
struct LcdNvsDeadline {static LcdNvsDeadline fromAttempt(unsigned,unsigned){return {};}};
static unsigned ack_count,cleanup_count,terminal_count,restore_count;
static uint16_t ack_session;
static std::string terminal;
static void lcd_ota_send_abort_ack(uint16_t s){++ack_count;ack_session=s;}
static void esp_ota_abort(unsigned){++cleanup_count;}
static void mbedtls_sha256_free(int*){}
static void lcd_ota_nvs_clear(const LcdNvsDeadline&){}
static void lcd_ota_uart_restore_ui(const LcdNvsDeadline&){++restore_count;}
static void lcd_ota_store_terminal(const char*,const char*,int,const char* text,const LcdNvsDeadline&){++terminal_count;terminal=text;}
struct Protocol {unsigned crc_error_count(){return 2;}unsigned frame_error_count(){return 3;}};
static Protocol* s_lcd_ota_protocol=nullptr;
static constexpr int MAX_LINE_LENGTH=1024,UART_RX_MAX_BYTES_PER_LOOP=512;
static constexpr bool UART_RX_DEBUG=false;
static constexpr unsigned UART_RX_DIAG_CAPTURE_LIMIT=0;
static unsigned uart_rx_raw_bytes_seen,uart_rx_diag_raw_logged,uart_rx_completed_lines_seen,uart_rx_partial_started_ms;
static int uart_rx_line_pos;
static char uart_rx_line_buffer[MAX_LINE_LENGTH];
static bool yielded_early;
static unsigned parsed_count;
static struct {std::deque<unsigned char> bytes;int available(){return bytes.size();}char read(){char c=bytes.front();bytes.pop_front();return c;}} senseSerial;
'''.replace('BASELINE', str(args.baseline).lower())
    dispatch = r'''
static void uart_process_received_message(char* text){
  JsonDocument doc;
  if(deserializeJson(doc,text)!=DeserializationError::Ok)return;
  ++parsed_count;
  const char* type=doc["type"]|"";
  if(!strcmp(type,"LCD_OTA_BEGIN"))g_lcd_ota_binary_mode=true;
  if(!strcmp(type,"IMG_XFER_BEGIN"))g_img_rx_binary_mode=true;
  if(!strcmp(type,"LCD_OTA_ABORT")){auto obj=doc.as<JsonObject>();lcd_ota_handle_abort(obj);}
}
static void pump(){RX}
static void feed(const std::string& s){for(unsigned char c:s)senseSerial.bytes.push_back(c);pump();}
static void clear(){senseSerial.bytes.clear();uart_rx_line_pos=0;uart_rx_partial_started_ms=0;parsed_count=0;g_img_rx_binary_mode=g_lcd_ota_binary_mode=false;}
static const char* replay=R"({"type":"LCD_OTA_ABORT","session_id":48089,"reason":"scheduled"})";
int main(){
  clear();std::string binary("\x04\x03\x86\x02\x11\x00",6);
  feed(std::string("{\"type\":\"LCD_OTA_BEGIN\"}\n")+binary);
  if(baseline){assert(senseSerial.bytes.empty());puts("PASS baseline reproduces BEGIN line parser consuming queued COBS bytes");}
  else{assert(senseSerial.bytes.size()==binary.size());puts("PASS OTA BEGIN immediately hands queued COBS bytes to binary owner");}
  clear();feed(std::string("{\"type\":\"IMG_XFER_BEGIN\"}\n")+binary);assert(senseSerial.bytes.size()==binary.size());
  clear();s_lcd_ota_state=LCD_OTA_IDLE;ack_count=0;
  feed(std::string(1,'\0')+replay+"\n");
  if(baseline){assert(ack_count==0&&parsed_count==0);puts("PASS baseline reproduces lost ABORT_ACK: idle parser rejects delimiter+JSON retry");return 0;}
  assert(ack_count==1&&ack_session==48089&&cleanup_count==0&&terminal_count==0);
  puts("PASS idle matching-session replay after lost ACK re-ACKs without repeating cleanup or NVS writes");
  feed(std::string("damaged-frame-tail")+'\0'+replay+"\r\n");assert(ack_count==2);
  feed(std::string(1,'\0')+"{\"type\":\"LCD_OTA_ABORT\",\"session_id\":48088}\n");assert(ack_count==2);
  feed(std::string(1,'\0')+"{\"type\":\"LCD_OTA_ABORT\",\"session_id\":\"48089\"}\n");assert(ack_count==2);
  feed(std::string(1,'\0')+"{\"type\":\"LCD_OTA_ABORT\",\"session_id\":0}\n");assert(ack_count==2);
  s_lcd_ota_state=LCD_OTA_RECEIVING;s_lcd_ota_session_id=48090;g_lcd_ota_binary_mode=false;
  feed(std::string(1,'\0')+replay+"\n");assert(ack_count==2&&cleanup_count==0&&s_lcd_ota_state==LCD_OTA_RECEIVING);
  puts("PASS damaged-prefix resync, CRLF, stale/zero/string sessions, and newer-active-session isolation");
  s_lcd_ota_session_id=48089;s_lcd_ota_protocol=new Protocol;
  feed(std::string(1,'\0')+replay+"\n");
  assert(ack_count==3&&cleanup_count==1&&terminal_count==1&&restore_count==1);
  assert(s_lcd_ota_state==LCD_OTA_IDLE&&!g_lcd_ota_uart_receiving&&!s_lcd_ota_protocol);
  assert(terminal.find("s=48089")!=std::string::npos&&terminal.find("seq=646 next=647")!=std::string::npos);
  assert(terminal.find("crc=2 frame=3")!=std::string::npos);
  feed(std::string(1,'\0')+replay+"\n");assert(ack_count==4&&cleanup_count==1&&terminal_count==1&&restore_count==1);
  puts("PASS active cleanup records receiver evidence once; lost-ACK replay adds no flash/NVS writes");
  CODEC
}
'''.replace('RX', rx)
    codec = r'''
  for(unsigned i=0;i<14;++i){static const char*const reasons[]={"timeout","attempt_deadline","deadline_after_rx","deadline_after_write","deadline_after_hash","begin_deadline","end_deadline","end_size_mismatch","unexpected_frame_type","unnegotiated_control","duplicate_payload_mismatch","sequence_mismatch","chunk_size_mismatch","write_failed"};assert(lcd_ota_abort_reason_code(reasons[i])==10+i);}
  assert(lcd_ota_abort_reason_code(nullptr)==5&&lcd_ota_abort_reason_code("scheduled",true)==24);
  assert(lcd_ota_abort_transport_flags(0,0)==4);
  const uint32_t flags=2u|(21u<<8)|(1u<<24)|lcd_ota_abort_transport_flags(999,999);
  assert(((flags>>16)&255)==255&&((flags>>26)&63)==63&&((flags>>8)&255)==21&&((flags>>24)&3)==1);
  halo_diag::Failure f{};f.attempt_id[0]=1;f.boot_id=765;f.stage=halo_diag::Stage::Failure;f.accepted=330248;f.expected=1889504;
  strcpy(f.fw,"130");strcpy(f.build,"host-real-codec");f.flags=flags;f.hash_kind=halo_diag::HashKind::CompiledElf;f.image_hash[0]=77;
  uint8_t raw[256];assert(halo_diag::encode_failure(raw,1,2,0x12345678,f));assert(halo_diag::valid(raw,256,1));
  assert(halo_diag::get32(raw+halo_diag::HEADER+68)==flags);assert(raw[halo_diag::HEADER+168]==77);
  assert(halo_diag::get32(raw+halo_diag::HEADER+40)==0&&halo_diag::get32(raw+halo_diag::HEADER+64)==0);
  puts("PASS specific reasons and saturated counters round-trip unchanged 256-byte diagnostic codec; SDK/hash fields preserved");
'''
    dispatch = dispatch.replace('CODEC', '' if args.baseline else codec)
    with tempfile.TemporaryDirectory(prefix='halo-lcd-ota-recovery-') as tmp:
        cpp = Path(tmp) / 'test.cpp'
        cpp.write_text(pre + extra + abort + dispatch)
        binary = Path(tmp) / 'test'
        subprocess.run(['clang++', '-std=c++17', '-Wno-deprecated-declarations', '-I', str(JSON),
                        '-I', str(ROOT/'halo_ota_demo/firmware/shared'), str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()

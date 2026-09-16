#!/usr/bin/env python3
"""Run the complete LCD voice header against real POSIX storage and mocked I/O.

Only UART events, SD mounting/stream pins and the clock are replaced. Production
JSON handlers, ownership transitions, receive/send loops and storage are included
without extraction or rewriting. No hardware, network or retained data access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

HARNESS = r'''
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <atomic>
#include <filesystem>
#include "halo_common/VoiceSpoolStore.h"
static unsigned checks=0,failures=0;
static void check(bool ok,const char* label){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",label);}}
static uint32_t tick_ms=1000;
static uint32_t millis(){return tick_ms;}
static void vTaskDelay(uint32_t n){tick_ms+=n;}
#define pdMS_TO_TICKS(n) (n)
static unsigned pin_count=0,lease_depth=0;
static std::vector<unsigned> json_pins;
struct HardwareSerial {
  std::string output;std::deque<uint8_t> incoming;bool continuous=false;unsigned reads=0;
  size_t write(uint8_t v){if(v=='{')json_pins.push_back(pin_count);output+=(char)v;return 1;}
  size_t write(const uint8_t* p,size_t n){for(size_t i=0;i<n;++i)write(p[i]);return n;}
  void print(char c){write((uint8_t)c);}
  void print(const char* s){write((const uint8_t*)s,std::strlen(s));}
  void println(const char*){}
  template<class... Args>void printf(const char*,Args...){}
  void flush(){}
  int available(){return continuous?100000:(int)incoming.size();}
  int read(){++reads;if(continuous){++tick_ms;return '\n';}if(incoming.empty())return -1;int c=incoming.front();incoming.pop_front();return c;}
};
static HardwareSerial senseSerial,Serial;
static const unsigned PROTOCOL_VERSION=1;
static uint32_t get_next_msg_id(){static uint32_t n=1;return n++;}
static bool g_img_rx_active=false,g_img_rx_binary_mode=false;
static bool g_spool_tx_pending=false,g_spool_tx_active=false,g_suppress_uart_json_tx=false;
static bool g_sleep_transition=false,ota_busy=false,image_valid=true;
static std::atomic<bool> g_lcd_sleep_commit_gate{false};
static unsigned accepted_binary_proofs=0;
static void note_sense_binary_media_rx(const char*){
  check(g_img_rx_active||g_spool_tx_active||(!g_img_rx_binary_mode&&!g_suppress_uart_json_tx),"typed proof never follows foreign binary owner");
  ++accepted_binary_proofs;
}
static bool lcd_nvs_image_valid(){return image_valid;}
static bool lcd_ota_in_progress_for_sd_guard(){return ota_busy;}
static void lcd_freeze_wdt_feed(){++tick_ms;}
struct LcdMaintenanceStorageGuard {};
static bool mount_ok=true;
struct SdCardLease {SdCardLease(){++lease_depth;}~SdCardLease(){--lease_depth;}explicit operator bool()const{return true;}};
static constexpr int ESP_OK=0;
static int sd_card_Init(){return mount_ok?ESP_OK:-1;}
static bool sd_card_is_mounted(){return mount_ok;}
static int sd_open_file_for_read(const char* p,FILE** f){
  check(lease_depth>0,"open stream under SD lease");
  check(g_spool_tx_active&&g_suppress_uart_json_tx,"stream open while UART/sleep owned");
  *f=std::fopen(p,"rb");if(!*f)return -1;++pin_count;return ESP_OK;
}
static int sd_read_chunk(FILE* f,uint8_t* p,size_t cap,size_t* n){
  SdCardLease lease;*n=std::fread(p,1,cap,f);return std::ferror(f)?-1:ESP_OK;
}
static int sd_close_file(FILE* f){
  SdCardLease lease;check(pin_count>0,"close owns a live mount pin");
  check(g_spool_tx_active&&g_suppress_uart_json_tx,"close before releasing sleep/JSON ownership");
  int r=std::fclose(f);--pin_count;return r==0?ESP_OK:-1;
}
static const uint8_t MSG_IMG_CHUNK=0x12,MSG_IMG_END=0x13,MSG_IMG_ACK=0x14,MSG_IMG_NACK=0x15;
static const size_t MAX_CHUNK_SIZE=512,MAX_FRAME_SIZE=600;
struct Event {int event;uint8_t type;uint16_t seq;std::vector<uint8_t> bytes;std::string json;};
struct Frame {uint8_t type;uint16_t seq;std::vector<uint8_t> bytes;};
static std::deque<Event> input_events;
static std::vector<Frame> output_frames;
enum class Peer {Passive,Accept,WrongChunk,DropChunk,DropEnd,WrongEnd,AbortChunk};
static Peer peer=Peer::Passive;
static bool drop_commit_ack=false,auto_abort=true;
static std::string abort_text;
class UartOtaProtocol {
 public:
  enum ReceiveEvent{TIMEOUT,FRAME,JSON};bool quiet=false;
  explicit UartOtaProtocol(HardwareSerial*){}
  bool valid()const{return true;}
  bool send_frame(uint8_t type,uint16_t seq,const uint8_t* p,size_t n){
    Frame f{type,seq,{}};if(n)f.bytes.assign(p,p+n);output_frames.push_back(f);
    if(type==MSG_IMG_CHUNK&&peer!=Peer::Passive){
      if(peer==Peer::AbortChunk)input_events.push_back({JSON,0,0,{},abort_text});
      else if(peer!=Peer::DropChunk)input_events.push_back({FRAME,MSG_IMG_ACK,(uint16_t)(seq+(peer==Peer::WrongChunk?1:0)),{},""});
    }
    if(type==MSG_IMG_END&&peer!=Peer::Passive&&peer!=Peer::DropEnd)
      input_events.push_back({FRAME,MSG_IMG_ACK,(uint16_t)(seq+(peer==Peer::WrongEnd?1:0)),{},""});
    // Actual Sense closes every FETCH with its bound JSON-ready ABORT handshake.
    // Queue this after the terminal frame ACK or NACK, never before a data ACK.
    if(auto_abort && !abort_text.empty() && ((type==MSG_IMG_END && peer!=Peer::DropEnd) || type==MSG_IMG_NACK))
      input_events.push_back({JSON,0,0,{},abort_text});
    return !(drop_commit_ack&&type==MSG_IMG_ACK&&n==40);
  }
  ReceiveEvent recv_event(uint8_t* type,uint16_t* seq,uint8_t* p,size_t* n,char* json,size_t cap,uint32_t timeout){
    if(input_events.empty()){tick_ms+=timeout;return TIMEOUT;}
    Event e=input_events.front();input_events.pop_front();++tick_ms;
    if(e.event==JSON){check(e.json.size()<cap,"abort JSON fits production receive buffer");std::snprintf(json,cap,"%s",e.json.c_str());return JSON;}
    check(e.bytes.size()<=*n,"frame fits production receive buffer");*type=e.type;*seq=e.seq;*n=e.bytes.size();
    if(*n)std::memcpy(p,e.bytes.data(),*n);return FRAME;
  }
};
#if __has_include("LCD_Minimal/lcd_media_foreground.h")
#define HAS_MEDIA_FOREGROUND 1
using portMUX_TYPE=unsigned;
#define portMUX_INITIALIZER_UNLOCKED 0
static unsigned media_critical_depth=0;
static void portENTER_CRITICAL(portMUX_TYPE*){check(media_critical_depth==0,"media critical section is not nested");++media_critical_depth;}
static void portEXIT_CRITICAL(portMUX_TYPE*){check(media_critical_depth==1,"media critical section is balanced");--media_critical_depth;}
#include "LCD_Minimal/lcd_media_foreground.h"
static bool lcd_media_deferred_intent_pending(){return false;}
#endif
#define LCD_VOICE_SPOOL_DIR "voice-default"
#include "LCD_Minimal/lcd_voice_spool.h"
static const std::vector<uint8_t> pcm{1,2,3,4,5,6,7,8};
static halo_voice::Meta fixture(unsigned id=1){
  halo_voice::Meta m;m.len=pcm.size();m.crc32=halo_voice::crc32(pcm.data(),pcm.size());
  m.job_id=42;m.epoch=1789500000;m.retries=1;
  std::strcpy(m.owner_id,"test-owner");std::strcpy(m.device_id,"test-device");std::strcpy(m.session_id,"test-session");
  std::snprintf(m.request_id,sizeof(m.request_id),"%032x",id);return m;
}
static void reset_case(){
  static unsigned n=0;check(pin_count==0,"previous case released all mount pins");
  delete g_voice_proto;g_voice_proto=nullptr;g_voice_store.abort();
  std::string path="case-"+std::to_string(++n);g_voice_store=halo_voice::Store(path.c_str());
  g_voice_rx=g_voice_tx_pending=g_voice_tx_abort_pending=false;
  g_img_rx_active=g_img_rx_binary_mode=g_spool_tx_pending=g_spool_tx_active=g_suppress_uart_json_tx=false;
  g_voice_have_last=false;g_sleep_transition=false;ota_busy=false;image_valid=true;mount_ok=true;g_lcd_sleep_commit_gate=false;
#ifdef HAS_MEDIA_FOREGROUND
  lcd_media_release();
#endif
  input_events.clear();output_frames.clear();senseSerial.output.clear();senseSerial.incoming.clear();
  senseSerial.continuous=false;senseSerial.reads=0;json_pins.clear();peer=Peer::Passive;drop_commit_ack=false;auto_abort=true;abort_text.clear();tick_ms+=1000;
}
static JsonDocument last_reply(){
  JsonDocument d;const size_t end=senseSerial.output.rfind('\n');
  const size_t start=end==std::string::npos?std::string::npos:senseSerial.output.rfind('\n',end?end-1:0);
  const std::string text=end==std::string::npos?senseSerial.output:senseSerial.output.substr(start==std::string::npos?0:start+1,end-(start==std::string::npos?0:start+1));
  check(!deserializeJson(d,text),"control reply is valid JSON");return d;
}
static bool released(){return !g_voice_rx&&!g_voice_tx_pending&&!g_img_rx_active&&!g_img_rx_binary_mode&&!g_spool_tx_pending&&!g_spool_tx_active&&!g_suppress_uart_json_tx&&pin_count==0;}
static void begin(const halo_voice::Meta& m){
  JsonDocument d;lcd_voice_put_meta(d.to<JsonObject>(),m);d["type"]="VOICE_XFER_BEGIN";
  check(lcd_voice_uart(d),"actual BEGIN handler recognized");
}
static void frame(uint8_t type,uint16_t seq,const std::vector<uint8_t>& data={}){input_events.push_back({UartOtaProtocol::FRAME,type,seq,data,""});}
static void commit(const halo_voice::Meta& m){
  begin(m);check(g_voice_rx&&g_img_rx_active&&g_img_rx_binary_mode&&g_suppress_uart_json_tx,"READY holds UART and sleep");
  frame(MSG_IMG_CHUNK,0,pcm);check(lcd_voice_receive_loop(),"chunk accepted without ending transfer");
  frame(MSG_IMG_END,1);check(!lcd_voice_receive_loop(),"END terminates receive loop");
  check(released(),"receive completion releases all owners");
}
static bool retained(const halo_voice::Meta& m){halo_voice::Meta observed;return g_voice_store.lookup(m.request_id,m.owner_id,m.device_id,&observed)==halo_voice::Result::Ok;}
static unsigned count(const halo_voice::Meta& m){halo_voice::Meta first;halo_voice::Stats s;g_voice_store.list(m.owner_id,m.device_id,&first,&s);return s.count;}
static std::string abort_json(const halo_voice::Meta& m){JsonDocument d;d["type"]="VOICE_XFER_ABORT";d["voice_schema"]=1;lcd_voice_echo(d,m);std::string out;serializeJson(d,out);return out;}
static void fetch(const halo_voice::Meta& m){
  abort_text=abort_json(m);
  JsonDocument d;d["type"]="VOICE_SPOOL_FETCH";d["voice_schema"]=1;
  d["owner_id"]=m.owner_id;d["device_id"]=m.device_id;d["request_id"]=m.request_id;
  check(lcd_voice_uart(d),"actual FETCH handler recognized");
  check(g_voice_tx_pending&&g_spool_tx_pending&&g_spool_tx_active&&g_suppress_uart_json_tx,"FETCH reserves UART and sleep before streaming");
}
int main(){
  {
    reset_case();auto m=fixture();commit(m);check(retained(m),"receive commits real PCM on filesystem");
    check(output_frames.back().type==MSG_IMG_ACK&&output_frames.back().seq==1&&output_frames.back().bytes.size()==40,"durable END ACK has exact sequence and forty-byte proof");
    const auto& p=output_frames.back().bytes;const uint8_t* c=p.data();
    check(halo_voice::get32(c)==m.len&&halo_voice::get32(c)==m.crc32&&!std::memcmp(c,m.request_id,32),"END ACK binds bytes checksum and this request");
    begin(m);auto r=last_reply();check(r["ok"]==1&&r["stored"]==1&&r["json_ready"]==true,"duplicate BEGIN proves existing commit without binary mode");
    check(released()&&count(m)==1,"duplicate does not allocate a second job or hold sleep");
  }
  {
    reset_case();auto m=fixture();drop_commit_ack=true;commit(m);
    check(retained(m),"dropped final ACK leaves committed recording intact");
    drop_commit_ack=false;begin(m);auto r=last_reply();check(r["stored"]==1&&released(),"retransmitted BEGIN recovers a lost final ACK");
  }
  for(int failure=0;failure<4;++failure){
    reset_case();auto m=fixture();if(failure==3)m.crc32^=1;begin(m);
    frame(MSG_IMG_CHUNK,failure==0?1:0,pcm);lcd_voice_receive_loop();
    if(g_voice_rx){frame(failure==2?0x99:MSG_IMG_END,failure==1?0:1);lcd_voice_receive_loop();}
    check(released(),"bad sequence/type/checksum releases sleep and JSON flags");
    check(count(m)==0,"rejected recording is never advertised committed");
    check(output_frames.back().type==MSG_IMG_NACK,"bad receive emits negative acknowledgement");
  }
  for(bool absolute:{false,true}){
    reset_case();auto m=fixture();begin(m);
    if(absolute){tick_ms=g_voice_started_ms+LCD_VOICE_XFER_MS;g_voice_last_frame_ms=tick_ms-1;}
    else tick_ms+=LCD_VOICE_IDLE_MS+1;
    check(!lcd_voice_receive_loop()&&released(),"idle and absolute timeouts release ownership");
    check(count(m)==0,"timeout preserves an unlisted partial instead of claiming success");
  }
  {
    reset_case();auto m=fixture();begin(m);auto wrong=fixture(2);
    input_events.push_back({UartOtaProtocol::JSON,0,0,{},abort_json(wrong)});
    lcd_voice_receive_loop();check(g_voice_rx,"another request cannot abort active voice");
    input_events.push_back({UartOtaProtocol::JSON,0,0,{},abort_json(m)});
    lcd_voice_receive_loop();auto r=last_reply();
    check(released()&&r["type"]=="VOICE_XFER_ABORT_ACK"&&r["ok"]==1&&r["json_ready"]==true,"matching binary JSON abort proves idle after release");
    JsonDocument d;deserializeJson(d,abort_json(m));lcd_voice_uart(d);r=last_reply();
    check(released()&&r["json_ready"]==true,"lost abort ACK can be repeated from ordinary JSON mode");
  }
  {
    reset_case();auto m=fixture();mount_ok=false;begin(m);auto r=last_reply();
    check(r["ok"]==0&&r["json_ready"]==true&&released(),"mount failure is explicit idle refusal");
    mount_ok=true;ota_busy=true;begin(m);check(released(),"OTA ownership refuses voice admission");
    ota_busy=false;image_valid=false;begin(m);check(released(),"unvalidated application refuses storage admission");
  }
  for(Peer mode:{Peer::Accept,Peer::WrongChunk,Peer::DropChunk,Peer::DropEnd,Peer::WrongEnd,Peer::AbortChunk}){
    reset_case();auto m=fixture();commit(m);fetch(m);abort_text=abort_json(m);peer=mode;
    output_frames.clear();senseSerial.output.clear();json_pins.clear();
    const auto before=tick_ms;lcd_voice_send_file();
    check(released(),"send completion/failure closes stream and releases all owners");
    check(tick_ms-before<6000,"missing/wrong acknowledgements end within bounded idle interval");
    check(retained(m),"replay cannot delete a recording without cloud acceptance");
    if(mode==Peer::Accept){
      check(output_frames.back().type==MSG_IMG_END&&output_frames.back().bytes.size()==40,"replay END retains full request proof");
    }else if(mode==Peer::AbortChunk){
      auto r=last_reply();check(r["type"]=="VOICE_XFER_ABORT_ACK"&&r["json_ready"]==true,"send-side abort returns explicit idle proof");
      check(!json_pins.empty()&&json_pins.back()==0,"abort ACK follows closing the pinned SD stream");
    }else check(output_frames.back().type==MSG_IMG_NACK,"failed replay sends NACK and retains original file");
  }
  {
    reset_case();auto m=fixture();commit(m);fetch(m);peer=Peer::Accept;senseSerial.continuous=true;
    const auto before=tick_ms;lcd_voice_send_file();senseSerial.continuous=false;
    check(senseSerial.reads<=1024&&tick_ms-before<6000&&released(),"continuously arriving residue cannot create unbounded drain loop");
  }
  {
    reset_case();auto m=fixture();commit(m);JsonDocument d;
    d["type"]="VOICE_SPOOL_DELETE";d["voice_schema"]=1;d["owner_id"]=m.owner_id;d["device_id"]=m.device_id;
    d["request_id"]=m.request_id;d["len"]=m.len;d["crc32"]=m.crc32^1;lcd_voice_uart(d);
    check(retained(m),"wrong deletion checksum cannot remove recording");
    d["crc32"]=m.crc32;lcd_voice_uart(d);auto r=last_reply();
    check(r["ok"]==1&&count(m)==0&&released(),"only matching cloud-acceptance deletion removes record");
  }
  std::printf("%s %u actual-source LCD voice transport checks (%u failures)\n",failures?"FAIL":"PASS",checks,failures);
  delete g_voice_proto;g_voice_proto=nullptr;return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--arduino-json', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    aj = args.arduino_json or root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler or not (aj / 'ArduinoJson.h').is_file():
        raise SystemExit('C++ compiler and canonical ArduinoJson source are required')
    with tempfile.TemporaryDirectory(prefix='halo-lcd-voice-transport-') as tmp:
        work = Path(tmp)
        (work / 'main.cpp').write_text(HARNESS)
        built = subprocess.run([compiler, '-std=c++17', '-Wno-deprecated-declarations',
                                '-I', str(root), '-I', str(aj), str(work / 'main.cpp'),
                                '-o', str(work / 'test')], capture_output=True, text=True)
        if built.returncode:
            print(built.stdout + built.stderr)
            raise SystemExit(built.returncode)
        run = subprocess.run([str(work / 'test')], cwd=work, capture_output=True, text=True, timeout=30)
        print(run.stdout + run.stderr, end='')
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps({
                'status': 'PASS' if run.returncode == 0 else 'FAIL',
                'scope': 'Actual LCD handlers/store; UART/SD boundaries and clock mocked; no hardware acceptance',
                'stdout': run.stdout, 'stderr': run.stderr, 'returncode': run.returncode,
                'source_sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [
                    root / 'LCD_Minimal/lcd_voice_spool.h', root / 'halo_common/VoiceSpoolStore.h',
                    Path(__file__).resolve()]},
            }, indent=2) + '\n')
        raise SystemExit(run.returncode)


if __name__ == '__main__':
    main()

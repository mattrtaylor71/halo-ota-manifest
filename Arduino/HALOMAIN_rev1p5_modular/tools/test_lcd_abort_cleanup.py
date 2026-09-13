"""Compile the actual Sense cleanup/reply lambdas with bounded fake wire/time.

Real ArduinoJson validates replies. No serial, network, or firmware execution.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_retry_peer_ready import definition

ROOT = Path(__file__).resolve().parents[1]
JSON = Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src'


def harness():
    src = (ROOT / 'Sense_Minimal/sense_ota_lcd.h').read_text()
    proto = (ROOT / 'halo_ota_demo/firmware/shared/UartOtaProtocol.cpp').read_text()
    complete = definition(proto, 'bool UartOtaProtocol::json_record_complete').replace(
        'bool UartOtaProtocol::json_record_complete', 'static bool json_record_complete')
    helpers = definition(src, 'struct LcdOtaCleanupResult', True) + '\n' + definition(
        src, 'template<class Send, class Receive, class Remaining>')
    send = definition(src, 'auto send_control =') + ';'
    receive = definition(src, 'auto receive_control =') + ';'
    return r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
using String=std::string;
static uint32_t now_ms=0;
static uint32_t millis(){return now_ms;}
static uint32_t get_next_msg_id(){return 123;}
static constexpr unsigned MAX_CHUNK_SIZE=512,MSG_OTA_CONTROL=9,PROTOCOL_VERSION=1;
struct Pending {uint32_t due;std::string json;};
static std::vector<Pending> input;
struct UartOtaProtocol {
  enum Event {TIMEOUT,FRAME,JSON};
''' + complete + r'''
  unsigned framed=0;
  bool write_ok=true;
  bool send_frame(unsigned type,unsigned seq,const uint8_t* data,size_t length){
    assert(type==MSG_OTA_CONTROL&&seq==0);
    JsonDocument d;assert(!deserializeJson(d,data,length));
    assert(d["session_id"].as<unsigned>()==48089&&d["type"]=="LCD_OTA_ABORT");
    ++framed;return write_ok;
  }
  Event recv_event(uint8_t*,uint16_t*,uint8_t*,size_t*,char* json,size_t cap,uint32_t wait){
    if(!input.empty()&&input.front().due<=now_ms+wait){
      now_ms=std::max(now_ms,input.front().due);
      assert(input.front().json.size()<cap);strcpy(json,input.front().json.c_str());
      input.erase(input.begin());return JSON;
    }
    now_ms+=wait;return TIMEOUT;
  }
};
static struct Serial {
  unsigned plain=0;std::string bytes;
  size_t write(const uint8_t* p,size_t n){bytes.append((const char*)p,n);return n;}
  void flush(){
    assert(bytes.front()==0&&bytes.back()=='\n');
    JsonDocument d;assert(!deserializeJson(d,bytes.data()+1,bytes.size()-1));
    assert(d["session_id"].as<unsigned>()==48089&&d["type"]=="LCD_OTA_ABORT");
    ++plain;bytes.clear();
  }
} lcdSerial;
''' + helpers + r'''
static LcdOtaCleanupResult run(unsigned scenario,uint32_t budget=2400000){
  now_ms=0;input.clear();lcdSerial={};
  UartOtaProtocol protocol;
  const uint16_t session_id=48089;
  const bool control_v2=true;
  bool json_ready=false;
  struct {unsigned size=1889504;} manifest;
  char terminal[224]="s=48089 why=chunk_retry_exhausted";
  auto remaining_ms=[&](){return now_ms<budget?budget-now_ms:0;};
''' + send + '\n' + receive + r'''
  unsigned attempt=0;
  const auto result=sense_lcd_abort_cleanup([&](bool force_json){
    assert(force_json==(attempt!=0));
    ++attempt;
    protocol.write_ok=!(scenario==8&&attempt==1);
    const bool sent=send_control("LCD_OTA_ABORT","chunk_retry_exhausted",force_json);
    std::string reply=R"({"type":"LCD_OTA_ABORT_ACK","session_id":48089,"json_ready":true})";
    bool deliver=(scenario==0)||(scenario==1&&attempt==2)||(scenario==2&&attempt==3)||scenario==8;
    if(scenario==4){reply=R"({"type":"LCD_OTA_ABORT_ACK","session_id":48090,"json_ready":true})";deliver=true;}
    if(scenario==5){reply=R"({"type":"LCD_OTA_ABORT_ACK","session_id":48089,"json_ready":"true"})";deliver=true;}
    if(scenario==6){reply=R"({"type":"LCD_OTA_ABORT_ACK","session_id":48089,"json_ready":false})";deliver=true;}
    if(scenario==7){reply=R"({"type":"LCD_OTA_ABORT_ACK","json_ready":true})";deliver=true;}
    if(scenario==9){reply=R"({"type":"LCD_OTA_ABORT_ACK","session_id":65536,"json_ready":true})";deliver=true;}
    if(scenario==10&&attempt==3){input.push_back({36000,reply});}
    if(scenario==11&&attempt==2){now_ms=budget;input.push_back({now_ms,reply});}
    if(deliver&&sent)input.push_back({now_ms+50,reply});
    return sent;
  },[&](uint32_t wait){return !strcmp(receive_control(wait,false),"receiver_aborted");},remaining_ms);
  // These are actual production send/receive lambdas: framed first request,
  // delimiter + JSON replays, typed exact-session ACK, original slot bounds.
  assert(protocol.framed==1&&lcdSerial.plain==result.attempts-1);
  assert(result.elapsed_ms<=budget);
  if(result.acknowledged)assert(json_ready&&remaining_ms());
  return result;
}
int main(){
  auto r=run(0);assert(r.acknowledged&&r.attempts==1&&r.elapsed_ms==50&&r.sent_mask==1);
  r=run(1);assert(r.acknowledged&&r.attempts==2&&r.elapsed_ms==3050&&r.sent_mask==3);
  r=run(2);assert(r.acknowledged&&r.attempts==3&&r.elapsed_ms==6050&&r.sent_mask==7);
  for(unsigned c:{3,4,5,6,7,9,10}){
    r=run(c);assert(!r.acknowledged&&r.attempts==3&&r.elapsed_ms==36000);
  }
  r=run(3,5000);assert(!r.acknowledged&&r.attempts==2&&r.elapsed_ms==5000);
  r=run(8);assert(r.acknowledged&&r.attempts==2&&r.sent_mask==2&&r.elapsed_ms==3050);
  r=run(11,5000);assert(!r.acknowledged&&r.attempts==2&&r.elapsed_ms==5000);
  // millis wrap preserves the subtraction-based original deadline.
  now_ms=0xfffffff0U;unsigned calls=0;
  r=sense_lcd_abort_cleanup([&](bool){++calls;return true;},[&](uint32_t n){now_ms+=n;return false;},[](){return 50000U;});
  assert(!r.acknowledged&&r.elapsed_ms==36000&&calls==3);
  puts("PASS actual Sense cleanup/send/reply: lost first/second ACK, stale/type/missing/overflow rejection, short TX, deadline/late reply, bounded 3 sends and millis wrap");
}
'''


class AbortCleanupTests(unittest.TestCase):
    def test_actual_cleanup_and_correlated_reply(self):
        self.assertTrue(JSON.is_dir(), 'ArduinoJson dependency missing')
        with tempfile.TemporaryDirectory(prefix='halo-abort-cleanup-') as tmp:
            cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test';cpp.write_text(harness())
            subprocess.run([shutil.which('c++'),'-std=c++17','-Wno-deprecated-declarations','-I',str(JSON),str(cpp),'-o',str(binary)],check=True,timeout=30)
            subprocess.run([str(binary)],check=True,timeout=5)


if __name__=='__main__':
    unittest.main()

#!/usr/bin/env python3
"""Run the real Sense retry-arm transaction with timed ACK loss/cancellation."""
from pathlib import Path
from host_paths import arduino_user
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
JSON_INCLUDE = arduino_user() / 'libraries/ArduinoJson/src'
SOURCE = r'''
#include <ArduinoJson.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>
using String=std::string;
#define HALO_SENSE_PROD_WRAPPER 1
#define PROTOCOL_VERSION 1
static unsigned tick=0, generation=1, sequence=0, pumps=0;
static bool ordinary=true,query=false,provisioning=false;
static unsigned scenario=0;
static std::vector<std::string> tx;
static unsigned millis(){return tick;}
static void delay(unsigned ms){tick+=ms;}
static unsigned esp_random(){return ++sequence;}
static unsigned get_next_msg_id(){return ++sequence;}
static unsigned sense_user_action_generation(){return generation;}
static bool sense_uart_ordinary_tx_allowed(){return ordinary;}
static bool sense_lcd_query_busy(){return query;}
static bool halo_provisioning_active(){return provisioning;}
static void uart_send_json(const char* s){tx.emplace_back(s);}
static void pump_uart_rx_once();
static struct {template<class... A>void printf(const char*,A...){}}Serial;
#include "Sense_Minimal/sense_media_retry_transport.h"
static void pump_uart_rx_once(){
  ++pumps;
  if(scenario==8){++generation;return;}
  if(scenario==9){ordinary=false;return;}
  if(scenario==10 && tx.size()<2)return;
  if(scenario==11)return;
  JsonDocument d; assert(deserializeJson(d,tx.back())==DeserializationError::Ok);
  d["type"]="MEDIA_RETRY_ARM_ACK"; d["ok"]=1;
  if(scenario==1)d["token"]="0000000000000000";
  if(scenario==2)d["wake_in_s"]=299;
  if(scenario==3)d["ok"]=0;
  if(scenario==4)d.remove("ok");
  if(scenario==5)d["token"]=1;
  if(scenario==6)d["wake_in_s"]="300";
  if(scenario==7)d["ok"]="1";
  if(scenario==12)d["ok"]=true;
  if(scenario==13)d["wake_in_s"]=true;
  if(scenario==14)d.remove("token");
  media_retry_arm_ack(d);
}
static void reset(unsigned test){tick=0;generation=1;ordinary=true;query=provisioning=false;tx.clear();pumps=0;scenario=test;}
int main(){
  reset(0);assert(media_retry_arm_lcd(300,1));assert(tx.size()==1 && tick<1800);
  assert(!g_media_retry_arm_token[0]);
  for(unsigned test=1;test<=7;++test){
    reset(test);assert(!media_retry_arm_lcd(300,1));assert(tx.size()==2 && tx[0]==tx[1]);assert(tick==1800);
  }
  for(unsigned test:{12U,13U,14U}){
    reset(test);assert(!media_retry_arm_lcd(300,1));assert(tx.size()==2 && tx[0]==tx[1]);assert(tick==1800);
  }
  for(unsigned test=8;test<=9;++test){reset(test);assert(!media_retry_arm_lcd(300,1));assert(tx.size()==1 && tick<1800);}
  reset(10);assert(media_retry_arm_lcd(300,1));assert(tx.size()==2 && tx[0]==tx[1]);
  reset(11);assert(!media_retry_arm_lcd(300,1));assert(tick==1800);
  JsonDocument late;deserializeJson(late,tx.back());late["ok"]=1;media_retry_arm_ack(late);assert(!g_media_retry_arm_acked);
  reset(0);assert(media_retry_arm_lcd(0,1));JsonDocument clear;deserializeJson(clear,tx[0]);assert(clear["wake_in_s"].as<unsigned>()==0);
  for(unsigned busy=0;busy<3;++busy){reset(0);g_media_retry_arm_acked=true;if(busy==0)ordinary=false;if(busy==1)query=true;if(busy==2)provisioning=true;
    assert(!media_retry_arm_lcd(300,1));assert(tx.empty() && !g_media_retry_arm_acked);
  }
  puts("PASS actual Sense media arm: bound ACK, malformed/stale/lost replies, same-token retry, deadline, user cancellation, peer-busy and clear");
}
'''
with tempfile.TemporaryDirectory(prefix="halo-media-arm-") as directory:
    source=Path(directory)/"test.cpp"
    binary=Path(directory)/"test"
    source.write_text(SOURCE)
    subprocess.run(["c++","-std=c++17","-fsanitize=address,undefined","-I",str(ROOT),
                    "-I",str(JSON_INCLUDE),str(source),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
# This ordering is safety-relevant: pre_setup calls the nightly boot classifier.
ino=(ROOT/"Sense_Minimal/Sense_Minimal.ino").read_text()
setup=ino[ino.index("void setup() {"):]
assert setup.index("g_media_retry_timer_boot =") < setup.index("  halo_prod_pre_setup();")
assert "if (!g_media_retry_timer_boot) ota_on_timer_wake();" in setup
wrapper=(ROOT/"halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino").read_text()
assert "ESP_SLEEP_WAKEUP_TIMER && !g_media_retry_timer_boot" in wrapper

#!/usr/bin/env python3
"""Execute production rail averaging/cache/parser/serializer; hardware is doubled."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile
from host_paths import arduino_user

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "Sense_Minimal/Sense_Minimal.ino").read_text()
    dispatch = source[source.index("static bool parse_input_message(const char* json_str) {"):]
    assert dispatch.index("sense_power_accept_uart(doc)") < dispatch.index("last_lcd_communication = millis()")
    code = r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include <ctime>
static time_t wall_epoch=1790641591;
static time_t fake_time(time_t*){return wall_epoch;}
#define time fake_time
static int64_t clock_us=1000000;
static int64_t esp_timer_get_time(){return clock_us;}
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) do{}while(0)
#define portEXIT_CRITICAL(x) do{}while(0)
static struct{template<class...A>void printf(const char*,A...){}}Serial;
#include "Sense_Minimal/sense_power.h"
using namespace halo_power;
static Snapshot sample(unsigned boot=1,unsigned seq=1){
  Snapshot s;s.boot_id=boot;s.sequence=seq;s.epoch_s=1790641591;s.uptime_ms=999;
  Burst b;for(int i=0;i<32;++i)assert(b.add(2000+i,1990+i));b.finish(s);
  assert(s.system_supply_mv==4012);return s;
}
static JsonDocument wire(const Snapshot&s){
  JsonDocument d;d["type"]="LCD_POWER";auto p=d["p"].to<JsonObject>();
  p["v"]=1;p["b"]=s.boot_id;p["q"]=s.sequence;p["u"]=s.uptime_ms;
  p["e"]=s.epoch_s;p["a"]=50;p["s"]=unsigned(s.status);p["mv"]=s.system_supply_mv;
  p["lo"]=s.raw_min;p["hi"]=s.raw_max;p["n"]=s.samples;return d;
}
int main(){
  char out[kJsonCapacity];assert(halo_system_power_json(out,sizeof(out)));
  JsonDocument d;assert(!deserializeJson(d,out));assert(d["system_supply_mv"].isNull());
  assert(!d["valid"].as<bool>());assert(!strcmp(d["status"],"peer_missing"));
  Snapshot s=sample();assert(valid(s));
  // USB-fed voltage is still a supply measurement, never charge/cell percent.
  auto usb=s;usb.system_supply_mv=4646;
  assert(json(View{usb,0,true},out,sizeof(out)));assert(!deserializeJson(d,out));
  assert(d["battery_percent"].isNull()&&d["cell_mv"].isNull());
  assert(!strcmp(d["power_source"],"unknown"));
  Burst partial;partial.add(1000,900);partial.finish(s);assert(!valid(s));
  assert(s.system_supply_mv==0&&s.status==Status::ReadError);
  Burst clip;assert(!clip.add(4095,2500));clip.finish(s);assert(s.status==Status::Saturated);
  Burst zero;assert(!zero.add(0,0));zero.finish(s);assert(!valid(s));
  Burst badcal;assert(!badcal.add(1000,0));badcal.finish(s);assert(!valid(s));
  s=sample();Cache c;assert(c.accept(s,50,1000));
  assert(c.view(5950).age_ms==5000);assert(c.view(5951).age_ms==5001);
  assert(json(c.view(5951),out,sizeof(out)));assert(!deserializeJson(d,out));
  assert(d["valid"].as<bool>()&&!d["fresh"].as<bool>());assert(d["age_ms"]==5001);
  assert(!c.accept(s,0,9000));assert(c.view(9000).age_ms==8050);
  // Cross-board uptime never participates in age subtraction.
  s=sample(2);s.uptime_ms=9999999999ULL;assert(c.accept(s,5,12000));
  assert(c.view(12010).age_ms==15);assert(c.view(11999).age_ms==UINT64_MAX);
  assert(c.accept(sample(2,2),UINT64_MAX-1,13000));assert(c.view(13002).age_ms==UINT64_MAX);
  assert(json(c.view(13002),out,sizeof(out)));
  auto unknown=sample(3);unknown.status=Status::ReadError;
  assert(c.accept(unknown,0,14000));assert(json(c.view(14000),out,sizeof(out)));
  assert(!deserializeJson(d,out));assert(d["system_supply_mv"].isNull());
  // Max-width values must fit, and small buffers never return partial JSON.
  auto maximum=sample(UINT32_MAX,UINT32_MAX);maximum.epoch_s=UINT64_MAX;
  maximum.uptime_ms=UINT64_MAX;maximum.status=Status::CalibrationUnavailable;
  assert(json(View{maximum,UINT64_MAX,true},out,sizeof(out)));std::cout<<out<<"\n";
  char tiny[8];assert(!json(View{maximum,0,true},tiny,sizeof(tiny)));assert(tiny[0]==0);
  assert(!json(View{},nullptr,0));
  // Real parser accepts data but consumes bad optional frames without side effects.
  auto message=wire(sample());assert(sense_power_accept_uart(message));
  assert(halo_system_power_json(out,sizeof(out)));assert(!deserializeJson(d,out));assert(d["fresh"]==true);
  message["p"]["q"]=2;message["p"]["mv"]=-1;assert(sense_power_accept_uart(message));
  assert(sense_power_view().sample.sequence==1);
  message=wire(sample(1,2));message["p"]["a"]="fresh";assert(sense_power_accept_uart(message));
  assert(sense_power_view().sample.sequence==1);
  message=wire(sample(1,2));message["p"].remove("n");assert(sense_power_accept_uart(message));
  assert(sense_power_view().sample.sequence==1);
  message=wire(sample(1,2));message["p"]["s"]=255;assert(sense_power_accept_uart(message));
  assert(sense_power_view().sample.sequence==1);
  message=wire(sample(1,2));assert(sense_power_accept_uart(message));
  clock_us+=6000000;JsonDocument report;assert(sense_power_append(report.to<JsonObject>()));
  assert(report["system_power"]["fresh"]==false);assert(report["system_power"]["system_supply_mv"]==4012);
  assert(sense_power_json(out,sizeof(out)));assert(!deserializeJson(d,out));
  assert(report["system_power"].as<JsonVariantConst>()==d.as<JsonVariantConst>());
  message=wire(sample(1,3));wall_epoch=1790641591+30;
  assert(sense_power_accept_uart(message));assert(sense_power_json(out,sizeof(out)));
  assert(!deserializeJson(d,out));assert(d["age_ms"]==31000);assert(d["fresh"]==false);
  message=wire(sample(1,4));wall_epoch=0;assert(sense_power_accept_uart(message));
  assert(sense_power_json(out,sizeof(out)));assert(!deserializeJson(d,out));
  assert(d["age_ms"].isNull());assert(d["age_known"]==false);assert(d["fresh"]==false);
  message=wire(sample(1,5));wall_epoch=1790641590;assert(sense_power_accept_uart(message));
  assert(sense_power_json(out,sizeof(out)));assert(!deserializeJson(d,out));assert(d["fresh"]==false);
  g_system_power_cache=Cache{};assert(halo_system_power_json(out,sizeof(out)));
  assert(!deserializeJson(d,out));assert(d["system_supply_mv"].isNull());
  message["type"]="INPUT_WAKE";assert(!sense_power_accept_uart(message));
}
'''
    with tempfile.TemporaryDirectory(prefix="halo-power-") as tmp:
        tmp = Path(tmp)
        (tmp / "esp_timer.h").write_text("#pragma once\n")
        (tmp / "test.cpp").write_text(code)
        compiler = shutil.which("clang++")
        assert compiler, "clang++ required"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-fsanitize=address,undefined",
                        "-I" + str(tmp), "-I" + str(ROOT),
                        "-I" + str(arduino_user() / "libraries/ArduinoJson/src"),
                        str(tmp / "test.cpp"), "-o", str(tmp / "test")], check=True)
        result = subprocess.run([str(tmp / "test")], check=True, capture_output=True, text=True)
        maximum = json.loads(result.stdout)
        assert maximum["battery_percent"] is None
        print(f"PASS production power model/parser: worst fixture {len(result.stdout.strip())} bytes; ADC/radio/RTOS are not emulated")


if __name__ == "__main__":
    main()

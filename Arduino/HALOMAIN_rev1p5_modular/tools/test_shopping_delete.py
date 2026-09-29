#!/usr/bin/env python3
"""Run production list parsing and deletion against fake HTTP/UART boundaries.

Uses the installed ArduinoJson parser and the complete production functions.
--source-root can reproduce the observed failure using the 120 build snapshot.
No device, network, Arduino build, or repository source mutation is performed.
"""
import argparse
from pathlib import Path
from host_paths import arduino_user
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
    text = (root / 'Sense_Minimal/sense_list.h').read_text()
    helpers = []
    for signature in ('static const char* shopping_list_item_uuid(',
                      'static bool shopping_list_delete_response_ok(',
                      'static void shopping_list_send_delete_result(',
                      'static void shopping_list_delete_failed('):
        if signature in text:
            helpers.append(definition(text, signature))
    functions = [definition(text, signature) for signature in (
        'static int shopping_list_cmp_by_store(',
        'static bool parse_and_update_shopping_list(',
        'static void remove_item_from_ram_list_locked(',
        ('static ListRequestResult delete_item_from_api(' if 'static ListRequestResult delete_item_from_api(' in text else 'static void delete_item_from_api('))]
    return r'''
#include <ArduinoJson.h>
#include "halo_ota_demo/firmware/shared/SystemPowerTransport.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
using String=std::string;
// This suite isolates parsing/delete responses. Transport ownership is exercised
// with the actual lease in test_list_transport_serialization.py.
enum class ListRequestResult : uint8_t {Completed,Failed,Deferred};
struct SenseListTransportLease {explicit SenseListTransportLease(const char*){} explicit operator bool()const{return true;}};
struct SenseBackupWifiCall {explicit operator bool()const{return true;}};
static int failures, checks;
static void check(bool ok,const char* label){
  ++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",label);}
}
static constexpr int MAX_LIST_ITEMS=50,MAX_ITEM_LENGTH=64;
struct shopping_list_item_t{char text[MAX_ITEM_LENGTH],id[MAX_ITEM_LENGTH],huuid[64],store[48];};
static shopping_list_item_t g_shopping_list[MAX_LIST_ITEMS];
static int g_list_count,g_selected_index;
static void* g_list_mutex=reinterpret_cast<void*>(1);
static bool mutex_available=true;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static int xSemaphoreTake(void*,int){return mutex_available;}
static void xSemaphoreGive(void*){}
static unsigned now_ms=100,list_last_fetch_ok_ms;
static unsigned millis(){return now_ms;}
static void delay(unsigned ms){now_ms+=ms;}
static struct{
  template<class... A>void printf(const char*,A...){}
  template<class T>void print(const T&){}
  template<class T>void println(const T&){}
}Serial;
static std::vector<String> statuses;
static std::vector<String> wire,call_order;
static unsigned list_pushes,post_calls,http_ends,client_stops,dma_releases,dma_acquires;
static String posted,response;
static int response_code=200;
static constexpr int PROTOCOL_VERSION=1;
static uint32_t get_next_msg_id(){static uint32_t n=0;return ++n;}
static void uart_send_json(const char* s){wire.emplace_back(s);call_order.emplace_back("result");}
static void uart_send_ui_list(){++list_pushes;call_order.emplace_back("list");}
static void uart_send_ui_status(const char* s){statuses.emplace_back(s);}
static void load_owner_id_or_default(char* out,size_t size){std::snprintf(out,size,"qa-owner");}
static const char* TREPO_DEVICE_ID="qa-device";
static const char* TREPO_API_BASE_URL="https://example.invalid";
static const char* TREPO_LIST_ENDPOINT="/v1/list";
static void* g_camera_dma_reserve=reinterpret_cast<void*>(1);
static void camera_dma_reserve_release(const char*){++dma_releases;g_camera_dma_reserve=nullptr;}
static void camera_dma_reserve_acquire(const char*){++dma_acquires;g_camera_dma_reserve=reinterpret_cast<void*>(1);}
struct HaloNtpDnsGuard{};
struct WiFiClientSecure{void setInsecure(){}void stop(){++client_stops;}};
struct HTTPClient{
  bool begin(WiFiClientSecure&,const String&){return true;}
  void setTimeout(unsigned){}void setConnectTimeout(unsigned){}
  String power_header;
  void addHeader(const char* name,const char* value){if(!strcmp(name,"X-Halo-System-Power"))power_header=value;}
  int POST(const String& body){
    JsonDocument power;check(!deserializeJson(power,power_header),"power header is valid JSON on actual list POST");
    check(power["measurement"]=="lcd_system_supply" && power["system_supply_mv"].isNull(),"missing rail remains unknown on actual list POST");
    ++post_calls;posted=body;return response_code;
  }
  String getString(){return response;}
  void end(){++http_ends;}
};
''' + '\n'.join(helpers + functions) + r'''
static void reset(){
  std::memset(g_shopping_list,0,sizeof(g_shopping_list));g_list_count=0;g_selected_index=-1;
  statuses.clear();wire.clear();call_order.clear();posted.clear();response="{\"affectedRows\":1}";response_code=200;
  list_pushes=post_calls=http_ends=client_stops=dma_releases=dma_acquires=0;
  mutex_available=true;g_list_mutex=reinterpret_cast<void*>(1);
  g_camera_dma_reserve=reinterpret_cast<void*>(1);
}
static void parse(const char* uuid_fields){
  String payload="{\"items\":[{\"id\":6,\"product_name\":\"QA Olives\",\"store\":\"\",";
  payload+=uuid_fields;payload+="},{\"id\":\"7\",\"product_name\":\"QA untouched\",\"itemUUID\":\"other-uuid\",\"store\":\"\"}]}";
  check(parse_and_update_shopping_list(payload),"actual list parser accepts fixture");
  check(g_list_count==2,"both fixture rows parsed");
  check(!std::strcmp(g_shopping_list[0].id,"6"),"numeric row id retained as lookup string");
  statuses.clear();list_pushes=0;wire.clear();call_order.clear();
}
static void result_message(const char* id,bool ok,const char* reason=nullptr){
  check(wire.size()==1,"one actual correlated delete result emitted");
  if(wire.size()!=1)return;
  DynamicJsonDocument message(512);check(!deserializeJson(message,wire[0]),"delete result JSON valid");
  check(!std::strcmp(message["type"]|"","LIST_DELETE_RESULT"),"delete result has dedicated type");
  check(!std::strcmp(message["id"]|"",id),"delete result preserves requested row ID");
  check(message["ok"].is<bool>()&&message["ok"].as<bool>()==ok,"delete result truthfully reports success/failure");
  if(reason)check(!std::strcmp(message["reason"]|"",reason),"delete result gives exact refusal reason");
}
static void expected_uuid(const char* expected){
  check(!std::strcmp(g_shopping_list[0].huuid,expected),"actual parser retains canonical/legacy UUID");
}
static void successful_delete(const char* expected,bool alias=false){
  delete_item_from_api(alias?g_shopping_list[0].id:"6");
  DynamicJsonDocument body(512);check(!deserializeJson(body,posted),"actual POST JSON valid");
  check(!std::strcmp(body["itemUUID"]|"",expected),"actual remove body uses correct UUID");
  check(!body.containsKey("id"),"UUID-backed remove never substitutes numeric row id");
  check(post_calls==1&&g_list_count==1,"positive affectedRows removes one cached row");
  check(!std::strcmp(g_shopping_list[0].id,"7"),"unrelated cached row preserved");
  check(statuses.size()==1&&statuses[0]=="Item deleted","success UI follows actual affected row");
  result_message("6",true);
  check(http_ends==1&&client_stops==1&&dma_releases==1&&dma_acquires==1,"HTTP and DMA resources closed");
}
static void rejected_response(const char* body,int code=200){
  reset();parse("\"itemUUID\":\"canonical-uuid\"");
  shopping_list_item_t before[MAX_LIST_ITEMS];std::memcpy(before,g_shopping_list,sizeof(before));
  response=body;response_code=code;delete_item_from_api("6");
  check(g_list_count==2&&!std::memcmp(before,g_shopping_list,sizeof(before)),"unconfirmed delete preserves cached rows");
  bool claimed=false;for(const auto& s:statuses)claimed|=s=="Item deleted";
  check(!claimed,"zero/malformed/error response never claims deletion");
  result_message("6",false);
  check(list_pushes==1,"failed delete requests existing list reconciliation");
  check(call_order.size()==2&&call_order[0]=="result"&&call_order[1]=="list","failure result precedes list reconciliation");
  check(http_ends==1&&client_stops==1&&dma_releases==1&&dma_acquires==1,"failure closes HTTP and DMA");
}
int main(){
  reset();parse("\"itemUUID\":\"canonical-uuid\"");expected_uuid("canonical-uuid");successful_delete("canonical-uuid");
  reset();parse("\"household_item_uuid\":\"legacy-uuid\"");expected_uuid("legacy-uuid");successful_delete("legacy-uuid");
  reset();parse("\"itemUUID\":\"canonical-uuid\",\"household_item_uuid\":\"legacy-uuid\"");expected_uuid("canonical-uuid");successful_delete("canonical-uuid");
  reset();parse("\"itemUUID\":\"canonical-uuid\"");successful_delete("canonical-uuid",true);
  reset();parse("\"itemUUID\":\"\",\"household_item_uuid\":\"legacy-uuid\"");expected_uuid("legacy-uuid");
  reset();parse("\"itemUUID\":null,\"household_item_uuid\":\"legacy-uuid\"");expected_uuid("legacy-uuid");
  std::puts("CHECKED canonical itemUUID, legacy alias, precedence and numeric lookup ID through actual parser/HTTP path");

  for(const char* body:{"{\"affectedRows\":0}","{}","not json","{\"affectedRows\":-1}",
                       "{\"affectedRows\":true}","{\"affectedRows\":0.5}","{\"affectedRows\":\"1\"}"})
    rejected_response(body);
  rejected_response("{\"affectedRows\":1}",500);
  rejected_response("",-1);
  std::puts("CHECKED HTTP200 zero-row/malformed/invalid counts and HTTP/network failure never fabricate success");

  reset();parse("\"unrelated\":true");expected_uuid("");
  response="{\"affectedRows\":0}";delete_item_from_api("6");
  check(post_calls==0,"missing UUID makes no legacy numeric-ID HTTP request");
  check(g_list_count==2,"missing UUID zero-row response preserves cache");
  bool claimed=false;for(const auto& s:statuses)claimed|=s=="Item deleted";
  check(!claimed,"missing UUID zero-row response never claims success");
  result_message("6",false,"missing_uuid");
  reset();parse("\"itemUUID\":\"canonical-uuid\"");delete_item_from_api("999");
  check(post_calls==0&&g_list_count==2,"unknown row ID makes no HTTP request or cache change");
  result_message("999",false,"missing_uuid");
  reset();delete_item_from_api("6");
  check(post_calls==0&&list_pushes==0,"empty boot cache never becomes an authoritative empty list");
  result_message("6",false,"missing_uuid");
  reset();parse("\"itemUUID\":\"canonical-uuid\"");mutex_available=false;delete_item_from_api("6");
  check(post_calls==0&&g_list_count==2,"unavailable UUID lookup mutex makes no HTTP request or cache change");
  result_message("6",false,"missing_uuid");
  reset();parse("\"itemUUID\":\"canonical-uuid\"");delete_item_from_api(nullptr);delete_item_from_api("");
  check(post_calls==0&&g_list_count==2,"invalid delete IDs make no HTTP request");
  reset();check(!parse_and_update_shopping_list("{broken"),"malformed list rejected");
  check(!parse_and_update_shopping_list("{\"items\":{}}"),"non-array list rejected");
  std::printf("%s %d production-path checks (%d failures)\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--arduino-json', type=Path,
                        default=arduino_user() / 'libraries/ArduinoJson/src')
    args = parser.parse_args()
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler or not (args.arduino_json / 'ArduinoJson.h').is_file():
        parser.error('C++ compiler and canonical ArduinoJson include directory are required')
    with tempfile.TemporaryDirectory(prefix='halo-shopping-delete-') as directory:
        cpp = Path(directory) / 'test.cpp'
        executable = Path(directory) / 'test'
        cpp.write_text(harness(args.source_root))
        subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra',
                        '-Wno-unused-variable', '-Wno-deprecated-declarations',
                        '-I', str(args.arduino_json), '-I', str(args.source_root),
                        str(cpp), '-o', str(executable)], check=True, timeout=30)
        result = subprocess.run([str(executable)], timeout=5)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()

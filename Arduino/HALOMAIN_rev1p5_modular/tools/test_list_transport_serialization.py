#!/usr/bin/env python3
"""Exercise complete production list requests and transport leases offline.

Uses actual ArduinoJson, list functions and camera/network admission with SDK,
HTTP and semaphore doubles. No device, network or firmware build is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
from host_paths import arduino_user
import shutil
import subprocess
import tempfile

from test_shopping_delete import definition, harness as delete_harness


def harness(root):
    # Reuse the existing list/cache/UART boundary fixtures; replace only the
    # transport doubles, then compile the complete list header under review.
    prefix = delete_harness(root).split('static const char* shopping_list_item_uuid(')[0]
    start = prefix.index('// This suite isolates')
    end = prefix.index('static int failures', start)
    prefix = prefix[:start] + prefix[end:]
    prefix = prefix.replace('using String=std::string;', r'''
#include <atomic>
#include <functional>
#include <mutex>
#include <algorithm>
#include <cctype>
struct String : std::string {
 using std::string::string;
 String(const std::string& s):std::string(s){}
 char charAt(size_t n)const{return n<size()?at(n):0;}
 int indexOf(const char* s)const{auto n=find(s);return n==npos?-1:int(n);}
 void toLowerCase(){std::transform(begin(),end(),begin(),[](unsigned char c){return std::tolower(c);});}
 String substring(size_t a,size_t b)const{return substr(a,b-a);}
 size_t write(uint8_t c){push_back(char(c));return 1;}
 size_t write(const uint8_t* s,size_t n){append(reinterpret_cast<const char*>(s),n);return n;}
};
template<class T>static auto deserializeJson(T& doc,const String& s)
 -> decltype(ArduinoJson::deserializeJson(doc,static_cast<const std::string&>(s))){
 return ArduinoJson::deserializeJson(doc,static_cast<const std::string&>(s));
}
''')
    prefix = prefix.replace('static int xSemaphoreTake(void*,int){return mutex_available;}\nstatic void xSemaphoreGive(void*){}', r'''
static void* http_mutex=reinterpret_cast<void*>(2);
static bool http_inflight,transport_locked;
static unsigned transport_take_calls,transport_give_calls,io_calls,client_live,http_live;
static unsigned tries_while_owned,owner_attempts,owner_completed;
static bool provisioning,offline,camera_after_take,owner_missing;
static bool wifi_connected=true,heap_ok=true,host_ok=true,http_begin_ok=true;
static std::vector<std::string> lifetime;
static std::function<void()> inside_post;
static bool xSemaphoreTake(void* mutex,int ticks){
 if(mutex!=http_mutex)return mutex_available;
 ++transport_take_calls;check(ticks==0,"list mutex admission never blocks");
 if(transport_locked)return false;
 transport_locked=true;lifetime.emplace_back("take");
 return true;
}
static void xSemaphoreGive(void* mutex){
 if(mutex!=http_mutex)return;
 check(transport_locked,"release belongs to admitted transport");
 check(!http_inflight,"HTTP flag cleared before mutex release");
 check(!client_live&&!http_live,"all HTTP/TLS objects destroyed before mutex release");
 transport_locked=false;++transport_give_calls;lifetime.emplace_back("give");
}
''')
    prefix = prefix.replace('static void load_owner_id_or_default(char* out,size_t size){std::snprintf(out,size,"qa-owner");}',
                            'static void load_owner_id_or_default(char* out,size_t size){std::snprintf(out,size,"%s",owner_missing?"":"qa-owner");}')
    prefix = prefix.replace('static void camera_dma_reserve_release(const char*){++dma_releases;g_camera_dma_reserve=nullptr;}', r'''
static void camera_dma_reserve_release(const char*){
 check(transport_locked&&http_inflight,"DMA release occurs inside transport lease");
 ++dma_releases;g_camera_dma_reserve=nullptr;lifetime.emplace_back("dma_release");
}''')
    prefix = prefix.replace('static void camera_dma_reserve_acquire(const char*){++dma_acquires;g_camera_dma_reserve=reinterpret_cast<void*>(1);}', r'''
static bool camera_dma_reserve_acquire(const char*){
 check(transport_locked&&http_inflight,"reserve restored before unlocking transport");
 check(!client_live&&!http_live,"TLS destructors precede DMA restoration");
 ++dma_acquires;g_camera_dma_reserve=reinterpret_cast<void*>(1);lifetime.emplace_back("dma_restore");
 return true;
}''')
    start = prefix.index('struct WiFiClientSecure')
    prefix = prefix[:start] + r'''
static void io(){++io_calls;check(transport_locked&&http_inflight,"network I/O owns transport");}
struct WiFiClientSecure{
 WiFiClientSecure(){io();++client_live;lifetime.emplace_back("client_create");}
 ~WiFiClientSecure(){--client_live;lifetime.emplace_back("client_destroy");}
 void setInsecure(){}void stop(){io();++client_stops;}
 int lastError(char* p,size_t n){if(n)p[0]=0;return 0;}
};
struct HTTPClient{
 HTTPClient(){++http_live;}
 ~HTTPClient(){--http_live;lifetime.emplace_back("http_destroy");}
 bool begin(WiFiClientSecure&,const String&){io();return http_begin_ok;}
 void setTimeout(unsigned){}void setConnectTimeout(unsigned){}
 String power_header;
 void addHeader(const char* name,const char* value){if(!strcmp(name,"X-Halo-System-Power"))power_header=value;}
 int POST(const String& body){
  JsonDocument power;check(!deserializeJson(power,power_header),"actual view/remove power header parses");
  check(power["measurement"]=="lcd_system_supply" && power["system_supply_mv"].isNull(),"actual view/remove unknown rail is null");
  io();++post_calls;posted=body;if(inside_post)inside_post();return response_code;
 }
 String getString(){io();return response;}
 String errorToString(int){return "connection failed";}
 void end(){io();++http_ends;}
};
#define HALO_SENSE_PROD_WRAPPER 1
#define portMUX_TYPE std::mutex
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
static bool halo_provisioning_active(){return provisioning;}
static std::atomic<bool> g_camera_radio_off_owned{false};
namespace sense_backup_diag{
 enum {Idle=0,Offline=1};
 static std::atomic<unsigned> phase{Idle},wifi_calls{0};
}
static bool sense_backup_offline_active(){
 if(camera_after_take)g_camera_radio_off_owned.store(true);
 return offline;
}
''' + definition((root / 'Sense_Minimal/sense_backup_diagnostic.h').read_text(),
                 'class SenseBackupWifiCall') + r''';
static const int WL_CONNECTED=3,HTTPC_ERROR_READ_TIMEOUT=-11;
struct IPAddress{
 explicit operator uint32_t()const{return wifi_connected?1:0;}
 String toString()const{return "192.0.2.1";}
};
static struct{
 int status(){return wifi_connected?WL_CONNECTED:0;}
 IPAddress localIP(){return {};}
 IPAddress dnsIP(int){return {};}
 bool hostByName(const char*,IPAddress&){io();return true;}
}WiFi;
static bool wifi_connect_inflight;
static bool ensure_wifi_connected(const char*,unsigned){io();return false;}
static bool ensure_dns_ready(const char*){io();return true;}
static bool extract_host_from_url(const String&,String& host){host="example.invalid";return host_ok;}
static const int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;
static size_t heap_caps_get_free_size(int){return heap_ok?65536:1000;}
static const char* g_last_refresh_reason="test";
static void vTaskDelay(unsigned n){delay(n);}
static long random(int,int){return 0;}
static void list_refresh_mark_complete(const char*){}
#include "sense_list.h"
static void reset(){
 check(!client_live&&!http_live,"previous TLS scope closed");
 std::memset(g_shopping_list,0,sizeof(g_shopping_list));g_list_count=1;g_selected_index=0;
 std::strcpy(g_shopping_list[0].id,"6");std::strcpy(g_shopping_list[0].huuid,"fixture-uuid");
 std::strcpy(g_shopping_list[0].text,"Retained item");
 statuses.clear();wire.clear();call_order.clear();posted.clear();lifetime.clear();
 response="{\"items\":[]}";response_code=200;
 list_pushes=post_calls=http_ends=client_stops=dma_releases=dma_acquires=0;
 transport_take_calls=transport_give_calls=io_calls=0;now_ms=100;
 mutex_available=true;http_mutex=reinterpret_cast<void*>(2);transport_locked=http_inflight=false;
 provisioning=offline=camera_after_take=owner_missing=false;
 wifi_connected=heap_ok=host_ok=http_begin_ok=true;wifi_connect_inflight=false;
 g_camera_radio_off_owned.store(false);sense_backup_diag::phase.store(0);sense_backup_diag::wifi_calls.store(0);
 g_camera_dma_reserve=reinterpret_cast<void*>(1);inside_post={};
 sense_action_summary::state={};
}
static ListRequestResult request(bool remove){
 if(remove)response="{\"affectedRows\":1}";
 return remove?delete_item_from_api("6"):fetch_shopping_list_from_api();
}
static void closed(){
 check(!transport_locked&&!http_inflight,"transport released on return");
 check(sense_backup_diag::wifi_calls.load()==0,"complete network ownership released");
 check(transport_give_calls==1,"one unlock per admitted request");
}
static void refused(bool remove,int mode){
 reset();
 if(mode==0){transport_locked=http_inflight=true;}
 if(mode==1)provisioning=true;
 if(mode==2)g_camera_radio_off_owned.store(true);
 if(mode==3)offline=true;
 if(mode==4)http_mutex=nullptr;
 if(mode==5)camera_after_take=true;
 const auto before=sense_action_summary::snapshot();
 const auto result=request(remove);
 check(result==ListRequestResult::Deferred,"busy/provision/camera/offline request is deferred");
 check(now_ms==100,"deferred admission consumes no wait budget");
 check(!io_calls&&!post_calls&&!dma_releases&&!dma_acquires,"deferred request has no network/DMA work");
 check(wire.empty()&&statuses.empty()&&!list_pushes&&g_list_count==1,"deferred request preserves UI/cache/request outcome");
 check(sense_action_summary::snapshot().attempts==before.attempts,"deferred refresh is not a failed attempt");
 check(sense_backup_diag::wifi_calls.load()==0,"refused admission retains no network counter");
 if(mode==0)check(transport_locked&&http_inflight&&!transport_give_calls,"foreign mutex/HTTP owner untouched");
 else check(!transport_locked&&!http_inflight,"partial admission releases lease");
}
static void completed(bool remove){
 reset();
 inside_post=[](){
   check(sense_backup_diag::wifi_calls.load()==1,"network counter spans list HTTP");
   const auto releases=dma_releases;
   SenseListTransportLease competitor("competing_voice");
   check(!competitor,"fresh voice cannot enter while list TLS owns mutex");
   check(dma_releases==releases,"losing transport entrant cannot release list DMA");
 };
 check(request(remove)==ListRequestResult::Completed,"successful HTTP request completes");
 check(post_calls==1&&dma_releases==1&&dma_acquires==1,"one request releases/restores camera reserve");
 closed();
 auto position=[](const char* event){return std::find(lifetime.begin(),lifetime.end(),event)-lifetime.begin();};
 check(position("client_destroy")<position("dma_restore")&&position("dma_restore")<position("give"),
       "complete TLS destruction then DMA restoration then unlock");
 {SenseListTransportLease next("next_voice");check(bool(next),"next voice transport can acquire after cleanup");}
}
static void full_voice_cleanup_blocks_list(){
 reset();
 {
   // The shared mutex is held by the complete voice scope, including the
   // cleanup tail after POST. This is the media-side contract, not a live SDK.
   SenseListTransportLease voice("voice");
   {
     SenseBackupWifiCall owner;
     check(bool(owner),"voice complete call admitted");
     camera_dma_reserve_release("voice");
     check(fetch_shopping_list_from_api()==ListRequestResult::Deferred,"list waits through voice HTTP phase");
     check(delete_item_from_api("6")==ListRequestResult::Deferred,"delete waits through voice cleanup tail");
     camera_dma_reserve_acquire("voice");
     check(fetch_shopping_list_from_api()==ListRequestResult::Deferred,"restored reserve alone does not release voice lease");
   }
   check(fetch_shopping_list_from_api()==ListRequestResult::Deferred,"counter cleared alone does not release voice lease");
 }
 check(fetch_shopping_list_from_api()==ListRequestResult::Completed,"pending list enters after entire voice scope ends");
}
int main(){
 for(bool remove:{false,true}){
   for(int mode=0;mode<6;++mode)refused(remove,mode);
   completed(remove);
   reset();owner_missing=true;
   check(request(remove)==ListRequestResult::Failed,"missing owner is a real failure after admission");closed();
   reset();response_code=500;
   check(request(remove)==ListRequestResult::Failed,"HTTP failure returns failed and releases resources");closed();
 }
 reset();heap_ok=false;
 check(fetch_shopping_list_from_api()==ListRequestResult::Failed,"low heap refuses list handshake");
 check(!post_calls&&!client_live&&dma_releases==1&&dma_acquires==1,"low-heap path restores reserve without TLS");closed();
 reset();response_code=-1;
 check(fetch_shopping_list_from_api()==ListRequestResult::Failed&&post_calls==2,"existing finite HTTP retry remains");
 check(dma_releases==2&&dma_acquires==2&&transport_give_calls==1,"retry keeps one transport lease across both attempts");closed();
 reset();http_begin_ok=false;
 check(fetch_shopping_list_from_api()==ListRequestResult::Failed&&!post_calls,"begin failure closes both attempts");closed();
 reset();wifi_connected=false;
 check(fetch_shopping_list_from_api()==ListRequestResult::Failed&&!post_calls,"WiFi failure releases transport before any TLS");closed();
 full_voice_cleanup_blocks_list();
 std::printf("%s %u actual list transport checks (%u failures)\n",failures?"FAIL":"PASS",checks,failures);
 return failures?1:0;
}
'''
    return prefix


def run(root, out, arduino_json, sanitize):
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler or not (arduino_json / 'ArduinoJson.h').is_file():
        raise SystemExit('C++ compiler and installed ArduinoJson are required')
    cpp, binary = out / 'list-transport.cpp', out / 'list-transport'
    cpp.write_text(harness(root))
    cmd = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Wno-unused-function',
           '-Wno-unused-variable', '-Wno-deprecated-declarations',
           '-I', str(arduino_json), '-I', str(root), '-I', str(root / 'Sense_Minimal'),
           str(cpp), '-o', str(binary)]
    if sanitize:
        cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with (out / 'compile.log').open('w') as log:
        built = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=45)
    if built.returncode:
        print((out / 'compile.log').read_text())
        raise SystemExit(built.returncode)
    with (out / 'run.log').open('w') as log:
        tested = subprocess.run([str(binary)], stdout=log, stderr=subprocess.STDOUT, timeout=10)
    print((out / 'run.log').read_text(), end='')
    result = {'status': 'PASS' if tested.returncode == 0 else 'FAIL',
              'exit_code': tested.returncode, 'sanitizers': sanitize,
              'source_files': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                               for p in [root / 'Sense_Minimal/sense_list.h',
                                         root / 'Sense_Minimal/sense_list_transport.h',
                                         root / 'Sense_Minimal/sense_backup_diagnostic.h',
                                         root / 'halo_ota_demo/firmware/shared/SystemPower.h',
                                         root / 'halo_ota_demo/firmware/shared/SystemPowerTransport.h']},
              'limits': 'SDK/HTTP/semaphore doubles; voice full-scope lease modeled; no hardware latency or heap guarantee.'}
    (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    return tested.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path)
    parser.add_argument('--arduino-json', type=Path, default=arduino_user() / 'libraries/ArduinoJson/src')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if args.out:
        args.out.mkdir(parents=True, exist_ok=False)
        raise SystemExit(run(args.source_root, args.out, args.arduino_json, args.sanitize))
    with tempfile.TemporaryDirectory(prefix='halo-list-transport-') as directory:
        raise SystemExit(run(args.source_root, Path(directory), args.arduino_json, args.sanitize))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Run the production delete/inventory function against typed UART fault edges."""
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_voice_persistence import definition

ROOT = Path(__file__).resolve().parents[1]

def harness():
    source = (ROOT / 'Sense_Minimal/sense_voice_spool.h').read_text()
    return r'''
#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
struct UploadJob {size_t image_len=100;struct {const char* owner_id="owner-a";
 const char* device_id="device";const char* request_id="request";uint32_t crc32=42;}voice;};
static unsigned checks=0;
static void check(bool x){++checks;assert(x);}
static uint32_t now=100,deadline_seen=0,g_voice_spool_depth=1;
static char g_voice_spool_cursor[33]="retained";
static bool lease_ok=true,delete_ok=true,reply_ok=true,owner_ok=true,expire=false;
static bool foreground_active=false,voice_recording_active=false;
static struct {bool active=false;}current_job;
static std::atomic<bool> g_media_retry_user_paused{false};
static unsigned reads=0,sends=0,inventories=0,leases=0;
static bool pending=true;
static std::string fault,probe;
static uint32_t millis(){return now;}
static uint32_t esp_random(){return 0x12345678;}
static uint32_t sense_voice_spool_remaining(uint32_t end){return int32_t(end-now)>0?end-now:0;}
struct SenseVoiceUartLease {SenseVoiceUartLease(uint32_t end){deadline_seen=end;++leases;}
 ~SenseVoiceUartLease(){--leases;}bool held(){return lease_ok;}};
static bool sense_voice_owner_matches(const UploadJob&){return owner_ok;}
namespace halo_media_retry {enum Store{VoiceSd};}
static void media_retry_inventory(halo_media_retry::Store,bool value){++inventories;pending=value;}
static struct {template<class...T>void printf(const char*,T...){}}Serial;
static void sense_voice_spool_json(JsonDocument& d,const char* type){
 ++sends;if(!strcmp(type,"VOICE_SPOOL_LIST_REQ")){
  check(!strcmp(d["owner_id"]|"","owner-a"));check(!strcmp(d["device_id"]|"","device"));
  check(!d["after_request_id"].is<const char*>());probe=d["probe"].as<const char*>();check(probe.size()==16);
 }}
static bool sense_voice_spool_read(JsonDocument& d,const char* type,const char* request,uint32_t end){
 check(end==deadline_seen);++reads;d.clear();
 if(!strcmp(type,"VOICE_SPOOL_DELETE_ACK")){
  check(request&&!strcmp(request,"request"));d["ok"]=delete_ok?1:0;if(expire)now=end;return true;
 }
 check(!request);if(!reply_ok)return false;
 d["ok"]=1;d["reason"]="empty";d["owner_id"]="owner-a";d["device_id"]="device";
 d["probe"]=probe;d["count"]=0;
 if(fault=="remaining"){d["count"]=2;d["reason"]="ok";}
 if(fault=="owner")d["owner_id"]="owner-b";
 if(fault=="device")d["device_id"]="other";
 if(fault=="probe")d["probe"]="8765432187654321";
 if(fault=="missing")d.remove("count");
 if(fault=="negative")d["count"]=-1;
 if(fault=="string")d["count"]="0";
 if(fault=="reason")d["reason"]="io";
 if(fault=="busy")d["ok"]=0;
 if(fault=="transfer")owner_ok=false;
 if(fault=="late")now=end;
 if(fault=="pause")g_media_retry_user_paused=true;
 if(fault=="foreground")foreground_active=true;
 if(fault=="capture")current_job.active=true;
 if(fault=="recording")voice_recording_active=true;
 return true;
}
''' + definition(source, 'static void sense_voice_spool_identity(') + '\n' + definition(source, 'static bool sense_voice_spool_delete(') + r'''
static void reset(){now=100;g_voice_spool_depth=1;strcpy(g_voice_spool_cursor,"retained");
 lease_ok=delete_ok=reply_ok=owner_ok=pending=true;expire=false;reads=sends=inventories=0;
 foreground_active=voice_recording_active=current_job.active=false;g_media_retry_user_paused=false;fault.clear();}
int main(){UploadJob job;
 reset();check(sense_voice_spool_delete(job));check(inventories==1&&!pending&&g_voice_spool_depth==0);
 check(!g_voice_spool_cursor[0]&&reads==2&&sends==2&&!leases&&deadline_seen==4100);
 reset();fault="remaining";check(sense_voice_spool_delete(job));check(inventories==1&&pending&&g_voice_spool_depth==2);
 check(!strcmp(g_voice_spool_cursor,"retained"));
 for(const char* f:{"owner","device","probe","missing","negative","string","reason","busy","transfer",
                    "late","pause","foreground","capture","recording"}){
  reset();fault=f;check(sense_voice_spool_delete(job));check(!inventories&&pending&&g_voice_spool_depth==1&&!leases);
 }
 reset();reply_ok=false;check(sense_voice_spool_delete(job));check(pending&&!inventories&&!leases);
 reset();delete_ok=false;check(!sense_voice_spool_delete(job));check(reads==1&&sends==1&&pending&&!inventories);
 reset();lease_ok=false;check(!sense_voice_spool_delete(job));check(!reads&&!sends&&!leases);
 for(int i=0;i<6;++i){reset();if(i==0)expire=true;if(i==1)owner_ok=false;if(i==2)g_media_retry_user_paused=true;
  if(i==3)foreground_active=true;if(i==4)current_job.active=true;if(i==5)voice_recording_active=true;
  check(sense_voice_spool_delete(job));check(reads==1&&sends==1&&!inventories&&pending&&!leases);
 }
 printf("PASS %u actual-function post-delete inventory checks\n",checks);
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix='halo-post-delete-') as name:
        path = Path(name)
        (path / 'test.cpp').write_text(harness())
        subprocess.run([shutil.which('clang++') or 'c++', '-std=c++17', '-O1',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I'+str(Path('/Users/MattTaylor/Documents/Arduino/libraries/ArduinoJson/src')),
                        str(path/'test.cpp'), '-o', str(path/'test')], check=True, timeout=45)
        subprocess.run([str(path/'test')], check=True, timeout=10)

if __name__ == '__main__':
    main()

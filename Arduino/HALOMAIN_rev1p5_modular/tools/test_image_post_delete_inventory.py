#!/usr/bin/env python3
"""Execute the actual image delete/inventory function at scripted UART boundaries.

No device/cloud/storage operations. The real ArduinoJson type checks and media
retry policy run; the lease, clock, ownership and UART replies are host doubles.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_voice_persistence import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    source = (root / 'Sense_Minimal/sense_image_spool.h').read_text()
    return r'''
#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "halo_common/MediaRetryPolicy.h"
struct UploadJob {size_t image_len=100;struct {const char* owner_id="owner-a";
 const char* device_id="device";const char* request_id="request";uint32_t crc32=42;}image;};
static unsigned checks=0;
static const char* scenario="setup";
static void check(bool value,const char* message){
 ++checks;if(!value){fprintf(stderr,"FAIL %s: %s\n",scenario,message);abort();}
}
static uint32_t now=100,deadline_seen=0,g_image_spool_depth=1,random_value=0;
static char g_image_spool_cursor[33]="retained";
static bool lease_ok=true,delete_reply=true,list_reply=true,owner_ok=true;
static bool foreground_active=false,voice_recording_active=false;
static struct {bool active=false;}current_job;
static std::atomic<bool> g_media_retry_user_paused{false};
static unsigned reads=0,sends=0,inventories=0,leases=0,lease_acquisitions=0;
static halo_media_retry::State retry_state;
static std::string fault,probe;
static int takeover_after_delete=-1,takeover_after_list=-1;
static uint32_t delete_duration=20,list_duration=30;
static uint32_t millis(){return now;}
static uint32_t esp_random(){return ++random_value;}
struct SenseImageUartLease {
 explicit SenseImageUartLease(uint32_t end){deadline_seen=end;++leases;++lease_acquisitions;}
 ~SenseImageUartLease(){--leases;}
 bool held()const{return lease_ok;}
};
static bool sense_image_owner_matches(const UploadJob&){return owner_ok;}
static void media_retry_inventory(halo_media_retry::Store store,bool value){
 check(store==halo_media_retry::ImageSd,"only ImageSd may change");
 ++inventories;halo_media_retry::inventory(retry_state,store,value);
}
static struct {template<class...T>void printf(const char*,T...){}}Serial;
static void takeover(int kind){
 if(kind==0)owner_ok=false;
 if(kind==1)g_media_retry_user_paused=true;
 if(kind==2)foreground_active=true;
 if(kind==3)current_job.active=true;
 if(kind==4)voice_recording_active=true;
}
static void sense_image_spool_json(JsonDocument& d,const char* type){
 check(leases==1&&lease_ok,"all JSON remains in the single acquired lease");
 ++sends;
 check(!strcmp(d["owner_id"]|"","owner-a"),"request owner is bound");
 check(!strcmp(d["device_id"]|"","device"),"request device is bound");
 if(!strcmp(type,"IMAGE_SPOOL_DELETE")){
  check(!strcmp(d["request_id"]|"","request"),"delete request ID is unchanged");
  check(d["len"].as<uint32_t>()==100&&d["crc32"].as<uint32_t>()==42,"delete payload proof is unchanged");
 }else{
  check(!strcmp(type,"IMAGE_SPOOL_LIST_REQ"),"only one inventory query follows deletion");
  check(!d["after_request_id"].is<const char*>()&&!d["request_id"].is<const char*>(),"inventory starts at the whole queue, not a stale cursor");
  check(!d["len"].is<uint32_t>()&&!d["crc32"].is<uint32_t>(),"inventory clears delete-only fields");
  probe=d["probe"].as<const char*>();
  check(probe.size()==16&&probe!="0000000000000000","inventory has a fresh probe");
 }
}
static bool sense_image_spool_read(JsonDocument& d,const char* type,const char* request,uint32_t end){
 check(end==deadline_seen&&leases==1,"both reads share the original deadline and lease");
 ++reads;d.clear();
 if(!strcmp(type,"IMAGE_SPOOL_DELETE_ACK")){
  check(request&&!strcmp(request,"request"),"delete ACK is request-bound");
  now+=delete_duration;takeover(takeover_after_delete);
  if(!delete_reply)return false;
  d["ok"]=1;
  if(fault=="delete_refused")d["ok"]=0;
  if(fault=="delete_missing")d.remove("ok");
  if(fault=="delete_string")d["ok"]="1";
  if(fault=="delete_bool")d["ok"]=true;
  return true;
 }
 check(!strcmp(type,"IMAGE_SPOOL_LIST")&&!request,"typed inventory response is probe-bound");
 now+=list_duration;takeover(takeover_after_list);
 if(!list_reply)return false;
 d["ok"]=1;d["reason"]="empty";d["owner_id"]="owner-a";d["device_id"]="device";
 d["probe"]=probe;d["count"]=0;
 if(fault=="remaining"){d["count"]=2;d["reason"]="ok";}
 if(fault=="ok_empty")d["reason"]="ok";
 if(fault=="owner")d["owner_id"]="owner-b";
 if(fault=="owner_missing")d.remove("owner_id");
 if(fault=="device")d["device_id"]="other";
 if(fault=="device_missing")d.remove("device_id");
 if(fault=="probe")d["probe"]="8765432187654321";
 if(fault=="probe_missing")d.remove("probe");
 if(fault=="count_missing")d.remove("count");
 if(fault=="negative")d["count"]=-1;
 if(fault=="string")d["count"]="0";
 if(fault=="bool")d["count"]=false;
 if(fault=="fraction")d["count"]=0.5;
 if(fault=="overflow")d["count"]=uint64_t(UINT32_MAX)+1;
 if(fault=="reason")d["reason"]="io";
 if(fault=="reason_missing")d.remove("reason");
 if(fault=="busy")d["ok"]=0;
 if(fault=="ok_missing")d.remove("ok");
 if(fault=="ok_string")d["ok"]="1";
 if(fault=="ok_bool")d["ok"]=true;
 return true;
}
''' + '\n'.join(definition(source, signature) for signature in (
        'static uint32_t sense_image_spool_remaining(',
        'static void sense_image_spool_identity(',
        'static bool sense_image_spool_delete(')) + r'''
static void reset(const char* name){
 scenario=name;now=100;deadline_seen=0;g_image_spool_depth=1;strcpy(g_image_spool_cursor,"retained");
 lease_ok=delete_reply=list_reply=owner_ok=true;
 reads=sends=inventories=leases=lease_acquisitions=0;retry_state={};retry_state.backoff=3;
 foreground_active=voice_recording_active=current_job.active=false;g_media_retry_user_paused=false;
 fault.clear();probe.clear();takeover_after_delete=takeover_after_list=-1;
 delete_duration=20;list_duration=30;
}
static void unchanged(){
 check(!inventories&&retry_state.pending==halo_media_retry::kAllStores&&retry_state.backoff==3,"uncertain inventory preserves all pending state");
 check(g_image_spool_depth==1&&!strcmp(g_image_spool_cursor,"retained"),"uncertain inventory preserves depth and cursor");
 check(!leases&&lease_acquisitions==1,"every exit releases its single lease");
}
int main(){UploadJob job;
 reset("last image");check(sense_image_spool_delete(job),"proven deletion succeeds");
 check(inventories==1&&g_image_spool_depth==0&&!g_image_spool_cursor[0],"fresh zero inventory clears depth and cursor");
 check(retry_state.pending==(halo_media_retry::VoiceSd|halo_media_retry::VoiceFlash)&&retry_state.backoff==3,"other stores and their backoff remain pending");
 check(reads==2&&sends==2&&!leases&&lease_acquisitions==1&&deadline_seen==4100&&now==150,"inventory does not acquire a new lease or reset the four-second budget");
 const std::string first_probe=probe;
 reset("only pending store");retry_state.pending=halo_media_retry::ImageSd;
 check(sense_image_spool_delete(job),"last-store deletion succeeds");
 check(!retry_state.pending&&!retry_state.backoff&&halo_media_retry::interval(retry_state,true)==0,"last proven-empty store cancels media retry");
 check(probe!=first_probe,"next transaction uses a different probe");
 reset("remaining image");fault="remaining";check(sense_image_spool_delete(job),"deleted image stays successful");
 check(inventories==1&&g_image_spool_depth==2&&retry_state.pending==halo_media_retry::kAllStores,"remaining images stay pending");
 check(!strcmp(g_image_spool_cursor,"retained"),"nonempty inventory retains cursor");
 reset("ok zero");fault="ok_empty";check(sense_image_spool_delete(job)&&g_image_spool_depth==0,"successful zero inventory accepts either supported success reason");
 for(const char* f:{"owner","owner_missing","device","device_missing","probe","probe_missing","count_missing","negative","string","bool","fraction","overflow","reason","reason_missing","busy","ok_missing","ok_string","ok_bool"}){
  reset(f);fault=f;check(sense_image_spool_delete(job),"unknown inventory cannot undo the confirmed deletion");unchanged();
  check(reads==2&&sends==2,"invalid inventory is not retried within this operation");
 }
 reset("inventory timeout");list_reply=false;list_duration=3980;
 check(sense_image_spool_delete(job),"lost inventory reply leaves deletion successful");unchanged();
 check(now==deadline_seen,"timeout stays within original deadline");
 for(const char* f:{"delete_refused","delete_missing","delete_string","delete_bool"}){
  reset(f);fault=f;check(!sense_image_spool_delete(job),"unproven delete fails");unchanged();
  check(reads==1&&sends==1,"failed delete never queries or clears inventory");
 }
 reset("delete timeout");delete_reply=false;delete_duration=4000;
 check(!sense_image_spool_delete(job),"missing deletion ACK fails");unchanged();check(reads==1&&sends==1,"no inventory after lost delete ACK");
 reset("lease refused");lease_ok=false;check(!sense_image_spool_delete(job),"unavailable lease fails");unchanged();
 check(!reads&&!sends,"no UART use without lease");
 reset("delete deadline exhausted");delete_duration=4000;
 check(sense_image_spool_delete(job),"delete at deadline remains confirmed");unchanged();check(reads==1&&sends==1,"no optional work without time");
 reset("inventory arrives at deadline");list_duration=3980;
 check(sense_image_spool_delete(job),"late inventory cannot undo delete");unchanged();
 reset("inventory just in time");list_duration=3979;
 check(sense_image_spool_delete(job)&&inventories==1&&now==deadline_seen-1,"fresh inventory can finish inside the original deadline");
 for(int i=0;i<5;++i){
  reset("takeover after delete");takeover_after_delete=i;
  check(sense_image_spool_delete(job),"foreground/ownership change preserves confirmed delete");unchanged();
  check(reads==1&&sends==1,"foreground/ownership change skips optional query");
  reset("takeover during inventory");takeover_after_list=i;
  check(sense_image_spool_delete(job),"late takeover preserves confirmed delete");unchanged();
  check(reads==2&&sends==2,"late takeover does not retry inventory");
 }
 reset("millis rollover");now=UINT32_MAX-1000;
 check(sense_image_spool_delete(job)&&inventories==1&&!leases&&deadline_seen==2999,"four-second deadline remains valid across millis rollover");
 printf("PASS %u actual-function image post-delete inventory checks\n",checks);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-image-post-delete-') as name:
        path = Path(name)
        (path / 'test.cpp').write_text(harness(args.source_root))
        subprocess.run([shutil.which('clang++') or 'c++', '-std=c++17', '-O1',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-Wno-deprecated-declarations', '-I'+str(args.source_root),
                        '-I'+str(args.source_root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
                        str(path / 'test.cpp'), '-o', str(path / 'test')],
                       check=True, timeout=45)
        subprocess.run([str(path / 'test')], check=True, timeout=10)


if __name__ == '__main__':
    main()

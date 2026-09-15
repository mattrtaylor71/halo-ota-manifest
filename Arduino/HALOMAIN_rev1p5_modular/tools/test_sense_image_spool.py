#!/usr/bin/env python3
"""Actual Sense image decode/hash/fetch custody with scripted UART/heap boundaries.

Reuses the actual shared collector/query/voice ownership harness. Production
image helpers execute unchanged; SHA uses CommonCrypto, not a fake success.
No SD, device, backend or physical durability claim.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_voice_uart_ownership import harness as common_harness, definition

def harness(root):
    spool=(root/'Sense_Minimal/sense_image_spool.h').read_text()
    identity=(root/'Sense_Minimal/sense_image_identity.h').read_text()
    voice=(root/'Sense_Minimal/sense_voice.h').read_text()
    common=common_harness(root).replace('int main(){','static void common_controls(){')
    common=common.replace('  int available(){return (int)bytes.size();}',
        '  size_t write(uint8_t){return 1;}\n  int available(){return (int)bytes.size();}')
    funcs='\n'.join(definition(voice,n) for n in ('static uint32_t sense_voice_crc32(', 'static bool sense_voice_request_id_valid(', 'static bool sense_voice_replay_age_ok('))
    funcs+='\n'+'\n'.join(definition(identity,n) for n in ('static bool sense_image_hash(', 'static bool sense_image_envelope_valid(', 'static bool sense_image_owner_matches(', 'static bool sense_image_payload_matches('))
    funcs+='\n'+'\n'.join(definition(spool,n) for n in ('static uint32_t sense_image_spool_remaining(', 'static uint32_t sense_image_spool_step_deadline(', 'static uint32_t sense_image_spool_operation_deadline(', 'static bool sense_image_spool_read(', 'static void sense_image_spool_identity(', 'static void sense_image_spool_meta(', 'static bool sense_image_spool_ready(', 'static uint32_t sense_image_spool_u32(', 'static bool sense_image_spool_committed(', 'static bool sense_image_spool_decode('))
    return common+r'''
#include <CommonCrypto/CommonDigest.h>
#include <ctime>
#include "Sense_Minimal/sense_ops.h"
static time_t epoch=1800000000;
static time_t fake_time(time_t*){return epoch;}
#define time fake_time
static constexpr uint32_t TIME_VALID_MIN_EPOCH=1700000000;
static bool fresh=true,hash_failed=false;
static bool sense_time_has_fresh_sync(){return fresh;}
static const char* owner="owner",*device="device",*TREPO_DEVICE_ID="fallback";
static void load_owner_id_or_default(char* p,size_t n){snprintf(p,n,"%s",owner);}
static void load_runtime_device_id(char* p,size_t n){snprintf(p,n,"%s",device);}
static int mbedtls_sha256(const uint8_t* p,size_t n,uint8_t* out,int is224){
 return hash_failed||is224?-1:(CC_SHA256(p,(CC_LONG)n,out)?0:-1);
}
static constexpr uint32_t IMAGE_SPOOL_TRANSACTION_MS=120000,IMAGE_SPOOL_CLEANUP_MS=2500;
static constexpr uint32_t GUARDIAN_FORCE_SLEEP_MS=240000;
static uint32_t guardian_awake_start_ms=0,g_image_spool_depth=0;
static char g_image_spool_cursor[33]={};
static constexpr unsigned MAX_FRAME_SIZE=600,MAX_CHUNK_SIZE=512;
static constexpr uint8_t MSG_IMG_CHUNK=0x12,MSG_IMG_END=0x13,MSG_IMG_ACK=0x14,MSG_IMG_NACK=0x15;
using SenseImageUartLease=SenseVoiceUartLease;
'''+funcs+r'''
static uint8_t jpeg[]={0xff,0xd8,1,2,3,4,0xff,0xd9};
static UploadJob fixture(){
 UploadJob j={};j.job_id=42;j.image_buf=jpeg;j.image_len=sizeof(jpeg);j.quantity=2;j.created_epoch=epoch;
 strcpy(j.mode,"discard");strcpy(j.expiry_date,"2026-10-01");j.add_to_shopping_list=true;
 strcpy(j.image.owner_id,owner);strcpy(j.image.device_id,device);memset(j.image.request_id,'a',32);
 j.image.crc32=sense_voice_crc32(jpeg,sizeof(jpeg));sense_image_hash(jpeg,sizeof(jpeg),j.image.checksum_sha256);
 j.camera_meta.profile=3;j.camera_meta.flash_enabled=1;j.camera_meta.actual_width=640;j.camera_meta.actual_height=480;j.camera_meta.jpeg_quality=12;
 return j;
}
static void ready(JsonDocument& d,const UploadJob& j){
 d.clear();d["ver"]=1;d["image_schema"]=1;d["type"]="IMAGE_SPOOL_FETCH_READY";d["ok"]=1;
 d["request_id"]=j.image.request_id;d["job_id"]=j.job_id;d["len"]=(uint32_t)j.image_len;d["crc32"]=j.image.crc32;
 JsonDocument meta;sense_image_spool_meta(meta,j);d["meta"].set(meta.as<JsonObject>());
}
struct Frame{uint8_t type;uint16_t seq;std::vector<uint8_t> bytes;};
static std::vector<Frame> incoming;
static bool final_ack=true,cleanup_ok=true,wrong_kind=false;
static unsigned allocations=0,frees=0,markers=0,final_acks=0;
static std::vector<std::string> controls;
static UploadJob stored;
static void put32(std::vector<uint8_t>& p,uint32_t n){for(unsigned i=0;i<4;++i)p.push_back(n>>(8*i));}
class UartOtaProtocol {
 public:bool quiet=false;explicit UartOtaProtocol(SerialPort*){}bool valid(){return true;}
 bool send_frame(uint8_t type,uint16_t seq,const uint8_t*,size_t){
  assert(uart_rx_mutex->held);if(type==MSG_IMG_ACK&&seq==1){++final_acks;return final_ack;}return true;
 }
 bool recv_frame(uint8_t* type,uint16_t* seq,uint8_t* bytes,size_t* n,uint32_t ms){
  assert(uart_rx_mutex->held);if(incoming.empty()){now+=ms;return false;}
  Frame f=incoming.front();incoming.erase(incoming.begin());if(*n<f.bytes.size())return false;
  *type=f.type;*seq=f.seq;*n=f.bytes.size();memcpy(bytes,f.bytes.data(),*n);return true;
 }
};
static constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
static void* heap_caps_malloc(size_t n,int){++allocations;return malloc(n);}
static void recorded_free(void* p){if(p)++frees;free(p);}
static bool sense_lcd_mode_before_begin(){++markers;return true;}
// Scripted LCD boundary: never a backend success. Storage DELETE is forbidden
// in this fetch-only harness, including on failed ACK or cleanup.
static void sense_image_spool_json(JsonDocument& d,const char* type){
 controls.emplace_back(type);JsonDocument r;r["ver"]=1;r["image_schema"]=1;
 if(!strcmp(type,"IMAGE_SPOOL_LIST_REQ")){
  r["type"]="IMAGE_SPOOL_LIST";r["count"]=1;r["request_id"]=stored.image.request_id;r["epoch"]=stored.created_epoch;
 }else if(!strcmp(type,"IMAGE_SPOOL_FETCH")){
  ready(r,stored);if(wrong_kind)r["meta"]["kind"]="voice";
 }else if(!strcmp(type,"IMAGE_XFER_ABORT")){
  if(!cleanup_ok)return;
  r["type"]="IMAGE_XFER_ABORT_ACK";r["request_id"]=d["request_id"];r["job_id"]=d["job_id"];
  r["len"]=d["len"];r["crc32"]=d["crc32"];r["ok"]=1;r["json_ready"]=true;
 }else check(false,"fetch cannot delete or copy the committed SD record");
 std::string wire;serializeJson(r,wire);lcdSerial.bytes+=wire+"\n";
}
'''+definition(spool,'static bool sense_image_spool_cleanup(')+r'''
#define free recorded_free
'''+definition(spool,'static bool sense_image_spool_fetch(')+r'''
#undef free
static void prepare(){
 reset();stored=fixture();controls.clear();allocations=frees=markers=final_acks=0;
 final_ack=cleanup_ok=true;wrong_kind=hash_failed=false;g_image_spool_cursor[0]=0;
 std::vector<uint8_t> proof;put32(proof,stored.image_len);put32(proof,stored.image.crc32);
 proof.insert(proof.end(),stored.image.request_id,stored.image.request_id+32);
 incoming={{MSG_IMG_CHUNK,0,std::vector<uint8_t>(jpeg,jpeg+sizeof(jpeg))},{MSG_IMG_END,1,proof}};
}
int main(){
 common_controls();reset();UploadJob j=fixture();JsonDocument d;ready(d,j);UploadJob decoded={};decoded.is_voice=true;
 check(sense_image_spool_decode(d,decoded)&&!decoded.is_voice&&decoded.from_image_sd&&!decoded.from_persisted,"decoded image retains independent custody and clears voice discriminator");
 check(decoded.quantity==2&&decoded.add_to_shopping_list&&!strcmp(decoded.expiry_date,j.expiry_date)&&decoded.camera_meta.actual_width==640,"image action and camera metadata round trip");
 for(unsigned change=0;change<10;++change){ready(d,j);
  if(change==0)d["meta"]["kind"]="voice";
  if(change==1)d["meta"]["content_type"]="audio/pcm";
  if(change==2)d["meta"]["owner_id"]="other";
  if(change==3)d["meta"]["device_id"]="other";
  if(change==4){d["meta"]["len"]=524289;d["len"]=524289;}
  if(change==5){d["meta"]["len"]=0;d["len"]=0;}
  if(change==6)d["meta"]["request_id"]="wrong";
  if(change==7)d["meta"]["checksum_sha256"]="bad";
  if(change==8)d["meta"]["qty"]=0;
  if(change==9)d["crc32"]=0;
  UploadJob out={};check(!sense_image_spool_decode(d,out)&&!out.image_buf,"wrong kind/owner/size/hash metadata refuses before allocation");}
 check(sense_image_payload_matches(j),"actual JPEG CRC and SHA match frozen envelope");
 jpeg[2]^=1;check(!sense_image_payload_matches(j),"payload change rejected");j.image.crc32=sense_voice_crc32(jpeg,sizeof(jpeg));
 check(!sense_image_payload_matches(j),"matching CRC alone cannot replace original SHA");jpeg[2]^=1;j=fixture();
 hash_failed=true;check(!sense_image_payload_matches(j),"SHA provider error fails closed");hash_failed=false;
 j.is_voice=true;check(!sense_image_payload_matches(j),"voice cannot enter image custody");
 prepare();UploadJob out={};check(sense_image_spool_fetch(out),"actual image fetch accepts complete bytes and bound JSON-ready handoff");
 check(!out.is_voice&&out.from_image_sd&&sense_image_payload_matches(out)&&allocations==1&&frees==0,"successful fetch hands one verified RAM copy to caller and keeps SD");
 check(controls.size()==3&&controls.back()=="IMAGE_XFER_ABORT"&&final_acks==1&&!g_lcd_ota_mode_unconfirmed,"terminal ACK and explicit idle proof are both required");recorded_free(out.image_buf);
 prepare();wrong_kind=true;out={};check(!sense_image_spool_fetch(out)&&allocations==0&&frees==0&&!out.image_buf,"wrong media kind never allocates or frees unrelated data");
 prepare();incoming[0].bytes[2]^=1;out={};check(!sense_image_spool_fetch(out)&&allocations==1&&frees==1&&!out.image_buf,"corrupt fetched bytes discard RAM only and preserve original slot");
 prepare();final_ack=false;out={};check(!sense_image_spool_fetch(out)&&frees==1&&!out.image_buf,"failed final ACK cannot establish completed fetch custody");
 prepare();cleanup_ok=false;out={};check(!sense_image_spool_fetch(out)&&frees==1&&!out.image_buf&&g_lcd_ota_mode_unconfirmed,"missing JSON-ready proof keeps quarantine and original slot");
 check(!uart_rx_mutex->held&&!uart_json_tx_mutex->held&&!g_img_spool_tx_active,"all image exits release only local UART custody");
 printf("PASS %u combined actual-source image/read/voice/query checks\n",checks);
}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1]);a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-image-sense-') as temp:
        cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test';cpp.write_text(harness(a.source_root))
        subprocess.run([shutil.which('clang++') or 'c++','-std=c++17','-O1','-Wno-deprecated-declarations','-I',str(a.source_root),'-I',str(a.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),str(cpp),'-o',str(exe)],check=True,timeout=30)
        subprocess.run([str(exe)],check=True,timeout=15)

if __name__=='__main__':main()

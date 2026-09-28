#!/usr/bin/env python3
"""Run actual presign helpers with the bundled ArduinoJson allocator observed.

Only ArduinoJson's default malloc/free/realloc boundary and synchronous HTTP,
identity, telemetry and clock are doubled. Production functions are extracted
unchanged. No device/network access; this proves lifetime/contract behavior,
not ESP32 heap savings, TLS peaks or allocator placement.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <set>
#include <string>
#include <CommonCrypto/CommonDigest.h>
static std::set<void*> json_live;
static int json_fail_after=-1;
static unsigned json_allocations=0,json_releases=0;
static bool json_allow(){if(json_fail_after==0)return false;if(json_fail_after>0)--json_fail_after;return true;}
static void* json_malloc(size_t n){if(!json_allow())return nullptr;void* p=std::malloc(n);if(p){json_live.insert(p);++json_allocations;}return p;}
static void json_free(void* p){if(p){if(json_live.erase(p)!=1)std::abort();++json_releases;}std::free(p);}
static void* json_realloc(void* p,size_t n){
 if(!json_allow())return nullptr;
 const uintptr_t prior=reinterpret_cast<uintptr_t>(p);
 void* next=std::realloc(p,n);
 if(next){if(prior)json_live.erase(reinterpret_cast<void*>(prior));json_live.insert(next);++json_allocations;}
 return next;
}
// Hook this allocator header only. All real JsonDocument/serialization code is
// unchanged; std::string and TLS doubles do not contaminate JSON accounting.
#define malloc json_malloc
#define free json_free
#define realloc json_realloc
#include <ArduinoJson/Memory/Allocator.hpp>
#undef malloc
#undef free
#undef realloc
#include <ArduinoJson.h>
#include "Sense_Minimal/sense_ops.h"
static int writer_remaining=-1;
struct String:std::string{
 using std::string::string;using std::string::operator=;
 String()=default;String(const std::string& s):std::string(s){}
 bool isEmpty()const{return empty();}
 void remove(size_t offset){erase(offset);}
 size_t write(uint8_t c){return write(&c,1);}
 size_t write(const uint8_t* p,size_t n){
  if(writer_remaining>=0){n=std::min(n,size_t(writer_remaining));writer_remaining-=int(n);}
  append(reinterpret_cast<const char*>(p),n);return n;
 }
};
namespace ArduinoJson {template<>struct Converter<String>{
 static String fromJson(JsonVariantConst v){return v.as<std::string>();}
};}
struct PresignReply{String job_id,put_url,s3_key,content_type,result_url,checksum_sha256_b64,operation_token;int ttl_s=0;bool immutable_image=false,upload_stored=false;uint32_t expected_image_bytes=0;};
using TaskHandle_t=void*;
static TaskHandle_t xTaskGetCurrentTaskHandle(){return reinterpret_cast<void*>(1);}
static constexpr uint32_t MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
static void* heap_caps_calloc(size_t n,size_t s,uint32_t){return std::calloc(n,s);}
static void heap_caps_free(void* p){std::free(p);}
#include "halo_ota_demo/firmware/shared/ScopedTlsMemory.h"
static unsigned checks=0,failures=0,transports=0;
static void check(bool ok,const char* message){++checks;if(!ok){++failures;fprintf(stderr,"FAIL %s\n",message);}}
static bool expired=false,owner_missing=false,owner_changed=false,transport_ok=true,telemetry=false;
static bool malformed_reply=false,oversized_reply=false,missing_reply=false,fail_reply_allocation=false;
static unsigned delivered=0;
static uint32_t g_errlog_last_collect_seq=17;
static OpJob current_job={};
static String last_url,last_body,last_label,error_stage,error_detail,error_text;
static uint32_t last_job=0,last_deadline=0;
static const char* last_api=nullptr;
static const char* last_bearer=nullptr;
static const char* CHECKIN_API_BASE_URL="https://fixture.invalid";
static const char* CHECKIN_PRESIGN_ENDPOINT="/presign";
static const char* API_KEY="fixture-key";
static const char* BEARER_TOKEN="fixture-bearer";
static const UploadJob* transport_job=nullptr;
static constexpr unsigned PRESIGN_RESPONSE_DOC_BYTES=4096;
static bool deadline_expired(uint32_t){return expired;}
static void load_owner_id_or_default(char* b,size_t n){snprintf(b,n,"%s",owner_missing?"":owner_changed?"other-owner":"qa-owner");}
static void load_runtime_device_id(char* b,size_t n){snprintf(b,n,"qa-device");}
static void append_camera_meta_json(JsonDocument& d,const UploadJob::CameraUploadMeta& m){d["camera_meta"]["profile_code"]=m.profile;}
static void log_camera_meta_for_presign(const char*,const UploadJob::CameraUploadMeta*){}
static void diag_note_stage(const char*,int){}
static void diag_record_error(const char* stage,int,const char* detail){error_stage=stage;error_detail=detail;}
static void presign_set_error_text(const char* message){error_text=message;}
static uint32_t sense_errlog_collect_json(char* b,size_t n,int){if(!telemetry)return 0;snprintf(b,n,"[{\"stage\":\"fixture\",\"code\":7}]");return 17;}
static void sense_errlog_mark_delivered(uint32_t seq){check(seq==17,"original telemetry acknowledgment sequence");++delivered;}
static struct {template<class... A>void printf(const char*,A...){}template<class T>void print(const T&){}template<class T>void println(const T&){} }Serial;
static int mbedtls_sha256(const unsigned char* p,size_t n,uint8_t* out,int b){return b||!CC_SHA256(p,(CC_LONG)n,out)?-1:0;}
static void durable_receipt(JsonDocument&,const UploadJob&,bool);
static const String signed_url=String("https://fixture.s3.amazonaws.com/image.jpg?signature=")+String(1700,'x');
static bool http_post_json_with_retries(const char* url,const String& body,int& code,String& response,
 const char* label,const char* api,const char* bearer,uint32_t job,uint32_t deadline){
 ++transports;
 // This is the real caller's next boundary after JSON serialization, before
 // constructing/connecting the nested HTTP/TLS client.
 check(json_live.empty(),"request JSON allocations retired before HTTP/TLS boundary");
 last_url=url;last_body=body;last_label=label;last_job=job;last_deadline=deadline;last_api=api;last_bearer=bearer;
 writer_remaining=-1;json_fail_after=-1;
 void* tls=halo_tls_memory::calloc(1,1024);check(tls!=nullptr,"transport allocation succeeds after request retirement");halo_tls_memory::free(tls);
 code=transport_ok?200:503;
 if(malformed_reply)response="{broken";
 else if(oversized_reply)response=String(6145,'x');
 else if(missing_reply)response="{\"job_id\":\"\",\"put_url\":\"\"}";
 else {JsonDocument r;
  if(!strcmp(label,"IMAGE_PRESIGN")||!strcmp(label,"IMAGE_RECONCILE"))durable_receipt(r,*transport_job,!strcmp(label,"IMAGE_RECONCILE"));
  else {r["job_id"]="cloud-job";r["put_url"]=static_cast<const std::string&>(signed_url);r["s3_key"]="image-key";r["ttl_s"]=321;r["expires_in"]=322;}
  serializeJson(r,response);
 }
 if(fail_reply_allocation)json_fail_after=0;
 return transport_ok;
}
'''

SUFFIX = r'''
static UploadJob fixture(){UploadJob j={};j.job_id=73;strcpy(j.mode,"check-in");j.image_len=120;j.quantity=2;j.camera_meta.profile=4;
 strcpy(j.image.owner_id,"qa-owner");strcpy(j.image.device_id,"qa-device");strcpy(j.image.request_id,"0123456789abcdef0123456789abcdef");memset(j.image.checksum_sha256,'a',64);return j;}
static void durable_receipt(JsonDocument& d,const UploadJob& j,bool stored){char token[65],checksum[45];sense_image_operation_token(j,token);sense_image_checksum_base64(j.image.checksum_sha256,checksum);
 d["upload_protocol"]=1;d["upload_operation"]["version"]=1;d["upload_operation"]["token"]=token;d["upload_operation"]["checksum_sha256"]=j.image.checksum_sha256;d["upload_operation"]["content_length"]=(uint32_t)j.image_len;
 d["job_id"]="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";d["s3_key"]="images/qa-owner/qa-device/2026/09/15/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg";d["content_type"]="image/jpeg";d["upload_stored"]=stored;
 if(!stored){d["expires_in"]=300;d["put_url"]="https://fixture.s3.amazonaws.com/images/qa-owner/qa-device/2026/09/15/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=fixture";d["put_headers"]["Content-Type"]="image/jpeg";d["put_headers"]["If-None-Match"]="*";d["put_headers"]["x-amz-checksum-sha256"]=checksum;}}
static void reset(){check(json_live.empty(),"no JSON remains after previous helper return");expired=owner_missing=owner_changed=telemetry=malformed_reply=oversized_reply=missing_reply=fail_reply_allocation=false;
 transport_ok=true;json_fail_after=writer_remaining=-1;last_body.clear();last_url.clear();last_label.clear();error_stage.clear();error_detail.clear();error_text.clear();transports=delivered=0;json_allocations=json_releases=0;}
static bool legacy(unsigned which,PresignReply& out,const UploadJob::CameraUploadMeta& meta){int code=0;String response;
 if(which==0)return do_presign_request_simple("https://fixture.invalid/simple","dish",out,code,response,9001);
 if(which==1)return do_presign_request(CHECKIN_API_BASE_URL,"/discard","discard","IN","captured-owner","2026-10-01",true,&meta,out,code,response,9001);
 return get_presign_checkin(out,"2026-10-01",0,&meta,9001);
}
static void request_common(){check(transports==1&&last_job==73&&last_deadline==9001,"transport count, job and deadline unchanged");check(json_allocations>0&&json_releases>0&&json_live.empty(),"real ArduinoJson allocated and released its pools");}
int main(){halo_tls_memory::initialize(true);UploadJob j=fixture();current_job.job_id=73;transport_job=&j;PresignReply out;
 for(unsigned which=0;which<3;++which){
  reset();telemetry=true;check(legacy(which,out,j.camera_meta),"legacy helper success");request_common();
  check(out.put_url==signed_url&&out.job_id=="cloud-job"&&out.content_type=="image/jpeg","legacy response owns complete long URL after JSON destruction");
  check(out.ttl_s==(which==2?322:321),"legacy TTL field unchanged");
  check(delivered==(which==2?1u:0u),"only check-in helper acknowledges telemetry here");
  check(last_api==(which==2?nullptr:API_KEY)&&last_bearer==(which==2?nullptr:BEARER_TOKEN),"legacy auth arguments unchanged");
  {JsonDocument d;check(!deserializeJson(d,last_body),"legacy body remains valid JSON");
   check(d["user_id"]=="qa-owner"&&d["device_id"]=="qa-device","legacy current account/device retained");
   check(d["errors"][0]["stage"]=="fixture"&&d["errors"][0]["code"]==7,"piggyback error content retained");
   check(d["type"]==(which==0?"dish":which==1?"discard":"grocery"),"legacy action type retained");
   if(which){check(d["product_expiration"]=="2026-10-01"&&d["camera_meta"]["profile_code"]==4,"expiration and camera metadata retained");
    check(d["owner"]==(which==1?"captured-owner":"qa-owner")&&d["action"]=="IN","captured owner and action retained");}
   if(which==1)check(d["add_to_shopping_list"]==true,"discard shopping option retained");
   if(which==2)check(d["quantity"]==1,"check-in quantity clamp retained");
  }
  for(unsigned fault=0;fault<4;++fault){reset();transport_ok=fault!=0;malformed_reply=fault==1;missing_reply=fault==2;fail_reply_allocation=fault==3;
   check(!legacy(which,out,j.camera_meta),"legacy transport/parse/empty-field/allocation failure rejected");
   check(transports==1&&json_live.empty()&&delivered==0,"legacy failures release JSON without delivery acknowledgment");
   if(fault==1||fault==3)check(error_stage=="presign_parse"&&error_detail=="json_parse","legacy JSON parse diagnostic preserved");
   if(fault==2)check(error_stage=="presign_parse"&&error_detail=="missing_fields","legacy empty fields diagnostic preserved");
  }
  if(which<2){reset();owner_missing=true;check(!legacy(which,out,j.camera_meta)&&transports==0&&error_detail=="owner_missing","legacy missing owner rejected before transport");}
  // Existing legacy semantics intentionally retained: no request-overflow or
  // serialized-length guard. This test does not endorse the malformed request.
  reset();json_fail_after=0;check(legacy(which,out,j.camera_meta)&&last_body=="{}","legacy request allocation failure behavior unchanged");
  reset();writer_remaining=17;check(legacy(which,out,j.camera_meta)&&last_body.length()==17,"legacy partial serialization behavior unchanged");
 }
 for(const char* mode:{"check-in","discard","dish"}){reset();j=fixture();strcpy(j.mode,mode);j.add_to_shopping_list=true;strcpy(j.expiry_date,"2026-10-01");out={};
  check(sense_image_get_presign(j,out,9001),"durable presign success");request_common();
  check(out.immutable_image&&out.expected_image_bytes==120&&!out.put_url.empty(),"durable grant owns reply outside response lifetime");
  {JsonDocument d;check(!deserializeJson(d,last_body),"durable serialized body intact");
   check(d["owner"]=="qa-owner"&&d["device_id"]=="qa-device"&&d["quantity"]==2,"durable frozen capture identity and quantity");
   check(d["product_expiration"]=="2026-10-01"&&d["camera_meta"]["profile_code"]==4,"durable expiration and camera metadata");
   check(d["type"]==(!strcmp(mode,"check-in")?"grocery":mode)&&d["add_to_shopping_list"].as<bool>()==!strcmp(mode,"discard"),"durable mode and shopping option");
   check(d["upload_operation"]["content_length"]==120&&d["upload_operation"]["checksum_sha256"]==j.image.checksum_sha256,"durable immutable checksum/size contract");}
  check(sense_image_reconcile_stored(j,out,9001)&&out.upload_stored&&out.put_url.empty(),"reconciliation retains custody and scopes request JSON");
  {JsonDocument d;check(!deserializeJson(d,last_body)&&d["upload_operation"]["reconcile_only"]==true,"reconciliation payload flag retained");}
 }
 j=fixture();reset();out={};check(sense_image_get_presign(j,out,9001),"prior durable receipt prepared");const auto prior=out;
 for(unsigned fault=0;fault<7;++fault){reset();out=prior;expired=fault==0;owner_changed=fault==1;transport_ok=fault!=2;malformed_reply=fault==3;oversized_reply=fault==4;fail_reply_allocation=fault==5;if(fault==6)json_fail_after=0;
  check(!sense_image_get_presign(j,out,9001),"durable admission/transport/parse/allocation failure rejected");
  check(transports==unsigned(fault>=2&&fault<=5)&&json_live.empty(),"durable failure boundary and cleanup unchanged");
  check(out.put_url==prior.put_url&&out.operation_token==prior.operation_token&&out.job_id==prior.job_id,"durable failure preserves prior owned receipt");
 }
 reset();writer_remaining=17;out={};check(sense_image_get_presign(j,out,9001)&&last_body.length()==17,"durable partial serialization behavior remains unchanged with successful transport double");
 check(json_live.empty(),"all JSON retired at end");printf("%s %u presign lifetime/contract checks (%u failures)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;
}
'''


def harness(root):
    image = (root / 'Sense_Minimal/sense_image_upload.h').read_text()
    image = image[image.index('static bool sense_image_hex('):image.rindex('#endif')]
    legacy = (root / 'Sense_Minimal/sense_presign.h').read_text()
    legacy = legacy[legacy.index('static bool do_presign_request_simple('):legacy.index('// ── Mode-aware presign dispatch')]
    checkin = (root / 'Sense_Minimal/sense_upload_exec.h').read_text()
    checkin = checkin[checkin.index('static bool get_presign_checkin('):checkin.index('// ── S3 PUT upload')]
    return PREFIX + image + legacy + checkin + SUFFIX


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='presign-json-lifetime-') as tmp:
        temp = Path(tmp)
        cpp, exe = temp / 'test.cpp', temp / 'test'
        cpp.write_text(harness(args.source_root))
        for name in ('esp_heap_caps.h', 'freertos/FreeRTOS.h', 'freertos/task.h'):
            stub = temp / 'sdk-stubs' / name
            stub.parent.mkdir(parents=True, exist_ok=True)
            stub.write_text('#pragma once\n')
        subprocess.run([shutil.which('clang++') or 'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I', str(temp / 'sdk-stubs'), '-I', str(args.source_root),
                        '-I', str(args.source_root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
                        str(cpp), '-o', str(exe)], check=True, timeout=30)
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == '__main__':
    main()

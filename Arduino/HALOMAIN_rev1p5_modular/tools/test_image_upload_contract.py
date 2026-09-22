#!/usr/bin/env python3
"""Compile actual image admission/receipt functions with real ArduinoJson.
No HTTP, cloud or device access; SHA256 uses the platform implementation.
"""
from pathlib import Path
import argparse,hashlib,shutil,subprocess,tempfile

def harness(root):
    text=(root/'Sense_Minimal/sense_image_upload.h').read_text()
    functions=text[text.index('static bool sense_image_hex('):text.index('#endif')]
    return r'''
#include <ArduinoJson.h>
#include <CommonCrypto/CommonDigest.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include "Sense_Minimal/sense_ops.h"
struct String:std::string{
 using std::string::string;using std::string::operator=;
 String()=default;String(const std::string& s):std::string(s){}
 bool isEmpty()const{return empty();}
 size_t write(uint8_t c){push_back(char(c));return 1;}
 size_t write(const uint8_t* p,size_t n){append(reinterpret_cast<const char*>(p),n);return n;}
};
struct PresignReply{String job_id,put_url,s3_key,content_type,result_url,checksum_sha256_b64,operation_token;int ttl_s=0;bool immutable_image=false,upload_stored=false;uint32_t expected_image_bytes=0;};
using TaskHandle_t=void*;
static TaskHandle_t task=reinterpret_cast<void*>(1);
static TaskHandle_t xTaskGetCurrentTaskHandle(){return task;}
static constexpr uint32_t MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
static std::set<void*> external_live;
static void* heap_caps_calloc(size_t n,size_t s,uint32_t caps){
 if(caps!=(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))std::abort();
 void* p=std::calloc(n,s);if(p)external_live.insert(p);return p;
}
static void heap_caps_free(void* p){external_live.erase(p);std::free(p);}
#include "halo_ota_demo/firmware/shared/ScopedTlsMemory.h"
static void append_camera_meta_json(JsonDocument& d,const UploadJob::CameraUploadMeta& m){d["camera_meta"]["profile_code"]=m.profile;}
static int mbedtls_sha256(const unsigned char* p,size_t n,uint8_t* out,int b){return b||!CC_SHA256(p,(CC_LONG)n,out)?-1:0;}
static int checks=0,failures=0;
static void check(bool ok,const char* label){++checks;if(!ok){++failures;fprintf(stderr,"FAIL %s\n",label);}}
static bool expired=false,expire_reply=false,transport_ok=true,owner_ok=true;
static unsigned transports=0;
static const UploadJob* transport_job=nullptr;
static String reply_override;
static const char* CHECKIN_API_BASE_URL="https://example.invalid";
static const char* CHECKIN_PRESIGN_ENDPOINT="/presign";
static const char* API_KEY="fixture";
static const char* BEARER_TOKEN="fixture";
static bool deadline_expired(uint32_t){return expired;}
static void load_owner_id_or_default(char* b,size_t n){snprintf(b,n,"%s",owner_ok?"qa-owner":"changed-owner");}
static void load_runtime_device_id(char* b,size_t n){snprintf(b,n,"qa-device");}
static struct {template<class... A>void printf(const char*,A...) {}}Serial;
static void receipt(JsonDocument&,const UploadJob&,bool);
static bool http_post_json_with_retries(const char*,const String&,int& code,String& response,
 const char* label,const char*,const char*,uint32_t,uint32_t){
 ++transports;
 // The actual caller is under test; the synchronous HTTP/TLS boundary is
 // doubled here. Whole PUT separately exercises actual client destruction.
 void* p=halo_tls_memory::calloc(1,1024);
 check(p&&external_live.count(p),"presign/reconcile transport owns allocation scope");
 task=reinterpret_cast<void*>(2);void* other=halo_tls_memory::calloc(1,511);
 check(other&&!external_live.count(other),"concurrent other task uses default allocation");
 halo_tls_memory::free(other);task=reinterpret_cast<void*>(1);
 halo_tls_memory::free(p);code=200;
 if(reply_override.empty()){JsonDocument d;receipt(d,*transport_job,!strcmp(label,"IMAGE_RECONCILE"));serializeJson(d,response);}
 else response=reply_override;
 if(expire_reply)expired=true;
 return transport_ok;
}
'''+functions+r'''
static UploadJob fixture(){UploadJob j={};strcpy(j.mode,"check-in");j.image_len=120;j.quantity=2;
strcpy(j.image.owner_id,"qa-owner");strcpy(j.image.device_id,"qa-device");strcpy(j.image.request_id,"0123456789abcdef0123456789abcdef");memset(j.image.checksum_sha256,'a',64);return j;}
static void receipt(JsonDocument& d,const UploadJob& j,bool stored=false){d.clear();char token[65],checksum[45];sense_image_operation_token(j,token);sense_image_checksum_base64(j.image.checksum_sha256,checksum);
d["upload_protocol"]=1;d["upload_operation"]["version"]=1;d["upload_operation"]["token"]=token;d["upload_operation"]["checksum_sha256"]=j.image.checksum_sha256;d["upload_operation"]["content_length"]=(uint32_t)j.image_len;
d["job_id"]="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";d["s3_key"]="images/qa-owner/qa-device/2026/09/15/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg";d["content_type"]="image/jpeg";d["upload_stored"]=stored;
if(!stored){d["expires_in"]=300;d["put_url"]="https://fixture.s3.amazonaws.com/images/qa-owner/qa-device/2026/09/15/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=fixture";d["put_headers"]["Content-Type"]="image/jpeg";d["put_headers"]["If-None-Match"]="*";d["put_headers"]["x-amz-checksum-sha256"]=checksum;}}
static void closed_scope(){void* p=halo_tls_memory::calloc(1,511);
 check(p&&!external_live.count(p),"transport scope closed on return");halo_tls_memory::free(p);
 check(external_live.empty(),"transport allocations released");}
int main(){halo_tls_memory::initialize(true);UploadJob j=fixture();JsonDocument d;PresignReply out;char token[65];
check(sense_image_operation_token(j,token)&&!strcmp(token,"TOKEN_FIXTURE"),"framed token matches independent SHA256");
check(sense_image_contract_request(j,false,d),"request accepted");check(d["type"]=="grocery"&&d["quantity"]==2,"capture mode and quantity preserved");
check(d["owner"]=="qa-owner"&&d["upload_operation"]["content_length"]==120,"frozen owner and length");
check(sense_image_contract_request(j,true,d)&&d["upload_operation"]["reconcile_only"]==true,"reconciliation retains same operation");
strcpy(j.mode,"discard");j.add_to_shopping_list=true;check(sense_image_contract_request(j,false,d)&&d["type"]=="discard"&&d["add_to_shopping_list"]==true,"discard options retained");
strcpy(j.mode,"dish");check(sense_image_contract_request(j,false,d)&&d["type"]=="dish"&&d["add_to_shopping_list"]==false,"dish mapping");
strcpy(j.mode,"unknown");check(!sense_image_contract_request(j,false,d),"unknown mode rejected");j=fixture();
receipt(d,j);check(sense_image_contract_reply(j,false,200,d,out)&&out.immutable_image&&!out.upload_stored&&out.expected_image_bytes==120,"full immutable grant accepted");
check(out.checksum_sha256_b64=="qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqo=","SHA256 base64 exact");
const PresignReply prior=out;
check(!sense_image_contract_reply(j,false,503,d,out),"HTTP error rejected");
d.remove("upload_operation");check(!sense_image_contract_reply(j,false,200,d,out),"legacy grant rejected");
receipt(d,j);d["upload_operation"]["token"]="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";check(!sense_image_contract_reply(j,false,200,d,out),"another request rejected");
receipt(d,j);d["upload_operation"]["content_length"]="120";check(!sense_image_contract_reply(j,false,200,d,out),"string length rejected");
receipt(d,j);d["upload_operation"]["content_length"]=121;check(!sense_image_contract_reply(j,false,200,d,out),"wrong length rejected");
receipt(d,j);d["upload_operation"]["checksum_sha256"]="bad";check(!sense_image_contract_reply(j,false,200,d,out),"wrong checksum rejected");
receipt(d,j);d["put_headers"].remove("If-None-Match");check(!sense_image_contract_reply(j,false,200,d,out),"unconditional PUT rejected");
receipt(d,j);d["put_headers"]["x-amz-checksum-sha256"]="bad";check(!sense_image_contract_reply(j,false,200,d,out),"wrong signed checksum rejected");
receipt(d,j);d["put_headers"]["unexpected"]="required";check(!sense_image_contract_reply(j,false,200,d,out),"unknown required header rejected");
receipt(d,j);d["expires_in"]=901;check(!sense_image_contract_reply(j,false,200,d,out),"unbounded TTL rejected");
receipt(d,j);d["put_url"]="https://fixture.s3.amazonaws.com.evil.test/images/x?X-Amz-Algorithm=AWS4-HMAC-SHA256";check(!sense_image_contract_reply(j,false,200,d,out),"lookalike S3 hostname rejected");
receipt(d,j);d["s3_key"]="images/another/qa-device/2026/09/15/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg";check(!sense_image_contract_reply(j,false,200,d,out),"other owner key rejected");
receipt(d,j);d["upload_stored"]=true;check(!sense_image_contract_reply(j,false,200,d,out),"stored not accepted on grant path");
receipt(d,j,true);check(sense_image_contract_reply(j,true,200,d,out)&&out.upload_stored&&out.put_url.isEmpty(),"HEAD proof completes exact original object");
receipt(d,j,true);out=PresignReply();check(!sense_image_contract_reply(j,true,200,d,out),"unbound 412 proof rejected");out=prior;
receipt(d,j,true);d["job_id"]="cccccccccccccccccccccccccccccccc";d["s3_key"]="images/qa-owner/qa-device/2026/09/15/cccccccccccccccccccccccccccccccc.jpg";check(!sense_image_contract_reply(j,true,200,d,out),"new cloud job cannot settle old grant");
check(out.job_id==prior.job_id&&!out.upload_stored,"invalid receipt leaves prior grant unchanged");
j=fixture();j.image_len=512*1024;check(sense_image_operation_token(j,token),"maximum image accepted");j.image_len++;check(!sense_image_operation_token(j,token),"oversized image rejected");
j=fixture();memset(j.image.owner_id,'a',sizeof(j.image.owner_id));check(!sense_image_operation_token(j,token),"unterminated owner rejected");
j=fixture();j.image.owner_id[0]='/';check(!sense_image_operation_token(j,token),"path owner rejected");
j=fixture();transport_job=&j;out={};
check(sense_image_get_presign(j,out,1234),"actual scoped presign succeeds");closed_scope();
const PresignReply saved=out;
check(out.put_url.find("X-Amz-Signature=fixture")!=String::npos&&out.job_id=="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "reply owns URL and receipt after JSON/transport scope ends");
check(sense_image_reconcile_stored(j,out,1234)&&out.upload_stored&&out.put_url.empty(),"actual scoped reconciliation retains exact custody proof");closed_scope();
for(unsigned fault=0;fault<6;++fault){
 out=saved;expired=fault==0;owner_ok=fault!=1;transport_ok=fault!=2;expire_reply=fault==3;
 reply_override=fault==4?"malformed":fault==5?String(6145,'x'):String();
 const unsigned before=transports;
 check(!sense_image_get_presign(j,out,1234),"presign fault rejected");closed_scope();
 check(transports==before+(fault>=2),"invalid admission avoids transport");
 check(out.put_url==saved.put_url&&out.job_id==saved.job_id,"failed presign preserves prior owned reply");
}
std::printf("%s %d image contract checks (%d failures)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1]);a=p.parse_args()
    digest=hashlib.sha256(b'halo-image-v1:8:qa-owner9:qa-device32:0123456789abcdef0123456789abcdef').hexdigest()
    source=harness(a.source_root).replace('TOKEN_FIXTURE',digest)
    with tempfile.TemporaryDirectory(prefix='image-contract-') as tmp:
        cpp=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';cpp.write_text(source)
        for name in ('esp_heap_caps.h','freertos/FreeRTOS.h','freertos/task.h'):
            stub=Path(tmp)/'sdk-stubs'/name;stub.parent.mkdir(parents=True,exist_ok=True);stub.write_text('#pragma once\n')
        subprocess.run([shutil.which('clang++') or 'g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-deprecated-declarations','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(Path(tmp)/'sdk-stubs'),'-I',str(a.source_root),'-I',str(a.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),str(cpp),'-o',str(exe)],check=True,timeout=30)
        subprocess.run([str(exe)],check=True,timeout=10)
if __name__=='__main__':main()

#!/usr/bin/env python3
"""Compile actual image admission/receipt functions with real ArduinoJson.
No HTTP, cloud or device access; SHA256 uses the platform implementation.
"""
from pathlib import Path
import argparse,hashlib,shutil,subprocess,tempfile

def harness(root):
    text=(root/'Sense_Minimal/sense_image_upload.h').read_text()
    functions=text[text.index('static bool sense_image_hex('):text.index('static bool sense_image_presign_request(')]
    return r'''
#include <ArduinoJson.h>
#include <CommonCrypto/CommonDigest.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include "Sense_Minimal/sense_ops.h"
struct String:std::string{using std::string::string;using std::string::operator=;bool isEmpty()const{return empty();}};
struct PresignReply{String job_id,put_url,s3_key,content_type,result_url,checksum_sha256_b64,operation_token;int ttl_s=0;bool immutable_image=false,upload_stored=false;uint32_t expected_image_bytes=0;};
static void append_camera_meta_json(JsonDocument& d,const UploadJob::CameraUploadMeta& m){d["camera_meta"]["profile_code"]=m.profile;}
static int mbedtls_sha256(const unsigned char* p,size_t n,uint8_t* out,int b){return b||!CC_SHA256(p,(CC_LONG)n,out)?-1:0;}
static int checks=0,failures=0;
static void check(bool ok,const char* label){++checks;if(!ok){++failures;fprintf(stderr,"FAIL %s\n",label);}}
'''+functions+r'''
static UploadJob fixture(){UploadJob j={};strcpy(j.mode,"check-in");j.image_len=120;j.quantity=2;
strcpy(j.image.owner_id,"qa-owner");strcpy(j.image.device_id,"qa-device");strcpy(j.image.request_id,"0123456789abcdef0123456789abcdef");memset(j.image.checksum_sha256,'a',64);return j;}
static void receipt(JsonDocument& d,const UploadJob& j,bool stored=false){d.clear();char token[65],checksum[45];sense_image_operation_token(j,token);sense_image_checksum_base64(j.image.checksum_sha256,checksum);
d["upload_protocol"]=1;d["upload_operation"]["version"]=1;d["upload_operation"]["token"]=token;d["upload_operation"]["checksum_sha256"]=j.image.checksum_sha256;d["upload_operation"]["content_length"]=(uint32_t)j.image_len;
d["job_id"]="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";d["s3_key"]="images/qa-owner/qa-device/2026/09/15/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg";d["content_type"]="image/jpeg";d["upload_stored"]=stored;
if(!stored){d["expires_in"]=300;d["put_url"]="https://fixture.s3.amazonaws.com/images/qa-owner/qa-device/2026/09/15/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.jpg?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Signature=fixture";d["put_headers"]["Content-Type"]="image/jpeg";d["put_headers"]["If-None-Match"]="*";d["put_headers"]["x-amz-checksum-sha256"]=checksum;}}
int main(){UploadJob j=fixture();JsonDocument d;PresignReply out;char token[65];
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
std::printf("%s %d image contract checks (%d failures)\n",failures?"FAIL":"PASS",checks,failures);return failures?1:0;}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1]);a=p.parse_args()
    digest=hashlib.sha256(b'halo-image-v1:8:qa-owner9:qa-device32:0123456789abcdef0123456789abcdef').hexdigest()
    source=harness(a.source_root).replace('TOKEN_FIXTURE',digest)
    with tempfile.TemporaryDirectory(prefix='image-contract-') as tmp:
        cpp=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';cpp.write_text(source)
        subprocess.run([shutil.which('clang++') or 'g++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-deprecated-declarations','-I',str(a.source_root),'-I',str(a.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),str(cpp),'-o',str(exe)],check=True,timeout=30)
        subprocess.run([str(exe)],check=True,timeout=10)
if __name__=='__main__':main()

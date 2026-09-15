#!/usr/bin/env python3
"""Exercise actual voice identity, receipt and LCD metadata functions on host.

Only clock/device identity and SHA implementation are replaced by host boundaries.
ArduinoJson and both board parsers are production code; no network or device I/O.
"""
import argparse
from pathlib import Path
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
    voice = (root / 'Sense_Minimal/sense_voice.h').read_text()
    spool = (root / 'Sense_Minimal/sense_voice_spool.h').read_text()
    lcd = (root / 'LCD_Minimal/lcd_voice_spool.h').read_text()
    functions = [definition(voice, signature) for signature in (
        'static uint32_t sense_voice_crc32(',
        'static bool sense_voice_request_id_valid(',
        'static bool sense_voice_envelope_valid(',
        'static bool sense_voice_replay_age_ok(',
        'static bool sense_voice_owner_matches(',
        'static bool sense_voice_backend_ack(')]
    functions += [definition(spool, signature) for signature in (
        'static bool sense_voice_spool_ready(',
        'static uint32_t sense_voice_spool_u32(',
        'static bool sense_voice_spool_committed(',
        'static bool sense_voice_spool_decode(')]
    functions += [definition(lcd, signature) for signature in (
        'static bool lcd_voice_copy(',
        'static bool lcd_voice_u32(',
        'static bool lcd_voice_parse_meta(',
        'static void lcd_voice_put_meta(',
        'static void lcd_voice_echo(')]
    return r'''
#include <ArduinoJson.h>
#include <CommonCrypto/CommonDigest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>
#include "Sense_Minimal/sense_ops.h"
#include "halo_common/VoiceSpoolStore.h"
static int checks=0,failures=0;
static void check(bool ok,const char* name){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",name);}}
static time_t current_epoch=1800000000;
static bool fresh=true;
static time_t fake_time(time_t*){return current_epoch;}
#define time fake_time
static const uint32_t TIME_VALID_MIN_EPOCH=1700000000;
static bool sense_time_has_fresh_sync(){return fresh;}
static const char* current_owner="qa-owner";
static const char* current_device="qa-device";
static const char* TREPO_DEVICE_ID="qa-fallback-device";
static void load_owner_id_or_default(char* p,size_t n){std::snprintf(p,n,"%s",current_owner);}
static void load_runtime_device_id(char* p,size_t n){std::snprintf(p,n,"%s",current_device);}
static bool hash_fail=false;
static int mbedtls_sha256(const unsigned char* data,size_t size,uint8_t* out,int is224){
  if(hash_fail||is224)return -1;
  return CC_SHA256(data,static_cast<CC_LONG>(size),out)?0:-1;
}
static const uint8_t MSG_IMG_ACK=0x44,MSG_IMG_NACK=0x45;
''' + '\n'.join(functions) + r'''
static UploadJob fixture(){
  UploadJob j={};j.is_voice=true;j.job_id=42;j.image_len=8;j.created_epoch=current_epoch;
  std::strcpy(j.mode,"voice");std::strcpy(j.voice.owner_id,"qa-owner");
  std::strcpy(j.voice.device_id,"qa-device");std::strcpy(j.voice.session_id,"qa-session-1");
  std::strcpy(j.voice.request_id,"0123456789abcdef0123456789abcdef");
  j.voice.crc32=0x12345678;return j;
}
static void receipt(JsonDocument& d){
  d.clear();d["accepted"]=true;d["async"]=true;d["duplicate"]=false;
  // Independent Python hashlib fixture for request:owner|device|session|request.
  d["jobId"]="HASH_FIXTURE";
}
static void ready(JsonDocument& d,const UploadJob& j){
  d.clear();d["ok"]=1;d["request_id"]=j.voice.request_id;d["job_id"]=j.job_id;
  d["len"]=(uint32_t)j.image_len;d["crc32"]=j.voice.crc32;
}
static void metadata(JsonDocument& d,const UploadJob& j){
  d.clear();d["voice_schema"]=1;d["kind"]="voice";d["pcm_fmt"]="s16le_mono";d["rate"]=16000;
  d["owner_id"]=j.voice.owner_id;d["device_id"]=j.voice.device_id;d["session_id"]=j.voice.session_id;
  d["request_id"]=j.voice.request_id;d["job_id"]=j.job_id;d["len"]=(uint32_t)j.image_len;
  d["crc32"]=j.voice.crc32;d["epoch"]=j.created_epoch;d["retries"]=j.retries;
}
int main(){
  UploadJob j=fixture();JsonDocument d;
  check(sense_voice_crc32((const uint8_t*)"123456789",9)==0xcbf43926,"known independent IEEE CRC");
  check(sense_voice_envelope_valid(j)&&sense_voice_owner_matches(j),"valid owned recording eligible");
  check(!sense_voice_request_id_valid(nullptr)&&!sense_voice_request_id_valid("")&&
        !sense_voice_request_id_valid("../../abc"),"missing/traversal identity rejected");
  char nozero[33];std::memset(nozero,'a',sizeof(nozero));
  check(!sense_voice_request_id_valid(nozero),"bounded request validation");
  for(int mutation=0;mutation<7;++mutation){
    UploadJob bad=j;
    if(mutation==0)bad.is_voice=false;
    if(mutation==1)bad.image_len=0;
    if(mutation==2)bad.image_len=512*1024+2;
    if(mutation==3)bad.image_len=3;
    if(mutation==4)std::memset(bad.voice.owner_id,'a',sizeof(bad.voice.owner_id));
    if(mutation==5)bad.voice.session_id[0]=0;
    if(mutation==6)bad.voice.request_id[32]='a';
    check(!sense_voice_envelope_valid(bad),"invalid envelope blocked");
  }
  current_owner="new-owner";check(!sense_voice_owner_matches(j),"reprovisioned owner cannot replay old recording");
  current_owner="qa-owner";current_device="other-device";
  check(!sense_voice_owner_matches(j),"different device cannot replay recording");current_device="qa-device";
  check(sense_voice_replay_age_ok(j),"fresh recording within retry horizon");
  j.created_epoch=current_epoch-7*86400;check(sense_voice_replay_age_ok(j),"seven day boundary accepted");
  --j.created_epoch;check(!sense_voice_replay_age_ok(j),"expired recording held");
  j.created_epoch=current_epoch+1;check(!sense_voice_replay_age_ok(j),"future timestamp held");
  j.created_epoch=0;check(!sense_voice_replay_age_ok(j),"unknown timestamp held");
  j=fixture();fresh=false;check(!sense_voice_replay_age_ok(j),"unsynchronized clock cannot replay");fresh=true;
  receipt(d);check(sense_voice_backend_ack(j,202,d),"exact async receipt accepted");
  for(int code:{-1,200,201,204,400,500})check(!sense_voice_backend_ack(j,code,d),"generic HTTP success/error cannot delete recording");
  for(const char* field:{"accepted","async","duplicate","jobId"}){
    receipt(d);d.remove(field);check(!sense_voice_backend_ack(j,202,d),"missing receipt field rejected");
  }
  for(const char* field:{"accepted","async","duplicate"}){
    receipt(d);d[field]=1;check(!sense_voice_backend_ack(j,202,d),"receipt booleans require exact type");
  }
  for(const char* state:{"accepted","enqueued","processing","completed"}){
    receipt(d);d["duplicate"]=true;d["status"]=state;
    check(sense_voice_backend_ack(j,202,d),"durable duplicate receipt accepted");
  }
  for(const char* state:{"failed","ambiguous","", "unknown"}){
    receipt(d);d["duplicate"]=true;d["status"]=state;
    check(!sense_voice_backend_ack(j,202,d),"unconfirmed/failed duplicate retains recording");
  }
  receipt(d);d["jobId"]="another-job";check(!sense_voice_backend_ack(j,202,d),"wrong backend identity rejected");
  receipt(d);hash_fail=true;check(!sense_voice_backend_ack(j,202,d),"hash error retains recording");hash_fail=false;
  for(int mutation=0;mutation<4;++mutation){
    UploadJob bad=j;if(mutation==0)bad.voice.owner_id[0]='z';if(mutation==1)bad.voice.device_id[0]='z';
    if(mutation==2)bad.voice.session_id[0]='z';if(mutation==3)bad.voice.request_id[0]='a';
    check(!sense_voice_backend_ack(bad,202,d),"every frozen identity component binds receipt");
  }
  ready(d,j);check(sense_voice_spool_ready(d,j),"correlated storage ready accepted");
  for(const char* field:{"ok","job_id","len","crc32","request_id"}){
    ready(d,j);d.remove(field);check(!sense_voice_spool_ready(d,j),"missing storage proof rejected");
  }
  uint8_t proof[40]={8,0,0,0,0x78,0x56,0x34,0x12};
  std::memcpy(proof+8,j.voice.request_id,32);
  check(sense_voice_spool_committed(MSG_IMG_ACK,2,2,proof,40,j),"matching END length CRC and request prove storage");
  check(!sense_voice_spool_committed(MSG_IMG_ACK,1,2,proof,40,j),"stale chunk acknowledgement is not commit");
  check(!sense_voice_spool_committed(MSG_IMG_NACK,2,2,proof,40,j),"negative END cannot commit");
  check(!sense_voice_spool_committed(MSG_IMG_ACK,2,2,proof,0,j),"empty chunk ACK cannot commit");
  check(!sense_voice_spool_committed(MSG_IMG_ACK,2,2,proof,8,j),"legacy proof without request cannot commit");
  proof[0]=6;check(!sense_voice_spool_committed(MSG_IMG_ACK,2,2,proof,40,j),"wrong committed length rejected");
  proof[0]=8;proof[4]^=1;check(!sense_voice_spool_committed(MSG_IMG_ACK,2,2,proof,40,j),"wrong committed CRC rejected");
  proof[4]^=1;proof[8]^=1;check(!sense_voice_spool_committed(MSG_IMG_ACK,2,2,proof,40,j),"old identical audio for another request cannot commit");
  metadata(d,j);halo_voice::Meta m;
  check(lcd_voice_parse_meta(d.as<JsonObjectConst>(),&m),"LCD accepts Sense typed metadata");
  JsonDocument reply;reply["ok"]=1;lcd_voice_echo(reply,m);
  lcd_voice_put_meta(reply["meta"].to<JsonObject>(),m);UploadJob restored={};
  check(sense_voice_spool_decode(reply,restored),"Sense accepts full LCD fetch metadata");
  check(restored.from_voice_sd&&restored.job_id==j.job_id&&restored.created_epoch==j.created_epoch&&
        !std::strcmp(restored.voice.session_id,j.voice.session_id)&&!std::strcmp(restored.voice.request_id,j.voice.request_id),
        "cross-board metadata roundtrip preserves retry identity");
  current_owner="new-owner";check(!sense_voice_spool_decode(reply,restored),"LCD replay cannot change account binding");current_owner="qa-owner";
  for(const char* field:{"voice_schema","rate","job_id","len","crc32","epoch","retries","owner_id","device_id","session_id","request_id"}){
    metadata(d,j);d.remove(field);check(!lcd_voice_parse_meta(d.as<JsonObjectConst>(),&m),"LCD rejects missing voice metadata");
  }
  metadata(d,j);d["kind"]="photo";check(!lcd_voice_parse_meta(d.as<JsonObjectConst>(),&m),"photo cannot enter voice queue");
  metadata(d,j);d["pcm_fmt"]="jpeg";check(!lcd_voice_parse_meta(d.as<JsonObjectConst>(),&m),"incorrect audio format rejected");
  metadata(d,j);d["rate"]=48000;check(!lcd_voice_parse_meta(d.as<JsonObjectConst>(),&m),"unsupported sample rate rejected");
  metadata(d,j);d["owner_id"]=std::string(64,'a');check(!lcd_voice_parse_meta(d.as<JsonObjectConst>(),&m),"overlong owner rejected without truncation");
  metadata(d,j);d["request_id"]="../../evil";check(!lcd_voice_parse_meta(d.as<JsonObjectConst>(),&m),"path traversal rejected at wire boundary");
  std::memset(j.voice.owner_id,'o',63);j.voice.owner_id[63]=0;
  std::memset(j.voice.device_id,'d',31);j.voice.device_id[31]=0;
  std::memset(j.voice.session_id,'s',95);j.voice.session_id[95]=0;
  current_owner=j.voice.owner_id;current_device=j.voice.device_id;
  metadata(d,j);check(lcd_voice_parse_meta(d.as<JsonObjectConst>(),&m),"maximum supported identity fields accepted");
  StaticJsonDocument<1024> full;
  full["ver"]=1;full["voice_schema"]=1;full["type"]="VOICE_SPOOL_FETCH_READY";
  full["msg_id"]=UINT32_MAX;full["ts"]=UINT32_MAX;full["ok"]=1;
  lcd_voice_echo(full,m);lcd_voice_put_meta(full["meta"].to<JsonObject>(),m);
  std::string wire;serializeJson(full,wire);
  check(!full.overflowed()&&wire.size()<1024,"maximum fetch metadata fits bounded private wire reader");
  StaticJsonDocument<1024> reparsed;
  check(!deserializeJson(reparsed,wire)&&!reparsed.overflowed()&&sense_voice_spool_decode(reparsed,restored),
        "maximum metadata roundtrips actual ArduinoJson and board parsers");
  std::printf("%s %d actual-source voice checks (%d failures)\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def main():
    import hashlib
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--arduino-json', type=Path)
    args = parser.parse_args()
    if args.arduino_json is None:
        args.arduino_json = args.source_root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler or not (args.arduino_json / 'ArduinoJson.h').is_file():
        parser.error('C++ compiler and canonical ArduinoJson required')
    source = harness(args.source_root)
    digest = hashlib.sha256(b'request:qa-owner|qa-device|qa-session-1|0123456789abcdef0123456789abcdef').hexdigest()
    source = source.replace('HASH_FIXTURE', digest)
    with tempfile.TemporaryDirectory(prefix='halo-voice-receipts-') as directory:
        cpp = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        cpp.write_text(source)
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-deprecated-declarations',
                        '-I', str(args.source_root), '-I', str(args.arduino_json),
                        str(cpp), '-o', str(binary)], check=True, timeout=30)
        result = subprocess.run([str(binary)], timeout=10)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Host-only power-header coverage and real bounded writer fault tests.

Compiles the production formatter/adapter and actual write_request function.
TLS writes are doubles; this proves payload preservation, framing, size guards
and deadline behavior, not SDK/hardware latency or backend retention.
"""
import argparse
import hashlib
import json
import resource
from pathlib import Path
import shutil
import subprocess

from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]
SHARED = "halo_ota_demo/firmware/shared/"
FILES = [SHARED + name for name in (
    "SystemPower.h", "SystemPowerTransport.h", "SenseBoundedPost.h",
    "ProvisioningClaimTransport.h", "DiagnosticAdmissionAuth.h",
)] + ["Sense_Minimal/" + name for name in (
    "sense_upload.h", "sense_image_upload.h", "sense_voice.h", "sense_list.h",
    "sense_upload_exec.h", "sense_image_http.h", "sense_ota_lcd.h",
)] + ["halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino"]


def coverage(root):
    paths = [
        ("Sense_Minimal/sense_upload.h", "static bool http_post_json_with_retries(", "http.POST(body)"),
        ("Sense_Minimal/sense_voice.h", "static bool voice_upload_and_parse(", "http.POST((uint8_t*)audio_buf, audio_size)"),
        ("Sense_Minimal/sense_list.h", "static ListRequestResult delete_item_from_api(", "http.POST(request_body)"),
        ("Sense_Minimal/sense_list.h", "static ListRequestResult fetch_shopping_list_from_api(", "req_http.POST(request_body)"),
        (SHARED + "ProvisioningClaimTransport.h", "inline Result transport(", "http.POST(String(request.body))"),
    ]
    for path, signature, post in paths:
        body = definition((root / path).read_text(), signature)
        assert body.count("halo_power_transport::add_header(") == 1, path
        assert body.index("halo_power_transport::add_header(") < body.index(post), path
    image = (root / "Sense_Minimal/sense_image_upload.h").read_text()
    builder = definition(image, "static bool sense_image_contract_request(")
    assert "system_power" not in builder and "halo_power_transport" not in builder
    assert "measureJson(doc) <= 2048" in builder
    assert "http_post_json_with_retries(" in definition(image, "static bool sense_image_presign_request(")
    # The adapter cannot become a signed-object or firmware-download header.
    for name in ("sense_upload_exec.h", "sense_image_http.h", "sense_ota_lcd.h"):
        assert "halo_power_transport" not in (root / "Sense_Minimal" / name).read_text()
    claim = (root / SHARED / "ProvisioningClaimJob.h").read_text()
    assert "char body[256]" in claim
    report = definition((root / FILES[-1]).read_text(), "static bool ota_report_build_payload(")
    assert "sense_power_append(payload)" in report
    auth = (root / SHARED / "DiagnosticAdmissionAuth.h").read_text()
    assert "system_power" not in auth and "halo_power_transport" not in auth
    return len(paths) + 7


PREFIX = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include "halo_ota_demo/firmware/shared/SystemPowerTransport.h"
static unsigned mode=0, hook_calls=0;
#ifndef WITHOUT_HOOK
extern "C" bool halo_system_power_json(char* out,size_t capacity) {
 ++hook_calls;
 if(mode==1) return false;
 if(mode==2) {memset(out,'x',capacity);return true;}
 if(mode==3) {snprintf(out,capacity,"{\"x\":1}\r\nInjected: true");return true;}
 if(mode==4) {snprintf(out,capacity,"{\"x\":1");return true;}
 if(mode==5) {
   const std::string longest="{\"pad\":\""+std::string(501,'x')+"\"}";
   assert(longest.size()==511 && capacity==512);
   memcpy(out,longest.c_str(),512);return true;
 }
 halo_power::View v;v.received=true;
 v.sample.status=halo_power::Status::Ok;v.sample.boot_id=7;v.sample.sequence=hook_calls;
 v.sample.samples=32;v.sample.raw_min=2000;v.sample.raw_max=2020;
 v.sample.system_supply_mv=mode==6?3700:3800;v.age_ms=mode==7?6000:0;
 return halo_power::json(v,out,capacity);
}
#endif
struct Http {
 std::map<std::string,std::string> headers;
 void addHeader(const char* name,const char* value){headers[name]=value;}
};
static uint32_t now_ms=10;
static uint32_t millis(){return now_ms;}
static void delay(unsigned n){now_ms+=n;}
struct esp_tls_t {};
static constexpr int ESP_TLS_ERR_SSL_WANT_READ=-1,ESP_TLS_ERR_SSL_WANT_WRITE=-2;
static std::string wire;
static unsigned write_calls=0, cancel_at=0;
static bool slow_writes=false, want_once=false;
static int esp_tls_conn_write(esp_tls_t*,const void* data,size_t n){
 ++write_calls;
 if(want_once){want_once=false;return ESP_TLS_ERR_SSL_WANT_WRITE;}
 if(slow_writes)now_ms+=100;
 n=std::min(n,size_t(13));wire.append(static_cast<const char*>(data),n);return int(n);
}
namespace sense_post {
static constexpr size_t CHUNK=256;
enum class Result {Received,Invalid,Deadline,Write};
enum class Stage {Write};
struct Request {const char*url;const char*device;const char*owner;const char*request;const uint8_t*payload;size_t length;bool require_body=true;const char*b1_key_id=nullptr;const char*b1_signature=nullptr;};
struct Lease {void*arg;bool(*live)(void*);uint32_t deadline_ms,io_timeout_ms,started_ms;};
struct Response {size_t bytes_sent=0;Stage stage=Stage::Write;};
struct Endpoint {char host[192]{},authority[208]{},path[256]{};};
static bool live(const Lease& lease,uint32_t deadline){return lease.live(lease.arg)&&int32_t(deadline-now_ms)>0;}
'''

SUFFIX = r'''
}
static bool lease_live(void*){return !cancel_at||write_calls<cancel_at;}
static std::string body_of(const std::string& request){
 const size_t end=request.find("\r\n\r\n");assert(end!=std::string::npos);return request.substr(end+4);
}
static std::string send(const std::string& body,bool b1=false){
 wire.clear();write_calls=0;now_ms=10;want_once=true;
 sense_post::Request q{"https://example.invalid/api","halo-1234-5678","owner","frozen-request",
   reinterpret_cast<const uint8_t*>(body.data()),body.size(),true,b1?"key-id":nullptr,b1?"unchanged-signature":nullptr};
 sense_post::Endpoint ep;strcpy(ep.authority,"example.invalid");strcpy(ep.path,"/api");
 sense_post::Lease lease{nullptr,lease_live,5000,2000,10};sense_post::Response response;
 const auto result=sense_post::write_request(nullptr,q,ep,lease,2000,response);
 // Cancellation can occur immediately after an SDK write, before its byte
 // counter is committed. Preserve that existing accounting behavior.
 if(cancel_at||slow_writes){assert(result==sense_post::Result::Deadline);assert(response.bytes_sent<=wire.size());return wire;}
 assert(response.bytes_sent==wire.size());
 assert(result==sense_post::Result::Received);
 assert(body_of(wire)==body);
 assert(wire.find("Content-Length: "+std::to_string(body.size())+"\r\n")!=std::string::npos);
 const std::string key="X-Halo-System-Power: ";
 const size_t begin=wire.find(key);assert(begin!=std::string::npos);
 assert(wire.find(key,begin+1)==std::string::npos);
 if(b1)assert(wire.find("X-Halo-B1-Signature: unchanged-signature\r\n")!=std::string::npos);
 return wire;
}
int main(){
 static_assert(halo_power_transport::JSON_BYTES==512,"bounded header contract");
 char unknown[512];assert(halo_power::json(halo_power::View{},unknown,sizeof unknown));
 char out[512];Http http;
#ifdef WITHOUT_HOOK
 assert(halo_power_transport::snapshot(out,sizeof out)==strlen(unknown));assert(!strcmp(out,unknown));
 assert(halo_power_transport::add_header(http));assert(http.headers.at(halo_power_transport::HEADER_NAME)==unknown);
#else
 assert(halo_power_transport::add_header(http));assert(http.headers.at(halo_power_transport::HEADER_NAME).find("\"system_supply_mv\":3800")!=std::string::npos);
 for(mode=1;mode<=4;++mode){assert(halo_power_transport::snapshot(out,sizeof out));assert(!strcmp(out,unknown));}
 mode=5;assert(halo_power_transport::snapshot(out,sizeof out)==511);send("{\"bounded\":true}");
 mode=7;assert(halo_power_transport::snapshot(out,sizeof out));assert(strstr(out,"\"fresh\":false"));assert(strstr(out,"\"system_supply_mv\":3800"));
#endif
 char tiny[4]={'x','x','x','x'};mode=1;assert(!halo_power_transport::snapshot(tiny,sizeof tiny));assert(tiny[0]==0);
 assert(!halo_power_transport::snapshot(nullptr,512));
 const std::string original="{\"upload_operation\":{\"version\":1,\"token\":\"fixed\"},\"camera_meta\":{\"flash_enabled\":true}}";
 mode=0;const auto first=send(original,true);mode=6;const auto replay=send(original,true);
 assert(body_of(first)==body_of(replay));
#ifndef WITHOUT_HOOK
 assert(first!=replay); // A fresh reading must not change the immutable body.
#endif
 send(std::string(8192,'x'));
 cancel_at=2;send(original);cancel_at=0;slow_writes=true;send(original);slow_writes=false;
 puts("PASS power header: fallback, safety, max size, partial writes, immutable replay body, deadlines");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    compiler = shutil.which("clang++") or shutil.which("g++")
    assert compiler
    covered = coverage(args.source_root)
    actual = definition((args.source_root / SHARED / "SenseBoundedPost.h").read_text(),
                        "static __attribute__((noinline)) Result write_request(")
    code = PREFIX + actual + SUFFIX
    (args.out / "test.cpp").write_text(code)
    results = []
    for absent in (False, True):
        name = "missing-hook" if absent else "with-hook"
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-function",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(args.source_root)]
        if absent:
            command.append("-DWITHOUT_HOOK")
        else:
            command.append("-DHALO_SYSTEM_POWER_HAS_HOOK")
        command += [str(args.out / "test.cpp"), "-o", str(args.out / name)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=45)
        (args.out / (name + "-compile.log")).write_text(built.stdout + built.stderr)
        ran = subprocess.run([str(args.out / name)], capture_output=True, text=True, timeout=15,
                             preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0))) if not built.returncode else None
        output = (ran.stdout + ran.stderr) if ran else built.stderr
        (args.out / (name + "-run.log")).write_text(output)
        print(output)
        results.append(bool(ran and not ran.returncode))
    result = {"status": "PASS" if all(results) else "FAIL", "coverage_checks": covered,
              "scope": "HOST_ONLY_NO_NETWORK_OR_BACKEND_RETENTION_CLAIM", "hardware_commands": 0,
              "source_files": {p: hashlib.sha256((args.source_root / p).read_bytes()).hexdigest() for p in FILES}}
    (args.out / "RESULT.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

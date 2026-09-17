#!/usr/bin/env python3
"""Run the actual crypto diagnostic with native mbedTLS and boundary faults.

No device, serial, radio, credentials, allocator override or firmware build.
Native crypto validates public vectors and failure reporting, not ESP hardware.
The actual owner request/service functions also execute against state doubles.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]
HEADER = "halo_ota_demo/firmware/shared/ProvisioningCryptoDiagnostic.h"
WRAPPER = "halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino"
SENSE = "Sense_Minimal/Sense_Minimal.ino"

PREFIX = r'''
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
static unsigned checks=0,failures=0,md_frees=0,crypto_calls=0;
static int fault=0,update_calls=0;
static void check(bool ok,const char* message) {
  ++checks; if(!ok){++failures; printf("FAIL %s\n",message);}
}
static constexpr int injected_error=-12345;
static const mbedtls_md_info_t* test_md_info(mbedtls_md_type_t type) {
  ++crypto_calls; return fault==1?nullptr:mbedtls_md_info_from_type(type);
}
static int setup(mbedtls_md_context_t* c,const mbedtls_md_info_t* i,int h) {
  const int rc=mbedtls_md_setup(c,i,h); return fault==2?injected_error:rc;
}
static int starts(mbedtls_md_context_t* c,const unsigned char* k,size_t n) {
  return fault==3?injected_error:mbedtls_md_hmac_starts(c,k,n);
}
static int update(mbedtls_md_context_t* c,const unsigned char* d,size_t n) {
  ++update_calls;
  return ((fault==4&&update_calls==1)||(fault==5&&update_calls==2))?
      injected_error:mbedtls_md_hmac_update(c,d,n);
}
static int finish(mbedtls_md_context_t* c,unsigned char* out) {
  if(fault==6)return injected_error;
  const int rc=mbedtls_md_hmac_finish(c,out); if(fault==8&&!rc)out[0]^=1;
  return rc;
}
static void md_free(mbedtls_md_context_t* c){++md_frees;mbedtls_md_free(c);}
static int pbkdf2(mbedtls_md_type_t t,const unsigned char* p,size_t pn,
                 const unsigned char* s,size_t sn,unsigned int iterations,
                 uint32_t n,unsigned char* out) {
  ++crypto_calls;
  if(fault==7)return injected_error;
  const int rc=mbedtls_pkcs5_pbkdf2_hmac_ext(t,p,pn,s,sn,iterations,n,out);
  if(fault==9&&!rc)out[n-1]^=1;
  return rc;
}
#define mbedtls_md_info_from_type test_md_info
#define mbedtls_md_setup setup
#define mbedtls_md_hmac_starts starts
#define mbedtls_md_hmac_update update
#define mbedtls_md_hmac_finish finish
#define mbedtls_md_free md_free
#define mbedtls_pkcs5_pbkdf2_hmac_ext pbkdf2
'''

AFTER_HEADER = r'''
#undef mbedtls_md_info_from_type
#undef mbedtls_md_setup
#undef mbedtls_md_hmac_starts
#undef mbedtls_md_hmac_update
#undef mbedtls_md_hmac_finish
#undef mbedtls_md_free
#undef mbedtls_pkcs5_pbkdf2_hmac_ext
static std::string output;
struct Logger {
  template<class... A> void printf(const char* format,A... args){
    char b[512];snprintf(b,sizeof(b),format,args...);output+=b;
  }
  void println(const char* s){output+=s;output+='\n';}
} Serial;
static std::atomic<bool> g_provision_crypto_diag_requested{false};
struct Manager {bool setup=false;bool isSetupModeActive()const{return setup;}}g_provisioning_manager;
namespace ProvisioningState {
enum State {STATE_UNPROVISIONED,STATE_AP_SETUP,STATE_CONNECTING_HOME_WIFI,STATE_CONNECTED,STATE_ERROR};
static bool provisioned=false;
static State state=STATE_AP_SETUP;
static bool isProvisioned(){return provisioned;}
static State getState(){return state;}
}
static void reset(int next_fault=0){
  fault=next_fault;update_calls=0;md_frees=crypto_calls=0;output.clear();
  g_provision_crypto_diag_requested.store(false);
  g_provisioning_manager.setup=true;ProvisioningState::provisioned=false;
  ProvisioningState::state=ProvisioningState::STATE_AP_SETUP;
}
'''

TESTS = r'''
int main(){
  const std::string success="[PROVISION_CRYPTO_DIAG] hmac_sha1_rc=0 hmac_sha1_ok=1 "
    "pbkdf2_sha1_rc=0 pbkdf2_sha1_ok=1 all_ok=1\n";
  for(int i=0;i<16;++i){
    reset();const auto r=provision_crypto_diag::run_and_log(Serial);
    check(r.ok(),"both public vectors agree using real mbedTLS");
    check(output==success,"success log contains only named return codes and booleans");
    check(md_frees==1,"HMAC context cleaned");
  }
  for(int f=1;f<=9;++f){
    reset(f);const auto r=provision_crypto_diag::run_and_log(Serial);
    check(!r.ok(),"injected error/corrupt result cannot report success");
    const auto failed=(f<=6||f==8)?r.hmac_sha1:r.pbkdf2_sha1;
    check(!failed.matches,"failure equality is false");
    check(failed.rc==(f==1?MBEDTLS_ERR_MD_FEATURE_UNAVAILABLE:f<=7?injected_error:0),
          "SDK return preserved separately from output mismatch");
    check(md_frees==1,"HMAC cleanup runs after every failure boundary");
    check(crypto_calls==2,"other independent primitive runs after a failure");
    check(output.find("all_ok=0\n")!=std::string::npos,"failure log reports aggregate false");
    for(char c:output) check((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
                            (c>='0'&&c<='9')||strchr("[]_=- \n",c),"bounded log alphabet");
  }
  reset();service_provision_crypto_diag();
  check(crypto_calls==0&&output.empty(),"idle owner never starts a diagnostic");
  for(int i=0;i<5;++i)halo_prod_provision_crypto_diag();
  check(crypto_calls==0&&output.empty(),"request path only queues, no crypto/log work");
  service_provision_crypto_diag();
  check(crypto_calls==2&&output==success,"duplicate pending requests coalesce into one owner run");
  service_provision_crypto_diag();
  check(crypto_calls==2,"consumed request does not run again");
  for(int scenario=0;scenario<6;++scenario){
    reset();halo_prod_provision_crypto_diag();
    if(scenario==0)g_provisioning_manager.setup=false;
    else if(scenario==1)ProvisioningState::provisioned=true;
    else ProvisioningState::state=static_cast<ProvisioningState::State>(scenario==2?0:scenario-1);
    service_provision_crypto_diag();
    check(crypto_calls==0&&output=="[PROVISION_CRYPTO] skipped=setup_guard\n",
          "owner rechecks active, unprovisioned AP_SETUP at execution");
    g_provisioning_manager.setup=true;ProvisioningState::provisioned=false;
    ProvisioningState::state=ProvisioningState::STATE_AP_SETUP;
    output.clear();service_provision_crypto_diag();
    check(crypto_calls==0&&output.empty(),"skipped diagnostic is consumed, never deferred into later session");
  }
  printf("%s provisioning crypto diagnostic: %u checks, %u failures\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--mbedtls-prefix", type=Path, default=Path("/opt/homebrew/opt/mbedtls"))
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    texts = {name: (root/name).read_text() for name in (HEADER, WRAPPER, SENSE)}
    request = definition(texts[WRAPPER], "void halo_prod_provision_crypto_diag()")
    service = definition(texts[WRAPPER], "static void service_provision_crypto_diag()")
    dispatch = definition(texts[SENSE], 'if (strcmp(type, "INPUT_PROVISION_CRYPTO_DIAG") == 0)')
    assert "halo_prod_provision_crypto_diag();" in dispatch
    owner = definition(texts[WRAPPER], "void halo_prod_loop()")
    assert owner.count("service_provision_crypto_diag();") == 1
    assert texts[WRAPPER].count("provision_crypto_diag::run_and_log(") == 1
    assert texts[WRAPPER].count("service_provision_crypto_diag();") == 1
    for forbidden in ("mbedtls_platform_set_calloc_free", "WiFi.", "esp_wifi_", "Preferences", "Serial."):
        assert forbidden not in texts[HEADER], forbidden
    harness = "\n".join([PREFIX, '#include "'+str(root/HEADER)+'"', AFTER_HEADER, request, service, TESTS])
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("C++ compiler required")
    prefix = args.mbedtls_prefix.resolve()
    with tempfile.TemporaryDirectory(prefix="halo-provision-crypto-") as work:
        cpp, binary = Path(work)/"test.cpp", Path(work)/"test"
        cpp.write_text(harness)
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                   "-I"+str(prefix/"include"), str(cpp), "-L"+str(prefix/"lib"),
                   "-Wl,-rpath,"+str(prefix/"lib"), "-lmbedcrypto", "-o", str(binary)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=30)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30) if not build.returncode else None
    result = {"status": "PASS" if run and not run.returncode else "FAIL",
              "scope": "Actual header with native mbedTLS HMAC-SHA1/PBKDF2 public vectors,9 injected failure/corruption boundaries and actual owner queue/service. Not ESP hardware or active-handshake proof; AES-wrap is outside scope.",
              "source_root": str(root), "mbedtls_prefix": str(prefix),
              "source_sha256": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in texts.items()},
              "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "harness_sha256": hashlib.sha256(harness.encode()).hexdigest(),
              "build_returncode": build.returncode, "run_returncode": run.returncode if run else None,
              "output": build.stdout+build.stderr+(run.stdout+run.stderr if run else "")}
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out/"RESULT.json").write_text(json.dumps(result, indent=2)+"\n")
        (args.out/"run.log").write_text(result["output"])
        (args.out/"harness.cpp").write_text(harness)
    print(result["output"], end="")
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

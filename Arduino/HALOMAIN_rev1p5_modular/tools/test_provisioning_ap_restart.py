#!/usr/bin/env python3
"""Run the actual setup-only AP restart using faulted public SDK boundaries.

No hardware/network access. Normal startup is source-pinned to remain unchanged;
the explicit command executes same-profile AP disable/begin and IP verification.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_provisioning_display_status import definition

ROOT = Path(__file__).resolve().parents[1]
MANAGER = "halo_ota_demo/firmware/shared/ProvisioningManager.cpp"
HEADER = "halo_ota_demo/firmware/shared/ProvisioningManager.h"
WRAPPER = "halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino"
SENSE = "Sense_Minimal/Sense_Minimal.ino"

PREFIX = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
static unsigned checks=0,failures=0;
static void check(bool ok,const char* label){++checks;if(!ok){++failures;printf("FAIL %s\n",label);}}
static std::string output;
static struct {
  void println(const char* line){output+=line;output+='\n';}
  template<class... A> void printf(const char* format,A... args){
    char line[512];snprintf(line,sizeof(line),format,args...);output+=line;
  }
} Serial;
namespace ProvisioningState {
  enum State {STATE_UNPROVISIONED,STATE_AP_SETUP,STATE_CONNECTING_HOME_WIFI,STATE_CONNECTED,STATE_ERROR};
  static State state=STATE_AP_SETUP;
  static bool provisioned=false;
  static bool isProvisioned(){return provisioned;}
  static State getState(){return state;}
}
using esp_err_t=int; using wifi_mode_t=int;
static constexpr int ESP_OK=0,WIFI_MODE_NULL=0,WIFI_MODE_APSTA=3,WIFI_IF_AP=1,WL_CONNECTED=3;
struct wifi_config_t {struct {
  uint8_t ssid[32],password[64],ssid_len,channel;
  int authmode,pairwise_cipher;
  struct {bool capable,required;} pmf_cfg;
  uint8_t ssid_hidden,max_connection;
  uint16_t beacon_interval;
} ap;};
struct Ip {uint32_t addr;};
struct esp_netif_ip_info_t {Ip ip,gw,netmask;};
struct esp_netif_t {};
struct IPAddress {uint32_t addr;explicit IPAddress(uint32_t value):addr(value){}};
class ProvisioningManager {public:
  char ap_ssid[64]={},ap_password[16]={};
  bool setup_mode_active=true,claim_in_progress=false;
  bool restartSetupAp();
} g_provisioning_manager;
static std::atomic<bool> g_provision_ap_restart_requested{false};
static int http_object=1,dns_object=2;
static int* server=&http_object;
static int* dns_server=&dns_object;
static bool g_scan_inflight=false;
static esp_netif_t netif;
static wifi_config_t driver;
static esp_netif_ip_info_t ip;
static std::vector<std::string> actions;
static int mode=3,status=6,mode_reads=0,config_reads=0,ip_reads=0,netif_reads=0;
static int bad_mode_read=0,bad_config_read=0,bad_ip_read=0,missing_netif_read=0;
static int changed_config_byte=-1,changed_ip_field=-1;
static bool disable_ok=true,enable_ok=true,restore_ok=true,restore_takes_effect=true;
static int mode_after_begin=3;
static esp_err_t esp_wifi_get_mode(wifi_mode_t* out){
  *out=mode;return ++mode_reads==bad_mode_read?123:ESP_OK;
}
static esp_err_t esp_wifi_get_config(int interface,wifi_config_t* out){
  check(interface==WIFI_IF_AP,"only AP config read");*out=driver;
  return ++config_reads==bad_config_read?123:ESP_OK;
}
static esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key){
  check(!strcmp(key,"WIFI_AP_DEF"),"only AP netif read");
  return ++netif_reads==missing_netif_read?nullptr:&netif;
}
static esp_err_t esp_netif_get_ip_info(esp_netif_t* n,esp_netif_ip_info_t* out){
  check(n==&netif,"existing netif");*out=ip;
  return ++ip_reads==bad_ip_read?123:ESP_OK;
}
static struct {
  struct {
    bool begin(){
      actions.push_back("begin");
      check(mode==1,"AP disabled while STA retained before begin");
      if(!enable_ok)return false;
      mode=mode_after_begin;
      if(changed_config_byte>=0)reinterpret_cast<unsigned char*>(&driver.ap)[changed_config_byte]^=1;
      if(changed_ip_field==0)ip.ip.addr^=1;
      if(changed_ip_field==1)ip.gw.addr^=1;
      if(changed_ip_field==2)ip.netmask.addr^=1;
      return true;
    }
  } AP;
  int status(){return ::status;}
  bool enableAP(bool enabled){
    check(!enabled,"only public disable called directly");actions.push_back("disable");
    if(disable_ok)mode=1;return disable_ok;
  }
  bool softAPConfig(IPAddress addr,IPAddress gateway,IPAddress mask){
    actions.push_back("restore_ip");
    if(restore_ok&&restore_takes_effect)ip={{addr.addr},{gateway.addr},{mask.addr}};
    return restore_ok;
  }
} WiFi;
static void reset(){
  output.clear();actions.clear();driver={};g_provisioning_manager={};
  strcpy(g_provisioning_manager.ap_ssid,"Trepo-Halo-1A6D-ABCF");
  strcpy(g_provisioning_manager.ap_password,"private12345");
  memcpy(driver.ap.ssid,g_provisioning_manager.ap_ssid,20);
  memcpy(driver.ap.password,g_provisioning_manager.ap_password,12);
  driver.ap.ssid_len=20;driver.ap.channel=1;driver.ap.authmode=3;
  driver.ap.pairwise_cipher=4;driver.ap.max_connection=4;driver.ap.beacon_interval=100;
  ip={{0x0104a8c0},{0x0104a8c0},{0x00ffffff}};
  ProvisioningState::state=ProvisioningState::STATE_AP_SETUP;ProvisioningState::provisioned=false;
  g_scan_inflight=false;server=&http_object;dns_server=&dns_object;
  mode=3;status=6;mode_reads=config_reads=ip_reads=netif_reads=0;
  bad_mode_read=bad_config_read=bad_ip_read=missing_netif_read=0;
  changed_config_byte=changed_ip_field=-1;
  disable_ok=enable_ok=restore_ok=restore_takes_effect=true;mode_after_begin=3;
  g_provision_ap_restart_requested.store(false);
}
static bool contains(const char* token){return output.find(token)!=std::string::npos;}
'''

TESTS = r'''
static void run(bool expected,const char* stage){
  auto manager_before=g_provisioning_manager;
  const auto state_before=ProvisioningState::state;
  const bool provisioned_before=ProvisioningState::provisioned;
  const auto* http_before=server;const auto* dns_before=dns_server;
  bool result=g_provisioning_manager.restartSetupAp();
  check(result==expected,"return status");check(contains(stage),"specific result stage");
  check(!memcmp(&manager_before,&g_provisioning_manager,sizeof(manager_before)),"manager identity/state untouched");
  check(state_before==ProvisioningState::state&&provisioned_before==ProvisioningState::provisioned,
        "provisioning state/ownership untouched");
  check(server==http_before&&dns_server==dns_before,"existing HTTP/DNS objects retained");
  check(!contains("private12345")&&!contains("Trepo-Halo"),"no credential output");
}
static void rejected(const char* stage){run(false,stage);check(actions.empty(),"rejected before radio mutation");}
int main(){
  reset();run(true,"stage=verify config_same=1 ip_same=1 ip_restored=0");
  check(actions==std::vector<std::string>({"disable","begin"}),"exactly one AP-only cycle");
  for(int flag=0;flag<7;++flag){
    reset();
    if(flag==0)g_provisioning_manager.setup_mode_active=false;
    if(flag==1)ProvisioningState::provisioned=true;
    if(flag==2)g_provisioning_manager.claim_in_progress=true;
    if(flag==3)g_scan_inflight=true;
    if(flag==4)server=nullptr;
    if(flag==5)dns_server=nullptr;
    if(flag==6)ProvisioningState::state=ProvisioningState::STATE_CONNECTING_HOME_WIFI;
    rejected("stage=setup_guard");
  }
  for(int state=0;state<5;++state){
    if(state==ProvisioningState::STATE_AP_SETUP)continue;
    reset();ProvisioningState::state=(ProvisioningState::State)state;rejected("stage=setup_guard");
  }
  for(int m=0;m<3;++m){reset();mode=m;rejected("stage=radio_guard");}
  reset();status=WL_CONNECTED;rejected("stage=radio_guard");
  reset();bad_mode_read=1;rejected("stage=radio_guard");
  reset();bad_config_read=1;rejected("stage=radio_guard");
  reset();bad_ip_read=1;rejected("stage=radio_guard");
  reset();missing_netif_read=1;rejected("stage=radio_guard");
  reset();ip.ip.addr=0;rejected("stage=radio_guard");
  for(int mutation=0;mutation<8;++mutation){
    reset();
    if(mutation==0)driver.ap.ssid[0]^=1;
    if(mutation==1)driver.ap.password[0]^=1;
    if(mutation==2)driver.ap.ssid_len=255;
    if(mutation==3)g_provisioning_manager.ap_ssid[0]=0;
    if(mutation==4)memset(g_provisioning_manager.ap_ssid,'s',64);
    if(mutation==5)g_provisioning_manager.ap_password[7]=0;
    if(mutation==6)memset(g_provisioning_manager.ap_password,'p',16);
    if(mutation==7)memset(driver.ap.password,'p',64);
    rejected("stage=identity_guard");
  }
  reset();driver.ap.ssid_len=0;run(true,"stage=verify config_same=1");
  reset();disable_ok=false;run(false,"stage=disable_failed");
  check(actions==std::vector<std::string>({"disable"}),"disable failure does not attempt begin");
  reset();enable_ok=false;run(false,"stage=enable_failed");
  check(actions==std::vector<std::string>({"disable","begin"}),"enable failure has no hidden retry/reset");
  reset();missing_netif_read=2;run(false,"stage=ip_read_failed");
  reset();bad_ip_read=2;run(false,"stage=ip_read_failed");
  for(int part=0;part<3;++part){
    reset();changed_ip_field=part;auto before=ip;
    run(true,"stage=verify config_same=1 ip_same=1 ip_restored=1");
    check(!memcmp(&before,&ip,sizeof(ip)),"IP/gateway/netmask restored exactly");
    check(actions==std::vector<std::string>({"disable","begin","restore_ip"}),"single supported IP restore");
  }
  reset();changed_ip_field=0;restore_ok=false;run(false,"stage=ip_restore_failed");
  reset();changed_ip_field=1;bad_ip_read=3;run(false,"stage=ip_restore_failed");
  reset();changed_ip_field=2;restore_takes_effect=false;run(false,"ip_same=0");
  for(unsigned byte=0;byte<sizeof(driver.ap);++byte){
    reset();changed_config_byte=(int)byte;run(false,"config_same=0");
  }
  reset();bad_config_read=2;run(false,"config_err=123");
  reset();bad_mode_read=2;run(false,"mode_err=123 mode=-1");
  reset();mode_after_begin=1;run(false,"mode_err=0 mode=1");
  reset();halo_prod_provision_restart_ap();halo_prod_provision_restart_ap();
  check(actions.empty()&&output.empty(),"UART wrapper only queues, never mutates radio");
  service_provision_ap_restart();
  check(contains("ok=1 stage=verify")&&actions.size()==2,"owner consumes/coalesces pending command once");
  service_provision_ap_restart();
  check(actions.size()==2,"consumed command does not repeat");
  reset();halo_prod_provision_restart_ap();ProvisioningState::state=ProvisioningState::STATE_CONNECTED;
  service_provision_ap_restart();
  check(actions.empty()&&contains("stage=setup_guard"),"owner rechecks setup state after queued admission");
  printf("%s AP restart: %u checks, %u failures\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source-root",type=Path,default=ROOT)
    p.add_argument("--out",type=Path)
    args = p.parse_args()
    root = args.source_root.resolve()
    texts = {name:(root/name).read_text() for name in (MANAGER,HEADER,WRAPPER,SENSE)}
    actual = definition(texts[MANAGER],"bool ProvisioningManager::restartSetupAp()")
    wrapper = definition(texts[WRAPPER],"void halo_prod_provision_restart_ap()")
    owner = definition(texts[WRAPPER],"static void service_provision_ap_restart()")
    loop = definition(texts[WRAPPER],"void halo_prod_loop()")
    assert loop.index('service_provision_ap_restart();') < loop.index('g_provisioning_manager.update();')
    dispatch = definition(texts[SENSE],'if (strcmp(type, "INPUT_PROVISION_RESTART_AP") == 0)')
    assert 'halo_prod_provision_restart_ap();' in dispatch
    assert 'void halo_prod_provision_restart_ap();' in texts[SENSE]
    assert 'bool restartSetupAp();' in texts[HEADER]
    for signature in ('bool ProvisioningManager::startSoftAP()', 'bool ProvisioningManager::startSetupMode()'):
        assert 'restartSetupAp' not in definition(texts[MANAGER],signature)
    # Command remains behind the existing ACK/dedup admission, not a second parser.
    assert texts[SENSE].index('sense_input_seen_recently(in_msg_id)') < texts[SENSE].index(dispatch)
    for forbidden in ('esp_wifi_set_config(', 'softAPdisconnect(', 'startSetupMode(', 'stopSetupMode(',
                      'saveApCreds(', 'generateApSsid(', 'startHttpServer(', 'stopHttpServer(', 'esp_wifi_restore('):
        assert forbidden not in re.sub(r'//[^\n]*','',actual), forbidden
    harness='\n'.join([PREFIX,actual,wrapper,owner,TESTS])
    compiler=shutil.which('clang++') or shutil.which('g++')
    with tempfile.TemporaryDirectory(prefix='halo-provision-restart-') as tmp:
        cpp,binary=Path(tmp)/'test.cpp',Path(tmp)/'test'
        cpp.write_text(harness)
        build=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                              '-fsanitize=address,undefined',str(cpp),'-o',str(binary)],
                             capture_output=True,text=True,timeout=30)
        run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10) if build.returncode==0 else None
    result={'status':'PASS' if run and run.returncode==0 else 'FAIL',
            'scope':'Actual restart and wrapper with faulted public SDK boundaries; startup unchanged and console admission source-pinned. No hardware.',
            'source_root':str(root),'source_sha256':{name:hashlib.sha256(s.encode()).hexdigest() for name,s in texts.items()},
            'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'harness_sha256':hashlib.sha256(harness.encode()).hexdigest(),
            'build_returncode':build.returncode,'run_returncode':run.returncode if run else None,
            'output':build.stdout+build.stderr+(run.stdout+run.stderr if run else '')}
    if args.out:
        args.out.mkdir(parents=True,exist_ok=True)
        (args.out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
        (args.out/'run.log').write_text(result['output'])
        (args.out/'harness.cpp').write_text(harness)
    print(result['output'],end='')
    return 0 if result['status']=='PASS' else 1


if __name__=='__main__':
    raise SystemExit(main())

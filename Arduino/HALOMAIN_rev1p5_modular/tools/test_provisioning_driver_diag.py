#!/usr/bin/env python3
"""Execute the production AP diagnostic/event functions with SDK getter doubles.

Host-only: no serial, Wi-Fi, firmware build, NVS, or device access. The doubles
expose only read APIs to the diagnostic. Source checks pin startup and UART wiring.
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
MANAGER = "halo_ota_demo/firmware/shared/ProvisioningManager.cpp"
HEADER = "halo_ota_demo/firmware/shared/ProvisioningManager.h"
WRAPPER = "halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino"
SENSE = "Sense_Minimal/Sense_Minimal.ino"
WIFI = "Sense_Minimal/sense_wifi.h"
EVENTS = "Sense_Minimal/sense_wifi_events.h"

PREFIX = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
static unsigned checks=0, failures=0;
static void check(bool ok, const char* message) {
  ++checks; if (!ok) { ++failures; printf("FAIL %s\n", message); }
}
static std::string output;
struct Logger {
  template<class... A> void printf(const char* format, A... args) {
    char buffer[2048]; snprintf(buffer,sizeof(buffer),format,args...); output+=buffer;
  }
} Serial;
static unsigned long millis() { return 12345; }
using esp_err_t=int;
static constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=259, WIFI_IF_AP=1;
using wifi_mode_t=int;
using wifi_second_chan_t=int;
static constexpr int WIFI_MODE_NULL=0, WIFI_SECOND_CHAN_NONE=0;
struct wifi_config_t {
  struct {
    uint8_t ssid[32], password[64], ssid_len, channel;
    int authmode, pairwise_cipher;
    struct {bool capable,required;} pmf_cfg;
    uint8_t ssid_hidden,max_connection;
    uint16_t beacon_interval;
  } ap;
};
struct wifi_sta_list_t {int num;};
struct esp_netif_t {};
struct Ip {uint8_t bytes[4];};
struct esp_netif_ip_info_t {Ip ip;};
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) (ip)->bytes[0],(ip)->bytes[1],(ip)->bytes[2],(ip)->bytes[3]
static wifi_config_t driver;
static int errors[5], reads[6];
static bool have_netif=true;
static esp_netif_t ap_netif;
static esp_err_t esp_wifi_get_mode(wifi_mode_t* out) {
  ++reads[0]; *out=3; return errors[0];
}
static esp_err_t esp_wifi_get_config(int interface,wifi_config_t* out) {
  check(interface==WIFI_IF_AP,"only AP configuration requested");
  ++reads[1]; *out=driver; return errors[1];
}
static esp_err_t esp_wifi_get_channel(uint8_t* channel,wifi_second_chan_t* secondary) {
  ++reads[2]; *channel=6; *secondary=0; return errors[2];
}
static esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t* out) {
  ++reads[3]; out->num=2; return errors[3];
}
static esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key) {
  check(!strcmp(key,"WIFI_AP_DEF"),"only AP netif requested"); ++reads[4];
  return have_netif?&ap_netif:nullptr;
}
static esp_err_t esp_netif_get_ip_info(esp_netif_t* netif,esp_netif_ip_info_t* out) {
  check(netif==&ap_netif,"AP IP read"); ++reads[5];
  *out={{{192,168,4,1}}}; return errors[4];
}
class ProvisioningManager {
 public:
  char ap_ssid[64]={},ap_password[16]={};
  bool setup_mode_active=false;
  void logApDiagnostics(const char*) const;
} g_provisioning_manager;
static bool contains(const char* token) {return output.find(token)!=std::string::npos;}
static void reset() {
  output.clear(); memset(errors,0,sizeof(errors)); memset(reads,0,sizeof(reads));
  have_netif=true; driver={}; g_provisioning_manager={};
  strcpy(g_provisioning_manager.ap_ssid,"Trepo-Halo-1A6D-2752");
  strcpy(g_provisioning_manager.ap_password,"MANAGERabc123");
  memcpy(driver.ap.ssid,g_provisioning_manager.ap_ssid,strlen(g_provisioning_manager.ap_ssid));
  memcpy(driver.ap.password,g_provisioning_manager.ap_password,strlen(g_provisioning_manager.ap_password));
  driver.ap.ssid_len=strlen(g_provisioning_manager.ap_ssid); driver.ap.channel=1; driver.ap.authmode=3;
  driver.ap.pairwise_cipher=4; driver.ap.pmf_cfg={true,false};
  driver.ap.max_connection=4; driver.ap.beacon_interval=100;
}
static void query() {
  auto manager_before=g_provisioning_manager;
  auto driver_before=driver;
  halo_prod_provision_diag();
  check(!memcmp(&manager_before,&g_provisioning_manager,sizeof(manager_before)),"manager untouched");
  check(!memcmp(&driver_before,&driver,sizeof(driver_before)),"driver untouched");
  check(!contains("MANAGERabc123")&&!contains("DRIVERxyz9876"),"no password output");
  for(int i=0;i<5;++i) check(reads[i]==1,"each SDK getter called once");
  check(reads[5]==(have_netif?1:0),"IP getter only with existing netif");
}
'''

EVENT_PREFIX = r'''
namespace events_test {
static bool in_callback=false;
#define portMUX_TYPE std::mutex
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
enum WiFiEvent_t {ARDUINO_EVENT_WIFI_STA_DISCONNECTED=113,
  ARDUINO_EVENT_WIFI_STA_GOT_IP=115, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED=133};
struct WiFiEventInfo_t {
  struct {uint8_t reason;} wifi_sta_disconnected;
  struct {uint16_t reason;} wifi_ap_stadisconnected;
};
using wl_status_t=int;
static constexpr int WL_CONNECTED=3;
using String=std::string;
static unsigned disconnects=0,successes=0,uart_calls=0;
static bool wifi_connect_inflight=true;
static int wifi_state=1;
struct FakeIp {String toString(){return "192.0.2.1";}};
static struct {
  wl_status_t status(){check(!in_callback,"no callback WiFi access");return 6;}
  int RSSI(){return -50;}
  FakeIp localIP(){return {};}
} WiFi;
static void wifi_diag_note_disconnect(){++disconnects;}
static void wifi_diag_note_success(int){++successes;}
static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){++uart_calls;}
'''

TESTS = r'''
static void run_events() {
  output.clear();
  WiFiEventInfo_t info={{201},{0xabcd}};
  in_callback=true;
  handle_wifi_event(ARDUINO_EVENT_WIFI_AP_STADISCONNECTED,info);
  in_callback=false;
  check(output.empty(),"callback does not log");
  wifi_service_events();
  check(contains("reason=43981 scope=ap_client"),"full 16-bit AP reason, correct scope");
  check(disconnects==0&&successes==0&&uart_calls==0&&wifi_connect_inflight,
        "AP event cannot alter home-STA guard/counters");
  output.clear();
  handle_wifi_event(ARDUINO_EVENT_WIFI_STA_DISCONNECTED,info);
  wifi_service_events();
  check(contains("reason=201 scope=home_sta")&&disconnects==1,"home-STA reason still distinct");
  output.clear();
  for(unsigned n=0;n<20;++n) handle_wifi_event(ARDUINO_EVENT_WIFI_AP_STADISCONNECTED,info);
  wifi_service_events();
  check(contains("dropped=4"),"overflow remains bounded and disclosed");
}
} // events_test
int main() {
  reset(); query();
  check(contains("source=console")&&contains("setup=0 mode=3"),"inactive manager does not start AP");
  check(contains("driver_ssid=\"Trepo-Halo-1A6D-2752\" ssid_len=20")&&
        contains("ssid_matches_manager=1 password_matches_manager=1"),"matching credentials read from driver");
  check(contains("channel=6")&&contains("configured_channel=1")&&contains("clients=2")&&
        contains("ip=192.168.4.1")&&contains("auth=3 pairwise=4")&&
        contains("pmf_capable=1 pmf_required=0"),"live/configured radio settings disclosed");
  reset(); driver.ap.ssid_len=0; query();
  check(contains("ssid_matches_manager=1"),"SDK zero SSID length uses bounded string");
  reset(); strcpy((char*)driver.ap.password,"DRIVERxyz9876"); query();
  check(contains("ssid_matches_manager=1 password_matches_manager=0"),"password mismatch detected without disclosure");
  reset(); driver.ap.ssid[18]='3'; query();
  check(contains("ssid_matches_manager=0 password_matches_manager=1"),"SSID mismatch detected separately");
  reset(); memset(driver.ap.password,'x',sizeof(driver.ap.password)); query();
  check(contains("password_matches_manager=0"),"nonterminated driver password bounded");
  reset(); memset(driver.ap.ssid,'s',sizeof(driver.ap.ssid)); driver.ap.ssid_len=255; query();
  check(contains("ssid_len=255 ssid_matches_manager=0"),"invalid driver SSID length bounded");
  reset(); driver.ap.ssid[0]='\n'; driver.ap.ssid[1]='"'; driver.ap.ssid[2]='\\'; query();
  check(contains("driver_ssid=\"???po-"),"driver SSID cannot inject diagnostic lines");
  reset(); memset(g_provisioning_manager.ap_password,0,16); memset(driver.ap.password,0,64); query();
  check(contains("password_matches_manager=0"),"uninitialized empty credentials cannot claim match");
  reset(); memset(g_provisioning_manager.ap_password,'x',16); query();
  check(contains("password_matches_manager=0"),"nonterminated manager password bounded");
  const char* error_tokens[]={"mode=-1 mode_err=123","config_err=123","channel=-1 secondary=-1 channel_err=123",
                             "clients=-1 clients_err=123","ip_err=123"};
  for(unsigned e=0;e<5;++e) {
    reset(); errors[e]=123; query(); check(contains(error_tokens[e]),"getter failure explicit");
    if(e==1) check(contains("driver_ssid=\"\" ssid_len=-1 ssid_matches_manager=0 password_matches_manager=0 auth=-1"),
                   "failed config never presents stale credentials/settings as valid");
  }
  reset(); have_netif=false; query();
  check(contains("ip=0.0.0.0 ip_err=259"),"missing AP netif is not initialized by diagnostic");
  reset(); g_provisioning_manager.logApDiagnostics(nullptr);
  check(contains("source=query"),"null source safely labeled");
  events_test::run_events();
  printf("%s provisioning driver diagnostic: %u checks, %u failures\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    texts = {name: (root/name).read_text() for name in (MANAGER, HEADER, WRAPPER, SENSE, WIFI, EVENTS)}
    diagnostic = definition(texts[MANAGER], "void ProvisioningManager::logApDiagnostics(")
    helper = definition(texts[WRAPPER], "void halo_prod_provision_diag()")
    startup = definition(texts[MANAGER], "bool ProvisioningManager::startSoftAP()")
    dispatch = definition(texts[SENSE], 'if (strcmp(type, "INPUT_PROVISION_DIAG") == 0)')
    assert 'void logApDiagnostics(const char* source) const;' in texts[HEADER]
    assert 'void halo_prod_provision_diag();' in texts[SENSE]
    assert startup.index('logApDiagnostics("ap_started");') > startup.index('WiFi.softAP(ap_ssid, ap_password)')
    assert 'halo_prod_provision_diag();' in dispatch
    assert 'sfwd ' in (root/"LCD_Minimal/lcd_uart_task.h").read_text()
    for forbidden in ('WiFi.', 'esp_wifi_set_', 'esp_wifi_start(', 'esp_wifi_stop(', 'ProvisioningState::', 'delay('):
        assert forbidden not in diagnostic, forbidden
    # The complete actual callback/owner service plus its production queue.
    event_source = texts[WIFI][texts[WIFI].index('static constexpr size_t WIFI_EVENT_QUEUE_CAPACITY'):
                               texts[WIFI].index('// ── WiFi connection functions')]
    prefix = PREFIX.replace('static void query() {', helper + '\nstatic void query() {')
    harness = '\n'.join([prefix, diagnostic, '#include "'+str(root/EVENTS)+'"',
                          EVENT_PREFIX, event_source, TESTS])
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("C++ compiler required")
    with tempfile.TemporaryDirectory(prefix="halo-provision-driver-") as work:
        cpp, binary = Path(work)/"test.cpp", Path(work)/"test"
        cpp.write_text(harness)
        build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                "-fsanitize=address,undefined", str(cpp), "-o", str(binary)],
                               capture_output=True, text=True, timeout=30)
        run = subprocess.run([str(binary)],capture_output=True,text=True,timeout=10) if build.returncode==0 else None
    result = {"status":"PASS" if run and run.returncode==0 else "FAIL",
              "scope":"Actual read-only diagnostic, wrapper helper, event callback and owner service; SDK doubles, source-pinned startup/console wiring. No hardware.",
              "source_root":str(root), "source_sha256":{name:hashlib.sha256(text.encode()).hexdigest() for name,text in texts.items()},
              "test_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "harness_sha256":hashlib.sha256(harness.encode()).hexdigest(),
              "build_returncode":build.returncode, "run_returncode":run.returncode if run else None,
              "output":build.stdout+build.stderr+(run.stdout+run.stderr if run else "")}
    if args.out:
        args.out.mkdir(parents=True,exist_ok=True)
        (args.out/"RESULT.json").write_text(json.dumps(result,indent=2)+"\n")
        (args.out/"run.log").write_text(result["output"])
        (args.out/"harness.cpp").write_text(harness)
    print(result["output"],end="")
    return 0 if result["status"]=="PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

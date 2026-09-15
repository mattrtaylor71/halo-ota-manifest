#!/usr/bin/env python3
"""Actual RAM-only USB diagnostic, with Wi-Fi/clock replaced; no hardware access."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HARNESS = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
static uint32_t now_ms=100;
static uint32_t millis(){return now_ms;}
struct Logger {std::string last;template<class... A> void printf(const char* fmt,A... a){char b[512];std::snprintf(b,sizeof(b),fmt,a...);last=b;}void println(const char* s){last=s;}} Serial;
static bool busy=false,wifi_connect_inflight=false;
static bool sense_backup_diagnostic_busy(){return busy;}
struct Wifi {
 bool autoreconnect=true,set_ok=true,disconnect_ok=true,connected=true;
 unsigned disconnects=0,sets=0;bool saw_wifi_off=false,saw_erase=false;
 bool getAutoReconnect(){return autoreconnect;}
 bool setAutoReconnect(bool value){++sets;if(!set_ok)return false;autoreconnect=value;return true;}
 bool disconnect(bool off,bool erase){++disconnects;saw_wifi_off|=off;saw_erase|=erase;if(disconnect_ok)connected=false;return disconnect_ok;}
 int status(){return connected?3:6;}
} WiFi;
#include "Sense_Minimal/sense_backup_diagnostic.h"
static unsigned checks=0,failed=0;
static void check(bool b,const char* text){++checks;if(!b){++failed;std::fprintf(stderr,"FAIL: %s\n",text);}}
static const char* test_nonce="0123456789abcdef0123456789abcdef";
static void reset(){using namespace sense_backup_diag;phase=Idle;wifi_calls=0;deadline_ms=0;previous_autoreconnect=true;nonce[0]=0;now_ms=100;WiFi=Wifi{};busy=false;wifi_connect_inflight=false;Serial.last.clear();}
static void command(unsigned sec,const char* id=test_nonce){char line[96];std::snprintf(line,sizeof(line),"backupoffline %s %u",id,sec);check(sense_backup_usb_command(line),"USB command recognized");}
int main(){
 reset();check(!sense_backup_usb_command("status"),"unrelated USB input unchanged");check(!sense_backup_offline_active(),"ordinary boot defaults online");
 for(const char* text:{"backupoffline", "backupoffline bad 1", "backupoffline 0123456789abcdef0123456789abcdef 181", "backupoffline 0123456789abcdef0123456789abcdef -1", "backupoffline 0123456789abcdef0123456789abcdef 1 trailing"}){
  check(sense_backup_usb_command(text),"invalid service input handled");check(!sense_backup_offline_active()&&WiFi.disconnects==0,"invalid request cannot alter network");}
 command(180);check(sense_backup_offline_active()&&!WiFi.autoreconnect&&!WiFi.connected,"real disconnect requested and reconnect suppressed");
 check(WiFi.disconnects==1&&!WiFi.saw_wifi_off&&!WiFi.saw_erase,"disconnect never powers off radio or erases credentials");
 const uint32_t original=sense_backup_diag::deadline_ms.load();now_ms+=1000;command(180);check(sense_backup_diag::deadline_ms==original&&WiFi.disconnects==1,"active same nonce cannot renew original deadline");
 command(2,"11111111111111111111111111111111");check(sense_backup_diag::deadline_ms==original&&WiFi.disconnects==1,"another nonce cannot replace active episode");
 command(0,"11111111111111111111111111111111");check(sense_backup_offline_active(),"wrong nonce cannot resume episode");
 {SenseBackupWifiCall call;check(!call&&sense_backup_diag::wifi_calls==0,"connection call refuses while offline");}
 now_ms=original-1;sense_backup_diagnostic_tick();check(sense_backup_offline_active(),"offline until exact deadline");
 now_ms=original;sense_backup_diagnostic_tick();check(!sense_backup_offline_active()&&WiFi.autoreconnect,"expiry restores previous reconnect setting");
 check(!WiFi.saw_erase,"expiry does not erase credentials");
 reset();WiFi.autoreconnect=false;command(1);command(0);check(!sense_backup_offline_active()&&!WiFi.autoreconnect,"resume restores original false autoreconnect setting");
 reset();busy=true;command(10);check(!sense_backup_offline_active()&&!WiFi.disconnects,"OTA HTTP provisioning binary busy predicate refuses");
 reset();wifi_connect_inflight=true;command(10);check(!sense_backup_offline_active()&&!WiFi.disconnects,"inflight connection refuses diagnostic");
 reset();{SenseBackupWifiCall a;SenseBackupWifiCall b;check(a&&b&&sense_backup_diag::wifi_calls==2,"nested connection calls tracked");command(10);check(!sense_backup_offline_active()&&!WiFi.disconnects,"active SDK caller excludes diagnostic");}
 check(sense_backup_diag::wifi_calls==0,"all SDK call admissions released");command(1);check(sense_backup_offline_active(),"diagnostic admitted after earlier calls close");
 reset();sense_backup_diag::phase=sense_backup_diag::Arming;{SenseBackupWifiCall a;check(!a&&sense_backup_diag::wifi_calls==0,"arming transition excludes new connection calls");}
 reset();WiFi.set_ok=false;command(10);check(!sense_backup_offline_active()&&!WiFi.disconnects,"failed autoreconnect control never disconnects");
 reset();WiFi.disconnect_ok=false;command(10);check(!sense_backup_offline_active()&&WiFi.autoreconnect,"failed disconnect restores prior reconnect mode");
 reset();now_ms=0xffffff00u;command(1);now_ms+=999;check(sense_backup_offline_active(),"wrap-safe deadline remains active before expiry");now_ms++;check(!sense_backup_offline_active()&&WiFi.autoreconnect,"wrap-safe expiry restores network");
 reset();command(1);now_ms+=1000;{SenseBackupWifiCall call;check(call&&!sense_backup_offline_active(),"connection admission itself services elapsed deadline");}
 reset();check(!sense_backup_offline_active()&&WiFi.autoreconnect,"fresh RAM initialization restores normal boot behavior");
 std::printf("%s %u actual diagnostic controls (%u failures)\n",failed?"FAIL":"PASS",checks,failed);return failed?1:0;
}
'''

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    args = ap.parse_args()
    root = args.source_root.resolve()
    wifi = (root/'Sense_Minimal/sense_wifi.h').read_text()
    names = ['wifi_guard_try_claim_connect','wifi_dump_scan','wifi_guard_handle_timeout','wifi_guard_poll',
             'ensure_wifi_connected','wifi_connect','service_wifi_maintenance','ensure_wifi_ready',
             'wifi_hard_reset_and_reconnect','wifi_recover_if_needed']
    for name in names:
        pattern = r'static (?:bool|void) '+name+r'\([^;{}]*\)\s*\{\s*SenseBackupWifiCall backup_call;\s*if\(!backup_call\)return(?: false)?;'
        assert re.search(pattern,wifi), f'missing first-entry SDK guard: {name}'
    compiler=shutil.which('clang++') or shutil.which('g++')
    if not compiler: raise SystemExit('A host C++ compiler is required')
    with tempfile.TemporaryDirectory(prefix='halo-backup-diagnostic-') as tmp:
        p=Path(tmp);(p/'test.cpp').write_text(HARNESS)
        subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-I',str(root),str(p/'test.cpp'),'-o',str(p/'test')],check=True,timeout=30)
        subprocess.run([str(p/'test')],check=True,timeout=10)
    print('PASS 10 actual Wi-Fi entry guard checks; USB-only hook wiring remains a sketch integration check')
if __name__=='__main__':main()

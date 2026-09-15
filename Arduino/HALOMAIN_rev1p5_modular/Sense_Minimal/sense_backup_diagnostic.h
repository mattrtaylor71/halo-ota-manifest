#pragma once

// Direct Sense USB service only. The caller must never route UART/cloud input
// here. This diagnostic forces a local disconnect; it does not simulate RF loss.
// State is RAM-only: no credential, policy or NVS writes. Normal boot restores
// ordinary Wi-Fi configuration. Include after sense_wifi.h's state globals.
#include <atomic>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// Implemented by the sketch: OTA/provisioning/HTTP/upload/binary/sleep commit.
static bool sense_backup_diagnostic_busy();
namespace sense_backup_diag {
enum Phase : uint8_t { Idle=0, Arming=1, Offline=2, Restoring=3 };
static std::atomic<uint8_t> phase{Idle};
static std::atomic<uint32_t> wifi_calls{0};
static std::atomic<uint32_t> deadline_ms{0};
static bool previous_autoreconnect=true;
static char nonce[33]={};
static bool nonce_valid(const char* s) {
  if(!s||strnlen(s,33)!=32)return false;
  for(unsigned i=0;i<32;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return false;
  return true;
}
static void restore(const char* reason) {
  uint8_t expected=Offline;
  if(!phase.compare_exchange_strong(expected,Restoring))return;
  const bool restored=WiFi.setAutoReconnect(previous_autoreconnect);
  Serial.printf("[BACKUP_OFFLINE] nonce=%s result=%s autoreconnect_restored=%u diagnostic=1\n",
                nonce,reason,restored?1:0);
  phase.store(Idle);
}
}

static void sense_backup_diagnostic_tick() {
  using namespace sense_backup_diag;
  if(phase.load()==Offline && (int32_t)((uint32_t)millis()-deadline_ms.load())>=0)restore("expired");
}
static bool sense_backup_offline_active() {
  sense_backup_diagnostic_tick();
  return sense_backup_diag::phase.load()!=sense_backup_diag::Idle;
}

// Counts complete connecting/recovery calls, including nested ones. Arming
// publishes first and then checks the count; a racing entrant rechecks phase.
// Thus no accepted diagnostic interrupts a connection operation already in flight.
// No mutex/critical section is held across SDK I/O or application callbacks.
class SenseBackupWifiCall {
 public:
  SenseBackupWifiCall() {
    if(sense_backup_offline_active())return;
    sense_backup_diag::wifi_calls.fetch_add(1);
    if(sense_backup_diag::phase.load()!=sense_backup_diag::Idle) {
      sense_backup_diag::wifi_calls.fetch_sub(1);return;
    }
    admitted_=true;
  }
  ~SenseBackupWifiCall(){if(admitted_)sense_backup_diag::wifi_calls.fetch_sub(1);}
  explicit operator bool()const{return admitted_;}
  SenseBackupWifiCall(const SenseBackupWifiCall&)=delete;
  SenseBackupWifiCall& operator=(const SenseBackupWifiCall&)=delete;
 private: bool admitted_=false;
};

static bool sense_backup_usb_command(const char* line) {
  using namespace sense_backup_diag;
  static constexpr const char* command="backupoffline";
  if(!line||strncmp(line,command,13)||(line[13]&&line[13]!=' '))return false;
  char request[33]={},extra=0;unsigned seconds=0;
  if(strlen(line)>96||sscanf(line+13," %32s %u %c",request,&seconds,&extra)!=2||
     !nonce_valid(request)||seconds>180) {
    Serial.println("[BACKUP_OFFLINE] result=invalid expected=backupoffline_nonce32_seconds0to180 diagnostic=1");return true;
  }
  sense_backup_diagnostic_tick();
  if(seconds==0) {
    if(phase.load()!=Offline||strcmp(request,nonce)) {
      Serial.println("[BACKUP_OFFLINE] result=no_matching_active_episode diagnostic=1");return true;
    }
    restore("resumed");return true;
  }
  uint8_t expected=Idle;
  if(!phase.compare_exchange_strong(expected,Arming)) {
    Serial.println("[BACKUP_OFFLINE] result=active_no_renewal diagnostic=1");return true;
  }
  if(wifi_calls.load()!=0||wifi_connect_inflight||sense_backup_diagnostic_busy()) {
    phase.store(Idle);Serial.println("[BACKUP_OFFLINE] result=busy diagnostic=1");return true;
  }
  previous_autoreconnect=WiFi.getAutoReconnect();
  if(!WiFi.setAutoReconnect(false)) {
    phase.store(Idle);Serial.println("[BACKUP_OFFLINE] result=autoreconnect_refused diagnostic=1");return true;
  }
  // false,false: keep Wi-Fi enabled and retain all saved credentials.
  if(!WiFi.disconnect(false,false)) {
    WiFi.setAutoReconnect(previous_autoreconnect);phase.store(Idle);
    Serial.println("[BACKUP_OFFLINE] result=disconnect_refused diagnostic=1");return true;
  }
  memcpy(nonce,request,sizeof(nonce));
  deadline_ms=(uint32_t)millis()+seconds*1000u;
  phase.store(Offline);
  Serial.printf("[BACKUP_OFFLINE] nonce=%s result=active seconds=%u until_ms=%lu wifi_status=%d diagnostic=1\n",
                nonce,seconds,(unsigned long)deadline_ms.load(),(int)WiFi.status());
  return true;
}

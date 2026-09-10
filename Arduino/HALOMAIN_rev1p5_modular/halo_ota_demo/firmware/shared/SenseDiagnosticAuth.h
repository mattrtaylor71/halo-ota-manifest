#pragma once
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
#include "DiagnosticAuthProvision.h"
#ifndef HALO_DIAG_AUTH_PROVISIONING
#define HALO_DIAG_AUTH_PROVISIONING 0
#endif
namespace sense_diag_auth {
static bool uncertain=false,attempted=false;
static bool public_id(char*out,size_t n){
 if(n<16)return false;uint8_t mac[6]{};if(esp_read_mac(mac,ESP_MAC_ETH)!=ESP_OK)return false;
 snprintf(out,n,"b1-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);return true;
}
struct Lease {bool held=false;};
static bool safe(){return !uncertain&&sense_diag_safe(nullptr)&&nvs_capacity_image_valid();}
static halo_diag_auth::Guard guard(Lease&l){return {&l,
 [](void*v){auto&x=*static_cast<Lease*>(v);if(x.held||!safe()||g_optional_nvs_writer.test_and_set(std::memory_order_acquire))return false;return x.held=true;},
 [](void*v){auto&x=*static_cast<Lease*>(v);if(x.held){x.held=false;g_optional_nvs_writer.clear(std::memory_order_release);}},
 [](void*){return safe();},[](void*){return halo_policy_allocated_held();}};}
static bool load(char*id,size_t id_n,uint8_t*key,size_t key_n){
 if(!id||!key||key_n!=32||!public_id(id,id_n))return false;
 SenseDiagnosticScope scope(100);if(!scope.active)return false;Lease l;halo_diag_auth::Storage storage(guard(l));
 auto&raw=*reinterpret_cast<uint8_t(*)[32]>(key);if(storage.read(raw))return true;halo_admission::wipe(key,key_n);return false;
}
static bool emit(JsonDocument&doc){
 char line[512];const size_t measured=measureJson(doc);if(doc.overflowed()||measured+1>sizeof(line))return false;
 const size_t n=serializeJson(doc,line,sizeof(line));if(n!=measured)return false;line[n]='\n';size_t sent=0;const uint32_t start=millis();
 while(sent<n+1&&uint32_t(millis()-start)<2000){const int room=Serial.availableForWrite();
  if(Serial&&room>0){size_t z=n+1-sent;if(z>64)z=64;if(z>size_t(room))z=size_t(room);const size_t wrote=Serial.write(reinterpret_cast<uint8_t*>(line)+sent,z);if(wrote>z)return false;sent+=wrote;}
  if(sent<n+1)delay(2);
 }
 return sent==n+1;
}
static void reply(const halo_diag_auth::Request*request,const char*status,const halo_diag_auth::Capacity&c,const char*proof){
 StaticJsonDocument<768> doc;doc["type"]="OTA_DIAG_AUTH_ACK";doc["nonce"]=request?request->nonce:"";doc["key_id"]=request?request->key_id:"";doc["status"]=status;
 doc["floor_entries"]=halo_diag_auth::FLOOR;doc["stats_known"]=c.stats_known;doc["free_entries"]=c.free_entries;doc["available_entries"]=c.available_entries;doc["missing_profile"]=c.missing_profile;
 doc["policy_reserve"]=c.policy_reserve;doc["breadcrumb_reserve"]=c.breadcrumb_reserve;doc["auth_entries"]=c.auth_entries;doc["required_entries"]=c.required_entries;doc["proof"]=proof;
 (void)emit(doc);
}
static bool idle(){
#if HALO_OTA_ONE_SHOT && HALO_DURABLE_OTA_POLICY
 if(sense_one_shot::active_exchange)return false;
#endif
 return !sense_policy::work.live&&!halo_primary_user_work_busy()&&nvs_capacity_image_valid()&&!xPortInIsrContext()&&
 !g_ota_apply_in_progress&&!g_lcd_ota_proxy_owns_uart&&!g_lcd_ota_task_running&&!g_spool_owns_uart&&!g_img_spool_tx_active&&
 !g_boot_ota_pending&&!g_peer_gate.active&&!g_ota_check_in_progress&&!g_ota_check_requested&&!sense_action_inflight()&&!uncertain;
}
static void install(const halo_diag_auth::Request&r){
 halo_diag_auth::Capacity capacity;const char*status="DECLINED";char confirmation[65]{};
#if HALO_DIAG_AUTH_PROVISIONING
 const uint32_t started=millis();constexpr uint32_t budget=10000;char actual_id[16]{};
 if(attempted||!public_id(actual_id,sizeof(actual_id))||strcmp(r.key_id,actual_id)||r.sense_boot!=g_coord_sense_boot_id||!idle()||!nvs_capacity_image_valid()){reply(&r,status,capacity,confirmation);return;}
 char challenge[40];snprintf(challenge,sizeof(challenge),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
 if(!sense_lcd_ota_query_start(challenge,budget)){reply(&r,status,capacity,confirmation);return;}
 struct QueryEnd{const char*id;~QueryEnd(){if(!strcmp(id,s_lcd_query_requested_id))s_lcd_query_pending=false;}} cleanup{challenge};
 LcdOtaQuerySnapshot peer{};bool ready=false;
 while(uint32_t(millis()-started)<budget){pump_uart_rx_once();if(uint32_t(millis()-started)>=budget)break;const auto q=sense_lcd_ota_query_poll(peer);if(q==LCD_QUERY_READY){ready=true;break;}if(q==LCD_QUERY_TIMEOUT)break;delay(10);}
 const bool peer_ok=ready&&peer.correlated&&peer.peer_boot_id==r.peer_boot&&peer.boot_ready&&!strcmp(peer.running_state,"VALID")&&
  peer.running_part[0]&&strcmp(peer.running_part,"?")&&!strcmp(peer.running_part,peer.boot_part)&&!peer.coord_waiting&&!peer.coord_lease_ms&&!peer.coord_owner[0];
 const uint32_t elapsed=uint32_t(millis()-started);
 if(peer_ok&&elapsed<budget&&idle()&&r.sense_boot==g_coord_sense_boot_id){
  attempted=true;SenseDiagnosticScope scope(budget-elapsed);
  if(scope.active){Lease l;halo_diag_auth::Storage storage(guard(l));const auto result=storage.install(r.key,capacity);
   if(result==halo_diag_auth::Result::Installed){if(halo_diag_auth::proof(r,confirmation))status="INSTALLED";else{status="UNKNOWN";uncertain=true;}}
   else if(result==halo_diag_auth::Result::Unknown){status="UNKNOWN";uncertain=true;}
  }
 }
#endif
 reply(&r,status,capacity,confirmation);
}
// This gate runs BEFORE the ordinary USB logger/parser. The strict secret frame
// never enters ArduinoJson. A zero-copy sanitized parse classifies all failed
// candidates; every malformed JSON line is consumed without raw head/tail logs.
static bool usb_line(const char*raw,size_t length){
 halo_diag_auth::Request r;if(halo_diag_auth::parse(raw,length,r)){install(r);return true;}
 struct Copy {char bytes[513]{};~Copy(){halo_admission::wipe(bytes,sizeof(bytes));}} copy;
 if(length>512){reply(nullptr,"DECLINED",{},"");return true;}memcpy(copy.bytes,raw,length);
 StaticJsonDocument<512> doc;const auto error=deserializeJson(doc,copy.bytes);
 if(error){Serial.printf("[OTA_DIAG_AUTH] malformed_usb_json bytes=%u\n",unsigned(length));return true;}
 const char*type=doc["type"]|"";
 if(!strcmp(type,"OTA_DIAG_AUTH")||doc.containsKey("key_hex")){reply(nullptr,"DECLINED",{},"");return true;}
 return false;
}
} // namespace sense_diag_auth
extern "C" bool halo_diag_b1_load_credentials(char*id,size_t id_n,uint8_t*key,size_t key_n){return sense_diag_auth::load(id,id_n,key,key_n);}
static bool halo_diag_auth_usb_line(const char*raw,size_t n){return sense_diag_auth::usb_line(raw,n);}
#endif

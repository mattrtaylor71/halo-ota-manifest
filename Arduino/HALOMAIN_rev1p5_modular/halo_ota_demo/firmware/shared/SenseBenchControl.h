#pragma once
#include "DurableOtaBench.h"
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
// Explicit externally driven fixture management on the existing main task.
// Serial output yields under one deadline; a short write never means delivery.
namespace sense_bench {
inline bool emit(JsonDocument&doc){
  char line[512];const char prefix[]="[OTA_BENCH] ";memcpy(line,prefix,sizeof(prefix)-1);
  size_t n=serializeJson(doc,line+sizeof(prefix)-1,sizeof(line)-sizeof(prefix)-1);
  if(!n||doc.overflowed()||n>=sizeof(line)-sizeof(prefix)-2)return false;
  n+=sizeof(prefix)-1;line[n++]='\n';const uint32_t started=millis();size_t sent=0;
  while(sent<n&&uint32_t(millis()-started)<2000){
    const int available=Serial.availableForWrite();
    if(Serial&&available>0){size_t count=n-sent;if(count>64)count=64;if(count>size_t(available))count=size_t(available);
      const size_t wrote=Serial.write(reinterpret_cast<const uint8_t*>(line)+sent,count);
      if(wrote>count)return false;sent+=wrote;}
    if(sent<n)delay(2);
  }
  return sent==n;
}
inline void hex(const uint8_t*bytes,size_t n,char*out){static const char h[]="0123456789abcdef";for(size_t i=0;i<n;++i){out[2*i]=h[bytes[i]>>4];out[2*i+1]=h[bytes[i]&15];}out[2*n]=0;}
inline bool command(JsonDocument&input){
  const auto d=input.as<JsonObjectConst>();const char*op=d["op"]|"";const uint32_t id=d["msg_id"]|uint32_t(0);
  StaticJsonDocument<640> reply;reply["op"]=op;reply["msg_id"]=id;reply["boot"]=g_coord_sense_boot_id;
  auto refused=[&](const char*why){reply["result"]=why;(void)emit(reply);return false;};
#if HALO_OTA_BENCH_PROFILE && HALO_DURABLE_OTA_POLICY && HALO_OTA_ONE_SHOT
  const uint32_t started=millis();constexpr uint32_t budget=10000;
  if(!d||!d["ver"].is<uint32_t>()||d["ver"].as<uint32_t>()!=PROTOCOL_VERSION||
     !d["msg_id"].is<uint32_t>()||!d["ts"].is<uint32_t>()||!d["type"].is<const char*>()||
     strcmp(d["type"],"OTA_BENCH")||!d["op"].is<const char*>())return refused("schema");
  if(!sense_policy::load_state(started,budget))return refused("storage");
  const auto*r=sense_policy::current();
  if(!strcmp(op,"read")){
    if(d.size()!=6||!d["off"].is<uint32_t>())return refused("schema");
    const uint32_t off=d["off"];if(off>640||off%128)return refused("offset");
    if(!r){if(!sense_policy::absent())return refused("storage");reply["gen"]=0;return refused("absent");}
    if(!durable_ota::encode(*r,sense_policy::state_scratch))return refused("codec");
    char data[173];size_t length=0;if(mbedtls_base64_encode(reinterpret_cast<unsigned char*>(data),sizeof(data),&length,sense_policy::state_scratch+off,128)||length!=172)return refused("codec");data[length]=0;
    reply["gen"]=r->generation;reply["off"]=off;reply["len"]=128;reply["crc"]=durable_ota::crc32(sense_policy::state_scratch,764);reply["data"]=data;
    return emit(reply);
  }
  const bool stop=!strcmp(op,"stop"),abort=!strcmp(op,"abort"),configure=!strcmp(op,"configure");
  if((!stop&&!abort&&!configure)||d.size()!=(stop?9:13)||!d["gen"].is<uint32_t>()||
     !d["campaign"].is<const char*>()||!d["sha256"].is<const char*>()||!d["session"].is<const char*>())return refused("schema");
  uint8_t campaign[16],expected_sha[32];durable_ota::BenchProfile requested{};
  if(!sense_one_shot::unhex(d["campaign"],campaign,16)||!sense_one_shot::unhex(d["sha256"],expected_sha,32)||
     !sense_one_shot::unhex(d["session"],requested.session,8)||!durable_ota::nonzero(requested.session,8))return refused("schema");
  requested.state=durable_ota::BenchState::ACTIVE;
  if(!stop){
    if(!d["until"].is<uint32_t>()||!d["initial_s"].is<uint16_t>()||!d["retry_s"].is<uint16_t>()||!d["defer_s"].is<uint16_t>())return refused("schema");
    requested.until=d["until"];requested.initial_s=d["initial_s"];requested.retry_s=d["retry_s"];requested.defer_s=d["defer_s"];
  }
  if(sense_one_shot::active_exchange||sense_policy::work.live||!sense_one_shot::idle()||g_boot_ota_pending||g_peer_gate.active||g_ota_check_in_progress||g_ota_check_requested||sense_action_inflight())return refused("busy");
  uint32_t prior_crc=0;
  if(r){
    if(d["gen"].as<uint32_t>()!=r->generation||memcmp(campaign,r->campaign,16)||!durable_ota::encode(*r,sense_policy::state_scratch))return refused("preimage");
    uint8_t actual_sha[32];if(mbedtls_sha256(sense_policy::state_scratch,768,actual_sha,0)||memcmp(actual_sha,expected_sha,32))return refused("preimage");
    prior_crc=durable_ota::crc32(sense_policy::state_scratch,764);
    if(stop&&memcmp(requested.session,r->bench.session,8))return refused("session");
  }else if(!sense_policy::absent()||!configure||d["gen"].as<uint32_t>()||durable_ota::nonzero(campaign,16)||durable_ota::nonzero(expected_sha,32))return refused("preimage");
  const uint32_t generation=r?r->generation:0;
  // One fresh matched query; no borrowed owner, repeated request or reopened deadline.
  char challenge[40];snprintf(challenge,sizeof(challenge),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
  if(uint32_t(millis()-started)>=budget||!sense_lcd_ota_query_start(challenge,budget-uint32_t(millis()-started)))return refused("query");
  struct QueryEnd{const char*id;~QueryEnd(){if(!strcmp(id,s_lcd_query_requested_id))s_lcd_query_pending=false;}} cleanup{challenge};
  LcdOtaQuerySnapshot peer{};bool ready=false;
  while(uint32_t(millis()-started)<budget){pump_uart_rx_once();if(uint32_t(millis()-started)>=budget)break;
    const auto status=sense_lcd_ota_query_poll(peer);if(status==LCD_QUERY_READY){ready=true;break;}if(status==LCD_QUERY_TIMEOUT)break;delay(10);}
  r=sense_policy::current();
  const bool peer_ok=ready&&peer.correlated&&peer.peer_boot_id&&peer.boot_ready&&!strcmp(peer.running_state,"VALID")&&
    peer.running_part[0]&&strcmp(peer.running_part,"?")&&!strcmp(peer.running_part,peer.boot_part)&&
    !peer.coord_waiting&&!peer.coord_lease_ms&&!peer.coord_owner[0];
  if(!peer_ok||!sense_one_shot::idle()||uint32_t(millis()-started)>=budget||(r?r->generation:0)!=generation)return refused("peer_or_deadline");
  durable_ota::Record candidate{};const auto clock=sense_policy::fresh_clock();
  const bool accepted=stop?(r&&durable_ota::bench_stop(*r,clock,sense_policy::shipping_next_normal_epoch(),prior_crc,true,nvs_capacity_image_valid(),true,true,candidate)):
    durable_ota::bench_configure(r,clock,requested,abort,true,nvs_capacity_image_valid(),true,true,prior_crc,candidate);
  if(!accepted)return refused("admission");
  if(!sense_policy::commit_candidate(candidate,started,budget,!r))return refused("storage");
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  if(generation)sense_diag_close_campaign(campaign,candidate.origin);
#endif
  r=sense_policy::current();if(!r)return refused("storage");
  char session[17];hex(r->bench.session,8,session);reply["result"]="committed";reply["gen"]=r->generation;reply["session"]=session;
  reply["until"]=r->bench.until;reply["abort_crc"]=r->bench.abort_crc;reply["abort_gen"]=r->bench.abort_generation;reply["abort_count"]=r->bench.abort_count;reply["peer_boot"]=peer.peer_boot_id;
  // Delivery loss does not undo the checked transaction. Host must re-read.
  return emit(reply);
#else
  return refused("disabled");
#endif
}
inline void append_report(JsonDocument&doc){
#if HALO_DURABLE_OTA_POLICY
 const auto*r=sense_policy::current();if(!r||r->bench.state==durable_ota::BenchState::NONE)return;
 char session[17];hex(r->bench.session,8,session);doc["ota_bench_session"]=session;doc["ota_bench_state"]=uint8_t(r->bench.state);
 doc["ota_bench_until"]=r->bench.until;doc["ota_bench_initial_s"]=r->bench.initial_s;doc["ota_bench_retry_s"]=r->bench.retry_s;doc["ota_bench_defer_s"]=r->bench.defer_s;
 doc["ota_bench_abort_crc"]=r->bench.abort_crc;doc["ota_bench_abort_gen"]=r->bench.abort_generation;doc["ota_bench_abort_count"]=r->bench.abort_count;
#endif
}
} // namespace sense_bench

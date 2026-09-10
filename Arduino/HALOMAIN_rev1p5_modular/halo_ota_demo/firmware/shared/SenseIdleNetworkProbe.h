#pragma once
#if HALO_IDLE_NETWORK_PROBE && HALO_IDLE_NETWORK_RECOVERY && HALO_OTA_BENCH_PROFILE && HALO_DURABLE_OTA_POLICY && HALO_DURABLE_DIAGNOSTICS && HALO_DIAGNOSTIC_ADMISSION
#include "IdleNetworkProbe.h"
namespace sense_idle_probe {
static bool attempted=false;
static constexpr uint32_t TOTAL_MS=20000, PREPARE_MS=10000;
static bool identity(const halo_idle_probe::Request&r,bool active){
 const auto*p=sense_policy::current();
 return r.boot==g_coord_sense_boot_id&&p&&p->generation==r.generation&&!memcmp(p->bench.session,r.session,8)&&
  p->phase==durable_ota::Phase::RESOLVED&&(!active||durable_ota::bench_live(*p,sense_policy::fresh_clock()));
}
static bool ready(const halo_idle_probe::Request&r){return identity(r,true)&&sense_diag_auth::idle()&&halo_idle_network_safe()&&
 halo_idle_network_fresh()&&!esp_cpu_dbgr_is_attached()&&!sense_idle_network::session.blocked_boot&&!sense_idle_network::window_deadline;}
static bool reply(const halo_idle_probe::Request*r,const char*status){
 StaticJsonDocument<768> doc;doc["type"]="OTA_IDLE_NET_ACK";doc["nonce"]=r?r->nonce:"";doc["status"]=status;doc["boot"]=g_coord_sense_boot_id;
 doc["reset"]=uint8_t(esp_reset_reason());doc["fresh"]=halo_idle_network_fresh();doc["decision"]=sense_idle_network::last_decision;
 const auto*p=sense_policy::current();char session[17]{};if(p)sense_bench::hex(p->bench.session,8,session);
 doc["gen"]=p?p->generation:0;doc["session"]=session;
 halo_idle_net::Record saved;const bool valid=halo_idle_net::decode(sense_idle_network::retained,saved);doc["rtc_valid"]=valid;doc["cooldown_s"]=valid?saved.cooldown:0;
 char raw[65]{};if(valid)sense_bench::hex(reinterpret_cast<const uint8_t*>(&sense_idle_network::retained),32,raw);doc["rtc"]=raw;
 // At most512B including LF, emitted in partial <=64B writes under its2s bound.
 return sense_diag_auth::emit(doc);
}
static void command(const halo_idle_probe::Request&r){
 const uint32_t started=millis(),deadline=started+TOTAL_MS;
 if(!sense_policy::load_state(started,PREPARE_MS)||!identity(r,false)){reply(&r,"identity");return;}
 // Read is local only; it cannot subscribe a watchdog, query a peer, change a
 // cooldown, or mutate policy. RTC boot processing already happened in setup.
 if(!r.probe){reply(&r,"observed");return;}
 if(attempted||!ready(r)||uint32_t(millis()-started)>=PREPARE_MS){reply(&r,"declined");return;}
 char challenge[40];snprintf(challenge,sizeof(challenge),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
 bool peer_ok=false;
 {const uint32_t elapsed=uint32_t(millis()-started);
  if(elapsed>=PREPARE_MS||!sense_lcd_ota_query_start(challenge,PREPARE_MS-elapsed)){reply(&r,"query");return;}
  struct QueryEnd{const char*id;~QueryEnd(){if(!strcmp(id,s_lcd_query_requested_id))s_lcd_query_pending=false;}} cleanup{challenge};
  LcdOtaQuerySnapshot peer{};bool observed=false;
  while(uint32_t(millis()-started)<PREPARE_MS){pump_uart_rx_once();if(uint32_t(millis()-started)>=PREPARE_MS)break;
   const auto status=sense_lcd_ota_query_poll(peer);if(status==LCD_QUERY_READY){observed=true;break;}if(status==LCD_QUERY_TIMEOUT)break;delay(10);}
  peer_ok=observed&&peer.correlated&&peer.peer_boot_id==r.peer_boot&&peer.boot_ready&&!strcmp(peer.running_state,"VALID")&&
   peer.running_part[0]&&strcmp(peer.running_part,"?")&&!strcmp(peer.running_part,peer.boot_part)&&peer.part_size&&!strcmp(peer.fw,kFirmwareVersion)&&
   !peer.coord_waiting&&!peer.coord_lease_ms&&!peer.coord_owner[0];
 }
 if(!peer_ok||!ready(r)||uint32_t(millis()-started)>=PREPARE_MS){reply(&r,"peer_or_deadline");return;}
 // One intent per boot, latched before reply delivery. Uncertain delivery or a
 // failed later subscription must never cause an automatic command replay.
 attempted=true;
 if(!reply(&r,"accepted"))return;
 if(!ready(r)||sense_idle_network::left(deadline)<halo_idle_net::REQUIRED_MS){reply(&r,"changed");return;}
 bool active=false;
 {sense_idle_network::Window window(deadline);sense_idle_network::Guard guard(halo_idle_net::Op::Probe,deadline,true);active=guard.active();
  // This deliberately yields so the idle task continues. It never feeds the
  // subscribed user, calls HTTP, resets the chip, or writes policy/keys/budgets.
  //5s is detection opportunity, not a guarantee of completed reset by20s.
  // Only this checked ACTIVE bench probe receives a180s retained cooldown.
  // Every actual HTTP operation and shipping build retains21600s.
  if(active)while(const uint32_t left=sense_idle_network::left(deadline))delay(left<20?left:20);
 }
 // Only reachable if the guard declined or the real watchdog did not reset us.
 // Removal/RETURNED ordering is exactly the normal optional-network guard.
 reply(&r,active?"no_reset":"guard_declined");
}
static bool usb_line(const char*raw,size_t n){
 halo_idle_probe::Request r;if(halo_idle_probe::parse(raw,n,r)){command(r);return true;}
 // Auth gate has already redacted/consumed malformed or secret-bearing input.
 // Invalid probe JSON must not fall through into normal command dispatch.
 StaticJsonDocument<512> doc;if(!deserializeJson(doc,raw,n)&&!strcmp(doc["type"]|"","OTA_IDLE_NET")){reply(nullptr,"schema");return true;}
 return false;
}
} // namespace sense_idle_probe
static bool halo_idle_probe_usb_line(const char*raw,size_t n){return sense_idle_probe::usb_line(raw,n);}
#endif

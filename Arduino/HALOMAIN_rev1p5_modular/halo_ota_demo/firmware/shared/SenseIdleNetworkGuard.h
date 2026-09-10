#pragma once
#include "IdleNetworkRecovery.h"
#include <esp_task_wdt.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_cpu.h>
#include <sdkconfig.h>
#ifndef HALO_IDLE_NETWORK_RECOVERY
#define HALO_IDLE_NETWORK_RECOVERY 0
#endif
static bool halo_idle_network_safe();
static uint32_t halo_idle_network_boot_id();
static bool halo_idle_network_fresh();
static uint32_t halo_idle_network_epoch();
namespace sense_idle_network {
#if HALO_IDLE_NETWORK_RECOVERY
static_assert(CONFIG_ESP_TASK_WDT_INIT&&CONFIG_ESP_TASK_WDT_PANIC&&CONFIG_ESP_TASK_WDT_TIMEOUT_S==5,"reviewed WDT configuration required");
RTC_NOINIT_ATTR static halo_idle_net::Retained retained;
static halo_idle_net::Session session;
#endif
static uint32_t window_deadline=0;
static uint8_t last_decision=0,last_operation=0;
static uint32_t last_left=0;
static constexpr uint32_t REPORT_RESERVE_MS=HALO_IDLE_NETWORK_RECOVERY?6000:OTA_REPORT_HTTP_TIMEOUT_MS;
static constexpr uint32_t POST_ADMISSION_MS=HALO_IDLE_NETWORK_RECOVERY?halo_idle_net::REQUIRED_MS:500;
struct Window {
  explicit Window(uint32_t deadline){window_deadline=deadline;}
  ~Window(){window_deadline=0;}
};
static void boot(){
#if HALO_IDLE_NETWORK_RECOVERY
  session.begin(retained,halo_idle_network_boot_id(),uint8_t(esp_reset_reason()),
#if HALO_IDLE_NETWORK_PROBE && HALO_OTA_BENCH_PROFILE && HALO_DURABLE_OTA_POLICY && HALO_DURABLE_DIAGNOSTICS && HALO_DIAGNOSTIC_ADMISSION
    true
#else
    false
#endif
  );
#endif
}
static uint32_t left(uint32_t deadline){const int32_t n=int32_t(deadline-millis());return n>0?uint32_t(n):0;}
static void note(halo_idle_net::Op op,halo_idle_net::Why why,uint32_t deadline){last_operation=uint8_t(op);last_decision=uint8_t(why);last_left=left(deadline);}
#if HALO_IDLE_NETWORK_RECOVERY
// One finite enrolment acknowledgement closes ordinary residual-cycle exposure
// after success. No request/cleanup loop feeds this user. SDK public calls still
// have an add->reset ISR gap; other overdue subscribers remain effective and
// this user's last-resort timeout can span nearly two global five-second cycles.
static bool add_user_acknowledged(void**out){
  *out=nullptr;esp_task_wdt_user_handle_t u=nullptr;
  if(esp_task_wdt_add_user("idle_http",&u)!=ESP_OK)return false;
  if(esp_task_wdt_reset_user(u)!=ESP_OK){
    // No caller may proceed with an unacknowledged or leaked subscription.
    // The pure Guard has already marked ACTIVE; keep it if cleanup fails.
    if(esp_task_wdt_delete_user(u)!=ESP_OK){esp_restart();for(;;)delay(1000);}
    return false;
  }
  *out=u;return true;
}
static halo_idle_net::Port port(){return {nullptr,
 [](void*){return uint32_t(millis());},[](void*){return halo_idle_network_safe()&&!esp_cpu_dbgr_is_attached();},
 [](void*,void**out){return add_user_acknowledged(out);},
 [](void*,void*u){return esp_task_wdt_delete_user(static_cast<esp_task_wdt_user_handle_t>(u))==ESP_OK;},
 [](void*){esp_restart();for(;;)delay(1000);}};}
struct Guard {
  halo_idle_net::Guard owned;
  Guard(halo_idle_net::Op op,uint32_t deadline,bool short_probe=false):owned(port(),retained,session,op,
      !window_deadline?uint32_t(millis()):(left(window_deadline)<left(deadline)?window_deadline:deadline),
      halo_idle_network_fresh(),halo_idle_network_epoch(),
#if HALO_IDLE_NETWORK_PROBE && HALO_OTA_BENCH_PROFILE && HALO_DURABLE_OTA_POLICY && HALO_DURABLE_DIAGNOSTICS && HALO_DIAGNOSTIC_ADMISSION
      short_probe
#else
      false
#endif
      ){
    note(op,owned.why,deadline);
  }
  bool active()const{return window_deadline&&owned.active();}
};
#else
struct Guard {Guard(halo_idle_net::Op,uint32_t){}bool active()const{return true;}};
#endif
template<class J> static void append(J&doc){
#if HALO_IDLE_NETWORK_RECOVERY
  doc["idle_net_decision"]=last_decision;doc["idle_net_op"]=last_operation;doc["idle_net_left_ms"]=last_left;
  halo_idle_net::Record r;if(halo_idle_net::decode(retained,r)){
    doc["idle_net_state"]=uint32_t(r.state);doc["idle_net_prior_boot"]=r.boot;
    doc["idle_net_reset"]=r.reset;doc["idle_net_until"]=r.until;
  }
#endif
}
} // namespace sense_idle_network

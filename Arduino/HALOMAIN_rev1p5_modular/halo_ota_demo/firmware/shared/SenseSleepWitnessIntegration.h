#pragma once
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
#include "SenseSleepAttestation.h"
// RTC is consumed on every boot, including software/cold resets. It cannot
// authorize a schedule, reset a quota, or extend a work deadline.
namespace sense_sleep_witness {
RTC_NOINIT_ATTR static uint8_t retained[halo_sense_sleep::BYTES];
static halo_sense_sleep::Observation observed{};
static bool boot_consumed=false;
static bool hash(const uint8_t*p,size_t n,uint8_t*out){return mbedtls_sha256(p,n,out,0)==0;}
static_assert(ESP_RST_DEEPSLEEP==8&&ESP_SLEEP_WAKEUP_EXT0==2&&ESP_SLEEP_WAKEUP_TIMER==4,"reviewed reset/wake enums");
static void sent(const MaintenanceWindow*mw,uint32_t remaining,uint32_t wake,bool clear,
                 uint32_t peer,bool carried_boot) {
  if(!boot_consumed)return; // do not destroy predecessor before boot consume
  if(!carried_boot||clear||!mw||!sense_time_has_fresh_sync()||mw->start_epoch>UINT32_MAX){
    halo_sense_sleep::cancel(retained);return;
  }
  // A caller-supplied peer is from the real coordinated transaction. Ordinary
  // calendar sends may use only a recently received existing diagnostic INFO.
  if(!peer&&g_diag_peer_qualification_boot&&
     uint32_t(millis()-g_diag_peer_qualification_ms)<2000)peer=g_diag_peer_qualification_boot;
  halo_maintenance_witness::Window w{};w.prior_lcd=peer;w.prior_sense=g_coord_sense_boot_id;
  w.armed=(wake||remaining)?1:0;w.wake_s=wake?wake:remaining;w.remaining_s=remaining;
  w.start_epoch=uint32_t(mw->start_epoch);w.duration_s=mw->duration_sec;
  w.grace_before_s=mw->grace_before_sec;w.grace_after_s=mw->grace_after_sec;
  if(strnlen(mw->request_id,sizeof(mw->request_id))>=sizeof(w.request)){
    halo_sense_sleep::cancel(retained);return;
  }
  strlcpy(w.request,mw->request_id,sizeof(w.request));
  (void)halo_sense_sleep::stage(retained,w,hash);
}
static void boot() {
  if(boot_consumed)return;
  observed=halo_sense_sleep::consume(retained,g_coord_sense_boot_id,
    unsigned(esp_reset_reason()),unsigned(esp_sleep_get_wakeup_cause()));
  boot_consumed=true;
}
static void enter() {
  if(!boot_consumed||!g_diag_timer_observed||g_diag_timer_selected_epoch>UINT32_MAX){
    halo_sense_sleep::cancel(retained);return;
  }
  (void)halo_sense_sleep::commit_entry(retained,g_coord_sense_boot_id,
    uint32_t(g_diag_timer_selected_epoch),g_diag_timer_sdk,g_diag_timer_us);
}
static bool peer_current(uint32_t peer) {
  return peer&&g_peer_gate.active&&g_peer_gate.entered&&g_peer_gate.locked&&g_peer_gate.ready&&
    !g_peer_gate.legacy&&g_peer_gate.peer_boot==peer&&uint32_t(millis()-g_peer_gate.proof_ms)<2000;
}
static bool read(uint32_t due,uint32_t generation,uint32_t ordinal,const uint8_t(&context)[32],
                 halo_sleep_attestation::Proof&proof) {
  proof={};const uint32_t peer=g_peer_gate.peer_boot;
  if(!peer_current(peer)||observed.quality!=halo_sense_sleep::Quality::DeepSleepPredecessor||
     observed.current_sense!=g_coord_sense_boot_id||observed.record.due!=due||
     !sense_diag_safe(nullptr)||!sense_policy::remaining())return false;
  const uint32_t elapsed=millis()-g_diag_started_ms;
  if(elapsed>=g_diag_budget_ms)return false;
  uint32_t left=g_diag_budget_ms-elapsed;const uint32_t work=sense_policy::remaining();
  if(left>work)left=work;
  if(left<=50)return false;
  const uint32_t budget=(left-50)<250?(left-50):250,deadline=millis()+budget;
  SenseDiagnosticReply reply;
  // One optional read, no retry. It consumes the existing diagnostic/work
  // allowance and never changes peer health/admission state on any outcome.
  if(!sense_diag_exchange("sleep",peer,nullptr,0,nullptr,0,0,deadline,reply)||
     reply.result!=halo_diag::Result::Ok||reply.slot||reply.offset||reply.total!=72||reply.size!=72||
     !peer_current(peer)||!sense_diag_safe(nullptr)||!sense_policy::remaining())return false;
  const auto&raw=*reinterpret_cast<const uint8_t(*)[72]>(reply.bytes);
  static_assert(durable_ota::kPeerLead==15,"actual LCD maintenance lead");
  return halo_sleep_attestation::Proof::from_receipt(raw,reply.sw_reset,reply.sw_wake,reply.sw_quality,
    peer,observed,g_coord_sense_boot_id,due,generation,ordinal,context,15,proof);
}
} // namespace sense_sleep_witness
static void halo_sleep_witness_sent(const MaintenanceWindow*m,uint32_t r,uint32_t w,bool c,uint32_t p,bool b){sense_sleep_witness::sent(m,r,w,c,p,b);}
static void halo_sleep_witness_boot(){sense_sleep_witness::boot();}
static void halo_sleep_witness_enter(){sense_sleep_witness::enter();}
#endif

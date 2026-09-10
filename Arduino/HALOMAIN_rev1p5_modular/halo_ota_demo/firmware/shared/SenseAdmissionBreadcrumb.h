#pragma once
#ifndef HALO_DIAGNOSTIC_ADMISSION
#define HALO_DIAGNOSTIC_ADMISSION 0
#endif
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
#include "DiagnosticAdmissionAuthority.h"
#include "DiagnosticAdmissionNvs.h"

// Existing synchronous main-task admission callback. It borrows the policy
// scratch only while no policy/Journal call is on the stack. No normal capture
// flag, policy allowance, task or new storage lease is introduced here.
namespace sense_admission {
static bool uncertain=false,capture_attempted=false;
struct Capture {
  const halo_diag::Context* expected;
  halo_admission::Observation observed;
  bool held=false;
};
static bool safe(Capture& c) {
  return c.expected&&!uncertain&&sense_diag_safe(nullptr)&&nvs_capacity_image_valid()&&
    sense_policy::work.live&&!sense_policy::work.finished&&
    sense_policy::remaining()&&sense_time_has_fresh_sync();
}
static bool hash(const uint8_t*p,size_t n,uint8_t*out) {
  return mbedtls_sha256(p,n,out,0)==0;
}
static bool read_blob(Capture&c,nvs_handle_t h,const char*key,uint8_t*out,size_t expected,size_t&n,bool optional=false) {
  n=0;if(!c.held||!safe(c))return false;
  auto e=nvs_get_blob(h,key,nullptr,&n);
  if(e==ESP_ERR_NVS_NOT_FOUND){n=0;return optional;}
  if(e!=ESP_OK){uncertain=true;return false;}
  if(n!=expected)return false;
  size_t got=n;e=nvs_get_blob(h,key,out,&got);
  if(e!=ESP_OK){uncertain=true;return false;}
  return got==n&&safe(c);
}
static bool authority(void*arg,const halo_admission::Record& record) {
  auto&c=*static_cast<Capture*>(arg);const auto* p=sense_policy::current();
  if(!c.held||!safe(c)||!p||p->phase!=durable_ota::Phase::PREFLIGHT)return false;
  auto&scratch=sense_policy::state_scratch;
  nvs_handle_t h=0;if(nvs_open(durable_ota::kPolicyNamespace,NVS_READONLY,&h)!=ESP_OK)return false;
  size_t n=0;const bool loaded=read_blob(c,h,durable_ota::kPolicyKey,scratch,sizeof(scratch),n);nvs_close(h);
  // Exact canonical comparison includes CRC and every otherwise unreported
  // policy byte. This proves the already-decoded checked current() is still the
  // actual stored record; no second 732B RAM Record or allowance is created.
  if(!loaded||!durable_ota::encode(*p,scratch,true)||!safe(c))return false;
  if(nvs_open("ota_diag",NVS_READONLY,&h)!=ESP_OK)return false;
  auto&ctx=*reinterpret_cast<uint8_t(*)[256]>(scratch);
  auto&state=*reinterpret_cast<uint8_t(*)[256]>(scratch+256);
  size_t n0=0,n1=0;
  const bool loaded_diag=read_blob(c,h,"ctx",ctx,256,n)&&
    read_blob(c,h,"state",state,256,n)&&
    read_blob(c,h,"cp0",scratch+512,64,n0,true)&&
    read_blob(c,h,"cp1",scratch+576,64,n1,true);
  nvs_close(h);
  return loaded_diag&&safe(c)&&halo_admission::eligible(record,c.observed,*p,*c.expected,
    ctx,state,scratch+512,n0,scratch+576,n1,hash);
}
static size_t missing_profile(void*arg) {
  auto&c=*static_cast<Capture*>(arg);if(!c.held||!safe(c))return SIZE_MAX;
  nvs_handle_t h=0;if(nvs_open("ota_diag",NVS_READONLY,&h)!=ESP_OK)return SIZE_MAX;
  const char*keys[]={"ctx","fail0","fail1","state","cp0","cp1"};size_t missing=0;
  for(unsigned i=0;i<6;++i){
    if(!safe(c)){missing=SIZE_MAX;break;}
    size_t n=0;const auto e=nvs_get_blob(h,keys[i],nullptr,&n);const size_t expected=i<4?256:64;
    if(e==ESP_ERR_NVS_NOT_FOUND)missing+=2+(expected+31)/32;
    else if(e!=ESP_OK||n!=expected){missing=49;break;}
  }
  nvs_close(h);return safe(c)?missing:SIZE_MAX;
}
static bool scheduled_origin(const durable_ota::Record&p,uint32_t due) {
  if(!due||!g_boot_ota_pending||!g_boot_ota_reason)return false;
  if(!strcmp(g_boot_ota_reason,"lcd_timer")){
    char request[64]{};
    if(p.deferred_path&&durable_ota::bench_active(p)){
      // Same real selector encoding as bench_deferred_request, after its
      // checked transition to PREFLIGHT; no fabricated wire request.
      char session[17]{};static const char hex[]="0123456789abcdef";
      for(unsigned i=0;i<8;++i){session[i*2]=hex[p.bench.session[i]>>4];session[i*2+1]=hex[p.bench.session[i]&15];}
      snprintf(request,sizeof(request),"bench_%s_%lu",session,(unsigned long)p.not_before);
    }else if(p.arm_id[0]&&p.arm_epoch==due)strlcpy(request,p.arm_id,sizeof(request));
    else if(p.one_shot.due==due&&p.one_shot.phase==durable_ota::OneShotPhase::CONSUMED)
      (void)durable_ota::one_shot_request(p,request);
    return halo_policy_accepted_lcd_origin(request);
  }
  // The real calendar origin was copied from this boot's checked schedule.
  // Manual, unsupported or unmatched calendar identities remain unproved.
  return !strcmp(g_boot_ota_reason,"nightly")&&sense_policy::work.normal&&
    g_calendar_timer_origin.id[0]&&g_calendar_timer_origin.bound&&
    g_calendar_timer_origin.target_epoch==due;
}
static halo_admission::Result capture(const halo_diag::Context& expected) {
  using namespace halo_admission;
  if(capture_attempted)return Result::Declined;
  capture_attempted=true; // even a busy/late/refused opportunity is never replayed
  const auto*p=sense_policy::current();const auto clock=sense_policy::fresh_clock();
  if(!p||p->phase!=durable_ota::Phase::PREFLIGHT||!clock.fresh||!clock.epoch||
     !g_coord_sense_boot_id||!sense_policy::work.live||sense_policy::work.finished)return Result::Declined;
  const uint32_t due=p->deferred_path?p->not_before:(p->fast_due?p->fast_due:p->one_shot.due);
  Record record{};record.boot=g_coord_sense_boot_id;record.epoch=clock.epoch;record.due=due;
  record.generation=p->generation;record.ordinal=p->attempt_ordinal;
  record.reset=uint8_t(esp_reset_reason());record.wake=uint8_t(esp_sleep_get_wakeup_cause());
  record.flags=FRESH;
  if(record.reset==8&&(record.wake==2||record.wake==4)&&scheduled_origin(*p,due))record.flags|=SCHEDULED;
  auto&raw=*reinterpret_cast<uint8_t(*)[256]>(sense_policy::state_scratch);
  if(!halo_diag::encode_context(raw,expected)||!hash(raw,256,record.context_sha))return Result::Declined;
  halo_sleep_attestation::Proof peer_proof;
  if((record.flags&SCHEDULED)&&sense_sleep_witness::read(due,record.generation,record.ordinal,
       record.context_sha,peer_proof)){
    record.flags|=LCD_VERIFIED;record.lcd_prior_boot=peer_proof.prior_lcd();
  }
  Capture c{&expected,{record.boot,record.epoch,record.due,record.generation,record.ordinal,
    record.reset,record.wake,true,bool(record.flags&SCHEDULED),&peer_proof}};
  NvsGuard guard{&c,
    [](void*a){auto&x=*static_cast<Capture*>(a);if(x.held||!safe(x)||g_optional_nvs_writer.test_and_set(std::memory_order_acquire))return false;x.held=true;return true;},
    [](void*a){auto&x=*static_cast<Capture*>(a);if(x.held){x.held=false;g_optional_nvs_writer.clear(std::memory_order_release);}},
    [](void*a){return safe(*static_cast<Capture*>(a));},authority,missing_profile,
    [](void*){return true;}, // exact current policy already re-read under this lease
    [](void*){uncertain=true;}};
  NvsAdapter adapter(guard,true);Store store(adapter.port());return store.capture(record);
}
} // namespace sense_admission
static void halo_diag_admission_state_blocked(const halo_diag::Context& expected) {
  const auto result=sense_admission::capture(expected);
  Serial.printf("[OTA_DIAG_ADMISSION] result=%u\n",unsigned(result));
}
#endif

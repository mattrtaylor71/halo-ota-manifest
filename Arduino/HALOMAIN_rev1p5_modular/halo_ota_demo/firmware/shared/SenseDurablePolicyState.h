#pragma once
#include "DurableOtaDiscovery.h"
#include "DurableOtaOneShot.h"
#include "DurableOtaBench.h"
#include "SenseDurablePolicyStorage.h"

// Included by the existing production main-task wrapper after its clock,
// coordinator, health and NVS owners. No task, namespace, timer or heap object.
namespace sense_policy {
#if HALO_DURABLE_OTA_POLICY
static durable_ota::Record state_record{};
static uint8_t state_scratch[durable_ota::kRecordBytes]{};
static bool state_loaded=false,state_present=false,state_allowed=false;
static durable_ota::StorageStatus state_status=durable_ota::StorageStatus::FEATURE_OFF;
#endif
inline durable_ota::Clock fresh_clock(bool normal_maintenance=false) {
  const time_t now=time(nullptr);
  return {now>=durable_ota::kMinimumEpoch&&uint64_t(now)<=UINT32_MAX?uint32_t(now):0,
          sense_time_has_fresh_sync(),normal_maintenance};
}
inline uint32_t shipping_next_normal_epoch() {
  const auto c=fresh_clock();if(!c.fresh||!c.epoch)return 0;
  std::lock_guard<std::recursive_mutex> lock(g_time_mutex);
  const uint32_t delta=halo_seconds_until_maintenance((time_t)c.epoch);
  return delta&&uint64_t(c.epoch)+delta<=UINT32_MAX?c.epoch+delta:0;
}
inline uint32_t next_normal_epoch() {
#if HALO_DURABLE_OTA_POLICY
  const auto c=fresh_clock();
  if(state_loaded&&state_present&&state_allowed&&durable_ota::bench_live(state_record,c)&&
     uint64_t(c.epoch)+state_record.bench.defer_s<state_record.bench.until)
    return c.epoch+state_record.bench.defer_s;
#endif
  return shipping_next_normal_epoch();
}
// Failed unbound bench discovery retains its campaign and consumed budget.
// Reuse the deferred wake path only after its reservation has been settled.
inline bool bench_retry_phase(const durable_ota::Record&r){
  return r.phase==durable_ota::Phase::DEFERRED||
    (r.phase==durable_ota::Phase::DISCOVERY&&!durable_ota::active_phase(r)&&
     r.begins[0]<durable_ota::begin_cap(r)&&r.begins[1]<durable_ota::begin_cap(r));
}
inline uint32_t bench_deferred_delta(const durable_ota::Record&r,durable_ota::Clock c){
  return durable_ota::bench_live(r,c)&&durable_ota::clock_valid(r,c)&&bench_retry_phase(r)&&
    r.network_windows<durable_ota::network_cap(r)&&r.day_attempts<durable_ota::apply_cap(r)&&
    r.work_remaining_ms>=1000&&r.not_before>c.epoch&&r.not_before<r.bench.until?r.not_before-c.epoch:0;
}
inline bool bench_deferred_due(const durable_ota::Record&r,durable_ota::Clock c){
  return durable_ota::bench_live(r,c)&&durable_ota::clock_valid(r,c)&&bench_retry_phase(r)&&
    r.network_windows<durable_ota::network_cap(r)&&r.day_attempts<durable_ota::apply_cap(r)&&
    r.work_remaining_ms>=1000&&r.not_before<=c.epoch;
}
inline bool bench_deferred_request(const durable_ota::Record&r,char(&id)[64]){
  if(!durable_ota::bench_active(r)||!bench_retry_phase(r))return false;
  static const char h[]="0123456789abcdef";char session[17]{};
  for(unsigned i=0;i<8;++i){session[2*i]=h[r.bench.session[i]>>4];session[2*i+1]=h[r.bench.session[i]&15];}
  snprintf(id,sizeof(id),"bench_%s_%lu",session,(unsigned long)r.not_before);return true;
}
// A definitive absent read is retained separately from failed/invalid reads.
// Calls may retry an unsafe read after health becomes VALID; no allowance is
// loaded on error. Writes always use the original caller's monotonic budget.
inline bool load_state(uint32_t original_start,uint32_t original_budget) {
#if HALO_DURABLE_OTA_POLICY
  if(state_loaded)return state_present?state_allowed:true;
  StorageScope scope(original_start,original_budget);
  if(!scope.active())return false;
  const auto result=durable_ota::load_nvs(scope.guard(),true,state_record,
      state_allowed,state_scratch,state_status);
  if(result==durable_ota::ReadResult::ERROR)return false;
  state_loaded=true;state_present=result==durable_ota::ReadResult::PRESENT;
  return !state_present||state_allowed;
#else
  (void)original_start;(void)original_budget;return false;
#endif
}
inline const durable_ota::Record* current() {
#if HALO_DURABLE_OTA_POLICY
  return state_loaded&&state_present&&state_allowed?&state_record:nullptr;
#else
  return nullptr;
#endif
}
inline bool absent() {
#if HALO_DURABLE_OTA_POLICY
  return state_loaded&&!state_present;
#else
  return false;
#endif
}
inline bool commit_candidate(const durable_ota::Record& candidate,
                             uint32_t original_start,uint32_t original_budget,
                             bool first=false,const void* begin_owner=nullptr) {
#if HALO_DURABLE_OTA_POLICY
  if(!state_loaded || first!=!state_present || (state_present&&!state_allowed))return false;
  StorageScope scope(original_start,original_budget,begin_owner);if(!scope.active())return false;
  // The caller obtained this generation-one candidate only through an admitted
  // pure start function. Absent storage by itself never creates a candidate.
  bool allowed=first?true:state_allowed;
  const bool ok=durable_ota::commit_nvs(scope.guard(),true,candidate,state_record,
      allowed,state_scratch,state_status,first);
  state_allowed=allowed;
  if(ok)state_present=true;
  // An attempted uncertain first write may now exist; don't regain "absent"
  // allowance in this boot. The shared transaction owns its uncertainty latch.
  if(!ok&&!allowed)state_present=true;
  return ok;
#else
  (void)candidate;(void)original_start;(void)original_budget;(void)first;(void)begin_owner;return false;
#endif
}
// The old multi-key expectation is read only during first migration. Missing
// keys are distinct from type/I/O errors; a partially cleared tuple remains
// debt. Once a checked policy exists, these historical bytes cannot control it.
inline bool legacy_expectation(uint32_t original_start,uint32_t original_budget,bool& pending) {
  pending=false;
#if HALO_DURABLE_OTA_POLICY
  if(current())return true;
  if(!absent())return false;
  StorageScope scope(original_start,original_budget);if(!scope.active())return false;
  auto guard=scope.guard();if(!guard.take(guard.owner))return false;
  struct Release {durable_ota::NvsPolicyGuard&g;~Release(){g.give(g.owner);}} release{guard};
  if(!guard.read_safe(guard.owner))return false;
  nvs_handle_t h=0;const auto opened=nvs_open("ota_expect",NVS_READONLY,&h);
  if(opened==ESP_ERR_NVS_NOT_FOUND)return guard.read_safe(guard.owner);
  if(opened!=ESP_OK)return false;
  struct Close {nvs_handle_t h;~Close(){nvs_close(h);}} close{h};
  uint8_t flag=0;auto e=nvs_get_u8(h,"pending",&flag);
  if((e!=ESP_OK&&e!=ESP_ERR_NVS_NOT_FOUND)||(e==ESP_OK&&flag>1))return false;
  pending=e==ESP_OK&&flag==1;
  const char* keys[]={"exp_ver","prev_lbl"};const size_t limits[]={33,17};
  char value[33];
  for(unsigned i=0;i<2;++i){
    if(!guard.read_safe(guard.owner))return false;
    size_t n=0;e=nvs_get_str(h,keys[i],nullptr,&n);
    if(e==ESP_ERR_NVS_NOT_FOUND)continue;
    if(e!=ESP_OK||!n||n>limits[i])return false;
    size_t actual=n;e=nvs_get_str(h,keys[i],value,&actual);
    if(e!=ESP_OK||actual!=n||value[n-1]||memchr(value,0,n-1))return false;
    pending|=n>1;
  }
  uint32_t address=0;e=nvs_get_u32(h,"prev_addr",&address);
  if(e!=ESP_OK&&e!=ESP_ERR_NVS_NOT_FOUND)return false;
  pending|=e==ESP_OK&&address!=0;
  return guard.read_safe(guard.owner);
#else
  (void)original_start;(void)original_budget;return false;
#endif
}
// Coordinator debt remains separately owned. Never erase or rewrite its keys
// to admit a test or replenish a policy record.
inline bool unresolved_legacy() {
  const CoordinatorCreditState base=coord_credit_base();
  const bool debt=(base.pending.id[0]&&!base.pending_resolved)||g_coord_pending[0]||
      get_lcd_ota_due_nvs();
  return debt||g_ota_storage_uncertain||g_coord_credit_uncertain||
      g_serial_install_uncertain||g_nvs_reclaim_uncertain;
}
// Persist only the fully charged PREPARING record. A reset between target
// admission and control reservation must never expose an ordinary FAST target.
inline bool prepare_supplied_control(const durable_ota::Target& target,const char* origin,
                             const uint8_t(&campaign)[16],uint32_t due,uint32_t local_boot,
                             uint32_t original_start,uint32_t original_budget,
                             durable_ota::Clock accepted) {
#if HALO_DURABLE_OTA_POLICY
  if(!HALO_OTA_ONE_SHOT||!nvs_capacity_image_valid()||!durable_ota::target_valid(target)||
     containsDisallowedHost(target.url)||!isAllowedOtaHost(target.url)||
     compareSemver(target.version,kFirmwareVersion)<=0||!original_budget||
     original_budget>durable_ota::kPreflightMs||!load_state(original_start,original_budget))return false;
  bool old_expectation=false;
  const auto* existing=current();
  if(!(existing&&durable_ota::bench_active(*existing))&&
     (unresolved_legacy()||!legacy_expectation(original_start,original_budget,old_expectation)||old_expectation))return false;
  const auto observed=fresh_clock();
  const uint32_t spent=uint32_t(millis()-original_start);
  if(!accepted.fresh||!observed.fresh||accepted.epoch<durable_ota::kMinimumEpoch||
     observed.epoch<accepted.epoch||uint64_t(observed.epoch-accepted.epoch)>(spent+999)/1000+1||
     spent>=original_budget||uint64_t(accepted.epoch)+(existing?durable_ota::initial_delay(*existing):600)!=due)return false;
  const auto c=accepted;durable_ota::Record candidate{};
  const bool first=absent();
  if(first) {
    if(!durable_ota::start(target,origin,campaign,c,false,candidate))return false;
  } else {
    const auto* old=current();
    if(!old)return false;
    if(durable_ota::bench_active(*old)){
      if(!durable_ota::bench_new_campaign(*old,target,origin,campaign,c,candidate))return false;
    }else if(old->phase!=durable_ota::Phase::RESOLVED||
       !durable_ota::replace_target(*old,target,origin,campaign,c,true,candidate))return false;
  }
  const uint32_t elapsed=uint32_t(millis()-original_start);
  if(elapsed>=original_budget)return false;
  // This pure operation supports this specific in-place use: every old-field
  // read precedes next(), and subsequent writes use the copied grant and out.
  const uint32_t admitted_generation=candidate.generation;
  if(!durable_ota::one_shot_control_reserve(candidate,c,due,original_budget-elapsed,
       local_boot,true,true,false,candidate))return false;
  candidate.generation=admitted_generation; // both pure transitions form one persistent transaction
  return commit_candidate(candidate,original_start,original_budget,first);
#else
  (void)target;(void)origin;(void)campaign;(void)due;(void)local_boot;
  (void)original_start;(void)original_budget;(void)accepted;return false;
#endif
}
} // namespace sense_policy

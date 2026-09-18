#pragma once
#include "DurableOtaPolicy.h"
namespace durable_ota {
// First durable network reservation, before a latest-manifest GET. Only proven
// absence with no legacy debt can use this entry; caller commits before I/O.
inline bool start_discovery(const char* origin,const uint8_t(&campaign)[16],
                            Clock c,bool legacy_debt,bool busy,Record& out) {
  if(legacy_debt || busy || !c.fresh || c.epoch<kMinimumEpoch ||
      uint64_t(c.epoch)+(kPreflightMs/1000)>UINT32_MAX ||
      !origin || !*origin || strnlen(origin,64)>=64 || !nonzero(campaign,16))return false;
  Record r{};r.generation=1;r.phase=Phase::DISCOVERY;
  memcpy(r.campaign,campaign,16);memcpy(r.origin,origin,strlen(origin));
  r.created=r.high_water=r.budget_granted=r.active_started=c.epoch;
  r.budget_day=c.epoch/86400UL;r.network_windows=1;
  r.work_remaining_ms=kDailyWorkMs-kPreflightMs;r.reserved_work_ms=kPreflightMs;
  r.active_deadline=c.epoch+kPreflightMs/1000;
  if(!shape(r))return false;out=r;return true;
}
// Migration from an actually absent new record with retained old debt. Fresh
// local/peer VALID and exact retained origin are mandatory caller evidence.
// This permits only one charged read to recover immutable identity. Today's
// destructive allowance is conservatively exhausted, not invented from absence.
inline bool start_legacy_discovery(const char* retained_origin,
                                  const uint8_t(&campaign)[16],Clock c,
                                  bool legacy_debt,bool exact_origin,
                                  bool local_valid,bool peer_valid,bool busy,
                                  Record& out) {
  if(!legacy_debt || !exact_origin || !local_valid || !peer_valid)return false;
  Record r{};
  if(!start_discovery(retained_origin,campaign,c,false,busy,r))return false;
  r.deferred_path=true;r.fast_opportunities=2;r.day_attempts=1;
  r.begins[0]=r.begins[1]=2;r.work_remaining_ms=0;
  if(!shape(r))return false;out=r;return true;
}

// Only resolved or previously closed read-only discovery can discover a new
// target. Pending/quarantined/armed/active target identity remains untouched.
inline Admission reserve_discovery(const Record& old,Clock c,bool legacy_debt,
                                   bool busy,Record& out,const char* new_origin=nullptr,
                                   const uint8_t* new_campaign=nullptr,bool explicit_manual=false) {
  const bool bench=bench_active(old);
  if(!shape(old)||(bench&&old.phase!=Phase::DISCOVERY))return Admission::IDENTITY;
  if(!clock_valid(old,c))return Admission::CLOCK;
  if(bench&&!bench_live(old,c))return Admission::NOT_DUE;
  if(legacy_debt)return Admission::LEGACY;
  if(busy || active_phase(old))return Admission::BUSY;
  if(old.phase!=Phase::RESOLVED && old.phase!=Phase::DISCOVERY)return Admission::IDENTITY;
  // Each deliberate user check grants a fresh finite invocation, regardless of
  // today's previous checks. Automatic wakes, unbound legacy slow-path debt
  // and bench campaigns retain their existing cooldown and caps.
  const bool manual_discovery=explicit_manual&&!bench&&
      (old.phase==Phase::RESOLVED||!old.deferred_path);
  if(old.phase==Phase::DISCOVERY && !manual_discovery &&
      (!c.normal_maintenance || c.epoch<old.not_before))return Admission::NOT_DUE;
  if(old.phase==Phase::RESOLVED && (!new_origin || !*new_origin || strnlen(new_origin,64)>=64 ||
      !new_campaign || !nonzero(new_campaign,16) || !memcmp(new_campaign,old.campaign,16)))return Admission::IDENTITY;
  if(!next(old,c,out))return Admission::STORAGE;
  if(old.phase==Phase::RESOLVED) {
    // A newly completed-target discovery is a new campaign, not a mutation of
    // the completed target's identity. Preserve its target only for comparison.
    out.one_shot=OneShot{}; // completed campaign only
    memset(out.origin,0,sizeof(out.origin));memcpy(out.origin,new_origin,strlen(new_origin));
    memcpy(out.campaign,new_campaign,16);
  }
  if(manual_discovery) {
    replenish_manual_budget(out,c);
    // These records have no unfinished target. A newly bound update gets the
    // ordinary finite fast retry opportunities; no historical target is erased.
    out.fast_opportunities=0;out.fast_start=out.fast_due=out.fast_expiry=0;
    out.deferred_path=false;
  } else if(!bench&&c.epoch/86400UL>old.budget_day) {
    // A deliberate request may open the new day only after completed work.
    // Failed, deferred and read-only campaigns keep their original schedule/debt.
    if(!c.normal_maintenance && !(explicit_manual&&old.phase==Phase::RESOLVED))return Admission::NOT_DUE;
    out.budget_day=c.epoch/86400UL;out.budget_granted=c.epoch;
    out.network_windows=out.day_attempts=out.begins[0]=out.begins[1]=0;
    out.attempt_begins[0]=out.attempt_begins[1]=0;out.work_remaining_ms=kDailyWorkMs;
    // An unbound legacy discovery retains its slow path on later days;
    // a clock rollover never grants it two fresh fast opportunities.
    out.fast_opportunities=old.deferred_path?old.fast_opportunities:0;
    out.fast_start=out.fast_due=out.fast_expiry=0;out.deferred_path=old.deferred_path;
  }
  if(out.network_windows>=(bench?network_cap(out):(out.deferred_path?1:2)) ||
     (bench&&(out.day_attempts>=apply_cap(out)||out.begins[0]>=begin_cap(out)||out.begins[1]>=begin_cap(out))))return Admission::BUDGET;
  uint32_t grant=out.work_remaining_ms<kPreflightMs?out.work_remaining_ms:kPreflightMs;
  if(bench){const uint64_t left=uint64_t(old.bench.until-c.epoch)*1000;if(left<grant)grant=uint32_t(left);}
  if(grant<1000 || uint64_t(c.epoch)+(grant+999)/1000>UINT32_MAX)return Admission::BUDGET;
  out.phase=Phase::DISCOVERY;++out.network_windows;out.work_remaining_ms-=grant;
  out.reserved_work_ms=grant;out.active_started=c.epoch;out.active_deadline=c.epoch+(grant+999)/1000;
  out.arm_epoch=out.arm_peer_boot=0;memset(out.arm_id,0,sizeof(out.arm_id));
  return shape(out)?Admission::ALLOWED:Admission::IDENTITY;
}
// Manifest and compatible peer identity were validated in this SAME network
// reservation. Binding is not a fresh preflight and cannot refund elapsed time.
inline bool bind_discovery(const Record& old,Clock c,const Target& target,
                           bool validated,bool verified_newer,Record& out) {
  if(!next(old,c,out) || old.phase!=Phase::DISCOVERY || !active_phase(old) ||
      c.epoch>=old.active_deadline || !validated || !verified_newer || !target_valid(target))return false;
  if(target_valid(old.target)) {
    // Same already-resolved target is a no-update close, never a new apply.
    if(!verified_newer || !strcmp(old.target.version,target.version))return false;
  }
  if(!old.deferred_path && old.fast_opportunities>=2)return false;
  out.target=target;out.phase=Phase::PREFLIGHT;
  if(!old.deferred_path)++out.fast_opportunities;
  return shape(out);
}
// Conservative legacy bridge: its original readonly reservation and exhausted
// destructive allowance distinguish it from a new target discovery. Binding a
// known older manifest supplies identity only; it never authorizes a downgrade
// or reports the historical failed transfer as successful.
inline bool bind_legacy_discovery(const Record& old,Clock c,const Target& target,
                                 bool validated,bool exact_origin,bool local_valid,
                                 bool peer_valid,Record& out) {
  if(!shape(old)||old.phase!=Phase::DISCOVERY||!active_phase(old)||!old.deferred_path||
     old.fast_opportunities!=2||old.network_windows!=1||!validated||!exact_origin||!local_valid||!peer_valid||
     !target_valid(target)||c.epoch>=old.active_deadline||!next(old,c,out))return false;
  out.target=target;out.phase=Phase::PREFLIGHT;return shape(out);
}

// Both no-update and failed read-only discovery close accounting only; neither
// resolves an original target nor grants completion/calendar credit. The caller
// supplies verified no-update only after the complete pair check returns true.
// A live bench no-update settles BENCH_READY; failure remains retryable DISCOVERY.
// A crash uses reconcile_reset
// instead and receives no refund of unknown elapsed work.
inline bool close_discovery(const Record& old,Clock c,uint32_t elapsed_ms,
                            bool cleaned,uint32_t next_normal,Record& out,bool no_update=false) {
  if(!next(old,c,out) || old.phase!=Phase::DISCOVERY || !active_phase(old) ||
      !cleaned || next_normal<=c.epoch)return false;
  if(elapsed_ms<old.reserved_work_ms)out.work_remaining_ms+=old.reserved_work_ms-elapsed_ms;
  clear_active(out);out.not_before=next_normal;
  if(HALO_OTA_BENCH_PROFILE==1&&bench_active(old)&&no_update){out.phase=Phase::BENCH_READY;out.not_before=0;}
  return shape(out);
}
} // namespace durable_ota

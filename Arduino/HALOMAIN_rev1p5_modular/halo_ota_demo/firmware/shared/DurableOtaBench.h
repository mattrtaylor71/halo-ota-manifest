#pragma once
#include "DurableOtaPolicy.h"
namespace durable_ota {
// Explicit external fixture boundary. This is never a recovery/success result.
// Caller archives the exact canonical preimage and proves both boards settled,
// fresh SDK VALID and idle before invoking this checked transaction.
inline bool bench_configure(const Record* old,Clock c,const BenchProfile& requested,
                            bool abort_obligation,bool archive_bound,bool local_valid,
                            bool peer_valid,bool idle,uint32_t prior_crc,Record& out){
 if(!HALO_OTA_BENCH_PROFILE||!archive_bound||!local_valid||!peer_valid||!idle||!c.fresh||
    c.epoch<kMinimumEpoch||requested.state!=BenchState::ACTIVE||!nonzero(requested.session,8)||
    requested.until<=uint64_t(c.epoch)+120||requested.until>uint64_t(c.epoch)+kBenchLifetime||
    requested.abort_crc||requested.abort_generation||requested.abort_count)return false;
 Record r{};
 if(old){
  if(!shape(*old)||!clock_valid(*old,c)||active_phase(*old)||old->phase==Phase::ARMED||old->phase==Phase::ARM_PENDING||
     old->one_shot.phase==OneShotPhase::ARMED||old->one_shot.phase==OneShotPhase::ARM_PENDING||
     (!abort_obligation&&old->phase!=Phase::RESOLVED&&old->phase!=Phase::BENCH_ABORTED&&old->phase!=Phase::BENCH_READY)||
     !next(*old,c,r))return false;
 }else{
  if(abort_obligation)return false;
  r.generation=1;r.created=r.high_water=r.budget_granted=c.epoch;r.budget_day=c.epoch/86400UL;
  memcpy(r.campaign,requested.session,8);memcpy(r.campaign+8,requested.session,8);
  memcpy(r.origin,"bench_session",14);
 }
 const bool same_session=old&&old->bench.state==BenchState::ACTIVE&&!memcmp(old->bench.session,requested.session,8);
 if(same_session){
  if(!abort_obligation||requested.until!=old->bench.until||requested.initial_s!=old->bench.initial_s||
     requested.retry_s!=old->bench.retry_s||requested.defer_s!=old->bench.defer_s||old->bench.abort_count==255)return false;
 }else{
  // Explicit new external session grants a documented test budget. Never use
  // this transaction between the initial and retry legs of one recovery case.
  r.bench=requested;r.work_remaining_ms=kBenchWorkMs;r.network_windows=r.day_attempts=0;
  r.begins[0]=r.begins[1]=r.attempt_begins[0]=r.attempt_begins[1]=0;
  r.budget_day=c.epoch/86400UL;r.budget_granted=c.epoch;
 }
 if(old){r.bench.abort_crc=prior_crc;r.bench.abort_generation=old->generation;
  r.bench.abort_count=same_session?uint8_t(old->bench.abort_count+1):1;}
 clear_active(r);r.one_shot={};r.fast_start=r.fast_due=r.fast_expiry=0;r.fast_opportunities=0;
 r.arm_epoch=r.arm_peer_boot=0;memset(r.arm_id,0,sizeof(r.arm_id));r.deferred_path=false;
 r.phase=target_valid(r.target)?Phase::BENCH_ABORTED:Phase::BENCH_READY;
 // No transfer success, target validation or production calendar credit here.
 if(!shape(r))return false;out=r;return true;
}
inline bool bench_new_campaign(const Record& old,const Target& target,const char* origin,
                               const uint8_t(&campaign)[16],Clock c,Record& out){
 if(!bench_live(old,c)||!clock_valid(old,c)||!shape(old)||
    (old.phase!=Phase::RESOLVED&&old.phase!=Phase::BENCH_ABORTED&&old.phase!=Phase::BENCH_READY)||
    !origin||!*origin||strnlen(origin,64)>=64||!nonzero(campaign,16)||!memcmp(campaign,old.campaign,16)||
    !target_valid(target)||old.network_windows>=network_cap(old)||old.day_attempts>=apply_cap(old)||
    old.work_remaining_ms<1000||old.generation==UINT32_MAX)return false;
 // Same version may only mean the same immutable bytes after an explicit abort.
 if(target_valid(old.target)&&!strcmp(target.version,old.target.version)&&
    (old.phase!=Phase::BENCH_ABORTED||!same_target(old.target,target)))return false;
 Record r{};if(!start(target,origin,campaign,c,false,r))return false;
 r.generation=old.generation+1;r.bench=old.bench;r.work_remaining_ms=old.work_remaining_ms;
 r.network_windows=old.network_windows;r.day_attempts=old.day_attempts;
 r.begins[0]=old.begins[0];r.begins[1]=old.begins[1];r.budget_day=old.budget_day;r.budget_granted=old.budget_granted;
 if(!shape(r))return false;out=r;return true;
}
// An actual manual request may spend an explicitly configured live test
// allowance. It neither grants credit nor changes a pending target. Commit the
// returned DISCOVERY before any manifest GET, just like production discovery.
inline Admission bench_manual_discovery(const Record& old,Clock c,bool manual,
                                        bool legacy_debt,bool busy,uint32_t external_ms,
                                        const char* origin,const uint8_t(&campaign)[16],Record& out){
 if(!HALO_OTA_BENCH_PROFILE||!manual||!shape(old)||!bench_active(old))return Admission::IDENTITY;
 if(!clock_valid(old,c))return Admission::CLOCK;
 if(!bench_live(old,c))return Admission::NOT_DUE;
 if(legacy_debt)return Admission::LEGACY;
 if(busy||active_phase(old)||old.reserved_work_ms)return Admission::BUSY;
 if((old.phase!=Phase::RESOLVED&&old.phase!=Phase::BENCH_ABORTED&&old.phase!=Phase::BENCH_READY)||
    (old.one_shot.phase!=OneShotPhase::NONE&&old.one_shot.phase!=OneShotPhase::CLOSED)||
    !origin||!*origin||strnlen(origin,64)>=64||!nonzero(campaign,16)||!memcmp(campaign,old.campaign,16))return Admission::IDENTITY;
 if(old.network_windows>=network_cap(old)||old.day_attempts>=apply_cap(old)||
    old.begins[0]>=begin_cap(old)||old.begins[1]>=begin_cap(old))return Admission::BUDGET;
 uint32_t grant=old.work_remaining_ms<kPreflightMs?old.work_remaining_ms:kPreflightMs;
 if(external_ms<grant)grant=external_ms;
 const uint64_t left=uint64_t(old.bench.until-c.epoch)*1000;
 if(left<grant)grant=uint32_t(left);
 if(grant<1000||uint64_t(c.epoch)+(grant+999)/1000>UINT32_MAX)return Admission::BUDGET;
 if(old.generation==UINT32_MAX)return Admission::STORAGE;
 Record r{};r.generation=old.generation+1;r.phase=Phase::DISCOVERY;r.target=old.target;
 memcpy(r.campaign,campaign,16);memcpy(r.origin,origin,strlen(origin));
 r.created=r.high_water=r.active_started=c.epoch;r.active_deadline=c.epoch+(grant+999)/1000;
 r.bench=old.bench;r.budget_day=old.budget_day;r.budget_granted=old.budget_granted;
 r.network_windows=old.network_windows+1;r.day_attempts=old.day_attempts;
 r.begins[0]=old.begins[0];r.begins[1]=old.begins[1];
 r.work_remaining_ms=old.work_remaining_ms-grant;r.reserved_work_ms=grant;
 if(!shape(r))return Admission::IDENTITY;out=r;return Admission::ALLOWED;
}
inline bool bench_stop(const Record& old,Clock c,uint32_t next_shipping,uint32_t prior_crc,
                       bool archive_bound,bool local_valid,bool peer_valid,bool idle,Record& out){
 if(!HALO_OTA_BENCH_PROFILE||!bench_active(old)||!archive_bound||!local_valid||!peer_valid||!idle||
    active_phase(old)||old.phase==Phase::ARMED||old.phase==Phase::ARM_PENDING||
    old.one_shot.phase==OneShotPhase::ARMED||old.one_shot.phase==OneShotPhase::ARM_PENDING||
    next_shipping<=c.epoch||old.bench.abort_count==255||!next(old,c,out))return false;
 clear_active(out);out.one_shot={};out.fast_start=out.fast_due=out.fast_expiry=0;out.fast_opportunities=2;
 out.arm_epoch=out.arm_peer_boot=0;memset(out.arm_id,0,sizeof(out.arm_id));
 out.bench.state=BenchState::AUDIT_ONLY;out.bench.abort_crc=prior_crc;out.bench.abort_generation=old.generation;++out.bench.abort_count;
 // Shipping limits resume conservatively; this externally requested boundary
 // is closed discovery, never a fabricated resolution of the archived target.
 out.phase=target_valid(old.target)?Phase::DEFERRED:Phase::DISCOVERY;out.deferred_path=target_valid(old.target);out.not_before=next_shipping;
 out.network_windows=out.day_attempts=2;out.begins[0]=out.begins[1]=4;
 out.attempt_begins[0]=out.attempt_begins[1]=0;out.work_remaining_ms=0;
 out.budget_day=c.epoch/86400UL;out.budget_granted=c.epoch;
 return shape(out);
}
} // namespace durable_ota

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Pure policy and canonical codec. No task, heap, clock, NVS, UART or OTA API.
// A candidate grants nothing until the caller commits/readbacks it. A reset
// cannot refund an outstanding reservation. Diagnostic records are not input.
#ifndef HALO_OTA_ONE_SHOT
#define HALO_OTA_ONE_SHOT 0
#endif
#ifndef HALO_OTA_BENCH_PROFILE
#define HALO_OTA_BENCH_PROFILE 0
#endif
namespace durable_ota {
constexpr uint32_t kMinimumEpoch = 1700000000UL;
constexpr uint32_t kFastDelay = 300, kFastLifetime = 1800, kPeerLead = 15;
constexpr uint32_t kDailyWorkMs = 40UL * 60 * 1000;
constexpr uint32_t kPreflightMs = 120000, kSelfApplyMs = 20UL * 60 * 1000;
constexpr size_t kRecordBytes = 768;
enum class Phase : uint8_t {
  FAST = 1, PREFLIGHT, APPLY, ARM_PENDING, ARMED, DEFERRED,
  QUARANTINED, RESOLVED, DISCOVERY, ARM_PREPARATION, BENCH_ABORTED, BENCH_READY
};
enum class Failure : uint8_t {
  TEMPORARY = 0, INVALID_TARGET, ROLLBACK, EXACT_VALIDATION
};
enum class Admission : uint8_t {
  ALLOWED, CLOCK, IDENTITY, BUSY, NOT_DUE, BUDGET, STORAGE, LEGACY,
  QUARANTINED, RESOLVED, DISCOVERY
};
enum class ReadResult : uint8_t { ABSENT, PRESENT, ERROR };
enum class LegacyDecision : uint8_t { NONE, WAIT_PROOF, SUPERSEDED_UNCREDITED, REBIND_UNCREDITED };

struct Target {
  char version[32]{};
  char url[256]{};
  uint8_t sha256[32]{};
  uint32_t bytes{};
  char peer_version[32]{};
  uint8_t peer_sha256[32]{};
  uint32_t peer_bytes{};
};
// Uses112 of the140 canonical reserved bytes. No new NVS key or target copy.
constexpr uint32_t kOneShotDelay = 600, kOneShotMaximumDelay = 900, kOneShotWindow = 2700, kOneShotArmSeconds = 5;
enum class OneShotPhase : uint8_t { NONE=0, ARM_PENDING, ARMED, CONSUMED, CLOSED, PREPARING };
struct OneShot {
  uint32_t timer_us{}, query_elapsed_ms{};
  uint32_t due{}, expiry{}, arm_started{}, peer_boot{}, sent_remaining{}, ack_epoch{};
  uint32_t timer_epoch{}, arm_boot{};
  int32_t timer_sdk{};
  OneShotPhase phase{OneShotPhase::NONE};
  bool timer_recorded{}, control_debited{};
  char challenge[64]{};
};
static_assert(sizeof(OneShot)==112,"bounded one-shot RAM fields");
enum class BenchState:uint8_t { NONE=0, ACTIVE, AUDIT_ONLY };
struct BenchProfile {
  uint8_t session[8]{};
  uint32_t until{},abort_crc{},abort_generation{};
  uint16_t initial_s{},retry_s{},defer_s{};
  BenchState state{BenchState::NONE};uint8_t abort_count{};
};
static_assert(sizeof(BenchProfile)==28,"existing canonical tail only");
constexpr uint32_t kBenchLifetime=4UL*60*60, kBenchWorkMs=4UL*60*60*1000;
constexpr uint8_t kBenchNetworks=64,kBenchApplies=64,kBenchBegins=128;
struct Record {
  uint32_t generation{};
  Phase phase{Phase::FAST};
  bool deferred_path{};
  uint8_t fast_opportunities{}, network_windows{}, day_attempts{};
  uint8_t begins[2]{}, attempt_begins[2]{};
  uint8_t validation_repeats{};
  uint8_t campaign[16]{};
  char origin[64]{};
  Target target{};
  uint32_t created{}, high_water{}, budget_day{}, budget_granted{};
  uint32_t fast_start{}, fast_due{}, fast_expiry{}, not_before{};
  uint32_t attempt_ordinal{}, active_started{}, active_deadline{};
  uint32_t work_remaining_ms{}, reserved_work_ms{};
  uint32_t arm_epoch{}, arm_peer_boot{};
  uint32_t validation_stage{};
  int32_t validation_error{};
  char arm_id[64]{};
  OneShot one_shot{};
  BenchProfile bench{};
};
struct Clock { uint32_t epoch; bool fresh; bool normal_maintenance; };

inline bool text(const char* s, size_t n, bool empty = false) {
  const char* end = static_cast<const char*>(memchr(s, 0, n));
  if (!end || (!empty && end == s)) return false;
  for (const char* p=s; p!=end; ++p)
    if (static_cast<uint8_t>(*p)<32 || static_cast<uint8_t>(*p)>126) return false;
  for (const char* p=end; p!=s+n; ++p) if (*p) return false;
  return true;
}
inline bool nonzero(const uint8_t* b, size_t n) {
  uint8_t any=0; for(size_t i=0;i<n;++i) any|=b[i]; return any!=0;
}
inline bool target_valid(const Target& t) {
  return text(t.version,sizeof(t.version)) && text(t.url,sizeof(t.url)) &&
      !strncmp(t.url,"https://",8) && t.bytes && nonzero(t.sha256,32) &&
      text(t.peer_version,sizeof(t.peer_version)) &&
      ((t.peer_bytes && nonzero(t.peer_sha256,32)) ||
       (!t.peer_bytes && !nonzero(t.peer_sha256,32)));
}
inline bool same_target(const Target& a,const Target& b) {
  return target_valid(a) && target_valid(b) && !strcmp(a.version,b.version) &&
      !strcmp(a.url,b.url) && a.bytes==b.bytes && !memcmp(a.sha256,b.sha256,32) &&
      !strcmp(a.peer_version,b.peer_version) && a.peer_bytes==b.peer_bytes &&
      !memcmp(a.peer_sha256,b.peer_sha256,32);
}
inline bool bench_active(const Record&r){return r.bench.state==BenchState::ACTIVE;}
inline uint32_t work_cap(const Record&r){return bench_active(r)?kBenchWorkMs:kDailyWorkMs;}
inline uint8_t network_cap(const Record&r){return bench_active(r)?kBenchNetworks:2;}
inline uint8_t apply_cap(const Record&r){return bench_active(r)?kBenchApplies:2;}
inline uint8_t begin_cap(const Record&r){return bench_active(r)?kBenchBegins:4;}
inline uint32_t retry_delay(const Record&r){return bench_active(r)?r.bench.retry_s:kFastDelay;}
inline uint32_t initial_delay(const Record&r){return bench_active(r)?r.bench.initial_s:kOneShotDelay;}
inline bool bench_live(const Record&r,Clock c){return HALO_OTA_BENCH_PROFILE==1&&bench_active(r)&&c.fresh&&c.epoch<r.bench.until;}
inline bool bench_shape(const Record&r){
 const auto&b=r.bench;if(b.state==BenchState::NONE)return !nonzero(reinterpret_cast<const uint8_t*>(&b),sizeof(b));
 return b.state<=BenchState::AUDIT_ONLY&&nonzero(b.session,8)&&b.until>=kMinimumEpoch&&
  b.initial_s>=120&&b.initial_s<=900&&b.retry_s>=120&&b.retry_s<=900&&b.defer_s>=120&&b.defer_s<=900&&
  ((!b.abort_count&&!b.abort_crc&&!b.abort_generation)||(b.abort_count&&b.abort_generation));
}
inline bool one_shot_shape(const Record& r) {
  const OneShot& o=r.one_shot;
  if(r.phase==Phase::DISCOVERY && o.phase!=OneShotPhase::NONE)return false;
  if(r.phase==Phase::ARM_PREPARATION && (!o.control_debited ||
     (o.phase!=OneShotPhase::PREPARING && o.phase!=OneShotPhase::ARM_PENDING)))return false;
  if(!o.control_debited && o.query_elapsed_ms)return false;
  if(o.control_debited && !o.arm_boot)return false;
  if(o.phase>OneShotPhase::PREPARING || !text(o.challenge,sizeof(o.challenge),o.phase==OneShotPhase::NONE||o.phase==OneShotPhase::PREPARING||(o.phase==OneShotPhase::CLOSED&&o.control_debited&&!o.arm_started)))return false;
  if(o.phase==OneShotPhase::NONE)
    return !o.challenge[0]&&!o.query_elapsed_ms&&!o.control_debited&&!o.timer_recorded&&!o.due&&!o.expiry&&!o.arm_started&&!o.peer_boot&&!o.sent_remaining&&
      !o.ack_epoch&&!o.timer_epoch&&!o.arm_boot&&!o.timer_us&&!o.timer_sdk;
  if(o.phase==OneShotPhase::PREPARING) {
    return r.phase==Phase::ARM_PREPARATION && o.control_debited &&
      uint64_t(r.active_started)+initial_delay(r)<=o.due && uint64_t(r.active_started)+kOneShotMaximumDelay>=o.due &&
      uint64_t(o.due)+kOneShotWindow==o.expiry && !o.arm_started && !o.peer_boot && !o.sent_remaining &&
      !o.ack_epoch && !o.challenge[0] && !o.query_elapsed_ms && !o.timer_recorded && !o.timer_epoch && !o.timer_us && !o.timer_sdk;
  }
  // Closed failed control may never have received a query/ACK. Retain its
  // immutable due/expiry and debit marker without inventing peer evidence.
  if(o.phase==OneShotPhase::CLOSED && o.control_debited && !o.arm_started) {
    return (r.phase==Phase::DEFERRED || r.phase==Phase::RESOLVED || r.phase==Phase::QUARANTINED ||
      (r.deferred_path && (r.phase==Phase::PREFLIGHT || r.phase==Phase::APPLY))) &&
      o.due>=r.created && uint64_t(o.due)+kOneShotWindow==o.expiry && !o.peer_boot && !o.sent_remaining &&
      !o.ack_epoch && !o.challenge[0] && !o.query_elapsed_ms && !o.timer_recorded && !o.timer_epoch && !o.timer_us && !o.timer_sdk;
  }
  if(o.arm_started<r.created || o.arm_started>r.high_water || !o.peer_boot ||
      uint64_t(o.arm_started)+kPeerLead>=o.due ||
      uint64_t(o.due)+kOneShotWindow!=o.expiry || o.sent_remaining!=o.due-o.arm_started)return false;
  if(o.phase==OneShotPhase::ARM_PENDING) {
    if(o.ack_epoch || o.timer_recorded || (o.control_debited?r.phase!=Phase::ARM_PREPARATION:r.phase!=Phase::FAST))return false;
    if(o.control_debited && (uint64_t(r.active_started)+initial_delay(r)>o.due ||
       uint64_t(r.active_started)+kOneShotMaximumDelay<o.due || o.arm_started<r.active_started || o.arm_started>=r.active_deadline || o.query_elapsed_ms>=r.reserved_work_ms))return false;
  } else if(o.phase!=OneShotPhase::CLOSED || o.ack_epoch) {
    if(o.ack_epoch<o.arm_started || o.ack_epoch>=uint64_t(o.arm_started)+kOneShotArmSeconds ||
       o.ack_epoch>r.high_water)return false;
  }
  if(o.phase==OneShotPhase::ARMED && r.phase!=Phase::FAST)return false;
  if(o.phase==OneShotPhase::CLOSED && r.phase!=Phase::DEFERRED &&
     r.phase!=Phase::QUARANTINED && r.phase!=Phase::RESOLVED &&
     !(r.deferred_path && (r.phase==Phase::PREFLIGHT || r.phase==Phase::APPLY)))return false;
  if(o.timer_recorded) {
    if(!o.ack_epoch || !o.arm_boot || o.timer_epoch<o.ack_epoch || o.timer_epoch>=o.due ||
       o.timer_epoch>r.high_water || o.timer_us!=uint64_t(o.due-o.timer_epoch)*1000000ULL)return false;
  } else if(o.timer_epoch||(!o.control_debited&&o.arm_boot)||o.timer_us||o.timer_sdk)return false;
  return true;
}
inline bool target_empty(const Target& t) {
  return !nonzero(reinterpret_cast<const uint8_t*>(t.version),sizeof(t.version)) &&
      !nonzero(reinterpret_cast<const uint8_t*>(t.url),sizeof(t.url)) &&
      !nonzero(t.sha256,32) && !t.bytes &&
      !nonzero(reinterpret_cast<const uint8_t*>(t.peer_version),sizeof(t.peer_version)) &&
      !nonzero(t.peer_sha256,32) && !t.peer_bytes;
}
inline bool active_phase(const Record& r) {
  return r.phase==Phase::PREFLIGHT || r.phase==Phase::APPLY || r.phase==Phase::ARM_PREPARATION ||
      (r.phase==Phase::DISCOVERY && r.reserved_work_ms!=0);
}
inline bool shape(const Record& r) {
  if (!r.generation || r.phase<Phase::FAST || r.phase>Phase::BENCH_READY ||
      !nonzero(r.campaign,16) || !text(r.origin,sizeof(r.origin)) ||
      !(target_valid(r.target) || ((r.phase==Phase::DISCOVERY || r.phase==Phase::BENCH_READY) && target_empty(r.target))) ||
      r.created<kMinimumEpoch || r.high_water<r.created ||
      r.budget_granted<kMinimumEpoch || r.budget_granted>r.high_water ||
      r.budget_day!=r.budget_granted/86400UL || r.fast_opportunities>2 ||
      r.network_windows>network_cap(r) || r.day_attempts>apply_cap(r) || r.begins[0]>begin_cap(r) || r.begins[1]>begin_cap(r) ||
      r.attempt_begins[0]>2 || r.attempt_begins[1]>2 || r.validation_repeats>2 ||
      r.work_remaining_ms>work_cap(r) || r.reserved_work_ms>work_cap(r) ||
      uint64_t(r.work_remaining_ms)+r.reserved_work_ms>work_cap(r) ||
      !text(r.arm_id,sizeof(r.arm_id),true) || !bench_shape(r) ||
      ((r.phase==Phase::BENCH_ABORTED||r.phase==Phase::BENCH_READY)&&r.bench.state!=BenchState::ACTIVE)) return false;
  if (r.fast_start) {
    if (r.fast_start<r.created || uint64_t(r.fast_start)+retry_delay(r)!=r.fast_due ||
        uint64_t(r.fast_start)+kFastLifetime!=r.fast_expiry) return false;
  } else if (r.fast_due || r.fast_expiry) return false;
  const bool active=active_phase(r);
  if (active) {
    if (!r.reserved_work_ms || r.active_started<r.created ||
        r.active_deadline<=r.active_started ||
        uint64_t(r.active_deadline-r.active_started)*1000<r.reserved_work_ms) return false;
  } else if (r.reserved_work_ms || r.active_started || r.active_deadline) return false;
  if (r.phase==Phase::ARM_PENDING || r.phase==Phase::ARMED) {
    if (!r.fast_start || !r.arm_id[0] || r.arm_epoch!=r.fast_due) return false;
    if (r.phase==Phase::ARMED && !r.arm_peer_boot) return false;
  }
  if ((r.phase==Phase::DEFERRED || (r.phase==Phase::DISCOVERY && !active)) && r.not_before<kMinimumEpoch) return false;
  return one_shot_shape(r);
}
inline bool clock_valid(const Record& r,Clock c) {
  return c.fresh && c.epoch>=kMinimumEpoch && c.epoch>=r.high_water &&
      (!bench_active(r)||HALO_OTA_BENCH_PROFILE==1);
}
// Actual work spanning midnight carries its already reserved charge forward.
// This does not grant credit; only rollover() may replenish a deferred day.
inline void carry_work_day(Record& r,Clock c) {
  if(c.epoch/86400UL>r.budget_day) {
    r.budget_day=c.epoch/86400UL;r.budget_granted=c.epoch;
  }
}
inline bool next(const Record& old,Clock c,Record& out) {
  if (!shape(old) || !clock_valid(old,c) || old.generation==UINT32_MAX) return false;
  out=old; ++out.generation; out.high_water=c.epoch;
  if(active_phase(old))carry_work_day(out,c);
  return true;
}
inline bool identity(const Record& r,const char* origin,const Target& t) {
  return shape(r) && origin && !strcmp(r.origin,origin) && same_target(r.target,t);
}

// Definitive absence is not enough when legacy debt exists. Fresh local/peer
// proof is interpreted as supersession/rebinding WITHOUT historical credit.
// This returns a decision only; it never removes or edits the legacy keys.
inline LegacyDecision legacy_decide(bool legacy_debt,bool local_valid,bool peer_valid,
                                   bool local_newer_than_legacy,bool immutable_target_verified) {
  if (!legacy_debt) return LegacyDecision::NONE;
  if (!local_valid || !peer_valid) return LegacyDecision::WAIT_PROOF;
  if (local_newer_than_legacy) return LegacyDecision::SUPERSEDED_UNCREDITED;
  return immutable_target_verified ? LegacyDecision::REBIND_UNCREDITED : LegacyDecision::WAIT_PROOF;
}
inline bool start(const Target& target,const char* origin,const uint8_t (&campaign)[16],
                  Clock c,bool legacy_debt,Record& out) {
  if (!c.fresh || c.epoch<kMinimumEpoch || uint64_t(c.epoch)+kFastLifetime>UINT32_MAX ||
      !target_valid(target) || !origin || !*origin || strnlen(origin,64)>=64 ||
      !nonzero(campaign,16) || legacy_debt) return false;
  Record r{};r.generation=1;r.target=target;memcpy(r.campaign,campaign,16);
  memcpy(r.origin,origin,strlen(origin));r.created=r.high_water=r.budget_granted=c.epoch;
  r.budget_day=c.epoch/86400UL;r.work_remaining_ms=kDailyWorkMs;
  if(!shape(r))return false;out=r;return true;
}
// Explicit, freshly proved legacy reconciliation still does not invent an
// unused allowance. Import as deferred with this day's credit exhausted.
// Legacy keys/credit remain the wrapper's separate checked responsibility.
inline bool import_legacy(const Target& target,const char* origin,const uint8_t (&campaign)[16],
                          Clock c,LegacyDecision proof,uint32_t next_normal,Record& out) {
  if(proof!=LegacyDecision::SUPERSEDED_UNCREDITED && proof!=LegacyDecision::REBIND_UNCREDITED)return false;
  Record r{};if(!start(target,origin,campaign,c,false,r) || next_normal<=c.epoch)return false;
  r.phase=Phase::DEFERRED;r.deferred_path=true;r.fast_opportunities=2;
  r.network_windows=1;r.day_attempts=1;r.begins[0]=r.begins[1]=2;
  r.work_remaining_ms=0;r.not_before=next_normal;
  if(!shape(r))return false;out=r;return true;
}
inline bool replace_target(const Record& old,const Target& target,const char* origin,
                           const uint8_t (&campaign)[16],Clock c,bool verified_newer,Record& out) {
  if(bench_active(old) || !verified_newer || !shape(old) || !target_valid(target) || !clock_valid(old,c) ||
      active_phase(old) || old.phase==Phase::DISCOVERY ||
      old.one_shot.phase==OneShotPhase::ARM_PENDING || old.one_shot.phase==OneShotPhase::ARMED ||
      !strcmp(old.target.version,target.version) || !memcmp(old.campaign,campaign,16))return false;
  Record r{};if(!start(target,origin,campaign,c,false,r) || old.generation==UINT32_MAX)return false;
  r.generation=old.generation+1;
  // A new target is not a new daily energy/erase budget.
  r.budget_day=old.budget_day;r.budget_granted=old.budget_granted;
  r.network_windows=old.network_windows;r.day_attempts=old.day_attempts;
  r.begins[0]=old.begins[0];r.begins[1]=old.begins[1];r.work_remaining_ms=old.work_remaining_ms;
  if(old.deferred_path) {
    r.phase=Phase::DEFERRED;r.deferred_path=true;
    r.fast_opportunities=old.fast_opportunities;r.not_before=old.not_before;
  }
  if(!shape(r))return false;out=r;return true;
}
inline void clear_active(Record& r) {
  r.active_started=r.active_deadline=r.reserved_work_ms=0;
}
inline bool defer(Record& r,uint32_t next_normal) {
  if(next_normal<kMinimumEpoch || next_normal<=r.high_water)return false;
  r.phase=Phase::DEFERRED;r.deferred_path=true;
  // A later observation cannot move an already chosen earlier opportunity.
  if(!r.not_before || r.not_before<=r.high_water)r.not_before=next_normal;
  r.arm_peer_boot=0;
  if(r.one_shot.phase!=OneShotPhase::NONE)r.one_shot.phase=OneShotPhase::CLOSED;
  return true;
}
inline bool reconcile_reset(const Record& old,Clock c,uint32_t next_normal,Record& out) {
  if(!next(old,c,out))return false;
  if(old.phase==Phase::DISCOVERY) {
    if(!active_phase(old) || next_normal<=c.epoch)return false;
    // The original read-only network reservation remains fully charged after
    // reset. No target/update/credit or short arm is fabricated from a GET.
    clear_active(out);out.not_before=next_normal;return shape(out);
  }
  if(old.phase==Phase::ARM_PREPARATION) {
    clear_active(out);return defer(out,next_normal)&&shape(out);
  }
  if(old.phase!=Phase::PREFLIGHT && old.phase!=Phase::APPLY)return false;
  // Work was debited before the interruption. Never refund unknown elapsed work.
  const uint32_t anchor=old.active_started;clear_active(out);
  if(!out.fast_start && !out.deferred_path && out.fast_opportunities<2 &&
      uint64_t(anchor)+kFastLifetime>c.epoch && uint64_t(anchor)+kFastLifetime<=UINT32_MAX) {
    out.fast_start=anchor;out.fast_due=anchor+retry_delay(out);out.fast_expiry=anchor+kFastLifetime;
  }
  // No new arm is claimed from a reset. A later explicit arm operation must
  // prove peer storage before a short retry; default to normal deferred work.
  return defer(out,next_normal) && shape(out);
}
inline bool rollover(const Record& old,Clock c,Record& out) {
  if(bench_active(old) || !next(old,c,out) || !c.normal_maintenance || old.phase!=Phase::DEFERRED ||
      c.epoch<old.not_before || c.epoch/86400UL<=old.budget_day)return false;
  out.budget_day=c.epoch/86400UL;out.budget_granted=c.epoch;
  out.network_windows=out.day_attempts=out.begins[0]=out.begins[1]=0;
  out.attempt_begins[0]=out.attempt_begins[1]=0;out.work_remaining_ms=kDailyWorkMs;
  return shape(out);
}
// Internal shared reservation mechanics; one-shot admission must first prove
// its immutable arm and new boot in one_shot_reserve(). Normal callers use
// reserve_preflight(), which never consumes an armed one-shot implicitly.
inline Admission reserve_preflight_impl(const Record& old,Clock c,bool busy,Record& out,bool one_shot_admit) {
  if(!shape(old))return Admission::IDENTITY;
  if(!clock_valid(old,c))return Admission::CLOCK;
  if(busy)return Admission::BUSY;
  if(old.phase==Phase::QUARANTINED)return Admission::QUARANTINED;
  if(old.phase==Phase::RESOLVED)return Admission::RESOLVED;
  const bool slow=old.phase==Phase::DEFERRED;
  if(bench_active(old)&&!bench_live(old,c))return Admission::NOT_DUE;
  if(old.one_shot.phase!=OneShotPhase::NONE && !slow) {
    if(!HALO_OTA_ONE_SHOT || !old.one_shot.control_debited || c.epoch>=old.one_shot.expiry ||
       old.one_shot.phase==OneShotPhase::ARM_PENDING ||
       (old.one_shot.phase==OneShotPhase::ARMED && !one_shot_admit) ||
       old.one_shot.phase==OneShotPhase::CLOSED)return Admission::NOT_DUE;
  }
  if(slow) {
    if(!c.normal_maintenance || c.epoch<old.not_before)return Admission::NOT_DUE;
    if(bench_active(old)?old.network_windows>=network_cap(old):(old.network_windows>=1 || old.budget_day!=c.epoch/86400UL))return Admission::BUDGET;
  } else if(old.phase==Phase::ARMED) {
    if(uint64_t(c.epoch)+kPeerLead<old.fast_due || c.epoch>=old.fast_expiry)return Admission::NOT_DUE;
  } else if(old.phase!=Phase::FAST) return Admission::BUSY;
  if(!slow && (old.fast_opportunities>=2 || old.network_windows>=network_cap(old)))return Admission::BUDGET;
  uint32_t grant=old.work_remaining_ms<kPreflightMs?old.work_remaining_ms:kPreflightMs;
  if(!slow && old.fast_expiry) {
    if(c.epoch>=old.fast_expiry)return Admission::NOT_DUE;
    const uint64_t left=uint64_t(old.fast_expiry-c.epoch)*1000;
    if(left<grant)grant=uint32_t(left);
  }
  if(!slow && old.one_shot.phase!=OneShotPhase::NONE) {
    const uint64_t left=uint64_t(old.one_shot.expiry-c.epoch)*1000;
    if(left<grant)grant=uint32_t(left);
  }
  if(bench_active(old)){const uint64_t left=uint64_t(old.bench.until-c.epoch)*1000;if(left<grant)grant=uint32_t(left);}
  if(grant<1000 || uint64_t(c.epoch)+(grant+999)/1000>UINT32_MAX)return Admission::BUDGET;
  if(!next(old,c,out))return Admission::STORAGE;
  // A fast reservation after midnight consumes the new day without refilling.
  // Pure deferred observations and target replacement leave rollover eligible.
  carry_work_day(out,c);
  out.phase=Phase::PREFLIGHT;out.deferred_path=slow;
  if(one_shot_admit)out.one_shot.phase=OneShotPhase::CONSUMED;
  ++out.network_windows;if(!slow)++out.fast_opportunities;
  out.work_remaining_ms-=grant;out.reserved_work_ms=grant;out.active_started=c.epoch;
  out.active_deadline=c.epoch+(grant+999)/1000;
  return shape(out)?Admission::ALLOWED:Admission::IDENTITY;
}
inline Admission reserve_preflight(const Record& old,Clock c,bool busy,Record& out) {
  return reserve_preflight_impl(old,c,busy,out,false);
}
inline bool reserve_apply(const Record& old,Clock c,uint32_t preflight_elapsed_ms,
                          uint32_t external_budget_ms,bool peer_verified,Record& out) {
  if(!next(old,c,out) || old.phase!=Phase::PREFLIGHT || !peer_verified ||
      c.epoch>=old.active_deadline || preflight_elapsed_ms>old.reserved_work_ms ||
      old.day_attempts>=(bench_active(old)?apply_cap(old):(old.deferred_path?1:2)) || (bench_active(old)&&!bench_live(old,c)))return false;
  out.work_remaining_ms+=old.reserved_work_ms-preflight_elapsed_ms;clear_active(out);
  uint32_t grant=out.work_remaining_ms<external_budget_ms?out.work_remaining_ms:external_budget_ms;
  if(!old.deferred_path && old.fast_expiry) {
    if(c.epoch>=old.fast_expiry)return false;
    const uint64_t left=uint64_t(old.fast_expiry-c.epoch)*1000;if(left<grant)grant=uint32_t(left);
  }
  if(!old.deferred_path && old.one_shot.phase!=OneShotPhase::NONE) {
    if(c.epoch>=old.one_shot.expiry)return false;
    const uint64_t left=uint64_t(old.one_shot.expiry-c.epoch)*1000;
    if(left<grant)grant=uint32_t(left);
  }
  if(bench_active(old)){const uint64_t left=uint64_t(old.bench.until-c.epoch)*1000;if(left<grant)grant=uint32_t(left);}
  if(grant<1000 || uint64_t(c.epoch)+(grant+999)/1000>UINT32_MAX || old.attempt_ordinal==UINT32_MAX)return false;
  out.phase=Phase::APPLY;out.work_remaining_ms-=grant;out.reserved_work_ms=grant;
  out.active_started=c.epoch;out.active_deadline=c.epoch+(grant+999)/1000;
  ++out.day_attempts;++out.attempt_ordinal;out.attempt_begins[0]=out.attempt_begins[1]=0;
  return shape(out);
}
inline bool reserve_begin(const Record& old,Clock c,unsigned board,Record& out) {
  if(!next(old,c,out) || board>1 || old.phase!=Phase::APPLY || c.epoch>=old.active_deadline ||
      old.attempt_begins[board]>=2 || old.begins[board]>=(bench_active(old)?begin_cap(old):(old.deferred_path?2:4)) || (bench_active(old)&&!bench_live(old,c)))return false;
  ++out.begins[board];++out.attempt_begins[board];return shape(out);
}
inline bool finish(const Record& old,Clock c,uint32_t elapsed_ms,bool cleaned,
                   Failure failure,uint32_t stage,int32_t error,uint32_t next_normal,
                   const char* arm_id,Record& out) {
  if(!next(old,c,out) || (old.phase!=Phase::APPLY && old.phase!=Phase::PREFLIGHT) || !cleaned)return false;
  if(elapsed_ms<old.reserved_work_ms)out.work_remaining_ms+=old.reserved_work_ms-elapsed_ms;
  clear_active(out);
  if(failure==Failure::INVALID_TARGET || failure==Failure::ROLLBACK) {
    out.phase=Phase::QUARANTINED;return shape(out);
  }
  if(failure==Failure::EXACT_VALIDATION && stage && error) {
    out.validation_repeats=(old.validation_stage==stage && old.validation_error==error)
        ? uint8_t(old.validation_repeats<2?old.validation_repeats+1:2):1;
    out.validation_stage=stage;out.validation_error=error;
    if(out.validation_repeats>=2){out.phase=Phase::QUARANTINED;return shape(out);}
  }
  // A coarse/inconclusive error does not become validation evidence.
  if((bench_active(old)&&(!bench_live(old,c)||uint64_t(c.epoch)+retry_delay(old)+kPeerLead>=old.bench.until)) || old.deferred_path || out.fast_opportunities>=2 ||
     (old.one_shot.phase!=OneShotPhase::NONE && (!HALO_OTA_ONE_SHOT || !old.one_shot.control_debited || uint64_t(c.epoch)+retry_delay(old)+kPeerLead>=old.one_shot.expiry)))
    return defer(out,next_normal)&&shape(out);
  if(!out.fast_start) {
    if(uint64_t(c.epoch)+kFastLifetime>UINT32_MAX)return defer(out,next_normal)&&shape(out);
    out.fast_start=c.epoch;out.fast_due=c.epoch+retry_delay(out);out.fast_expiry=c.epoch+kFastLifetime;
  }
  if((old.one_shot.phase!=OneShotPhase::NONE && uint64_t(out.fast_due)+kPeerLead>=old.one_shot.expiry) ||
     uint64_t(c.epoch)+kPeerLead>=out.fast_due || !arm_id || !*arm_id || strnlen(arm_id,64)>=64)
    return defer(out,next_normal)&&shape(out);
  memset(out.arm_id,0,sizeof(out.arm_id));memcpy(out.arm_id,arm_id,strlen(arm_id));
  out.arm_epoch=out.fast_due;out.arm_peer_boot=0;out.phase=Phase::ARM_PENDING;
  return shape(out);
}
inline bool confirm_arm(const Record& old,Clock c,const char* id,uint32_t epoch,
                        uint32_t expected_peer,uint32_t actual_peer,bool challenge_matched,
                        bool stored_readback,Record& out) {
  if(!next(old,c,out) || old.phase!=Phase::ARM_PENDING || !id || strcmp(id,old.arm_id) ||
      (bench_active(old)&&(!bench_live(old,c)||epoch>=old.bench.until)) ||
      epoch!=old.arm_epoch || !expected_peer || expected_peer!=actual_peer ||
      !challenge_matched || !stored_readback || uint64_t(c.epoch)+kPeerLead>=epoch ||
      (old.one_shot.phase!=OneShotPhase::NONE && (!HALO_OTA_ONE_SHOT || !old.one_shot.control_debited || uint64_t(epoch)+kPeerLead>=old.one_shot.expiry)))return false;
  out.arm_peer_boot=actual_peer;out.phase=Phase::ARMED;return shape(out);
}
inline bool close_fast(const Record& old,Clock c,uint32_t next_normal,Record& out) {
  if(!next(old,c,out) || (old.phase!=Phase::ARM_PENDING && old.phase!=Phase::ARMED && old.phase!=Phase::FAST))return false;
  return defer(out,next_normal)&&shape(out);
}
inline uint32_t timer_delta(const Record& r,Clock c) {
  if(!shape(r)||!clock_valid(r,c)||(bench_active(r)&&(!bench_live(r,c)||r.fast_due>=r.bench.until))||r.phase!=Phase::ARMED||c.epoch>=r.fast_due ||
     (r.one_shot.phase!=OneShotPhase::NONE && (!HALO_OTA_ONE_SHOT || !r.one_shot.control_debited || c.epoch>=r.one_shot.expiry)))return 0;
  return r.fast_due-c.epoch;
}
inline bool resolve(const Record& old,Clock c,const Target& actual,bool local_valid,
                    bool peer_valid,bool selected_running_equal,Record& out) {
  if(!next(old,c,out)||old.phase==Phase::DISCOVERY||old.phase==Phase::BENCH_ABORTED||old.phase==Phase::BENCH_READY||!same_target(old.target,actual)||!local_valid||!peer_valid||
      !selected_running_equal)return false;
  // A post-reset VALID result may resolve APPLY, but unobserved time is charged.
  clear_active(out);out.phase=Phase::RESOLVED;
  if(out.one_shot.phase!=OneShotPhase::NONE)out.one_shot.phase=OneShotPhase::CLOSED;
  return shape(out);
}

inline uint32_t crc32(const uint8_t* b,size_t n) {
  uint32_t c=0xffffffffUL;
  for(size_t i=0;i<n;++i){c^=b[i];for(unsigned k=0;k<8;++k)c=(c>>1)^((c&1)?0xedb88320UL:0);}
  return c^0xffffffffUL;
}
struct Writer {
  uint8_t* bytes;bool compare;size_t pos=0;bool ok=true;uint32_t crc=0xffffffffUL;
  void byte(uint8_t v,bool checksum=true) {
    if(pos>=kRecordBytes){ok=false;return;}
    if(compare){if(bytes[pos]!=v)ok=false;}else bytes[pos]=v;
    ++pos;if(checksum){crc^=v;for(unsigned i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320UL:0);}
  }
  void u32(uint32_t v){for(unsigned i=0;i<4;++i)byte(uint8_t(v>>(8*i)));}
  void data(const void* p,size_t n){auto b=static_cast<const uint8_t*>(p);for(size_t i=0;i<n;++i)byte(b[i]);}
};
inline bool encode(const Record& r,uint8_t (&bytes)[kRecordBytes],bool compare=false) {
  if(!shape(r))return false;
  Writer w{bytes,compare};w.data("DOR1",4);w.byte(r.bench.state!=BenchState::NONE?3:(r.one_shot.phase==OneShotPhase::NONE?1:2));w.byte(uint8_t(r.phase));w.byte(r.deferred_path?1:0);w.byte(0);
  w.u32(r.generation);w.data(r.campaign,16);w.data(r.origin,64);
  w.data(r.target.version,32);w.data(r.target.url,256);w.data(r.target.sha256,32);w.u32(r.target.bytes);
  w.data(r.target.peer_version,32);w.data(r.target.peer_sha256,32);w.u32(r.target.peer_bytes);
  const uint32_t values[]={r.created,r.high_water,r.budget_day,r.budget_granted,r.fast_start,r.fast_due,r.fast_expiry,
    r.not_before,r.attempt_ordinal,r.active_started,r.active_deadline,r.work_remaining_ms,r.reserved_work_ms,
    r.arm_epoch,r.arm_peer_boot,r.validation_stage,uint32_t(r.validation_error)};
  for(uint32_t v:values)w.u32(v);
  w.byte(r.fast_opportunities);w.byte(r.network_windows);w.byte(r.day_attempts);
  w.byte(r.begins[0]);w.byte(r.begins[1]);w.byte(r.attempt_begins[0]);w.byte(r.attempt_begins[1]);w.byte(r.validation_repeats);
  w.data(r.arm_id,64);
  if(r.one_shot.phase!=OneShotPhase::NONE || r.bench.state!=BenchState::NONE) {
    const OneShot& o=r.one_shot;
    w.byte(uint8_t(o.phase));w.byte((o.timer_recorded?1:0)|(o.control_debited?2:0));w.byte(0);w.byte(0);
    const uint32_t vals[]={o.due,o.expiry,o.arm_started,o.peer_boot,o.sent_remaining,o.ack_epoch,o.timer_epoch,o.arm_boot};
    for(uint32_t v:vals)w.u32(v);
    w.u32(o.timer_us);w.u32(o.query_elapsed_ms);w.u32(uint32_t(o.timer_sdk));w.data(o.challenge,64);
  }
  if(r.bench.state!=BenchState::NONE){
    const auto&b=r.bench;w.data(b.session,8);w.u32(b.until);w.u32(b.abort_crc);w.u32(b.abort_generation);
    const uint16_t timing[]={b.initial_s,b.retry_s,b.defer_s};for(uint16_t n:timing){w.byte(uint8_t(n));w.byte(uint8_t(n>>8));}
    w.byte(uint8_t(b.state));w.byte(b.abort_count);
  }
  while(w.pos<kRecordBytes-4)w.byte(0);
  const uint32_t crc=w.crc^0xffffffffUL;for(unsigned i=0;i<4;++i)w.byte(uint8_t(crc>>(8*i)),false);
  return w.ok&&w.pos==kRecordBytes;
}
struct Reader {
  const uint8_t* bytes;size_t pos=0;
  uint8_t byte(){return bytes[pos++];}
  uint32_t u32(){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(byte())<<(8*i);return v;}
  void data(void* out,size_t n){memcpy(out,bytes+pos,n);pos+=n;}
};
inline bool decode(const uint8_t* bytes,size_t n,Record& out) {
  if(!bytes||n!=kRecordBytes||memcmp(bytes,"DOR1",4)||(bytes[4]!=1&&bytes[4]!=2&&bytes[4]!=3)||bytes[6]>1||bytes[7])return false;
  Reader tail{bytes};tail.pos=kRecordBytes-4;
  if(tail.u32()!=crc32(bytes,kRecordBytes-4))return false;
  Reader rd{bytes};rd.pos=5;Record r{};r.phase=Phase(rd.byte());r.deferred_path=rd.byte()!=0;rd.byte();
  r.generation=rd.u32();rd.data(r.campaign,16);rd.data(r.origin,64);
  rd.data(r.target.version,32);rd.data(r.target.url,256);rd.data(r.target.sha256,32);r.target.bytes=rd.u32();
  rd.data(r.target.peer_version,32);rd.data(r.target.peer_sha256,32);r.target.peer_bytes=rd.u32();
  uint32_t* values[]={&r.created,&r.high_water,&r.budget_day,&r.budget_granted,&r.fast_start,&r.fast_due,&r.fast_expiry,
    &r.not_before,&r.attempt_ordinal,&r.active_started,&r.active_deadline,&r.work_remaining_ms,&r.reserved_work_ms,
    &r.arm_epoch,&r.arm_peer_boot,&r.validation_stage};
  for(uint32_t* v:values)*v=rd.u32();r.validation_error=int32_t(rd.u32());
  r.fast_opportunities=rd.byte();r.network_windows=rd.byte();r.day_attempts=rd.byte();
  r.begins[0]=rd.byte();r.begins[1]=rd.byte();r.attempt_begins[0]=rd.byte();r.attempt_begins[1]=rd.byte();r.validation_repeats=rd.byte();
  rd.data(r.arm_id,64);
  if(bytes[4]>=2) {
    OneShot& o=r.one_shot;o.phase=OneShotPhase(rd.byte());const uint8_t flags=rd.byte();
    if((bytes[4]==2&&o.phase==OneShotPhase::NONE) || flags>3 || rd.byte() || rd.byte())return false;
    o.timer_recorded=(flags&1)!=0;o.control_debited=(flags&2)!=0;
    uint32_t* vals[]={&o.due,&o.expiry,&o.arm_started,&o.peer_boot,&o.sent_remaining,&o.ack_epoch,&o.timer_epoch,&o.arm_boot};
    for(uint32_t* v:vals)*v=rd.u32();
    o.timer_us=rd.u32();o.query_elapsed_ms=rd.u32();o.timer_sdk=int32_t(rd.u32());rd.data(o.challenge,64);
  }
  if(bytes[4]==3){
    auto&b=r.bench;rd.data(b.session,8);b.until=rd.u32();b.abort_crc=rd.u32();b.abort_generation=rd.u32();
    uint16_t* fields[]={&b.initial_s,&b.retry_s,&b.defer_s};for(auto f:fields){*f=rd.byte();*f|=uint16_t(rd.byte())<<8;}
    b.state=BenchState(rd.byte());b.abort_count=rd.byte();if(b.state==BenchState::NONE)return false;
  }
  while(rd.pos<kRecordBytes-4)if(rd.byte())return false;
  if(!shape(r))return false;out=r;return true;
}

// Store.read distinguishes absent from failure and returns actual length.
// The wrapper supplies its existing capacity lease and qualified activation.
// One scratch buffer; exact comparison re-encodes without a second RAM mirror.
template<class Store>
inline bool commit(Store& store,const Record& candidate,Record& live,bool& allowed,
                   uint8_t (&scratch)[kRecordBytes],bool first=false) {
  if(!allowed || !shape(candidate) ||
      (first?candidate.generation!=1:(!shape(live)||live.generation==UINT32_MAX||candidate.generation!=live.generation+1)))return false;
  allowed=false;
  size_t size=0;
  const auto before=store.read(scratch,sizeof(scratch),size);
  if(first) {if(before!=ReadResult::ABSENT)return false;}
  else if(before!=ReadResult::PRESENT || size!=sizeof(scratch) || !encode(live,scratch,true))return false;
  if(!encode(candidate,scratch)||store.write(scratch,sizeof(scratch))!=sizeof(scratch))return false;
  size=0;
  if(store.read(scratch,sizeof(scratch),size)!=ReadResult::PRESENT||size!=sizeof(scratch)||
      !encode(candidate,scratch,true))return false;
  live=candidate;allowed=true;return true;
}
template<class Store>
inline ReadResult load(Store& store,Record& live,bool& allowed,uint8_t (&scratch)[kRecordBytes]) {
  allowed=false;size_t size=0;const auto status=store.read(scratch,sizeof(scratch),size);
  if(status!=ReadResult::PRESENT)return status;
  Record candidate{};if(!decode(scratch,size,candidate))return ReadResult::ERROR;
  live=candidate;allowed=true;return ReadResult::PRESENT;
}
} // namespace durable_ota

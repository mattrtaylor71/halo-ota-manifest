#pragma once
#include "DurableOtaDiscovery.h"
#include "CoordinatorCreditState.h"

namespace durable_ota {
// Evidence belongs to one main-task admission. A known LCD-due hint remains
// pending until the existing verified pair path clears it; it is not a target.
struct DiscoveryRecoveryProof {
  const CoordinatorCreditState* credit=nullptr;
  const char* pending=nullptr;
  const char* completion_target=nullptr;
  bool storage_ready=false,uncertainty_clear=false,lcd_debt_known=false;
  bool local_valid=false,peer_ready=false,pending_due=false,user_idle=false;
  bool completed_comparison=false;
};

// Refusal-only diagnostic bits, with no identity/credential payload.
enum RecoveryMissing : uint16_t {
  RECOVERY_SHAPE=1U<<0, RECOVERY_CLOCK=1U<<1, RECOVERY_STORAGE=1U<<2,
  RECOVERY_UNCERTAIN=1U<<3, RECOVERY_DEBT_READ=1U<<4, RECOVERY_LOCAL_VALID=1U<<5,
  RECOVERY_PEER=1U<<6, RECOVERY_DUE=1U<<7, RECOVERY_USER_BUSY=1U<<8,
  RECOVERY_CREDIT=1U<<9, RECOVERY_TARGET=1U<<10, RECOVERY_PENDING=1U<<11,
  RECOVERY_COMPLETION_TARGET=1U<<12, RECOVERY_ORDER=1U<<13
};
inline uint16_t recovery_discovery_missing(const Record& old,Clock c,
                                           const DiscoveryRecoveryProof& p) {
  uint16_t missing=0;
  const bool record_ok=shape(old);
  if(!record_ok||old.phase!=Phase::DISCOVERY||active_phase(old)||bench_active(old)||
     old.one_shot.phase!=OneShotPhase::NONE)missing|=RECOVERY_SHAPE;
  if(!clock_valid(old,c))missing|=RECOVERY_CLOCK;
  if(!p.storage_ready)missing|=RECOVERY_STORAGE;
  if(!p.uncertainty_clear)missing|=RECOVERY_UNCERTAIN;
  if(!p.lcd_debt_known)missing|=RECOVERY_DEBT_READ;
  if(!p.local_valid)missing|=RECOVERY_LOCAL_VALID;
  if(!p.peer_ready)missing|=RECOVERY_PEER;
  if(!p.pending_due)missing|=RECOVERY_DUE;
  if(!p.user_idle)missing|=RECOVERY_USER_BUSY;
  if(!p.completion_target||p.completion_target[0])missing|=RECOVERY_COMPLETION_TARGET;
  // Canonical non-bench DISCOVERY retains a valid comparison target only
  // when opened from RESOLVED. It is never an unfinished apply identity;
  // retain its counters/target and additionally prove the running pair.
  if(target_empty(old.target)){
    if(old.deferred_path||old.fast_opportunities||old.attempt_ordinal||old.day_attempts||
       old.begins[0]||old.begins[1]||old.attempt_begins[0]||old.attempt_begins[1])missing|=RECOVERY_TARGET;
  }else if(!target_valid(old.target)||!p.completed_comparison)missing|=RECOVERY_TARGET;
  if(!p.credit||!credit_state_shape(*p.credit))return missing|RECOVERY_CREDIT;
  const auto& credit=*p.credit;
  if(!credit.pending.id[0]||!credit.pending.bound||credit.pending_resolved||
     !credit.pending_credit_admitted||!credit.admitted_epoch||
     credit.admitted_epoch>c.epoch||!p.pending||strcmp(p.pending,credit.pending.id))missing|=RECOVERY_PENDING;
  // The first adoption may replace only an older closed read-only origin.
  // After adoption, failures retain that exact origin and its spent allowance.
  if(record_ok&&strcmp(old.origin,credit.pending.id)&&credit.admitted_epoch<old.high_water)missing|=RECOVERY_ORDER;
  return missing;
}
inline bool recovery_discovery_owned(const Record& old,Clock c,
                                     const DiscoveryRecoveryProof& p) {
  return recovery_discovery_missing(old,c,p)==0;
}

inline Admission reserve_recovery_discovery(const Record& old,Clock c,
    const DiscoveryRecoveryProof& proof,bool explicit_manual,Record& out) {
  if(!recovery_discovery_owned(old,c,proof))return Admission::LEGACY;
  // A deliberate recovery may open a genuinely later UTC day, using the same
  // ordinary daily accounting as maintenance. It never renews the current day
  // or advances a future not-before. Automatic work still needs its real timer.
  Clock charged=c;
  if(explicit_manual&&!old.deferred_path&&c.epoch/86400UL>old.budget_day&&c.epoch>=old.not_before)
    charged.normal_maintenance=true;
  Record candidate{};
  const Admission result=reserve_discovery(old,charged,false,false,candidate,
                                           nullptr,nullptr,explicit_manual);
  if(result!=Admission::ALLOWED)return result;
  memset(candidate.origin,0,sizeof(candidate.origin));
  memcpy(candidate.origin,proof.credit->pending.id,strlen(proof.credit->pending.id));
  if(!shape(candidate))return Admission::IDENTITY;
  out=candidate;return Admission::ALLOWED;
}
} // namespace durable_ota

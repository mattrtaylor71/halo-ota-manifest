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

inline bool recovery_discovery_owned(const Record& old,Clock c,
                                     const DiscoveryRecoveryProof& p) {
  if(!shape(old)||!clock_valid(old,c)||old.phase!=Phase::DISCOVERY||
     active_phase(old)||bench_active(old)||
     old.one_shot.phase!=OneShotPhase::NONE||
     !p.storage_ready||!p.uncertainty_clear||!p.lcd_debt_known||
     !p.local_valid||!p.peer_ready||!p.pending_due||!p.user_idle||
     !p.credit||!credit_state_shape(*p.credit)||!p.pending||
     !p.completion_target||p.completion_target[0])return false;
  // Canonical non-bench DISCOVERY retains a valid comparison target only
  // when opened from RESOLVED. It is never an unfinished apply identity;
  // retain its counters/target and additionally prove the running pair.
  if(target_empty(old.target)){
    if(old.deferred_path||old.fast_opportunities||old.attempt_ordinal||old.day_attempts||
       old.begins[0]||old.begins[1]||old.attempt_begins[0]||old.attempt_begins[1])return false;
  }else if(!target_valid(old.target)||!p.completed_comparison)return false;
  const auto& credit=*p.credit;
  if(!credit.pending.id[0]||!credit.pending.bound||credit.pending_resolved||
     !credit.pending_credit_admitted||!credit.admitted_epoch||
     credit.admitted_epoch>c.epoch||strcmp(p.pending,credit.pending.id))return false;
  // The first adoption may replace only an older closed read-only origin.
  // After adoption, failures retain that exact origin and its spent allowance.
  return !strcmp(old.origin,credit.pending.id)||credit.admitted_epoch>=old.high_water;
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

#pragma once
#include "DiagnosticAdmission.h"
#include "DurableOtaPolicy.h"
#include "SenseSleepAttestation.h"

namespace halo_admission {
// Local actual-admission observation. The integration owner supplies this only
// after the checked PREFLIGHT commit. It never comes from a host JSON payload.
struct Observation {
  uint32_t boot,epoch,due,generation,ordinal;
  uint8_t reset,wake;
  bool fresh,accepted_scheduled_origin;
  const halo_sleep_attestation::Proof* lcd_proof=nullptr;
};
using Sha256=bool(*)(const uint8_t*,size_t,uint8_t*);
// All byte views and the decoded policy are read under the same existing NVS
// lease immediately before Store::write. Decode policy from its current full
// canonical768B read; a caller's stale RAM Record is not sufficient authority.
// This function performs no write and grants no policy allowance.
inline bool eligible(const Record&r,const Observation&o,
    const durable_ota::Record&policy,const halo_diag::Context&expected,
    const uint8_t(&context)[256],const uint8_t(&state)[256],
    const uint8_t*cp0,size_t n0,const uint8_t*cp1,size_t n1,Sha256 hash) {
  using namespace halo_diag;
  if(!valid(r)||!o.fresh||!hash||r.boot!=o.boot||r.epoch!=o.epoch||r.due!=o.due||
     r.generation!=o.generation||r.ordinal!=o.ordinal||r.reset!=o.reset||r.wake!=o.wake||
     bool(r.flags&SCHEDULED)!=o.accepted_scheduled_origin)return false;
  // Only a typed exact-context/current-admission proof permits this bit. The
  // proof is optional; unknown peers never prevent the ordinary breadcrumb.
  if((r.flags&LCD_VERIFIED)&&(!o.lcd_proof||!o.lcd_proof->matches(r.context_sha,r.boot,
      r.due,r.generation,r.ordinal,r.lcd_prior_boot)))return false;
  if(!durable_ota::shape(policy)||policy.phase!=durable_ota::Phase::PREFLIGHT||
     policy.generation!=r.generation||policy.attempt_ordinal!=r.ordinal||
     !policy.reserved_work_ms||r.epoch<policy.active_started||r.epoch>=policy.active_deadline||
     r.epoch<policy.high_water)return false;
  const uint32_t due=policy.deferred_path?policy.not_before:
    (policy.fast_due?policy.fast_due:policy.one_shot.due);
  if(r.due!=due)return false;
  if(expected.board!=Board::Sense||!context_matches(context,expected)||memcmp(expected.campaign,policy.campaign,16)||
     memcmp(expected.target_sha,policy.target.sha256,32)||expected.expected_bytes!=policy.target.bytes||
     strcmp(expected.target_version,policy.target.version)||strcmp(expected.origin,policy.origin))return false;
  uint8_t context_hash[32];if(!hash(context,256,context_hash)||memcmp(context_hash,r.context_sha,32))return false;
  Checkpoint best{};bool have=false;
  for(unsigned i=0;i<2;++i) {
    const uint8_t*p=i?cp1:cp0;const size_t n=i?n1:n0;
    if(!n)continue;
    Checkpoint c{};
    if(!p||n!=64||p[4]!=CHECKPOINT_VERSION||!decode_checkpoint(p,uint8_t(i+4),c)||
       c.context_crc!=record_crc(context,0)||c.damage_flags||c.lifecycle!=Lifecycle::Active)return false;
    if(have&&c.generation==best.generation&&!checkpoint_equal(c,best))return false;
    if(!have||c.generation>best.generation){best=c;have=true;}
  }
  if(!have||!best.state_reservations||!halo_diag::valid(state,256,3)||
     get16(state+20)!=228||get32(state+16)!=best.context_crc)return false;
  const uint8_t*p=state+HEADER;const uint64_t old_boot=get64(p);
  if(!old_boot||old_boot>UINT32_MAX||old_boot==r.boot||
     sequence(state)<=best.state_ack||sequence(state)>=best.next_sequence||
     !text_ok((const char*)p+20,96)||!text_ok((const char*)p+140,12)||
     get16(p+136)<1||get16(p+136)>uint16_t(Stage::ReportAfter)||p[138]>1||p[207]>2||p[208]>3)return false;
  return true;
}
} // namespace halo_admission

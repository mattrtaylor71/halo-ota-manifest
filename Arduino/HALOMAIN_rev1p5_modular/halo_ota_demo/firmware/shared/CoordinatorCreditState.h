#pragma once
#include "NightlyCredit.h"

// State/codec for the single eligibility NVS value. These pure
// policy operations do not clear legacy history, run OTA or renew budgets.
struct CoordinatorCreditState {
  NightlyCreditOrigin schedule;
  NightlyCreditOrigin pending;
  NightlyCreditOrigin deferred; // observed calendar obligation, not admitted work
  bool schedule_observed = false; // bounded overflow: do not overwrite this arm
  char last_uncredited[64] = {};
  bool pending_resolved = false;
  bool pending_credit_admitted = false;
  uint32_t admitted_epoch = 0;
};
static const size_t COORDINATOR_CREDIT_V1_BYTES = 340;
static const size_t COORDINATOR_CREDIT_BYTES = 472;

static bool credit_text(const char* s, size_t n, bool allow_empty) {
  const char* end=(const char*)memchr(s,0,n);
  if (!end || (!allow_empty && end==s)) return false;
  for (const char* p=s;p!=end;++p)
    if ((unsigned char)*p<32 || (unsigned char)*p>126) return false;
  return true;
}
static bool credit_origin_shape(const NightlyCreditOrigin& r) {
  if (!credit_text(r.id,sizeof(r.id),true) || !credit_text(r.timezone,sizeof(r.timezone),true)) return false;
  if (r.bound) {
    int y,m,d;
    return nightly_credit_date(r.id,y,m,d) && r.target_epoch && r.timezone[0];
  }
  return !r.target_epoch && !r.timezone[0];
}
static bool credit_same_origin(const NightlyCreditOrigin& a,const NightlyCreditOrigin& b) {
  return !strcmp(a.id,b.id) && !strcmp(a.timezone,b.timezone) &&
      a.target_epoch==b.target_epoch && a.bound==b.bound;
}
static bool credit_compatible_origin(const NightlyCreditOrigin& a,const NightlyCreditOrigin& b) {
  return !a.id[0] || strcmp(a.id,b.id) || !a.bound || !b.bound || credit_same_origin(a,b);
}
static bool credit_state_shape(const CoordinatorCreditState& s) {
  return credit_origin_shape(s.schedule) && credit_origin_shape(s.pending) &&
         credit_origin_shape(s.deferred) &&
         credit_compatible_origin(s.schedule,s.pending) &&
         credit_compatible_origin(s.schedule,s.deferred) &&
         credit_compatible_origin(s.pending,s.deferred) &&
         (!s.deferred.id[0] || s.deferred.bound) &&
         (!s.schedule_observed || (s.schedule.id[0] && s.schedule.bound)) &&
         credit_text(s.last_uncredited,sizeof(s.last_uncredited),true) &&
         (!s.pending_resolved || s.pending.id[0]) &&
         (s.pending_credit_admitted ?
           (s.pending.bound && s.admitted_epoch >= 1700000000UL &&
            (uint64_t)s.admitted_epoch + 15 >= s.pending.target_epoch) :
           s.admitted_epoch == 0);
}
static void credit_write_u32(uint8_t* b,uint32_t v) {
  for (unsigned i=0;i<4;++i)b[i]=(uint8_t)(v>>(8*i));
}
static uint32_t credit_read_u32(const uint8_t* b) {
  uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)b[i]<<(8*i);return v;
}
static bool credit_encode(const CoordinatorCreditState& s,uint8_t (&b)[COORDINATOR_CREDIT_BYTES]) {
  if (!credit_state_shape(s)) return false;
  memset(b,0,sizeof(b));memcpy(b,"NCD2",4);b[4]=2;
  b[5]=(s.schedule.bound?1:0)|(s.pending.bound?2:0)|(s.pending_resolved?4:0)|(s.pending_credit_admitted?8:0)|(s.deferred.bound?16:0)|(s.schedule_observed?32:0);
  const char* text[]={s.schedule.id,s.pending.id,s.last_uncredited,s.schedule.timezone,s.pending.timezone};
  for(unsigned i=0;i<5;++i)memcpy(b+8+64*i,text[i],strlen(text[i]));
  credit_write_u32(b+328,s.schedule.target_epoch);credit_write_u32(b+332,s.pending.target_epoch);
  credit_write_u32(b+336,s.admitted_epoch);
  memcpy(b+340,s.deferred.id,strlen(s.deferred.id));
  memcpy(b+404,s.deferred.timezone,strlen(s.deferred.timezone));
  credit_write_u32(b+468,s.deferred.target_epoch);
  return true;
}
static bool credit_decode(const uint8_t* b,size_t size,CoordinatorCreditState& out) {
  if (!b || (size!=COORDINATOR_CREDIT_V1_BYTES && size!=COORDINATOR_CREDIT_BYTES))return false;
  const bool legacy=size==COORDINATOR_CREDIT_V1_BYTES;
  if (memcmp(b,legacy?"NCD1":"NCD2",4) || b[4]!=(legacy?1:2) ||
      b[5]>(legacy?15:63) || b[6] || b[7])return false;
  CoordinatorCreditState candidate;
  char* text[]={candidate.schedule.id,candidate.pending.id,candidate.last_uncredited,candidate.schedule.timezone,candidate.pending.timezone};
  for(unsigned i=0;i<5;++i){
    const uint8_t* begin=b+8+64*i;const uint8_t* end=(const uint8_t*)memchr(begin,0,64);
    if (!end)return false;
    for(const uint8_t*p=end;p!=begin+64;++p)if(*p)return false;
    memcpy(text[i],begin,64);
  }
  candidate.schedule.bound=(b[5]&1)!=0;candidate.pending.bound=(b[5]&2)!=0;candidate.pending_resolved=(b[5]&4)!=0;
  candidate.pending_credit_admitted=(b[5]&8)!=0;candidate.admitted_epoch=credit_read_u32(b+336);
  candidate.schedule.target_epoch=credit_read_u32(b+328);candidate.pending.target_epoch=credit_read_u32(b+332);
  if(!legacy){
    for(unsigned i=0;i<2;++i){
      const uint8_t* begin=b+340+64*i;const uint8_t* end=(const uint8_t*)memchr(begin,0,64);
      if(!end)return false;
      for(const uint8_t*p=end;p!=begin+64;++p)if(*p)return false;
      memcpy(i?candidate.deferred.timezone:candidate.deferred.id,begin,64);
    }
    candidate.deferred.bound=(b[5]&16)!=0;
    candidate.schedule_observed=(b[5]&32)!=0;
    candidate.deferred.target_epoch=credit_read_u32(b+468);
  }
  if (!credit_state_shape(candidate))return false;
  out=candidate;return true;
}

// Store is the already-open ota_coord Preferences handle. The wrapper enables
// mutations_allowed only after validated authoritative load (or a reviewed
// absent-record import). Any failed mutation latches it false for this session.
// No later arm or work admission may use stale RAM. Production reload is allowed
// only once after normal boot/NVS recovery, never to reopen an uncertain boot.
template<class Store>
static bool credit_commit(Store& store,const CoordinatorCreditState& candidate,
                          CoordinatorCreditState& live,bool& mutations_allowed) {
  if(!mutations_allowed)return false;
  mutations_allowed=false;
  uint8_t encoded[COORDINATOR_CREDIT_BYTES],readback[COORDINATOR_CREDIT_BYTES];
  if(!credit_encode(candidate,encoded))return false;
  if(store.putBytes("elig_v1",encoded,sizeof(encoded))!=sizeof(encoded))return false;
  if(store.getBytesLength("elig_v1")!=sizeof(readback) ||
     store.getBytes("elig_v1",readback,sizeof(readback))!=sizeof(readback) ||
     memcmp(encoded,readback,sizeof(encoded)))return false;
  live=candidate;mutations_allowed=true;return true;
}
template<class Store>
static bool credit_reload(Store& store,CoordinatorCreditState& live,bool& mutations_allowed) {
  mutations_allowed=false;
  uint8_t encoded[COORDINATOR_CREDIT_BYTES];CoordinatorCreditState candidate;
  const size_t size=store.getBytesLength("elig_v1");
  if((size!=COORDINATOR_CREDIT_V1_BYTES && size!=sizeof(encoded)) ||
     store.getBytes("elig_v1",encoded,size)!=size ||
     !credit_decode(encoded,size,candidate))return false;
  live=candidate;mutations_allowed=true;return true;
}

// Preserve an authoritative timer origin without claiming it was due or worked.
// A second distinct obligation stays in schedule until a slot becomes free.
static bool credit_observe_calendar(const CoordinatorCreditState& old,const NightlyCreditOrigin& origin,
                                    CoordinatorCreditState& candidate) {
  if(!credit_state_shape(old)||!credit_origin_shape(origin)||!origin.id[0]||!origin.bound)return false;
  candidate=old;
  if(!old.deferred.id[0]){candidate.deferred=origin;return credit_state_shape(candidate);}
  if(!strcmp(old.deferred.id,origin.id))return credit_same_origin(old.deferred,origin);
  if(!credit_same_origin(old.schedule,origin))return false;
  candidate.schedule_observed=true;return credit_state_shape(candidate);
}

// Proposal caller contract: persist the candidate blob in one exact-length
// putBytes and verify exact readback BEFORE publishing candidate in live RAM.
// If persistence fails, retain old RAM only as historical data, latch eligibility
// mutations/admission off, and preserve recovery obligations until reload.
static bool credit_next_arm(const CoordinatorCreditState& old,const NightlyCreditOrigin& next,
                            CoordinatorCreditState& candidate) {
  if(!credit_state_shape(old)||!credit_origin_shape(next)||!next.id[0])return false;
  candidate=old;
  if(old.deferred.id[0] && !strcmp(old.deferred.id,next.id) &&
     !credit_same_origin(old.deferred,next))return false;
  if(old.schedule_observed){
    if(!strcmp(old.schedule.id,next.id)){
      if(!credit_same_origin(old.schedule,next))return false;
    }else{
      if(old.deferred.id[0])return false; // bounded capacity; never erase an observed origin
      candidate.deferred=old.schedule;candidate.schedule_observed=false;
    }
  }
  candidate.schedule=next;return credit_state_shape(candidate);
}
// Confirmed configuration supersedes unused old-zone origins. Pending work,
// its admission/repair state and last_uncredited keep their existing meaning.
// A captured timer is an observation of its exact stored schedule even before
// the initial reservation could persist (for example before image VALID).
static unsigned credit_retire_timezone(const CoordinatorCreditState& old,const char* tz,
                                      const NightlyCreditOrigin& timer,CoordinatorCreditState& candidate) {
  candidate=old;
  if(!credit_state_shape(old)||!tz||!tz[0]||strlen(tz)>=64)return 0;
  unsigned retired=0;
  if(old.deferred.bound && strcmp(old.deferred.timezone,tz)){
    candidate.deferred={};retired|=1;
  }
  if(old.schedule.bound && (old.schedule_observed || credit_same_origin(timer,old.schedule)) &&
      strcmp(old.schedule.timezone,tz)){
    candidate.schedule={};candidate.schedule_observed=false;retired|=2;
  }
  return credit_state_shape(candidate)?retired:0;
}

// Select a presently eligible origin, not a date high-water mark. A future
// deferred date must not block an earlier due schedule after clock correction.
static const NightlyCreditOrigin* credit_due_origin(const CoordinatorCreditState& s,uint64_t now,
                                                    bool fresh,const char* tz,bool allow_schedule=true) {
  const NightlyCreditOrigin* selected=nullptr;
  if(s.deferred.id[0] && nightly_credit_decide(s.deferred,s.deferred.id,now,fresh,tz)==NightlyCreditDecision::Due)
    selected=&s.deferred;
  if(allow_schedule && s.schedule.id[0]){
    const auto d=nightly_credit_decide(s.schedule,s.schedule.id,now,fresh,tz);
    if(d==NightlyCreditDecision::Due && (!selected || s.schedule.target_epoch<selected->target_epoch))
      selected=&s.schedule;
    if(d==NightlyCreditDecision::NonCalendar && !selected)selected=&s.schedule;
  }
  return selected;
}
static bool credit_claim_due(const CoordinatorCreditState& old,uint64_t now,bool fresh,
                             const char* tz,CoordinatorCreditState& candidate,bool allow_schedule=true) {
  if(!credit_state_shape(old))return false;
  const NightlyCreditOrigin* selected=credit_due_origin(old,now,fresh,tz,allow_schedule);
  if(!selected)return false;
  const NightlyCreditOrigin& origin=*selected;
  const auto d=nightly_credit_decide(origin,origin.id,now,fresh,tz);
  if(d!=NightlyCreditDecision::Due && d!=NightlyCreditDecision::NonCalendar)return false;
  // Never replace active work, even when a new timer carries another schedule.
  if(old.pending.id[0]&&!old.pending_resolved)return false;
  candidate=old;
  if(old.pending.id[0])memcpy(candidate.last_uncredited,old.pending.id,sizeof(candidate.last_uncredited));
  candidate.pending=origin;candidate.pending_resolved=false;
  const bool selected_deferred=selected==&old.deferred;
  if(selected_deferred)candidate.deferred={};
  if(old.schedule_observed && selected_deferred){
    if(strcmp(old.schedule.id,origin.id))candidate.deferred=old.schedule;
    candidate.schedule_observed=false;
  }else if(old.schedule_observed && selected==&old.schedule)candidate.schedule_observed=false;
  candidate.pending_credit_admitted=d==NightlyCreditDecision::Due;
  candidate.admitted_epoch=candidate.pending_credit_admitted?(uint32_t)now:0;
  return true;
}
// Re-admission is permitted only for newly admitted actual work whose existing
// original pending has a due, bound origin. This never replaces the pending ID.
static bool credit_admit_pending(const CoordinatorCreditState& old,uint64_t now,bool fresh,
                                 const char* tz,CoordinatorCreditState& candidate) {
  if(!credit_state_shape(old)||!old.pending.id[0]||old.pending_resolved||old.pending_credit_admitted)
    return false;
  if(nightly_credit_decide(old.pending,old.pending.id,now,fresh,tz)!=NightlyCreditDecision::Due)
    return false;
  candidate=old;candidate.pending_credit_admitted=true;candidate.admitted_epoch=(uint32_t)now;
  if(!strcmp(candidate.deferred.id,candidate.pending.id))candidate.deferred={};
  if(candidate.schedule_observed && !strcmp(candidate.schedule.id,candidate.pending.id))
    candidate.schedule_observed=false;
  return true;
}
static bool credit_recovery_needed(const CoordinatorCreditState& s,bool lcd_debt) {
  return lcd_debt || (s.pending.id[0]&&!s.pending_resolved);
}
enum class CreditResolution { Unresolved, CreditDue, ResolvedUncredited };
static CreditResolution credit_resolve(const CoordinatorCreditState& old,bool actual_pair_resolved,
                                      uint64_t now,bool fresh,const char* tz,
                                      bool clock_opportunity_finished,CoordinatorCreditState& candidate) {
  if(!credit_state_shape(old)||!old.pending.id[0]||!actual_pair_resolved)return CreditResolution::Unresolved;
  const auto d=nightly_credit_decide(old.pending,old.pending.id,now,fresh,tz);
  candidate=old;
  if(old.pending_resolved)return CreditResolution::ResolvedUncredited;
  // After a legitimate admitted OTA/restart, the first loop may precede SNTP
  // and configured TZ readiness. Preserve that original bounded opportunity;
  // absence of evidence at this instant is not its terminal failure.
  if(old.pending_credit_admitted && !clock_opportunity_finished &&
     (d==NightlyCreditDecision::ClockUnconfirmed || d==NightlyCreditDecision::TimezoneUnconfirmed))
    return CreditResolution::Unresolved;
  if((d==NightlyCreditDecision::Due && old.pending_credit_admitted)||d==NightlyCreditDecision::NonCalendar)
    return CreditResolution::CreditDue; // actual existing atomic done_ids commit must still succeed
  // A mere time/metadata problem does not make the verified repair unresolved.
  // Retain the ID and original record, but do not requeue it on each touch wake.
  candidate.pending_resolved=true;return CreditResolution::ResolvedUncredited;
}

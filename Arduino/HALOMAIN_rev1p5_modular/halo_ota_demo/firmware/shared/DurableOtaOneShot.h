#pragma once
#include "DurableOtaPolicy.h"

namespace durable_ota {
// Pure adapter contract. These are observed facts, never a UART parser or clock.
// The caller commits/readbacks every returned candidate before any side effect.
struct OneShotPeer {
  uint32_t boot{};
  char challenge[64]{};
  bool query_matched{}, local_valid{}, peer_valid{}, target_verified{}, idle{};
};
struct OneShotAck {
  char request[64]{}, challenge[64]{};
  uint32_t peer_boot{}, start_epoch{}, remaining_s{}, wake_in_s{};
  uint32_t duration_s{}, grace_before_s{}, grace_after_s{};
  bool clear{}, stored_verified{}, persisted{};
};
enum class OneShotSelection : uint8_t { NONE, FEATURE_OFF, CLOCK, UNVERIFIED, WAIT, DUE, EXPIRED, CLOSED };
struct OneShotTimer { OneShotSelection state; uint32_t delta_s; uint64_t microseconds; };

inline bool one_shot_request(const Record& r,char (&out)[64]) {
  if(!shape(r))return false;
  static const char hex[]="0123456789abcdef";
  memset(out,0,sizeof(out));memcpy(out,"oneshot_",8);
  for(unsigned i=0;i<16;++i){out[8+i*2]=hex[r.campaign[i]>>4];out[9+i*2]=hex[r.campaign[i]&15];}
  return true;
}
// Commit this reservation BEFORE the first peer query. The caller supplies
// its actual remaining control budget, never an invented full120 seconds.
inline bool one_shot_control_reserve(const Record& old,Clock c,uint32_t due,uint32_t requested_ms,uint32_t local_boot,
                                     bool local_valid,bool target_verified,bool busy,Record& out) {
  if(!HALO_OTA_ONE_SHOT || !shape(old) || !clock_valid(old,c) || !local_boot || !local_valid || !target_verified || busy ||
     old.one_shot.phase!=OneShotPhase::NONE || old.phase!=Phase::FAST || old.deferred_path ||
     old.fast_start || old.fast_opportunities || (!bench_active(old)&&(old.network_windows || old.day_attempts)) ||
     old.attempt_ordinal || (!bench_active(old)&&(old.begins[0] || old.begins[1])) || old.arm_id[0] ||
     (bench_active(old)&&(!bench_live(old,c)||old.network_windows>=network_cap(old)||old.day_attempts>=apply_cap(old))) ||
     !requested_ms || requested_ms>kPreflightMs || uint64_t(c.epoch)+initial_delay(old)>due ||
     uint64_t(c.epoch)+kOneShotMaximumDelay<due || uint64_t(due)+kOneShotWindow>UINT32_MAX || (bench_active(old)&&due>=old.bench.until))return false;
  const uint32_t grant=requested_ms<old.work_remaining_ms?requested_ms:old.work_remaining_ms;
  if(grant<1000 || uint64_t(c.epoch)+(grant+999)/1000>UINT32_MAX || !next(old,c,out))return false;
  carry_work_day(out,c);out.phase=Phase::ARM_PREPARATION;
  out.work_remaining_ms-=grant;out.reserved_work_ms=grant;out.active_started=c.epoch;
  out.active_deadline=c.epoch+(grant+999)/1000;
  OneShot& o=out.one_shot;o.phase=OneShotPhase::PREPARING;o.control_debited=true;o.arm_boot=local_boot;
  o.due=due;o.expiry=due+kOneShotWindow;
  return shape(out);
}
// Query succeeded under the ORIGINAL reservation. This neither settles nor
// extends it; the full query+ACK elapsed time is settled only after exact ACK.
inline bool one_shot_prepare(const Record& old,Clock c,uint32_t due,
                             const OneShotPeer& peer,uint32_t query_elapsed_ms,uint32_t local_boot,Record& out) {
  if(!HALO_OTA_ONE_SHOT || !shape(old) || !clock_valid(old,c) ||
     old.phase!=Phase::ARM_PREPARATION || old.one_shot.phase!=OneShotPhase::PREPARING ||
     !old.one_shot.control_debited || local_boot!=old.one_shot.arm_boot || c.epoch>=old.active_deadline || query_elapsed_ms>=old.reserved_work_ms || due!=old.one_shot.due ||
     !peer.boot || !text(peer.challenge,sizeof(peer.challenge)) || !peer.query_matched ||
     !peer.local_valid || !peer.peer_valid || !peer.target_verified || !peer.idle ||
     uint64_t(c.epoch)+kPeerLead>=due || !next(old,c,out))return false;
  OneShot& o=out.one_shot;o.phase=OneShotPhase::ARM_PENDING;o.arm_started=c.epoch;o.peer_boot=peer.boot;
  o.sent_remaining=due-c.epoch;o.query_elapsed_ms=query_elapsed_ms;memcpy(o.challenge,peer.challenge,sizeof(o.challenge));
  return shape(out);
}
// Elapsed values must come from the retained ORIGINAL same-boot monotonic
// start and ACK start. If that evidence is missing, close with no refund.
inline bool one_shot_confirm(const Record& old,Clock c,const OneShotAck& ack,
                             uint32_t current_peer_boot,uint32_t local_boot,uint32_t elapsed_ms,uint32_t total_control_elapsed_ms,bool idle,Record& out) {
  char request[64];
  if(!HALO_OTA_ONE_SHOT || !shape(old) || !clock_valid(old,c) ||
     old.phase!=Phase::ARM_PREPARATION || old.one_shot.phase!=OneShotPhase::ARM_PENDING ||
     !old.one_shot.control_debited || local_boot!=old.one_shot.arm_boot || !idle || c.epoch>=old.active_deadline ||
     total_control_elapsed_ms>=old.reserved_work_ms || uint64_t(old.one_shot.query_elapsed_ms)+elapsed_ms>total_control_elapsed_ms ||
     elapsed_ms>=kOneShotArmSeconds*1000 || c.epoch>=uint64_t(old.one_shot.arm_started)+kOneShotArmSeconds ||
     !one_shot_request(old,request) || !text(ack.request,sizeof(ack.request)) ||
     !text(ack.challenge,sizeof(ack.challenge)) || strcmp(ack.request,request) ||
     strcmp(ack.challenge,old.one_shot.challenge) || !current_peer_boot ||
     ack.peer_boot!=current_peer_boot || current_peer_boot!=old.one_shot.peer_boot ||
     ack.clear || !ack.stored_verified || !ack.persisted || ack.start_epoch!=old.one_shot.due ||
     ack.remaining_s!=old.one_shot.sent_remaining || ack.wake_in_s!=ack.remaining_s ||
     ack.duration_s || ack.grace_before_s || ack.grace_after_s ||
     uint64_t(c.epoch)+kPeerLead>=old.one_shot.due)return false;
  if(!next(old,c,out))return false;
  out.work_remaining_ms+=old.reserved_work_ms-total_control_elapsed_ms;
  clear_active(out);out.phase=Phase::FAST;
  out.one_shot.phase=OneShotPhase::ARMED;out.one_shot.ack_epoch=c.epoch;
  return shape(out);
}
inline OneShotTimer one_shot_timer(const Record& r,Clock c) {
  if(!shape(r))return {OneShotSelection::UNVERIFIED,0,0};
  if(bench_active(r)&&!bench_live(r,c))return {OneShotSelection::EXPIRED,0,0};
  if(r.one_shot.phase==OneShotPhase::NONE)return {OneShotSelection::NONE,0,0};
  if(r.one_shot.phase==OneShotPhase::CLOSED || r.one_shot.phase==OneShotPhase::CONSUMED)
    return {OneShotSelection::CLOSED,0,0};
  if(!HALO_OTA_ONE_SHOT)return {OneShotSelection::FEATURE_OFF,0,0};
  if(!r.one_shot.control_debited)return {OneShotSelection::UNVERIFIED,0,0};
  if(!clock_valid(r,c))return {OneShotSelection::CLOCK,0,0};
  if(c.epoch>=r.one_shot.expiry)return {OneShotSelection::EXPIRED,0,0};
  if(r.one_shot.phase!=OneShotPhase::ARMED)return {OneShotSelection::UNVERIFIED,0,0};
  if(c.epoch>=r.one_shot.due)return {OneShotSelection::DUE,0,0};
  uint32_t delta=r.one_shot.due-c.epoch;
  return {OneShotSelection::WAIT,delta,uint64_t(delta)*1000000ULL};
}
// Records actual final SDK return; it does not call the timer or prove sleep.
// A diagnostic journal records any later early-wake re-selection separately.
inline bool one_shot_note_timer(const Record& old,Clock c,uint32_t local_boot,
                                uint64_t actual_us,int32_t sdk_result,Record& out) {
  const OneShotTimer plan=one_shot_timer(old,c);
  if(plan.state!=OneShotSelection::WAIT || !local_boot || local_boot!=old.one_shot.arm_boot || old.one_shot.timer_recorded ||
     actual_us!=plan.microseconds || !next(old,c,out))return false;
  OneShot& o=out.one_shot;o.timer_recorded=true;o.timer_epoch=c.epoch;
  o.arm_boot=local_boot;o.timer_us=uint32_t(actual_us);o.timer_sdk=sdk_result;
  return shape(out);
}
inline Admission one_shot_reserve(const Record& old,Clock c,uint32_t current_boot,
                                  bool local_valid,bool busy,Record& out) {
  if(!HALO_OTA_ONE_SHOT)return Admission::NOT_DUE;
  if(!shape(old))return Admission::IDENTITY;
  if(!clock_valid(old,c))return Admission::CLOCK;
  if(busy)return Admission::BUSY;
  if((bench_active(old)&&!bench_live(old,c)) || !local_valid || !current_boot || !old.one_shot.control_debited || !old.one_shot.timer_recorded || old.one_shot.timer_sdk ||
     current_boot==old.one_shot.arm_boot)return Admission::IDENTITY;
  if(old.one_shot.phase!=OneShotPhase::ARMED || old.phase!=Phase::FAST ||
     c.epoch<old.one_shot.due || c.epoch>=old.one_shot.expiry)return Admission::NOT_DUE;
  return reserve_preflight_impl(old,c,false,out,true);
}
// Explicit close for expiry, failed arm/SDK proof or a user wake held through due.
// It preserves debt and charges, ending acceleration only. Never new credit.
inline bool one_shot_close(const Record& old,Clock c,uint32_t next_normal,Record& out) {
  if(!shape(old) || (old.one_shot.phase!=OneShotPhase::PREPARING && old.one_shot.phase!=OneShotPhase::ARM_PENDING &&
      old.one_shot.phase!=OneShotPhase::ARMED) || !next(old,c,out))return false;
  // Cancellation/timeout has no verified full control elapsed receipt: charge
  // the entire reservation, as on reset. Never refund unknown work.
  if(old.phase==Phase::ARM_PREPARATION)clear_active(out);
  return defer(out,next_normal)&&shape(out);
}
} // namespace durable_ota

#pragma once
#include "MaintenanceWitnessDigest.h"

namespace halo_sense_sleep {
constexpr size_t BYTES=64;
constexpr uint32_t PREPARED=1,ENTERED=2;
struct Record {
  uint32_t flags=0,prior_sense=0,prior_lcd=0,due=0,grace_before=0;
  uint8_t window_sha[32]{};
};
static_assert(sizeof(Record)==52,"bounded decoded cache view");
inline uint32_t get32(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline uint32_t crc32(const uint8_t*p,size_t n){uint32_t c=0xffffffffUL;for(size_t i=0;i<n;++i){c^=p[i];for(unsigned k=0;k<8;++k)c=(c>>1)^(0xedb88320UL&(0UL-(c&1)));}return ~c;}
inline bool nonzero(const uint8_t*p,size_t n){uint8_t v=0;for(size_t i=0;i<n;++i)v|=p[i];return v!=0;}
inline bool valid(const Record&r){return (r.flags==PREPARED||r.flags==(PREPARED|ENTERED))&&r.prior_sense&&r.prior_lcd&&r.due>=1700000000UL&&r.grace_before<r.due&&nonzero(r.window_sha,32);}
inline bool encode(const Record&r,uint8_t(&out)[BYTES]){
  if(!valid(r))return false;memset(out,0,BYTES);memcpy(out,"SWS1",4);
  using halo_maintenance_witness::put32;put32(out+4,r.flags);put32(out+8,r.prior_sense);
  put32(out+12,r.prior_lcd);put32(out+16,r.due);put32(out+20,r.grace_before);
  memcpy(out+24,r.window_sha,32);put32(out+60,crc32(out,60));return true;
}
inline bool decode(const uint8_t*p,size_t n,Record&out){
  if(!p||n!=BYTES||memcmp(p,"SWS1",4)||get32(p+56)||get32(p+60)!=crc32(p,60))return false;
  Record r;r.flags=get32(p+4);r.prior_sense=get32(p+8);r.prior_lcd=get32(p+12);r.due=get32(p+16);r.grace_before=get32(p+20);memcpy(r.window_sha,p+24,32);
  if(!valid(r))return false;out=r;return true;
}
inline void cancel(uint8_t(&retained)[BYTES]){memset(retained,0,BYTES);}
// Note a real sent supported arm only. A later fresh LCD witness must prove
// that exact digest was accepted; this local cache never fabricates an ACK.
inline bool stage(uint8_t(&retained)[BYTES],const halo_maintenance_witness::Window&w,
                  halo_maintenance_witness::Sha256 hash){
  cancel(retained);Record r;r.flags=PREPARED;r.prior_sense=w.prior_sense;r.prior_lcd=w.prior_lcd;
  r.due=w.start_epoch;r.grace_before=w.grace_before_s;
  return halo_maintenance_witness::digest(w,hash,r.window_sha)&&encode(r,retained);
}
// Called only at the actual terminal sleep boundary after the existing timer
// SDK call. A different selector, failed timer or cancellation leaves unknown.
inline bool commit_entry(uint8_t(&retained)[BYTES],uint32_t actual_sense,
                         uint32_t selected_due,int32_t timer_sdk,uint64_t timer_us){
  Record r;if(!decode(retained,BYTES,r)||r.flags!=PREPARED||r.prior_sense!=actual_sense||
     r.due!=selected_due||timer_sdk!=0||!timer_us){cancel(retained);return false;}
  r.flags|=ENTERED;return encode(r,retained);
}
enum class Quality:uint8_t{Unknown,DeepSleepPredecessor};
struct Observation {Record record{};uint32_t current_sense=0;Quality quality=Quality::Unknown;};
// Consume exactly once for every reset cause, including unknown/cold/software.
// RTC retention across power loss is not assumed. External deep-sleep wake is
// only a cache predecessor; the later accepted LCD origin still gates quality.
inline Observation consume(uint8_t(&retained)[BYTES],uint32_t current_sense,
                           unsigned actual_reset,unsigned actual_wake){
  Record r;const bool ok=decode(retained,BYTES,r);cancel(retained);Observation out;
  if(!ok||r.flags!=(PREPARED|ENTERED)||actual_reset!=8||(actual_wake!=2&&actual_wake!=4)||
     !current_sense||current_sense==r.prior_sense)return out;
  out.record=r;out.current_sense=current_sense;out.quality=Quality::DeepSleepPredecessor;return out;
}
inline bool matches(const Observation&o,uint32_t current_sense,uint32_t prior_lcd,
                    uint32_t due,const uint8_t(&digest)[32]){
  return o.quality==Quality::DeepSleepPredecessor&&o.current_sense==current_sense&&
    current_sense!=o.record.prior_sense&&o.record.prior_lcd==prior_lcd&&
    o.record.due==due&&!memcmp(o.record.window_sha,digest,32);
}
} // namespace halo_sense_sleep

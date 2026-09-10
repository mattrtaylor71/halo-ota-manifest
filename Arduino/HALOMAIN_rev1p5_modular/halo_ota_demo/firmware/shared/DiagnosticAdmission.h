#pragma once
#include "DiagnosticCapsule.h"

// Future prototype. Independent admission observation, never a d3 State and
// never policy authority. Actual Sense/LCD coordinator boot IDs are uint32_t.
namespace halo_admission {
constexpr size_t BYTES=64;
constexpr uint8_t FORMAT=0xb1, FRESH=2, TIME_MASK=3, SCHEDULED=4, LCD_VERIFIED=8;
constexpr uint32_t MIN_EPOCH=1700000000UL;
struct Record {
  uint8_t context_sha[32]{};
  uint32_t boot=0,epoch=0,due=0,generation=0,ordinal=0,lcd_prior_boot=0;
  uint8_t reset=0,wake=0,flags=0;
};
static_assert(sizeof(Record)<=64,"bounded admission RAM view");
inline bool valid(const Record&r){
  if(!halo_diag::nonzero(r.context_sha,32)||!r.boot||!r.generation||r.epoch<MIN_EPOCH||
     (r.flags&TIME_MASK)!=FRESH||(r.flags&0xf0))return false;
  if(r.due && (r.due<MIN_EPOCH||r.due>r.epoch))return false;
  if((r.flags&SCHEDULED)&&(!r.due||r.reset!=8||(r.wake!=2&&r.wake!=4)))return false;
  if(r.flags&LCD_VERIFIED){if(!(r.flags&SCHEDULED)||!r.lcd_prior_boot)return false;}
  else if(r.lcd_prior_boot)return false;
  return true;
}
inline bool encode(const Record&r,uint8_t(&b)[BYTES]){
  if(!valid(r))return false;
  memset(b,0,BYTES);memcpy(b,r.context_sha,32);
  const uint32_t fields[]={r.boot,r.epoch,r.due,r.generation,r.ordinal,r.lcd_prior_boot};
  for(unsigned i=0;i<6;++i)halo_diag::put32(b+32+i*4,fields[i]);
  b[56]=FORMAT;b[57]=r.reset;b[58]=r.wake;b[59]=r.flags;
  halo_diag::put32(b+60,halo_diag::crc32(b,60));return true;
}
inline bool decode(const uint8_t*b,size_t n,Record&r){
  if(!b||n!=BYTES||b[56]!=FORMAT||halo_diag::get32(b+60)!=halo_diag::crc32(b,60))return false;
  Record t;memcpy(t.context_sha,b,32);t.boot=halo_diag::get32(b+32);t.epoch=halo_diag::get32(b+36);
  t.due=halo_diag::get32(b+40);t.generation=halo_diag::get32(b+44);t.ordinal=halo_diag::get32(b+48);
  t.lcd_prior_boot=halo_diag::get32(b+52);t.reset=b[57];t.wake=b[58];t.flags=b[59];
  if(!valid(t))return false;r=t;return true;
}
// Matching IDF5.5.4 moves a64B first blob away from an insufficient page tail;
// one data header + two payload entries + one index. A typed live check remains
// necessary: available entries are not an allocation guarantee.
constexpr size_t BLOB_ENTRIES=4, ESSENTIAL_FLOOR=96, POLICY_PEAK=27;
inline bool capacity(size_t available,size_t missing_profile,bool policy_present,bool key_missing){
  if(missing_profile>49)return false;
  const size_t required=ESSENTIAL_FLOOR+missing_profile+(policy_present?POLICY_PEAK:2*POLICY_PEAK)+(key_missing?BLOB_ENTRIES:0);
  return available>=required;
}
} // namespace halo_admission

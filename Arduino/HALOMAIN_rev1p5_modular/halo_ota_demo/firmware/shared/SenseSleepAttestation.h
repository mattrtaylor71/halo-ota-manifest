#pragma once
#include "SenseSleepWitness.h"
#include "LcdSleepWitness.h"

namespace halo_sleep_attestation {
// A proof can only be formed by validating a typed, fresh nonce-bound peer
// receipt. It is observational and bound to this exact admitted context/record.
class Proof {
  uint8_t context_[32]{};
  uint32_t sense_=0,lcd_prior_=0,due_=0,generation_=0,ordinal_=0;
public:
  uint32_t prior_lcd() const {return lcd_prior_;}
  bool matches(const uint8_t*context,uint32_t sense,uint32_t due,uint32_t generation,
               uint32_t ordinal,uint32_t lcd_prior) const {
    return lcd_prior_&&context&&sense==sense_&&due==due_&&generation==generation_&&
      ordinal==ordinal_&&lcd_prior==lcd_prior_&&!memcmp(context,context_,32);
  }
  static bool from_receipt(const uint8_t(&raw)[72],unsigned reset,unsigned wake,unsigned quality,
      uint32_t current_peer,const halo_sense_sleep::Observation&cache,uint32_t current_sense,
      uint32_t due,uint32_t generation,uint32_t ordinal,const uint8_t(&context)[32],
      uint32_t actual_lcd_lead_s,Proof&out) {
    out={};
    if(reset!=8||wake!=4||quality!=2||!current_peer||!generation||
       !halo_sense_sleep::nonzero(context,32))return false;
    halo_lcd_sleep_witness::Snapshot lcd{};uint32_t flags=0;
    if(!halo_lcd_sleep_witness::decode(raw,lcd,flags)||flags!=3||lcd.sdk_result!=0||
       !lcd.selected_epoch||current_peer==lcd.prior_lcd_boot||
       !halo_sense_sleep::matches(cache,current_sense,lcd.prior_lcd_boot,due,lcd.request_sha256)||
       cache.record.prior_sense!=lcd.prior_sense_boot||lcd.due_epoch!=due)return false;
    const uint64_t lead=uint64_t(cache.record.grace_before)+actual_lcd_lead_s;
    if(uint64_t(due)<=lead||uint64_t(due)-lead<=lcd.selected_epoch||
       (uint64_t(due)-lead-lcd.selected_epoch)*1000000ULL!=lcd.timer_us)return false;
    memcpy(out.context_,context,32);out.sense_=current_sense;out.lcd_prior_=lcd.prior_lcd_boot;
    out.due_=due;out.generation_=generation;out.ordinal_=ordinal;return true;
  }
};
static_assert(sizeof(Proof)<=56,"bounded local attestation");
} // namespace halo_sleep_attestation

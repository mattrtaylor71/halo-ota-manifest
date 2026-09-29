#pragma once

#include "SystemPower.h"

// Presentation only. GPIO1 measures the system rail, not the cell. This private
// beta curve is a conservative voltage estimate, NOT calibrated state of charge.
// It must not drive OTA admission, charging, shutdown, or cloud battery fields.
namespace home_battery {
enum class Mode : uint8_t { Unknown, Battery, ExternalPower };
enum class Band : uint8_t { Neutral, Green, Yellow, Red };
struct Display {
  Mode mode = Mode::Unknown;
  Band band = Band::Neutral;
  uint8_t percent = 0;
};
inline bool operator==(const Display& a, const Display& b) {
  return a.mode == b.mode && a.band == b.band && a.percent == b.percent;
}
inline bool operator!=(const Display& a, const Display& b) { return !(a == b); }
inline Band band(uint8_t percent) {
  return percent <= 10 ? Band::Red : percent <= 25 ? Band::Yellow : Band::Green;
}
inline uint8_t estimate_percent(uint16_t mv) {
  // No diode-drop correction: that path is not characterized. These anchors
  // intentionally do not relabel the observed 3944 mV probe start as full.
  static constexpr uint16_t volts[] = {3300,3500,3600,3700,3800,3900,4000,4100,4200};
  static constexpr uint8_t percents[] = {0,5,10,25,50,75,90,97,100};
  if(mv <= volts[0]) return 0;
  for(unsigned i=1;i<sizeof(percents);++i) {
    if(mv <= volts[i]) return uint8_t(percents[i-1] +
      (uint32_t(mv-volts[i-1])*(percents[i]-percents[i-1]) +
       (volts[i]-volts[i-1])/2)/(volts[i]-volts[i-1]));
  }
  return 100;
}

class Model {
 public:
  Display update(const halo_power::View& view, bool usb_data_attached) {
    const auto& s = view.sample;
    if(!view.received || !view.age_known || view.age_ms > halo_power::kFreshMs ||
       !halo_power::valid(s) || s.system_supply_mv < 2500 || s.system_supply_mv > 5500) {
      reset(); return {};
    }
    const bool new_boot = boot_ != s.boot_id;
    if(!new_boot && sequence_ &&
       (s.sequence < sequence_ || s.uptime_ms < uptime_)) {
      // Do not let an old frame refresh the displayed estimate or hysteresis.
      return {};
    }
    const bool new_sample = new_boot || sequence_ != s.sequence;
    const uint64_t elapsed = new_boot ? 0 : s.uptime_ms-uptime_;
    if(new_boot || (new_sample && elapsed > halo_power::kFreshMs)) reset();

    // High rail is evidence of external power, not active battery charging.
    // SOF/USB data may veto a battery estimate but never assert a 5 V supply.
    const uint16_t mv = s.system_supply_mv;
    const bool external = mv >= 4400 || (external_ && mv > 4300);
    if(external != external_) clear_filter();
    external_ = external;
    if(external) {
      remember(s);
      return {Mode::ExternalPower, Band::Green, 0};
    }
    if(usb_data_attached || mv > 4250) {
      clear_filter(); remember(s); return {};
    }
    // Detaching USB data does not turn the previous ambiguous rail sample
    // into a battery measurement. Wait for the existing sampler's next burst.
    if(!count_ && !new_sample && sequence_) return {};

    if(new_sample || !count_) {
      samples_[next_] = mv;
      next_ = uint8_t((next_+1)%5);
      if(count_ < 5) ++count_;
      uint16_t sorted[5];
      for(uint8_t i=0;i<count_;++i) sorted[i]=samples_[i];
      for(uint8_t i=1;i<count_;++i) {
        const uint16_t value=sorted[i]; uint8_t j=i;
        while(j && sorted[j-1]>value) {sorted[j]=sorted[j-1];--j;}
        sorted[j]=value;
      }
      const int32_t target = int32_t(sorted[count_/2])*256;
      if(!filtered_) filtered_=target;
      else {
        // Time-aware causal low-pass, ~10 s. No state writes on duplicate reads.
        const uint32_t dt=elapsed ? uint32_t(elapsed) : 1000;
        filtered_ += int32_t(int64_t(target-filtered_)*dt/(10000+dt));
      }
    }
    remember(s);
    const auto percent=estimate_percent(uint16_t((filtered_+128)/256));
    return {Mode::Battery, band(percent), percent};
  }
  void reset() { boot_=sequence_=0; uptime_=0; external_=false; clear_filter(); }
 private:
  void clear_filter() { count_=next_=0; filtered_=0; }
  void remember(const halo_power::Snapshot& s) {
    boot_=s.boot_id; sequence_=s.sequence; uptime_=s.uptime_ms;
  }
  uint64_t uptime_=0;
  uint32_t boot_=0, sequence_=0;
  int32_t filtered_=0;
  uint16_t samples_[5]{};
  uint8_t count_=0, next_=0;
  bool external_=false;
};
static_assert(sizeof(Model) <= 40, "Review battery presentation state growth");
} // namespace home_battery

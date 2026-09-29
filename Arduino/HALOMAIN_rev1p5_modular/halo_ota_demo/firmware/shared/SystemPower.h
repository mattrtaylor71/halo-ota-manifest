#pragma once

// The LCD divider measures the system rail, not the cell or charge state.
// Pure, bounded value/formatting helpers: no ADC, allocation, clock or I/O.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <limits.h>

namespace halo_power {
constexpr uint8_t kSamples = 32;
constexpr uint64_t kFreshMs = 5000;
constexpr size_t kJsonCapacity = 512;
enum class Status : uint8_t {
  NotSampled=0, Ok=1, AdcUnavailable=2, CalibrationUnavailable=3,
  ReadError=4, Saturated=5, PeerMissing=6
};
struct Snapshot {
  uint64_t epoch_s=0, uptime_ms=0;
  uint32_t boot_id=0, sequence=0;
  uint16_t system_supply_mv=0, raw_min=0, raw_max=0;
  uint8_t samples=0;
  Status status=Status::NotSampled;
};
static_assert(sizeof(Snapshot) <= 40, "Review power snapshot RAM growth");

inline const char* status_name(Status s) {
  switch(s) {
    case Status::Ok:return "ok";
    case Status::AdcUnavailable:return "adc_unavailable";
    case Status::CalibrationUnavailable:return "calibration_unavailable";
    case Status::ReadError:return "read_error";
    case Status::Saturated:return "saturated";
    case Status::PeerMissing:return "peer_missing";
    default:return "not_sampled";
  }
}
inline bool valid(const Snapshot& s) {
  return s.status==Status::Ok && s.samples==kSamples && s.boot_id &&
    s.sequence && s.raw_min>0 && s.raw_max<4095 && s.raw_min<=s.raw_max &&
    s.system_supply_mv>0 && s.system_supply_mv<=6600;
}
struct Burst {
  uint32_t sum_mv=0;
  uint16_t raw_min=4095, raw_max=0;
  uint8_t samples=0;
  Status status=Status::Ok;
  bool add(int raw,int calibrated_mv) {
    if(status!=Status::Ok || samples>=kSamples) return false;
    if(raw<=0 || raw>=4095) {status=Status::Saturated;return false;}
    if(calibrated_mv<=0 || calibrated_mv>3300) {status=Status::ReadError;return false;}
    if(raw<raw_min) raw_min=uint16_t(raw);
    if(raw>raw_max) raw_max=uint16_t(raw);
    sum_mv+=uint32_t(calibrated_mv); ++samples;return true;
  }
  void finish(Snapshot& s) const {
    s.samples=samples;
    s.raw_min=samples?raw_min:0; s.raw_max=samples?raw_max:0;
    s.status=status==Status::Ok && samples!=kSamples?Status::ReadError:status;
    s.system_supply_mv=s.status==Status::Ok ? uint16_t(2*((sum_mv+kSamples/2)/kSamples)):0;
  }
};
struct View {
  Snapshot sample{};
  uint64_t age_ms=0;
  bool received=false;
  bool age_known=true;
};
struct Cache {
  Snapshot sample{};
  uint64_t received_ms=0, age_at_receive_ms=0;
  bool received=false;
  bool age_known=false;
  bool accept(const Snapshot& s,uint64_t age,uint64_t now,bool known=true) {
    if(!s.boot_id || !s.sequence || uint8_t(s.status)>uint8_t(Status::PeerMissing) ||
       s.samples>kSamples || (s.status==Status::Ok && !valid(s))) return false;
    // Duplicate/late frames cannot make an old value fresh again.
    if(received && s.boot_id==sample.boot_id && s.sequence<=sample.sequence) return false;
    sample=s;age_at_receive_ms=age;received_ms=now;received=true;age_known=known;return true;
  }
  View view(uint64_t now) const {
    View v;v.sample=sample;v.received=received;v.age_known=age_known;
    if(!received) {v.sample.status=Status::PeerMissing;return v;}
    const uint64_t elapsed=now>=received_ms?now-received_ms:UINT64_MAX;
    v.age_ms=elapsed>UINT64_MAX-age_at_receive_ms?UINT64_MAX:elapsed+age_at_receive_ms;
    return v;
  }
};

inline bool json(const View& v,char* out,size_t capacity) {
  if(!out || !capacity) return false;
  const Snapshot& s=v.sample;
  const bool measured=v.received && valid(s);
  const bool fresh=measured && v.age_known && v.age_ms<=kFreshMs;
  char voltage[8]="null",age[24]="null",epoch[24]="null";
  if(measured) snprintf(voltage,sizeof(voltage),"%u",unsigned(s.system_supply_mv));
  if(v.received && v.age_known) snprintf(age,sizeof(age),"%llu",(unsigned long long)v.age_ms);
  if(v.received && s.epoch_s>=1700000000ULL)
    snprintf(epoch,sizeof(epoch),"%llu",(unsigned long long)s.epoch_s);
  const int n=snprintf(out,capacity,
    "{\"schema\":1,\"measurement\":\"lcd_system_supply\",\"system_supply_mv\":%s,"
    "\"status\":\"%s\",\"valid\":%s,\"fresh\":%s,\"age_ms\":%s,\"age_known\":%s,"
    "\"sample_epoch_s\":%s,\"sample_uptime_ms\":%llu,\"sample_boot_id\":%lu,"
    "\"sample_sequence\":%lu,\"samples\":%u,\"raw_min\":%u,\"raw_max\":%u,"
    "\"power_source\":\"unknown\",\"cell_mv\":null,\"battery_percent\":null}",
    voltage,status_name(v.received?s.status:Status::PeerMissing),
    measured?"true":"false",fresh?"true":"false",age,v.received&&v.age_known?"true":"false",epoch,
    (unsigned long long)s.uptime_ms,(unsigned long)s.boot_id,(unsigned long)s.sequence,
    unsigned(s.samples),unsigned(s.raw_min),unsigned(s.raw_max));
  if(n<0 || size_t(n)>=capacity) {out[0]='\0';return false;}
  return true;
}
} // namespace halo_power

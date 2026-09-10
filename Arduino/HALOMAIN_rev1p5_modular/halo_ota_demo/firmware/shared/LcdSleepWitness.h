#pragma once
// Pure observational prototype. No SDK, timer, NVS, transport or policy calls.
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace halo_lcd_sleep_witness {
static const size_t kBytes = 72;
static const uint32_t kConfigured = 1, kEntered = 2;
// Values of the reviewed ESP-IDF enums; the integration must static_assert them.
static const uint32_t kDeepSleepReset = 8, kExt0Wake = 2, kExt1Wake = 3, kTimerWake = 4;
struct Snapshot {
  uint32_t prior_lcd_boot, prior_sense_boot, due_epoch, selected_epoch;
  uint64_t timer_us;
  int32_t sdk_result;
  uint8_t request_sha256[32];
};
enum class Quality : uint8_t { Unknown = 0, DiagnosticOnly = 1, TimerPredecessor = 2 };
struct Observation {
  Snapshot selected;
  uint32_t current_lcd_boot, reset_reason, wake_cause;
  Quality quality;
};
inline uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8*i)); }
inline uint64_t get64(const uint8_t* p) { return uint64_t(get32(p)) | (uint64_t(get32(p+4)) << 32); }
inline void put64(uint8_t* p, uint64_t v) { put32(p,uint32_t(v)); put32(p+4,uint32_t(v>>32)); }
inline uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
inline bool nonzero_hash(const uint8_t* p) { uint8_t any = 0; for (unsigned i=0;i<32;++i) any |= p[i]; return any != 0; }
inline bool valid_snapshot(const Snapshot& s) {
  return s.prior_lcd_boot && s.prior_sense_boot && s.due_epoch && s.timer_us && nonzero_hash(s.request_sha256);
}
inline void invalidate(uint8_t (&rtc)[kBytes]) { memset(rtc,0,kBytes); }
inline void encode(uint8_t (&rtc)[kBytes], const Snapshot& s, uint32_t flags) {
  invalidate(rtc);
  rtc[0]='L'; rtc[1]='S'; rtc[2]='W'; rtc[3]=1;
  put32(rtc+4,flags); put32(rtc+8,s.prior_lcd_boot); put32(rtc+12,s.prior_sense_boot);
  put32(rtc+16,s.due_epoch); put32(rtc+20,s.selected_epoch); put64(rtc+24,s.timer_us);
  put32(rtc+32,uint32_t(s.sdk_result)); memcpy(rtc+36,s.request_sha256,32); put32(rtc+68,crc32(rtc,68));
}
inline bool decode(const uint8_t (&rtc)[kBytes], Snapshot& s, uint32_t& flags) {
  memset(&s,0,sizeof(s)); flags=0;
  if (rtc[0]!='L' || rtc[1]!='S' || rtc[2]!='W' || rtc[3]!=1 || get32(rtc+68)!=crc32(rtc,68)) return false;
  flags=get32(rtc+4);
  if (!(flags & kConfigured) || (flags & ~(kConfigured|kEntered))) return false;
  s.prior_lcd_boot=get32(rtc+8); s.prior_sense_boot=get32(rtc+12); s.due_epoch=get32(rtc+16);
  s.selected_epoch=get32(rtc+20); s.timer_us=get64(rtc+24);
  // memcpy preserves the reviewed two's-complement SDK result bits without a
  // uint32-to-int32 out-of-range numeric conversion.
  uint32_t sdk=get32(rtc+32); memcpy(&s.sdk_result,&sdk,sizeof(sdk));
  memcpy(s.request_sha256,rtc+36,32); return valid_snapshot(s);
}
// Call once immediately after the actual timer SDK call. A new capture replaces
// the old candidate, including when the replacement is inadmissible.
inline bool capture_after_sdk(uint8_t (&rtc)[kBytes], const Snapshot& actual) {
  invalidate(rtc);
  if (!valid_snapshot(actual)) return false;
  encode(rtc,actual,kConfigured); return true;
}
// Call only at the final sleep-entry boundary, after all cancellation guards.
inline bool commit_before_sleep(uint8_t (&rtc)[kBytes]) {
  Snapshot s; uint32_t flags;
  if (!decode(rtc,s,flags)) { invalidate(rtc); return false; }
  put32(rtc+4,flags|kEntered); put32(rtc+68,crc32(rtc,68)); return true;
}
// Boot consumes every candidate, including corrupt/cold-reset data, once. The
// returned RAM copy may answer several fresh queries during this current boot.
inline Observation consume_on_boot(uint8_t (&rtc)[kBytes], uint32_t reset,
                                   uint32_t wake, uint32_t current_lcd_boot) {
  Observation out = {}; Snapshot s; uint32_t flags;
  bool decoded=decode(rtc,s,flags); invalidate(rtc);
  out.current_lcd_boot=current_lcd_boot; out.reset_reason=reset; out.wake_cause=wake;
  if (!decoded || reset!=kDeepSleepReset || !(flags&kEntered) || !current_lcd_boot || current_lcd_boot==s.prior_lcd_boot) return out;
  if (wake!=kTimerWake && wake!=kExt0Wake && wake!=kExt1Wake) return out;
  out.selected=s; out.quality=Quality::DiagnosticOnly;
  if (wake==kTimerWake && s.sdk_result==0 && s.selected_epoch) out.quality=Quality::TimerPredecessor;
  return out;
}
// A fresh nonce/current-peer check is the caller's responsibility. The exact
// checked selector tuple binds due/selected epoch/timer without a guessed rule.
inline bool matches_expected_selector(const Observation& o, uint32_t current_lcd_boot,
                                      const Snapshot& expected) {
  const Snapshot& a=o.selected;
  return o.quality==Quality::TimerPredecessor && o.current_lcd_boot==current_lcd_boot &&
    a.prior_lcd_boot==expected.prior_lcd_boot && a.prior_sense_boot==expected.prior_sense_boot &&
    a.due_epoch==expected.due_epoch && a.selected_epoch==expected.selected_epoch &&
    a.timer_us==expected.timer_us && a.sdk_result==expected.sdk_result &&
    memcmp(a.request_sha256,expected.request_sha256,32)==0;
}
} // namespace halo_lcd_sleep_witness

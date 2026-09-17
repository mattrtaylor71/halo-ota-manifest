#pragma once
#include <stdint.h>

// A scheduling hint, never proof of media custody. Only a validated empty
// inventory may clear a store; timeouts and failed transfers keep it pending.
namespace halo_media_retry {
enum Store : uint8_t { VoiceSd = 1, ImageSd = 2, VoiceFlash = 4 };
static constexpr uint8_t kAllStores = VoiceSd | ImageSd | VoiceFlash;
static constexpr uint32_t kMagic = 0x4d520100U;
struct State {
  uint8_t pending = kAllStores; // cold/missing metadata: discover, don't assume empty
  uint8_t backoff = 0;
  bool image_first = false;
};
inline uint32_t encode(const State& s) {
  return kMagic | (s.pending & kAllStores) | ((s.backoff & 3U) << 3) |
         (s.image_first ? 32U : 0U);
}
inline bool decode(uint32_t word, State& s) {
  if ((word & ~63U) != kMagic) return false;
  s.pending = word & kAllStores;
  s.backoff = (word >> 3) & 3U;
  s.image_first = (word & 32U) != 0;
  return true;
}
inline void inventory(State& s, Store store, bool nonempty) {
  if (nonempty) s.pending |= store;
  else s.pending &= ~store;
  if (!s.pending) s.backoff = 0;
}
inline void saved(State& s, Store store) {
  s.pending |= store;
  s.backoff = 0;
}
inline uint32_t interval(const State& s, bool made_progress) {
  static constexpr uint32_t seconds[] = {300, 900, 3600, 21600};
  return !s.pending ? 0 : (made_progress ? 60 : seconds[s.backoff & 3U]);
}
inline void sleep_committed(State& s, bool made_progress) {
  if (!s.pending || made_progress) s.backoff = 0;
  else if (s.backoff < 3) ++s.backoff;
}
inline void attempted(State& s, Store store) {
  // Each namespace gets a turn even when the other's oldest request fails.
  s.image_first = store == VoiceSd;
}
inline bool selected(uint32_t actual, uint32_t media, uint32_t earlier) {
  // OTA and all previously selected safety timers win ties.
  return media && media < earlier && actual == media;
}
} // namespace halo_media_retry

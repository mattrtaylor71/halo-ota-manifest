#pragma once

#include <stdint.h>
#include <string.h>

// Relative LCD media rendezvous. This record has no OTA identity or wall clock.
// The adapter retains it in RTC memory and supplies monotonic time per boot.
namespace halo_media_timer {
static constexpr uint32_t kMagic = 0x4d525431;
static constexpr uint32_t kMinArmSeconds = 60;
static constexpr uint32_t kMaxArmSeconds = 21600;
static constexpr uint32_t kLeadSeconds = 5;
static constexpr uint32_t kMinimumSleepSeconds = 5;

struct State {
  uint32_t magic = 0;
  uint32_t requested_s = 0;
  uint64_t remaining_us = 0;
  uint32_t slept_s = 0;
  uint8_t armed = 0;
  uint8_t selected = 0;
  char token[17] = {};
};

inline bool token_valid(const char* token) {
  if (!token || strnlen(token, 17) != 16) return false;
  for (unsigned i = 0; i < 16; ++i)
    if (!((token[i] >= '0' && token[i] <= '9') ||
          (token[i] >= 'a' && token[i] <= 'f'))) return false;
  return true;
}

inline bool valid(const State& state) {
  return state.magic == kMagic && token_valid(state.token) &&
      state.armed <= 1 && state.selected <= 1 &&
      (!state.requested_s || (state.requested_s >= kMinArmSeconds &&
                             state.requested_s <= kMaxArmSeconds)) &&
      state.remaining_us <= uint64_t(kMaxArmSeconds) * 1000000ULL &&
      (!state.armed || state.requested_s != 0);
}

inline void elapse(State& state, uint64_t elapsed_us) {
  state.remaining_us = elapsed_us < state.remaining_us ? state.remaining_us - elapsed_us : 0;
}

inline void account(State& state, uint64_t& checkpoint_us, uint64_t now_us) {
  if (valid(state) && state.armed && now_us >= checkpoint_us)
    elapse(state, now_us - checkpoint_us);
  checkpoint_us = now_us;
}

inline bool arm(State& state, uint64_t& checkpoint_us, uint64_t now_us,
                const char* token, uint32_t seconds) {
  if (!token_valid(token) || (seconds &&
      (seconds < kMinArmSeconds || seconds > kMaxArmSeconds))) return false;
  account(state, checkpoint_us, now_us);
  // A retransmitted ACK request must not move the original deadline. Reusing
  // an identity for a different nonzero interval is an invalid request.
  if (seconds && valid(state) && !strcmp(state.token, token))
    return state.requested_s == seconds;
  state = State{};
  state.magic = kMagic;
  memcpy(state.token, token, sizeof(state.token));
  state.requested_s = seconds;
  state.armed = seconds != 0;
  state.remaining_us = seconds ? uint64_t(seconds - kLeadSeconds) * 1000000ULL : 0;
  return true;
}

inline bool boot(State& state, uint64_t& checkpoint_us, uint64_t now_us,
                 bool deep_sleep_reset, bool timer_wake) {
  checkpoint_us = now_us;
  if (!deep_sleep_reset || !valid(state)) { state = State{}; return false; }
  const bool media_wake = timer_wake && state.selected && state.armed && state.slept_s;
  if (media_wake) {
    state.armed = 0;
    state.remaining_us = 0;
  } else if (timer_wake && state.armed) {
    // An earlier OTA/fallback timer consumed a known interval. A touch wake
    // supplies no trustworthy elapsed sleep time, so retain a conservative
    // remainder until Sense refreshes its independent pending-work hint.
    elapse(state, uint64_t(state.slept_s) * 1000000ULL);
  }
  state.selected = 0;
  state.slept_s = 0;
  return media_wake;
}

inline uint32_t choose(State& state, uint64_t& checkpoint_us, uint64_t now_us,
                       uint32_t existing_s, bool* media_selected) {
  *media_selected = false;
  account(state, checkpoint_us, now_us);
  if (!valid(state) || !state.armed) return existing_s;
  uint32_t media_s = uint32_t((state.remaining_us + 999999ULL) / 1000000ULL);
  if (media_s < kMinimumSleepSeconds) media_s = kMinimumSleepSeconds;
  // An existing OTA/fallback wake wins a tie. Never rewrite its provenance.
  if (existing_s && existing_s <= media_s) return existing_s;
  *media_selected = true;
  return media_s;
}

inline void commit_sleep(State& state, uint64_t& checkpoint_us, uint64_t now_us,
                         uint32_t seconds, bool media_selected, bool timer_ok) {
  account(state, checkpoint_us, now_us);
  if (!valid(state)) return;
  state.slept_s = timer_ok ? seconds : 0;
  state.selected = timer_ok && seconds && media_selected && state.armed;
}
}  // namespace halo_media_timer

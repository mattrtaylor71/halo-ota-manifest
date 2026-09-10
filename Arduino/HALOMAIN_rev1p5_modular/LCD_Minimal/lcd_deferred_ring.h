/*
 * lcd_deferred_ring.h
 *
 * The deferred-until-awake TX ring, split out of lcd_uart.h so it can be tested
 * on the host.
 *
 * This is the REAL code the firmware compiles — tools/test_deferred_ring.c
 * includes THIS file with small shims. It is deliberately not a copy: a UB fix
 * once went into shared/UartOtaProtocol.cpp while a duplicate in the sketch dir
 * kept compiling, and a valid 512-byte chunk decoded as data_len=59649 for days
 * after the fix was written, built and flashed. Never test a copy.
 *
 * Requires, before inclusion:
 *   - tx_msg_t              (message struct, with a .type char array)
 *   - millis()              (monotonic ms)
 *   - DEFERRED_RING_LOGF()  (printf-style logger)
 */

#ifndef LCD_DEFERRED_RING_H
#define LCD_DEFERRED_RING_H

#include <string.h>

#ifndef DEFERRED_RING_LOGF
#define DEFERRED_RING_LOGF(...) Serial.printf(__VA_ARGS__)
#endif

// ── Deferred-until-awake ring ────────────────────────────────────────
//
// Was a SINGLE slot, and the TX drain stopped dead at the first message that
// needed awake-proof. Everything behind it waited too — including messages that
// need no proof at all (pings, diagnostics), which is what actually made this
// head-of-line blocking rather than just "one message waits".
//
// A ring lets the drain step over a waiting message and keep sending the ones
// that can go now. Order is preserved AMONG the deferred messages, which is the
// part that matters: two user actions must never swap. A diagnostic overtaking a
// waiting user action is fine and is the whole point.
//
// Four deep: these only accumulate while the Sense is unreachable, and each is
// independently abandoned after DEFERRED_AWAKE_TX_MAX_MS, so the window in which
// more than a handful can pile up is small. Drop-oldest with a logged count —
// silently discarding the oldest user action is exactly the failure this layer
// exists to prevent, so it must be visible.
#define DEFERRED_AWAKE_RING_SLOTS 4

typedef struct {
  tx_msg_t msg;
  unsigned long since_ms;    // when this message was first deferred
} deferred_tx_entry_t;

static deferred_tx_entry_t deferred_ring[DEFERRED_AWAKE_RING_SLOTS];
static uint8_t  deferred_ring_head  = 0;   // oldest
static uint8_t  deferred_ring_count = 0;
static uint32_t deferred_ring_dropped = 0;

static inline bool deferred_awake_tx_pending() { return deferred_ring_count > 0; }

static inline deferred_tx_entry_t* deferred_ring_oldest() {
  return deferred_ring_count ? &deferred_ring[deferred_ring_head] : nullptr;
}

static inline void deferred_ring_pop_oldest() {
  if (!deferred_ring_count) return;
  deferred_ring_head = (uint8_t)((deferred_ring_head + 1) % DEFERRED_AWAKE_RING_SLOTS);
  deferred_ring_count--;
}

static inline void deferred_ring_clear() {
  deferred_ring_head = 0;
  deferred_ring_count = 0;
}

// Returns true if a message was evicted to make room.
static inline bool deferred_ring_push(const tx_msg_t* msg) {
  bool evicted = false;
  if (deferred_ring_count == DEFERRED_AWAKE_RING_SLOTS) {
    deferred_ring_dropped++;
    DEFERRED_RING_LOGF("[UART_TX_DROP] deferred ring full - DROPPED oldest type=%s "
                  "total_ring_dropped=%lu\n",
                  deferred_ring[deferred_ring_head].msg.type,
                  (unsigned long)deferred_ring_dropped);
    deferred_ring_pop_oldest();
    evicted = true;
  }
  const uint8_t idx = (uint8_t)((deferred_ring_head + deferred_ring_count) % DEFERRED_AWAKE_RING_SLOTS);
  deferred_ring[idx].msg = *msg;
  deferred_ring[idx].since_ms = millis();
  deferred_ring_count++;
  return evicted;
}

// True if any deferred message is of this type (used to cancel a pending
// INPUT_SENSE_FW when the answer already arrived).
static inline bool deferred_ring_has_type(const char* type) {
  for (uint8_t i = 0; i < deferred_ring_count; i++) {
    const uint8_t idx = (uint8_t)((deferred_ring_head + i) % DEFERRED_AWAKE_RING_SLOTS);
    if (strcmp(deferred_ring[idx].msg.type, type) == 0) return true;
  }
  return false;
}

// Remove every deferred message of this type, preserving the order of the rest.
static inline void deferred_ring_remove_type(const char* type) {
  deferred_tx_entry_t kept[DEFERRED_AWAKE_RING_SLOTS];
  uint8_t n = 0;
  for (uint8_t i = 0; i < deferred_ring_count; i++) {
    const uint8_t idx = (uint8_t)((deferred_ring_head + i) % DEFERRED_AWAKE_RING_SLOTS);
    if (strcmp(deferred_ring[idx].msg.type, type) != 0) kept[n++] = deferred_ring[idx];
  }
  for (uint8_t i = 0; i < n; i++) deferred_ring[i] = kept[i];
  deferred_ring_head = 0;
  deferred_ring_count = n;
}

#endif  // LCD_DEFERRED_RING_H

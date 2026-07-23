#pragma once
// Generic PSRAM ring buffer used by the Wi-Fi net task (tx/rx).
//
// This type lives in a header (rather than inline in the .ino) on purpose:
// arduino-cli auto-generates prototypes for the ring_* helpers and injects them
// right after the #include block, BEFORE any top-level typedef in the .ino body.
// Defining sb_ring_t here — and #including it among the other headers — guarantees
// the type is visible to those injected prototypes no matter where the helpers or
// other declarations sit in the .ino. (Prior inline placements kept breaking each
// time the file was regenerated and the prototype-injection point shifted.)
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"

typedef struct {
  uint8_t* buf; size_t sz;
  volatile size_t head, tail;
  portMUX_TYPE mux;
} sb_ring_t;

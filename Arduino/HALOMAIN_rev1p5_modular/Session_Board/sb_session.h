#pragma once
// Session record for one Claude Code terminal the daemon streams to the board.
//
// Defined in a header (not inline in the .ino) for the same reason as sb_ring_t:
// arduino-cli auto-generates prototypes for the helpers that take sb_session_t*
// (dot_color/dot_pulses/sess_changed/…) and injects them right after the #include
// block, BEFORE any top-level typedef in the .ino body. Including this header among
// the other headers guarantees the type is visible to those injected prototypes
// regardless of where the includes/defines above the struct shift to. (Adding the
// WiFiManager include shifted the injection point and re-exposed this exact bug.)
#include <stdint.h>

typedef struct {
  char id[12];      // 8-char anchor key
  char name[28];
  char proj[24];
  char status;      // 'w' working, 'd' done, 'i' idle
  uint32_t age_s;
  char msg[512];
  char dtl[768];    // richer detail text (from "x" lines); empty -> detail falls back to msg
  uint8_t agn;      // sub-agent processes working for this session (0 if absent)
  uint8_t aga;      // sub-agents currently active (<= agn)
  uint8_t drm;      // dormant: terminal not actively reachable (grey dot + "may not respond")
} sb_session_t;

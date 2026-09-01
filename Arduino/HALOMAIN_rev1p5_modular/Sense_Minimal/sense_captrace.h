// sense_captrace.h — Sense half of the end-to-end capture trace.
//
// Pairs with LCD_Minimal/lcd_captrace.h. The LCD half says WHICH board is at
// fault when a capture strands; this half says WHERE inside the Sense it stopped.
//
// The load-bearing pair is CAM_GRAB_BEGIN / CAM_GRAB_END around
// esp_camera_fb_get(). That call is unbounded — it takes no timeout, and every
// budget check in sense_camera.h happens AFTER it returns, so a sensor that
// never delivers a frame is never caught by anything. A BEGIN with no matching
// END in the log is therefore direct proof of a camera stall, which no existing
// log line can currently show.
//
// Correlation id is the LCD's msg_id, carried on the INPUT_MENU_SELECT that
// started the capture — same id the LCD prints, so the two logs line up.

#pragma once

#include <Arduino.h>
#include <string.h>

static uint32_t      g_sense_captrace_id = 0;
static unsigned long g_sense_captrace_t0 = 0;
static char          g_sense_captrace_mode[24] = "";

// Capture request received from the LCD.
static inline void sense_captrace_begin(uint32_t msg_id, const char* menu_item) {
  g_sense_captrace_id = msg_id;
  g_sense_captrace_t0 = millis();
  snprintf(g_sense_captrace_mode, sizeof(g_sense_captrace_mode), "%s",
           menu_item ? menu_item : "");
  Serial.printf("[CAPTRACE] cap=%lu hop=sense_rx item=%s\n",
                (unsigned long)msg_id, g_sense_captrace_mode);
}

// A stage boundary inside the Sense. `detail` may be NULL.
static inline void sense_captrace_mark(const char* stage, const char* detail) {
  if (g_sense_captrace_t0 == 0) return;   // no capture in flight
  Serial.printf("[CAPTRACE] cap=%lu hop=%s t+%lums%s%s\n",
                (unsigned long)g_sense_captrace_id,
                stage ? stage : "?",
                millis() - g_sense_captrace_t0,
                detail ? " " : "",
                detail ? detail : "");
}

// Same, with an integer payload (bytes, error code, elapsed).
static inline void sense_captrace_mark_i(const char* stage, const char* key, long value) {
  if (g_sense_captrace_t0 == 0) return;
  Serial.printf("[CAPTRACE] cap=%lu hop=%s t+%lums %s=%ld\n",
                (unsigned long)g_sense_captrace_id,
                stage ? stage : "?",
                millis() - g_sense_captrace_t0,
                key ? key : "v", value);
}

// Capture reached a terminal state on the Sense side.
static inline void sense_captrace_end(const char* outcome) {
  if (g_sense_captrace_t0 == 0) return;
  Serial.printf("[CAPTRACE] cap=%lu hop=sense_done outcome=%s total=%lums\n",
                (unsigned long)g_sense_captrace_id,
                outcome ? outcome : "?",
                millis() - g_sense_captrace_t0);
  g_sense_captrace_t0 = 0;
}

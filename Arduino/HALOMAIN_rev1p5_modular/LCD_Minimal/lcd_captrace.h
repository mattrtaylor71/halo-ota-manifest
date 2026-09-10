// lcd_captrace.h — end-to-end trace for a single capture, LCD side.
//
// WHY THIS EXISTS
// The reported fault is "tap Check In, UI strands on the capturing screen".
// Two very different causes produce that identical symptom:
//
//   (a) the request never reached the Sense, or the Sense never answered
//       (cold UART link, Sense asleep, deferred-TX slot overwritten) — the link
//       layer is fire-and-forget for INPUT_*, so a lost request is invisible;
//   (b) the Sense answered, started capturing, and then stalled inside
//       esp_camera_fb_get(), which is unbounded and has no timeout.
//
// Today the only evidence is "[SCAN_TIMEOUT] no response from Sense in N ms",
// which cannot tell those apart — and they need opposite fixes. This records
// how far each capture actually got, so the next strand answers the question
// instead of prompting another guess.
//
// Correlation id is the EXISTING per-message msg_id (lcd_uart.h get_next_msg_id).
// No new protocol field, nothing for the Sense to echo before this is useful.
//
// Cost: a handful of statics and one printf per capture. Safe to leave in.

#pragma once

#include <Arduino.h>
#include <string.h>

static uint32_t      g_captrace_msg_id = 0;        // correlation id (msg_id of the request)
static unsigned long g_captrace_sent_ms = 0;       // when the request went out
static char          g_captrace_mode[24] = "";     // check / discard / dish
static char          g_captrace_last_phase[24] = "";
static unsigned long g_captrace_last_phase_ms = 0; // when the last phase arrived
static uint16_t      g_captrace_status_count = 0;  // how many status msgs came back
static unsigned long g_captrace_rx_baseline_ms = 0;// last_sense_rx_ms at send time

// A capture request is going out. Resets the record.
static inline void captrace_request(uint32_t msg_id, const char* mode,
                                    unsigned long last_sense_rx_ms) {
  g_captrace_msg_id = msg_id;
  g_captrace_sent_ms = millis();
  g_captrace_status_count = 0;
  g_captrace_last_phase[0] = '\0';
  g_captrace_last_phase_ms = 0;
  g_captrace_rx_baseline_ms = last_sense_rx_ms;
  snprintf(g_captrace_mode, sizeof(g_captrace_mode), "%s", mode ? mode : "");
  Serial.printf("[CAPTRACE] cap=%lu hop=lcd_tx mode=%s\n",
                (unsigned long)msg_id, g_captrace_mode);
}

// A status phase came back from the Sense for the in-flight capture.
static inline void captrace_phase(const char* op, const char* phase) {
  if (g_captrace_sent_ms == 0) return;              // no capture in flight
  if (!op || strcmp(op, "SCAN") != 0) return;       // only the capture op
  g_captrace_status_count++;
  g_captrace_last_phase_ms = millis();
  snprintf(g_captrace_last_phase, sizeof(g_captrace_last_phase), "%s", phase ? phase : "");
  Serial.printf("[CAPTRACE] cap=%lu hop=lcd_rx phase=%s t+%lums n=%u\n",
                (unsigned long)g_captrace_msg_id, g_captrace_last_phase,
                g_captrace_last_phase_ms - g_captrace_sent_ms,
                (unsigned)g_captrace_status_count);
  // Close the trace on a terminal phase, otherwise a strand later in the
  // session would report this stale capture's numbers instead of its own.
  if (strcmp(g_captrace_last_phase, "DONE") == 0 ||
      strcmp(g_captrace_last_phase, "ERROR") == 0) {
    Serial.printf("[CAPTRACE] cap=%lu hop=done why=phase_%s total=%lums n=%u\n",
                  (unsigned long)g_captrace_msg_id, g_captrace_last_phase,
                  millis() - g_captrace_sent_ms, (unsigned)g_captrace_status_count);
    g_captrace_sent_ms = 0;
  }
}

// The capture reached a terminal state normally — stop tracing it.
static inline void captrace_done(const char* why) {
  if (g_captrace_sent_ms == 0) return;
  Serial.printf("[CAPTRACE] cap=%lu hop=done why=%s total=%lums n=%u\n",
                (unsigned long)g_captrace_msg_id, why ? why : "",
                millis() - g_captrace_sent_ms, (unsigned)g_captrace_status_count);
  g_captrace_sent_ms = 0;
}

// The strand watchdog fired. Dump everything, and state the verdict plainly:
// whether the Sense was ever heard from at all decides which bug this is.
static inline void captrace_strand(unsigned long age_ms, unsigned long last_sense_rx_ms) {
  bool heard_from_sense = (last_sense_rx_ms != g_captrace_rx_baseline_ms);
  const char* verdict;
  if (g_captrace_status_count == 0 && !heard_from_sense) {
    verdict = "NO_RESPONSE_AT_ALL(link_or_sense_asleep)";
  } else if (g_captrace_status_count == 0) {
    verdict = "SENSE_ALIVE_BUT_NO_SCAN_STATUS(request_lost_or_dropped)";
  } else {
    verdict = "STALLED_MID_CAPTURE(see_last_phase)";
  }
  Serial.printf("[CAPTRACE] cap=%lu hop=STRAND age=%lums mode=%s n=%u last_phase=%s "
                "last_phase_age=%lums heard_from_sense=%d verdict=%s\n",
                (unsigned long)g_captrace_msg_id, age_ms, g_captrace_mode,
                (unsigned)g_captrace_status_count,
                g_captrace_last_phase[0] ? g_captrace_last_phase : "(none)",
                g_captrace_last_phase_ms ? (millis() - g_captrace_last_phase_ms) : 0,
                heard_from_sense ? 1 : 0, verdict);
  g_captrace_sent_ms = 0;
}

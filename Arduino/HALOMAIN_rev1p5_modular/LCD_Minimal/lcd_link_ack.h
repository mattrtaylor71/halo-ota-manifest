// lcd_link_ack.h — ACK + retransmit for the LCD→Sense messages that carry user
// intent.
//
// WHY: the link is fire-and-forget for exactly the messages that matter. There
// are four bespoke ack types (SYNC_ACK, MAINT_WINDOW_ACK, WIFI_ON_ACK,
// WIFI_CREDS_ACK), and none of them cover INPUT_*. A capture request lost to a
// cold or noisy UART is simply gone: the Sense never scans, the LCD never hears
// back, and the user sits on the capturing screen until a watchdog fires. That
// is the single best explanation for the reported hang, and no amount of
// Sense-side hardening can fix it — the Sense never saw the request.
//
// WHAT: every tracked message is held until the Sense acks its msg_id.
// Unacked messages are retransmitted on a fixed interval, and after a bounded
// number of attempts the send is declared failed and the UI is told, so the
// screen resolves with an honest error instead of hanging.
//
// TWO THINGS THIS MUST GET RIGHT:
//
//  1. A retransmit reuses the ORIGINAL msg_id and the original serialized
//     bytes. Re-serializing would mint a new msg_id (get_next_msg_id()
//     increments), and the Sense would have no way to tell a retransmit from a
//     second deliberate button press.
//  2. Because of (1) the Sense MUST dedupe on msg_id — see the seen-ring in
//     sense_uart_msg.h. Without it, a retransmit that arrives after a delayed
//     first copy fires the capture twice. Retransmission without dedupe is a
//     worse bug than the one being fixed.
//
// Only messages that carry user intent are tracked. INPUT_SCROLL and INPUT_PING
// are deliberately excluded: they are high-frequency, idempotent-ish, and
// retransmitting them would add link load during exactly the moments the link
// is already struggling.

#pragma once

#include <Arduino.h>

#ifndef LINK_ACK_SLOTS
#define LINK_ACK_SLOTS 4
#endif
#ifndef LINK_ACK_RETRY_MS
#define LINK_ACK_RETRY_MS 400
#endif
#ifndef LINK_ACK_MAX_ATTEMPTS
#define LINK_ACK_MAX_ATTEMPTS 5      // ~2.0s before we give up and tell the user
#endif
#ifndef LINK_ACK_PAYLOAD_MAX
#define LINK_ACK_PAYLOAD_MAX 224
#endif

typedef struct {
  bool     valid;
  uint32_t msg_id;
  char     type[32];
  char     payload[LINK_ACK_PAYLOAD_MAX];
  uint32_t first_sent_ms;
  uint32_t last_sent_ms;
  uint8_t  attempts;          // 1 == original transmission
} link_ack_slot_t;

static link_ack_slot_t g_link_ack[LINK_ACK_SLOTS];

// Counters — surfaced by the `linkstats` USB command and worth watching in soak.
static uint32_t g_link_ack_tracked   = 0;
static uint32_t g_link_ack_acked     = 0;
static uint32_t g_link_ack_retries   = 0;
static uint32_t g_link_ack_failed    = 0;
static uint32_t g_link_ack_overflow  = 0;
static uint32_t g_link_ack_rtt_max_ms = 0;

// Which messages are worth the retransmit machinery. Mirrors the
// input_requires_sense() set in lcd_uart.h: these are the ones where the Sense
// must act, so silent loss is user-visible.
static bool link_ack_should_track(const char* type) {
  if (!type) return false;
  return strcmp(type, "INPUT_MENU_SELECT") == 0 ||      // capture / check-in / dish
         strcmp(type, "INPUT_WAKE") == 0 ||
         strcmp(type, "INPUT_USER_ACTIVE") == 0 ||
         strcmp(type, "INPUT_DELETE") == 0 ||
         strcmp(type, "INPUT_LONG_PRESS_START") == 0 || // voice hold
         strcmp(type, "INPUT_LONG_PRESS_END") == 0 ||
         strcmp(type, "INPUT_RETRY") == 0 ||
         strcmp(type, "INPUT_OTA_CHECK") == 0 ||
         strcmp(type, "INPUT_RESET_WIFI") == 0 ||
         strcmp(type, "INPUT_FW_INFO") == 0 ||
         strcmp(type, "INPUT_SENSE_FW") == 0;
}

// Register a just-sent message. `payload` must be the exact bytes put on the
// wire, WITHOUT the trailing newline.
static void link_ack_track(uint32_t msg_id, const char* type, const char* payload) {
#if HALO_DEMO_MODE
  // Demo unit: there is no peer to ACK, so tracking a retransmit can only ever
  // end one way — the retry budget expires, EVT_LINK_SEND_FAILED fires, and the
  // UI shows "Couldn't reach sensor", which sets g_ship_ui_finalized and makes
  // every later UI_STATUS route as IGNORED_LATE. Observed on hardware: the
  // USB-injected INPUT_MENU_SELECT path calls this directly right after
  // uart_send_json(), so the demo swallowed the send but the ack layer still
  // armed. Nothing in the demo needs retransmits — it answers itself.
  (void)msg_id; (void)type; (void)payload;
  return;
#endif
  if (!link_ack_should_track(type) || !payload) return;
  const size_t plen = strlen(payload);
  if (plen >= LINK_ACK_PAYLOAD_MAX) {
    // Too big to replay verbatim. Tracking it would mean re-serializing, which
    // would change msg_id and defeat the Sense's dedupe — so don't pretend.
    Serial.printf("[LINK_ACK] not tracked (payload %u >= %u): %s\n",
                  (unsigned)plen, (unsigned)LINK_ACK_PAYLOAD_MAX, type);
    return;
  }
  for (int i = 0; i < LINK_ACK_SLOTS; i++) {
    if (g_link_ack[i].valid) continue;
    g_link_ack[i].valid         = true;
    g_link_ack[i].msg_id        = msg_id;
    g_link_ack[i].attempts      = 1;
    g_link_ack[i].first_sent_ms = millis();
    g_link_ack[i].last_sent_ms  = g_link_ack[i].first_sent_ms;
    snprintf(g_link_ack[i].type, sizeof(g_link_ack[i].type), "%s", type);
    memcpy(g_link_ack[i].payload, payload, plen + 1);
    g_link_ack_tracked++;
    return;
  }
  // All slots busy means the Sense has stopped acking several messages at once,
  // which is itself the failure this exists to report. Say so rather than
  // silently dropping the guarantee.
  g_link_ack_overflow++;
  Serial.printf("[LINK_ACK] OVERFLOW no slot for %s msg_id=%lu (unacked=%d)\n",
                type, (unsigned long)msg_id, LINK_ACK_SLOTS);
}

// Clear on ack. Returns true if this ack matched something we were tracking.
static bool link_ack_on_ack(uint32_t msg_id) {
  for (int i = 0; i < LINK_ACK_SLOTS; i++) {
    if (!g_link_ack[i].valid || g_link_ack[i].msg_id != msg_id) continue;
    const uint32_t rtt = millis() - g_link_ack[i].first_sent_ms;
    if (rtt > g_link_ack_rtt_max_ms) g_link_ack_rtt_max_ms = rtt;
    if (g_link_ack[i].attempts > 1) {
      // Worth a line: this is a message that WOULD have been lost before.
      Serial.printf("[LINK_ACK] recovered %s msg_id=%lu after %u attempts (%lums)\n",
                    g_link_ack[i].type, (unsigned long)msg_id,
                    (unsigned)g_link_ack[i].attempts, (unsigned long)rtt);
    }
    g_link_ack[i].valid = false;
    g_link_ack_acked++;
    return true;
  }
  return false;
}

// Called from the UART task loop. Retransmits overdue messages and gives up on
// ones past their attempt budget.
static void link_ack_service() {
  const uint32_t now = millis();
  for (int i = 0; i < LINK_ACK_SLOTS; i++) {
    if (!g_link_ack[i].valid) continue;
    if ((int32_t)(now - g_link_ack[i].last_sent_ms) < (int32_t)LINK_ACK_RETRY_MS) continue;

    if (g_link_ack[i].attempts >= LINK_ACK_MAX_ATTEMPTS) {
      Serial.printf("[LINK_ACK] GIVE_UP %s msg_id=%lu after %u attempts (%lums)\n",
                    g_link_ack[i].type, (unsigned long)g_link_ack[i].msg_id,
                    (unsigned)g_link_ack[i].attempts,
                    (unsigned long)(now - g_link_ack[i].first_sent_ms));
      g_link_ack_failed++;
      lcd_errlog_store_with_context("lcd", "link", "SENSE_NO_ACK", -1,
                                    g_link_ack[i].type);
      // Tell the UI so the screen resolves. NOT a direct UI call: this runs on
      // Core 0 and LVGL must only be touched from the UI task.
      if (!strcmp(g_link_ack[i].type, "INPUT_OTA_CHECK")) {
        lcd_manual_ota_finish("request_failed");
      } else if (!strcmp(g_link_ack[i].type, "INPUT_DELETE")) {
        StaticJsonDocument<256> failed;
        if (!deserializeJson(failed, g_link_ack[i].payload)) post_list_delete_result(failed["id"] | "", false);
      } else if (strcmp(g_link_ack[i].type, "INPUT_USER_ACTIVE") != 0 && app_event_queue != NULL) {
        // A background-pause notice must not replace the user's current UI.
        // Its bounded failure remains available in the diagnostic log above.
        app_event_t evt = {EVT_LINK_SEND_FAILED, {0}};
        xQueueSend(app_event_queue, &evt, 0);
      }
      g_link_ack[i].valid = false;
      continue;
    }

    // Retransmit the ORIGINAL bytes verbatim, same msg_id. The Sense dedupes.
    senseSerial.print(g_link_ack[i].payload);
    senseSerial.print("\n");
    senseSerial.flush();
    g_link_ack[i].attempts++;
    g_link_ack[i].last_sent_ms = now;
    g_link_ack_retries++;
    Serial.printf("[LINK_ACK] retry %u/%u %s msg_id=%lu\n",
                  (unsigned)g_link_ack[i].attempts, (unsigned)LINK_ACK_MAX_ATTEMPTS,
                  g_link_ack[i].type, (unsigned long)g_link_ack[i].msg_id);
  }
}

// True while any user-intent message is still awaiting an ack. Used to hold off
// sleep: sleeping on top of an unacked capture request throws the user's action
// away silently, which is the exact class of loss this layer exists to end.
// Bounded by construction — a slot lives at most
// LINK_ACK_MAX_ATTEMPTS * LINK_ACK_RETRY_MS (~2s) before it is retired — so this
// can delay sleep briefly but can never prevent it.
static bool link_ack_inflight() {
  for (int i = 0; i < LINK_ACK_SLOTS; i++) {
    if (g_link_ack[i].valid) return true;
  }
  return false;
}

static void link_ack_print_stats() {
  Serial.printf("[LINK_ACK] tracked=%lu acked=%lu retries=%lu failed=%lu overflow=%lu rtt_max=%lums\n",
                (unsigned long)g_link_ack_tracked, (unsigned long)g_link_ack_acked,
                (unsigned long)g_link_ack_retries, (unsigned long)g_link_ack_failed,
                (unsigned long)g_link_ack_overflow, (unsigned long)g_link_ack_rtt_max_ms);
  for (int i = 0; i < LINK_ACK_SLOTS; i++) {
    if (!g_link_ack[i].valid) continue;
    Serial.printf("[LINK_ACK]   inflight %s msg_id=%lu attempts=%u age=%lums\n",
                  g_link_ack[i].type, (unsigned long)g_link_ack[i].msg_id,
                  (unsigned)g_link_ack[i].attempts,
                  (unsigned long)(millis() - g_link_ack[i].first_sent_ms));
  }
}

// sense_spool_drain.h — pull spooled captures back off the LCD's SD card and
// upload them.
//
// WHY: sense_img_spool.h saves a photo to the LCD's SD card when SPIFFS cannot
// (every real capture: the partition is 173,441 B and images are 177-189 KB).
// Without this file those images are written and never sent — durable, and
// useless. This is the other half of "capture reliably AND get it to the
// backend".
//
// SHAPE: the Sense owns WiFi/TLS/presign/retry and the LCD owns the card, so the
// bytes come back over the same 115200 UART (~16s each). That cost is paid only
// when the device is idle and the user has walked away, which is exactly when
// the spool has something in it.
//
// A state machine rather than a blocking call: the SPOOL_LIST / SPOOL_FETCH_READY
// replies arrive through the normal RX dispatch, and blocking the main loop for
// 16s would stall INPUT_* handling — a user walking up mid-drain must not wait.
//
// DELETION IS THE SUBTLE PART. The slot is removed only after the upload is
// CONFIRMED (Sense_Minimal.ino, the put-success path). Deleting at queue time
// would be a silent regression to the original bug: an upload that then failed
// would have nowhere to fall back to, because SPIFFS still cannot hold the image.

#pragma once

#include <Arduino.h>

#ifndef SPOOL_DRAIN_INTERVAL_MS
#define SPOOL_DRAIN_INTERVAL_MS 30000   // how often to ask when idle
#endif
#ifndef SPOOL_DRAIN_REPLY_TIMEOUT_MS
#define SPOOL_DRAIN_REPLY_TIMEOUT_MS 4000
#endif
#ifndef SPOOL_DRAIN_FRAME_TIMEOUT_MS
#define SPOOL_DRAIN_FRAME_TIMEOUT_MS 5000
#endif

enum SpoolDrainState : uint8_t {
  SPOOL_IDLE = 0,
  SPOOL_WAIT_LIST,
  SPOOL_WAIT_READY,
  SPOOL_RECEIVING,
};

static SpoolDrainState g_spool_state = SPOOL_IDLE;
// g_spool_owns_uart is declared early in Sense_Minimal.ino, before sense_uart.h,
// because pump_uart_rx_once() must test it and that header is included long
// before this one.
static uint32_t g_spool_slot = 0;
static uint32_t g_spool_len = 0;
static uint32_t g_spool_got = 0;
static uint16_t g_spool_next_seq = 0;
static uint8_t* g_spool_buf = nullptr;
static UploadJob g_spool_job = {};
static uint32_t g_spool_deadline_ms = 0;
static uint32_t g_spool_next_try_ms = 0;
static UartOtaProtocol* g_spool_proto = nullptr;

// Set once a drained image is queued; cleared when the upload is confirmed (and
// the slot deleted) or when it fails (slot kept for another attempt).
static uint32_t g_drain_slot_inflight = 0;
static uint32_t g_drain_job_id = 0;

static uint32_t g_spool_drained_ok = 0;
static uint32_t g_spool_drain_fails = 0;
// Last spool depth the LCD reported. Recorded in the wake log so an overnight
// run shows whether images are accumulating on the card rather than draining.
static uint16_t g_spool_last_known_depth = 0;

// ── Drain scheduling ──────────────────────────────────────────────
//
// The drain used to never run at all. sense_spool_drain_tick() needs an empty
// upload queue, and arms a SPOOL_DRAIN_INTERVAL_MS throttle every time that
// check fails -- so on a normal wake the queue is busy, the throttle is armed,
// and the device sleeps before it expires. Every wake logged spool=0, which did
// not mean the card was empty: it meant nothing ever asked. A forced drain found
// 29 images stranded.
//
// Two things fix that, and BOTH have to avoid the traps found on 2026-09-01:
//
//   1. Ask how deep the card is ONCE PER WAKE, from the main loop. Not from the
//      sleep path -- a probe there has to pump UART RX by hand, which dispatches
//      sleep-handshake messages at a point the sleep state machine does not
//      expect them, and every Check-in cycle then hung at 245s.
//
//   2. Do the actual transfer on a wake of its own. Draining after a capture
//      stretched the wake to 246s; the device was then still awake when the next
//      tap arrived, making that tap's capture the SECOND camera init of the same
//      boot -- which this hardware cannot do -- so CAMERA_FAIL appeared on cycles
//      that had nothing wrong with them.
//
// So: probe on any wake, transfer only on a timer wake that captured nothing.

// Depth is asked for once per boot -- but "asked" means ANSWERED, not "sent".
// A single fire-and-forget attempt silently did nothing on any boot where the
// LCD link had not synced yet, and those are common: the wake log went back to
// spool=0 and no drain wake was ever armed. Retry until the LCD answers or we
// run out of attempts.
static bool g_spool_probe_done = false;
// "The LCD told us a number", as distinct from "we stopped asking". Conflating
// the two made a drain wake conclude the card was EMPTY whenever the probe went
// unanswered -- observed as a wake logging spool=0 drain=empty while 35 images
// sat on the card. An unanswered probe means we do not know, and not knowing
// must not be treated as good news.
static bool g_spool_probe_answered = false;
static uint8_t g_spool_probe_attempts = 0;
static uint32_t g_spool_probe_next_ms = 0;
#ifndef SPOOL_PROBE_MAX_ATTEMPTS
#define SPOOL_PROBE_MAX_ATTEMPTS 4
#endif
#ifndef SPOOL_PROBE_RETRY_MS
#define SPOOL_PROBE_RETRY_MS 5000
#endif
// While true, a SPOOL_LIST reply only records the depth and does NOT start a
// fetch. This is what makes the probe cheap enough to run on a capture wake.
static bool g_spool_probe_only = false;

// g_spool_drain_wake / g_spool_drain_wake_start_ms / _ok_at_start are declared
// early in Sense_Minimal.ino, because sense_can_sleep_now() must test them and
// is defined well before this header is included.
// Wall-clock ceiling for a drain wake. A transfer is ~16s of UART at 115200 plus
// an upload, so this is about three images. Bounded because a drain upload
// registers as op_inflight, and while it runs every LCD sleep request comes back
// "SLEEP_DENY reason=op_inflight" -- an unbounded drain is a device that never
// sleeps, which is worse than a late photo.
#ifndef SPOOL_DRAIN_WAKE_BUDGET_MS
#define SPOOL_DRAIN_WAKE_BUDGET_MS 90000
#endif

// How soon to come back when the card still holds captures.
#ifndef SPOOL_DRAIN_WAKE_S
#define SPOOL_DRAIN_WAKE_S 300
#endif

// OFF until the LCD is co-woken. READ THIS BEFORE TURNING IT ON.
//
// Everything on the Sense side works: the wake arms, the device holds itself
// awake for its budget instead of sleeping through the transfer, the outcome is
// recorded, and the backoff bounds it. What does not work is the other board.
//
// A Sense timer wake wakes ONLY the Sense. The LCD owns the SD card and is still
// in its own deep sleep, so it never answers SPOOL_LIST -- measured as drain
// wakes running their full 90s budget and recovering nothing, with the probe
// unanswered every time. This is a known property of the link, documented at
// sense_link_recent(): "after a TIMER wake the LCD does not know the Sense woke
// at all".
//
// Enabling this as-is is a pure battery cost: ~90s of radio-on after every user
// interaction, for nothing. The missing piece is co-scheduling -- the OTA path
// already solves the same problem by having BOTH boards arm their own timers via
// send_maint_window(..., wake_in_s, ...) and MAINT_WINDOW_ACK. The drain needs
// the same handshake before this can be 1. That path is entangled with nightly
// OTA scheduling, so it wants its own pass rather than a bolt-on.
#ifndef SPOOL_DRAIN_WAKE_ENABLED
#define SPOOL_DRAIN_WAKE_ENABLED 0
#endif
// Consecutive drain wakes that recovered nothing. Survives deep sleep (which is
// all we need; losing it to a reset just means we try again, which is harmless).
//
// Without this, a spool the device can never send -- a corrupt slot, an LCD that
// stops answering -- would wake it every 5 minutes forever and flatten the
// battery, which is a far worse outcome than a late photo.
RTC_DATA_ATTR static uint8_t g_spool_barren_wakes = 0;
#ifndef SPOOL_DRAIN_MAX_BARREN_WAKES
#define SPOOL_DRAIN_MAX_BARREN_WAKES 3
#endif

// Should sleep arm a short wake to come back and drain? Read at sleep time from
// cached state only -- it must NOT touch the UART. See the note above about the
// pre-sleep probe that broke the sleep handshake.
static bool sense_spool_wants_early_wake() {
#if !SPOOL_DRAIN_WAKE_ENABLED
  return false;   // see SPOOL_DRAIN_WAKE_ENABLED above
#endif
  if (g_spool_last_known_depth == 0) return false;
  if (g_spool_barren_wakes >= SPOOL_DRAIN_MAX_BARREN_WAKES) return false;
  return true;
}

// Close out a drain wake: record why it ended and whether it achieved anything.
//
// This MUST happen where the wake ends, not in the sleep path. It was in the
// sleep path, gated on g_spool_drain_wake -- which the end-of-wake check had
// already cleared, so the barren counter never incremented and the backoff never
// engaged. A drain that can never succeed would then wake the device every 300s
// forever. Observed directly: "arming a 300s drain wake (depth=35 barren=0)"
// after many consecutive wakes that recovered nothing.
static void sense_spool_drain_wake_finish(uint8_t why) {
  if (!g_spool_drain_wake) return;
  const uint32_t drained = g_spool_drained_ok - g_spool_drain_wake_ok_at_start;
  g_cycle_drain_end = why;
  g_spool_drain_wake = false;
  if (drained > 0) {
    g_spool_barren_wakes = 0;
  } else if (g_spool_barren_wakes < 0xFF) {
    g_spool_barren_wakes++;
  }
  Serial.printf("[SPOOL_DRAIN] drain wake ended: %s drained=%lu barren=%u depth=%u\n",
                wakelog_drain_end_str(why), (unsigned long)drained,
                (unsigned)g_spool_barren_wakes, (unsigned)g_spool_last_known_depth);
}

// Retained for the sleep path's benefit; see above for where the real work is.
static void sense_spool_note_drain_wake_result(uint32_t drained_this_wake) {
  if (drained_this_wake > 0) {
    g_spool_barren_wakes = 0;
    return;
  }
  if (g_spool_barren_wakes < 0xFF) g_spool_barren_wakes++;
  if (g_spool_barren_wakes >= SPOOL_DRAIN_MAX_BARREN_WAKES) {
    Serial.printf("[SPOOL_DRAIN] %u barren drain wakes - backing off, depth=%u stays put\n",
                  (unsigned)g_spool_barren_wakes, (unsigned)g_spool_last_known_depth);
  }
}

#if HALO_SPOOL_TEST
// Bench-only: run the drain but stop before queueing the upload, and instead
// verify the received bytes against the spooltest pattern.
//
// The spooltest image is a synthetic 180KB pattern, not a real JPEG. Driving it
// through the full drain would PUT it to the real backend and create a junk
// check-in in someone's actual kitchen. This exercises everything that is new
// and risky - SPOOL_LIST, SPOOL_FETCH, the reverse COBS transfer, the metadata
// round-trip - and stops at the one step that has side effects off-device.
static bool g_spool_verify_only = false;
#endif

static void sense_spool_drain_reset(const char* why) {
  if (g_spool_buf) { free(g_spool_buf); g_spool_buf = nullptr; }
  if (g_spool_state != SPOOL_IDLE) {
    Serial.printf("[SPOOL_DRAIN] reset state=%d reason=%s\n", (int)g_spool_state, why ? why : "");
  }
  g_spool_state = SPOOL_IDLE;
  g_spool_owns_uart = false;
  g_spool_slot = g_spool_len = g_spool_got = 0;
  g_spool_next_seq = 0;
  g_spool_next_try_ms = millis() + SPOOL_DRAIN_INTERVAL_MS;
}

// A user action arrived mid-drain. The drain is strictly lower priority than
// anything the person standing at the device is doing, so give the link back.
static void sense_spool_drain_yield_to_user() {
  if (g_spool_state == SPOOL_IDLE) return;

  // Tell the LCD to stop RIGHT NOW if a transfer is actually in flight.
  //
  // Abandoning silently leaves the LCD streaming into a Sense that has stopped
  // listening, and it only notices after a 4s ack timeout — four seconds of the
  // link tied up by a dead transfer at precisely the moment someone is standing
  // at the device. A NACK is what lcd_spool_send_file() already treats as an
  // abort (it checks `rtype != MSG_IMG_ACK`), so this needs no new protocol.
  //
  // Sent through the proto rather than as JSON because the link is mid-COBS:
  // a JSON line here would be exactly the stream corruption fixed three times
  // elsewhere.
  if (g_spool_state == SPOOL_RECEIVING && g_spool_proto) {
    g_spool_proto->send_frame(MSG_IMG_NACK, g_spool_next_seq, nullptr, 0);
    Serial.println("[SPOOL_DRAIN] user acted - NACKed to stop the LCD immediately");
  }
  sense_spool_drain_reset("user_input");
  g_spool_drain_fails++;
}

static bool sense_spool_drain_conditions_ok() {
  if (!wifi_is_connected()) return false;
  if (upload_inflight || upload_queue_count() > 0) return false;
  if (dish_scan_inflight || scan_ui_inflight) return false;
  if (g_drain_slot_inflight != 0) return false;   // one at a time
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active()) return false;
#endif
  return true;
}

// Called from the main loop.
static void sense_spool_drain_tick() {
  const uint32_t now = millis();

  if (g_spool_state != SPOOL_IDLE && g_spool_deadline_ms &&
      (int32_t)(now - g_spool_deadline_ms) >= 0) {
    sense_spool_drain_reset("timeout");
    g_spool_drain_fails++;
    return;
  }
  if (g_spool_state != SPOOL_IDLE) return;
  if (g_spool_next_try_ms && (int32_t)(now - g_spool_next_try_ms) < 0) return;
  if (!sense_spool_drain_conditions_ok()) {
    g_spool_next_try_ms = now + SPOOL_DRAIN_INTERVAL_MS;
    return;
  }

  StaticJsonDocument<128> d;
  d["ver"] = PROTOCOL_VERSION;
  d["type"] = "SPOOL_LIST_REQ";
  d["msg_id"] = get_next_msg_id();
  d["ts"] = now;
  String out; serializeJson(d, out);
  uart_send_json(out.c_str());
  g_spool_state = SPOOL_WAIT_LIST;
  g_spool_deadline_ms = now + SPOOL_DRAIN_REPLY_TIMEOUT_MS;
}

// Ask the LCD how deep the card is, once per boot, from the MAIN LOOP.
//
// No hand-pumping of UART RX: the SPOOL_LIST reply comes back through the normal
// dispatch like any other message. That is the difference between this and the
// pre-sleep probe that broke the sleep handshake.
//
// Costs one JSON round trip and tells us whether to bother waking up early.
static void sense_spool_probe_tick() {
  if (g_spool_probe_done) return;
  if (g_spool_state != SPOOL_IDLE) return;
  // The ONLY hard requirement is that nobody else owns the UART. A COBS transfer
  // in either direction must not have a JSON line dropped into it.
  if (g_spool_owns_uart || g_img_spool_tx_active || g_lcd_ota_proxy_owns_uart) return;

  // NOT gated on link_synced. That gate looked reasonable and silently disabled
  // the probe on every boot the LCD did not re-SYNC -- which includes any boot
  // where the LCD already thinks the link is up, so the wake log went straight
  // back to spool=0. An unanswered probe is already handled: it times out in 4s
  // and retries, and gives up quietly after SPOOL_PROBE_MAX_ATTEMPTS.

  // Deliberately NOT gated on WiFi or an idle upload queue. Those are the
  // conditions the drain tick needs before it moves 170KB and uploads it, and
  // requiring them is precisely why that tick never fired: the queue is busy for
  // almost all of a wake, and the device sleeps seconds after it clears. This is
  // one short JSON round trip to the LCD -- no radio, no allocation -- and it can
  // run alongside an upload as safely as the SENSE_DIAG lines already do.
  if (g_spool_probe_next_ms && (int32_t)(millis() - g_spool_probe_next_ms) < 0) return;
  if (g_spool_probe_attempts >= SPOOL_PROBE_MAX_ATTEMPTS) {
    g_spool_probe_done = true;   // give up for this boot, quietly
    Serial.println("[SPOOL_DRAIN] probe: no answer from LCD this boot");
    return;
  }
  g_spool_probe_attempts++;
  g_spool_probe_next_ms = millis() + SPOOL_PROBE_RETRY_MS;
  g_spool_probe_only = true;
  Serial.printf("[SPOOL_DRAIN] probe: asking LCD (attempt %u/%u)\n",
                (unsigned)g_spool_probe_attempts, (unsigned)SPOOL_PROBE_MAX_ATTEMPTS);

  StaticJsonDocument<128> d;
  d["ver"] = PROTOCOL_VERSION;
  d["type"] = "SPOOL_LIST_REQ";
  d["msg_id"] = get_next_msg_id();
  d["ts"] = millis();
  String out; serializeJson(d, out);
  uart_send_json(out.c_str());
  g_spool_state = SPOOL_WAIT_LIST;
  g_spool_deadline_ms = millis() + SPOOL_DRAIN_REPLY_TIMEOUT_MS;
}

// True while this wake exists to empty the card and nothing else.
static bool sense_spool_drain_wake_active() {
  if (!g_spool_drain_wake) return false;
  // A capture means a person is here. Their wake is not ours to extend.
  if (g_cycle_captures > 0 || foreground_active) {
    sense_spool_drain_wake_finish(WAKE_DRAIN_USER);
    return false;
  }
  if (g_spool_drain_wake_start_ms &&
      (millis() - g_spool_drain_wake_start_ms) > SPOOL_DRAIN_WAKE_BUDGET_MS) {
    sense_spool_drain_wake_finish(WAKE_DRAIN_BUDGET);
    return false;
  }
  if (g_spool_probe_answered && g_spool_last_known_depth == 0 &&
      g_spool_state == SPOOL_IDLE && g_drain_slot_inflight == 0) {
    sense_spool_drain_wake_finish(WAKE_DRAIN_EMPTY);
    return false;
  }
  g_cycle_drain_end = WAKE_DRAIN_ACTIVE;
  return true;
}

// Main-loop driver for a drain wake. Keeps the throttle out of the way so slots
// go back to back instead of one every 30s, but changes nothing else -- the
// transfer and the upload run exactly as they do from any other caller.
static void sense_spool_drain_wake_tick() {
  if (!sense_spool_drain_wake_active()) return;
  if (g_spool_state != SPOOL_IDLE) return;      // a transfer is already running
  if (g_drain_slot_inflight != 0) return;       // waiting on an upload to confirm
  g_spool_probe_only = false;
  g_spool_next_try_ms = 0;
  sense_spool_drain_tick();
}

#if HALO_SPOOL_TEST
// Bench-only: kick a drain immediately, bypassing the idle/WiFi/queue gating so
// the transfer can be tested on demand rather than by waiting for the device to
// go quiet at the right moment.
static void sense_spool_drain_force(bool verify_only) {
  sense_spool_drain_reset("force");
  g_spool_verify_only = verify_only;
  g_spool_next_try_ms = 0;
  StaticJsonDocument<128> d;
  d["ver"] = PROTOCOL_VERSION;
  d["type"] = "SPOOL_LIST_REQ";
  d["msg_id"] = get_next_msg_id();
  d["ts"] = millis();
  String out; serializeJson(d, out);
  uart_send_json(out.c_str());
  g_spool_state = SPOOL_WAIT_LIST;
  g_spool_deadline_ms = millis() + SPOOL_DRAIN_REPLY_TIMEOUT_MS;
  Serial.printf("[DRAINTEST] forced drain (verify_only=%d)\n", verify_only ? 1 : 0);
}
#endif

// SPOOL_LIST from the LCD: what is waiting, plus the sidecar we need to rebuild
// the UploadJob. Everything except the pixels comes from here.
static void sense_spool_on_list(JsonDocument& doc) {
  if (g_spool_state != SPOOL_WAIT_LIST) return;
  const uint32_t slot = (uint32_t)(doc["slot"] | 0);
  const uint32_t len  = (uint32_t)(doc["len"] | 0);
  const uint32_t count = (uint32_t)(doc["count"] | 0);
  g_spool_last_known_depth = (uint16_t)count;   // for the wake log
  if (g_spool_probe_only) {
    // Depth is all we wanted. Stopping here is the whole point: fetching 170KB
    // on a capture wake is what used to stretch the wake past four minutes.
    g_spool_probe_done = true;   // answered -- stop retrying
    g_spool_probe_answered = true;
    Serial.printf("[SPOOL_DRAIN] probe: depth=%u\n", (unsigned)count);
    sense_spool_drain_reset("probe");
    return;
  }
  if (slot == 0 || len == 0) {
    sense_spool_drain_reset("empty");
    return;
  }
  JsonObject meta = doc["meta"];
  if (meta.isNull()) {
    // No sidecar means we cannot know the mode; uploading it would risk filing
    // a check-in as a dish. Leave it on the card and say so.
    Serial.printf("[SPOOL_DRAIN] slot=%lu has no metadata - skipping\n", (unsigned long)slot);
    sense_spool_drain_reset("no_meta");
    return;
  }

  g_spool_job = UploadJob{};
  g_spool_job.job_id   = (uint32_t)(meta["job_id"] | 0);
  g_spool_job.is_voice = ((int)(meta["is_voice"] | 0) != 0);
  snprintf(g_spool_job.mode, sizeof(g_spool_job.mode), "%s",
           (const char*)(meta["mode"] | ""));
  snprintf(g_spool_job.expiry_date, sizeof(g_spool_job.expiry_date), "%s",
           (const char*)(meta["expiry"] | ""));
  g_spool_job.quantity             = (uint16_t)(meta["qty"] | 1);
  g_spool_job.add_to_shopping_list = ((int)(meta["add_list"] | 0) != 0);
  g_spool_job.retries              = (uint8_t)(meta["retries"] | 0);
  g_spool_job.created_epoch        = (uint32_t)(meta["epoch"] | 0);
  JsonObject cm = meta["cam"];
  if (!cm.isNull()) {
    g_spool_job.camera_meta.profile             = (uint8_t)(cm["p"] | 0);
    g_spool_job.camera_meta.flash_enabled       = (uint8_t)(cm["f"] | 0);
    g_spool_job.camera_meta.jpeg_quality        = (uint8_t)(cm["q"] | 0);
    g_spool_job.camera_meta.actual_width        = (uint16_t)(cm["w"] | 0);
    g_spool_job.camera_meta.actual_height       = (uint16_t)(cm["h"] | 0);
    g_spool_job.camera_meta.configured_framesize= (uint16_t)(cm["fs"] | 0);
    g_spool_job.camera_meta.scene_luma          = (int16_t)(cm["l"] | 0);
    g_spool_job.camera_meta.scene_green_ratio   = (int16_t)(cm["g"] | 0);
    g_spool_job.camera_meta.xclk_hz             = (uint32_t)(cm["x"] | 0);
  }

  g_spool_buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!g_spool_buf) {
    Serial.printf("[SPOOL_DRAIN] alloc failed for %lu bytes\n", (unsigned long)len);
    sense_spool_drain_reset("alloc");
    g_spool_drain_fails++;
    return;
  }
  g_spool_slot = slot; g_spool_len = len; g_spool_got = 0; g_spool_next_seq = 0;

  Serial.printf("[SPOOL_DRAIN] fetching slot=%lu len=%lu mode=%s (queue depth %lu)\n",
                (unsigned long)slot, (unsigned long)len, g_spool_job.mode,
                (unsigned long)count);

  StaticJsonDocument<128> d;
  d["ver"] = PROTOCOL_VERSION;
  d["type"] = "SPOOL_FETCH";
  d["msg_id"] = get_next_msg_id();
  d["ts"] = millis();
  d["slot"] = slot;
  String out; serializeJson(d, out);
  uart_send_json(out.c_str());
  g_spool_state = SPOOL_WAIT_READY;
  g_spool_deadline_ms = millis() + SPOOL_DRAIN_REPLY_TIMEOUT_MS;
}

static void sense_spool_on_fetch_ready(JsonDocument& doc) {
  if (g_spool_state != SPOOL_WAIT_READY) return;
  if ((int)(doc["ok"] | 0) != 1) {
    sense_spool_drain_reset("lcd_not_ready");
    g_spool_drain_fails++;
    return;
  }
  // The LCD waits 120ms after this reply before its first frame. Drain whatever
  // the handshake left behind: a single leftover byte would be consumed as the
  // first COBS byte and shift the entire decode (that exact bug cost a full
  // debug cycle on the inbound direction).
  const uint32_t quiet_deadline = millis() + 60;
  uint32_t last_byte_ms = millis();
  while ((int32_t)(millis() - quiet_deadline) < 0) {
    if (lcdSerial.available()) { lcdSerial.read(); last_byte_ms = millis(); }
    else if ((millis() - last_byte_ms) >= 10) break;
    else delay(1);
  }
  g_spool_owns_uart = true;          // JSON line reader stands down
  g_spool_state = SPOOL_RECEIVING;
  g_spool_deadline_ms = millis() + 60000;   // whole-transfer bound
}

// Pump inbound frames while the drain owns the port. Called from the main loop.
static void sense_spool_receive_pump() {
  if (!g_spool_owns_uart) return;
  if (!g_spool_proto) {
    g_spool_proto = new UartOtaProtocol(&lcdSerial);
    if (!g_spool_proto) { sense_spool_drain_reset("no_proto"); return; }
    g_spool_proto->quiet = true;
  }
  uint8_t type = 0; uint16_t seq = 0;
  static uint8_t frame[MAX_FRAME_SIZE];
  size_t len = sizeof(frame);        // IN/OUT: must carry capacity IN
  if (!g_spool_proto->recv_frame(&type, &seq, frame, &len, SPOOL_DRAIN_FRAME_TIMEOUT_MS)) {
    sense_spool_drain_reset("frame_timeout");
    g_spool_drain_fails++;
    return;
  }
  switch (type) {
    case MSG_IMG_CHUNK:
      if (seq != g_spool_next_seq || (g_spool_got + len) > g_spool_len) {
        // A gap would silently corrupt the image; refuse rather than write past.
        Serial.printf("[SPOOL_DRAIN] seq/size fault seq=%u want=%u got=%lu+%u len=%lu\n",
                      (unsigned)seq, (unsigned)g_spool_next_seq,
                      (unsigned long)g_spool_got, (unsigned)len,
                      (unsigned long)g_spool_len);
        g_spool_proto->send_frame(MSG_IMG_NACK, seq, nullptr, 0);
        sense_spool_drain_reset("seq_gap");
        g_spool_drain_fails++;
        return;
      }
      memcpy(g_spool_buf + g_spool_got, frame, len);
      g_spool_got += len;
      g_spool_next_seq++;
      g_spool_proto->send_frame(MSG_IMG_ACK, seq, nullptr, 0);
      break;

    case MSG_IMG_END: {
      const bool complete = (g_spool_got == g_spool_len);
      g_spool_proto->send_frame(complete ? MSG_IMG_ACK : MSG_IMG_NACK, seq, nullptr, 0);
      g_spool_owns_uart = false;
      if (!complete) {
        Serial.printf("[SPOOL_DRAIN] incomplete got=%lu expect=%lu\n",
                      (unsigned long)g_spool_got, (unsigned long)g_spool_len);
        sense_spool_drain_reset("incomplete");
        g_spool_drain_fails++;
        return;
      }
#if HALO_SPOOL_TEST
      if (g_spool_verify_only) {
        // Byte-for-byte check against the pattern the Sense wrote in spooltest.
        // A matching LENGTH proves nothing: a framing bug that swaps, drops or
        // repeats bytes preserves length exactly.
        uint32_t bad = 0; long first_bad = -1;
        for (uint32_t i = 0; i < g_spool_len; i++) {
          if (g_spool_buf[i] != (uint8_t)((i * 31 + 7) & 0xFF)) {
            if (first_bad < 0) first_bad = (long)i;
            bad++;
          }
        }
        Serial.printf("[DRAINTEST] slot=%lu bytes=%lu mismatches=%lu first_bad=%ld -> %s\n",
                      (unsigned long)g_spool_slot, (unsigned long)g_spool_got,
                      (unsigned long)bad, first_bad,
                      (bad == 0 && g_spool_got == g_spool_len) ? "PASS" : "FAIL");
        g_spool_verify_only = false;
        sense_spool_drain_reset("verify_done");   // frees the buffer, keeps the slot
        return;
      }
#endif
      // Hand to the normal upload path. from_persisted=true so the existing
      // retry/telemetry treats it as a replay, which is what it is.
      const bool queued = queue_upload_job(g_spool_job.job_id,
                                           g_spool_job.mode,
                                           g_spool_job.expiry_date,
                                           g_spool_job.quantity,
                                           g_spool_job.add_to_shopping_list,
                                           &g_spool_job.camera_meta,
                                           g_spool_buf,
                                           g_spool_len,
                                           g_spool_job.retries,
                                           true,
                                           g_spool_job.created_epoch);
      if (queued) {
        // Ownership of the buffer passes to the queue.
        g_drain_slot_inflight = g_spool_slot;
        g_drain_job_id = g_spool_job.job_id;
        g_spool_buf = nullptr;
        Serial.printf("[SPOOL_DRAIN] slot=%lu queued for upload job_id=%lu mode=%s\n",
                      (unsigned long)g_spool_slot, (unsigned long)g_spool_job.job_id,
                      g_spool_job.mode);
      } else {
        Serial.println("[SPOOL_DRAIN] queue failed - leaving image on SD");
        g_spool_drain_fails++;
      }
      sense_spool_drain_reset(queued ? "queued" : "queue_failed");
      return;
    }

    default:
      Serial.printf("[SPOOL_DRAIN] unexpected frame type=0x%02X\n", (unsigned)type);
      sense_spool_drain_reset("wrong_frame_type");
      g_spool_drain_fails++;
      return;
  }
}

// Called from the upload success path. Only now is it safe to drop the slot:
// until the bytes are actually at the backend, the SD copy is the only copy.
static void sense_spool_on_upload_result(uint32_t job_id, bool ok) {
  if (g_drain_slot_inflight == 0 || job_id != g_drain_job_id) return;
  if (ok) {
    StaticJsonDocument<128> d;
    d["ver"] = PROTOCOL_VERSION;
    d["type"] = "SPOOL_DELETE";
    d["msg_id"] = get_next_msg_id();
    d["ts"] = millis();
    d["slot"] = g_drain_slot_inflight;
    String out; serializeJson(d, out);
    uart_send_json(out.c_str());
    g_spool_drained_ok++;
    Serial.printf("[SPOOL_DRAIN] slot=%lu uploaded - asked LCD to delete (total %lu)\n",
                  (unsigned long)g_drain_slot_inflight, (unsigned long)g_spool_drained_ok);
  } else {
    // Keep the slot. The photo is still only on the SD card.
    Serial.printf("[SPOOL_DRAIN] slot=%lu upload failed - keeping it on SD\n",
                  (unsigned long)g_drain_slot_inflight);
    g_spool_drain_fails++;
  }
  g_drain_slot_inflight = 0;
  g_drain_job_id = 0;
}

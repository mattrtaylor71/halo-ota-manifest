/*
 * Session_Board.ino  —  live Claude Code session board for the HALO LCD board
 *
 * HALO LCD board (ESP32-S3, 360x360 ROUND SH8601 LCD, CST816 touch, rotary knob,
 * DRV2605 haptic, OPI PSRAM, 8MB flash).
 *
 * A Mac-side daemon (session_board.py) streams newline-delimited JSON over USB
 * serial describing Matt's active Claude Code terminal sessions. This sketch
 * renders them: an overview list + one page per session (knob / swipe / tap to
 * navigate), with a scrollable full-message detail view.
 *
 * Flow:  WAIT --(first snapshot commit)--> BOARD(page 0 overview, 1..N sessions)
 *   WAIT  : dark screen, pulsing dot, "waiting for session_board.py".
 *   BOARD : rim progress arc + per-page content. Knob CW/CCW or swipe L/R changes
 *           page (clamped, no wrap). Tap a session page to open a scrollable detail
 *           view of its full message; in detail the knob scrolls instead of paging.
 *
 * No WiFi / SD / OTA / sleep. LVGL v8, LV_COLOR_DEPTH 16, LV_COLOR_16_SWAP 1.
 * All LVGL calls happen on the Arduino loop task only (knob callback posts a delta;
 * touch is delivered through LVGL's own pointer indev registered in lcd_bsp.c;
 * serial is drained in loop() before lv_timer_handler()).
 *
 * Rendering rules honored (learned the hard way on this panel, see Recipe_Demo):
 *   - Property anims only (position/size/opacity/arc/color). NO transform_zoom.
 *   - NO full-screen bg_opa fades. Screen-to-screen uses a single lv_scr_load repaint.
 *   - One-shot timers NULL their handle first; repeating anims die with their object
 *     (lv_obj_clean on the content container deletes children and their anims).
 */

#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "bidi_switch_knob.h"
#include "lvgl.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "audio_bsp.h"
#include <ArduinoJson.h>
#include <string.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <lwip/sockets.h>   // lwip_send + MSG_DONTWAIT for a truly non-blocking socket TX drain
#include <errno.h>          // errno on lwip_send<=0 (upload send-path telemetry: EAGAIN vs real error)
#include "sb_ring.h"   // sb_ring_t — in a header so auto-prototypes resolve it (see sb_ring.h)
#include "sb_session.h" // sb_session_t — same reason (see sb_session.h)
#include "nvs.h"

// ===================== Custom fonts =====================
LV_FONT_DECLARE(roboto_med_22);
LV_FONT_DECLARE(roboto_reg_20);
LV_FONT_DECLARE(roboto_bold_20);
LV_FONT_DECLARE(roboto_bold_22);
LV_FONT_DECLARE(roboto_bold_30);

// ===================== DESIGN PALETTE (dark theme) =====================
#define COL_SB_BG        0x101416   // near-black background (overview)
#define COL_SB_BG_DETAIL 0x16343B   // clearly-teal dark (detail view — unmistakable vs overview)
#define COL_SB_WHITE     0xFFFFFF
#define COL_SB_SOFT      0xC9D1D3   // soft grey body text
#define COL_SB_TEAL      0x296065   // brand teal
#define COL_SB_TEAL_BR   0x3E8E96   // brighter teal for headers
#define COL_SB_GOLD      0xF6BF41   // rim arc + working
#define COL_SB_GREEN     0x39B54A   // done
#define COL_SB_GREY      0x6B7276   // idle / stale

// Rotary encoder pins (HALO LCD board)
#define ENCODER_ECA_PIN   8
#define ENCODER_ECB_PIN   7

// Backlight duty (Matt's pick on this unit)
#define BL_DUTY_PCT       64

// Flip to 1 to skip WAIT and boot straight into the board with canned data.
#define SB_FAKE_DATA 0

// ===================== DATA MODEL =====================
#define SB_MAX_SESSIONS 12
// sb_session_t is defined in sb_session.h (included at the top) so arduino-cli's
// auto-generated prototypes resolve it — see that header for the why.

// live/shadow/s_pending_order are ~16KB each; they live in PSRAM (allocated in setup)
// so the WiFi driver has enough INTERNAL DRAM for its task/structures. Loop-task data
// only — never DMA'd, never ISR-touched, so PSRAM is safe. Indexing is unchanged.
#define SB_SESS_ARR_BYTES (sizeof(sb_session_t) * SB_MAX_SESSIONS)
static sb_session_t* live = NULL;
static int          live_n = 0;

// Incoming-frame staging (a frame = hdr, N rows, end).
static sb_session_t* shadow = NULL;
static int          shadow_n     = 0;   // expected N from hdr
static uint32_t     shadow_seq   = 0;
static int          shadow_count = 0;   // rows actually received
static bool         in_frame     = false;
static unsigned long last_commit_ms = 0;
static unsigned long last_serial_commit_ms = 0;  // last accepted frame that arrived over USB serial
static bool          s_rx_from_serial = false;    // source of the line currently in process_line

// ===================== APP STATE =====================
// Two levels: OVERVIEW = one focused agent card (carousel); DETAIL = its full,
// knob-scrollable message. The knob/swipes flip the focused agent in both.
typedef enum { ST_WAIT = 0, ST_BOARD } app_state_t;
static app_state_t s_state = ST_WAIT;

static int  s_sel         = 0;      // focused agent index, 0..live_n-1
static bool s_detail_open = false;  // false = focused card, true = detail
static bool s_stale       = false;  // link stale (no commit >10s)

// FLEET LIST: a scrollable "all agents" screen, one level before the first card.
#define FLEET_VIS 8                 // rows visible at once
static bool s_fleet_open = false;
static int  s_fleet_top  = 0;       // first visible registry index (windowed)
// First-seen registry: ids in arrival order; survivors NEVER reorder (stable list).
static char s_fleet_ids[SB_MAX_SESSIONS][12];
static int  s_fleet_n = 0;
static int  s_fleet_last_vis = -1;  // last registry index rendered (for the down-overflow marker)

// CCW-x5-on-first-agent = force refresh gesture.
#define CCW_REFRESH_TICKS  5
#define CCW_REFRESH_GAP_MS 1500
static int           s_ccw_count   = 0;
static unsigned long s_ccw_last_ms = 0;

// What's currently rendered — used to skip needless rebuilds.
static sb_session_t rendered_sess;
static int  rendered_sel     = -1;
static bool rendered_detail  = false;
static int  rendered_live_n  = -1;
static bool rendered_stale   = false;
static bool rendered_fleet   = false;

// Knob handle + thread-safe pending rotation delta (callback -> loop)
static knob_handle_t   s_knob = 0;
static portMUX_TYPE    s_knob_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int32_t s_knob_delta = 0;

// ===================== LVGL objects =====================
static lv_obj_t* wait_screen  = NULL;
static lv_obj_t* board_screen = NULL;
// On-device Wi-Fi setup (touch keyboard). Separate lv screens, built on demand.
static lv_obj_t* settings_screen = NULL;
static lv_obj_t* wifi_screen     = NULL;
static lv_obj_t* wifi_ssid_ta    = NULL;   // SSID text field
static lv_obj_t* wifi_pwd_ta     = NULL;   // password text field
static lv_obj_t* wifi_kb         = NULL;   // lv_keyboard (QWERTY)
static lv_obj_t* wifi_status_lbl = NULL;   // "connecting..." / result line
static bool      s_settings_open = false;  // a modal setup screen owns input (freeze board nav)
static bool      s_wifi_entry_open = false;
static bool      s_wifi_connecting = false;// submitted creds, waiting on the join result
static unsigned long s_wifi_connect_ms = 0;
static unsigned long s_wifi_close_ms = 0;  // non-zero: auto-close to board at this time (after "connected!")
static lv_obj_t* rim_arc      = NULL;   // full-perimeter progress arc (persistent)
static lv_obj_t* board_tap    = NULL;   // full-screen tap catcher (persistent, BELOW content)
static lv_obj_t* board_content= NULL;   // rebuilt per page (persistent container)
static lv_obj_t* stale_chip   = NULL;   // "reconnecting..." staleness label (persistent, hidden)
static lv_obj_t* s_detail_scroll = NULL;// scrollable msg container while in detail (child of content)
static lv_timer_t* s_stale_timer = NULL;// 1s repeating staleness check
static lv_obj_t* refresh_cue     = NULL;// "refreshing..." label (persistent on board_screen, hidden)
static lv_timer_t* s_rec_thump = NULL;   // 90ms one-shot: record-stop second haptic beat
static lv_timer_t* s_refresh_hide  = NULL;// 6s one-shot: hide cue if daemon never answers

// ---- voice recording (hold-to-talk) ----
static lv_obj_t* rec_overlay = NULL;    // opaque strip: red dot + RECORDING (persistent, hidden)
static lv_obj_t* rec_dot     = NULL;    // pulsing red dot inside rec_overlay
// Animated voice feedback (sending dots -> delivered/failed pop). All children of
// fb_root (a persistent child of board_screen), so view rebuilds never kill them.
static lv_obj_t* fb_root     = NULL;    // transparent full-screen overlay (non-clickable)
static lv_obj_t* fb_pop      = NULL;    // the result pop circle (size anim + fade)
static lv_timer_t* fb_timer  = NULL;    // one-shot: sending-fallback / result-hold / fade-delete
static lv_coord_t  fb_pop_cy = 270;     // pop centre y (kept fixed as the circle grows)
static bool     s_audio_ok   = false;   // mic came up in setup()
static volatile bool s_recording = false;// loop-task single-writer flag (mutes all other Serial)
static bool     s_rec_over_tcp = false; // transport LOCKED at rec_start: true=stream to socket, false=Serial
static volatile uint32_t s_rec_pushed  = 0; // bytes voice_out queued this recording (loop)
static volatile uint32_t s_rec_drained = 0; // bytes net_task wrote to the socket in the rec window (net)
static volatile uint32_t s_rec_txdrop  = 0; // bytes dropped because the tx ring was full (loop)
static bool      s_last_rec_tcp = false;    // transport of the last recording (for the failure diagnostic)
static uint32_t  s_last_rec_dur = 0;        // duration (ms) of the last recording
// Full-screen, photographable voice-failure diagnostic (Wi-Fi-only debugging aid).
static lv_obj_t* s_diag      = NULL;        // overlay on board_screen; persists until tapped
static bool      s_diag_open = false;
static bool     s_rec_ui     = false;   // while true, the card/detail omits its eyebrow/title
                                        // so the RECORDING overlay owns the cleared top area
static char     s_rec_id[12]  = {0};    // focused agent id captured at record start
static uint32_t s_rec_start_ms = 0;

// PSRAM voice buffer: mic (producer, AUDIO task) -> loop task. Two modes share it:
//  - LIVE (serial / LAN TCP): circular drop-oldest ring, drained to the transport DURING
//    capture (low RTT keeps up). Unchanged behaviour.
//  - SPOOL (remote/ngrok TCP): linear fill during capture, uploaded WHOLE after capture.
//    A high-RTT tunnel can't sustain the mic's 88KB/s live, so we remove the real-time
//    deadline: record to PSRAM, then upload at whatever rate the tunnel allows (no drops,
//    no false "link dead"). Spooling caps the clip at what the buffer holds.
#define SB_SPOOL_TARGET   (4 * 1024 * 1024) // desired spool bytes (~131s @ 16kHz/32KB/s); allocated adaptively
#define SB_REC_DRAIN_CAP  4096          // bytes moved per loop iteration (live drain + spool upload chunk)
#define SB_REC_MAX_MS     180000        // failsafe auto-stop (live mode; spool mode also auto-stops at buffer-full)
#define SB_UPLOAD_STALL_MS 20000        // spool upload: no socket-drain progress this long -> abort (> daemon's 8s)
#define SB_UPLOAD_CAP_MS   180000       // spool upload: absolute ceiling (< daemon's 200s hard cap)
static uint8_t*  s_ring = NULL;
static size_t    s_spool_cap = 0;       // ACTUAL allocated buffer size (adaptive; wrap math uses this)
static volatile size_t   s_ring_head = 0, s_ring_tail = 0;
static volatile uint32_t s_ring_dropped = 0;
static portMUX_TYPE      s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static bool          s_spool_mode   = false;   // this recording spools-then-uploads (remote TCP)
static volatile bool s_spool_full   = false;   // spool buffer filled -> loop() auto-stops capture
static volatile bool s_upload_active = false;  // post-capture upload in progress (net_task reads it for the watchdog)
static size_t    s_upload_off = 0, s_upload_total = 0;         // spool bytes queued / total (loop task)
static uint32_t  s_upload_start_ms = 0, s_upload_progress_ms = 0;
static uint32_t  s_upload_last_drained = 0;    // last s_rec_drained seen (upload stall detection)
// Upload send-path telemetry (per-second log during a spool upload; diagnosing tunnel throughput).
static volatile uint32_t s_up_calls = 0, s_up_eagain = 0, s_up_err = 0, s_up_offered = 0;
static uint32_t  s_up_log_ms = 0, s_up_log_drained = 0;

// sb_ring_t (Wi-Fi net-task tx/rx ring) is defined in sb_ring.h, included at the top.

// ---- calm ordering (don't reorder the list under the user's fingers) ----
#define SB_CALM_MS 4000                       // quiet window before a deferred reorder applies
static unsigned long s_last_input_ms = 0;     // last knob/tap/swipe/record activity
static sb_session_t* s_pending_order = NULL;   // full snapshot awaiting a calm moment (PSRAM)
static int           s_pending_n = 0;
static bool          s_pending   = false;

// ---- Wi-Fi / TCP transport (standalone-over-Wi-Fi; USB Serial always works too) ----
#define SB_UDP_BEACON_PORT 8384               // daemon UDP beacon: "SBHALO1 <tcp_port>"
#define SB_NET_TX_SZ (256 * 1024)             // device->daemon ring (PSRAM); holds voice PCM bursts (88KB/s)
#define SB_NET_RX_SZ (32 * 1024)              // daemon->device byte ring (holds a frame burst)
#define SB_BEACON_WAIT_MS  8000               // no LAN beacon within this -> try the remote fallback
static volatile bool s_wifi_up       = false; // STA associated + has IP
static volatile bool s_tcp_up        = false; // daemon socket connected
static volatile bool s_conn_remote   = false; // current socket is the remote (ngrok) fallback, not LAN
static volatile bool s_net_reconnect = false; // loop asks net task to reload creds + reconnect
static volatile bool s_wifi_scan_req = false; // loop asks net task to run a diagnostic WiFi.scanNetworks()
static volatile bool s_have_creds    = false; // creds present (for the "no creds" state + glyph)

// ===================== HAPTICS (DRV2605, shares touch I2C bus 0x5A) =====================
static const uint8_t    HAPTIC_ADDR          = 0x5A;
static const i2c_port_t HAPTIC_I2C_PORT      = I2C_NUM_0;
static const uint8_t    DRV2605_REG_STATUS   = 0x00;
static const uint8_t    DRV2605_REG_MODE     = 0x01;
static const uint8_t    DRV2605_REG_RTPIN    = 0x02;
static const uint8_t    DRV2605_REG_LIBRARY  = 0x03;
static const uint8_t    DRV2605_REG_WAVESEQ1 = 0x04;
static const uint8_t    DRV2605_REG_WAVESEQ2 = 0x05;
static const uint8_t    DRV2605_REG_GO       = 0x0C;
static const uint8_t    DRV2605_REG_OVERDRIVE= 0x0D;
static const uint8_t    DRV2605_REG_SUSTAINPOS=0x0E;
static const uint8_t    DRV2605_REG_SUSTAINNEG=0x0F;
static const uint8_t    DRV2605_REG_BREAK    = 0x10;
static const uint8_t    DRV2605_REG_AUDIOMAX = 0x13;
static const uint8_t    DRV2605_REG_RATEDV   = 0x16;
static const uint8_t    DRV2605_REG_CLAMPV   = 0x17;
static const uint8_t    DRV2605_REG_FEEDBACK = 0x1A;
static const uint8_t    DRV2605_REG_CONTROL3 = 0x1D;
static const uint8_t    DRV2605_MODE_INTTRIG = 0x00;
static const uint8_t    DRV2605_EFFECT_CLICK = 4;   // Sharp Click - 100%
static bool             s_haptic_ready       = false;
static unsigned long    s_last_haptic_ms     = 0;
static const unsigned long HAPTIC_MIN_INTERVAL_MS = 40;

static bool haptic_write(uint8_t reg, uint8_t value) {
  uint8_t data[2] = { reg, value };
  return i2c_master_write_to_device(HAPTIC_I2C_PORT, HAPTIC_ADDR, data, sizeof(data),
                                    pdMS_TO_TICKS(20)) == ESP_OK;
}
static bool haptic_read(uint8_t reg, uint8_t* value) {
  if (!value) return false;
  return i2c_master_write_read_device(HAPTIC_I2C_PORT, HAPTIC_ADDR, &reg, 1, value, 1,
                                      pdMS_TO_TICKS(20)) == ESP_OK;
}
static bool haptic_write_mask(uint8_t reg, uint8_t clear_mask, uint8_t set_mask) {
  uint8_t v = 0;
  if (!haptic_read(reg, &v)) return false;
  v &= clear_mask; v |= set_mask;
  return haptic_write(reg, v);
}
static void haptic_init() {
  if (s_haptic_ready) return;
  uint8_t status = 0;
  if (!haptic_read(DRV2605_REG_STATUS, &status)) {
    Serial.println("[HAPTIC] not found (no ACK) — haptics disabled");
    return;
  }
  uint8_t chip_id = (status >> 5) & 0x07;
  if (chip_id != 0x03 && chip_id != 0x04 && chip_id != 0x06 && chip_id != 0x07) {
    Serial.printf("[HAPTIC] unexpected chip_id=%u — haptics disabled\n", chip_id);
    return;
  }
  if (!haptic_write(DRV2605_REG_MODE, DRV2605_MODE_INTTRIG)) return;
  haptic_write(DRV2605_REG_RTPIN, 0);
  haptic_write(DRV2605_REG_LIBRARY, 6);          // LRA library
  haptic_write(DRV2605_REG_WAVESEQ1, DRV2605_EFFECT_CLICK);
  haptic_write(DRV2605_REG_WAVESEQ2, 0);
  haptic_write(DRV2605_REG_OVERDRIVE, 0);
  haptic_write(DRV2605_REG_SUSTAINPOS, 0);
  haptic_write(DRV2605_REG_SUSTAINNEG, 0);
  haptic_write(DRV2605_REG_BREAK, 0);
  haptic_write(DRV2605_REG_AUDIOMAX, 0x64);
  haptic_write(DRV2605_REG_RATEDV, 0xFF);
  haptic_write(DRV2605_REG_CLAMPV, 0xFF);
  haptic_write_mask(DRV2605_REG_FEEDBACK, 0xFF, 0x80);  // LRA mode
  haptic_write_mask(DRV2605_REG_CONTROL3, 0xDF, 0x00);
  s_haptic_ready = true;
  Serial.println("[HAPTIC] DRV2605 ready");
}
static void haptic_click() {
  unsigned long now = millis();
  if (now - s_last_haptic_ms < HAPTIC_MIN_INTERVAL_MS) return;
  if (!s_haptic_ready) return;
  haptic_write(DRV2605_REG_WAVESEQ1, DRV2605_EFFECT_CLICK);
  haptic_write(DRV2605_REG_WAVESEQ2, 0);
  haptic_write(DRV2605_REG_GO, 1);
  s_last_haptic_ms = now;
}

// ===================== small helpers =====================
// Make a touch target inert visually: no focus flash, no theme outline.
static void quiet_clickable(lv_obj_t* obj) {
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_set_style_outline_width(obj, 0, 0);
  lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUSED);
  lv_obj_set_style_outline_width(obj, 0, LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_width(obj, 0, LV_STATE_PRESSED);
}

// A full-screen invisible clickable tap catcher.
static lv_obj_t* make_tap_zone(lv_obj_t* parent, lv_event_cb_t cb) {
  lv_obj_t* z = lv_obj_create(parent);
  lv_obj_set_size(z, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_set_pos(z, 0, 0);
  lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(z, 0, 0);
  lv_obj_set_style_radius(z, 0, 0);
  lv_obj_set_style_pad_all(z, 0, 0);
  lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);   // keep GESTURE_BUBBLE (default) so swipes reach the screen
  quiet_clickable(z);
  // Tap (short-click) / long-press-record / release-stop.
  lv_obj_add_event_cb(z, cb, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(z, cb, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(z, cb, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(z, cb, LV_EVENT_PRESS_LOST, NULL);
  return z;
}

static lv_obj_t* make_label(lv_obj_t* parent, const char* txt,
                            const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, txt);
  return l;
}

// Status -> colour (greyed while the link is stale).
static uint32_t status_color(char st) {
  if (s_stale) return COL_SB_GREY;
  switch (st) {
    case 'w': return COL_SB_GOLD;
    case 'd': return COL_SB_GREEN;
    default:  return COL_SB_GREY;
  }
}
// Dot colour for a session: dormant (terminal unreachable) forces grey, overriding w/d/i.
static uint32_t dot_color(const sb_session_t* s) {
  if (s->drm) return COL_SB_GREY;
  return status_color(s->status);
}
// Should this session's dot pulse? Only a live "working" agent (not dormant/stale).
static bool dot_pulses(const sb_session_t* s) {
  return s->status == 'w' && !s->drm && !s_stale;
}
static const char* status_word(char st) {
  switch (st) {
    case 'w': return "WORKING";
    case 'd': return "DONE";
    default:  return "IDLE";
  }
}
// "12s" / "3m" / "2h" into caller buffer.
static void fmt_age(uint32_t s, char* out, size_t n) {
  if (s < 60)        snprintf(out, n, "%us", (unsigned)s);
  else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
  else               snprintf(out, n, "%uh", (unsigned)(s / 3600));
}

// Compare two sessions by their meaningful fields (strlcpy leaves garbage tails,
// so a raw memcmp is unsafe). When ignore_age, the age field is not compared —
// used in detail mode so ticking age doesn't reset the reader's scroll position.
static bool sess_changed(const sb_session_t* a, const sb_session_t* b, bool ignore_age) {
  if (strcmp(a->id, b->id)) return true;
  if (strcmp(a->name, b->name)) return true;
  if (strcmp(a->proj, b->proj)) return true;
  if (a->status != b->status) return true;
  if (strcmp(a->msg, b->msg)) return true;
  if (strcmp(a->dtl, b->dtl)) return true;
  if (a->agn != b->agn || a->aga != b->aga) return true;   // sub-agent dots
  if (a->drm != b->drm) return true;                       // dormant flag
  if (!ignore_age && a->age_s != b->age_s) return true;
  return false;
}

// Infinite gentle pulse on a status dot's bg opacity (dies with the object).
static void dot_pulse_cb(void* var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}
static void add_pulse(lv_obj_t* dot) {
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, dot);
  lv_anim_set_exec_cb(&a, dot_pulse_cb);
  lv_anim_set_values(&a, 110, 255);
  lv_anim_set_time(&a, 700);
  lv_anim_set_playback_time(&a, 700);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

// ===================== knob (callback posts delta; loop consumes) =====================
static void knob_right_cb(void* arg, void* data) { (void)arg; (void)data;
  portENTER_CRITICAL_ISR(&s_knob_mux); s_knob_delta += 1; portEXIT_CRITICAL_ISR(&s_knob_mux); }
static void knob_left_cb(void* arg, void* data) { (void)arg; (void)data;
  portENTER_CRITICAL_ISR(&s_knob_mux); s_knob_delta -= 1; portEXIT_CRITICAL_ISR(&s_knob_mux); }

static void init_knob() {
  Serial.println("[KNOB] init");
  knob_config_t cfg = { .gpio_encoder_a = ENCODER_ECA_PIN, .gpio_encoder_b = ENCODER_ECB_PIN };
  s_knob = iot_knob_create(&cfg);
  if (!s_knob) { Serial.println("[KNOB] ERROR: create failed"); return; }
  iot_knob_register_cb(s_knob, KNOB_RIGHT, knob_right_cb, NULL);
  iot_knob_register_cb(s_knob, KNOB_LEFT,  knob_left_cb,  NULL);
  Serial.println("[KNOB] ready (A=8 CW=next, B=7 CCW=prev)");
}

// ===================== forward decls =====================
static void build_view();
static void flip_to(int sel);
static void apply_snapshot();
static void commit_order(const sb_session_t* src, int n);   // used by stale_timer_cb (deferred reorder)
static void build_fleet();
static void open_fleet();
static void fleet_registry_update();
static void fleet_close_to_carousel();
// Factored UI actions — shared by LVGL events and the serial test-injection hooks.
static void do_tap();
static void do_swipe(lv_dir_t d);
static void rec_start(bool new_session);
static void rec_stop();
static void fb_clear();
static void fb_sending();
static void fb_result(bool ok, bool unread, const char* err);
static void show_voice_fail(const char* reason, uint32_t header_col);
// On-device Wi-Fi setup navigation (circular refs between the settings/keyboard handlers).
static void settings_open();
static void settings_close();
static void wifi_entry_open();
static void wifi_entry_close();
// Wi-Fi / TCP transport (defined in the NET section, used earlier by trigger_refresh + process_line).
static void proto_send(const char* line);
static void save_sboard_creds(const char* ssid, const char* pwd);
static void save_remotecfg(const char* tok, const char* host, int port);   // partial-safe (see def)
static void net_request_reconnect();
static void net_request_wifiscan();
// Voice transport: routes the WAV/PCM stream to the socket (rec over TCP) or Serial.
static void voice_out(const uint8_t* p, size_t n);
static void voice_out_str(const char* s);

// ===================== rim progress arc =====================
static void rim_arc_cb(void* var, int32_t v) { (void)var; lv_arc_set_value(rim_arc, v); }
static void animate_rim(int page) {
  int denom = live_n > 0 ? live_n : 1;
  int target = page * 1000 / denom;
  lv_anim_del(rim_arc, rim_arc_cb);
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, rim_arc);
  lv_anim_set_exec_cb(&a, rim_arc_cb);
  lv_anim_set_values(&a, lv_arc_get_value(rim_arc), target);
  lv_anim_set_time(&a, 200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// ===================== page builders =====================
static void board_press_cb(lv_event_t* e);  // fwd (tap/long-press/release dispatcher)

// Uppercase an ASCII string into a caller buffer (for the all-caps meta rows).
static void upper_copy(char* out, size_t n, const char* in) {
  size_t i = 0;
  for (; in[i] && i + 1 < n; i++) {
    char c = in[i];
    out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
  out[i] = 0;
}

// The status eyebrow: dot + WORD, composed so the pair centers as one unit at y.
// Measured manually (no flex dependency) via lv_txt_get_size.
static void build_eyebrow(const char* word, uint32_t color, bool pulse, int dot_sz, int y) {
  lv_point_t sz;
  lv_txt_get_size(&sz, word, &lv_font_montserrat_14, 2, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  int total = dot_sz + 8 + sz.x;
  int left  = -total / 2;                 // relative to the horizontal centre

  lv_obj_t* dot = lv_obj_create(board_content);
  lv_obj_set_size(dot, dot_sz, dot_sz);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  int dy = (lv_font_get_line_height(&lv_font_montserrat_14) - dot_sz) / 2;   // vertical-center to text
  lv_obj_align(dot, LV_ALIGN_TOP_MID, left + dot_sz / 2, y + dy);
  if (pulse) add_pulse(dot);

  lv_obj_t* lbl = make_label(board_content, word, &lv_font_montserrat_14, color);
  lv_obj_set_style_text_letter_space(lbl, 2, 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_MID, left + dot_sz + 8 + sz.x / 2, y);
}

// OVERVIEW: one focused agent card (a carousel entry). Centered column, roomy.
static void build_focus_card() {
  if (live_n == 0) {                    // empty: eyebrow header + centered message
    lv_obj_t* eb = make_label(board_content, "CLAUDE SESSIONS", &lv_font_montserrat_14, COL_SB_TEAL_BR);
    lv_obj_set_style_text_letter_space(eb, 2, 0);
    lv_obj_align(eb, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_t* none = make_label(board_content, "no active sessions", &roboto_reg_20, COL_SB_SOFT);
    lv_obj_align(none, LV_ALIGN_CENTER, 0, 0);
    return;
  }

  sb_session_t* s = &live[s_sel];

  if (!s_rec_ui)   // hidden while recording; the RECORDING overlay takes this spot
    build_eyebrow(status_word(s->status), dot_color(s), dot_pulses(s), 10, 46);   // dormant -> grey, no pulse

  // Title clips to 2 lines (fixed height + LONG_DOT); the elements below are
  // placed against its ACTUAL used height (1 vs 2 lines) so nothing overlaps.
  int lh = lv_font_get_line_height(&roboto_bold_22);
  lv_obj_t* title = make_label(board_content, s->name, &roboto_bold_22, COL_SB_WHITE);
  lv_obj_set_width(title, 232);
  lv_obj_set_height(title, lh * 2);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 76);

  lv_point_t nsz;                               // real wrapped height at width 232
  lv_txt_get_size(&nsz, s->name, &roboto_bold_22, 0, 0, 232, LV_TEXT_FLAG_NONE);
  int used_h = nsz.y > lh * 2 ? lh * 2 : nsz.y;

  int sub_h = 0;                                // dormant subtitle under the title
  if (s->drm) {
    lv_obj_t* sub = make_label(board_content, "may not respond", &lv_font_montserrat_12, COL_SB_GREY);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 76 + used_h + 2);
    sub_h = 18;
  }

  int bar_y  = 76 + used_h + sub_h + 12;
  int body_y = bar_y + 4 + 14;

  lv_obj_t* bar = lv_obj_create(board_content);
  lv_obj_set_size(bar, 36, 4);
  lv_obj_set_style_radius(bar, 2, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COL_SB_GOLD), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, bar_y);

  lv_obj_t* body = make_label(board_content, s->msg, &roboto_reg_20, COL_SB_SOFT);
  lv_obj_set_width(body, 252);
  lv_obj_set_height(body, 110 - sub_h);         // shrink when a dormant subtitle pushed us down
  lv_obj_set_style_text_line_space(body, 6, 0);
  lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, body_y);

  if (s->proj[0]) {
    lv_obj_t* pj = make_label(board_content, s->proj, &lv_font_montserrat_14, COL_SB_TEAL_BR);
    lv_obj_set_style_text_letter_space(pj, 1, 0);
    lv_obj_set_width(pj, 200);
    lv_label_set_long_mode(pj, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(pj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(pj, LV_ALIGN_TOP_MID, 0, 292);
  }

  char age[12], ageU[12], foot[40];
  fmt_age(s->age_s, age, sizeof(age));
  upper_copy(ageU, sizeof(ageU), age);
  snprintf(foot, sizeof(foot), "%d OF %d \xE2\x80\xA2 %s AGO", s_sel + 1, live_n, ageU);
  lv_point_t fsz;
  lv_txt_get_size(&fsz, foot, &lv_font_montserrat_12, 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (fsz.x > 190)   // "OF" spelling too wide low in the circle — use the tight form
    snprintf(foot, sizeof(foot), "%d/%d \xE2\x80\xA2 %s AGO", s_sel + 1, live_n, ageU);
  lv_obj_t* ft = make_label(board_content, foot, &lv_font_montserrat_12, COL_SB_GREY);
  lv_obj_set_style_text_letter_space(ft, 1, 0);
  lv_obj_align(ft, LV_ALIGN_TOP_MID, 0, 312);   // nudged up to clear the expand hint

  // Expand hint (tap opens detail).
  lv_obj_t* plus = make_label(board_content, LV_SYMBOL_PLUS, &lv_font_montserrat_14, COL_SB_GREY);
  lv_obj_set_style_text_opa(plus, LV_OPA_40, 0);
  lv_obj_align(plus, LV_ALIGN_TOP_MID, 0, 334);
}

// ---- HYBRID detail text: static circle-flow OR a rectangle scroll container ----
// If the whole text (+ meta) fits one screen of chord-width rows -> render it STATIC
// (circle-flow; nothing ever moves). If it overflows -> a plain LVGL scroll rectangle
// (native touch drag = free smooth pixel scroll; knob = animated scroll_by). No word
// reflow while scrolling, so reading stays stable. Static circle beauty, rect readability.
#define DET_TOP_Y     72
#define DET_BOT_Y     308
#define DET_MAX_LPP   14
#define DET_MAX_WORDS 240
// Parsed text (dtl with ** stripped) + per-char bold, then words + measured widths.
static char sb_clean[768];
static bool sb_cbold[768];
static int  sb_wstart[DET_MAX_WORDS], sb_wlen[DET_MAX_WORDS], sb_wwidth[DET_MAX_WORDS];
static bool sb_wbold[DET_MAX_WORDS];
static int  sb_nwords = 0;
static int  sb_pitch = 26, sb_lpp = 9, sb_band_top = DET_TOP_Y, sb_space_w = 4;
static int  sb_budget[DET_MAX_LPP];
static lv_obj_t* sb_more_obj = NULL;        // ▼ affordance (rectangle mode only)

// Non-clickable + bubble so a press on the text reaches whatever owns the press
// (the tap zone in static mode, the scroll container in rectangle mode).
static void detail_passthrough(lv_obj_t* o) {
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}
static inline bool sb_is_space(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Parse text -> words, each tagged bold/normal with its measured width. Caches space width.
static void detail_tokenize(const char* text) {
  int marks = 0;
  for (const char* q = text; q[0]; ) { if (q[0]=='*'&&q[1]=='*') { marks++; q+=2; } else q++; }
  bool use_bold = (marks > 0) && (marks % 2 == 0);
  int ci = 0; bool bold = false;
  for (const char* p = text; *p && ci < (int)sizeof(sb_clean) - 1; ) {
    if (p[0]=='*' && p[1]=='*') { if (use_bold) bold = !bold; p += 2; continue; }
    sb_clean[ci] = *p; sb_cbold[ci] = use_bold ? bold : false; ci++; p++;
  }
  sb_clean[ci] = 0;
  int clean_n = ci;

  lv_point_t spsz;
  lv_txt_get_size(&spsz, " ", &roboto_reg_20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  sb_space_w = spsz.x;

  sb_nwords = 0;
  for (int i = 0; i < clean_n && sb_nwords < DET_MAX_WORDS; ) {
    while (i < clean_n && sb_is_space(sb_clean[i])) i++;
    if (i >= clean_n) break;
    int st = i; bool b = sb_cbold[i];
    while (i < clean_n && !sb_is_space(sb_clean[i])) i++;
    int wl = i - st;
    char wb[80]; int cl = wl > (int)sizeof(wb) - 1 ? (int)sizeof(wb) - 1 : wl;
    memcpy(wb, sb_clean + st, cl); wb[cl] = 0;
    lv_point_t wsz;
    lv_txt_get_size(&wsz, wb, b ? &roboto_bold_20 : &roboto_reg_20,
                    0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    sb_wstart[sb_nwords] = st; sb_wlen[sb_nwords] = wl;
    sb_wbold[sb_nwords] = b;   sb_wwidth[sb_nwords] = wsz.x;
    sb_nwords++;
  }
}

// How many words fit on one line starting at `start` within `budget` (greedy, >=1).
static int flow_line_len(int start, int budget) {
  if (start >= sb_nwords) return 0;
  int used = sb_wwidth[start], wi = start + 1;               // first word always placed
  while (wi < sb_nwords) {
    if (used + sb_space_w + sb_wwidth[wi] <= budget) { used += sb_space_w + sb_wwidth[wi]; wi++; }
    else break;
  }
  return wi - start;
}

// Does the ENTIRE text (all words + the meta line) fit within the lpp chord rows?
static bool detail_all_fits() {
  int wi = 0;
  for (int i = 0; i < sb_lpp; i++) {
    if (wi < sb_nwords)       wi += flow_line_len(wi, sb_budget[i]);
    else if (wi == sb_nwords) { wi = sb_nwords + 1; break; }
    else break;
  }
  return wi >= sb_nwords + 1;
}

static void detail_meta_str(char* out, int n) {
  sb_session_t* s = &live[s_sel];
  char age[12], ageU[12], pjU[24];
  fmt_age(s->age_s, age, sizeof(age));
  upper_copy(ageU, sizeof(ageU), age);
  if (s->proj[0]) { upper_copy(pjU, sizeof(pjU), s->proj);
    snprintf(out, n, "%s \xE2\x80\xA2 %s AGO", pjU, ageU); }
  else            snprintf(out, n, "%s AGO", ageU);
}

// STATIC circle-flow: render words start..end-1 as an EXPAND spangroup child of
// board_content, centred at screen y. (One row of the fixed layout.)
static void render_static_line(int start, int end, int y) {
  lv_obj_t* sg = lv_spangroup_create(board_content);
  lv_spangroup_set_mode(sg, LV_SPAN_MODE_EXPAND);
  for (int w = start; w < end; w++) {
    char wb[82]; int wl = sb_wlen[w]; if (wl > (int)sizeof(wb) - 2) wl = sizeof(wb) - 2;
    memcpy(wb, sb_clean + sb_wstart[w], wl);
    int bl = wl; if (w < end - 1) wb[bl++] = ' ';
    wb[bl] = 0;
    lv_span_t* sp = lv_spangroup_new_span(sg);
    lv_span_set_text(sp, wb);
    bool b = sb_wbold[w];
    lv_style_set_text_font(&sp->style, b ? &roboto_bold_20 : &roboto_reg_20);
    lv_style_set_text_color(&sp->style, lv_color_hex(b ? COL_SB_WHITE : COL_SB_SOFT));
  }
  lv_spangroup_refr_mode(sg);
  lv_obj_align(sg, LV_ALIGN_TOP_MID, 0, y);
  detail_passthrough(sg);
}

// RECTANGLE mode: one wrapping (BREAK) spangroup of the whole text with **bold**
// runs merged into spans. Child of the scroll container.
static lv_obj_t* render_rect_spangroup(lv_obj_t* parent) {
  lv_obj_t* sg = lv_spangroup_create(parent);
  lv_obj_set_width(sg, 248);            // wide box: fat-middle band
  lv_obj_set_height(sg, LV_SIZE_CONTENT);
  lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
  lv_obj_set_style_text_line_space(sg, 6, 0);
  int w = 0;
  while (w < sb_nwords) {
    bool b = sb_wbold[w];
    char buf[768]; int bl = 0;
    while (w < sb_nwords && sb_wbold[w] == b && bl < (int)sizeof(buf) - 40) {
      int wl = sb_wlen[w]; if (wl > (int)sizeof(buf) - 2 - bl) wl = sizeof(buf) - 2 - bl;
      memcpy(buf + bl, sb_clean + sb_wstart[w], wl); bl += wl;
      buf[bl++] = ' ';                                        // trailing space (harmless in BREAK)
      w++;
    }
    buf[bl] = 0;
    lv_span_t* sp = lv_spangroup_new_span(sg);
    lv_span_set_text(sp, buf);
    lv_style_set_text_font(&sp->style, b ? &roboto_bold_20 : &roboto_reg_20);
    lv_style_set_text_color(&sp->style, lv_color_hex(b ? COL_SB_WHITE : COL_SB_SOFT));
  }
  lv_spangroup_refr_mode(sg);
  detail_passthrough(sg);
  return sg;
}

// Toggle the ▼ affordance as the rectangle scrolls (hidden once at the bottom).
static void rect_scroll_cb(lv_event_t* e) {
  lv_obj_t* c = lv_event_get_target(e);
  if (!sb_more_obj) return;
  if (lv_obj_get_scroll_bottom(c) > 4) lv_obj_clear_flag(sb_more_obj, LV_OBJ_FLAG_HIDDEN);
  else                                 lv_obj_add_flag(sb_more_obj, LV_OBJ_FLAG_HIDDEN);
}

// Sub-agent activity dots: a centred row at `y` — `aga` gold (pulsing) + the rest grey.
static void render_subagent_dots(int y) {
  sb_session_t* s = &live[s_sel];
  int n = s->agn; if (n <= 0) return; if (n > 8) n = 8;
  int act = s->aga; if (act > n) act = n;
  for (int i = 0; i < n; i++) {
    lv_obj_t* dot = lv_obj_create(board_content);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    bool on = (i < act);
    lv_obj_set_style_bg_color(dot, lv_color_hex(on ? COL_SB_GOLD : COL_SB_GREY), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, i * 12 - (n - 1) * 6, y);
    if (on) add_pulse(dot);
  }
}

// DETAIL: back arrow + up-to-2-line title + hybrid body (static circle OR rectangle
// scroll) + optional sub-agent dots. The final flowed/scrolled line is the meta.
static void build_detail() {
  sb_session_t* s = &live[s_sel];
  s_detail_scroll = NULL;
  sb_more_obj = NULL;

  sb_band_top = DET_TOP_Y;
  if (!s_rec_ui) {   // hidden while recording (RECORDING overlay owns the top)
    lv_obj_t* back = make_label(board_content, LV_SYMBOL_LEFT, &lv_font_montserrat_14, COL_SB_GREY);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 20);
    int lh = lv_font_get_line_height(&roboto_bold_20);
    lv_obj_t* title = make_label(board_content, s->name, &roboto_bold_20, COL_SB_WHITE);
    lv_obj_set_width(title, 160);
    lv_obj_set_height(title, lh * 2);                         // up to 2 lines
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 44);
    lv_point_t nsz;
    lv_txt_get_size(&nsz, s->name, &roboto_bold_20, 0, 0, 160, LV_TEXT_FLAG_NONE);
    int used_h = nsz.y > lh * 2 ? lh * 2 : nsz.y;
    sb_band_top = 44 + used_h + 10;                           // text below the title's real bottom
    if (s->drm) {                                             // dormant subtitle under the title
      lv_obj_t* sub = make_label(board_content, "may not respond", &lv_font_montserrat_12, COL_SB_GREY);
      lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 44 + used_h + 4);
      sb_band_top = 44 + used_h + 4 + 20;                     // push the body band below the subtitle
    }
    if (sb_band_top < DET_TOP_Y) sb_band_top = DET_TOP_Y;
  }

  // Row pitch + count + per-row chord budgets (for the static-fits check + static render).
  sb_pitch = lv_font_get_line_height(&roboto_reg_20) + 6;
  if (sb_pitch < 12) sb_pitch = 12;
  sb_lpp = (DET_BOT_Y - sb_band_top) / sb_pitch;
  if (sb_lpp < 1) sb_lpp = 1;
  if (sb_lpp > DET_MAX_LPP) sb_lpp = DET_MAX_LPP;
  for (int i = 0; i < sb_lpp; i++) {
    int y0 = sb_band_top + sb_pitch * i, y1 = y0 + sb_pitch;
    int d0 = y0 - 180; if (d0 < 0) d0 = -d0;
    int d1 = y1 - 180; if (d1 < 0) d1 = -d1;
    int dy = d0 > d1 ? d0 : d1;
    int inside = 160 * 160 - dy * dy;
    int half = inside > 0 ? (int)sqrt((double)inside) : 0;
    int b = 2 * half - 16;
    sb_budget[i] = b < 40 ? 40 : b;
  }

  detail_tokenize(s->dtl[0] ? s->dtl : s->msg);
  bool has_dots = s->agn > 0;
  char meta[64]; detail_meta_str(meta, sizeof(meta));

  if (detail_all_fits()) {
    // ---- STATIC circle-flow: everything fits; nothing moves. No scroll, no ▼. ----
    int wi = 0;
    for (int i = 0; i < sb_lpp; i++) {
      int y = sb_band_top + sb_pitch * i;
      if (wi < sb_nwords) {
        int len = flow_line_len(wi, sb_budget[i]); if (len < 1) len = 1;
        render_static_line(wi, wi + len, y);
        wi += len;
      } else if (wi == sb_nwords) {
        lv_obj_t* ml = make_label(board_content, meta, &lv_font_montserrat_12, COL_SB_GREY);
        lv_obj_set_style_text_letter_space(ml, 1, 0);
        lv_obj_set_width(ml, sb_budget[i]);
        lv_label_set_long_mode(ml, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(ml, LV_ALIGN_TOP_MID, 0, y);
        detail_passthrough(ml);
        break;
      } else break;
    }
  } else {
    // ---- RECTANGLE scroll: WIDE + SHORT box in the fat middle of the circle. ----
    // Clear zones: title on top / wide text / sub-agent dots + ▼ below.
    int rt = sb_band_top - 2; if (rt < 100) rt = 100;         // top = max(100, title_bottom+8)
    int rb = 268;                                             // bottom fixed (dots get y284+)
    lv_obj_t* cont = lv_obj_create(board_content);
    lv_obj_set_size(cont, 264, rb - rt);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_top(cont, 12, 0);
    lv_obj_set_style_pad_bottom(cont, 12, 0);
    lv_obj_set_style_pad_left(cont, 0, 0);
    lv_obj_set_style_pad_right(cont, 0, 0);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, rt);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(cont, 0, LV_PART_SCROLLBAR);
    quiet_clickable(cont);
    // Container owns the press: a tap (no drag) closes, a long-press records, a drag
    // scrolls natively (scroll suppresses the click, so no accidental close).
    lv_obj_add_event_cb(cont, board_press_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(cont, board_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(cont, board_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(cont, board_press_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(cont, rect_scroll_cb, LV_EVENT_SCROLL, NULL);

    lv_obj_t* sg = render_rect_spangroup(cont);
    lv_obj_align(sg, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_update_layout(cont);                               // realize span height for stacking
    int sh = lv_obj_get_height(sg);
    lv_obj_t* ml = make_label(cont, meta, &lv_font_montserrat_12, COL_SB_GREY);
    lv_obj_set_style_text_letter_space(ml, 1, 0);
    lv_obj_set_width(ml, 248);
    lv_label_set_long_mode(ml, LV_LABEL_LONG_WRAP);
    lv_obj_align(ml, LV_ALIGN_TOP_MID, 0, sh + 14);
    detail_passthrough(ml);
    s_detail_scroll = cont;

    // ▼ affordance at y304; hidden once scrolled to the bottom.
    sb_more_obj = make_label(board_content, LV_SYMBOL_DOWN, &lv_font_montserrat_12, COL_SB_GREY);
    lv_obj_set_style_text_opa(sb_more_obj, LV_OPA_40, 0);
    lv_obj_align(sb_more_obj, LV_ALIGN_TOP_MID, 0, 304);
    detail_passthrough(sb_more_obj);
    lv_obj_update_layout(cont);
    if (lv_obj_get_scroll_bottom(cont) <= 4) lv_obj_add_flag(sb_more_obj, LV_OBJ_FLAG_HIDDEN);
  }

  // Sub-agent dots: y284 in the wide rectangle's bottom zone, y318 under static circle.
  if (has_dots) render_subagent_dots(s_detail_scroll ? 284 : 318);
}

// ===================== FLEET LIST =====================
// First-seen registry: sync with live[] keeping survivor order stable (never re-sort).
static void fleet_registry_update() {
  int w = 0;                                                 // drop vanished ids, keep order
  for (int i = 0; i < s_fleet_n; i++) {
    bool exists = false;
    for (int j = 0; j < live_n; j++) if (!strcmp(s_fleet_ids[i], live[j].id)) { exists = true; break; }
    if (exists) { if (w != i) strlcpy(s_fleet_ids[w], s_fleet_ids[i], sizeof(s_fleet_ids[w])); w++; }
  }
  s_fleet_n = w;
  for (int j = 0; j < live_n; j++) {                         // append newly-seen ids at the end
    bool known = false;
    for (int i = 0; i < s_fleet_n; i++) if (!strcmp(s_fleet_ids[i], live[j].id)) { known = true; break; }
    if (!known && s_fleet_n < SB_MAX_SESSIONS) {
      strlcpy(s_fleet_ids[s_fleet_n], live[j].id, sizeof(s_fleet_ids[s_fleet_n]));
      s_fleet_n++;
    }
  }
}

// Fleet header press dispatcher: a short tap goes BACK to the carousel; a LONG-press
// starts a brand-NEW session by voice (reserved sentinel id, see rec_start); release
// stops it. SHORT_CLICKED (not CLICKED) so a long-press doesn't also fire "back".
static void fleet_header_cb(lv_event_t* e) {
  switch (lv_event_get_code(e)) {
    case LV_EVENT_SHORT_CLICKED: fleet_close_to_carousel(); break;
    case LV_EVENT_LONG_PRESSED:  rec_start(true); break;      // new session
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:    if (s_recording) rec_stop(); break;
    default: break;
  }
}
// The Settings entry at the bottom of the fleet list.
static void fleet_settings_cb(lv_event_t* e) { (void)e; settings_open(); }

// ALL-AGENTS list: a tappable header (the ONLY tap target — back to carousel), then
// a uniform straight column of dot+title rows in a fixed band (y76..284). Rows are
// NOT tappable (too dense -> mistaps); knob/swipe scroll only. Gold up/down arrows
// mark off-window rows. Stable first-seen order (registry).
#define FLEET_BAND_TOP   76
#define FLEET_BAND_BOT   284
#define FLEET_ROW_W      236                 // uniform row width (chord-safe: half≈121 at y284)
#define FLEET_DOT_OFF    8                   // dot inset from the row's left edge
#define FLEET_TITLE_OFF  26                  // title inset from the row's left edge
static void build_fleet() {
  // Header zone FIRST (below the labels): short-tap = back, long-press = new session.
  lv_obj_t* hz = lv_obj_create(board_content);
  lv_obj_set_size(hz, EXAMPLE_LCD_H_RES, 64);
  lv_obj_set_pos(hz, 0, 0);
  lv_obj_set_style_bg_opa(hz, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(hz, 0, 0);
  lv_obj_set_style_radius(hz, 0, 0);
  lv_obj_set_style_pad_all(hz, 0, 0);
  lv_obj_clear_flag(hz, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hz, LV_OBJ_FLAG_CLICKABLE);
  quiet_clickable(hz);
  lv_obj_add_event_cb(hz, fleet_header_cb, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_event_cb(hz, fleet_header_cb, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(hz, fleet_header_cb, LV_EVENT_RELEASED, NULL);
  lv_obj_add_event_cb(hz, fleet_header_cb, LV_EVENT_PRESS_LOST, NULL);

  // Back affordance (RIGHT: the carousel is to the right) + eyebrow with the count
  // folded in (frees y58 for the up-overflow arrow).
  lv_obj_t* back = make_label(board_content, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, COL_SB_GREY);
  lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 20);
  char ebuf[24]; snprintf(ebuf, sizeof(ebuf), "ALL AGENTS  %d", s_fleet_n);
  lv_obj_t* eb = make_label(board_content, ebuf, &lv_font_montserrat_14, COL_SB_TEAL_BR);
  lv_obj_set_style_text_letter_space(eb, 2, 0);
  lv_obj_align(eb, LV_ALIGN_TOP_MID, 0, 42);

  if (s_fleet_top > s_fleet_n - 1) s_fleet_top = s_fleet_n - 1;
  if (s_fleet_top < 0) s_fleet_top = 0;
  int lh = lv_font_get_line_height(&roboto_reg_20);

  // Uniform straight column: every row same width/left-edge; text left-aligned. Only
  // the height varies (1-line 30 / 2-line taller). Fill the band top-down; stop when
  // the next row would cross the band bottom.
  int y = FLEET_BAND_TOP;
  int ri = s_fleet_top;
  s_fleet_last_vis = s_fleet_top - 1;
  while (ri < s_fleet_n) {
    int idx = -1;
    for (int j = 0; j < live_n; j++) if (!strcmp(live[j].id, s_fleet_ids[ri])) { idx = j; break; }
    if (idx < 0) { ri++; continue; }                        // vanished mid-frame -> skip
    sb_session_t* s = &live[idx];

    lv_point_t nsz;                                          // measure the title at the fixed width
    lv_txt_get_size(&nsz, s->name, &roboto_reg_20, 0, 0, FLEET_ROW_W - FLEET_TITLE_OFF - 4,
                    LV_TEXT_FLAG_NONE);
    int rowh = (nsz.y > lh + lh / 2) ? (2 * lh + 8) : 30;   // 2 lines -> taller row
    if (y + rowh > FLEET_BAND_BOT) break;                    // next row would cross the band bottom

    lv_obj_t* row = lv_obj_create(board_content);           // NOT clickable (rows aren't tap targets)
    lv_obj_set_size(row, FLEET_ROW_W, rowh);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);

    lv_obj_t* dot = lv_obj_create(row);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color(s)), 0);   // dormant -> grey
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, FLEET_DOT_OFF, 0);
    if (dot_pulses(s)) add_pulse(dot);

    lv_obj_t* nm = make_label(row, s->name, &roboto_reg_20, COL_SB_WHITE);
    lv_obj_set_width(nm, FLEET_ROW_W - FLEET_TITLE_OFF - 4);
    lv_obj_set_height(nm, rowh == 30 ? lh : 2 * lh);         // clip to 1 or 2 lines
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, FLEET_TITLE_OFF, 0);

    s_fleet_last_vis = ri;
    y += rowh + 2;                                           // small inter-row gap
    ri++;
  }

  // Settings entry, revealed once the LAST agent is in view (scroll to the bottom). A
  // clickable child, so a tap opens Settings while long-press-elsewhere still = new session.
  if (s_fleet_last_vis == s_fleet_n - 1 && y + 6 <= FLEET_BAND_BOT + 34) {
    lv_obj_t* set = lv_obj_create(board_content);
    lv_obj_set_size(set, 168, 32);
    lv_obj_set_style_bg_opa(set, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(set, 0, 0);
    lv_obj_set_style_radius(set, 0, 0);
    lv_obj_set_style_pad_all(set, 0, 0);
    lv_obj_clear_flag(set, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(set, LV_OBJ_FLAG_CLICKABLE);
    quiet_clickable(set);
    lv_obj_align(set, LV_ALIGN_TOP_MID, 0, y + 4);
    lv_obj_add_event_cb(set, fleet_settings_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* sl = make_label(set, LV_SYMBOL_SETTINGS "  Settings", &lv_font_montserrat_14, COL_SB_GREY);
    lv_obj_center(sl);
  }

  // Off-window markers: gold arrows (clearer than bars). Up @ y58, down @ y294.
  if (s_fleet_top > 0) {                                     // rows exist above the window
    lv_obj_t* up = make_label(board_content, LV_SYMBOL_UP, &lv_font_montserrat_14, COL_SB_GOLD);
    lv_obj_align(up, LV_ALIGN_TOP_MID, 0, 58);
  }
  if (s_fleet_last_vis < s_fleet_n - 1) {                    // rows exist below the window
    lv_obj_t* dn = make_label(board_content, LV_SYMBOL_DOWN, &lv_font_montserrat_14, COL_SB_GOLD);
    lv_obj_align(dn, LV_ALIGN_TOP_MID, 0, 294);
  }
}

static void build_view() {
  s_detail_scroll = NULL;               // content about to be cleaned
  sb_more_obj = NULL;                   // invalidate (freed by clean below)
  lv_obj_clean(board_content);          // deletes children + their anims

  if (s_fleet_open && live_n == 0) s_fleet_open = false;   // no agents -> no fleet
  bool in_detail = !s_fleet_open && s_detail_open && live_n > 0;
  // Fleet + overview share the near-black bg; detail is teal (plain set, no fade).
  lv_obj_set_style_bg_color(board_screen,
      lv_color_hex(in_detail ? COL_SB_BG_DETAIL : COL_SB_BG), LV_PART_MAIN);

  // Fleet uses gold overflow markers instead of the rim halo (rim = focused-agent
  // position, meaningless in a flat list). Hide it on fleet, restore on every other
  // view — so ALL fleet exits (swipe-back, header-tap, row-tap jump) bring it back.
  if (rim_arc) {
    if (s_fleet_open) lv_obj_add_flag(rim_arc, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_clear_flag(rim_arc, LV_OBJ_FLAG_HIDDEN);
  }

  if (s_fleet_open)   build_fleet();
  else if (in_detail) build_detail();
  else                build_focus_card();   // also handles the empty list

  rendered_sel    = s_sel;
  rendered_detail = s_detail_open;
  rendered_live_n = live_n;
  rendered_stale  = s_stale;
  rendered_fleet  = s_fleet_open;
  if (live_n > 0 && s_sel >= 0 && s_sel < live_n) rendered_sess = live[s_sel];
  else memset(&rendered_sess, 0, sizeof(rendered_sess));

  if (!s_recording)   // single-writer: only PCM goes to Serial while recording
    Serial.printf("[VIEW] sel=%d n=%d%s%s\n", s_sel, live_n,
                  s_fleet_open ? " fleet" : "", s_detail_open ? " detail" : "");
}

// ===================== refresh gesture (CCW x5 on overview) =====================
static void hide_refresh_cue() {
  if (refresh_cue) lv_obj_add_flag(refresh_cue, LV_OBJ_FLAG_HIDDEN);
  if (s_refresh_hide) { lv_timer_del(s_refresh_hide); s_refresh_hide = NULL; }
}
static void rec_thump_cb(lv_timer_t* t) {   // second beat of the record-stop double haptic
  (void)t;
  s_rec_thump = NULL;                   // one-shot: NULL first
  haptic_click();
}
static void refresh_hide_cb(lv_timer_t* t) {
  (void)t;
  s_refresh_hide = NULL;                 // one-shot: NULL first (before hide clears it too)
  if (refresh_cue) lv_obj_add_flag(refresh_cue, LV_OBJ_FLAG_HIDDEN);
}
static void trigger_refresh() {
  Serial.println("[SB] refresh gesture");   // NO haptic (haptics are recording-only)
  proto_send("{\"t\":\"refresh\"}");        // to the Mac daemon (USB + TCP), own line, valid JSON

  if (refresh_cue) {
    lv_obj_clear_flag(refresh_cue, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(refresh_cue);
  }
  if (s_refresh_hide) lv_timer_del(s_refresh_hide);
  s_refresh_hide = lv_timer_create(refresh_hide_cb, 6000, NULL);   // fallback hide
  lv_timer_set_repeat_count(s_refresh_hide, 1);
}

// ===================== voice recording (hold-to-talk) =====================
// WAV header (44 bytes) into a caller buffer — placeholder sizes; the daemon rebuilds
// it from the actual byte count between the WAV markers. Buffer form (not Stream) so
// it can go through voice_out() to either Serial or the TCP socket. `out` must be >=44.
static void wav_header_bytes(uint8_t* out, uint32_t sample_rate, uint16_t bits_per_sample,
                             uint16_t num_channels, uint32_t num_samples) {
  uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
  uint16_t block_align = num_channels * (bits_per_sample / 8);
  uint32_t data_bytes = num_samples * num_channels * (bits_per_sample / 8);
  uint32_t riff_chunk_size = 36 + data_bytes;
  uint32_t fmt_chunk_size = 16; uint16_t audio_format = 1;   // PCM
  int o = 0;
  memcpy(out + o, "RIFF", 4);            o += 4; memcpy(out + o, &riff_chunk_size, 4); o += 4;
  memcpy(out + o, "WAVE", 4);            o += 4; memcpy(out + o, "fmt ", 4);           o += 4;
  memcpy(out + o, &fmt_chunk_size, 4);   o += 4; memcpy(out + o, &audio_format, 2);    o += 2;
  memcpy(out + o, &num_channels, 2);     o += 2; memcpy(out + o, &sample_rate, 4);     o += 4;
  memcpy(out + o, &byte_rate, 4);        o += 4; memcpy(out + o, &block_align, 2);     o += 2;
  memcpy(out + o, &bits_per_sample, 2);  o += 2;
  memcpy(out + o, "data", 4);            o += 4; memcpy(out + o, &data_bytes, 4);      o += 4;
  // o == 44
}

// Producer: runs on the AUDIO task. NEVER writes Serial — pushes into the ring.
// Overflow policy: drop-OLDEST (advance tail) and count; the count is logged only
// AFTER the stream ends, never mid-stream (would corrupt the PCM).
static void rec_audio_cb(const int16_t* samples, size_t n) {
  if (!s_ring || n == 0) return;
  const uint8_t* p = (const uint8_t*)samples;
  size_t bytes = n * sizeof(int16_t);
  if (s_spool_mode) {
    // SPOOL: linear append; on full, STOP appending (keep the earliest audio -> the actual
    // speech) and flag loop() to auto-stop capture. tail stays 0 (no draining during capture).
    portENTER_CRITICAL(&s_ring_mux);
    size_t off   = s_ring_head;
    size_t freeb = off < s_spool_cap ? (s_spool_cap - off) : 0;
    size_t take  = bytes < freeb ? bytes : freeb;
    if (take) { memcpy(s_ring + off, p, take); s_ring_head = off + take; }
    if (take < bytes) s_spool_full = true;
    portEXIT_CRITICAL(&s_ring_mux);
    return;
  }
  // LIVE: circular drop-oldest ring (drained during capture by the loop task).
  portENTER_CRITICAL(&s_ring_mux);
  size_t used = (s_ring_head + s_spool_cap - s_ring_tail) % s_spool_cap;
  size_t freeb = s_spool_cap - 1 - used;
  if (bytes > freeb) {
    size_t drop = bytes - freeb;
    s_ring_tail = (s_ring_tail + drop) % s_spool_cap;
    s_ring_dropped += drop;
  }
  size_t first = s_spool_cap - s_ring_head;
  if (first > bytes) first = bytes;
  memcpy(s_ring + s_ring_head, p, first);
  if (bytes > first) memcpy(s_ring, p + first, bytes - first);
  s_ring_head = (s_ring_head + bytes) % s_spool_cap;
  portEXIT_CRITICAL(&s_ring_mux);
}

// Consumer: LOOP task only. Drain up to `cap` bytes of PCM to the locked transport
// (TCP socket via the tx ring when recording over Wi-Fi, else Serial).
static void rec_drain(size_t cap) {
  static uint8_t stage[SB_REC_DRAIN_CAP];
  if (!s_ring) return;
  if (cap > SB_REC_DRAIN_CAP) cap = SB_REC_DRAIN_CAP;
  size_t take;
  portENTER_CRITICAL(&s_ring_mux);
  size_t used = (s_ring_head + s_spool_cap - s_ring_tail) % s_spool_cap;
  take = used < cap ? used : cap;
  size_t first = s_spool_cap - s_ring_tail;
  if (first > take) first = take;
  memcpy(stage, s_ring + s_ring_tail, first);
  if (take > first) memcpy(stage + first, s_ring, take - first);
  s_ring_tail = (s_ring_tail + take) % s_spool_cap;
  portEXIT_CRITICAL(&s_ring_mux);
  if (take) voice_out(stage, take);
}
static void rec_drain_full() {
  for (;;) {
    portENTER_CRITICAL(&s_ring_mux);
    size_t used = (s_ring_head + s_spool_cap - s_ring_tail) % s_spool_cap;
    portEXIT_CRITICAL(&s_ring_mux);
    if (used == 0) break;
    rec_drain(SB_REC_DRAIN_CAP);
  }
}

// Show/hide the recording overlay + recolour the rim indicator red.
static void rec_ui(bool on) {
  if (rec_overlay) {
    if (on) {
      lv_obj_clear_flag(rec_overlay, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(rec_overlay);
    } else {
      lv_obj_add_flag(rec_overlay, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (rim_arc) {
    lv_obj_set_style_arc_color(rim_arc, lv_color_hex(on ? 0xE5484D : COL_SB_GOLD), LV_PART_INDICATOR);
    if (on) lv_obj_clear_flag(rim_arc, LV_OBJ_FLAG_HIDDEN);   // show the red rim even in the fleet
    // (off path: build_view re-hides it for the fleet as needed)
  }
  if (rec_dot) {
    if (on) add_pulse(rec_dot);
    else    lv_anim_del(rec_dot, dot_pulse_cb);   // stop pulse on the persistent dot
  }
}

// ---- Animated voice feedback (replaces the text toasts) ----
// Grows/keeps the result pop circle centred at fb_pop_cy as its size animates.
static void fb_pop_cb(void* var, int32_t v) {
  lv_obj_t* c = (lv_obj_t*)var;
  lv_obj_set_size(c, v, v);
  lv_obj_align(c, LV_ALIGN_TOP_MID, 0, fb_pop_cy - v / 2);
}
// Whole-overlay opacity (cascades to every feedback child) — used for the fade-out.
static void fb_fade_root_cb(void* var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}
// Tear down all in-flight feedback (objects + timer + any running root fade), and
// reset the overlay opacity so the next feedback starts fully opaque.
static void fb_clear() {
  if (fb_timer) { lv_timer_del(fb_timer); fb_timer = NULL; }
  fb_pop = NULL;
  if (fb_root) {
    lv_anim_del(fb_root, fb_fade_root_cb);
    lv_obj_clean(fb_root);                     // deletes children + their anims
    lv_obj_set_style_opa(fb_root, LV_OPA_COVER, 0);
  }
}
static void fb_clear_timer_cb(lv_timer_t* t) { (void)t; fb_timer = NULL; fb_clear(); }
static void fb_delete_cb(lv_timer_t* t)     { (void)t; fb_timer = NULL; fb_clear(); }
// Dismiss the full-screen voice-failure diagnostic (tap anywhere).
static void diag_dismiss_cb(lv_event_t* e) {
  (void)e;
  if (s_diag) { lv_obj_del(s_diag); s_diag = NULL; }
  s_diag_open = false;
}
// The sending dots timed out with NO daemon reply (the silent-clear case Matt hit) —
// surface the full-screen diagnostic instead of quietly dropping it. One-shot.
static void fb_sending_timeout_cb(lv_timer_t* t) {
  (void)t;
  fb_timer = NULL;                              // one-shot: NULL first (auto-deleted after return)
  show_voice_fail("no response", COL_SB_GOLD);  // amber: no answer at all
}
// After the hold, fade the whole overlay out, then delete once the fade completes.
static void fb_start_fade(lv_timer_t* t) {
  (void)t;
  fb_timer = NULL;
  if (!fb_root) return;
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, fb_root);
  lv_anim_set_exec_cb(&a, fb_fade_root_cb);
  lv_anim_set_values(&a, 255, 0);
  lv_anim_set_time(&a, 320);
  lv_anim_start(&a);
  fb_timer = lv_timer_create(fb_delete_cb, 340, NULL);
  lv_timer_set_repeat_count(fb_timer, 1);
}
// Staggered twinkle on one sending dot (the idle-starfield pattern).
static void fb_dot_pulse(lv_obj_t* d, uint32_t delay) {
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, d);
  lv_anim_set_exec_cb(&a, dot_pulse_cb);       // reuse: sets bg_opa
  lv_anim_set_values(&a, 40, 255);
  lv_anim_set_time(&a, 450);
  lv_anim_set_playback_time(&a, 450);
  lv_anim_set_delay(&a, delay);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}
// SENDING: three gold dots at the bottom, pulsing in a staggered wave. 15s fallback
// raises the full-screen diagnostic if the daemon never replies (was a silent clear).
static void fb_sending() {
  fb_clear();
  if (!fb_root) return;
  const int spacing = 16, sz = 6;
  for (int i = 0; i < 3; i++) {
    lv_obj_t* d = lv_obj_create(fb_root);
    lv_obj_set_size(d, sz, sz);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(COL_SB_GOLD), 0);
    lv_obj_set_style_bg_opa(d, 40, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(d, LV_ALIGN_TOP_MID, (i - 1) * spacing, 300);
    fb_dot_pulse(d, i * 150);
  }
  // A spooled upload over a slow tunnel can legitimately take far longer than 15s before the
  // daemon's "sent" arrives, so widen the no-reply fallback in that case (past the 180s upload
  // ceiling). Live/LAN stays snappy at 15s. A real failure still surfaces early via fb_result.
  uint32_t no_reply_ms = s_upload_active ? 200000 : 15000;
  fb_timer = lv_timer_create(fb_sending_timeout_cb, no_reply_ms, NULL);   // no reply -> diagnostic
  lv_timer_set_repeat_count(fb_timer, 1);
}
// DELIVERED/FAILED: pop a green ✓ / red ✗ circle (overshoot) at centre-bottom; on
// failure show the short err reason beneath and hold longer so it's readable.
// Three result states: green check (delivered+read), AMBER check (delivered but the
// agent hasn't looked yet), red X (failed). Same overshoot pop; caption + hold vary.
static void fb_result(bool ok, bool unread, const char* err) {
  fb_clear();
  if (!fb_root) return;
  fb_pop_cy = 270;
  bool amber = ok && unread;
  uint32_t col = !ok ? 0xE5484D : (amber ? COL_SB_GOLD : COL_SB_GREEN);
  fb_pop = lv_obj_create(fb_root);
  lv_obj_set_size(fb_pop, 12, 12);
  lv_obj_set_style_radius(fb_pop, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(fb_pop, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(fb_pop, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(fb_pop, 0, 0);
  lv_obj_clear_flag(fb_pop, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(fb_pop, LV_ALIGN_TOP_MID, 0, fb_pop_cy - 6);
  lv_obj_t* sym = make_label(fb_pop, ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,   // amber reuses OK (gold circle)
                             &lv_font_montserrat_28, COL_SB_WHITE);
  lv_obj_center(sym);

  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, fb_pop);
  lv_anim_set_exec_cb(&a, fb_pop_cb);
  lv_anim_set_values(&a, 12, 64);
  lv_anim_set_time(&a, 360);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
  lv_anim_start(&a);

  uint32_t hold = 900;
  char buf[40];
  const char* cap = NULL;
  uint32_t cap_col = 0xE5484D;
  if (!ok) {
    snprintf(buf, sizeof(buf), "%.28s", (err && err[0]) ? err : "failed");
    cap = buf; cap_col = 0xE5484D; hold = 2500;
  } else if (amber) {
    cap = "delivered, unread"; cap_col = COL_SB_GOLD; hold = 1600;
  }
  if (cap) {
    lv_obj_t* el = make_label(fb_root, cap, &lv_font_montserrat_12, cap_col);
    lv_obj_set_width(el, 240);
    lv_label_set_long_mode(el, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(el, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(el, LV_ALIGN_TOP_MID, 0, 314);   // beneath the pop (270 + 32)
  }
  fb_timer = lv_timer_create(fb_start_fade, hold, NULL);
  lv_timer_set_repeat_count(fb_timer, 1);
}

// Full-screen, photographable voice-failure diagnostic. Opaque dark overlay on
// board_screen (survives view rebuilds), dense montserrat_12 key/value lines, NO
// auto-clear — persists until the user taps (time to take the photo). header_col:
// red for an explicit failure, gold for a no-response timeout.
static void show_voice_fail(const char* reason, uint32_t header_col) {
  fb_clear();                                   // remove any pop/sending dots
  if (!board_screen) return;
  if (s_diag) { lv_obj_del(s_diag); s_diag = NULL; }   // replace any prior diagnostic

  s_diag = lv_obj_create(board_screen);
  lv_obj_set_size(s_diag, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_center(s_diag);
  lv_obj_set_style_bg_color(s_diag, lv_color_hex(0x0A0D0E), 0);
  lv_obj_set_style_bg_opa(s_diag, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_diag, 0, 0);
  lv_obj_set_style_border_width(s_diag, 0, 0);
  lv_obj_set_style_pad_all(s_diag, 0, 0);
  lv_obj_clear_flag(s_diag, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_diag, LV_OBJ_FLAG_CLICKABLE);
  quiet_clickable(s_diag);
  lv_obj_add_event_cb(s_diag, diag_dismiss_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_move_foreground(s_diag);
  s_diag_open = true;

  lv_obj_t* hdr = make_label(s_diag, "VOICE SEND FAILED", &lv_font_montserrat_14, header_col);
  lv_obj_set_style_text_letter_space(hdr, 1, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 90);

  char ip[20]; strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
  uint32_t ago = last_commit_ms ? (millis() - last_commit_ms) / 1000 : 0;
  char body[320];
  snprintf(body, sizeof(body),
           "%s\n"
           "rec %ums via %s\n"
           "tx push=%u drain=%u\n"
           "drop=%u\n"
           "wifi %s %s %ddBm\n"
           "tcp %s  ack %us ago\n"
           "agent %s",
           (reason && reason[0]) ? reason : "unknown",
           (unsigned)s_last_rec_dur, s_last_rec_tcp ? "tcp" : "serial",
           (unsigned)s_rec_pushed, (unsigned)s_rec_drained,
           (unsigned)s_rec_txdrop,
           s_wifi_up ? "up" : "DOWN", ip, (int)WiFi.RSSI(),
           s_tcp_up ? "up" : "DOWN", (unsigned)ago,
           s_rec_id[0] ? s_rec_id : "-");
  lv_obj_t* b = make_label(s_diag, body, &lv_font_montserrat_12, COL_SB_SOFT);
  lv_obj_set_width(b, 240);
  lv_obj_set_style_text_line_space(b, 3, 0);
  lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 116);

  lv_obj_t* hint = make_label(s_diag, "tap to dismiss", &lv_font_montserrat_12, COL_SB_GREY);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -22);
}

// Start recording. new_session=false: message the focused agent (carousel/detail).
// new_session=true: fleet long-press -> start a BRAND-NEW session (sentinel id, the
// daemon spawns it). Runs on the LOOP task. From here on s_recording gates all other
// Serial output so the PCM stream between the markers is the only writer.
static void rec_start(bool new_session) {
  if (s_recording || s_upload_active) return;   // never start over a live capture or an in-flight upload
  if (s_state != ST_BOARD) return;
  if (new_session) {
    if (!s_fleet_open) return;                  // new-session only from the fleet view
  } else {
    if (live_n == 0 || s_fleet_open) return;    // per-agent needs a focused card, not the fleet
  }
  if (!s_audio_ok || !s_ring) { fb_result(false, false, "no mic"); return; }

  fb_clear();                                  // a new recording cancels any in-flight feedback
  if (s_diag_open) diag_dismiss_cb(NULL);       // and any open failure diagnostic
  if (new_session) strlcpy(s_rec_id, "NEWSESS0", sizeof(s_rec_id));   // reserved sentinel
  else             strlcpy(s_rec_id, live[s_sel].id, sizeof(s_rec_id));
  s_ring_head = s_ring_tail = 0;
  s_ring_dropped = 0;
  s_spool_full = false;
  s_last_input_ms = millis();                  // calm-ordering: recording is interaction
  // LOCK the voice transport: prefer the WIRED serial link whenever it's PROVEN alive
  // (an accepted frame arrived over it within 5s), falling back to TCP only when serial
  // is idle/absent. Robust against spotty Wi-Fi while the cable is plugged in.
  bool serial_ok = last_serial_commit_ms && (millis() - last_serial_commit_ms) < 5000;
  s_rec_over_tcp = s_tcp_up && !serial_ok;
  // SPOOL mode only over the REMOTE (ngrok) tunnel, where live 88KB/s can't be sustained.
  // Serial and LAN TCP are fast enough to live-stream, so they stay exactly as before.
  s_spool_mode   = s_rec_over_tcp && s_conn_remote;
  s_rec_pushed = 0; s_rec_drained = 0; s_rec_txdrop = 0;   // tx instrumentation, per-recording
  s_recording = true;                          // <-- mutes diagnostics from here
  s_rec_ui = true;
  haptic_click();
  // Per-agent: rebuild the card without its eyebrow/title. Fleet: DON'T rebuild — that
  // would destroy the header/rows mid-press and lose the RELEASE that stops recording;
  // the RECORDING overlay just draws on top of the intact list.
  if (!s_fleet_open) build_view();
  rec_ui(true);                                // overlay + red rim + red dot pulse

  if (!s_spool_mode) {
    // LIVE: frame + stream to the locked transport now. Sole writer until WAV_END.
    char j[64];
    snprintf(j, sizeof(j), "{\"t\":\"rec\",\"state\":\"start\",\"id\":\"%s\",\"rate\":16000}\n", s_rec_id);
    voice_out_str(j);
    voice_out_str("-- WAV_BEGIN --\n");
    uint8_t hdr[44]; wav_header_bytes(hdr, 16000, 16, 1, 0);   // placeholder sizes; daemon rebuilds
    voice_out(hdr, sizeof(hdr));
  }
  // SPOOL: nothing on the wire yet. The rec-start line + PCM go out AFTER capture (rec_stop),
  // so the daemon's receive watchdog doesn't run during the silent record phase.
  s_rec_start_ms = millis();
  audio_start_recording_ms(SB_REC_MAX_MS);     // 3-min failsafe cap (spool also stops at buffer-full)
}

// Stop recording, flush the tail, close the WAV, report duration. LOOP task only
// (RELEASED event, recstop injection, or the failsafe in loop()).
static void rec_stop() {
  if (!s_recording) return;
  bool over_tcp = s_rec_over_tcp;              // capture for the tx report below
  audio_stop_recording();                      // no more rec_audio_cb -> spool buffer is now stable
  uint32_t dur = millis() - s_rec_start_ms;
  s_last_rec_tcp = over_tcp;                    // stash context for the failure diagnostic
  s_last_rec_dur = dur;

  if (s_spool_mode) {
    // SPOOL: capture done. Kick off the async upload — the rec-start line (now carrying the
    // exact byte count) is the FIRST thing on the wire, so the daemon's receive watchdog only
    // starts once PCM is already flowing. upload_pump() drains the body across loop iterations
    // (UI stays live); s_rec_over_tcp stays TRUE so voice_out keeps routing to the tx ring.
    s_upload_total = s_ring_head;              // linear fill: tail stayed 0
    s_upload_off = 0;
    s_upload_start_ms = s_upload_progress_ms = millis();
    s_upload_last_drained = 0;
    s_rec_pushed = 0; s_rec_drained = 0;
    s_up_calls = s_up_eagain = s_up_err = s_up_offered = 0;   // reset send-path telemetry
    s_up_log_ms = millis(); s_up_log_drained = 0;
    char j[80];
    snprintf(j, sizeof(j), "{\"t\":\"rec\",\"state\":\"start\",\"id\":\"%s\",\"bytes\":%u,\"rate\":16000}\n",
             s_rec_id, (unsigned)s_upload_total);
    voice_out_str(j);
    voice_out_str("-- WAV_BEGIN --\n");
    uint8_t hdr[44]; wav_header_bytes(hdr, 16000, 16, 1, (uint32_t)(s_upload_total / 2));
    voice_out(hdr, sizeof(hdr));
    s_upload_active = true;                    // relaxed watchdog + upload_pump take over
    Serial.printf("[SB] rec: spooled %u bytes (%ums) -> uploading over tunnel\n",
                  (unsigned)s_upload_total, (unsigned)dur);
    if (s_ring_dropped) Serial.printf("[SB] rec dropped %u bytes\n", (unsigned)s_ring_dropped);
  } else {
    // LIVE: flush the tail + close the WAV inline (transport already kept up), as before.
    rec_drain_full();                          // flush everything still buffered (locked transport)
    voice_out_str("\n-- WAV_END --\n");
    char j[48];
    snprintf(j, sizeof(j), "{\"t\":\"rec\",\"state\":\"stop\",\"ms\":%u}\n", (unsigned)dur);
    voice_out_str(j);
    s_rec_over_tcp = false;                     // stream closed -> normal transport routing resumes

    // Diagnostics AFTER the stream framing (safe on both transports — never mid-WAV).
    Serial.println("[SB] rec: WAV_END queued");
    Serial.printf("[SB] rec tx: pcm_pushed=%u ring_drained=%u dropped=%u transport=%s\n",
                  (unsigned)s_rec_pushed, (unsigned)s_rec_drained, (unsigned)s_rec_txdrop,
                  over_tcp ? "tcp" : "usb");
    if (s_ring_dropped)                        // audio-ring drops (mic producer overran)
      Serial.printf("[SB] rec dropped %u bytes\n", (unsigned)s_ring_dropped);
  }

  s_recording = false;                         // <-- diagnostics un-muted (capture is done for both paths)
  s_rec_ui = false;
  s_last_input_ms = millis();                  // calm-ordering: keep the quiet window running after release
  haptic_click();                              // record-stop DOUBLE haptic: first beat...
  if (s_rec_thump) lv_timer_del(s_rec_thump);
  s_rec_thump = lv_timer_create(rec_thump_cb, 90, NULL);   // ...second beat 90ms later (thump)
  lv_timer_set_repeat_count(s_rec_thump, 1);
  rec_ui(false);                               // hide overlay + restore gold rim + stop pulse
  // Fleet (new-session) recording: the list is still intact (rec_start didn't rebuild),
  // and this release may be firing FROM the fleet header's own callback — rebuilding
  // would delete that object mid-dispatch. So just re-hide the rim; don't rebuild.
  if (s_fleet_open) { if (rim_arc) lv_obj_add_flag(rim_arc, LV_OBJ_FLAG_HIDDEN); }
  else              build_view();              // per-agent: restore the eyebrow/title
  fb_sending();                                // animated dots; cleared by "sent" or the 15s fallback
}

// ===================== navigation =====================
// Rim halo reflects the focused agent (sel+1 / live_n) in BOTH overview and
// detail; empty list -> 0. animate_rim(x) = x*1000/max(live_n,1).
static void rim_for_state() {
  animate_rim(live_n > 0 ? s_sel + 1 : 0);
}

// One CCW detent against the first-agent clamp: count toward a forced refresh.
static void ccw_tick() {
  unsigned long now = millis();
  if (now - s_ccw_last_ms > CCW_REFRESH_GAP_MS) s_ccw_count = 0;  // stale run
  s_ccw_last_ms = now;
  if (++s_ccw_count >= CCW_REFRESH_TICKS) {
    s_ccw_count = 0;
    trigger_refresh();
  }
}

// Flip the focused agent (carousel). Rebuilds whichever view is showing (in detail
// it swaps straight to the neighbour's detail). NO haptic.
static void flip_to(int sel) {
  if (live_n == 0) return;
  if (sel < 0) sel = 0;
  if (sel > live_n - 1) sel = live_n - 1;
  if (sel == s_sel) return;             // clamped edge: no-op
  s_ccw_count = 0;                      // any flip resets the refresh gesture
  s_sel = sel;
  rim_for_state();
  build_view();
  Serial.printf("[NAV] agent -> %d\n", s_sel);
}

static void open_detail() {
  if (live_n == 0) return;
  s_detail_open = true;                 // NO haptic (haptics are recording-only)
  build_view();                         // rim unchanged (same agent)
}
static void close_detail() {
  s_detail_open = false;
  build_view();
}
// Fleet list = the level before the first carousel card.
static void open_fleet() {
  if (live_n == 0) return;
  s_fleet_open = true;
  s_detail_open = false;
  s_fleet_top = 0;
  s_ccw_count = 0;
  build_view();
}
static void fleet_close_to_carousel() {
  s_fleet_open = false;
  s_sel = 0;
  s_detail_open = false;
  rim_for_state();
  build_view();
}
// Step the fleet window (CW down / CCW up). Clamped; CCW at the top -> refresh. NO haptic.
// Rows are variable-height, so the bottom clamp is "did the last render show the
// final registry row?" (s_fleet_last_vis), not a fixed FLEET_VIS window.
static void fleet_step(int dir) {
  if (dir > 0) {                         // CW: down (clamp at bottom)
    s_ccw_count = 0;                      // any downward move cancels the refresh run
    if (s_fleet_last_vis < s_fleet_n - 1) { s_fleet_top++; build_view(); }
  } else if (s_fleet_top > 0) {          // CCW: up (a move, cancels the run)
    s_ccw_count = 0;
    s_fleet_top--; build_view();
  } else {                               // CCW already at the top -> refresh counter
    ccw_tick();
  }
}

// One knob detent in a direction.
static void knob_step(int dir) {
  if (s_state != ST_BOARD || s_recording || s_diag_open || s_settings_open || s_wifi_entry_open) return;
  if (s_fleet_open) { fleet_step(dir); return; }     // fleet: scroll rows (silent)
  if (s_detail_open) {                   // detail: knob scrolls the rectangle (static mode: nothing)
    if (s_detail_scroll)                 // NO haptic (text traversal)
      lv_obj_scroll_by(s_detail_scroll, 0, dir > 0 ? -48 : 48, LV_ANIM_ON);
    return;
  }
  // Overview carousel: knob flips the focused agent.
  if (live_n == 0) {                     // empty: only the refresh gesture lives here
    if (dir < 0) ccw_tick();
    else         s_ccw_count = 0;
    return;
  }
  if (dir > 0) {                         // CW: next agent
    s_ccw_count = 0;
    flip_to(s_sel + 1);
    return;
  }
  // CCW: prev agent, or (already on the first) OPEN THE FLEET LIST.
  if (s_sel > 0) flip_to(s_sel - 1);
  else           open_fleet();
}

// ===================== tap / swipe handlers =====================
// --- Factored actions (called by both LVGL events and serial injection) ---
// Whole-screen tap: overview -> open detail; detail -> back to overview.
static void do_tap() {
  if (s_diag_open) { diag_dismiss_cb(NULL); return; }   // any tap dismisses the diagnostic (incl. injected)
  if (s_state != ST_BOARD || live_n == 0 || s_recording) return;
  if (s_fleet_open) return;             // fleet: only the rows are tappable (row handlers do it)
  s_last_input_ms = millis();           // calm-ordering: hold off reorders after a tap
  if (s_detail_open) close_detail();
  else               open_detail();
}
// Swipe: in the FLEET, left -> carousel, vertical -> step rows. On the carousel,
// horizontal flips agents (swipe-right at the first card opens the fleet). Detail
// vertical is native scroll (not here); horizontal flips agents.
static void do_swipe(lv_dir_t d) {
  if (s_diag_open) return;              // navigation frozen while the diagnostic is up
  if (s_state != ST_BOARD || live_n == 0 || s_recording) return;
  s_last_input_ms = millis();           // calm-ordering: hold off reorders after a swipe
  if (s_fleet_open) {
    if (d == LV_DIR_LEFT)        fleet_close_to_carousel();
    else if (d == LV_DIR_TOP)    { fleet_step(+1); fleet_step(+1); fleet_step(+1); }
    else if (d == LV_DIR_BOTTOM) { fleet_step(-1); fleet_step(-1); fleet_step(-1); }
    return;
  }
  if (d == LV_DIR_LEFT)       flip_to(s_sel + 1);
  else if (d == LV_DIR_RIGHT) {
    if (!s_detail_open && s_sel == 0) open_fleet();   // swipe-right at the first card -> fleet
    else                              flip_to(s_sel - 1);
  }
}

// Press dispatcher on the tap zone AND the rectangle scroll container (text children
// are non-clickable, so presses fall through to whichever owns them):
//  - SHORT_CLICKED -> tap (a rectangle drag scrolls natively and suppresses the click).
//  - LONG_PRESSED  -> start recording.
//  - RELEASED/PRESS_LOST -> stop recording.
static void board_press_cb(lv_event_t* e) {
  switch (lv_event_get_code(e)) {
    case LV_EVENT_SHORT_CLICKED: do_tap();  break;
    case LV_EVENT_LONG_PRESSED:
      // Fleet: a STILL hold ANYWHERE (header, over a row, or gap) = new session. A moving
      // drag instead fires a GESTURE -> do_swipe (scrolls the list), because LVGL cancels
      // the pending long-press once the press moves — same still-vs-drag split as detail
      // text. Carousel/detail: message the focused agent.
      rec_start(s_fleet_open);
      break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:    if (s_recording) rec_stop(); break;
    default: break;
  }
}

// Swipe on the board screen (gestures bubble up from the tap zone).
static void board_gesture_cb(lv_event_t* e) {
  (void)e;
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;
  lv_dir_t d = lv_indev_get_gesture_dir(indev);
  lv_indev_wait_release(indev);         // swallow the CLICKED that v8 also fires
  do_swipe(d);
}

// ===================== calm ordering =====================
static bool user_interacting() {
  return s_recording || (millis() - s_last_input_ms < SB_CALM_MS);
}
// Does src's id-sequence differ from the live[] id-sequence (reorder/add/remove)?
static bool order_differs(const sb_session_t* src, int n) {
  if (n != live_n) return true;
  for (int i = 0; i < n; i++) if (strcmp(src[i].id, live[i].id)) return true;
  return false;
}
// Update live[] entries' fields in place, matched by id, WITHOUT changing order/count.
// Keeps the visible card's age/status/text fresh while a reorder is deferred.
static void update_fields_in_place(const sb_session_t* src, int n) {
  for (int i = 0; i < live_n; i++) {
    for (int j = 0; j < n; j++) {
      if (!strcmp(live[i].id, src[j].id)) {
        strlcpy(live[i].name, src[j].name, sizeof(live[i].name));
        strlcpy(live[i].proj, src[j].proj, sizeof(live[i].proj));
        live[i].status = src[j].status;
        live[i].age_s  = src[j].age_s;
        strlcpy(live[i].msg, src[j].msg, sizeof(live[i].msg));
        strlcpy(live[i].dtl, src[j].dtl, sizeof(live[i].dtl));
        break;
      }
    }
  }
}
// Rebuild the focused view only if its rendered content changed. In detail, the
// read position is the page index (build_detail re-paginates and clamps it), so
// there's no scroll offset to preserve.
static void rebuild_visible() {
  bool need = false;
  if (s_sel != rendered_sel)            need = true;
  if (s_detail_open != rendered_detail) need = true;
  if (s_fleet_open != rendered_fleet)   need = true;
  if (s_fleet_open)                     need = true;   // fleet: refresh dots/titles in place
  if (s_stale != rendered_stale)        need = true;
  if (live_n != rendered_live_n)        need = true;   // footer "x OF y" tracks count
  if (live_n == 0) {
    if (rendered_live_n != 0) need = true;
  } else if (sess_changed(&live[s_sel], &rendered_sess, s_detail_open)) {
    need = true;
  }
  if (need) {
    lv_coord_t saved = (s_detail_open && s_detail_scroll)   // preserve rectangle read position
                         ? lv_obj_get_scroll_y(s_detail_scroll) : 0;
    build_view();
    if (s_detail_open && s_detail_scroll && saved > 0)
      lv_obj_scroll_to_y(s_detail_scroll, saved, LV_ANIM_OFF);
  } else {
    rendered_live_n = live_n;
  }
}
// Full apply of a snapshot: swap into live[], re-anchor the focused agent by id,
// update the rim, rebuild. Used for immediate commits AND the deferred reorder.
static void commit_order(const sb_session_t* src, int n) {
  char cur_id[12]; cur_id[0] = 0;
  if (live_n > 0 && s_sel >= 0 && s_sel < live_n)
    strlcpy(cur_id, live[s_sel].id, sizeof(cur_id));

  int new_n = n; if (new_n > SB_MAX_SESSIONS) new_n = SB_MAX_SESSIONS;
  for (int i = 0; i < new_n; i++) live[i] = src[i];
  live_n = new_n;
  last_commit_ms = millis();
  fleet_registry_update();              // keep the first-seen fleet order in sync

  if (live_n == 0) {
    s_sel = 0; s_detail_open = false;
  } else if (cur_id[0]) {
    int found = -1;
    for (int i = 0; i < live_n; i++) { if (!strcmp(live[i].id, cur_id)) { found = i; break; } }
    if (found >= 0) {
      s_sel = found;
    } else {
      if (s_sel > live_n - 1) s_sel = live_n - 1;   // vanished -> clamp
      s_detail_open = false;
    }
  } else if (s_sel > live_n - 1) {
    s_sel = live_n - 1;
  }
  if (s_sel < 0) s_sel = 0;

  rim_for_state();
  rebuild_visible();
}

// ===================== staleness =====================
static void stale_timer_cb(lv_timer_t* t) {
  (void)t;
  if (s_recording) return;                        // no rebuilds/Serial while recording

  // Deferred reorder: apply the stashed snapshot once input has been quiet >= 4s.
  if (s_pending && !user_interacting()) {
    s_pending = false;
    commit_order(s_pending_order, s_pending_n);
  }

  bool stale = (last_commit_ms > 0 && (millis() - last_commit_ms) > 10000);
  if (stale == s_stale) return;
  s_stale = stale;
  if (stale_chip) {
    if (stale) lv_obj_clear_flag(stale_chip, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_add_flag(stale_chip, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_state == ST_BOARD) build_view();          // recolour status dot
}

// ===================== screens =====================
static void wait_dot_opa_cb(void* var, int32_t v) {
  lv_obj_set_style_bg_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}
static void build_wait_screen() {
  wait_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(wait_screen, lv_color_hex(COL_SB_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(wait_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(wait_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(wait_screen, 0, 0);

  lv_obj_t* hdr = make_label(wait_screen, "SESSION BOARD", &lv_font_montserrat_14, COL_SB_TEAL_BR);
  lv_obj_set_style_text_letter_space(hdr, 2, 0);
  lv_obj_align(hdr, LV_ALIGN_CENTER, 0, -70);

  lv_obj_t* dot = lv_obj_create(wait_screen);
  lv_obj_set_size(dot, 14, 14);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(COL_SB_GOLD), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dot, LV_ALIGN_CENTER, 0, 0);

  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, dot);
  lv_anim_set_exec_cb(&a, wait_dot_opa_cb);
  lv_anim_set_values(&a, 60, 255);
  lv_anim_set_time(&a, 900);
  lv_anim_set_playback_time(&a, 900);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);

  lv_obj_t* lbl = make_label(wait_screen, "waiting for session_board.py", &roboto_reg_20, COL_SB_SOFT);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 50);
}

static void build_board_screen() {
  board_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(board_screen, lv_color_hex(COL_SB_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(board_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(board_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(board_screen, 0, 0);

  // Full-perimeter progress arc.
  rim_arc = lv_arc_create(board_screen);
  lv_obj_set_size(rim_arc, 344, 344);
  lv_obj_center(rim_arc);
  lv_arc_set_rotation(rim_arc, 270);
  lv_arc_set_bg_angles(rim_arc, 0, 360);
  lv_arc_set_range(rim_arc, 0, 1000);
  lv_arc_set_value(rim_arc, 0);
  lv_obj_remove_style(rim_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(rim_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(rim_arc, lv_color_hex(COL_SB_TEAL), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(rim_arc, 30, LV_PART_MAIN);   // ~12%
  lv_obj_set_style_arc_width(rim_arc, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_color(rim_arc, lv_color_hex(COL_SB_GOLD), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(rim_arc, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(rim_arc, true, LV_PART_INDICATOR);

  // Tap catcher FIRST (below content). Handles tap (short-click), long-press
  // (record), and release (stop) via board_press_cb.
  board_tap = make_tap_zone(board_screen, board_press_cb);

  // Page content container (transparent, NON-interactive so taps fall through).
  board_content = lv_obj_create(board_screen);
  lv_obj_set_size(board_content, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_center(board_content);
  lv_obj_set_style_bg_opa(board_content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(board_content, 0, 0);
  lv_obj_set_style_radius(board_content, 0, 0);
  lv_obj_set_style_pad_all(board_content, 0, 0);
  lv_obj_clear_flag(board_content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // Staleness cue: subtle grey "reconnecting..." text (no box), hidden until stale.
  stale_chip = make_label(board_screen, "reconnecting...", &lv_font_montserrat_12, COL_SB_GREY);
  lv_obj_align(stale_chip, LV_ALIGN_TOP_MID, 0, 24);
  lv_obj_add_flag(stale_chip, LV_OBJ_FLAG_HIDDEN);

  // "refreshing..." cue (persistent so page rebuilds don't kill it, hidden until
  // the CCW-x5 gesture fires; cleared on the next commit or a 6s fallback timer).
  refresh_cue = make_label(board_screen, "refreshing...", &lv_font_montserrat_12, COL_SB_GOLD);
  lv_obj_align(refresh_cue, LV_ALIGN_TOP_MID, 0, 304);
  lv_obj_add_flag(refresh_cue, LV_OBJ_FLAG_HIDDEN);

  // RECORDING overlay: an opaque strip (blends with the view bg) over the top
  // eyebrow/header area, carrying a pulsing red dot + "RECORDING". Hidden by default.
  rec_overlay = lv_obj_create(board_screen);
  lv_obj_set_size(rec_overlay, 250, 30);
  lv_obj_set_style_bg_opa(rec_overlay, LV_OPA_TRANSP, 0);   // transparent: the view rebuild clears the top area
  lv_obj_set_style_border_width(rec_overlay, 0, 0);
  lv_obj_set_style_radius(rec_overlay, 0, 0);
  lv_obj_set_style_pad_all(rec_overlay, 0, 0);
  lv_obj_clear_flag(rec_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(rec_overlay, LV_ALIGN_TOP_MID, 0, 44);   // sits where the eyebrow/title were (both omitted while recording)
  {
    lv_point_t rsz;
    lv_txt_get_size(&rsz, "RECORDING", &lv_font_montserrat_14, 2, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int rtot = 10 + 8 + rsz.x;
    int rleft = -rtot / 2;
    rec_dot = lv_obj_create(rec_overlay);
    lv_obj_set_size(rec_dot, 10, 10);
    lv_obj_set_style_radius(rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(rec_dot, lv_color_hex(0xE5484D), 0);
    lv_obj_set_style_bg_opa(rec_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rec_dot, 0, 0);
    lv_obj_clear_flag(rec_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(rec_dot, LV_ALIGN_CENTER, rleft + 5, 0);
    lv_obj_t* rl = make_label(rec_overlay, "RECORDING", &lv_font_montserrat_14, 0xE5484D);
    lv_obj_set_style_text_letter_space(rl, 2, 0);
    lv_obj_align(rl, LV_ALIGN_CENTER, rleft + 18 + rsz.x / 2, 0);
  }
  lv_obj_add_flag(rec_overlay, LV_OBJ_FLAG_HIDDEN);

  // Voice-feedback overlay: a transparent, NON-clickable full-screen layer that
  // owns the sending dots + delivered/failed pop. Persistent on board_screen (not
  // board_content), so view rebuilds never kill an animation mid-flight. Created
  // last -> foreground; non-clickable so taps/swipes still fall through to the tap zone.
  fb_root = lv_obj_create(board_screen);
  lv_obj_set_size(fb_root, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
  lv_obj_center(fb_root);
  lv_obj_set_style_bg_opa(fb_root, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(fb_root, 0, 0);
  lv_obj_set_style_radius(fb_root, 0, 0);
  lv_obj_set_style_pad_all(fb_root, 0, 0);
  lv_obj_clear_flag(fb_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // Swipe handling (gestures bubble here from the tap zone).
  lv_obj_add_event_cb(board_screen, board_gesture_cb, LV_EVENT_GESTURE, NULL);
}

// ===================== on-device Wi-Fi setup (touch keyboard) =====================
// A settings screen (reached from the fleet list) -> a Wi-Fi entry screen with an
// lv_keyboard. On Connect it calls the SAME save_sboard_creds()+net_request_reconnect()
// path as the serial {"t":"wifi"} command — no AP mode, so no softAP crash surface.

// Submit typed creds through the proven serial-command path, then show a status line.
static void wifi_connect_submit() {
  const char* ssid = lv_textarea_get_text(wifi_ssid_ta);
  const char* pwd  = lv_textarea_get_text(wifi_pwd_ta);
  if (!ssid || !ssid[0]) {                           // need at least an SSID
    if (wifi_status_lbl) { lv_label_set_text(wifi_status_lbl, "enter a network name"); lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(0xE5484D), 0); }
    return;
  }
  save_sboard_creds(ssid, pwd);
  net_request_reconnect();
  Serial.printf("[SB] wifi: creds set via keyboard (ssid=%s), reconnecting\n", ssid);   // never log pwd
  s_wifi_connecting = true;
  s_wifi_connect_ms = millis();
  if (wifi_kb) lv_obj_add_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);   // hide keyboard while connecting
  if (wifi_status_lbl) {
    lv_label_set_text(wifi_status_lbl, "connecting...");
    lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(COL_SB_GOLD), 0);
  }
}

// Focus a text field -> point the keyboard at it and show it.
static void wifi_ta_cb(lv_event_t* e) {
  lv_obj_t* ta = lv_event_get_target(e);
  lv_keyboard_set_textarea(wifi_kb, ta);
  lv_obj_clear_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);
}
// Keyboard OK: SSID field -> move to password; password field -> Connect. Close -> cancel.
static void wifi_kb_cb(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    if (lv_keyboard_get_textarea(wifi_kb) == wifi_ssid_ta) {
      lv_keyboard_set_textarea(wifi_kb, wifi_pwd_ta);
    } else {
      wifi_connect_submit();
    }
  } else if (code == LV_EVENT_CANCEL) {
    wifi_entry_close();
  }
}
static void wifi_reveal_cb(lv_event_t* e) {   // tap the eye to show/hide the password
  (void)e;
  bool masked = lv_textarea_get_password_mode(wifi_pwd_ta);
  lv_textarea_set_password_mode(wifi_pwd_ta, !masked);
}
static void wifi_cancel_cb(lv_event_t* e) { (void)e; wifi_entry_close(); }
// Shared dark styling for the Wi-Fi setup text fields.
static void style_wifi_ta(lv_obj_t* ta) {
  lv_obj_set_style_bg_color(ta, lv_color_hex(0x1E2528), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(ta, lv_color_hex(COL_SB_TEAL), LV_PART_MAIN);
  lv_obj_set_style_border_width(ta, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(ta, 6, LV_PART_MAIN);
  lv_obj_set_style_text_color(ta, lv_color_hex(COL_SB_WHITE), LV_PART_MAIN);
  lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(ta, 6, LV_PART_MAIN);
  lv_obj_set_style_text_color(ta, lv_color_hex(COL_SB_GREY), LV_PART_TEXTAREA_PLACEHOLDER);
  // Teal focus cursor so the active field is obvious.
  lv_obj_set_style_border_color(ta, lv_color_hex(COL_SB_TEAL_BR), LV_PART_MAIN | LV_STATE_FOCUSED);
}

static void build_wifi_entry_screen() {
  if (wifi_screen) { lv_obj_del(wifi_screen); wifi_screen = NULL; }
  wifi_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(wifi_screen, lv_color_hex(COL_SB_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(wifi_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(wifi_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(wifi_screen, 0, 0);

  lv_obj_t* hdr = make_label(wifi_screen, "WI-FI SETUP", &lv_font_montserrat_14, COL_SB_TEAL_BR);
  lv_obj_set_style_text_letter_space(hdr, 2, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 40);

  // Cancel (X) top-left-ish.
  lv_obj_t* cancel = make_label(wifi_screen, LV_SYMBOL_CLOSE, &lv_font_montserrat_14, COL_SB_GREY);
  lv_obj_align(cancel, LV_ALIGN_TOP_MID, -120, 40);
  lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
  quiet_clickable(cancel);
  lv_obj_set_ext_click_area(cancel, 20);
  lv_obj_add_event_cb(cancel, wifi_cancel_cb, LV_EVENT_CLICKED, NULL);

  // SSID + password fields, stacked in the wide middle band.
  wifi_ssid_ta = lv_textarea_create(wifi_screen);
  lv_textarea_set_one_line(wifi_ssid_ta, true);
  lv_textarea_set_placeholder_text(wifi_ssid_ta, "network name");
  lv_obj_set_width(wifi_ssid_ta, 220);
  lv_obj_align(wifi_ssid_ta, LV_ALIGN_TOP_MID, 0, 70);
  style_wifi_ta(wifi_ssid_ta);
  lv_obj_add_event_cb(wifi_ssid_ta, wifi_ta_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(wifi_ssid_ta, wifi_ta_cb, LV_EVENT_CLICKED, NULL);

  wifi_pwd_ta = lv_textarea_create(wifi_screen);
  lv_textarea_set_one_line(wifi_pwd_ta, true);
  lv_textarea_set_password_mode(wifi_pwd_ta, true);
  lv_textarea_set_placeholder_text(wifi_pwd_ta, "password");
  lv_obj_set_width(wifi_pwd_ta, 220);
  lv_obj_align(wifi_pwd_ta, LV_ALIGN_TOP_MID, 0, 112);
  style_wifi_ta(wifi_pwd_ta);
  lv_obj_add_event_cb(wifi_pwd_ta, wifi_ta_cb, LV_EVENT_FOCUSED, NULL);
  lv_obj_add_event_cb(wifi_pwd_ta, wifi_ta_cb, LV_EVENT_CLICKED, NULL);

  // Eye toggle to reveal the password.
  lv_obj_t* eye = make_label(wifi_screen, LV_SYMBOL_EYE_OPEN, &lv_font_montserrat_14, COL_SB_GREY);
  lv_obj_align_to(eye, wifi_pwd_ta, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
  lv_obj_add_flag(eye, LV_OBJ_FLAG_CLICKABLE);
  quiet_clickable(eye);
  lv_obj_set_ext_click_area(eye, 16);
  lv_obj_add_event_cb(eye, wifi_reveal_cb, LV_EVENT_CLICKED, NULL);

  // Status line (hidden text until Connect).
  wifi_status_lbl = make_label(wifi_screen, "", &lv_font_montserrat_12, COL_SB_GOLD);
  lv_obj_align(wifi_status_lbl, LV_ALIGN_TOP_MID, 0, 150);

  // The keyboard (QWERTY). Bottom portion; on a round display the extreme corner keys
  // clip slightly — acceptable, center keys are all reachable.
  wifi_kb = lv_keyboard_create(wifi_screen);
  lv_keyboard_set_mode(wifi_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(wifi_kb, 360, 176);
  lv_obj_align(wifi_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  // Dark theme so the keyboard doesn't flash a bright default panel over the board palette.
  lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(COL_SB_BG), LV_PART_MAIN);
  lv_obj_set_style_border_width(wifi_kb, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(wifi_kb, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(wifi_kb, lv_color_hex(0x1E2528), LV_PART_ITEMS);
  lv_obj_set_style_text_color(wifi_kb, lv_color_hex(COL_SB_WHITE), LV_PART_ITEMS);
  lv_obj_set_style_border_width(wifi_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_radius(wifi_kb, 4, LV_PART_ITEMS);
  lv_keyboard_set_textarea(wifi_kb, wifi_ssid_ta);
  lv_obj_add_event_cb(wifi_kb, wifi_kb_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(wifi_kb, wifi_kb_cb, LV_EVENT_CANCEL, NULL);

  lv_scr_load(wifi_screen);
}

// Settings screen: minimal — a Wi-Fi entry + a back-to-board.
static void settings_wifi_cb(lv_event_t* e) { (void)e; wifi_entry_open(); }
static void settings_back_cb(lv_event_t* e) { (void)e; settings_close(); }

static void build_settings_screen() {
  if (settings_screen) { lv_obj_del(settings_screen); settings_screen = NULL; }
  settings_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(settings_screen, lv_color_hex(COL_SB_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(settings_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(settings_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(settings_screen, 0, 0);

  lv_obj_t* hdr = make_label(settings_screen, "SETTINGS", &lv_font_montserrat_14, COL_SB_TEAL_BR);
  lv_obj_set_style_text_letter_space(hdr, 2, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 66);

  // "Wi-Fi" option (a pill button).
  lv_obj_t* wbtn = lv_obj_create(settings_screen);
  lv_obj_set_size(wbtn, 200, 56);
  lv_obj_align(wbtn, LV_ALIGN_CENTER, 0, -10);
  lv_obj_set_style_bg_color(wbtn, lv_color_hex(COL_SB_TEAL), 0);
  lv_obj_set_style_bg_opa(wbtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(wbtn, 14, 0);
  lv_obj_set_style_border_width(wbtn, 0, 0);
  lv_obj_clear_flag(wbtn, LV_OBJ_FLAG_SCROLLABLE);
  quiet_clickable(wbtn);
  lv_obj_add_event_cb(wbtn, settings_wifi_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* wl = make_label(wbtn, LV_SYMBOL_WIFI "  Wi-Fi", &roboto_bold_20, COL_SB_WHITE);
  lv_obj_center(wl);

  // Back to board.
  lv_obj_t* back = lv_obj_create(settings_screen);
  lv_obj_set_size(back, 120, 40);
  lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(back, 0, 0);
  lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
  quiet_clickable(back);
  lv_obj_add_event_cb(back, settings_back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = make_label(back, LV_SYMBOL_LEFT "  Back", &lv_font_montserrat_14, COL_SB_GREY);
  lv_obj_center(bl);

  lv_scr_load(settings_screen);
}

static void settings_open() { s_settings_open = true; s_wifi_entry_open = false; build_settings_screen(); }
static void settings_close() {
  s_settings_open = false; s_wifi_entry_open = false; s_wifi_connecting = false; s_wifi_close_ms = 0;
  lv_scr_load(board_screen);
  // del_async: these can be called from a button on the very screen being deleted (its
  // CLICKED event is still on the stack) — defer so we don't free it mid-dispatch.
  if (wifi_screen)     { lv_obj_del_async(wifi_screen);     wifi_screen = NULL; }
  if (settings_screen) { lv_obj_del_async(settings_screen); settings_screen = NULL; }
  wifi_ssid_ta = wifi_pwd_ta = wifi_kb = wifi_status_lbl = NULL;
}
static void wifi_entry_open()  { s_wifi_entry_open = true; build_wifi_entry_screen(); }
static void wifi_entry_close() {
  s_wifi_entry_open = false; s_wifi_connecting = false;
  build_settings_screen();                     // back to settings (loads a fresh settings_screen)
  if (wifi_screen) { lv_obj_del_async(wifi_screen); wifi_screen = NULL; }   // defer (event still on stack)
  wifi_ssid_ta = wifi_pwd_ta = wifi_kb = wifi_status_lbl = NULL;
}

// Poll the join result after a keyboard-submitted connect (called from loop()).
static void wifi_entry_poll() {
  if (s_wifi_connecting && wifi_status_lbl) {
    if (s_wifi_up) {                                 // net task got us connected
      s_wifi_connecting = false;
      lv_label_set_text(wifi_status_lbl, "connected!");
      lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(COL_SB_GREEN), 0);
      s_wifi_close_ms = millis() + 1400;             // hold, then auto-exit to the board
    } else if (millis() - s_wifi_connect_ms > 20000) { // 20s with no join
      s_wifi_connecting = false;
      lv_label_set_text(wifi_status_lbl, "couldn't connect - check + retry");
      lv_obj_set_style_text_color(wifi_status_lbl, lv_color_hex(0xE5484D), 0);
      if (wifi_kb) lv_obj_clear_flag(wifi_kb, LV_OBJ_FLAG_HIDDEN);   // let them edit + retry
    }
    return;
  }
  if (s_wifi_close_ms && millis() > s_wifi_close_ms) {  // "connected!" shown -> return to board
    s_wifi_close_ms = 0;
    settings_close();
  }
}

// ===================== snapshot commit =====================
static void apply_snapshot() {
  hide_refresh_cue();                   // a fresh frame answers any pending refresh
  bool first = (s_state != ST_BOARD);

  if (first) {
    int new_n = shadow_n;
    if (new_n > SB_MAX_SESSIONS) new_n = SB_MAX_SESSIONS;
    for (int i = 0; i < new_n; i++) live[i] = shadow[i];
    live_n = new_n;
    last_commit_ms = millis();
    fleet_registry_update();
    s_pending = false;
    s_state = ST_BOARD;
    s_sel = 0;
    s_detail_open = false;
    lv_arc_set_value(rim_arc, 0);
    lv_scr_load(board_screen);
    build_view();
    rim_for_state();
    if (wait_screen) { lv_obj_del(wait_screen); wait_screen = NULL; }  // safe: no longer active
    Serial.println("[NAV] -> BOARD (first commit)");
    return;
  }

  // Calm ordering: if the list ORDER would change while the user is interacting
  // (knob/tap/swipe within 4s, or recording), do NOT reorder under their fingers.
  // Refresh the visible fields in place (age/status/text stay live) and stash the
  // full snapshot; the 1s stale timer applies it once input is quiet >= 4s.
  if (order_differs(shadow, shadow_n) && user_interacting()) {
    update_fields_in_place(shadow, shadow_n);
    memcpy(s_pending_order, shadow, SB_SESS_ARR_BYTES);
    s_pending_n = shadow_n;
    s_pending = true;
    last_commit_ms = millis();
    rebuild_visible();                  // repaint the focused card with fresh fields, same order
    return;
  }

  // Content-only change, or the user is idle -> apply now (supersedes any pending).
  s_pending = false;
  commit_order(shadow, shadow_n);
}

// ===================== serial RX (Mac -> device, newline-delimited JSON) =====================
static void process_line(const char* buf) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, buf);
  if (err) return;

  const char* t = doc["t"] | "";

  // While recording, the loop task must be the sole Serial writer (PCM only).
  // Parse but emit NOTHING; only the stop injection is honored.
  if (s_recording) {
    if (!strcmp(t, "input") && !strcmp(doc["k"] | "", "recstop")) rec_stop();
    return;
  }

  if (!strcmp(t, "hdr")) {
    int n = doc["n"] | 0;
    if (n < 0 || n > SB_MAX_SESSIONS) { in_frame = false; return; }  // protocol error, drop
    shadow_seq   = doc["seq"] | 0;
    shadow_n     = n;
    shadow_count = 0;
    in_frame     = true;
    memset(shadow, 0, SB_SESS_ARR_BYTES);
  } else if (!strcmp(t, "s")) {
    if (!in_frame) return;
    int i = doc["i"] | -1;
    if (i < 0 || i >= SB_MAX_SESSIONS) return;
    strlcpy(shadow[i].id,   doc["id"] | "", sizeof(shadow[i].id));
    strlcpy(shadow[i].name, doc["nm"] | "", sizeof(shadow[i].name));
    strlcpy(shadow[i].proj, doc["pj"] | "", sizeof(shadow[i].proj));
    const char* st = doc["st"] | "i";
    shadow[i].status = st[0] ? st[0] : 'i';
    shadow[i].age_s  = doc["age"] | 0;
    strlcpy(shadow[i].msg,  doc["msg"] | "", sizeof(shadow[i].msg));
    shadow[i].drm = (doc["drm"] | 0) ? 1 : 0;   // dormant flag (omit -> 0)
    shadow_count++;
  } else if (!strcmp(t, "x")) {
    // Optional richer detail text for row i (arrives between "s" lines and "end").
    // Does NOT count toward the frame; a missing "x" leaves dtl empty.
    if (!in_frame) return;
    int i = doc["i"] | -1;
    if (i < 0 || i >= SB_MAX_SESSIONS) return;
    strlcpy(shadow[i].dtl, doc["dtl"] | "", sizeof(shadow[i].dtl));
    int agn = doc["agn"] | 0; if (agn < 0) agn = 0; if (agn > 255) agn = 255;   // defensive
    int aga = doc["aga"] | 0; if (aga < 0) aga = 0; if (aga > agn) aga = agn;
    shadow[i].agn = (uint8_t)agn;
    shadow[i].aga = (uint8_t)aga;
  } else if (!strcmp(t, "end")) {
    uint32_t seq = doc["seq"] | 0;
    int n = doc["n"] | 0;
    if (in_frame && seq == shadow_seq && n == shadow_n && shadow_count == n) {
      Serial.printf("[SB] ack seq=%d n=%d\n", (int)seq, n);
      if (s_rx_from_serial) last_serial_commit_ms = millis();   // serial link is proven alive
      apply_snapshot();
    } else {
      Serial.println("[SB] drop frame");
    }
    in_frame = false;
  } else if (!strcmp(t, "input")) {
    // Test injection (my scripts only; the daemon never sends this). Runs on the
    // loop task, so LVGL calls here are safe. All actions route through the SAME
    // factored handlers as physical input, so behavior is identical — including
    // the CCW refresh counter (knob delta feeds the normal process_knob path).
    const char* k = doc["k"] | "";
    if (!strcmp(k, "cw")) {
      portENTER_CRITICAL(&s_knob_mux); s_knob_delta += 1; portEXIT_CRITICAL(&s_knob_mux);
    } else if (!strcmp(k, "ccw")) {
      portENTER_CRITICAL(&s_knob_mux); s_knob_delta -= 1; portEXIT_CRITICAL(&s_knob_mux);
    } else if (!strcmp(k, "tap")) {
      if (doc["row"].is<int>()) {         // fleet rows are NOT tappable (only the header is)
        Serial.println("[SB] no rows in this UI");
      } else {
        do_tap();
      }
    } else if (!strcmp(k, "swl")) {
      do_swipe(LV_DIR_LEFT);
    } else if (!strcmp(k, "swr")) {
      do_swipe(LV_DIR_RIGHT);
    } else if (!strcmp(k, "swu")) {
      if (s_detail_open && s_detail_scroll) lv_obj_scroll_by(s_detail_scroll, 0, -48*5, LV_ANIM_ON);
    } else if (!strcmp(k, "swd")) {
      if (s_detail_open && s_detail_scroll) lv_obj_scroll_by(s_detail_scroll, 0, 48*5, LV_ANIM_ON);
    } else if (!strcmp(k, "recstart")) {
      rec_start(doc["new"] | false);     // {"k":"recstart","new":true} -> new-session recording
    } else if (!strcmp(k, "recstop")) {
      rec_stop();                        // no-op if not recording
    }
  } else if (!strcmp(t, "sent")) {
    // Daemon result after a voice prompt was delivered (arrives post-recording).
    bool ok = doc["ok"] | false;
    bool unread = doc["unread"] | false;         // delivered but the agent hasn't looked yet
    if (ok) fb_result(true, unread, NULL);
    else    show_voice_fail(doc["err"] | "send failed", 0xE5484D);   // full-screen diagnostic (red)
  } else if (!strcmp(t, "dump")) {
    int disp = (live_n > 0 && s_sel >= 0 && s_sel < live_n) ? s_sel : -1;
    const char* id = "-";
    const char* nm = "";
    char st = '-';
    char m0[49]; m0[0] = 0;
    if (disp >= 0) {
      id = live[disp].id;
      nm = live[disp].name;
      st = live[disp].status;
      strlcpy(m0, live[disp].msg, sizeof(m0));   // first 48 chars
    }
    int page = s_detail_open ? s_sel + 1 : 0;    // page = detail_open ? sel+1 : 0
    // Dual-write (Serial + active TCP link) so test scripts can read state when serial
    // is closed. proto_send appends the newline on both paths.
    char dumpbuf[256];
    snprintf(dumpbuf, sizeof(dumpbuf),
             "[SB] dump page=%d sel=%d detail=%d n=%d stale=%d fleet=%d id=%s title=\"%s\" st=%c msg0=\"%s\"",
             page, s_sel, s_detail_open ? 1 : 0, live_n, s_stale ? 1 : 0, s_fleet_open ? 1 : 0,
             id, nm, st, m0);
    proto_send(dumpbuf);
  } else if (!strcmp(t, "wifi")) {
    // Set/replace our own Wi-Fi creds (daemon answering a wificreds request, or a
    // manual push). Stored in NVS "sboard"; the net task reloads + reconnects.
    const char* ssid = doc["ssid"] | "";
    const char* pwd  = doc["pwd"]  | "";
    if (ssid[0]) {
      save_sboard_creds(ssid, pwd);
      net_request_reconnect();
      Serial.printf("[SB] wifi: creds set (ssid=%s), reconnecting\n", ssid);  // never log pwd
    }
  } else if (!strcmp(t, "wifiscan")) {
    // Diagnostic: ask the net task to run a read-only WiFi.scanNetworks() and print every AP.
    net_request_wifiscan();
    Serial.println("[SB] scan: queued (net task will run it)");
  } else if (!strcmp(t, "remotecfg")) {
    // Auth token (+ optional ngrok host/port) pushed by the daemon over serial or the
    // first LAN connection. Partial-safe: tok may arrive alone first, host/port later.
    const char* tok  = doc["tok"]  | "";
    const char* host = doc["host"] | "";
    int         port = doc["port"] | 0;
    save_remotecfg(tok, host, port);   // persists tok if present; updates addr only if both present
    Serial.printf("[SB] remotecfg set (tok=%s host=%s port=%d)\n",   // never log the token VALUE
                  tok[0] ? "yes" : "-", host[0] ? host : "-", port);
  }
}

static void serial_poll() {
  static char line[2048];   // must exceed the largest line: an "x" line carries dtl[768]
  static int  len = 0;
  static bool overflow = false;
  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (len > 0 && !overflow) {
        line[len] = 0;
        if (line[0] == '{') { s_rx_from_serial = true; process_line(line); }
      }
      len = 0; overflow = false;
    } else {
      if (overflow) continue;              // discard rest of an overlong line
      if (len < (int)sizeof(line) - 1) line[len++] = (char)c;
      else overflow = true;
    }
  }
}

// ===================== fake data (dev only) =====================
#if SB_FAKE_DATA
static void load_fake_data() {
  memset(live, 0, SB_SESS_ARR_BYTES);
  struct { const char* id; const char* nm; const char* pj; char st; uint32_t age; const char* msg; const char* dtl; } f[] = {
    { "372daa52", "matttaylor-34", "HALOMAIN_rev1p5", 'w', 12,
      "Implementing the Session_Board sketch. Copied the support files from Recipe_Demo, "
      "cloned the haptics and knob blocks verbatim, and I'm now wiring the serial RX protocol "
      "into a shadow buffer that commits atomically on the end frame. The UI has a WAIT screen "
      "with a pulsing dot and a BOARD screen with a rim progress arc, an overview list of up to "
      "seven sessions, and a per-session page with a scrollable detail view. Knob rotates pages, "
      "swipe left/right pages too, and tapping a session opens its full message which the knob "
      "then scrolls instead of paging. Next I need to self-review the z-order so overview rows "
      "actually receive their clicks while empty taps fall through to the catcher below.",
      "DETAIL (dtl): This richer text comes from an \"x\" line and overrides msg in the detail "
      "view only. It can run much longer than the card preview — up to 767 characters — so the "
      "daemon can stream a fuller summary of what the agent is doing without bloating the "
      "carousel card. When absent, the detail view falls back to the msg string verbatim." },
    { "8f1c02ab", "anna-recipes", "trepo-ios", 'd', 45, "Finished household recipe sharing read-union.", "" },
    { "a1b2c3d4", "obs-forwarder", "trepo-backend", 'i', 180, "Idle.", "" },
    { "e5f6a7b8", "recall-check", "trepo-analytics", 'i', 900, "Waiting on FSIS feed refresh.", "" },
    { "c9d0e1f2", "luxafor-daemon", "local-tools", 'i', 3700, "Rendering status light.", "" },
    { "b2c3d4e5", "email-agent", "trepo-backend", 'w', 8, "Drafting first-touch opener emails from the Graph thread history.", "" },
    { "f6a7b8c9", "thyme-audit", "trepo-analytics", 'd', 300, "Convo audit complete; wrote fix recon to Desktop.", "" },
    { "d0e1f2a3", "sense-camera", "halo_sense_prod", 'i', 1200, "Idle after capture pipeline sweep.", "" },
  };
  int n = sizeof(f) / sizeof(f[0]);
  for (int i = 0; i < n; i++) {
    strlcpy(live[i].id,   f[i].id, sizeof(live[i].id));
    strlcpy(live[i].name, f[i].nm, sizeof(live[i].name));
    strlcpy(live[i].proj, f[i].pj, sizeof(live[i].proj));
    live[i].status = f[i].st;
    live[i].age_s  = f[i].age;
    strlcpy(live[i].msg,  f[i].msg, sizeof(live[i].msg));
    strlcpy(live[i].dtl,  f[i].dtl, sizeof(live[i].dtl));
  }
  live_n = n;
  last_commit_ms = millis();
}
#endif

// ===================== input processing (loop task) =====================
#define KNOB_COUNTS_PER_PAGE 1
#define KNOB_ACC_STALE_MS    600
static int32_t       s_knob_acc      = 0;
static unsigned long s_knob_last_ms  = 0;

static void process_knob() {
  int32_t d;
  portENTER_CRITICAL(&s_knob_mux);
  d = s_knob_delta;
  s_knob_delta = 0;
  portEXIT_CRITICAL(&s_knob_mux);
  if (d == 0) return;
  s_last_input_ms = millis();           // calm-ordering: any knob detent counts as interaction

  if (s_state != ST_BOARD || s_recording) { s_knob_acc = 0; return; }  // frozen mid-record

  unsigned long now = millis();
  if (now - s_knob_last_ms > KNOB_ACC_STALE_MS) s_knob_acc = 0;
  s_knob_last_ms = now;

  // Direction change discards leftover counts from the other way.
  if ((s_knob_acc > 0 && d < 0) || (s_knob_acc < 0 && d > 0)) s_knob_acc = 0;
  s_knob_acc += d;

  while (s_knob_acc >= KNOB_COUNTS_PER_PAGE)  { knob_step(+1); s_knob_acc -= KNOB_COUNTS_PER_PAGE; }
  while (s_knob_acc <= -KNOB_COUNTS_PER_PAGE) { knob_step(-1); s_knob_acc += KNOB_COUNTS_PER_PAGE; }
}

// ===================== Wi-Fi / TCP transport (net task on Core 0) =====================
// Discipline mirrors the audio path: a dedicated Core-0 task owns all WiFi/socket I/O
// and never touches LVGL; it hands RX bytes to the loop task via a ring and takes TX
// bytes from another ring. All UI mutation + process_line() stay on the loop task.
// (sb_ring_t is declared near the top of the file so auto-prototypes resolve it.)
static bool ring_init(sb_ring_t* r, size_t sz) {
  r->buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  if (!r->buf) return false;
  r->sz = sz; r->head = 0; r->tail = 0;
  r->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
  return true;
}
// Push up to n bytes; returns bytes actually written (short write = ring full). No
// blocking, short critical section (bounded by the caller's chunk size).
static size_t ring_push(sb_ring_t* r, const uint8_t* p, size_t n) {
  portENTER_CRITICAL(&r->mux);
  size_t used  = (r->head + r->sz - r->tail) % r->sz;
  size_t freeb = r->sz - 1 - used;
  size_t take  = n < freeb ? n : freeb;
  size_t first = r->sz - r->head; if (first > take) first = take;
  memcpy(r->buf + r->head, p, first);
  if (take > first) memcpy(r->buf, p + first, take - first);
  r->head = (r->head + take) % r->sz;
  portEXIT_CRITICAL(&r->mux);
  return take;
}
static void ring_reset(sb_ring_t* r) {
  portENTER_CRITICAL(&r->mux);
  r->head = 0; r->tail = 0;
  portEXIT_CRITICAL(&r->mux);
}
// Pop up to cap bytes into out; returns bytes copied.
static size_t ring_pop(sb_ring_t* r, uint8_t* out, size_t cap) {
  portENTER_CRITICAL(&r->mux);
  size_t used  = (r->head + r->sz - r->tail) % r->sz;
  size_t take  = used < cap ? used : cap;
  size_t first = r->sz - r->tail; if (first > take) first = take;
  memcpy(out, r->buf + r->tail, first);
  if (take > first) memcpy(out + first, r->buf, take - first);
  r->tail = (r->tail + take) % r->sz;
  portEXIT_CRITICAL(&r->mux);
  return take;
}
// Peek up to cap bytes WITHOUT advancing (for socket TX where the write may only
// take part of it — advance by the actually-written count with ring_advance).
static size_t ring_peek(sb_ring_t* r, uint8_t* out, size_t cap) {
  portENTER_CRITICAL(&r->mux);
  size_t used  = (r->head + r->sz - r->tail) % r->sz;
  size_t take  = used < cap ? used : cap;
  size_t first = r->sz - r->tail; if (first > take) first = take;
  memcpy(out, r->buf + r->tail, first);
  if (take > first) memcpy(out + first, r->buf, take - first);
  portEXIT_CRITICAL(&r->mux);
  return take;
}
static void ring_advance(sb_ring_t* r, size_t k) {
  portENTER_CRITICAL(&r->mux);
  r->tail = (r->tail + k) % r->sz;
  portEXIT_CRITICAL(&r->mux);
}
static size_t ring_used(sb_ring_t* r) {
  portENTER_CRITICAL(&r->mux);
  size_t used = (r->head + r->sz - r->tail) % r->sz;
  portEXIT_CRITICAL(&r->mux);
  return used;
}

static WiFiUDP    s_udp;
static WiFiClient s_client;
static sb_ring_t  s_net_tx, s_net_rx;

// ---- NVS credentials (raw nvs_ API, matching production provisioning) ----
static bool nvs_get_str_ns(const char* ns, const char* key, char* out, size_t outsz) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
  size_t sz = outsz;
  esp_err_t e = nvs_get_str(h, key, out, &sz);
  nvs_close(h);
  return (e == ESP_OK) && out[0];
}
// Cred sources, in order: (a) production provisioning creds (survive app-only flashes),
// (b) our own "sboard" namespace (settable over serial).
static bool net_load_creds(char* ssid, size_t ss, char* pwd, size_t ps) {
  ssid[0] = 0; pwd[0] = 0;
  if (nvs_get_str_ns("provisioning", "home_ssid", ssid, ss) &&
      nvs_get_str_ns("provisioning", "home_pass", pwd, ps)) return true;
  ssid[0] = 0; pwd[0] = 0;
  if (nvs_get_str_ns("sboard", "ssid", ssid, ss) &&
      nvs_get_str_ns("sboard", "pwd", pwd, ps)) return true;
  return false;
}
static void save_sboard_creds(const char* ssid, const char* pwd) {
  nvs_handle_t h;
  if (nvs_open("sboard", NVS_READWRITE, &h) != ESP_OK) { Serial.println("[SB] wifi: nvs open failed"); return; }
  nvs_set_str(h, "ssid", ssid);
  nvs_set_str(h, "pwd",  pwd);
  nvs_commit(h);
  nvs_close(h);
}
static bool nvs_get_i32_ns(const char* ns, const char* key, int32_t* out) {
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
  esp_err_t e = nvs_get_i32(h, key, out);
  nvs_close(h);
  return e == ESP_OK;
}
// Remote (ngrok) dial config, persisted in "sboard". All three must be present to dial.
static bool net_load_remote(char* host, size_t hs, int* port, char* tok, size_t ts) {
  host[0] = 0; tok[0] = 0; *port = 0;
  int32_t p = 0;
  bool ok = nvs_get_str_ns("sboard", "remote_host", host, hs) &&
            nvs_get_i32_ns("sboard", "remote_port", &p) && p > 0 &&
            nvs_get_str_ns("sboard", "remote_tok", tok, ts);
  *port = (int)p;
  return ok;
}
// Partial-safe persist: remotecfg may arrive as tok-only first (tunnel not resolved),
// then tok+host+port later, and host/port change every ngrok session. Persist tok
// whenever present; update host/port ONLY when both present — NEVER wipe a stored
// address when a later push omits it (keeps the last-known remote for off-LAN dialing).
static void save_remotecfg(const char* tok, const char* host, int port) {
  nvs_handle_t h;
  if (nvs_open("sboard", NVS_READWRITE, &h) != ESP_OK) { Serial.println("[SB] remotecfg: nvs open failed"); return; }
  bool dirty = false;
  if (tok && tok[0]) { nvs_set_str(h, "remote_tok", tok); dirty = true; }
  if (host && host[0] && port > 0) {
    nvs_set_str(h, "remote_host", host);
    nvs_set_i32(h, "remote_port", (int32_t)port);
    dirty = true;
  }
  if (dirty) nvs_commit(h);
  nvs_close(h);
}
// Also read our own token (for the auth line on any connection). Empty if unset.
static void net_load_token(char* tok, size_t ts) {
  tok[0] = 0;
  nvs_get_str_ns("sboard", "remote_tok", tok, ts);
}
static void net_request_reconnect() { s_net_reconnect = true; }
static void net_request_wifiscan()  { s_wifi_scan_req = true; }

// Diagnostic: dump every visible AP (SSID exact-quoted, RSSI, encryption, channel). STA-only,
// read-only — NO AP bring-up. Runs ON the net task (the sole WiFi-driver owner) so it can't
// race an in-progress connect. Blocks the net task ~2-5s during the scan; the loop task / UI
// keep running on the other core. Serial-only (never mirrored to the voice/TCP path).
static void wifi_scan_dump() {
  Serial.println("[SB] scan: starting WiFi.scanNetworks()...");
  int n = WiFi.scanNetworks();                  // synchronous; STA mode, no AP
  if (n < 0) { Serial.printf("[SB] scan: failed (%d)\n", n); return; }
  Serial.printf("[SB] scan: found %d networks\n", n);
  for (int i = 0; i < n; i++) {
    const char* enc;
    switch (WiFi.encryptionType(i)) {           // wifi_auth_mode_t
      case WIFI_AUTH_OPEN:            enc = "OPEN";            break;
      case WIFI_AUTH_WEP:             enc = "WEP";             break;
      case WIFI_AUTH_WPA_PSK:         enc = "WPA";             break;
      case WIFI_AUTH_WPA2_PSK:        enc = "WPA2";            break;
      case WIFI_AUTH_WPA_WPA2_PSK:    enc = "WPA/WPA2";        break;
      case WIFI_AUTH_WPA2_ENTERPRISE: enc = "WPA2-ENT";        break;
      case WIFI_AUTH_WPA3_PSK:        enc = "WPA3";            break;
      case WIFI_AUTH_WPA2_WPA3_PSK:   enc = "WPA2/WPA3";       break;
      default:                        enc = "?";               break;
    }
    // Quote the SSID so trailing spaces / odd characters (curly apostrophes etc.) are visible.
    Serial.printf("[SB] scan: ssid=\"%s\" rssi=%d enc=%s ch=%d\n",
                  WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i), enc, (int)WiFi.channel(i));
  }
  WiFi.scanDelete();                            // free the scan result memory
  Serial.println("[SB] scan: done");
}

// Transport TX: a protocol line to USB Serial always; mirrored to the TCP socket (via
// the tx ring, drained by the net task) when the daemon is connected. Diagnostic
// "[SB] ..." logs are NOT routed here — they stay Serial-only.
static void proto_send(const char* line) {
  Serial.println(line);
  // Suppress the TCP copy while voice owns the socket (live capture OR a spooled upload):
  // a protocol line mid-PCM would corrupt the clip. Serial copy is always safe (voice-over-
  // TCP leaves Serial free; voice-over-serial blocks proto_send upstream via s_recording).
  if (s_tcp_up && !s_recording && !s_upload_active) {
    ring_push(&s_net_tx, (const uint8_t*)line, strlen(line));
    ring_push(&s_net_tx, (const uint8_t*)"\n", 1);
  }
}

// Voice transport: the WAV framing + PCM go here during a recording. The transport is
// locked at rec_start (s_rec_over_tcp): to the tx ring (net task -> socket) over Wi-Fi,
// else USB Serial. During recording this is the ONLY producer to the tx ring (proto_send
// is gated off), so nothing interleaves with the PCM on the socket. Loop task only.
static void voice_out(const uint8_t* p, size_t n) {
  if (s_rec_over_tcp) {
    size_t w = ring_push(&s_net_tx, p, n);          // net task drains to socket
    s_rec_pushed += (uint32_t)w;
    s_rec_txdrop += (uint32_t)(n - w);              // only if the ring is full (socket stalled)
  } else {
    Serial.write(p, n);
    s_rec_pushed += (uint32_t)n;
  }
}
static void voice_out_str(const char* s) { voice_out((const uint8_t*)s, strlen(s)); }

// Spool-mode upload pump (LOOP task, called every loop iteration; no-op unless a spooled
// clip is uploading). Feeds the spool buffer into the tx ring ONE chunk per iteration so
// LVGL stays responsive, then closes the WAV once every byte is on the socket. On a slow
// tunnel this just takes longer — it never drops PCM. Aborts only on a genuinely stuck link
// (no socket-drain progress for SB_UPLOAD_STALL_MS) or the SB_UPLOAD_CAP_MS ceiling.
static void upload_pump() {
  if (!s_upload_active) return;
  uint32_t now = millis();
  // Progress = bytes the net task actually pushed to the socket (s_rec_drained). Any growth
  // resets the stall timer; a slow trickle keeps it alive indefinitely (up to the cap).
  uint32_t drained = s_rec_drained;
  if (drained != s_upload_last_drained) { s_upload_last_drained = drained; s_upload_progress_ms = now; }

  if ((now - s_upload_start_ms) > SB_UPLOAD_CAP_MS ||
      (now - s_upload_progress_ms) > SB_UPLOAD_STALL_MS) {
    Serial.printf("[SB] rec upload aborted: off=%u/%u drained=%u\n",
                  (unsigned)s_upload_off, (unsigned)s_upload_total, (unsigned)s_rec_drained);
    s_upload_active = false; s_rec_over_tcp = false;
    show_voice_fail("upload stalled", COL_SB_GOLD);
    return;
  }

  if (s_upload_off < s_upload_total) {
    // Push as much of the next chunk as the tx ring will take right now (partial is fine —
    // we advance only by what was accepted and finish the rest next iteration).
    size_t remain = s_upload_total - s_upload_off;
    size_t chunk  = remain < SB_REC_DRAIN_CAP ? remain : SB_REC_DRAIN_CAP;
    size_t w = ring_push(&s_net_tx, s_ring + s_upload_off, chunk);
    s_upload_off += w;
    s_rec_pushed += (uint32_t)w;
    return;                                        // one chunk/iter -> UI never freezes
  }

  // Whole body queued -> wait for the ring to fully drain to the socket, then close the WAV.
  if (ring_used(&s_net_tx) > 0) return;
  voice_out_str("\n-- WAV_END --\n");
  char j[48];
  snprintf(j, sizeof(j), "{\"t\":\"rec\",\"state\":\"stop\",\"ms\":%u}\n", (unsigned)s_last_rec_dur);
  voice_out_str(j);
  s_upload_active = false; s_rec_over_tcp = false;   // upload done -> normal transport routing resumes
  Serial.printf("[SB] rec upload done: %u bytes over tunnel\n", (unsigned)s_upload_total);
}

// Transport RX: drain socket bytes the net task queued and run whole lines through the
// SAME process_line() as USB. Own line-assembly state (never interleaves with the
// serial splitter). LOOP task only.
static void tcp_rx_poll() {
  static uint8_t chunk[512];
  static char    line[2048];
  static int     len = 0;
  static bool    overflow = false;
  size_t got;
  while ((got = ring_pop(&s_net_rx, chunk, sizeof(chunk))) > 0) {
    for (size_t k = 0; k < got; k++) {
      char c = (char)chunk[k];
      if (c == '\n' || c == '\r') {
        if (len > 0 && !overflow) { line[len] = 0; if (line[0] == '{') { s_rx_from_serial = false; process_line(line); } }
        len = 0; overflow = false;
      } else if (!overflow) {
        if (len < (int)sizeof(line) - 1) line[len++] = c; else overflow = true;
      }
    }
  }
}

// Post-connect setup shared by the LAN and remote dials: bound blocking, flush the
// rings, then queue the AUTH line as the very FIRST bytes (before s_tcp_up lets the loop
// task's proto_send/voice_out write anything), so the daemon sees auth first on every
// connection. Empty token (first-ever LAN boot) is fine — daemon grace-provisions it.
static void net_open_socket(bool remote) {
  s_client.setNoDelay(true);
  s_client.setConnectionTimeout(200);
  ring_reset(&s_net_tx); ring_reset(&s_net_rx);
  char tok[96]; net_load_token(tok, sizeof(tok));
  char auth[160];
  int an = snprintf(auth, sizeof(auth), "{\"t\":\"auth\",\"tok\":\"%s\"}\n", tok);
  if (an > 0 && an < (int)sizeof(auth)) ring_push(&s_net_tx, (const uint8_t*)auth, (size_t)an);
  s_conn_remote = remote;
  s_tcp_up = true;
  if (!s_recording) {
    Serial.println(remote ? "[SB] remote: connected" : "[SB] tcp: connected");
    Serial.println(remote ? "[SB] remote: auth sent" : "[SB] tcp: auth sent");
  }
}

// The net task: join Wi-Fi (creds -> WiFi.begin, retry w/ backoff), listen for the daemon
// UDP beacon and dial that LAN address; if no beacon within ~8s (or after a drop) and a
// remote (ngrok) is configured, fall back to dialing it directly. Pump RX/TX until the
// socket drops, then repeat. Reconnect on a creds change.
static void net_task(void* arg) {
  (void)arg;
  char ssid[48], pwd[64];
  unsigned long next_wifi_try = 0;
  int wifi_fails = 0;                            // consecutive connect failures
  unsigned long last_tx_progress = 0;           // last time a socket write moved bytes (dead-link watchdog)
  unsigned long last_beacon_ms = 0;             // last LAN beacon / Wi-Fi join (starts the remote-fallback window)
  unsigned long last_remote_try = 0;            // last remote dial attempt (retry backoff)

  for (;;) {
    // Diagnostic scan request (serviced here so it never collides with an in-flight WiFi.begin;
    // during the fail-retry backoff this top-of-loop runs every ~50ms so it's near-immediate).
    if (s_wifi_scan_req) { s_wifi_scan_req = false; wifi_scan_dump(); }

    if (s_net_reconnect) {                       // new creds arrived over serial
      s_net_reconnect = false;
      if (s_client.connected()) s_client.stop();
      s_tcp_up = false;
      WiFi.disconnect(true, false);
      s_wifi_up = false;
      next_wifi_try = 0; wifi_fails = 0;
    }

    // 1) Ensure Wi-Fi is up.
    if (WiFi.status() != WL_CONNECTED) {
      if (s_wifi_up || s_tcp_up) {               // lost association -> drop socket too
        if (s_client.connected()) s_client.stop();
        s_wifi_up = false; s_tcp_up = false;
      }
      if (millis() >= next_wifi_try) {
        if (net_load_creds(ssid, sizeof(ssid), pwd, sizeof(pwd))) {
          s_have_creds = true;
          if (!s_recording) Serial.printf("[SB] wifi: connecting to \"%s\"\n", ssid);
          WiFi.mode(WIFI_STA);
          WiFi.setSleep(false);
          WiFi.setTxPower(WIFI_POWER_19_5dBm);   // max TX — help the marginal uplink under sustained voice upload
          WiFi.begin(ssid, pwd);
          unsigned long t0 = millis();
          while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 20000)
            vTaskDelay(pdMS_TO_TICKS(200));
          if (WiFi.status() == WL_CONNECTED) {
            s_wifi_up = true; wifi_fails = 0;
            if (!s_recording) { Serial.print("[SB] wifi: up, ip="); Serial.println(WiFi.localIP()); }
            s_udp.begin(SB_UDP_BEACON_PORT);
            last_beacon_ms = millis();             // start the "no beacon -> remote" window from join
          } else {
            // Back off HARD after 3 fails (driver "create wifi task" spam) -> 60s, muted.
            wifi_fails++;
            unsigned long wait = (wifi_fails <= 3) ? 3000UL : 60000UL;
            next_wifi_try = millis() + wait;
            if (!s_recording) {
              if (wifi_fails <= 3)      Serial.printf("[SB] wifi: connect failed (%d/3), retrying\n", wifi_fails);
              else if (wifi_fails == 4) Serial.println("[SB] wifi: still failing -> 60s backoff (log muted)");
            }
          }
        } else {
          s_have_creds = false;
          next_wifi_try = millis() + 5000;       // no creds: recheck (serial cmd may add them)
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    s_wifi_up = true;

    // 2) No socket yet. First try the LAN beacon; if none within the window, fall back to
    //    the remote (ngrok) dial — while still listening for the beacon in case LAN returns.
    if (!s_client.connected()) {
      if (s_tcp_up) {                            // just dropped -> restart the LAN-preference window
        s_tcp_up = false; s_conn_remote = false;
        last_beacon_ms = millis();
      }
      // (a) LAN beacon.
      int psize = s_udp.parsePacket();
      if (psize > 0) {
        char pkt[64];
        int n = s_udp.read((uint8_t*)pkt, sizeof(pkt) - 1); if (n < 0) n = 0; pkt[n] = 0;
        int port = 0;
        if (!strncmp(pkt, "SBHALO1 ", 8)) port = atoi(pkt + 8);
        if (port > 0) {
          last_beacon_ms = millis();             // beacon heard -> reset the fallback window
          IPAddress ip = s_udp.remoteIP();
          if (!s_recording) Serial.printf("[SB] beacon %s:%d\n", ip.toString().c_str(), port);
          if (s_client.connect(ip, (uint16_t)port)) { net_open_socket(false); last_tx_progress = millis(); }
        }
      }
      // (b) No beacon within the window -> remote fallback (retry w/ backoff).
      else if ((millis() - last_beacon_ms) > SB_BEACON_WAIT_MS &&
               (millis() - last_remote_try)  > SB_BEACON_WAIT_MS) {
        char rhost[64], rtok[96]; int rport = 0;
        if (net_load_remote(rhost, sizeof(rhost), &rport, rtok, sizeof(rtok))) {
          last_remote_try = millis();
          if (!s_recording) Serial.printf("[SB] wifi: no beacon, trying remote %s:%d\n", rhost, rport);
          if (s_client.connect(rhost, (uint16_t)rport)) { net_open_socket(true); last_tx_progress = millis(); }
          else if (!s_recording) Serial.println("[SB] remote: dial failed");
        }
      }
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }
    s_tcp_up = true;

    // 3) Connected: pump RX (socket -> rx ring) and TX (tx ring -> socket).
    uint8_t buf[1460];                           // ~1 TCP MSS
    int avail = s_client.available();
    while (avail > 0) {
      int n = s_client.read(buf, sizeof(buf));
      if (n <= 0) break;
      ring_push(&s_net_rx, buf, (size_t)n);      // drop-on-full: corrupt frame fails CRC -> daemon resends
      avail -= n;
    }
    // TX drain via raw non-blocking lwip_send(MSG_DONTWAIT). WiFiClient::write can block
    // up to ~10s (10 retries x a 1s select) on a stalled reader — fatal for net_task, the
    // sole delivery path — and availableForWrite() is a no-op stub (always 0) on this core.
    // lwip_send returns immediately: >0 = bytes accepted, <=0 = buffer full/err -> retry
    // next 5ms cycle. We PEEK, send, and advance the ring by the ACTUALLY-sent count so no
    // bytes are ever lost. If nothing moves for 2s while data waits, the link is dead -> drop.
    int sfd = s_client.fd();
    bool progressed = false;
    for (int iter = 0; iter < 64 && sfd >= 0; iter++) {
      size_t got = ring_peek(&s_net_tx, buf, sizeof(buf));
      if (got == 0) break;
      int w = lwip_send(sfd, buf, got, MSG_DONTWAIT);
      if (s_upload_active) {                      // send-path telemetry (offered vs accepted)
        s_up_calls++; s_up_offered += (uint32_t)got;
        if (w <= 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) s_up_eagain++; else s_up_err++; }
      }
      if (w <= 0) break;                         // EAGAIN (buffer full) or error -> next cycle
      ring_advance(&s_net_tx, (size_t)w);
      s_rec_drained += (uint32_t)w;
      progressed = true;
      if ((size_t)w < got) break;                // took only part -> socket buffer now full
    }
    if (progressed) {
      last_tx_progress = millis();
    } else {
      // A spooled upload over a high-RTT tunnel legitimately pauses >2s between drains while
      // the send buffer waits on ACKs — that's backpressure, not death. Relax the cutoff during
      // an upload (upload_pump's 20s is the user-facing decision; this 25s is just a backstop),
      // and keep the tight 2s for normal live traffic.
      unsigned long stall_ms = s_upload_active ? 25000UL : 2000UL;
      if (ring_used(&s_net_tx) > 0 && (millis() - last_tx_progress) > stall_ms) {
        if (!s_recording && !s_upload_active) Serial.println("[SB] tcp: write stalled -> dropping link");
        s_client.stop();                         // leave s_tcp_up set: state 2 detects the drop + resets window
        continue;                                // fall back to beacon/remote reconnect
      }
    }

    // Per-second send-side telemetry during a spool upload. Serial is free here (voice is on
    // TCP, capture is done), so this doesn't touch the PCM. Shows whether the board is offering
    // full-MSS sends the socket rejects (EAGAIN -> TCP/tunnel-limited) vs RSSI/error weakness.
    if (s_upload_active && (millis() - s_up_log_ms) >= 1000) {
      uint32_t d = s_rec_drained;
      Serial.printf("[SB] up: %uB/s off=%u/%u calls=%u eagain=%u err=%u avgoffer=%u rssi=%d\n",
                    (unsigned)(d - s_up_log_drained), (unsigned)s_upload_off, (unsigned)s_upload_total,
                    (unsigned)s_up_calls, (unsigned)s_up_eagain, (unsigned)s_up_err,
                    (unsigned)(s_up_calls ? s_up_offered / s_up_calls : 0), (int)WiFi.RSSI());
      s_up_log_drained = d; s_up_log_ms = millis();
      s_up_calls = s_up_eagain = s_up_err = s_up_offered = 0;
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void net_start() {
  if (!ring_init(&s_net_tx, SB_NET_TX_SZ) || !ring_init(&s_net_rx, SB_NET_RX_SZ)) {
    Serial.println("[SB] net: ring alloc failed -> USB-only");
    return;
  }
  xTaskCreatePinnedToCore(net_task, "sb_net", 8192, NULL, 2, NULL, 0);   // Core 0, like the audio task
}

static void log_heap(const char* tag) {
  Serial.printf("[SB] heap %s: internal free=%u largest=%u psram free=%u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// ===================== setup / loop =====================
void setup() {
  Serial.setRxBufferSize(16384);      // must precede begin(); frame = 12x(s+x) ~ up to 24KB
  Serial.begin(115200);
  delay(800);
  Serial.println("\n[BOOT] Session_Board starting");

  // Big session arrays -> PSRAM, so the WiFi driver has enough INTERNAL DRAM.
  live           = (sb_session_t*)heap_caps_malloc(SB_SESS_ARR_BYTES, MALLOC_CAP_SPIRAM);
  shadow         = (sb_session_t*)heap_caps_malloc(SB_SESS_ARR_BYTES, MALLOC_CAP_SPIRAM);
  s_pending_order = (sb_session_t*)heap_caps_malloc(SB_SESS_ARR_BYTES, MALLOC_CAP_SPIRAM);
  if (!live || !shadow || !s_pending_order) {
    Serial.println("[SB] FATAL: session buffer PSRAM alloc failed");
    while (1) delay(1000);
  }
  memset(live, 0, SB_SESS_ARR_BYTES);
  memset(shadow, 0, SB_SESS_ARR_BYTES);
  memset(s_pending_order, 0, SB_SESS_ARR_BYTES);

  log_heap("boot");

  // Claim the WiFi driver's INTERNAL RAM EARLY — before LVGL/audio grab their buffers —
  // so esp_wifi_init can create its task. Only when creds exist (no creds -> USB-only,
  // no WiFi RAM cost). The net task still owns begin()/connect lifecycle afterward.
  {
    char ss[48], pw[64];
    if (net_load_creds(ss, sizeof(ss), pw, sizeof(pw))) {
      Serial.println("[BOOT] wifi pre-init (claiming driver RAM early)");
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.setTxPower(WIFI_POWER_19_5dBm);   // max TX power (persist from first init)
      log_heap("post-wifi-init");
    }
  }

  Serial.println("[BOOT] Touch_Init");
  Touch_Init();                 // installs I2C_NUM_0 (shared with haptic)

  Serial.println("[BOOT] haptic_init");
  haptic_init();                // no-op flag if DRV2605 absent

  Serial.println("[BOOT] lcd_lvgl_Init");
  lcd_lvgl_Init();              // rotation 180 handled via EXAMPLE_Rotate_180 in lcd_config.h

  lv_timer_set_period(lv_disp_get_default()->refr_timer, 16);   // ~60fps

  Serial.println("[BOOT] backlight");
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  setUpdutySubdivide(BL_DUTY_PCT);

  Serial.println("[BOOT] knob");
  init_knob();

  Serial.println("[BOOT] audio");
  s_audio_ok = audio_bsp_init();                 // PDM mic + DAC (graceful on failure)
  if (s_audio_ok) {
    // Spool buffer: try the target, halving on failure so a smaller-PSRAM board still works
    // (just a shorter max clip). s_spool_cap = what we actually got; the clip cap derives from it.
    size_t want = SB_SPOOL_TARGET;
    while (want >= (256 * 1024)) {
      s_ring = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
      if (s_ring) { s_spool_cap = want; break; }
      want /= 2;
    }
    if (!s_ring) { s_audio_ok = false; Serial.println("[SB] mic spool alloc failed"); }
    else {
      audio_set_record_callback(rec_audio_cb);
      volume_adjustment(0);                       // mute the constant mic->DAC monitor passthrough
      Serial.printf("[BOOT] audio ready (spool=%uKB, ~%us max clip @16k)\n",
                    (unsigned)(s_spool_cap / 1024), (unsigned)(s_spool_cap / 32000));
    }
  } else {
    Serial.println("[SB] mic init failed");
  }

  Serial.println("[BOOT] net");
  net_start();                                   // WiFi/TCP task on Core 0 (USB-only if this no-ops)
  {
    char ss[48], pw[64];
    if (net_load_creds(ss, sizeof(ss), pw, sizeof(pw))) {
      s_have_creds = true;
      Serial.println("[SB] wifi: creds found, joining in background");
    } else {
      s_have_creds = false;
      Serial.println("[SB] wifi: no creds");     // works USB-only; ask the daemon for a network
      Serial.println("{\"t\":\"wificreds\"}");
    }
  }

  build_wait_screen();
  build_board_screen();

  s_stale_timer = lv_timer_create(stale_timer_cb, 1000, NULL);   // repeating

#if SB_FAKE_DATA
  load_fake_data();
  s_state = ST_BOARD;
  s_detail_open = false;
  s_sel   = 0;
  lv_scr_load(board_screen);
  build_view();
  rim_for_state();
  if (wait_screen) { lv_obj_del(wait_screen); wait_screen = NULL; }
  Serial.println("[NAV] -> BOARD (fake data)");
#else
  lv_scr_load(wait_screen);
  s_state = ST_WAIT;
  Serial.println("[NAV] -> WAIT");
#endif

  Serial.println("[BOOT] ready");
}

void loop() {
  serial_poll();          // drain USB serial before LVGL runs (may start/stop recording)
  tcp_rx_poll();          // drain TCP-delivered protocol lines (same process_line path)
  if (s_recording) {
    if (!s_spool_mode) rec_drain(SB_REC_DRAIN_CAP);   // LIVE: stream PCM during capture (spool doesn't)
    // Stop on: driver auto-stop, spool buffer full (spool mode), or the hard ceiling.
    if (!audio_is_recording() || s_spool_full || (millis() - s_rec_start_ms) > SB_REC_MAX_MS) rec_stop();
  }
  upload_pump();          // SPOOL: drain the recorded clip to the socket after capture (no-op otherwise)
  wifi_entry_poll();      // update the on-device Wi-Fi setup connect status, if active
  lv_timer_handler();     // LVGL long-press/release fire here -> rec_start/rec_stop
  process_knob();
  delay(5);
}

// lcd_demo.h — LCD-ONLY offline demo unit ("virtual Sense")
//
// PURPOSE
//   Run the entire HALO demo on the LCD board alone. No Sense board, no UART
//   peer, no WiFi, no cloud. Every user flow completes with the REAL production
//   UI and the shopping list is preloaded.
//
// HOW IT WORKS
//   In production the LCD is a slave to the Sense's UI_STATUS phase machine:
//   the user taps, the LCD sends an INPUT_* message, and the Sense answers with
//   UI_STATUS(op/phase/mode/text/job_id) which drives the screens.
//
//   This module is a local stand-in for that peer. It intercepts the outbound
//   user actions and injects the exact messages the Sense would have replied
//   with, straight into uart_process_received_message() — the same function the
//   real UART RX path calls. So the UI, routing, animations and timing code are
//   IDENTICAL to production; nothing downstream knows the demo exists.
//
//     tap "Dish"  ->  intercept INPUT_MENU_SELECT
//                 ->  inject UI_STATUS CAPTURING   -> HOLD_STILL screen
//                 ->  inject UI_STATUS DONE        -> LOGGED screen
//
//   Phase sequences and text are transcribed from the production Sense
//   (Sense_Minimal.ino op_worker_task OP_SCAN, flow_plan_print in sense_scan.h).
//
// WHY INTERCEPT RATHER THAN LET THE MESSAGES GO OUT
//   With no Sense attached, INPUT_MENU_SELECT never even leaves: it requires
//   "awake proof" from the peer (tx_msg_requires_awake_proof) and would sit in
//   the deferred-awake ring forever. Likewise a list refresh parks in
//   REFRESH_WAKE_PENDING. Intercepting upstream of the TX queue avoids all of
//   that, and makes the board immune to a Sense being physically present.
//
// THREADING
//   lcd_demo_pump() is called from uart_task on Core 0 — the same task and core
//   that normally parses inbound UART lines. Injection therefore happens in
//   exactly the production context, which matters because
//   uart_process_received_message() is documented as Core-0-only and must not
//   touch LVGL (it only writes g_pending and posts to the UI queue).
//   NEVER call the pump or the inject helpers from the UI task or loop().
//
// SCOPE
//   Entirely compiled out when HALO_DEMO_MODE == 0.
//
// ORDERING
//   Include AFTER lcd_uart_rx.h (needs uart_process_received_message) and
//   BEFORE lcd_uart_task.h (which calls the pump). The small API used by the
//   hooks in lcd_uart.h / lcd_ship_screens.h is forward-declared in
//   LCD_Minimal.ino.

#ifndef LCD_DEMO_H
#define LCD_DEMO_H

#if HALO_DEMO_MODE

// ── Timing ────────────────────────────────────────────────────────────────────
// Mimics the real device so the demo does not feel instant/fake. Real hardware
// spends ~700-1500ms on camera init + capture.
static const uint32_t LCD_DEMO_CAPTURE_MS     = 900;    // HOLD_STILL dwell
static const uint32_t LCD_DEMO_SETTLE_MS      = 250;    // beat before LOGGED
static const uint32_t LCD_DEMO_VOICE_THINK_MS = 1100;   // "On it" -> DONE
static const uint32_t LCD_DEMO_INPUT_WAIT_MS  = 30000;  // matches production
static const uint32_t LCD_DEMO_HB_MS          = 3000;   // keep the link "alive"

// ── Preloaded shopping list ───────────────────────────────────────────────────
// `id` must be unique and non-empty: the LCD keys row identity and its delete
// overlay on it. Keep text under 64 chars and store under 48.
struct lcd_demo_item_t {
  const char* id;
  const char* text;
  const char* store;
};

static lcd_demo_item_t g_lcd_demo_items[] = {
  { "demo-001", "Bananas",         "Whole Foods" },
  { "demo-002", "Whole milk",      "Whole Foods" },
  { "demo-003", "Large eggs",      "Whole Foods" },
  { "demo-004", "Sourdough bread", "Whole Foods" },
  { "demo-005", "Baby spinach",    "Trader Joe's" },
  { "demo-006", "Roma tomatoes",   "Trader Joe's" },
  { "demo-007", "Chicken thighs",  "Trader Joe's" },
  { "demo-008", "Greek yogurt",    "Trader Joe's" },
  { "demo-009", "Sharp cheddar",   "" },
  { "demo-010", "Olive oil",       "" },
  { "demo-011", "Ground coffee",   "" },
  { "demo-012", "Avocados",        "" },
};
static const int kLcdDemoItemCountInitial =
    (int)(sizeof(g_lcd_demo_items) / sizeof(g_lcd_demo_items[0]));
// Live count — deletes shrink it. Reset by a list refresh.
static int g_lcd_demo_item_count = kLcdDemoItemCountInitial;

// ── Script state ──────────────────────────────────────────────────────────────
enum lcd_demo_state_t {
  LCD_DEMO_IDLE = 0,
  LCD_DEMO_SCAN_CAPTURING,   // HOLD_STILL shown; waiting out the capture dwell
  LCD_DEMO_SCAN_PROMPT,      // WAITING_INPUT shown; waiting for the user's tap
  LCD_DEMO_SCAN_SETTLE,      // user answered; brief beat before LOGGED
  LCD_DEMO_VOICE_THINKING,   // "On it" shown; waiting before DONE
};

static lcd_demo_state_t g_lcd_demo_state = LCD_DEMO_IDLE;
static uint32_t g_lcd_demo_next_ms   = 0;   // when the current step expires
static uint32_t g_lcd_demo_job_id    = 0;
static char     g_lcd_demo_mode[16]  = {0};
static bool     g_lcd_demo_reply_seen = false;
static uint32_t g_lcd_demo_last_hb_ms = 0;
static uint32_t g_lcd_demo_job_seq    = 0;

// ── Mode classification (mirrors sense_scan.h) ────────────────────────────────
static bool lcd_demo_mode_is_check(const char* m) {
  return m && (strcmp(m, "check-in") == 0 ||
               strcmp(m, "check-out") == 0 ||
               strcmp(m, "check_out") == 0);
}
static bool lcd_demo_mode_is_discard(const char* m) {
  return m && strcmp(m, "discard") == 0;
}

// Map a menu item label to the production `mode` string.
// Mirrors ship_menu_mode_for_item() in lcd_ship_action.h.
static const char* lcd_demo_mode_for_menu_item(const char* menu_item) {
  if (!menu_item || !menu_item[0]) return NULL;
  if (strcmp(menu_item, "Dish") == 0)      return "dish";
  if (strcmp(menu_item, "Check-in") == 0)  return "check-in";
  if (strcmp(menu_item, "Check-out") == 0) return "check-out";
  if (strcmp(menu_item, "Discard") == 0)   return "discard";
  return NULL;
}

// ── Injection ─────────────────────────────────────────────────────────────────
// True only while this module is feeding its own message in, so the inbound
// filter can tell our synthetic traffic from a real peer's.
static bool g_lcd_demo_injecting = false;

// Build the JSON the Sense would have sent and feed it to the real RX handler.
static void lcd_demo_inject(const char* json) {
  Serial.printf("[LCD_DEMO] inject: %s\n", json);
  g_lcd_demo_injecting = true;
  uart_process_received_message(json);
  g_lcd_demo_injecting = false;
}

// Inbound filter. The demo is meant to be self-contained, but a Sense may still
// be sitting in the enclosure running production firmware. Left alone it would
// fight the demo — worst case an unprovisioned Sense pushes PROVISION_QR and
// parks the display on the Wi-Fi QR screen, killing the demo outright. So in
// demo mode this board is authoritative: drop the message families the demo
// owns unless we are the one injecting them.
// Returns true to discard the message.
static bool lcd_demo_should_drop_rx(const char* type) {
  if (g_lcd_demo_injecting) return false;   // our own traffic, always allowed
  if (!type || !type[0]) return false;
  if (strcmp(type, "PROVISION_QR") == 0 ||
      strcmp(type, "PROVISION_STATUS") == 0 ||
      strcmp(type, "UI_LIST") == 0 ||
      strcmp(type, "UI_STATUS") == 0 ||
      strcmp(type, "UI_VOICE_RESPONSE") == 0 ||
      strcmp(type, "UI_VOICE_ITEMS") == 0 ||
      strcmp(type, "UI_TOAST") == 0) {
    Serial.printf("[LCD_DEMO] dropped peer message type=%s (demo owns the UI)\n",
                  type);
    return true;
  }
  return false;
}

static void lcd_demo_inject_ui_status(const char* op, const char* phase,
                                      const char* text, const char* mode,
                                      uint32_t job_id) {
  StaticJsonDocument<320> doc;
  doc["ver"]    = PROTOCOL_VERSION;
  doc["type"]   = "UI_STATUS";
  doc["msg_id"] = get_next_msg_id();   // fresh id, or the LCD dedups it away
  doc["ts"]     = millis();
  if (op && op[0])       doc["op"]    = op;
  if (phase && phase[0]) doc["phase"] = phase;
  doc["text"] = text ? text : "";
  if (mode && mode[0])   doc["mode"]  = mode;
  if (job_id)            doc["job_id"] = job_id;
  String out;
  serializeJson(doc, out);
  lcd_demo_inject(out.c_str());
}

// Serve the preloaded list. Mirrors uart_send_ui_list() on the Sense: the
// leading UI_STATUS "IDLE" is the awake-proof that releases the LCD's refresh
// state machine from REFRESH_WAKE_PENDING — without it the spinner can hang.
static void lcd_demo_inject_ui_list() {
  {
    StaticJsonDocument<192> s;
    s["ver"] = PROTOCOL_VERSION;
    s["type"] = "UI_STATUS";
    s["msg_id"] = get_next_msg_id();
    s["ts"] = millis();
    s["text"] = "IDLE";
    String out;
    serializeJson(s, out);
    lcd_demo_inject(out.c_str());
  }

  DynamicJsonDocument doc(4096);   // 12 short items — comfortably inside 4KB
  doc["ver"]            = PROTOCOL_VERSION;
  doc["type"]           = "UI_LIST";
  doc["msg_id"]         = get_next_msg_id();
  doc["ts"]             = millis();
  doc["selected_index"] = -1;
  JsonArray items = doc.createNestedArray("items");
  for (int i = 0; i < g_lcd_demo_item_count; i++) {
    JsonObject it = items.createNestedObject();
    if (it.isNull()) {
      Serial.printf("[LCD_DEMO] UI_LIST truncated at %d\n", i);
      break;
    }
    it["id"]    = g_lcd_demo_items[i].id;
    it["text"]  = g_lcd_demo_items[i].text;
    it["store"] = g_lcd_demo_items[i].store;
  }
  String out;
  serializeJson(doc, out);
  if (doc.overflowed()) {
    Serial.println("[LCD_DEMO] WARN UI_LIST doc overflowed");
  }
  lcd_demo_inject(out.c_str());
}

// ── Local list delete ─────────────────────────────────────────────────────────
static void lcd_demo_delete_item(const char* id) {
  if (!id || !id[0]) {
    Serial.println("[LCD_DEMO] delete ignored (empty id)");
    return;
  }
  int found = -1;
  for (int i = 0; i < g_lcd_demo_item_count; i++) {
    if (strcmp(g_lcd_demo_items[i].id, id) == 0) { found = i; break; }
  }
  if (found >= 0) {
    for (int i = found; i < g_lcd_demo_item_count - 1; i++) {
      g_lcd_demo_items[i] = g_lcd_demo_items[i + 1];
    }
    g_lcd_demo_item_count--;
    Serial.printf("[LCD_DEMO] deleted id=%s idx=%d remaining=%d\n",
                  id, found, g_lcd_demo_item_count);
  } else {
    Serial.printf("[LCD_DEMO] delete id=%s not found\n", id);
  }
  lcd_demo_inject_ui_list();   // repaint either way, so the UI cannot desync
}

// Restore the full table (a refresh re-fetches in production).
static void lcd_demo_reset_list(const char* reason) {
  static const lcd_demo_item_t kPristine[] = {
    { "demo-001", "Bananas",         "Whole Foods" },
    { "demo-002", "Whole milk",      "Whole Foods" },
    { "demo-003", "Large eggs",      "Whole Foods" },
    { "demo-004", "Sourdough bread", "Whole Foods" },
    { "demo-005", "Baby spinach",    "Trader Joe's" },
    { "demo-006", "Roma tomatoes",   "Trader Joe's" },
    { "demo-007", "Chicken thighs",  "Trader Joe's" },
    { "demo-008", "Greek yogurt",    "Trader Joe's" },
    { "demo-009", "Sharp cheddar",   "" },
    { "demo-010", "Olive oil",       "" },
    { "demo-011", "Ground coffee",   "" },
    { "demo-012", "Avocados",        "" },
  };
  for (int i = 0; i < kLcdDemoItemCountInitial; i++) {
    g_lcd_demo_items[i] = kPristine[i];
  }
  g_lcd_demo_item_count = kLcdDemoItemCountInitial;
  Serial.printf("[LCD_DEMO] list reset count=%d reason=%s\n",
                g_lcd_demo_item_count, reason ? reason : "?");
}

// ── Flow starters ─────────────────────────────────────────────────────────────
static void lcd_demo_start_scan(const char* menu_item) {
  const char* mode = lcd_demo_mode_for_menu_item(menu_item);
  if (!mode) {
    Serial.printf("[LCD_DEMO] menu_item '%s' is not a scan action — ignored\n",
                  menu_item ? menu_item : "");
    return;
  }
  g_lcd_demo_job_id = ++g_lcd_demo_job_seq;   // non-zero: 0 means "local only"
  strncpy(g_lcd_demo_mode, mode, sizeof(g_lcd_demo_mode) - 1);
  g_lcd_demo_mode[sizeof(g_lcd_demo_mode) - 1] = '\0';
  g_lcd_demo_reply_seen = false;
  Serial.printf("[LCD_DEMO] scan start mode=%s job_id=%lu\n",
                g_lcd_demo_mode, (unsigned long)g_lcd_demo_job_id);
  lcd_demo_inject_ui_status("SCAN", "CAPTURING", "Capturing image…",
                            g_lcd_demo_mode, g_lcd_demo_job_id);
  g_lcd_demo_state   = LCD_DEMO_SCAN_CAPTURING;
  g_lcd_demo_next_ms = millis() + LCD_DEMO_CAPTURE_MS;
}

static void lcd_demo_start_voice() {
  g_lcd_demo_job_id = ++g_lcd_demo_job_seq;
  Serial.printf("[LCD_DEMO] voice start job_id=%lu\n",
                (unsigned long)g_lcd_demo_job_id);
  // The LCD shows "On it" itself on long-press-end (fire-and-forget) — all we
  // owe it is a terminal DONE so it clears its own flags. Never ERROR: the LCD
  // *does* surface a voice ERROR even in fire-and-forget mode.
  g_lcd_demo_state   = LCD_DEMO_VOICE_THINKING;
  g_lcd_demo_next_ms = millis() + LCD_DEMO_VOICE_THINK_MS;
}

// ── TX interception ───────────────────────────────────────────────────────────
// CRITICAL THREADING RULE
//   These hooks are called from WHICHEVER task performed the user action:
//     * physical touch  -> the UI TASK (menu taps, delete, voice long-press all
//                          reach uart_tx_enqueue from lcd_ui_task.h)
//     * USB commands    -> uart_task, Core 0
//   They therefore must NOT inject. uart_process_received_message() is
//   Core-0-only and posts to app_event_queue, which the UI task is itself the
//   consumer of — injecting from the UI task makes it its own producer and
//   consumer. The first build did inject inline here; every USB-driven test
//   passed (uart_task = Core 0) while real finger taps eventually froze the
//   board, because those came in on the UI task. Found on hardware 2026-09-04.
//
//   So the hooks only LATCH INTENT. lcd_demo_pump() consumes these on Core 0 in
//   uart_task — the same task and core a real Sense reply would arrive on. The
//   pump runs every ~5ms, so the added latency is imperceptible.
static volatile bool g_lcd_demo_req_scan   = false;
static char          g_lcd_demo_req_item[24] = {0};
static volatile bool g_lcd_demo_req_voice  = false;
static volatile bool g_lcd_demo_req_delete = false;
static char          g_lcd_demo_req_del_id[64] = {0};
static volatile bool g_lcd_demo_req_reply  = false;

// Called from uart_tx_enqueue(). Return true to swallow the message (the demo
// handles it locally); false to let it through unchanged.
static bool lcd_demo_intercept_tx(const char* type, const char* id) {
  if (!type || !type[0]) return false;

  if (strcmp(type, "INPUT_MENU_SELECT") == 0) {
    strncpy(g_lcd_demo_req_item, id ? id : "", sizeof(g_lcd_demo_req_item) - 1);
    g_lcd_demo_req_item[sizeof(g_lcd_demo_req_item) - 1] = '\0';
    g_lcd_demo_req_scan = true;
    return true;
  }
  if (strcmp(type, "INPUT_LONG_PRESS_END") == 0) {
    g_lcd_demo_req_voice = true;
    return true;
  }
  if (strcmp(type, "INPUT_LONG_PRESS_START") == 0) {
    return true;   // nothing to answer; recording is imaginary
  }
  if (strcmp(type, "INPUT_DELETE") == 0) {
    strncpy(g_lcd_demo_req_del_id, id ? id : "", sizeof(g_lcd_demo_req_del_id) - 1);
    g_lcd_demo_req_del_id[sizeof(g_lcd_demo_req_del_id) - 1] = '\0';
    g_lcd_demo_req_delete = true;
    return true;
  }
  // Everything else (pings, scroll, sleep coordination, diagnostics) is
  // harmless with no peer attached — let it go out and be ignored.
  return false;
}

// Called from uart_send_json(). This is the SECOND of the board's two
// non-equivalent TX paths and it must be hooked as well as uart_tx_enqueue():
//
//   * touch/UI actions      -> uart_tx_enqueue()  -> lcd_demo_intercept_tx()
//   * expiry/discard replies-> uart_send_json()   -> here (seven call sites,
//                                                    all funnel through this)
//   * USB-injected commands -> uart_send_json()   -> here. The USB handler in
//     lcd_uart_task.h builds INPUT_MENU_SELECT itself and calls uart_send_json
//     directly, bypassing the queue entirely — so without the menu-select case
//     below, `{"type":"INPUT_MENU_SELECT",...}` over USB would do nothing.
//     That also makes every flow drivable from the bench without the actuator.
//
// Return true to swallow.
static bool lcd_demo_intercept_json(const char* json) {
  if (!json || !json[0]) return false;
  // Cheap substring prefilter — this runs on every outbound JSON line.
  bool is_expiry  = (strstr(json, "\"INPUT_EXPIRY_DATE\"") != NULL);
  bool is_discard = (strstr(json, "\"INPUT_DISCARD_OPTIONS\"") != NULL);
  bool is_menu    = (strstr(json, "\"INPUT_MENU_SELECT\"") != NULL);
  bool is_voice   = (strstr(json, "\"INPUT_LONG_PRESS_END\"") != NULL);
  if (!is_expiry && !is_discard && !is_menu && !is_voice) return false;

  // Latch only — same Core-0 rule as lcd_demo_intercept_tx().
  if (is_menu) {
    // Parse out menu_item rather than guessing from the raw string.
    StaticJsonDocument<384> d;
    if (deserializeJson(d, json) == DeserializationError::Ok) {
      const char* item = d["menu_item"] | "";
      Serial.printf("[LCD_DEMO] menu select via uart_send_json item=%s\n", item);
      strncpy(g_lcd_demo_req_item, item, sizeof(g_lcd_demo_req_item) - 1);
      g_lcd_demo_req_item[sizeof(g_lcd_demo_req_item) - 1] = '\0';
      g_lcd_demo_req_scan = true;
    } else {
      Serial.println("[LCD_DEMO] menu select JSON unparseable — ignored");
    }
    return true;
  }
  if (is_voice) {
    g_lcd_demo_req_voice = true;
    return true;
  }
  g_lcd_demo_req_reply = true;
  Serial.printf("[LCD_DEMO] %s prompt answered\n", is_expiry ? "expiry" : "discard");
  return true;
}

// Called from shopping_list_trigger_refresh(). Serves the canned list instead
// of waking a peer that does not exist.
//
// DEFERRED ON PURPOSE. trigger_refresh runs on the UI TASK (screen entry, the
// encoder overscroll gesture, and the LVGL touch-pull RELEASED callback), but
// uart_process_received_message() is documented Core-0-only — it writes
// g_pending and posts to app_event_queue, which the UI task is itself the
// consumer of. Injecting inline from the UI task means the producer and the
// consumer are the same task: a full queue then blocks the only thing that
// could drain it. This originally injected inline, which is exactly the kind of
// threading inversion the header's own rules forbid. So just latch a request
// here and let lcd_demo_pump() serve it from uart_task on Core 0 — the same
// task and core a real UI_LIST would have arrived on.
static volatile bool g_lcd_demo_list_req = false;

static void lcd_demo_serve_list_refresh(const char* reason) {
  Serial.printf("[LCD_DEMO] list refresh requested (%s) — deferred to uart_task\n",
                reason ? reason : "?");
  g_lcd_demo_list_req = true;
}

// ── Pump ──────────────────────────────────────────────────────────────────────
// Drives the script forward. MUST be called from uart_task (Core 0) only.
static void lcd_demo_pump() {
  uint32_t now = millis();

  // ── Consume latched user intents, here on Core 0 ──────────────────────────
  // Everything the TX hooks latched gets injected from THIS task, which is the
  // task and core a real Sense reply would have arrived on. See the threading
  // rule above lcd_demo_intercept_tx().
  if (g_lcd_demo_list_req) {
    g_lcd_demo_list_req = false;
    Serial.println("[LCD_DEMO] serving deferred list refresh (uart_task)");
    lcd_demo_reset_list("refresh");
    lcd_demo_inject_ui_list();
  }
  if (g_lcd_demo_req_delete) {
    g_lcd_demo_req_delete = false;
    lcd_demo_delete_item(g_lcd_demo_req_del_id);
  }
  if (g_lcd_demo_req_reply) {
    g_lcd_demo_req_reply = false;
    if (g_lcd_demo_state == LCD_DEMO_SCAN_PROMPT) {
      g_lcd_demo_reply_seen = true;
    } else {
      Serial.printf("[LCD_DEMO] prompt reply outside a prompt (state=%d) — ignored\n",
                    (int)g_lcd_demo_state);
    }
  }
  if (g_lcd_demo_req_scan) {
    g_lcd_demo_req_scan = false;
    // A new scan supersedes anything in flight — the user tapped a fresh action.
    lcd_demo_start_scan(g_lcd_demo_req_item);
  }
  if (g_lcd_demo_req_voice) {
    g_lcd_demo_req_voice = false;
    lcd_demo_start_voice();
  }

  // Keep the link looking alive so nothing reports the peer as stale. LINK_HB
  // counts as awake proof (sense_rx_type_is_awake_proof) and its handler does
  // nothing but log, so this is the cheapest safe keepalive.
  if (now - g_lcd_demo_last_hb_ms >= LCD_DEMO_HB_MS) {
    g_lcd_demo_last_hb_ms = now;
    StaticJsonDocument<160> doc;
    doc["ver"]    = PROTOCOL_VERSION;
    doc["type"]   = "LINK_HB";
    doc["msg_id"] = get_next_msg_id();
    doc["ts"]     = now;
    String out;
    serializeJson(doc, out);
    uart_process_received_message(out.c_str());   // quiet: no [LCD_DEMO] log spam
  }

  if (g_lcd_demo_state == LCD_DEMO_IDLE) return;

  switch (g_lcd_demo_state) {
    case LCD_DEMO_SCAN_CAPTURING:
      if ((int32_t)(now - g_lcd_demo_next_ms) < 0) break;
      if (lcd_demo_mode_is_check(g_lcd_demo_mode)) {
        lcd_demo_inject_ui_status("SCAN", "WAITING_INPUT",
                                  "Waiting for expiry date...",
                                  g_lcd_demo_mode, g_lcd_demo_job_id);
        g_lcd_demo_state   = LCD_DEMO_SCAN_PROMPT;
        g_lcd_demo_next_ms = now + LCD_DEMO_INPUT_WAIT_MS;
      } else if (lcd_demo_mode_is_discard(g_lcd_demo_mode)) {
        lcd_demo_inject_ui_status("SCAN", "WAITING_INPUT",
                                  "Add to shopping list?",
                                  g_lcd_demo_mode, g_lcd_demo_job_id);
        g_lcd_demo_state   = LCD_DEMO_SCAN_PROMPT;
        g_lcd_demo_next_ms = now + LCD_DEMO_INPUT_WAIT_MS;
      } else {
        // dish: no prompt, straight to LOGGED
        g_lcd_demo_state   = LCD_DEMO_SCAN_SETTLE;
        g_lcd_demo_next_ms = now + LCD_DEMO_SETTLE_MS;
      }
      break;

    case LCD_DEMO_SCAN_PROMPT:
      // Advance as soon as the user answers; otherwise fall through on the same
      // 30s window production uses, so a demo can never wedge on an un-tapped
      // prompt.
      if (g_lcd_demo_reply_seen) {
        g_lcd_demo_state   = LCD_DEMO_SCAN_SETTLE;
        g_lcd_demo_next_ms = now + LCD_DEMO_SETTLE_MS;
      } else if ((int32_t)(now - g_lcd_demo_next_ms) >= 0) {
        Serial.println("[LCD_DEMO] prompt timeout — completing anyway");
        g_lcd_demo_state   = LCD_DEMO_SCAN_SETTLE;
        g_lcd_demo_next_ms = now + LCD_DEMO_SETTLE_MS;
      }
      break;

    case LCD_DEMO_SCAN_SETTLE:
      if ((int32_t)(now - g_lcd_demo_next_ms) < 0) break;
      lcd_demo_inject_ui_status("SCAN", "DONE", "Logged!",
                                g_lcd_demo_mode, g_lcd_demo_job_id);
      Serial.printf("[LCD_DEMO] scan complete mode=%s\n", g_lcd_demo_mode);
      g_lcd_demo_state = LCD_DEMO_IDLE;
      break;

    case LCD_DEMO_VOICE_THINKING:
      if ((int32_t)(now - g_lcd_demo_next_ms) < 0) break;
      lcd_demo_inject_ui_status("VOICE", "DONE", "Got it!", NULL,
                                g_lcd_demo_job_id);
      Serial.println("[LCD_DEMO] voice complete");
      g_lcd_demo_state = LCD_DEMO_IDLE;
      break;

    case LCD_DEMO_IDLE:
    default:
      break;
  }
}

static void lcd_demo_banner() {
  Serial.println("========================================");
  Serial.println("[LCD_DEMO]  HALO DEMO UNIT - LCD ONLY");
  Serial.println("[LCD_DEMO]  no Sense board / no WiFi / no cloud");
  Serial.printf("[LCD_DEMO]  preloaded list items: %d\n", kLcdDemoItemCountInitial);
  Serial.println("========================================");
}

#endif  // HALO_DEMO_MODE
#endif  // LCD_DEMO_H

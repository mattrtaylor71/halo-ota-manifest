// sense_demo.h — HALO demo-unit mode (fully offline, no cloud, no uploads)
//
// PURPOSE
//   A showroom / trade-show / investor-demo build. Every user-facing flow
//   completes successfully with the REAL production UI, but nothing touches
//   WiFi, TLS, MQTT, S3, the backend, or OTA:
//
//     Check-in   HOLD_STILL -> "expiry?"        -> LOGGED
//     Check-out  HOLD_STILL -> "expiry?"        -> LOGGED
//     Discard    HOLD_STILL -> "add to list?"   -> LOGGED
//     Dish       HOLD_STILL                     -> LOGGED
//     Voice      "On it"                        -> done (LCD is fire-and-forget)
//     List       preloaded canned table, instant
//
// WHY THIS IS SENSE-ONLY
//   The LCD is already a slave to the Sense's UI_STATUS phase machine
//   (op/phase/mode/text/job_id -> screen). Scripting those exact phases here
//   reproduces the shipped UX pixel-for-pixel, so the LCD firmware needs ZERO
//   changes. That keeps the hardest-to-test code (LVGL) byte-identical to the
//   build already validated on hardware.
//
//   Phase sequences and DONE text below are transcribed from the production
//   state machine in Sense_Minimal.ino (op_worker_task, OP_SCAN branch) and
//   flow_plan_print() in sense_scan.h. If prod changes, re-check them.
//
// SCOPE
//   Entirely compiled out when HALO_DEMO_MODE == 0. Production builds are
//   unaffected. See halo_ota_demo/firmware/shared/BuildFlags.h.
//
// ORDERING REQUIREMENT
//   Include this LATE in Sense_Minimal.ino — immediately before
//   op_worker_task(). It calls, without forward declarations:
//     OpJob/OpType (sense_ops.h) · g_shopping_list/g_list_count/g_list_mutex
//     scan_ui_status_emit, flow_plan_print, flow_step, scan_ui_inflight_set,
//     scan_terminal_reset, dish_scan_inflight_set (sense_scan.h)
//     uart_send_ui_status, uart_send_ui_list, uart_send_ui_status_extended
//     expiry_date_response_received, discard_choice_response_received
//     list_refresh_mark_complete (sense_list.h) · capture_fill_led_set

#ifndef SENSE_DEMO_H
#define SENSE_DEMO_H

// Sense_Minimal.ino is #included by the prod wrapper BEFORE the wrapper pulls
// in BuildFlags.h, so HALO_DEMO_MODE is not yet visible. Pull it in directly.
#include "../halo_ota_demo/firmware/shared/BuildFlags.h"

#if HALO_DEMO_MODE

// Fail loudly rather than ship a demo unit that falls asleep mid-showroom and
// drops its USB port. Both gates are set for us in BuildFlags.h / the build
// script; if something has overridden one, say so at compile time.
#if !STRESS_TEST_NO_SLEEP
#warning "HALO_DEMO_MODE=1 but STRESS_TEST_NO_SLEEP=0 - the Sense WILL deep-sleep"
#endif

// ── Timing ────────────────────────────────────────────────────────────────────
// Chosen to mimic the real device so the demo does not feel instant/fake.
// Real hardware: camera init + capture is ~700-1500ms.
static const uint32_t DEMO_CAPTURE_MS      = 900;   // HOLD_STILL dwell
static const uint32_t DEMO_SETTLE_MS       = 250;   // beat before LOGGED
static const uint32_t DEMO_VOICE_THINK_MS  = 1100;  // "On it" -> DONE
// Mirrors MAX_EXPIRY_WAIT_MS / MAX_DISCARD_WAIT_MS in the production path.
static const uint32_t DEMO_INPUT_WAIT_MS   = 30000;
static const uint32_t DEMO_POLL_MS         = 100;

// ── Preloaded shopping list ───────────────────────────────────────────────────
// Shown when the user opens the shopping-list screen. Edit freely; keep it
// under MAX_LIST_ITEMS (50) and each field under its column width
// (text/id 64B, store 48B). `id` must be unique and non-empty — the LCD keys
// row identity and its delete overlay on it.
struct demo_list_seed_t {
  const char* id;
  const char* text;
  const char* store;
};

static const demo_list_seed_t kDemoShoppingList[] = {
  { "demo-001", "Bananas",              "Whole Foods" },
  { "demo-002", "Whole milk",           "Whole Foods" },
  { "demo-003", "Large eggs",           "Whole Foods" },
  { "demo-004", "Sourdough bread",      "Whole Foods" },
  { "demo-005", "Baby spinach",         "Trader Joe's" },
  { "demo-006", "Roma tomatoes",        "Trader Joe's" },
  { "demo-007", "Chicken thighs",       "Trader Joe's" },
  { "demo-008", "Greek yogurt",         "Trader Joe's" },
  { "demo-009", "Sharp cheddar",        "" },
  { "demo-010", "Olive oil",            "" },
  { "demo-011", "Ground coffee",        "" },
  { "demo-012", "Avocados",             "" },
};
static const int kDemoShoppingListCount =
    (int)(sizeof(kDemoShoppingList) / sizeof(kDemoShoppingList[0]));

// Copy the canned table into the live list state. Safe to call repeatedly.
// Takes g_list_mutex when it exists (it is created in setup() before this runs,
// but tolerate NULL so an early/absent mutex cannot deadlock a demo unit).
static void demo_seed_shopping_list(const char* reason) {
  bool locked = false;
  if (g_list_mutex != NULL &&
      xSemaphoreTake(g_list_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    locked = true;
  }

  int n = kDemoShoppingListCount;
  if (n > MAX_LIST_ITEMS) {
    n = MAX_LIST_ITEMS;   // table outgrew the array — clamp, never overrun
  }

  for (int i = 0; i < n; i++) {
    // strncpy + explicit NUL: every field is a fixed-width char array.
    strncpy(g_shopping_list[i].text, kDemoShoppingList[i].text,
            sizeof(g_shopping_list[i].text) - 1);
    g_shopping_list[i].text[sizeof(g_shopping_list[i].text) - 1] = '\0';

    strncpy(g_shopping_list[i].id, kDemoShoppingList[i].id,
            sizeof(g_shopping_list[i].id) - 1);
    g_shopping_list[i].id[sizeof(g_shopping_list[i].id) - 1] = '\0';

    strncpy(g_shopping_list[i].store, kDemoShoppingList[i].store,
            sizeof(g_shopping_list[i].store) - 1);
    g_shopping_list[i].store[sizeof(g_shopping_list[i].store) - 1] = '\0';

    // No household UUID offline. The LCD only needs it to delete server-side,
    // and demo deletes are local-only.
    g_shopping_list[i].huuid[0] = '\0';
  }

  g_list_count = n;
  g_selected_index = -1;

  if (locked) {
    xSemaphoreGive(g_list_mutex);
  }
  Serial.printf("[DEMO] shopping list seeded count=%d reason=%s\n",
                n, reason ? reason : "unknown");
}

// ── Local list delete ─────────────────────────────────────────────────────────
// Production deletes server-side then refreshes. Offline, do it in RAM and push
// a fresh UI_LIST so the row visibly disappears — a demo viewer will tap this.
// Deletions last until reboot / next LIST_REFRESH, which re-seeds the table.
static void demo_delete_list_item(const char* id) {
  if (!id || !id[0]) {
    Serial.println("[DEMO] delete ignored (empty id)");
    return;
  }
  bool locked = false;
  if (g_list_mutex != NULL &&
      xSemaphoreTake(g_list_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    locked = true;
  }

  int found = -1;
  for (int i = 0; i < g_list_count; i++) {
    if (strcmp(g_shopping_list[i].id, id) == 0) {
      found = i;
      break;
    }
  }
  if (found >= 0) {
    // Shift the tail down one slot to close the gap.
    for (int i = found; i < g_list_count - 1; i++) {
      g_shopping_list[i] = g_shopping_list[i + 1];
    }
    g_list_count--;
    if (g_selected_index >= g_list_count) {
      g_selected_index = -1;
    }
  }

  if (locked) {
    xSemaphoreGive(g_list_mutex);
  }

  if (found >= 0) {
    Serial.printf("[DEMO] deleted list item id=%s idx=%d remaining=%d\n",
                  id, found, g_list_count);
  } else {
    Serial.printf("[DEMO] delete id=%s not found (list unchanged)\n", id);
  }
  uart_send_ui_list();          // repaint either way, so the UI cannot desync
  uart_send_ui_status("IDLE");  // release the LCD's pending-delete state
}

// ── Scripted scan ─────────────────────────────────────────────────────────────
// Waits for a user choice the LCD is expected to send, exactly like prod:
// polls a response flag up to DEMO_INPUT_WAIT_MS, then proceeds anyway so a
// demo can never wedge on an un-tapped prompt.
static bool demo_wait_for_flag(volatile bool* flag, const char* what) {
  uint32_t start = millis();
  while (!(*flag) && (millis() - start) < DEMO_INPUT_WAIT_MS) {
    vTaskDelay(pdMS_TO_TICKS(DEMO_POLL_MS));
  }
  bool got = *flag;
  Serial.printf("[DEMO] %s %s after %lums\n",
                what ? what : "input",
                got ? "received" : "TIMEOUT (proceeding)",
                (unsigned long)(millis() - start));
  return got;
}

static void demo_run_scan(const OpJob& job) {
  Serial.printf("[DEMO] SCAN mode=%s job_id=%lu (offline, no upload)\n",
                job.mode, (unsigned long)job.job_id);
  scan_ui_inflight_set(true, "demo_scan_start");
  bool is_dish = scan_mode_is_dish(job.mode);
  if (is_dish) {
    dish_scan_inflight_set(true, "demo_scan_start");
  }
  scan_terminal_reset();
  flow_plan_print(job.job_id, job.mode);

  // 1) HOLD_STILL. Fill LED on for the whole "capture" so it reads as real.
  capture_fill_led_set(true, "demo_capturing");
  scan_ui_status_emit("CAPTURING", "Capturing image…", job.mode, job.job_id, false);
  flow_step(job.job_id, "CAPTURING");
  vTaskDelay(pdMS_TO_TICKS(DEMO_CAPTURE_MS));

#if HALO_DEMO_CAPTURE
  // Opt-in: take a real frame and throw it away. Never uploaded, never spooled.
  // Failure is deliberately NON-FATAL — a demo unit must still report success.
  if (init_camera()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      Serial.printf("[DEMO] captured %ux%u %ubytes (discarded)\n",
                    fb->width, fb->height, (unsigned)fb->len);
      esp_camera_fb_return(fb);
    } else {
      Serial.println("[DEMO] capture returned no frame — continuing anyway");
    }
    deinit_camera();
  } else {
    Serial.println("[DEMO] camera init failed — continuing anyway");
  }
#endif

  capture_fill_led_set(false, "demo_captured");

  // 2) Mode-specific user prompt, mirroring the production sequence.
  if (scan_mode_is_check(job.mode)) {
    // check-in / check-out: LCD shows the expiry picker.
    expiry_date_response_received = false;
    scan_ui_status_emit("WAITING_INPUT", "Waiting for expiry date...",
                        job.mode, job.job_id, false);
    flow_step(job.job_id, "WAITING_INPUT");
    demo_wait_for_flag(&expiry_date_response_received, "expiry_date");
  } else if (scan_mode_is_discard(job.mode)) {
    // discard: LCD shows "add to shopping list?".
    discard_choice_response_received = false;
    scan_ui_status_emit("WAITING_INPUT", "Add to shopping list?",
                        job.mode, job.job_id, false);
    flow_step(job.job_id, "WAITING_INPUT");
    demo_wait_for_flag(&discard_choice_response_received, "discard_choice");
  }
  // dish: no prompt, straight to LOGGED.

  vTaskDelay(pdMS_TO_TICKS(DEMO_SETTLE_MS));

  // 3) LOGGED. Same phase + text prod sends, so the LCD shows the real screen.
  scan_ui_status_emit("DONE", "Logged!", job.mode, job.job_id, false);
  flow_step(job.job_id, "DONE_UI");
  scan_ui_inflight_set(false, "demo_ui_done");
  if (is_dish) {
    dish_scan_inflight_set(false, "demo_ui_done");
  }
  current_job.state = OP_DONE;
  Serial.printf("[DEMO] SCAN complete mode=%s (nothing queued for upload)\n",
                job.mode);
}

// ── Scripted voice ────────────────────────────────────────────────────────────
// The LCD is already fire-and-forget for voice: on long-press-end it shows
// "On it" locally and ignores our UI_STATUS except to clear its own flags on
// DONE/ERROR. So emit DONE — never ERROR, which the LCD *does* surface.
static void demo_run_voice(const OpJob& job) {
  Serial.printf("[DEMO] VOICE job_id=%lu (offline, no transcription)\n",
                (unsigned long)job.job_id);
  vTaskDelay(pdMS_TO_TICKS(DEMO_VOICE_THINK_MS));
  uart_send_ui_status_extended("VOICE", "DONE", "Got it!", NULL,
                               job.job_id, NULL, NULL);
  current_job.state = OP_DONE;
  Serial.println("[DEMO] VOICE complete");
}

// ── Scripted list refresh ─────────────────────────────────────────────────────
// Mirrors the production OP_LIST_REFRESH branch minus the network fetch. The
// "IDLE" awake-proof line matters: it is what makes the LCD leave its
// REFRESH_WAKE_PENDING state deterministically before UI_LIST lands.
static void demo_run_list_refresh(const OpJob& job) {
  Serial.println("[DEMO] LIST_REFRESH (serving preloaded list)");
  uart_send_ui_status("IDLE");
  demo_seed_shopping_list("list_refresh");
  uart_send_ui_list();
  list_refresh_mark_complete("demo");
  (void)job;
}

// ── Entry point ───────────────────────────────────────────────────────────────
// Returns true if the job was fully handled here, so op_worker_task should skip
// the real state machines. Falls through (false) for anything unrecognised so a
// new op type cannot be silently swallowed.
static bool demo_run_job(const OpJob& job) {
  switch (job.type) {
    case OP_SCAN:
      demo_run_scan(job);
      return true;
    case OP_VOICE:
      demo_run_voice(job);
      return true;
    case OP_LIST_REFRESH:
      demo_run_list_refresh(job);
      return true;
    default:
      Serial.printf("[DEMO] unhandled op type=%d — falling through to prod path\n",
                    (int)job.type);
      return false;
  }
}

static void demo_banner() {
  Serial.println("========================================");
  Serial.println("[DEMO]  HALO DEMO UNIT — OFFLINE BUILD");
  Serial.println("[DEMO]  no WiFi / no uploads / no cloud");
  Serial.printf("[DEMO]  real camera capture: %s\n",
                HALO_DEMO_CAPTURE ? "ON" : "off");
  Serial.printf("[DEMO]  preloaded list items: %d\n", kDemoShoppingListCount);
  Serial.println("========================================");
}

#endif  // HALO_DEMO_MODE
#endif  // SENSE_DEMO_H

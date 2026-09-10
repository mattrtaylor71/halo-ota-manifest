// lcd_freeze_wdt.h — system-level watchdog so a frozen LCD reboots itself.
//
// WHY: the LCD wedged three times in one bench session. Each time the USB CDC
// stayed enumerated but the firmware produced NOTHING — no response to `home`
// or `liststate`, and esptool could not sync ("No serial data received"). The
// board was unrecoverable without physically power-cycling it.
//
// That is the worst possible failure for this product. uart_task runs on Core 0
// and owns the inter-board link, so when the LCD stops, the Sense is talking to
// a corpse: captures never relay, nothing reports, and the device is dead until
// a human unplugs it. In a kitchen there is no human who knows to do that.
//
// LCD_Minimal.ino carried the comment "Minimal telemetry for freeze triage (no
// watchdog)" — the application-level watchdogs (processing, capture,
// WAKE_PENDING) all assume the firmware is still RUNNING. None of them can fire
// when the scheduler itself is stuck, which is exactly the observed fault.
//
// A panic reboots. A freeze does not. This closes the second case: the hardware
// TWDT fires from interrupt context and resets the chip regardless of what the
// tasks are doing, and the reboot reason is recorded so the freeze is still
// diagnosable afterwards rather than silently swallowed.
//
// The timeout is deliberately generous. uart_task legitimately blocks for long
// stretches during a binary transfer (an image spool is ~16-18s of lock-step
// COBS frames), so a tight timeout would reboot mid-transfer and destroy the
// photo it was saving. Those loops feed the watchdog explicitly instead.

#pragma once

#include "esp_task_wdt.h"

#ifndef LCD_FREEZE_WDT_TIMEOUT_MS
#define LCD_FREEZE_WDT_TIMEOUT_MS 45000   // > the ~18s worst-case image transfer
#endif
#ifndef LCD_FREEZE_WDT_ENABLE
#define LCD_FREEZE_WDT_ENABLE 1
#endif

static bool g_freeze_wdt_ready = false;

// Bench-only hang injection.
//
// The watchdog above is the one protection that cannot be verified by reading
// the code: it either resets a wedged board or it does not, and the only way to
// know is to wedge one on purpose. Compiled out of production entirely — this
// must not be a flag someone can set at runtime on a shipped device.
#ifndef HALO_FREEZE_TEST
#define HALO_FREEZE_TEST 0
#endif
#if HALO_FREEZE_TEST
// Set from the `freezeui` bench command; ui_task spins on it WITHOUT feeding,
// which is the realistic freeze — the task is alive, it just never comes back
// round to the reset call.
static volatile bool g_bench_freeze_ui = false;
#endif

// Call once from setup(), AFTER the scheduler is running and the tasks exist.
static void lcd_freeze_wdt_init() {
#if LCD_FREEZE_WDT_ENABLE
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = LCD_FREEZE_WDT_TIMEOUT_MS;
  // Do NOT subscribe the idle tasks. The UI task runs long LVGL flushes and the
  // idle task can legitimately be starved for a while; subscribing it would
  // reboot a healthy device under load. We watch the two tasks whose death
  // actually bricks the board.
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;

  // The Arduino core may have already initialised the TWDT; init() then returns
  // ESP_ERR_INVALID_STATE and reconfigure() is the correct call.
  esp_err_t err = esp_task_wdt_init(&cfg);
  if (err == ESP_ERR_INVALID_STATE) {
    err = esp_task_wdt_reconfigure(&cfg);
  }
  if (err != ESP_OK) {
    Serial.printf("[FREEZE_WDT] init failed: %s - LCD remains unprotected\n",
                  esp_err_to_name(err));
    return;
  }
  g_freeze_wdt_ready = true;
  Serial.printf("[FREEZE_WDT] armed timeout=%ums\n",
                (unsigned)LCD_FREEZE_WDT_TIMEOUT_MS);
#endif
}

// Each protected task calls this once, from inside itself.
static void lcd_freeze_wdt_subscribe(const char* who) {
#if LCD_FREEZE_WDT_ENABLE
  if (!g_freeze_wdt_ready) return;
  const esp_err_t err = esp_task_wdt_add(NULL);
  Serial.printf("[FREEZE_WDT] subscribe %s: %s\n", who ? who : "?",
                err == ESP_OK ? "ok" : esp_err_to_name(err));
#endif
}

// Feed. Safe to call from a task that is not subscribed (returns an error we
// ignore), so the long-transfer loops can call it unconditionally.
static inline void lcd_freeze_wdt_feed() {
#if LCD_FREEZE_WDT_ENABLE
  if (!g_freeze_wdt_ready) return;
  esp_task_wdt_reset();
#endif
}

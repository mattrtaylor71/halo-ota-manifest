// sense_camera_grab.h — a bounded replacement for esp_camera_fb_get().
//
// WHY: esp_camera_fb_get() takes no timeout. The installed driver exposes only
//   camera_fb_t* esp_camera_fb_get(void);
// with no timeout variant, so a sensor that never delivers a frame blocks the
// calling task forever. Every budget check in sense_camera.h (CAMERA_PREFLIGHT_
// BUDGET_MS, camera_timeline_event, the elapsed_ms comparisons) runs AFTER the
// call returns, so none of them can fire on the one failure they were written
// for. That is the second candidate cause of the reported "capture hangs on the
// capturing screen", alongside a lost UART request.
//
// HOW: the grab runs on a dedicated worker task; callers wait on a semaphore
// with a deadline. On expiry the caller gets nullptr — an honest, reportable
// error — instead of never returning.
//
// EVERY grab must go through here. Two contexts calling esp_camera_fb_get()
// concurrently would race inside the driver, so a direct call left anywhere
// else reintroduces the bug in a harder-to-find form. All five former call
// sites in sense_camera.h are converted.
//
// ON A REAL STALL the worker stays blocked inside the driver — there is no way
// to cancel it, and esp_camera_deinit() would itself block on the same state.
// So a stall latches: further grabs are refused rather than queued behind a
// task that will never wake. sense_camera_grab_recover() is the only exit, and
// on a genuinely wedged worker a restart is the only mechanism that works.

#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifndef CAMERA_GRAB_TIMEOUT_MS
#define CAMERA_GRAB_TIMEOUT_MS 5000      // final capture: generous vs ~300-800ms typical
#endif
#ifndef CAMERA_GRAB_WARMUP_TIMEOUT_MS
#define CAMERA_GRAB_WARMUP_TIMEOUT_MS 2500  // warmup/preflight frames are small and fast
#endif
#ifndef CAMERA_GRAB_TASK_STACK
#define CAMERA_GRAB_TASK_STACK 4096
#endif

// Self-heal a wedged camera by restarting. Set to 0 to leave the camera refused
// instead. Default on: "camera dead until someone power-cycles it" is the wrong
// behaviour for a device sitting in a kitchen, and a restart is the only thing
// that actually clears a task blocked inside the driver.
#ifndef CAMERA_GRAB_WEDGE_RESTART
#define CAMERA_GRAB_WEDGE_RESTART 1
#endif
// Hard cap so a genuinely broken sensor cannot turn into a boot loop. Cleared by
// the first successful grab after a restart, so an occasional wedge weeks apart
// never accumulates toward the cap.
#ifndef CAMERA_GRAB_WEDGE_RESTART_MAX
#define CAMERA_GRAB_WEDGE_RESTART_MAX 3
#endif

// RTC_NOINIT_ATTR, *not* RTC_DATA_ATTR — this was a real bug, caught 2026-08-20.
//
// `RTC_DATA_ATTR uint32_t x = 0;` lands in `.rtc.data`, which is INITIALISED
// data: it survives a deep-sleep wake, but the startup code re-initialises it
// from the image on any full reset — including the `esp_restart()` that self-heal
// itself performs. So the counter this cap depends on was being zeroed by the
// very restart it was counting. Measured on hardware: eight consecutive
// self-heal restarts all reported `wedge_restart code=1`, never 2, and
// `restart_budget_exhausted` never fired. A genuinely faulty camera would have
// rebooted the device forever — precisely the boot loop the cap exists to
// prevent. Only the SEEDED branches had ever been tested, which is why it hid.
//
// `.rtc_noinit` is not initialised at all, so it carries across a restart. The
// cost is that it holds garbage after a power cycle, hence the magic below.
RTC_NOINIT_ATTR static uint32_t g_cam_wedge_restarts;
RTC_NOINIT_ATTR static uint32_t g_cam_wedge_magic;
#define CAM_WEDGE_MAGIC 0x57454447u   // 'WEDG'

// Call once early on every boot. Power-on leaves .rtc_noinit as garbage, so the
// magic is what distinguishes "fresh device" from "restarted mid-self-heal".
static void cam_wedge_counter_init() {
  if (g_cam_wedge_magic != CAM_WEDGE_MAGIC) {
    g_cam_wedge_magic = CAM_WEDGE_MAGIC;
    g_cam_wedge_restarts = 0;
    Serial.println("[CAM_GRAB] wedge counter initialised (power-on)");
  } else {
    Serial.printf("[CAM_GRAB] wedge counter carried across restart: %lu/%d\n",
                  (unsigned long)g_cam_wedge_restarts,
                  (int)CAMERA_GRAB_WEDGE_RESTART_MAX);
  }
}

static TaskHandle_t      s_cam_grab_task   = nullptr;
static SemaphoreHandle_t s_cam_grab_req    = nullptr;  // caller -> worker
static SemaphoreHandle_t s_cam_grab_done   = nullptr;  // worker -> caller
static portMUX_TYPE      s_cam_grab_mux    = portMUX_INITIALIZER_UNLOCKED;

static camera_fb_t* volatile s_cam_grab_result    = nullptr;
static volatile bool         s_cam_grab_abandoned = false;  // caller gave up on the in-flight grab
static volatile bool         s_cam_grab_stalled   = false;  // a grab overran; camera unusable until recover
static volatile uint32_t     s_cam_grab_stalls    = 0;

// Bench-only stall injection.
//
// The whole point of this file is the case where the sensor never delivers a
// frame, and that case cannot be produced on demand with working hardware. The
// counter lives in RTC memory ON PURPOSE: self-heal calls esp_restart(), so a
// plain static would clear on the way through and the camera would "recover"
// after one restart no matter what. Surviving the restart is what lets the
// restart BUDGET be tested rather than just the timeout.
#ifndef HALO_CAM_STALL_TEST
#define HALO_CAM_STALL_TEST 0
#endif
#if HALO_CAM_STALL_TEST
RTC_NOINIT_ATTR static uint32_t g_bench_cam_stall;   // stall the next N grabs

// Self-arming variant, so the wedge-restart COUNTER can be watched incrementing
// across REAL self-heal restarts.
//
// The runtime `camstall` command cannot do that: arming it needs the Sense's USB
// port, and opening that port resets the board and clears the very RTC counter
// under test. Every reconnect after a self-heal restart would zero it again. A
// compile-time count needs no arming at all, so after flashing the port is never
// touched and the increments are observed through the LCD's SENSE_DIAG relay
// (the counter is carried in the diag's `code` field).
//
// Seeded once per POWER-ON: RTC is cleared by a power cycle / esptool reset but
// SURVIVES the esp_restart() that self-heal performs — which is exactly the
// distinction being tested. The magic guards against re-seeding on those SW
// restarts, which would make the count immortal and never reach the budget.
#ifdef HALO_CAM_STALL_AUTO
RTC_NOINIT_ATTR static uint32_t g_bench_cam_stall_seeded;
#define CAM_STALL_SEED_MAGIC 0xC0FFEE01u

static void bench_cam_stall_autoseed() {
  if (g_bench_cam_stall_seeded == CAM_STALL_SEED_MAGIC) {
    Serial.printf("[CAM_STALL_TEST] auto: already seeded, %lu stall(s) left, restarts=%lu\n",
                  (unsigned long)g_bench_cam_stall,
                  (unsigned long)g_cam_wedge_restarts);
    return;
  }
  g_bench_cam_stall = (uint32_t)HALO_CAM_STALL_AUTO;
  g_bench_cam_stall_seeded = CAM_STALL_SEED_MAGIC;
  Serial.printf("[CAM_STALL_TEST] auto: seeded %lu stall(s) at power-on\n",
                (unsigned long)g_bench_cam_stall);
}
#endif
#endif

static void sense_camera_grab_worker(void* /*arg*/) {
  for (;;) {
    xSemaphoreTake(s_cam_grab_req, portMAX_DELAY);

#if HALO_CAM_STALL_TEST
    if (g_bench_cam_stall) {
      g_bench_cam_stall--;
      Serial.printf("[CAM_STALL_TEST] stalling this grab (%lu more after)\n",
                    (unsigned long)g_bench_cam_stall);
      // Outlast any grab timeout, then fall through to a real grab so the
      // late-frame discard path gets exercised too rather than parking a task
      // in the driver forever.
      vTaskDelay(pdMS_TO_TICKS(CAMERA_GRAB_TIMEOUT_MS + 3000));
    }
#endif

    camera_fb_t* fb = esp_camera_fb_get();   // may never return; that is the point

    bool abandoned;
    taskENTER_CRITICAL(&s_cam_grab_mux);
    abandoned = s_cam_grab_abandoned;
    if (!abandoned) s_cam_grab_result = fb;
    taskEXIT_CRITICAL(&s_cam_grab_mux);

    if (abandoned) {
      // The caller timed out and moved on. Returning the frame buffer here is
      // what keeps a late grab from leaking one of the driver's small pool.
      if (fb) esp_camera_fb_return(fb);
      Serial.println("[CAM_GRAB] late frame discarded (caller had given up)");
      continue;   // deliberately no give(): nobody is waiting
    }
    xSemaphoreGive(s_cam_grab_done);
  }
}

// Start the worker. Call once, AFTER esp_camera_init() succeeds.
static bool sense_camera_grab_start() {
  cam_wedge_counter_init();
#if HALO_CAM_STALL_TEST && defined(HALO_CAM_STALL_AUTO)
  bench_cam_stall_autoseed();
#endif
  if (s_cam_grab_task) return true;
  s_cam_grab_req  = xSemaphoreCreateBinary();
  s_cam_grab_done = xSemaphoreCreateBinary();
  if (!s_cam_grab_req || !s_cam_grab_done) {
    Serial.println("[CAM_GRAB] semaphore alloc failed - falling back to unbounded grabs");
    return false;
  }
  // Pinned to core 1 alongside the Arduino/capture context; core 0 carries WiFi
  // and lwIP, and this task must not compete with the upload path.
  BaseType_t ok = xTaskCreatePinnedToCore(sense_camera_grab_worker, "cam_grab",
                                          CAMERA_GRAB_TASK_STACK, nullptr, 5,
                                          &s_cam_grab_task, 1);
  if (ok != pdPASS) {
    s_cam_grab_task = nullptr;
    Serial.println("[CAM_GRAB] task create failed - falling back to unbounded grabs");
    return false;
  }
  Serial.println("[CAM_GRAB] worker started");
  return true;
}

// Release the worker so its stack goes back to the internal heap.
//
// MEASURED: the task costs 5,120 bytes of contiguous DMA-capable RAM (4KB stack
// + TCB), and esp_camera_init() needs a contiguous 16KB block from that same
// pool. Holding it across a camera deinit/init cycle took dma_largest from
// 19444 to 14324 and made init fail outright — 15 of 30 captures in a bench run,
// with the WiFi kill/reconnect churn from each retry then panicking the PHY.
// The worker is idle between captures, so there is nothing to gain from keeping
// it and 5KB to lose.
//
// Refuses to delete a wedged worker: vTaskDelete on a task blocked inside the
// driver would leak the driver's internal state rather than reclaim anything.
// In that case the memory stays gone until the self-heal restart, which is the
// correct trade.
static void sense_camera_grab_stop() {
  if (!s_cam_grab_task) return;
  if (s_cam_grab_stalled) {
    Serial.println("[CAM_GRAB] not stopping - worker is wedged, awaiting restart");
    return;
  }
  vTaskDelete(s_cam_grab_task);
  s_cam_grab_task = nullptr;
  // vTaskDelete hands the stack to the IDLE task to free; it is NOT back the
  // instant this returns. The caller re-reserves a contiguous DMA block right
  // after, so yield first or the reclaim races the reservation it exists to
  // enable.
  vTaskDelay(pdMS_TO_TICKS(20));
  if (s_cam_grab_req)  { vSemaphoreDelete(s_cam_grab_req);  s_cam_grab_req = nullptr; }
  if (s_cam_grab_done) { vSemaphoreDelete(s_cam_grab_done); s_cam_grab_done = nullptr; }
  s_cam_grab_result = nullptr;
  s_cam_grab_abandoned = false;
  Serial.printf("[CAM_GRAB] worker stopped, dma_largest=%u\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static bool sense_camera_grab_stalled() { return s_cam_grab_stalled; }
static uint32_t sense_camera_grab_stall_count() { return s_cam_grab_stalls; }

// The bounded grab. Returns nullptr on timeout (and latches the stall) or when
// the camera is already known-stalled.
static camera_fb_t* sense_camera_fb_get_bounded(uint32_t timeout_ms) {
  // No worker (alloc failed, or called before start): behave exactly as before
  // rather than failing captures outright. Unbounded, but no worse than the
  // code this replaces.
  if (!s_cam_grab_task) return esp_camera_fb_get();

  if (s_cam_grab_stalled) {
    Serial.println("[CAM_GRAB] refusing grab - camera stalled, awaiting recover");
    return nullptr;
  }

  taskENTER_CRITICAL(&s_cam_grab_mux);
  s_cam_grab_abandoned = false;
  s_cam_grab_result    = nullptr;
  taskEXIT_CRITICAL(&s_cam_grab_mux);

  const uint32_t t0 = millis();
  xSemaphoreGive(s_cam_grab_req);

  if (xSemaphoreTake(s_cam_grab_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
    // A good frame proves the restart (if any) worked, so retire the budget.
    // Without this, three unlucky wedges spread over months would permanently
    // disable self-heal.
    if (s_cam_grab_result && g_cam_wedge_restarts) {
      Serial.printf("[CAM_GRAB] healthy again - clearing wedge restart budget (was %lu)\n",
                    (unsigned long)g_cam_wedge_restarts);
      // Relay it. The stall, the wedge and the restart all report to the cloud;
      // without this the recovery is the one step in that chain that is invisible,
      // so a device that wedged once and healed looks identical to one still
      // failing. It is also the only way to see this path on the bench at all:
      // reading the Sense's own USB port resets the board and clears the RTC
      // counter that this branch keys on.
      uart_send_sense_diag("camera", "wedge_recovered", camera_diag_label(),
                           (int32_t)g_cam_wedge_restarts, "budget_retired");
      g_cam_wedge_restarts = 0;
    }
    return s_cam_grab_result;
  }

  // Deadline passed. The worker may have completed inside the race window
  // between the timeout firing and this flag being set, so check before
  // declaring a stall — otherwise a merely-slow frame is reported as a fault.
  taskENTER_CRITICAL(&s_cam_grab_mux);
  s_cam_grab_abandoned = true;
  taskEXIT_CRITICAL(&s_cam_grab_mux);

  if (xSemaphoreTake(s_cam_grab_done, 0) == pdTRUE) {
    taskENTER_CRITICAL(&s_cam_grab_mux);
    s_cam_grab_abandoned = false;
    taskEXIT_CRITICAL(&s_cam_grab_mux);
    Serial.printf("[CAM_GRAB] frame landed in the race window at %lums\n",
                  (unsigned long)(millis() - t0));
    return s_cam_grab_result;
  }

  s_cam_grab_stalled = true;
  s_cam_grab_stalls++;
  Serial.printf("[CAM_GRAB] STALL: no frame after %lums (timeout=%lu) count=%lu\n",
                (unsigned long)(millis() - t0), (unsigned long)timeout_ms,
                (unsigned long)s_cam_grab_stalls);
  uart_send_sense_diag("camera", "grab_stall", camera_diag_label(),
                       (int32_t)timeout_ms, "no_frame_within_bound");
  return nullptr;
}

// Attempt to return to a usable camera after a latched stall.
//
// If the worker is genuinely wedged inside the driver there is no way to
// reclaim it: FreeRTOS cannot cancel a blocked task, and esp_camera_deinit()
// contends on the same internal state. A restart is the only mechanism that
// actually clears it, and leaving the device unable to capture until someone
// power-cycles it is worse than a few seconds of downtime the user can retry
// through. Returns true only if the camera is usable WITHOUT restarting.
static bool sense_camera_grab_recover() {
  if (!s_cam_grab_stalled) return true;

  // If the late frame eventually arrived, the worker is alive after all and the
  // stall was a slow frame rather than a wedge — clear and carry on.
  if (xSemaphoreTake(s_cam_grab_done, 0) == pdTRUE) {
    if (s_cam_grab_result) esp_camera_fb_return(s_cam_grab_result);
    taskENTER_CRITICAL(&s_cam_grab_mux);
    s_cam_grab_result    = nullptr;
    s_cam_grab_abandoned = false;
    s_cam_grab_stalled   = false;
    taskEXIT_CRITICAL(&s_cam_grab_mux);
    Serial.println("[CAM_GRAB] recovered: worker drained, camera usable");
    return true;
  }

  Serial.println("[CAM_GRAB] worker still wedged in driver - restart is the only exit");
  uart_send_sense_diag("camera", "grab_wedged", camera_diag_label(),
                       (int32_t)s_cam_grab_stalls, "restart_required");

#if CAMERA_GRAB_WEDGE_RESTART
  if (g_cam_wedge_restarts < CAMERA_GRAB_WEDGE_RESTART_MAX) {
    g_cam_wedge_restarts++;
    Serial.printf("[CAM_GRAB] self-heal restart %lu/%u\n",
                  (unsigned long)g_cam_wedge_restarts,
                  (unsigned)CAMERA_GRAB_WEDGE_RESTART_MAX);
    uart_send_sense_diag("camera", "wedge_restart", camera_diag_label(),
                         (int32_t)g_cam_wedge_restarts, "self_heal");
    // Let the diag line and the LCD's error report actually leave the UART —
    // restarting mid-write would lose the one message that explains the reboot.
    Serial.flush();
    delay(120);
    ESP.restart();
  }
  // Budget exhausted: a sensor that wedges every time is a hardware fault, and
  // rebooting forever would be worse than staying up with a dead camera — the
  // user can still use voice, the list and settings, and the diag above says why.
  Serial.printf("[CAM_GRAB] wedge restart budget exhausted (%lu) - staying up with camera refused\n",
                (unsigned long)g_cam_wedge_restarts);
  uart_send_sense_diag("camera", "wedge_giveup", camera_diag_label(),
                       (int32_t)g_cam_wedge_restarts, "restart_budget_exhausted");
#endif
  return false;
}

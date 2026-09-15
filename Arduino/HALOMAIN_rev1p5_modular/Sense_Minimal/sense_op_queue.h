/*
 * sense_op_queue.h
 *
 * Op-queue enqueue, foreground priority detection, upload job
 * parking/requeueing, and foreground-clear wait logic.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 15.
 *
 * Prerequisites (must be declared before #include "sense_op_queue.h"):
 *   - freertos/FreeRTOS.h, freertos/semphr.h, freertos/task.h
 *   - OpJob, UploadJob structs from sense_ops.h
 *   - op_queue, upload_queue queue handles
 *   - foreground_active, scan_ui_inflight, dish_scan_inflight
 *   - current_job (OpJob)
 *   - last_input_wake_ms, last_user_activity_ms, last_lcd_communication
 *   - ACTION_MIN_REMAINING_MS
 *   - upload_queue_count() (forward declared)
 *   - scan_mode_is_dish() from sense_scan.h
 *   - deadline_remaining_ms() from sense_http.h
 */

#ifndef SENSE_OP_QUEUE_H
#define SENSE_OP_QUEUE_H

// Forward declarations for .ino functions called by op queue helpers
static uint32_t upload_queue_count();

// ── Op-queue enqueue ──────────────────────────────────────────────

static bool enqueue_op_job(const OpJob& job, bool prioritize_front, const char* source) {
  if (op_queue == NULL) {
    Serial.printf("[OP_QUEUE] unavailable type=%d source=%s\n",
                  (int)job.type,
                  source ? source : "unknown");
    return false;
  }
  BaseType_t queued = prioritize_front
                          ? xQueueSendToFront(op_queue, &job, pdMS_TO_TICKS(10))
                          : xQueueSend(op_queue, &job, pdMS_TO_TICKS(10));
  Serial.printf("[OP_QUEUE] %s type=%d pri=%d id=%lu source=%s front=%d depth=%lu\n",
                queued == pdTRUE ? "queued" : "queue_failed",
                (int)job.type,
                (int)job.pri,
                (unsigned long)job.job_id,
                source ? source : "unknown",
                prioritize_front ? 1 : 0,
                (unsigned long)(op_queue ? uxQueueMessagesWaiting(op_queue) : 0));
  return queued == pdTRUE;
}

// ── Foreground priority detection ─────────────────────────────────

static bool foreground_priority_active(unsigned long now_ms, const char** reason_out) {
  const char* reason = NULL;
  if (foreground_active ||
      (current_job.active &&
       current_job.pri == PRI_USER &&
       current_job.state != OP_IDLE &&
       current_job.state != OP_DONE)) {
    reason = "foreground_active";
  } else if (scan_ui_inflight || dish_scan_inflight) {
    reason = "scan_ui_inflight";
  } else if (voice_recording_active) {
    reason = "voice_recording";
  } else if (op_queue != NULL && uxQueueMessagesWaiting(op_queue) > 0) {
    OpJob queued_job = {};
    if (xQueuePeek(op_queue, &queued_job, 0) == pdTRUE && queued_job.pri == PRI_USER) {
      reason = "queued_user_job";
    }
  }
  if (reason == NULL && last_input_wake_ms > 0 && (now_ms - last_input_wake_ms) < 2500UL) {
    reason = "recent_input_wake";
  }
  if (reason == NULL && last_user_activity_ms > 0 && (now_ms - last_user_activity_ms) < 1800UL) {
    reason = "recent_user_input";
  }
  if (reason == NULL && last_lcd_communication > 0 && (now_ms - last_lcd_communication) < 1000UL) {
    reason = "recent_lcd_link";
  }
  if (reason_out) {
    *reason_out = reason;
  }
  return reason != NULL;
}

static bool foreground_priority_reason_is_transient(const char* reason) {
  if (!reason) {
    return false;
  }
  return strcmp(reason, "recent_input_wake") == 0 ||
         strcmp(reason, "recent_user_input") == 0 ||
         strcmp(reason, "recent_lcd_link") == 0;
}

// ── Foreground wait / park / requeue ──────────────────────────────

// True while the worker is holding a DEQUEUED job in its own stack frame.
//
// Without this the job is invisible: it is out of the queue, not parked, and not
// inflight, so the sleep flush saw "nothing pending", logged DRAINED and slept --
// and deep sleep wipes the PSRAM the image lives in. Measured 2026-08-21:
// 15 captures, 12 uploads, 3 destroyed exactly this way, with every gate green.
static volatile bool upload_worker_holding_in_place = false;

static bool upload_wait_for_foreground_window(const UploadJob& job,
                                              const char* initial_reason,
                                              uint32_t max_wait_ms) {
  if (!foreground_priority_reason_is_transient(initial_reason)) {
    return false;
  }
  // The sleep flush IS the moment to upload. The transient reasons are all
  // proxies for "the user is mid-interaction", and the loudest of them,
  // recent_lcd_link, is guaranteed true during pre-sleep because the sleep
  // handshake is itself LCD traffic -- so this held almost every single-capture
  // session right at the point it needed to drain.
  if (g_upload_flush_requested) {
    return true;
  }
  unsigned long start_ms = millis();
  unsigned long last_log_ms = 0;
  struct HoldFlag {
    HoldFlag()  { upload_worker_holding_in_place = true;  }
    ~HoldFlag() { upload_worker_holding_in_place = false; }
  } hold_flag;
  while ((millis() - start_ms) < max_wait_ms) {
    const char* active_reason = NULL;
    if (!foreground_priority_active(millis(), &active_reason)) {
      return true;
    }
    if (!foreground_priority_reason_is_transient(active_reason)) {
      return false;
    }
    if (g_upload_flush_requested) {
      return true;   // sleep flush opened while we were holding
    }
    unsigned long now_ms = millis();
    if (last_log_ms == 0 || (now_ms - last_log_ms) >= 700UL) {
      Serial.printf("[UPLOAD_QUEUE] hold_in_place job_id=%lu mode=%s voice=%d reason=%s waited=%lu\n",
                    (unsigned long)job.job_id,
                    job.mode,
                    job.is_voice ? 1 : 0,
                    active_reason ? active_reason : "foreground_priority",
                    (unsigned long)(now_ms - start_ms));
      last_log_ms = now_ms;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  return false;
}

static bool upload_worker_has_parked_job = false;
static UploadJob upload_worker_parked_job = {};
static const char* upload_worker_parked_stage = "idle";
static unsigned long upload_worker_parked_at_ms = 0;
static portMUX_TYPE upload_worker_parked_mux = portMUX_INITIALIZER_UNLOCKED;

static bool upload_worker_parked_pending() {
  portENTER_CRITICAL(&upload_worker_parked_mux);
  const bool pending = upload_worker_has_parked_job;
  portEXIT_CRITICAL(&upload_worker_parked_mux);
  return pending;
}

// A worker resume and a main-task sleep rescue can race. Transfer the whole
// descriptor exactly once; neither consumer may retain the shared reference.
static bool upload_worker_take_parked_job(UploadJob& out,
                                         const char** stage = nullptr,
                                         unsigned long* parked_at_ms = nullptr) {
  portENTER_CRITICAL(&upload_worker_parked_mux);
  const bool claimed = upload_worker_has_parked_job;
  if (claimed) {
    out = upload_worker_parked_job;
    if (stage) *stage = upload_worker_parked_stage;
    if (parked_at_ms) *parked_at_ms = upload_worker_parked_at_ms;
    upload_worker_parked_job = {};
    upload_worker_has_parked_job = false;
    upload_worker_parked_stage = "idle";
    upload_worker_parked_at_ms = 0;
  }
  portEXIT_CRITICAL(&upload_worker_parked_mux);
  return claimed;
}

static bool upload_worker_park_job(const UploadJob& job, const char* stage, const char* reason) {
  const char* parked_stage = stage ? stage : "foreground";
  const unsigned long parked_at_ms = millis();
  portENTER_CRITICAL(&upload_worker_parked_mux);
  const bool admitted = !upload_worker_has_parked_job;
  if (admitted) {
    upload_worker_parked_job = job;
    upload_worker_parked_stage = parked_stage;
    upload_worker_parked_at_ms = parked_at_ms;
    upload_worker_has_parked_job = true;
  }
  portEXIT_CRITICAL(&upload_worker_parked_mux);
  // Refusal leaves the caller's local job owned by that caller.
  if (!admitted) return false;
  Serial.printf("[UPLOAD_QUEUE] parked job_id=%lu mode=%s voice=%d stage=%s reason=%s q=%lu\n",
                (unsigned long)job.job_id,
                job.mode,
                job.is_voice ? 1 : 0,
                parked_stage,
                reason ? reason : "foreground_priority",
                (unsigned long)upload_queue_count());
  return true;
}

static bool park_upload_job_if_foreground_active(const UploadJob& job, const char* stage) {
  const char* reason = NULL;
  if (!foreground_priority_active(millis(), &reason)) {
    return false;
  }
  if (upload_wait_for_foreground_window(job, reason, 2200UL)) {
    return false;
  }
  return upload_worker_park_job(job, stage, reason);
}

static bool upload_wait_for_foreground_clear_in_place(const UploadJob& job,
                                                      const char* stage,
                                                      uint32_t deadline_ms,
                                                      bool* budget_exhausted) {
  const char* reason = NULL;
  if (!foreground_priority_active(millis(), &reason)) {
    return true;
  }
  if (upload_wait_for_foreground_window(job, reason, 2200UL)) {
    return true;
  }
  unsigned long parked_start_ms = millis();
  unsigned long last_log_ms = 0;
  while (foreground_priority_active(millis(), &reason)) {
    if (deadline_ms != 0) {
      uint32_t remaining_ms = deadline_remaining_ms(deadline_ms);
      if (remaining_ms < ACTION_MIN_REMAINING_MS) {
        if (budget_exhausted) {
          *budget_exhausted = true;
        }
        return false;
      }
    }
    unsigned long now_ms = millis();
    if (last_log_ms == 0 || (now_ms - last_log_ms) >= 1000UL) {
      Serial.printf("[UPLOAD_QUEUE] parked_in_place job_id=%lu mode=%s voice=%d stage=%s reason=%s waited=%lu\n",
                    (unsigned long)job.job_id,
                    job.mode,
                    job.is_voice ? 1 : 0,
                    stage ? stage : "foreground",
                    reason ? reason : "foreground_priority",
                    (unsigned long)(now_ms - parked_start_ms));
      last_log_ms = now_ms;
    }
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  return true;
}

#endif // SENSE_OP_QUEUE_H

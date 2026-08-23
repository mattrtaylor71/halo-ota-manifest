/*
 * sense_scan.h
 *
 * Scan/flow helpers: screen hints, mode classification, UI status
 * emission, terminal status, flow plan/step logging, inflight tracking,
 * result context management, and scan-request-pending checks.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 14.
 *
 * Prerequisites (must be declared before #include "sense_scan.h"):
 *   - string.h
 *   - PROTOCOL_VERSION, get_next_msg_id(), uart_send_json() from sense_uart.h
 *   - scan_ui_inflight, dish_scan_inflight, scan_terminal_sent globals
 *   - current_job (OpJob)
 *   - current_result_mode, current_scan_job_id
 *   - upload_queue
 *   - foreground_active, last_input_wake_ms, last_user_activity_ms
 *   - uart_send_ui_status_extended() must be defined before this header
 */

#ifndef SENSE_SCAN_H
#define SENSE_SCAN_H

// Forward declarations for .ino functions called by scan helpers
static uint32_t upload_queue_count();
static bool upload_queue_is_full();
static void uart_send_ui_status_extended(const char* op, const char* phase, const char* text, const char* mode, uint32_t job_id, const char* ui_policy, const char* screen_hint);

// ── Screen hint mapping ───────────────────────────────────────────

static const char* scan_screen_hint(const char* phase, const char* mode) {
  const char* safe_phase = phase ? phase : "";
  if (strcmp(safe_phase, "CAPTURING") == 0) {
    return "SHIP_HOLD_STILL";
  }
  if (strcmp(safe_phase, "WAITING_INPUT") == 0) {
    return "SHIP_EXPIRY";
  }
  if (strcmp(safe_phase, "DONE") == 0) {
    return "SHIP_LOGGED";
  }
  if (strcmp(safe_phase, "UPLOAD_STARTING") == 0 ||
      strcmp(safe_phase, "UPLOADING") == 0 ||
      strcmp(safe_phase, "RESULT_WAITING") == 0 ||
      strcmp(safe_phase, "PROCESSING") == 0) {
    return "SHIP_PROCESSING";
  }
  if (strcmp(safe_phase, "ERROR") == 0) {
    return "SHIP_RESULT";
  }
  (void)mode;
  return "";
}

// ── Mode classification ───────────────────────────────────────────

static bool scan_mode_is_quiet(const char* mode) {
  return mode && (strcmp(mode, "check-in") == 0 ||
                  strcmp(mode, "check-out") == 0 ||
                  strcmp(mode, "check_out") == 0 ||
                  strcmp(mode, "discard") == 0);
}

static bool scan_mode_is_check(const char* mode) {
  return mode && (strcmp(mode, "check-in") == 0 ||
                  strcmp(mode, "check-out") == 0 ||
                  strcmp(mode, "check_out") == 0);
}

static bool scan_mode_is_discard(const char* mode) {
  return mode && (strcmp(mode, "discard") == 0);
}

static bool scan_mode_is_dish(const char* mode) {
  return mode && (strcmp(mode, "dish") == 0 ||
                  strcmp(mode, "dish-log") == 0 ||
                  strcmp(mode, "dish_log") == 0);
}

// ── Scan UI emission logic ────────────────────────────────────────

static bool scan_phase_is_allowed_quiet(const char* phase) {
  if (!phase) return false;
  return (strcmp(phase, "CAPTURING") == 0 ||
          strcmp(phase, "WAITING_INPUT") == 0 ||
          strcmp(phase, "DONE") == 0 ||
          strcmp(phase, "ERROR") == 0);
}

static bool scan_ui_should_emit(const char* mode, const char* phase, bool immediate_error) {
  if (!scan_mode_is_quiet(mode)) {
    return true;
  }
  if (phase && strcmp(phase, "ERROR") == 0) {
    return immediate_error;
  }
  return scan_phase_is_allowed_quiet(phase);
}

static bool scan_ui_status_emit(const char* phase,
                                const char* text,
                                const char* mode,
                                uint32_t job_id,
                                bool immediate_error) {
  if (!scan_ui_should_emit(mode, phase, immediate_error)) {
    Serial.printf("[UI_STATUS] suppressed op=SCAN phase=%s mode=%s\n",
                  phase ? phase : "",
                  mode ? mode : "");
    return false;
  }
  uart_send_ui_status_extended("SCAN", phase, text ? text : "", mode, job_id, NULL, NULL);
  return true;
}

// ── Inflight tracking ─────────────────────────────────────────────

static void scan_terminal_reset() {
  scan_terminal_sent = false;
}

static void scan_ui_inflight_set(bool active, const char* reason) {
  if (scan_ui_inflight == active) {
    return;
  }
  scan_ui_inflight = active;
  Serial.printf("[OP_INFLIGHT] %s op=scan reason=%s\n",
                active ? "set" : "clear",
                reason ? reason : "unknown");
}

static void dish_scan_inflight_set(bool active, const char* reason) {
  if (dish_scan_inflight == active) {
    return;
  }
  dish_scan_inflight = active;
  Serial.printf("[OP_INFLIGHT] %s op=dish_scan reason=%s\n",
                active ? "set" : "clear",
                reason ? reason : "unknown");
}

// ── Flow plan/step logging ────────────────────────────────────────

static void flow_plan_print(uint32_t job_id, const char* mode) {
  Serial.printf("[FLOW_PLAN] job_id=%lu mode=%s\n", (unsigned long)job_id, mode ? mode : "");
  if (mode && (strcmp(mode, "check-in") == 0 || strcmp(mode, "check-out") == 0 || strcmp(mode, "check_out") == 0)) {
    Serial.println("1) LCD: show HOLD_STILL (phase=CAPTURING)");
    Serial.println("2) Sense: init camera + capture");
    Serial.println("3) LCD: show EXPIRY (phase=WAITING_INPUT)");
    Serial.println("4) LCD->Sense: INPUT_EXPIRY_DATE");
    Serial.println("5) LCD: show LOGGED for 2s (phase=DONE)");
    Serial.println("6) Sense: background presign + upload (no UI_STATUS)");
  } else if (scan_mode_is_dish(mode)) {
    Serial.println("1) LCD: show HOLD_STILL (phase=CAPTURING)");
    Serial.println("2) Sense: capture + enqueue upload");
    Serial.println("3) LCD: show LOGGED for 2s (phase=DONE)");
    Serial.println("4) Sense: background presign + upload (deferred to sleep)");
  } else {
    Serial.println("1) LCD: show HOLD_STILL (phase=CAPTURING)");
    Serial.println("2) Sense: capture + enqueue upload");
    Serial.println("3) LCD: show LOGGED for 2s (phase=DONE)");
    Serial.println("4) Sense: background presign + upload (deferred to sleep)");
  }
}

static void flow_step(uint32_t job_id, const char* step_name) {
  Serial.printf("[FLOW_STEP] job_id=%lu %s\n",
                (unsigned long)job_id,
                step_name ? step_name : "");
}

// ── Terminal status ───────────────────────────────────────────────

static void scan_send_terminal_status(const char* phase, const char* text, const char* mode, bool immediate_error) {
  if (scan_terminal_sent) {
    return;
  }
  if (scan_ui_status_emit(phase, text, mode, current_job.job_id, immediate_error)) {
    scan_terminal_sent = true;
  }
}

// ── Upload/scan pending checks ────────────────────────────────────

// Always false since 2026-08-21: dish is a plain capture-and-log, exactly like
// check-in and discard.
//
// This existed to hold and preempt normal uploads so a dish upload could go out
// first and the device could then wait on the AI nutrition result the user was
// watching. That feature was removed from the backend — the firmware still emits
// "DONE / Logged!" for dish and nothing consumes a result — so the special
// treatment only had one remaining effect: it forced a dish upload to run DURING
// a capture session, whose TLS handshake fragments the contiguous DMA block
// esp_camera_init() needs and broke every capture after it (SHIP_CHECKLIST §6).
//
// Kept as a function returning false rather than deleted at every call site, so
// the change is one line to audit and one line to revert.
static bool foreground_scan_pending() {
  if (scan_ui_inflight || dish_scan_inflight) {
    return true;
  }
  if (current_job.active && current_job.type == OP_SCAN &&
      current_job.state != OP_IDLE && current_job.state != OP_DONE) {
    return true;
  }
  return false;
}

static const char* sense_user_state_name() {
  if (foreground_scan_pending()) {
    return "CAPTURE_COMMITTED";
  }
  return "MENU_READY";
}

// sense_device_state_name() stays in .ino (depends on sleep state globals)

static bool scan_request_pending_for_mode(const char* requested_mode) {
  if (foreground_scan_pending()) {
    return true;
  }
  if (upload_queue_is_full()) {
    return true;
  }
  return false;
}

#endif // SENSE_SCAN_H

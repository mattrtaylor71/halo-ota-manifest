/*
 * sense_diag.h
 *
 * Diagnostics and telemetry recording helpers for Sense_Minimal.
 * Pure data-recording functions with no hardware side effects
 * (except Serial prints for boot diagnostics).
 *
 * Extracted from Sense_Minimal.ino as modularization Step 1.
 *
 * Prerequisites (must be declared before #include "sense_diag.h"):
 *   - Arduino.h (millis, Serial, delay)
 *   - esp_sleep.h (esp_sleep_wakeup_cause_t, esp_sleep_get_wakeup_cause)
 *   - esp_system.h (esp_reset_reason_t, esp_reset_reason) [via esp_sleep.h]
 *   - driver/gpio.h (gpio_get_level)
 *   - WAKE_GPIO macro
 *   - RTC_DATA_ATTR globals: g_rtc_clean_shutdown, g_rtc_last_stage,
 *     g_rtc_last_stage_code, g_rtc_last_stage_uptime_ms, g_rtc_crash_count,
 *     g_rtc_last_crash_reason, g_rtc_last_crash_wake_cause,
 *     g_rtc_last_crash_stage, g_rtc_last_crash_stage_code,
 *     g_rtc_last_crash_stage_uptime_ms
 */

#ifndef SENSE_DIAG_H
#define SENSE_DIAG_H

// ── Error / action tracking globals ──────────────────────────────────
static char last_action_label[24] = "";
static unsigned long last_action_ms = 0;
static char last_error_stage[24] = "";
static char last_error_text[96] = "";
static int32_t last_error_code = 0;
static unsigned long last_error_ms = 0;
static uint8_t g_prev_clean_shutdown = 0;

// ── Action event circular log ────────────────────────────────────────
static const uint8_t ACTION_LOG_SIZE = 6;
typedef struct {
  char action[12];
  char mode[12];
  char result[8];
  char stage[16];
  int32_t code;
  unsigned long ts_ms;
} DiagActionEvent;
static DiagActionEvent g_action_events[ACTION_LOG_SIZE];
static uint8_t g_action_event_head = 0;
static uint8_t g_action_event_count = 0;

// ── Camera timeline circular log ─────────────────────────────────────
static const uint8_t CAMERA_TIMELINE_LOG_SIZE = 24;
typedef struct {
  char label[16];
  int32_t code;
  uint32_t ts_ms;
} CameraTimelineEvent;
static CameraTimelineEvent g_camera_events[CAMERA_TIMELINE_LOG_SIZE];
static uint8_t g_camera_event_head = 0;
static uint8_t g_camera_event_count = 0;
static uint32_t g_camera_timeline_start_ms = 0;
static uint32_t g_camera_timeline_last_ms = 0;
static uint32_t g_camera_timeline_total_ms = 0;
static uint32_t g_camera_timeline_len = 0;
static uint16_t g_camera_timeline_w = 0;
static uint16_t g_camera_timeline_h = 0;
static bool g_camera_timeline_ok = false;
static bool g_camera_timeline_done = false;
static char g_camera_timeline_mode[12] = "";

// ── Pure recording helpers ───────────────────────────────────────────

static void diag_sanitize_token(char* dst, size_t dst_len, const char* src) {
  if (!dst || dst_len == 0) {
    return;
  }
  dst[0] = '\0';
  if (!src || !src[0]) {
    return;
  }
  size_t out = 0;
  for (size_t i = 0; src[i] != '\0' && out + 1 < dst_len; ++i) {
    char c = src[i];
    bool ok = (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              (c == '_' || c == '-');
    dst[out++] = ok ? c : '_';
  }
  dst[out] = '\0';
}

static void diag_record_action(const char* action) {
  if (!action || !action[0]) {
    return;
  }
  strncpy(last_action_label, action, sizeof(last_action_label) - 1);
  last_action_label[sizeof(last_action_label) - 1] = '\0';
  last_action_ms = millis();
}

static void diag_record_error(const char* stage, int32_t code, const char* text) {
  if (stage && stage[0]) {
    strncpy(last_error_stage, stage, sizeof(last_error_stage) - 1);
    last_error_stage[sizeof(last_error_stage) - 1] = '\0';
  } else {
    last_error_stage[0] = '\0';
  }
  last_error_code = code;
  if (text && text[0]) {
    strncpy(last_error_text, text, sizeof(last_error_text) - 1);
    last_error_text[sizeof(last_error_text) - 1] = '\0';
  } else {
    last_error_text[0] = '\0';
  }
  last_error_ms = millis();
}

static void diag_note_stage(const char* stage, int32_t code) {
  if (!stage || !stage[0]) {
    return;
  }
  strncpy(g_rtc_last_stage, stage, sizeof(g_rtc_last_stage) - 1);
  g_rtc_last_stage[sizeof(g_rtc_last_stage) - 1] = '\0';
  g_rtc_last_stage_code = code;
  g_rtc_last_stage_uptime_ms = millis();
}

static void diag_record_crash_boot(esp_reset_reason_t reset_reason,
                                   esp_sleep_wakeup_cause_t wake_cause) {
  bool unexpected = (reset_reason == ESP_RST_PANIC ||
                     reset_reason == ESP_RST_INT_WDT ||
                     reset_reason == ESP_RST_TASK_WDT ||
                     reset_reason == ESP_RST_WDT ||
                     reset_reason == ESP_RST_BROWNOUT);
  if (!unexpected) {
    return;
  }
  g_rtc_crash_count++;
  g_rtc_last_crash_reason = static_cast<uint32_t>(reset_reason);
  g_rtc_last_crash_wake_cause = static_cast<uint32_t>(wake_cause);
  strncpy(g_rtc_last_crash_stage, g_rtc_last_stage, sizeof(g_rtc_last_crash_stage) - 1);
  g_rtc_last_crash_stage[sizeof(g_rtc_last_crash_stage) - 1] = '\0';
  g_rtc_last_crash_stage_code = g_rtc_last_stage_code;
  g_rtc_last_crash_stage_uptime_ms = g_rtc_last_stage_uptime_ms;
}

static void diag_record_action_event(const char* action,
                                     const char* mode,
                                     const char* result,
                                     const char* stage,
                                     int32_t code) {
  DiagActionEvent* evt = &g_action_events[g_action_event_head];
  diag_sanitize_token(evt->action, sizeof(evt->action), action);
  diag_sanitize_token(evt->mode, sizeof(evt->mode), mode);
  diag_sanitize_token(evt->result, sizeof(evt->result), result);
  diag_sanitize_token(evt->stage, sizeof(evt->stage), stage);
  evt->code = code;
  evt->ts_ms = millis();
  g_action_event_head = (uint8_t)((g_action_event_head + 1) % ACTION_LOG_SIZE);
  if (g_action_event_count < ACTION_LOG_SIZE) {
    g_action_event_count++;
  }
}

// ── Camera timeline functions ────────────────────────────────────────

static void camera_timeline_event(const char* label, int32_t code) {
  if (!label || !label[0]) {
    return;
  }
  CameraTimelineEvent* evt = &g_camera_events[g_camera_event_head];
  diag_sanitize_token(evt->label, sizeof(evt->label), label);
  evt->code = code;
  evt->ts_ms = millis();
  g_camera_event_head = (uint8_t)((g_camera_event_head + 1) % CAMERA_TIMELINE_LOG_SIZE);
  if (g_camera_event_count < CAMERA_TIMELINE_LOG_SIZE) {
    g_camera_event_count++;
  }
}

static void camera_timeline_reset(const char* mode) {
  g_camera_event_head = 0;
  g_camera_event_count = 0;
  g_camera_timeline_start_ms = millis();
  g_camera_timeline_last_ms = g_camera_timeline_start_ms;
  g_camera_timeline_total_ms = 0;
  g_camera_timeline_len = 0;
  g_camera_timeline_w = 0;
  g_camera_timeline_h = 0;
  g_camera_timeline_ok = false;
  g_camera_timeline_done = false;
  diag_sanitize_token(g_camera_timeline_mode, sizeof(g_camera_timeline_mode), mode);
  camera_timeline_event("start", 0);
}

static const char* camera_timeline_build() {
  static char buf[640];
  buf[0] = '\0';
  if (g_camera_event_count == 0) {
    return buf;
  }
  long age_ms = (g_camera_timeline_last_ms > 0)
                ? (long)(millis() - g_camera_timeline_last_ms)
                : -1;
  int wrote = snprintf(buf, sizeof(buf),
                       "mode=%s ok=%d len=%lu size=%ux%u total_ms=%lu age_ms=%ld events=",
                       g_camera_timeline_mode[0] ? g_camera_timeline_mode : "-",
                       g_camera_timeline_ok ? 1 : 0,
                       (unsigned long)g_camera_timeline_len,
                       (unsigned)g_camera_timeline_w,
                       (unsigned)g_camera_timeline_h,
                       (unsigned long)g_camera_timeline_total_ms,
                       age_ms);
  if (wrote <= 0) {
    return buf;
  }
  size_t len = (size_t)wrote;
  if (len >= sizeof(buf)) {
    return buf;
  }
  uint8_t start_idx = (uint8_t)((g_camera_event_head + CAMERA_TIMELINE_LOG_SIZE -
                                 g_camera_event_count) % CAMERA_TIMELINE_LOG_SIZE);
  for (uint8_t i = 0; i < g_camera_event_count; ++i) {
    const CameraTimelineEvent* evt =
        &g_camera_events[(uint8_t)((start_idx + i) % CAMERA_TIMELINE_LOG_SIZE)];
    long delta_ms = (g_camera_timeline_start_ms > 0 && evt->ts_ms >= g_camera_timeline_start_ms)
                    ? (long)(evt->ts_ms - g_camera_timeline_start_ms)
                    : -1;
    size_t remaining = (len < sizeof(buf)) ? (sizeof(buf) - len - 1) : 0;
    if (remaining < 8) {
      break;
    }
    int wrote_evt = snprintf(buf + len, remaining + 1,
                             "%s%s|%ld|%ld",
                             (i > 0) ? ";" : "",
                             evt->label[0] ? evt->label : "-",
                             delta_ms,
                             static_cast<long>(evt->code));
    if (wrote_evt <= 0 || (size_t)wrote_evt > remaining) {
      break;
    }
    len += (size_t)wrote_evt;
  }
  return buf;
}

// ── Action log formatter ─────────────────────────────────────────────

static const char* diag_build_action_log() {
  static char buf[384];
  buf[0] = '\0';
  if (g_action_event_count == 0) {
    return buf;
  }
  for (uint8_t i = 0; i < g_action_event_count; ++i) {
    uint8_t idx = (uint8_t)((g_action_event_head + ACTION_LOG_SIZE - 1 - i) % ACTION_LOG_SIZE);
    const DiagActionEvent* evt = &g_action_events[idx];
    long age_ms = (evt->ts_ms > 0) ? (long)(millis() - evt->ts_ms) : -1;
    size_t len = strlen(buf);
    size_t remaining = (len < sizeof(buf)) ? (sizeof(buf) - len - 1) : 0;
    if (remaining < 8) {
      break;
    }
    int wrote = snprintf(buf + len, remaining + 1,
                         "%s%s|%s|%s|%s|%ld|%ld",
                         (len > 0) ? ";" : "",
                         evt->action[0] ? evt->action : "-",
                         evt->mode[0] ? evt->mode : "-",
                         evt->result[0] ? evt->result : "-",
                         evt->stage[0] ? evt->stage : "-",
                         static_cast<long>(evt->code),
                         age_ms);
    if (wrote <= 0 || (size_t)wrote > remaining) {
      break;
    }
  }
  return buf;
}

// ── Camera diag label ────────────────────────────────────────────────

static const char* camera_diag_label() {
  return g_camera_timeline_mode[0] ? g_camera_timeline_mode : "camera";
}

// ── Public getters (called from halo_sense_prod wrapper) ─────────────

const char* sense_get_recent_action_log() {
  return diag_build_action_log();
}

const char* sense_get_camera_timeline() {
  return camera_timeline_build();
}

const char* sense_get_last_stage() {
  return g_rtc_last_stage;
}

int32_t sense_get_last_stage_code() {
  return g_rtc_last_stage_code;
}

int32_t sense_get_last_stage_uptime_ms() {
  return static_cast<int32_t>(g_rtc_last_stage_uptime_ms);
}

int32_t sense_get_prev_clean_shutdown() {
  return g_prev_clean_shutdown ? 1 : 0;
}

uint32_t sense_get_crash_count() {
  return g_rtc_crash_count;
}

const char* sense_get_last_crash_stage() {
  return g_rtc_last_crash_stage;
}

int32_t sense_get_last_crash_stage_code() {
  return g_rtc_last_crash_stage_code;
}

int32_t sense_get_last_crash_stage_uptime_ms() {
  return static_cast<int32_t>(g_rtc_last_crash_stage_uptime_ms);
}

int32_t sense_get_last_crash_reason() {
  return static_cast<int32_t>(g_rtc_last_crash_reason);
}

int32_t sense_get_last_crash_wake_cause() {
  return static_cast<int32_t>(g_rtc_last_crash_wake_cause);
}

// ── Boot / reset diagnostics ─────────────────────────────────────────

static const char* reset_reason_label(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "ESP_RST_POWERON";
    case ESP_RST_EXT:
      return "ESP_RST_EXT";
    case ESP_RST_SW:
      return "ESP_RST_SW";
    case ESP_RST_PANIC:
      return "ESP_RST_PANIC";
    case ESP_RST_INT_WDT:
      return "ESP_RST_INT_WDT";
    case ESP_RST_TASK_WDT:
      return "ESP_RST_TASK_WDT";
    case ESP_RST_WDT:
      return "ESP_RST_WDT";
    case ESP_RST_DEEPSLEEP:
      return "ESP_RST_DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "ESP_RST_BROWNOUT";
    case ESP_RST_SDIO:
      return "ESP_RST_SDIO";
    case ESP_RST_UNKNOWN:
    default:
      return "ESP_RST_UNKNOWN";
  }
}

static const char* wake_cause_label(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "UNDEFINED";
    default:
      return "OTHER";
  }
}

static void print_wakeup_diagnostics(const char* board_name) {
  Serial.begin(115200);
  delay(10);
  esp_reset_reason_t reset_reason = esp_reset_reason();
  esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  int wake_pin_level = gpio_get_level(WAKE_GPIO);
  Serial.printf("[BOOT] board=%s\n", board_name ? board_name : "unknown");
  Serial.printf("[BOOT] reset_reason=%s\n", reset_reason_label(reset_reason));
  Serial.printf("[BOOT] wake_cause=%s\n", wake_cause_label(wake_cause));
  Serial.printf("[BOOT] wake_pin_level=%d\n", wake_pin_level);
}

#endif // SENSE_DIAG_H

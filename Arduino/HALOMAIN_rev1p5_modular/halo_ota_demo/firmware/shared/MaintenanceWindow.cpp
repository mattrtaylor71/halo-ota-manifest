#include "MaintenanceWindow.h"
#include "Log.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>

static const char* NVS_NAMESPACE = "mw";
static const char* KEY_SCHEDULED = "sched";
static const char* KEY_START = "start";
static const char* KEY_DURATION = "dur";
static const char* KEY_REQUEST_ID = "req_id";
static const char* KEY_GRACE_BEFORE = "g_bef";
static const char* KEY_GRACE_AFTER = "g_aft";

static bool s_pending_sync_to_lcd = false;

MaintenanceWindow::MaintenanceWindow() {
  scheduled = false;
  start_epoch = 0;
  duration_sec = 600;
  request_id[0] = '\0';
  grace_before_sec = 60;
  grace_after_sec = 600;
}

bool maintenance_schedule_pending_sync_to_lcd() {
  return s_pending_sync_to_lcd;
}

void set_maintenance_schedule_pending_sync_to_lcd(bool value) {
  s_pending_sync_to_lcd = value;
}

bool MaintenanceWindow::loadFromNvs() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return false;
  }
  scheduled = prefs.getBool(KEY_SCHEDULED, false);
  if (!scheduled) {
    prefs.end();
    return false;
  }
  start_epoch = prefs.getULong64(KEY_START, 0);
  duration_sec = prefs.getUInt(KEY_DURATION, 600);
  prefs.getString(KEY_REQUEST_ID, request_id, sizeof(request_id));
  grace_before_sec = prefs.getUInt(KEY_GRACE_BEFORE, 60);
  grace_after_sec = prefs.getUInt(KEY_GRACE_AFTER, 600);
  prefs.end();
  return true;
}

void MaintenanceWindow::saveToNvs() const {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return;
  }
  prefs.putBool(KEY_SCHEDULED, scheduled);
  prefs.putULong64(KEY_START, start_epoch);
  prefs.putUInt(KEY_DURATION, duration_sec);
  prefs.putString(KEY_REQUEST_ID, request_id);
  prefs.putUInt(KEY_GRACE_BEFORE, grace_before_sec);
  prefs.putUInt(KEY_GRACE_AFTER, grace_after_sec);
  prefs.end();
}

void MaintenanceWindow::clear() {
  scheduled = false;
  start_epoch = 0;
  duration_sec = 600;
  request_id[0] = '\0';
  grace_before_sec = 60;
  grace_after_sec = 600;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.clear();
    prefs.end();
  }
}

bool MaintenanceWindow::isWithinWindow(uint64_t now_epoch) const {
  if (!scheduled || start_epoch == 0) {
    return false;
  }
  uint64_t window_start = (grace_before_sec >= start_epoch) ? 0 : (start_epoch - grace_before_sec);
  uint64_t window_end = start_epoch + duration_sec + grace_after_sec;
  return (now_epoch >= window_start && now_epoch <= window_end);
}

uint64_t MaintenanceWindow::nextWakeEpoch(uint64_t now_epoch) const {
  if (!scheduled || start_epoch == 0) {
    return 0;
  }
  uint64_t wake = (grace_before_sec >= start_epoch) ? start_epoch : (start_epoch - grace_before_sec);
  if (wake <= now_epoch + 5) {
    return 0;
  }
  return wake;
}

uint64_t MaintenanceWindow::nextWakeEpochForSleep(uint64_t now_epoch, uint32_t wake_early_sec, uint32_t min_wake_lead_sec) const {
  if (!scheduled || start_epoch == 0 || hasExpired(now_epoch)) {
    return 0;
  }
  uint64_t wake_by_schedule = (wake_early_sec >= start_epoch) ? start_epoch : (start_epoch - wake_early_sec);
  uint64_t min_wake = now_epoch + (uint64_t)min_wake_lead_sec;
  uint64_t wake = (wake_by_schedule > min_wake) ? wake_by_schedule : min_wake;
  if (wake <= now_epoch + 5) {
    return 0;
  }
  return wake;
}

bool MaintenanceWindow::shouldEnterMaintenanceMode(uint64_t now_epoch) const {
  return isWithinWindow(now_epoch);
}

bool MaintenanceWindow::hasExpired(uint64_t now_epoch) const {
  if (!scheduled || start_epoch == 0) {
    return true;
  }
  return now_epoch > (start_epoch + duration_sec + grace_after_sec);
}

bool MaintenanceWindow::setFromJson(const char* json, size_t json_len, uint64_t now_epoch, uint32_t min_future_sec) {
  if (!json || json_len == 0) {
    return false;
  }
  DynamicJsonDocument doc(768);
  DeserializationError err = deserializeJson(doc, json, json_len);
  if (err) {
    LOG_WARN("[MAINT_WINDOW] parse error: %s", err.c_str());
    return false;
  }
  JsonObject obj = doc.as<JsonObject>();
  if (obj.isNull()) {
    return false;
  }
  bool enabled = obj["enabled"] | false;
  if (!enabled) {
    LOG_INFO("[MAINT_WINDOW] maintenance disabled in payload");
    return false;
  }
  uint64_t start = obj["start_epoch"] | 0ULL;
  if (start == 0) {
    LOG_WARN("[MAINT_WINDOW] missing or zero start_epoch");
    return false;
  }
  const char* rid = obj["request_id"] | "";
  uint32_t dur = obj["duration_sec"] | 600u;
  uint32_t g_bef = obj["grace_before_sec"] | 60u;
  uint32_t g_aft = obj["grace_after_sec"] | 600u;
  if (start < now_epoch + min_future_sec) {
    uint64_t window_end = start + dur + g_aft;
    if (now_epoch <= window_end) {
      LOG_INFO("[MAINT_WINDOW] start_epoch=%llu now=%llu min_future=%lu - late but within window, accepting",
               (unsigned long long)start, (unsigned long long)now_epoch, (unsigned long)min_future_sec);
    } else {
      LOG_INFO("[MAINT_WINDOW] start_epoch=%llu now=%llu min_future=%lu - too soon, ignoring",
               (unsigned long long)start, (unsigned long long)now_epoch, (unsigned long)min_future_sec);
      return false;
    }
  }

  scheduled = true;
  start_epoch = start;
  duration_sec = dur;
  grace_before_sec = g_bef;
  grace_after_sec = g_aft;
  strncpy(request_id, rid ? rid : "", sizeof(request_id) - 1);
  request_id[sizeof(request_id) - 1] = '\0';

  LOG_INFO("[MAINT_WINDOW] stored start_epoch=%llu duration_sec=%lu request_id=%s grace_before=%lu grace_after=%lu",
           (unsigned long long)start_epoch, (unsigned long)duration_sec, request_id,
           (unsigned long)grace_before_sec, (unsigned long)grace_after_sec);
  return true;
}

bool MaintenanceWindow::setFromJson(JsonObject& obj, uint64_t now_epoch, uint32_t min_future_sec) {
  bool enabled = obj["enabled"] | false;
  if (!enabled) {
    LOG_INFO("[MAINT_WINDOW] maintenance disabled in payload");
    return false;
  }
  uint64_t start = obj["start_epoch"] | 0ULL;
  if (start == 0) {
    LOG_WARN("[MAINT_WINDOW] missing or zero start_epoch");
    return false;
  }
  const char* rid = obj["request_id"] | "";
  uint32_t dur = obj["duration_sec"] | 600u;
  uint32_t g_bef = obj["grace_before_sec"] | 60u;
  uint32_t g_aft = obj["grace_after_sec"] | 600u;
  if (start < now_epoch + min_future_sec) {
    uint64_t window_end = start + dur + g_aft;
    if (now_epoch <= window_end) {
      LOG_INFO("[MAINT_WINDOW] start_epoch=%llu now=%llu min_future=%lu - late but within window, accepting",
               (unsigned long long)start, (unsigned long long)now_epoch, (unsigned long)min_future_sec);
    } else {
      LOG_INFO("[MAINT_WINDOW] start_epoch=%llu now=%llu min_future=%lu - too soon, ignoring",
               (unsigned long long)start, (unsigned long long)now_epoch, (unsigned long)min_future_sec);
      return false;
    }
  }

  scheduled = true;
  start_epoch = start;
  duration_sec = dur;
  grace_before_sec = g_bef;
  grace_after_sec = g_aft;
  strncpy(request_id, rid ? rid : "", sizeof(request_id) - 1);
  request_id[sizeof(request_id) - 1] = '\0';

  LOG_INFO("[MAINT_WINDOW] stored start_epoch=%llu duration_sec=%lu request_id=%s grace_before=%lu grace_after=%lu",
           (unsigned long long)start_epoch, (unsigned long)duration_sec, request_id,
           (unsigned long)grace_before_sec, (unsigned long)grace_after_sec);
  return true;
}

bool MaintenanceWindow::storeFromWire(const char* json, size_t json_len) {
  if (!json || json_len == 0) return false;
  DynamicJsonDocument doc(768);
  DeserializationError err = deserializeJson(doc, json, json_len);
  if (err) return false;
  JsonObject obj = doc.as<JsonObject>();
  if (obj.isNull()) return false;
  uint64_t start = obj["start_epoch"] | 0ULL;
  if (start == 0) return false;
  const char* rid = obj["request_id"] | "";
  uint32_t dur = obj["duration_sec"] | 600u;
  uint32_t g_bef = obj["grace_before_sec"] | 60u;
  uint32_t g_aft = obj["grace_after_sec"] | 600u;
  scheduled = true;
  start_epoch = start;
  duration_sec = dur;
  grace_before_sec = g_bef;
  grace_after_sec = g_aft;
  strncpy(request_id, rid ? rid : "", sizeof(request_id) - 1);
  request_id[sizeof(request_id) - 1] = '\0';
  return true;
}

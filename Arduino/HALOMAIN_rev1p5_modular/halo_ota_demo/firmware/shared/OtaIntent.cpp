#include "OtaIntent.h"

#include "BuildInfo.h"
#include "Log.h"
#include "Version.h"

#include <Preferences.h>
#include <time.h>

// Extern OTA request flag from sense_ota_demo.ino
extern volatile bool mqtt_ota_check_requested;

namespace {
  static const char* kNvsNamespace = "halo_ota_intent";
  static const char* kKeySense = "desired_sense_version";
  static const char* kKeyLcd = "desired_lcd_version";
  static const char* kKeyForce = "desired_force";
  static const char* kKeyAllowDowngrade = "allow_downgrade";
  static const char* kKeyTs = "desired_ts";
  static const char* kKeyReason = "desired_reason";
  static const char* kKeyLastResult = "last_ota_result";

  static char s_desired_sense[16] = {0};
  static char s_desired_lcd[16] = {0};
  static bool s_desired_force = false;
  static bool s_desired_allow_downgrade = false;
  static uint32_t s_desired_ts = 0;
  static char s_desired_reason[64] = {0};
  static uint32_t s_desired_received_ms = 0;

  static uint32_t s_last_attempt_ms = 0;
  static bool s_attempt_recorded = false;
  static char s_last_result[16] = {0};

  static bool desired_indicates_update() {
    if (s_desired_force) {
      return true;
    }
    if (s_desired_sense[0] == '\0') {
      return false;
    }
    Version desired(s_desired_sense);
    Version current(kFirmwareVersion);
    return desired > current;
  }

  static bool cooldown_allows() {
    if (s_desired_force) {
      return true;
    }
    if (!s_attempt_recorded) {
      return true;
    }
    // millis() is meaningful only within this boot. Unsigned subtraction also
    // keeps the same-boot cooldown correct across the millis() wrap.
    return (uint32_t)(millis() - s_last_attempt_ms) >= 600000UL;  // 10 minutes
  }

  static void save_string(Preferences& prefs, const char* key, const char* value) {
    if (!value) {
      prefs.remove(key);
      return;
    }
    prefs.putString(key, value);
  }
}  // namespace

void OtaIntent::init() {
  // Every deep-sleep/OTA boot gets a fresh discovery opportunity. Never compare
  // this boot's uptime with an NVS uptime recorded by a different boot.
  s_last_attempt_ms = 0;
  s_attempt_recorded = false;
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) {
    return;
  }
  prefs.getString(kKeySense, s_desired_sense, sizeof(s_desired_sense));
  prefs.getString(kKeyLcd, s_desired_lcd, sizeof(s_desired_lcd));
  s_desired_force = prefs.getBool(kKeyForce, false);
  s_desired_allow_downgrade = prefs.getBool(kKeyAllowDowngrade, false);
  s_desired_ts = prefs.getUInt(kKeyTs, 0);
  prefs.getString(kKeyReason, s_desired_reason, sizeof(s_desired_reason));
  prefs.getString(kKeyLastResult, s_last_result, sizeof(s_last_result));
  prefs.end();

  // Force-only intents from manual/force_now triggers should not survive a
  // deep-sleep reboot and block idle sleep forever outside a maintenance window.
  if (s_desired_force &&
      s_desired_sense[0] == '\0' &&
      s_desired_lcd[0] == '\0') {
    clearForceAndCheck();
    LOG_INFO("[OTA_INTENT] cleared stale force-only intent on boot");
  }
}

void OtaIntent::updateDesired(const char* sense_ver,
                              const char* lcd_ver,
                              bool force,
                              bool allow_downgrade,
                              uint32_t ts,
                              const char* reason) {
  if (sense_ver) {
    strncpy(s_desired_sense, sense_ver, sizeof(s_desired_sense) - 1);
    s_desired_sense[sizeof(s_desired_sense) - 1] = '\0';
  } else {
    s_desired_sense[0] = '\0';
  }

  if (lcd_ver) {
    strncpy(s_desired_lcd, lcd_ver, sizeof(s_desired_lcd) - 1);
    s_desired_lcd[sizeof(s_desired_lcd) - 1] = '\0';
  } else {
    s_desired_lcd[0] = '\0';
  }

  s_desired_force = force;
  s_desired_allow_downgrade = allow_downgrade;
  s_desired_ts = ts;

  if (reason) {
    strncpy(s_desired_reason, reason, sizeof(s_desired_reason) - 1);
    s_desired_reason[sizeof(s_desired_reason) - 1] = '\0';
  } else {
    s_desired_reason[0] = '\0';
  }

  s_desired_received_ms = millis();

  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    save_string(prefs, kKeySense, s_desired_sense);
    save_string(prefs, kKeyLcd, s_desired_lcd);
    prefs.putBool(kKeyForce, s_desired_force);
    prefs.putBool(kKeyAllowDowngrade, s_desired_allow_downgrade);
    prefs.putUInt(kKeyTs, s_desired_ts);
    save_string(prefs, kKeyReason, s_desired_reason);
    prefs.end();
  }

  int32_t age_s = getDesiredAgeS();
  LOG_INFO("[MQTT_DESIRED] parsed desired_sense=%s desired_force=%d allow_downgrade=%d age_s=%ld",
           s_desired_sense[0] ? s_desired_sense : "-",
           s_desired_force ? 1 : 0,
           s_desired_allow_downgrade ? 1 : 0,
           static_cast<long>(age_s));

  bool update = desired_indicates_update();
  if (update) {
    mqtt_ota_check_requested = true;
    const char* reason_str = s_desired_force ? "force" : "desired";
    LOG_INFO("[OTA_INTENT] reason=%s result=1", reason_str);
  } else {
    LOG_INFO("[OTA_INTENT] reason=desired result=0");
  }
}

void OtaIntent::clearDesired() {
  // Clear in-memory state
  s_desired_sense[0] = '\0';
  s_desired_lcd[0] = '\0';
  s_desired_force = false;
  s_desired_allow_downgrade = false;
  s_desired_ts = 0;
  s_desired_reason[0] = '\0';
  s_desired_received_ms = 0;
  
  // Clear from NVS
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.remove(kKeySense);
    prefs.remove(kKeyLcd);
    prefs.remove(kKeyForce);
    prefs.remove(kKeyAllowDowngrade);
    prefs.remove(kKeyTs);
    prefs.remove(kKeyReason);
    prefs.end();
  }
  
  // Clear the MQTT OTA check flag
  mqtt_ota_check_requested = false;
  
  LOG_INFO("[OTA_INTENT] Cleared desired OTA intent");
}

bool OtaIntent::shouldUpdateNow() {
  bool update = mqtt_ota_check_requested || desired_indicates_update();
  if (!update) {
    return false;
  }
  if (!cooldown_allows()) {
    return false;
  }
  return true;
}

bool OtaIntent::cooldownAllows() {
  return cooldown_allows();
}

void OtaIntent::recordOtaAttempt(const char* result) {
  s_last_attempt_ms = millis();
  s_attempt_recorded = true;
  if (result) {
    strncpy(s_last_result, result, sizeof(s_last_result) - 1);
    s_last_result[sizeof(s_last_result) - 1] = '\0';
  }
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    if (result) {
      prefs.putString(kKeyLastResult, s_last_result);
    }
    prefs.end();
  }
}

void OtaIntent::recordOtaResult(const char* result) {
  if (!result) {
    return;
  }
  strncpy(s_last_result, result, sizeof(s_last_result) - 1);
  s_last_result[sizeof(s_last_result) - 1] = '\0';
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.putString(kKeyLastResult, s_last_result);
    prefs.end();
  }
}

void OtaIntent::markNoUpdateNeeded() {
  bool changed = false;
  if (mqtt_ota_check_requested) {
    mqtt_ota_check_requested = false;
    changed = true;
  }

  if (s_desired_force) {
    bool clear_force = false;
    if (s_desired_sense[0] == '\0') {
      clear_force = true;
    } else {
      Version desired(s_desired_sense);
      Version current(kFirmwareVersion);
      clear_force = !(desired > current);
    }
    if (clear_force) {
      s_desired_force = false;
      changed = true;
    }
  }
  if (s_desired_allow_downgrade) {
    s_desired_allow_downgrade = false;
    changed = true;
  }

  if (!changed) {
    return;
  }

  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.putBool(kKeyForce, s_desired_force);
    prefs.putBool(kKeyAllowDowngrade, s_desired_allow_downgrade);
    prefs.end();
  }
  LOG_INFO("[OTA_INTENT] cleared force/check (no update)");
}

void OtaIntent::clearForceAndCheck() {
  bool changed = false;
  if (mqtt_ota_check_requested) {
    mqtt_ota_check_requested = false;
    changed = true;
  }
  if (s_desired_force) {
    s_desired_force = false;
    changed = true;
  }
  if (s_desired_allow_downgrade) {
    s_desired_allow_downgrade = false;
    changed = true;
  }
  if (!changed) {
    return;
  }
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.putBool(kKeyForce, s_desired_force);
    prefs.putBool(kKeyAllowDowngrade, s_desired_allow_downgrade);
    prefs.end();
  }
  LOG_INFO("[OTA_INTENT] cleared force/check (attempt complete)");
}

const char* OtaIntent::getDesiredSense() {
  return s_desired_sense;
}

const char* OtaIntent::getDesiredLcd() {
  return s_desired_lcd;
}

bool OtaIntent::getDesiredForce() {
  return s_desired_force;
}

bool OtaIntent::getAllowDowngrade() {
  return s_desired_allow_downgrade;
}

int32_t OtaIntent::getDesiredAgeS() {
  if (s_desired_ts == 0) {
    return -1;
  }
  time_t now_s = time(nullptr);
  if (now_s < 1609459200) {  // 2021-01-01
    return -1;
  }
  if (now_s < static_cast<time_t>(s_desired_ts)) {
    return -1;
  }
  return static_cast<int32_t>(now_s - s_desired_ts);
}

bool OtaIntent::getOtaIntentActive() {
  return shouldUpdateNow();
}

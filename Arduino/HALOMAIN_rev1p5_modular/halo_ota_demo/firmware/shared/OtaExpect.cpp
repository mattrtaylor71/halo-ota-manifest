#include "OtaExpect.h"

#include <Preferences.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include "Log.h"

// NVS namespace and keys
static const char* OTA_EXPECT_NAMESPACE    = "ota_expect";
static const char* KEY_PENDING             = "pending";
static const char* KEY_EXPECTED_VERSION    = "exp_ver";
static const char* KEY_PREV_LABEL          = "prev_lbl";
static const char* KEY_PREV_ADDRESS        = "prev_addr";
static const char* KEY_LAST_ERROR          = "last_err";
static const char* KEY_LAST_SUCCESS_TS    = "last_ok_ts";

#define LOG_TAG_OTA_EXPECT "OTA_EXPECT"

namespace OtaExpect {

bool setPending(const char* expected_version, const esp_partition_t* prev_running) {
  if (!expected_version || !prev_running) {
    LOG_ERROR_TAG(LOG_TAG_OTA_EXPECT, "setPending: invalid args (expected_version or prev_running null)");
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, false)) {
    LOG_ERROR_TAG(LOG_TAG_OTA_EXPECT, "setPending: failed to open NVS namespace");
    return false;
  }

  // Write values
  prefs.putBool(KEY_PENDING, true);
  prefs.putString(KEY_EXPECTED_VERSION, expected_version);
  prefs.putString(KEY_PREV_LABEL, prev_running->label ? prev_running->label : "");
  prefs.putUInt(KEY_PREV_ADDRESS, prev_running->address);

  prefs.end();

  LOG_INFO_TAG(LOG_TAG_OTA_EXPECT,
               "setPending: expected_version=%s, prev_label=%s, prev_addr=0x%x",
               expected_version,
               prev_running->label ? prev_running->label : "",
               prev_running->address);

  return true;
}

bool isPending() {
  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, true)) {
    // Read-only open failure - treat as no pending expectation
    return false;
  }
  bool pending = prefs.getBool(KEY_PENDING, false);
  prefs.end();
  return pending;
}

bool getExpectedVersion(char* out, size_t out_sz) {
  if (!out || out_sz == 0) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, true)) {
    return false;
  }

  String stored = prefs.getString(KEY_EXPECTED_VERSION, "");
  prefs.end();

  if (stored.length() == 0) {
    out[0] = '\0';
    return false;
  }

  stored.toCharArray(out, out_sz);
  return true;
}

bool getPrevPartitionInfo(char* label_out, size_t label_out_sz, uint32_t& address_out) {
  if (!label_out || label_out_sz == 0) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, true)) {
    return false;
  }

  String label = prefs.getString(KEY_PREV_LABEL, "");
  uint32_t addr = prefs.getUInt(KEY_PREV_ADDRESS, 0);
  prefs.end();

  if (label.length() == 0 && addr == 0) {
    label_out[0] = '\0';
    address_out = 0;
    return false;
  }

  label.toCharArray(label_out, label_out_sz);
  address_out = addr;
  return true;
}

bool clearPending() {
  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, false)) {
    return false;
  }

  prefs.putBool(KEY_PENDING, false);
  prefs.putString(KEY_EXPECTED_VERSION, "");
  prefs.putString(KEY_PREV_LABEL, "");
  prefs.putUInt(KEY_PREV_ADDRESS, 0);
  prefs.putString(KEY_LAST_ERROR, "");  // Clear last error; last_success_ts is left for diagnostics
  prefs.end();

  LOG_INFO_TAG(LOG_TAG_OTA_EXPECT, "clearPending: cleared OTA expectation");
  return true;
}

void setLastError(const char* error) {
  if (!error) return;
  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, false)) return;
  size_t len = strlen(error);
  if (len >= LAST_ERROR_MAX_LEN) len = LAST_ERROR_MAX_LEN - 1;
  prefs.putString(KEY_LAST_ERROR, String(error).substring(0, len));
  prefs.end();
}

bool getLastError(char* out, size_t out_sz) {
  if (!out || out_sz == 0) return false;
  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, true)) { out[0] = '\0'; return false; }
  String s = prefs.getString(KEY_LAST_ERROR, "");
  prefs.end();
  s.toCharArray(out, out_sz);
  return (s.length() > 0);
}

void setLastSuccessTs(uint32_t ts_sec) {
  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, false)) return;
  prefs.putUInt(KEY_LAST_SUCCESS_TS, ts_sec);
  prefs.end();
}

uint32_t getLastSuccessTs() {
  Preferences prefs;
  if (!prefs.begin(OTA_EXPECT_NAMESPACE, true)) return 0;
  uint32_t ts = prefs.getUInt(KEY_LAST_SUCCESS_TS, 0);
  prefs.end();
  return ts;
}

}  // namespace OtaExpect


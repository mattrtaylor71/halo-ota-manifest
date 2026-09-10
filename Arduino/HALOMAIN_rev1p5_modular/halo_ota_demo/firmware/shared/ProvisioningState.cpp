#include "ProvisioningState.h"
#include "Log.h"
#include <Preferences.h>
#include <nvs.h>
#include <string.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <stdlib.h>  // For rand/srand

#define NVS_NAMESPACE "halo_prov"
#define KEY_PROVISIONED "prov"
#define KEY_HOME_SSID "home_ssid"
#define KEY_HOME_PASSWORD "home_pass"
#define KEY_AP_SSID "ap_ssid"
#define KEY_AP_PASSWORD "ap_pass"
#define KEY_OWNER_ID "owner_id"
#define KEY_OWNER_CODE "owner_code"
// POSIX TZ string from the backend, e.g. "EST5EDT,M3.2.0,M11.1.0". Without it the
// device falls back to HALO_DEFAULT_TZ (US Pacific), which puts the nightly 02:00
// check at the wrong local hour for everyone else.
#define KEY_TIMEZONE "tz"
#define KEY_STATE "state"

static ProvisioningState::State current_state = ProvisioningState::STATE_UNPROVISIONED;
static Preferences prefs;

void ProvisioningState::init() {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERROR("[PROVISION] Failed to open NVS namespace");
    return;
  }
  
  // Load provisioning flag
  bool provisioned = prefs.getBool(KEY_PROVISIONED, false);
  
  // Load state (default to UNPROVISIONED)
  current_state = (State)prefs.getUChar(KEY_STATE, STATE_UNPROVISIONED);
  
  // Load owner_id for boot log (test verification)
  char owner_id[64];
  bool owner_id_set = false;
  size_t owner_id_len = prefs.getString(KEY_OWNER_ID, owner_id, sizeof(owner_id));
  if (owner_id_len > 0 && owner_id_len < sizeof(owner_id)) {
    owner_id_set = true;
  } else {
    owner_id[0] = '\0';
  }
  
  LOG_INFO("[PROVISION] NVS init: provisioned=%d, state=%s", 
           provisioned ? 1 : 0, getStateString(current_state));
  LOG_INFO("[PROVISION] owner_id=%s, owner_id_set=%d", 
           owner_id_set ? owner_id : "", owner_id_set ? 1 : 0);
  
  prefs.end();
}

ProvisioningState::State ProvisioningState::getState() {
  return current_state;
}

void ProvisioningState::setState(State state) {
  current_state = state;
  
  // Persist state to NVS
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.putUChar(KEY_STATE, (uint8_t)state);
    prefs.end();
  }
  
  LOG_INFO("[PROVISION] State changed to: %s", getStateString(state));
}

const char* ProvisioningState::getStateString(State state) {
  switch (state) {
    case STATE_UNPROVISIONED: return "unprovisioned";
    case STATE_AP_SETUP: return "ap_setup";
    case STATE_CONNECTING_HOME_WIFI: return "connecting";
    case STATE_CONNECTED: return "connected";
    case STATE_ERROR: return "error";
    default: return "unknown";
  }
}

const char* ProvisioningState::getStateString() {
  return getStateString(current_state);
}

bool ProvisioningState::loadHomeWifiCreds(char* ssid, size_t ssid_sz, char* password, size_t password_sz) {
  if (!ssid || ssid_sz == 0) {
    return false;
  }
  if (!prefs.begin(NVS_NAMESPACE, true)) {  // Read-only
    return false;
  }
  
  bool success = true;
  size_t ssid_len = prefs.getString(KEY_HOME_SSID, ssid, ssid_sz);
  size_t pass_len = 0;
  if (password && password_sz > 0) {
    pass_len = prefs.getString(KEY_HOME_PASSWORD, password, password_sz);
  } else {
    String pass_value = prefs.getString(KEY_HOME_PASSWORD, "");
    pass_len = pass_value.length();
  }
  
  if (ssid_len == 0 || ssid_len >= ssid_sz) {
    success = false;
  }
  if (password && password_sz > 0 && pass_len >= password_sz) {
    success = false;
  }
  
  prefs.end();
  return success && ssid_len > 0;
}

void ProvisioningState::saveHomeWifiCreds(const char* ssid, const char* password) {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERROR("[PROVISION] Failed to open NVS for writing");
    return;
  }
  
  prefs.putString(KEY_HOME_SSID, ssid);
  prefs.putString(KEY_HOME_PASSWORD, password);
  prefs.end();
  
  LOG_INFO("[PROVISION] Saved home Wi-Fi credentials: ssid=%s", ssid);
}

void ProvisioningState::clearHomeWifiCreds() {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return;
  }
  
  prefs.remove(KEY_HOME_SSID);
  prefs.remove(KEY_HOME_PASSWORD);
  prefs.end();
}

bool ProvisioningState::loadApCreds(char* ssid, size_t ssid_sz, char* password, size_t password_sz) {
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return false;
  }
  
  bool success = true;
  size_t ssid_len = prefs.getString(KEY_AP_SSID, ssid, ssid_sz);
  size_t pass_len = prefs.getString(KEY_AP_PASSWORD, password, password_sz);
  
  if (ssid_len == 0 || ssid_len >= ssid_sz || pass_len >= password_sz) {
    success = false;
  }
  
  prefs.end();
  return success && ssid_len > 0;
}

void ProvisioningState::saveApCreds(const char* ssid, const char* password) {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERROR("[PROVISION] Failed to open NVS for writing");
    return;
  }
  
  prefs.putString(KEY_AP_SSID, ssid);
  prefs.putString(KEY_AP_PASSWORD, password);
  prefs.end();
  
  LOG_INFO("[PROVISION] Saved AP credentials: ssid=%s", ssid);
}

void ProvisioningState::clearApCreds() {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return;
  }
  
  prefs.remove(KEY_AP_SSID);
  prefs.remove(KEY_AP_PASSWORD);
  prefs.end();
  
  LOG_INFO("[PROVISION] Cleared AP credentials");
}

bool ProvisioningState::isProvisioned() {
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return false;
  }
  
  bool provisioned = prefs.getBool(KEY_PROVISIONED, false);
  prefs.end();
  return provisioned;
}

void ProvisioningState::setProvisioned(bool value) {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERROR("[PROVISION] Failed to open NVS for writing");
    return;
  }
  
  prefs.putBool(KEY_PROVISIONED, value);
  prefs.end();
  
  LOG_INFO("[PROVISION] Set provisioned=%d", value ? 1 : 0);
}

bool ProvisioningState::loadOwnerId(char* owner_id, size_t owner_id_sz) {
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return false;
  }
  
  size_t len = prefs.getString(KEY_OWNER_ID, owner_id, owner_id_sz);
  prefs.end();
  return len > 0 && len < owner_id_sz;
}

void ProvisioningState::saveOwnerId(const char* owner_id) {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERROR("[PROVISION] Failed to open NVS for writing");
    return;
  }
  
  prefs.putString(KEY_OWNER_ID, owner_id);
  prefs.end();
  
  LOG_INFO("[PROVISION] Saved owner_id=%s", owner_id);
}

// All TZ writers use this lock. A consumer may hold it across proof use and
// its dependent commit; the time mutex is always acquired after this lock.
static std::recursive_mutex g_timezone_mutex;
static bool g_timezone_loaded=false;
static ProvisioningState::TimezoneStatus g_timezone_status=ProvisioningState::TimezoneStatus::Unknown;
static char g_timezone_value[64]={};
std::recursive_mutex& ProvisioningState::timezoneMutex(){return g_timezone_mutex;}
static ProvisioningState::TimezoneStatus timezone_read_value(char (&out)[64]) {
  out[0]=0;nvs_handle_t h;
  const esp_err_t opened=nvs_open(NVS_NAMESPACE,NVS_READONLY,&h);
  if(opened==ESP_ERR_NVS_NOT_FOUND)return ProvisioningState::TimezoneStatus::Absent;
  if(opened!=ESP_OK)return ProvisioningState::TimezoneStatus::Unknown;
  size_t needed=0;const esp_err_t sized=nvs_get_str(h,KEY_TIMEZONE,nullptr,&needed);
  if(sized==ESP_ERR_NVS_NOT_FOUND){nvs_close(h);return ProvisioningState::TimezoneStatus::Absent;}
  if(sized!=ESP_OK || needed<2 || needed>sizeof(out)){nvs_close(h);return ProvisioningState::TimezoneStatus::Unknown;}
  size_t actual=needed;const esp_err_t read=nvs_get_str(h,KEY_TIMEZONE,out,&actual);nvs_close(h);
  // A corrupt full read may remove the item and return NOT_FOUND. Once size
  // succeeded that is unknown, never affirmative default-configuration proof.
  if(read!=ESP_OK || actual!=needed || out[needed-1] || memchr(out,0,needed-1))return ProvisioningState::TimezoneStatus::Unknown;
  for(size_t i=0;i+1<needed;++i)if((unsigned char)out[i]<32 || (unsigned char)out[i]>126)return ProvisioningState::TimezoneStatus::Unknown;
  return ProvisioningState::TimezoneStatus::Present;
}
ProvisioningState::TimezoneStatus ProvisioningState::loadTimezoneStatus(char* tz,size_t size) {
  std::lock_guard<std::recursive_mutex> lock(g_timezone_mutex);
  if(!tz || size<64)return TimezoneStatus::Unknown;
  tz[0]=0;
  if(!g_timezone_loaded){g_timezone_loaded=true;g_timezone_status=timezone_read_value(g_timezone_value);}
  if(g_timezone_status==TimezoneStatus::Present)memcpy(tz,g_timezone_value,strlen(g_timezone_value)+1);
  return g_timezone_status;
}
bool ProvisioningState::loadTimezone(char* tz,size_t size) {
  char value[64];const auto status=loadTimezoneStatus(value,sizeof(value));
  if(status!=TimezoneStatus::Present || !tz || strlen(value)+1>size){if(tz&&size)tz[0]=0;return false;}
  memcpy(tz,value,strlen(value)+1);return true;
}
bool ProvisioningState::saveTimezone(const char* tz) {
  std::lock_guard<std::recursive_mutex> lock(g_timezone_mutex);
  if(!tz)return false;const size_t size=strnlen(tz,64);
  if(!size || size>=64)return false;
  for(size_t i=0;i<size;++i)if((unsigned char)tz[i]<32 || (unsigned char)tz[i]>126)return false;
  // A prior read/mutation uncertainty requires normal NVS boot recovery.
  if(g_timezone_loaded && g_timezone_status==TimezoneStatus::Unknown)return false;
  nvs_handle_t h;
  g_timezone_loaded=true;g_timezone_status=TimezoneStatus::Unknown;
  if(nvs_open(NVS_NAMESPACE,NVS_READWRITE,&h)!=ESP_OK)return false;
  const esp_err_t wrote=nvs_set_str(h,KEY_TIMEZONE,tz);
  const esp_err_t committed=wrote==ESP_OK?nvs_commit(h):wrote;nvs_close(h);
  char actual[64]={};
  if(committed!=ESP_OK || timezone_read_value(actual)!=TimezoneStatus::Present || strcmp(actual,tz))return false;
  memcpy(g_timezone_value,actual,size+1);g_timezone_status=TimezoneStatus::Present;
  LOG_INFO("[PROVISION] Timezone persistence verified");return true;
}

void ProvisioningState::clearOwnerId() {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return;
  }
  
  prefs.remove(KEY_OWNER_ID);
  prefs.end();
  
  LOG_INFO("[PROVISION] Cleared owner_id");
}

bool ProvisioningState::loadOwnerCode(char* owner_code, size_t owner_code_sz) {
  if (!prefs.begin(NVS_NAMESPACE, true)) {
    return false;
  }
  
  size_t len = prefs.getString(KEY_OWNER_CODE, owner_code, owner_code_sz);
  prefs.end();
  return len > 0 && len < owner_code_sz;
}

void ProvisioningState::saveOwnerCode(const char* owner_code) {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERROR("[PROVISION] Failed to open NVS for writing");
    return;
  }
  
  prefs.putString(KEY_OWNER_CODE, owner_code);
  prefs.end();
  
  LOG_INFO("[PROVISION] Saved owner_code=%s", owner_code);
}

void ProvisioningState::clearOwnerCode() {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    return;
  }
  
  prefs.remove(KEY_OWNER_CODE);
  prefs.end();
}

void ProvisioningState::getDeviceId(char* device_id, size_t device_id_sz) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_ETH);
  
  // Format: halo-XXXX-YYYY (last 4 MAC bytes as hex)
  snprintf(device_id, device_id_sz, "halo-%02x%02x-%02x%02x", 
           mac[2], mac[3], mac[4], mac[5]);
}

void ProvisioningState::generateApSsid(char* ap_ssid, size_t ap_ssid_sz) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_ETH);

  // Make each setup session unique so phones do not reuse stale cached
  // credentials for the same SSID after the password rotates.
  uint16_t session_suffix = (uint16_t)(esp_random() & 0xFFFF);

  // Format: Trepo-Halo-XXXX-YYYY
  snprintf(ap_ssid, ap_ssid_sz, "Trepo-Halo-%02X%02X-%04X", mac[4], mac[5], session_suffix);
}

void ProvisioningState::generateRandomPassword(char* password, size_t password_sz) {
  if (!password || password_sz < 2) {
    return;
  }

  // Generate a fresh per-setup alphanumeric password.
  const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  const size_t charset_size = sizeof(charset) - 1;

  size_t len = (password_sz - 1 < 12) ? (password_sz - 1) : 12;
  for (size_t i = 0; i < len; i++) {
    uint32_t r = esp_random();
    password[i] = charset[r % charset_size];
  }
  password[len] = '\0';
}

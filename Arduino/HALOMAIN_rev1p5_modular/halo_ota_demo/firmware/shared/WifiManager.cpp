#include "WifiManager.h"
#include "Log.h"
#include "WifiGuard.h"  // WiFi guard to prevent "STA not started" errors
#include "WifiUtils.h"  // hardResetSta to avoid driver "sta is connecting" race
#include <esp_wifi.h>

#define LOG_TAG_WIFI "WIFI"

WifiManager::WifiManager()
  : status(STATUS_IDLE)
  , state(STATE_IDLE)
  , attempt_start_ms(0)
  , timeout_ms(10000)
  , max_retries(3)
  , current_retry(0)
  , retry_delay_ms(2000)
  , backoff_until_ms(0) {
}

WifiManager::~WifiManager() {
  disconnect();
}

void WifiManager::setStatus(Status new_status) {
  if (status != new_status) {
    status = new_status;
    switch (status) {
      case STATUS_IDLE:
        LOG_DEBUG_TAG(LOG_TAG_WIFI, "Status: IDLE");
        break;
      case STATUS_CONNECTING:
        LOG_DEBUG_TAG(LOG_TAG_WIFI, "Status: CONNECTING");
        break;
      case STATUS_CONNECTED:
        LOG_INFO_TAG(LOG_TAG_WIFI, "Status: CONNECTED (IP: %s)", WiFi.localIP().toString().c_str());
        break;
      case STATUS_FAILED:
        LOG_WARN_TAG(LOG_TAG_WIFI, "Status: FAILED");
        break;
    }
  }
}

void WifiManager::transitionTo(InternalState new_state) {
  if (state != new_state) {
    state = new_state;
  }
}

WifiManager::ConnectResult WifiManager::connect(const char* ssid, const char* password, 
                                                 unsigned long timeout_ms, 
                                                 uint8_t max_retries,
                                                 unsigned long retry_delay_ms) {
  if (!ssid || strlen(ssid) == 0) {
    LOG_ERROR_TAG(LOG_TAG_WIFI, "Invalid SSID");
    setStatus(STATUS_FAILED);
    return CONNECT_RESULT_STARTED;  // Invalid but we'll treat as started
  }
  
  // Non-reentrant: if already connecting, return IN_PROGRESS
  if (state == STATE_WAIT || state == STATE_START_ATTEMPT || state == STATE_BACKOFF) {
    LOG_DEBUG_TAG(LOG_TAG_WIFI, "Connection already in progress, ignoring connect() call");
    return CONNECT_RESULT_IN_PROGRESS;
  }
  
  // Reset state for new connection attempt
  this->ssid = ssid;
  this->password = password ? password : "";
  this->timeout_ms = timeout_ms;
  this->max_retries = max_retries;
  this->retry_delay_ms = retry_delay_ms;
  this->current_retry = 0;
  this->backoff_until_ms = 0;
  
  setStatus(STATUS_CONNECTING);
  transitionTo(STATE_START_ATTEMPT);
  
  LOG_INFO_TAG(LOG_TAG_WIFI, "Connecting to: %s (timeout=%lu ms, max_retries=%d)", 
               ssid, timeout_ms, max_retries);
  
  return CONNECT_RESULT_STARTED;
}

void WifiManager::disconnect() {
  // Guard: Only call WiFi.disconnect() if STA is actually started
  // Use safe helper to avoid "STA not started" error on post-OTA reboot
  if (status == STATUS_CONNECTED || status == STATUS_CONNECTING) {
    LOG_INFO_TAG(LOG_TAG_WIFI, "Disconnecting from Wi-Fi");
    
    // Use safe disconnect helper (only disconnects if STA was started)
    HALO_SAFE_DISCONNECT("wifi_disconnect", true);
    
    // Mark WiFi as off since we're disconnecting
    WiFi.mode(WIFI_OFF);
    halo_sta_mark_off();
  }
  setStatus(STATUS_IDLE);
  transitionTo(STATE_IDLE);
  ssid = "";
  password = "";
  current_retry = 0;
  backoff_until_ms = 0;
}

bool WifiManager::isConnected() const {
  return status == STATUS_CONNECTED && WiFi.status() == WL_CONNECTED;
}

const char* WifiManager::getSSID() const {
  if (isConnected()) {
    return WiFi.SSID().c_str();
  }
  return NULL;
}

IPAddress WifiManager::getIP() const {
  if (isConnected()) {
    return WiFi.localIP();
  }
  return IPAddress(0, 0, 0, 0);
}

void WifiManager::update() {
  if (halo_rebooting()) {
    return;
  }
  unsigned long now = millis();
  
  // State machine: IDLE -> START_ATTEMPT -> WAIT -> SUCCESS | FAIL -> BACKOFF -> START_ATTEMPT
  
  switch (state) {
    case STATE_IDLE:
      // Nothing to do
      break;
      
    case STATE_START_ATTEMPT: {
      // Full STA reset per retry to avoid driver "sta is connecting" race / stale state.
      current_retry++;
      LOG_INFO_TAG(LOG_TAG_WIFI, "Starting connection attempt %d/%d", current_retry, max_retries);
      hardResetSta();
      WiFi.begin(ssid.c_str(), password.c_str());
      halo_sta_mark_begin_called();
      attempt_start_ms = now;
      int st = WiFi.status();
      LOG_INFO_TAG(LOG_TAG_WIFI, "Attempt %d: begin done, status=%d mode=STA", current_retry, st);
      transitionTo(STATE_WAIT);
      break;
    }
      
    case STATE_WAIT:
      // Check connection status and timeout
      if (WiFi.status() == WL_CONNECTED) {
        // Success!
        transitionTo(STATE_SUCCESS);
        setStatus(STATUS_CONNECTED);
        LOG_INFO_TAG(LOG_TAG_WIFI, "Connected successfully on attempt %d", current_retry);

        // Disable Wi-Fi power save / sleep to avoid OTA stalls
        WiFi.setSleep(false);
        esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ps_err == ESP_OK) {
          LOG_INFO_TAG(LOG_TAG_WIFI, "Wi-Fi sleep disabled (WIFI_PS_NONE)");
        } else {
          LOG_WARN_TAG(LOG_TAG_WIFI, "Failed to disable Wi-Fi power save: %s", esp_err_to_name(ps_err));
        }

        current_retry = 0;  // Reset for next connection
      } else if (now - attempt_start_ms >= timeout_ms) {
        int st = WiFi.status();
        LOG_WARN_TAG(LOG_TAG_WIFI, "Attempt %d timeout; status=%d", current_retry, st);
        transitionTo(STATE_FAIL);
      }
      break;
      
    case STATE_SUCCESS:
      // Check if connection dropped
      if (WiFi.status() != WL_CONNECTED) {
        LOG_WARN_TAG(LOG_TAG_WIFI, "Wi-Fi connection lost");
        setStatus(STATUS_IDLE);
        transitionTo(STATE_IDLE);
      }
      break;
      
    case STATE_FAIL: {
      int fail_status = WiFi.status();
      LOG_WARN_TAG(LOG_TAG_WIFI, "Connection attempt %d failed (timeout %lu ms) status=%d",
                   current_retry, timeout_ms, fail_status);
      
      WiFi.disconnect(true, true);
      
      if (current_retry >= max_retries) {
        LOG_ERROR_TAG(LOG_TAG_WIFI, "Final failure: %d attempts exhausted, last status=%d",
                     max_retries, fail_status);
        setStatus(STATUS_FAILED);
        transitionTo(STATE_IDLE);
        current_retry = 0;
      } else {
        // Start backoff period before next retry
        backoff_until_ms = now + retry_delay_ms;
        transitionTo(STATE_BACKOFF);
        LOG_INFO_TAG(LOG_TAG_WIFI, "Waiting %lu ms before retry %d/%d...",
                     retry_delay_ms, current_retry + 1, max_retries);
      }
      break;
    }
      
    case STATE_BACKOFF:
      // Wait for backoff period to complete
      if (now >= backoff_until_ms) {
        // Backoff complete - start next attempt
        transitionTo(STATE_START_ATTEMPT);
      }
      break;
  }
}

unsigned long WifiManager::getBudgetMs(unsigned long safety_margin_ms) const {
  if (max_retries == 0) {
    return safety_margin_ms;
  }
  unsigned long retries = (unsigned long)max_retries;
  unsigned long total = timeout_ms * retries;
  if (retries > 1) {
    total += retry_delay_ms * (retries - 1);
  }
  total += safety_margin_ms;
  return total;
}

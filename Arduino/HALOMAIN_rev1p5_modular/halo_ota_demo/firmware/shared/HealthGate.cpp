#include "HealthGate.h"
#include "Log.h"
#include "OtaExpect.h"
#include "BuildFlags.h"
#include <esp_ota_ops.h>

HealthGate::HealthGate() 
  : state(STATE_INIT)
  , boot_time_ms(millis())
  , eligible_uptime_ms(0)
  , rollback_eligible(false)
  , uart_initialized(false)
  , uart_sync_established(false)
  , wifi_connected_once(false)
  , manifest_fetched_once(false)
  , manifest_disabled(false)
  , cloud_ready(false)
  , last_status_log_ms(0)
  , pending_verify(false)
  , marked_valid(false)
  , validation_attempted(false)
  , skip_mark_valid(false) {
}

void HealthGate::update() {
  // Update OTA state (check if we're in PENDING_VERIFY)
  updateOtaState();
  
  if (state == STATE_INIT || state == STATE_ELIGIBLE) {
    checkRollbackCriteria();
    checkCloudReadiness();
  }
  
  // Periodic status logging (every 5 seconds)
  logPeriodicStatus();
  
  // Once valid or failed, state doesn't change (until reset)
}

bool HealthGate::computeRollbackEligible(unsigned long uptime_ms) const {
  // CRITICAL: rollback_eligible can ONLY be true if we're in PENDING_VERIFY state
  if (!pending_verify) {
    return false;  // Force false when not in PENDING_VERIFY
  }
  
  // Check reset reason (must not be panic/wdt)
  esp_reset_reason_t reset_reason = esp_reset_reason();
  bool no_crash = (reset_reason != ESP_RST_PANIC && 
                   reset_reason != ESP_RST_INT_WDT && 
                   reset_reason != ESP_RST_TASK_WDT &&
                   reset_reason != ESP_RST_WDT);
  
  // Criteria for rollback eligibility (only applies when pending_verify==1)
  // Mark-valid needs local uptime/UART health; Wi-Fi and manifest are diagnostic only
  bool uptime_ok = uptime_ms >= ELIGIBILITY_UPTIME_MS;
  bool uart_ok = uart_initialized;  // actual local driver readiness, not peer presence
  bool main_loop_ok = true;  // If we're here, main loop is running
  
  // All criteria must be met AND pending_verify must be true (no manifest required for rollback eligibility)
  return pending_verify && uptime_ok && no_crash && uart_ok && main_loop_ok;
}

void HealthGate::checkRollbackCriteria() {
  unsigned long uptime = millis() - boot_time_ms;
  
  // Compute rollback eligibility using helper function (includes pending_verify check)
  bool new_rollback_eligible = computeRollbackEligible(uptime);
  
  // Force rollback_eligible to false if not in PENDING_VERIFY
  if (!pending_verify) {
    if (rollback_eligible) {
      rollback_eligible = false;
      logState();
    }
    return;  // No rollback logic when not in PENDING_VERIFY
  }
  
  if (new_rollback_eligible != rollback_eligible) {
    rollback_eligible = new_rollback_eligible;
    logState();
    
    if (rollback_eligible && state == STATE_INIT && pending_verify) {
      // Only log "Rollback eligible" when actually in PENDING_VERIFY
      state = STATE_ELIGIBLE;
      eligible_uptime_ms = uptime;
      LOG_INFO_TAG(LOG_TAG_HEALTH, "Rollback eligible - uptime: %lu ms", uptime);
    }
    
    // Don't mark valid here - markValid() will check all criteria
  }
}

void HealthGate::checkCloudReadiness() {
  bool new_cloud_ready = wifi_connected_once && manifest_fetched_once;
  
  if (new_cloud_ready != cloud_ready) {
    cloud_ready = new_cloud_ready;
    logState();
    
    if (cloud_ready) {
      LOG_INFO_TAG(LOG_TAG_HEALTH, "Cloud ready - WiFi and manifest both successful");
    }
  }
}

void HealthGate::logState() {
  // CRITICAL: Always ensure rollback_eligible is false when not in PENDING_VERIFY
  if (!pending_verify) {
    rollback_eligible = false;  // Force to false
  }
  
  // Only log rollback_eligible when pending_verify is true (otherwise it's always 0)
  if (pending_verify) {
    LOG_INFO_TAG(LOG_TAG_HEALTH, "rollback_eligible=%d cloud_ready=%d wifi=%d manifest=%d uart_sync=%d", 
                 rollback_eligible ? 1 : 0, 
                 cloud_ready ? 1 : 0,
                 wifi_connected_once ? 1 : 0,
                 manifest_fetched_once ? 1 : 0,
                 uart_sync_established ? 1 : 0);
  } else {
    // When not pending_verify, always log rollback_eligible=0 and suppress "Rollback eligible" messages
    LOG_INFO_TAG(LOG_TAG_HEALTH, "rollback_eligible=0 cloud_ready=%d wifi=%d manifest=%d uart_sync=%d (not in PENDING_VERIFY)", 
                 cloud_ready ? 1 : 0,
                 wifi_connected_once ? 1 : 0,
                 manifest_fetched_once ? 1 : 0,
                 uart_sync_established ? 1 : 0);
  }
}

void HealthGate::updateOtaState() {
  // A returned rollback failure is terminal for this boot. Unreadable SDK
  // state cannot erase the pending recovery hold.
  if (state == STATE_FAILED) { pending_verify = true; rollback_eligible = false; return; }
  if (validation_attempted && !marked_valid) {
    if (confirmValid()) return;
    pending_verify = true; rollback_eligible = false; return;
  }
  // Determine running partition and its OTA state
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) {
    if (pending_verify) {
      pending_verify = false;
      rollback_eligible = false;  // Clear rollback_eligible when partition is invalid
    }
    return;
  }
  
  esp_ota_img_states_t ota_state;
  esp_err_t ota_err = esp_ota_get_state_partition(running, &ota_state);
  
  // Compute pending_verify: only true if OTA state is PENDING_VERIFY
  bool new_pending_verify = (ota_err == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY);
  
  if (new_pending_verify != pending_verify) {
    bool was_pending = pending_verify;
    pending_verify = new_pending_verify;
    
    // CRITICAL: Immediately clear rollback_eligible when leaving PENDING_VERIFY state
    if (!pending_verify) {
      rollback_eligible = false;  // Force to false
      if (state == STATE_ELIGIBLE) {
        state = STATE_INIT;  // Reset state when leaving PENDING_VERIFY
      }
    }
    
    if (pending_verify) {
      LOG_INFO_TAG(LOG_TAG_HEALTH, "OTA state: PENDING_VERIFY detected (rollback eligible)");
    } else {
      const char* state_str = "UNKNOWN";
      if (ota_err == ESP_OK) {
        switch (ota_state) {
          case ESP_OTA_IMG_NEW: state_str = "NEW"; break;
          case ESP_OTA_IMG_VALID: state_str = "VALID"; break;
          case ESP_OTA_IMG_INVALID: state_str = "INVALID"; break;
          case ESP_OTA_IMG_ABORTED: state_str = "ABORTED"; break;
          default: state_str = "OTHER"; break;
        }
      }
      LOG_INFO_TAG(LOG_TAG_HEALTH, "OTA state: %s (not PENDING_VERIFY - no rollback cancel needed)", state_str);
      
      // Log when OTA state is VALID (throttled to once per state change)
      if (ota_err == ESP_OK && ota_state == ESP_OTA_IMG_VALID) {
        LOG_INFO_TAG(LOG_TAG_HEALTH, "OTA already VALID; no rollback logic active.");
      }
      
      // Throttled log when transitioning away from PENDING_VERIFY
      if (was_pending) {
        LOG_INFO_TAG(LOG_TAG_HEALTH, "Not in PENDING_VERIFY; rollback logic disabled");
      }
    }
  }
  
  // Ensure rollback_eligible is always false when not in PENDING_VERIFY (defensive check)
  if (!pending_verify && rollback_eligible) {
    rollback_eligible = false;
  }
}

void HealthGate::logPeriodicStatus() {
  unsigned long now = millis();
  if ((now - last_status_log_ms) >= STATUS_LOG_INTERVAL_MS) {
    last_status_log_ms = now;
    
    unsigned long uptime = now - boot_time_ms;
    
    // Determine can_mark_valid using method (for consistency)
    bool can_mark_valid = this->canMarkValid();
    
    // Min uptime for mark-valid (OTA_VERIFY_MIN_UPTIME_MS)
    bool stability_ok = (uptime >= OTA_VERIFY_MIN_UPTIME_MS);
    
    if (pending_verify) {
      // Log why can_mark_valid is blocked (if it is) - no manifest required for mark-valid
      if (!can_mark_valid && !marked_valid) {
        const char* blockers[5];
        int blocker_count = 0;
        if (!stability_ok) blockers[blocker_count++] = "uptime";
        if (!hasRuntimeReadiness() && stability_ok && uart_initialized) blockers[blocker_count++] = "local_health";
        if (!uart_initialized) blockers[blocker_count++] = "uart";
        
        char blocker_str[128] = "";
        for (int i = 0; i < blocker_count; i++) {
          if (i > 0) strcat(blocker_str, ",");
          strcat(blocker_str, blockers[i]);
        }
        
        LOG_INFO_TAG(LOG_TAG_HEALTH, 
                     "HEALTH: pending_verify=1 rollback_eligible=%d marked_valid=%d uptime=%lu wifi=%d uart=%d -> can_mark_valid=0 (blocked by: %s)",
                     rollback_eligible ? 1 : 0,
                     marked_valid ? 1 : 0,
                     uptime,
                     wifi_connected_once ? 1 : 0,
                     uart_initialized ? 1 : 0,
                     blocker_count > 0 ? blocker_str : "unknown");
      } else {
        LOG_INFO_TAG(LOG_TAG_HEALTH, 
                     "HEALTH: pending_verify=1 rollback_eligible=%d marked_valid=%d uptime=%lu wifi=%d uart=%d -> can_mark_valid=%d",
                     rollback_eligible ? 1 : 0,
                     marked_valid ? 1 : 0,
                     uptime,
                     wifi_connected_once ? 1 : 0,
                     uart_initialized ? 1 : 0,
                     can_mark_valid ? 1 : 0);
      }
    } else {
      // Not in PENDING_VERIFY - force rollback_eligible to false and log status
      rollback_eligible = false;  // Ensure it's false
      
      // Throttled log: only log once per status interval when not in PENDING_VERIFY
      static unsigned long last_not_pending_log_ms = 0;
      if ((now - last_not_pending_log_ms) >= STATUS_LOG_INTERVAL_MS) {
        last_not_pending_log_ms = now;
        LOG_INFO_TAG(LOG_TAG_HEALTH, "Not in PENDING_VERIFY; rollback logic disabled");
      }
      
      LOG_INFO_TAG(LOG_TAG_HEALTH, 
                   "HEALTH: pending_verify=0 rollback_eligible=0 marked_valid=%d uptime=%lu wifi=%d uart=%d -> can_mark_valid=0 (not in PENDING_VERIFY)",
                   marked_valid ? 1 : 0,
                   uptime,
                   wifi_connected_once ? 1 : 0,
                   uart_initialized ? 1 : 0);
    }
    
    // SIMPLE proof uses the same actual SDK validation and local deadline.
    // Connectivity is external readiness, never evidence of corrupt firmware.
    // A locally unhealthy PENDING image remains a rollback candidate. Do not
    // retry the SDK action if it returns (e.g. no usable rollback image).
    if (pending_verify && !marked_valid && state != STATE_FAILED &&
        uptime >= OTA_VERIFY_LOCAL_TIMEOUT_MS && (validation_attempted || !hasRuntimeReadiness())) {
      state = STATE_FAILED;
      OtaExpect::setLastError("local_health_timeout");
      const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
      LOG_ERROR_TAG(LOG_TAG_HEALTH, "Local health rollback returned: %s", esp_err_to_name(err));
    }

    // Try to mark valid if criteria are met (only if not already marked)
    if (can_mark_valid && !marked_valid) {
      markValid();
    }
  }
}

void HealthGate::markValid() {
  if (marked_valid || validation_attempted || state == STATE_FAILED) {
    return;  // Already marked, attempted, or failed locally
  }
  if (skip_mark_valid) {
    LOG_WARN_TAG(LOG_TAG_HEALTH, "markValid() skipped by OTA test flag");
    return;
  }
  
  // CRITICAL: Only mark valid when ALL criteria are met (two-phase OTA verify):
  // 1. We are in PENDING_VERIFY (checked in updateOtaState)
  // 2. uptime >= OTA_VERIFY_MIN_UPTIME_MS (10s)
  // 3. actual local UART driver initialized and no crash reset
  // Wi-Fi, manifest, peer presence and clock are not local firmware health.
  
  if (!pending_verify) {
    LOG_WARN_TAG(LOG_TAG_HEALTH, "markValid() called but pending_verify=false - skipping");
    return;
  }
  
  unsigned long uptime = millis() - boot_time_ms;
  bool stability_ok = (uptime >= OTA_VERIFY_MIN_UPTIME_MS);
  bool wifi_ok = wifi_connected_once;  // diagnostic only
  bool uart_ok = uart_initialized;
  
  bool can_mark_valid = hasRuntimeReadiness();
  
  if (!can_mark_valid) {
    LOG_DEBUG_TAG(LOG_TAG_HEALTH, 
                  "markValid() criteria not met: uptime_ok=%d wifi=%d uart=%d uptime=%lu",
                  stability_ok ? 1 : 0, wifi_ok ? 1 : 0, uart_ok ? 1 : 0, uptime);
    return;
  }
  
  validation_attempted = true;
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* selected = esp_ota_get_boot_partition();
  esp_ota_img_states_t current;
  if (!running || !selected || running->address != selected->address ||
      esp_ota_get_state_partition(running, &current) != ESP_OK ||
      current != ESP_OTA_IMG_PENDING_VERIFY) return;
  // All local criteria met - one SDK mark-valid attempt this boot
  LOG_INFO_TAG(LOG_TAG_HEALTH, 
               "All criteria met: pending_verify=1 uptime>=%lu local_uart=1 -> calling esp_ota_mark_app_valid_cancel_rollback()",
               (unsigned long)OTA_VERIFY_MIN_UPTIME_MS);

  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    if (confirmValid()) {
      LOG_INFO_TAG(LOG_TAG_HEALTH, "Firmware marked valid with current readback - SUCCESS");
    } else {
      LOG_ERROR_TAG(LOG_TAG_HEALTH, "Mark-valid returned OK but VALID readback unresolved");
    }
  } else {
    LOG_ERROR_TAG(LOG_TAG_HEALTH, "Failed to mark firmware valid: %s", esp_err_to_name(err));
  }
}

bool HealthGate::confirmValid() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* selected = esp_ota_get_boot_partition();
  esp_ota_img_states_t current;
  if (!running || !selected || running->address != selected->address ||
      esp_ota_get_state_partition(running, &current) != ESP_OK ||
      current != ESP_OTA_IMG_VALID) return false;
  marked_valid = true; pending_verify = false; rollback_eligible = false;
  state = STATE_VALID;
  // The wrapper services the still-intact expectation after actual VALID.
  return true;
}

void HealthGate::markWifiConnected() {
  if (!wifi_connected_once) {
    wifi_connected_once = true;
    LOG_INFO_TAG(LOG_TAG_HEALTH, "Wi-Fi connected");
  }
}

void HealthGate::markWifiDisconnected() {
  if (wifi_connected_once) {
    wifi_connected_once = false;
  }
}

void HealthGate::markManifestFetched() {
  if (!manifest_fetched_once) {
    manifest_fetched_once = true;
    manifest_disabled = false;  // Clear disabled flag if manifest is fetched
    LOG_INFO_TAG(LOG_TAG_HEALTH, "Manifest fetched");
  }
}

void HealthGate::markManifestDisabled() {
  if (!manifest_disabled) {
    manifest_disabled = true;
    manifest_fetched_once = false;  // Clear fetched flag
    LOG_INFO_TAG(LOG_TAG_HEALTH, "HealthGate: manifest disabled (cloud readiness only)");
  }
}

void HealthGate::markUartInitialized(bool ready) {
  if (uart_initialized != ready) {
    uart_initialized = ready;
    LOG_INFO_TAG(LOG_TAG_HEALTH, "Local UART ready=%d", ready ? 1 : 0);
  }
}

void HealthGate::markUartSyncEstablished() {
  if (!uart_sync_established) {
    uart_sync_established = true;
    LOG_INFO_TAG(LOG_TAG_HEALTH, "UART sync established");
  }
}

bool HealthGate::canMarkValid() const {
  if (!pending_verify || marked_valid || validation_attempted || state == STATE_FAILED) {
    return false;
  }
  return hasRuntimeReadiness();
}

bool HealthGate::hasRuntimeReadiness() const {
  unsigned long uptime = millis() - boot_time_ms;
  bool stability_ok = (uptime >= OTA_VERIFY_MIN_UPTIME_MS);
  const esp_reset_reason_t reason = esp_reset_reason();
  const bool no_crash = reason != ESP_RST_PANIC && reason != ESP_RST_INT_WDT &&
      reason != ESP_RST_TASK_WDT && reason != ESP_RST_WDT;
  return stability_ok && uart_initialized && no_crash;
}

void HealthGate::reset() {
  state = STATE_INIT;
  boot_time_ms = millis();
  eligible_uptime_ms = 0;
  rollback_eligible = false;
  uart_initialized = false;
  uart_sync_established = false;
  wifi_connected_once = false;
  manifest_fetched_once = false;
  manifest_disabled = false;
  cloud_ready = false;
  last_status_log_ms = 0;
  pending_verify = false;
  marked_valid = false;
  validation_attempted = false;
  LOG_INFO_TAG(LOG_TAG_HEALTH, "Health gate reset");
}

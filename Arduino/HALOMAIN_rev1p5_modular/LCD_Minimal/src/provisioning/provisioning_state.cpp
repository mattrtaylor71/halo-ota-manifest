#include "provisioning_state.h"
#include "wifi_provisioning.h"
#include "nvs_store.h"
#include <esp_log.h>
#include <WiFi.h>

static const char *TAG = "PROV_STATE";
static ProvisioningState currentState = STATE_UNPROVISIONED;
static StateChangeCallback stateCallback = nullptr;
static unsigned long errorStateStartTime = 0;
static const unsigned long ERROR_RETRY_DELAY_MS = 2000; // 2 seconds

ProvisioningState getProvisioningState() {
    return currentState;
}

void setProvisioningState(ProvisioningState newState) {
    if (currentState != newState) {
        ProvisioningState oldState = currentState;
        currentState = newState;
        ESP_LOGI(TAG, "State transition: %s -> %s", 
                 getProvisioningStateString(oldState),
                 getProvisioningStateString(newState));
        
        if (stateCallback) {
            stateCallback(oldState, newState);
        }
        
        if (newState == STATE_ERROR) {
            errorStateStartTime = millis();
        }
    }
}

const char* getProvisioningStateString(ProvisioningState state) {
    switch (state) {
        case STATE_UNPROVISIONED: return "unprovisioned";
        case STATE_AP_SETUP: return "ap_setup";
        case STATE_CONNECTING_HOME_WIFI: return "connecting_home_wifi";
        case STATE_CONNECTED: return "connected";
        case STATE_ERROR: return "error";
        default: return "unknown";
    }
}

void registerStateChangeCallback(StateChangeCallback callback) {
    stateCallback = callback;
}

void provisioningStateMachineLoop() {
    switch (currentState) {
        case STATE_UNPROVISIONED: {
            // Check if we have home Wi-Fi credentials
            std::string homeSsid, homePass;
            if (NVSStore::loadHomeWifiCreds(homeSsid, homePass)) {
                // Try to connect to home Wi-Fi
                setProvisioningState(STATE_CONNECTING_HOME_WIFI);
                startHomeWifiConnect(homeSsid, homePass);
            } else {
                // No credentials, go to AP setup
                // Note: startSoftAP() should be called in setup() before state is set
                // Don't call it here to avoid double initialization
                setProvisioningState(STATE_AP_SETUP);
            }
            break;
        }
        
        case STATE_AP_SETUP: {
            // State is set externally when AP is started (in setup())
            // Don't call startSoftAP() here - it's already called in setup()
            // This prevents double initialization and thread safety issues
            break;
        }
        
        case STATE_CONNECTING_HOME_WIFI: {
            // Check connection status
            if (isWifiConnected()) {
                ESP_LOGI(TAG, "Wi-Fi connected successfully");
                NVSStore::setProvisioned(true);
                setProvisioningState(STATE_CONNECTED);
                stopSoftAP(); // Stop AP when connected
            } else {
                // Check timeout (handled in wifi_provisioning)
                // Connection failure will transition to ERROR state
            }
            break;
        }
        
        case STATE_CONNECTED: {
            // Monitor Wi-Fi connection
            if (!isWifiConnected()) {
                ESP_LOGW(TAG, "Wi-Fi connection lost");
                setProvisioningState(STATE_ERROR);
            }
            break;
        }
        
        case STATE_ERROR: {
            // Wait for retry delay, then return to AP_SETUP
            if (millis() - errorStateStartTime >= ERROR_RETRY_DELAY_MS) {
                ESP_LOGI(TAG, "Error retry delay expired, returning to AP_SETUP");
                setProvisioningState(STATE_AP_SETUP);
            }
            break;
        }
    }
}


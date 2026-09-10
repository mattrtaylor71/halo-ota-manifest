#ifndef PROVISIONING_STATE_H
#define PROVISIONING_STATE_H

#include <Arduino.h>

typedef enum {
    STATE_UNPROVISIONED,
    STATE_AP_SETUP,
    STATE_CONNECTING_HOME_WIFI,
    STATE_CONNECTED,
    STATE_ERROR
} ProvisioningState;

// State management
ProvisioningState getProvisioningState();
void setProvisioningState(ProvisioningState newState);
const char* getProvisioningStateString(ProvisioningState state);

// State machine loop (call periodically)
void provisioningStateMachineLoop();

// Callback registration for state changes
typedef void (*StateChangeCallback)(ProvisioningState oldState, ProvisioningState newState);
void registerStateChangeCallback(StateChangeCallback callback);

#endif // PROVISIONING_STATE_H


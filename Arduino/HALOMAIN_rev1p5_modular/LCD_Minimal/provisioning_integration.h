#ifndef PROVISIONING_INTEGRATION_H
#define PROVISIONING_INTEGRATION_H

#include <Arduino.h>

// Provisioning states (simplified for C compatibility)
typedef enum {
    PROV_STATE_UNPROVISIONED,
    PROV_STATE_SETUP_MODE,      // SoftAP + QR code showing
    PROV_STATE_CONNECTING,       // Connecting to home Wi-Fi
    PROV_STATE_CONNECTED,       // Connected to home Wi-Fi
    PROV_STATE_ERROR            // Connection failed, fallback to setup
} provisioning_state_t;

// Initialize provisioning system (call once at startup)
bool provisioning_init();

// Check if device has home Wi-Fi credentials stored
bool provisioning_has_credentials();

// Get current provisioning state
provisioning_state_t provisioning_get_state();

// Start provisioning flow (call on boot/wake)
// Returns true if we should proceed to normal Halo UI (Wi-Fi connected)
// Returns false if we're in setup mode (show provisioning UI)
bool provisioning_start();

// Update provisioning state machine (call periodically in main loop)
void provisioning_update();

// Check if Wi-Fi is connected (for use by other parts of firmware)
bool provisioning_is_wifi_connected();

// Get Wi-Fi connection info
const char* provisioning_get_wifi_ssid();
IPAddress provisioning_get_wifi_ip();

// Force enter setup mode (for testing or manual trigger)
void provisioning_enter_setup_mode();

// Clear all provisioning data (for testing)
void provisioning_clear_all();

#endif // PROVISIONING_INTEGRATION_H



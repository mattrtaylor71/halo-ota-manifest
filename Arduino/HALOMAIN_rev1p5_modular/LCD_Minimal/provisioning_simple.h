#ifndef PROVISIONING_SIMPLE_H
#define PROVISIONING_SIMPLE_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

// Simplified provisioning system for Halo firmware
// This provides the core functionality without requiring all provisioning UI code

class ProvisioningSimple {
public:
    // Initialize provisioning (call once at startup)
    static bool init();
    
    // Check if home Wi-Fi credentials are stored
    static bool hasCredentials();
    
    // Load home Wi-Fi credentials
    static bool loadCredentials(String &ssid, String &password);
    
    // Save home Wi-Fi credentials
    static void saveCredentials(const String &ssid, const String &password);
    
    // Clear all provisioning data
    static void clearAll();
    
    // Start SoftAP for provisioning
    static bool startSoftAP();
    
    // Stop SoftAP
    static void stopSoftAP();
    
    // Get SoftAP SSID
    static String getApSsid();
    
    // Get SoftAP password
    static String getApPassword();
    
    // Connect to home Wi-Fi with timeout
    // Returns true if connected, false if failed
    static bool connectToHomeWifi(const String &ssid, const String &password, 
                                   unsigned long timeoutMs = 20000);
    
    // Check if Wi-Fi is connected
    static bool isConnected();
    
    // Get current Wi-Fi SSID
    static String getCurrentSsid();
    
    // Get current Wi-Fi IP
    static IPAddress getCurrentIp();

private:
    static Preferences preferences;
    static const char* NVS_NAMESPACE;
    static const char* KEY_HOME_SSID;
    static const char* KEY_HOME_PASS;
    static const char* KEY_AP_SSID;
    static const char* KEY_AP_PASS;
    
    static String apSsid;
    static String apPassword;
    
    static String generateApSsid();
    static String generateApPassword();
};

#endif // PROVISIONING_SIMPLE_H



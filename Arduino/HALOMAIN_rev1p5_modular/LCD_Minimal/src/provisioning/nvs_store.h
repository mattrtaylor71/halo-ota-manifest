#ifndef NVS_STORE_H
#define NVS_STORE_H

#include <string>
#include <Arduino.h>

namespace NVSStore {
    // Initialize NVS (call once at startup)
    bool init();
    
    // Home Wi-Fi credentials
    bool loadHomeWifiCreds(std::string &ssid, std::string &password);
    void saveHomeWifiCreds(const std::string &ssid, const std::string &password);
    void clearHomeWifiCreds();  // Clear home Wi-Fi credentials (for testing)
    
    // AP credentials
    bool loadApCreds(std::string &ssid, std::string &password);
    void saveApCreds(const std::string &ssid, const std::string &password);
    
    // Provisioning status
    bool isProvisioned();
    void setProvisioned(bool value);
    
    // User/Owner ID (UUID)
    bool loadOwnerId(std::string &ownerId);
    void saveOwnerId(const std::string &ownerId);
    
    // Clear all provisioning data (for testing)
    void clearAllProvisioningData();
}

#endif // NVS_STORE_H


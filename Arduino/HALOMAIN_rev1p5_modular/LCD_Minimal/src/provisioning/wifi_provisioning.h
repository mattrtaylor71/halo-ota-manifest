#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <string>
#include <Arduino.h>
#include <WiFi.h>

// SoftAP functions
void startSoftAP();
void stopSoftAP();
std::string getApSsid();
std::string getApPassword();

// Home Wi-Fi connection
void startHomeWifiConnect(const std::string &ssid, const std::string &password);
bool isWifiConnected();
std::string getDeviceId();

// Helper function for logging
const char* getWifiStatusString(wl_status_t status);

// Monitor SoftAP connections (call periodically from main loop)
void monitorSoftAPConnections();

// Connection timeout (20 seconds)
#define WIFI_CONNECT_TIMEOUT_MS 20000

#endif // WIFI_PROVISIONING_H


#include "wifi_provisioning.h"
#include "nvs_store.h"
#include "provisioning_state.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sstream>
#include <iomanip>
#include <cstdlib>

static const char *TAG = "WIFI_PROV";
static std::string apSsid;
static std::string apPassword;
static unsigned long connectStartTime = 0;
static bool connectingToHomeWifi = false;
static bool softAPStarted = false;  // Guard to prevent double initialization

// Generate random alphanumeric password
std::string generateRandomPassword(int length) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string password;
    password.reserve(length);
    
    for (int i = 0; i < length; i++) {
        uint32_t random = esp_random();
        password += charset[random % (sizeof(charset) - 1)];
    }
    
    return password;
}

// Get last 2 bytes of MAC address as hex string
std::string getMacSuffix() {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    
    std::stringstream ss;
    ss << std::uppercase << std::hex 
       << std::setw(2) << std::setfill('0') << (int)mac[4]
       << std::setw(2) << std::setfill('0') << (int)mac[5];
    
    return ss.str();
}

void startSoftAP() {
    // Guard: Prevent double initialization (thread safety)
    if (softAPStarted) {
        Serial.println("⚠ SoftAP already started, skipping");
        ESP_LOGW(TAG, "SoftAP already started, skipping");
        return;
    }
    
    Serial.println("=== Starting SoftAP ===");
    ESP_LOGI(TAG, "=== Starting SoftAP ===");
    
    // Check if AP credentials exist in NVS
    std::string savedSsid, savedPass;
    if (NVSStore::loadApCreds(savedSsid, savedPass)) {
        apSsid = savedSsid;
        apPassword = savedPass;
        Serial.println("Using saved AP credentials");
        Serial.print("  SSID: ");
        Serial.println(apSsid.c_str());
        Serial.print("  Password: ");
        Serial.println(apPassword.c_str());
        ESP_LOGI(TAG, "Using saved AP credentials");
        ESP_LOGI(TAG, "  SSID: %s", apSsid.c_str());
        ESP_LOGI(TAG, "  Password: %s", apPassword.c_str());
    } else {
        // Generate new AP credentials
        std::string macSuffix = getMacSuffix();
        apSsid = "Trepo-Halo-" + macSuffix;
        apPassword = generateRandomPassword(10);
        
        // Save to NVS
        NVSStore::saveApCreds(apSsid, apPassword);
        Serial.println("Generated new AP credentials");
        Serial.print("  SSID: ");
        Serial.println(apSsid.c_str());
        Serial.print("  Password: ");
        Serial.println(apPassword.c_str());
        ESP_LOGI(TAG, "Generated new AP credentials");
        ESP_LOGI(TAG, "  SSID: %s", apSsid.c_str());
        ESP_LOGI(TAG, "  Password: %s", apPassword.c_str());
    }
    
    // Set WiFi mode to AP+STA
    ESP_LOGI(TAG, "Setting WiFi mode to AP+STA");
    WiFi.mode(WIFI_AP_STA);
    
    // Configure SoftAP
    IPAddress localIP(192, 168, 4, 1);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    
    ESP_LOGI(TAG, "Configuring SoftAP network:");
    ESP_LOGI(TAG, "  IP: %s", localIP.toString().c_str());
    ESP_LOGI(TAG, "  Gateway: %s", gateway.toString().c_str());
    ESP_LOGI(TAG, "  Subnet: %s", subnet.toString().c_str());
    
    if (!WiFi.softAPConfig(localIP, gateway, subnet)) {
        ESP_LOGE(TAG, "Failed to configure SoftAP");
        return;
    }
    
    ESP_LOGI(TAG, "Starting SoftAP with SSID: %s", apSsid.c_str());
    if (!WiFi.softAP(apSsid.c_str(), apPassword.c_str())) {
        ESP_LOGE(TAG, "Failed to start SoftAP");
        return;
    }
    
    delay(100); // Give AP time to start
    
    Serial.println("=== SoftAP Started Successfully ===");
    Serial.print("  SSID: ");
    Serial.println(apSsid.c_str());
    Serial.print("  Password: ");
    Serial.println(apPassword.c_str());
    Serial.print("  IP Address: ");
    Serial.println(WiFi.softAPIP().toString().c_str());
    Serial.print("  MAC Address: ");
    Serial.println(WiFi.softAPmacAddress().c_str());
    Serial.print("  Connected Stations: ");
    Serial.println(WiFi.softAPgetStationNum());
    
    ESP_LOGI(TAG, "=== SoftAP Started Successfully ===");
    ESP_LOGI(TAG, "  SSID: %s", apSsid.c_str());
    ESP_LOGI(TAG, "  Password: %s", apPassword.c_str());
    ESP_LOGI(TAG, "  IP Address: %s", WiFi.softAPIP().toString().c_str());
    ESP_LOGI(TAG, "  MAC Address: %s", WiFi.softAPmacAddress().c_str());
    ESP_LOGI(TAG, "  Connected Stations: %d", WiFi.softAPgetStationNum());
    
    softAPStarted = true;  // Mark as started
}

void stopSoftAP() {
    Serial.println("Stopping SoftAP...");
    ESP_LOGI(TAG, "Stopping SoftAP...");
    
    // Yield to allow UI task to process scroll events before blocking operation
    vTaskDelay(pdMS_TO_TICKS(1));
    
    if (WiFi.softAPdisconnect(true)) {
        Serial.println("SoftAP stopped successfully");
        ESP_LOGI(TAG, "SoftAP stopped successfully");
    } else {
        Serial.println("Failed to stop SoftAP");
        ESP_LOGW(TAG, "Failed to stop SoftAP");
    }
    
    softAPStarted = false;  // Reset flag so it can be started again if needed
    
    // Yield again after stopping to allow UI task to catch up
    vTaskDelay(pdMS_TO_TICKS(1));
}

// Monitor station connections (call periodically from main loop)
void monitorSoftAPConnections() {
    static int lastStationCount = -1;
    int currentStationCount = WiFi.softAPgetStationNum();
    
    if (currentStationCount != lastStationCount) {
        if (currentStationCount > lastStationCount) {
            Serial.print(">>> Station CONNECTED! Total stations: ");
            Serial.println(currentStationCount);
            Serial.print("  Station MAC: ");
            // Get station info if possible
            Serial.println("(checking...)");
            ESP_LOGI(TAG, ">>> Station CONNECTED! Total stations: %d", currentStationCount);
        } else if (currentStationCount < lastStationCount) {
            Serial.print(">>> Station DISCONNECTED! Remaining stations: ");
            Serial.println(currentStationCount);
            ESP_LOGI(TAG, ">>> Station DISCONNECTED! Remaining stations: %d", currentStationCount);
        }
        lastStationCount = currentStationCount;
    }
}

std::string getApSsid() {
    return apSsid;
}

std::string getApPassword() {
    return apPassword;
}

void startHomeWifiConnect(const std::string &ssid, const std::string &password) {
    // CRITICAL: Prevent multiple simultaneous connection attempts
    if (connectingToHomeWifi) {
        ESP_LOGW(TAG, "Connection already in progress, ignoring duplicate call");
        Serial.println("⚠ Connection already in progress, ignoring duplicate call");
        return;
    }
    
    Serial.println("=== Starting Home Wi-Fi Connection ===");
    Serial.print("  SSID: ");
    Serial.println(ssid.c_str());
    Serial.print("  Password length: ");
    Serial.println(password.length());
    Serial.print("  Timeout: ");
    Serial.print(WIFI_CONNECT_TIMEOUT_MS);
    Serial.println(" ms");
    
    ESP_LOGI(TAG, "=== Starting Home Wi-Fi Connection ===");
    ESP_LOGI(TAG, "  SSID: %s", ssid.c_str());
    ESP_LOGI(TAG, "  Password length: %d", password.length());
    ESP_LOGI(TAG, "  Timeout: %d ms", WIFI_CONNECT_TIMEOUT_MS);
    
    connectingToHomeWifi = true;
    connectStartTime = millis();
    
    // CRITICAL: Disconnect any existing connection attempt first
    // This prevents "sta is connecting, return error" when WiFi.begin() is called
    wl_status_t current_status = WiFi.status();
    if (current_status != WL_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnecting existing connection (status: %d)...", current_status);
        WiFi.disconnect(true);
        delay(1000);  // Give disconnect more time to complete
        
        // Wait for disconnect to actually complete
        unsigned long disconnect_start = millis();
        while (WiFi.status() != WL_DISCONNECTED && (millis() - disconnect_start) < 2000) {
            delay(100);
        }
        
        if (WiFi.status() != WL_DISCONNECTED) {
            ESP_LOGW(TAG, "Disconnect taking longer than expected, forcing mode change...");
            WiFi.mode(WIFI_OFF);
            delay(500);
        }
    }
    
    // Check if SoftAP is running - if so, use AP+STA mode, otherwise use STA only
    bool apRunning = (WiFi.getMode() & WIFI_MODE_AP) != 0;
    if (apRunning) {
        // SoftAP is running, use AP+STA mode
        WiFi.mode(WIFI_AP_STA);
        ESP_LOGI(TAG, "WiFi mode set to AP+STA (SoftAP running)");
    } else {
        // No SoftAP, use STA only mode
        WiFi.mode(WIFI_STA);
        ESP_LOGI(TAG, "WiFi mode set to STA (no SoftAP)");
    }
    
    // Delay to ensure mode is set
    delay(300);
    
    // Final check - ensure we're in a clean state before calling begin()
    current_status = WiFi.status();
    if (current_status != WL_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi still not disconnected (status: %d), forcing disconnect again...", current_status);
        WiFi.disconnect(true);
        delay(1000);
        // If still not disconnected, force mode change
        if (WiFi.status() != WL_DISCONNECTED) {
            WiFi.mode(WIFI_OFF);
            delay(500);
            if (apRunning) {
                WiFi.mode(WIFI_AP_STA);
            } else {
                WiFi.mode(WIFI_STA);
            }
            delay(300);
        }
    }
    
    // Begin connection
    ESP_LOGI(TAG, "Calling WiFi.begin()...");
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);  // Disable sleep for more reliable connection
    wl_status_t status = WiFi.begin(ssid.c_str(), password.c_str());
    ESP_LOGI(TAG, "WiFi.begin() returned status: %d", status);
    
    // Check if begin() returned an error (status 3 = WL_CONNECT_FAILED typically means already connecting)
    if (status == WL_CONNECT_FAILED) {
        ESP_LOGW(TAG, "WiFi.begin() returned WL_CONNECT_FAILED - may be already connecting, waiting...");
        delay(1000);
        // Check status again
        status = WiFi.status();
        ESP_LOGI(TAG, "WiFi.status() after wait: %d", status);
    }
    
    // Log initial connection status
    unsigned long start = millis();
    int attempts = 0;
    while (WiFi.status() == WL_DISCONNECTED && (millis() - start) < 1000) {
        delay(100);
        attempts++;
    }
    ESP_LOGI(TAG, "Initial connection check after %d ms: status=%d", attempts * 100, WiFi.status());
}

bool isWifiConnected() {
    wl_status_t status = WiFi.status();
    static unsigned long lastLogTime = 0;
    static int connectionAttempts = 0;
    
    if (connectingToHomeWifi) {
        unsigned long elapsed = millis() - connectStartTime;
        connectionAttempts++;
        
        // Log progress every 2 seconds
        if (millis() - lastLogTime > 2000) {
            ESP_LOGI(TAG, "Connection attempt in progress...");
            ESP_LOGI(TAG, "  Elapsed time: %lu ms / %d ms", elapsed, WIFI_CONNECT_TIMEOUT_MS);
            ESP_LOGI(TAG, "  Status: %d (%s)", status, getWifiStatusString(status));
            ESP_LOGI(TAG, "  RSSI: %d dBm", WiFi.RSSI());
            lastLogTime = millis();
        }
        
        // Check timeout
        if (elapsed > WIFI_CONNECT_TIMEOUT_MS) {
            ESP_LOGE(TAG, "=== Wi-Fi Connection Timeout ===");
            ESP_LOGE(TAG, "  Timeout after %lu ms", elapsed);
            ESP_LOGE(TAG, "  Final status: %d (%s)", status, getWifiStatusString(status));
            ESP_LOGE(TAG, "  Total attempts: %d", connectionAttempts);
            connectingToHomeWifi = false;
            connectionAttempts = 0;
            setProvisioningState(STATE_ERROR);
            return false;
        }
        
        // Check connection status - verify we have a valid IP address
        if (status == WL_CONNECTED) {
            // Double-check we actually have an IP address (not just associated)
            IPAddress ip = WiFi.localIP();
            if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) {
                // No IP yet - still connecting, wait a bit more
                return false;
            }
            
            connectingToHomeWifi = false;
            Serial.println("=== Wi-Fi Connected Successfully! ===");
            Serial.print("  Connection time: ");
            Serial.print(elapsed);
            Serial.println(" ms");
            Serial.print("  IP Address: ");
            Serial.println(ip.toString().c_str());
            Serial.print("  Subnet Mask: ");
            Serial.println(WiFi.subnetMask().toString().c_str());
            Serial.print("  Gateway: ");
            Serial.println(WiFi.gatewayIP().toString().c_str());
            Serial.print("  DNS: ");
            Serial.println(WiFi.dnsIP().toString().c_str());
            Serial.print("  MAC Address: ");
            Serial.println(WiFi.macAddress().c_str());
            Serial.print("  RSSI: ");
            Serial.print(WiFi.RSSI());
            Serial.println(" dBm");
            Serial.print("  Channel: ");
            Serial.println(WiFi.channel());
            
            ESP_LOGI(TAG, "=== Wi-Fi Connected Successfully! ===");
            ESP_LOGI(TAG, "  Connection time: %lu ms", elapsed);
            ESP_LOGI(TAG, "  IP Address: %s", WiFi.localIP().toString().c_str());
            ESP_LOGI(TAG, "  Subnet Mask: %s", WiFi.subnetMask().toString().c_str());
            ESP_LOGI(TAG, "  Gateway: %s", WiFi.gatewayIP().toString().c_str());
            ESP_LOGI(TAG, "  DNS: %s", WiFi.dnsIP().toString().c_str());
            ESP_LOGI(TAG, "  MAC Address: %s", WiFi.macAddress().c_str());
            ESP_LOGI(TAG, "  RSSI: %d dBm", WiFi.RSSI());
            ESP_LOGI(TAG, "  Channel: %d", WiFi.channel());
            connectionAttempts = 0;
            return true;
        } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
            connectingToHomeWifi = false;
            ESP_LOGE(TAG, "=== Wi-Fi Connection Failed ===");
            ESP_LOGE(TAG, "  Status: %d (%s)", status, getWifiStatusString(status));
            ESP_LOGE(TAG, "  Elapsed time: %lu ms", elapsed);
            ESP_LOGE(TAG, "  Total attempts: %d", connectionAttempts);
            connectionAttempts = 0;
            setProvisioningState(STATE_ERROR);
            return false;
        }
        
        // Still connecting
        return false;
    }
    
    // Not actively connecting, just check current status
    bool connected = (status == WL_CONNECTED);
    if (connected && lastLogTime > 0) {
        // Reset logging after connection
        lastLogTime = 0;
    }
    return connected;
}

// Helper function to get WiFi status string
const char* getWifiStatusString(wl_status_t status) {
    switch (status) {
        case WL_IDLE_STATUS: return "IDLE";
        case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
        case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
        case WL_CONNECTED: return "CONNECTED";
        case WL_CONNECT_FAILED: return "CONNECT_FAILED";
        case WL_CONNECTION_LOST: return "CONNECTION_LOST";
        case WL_DISCONNECTED: return "DISCONNECTED";
        default: return "UNKNOWN";
    }
}

std::string getDeviceId() {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    
    std::stringstream ss;
    ss << "halo-";
    for (int i = 4; i < 6; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)mac[i];
    }
    
    return ss.str();
}


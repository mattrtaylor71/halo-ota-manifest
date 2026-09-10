#include "http_server.h"
#include "wifi_provisioning.h"
#include "provisioning_state.h"
#include "nvs_store.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_log.h>
#include <string>

static const char *TAG = "HTTP_SERVER";
static AsyncWebServer *server = nullptr;
static bool serverRunning = false;

// Forward declarations
void handleInfo(AsyncWebServerRequest *request);
void handleScan(AsyncWebServerRequest *request);
void handleWifiPost(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleUserIdPost(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void handleStatus(AsyncWebServerRequest *request);
static void handleAndroid204(AsyncWebServerRequest *request);
static void handleAppleHotspotDetect(AsyncWebServerRequest *request);
static void handleMicrosoftNCSI(AsyncWebServerRequest *request);

void startHttpServer() {
    Serial.println("=== Starting HTTP Server ===");
    ESP_LOGI(TAG, "=== Starting HTTP Server ===");
    
    if (serverRunning && server != nullptr) {
        Serial.println("HTTP server already running, skipping start");
        ESP_LOGW(TAG, "HTTP server already running, skipping start");
        return;
    }
    
    if (server == nullptr) {
        Serial.println("Creating new AsyncWebServer instance on port 80");
        ESP_LOGI(TAG, "Creating new AsyncWebServer instance on port 80");
        server = new AsyncWebServer(80);
    }
    
    // GET /info
    Serial.println("Registering endpoint: GET /info");
    ESP_LOGI(TAG, "Registering endpoint: GET /info");
    server->on("/info", HTTP_GET, handleInfo);
    
    // GET /scan
    Serial.println("Registering endpoint: GET /scan");
    ESP_LOGI(TAG, "Registering endpoint: GET /scan");
    server->on("/scan", HTTP_GET, handleScan);
    
    // POST /wifi
    Serial.println("Registering endpoint: POST /wifi");
    ESP_LOGI(TAG, "Registering endpoint: POST /wifi");
    server->on("/wifi", HTTP_POST, 
        [](AsyncWebServerRequest *request){}, 
        NULL, 
        handleWifiPost);
    
    // POST /user-id
    Serial.println("Registering endpoint: POST /user-id");
    ESP_LOGI(TAG, "Registering endpoint: POST /user-id");
    server->on("/user-id", HTTP_POST, 
        [](AsyncWebServerRequest *request){}, 
        NULL, 
        handleUserIdPost);
    
    // GET /status
    Serial.println("Registering endpoint: GET /status");
    ESP_LOGI(TAG, "Registering endpoint: GET /status");
    server->on("/status", HTTP_GET, handleStatus);

    // Common OS captive-network probes. Reply with success so the phone stops
    // repeatedly reopening the captive assistant while the app talks directly
    // to the provisioning API on 192.168.4.1.
    server->on("/generate_204", HTTP_GET, handleAndroid204);
    server->on("/gen_204", HTTP_GET, handleAndroid204);
    server->on("/hotspot-detect.html", HTTP_GET, handleAppleHotspotDetect);
    server->on("/library/test/success.html", HTTP_GET, handleAppleHotspotDetect);
    server->on("/connecttest.txt", HTTP_GET, handleMicrosoftNCSI);
    server->on("/ncsi.txt", HTTP_GET, handleMicrosoftNCSI);
    
    // CORS headers for all responses
    Serial.println("Setting CORS headers");
    ESP_LOGI(TAG, "Setting CORS headers");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");
    
    server->begin();
    serverRunning = true;
    Serial.println("=== HTTP Server Started Successfully ===");
    Serial.println("  Port: 80");
    Serial.println("  Base URL: http://192.168.4.1");
    Serial.println("  Endpoints:");
    Serial.println("    GET  /info");
    Serial.println("    GET  /scan");
    Serial.println("    POST /wifi");
    Serial.println("    POST /user-id");
    Serial.println("    GET  /status");
    
    ESP_LOGI(TAG, "=== HTTP Server Started Successfully ===");
    ESP_LOGI(TAG, "  Port: 80");
    ESP_LOGI(TAG, "  Base URL: http://192.168.4.1");
    ESP_LOGI(TAG, "  Endpoints:");
    ESP_LOGI(TAG, "    GET  /info");
    ESP_LOGI(TAG, "    GET  /scan");
    ESP_LOGI(TAG, "    POST /wifi");
    ESP_LOGI(TAG, "    POST /user-id");
    ESP_LOGI(TAG, "    GET  /status");
}

void stopHttpServer() {
    if (server != nullptr && serverRunning) {
        server->end();
        serverRunning = false;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

bool isHttpServerRunning() {
    return serverRunning;
}

void handleInfo(AsyncWebServerRequest *request) {
    Serial.println();
    Serial.println("=== GET /info ===");
    Serial.print("  Client IP: ");
    Serial.println(request->client()->remoteIP().toString().c_str());
    Serial.print("  User-Agent: ");
    if (request->hasHeader("User-Agent")) {
        Serial.println(request->header("User-Agent").c_str());
    } else {
        Serial.println("(none)");
    }
    
    ESP_LOGI(TAG, "=== GET /info ===");
    ESP_LOGI(TAG, "  Client IP: %s", request->client()->remoteIP().toString().c_str());
    
    DynamicJsonDocument doc(512);
    doc["device_id"] = getDeviceId();
    doc["ap_ssid"] = getApSsid();
    doc["ap_ip"] = "192.168.4.1";
    doc["state"] = getProvisioningStateString(getProvisioningState());
    
    String response;
    serializeJson(doc, response);
    
    ESP_LOGI(TAG, "  Response: %s", response.c_str());
    request->send(200, "application/json", response);
}

static void handleAndroid204(AsyncWebServerRequest *request) {
    ESP_LOGI(TAG, "=== GET %s (android probe) ===", request->url().c_str());
    request->send(204);
}

static void handleAppleHotspotDetect(AsyncWebServerRequest *request) {
    ESP_LOGI(TAG, "=== GET %s (apple probe) ===", request->url().c_str());
    request->send(200, "text/html",
                  "<!DOCTYPE html><html><head><title>Success</title></head>"
                  "<body>Success</body></html>");
}

static void handleMicrosoftNCSI(AsyncWebServerRequest *request) {
    ESP_LOGI(TAG, "=== GET %s (microsoft probe) ===", request->url().c_str());
    request->send(200, "text/plain", "Microsoft Connect Test");
}

void handleScan(AsyncWebServerRequest *request) {
    Serial.println();
    Serial.println("=== GET /scan ===");
    Serial.print("  Client IP: ");
    Serial.println(request->client()->remoteIP().toString().c_str());
    Serial.print("  User-Agent: ");
    if (request->hasHeader("User-Agent")) {
        Serial.println(request->header("User-Agent").c_str());
    } else {
        Serial.println("(none)");
    }
    
    ESP_LOGI(TAG, "=== GET /scan ===");
    ESP_LOGI(TAG, "  Client IP: %s", request->client()->remoteIP().toString().c_str());
    
    Serial.println("Starting Wi-Fi network scan...");
    ESP_LOGI(TAG, "Starting Wi-Fi network scan...");
    unsigned long scanStart = millis();
    
    // Start scan (non-blocking)
    int n = WiFi.scanNetworks();
    
    unsigned long scanTime = millis() - scanStart;
    Serial.print("Scan completed in ");
    Serial.print(scanTime);
    Serial.print(" ms, found ");
    Serial.print(n);
    Serial.println(" networks");
    ESP_LOGI(TAG, "Scan completed in %lu ms, found %d networks", scanTime, n);
    
    DynamicJsonDocument doc(2048);
    JsonArray networks = doc.createNestedArray("networks");
    
    // Sort by RSSI (strongest first)
    struct NetworkInfo {
        String ssid;
        int rssi;
        bool secured;
    };
    
    NetworkInfo *networks_array = new NetworkInfo[n];
    for (int i = 0; i < n; i++) {
        networks_array[i].ssid = WiFi.SSID(i);
        networks_array[i].rssi = WiFi.RSSI(i);
        networks_array[i].secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    
    // Simple bubble sort by RSSI (descending)
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (networks_array[j].rssi < networks_array[j + 1].rssi) {
                NetworkInfo temp = networks_array[j];
                networks_array[j] = networks_array[j + 1];
                networks_array[j + 1] = temp;
            }
        }
    }
    
    // Build JSON response
    for (int i = 0; i < n; i++) {
        JsonObject network = networks.createNestedObject();
        network["ssid"] = networks_array[i].ssid;
        network["rssi"] = networks_array[i].rssi;
        network["secured"] = networks_array[i].secured;
        
        if (i < 5) { // Log first 5 networks
            ESP_LOGI(TAG, "  Network %d: %s (RSSI: %d, Secured: %s)", 
                     i + 1, 
                     networks_array[i].ssid.c_str(),
                     networks_array[i].rssi,
                     networks_array[i].secured ? "Yes" : "No");
        }
    }
    
    delete[] networks_array;
    
    String response;
    serializeJson(doc, response);
    
    ESP_LOGI(TAG, "Sending %d networks to client", n);
    request->send(200, "application/json", response);
}

void handleWifiPost(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    Serial.println();
    Serial.println("=== POST /wifi ===");
    Serial.print("  Client IP: ");
    Serial.println(request->client()->remoteIP().toString().c_str());
    Serial.print("  User-Agent: ");
    if (request->hasHeader("User-Agent")) {
        Serial.println(request->header("User-Agent").c_str());
    } else {
        Serial.println("(none)");
    }
    Serial.print("  Content-Type: ");
    if (request->hasHeader("Content-Type")) {
        Serial.println(request->header("Content-Type").c_str());
    } else {
        Serial.println("(none)");
    }
    Serial.print("  Data length: ");
    Serial.print(len);
    Serial.println(" bytes");
    Serial.print("  Raw data: ");
    for (size_t i = 0; i < len && i < 100; i++) {
        Serial.print((char)data[i]);
    }
    Serial.println();
    
    ESP_LOGI(TAG, "=== POST /wifi ===");
    ESP_LOGI(TAG, "  Client IP: %s", request->client()->remoteIP().toString().c_str());
    ESP_LOGI(TAG, "  Data length: %d bytes", len);
    ESP_LOGI(TAG, "  Raw data: %.*s", len, data);
    
    // Parse JSON
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, (const char *)data, len);
    
    if (error) {
        ESP_LOGE(TAG, "JSON parse error: %s", error.c_str());
        ESP_LOGE(TAG, "  Error code: %d", error.code());
        DynamicJsonDocument errorDoc(128);
        errorDoc["error"] = "invalid_payload";
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        request->send(400, "application/json", errorResponse);
        return;
    }
    
    if (!doc.containsKey("ssid") || !doc.containsKey("password")) {
        ESP_LOGE(TAG, "Missing required fields in request");
        ESP_LOGE(TAG, "  Has SSID: %s", doc.containsKey("ssid") ? "Yes" : "No");
        ESP_LOGE(TAG, "  Has Password: %s", doc.containsKey("password") ? "Yes" : "No");
        DynamicJsonDocument errorDoc(128);
        errorDoc["error"] = "invalid_payload";
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        request->send(400, "application/json", errorResponse);
        return;
    }
    
    String ssid = doc["ssid"].as<String>();
    String password = doc["password"].as<String>();
    
    if (ssid.length() == 0) {
        ESP_LOGE(TAG, "Empty SSID provided");
        DynamicJsonDocument errorDoc(128);
        errorDoc["error"] = "invalid_payload";
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        request->send(400, "application/json", errorResponse);
        return;
    }
    
    Serial.println("=== Received Wi-Fi Credentials ===");
    Serial.print("  SSID: ");
    Serial.println(ssid.c_str());
    Serial.print("  Password length: ");
    Serial.println(password.length());
    Serial.print("  Current state: ");
    Serial.println(getProvisioningStateString(getProvisioningState()));
    
    ESP_LOGI(TAG, "=== Received Wi-Fi Credentials ===");
    ESP_LOGI(TAG, "  SSID: %s", ssid.c_str());
    ESP_LOGI(TAG, "  Password length: %d", password.length());
    ESP_LOGI(TAG, "  Current state: %s", getProvisioningStateString(getProvisioningState()));
    
    // Save to NVS
    Serial.println("Saving credentials to NVS...");
    ESP_LOGI(TAG, "Saving credentials to NVS...");
    NVSStore::saveHomeWifiCreds(ssid.c_str(), password.c_str());
    Serial.println("Credentials saved successfully");
    ESP_LOGI(TAG, "Credentials saved successfully");
    
    // Change state to connecting
    Serial.println("Transitioning to CONNECTING_HOME_WIFI state");
    ESP_LOGI(TAG, "Transitioning to CONNECTING_HOME_WIFI state");
    setProvisioningState(STATE_CONNECTING_HOME_WIFI);
    
    // Start connection
    startHomeWifiConnect(ssid.c_str(), password.c_str());
    
    // Send response
    DynamicJsonDocument responseDoc(128);
    responseDoc["status"] = "connecting";
    String response;
    serializeJson(responseDoc, response);
    
    ESP_LOGI(TAG, "Sending response: %s", response.c_str());
    request->send(200, "application/json", response);
}

void handleUserIdPost(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    Serial.println();
    Serial.println("=== POST /user-id ===");
    Serial.print("  Client IP: ");
    Serial.println(request->client()->remoteIP().toString().c_str());
    Serial.print("  User-Agent: ");
    if (request->hasHeader("User-Agent")) {
        Serial.println(request->header("User-Agent").c_str());
    } else {
        Serial.println("(none)");
    }
    Serial.print("  Content-Type: ");
    if (request->hasHeader("Content-Type")) {
        Serial.println(request->header("Content-Type").c_str());
    } else {
        Serial.println("(none)");
    }
    Serial.print("  Data length: ");
    Serial.print(len);
    Serial.println(" bytes");
    Serial.print("  Raw data: ");
    for (size_t i = 0; i < len && i < 100; i++) {
        Serial.print((char)data[i]);
    }
    Serial.println();
    
    ESP_LOGI(TAG, "=== POST /user-id ===");
    ESP_LOGI(TAG, "  Client IP: %s", request->client()->remoteIP().toString().c_str());
    ESP_LOGI(TAG, "  Data length: %d bytes", len);
    ESP_LOGI(TAG, "  Raw data: %.*s", len, data);
    
    // Parse JSON
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, (const char *)data, len);
    
    if (error) {
        ESP_LOGE(TAG, "JSON parse error: %s", error.c_str());
        ESP_LOGE(TAG, "  Error code: %d", error.code());
        DynamicJsonDocument errorDoc(128);
        errorDoc["error"] = "invalid_payload";
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        request->send(400, "application/json", errorResponse);
        return;
    }
    
    if (!doc.containsKey("owner_id") && !doc.containsKey("ownerId")) {
        ESP_LOGE(TAG, "Missing owner_id field in request");
        DynamicJsonDocument errorDoc(128);
        errorDoc["error"] = "missing_owner_id";
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        request->send(400, "application/json", errorResponse);
        return;
    }
    
    // Support both "owner_id" and "ownerId" for flexibility
    String ownerId;
    if (doc.containsKey("owner_id")) {
        ownerId = doc["owner_id"].as<String>();
    } else {
        ownerId = doc["ownerId"].as<String>();
    }
    
    if (ownerId.length() == 0) {
        ESP_LOGE(TAG, "Empty owner ID provided");
        DynamicJsonDocument errorDoc(128);
        errorDoc["error"] = "invalid_owner_id";
        String errorResponse;
        serializeJson(errorDoc, errorResponse);
        request->send(400, "application/json", errorResponse);
        return;
    }
    
    // Basic UUID format validation (36 characters: 8-4-4-4-12)
    if (ownerId.length() != 36) {
        ESP_LOGW(TAG, "Owner ID length is %d, expected 36 (UUID format)", ownerId.length());
        // Still accept it, but log a warning
    }
    
    Serial.println("=== Received Owner ID ===");
    Serial.print("  Owner ID: ");
    Serial.println(ownerId.c_str());
    
    ESP_LOGI(TAG, "=== Received Owner ID ===");
    ESP_LOGI(TAG, "  Owner ID: %s", ownerId.c_str());
    
    // Save to NVS
    Serial.println("Saving owner ID to NVS...");
    ESP_LOGI(TAG, "Saving owner ID to NVS...");
    NVSStore::saveOwnerId(ownerId.c_str());
    Serial.println("Owner ID saved successfully");
    ESP_LOGI(TAG, "Owner ID saved successfully");
    
    // Send response
    DynamicJsonDocument responseDoc(128);
    responseDoc["status"] = "success";
    responseDoc["owner_id"] = ownerId;
    String response;
    serializeJson(responseDoc, response);
    
    ESP_LOGI(TAG, "Sending response: %s", response.c_str());
    request->send(200, "application/json", response);
}

void handleStatus(AsyncWebServerRequest *request) {
    Serial.println();
    Serial.println("=== GET /status ===");
    Serial.print("  Client IP: ");
    Serial.println(request->client()->remoteIP().toString().c_str());
    Serial.print("  User-Agent: ");
    if (request->hasHeader("User-Agent")) {
        Serial.println(request->header("User-Agent").c_str());
    } else {
        Serial.println("(none)");
    }
    
    ESP_LOGI(TAG, "=== GET /status ===");
    ESP_LOGI(TAG, "  Client IP: %s", request->client()->remoteIP().toString().c_str());
    
    ProvisioningState state = getProvisioningState();
    std::string homeSsid;
    std::string homePass;
    bool hasHomeCreds = NVSStore::loadHomeWifiCreds(homeSsid, homePass);
    bool provisioned = NVSStore::isProvisioned();
    bool wifiConnected = isWifiConnected();
    
    ESP_LOGI(TAG, "  Current state: %s", getProvisioningStateString(state));
    ESP_LOGI(TAG, "  Provisioned: %s", provisioned ? "Yes" : "No");
    ESP_LOGI(TAG, "  Wi-Fi connected: %s", wifiConnected ? "Yes" : "No");
    if (hasHomeCreds) {
        ESP_LOGI(TAG, "  Home SSID: %s", homeSsid.c_str());
    }
    if (wifiConnected) {
        ESP_LOGI(TAG, "  IP Address: %s", WiFi.localIP().toString().c_str());
        ESP_LOGI(TAG, "  RSSI: %d dBm", WiFi.RSSI());
    }
    
    DynamicJsonDocument doc(256);
    doc["provisioned"] = provisioned;
    
    // Determine wifi_state
    String wifiState = "failed";
    if (state == STATE_CONNECTING_HOME_WIFI) {
        wifiState = "connecting";
    } else if (state == STATE_CONNECTED && wifiConnected) {
        wifiState = "connected";
    }
    doc["wifi_state"] = wifiState;
    
    if (hasHomeCreds) {
        doc["home_ssid"] = homeSsid.c_str();
    } else {
        doc["home_ssid"] = "";
    }
    
    String response;
    serializeJson(doc, response);
    
    ESP_LOGI(TAG, "  Response: %s", response.c_str());
    request->send(200, "application/json", response);
}


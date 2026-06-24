#include "ProvisioningManager.h"
#include "ProvisioningState.h"
#include "UartProto.h"
#include "Log.h"
#include "Truth.h"
#include "WifiGuard.h"  // WiFi guard to prevent "STA not started" errors
#include "WifiUtils.h"  // hardResetSta not used here (we keep AP); disconnect + wait to avoid "sta is connecting" race
#include "BuildInfo.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <ctype.h>

static WebServer* server = nullptr;
static DNSServer* dns_server = nullptr;
static const uint16_t DNS_PORT = 53;
static ProvisioningManager* g_provisioning_manager = nullptr;

static bool g_scan_inflight = false;
static unsigned long g_scan_started_ms = 0;
static unsigned long g_scan_cached_ms = 0;
static unsigned long g_scan_forced_ms = 0;
static String g_scan_cached_response;
static const unsigned long kProvisionScanCacheMs = 60000;
static const unsigned long kProvisionScanTimeoutMs = 15000;
static const uint32_t kProvisionScanMaxMsPerChan = 80;
static const unsigned long kProvisionScanForceCooldownMs = 5000;
static const uint8_t kProvisioningApiVersion = 1;
static const char* kProvisioningTransport = "softap_http";
static const char* kProvisioningOwnerFlow = "owner_code_claim";

static String buildProvisionScanResponse(int n);

#ifndef WIFI_SCAN_RUNNING
#define WIFI_SCAN_RUNNING (-1)
#endif

#ifndef WIFI_SCAN_FAILED
#define WIFI_SCAN_FAILED (-2)
#endif

#ifndef PROVISIONING_CLAIM_BASE_URL
#define PROVISIONING_CLAIM_BASE_URL "https://7q6to34qtf3htmmo76td2k7rei0sivxi.lambda-url.us-east-1.on.aws"
#endif

static const char* kProvisioningClaimPath = "/v1/provisioning/claim";

static String getCollectedHeader(const char* name) {
  if (!server || !name || !name[0] || !server->hasHeader(name)) {
    return String();
  }
  return server->header(name);
}

static bool shouldSuppressCaptivePortalUi() {
  if (!g_provisioning_manager) {
    return false;
  }
  return g_provisioning_manager->isSetupModeActive() &&
         g_provisioning_manager->isAppSessionActive(millis());
}

static bool requestLooksInteractiveBrowser() {
  String app_client = getCollectedHeader("X-Halo-Provisioning-Client");
  if (app_client.length() > 0) {
    return false;
  }
  String user_agent = getCollectedHeader("User-Agent");
  return user_agent.indexOf("Mozilla") >= 0 || user_agent.indexOf("Safari") >= 0;
}

static void sendNoContentResponse(const char* route, const char* reason) {
  if (!server) {
    return;
  }
  server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server->sendHeader("Pragma", "no-cache");
  server->send(204, "text/plain", "");
  LOG_INFO("[HTTP] %s → 204 (%s)", route ? route : "-", reason ? reason : "-");
}

static void noteProvisionRequestContext(const char* route) {
  if (!g_provisioning_manager) {
    return;
  }

  String client_type = "unknown";
  String app_version = getCollectedHeader("X-Halo-App-Version");
  String app_client = getCollectedHeader("X-Halo-Provisioning-Client");
  String user_agent = getCollectedHeader("User-Agent");
  bool app_session = false;

  if (app_client.length() > 0) {
    client_type = app_client;
    app_session = true;
  } else if (user_agent.indexOf("Mozilla") >= 0 || user_agent.indexOf("Safari") >= 0) {
    client_type = "browser";
  } else if (user_agent.indexOf("CFNetwork") >= 0) {
    client_type = "ios_http";
  } else if (user_agent.length() > 0) {
    client_type = "http_client";
  }

  g_provisioning_manager->noteProvisionClientRequest(client_type.c_str(),
                                                     app_version.c_str(),
                                                     app_session);
  LOG_INFO("[HTTP] %s client=%s app_session=%d app_ver=%s",
           route ? route : "-",
           client_type.c_str(),
           app_session ? 1 : 0,
           app_version.length() > 0 ? app_version.c_str() : "-");
}

static bool requestArgIsTrue(const char* name) {
  if (!server || !name || !name[0] || !server->hasArg(name)) {
    return false;
  }
  String value = server->arg(name);
  value.trim();
  value.toLowerCase();
  return value.length() == 0 ||
         value == "1" ||
         value == "true" ||
         value == "yes" ||
         value == "y";
}

static void clearProvisionScanCache() {
  g_scan_inflight = false;
  g_scan_started_ms = 0;
  g_scan_cached_ms = 0;
  g_scan_cached_response = "";
  WiFi.scanDelete();
}

static void primeProvisionScanCache() {
  LOG_INFO("[PROVISION] Priming Wi-Fi scan cache before SoftAP start");
  WiFi.scanDelete();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(150);

  int scan_count = WiFi.scanNetworks(false, false, true, kProvisionScanMaxMsPerChan);
  if (scan_count < 0) {
    LOG_WARN("[PROVISION] Pre-setup scan failed rc=%d", scan_count);
    return;
  }

  g_scan_cached_response = buildProvisionScanResponse(scan_count);
  g_scan_cached_ms = millis();
  LOG_INFO("[PROVISION] Pre-setup scan cached %d networks", scan_count);
  WiFi.scanDelete();
}

static String buildProvisionScanResponse(int n) {
  DynamicJsonDocument doc(2304);
  doc["api_version"] = kProvisioningApiVersion;
  doc["status"] = "ok";
  JsonArray networks = doc.createNestedArray("networks");

  if (n > 0) {
    struct NetworkInfo {
      String ssid;
      int rssi;
      bool secured;
    };

    NetworkInfo* networks_array = new NetworkInfo[n];
    for (int i = 0; i < n; i++) {
      networks_array[i].ssid = WiFi.SSID(i);
      networks_array[i].rssi = WiFi.RSSI(i);
      networks_array[i].secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }

    for (int i = 0; i < n - 1; i++) {
      for (int j = 0; j < n - i - 1; j++) {
        if (networks_array[j].rssi < networks_array[j + 1].rssi) {
          NetworkInfo temp = networks_array[j];
          networks_array[j] = networks_array[j + 1];
          networks_array[j + 1] = temp;
        }
      }
    }

    for (int i = 0; i < n; i++) {
      JsonObject network = networks.createNestedObject();
      network["ssid"] = networks_array[i].ssid;
      network["rssi"] = networks_array[i].rssi;
      network["secured"] = networks_array[i].secured;
    }

    delete[] networks_array;
  }

  String response;
  serializeJson(doc, response);
  return response;
}

static void updateProvisionScanState() {
  if (!g_scan_inflight) {
    return;
  }

  int scan_status = WiFi.scanComplete();
  if (scan_status == WIFI_SCAN_RUNNING) {
    if ((millis() - g_scan_started_ms) > kProvisionScanTimeoutMs) {
      LOG_WARN("[PROVISION] Async Wi-Fi scan timed out");
      clearProvisionScanCache();
    }
    return;
  }

  if (scan_status == WIFI_SCAN_FAILED) {
    LOG_WARN("[PROVISION] Async Wi-Fi scan failed");
    clearProvisionScanCache();
    return;
  }

  if (scan_status < 0) {
    LOG_WARN("[PROVISION] Async Wi-Fi scan returned status=%d", scan_status);
    clearProvisionScanCache();
    return;
  }

  g_scan_cached_response = buildProvisionScanResponse(scan_status);
  g_scan_cached_ms = millis();
  g_scan_inflight = false;
  LOG_INFO("[PROVISION] Async scan complete: %d networks", scan_status);
  WiFi.scanDelete();
}

// Forward declarations for HTTP handlers (friend functions, declared in header)
void handleInfo();
void handleScan();
void handleWifiPost();
void handleStatus();
void handleUserIdPost();
void handleRoot();
void handlePortalOverride();
void handleCaptivePortal();
void handleNotFound();

ProvisioningManager::ProvisioningManager()
  : uart_proto(nullptr), setup_mode_active(false), 
    connecting_start_ms(0), connected_verified_ms(0), last_disconnect_ms(0), connected_state_set_ms(0), owner_id_set_ms(0),
    sta_failure_count(0), connect_attempt(0), http_server(nullptr),
    last_claim_attempt_ms(0), claim_attempts(0), claim_in_progress(false), claim_completed(false),
    last_app_request_ms(0) {
  ap_ssid[0] = '\0';
  ap_password[0] = '\0';
  device_id[0] = '\0';
  target_home_ssid[0] = '\0';
  last_error[0] = '\0';
  last_provision_client[0] = '\0';
  last_app_version[0] = '\0';
  g_provisioning_manager = this;
}

static bool normalize_owner_code(const char* input, char* output, size_t output_sz) {
  if (!output || output_sz == 0) {
    return false;
  }
  output[0] = '\0';
  if (!input) {
    return false;
  }
  size_t out_len = 0;
  for (const char* p = input; *p; ++p) {
    char c = *p;
    if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      continue;
    }
    if (!isalnum(static_cast<unsigned char>(c))) {
      continue;
    }
    if (out_len + 1 >= output_sz) {
      break;
    }
    output[out_len++] = static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }
  output[out_len] = '\0';
  return out_len > 0;
}

ProvisioningManager::~ProvisioningManager() {
  stopSetupMode();
  if (server) {
    delete server;
    server = nullptr;
  }
  g_provisioning_manager = nullptr;
}

void ProvisioningManager::init(UartProto* uart) {
  uart_proto = uart;
  ProvisioningState::init();
  ProvisioningState::getDeviceId(device_id, sizeof(device_id));
  
  // Initialize device_id if not set
  if (device_id[0] == '\0') {
    ProvisioningState::getDeviceId(device_id, sizeof(device_id));
  }
  
  LOG_INFO("[PROVISION] Manager initialized: device_id=%s", device_id);
}

void ProvisioningManager::noteProvisionClientRequest(const char* client_type,
                                                     const char* app_version,
                                                     bool app_session) {
  const char* client = (client_type && client_type[0]) ? client_type : "unknown";
  strncpy(last_provision_client, client, sizeof(last_provision_client) - 1);
  last_provision_client[sizeof(last_provision_client) - 1] = '\0';

  if (app_version && app_version[0]) {
    strncpy(last_app_version, app_version, sizeof(last_app_version) - 1);
    last_app_version[sizeof(last_app_version) - 1] = '\0';
  } else {
    last_app_version[0] = '\0';
  }

  if (app_session) {
    last_app_request_ms = millis();
  }
}

bool ProvisioningManager::isAppSessionActive(unsigned long now_ms) const {
  if (last_app_request_ms == 0) {
    return false;
  }
  if (now_ms == 0) {
    now_ms = millis();
  }
  return (now_ms - last_app_request_ms) <= APP_SESSION_ACTIVE_MS;
}

void ProvisioningManager::update() {
  if (halo_rebooting()) {
    return;
  }
  if (!setup_mode_active) {
    return;
  }
  
  // Handle DNS (captive portal)
  if (dns_server && !shouldSuppressCaptivePortalUi()) {
    dns_server->processNextRequest();
  }

  // Handle HTTP server requests
  if (server) {
    server->handleClient();
  } else {
    // Diagnostic: Log if server is null but setup_mode_active is true
    static unsigned long last_server_null_log_ms = 0;
    unsigned long now = millis();
    if (now - last_server_null_log_ms > 5000) {  // Log every 5 seconds max
      LOG_WARN("[PROVISION] setup_mode_active=true but server is null!");
      last_server_null_log_ms = now;
    }
  }

  updateProvisionScanState();
  
  // State machine: CONNECTING_HOME_WIFI
  // Single source of truth: state and last_error drive /status (wifi_state + last_error). On timeout/fail we set state=ERROR so next poll sees failed.
  if (ProvisioningState::getState() == ProvisioningState::STATE_CONNECTING_HOME_WIFI) {
    unsigned long now = millis();
    wl_status_t wifi_status = WiFi.status();
    
    // Success: connected and verified
    if (wifi_status == WL_CONNECTED) {
      // Connected again — clear any in-progress flap timer (AP_STA STA link recovered)
      last_disconnect_ms = 0;
      // Connection established - verify it's stable
      if (connected_verified_ms == 0) {
        connected_verified_ms = now;
        int rssi = WiFi.RSSI();
        LOG_INFO("[PROVISION] Home Wi-Fi connected: IP=%s RSSI=%d mode=%d (verifying stability...)",
                 WiFi.localIP().toString().c_str(), rssi, (int)WiFi.getMode());
        snprintf(last_error, sizeof(last_error), "");
      } else if (now - connected_verified_ms >= CONNECTION_VERIFY_DELAY_MS) {
        // Connection verified stable - mark as provisioned
        if (!ProvisioningState::isProvisioned()) {
          LOG_INFO("[PROVISION] Connection verified stable, marking as provisioned");
          ProvisioningState::setProvisioned(true);
        }
        ProvisioningState::setState(ProvisioningState::STATE_CONNECTED);
        sendProvisionStatus("connected");
        // Kick NTP/SNTP the instant STA is connected so the owner-claim TLS (and OTA
        // schedule) calls have valid time on the first try instead of failing http=-1
        // while time is invalid right after connect. Non-blocking + idempotent.
        extern void halo_prod_kick_time_sync(const char* reason);
        halo_prod_kick_time_sync("provision_connected");
        sta_failure_count = 0;
        connect_attempt = 0;
        snprintf(last_error, sizeof(last_error), "");
        dump_system_truth("wifi_stable");
        
        if (connected_state_set_ms == 0) {
          connected_state_set_ms = now;
          LOG_INFO("[PROVISION] Marked STATE_CONNECTED - keeping SoftAP/server alive for minimum 1500ms for browser UX");
          // Send Wi-Fi creds to LCD once per provisioning success (LCD stores in NVS; does not enable Wi-Fi)
          if (uart_proto) {
            char ssid[64];
            char pass[64];
            if (ProvisioningState::loadHomeWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass))) {
              size_t ssid_len = strlen(ssid);
              size_t pass_len = strlen(pass);
              uart_proto->send_wifi_creds(0, ssid, pass);
              Serial.printf("[UART][WIFI_CREDS] sending to LCD ssid_len=%u pass_len=%u\n", (unsigned)ssid_len, (unsigned)pass_len);
              // Immediately command LCD to enable Wi-Fi so it can run LCD OTA (10 min timeout)
              const uint32_t lcd_wifi_timeout_ms = 600000;
              uart_proto->send_wifi_on(0, lcd_wifi_timeout_ms);
              Serial.printf("[UART][WIFI_ON] sending to LCD timeout_ms=%lu\n", (unsigned long)lcd_wifi_timeout_ms);
            }
          }
        }
        
        extern void maybeRunOtaCheck(const char* reason);
        maybeRunOtaCheck("post_provision");
      }
    } else {
    // AP_STA flap tolerance: if we already saw WL_CONNECTED (connected_verified_ms set),
    // the STA link momentarily leaving WL_CONNECTED is almost always radio-sharing flap,
    // not a real drop. Don't restart the verify window or fall into the fail/retry path
    // (which would re-WiFi.begin() and reset connecting_start_ms, delaying STATE_CONNECTED).
    // Track continuous-disconnect time and only treat it as a real failure after
    // FLAP_TOLERANCE_MS of being continuously not-connected. (last_disconnect_ms is cleared
    // the moment WL_CONNECTED is seen again in the success branch above.)
    if (connected_verified_ms != 0) {
      if (last_disconnect_ms == 0) {
        last_disconnect_ms = now;
        LOG_INFO("[PROVISION] STA flap: status=%d after connect (tolerating up to %lu ms)",
                 (int)wifi_status, FLAP_TOLERANCE_MS);
      }
      if ((now - last_disconnect_ms) < FLAP_TOLERANCE_MS) {
        // Brief flap — stay in CONNECTING, let it recover. Do not evaluate failure this tick.
        return;
      }
      // Disconnected continuously past the tolerance window — real drop; fall through to
      // the normal failure handling below so the app still sees wifi_state=failed.
      LOG_WARN("[PROVISION] STA drop persisted > %lu ms after connect; treating as failure (status=%d)",
               FLAP_TOLERANCE_MS, (int)wifi_status);
    }
    // Failure check: WL_NO_SSID_AVAIL is terminal (wrong SSID), but WL_CONNECT_FAILED
    // can be transient in AP_STA mode — wait at least 8s before treating it as real.
    unsigned long elapsed_connect = now - connecting_start_ms;
    bool immediate_fail = (wifi_status == WL_NO_SSID_AVAIL);
    bool connect_failed_stable = (wifi_status == WL_CONNECT_FAILED && elapsed_connect >= 8000);
    bool timed_out = (elapsed_connect >= CONNECTING_TIMEOUT_MS);
    bool failed = immediate_fail || connect_failed_stable || timed_out;
    if (failed) {
      unsigned int attempt_num = connect_attempt + 1;
      unsigned int max_attempts = CONNECT_MAX_RETRIES + 1;  // 2 attempts total
      if (wifi_status == WL_CONNECT_FAILED || wifi_status == WL_NO_SSID_AVAIL) {
        snprintf(last_error, sizeof(last_error), "Wi-Fi failed (status=%d)", (int)wifi_status);
        LOG_ERROR("[PROVISION] Home Wi-Fi connection failed (attempt %u/%u, status=%d)", attempt_num, max_attempts, (int)wifi_status);
      } else {
        snprintf(last_error, sizeof(last_error), "Connection timeout after %lu ms", CONNECTING_TIMEOUT_MS);
        LOG_ERROR("[PROVISION] Home Wi-Fi connection timeout (attempt %u/%u)", attempt_num, max_attempts);
      }
      sta_failure_count++;
      
      if (connect_attempt < CONNECT_MAX_RETRIES) {
        // One automatic retry: reset STA without tearing down SoftAP/captive portal.
        connect_attempt++;
        int status_before = (int)WiFi.status();
        wifi_mode_t mode_before = WiFi.getMode();
        LOG_INFO("[PROVISION] Retry %u/%u: status=%d mode=%d; disconnect and wait before begin", connect_attempt + 1, max_attempts, status_before, mode_before);
        // Wait until driver leaves "connecting" (WL_IDLE_STATUS=0 or WL_DISCONNECTED=6 / WL_CONNECT_FAILED=4) so next begin() is accepted.
        for (int wait = 0; wait < 30; wait++) {
          wl_status_t st = WiFi.status();
          if (st == WL_DISCONNECTED || st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) break;
          delay(100);
        }
        WiFi.disconnect(false, false);
        WiFi.mode(WIFI_AP_STA);
        delay(2000);  // Let driver fully reset before retry (400ms was too short in AP_STA mode)
        char ssid[64];
        char pass[64];
        if (ProvisioningState::loadHomeWifiCreds(ssid, sizeof(ssid), pass, sizeof(pass))) {
          WiFi.begin(ssid, pass);
          halo_sta_mark_begin_called();
          connecting_start_ms = millis();
          int status_after = (int)WiFi.status();
          LOG_INFO("[PROVISION] Retry %u: begin done, status=%d mode=AP_STA", connect_attempt + 1, status_after);
        } else {
          // No creds to retry - go to error
          ProvisioningState::setState(ProvisioningState::STATE_ERROR);
          setLastError("No credentials for retry");
          sendProvisionStatus("failed");
          connect_attempt = 0;
        }
      } else {
        // Second failure: transition to error/failed; keep SoftAP and server so /status returns failed + last_error
        ProvisioningState::setState(ProvisioningState::STATE_ERROR);
        sendProvisionStatus("failed");
        connect_attempt = 0;
        LOG_ERROR("[PROVISION] Connection failed after %u attempts; state=error, portal still up for retry", max_attempts);
        // Do NOT call startSetupMode() - keep portal so user can re-submit credentials; /status will show wifi_state=failed
      }
    }
    }  // end else (not connected)
  }
  
  // State machine: CONNECTED (grace period for SoftAP)
  if (ProvisioningState::getState() == ProvisioningState::STATE_CONNECTED) {
    unsigned long now = millis();
    
    // Check if owner_id was set (extends grace period)
    bool owner_id_set = false;
    char owner_id[64];
    if (ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id))) {
      if (owner_id_set_ms == 0) {
        owner_id_set_ms = now;  // Record when owner_id was set
      }
      owner_id_set = true;
    }
    
    if (!owner_id_set) {
      if (tryClaimOwnerId()) {
        owner_id_set = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
        if (owner_id_set && owner_id_set_ms == 0) {
          owner_id_set_ms = millis();
        }
      }
    }
    
    // Stop SoftAP if:
    // 1. Minimum delay after marking connected has passed (1500ms for browser UX), AND
    // 2. Grace period expired (60s after connection verified) AND owner_id not set, OR
    // 3. Owner_id was set AND 15s have passed since owner_id was set
    const unsigned long MIN_CONNECTED_DELAY_MS = 1500;  // Minimum delay for browser to poll /status
    // Owner grace bumped 10s→15s so the app's 1.5s-interval /status poll reliably catches owner_id_set=true before SoftAP teardown.
    unsigned long grace_end_ms = (owner_id_set_ms > 0) ? owner_id_set_ms + 15000 : connected_verified_ms + SOFTAP_GRACE_PERIOD_MS;
    
    // Ensure minimum delay after marking connected (for browser UX)
    bool min_delay_passed = (connected_state_set_ms > 0 && (now - connected_state_set_ms) >= MIN_CONNECTED_DELAY_MS);
    
    if (connected_verified_ms > 0 && min_delay_passed && now >= grace_end_ms) {
      // Log WiFi state before stopping
      wifi_mode_t wifi_mode = WiFi.getMode();
      wl_status_t wifi_status = WiFi.status();
      LOG_INFO("[PROVISION] Grace period expired (owner_id_set=%d), stopping SoftAP (WiFi.mode=%d status=%d)", 
               owner_id_set ? 1 : 0, wifi_mode, wifi_status);
      stopSetupMode();
    } else if (connected_state_set_ms > 0 && !min_delay_passed) {
      // Log that we're waiting for minimum delay (only once)
      static bool min_delay_logged = false;
      if (!min_delay_logged) {
        LOG_INFO("[PROVISION] Waiting minimum %lu ms after marking connected before allowing SoftAP shutdown", MIN_CONNECTED_DELAY_MS);
        min_delay_logged = true;
      }
    }
    
    // Monitor connection - if Wi-Fi drops (definitively disconnected, not connecting), re-enter Setup Mode
    // Only check if we were previously connected (connected_verified_ms > 0)
    // Guard: Only check disconnection if connection was verified (not during initial connection)
    wl_status_t wifi_status = WiFi.status();
    if (connected_verified_ms > 0 && (wifi_status == WL_DISCONNECTED || wifi_status == WL_CONNECTION_LOST)) {
      LOG_ERROR("[PROVISION] Wi-Fi connection lost after provisioning");
      if (!setup_mode_active) {
        // Re-enter Setup Mode (this will clear failure state and set STATE_AP_SETUP)
        startSetupMode();
      }
    }
  }
}

bool ProvisioningManager::startSetupMode() {
  if (setup_mode_active) {
    LOG_WARN("[PROVISION] Setup Mode already active");
    return true;
  }
  
  LOG_INFO("[PROVISION] Starting Setup Mode...");
  
  // Generate a fresh SoftAP session so phones do not reuse stale captive-portal heuristics.
  ProvisioningState::generateApSsid(ap_ssid, sizeof(ap_ssid));
  ProvisioningState::generateRandomPassword(ap_password, sizeof(ap_password));
  ProvisioningState::saveApCreds(ap_ssid, ap_password);
  clearProvisionScanCache();
  primeProvisionScanCache();
  LOG_INFO("[PROVISION] Generated setup AP credentials: SSID=%s", ap_ssid);
  
  // Start SoftAP
  if (!startSoftAP()) {
    LOG_ERROR("[PROVISION] Failed to start SoftAP");
    return false;
  }
  
  // Start DNS server for captive portal
  if (!dns_server) {
    dns_server = new DNSServer();
  }
  if (dns_server) {
    dns_server->setErrorReplyCode(DNSReplyCode::NoError);
    dns_server->start(DNS_PORT, "*", WiFi.softAPIP());
    LOG_INFO("[PROVISION] DNS captive portal started (port=%u)", (unsigned)DNS_PORT);
  }

  // Start HTTP server
  if (!startHttpServer()) {
    LOG_ERROR("[PROVISION] Failed to start HTTP server");
    stopSoftAP();
    return false;
  }
  
  // Clear any stale failure state and set to AP_SETUP
  setLastError("");  // Clear any previous error
  sta_failure_count = 0;  // Reset failure count
  target_home_ssid[0] = '\0';  // Clear target SSID when entering Setup Mode
  ProvisioningState::setState(ProvisioningState::STATE_AP_SETUP);
  setup_mode_active = true;
  sendProvisionStatus("ap_setup");
  
  LOG_INFO("[PROVISION] Entered Setup Mode: clearing failure state, wifi_state=ap_setup");
  LOG_INFO("[PROVISION] Setup Mode active: SSID=%s, Password=%s", ap_ssid, ap_password);
  return true;
}

void ProvisioningManager::stopSetupMode() {
  if (!setup_mode_active) {
    return;
  }
  
  // Log WiFi state before stopping (for diagnostics)
  wifi_mode_t wifi_mode = WiFi.getMode();
  wl_status_t wifi_status = WiFi.status();
  LOG_INFO("[PROVISION] Stopping Setup Mode... (WiFi.mode=%d status=%d)", wifi_mode, wifi_status);
  
  // CRITICAL: Safe teardown order to prevent "STA not started" errors
  // 1. Stop HTTP server first (closes all client connections)
  stopHttpServer();
  
  // Stop DNS server
  if (dns_server) {
    dns_server->stop();
    delete dns_server;
    dns_server = nullptr;
    LOG_INFO("[PROVISION] DNS captive portal stopped");
  }

  // 2. Stop SoftAP
  stopSoftAP();
  
  // 3. Brief delay to ensure teardown completes
  delay(50);
  
  // 4. If STA was started, switch to STA-only mode (do NOT disconnect unless STA was started)
  // Note: We don't call WiFi.disconnect() here - let WifiManager handle STA disconnection
  // Only change mode if we're in AP_STA mode and want to keep STA
  if (wifi_mode == WIFI_AP_STA && WiFi.status() == WL_CONNECTED) {
    // Keep STA running - don't change mode
    LOG_INFO("[PROVISION] Keeping STA mode after SoftAP stop (WiFi connected)");
  } else if (wifi_mode == WIFI_AP_STA) {
    // STA not connected - safe to switch to STA mode
    WiFi.mode(WIFI_STA);
    LOG_INFO("[PROVISION] Switched to WIFI_STA mode after SoftAP stop");
    // Note: Don't mark STA as off here - STA is still active, just not connected
  }
  
  setup_mode_active = false;
  connected_state_set_ms = 0;  // Reset connected state timestamp
  LOG_INFO("[PROVISION] Setup Mode stopped");
  
  // Truth logger: Setup mode stopped
  dump_system_truth("setup_stopped");
}

bool ProvisioningManager::startSoftAP() {
  // Set WiFi mode to AP+STA (allows both SoftAP and STA)
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  
  // Configure SoftAP IP — standard ESP32 SoftAP subnet
  IPAddress localIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  
  if (!WiFi.softAPConfig(localIP, gateway, subnet)) {
    LOG_ERROR("[PROVISION] Failed to configure SoftAP");
    return false;
  }
  
  // Start SoftAP
  if (!WiFi.softAP(ap_ssid, ap_password)) {
    LOG_ERROR("[PROVISION] Failed to start SoftAP");
    return false;
  }
  
  LOG_INFO("[PROVISION] SoftAP started: SSID=%s, IP=%s", ap_ssid, WiFi.softAPIP().toString().c_str());
  return true;
}

void ProvisioningManager::stopSoftAP() {
  WiFi.softAPdisconnect(true);
  LOG_INFO("[PROVISION] SoftAP stopped");
}

bool ProvisioningManager::startHttpServer() {
  if (server) {
    LOG_INFO("[PROVISION] Stopping existing HTTP server before restart");
    server->stop();
    delete server;
    server = nullptr;
  }
  
  LOG_INFO("[PROVISION] Creating new WebServer on port 80");
  server = new WebServer(80);
  
  if (!server) {
    LOG_ERROR("[PROVISION] Failed to allocate WebServer");
    return false;
  }

  static const char* kCollectedHeaders[] = {
    "User-Agent",
    "X-Halo-Provisioning-Client",
    "X-Halo-App-Version"
  };
  server->collectHeaders(kCollectedHeaders, sizeof(kCollectedHeaders) / sizeof(kCollectedHeaders[0]));
  
  // Register endpoints
  server->on("/info", HTTP_GET, handleInfo);
  server->on("/scan", HTTP_GET, handleScan);
  server->on("/wifi", HTTP_POST, handleWifiPost);
  server->on("/status", HTTP_GET, handleStatus);
  server->on("/user-id", HTTP_POST, handleUserIdPost);
  server->on("/portal", HTTP_GET, handlePortalOverride);
  // Captive portal endpoints (OS connectivity checks)
  server->on("/generate_204", HTTP_GET, handleCaptivePortal);
  server->on("/gen_204", HTTP_GET, handleCaptivePortal);
  server->on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  server->on("/library/test/success.html", HTTP_GET, handleCaptivePortal);
  server->on("/ncsi.txt", HTTP_GET, handleCaptivePortal);
  server->on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  server->on("/success.txt", HTTP_GET, handleCaptivePortal);
  server->on("/redirect", HTTP_GET, handleCaptivePortal);
  server->on("/canonical.html", HTTP_GET, handleCaptivePortal);
  server->on("/fwlink", HTTP_GET, handleCaptivePortal);
  server->on("/mobile/status.php", HTTP_GET, handleCaptivePortal);
  server->on("/connectivity-check.html", HTTP_GET, handleCaptivePortal);
  server->on("/", HTTP_GET, handleRoot);
  server->onNotFound(handleNotFound);
  
  LOG_INFO("[PROVISION] Registered HTTP endpoints: /, /portal, /info, /scan, /wifi, /status, /user-id, captive");
  
  server->begin();
  
  // Verify server started
  IPAddress ap_ip = WiFi.softAPIP();
  LOG_INFO("[PROVISION] HTTP server started on port 80");
  LOG_INFO("[PROVISION] Connect to: http://%s", ap_ip.toString().c_str());
  return true;
}

void ProvisioningManager::stopHttpServer() {
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }
  LOG_INFO("[PROVISION] HTTP server stopped");
}

void ProvisioningManager::setLastError(const char* error) {
  if (error) {
    strncpy(last_error, error, sizeof(last_error) - 1);
    last_error[sizeof(last_error) - 1] = '\0';
  } else {
    last_error[0] = '\0';
  }
}

void ProvisioningManager::resetOwnerClaimState() {
  last_claim_attempt_ms = 0;
  claim_attempts = 0;
  claim_in_progress = false;
  claim_completed = false;
}

void ProvisioningManager::resetClaimForRetry() {
  if (claim_completed && !claim_in_progress) {
    // Only reset if claim was attempted and failed (not if it succeeded)
    char owner_id[64] = {0};
    bool has_owner = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
    if (has_owner && owner_id[0] != '\0') {
      // Owner already set (claim succeeded or /user-id fallback worked) — no retry needed
      return;
    }
    char owner_code[32] = {0};
    bool has_code = ProvisioningState::loadOwnerCode(owner_code, sizeof(owner_code));
    if (!has_code || owner_code[0] == '\0') {
      // No owner code to claim — nothing to retry
      return;
    }
    LOG_INFO("[PROVISION] Resetting claim state for post-AP retry (attempts=%u)", claim_attempts);
    claim_completed = false;
    claim_attempts = 0;
    last_claim_attempt_ms = 0;
  }
}

void ProvisioningManager::startHomeWifiConnect(const char* ssid, const char* password) {
  ProvisioningState::State current_state = ProvisioningState::getState();
  if (current_state == ProvisioningState::STATE_CONNECTING_HOME_WIFI) {
    LOG_WARN("[PROVISION] Restarting in-flight Wi-Fi connection with new credentials");
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_AP_STA);
    delay(300);
  }
  
  LOG_INFO("[PROVISION] Starting home Wi-Fi connection: ssid=%s", ssid);
  
  // Store target SSID (should already be set by handleWifiPost, but ensure it's set here too)
  if (target_home_ssid[0] == '\0' || strcmp(target_home_ssid, ssid) != 0) {
    strncpy(target_home_ssid, ssid, sizeof(target_home_ssid) - 1);
    target_home_ssid[sizeof(target_home_ssid) - 1] = '\0';
  }
  
  // Save credentials
  ProvisioningState::saveHomeWifiCreds(ssid, password);
  resetOwnerClaimState();
  
  // Reset verification timers and attempt counter (new submit = attempt 1/2)
  connected_verified_ms = 0;
  last_disconnect_ms = 0;  // Fresh submit: clear flap-tolerance disconnect tracker
  owner_id_set_ms = 0;
  connect_attempt = 0;
  snprintf(last_error, sizeof(last_error), "");
  
  // Set state BEFORE calling WiFi.begin() to prevent race conditions
  ProvisioningState::setState(ProvisioningState::STATE_CONNECTING_HOME_WIFI);
  connecting_start_ms = millis();
  sendProvisionStatus("connecting");
  
  // Start STA connection (non-blocking). AP_STA so AP stays up during connect.
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  halo_sta_mark_begin_called();
  int st = (int)WiFi.status();
  LOG_INFO("[PROVISION] Wi-Fi.begin() called, status=%d mode=%d", st, (int)WiFi.getMode());
}

// Implemented in halo_sense_prod.ino (C linkage). Free/re-acquire the 16KB camera
// DMA reserve around the claim's TLS handshake so it has contiguous internal RAM.
extern "C" void halo_tls_free_dma_reserve();
extern "C" void halo_tls_restore_dma_reserve();

bool ProvisioningManager::tryClaimOwnerId() {
  if (claim_completed || claim_in_progress) {
    return false;
  }
  
  // If owner_id already set and no owner_code present, nothing to claim
  char owner_id[64];
  bool owner_id_set = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  char prev_owner_id[64];
  bool prev_owner_id_set = owner_id_set;
  if (owner_id_set) {
    strncpy(prev_owner_id, owner_id, sizeof(prev_owner_id) - 1);
    prev_owner_id[sizeof(prev_owner_id) - 1] = '\0';
  } else {
    prev_owner_id[0] = '\0';
  }
  char owner_code_raw[32];
  bool owner_code_set = ProvisioningState::loadOwnerCode(owner_code_raw, sizeof(owner_code_raw));
  if (!owner_code_set) {
    if (owner_id_set) {
      claim_completed = true;
      return true;
    }
    setLastError("Setup code missing");
    claim_completed = true;
    return false;
  }
  if (owner_id_set) {
    LOG_WARN("[PROVISION] Owner code present; clearing existing owner_id to re-claim");
    ProvisioningState::clearOwnerId();
  }
  
  char owner_code[32];
  if (!normalize_owner_code(owner_code_raw, owner_code, sizeof(owner_code))) {
    setLastError("Setup code invalid");
    claim_completed = true;
    return false;
  }
  
  static const unsigned long kBackoffMs[] = { 2000, 5000, 10000 };
  const unsigned int kMaxAttempts = 4;
  unsigned long now = millis();
  if (claim_attempts > 0) {
    unsigned int backoff_idx = (claim_attempts - 1 < (sizeof(kBackoffMs) / sizeof(kBackoffMs[0])))
                                 ? (claim_attempts - 1)
                                 : (sizeof(kBackoffMs) / sizeof(kBackoffMs[0])) - 1;
    if (now - last_claim_attempt_ms < kBackoffMs[backoff_idx]) {
      return false;
    }
  }
  if (claim_attempts >= kMaxAttempts) {
    return false;
  }
  
  String url = String(PROVISIONING_CLAIM_BASE_URL) + kProvisioningClaimPath;
  claim_in_progress = true;
  claim_attempts++;
  last_claim_attempt_ms = now;
  
  DynamicJsonDocument payload(256);
  payload["code"] = owner_code;
  payload["device_id"] = device_id[0] ? device_id : "";
  payload["firmware"] = kFirmwareVersion ? kFirmwareVersion : "";
  String body;
  serializeJson(payload, body);

  // Free the 16KB camera DMA reserve so the TLS handshake below has enough
  // CONTIGUOUS internal RAM (the owner-claim runs during provisioning when the
  // internal heap is fragmented by AP_STA). The RAII guard re-acquires it on
  // EVERY exit path so the camera still has its reserve for later captures.
  // Hooks are implemented in halo_sense_prod.ino (external "C" linkage); this is
  // the same free+reacquire pattern the upload/voice TLS paths already use.
  struct DmaReserveTlsGuard {
    DmaReserveTlsGuard() { halo_tls_free_dma_reserve(); }
    ~DmaReserveTlsGuard() { halo_tls_restore_dma_reserve(); }
  } _dma_tls_guard;

  HTTPClient http;
  WiFiClientSecure secure_client;
  secure_client.setInsecure();
  
  if (!http.begin(secure_client, url)) {
    claim_in_progress = false;
    setLastError("Claim failed (HTTP begin)");
    return false;
  }
  http.setConnectTimeout(15000);
  http.setReuse(false);
  http.addHeader("Content-Type", "application/json");
  
  int http_code = http.POST(body);
  String response = http.getString();
  http.end();
  claim_in_progress = false;
  
  DynamicJsonDocument resp_doc(256);
  bool resp_ok = false;
  if (response.length() > 0) {
    DeserializationError err = deserializeJson(resp_doc, response);
    resp_ok = (!err);
  }
  const char* owner_id_str = "";
  bool owner_id_present = false;
  String error_str;
  if (resp_ok) {
    if (resp_doc.containsKey("owner_id")) {
      owner_id_str = resp_doc["owner_id"] | "";
      owner_id_present = owner_id_str && owner_id_str[0] != '\0';
    }
    if (resp_doc.containsKey("error")) {
      error_str = resp_doc["error"].as<String>();
    }
  }
  LOG_INFO("[PROVISION] Claim response http=%d ok=%d owner_id_present=%d owner_id=%s error=%s",
           http_code,
           resp_ok ? 1 : 0,
           owner_id_present ? 1 : 0,
           owner_id_present ? owner_id_str : "-",
           error_str.length() ? error_str.c_str() : "-");

  if (http_code == HTTP_CODE_OK) {
    if (owner_id_present) {
      if (prev_owner_id_set && strcmp(owner_id_str, prev_owner_id) == 0) {
        setLastError("Setup code returned existing owner_id");
        claim_completed = true;
        LOG_WARN("[PROVISION] Owner claim returned existing owner_id=%s", owner_id_str);
        return false;
      }
      ProvisioningState::saveOwnerId(owner_id_str);
      ProvisioningState::clearOwnerCode();
      owner_id_set_ms = millis();
      setLastError("");
      claim_completed = true;
      LOG_INFO("[PROVISION] Owner claim success: owner_id=%s", owner_id_str);
      return true;
    }
    setLastError("Claim failed (bad response)");
    return false;
  }
  
  if (error_str == "device_already_claimed") {
    setLastError("Device already linked");
    claim_completed = true;
    ProvisioningState::clearOwnerCode();  // Claim terminal — clear stale code so owner_id isn't suppressed
  } else if (error_str == "invalid_code") {
    setLastError("Setup code not recognized");
    claim_completed = true;
    ProvisioningState::clearOwnerCode();
  } else if (error_str == "expired_code") {
    setLastError("Setup code expired");
    claim_completed = true;
    ProvisioningState::clearOwnerCode();
  } else if (error_str == "code_already_used") {
    setLastError("Setup code already used");
    claim_completed = true;
    ProvisioningState::clearOwnerCode();
  } else if (error_str == "rate_limited") {
    setLastError("Rate limited, try again");
  } else if (http_code >= 400) {
    char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "Claim failed (HTTP %d)", http_code);
    setLastError(err_buf);
  }
  
  return false;
}

void ProvisioningManager::sendProvisionStatus(const char* state_str) {
  if (!uart_proto) {
    return;
  }
  
  // Pack PROVISION_STATUS: [state:null-term]
  uint8_t data[64];
  size_t len = strlen(state_str);
  if (len >= sizeof(data)) {
    len = sizeof(data) - 1;
  }
  memcpy(data, state_str, len);
  data[len] = '\0';
  
  uart_proto->send_frame(MSG_PROVISION_STATUS, 0, data, len + 1);
  LOG_INFO("[PROVISION] Sent PROVISION_STATUS: %s", state_str);
}

// HTTP Handler implementations (static functions)

void handleInfo() {
  if (!g_provisioning_manager) {
    server->send(500, "application/json", "{\"error\":\"server_error\"}");
    return;
  }

  noteProvisionRequestContext("GET /info");

  DynamicJsonDocument doc(768);
  doc["status"] = "ok";
  doc["api_version"] = kProvisioningApiVersion;
  doc["transport"] = kProvisioningTransport;
  doc["owner_flow"] = kProvisioningOwnerFlow;
  doc["device_id"] = g_provisioning_manager->getDeviceId();
  doc["ap_ssid"] = g_provisioning_manager->getApSsid();
  doc["ap_password"] = g_provisioning_manager->getApPassword();  // Deterministic password (MAC-seeded)
  doc["ap_ip"] = WiFi.softAPIP().toString();
  doc["state"] = ProvisioningState::getStateString();
  doc["app_session_active"] = g_provisioning_manager->isAppSessionActive(millis());
  doc["last_client"] = g_provisioning_manager->getLastProvisionClient();
  
  String response;
  serializeJson(doc, response);
  
  LOG_INFO("[HTTP] GET /info → %s", response.c_str());
  server->send(200, "application/json", response);
}

void handleScan() {
  noteProvisionRequestContext("GET /scan");

  updateProvisionScanState();
  unsigned long now = millis();
  bool force_refresh = requestArgIsTrue("force") || requestArgIsTrue("refresh");

  if (force_refresh) {
    if (!g_provisioning_manager->isAppSessionActive(now)) {
      LOG_WARN("[HTTP] GET /scan force requested without active app session");
    } else if (g_scan_inflight || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
      LOG_INFO("[HTTP] GET /scan force → scan already running");
      server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
      server->send(202, "application/json", "{\"api_version\":1,\"status\":\"scanning\",\"networks\":[]}");
      return;
    } else if ((now - g_scan_forced_ms) < kProvisionScanForceCooldownMs) {
      LOG_INFO("[HTTP] GET /scan force → cooldown active, serving latest cache");
      if (g_scan_cached_response.length() > 0) {
        server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        server->send(200, "application/json", g_scan_cached_response);
        return;
      }
    } else {
      LOG_INFO("[HTTP] GET /scan force → starting async passive refresh");
      clearProvisionScanCache();
      int start_rc = WiFi.scanNetworks(true, false, true, kProvisionScanMaxMsPerChan);
      if (start_rc == WIFI_SCAN_FAILED) {
        LOG_ERROR("[HTTP] Forced async scan start failed");
        server->send(500, "application/json", "{\"api_version\":1,\"status\":\"error\",\"error\":\"scan_failed\"}");
        return;
      }
      g_scan_inflight = true;
      g_scan_started_ms = now;
      g_scan_forced_ms = now;
      server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
      server->send(202, "application/json", "{\"api_version\":1,\"status\":\"scanning\",\"networks\":[]}");
      return;
    }
  }

  if (g_scan_cached_response.length() > 0 && (now - g_scan_cached_ms) <= kProvisionScanCacheMs) {
    LOG_INFO("[HTTP] GET /scan → cached");
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server->send(200, "application/json", g_scan_cached_response);
    return;
  }

  uint8_t ap_clients = WiFi.softAPgetStationNum();
  if (ap_clients > 0 && g_scan_cached_response.length() > 0) {
    LOG_WARN("[HTTP] GET /scan → serving stale cache to avoid AP disruption (clients=%u age_ms=%lu)",
             (unsigned)ap_clients,
             (unsigned long)(now - g_scan_cached_ms));
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server->send(200, "application/json", g_scan_cached_response);
    return;
  }

  if (g_scan_inflight || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server->send(202, "application/json", "{\"api_version\":1,\"status\":\"scanning\",\"networks\":[]}");
    return;
  }

  clearProvisionScanCache();
  int start_rc = WiFi.scanNetworks(true, false, true, kProvisionScanMaxMsPerChan);
  if (start_rc == WIFI_SCAN_FAILED) {
    LOG_ERROR("[HTTP] Async scan start failed");
    server->send(500, "application/json", "{\"api_version\":1,\"status\":\"error\",\"error\":\"scan_failed\"}");
    return;
  }

  g_scan_inflight = true;
  g_scan_started_ms = now;
  LOG_INFO("[HTTP] Async scan started");
  server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server->send(202, "application/json", "{\"api_version\":1,\"status\":\"scanning\",\"networks\":[]}");
}

void handleWifiPost() {
  if (!g_provisioning_manager) {
    server->send(500, "application/json", "{\"error\":\"server_error\"}");
    return;
  }

  noteProvisionRequestContext("POST /wifi");
  
  if (!server->hasArg("plain")) {
    g_provisioning_manager->setLastError("Missing request body");
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"missing_body\"}");
    return;
  }
  
  String body = server->arg("plain");
  
  // Validate JSON size (prevent overflow)
  if (body.length() > 512) {
    char err_msg[128];
    snprintf(err_msg, sizeof(err_msg), "Request body too large: %d bytes", body.length());
    g_provisioning_manager->setLastError(err_msg);
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"body_too_large\"}");
    return;
  }
  
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    char err_msg[128];
    snprintf(err_msg, sizeof(err_msg), "JSON parse error: %s", error.c_str());
    g_provisioning_manager->setLastError(err_msg);
    LOG_ERROR("[HTTP] JSON parse error: %s", error.c_str());
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"json_parse_error\"}");
    return;
  }
  
  if (!doc.containsKey("ssid")) {
    g_provisioning_manager->setLastError("Missing required field: ssid");
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"missing_ssid\"}");
    return;
  }
  
  if (!doc.containsKey("password")) {
    g_provisioning_manager->setLastError("Missing required field: password");
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"missing_password\"}");
    return;
  }
  
  String ssid = doc["ssid"].as<String>();
  String password = doc["password"].as<String>();
  
  // Validate SSID length (Wi-Fi SSID max 32 bytes)
  if (ssid.length() == 0) {
    g_provisioning_manager->setLastError("SSID cannot be empty");
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"empty_ssid\"}");
    return;
  }
  
  if (ssid.length() > 32) {
    char err_msg[128];
    snprintf(err_msg, sizeof(err_msg), "SSID too long: %d bytes (max 32)", ssid.length());
    g_provisioning_manager->setLastError(err_msg);
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"ssid_too_long\"}");
    return;
  }
  
  // Validate password length (Wi-Fi password max 63 bytes for WPA2)
  if (password.length() > 63) {
    char err_msg[128];
    snprintf(err_msg, sizeof(err_msg), "Password too long: %d bytes (max 63)", password.length());
    g_provisioning_manager->setLastError(err_msg);
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"password_too_long\"}");
    return;
  }

  String owner_code = doc["owner_code"] | "";
  if (owner_code.length() == 0) {
    owner_code = doc["ownerCode"] | "";
  }
  if (owner_code.length() == 0) {
    g_provisioning_manager->setLastError("Missing setup code");
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"missing_owner_code\"}");
    return;
  }
  if (owner_code.length() > 31) {
    g_provisioning_manager->setLastError("Setup code too long");
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"owner_code_too_long\"}");
    return;
  }
  char owner_code_norm[32];
  if (!normalize_owner_code(owner_code.c_str(), owner_code_norm, sizeof(owner_code_norm))) {
    g_provisioning_manager->setLastError("Invalid setup code");
    server->send(400, "application/json", "{\"error\":\"invalid_payload\",\"reason\":\"owner_code_invalid\"}");
    return;
  }
  ProvisioningState::clearOwnerId();
  ProvisioningState::saveOwnerCode(owner_code_norm);

  String direct_owner_id = doc["owner_id"] | "";
  if (direct_owner_id.length() == 0) {
    direct_owner_id = doc["ownerId"] | "";
  }
  if (direct_owner_id.length() > 0) {
    LOG_INFO("[HTTP] POST /wifi included owner_id hint; ignoring while owner_code claim is primary");
  }
  
  LOG_INFO("[HTTP] Received credentials: ssid=%s, password_len=%d", ssid.c_str(), password.length());
  
  // Store target SSID for /status reporting
  strncpy(g_provisioning_manager->target_home_ssid, ssid.c_str(), sizeof(g_provisioning_manager->target_home_ssid) - 1);
  g_provisioning_manager->target_home_ssid[sizeof(g_provisioning_manager->target_home_ssid) - 1] = '\0';
  LOG_INFO("[PROVISION] target_ssid set to: %s", g_provisioning_manager->target_home_ssid);
  
  g_provisioning_manager->setLastError("");  // Clear error on success
  
  // Start connection (non-blocking)
  g_provisioning_manager->startHomeWifiConnectPublic(ssid.c_str(), password.c_str());
  
  // Truth logger: WiFi credentials posted
  dump_system_truth("wifi_posted");
  
  // Respond immediately
  DynamicJsonDocument responseDoc(256);
  responseDoc["api_version"] = kProvisioningApiVersion;
  responseDoc["status"] = "connecting";
  responseDoc["wifi_state"] = "connecting";
  responseDoc["owner_flow"] = kProvisioningOwnerFlow;
  responseDoc["connect_timeout_ms"] = ProvisioningManager::CONNECTING_TIMEOUT_MS;
  responseDoc["connect_max"] = ProvisioningManager::CONNECT_MAX_RETRIES + 1;
  String response;
  serializeJson(responseDoc, response);
  
  server->send(200, "application/json", response);
}

void handleStatus() {
  if (!g_provisioning_manager) {
    server->send(500, "application/json", "{\"error\":\"server_error\"}");
    return;
  }

  noteProvisionRequestContext("GET /status");

  DynamicJsonDocument doc(768);
  doc["status"] = "ok";
  doc["api_version"] = kProvisioningApiVersion;
  doc["transport"] = kProvisioningTransport;
  doc["owner_flow"] = kProvisioningOwnerFlow;
  doc["device_id"] = g_provisioning_manager->getDeviceId();
  doc["state"] = ProvisioningState::getStateString();
  doc["provisioned"] = ProvisioningState::isProvisioned();
  doc["connect_attempt"] = g_provisioning_manager->connect_attempt;
  doc["connect_max"] = ProvisioningManager::CONNECT_MAX_RETRIES + 1;
  doc["connect_timeout_ms"] = ProvisioningManager::CONNECTING_TIMEOUT_MS;
  
  // Get wifi_state from provisioning state machine
  const char* wifi_state = "failed";  // Default fallback
  ProvisioningState::State state = ProvisioningState::getState();
  wl_status_t wifi_status = WiFi.status();
  
  if (wifi_status == WL_CONNECTED) {
    wifi_state = "connected";
  } else if (state == ProvisioningState::STATE_AP_SETUP || state == ProvisioningState::STATE_UNPROVISIONED) {
    wifi_state = "ap_setup";
  } else if (state == ProvisioningState::STATE_CONNECTING_HOME_WIFI) {
    wifi_state = "connecting";
  } else if (state == ProvisioningState::STATE_ERROR) {
    wifi_state = "failed";
  }
  
  // Log resolved wifi_state for debugging
  LOG_INFO("[PROVISION] handleStatus: state=%s, wifi_state=%s wifi=%d", 
           ProvisioningState::getStateString(state), wifi_state, (int)wifi_status);
  
  doc["wifi_state"] = wifi_state;
  
  // Get home_ssid: use target_home_ssid if set, otherwise try loading from NVS
  if (g_provisioning_manager->target_home_ssid[0] != '\0') {
    doc["home_ssid"] = g_provisioning_manager->target_home_ssid;
  } else {
    // Fallback to NVS if target_home_ssid not set (e.g., after reboot)
    char home_ssid[64];
    if (ProvisioningState::loadHomeWifiCreds(home_ssid, sizeof(home_ssid), nullptr, 0)) {
      doc["home_ssid"] = home_ssid;
    } else {
      doc["home_ssid"] = "";
    }
  }
  
  // Add last_error if present (access via getLastError())
  const char* last_error = g_provisioning_manager->getLastError();
  if (last_error && last_error[0] != '\0') {
    doc["last_error"] = last_error;
  } else {
    doc["last_error"] = "";
  }
  doc["app_session_active"] = g_provisioning_manager->isAppSessionActive(millis());
  doc["last_client"] = g_provisioning_manager->getLastProvisionClient();
  doc["last_app_version"] = g_provisioning_manager->getLastAppVersion();
  
  // Always include owner_id and owner_id_set for test verification
  char owner_id[64];
  bool owner_id_set = ProvisioningState::loadOwnerId(owner_id, sizeof(owner_id));
  if (owner_id_set) {
    doc["owner_id"] = owner_id;
  } else {
    doc["owner_id"] = "";
  }
  doc["owner_id_set"] = owner_id_set;
  
  String response;
  serializeJson(doc, response);
  
  LOG_INFO("[HTTP] GET /status → %s", response.c_str());
  server->send(200, "application/json", response);
}

void handleUserIdPost() {
  if (!g_provisioning_manager) {
    server->send(500, "application/json", "{\"error\":\"server_error\"}");
    return;
  }

  noteProvisionRequestContext("POST /user-id");
  
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"invalid_payload\"}");
    return;
  }
  
  String body = server->arg("plain");
  
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    LOG_ERROR("[HTTP] Invalid JSON for /user-id");
    server->send(400, "application/json", "{\"error\":\"invalid_payload\"}");
    return;
  }

  String owner_id = doc["owner_id"] | "";
  if (owner_id.length() == 0) {
    owner_id = doc["ownerId"] | "";
  }
  if (owner_id.length() == 0) {
    LOG_ERROR("[HTTP] Missing owner_id");
    server->send(400, "application/json", "{\"error\":\"missing_owner_id\"}");
    return;
  }
  
  LOG_INFO("[HTTP] Received owner_id: %s", owner_id.c_str());
  
  ProvisioningState::saveOwnerId(owner_id.c_str());
  
  // Record when owner_id was set (extends SoftAP grace period)
  if (g_provisioning_manager && g_provisioning_manager->getOwnerIdSetMs() == 0) {
    g_provisioning_manager->setOwnerIdSetMs(millis());
    LOG_INFO("[PROVISION] Owner ID set, extending SoftAP grace period");
  }
  
  DynamicJsonDocument responseDoc(192);
  responseDoc["api_version"] = kProvisioningApiVersion;
  responseDoc["status"] = "success";
  responseDoc["owner_id"] = owner_id;
  String response;
  serializeJson(responseDoc, response);
  
  server->send(200, "application/json", response);
}

void handleRoot() {
  noteProvisionRequestContext("GET /");
  bool explicit_portal = requestArgIsTrue("portal");
  if (!explicit_portal && !requestLooksInteractiveBrowser()) {
    sendNoContentResponse("GET /", "non_interactive_client");
    return;
  }
  if (shouldSuppressCaptivePortalUi() && !explicit_portal) {
    sendNoContentResponse("GET /", "app_session_active");
    return;
  }
  // Serve simple web portal HTML
  const char* html = R"HTML(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Halo Provisioning</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 400px; margin: 20px auto; padding: 20px; }
    h1 { color: #333; }
    button { background: #007bff; color: white; border: none; padding: 10px 20px; border-radius: 4px; cursor: pointer; width: 100%; margin: 10px 0; }
    button:hover { background: #0056b3; }
    .secondary-btn { background: #6b7280; }
    .secondary-btn:hover { background: #4b5563; }
    input { width: 100%; padding: 8px; margin: 5px 0; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }
    .network-item { padding: 10px; border: 1px solid #ddd; margin: 5px 0; border-radius: 4px; cursor: pointer; }
    .network-item:hover { background: #f0f0f0; }
    .status { padding: 10px; margin: 10px 0; border-radius: 4px; }
    .status.connecting { background: #fff3cd; }
    .status.success { background: #d4edda; }
    .status.error { background: #f8d7da; }
    #page1, #page2, #page3, #page4 { display: none; }
    #page1.active, #page2.active, #page3.active, #page4.active { display: block; }
  </style>
</head>
<body>
  <div id="page1" class="active">
    <h1>Halo Wi-Fi Setup</h1>
    <p>Device: <span id="device-id">-</span></p>
    <p>AP SSID: <span id="ap-ssid">-</span></p>
    <p>If the portal does not stay open automatically, use <a href="http://192.168.4.1/portal">http://192.168.4.1/portal</a>.</p>
    <button onclick="scanNetworks()">Scan Networks</button>
  </div>
  
  <div id="page2">
    <h1>Select Network</h1>
    <p id="scan-message" style="color: #666;">Tap "Scan Networks" to look for nearby Wi-Fi. If your network is missing, use "Refresh Networks" or enter it manually below.</p>
    <div id="network-list"><p style="color: #666;">No scan results yet.</p></div>
    <p style="margin-top: 20px; font-size: 0.9em; color: #666;">Enter network manually if scan results are incomplete, hidden, or 5 GHz-only:</p>
    <input type="text" id="manual-ssid" placeholder="Network Name (SSID)" style="margin-top: 10px;">
    <input type="password" id="wifi-password" placeholder="Wi-Fi Password">
    <input type="text" id="setup-code" placeholder="Setup Code (e.g., AB12-CD34)">
    <button onclick="connectWifi()">Connect</button>
    <button onclick="showPage(1)">Back</button>
    <button class="secondary-btn" onclick="scanNetworks(true)">Refresh Networks</button>
  </div>
  
  <div id="page3">
    <h1>Connecting...</h1>
    <div id="status" class="status connecting">Connecting to Wi-Fi...</div>
    <button id="try-again-btn" style="display: none; margin-top: 12px;" onclick="showPage(2)">Try again</button>
  </div>
  
  <div id="page4">
    <h1>Success!</h1>
    <div class="status success">Wi-Fi connected successfully. You can close this tab.</div>
  </div>
  
  <script>
    let selectedSsid = '';
    let statusPollInterval = null;

    function setScanMessage(message, isError=false) {
      const el = document.getElementById('scan-message');
      if (!el) return;
      el.textContent = message;
      el.style.color = isError ? '#d32f2f' : '#666';
    }
    
    function blurInputs() {
      const manual = document.getElementById('manual-ssid');
      const pass = document.getElementById('wifi-password');
      const code = document.getElementById('setup-code');
      if (manual) manual.blur();
      if (pass) pass.blur();
      if (code) code.blur();
      const active = document.activeElement;
      if (active && active.tagName === 'INPUT') {
        active.blur();
      }
    }

    function showPage(n) {
      for (let i = 1; i <= 4; i++) {
        document.getElementById('page' + i).classList.remove('active');
      }
      document.getElementById('page' + n).classList.add('active');
      if (n === 2) {
        setTimeout(() => {
          blurInputs();
        }, 0);
      }
    }
    
    async function loadInfo() {
      try {
        const res = await fetch('/info', {cache: 'no-store'});
        const data = await res.json();
        document.getElementById('device-id').textContent = data.device_id || '-';
        document.getElementById('ap-ssid').textContent = data.ap_ssid || '-';
        // Note: ap_password available in data.ap_password if needed for display
      } catch (e) {
        console.error('Failed to load info:', e);
      }
    }
    
    async function scanNetworks(forceRefresh = false) {
      const list = document.getElementById('network-list');
      showPage(2);
      selectedSsid = '';
      setScanMessage(forceRefresh
        ? 'Refreshing nearby networks. If yours still does not appear, enter it manually below.'
        : 'Scanning nearby networks. The first scan may use cached results to keep setup Wi-Fi stable.');
      list.innerHTML = `<p style="color: #666;">${forceRefresh ? 'Refreshing nearby networks...' : 'Scanning nearby networks...'}</p>`;
      await pollScanResults(0, forceRefresh);
      blurInputs();
    }

    async function pollScanResults(attempt, forceRefresh) {
      try {
        const path = forceRefresh && attempt === 0 ? '/scan?force=1' : '/scan';
        const res = await fetch(path, {cache: 'no-store'});
        const data = await res.json();
        const networks = data.networks || data;
        const list = document.getElementById('network-list');

        if (res.status === 202 || data.status === 'scanning') {
          setScanMessage(forceRefresh
            ? 'Refreshing nearby networks. This can take a few seconds.'
            : 'Scanning nearby networks...');
          list.innerHTML = `<p style="color: #666;">${forceRefresh ? 'Refreshing nearby networks...' : 'Scanning nearby networks...'}</p>`;
          if (attempt < 20) {
            setTimeout(() => pollScanResults(attempt + 1, false), 1000);
          } else {
            setScanMessage('Scan is taking longer than expected. You can still enter your network manually below.', true);
            list.innerHTML = '<p style="color: #d32f2f;">Scan is taking longer than expected. You can still enter your network manually below.</p>';
          }
          return;
        }

        list.innerHTML = '';

        if (!networks || networks.length === 0) {
          setScanMessage(forceRefresh
            ? 'Refresh complete, but no networks were found. Enter the SSID manually below.'
            : 'No networks found yet. Enter the SSID manually below or try Refresh Networks.');
          list.innerHTML = '<p style="color: #666; font-style: italic;">No networks found. Please enter network name manually below.</p>';
          return;
        }

        setScanMessage(forceRefresh
          ? 'Refresh complete. Select your network, or enter it manually if it is still missing.'
          : 'Showing nearby networks. If yours is missing, use Refresh Networks or enter it manually below.');

        networks.forEach(net => {
          const div = document.createElement('div');
          div.className = 'network-item';
          div.innerHTML = `<strong>${net.ssid}</strong> (RSSI: ${net.rssi}) ${net.secured ? '🔒' : ''}`;
          div.onclick = () => {
            selectedSsid = net.ssid;
            document.getElementById('manual-ssid').value = net.ssid;
          };
          list.appendChild(div);
        });
      } catch (e) {
        console.error('Scan failed:', e);
        const list = document.getElementById('network-list');
        setScanMessage('Scan failed. You can still enter your network name manually below.', true);
        list.innerHTML = '<p style="color: #d32f2f;">Scan failed. Please enter network name manually below.</p>';
      }
    }
    
    async function connectWifi() {
      // Use manual SSID if provided, otherwise use selected network
      const manualSsid = document.getElementById('manual-ssid').value.trim();
      const ssid = manualSsid || selectedSsid;
      
      if (!ssid) {
        alert('Please select a network or enter network name');
        return;
      }
      
      const password = document.getElementById('wifi-password').value;
      const setupCode = document.getElementById('setup-code').value.trim();
      
      if (!setupCode) {
        alert('Please enter your setup code');
        return;
      }
      
      try {
        const res = await fetch('/wifi', {
          method: 'POST',
          cache: 'no-store',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({ssid: ssid, password: password, owner_code: setupCode})
        });
        
        if (res.ok) {
          showPage(3);
          startStatusPoll();
        } else {
          let message = 'Failed to connect';
          try {
            const data = await res.json();
            if (data.reason) message = data.reason;
          } catch (_) {}
          alert(message);
        }
      } catch (e) {
        console.error('Connect failed:', e);
        alert('Failed to connect');
      }
    }
    
    function startStatusPoll() {
      let pollCount = 0;
      const maxPolls = 100; // 100 * 1s = 100s (covers 45s + retry + 45s device-side)
      const tryAgainBtn = document.getElementById('try-again-btn');
      if (tryAgainBtn) tryAgainBtn.style.display = 'none';
      
      statusPollInterval = setInterval(async () => {
        pollCount++;
        
        try {
          const res = await fetch('/status', {cache: 'no-store'});
          const data = await res.json();
          
          const statusDiv = document.getElementById('status');
          if (data.wifi_state === 'connected') {
            if (data.owner_id_set) {
              clearInterval(statusPollInterval);
              showPage(4);
            } else if (data.last_error && data.last_error.length) {
              clearInterval(statusPollInterval);
              statusDiv.className = 'status error';
              statusDiv.textContent = data.last_error;
              if (tryAgainBtn) tryAgainBtn.style.display = 'block';
            } else {
              statusDiv.className = 'status connecting';
              statusDiv.textContent = 'Connected. Claiming owner...';
            }
          } else if (data.wifi_state === 'failed') {
            clearInterval(statusPollInterval);
            statusDiv.className = 'status error';
            statusDiv.textContent = (data.last_error && data.last_error.length) ? data.last_error : 'Connection failed. Please try again.';
            if (tryAgainBtn) tryAgainBtn.style.display = 'block';
          } else {
            const attempt = (data.connect_attempt !== undefined ? data.connect_attempt : 0) + 1;
            const maxAttempts = (data.connect_max !== undefined ? data.connect_max : 2);
            let message = `Connecting... (${pollCount}s)`;
            if (maxAttempts > 0) {
              message = `Connecting... (attempt ${attempt}/${maxAttempts})`;
            }
            if (data.last_error && data.last_error.length) {
              message += `\n${data.last_error} (retrying)`;
            }
            statusDiv.className = 'status connecting';
            statusDiv.textContent = message;
          }
        } catch (e) {
          console.error('Status poll failed:', e);
        }
        
        if (pollCount >= maxPolls) {
          clearInterval(statusPollInterval);
          const statusDiv = document.getElementById('status');
          statusDiv.className = 'status error';
          statusDiv.textContent = 'Connection timeout. Please try again.';
          if (tryAgainBtn) tryAgainBtn.style.display = 'block';
        }
      }, 1000); // Poll every 1s per API contract
    }
    
    // Load device info on page load. Network scanning is user-triggered.
    loadInfo();
  </script>
</body>
</html>
)HTML";

  server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server->sendHeader("Pragma", "no-cache");
  server->send(200, "text/html", html);
  LOG_INFO("[HTTP] GET / → serving web portal explicit=%d", explicit_portal ? 1 : 0);
}

void handlePortalOverride() {
  if (!server) {
    return;
  }
  noteProvisionRequestContext("GET /portal");
  server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server->sendHeader("Pragma", "no-cache");
  server->sendHeader("Location", "/?portal=1");
  server->send(302, "text/plain", "");
  LOG_INFO("[HTTP] GET /portal → redirect /?portal=1");
}

void handleCaptivePortal() {
  if (!server) {
    return;
  }
  noteProvisionRequestContext("GET captive");

  // Apple's CaptiveNetworkSupport agent checks /hotspot-detect.html and
  // /library/test/success.html.  It expects HTTP 200 with body containing
  // "Success".  Any other response (including 204) makes iOS mark the
  // network as "no internet" and drop the WiFi connection — which breaks
  // app-based provisioning.  Always return the Apple success response for
  // connectivity probes, regardless of client type or app session state.
  String uri = server->uri();
  if (uri == "/hotspot-detect.html" || uri == "/library/test/success.html") {
    server->sendHeader("Cache-Control", "no-store");
    server->send(200, "text/html",
      "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    LOG_INFO("[HTTP] GET %s → 200 Apple success (keep WiFi alive)", uri.c_str());
    return;
  }

  // Android / Windows connectivity checks — 204 is the expected response
  if (!requestLooksInteractiveBrowser()) {
    sendNoContentResponse("GET captive", "non_interactive_client");
    return;
  }
  if (shouldSuppressCaptivePortalUi()) {
    sendNoContentResponse("GET captive", "app_session_active");
    return;
  }
  handleRoot();
}

void handleNotFound() {
  noteProvisionRequestContext("GET not_found");
  if (!requestLooksInteractiveBrowser()) {
    sendNoContentResponse("GET not_found", "non_interactive_client");
    return;
  }
  if (shouldSuppressCaptivePortalUi()) {
    sendNoContentResponse("GET not_found", "app_session_active");
    return;
  }
  handleRoot();
}

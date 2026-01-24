/*
 * HALO SENSE - Phase 1
 * 
 * Phase 1: Sense Wi-Fi + Sense OTA (LCD unchanged except UART gating cleanup)
 * - Boot banner with version, reset reason, OTA partition info
 * - UART coordination protocol (SYNC handshake, heartbeat, version, status)
 * - HealthGate state machine (marks valid after criteria met)
 * - Hard header framing with magic bytes (0xA5 0x5A)
 * - Wi-Fi connection management
 * - OTA manifest fetch and firmware update
 * 
 * NO AWS IoT yet - manifest is HTTP(S) GET to existing endpoint.
 */

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <string.h>  // For strnlen

// Compile-time guard: Ensure RTC_DATA_ATTR is not used for boot/OTA tracking
// This header provides compile-time documentation and static assertion
#include "../shared/BootStateGuard.h"

#include "../shared/Version.h"
#include "../shared/Log.h"
#include "../shared/UartProto.h"
#include "../shared/HealthGate.h"
#include "../shared/WifiManager.h"
#include "../shared/ManifestClient.h"
#include "../shared/SenseOtaApplier.h"
#include "../shared/SenseDownloadVerifier.h"
#include "../shared/OtaExpect.h"
#include "../shared/BootState.h"
#include <esp_timer.h>

// ============================================================================
// CONFIGURATION
// ============================================================================
// UART pins (XIAO ESP32S3 - D6=GPIO43, D7=GPIO44)
// Must match LCD board: Sense TX (43) → LCD RX (48), Sense RX (44) → LCD TX (38)
#define UART_TX_PIN 43  // D6 on XIAO ESP32S3
#define UART_RX_PIN 44  // D7 on XIAO ESP32S3
#define UART_BAUD 115200

#define HEARTBEAT_INTERVAL_MS 2000  // 2 seconds
#define SYNC_INTERVAL_MS 250        // Send SYNC every 250ms until ACK received

// Version request retry configuration
#define VERSION_REQUEST_RETRY_DELAYS_MS {250, 500, 1000}
#define VERSION_REQUEST_MAX_RETRIES 3

// Wi-Fi configuration (set these for your network)
#define WIFI_SSID "Garage Member"
#define WIFI_PASSWORD "build00!"

// OTA manifest URL (versioned to avoid caching)
// Set this to your hosted manifest URL (GitHub Pages / Netlify)
// Use manifest_latest.json so device always checks latest without reflashing
// Versioned manifests (manifest_0.5.x.json) are still created for rollback/debug
// Example: "https://your-username.github.io/repo-name/manifest_latest.json"
#define OTA_MANIFEST_URL_BASE "https://mattrtaylor71.github.io/halo-ota-manifest/manifest_latest.json"

// Test mode: set to true to run without LCD connected
#define TEST_MODE_NO_LCD false

// OTA modes:
// - OTA_DRY_RUN: Download and verify SHA256 only (no write, no reboot)
// - OTA_APPLY_AND_REBOOT: Write to OTA partition, set boot partition, and reboot
#define OTA_DRY_RUN false
#define OTA_APPLY_AND_REBOOT true

// Artifact verification:
// - ALLOW_UNVERIFIED_MANIFEST: If true, allow OTA apply even if manifest lacks build_id/artifact_fw_version
//   (default: false for production safety)
#define ALLOW_UNVERIFIED_MANIFEST false

// OTA apply cooldown: minimum time between OTA apply attempts (milliseconds)
#define OTA_APPLY_COOLDOWN_MS (5 * 60 * 1000)  // 5 minutes

// OTA check: check once at boot after 5 seconds (if Wi-Fi connected)

// ============================================================================
// GLOBALS
// ============================================================================
// Boot identity and OTA tracking now stored in NVS via BootState namespace
// (RTC_DATA_ATTR doesn't persist across ESP.restart() on ESP32-S3)

// Boot count (set in setup() via BootState::nextBootCount())
uint32_t g_boot_count = 0;

HardwareSerial lcdSerial(1);
UartProto* uart_proto = NULL;
HealthGate health_gate;
WifiManager wifi_manager;
ManifestClient manifest_client;
SenseOtaApplier ota_applier;
SenseDownloadVerifier download_verifier;

unsigned long last_heartbeat_ms = 0;
unsigned long boot_time_ms = 0;
uint16_t heartbeat_seq = 0;

// SYNC handshake state
bool sync_established = false;
unsigned long last_sync_ms = 0;

// Version request state
bool lcd_version_received = false;
uint8_t version_request_attempt = 0;
const unsigned long version_retry_delays_ms[VERSION_REQUEST_MAX_RETRIES] = VERSION_REQUEST_RETRY_DELAYS_MS;
unsigned long next_version_request_ms = 0;

// STATUS message logging throttle
unsigned long last_status_log_ms = 0;
#define STATUS_LOG_THROTTLE_MS 5000  // Log STATUS at most once every 5 seconds

// OTA state
bool ota_check_done = false;
unsigned long last_ota_check_ms = 0;
bool wifi_connected_reported = false;
bool ota_apply_done = false;  // Track if OTA apply was attempted this boot

// OTA validation state (per-boot, RAM only)
bool g_ota_validated_this_boot = false;
unsigned long g_ota_validated_time_ms = 0;

// ============================================================================
// HELPERS
// ============================================================================
void printBootBanner(esp_reset_reason_t reset_reason);

const char* getResetReasonString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

void printBootBanner(esp_reset_reason_t reset_reason) {
  Serial.println("\n========================================");
  Serial.println("HALO SENSE -- Phase 1");
  Serial.println("========================================");
  
  // Version info
  BuildInfo build;
  Serial.print("Firmware Version: ");
  Serial.println(build.version);
  Serial.print("Build ID: ");
  Serial.println(build.build_id);
  Serial.print("Build Date: ");
  Serial.print(build.build_date);
  Serial.print(" ");
  Serial.println(build.build_time);
  Serial.print("Git Hash: ");
  Serial.println(build.git_hash);
  
  // UART pins
  Serial.print("UART Pins: TX=");
  Serial.print(UART_TX_PIN);
  Serial.print(", RX=");
  Serial.println(UART_RX_PIN);
  Serial.print("UART Baud: ");
  Serial.println(UART_BAUD);
  
  // Reset reason (use passed value)
  Serial.print("Reset Reason: ");
  Serial.print(getResetReasonString(reset_reason));
  Serial.print(" (");
  Serial.print(reset_reason);
  Serial.println(")");
  
  // OTA partition info
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    Serial.print("Running Partition: ");
    Serial.print(running->label);
    Serial.print(" (offset: 0x");
    Serial.print(running->address, HEX);
    Serial.print(", size: ");
    Serial.print(running->size);
    Serial.println(")");
  }
  
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  if (boot) {
    Serial.print("Boot Partition: ");
    Serial.print(boot->label);
    Serial.print(" (offset: 0x");
    Serial.print(boot->address, HEX);
    Serial.println(")");
  }
  
  // OTA state information
  if (running) {
    esp_ota_img_states_t ota_state;
    esp_err_t ota_err = esp_ota_get_state_partition(running, &ota_state);
    if (ota_err == ESP_OK) {
      const char* state_str = "UNKNOWN";
      switch (ota_state) {
        case ESP_OTA_IMG_NEW: state_str = "NEW"; break;
        case ESP_OTA_IMG_PENDING_VERIFY: state_str = "PENDING_VERIFY"; break;
        case ESP_OTA_IMG_VALID: state_str = "VALID"; break;
        case ESP_OTA_IMG_INVALID: state_str = "INVALID"; break;
        case ESP_OTA_IMG_ABORTED: state_str = "ABORTED"; break;
        case ESP_OTA_IMG_UNDEFINED: state_str = "UNDEFINED"; break;
        default: state_str = "UNKNOWN"; break;
      }
      Serial.print("OTA State: ");
      Serial.print(state_str);
      Serial.print(" (");
      Serial.print(ota_state);
      Serial.println(")");
    } else {
      Serial.print("OTA State: Error reading state (");
      Serial.print(esp_err_to_name(ota_err));
      Serial.println(")");
    }
  }
  
  // Test mode
  if (TEST_MODE_NO_LCD) {
    Serial.println("TEST MODE: LCD disabled");
  }
  
  Serial.println("========================================");
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  // Initialize Serial for USB CDC (needed for nextBootCount logging)
  Serial.begin(115200);
  
  // Non-blocking wait for USB CDC (max 2 seconds)
  unsigned long usb_wait_start = millis();
  while (!Serial && (millis() - usb_wait_start < 2000)) {
    delay(10);
  }
  
  if (Serial) {
    delay(100);
  }
  
  // Get next boot count from NVS (BEFORE constructing manifest URL)
  // This function opens NVS, increments, closes, reopens to verify, and logs details
  g_boot_count = BootState::nextBootCount();
  
  // Capture reset reason ONCE
  esp_reset_reason_t rr = esp_reset_reason();
  
  // Print boot identity (boot count already logged by nextBootCount)
  Serial.print("[BOOT] boot_count=");
  Serial.print(g_boot_count);
  Serial.print("(NVS) reset_reason=");
  Serial.print(getResetReasonString(rr));
  Serial.print("(");
  Serial.print(rr);
  Serial.print(") free_heap=");
  Serial.println(ESP.getFreeHeap());
  
  // Print boot banner (pass captured reset reason)
  printBootBanner(rr);

  // Build provenance prints
  Serial.print("[BUILD] Version.h __FILE__ = ");
  Serial.println(VERSION_H_PATH);

  Serial.print("[BUILD] FIRMWARE_VERSION = ");
  Serial.println(FIRMWARE_VERSION);

  Serial.print("[BUILD] BUILD_ID = ");
  Serial.println(BUILD_ID);

  Serial.print("[BUILD] FW_EMBED_MARKER = ");
  Serial.println(getFwEmbedMarker());

  // --------------------------------------------------------------------------
  // Post-OTA expectation validation
  // --------------------------------------------------------------------------
  if (OtaExpect::isPending()) {
    char expected_version[OtaExpect::EXPECTED_VERSION_MAX_LEN + 1];
    expected_version[0] = '\0';
    bool have_expected = OtaExpect::getExpectedVersion(expected_version, sizeof(expected_version));

    const char* actual_version = FIRMWARE_VERSION;

    Serial.print("[OTA_EXPECT] Pending OTA expectation detected. expected_version=");
    Serial.print(have_expected ? expected_version : "<none>");
    Serial.print(" actual_running_version=");
    Serial.println(actual_version);

    // If we don't have an expected version recorded, clear and continue.
    if (!have_expected || expected_version[0] == '\0') {
      Serial.println("[OTA_EXPECT][WARN] Pending flag set but expected_version missing - clearing.");
      OtaExpect::clearPending();
    } else if (strcmp(actual_version, expected_version) != 0) {
      // Version mismatch - likely wrong artifact deployed.
      Serial.print("[OTA_EXPECT][ERROR] Version mismatch after OTA. Expected ");
      Serial.print(expected_version);
      Serial.print(" but running ");
      Serial.print(actual_version);
      Serial.println(".");
      Serial.print("[OTA_EXPECT][ERROR] Hosted artifact likely wrong: manifest says ");
      Serial.print(expected_version);
      Serial.print(" but binary reports ");
      Serial.print(actual_version);
      Serial.println(".");

      // Attempt to restore previous partition
      char prev_label[OtaExpect::PREV_LABEL_MAX_LEN + 1];
      uint32_t prev_addr = 0;
      bool have_prev = OtaExpect::getPrevPartitionInfo(prev_label, sizeof(prev_label), prev_addr);

      const esp_partition_t* prev_part = nullptr;
      if (have_prev && prev_label[0] != '\0') {
        prev_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                             ESP_PARTITION_SUBTYPE_ANY,
                                             prev_label);
      }
      // Fallback: search by address if label lookup failed
      if (!prev_part && prev_addr != 0) {
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                         ESP_PARTITION_SUBTYPE_ANY,
                                                         nullptr);
        while (it) {
          const esp_partition_t* p = esp_partition_get(it);
          if (p && p->address == prev_addr) {
            prev_part = p;
            break;
          }
          it = esp_partition_next(it);
        }
        esp_partition_iterator_release(it);
      }

      if (prev_part) {
        Serial.print("[OTA_EXPECT][INFO] Reverting to previous partition: ");
        Serial.print(prev_part->label);
        Serial.print(" (0x");
        Serial.print(prev_part->address, HEX);
        Serial.println(")");

        esp_err_t set_boot_err = esp_ota_set_boot_partition(prev_part);
        if (set_boot_err == ESP_OK) {
          Serial.println("[OTA_EXPECT][INFO] Boot partition set to previous image. Clearing pending flag and rebooting...");
          OtaExpect::clearPending();
          delay(200);
          ESP.restart();
        } else {
          Serial.print("[OTA_EXPECT][ERROR] Failed to set previous boot partition: ");
          Serial.println(esp_err_to_name(set_boot_err));
        }
      } else {
        Serial.println("[OTA_EXPECT][ERROR] Previous partition info not found - cannot revert automatically.");
      }
    } else {
      // Version matches expectation - clear pending flag and mark validated.
      Serial.print("[OTA_EXPECT][INFO] OTA expectation satisfied. Running expected version ");
      Serial.print(actual_version);
      Serial.println(".");
      OtaExpect::clearPending();
      g_ota_validated_this_boot = true;
      g_ota_validated_time_ms = millis();
    }
  }
  
  // Build cache-busted manifest URL (with query parameter to avoid stale cache)
  // Format: <base_url>?v=<firmware_version>-boot<boot_count>
  char manifest_url[512];
  snprintf(manifest_url, sizeof(manifest_url), "%s?v=%s-boot%lu", 
           OTA_MANIFEST_URL_BASE, FIRMWARE_VERSION, g_boot_count);
  
  // Log OTA manifest URL at boot (with cache-busting)
  Serial.print("[BOOT] OTA manifest URL: ");
  Serial.println(manifest_url);
  
  boot_time_ms = millis();
  
  // Initialize UART for LCD communication (unless test mode)
  if (!TEST_MODE_NO_LCD) {
    lcdSerial.setTxBufferSize(2048);
    lcdSerial.setRxBufferSize(2048);
    lcdSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    delay(100);  // Allow UART to stabilize
    
    uart_proto = new UartProto(&lcdSerial);
    
    // Boot flush: discard bytes for 400ms to clear boot garbage
    uart_proto->boot_flush();
    
    LOG_INFO_TAG(LOG_TAG_UART, "UART initialized: TX=%d, RX=%d", UART_TX_PIN, UART_RX_PIN);
    health_gate.markUartInitialized();  // Notify HealthGate that UART is initialized
    
    LOG_INFO("Starting SYNC handshake...");
    
    // Start SYNC handshake immediately after boot flush
    last_sync_ms = millis();
  } else {
    LOG_INFO("TEST MODE: UART disabled");
  }
  
  // Connect to Wi-Fi
  LOG_INFO("Connecting to Wi-Fi...");
  WifiManager::ConnectResult result = wifi_manager.connect(WIFI_SSID, WIFI_PASSWORD, 10000, 3, 2000);
  if (result == WifiManager::CONNECT_RESULT_IN_PROGRESS) {
    LOG_DEBUG("Wi-Fi connection already in progress");
  }
  
  LOG_INFO("Phase 1: Sense board ready");
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // Update Wi-Fi manager
  wifi_manager.update();
  
  // Check if Wi-Fi connected (report to health gate)
  if (wifi_manager.isConnected() && !wifi_connected_reported) {
    wifi_connected_reported = true;
    health_gate.markWifiConnected();
    
    // Disable Wi-Fi sleep to prevent OTA stalls during long downloads
    // (WifiManager also does this, but ensure it's set here too)
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    LOG_INFO("Wi-Fi connected: %s (sleep disabled for OTA stability)", wifi_manager.getIP().toString().c_str());
  }
  
  // Update health gate
  health_gate.update();
  
  unsigned long now = millis();
  
  // UART handling (only if not in test mode)
  if (!TEST_MODE_NO_LCD && uart_proto) {
    // SYNC handshake: send SYNC every 250ms until ACK received
    if (!sync_established) {
      if ((now - last_sync_ms) >= SYNC_INTERVAL_MS) {
        if (uart_proto->send_sync(0)) {
          LOG_DEBUG_TAG(LOG_TAG_UART, "SYNC sent");
        }
        last_sync_ms = now;
      }
    }
    
    // Send heartbeat every 2 seconds (only after SYNC established)
    if (sync_established && (now - last_heartbeat_ms) >= HEARTBEAT_INTERVAL_MS) {
      if (uart_proto->send_heartbeat(heartbeat_seq++)) {
        LOG_DEBUG_TAG(LOG_TAG_UART, "Heartbeat sent (seq=%d)", heartbeat_seq - 1);
      } else {
        LOG_WARN_TAG(LOG_TAG_UART, "Failed to send heartbeat");
      }
      last_heartbeat_ms = now;
    }
    
    // Request LCD version (only after SYNC established, once at boot, with retries)
    // Must fully stop once lcd_version_received=true
    if (sync_established && !lcd_version_received && now >= next_version_request_ms && next_version_request_ms > 0) {
      if (version_request_attempt <= VERSION_REQUEST_MAX_RETRIES) {
        LOG_INFO_TAG(LOG_TAG_UART, "Requesting LCD version (attempt %d/%d)...", 
                     version_request_attempt + 1, VERSION_REQUEST_MAX_RETRIES + 1);
        uart_proto->send_get_version(0);
        
        // Schedule next retry
        version_request_attempt++;
        if (version_request_attempt <= VERSION_REQUEST_MAX_RETRIES) {
          next_version_request_ms = now + version_retry_delays_ms[version_request_attempt - 1];
        } else {
          // No more retries - give up
          LOG_WARN_TAG(LOG_TAG_UART, "LCD version request failed after %d attempts", 
                       VERSION_REQUEST_MAX_RETRIES + 1);
          next_version_request_ms = 0;  // Stop trying
        }
      }
    }
    
    // Handle incoming UART messages
    if (lcdSerial.available() >= 5) {  // Minimum frame size
      uint8_t msg_type;
      uint16_t seq;
      uint8_t data[UART_PROTO_MAX_DATA_SIZE];
      size_t data_len = sizeof(data);
      
      if (uart_proto->recv_frame(&msg_type, &seq, data, &data_len, 100)) {
        LOG_DEBUG_TAG(LOG_TAG_UART, "Received frame: type=%d, seq=%d, data_len=%zu", 
                      msg_type, seq, data_len);
        
        // UART RX gating: before SYNC established, accept only SYNC/SYNC_ACK
        if (!sync_established && msg_type != MSG_SYNC && msg_type != MSG_SYNC_ACK) {
          LOG_DEBUG_TAG(LOG_TAG_UART, "Dropping message type %d (SYNC not established)", msg_type);
          // Drop message - don't process
        } else {
          // Process message
          switch (msg_type) {
            case MSG_SYNC:
              // LCD sent SYNC - respond with SYNC_ACK
              LOG_INFO_TAG(LOG_TAG_UART, "SYNC received from LCD, sending SYNC_ACK");
              uart_proto->send_sync_ack(0);
              
              // If SYNC arrives after sync_established==true, treat as LCD reboot / link reset
              if (sync_established) {
                LOG_WARN_TAG(LOG_TAG_UART, "Peer SYNC after established -> treating as LCD reboot");
                lcd_version_received = false;
                version_request_attempt = 0;
                next_version_request_ms = millis() + 250;
              }
              break;
              
            case MSG_SYNC_ACK:
              // LCD acknowledged our SYNC - handshake complete
              if (!sync_established) {
                sync_established = true;
                LOG_INFO_TAG(LOG_TAG_UART, "SYNC established with LCD");
                health_gate.markUartSyncEstablished();  // Notify HealthGate that SYNC is established
                // Schedule first version request after SYNC
                next_version_request_ms = millis() + 500;  // Wait 500ms after SYNC
              }
              break;
              
            case MSG_HEARTBEAT:
              LOG_DEBUG_TAG(LOG_TAG_UART, "Heartbeat received from LCD");
              break;
              
            case MSG_VERSION:
              // LCD sent version (response to our request)
              // Safety: Only null-terminate if we have space, otherwise treat as invalid
              if (data_len > 0 && data_len < UART_PROTO_MAX_DATA_SIZE) {
                data[data_len] = '\0';  // Ensure null termination (safe - we have space)
                LOG_INFO_TAG(LOG_TAG_UART, "LCD version: %s", (const char*)data);
                lcd_version_received = true;  // Stop requesting
                next_version_request_ms = 0;  // Cancel any pending retries
                version_request_attempt = 0;  // Reset attempt counter
                LOG_INFO_TAG(LOG_TAG_UART, "LCD version latched, retries stopped");
              } else if (data_len >= UART_PROTO_MAX_DATA_SIZE) {
                LOG_WARN_TAG(LOG_TAG_UART, "MSG_VERSION payload too large: %zu bytes (max %d), dropping", 
                             data_len, UART_PROTO_MAX_DATA_SIZE);
              }
              break;
              
            case MSG_STATUS:
              // LCD sent status
              // Format: [version_str:null-term][uptime_ms:4][reset_reason:1]
              // Safety: Use strnlen to find null terminator within bounds
              if (data_len >= 6) {  // Minimum: version(1) + null(1) + uptime(4) + reset(1) = 7, but allow empty version
                const char* version = (const char*)data;
                size_t version_len = strnlen(version, data_len);  // Safe: bounded search
                
                // Validate null terminator exists within data_len
                if (version_len >= data_len) {
                  LOG_WARN_TAG(LOG_TAG_UART, "MSG_STATUS missing null terminator in version string, dropping");
                  break;  // Drop frame safely
                }
                
                // Check: version_len + 1 (null) + 4 (uptime) + 1 (reset) = version_len + 6
                if (version_len + 6 <= data_len) {
                  uint32_t uptime_ms = data[version_len + 1] | 
                                       (data[version_len + 2] << 8) |
                                       (data[version_len + 3] << 16) |
                                       (data[version_len + 4] << 24);
                  uint8_t reset_reason = data[version_len + 5];
                  
                  // Throttle STATUS logging (at most once every 5 seconds)
                  unsigned long now_status = millis();
                  if ((now_status - last_status_log_ms) >= STATUS_LOG_THROTTLE_MS) {
                    LOG_INFO_TAG(LOG_TAG_UART, "LCD status: version=%s, uptime=%lu ms, reset_reason=%d",
                                 version, uptime_ms, reset_reason);
                    last_status_log_ms = now_status;
                  }
                } else {
                  LOG_WARN_TAG(LOG_TAG_UART, "Status message too short: %zu bytes (expected >= %zu), dropping", 
                               data_len, version_len + 6);
                }
              }
              break;
              
            case MSG_GET_VERSION:
              // LCD requested our version - send version with build_id suffix for identification
              LOG_INFO_TAG(LOG_TAG_UART, "LCD requested version, sending response");
              {
                // Format: "VERSION|BUILD_ID" (LCD can parse version part before |)
                char version_with_build[160];
                snprintf(version_with_build, sizeof(version_with_build), "%s|%s", FIRMWARE_VERSION, BUILD_ID);
                uart_proto->send_version(seq, version_with_build);
              }
              break;
              
            default:
              LOG_WARN_TAG(LOG_TAG_UART, "Unknown message type: %d", msg_type);
              break;
          }
        }
      }
    }
  }
  
  // Check if we're in PENDING_VERIFY state (booted into new OTA partition)
  // If so, prioritize stability + marking valid, don't attempt another OTA
  const esp_partition_t* running = esp_ota_get_running_partition();
  bool in_pending_verify = false;
  if (running) {
    esp_ota_img_states_t ota_state;
    esp_err_t ota_err = esp_ota_get_state_partition(running, &ota_state);
    if (ota_err == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      in_pending_verify = true;
      LOG_INFO("[OTA] Running partition is in PENDING_VERIFY state - prioritizing stability + mark valid");
    }
  }
  
  // Manifest Fetch and OTA Check Mode
  // Runs once per boot, 5 seconds after Wi-Fi connects (or after boot if already connected)
  // BUT: Skip OTA apply if we're in PENDING_VERIFY (prioritize marking valid)
  if (wifi_manager.isConnected() && !ota_check_done && (now - boot_time_ms) >= 5000) {
    ota_check_done = true;  // Only check once per boot
    last_ota_check_ms = now;
    
    // Build cache-busted manifest URL (with query parameter to avoid stale cache)
    // Format: <base_url>?v=<firmware_version>-boot<boot_count>
    char manifest_url[512];
    snprintf(manifest_url, sizeof(manifest_url), "%s?v=%s-boot%lu", 
             OTA_MANIFEST_URL_BASE, FIRMWARE_VERSION, g_boot_count);
    
    // Check if manifest URL is configured (not placeholder/empty)
    bool manifest_url_configured = (strlen(OTA_MANIFEST_URL_BASE) > 0 && 
                                     strcmp(OTA_MANIFEST_URL_BASE, "REPLACE_ME") != 0);
    
    if (!manifest_url_configured) {
      // Manifest URL not configured - mark as disabled and skip
      health_gate.markManifestDisabled();
      LOG_INFO("OTA manifest URL not configured; skipping");
    } else {
      // Manifest Fetch: fetch and parse manifest
      LOG_INFO("[MANIFEST] Fetching manifest from: %s", manifest_url);
      
      OtaManifest manifest;
      if (manifest_client.fetchManifest(manifest_url, manifest, 10000)) {
        // Manifest fetch and parse succeeded
        health_gate.markManifestFetched();
        
        // Log parsed manifest with detailed values (size, sha256, bin_url)
        char size_buf[32];
        if (manifest.size_specified) {
          snprintf(size_buf, sizeof(size_buf), "%lu", static_cast<unsigned long>(manifest.size));
        } else {
          snprintf(size_buf, sizeof(size_buf), "unknown");
        }

        if (manifest.min_version[0] != '\0') {
          LOG_INFO("[MANIFEST] Parsed: version=%s, size=%s, sha256=%s, min_version=%s, bin_url=%s",
                   manifest.version, size_buf, manifest.sha256, manifest.min_version, manifest.url);
        } else {
          LOG_INFO("[MANIFEST] Parsed: version=%s, size=%s, sha256=%s, bin_url=%s",
                   manifest.version, size_buf, manifest.sha256, manifest.url);
        }
        
        // Check min_version requirement
        if (manifest.min_version[0] != '\0') {
          int min_compare = ManifestClient::compareVersions(FIRMWARE_VERSION, manifest.min_version);
          if (min_compare < 0) {
            LOG_WARN("[MANIFEST] Current firmware below min_version; update required");
          }
        }
        
        // Artifact sanity check: verify build_id or artifact_fw_version matches expected
        bool artifact_verified = false;
        if (manifest.build_id_specified) {
          // Check if build_id matches expected (should start with version)
          if (strncmp(manifest.build_id, manifest.version, strlen(manifest.version)) == 0) {
            artifact_verified = true;
            LOG_INFO("[MANIFEST] Artifact verified: build_id=%s matches version=%s", 
                     manifest.build_id, manifest.version);
          } else {
            LOG_WARN("[MANIFEST] Artifact mismatch: build_id=%s does not match version=%s", 
                     manifest.build_id, manifest.version);
          }
        } else if (manifest.artifact_fw_version_specified) {
          // Check if artifact_fw_version matches manifest.version
          if (strcmp(manifest.artifact_fw_version, manifest.version) == 0) {
            artifact_verified = true;
            LOG_INFO("[MANIFEST] Artifact verified: artifact_fw_version=%s matches version=%s", 
                     manifest.artifact_fw_version, manifest.version);
          } else {
            LOG_WARN("[MANIFEST] Artifact mismatch: artifact_fw_version=%s does not match version=%s", 
                     manifest.artifact_fw_version, manifest.version);
          }
        } else {
          // No build identity provided
          if (ALLOW_UNVERIFIED_MANIFEST) {
            artifact_verified = true;
            LOG_WARN("[MANIFEST] No build_id/artifact_fw_version in manifest (ALLOW_UNVERIFIED_MANIFEST=true)");
          } else {
            LOG_ERROR("[MANIFEST] Artifact verification FAILED: manifest lacks build_id/artifact_fw_version (ALLOW_UNVERIFIED_MANIFEST=false)");
          }
        }
        
        // Check if update is needed using semantic version comparison
        if (manifest_client.isVersionCurrent(manifest, FIRMWARE_VERSION)) {
          LOG_INFO("[MANIFEST] Firmware is up to date (version: %s)", manifest.version);
        } else if (manifest_client.isVersionNewer(manifest, FIRMWARE_VERSION)) {
          // Newer version available
          LOG_INFO("[MANIFEST] Update available: %s -> %s", FIRMWARE_VERSION, manifest.version);
          LOG_INFO("[MANIFEST] Versions: manifest.version=%s, running_version=%s", 
                   manifest.version, FIRMWARE_VERSION);
          
          // Guard: Don't attempt OTA if artifact verification failed
          if (!artifact_verified) {
            LOG_ERROR("[OTA] Refusing OTA apply: artifact verification failed");
          }
          // Guard: Don't attempt OTA if we're in PENDING_VERIFY (prioritize marking valid)
          else if (in_pending_verify) {
            LOG_INFO("[OTA] Skipping OTA apply (in PENDING_VERIFY - prioritize marking valid)");
          }
          // Guard: Don't attempt OTA if an expectation is still pending
          else if (OtaExpect::isPending()) {
            LOG_INFO("[OTA] Skipping OTA apply: OTA expectation pending from previous update");
          }
          // Guard: After a successful validation this boot, skip OTA for a short window to avoid loops
          else if (g_ota_validated_this_boot && (now - g_ota_validated_time_ms) < 30000) {
            LOG_INFO("[OTA] Skipping OTA apply: just validated OTA this boot (cooldown window)");
          }
          // Guard: Prevent OTA loops - check if we already applied this version recently
          else {
            char last_version[32] = "";
            int64_t last_timestamp = BootState::getLastOtaApplyTimestamp();
            bool has_last_version = BootState::getLastAppliedVersion(last_version, sizeof(last_version));
            bool cooldown_active = false;
            
            if (has_last_version && strcmp(last_version, manifest.version) == 0 && last_timestamp > 0) {
              // Get current timestamp (milliseconds from esp_timer, which resets on boot)
              int64_t current_timestamp = esp_timer_get_time() / 1000;  // Convert microseconds to milliseconds
              
              // Handle timestamp comparison: esp_timer_get_time() resets on boot, so if current < last,
              // it means we've rebooted since the last apply. In that case, treat as cooldown expired.
              int64_t elapsed_ms = 0;
              if (current_timestamp < last_timestamp) {
                // Timestamp wrapped/reset (reboot occurred) - treat as cooldown expired
                elapsed_ms = (int64_t)OTA_APPLY_COOLDOWN_MS + 1;  // Force cooldown expired
                LOG_INFO("[OTA] Timestamp reset detected (reboot occurred) - cooldown check passed");
              } else {
                elapsed_ms = current_timestamp - last_timestamp;
              }
              
              if (elapsed_ms < (int64_t)OTA_APPLY_COOLDOWN_MS) {
                LOG_INFO("[OTA] Skipping OTA apply: version %s was applied %lld ms ago (cooldown: %lu ms)", 
                         manifest.version, elapsed_ms, (unsigned long)OTA_APPLY_COOLDOWN_MS);
                cooldown_active = true;
              }
            }
            
            // Only proceed with OTA if cooldown is not active
            if (!cooldown_active) {
              if (OTA_DRY_RUN && !ota_apply_done) {
                // DRY RUN: Download and verify (no OTA write/apply)
                ota_apply_done = true;
                LOG_INFO("[OTA] Running DRY RUN verification (OTA_DRY_RUN=true)");
                
                SenseDownloadVerifier::Result verify_result = download_verifier.verifyDownload(
                    manifest.url, manifest.sha256, manifest.size_specified ? manifest.size : 0, 60000);
                
                if (verify_result == SenseDownloadVerifier::RESULT_SUCCESS) {
                  LOG_INFO("[DRY_RUN] download+sha PASS (no OTA apply performed)");
                  LOG_INFO("[DRY_RUN] Downloaded: %lu bytes, SHA256=%s", 
                           download_verifier.getDownloadedSize(), download_verifier.getComputedSha256());
                } else {
                  LOG_ERROR("[DRY_RUN] FAIL reason=%s", SenseDownloadVerifier::getResultString(verify_result));
                }
              } else if (OTA_APPLY_AND_REBOOT && !ota_apply_done && artifact_verified) {
                // REAL OTA: Write to OTA partition, set boot partition, and reboot
                ota_apply_done = true;
                
                // Record this apply attempt in NVS to prevent loops
                // Use esp_timer_get_time() for wallclock-ish monotonic timestamp (valid across resets)
                int64_t apply_timestamp = esp_timer_get_time() / 1000;  // Convert microseconds to milliseconds
                BootState::setLastOtaApply(manifest.version, apply_timestamp);
                
                // Loud OTA guard logs before apply
                LOG_WARN("[OTA][GUARD] Current running FIRMWARE_VERSION=%s BUILD_ID=%s", FIRMWARE_VERSION, BUILD_ID);
                LOG_WARN("[OTA][GUARD] Expecting to install manifest.version=%s from URL=%s", manifest.version, manifest.url);
                LOG_WARN("[OTA][GUARD] Release verification tip: `strings -a <bin> | grep HALO_FW_MARKER:%s`", manifest.version);
                
                LOG_INFO("[OTA] OTA_APPLY_AND_REBOOT enabled - starting OTA apply...");
                LOG_INFO("[OTA] Target version: %s (will record in NVS to prevent loops)", manifest.version);
                
                uint32_t expected_size = manifest.size_specified ? manifest.size : 0;
                SenseOtaApplier::Result apply_result = ota_applier.applyToOtaPartition(
                    manifest.url, manifest.sha256, expected_size, 600000, true, manifest.version);  // hard_deadline_ms=600000 (10 minutes), reboot=true
                
                if (apply_result == SenseOtaApplier::RESULT_SUCCESS) {
                  LOG_INFO("[OTA] Apply successful - device will reboot into new partition");
                  // Function should have already rebooted, but if it returns, log error
                  LOG_ERROR("[OTA] ERROR: applyToOtaApplier returned SUCCESS but did not reboot!");
                } else {
                  LOG_ERROR("[OTA] Apply FAILED: %s", SenseOtaApplier::getResultString(apply_result));
                  // Clear the NVS flag on failure so we can retry
                  BootState::clearLastOtaApply();
                }
              }
            }
          }
        } else {
          // Manifest version is older or invalid - log warning
          LOG_WARN("[MANIFEST] Manifest version %s is not newer than current %s", 
                   manifest.version, FIRMWARE_VERSION);
        }
      } else {
        // Manifest fetch or parse failed
        LOG_ERROR("[MANIFEST] FAILED: Could not fetch or parse manifest");
        // Do NOT mark manifest as fetched - HealthGate will not cancel rollback
      }
    }
  }
  
  delay(10);  // Small delay to prevent tight loop
}

// Optional: Close NVS on shutdown (though this is rarely called)
void shutdown() {
  BootState::end();
}

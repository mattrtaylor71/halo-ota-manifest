#include "SenseOtaApplier.h"
#include "Log.h"
#include "BuildFlags.h"
#ifndef HALO_BOARD_LCD
#include "OtaExpect.h"
#endif
#include "WifiGuard.h"  // WiFi guard to prevent "STA not started" errors
#include <WiFi.h>
#include <esp_ota_ops.h>

extern "C" __attribute__((weak)) bool halo_wifi_hard_reset_for_ota(const char* reason, uint32_t timeout_ms) {
  (void)reason;
  (void)timeout_ms;
  return false;
}
#include <esp_system.h>

#define LOG_TAG_OTA "OTA"
#define LOG_TAG_OTA_WRITE "OTA_WRITE"

#ifdef HALO_BOARD_LCD
extern "C" void lcd_ota_on_success(const char* version) __attribute__((weak));
#endif

namespace {
static const char kAmazonRootCa1[] PROGMEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
    "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
    "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
    "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
    "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
    "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
    "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
    "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
    "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
    "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
    "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
    "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
    "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
    "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
    "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
    "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
    "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
    "rqXRfboQnoZsG4q5WTP468SQvvG5\n"
    "-----END CERTIFICATE-----\n";

static inline void ota_cooperative_yield(unsigned long& last_yield_ms, unsigned long interval_ms = 20) {
  unsigned long now_ms = millis();
  if ((now_ms - last_yield_ms) < interval_ms) {
    return;
  }
  last_yield_ms = now_ms;
  delay(1);
  yield();
}

static inline size_t ota_begin_size_for_board(uint32_t expected_size) {
#ifdef HALO_BOARD_LCD
  // Avoid a long upfront partition erase on LCD, which can starve the task
  // watchdog before the streamed OTA loop begins.
  (void)expected_size;
  return OTA_WITH_SEQUENTIAL_WRITES;
#else
  return (expected_size > 0) ? expected_size : OTA_SIZE_UNKNOWN;
#endif
}
}  // namespace

SenseOtaApplier::SenseOtaApplier() {
  mbedtls_sha256_init(&sha256_ctx);
#if OTA_TLS_INSECURE_DEBUG
  client.setInsecure();
#else
  client.setCACert(kAmazonRootCa1);
#endif
}

SenseOtaApplier::~SenseOtaApplier() {
  mbedtls_sha256_free(&sha256_ctx);
  client.stop();
}

const char* SenseOtaApplier::getResultString(Result result) {
  switch (result) {
    case RESULT_SUCCESS: return "SUCCESS";
    case RESULT_FAILED_DOWNLOAD: return "FAILED_DOWNLOAD";
    case RESULT_FAILED_SHA256_MISMATCH: return "FAILED_SHA256_MISMATCH";
    case RESULT_FAILED_WRITE: return "FAILED_WRITE";
    case RESULT_FAILED_VERIFY: return "FAILED_VERIFY";
    case RESULT_FAILED_INVALID_MANIFEST: return "FAILED_INVALID_MANIFEST";
    case RESULT_PARTITION_TOO_SMALL: return "PARTITION_TOO_SMALL";
    case RESULT_FAILED_TIMEOUT: return "HARD_DEADLINE_TIMEOUT";
    case RESULT_FAILED_NO_PROGRESS: return "NO_PROGRESS_TIMEOUT";
    case RESULT_FAILED_INCOMPLETE: return "INCOMPLETE";
    case RESULT_FAILED_SIZE_MISMATCH: return "SIZE_MISMATCH";
    case RESULT_NO_STREAM: return "NO_STREAM";
    case RESULT_HTTP_BEGIN_FAIL: return "HTTP_BEGIN_FAIL";
    case RESULT_HTTP_GET_FAIL: return "HTTP_GET_FAIL";
    case RESULT_STALL_RETRY_EXHAUSTED: return "STALL_RETRY_EXHAUSTED";
    case RESULT_SET_BOOT_FAIL: return "SET_BOOT_FAIL";
    case RESULT_FAILED_MARKER_NOT_FOUND: return "MARKER_NOT_FOUND";
    case RESULT_FAILED_MARKER_MISMATCH: return "MARKER_VERSION_MISMATCH";
    default: return "UNKNOWN";
  }
}

bool SenseOtaApplier::parseUrl(const char* url, String& host, int& port, String& path) {
  if (!url || strlen(url) == 0) {
    return false;
  }
  
  String url_str = url;
  
  if (!url_str.startsWith("https://")) {
    LOG_ERROR_TAG(LOG_TAG_OTA, "URL must be HTTPS");
    return false;
  }
  
  url_str = url_str.substring(8);  // Remove "https://"
  
  int path_start = url_str.indexOf('/');
  if (path_start < 0) {
    host = url_str;
    path = "/";
  } else {
    host = url_str.substring(0, path_start);
    path = url_str.substring(path_start);
  }
  
  port = 443;  // Default HTTPS port
  int port_start = host.indexOf(':');
  if (port_start >= 0) {
    port = host.substring(port_start + 1).toInt();
    host = host.substring(0, port_start);
  }
  
  return true;
}

void SenseOtaApplier::sha256ToHex(const uint8_t* hash, char* hex_str) {
  const char hex_chars[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    hex_str[i * 2] = hex_chars[(hash[i] >> 4) & 0x0F];
    hex_str[i * 2 + 1] = hex_chars[hash[i] & 0x0F];
  }
  hex_str[64] = '\0';
}

bool SenseOtaApplier::compareSha256(const char* computed, const char* expected) {
  if (!computed || !expected) {
    return false;
  }
  
  // Case-insensitive comparison
  for (int i = 0; i < 64; i++) {
    char c1 = computed[i];
    char c2 = expected[i];
    
    // Convert to lowercase for comparison
    if (c1 >= 'A' && c1 <= 'F') c1 = c1 - 'A' + 'a';
    if (c2 >= 'A' && c2 <= 'F') c2 = c2 - 'A' + 'a';
    
    if (c1 != c2) {
      return false;
    }
  }
  
  return true;
}

SenseOtaApplier::Result SenseOtaApplier::applyToOtaPartition(
    const char* url, const char* expected_sha256_hex, 
    uint32_t expected_size, uint32_t hard_deadline_ms, 
    bool set_boot_and_reboot,
    const char* expected_version) {
#ifdef HALO_BOARD_LCD
#define LCD_OTA_RETURN_FAIL(r) do { Serial.printf("[LCD_OTA] applier FAIL reason=%s\n", SenseOtaApplier::getResultString(r)); return (r); } while(0)
#else
#define LCD_OTA_RETURN_FAIL(r) return (r)
#endif

  // Validate parameters
  if (!url || strlen(url) == 0 || !expected_sha256_hex || strlen(expected_sha256_hex) != 64) {
    LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Invalid parameters");
    LCD_OTA_RETURN_FAIL(RESULT_FAILED_INVALID_MANIFEST);
  }
  
  // Host is validated against allowlist by caller before apply. No URL parsing here.

#ifdef HALO_BOARD_LCD
  Serial.printf("[LCD_OTA] applier begin url=%s expected_size=%u\n", url ? url : "(null)", (unsigned)expected_size);
#endif

  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Starting OTA partition write from: %s", url);
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Expected SHA256: %s", expected_sha256_hex);
  if (expected_size > 0) {
    LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Expected size: %lu bytes", expected_size);
  }
  uint32_t free_heap_start = ESP.getFreeHeap();
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Free heap at start: %lu bytes", free_heap_start);

  // Safety: ensure we have a reasonable amount of free heap before starting OTA
  // to avoid crashes during TLS/HTTP/OTA operations.
  const uint32_t MIN_HEAP_REQUIRED = 40 * 1024;  // 40 KB
  const uint32_t OTA_DOWNLOAD_MIN_HEAP = 8 * 1024;  // 8 KB: abort during download if heap drops below
  if (free_heap_start < MIN_HEAP_REQUIRED) {
    LOG_ERROR_TAG(LOG_TAG_OTA_WRITE,
                  "Insufficient heap for OTA: free_heap=%lu, required>=%lu. Aborting.",
                  free_heap_start, MIN_HEAP_REQUIRED);
    LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
  }
  
  // Choose target partition
  const esp_partition_t* update = esp_ota_get_next_update_partition(NULL);
  if (!update) {
    LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "No OTA update partition found");
    LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
  }
  
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Next partition: label=%s, addr=0x%x, size=%lu", 
               update->label, update->address, update->size);
  
  // Check if partition is large enough
  if (expected_size > 0 && expected_size > update->size) {
    LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Partition too small: need %lu, have %lu", 
                  expected_size, update->size);
    LCD_OTA_RETURN_FAIL(RESULT_PARTITION_TOO_SMALL);
  }
  
  // Marker check is done in-stream: one HTTPS connection for full artifact, read first
  // 64KB, verify HALO_FW_MARKER, then stream rest to OTA (avoids second connection
  // that often gets no body on ESP32 TLS).
  
  // Begin OTA update
  esp_ota_handle_t ota_handle;
  size_t ota_size = ota_begin_size_for_board(expected_size);
  esp_err_t err = esp_ota_begin(update, ota_size, &ota_handle);
  
  if (err != ESP_OK) {
    LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_begin FAIL: %s", esp_err_to_name(err));
    LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
  }
  
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "esp_ota_begin OK");
  
  // Initialize SHA256
  mbedtls_sha256_starts(&sha256_ctx, 0);  // 0 = SHA256 (not SHA224)
  
  // Retry configuration (allowlist-only: caller validates host; S3 supports Range)
  const int RESUME_MAX_ATTEMPTS = 5;
  int RESTART_MAX_ATTEMPTS = 3;
  const uint32_t BACKOFF_DELAYS_MS[RESUME_MAX_ATTEMPTS] = {200, 400, 800, 1600, 3000};
  
  // Restart backoff delays (exponential: 250ms, 500ms, 1s, 2s, 4s)
  const uint32_t RESTART_BACKOFF_DELAYS_MS[5] = {250, 500, 1000, 2000, 4000};
  
  // Initial GET retries when connection refused / no stream on first attempt
  const int INITIAL_GET_MAX_RETRIES = 5;
  
  // Progress-based timeout configuration (will be adjusted by server profile)
  uint32_t NO_PROGRESS_TIMEOUT_MS = 15000;  // 15s: react fast to stalls, preserve retry budget
  uint32_t STALL_GRACE_MS = 12000;  // First-bytes probe grace (12s) – still tolerant of weak Wi‑Fi
  
  // Wait for WiFi reconnect before HTTP retries (avoids connection refused when WiFi dropped)
  auto waitForWifiReconnect = [](uint32_t timeout_ms) -> bool {
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeout_ms) {
      delay(100);
      yield();
    }
    return (WiFi.status() == WL_CONNECTED);
  };
  
  // Backoff jitter (0–500 ms) to avoid thundering herd on retries
  auto backoffWithJitter = [](uint32_t base_ms) -> uint32_t {
    return base_ms + (esp_random() % 501);
  };
  
  // HTTP context struct (defined outside lambda for reuse)
  struct HttpCtx {
    WiFiClientSecure client;
    HTTPClient http;
    
    HttpCtx() {
      client.setInsecure();  // TODO: Phase 2 - add proper certificate validation
      // Note: client.setTimeout() should be set AFTER connect() succeeds
      // Do NOT set timeout here - will be set after http.begin() succeeds
    }
    
    ~HttpCtx() {
      http.end();
      client.stop();
    }
  };
  
  // Range support: S3 allows Range; caller only passes allowlisted URLs
  bool resume_supported = true;
  int range_http_code = 0;
  
  // Helper: Begin HTTP request with fresh context and get stream pointer
  auto httpBeginAndGet = [&](uint32_t offset, int attempt_num, bool using_range,
                             HttpCtx*& ctx_out,
                             int& httpCode, int& contentLen) -> WiFiClient* {
    // Force clean HTTP teardown before ANY new attempt
    if (ctx_out) {
      ctx_out->http.end();
      ctx_out->client.stop();
      delete ctx_out;
      ctx_out = nullptr;
      delay(50);  // Brief delay to ensure connection fully closes
    }
    
    // Create fresh context (fresh WiFiClientSecure instance)
    ctx_out = new HttpCtx();
    if (!ctx_out) {
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Failed to create fresh HTTP context (offset=%lu, attempt=%d)",
                    (unsigned long)offset, attempt_num);
      return nullptr;
    }
    
    if (!ctx_out->http.begin(ctx_out->client, url)) {
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "HTTP begin failed (offset=%lu, attempt=%d)",
                    (unsigned long)offset, attempt_num);
      delete ctx_out;
      ctx_out = nullptr;
      return nullptr;
    }
    
    // Set connect timeout only before GET (safe); read timeout after GET to avoid errno=9
    ctx_out->http.setConnectTimeout(15000);
    ctx_out->http.setReuse(false);  // Disable keep-alive reuse - force fresh connection per attempt
    // Note: http.setTimeout() and client.setTimeout() must be set AFTER GET() succeeds (connection established)
    
    // Add headers for better compatibility
    ctx_out->http.addHeader("Connection", "close");  // Force connection close
    ctx_out->http.addHeader("Accept-Encoding", "identity");  // Disable compression for predictable download
    
    // Optional: Use HTTP/1.0 for simpler connection handling (if available)
    // ctx_out->http.useHTTP10(true);  // Uncomment if HTTPClient supports this
    
    if (using_range && offset > 0) {
      String range_header = String("bytes=") + String(offset) + "-";
      ctx_out->http.addHeader("Range", range_header);
    }
    
    httpCode = ctx_out->http.GET();
    
    // NULL-SAFE: If HTTP failed (negative or invalid code), do not call getStreamPtr() or header() - clean up and return nullptr.
    bool valid_code = (offset == 0) ? (httpCode == HTTP_CODE_OK) : (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_PARTIAL_CONTENT);
    if (httpCode < 0 || !valid_code) {
      if (httpCode < 0) {
        String error_str = ctx_out->http.errorToString(httpCode);
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "HTTP GET returned %d (TRANSIENT): %s (offset=%lu, attempt=%d, using_range=%d)",
                     httpCode, error_str.c_str(), (unsigned long)offset, attempt_num, using_range ? 1 : 0);
      } else {
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "HTTP GET returned httpCode=%d (expected 200 or 206 for range), offset=%lu, attempt=%d",
                     httpCode, (unsigned long)offset, attempt_num);
      }
      ctx_out->http.end();
      ctx_out->client.stop();
      delete ctx_out;
      ctx_out = nullptr;
      contentLen = 0;
      return nullptr;
    }
    
    // NOW safe to set HTTP and client timeouts - connection is established. Prevents errno=9 (Bad file number)
    if (ctx_out->client.connected()) {
      ctx_out->http.setTimeout(45000);  // 45s read timeout - only after GET so socket exists
      ctx_out->client.setTimeout(2000);  // 2s timeout for blocking reads - allows readBytes() to block briefly
    }
    
    contentLen = ctx_out->http.getSize();
    bool connected = ctx_out->http.connected();
    WiFiClient* stream = ctx_out->http.getStreamPtr();
    
    // Log Content-Range header if available (only when we have valid response)
    String content_range = ctx_out->http.header("Content-Range");
    
    // Enhanced logging for each HTTP attempt (include resume_supported state)
    if (content_range.length() > 0) {
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE,
                   "HTTP attempt %d: use_range=%d, requested_offset=%lu, httpCode=%d, connected=%d, contentLength=%d, Content-Range=%s, stream_ptr=%p, resume_supported=%d",
                   attempt_num, using_range ? 1 : 0, (unsigned long)offset, httpCode, connected ? 1 : 0, contentLen, content_range.c_str(), stream, resume_supported ? 1 : 0);
    } else {
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE,
                   "HTTP attempt %d: use_range=%d, requested_offset=%lu, httpCode=%d, connected=%d, contentLength=%d, stream_ptr=%p, resume_supported=%d",
                   attempt_num, using_range ? 1 : 0, (unsigned long)offset, httpCode, connected ? 1 : 0, contentLen, stream, resume_supported ? 1 : 0);
    }
    
    if (!stream) {
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "getStreamPtr() returned null (httpCode=%d, connected=%d, attempt=%d)",
                    httpCode, connected ? 1 : 0, attempt_num);
      delete ctx_out;
      ctx_out = nullptr;
      return nullptr;
    }
    
    // Validate stream exists and connection is active
    if (!connected) {
      LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "HTTP connected() returned false immediately after GET (attempt=%d)", attempt_num);
      delete ctx_out;
      ctx_out = nullptr;
      return nullptr;
    }
    
    // Validate HTTP response code
    if (offset == 0) {
      // Initial request: expect 200 OK
      if (httpCode != HTTP_CODE_OK) {
        LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Initial HTTP GET failed: expected 200, got %d (attempt=%d)",
                      httpCode, attempt_num);
        delete ctx_out;
        ctx_out = nullptr;
        return nullptr;
      }
    } else if (using_range) {
      // Resume request: ONLY trust HTTP 206 Partial Content
      // HTTP 200 means Range was ignored - will be detected in main loop
      if (httpCode != HTTP_CODE_PARTIAL_CONTENT && httpCode != HTTP_CODE_OK) {
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Resume request returned %d (expected 206 or 200): server may not support Range (attempt=%d)",
                     httpCode, attempt_num);
        delete ctx_out;
        ctx_out = nullptr;
        return nullptr;
      }
      // When 206: validate Content-Range start matches requested offset (S3 format: "bytes start-end/total")
      if (httpCode == HTTP_CODE_PARTIAL_CONTENT && offset > 0 && content_range.length() > 0) {
        const char* p = content_range.c_str();
        if (strncmp(p, "bytes ", 6) == 0) {
          p += 6;
          unsigned long range_start = 0;
          while (*p >= '0' && *p <= '9') {
            range_start = range_start * 10 + (unsigned long)(*p - '0');
            p++;
          }
          if ((unsigned long)offset != range_start) {
            LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Content-Range start mismatch: requested_offset=%lu, Content-Range start=%lu, header=%s (attempt=%d)",
                         (unsigned long)offset, range_start, content_range.c_str(), attempt_num);
            delete ctx_out;
            ctx_out = nullptr;
            return nullptr;
          }
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Content-Range validated: start=%lu matches requested offset (attempt=%d)", range_start, attempt_num);
        } else {
          LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Content-Range missing or invalid format (expected 'bytes start-end/total'), got: %s (attempt=%d)",
                       content_range.c_str(), attempt_num);
          delete ctx_out;
          ctx_out = nullptr;
          return nullptr;
        }
      }
      // Note: If httpCode == 200 with Range requested, we'll detect this in main loop
      // and handle it by disabling resume and restarting from 0
      // Only HTTP 206 confirms resume support
    }
    
    return stream;
  };
  
  // Determine target bytes (before download loop)
  uint32_t target_bytes = 0;
  int initial_contentLength = 0;
  
  // Stream download state
  uint32_t downloaded_bytes = 0;
  const uint32_t WRITE_BUFFER_SIZE = 4096;
  static uint8_t write_buffer[WRITE_BUFFER_SIZE];
  
  // Hard deadline and stall reconnect (not adjusted by server profile)
  const uint32_t HARD_DEADLINE_TIMEOUT_MS = hard_deadline_ms;
  const uint32_t STALL_RECONNECT_MS = 8000;
  
  unsigned long start_ms = millis();
  unsigned long last_progress_ms = start_ms;
  unsigned long last_progress_log_ms = start_ms;
  uint32_t last_progress_log_bytes = 0;
  
  // Retry state
  int resume_attempt = 0;
  int restart_attempt = 0;
  int total_http_attempts = 0;  // Global counter for all HTTP attempts (for logging)
  int initial_get_fail_count = 0;  // Consecutive initial (non-range) GET failures; retry up to INITIAL_GET_MAX_RETRIES
  int consecutive_transient_failures = 0;  // Consecutive null stream / httpCode<0; triggers FULL_HTTP_RESET when >=2
  unsigned long last_hard_reset_ms = 0;
  const unsigned long HARD_RESET_COOLDOWN_MS = 15000;
  
  // Same-offset stall detection
  uint32_t stall_offset = 0;
  uint8_t stall_same_offset_count = 0;
  
  // First-bytes probe: allow multiple probe windows before declaring NO_STREAM (weak Wi‑Fi)
  const int FIRST_BYTES_PROBE_MAX_WINDOWS = 3;  // Allow 2–3 consecutive grace windows before fail
  int first_bytes_probe_fail_count = 0;
  
  // Current HTTP context (raw pointer, manually managed)
  HttpCtx* current_ctx = nullptr;
  
  // Extract hostname from URL for logging (do this once before the loop)
  String hostname_str = "unknown";
  if (url) {
    String host, path;
    int port;
    if (parseUrl(url, host, port, path)) {
      hostname_str = host;
    }
  }
  
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE,
               "Starting OTA download (buf=%lu, no_progress_timeout=%lu ms, hard_deadline=%lu ms, stall_grace=%lu ms, stall_reconnect=%lu ms, resume_max=%d, restart_max=%d, resume_supported=%d [sticky], server_profile=%s)",
              (unsigned long)WRITE_BUFFER_SIZE, (unsigned long)NO_PROGRESS_TIMEOUT_MS, (unsigned long)HARD_DEADLINE_TIMEOUT_MS,
              (unsigned long)STALL_GRACE_MS, (unsigned long)STALL_RECONNECT_MS, RESUME_MAX_ATTEMPTS, RESTART_MAX_ATTEMPTS,
              resume_supported ? 1 : 0, "default");
  
  // Main download loop with retry/restart capability
  while (true) {
    // If we need to restart from scratch (restart_attempt > 0), abort current OTA and begin again
    if (restart_attempt > 0 && downloaded_bytes == 0) {
      LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Restarting OTA from scratch (attempt %d/%d)", restart_attempt, RESTART_MAX_ATTEMPTS);
      
      // Clean teardown: close HTTP client and stream so we don't retain a dead socket
      bool http_closed = false;
      bool client_stopped = false;
      if (current_ctx) {
        current_ctx->http.end();
        current_ctx->client.stop();
        delete current_ctx;
        current_ctx = nullptr;
        http_closed = true;
        client_stopped = true;
      }
      
      // Abort current OTA handle if it exists
      bool ota_aborted = false;
      if (ota_handle) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
        ota_aborted = true;
      }
      
      // Reset SHA256 context
      mbedtls_sha256_starts(&sha256_ctx, 0);
      
      // Begin fresh OTA (next httpBeginAndGet will recreate HTTPClient/WiFiClient – objects_recreated=1)
      size_t ota_size = ota_begin_size_for_board(expected_size);
      err = esp_ota_begin(update, ota_size, &ota_handle);
      if (err != ESP_OK) {
        LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_begin FAIL on restart: %s", esp_err_to_name(err));
        LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
      }
      
      // Reset all state for fresh restart
      downloaded_bytes = 0;
      resume_attempt = 0;
      start_ms = millis();  // Reset timers for restart
      last_progress_ms = start_ms;
      stall_offset = 0;
      stall_same_offset_count = 0;
      
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Restart cleanup complete: http_closed=%d ota_aborted=%d client_stopped=%d objects_recreated=1",
                   http_closed ? 1 : 0, ota_aborted ? 1 : 0, client_stopped ? 1 : 0);
      
      // CRITICAL: resume_supported is NOT reset on restart - once detected as unsupported, stay disabled for entire OTA session
      // This ensures sticky state: if server doesn't support Range, we never try Range again
    }
    
    // Before any retry, wait for WiFi (avoids connection refused when WiFi dropped mid-download)
    if (total_http_attempts >= 1) {
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Waiting for WiFi before retry (timeout=%lu ms)", (unsigned long)STALL_RECONNECT_MS);
      if (!waitForWifiReconnect(STALL_RECONNECT_MS)) {
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "WiFi reconnect timeout - attempting HTTP anyway");
      }
    }
    
    // Start or resume HTTP connection
    int httpCode = 0;
    int contentLen = 0;
    // Only use Range if we have progress AND server supports it
    bool using_range = (downloaded_bytes > 0 && resume_supported);
    total_http_attempts++;  // Increment global counter
    int attempt_num = total_http_attempts;
    
    if (using_range && downloaded_bytes > 0) {
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "RESUME: offset=%lu, attempt %d/%d, expecting 206",
                   (unsigned long)downloaded_bytes, resume_attempt + 1, RESUME_MAX_ATTEMPTS);
    }
    
    WiFiClient* stream = httpBeginAndGet(downloaded_bytes, attempt_num, using_range, current_ctx, httpCode, contentLen);
    
    // Log resume support status early (after first HTTP attempt)
    if (attempt_num == 1) {
      range_http_code = httpCode;
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "resume_supported=%d (range_http=%d) host=%s", resume_supported ? 1 : 0, range_http_code, hostname_str.c_str());
    }
    
    // NULL-SAFE: Handle failed GET / null stream BEFORE any use of current_ctx.
    // When httpBeginAndGet returns nullptr (e.g. httpCode=-1 connection refused), current_ctx is already cleaned up.
    if (!stream) {
      consecutive_transient_failures++;
      if (consecutive_transient_failures >= 2) {
        unsigned long now_ms = millis();
        if (now_ms - last_hard_reset_ms >= HARD_RESET_COOLDOWN_MS) {
          LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "WiFi hard reset for OTA (httpCode=%d)", httpCode);
          if (halo_wifi_hard_reset_for_ota("ota_http_retry", 8000)) {
            LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "WiFi hard reset succeeded");
          } else {
            LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "WiFi hard reset did not recover");
          }
          last_hard_reset_ms = now_ms;
        }
      }
      if (using_range) {
        // Resume failed - retry with exponential backoff
        if (resume_attempt < RESUME_MAX_ATTEMPTS - 1) {
          resume_attempt++;
          uint32_t backoff_ms = backoffWithJitter(BACKOFF_DELAYS_MS[resume_attempt - 1]);
          LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Resume attempt failed (httpCode=%d) - retrying with backoff %lu ms (resume_attempt=%d/%d, restart_attempt=%d/%d, offset=%lu)",
                       httpCode, (unsigned long)backoff_ms, resume_attempt + 1, RESUME_MAX_ATTEMPTS, restart_attempt, RESTART_MAX_ATTEMPTS, downloaded_bytes);
          if (consecutive_transient_failures >= 2) {
            LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "FULL_HTTP_RESET: >=2 consecutive transient failures, extra delay 500ms before retry");
            delay(500);
          }
          delay(backoff_ms);
          continue;  // Retry resume
        } else {
          // Resume exhausted - try full restart
          LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Resume attempts exhausted (%d/%d) - attempting full restart (restart_attempt=%d/%d)",
                       resume_attempt + 1, RESUME_MAX_ATTEMPTS, restart_attempt, RESTART_MAX_ATTEMPTS);
          if (restart_attempt < RESTART_MAX_ATTEMPTS) {
            restart_attempt++;
            resume_attempt = 0;
            downloaded_bytes = 0;  // Trigger restart on next iteration
            uint32_t restart_backoff = backoffWithJitter(RESTART_BACKOFF_DELAYS_MS[restart_attempt - 1]);
            delay(restart_backoff);
            continue;
          } else {
            LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "All recovery attempts exhausted - failing (resume_attempt=%d/%d, restart_attempt=%d/%d)",
                         resume_attempt + 1, RESUME_MAX_ATTEMPTS, restart_attempt, RESTART_MAX_ATTEMPTS);
            if (ota_handle) esp_ota_abort(ota_handle);
            if (current_ctx) {
              current_ctx->http.end();
              current_ctx->client.stop();
              delete current_ctx;
              current_ctx = nullptr;
            }
            LCD_OTA_RETURN_FAIL(RESULT_STALL_RETRY_EXHAUSTED);
          }
        }
      } else {
        // Initial connection failed (no range, downloaded_bytes==0): retry with WiFi wait and backoff
        if (initial_get_fail_count < INITIAL_GET_MAX_RETRIES) {
          initial_get_fail_count++;
          LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Initial HTTP connection failed (httpCode=%d) - retrying with WiFi wait and backoff (attempt %d/%d)",
                       httpCode, initial_get_fail_count, INITIAL_GET_MAX_RETRIES);
          if (!waitForWifiReconnect(STALL_RECONNECT_MS)) {
            LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "WiFi reconnect timeout before initial retry");
          }
          if (consecutive_transient_failures >= 2) {
            LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "FULL_HTTP_RESET: >=2 consecutive transient failures, extra delay 500ms before retry");
            delay(500);
          }
          uint32_t backoff_ms = backoffWithJitter(BACKOFF_DELAYS_MS[initial_get_fail_count - 1]);
          delay(backoff_ms);
          continue;  // Retry from outer loop (will wait for WiFi again at loop top)
        } else {
          LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Initial HTTP connection failed after %d retries (httpCode=%d) - aborting",
                        INITIAL_GET_MAX_RETRIES, httpCode);
          if (ota_handle) esp_ota_abort(ota_handle);
          if (current_ctx) {
            current_ctx->http.end();
            current_ctx->client.stop();
            delete current_ctx;
            current_ctx = nullptr;
          }
          LCD_OTA_RETURN_FAIL(RESULT_HTTP_GET_FAIL);
        }
      }
    }
    
    // Successfully got stream - reset consecutive transient failure counter
    consecutive_transient_failures = 0;
    
    // Range capability detection: only when stream != nullptr (current_ctx is valid). Do NOT dereference current_ctx when stream was null.
    if (using_range && downloaded_bytes > 0 && resume_supported) {
      range_http_code = httpCode;  // Track for logging
      bool server_ignored_range = false;
      String content_range_header = current_ctx->http.header("Content-Range");
      
      // Detect if server ignored Range header:
      // ONLY trust HTTP 206 Partial Content. HTTP 200 means Range was ignored.
      if (httpCode == HTTP_CODE_OK) {
        // Server returned 200 OK instead of 206 - Range was ignored
        server_ignored_range = true;
      } else if (httpCode == HTTP_CODE_PARTIAL_CONTENT) {
        // Got 206 - verify Content-Range header is present
        if (content_range_header.length() == 0) {
          // Missing Content-Range header - suspicious, but 206 is still valid
          LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Range request returned 206 but Content-Range header missing (continuing)");
        }
        // HTTP 206 with Range requested = resume supported (even if Content-Range missing)
        // Don't set server_ignored_range = true here
      } else {
        // Unexpected HTTP code - treat as Range not supported
        server_ignored_range = true;
      }
      
      if (server_ignored_range) {
        // Server does not support Range - disable resume for entire OTA session (sticky state)
        resume_supported = false;
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Host ignored Range; disabling resume and using restart-from-zero strategy (http=%d, offset=%lu)",
                     httpCode, (unsigned long)downloaded_bytes);
        
        // Increase no_progress_timeout for no-resume mode (longer patience needed)
        if (NO_PROGRESS_TIMEOUT_MS < 90000) {
          NO_PROGRESS_TIMEOUT_MS = 90000;  // 90 seconds for no-resume mode
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Increased no_progress_timeout to %lu ms for no-resume mode", (unsigned long)NO_PROGRESS_TIMEOUT_MS);
        }
        
        // Clean up current HTTP connection
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        
        // Abort current OTA handle to prevent duplicate writes
        if (ota_handle) {
          esp_ota_abort(ota_handle);
          ota_handle = 0;
        }
        
        // Reset SHA256
        mbedtls_sha256_starts(&sha256_ctx, 0);
        
        // Reset state for restart from 0
        downloaded_bytes = 0;
        resume_attempt = 0;
        stall_offset = 0;
        stall_same_offset_count = 0;
        start_ms = millis();  // Reset timers
        last_progress_ms = start_ms;
        
        // Begin fresh OTA
        size_t ota_size = ota_begin_size_for_board(expected_size);
        err = esp_ota_begin(update, ota_size, &ota_handle);
        if (err != ESP_OK) {
          LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_begin FAIL after Range detection: %s", esp_err_to_name(err));
          LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
        }
        
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Restarting from 0 with resume disabled (server does not support Range)");
        continue;  // Restart from beginning of loop with offset=0 and resume_supported=false
      }
    }
    
    // Successfully got stream - reset initial-fail counter for any future restart
    initial_get_fail_count = 0;
    
    // Successfully got stream - validate response
    if (downloaded_bytes == 0) {
      // Initial connection - validate size
      initial_contentLength = contentLen;
      
      if (expected_size > 0 && contentLen > 0) {
        if ((uint32_t)contentLen != expected_size) {
          LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Size mismatch: expected_size(manifest)=%lu, content_length=%d",
                        expected_size, contentLen);
          if (current_ctx) {
            delete current_ctx;
            current_ctx = nullptr;
          }
          if (ota_handle) esp_ota_abort(ota_handle);
          LCD_OTA_RETURN_FAIL(RESULT_FAILED_SIZE_MISMATCH);
        }
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Size validation: manifest=%lu matches Content-Length=%d",
                     expected_size, contentLen);
      }
      
      // Determine target bytes
      if (expected_size > 0) {
        target_bytes = expected_size;
      } else if (contentLen > 0) {
        target_bytes = contentLen;
      } else {
        target_bytes = 0;
      }
      
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Target bytes: %lu (expected_size=%lu, contentLength=%d)",
                   target_bytes, expected_size, contentLen);
    } else {
      // Resume attempt - validate we got 206 Partial Content
      // Note: Range detection already happened above, so if we reach here and using_range is true,
      // the server supports Range (resume_supported is still true)
      if (httpCode != HTTP_CODE_PARTIAL_CONTENT) {
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Resume returned %d (expected 206) - treating as retryable", httpCode);
        if (current_ctx) {
          delete current_ctx;
          current_ctx = nullptr;
        }
        resume_attempt++;
        if (resume_attempt < RESUME_MAX_ATTEMPTS) {
          uint32_t backoff_ms = backoffWithJitter(BACKOFF_DELAYS_MS[resume_attempt - 1]);
          delay(backoff_ms);
          continue;
        } else {
          // Try restart
          if (restart_attempt < RESTART_MAX_ATTEMPTS) {
            restart_attempt++;
            resume_attempt = 0;
            downloaded_bytes = 0;
            uint32_t restart_backoff = backoffWithJitter(restart_attempt <= 2 ? RESTART_BACKOFF_DELAYS_MS[restart_attempt - 1] : 1000);
            delay(restart_backoff);
            continue;
          } else {
            if (ota_handle) esp_ota_abort(ota_handle);
            LCD_OTA_RETURN_FAIL(RESULT_STALL_RETRY_EXHAUSTED);
          }
        }
      }
      
      // Resume successful - reset resume attempt counter and stall tracking
      resume_attempt = 0;
      stall_same_offset_count = 0;  // Reset on successful resume
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Resume successful: continuing from offset=%lu", downloaded_bytes);
    }

    // CRITICAL: Get fresh stream pointer from current_ctx (must be done while http is active)
    // Do NOT keep a stale pointer - get it fresh each time we need it
    stream = current_ctx->http.getStreamPtr();
    if (!stream) {
      // Stream pointer null - treat as transient and retry with Range resume
      LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Stream pointer is null after successful GET (downloaded=%lu, resume_attempt=%d/%d, restart_attempt=%d/%d) - treating as transient stall",
                    downloaded_bytes, resume_attempt, RESUME_MAX_ATTEMPTS, restart_attempt, RESTART_MAX_ATTEMPTS);
      if (current_ctx) {
        current_ctx->http.end();
        current_ctx->client.stop();
        delete current_ctx;
        current_ctx = nullptr;
      }
      // Check if we can retry (only if resume is supported)
      if (resume_supported && using_range && resume_attempt < RESUME_MAX_ATTEMPTS - 1) {
        resume_attempt++;
        uint32_t backoff_ms = backoffWithJitter(BACKOFF_DELAYS_MS[resume_attempt - 1]);
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Retrying resume with backoff %lu ms (resume_attempt=%d/%d)",
                     (unsigned long)backoff_ms, resume_attempt + 1, RESUME_MAX_ATTEMPTS);
        delay(backoff_ms);
        continue;  // Retry from outer loop
      } else if (restart_attempt < RESTART_MAX_ATTEMPTS) {
        restart_attempt++;
        resume_attempt = 0;
        
        // Clean teardown before restart
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        
        if (ota_handle) {
          esp_ota_abort(ota_handle);
          ota_handle = 0;
        }
        
        mbedtls_sha256_starts(&sha256_ctx, 0);
        downloaded_bytes = 0;
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Attempting full restart (restart_attempt=%d/%d)",
                     restart_attempt, RESTART_MAX_ATTEMPTS);
        uint32_t restart_backoff = backoffWithJitter(restart_attempt <= 2 ? RESTART_BACKOFF_DELAYS_MS[restart_attempt - 1] : 1000);
        delay(restart_backoff);
        continue;  // Retry from outer loop (will call esp_ota_begin in outer loop)
      } else {
        // All retries exhausted - only now return NO_STREAM
        LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Stream pointer null and all retries exhausted - failing with NO_STREAM");
        if (ota_handle) esp_ota_abort(ota_handle);
        LCD_OTA_RETURN_FAIL(RESULT_NO_STREAM);
      }
    }

    // First bytes: on initial connection (downloaded_bytes==0) read first 64KB, verify
    // marker in-stream, then write to OTA. On resume, wait for next chunk (256 bytes).
    const uint32_t MARKER_CHECK_SIZE = 64 * 1024;
    static uint8_t marker_check_buffer[MARKER_CHECK_SIZE];
    uint8_t probe_buf[256];
    uint32_t probe_size = (target_bytes > 0 && target_bytes - downloaded_bytes < 256) ? 
                           (target_bytes - downloaded_bytes) : 256;
    unsigned long probe_start = millis();
    uint32_t probe_start_downloaded = downloaded_bytes;
    int probe_n = 0;
    bool probe_success = false;
    unsigned long probe_elapsed = 0;
    bool probe_disconnected = false;
    unsigned long last_probe_log_ms = probe_start;
    
    LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Probe window start: offset=%lu grace_ms=%lu probe_fail_count=%d/%d",
                 (unsigned long)downloaded_bytes, (unsigned long)STALL_GRACE_MS, first_bytes_probe_fail_count, FIRST_BYTES_PROBE_MAX_WINDOWS);
    
    if (downloaded_bytes == 0) {
      // Single-connection path: read first 64KB from this stream, verify marker, write to OTA
      size_t marker_read = 0;
      const uint32_t to_read_max = (target_bytes > 0 && target_bytes < MARKER_CHECK_SIZE) ? target_bytes : MARKER_CHECK_SIZE;
      unsigned long last_probe_yield_ms = millis();
      
      while (marker_read < to_read_max) {
        probe_elapsed = millis() - probe_start;
        if (!current_ctx->http.connected()) {
          probe_disconnected = true;
          break;
        }
        size_t chunk = (to_read_max - marker_read) < sizeof(probe_buf) ? (to_read_max - marker_read) : sizeof(probe_buf);
        int n = stream->read(probe_buf, chunk);
        if (n > 0) {
          memcpy(marker_check_buffer + marker_read, probe_buf, n);
          marker_read += n;
          last_probe_log_ms = millis();
          ota_cooperative_yield(last_probe_yield_ms);
          if (marker_read % 8192 == 0 || marker_read == to_read_max) {
            LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "First bytes probe: read %zu / %lu bytes (elapsed=%lu ms)",
                         marker_read, (unsigned long)to_read_max, millis() - probe_start);
          }
        } else {
          if (probe_elapsed >= STALL_GRACE_MS) {
            if (marker_read > 0) {
              LOG_INFO_TAG(LOG_TAG_OTA_WRITE,
                           "First bytes probe: grace elapsed after partial progress (%zu bytes) - ending probe and verifying buffered bytes",
                           marker_read);
            } else {
              LOG_INFO_TAG(LOG_TAG_OTA_WRITE,
                           "First bytes probe: elapsed=%lu ms >= STALL_GRACE_MS with no bytes - treating as stall",
                           probe_elapsed);
            }
            break;
          }
          delay(25);
          yield();
        }
      }
      
      if (marker_read > 0) {
        // Verify marker if expected_version provided
        if (expected_version && strlen(expected_version) > 0) {
          const char* marker_prefix = "HALO_FW_MARKER:";
          const size_t marker_prefix_len = strlen(marker_prefix);
          const char* buffer_end = (const char*)(marker_check_buffer + marker_read);
          bool valid_marker_found = false;
          char found_version[32] = {0};
          
          for (size_t i = 0; i + marker_prefix_len <= marker_read; i++) {
            if (memcmp(marker_check_buffer + i, marker_prefix, marker_prefix_len) != 0) continue;
            const char* version_start = (const char*)(marker_check_buffer + i + marker_prefix_len);
            if (version_start >= buffer_end) continue;
            char first_char = *version_start;
            if (first_char < '0' || first_char > '9') continue;
            
            size_t version_idx = 0;
            for (const char* p = version_start; p < buffer_end && version_idx < sizeof(found_version) - 1; p++) {
              char c = *p;
              if (c == '|' || c == '\0' || c == '\r' || c == '\n' || c == ' ' || c == '\t') break;
              if ((c >= '0' && c <= '9') || c == '.') found_version[version_idx++] = c;
              else break;
            }
            found_version[version_idx] = '\0';
            if (version_idx > 0) {
              valid_marker_found = true;
              break;
            }
          }
          
          if (!valid_marker_found) {
            LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Marker check: No valid HALO_FW_MARKER in first %zu bytes", marker_read);
            if (current_ctx) { current_ctx->http.end(); current_ctx->client.stop(); delete current_ctx; current_ctx = nullptr; }
            esp_ota_abort(ota_handle);
            LCD_OTA_RETURN_FAIL(RESULT_FAILED_MARKER_NOT_FOUND);
          }
          if (strcmp(found_version, expected_version) != 0) {
            LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Marker version mismatch: manifest=%s bin_marker=%s", expected_version, found_version);
            if (current_ctx) { current_ctx->http.end(); current_ctx->client.stop(); delete current_ctx; current_ctx = nullptr; }
            esp_ota_abort(ota_handle);
            LCD_OTA_RETURN_FAIL(RESULT_FAILED_MARKER_MISMATCH);
          }
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Marker check: version match confirmed (%s)", found_version);
        }
        
        mbedtls_sha256_update(&sha256_ctx, marker_check_buffer, marker_read);
        err = esp_ota_write(ota_handle, marker_check_buffer, marker_read);
        if (err != ESP_OK) {
          LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_write FAIL on first chunk: %s", esp_err_to_name(err));
          if (current_ctx) { current_ctx->http.end(); current_ctx->client.stop(); delete current_ctx; current_ctx = nullptr; }
          esp_ota_abort(ota_handle);
          LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
        }
        downloaded_bytes = marker_read;
        last_progress_ms = millis();
        ota_cooperative_yield(last_probe_yield_ms, 0);
        probe_success = true;
        first_bytes_probe_fail_count = 0;
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Probe window end: bytes_read=%zu reason=success (marker verified)", marker_read);
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "First bytes probe SUCCESS: read %zu bytes (marker verified), downloaded now=%lu",
                     marker_read, (unsigned long)downloaded_bytes);
      }
    } else {
      // Resume path: wait for next chunk (same as previous first-bytes probe)
      while (true) {
        probe_elapsed = millis() - probe_start;
        bool probe_connected = current_ctx->http.connected();
        if (!probe_connected) {
          probe_disconnected = true;
          break;
        }
        size_t probe_available = stream->available();
        probe_n = stream->read(probe_buf, probe_size);
        unsigned long now_probe_log = millis();
        if (probe_n != 0 || (now_probe_log - last_probe_log_ms >= 500)) {
          last_probe_log_ms = now_probe_log;
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE,
                       "First bytes probe: waiting_for_first_bytes_ms=%lu, n=%d, connected=%d, available=%zu, downloaded=%lu (start=%lu), resume_attempt=%d/%d, restart_attempt=%d/%d",
                       probe_elapsed, probe_n, probe_connected ? 1 : 0, probe_available, downloaded_bytes, probe_start_downloaded,
                       resume_attempt, RESUME_MAX_ATTEMPTS, restart_attempt, RESTART_MAX_ATTEMPTS);
        }
        if (probe_n > 0) {
          probe_success = true;
          first_bytes_probe_fail_count = 0;
          mbedtls_sha256_update(&sha256_ctx, probe_buf, probe_n);
          err = esp_ota_write(ota_handle, probe_buf, probe_n);
          if (err != ESP_OK) {
            LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_write FAIL on probe bytes: %s", esp_err_to_name(err));
            if (current_ctx) { current_ctx->http.end(); current_ctx->client.stop(); delete current_ctx; current_ctx = nullptr; }
            esp_ota_abort(ota_handle);
            LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
          }
          downloaded_bytes += probe_n;
          last_progress_ms = millis();
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Probe window end: bytes_read=%d reason=success (resume)", probe_n);
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "First bytes probe SUCCESS: read %d bytes after %lu ms, downloaded now=%lu",
                       probe_n, probe_elapsed, (unsigned long)downloaded_bytes);
          break;
        }
        if (probe_elapsed >= STALL_GRACE_MS && probe_n <= 0) {
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "First bytes probe: elapsed=%lu ms >= STALL_GRACE_MS=%lu ms with no bytes - treating as stall",
                       probe_elapsed, (unsigned long)STALL_GRACE_MS);
          break;
        }
        if (probe_elapsed >= NO_PROGRESS_TIMEOUT_MS && downloaded_bytes == probe_start_downloaded) {
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "First bytes probe: no-progress timeout");
          break;
        }
        delay(25);
        yield();
      }
    }
    
    if (!probe_success) {
      first_bytes_probe_fail_count++;
      bool still_connected = !probe_disconnected && current_ctx && current_ctx->http.connected();
      unsigned long final_elapsed = millis() - probe_start;
      bool had_progress = (downloaded_bytes > probe_start_downloaded);
      
      // Determine failure reason for logging
      const char* failure_reason = "unknown";
      if (probe_disconnected) {
        failure_reason = "disconnected";
      } else if (final_elapsed >= NO_PROGRESS_TIMEOUT_MS && !had_progress) {
        failure_reason = "no-progress-timeout";
      } else if (final_elapsed >= STALL_GRACE_MS) {
        failure_reason = "stall-grace-timeout";
      }
      
      LOG_WARN_TAG(LOG_TAG_OTA_WRITE,
                   "Probe window end: bytes_read=0 reason=%s probe_fail_count=%d/%d (do not treat as immediate fatal)",
                   failure_reason, first_bytes_probe_fail_count, FIRST_BYTES_PROBE_MAX_WINDOWS);
      LOG_WARN_TAG(LOG_TAG_OTA_WRITE,
                   "First bytes probe: no bytes after %lu ms, connected=%d, downloaded_start=%lu, downloaded_now=%lu, had_progress=%d, resume_attempt=%d/%d, restart_attempt=%d/%d",
                   final_elapsed, still_connected ? 1 : 0, probe_start_downloaded, downloaded_bytes, had_progress ? 1 : 0,
                   resume_attempt, RESUME_MAX_ATTEMPTS, restart_attempt, RESTART_MAX_ATTEMPTS);
      
      // Only fail NO_STREAM after multiple probe windows exhausted (weak Wi‑Fi tolerance)
      bool probe_windows_exhausted = (first_bytes_probe_fail_count >= FIRST_BYTES_PROBE_MAX_WINDOWS);
      if (probe_windows_exhausted && (probe_disconnected || (resume_attempt >= RESUME_MAX_ATTEMPTS - 1 && restart_attempt >= RESTART_MAX_ATTEMPTS))) {
        LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "First bytes probe: %d probe windows with no bytes and retries exhausted - failing with NO_STREAM (reason=%s)",
                      first_bytes_probe_fail_count, failure_reason);
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        if (ota_handle) esp_ota_abort(ota_handle);
        LCD_OTA_RETURN_FAIL(RESULT_NO_STREAM);
      }
      
      // Treat as transient stall: tear down and retry (resume or restart). Do not full restart until probe windows exhausted.
      LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Why retry: first_bytes no progress (probe_fail_count=%d/%d) - resume_supported=%d",
                   first_bytes_probe_fail_count, FIRST_BYTES_PROBE_MAX_WINDOWS, resume_supported ? 1 : 0);
      if (current_ctx) {
        current_ctx->http.end();
        current_ctx->client.stop();
        delete current_ctx;
        current_ctx = nullptr;
      }
      
      if (resume_supported && using_range && resume_attempt < RESUME_MAX_ATTEMPTS - 1) {
        resume_attempt++;
        uint32_t backoff_ms = backoffWithJitter(BACKOFF_DELAYS_MS[resume_attempt - 1]);
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Retrying resume with backoff %lu ms (resume_attempt=%d/%d, offset=%lu)",
                     (unsigned long)backoff_ms, resume_attempt + 1, RESUME_MAX_ATTEMPTS, downloaded_bytes);
        delay(backoff_ms);
        continue;  // Retry from outer loop
      } else if (restart_attempt < RESTART_MAX_ATTEMPTS) {
        restart_attempt++;
        resume_attempt = 0;
        
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        
        if (ota_handle) {
          esp_ota_abort(ota_handle);
          ota_handle = 0;
        }
        
        mbedtls_sha256_starts(&sha256_ctx, 0);
        downloaded_bytes = 0;
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Why restart: first_bytes no progress - full restart (restart_attempt=%d/%d) http_closed=1 ota_aborted=1 client_stopped=1 objects_recreated=1",
                     restart_attempt, RESTART_MAX_ATTEMPTS);
        uint32_t restart_backoff = backoffWithJitter(restart_attempt <= 2 ? RESTART_BACKOFF_DELAYS_MS[restart_attempt - 1] : 1000);
        delay(restart_backoff);
        continue;  // Retry from outer loop (will call esp_ota_begin in outer loop)
      } else {
        LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "First bytes probe failed and all retries exhausted - failing with NO_STREAM (probe_fail_count=%d)",
                      first_bytes_probe_fail_count);
        if (ota_handle) esp_ota_abort(ota_handle);
        LCD_OTA_RETURN_FAIL(RESULT_NO_STREAM);
      }
    }

    // Download loop with progress-based timeout and stall detection
    // CRITICAL: current_ctx must remain alive for the entire duration of this loop
    // Do NOT call http.end() or client.stop() until we break out of this loop
    unsigned long last_download_yield_ms = millis();
    while (true) {
      unsigned long now_ms = millis();
      unsigned long elapsed_since_start = now_ms - start_ms;
      unsigned long elapsed_since_progress = now_ms - last_progress_ms;
      
      // Get fresh stream pointer (in case it changed)
      stream = current_ctx->http.getStreamPtr();
      
      // Hard deadline check: absolute maximum time exceeded
      if (elapsed_since_start > HARD_DEADLINE_TIMEOUT_MS) {
        LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, 
                      "Hard deadline timeout: downloaded=%lu, target=%lu, elapsed=%lu ms (deadline=%lu ms), last_progress_ago=%lu ms, resume_attempt=%d, restart_attempt=%d",
                      downloaded_bytes, target_bytes, elapsed_since_start, (unsigned long)HARD_DEADLINE_TIMEOUT_MS,
                      elapsed_since_progress, resume_attempt, restart_attempt);
        // Teardown HTTP connection before aborting
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        if (ota_handle) esp_ota_abort(ota_handle);
        LCD_OTA_RETURN_FAIL(RESULT_FAILED_TIMEOUT);
      }

      // Heap watchdog: abort gracefully if free heap drops critically low
      {
        static unsigned long last_heap_check_ms = 0;
        if (now_ms - last_heap_check_ms >= 2000) {
          last_heap_check_ms = now_ms;
          uint32_t current_heap = ESP.getFreeHeap();
          if (current_heap < OTA_DOWNLOAD_MIN_HEAP) {
            LOG_ERROR_TAG(LOG_TAG_OTA_WRITE,
                          "Heap watchdog: free_heap=%lu < min=%lu - aborting OTA",
                          (unsigned long)current_heap, (unsigned long)OTA_DOWNLOAD_MIN_HEAP);
            if (current_ctx) {
              current_ctx->http.end();
              current_ctx->client.stop();
              delete current_ctx;
              current_ctx = nullptr;
            }
            if (ota_handle) esp_ota_abort(ota_handle);
            LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
          }
        }
      }

      // No-progress timeout check: no bytes read/written for too long
      if (elapsed_since_progress > NO_PROGRESS_TIMEOUT_MS) {
        // Same-offset stall detection
        if (downloaded_bytes == stall_offset) {
          stall_same_offset_count++;
        } else {
          stall_offset = downloaded_bytes;
          stall_same_offset_count = 0;
        }
        
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE,
                     "No-progress timeout: downloaded=%lu, target=%lu, elapsed_since_start=%lu ms, no_progress_for=%lu ms (threshold=%lu ms), same_offset_count=%u - triggering resume/reconnect",
                     downloaded_bytes, target_bytes, elapsed_since_start, elapsed_since_progress, (unsigned long)NO_PROGRESS_TIMEOUT_MS, stall_same_offset_count);
        
        // If stuck at same offset for 3+ resumes, force full restart from 0 (no Range)
        if (stall_same_offset_count >= 3) {
          LOG_WARN_TAG(LOG_TAG_OTA_WRITE, 
                       "Stuck at same offset=%lu for %u resumes -> restarting download from 0 (no Range)",
                       (unsigned long)stall_offset, stall_same_offset_count);
          
          // Force clean teardown
          if (current_ctx) {
            current_ctx->http.end();
            current_ctx->client.stop();
            delete current_ctx;
            current_ctx = nullptr;
          }
          
          // Abort OTA write session cleanly
          if (ota_handle) {
            esp_ota_abort(ota_handle);
            ota_handle = 0;
          }
          
          // Reset SHA256
          mbedtls_sha256_starts(&sha256_ctx, 0);
          
          // Begin fresh OTA
          size_t ota_size = ota_begin_size_for_board(expected_size);
          err = esp_ota_begin(update, ota_size, &ota_handle);
          if (err != ESP_OK) {
            LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_begin FAIL on same-offset restart: %s", esp_err_to_name(err));
            LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
          }
          
          // Reset state for full restart
          uint32_t previous_offset = downloaded_bytes;
          downloaded_bytes = 0;
          resume_attempt = 0;
          restart_attempt++;
          stall_offset = 0;
          stall_same_offset_count = 0;
          start_ms = millis();
          last_progress_ms = start_ms;
          
          // Log cleanup confirmation
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Restart cleanup complete: http_closed=1 ota_aborted=1 client_stopped=1 objects_recreated=1");
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, 
                       "Full restart from 0: previous_offset=%lu, restart_count=%d, ota_begin result=OK",
                       previous_offset, restart_attempt);
          
          break;  // Exit inner loop, will restart from 0 in outer loop
        }
        
        // Normal resume/reconnect logic
        // Teardown only when starting a NEW attempt
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        // Recovery rules based on resume_supported state
        if (resume_supported && downloaded_bytes > 0) {
          // Resume supported and we have progress - attempt Range resume from current offset
          resume_attempt++;
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "No-progress timeout: triggering Range resume (resume_attempt=%d/%d, restart_attempt=%d/%d, offset=%lu, resume_supported=1)",
                       resume_attempt, RESUME_MAX_ATTEMPTS, restart_attempt, RESTART_MAX_ATTEMPTS, downloaded_bytes);
          if (resume_attempt < RESUME_MAX_ATTEMPTS) {
            uint32_t backoff_ms = backoffWithJitter(BACKOFF_DELAYS_MS[resume_attempt - 1]);
            delay(backoff_ms);
            break;  // Exit inner loop, retry resume in outer loop
          } else {
            // Resume attempts exhausted - try restart
            if (restart_attempt < RESTART_MAX_ATTEMPTS) {
              restart_attempt++;
              resume_attempt = 0;
              downloaded_bytes = 0;
              LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Resume attempts exhausted, attempting full restart (restart_attempt=%d/%d)",
                           restart_attempt, RESTART_MAX_ATTEMPTS);
              uint32_t restart_backoff = backoffWithJitter(restart_attempt <= 2 ? RESTART_BACKOFF_DELAYS_MS[restart_attempt - 1] : 1000);
              delay(restart_backoff);
              break;  // Exit inner loop, trigger restart in outer loop
            } else {
              LOG_ERROR_TAG(LOG_TAG_OTA_WRITE,
                            "No-progress timeout: all recovery attempts exhausted (downloaded=%lu, target=%lu, elapsed=%lu ms, no_progress_for=%lu ms, resume_attempt=%d/%d, restart_attempt=%d/%d)",
                            downloaded_bytes, target_bytes, elapsed_since_start, elapsed_since_progress,
                            resume_attempt, RESUME_MAX_ATTEMPTS, restart_attempt, RESTART_MAX_ATTEMPTS);
              if (ota_handle) esp_ota_abort(ota_handle);
              LCD_OTA_RETURN_FAIL(RESULT_FAILED_NO_PROGRESS);
            }
          }
        } else {
          // Resume not supported OR no progress yet - force full restart from 0
          // Close HTTP, abort OTA handle, begin fresh
          if (restart_attempt < RESTART_MAX_ATTEMPTS) {
            restart_attempt++;
            resume_attempt = 0;
            
            // Clean teardown before restart
            bool http_closed = false;
            if (current_ctx) {
              current_ctx->http.end();
              current_ctx->client.stop();
              delete current_ctx;
              current_ctx = nullptr;
              http_closed = true;
            }
            
            // Abort current OTA handle
            bool ota_aborted = false;
            if (ota_handle) {
              esp_ota_abort(ota_handle);
              ota_handle = 0;
              ota_aborted = true;
            }
            
            // Reset SHA256
            mbedtls_sha256_starts(&sha256_ctx, 0);
            bool sha_reset = true;
            
            // Reset all state
            downloaded_bytes = 0;
            stall_offset = 0;
            stall_same_offset_count = 0;
            
            // Begin fresh OTA immediately (don't rely on outer loop)
            size_t ota_size = ota_begin_size_for_board(expected_size);
            err = esp_ota_begin(update, ota_size, &ota_handle);
            if (err != ESP_OK) {
              LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_begin FAIL on no-progress restart: %s", esp_err_to_name(err));
              LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
            }
            
            // Reset timers
            start_ms = millis();
            last_progress_ms = start_ms;
            
            // Log cleanup confirmation
            LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Restart cleanup complete: http_closed=%d ota_aborted=%d client_stopped=%d objects_recreated=1",
                         http_closed ? 1 : 0, ota_aborted ? 1 : 0, http_closed ? 1 : 0);
            LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "No-progress timeout: resume_supported=%d, forcing full restart from 0 (restart_attempt=%d/%d, no_progress_timeout=%lu ms)",
                         resume_supported ? 1 : 0, restart_attempt, RESTART_MAX_ATTEMPTS, (unsigned long)NO_PROGRESS_TIMEOUT_MS);
            uint32_t restart_backoff = backoffWithJitter(restart_attempt <= 2 ? RESTART_BACKOFF_DELAYS_MS[restart_attempt - 1] : 1000);
            delay(restart_backoff);
            break;  // Exit inner loop, will restart from 0 in outer loop
          } else {
            LOG_ERROR_TAG(LOG_TAG_OTA_WRITE,
                          "No-progress timeout: all recovery attempts exhausted (downloaded=%lu, target=%lu, elapsed=%lu ms, no_progress_for=%lu ms, resume_supported=%d, restart_attempt=%d/%d)",
                          downloaded_bytes, target_bytes, elapsed_since_start, elapsed_since_progress,
                          resume_supported ? 1 : 0, restart_attempt, RESTART_MAX_ATTEMPTS);
            if (ota_handle) esp_ota_abort(ota_handle);
            LCD_OTA_RETURN_FAIL(RESULT_FAILED_NO_PROGRESS);
          }
        }
      }
      
      // Check if we've reached target bytes
      if (target_bytes > 0 && downloaded_bytes >= target_bytes) {
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Reached target bytes: %lu", target_bytes);
        break;  // Exit download loop, proceed to verification
      }
      
      // Validate stream pointer (get fresh pointer from current_ctx)
      if (!stream) {
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Stream became null during download (downloaded=%lu, resume_supported=%d) - triggering recovery",
                     downloaded_bytes, resume_supported ? 1 : 0);
        // Teardown only when starting a NEW attempt
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        // Only attempt resume if server supports Range
        if (resume_supported && using_range && resume_attempt < RESUME_MAX_ATTEMPTS) {
          resume_attempt++;
          uint32_t backoff_ms = backoffWithJitter(BACKOFF_DELAYS_MS[resume_attempt - 1]);
          delay(backoff_ms);
          break;  // Exit inner loop, retry resume in outer loop
        } else {
          // Try restart (resume not supported or attempts exhausted)
          if (restart_attempt < RESTART_MAX_ATTEMPTS) {
            restart_attempt++;
            resume_attempt = 0;
            downloaded_bytes = 0;
            delay(backoffWithJitter(1000));
            break;  // Exit inner loop, trigger restart in outer loop
          } else {
            if (ota_handle) esp_ota_abort(ota_handle);
            LCD_OTA_RETURN_FAIL(RESULT_STALL_RETRY_EXHAUSTED);
          }
        }
      }
      
      // Check connection status (while http is still active)
      bool connected = current_ctx->http.connected();
      if (!connected) {
        // Connection closed - trigger recovery (resume if supported, otherwise restart)
        LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "Connection closed (downloaded=%lu, resume_supported=%d) - triggering recovery",
                     downloaded_bytes, resume_supported ? 1 : 0);
        // Teardown only when starting a NEW attempt
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        // Only attempt resume if server supports Range
        if (resume_supported && using_range && resume_attempt < RESUME_MAX_ATTEMPTS) {
          resume_attempt++;
          uint32_t backoff_ms = backoffWithJitter(BACKOFF_DELAYS_MS[resume_attempt - 1]);
          delay(backoff_ms);
          break;  // Exit inner loop, retry resume
        } else {
          // Try restart (resume not supported or attempts exhausted)
          if (restart_attempt < RESTART_MAX_ATTEMPTS) {
            restart_attempt++;
            resume_attempt = 0;
            downloaded_bytes = 0;
            delay(backoffWithJitter(1000));
            break;  // Exit inner loop, trigger restart
          } else {
            if (ota_handle) esp_ota_abort(ota_handle);
            LCD_OTA_RETURN_FAIL(RESULT_STALL_RETRY_EXHAUSTED);
          }
        }
      }
      
      // Determine how much to read
      size_t to_read = WRITE_BUFFER_SIZE;
      if (target_bytes > 0 && downloaded_bytes < target_bytes) {
        uint32_t remaining = target_bytes - downloaded_bytes;
        if (to_read > remaining) {
          to_read = remaining;
        }
      }
      
      // Refactored read pattern: use readBytes() with timeout instead of relying on available()
      // client.setTimeout(2000) is set in httpBeginAndGet, so readBytes() will block briefly (up to 2s)
      // This avoids dead time when available() returns 0 but data is still arriving
      size_t bytes_read = stream->readBytes(write_buffer, to_read);
      
      // Periodic stall logging (every ~5 seconds during stalls)
      static unsigned long last_stall_log_ms = 0;
      if (bytes_read == 0) {
        // Check if we should log stall status
        unsigned long since_last_stall_log = (last_stall_log_ms == 0) ? 6000 : (now_ms - last_stall_log_ms);
        if (since_last_stall_log >= 5000) {
          bool connected = current_ctx->http.connected();
          int rssi = WiFi.RSSI();
          uint32_t free_heap = ESP.getFreeHeap();
          LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Stall status: connected=%d, downloaded=%lu, WiFi.RSSI=%d, free_heap=%lu, no_progress_for=%lu ms",
                       connected ? 1 : 0, downloaded_bytes, rssi, free_heap, elapsed_since_progress);
          last_stall_log_ms = now_ms;
        }
        
        // No bytes read - yield to WiFi stack and check timeout
        delay(1);
        yield();  // Service WiFi background tasks
        
        // Re-check elapsed time since last progress
        now_ms = millis();
        elapsed_since_progress = now_ms - last_progress_ms;
        
        // If no progress for too long, trigger recovery (handled by no-progress timeout check at top of loop)
        // Continue to next iteration to let that logic handle it
        continue;
      } else {
        // Reset stall log timer on successful read
        last_stall_log_ms = 0;  // Reset so next stall logs immediately
      }
      
      // bytes_read > 0 - process the data
      // Update SHA256
      mbedtls_sha256_update(&sha256_ctx, write_buffer, bytes_read);
      
      // Write to OTA partition
      err = esp_ota_write(ota_handle, write_buffer, bytes_read);
      if (err != ESP_OK) {
        LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_write FAIL: %s", esp_err_to_name(err));
        // Teardown only when aborting
        if (current_ctx) {
          current_ctx->http.end();
          current_ctx->client.stop();
          delete current_ctx;
          current_ctx = nullptr;
        }
        if (ota_handle) esp_ota_abort(ota_handle);
        LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
      }
      
      // Update progress counters (only after successful write)
      downloaded_bytes += bytes_read;
      last_progress_ms = millis();  // Reset no-progress timer on any forward progress
      ota_cooperative_yield(last_download_yield_ms);
      
      // Reset stall tracking on successful progress
      if (downloaded_bytes > stall_offset) {
        stall_offset = downloaded_bytes;
        stall_same_offset_count = 0;
      }
      
      // Debug log: confirm progress-based logic is working
      LOG_DEBUG_TAG(LOG_TAG_OTA_WRITE, "Progress: read=%d bytes, downloaded=%lu, last_progress_ms updated (elapsed_since_start=%lu ms)",
                    bytes_read, downloaded_bytes, (millis() - start_ms));
      
      // Log progress
      unsigned long now_log = millis();
      bool should_log = false;
      if ((now_log - last_progress_log_ms) >= 2000) {
        should_log = true;
        last_progress_log_ms = now_log;
      } else if ((downloaded_bytes - last_progress_log_bytes) >= (32 * 1024)) {
        should_log = true;
      }
      
      if (should_log) {
        uint32_t remaining = (target_bytes > downloaded_bytes) ? (target_bytes - downloaded_bytes) : 0;
        LOG_INFO_TAG(LOG_TAG_OTA_WRITE,
                     "Progress: downloaded=%lu, remaining=%lu, resume_attempt=%d, restart_attempt=%d, free_heap=%lu",
                     downloaded_bytes, remaining, resume_attempt, restart_attempt, ESP.getFreeHeap());
        last_progress_log_bytes = downloaded_bytes;
      }
    }
    
    // If we exited the inner loop due to completion, break outer loop
    if (target_bytes > 0 && downloaded_bytes >= target_bytes) {
      // Download complete - teardown HTTP connection before proceeding to verification
      if (current_ctx) {
        current_ctx->http.end();
        current_ctx->client.stop();
        delete current_ctx;
        current_ctx = nullptr;
      }
      break;
    }
    
    // Otherwise, we're retrying (resume or restart) - teardown and continue outer loop
    // Teardown happens here because we're about to start a NEW HTTP attempt
    if (current_ctx) {
      current_ctx->http.end();
      current_ctx->client.stop();
      delete current_ctx;
      current_ctx = nullptr;
    }
  }
  
  // Final cleanup (should be null if we exited normally, but check anyway)
  if (current_ctx) {
    current_ctx->http.end();
    current_ctx->client.stop();
    delete current_ctx;
    current_ctx = nullptr;
  }
  
  // Finalize SHA256
  uint8_t hash[32];
  mbedtls_sha256_finish(&sha256_ctx, hash);
  char computed_sha256_hex[65];
  sha256ToHex(hash, computed_sha256_hex);
  
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Download complete: %lu bytes", downloaded_bytes);
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Final computed SHA256: %s", computed_sha256_hex);
  
  // Verify size (strict: must match expected_size if provided, otherwise Content-Length)
  if (expected_size > 0) {
    // Manifest provided size - must match exactly
    if (downloaded_bytes != expected_size) {
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Size mismatch: expected_size(manifest)=%lu, downloaded=%lu", 
                    expected_size, downloaded_bytes);
      if (ota_handle) esp_ota_abort(ota_handle);
      LCD_OTA_RETURN_FAIL(RESULT_FAILED_SIZE_MISMATCH);
    }
    // Also verify Content-Length if it was provided
    if (initial_contentLength > 0 && downloaded_bytes != (uint32_t)initial_contentLength) {
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Size mismatch: expected_size(manifest)=%lu, content_length=%d, downloaded=%lu", 
                    expected_size, initial_contentLength, downloaded_bytes);
      if (ota_handle) esp_ota_abort(ota_handle);
      LCD_OTA_RETURN_FAIL(RESULT_FAILED_SIZE_MISMATCH);
    }
    LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Size verified: downloaded=%lu matches manifest=%lu", 
                 downloaded_bytes, expected_size);
  } else if (initial_contentLength > 0) {
    // No manifest size, but Content-Length was provided - must match
    if (downloaded_bytes != (uint32_t)initial_contentLength) {
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Size mismatch: Content-Length=%d, downloaded=%lu", 
                    initial_contentLength, downloaded_bytes);
      if (ota_handle) esp_ota_abort(ota_handle);
      LCD_OTA_RETURN_FAIL(RESULT_FAILED_SIZE_MISMATCH);
    }
    LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Size verified: downloaded=%lu matches Content-Length=%d", 
                 downloaded_bytes, initial_contentLength);
  } else {
    // No size info - just log what we got
    LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Size unknown: downloaded=%lu (no manifest size or Content-Length)", 
                 downloaded_bytes);
  }
  
  // Verify SHA256
  if (!compareSha256(computed_sha256_hex, expected_sha256_hex)) {
    LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "SHA256 mismatch: expected %s, got %s", 
                  expected_sha256_hex, computed_sha256_hex);
    esp_ota_abort(ota_handle);
    LCD_OTA_RETURN_FAIL(RESULT_FAILED_SHA256_MISMATCH);
  }
  
  // Finalize OTA
  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_end FAIL: %s", esp_err_to_name(err));
    LCD_OTA_RETURN_FAIL(RESULT_FAILED_WRITE);
  }
  
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "esp_ota_end OK");
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "PASS: wrote image to OTA partition");
  LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Final: downloaded=%lu, target=%lu, contentLength=%d, partition_size=%lu", 
               downloaded_bytes, target_bytes, initial_contentLength, update->size);
  
  // If set_boot_and_reboot is true, set boot partition and reboot
  if (set_boot_and_reboot) {
#ifndef HALO_BOARD_LCD
    // Record expectation before switching boot partition so we can verify
    // on next boot that the running firmware version matches manifest.version.
#endif
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
#ifdef HALO_BOARD_LCD
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Failed to get running partition");
#else
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Failed to get running partition for expectation tracking");
#endif
      LCD_OTA_RETURN_FAIL(RESULT_SET_BOOT_FAIL);
    }

    if (!expected_version || strlen(expected_version) == 0) {
#ifdef HALO_BOARD_LCD
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Expected version is empty - cannot set boot partition");
#else
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "Expected version is empty - cannot record OTA expectation");
#endif
      LCD_OTA_RETURN_FAIL(RESULT_SET_BOOT_FAIL);
    }

#ifndef HALO_BOARD_LCD
    if (!OtaExpect::setPending(expected_version, running)) {
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "OtaExpect::setPending failed - aborting boot partition switch");
      LCD_OTA_RETURN_FAIL(RESULT_SET_BOOT_FAIL);
    }
#endif

    LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Setting boot partition to: %s (0x%x)", 
                 update->label, update->address);
    
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
      LOG_ERROR_TAG(LOG_TAG_OTA_WRITE, "esp_ota_set_boot_partition FAIL: %s", esp_err_to_name(err));
      LCD_OTA_RETURN_FAIL(RESULT_SET_BOOT_FAIL);
    }
#ifdef HALO_BOARD_LCD
    Serial.printf("[LCD_OTA] applier success set_boot_partition label=%s rebooting\n", update->label);
#endif
    LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "Boot partition set successfully - cleaning up network before reboot...");
    
    // CRITICAL: Clean up all network clients before reboot to prevent "Bad file number" errors
    // HTTP clients are already cleaned up above (lines 1509-1515), but ensure nothing is left open
    // Note: current_ctx should already be null at this point, but double-check
    if (current_ctx) {
      LOG_WARN_TAG(LOG_TAG_OTA_WRITE, "WARNING: current_ctx still exists before reboot - cleaning up");
      current_ctx->http.end();
      current_ctx->client.stop();
      delete current_ctx;
      current_ctx = nullptr;
    }
    
    // CRITICAL: Always stop member client before reboot (prevents errno=9 / Bad file number)
    client.stop();
    
    // Safe WiFi disconnect (only if STA was started; skips if mode OFF to avoid STA not started)
    HALO_SAFE_DISCONNECT("ota_cleanup", false);
    delay(50);
    
#ifdef HALO_BOARD_LCD
    if (lcd_ota_on_success) {
      lcd_ota_on_success(expected_version);
    }
#endif
    // Single reboot path: sets g_rebooting, logs [REBOOT], then ESP.restart() (no network after this)
    halo_reboot("ota_success");
    
    // Should not reach here, but if it does, return success anyway
    return RESULT_SUCCESS;
  } else {
    LOG_INFO_TAG(LOG_TAG_OTA_WRITE, "PASS: wrote image to OTA partition (no reboot - set_boot_and_reboot=false)");
    return RESULT_SUCCESS;
  }
}

// REMOVED: downloadAndVerify() and applyUpdate() methods (dead code, unused).
// These methods used Arduino Update library which may contain example GitHub URLs.
// Main code path uses esp_ota_* functions directly via applyToOtaPartition().
// Removing Update.h include eliminates any GitHub strings from Update library.

#ifdef OTA_SELF_TEST
// Self-test helper for marker version parser
// To enable: #define OTA_SELF_TEST before including this file
static void testMarkerVersionParser() {
  Serial.println("\n=== OTA Marker Parser Self-Test ===");
  
  struct TestCase {
    const char* input;
    const char* expected;
  };
  
  TestCase tests[] = {
    {"HALO_FW_MARKER:0.5.2|BUILD_ID:0.5.2-Jan 23 2026-16:54:09-unknown", "0.5.2"},
    {"HALO_FW_MARKER:0.5.2", "0.5.2"},
    {"HALO_FW_MARKER:0.5.2\r\n", "0.5.2"},
    {"HALO_FW_MARKER:1.2.3|BUILD_ID:test", "1.2.3"},
    {"HALO_FW_MARKER:0.0.1 ", "0.0.1"},
    {"HALO_FW_MARKER:10.20.30\t", "10.20.30"},
    {"HALO_FW_MARKER:0.5.2\0", "0.5.2"},
  };
  
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    const char* input = tests[i].input;
    const char* expected = tests[i].expected;
    
    // Find marker prefix
    const char* marker_prefix = "HALO_FW_MARKER:";
    const char* marker_start = strstr(input, marker_prefix);
    
    if (!marker_start) {
      Serial.printf("TEST %zu: FAILED - marker not found in: %s\n", i, input);
      continue;
    }
    
    const char* version_start = marker_start + strlen(marker_prefix);
    const char* buffer_end = input + strlen(input);
    
    // Parse version using same logic as marker check
    char found_version[32] = {0};
    size_t version_idx = 0;
    bool parse_success = false;
    
    for (const char* p = version_start; p < buffer_end && version_idx < (sizeof(found_version) - 1); p++) {
      char c = *p;
      
      // Stop at delimiters
      if (c == '|' || c == '\0' || c == '\r' || c == '\n' || c == ' ' || c == '\t') {
        if (version_idx > 0) {
          found_version[version_idx] = '\0';
          parse_success = true;
        }
        break;
      }
      
      // Accept only digits and dots
      if ((c >= '0' && c <= '9') || c == '.') {
        found_version[version_idx++] = c;
      } else {
        if (version_idx > 0) {
          found_version[version_idx] = '\0';
          parse_success = true;
        }
        break;
      }
    }
    
    if (!parse_success && version_idx > 0) {
      found_version[version_idx] = '\0';
      parse_success = true;
    }
    
    bool passed = parse_success && strlen(found_version) > 0 && strcmp(found_version, expected) == 0;
    
    if (passed) {
      Serial.printf("TEST %zu: PASSED - input='%s' -> parsed='%s' (expected='%s')\n", 
                   i, input, found_version, expected);
    } else {
      Serial.printf("TEST %zu: FAILED - input='%s' -> parsed='%s' (expected='%s')\n", 
                   i, input, found_version ? found_version : "<empty>", expected);
    }
  }
  
  Serial.println("=== End Self-Test ===\n");
}
#endif // OTA_SELF_TEST

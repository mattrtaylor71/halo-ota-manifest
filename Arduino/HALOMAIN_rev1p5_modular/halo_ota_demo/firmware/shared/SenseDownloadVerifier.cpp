#include "SenseDownloadVerifier.h"
#include "Log.h"
#include "BuildFlags.h"            // OTA_TLS_INSECURE_DEBUG
#include "../shared/AmazonRootCa.h"
#include <WiFi.h>
#include <string.h>
#include <stdio.h>  // For sprintf
#include <esp_system.h>

#define LOG_TAG_DOWNLOAD "DOWNLOAD"

SenseDownloadVerifier::SenseDownloadVerifier() 
  : downloaded_size(0)
  , last_progress_log_ms(0)
  , last_progress_bytes(0) {
  computed_sha256_hex[0] = '\0';
#if OTA_TLS_INSECURE_DEBUG
  secure_client.setInsecure();
#else
  secure_client.setCACert(kAmazonRootCa1);
#endif
  mbedtls_sha256_init(&sha256_ctx);
}

SenseDownloadVerifier::~SenseDownloadVerifier() {
  secure_client.stop();
  mbedtls_sha256_free(&sha256_ctx);
}

const char* SenseDownloadVerifier::getResultString(Result result) {
  switch (result) {
    case RESULT_SUCCESS: return "SUCCESS";
    case RESULT_FAIL_CONNECTION: return "CONNECTION_FAILED";
    case RESULT_FAIL_HTTP: return "HTTP_ERROR";
    case RESULT_FAIL_SIZE_MISMATCH: return "SIZE_MISMATCH";
    case RESULT_FAIL_SHA256_MISMATCH: return "SHA256_MISMATCH";
    case RESULT_FAIL_TIMEOUT: return "TIMEOUT";
    case RESULT_FAIL_INVALID_PARAMS: return "INVALID_PARAMS";
    case RESULT_FAIL_NOT_BINARY: return "NOT_BINARY";
    case RESULT_FAIL_INCOMPLETE: return "INCOMPLETE";
    case RESULT_FAIL_TRUNCATED: return "TRUNCATED";
    default: return "UNKNOWN";
  }
}

void SenseDownloadVerifier::updateProgress(uint32_t bytes_downloaded, uint32_t target_bytes, uint32_t free_heap) {
  unsigned long now = millis();
  bool should_log = false;
  
  // Log if 2 seconds passed OR 32KB downloaded
  if ((now - last_progress_log_ms) >= PROGRESS_LOG_INTERVAL_MS) {
    should_log = true;
    last_progress_log_ms = now;
  } else if ((bytes_downloaded - last_progress_bytes) >= PROGRESS_LOG_SIZE_INTERVAL) {
    should_log = true;
  }
  
  if (should_log) {
    uint32_t remaining = (target_bytes > bytes_downloaded) ? (target_bytes - bytes_downloaded) : 0;
    float mb = bytes_downloaded / (1024.0 * 1024.0);
    LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Progress: %.2f MB, downloaded=%lu, target=%lu, remaining=%lu, free_heap=%lu", 
                 mb, bytes_downloaded, target_bytes, remaining, free_heap);
    last_progress_bytes = bytes_downloaded;
  }
}

void SenseDownloadVerifier::finalizeSha256() {
  unsigned char hash[32];
  mbedtls_sha256_finish(&sha256_ctx, hash);
  
  // Convert to hex string
  for (int i = 0; i < 32; i++) {
    sprintf(&computed_sha256_hex[i * 2], "%02x", hash[i]);
  }
  computed_sha256_hex[64] = '\0';
}

bool SenseDownloadVerifier::compareSha256(const char* computed, const char* expected) {
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

SenseDownloadVerifier::Result SenseDownloadVerifier::verifyDownload(
    const char* bin_url, const char* expected_sha256, 
    uint32_t expected_size, unsigned long timeout_ms) {
  
  // Reset state
  downloaded_size = 0;
  computed_sha256_hex[0] = '\0';
  last_progress_log_ms = millis();
  last_progress_bytes = 0;
  
  // Validate parameters
  if (!bin_url || strlen(bin_url) == 0 || !expected_sha256 || strlen(expected_sha256) != 64) {
    LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "Invalid parameters");
    return RESULT_FAIL_INVALID_PARAMS;
  }
  
  // Log initial state
  uint32_t free_heap_start = ESP.getFreeHeap();
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Starting download from: %s", bin_url);
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Expected SHA256: %s", expected_sha256);
  if (expected_size > 0) {
    LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Expected size: %lu bytes", expected_size);
  }
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Free heap at start: %lu bytes", free_heap_start);
  
  // Initialize SHA256
  mbedtls_sha256_starts(&sha256_ctx, 0);  // 0 = SHA256 (not SHA224)
  
  // Setup HTTPClient with WiFiClientSecure
  HTTPClient http;
#if OTA_TLS_INSECURE_DEBUG
  secure_client.setInsecure();
#else
  secure_client.setCACert(kAmazonRootCa1);
#endif

  // Begin HTTP connection
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "HTTP begin...");
  if (!http.begin(secure_client, bin_url)) {
    LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "HTTP begin failed");
    http.end();
    return RESULT_FAIL_CONNECTION;
  }
  
  uint32_t free_heap_after_begin = ESP.getFreeHeap();
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "HTTP begin complete, free_heap=%lu", free_heap_after_begin);
  
  http.setConnectTimeout(15000);  // 15 second connection timeout (OK before GET)
  http.setReuse(false);  // Disable connection reuse for reliability
  
  // Perform GET request
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Sending HTTP GET...");
  int httpCode = http.GET();
  
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "HTTP status code: %d", httpCode);
  
  // Only set read timeout after connection established to avoid errno=9 on invalid socket
  if (httpCode == HTTP_CODE_OK && secure_client.connected()) {
    http.setTimeout(timeout_ms);
  }
  
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "HTTP GET failed with code: %d", httpCode);
    http.end();
    return RESULT_FAIL_HTTP;
  }
  
  // Get content length (if available)
  int contentLength = http.getSize();
  if (contentLength > 0) {
    LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Content-Length: %d bytes", contentLength);
  } else {
    LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Content-Length: unknown");
  }
  
  // Get stream pointer
  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "Failed to get stream pointer");
    http.end();
    return RESULT_FAIL_HTTP;
  }
  
  // Check if response is HTML (not binary)
  // Check Content-Type header first
  String contentType = http.header("Content-Type");
  if (contentType.length() > 0) {
    contentType.toLowerCase();
    if (contentType.indexOf("text/html") >= 0) {
      LOG_WARN_TAG(LOG_TAG_DOWNLOAD, "Content-Type indicates HTML: %s", contentType.c_str());
      http.end();
      return RESULT_FAIL_NOT_BINARY;
    }
  }
  
  // Read first bytes to check for HTML markers, then process them normally
  // This ensures bytes are not lost - they'll be included in SHA256 and download count
  uint8_t first_check_buffer[16];
  size_t first_check_read = 0;
  
  if (stream->available() > 0) {
    first_check_read = stream->readBytes(first_check_buffer, sizeof(first_check_buffer));
    
    if (first_check_read >= 9) {
      // Check for <!DOCTYPE
      if (first_check_buffer[0] == '<' && first_check_buffer[1] == '!' && first_check_buffer[2] == 'D' && 
          first_check_buffer[3] == 'O' && first_check_buffer[4] == 'C' && first_check_buffer[5] == 'T' && 
          first_check_buffer[6] == 'Y' && first_check_buffer[7] == 'P' && first_check_buffer[8] == 'E') {
        LOG_WARN_TAG(LOG_TAG_DOWNLOAD, "Response starts with <!DOCTYPE (HTML detected)");
        http.end();
        return RESULT_FAIL_NOT_BINARY;
      }
    }
    if (first_check_read >= 5) {
      // Check for <html
      if (first_check_buffer[0] == '<' && first_check_buffer[1] == 'h' && first_check_buffer[2] == 't' && 
          first_check_buffer[3] == 'm' && first_check_buffer[4] == 'l') {
        LOG_WARN_TAG(LOG_TAG_DOWNLOAD, "Response starts with <html (HTML detected)");
        http.end();
        return RESULT_FAIL_NOT_BINARY;
      }
    }
    
    // If we read bytes and they're not HTML, process them immediately
    // (they'll be included in SHA256 and downloaded_size in the main loop)
    if (first_check_read > 0) {
      // Process these bytes through the normal pipeline
      mbedtls_sha256_update(&sha256_ctx, first_check_buffer, first_check_read);
      downloaded_size += first_check_read;
      LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "First check: %zu bytes (checked for HTML, included in download), free_heap=%lu", 
                   first_check_read, ESP.getFreeHeap());
    }
  }
  
  // Determine target bytes for completion
  uint32_t target_bytes = 0;
  if (expected_size > 0) {
    target_bytes = expected_size;
  } else if (contentLength > 0) {
    target_bytes = contentLength;
  } else {
    // Unknown size - download until connection closes
    target_bytes = 0;  // 0 = download until EOF
  }
  
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Target bytes: %lu (expected_size=%lu, contentLength=%d)", 
               target_bytes, expected_size, contentLength);
  
  // Stream download and compute SHA256
  unsigned long start_ms = millis();
  unsigned long last_progress_ms = start_ms;
  const uint32_t NO_PROGRESS_TIMEOUT_MS = 30000;  // 30 seconds no-progress timeout (not total timeout)
  
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Starting stream download...");
  
  while (true) {
    // Check no-progress timeout (30s since last successful read)
    unsigned long now_ms = millis();
    if ((now_ms - last_progress_ms) > NO_PROGRESS_TIMEOUT_MS) {
      LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "No-progress timeout: downloaded=%lu, target=%lu, available=%d, connected=%d", 
                    downloaded_size, target_bytes, stream->available(), http.connected());
      http.end();
      return RESULT_FAIL_TIMEOUT;
    }
    
    // Check if we've reached target bytes
    if (target_bytes > 0 && downloaded_size >= target_bytes) {
      LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Reached target bytes: %lu", target_bytes);
      break;  // Success - downloaded enough
    }
    
    // Check available bytes
    size_t available = stream->available();
    
    if (available == 0) {
      // No data available
      if (target_bytes > 0 && downloaded_size >= target_bytes) {
        // We have enough bytes
        break;
      } else if (!http.connected()) {
        // Connection closed
        if (target_bytes > 0 && downloaded_size < target_bytes) {
          // Stream ended before reaching target_bytes
          LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "Stream truncated: downloaded=%lu, target=%lu", 
                        downloaded_size, target_bytes);
          http.end();
          return RESULT_FAIL_TRUNCATED;
        } else if (target_bytes == 0) {
          // Unknown size - connection closed, require at least some bytes
          if (downloaded_size > 0) {
            LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Connection closed, size unknown, downloaded=%lu bytes", 
                         downloaded_size);
            break;
          } else {
            LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "Connection closed with zero bytes downloaded");
            http.end();
            return RESULT_FAIL_INCOMPLETE;
          }
        } else {
          // We have enough bytes
          break;
        }
      } else {
        // Still connected but no data - wait briefly
        delay(1);
        continue;
      }
    }
    
    // Read chunk (limit to buffer size and remaining bytes if target known)
    size_t to_read = (available > DOWNLOAD_BUFFER_SIZE) ? DOWNLOAD_BUFFER_SIZE : available;
    if (target_bytes > 0 && downloaded_size < target_bytes) {
      size_t remaining = target_bytes - downloaded_size;
      if (to_read > remaining) {
        to_read = remaining;
      }
    }
    
    size_t bytes_read = stream->readBytes(download_buffer, to_read);
    
    if (bytes_read == 0) {
      // No bytes read - check if we should continue
      if (target_bytes > 0 && downloaded_size >= target_bytes) {
        break;
      } else if (!http.connected()) {
        // Connection closed
        if (target_bytes > 0 && downloaded_size < target_bytes) {
          LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "Stream truncated: downloaded=%lu, target=%lu", 
                        downloaded_size, target_bytes);
          http.end();
          return RESULT_FAIL_TRUNCATED;
        } else if (target_bytes == 0) {
          if (downloaded_size > 0) {
            break;  // Unknown size, got some bytes, assume complete
          } else {
            LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "Connection closed with zero bytes");
            http.end();
            return RESULT_FAIL_INCOMPLETE;
          }
        } else {
          break;  // We have enough
        }
      }
      delay(1);
      continue;
    }
    
    // Update last progress time (we read bytes > 0)
    last_progress_ms = millis();
    
    // Update SHA256 incrementally
    mbedtls_sha256_update(&sha256_ctx, download_buffer, bytes_read);
    
    // Update size
    downloaded_size += bytes_read;
    
    // Log progress (every 32KB or 2 seconds)
    updateProgress(downloaded_size, target_bytes, ESP.getFreeHeap());
    
    // Check if we've reached target (after reading)
    if (target_bytes > 0 && downloaded_size >= target_bytes) {
      LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Reached target bytes after read: %lu", target_bytes);
      break;
    }
  }
  
  // Clean up HTTP connection
  http.end();
  
  // Finalize SHA256
  finalizeSha256();
  
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Download complete: %lu bytes", downloaded_size);
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Final computed SHA256: %s", computed_sha256_hex);
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Free heap at end: %lu bytes", ESP.getFreeHeap());
  
  // Verify size
  // Priority: expected_size > contentLength > no check
  if (expected_size > 0) {
    // Must match expected_size exactly
    if (downloaded_size != expected_size) {
      LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "Size mismatch: expected %lu, got %lu", 
                    expected_size, downloaded_size);
      return RESULT_FAIL_SIZE_MISMATCH;
    }
  } else if (contentLength > 0) {
    // If no expected_size but we have contentLength, verify against that
    if (downloaded_size != (uint32_t)contentLength) {
      LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "Size mismatch: Content-Length was %d, got %lu", 
                    contentLength, downloaded_size);
      return RESULT_FAIL_SIZE_MISMATCH;
    }
  }
  // If neither expected_size nor contentLength, we don't verify size
  
  // Verify SHA256
  if (!compareSha256(computed_sha256_hex, expected_sha256)) {
    LOG_ERROR_TAG(LOG_TAG_DOWNLOAD, "SHA256 mismatch: expected %s, got %s", 
                  expected_sha256, computed_sha256_hex);
    return RESULT_FAIL_SHA256_MISMATCH;
  }
  
  LOG_INFO_TAG(LOG_TAG_DOWNLOAD, "Verification PASS: size and SHA256 match");
  return RESULT_SUCCESS;
}

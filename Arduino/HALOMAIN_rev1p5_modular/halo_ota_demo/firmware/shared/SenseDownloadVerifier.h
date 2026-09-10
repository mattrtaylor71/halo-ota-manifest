#ifndef SENSE_DOWNLOAD_VERIFIER_H
#define SENSE_DOWNLOAD_VERIFIER_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/sha256.h>

/**
 * SenseDownloadVerifier: Downloads firmware binary and verifies SHA256 (DRY RUN)
 * 
 * Phase 1B: Stream download + SHA256 verification without OTA write/apply
 * - Uses fixed-size buffer (4096 bytes) for bounded RAM usage
 * - Logs progress every 250KB or 2 seconds
 * - Verifies SHA256 and optional size
 */
class SenseDownloadVerifier {
public:
  enum Result {
    RESULT_SUCCESS,
    RESULT_FAIL_CONNECTION,
    RESULT_FAIL_HTTP,
    RESULT_FAIL_SIZE_MISMATCH,
    RESULT_FAIL_SHA256_MISMATCH,
    RESULT_FAIL_TIMEOUT,
    RESULT_FAIL_INVALID_PARAMS,
    RESULT_FAIL_NOT_BINARY,  // Server returned HTML instead of binary
    RESULT_FAIL_INCOMPLETE,  // Connection closed before all bytes received
    RESULT_FAIL_TRUNCATED    // Stream ended before reaching target_bytes
  };
  
  SenseDownloadVerifier();
  ~SenseDownloadVerifier();
  
  // Verify download (DRY RUN - no OTA write)
  // bin_url: URL to download from
  // expected_sha256: Expected SHA256 hash (64 hex chars, case-insensitive)
  // expected_size: Expected size in bytes (0 = don't check)
  // timeout_ms: Maximum time for entire download
  // Returns RESULT_SUCCESS if download and verification pass
  Result verifyDownload(const char* bin_url, const char* expected_sha256, 
                        uint32_t expected_size, unsigned long timeout_ms);
  
  // Get computed SHA256 (after verifyDownload)
  const char* getComputedSha256() const { return computed_sha256_hex; }
  
  // Get downloaded size (after verifyDownload)
  uint32_t getDownloadedSize() const { return downloaded_size; }
  
  // Get result string for logging
  static const char* getResultString(Result result);

private:
  WiFiClientSecure secure_client;
  char computed_sha256_hex[65];  // 64 hex chars + null
  uint32_t downloaded_size;
  
  // Progress logging
  unsigned long last_progress_log_ms;
  uint32_t last_progress_bytes;
  static const uint32_t PROGRESS_LOG_INTERVAL_MS = 2000;  // 2 seconds
  static const uint32_t PROGRESS_LOG_SIZE_INTERVAL = 32 * 1024;  // 32KB (changed from 250KB)
  
  // Download buffer (fixed size - small for streaming)
  static const size_t DOWNLOAD_BUFFER_SIZE = 4096;
  uint8_t download_buffer[DOWNLOAD_BUFFER_SIZE];
  
  // SHA256 context
  mbedtls_sha256_context sha256_ctx;
  
  // Helper methods
  void updateProgress(uint32_t bytes_downloaded, uint32_t target_bytes, uint32_t free_heap);
  void finalizeSha256();
  bool compareSha256(const char* computed, const char* expected);
};

#endif // SENSE_DOWNLOAD_VERIFIER_H

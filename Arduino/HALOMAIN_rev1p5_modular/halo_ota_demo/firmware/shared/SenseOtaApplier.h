#ifndef SENSE_OTA_APPLIER_H
#define SENSE_OTA_APPLIER_H
#include <esp_heap_caps.h>
#ifndef HALO_DURABLE_OTA_POLICY
#define HALO_DURABLE_OTA_POLICY 0
#endif

#include <Arduino.h>
// Update.h removed: downloadAndVerify() and applyUpdate() are unused (dead code).
// Main code path uses esp_ota_* functions directly via applyToOtaPartition().
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include "ManifestClient.h"  // For OtaManifest struct

/**
 * SenseOtaApplier: Handles OTA update flow for Sense board.
 * 
 * Phase 1: Dual-partition Update flow with SHA256 verification.
 * - Downloads firmware image via HTTPS
 * - Computes SHA256 while streaming
 * - Verifies SHA256 matches manifest
 * - Writes to OTA partition
 * - Reboots on success
 * - Reports failure without crashing
 */
class SenseOtaApplier {
public:
  enum Result {
    RESULT_SUCCESS,
    RESULT_FAILED_DOWNLOAD,
    RESULT_FAILED_SHA256_MISMATCH,
    RESULT_FAILED_WRITE,
    RESULT_FAILED_VERIFY,
    RESULT_FAILED_INVALID_MANIFEST,
    RESULT_PARTITION_TOO_SMALL,
    RESULT_FAILED_TIMEOUT,         // Hard deadline timeout exceeded
    RESULT_FAILED_NO_PROGRESS,     // No progress timeout exceeded
    RESULT_FAILED_INCOMPLETE,
    RESULT_FAILED_SIZE_MISMATCH,
    RESULT_NO_STREAM,              // getStreamPtr() returned null
    RESULT_HTTP_BEGIN_FAIL,        // http.begin() failed
    RESULT_HTTP_GET_FAIL,          // http.GET() failed
    RESULT_STALL_RETRY_EXHAUSTED,  // Stall recovery exhausted all retries
    RESULT_SET_BOOT_FAIL,          // esp_ota_set_boot_partition() failed
    RESULT_FAILED_MARKER_NOT_FOUND, // No valid HALO_FW_MARKER found in binary
    RESULT_FAILED_MARKER_MISMATCH, // Marker version doesn't match manifest version
    RESULT_DEFERRED_POLICY        // No committed destructive-work reservation; no SDK begin
  };
  
  // One invocation's write/integrity failure detail. RAM only; no allocation or NVS.
  enum class FailureStage : uint8_t {
    NONE, HEAP_START, PARTITION, BEGIN_INITIAL, BEGIN_RESTART, BEGIN_RANGE,
    WRITE_MARKER, WRITE_PROBE, HEAP_STREAM, BEGIN_SAME_OFFSET,
    BEGIN_NO_PROGRESS, WRITE_STREAM, END, SHA_CHECK
  };
  struct FailureSnapshot {
    FailureStage stage;
    int32_t sdk_error;       // 0 when no SDK error applies (heap/partition guards).
    uint32_t offset;         // Accepted bytes in this write session; begin = 0.
    uint32_t expected_bytes; // Manifest size, else resolved target, else 0.
    uint32_t free_heap;      // Sampled before failure logging/cleanup.
    uint32_t largest_internal;
    uint32_t minimum_internal;
  };
  static_assert(sizeof(FailureSnapshot) <= 28, "OTA failure snapshot must stay small");
  FailureSnapshot getFailureSnapshot() const { return failure_snapshot_; }
  static const char* getFailureStageString(FailureStage stage);

  SenseOtaApplier();
  ~SenseOtaApplier();
  
  // Apply OTA update from manifest
  // Returns RESULT_SUCCESS on success, error code on failure
  // On success, device will reboot (does not return)
  Result applyUpdate(const struct OtaManifest& manifest, unsigned long timeout_ms = 60000);
  
  // Borrowed for this synchronous invocation; the caller owns immutable target
  // identity and a committed APPLY operation. Called only with no live OTA
  // handle or HTTP context, once BEFORE each actual initial/restarted begin.
  // The callback must commit/read back the begin charge and may not extend the
  // original invocation budget. No callback or source of allowance is stored.
  struct BeginAdmission {
    void* owner;
    bool (*reserve)(void* owner, FailureStage site, uint32_t invocation_started_ms,
                    uint32_t invocation_budget_ms);
  };

  // Apply OTA update by streaming download and writing to OTA partition.
  // url: Download URL (HTTPS)
  // expected_sha256_hex: Expected SHA256 hash (64 hex chars)
  // expected_size: Expected size in bytes (0 = unknown, use Content-Length if available)
  // hard_deadline_ms: Hard deadline timeout (milliseconds) - absolute maximum time for entire download
  // set_boot_and_reboot: If true, set boot partition and reboot (Phase 1: always false)
  // expected_version: Manifest version string (used for OtaExpect pending tracking)
  // Returns RESULT_SUCCESS if download, write, and verification pass (no reboot)
  Result applyToOtaPartition(const char* url, const char* expected_sha256_hex, 
                              uint32_t expected_size, uint32_t hard_deadline_ms, 
                              bool set_boot_and_reboot,
                              const char* expected_version,
                              const BeginAdmission* admission = nullptr);
  
  // Get result description string
  static const char* getResultString(Result result);

private:
  FailureSnapshot failure_snapshot_{};
  void captureWriteFailure(FailureStage stage, int32_t sdk_error,
                           uint32_t offset, uint32_t expected_bytes,
                           uint32_t free_heap) {
    failure_snapshot_ = {stage, sdk_error, offset, expected_bytes, free_heap,
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)};
  }

  mbedtls_sha256_context sha256_ctx;
  
  // Parse URL (extract host, port, path)
  bool parseUrl(const char* url, String& host, int& port, String& path);
  
  // Compute SHA256 hex string from binary hash
  void sha256ToHex(const uint8_t* hash, char* hex_str);
  
  // Compare SHA256 hex strings (case-insensitive)
  bool compareSha256(const char* computed, const char* expected);
};

#endif // SENSE_OTA_APPLIER_H

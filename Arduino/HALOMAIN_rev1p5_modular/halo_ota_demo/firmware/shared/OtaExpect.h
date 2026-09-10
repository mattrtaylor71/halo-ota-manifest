#ifndef OTA_EXPECT_H
#define OTA_EXPECT_H

#include <Arduino.h>
#include <esp_ota_ops.h>

/**
 * OtaExpect: persists OTA expectations across reboot using NVS (Preferences).
 *
 * Used to guard against mismatched artifacts: when we decide to boot into a
 * newly written OTA partition, we record the expected firmware version and the
 * previous running partition. On next boot we verify that the running firmware
 * version matches the expected one; if not, we revert to the previous
 * partition.
 */
namespace OtaExpect {

// Maximum stored string lengths (including null terminator)
static const size_t EXPECTED_VERSION_MAX_LEN = 31;   // Matches typical FIRMWARE_VERSION size
static const size_t PREV_LABEL_MAX_LEN      = 15;   // esp_partition_t::label is 16 bytes inc. null
static const size_t LAST_ERROR_MAX_LEN      = 63;   // Short error code / reason

// Store expectation before switching boot partition.
// expected_version: semantic version string from manifest (e.g., "0.5.1")
// prev_running: currently running partition (before switching boot partition)
bool setPending(const char* expected_version, const esp_partition_t* prev_running);

// Check if there is a pending expectation recorded.
bool isPending();

// Get expected version string. Returns true if present.
bool getExpectedVersion(char* out, size_t out_sz);

// Get previous partition info (label + address). Returns true if present.
bool getPrevPartitionInfo(char* label_out, size_t label_out_sz, uint32_t& address_out);

// Clear any pending expectation (clears pending flag and optional metadata).
bool clearPending();

// Optional: record last error (e.g. before rollback). Max LAST_ERROR_MAX_LEN-1 chars.
void setLastError(const char* error);
bool getLastError(char* out, size_t out_sz);

// Optional: record last success timestamp (seconds since boot or epoch). Cleared by clearPending.
void setLastSuccessTs(uint32_t ts_sec);
uint32_t getLastSuccessTs();

}  // namespace OtaExpect

#endif  // OTA_EXPECT_H


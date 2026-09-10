#ifndef MANIFEST_CLIENT_H
#define MANIFEST_CLIENT_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ManifestOutcome.h"

/**
 * OTA Manifest structure
 */
struct OtaManifest {
  char version[32];           // Semantic version string (e.g., "1.0.0")
  char url[256];              // Download URL (HTTPS)
  uint32_t size;              // Firmware size in bytes (0 = unknown/not specified)
  char sha256[65];            // SHA256 hash (hex string, 64 chars + null)
  char min_version[32];       // Minimum required version (optional)
  char build_id[128];         // Build identity string (optional, for artifact verification)
  char artifact_fw_version[32];  // Alternative: artifact firmware version (optional)
  char board[16];               // Optional board identifier (e.g., "sense", "lcd")
  uint8_t rollout_pct;        // Optional rollout percent (0-100)
  uint32_t rollout_seed;      // Optional rollout seed
  char min_version_allowed[32]; // Optional rollout min version gate
  bool valid;                 // True if manifest was successfully parsed
  bool size_specified;        // True if size was explicitly provided in manifest
  bool build_id_specified;    // True if build_id was provided in manifest
  bool artifact_fw_version_specified;  // True if artifact_fw_version was provided
  bool board_specified;       // True if board was provided
  bool rollout_pct_specified; // True if rollout_pct was provided
  bool rollout_seed_specified; // True if rollout_seed was provided
  bool min_version_allowed_specified; // True if min_version_allowed was provided
  
  OtaManifest() : size(0), valid(false), size_specified(false), 
                  build_id_specified(false), artifact_fw_version_specified(false),
                  board_specified(false),
                  rollout_pct(0), rollout_seed(0),
                  rollout_pct_specified(false), rollout_seed_specified(false),
                  min_version_allowed_specified(false) {
    version[0] = '\0';
    url[0] = '\0';
    sha256[0] = '\0';
    min_version[0] = '\0';
    build_id[0] = '\0';
    artifact_fw_version[0] = '\0';
    board[0] = '\0';
    min_version_allowed[0] = '\0';
  }
};

/**
 * ManifestClient: Fetches and parses OTA manifest from HTTPS endpoint.
 * 
 * Phase 1: HTTPS GET to manifest endpoint, parse JSON into OtaManifest struct.
 * - Explicit timeouts for all HTTP operations
 * - Returns parsed manifest or invalid manifest on failure
 */
class ManifestClient {
public:
  ManifestClient() = default;
  ~ManifestClient() = default;
  
  // Fetch manifest from URL
  // Returns true if manifest was successfully fetched and parsed
  // timeout_ms: maximum time to wait for HTTP response
  bool fetchManifest(const char* url, OtaManifest& manifest, unsigned long timeout_ms = 10000);
  
  // Final synchronous call outcome; immutable until the next parse/fetch.
  const ManifestOutcome& outcome() const { return outcome_; }

  // Check if current firmware version matches manifest version
  // Returns true if versions match (no update needed)
  bool isVersionCurrent(const OtaManifest& manifest, const char* current_version);
  
  // Compare two semantic versions (X.Y.Z format)
  // Returns: -1 if v1 < v2, 0 if v1 == v2, 1 if v1 > v2
  // Returns 0 if either version is invalid
  static int compareVersions(const char* v1, const char* v2);
  
  // Check if manifest version is newer than current version
  // Returns true if manifest.version > current_version
  bool isVersionNewer(const OtaManifest& manifest, const char* current_version);

  // Parse JSON manifest (public for raw HTTP path on LCD)
  bool parseManifestJson(const String& json, OtaManifest& manifest);

private:
  ManifestOutcome outcome_{};
  bool parseManifestJsonObserved(const String& json, OtaManifest& manifest);
  // Request-local secure clients own the actual connection; no unused
  // permanently allocated TLS context lives in the manifest parser.
  
  // Extract string field from JSON (with bounds checking)
  bool extractStringField(JsonObject& obj, const char* field, char* dest, size_t dest_size);
};

#endif // MANIFEST_CLIENT_H

#include "ManifestClient.h"
#include "Log.h"
#include "BuildFlags.h"
#include "../shared/AmazonRootCa.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#define LOG_TAG_MANIFEST "MANIFEST"

namespace {
static void log_tls_context() {
  time_t now = time(nullptr);
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  char time_buf[32];
  strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  IPAddress dns0 = WiFi.dnsIP(0);
  IPAddress dns1 = WiFi.dnsIP(1);
  LOG_INFO_TAG(LOG_TAG_MANIFEST,
               "TLS ctx time=%ld utc=%s wifi_status=%d rssi=%d ip=%s dns0=%s dns1=%s",
               static_cast<long>(now),
               time_buf,
               static_cast<int>(WiFi.status()),
               WiFi.RSSI(),
               WiFi.localIP().toString().c_str(),
               dns0.toString().c_str(),
               dns1.toString().c_str());
}
}  // namespace

bool ManifestClient::extractStringField(JsonObject& obj, const char* field, char* dest, size_t dest_size) {
  if (!obj.containsKey(field)) {
    LOG_WARN_TAG(LOG_TAG_MANIFEST, "Missing field: %s", field);
    return false;
  }
  
  const char* value = obj[field];
  if (!value) {
    LOG_WARN_TAG(LOG_TAG_MANIFEST, "Field %s is null", field);
    return false;
  }
  
  size_t len = strlen(value);
  if (len >= dest_size) {
    LOG_WARN_TAG(LOG_TAG_MANIFEST, "Field %s too long: %zu >= %zu", field, len, dest_size);
    return false;
  }
  
  strncpy(dest, value, dest_size - 1);
  dest[dest_size - 1] = '\0';
  return true;
}

bool ManifestClient::parseManifestJson(const String& json, OtaManifest& manifest) {
  outcome_ = ManifestOutcome{};
  return parseManifestJsonObserved(json, manifest);
}

bool ManifestClient::parseManifestJsonObserved(const String& json, OtaManifest& manifest) {
  // Reset manifest
  manifest = OtaManifest();
  
  // Parse JSON (using DynamicJsonDocument for flexibility)
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, json);
  
  if (error) {
    outcome_.stage=ManifestStage::JsonSyntax;outcome_.parser_code=uint8_t(error.code());
    LOG_ERROR_TAG(LOG_TAG_MANIFEST, "JSON parse error: %s", error.c_str());
    return false;
  }
  
  if (!doc.is<JsonObject>()) {
    outcome_.stage=ManifestStage::JsonObject;
    LOG_ERROR_TAG(LOG_TAG_MANIFEST, "JSON is not an object");
    return false;
  }
  
  JsonObject obj = doc.as<JsonObject>();
  
  // Extract required fields
  if (!extractStringField(obj, "version", manifest.version, sizeof(manifest.version))) {
    outcome_.stage=ManifestStage::VersionField;
    return false;
  }
  
  // Support both "url" and "bin_url" (bin_url is preferred for new format)
  bool url_found = false;
  if (obj.containsKey("bin_url")) {
    url_found = extractStringField(obj, "bin_url", manifest.url, sizeof(manifest.url));
  }
  if (!url_found && obj.containsKey("url")) {
    url_found = extractStringField(obj, "url", manifest.url, sizeof(manifest.url));
  }
  if (!url_found) {
    outcome_.stage=ManifestStage::UrlField;
    LOG_WARN_TAG(LOG_TAG_MANIFEST, "Missing 'url' or 'bin_url' field");
    return false;
  }
  
  // Size is optional (default to 0 if not present, but track if it was specified)
  if (obj.containsKey("size") && obj["size"].is<uint32_t>()) {
    manifest.size = obj["size"].as<uint32_t>();
    manifest.size_specified = true;
  } else {
    manifest.size = 0;  // Unknown/not specified
    manifest.size_specified = false;
  }
  
  if (!extractStringField(obj, "sha256", manifest.sha256, sizeof(manifest.sha256))) {
    outcome_.stage=ManifestStage::ShaField;
    return false;
  }
  
  // Validate SHA256 length (should be 64 hex chars)
  if (strlen(manifest.sha256) != 64) {
    outcome_.stage=ManifestStage::ShaField;
    LOG_WARN_TAG(LOG_TAG_MANIFEST, "Invalid SHA256 length: %zu (expected 64)", strlen(manifest.sha256));
    return false;
  }
  
  // min_version is optional (extract and store it)
  if (obj.containsKey("min_version")) {
    if (!extractStringField(obj, "min_version", manifest.min_version, sizeof(manifest.min_version))) {
      // If present but invalid, log warning but continue
      LOG_WARN_TAG(LOG_TAG_MANIFEST, "Invalid min_version field, ignoring");
      manifest.min_version[0] = '\0';
    }
  } else {
    manifest.min_version[0] = '\0';
  }

  // rollout_pct is optional (0-100)
  if (obj.containsKey("rollout_pct")) {
    int pct = obj["rollout_pct"] | 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    manifest.rollout_pct = (uint8_t)pct;
    manifest.rollout_pct_specified = true;
  } else {
    manifest.rollout_pct = 0;
    manifest.rollout_pct_specified = false;
  }

  // rollout_seed is optional
  if (obj.containsKey("rollout_seed")) {
    manifest.rollout_seed = obj["rollout_seed"] | 0;
    manifest.rollout_seed_specified = true;
  } else {
    manifest.rollout_seed = 0;
    manifest.rollout_seed_specified = false;
  }

  // min_version_allowed is optional (rollout gate)
  if (obj.containsKey("min_version_allowed")) {
    if (!extractStringField(obj, "min_version_allowed",
                            manifest.min_version_allowed,
                            sizeof(manifest.min_version_allowed))) {
      LOG_WARN_TAG(LOG_TAG_MANIFEST, "Invalid min_version_allowed field, ignoring");
      manifest.min_version_allowed[0] = '\0';
      manifest.min_version_allowed_specified = false;
    } else {
      manifest.min_version_allowed_specified = true;
    }
  } else {
    manifest.min_version_allowed[0] = '\0';
    manifest.min_version_allowed_specified = false;
  }
  
  // build_id is REQUIRED (fail if missing)
  if (!obj.containsKey("build_id")) {
    outcome_.stage=ManifestStage::BuildField;
    LOG_ERROR_TAG(LOG_TAG_MANIFEST, "Missing required field: build_id");
    return false;
  }
  if (!extractStringField(obj, "build_id", manifest.build_id, sizeof(manifest.build_id))) {
    outcome_.stage=ManifestStage::BuildField;
    LOG_ERROR_TAG(LOG_TAG_MANIFEST, "Invalid build_id field");
    return false;
  }
  manifest.build_id_specified = true;
  
  // Validate build_id is not empty
  if (strlen(manifest.build_id) == 0) {
    outcome_.stage=ManifestStage::BuildField;
    LOG_ERROR_TAG(LOG_TAG_MANIFEST, "build_id is empty");
    return false;
  }
  
  // artifact_fw_version is optional (alternative to build_id)
  if (obj.containsKey("artifact_fw_version")) {
    if (extractStringField(obj, "artifact_fw_version", manifest.artifact_fw_version, sizeof(manifest.artifact_fw_version))) {
      manifest.artifact_fw_version_specified = true;
    } else {
      manifest.artifact_fw_version_specified = false;
      manifest.artifact_fw_version[0] = '\0';
    }
  } else {
    manifest.artifact_fw_version_specified = false;
    manifest.artifact_fw_version[0] = '\0';
  }

  // board is optional (sense/lcd)
  if (obj.containsKey("board")) {
    if (extractStringField(obj, "board", manifest.board, sizeof(manifest.board))) {
      manifest.board_specified = true;
    } else {
      manifest.board_specified = false;
      manifest.board[0] = '\0';
    }
  } else {
    manifest.board_specified = false;
    manifest.board[0] = '\0';
  }
  
  manifest.valid = true;
  outcome_.stage=ManifestStage::None;
  
  // Log with proper size indication and build identity
  char size_buf[32];
  if (manifest.size_specified) {
    snprintf(size_buf, sizeof(size_buf), "%lu", static_cast<unsigned long>(manifest.size));
  } else {
    snprintf(size_buf, sizeof(size_buf), "unknown");
  }
  
  // Log manifest identity (build_id is now required)
  LOG_INFO_TAG(LOG_TAG_MANIFEST,
               "Manifest parsed: version=%s, size=%s, sha256=%s, build_id=%s board=%s rollout_pct=%u rollout_seed=%lu min_allowed=%s",
               manifest.version, size_buf, manifest.sha256, manifest.build_id,
               manifest.board_specified ? manifest.board : "-",
               (unsigned)manifest.rollout_pct,
               (unsigned long)manifest.rollout_seed,
               manifest.min_version_allowed_specified ? manifest.min_version_allowed : "-");
  
  return true;
}

bool ManifestClient::fetchManifest(const char* url, OtaManifest& manifest, unsigned long timeout_ms) {
  outcome_=ManifestOutcome{};
  const uint32_t started=millis();
  struct Elapsed { ManifestOutcome& out; uint32_t start; ~Elapsed(){out.elapsed_ms=uint32_t(millis()-start);} } elapsed{outcome_,started};
  if (!url || strlen(url) == 0) {
    outcome_.stage=ManifestStage::InvalidUrl;
    LOG_ERROR_TAG(LOG_TAG_MANIFEST, "Invalid URL");
    return false;
  }
  
  manifest = OtaManifest();
  
  // Backoff (ms) before retries 1, 2, 3 when httpCode < 0 (transport/TLS error)
  static const unsigned long kBackoffMs[] = { 2000, 5000, 10000 };
  const int kMaxAttempts = 4;  // 1 initial + 3 retries
  
  LOG_INFO_TAG(LOG_TAG_MANIFEST, "Fetching manifest from: %s", url);
  LOG_INFO_TAG(LOG_TAG_MANIFEST, "[HEAP] free=%lu largest_block=%lu min_ever=%lu",
               ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
  
  for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
    if (attempt > 0) {
      unsigned long delay_ms = kBackoffMs[attempt - 1];
      LOG_WARN_TAG(LOG_TAG_MANIFEST, "Manifest fetch retry %d/%d (httpCode<0), waiting %lu ms",
                   attempt, kMaxAttempts - 1, delay_ms);
      delay(delay_ms);
    }
    
    outcome_.attempts=uint8_t(attempt+1);
    outcome_.transport=outcome_.tls_error=0;outcome_.http_status=0;outcome_.parser_code=0;
    HTTPClient http;
    WiFiClientSecure secure_client;
#if OTA_TLS_INSECURE_DEBUG
    secure_client.setInsecure();
#else
    secure_client.setCACert(kAmazonRootCa1);
#endif
    log_tls_context();
    char tls_err_buf[128];
    tls_err_buf[0] = '\0';
    secure_client.setHandshakeTimeout(15);
    secure_client.setTimeout(timeout_ms);

    // Resolve the host we are ACTUALLY fetching, not a hardcoded one.
    //
    // This used to probe "halo-ota-dev...", which on a production build meant the
    // sanity check passed against a host we never contact — and would have failed
    // a prod device whose DNS was fine but could not see the dev bucket.
    char dns_host[96] = {0};
    {
      const char* h = strstr(url, "://");
      if (h) {
        h += 3;
        const char* e = strchr(h, '/');
        size_t n = e ? (size_t)(e - h) : strlen(h);
        if (n >= sizeof(dns_host)) n = sizeof(dns_host) - 1;
        memcpy(dns_host, h, n);
        dns_host[n] = '\0';
      }
    }
    IPAddress dns_ip;
    bool dns_ok = dns_host[0] && WiFi.hostByName(dns_host, dns_ip);
    bool dns_ip_ok = dns_ok && (static_cast<uint32_t>(dns_ip) != 0);
    LOG_INFO_TAG(LOG_TAG_MANIFEST, "DNS sanity host=%s ok=%d ip=%s",
                 dns_host[0] ? dns_host : "(none)", dns_ip_ok ? 1 : 0, dns_ip.toString().c_str());
    if (!dns_ip_ok) {
      outcome_.stage=ManifestStage::DnsProbe;
      LOG_WARN_TAG(LOG_TAG_MANIFEST, "DNS sanity failed (ip=0.0.0.0) - skipping attempt");
      continue;
    }
    
    if (!http.begin(secure_client, url)) {
      int tls_err = secure_client.lastError(tls_err_buf, sizeof(tls_err_buf));
      outcome_.stage=ManifestStage::HttpBegin;outcome_.tls_error=tls_err;
      LOG_ERROR_TAG(LOG_TAG_MANIFEST, "HTTP begin failed for URL: %s", url);
      LOG_ERROR_TAG(LOG_TAG_MANIFEST, "TLS begin last_error=%d (%s)", tls_err, tls_err_buf);
      http.end();
      return false;
    }
    http.setConnectTimeout(15000);
    http.setReuse(false);
    const char* header_keys[] = {"ETag", "Last-Modified", "Cache-Control", "Age"};
    http.collectHeaders(header_keys, sizeof(header_keys) / sizeof(header_keys[0]));
    
    int httpCode = http.GET();
    // HTTPClient positive response code is the only source of http_status.
    if(httpCode>0 && httpCode<=32767)outcome_.http_status=int16_t(httpCode);
    
    if (httpCode < 0) {
      int tls_err = secure_client.lastError(tls_err_buf, sizeof(tls_err_buf));
      outcome_.stage=ManifestStage::Transport;outcome_.transport=httpCode;outcome_.tls_error=tls_err;
      LOG_WARN_TAG(LOG_TAG_MANIFEST, "TLS last_error=%d (%s)", tls_err, tls_err_buf);
      LOG_WARN_TAG(LOG_TAG_MANIFEST, "HTTP error: %s", http.errorToString(httpCode).c_str());
      LOG_WARN_TAG(LOG_TAG_MANIFEST, "Manifest attempt %d failed: httpCode=%d (transport/TLS)",
                   attempt + 1, httpCode);
      http.end();
      continue;
    }
    
    if (httpCode != HTTP_CODE_OK) {
      outcome_.stage=ManifestStage::HttpStatus;
      LOG_ERROR_TAG(LOG_TAG_MANIFEST, "HTTP GET failed with code: %d for URL: %s", httpCode, url);
      http.end();
      return false;
    }

    String etag = http.header("ETag");
    String last_modified = http.header("Last-Modified");
    String cache_control = http.header("Cache-Control");
    String age = http.header("Age");
    LOG_INFO_TAG(LOG_TAG_MANIFEST, "Manifest headers url=%s etag=%s last_modified=%s cache_control=%s age=%s",
                 url,
                 etag.length() ? etag.c_str() : "-",
                 last_modified.length() ? last_modified.c_str() : "-",
                 cache_control.length() ? cache_control.c_str() : "-",
                 age.length() ? age.c_str() : "-");
    
    if (secure_client.connected()) {
      http.setTimeout(timeout_ms);
    }
    
    String json = http.getString();
    http.end();
    
    if (json.length() == 0) {
      outcome_.stage=ManifestStage::EmptyBody;
      LOG_ERROR_TAG(LOG_TAG_MANIFEST, "Empty response body from URL: %s", url);
      return false;
    }
    
    LOG_DEBUG_TAG(LOG_TAG_MANIFEST, "Received JSON (%d bytes): %s", json.length(), json.c_str());
    
    bool parse_success = parseManifestJsonObserved(json, manifest);
    
    if (parse_success && manifest.valid) {
      char sha8[9];
      strncpy(sha8, manifest.sha256, 8);
      sha8[8] = '\0';
      char size_str[32];
      if (manifest.size_specified) {
        snprintf(size_str, sizeof(size_str), "%lu", static_cast<unsigned long>(manifest.size));
      } else {
        snprintf(size_str, sizeof(size_str), "unknown");
      }
      Serial.printf("[MANIFEST_ID] url=%s version=%s build_id=%s sha8=%s size=%s\n",
                    url, manifest.version, manifest.build_id, sha8, size_str);
    }
    return parse_success;
  }
  
  LOG_ERROR_TAG(LOG_TAG_MANIFEST, "Manifest fetch failed after %d attempts (httpCode<0)", kMaxAttempts);
  return false;
}

int ManifestClient::compareVersions(const char* v1, const char* v2) {
  if (!v1 || !v2) {
    return 0;  // Invalid versions
  }
  
  int major1 = 0, minor1 = 0, patch1 = 0;
  int major2 = 0, minor2 = 0, patch2 = 0;
  
  // Parse v1: "MAJOR.MINOR.PATCH"
  int parsed1 = sscanf(v1, "%d.%d.%d", &major1, &minor1, &patch1);
  // Parse v2: "MAJOR.MINOR.PATCH"
  int parsed2 = sscanf(v2, "%d.%d.%d", &major2, &minor2, &patch2);
  
  // If parsing failed, treat as invalid (return 0)
  if (parsed1 < 2 || parsed2 < 2) {
    return 0;
  }
  
  // Compare major, minor, patch
  if (major1 < major2) return -1;
  if (major1 > major2) return 1;
  if (minor1 < minor2) return -1;
  if (minor1 > minor2) return 1;
  if (patch1 < patch2) return -1;
  if (patch1 > patch2) return 1;
  return 0;
}

bool ManifestClient::isVersionCurrent(const OtaManifest& manifest, const char* current_version) {
  if (!manifest.valid || !current_version) {
    return false;
  }
  
  return compareVersions(manifest.version, current_version) == 0;
}

bool ManifestClient::isVersionNewer(const OtaManifest& manifest, const char* current_version) {
  if (!manifest.valid || !current_version) {
    return false;
  }
  
  return compareVersions(manifest.version, current_version) > 0;
}

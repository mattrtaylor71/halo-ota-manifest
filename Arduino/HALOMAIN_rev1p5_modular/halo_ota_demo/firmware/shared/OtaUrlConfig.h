#ifndef OTA_URL_CONFIG_H
#define OTA_URL_CONFIG_H

#include <stddef.h>
#include <string.h>
#include <strings.h>

struct OtaUrlConfig {
  char base_dir[256];
  char manifest_url[512];
  char channel[32];
  char env[8];
  char host[96];
  bool channel_enabled;
  bool allowed_host;
};

static inline void initOtaUrlConfig(OtaUrlConfig* config) {
  if (!config) {
    return;
  }
  memset(config, 0, sizeof(OtaUrlConfig));
}

static inline bool extractHostFromUrl(const char* url, char* dst, size_t dst_len) {
  if (!url || !dst || dst_len == 0) {
    return false;
  }
  const char* start = strstr(url, "://");
  if (!start) {
    return false;
  }
  start += 3;
  const char* end = strchr(start, '/');
  if (!end) {
    end = start + strlen(start);
  }
  size_t n = (size_t)(end - start);
  if (n >= dst_len) {
    n = dst_len - 1;
  }
  memcpy(dst, start, n);
  dst[n] = '\0';
  return n > 0;
}

static inline bool isAllowedOtaHost(const char* url) {
  static const char* const kOtaAllowedHosts[] = {
    "halo-ota-dev.s3.us-east-1.amazonaws.com",
    "halo-ota-prod.s3.us-east-1.amazonaws.com",
    nullptr
  };

  if (!url || strlen(url) == 0) {
    return false;
  }
  char host_buf[96];
  if (!extractHostFromUrl(url, host_buf, sizeof(host_buf))) {
    return false;
  }
  for (const char* const* p = kOtaAllowedHosts; *p; ++p) {
    if (strcasecmp(host_buf, *p) == 0) {
      return true;
    }
  }
  return false;
}

static inline bool containsDisallowedHost(const char* url) {
  if (!url || strlen(url) == 0) {
    return false;
  }
  const char* p = url;
  while (*p) {
    if ((p[0] == 'g' || p[0] == 'G') && (p[1] == 'i' || p[1] == 'I') &&
        (p[2] == 't' || p[2] == 'T') && (p[3] == 'h' || p[3] == 'H') &&
        (p[4] == 'u' || p[4] == 'U') && (p[5] == 'b' || p[5] == 'B') &&
        p[6] == '.' && (p[7] == 'i' || p[7] == 'I') &&
        (p[8] == 'o' || p[8] == 'O') &&
        (p[9] == '\0' || p[9] == '/' || p[9] == ':' || p[9] == '?' ||
         p[9] == '&' || p[9] == '#')) {
      return true;
    }
    if ((p[0] == 'g' || p[0] == 'G') && (p[1] == 'i' || p[1] == 'I') &&
        (p[2] == 't' || p[2] == 'T') && (p[3] == 'h' || p[3] == 'H') &&
        (p[4] == 'u' || p[4] == 'U') && (p[5] == 'b' || p[5] == 'B') &&
        (p[6] == 'u' || p[6] == 'U') && (p[7] == 's' || p[7] == 'S') &&
        (p[8] == 'e' || p[8] == 'E') && (p[9] == 'r' || p[9] == 'R') &&
        (p[10] == 'c' || p[10] == 'C') && (p[11] == 'o' || p[11] == 'O') &&
        (p[12] == 'n' || p[12] == 'N') && (p[13] == 't' || p[13] == 'T') &&
        (p[14] == 'e' || p[14] == 'E') && (p[15] == 'n' || p[15] == 'N') &&
        (p[16] == 't' || p[16] == 'T') && p[17] == '.' &&
        (p[18] == 'c' || p[18] == 'C') && (p[19] == 'o' || p[19] == 'O') &&
        (p[20] == 'm' || p[20] == 'M') &&
        (p[21] == '\0' || p[21] == '/' || p[21] == ':' || p[21] == '?' ||
         p[21] == '&' || p[21] == '#')) {
      return true;
    }
    ++p;
  }
  return false;
}

#endif  // OTA_URL_CONFIG_H

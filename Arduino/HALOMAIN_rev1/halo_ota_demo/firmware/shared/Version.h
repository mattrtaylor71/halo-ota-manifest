#ifndef VERSION_H
#define VERSION_H

#include <Arduino.h>

// Capture which file path the compiler used
#ifndef VERSION_H_PATH
#define VERSION_H_PATH __FILE__
#endif

// Firmware version string (semantic versioning: MAJOR.MINOR.PATCH)
#define FIRMWARE_VERSION "0.5.2"

// Build metadata (can be overridden at compile time)
#ifndef BUILD_DATE
#define BUILD_DATE __DATE__
#endif

#ifndef BUILD_TIME
#define BUILD_TIME __TIME__
#endif

#ifndef BUILD_GIT_HASH
#define BUILD_GIT_HASH "unknown"
#endif

// Build ID: unique identifier for this build (version + date + time + git hash)
// Format: "VERSION-DATE-TIME-GIT" (e.g., "0.5.1-Jan 22 2026-14:30:15-abc123")
#ifndef BUILD_ID
#define BUILD_ID FIRMWARE_VERSION "-" BUILD_DATE "-" BUILD_TIME "-" BUILD_GIT_HASH
#endif

// Version comparison helper
struct Version {
  int major;
  int minor;
  int patch;
  
  Version() : major(0), minor(0), patch(0) {}
  Version(const char* version_str) {
    parse(version_str);
  }
  
  void parse(const char* version_str) {
    major = minor = patch = 0;
    if (version_str) {
      sscanf(version_str, "%d.%d.%d", &major, &minor, &patch);
    }
  }
  
  bool operator==(const Version& other) const {
    return major == other.major && minor == other.minor && patch == other.patch;
  }
  
  bool operator<(const Version& other) const {
    if (major < other.major) return true;
    if (major > other.major) return false;
    if (minor < other.minor) return true;
    if (minor > other.minor) return false;
    return patch < other.patch;
  }
  
  bool operator>(const Version& other) const {
    return other < *this;
  }
  
  String toString() const {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d.%d.%d", major, minor, patch);
    return String(buf);
  }
};

// Build info structure
struct BuildInfo {
  const char* version;
  const char* build_date;
  const char* build_time;
  const char* git_hash;
  const char* build_id;
  
  BuildInfo() 
    : version(FIRMWARE_VERSION)
    , build_date(BUILD_DATE)
    , build_time(BUILD_TIME)
    , git_hash(BUILD_GIT_HASH)
    , build_id(BUILD_ID) {}
};

// Embed an unmistakable marker in the binary for release verification via `strings`.
#ifndef FW_EMBED_MARKER
#define FW_EMBED_MARKER "HALO_FW_MARKER:" FIRMWARE_VERSION "|BUILD_ID:" BUILD_ID
#endif

// Force retention in binary (prevent LTO/GC from stripping it)
static const char g_fw_embed_marker[] __attribute__((used)) = FW_EMBED_MARKER;

static inline const char* getFwEmbedMarker() {
  return g_fw_embed_marker;
}

#endif // VERSION_H

#ifndef VERSION_H
#define VERSION_H

// Firmware version string (semantic versioning: MAJOR.MINOR.PATCH)
#define FIRMWARE_VERSION "1.0.13"

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

#endif // VERSION_H




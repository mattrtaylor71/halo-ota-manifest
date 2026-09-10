// BuildInfo.h - Single source of truth for BUILD_ID and FIRMWARE_VERSION
// 
// PURPOSE: Ensure BUILD_ID consistency across all compilation units (.ino, .cpp)
//
// This header provides extern declarations for variables defined in BuildInfo.cpp.
// All runtime code should use kFirmwareVersion and kBuildId instead of macros.
//
// USAGE:
//   #include "BuildInfo.h"
//   Serial.printf("Build ID: %s\n", kBuildId);
//
#ifndef BUILD_INFO_H
#define BUILD_INFO_H

// Extern declarations - actual definitions are in BuildInfo.cpp
extern const char* kFirmwareVersion;
extern const char* kBuildId;
extern const char* kFwEmbedMarker;

#endif // BUILD_INFO_H

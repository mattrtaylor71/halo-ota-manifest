// BuildInfo.cpp - Single source of truth for BUILD_ID and FIRMWARE_VERSION
// 
// This file contains the ONLY definitions of kFirmwareVersion, kBuildId, and kFwEmbedMarker.
// All other files should include BuildInfo.h and use the extern declarations.
//
// IMPORTANT: This is the ONLY .cpp file that should define these variables.
// All runtime code should use kFirmwareVersion and kBuildId instead of FIRMWARE_VERSION/BUILD_ID macros.

#include "BuildInfo.h"
#include "Version.h"

// Single source of truth: Actual variable definitions
// These are computed at compile time from Version.h macros
// The macros are still used here for initialization, but all runtime code uses these variables
const char* kFirmwareVersion = FIRMWARE_VERSION;
const char* kBuildId = BUILD_ID;

// Embed marker string (for release verification via `strings`)
// Format: "HALO_FW_MARKER:<version>|BUILD_ID:<build_id>"
const char* kFwEmbedMarker = FW_EMBED_MARKER;

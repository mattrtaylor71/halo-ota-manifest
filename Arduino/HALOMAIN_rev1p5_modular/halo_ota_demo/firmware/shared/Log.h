#ifndef LOG_H
#define LOG_H

#include <Arduino.h>

// Log verbosity levels
#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

// Default log level (can be overridden at compile time)
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

// Log tags
#define LOG_TAG_MAIN     "MAIN"
#define LOG_TAG_UART     "UART"
#define LOG_TAG_HEALTH   "HEALTH"
#define LOG_TAG_OTA      "OTA"
#define LOG_TAG_WIFI     "WIFI"

// Internal macro to check log level
#define _LOG_ENABLED(level) ((level) <= LOG_LEVEL)

// Tagged logging macros with verbosity control
#define LOG_ERROR_TAG(tag, fmt, ...) \
  do { \
    if (_LOG_ENABLED(LOG_LEVEL_ERROR)) { \
      Serial.printf("[T=%lu][%s][ERROR] " fmt "\n", (unsigned long)millis(), tag, ##__VA_ARGS__); \
    } \
  } while(0)

#define LOG_WARN_TAG(tag, fmt, ...) \
  do { \
    if (_LOG_ENABLED(LOG_LEVEL_WARN)) { \
      Serial.printf("[T=%lu][%s][WARN] " fmt "\n", (unsigned long)millis(), tag, ##__VA_ARGS__); \
    } \
  } while(0)

#define LOG_INFO_TAG(tag, fmt, ...) \
  do { \
    if (_LOG_ENABLED(LOG_LEVEL_INFO)) { \
      Serial.printf("[T=%lu][%s][INFO] " fmt "\n", (unsigned long)millis(), tag, ##__VA_ARGS__); \
    } \
  } while(0)

#define LOG_DEBUG_TAG(tag, fmt, ...) \
  do { \
    if (_LOG_ENABLED(LOG_LEVEL_DEBUG)) { \
      Serial.printf("[T=%lu][%s][DEBUG] " fmt "\n", (unsigned long)millis(), tag, ##__VA_ARGS__); \
    } \
  } while(0)

// Convenience macros without tags (use MAIN tag)
#define LOG_ERROR(fmt, ...) LOG_ERROR_TAG(LOG_TAG_MAIN, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG_WARN_TAG(LOG_TAG_MAIN, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG_INFO_TAG(LOG_TAG_MAIN, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_DEBUG_TAG(LOG_TAG_MAIN, fmt, ##__VA_ARGS__)

#endif // LOG_H

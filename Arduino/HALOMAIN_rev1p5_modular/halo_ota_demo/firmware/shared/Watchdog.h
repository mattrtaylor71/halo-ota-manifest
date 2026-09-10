#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <Arduino.h>

class Watchdog {
public:
  // Initialize watchdog (seconds)
  static void begin(uint32_t timeout_s);

  // Register a task by name (string literal preferred)
  static void registerTask(const char* name);

  // Kick task watchdog (updates last-seen timestamp)
  static void kick(const char* name);

  // Mark task as critical (critical tasks trigger reboot on missed windows)
  static void markCritical(const char* name, bool critical = true);

  // Periodic tick from loop()
  static void tick();
};

#endif // WATCHDOG_H

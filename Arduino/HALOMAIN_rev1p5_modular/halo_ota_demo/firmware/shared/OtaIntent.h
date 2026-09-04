#ifndef HALO_OTA_INTENT_H
#define HALO_OTA_INTENT_H

#include <Arduino.h>

namespace OtaIntent {
  void init();

  void updateDesired(const char* sense_ver,
                     const char* lcd_ver,
                     bool force,
                     bool allow_downgrade,
                     uint32_t ts,
                     const char* reason);

  // Clear the desired OTA intent
  void clearDesired();

  // True if OTA should run now (respects cooldown unless force)
  bool shouldUpdateNow();
  bool cooldownAllows();

  // Track cooldown in RAM for this boot; persist only the result string.
  void recordOtaAttempt(const char* result);
  void recordOtaResult(const char* result);
  void markNoUpdateNeeded();
  void clearForceAndCheck();

  // Desired state getters (for TRUTH)
  const char* getDesiredSense();
  const char* getDesiredLcd();
  bool getDesiredForce();
  bool getAllowDowngrade();
  int32_t getDesiredAgeS();
  bool getOtaIntentActive();
}

#endif  // HALO_OTA_INTENT_H

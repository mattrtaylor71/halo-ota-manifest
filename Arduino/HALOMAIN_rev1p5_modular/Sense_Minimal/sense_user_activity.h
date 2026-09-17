#ifndef SENSE_USER_ACTIVITY_H
#define SENSE_USER_ACTIVITY_H

#include <atomic>
#include <stdint.h>
#include <string.h>

// Only a fresh, validated user action may interrupt pre-sleep upload cleanup.
// Routine firmware queries and link traffic must not renew an awake period.
static bool sense_user_action_cancels_flush(const char* type) {
  if (!type) return false;
  static const char* const actions[] = {
    "INPUT_WAKE", "INPUT_USER_ACTIVE", "INPUT_TOUCH", "INPUT_MENU_PRESS", "INPUT_MENU_SELECT",
    "INPUT_SCROLL", "INPUT_DELETE", "INPUT_EXPIRY_DATE",
    "INPUT_DISCARD_OPTIONS", "INPUT_LONG_PRESS_START", "INPUT_LONG_PRESS_END",
    "INPUT_RETRY", "INPUT_RESET_WIFI", "INPUT_OTA_CHECK",
    "INPUT_WIFI_SCAN", "INPUT_WIFI_TEST"
  };
  for (const char* action : actions) {
    if (strcmp(type, action) == 0) return true;
  }
  return false;
}

static std::atomic<uint32_t> g_sense_user_action_generation{0};

static uint32_t sense_user_action_generation() {
  return g_sense_user_action_generation.load(std::memory_order_relaxed);
}

static void sense_note_admitted_user_action() {
  g_sense_user_action_generation.fetch_add(1, std::memory_order_relaxed);
}

#endif

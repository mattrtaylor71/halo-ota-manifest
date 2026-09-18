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
static std::atomic<uint32_t> g_sense_user_interrupt_generation{0};

static uint32_t sense_user_action_generation() {
  return g_sense_user_action_generation.load(std::memory_order_relaxed);
}

static uint32_t sense_user_interrupt_generation() {
  return g_sense_user_interrupt_generation.load(std::memory_order_relaxed);
}

// List browsing still renews activity/cancels sleep, but need not restart the
// fresh voice upload whose result the user is waiting to see in that list.
static bool sense_voice_list_input_compatible(const char* type, bool list_active) {
  return list_active && type && (!strcmp(type, "INPUT_SCROLL") ||
      !strcmp(type, "INPUT_WAKE") || !strcmp(type, "INPUT_TOUCH") ||
      // LCD encoder activity is sent using this common wire notification.
      !strcmp(type, "INPUT_USER_ACTIVE"));
}

static void sense_note_admitted_user_action(bool list_compatible = false) {
  g_sense_user_action_generation.fetch_add(1, std::memory_order_relaxed);
  if (!list_compatible)
    g_sense_user_interrupt_generation.fetch_add(1, std::memory_order_relaxed);
}

#endif

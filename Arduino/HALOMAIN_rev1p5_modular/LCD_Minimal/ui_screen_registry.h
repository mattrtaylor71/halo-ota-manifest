// ui_screen_registry.h
#ifndef UI_SCREEN_REGISTRY_H
#define UI_SCREEN_REGISTRY_H

#include <Arduino.h>

typedef enum {
  SCREEN_UNKNOWN = 0,
  SCREEN_SHIP_MAIN_MENU,
  SCREEN_SHIP_SECOND_MENU,
  SCREEN_SHIP_SETTINGS,
  SCREEN_SHIP_BACKLIGHT,
  SCREEN_SHIP_HOLD_STILL,
  SCREEN_SHIP_VOICE_ACK,
  SCREEN_SHIP_PROCESSING,
  SCREEN_SHIP_LOGGED,
  SCREEN_SHIP_ERROR,
  SCREEN_SHIP_EXPIRY_CHOICE,
  SCREEN_SHIP_EXPIRY,
  SCREEN_SHIP_RESULT,
  SCREEN_SHIP_DEBUG,
  SCREEN_SHIP_ERRLOG,
  SCREEN_SHIP_SHOPPING_LIST,
  SCREEN_COUNT
} ScreenId;

typedef struct {
  ScreenId id;
  const char* name;
  const char* bitmap;
  const char* file;
  int line;
} ScreenMeta;

static const ScreenMeta kScreenRegistry[] = {
  { SCREEN_SHIP_MAIN_MENU,   "SHIP_MAIN_MENU",   "ui_img_Frame_443_png",   __FILE__, __LINE__ },
  { SCREEN_SHIP_SECOND_MENU, "SHIP_SECOND_MENU", "ui_img_Frame_443_1_png", __FILE__, __LINE__ },
  { SCREEN_SHIP_SETTINGS,    "SHIP_SETTINGS",    "ui_img_Frame_443_1_png", __FILE__, __LINE__ },
  { SCREEN_SHIP_BACKLIGHT,   "SHIP_BACKLIGHT",   "(none)",                 __FILE__, __LINE__ },
  { SCREEN_SHIP_HOLD_STILL,  "SHIP_HOLD_STILL",  "ui_img_SCREEN_HOLDSTILL_png",   __FILE__, __LINE__ },
  { SCREEN_SHIP_VOICE_ACK,   "SHIP_VOICE_ACK",   "(none)",                __FILE__, __LINE__ },
  { SCREEN_SHIP_PROCESSING,  "SHIP_PROCESSING",  "ui_img_SCREEN_PROCESSING_png", __FILE__, __LINE__ },
  { SCREEN_SHIP_LOGGED,      "SHIP_LOGGED",      "ui_img_SCREEN_LOGGED_png",     __FILE__, __LINE__ },
  { SCREEN_SHIP_ERROR,       "SHIP_ERROR",       "ui_img_SCREEN_ERROR_png",      __FILE__, __LINE__ },
  { SCREEN_SHIP_EXPIRY_CHOICE, "SHIP_EXPIRY_CHOICE", "ui_img_Frame_495__1__png",  __FILE__, __LINE__ },
  { SCREEN_SHIP_EXPIRY,      "SHIP_EXPIRY",      "(none)",                __FILE__, __LINE__ },
  { SCREEN_SHIP_RESULT,      "SHIP_RESULT",      "(none)",                __FILE__, __LINE__ },
  { SCREEN_SHIP_DEBUG,       "SHIP_DEBUG",       "(none)",                __FILE__, __LINE__ },
  { SCREEN_SHIP_ERRLOG,      "SHIP_ERRLOG",      "(none)",                __FILE__, __LINE__ },
  { SCREEN_SHIP_SHOPPING_LIST, "SHIP_SHOPPING_LIST", "(none)",             __FILE__, __LINE__ }
};

static inline const ScreenMeta* screen_meta(ScreenId id) {
  for (size_t i = 0; i < (sizeof(kScreenRegistry) / sizeof(kScreenRegistry[0])); ++i) {
    if (kScreenRegistry[i].id == id) {
      return &kScreenRegistry[i];
    }
  }
  return NULL;
}

static inline void dump_screen_registry() {
  Serial.println("[SCREEN_REGISTRY]");
  for (size_t i = 0; i < (sizeof(kScreenRegistry) / sizeof(kScreenRegistry[0])); ++i) {
    const ScreenMeta* meta = &kScreenRegistry[i];
    Serial.printf("- %s bitmap=%s defined_at=%s:%d\n",
                  meta->name ? meta->name : "(unknown)",
                  meta->bitmap ? meta->bitmap : "(none)",
                  meta->file ? meta->file : "(unknown)",
                  meta->line);
  }
}

#endif  // UI_SCREEN_REGISTRY_H

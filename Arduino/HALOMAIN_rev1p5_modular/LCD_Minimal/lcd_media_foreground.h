#pragma once

// One short lock arbitrates UI admission against typed SD transport admission.
// No UART, queue, storage, clock or UI calls occur while this lock is held.
enum LcdMediaMode : uint8_t { LCD_MEDIA_IDLE, LCD_MEDIA_SAVE, LCD_MEDIA_REPLAY };
static portMUX_TYPE g_lcd_media_mux = portMUX_INITIALIZER_UNLOCKED;
static LcdMediaMode g_lcd_media_mode = LCD_MEDIA_IDLE;
static uint32_t g_lcd_media_queued_intents = 0;
static bool g_lcd_media_voice_gesture = false;
static bool g_lcd_media_cancel_replay = false;

static bool lcd_media_is_intent(const char* type) {
  return type && (!strcmp(type,"INPUT_MENU_SELECT") ||
    !strcmp(type,"INPUT_LONG_PRESS_START") || !strcmp(type,"INPUT_LONG_PRESS_END") ||
    !strcmp(type,"INPUT_DELETE") || !strcmp(type,"INPUT_RETRY") ||
    !strcmp(type,"INPUT_OTA_CHECK") || !strcmp(type,"INPUT_RESET_WIFI"));
}

static bool lcd_media_queue_begin(const char* type) {
  if(!lcd_media_is_intent(type))return true;
  portENTER_CRITICAL(&g_lcd_media_mux);
  // A new recording cannot be represented by delayed START/END edges. SAVE
  // also keeps its current RAM capture's custody rather than yielding it.
  const bool voice=!strcmp(type,"INPUT_LONG_PRESS_START") || !strcmp(type,"INPUT_LONG_PRESS_END");
  const bool allowed=(!voice || g_lcd_media_mode==LCD_MEDIA_IDLE) &&
    (strcmp(type,"INPUT_MENU_SELECT") || g_lcd_media_mode!=LCD_MEDIA_SAVE);
  if(g_lcd_media_mode==LCD_MEDIA_REPLAY)g_lcd_media_cancel_replay=true;
  if(allowed)++g_lcd_media_queued_intents;
  portEXIT_CRITICAL(&g_lcd_media_mux);
  return allowed;
}

static void lcd_media_queue_end(const char* type) {
  if(!lcd_media_is_intent(type))return;
  portENTER_CRITICAL(&g_lcd_media_mux);
  if(g_lcd_media_queued_intents)--g_lcd_media_queued_intents;
  portEXIT_CRITICAL(&g_lcd_media_mux);
}

static bool lcd_media_try_claim(bool replay, bool deferred_intent) {
  portENTER_CRITICAL(&g_lcd_media_mux);
  const bool ok=g_lcd_media_mode==LCD_MEDIA_IDLE && !g_lcd_media_voice_gesture &&
    !g_lcd_media_queued_intents && !deferred_intent;
  if(ok){g_lcd_media_mode=replay?LCD_MEDIA_REPLAY:LCD_MEDIA_SAVE;g_lcd_media_cancel_replay=false;}
  portEXIT_CRITICAL(&g_lcd_media_mux);
  return ok;
}

static void lcd_media_release() {
  portENTER_CRITICAL(&g_lcd_media_mux);
  g_lcd_media_mode=LCD_MEDIA_IDLE;g_lcd_media_cancel_replay=false;
  portEXIT_CRITICAL(&g_lcd_media_mux);
}

static bool lcd_media_replay_cancelled() {
  portENTER_CRITICAL(&g_lcd_media_mux);
  const bool cancel=g_lcd_media_mode==LCD_MEDIA_REPLAY && g_lcd_media_cancel_replay;
  portEXIT_CRITICAL(&g_lcd_media_mux);
  return cancel;
}

static bool lcd_media_voice_begin() {
  portENTER_CRITICAL(&g_lcd_media_mux);
  const bool ok=g_lcd_media_mode==LCD_MEDIA_IDLE && !g_lcd_media_voice_gesture;
  if(g_lcd_media_mode==LCD_MEDIA_REPLAY)g_lcd_media_cancel_replay=true;
  if(ok)g_lcd_media_voice_gesture=true;
  portEXIT_CRITICAL(&g_lcd_media_mux);
  return ok;
}

static void lcd_media_voice_end() {
  portENTER_CRITICAL(&g_lcd_media_mux);
  g_lcd_media_voice_gesture=false;
  portEXIT_CRITICAL(&g_lcd_media_mux);
}

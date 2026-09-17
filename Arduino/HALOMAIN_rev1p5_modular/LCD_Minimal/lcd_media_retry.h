#pragma once

#include "../halo_common/MediaRetryTimer.h"

// Only a wake hint lives here. SD custody and retry policy remain independent.
RTC_DATA_ATTR static halo_media_timer::State g_lcd_media_retry_timer;
static uint64_t g_lcd_media_retry_checkpoint_us = 0;
static portMUX_TYPE g_lcd_media_retry_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_lcd_media_retry_boot = false;
static std::atomic<uint32_t> g_lcd_media_retry_wait_until_ms{0};
static constexpr uint32_t LCD_MEDIA_RETRY_WAIT_MS = 120000;

class LcdMediaRetryGuard {
 public:
  LcdMediaRetryGuard() { portENTER_CRITICAL(&g_lcd_media_retry_mux); }
  ~LcdMediaRetryGuard() { portEXIT_CRITICAL(&g_lcd_media_retry_mux); }
};

static bool lcd_media_retry_arm(const char* token, uint32_t seconds) {
  const uint64_t now_us = uint64_t(esp_timer_get_time());
  LcdMediaRetryGuard guard;
  return halo_media_timer::arm(g_lcd_media_retry_timer, g_lcd_media_retry_checkpoint_us,
                               now_us, token, seconds);
}

static void lcd_media_retry_note_boot(bool deep_sleep_reset, bool timer_wake) {
  const uint64_t now_us = uint64_t(esp_timer_get_time());
  const uint32_t now_ms = millis();
  {
    LcdMediaRetryGuard guard;
    g_lcd_media_retry_boot = halo_media_timer::boot(g_lcd_media_retry_timer,
        g_lcd_media_retry_checkpoint_us, now_us, deep_sleep_reset, timer_wake);
  }
  if (g_lcd_media_retry_boot) {
    g_lcd_media_retry_wait_until_ms.store(now_ms + LCD_MEDIA_RETRY_WAIT_MS);
    Serial.printf("[MEDIA_RETRY] wake purpose=media wait_ms=%lu\n",
                  (unsigned long)LCD_MEDIA_RETRY_WAIT_MS);
  }
}

static bool lcd_media_retry_wait_active() {
  const uint32_t until = g_lcd_media_retry_wait_until_ms.load();
  return until && int32_t(until - uint32_t(millis())) > 0;
}

static void lcd_media_retry_wait_release(const char* reason) {
  if (g_lcd_media_retry_wait_until_ms.exchange(0))
    Serial.printf("[MEDIA_RETRY] receiver_wait_end reason=%s\n", reason ? reason : "done");
}

static uint32_t lcd_media_retry_choose_timer(uint32_t existing_s, bool* media_selected) {
  const uint64_t now_us = uint64_t(esp_timer_get_time());
  LcdMediaRetryGuard guard;
  return halo_media_timer::choose(g_lcd_media_retry_timer, g_lcd_media_retry_checkpoint_us,
                                  now_us, existing_s, media_selected);
}

static void lcd_media_retry_commit_sleep(uint32_t seconds, bool media_selected, bool timer_ok) {
  const uint64_t now_us = uint64_t(esp_timer_get_time());
  LcdMediaRetryGuard guard;
  halo_media_timer::commit_sleep(g_lcd_media_retry_timer, g_lcd_media_retry_checkpoint_us,
                                 now_us, seconds, media_selected, timer_ok);
}

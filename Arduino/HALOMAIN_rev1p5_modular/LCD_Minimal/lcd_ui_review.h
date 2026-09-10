// Optional inspection of the real, currently displayed UI. No screen routing,
// peer messages, test data, timers, input simulation or automatic execution.
#pragma once

#if defined(HALO_UI_REVIEW) && HALO_UI_REVIEW
#include "lcd_bsp.h"
#include "lcd_theme.h"

// Call only from the UI event handler while it owns the LVGL lock.
// The reported duration includes tree inspection and USB printing; it is NOT
// frame time or FPS. BSP flush counters count submissions, not whole frames.
static inline void lcd_ui_review_dump(const char* reason) {
  if (!lvgl_lock_held_by_current_task()) {
    Serial.println("[UI_REVIEW] rejected=lvgl_lock_not_owned");
    return;
  }
  lv_obj_t* screen = lv_scr_act();
  if (screen == NULL) {
    Serial.println("[UI_REVIEW] rejected=no_active_screen");
    return;
  }

  const int64_t started_us = esp_timer_get_time();
  const char* tag = reason ? reason : "current";
  lv_obj_update_layout(screen);

  lv_mem_monitor_t memory = {};
  lv_mem_monitor(&memory);
  uint32_t submit_ok = 0;
  uint32_t submit_fail = 0;
  int outstanding = 0;
  int soft_fault = 0;
  lcd_bsp_get_flush_submit_stats(&submit_ok, &submit_fail,
                               &outstanding, &soft_fault);

  Serial.printf("[UI_REVIEW_BEGIN] reason=%.32s screen=%p\n", tag, (void*)screen);
  Serial.printf("[UI_REVIEW_MEM] total=%lu free=%lu largest=%lu used_pct=%u frag_pct=%u\n",
                (unsigned long)memory.total_size,
                (unsigned long)memory.free_size,
                (unsigned long)memory.free_biggest_size,
                (unsigned)memory.used_pct, (unsigned)memory.frag_pct);
  Serial.printf("[UI_REVIEW_FLUSH] submitted_ok=%lu submitted_fail=%lu outstanding=%d soft_fault=%d inflight_age_ms=%lu fail_count=%lu\n",
                (unsigned long)submit_ok, (unsigned long)submit_fail,
                outstanding, soft_fault, lcd_bsp_flush_inflight_age_ms(),
                (unsigned long)lcd_bsp_get_flush_fail_count());
#if LAYOUT_AUDIT
  // Keep the existing layout_audit.py record format and actual object metrics.
  Serial.println("[LAYSTART] UI_REVIEW");
  audit_dump_tree(screen, "UI_REVIEW", 0);
  Serial.println("[LAYEND] UI_REVIEW");
#else
  Serial.println("[UI_REVIEW] layout_dump=disabled");
#endif
  Serial.printf("[UI_REVIEW_END] inspection_us=%llu\n",
                (unsigned long long)(esp_timer_get_time() - started_us));
}
#else
static inline void lcd_ui_review_dump(const char* reason) { (void)reason; }
#endif

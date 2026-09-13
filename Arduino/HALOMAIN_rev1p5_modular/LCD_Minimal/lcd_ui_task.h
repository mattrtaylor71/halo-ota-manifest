/*
 * lcd_ui_task.h
 *
 * FreeRTOS UI task: processes app_event_queue events under the LVGL
 * lock, dispatching to screen transitions, animations, and state
 * updates.  Runs on the default core with priority 1.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 3.
 *
 * Prerequisites (must be declared before #include "lcd_ui_task.h"):
 *   - All LVGL UI functions, screen builders, animation helpers,
 *     event handlers, and state management functions
 *   - app_event_queue, app_event_t, example_lvgl_lock/unlock
 */

#ifndef LCD_UI_TASK_H
#define LCD_UI_TASK_H

// USB 'deltouch' deferred tap: after the overlay opens, the synthesized tap at
// the center of the real Delete button runs on a LATER UI-task iteration
// (~300ms) so the overlay is fully laid out/rendered first — same two-step
// cadence as a finger. 0 = no tap pending.
static unsigned long usb_deltouch_tap_due_ms = 0;

static void ui_task(void *arg) {
  Serial.println("[UI] UI task started");

  // The other half of the freeze protection. A wedged UI task leaves the panel
  // frozen and the user with no feedback at all; the observed wedges killed
  // serial output entirely, so both tasks are watched rather than guessing
  // which one dies.
  lcd_freeze_wdt_subscribe("ui_task");

  for (;;) {
    lcd_freeze_wdt_feed();
#if HALO_FREEZE_TEST
    // Bench: hang here, past the feed, so the watchdog is the only thing that
    // can recover the board.
    if (g_bench_freeze_ui) {
      Serial.println("[FREEZE_TEST] ui_task hanging now — TWDT should reset the board");
      for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    // Check for clean shutdown request BEFORE acquiring the LVGL lock.
    // This prevents heap corruption from vTaskDelete while holding the lock.
    if (g_ui_task_exit_requested) {
      Serial.println("[UI] exit requested — stopping cleanly");
      ui_task_handle = NULL;
      vTaskDelete(NULL);  // self-delete; does not return
    }

    if (!example_lvgl_lock(50)) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // USB 'ota' and the physical Settings button use exactly the same action,
    // including its latch and awake-proof delivery, on the LVGL owner task.
    if (g_manual_ota_ui_requested) {
      g_manual_ota_ui_requested = false;
      ship_menu_send_manual_ota("usb_settings_action");
    }
    if (g_manual_ota_ui_active && !g_manual_ota_result && !ota_locked &&
        !g_lcd_ota_uart_receiving && g_manual_ota_ui_deadline_ms &&
        (int32_t)(millis() - g_manual_ota_ui_deadline_ms) >= 0) {
      lcd_manual_ota_finish("request_failed");
    }
    if (g_manual_ota_ui_active && g_manual_ota_result && !ota_locked &&
        !g_lcd_ota_uart_receiving &&
        (int32_t)(millis() - g_manual_ota_result_until_ms) >= 0) {
      g_manual_ota_ui_active = false;
      g_manual_ota_result = 0;
      g_manual_ota_result_until_ms = 0;
      g_ota_screen_active = false;
      ota_stay_awake_until_ms = 0;
      provision_return_home_pending = true;
    }

    // ── Cold-boot repaint (one-shot) ──────────────────────────────────────
    // On a cold boot the device came up on a BLACK screen and stayed there.
    // Cause: setup() calls show_ship_main_menu() (lcd_activity.h ~519) before
    // this task exists (created ~527) and before g_lvgl_running is set (~541).
    // That first frame does reach the panel — flush_ok settles at exactly 10,
    // which is one full 360px screen at a 36-line buffer — but comes up blank.
    // HOME is static, so LVGL never invalidates it again and the blank frame is
    // latched forever. (The flush_cb also drops frames outright while
    // g_sleep_transition is set, yet still calls lv_disp_flush_ready(), so LVGL
    // marks those areas clean — same trap, different route.)
    // Measured: a forced redraw takes flush_ok 10 -> 20 and the UI appears.
    // Repaint once here, under the LVGL lock, after a short settle so the task,
    // panel and backlight are all definitely up.
    // A SINGLE repaint at a fixed delay is not enough: it fired, flush_ok reached
    // 20 (two full frames), and the panel was still black — the frame is composed
    // but does not land. Two known ways that happens, both timing-dependent:
    // the panel isn't actually up yet, and example_lvgl_flush_cb() DISCARDS the
    // frame outright while g_sleep_transition is set yet still calls
    // lv_disp_flush_ready(), so LVGL marks it clean and never retries.
    // So repaint repeatedly over the first few seconds, only while the panel is
    // genuinely enabled and no sleep transition is in flight. Cost is a handful
    // of full repaints at boot and nothing thereafter.
    static uint8_t s_boot_repaints_left = 8;
    static unsigned long s_boot_repaint_next_ms = 0;
    if (s_boot_repaints_left && g_ui_initialized) {
      unsigned long now_ms = millis();
      if (s_boot_repaint_next_ms == 0) {
        s_boot_repaint_next_ms = now_ms + 300;
      } else if ((long)(now_ms - s_boot_repaint_next_ms) >= 0) {
        if (g_panel_enabled && g_lvgl_running && !g_sleep_transition) {
          lv_obj_t* scr = lv_scr_act();
          if (scr) lv_obj_invalidate(scr);
          s_boot_repaints_left--;
          if (s_boot_repaints_left == 0) {
            Serial.println("[UI] boot_repaint sequence complete");
          }
        }
        // Not ready (panel off / mid sleep transition): retry, don't burn a slot.
        s_boot_repaint_next_ms = now_ms + 500;
      }
    }
    // OTA overlay state — hoisted out of the if (g_ota_screen_active) block so
    // the teardown below (reached only when OTA is no longer active) can see and
    // delete the overlay. These remain static so they persist across loop
    // iterations and across the active/inactive transition.
    static lv_obj_t* ota_overlay = NULL;
    static lv_obj_t* ota_label = NULL;
    static lv_obj_t* ota_percent = NULL;
    static lv_obj_t* ota_progress = NULL;
    static lv_obj_t* ota_description = NULL;
    static lv_obj_t* ota_activity = NULL;
    static lv_obj_t* ota_spinner = NULL;
    static int last_shown_pct = -1;
    static int last_ota_phase = -1;
    static bool ota_saw_transfer = false;

    // While OTA screen is active, don't process any screen transitions.
    // Just tick LVGL to keep the display alive and release the lock.
    if (g_ota_screen_active) {
      lcd_allow_visible_ui("ota_screen");
      if (g_idle_screen_dark) lcd_set_idle_screen_dark(false, "ota_screen");
      // OTA_LOCK can arrive while setup is still finishing the dark UI init.
      // Repair the final panel state here on its owner task as well as the PWM.
      if (!g_panel_enabled && g_lcd_initialized) {
        lcd_panel_set_power(true);
        g_panel_enabled = true;
      }
      g_lvgl_running = true;
      // Non-user (timer/maintenance) wakes no longer relight the panel (see the
      // boot policy in setup()). Guarantee a real OTA is still visible:
      // whenever the "Updating..." overlay is active but the backlight was left
      // dark by a non-user wake, relight it here. A bare handshake-retry/fallback
      // wake never sets g_ota_screen_active, so it stays dark.
      if (g_backlight_duty == 0) {
        lcd_set_backlight_binary(true, "ota_screen");
      }
      // Dark designer system surface. Only the display byte counter is a
      // percentage; unknown/preparation and continuation remain indeterminate.
      bool is_transfer = g_lcd_ota_show_progress && g_lcd_ota_progress_pct >= 0;
      bool need_update = false;
      if (is_transfer)
        ota_saw_transfer = true;
      const bool finishing = ota_saw_transfer || g_ota_continuation_hold_start_ms != 0;
      const uint8_t manual_result = g_manual_ota_ui_active ? g_manual_ota_result.load() : 0;
      const int phase = manual_result ? 2 + manual_result : (is_transfer ? 1 : (finishing ? 2 : 0));

      if (!ota_overlay) {
        // Covered presentation must not keep animating behind this overlay.
        halo_ui_motion_stop(lv_scr_act());
        if (provision_ui_spinner) {
          provision_ui_deferred_view = (int)provision_ui_view;
          provision_ui_stop_spinner();
        }
        // Create overlay on first entry
        ota_overlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(ota_overlay);
        lv_obj_set_size(ota_overlay, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(COL_DARK), 0);
        lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_COVER, 0);
        lv_obj_center(ota_overlay);
        lv_obj_clear_flag(ota_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* eyebrow =
            halo_ui_label(ota_overlay, "SOFTWARE UPDATE", &nunito_12, COL_GOLD, 60, 56, 240);
        lv_obj_set_style_text_letter_space(eyebrow, 1, 0);
        ota_label =
            halo_ui_label(ota_overlay, "Starting update", &nunito_22, COL_WHITE, 40, 196, 280);
        ota_percent = halo_ui_label(ota_overlay, "0%", &nunito_44, COL_WHITE, 80, 126, 200);
        ota_description = halo_ui_label(ota_overlay, "Getting things ready...",
                                        &lv_font_montserrat_14, 0xD1D1D1, 50, 236, 260);
        lv_obj_set_style_text_line_space(ota_description, 3, 0);
        ota_progress = lv_bar_create(ota_overlay);
        lv_obj_set_pos(ota_progress, 80, 212);
        lv_obj_set_size(ota_progress, 200, 20);
        lv_bar_set_range(ota_progress, 0, 100);
        lv_obj_set_style_bg_color(ota_progress, lv_color_hex(0x3D3D3D), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ota_progress, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(ota_progress, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(ota_progress, lv_color_hex(0x696969), LV_PART_MAIN);
        lv_obj_set_style_radius(ota_progress, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ota_progress, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ota_progress, lv_color_hex(COL_GOLD), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ota_progress, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(ota_progress, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_clear_flag(ota_progress, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        ota_activity = halo_ui_ring(ota_overlay, 138, 92, 84, 7, COL_GOLD);
        lv_obj_set_style_arc_color(ota_activity, lv_color_hex(0x3D3D3D), LV_PART_MAIN);
        lv_arc_set_value(ota_activity, 300);
        need_update = true;
      }

      if (phase != last_ota_phase || need_update) {
        last_ota_phase = phase;
        const char* result_title = manual_result == 1 ? "Already up to date" :
            manual_result == 2 ? "Daily update limit" : manual_result == 3 ? "Update postponed" :
            manual_result == 4 ? "Update not completed" : "Update finished";
        const char* result_detail = manual_result == 1 ? "HALO has the latest software." :
            manual_result == 2 ? "Please try again tomorrow." :
            manual_result == 3 ? "The automatic retry stays scheduled." :
            manual_result == 4 ? "Please try again when connected." : "Returning to the menu.";
        lv_label_set_text(ota_label, manual_result ? result_title : is_transfer
                                         ? "Something new is coming"
                                         : (finishing ? "Finishing update" : "Checking for updates"));
        lv_obj_set_pos(ota_label, 40, is_transfer ? 84 : 196);
        lv_label_set_text(ota_description, manual_result ? result_detail : is_transfer ? ""
                                                       : (finishing ? "" : "Getting things ready..."));
        lv_obj_set_pos(ota_description, 50, is_transfer ? 250 : 236);
        if (!manual_result && (is_transfer || finishing))
          lv_obj_add_flag(ota_description, LV_OBJ_FLAG_HIDDEN);
        else
          lv_obj_clear_flag(ota_description, LV_OBJ_FLAG_HIDDEN);
        if (is_transfer) {
          lv_obj_clear_flag(ota_percent, LV_OBJ_FLAG_HIDDEN);
          lv_obj_clear_flag(ota_progress, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(ota_activity, LV_OBJ_FLAG_HIDDEN);
        } else {
          lv_obj_add_flag(ota_percent, LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(ota_progress, LV_OBJ_FLAG_HIDDEN);
        }
        need_update = true;
      }

      // Never run an overlay animation during binary receive. A static arc
      // remains available for the first frame when progress is not yet known.
      if (manual_result || is_transfer || g_lcd_ota_binary_mode) {
        if (ota_spinner) {
          lv_obj_del(ota_spinner);
          ota_spinner = NULL;
        }
        if (!is_transfer && !manual_result)
          lv_obj_clear_flag(ota_activity, LV_OBJ_FLAG_HIDDEN);
        if (manual_result)
          lv_obj_add_flag(ota_activity, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(ota_activity, LV_OBJ_FLAG_HIDDEN);
        if (!ota_spinner) {
          ota_spinner = halo_ui_spinner(ota_overlay, 92, COL_GOLD);
          lv_obj_set_style_arc_color(ota_spinner, lv_color_hex(0x3D3D3D), LV_PART_MAIN);
          need_update = true;
        }
      }
      if (is_transfer) {
        if (g_lcd_ota_progress_pct != last_shown_pct || need_update) {
          last_shown_pct = g_lcd_ota_progress_pct;
          char buf[16];
          snprintf(buf, sizeof(buf), "%d%%", last_shown_pct);
          lv_label_set_text(ota_percent, buf);
          lv_bar_set_value(ota_progress, last_shown_pct, LV_ANIM_OFF);
          need_update = true;
        }
      }

      if (ota_overlay) {
        lv_obj_move_foreground(ota_overlay);
      }
      // Render the existing overlay during preflight. Once BEGIN owns flash,
      // pause all flushes (including a delayed first frame) until cleanup;
      // the main loop observes the same receive/binary guard. A PSRAM-backed
      // LVGL flush must not overlap flash erase, write or finalization.
      if (!g_lcd_ota_uart_receiving && !g_lcd_ota_binary_mode) {
        lv_timer_handler();
      }
      example_lvgl_unlock();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // OTA screen no longer active: delete the overlay so it doesn't linger as a
    // child of whatever screen it was created on (e.g. created while on Settings
    // -> "Software Update" reappears every time Settings is reopened). Runs on
    // the UI task (Core 1) so LVGL is safe here. The LVGL lock acquired at the
    // top of this loop iteration is still held at this point (the active-OTA
    // branch above releases it only inside its own block before continuing), so
    // call lv_obj_del directly — do NOT re-lock (deadlock) and do NOT unlock
    // (the normal path below releases the lock exactly once).
    if (ota_overlay) {
      if (ota_spinner) {
        lv_obj_del(ota_spinner); // stops the indeterminate animation explicitly
        ota_spinner = NULL;
      }
      lv_obj_del(ota_overlay); // also deletes all static presentation children
      ota_overlay = NULL;
      ota_label = NULL;
      ota_percent = NULL;
      ota_progress = NULL;
      ota_description = NULL;
      ota_activity = NULL;
      last_shown_pct = -1;
      last_ota_phase = -1;
      ota_saw_transfer = false;
      Serial.println("[OTA] overlay torn down (screen inactive)");
    }

    app_event_t evt;
    provision_ui_resume_deferred();
    // A provisioning overlay can also cover Processing without a route change.
    // Keep its owned spinner stopped while covered and resume only the still-
    // current Processing screen. Never reset the operation's actual deadline.
    if (ship_processing_spinner) {
      const bool processing_visible =
          ui_screen_state == SCREEN_PROCESSING && lv_scr_act() == ship_processing_screen &&
          !provision_intro_visible &&
          !(provision_screen && !lv_obj_has_flag(provision_screen, LV_OBJ_FLAG_HIDDEN));
      if (!processing_visible)
        lv_anim_del(ship_processing_spinner, NULL);
      else if (!lv_anim_get(ship_processing_spinner, halo_ui_spin))
        halo_ui_spinner_start(ship_processing_spinner);
    }
    bool processed_anything = false;
    bool deferred_evt_ready = false;

    ui_loop_counter++;
    uint32_t submit_ok = 0, submit_fail = 0;
    int outstanding = 0, soft_fault = 0;
    lcd_bsp_get_flush_submit_stats(&submit_ok, &submit_fail, &outstanding, &soft_fault);

    if (soft_fault) {
        lcd_bsp_clear_flush_soft_fault();
        // Force full screen redraw to recover from partial flush corruption
        lv_obj_t *scr = lv_scr_act();
        if (scr) {
            lv_obj_invalidate(scr);
            Serial.println("[LCD_FLUSH] soft_fault detected — full screen invalidated for redraw");
        }
    }
      display_busy_hide_at_ms = 0;

    {
      unsigned long now_hb = millis();
      if ((now_hb - last_heartbeat_ms) >= UI_HEARTBEAT_INTERVAL_MS) {
        last_heartbeat_ms = now_hb;
        size_t heap_internal = esp_get_free_heap_size();
        size_t heap_min = esp_get_minimum_free_heap_size();
        size_t heap_psram = 0;
#if (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM)
        heap_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#endif
        HALO_CHATTY_PRINTF("[UI_HB] heap=%u min=%u psram=%u hwm=%u loop=%lu tick_ms=%lu input_ms=%lu flush_ok=%lu fail=%lu out=%d soft_fault=%d\n",
                      (unsigned)heap_internal,
                      (unsigned)heap_min,
                      (unsigned)heap_psram,
                      (unsigned)uxTaskGetStackHighWaterMark(NULL),
                      (unsigned long)ui_loop_counter,
                      (unsigned long)last_ui_tick_ms,
                      (unsigned long)last_touch_or_input_ms,
                      (unsigned long)submit_ok,
                      (unsigned long)submit_fail,
                      outstanding,
                      soft_fault);
        lv_obj_t* active = lv_scr_act();
        if (ui_screen_state == SCREEN_PROCESSING && active != ship_processing_screen) {
          Serial.printf("[UI_SCREEN][MISMATCH] state=PROCESSING active=%s ptr=%p\n",
                        ui_screen_from_ptr(active),
                        (void*)active);
        }
        if (ui_screen_state == SCREEN_HOLD_STILL && active != ship_hold_screen) {
          Serial.printf("[UI_SCREEN][MISMATCH] state=HOLD_STILL active=%s ptr=%p\n",
                        ui_screen_from_ptr(active),
                        (void*)active);
        }
        if (ui_screen_state == SCREEN_AI_LISTENING && active != ship_ai_listening_screen) {
          Serial.printf("[UI_SCREEN][MISMATCH] state=AI_LISTENING active=%s ptr=%p\n",
                        ui_screen_from_ptr(active),
                        (void*)active);
        }
        if (ui_screen_state == SCREEN_VOICE_JSON && active != ship_voice_json_screen) {
          Serial.printf("[UI_SCREEN][MISMATCH] state=VOICE_JSON active=%s ptr=%p\n",
                        ui_screen_from_ptr(active),
                        (void*)active);
        }
        if (ui_screen_state == SCREEN_HOME && active != ship_menu_screen) {
          Serial.printf("[UI_SCREEN][MISMATCH] state=HOME active=%s ptr=%p\n",
                        ui_screen_from_ptr(active),
                        (void*)active);
        }
      }
    }

#if SHIP_MENU_UI
    shopping_list_expire_pending_delete();
    if (app_event_queue != NULL) {
      while (xQueueReceive(app_event_queue, &evt, 0) == pdTRUE) {
        if (evt.type == EVT_SHIP_UI_STATUS) {
          g_ship_ui_dirty = true;
          ui_apply_ship_ui_status(NULL);
          processed_anything = true;
        } else if (evt.type == EVT_SHIP_UI_TOAST) {
          ui_handle_ship_toast_event(&evt);
          processed_anything = true;
        } else if (evt.type == EVT_SHIP_VOICE_JSON) {
          ship_show_voice_json_screen(g_ship_voice_json_text);
          processed_anything = true;
        } else {
          deferred_evt_ready = true;
          break;
        }
      }
    }
    if (provision_return_home_pending) {
      provision_return_home_pending = false;
      Serial.println("[PROVISION] return_home -> show_main_menu");
      hide_provisioning_screen();
      hide_provision_intro_screen("return_home");
      show_ship_main_menu();
    }
    if (usb_deltouch_tap_due_ms != 0 && millis() >= usb_deltouch_tap_due_ms) {
      // USB 'deltouch' step 2: synthesize the tap at the CENTER of the real
      // Delete button through the normal touch handler, so the derived
      // hitboxes (lv_obj_get_coords) are exercised exactly like a finger.
      usb_deltouch_tap_due_ms = 0;
      if (ui_screen_state == SCREEN_SHOPPING_LIST && shopping_list_overlay_visible &&
          shopping_list_overlay_delete_btn != NULL) {
        lv_area_t btn_area;
        lv_obj_get_coords(shopping_list_overlay_delete_btn, &btn_area);
        int cx = (btn_area.x1 + btn_area.x2) / 2;
        int cy = (btn_area.y1 + btn_area.y2) / 2;
        Serial.printf("[USB] deltouch tap at center of Delete (%d,%d)\n", cx, cy);
        shopping_list_handle_touch(cx, cy);
        ui_lvgl_tick();
        resetActivityTimer();
      } else {
        Serial.println("[USB] deltouch tap skipped (overlay gone)");
      }
      processed_anything = true;
    }
    if (ship_voice_ack_hide_at_ms) {
      if (ui_screen_state != SCREEN_VOICE_ACK) {
        ship_voice_ack_hide_at_ms = 0;
      } else if (millis() >= ship_voice_ack_hide_at_ms) {
        ship_voice_ack_hide_at_ms = 0;
        g_voice_fire_and_forget_ignore_ui = false;
        waiting_for_voice_response = false;
        voice_response_deadline_ms = 0;
        g_ship_voice_json_pending = false;
        stop_glowing_animation();
        Serial.println("[VOICE_FAF] ack_done -> main_menu");
        show_ship_main_menu();
        processed_anything = true;
      }
    }
    if (waiting_for_voice_response &&
        voice_response_deadline_ms > 0 &&
        millis() >= voice_response_deadline_ms) {
      Serial.printf("[VOICE_TIMEOUT] fired screen=%s timeout_ms=%lu\n",
                    ui_screen_state_name(ui_screen_state),
                    (unsigned long)VOICE_RESPONSE_TIMEOUT_MS);
      waiting_for_voice_response = false;
      voice_response_deadline_ms = 0;
      g_ship_voice_json_pending = false;
      stop_glowing_animation();
      ship_show_error_impl();
      processed_anything = true;
    }
    if (g_ship_voice_json_pending) {
      Serial.println("[VOICE_JSON] apply_pending_direct");
      ship_show_voice_json_screen(g_ship_voice_json_text);
      processed_anything = true;
    }
    debug_screen_update();
    result_retry_tick();
    if (ship_logged_hide_at_ms && millis() >= ship_logged_hide_at_ms) {
      ship_logged_hide_at_ms = 0;
      show_ship_main_menu();
    }
    if (ui_screen_state == SCREEN_HOLD_STILL) {
      ship_update_hold_still_countdown();
    }
    if (ui_screen_state == SCREEN_EXPIRY_CHOICE && ship_expiry_choice_shown_time > 0) {
      ship_update_expiry_choice_timeout_ring();
    }
    if (ui_screen_state == SCREEN_EXPIRY && expiry_screen_visible) {
      expiry_update_timeout_ring();
    }
    if (ui_screen_state == SCREEN_AI_LISTENING) {
      ship_update_ai_listening_countdown();
    }
    if (ui_screen_state == SCREEN_SHOPPING_LIST) {
      // Poll the refresh SM state into the border refresh ring. The
      // WAKE_PENDING -> INFLIGHT transition happens on the UART task (Core 0),
      // so the UI task picks it up here; no-op when the state hasn't changed.
      shopping_list_refresh_indicator_sync(false);
    }
    if (ship_error_hide_at_ms && millis() >= ship_error_hide_at_ms) {
      ship_error_hide_at_ms = 0;
      show_ship_main_menu();
    }
    // Strand net. Every user-feedback screen (VOICE_ACK / LOGGED / RESULT) is
    // supposed to arm its own deadline, and none of them is sleep-eligible --
    // so one that forgets pins the panel awake until the guardian force-sleeps
    // five minutes later. That is exactly how the "stuck on the result screen"
    // report happened. This only fires when NO deadline is armed, so a screen
    // with a legitimately long one (the dish card) is never cut short.
    {
      static unsigned long feedback_stranded_since_ms = 0;
      unsigned long now_fb = millis();
      bool undeadlined_feedback =
          (ship_user_state_current() == SHIP_USER_STATE_USER_FEEDBACK) &&
          ship_error_hide_at_ms == 0 &&
          ship_logged_hide_at_ms == 0 &&
          ship_voice_ack_hide_at_ms == 0;
      if (!undeadlined_feedback) {
        feedback_stranded_since_ms = 0;
      } else if (feedback_stranded_since_ms == 0) {
        feedback_stranded_since_ms = now_fb;
      } else if (now_fb - feedback_stranded_since_ms >= UI_FEEDBACK_STRAND_MS) {
        Serial.printf("[UI_STRAND] feedback screen=%s had no deadline for %lu ms -> main_menu\n",
                      ui_screen_state_name(ui_screen_state),
                      now_fb - feedback_stranded_since_ms);
        feedback_stranded_since_ms = 0;
        show_ship_main_menu();
      }
    }
    if (g_ship_ui_dirty) {
      ui_apply_ship_ui_status(NULL);
      processed_anything = true;
    } else if (ship_ui_terminal_apply_pending()) {
      Serial.printf("[SHIP_UI] reapply_terminal phase=%s job_id=%lu screen=%s applied_msg_id=%lu latest_msg_id=%lu\n",
                    g_ship_ui_phase,
                    (unsigned long)g_ship_ui_job_id,
                    ui_screen_state_name(ui_screen_state),
                    (unsigned long)g_ship_ui_applied_msg_id,
                    (unsigned long)g_last_ui_status_msg_id);
      ui_apply_ship_ui_status(NULL);
      processed_anything = true;
    }
    if (dish_processing_active && ui_screen_state == SCREEN_PROCESSING && dish_processing_start_ms > 0) {
      ship_update_processing_progress();
      unsigned long now_ms = millis();
      unsigned long age_ms = now_ms - dish_processing_start_ms;
      if (age_ms > DISH_PROCESSING_TIMEOUT_MS) {
        Serial.printf("[DISH_TIMEOUT] processing exceeded %lu ms -> error_then_home\n",
                      (unsigned long)DISH_PROCESSING_TIMEOUT_MS);
        dish_processing_active = false;
        dish_processing_start_ms = 0;
        waiting_for_scan_response = false;
        scan_request_sent_ms = 0;
        dish_timeout_at_ms = now_ms;
        dish_timeout_job_id = g_ship_ui_job_id;
        g_ship_ui_finalized = true;
        g_ship_ui_finalized_job_id = g_ship_ui_job_id;
        strncpy(g_ship_ui_phase, "ERROR", sizeof(g_ship_ui_phase) - 1);
        g_ship_ui_phase[sizeof(g_ship_ui_phase) - 1] = '\0';
        strncpy(g_ship_ui_text, "Timed out. Try again.", sizeof(g_ship_ui_text) - 1);
        g_ship_ui_text[sizeof(g_ship_ui_text) - 1] = '\0';
        g_ship_ui_error = true;
        g_ship_ui_terminal = true;
        UI_SHOW(SCREEN_SHIP_ERROR, "dish_timeout");
        ship_error_hide_at_ms = now_ms + 2000;
      }
    }
    // Scan response timeout: if LCD sent a capture request and Sense never
    // responded with any UI_STATUS within SCAN_NO_RESPONSE_TIMEOUT_MS,
    // show an error and return to menu.  This catches sleep/wake races,
    // Sense memory exhaustion, and deferred-TX stalls.
    if (waiting_for_scan_response && scan_request_sent_ms > 0 &&
        !dish_processing_active &&  // dish has its own timeout
        // Only fire while genuinely awaiting Sense's FIRST response on a
        // capture/processing screen.  Once a user-choice/expiry/terminal
        // screen is up, the Sense has already responded and that screen
        // owns its own graceful countdown -- this watchdog must not race it
        // into an error screen.  (SCREEN_RESULT is the error-screen state.)
        ui_screen_state != SCREEN_EXPIRY_CHOICE &&
        ui_screen_state != SCREEN_EXPIRY &&
        ui_screen_state != SCREEN_LOGGED &&
        ui_screen_state != SCREEN_RESULT) {
      unsigned long now_ms = millis();
      unsigned long age_ms = now_ms - scan_request_sent_ms;
      if (age_ms > SCAN_NO_RESPONSE_TIMEOUT_MS) {
        Serial.printf("[SCAN_TIMEOUT] no response from Sense in %lu ms -> error_then_home mode=%s\n",
                      age_ms, g_ship_ui_mode);
        // Dump how far this capture actually got. "No response" alone cannot
        // distinguish a lost request from a camera stall, and they need
        // opposite fixes — this line names which one happened.
        captrace_strand(age_ms, last_sense_rx_ms);
        waiting_for_scan_response = false;
        scan_request_sent_ms = 0;
        g_ship_ui_finalized = true;
        g_ship_ui_finalized_job_id = g_ship_ui_job_id;
        strncpy(g_ship_ui_phase, "ERROR", sizeof(g_ship_ui_phase) - 1);
        g_ship_ui_phase[sizeof(g_ship_ui_phase) - 1] = '\0';
        strncpy(g_ship_ui_text, "No response. Try again.", sizeof(g_ship_ui_text) - 1);
        g_ship_ui_text[sizeof(g_ship_ui_text) - 1] = '\0';
        g_ship_ui_error = true;
        g_ship_ui_terminal = true;
        UI_SHOW(SCREEN_SHIP_ERROR, "scan_no_response_timeout");
        ship_error_hide_at_ms = now_ms + 3000;
      }
    }
    wifi_on_run_deferred_if_ready("ui_tick");
    if (g_status_hide_at_ms && millis() >= g_status_hide_at_ms) {
      status_overlay_hide();
    }
#endif

    /* Deferred scroll redraw: avoid flooding SPI when we throttled the last scroll */
    if (scroll_pending_redraw && (millis() - last_scroll_refresh_ms >= SCROLL_REFRESH_MIN_MS)) {
      ui_refresh_from_state(&g_active);
      ui_lvgl_tick();
      last_scroll_refresh_ms = millis();
      scroll_pending_redraw = false;
    }
    /* Deferred glowing animation: run on next iteration so list redraw + one lv_timer_handler can drain before adding animation (avoids panel_io_spi_tx_color queue failed) */
    if (defer_glowing_after_refresh) {
      defer_glowing_after_refresh = false;
      start_glowing_animation(defer_glowing_reason[0] ? defer_glowing_reason : "list_refresh");
      defer_glowing_reason[0] = '\0';
      ui_lvgl_tick();
    }
    
    // CRITICAL: Process scroll events ONE AT A TIME for maximum responsiveness
    // Check for scroll events with 0 timeout (non-blocking, immediate)
    if (deferred_evt_ready ||
        (app_event_queue != NULL && xQueueReceive(app_event_queue, &evt, 0) == pdTRUE)) {
      
      if (evt.type == EVT_SCROLL_DELTA) {
        if (provisioning_input_locked()) {
          resetActivityTimer();
          continue;
        }
        user_activity_bump("scroll");
        bool scroll_wake_context =
            g_idle_screen_dark || g_sleep_transition || g_in_light_sleep ||
            (g_backlight_duty == 0) || !g_panel_enabled || !g_lvgl_running;
        ensure_awake_for_ui("scroll_evt");
        if (scroll_wake_context || sense_state != SENSE_AWAKE || sleep_ready_received) {
          request_sense_wake("scroll_wake");
        }
        Serial.printf("[UI] scroll_evt delta=%d\n", evt.data.scroll_delta);
        // Ignore scrolls during wake-up period
        if (millis() >= scroll_ignore_until) {
          if (ui_screen_state == SCREEN_VOICE_JSON) {
            ship_voice_ui_handle_scroll(evt.data.scroll_delta);
            lv_timer_handler();
            resetActivityTimer();
            example_lvgl_unlock();
            continue;
          }
          if (ui_screen_state == SCREEN_BACKLIGHT) {
            int pct = constrain(backlight_get_pct() + evt.data.scroll_delta * 5, 5, 100);
            backlight_apply_pct(pct);  // applies live; floor keeps screen recoverable
            if (ship_backlight_ring != NULL) {
              lv_arc_set_value(ship_backlight_ring, pct * 10);
            }
            if (ship_backlight_pct_label != NULL) {
              char buf[8];
              snprintf(buf, sizeof(buf), "%d%%", pct);
              lv_label_set_text(ship_backlight_pct_label, buf);
            }
            lv_timer_handler();
            resetActivityTimer();
            example_lvgl_unlock();
            continue;
          }
          if (ui_screen_state == SCREEN_SHOPPING_LIST) {
            int new_idx = shopping_list_scroll_idx + evt.data.scroll_delta;
            if (new_idx < 0) new_idx = 0;
            if (new_idx >= g_active.count) new_idx = g_active.count - 1;
            // Track overscroll at top for pull-to-refresh. Forgiving: ticks
            // only reset when the selection leaves the top, on a CW (down)
            // scroll, or when the last CCW tick is stale (>1.5s old).
            if (shopping_list_scroll_idx == 0 && evt.data.scroll_delta < 0) {
              unsigned long now_ticks = millis();
              if (shopping_list_last_ccw_tick_ms > 0 &&
                  (now_ticks - shopping_list_last_ccw_tick_ms) > SHOPPING_LIST_OVERSCROLL_WINDOW_MS) {
                shopping_list_overscroll_ticks = 0;  // stale ticks expire
              }
              shopping_list_overscroll_ticks -= evt.data.scroll_delta;  // delta is negative, so this adds
              shopping_list_last_ccw_tick_ms = now_ticks;
              if (shopping_list_overscroll_ticks >= SHOPPING_LIST_REFRESH_TICKS) {
                shopping_list_overscroll_ticks = 0;
                shopping_list_last_ccw_tick_ms = 0;
                shopping_list_trigger_refresh("list_refresh");
              }
            } else if (evt.data.scroll_delta > 0 || new_idx > 0) {
              shopping_list_overscroll_ticks = 0;  // scrolled down or selection moved off the top
              shopping_list_last_ccw_tick_ms = 0;
            }
            if (new_idx != shopping_list_scroll_idx && g_active.count > 0) {
              // Lightweight in-place restyle of the two affected cards —
              // no full repopulate, so fast knob scrolls stay smooth.
              int old_idx = shopping_list_scroll_idx;
              shopping_list_scroll_idx = new_idx;
              shopping_list_update_selection(old_idx, new_idx);
              ui_lvgl_tick();
            }
            resetActivityTimer();
            example_lvgl_unlock();
            continue;
          }
          if (ui_screen_state == SCREEN_HOME && ship_menu_screen_state == SHIP_MENU_SCREEN_MAIN) {
            // The ship home screen is touch-first. Do not fall through to the retired list UI.
            resetActivityTimer();
            example_lvgl_unlock();
            continue;
          }
          if (ui_screen_state == SCREEN_EXPIRY_CHOICE) {
            if (!expiry_submitted && !ship_choice_mode_is_discard()) {
              expiry_choice_adjust_quantity(evt.data.scroll_delta);
              lv_timer_handler();
              resetActivityTimer();
            }
            example_lvgl_unlock();
            continue;
          }
          // Expiry screen uses the knob to edit the selected date.
          if (expiry_screen_visible && expiry_screen != NULL && !lv_obj_has_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN)) {
            if (!expiry_submitted) {
              if (expiry_active_segment == EXPIRY_SEGMENT_MONTH) {
                expiry_step_month(evt.data.scroll_delta);
              } else if (expiry_active_segment == EXPIRY_SEGMENT_DAY) {
                expiry_step_day(evt.data.scroll_delta);
              } else {
                expiry_step_year(evt.data.scroll_delta);
              }
              expiry_refresh_picker_ui();
              Serial.printf("[EXPIRY] Scroll adjusted segment=%s delta=%d -> %04d-%02d-%02d\n",
                            expiry_active_segment == EXPIRY_SEGMENT_MONTH ? "month"
                                : (expiry_active_segment == EXPIRY_SEGMENT_DAY ? "day" : "year"),
                            evt.data.scroll_delta,
                            expiry_selected_year,
                            expiry_selected_month,
                            expiry_selected_day);
            }
            lv_timer_handler();
            resetActivityTimer();
            example_lvgl_unlock();
            continue;
          }
          
          // Check if menu screen is visible - handle menu scrolling
          if (menu_screen_visible) {
            // Menu scrolling
            int old_index = menu_selected_index;
            menu_selected_index += evt.data.scroll_delta;
            
            // Clamp to valid range
            if (menu_selected_index < 0) {
              menu_selected_index = 0;
            }
            if (menu_selected_index >= menu_item_count) {
              menu_selected_index = menu_item_count - 1;
            }
            
            // Update menu display if selection changed
            if (menu_selected_index != old_index) {
              update_menu_display();
              lv_timer_handler();  // Force immediate render
              resetActivityTimer();  // Reset activity timer on scroll
            }
          } else {
            // Normal list scrolling
            if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
              apply_event_to_state(&g_active, &evt);
              xSemaphoreGive(app_state_mutex);
              
              // Hide buttons on scroll (user is scrolling, not interacting with buttons)
              if (buttons_visible) {
                buttons_visible = false;
                if (delete_menu != NULL) {
                  lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
                }
                if (menu_menu != NULL) {
                  lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
                }
              }
            
            // Update UI: throttle redraws to avoid SPI queue overflow on fast scroll
            int sel_before = g_active.selected_index;
            unsigned long now_scroll = millis();
            if (now_scroll - last_scroll_refresh_ms >= SCROLL_REFRESH_MIN_MS) {
              ui_refresh_from_state(&g_active);
              ui_lvgl_tick();
              last_scroll_refresh_ms = now_scroll;
              scroll_pending_redraw = false;
            } else {
              scroll_pending_redraw = true;
            }
            int sel_after = g_active.selected_index;
            if (sel_after != sel_before) {
              Serial.printf("[UI] scroll_apply sel_before=%d sel_after=%d\n", sel_before, sel_after);
            }
              
              // Reset activity timer on user interaction
              resetActivityTimer();
              
              // LVGL tick already done in throttle block when we refreshed
              
              processed_anything = true;
            }
          }
        }
        // Continue immediately to check for next scroll event (no delay)
    example_lvgl_unlock();
        continue;
        
      } else if (evt.type == EVT_VOICE_ITEMS_ADDED) {
        // Optimistic voice items added - refresh UI directly without swapping
        Serial.printf("[UI] Voice items added event - refreshing UI (count=%d)\n", evt.data.new_count);
        if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          // Refresh UI directly from g_active (no swap needed)
          ui_refresh_from_state(&g_active);
          xSemaphoreGive(app_state_mutex);
          
          // Force LVGL to render
          lv_timer_handler();
          
          processed_anything = true;
        }
        
      } else if (evt.type == EVT_RENDER_ACTIVE_LIST) {
        // Render active list (e.g., restore after wake)
        Serial.printf("[UI] Render active list event - refreshing UI (count=%d)\n", evt.data.new_count);
        if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          ui_refresh_from_state(&g_active);
          xSemaphoreGive(app_state_mutex);
          lv_timer_handler();
          processed_anything = true;
        }
        
      } else if (evt.type == EVT_STOP_GLOWING) {
        stop_glowing_animation();
        lv_timer_handler();
        processed_anything = true;
      } else if (evt.type == EVT_START_GLOWING) {
        if (!is_glowing_animation) {
          start_glowing_animation(evt.data.glow_reason[0] ? evt.data.glow_reason : "uart");
          ui_lvgl_tick();
        }
        processed_anything = true;
      } else if (evt.type == EVT_UI_STATUS_IDLE) {
        set_status_reset_visible(false);
        if (is_glowing_animation) {
          stop_glowing_animation();
          ui_lvgl_tick();
        }
        processed_anything = true;
      } else if (evt.type == EVT_REFRESH_TIMEOUT) {
        // Refresh got stuck inflight (Sense never returned UI_LIST). The Core-0
        // refresh SM already cleared all the inflight flags; here we just stop
        // the glow, re-render the existing (cached) list, and surface a brief
        // "Couldn't refresh" notice so the user isn't stuck on "Refreshing...".
        Serial.println("[UI] Refresh timeout event - clearing stuck refresh UI");
        if (is_glowing_animation) {
          stop_glowing_animation();
        }
        // Refresh RESOLVED (timed out). Re-arm the pull latch even if we don't
        // repopulate below — a timed-out refresh that never populates must not
        // leave the latch consumed, or the next pull-to-refresh won't fire.
        shopping_list_reset_pull_latch();
        if (ui_screen_state == SCREEN_SHOPPING_LIST) {
          // Revert to the existing (cached) list, then flash the border ring
          // in the error red + transient "Couldn't refresh" toast.
          shopping_list_screen_populate();
          shopping_list_ring_show_error();
          // The SM already cleared itself to IDLE — keep the sync cache
          // coherent so the UI-task poll doesn't cut the error flash short.
          shopping_list_refresh_ui_state = (int)refresh_state;
        }
        ui_lvgl_tick();
        processed_anything = true;
      } else if (evt.type == EVT_LINK_SEND_FAILED) {
        // The Sense never acked a user-intent message after every retry. The
        // request is genuinely gone — not slow — so the honest thing is to stop
        // the animation and tell the user, rather than leave them watching a
        // capture that will never happen. This is the strand the whole ack layer
        // exists to convert into an error.
        Serial.println("[UI] Link send failed - resolving screen with error");
        if (is_glowing_animation) {
          stop_glowing_animation();
        }
        // Retire the in-flight capture before showing the result. Leaving
        // waiting_for_scan_response set strands the device: the scan watchdog
        // below is deliberately suppressed on SCREEN_RESULT, so nothing else
        // would ever resolve this screen, and SCREEN_RESULT is not
        // sleep-eligible -- the panel stays lit until the 5-minute guardian.
        waiting_for_scan_response = false;
        scan_request_sent_ms = 0;
        dish_processing_active = false;
        dish_processing_start_ms = 0;
        g_ship_ui_finalized = true;
        g_ship_ui_finalized_job_id = g_ship_ui_job_id;
        ui_show_result(true, "Couldn't reach sensor", "");
        ui_lvgl_tick();
        processed_anything = true;
      } else if (evt.type == EVT_LIST_DELETE_RESULT) {
        int removed = shopping_list_apply_delete_result(evt.data.list_delete_result.id,
                                                       evt.data.list_delete_result.ok);
        if (removed >= 0 && ui_screen_state == SCREEN_SHOPPING_LIST) shopping_list_animate_card_removal(removed);
        resetActivityTimer();
        processed_anything = true;
      } else if (evt.type == EVT_LIST_REPLACED) {
        // List was replaced (from UART task) - stop glowing, then swap pending → active and render
        stop_glowing_animation();
        Serial.printf("[UI] List replaced event - swapping pending → active (count=%d)\n", evt.data.new_count);
        if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          if (pending_ready) {
            // Save previous selection
            int prev_sel = g_active.selected_index;
            bool preserve = user_has_scrolled;
            
            // Copy pending → active (struct copy)
            // Note: deleted items have already been filtered out in uart_process_received_message
            g_active = g_pending;
            
            // Clean up deleted_item_ids: remove IDs that are no longer in the list (backend processed them)
            // This prevents the deleted list from growing indefinitely
            if (deleted_item_count > 0) {
              int new_deleted_count = 0;
              for (int i = 0; i < deleted_item_count; i++) {
                bool still_exists = false;
                // Check if this deleted ID still exists in the new list
                for (int j = 0; j < g_active.count; j++) {
                  if (strcmp(deleted_item_ids[i], g_active.item_ids[j]) == 0) {
                    still_exists = true;
                    break;
                  }
                }
                // If item no longer exists in list, backend has processed the delete - remove from tracking
                if (!still_exists) {
                  Serial.printf("[UI] Deleted item ID %s no longer in list - removing from tracking\n", deleted_item_ids[i]);
                  // Don't copy to new_deleted_count - effectively removes it
                } else {
                  // Still exists (shouldn't happen if filtering worked, but keep it just in case)
                  if (new_deleted_count < i) {
                    strncpy(deleted_item_ids[new_deleted_count], deleted_item_ids[i], 63);
                    deleted_item_ids[new_deleted_count][63] = '\0';
                  }
                  new_deleted_count++;
                }
              }
              deleted_item_count = new_deleted_count;
              if (new_deleted_count < deleted_item_count) {
                Serial.printf("[UI] Cleaned up deleted items tracking: %d remaining\n", deleted_item_count);
              }
            }
            
            // Reset UI to clean state if we just woke up
            bool was_just_woke = just_woke_up;
            if (was_just_woke) {
              Serial.println("[UI] Just woke up - resetting UI to clean state");
              
              // Reset selected_index to 0 for clean start
              g_active.selected_index = (g_active.count > 0) ? 0 : -1;
              
              // Hide all screens and show only the list
              if (status_screen != NULL) {
                lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              }
              if (delete_menu != NULL) {
                lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
              }
              if (menu_menu != NULL) {
                lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
              }
              if (recording_indicator != NULL) {
                lv_obj_add_flag(recording_indicator, LV_OBJ_FLAG_HIDDEN);
              }
              if (loading_screen != NULL) {
                lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
              }
              
              // Ensure list container is visible
              if (list_container != NULL && g_active.count > 0) {
                lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              
              // Clear the wake-up flag
              just_woke_up = false;
              
              Serial.println("[UI] UI reset complete - showing clean list at index 0");
            }
            
            // Preserve user's scroll position if they've scrolled (only if not just woke up)
            if (preserve && !was_just_woke) {
              if (g_active.count <= 0) {
                g_active.selected_index = -1;
              } else {
                if (prev_sel < 0) prev_sel = 0;
                if (prev_sel >= g_active.count) prev_sel = g_active.count - 1;
                g_active.selected_index = prev_sel;
                Serial.printf("[UI] Preserved user scroll position: %d\n", g_active.selected_index);
              }
            } else {
              Serial.printf("[UI] Using Sense selected_index: %d\n", g_active.selected_index);
            }
            
            pending_ready = false;
            
            // Ensure list container is visible
            if (g_active.count > 0 && list_container != NULL) {
              lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
            }
            if (loading_screen != NULL) {
              lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
            }
            
            // Render from g_active (LVGL call ONLY here)
            ui_refresh_from_state(&g_active);
            
            // Save to storage immediately
            save_list_to_storage(&g_active);
            
            // CRITICAL: Reset activity timer appropriately
            // If we just woke up, always reset the timer (user just woke the device)
            // If it's a background update after wake (more than 3 seconds), also reset it
            // Only skip reset if it's a background update within 3 seconds of wake (to prevent sleep loop)
            unsigned long time_since_wake = millis() - last_wake_time;
            if (was_just_woke) {
              // Just woke up - always reset activity timer so device doesn't immediately sleep
              resetActivityTimer();
              Serial.println("[UI] Just woke up - resetting activity timer");
            } else if (!g_in_light_sleep && time_since_wake > 3000) {
              // Background update after wake period (3+ seconds) - safe to reset
              resetActivityTimer();
              Serial.println("[UI] Background list update (3+ seconds after wake) - resetting activity timer");
            } else if (time_since_wake <= 3000) {
              // Background update within 3 seconds of wake - DON'T reset timer to prevent sleep loop
              Serial.printf("[UI] Background list update (%lu ms after wake) - NOT resetting activity timer to prevent sleep loop\n", time_since_wake);
            } else {
              // Normal background update (not in sleep, more than 3 seconds after wake)
              resetActivityTimer();
            }
            
            Serial.printf("[UI] Swapped pending → active: %d items, selected_index=%d\n", 
                          g_active.count, g_active.selected_index);
          }
          xSemaphoreGive(app_state_mutex);
        }
        // If on shopping list screen, re-populate with new data — but only
        // when the content actually changed. A revalidate that returns the
        // identical list (the common case) skips the rebuild entirely: no
        // staggered reveal, no flicker, selection/scroll untouched.
        if (ui_screen_state == SCREEN_SHOPPING_LIST) {
          // Refresh RESOLVED (list landed). Re-arm the pull latch covering BOTH
          // branches below — the unchanged-list branch skips the populate (and
          // thus its built-in latch reset), so do it here unconditionally.
          resetActivityTimer();  // Start the idle interval when the list is ready.
          shopping_list_reset_pull_latch();
          uint32_t new_sig = shopping_list_content_sig(&g_active);
          if (new_sig == shopping_list_rendered_sig) {
            shopping_list_refresh_indicator_sync(true);  // refresh done — ring closes to full + fades (REFRESH_COMPLETE)
            ui_lvgl_tick();
            Serial.println("[UI] Shopping list unchanged — skipped re-render");
          } else {
            shopping_list_reveal_pending = true;  // staggered fade-in for the fresh rows
            shopping_list_screen_populate();
            shopping_list_refresh_indicator_sync(true);  // refresh done — ring closes to full + fades (REFRESH_COMPLETE)
            ui_lvgl_tick();
            Serial.println("[UI] Shopping list screen refreshed with new data");
          }
        }
        processed_anything = true;
      } else if (evt.type == EVT_SHOW_PROVISION_QR) {
        show_provisioning_screen(evt.data.provision.ssid,
                                 evt.data.provision.password,
                                 evt.data.provision.url);
        processed_anything = true;
      } else if (evt.type == EVT_HIDE_PROVISION_QR) {
        hide_provisioning_screen();
        processed_anything = true;
      } else if (evt.type == EVT_UPDATE_PROVISION_STATUS) {
        update_provision_status_label(evt.data.provision_status.state);
        processed_anything = true;
      } else if (evt.type == EVT_SHOW_PROVISION_INTRO) {
        show_provision_intro_screen("status_ap_setup");
        processed_anything = true;
      } else if (evt.type == EVT_HIDE_PROVISION_INTRO) {
        hide_provision_intro_screen("status_complete");
        processed_anything = true;
      } else if (evt.type == EVT_RESET_UI) {
        // Force UI back to list state after sleep
        if (status_screen != NULL) {
          lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
        }
        set_status_reset_visible(false);
        if (logged_screen != NULL) {
          lv_obj_add_flag(logged_screen, LV_OBJ_FLAG_HIDDEN);
        }
        if (expiry_screen != NULL) {
          lv_obj_add_flag(expiry_screen, LV_OBJ_FLAG_HIDDEN);
        }
        expiry_screen_visible = false;
        expiry_screen_shown_time = 0;
        provision_ui_stop_spinner();
        if (provision_screen != NULL) {
          lv_obj_add_flag(provision_screen, LV_OBJ_FLAG_HIDDEN);
        }
        provision_screen_visible = false;
        if (menu_screen != NULL) {
          lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
        }
        menu_screen_visible = false;
        set_menu_mode(MENU_MODE_MAIN);
        menu_selected_index = 0;
        if (loading_screen != NULL) {
          lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
        }
        if (delete_menu != NULL) {
          lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
        }
        if (menu_menu != NULL) {
          lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
        }
        buttons_visible = false;

        if (list_container != NULL && g_active.count > 0) {
          lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
        }
        status_screen_shown_time = 0;
        lv_timer_handler();
        processed_anything = true;
      } else if (evt.type == EVT_HAPTIC_TICK) {
        haptic_pulse_scroll();
        processed_anything = true;
      } else if (evt.type == EVT_MENU_SELECTED) {
        // Menu item selected - send to Sense board and handle accordingly
        int selected_index = evt.data.menu_index;
        if (selected_index >= 0 && selected_index < menu_item_count && menu_items_current[selected_index] != NULL) {
          const char* selected_item = menu_items_current[selected_index];
          Serial.printf("[MENU] Menu item selected: %s (index %d)\n", selected_item, selected_index);
          
          if (menu_mode == MENU_MODE_SETTINGS) {
            if (strcmp(selected_item, "Back") == 0) {
              set_menu_mode(MENU_MODE_MAIN);
              menu_selected_index = 0;
              update_menu_display();
              resetActivityTimer();
              processed_anything = true;
              example_lvgl_unlock();
              continue;
            } else if (strcmp(selected_item, "Reset Wi-Fi") == 0) {
              lcd_force_wake_sense("reset_wifi");
              provision_user_requested = true;
              StaticJsonDocument<128> doc;
              doc["ver"] = PROTOCOL_VERSION;
              doc["type"] = "INPUT_RESET_WIFI";
              doc["msg_id"] = get_next_msg_id();
              doc["ts"] = millis();
              String output;
              serializeJson(doc, output);
              senseSerial.println(output);
              Serial.printf("[MENU] Sent reset Wi-Fi request to Sense: %s\n", output.c_str());
              provision_qr_wait_begin("menu_select");
              
              hide_menu_screen();
              if (status_screen != NULL && status_label != NULL) {
                status_screen_use_text("Resetting\nWi-Fi...");
                lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              }
              ui_lvgl_tick();
              resetActivityTimer();
              processed_anything = true;
              example_lvgl_unlock();
              continue;
            } else if (strcmp(selected_item, "Run OTA Update") == 0) {
              ship_menu_send_manual_ota("menu_select");
              hide_menu_screen();
              ui_lvgl_tick();
              resetActivityTimer();
              processed_anything = true;
              example_lvgl_unlock();
              continue;
            }
          }
          
          // Handle "Settings" selection - open settings menu
          if (strcmp(selected_item, "Settings") == 0) {
            set_menu_mode(MENU_MODE_SETTINGS);
            menu_selected_index = 0;
            update_menu_display();
            resetActivityTimer();
            processed_anything = true;
            example_lvgl_unlock();
            continue;
          }
          
          // Handle "Home" selection - just go back to shopping list
          if (strcmp(selected_item, "Home") == 0) {
            hide_menu_screen();
            // Show list again
            if (list_container != NULL && g_active.count > 0) {
              lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
            }
            resetActivityTimer();
            processed_anything = true;
            example_lvgl_unlock();
            continue;  // Don't send message to Sense for Home
          }
          
          // Handle "Dish" selection - show status screen immediately
          if (strcmp(selected_item, "Dish") == 0) {
            // Show "Hold still!" status screen immediately (same as old menu button behavior)
            if (status_screen != NULL && status_label != NULL) {
              // Hide menu and list
              hide_menu_screen();
              if (list_container != NULL) {
                lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              // Show status screen with "Hold still!"
              status_screen_use_text("");
              lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              status_screen_shown_time = millis();
              Serial.println("[STATUS] Showing Frame_439_2 for Dish scan");
                ui_lvgl_tick();  // Force immediate render
            }
          }
          
          // Handle "Discard" selection - show status screen
          else if (strcmp(selected_item, "Discard") == 0) {
            // Show "Hold still!" status screen immediately
            if (status_screen != NULL && status_label != NULL) {
              // Hide menu and list
              hide_menu_screen();
              if (list_container != NULL) {
                lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              // Show status screen with "Hold still!"
              status_screen_use_text("");
              lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              status_screen_shown_time = millis();
              Serial.println("[STATUS] Showing Frame_439_2 for Discard scan");
              lv_timer_handler();  // Force immediate render
            }
          }
          
          // Handle "Check-in" selection - show status screen (same UI as discard)
          else if (strcmp(selected_item, "Check-in") == 0) {
            // Show "Hold still!" status screen immediately
            if (status_screen != NULL && status_label != NULL) {
              // Hide menu and list
              hide_menu_screen();
              if (list_container != NULL) {
                lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
              }
              // Show status screen with "Hold still!"
              status_screen_use_text("");
              lv_obj_clear_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
              status_screen_shown_time = millis();
              Serial.println("[STATUS] Showing Frame_439_2 for Check-in scan");
              lv_timer_handler();  // Force immediate render
            }
          }
          
          // Send menu selection to Sense board (for Dish, Discard, and Check-in)
          ship_menu_send_menu_select(selected_item, selected_index, selected_item);
          
          // Hide menu screen (if not already hidden for Dish/Discard)
          if (strcmp(selected_item, "Dish") != 0 && strcmp(selected_item, "Discard") != 0) {
            hide_menu_screen();
          }
          
          // Reset activity timer
          resetActivityTimer();
        }
        processed_anything = true;
      } else if (evt.type == EVT_USB_ENTER_LIST) {
        // USB 'list' — same path as tapping List on the second menu
        // (SHIP_MENU_ACTION_SHOPPING_LIST in lcd_ship_action.h). Entry renders
        // the cached list and auto-triggers the "entry_revalidate" refresh.
        show_shopping_list_screen();
        resetActivityTimer();
        Serial.println("[USB] list -> shopping list");
        processed_anything = true;
      } else if (evt.type == EVT_USB_REFRESH || evt.type == EVT_USB_PULL) {
        // USB 'refresh' / 'pull' — same path as the pull-to-refresh gesture
        // (distinct reasons for log/assertion clarity).
        if (ui_screen_state == SCREEN_SHOPPING_LIST) {
          shopping_list_trigger_refresh(evt.type == EVT_USB_PULL ? "usb_pull" : "usb_refresh");
          resetActivityTimer();
          Serial.println("[USB] refresh/pull -> list refresh triggered");
        } else {
          Serial.println("[USB] refresh/pull ignored (not on shopping list screen)");
        }
        processed_anything = true;
      } else if (evt.type == EVT_USB_UI_REVIEW) {
        lcd_ui_review_dump("usb_uilayout");
        processed_anything = true;
      } else if (evt.type == EVT_USB_LISTSTATE) {
        // USB 'liststate' — one machine-readable line for the e2e harness.
        // Printed from the UI task so screen/refresh/list state is coherent.
        // The "pill"/"pill_hiding" field NAMES are kept for harness
        // compatibility but now report the border refresh ring (ring active /
        // ring fade-out in progress).
        bool ring_active = (ui_screen_state == SCREEN_SHOPPING_LIST &&
                            shopping_list_refresh_ring != NULL &&
                            !lv_obj_has_flag(shopping_list_refresh_ring, LV_OBJ_FLAG_HIDDEN));
        // dedupe_age_ms: ms since the last UI_LIST completion (capped at
        // 99999, -1 if never) — makes the UI_LIST dedup window observable.
        long dedupe_age_ms = -1;
        if (lcd_last_ui_list_complete_ms > 0) {
          unsigned long age = millis() - lcd_last_ui_list_complete_ms;
          dedupe_age_ms = (age > 99999UL) ? 99999L : (long)age;
        }
        StaticJsonDocument<128> selected_id_doc;
        selected_id_doc["id"] = (shopping_list_scroll_idx >= 0 && shopping_list_scroll_idx < g_active.count)
                                    ? g_active.item_ids[shopping_list_scroll_idx] : "";
        char selected_id_json[6 * sizeof(g_active.item_ids[0]) + 3];
        serializeJson(selected_id_doc["id"], selected_id_json, sizeof(selected_id_json));
        Serial.printf("[LISTSTATE] {\"screen\":%d,\"refresh_state\":%d,\"pill\":%d,\"count\":%d,"
                      "\"cache_age_s\":%d,\"auto_retry\":%d,\"selected\":%d,"
                      "\"selected_id\":%s,"
                      "\"latch\":%d,\"armed\":%d,\"scroll_y\":%d,\"pill_hiding\":%d,"
                      "\"dedupe_age_ms\":%ld}\n",
                      (int)ui_screen_state,
                      (int)refresh_state,
                      ring_active ? 1 : 0,
                      g_active.count,
                      list_cache_age_s(),
                      (int)s_list_auto_retry_count,
                      shopping_list_scroll_idx,
                      selected_id_json,
                      shopping_list_touch_pull_consumed ? 1 : 0,
                      shopping_list_touch_pull_armed ? 1 : 0,
                      shopping_list_scroll ? (int)lv_obj_get_scroll_y(shopping_list_scroll) : 0,
                      shopping_list_ring_hiding ? 1 : 0,
                      dedupe_age_ms);
        processed_anything = true;
      } else if (evt.type == EVT_USB_DELETE) {
        // USB 'del N' — same path as the DELETE touch on the N-th visible item.
        int idx = evt.data.usb_index;
        if (ui_screen_state != SCREEN_SHOPPING_LIST) {
          Serial.println("[USB] del ignored (not on shopping list screen)");
        } else if (idx < 0 || idx >= g_active.count) {
          Serial.printf("[USB] del %d -> out of range (count=%d)\n", idx, g_active.count);
        } else {
          const char* del_id = shopping_list_delete_index(idx);
          ui_lvgl_tick();
          resetActivityTimer();
          Serial.printf("[USB] del %d -> %s\n", idx, del_id[0] ? del_id : "(no id)");
        }
        processed_anything = true;
      } else if (evt.type == EVT_USB_DELTOUCH) {
        // USB 'deltouch' — full touch-path delete. Open the overlay for the
        // current selection (the same call the tap path makes), then schedule
        // the synthesized tap at the center of the real Delete button for a
        // later iteration (deferred check above), exercising the derived
        // hitboxes end-to-end. Assertions key off the
        // "[SHOPPING_LIST] overlay tap ... -> delete" classification log.
        if (ui_screen_state != SCREEN_SHOPPING_LIST) {
          Serial.println("[USB] deltouch ignored (not on shopping list screen)");
        } else if (g_active.count == 0) {
          Serial.println("[USB] deltouch ignored (empty list)");
        } else {
          if (!shopping_list_overlay_visible) {
            shopping_list_show_overlay();
          }
          Serial.printf("[USB] deltouch -> overlay@%d\n", shopping_list_scroll_idx);
          usb_deltouch_tap_due_ms = millis() + 300;
          ui_lvgl_tick();
          resetActivityTimer();
        }
        processed_anything = true;
      } else if (evt.type == EVT_USB_HOME) {
        // USB 'home' — return to the main menu.
        show_ship_main_menu();
        resetActivityTimer();
        Serial.println("[USB] home -> main menu");
        processed_anything = true;
      } else if (evt.type == EVT_TOGGLE_BUTTONS) {
        // Toggle menu buttons visibility
        buttons_visible = !buttons_visible;
        
        if (buttons_visible) {
          // Show buttons
          if (delete_menu != NULL && g_active.count > 0) {
            lv_obj_clear_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
          }
          if (menu_menu != NULL) {
            lv_obj_clear_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
          }
          Serial.println("[UI] Showing menu buttons");
        } else {
          // Hide buttons
          if (delete_menu != NULL) {
            lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
          }
          if (menu_menu != NULL) {
            lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
          }
          Serial.println("[UI] Hiding menu buttons");
        }
        
        processed_anything = true;
      }
    }
    
    // Only call lv_timer_handler() if we didn't just process a scroll event
    if (!processed_anything) {
      ui_lvgl_tick();
      example_lvgl_unlock();
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      example_lvgl_unlock();
    }
    unsigned long now_ms = millis();
    if ((now_ms - last_ui_heartbeat_ms) >= 500) {
      ui_heartbeat_counter++;
      HALO_CHATTY_PRINTF("[UI_HEARTBEAT] now=%lu sleep=%d transition=%d hb=%lu lvgl_calls=%lu backlight=%d panel_on=%d refresh_ok=%lu refresh_timeout=%lu refresh_retry=%lu\n",
                    now_ms,
                    g_in_light_sleep ? 1 : 0,
                    g_sleep_transition ? 1 : 0,
                    (unsigned long)ui_heartbeat_counter,
                    (unsigned long)lvgl_timer_calls,
                    g_backlight_duty,
                    g_panel_enabled ? 1 : 0,
                    (unsigned long)refresh_success_count,
                    (unsigned long)refresh_timeout_count,
                    (unsigned long)refresh_retry_count);
      last_ui_heartbeat_ms = now_ms;
    }
  }
}

#endif // LCD_UI_TASK_H

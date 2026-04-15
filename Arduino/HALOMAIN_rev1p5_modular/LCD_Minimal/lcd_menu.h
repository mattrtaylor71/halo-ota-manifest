/*
 * lcd_menu.h
 *
 * Menu display, menu button handlers, delete button handler,
 * list state application (pure data, no LVGL), scroll handling,
 * and touch/knob ISR callbacks.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 8.
 *
 * Prerequisites (must be declared before #include "lcd_menu.h"):
 *   - All LVGL UI objects (menu buttons, labels, hitboxes)
 *   - All menu state globals (menu_mode, menu_items_*, etc.)
 *   - All list state (g_active, g_pending, app_state_mutex)
 *   - lcd_uart.h (uart_send_input_message, uart_tx_queue)
 */

#ifndef LCD_MENU_H
#define LCD_MENU_H

static void set_menu_mode(menu_mode_t mode) {
  menu_mode = mode;
  if (menu_mode == MENU_MODE_SETTINGS) {
    menu_items_current = menu_items_settings;
    menu_item_count = MENU_SETTINGS_ITEM_COUNT;
  } else {
    menu_items_current = menu_items_main;
    menu_item_count = MENU_MAIN_ITEM_COUNT;
  }
  
  for (int i = 0; i < MENU_MAX_ITEMS; i++) {
    if (menu_item_labels[i] != NULL) {
      if (i < menu_item_count) {
        lv_label_set_text(menu_item_labels[i], menu_items_current[i]);
        lv_obj_clear_flag(menu_item_labels[i], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_label_set_text(menu_item_labels[i], "");
        lv_obj_add_flag(menu_item_labels[i], LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
}

static void update_menu_display() {
  if (menu_list_container == NULL) return;
  
  // Safety check: ensure selected index is valid
  if (menu_selected_index < 0 || menu_selected_index >= menu_item_count) {
    Serial.printf("[MENU] Invalid selected_index: %d (max: %d) - clamping to 0\n", menu_selected_index, menu_item_count - 1);
    menu_selected_index = 0;
  }
  
  // Update all menu item labels with highlighting for selected item
  for (int i = 0; i < MENU_MAX_ITEMS; i++) {
    if (menu_item_labels[i] != NULL) {
      if (i >= menu_item_count) {
        continue;
      }
      if (i == menu_selected_index) {
        // Selected item - brighter white or bold
        lv_obj_set_style_text_color(menu_item_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_opa(menu_item_labels[i], LV_OPA_COVER, LV_PART_MAIN);
        // Could add background highlight or make font larger/bolder
        // For now, we'll add a subtle background highlight
        lv_obj_set_style_bg_color(menu_item_labels[i], lv_color_hex(0x003D7A), LV_PART_MAIN);  // Darker blue highlight
        lv_obj_set_style_bg_opa(menu_item_labels[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(menu_item_labels[i], 8, LV_PART_MAIN);  // Rounded corners
        lv_obj_set_style_pad_all(menu_item_labels[i], 8, LV_PART_MAIN);  // Padding for highlight
      } else {
        // Unselected item - white text, no background
        lv_obj_set_style_text_color(menu_item_labels[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_opa(menu_item_labels[i], LV_OPA_70, LV_PART_MAIN);  // Slightly transparent
        lv_obj_set_style_bg_opa(menu_item_labels[i], LV_OPA_TRANSP, LV_PART_MAIN);  // No background
        lv_obj_set_style_pad_all(menu_item_labels[i], 0, LV_PART_MAIN);  // No padding
      }
    }
  }
}

static void show_menu_screen() {
  Serial.println("[MENU] Showing menu screen");

  // Shopping-list UI is retired for the ship flow.
  if (menu_screen != NULL) {
    lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  }
  if (list_container != NULL) {
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  menu_screen_visible = false;
  Serial.println("[MENU] legacy menu disabled");
  return;
  
  // Safety check: ensure menu UI is initialized
  if (menu_screen == NULL || menu_list_container == NULL) {
    Serial.println("[MENU] ERROR: Menu UI not initialized! Cannot show menu.");
    return;
  }
  
  menu_screen_visible = true;
  set_menu_mode(MENU_MODE_MAIN);
  menu_selected_index = 0;  // Reset to first item
  
  // Clamp selected index to valid range
  if (menu_selected_index < 0 || menu_selected_index >= menu_item_count) {
    menu_selected_index = 0;
  }
  
  update_menu_display();
  
  // Set cooldown to prevent the touch that opened the menu from also selecting a menu item
  menu_cooldown_until = millis() + 300;  // 300ms cooldown
  
  // Hide all other screens
  if (list_container != NULL) {
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  if (meal_result_screen != NULL) {
    lv_obj_add_flag(meal_result_screen, LV_OBJ_FLAG_HIDDEN);
  }
  if (status_screen != NULL) {
    lv_obj_add_flag(status_screen, LV_OBJ_FLAG_HIDDEN);
  }
  
  // Show menu screen
  lv_obj_clear_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  
  lv_timer_handler();  // Force immediate render
}

static void hide_menu_screen() {
  Serial.println("[MENU] Hiding menu screen");
  menu_screen_visible = false;
  menu_cooldown_until = 0;  // Reset cooldown when menu is hidden
  
  if (menu_screen != NULL) {
    lv_obj_add_flag(menu_screen, LV_OBJ_FLAG_HIDDEN);
  }

  if (list_container != NULL) {
    lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  return;
  
  // Show list again
  if (list_container != NULL && g_active.count > 0) {
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  
  lv_timer_handler();  // Force immediate render
}

// ── Menu Button Handler ────────────────────────────────────────────
static void menu_btn_event_handler(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    Serial.println("[MENU] Menu button clicked - showing menu screen");
    
    // Hide buttons immediately after click
    buttons_visible = false;
    if (delete_menu != NULL) {
      lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
    }
    if (menu_menu != NULL) {
      lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Show menu screen
    show_menu_screen();
    
    // Set cooldown to prevent touch toggle from re-showing buttons immediately
    delete_cooldown_until = millis() + 500;  // 500ms cooldown
    
    // Reset activity timer (user is interacting)
    resetActivityTimer();
  }
}

static void ui_update_list(const app_state_t *s) {
  if (list_container == NULL) {
    Serial.println("[UI] ERROR: list_container is NULL!");
    return;
  }

  // Shopping-list UI is retired; keep the legacy list surface hidden.
  if (empty_label != NULL) {
    lv_obj_del(empty_label);
    empty_label = NULL;
  }
  lv_obj_add_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  return;
  
  const int NUM_ROWS = 7;  // Number of visible rows
  const int CENTER_ROW = NUM_ROWS / 2;  // Middle row (index 3)
  const int ROW_HEIGHT = 42;  // Fixed height per row
  const int START_Y = -(ROW_HEIGHT * (NUM_ROWS - 1)) / 2;  // Center block vertically
  
  // Ensure we have enough labels (use first NUM_ROWS of list_labels array)
  static bool labels_created = false;
  if (!labels_created) {
    Serial.println("[UI] Creating list labels...");
    for (int row = 0; row < NUM_ROWS && row < MAX_LIST_ITEMS; row++) {
      if (list_labels[row] == NULL) {
        list_labels[row] = lv_label_create(list_container);
        lv_obj_set_width(list_labels[row], 280);  // Fixed width
        lv_label_set_long_mode(list_labels[row], LV_LABEL_LONG_WRAP);  // Let LVGL handle wrapping
        lv_obj_set_style_text_align(list_labels[row], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        int y = START_Y + row * ROW_HEIGHT;
        lv_obj_align(list_labels[row], LV_ALIGN_CENTER, 0, y);
        // Hide labels initially - they'll be shown when we have data
        lv_obj_add_flag(list_labels[row], LV_OBJ_FLAG_HIDDEN);
      }
    }
    labels_created = true;
    Serial.println("[UI] List labels created");
  }
  
  // Hide empty label if items exist
  if (s->count > 0 && empty_label != NULL) {
    lv_obj_del(empty_label);
    empty_label = NULL;
  }
  
  // Show empty message if no items
  if (s->count == 0) {
    for (int row = 0; row < NUM_ROWS; row++) {
      if (list_labels[row] != NULL) {
        lv_obj_add_flag(list_labels[row], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (empty_label == NULL) {
      empty_label = lv_label_create(list_container);
      lv_label_set_text(empty_label, "Shopping list is empty");
      lv_obj_set_style_text_color(empty_label, lv_color_hex(0x6B7280), LV_PART_MAIN);  // Text Muted Dark
      lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
    }
    return;
  }
  
  // Update each row - simple mapping: logical_index = selected_index + (row - CENTER_ROW)
  // When selected_index = 0, item 0 appears in center row (row 3), but we highlight it
  int visible_count = 0;
  for (int row = 0; row < NUM_ROWS; row++) {
    int logical_index = s->selected_index + (row - CENTER_ROW);
    
    if (logical_index < 0 || logical_index >= s->count) {
      // Out of bounds - hide this row
      if (list_labels[row] != NULL) {
        lv_obj_add_flag(list_labels[row], LV_OBJ_FLAG_HIDDEN);
      }
    } else {
      // Show this row with item
      if (list_labels[row] != NULL) {
        lv_obj_clear_flag(list_labels[row], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(list_labels[row], s->items[logical_index]);
        
        // Reset font styles
        lv_obj_set_style_text_letter_space(list_labels[row], 0, LV_PART_MAIN);
        
        // Highlight the selected item
        bool is_selected = (logical_index == s->selected_index);
        
        if (is_selected) {
          lv_obj_set_style_text_font(list_labels[row], &lv_font_montserrat_20, LV_PART_MAIN);
          lv_obj_set_style_text_color(list_labels[row], lv_color_hex(0xF9FAFB), LV_PART_MAIN);  // Text Primary Dark
        } else {
          lv_obj_set_style_text_font(list_labels[row], &lv_font_montserrat_16, LV_PART_MAIN);
          lv_obj_set_style_text_color(list_labels[row], lv_color_hex(0x9CA3AF), LV_PART_MAIN);  // Text Secondary Dark
        }
        visible_count++;
      }
    }
  }
  
  Serial.printf("[UI] Updated list display: count=%d, selected=%d, visible_rows=%d\n", 
                s->count, s->selected_index, visible_count);
}

// CRITICAL: This function must ONLY be called from UI task context!
// It does NOT take the mutex - caller must ensure mutex is held if needed
static void ui_refresh_from_state(const app_state_t *s) {
  if (g_sleep_transition) {
    return;
  }
  if (s == NULL) {
    Serial.println("[UI] ERROR: ui_refresh_from_state called with NULL pointer!");
    return;
  }
  
  // Safety check: validate count
  if (s->count < 0 || s->count > MAX_LIST_ITEMS) {
    Serial.printf("[UI] ERROR: Invalid count in ui_refresh_from_state: %d\n", s->count);
    return;
  }
  // Update list display
  ui_update_list(s);
  
  // Ensure list container is visible when we have items
  if (s->count > 0 && list_container != NULL) {
    lv_obj_clear_flag(list_container, LV_OBJ_FLAG_HIDDEN);
  }
  
  // Ensure loading screen is hidden when we have items
  if (s->count > 0 && loading_screen != NULL) {
    lv_obj_add_flag(loading_screen, LV_OBJ_FLAG_HIDDEN);
  }
}

// ── Delete Button Handler ────────────────────────────────────────────
// CRITICAL: This runs from LVGL event context - must send event to UI task for state updates
static void delete_item_btn_handler(lv_event_t * e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_CLICKED) {
    // Get selected item ID from g_active
    char item_id_to_delete[64] = {0};
    int local_selected = -1;
    int local_count = 0;
    
    if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      local_selected = g_active.selected_index;
      local_count = g_active.count;
      
      if (local_selected >= 0 && local_selected < local_count && local_selected < MAX_LIST_ITEMS) {
        // Copy the item ID (don't use pointer - it might become invalid)
        strncpy(item_id_to_delete, g_active.item_ids[local_selected], sizeof(item_id_to_delete) - 1);
        item_id_to_delete[sizeof(item_id_to_delete) - 1] = '\0';
        
        // Add to deleted items tracking (if ID exists and not already tracked)
        if (strlen(item_id_to_delete) > 0 && deleted_item_count < MAX_DELETED_ITEMS) {
          bool already_tracked = false;
          for (int i = 0; i < deleted_item_count; i++) {
            if (strcmp(item_id_to_delete, deleted_item_ids[i]) == 0) {
              already_tracked = true;
              break;
            }
          }
          if (!already_tracked) {
            strncpy(deleted_item_ids[deleted_item_count], item_id_to_delete, 63);
            deleted_item_ids[deleted_item_count][63] = '\0';
            deleted_item_count++;
            Serial.printf("[DELETE] Added ID to deleted tracking: %s (total tracked: %d)\n", 
                          item_id_to_delete, deleted_item_count);
          }
        }
        
        Serial.printf("[DELETE] Delete button pressed - item at index %d, ID: %s\n", 
                      local_selected, item_id_to_delete);
      } else {
        Serial.printf("[DELETE] Invalid selected index: %d (count: %d)\n", local_selected, local_count);
        xSemaphoreGive(app_state_mutex);
        return;
      }
      xSemaphoreGive(app_state_mutex);
    } else {
      Serial.println("[DELETE] Failed to acquire mutex");
      return;
    }
    
    // Optimistically remove item from g_active immediately
    if (app_state_mutex != NULL && xSemaphoreTake(app_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (local_selected < g_active.count) {
        // Shift remaining items up
        for (int i = local_selected; i < g_active.count - 1; i++) {
          strncpy(g_active.items[i], g_active.items[i + 1], 64);
          g_active.items[i][63] = '\0';
          strncpy(g_active.item_ids[i], g_active.item_ids[i + 1], 64);
          g_active.item_ids[i][63] = '\0';
        }
        // Clear last item
        g_active.items[g_active.count - 1][0] = '\0';
        g_active.item_ids[g_active.count - 1][0] = '\0';
        g_active.count--;
        
        // Adjust selected index if needed
        if (g_active.selected_index >= g_active.count && g_active.count > 0) {
          g_active.selected_index = g_active.count - 1;
        } else if (g_active.count == 0) {
          g_active.selected_index = -1;
        }
        
        // CRITICAL: Update UI IMMEDIATELY for instant visual feedback
        // This is safe because we're in an LVGL event callback context
        ui_refresh_from_state(&g_active);
        
        // Hide buttons IMMEDIATELY after deletion (before any other processing)
        buttons_visible = false;
        if (delete_menu != NULL) {
          lv_obj_add_flag(delete_menu, LV_OBJ_FLAG_HIDDEN);
        }
        if (menu_menu != NULL) {
          lv_obj_add_flag(menu_menu, LV_OBJ_FLAG_HIDDEN);
        }
        
        // Set cooldown to prevent touch toggle from re-showing buttons immediately
        delete_cooldown_until = millis() + 500;  // 500ms cooldown
        
        // Force LVGL to render immediately (including button hiding)
        lv_timer_handler();
        
        // Save updated list to storage (non-blocking, happens after UI update)
        save_list_to_storage(&g_active);
      }
      xSemaphoreGive(app_state_mutex);
      
      // Also send event to UI task (backup, but UI is already updated above)
      if (app_event_queue != NULL) {
        app_event_t evt = {EVT_LIST_REPLACED, {.new_count = g_active.count}};
        xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(10));
      }
    }
    
    // Send delete request to Sense board
    tx_msg_t tx_msg = {};
    strncpy(tx_msg.type, "INPUT_DELETE", sizeof(tx_msg.type) - 1);
    strncpy(tx_msg.id, item_id_to_delete, sizeof(tx_msg.id) - 1);
    tx_msg.has_id = true;
    if (uart_tx_queue != NULL) {
      xQueueSend(uart_tx_queue, &tx_msg, pdMS_TO_TICKS(10));
      Serial.printf("[DELETE] Sent delete request to Sense board for ID: %s\n", item_id_to_delete);
    }
    
    Serial.println("[DELETE] Item removed from local state, buttons hidden, UI refreshed");
  }
}

// ── Apply Event to State (Pure Data, No LVGL) ──────────────────────
// Returns true if pull-to-refresh was triggered
static bool apply_event_to_state(app_state_t *s, const app_event_t *evt) {
  if (s == NULL || evt == NULL) return false;
  
  if (evt->type == EVT_SCROLL_DELTA) {
    // Mark that user has scrolled
    user_has_scrolled = true;
    
    if (s->count > 0) {
      int old_index = s->selected_index;
      s->selected_index += evt->data.scroll_delta;
      
      // Clamp to valid range
      if (s->selected_index < 0) {
        s->selected_index = 0;
      }
      if (s->selected_index >= s->count) {
        s->selected_index = s->count - 1;
      }
      
    }
  }
  
  return false;  // No refresh triggered
}

// ── Scroll Handling ────────────────────────────────────────────────
// CRITICAL: These callbacks run in interrupt context - NO LVGL, NO UART TX!
// Only post events to queues, tasks will handle LVGL and UART
static void knob_left_cb(void *arg, void *data) {
  if (g_ship_ota_wake_window && !g_ui_initialized) {
    g_ship_ota_user_input = true;
    return;
  }
  if (provisioning_input_locked()) {
    return;
  }
  // Ignore scrolls during brief wake-up period (150ms)
  if (millis() < scroll_ignore_until) {
    return;
  }
  user_activity_since_sleep = true;
  last_scroll_activity_ms = millis();
  scroll_activity_pending = true;
  
  // Post to UI event queue for immediate UI feedback (optimistic update)
  app_event_t evt = {EVT_SCROLL_DELTA, {.scroll_delta = -1}};
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (app_event_queue != NULL) {
    xQueueSendFromISR(app_event_queue, &evt, &xHigherPriorityTaskWoken);
  }

  // Haptic tick (queued to UI task)
  app_event_t h_evt = {};
  h_evt.type = EVT_HAPTIC_TICK;
  if (app_event_queue != NULL) {
    xQueueSendFromISR(app_event_queue, &h_evt, &xHigherPriorityTaskWoken);
  }
  
  // Queue UART TX message (NOT sent here - uart_task will send it)
  if (!refresh_request_pending && !ui_busy) {
    tx_msg_t tx_msg = {};
    strncpy(tx_msg.type, "INPUT_SCROLL", sizeof(tx_msg.type) - 1);
    tx_msg.delta = -1;
    tx_msg.has_delta = true;
    if (uart_tx_queue != NULL) {
      xQueueSendFromISR(uart_tx_queue, &tx_msg, &xHigherPriorityTaskWoken);
    }
  }
  
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

static void knob_right_cb(void *arg, void *data) {
  if (g_ship_ota_wake_window && !g_ui_initialized) {
    g_ship_ota_user_input = true;
    return;
  }
  if (provisioning_input_locked()) {
    return;
  }
  // Ignore scrolls during brief wake-up period (150ms)
  if (millis() < scroll_ignore_until) {
    return;
  }
  user_activity_since_sleep = true;
  last_scroll_activity_ms = millis();
  scroll_activity_pending = true;
  
  // Post to UI event queue for immediate UI feedback (optimistic update)
  app_event_t evt = {EVT_SCROLL_DELTA, {.scroll_delta = 1}};
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (app_event_queue != NULL) {
    xQueueSendFromISR(app_event_queue, &evt, &xHigherPriorityTaskWoken);
  }

  // Haptic tick (queued to UI task)
  app_event_t h_evt = {};
  h_evt.type = EVT_HAPTIC_TICK;
  if (app_event_queue != NULL) {
    xQueueSendFromISR(app_event_queue, &h_evt, &xHigherPriorityTaskWoken);
  }
  
  // Queue UART TX message (NOT sent here - uart_task will send it)
  if (!refresh_request_pending && !ui_busy) {
    tx_msg_t tx_msg = {};
    strncpy(tx_msg.type, "INPUT_SCROLL", sizeof(tx_msg.type) - 1);
    tx_msg.delta = 1;
    tx_msg.has_delta = true;
    if (uart_tx_queue != NULL) {
      xQueueSendFromISR(uart_tx_queue, &tx_msg, &xHigherPriorityTaskWoken);
    }
  }
  
  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

#endif // LCD_MENU_H

/*
 * lcd_persist.h
 *
 * Persistent storage: NVS-backed shopping list save/load,
 * reset reason labels, and boot wakeup diagnostics.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 7.
 *
 * Prerequisites (must be declared before #include "lcd_persist.h"):
 *   - Preferences.h, ArduinoJson.h, esp_sleep.h
 *   - app_state_t, MAX_LIST_ITEMS
 *   - HALO_WAKE_GPIO, HALO_BOARD_NAME
 *   - log_ext1_wakeup_status()
 */

#ifndef LCD_PERSIST_H
#define LCD_PERSIST_H

static Preferences preferences;
static const char* PREF_NAMESPACE = "shopping_list";
static const char* PREF_KEY_COUNT = "count";
static const char* PREF_KEY_SELECTED = "selected";
static const char* PREF_KEY_ITEMS = "items";  // JSON string of items
static const char* PREF_KEY_IDS = "ids";      // JSON string of item IDs

// Save shopping list to persistent storage
static void save_list_to_storage(const app_state_t *s) {
  if (s == NULL) {
    Serial.println("✗ Cannot save list: app_state_t is NULL");
    return;
  }
  
  if (!preferences.begin(PREF_NAMESPACE, false)) {
    Serial.println("✗ Failed to open preferences for saving");
    return;
  }
  
  // Create JSON document to store items and IDs
  StaticJsonDocument<8192> doc;  // 8KB should be enough
  JsonArray items_array = doc.createNestedArray("items");
  JsonArray ids_array = doc.createNestedArray("ids");
  
  for (int i = 0; i < s->count && i < MAX_LIST_ITEMS; i++) {
    items_array.add(s->items[i]);
    ids_array.add(s->item_ids[i]);
  }
  
  // Serialize to string
  String json_str;
  serializeJson(doc, json_str);
  
  // Save count, selected index, and JSON string
  bool count_saved = preferences.putInt(PREF_KEY_COUNT, s->count);
  bool selected_saved = preferences.putInt(PREF_KEY_SELECTED, s->selected_index);
  bool items_saved = preferences.putString(PREF_KEY_ITEMS, json_str.c_str());
  
  preferences.end();
  
  if (count_saved && selected_saved && items_saved) {
    Serial.printf("✓ Saved %d items (selected_index=%d) to persistent storage\n", s->count, s->selected_index);
  } else {
    Serial.printf("✗ Failed to save list! count=%d, selected=%d, items=%d\n", 
                  count_saved, selected_saved, items_saved);
  }
}

// Load shopping list from persistent storage
static int load_list_from_storage(app_state_t *s) {
  if (!preferences.begin(PREF_NAMESPACE, true)) {  // Read-only mode
    Serial.println("✗ Failed to open preferences for loading");
    return -1;
  }
  
  int count = preferences.getInt(PREF_KEY_COUNT, -1);
  if (count < 0 || count > MAX_LIST_ITEMS) {
    preferences.end();
    Serial.println("✗ No valid saved list found");
    return -1;
  }
  
  int selected = preferences.getInt(PREF_KEY_SELECTED, 0);
  String json_str = preferences.getString(PREF_KEY_ITEMS, "");
  preferences.end();
  
  if (json_str.length() == 0) {
    Serial.println("✗ Saved list JSON is empty");
    return -1;
  }
  
  // Parse JSON (reduced size to prevent stack overflow)
  // Note: 4096 should be enough for most shopping lists (typically < 50 items)
  StaticJsonDocument<4096> doc;
  DeserializationError error = deserializeJson(doc, json_str);
  
  if (error) {
    Serial.printf("✗ Failed to parse saved list JSON: %s\n", error.c_str());
    return -1;
  }
  
  // Extract arrays
  if (!doc.containsKey("items") || !doc.containsKey("ids")) {
    Serial.println("✗ Saved list JSON missing items or ids");
    return -1;
  }
  
  JsonArray items_array = doc["items"].as<JsonArray>();
  JsonArray ids_array = doc["ids"].as<JsonArray>();
  
  // Clear old items
  for (int i = 0; i < MAX_LIST_ITEMS; i++) {
    s->items[i][0] = '\0';
    s->item_ids[i][0] = '\0';
  }
  
  // Copy items from JSON
  int actual_count = 0;
  for (int i = 0; i < count && i < MAX_LIST_ITEMS && i < (int)items_array.size(); i++) {
    const char* item_text = items_array[i] | "";
    const char* item_id = ids_array[i] | "";
    
    if (item_text != NULL && strlen(item_text) > 0) {
      strncpy(s->items[actual_count], item_text, 63);
      s->items[actual_count][63] = '\0';
      strncpy(s->item_ids[actual_count], item_id, 63);
      s->item_ids[actual_count][63] = '\0';
      actual_count++;
    }
  }
  
  s->count = actual_count;
  // Restore selected index (clamp to valid range)
  if (selected >= 0 && selected < actual_count) {
    s->selected_index = selected;
  } else {
    s->selected_index = (actual_count > 0) ? 0 : -1;
  }
  
  Serial.printf("✓ Loaded %d items (selected_index=%d) from persistent storage\n", 
                actual_count, s->selected_index);
  return actual_count;
}

static const char* reset_reason_label(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "ESP_RST_POWERON";
    case ESP_RST_EXT:
      return "ESP_RST_EXT";
    case ESP_RST_SW:
      return "ESP_RST_SW";
    case ESP_RST_PANIC:
      return "ESP_RST_PANIC";
    case ESP_RST_INT_WDT:
      return "ESP_RST_INT_WDT";
    case ESP_RST_TASK_WDT:
      return "ESP_RST_TASK_WDT";
    case ESP_RST_WDT:
      return "ESP_RST_WDT";
    case ESP_RST_DEEPSLEEP:
      return "ESP_RST_DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "ESP_RST_BROWNOUT";
    case ESP_RST_SDIO:
      return "ESP_RST_SDIO";
    case ESP_RST_UNKNOWN:
    default:
      return "ESP_RST_UNKNOWN";
  }
}

static const char* wake_cause_label(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "UNDEFINED";
    default:
      return "OTHER";
  }
}

static void print_wakeup_diagnostics(const char* board_name) {
  Serial.begin(115200);
  delay(10);
  esp_reset_reason_t reset_reason = esp_reset_reason();
  esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
  int wake_pin_level = gpio_get_level((gpio_num_t)HALO_WAKE_GPIO);
  Serial.printf("[BOOT] board=%s\n", board_name ? board_name : "unknown");
  Serial.printf("[BOOT] reset_reason=%s\n", reset_reason_label(reset_reason));
  Serial.printf("[BOOT] wake_cause=%s\n", wake_cause_label(wake_cause));
  Serial.printf("[BOOT] wake_pin_level=%d\n", wake_pin_level);
  if (wake_cause == ESP_SLEEP_WAKEUP_EXT1) {
    log_ext1_wakeup_status("boot");
  }
}

#endif // LCD_PERSIST_H

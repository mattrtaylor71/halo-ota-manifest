#include "Watchdog.h"
#include "BuildFlags.h"

#include <esp_system.h>

#if __has_include(<esp_task_wdt.h>)
#include <esp_task_wdt.h>
#define WDT_HAS_ESP_TASK_WDT 1
#else
#define WDT_HAS_ESP_TASK_WDT 0
#endif

#if __has_include(<esp_idf_version.h>)
#include <esp_idf_version.h>
#endif

#ifndef WATCHDOG_ENABLED
#define WATCHDOG_ENABLED 1
#endif
#ifndef WATCHDOG_LOG_REGISTRATION
#define WATCHDOG_LOG_REGISTRATION 0
#endif

#if WATCHDOG_ENABLED
namespace {
  struct TaskEntry {
    const char* name;
    uint32_t last_kick_ms;
    uint8_t missed_windows;
    bool critical;
    bool used;
  };

  static const uint8_t kMaxTasks = 12;
  static TaskEntry s_tasks[kMaxTasks];
  static uint8_t s_task_count = 0;
  static uint32_t s_timeout_ms = 0;
  static bool s_started = false;
  static bool s_triggered = false;
  static bool s_use_esp_task_wdt = false;
#if WDT_HAS_ESP_TASK_WDT
  static esp_task_wdt_user_handle_t s_user_handle = nullptr;
#endif

  static void resetHwWdt() {
#if WDT_HAS_ESP_TASK_WDT
    if (s_use_esp_task_wdt) {
      if (s_user_handle) {
        (void)esp_task_wdt_reset_user(s_user_handle);
      } else {
        (void)esp_task_wdt_reset();
      }
    }
#endif
  }

  static int findTask(const char* name) {
    for (uint8_t i = 0; i < s_task_count; ++i) {
      if (s_tasks[i].used && s_tasks[i].name && name && strcmp(s_tasks[i].name, name) == 0) {
        return i;
      }
    }
    return -1;
  }

  static TaskEntry* ensureTask(const char* name) {
    int idx = findTask(name);
    if (idx >= 0) {
      return &s_tasks[idx];
    }
    if (s_task_count >= kMaxTasks || !name) {
      return nullptr;
    }
    TaskEntry& t = s_tasks[s_task_count++];
    t.name = name;
    t.last_kick_ms = millis();
    t.missed_windows = 0;
    t.critical = false;
    t.used = true;
    return &t;
  }

  static void logAndReboot(const char* missed_list) {
    if (s_triggered) {
      return;
    }
    s_triggered = true;
    uint32_t uptime_ms = millis();
    uint32_t free_heap = ESP.getFreeHeap();
    Serial.printf("[WDT] missed=%s uptime_ms=%lu free_heap=%lu\n",
                  missed_list ? missed_list : "",
                  static_cast<unsigned long>(uptime_ms),
                  static_cast<unsigned long>(free_heap));
    Serial.flush();
    delay(50);
    esp_restart();
  }
}  // namespace
#endif  // WATCHDOG_ENABLED

void Watchdog::begin(uint32_t timeout_s) {
#if !WATCHDOG_ENABLED
  (void)timeout_s;
  return;
#else
  s_timeout_ms = timeout_s * 1000UL;
  s_started = true;
  s_triggered = false;

#if WDT_HAS_ESP_TASK_WDT
  s_use_esp_task_wdt = true;
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5)
  esp_task_wdt_config_t config = {};
  config.timeout_ms = (s_timeout_ms * 3UL) + 1000UL;  // allow 2 windows + headroom
  config.idle_core_mask = 0;  // do not subscribe idle tasks
  config.trigger_panic = false;
  esp_err_t st = esp_task_wdt_reconfigure(&config);
  if (st == ESP_ERR_INVALID_STATE) {
    (void)esp_task_wdt_init(&config);
  }
#else
  uint32_t hw_timeout_s = (timeout_s * 3U) + 1U;
  (void)esp_task_wdt_init(hw_timeout_s, false);
#endif
  // Unsubscribe loopTask only if it was registered by Arduino core
  if (esp_task_wdt_status(NULL) == ESP_OK) {
    (void)esp_task_wdt_delete(NULL);
  }
  if (!s_user_handle) {
    (void)esp_task_wdt_add_user("halo_wdt", &s_user_handle);
  }
#else
  s_use_esp_task_wdt = false;
#endif
#endif
}

void Watchdog::registerTask(const char* name) {
#if !WATCHDOG_ENABLED
  (void)name;
  return;
#else
  TaskEntry* t = ensureTask(name);
  if (t) {
    t->last_kick_ms = millis();
    t->missed_windows = 0;
    resetHwWdt();
#if WATCHDOG_LOG_REGISTRATION
    Serial.printf("[WDT] register=%s\n", name ? name : "");
#endif
  }
#endif
}

void Watchdog::kick(const char* name) {
#if !WATCHDOG_ENABLED
  (void)name;
  return;
#else
  TaskEntry* t = ensureTask(name);
  if (t) {
    t->last_kick_ms = millis();
    t->missed_windows = 0;
    resetHwWdt();
  }
#endif
}

void Watchdog::markCritical(const char* name, bool critical) {
#if !WATCHDOG_ENABLED
  (void)name;
  (void)critical;
  return;
#else
  TaskEntry* t = ensureTask(name);
  if (t) {
    t->critical = critical;
  }
#endif
}

void Watchdog::tick() {
#if !WATCHDOG_ENABLED
  return;
#else
  if (!s_started) {
    return;
  }
  if (s_use_esp_task_wdt) {
    resetHwWdt();
  }

  uint32_t now_ms = millis();
  char missed_buf[128];
  missed_buf[0] = '\0';
  size_t remaining = sizeof(missed_buf) - 1;

  for (uint8_t i = 0; i < s_task_count; ++i) {
    TaskEntry& t = s_tasks[i];
    if (!t.used || !t.critical) {
      continue;
    }
    uint32_t elapsed = now_ms - t.last_kick_ms;
    if (elapsed <= s_timeout_ms) {
      t.missed_windows = 0;
    } else {
      uint32_t windows = elapsed / s_timeout_ms;
      t.missed_windows = (windows >= 2) ? 2 : 1;
    }
    if (t.missed_windows >= 2) {
      const char* name = t.name ? t.name : "unknown";
      size_t name_len = strlen(name);
      if (missed_buf[0] != '\0' && remaining > 1) {
        strncat(missed_buf, ",", remaining);
        remaining = (remaining > 0) ? remaining - 1 : 0;
      }
      if (remaining > 0) {
        strncat(missed_buf, name, remaining);
        remaining = (remaining > name_len) ? remaining - name_len : 0;
      }
    }
  }

  if (missed_buf[0] != '\0') {
    logAndReboot(missed_buf);
  }
#endif
}

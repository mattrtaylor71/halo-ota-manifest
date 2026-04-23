/*
 * lcd_diag.h
 *
 * WiFi diagnostic storage for LCD board.
 * - NVS ring buffer: 20 per-cycle WiFi summaries (persistent)
 * - RAM ring buffer: 50 individual WiFi events (volatile)
 * - USB serial command interface helpers
 */

#ifndef LCD_DIAG_H
#define LCD_DIAG_H

#include <Preferences.h>

// ── NVS WiFi Summary Ring Buffer ───────────────────────────────────

static const int DIAG_NVS_SLOTS = 20;
static const char* DIAG_NVS_NS = "wifi_diag";

// Store a WiFi diagnostic summary in NVS ring buffer
static void diag_store_wifi_summary(const char* json_str) {
    Preferences prefs;
    if (!prefs.begin(DIAG_NVS_NS, false)) {
        Serial.println("[DIAG] NVS open failed");
        return;
    }
    int head = prefs.getInt("head", 0);
    int count = prefs.getInt("count", 0);

    char key[12];
    snprintf(key, sizeof(key), "slot_%d", head);
    prefs.putString(key, json_str);

    head = (head + 1) % DIAG_NVS_SLOTS;
    prefs.putInt("head", head);
    if (count < DIAG_NVS_SLOTS) {
        prefs.putInt("count", count + 1);
    }
    prefs.end();
    Serial.printf("[DIAG] stored wifi summary in slot %d (count=%d)\n",
                  (head - 1 + DIAG_NVS_SLOTS) % DIAG_NVS_SLOTS,
                  count < DIAG_NVS_SLOTS ? count + 1 : DIAG_NVS_SLOTS);
}

// Dump all stored WiFi summaries to a Print output (newest first)
static void diag_dump_wifi_summaries(Print& out) {
    Preferences prefs;
    if (!prefs.begin(DIAG_NVS_NS, true)) {
        out.println("{\"error\":\"NVS open failed\"}");
        return;
    }
    int head = prefs.getInt("head", 0);
    int count = prefs.getInt("count", 0);

    out.printf("=== WiFi Summaries (%d stored) ===\n", count);
    if (count == 0) {
        out.println("(none)");
        prefs.end();
        return;
    }

    for (int i = 0; i < count; i++) {
        // Read newest first
        int idx = (head - 1 - i + DIAG_NVS_SLOTS) % DIAG_NVS_SLOTS;
        char key[12];
        snprintf(key, sizeof(key), "slot_%d", idx);
        String val = prefs.getString(key, "");
        if (val.length() > 0) {
            out.printf("[%d] %s\n", i, val.c_str());
        }
    }
    out.println("=== end ===");
    prefs.end();
}

// Get count of stored summaries
static int diag_get_wifi_summary_count() {
    Preferences prefs;
    if (!prefs.begin(DIAG_NVS_NS, true)) return 0;
    int count = prefs.getInt("count", 0);
    prefs.end();
    return count;
}

// Clear all WiFi diagnostic data from NVS
static void diag_clear_wifi_summaries() {
    Preferences prefs;
    if (!prefs.begin(DIAG_NVS_NS, false)) return;
    prefs.clear();
    prefs.end();
    Serial.println("[DIAG] WiFi summaries cleared");
}

// ── RAM WiFi Event Ring Buffer ─────────────────────────────────────

static const int DIAG_EVENT_SLOTS = 50;

struct DiagWifiEvent {
    uint32_t ts;
    char event[16];
    char label[16];
    int32_t code;
    char detail[16];
};

static DiagWifiEvent diag_wifi_events[DIAG_EVENT_SLOTS];
static int diag_event_head = 0;
static int diag_event_count = 0;

static void diag_record_wifi_event(const char* event, const char* label,
                                   int32_t code, const char* detail) {
    DiagWifiEvent& e = diag_wifi_events[diag_event_head];
    e.ts = millis();
    strncpy(e.event, event ? event : "", sizeof(e.event) - 1);
    e.event[sizeof(e.event) - 1] = '\0';
    strncpy(e.label, label ? label : "", sizeof(e.label) - 1);
    e.label[sizeof(e.label) - 1] = '\0';
    e.code = code;
    strncpy(e.detail, detail ? detail : "", sizeof(e.detail) - 1);
    e.detail[sizeof(e.detail) - 1] = '\0';

    diag_event_head = (diag_event_head + 1) % DIAG_EVENT_SLOTS;
    if (diag_event_count < DIAG_EVENT_SLOTS) {
        diag_event_count++;
    }
}

static void diag_dump_wifi_events(Print& out) {
    out.printf("=== WiFi Events (%d stored) ===\n", diag_event_count);
    if (diag_event_count == 0) {
        out.println("(none)");
        return;
    }
    for (int i = 0; i < diag_event_count; i++) {
        int idx = (diag_event_head - diag_event_count + i + DIAG_EVENT_SLOTS) % DIAG_EVENT_SLOTS;
        DiagWifiEvent& e = diag_wifi_events[idx];
        out.printf("[%d] ts=%lu event=%s label=%s code=%ld detail=%s\n",
                   i, (unsigned long)e.ts, e.event, e.label,
                   (long)e.code, e.detail);
    }
    out.println("=== end ===");
}

// ── USB Serial Command Dispatcher ──────────────────────────────────

static char diag_cmd_buf[64];
static int diag_cmd_len = 0;

// Call from uart_task loop when Serial.available()
static void diag_process_serial_byte(uint8_t b) {
    if (b == '\n' || b == '\r') {
        if (diag_cmd_len == 0) return;
        diag_cmd_buf[diag_cmd_len] = '\0';

        // Trim whitespace
        char* cmd = diag_cmd_buf;
        while (*cmd == ' ') cmd++;

        if (strcmp(cmd, "wifi") == 0) {
            diag_dump_wifi_summaries(Serial);
        } else if (strcmp(cmd, "events") == 0) {
            diag_dump_wifi_events(Serial);
        } else if (strcmp(cmd, "status") == 0) {
            Serial.printf("=== LCD Status ===\n");
            Serial.printf("uptime_ms: %lu\n", (unsigned long)millis());
            Serial.printf("free_heap: %lu\n", (unsigned long)ESP.getFreeHeap());
            Serial.printf("free_psram: %lu\n", (unsigned long)ESP.getFreePsram());
            Serial.printf("wifi_summaries: %d\n", diag_get_wifi_summary_count());
            Serial.printf("wifi_events: %d\n", diag_event_count);
            Serial.println("=== end ===");
        } else if (strcmp(cmd, "clear") == 0) {
            diag_clear_wifi_summaries();
            Serial.println("WiFi summaries cleared.");
        } else if (strcmp(cmd, "help") == 0) {
            Serial.println("=== HALO LCD Diagnostics ===");
            Serial.println("  wifi    - Dump NVS WiFi summaries (20 wake cycles)");
            Serial.println("  events  - Dump RAM WiFi events (50 recent)");
            Serial.println("  status  - LCD status (uptime, heap, counts)");
            Serial.println("  clear   - Clear NVS WiFi summaries");
            Serial.println("  help    - This message");
            Serial.println("============================");
        } else {
            Serial.printf("Unknown command: '%s' (type 'help')\n", cmd);
        }

        diag_cmd_len = 0;
    } else if (diag_cmd_len < (int)sizeof(diag_cmd_buf) - 1) {
        diag_cmd_buf[diag_cmd_len++] = (char)b;
    }
}

#endif // LCD_DIAG_H

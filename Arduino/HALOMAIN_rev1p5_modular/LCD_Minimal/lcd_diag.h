/*
 * lcd_diag.h
 *
 * WiFi diagnostic storage for LCD board.
 * - NVS ring buffer: 20 per-cycle WiFi summaries (persistent)
 *
 * Zero static RAM -- all functions use stack-allocated Preferences objects.
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

#endif // LCD_DIAG_H

/*
 * lcd_diag.h
 *
 * WiFi diagnostic storage for LCD board.
 * - NVS ring buffer: 20 per-cycle WiFi summaries (persistent)
 *
 * Diagnostic admission and cleanup share the bounded LCD storage policy.
 */

#ifndef LCD_DIAG_H
#define LCD_DIAG_H

#include <Preferences.h>
#include "lcd_nvs_capacity.h"

// ── NVS WiFi Summary Ring Buffer ───────────────────────────────────

static const int DIAG_NVS_SLOTS = 20;
static const char* DIAG_NVS_NS = "wifi_diag";

// Store a WiFi diagnostic summary in NVS ring buffer
static void diag_store_wifi_summary(const char* json_str) {
    const bool stored=lcd_nvs_store_diagnostic(DIAG_NVS_NS,json_str);
    Serial.printf("[DIAG] persistence verified=%d\n",stored?1:0);
}

// Dump all stored WiFi summaries to a Print output (reverse stored position order)
static void diag_dump_wifi_summaries(Print& out) {
    Preferences prefs;
    if (!prefs.begin(DIAG_NVS_NS, true)) {
        out.println("{\"error\":\"NVS open failed\"}");
        return;
    }
    int head = prefs.getType("head") == PT_I32 ? prefs.getInt("head", -1) : -1;
    int count = prefs.getType("count") == PT_I32 ? prefs.getInt("count", -1) : -1;

    if(head<0 || head>=20 || count<0 || count>20){
        out.println("{\"error\":\"invalid typed ring indices\"}");prefs.end();return;
    }
    out.printf("=== WiFi Summaries (%d stored) ===\n", count);
    if (count == 0) {
        out.println("(none)");
        prefs.end();
        return;
    }

    for (int i = 0; i < count; i++) {
        // Read stored positions backward; partial writes do not prove chronology.
        int idx = (head - 1 - i + DIAG_NVS_SLOTS) % DIAG_NVS_SLOTS;
        char key[12];
        snprintf(key, sizeof(key), "slot_%d", idx);
        String val = prefs.getType(key)==PT_STR ? prefs.getString(key, "") : String("");
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
    if (!prefs.begin(DIAG_NVS_NS, true)) return -1;
    int count = prefs.getType("count") == PT_I32 ? prefs.getInt("count", -1) : -1;
    prefs.end();
    return count>=0 && count<=20 ? count : -1;
}

// Clear all WiFi diagnostic data from NVS
static void diag_clear_wifi_summaries() {
    const bool cleared=lcd_nvs_clear_diagnostic(DIAG_NVS_NS);
    Serial.printf("[DIAG] diagnostic_clear_verified=%u\n",cleared?1:0);
}

#endif // LCD_DIAG_H

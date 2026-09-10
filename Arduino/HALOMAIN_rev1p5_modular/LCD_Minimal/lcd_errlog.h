/*
 * lcd_errlog.h
 *
 * Error log persistence for LCD board.
 * - NVS ring buffer: 20 error entries (persistent across reboots)
 *
 * Diagnostic admission and cleanup share the bounded LCD storage policy.
 */

#ifndef LCD_ERRLOG_H
#define LCD_ERRLOG_H

#include <Preferences.h>
#include "lcd_nvs_capacity.h"

// ── NVS Error Log Ring Buffer ─────────────────────────────────────

static const int ERRLOG_NVS_SLOTS = 20;
static const char* ERRLOG_NVS_NS = "err_log";

// Store an error entry in NVS ring buffer
static void errlog_store(const char* json_str,const LcdNvsDeadline& deadline=LcdNvsDeadline()) {
    const bool stored=lcd_nvs_store_diagnostic(ERRLOG_NVS_NS,json_str,deadline);
    Serial.printf("[ERRLOG] persistence verified=%d\n",stored?1:0);
}

// Dump all stored error entries to a Print output (reverse stored position order)
static void errlog_dump(Print& out) {
    Preferences prefs;
    if (!prefs.begin(ERRLOG_NVS_NS, true)) {
        out.println("{\"error\":\"NVS open failed\"}");
        return;
    }
    int head = prefs.getType("head") == PT_I32 ? prefs.getInt("head", -1) : -1;
    int count = prefs.getType("count") == PT_I32 ? prefs.getInt("count", -1) : -1;

    if(head<0 || head>=20 || count<0 || count>20){
        out.println("{\"error\":\"invalid typed ring indices\"}");prefs.end();return;
    }
    out.printf("=== Error Log (%d stored) ===\n", count);
    if (count == 0) {
        out.println("(none)");
        prefs.end();
        return;
    }

    for (int i = 0; i < count; i++) {
        // Read stored positions backward; partial writes do not prove chronology.
        int idx = (head - 1 - i + ERRLOG_NVS_SLOTS) % ERRLOG_NVS_SLOTS;
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

// Get count of stored error entries
static int errlog_count() {
    Preferences prefs;
    if (!prefs.begin(ERRLOG_NVS_NS, true)) return -1;
    int count = prefs.getType("count") == PT_I32 ? prefs.getInt("count", -1) : -1;
    prefs.end();
    return count>=0 && count<=20 ? count : -1;
}

// Read a single entry by reverse index (0 = newest). Returns true if entry exists.
static bool errlog_read_entry(int reverse_index, char* buf, size_t buf_size) {
    Preferences prefs;
    if (!prefs.begin(ERRLOG_NVS_NS, true)) return false;
    int head = prefs.getType("head") == PT_I32 ? prefs.getInt("head", -1) : -1;
    int count = prefs.getType("count") == PT_I32 ? prefs.getInt("count", -1) : -1;
    if (!buf || !buf_size || head<0 || head>=20 || count<0 || count>20 || reverse_index<0 || reverse_index >= count) {
        prefs.end();
        return false;
    }
    int idx = (head - 1 - reverse_index + ERRLOG_NVS_SLOTS) % ERRLOG_NVS_SLOTS;
    char key[12];
    snprintf(key, sizeof(key), "slot_%d", idx);
    String val = prefs.getType(key)==PT_STR ? prefs.getString(key, "") : String("");
    prefs.end();
    if (val.length() == 0 || val.length()>=buf_size) return false;
    strlcpy(buf, val.c_str(), buf_size);
    return true;
}

// Clear all error log data from NVS
static void errlog_clear() {
    const bool cleared=lcd_nvs_clear_diagnostic(ERRLOG_NVS_NS);
    Serial.printf("[ERRLOG] diagnostic_clear_verified=%u\n",cleared?1:0);
}

#endif // LCD_ERRLOG_H

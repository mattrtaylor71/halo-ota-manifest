/*
 * sense_errlog.h
 *
 * Error log persistence for Sense board.
 * - NVS ring buffer: 8 error entries (persistent across reboots + power cycles)
 * - Monotonic sequence numbers for cloud delivery tracking
 * - Collect undelivered entries as JSON for presign piggyback
 *
 * Mirrors LCD's lcd_errlog.h pattern. Zero static RAM — stack-allocated
 * Preferences objects.
 *
 * Prerequisites (must be declared before #include):
 *   - Arduino.h, Preferences.h
 *   - g_sense_boot_count (uint32_t)
 */

#ifndef SENSE_ERRLOG_H
#define SENSE_ERRLOG_H

#include <Preferences.h>

// ── Constants ────────────────────────────────────────────────────────
static const int SENSE_ERRLOG_SLOTS = 8;
static const char* SENSE_ERRLOG_NS = "sense_err";

// Tracks the last seq included in a presign collect, so we can mark
// delivered after presign succeeds.  Only one presign runs at a time
// (serialized behind http_mutex), so a file-scope static is safe.
static uint32_t g_errlog_last_collect_seq = 0;

// ── Store an error entry ─────────────────────────────────────────────

static void sense_errlog_store(const char* area, int32_t code, const char* detail) {
    Preferences prefs;
    if (!prefs.begin(SENSE_ERRLOG_NS, false)) {
        Serial.println("[SENSE_ERRLOG] NVS open failed");
        return;
    }
    int head = prefs.getInt("head", 0);
    int count = prefs.getInt("count", 0);
    uint32_t seq = prefs.getUInt("next_seq", 1);

    // Build compact JSON entry
    char entry[256];
    snprintf(entry, sizeof(entry),
        "{\"seq\":%lu,\"area\":\"%s\",\"code\":%ld,\"detail\":\"%.140s\",\"up\":%lu,\"boot\":%lu}",
        (unsigned long)seq,
        area ? area : "",
        (long)code,
        detail ? detail : "",
        (unsigned long)millis(),
        (unsigned long)g_sense_boot_count);

    char key[12];
    snprintf(key, sizeof(key), "slot_%d", head);
    prefs.putString(key, entry);

    head = (head + 1) % SENSE_ERRLOG_SLOTS;
    prefs.putInt("head", head);
    if (count < SENSE_ERRLOG_SLOTS) {
        prefs.putInt("count", count + 1);
    }
    prefs.putUInt("next_seq", seq + 1);
    prefs.end();

    Serial.printf("[SENSE_ERRLOG] stored seq=%lu area=%s code=%ld slot=%d\n",
                  (unsigned long)seq, area ? area : "", (long)code,
                  (head - 1 + SENSE_ERRLOG_SLOTS) % SENSE_ERRLOG_SLOTS);
}

// ── Collect undelivered entries as JSON array string ──────────────────
// Writes a JSON array like [{"seq":1,...},{"seq":2,...}] into buf.
// Returns the highest seq included, or 0 if none.

static uint32_t sense_errlog_collect_json(char* buf, size_t buf_size, int max_count) {
    buf[0] = '\0';
    Preferences prefs;
    if (!prefs.begin(SENSE_ERRLOG_NS, true)) return 0;

    int head = prefs.getInt("head", 0);
    int count = prefs.getInt("count", 0);
    uint32_t del_seq = prefs.getUInt("del_seq", 0);

    if (count == 0) {
        prefs.end();
        return 0;
    }

    // Collect entries with seq > del_seq, oldest first, up to max_count
    // Temp storage for matched entries
    struct Match { int slot; uint32_t seq; };
    Match matches[8];
    int match_count = 0;

    for (int i = count - 1; i >= 0 && match_count < max_count; i--) {
        int idx = (head - 1 - i + SENSE_ERRLOG_SLOTS) % SENSE_ERRLOG_SLOTS;
        char key[12];
        snprintf(key, sizeof(key), "slot_%d", idx);
        String val = prefs.getString(key, "");
        if (val.length() == 0) continue;

        // Extract seq from the JSON string (format: {"seq":NNN,...)
        uint32_t entry_seq = 0;
        const char* seq_pos = strstr(val.c_str(), "\"seq\":");
        if (seq_pos) {
            entry_seq = (uint32_t)atol(seq_pos + 6);
        }
        if (entry_seq > del_seq) {
            matches[match_count].slot = idx;
            matches[match_count].seq = entry_seq;
            match_count++;
        }
    }
    prefs.end();

    if (match_count == 0) return 0;

    // Build JSON array, oldest first (reverse the matches array)
    size_t pos = 0;
    buf[pos++] = '[';
    uint32_t highest_seq = 0;

    for (int i = match_count - 1; i >= 0; i--) {
        Preferences p2;
        if (!p2.begin(SENSE_ERRLOG_NS, true)) break;
        char key[12];
        snprintf(key, sizeof(key), "slot_%d", matches[i].slot);
        String val = p2.getString(key, "");
        p2.end();

        if (val.length() == 0) continue;
        if (matches[i].seq > highest_seq) highest_seq = matches[i].seq;

        // Check if it fits (entry + comma + closing bracket + null)
        size_t needed = val.length() + 2;
        if (pos + needed >= buf_size) break;

        if (pos > 1) buf[pos++] = ',';
        memcpy(buf + pos, val.c_str(), val.length());
        pos += val.length();
    }
    buf[pos++] = ']';
    buf[pos] = '\0';

    g_errlog_last_collect_seq = highest_seq;
    return highest_seq;
}

// ── Mark errors as delivered ─────────────────────────────────────────

static void sense_errlog_mark_delivered(uint32_t seq) {
    if (seq == 0) return;
    Preferences prefs;
    if (!prefs.begin(SENSE_ERRLOG_NS, false)) return;
    uint32_t cur = prefs.getUInt("del_seq", 0);
    if (seq > cur) {
        prefs.putUInt("del_seq", seq);
        Serial.printf("[SENSE_ERRLOG] delivered up to seq=%lu\n", (unsigned long)seq);
    }
    prefs.end();
}

// ── Dump all entries to serial ───────────────────────────────────────

static void sense_errlog_dump(Print& out) {
    Preferences prefs;
    if (!prefs.begin(SENSE_ERRLOG_NS, true)) {
        out.println("{\"error\":\"NVS open failed\"}");
        return;
    }
    int head = prefs.getInt("head", 0);
    int count = prefs.getInt("count", 0);
    uint32_t next_seq = prefs.getUInt("next_seq", 1);
    uint32_t del_seq = prefs.getUInt("del_seq", 0);

    out.printf("=== Sense Error Log (%d stored, next_seq=%lu, delivered=%lu) ===\n",
               count, (unsigned long)next_seq, (unsigned long)del_seq);
    if (count == 0) {
        out.println("(none)");
        prefs.end();
        return;
    }
    for (int i = 0; i < count; i++) {
        int idx = (head - 1 - i + SENSE_ERRLOG_SLOTS) % SENSE_ERRLOG_SLOTS;
        char key[12];
        snprintf(key, sizeof(key), "slot_%d", idx);
        String val = prefs.getString(key, "");
        if (val.length() > 0) {
            // Mark delivered/pending
            uint32_t entry_seq = 0;
            const char* seq_pos = strstr(val.c_str(), "\"seq\":");
            if (seq_pos) entry_seq = (uint32_t)atol(seq_pos + 6);
            const char* status = (entry_seq > 0 && entry_seq <= del_seq) ? " [delivered]" : " [pending]";
            out.printf("[%d]%s %s\n", i, status, val.c_str());
        }
    }
    out.println("=== end ===");
    prefs.end();
}

// ── Clear all error log data ─────────────────────────────────────────

static void sense_errlog_clear() {
    Preferences prefs;
    if (!prefs.begin(SENSE_ERRLOG_NS, false)) return;
    prefs.clear();
    prefs.end();
    Serial.println("[SENSE_ERRLOG] error log cleared");
}

// ── Count stored entries ─────────────────────────────────────────────

static int sense_errlog_count() {
    Preferences prefs;
    if (!prefs.begin(SENSE_ERRLOG_NS, true)) return 0;
    int count = prefs.getInt("count", 0);
    prefs.end();
    return count;
}

#endif // SENSE_ERRLOG_H

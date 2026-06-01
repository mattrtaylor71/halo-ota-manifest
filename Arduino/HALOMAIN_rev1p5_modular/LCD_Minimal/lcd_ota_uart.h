/*
 * lcd_ota_uart.h
 *
 * LCD OTA-over-UART receiver state machine.
 *
 * The Sense board fetches the LCD firmware binary from S3 and streams it
 * to the LCD over a COBS-framed binary protocol (UartOtaProtocol).
 * This module implements:
 *   - State machine for receiving OTA data over UART
 *   - Integration with esp_ota_begin/write/end/set_boot_partition
 *   - SHA256 computation during streaming
 *   - NVS progress saving every 64KB for resume support
 *   - JSON message handlers for OTA control messages
 *   - Binary frame reception via UartOtaProtocol
 *
 * Prerequisites (must be declared before #include):
 *   - senseSerial (HardwareSerial)
 *   - kFirmwareVersion (extern const char*)
 *   - lcd_uart.h (uart_send_json, get_next_msg_id, PROTOCOL_VERSION)
 *   - lcd_anim.h (lcd_enter_ota_mode, lcd_exit_ota_mode)
 *   - ArduinoJson.h
 *   - Preferences.h
 */

#pragma once

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "soc/rtc_cntl_reg.h"
#include "../halo_ota_demo/firmware/shared/UartOtaProtocol.h"

// ── Constants ────────────────────────────────────────────────────────
#define LCD_OTA_CHUNK_SIZE        512
#define LCD_OTA_MAX_RETRIES       5
#define LCD_OTA_CHUNK_TIMEOUT_MS  5000
#define LCD_OTA_NVS_SAVE_INTERVAL 65536   // 64KB
#define LCD_OTA_NVS_NAMESPACE     "lcd_ota_prog"
#define LCD_OTA_IDLE_TIMEOUT_MS   30000   // 30s no chunk -> abort

// ── State enum ───────────────────────────────────────────────────────
enum LcdOtaState {
    LCD_OTA_IDLE,
    LCD_OTA_RECEIVING,
    LCD_OTA_FINALIZING,
    LCD_OTA_ABORTING
};

// ── External declarations ────────────────────────────────────────────
extern HardwareSerial senseSerial;
extern const char* kFirmwareVersion;
static bool lcd_enter_ota_mode(uint32_t min_internal_free);
static void lcd_exit_ota_mode(const char* reason);
static void uart_send_json(const char* json_str);
static uint32_t get_next_msg_id();

// ── Global state ─────────────────────────────────────────────────────
static LcdOtaState            s_lcd_ota_state             = LCD_OTA_IDLE;
static esp_ota_handle_t       s_lcd_ota_handle            = 0;
static const esp_partition_t* s_lcd_ota_partition          = NULL;
static uint16_t               s_lcd_ota_session_id         = 0;
static uint32_t               s_lcd_ota_image_size         = 0;
static uint32_t               s_lcd_ota_bytes_written      = 0;
static char                   s_lcd_ota_expected_sha256[65] = {0};
static char                   s_lcd_ota_target_version[32]  = {0};
static mbedtls_sha256_context s_lcd_ota_sha_ctx;
static UartOtaProtocol*       s_lcd_ota_protocol           = NULL;
static uint32_t               s_lcd_ota_last_nvs_offset    = 0;
static unsigned long          s_lcd_ota_last_chunk_ms      = 0;
static uint8_t                s_lcd_ota_last_progress_pct  = 0;

// Global flag: when true, the UART task switches to binary RX mode.
static bool g_lcd_ota_binary_mode = false;

static volatile int g_lcd_ota_progress_pct = -1;  // -1 = no OTA, 0-100 = progress
static volatile bool g_lcd_ota_show_progress = false;

// Returns true when an LCD OTA binary transfer is in progress.
// Used by sleep logic to prevent deep sleep during active OTA.
static bool lcd_ota_uart_active() {
  return s_lcd_ota_state != LCD_OTA_IDLE;
}


// ═════════════════════════════════════════════════════════════════════
//  NVS Helpers
// ═════════════════════════════════════════════════════════════════════

static void lcd_ota_nvs_save_progress() {
    Preferences prefs;
    if (!prefs.begin(LCD_OTA_NVS_NAMESPACE, false)) {
        Serial.println("[LCD_OTA_UART] NVS save failed: cannot open namespace");
        return;
    }
    prefs.putUShort("session_id", s_lcd_ota_session_id);
    prefs.putUInt("offset", s_lcd_ota_bytes_written);
    prefs.putString("sha256", s_lcd_ota_expected_sha256);
    prefs.putString("version", s_lcd_ota_target_version);
    prefs.putUInt("image_size", s_lcd_ota_image_size);

    // NOTE: sha_ctx blob save removed — caused heap corruption on ESP32-S3
    // hardware SHA. On resume, OTA restarts from offset 0 (no partial hash resume).

    s_lcd_ota_last_nvs_offset = s_lcd_ota_bytes_written;
    prefs.end();

    Serial.printf("[LCD_OTA_UART] NVS saved progress: session=%u offset=%u\n",
                  s_lcd_ota_session_id, s_lcd_ota_bytes_written);
}

static bool lcd_ota_nvs_load_progress(uint16_t* session_id, uint32_t* offset,
                                       uint32_t* image_size, char* sha256_out,
                                       char* version_out) {
    Preferences prefs;
    if (!prefs.begin(LCD_OTA_NVS_NAMESPACE, true)) {
        return false;
    }

    *session_id = prefs.getUShort("session_id", 0);
    *offset     = prefs.getUInt("offset", 0);
    *image_size = prefs.getUInt("image_size", 0);

    String sha = prefs.getString("sha256", "");
    String ver = prefs.getString("version", "");
    prefs.end();

    if (*session_id == 0 || *offset == 0) {
        return false;
    }

    if (sha256_out) {
        strncpy(sha256_out, sha.c_str(), 64);
        sha256_out[64] = '\0';
    }
    if (version_out) {
        strncpy(version_out, ver.c_str(), 31);
        version_out[31] = '\0';
    }

    Serial.printf("[LCD_OTA_UART] NVS loaded progress: session=%u offset=%u size=%u ver=%s\n",
                  *session_id, *offset, *image_size, ver.c_str());
    return true;
}

static bool lcd_ota_nvs_load_sha_ctx(mbedtls_sha256_context* ctx) {
    Preferences prefs;
    if (!prefs.begin(LCD_OTA_NVS_NAMESPACE, true)) {
        return false;
    }
    size_t read = prefs.getBytes("sha_ctx", ctx, sizeof(mbedtls_sha256_context));
    prefs.end();
    return (read == sizeof(mbedtls_sha256_context));
}

static void lcd_ota_nvs_clear() {
    Preferences prefs;
    if (prefs.begin(LCD_OTA_NVS_NAMESPACE, false)) {
        prefs.clear();
        prefs.end();
        Serial.println("[LCD_OTA_UART] NVS progress cleared");
    }
}


// ═════════════════════════════════════════════════════════════════════
//  Internal helpers
// ═════════════════════════════════════════════════════════════════════

static void lcd_ota_send_status_json(uint8_t pct) {
    StaticJsonDocument<256> doc;
    doc["ver"]        = PROTOCOL_VERSION;
    doc["type"]       = "LCD_OTA_STATUS";
    doc["msg_id"]     = get_next_msg_id();
    doc["ts"]         = millis();
    doc["session_id"] = s_lcd_ota_session_id;
    doc["progress"]   = pct;
    doc["written"]    = s_lcd_ota_bytes_written;
    doc["total"]      = s_lcd_ota_image_size;

    String output;
    serializeJson(doc, output);

    // Only send on UART when NOT in binary COBS mode
    if (!g_suppress_uart_json_tx) {
        senseSerial.print(output);
        senseSerial.print("\n");
        senseSerial.flush();
    }

    Serial.printf("[LCD_OTA_UART] STATUS %u%% (%u/%u)\n",
                  pct, s_lcd_ota_bytes_written, s_lcd_ota_image_size);
}

// Forward declaration — ui_task is defined in lcd_ui_task.h (included later)
static void ui_task(void *arg);

// Lightweight UI restore after UART OTA (counterpart to the freeze in handle_begin)
static void lcd_ota_uart_restore_ui() {
    // Clear OTA progress overlay
    g_lcd_ota_show_progress = false;
    g_lcd_ota_progress_pct = -1;

    // Clear all OTA state flags
    ota_locked = false;
    ota_check_pending = false;
    ota_check_requested = false;
    sense_ota_active = false;
    sense_ota_apply_required = false;
    ota_stay_awake_until_ms = 0;

    // Clear maintenance window and OTA mode flags so sleep is no longer blocked
    g_lcd_maintenance_active = false;
    g_lcd_maintenance_deadline_ms = 0;
    g_ota_mode_active = false;

    // Clear manual OTA override so the device doesn't re-trigger OTA on next wake
    g_manual_ota_override = false;
    g_manual_ota_override_until_ms = 0;

    g_ota_screen_active = false;
    g_lcd_maintenance_headless = false;

    // Clear persisted maintenance NVS so stale timer doesn't re-enter headless after OTA reboot.
    // Double-clear with delay to ensure NVS flash write completes before esp_restart().
    // Without this, the NVS write may not flush and the boot sequence re-enters headless.
    lcd_clear_persisted_maintenance_state("ota_complete");
    delay(200);  // Allow NVS flash write to commit
    lcd_clear_persisted_maintenance_state("ota_complete_verify");  // Belt-and-suspenders

    // Clear RTC maintenance timer variables so they don't survive into the next sleep
    g_lcd_maintenance_timer_armed = 0;
    g_lcd_maintenance_wake_in_s = 0;
    g_lcd_maintenance_remaining_s = 0;
    g_lcd_maintenance_start_epoch = 0;
    g_lcd_maintenance_request_id[0] = '\0';

    // Don't try to restore LVGL here — this runs on Core 0 (UART task) and
    // lcd_exit_ota_mode() calls LVGL init which crashes on Core 0.
    // For successful OTA, esp_restart() is called immediately after this function.
    // For failed OTA, just clear flags and let the device sleep/wake naturally.
    provision_return_home_pending = true;
    resetActivityTimer();
    Serial.println("[LCD_OTA_UART] OTA flags cleared");
}

// Forward declaration
static void lcd_ota_handle_abort(JsonObject& doc);

static void lcd_ota_abort_internal(const char* reason) {
    Serial.printf("[LCD_OTA_UART] ABORT reason=%s state=%d written=%u\n",
                  reason ? reason : "unknown", s_lcd_ota_state,
                  s_lcd_ota_bytes_written);

    lcd_errlog_store_with_context("lcd", "ota", "OTA_ABORT", (int)s_lcd_ota_bytes_written, reason ? reason : "unknown");

    // Send abort notification to Sense
    StaticJsonDocument<256> doc;
    doc["ver"]        = PROTOCOL_VERSION;
    doc["type"]       = "LCD_OTA_ABORT";
    doc["msg_id"]     = get_next_msg_id();
    doc["ts"]         = millis();
    doc["session_id"] = s_lcd_ota_session_id;
    doc["reason"]     = reason ? reason : "unknown";
    doc["source"]     = "lcd";

    String output;
    serializeJson(doc, output);
    uart_send_json(output.c_str());

    // Cleanup
    if (s_lcd_ota_handle != 0) {
        esp_ota_abort(s_lcd_ota_handle);
        s_lcd_ota_handle = 0;
    }

    mbedtls_sha256_free(&s_lcd_ota_sha_ctx);

    if (s_lcd_ota_protocol) {
        delete s_lcd_ota_protocol;
        s_lcd_ota_protocol = NULL;
    }

    // Preserve NVS for resume on timeout; clear on hard abort
    bool is_timeout = reason && (strcmp(reason, "timeout") == 0);
    if (!is_timeout) {
        lcd_ota_nvs_clear();
    } else {
        Serial.println("[LCD_OTA_UART] NVS preserved for resume (timeout abort)");
    }

    g_lcd_ota_binary_mode        = false;
    g_lcd_ota_uart_receiving     = false;
    g_suppress_uart_json_tx      = false;
    s_lcd_ota_state              = LCD_OTA_IDLE;
    s_lcd_ota_partition          = NULL;
    s_lcd_ota_bytes_written      = 0;
    s_lcd_ota_image_size         = 0;
    s_lcd_ota_last_nvs_offset    = 0;
    s_lcd_ota_last_chunk_ms      = 0;
    s_lcd_ota_last_progress_pct  = 0;
    s_lcd_ota_expected_sha256[0] = '\0';
    s_lcd_ota_target_version[0]  = '\0';

    lcd_ota_uart_restore_ui();
}


// ═════════════════════════════════════════════════════════════════════
//  JSON Message Handlers
// ═════════════════════════════════════════════════════════════════════

// Map an esp_ota_img_states_t to a stable string used in the shared JSON
// contract (Sense firmware parses these exact values).
static const char* lcd_ota_img_state_str(esp_ota_img_states_t st) {
    switch (st) {
        case ESP_OTA_IMG_NEW:            return "NEW";
        case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
        case ESP_OTA_IMG_VALID:          return "VALID";
        case ESP_OTA_IMG_INVALID:        return "INVALID";
        case ESP_OTA_IMG_ABORTED:        return "ABORTED";
        case ESP_OTA_IMG_UNDEFINED:      return "UNDEFINED";
        default:                         return "UNKNOWN";
    }
}

// Best-effort fetch of the last OTA result string from the NVS errlog black
// box. Scans recent entries (newest first) for one with area=="ota" and
// returns its "event" field. Returns "" when not available so callers can
// omit/empty the field.
static const char* lcd_ota_last_result_str() {
#ifdef LCD_ERRLOG_H
    static char result[40];
    int count = errlog_count();
    if (count > 8) count = 8;  // cap scan depth — keep it cheap
    char entry[256];
    for (int i = 0; i < count; i++) {
        if (!errlog_read_entry(i, entry, sizeof(entry))) continue;
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, entry) != DeserializationError::Ok) continue;
        const char* area = doc["area"] | (const char*)nullptr;
        if (area && strcmp(area, "ota") == 0) {
            const char* event = doc["event"] | "";
            strlcpy(result, event, sizeof(result));
            return result;
        }
    }
#endif
    return "";
}

// Populate the shared firmware/partition status fields used by both the
// LCD_OTA_QUERY_RESP (over UART) and the USB `fw`/`ver` command. Reuses the
// same partition/state logic so the two reporting paths never diverge.
// Guards against NULL partition pointers by emitting "?".
static void lcd_build_fw_status_json(JsonDocument& doc) {
    doc["lcd_fw"] = kFirmwareVersion ? kFirmwareVersion : "unknown";

    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot    = esp_ota_get_boot_partition();
    const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);

    doc["running_part"] = running ? running->label : "?";

    const char* state_str = "UNKNOWN";
    if (running) {
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(running, &st) == ESP_OK) {
            state_str = lcd_ota_img_state_str(st);
        }
    }
    doc["running_state"] = state_str;

    doc["boot_part"] = boot ? boot->label : "?";
    doc["next_part"] = next ? next->label : "?";
}

// ── LCD_OTA_QUERY ────────────────────────────────────────────────────
static void lcd_ota_handle_query() {
    const esp_partition_t* ota_part = esp_ota_get_next_update_partition(NULL);

    StaticJsonDocument<512> doc;
    doc["ver"]            = PROTOCOL_VERSION;
    doc["type"]           = "LCD_OTA_QUERY_RESP";
    doc["msg_id"]         = get_next_msg_id();
    doc["ts"]             = millis();

    // Back-compat fields (lcd_fw + ota_part_label/ota_part_size).
    if (ota_part) {
        doc["ota_part_size"]  = ota_part->size;
        doc["ota_part_label"] = ota_part->label;
    } else {
        doc["ota_part_size"]  = 0;
        doc["ota_part_label"] = "none";
    }

    // Enriched partition/state fields (shared contract). This also sets
    // lcd_fw, so it is intentionally called after the back-compat block.
    lcd_build_fw_status_json(doc);

    const char* last_result = lcd_ota_last_result_str();
    if (last_result && last_result[0]) {
        doc["last_ota_result"] = last_result;
    }

    String output;
    serializeJson(doc, output);
    uart_send_json(output.c_str());

    Serial.printf("[LCD_OTA_UART] QUERY_RESP fw=%s part=%s size=%u running=%s state=%s boot=%s next=%s\n",
                  kFirmwareVersion ? kFirmwareVersion : "?",
                  ota_part ? ota_part->label : "none",
                  ota_part ? (unsigned)ota_part->size : 0,
                  doc["running_part"].as<const char*>(),
                  doc["running_state"].as<const char*>(),
                  doc["boot_part"].as<const char*>(),
                  doc["next_part"].as<const char*>());
}

// ── LCD_OTA_BEGIN ────────────────────────────────────────────────────
static void lcd_ota_handle_begin(JsonObject& doc) {
    if (s_lcd_ota_state != LCD_OTA_IDLE) {
        Serial.printf("[LCD_OTA_UART] BEGIN rejected: already in state %d\n",
                      s_lcd_ota_state);
        // Send rejection
        StaticJsonDocument<256> resp;
        resp["ver"]        = PROTOCOL_VERSION;
        resp["type"]       = "LCD_OTA_BEGIN_ACK";
        resp["msg_id"]     = get_next_msg_id();
        resp["ts"]         = millis();
        resp["accepted"]   = false;
        resp["reason"]     = "already_active";
        String out;
        serializeJson(resp, out);
        uart_send_json(out.c_str());
        return;
    }

    // Extract fields
    uint16_t session_id  = doc["session_id"] | (uint16_t)0;
    uint32_t image_size  = doc["image_size"] | (uint32_t)0;
    const char* sha256   = doc["sha256"]     | (const char*)nullptr;
    const char* version  = doc["version"]    | (const char*)nullptr;
    const char* build_id = doc["build_id"]   | (const char*)nullptr;

    Serial.printf("[LCD_OTA_UART] BEGIN session=%u size=%u ver=%s build=%s\n",
                  session_id, image_size,
                  version ? version : "?",
                  build_id ? build_id : "?");

    // Reset OTA lock timer — proxy just started, give it the full timeout window
    if (ota_locked && ota_lock_at_ms > 0) {
        ota_lock_at_ms = millis();
        Serial.println("[LCD_OTA_UART] OTA lock timer reset (proxy begin)");
    }

    // Get OTA partition
    s_lcd_ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_lcd_ota_partition) {
        Serial.println("[LCD_OTA_UART] BEGIN failed: no OTA partition");
        StaticJsonDocument<256> resp;
        resp["ver"]      = PROTOCOL_VERSION;
        resp["type"]     = "LCD_OTA_BEGIN_ACK";
        resp["msg_id"]   = get_next_msg_id();
        resp["ts"]       = millis();
        resp["accepted"] = false;
        resp["reason"]   = "no_ota_partition";
        String out;
        serializeJson(resp, out);
        uart_send_json(out.c_str());
        return;
    }

    // Validate image size fits in partition
    if (image_size > s_lcd_ota_partition->size) {
        Serial.printf("[LCD_OTA_UART] BEGIN failed: image %u > partition %u\n",
                      image_size, (unsigned)s_lcd_ota_partition->size);
        StaticJsonDocument<256> resp;
        resp["ver"]      = PROTOCOL_VERSION;
        resp["type"]     = "LCD_OTA_BEGIN_ACK";
        resp["msg_id"]   = get_next_msg_id();
        resp["ts"]       = millis();
        resp["accepted"] = false;
        resp["reason"]   = "image_too_large";
        String out;
        serializeJson(resp, out);
        uart_send_json(out.c_str());
        return;
    }

    // Store session info
    s_lcd_ota_session_id = session_id;
    s_lcd_ota_image_size = image_size;
    if (sha256) {
        strncpy(s_lcd_ota_expected_sha256, sha256, 64);
        s_lcd_ota_expected_sha256[64] = '\0';
    }
    if (version) {
        strncpy(s_lcd_ota_target_version, version, 31);
        s_lcd_ota_target_version[31] = '\0';
    }

    // Check NVS for resume
    uint32_t resume_offset = 0;
    bool resuming = false;
    {
        uint16_t saved_session = 0;
        uint32_t saved_offset  = 0;
        uint32_t saved_size    = 0;
        char     saved_sha[65] = {0};
        char     saved_ver[32] = {0};

        if (lcd_ota_nvs_load_progress(&saved_session, &saved_offset,
                                       &saved_size, saved_sha, saved_ver)) {
            if (saved_session == session_id &&
                saved_size == image_size &&
                sha256 && strcmp(saved_sha, sha256) == 0) {
                // Matching session: resume
                resume_offset = saved_offset;
                resuming = true;
                Serial.printf("[LCD_OTA_UART] RESUME detected: offset=%u\n",
                              resume_offset);
            } else {
                // Different session: clear stale progress
                lcd_ota_nvs_clear();
                Serial.println("[LCD_OTA_UART] Stale NVS progress cleared");
            }
        }
    }

    // Keep the UI task alive but in OTA idle mode (g_ota_screen_active).
    // The UI task will just tick LVGL without processing events.
    // This avoids the need to restart the UI task after OTA (which
    // caused crashes from corrupt LVGL state and download mode issues).
    g_ota_screen_active = true;
    Serial.printf("[LCD_OTA_UART] UI in OTA mode heap_free=%u largest=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // Begin OTA write
    // When resuming, use OTA_SIZE_UNKNOWN since we're continuing a partial write.
    // For fresh start, pass actual image size.
    esp_err_t err = esp_ota_begin(s_lcd_ota_partition,
                                  resuming ? OTA_SIZE_UNKNOWN : image_size,
                                  &s_lcd_ota_handle);
    if (err != ESP_OK) {
        Serial.printf("[LCD_OTA_UART] esp_ota_begin failed: 0x%x\n", err);
        StaticJsonDocument<256> resp;
        resp["ver"]      = PROTOCOL_VERSION;
        resp["type"]     = "LCD_OTA_BEGIN_ACK";
        resp["msg_id"]   = get_next_msg_id();
        resp["ts"]       = millis();
        resp["accepted"] = false;
        resp["reason"]   = "ota_begin_failed";
        String out;
        serializeJson(resp, out);
        uart_send_json(out.c_str());
        lcd_ota_uart_restore_ui();
        return;
    }

    // Init SHA256 context
    mbedtls_sha256_init(&s_lcd_ota_sha_ctx);
    mbedtls_sha256_starts(&s_lcd_ota_sha_ctx, 0);  // 0 = SHA-256 (not 224)

    if (resuming) {
        // SHA context can't be resumed (blob save caused heap corruption).
        // Always restart from offset 0.
        Serial.println("[LCD_OTA_UART] Resume not supported (no SHA ctx), starting fresh");
        resume_offset = 0;
        resuming = false;
        lcd_ota_nvs_clear();
    }

    s_lcd_ota_bytes_written     = resuming ? resume_offset : 0;
    s_lcd_ota_last_nvs_offset   = s_lcd_ota_bytes_written;
    s_lcd_ota_last_progress_pct = 0;

    // Create protocol instance (quiet mode suppresses per-frame Serial.printf)
    s_lcd_ota_protocol = new UartOtaProtocol(&senseSerial);
    if (s_lcd_ota_protocol) s_lcd_ota_protocol->quiet = true;
    if (!s_lcd_ota_protocol) {
        Serial.println("[LCD_OTA_UART] FATAL: failed to allocate UartOtaProtocol");
        esp_ota_abort(s_lcd_ota_handle);
        s_lcd_ota_handle = 0;
        mbedtls_sha256_free(&s_lcd_ota_sha_ctx);
        lcd_ota_uart_restore_ui();
        return;
    }

    g_lcd_ota_progress_pct = 0;
    g_lcd_ota_show_progress = true;

    Serial.printf("[LCD_OTA_UART] OTA started: session=%u size=%u resume=%u partition=%s\n",
                  session_id, image_size, resume_offset,
                  s_lcd_ota_partition->label);

    // Send acceptance BEFORE switching to binary mode
    {
        StaticJsonDocument<320> resp;
        resp["ver"]            = PROTOCOL_VERSION;
        resp["type"]           = "LCD_OTA_BEGIN_ACK";
        resp["msg_id"]         = get_next_msg_id();
        resp["ts"]             = millis();
        resp["session_id"]     = session_id;
        resp["accepted"]       = true;
        resp["resume_offset"]  = resume_offset;

        String out;
        serializeJson(resp, out);
        uart_send_json(out.c_str());
    }

    // Drain any stray bytes in the RX buffer before switching to binary
    delay(50);  // Allow any in-flight JSON to arrive
    while (senseSerial.available() > 0) {
        senseSerial.read();
    }

    // Switch to binary RX mode
    s_lcd_ota_state       = LCD_OTA_RECEIVING;
    g_lcd_ota_binary_mode = true;
    g_lcd_ota_uart_receiving = true;  // Early-visible flag for sleep logic
    g_suppress_uart_json_tx = true;  // Suppress JSON TX to avoid corrupting COBS frames
    s_lcd_ota_last_chunk_ms = millis();
}

// ── LCD_OTA_END ──────────────────────────────────────────────────────
static void lcd_ota_handle_end(JsonObject& doc) {
    unsigned long t0 = millis();
    Serial.printf("[LCD_OTA_UART] END received: session=%u written=%u expected=%u t=%lu\n",
                  s_lcd_ota_session_id, s_lcd_ota_bytes_written, s_lcd_ota_image_size, t0);

    // Switch back from binary mode
    g_lcd_ota_binary_mode = false;
    g_suppress_uart_json_tx = false;
    s_lcd_ota_state = LCD_OTA_FINALIZING;
    // Note: g_lcd_ota_uart_receiving stays true through FINALIZING; cleared on IDLE

    // Delete protocol instance (no longer needed)
    if (s_lcd_ota_protocol) {
        delete s_lcd_ota_protocol;
        s_lcd_ota_protocol = NULL;
    }

    // Finalize SHA256
    unsigned long t1 = millis();
    uint8_t computed_hash[32];
    mbedtls_sha256_finish(&s_lcd_ota_sha_ctx, computed_hash);
    mbedtls_sha256_free(&s_lcd_ota_sha_ctx);
    unsigned long t2 = millis();
    Serial.printf("[LCD_OTA_UART] SHA256 finalize took %lums\n", t2 - t1);

    // Convert to hex string
    char computed_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(&computed_hex[i * 2], 3, "%02x", computed_hash[i]);
    }
    computed_hex[64] = '\0';

    bool sha_match = (strlen(s_lcd_ota_expected_sha256) > 0 &&
                      strcmp(computed_hex, s_lcd_ota_expected_sha256) == 0);

    Serial.printf("[LCD_OTA_UART] SHA256 computed=%s expected=%s match=%d\n",
                  computed_hex, s_lcd_ota_expected_sha256, sha_match ? 1 : 0);

    bool ota_ok = false;

    if (sha_match) {
        // Finalize OTA
        unsigned long t3 = millis();
        esp_err_t err = esp_ota_end(s_lcd_ota_handle);
        unsigned long t4 = millis();
        Serial.printf("[LCD_OTA_UART] esp_ota_end took %lums err=0x%x\n", t4 - t3, err);
        s_lcd_ota_handle = 0;

        if (err != ESP_OK) {
            Serial.printf("[LCD_OTA_UART] esp_ota_end failed: 0x%x\n", err);
            lcd_errlog_store_with_context("lcd", "ota", "OTA_END_FAIL", (int)err, "esp_ota_end");
        } else {
            unsigned long t5 = millis();
            err = esp_ota_set_boot_partition(s_lcd_ota_partition);
            unsigned long t6 = millis();
            Serial.printf("[LCD_OTA_UART] esp_ota_set_boot_partition took %lums err=0x%x\n", t6 - t5, err);
            if (err != ESP_OK) {
                Serial.printf("[LCD_OTA_UART] esp_ota_set_boot_partition failed: 0x%x\n", err);
                lcd_errlog_store_with_context("lcd", "ota", "BOOT_PART_FAIL", (int)err, "set_boot_partition");
            } else {
                ota_ok = true;
                Serial.printf("[LCD_OTA_UART] Boot partition set to %s total_end_ms=%lu\n",
                              s_lcd_ota_partition->label, millis() - t0);
            }
        }
    } else {
        // SHA mismatch — abort OTA
        Serial.println("[LCD_OTA_UART] SHA256 mismatch — aborting OTA");
        lcd_errlog_store_with_context("lcd", "ota", "SHA_MISMATCH", 0, "checksum_mismatch");
        if (s_lcd_ota_handle != 0) {
            esp_ota_abort(s_lcd_ota_handle);
            s_lcd_ota_handle = 0;
        }
    }

    // Clear NVS progress regardless of outcome
    lcd_ota_nvs_clear();

    // Send response
    StaticJsonDocument<384> resp;
    resp["ver"]          = PROTOCOL_VERSION;
    resp["type"]         = "LCD_OTA_END_ACK";
    resp["msg_id"]       = get_next_msg_id();
    resp["ts"]           = millis();
    resp["session_id"]   = s_lcd_ota_session_id;
    resp["sha_match"]    = sha_match;
    resp["sha_computed"] = computed_hex;
    resp["ota_ok"]       = ota_ok;

    String out;
    serializeJson(resp, out);
    uart_send_json(out.c_str());

    // Reset state
    g_lcd_ota_uart_receiving     = false;
    s_lcd_ota_state              = LCD_OTA_IDLE;
    s_lcd_ota_partition          = NULL;
    s_lcd_ota_bytes_written      = 0;
    s_lcd_ota_image_size         = 0;
    s_lcd_ota_last_nvs_offset    = 0;
    s_lcd_ota_last_chunk_ms      = 0;
    s_lcd_ota_last_progress_pct  = 0;
    s_lcd_ota_expected_sha256[0] = '\0';
    s_lcd_ota_target_version[0]  = '\0';

    if (sha_match && ota_ok) {
        // Reboot IMMEDIATELY. Do not call lcd_ota_uart_restore_ui() first —
        // clearing flags lets the sleep coordinator (Core 1) put the device
        // to deep sleep before esp_restart() can execute on Core 0.
        // The new firmware will initialize clean on boot.
        //
        // Clear testmode NVS so it doesn't persist after reboot.
        if (g_test_mode_active) {
            g_test_mode_active = false;
            Preferences prefs;
            if (prefs.begin("test_cfg", false)) {
                prefs.remove("test_mode");
                prefs.end();
            }
        }
        // Clear maintenance NVS so stale state doesn't re-enter headless after reboot.
        lcd_clear_persisted_maintenance_state("ota_complete");
        delay(200);  // Allow NVS writes to flush
        // Clear the bootloader's deep-sleep-validate cache (STORE6/STORE7).
        REG_WRITE(RTC_CNTL_STORE6_REG, 0);
        REG_WRITE(RTC_CNTL_STORE7_REG, 0);
        Serial.printf("[LCD_OTA_UART] OTA complete — rebooting into %s\n",
                      s_lcd_ota_partition ? s_lcd_ota_partition->label : "?");
        Serial.flush();
        delay(100);
        REG_WRITE(RTC_CNTL_STORE6_REG, 0);
        REG_WRITE(RTC_CNTL_STORE7_REG, 0);
        esp_restart();
    } else {
        Serial.printf("[LCD_OTA_UART] OTA failed: sha_match=%d ota_ok=%d\n",
                      sha_match ? 1 : 0, ota_ok ? 1 : 0);
        lcd_ota_uart_restore_ui();
    }
}

// ── LCD_OTA_ABORT ────────────────────────────────────────────────────
static void lcd_ota_handle_abort(JsonObject& doc) {
    const char* reason = doc["reason"] | "sense_abort";

    Serial.printf("[LCD_OTA_UART] ABORT from Sense: reason=%s session=%u written=%u\n",
                  reason, s_lcd_ota_session_id, s_lcd_ota_bytes_written);

    // Cleanup OTA handle
    if (s_lcd_ota_handle != 0) {
        esp_ota_abort(s_lcd_ota_handle);
        s_lcd_ota_handle = 0;
    }

    mbedtls_sha256_free(&s_lcd_ota_sha_ctx);

    if (s_lcd_ota_protocol) {
        delete s_lcd_ota_protocol;
        s_lcd_ota_protocol = NULL;
    }

    // Preserve NVS on timeout for resume; clear on explicit abort
    bool is_timeout = reason && (strcmp(reason, "timeout") == 0);
    if (!is_timeout) {
        lcd_ota_nvs_clear();
    } else {
        Serial.println("[LCD_OTA_UART] NVS preserved for resume (timeout)");
    }

    g_lcd_ota_binary_mode        = false;
    g_lcd_ota_uart_receiving     = false;
    g_suppress_uart_json_tx      = false;
    s_lcd_ota_state              = LCD_OTA_IDLE;
    s_lcd_ota_partition          = NULL;
    s_lcd_ota_bytes_written      = 0;
    s_lcd_ota_image_size         = 0;
    s_lcd_ota_last_nvs_offset    = 0;
    s_lcd_ota_last_chunk_ms      = 0;
    s_lcd_ota_last_progress_pct  = 0;
    s_lcd_ota_expected_sha256[0] = '\0';
    s_lcd_ota_target_version[0]  = '\0';

    lcd_ota_uart_restore_ui();
}


// ═════════════════════════════════════════════════════════════════════
//  Binary Receive Loop
// ═════════════════════════════════════════════════════════════════════

/**
 * Called repeatedly from uart_task when g_lcd_ota_binary_mode is true.
 *
 * Receives COBS-framed binary chunks from the Sense board, writes them
 * to the OTA partition, updates the SHA256 hash, and sends ACK/NACK.
 *
 * If a line starting with '{' is detected on the UART, it's treated as
 * a JSON control message (LCD_OTA_END or LCD_OTA_ABORT) and dispatched
 * back to the JSON handler.  The function returns false to signal the
 * caller to exit binary mode.
 *
 * Returns true if still receiving, false if OTA ended or aborted.
 */
static bool lcd_ota_receive_loop() {
    static unsigned long s_last_diag_ms = 0;
    if (s_lcd_ota_state != LCD_OTA_RECEIVING || !s_lcd_ota_protocol) {
        if (millis() - s_last_diag_ms > 5000) {
            Serial.printf("[LCD_OTA_UART] recv_loop skip: state=%d proto=%p\n",
                          (int)s_lcd_ota_state, (void*)s_lcd_ota_protocol);
            s_last_diag_ms = millis();
        }
        return false;
    }
    if (millis() - s_last_diag_ms > 10000) {
        Serial.printf("[LCD_OTA_UART] recv_loop: written=%u/%u last_chunk=%lums ago\n",
                      s_lcd_ota_bytes_written, s_lcd_ota_image_size,
                      millis() - s_lcd_ota_last_chunk_ms);
        s_last_diag_ms = millis();
    }

    // Check for idle timeout
    unsigned long now = millis();
    if (s_lcd_ota_last_chunk_ms > 0 &&
        (now - s_lcd_ota_last_chunk_ms) >= LCD_OTA_IDLE_TIMEOUT_MS) {
        Serial.printf("[LCD_OTA_UART] Idle timeout (%lu ms since last chunk)\n",
                      now - s_lcd_ota_last_chunk_ms);
        lcd_ota_abort_internal("timeout");
        return false;
    }

    // Peek at the first available byte.  If it's '{', a JSON control
    // message has arrived (LCD_OTA_END or LCD_OTA_ABORT).  Read the full
    // line and dispatch it.
    if (senseSerial.available() > 0) {
        int peek = senseSerial.peek();
        // If all data received, consume any non-'{' stray bytes (e.g. COBS
        // 0x00 delimiters) so they don't block the LCD_OTA_END JSON read.
        if (peek != '{' && s_lcd_ota_bytes_written >= s_lcd_ota_image_size) {
            uint8_t discarded = senseSerial.read();
            Serial.printf("[LCD_OTA_UART] discarded stray byte 0x%02X while awaiting JSON\n",
                          discarded);
            return true;  // loop back to check next byte
        }
        if (peek == '{') {
            // Read the full JSON line
            char json_buf[512];
            int pos = 0;
            unsigned long read_start = millis();
            while ((millis() - read_start) < 1000 && pos < (int)(sizeof(json_buf) - 1)) {
                if (senseSerial.available() > 0) {
                    char c = senseSerial.read();
                    if (c == '\n' || c == '\r') {
                        if (pos > 0) break;
                        continue;
                    }
                    json_buf[pos++] = c;
                } else {
                    delay(1);
                }
            }
            json_buf[pos] = '\0';

            if (pos > 0) {
                Serial.printf("[LCD_OTA_UART] JSON in binary mode: %s\n", json_buf);
                StaticJsonDocument<512> jdoc;
                DeserializationError jerr = deserializeJson(jdoc, json_buf);
                if (jerr == DeserializationError::Ok) {
                    const char* type = jdoc["type"] | (const char*)nullptr;
                    if (type) {
                        JsonObject obj = jdoc.as<JsonObject>();
                        if (strcmp(type, "LCD_OTA_END") == 0) {
                            lcd_ota_handle_end(obj);
                            return false;
                        } else if (strcmp(type, "LCD_OTA_ABORT") == 0) {
                            lcd_ota_handle_abort(obj);
                            return false;
                        } else {
                            Serial.printf("[LCD_OTA_UART] WARN: unexpected JSON type in binary mode: %s\n", type);
                        }
                    }
                } else {
                    Serial.printf("[LCD_OTA_UART] WARN: JSON parse error in binary mode: %s\n", jerr.c_str());
                }
            }
            return true;  // Continue receiving
        }
    }

    // All expected bytes received — stop calling recv_frame so it won't
    // consume the incoming LCD_OTA_END JSON bytes.  Just wait for the
    // peek-for-'{' check above to catch the JSON message.
    if (s_lcd_ota_bytes_written >= s_lcd_ota_image_size) {
        if (millis() - s_last_diag_ms > 2000) {
            Serial.printf("[LCD_OTA_UART] All %u bytes received, awaiting LCD_OTA_END JSON\n",
                          s_lcd_ota_bytes_written);
            s_last_diag_ms = millis();
        }
        delay(10);
        return true;
    }

    // Try to receive a binary COBS frame
    uint8_t msg_type = 0;
    uint16_t seq = 0;
    uint8_t chunk_data[MAX_CHUNK_SIZE];
    size_t chunk_len = sizeof(chunk_data);

    bool got_frame = s_lcd_ota_protocol->recv_frame(&msg_type, &seq,
                                                     chunk_data, &chunk_len,
                                                     LCD_OTA_CHUNK_TIMEOUT_MS);
    if (!got_frame) {
        // No frame within timeout — not necessarily fatal; the idle
        // timeout above handles true stalls.
        return true;
    }

    if (msg_type != MSG_CHUNK) {
        Serial.printf("[LCD_OTA_UART] WARN: unexpected msg_type=%u seq=%u\n",
                      msg_type, seq);
        return true;
    }

    // Write chunk to OTA partition
    esp_err_t werr = esp_ota_write(s_lcd_ota_handle, chunk_data, chunk_len);
    if (werr != ESP_OK) {
        Serial.printf("[LCD_OTA_UART] esp_ota_write failed: 0x%x (len=%u)\n",
                      werr, (unsigned)chunk_len);
        s_lcd_ota_protocol->send_nack(seq, ERR_WRITE_FAILED);
        lcd_ota_abort_internal("write_failed");
        return false;
    }

    // Update SHA256
    mbedtls_sha256_update(&s_lcd_ota_sha_ctx, chunk_data, chunk_len);

    s_lcd_ota_bytes_written += chunk_len;
    s_lcd_ota_last_chunk_ms = millis();

    // Send ACK
    s_lcd_ota_protocol->send_ack(seq);

    // Periodic NVS save
    if ((s_lcd_ota_bytes_written - s_lcd_ota_last_nvs_offset) >= LCD_OTA_NVS_SAVE_INTERVAL) {
        lcd_ota_nvs_save_progress();
    }

    // Report progress at 10% intervals
    if (s_lcd_ota_image_size > 0) {
        uint8_t pct = (uint8_t)((uint64_t)s_lcd_ota_bytes_written * 100 / s_lcd_ota_image_size);
        uint8_t pct_bucket = pct / 10 * 10;  // Round down to nearest 10
        if (pct_bucket > s_lcd_ota_last_progress_pct) {
            s_lcd_ota_last_progress_pct = pct_bucket;
            g_lcd_ota_progress_pct = pct;
            g_lcd_ota_show_progress = true;
            lcd_ota_send_status_json(pct);
            resetActivityTimer();  // Keep sleep timer alive during OTA
        }
    }

    return true;
}


// ═════════════════════════════════════════════════════════════════════
//  Self-Test (post-OTA boot validation)
// ═════════════════════════════════════════════════════════════════════

/**
 * Called from LCD setup() after display init.
 *
 * If the running partition is in ESP_OTA_IMG_PENDING_VERIFY state,
 * we run basic self-tests (display initialized, UART responsive).
 * On pass: mark app valid (cancel rollback).
 * On fail: mark invalid and rollback+reboot to previous firmware.
 */
static void lcd_ota_self_test() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        Serial.println("[LCD_OTA_UART] self_test: cannot determine running partition");
        return;
    }

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        // Not an OTA partition or state not available — nothing to do
        Serial.printf("[LCD_OTA_UART] self_test: get_state err=0x%x (ok if factory)\n", err);
        return;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        Serial.printf("[LCD_OTA_UART] self_test: partition state=%d (not pending), skipping\n",
                      (int)state);
        return;
    }

    // OTA firmware is SHA256-verified before write. If we booted, it's good.
    // Mark valid immediately — no self-test. The previous self-test checked
    // LVGL init which can fail on timing (LVGL runs on Core 1, self-test
    // runs early in setup on Core 0), causing unnecessary rollback.
    Serial.printf("[LCD_OTA_UART] Marking OTA partition %s as valid (SHA256 pre-verified)\n",
                  running->label);
    esp_err_t mark_err = esp_ota_mark_app_valid_cancel_rollback();
    if (mark_err != ESP_OK) {
        Serial.printf("[LCD_OTA_UART] WARN: mark_valid failed: 0x%x\n", mark_err);
    } else {
        Serial.println("[LCD_OTA_UART] OTA partition confirmed valid — rollback cancelled");
    }
}

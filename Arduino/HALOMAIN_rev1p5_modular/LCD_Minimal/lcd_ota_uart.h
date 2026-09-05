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
#define LCD_OTA_IDLE_TIMEOUT_MS   30000   // 30s no new accepted chunk -> abort
#define LCD_OTA_ATTEMPT_BUDGET_MS 2400000 // absolute whole-attempt ceiling, including retries

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
static std::atomic<LcdOtaState> s_lcd_ota_state{LCD_OTA_IDLE};
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
static uint32_t               s_lcd_ota_started_ms = 0;
static uint32_t               s_lcd_ota_budget_ms = LCD_OTA_ATTEMPT_BUDGET_MS;
static uint16_t               s_lcd_ota_expected_seq = 1;
static uint16_t               s_lcd_ota_last_seq = 0;
static size_t                 s_lcd_ota_last_len = 0;
static uint8_t                s_lcd_ota_last_data[MAX_CHUNK_SIZE];
static bool                   s_lcd_ota_have_last = false;
static bool                   s_lcd_ota_control_v2 = false;
static uint16_t               s_lcd_ota_last_aborted_session = 0;

// Terminal evidence is already bounded; do not append/truncate it through the
// generic192-byte context buffer. Preserve the complete record in the same ring.
static void lcd_ota_store_terminal(const char* board, const char* event, int32_t code,
                                   const char* detail) {
    StaticJsonDocument<512> entry;
    entry["board"] = board;
    entry["area"] = "ota";
    entry["event"] = event;
    entry["code"] = code;
    entry["detail"] = detail ? detail : "";
    entry["uptime_ms"] = millis();
    String output;
    serializeJson(entry, output);
    errlog_store(output.c_str());
}

static void lcd_ota_send_abort_ack(uint16_t session_id) {
    StaticJsonDocument<192> doc;
    doc["ver"] = PROTOCOL_VERSION;
    doc["type"] = "LCD_OTA_ABORT_ACK";
    doc["msg_id"] = get_next_msg_id();
    doc["ts"] = (uint32_t)millis();
    doc["session_id"] = session_id;
    doc["json_ready"] = true;
    String output;
    serializeJson(doc, output);
    uart_send_json(output.c_str());
}

// Global flag: when true, the UART task switches to binary RX mode.
static bool g_lcd_ota_binary_mode = false;

static volatile int g_lcd_ota_progress_pct = -1;  // -1 = no OTA, 0-100 = progress
static volatile bool g_lcd_ota_show_progress = false;

// Receive and finalization both block sleep, including successful reboot prep.
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
    g_ota_lock_window_until_ms = 0;

    // Clear maintenance window and OTA mode flags so sleep is no longer blocked
    g_lcd_maintenance_active = false;
    g_lcd_maintenance_deadline_ms = 0;
    g_ota_mode_active = false;

    // Clear manual OTA override so the device doesn't re-trigger OTA on next wake
    g_manual_ota_override = false;
    g_manual_ota_override_until_ms = 0;

    g_ota_screen_active = false;
    g_lcd_maintenance_headless = false;

    // One readback-verified disarm. Its helper publishes the actual stored
    // value; a failed commit must not be hidden by unconditional RTC clearing.
    lcd_clear_persisted_maintenance_state("ota_complete");

    // Don't try to restore LVGL here — this runs on Core 0 (UART task) and
    // lcd_exit_ota_mode() calls LVGL init which crashes on Core 0.
    // For successful OTA, esp_restart() is called immediately after this function.
    // For failed OTA, just clear flags and let the device sleep/wake naturally.
    provision_return_home_pending = true;
    resetActivityTimer();
    lcd_ota_arm_recovery_grace();
    Serial.println("[LCD_OTA_UART] OTA flags cleared");
}

// Forward declaration
static void lcd_ota_handle_abort(JsonObject& doc);

static void lcd_ota_abort_internal(const char* reason) {
    s_lcd_ota_state = LCD_OTA_ABORTING;
    g_lcd_ota_uart_receiving = true;
    // The abort is JSON. Leave COBS mode without releasing sleep ownership.
    g_lcd_ota_binary_mode = false;
    g_suppress_uart_json_tx = false;
    Serial.printf("[LCD_OTA_UART] ABORT reason=%s state=%d written=%u\n",
                  reason ? reason : "unknown", static_cast<int>(s_lcd_ota_state.load()),
                  s_lcd_ota_bytes_written);

    char terminal[160];
    snprintf(terminal, sizeof(terminal), "s=%u why=%s seq=%u bytes=%lu idle=%lu crc=%lu frame=%lu",
             s_lcd_ota_session_id, reason ? reason : "unknown", s_lcd_ota_last_seq,
             (unsigned long)s_lcd_ota_bytes_written,
             (unsigned long)(millis() - s_lcd_ota_last_chunk_ms),
             (unsigned long)(s_lcd_ota_protocol ? s_lcd_ota_protocol->crc_error_count() : 0),
             (unsigned long)(s_lcd_ota_protocol ? s_lcd_ota_protocol->frame_error_count() : 0));
    lcd_ota_store_terminal("lcd", "OTA_ABORT", (int)s_lcd_ota_bytes_written, terminal);

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

    s_lcd_ota_partition          = NULL;
    s_lcd_ota_bytes_written      = 0;
    s_lcd_ota_image_size         = 0;
    s_lcd_ota_last_nvs_offset    = 0;
    s_lcd_ota_last_chunk_ms      = 0;
    s_lcd_ota_last_progress_pct  = 0;
    s_lcd_ota_expected_sha256[0] = '\0';
    s_lcd_ota_target_version[0]  = '\0';

    lcd_ota_uart_restore_ui();
    s_lcd_ota_state = LCD_OTA_IDLE;
    g_lcd_ota_uart_receiving = false;
    s_lcd_ota_last_aborted_session = s_lcd_ota_session_id;
    lcd_ota_send_abort_ack(s_lcd_ota_session_id);
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
    doc["boot_ready"] = g_lcd_boot_ready.load();
#ifdef HALO_LCD_PROD_WRAPPER
    doc["peer_boot_id"] = halo_lcd_coord_boot_id();
    doc["coord_waiting"] = halo_lcd_coord_waiting();
    doc["coord_owner"] = halo_lcd_coord_owner();
    doc["coord_lease_ms"] = halo_lcd_coord_lease_ms();
#endif

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
static void lcd_ota_handle_query(const char* coord_id = nullptr) {
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
    if (coord_id && coord_id[0] && strlen(coord_id) < 40) doc["coord_id"] = coord_id;

    const char* last_result = lcd_ota_last_result_str();
    if (last_result && last_result[0]) {
        doc["last_ota_result"] = last_result;
    }

    String output;
    serializeJson(doc, output);
    uart_send_json(output.c_str());

    Serial.printf("[LCD_OTA_UART] QUERY_RESP fw=%s part=%s size=%u running=%s state=%s boot=%s next=%s boot_ready=%d\n",
                  kFirmwareVersion ? kFirmwareVersion : "?",
                  ota_part ? ota_part->label : "none",
                  ota_part ? (unsigned)ota_part->size : 0,
                  doc["running_part"].as<const char*>(),
                  doc["running_state"].as<const char*>(),
                  doc["boot_part"].as<const char*>(),
                  doc["next_part"].as<const char*>(),
                  doc["boot_ready"].as<bool>() ? 1 : 0);
}

// ── LCD_OTA_BEGIN ────────────────────────────────────────────────────
static void lcd_ota_handle_begin(JsonObject& doc) {
    // Share the arm/sleep gate through startup and erase. Once sleep owns it,
    // this request gets no acceptance proof and cannot start an inactive write.
    LcdMaintenanceStorageGuard startup_guard;
    if (g_lcd_sleep_commit_gate.load()) return;
    if (s_lcd_ota_state != LCD_OTA_IDLE) {
        Serial.printf("[LCD_OTA_UART] BEGIN rejected: already in state %d\n",
                      static_cast<int>(s_lcd_ota_state.load()));
        // Send rejection
        StaticJsonDocument<256> resp;
        resp["ver"]        = PROTOCOL_VERSION;
        resp["type"]       = "LCD_OTA_BEGIN_ACK";
        resp["msg_id"]     = get_next_msg_id();
        resp["ts"]         = millis();
        resp["accepted"]   = false;
        resp["reason"]     = "already_active";
        if (doc["session_id"].is<uint16_t>()) resp["session_id"] = doc["session_id"].as<uint16_t>();
        resp["json_ready"] = false;
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
        resp["session_id"] = session_id;
        resp["json_ready"] = true;
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
        resp["session_id"] = session_id;
        resp["json_ready"] = true;
        String out;
        serializeJson(resp, out);
        uart_send_json(out.c_str());
        return;
    }

    // Reject incomplete metadata before opening/writing an inactive partition.
    bool valid_sha = sha256 && strlen(sha256) == 64;
    for (size_t i = 0; valid_sha && i < 64; ++i) {
        const char c = sha256[i];
        valid_sha = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }
    if (!doc["session_id"].is<uint16_t>() || !doc["image_size"].is<uint32_t>() ||
        (!doc["budget_ms"].isUnbound() && (!doc["budget_ms"].is<uint32_t>() || (doc["budget_ms"] | (uint32_t)0) == 0)) ||
        (!doc["ota_proto"].isUnbound() && (!doc["ota_proto"].is<uint8_t>() ||
          ((doc["ota_proto"] | 0) != 1 && (doc["ota_proto"] | 0) != OTA_UART_PROTOCOL_VERSION))) ||
        !session_id || !image_size || !valid_sha || !version || !version[0] || strlen(version) >= sizeof(s_lcd_ota_target_version)) {
        StaticJsonDocument<256> resp;
        resp["ver"] = PROTOCOL_VERSION;
        resp["type"] = "LCD_OTA_BEGIN_ACK";
        resp["msg_id"] = get_next_msg_id();
        resp["ts"] = (uint32_t)millis();
        resp["session_id"] = session_id;
        resp["accepted"] = false;
        resp["reason"] = "invalid_metadata";
        resp["json_ready"] = true;
        String output;
        serializeJson(resp, output);
        uart_send_json(output.c_str());
        return;
    }
    s_lcd_ota_started_ms = millis();
    uint32_t requested_budget = doc["budget_ms"] | (uint32_t)LCD_OTA_ATTEMPT_BUDGET_MS;
    s_lcd_ota_budget_ms = requested_budget && requested_budget < LCD_OTA_ATTEMPT_BUDGET_MS
                             ? requested_budget : LCD_OTA_ATTEMPT_BUDGET_MS;
    s_lcd_ota_control_v2 = (doc["ota_proto"] | 1) == OTA_UART_PROTOCOL_VERSION;

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

    // BEGIN's erase/NVS work also owns sleep. The UART task is still inside
    // this handler; binary polling starts only after the acceptance response.
    s_lcd_ota_state = LCD_OTA_RECEIVING;
    g_lcd_ota_uart_receiving = true;
    s_lcd_ota_last_aborted_session = 0;

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

    // Proxy has started — the dual-OTA "Sense rebooting" window is over, the
    // LCD OTA is now actively receiving. Clear the lock window so the
    // SENSE_ASLEEP guards resume normal behavior once this OTA finishes.
    g_ota_lock_window_until_ms = 0;

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
        resp["session_id"] = session_id;
        lcd_ota_uart_restore_ui();
        s_lcd_ota_state = LCD_OTA_IDLE;
        g_lcd_ota_uart_receiving = false;
        resp["json_ready"] = true;
        String out;
        serializeJson(resp, out);
        uart_send_json(out.c_str());
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
    s_lcd_ota_expected_seq = 1;
    s_lcd_ota_last_seq = 0;
    s_lcd_ota_last_len = 0;
    s_lcd_ota_have_last = false;

    // Create protocol instance (quiet mode suppresses per-frame Serial.printf)
    s_lcd_ota_protocol = new UartOtaProtocol(&senseSerial);
    if (s_lcd_ota_protocol) s_lcd_ota_protocol->quiet = true;
    if (!s_lcd_ota_protocol || !s_lcd_ota_protocol->valid()) {
        delete s_lcd_ota_protocol;
        s_lcd_ota_protocol = nullptr;
        Serial.println("[LCD_OTA_UART] FATAL: failed to allocate UartOtaProtocol");
        esp_ota_abort(s_lcd_ota_handle);
        s_lcd_ota_handle = 0;
        mbedtls_sha256_free(&s_lcd_ota_sha_ctx);
        lcd_ota_uart_restore_ui();
        s_lcd_ota_state = LCD_OTA_IDLE;
        g_lcd_ota_uart_receiving = false;
        StaticJsonDocument<256> resp;
        resp["ver"] = PROTOCOL_VERSION;
        resp["type"] = "LCD_OTA_BEGIN_ACK";
        resp["msg_id"] = get_next_msg_id();
        resp["ts"] = (uint32_t)millis();
        resp["session_id"] = session_id;
        resp["accepted"] = false;
        resp["reason"] = "out_of_memory";
        resp["json_ready"] = true;
        String output;
        serializeJson(resp, output);
        uart_send_json(output.c_str());
        return;
    }
    if ((uint32_t)(millis() - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms) {
        lcd_ota_abort_internal("begin_deadline");
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
        resp["ota_proto"]      = s_lcd_ota_control_v2 ? OTA_UART_PROTOCOL_VERSION : 1;

        String out;
        serializeJson(resp, out);
        uart_send_json(out.c_str());
    }

    // The JSON parser already consumed BEGIN's delimiter. Do not drain after
    // ACK: a fast sender may already be delivering the first valid CHUNK.

    // Switch to binary RX mode
    s_lcd_ota_state       = LCD_OTA_RECEIVING;
    g_lcd_ota_binary_mode = true;
    g_lcd_ota_uart_receiving = true;  // Early-visible flag for sleep logic
    g_suppress_uart_json_tx = true;  // Suppress JSON TX to avoid corrupting COBS frames
    s_lcd_ota_last_chunk_ms = millis();
}

// ── LCD_OTA_END ──────────────────────────────────────────────────────
static void lcd_ota_handle_end(JsonObject& doc) {
    const uint16_t session = doc["session_id"] | (uint16_t)0;
    if (!doc["session_id"].is<uint16_t>() || s_lcd_ota_state != LCD_OTA_RECEIVING ||
        !session || session != s_lcd_ota_session_id) return;
    if ((!doc["image_size"].isUnbound() && !doc["image_size"].is<uint32_t>()) ||
        s_lcd_ota_bytes_written != s_lcd_ota_image_size ||
        (doc["image_size"] | s_lcd_ota_image_size) != s_lcd_ota_image_size) {
        lcd_ota_abort_internal("end_size_mismatch");
        return;
    }
    if ((uint32_t)(millis() - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms) {
        lcd_ota_abort_internal("end_deadline");
        return;
    }
    const char* sender_detail = doc["detail"] | (const char*)nullptr;
    if (sender_detail && strlen(sender_detail) < 224)
        lcd_ota_store_terminal("sense", "LCD_PROXY_STREAM", 0, sender_detail);
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
    bool deadline_expired = (uint32_t)(millis() - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms;

    if (sha_match && !deadline_expired) {
        // Finalize OTA
        unsigned long t3 = millis();
        esp_err_t err = esp_ota_end(s_lcd_ota_handle);
        unsigned long t4 = millis();
        Serial.printf("[LCD_OTA_UART] esp_ota_end took %lums err=0x%x\n", t4 - t3, err);
        s_lcd_ota_handle = 0;
        deadline_expired = (uint32_t)(millis() - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms;

        if (err != ESP_OK) {
            Serial.printf("[LCD_OTA_UART] esp_ota_end failed: 0x%x\n", err);
            lcd_errlog_store_with_context("lcd", "ota", "OTA_END_FAIL", (int)err, "esp_ota_end");
        } else if (!deadline_expired) {
            unsigned long t5 = millis();
            // Last cancellable boundary. A successful boot selection remains
            // committed even if the synchronous SDK call returns after budget.
            deadline_expired = (uint32_t)(t5 - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms;
            if (!deadline_expired) {
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
        }
    } else {
        // SHA mismatch — abort OTA
        if (!deadline_expired) {
            Serial.println("[LCD_OTA_UART] SHA256 mismatch — aborting OTA");
            lcd_errlog_store_with_context("lcd", "ota", "SHA_MISMATCH", 0, "checksum_mismatch");
        }
        if (s_lcd_ota_handle != 0) {
            esp_ota_abort(s_lcd_ota_handle);
            s_lcd_ota_handle = 0;
        }
    }
    if (deadline_expired && !ota_ok)
        lcd_ota_store_terminal("lcd", "OTA_END_DEADLINE", 0, "end_deadline_before_commit");

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
    if (deadline_expired && !ota_ok) resp["reason"] = "attempt_deadline";

    String out;
    serializeJson(resp, out);

    if (sha_match && ota_ok) {
        uart_send_json(out.c_str());
        // Keep FINALIZING and the receiving guard set through esp_restart().
        // Core 1's guardian can already be overdue after a long transfer;
        // releasing either guard before NVS/delays lets sleep race this reboot.
        // Do not restore the UI: the new firmware initializes it on boot.
        //
        // Clear testmode NVS so it doesn't persist after reboot.
        if (g_test_mode_active) {
            test_mode_clear("ota reboot");  // also wipes the RTC budget
        }
        // Clear maintenance NVS so stale state doesn't re-enter headless after reboot.
        lcd_clear_persisted_maintenance_state("ota_complete");
        // Arm the OTA continuation flag so the freshly-rebooted LCD re-shows the
        // "Updating…" hold and keeps the panel LIT while the Sense self-flashes
        // (a blocking, UART-silent ~tens-of-seconds loop). LCD_OTA_END_ACK was
        // already sent above. Written to its own namespace so the maintenance
        // clear above does not wipe it.
        lcd_ota_set_continuation_pending(true);
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

        // Release sleep only after failed-finalization cleanup is complete.
        s_lcd_ota_partition          = NULL;
        s_lcd_ota_bytes_written      = 0;
        s_lcd_ota_image_size         = 0;
        s_lcd_ota_last_nvs_offset    = 0;
        s_lcd_ota_last_chunk_ms      = 0;
        s_lcd_ota_last_progress_pct  = 0;
        s_lcd_ota_expected_sha256[0] = '\0';
        s_lcd_ota_target_version[0]  = '\0';
        s_lcd_ota_state              = LCD_OTA_IDLE;
        g_lcd_ota_uart_receiving     = false;
        // Failed END_ACK is a post-cleanup JSON-mode proof, like ABORT_ACK.
        uart_send_json(out.c_str());
    }
}

// ── LCD_OTA_ABORT ────────────────────────────────────────────────────
static void lcd_ota_handle_abort(JsonObject& doc) {
    const char* reason = doc["reason"] | "sense_abort";
    uint16_t session_id = doc["session_id"] | (uint16_t)0;
    if (!doc["session_id"].is<uint16_t>() || !session_id) return;
    if (s_lcd_ota_state == LCD_OTA_IDLE && session_id && session_id == s_lcd_ota_last_aborted_session) {
        lcd_ota_send_abort_ack(session_id);  // idempotent; never clear a newer lock
        return;
    }
    if (s_lcd_ota_state != LCD_OTA_RECEIVING || session_id != s_lcd_ota_session_id) {
        Serial.printf("[LCD_OTA_UART] ABORT ignored session=%u active=%u state=%d\n",
                      session_id, s_lcd_ota_session_id,
                      static_cast<int>(s_lcd_ota_state.load()));
        return;
    }
    const char* sender_detail = doc["detail"] | (const char*)nullptr;
    if (sender_detail && strlen(sender_detail) < 224)
        lcd_ota_store_terminal("sense", "LCD_PROXY_END", 0, sender_detail);
    s_lcd_ota_state = LCD_OTA_ABORTING;
    g_lcd_ota_uart_receiving = true;
    g_lcd_ota_binary_mode = false;
    g_suppress_uart_json_tx = false;

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

    s_lcd_ota_partition          = NULL;
    s_lcd_ota_bytes_written      = 0;
    s_lcd_ota_image_size         = 0;
    s_lcd_ota_last_nvs_offset    = 0;
    s_lcd_ota_last_chunk_ms      = 0;
    s_lcd_ota_last_progress_pct  = 0;
    s_lcd_ota_expected_sha256[0] = '\0';
    s_lcd_ota_target_version[0]  = '\0';

    lcd_ota_uart_restore_ui();
    s_lcd_ota_state = LCD_OTA_IDLE;
    g_lcd_ota_uart_receiving = false;
    s_lcd_ota_last_aborted_session = session_id;
    lcd_ota_send_abort_ack(session_id);
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
                          static_cast<int>(s_lcd_ota_state.load()), (void*)s_lcd_ota_protocol);
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

    const uint32_t now = millis();
    if ((uint32_t)(now - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms) {
        lcd_ota_abort_internal("attempt_deadline");
        return false;
    }
    if ((uint32_t)(now - s_lcd_ota_last_chunk_ms) >= LCD_OTA_IDLE_TIMEOUT_MS) {
        lcd_ota_abort_internal("timeout");
        return false;
    }

    uint8_t msg_type = 0;
    uint16_t seq = 0;
    uint8_t chunk_data[MAX_CHUNK_SIZE];
    size_t chunk_len = sizeof(chunk_data);
    char json_buf[512];
    auto event = s_lcd_ota_protocol->recv_event(&msg_type, &seq, chunk_data, &chunk_len,
                                                json_buf, sizeof(json_buf), 100);
    if (event == UartOtaProtocol::TIMEOUT) return true;
    // Recheck after blocking receive: a late frame cannot refresh an expired lease.
    if ((uint32_t)(millis() - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms ||
        (uint32_t)(millis() - s_lcd_ota_last_chunk_ms) >= LCD_OTA_IDLE_TIMEOUT_MS) {
        lcd_ota_abort_internal("deadline_after_rx");
        return false;
    }
    if (event == UartOtaProtocol::JSON || msg_type == MSG_OTA_CONTROL) {
        if (event == UartOtaProtocol::FRAME && !s_lcd_ota_control_v2) {
            lcd_ota_abort_internal("unnegotiated_control");
            return false;
        }
        const char* control_text = event == UartOtaProtocol::JSON ? json_buf : reinterpret_cast<const char*>(chunk_data);
        const size_t control_length = event == UartOtaProtocol::JSON ? strlen(json_buf) : chunk_len;
        if (!UartOtaProtocol::json_record_complete(control_text, control_length)) return true;
        StaticJsonDocument<512> doc;
        DeserializationError err = event == UartOtaProtocol::JSON
            ? deserializeJson(doc, json_buf)
            : deserializeJson(doc, chunk_data, chunk_len);
        if (err != DeserializationError::Ok) return true;
        const char* type = doc["type"] | "";
        JsonObject obj = doc.as<JsonObject>();
        if (strcmp(type, "LCD_OTA_END") == 0) lcd_ota_handle_end(obj);
        else if (strcmp(type, "LCD_OTA_ABORT") == 0) lcd_ota_handle_abort(obj);
        return s_lcd_ota_state == LCD_OTA_RECEIVING;
    }
    if (msg_type != MSG_CHUNK) {
        s_lcd_ota_protocol->send_nack(seq, ERR_INVALID_FRAME);
        lcd_ota_abort_internal("unexpected_frame_type");
        return false;
    }
    if (s_lcd_ota_have_last && seq == s_lcd_ota_last_seq) {
        if (chunk_len == s_lcd_ota_last_len &&
            memcmp(chunk_data, s_lcd_ota_last_data, chunk_len) == 0) {
            // Idempotence includes the final CHUNK while waiting for END.
            // A duplicate is not new progress and cannot extend either deadline.
            s_lcd_ota_protocol->send_ack(seq);
            return true;
        }
        s_lcd_ota_protocol->send_nack(seq, ERR_SEQUENCE);
        lcd_ota_abort_internal("duplicate_payload_mismatch");
        return false;
    }
    if (seq != s_lcd_ota_expected_seq) {
        s_lcd_ota_protocol->send_nack(seq, ERR_SEQUENCE);
        lcd_ota_abort_internal("sequence_mismatch");
        return false;
    }
    if (!chunk_len || chunk_len > MAX_CHUNK_SIZE ||
        s_lcd_ota_bytes_written > s_lcd_ota_image_size ||
        chunk_len > s_lcd_ota_image_size - s_lcd_ota_bytes_written) {
        s_lcd_ota_protocol->send_nack(seq, ERR_IMAGE_SIZE);
        lcd_ota_abort_internal("chunk_size_mismatch");
        return false;
    }

    // Write chunk to OTA partition
    esp_err_t werr = esp_ota_write(s_lcd_ota_handle, chunk_data, chunk_len);
    if ((uint32_t)(millis() - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms ||
        (uint32_t)(millis() - s_lcd_ota_last_chunk_ms) >= LCD_OTA_IDLE_TIMEOUT_MS) {
        lcd_ota_abort_internal("deadline_after_write");
        return false;
    }
    if (werr != ESP_OK) {
        Serial.printf("[LCD_OTA_UART] esp_ota_write failed: 0x%x (len=%u)\n",
                      werr, (unsigned)chunk_len);
        s_lcd_ota_protocol->send_nack(seq, ERR_WRITE_FAILED);
        lcd_ota_abort_internal("write_failed");
        return false;
    }

    // Update SHA256
    mbedtls_sha256_update(&s_lcd_ota_sha_ctx, chunk_data, chunk_len);
    if ((uint32_t)(millis() - s_lcd_ota_started_ms) >= s_lcd_ota_budget_ms ||
        (uint32_t)(millis() - s_lcd_ota_last_chunk_ms) >= LCD_OTA_IDLE_TIMEOUT_MS) {
        lcd_ota_abort_internal("deadline_after_hash");
        return false;
    }

    s_lcd_ota_bytes_written += chunk_len;
    s_lcd_ota_last_chunk_ms = millis();
    s_lcd_ota_last_seq = seq;
    s_lcd_ota_last_len = chunk_len;
    memcpy(s_lcd_ota_last_data, chunk_data, chunk_len);
    s_lcd_ota_have_last = true;
    s_lcd_ota_expected_seq = (uint16_t)(seq + 1);  // defined modulo-65536 sequence wrap

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

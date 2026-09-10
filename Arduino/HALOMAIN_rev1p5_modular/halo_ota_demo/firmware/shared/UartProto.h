#ifndef UART_PROTO_H
#define UART_PROTO_H

#include <HardwareSerial.h>
#include <Arduino.h>

// Coordination message types (Phase 0.5)
#define MSG_SYNC          0  // SYNC handshake (must be first, lowest value)
#define MSG_SYNC_ACK      5  // SYNC acknowledgment
#define MSG_HEARTBEAT     1
#define MSG_GET_VERSION   2
#define MSG_VERSION      3
#define MSG_STATUS        4

// NOTE: Production firmware uses newline-delimited JSON messages over UART.
// This header documents a legacy framed protocol and may not reflect the
// current JSON message set used by Sense/LCD production builds.

// Phase 2A: Wi-Fi commands
#define MSG_WIFI_ON       6  // Command LCD to enable Wi-Fi (with optional timeout)
#define MSG_WIFI_STATUS   7  // Request Wi-Fi status from LCD

// Phase 2B: OTA status updates
#define MSG_OTA_STATUS    8  // OTA status update (fetching, downloading, applying, complete, etc.)

// Phase 2C: Boot status (sent immediately after Stage-0 and after WiFi connect)
#define MSG_BOOT_STATUS   9  // Boot status: fw_version, build_id, partition label, reset_reason

// Phase 2D: Provisioning status
#define MSG_PROVISION_STATUS 10  // Provisioning status: state string

// Phase 2E: Wi-Fi credential sync (Sense -> LCD, one-time during provisioning)
#define MSG_WIFI_CREDS      11  // Sense sends SSID + password; LCD stores in NVS

// Phase 2F: OTA serialization (Sense -> LCD) – prevent Sense and LCD downloading OTA concurrently
#define MSG_OTA_LOCK        12  // Sense: "I am starting OTA download; LCD must not start LCD OTA"
#define MSG_OTA_UNLOCK      13  // Sense: "I finished OTA (success or fail); LCD may proceed with LCD OTA"

// Phase 2G: Maintenance window sync (Sense -> LCD)
#define MSG_MAINT_WINDOW_SET   14  // Sense -> LCD: JSON payload start_epoch, duration_sec, request_id, source
#define MSG_MAINT_WINDOW_CLEAR 15  // Sense -> LCD: no payload; clear stored schedule
#define MSG_MAINTENANCE_ENTER  16  // Sense -> LCD: enter no-UI maintenance mode (allow OTA)
#define MSG_MAINTENANCE_EXIT   17  // Sense -> LCD: exit maintenance mode
#define MSG_SLEEP_REQUEST      18  // LCD -> Sense: request Sense to enter deep sleep (e.g. idle timeout)

// Protocol constants
#define UART_PROTO_MAX_DATA_SIZE  256  // Maximum payload size
#define UART_PROTO_MAX_FRAME_SIZE (UART_PROTO_MAX_DATA_SIZE + 16)  // Overhead for framing
#define UART_PROTO_TIMEOUT_MS     2000
#define UART_PROTO_BOOT_FLUSH_MS  400  // Discard bytes for 400ms after UART init

// Hard header framing (Phase 0.5c)
#define UART_PROTO_MAGIC_0        0xA5
#define UART_PROTO_MAGIC_1        0x5A
#define UART_PROTO_VERSION        0x01
#define UART_PROTO_HEADER_SIZE    8    // magic[2] + proto_ver[1] + msg_type[1] + seq[2] + payload_len[2]

// COBS frame delimiter (legacy - not used in Phase 0.5c, but kept for reference)
#define UART_PROTO_DELIMITER 0x00

/**
 * UART coordination protocol for Sense <-> LCD communication.
 * 
 * Phase 0.5c: Hard header framing with magic bytes.
 * 
 * Frame format:
 *   magic[2] = 0xA5, 0x5A
 *   proto_ver[1] = 0x01
 *   msg_type[1]
 *   seq[2] little-endian
 *   payload_len[2] little-endian (0..MAX_PAYLOAD)
 *   payload[payload_len]
 *   crc16[2] over proto_ver..payload (exclude magic), little-endian
 * 
 * Receiver:
 *   - Always scan for magic (A5 5A). This is the ONLY frame start condition.
 *   - After magic, read fixed header bytes. If proto_ver mismatch -> drop and rescan.
 *   - If payload_len > MAX_PAYLOAD -> drop and rescan.
 *   - Read payload+crc; validate crc; if fail -> drop and rescan.
 */
class UartProto {
public:
  UartProto(HardwareSerial* serial);
  ~UartProto();
  
  // CRC16-CCITT
  static uint16_t crc16_ccitt(const uint8_t* data, size_t len);
  
  // Frame send/receive (Phase 0.5c: hard header framing)
  bool send_frame(uint8_t msg_type, uint16_t seq, const uint8_t* data, size_t data_len);
  bool recv_frame(uint8_t* msg_type, uint16_t* seq, uint8_t* data, size_t* data_len, 
                  unsigned long timeout_ms = UART_PROTO_TIMEOUT_MS);
  
  // Convenience methods
  bool send_sync(uint16_t seq);
  bool send_sync_ack(uint16_t seq);
  bool send_heartbeat(uint16_t seq);
  bool send_get_version(uint16_t seq);
  bool send_version(uint16_t seq, const char* version_str);
  bool send_status(uint16_t seq, const char* version, uint32_t uptime_ms, uint8_t reset_reason);
  
  // Phase 2A: Wi-Fi commands
  bool send_wifi_on(uint16_t seq, uint32_t timeout_ms);  // Command LCD to enable Wi-Fi (payload: [timeout_sec:2] LE)
  bool send_wifi_status(uint16_t seq, bool wifi_enabled, bool wifi_connected, const char* ip_str);
  
  // Phase 2E: Wi-Fi credential sync (Sense -> LCD)
  // Payload: [ssid_len:1][pass_len:1][ssid_bytes][pass_bytes] (no null terminators in wire format)
  // Limits: ssid_len 0..32, pass_len 0..63, total payload <= 256
  bool send_wifi_creds(uint16_t seq, const char* ssid, const char* pass);
  
  // Phase 2F: OTA serialization (Sense -> LCD). No payload. LCD must not start LCD OTA while locked.
  bool send_ota_lock(uint16_t seq);
  bool send_ota_unlock(uint16_t seq);

  // Phase 2G: Maintenance window (Sense -> LCD). LCD acks via existing framing and persists to NVS.
  bool send_maint_window_set(uint16_t seq, uint64_t start_epoch, uint32_t duration_sec,
                             const char* request_id, const char* source = "mqtt");
  bool send_maint_window_set_full(uint16_t seq, uint64_t start_epoch, uint32_t duration_sec,
                                  const char* request_id, uint32_t grace_before_sec, uint32_t grace_after_sec, const char* source = "mqtt");
  bool send_maint_window_clear(uint16_t seq);
  bool send_maintenance_enter(uint16_t seq);
  bool send_maintenance_exit(uint16_t seq);
  bool send_sleep_request(uint16_t seq);  // LCD -> Sense: request deep sleep

  // Parse MSG_WIFI_CREDS payload (receiver calls this when msg_type == MSG_WIFI_CREDS).
  // Fills ssid_out and pass_out with null-terminated strings; buffer sizes must be at least 33 and 64.
  // Returns true if payload valid; false on malformed (logs and leaves buffers unchanged).
  static bool parse_wifi_creds_payload(const uint8_t* data, size_t data_len,
                                       char* ssid_out, size_t ssid_size,
                                       char* pass_out, size_t pass_size);
  
  // Phase 2B: OTA status updates
  // Status format: "STATUS:fetching" | "STATUS:downloading" | "STATUS:applying" | "STATUS:complete" | "STATUS:error"
  // Optional info: version strings, progress, error messages
  bool send_ota_status(uint16_t seq, const char* status, const char* current_version = NULL, 
                       const char* target_version = NULL, uint32_t progress_bytes = 0, 
                       uint32_t total_bytes = 0, const char* error_msg = NULL);
  
  // Phase 2C: Boot status (sent immediately after Stage-0 and after WiFi connect)
  // Payload: [fw_version:null-term][build_id:null-term][partition_label:null-term][reset_reason:1][boot_count:4]
  bool send_boot_status(uint16_t seq, const char* fw_version, const char* build_id, 
                        const char* partition_label, uint8_t reset_reason, uint32_t boot_count);
  
  // Boot flush - call after UART init, discards bytes for UART_PROTO_BOOT_FLUSH_MS
  void boot_flush();
  
  // Flush RX buffer
  void flush_rx();
  
  // Check if boot flush period has elapsed
  bool is_boot_flush_complete() const { return boot_flush_complete; }

private:
  HardwareSerial* uart;
  uint8_t* rx_buffer;
  uint16_t next_seq;
  unsigned long uart_init_time_ms;
  bool boot_flush_complete;
  
  // Rate-limited error logging
  unsigned long last_error_log_ms;
  static const unsigned long ERROR_LOG_INTERVAL_MS = 1000;  // Max once per second
  
  // Scan for magic bytes (A5 5A)
  bool scan_for_magic(unsigned long timeout_ms);
  
  // Read frame after magic found
  bool read_frame_after_magic(uint8_t* msg_type, uint16_t* seq, uint8_t* data, size_t* data_len,
                              unsigned long timeout_ms);
  
  // Log error (rate-limited)
  void log_error_rate_limited(const char* fmt, ...);
};

#endif // UART_PROTO_H

#include "UartProto.h"
#include "Log.h"
#include <stdarg.h>
#include <string.h>

// CRC16-CCITT (polynomial 0x1021, initial value 0xFFFF)
uint16_t UartProto::crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int j = 0; j < 8; j++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  
  return crc;
}

UartProto::UartProto(HardwareSerial* serial) 
  : uart(serial), next_seq(1), uart_init_time_ms(millis()), 
    boot_flush_complete(false), last_error_log_ms(0) {
  rx_buffer = (uint8_t*)malloc(UART_PROTO_MAX_FRAME_SIZE * 2);
  if (!rx_buffer) {
    LOG_ERROR_TAG(LOG_TAG_UART, "Failed to allocate RX buffer!");
  }
}

UartProto::~UartProto() {
  if (rx_buffer) free(rx_buffer);
}

void UartProto::log_error_rate_limited(const char* fmt, ...) {
  unsigned long now = millis();
  if ((now - last_error_log_ms) >= ERROR_LOG_INTERVAL_MS) {
    va_list args;
    va_start(args, fmt);
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LOG_WARN_TAG(LOG_TAG_UART, "%s", buf);
    last_error_log_ms = now;
  }
}

void UartProto::boot_flush() {
  unsigned long flush_end = uart_init_time_ms + UART_PROTO_BOOT_FLUSH_MS;
  unsigned long flushed_bytes = 0;
  
  LOG_INFO_TAG(LOG_TAG_UART, "Boot flush: discarding bytes for %d ms", UART_PROTO_BOOT_FLUSH_MS);
  
  while (millis() < flush_end) {
    if (uart->available()) {
      uart->read();
      flushed_bytes++;
    }
    delay(1);
  }
  
  if (flushed_bytes > 0) {
    LOG_INFO_TAG(LOG_TAG_UART, "Boot flush: discarded %lu bytes", flushed_bytes);
  }
  
  boot_flush_complete = true;
}

bool UartProto::scan_for_magic(unsigned long timeout_ms) {
  unsigned long start = millis();
  bool found_first = false;
  
  while ((millis() - start) < timeout_ms) {
    if (uart->available()) {
      uint8_t byte = uart->read();
      
      if (!found_first) {
        // Looking for first magic byte (0xA5)
        if (byte == UART_PROTO_MAGIC_0) {
          found_first = true;
        }
      } else {
        // Found first magic byte, looking for second (0x5A)
        if (byte == UART_PROTO_MAGIC_1) {
          // Magic found!
          return true;
        } else {
          // Not 0x5A - reset search
          found_first = (byte == UART_PROTO_MAGIC_0);
        }
      }
    }
    delay(1);
  }
  
  return false;
}

bool UartProto::read_frame_after_magic(uint8_t* msg_type, uint16_t* seq, uint8_t* data, size_t* data_len,
                                       unsigned long timeout_ms) {
  if (!uart || !rx_buffer || !msg_type || !seq || !data || !data_len) return false;
  
  unsigned long start = millis();
  
  // Read fixed header (after magic): proto_ver[1] + msg_type[1] + seq[2] + payload_len[2] = 6 bytes
  size_t header_bytes_read = 0;
  uint8_t header[6];
  
  while ((millis() - start) < timeout_ms && header_bytes_read < 6) {
    if (uart->available()) {
      header[header_bytes_read++] = uart->read();
    }
    delay(1);
  }
  
  if (header_bytes_read < 6) {
    return false;  // Timeout reading header
  }
  
  // Parse header
  uint8_t proto_ver = header[0];
  *msg_type = header[1];
  *seq = header[2] | (header[3] << 8);
  uint16_t payload_len = header[4] | (header[5] << 8);
  
  // Validate protocol version
  if (proto_ver != UART_PROTO_VERSION) {
    log_error_rate_limited("Invalid proto_ver: 0x%02X (expected 0x%02X), dropping frame", 
                           proto_ver, UART_PROTO_VERSION);
    return false;
  }
  
  // Validate payload length
  if (payload_len > UART_PROTO_MAX_DATA_SIZE) {
    log_error_rate_limited("Payload too large: %d > %d, dropping frame", 
                           payload_len, UART_PROTO_MAX_DATA_SIZE);
    return false;
  }
  
  // Read payload + CRC (2 bytes)
  size_t payload_crc_bytes = payload_len + 2;
  size_t bytes_read = 0;
  
  while ((millis() - start) < timeout_ms && bytes_read < payload_crc_bytes) {
    if (uart->available()) {
      rx_buffer[bytes_read++] = uart->read();
    }
    delay(1);
  }
  
  if (bytes_read < payload_crc_bytes) {
    return false;  // Timeout reading payload+CRC
  }
  
  // Extract CRC (last 2 bytes)
  uint16_t received_crc = rx_buffer[payload_len] | (rx_buffer[payload_len + 1] << 8);
  
  // Compute CRC over: proto_ver + msg_type + seq + payload_len + payload
  // (exclude magic bytes as per spec)
  // Build CRC data in second half of buffer
  uint8_t* crc_data = rx_buffer + UART_PROTO_MAX_FRAME_SIZE;
  size_t pos = 0;
  
  crc_data[pos++] = proto_ver;
  crc_data[pos++] = *msg_type;
  crc_data[pos++] = *seq & 0xFF;
  crc_data[pos++] = (*seq >> 8) & 0xFF;
  crc_data[pos++] = payload_len & 0xFF;
  crc_data[pos++] = (payload_len >> 8) & 0xFF;
  if (payload_len > 0) {
    memcpy(&crc_data[pos], rx_buffer, payload_len);  // Payload is in rx_buffer[0..payload_len-1]
    pos += payload_len;
  }
  
  uint16_t computed_crc = crc16_ccitt(crc_data, pos);
  
  if (received_crc != computed_crc) {
    log_error_rate_limited("CRC mismatch: received=0x%04X, computed=0x%04X, dropping frame", 
                           received_crc, computed_crc);
    return false;
  }
  
  // Copy payload to output buffer
  if (payload_len > 0) {
    size_t copy_len = payload_len;
    if (copy_len > *data_len) {
      copy_len = *data_len;  // Truncate to buffer size
    }
    memcpy(data, rx_buffer, copy_len);
    *data_len = copy_len;
  } else {
    *data_len = 0;
  }
  LOG_DEBUG_TAG(LOG_TAG_UART, "Received frame: type=%d, seq=%d, data_len=%zu", 
                *msg_type, *seq, *data_len);
  return true;
}

bool UartProto::recv_frame(uint8_t* msg_type, uint16_t* seq, uint8_t* data, size_t* data_len, 
                           unsigned long timeout_ms) {
  if (!uart || !rx_buffer || !msg_type || !seq || !data || !data_len) return false;
  
  // Phase 0.5c: Always scan for magic bytes first
  if (!scan_for_magic(timeout_ms)) {
    return false;  // Magic not found within timeout
  }
  
  // Magic found, read frame
  return read_frame_after_magic(msg_type, seq, data, data_len, timeout_ms);
}

bool UartProto::send_frame(uint8_t msg_type, uint16_t seq, const uint8_t* data, size_t data_len) {
  if (!uart || !rx_buffer) return false;
  
  if (data_len > UART_PROTO_MAX_DATA_SIZE) {
    LOG_ERROR_TAG(LOG_TAG_UART, "Data too large: %zu bytes", data_len);
    return false;
  }
  
  // Build frame: magic[2] + proto_ver[1] + msg_type[1] + seq[2] + payload_len[2] + payload[N] + crc[2]
  uint8_t* frame = rx_buffer;  // Reuse buffer
  size_t pos = 0;
  
  // Magic bytes
  frame[pos++] = UART_PROTO_MAGIC_0;
  frame[pos++] = UART_PROTO_MAGIC_1;
  
  // Header (will compute CRC over this + payload)
  frame[pos++] = UART_PROTO_VERSION;  // proto_ver
  frame[pos++] = msg_type;
  frame[pos++] = seq & 0xFF;
  frame[pos++] = (seq >> 8) & 0xFF;
  frame[pos++] = data_len & 0xFF;
  frame[pos++] = (data_len >> 8) & 0xFF;
  
  // Payload
  if (data && data_len > 0) {
    memcpy(&frame[pos], data, data_len);
    pos += data_len;
  }
  
  // Compute CRC over: proto_ver + msg_type + seq + payload_len + payload
  // (exclude magic bytes)
  uint16_t crc = crc16_ccitt(&frame[2], pos - 2);  // Skip magic bytes
  
  // Append CRC
  frame[pos++] = crc & 0xFF;
  frame[pos++] = (crc >> 8) & 0xFF;
  
  // Send frame
  size_t written = uart->write(frame, pos);
  uart->flush();
  
  if (written != pos) {
    LOG_ERROR_TAG(LOG_TAG_UART, "Failed to send frame: wrote %zu/%zu bytes", written, pos);
    return false;
  }
  
  LOG_DEBUG_TAG(LOG_TAG_UART, "Sent frame: type=%d, seq=%d, data_len=%zu", msg_type, seq, data_len);
  return true;
}

bool UartProto::send_sync(uint16_t seq) {
  return send_frame(MSG_SYNC, seq, NULL, 0);
}

bool UartProto::send_sync_ack(uint16_t seq) {
  return send_frame(MSG_SYNC_ACK, seq, NULL, 0);
}

bool UartProto::send_heartbeat(uint16_t seq) {
  return send_frame(MSG_HEARTBEAT, seq, NULL, 0);
}

bool UartProto::send_get_version(uint16_t seq) {
  return send_frame(MSG_GET_VERSION, seq, NULL, 0);
}

bool UartProto::send_version(uint16_t seq, const char* version_str) {
  if (!version_str) return false;
  size_t len = strlen(version_str);
  if (len > UART_PROTO_MAX_DATA_SIZE) len = UART_PROTO_MAX_DATA_SIZE;
  return send_frame(MSG_VERSION, seq, (const uint8_t*)version_str, len);
}

bool UartProto::send_status(uint16_t seq, const char* version, uint32_t uptime_ms, uint8_t reset_reason) {
  if (!version) return false;
  
  // Pack status: [version_str:null-term][uptime_ms:4][reset_reason:1]
  // Limit version string to keep STATUS small (<=64 bytes total)
  uint8_t data[64];  // Fixed small buffer for STATUS
  size_t pos = 0;
  
  size_t version_len = strlen(version);
  if (version_len > 50) version_len = 50;  // Limit to 50 chars to keep total < 64 bytes
  if (version_len + 5 > sizeof(data)) {
    LOG_ERROR_TAG(LOG_TAG_UART, "Status data too large");
    return false;
  }
  
  memcpy(&data[pos], version, version_len);
  pos += version_len;
  data[pos++] = 0;  // Null terminator
  
  data[pos++] = uptime_ms & 0xFF;
  data[pos++] = (uptime_ms >> 8) & 0xFF;
  data[pos++] = (uptime_ms >> 16) & 0xFF;
  data[pos++] = (uptime_ms >> 24) & 0xFF;
  
  data[pos++] = reset_reason;
  
  return send_frame(MSG_STATUS, seq, data, pos);
}

void UartProto::flush_rx() {
  if (uart) {
    while (uart->available()) {
      uart->read();
    }
  }
}

// MSG_WIFI_CREDS length limits (payload = 2 + ssid_len + pass_len <= 256)
#define WIFI_CREDS_SSID_MAX  32
#define WIFI_CREDS_PASS_MAX  63

bool UartProto::send_wifi_creds(uint16_t seq, const char* ssid, const char* pass) {
  uint8_t payload[2 + WIFI_CREDS_SSID_MAX + WIFI_CREDS_PASS_MAX];
  size_t ssid_len = ssid ? strlen(ssid) : 0;
  size_t pass_len = pass ? strlen(pass) : 0;
  if (ssid_len > WIFI_CREDS_SSID_MAX) ssid_len = WIFI_CREDS_SSID_MAX;
  if (pass_len > WIFI_CREDS_PASS_MAX) pass_len = WIFI_CREDS_PASS_MAX;
  size_t total = 2 + ssid_len + pass_len;
  if (total > UART_PROTO_MAX_DATA_SIZE) {
    LOG_ERROR_TAG(LOG_TAG_UART, "[WIFI_CREDS] payload too large: %zu", total);
    return false;
  }
  payload[0] = (uint8_t)ssid_len;
  payload[1] = (uint8_t)pass_len;
  if (ssid_len > 0 && ssid) memcpy(&payload[2], ssid, ssid_len);
  if (pass_len > 0 && pass) memcpy(&payload[2 + ssid_len], pass, pass_len);
  LOG_INFO_TAG(LOG_TAG_UART, "[WIFI_CREDS] send ssid_len=%u pass_len=%u", (unsigned)ssid_len, (unsigned)pass_len);
  return send_frame(MSG_WIFI_CREDS, seq, payload, total);
}

bool UartProto::parse_wifi_creds_payload(const uint8_t* data, size_t data_len,
                                         char* ssid_out, size_t ssid_size,
                                         char* pass_out, size_t pass_size) {
  if (!data || !ssid_out || !pass_out || ssid_size < 1 || pass_size < 1) {
    return false;
  }
  ssid_out[0] = '\0';
  pass_out[0] = '\0';
  if (data_len < 2) {
    LOG_WARN_TAG(LOG_TAG_UART, "[WIFI_CREDS] recv malformed: payload_len=%zu (need >= 2)", data_len);
    return false;
  }
  uint8_t ssid_len = data[0];
  uint8_t pass_len = data[1];
  if (ssid_len > WIFI_CREDS_SSID_MAX || pass_len > WIFI_CREDS_PASS_MAX) {
    LOG_WARN_TAG(LOG_TAG_UART, "[WIFI_CREDS] recv malformed: ssid_len=%u pass_len=%u (max 32, 63)", ssid_len, pass_len);
    return false;
  }
  size_t expected = 2 + ssid_len + pass_len;
  if (data_len != expected) {
    LOG_WARN_TAG(LOG_TAG_UART, "[WIFI_CREDS] recv malformed: payload_len=%zu != 2+ssid+pass=%zu", data_len, expected);
    return false;
  }
  if (ssid_len > 0) {
    if (ssid_len >= ssid_size) {
      LOG_WARN_TAG(LOG_TAG_UART, "[WIFI_CREDS] recv malformed: ssid_len=%u >= ssid_size=%zu", ssid_len, ssid_size);
      return false;
    }
    memcpy(ssid_out, &data[2], ssid_len);
    ssid_out[ssid_len] = '\0';
  }
  if (pass_len > 0) {
    if (pass_len >= pass_size) {
      LOG_WARN_TAG(LOG_TAG_UART, "[WIFI_CREDS] recv malformed: pass_len=%u >= pass_size=%zu", pass_len, pass_size);
      return false;
    }
    memcpy(pass_out, &data[2 + ssid_len], pass_len);
    pass_out[pass_len] = '\0';
  }
  LOG_INFO_TAG(LOG_TAG_UART, "[WIFI_CREDS] recv ssid_len=%u pass_len=%u", (unsigned)ssid_len, (unsigned)pass_len);
  return true;
}

bool UartProto::send_ota_lock(uint16_t seq) {
  return send_frame(MSG_OTA_LOCK, seq, NULL, 0);
}

bool UartProto::send_ota_unlock(uint16_t seq) {
  return send_frame(MSG_OTA_UNLOCK, seq, NULL, 0);
}

bool UartProto::send_maint_window_set(uint16_t seq, uint64_t start_epoch, uint32_t duration_sec,
                                      const char* request_id, const char* source) {
  return send_maint_window_set_full(seq, start_epoch, duration_sec, request_id, 60, 600, source);
}

bool UartProto::send_maint_window_set_full(uint16_t seq, uint64_t start_epoch, uint32_t duration_sec,
                                           const char* request_id, uint32_t grace_before_sec, uint32_t grace_after_sec, const char* source) {
  char payload[UART_PROTO_MAX_DATA_SIZE];
  int n = snprintf(payload, sizeof(payload),
                   "{\"start_epoch\":%llu,\"duration_sec\":%lu,\"request_id\":\"%s\",\"grace_before_sec\":%lu,\"grace_after_sec\":%lu,\"source\":\"%s\"}",
                   (unsigned long long)start_epoch, (unsigned long)duration_sec,
                   request_id ? request_id : "", (unsigned long)grace_before_sec, (unsigned long)grace_after_sec,
                   source ? source : "mqtt");
  if (n < 0 || (size_t)n >= sizeof(payload)) {
    return false;
  }
  return send_frame(MSG_MAINT_WINDOW_SET, seq, (const uint8_t*)payload, (size_t)n);
}

bool UartProto::send_maint_window_clear(uint16_t seq) {
  return send_frame(MSG_MAINT_WINDOW_CLEAR, seq, NULL, 0);
}

bool UartProto::send_maintenance_enter(uint16_t seq) {
  return send_frame(MSG_MAINTENANCE_ENTER, seq, NULL, 0);
}

bool UartProto::send_maintenance_exit(uint16_t seq) {
  return send_frame(MSG_MAINTENANCE_EXIT, seq, NULL, 0);
}

bool UartProto::send_sleep_request(uint16_t seq) {
  return send_frame(MSG_SLEEP_REQUEST, seq, NULL, 0);
}

bool UartProto::send_wifi_on(uint16_t seq, uint32_t timeout_ms) {
  // Payload: [timeout_sec:2] little-endian (0 = use LCD default)
  uint16_t timeout_sec = (timeout_ms == 0) ? 0 : (uint16_t)(timeout_ms / 1000);
  if (timeout_ms > 0 && timeout_sec == 0) {
    timeout_sec = 1;  // At least 1 second if any timeout requested
  }
  uint8_t data[2];
  data[0] = timeout_sec & 0xFF;
  data[1] = (timeout_sec >> 8) & 0xFF;
  return send_frame(MSG_WIFI_ON, seq, data, sizeof(data));
}

bool UartProto::send_wifi_status(uint16_t seq, bool wifi_enabled, bool wifi_connected, const char* ip_str) {
  // Pack status: [wifi_enabled:1][wifi_connected:1][ip_str:null-term]
  uint8_t data[64];  // Fixed small buffer
  size_t pos = 0;
  
  data[pos++] = wifi_enabled ? 1 : 0;
  data[pos++] = wifi_connected ? 1 : 0;
  
  if (ip_str) {
    size_t ip_len = strlen(ip_str);
    if (ip_len > 50) ip_len = 50;  // Limit to 50 chars
    if (pos + ip_len + 1 > sizeof(data)) {
      LOG_ERROR_TAG(LOG_TAG_UART, "Wi-Fi status data too large");
      return false;
    }
    memcpy(&data[pos], ip_str, ip_len);
    pos += ip_len;
  }
  data[pos++] = 0;  // Null terminator
  
  return send_frame(MSG_WIFI_STATUS, seq, data, pos);
}

bool UartProto::send_ota_status(uint16_t seq, const char* status, const char* current_version, 
                                 const char* target_version, uint32_t progress_bytes, 
                                 uint32_t total_bytes, const char* error_msg) {
  if (!status) return false;
  
  // Pack OTA status: [status:null-term][current_version:null-term][target_version:null-term][progress:4][total:4][error:null-term]
  // Format: "STATUS:fetching" | "STATUS:downloading" | "STATUS:applying" | "STATUS:complete" | "STATUS:error"
  uint8_t data[256];  // Larger buffer for OTA status with versions
  size_t pos = 0;
  
  // Status string
  size_t status_len = strlen(status);
  if (status_len > 50) status_len = 50;
  if (pos + status_len + 1 > sizeof(data)) {
    LOG_ERROR_TAG(LOG_TAG_UART, "OTA status data too large");
    return false;
  }
  memcpy(&data[pos], status, status_len);
  pos += status_len;
  data[pos++] = 0;  // Null terminator
  
  // Current version (optional)
  if (current_version) {
    size_t cv_len = strlen(current_version);
    if (cv_len > 32) cv_len = 32;
    if (pos + cv_len + 1 > sizeof(data)) {
      LOG_ERROR_TAG(LOG_TAG_UART, "OTA status data too large");
      return false;
    }
    memcpy(&data[pos], current_version, cv_len);
    pos += cv_len;
  }
  data[pos++] = 0;  // Null terminator (even if current_version was NULL)
  
  // Target version (optional)
  if (target_version) {
    size_t tv_len = strlen(target_version);
    if (tv_len > 32) tv_len = 32;
    if (pos + tv_len + 1 > sizeof(data)) {
      LOG_ERROR_TAG(LOG_TAG_UART, "OTA status data too large");
      return false;
    }
    memcpy(&data[pos], target_version, tv_len);
    pos += tv_len;
  }
  data[pos++] = 0;  // Null terminator (even if target_version was NULL)
  
  // Progress bytes (4 bytes, little-endian)
  data[pos++] = progress_bytes & 0xFF;
  data[pos++] = (progress_bytes >> 8) & 0xFF;
  data[pos++] = (progress_bytes >> 16) & 0xFF;
  data[pos++] = (progress_bytes >> 24) & 0xFF;
  
  // Total bytes (4 bytes, little-endian)
  data[pos++] = total_bytes & 0xFF;
  data[pos++] = (total_bytes >> 8) & 0xFF;
  data[pos++] = (total_bytes >> 16) & 0xFF;
  data[pos++] = (total_bytes >> 24) & 0xFF;
  
  // Error message (optional)
  if (error_msg) {
    size_t err_len = strlen(error_msg);
    if (err_len > 64) err_len = 64;
    if (pos + err_len + 1 > sizeof(data)) {
      LOG_ERROR_TAG(LOG_TAG_UART, "OTA status data too large");
      return false;
    }
    memcpy(&data[pos], error_msg, err_len);
    pos += err_len;
  }
  data[pos++] = 0;  // Null terminator (even if error_msg was NULL)
  
  return send_frame(MSG_OTA_STATUS, seq, data, pos);
}

bool UartProto::send_boot_status(uint16_t seq, const char* fw_version, const char* build_id, 
                                  const char* partition_label, uint8_t reset_reason, uint32_t boot_count) {
  if (!fw_version) return false;
  
  // Pack boot status: [fw_version:null-term][build_id:null-term][partition_label:null-term][reset_reason:1][boot_count:4]
  uint8_t data[256];  // Buffer for boot status
  size_t pos = 0;
  
  // Firmware version
  size_t fw_len = strlen(fw_version);
  if (fw_len > 32) fw_len = 32;
  if (pos + fw_len + 1 > sizeof(data)) {
    LOG_ERROR_TAG(LOG_TAG_UART, "Boot status data too large");
    return false;
  }
  memcpy(&data[pos], fw_version, fw_len);
  pos += fw_len;
  data[pos++] = 0;  // Null terminator
  
  // Build ID (optional)
  if (build_id) {
    size_t bid_len = strlen(build_id);
    if (bid_len > 64) bid_len = 64;
    if (pos + bid_len + 1 > sizeof(data)) {
      LOG_ERROR_TAG(LOG_TAG_UART, "Boot status data too large");
      return false;
    }
    memcpy(&data[pos], build_id, bid_len);
    pos += bid_len;
  }
  data[pos++] = 0;  // Null terminator (even if build_id was NULL)
  
  // Partition label (optional)
  if (partition_label) {
    size_t part_len = strlen(partition_label);
    if (part_len > 16) part_len = 16;  // "app0" or "app1" is max 4 chars
    if (pos + part_len + 1 > sizeof(data)) {
      LOG_ERROR_TAG(LOG_TAG_UART, "Boot status data too large");
      return false;
    }
    memcpy(&data[pos], partition_label, part_len);
    pos += part_len;
  }
  data[pos++] = 0;  // Null terminator (even if partition_label was NULL)
  
  // Reset reason (1 byte)
  data[pos++] = reset_reason;
  
  // Boot count (4 bytes, little-endian)
  if (pos + 4 > sizeof(data)) {
    LOG_ERROR_TAG(LOG_TAG_UART, "Boot status data too large");
    return false;
  }
  data[pos++] = boot_count & 0xFF;
  data[pos++] = (boot_count >> 8) & 0xFF;
  data[pos++] = (boot_count >> 16) & 0xFF;
  data[pos++] = (boot_count >> 24) & 0xFF;
  
  return send_frame(MSG_BOOT_STATUS, seq, data, pos);
}

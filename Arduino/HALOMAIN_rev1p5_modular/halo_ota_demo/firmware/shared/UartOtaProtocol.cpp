#include "UartOtaProtocol.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// COBS (Consistent Overhead Byte Stuffing) encoding
size_t UartOtaProtocol::cobs_encode(const uint8_t* input, size_t input_len, uint8_t* output) {
  if (input_len == 0) return 0;
  
  size_t read_index = 0;
  size_t write_index = 1;
  size_t code_index = 0;
  uint8_t code = 1;
  
  while (read_index < input_len) {
    if (input[read_index] == 0) {
      output[code_index] = code;
      code_index = write_index++;
      code = 1;
    } else {
      output[write_index++] = input[read_index];
      code++;
      if (code == 0xFF) {
        output[code_index] = code;
        code_index = write_index++;
        code = 1;
      }
    }
    read_index++;
  }
  
  output[code_index] = code;
  output[write_index++] = 0;  // Frame delimiter
  return write_index;
}

// COBS decoding
size_t UartOtaProtocol::cobs_decode(const uint8_t* input, size_t input_len, uint8_t* output) {
  if (input_len == 0) return 0;
  
  size_t read_index = 0;
  size_t write_index = 0;
  uint8_t code = 0;
  uint8_t i = 0;
  
  while (read_index < input_len) {
    code = input[read_index++];
    if (code == 0 || (size_t)(code - 1) > input_len - read_index) return 0;
    
    for (i = 1; i < code && read_index < input_len; i++) {
      output[write_index++] = input[read_index++];
    }
    
    if (code < 0xFF && read_index < input_len) {
      output[write_index++] = 0;
    }
  }
  
  return write_index;
}

bool UartOtaProtocol::json_record_complete(const char* text, size_t length) {
  if (!text || !length) return false;
  size_t start = 0;
  while (start < length && (text[start] == ' ' || text[start] == '\t' ||
                            text[start] == '\r' || text[start] == '\n')) ++start;
  if (start == length || text[start] != '{') return false;
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (size_t i = start; i < length; ++i) {
    const char c = text[i];
    if (!c) return false;
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
      continue;
    }
    if (c == '"') { quoted = true; continue; }
    if (c == '{' || c == '[') ++depth;
    if (c == '}' || c == ']') {
      if (!depth) return false;
      if (--depth == 0) {
        if (c != '}') return false;
        for (++i; i < length; ++i) {
          if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') return false;
        }
        return true;
      }
    }
  }
  return false;
}

// CRC16-CCITT (polynomial 0x1021, initial value 0xFFFF)
uint16_t UartOtaProtocol::crc16_ccitt(const uint8_t* data, size_t len) {
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

bool UartOtaProtocol::send_frame(uint8_t msg_type, uint16_t seq, const uint8_t* data, size_t data_len) {
  if (!uart || !rx_buffer) return false;

  // Build frame payload (before COBS)
  size_t payload_len = 1 + 2 + 2 + data_len + 2;  // type + seq + len + data + crc
  if (payload_len > MAX_FRAME_SIZE) {
    Serial.printf("[UART_OTA] Frame too large: %zu bytes\n", payload_len);
    return false;
  }
  
  uint8_t* payload = &rx_buffer[MAX_ENCODED_FRAME_SIZE];  // TX scratch is separate from partial RX
  size_t pos = 0;
  
  payload[pos++] = msg_type;
  payload[pos++] = seq & 0xFF;
  payload[pos++] = (seq >> 8) & 0xFF;
  payload[pos++] = data_len & 0xFF;
  payload[pos++] = (data_len >> 8) & 0xFF;
  if (data_len > 0) {
    if (!data) {
      Serial.println("[UART_OTA] send_frame: data_len>0 but data==NULL");
      return false;
    }
    memcpy(&payload[pos], data, data_len);
    pos += data_len;
  }
  
  // Compute CRC over (type + seq + len + data)
  uint16_t crc = crc16_ccitt(payload, pos);
  payload[pos++] = crc & 0xFF;
  payload[pos++] = (crc >> 8) & 0xFF;
  
  // COBS encode
  uint8_t* encoded = &rx_buffer[MAX_ENCODED_FRAME_SIZE * 2];
  size_t encoded_len = cobs_encode(payload, pos, encoded);
  
  // Send
  if (tx_needs_sync) {
    const uint8_t delimiter = 0;
    if (uart->write(&delimiter, 1) != 1) return false;
  }
  const size_t written = uart->write(encoded, encoded_len);
  tx_needs_sync = written != encoded_len;
  uart->flush();
  if (tx_needs_sync) return false;
  
  if (!quiet) {
    Serial.printf("[UART_OTA] TX: type=%d, seq=%d, len=%zu, encoded=%zu\n",
                  msg_type, seq, data_len, encoded_len);
  }
  return true;
}

bool UartOtaProtocol::recv_frame(uint8_t* msg_type, uint16_t* seq, uint8_t* data, size_t* data_len, unsigned long timeout_ms) {
  return recv_event(msg_type, seq, data, data_len, nullptr, 0, timeout_ms) == FRAME;
}

UartOtaProtocol::ReceiveEvent UartOtaProtocol::recv_event(
    uint8_t* msg_type, uint16_t* seq, uint8_t* data, size_t* data_len,
    char* json, size_t json_capacity, unsigned long timeout_ms) {
  if (!valid() || !msg_type || !seq || !data || !data_len) return TIMEOUT;
  const uint32_t start = millis();
  const bool allow_json = json && json_capacity > 1;
  while ((uint32_t)(millis() - start) < timeout_ms) {
    // A fragmented record has its own absolute lifetime, independent of polls.
    if (rx_kind != 0 && rx_kind < 3 &&
        (uint32_t)(millis() - rx_started_ms) >= FRAME_TIMEOUT_MS) {
      rx_kind = rx_kind == 2 ? 4 : 3;
      rx_buffer_pos = 0;
      ++frame_errors;
    }
    if (!uart->available()) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    const uint8_t byte = uart->read();
    if (rx_skip_lf) {
      rx_skip_lf = false;
      if (byte == '\n') continue;
    }
    if (rx_kind == 3 || rx_kind == 4) {
      if ((rx_kind == 3 && byte == 0) ||
          (rx_kind == 4 && (byte == '\n' || byte == '\r' || byte == 0))) {
        rx_skip_lf = rx_kind == 4 && byte == '\r';
        rx_kind = 0;
      }
      continue;
    }
    if (rx_kind == 0) {
      if (byte == 0) continue;
      rx_started_ms = millis();
      rx_kind = 1;
      rx_buffer_pos = 0;
    }
    // An OTA COBS record may begin with code 0x7b ('{'), but its next byte is
    // the non-text message type. Wait for the second byte before selecting JSON.
    if (allow_json && rx_kind == 1 && rx_buffer_pos == 1 && rx_buffer[0] == '{' &&
        (byte == '"' || byte == ' ' || byte == '\t')) rx_kind = 2;
    if (rx_kind == 2) {
      if (byte == '\n' || byte == '\r') {
        rx_skip_lf = byte == '\r';
        if (rx_buffer_pos < json_capacity) {
          memcpy(json, rx_buffer, rx_buffer_pos);
          json[rx_buffer_pos] = '\0';
          rx_buffer_pos = 0;
          rx_kind = 0;
          return JSON;
        }
        ++frame_errors;
        rx_buffer_pos = 0;
        rx_kind = 0;
        continue;
      }
      if (byte == 0 || rx_buffer_pos >= MAX_ENCODED_FRAME_SIZE ||
          rx_buffer_pos + 1 >= json_capacity) {
        ++frame_errors;
        rx_buffer_pos = 0;
        rx_kind = byte == 0 ? 0 : 4;
        continue;
      }
      rx_buffer[rx_buffer_pos++] = byte;
      continue;
    }
    if (byte != 0) {
      if (rx_buffer_pos < MAX_ENCODED_FRAME_SIZE) rx_buffer[rx_buffer_pos++] = byte;
      else { rx_buffer_pos = 0; rx_kind = 3; ++frame_errors; }
      continue;
    }
    uint8_t* decoded = &rx_buffer[MAX_ENCODED_FRAME_SIZE];
    const size_t decoded_len = cobs_decode(rx_buffer, rx_buffer_pos, decoded);
    rx_buffer_pos = 0;
    rx_kind = 0;
    if (decoded_len < 7 || decoded_len > MAX_FRAME_SIZE) { ++frame_errors; continue; }
    const uint16_t recv_seq = (uint16_t)(decoded[1] | ((uint16_t)decoded[2] << 8));
    const uint16_t recv_len = (uint16_t)(decoded[3] | ((uint16_t)decoded[4] << 8));
    const size_t expected_total = 7 + (size_t)recv_len;
    if (expected_total != decoded_len || recv_len > *data_len) { ++frame_errors; continue; }
    const uint16_t recv_crc = (uint16_t)(decoded[expected_total - 2] |
                                       ((uint16_t)decoded[expected_total - 1] << 8));
    if (crc16_ccitt(decoded, expected_total - 2) != recv_crc) { ++crc_errors; continue; }
    if (recv_len) memcpy(data, decoded + 5, recv_len);
    *msg_type = decoded[0];
    *seq = recv_seq;
    *data_len = recv_len;
    return FRAME;
  }
  return TIMEOUT;
}

bool UartOtaProtocol::send_ack(uint16_t seq) {
  return send_frame(MSG_ACK, seq, NULL, 0);
}

bool UartOtaProtocol::send_nack(uint16_t seq, uint8_t error_code) {
  uint8_t data[1] = {error_code};
  return send_frame(MSG_NACK, seq, data, 1);
}

bool UartOtaProtocol::send_error(uint8_t error_code, const char* message) {
  uint8_t data[64] = {error_code};
  size_t len = 1;
  if (message) {
    size_t msg_len = strlen(message);
    if (msg_len > 62) msg_len = 62;
    memcpy(&data[1], message, msg_len);
    len = 1 + msg_len;
  }
  return send_frame(MSG_ERROR, 0, data, len);
}

void UartOtaProtocol::flush_rx() {
  while (uart->available()) {
    uart->read();
  }
  rx_buffer_pos = 0;
  rx_kind = 0;
  rx_skip_lf = false;
}



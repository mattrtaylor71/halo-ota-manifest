#ifndef UART_OTA_PROTOCOL_H
#define UART_OTA_PROTOCOL_H

#include <HardwareSerial.h>
#include <Arduino.h>

// Message types — firmware transfer (Sense -> LCD OTA proxy)
#define MSG_BEGIN  1
#define MSG_CHUNK  2
#define MSG_END    3
#define MSG_ACK    4
#define MSG_NACK   5
#define MSG_ERROR  6
// Negotiated OTA v2 control: JSON payload inside a CRC-protected COBS frame.
#define MSG_OTA_CONTROL 7
#define OTA_UART_PROTOCOL_VERSION 2

// Message types — capture-image spool (Sense -> LCD SD card).
// DELIBERATELY DISTINCT from the firmware types above. The two transfers share
// this link and this framing, and the consequences of confusing them are not
// symmetric: a firmware frame written to a .jpg is a corrupt photo, but an
// image frame written to an OTA partition is a bricked board. Distinct type
// bytes make that mix-up impossible rather than merely unlikely.
#define MSG_IMG_BEGIN  0x11
#define MSG_IMG_CHUNK  0x12
#define MSG_IMG_END    0x13
#define MSG_IMG_ACK    0x14
#define MSG_IMG_NACK   0x15

// Error codes
#define ERR_NONE        0
#define ERR_INVALID_FRAME 1
#define ERR_CRC_MISMATCH  2
#define ERR_WRITE_FAILED  3
#define ERR_SHA_MISMATCH  4
#define ERR_OUT_OF_MEMORY 5
#define ERR_SEQUENCE     6
#define ERR_IMAGE_SIZE   7

// Protocol constants
// NOTE: MAX_CHUNK_SIZE must match on BOTH boards.
// 512B is fine at 115200 baud (~55ms/frame incl. COBS) and gives decent throughput.
#define MAX_CHUNK_SIZE 512
#define MAX_FRAME_SIZE (MAX_CHUNK_SIZE + 64)  // type+seq+len+crc + BEGIN overhead
#define MAX_ENCODED_FRAME_SIZE (MAX_FRAME_SIZE + MAX_FRAME_SIZE / 254 + 2)
#define MAX_RETRIES 5
#define FRAME_TIMEOUT_MS 5000

class UartOtaProtocol {
public:
  UartOtaProtocol(HardwareSerial* serial) : quiet(false), uart(serial), rx_buffer_pos(0),
      rx_kind(0), rx_started_ms(0), rx_skip_lf(false), tx_needs_sync(false), crc_errors(0), frame_errors(0) {
    rx_buffer = (uint8_t*)malloc(MAX_ENCODED_FRAME_SIZE * 3);  // RX + decode/TX payload + encoded TX; never overwrite partial RX
    if (!rx_buffer) {
      Serial.println("[UART_OTA] Failed to allocate RX buffer!");
    }
  }

  // When true, suppress per-frame TX/RX logs (huge throughput win on USB CDC)
  bool quiet;

  ~UartOtaProtocol() {
    if (rx_buffer) free(rx_buffer);
  }

  // COBS encoding/decoding
  static size_t cobs_encode(const uint8_t* input, size_t input_len, uint8_t* output);
  static size_t cobs_decode(const uint8_t* input, size_t input_len, uint8_t* output);

  // Require exactly one complete JSON object (no trailing record or garbage).
  static bool json_record_complete(const char* text, size_t length);

  // CRC16-CCITT
  static uint16_t crc16_ccitt(const uint8_t* data, size_t len);

  // Frame send/receive
  bool send_frame(uint8_t msg_type, uint16_t seq, const uint8_t* data, size_t data_len);
  bool recv_frame(uint8_t* msg_type, uint16_t* seq, uint8_t* data, size_t* data_len, unsigned long timeout_ms = FRAME_TIMEOUT_MS);

  enum ReceiveEvent { TIMEOUT, FRAME, JSON };
  // Incremental OTA demultiplexer. Partial records survive calls; malformed or
  // oversized records are discarded through their delimiter. JSON is opt-in.
  ReceiveEvent recv_event(uint8_t* msg_type, uint16_t* seq, uint8_t* data,
                          size_t* data_len, char* json, size_t json_capacity,
                          unsigned long timeout_ms = FRAME_TIMEOUT_MS);
  bool valid() const { return uart && rx_buffer; }
  uint32_t crc_error_count() const { return crc_errors; }
  uint32_t frame_error_count() const { return frame_errors; }

  // Helper: send ACK/NACK
  bool send_ack(uint16_t seq);
  bool send_nack(uint16_t seq, uint8_t error_code);
  bool send_error(uint8_t error_code, const char* message = NULL);

private:
  HardwareSerial* uart;
  uint8_t* rx_buffer;
  size_t rx_buffer_pos;
  uint8_t rx_kind;  // 0 boundary, 1 binary/candidate, 2 JSON, 3/4 discard binary/JSON
  uint32_t rx_started_ms;
  bool rx_skip_lf;
  bool tx_needs_sync;
  uint32_t crc_errors;
  uint32_t frame_errors;

  void flush_rx();
};

#endif // UART_OTA_PROTOCOL_H




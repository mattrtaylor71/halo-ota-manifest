#pragma once

namespace HaloPins {
  static const uint32_t kHaloPinsSignature = 0xA5A5F00D;

  static const int kLcdUartTxPin = 38;    // LCD TX
  static const int kLcdUartRxPin = 48;    // LCD RX
  static const int kSenseUartTxPin = 43;  // Sense TX (D6 on XIAO ESP32S3)
  static const int kSenseUartRxPin = 44;  // Sense RX (D7 on XIAO ESP32S3)
}  // namespace HaloPins

#ifndef UART_TRANSPORT_H
#define UART_TRANSPORT_H

#include <Arduino.h>
#include <string.h>

// ============================================================================
// UartTransport - Ring buffer line reader for reliable UART communication
// ============================================================================
// Features:
// - Ring buffer with max 512 bytes
// - Line-based reading (newline-delimited)
// - Non-blocking operation
// ============================================================================

class UartTransport {
private:
  HardwareSerial* serial;
  static const size_t BUFFER_SIZE = 512;
  char buffer[BUFFER_SIZE];
  size_t head;  // Write position
  size_t tail;  // Read position
  bool overflow;

public:
  UartTransport(HardwareSerial* s) : serial(s), head(0), tail(0), overflow(false) {
    memset(buffer, 0, BUFFER_SIZE);
  }

  // Call this regularly in loop() to read bytes from UART into ring buffer
  void update() {
    while (serial->available() > 0) {
      char c = serial->read();
      
      // Check for buffer overflow
      size_t nextHead = (head + 1) % BUFFER_SIZE;
      if (nextHead == tail) {
        overflow = true;
        // Drop oldest byte (advance tail)
        tail = (tail + 1) % BUFFER_SIZE;
      }
      
      buffer[head] = c;
      head = nextHead;
    }
  }

  // Check if a complete line is available (ends with \n or \r\n)
  bool hasLine() {
    // Search forward from tail to head for newline
    size_t pos = tail;
    size_t count = 0;
    
    // Look for \n within reasonable distance (max line length)
    while (pos != head && count < 256) {
      if (buffer[pos] == '\n') {
        return true;
      }
      pos = (pos + 1) % BUFFER_SIZE;
      count++;
    }
    return false;
  }

  // Read a complete line (up to maxLen chars, null-terminated)
  // Returns empty string if no line available
  String readLine(size_t maxLen = 256) {
    if (!hasLine()) {
      return String("");
    }

    String line = "";
    size_t pos = tail;
    size_t count = 0;

    while (pos != head && count < maxLen && count < BUFFER_SIZE) {
      char c = buffer[pos];
      
      if (c == '\n' || c == '\r') {
        // Skip remaining \r if present
        if (c == '\r' && pos != head) {
          size_t nextPos = (pos + 1) % BUFFER_SIZE;
          if (nextPos != head && buffer[nextPos] == '\n') {
            pos = nextPos;
          }
        }
        // Advance tail past newline
        tail = (pos + 1) % BUFFER_SIZE;
        break;
      }
      
      line += c;
      pos = (pos + 1) % BUFFER_SIZE;
      count++;
    }

    // Trim whitespace
    line.trim();
    return line;
  }

  // Send a line (appends newline automatically)
  void sendLine(const String& line) {
    serial->print(line);
    serial->print('\n');
    serial->flush();
  }

  // Check if buffer overflow occurred (for debugging)
  bool hasOverflow() {
    return overflow;
  }

  // Clear overflow flag
  void clearOverflow() {
    overflow = false;
  }

  // Get available space in buffer
  size_t availableSpace() {
    if (head >= tail) {
      return BUFFER_SIZE - (head - tail) - 1;
    } else {
      return tail - head - 1;
    }
  }
};

#endif // UART_TRANSPORT_H


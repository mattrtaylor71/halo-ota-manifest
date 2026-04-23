/*
 * HALO Diagnostic Firmware — LCD Board
 *
 * Bare-bones: UART bridge to USB Serial + INT pin control + heartbeat monitor.
 * No sleep, no LVGL, no UI, no production code.
 *
 * Everything from Sense UART is forwarded to USB Serial.
 * USB Serial commands are forwarded to Sense UART.
 * Sends PING to Sense every 2s and monitors responses.
 *
 * Wiring:
 *   LCD GPIO 38 (TX)  --> Sense GPIO 44 (D7/RX)
 *   LCD GPIO 48 (RX)  <-- Sense GPIO 43 (D6/TX)
 *   LCD GPIO 39 (INT) --> Sense GPIO 2  (D0)
 *   GND <--> GND
 */

#include <Arduino.h>
#include <HardwareSerial.h>

// --- Pin Definitions ---
#define INT_PIN       39
#define UART_TX_PIN   38   // LCD sends TO Sense
#define UART_RX_PIN   48   // LCD receives FROM Sense
#define UART_BAUD     115200

HardwareSerial senseSerial(1);

// --- State ---
uint32_t ping_seq = 0;
uint32_t pong_count = 0;
uint32_t hb_count = 0;
uint32_t rx_bytes_total = 0;
uint32_t rx_lines_total = 0;
unsigned long last_ping_ms = 0;
unsigned long last_sense_rx_ms = 0;
unsigned long last_status_ms = 0;
unsigned long last_int_pulse_ms = 0;
bool sense_alive = false;

// --- Line buffer for Sense UART RX ---
char rx_buf[512];
int rx_pos = 0;

// --- USB Serial line buffer ---
char usb_buf[256];
int usb_pos = 0;

void process_sense_line(const char* line) {
  rx_lines_total++;
  last_sense_rx_ms = millis();
  sense_alive = true;

  // Forward everything to USB Serial with timestamp
  Serial.printf("[SENSE] %s\n", line);

  // Track specific message types
  if (strstr(line, "\"t\":\"HB\"")) {
    hb_count++;
  } else if (strstr(line, "PONG")) {
    pong_count++;
  }
}

void pulse_int_pin() {
  digitalWrite(INT_PIN, HIGH);
  delayMicroseconds(500);
  digitalWrite(INT_PIN, LOW);
  last_int_pulse_ms = millis();
  Serial.println("[INT] Pulse sent to Sense");
}

void process_usb_command(const char* cmd) {
  Serial.printf("[USB_CMD] '%s'\n", cmd);

  if (strcmp(cmd, "ping") == 0) {
    senseSerial.println("PING");
    Serial.println("[TX->SENSE] PING");
  } else if (strcmp(cmd, "scan") == 0) {
    senseSerial.println("SCAN");
    Serial.println("[TX->SENSE] SCAN");
  } else if (strcmp(cmd, "coldstart") == 0) {
    senseSerial.println("COLDSTART");
    Serial.println("[TX->SENSE] COLDSTART (WiFi cold-start test)");
  } else if (strcmp(cmd, "int") == 0) {
    pulse_int_pin();
  } else if (strcmp(cmd, "status") == 0) {
    unsigned long now = millis();
    unsigned long sense_age = last_sense_rx_ms > 0 ? (now - last_sense_rx_ms) : 0xFFFFFFFF;
    Serial.println("=== DIAGNOSTIC STATUS ===");
    Serial.printf("  Uptime:        %lu ms\n", now);
    Serial.printf("  Sense alive:   %s\n", sense_alive ? "YES" : "NO");
    Serial.printf("  Last Sense RX: %lu ms ago\n", sense_age);
    Serial.printf("  RX bytes:      %u\n", rx_bytes_total);
    Serial.printf("  RX lines:      %u\n", rx_lines_total);
    Serial.printf("  HBs received:  %u\n", hb_count);
    Serial.printf("  PINGs sent:    %u\n", ping_seq);
    Serial.printf("  PONGs received:%u\n", pong_count);
    Serial.printf("  INT pulses:    counted by Sense\n");
    Serial.printf("  Free heap:     %u\n", ESP.getFreeHeap());
    Serial.printf("  UART RX pin:   GPIO %d level=%d\n", UART_RX_PIN, digitalRead(UART_RX_PIN));
    Serial.println("========================");
  } else if (strcmp(cmd, "help") == 0) {
    Serial.println("=== DIAG LCD COMMANDS ===");
    Serial.println("  ping   - Send PING to Sense");
    Serial.println("  scan      - Trigger WiFi scan on Sense");
    Serial.println("  coldstart - WiFi cold-start test (radio off -> reconnect)");
    Serial.println("  int       - Pulse INT pin to Sense");
    Serial.println("  status - Print diagnostic status");
    Serial.println("  help   - This help");
    Serial.println("  (anything else is forwarded raw to Sense UART)");
    Serial.println("=========================");
  } else {
    // Forward unknown commands raw to Sense
    senseSerial.println(cmd);
    Serial.printf("[TX->SENSE] %s\n", cmd);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== HALO Diag LCD v1.0 ===");

  // INT pin — output, default LOW
  pinMode(INT_PIN, OUTPUT);
  digitalWrite(INT_PIN, LOW);

  // UART to Sense
  senseSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  delay(50);
  while (senseSerial.available()) senseSerial.read();

  Serial.printf("  UART TX: GPIO %d -> Sense RX\n", UART_TX_PIN);
  Serial.printf("  UART RX: GPIO %d <- Sense TX\n", UART_RX_PIN);
  Serial.printf("  INT out: GPIO %d -> Sense D0\n", INT_PIN);
  Serial.println();
  Serial.println("All Sense UART output is forwarded here.");
  Serial.println("Type 'help' for commands.");
  Serial.println();

  // Initial ping
  senseSerial.println("PING");
  Serial.println("[TX->SENSE] PING (initial)");
}

void loop() {
  unsigned long now = millis();

  // --- Read from Sense UART ---
  while (senseSerial.available()) {
    char c = senseSerial.read();
    rx_bytes_total++;

    if (c == '\n' || c == '\r') {
      if (rx_pos > 0) {
        rx_buf[rx_pos] = '\0';
        process_sense_line(rx_buf);
        rx_pos = 0;
      }
    } else if (rx_pos < (int)(sizeof(rx_buf) - 1)) {
      rx_buf[rx_pos++] = c;
    } else {
      Serial.println("[WARN] Sense RX buffer overflow, flushing");
      rx_pos = 0;
    }
  }

  // --- Send PING every 2s ---
  if ((now - last_ping_ms) >= 2000) {
    last_ping_ms = now;
    ping_seq++;
    senseSerial.println("PING");
    // Don't log every ping to reduce noise — only log if Sense is silent
    if (last_sense_rx_ms > 0 && (now - last_sense_rx_ms) > 5000) {
      Serial.printf("[WARN] Sense silent for %lu ms (pings=%u, pongs=%u)\n",
                    now - last_sense_rx_ms, ping_seq, pong_count);
      sense_alive = false;
    }
  }

  // --- Periodic status line every 10s ---
  if ((now - last_status_ms) >= 10000) {
    last_status_ms = now;
    unsigned long sense_age = last_sense_rx_ms > 0 ? (now - last_sense_rx_ms) : 0xFFFFFFFF;
    Serial.printf("[STATUS] up=%lus sense=%s rx_bytes=%u lines=%u hb=%u ping=%u pong=%u rx_age=%lums\n",
                  now / 1000,
                  sense_alive ? "ALIVE" : "DEAD",
                  rx_bytes_total, rx_lines_total,
                  hb_count, ping_seq, pong_count,
                  sense_age < 0xFFFFFFFF ? sense_age : 0);
  }

  // --- Read USB Serial commands ---
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (usb_pos > 0) {
        usb_buf[usb_pos] = '\0';
        process_usb_command(usb_buf);
        usb_pos = 0;
      }
    } else if (usb_pos < (int)(sizeof(usb_buf) - 1)) {
      usb_buf[usb_pos++] = c;
    }
  }

  delay(5);
}

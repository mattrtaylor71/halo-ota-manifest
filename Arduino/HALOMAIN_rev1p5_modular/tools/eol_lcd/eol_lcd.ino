/*
 * HALO EOL (End-Of-Line) Test Bridge — LCD Board (ESP32-S3)
 *
 * Bare-bones hardware test firmware for factory / EOL bring-up.
 * NO LVGL, NO UI, NO sleep, NO production code.
 *
 * Purpose:
 *   - Drive the UART1 link to the Sense board on demand (PING/PINGS).
 *   - Pulse the INT line to the Sense board (INT).
 *   - Passively count heartbeats/pongs/lines coming back from Sense (STATS).
 *
 * A Python orchestrator drives this over USB Serial and parses the
 * single-line responses below. The response formats are EXACT — do not change.
 *
 * Wiring (verbatim):
 *   UART1 link to Sense:
 *     LCD GPIO 38 (TX)  --> Sense (LCD -> Sense)
 *     LCD GPIO 48 (RX)  <-- Sense (LCD <- Sense)
 *     115200 baud, SERIAL_8N1
 *   INT line to Sense:
 *     LCD GPIO 39 (INT) --> Sense, OUTPUT, default LOW
 *     A pulse = HIGH; delayMicroseconds(500); LOW; delay(2)
 *   GND <--> GND
 *
 * USB Serial commands (newline-terminated, case-sensitive):
 *   PING        - send one PING over UART to Sense; -> "PING sent"
 *   PINGS [k]   - send k PINGs (default 5), 50ms apart; -> "PINGS_SENT:<k>"
 *   INT [n]     - pulse INT pin n times (default 10);  -> "INT_PULSED:<n>"
 *   STATS       - print stats JSON line (see below)
 *   RESETSTATS  - zero hb/pong/rx_lines counters;      -> "RESETSTATS ok"
 *   HELP        - print the command list
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#include <stdlib.h>
#include <string.h>

// --- Pin / UART Definitions (verbatim, do not change) ---
#define INT_PIN       39   // LCD INT out -> Sense
#define UART_TX_PIN   38   // LCD sends TO Sense
#define UART_RX_PIN   48   // LCD receives FROM Sense
#define UART_BAUD     115200

HardwareSerial senseSerial(1);

// --- Counters (parsed by orchestrator via STATS) ---
uint32_t hb_count   = 0;
uint32_t pong_count = 0;
uint32_t rx_lines   = 0;
unsigned long last_sense_rx_ms = 0;  // 0 = no Sense line ever received

// --- Line buffer for Sense UART RX ---
char rx_buf[512];
int  rx_pos = 0;

// --- USB Serial line buffer ---
char usb_buf[256];
int  usb_pos = 0;

// Send a single PING over the UART to Sense.
void send_ping() {
  senseSerial.println("PING");
}

// Pulse the INT pin once: HIGH 500us, LOW, then 2ms settle.
void pulse_int_once() {
  digitalWrite(INT_PIN, HIGH);
  delayMicroseconds(500);
  digitalWrite(INT_PIN, LOW);
  delay(2);
}

// Process one complete line received from the Sense UART.
void process_sense_line(const char* line) {
  rx_lines++;
  last_sense_rx_ms = millis();

  if (strstr(line, "\"t\":\"HB\"")) {
    hb_count++;
  }
  if (strstr(line, "PONG")) {
    pong_count++;
  }

  // Optional debug echo to USB host (does not interfere with parsed lines).
  Serial.printf("[SENSE] %s\n", line);
}

// Print the STATS line in the EXACT format the orchestrator parses.
void print_stats() {
  long age = (last_sense_rx_ms > 0)
               ? (long)(millis() - last_sense_rx_ms)
               : -1;
  Serial.printf("STATS {\"hb\":%u,\"pong\":%u,\"rx_lines\":%u,\"sense_rx_age_ms\":%ld}\n",
                hb_count, pong_count, rx_lines, age);
}

void print_help() {
  Serial.println("=== HALO EOL LCD COMMANDS ===");
  Serial.println("  PING        - send one PING to Sense over UART");
  Serial.println("  PINGS [k]   - send k PINGs (default 5), 50ms apart");
  Serial.println("  INT [n]     - pulse INT pin n times (default 10)");
  Serial.println("  STATS       - print stats JSON line");
  Serial.println("  RESETSTATS  - zero hb/pong/rx_lines counters");
  Serial.println("  HELP        - this help");
  Serial.println("=============================");
}

// Parse and execute one USB host command (case-sensitive).
void process_usb_command(const char* cmd) {
  if (strcmp(cmd, "PING") == 0) {
    send_ping();
    Serial.println("PING sent");

  } else if (strncmp(cmd, "PINGS", 5) == 0) {
    // Optional argument k after a space; default 5.
    int k = 5;
    const char* arg = cmd + 5;
    while (*arg == ' ') arg++;
    if (*arg != '\0') {
      int v = atoi(arg);
      if (v > 0) k = v;
    }
    for (int i = 0; i < k; i++) {
      send_ping();
      if (i < k - 1) delay(50);
    }
    Serial.printf("PINGS_SENT:%d\n", k);

  } else if (strncmp(cmd, "INT", 3) == 0) {
    // Optional argument n after a space; default 10.
    int n = 10;
    const char* arg = cmd + 3;
    while (*arg == ' ') arg++;
    if (*arg != '\0') {
      int v = atoi(arg);
      if (v > 0) n = v;
    }
    for (int i = 0; i < n; i++) {
      pulse_int_once();
    }
    Serial.printf("INT_PULSED:%d\n", n);

  } else if (strcmp(cmd, "STATS") == 0) {
    print_stats();

  } else if (strcmp(cmd, "RESETSTATS") == 0) {
    hb_count   = 0;
    pong_count = 0;
    rx_lines   = 0;
    Serial.println("RESETSTATS ok");

  } else if (strcmp(cmd, "HELP") == 0) {
    print_help();

  } else {
    // Unknown command — report but do nothing to Sense.
    Serial.printf("ERR unknown command: %s\n", cmd);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== HALO EOL LCD v1 ===");

  // INT pin — output, default LOW.
  pinMode(INT_PIN, OUTPUT);
  digitalWrite(INT_PIN, LOW);

  // UART1 to Sense.
  senseSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  delay(50);
  while (senseSerial.available()) senseSerial.read();

  Serial.printf("  UART TX: GPIO %d -> Sense\n", UART_TX_PIN);
  Serial.printf("  UART RX: GPIO %d <- Sense\n", UART_RX_PIN);
  Serial.printf("  INT out: GPIO %d -> Sense\n", INT_PIN);
  Serial.println("Type HELP for commands.");

  // Do NOT auto-ping Sense — pinging is host-driven only for deterministic counts.
}

void loop() {
  // --- Read newline-delimited lines from Sense UART ---
  while (senseSerial.available()) {
    char c = senseSerial.read();
    if (c == '\n' || c == '\r') {
      if (rx_pos > 0) {
        rx_buf[rx_pos] = '\0';
        process_sense_line(rx_buf);
        rx_pos = 0;
      }
    } else if (rx_pos < (int)(sizeof(rx_buf) - 1)) {
      rx_buf[rx_pos++] = c;
    } else {
      // Overflow — flush and resync.
      rx_pos = 0;
    }
  }

  // --- Read USB Serial host commands ---
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

  delay(2);
}

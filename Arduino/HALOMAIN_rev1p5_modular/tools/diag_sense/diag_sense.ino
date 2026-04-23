/*
 * HALO Diagnostic Firmware — Sense Board
 *
 * Bare-bones: WiFi RSSI + UART heartbeat + INT pin monitor.
 * No sleep, no camera, no MQTT, no OTA.
 *
 * Sends JSON lines over UART to LCD every second:
 *   {"t":"HB","n":123,"rssi":-55,"wifi":3,"up":45000,"int_ct":7}
 *
 * Also responds to commands from LCD UART:
 *   PING  -> replies PONG
 *   SCAN  -> does WiFi.scanNetworks, sends results
 *
 * Wiring:
 *   Sense GPIO 43 (D6/TX) --> LCD GPIO 48 (RX)
 *   Sense GPIO 44 (D7/RX) <-- LCD GPIO 38 (TX)
 *   Sense GPIO 2  (D0/INT) <-- LCD GPIO 39 (INT)
 *   GND <--> GND
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HardwareSerial.h>

// --- Pin Definitions ---
#define INT_PIN       2    // D0 — wake interrupt from LCD
#define UART_TX_PIN   43   // D6 — Sense sends TO LCD
#define UART_RX_PIN   44   // D7 — Sense receives FROM LCD
#define UART_BAUD     115200

// --- WiFi credentials (same as production) ---
const char* WIFI_SSID = "Garage Member";
const char* WIFI_PASS = "build00!";

HardwareSerial lcdSerial(1);

// --- State ---
volatile uint32_t int_count = 0;
uint32_t hb_seq = 0;
uint32_t rx_count = 0;
unsigned long last_hb_ms = 0;
unsigned long wifi_connect_start_ms = 0;

// --- INT ISR ---
void IRAM_ATTR onIntRising() {
  int_count++;
}

// --- Line buffer for UART RX ---
char rx_buf[256];
int rx_pos = 0;

void process_command(const char* cmd) {
  rx_count++;
  Serial.printf("[RX] cmd='%s' rx_count=%u\n", cmd, rx_count);

  if (strcmp(cmd, "PING") == 0) {
    lcdSerial.println("PONG");
    lcdSerial.flush();
    Serial.println("[TX] PONG");
  }
  else if (strcmp(cmd, "SCAN") == 0) {
    Serial.println("[SCAN] Starting WiFi scan...");
    lcdSerial.println("{\"t\":\"SCAN_START\"}");
    int n = WiFi.scanNetworks(false, true);
    Serial.printf("[SCAN] Found %d networks\n", n);
    for (int i = 0; i < n && i < 20; i++) {
      char buf[200];
      snprintf(buf, sizeof(buf),
               "{\"t\":\"AP\",\"ssid\":\"%s\",\"rssi\":%d,\"ch\":%d}",
               WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
      lcdSerial.println(buf);
      Serial.printf("  AP: %s rssi=%d ch=%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
    }
    char done[64];
    snprintf(done, sizeof(done), "{\"t\":\"SCAN_DONE\",\"count\":%d}", n);
    lcdSerial.println(done);
    WiFi.scanDelete();
  }
  else if (strcmp(cmd, "COLDSTART") == 0) {
    // Full WiFi cold-start test: radio off -> radio on -> connect
    // Simulates what happens after deep sleep wake
    Serial.println("[COLDSTART] === WiFi Cold Start Test ===");

    // Phase 1: Record pre-test state
    wl_status_t pre_st = WiFi.status();
    int8_t pre_rssi = (pre_st == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : 0;
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"t\":\"CS\",\"phase\":\"start\",\"pre_wifi\":%d,\"pre_rssi\":%d}",
             (int)pre_st, pre_rssi);
    lcdSerial.println(msg);

    // Phase 2: Kill WiFi completely
    Serial.println("[COLDSTART] Disconnecting WiFi + radio off...");
    WiFi.disconnect(true);  // disconnect and erase AP config
    WiFi.mode(WIFI_OFF);
    delay(1000);

    snprintf(msg, sizeof(msg),
             "{\"t\":\"CS\",\"phase\":\"radio_off\",\"status\":%d}",
             (int)WiFi.status());
    lcdSerial.println(msg);
    Serial.printf("[COLDSTART] Radio off. Status=%d\n", (int)WiFi.status());

    // Phase 3: Re-enable radio and connect (exactly like after deep sleep)
    Serial.printf("[COLDSTART] Re-enabling radio, connecting to '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    unsigned long connect_start = millis();
    unsigned long last_report = 0;
    bool connected = false;
    int attempt_reports = 0;

    // Phase 4: Poll until connected or 60s timeout, report every 1s
    while ((millis() - connect_start) < 60000) {
      wl_status_t st = WiFi.status();
      unsigned long elapsed = millis() - connect_start;

      if (st == WL_CONNECTED) {
        connected = true;
        int8_t rssi = (int8_t)WiFi.RSSI();
        snprintf(msg, sizeof(msg),
                 "{\"t\":\"CS\",\"phase\":\"connected\",\"elapsed_ms\":%lu,\"rssi\":%d,\"ip\":\"%s\"}",
                 elapsed, rssi, WiFi.localIP().toString().c_str());
        lcdSerial.println(msg);
        Serial.printf("[COLDSTART] CONNECTED in %lu ms, RSSI=%d, IP=%s\n",
                      elapsed, rssi, WiFi.localIP().toString().c_str());
        break;
      }

      // Report progress every 1s
      if ((elapsed - last_report) >= 1000) {
        last_report = elapsed;
        attempt_reports++;
        snprintf(msg, sizeof(msg),
                 "{\"t\":\"CS\",\"phase\":\"waiting\",\"elapsed_ms\":%lu,\"status\":%d,\"attempt\":%d}",
                 elapsed, (int)st, attempt_reports);
        lcdSerial.println(msg);
        Serial.printf("[COLDSTART] Waiting... %lu ms, status=%d\n", elapsed, (int)st);
      }

      // If we get a hard failure, note it but keep trying
      if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
        snprintf(msg, sizeof(msg),
                 "{\"t\":\"CS\",\"phase\":\"retry\",\"elapsed_ms\":%lu,\"status\":%d}",
                 elapsed, (int)st);
        lcdSerial.println(msg);
        Serial.printf("[COLDSTART] Fail status=%d, retrying...\n", (int)st);
        WiFi.disconnect();
        delay(500);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      }

      delay(100);
    }

    if (!connected) {
      unsigned long elapsed = millis() - connect_start;
      snprintf(msg, sizeof(msg),
               "{\"t\":\"CS\",\"phase\":\"timeout\",\"elapsed_ms\":%lu,\"status\":%d}",
               elapsed, (int)WiFi.status());
      lcdSerial.println(msg);
      Serial.printf("[COLDSTART] TIMEOUT after %lu ms, status=%d\n", elapsed, (int)WiFi.status());
    }

    // Final summary
    wl_status_t final_st = WiFi.status();
    int8_t final_rssi = (final_st == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : 0;
    snprintf(msg, sizeof(msg),
             "{\"t\":\"CS\",\"phase\":\"done\",\"ok\":%d,\"wifi\":%d,\"rssi\":%d}",
             connected ? 1 : 0, (int)final_st, final_rssi);
    lcdSerial.println(msg);
    Serial.println("[COLDSTART] === Test Complete ===");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== HALO Diag Sense v1.0 ===");

  // INT pin
  pinMode(INT_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), onIntRising, RISING);

  // UART to LCD
  lcdSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  delay(50);
  while (lcdSerial.available()) lcdSerial.read();

  Serial.printf("  UART TX: GPIO %d (D6) -> LCD RX\n", UART_TX_PIN);
  Serial.printf("  UART RX: GPIO %d (D7) <- LCD TX\n", UART_RX_PIN);
  Serial.printf("  INT in:  GPIO %d (D0) <- LCD INT\n", INT_PIN);

  // WiFi
  Serial.printf("  WiFi: connecting to '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifi_connect_start_ms = millis();

  // Send initial ready
  lcdSerial.println("{\"t\":\"READY\"}");
  Serial.println("Setup complete. Sending heartbeats every 1s.");
}

void loop() {
  unsigned long now = millis();

  // --- WiFi connection management (non-blocking) ---
  if (WiFi.status() != WL_CONNECTED && (now - wifi_connect_start_ms) > 30000) {
    Serial.println("[WIFI] Reconnecting...");
    WiFi.disconnect();
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    wifi_connect_start_ms = now;
  }

  // --- Send heartbeat every 1s ---
  if ((now - last_hb_ms) >= 1000) {
    last_hb_ms = now;
    hb_seq++;

    wl_status_t st = WiFi.status();
    int8_t rssi = (st == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : 0;
    int int_pin_level = digitalRead(INT_PIN);

    char hb[200];
    snprintf(hb, sizeof(hb),
             "{\"t\":\"HB\",\"n\":%u,\"rssi\":%d,\"wifi\":%d,\"up\":%lu,\"int_ct\":%u,\"int_pin\":%d,\"rx_ct\":%u,\"heap\":%u}",
             hb_seq, rssi, (int)st, now, (uint32_t)int_count, int_pin_level, rx_count, ESP.getFreeHeap());
    lcdSerial.println(hb);
    lcdSerial.flush();

    // Also print locally
    Serial.printf("[HB] #%u rssi=%d wifi=%d int_ct=%u int_pin=%d rx=%u heap=%u\n",
                  hb_seq, rssi, (int)st, (uint32_t)int_count, int_pin_level, rx_count, ESP.getFreeHeap());
  }

  // --- Read UART commands from LCD ---
  while (lcdSerial.available()) {
    char c = lcdSerial.read();
    if (c == '\n' || c == '\r') {
      if (rx_pos > 0) {
        rx_buf[rx_pos] = '\0';
        process_command(rx_buf);
        rx_pos = 0;
      }
    } else if (rx_pos < (int)(sizeof(rx_buf) - 1)) {
      rx_buf[rx_pos++] = c;
    } else {
      rx_pos = 0;
    }
  }

  // --- Read USB serial commands (for local testing) ---
  static char usb_buf[128];
  static int usb_pos = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (usb_pos > 0) {
        usb_buf[usb_pos] = '\0';
        if (strcmp(usb_buf, "scan") == 0) {
          process_command("SCAN");
        } else if (strcmp(usb_buf, "ping") == 0) {
          lcdSerial.println("PONG");
          Serial.println("[TX] PONG (local)");
        } else {
          // Forward raw text to LCD
          lcdSerial.println(usb_buf);
          Serial.printf("[TX] forwarded: %s\n", usb_buf);
        }
        usb_pos = 0;
      }
    } else if (usb_pos < (int)(sizeof(usb_buf) - 1)) {
      usb_buf[usb_pos++] = c;
    }
  }

  delay(10);
}

/*
 * eol_sense.ino — END-OF-LINE (EOL) hardware test firmware for the HALO Sense
 *                 board (XIAO ESP32-S3).
 *
 * ISOLATED TEST SKETCH — touches NO production source. It merges two existing
 * isolated harnesses:
 *   (a) heat-disciplined cold camera capture .... from tools/camera_tune
 *   (b) UART heartbeat + INT diagnostics ........ from tools/diag_sense
 *
 * Purpose: a single firmware a factory/EOL station flashes onto the Sense board
 * to validate the camera (incl. functional PWDN power cut), the UART1 link to
 * the LCD, and the INT line — all driven by a host-side Python orchestrator over
 * USB serial.
 *
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  HEAT DISCIPLINE (HARD RULE — the camera gets HOT if left initialized) ║
 * ║  The camera is DE-INITIALIZED and POWERED OFF at ALL times except      ║
 * ║  during a single SHOT / PWDNTEST step. The camera must NEVER be left   ║
 * ║  powered after any command or error path. A safety timer in loop()     ║
 * ║  force-deinits if a window ever overruns CAM_ON_SAFETY_MS. Boot leaves ║
 * ║  the camera OFF.                                                        ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 * USB SERIAL COMMANDS (host-facing, newline-terminated, case-sensitive):
 *   PING        -> "PONG"
 *   STATS       -> one compact-JSON line of counters/heap (see format below)
 *   RESETSTATS  -> zero rx/int/max_cam_on; "RESETSTATS ok"
 *   SHOT [...]  -> heat-disciplined cold capture; META + JPEG stream
 *   PWDNTEST    -> functional power-down test; one PWDN_RESULT line
 *   OFF         -> force full deinit (camera OFF); "OFF ok"
 *   HELP        -> command list
 *
 * UART1 (to/from LCD): emits an HB JSON line every 1000ms, echoes PING->PONG,
 * counts received lines (rx_count) and INT rising edges (int_count).
 *
 * Wiring:
 *   Sense GPIO 43 (TX) --> LCD RX        UART1
 *   Sense GPIO 44 (RX) <-- LCD TX        UART1
 *   Sense GPIO 2  (INT) <-- LCD INT      RISING -> int_count++
 *   GND <--> GND
 */

#include <Arduino.h>
#include <HardwareSerial.h>
#define CAMERA_MODEL_XIAO_ESP32S3
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"   // gpio_hold_dis / gpio_deep_sleep_hold_dis

// ── XIAO ESP32-S3 Sense camera pins (identical to production) ──
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y2_GPIO_NUM     15
#define Y3_GPIO_NUM     17
#define Y4_GPIO_NUM     18
#define Y5_GPIO_NUM     16
#define Y6_GPIO_NUM     14
#define Y7_GPIO_NUM     12
#define Y8_GPIO_NUM     11
#define Y9_GPIO_NUM     48
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13
#define CAM_PWDN_GPIO   1     // XIAO power-control FET: HIGH=camera OFF, LOW=camera ON
#define FILL_LED_PIN    4     // D3 white fill LED: HIGH=on

// ── UART1 link to LCD ──
#define INT_PIN       2    // D0 — INT line from LCD (RISING -> int_count++)
#define UART_TX_PIN   43   // D6 — Sense sends TO LCD
#define UART_RX_PIN   44   // D7 — Sense receives FROM LCD
#define UART_BAUD     115200

static const unsigned long CAM_ON_SAFETY_MS = 4000; // force-deinit if a window overruns

HardwareSerial lcdSerial(1);

// ── State / counters ──
static volatile bool   g_cam_on = false;
static unsigned long   g_cam_on_since = 0;
static unsigned long   g_max_cam_on_ms = 0;     // max single on-window observed

static volatile uint32_t int_count = 0;
static uint32_t rx_count = 0;
static uint32_t hb_seq = 0;
static unsigned long last_hb_ms = 0;
static unsigned long last_uart_rx_ms = 0;        // 0 == never

// ── per-shot params (defaults match production "label"/normal profile) ──
struct ShotParams {
  int           xclk     = 20000000;          // production nominal
  int           quality  = 5;                 // 0=best
  framesize_t   size     = FRAMESIZE_SXGA;    // 9 = 1280x1024 (production)
  int           ae       = 2;
  gainceiling_t gc       = GAINCEILING_64X;   // 5
  int           wb       = 3;                 // office
  int           sharp    = 3;
  int           bright   = 2;
  int           contrast = 2;
  int           sat      = 1;
  int           warmup   = 3;                 // production AEC-settle frames
  int           grab     = 1;                 // CAMERA_GRAB_LATEST
  int           fill     = 1;                 // white fill LED ON during capture
};

// ── INT ISR ──
void IRAM_ATTR onIntRising() {
  int_count++;
}

// ───────────────────────── power + deinit (heat-critical) ─────────────────────────
static void cam_power_on() {
  // Release any RTC GPIO hold left on PWDN by a prior firmware's pre-sleep —
  // otherwise the pin is pinned HIGH (camera off) and init fails 0x106.
  gpio_hold_dis((gpio_num_t)CAM_PWDN_GPIO);
  gpio_deep_sleep_hold_dis();
  pinMode(CAM_PWDN_GPIO, OUTPUT);
  digitalWrite(CAM_PWDN_GPIO, HIGH);  // ensure known state
  delay(5);
  digitalWrite(CAM_PWDN_GPIO, LOW);   // enable
  delay(50);                          // power/clock settle
}

static void cam_power_off() {
  pinMode(CAM_PWDN_GPIO, OUTPUT);
  digitalWrite(CAM_PWDN_GPIO, HIGH);  // disable
  delay(2);
}

// FULL deinit: stop the driver, stop XCLK, float all camera pins, cut power.
static void full_deinit(const char* reason) {
  if (g_cam_on) {
    // record the window that just closed
    unsigned long on_ms = millis() - g_cam_on_since;
    if (on_ms > g_max_cam_on_ms) g_max_cam_on_ms = on_ms;
    esp_log_level_set("gdma", ESP_LOG_NONE);
    esp_camera_deinit();
    esp_log_level_set("gdma", ESP_LOG_ERROR);
    g_cam_on = false;
  }
  // float XCLK so no clock is driven into the sensor (extra heat insurance)
  pinMode(XCLK_GPIO_NUM, INPUT);
  int pins[] = { Y2_GPIO_NUM,Y3_GPIO_NUM,Y4_GPIO_NUM,Y5_GPIO_NUM,Y6_GPIO_NUM,
                 Y7_GPIO_NUM,Y8_GPIO_NUM,Y9_GPIO_NUM,PCLK_GPIO_NUM,VSYNC_GPIO_NUM,
                 HREF_GPIO_NUM };
  for (size_t i = 0; i < sizeof(pins)/sizeof(pins[0]); i++) pinMode(pins[i], INPUT);
  cam_power_off();
  pinMode(FILL_LED_PIN, OUTPUT); digitalWrite(FILL_LED_PIN, LOW);  // fill LED OFF
  Serial.printf("[DEINIT] camera OFF (%s)\n", reason);
}

// Build the standard JPEG/SXGA camera_config_t used by SHOT and PWDNTEST.
static void build_config(camera_config_t& config, const ShotParams& p) {
  config = camera_config_t{};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0=Y2_GPIO_NUM; config.pin_d1=Y3_GPIO_NUM; config.pin_d2=Y4_GPIO_NUM;
  config.pin_d3=Y5_GPIO_NUM; config.pin_d4=Y6_GPIO_NUM; config.pin_d5=Y7_GPIO_NUM;
  config.pin_d6=Y8_GPIO_NUM; config.pin_d7=Y9_GPIO_NUM;
  config.pin_xclk=XCLK_GPIO_NUM; config.pin_pclk=PCLK_GPIO_NUM;
  config.pin_vsync=VSYNC_GPIO_NUM; config.pin_href=HREF_GPIO_NUM;
  config.pin_sccb_sda=SIOD_GPIO_NUM; config.pin_sccb_scl=SIOC_GPIO_NUM;
  config.pin_pwdn=PWDN_GPIO_NUM; config.pin_reset=RESET_GPIO_NUM;
  config.xclk_freq_hz = p.xclk;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = p.size;
  config.jpeg_quality = p.quality;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = p.grab ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
}

// Guarded deinit that can be called even when the driver may be half-initialized
// (e.g. an esp_camera_init that returned an error). Always suppresses gdma logs.
// Updates g_max_cam_on_ms when a real on-window was open (mirrors full_deinit).
static void guarded_deinit() {
  if (g_cam_on) {
    unsigned long on_ms = millis() - g_cam_on_since;
    if (on_ms > g_max_cam_on_ms) g_max_cam_on_ms = on_ms;
  }
  esp_log_level_set("gdma", ESP_LOG_NONE);
  esp_camera_deinit();
  esp_log_level_set("gdma", ESP_LOG_ERROR);
  g_cam_on = false;
}

// ───────────────────────── one cold shot ─────────────────────────
static void do_shot(const ShotParams& p) {
  unsigned long t_on = millis();
  cam_power_on();
  // white fill LED ON for the whole capture window (production parity)
  pinMode(FILL_LED_PIN, OUTPUT);
  digitalWrite(FILL_LED_PIN, p.fill ? HIGH : LOW);

  camera_config_t config;
  build_config(config, p);

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("SHOT_ERR init=0x%x xclk=%d size=%d\n", err, p.xclk, (int)p.size);
    full_deinit("init_fail");
    return;
  }
  g_cam_on = true; g_cam_on_since = millis();

  sensor_t* s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : 0;
  if (s) {
    s->set_quality(s, p.quality);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_gain_ctrl(s, 1);
    s->set_ae_level(s, p.ae);
    s->set_gainceiling(s, p.gc);
    s->set_brightness(s, p.bright);
    s->set_contrast(s, p.contrast);
    s->set_saturation(s, p.sat);
    if (s->set_whitebal) s->set_whitebal(s, 1);
    if (s->set_awb_gain) s->set_awb_gain(s, 1);
    if (s->set_wb_mode)  s->set_wb_mode(s, p.wb);
    if (s->set_denoise)  s->set_denoise(s, 0);
    if (s->set_sharpness)s->set_sharpness(s, p.sharp);
    if (s->set_raw_gma)  s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
  }

  // warmup / AEC-AWB settle (production discards 3 @ 30ms)
  unsigned long t_warm = millis();
  for (int i = 0; i < p.warmup; i++) {
    camera_fb_t* w = esp_camera_fb_get();
    if (w) esp_camera_fb_return(w);
    delay(30);
  }
  unsigned long warm_ms = millis() - t_warm;

  // final capture
  unsigned long t_cap = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  unsigned long cap_ms = millis() - t_cap;
  if (!fb) { Serial.println("SHOT_ERR capture=null"); full_deinit("cap_fail"); return; }

  // COPY jpeg out so we can deinit BEFORE the slow stream
  size_t jlen = fb->len;
  int w = fb->width, h = fb->height;
  bool soi = (jlen >= 2 && fb->buf[0]==0xFF && fb->buf[1]==0xD8);
  bool eoi = (jlen >= 2 && fb->buf[jlen-2]==0xFF && fb->buf[jlen-1]==0xD9);
  uint8_t* copy = (uint8_t*)heap_caps_malloc(jlen, MALLOC_CAP_SPIRAM);
  if (copy) memcpy(copy, fb->buf, jlen);
  esp_camera_fb_return(fb);

  // ── DEINIT NOW (camera off for the rest of this command) ──
  unsigned long cam_on_ms = millis() - t_on;
  full_deinit("post_capture");

  Serial.printf("META pid=0x%04X xclk=%d q=%d size=%d ae=%d gc=%d wb=%d sharp=%d "
                "bright=%d contrast=%d sat=%d warmup=%d w=%d h=%d len=%u soi=%d eoi=%d "
                "warm_ms=%lu cap_ms=%lu cam_on_ms=%lu\n",
                pid, p.xclk, p.quality, (int)p.size, p.ae, (int)p.gc, p.wb, p.sharp,
                p.bright, p.contrast, p.sat, p.warmup, w, h, (unsigned)jlen,
                soi?1:0, eoi?1:0, warm_ms, cap_ms, cam_on_ms);

  if (!copy) { Serial.println("SHOT_ERR copy_oom"); return; }
  Serial.printf("JPEG_START:%u\n", (unsigned)jlen);
  Serial.flush(); delay(5);
  size_t off = 0;
  while (off < jlen) {
    size_t c = jlen - off; if (c > 4096) c = 4096;
    Serial.write(copy + off, c); off += c; Serial.flush();
  }
  Serial.printf("\nJPEG_END:%u\n", (unsigned)jlen);
  heap_caps_free(copy);
}

// ───────────────────────── functional power-down test ─────────────────────────
// Validates that the GPIO1 power FET actually cuts power to the sensor:
//   1) on_ok     : power on -> init -> grab a frame      (camera works)
//   2) off_dead  : power OFF -> init MUST fail           (FET cut power)
//   3) revive_ok : power on -> init -> grab a frame      (recovers)
// pass = on_ok && off_dead && revive_ok. Camera always ends OFF.
static void do_pwdntest() {
  g_max_cam_on_ms = 0;
  ShotParams p;                 // JPEG/SXGA, default knobs
  p.warmup = 3;                 // keep each on-window short
  camera_config_t config;
  build_config(config, p);

  int on_ok = 0, off_dead = 0, revive_ok = 0;

  // ── Step 1: on_ok ──
  Serial.println("[PWDN] step1 on_ok: power on + init + grab");
  cam_power_on();
  esp_err_t err1 = esp_camera_init(&config);
  if (err1 == ESP_OK) {
    g_cam_on = true; g_cam_on_since = millis();
    camera_fb_t* fb1 = nullptr;
    for (int i = 0; i < p.warmup; i++) {           // warmup then capture
      camera_fb_t* w = esp_camera_fb_get();
      if (w) esp_camera_fb_return(w);
      delay(30);
    }
    fb1 = esp_camera_fb_get();
    on_ok = (fb1 != nullptr) ? 1 : 0;
    if (fb1) esp_camera_fb_return(fb1);
  } else {
    Serial.printf("[PWDN] step1 init=0x%x\n", err1);
  }
  // close window: deinit exactly once (updates max_cam_on), power off
  guarded_deinit();
  cam_power_off();

  // ── Step 2: off_dead (do NOT power on; init MUST fail) ──
  Serial.println("[PWDN] step2 off_dead: leave power OFF, init must FAIL");
  cam_power_off();          // ensure PWDN HIGH (power cut)
  delay(200);              // settle
  esp_err_t err2 = esp_camera_init(&config);
  off_dead = (err2 != ESP_OK) ? 1 : 0;   // FAIL to init == power truly cut == PASS
  if (err2 == ESP_OK) {
    // The PWDN line is NOT cutting power — record an on-window, this is a FAIL.
    Serial.println("[PWDN] step2 WARNING: init SUCCEEDED with power off (FET not cutting)");
    g_cam_on = true; g_cam_on_since = millis();
  }
  // Always deinit exactly once (guarded), even on failure.
  guarded_deinit();
  cam_power_off();

  // ── Step 3: revive_ok ──
  Serial.println("[PWDN] step3 revive_ok: power on + init + grab");
  cam_power_on();
  esp_err_t err3 = esp_camera_init(&config);
  if (err3 == ESP_OK) {
    g_cam_on = true; g_cam_on_since = millis();
    for (int i = 0; i < p.warmup; i++) {
      camera_fb_t* w = esp_camera_fb_get();
      if (w) esp_camera_fb_return(w);
      delay(30);
    }
    camera_fb_t* fb3 = esp_camera_fb_get();
    revive_ok = (fb3 != nullptr) ? 1 : 0;
    if (fb3) esp_camera_fb_return(fb3);
  } else {
    Serial.printf("[PWDN] step3 init=0x%x\n", err3);
  }
  // final teardown — camera OFF guaranteed
  full_deinit("pwdn_revive");

  int pass = (on_ok && off_dead && revive_ok) ? 1 : 0;
  Serial.printf("PWDN_RESULT {\"on_ok\":%d,\"off_dead\":%d,\"revive_ok\":%d,"
                "\"max_cam_on_ms\":%lu,\"pass\":%d}\n",
                on_ok, off_dead, revive_ok, g_max_cam_on_ms, pass);

  // belt-and-suspenders: ensure OFF on every exit path
  full_deinit("pwdn_end");
}

// ───────────────────────── command parsing ─────────────────────────
static int kv_int(const String& s, const char* key, int def) {
  String k = String(key) + "=";
  int i = s.indexOf(k);
  if (i < 0) return def;
  return s.substring(i + k.length()).toInt();
}

static void handle_shot(const String& line) {
  ShotParams p;
  p.xclk     = kv_int(line, "XCLK", p.xclk);
  p.quality  = kv_int(line, "QUALITY", p.quality);
  p.size     = (framesize_t)kv_int(line, "SIZE", (int)p.size);
  p.ae       = kv_int(line, "AE", p.ae);
  p.gc       = (gainceiling_t)kv_int(line, "GC", (int)p.gc);
  p.wb       = kv_int(line, "WB", p.wb);
  p.sharp    = kv_int(line, "SHARP", p.sharp);
  p.bright   = kv_int(line, "BRIGHT", p.bright);
  p.contrast = kv_int(line, "CONTRAST", p.contrast);
  p.sat      = kv_int(line, "SAT", p.sat);
  p.warmup   = kv_int(line, "WARMUP", p.warmup);
  p.grab     = kv_int(line, "GRAB", p.grab);
  p.fill     = kv_int(line, "FILL", p.fill);
  do_shot(p);
}

static void print_stats() {
  long age = (last_uart_rx_ms == 0) ? -1 : (long)(millis() - last_uart_rx_ms);
  Serial.printf("STATS {\"rx\":%u,\"int\":%u,\"hb\":%u,\"uart_rx_age_ms\":%ld,"
                "\"cam_on\":%d,\"max_cam_on_ms\":%lu,\"psram\":%u,\"heap\":%u}\n",
                rx_count, (uint32_t)int_count, hb_seq, age,
                g_cam_on ? 1 : 0, g_max_cam_on_ms,
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());
}

static void handle_usb_command(const String& cmd) {
  if (cmd == "PING") {
    Serial.println("PONG");
  } else if (cmd == "STATS") {
    print_stats();
  } else if (cmd == "RESETSTATS") {
    rx_count = 0;
    int_count = 0;
    g_max_cam_on_ms = 0;
    Serial.println("RESETSTATS ok");
  } else if (cmd.startsWith("SHOT")) {
    handle_shot(cmd);
  } else if (cmd == "PWDNTEST") {
    do_pwdntest();
  } else if (cmd == "OFF") {
    full_deinit("manual");
    Serial.println("OFF ok");
  } else if (cmd == "HELP") {
    Serial.println("CMDS: PING | STATS | RESETSTATS | "
                   "SHOT [XCLK= QUALITY= SIZE= AE= GC= WB= SHARP= BRIGHT= CONTRAST= SAT= WARMUP= GRAB= FILL=] | "
                   "PWDNTEST | OFF | HELP");
  } else {
    Serial.printf("UNKNOWN: %s\n", cmd.c_str());
  }
}

// ───────────────────────── UART1 line processing ─────────────────────────
static void process_uart_line(const char* line) {
  rx_count++;
  last_uart_rx_ms = millis();
  if (strcmp(line, "PING") == 0) {
    lcdSerial.println("PONG");
    lcdSerial.flush();
  }
}

// ───────────────────────── setup / loop ─────────────────────────
static String usb_cmd;

void setup() {
  Serial.begin(115200);
  delay(1500);

  // Release any RTC GPIO holds a prior firmware left armed (esp. PWDN GPIO1).
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)CAM_PWDN_GPIO);

  // Boot with camera OFF (heat discipline). Drive PWDN high (off), float pins.
  full_deinit("boot");

  // INT pin from LCD
  pinMode(INT_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(INT_PIN), onIntRising, RISING);

  // UART1 to LCD
  lcdSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  delay(50);
  while (lcdSerial.available()) lcdSerial.read();

  Serial.println();
  Serial.println("=== HALO EOL Sense v1 === camera OFF");
  Serial.printf("PSRAM_free=%u heap_free=%u\n",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());
  Serial.printf("UART1 TX=GPIO%d RX=GPIO%d  INT=GPIO%d\n",
                UART_TX_PIN, UART_RX_PIN, INT_PIN);
  Serial.println("READY. Cmds: PING | STATS | RESETSTATS | SHOT [...] | PWDNTEST | OFF | HELP");
}

void loop() {
  unsigned long now = millis();

  // safety: never let the camera stay on past the budget
  if (g_cam_on && (now - g_cam_on_since) > CAM_ON_SAFETY_MS) {
    full_deinit("safety_timeout");
  }

  // heartbeat to LCD over UART1 every 1000ms (proves Sense TX wire)
  if ((now - last_hb_ms) >= 1000) {
    last_hb_ms = now;
    char hb[160];
    snprintf(hb, sizeof(hb),
             "{\"t\":\"HB\",\"n\":%u,\"int\":%u,\"rx\":%u}",
             hb_seq++, (uint32_t)int_count, rx_count);
    lcdSerial.println(hb);
    lcdSerial.flush();
  }

  // read complete newline-delimited lines from the LCD over UART1
  static char rx_buf[256];
  static int  rx_pos = 0;
  while (lcdSerial.available()) {
    char c = lcdSerial.read();
    if (c == '\n' || c == '\r') {
      if (rx_pos > 0) {
        rx_buf[rx_pos] = '\0';
        process_uart_line(rx_buf);
        rx_pos = 0;
      }
    } else if (rx_pos < (int)(sizeof(rx_buf) - 1)) {
      rx_buf[rx_pos++] = c;
    } else {
      rx_pos = 0;  // overflow guard
    }
  }

  // read host commands from USB serial (newline-terminated)
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      usb_cmd.trim();
      if (usb_cmd.length()) handle_usb_command(usb_cmd);
      usb_cmd = "";
    } else if (usb_cmd.length() < 200) {
      usb_cmd += c;
    }
  }

  delay(5);
}

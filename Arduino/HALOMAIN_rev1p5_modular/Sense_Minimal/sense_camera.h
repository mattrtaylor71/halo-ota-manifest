/*
 * sense_camera.h
 *
 * Camera hardware control, sensor profiles, preflight scene analysis,
 * capture pipeline, and camera metadata helpers for Sense_Minimal.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 8.
 *
 * Prerequisites (must be declared before #include "sense_camera.h"):
 *   - esp_camera.h, camera_pins.h, driver/gpio.h, esp_log.h
 *   - ArduinoJson.h
 *   - Camera config constants: CAM_PWDN_GPIO, CAPTURE_SIZE, JPEG_QUALITY,
 *     CAMERA_XCLK_HZ, FILL_LED_PIN, CAMERA_PWDN_WAKE_DELAY_MS,
 *     CAMERA_PWDN_DISABLE_DELAY_MS, g_cam_pwdn_hold_enabled,
 *     CAMERA_DIAG, CAMERA_PREFLIGHT_*, CAMERA_INIT_*, CAMERA_DMA_*,
 *     CAMERA_NETWORK_QUIESCE_DELAY_MS, CAMERA_CAPTURE_*, CAMERA_WARMUP_*,
 *     CAMERA_RETRY_*, CAMERA_PROFILE_SWITCH_*, CAMERA_SVGA_FALLBACK_*,
 *     CAMERA_ENABLE_REINIT_FALLBACK
 *   - CameraProfile enum, CameraPreflightMode enum, CAMERA_PREFLIGHT_MODE
 *   - g_camera_profile, g_camera_preflight_force,
 *     g_last_scene_luma, g_last_scene_green_ratio
 *   - g_camera_timeline_* globals from sense_diag.h
 *   - camera_timeline_event(), camera_timeline_build(), camera_diag_label(),
 *     diag_record_error() from sense_diag.h
 *   - uart_send_sense_diag() from sense_uart.h
 *   - UploadJob::CameraUploadMeta from sense_ops.h
 *   - waiting_for_mqtt_result, mqttClient, wifiClient
 *   - mqtt_clear_result_subscription() from sense_mqtt.h
 */

#ifndef SENSE_CAMERA_H
#define SENSE_CAMERA_H

// ── Camera profile name ─────────────────────────────────────────────

static const char* camera_profile_name(CameraProfile profile) {
  switch (profile) {
    case CAM_PROFILE_LOW_LIGHT:
      return "low_light";
    case CAM_PROFILE_FLASH:
      return "flash";
    case CAM_PROFILE_LABEL:
      return "label";
    case CAM_PROFILE_NORMAL:
    default:
      return "normal";
  }
}

// ── Camera timeline completion ──────────────────────────────────────

static void camera_timeline_complete(bool ok, const camera_fb_t* fb, const char* reason) {
  if (g_camera_timeline_done) {
    return;
  }
  g_camera_timeline_done = true;
  g_camera_timeline_ok = ok;
  g_camera_preflight_force = !ok;
  g_camera_timeline_total_ms = (g_camera_timeline_start_ms > 0)
                                ? (millis() - g_camera_timeline_start_ms)
                                : 0;
  g_camera_timeline_last_ms = millis();
  if (fb) {
    g_camera_timeline_len = fb->len;
    g_camera_timeline_w = fb->width;
    g_camera_timeline_h = fb->height;
  } else {
    g_camera_timeline_len = 0;
    g_camera_timeline_w = 0;
    g_camera_timeline_h = 0;
  }
  camera_timeline_event(ok ? "done_ok" : "done_err",
                        ok ? (int32_t)g_camera_timeline_total_ms : -1);
  if (reason && reason[0]) {
    camera_timeline_event(reason, 0);
  }
  g_camera_timeline_last_ms = millis();
  Serial.printf("[CAMERA_TIMING] %s\n", camera_timeline_build());
  char detail[64];
  snprintf(detail, sizeof(detail), "reason=%s ok=%d len=%u",
           reason ? reason : "",
           g_camera_timeline_ok ? 1 : 0,
           (unsigned)g_camera_timeline_len);
  uart_send_sense_diag("camera", "timing", camera_diag_label(),
                       ok ? (int32_t)g_camera_timeline_total_ms : -1,
                       detail);
  if (g_camera_timeline_total_ms > CAMERA_CAPTURE_TARGET_MS) {
    Serial.printf("[CAMERA_TIMING] target_miss mode=%s total_ms=%lu target_ms=%lu ok=%d reason=%s luma=%d green=%d profile=%s\n",
                  g_camera_timeline_mode[0] ? g_camera_timeline_mode : "-",
                  (unsigned long)g_camera_timeline_total_ms,
                  (unsigned long)CAMERA_CAPTURE_TARGET_MS,
                  g_camera_timeline_ok ? 1 : 0,
                  reason ? reason : "",
                  g_last_scene_luma,
                  g_last_scene_green_ratio,
                  camera_profile_name(g_camera_profile));
  }
}

// ── Camera metadata helpers ─────────────────────────────────────────

static void capture_camera_meta_snapshot(UploadJob::CameraUploadMeta* meta, const camera_fb_t* fb) {
  if (!meta) {
    return;
  }
  memset(meta, 0, sizeof(*meta));
  meta->profile = static_cast<uint8_t>(g_camera_profile);
  meta->flash_enabled = (FILL_LED_PIN >= 0) ? 1 : 0;
  meta->jpeg_quality = (JPEG_QUALITY < 0) ? 0 : ((JPEG_QUALITY > 255) ? 255 : (uint8_t)JPEG_QUALITY);
  meta->actual_width = fb ? (uint16_t)fb->width : 0;
  meta->actual_height = fb ? (uint16_t)fb->height : 0;
  meta->configured_framesize = (uint16_t)CAPTURE_SIZE;
  meta->scene_luma = (g_last_scene_luma < -32768) ? -32768 : (g_last_scene_luma > 32767 ? 32767 : (int16_t)g_last_scene_luma);
  meta->scene_green_ratio = (g_last_scene_green_ratio < -32768) ? -32768 : (g_last_scene_green_ratio > 32767 ? 32767 : (int16_t)g_last_scene_green_ratio);
  meta->xclk_hz = (CAMERA_XCLK_HZ < 0) ? 0u : (uint32_t)CAMERA_XCLK_HZ;
}

static void append_camera_meta_json(JsonDocument& doc, const UploadJob::CameraUploadMeta& meta) {
  JsonObject camera_meta = doc["camera_meta"].to<JsonObject>();
  camera_meta["profile"] = camera_profile_name((CameraProfile)meta.profile);
  camera_meta["profile_code"] = meta.profile;
  camera_meta["flash_enabled"] = meta.flash_enabled ? true : false;
  camera_meta["jpeg_quality"] = meta.jpeg_quality;
  camera_meta["capture_width"] = meta.actual_width;
  camera_meta["capture_height"] = meta.actual_height;
  camera_meta["configured_framesize"] = meta.configured_framesize;
  camera_meta["scene_luma"] = meta.scene_luma;
  camera_meta["scene_green_ratio"] = meta.scene_green_ratio;
  camera_meta["xclk_hz"] = meta.xclk_hz;
}

static void log_camera_meta_for_presign(const char* label, const UploadJob::CameraUploadMeta* meta) {
  if (!meta) {
    Serial.printf("[%s] camera_meta missing\n", label ? label : "PRESIGN");
    return;
  }
  Serial.printf("[%s] camera_meta profile=%s profile_code=%u flash=%u capture=%ux%u framesize=%u jpeg=%u luma=%d green=%d xclk=%lu\n",
                label ? label : "PRESIGN",
                camera_profile_name((CameraProfile)meta->profile),
                (unsigned)meta->profile,
                (unsigned)meta->flash_enabled,
                (unsigned)meta->actual_width,
                (unsigned)meta->actual_height,
                (unsigned)meta->configured_framesize,
                (unsigned)meta->jpeg_quality,
                (int)meta->scene_luma,
                (int)meta->scene_green_ratio,
                (unsigned long)meta->xclk_hz);
}

// ── Camera hardware power control ───────────────────────────────────

static void camera_pwdn_gpio_init() {
  pinMode((int)CAM_PWDN_GPIO, OUTPUT);
  digitalWrite((int)CAM_PWDN_GPIO, HIGH);
  Serial.printf("[CAM_PWR] init gpio=%d level=%d\n",
                (int)CAM_PWDN_GPIO,
                gpio_get_level(CAM_PWDN_GPIO));
}

static void camera_power_hold_disable() {
  pinMode((int)CAM_PWDN_GPIO, OUTPUT);
  digitalWrite((int)CAM_PWDN_GPIO, HIGH);
  esp_err_t rc = gpio_hold_dis(CAM_PWDN_GPIO);
  g_cam_pwdn_hold_enabled = false;
  Serial.printf("[CAM_PWR] hold disabled gpio=%d level=%d rc=%d\n",
                (int)CAM_PWDN_GPIO,
                gpio_get_level(CAM_PWDN_GPIO),
                (int)rc);
}

static void camera_power_enable() {
  camera_power_hold_disable();
  pinMode((int)CAM_PWDN_GPIO, OUTPUT);
  digitalWrite((int)CAM_PWDN_GPIO, LOW);
  delay(CAMERA_PWDN_WAKE_DELAY_MS);
  Serial.printf("[CAM_PWR] PWDN LOW (camera enabled) gpio=%d level=%d\n",
                (int)CAM_PWDN_GPIO,
                gpio_get_level(CAM_PWDN_GPIO));
}

static void camera_power_disable() {
  pinMode((int)CAM_PWDN_GPIO, OUTPUT);
  digitalWrite((int)CAM_PWDN_GPIO, HIGH);
  delay(CAMERA_PWDN_DISABLE_DELAY_MS);
  Serial.printf("[CAM_PWR] PWDN HIGH (camera disabled) gpio=%d level=%d\n",
                (int)CAM_PWDN_GPIO,
                gpio_get_level(CAM_PWDN_GPIO));
}

static void camera_power_hold_enable() {
  pinMode((int)CAM_PWDN_GPIO, OUTPUT);
  digitalWrite((int)CAM_PWDN_GPIO, HIGH);
  esp_err_t hold_rc = gpio_hold_en(CAM_PWDN_GPIO);
  gpio_deep_sleep_hold_en();
  g_cam_pwdn_hold_enabled = true;
  Serial.printf("[CAM_PWR] hold enabled gpio=%d level=%d rc=%d\n",
                (int)CAM_PWDN_GPIO,
                gpio_get_level(CAM_PWDN_GPIO),
                (int)hold_rc);
}

static void camera_stop_xclk() {
  bool detached = ledcDetach(XCLK_GPIO_NUM);
  pinMode(XCLK_GPIO_NUM, INPUT);
  Serial.printf("[CAM_PWR] XCLK stopped gpio=%d detached=%d\n",
                XCLK_GPIO_NUM,
                detached ? 1 : 0);
}

static void camera_set_pins_high_z() {
  const int pins[] = {
    XCLK_GPIO_NUM, PCLK_GPIO_NUM, VSYNC_GPIO_NUM, HREF_GPIO_NUM,
    Y2_GPIO_NUM, Y3_GPIO_NUM, Y4_GPIO_NUM, Y5_GPIO_NUM,
    Y6_GPIO_NUM, Y7_GPIO_NUM, Y8_GPIO_NUM, Y9_GPIO_NUM,
    SIOD_GPIO_NUM, SIOC_GPIO_NUM
  };
  for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
    if (pins[i] >= 0) {
      pinMode(pins[i], INPUT);
    }
  }
  Serial.println("[CAM_PWR] camera pins high-z");
}

// ── Fill LED ────────────────────────────────────────────────────────

static void capture_fill_led_set(bool on, const char* reason) {
  if (FILL_LED_PIN < 0) {
    return;
  }
  pinMode(FILL_LED_PIN, OUTPUT);
  digitalWrite(FILL_LED_PIN, on ? HIGH : LOW);
  if (CAMERA_DIAG) {
    Serial.printf("[FILL_LED] state=%s reason=%s pin=%d\n",
                  on ? "on" : "off",
                  reason ? reason : "unspecified",
                  FILL_LED_PIN);
  }
}

// ── Sensor profile functions ────────────────────────────────────────

static void tune_sensor_for_food() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return;

  // Ensure automatics are on
  s->set_exposure_ctrl(s, 1);   // AEC on
  s->set_gain_ctrl(s, 1);       // AGC on
  s->set_whitebal(s, 1);        // AWB on
  s->set_awb_gain(s, 1);        // AWB gain on

  // Brighter default; allow higher analog gain for low light
  s->set_ae_level(s, 2);        // -2..2 (2 = brighter)
  s->set_gainceiling(s, GAINCEILING_64X);

  // Gentle image tweaks
  s->set_brightness(s, 1);      // -2..2
  s->set_contrast(s, 0);        // -2..2
  s->set_saturation(s, 0);      // -2..2
  s->set_lenc(s, 0);            // disable lens correction to avoid color banding
  if (s->set_wb_mode) s->set_wb_mode(s, 2);  // Cloudy for more stable color

  // Orientation sane defaults
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);

  // Keep framesize/quality in sync with globals
  s->set_framesize(s, CAPTURE_SIZE);
  s->set_quality(s, JPEG_QUALITY);
}

static void apply_sensor_profile_normal(sensor_t* s) {
  if (!s) return;
  s->set_exposure_ctrl(s, 1);   // AEC on
  s->set_aec2(s, 0);            // AEC2 off for stability in normal light
  s->set_gain_ctrl(s, 1);       // AGC on
  s->set_ae_level(s, 1);        // Slightly brighter
  s->set_gainceiling(s, GAINCEILING_32X);
  s->set_brightness(s, 1);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  if (s->set_whitebal) s->set_whitebal(s, 1);
  if (s->set_awb_gain) s->set_awb_gain(s, 1);
  if (s->set_wb_mode) s->set_wb_mode(s, 2);  // Cloudy
  if (s->set_denoise) s->set_denoise(s, 0);
  if (s->set_sharpness) s->set_sharpness(s, 1);
  if (s->set_raw_gma) s->set_raw_gma(s, 1);
  s->set_lenc(s, 0);
}

static void apply_sensor_profile_low_light(sensor_t* s) {
  if (!s) return;
  s->set_exposure_ctrl(s, 1);   // AEC on
  s->set_aec2(s, 1);            // Allow longer exposure
  s->set_gain_ctrl(s, 1);       // AGC on
  s->set_ae_level(s, 2);        // Brighter
  s->set_gainceiling(s, GAINCEILING_64X);
  s->set_brightness(s, 2);
  s->set_contrast(s, -2);
  s->set_saturation(s, 1);
  if (s->set_whitebal) s->set_whitebal(s, 1);
  if (s->set_awb_gain) s->set_awb_gain(s, 1);
  if (s->set_wb_mode) s->set_wb_mode(s, 2);  // Cloudy
  if (s->set_denoise) s->set_denoise(s, 1);
  if (s->set_sharpness) s->set_sharpness(s, 1);
  if (s->set_raw_gma) s->set_raw_gma(s, 1);
  s->set_lenc(s, 0);
}

static void apply_sensor_profile_flash(sensor_t* s) {
  if (!s) return;
  s->set_exposure_ctrl(s, 1);   // AEC on
  s->set_aec2(s, 0);            // Avoid long exposures with direct fill light
  s->set_gain_ctrl(s, 1);       // AGC on
  s->set_ae_level(s, 0);        // Neutral exposure target
  s->set_gainceiling(s, GAINCEILING_16X);
  s->set_brightness(s, 0);
  s->set_contrast(s, 1);        // Preserve label edges under glare
  s->set_saturation(s, 0);
  if (s->set_whitebal) s->set_whitebal(s, 1);
  if (s->set_awb_gain) s->set_awb_gain(s, 1);
  if (s->set_wb_mode) s->set_wb_mode(s, 2);  // Cloudy
  if (s->set_denoise) s->set_denoise(s, 0);
  if (s->set_sharpness) s->set_sharpness(s, 2);
  if (s->set_raw_gma) s->set_raw_gma(s, 1);
  s->set_lenc(s, 0);
}

static void apply_sensor_profile_label(sensor_t* s) {
  if (!s) return;
  s->set_exposure_ctrl(s, 1);   // AEC on
  s->set_aec2(s, 0);            // No long exposures (keep text sharp)
  s->set_gain_ctrl(s, 1);       // AGC on
  s->set_ae_level(s, 1);        // Slightly brighter
  s->set_gainceiling(s, GAINCEILING_32X);
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);        // Enhanced contrast for label text
  s->set_saturation(s, 0);
  if (s->set_whitebal) s->set_whitebal(s, 1);
  if (s->set_awb_gain) s->set_awb_gain(s, 1);
  if (s->set_wb_mode) s->set_wb_mode(s, 0);  // Auto WB (adapts to food packaging lighting)
  if (s->set_denoise) s->set_denoise(s, 0);
  if (s->set_sharpness) s->set_sharpness(s, 3);  // Maximum sharpness for text
  if (s->set_raw_gma) s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);            // Lens correction ON (improves edge sharpness)
}

static void apply_camera_profile(sensor_t* s, CameraProfile profile) {
  if (!s) return;
  if (profile == CAM_PROFILE_LABEL) {
    apply_sensor_profile_label(s);
  } else if (profile == CAM_PROFILE_FLASH) {
    apply_sensor_profile_flash(s);
  } else if (profile == CAM_PROFILE_LOW_LIGHT) {
    apply_sensor_profile_low_light(s);
  } else {
    apply_sensor_profile_normal(s);
  }
}

static void apply_preflight_fast_profile(sensor_t* s) {
  if (!s) {
    return;
  }
  if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
  if (s->set_aec2) s->set_aec2(s, 0);
  if (s->set_gain_ctrl) s->set_gain_ctrl(s, 1);
  if (s->set_gainceiling) s->set_gainceiling(s, GAINCEILING_32X);
  if (s->set_ae_level) s->set_ae_level(s, 0);
  if (s->set_whitebal) s->set_whitebal(s, 1);
  if (s->set_awb_gain) s->set_awb_gain(s, 1);
}

static void tune_sensor_for_low_light() {
  sensor_t* s = esp_camera_sensor_get();
  apply_sensor_profile_low_light(s);
}

// ── Camera capture helpers ──────────────────────────────────────────

static void camera_settle_discard(uint8_t frames, uint32_t delay_ms) {
  for (uint8_t i = 0; i < frames; ++i) {
    camera_fb_t* tmp = esp_camera_fb_get();
    if (tmp) {
      esp_camera_fb_return(tmp);
    }
    if (delay_ms > 0) {
      delay(delay_ms);
    }
  }
}

static int compute_scene_brightness_preflight(sensor_t* s, int* green_ratio_out) {
  if (green_ratio_out) {
    *green_ratio_out = -1;
  }
  if (!s) {
    return -1;
  }
  uint32_t pre_start_ms = millis();
  camera_timeline_event("pre_budget_ms", (int32_t)CAMERA_PREFLIGHT_BUDGET_MS);
  apply_preflight_fast_profile(s);
  camera_timeline_event("pre_fast", 1);
  if (s->set_pixformat) {
    if (s->set_pixformat(s, PIXFORMAT_RGB565) != 0) {
      s->set_pixformat(s, PIXFORMAT_GRAYSCALE);
    }
  }
  s->set_framesize(s, CAMERA_PREFLIGHT_FRAMESIZE);
  camera_timeline_event("pre_warmup", CAMERA_PREFLIGHT_WARMUP_FRAMES);
  camera_settle_discard(CAMERA_PREFLIGHT_WARMUP_FRAMES, CAMERA_PREFLIGHT_WARMUP_DELAY_MS);
  if (CAMERA_PREFLIGHT_SETTLE_MS > 0) {
    delay(CAMERA_PREFLIGHT_SETTLE_MS);
  }
  uint32_t pre_elapsed_ms = millis() - pre_start_ms;
  if (pre_elapsed_ms > CAMERA_PREFLIGHT_BUDGET_MS) {
    camera_timeline_event("pre_timeout", (int32_t)pre_elapsed_ms);
    if (s->set_pixformat) {
      s->set_pixformat(s, PIXFORMAT_JPEG);
    }
    s->set_framesize(s, CAPTURE_SIZE);
    s->set_quality(s, JPEG_QUALITY);
    return -1;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  pre_elapsed_ms = millis() - pre_start_ms;
  if (pre_elapsed_ms > CAMERA_PREFLIGHT_BUDGET_MS) {
    camera_timeline_event("pre_timeout", (int32_t)pre_elapsed_ms);
    if (fb) {
      esp_camera_fb_return(fb);
    }
    if (s->set_pixformat) {
      s->set_pixformat(s, PIXFORMAT_JPEG);
    }
    s->set_framesize(s, CAPTURE_SIZE);
    s->set_quality(s, JPEG_QUALITY);
    return -1;
  }
  if (!fb) {
    camera_timeline_event("pre_fb_null", -1);
    if (s->set_pixformat) {
      s->set_pixformat(s, PIXFORMAT_JPEG);
    }
    s->set_framesize(s, CAPTURE_SIZE);
    s->set_quality(s, JPEG_QUALITY);
    return -1;
  }
  uint32_t sum_luma = 0;
  uint32_t sum_r = 0;
  uint32_t sum_g = 0;
  uint32_t sum_b = 0;
  if (fb->format == PIXFORMAT_RGB565) {
    const uint16_t* p = (const uint16_t*)fb->buf;
    size_t count = fb->len / 2;
    for (size_t i = 0; i < count; ++i) {
      uint16_t v = p[i];
      uint8_t r = (v >> 11) & 0x1F;
      uint8_t g = (v >> 5) & 0x3F;
      uint8_t b = v & 0x1F;
      uint8_t r8 = (uint8_t)((r * 255) / 31);
      uint8_t g8 = (uint8_t)((g * 255) / 63);
      uint8_t b8 = (uint8_t)((b * 255) / 31);
      sum_r += r8;
      sum_g += g8;
      sum_b += b8;
      sum_luma += (uint32_t)((r8 * 30 + g8 * 59 + b8 * 11) / 100);
    }
    if (green_ratio_out && count > 0) {
      uint32_t avg_r = sum_r / count;
      uint32_t avg_g = sum_g / count;
      uint32_t avg_b = sum_b / count;
      uint32_t rb = (avg_r + avg_b) / 2;
      *green_ratio_out = (rb > 0) ? (int)((avg_g * 100) / rb) : -1;
    }
    int luma = (count > 0) ? (int)(sum_luma / count) : -1;
    esp_camera_fb_return(fb);
    if (s->set_pixformat) {
      s->set_pixformat(s, PIXFORMAT_JPEG);
    }
    s->set_framesize(s, CAPTURE_SIZE);
    s->set_quality(s, JPEG_QUALITY);
    return luma;
  }
  if (fb->format == PIXFORMAT_GRAYSCALE) {
    const uint8_t* p = fb->buf;
    size_t count = fb->len;
    for (size_t i = 0; i < count; ++i) {
      sum_luma += p[i];
    }
    int luma = (count > 0) ? (int)(sum_luma / count) : -1;
    esp_camera_fb_return(fb);
    if (s->set_pixformat) {
      s->set_pixformat(s, PIXFORMAT_JPEG);
    }
    s->set_framesize(s, CAPTURE_SIZE);
    s->set_quality(s, JPEG_QUALITY);
    return luma;
  }
  esp_camera_fb_return(fb);
  if (s->set_pixformat) {
    s->set_pixformat(s, PIXFORMAT_JPEG);
  }
  s->set_framesize(s, CAPTURE_SIZE);
  s->set_quality(s, JPEG_QUALITY);
  return -1;
}

static bool is_frame_quality_ok(const camera_fb_t* fb, int scene_luma, int scene_green_ratio, bool strict) {
  if (!fb || fb->len == 0) {
    return false;
  }
  uint32_t area = (uint32_t)fb->width * (uint32_t)fb->height;
  uint32_t min_len = area / 60;
  if (min_len < 6000) {
    min_len = 6000;
  }
  if (fb->len < min_len) {
    if (CAMERA_DIAG) {
      Serial.printf("[CAMERA_DIAG] Reject frame: len=%u min=%u\n", fb->len, (unsigned)min_len);
    }
    return false;
  }
  if (strict && scene_luma >= 0 &&
      (scene_luma < CAMERA_PREFLIGHT_LUMA_LOW / 2 || scene_luma > CAMERA_PREFLIGHT_LUMA_HIGH + 10)) {
    if (CAMERA_DIAG) {
      Serial.printf("[CAMERA_DIAG] Reject frame: luma=%d\n", scene_luma);
    }
    return false;
  }
  if (strict && scene_green_ratio > 0 && scene_green_ratio >= CAMERA_PREFLIGHT_GREEN_RATIO_PCT) {
    if (CAMERA_DIAG) {
      Serial.printf("[CAMERA_DIAG] Reject frame: green_ratio=%d\n", scene_green_ratio);
    }
    return false;
  }
  return true;
}

// ── Camera deinit ───────────────────────────────────────────────────

static void deinit_camera() {
  capture_fill_led_set(false, "camera_deinit");
  Serial.println("[CAMERA] De-initializing camera to save power...");
  // Suppress benign GDMA disconnect error during camera deinit
  esp_log_level_set("gdma", ESP_LOG_NONE);
  esp_camera_deinit();
  esp_log_level_set("gdma", ESP_LOG_ERROR);
  Serial.println("[CAM_PWR] esp_camera_deinit complete");
  camera_stop_xclk();
  camera_set_pins_high_z();
  camera_power_disable();
  Serial.printf("[CAMERA] Free heap after camera deinit: %d bytes\n", ESP.getFreeHeap());
  Serial.println("[CAMERA] Camera de-initialized successfully");
}

// ── Camera init ─────────────────────────────────────────────────────

static bool init_camera() {
  Serial.println("[CAMERA] Initializing camera...");
  camera_timeline_event("init_begin", 0);

  auto log_camera_init_memory = [](const char* stage) {
    size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    Serial.printf("[CAMERA] Memory %s heap=%u max=%u dma_largest=%u psram=%u/%u\n",
                  stage ? stage : "unknown",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)dma_largest,
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)ESP.getPsramSize());
  };
  auto quiesce_network_for_camera = [&](const char* reason) {
    bool changed = false;
#ifndef HALO_SENSE_PROD_WRAPPER
    if (!waiting_for_mqtt_result) {
      mqtt_clear_result_subscription();
    }
    if (mqttClient.connected()) {
      Serial.printf("[CAMERA] Disconnecting MQTT before init reason=%s\n",
                    reason ? reason : "unknown");
      mqttClient.disconnect();
      changed = true;
    }
    wifiClient.stop();
#endif
    if (changed || reason != nullptr) {
      delay(CAMERA_NETWORK_QUIESCE_DELAY_MS);
    }
    log_camera_init_memory(reason ? reason : "post_quiesce");
  };

  log_camera_init_memory("pre_init");
  size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (dma_largest < CAMERA_DMA_LARGEST_BLOCK_MIN_BYTES
#ifndef HALO_SENSE_PROD_WRAPPER
      || mqttClient.connected()
#endif
  ) {
    Serial.printf("[CAMERA] Pre-init guard dma_largest=%u threshold=%u mqtt=%d\n",
                  (unsigned)dma_largest,
                  (unsigned)CAMERA_DMA_LARGEST_BLOCK_MIN_BYTES,
#ifndef HALO_SENSE_PROD_WRAPPER
                  mqttClient.connected() ? 1 : 0);
#else
                  0);
#endif
    quiesce_network_for_camera("pre_init_guard");
  }

  if (FILL_LED_PIN >= 0) {
    pinMode(FILL_LED_PIN, OUTPUT);
  }

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = CAMERA_XCLK_HZ;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = CAPTURE_SIZE;
  config.jpeg_quality = JPEG_QUALITY;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  g_camera_last_init_err = 0;
  for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
    camera_power_enable();
    Serial.printf("[CAM_PWR] esp_camera_init begin attempt=%u\n", attempt);
    esp_err_t err = esp_camera_init(&config);
    if (err == ESP_OK) {
      break;
    }

    g_camera_last_init_err = static_cast<int32_t>(err);
    camera_timeline_event("init_fail", g_camera_last_init_err);
    Serial.printf("[CAMERA] Camera init failed: 0x%x attempt=%u\n", err, attempt);
    Serial.printf("[CAMERA] Error details: %s\n", esp_err_to_name(err));
    log_camera_init_memory("init_fail");
    uart_send_sense_diag("camera", "init_fail", camera_diag_label(),
                         (int32_t)err, esp_err_to_name(err));

    if (attempt < 2) {
      esp_log_level_set("gdma", ESP_LOG_NONE);
      esp_camera_deinit();
      esp_log_level_set("gdma", ESP_LOG_ERROR);
      camera_stop_xclk();
      camera_set_pins_high_z();
      camera_power_disable();
      quiesce_network_for_camera("retry_after_init_fail");
      continue;
    }

    diag_record_error("camera_init", (int32_t)err, esp_err_to_name(err));
    camera_stop_xclk();
    camera_set_pins_high_z();
    camera_power_disable();
    return false;
  }

  log_camera_init_memory("post_init");
  Serial.println("[CAMERA] Camera initialized successfully");
  camera_timeline_event("init_ok", 0);
  uart_send_sense_diag("camera", "init_ok", camera_diag_label(), 0, "camera_ready");
  sensor_t* s = esp_camera_sensor_get();
  if (s && s->reset) {
    s->reset(s);
  }
  bool do_preflight = false;
  g_camera_profile = CAM_PROFILE_LABEL;
  if (CAMERA_PREFLIGHT_MODE == CAMERA_PREFLIGHT_ALWAYS) {
    do_preflight = true;
  } else if (CAMERA_PREFLIGHT_MODE == CAMERA_PREFLIGHT_ON_FAIL && g_camera_preflight_force) {
    do_preflight = true;
  }
  if (do_preflight) {
    int green_ratio = -1;
    camera_timeline_event("preflight_begin", 0);
    int scene_luma = compute_scene_brightness_preflight(s, &green_ratio);
    g_last_scene_luma = scene_luma;
    g_last_scene_green_ratio = green_ratio;
    camera_timeline_event("pre_luma", scene_luma);
    camera_timeline_event("pre_green", green_ratio);
    if (FILL_LED_PIN >= 0) {
      g_camera_profile = CAM_PROFILE_FLASH;
    } else if (scene_luma >= 0) {
      if (scene_luma < CAMERA_PREFLIGHT_LUMA_LOW) {
        g_camera_profile = CAM_PROFILE_LOW_LIGHT;
      } else {
        g_camera_profile = CAM_PROFILE_NORMAL;
      }
    } else {
      camera_timeline_event("pre_skip_budget", g_camera_profile == CAM_PROFILE_LOW_LIGHT ? 1 : 0);
    }
    if (CAMERA_DIAG) {
      Serial.printf("[CAMERA_DIAG] preflight_luma=%d green_ratio=%d profile=%s\n",
                    scene_luma,
                    green_ratio,
                    camera_profile_name(g_camera_profile));
    }
  } else {
    g_last_scene_luma = -1;
    g_last_scene_green_ratio = -1;
    camera_timeline_event("preflight_skip", g_camera_preflight_force ? 1 : 0);
  }
  g_camera_preflight_force = false;
  apply_camera_profile(s, g_camera_profile);
  camera_timeline_event("profile_set", (int32_t)g_camera_profile);
  if (s) {
    s->set_framesize(s, CAPTURE_SIZE);
    s->set_quality(s, JPEG_QUALITY);
  }
  if (CAMERA_INIT_SETTLE_DELAY_MS > 0) {
    delay(CAMERA_INIT_SETTLE_DELAY_MS);
  }
  camera_timeline_event("init_warmup", CAMERA_INIT_WARMUP_FRAMES);
  camera_settle_discard(CAMERA_INIT_WARMUP_FRAMES, CAMERA_INIT_WARMUP_DELAY_MS);
  return true;
}

// ── Warmup and capture ──────────────────────────────────────────────

static bool warmup_and_capture(camera_fb_t*& fb, bool fast_profile) {
  const uint32_t overall_start_ms = millis();
  const uint32_t budget_ms = fast_profile ? CAMERA_CAPTURE_BUDGET_FAST_MS : CAMERA_CAPTURE_BUDGET_SLOW_MS;
  const bool strict_quality = !fast_profile;
  camera_timeline_event(fast_profile ? "cap_fast" : "cap_slow", (int32_t)budget_ms);
  if (CAMERA_CAPTURE_SETTLE_MS > 0) {
    delay(CAMERA_CAPTURE_SETTLE_MS);
  }
  auto capture_with_warmup = [&](camera_fb_t*& out,
                                 int warmup_frames,
                                 uint8_t max_attempts,
                                 int min_warmup_success,
                                 bool fail_fast_on_warmup,
                                 uint32_t warmup_delay_ms) -> bool {
    uint32_t start_ms = millis();
  Serial.println("[CAMERA] Starting warmup and capture...");
  Serial.printf("[CAMERA] Free heap: %d bytes, Free PSRAM: %d bytes\n",
                ESP.getFreeHeap(), ESP.getFreePsram());
  camera_timeline_event("warmup_begin", warmup_frames);

  // Let AEC/AWB settle: grab & discard frames
  camera_fb_t* tmp = nullptr;
  int warmup_success = 0;
    int warmup_fail_streak = 0;
    for (int i = 0; i < warmup_frames; ++i) {
    tmp = esp_camera_fb_get();
    if (tmp) {
      warmup_success++;
        warmup_fail_streak = 0;
        Serial.printf("[CAMERA] Warmup frame %d: %u bytes\n", i + 1, tmp->len);
      esp_camera_fb_return(tmp);
      tmp = nullptr;
    } else {
        Serial.printf("[CAMERA] WARNING: Warmup frame %d failed\n", i + 1);
        warmup_fail_streak++;
        if (fail_fast_on_warmup && warmup_fail_streak >= 2) {
          Serial.println("[CAMERA] Warmup fail-fast: consecutive failures");
          camera_timeline_event("warmup_fail", warmup_success);
          diag_record_error("camera_warmup", warmup_success, "consecutive_fail");
          uart_send_sense_diag("camera", "warmup_fail", camera_diag_label(),
                               warmup_success, "consecutive_fail");
          return false;
        }
      }
      delay(warmup_delay_ms); // small settle delay
    }

    Serial.printf("[CAMERA] Warmup complete: %d/%d frames captured\n",
                  warmup_success, warmup_frames);
    camera_timeline_event("warmup_done", warmup_success);

    if (fail_fast_on_warmup && warmup_success < min_warmup_success) {
      Serial.printf("[CAMERA] Warmup fail-fast: success=%d min=%d\n",
                    warmup_success, min_warmup_success);
      camera_timeline_event("warmup_min", warmup_success);
      diag_record_error("camera_warmup", warmup_success, "min_fail");
      uart_send_sense_diag("camera", "warmup_fail", camera_diag_label(),
                           warmup_success, "min_fail");
      return false;
    }

    // Final capture (retry on NULL)
  Serial.println("[CAMERA] Attempting final capture...");
    camera_timeline_event("final_try", max_attempts);
    for (uint8_t attempt = 1; attempt <= max_attempts; ++attempt) {
      out = esp_camera_fb_get();
      if (out) {
        break;
      }
      Serial.printf("[CAMERA] WARNING: Final capture attempt %u/%u returned NULL\n",
                    attempt, max_attempts);
      delay(CAMERA_CAPTURE_RETRY_DELAY_MS);
    }

    if (!out) {
    Serial.println("[CAMERA] ERROR: Final capture returned NULL");
    camera_timeline_event("final_null", -1);
    diag_record_error("camera_capture", -1, "final_null");
    uart_send_sense_diag("camera", "final_null", camera_diag_label(), -1, "final_capture_null");
    return false;
  }

    if (!is_frame_quality_ok(out, g_last_scene_luma, g_last_scene_green_ratio, strict_quality)) {
      Serial.println("[CAMERA] Rejecting frame: quality check failed");
      camera_timeline_event("quality_reject", (int32_t)out->len);
      diag_record_error("camera_quality", (int32_t)out->len, "reject");
      uart_send_sense_diag("camera", "quality_reject", camera_diag_label(),
                           (int32_t)out->len, "reject");
      if (strict_quality) {
        esp_camera_fb_return(out);
        out = nullptr;
        return false;
      }
    }

    Serial.printf("[CAMERA] SUCCESS: Captured %u bytes (%dx%d) elapsed_ms=%lu\n",
                  out->len, out->width, out->height, (unsigned long)(millis() - start_ms));
    return true;
  };

  if (fast_profile) {
    if (capture_with_warmup(fb, 3, 1, 1, true, CAMERA_WARMUP_DELAY_FAST_MS)) {
      camera_timeline_complete(true, fb, "capture_ok");
      return true;
    }
  } else {
    if (capture_with_warmup(fb, 3, 2, 2, true, CAMERA_WARMUP_DELAY_SLOW_MS)) {
      camera_timeline_complete(true, fb, "capture_ok");
      return true;
    }
  }

  if (millis() - overall_start_ms > budget_ms) {
    if (CAMERA_DIAG) {
      Serial.println("[CAMERA_DIAG] budget exhausted, skipping retries");
    }
    camera_timeline_complete(false, nullptr, "budget");
    return false;
  }

  // Retry once with extra settle (same profile).
  if (CAMERA_DIAG) {
    Serial.println("[CAMERA_DIAG] retry: same profile settle");
  }
  camera_timeline_event("retry_same", 0);
  camera_settle_discard(1, CAMERA_RETRY_SETTLE_MS);
  if (fast_profile) {
    if (capture_with_warmup(fb, 3, 1, 1, true, CAMERA_WARMUP_DELAY_FAST_MS)) {
      camera_timeline_complete(true, fb, "capture_ok");
      return true;
    }
  } else {
    if (capture_with_warmup(fb, 3, 2, 2, true, CAMERA_WARMUP_DELAY_SLOW_MS)) {
      camera_timeline_complete(true, fb, "capture_ok");
      return true;
    }
  }

  if (millis() - overall_start_ms > budget_ms) {
    if (CAMERA_DIAG) {
      Serial.println("[CAMERA_DIAG] budget exhausted, skipping profile switch");
    }
    camera_timeline_complete(false, nullptr, "budget");
    return false;
  }

  // Switch profile and retry.
  CameraProfile original_profile = g_camera_profile;
  if (g_camera_profile == CAM_PROFILE_FLASH) {
    g_camera_profile = CAM_PROFILE_NORMAL;
  } else {
    g_camera_profile = (g_camera_profile == CAM_PROFILE_LOW_LIGHT) ? CAM_PROFILE_NORMAL : CAM_PROFILE_LOW_LIGHT;
  }
  sensor_t* s_profile = esp_camera_sensor_get();
  apply_camera_profile(s_profile, g_camera_profile);
  camera_timeline_event("retry_profile", (int32_t)g_camera_profile);
  if (CAMERA_DIAG) {
    Serial.printf("[CAMERA_DIAG] retry: switched profile to %s\n",
                  camera_profile_name(g_camera_profile));
  }
  camera_settle_discard(1, CAMERA_PROFILE_SWITCH_SETTLE_MS);
  if (fast_profile) {
    if (capture_with_warmup(fb, 1, 1, 1, true, CAMERA_WARMUP_DELAY_FAST_MS)) {
      camera_timeline_complete(true, fb, "capture_ok");
      return true;
    }
  } else {
    if (capture_with_warmup(fb, 3, 2, 2, true, CAMERA_WARMUP_DELAY_SLOW_MS)) {
      camera_timeline_complete(true, fb, "capture_ok");
      return true;
    }
  }

  if (millis() - overall_start_ms > budget_ms) {
    if (CAMERA_DIAG) {
      Serial.println("[CAMERA_DIAG] budget exhausted, skipping SVGA fallback");
    }
    camera_timeline_complete(false, nullptr, "budget");
    return false;
  }

  // Fallback framesize to SVGA, keep current profile.
  if (s_profile) {
    s_profile->set_framesize(s_profile, FRAMESIZE_SVGA);
  }
  camera_timeline_event("retry_svga", 1);
  if (CAMERA_DIAG) {
    Serial.println("[CAMERA_DIAG] retry: fallback framesize SVGA");
  }
  camera_settle_discard(1, CAMERA_SVGA_FALLBACK_SETTLE_MS);
  if (capture_with_warmup(fb, 2, 1, 1, false, CAMERA_WARMUP_DELAY_FAST_MS)) {
    camera_timeline_complete(true, fb, "capture_ok");
    return true;
  }

  if (millis() - overall_start_ms > budget_ms) {
    if (CAMERA_DIAG) {
      Serial.println("[CAMERA_DIAG] budget exhausted, skipping reinit");
    }
    camera_timeline_complete(false, nullptr, "budget");
    return false;
  }

  // Recovery path: re-init camera and retry once with a smaller framesize.
  if (!CAMERA_ENABLE_REINIT_FALLBACK) {
    camera_timeline_event("reinit_skip", 0);
    g_camera_profile = original_profile;
    camera_timeline_complete(false, nullptr, "capture_fail");
    return false;
  }
  camera_timeline_event("reinit_begin", 0);
  Serial.println("[CAMERA] Recovery: reinit + fallback framesize + low_light");
  deinit_camera();
  delay(200);
  if (!init_camera()) {
    Serial.println("[CAMERA] Recovery init failed");
    camera_timeline_event("reinit_fail", g_camera_last_init_err);
    diag_record_error("camera_reinit", -1, "init_fail");
    g_camera_profile = original_profile;
    camera_timeline_complete(false, nullptr, "reinit_fail");
    return false;
  }
  camera_timeline_event("reinit_ok", 0);
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_SVGA);
    apply_camera_profile(s, g_camera_profile);
  }
  bool ok = capture_with_warmup(fb, 3, 2, 1, false, 120);
  if (!ok) {
    g_camera_profile = original_profile;
    camera_timeline_complete(false, nullptr, "capture_fail");
  } else {
    camera_timeline_complete(true, fb, "capture_ok");
  }
  return ok;
}

#endif // SENSE_CAMERA_H

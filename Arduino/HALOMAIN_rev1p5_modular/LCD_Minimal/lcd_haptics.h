#pragma once

// Product policy: no vibration, regardless of stored settings or input type.
// The driver may retain power across an MCU update/reset. DRV2605/2605L standby
// immediately stops playback while retaining register and I2C access. Never
// clear standby, configure an effect, or write GO=1 in this firmware.
// Call only after the existing touch initialization installs I2C0.
static constexpr uint8_t HAPTIC_ADDR = 0x5A;
static constexpr i2c_port_t HAPTIC_I2C_PORT = I2C_NUM_0;
static constexpr uint8_t HAPTIC_MODE = 0x01;
static constexpr uint8_t HAPTIC_RTP = 0x02;
static constexpr uint8_t HAPTIC_GO = 0x0C;
static constexpr uint8_t HAPTIC_STANDBY = 0x40;

static bool haptic_write(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return i2c_master_write_to_device(HAPTIC_I2C_PORT, HAPTIC_ADDR, data,
      sizeof(data), pdMS_TO_TICKS(20)) == ESP_OK;
}

static bool haptic_read(uint8_t reg, uint8_t* value) {
  return i2c_master_write_read_device(HAPTIC_I2C_PORT, HAPTIC_ADDR, &reg, 1,
      value, 1, pdMS_TO_TICKS(20)) == ESP_OK;
}

// Direct USB diagnostics are observation-only; a missing/unresponsive driver
// must not be reported as verified or trigger playback/configuration retries.
static bool haptic_report() {
  uint8_t mode = 0, go = 0, rtp = 0;
  const bool mode_ok = haptic_read(HAPTIC_MODE, &mode);
  const bool go_ok = haptic_read(HAPTIC_GO, &go);
  const bool rtp_ok = haptic_read(HAPTIC_RTP, &rtp);
  const bool verified = mode_ok && go_ok && rtp_ok &&
      (mode & 0xC7) == HAPTIC_STANDBY && (go & 1) == 0 && rtp == 0;
  Serial.printf("[HAPTIC] policy=disabled readable=%u mode=%u go=%u rtp=%u standby_verified=%u\n",
      mode_ok && go_ok && rtp_ok ? 1U : 0U, unsigned(mode), unsigned(go),
      unsigned(rtp), verified ? 1U : 0U);
  return verified;
}

static void haptic_init() {
  // Standby first: quiesce any old effect before clearing its retained inputs.
  // Try all stop writes even after a NACK. No unbounded loop, allocation, task,
  // setting or peripheral-power change is introduced; each I2C timeout is20ms.
  haptic_write(HAPTIC_MODE, HAPTIC_STANDBY);
  haptic_write(HAPTIC_GO, 0);
  haptic_write(HAPTIC_RTP, 0);
  haptic_report();
}

#pragma once

#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_heap_caps.h>
#include "../halo_ota_demo/firmware/shared/SystemPower.h"

// ADC/calibration lifetime belongs only to loop(). uart_task borrows a POD copy,
// never the handles. No timer, task, wake pulse, NVS write or network work.
static adc_oneshot_unit_handle_t s_lcd_power_adc = nullptr;
static adc_cali_handle_t s_lcd_power_cal = nullptr;
static bool s_lcd_power_initialized = false;
static halo_power::Status s_lcd_power_init_status = halo_power::Status::AdcUnavailable;
static uint32_t s_lcd_power_sequence = 0;
static uint64_t s_lcd_power_last_attempt_ms = 0;
static portMUX_TYPE s_lcd_power_mux = portMUX_INITIALIZER_UNLOCKED;
static halo_power::Snapshot s_lcd_power_sample;
static uint32_t s_lcd_power_sent_sequence = 0;
static uint32_t s_lcd_power_attempted_sequence = 0;
static uint32_t s_lcd_power_forced_sequence = 0;
// UART-task-only transport state. General worker traffic is not proof that
// Sense's control parser is draining its finite receive buffers.
static uint64_t s_lcd_power_tx_attempt_ms = 0;
static uint64_t s_lcd_power_control_reply_ms = 0;
static bool s_lcd_power_control_reply_seen = false;
static constexpr uint64_t LCD_POWER_TX_INTERVAL_MS = 3000;
static constexpr uint64_t LCD_POWER_CONTROL_PROOF_MS = 2000;

// A sleep attempt gets one absolute 150ms opportunity. Return to loop() to
// sample outside the LVGL lock, and let uart_task write before INPUT_SLEEP.
static bool s_lcd_power_sleep_pending = false;
static bool s_lcd_power_sleep_seen = false;
static uint64_t s_lcd_power_sleep_started_ms = 0;
static uint32_t s_lcd_power_sleep_sequence = 0;
static unsigned long s_lcd_power_sleep_user_ms = 0, s_lcd_power_sleep_scroll_ms = 0;
static constexpr uint64_t LCD_POWER_SLEEP_BUDGET_MS = 150;
static constexpr uint64_t LCD_POWER_BURST_BUDGET_MS = 100;

static uint64_t lcd_power_now_ms() { return uint64_t(esp_timer_get_time()) / 1000; }

static void lcd_power_deinit() {
  if (s_lcd_power_cal) {
    adc_cali_delete_scheme_curve_fitting(s_lcd_power_cal);
    s_lcd_power_cal = nullptr;
  }
  if (s_lcd_power_adc) {
    adc_oneshot_del_unit(s_lcd_power_adc);
    s_lcd_power_adc = nullptr;
  }
}

static void lcd_power_init() {
  if (s_lcd_power_initialized) return;
  s_lcd_power_initialized = true; // Fail closed for this boot; no allocation retry loop.
  adc_oneshot_unit_init_cfg_t unit{};
  unit.unit_id = ADC_UNIT_1;
  if (adc_oneshot_new_unit(&unit, &s_lcd_power_adc) != ESP_OK) {
    lcd_power_deinit(); return;
  }
  adc_oneshot_chan_cfg_t channel{};
  channel.atten = ADC_ATTEN_DB_12; channel.bitwidth = ADC_BITWIDTH_12;
  if (adc_oneshot_config_channel(s_lcd_power_adc, ADC_CHANNEL_0, &channel) != ESP_OK) {
    lcd_power_deinit(); return;
  }
  adc_cali_curve_fitting_config_t calibration{};
  calibration.unit_id = ADC_UNIT_1; calibration.chan = ADC_CHANNEL_0;
  calibration.atten = ADC_ATTEN_DB_12; calibration.bitwidth = ADC_BITWIDTH_12;
  if (adc_cali_create_scheme_curve_fitting(&calibration, &s_lcd_power_cal) != ESP_OK) {
    s_lcd_power_init_status = halo_power::Status::CalibrationUnavailable;
    lcd_power_deinit(); return;
  }
  s_lcd_power_init_status = halo_power::Status::Ok;
}

static halo_power::View lcd_power_view() {
  halo_power::View view;
  portENTER_CRITICAL(&s_lcd_power_mux);
  view.sample = s_lcd_power_sample;
  portEXIT_CRITICAL(&s_lcd_power_mux);
  view.received = view.sample.sequence != 0;
  const uint64_t now = lcd_power_now_ms();
  view.age_ms = now >= view.sample.uptime_ms ? now - view.sample.uptime_ms : UINT64_MAX;
  return view;
}

// USB-only cached diagnostic. The full cloud JSON is larger than the 256-byte
// HWCDC TX buffer; keep this versioned line small and make one bounded write.
// Lack of space drops this observation; never wait, retry, sample or wake.
static size_t lcd_power_diag_format(const halo_power::View& v, char* out, size_t capacity) {
  if (!out || !capacity) return 0;
  const halo_power::Snapshot& s = v.sample;
  const bool valid = v.received && halo_power::valid(s);
  const bool age_known = v.received && v.age_known;
  char mv[8] = "null", age[24] = "null";
  if (valid) snprintf(mv, sizeof(mv), "%u", unsigned(s.system_supply_mv));
  if (age_known) snprintf(age, sizeof(age), "%llu", (unsigned long long)v.age_ms);
  const int n = snprintf(out, capacity,
      "[POWER1] {\"v\":1,\"b\":%lu,\"q\":%lu,\"s\":%u,\"mv\":%s,\"a\":%s,"
      "\"f\":%s,\"ok\":%s,\"lo\":%u,\"hi\":%u,\"n\":%u}\n",
      (unsigned long)s.boot_id, (unsigned long)s.sequence,
      unsigned(v.received ? s.status : halo_power::Status::NotSampled), mv, age,
      valid && age_known && v.age_ms <= halo_power::kFreshMs ? "true" : "false",
      valid ? "true" : "false", unsigned(s.raw_min), unsigned(s.raw_max), unsigned(s.samples));
  if (n <= 0 || size_t(n) >= capacity) { out[0] = '\0'; return 0; }
  return size_t(n);
}

static void lcd_power_diag_write() {
  char line[192];
  const size_t n = lcd_power_diag_format(lcd_power_view(), line, sizeof(line));
  if (n && Serial.availableForWrite() >= int(n))
    (void)Serial.write(reinterpret_cast<const uint8_t*>(line), n);
}

static void lcd_power_sleep_cancel() {
  s_lcd_power_sleep_pending = false;
  s_lcd_power_sleep_sequence = 0;
}

// Called once at loop entry, before any example_lvgl_lock. A partial burst is
// invalid; never retry until 32 successes or spin while another owner is busy.
static void lcd_power_owner_service(bool acquisition_allowed) {
  if (s_lcd_power_sleep_pending && (!s_lcd_power_sleep_seen ||
      last_user_activity_ms != s_lcd_power_sleep_user_ms ||
      last_scroll_activity_ms != s_lcd_power_sleep_scroll_ms)) lcd_power_sleep_cancel();
  s_lcd_power_sleep_seen = false;
  const uint64_t now = lcd_power_now_ms();
  const bool forced = s_lcd_power_sleep_pending && !s_lcd_power_sleep_sequence;
  if (!acquisition_allowed || (forced && now - s_lcd_power_sleep_started_ms >= LCD_POWER_SLEEP_BUDGET_MS)) return;
  if (!forced && s_lcd_power_sequence && now - s_lcd_power_last_attempt_ms < 1000) return;
  s_lcd_power_last_attempt_ms = now;
  const bool first_init = !s_lcd_power_initialized;
  const size_t heap_before = first_init ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : 0;
  const size_t largest_before = first_init ? heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : 0;
  lcd_power_init();
  if (first_init) {
    Serial.printf("[POWER_INIT] status=%u internal_before=%u internal_after=%u largest_before=%u largest_after=%u\n",
        unsigned(s_lcd_power_init_status), unsigned(heap_before),
        unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), unsigned(largest_before),
        unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  }
  halo_power::Snapshot sample;
  sample.uptime_ms = now; sample.boot_id = g_lcd_coord_boot_id;
  if (++s_lcd_power_sequence == 0) ++s_lcd_power_sequence;
  sample.sequence = s_lcd_power_sequence;
  const time_t epoch = time(nullptr);
  sample.epoch_s = epoch >= 1700000000 ? uint64_t(epoch) : 0;
  halo_power::Burst burst;
  burst.status = s_lcd_power_init_status;
  if (burst.status == halo_power::Status::Ok) {
    for (uint8_t i = 0; i < halo_power::kSamples; ++i) {
      const uint64_t tick = lcd_power_now_ms();
      if (tick - now >= LCD_POWER_BURST_BUDGET_MS ||
          (forced && tick - s_lcd_power_sleep_started_ms >= LCD_POWER_SLEEP_BUDGET_MS)) {
        burst.status = halo_power::Status::ReadError; break;
      }
      int raw = 0, mv = 0;
      if (adc_oneshot_read(s_lcd_power_adc, ADC_CHANNEL_0, &raw) != ESP_OK) {
        burst.status = halo_power::Status::ReadError; break;
      }
      if (raw <= 0 || raw >= 4095) { burst.status = halo_power::Status::Saturated; break; }
      if (adc_cali_raw_to_voltage(s_lcd_power_cal, raw, &mv) != ESP_OK) {
        burst.status = halo_power::Status::ReadError; break;
      }
      if (!burst.add(raw, mv)) break;
      if (i + 1 < halo_power::kSamples) delay(1);
    }
  }
  burst.finish(sample);
  portENTER_CRITICAL(&s_lcd_power_mux);
  s_lcd_power_sample = sample; // One latest-value mailbox, never an accumulating queue.
  s_lcd_power_forced_sequence = forced ? sample.sequence : 0;
  portEXIT_CRITICAL(&s_lcd_power_mux);
  if (forced) s_lcd_power_sleep_sequence = sample.sequence;
}

static bool lcd_power_prepare_sleep(bool peer_awake) {
  s_lcd_power_sleep_seen = true;
  const uint64_t now = lcd_power_now_ms();
  if (!s_lcd_power_sleep_pending) {
    s_lcd_power_sleep_pending = true;
    s_lcd_power_sleep_started_ms = now;
    s_lcd_power_sleep_user_ms = last_user_activity_ms;
    s_lcd_power_sleep_scroll_ms = last_scroll_activity_ms;
    s_lcd_power_sleep_sequence = 0;
    return false;
  }
  if (now - s_lcd_power_sleep_started_ms >= LCD_POWER_SLEEP_BUDGET_MS) return true;
  if (!s_lcd_power_sleep_sequence) return false;
  if (!peer_awake) return true; // Never wake an absent/sleeping peer for telemetry.
  portENTER_CRITICAL(&s_lcd_power_mux);
  const bool written = s_lcd_power_sent_sequence == s_lcd_power_sleep_sequence;
  portEXIT_CRITICAL(&s_lcd_power_mux);
  return written; // Local full write only, not an acknowledgement from Sense.
}

struct LcdPowerSleepScope { ~LcdPowerSleepScope() { lcd_power_sleep_cancel(); } };

// These two legacy headless sleep paths are called only from loop(), after its
// LVGL unlock. They can take the bounded burst locally without another loop or
// peer wait. Existing sleep admission happens first; transfer priority remains.
static void lcd_power_headless_sample(bool acquisition_allowed) {
  lcd_power_sleep_cancel();
  (void)lcd_power_prepare_sleep(false);
  lcd_power_owner_service(acquisition_allowed);
  lcd_power_sleep_cancel();
}

// Called only by uart_task in ordinary JSON mode, with existing awake proof.
// The UART task also owns every transition into binary mode, so the check and
// write cannot race its RX dispatch. Keep this out of the deferred wake queue.
static void lcd_power_note_control_reply() {
  s_lcd_power_control_reply_ms = lcd_power_now_ms();
  s_lcd_power_control_reply_seen = true;
}

static void lcd_power_uart_service(bool send_allowed) {
  if (!send_allowed) return;
  const uint64_t now = lcd_power_now_ms();
  if (!s_lcd_power_control_reply_seen || now < s_lcd_power_control_reply_ms ||
      now - s_lcd_power_control_reply_ms > LCD_POWER_CONTROL_PROOF_MS) return;
  const halo_power::View view = lcd_power_view();
  portENTER_CRITICAL(&s_lcd_power_mux);
  const bool attempted = view.sample.sequence == s_lcd_power_attempted_sequence;
  const bool forced = view.sample.sequence == s_lcd_power_forced_sequence;
  const bool first = s_lcd_power_attempted_sequence == 0;
  const bool cadence_ready = first || forced || now - s_lcd_power_tx_attempt_ms >= LCD_POWER_TX_INTERVAL_MS;
  if (view.received && !attempted && cadence_ready) s_lcd_power_attempted_sequence = view.sample.sequence;
  portEXIT_CRITICAL(&s_lcd_power_mux);
  if (!view.received || attempted || !cadence_ready) return;
  s_lcd_power_tx_attempt_ms = now; // One attempt per sample, including short writes.
  const halo_power::Snapshot& s = view.sample;
  char frame[256]; // Max-width wire fixture is 220 bytes including delimiters.
  const int size = snprintf(frame, sizeof(frame),
    "\n{\"ver\":%u,\"type\":\"LCD_POWER\",\"msg_id\":%lu,\"ts\":%lu,"
    "\"p\":{\"v\":1,\"b\":%lu,\"q\":%lu,\"u\":%llu,\"e\":%llu,\"a\":%llu,"
    "\"s\":%u,\"mv\":%u,\"lo\":%u,\"hi\":%u,\"n\":%u}}\n",
    unsigned(PROTOCOL_VERSION), (unsigned long)get_next_msg_id(), (unsigned long)millis(),
    (unsigned long)s.boot_id, (unsigned long)s.sequence,
    (unsigned long long)s.uptime_ms, (unsigned long long)s.epoch_s,
    (unsigned long long)view.age_ms, unsigned(s.status), unsigned(s.system_supply_mv),
    unsigned(s.raw_min), unsigned(s.raw_max), unsigned(s.samples));
  if (size <= 0 || size_t(size) >= sizeof(frame) || !lcd_power_uart_write(frame)) return;
  portENTER_CRITICAL(&s_lcd_power_mux);
  s_lcd_power_sent_sequence = s.sequence;
  portEXIT_CRITICAL(&s_lcd_power_mux);
}

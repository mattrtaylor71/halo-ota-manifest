#pragma once

// Local USB observation only. Included after the real LCD state definitions;
// request, snapshot and service all run on the existing UART task. No UART TX,
// NVS, wake, activity, lease acquisition or OTA policy calls belong here.
#include <errno.h>
#include "../halo_ota_demo/firmware/shared/BuildInfo.h"

struct LcdId1Pending {
  char frame[224];
  uint32_t started_ms;
  uint32_t checked_ms;
  uint32_t rejected;
  uint32_t expired;
  uint32_t short_writes;
  uint32_t enqueued;
  uint16_t length;
  uint8_t previous_length;
  uint8_t previous_returned;
  bool pending;
  bool checked;
};
static LcdId1Pending s_lcd_id1 = {};
static_assert(sizeof(LcdId1Pending) <= 320, "ID1 pending RAM bound");

static void lcd_id1_count(uint32_t& value) { if (value != UINT32_MAX) ++value; }

// Match the existing half-range deadline convention, including zero=inactive.
// This read-only calculation must not expire/clear either coordinator atomic.
static uint32_t lcd_id1_remaining(uint32_t deadline, uint32_t now) {
  const int32_t left = (int32_t)(deadline - now);
  return deadline && left > 0 ? (uint32_t)left : 0;
}

static char lcd_id1_slot(const esp_partition_t* part) {
  if (!part) return '?';
  if (strcmp(part->label, "app0") == 0) return '0';
  if (strcmp(part->label, "app1") == 0) return '1';
  return '?';
}

static char lcd_id1_state(const esp_partition_t* running) {
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return '?';
  switch (state) {
    case ESP_OTA_IMG_NEW: return 'N';
    case ESP_OTA_IMG_PENDING_VERIFY: return 'P';
    case ESP_OTA_IMG_VALID: return 'V';
    case ESP_OTA_IMG_INVALID: return 'I';
    case ESP_OTA_IMG_ABORTED: return 'A';
    case ESP_OTA_IMG_UNDEFINED: return 'U';
    default: return '?';
  }
}

static bool lcd_id1_token(const char* text, size_t maximum) {
  if (!text || !text[0]) return false;
  for (size_t i = 0; i <= maximum; ++i) {
    const unsigned char c = (unsigned char)text[i];
    if (!c) return true;
    if (i == maximum || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  }
  return false;
}

static bool lcd_id1_binary_active() {
  return g_lcd_ota_binary_mode || g_img_rx_binary_mode ||
         g_spool_tx_pending || g_spool_tx_active;
}

static bool lcd_id1_format(const char* nonce, uint32_t now) {
  if (!lcd_id1_token(kFirmwareVersion, 31) || !kBuildId || !kBuildId[0]) return false;
  size_t build_length = 0;
  while (build_length < 96 && kBuildId[build_length]) ++build_length;
  if (build_length == 96) return false;
  uint8_t digest[32];
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(kBuildId),
                     build_length, digest, 0) != 0) return false;
  char fingerprint[33];
  const char* hex = "0123456789abcdef";
  for (size_t i = 0; i < 16; ++i) {
    fingerprint[i * 2] = hex[digest[i] >> 4];
    fingerprint[i * 2 + 1] = hex[digest[i] & 15];
  }
  fingerprint[32] = '\0';

  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
  const uint32_t wait_until = g_lcd_timer_receiver_wait_until_ms.load();
  const uint32_t lease_until = g_lcd_coord_lease_until_ms.load();
  const uint32_t lease = lcd_id1_remaining(lease_until, now);
  // owner is written/read only on this UART task. Expired owner bytes remain
  // untouched and are reported inactive, as in the existing owner accessor.
  const bool owner = lease && g_lcd_coord_owner[0];
  const int body = snprintf(s_lcd_id1.frame, sizeof(s_lcd_id1.frame),
      "[ID1] q=%s p=%08lx t=%08lx f=%s h=%s r=%c b=%c n=%c s=%c d=%u w=%u o=%u l=%08lx a=%u u=%u z=%08lx x=%02x%02x%08lx%08lx",
      nonce, (unsigned long)g_lcd_coord_boot_id, (unsigned long)now,
      kFirmwareVersion, fingerprint, lcd_id1_slot(running), lcd_id1_slot(boot),
      lcd_id1_slot(next), lcd_id1_state(running), g_lcd_boot_ready.load() ? 1U : 0U,
      lcd_id1_remaining(wait_until, now) ? 1U : 0U, owner ? 1U : 0U,
      (unsigned long)lease, lcd_ota_uart_active() ? 1U : 0U,
      lcd_id1_binary_active() ? 1U : 0U, (unsigned long)(next ? next->size : 0),
      (unsigned)s_lcd_id1.previous_length, (unsigned)s_lcd_id1.previous_returned,
      (unsigned long)s_lcd_id1.short_writes, (unsigned long)s_lcd_id1.expired);
  if (body < 0 || body > 196) return false;
  const uint16_t crc = UartOtaProtocol::crc16_ccitt(
      reinterpret_cast<const uint8_t*>(s_lcd_id1.frame), (size_t)body);
  const int suffix = snprintf(s_lcd_id1.frame + body, sizeof(s_lcd_id1.frame) - (size_t)body,
      " bytes=%u crc=%04x\n", (unsigned)body, (unsigned)crc);
  if (suffix < 0 || (size_t)suffix >= sizeof(s_lcd_id1.frame) - (size_t)body ||
      body + suffix > 216) return false;
  s_lcd_id1.length = (uint16_t)(body + suffix);
  return true;
}

static void lcd_id1_request(const char* command) {
  const int saved_errno = errno;
  // One pending request cannot be replaced or have its expiry renewed.
  bool valid = command && strncmp(command, "id1 ", 4) == 0;
  if (valid) {
    for (size_t i = 0; i < 16; ++i) {
      const char c = command[4 + i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { valid = false; break; }
    }
    if (valid && command[20] != '\0') valid = false;
  }
  if (!valid || s_lcd_id1.pending) { lcd_id1_count(s_lcd_id1.rejected); errno = saved_errno; return; }
  const uint32_t now = (uint32_t)millis();
  if (!lcd_id1_format(command + 4, now)) { lcd_id1_count(s_lcd_id1.rejected); errno = saved_errno; return; }
  s_lcd_id1.started_ms = now;
  s_lcd_id1.checked = false;
  s_lcd_id1.pending = true;
  errno = saved_errno;
}

static void lcd_id1_service() {
  if (!s_lcd_id1.pending) return;
  const int saved_errno = errno;
  const uint32_t now = (uint32_t)millis();
  if ((uint32_t)(now - s_lcd_id1.started_ms) >= 250) {
    s_lcd_id1.pending = false; lcd_id1_count(s_lcd_id1.expired); errno = saved_errno; return;
  }
  if (s_lcd_id1.checked && (uint32_t)(now - s_lcd_id1.checked_ms) < 10) { errno = saved_errno; return; }
  s_lcd_id1.checked_ms = now; s_lcd_id1.checked = true;
  // UART work wins if the snapshot has since become unsafe to service.
  if (lcd_ota_uart_active() || lcd_id1_binary_active()) {
    s_lcd_id1.pending = false; lcd_id1_count(s_lcd_id1.rejected); errno = saved_errno; return;
  }
  // Advisory capacity is not an atomic reservation. The SDK call remains
  // synchronous and may exceed our cooperative expiry; no hard bound claimed.
  if (!Serial || Serial.availableForWrite() < (int)s_lcd_id1.length) { errno = saved_errno; return; }
  s_lcd_id1.pending = false; // consume BEFORE the only write, including short/zero
  const size_t written = Serial.write(reinterpret_cast<const uint8_t*>(s_lcd_id1.frame), s_lcd_id1.length);
  s_lcd_id1.previous_length = (uint8_t)s_lcd_id1.length;
  // ff is an explicit impossible/out-of-range driver result, not full success.
  s_lcd_id1.previous_returned = written <= s_lcd_id1.length ? (uint8_t)written : 0xff;
  if (written == s_lcd_id1.length) lcd_id1_count(s_lcd_id1.enqueued);
  else lcd_id1_count(s_lcd_id1.short_writes);
  errno = saved_errno;
}

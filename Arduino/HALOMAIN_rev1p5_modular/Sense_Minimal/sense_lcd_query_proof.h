#ifndef SENSE_LCD_QUERY_PROOF_H
#define SENSE_LCD_QUERY_PROOF_H

#include <errno.h>

// USB evidence only. No protocol send, retained storage, delays or retries.
// The caller supplies one response after the strict UART parser accepts it.
static bool sense_lcd_query_proof_token(const char* value, size_t maximum) {
  if (!value || !value[0]) return false;
  for (size_t i = 0; ; ++i) {
    const unsigned char c = (unsigned char)value[i];
    if (!c) return i <= maximum;
    if (i >= maximum || !((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == '.')) return false;
  }
}

static bool sense_lcd_query_proof_emit(Stream& out, const char* coord_id,
    uint32_t peer_boot_id, const char* fw, const char* running,
    const char* boot, const char* state, bool ready, uint32_t part_size) {
  struct RestoreErrno {
    int saved;
    ~RestoreErrno() { errno = saved; }
  } restore_errno = {errno};
  // A legacy omitted challenge is explicit; it cannot satisfy fresh matching.
  const char* challenge = coord_id && coord_id[0] ? coord_id : "-";
  if (!sense_lcd_query_proof_token(challenge, 39) ||
      !sense_lcd_query_proof_token(fw, 31) ||
      !sense_lcd_query_proof_token(running, 15) ||
      !sense_lcd_query_proof_token(boot, 15) ||
      !sense_lcd_query_proof_token(state, 19)) return false;

  char record[224];
  const int body = snprintf(record, sizeof(record),
      "[LCD_Q1] q=%s p=%lu fw=%s run=%s boot=%s state=%s ready=%u size=%lu",
      challenge, (unsigned long)peer_boot_id, fw, running, boot, state,
      ready ? 1U : 0U, (unsigned long)part_size);
  if (body < 0 || (size_t)body >= sizeof(record)) return false;
  const uint16_t crc = UartOtaProtocol::crc16_ccitt(
      reinterpret_cast<const uint8_t*>(record), (size_t)body);
  const int suffix = snprintf(record + body, sizeof(record) - (size_t)body,
      " bytes=%u crc=%04x\n", (unsigned)body, (unsigned)crc);
  if (suffix < 0 || (size_t)suffix >= sizeof(record) - (size_t)body) return false;
  const size_t length = (size_t)body + (size_t)suffix;
  // One existing-style bounded write; the USB driver's own write mutex avoids
  // application-level interleaving within this call. Partial or later USB loss
  // is detected by the host length/CRC. It never changes the accepted response.
  return out.write(reinterpret_cast<const uint8_t*>(record), length) == length;
}

#endif

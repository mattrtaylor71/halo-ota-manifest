#ifndef LCD_NVS_RECORDS_H
#define LCD_NVS_RECORDS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Each independent transition has one authoritative NVS value. A tombstone
// remains a value: an invalid or cleared new record never reloads legacy keys.
namespace LcdNvsRecords {
enum class ReadStatus : uint8_t { Absent, Present, Invalid, IoError, Busy };
static constexpr uint32_t kFormat = 2;
static constexpr uint32_t kCoordMagic = 0x4c434f32;
static constexpr uint32_t kContinuationMagic = 0x4c435432;
static constexpr uint32_t kProgressMagic = 0x4c505232;

struct Coord {
  uint32_t magic, format, pending, epoch, wake, reset, resumes;
  char schedule[64];
  uint32_t crc;
};
struct Continuation {
  uint32_t magic, format, pending, target_address, image_size;
  uint8_t app_elf_sha256[32];
  char version[32];
  uint32_t crc;
};
struct Progress {
  uint32_t magic, format, pending, session, offset, image_size;
  uint8_t sha256[32];
  char version[32];
  uint32_t crc;
};
static_assert(sizeof(Coord) == 96, "Durable coord layout changed");
static_assert(sizeof(Continuation) == 88, "Durable continuation layout changed");
static_assert(sizeof(Progress) == 92, "Durable progress layout changed");

inline uint32_t crc32(const void* data, size_t length) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= p[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
inline bool zero(const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) if (p[i]) return false;
  return true;
}
inline bool text(const char* value, size_t size) {
  size_t end = 0;
  for (; end < size && value[end]; ++end)
    if (static_cast<unsigned char>(value[end]) < 0x21 ||
        static_cast<unsigned char>(value[end]) > 0x7e) return false;
  return end > 0 && end < size && zero(value + end, size - end);
}
inline void seal(Coord& r) {
  r.magic = kCoordMagic; r.format = kFormat;
  r.crc = crc32(&r, offsetof(Coord, crc));
}
inline void seal(Continuation& r) {
  r.magic = kContinuationMagic; r.format = kFormat;
  r.crc = crc32(&r, offsetof(Continuation, crc));
}
inline void seal(Progress& r) {
  r.magic = kProgressMagic; r.format = kFormat;
  r.crc = crc32(&r, offsetof(Progress, crc));
}
inline bool valid(const Coord& r) {
  if (r.magic != kCoordMagic || r.format != kFormat || r.pending > 1 ||
      r.crc != crc32(&r, offsetof(Coord, crc))) return false;
  if (!r.pending) return !r.epoch && !r.wake && !r.reset && !r.resumes &&
      zero(r.schedule, sizeof(r.schedule));
  // ESP_SLEEP_WAKEUP_TIMER is 4. A restored ID is never a fresh timer cause.
  return r.wake == 4 && r.epoch >= 1700000000U && r.resumes <= 2 &&
      r.reset <= 255 && text(r.schedule, sizeof(r.schedule));
}
inline bool valid(const Continuation& r) {
  if (r.magic != kContinuationMagic || r.format != kFormat || r.pending > 1 ||
      r.crc != crc32(&r, offsetof(Continuation, crc))) return false;
  if (!r.pending) return !r.target_address && !r.image_size &&
      zero(r.app_elf_sha256, sizeof(r.app_elf_sha256)) && zero(r.version, sizeof(r.version));
  // The verified target's app descriptor identity is compared with the running
  // descriptor on consumption. It is not relabeled as the full transfer SHA.
  // Actual slot membership and size are checked at the partition boundary.
  return r.target_address && r.image_size && !zero(r.app_elf_sha256,sizeof(r.app_elf_sha256)) &&
      text(r.version, sizeof(r.version));
}
inline bool valid(const Progress& r) {
  if (r.magic != kProgressMagic || r.format != kFormat || r.pending > 1 ||
      r.crc != crc32(&r, offsetof(Progress, crc))) return false;
  if (!r.pending) return !r.session && !r.offset && !r.image_size &&
      zero(r.sha256, sizeof(r.sha256)) && zero(r.version, sizeof(r.version));
  return r.session > 0 && r.session <= 65535 && r.offset > 0 &&
      r.offset <= r.image_size && text(r.version, sizeof(r.version));
}
inline bool nextResume(const Coord& prior, bool deep_sleep_reset, Coord& next) {
  if (!valid(prior) || !prior.pending || prior.resumes >= 2 || deep_sleep_reset)
    return false;
  next = prior; ++next.resumes; seal(next);
  // The caller may publish this candidate only after its durable commit.
  return true;
}
inline bool hexDigest(const char* text, uint8_t (&digest)[32]) {
  if (!text || strlen(text) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    unsigned pair = 0;
    for (size_t k = 0; k < 2; ++k) {
      const char c = text[i * 2 + k];
      if (c >= '0' && c <= '9') pair = pair * 16 + unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') pair = pair * 16 + unsigned(c - 'a' + 10);
      else return false;
    }
    digest[i] = static_cast<uint8_t>(pair);
  }
  return true;
}
}  // namespace LcdNvsRecords
#endif

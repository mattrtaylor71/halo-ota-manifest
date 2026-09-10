#pragma once
#include <stdint.h>
#include <string.h>

// One short recovery opportunity for one unresolved self-update. RTC retention
// is deliberate: no flash write is needed after an allocation/write failure.
// Existing durable coordinator debt survives independently; this grants no credit.
enum class SelfOtaRetryPhase : uint8_t { EMPTY, RESERVED, ARMED, CONSUMED, CLOSED };
struct SelfOtaRetry {
  uint32_t magic, reserved_epoch, due_epoch, expires_epoch, expected_bytes;
  SelfOtaRetryPhase phase;
  char origin[64], version[32], sha256[65], arm_id[64];
};
static const uint32_t SELF_OTA_RETRY_MAGIC = 0x53525431UL;
static const uint32_t SELF_OTA_RETRY_DELAY_S = 300;
static const uint32_t SELF_OTA_RETRY_LIFETIME_S = 1800;
static const uint32_t SELF_OTA_RETRY_LCD_LEAD_S = 15;

static bool self_retry_text(const char* text, size_t capacity) {
  if (!text || !text[0] || strnlen(text,capacity)>=capacity) return false;
  for (const char* p=text; *p; ++p) if ((uint8_t)*p < 32 || (uint8_t)*p > 126) return false;
  return true;
}
static bool self_retry_shape(const SelfOtaRetry& s) {
  if (s.magic != SELF_OTA_RETRY_MAGIC || s.phase < SelfOtaRetryPhase::RESERVED ||
      s.phase > SelfOtaRetryPhase::CLOSED || !self_retry_text(s.origin,sizeof(s.origin)) ||
      !self_retry_text(s.version,sizeof(s.version)) || !self_retry_text(s.sha256,sizeof(s.sha256)) ||
      strlen(s.sha256)!=64 || s.reserved_epoch < 1700000000UL ||
      uint64_t(s.reserved_epoch)+SELF_OTA_RETRY_DELAY_S != s.due_epoch ||
      uint64_t(s.reserved_epoch)+SELF_OTA_RETRY_LIFETIME_S != s.expires_epoch) return false;
  for (const char* p=s.sha256; *p; ++p)
    if (!(*p>='0'&&*p<='9') && !(*p>='a'&&*p<='f') && !(*p>='A'&&*p<='F')) return false;
  return s.phase != SelfOtaRetryPhase::ARMED || self_retry_text(s.arm_id,sizeof(s.arm_id));
}
static bool self_retry_identity(const SelfOtaRetry& s,const char* origin,const char* version) {
  return self_retry_shape(s) && origin && version && !strcmp(s.origin,origin) && !strcmp(s.version,version);
}
static bool self_retry_time(const SelfOtaRetry& s,uint32_t now,bool fresh) {
  return self_retry_shape(s) && fresh && now>=s.reserved_epoch && now<s.expires_epoch;
}
static bool self_retry_reserve(SelfOtaRetry& s,const char* origin,const char* version,
                               const char* sha,uint32_t bytes,uint32_t now,bool fresh) {
  if (!fresh || now<1700000000UL || uint64_t(now)+SELF_OTA_RETRY_LIFETIME_S>UINT32_MAX ||
      !self_retry_text(origin,64) || !self_retry_text(version,32) ||
      !self_retry_text(sha,65) || strlen(sha)!=64) return false;
  // An exhausted, missed or unverified reservation cannot be renewed by another
  // failure/user wake for this same origin+version (even with changed content).
  if (self_retry_identity(s,origin,version)) return false;
  SelfOtaRetry next={};
  next.magic=SELF_OTA_RETRY_MAGIC;next.phase=SelfOtaRetryPhase::RESERVED;
  next.reserved_epoch=now;next.due_epoch=now+SELF_OTA_RETRY_DELAY_S;
  next.expires_epoch=now+SELF_OTA_RETRY_LIFETIME_S;next.expected_bytes=bytes;
  memcpy(next.origin,origin,strlen(origin)+1);memcpy(next.version,version,strlen(version)+1);
  memcpy(next.sha256,sha,65);
  if (!self_retry_shape(next)) return false;
  s=next;return true;
}
static uint32_t self_retry_delta(const SelfOtaRetry& s,const char* origin,const char* version,
                                 uint32_t now,bool fresh) {
  if (!self_retry_identity(s,origin,version) || s.phase!=SelfOtaRetryPhase::ARMED ||
      !self_retry_time(s,now,fresh) || now>=s.due_epoch) return 0;
  return s.due_epoch-now;
}
static bool self_retry_consume(SelfOtaRetry& s,const char* origin,const char* version,
                                uint32_t now,bool fresh) {
  if (!self_retry_identity(s,origin,version) || s.phase!=SelfOtaRetryPhase::ARMED ||
      !self_retry_time(s,now,fresh) || uint64_t(now)+SELF_OTA_RETRY_LCD_LEAD_S<s.due_epoch) return false;
  s.phase=SelfOtaRetryPhase::CONSUMED;return true;
}
static uint32_t self_retry_work_ms(const SelfOtaRetry& s,const char* origin,const char* version,
                                   uint32_t now,bool fresh,uint32_t original_budget_ms) {
  if (!self_retry_identity(s,origin,version) || s.phase!=SelfOtaRetryPhase::CONSUMED ||
      !self_retry_time(s,now,fresh)) return 0;
  uint64_t remaining=uint64_t(s.expires_epoch-now)*1000;
  return remaining<original_budget_ms ? (uint32_t)remaining : original_budget_ms;
}

#pragma once
#include <mbedtls/sha256.h>

static bool sense_image_hash(const uint8_t* data, size_t len, char out[65]) {
  if (!data || !len) return false;
  uint8_t digest[32];
  if (mbedtls_sha256(data, len, digest, 0) != 0) return false;
  for (size_t i = 0; i < sizeof(digest); ++i) snprintf(out + 2*i, 3, "%02x", digest[i]);
  out[64] = 0; return true;
}
static bool sense_image_envelope_valid(const UploadJob& job) {
  if (job.is_voice || !job.image_len || job.image_len > 512U*1024U ||
      !job.image.owner_id[0] || !job.image.device_id[0] ||
      !memchr(job.image.owner_id, 0, sizeof(job.image.owner_id)) ||
      !memchr(job.image.device_id, 0, sizeof(job.image.device_id)) ||
      !sense_voice_request_id_valid(job.image.request_id) ||
      strnlen(job.image.checksum_sha256, 65) != 64) return false;
  for (unsigned i = 0; i < 64; ++i) {
    char c=job.image.checksum_sha256[i];
    if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
  }
  return true;
}
static bool sense_image_owner_matches(const UploadJob& job) {
  char owner[64]={}, device[32]={};
  load_owner_id_or_default(owner,sizeof(owner)); load_runtime_device_id(device,sizeof(device));
  return sense_image_envelope_valid(job) && !strcmp(owner,job.image.owner_id) &&
      !strcmp(device[0]?device:TREPO_DEVICE_ID,job.image.device_id);
}
static bool sense_image_payload_matches(const UploadJob& job) {
  char sha[65];
  return sense_image_owner_matches(job) && job.image_buf &&
      sense_voice_crc32(job.image_buf,job.image_len)==job.image.crc32 &&
      sense_image_hash(job.image_buf,job.image_len,sha) && !strcmp(sha,job.image.checksum_sha256);
}
static bool sense_image_freeze_envelope(UploadJob& job) {
  load_owner_id_or_default(job.image.owner_id,sizeof(job.image.owner_id));
  load_runtime_device_id(job.image.device_id,sizeof(job.image.device_id));
  if (!job.image.device_id[0]) snprintf(job.image.device_id,sizeof(job.image.device_id),"%s",TREPO_DEVICE_ID);
  snprintf(job.image.request_id,sizeof(job.image.request_id),"%08lx%08lx%08lx%08lx",
      (unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());
  job.image.crc32=sense_voice_crc32(job.image_buf,job.image_len);
  const time_t now=time(nullptr);
  if (sense_time_has_fresh_sync() && now>=(time_t)TIME_VALID_MIN_EPOCH) job.created_epoch=(uint32_t)now;
  return sense_image_hash(job.image_buf,job.image_len,job.image.checksum_sha256) && sense_image_envelope_valid(job);
}

/*
 * sense_upload_persist.h
 *
 * Upload persistence: SPIFFS-backed retry storage for failed uploads.
 * Saves one upload job (image + metadata) to flash for replay on next boot.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 18.
 *
 * Entire file is guarded by HALO_SENSE_PROD_WRAPPER && HALO_SENSE_UPLOAD_PERSISTENCE.
 *
 * Prerequisites (must be declared before #include "sense_upload_persist.h"):
 *   - FS.h, SPIFFS.h
 *   - UploadJob struct from sense_ops.h
 *   - diag_sanitize_token(), diag_record_action_event() from sense_diag.h
 *   - wifi_is_connected() from sense_wifi.h
 *   - TIME_VALID_MIN_EPOCH constant
 *   - upload_inflight, dish_scan_inflight, scan_ui_inflight globals
 *   - g_boot_reset_reason, reset_reason_label() from sense_diag.h
 *   - halo_provisioning_active() (under HALO_SENSE_PROD_WRAPPER)
 *   - dump_system_truth() (under HALO_SENSE_PROD_WRAPPER)
 */

#ifndef SENSE_UPLOAD_PERSIST_H
#define SENSE_UPLOAD_PERSIST_H

#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)

// Forward declarations for sense_upload_queue.h (late include)
static uint8_t* allocate_upload_buffer(size_t len, bool* used_psram);
static uint32_t upload_queue_count();
static bool queue_voice_upload_job(uint32_t job_id,
                                   uint8_t* audio_buf,
                                   size_t audio_len,
                                   uint8_t retries,
                                   bool from_persisted,
                                   uint32_t created_epoch,
                                   const UploadJob::VoiceEnvelope* voice,
                                   bool from_voice_sd);

// ── Structs ──────────────────────────────────────────────────────────

struct PersistedUploadMetaV1 {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t job_id;
  uint32_t image_len;
  uint32_t created_epoch;
  uint16_t quantity;
  uint8_t retries;
  uint8_t reserved;
  char mode[16];
  char expiry_date[16];
};

struct PersistedUploadMetaV2 {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t job_id;
  uint32_t image_len;
  uint32_t created_epoch;
  uint16_t quantity;
  uint8_t retries;
  uint8_t reserved;
  char mode[16];
  char expiry_date[16];
  UploadJob::CameraUploadMeta camera_meta;
};

struct PersistedUploadMeta : PersistedUploadMetaV2 {
  UploadJob::VoiceEnvelope voice;
  uint32_t checksum;
};

static uint32_t upload_persist_meta_checksum(const PersistedUploadMeta& meta) {
  // This derived type is not standard-layout: do not use offsetof on it.
  // Cover the complete stored prefix, including identity, flags, age and PCM
  // checksum. The writer zeroes the record before filling any fields.
  const uint8_t* begin = reinterpret_cast<const uint8_t*>(&meta);
  const uint8_t* end = reinterpret_cast<const uint8_t*>(&meta.checksum);
  return sense_voice_crc32(begin, static_cast<size_t>(end - begin));
}

// ── Constants ────────────────────────────────────────────────────────

static const uint32_t UPLOAD_PERSIST_MAGIC = 0x48555031UL;  // HUP1
static const uint16_t UPLOAD_PERSIST_VERSION = 3;
static const char* UPLOAD_PERSIST_META_PATH = "/upload_retry.meta";
static const char* UPLOAD_PERSIST_IMAGE_PATH = "/upload_retry.bin";
static const char* UPLOAD_PERSIST_VOICE_ATTEMPT_PATH = "/voice_retry.attempt";
static const char* UPLOAD_PERSIST_VOICE_ATTEMPT_TMP = "/voice_retry.attempt.part";
struct PersistedVoiceAttempt {
  char request_id[33];
  uint32_t epoch;
  uint32_t checksum;
};
static const uint8_t UPLOAD_PERSIST_MAX_RETRIES = 5;
static const uint32_t UPLOAD_PERSIST_MAX_AGE_S = 86400UL;
static const unsigned long UPLOAD_PERSIST_CHECK_INTERVAL_MS = 2000;
static const unsigned long UPLOAD_PERSIST_VOICE_REPLAY_BACKOFF_MS = 60000UL;
static const size_t UPLOAD_PERSIST_FREE_RESERVE_BYTES = 64 * 1024;
static const size_t UPLOAD_PERSIST_MAX_IMAGE_BYTES = 1024 * 1024;

// ── Globals ──────────────────────────────────────────────────────────

static bool g_upload_persist_ready = false;
static bool g_upload_persist_attempted_this_boot = false;
static unsigned long g_upload_persist_last_check_ms = 0;
static unsigned long g_upload_persist_replay_not_before_ms = 0;
static uint8_t g_upload_persist_cached_count = 0;
static char g_upload_persist_last_result[24] = "none";
static char g_upload_persist_last_reason[32] = "";
static unsigned long g_upload_persist_last_event_ms = 0;
static uint8_t g_upload_persist_last_retries = 0;
static StaticSemaphore_t g_upload_persist_mutex_storage;
static SemaphoreHandle_t g_upload_persist_mutex = nullptr;

// Worker fallback and main-loop sleep rescue can meet after the flush budget.
// Serialize the complete check/read/write/delete transaction, including nested
// helper reads. No file is opened while merely waiting for this bounded lease.
class UploadPersistLease {
 public:
  UploadPersistLease() : held_(g_upload_persist_mutex &&
      xSemaphoreTakeRecursive(g_upload_persist_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {}
  ~UploadPersistLease() { if (held_) xSemaphoreGiveRecursive(g_upload_persist_mutex); }
  bool held() const { return held_; }
 private: bool held_;
};

static const uint8_t UPLOAD_PERSIST_FLAG_ADD_TO_SHOPPING = 1u << 0;
static const uint8_t UPLOAD_PERSIST_FLAG_IS_VOICE = 1u << 1;

// ── Event tracking ───────────────────────────────────────────────────

static void upload_persist_note_event(const char* result,
                                      const char* reason,
                                      uint8_t cached_count,
                                      uint8_t retries) {
  diag_sanitize_token(g_upload_persist_last_result, sizeof(g_upload_persist_last_result), result);
  diag_sanitize_token(g_upload_persist_last_reason, sizeof(g_upload_persist_last_reason), reason);
  g_upload_persist_cached_count = cached_count;
  g_upload_persist_last_retries = retries;
  g_upload_persist_last_event_ms = millis();
  diag_record_action_event("upload_cache",
                           "",
                           g_upload_persist_last_result,
                           g_upload_persist_last_reason,
                           cached_count);
}

// ── Public diagnostics API (non-static, used by prod wrapper) ────────

uint32_t sense_get_upload_cache_count() {
  return g_upload_persist_cached_count;
}

const char* sense_get_upload_cache_last_result() {
  return g_upload_persist_last_result;
}

const char* sense_get_upload_cache_last_reason() {
  return g_upload_persist_last_reason;
}

int32_t sense_get_upload_cache_last_age_ms() {
  if (g_upload_persist_last_event_ms == 0) {
    return -1;
  }
  return static_cast<int32_t>(millis() - g_upload_persist_last_event_ms);
}

uint32_t sense_get_upload_cache_last_retries() {
  return g_upload_persist_last_retries;
}

// ── File helpers ─────────────────────────────────────────────────────

static size_t upload_persist_file_size(const char* path) {
  if (!g_upload_persist_ready || !path || !SPIFFS.exists(path)) {
    return 0;
  }
  File file = SPIFFS.open(path, "r");
  if (!file) {
    return 0;
  }
  size_t size = file.size();
  file.close();
  return size;
}

static bool upload_persist_has_pending() {
  bool pending = g_upload_persist_ready &&
         SPIFFS.exists(UPLOAD_PERSIST_META_PATH) &&
         SPIFFS.exists(UPLOAD_PERSIST_IMAGE_PATH);
  g_upload_persist_cached_count = pending ? 1 : 0;
  if (g_upload_persist_ready) media_retry_inventory(halo_media_retry::VoiceFlash, pending);
  return pending;
}

static bool upload_persist_delete() {
  UploadPersistLease lease;
  if (!lease.held()) return false;
  if (!g_upload_persist_ready) {
    return false;
  }
  bool removed = false;
  if (SPIFFS.exists(UPLOAD_PERSIST_META_PATH)) {
    removed = SPIFFS.remove(UPLOAD_PERSIST_META_PATH) || removed;
  }
  if (SPIFFS.exists(UPLOAD_PERSIST_IMAGE_PATH)) {
    removed = SPIFFS.remove(UPLOAD_PERSIST_IMAGE_PATH) || removed;
  }
  g_upload_persist_cached_count = upload_persist_has_pending() ? 1 : 0;
  return removed;
}

// ── Meta read/write ──────────────────────────────────────────────────

static bool upload_persist_read_meta(PersistedUploadMeta* meta) {
  UploadPersistLease lease;
  if (!lease.held()) return false;
  if (!meta || !upload_persist_has_pending()) {
    return false;
  }
  memset(meta, 0, sizeof(*meta));
  File file = SPIFFS.open(UPLOAD_PERSIST_META_PATH, "r");
  if (!file) {
    return false;
  }
  size_t file_size = file.size();
  bool ok = false;
  if (file_size == sizeof(PersistedUploadMeta)) {
    ok = (file.read((uint8_t*)meta, sizeof(*meta)) == (int)sizeof(*meta));
  } else if (file_size == sizeof(PersistedUploadMetaV2)) {
    PersistedUploadMetaV2 legacy = {};
    ok = file.read((uint8_t*)&legacy, sizeof(legacy)) == (int)sizeof(legacy);
    if (ok) static_cast<PersistedUploadMetaV2&>(*meta) = legacy;
  } else if (file_size == sizeof(PersistedUploadMetaV1)) {
    PersistedUploadMetaV1 legacy = {};
    ok = (file.read((uint8_t*)&legacy, sizeof(legacy)) == (int)sizeof(legacy));
    if (ok) {
      meta->magic = legacy.magic;
      meta->version = legacy.version;
      meta->header_size = legacy.header_size;
      meta->job_id = legacy.job_id;
      meta->image_len = legacy.image_len;
      meta->created_epoch = legacy.created_epoch;
      meta->quantity = legacy.quantity;
      meta->retries = legacy.retries;
      meta->reserved = legacy.reserved;
      strncpy(meta->mode, legacy.mode, sizeof(meta->mode) - 1);
      strncpy(meta->expiry_date, legacy.expiry_date, sizeof(meta->expiry_date) - 1);
    }
  }
  file.close();
  if (!ok) {
    return false;
  }
  bool valid_v3 = (file_size == sizeof(*meta) && meta->version == UPLOAD_PERSIST_VERSION &&
                   meta->header_size == sizeof(*meta));
  bool valid_v2 = file_size == sizeof(PersistedUploadMetaV2) &&
                  meta->version == 2 && meta->header_size == sizeof(PersistedUploadMetaV2);
  bool valid_v1 = (file_size == sizeof(PersistedUploadMetaV1) && meta->version == 1 &&
                   meta->header_size == sizeof(PersistedUploadMetaV1));
  if (meta->magic != UPLOAD_PERSIST_MAGIC ||
      (!valid_v3 && !valid_v2 && !valid_v1) ||
      (valid_v3 && meta->checksum != upload_persist_meta_checksum(*meta)) ||
      meta->image_len == 0 ||
      meta->image_len > UPLOAD_PERSIST_MAX_IMAGE_BYTES) {
    return false;
  }
  meta->mode[sizeof(meta->mode) - 1] = '\0';
  meta->expiry_date[sizeof(meta->expiry_date) - 1] = '\0';
  if (valid_v3 && (meta->reserved & UPLOAD_PERSIST_FLAG_IS_VOICE) && meta->created_epoch == 0 &&
      SPIFFS.exists(UPLOAD_PERSIST_VOICE_ATTEMPT_PATH)) {
    PersistedVoiceAttempt attempt = {};
    File marker = SPIFFS.open(UPLOAD_PERSIST_VOICE_ATTEMPT_PATH, "r");
    if (!marker) return false;
    const bool read = marker.size() == sizeof(attempt) && marker.read((uint8_t*)&attempt, sizeof(attempt)) == (int)sizeof(attempt);
    marker.close();
    if (!read || memcmp(attempt.request_id, meta->voice.request_id, sizeof(attempt.request_id)) ||
        attempt.epoch < TIME_VALID_MIN_EPOCH ||
        attempt.checksum != sense_voice_crc32((const uint8_t*)&attempt, offsetof(PersistedVoiceAttempt, checksum))) return false;
    meta->created_epoch = attempt.epoch;
  }
  return true;
}

static bool upload_persist_is_stale(const PersistedUploadMeta& meta) {
  if (meta.created_epoch < TIME_VALID_MIN_EPOCH) {
    return false;
  }
  time_t now = time(nullptr);
  if (now < (time_t)TIME_VALID_MIN_EPOCH) {
    return false;
  }
  return (uint32_t)(now - (time_t)meta.created_epoch) > UPLOAD_PERSIST_MAX_AGE_S;
}

static bool upload_persist_write_blob(const char* path, const uint8_t* data, size_t len) {
  File file = SPIFFS.open(path, "w");
  if (!file) {
    return false;
  }
  size_t written = file.write(data, len);
  file.close();
  return written == len;
}

static bool upload_persist_voice_matches(const UploadJob& job, const PersistedUploadMeta& meta) {
  if (!sense_voice_envelope_valid(job) || meta.version != 3 ||
      meta.image_len != job.image_len || meta.created_epoch != job.created_epoch ||
      !(meta.reserved & UPLOAD_PERSIST_FLAG_IS_VOICE) ||
      memcmp(&meta.voice, &job.voice, sizeof(job.voice))) return false;
  File file = SPIFFS.open(UPLOAD_PERSIST_IMAGE_PATH, "r");
  if (!file) return false;
  bool ok = file.size() == job.image_len;
  uint32_t crc = 0xffffffffU;
  uint8_t chunk[256]; size_t read = 0;
  while (ok && read < job.image_len) {
    const size_t n = job.image_len - read < sizeof(chunk) ? job.image_len - read : sizeof(chunk);
    if (file.read(chunk, n) != (int)n) { ok = false; break; }
    for (size_t i = 0; i < n; ++i) {
      crc ^= chunk[i];
      for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    read += n;
  }
  file.close();
  return ok && ~crc == job.voice.crc32;
}

static bool upload_persist_delete_voice(const UploadJob& job) {
  UploadPersistLease lease;
  if (!lease.held()) return false;
  PersistedUploadMeta pending = {};
  if (!upload_persist_read_meta(&pending) || !upload_persist_voice_matches(job, pending)) return false;
  const bool removed = upload_persist_delete();
  // Commit record first: losing power during cleanup must not resurrect an
  // already-attempted recording as epoch-zero/never-attempted.
  if (!SPIFFS.exists(UPLOAD_PERSIST_META_PATH) && !SPIFFS.exists(UPLOAD_PERSIST_IMAGE_PATH)) {
    SPIFFS.remove(UPLOAD_PERSIST_VOICE_ATTEMPT_PATH);
    SPIFFS.remove(UPLOAD_PERSIST_VOICE_ATTEMPT_TMP);
  }
  return removed;
}

static bool upload_persist_start_voice_attempt(UploadJob& job, uint32_t epoch) {
  UploadPersistLease lease;
  if (!lease.held()) return false;
  PersistedUploadMeta pending = {};
  if (!upload_persist_read_meta(&pending)) return false;
  UploadJob check = job; check.created_epoch = pending.created_epoch;
  if (!upload_persist_voice_matches(check, pending)) return false;
  if (pending.created_epoch) { job.created_epoch = pending.created_epoch; return sense_voice_replay_age_ok(job); }
  if (epoch < TIME_VALID_MIN_EPOCH || SPIFFS.exists(UPLOAD_PERSIST_VOICE_ATTEMPT_PATH) ||
      SPIFFS.exists(UPLOAD_PERSIST_VOICE_ATTEMPT_TMP)) return false;
  PersistedVoiceAttempt attempt = {};
  memcpy(attempt.request_id, job.voice.request_id, sizeof(attempt.request_id));
  attempt.epoch = epoch;
  attempt.checksum = sense_voice_crc32((const uint8_t*)&attempt, offsetof(PersistedVoiceAttempt, checksum));
  if (!upload_persist_write_blob(UPLOAD_PERSIST_VOICE_ATTEMPT_TMP, (const uint8_t*)&attempt, sizeof(attempt)) ||
      !SPIFFS.rename(UPLOAD_PERSIST_VOICE_ATTEMPT_TMP, UPLOAD_PERSIST_VOICE_ATTEMPT_PATH) ||
      !upload_persist_read_meta(&pending) || pending.created_epoch != epoch) return false;
  job.created_epoch = epoch;
  return upload_persist_voice_matches(job, pending);
}

// ── Save / Load ──────────────────────────────────────────────────────

static bool upload_persist_save(const UploadJob& job, uint8_t next_retries) {
  UploadPersistLease lease;
  if (!lease.held()) return false;
  if (!g_upload_persist_ready || !job.image_buf || job.image_len == 0) {
    return false;
  }
  PersistedUploadMeta pending = {};
  if (upload_persist_has_pending()) {
    const bool read = upload_persist_read_meta(&pending);
    const bool prior_voice = !read || (pending.reserved & UPLOAD_PERSIST_FLAG_IS_VOICE) || !strcmp(pending.mode, "voice");
    if (job.is_voice || prior_voice) {
      // Never replace another user's recording or a legacy payload. A failed
      // replay already has a durable copy; do not rewrite it during low space.
      return read && job.is_voice && upload_persist_voice_matches(job, pending);
    }
  }
  if (job.is_voice && (!sense_voice_envelope_valid(job) ||
      SPIFFS.exists(UPLOAD_PERSIST_META_PATH) || SPIFFS.exists(UPLOAD_PERSIST_IMAGE_PATH) ||
      SPIFFS.exists(UPLOAD_PERSIST_VOICE_ATTEMPT_PATH) || SPIFFS.exists(UPLOAD_PERSIST_VOICE_ATTEMPT_TMP))) return false;

  PersistedUploadMeta meta;
  memset(&meta, 0, sizeof(meta));
  meta.magic = UPLOAD_PERSIST_MAGIC;
  meta.version = UPLOAD_PERSIST_VERSION;
  meta.header_size = sizeof(meta);
  meta.job_id = job.job_id;
  meta.image_len = (uint32_t)job.image_len;
  meta.created_epoch = job.created_epoch;
  if (!job.is_voice && meta.created_epoch < TIME_VALID_MIN_EPOCH) {
    time_t now = time(nullptr);
    if (now >= (time_t)TIME_VALID_MIN_EPOCH) {
      meta.created_epoch = (uint32_t)now;
    } else {
      meta.created_epoch = 0;
    }
  }
  meta.quantity = (job.quantity < 1) ? 1 : job.quantity;
  meta.retries = next_retries;
  meta.reserved = 0;
  if (job.add_to_shopping_list) {
    meta.reserved |= UPLOAD_PERSIST_FLAG_ADD_TO_SHOPPING;
  }
  if (job.is_voice) {
    meta.reserved |= UPLOAD_PERSIST_FLAG_IS_VOICE;
  }
  strncpy(meta.mode, job.mode, sizeof(meta.mode) - 1);
  strncpy(meta.expiry_date, job.expiry_date, sizeof(meta.expiry_date) - 1);
  meta.camera_meta = job.camera_meta;
  meta.voice = job.voice;
  meta.checksum = upload_persist_meta_checksum(meta);

  size_t existing_bytes = upload_persist_file_size(UPLOAD_PERSIST_META_PATH) +
                          upload_persist_file_size(UPLOAD_PERSIST_IMAGE_PATH);
  size_t available_bytes = 0;
  if (SPIFFS.totalBytes() > SPIFFS.usedBytes()) {
    available_bytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
  }
  available_bytes += existing_bytes;
  size_t required_bytes = sizeof(meta) + job.image_len + UPLOAD_PERSIST_FREE_RESERVE_BYTES;
  if (job.image_len > UPLOAD_PERSIST_MAX_IMAGE_BYTES || available_bytes < required_bytes) {
    Serial.printf("[UPLOAD_PERSIST] skip_save len=%u avail=%u required=%u\n",
                  (unsigned)job.image_len,
                  (unsigned)available_bytes,
                  (unsigned)required_bytes);
    return false;
  }

  if (!upload_persist_write_blob(UPLOAD_PERSIST_IMAGE_PATH, job.image_buf, job.image_len)) {
    Serial.println("[UPLOAD_PERSIST] image_write_failed");
    if (!job.is_voice) upload_persist_delete();
    return false;
  }
  if (!upload_persist_write_blob(UPLOAD_PERSIST_META_PATH, (const uint8_t*)&meta, sizeof(meta))) {
    Serial.println("[UPLOAD_PERSIST] meta_write_failed");
    if (!job.is_voice) upload_persist_delete();
    return false;
  }

  Serial.printf("[UPLOAD_PERSIST] saved job_id=%lu len=%u retries=%u mode=%s\n",
                (unsigned long)meta.job_id,
                (unsigned)meta.image_len,
                (unsigned)meta.retries,
                meta.mode);
  if (job.is_voice) {
    PersistedUploadMeta verified = {};
    const bool saved = upload_persist_read_meta(&verified) && upload_persist_voice_matches(job, verified);
    if (saved) media_retry_saved(halo_media_retry::VoiceFlash);
    return saved;
  }
  return true;
}

static bool upload_persist_load(UploadJob* job) {
  UploadPersistLease lease;
  if (!lease.held()) return false;
  if (!job) {
    return false;
  }
  PersistedUploadMeta meta = {};
  if (!upload_persist_read_meta(&meta)) {
    Serial.println("[UPLOAD_PERSIST] invalid_meta retained for recovery");
    return false;
  }
  const bool voice = (meta.reserved & UPLOAD_PERSIST_FLAG_IS_VOICE) || !strcmp(meta.mode, "voice");
  if (voice && (meta.version != 3 || meta.retries >= UPLOAD_PERSIST_MAX_RETRIES)) {
    Serial.println("[UPLOAD_PERSIST] voice_retained legacy_or_expired; not replayed");
    return false;
  }
  if (!voice) {
    // Legacy photos lack a frozen request/owner and cannot safely join the new
    // idempotent upload contract. Hold their bytes for explicit recovery.
    Serial.println("[UPLOAD_PERSIST] legacy_photo_retained; identity_unavailable");
    return false;
  }

  File file = SPIFFS.open(UPLOAD_PERSIST_IMAGE_PATH, "r");
  if (!file) {
    Serial.println("[UPLOAD_PERSIST] image_open_failed");
    if (!voice) upload_persist_delete();
    return false;
  }
  if (file.size() != meta.image_len) {
    Serial.printf("[UPLOAD_PERSIST] image_size_mismatch file=%u meta=%u\n",
                  (unsigned)file.size(),
                  (unsigned)meta.image_len);
    file.close();
    if (!voice) upload_persist_delete();
    return false;
  }

  bool used_psram = false;
  uint8_t* image_buf = allocate_upload_buffer(meta.image_len, &used_psram);
  if (!image_buf) {
    file.close();
    Serial.printf("[UPLOAD_PERSIST] alloc_failed len=%u\n", (unsigned)meta.image_len);
    return false;
  }
  bool ok = (file.read(image_buf, meta.image_len) == (int)meta.image_len);
  file.close();
  if (!ok) {
    free(image_buf);
    Serial.println("[UPLOAD_PERSIST] image_read_failed");
    if (!voice) upload_persist_delete();
    return false;
  }

  UploadJob loaded = {};
  loaded.job_id = meta.job_id;
  loaded.quantity = (meta.quantity < 1) ? 1 : meta.quantity;
  loaded.add_to_shopping_list = (meta.reserved & UPLOAD_PERSIST_FLAG_ADD_TO_SHOPPING) != 0;
  loaded.is_voice = (meta.reserved & UPLOAD_PERSIST_FLAG_IS_VOICE) != 0 ||
                    strcmp(meta.mode, "voice") == 0;
  loaded.image_buf = image_buf;
  loaded.image_len = meta.image_len;
  loaded.retries = meta.retries;
  loaded.created_ms = millis();
  loaded.created_epoch = meta.created_epoch;
  loaded.from_persisted = true;
  loaded.camera_meta = meta.camera_meta;
  loaded.voice = meta.voice;
  strncpy(loaded.mode, meta.mode, sizeof(loaded.mode) - 1);
  strncpy(loaded.expiry_date, meta.expiry_date, sizeof(loaded.expiry_date) - 1);
  if (loaded.is_voice && (!sense_voice_owner_matches(loaded) ||
      (loaded.created_epoch != 0 && !sense_voice_replay_age_ok(loaded)) ||
      sense_voice_crc32(image_buf, loaded.image_len) != loaded.voice.crc32)) {
    free(image_buf);
    Serial.println("[UPLOAD_PERSIST] voice_retained identity_age_or_crc");
    return false;
  }
  *job = loaded;

  Serial.printf("[UPLOAD_PERSIST] loaded job_id=%lu len=%u retries=%u psram=%d mode=%s\n",
                (unsigned long)loaded.job_id,
                (unsigned)loaded.image_len,
                (unsigned)loaded.retries,
                used_psram ? 1 : 0,
                loaded.mode);
  return true;
}

// ── Setup ────────────────────────────────────────────────────────────

static void upload_persist_setup() {
  if (!g_upload_persist_mutex)
    g_upload_persist_mutex = xSemaphoreCreateRecursiveMutexStatic(&g_upload_persist_mutex_storage);
  if (!g_upload_persist_mutex) return;
  if (g_upload_persist_ready) {
    return;
  }
  if (!SPIFFS.begin(false)) {
    // Mount failure is not permission to erase queued user recordings.
    Serial.println("[UPLOAD_PERSIST] mount_failed retained; internal fallback disabled");
    return;
  }
  g_upload_persist_ready = true;
  g_upload_persist_cached_count = upload_persist_has_pending() ? 1 : 0;
  Serial.printf("[UPLOAD_PERSIST] mounted total=%u used=%u pending=%d\n",
                (unsigned)SPIFFS.totalBytes(),
                (unsigned)SPIFFS.usedBytes(),
                g_upload_persist_cached_count ? 1 : 0);
}

// ── Replay on boot ───────────────────────────────────────────────────

static void upload_persist_maybe_replay() {
  if (!g_upload_persist_ready || g_upload_persist_attempted_this_boot) {
    return;
  }
  if (!upload_persist_has_pending()) {
    return;
  }
  if (g_media_retry_user_paused.load() || !wifi_is_connected() || !sense_time_has_fresh_sync() || upload_inflight || upload_queue_count() > 0 ||
      foreground_active || current_job.active || voice_recording_active ||
      dish_scan_inflight || scan_ui_inflight || !sense_uart_ordinary_tx_allowed()) {
    return;
  }
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active() || halo_prod_boot_ota_pending()) {
    return;
  }
#endif
  unsigned long now_ms = millis();
  if (g_upload_persist_replay_not_before_ms > 0 &&
      now_ms < g_upload_persist_replay_not_before_ms) {
    static unsigned long last_defer_log_ms = 0;
    if (last_defer_log_ms == 0 || (now_ms - last_defer_log_ms) >= 5000UL) {
      Serial.printf("[UPLOAD_PERSIST] replay_deferred remaining_ms=%lu reset_reason=%s\n",
                    (unsigned long)(g_upload_persist_replay_not_before_ms - now_ms),
                    reset_reason_label(g_boot_reset_reason));
      last_defer_log_ms = now_ms;
    }
    return;
  }
  if ((now_ms - g_upload_persist_last_check_ms) < UPLOAD_PERSIST_CHECK_INTERVAL_MS) {
    return;
  }
  g_upload_persist_last_check_ms = now_ms;

  UploadJob job = {};
  if (!upload_persist_load(&job)) {
    g_upload_persist_attempted_this_boot = true;
    return;
  }
  bool queued = job.is_voice && queue_voice_upload_job(job.job_id, job.image_buf,
      job.image_len, job.retries, true, job.created_epoch, &job.voice, false);

  if (!queued) {
    Serial.println("[UPLOAD_PERSIST] replay_queue_failed");
    free(job.image_buf);
    return;
  }

  g_upload_persist_attempted_this_boot = true;
  upload_persist_note_event("retry_queued", job.mode, 1, job.retries);
  dump_system_truth("upload_retry_queued");
  Serial.printf("[UPLOAD_PERSIST] replay_queued job_id=%lu retries=%u\n",
                (unsigned long)job.job_id,
                (unsigned)job.retries);
}
#endif // HALO_SENSE_PROD_WRAPPER && HALO_SENSE_UPLOAD_PERSISTENCE

// ── Failure handler (outside #if guard — has internal guard) ─────────

static bool sense_voice_prepare_first_attempt(UploadJob& job) {
  if (!sense_voice_owner_matches(job)) return false;
  if (job.created_epoch != 0) return sense_voice_replay_age_ok(job);
  const time_t now = time(nullptr);
  if (!sense_time_has_fresh_sync() || now < (time_t)TIME_VALID_MIN_EPOCH) return false;
  if (job.from_voice_sd) return sense_voice_spool_mark_attempt(job, (uint32_t)now);
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
  if (job.from_persisted) return upload_persist_start_voice_attempt(job, (uint32_t)now);
#endif
  // Fresh RAM-only job: no uncertain previous POST exists. Failure persistence
  // carries this timestamp unchanged; a durable epoch-zero replay marks first.
  job.created_epoch = (uint32_t)now;
  return true;
}

// The worker may release the UART between a busy refusal and retry. Ordinary
// sleep must still retain the RAM-owned capture; the store deadline bounds this
// lease and the existing guardian remains the final limit.
class UploadMediaCustodyLease {
 public:
  UploadMediaCustodyLease() { g_media_custody_waiters.fetch_add(1); }
  ~UploadMediaCustodyLease() { g_media_custody_waiters.fetch_sub(1); }
  UploadMediaCustodyLease(const UploadMediaCustodyLease&) = delete;
  UploadMediaCustodyLease& operator=(const UploadMediaCustodyLease&) = delete;
};

// Called only after a typed busy refusal returned and released the raw UART
// lease. Keep the frozen caller-owned job while servicing a gesture's END.
static void upload_persist_busy_wait() {
  if (g_sense_main_task_handle &&
      xTaskGetCurrentTaskHandle() == g_sense_main_task_handle &&
      uart_dispatch_depth.load() == 0) {
    pump_uart_rx_once();
  } else {
    uart_collect_rx_once();
  }
  delay(250);
}

static bool upload_persist_handle_failure(const UploadJob& job, const char* reason) {
  UploadMediaCustodyLease custody;
  if (job.is_voice) {
    bool saved = job.from_voice_sd;
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
    if (!saved && job.from_persisted) {
      UploadPersistLease lease;
      PersistedUploadMeta pending = {};
      saved = lease.held() && upload_persist_read_meta(&pending) && upload_persist_voice_matches(job, pending);
    }
#endif
    const uint32_t deadline = sense_voice_spool_operation_deadline();
    // Foreground contention is not a failed storage attempt. Keep this exact
    // RAM job while the LCD finishes an admitted gesture. Each store call has
    // released its raw UART lease before the delay, so START/END can progress.
    // The original transaction/guardian deadline is never renewed.
    bool busy_reported = false;
    unsigned attempt = 0;
    while (!saved && attempt < 2 && sense_voice_spool_remaining(deadline)) {
      bool busy_refused = false;
      saved = sense_voice_spool_store(job, deadline, &busy_refused);
      if (!saved && busy_refused) {
        if (!busy_reported) Serial.printf("[MEDIA_BACKUP] kind=voice event=storage_busy job=%lu retained_ram=1\n", (unsigned long)job.job_id);
        busy_reported = true;
        if (sense_voice_spool_remaining(deadline) <= 250) break;
        upload_persist_busy_wait();
        continue;
      }
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
      if (!saved && sense_voice_spool_remaining(deadline) > 1000)
        saved = upload_persist_save(job, job.retries < 255 ? job.retries + 1 : 255);
#endif
      if (!saved && attempt == 0 && sense_voice_spool_remaining(deadline) > 500) delay(500);
      ++attempt;
    }
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
    g_upload_persist_attempted_this_boot = true;
    g_upload_persist_replay_not_before_ms = millis() + UPLOAD_PERSIST_VOICE_REPLAY_BACKOFF_MS;
#endif
    // A failed internal-flash retry must not consume the separate SD turn.
    // New captures still wait until a later wake after being backed up.
    if (!job.from_persisted) g_voice_spool_replayed_this_boot = true;
    char payload_sha[65] = {};
    sense_image_hash(job.image_buf, job.image_len, payload_sha);
    Serial.printf("[MEDIA_BACKUP] kind=voice request=%s job=%lu bytes=%u crc=%08lx sha256=%s saved=%u from_sd=%u\n",
                  job.voice.request_id,(unsigned long)job.job_id,(unsigned)job.image_len,
                  (unsigned long)job.voice.crc32,payload_sha,saved?1:0,job.from_voice_sd?1:0);
    uart_send_sense_diag("voice", saved ? "saved_for_retry" : "storage_failed", "voice",
                         (int32_t)job.job_id, saved ? "durable_copy_retained" : "not_saved");
    if (!saved) uart_send_ui_status_extended("VOICE", "ERROR", "Voice not saved. Please try again.", nullptr, job.job_id);
    return saved;
  }
  // New photos use the checked multi-record SD queue. The old single SPIFFS
  // photo record cannot carry this frozen identity and is never overwritten.
  const uint32_t deadline = sense_image_spool_operation_deadline();
  bool saved = job.from_image_sd;
  unsigned attempt = 0;
  bool busy_reported = false;
  while (!saved && attempt < 2 && sense_image_spool_remaining(deadline)) {
    bool busy_refused = false;
    saved = sense_image_spool_store(job, deadline, &busy_refused);
    if (!saved && busy_refused) {
      if (!busy_reported) Serial.printf("[MEDIA_BACKUP] kind=image event=storage_busy job=%lu retained_ram=1\n", (unsigned long)job.job_id);
      busy_reported = true;
      if (sense_image_spool_remaining(deadline) <= 250) break;
      upload_persist_busy_wait();
      continue;
    }
    if (!saved && attempt == 0 && sense_image_spool_remaining(deadline) > 500) delay(500);
    ++attempt;
  }
  g_image_spool_replayed_this_boot = true;
  Serial.printf("[MEDIA_BACKUP] kind=image request=%s job=%lu bytes=%u crc=%08lx sha256=%s saved=%u from_sd=%u\n",
                job.image.request_id,(unsigned long)job.job_id,(unsigned)job.image_len,
                (unsigned long)job.image.crc32,job.image.checksum_sha256,saved?1:0,job.from_image_sd?1:0);
  uart_send_sense_diag("upload", saved ? "saved_for_retry" : "storage_failed", job.mode,
                      (int32_t)job.job_id, saved ? "durable_copy_retained" : "not_saved");
  if (!saved) uart_send_ui_status_extended("SCAN", "ERROR", "Photo not saved. Please try again.", job.mode, job.job_id);
  return saved;
}

#endif // SENSE_UPLOAD_PERSIST_H

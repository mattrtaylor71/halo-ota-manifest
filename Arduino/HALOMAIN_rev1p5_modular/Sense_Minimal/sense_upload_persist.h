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
 *   - upload_inflight, waiting_for_mqtt_result, dish_scan_inflight, scan_ui_inflight globals
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
static bool queue_upload_job(uint32_t job_id,
                             const char* mode,
                             const char* expiry,
                             uint16_t quantity,
                             bool add_to_shopping_list,
                             const UploadJob::CameraUploadMeta* camera_meta,
                             uint8_t* image_buf,
                             size_t image_len,
                             uint8_t retries,
                             bool from_persisted,
                             uint32_t created_epoch);
static bool queue_voice_upload_job(uint32_t job_id,
                                   uint8_t* audio_buf,
                                   size_t audio_len,
                                   uint8_t retries,
                                   bool from_persisted,
                                   uint32_t created_epoch);

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

struct PersistedUploadMeta {
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

// ── Constants ────────────────────────────────────────────────────────

static const uint32_t UPLOAD_PERSIST_MAGIC = 0x48555031UL;  // HUP1
static const uint16_t UPLOAD_PERSIST_VERSION = 2;
static const char* UPLOAD_PERSIST_META_PATH = "/upload_retry.meta";
static const char* UPLOAD_PERSIST_IMAGE_PATH = "/upload_retry.bin";
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
  return pending;
}

static bool upload_persist_delete() {
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
  bool valid_v2 = (meta->version == UPLOAD_PERSIST_VERSION &&
                   meta->header_size == sizeof(*meta));
  bool valid_v1 = (meta->version == 1 &&
                   meta->header_size == sizeof(PersistedUploadMetaV1));
  if (meta->magic != UPLOAD_PERSIST_MAGIC ||
      (!valid_v2 && !valid_v1) ||
      meta->image_len == 0 ||
      meta->image_len > UPLOAD_PERSIST_MAX_IMAGE_BYTES) {
    return false;
  }
  meta->mode[sizeof(meta->mode) - 1] = '\0';
  meta->expiry_date[sizeof(meta->expiry_date) - 1] = '\0';
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

// ── Save / Load ──────────────────────────────────────────────────────

static bool upload_persist_save(const UploadJob& job, uint8_t next_retries) {
  if (!g_upload_persist_ready || !job.image_buf || job.image_len == 0) {
    return false;
  }

  PersistedUploadMeta meta = {};
  meta.magic = UPLOAD_PERSIST_MAGIC;
  meta.version = UPLOAD_PERSIST_VERSION;
  meta.header_size = sizeof(meta);
  meta.job_id = job.job_id;
  meta.image_len = (uint32_t)job.image_len;
  meta.created_epoch = job.created_epoch;
  if (meta.created_epoch < TIME_VALID_MIN_EPOCH) {
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
    upload_persist_delete();
    return false;
  }
  if (!upload_persist_write_blob(UPLOAD_PERSIST_META_PATH, (const uint8_t*)&meta, sizeof(meta))) {
    Serial.println("[UPLOAD_PERSIST] meta_write_failed");
    upload_persist_delete();
    return false;
  }

  Serial.printf("[UPLOAD_PERSIST] saved job_id=%lu len=%u retries=%u mode=%s\n",
                (unsigned long)meta.job_id,
                (unsigned)meta.image_len,
                (unsigned)meta.retries,
                meta.mode);
  return true;
}

static bool upload_persist_load(UploadJob* job) {
  if (!job) {
    return false;
  }
  PersistedUploadMeta meta = {};
  if (!upload_persist_read_meta(&meta)) {
    Serial.println("[UPLOAD_PERSIST] invalid_meta clearing");
    upload_persist_delete();
    return false;
  }
  if (meta.retries >= UPLOAD_PERSIST_MAX_RETRIES) {
    Serial.printf("[UPLOAD_PERSIST] drop_max_retries retries=%u\n", (unsigned)meta.retries);
    upload_persist_delete();
    return false;
  }
  if (upload_persist_is_stale(meta)) {
    Serial.printf("[UPLOAD_PERSIST] drop_stale age_limit_s=%lu\n",
                  (unsigned long)UPLOAD_PERSIST_MAX_AGE_S);
    upload_persist_delete();
    return false;
  }

  File file = SPIFFS.open(UPLOAD_PERSIST_IMAGE_PATH, "r");
  if (!file) {
    Serial.println("[UPLOAD_PERSIST] image_open_failed");
    upload_persist_delete();
    return false;
  }
  if (file.size() != meta.image_len) {
    Serial.printf("[UPLOAD_PERSIST] image_size_mismatch file=%u meta=%u\n",
                  (unsigned)file.size(),
                  (unsigned)meta.image_len);
    file.close();
    upload_persist_delete();
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
    upload_persist_delete();
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
  strncpy(loaded.mode, meta.mode, sizeof(loaded.mode) - 1);
  strncpy(loaded.expiry_date, meta.expiry_date, sizeof(loaded.expiry_date) - 1);
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
  if (g_upload_persist_ready) {
    return;
  }
  if (!SPIFFS.begin(false)) {
    Serial.println("[UPLOAD_PERSIST] mount_failed trying_format");
    if (!SPIFFS.begin(true)) {
      Serial.println("[UPLOAD_PERSIST] mount_failed disabled");
      return;
    }
    Serial.println("[UPLOAD_PERSIST] mounted_after_format");
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
  if (!wifi_is_connected() || upload_inflight || upload_queue_count() > 0 ||
      waiting_for_mqtt_result || dish_scan_inflight || scan_ui_inflight) {
    return;
  }
#ifdef HALO_SENSE_PROD_WRAPPER
  if (halo_provisioning_active()) {
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
  bool queued = job.is_voice
                  ? queue_voice_upload_job(job.job_id,
                                           job.image_buf,
                                           job.image_len,
                                           job.retries,
                                           true,
                                           job.created_epoch)
                  : queue_upload_job(job.job_id,
                                     job.mode,
                                     job.expiry_date,
                                     job.quantity,
                                     job.add_to_shopping_list,
                                     &job.camera_meta,
                                     job.image_buf,
                                     job.image_len,
                                     job.retries,
                                     true,
                                     job.created_epoch);
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

static void upload_persist_handle_failure(const UploadJob& job, const char* reason) {
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
  uint8_t next_retries = (job.retries < 0xFF) ? (uint8_t)(job.retries + 1) : 0xFF;
  bool saved = upload_persist_save(job, next_retries);
  if (saved && job.is_voice) {
    g_upload_persist_replay_not_before_ms = millis() + UPLOAD_PERSIST_VOICE_REPLAY_BACKOFF_MS;
  }
  uint8_t cached_count = upload_persist_has_pending() ? 1 : 0;
  g_upload_persist_attempted_this_boot = true;
  upload_persist_note_event(saved ? "cached" : "cache_fail",
                            reason ? reason : "unknown",
                            cached_count,
                            next_retries);
  dump_system_truth(saved ? "upload_cached" : "upload_cache_fail");
  Serial.printf("[UPLOAD_PERSIST] failure reason=%s job_id=%lu saved=%d next_retries=%u from_persisted=%d\n",
                reason ? reason : "unknown",
                (unsigned long)job.job_id,
                saved ? 1 : 0,
                (unsigned)next_retries,
                job.from_persisted ? 1 : 0);
#else
  (void)job;
  (void)reason;
#endif
}

#endif // SENSE_UPLOAD_PERSIST_H

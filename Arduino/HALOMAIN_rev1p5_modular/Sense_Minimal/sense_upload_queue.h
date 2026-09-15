/*
 * sense_upload_queue.h
 *
 * Upload queue management: queue count, full check, buffer allocation,
 * job queuing (image and voice), and sleep defer logic.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 17.
 *
 * Prerequisites (must be declared before #include "sense_upload_queue.h"):
 *   - freertos/FreeRTOS.h, freertos/semphr.h, freertos/task.h
 *   - UploadJob struct from sense_ops.h
 *   - upload_queue queue handle
 *   - UPLOAD_QUEUE_MAX
 *   - scan_mode_is_dish() from sense_scan.h
 *   - upload_persist_save(), upload_persist_has_pending(),
 *     upload_persist_note_event(), g_upload_persist_attempted_this_boot
 *     (conditionally, under HALO_SENSE_UPLOAD_PERSISTENCE)
 */

#ifndef SENSE_UPLOAD_QUEUE_H
#define SENSE_UPLOAD_QUEUE_H

// ── Queue count and full check ────────────────────────────────────

static uint32_t upload_queue_count() {
  uint32_t count = 0;
  if (upload_queue) {
    count += (uint32_t)uxQueueMessagesWaiting(upload_queue);
  }
  return count;
}

static bool upload_queue_is_full() {
  if (!upload_queue) {
    return true;
  }
  return upload_queue_count() >= UPLOAD_QUEUE_MAX;
}

// ── Sleep defer: drain queues before sleep ────────────────────────

static void sleep_defer_queued_background_uploads() {
  uint8_t saved_count = 0;
  uint8_t dropped_count = 0;
  auto drain_queue = [&](QueueHandle_t queue, const char* label) {
    if (!queue) {
      return;
    }
    UploadJob queued = {};
    while (xQueueReceive(queue, &queued, 0) == pdTRUE) {
      bool saved = false;
      saved = upload_persist_handle_failure(queued, "sleep_deferred");
      if (saved) ++saved_count; else ++dropped_count;
      if (queued.image_buf) {
        free(queued.image_buf);
        queued.image_buf = NULL;
      }
    }
  };

  drain_queue(upload_queue, "normal");

  if (saved_count > 0 || dropped_count > 0) {
    Serial.printf("[SLEEP] upload_defer_summary saved=%u dropped=%u\n",
                  (unsigned)saved_count, (unsigned)dropped_count);
  }
}

// ── Upload buffer allocation ──────────────────────────────────────

static uint8_t* allocate_upload_buffer(size_t len, bool* used_psram) {
  if (used_psram) {
    *used_psram = false;
  }
#if (CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM)
  uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf) {
    if (used_psram) {
      *used_psram = true;
    }
    return buf;
  }
#endif
  return (uint8_t*)malloc(len);
}

// ── Queue upload job (image) ──────────────────────────────────────

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
                             uint32_t created_epoch,
                             const UploadJob::ImageEnvelope* image,
                             bool from_image_sd) {
  if (!image_buf || image_len == 0) {
    return false;
  }
  QueueHandle_t target_queue = upload_queue;
  if (!target_queue || upload_queue_is_full()) {
    return false;
  }
  UploadJob job = {};
  job.job_id = job_id;
  if (mode) {
    strncpy(job.mode, mode, sizeof(job.mode) - 1);
    job.mode[sizeof(job.mode) - 1] = '\0';
  }
  if (expiry) {
    strncpy(job.expiry_date, expiry, sizeof(job.expiry_date) - 1);
    job.expiry_date[sizeof(job.expiry_date) - 1] = '\0';
  }
  job.quantity = (quantity < 1) ? 1 : quantity;
  job.add_to_shopping_list = add_to_shopping_list;
  job.image_buf = image_buf;
  job.image_len = image_len;
  job.retries = retries;
  job.created_ms = millis();
  job.created_epoch = created_epoch;
  job.from_persisted = from_persisted;
  if (camera_meta) {
    job.camera_meta = *camera_meta;
  } else {
    memset(&job.camera_meta, 0, sizeof(job.camera_meta));
  }
  job.from_image_sd = from_image_sd;
  if (image) job.image = *image;
  else if (from_persisted || !sense_image_freeze_envelope(job)) return false;
  if (!sense_image_payload_matches(job)) return false;
  // Plain FIFO for every mode. Dish used to xQueueSendToFront and jump ahead of
  // captures the user took first; that priority existed only to shorten the wait
  // for the nutrition result, which no longer exists.
  BaseType_t ok = xQueueSend(target_queue, &job, pdMS_TO_TICKS(10));
  if (ok != pdTRUE) {
    return false;
  }
  if (from_persisted) {
    Serial.printf("[UPLOAD_QUEUE] restored persisted job_id=%lu retries=%u\n",
                  (unsigned long)job_id,
                  (unsigned)retries);
  }
  return true;
}

// ── Queue voice upload job ────────────────────────────────────────

static bool queue_voice_upload_job(uint32_t job_id,
                                   uint8_t* audio_buf,
                                   size_t audio_len,
                                   uint8_t retries,
                                   bool from_persisted,
                                   uint32_t created_epoch,
                                   const UploadJob::VoiceEnvelope* voice,
                                   bool from_voice_sd) {
  if (!audio_buf || audio_len == 0 || !upload_queue || upload_queue_is_full()) {
    return false;
  }
  UploadJob job = {};
  job.job_id = job_id;
  job.is_voice = true;
  strncpy(job.mode, "voice", sizeof(job.mode) - 1);
  job.mode[sizeof(job.mode) - 1] = '\0';
  job.image_buf = audio_buf;
  job.image_len = audio_len;
  job.retries = retries;
  job.from_persisted = from_persisted;
  job.created_ms = millis();
  job.created_epoch = created_epoch;
  job.from_voice_sd = from_voice_sd;
  if (voice) job.voice = *voice;
  else if (from_persisted || !sense_voice_freeze_envelope(job)) return false;
  if (!sense_voice_envelope_valid(job)) return false;
  BaseType_t ok = xQueueSend(upload_queue, &job, pdMS_TO_TICKS(10));
  if (ok != pdTRUE) {
    return false;
  }
  Serial.printf("[VOICE_QUEUE] queued job_id=%lu len=%u q=%lu\n",
                (unsigned long)job_id,
                (unsigned)audio_len,
                (unsigned long)upload_queue_count());
  if (from_persisted) {
    Serial.printf("[VOICE_QUEUE] restored persisted job_id=%lu retries=%u\n",
                  (unsigned long)job_id,
                  (unsigned)retries);
  }
  return true;
}

#endif // SENSE_UPLOAD_QUEUE_H

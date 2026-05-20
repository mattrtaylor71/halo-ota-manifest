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
 *   - upload_queue, upload_queue_dish queue handles
 *   - UPLOAD_QUEUE_MAX
 *   - scan_mode_is_dish() from sense_scan.h
 *   - clear_active_dish_job() from sense_scan.h
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
  if (upload_queue_dish) {
    count += (uint32_t)uxQueueMessagesWaiting(upload_queue_dish);
  }
  return count;
}

static bool upload_queue_is_full() {
  if (!upload_queue || !upload_queue_dish) {
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
#if defined(HALO_SENSE_PROD_WRAPPER) && defined(HALO_SENSE_UPLOAD_PERSISTENCE)
      // Try to persist each upload — persist_save overwrites previous slot,
      // so last one wins. This is better than only saving the first one,
      // because the most recent capture is typically the most important.
      if (queued.image_buf && queued.image_len > 0) {
        uint8_t next_retries = (queued.retries < 0xFF) ? (uint8_t)(queued.retries + 1) : 0xFF;
        saved = upload_persist_save(queued, next_retries);
        if (saved) {
          saved_count++;
          g_upload_persist_attempted_this_boot = true;
          upload_persist_note_event("sleep_deferred", queued.mode, saved_count, next_retries);
          Serial.printf("[SLEEP] deferred_upload_saved label=%s job_id=%lu mode=%s retries=%u saved_total=%u\n",
                        label ? label : "upload",
                        (unsigned long)queued.job_id,
                        queued.mode,
                        (unsigned)next_retries,
                        (unsigned)saved_count);
        }
      }
#endif
      if (!saved) {
        dropped_count++;
        Serial.printf("[SLEEP] deferred_upload_dropped label=%s job_id=%lu mode=%s voice=%d\n",
                      label ? label : "upload",
                      (unsigned long)queued.job_id,
                      queued.mode,
                      queued.is_voice ? 1 : 0);
        uart_send_sense_diag("upload", "sleep_drop", queued.mode,
                             (int32_t)queued.job_id, "queue_not_persisted");
      }
      if (scan_mode_is_dish(queued.mode)) {
        clear_active_dish_job(queued.job_id, saved ? "sleep_deferred" : "sleep_dropped");
      }
      if (queued.image_buf) {
        free(queued.image_buf);
        queued.image_buf = NULL;
      }
    }
  };

  drain_queue(upload_queue_dish, "dish");
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
                             uint32_t created_epoch) {
  if (!image_buf || image_len == 0) {
    return false;
  }
  const bool is_dish = scan_mode_is_dish(mode);
  QueueHandle_t target_queue = is_dish ? upload_queue_dish : upload_queue;
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
  BaseType_t ok = is_dish
                    ? xQueueSendToFront(target_queue, &job, pdMS_TO_TICKS(10))
                    : xQueueSend(target_queue, &job, pdMS_TO_TICKS(10));
  if (ok != pdTRUE) {
    return false;
  }
  if (is_dish) {
    Serial.printf("[UPLOAD_QUEUE] prioritized dish job_id=%lu\n", (unsigned long)job_id);
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
                                   uint32_t created_epoch) {
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

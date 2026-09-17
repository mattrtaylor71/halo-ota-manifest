#pragma once
#include <mutex>
#include <atomic>
#include "../halo_common/MediaRetryPolicy.h"

// Protected across the upload worker and main task. NVS contains a compact
// hint only; SD/SPIFFS records and their bound cloud receipts remain authority.
static std::mutex g_media_retry_mutex;
static halo_media_retry::State g_media_retry_state;
static bool g_media_retry_loaded = false;
static uint32_t g_media_retry_durable_word = 0;
static bool g_media_retry_write_failed = false;
static uint32_t g_media_retry_write_attempt_ms = 0;
static bool g_media_retry_progress = false;
static bool g_media_retry_committed = false;
RTC_DATA_ATTR static bool g_media_retry_timer_selected = false;
static bool g_media_retry_timer_boot = false;
// A real user gesture pauses saved-media recovery for the rest of this wake.
// Fresh captures can still flush safely when that user session ends.
static std::atomic<bool> g_media_retry_user_paused{false};
static std::atomic<TaskHandle_t> g_media_retry_network_owner{nullptr};

// Socket cancellation is performed only by the task that owns that socket.
// A foreground request on another task must never inherit background cancel.
static bool media_retry_network_active() {
  const TaskHandle_t owner = g_media_retry_network_owner.load();
  return owner && owner == xTaskGetCurrentTaskHandle();
}
static bool media_retry_network_cancelled() {
  return g_media_retry_user_paused.load() && media_retry_network_active();
}
class MediaRetryNetworkScope {
 public:
  explicit MediaRetryNetworkScope(const UploadJob& job)
      : active_(job.from_voice_sd || job.from_image_sd || job.from_persisted) {
    if (active_) g_media_retry_network_owner.store(xTaskGetCurrentTaskHandle());
  }
  ~MediaRetryNetworkScope() {
    if (active_) g_media_retry_network_owner.store(nullptr);
  }
  MediaRetryNetworkScope(const MediaRetryNetworkScope&) = delete;
  MediaRetryNetworkScope& operator=(const MediaRetryNetworkScope&) = delete;
 private:
  bool active_;
};

static void media_retry_load_locked() {
  if (g_media_retry_loaded) return;
  Preferences p;
  if (p.begin("media_retry", true)) {
    const uint32_t word = p.getUInt("state", 0);
    if (halo_media_retry::decode(word, g_media_retry_state)) g_media_retry_durable_word = word;
    p.end();
  }
  g_media_retry_loaded = true;
}
static void media_retry_write_locked(bool force = false) {
  const uint32_t word = halo_media_retry::encode(g_media_retry_state);
  if (word == g_media_retry_durable_word) return;
  // A failed write stays dirty. Retry at sleep even if the state did not change,
  // with a short cooldown for repeated awake-loop inventory observations.
  if (!force && g_media_retry_write_failed &&
      (uint32_t)(millis() - g_media_retry_write_attempt_ms) < 5000) return;
  g_media_retry_write_attempt_ms = millis();
  Preferences p;
  const bool opened = p.begin("media_retry", false);
  const bool ok = opened && p.putUInt("state", word) == sizeof(word) &&
                  p.getUInt("state", 0) == word;
  if (opened) p.end();
  g_media_retry_write_failed = !ok;
  if (ok) g_media_retry_durable_word = word;
  Serial.printf("[MEDIA_RETRY] hint pending=%u backoff=%u image_first=%u durable=%u\n",
                g_media_retry_state.pending, g_media_retry_state.backoff,
                g_media_retry_state.image_first ? 1 : 0, ok ? 1 : 0);
}
static void media_retry_inventory(halo_media_retry::Store store, bool nonempty) {
  std::lock_guard<std::mutex> lock(g_media_retry_mutex);
  media_retry_load_locked();
  halo_media_retry::inventory(g_media_retry_state, store, nonempty);
  media_retry_write_locked();
}
static void media_retry_saved(halo_media_retry::Store store) {
  std::lock_guard<std::mutex> lock(g_media_retry_mutex);
  media_retry_load_locked();
  halo_media_retry::saved(g_media_retry_state, store);
  media_retry_write_locked();
}
static void media_retry_attempted(halo_media_retry::Store store) {
  std::lock_guard<std::mutex> lock(g_media_retry_mutex);
  media_retry_load_locked();
  halo_media_retry::attempted(g_media_retry_state, store);
  media_retry_write_locked();
}
static bool media_retry_image_first() {
  std::lock_guard<std::mutex> lock(g_media_retry_mutex);
  media_retry_load_locked();
  return g_media_retry_state.image_first;
}
static void media_retry_delivered() {
  std::lock_guard<std::mutex> lock(g_media_retry_mutex);
  g_media_retry_progress = true;
}
static uint32_t media_retry_interval() {
  std::lock_guard<std::mutex> lock(g_media_retry_mutex);
  media_retry_load_locked();
  return halo_media_retry::interval(g_media_retry_state, g_media_retry_progress);
}
static void media_retry_commit_sleep() {
  std::lock_guard<std::mutex> lock(g_media_retry_mutex);
  if (g_media_retry_committed) return;
  media_retry_load_locked();
  halo_media_retry::sleep_committed(g_media_retry_state, g_media_retry_progress);
  media_retry_write_locked(true);
  g_media_retry_committed = true;
}

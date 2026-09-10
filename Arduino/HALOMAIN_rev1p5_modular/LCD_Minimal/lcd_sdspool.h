// lcd_sdspool.h — SD-card spool for capture images, LCD side.
//
// WHY THIS EXISTS
// A capture is 150-190KB. The Sense's SPIFFS spool partition is 173,441 B total,
// so a single real image does not fit — measured 8/8 captures over the 173KB
// limit. There is therefore NO working failure path for an upload today: if it
// fails, the photo is lost. Meanwhile the LCD board carries a 480MB SD card that
// production firmware has never mounted.
//
// The design (per Matt, 2026-08-16): capture is user-facing and sacred, upload is
// background. Capture writes to SD and returns immediately; the upload drains
// from SD later, when the user has walked away and the screen is off. That also
// removes the crash we found — the panic only happened because an upload's TLS
// socket was live at the moment a capture tore WiFi down. Decoupled, they never
// contend.
//
// Storage layout: /sdcard/spool/<job_id>.jpg  + <job_id>.json (metadata)
// Drain order is oldest-first; a file is deleted only after a 200 from S3.

#pragma once

#include <Arduino.h>
#include "sd_card_bsp.h"
#include <dirent.h>
#include <sys/stat.h>
#include "../halo_ota_demo/firmware/shared/UartOtaProtocol.h"

// Defined in LCD_Minimal.ino (line ~117), after this header is included.
// Same forward declaration lcd_ota_uart.h uses for the identical reason.
extern HardwareSerial senseSerial;

// OTA-active predicate, defined after the OTA state in LCD_Minimal.ino.
// This remains an admission check, not a replacement for the shared SD lease.
bool lcd_ota_in_progress_for_sd_guard();

// HARD DISABLE retained (2026-09-07). The Sep 4 NULL SDMMC semaphore panic
// exposed unprotected mount/deinit concurrency. The lifecycle correction is
// prepared below, but physical card validation and safe retained-job handling
// are still required before any re-enable. A failed upload can still lose its
// photo; this disabled fallback is not a shipping durability guarantee.
#ifndef LCD_SD_SPOOL_ENABLED
#define LCD_SD_SPOOL_ENABLED 0
#endif

#ifndef LCD_SD_SPOOL_DIR
#define LCD_SD_SPOOL_DIR "/sdcard/spool"
#endif

// Cap so a long offline stretch cannot fill the card. 480MB / ~180KB is ~2600
// images; 500 is a generous ceiling that still leaves the card mostly free.
#ifndef LCD_SD_SPOOL_MAX_FILES
#define LCD_SD_SPOOL_MAX_FILES 500
#endif

static bool g_sd_ready = false;
static uint32_t g_sd_spool_writes = 0;
static uint32_t g_sd_spool_write_fails = 0;

// Mount the card. Safe to call more than once. SDMMC 4-wire on GPIO 2/3/4/5/6/42
// — verified against the LCD's pin map (panel 13-18/21, touch 11/12, encoder 7/8,
// UART 38/48, wake 39, backlight 47, I2S 40/41/45/46): no overlap.
static bool lcd_sd_init() {
#if !LCD_SD_SPOOL_ENABLED
  return false; // No mount or file operation while the fallback is disabled.
#endif
  SdCardLease lease;
  if (!lease) return false;
  if (g_sd_ready && sd_card_is_mounted()) return true;
  g_sd_ready = false;
  if (lcd_ota_in_progress_for_sd_guard()) return false;
  const esp_err_t mount_err = sd_card_Init();
  if (mount_err != ESP_OK) {
    Serial.printf("[SD] mount failed err=%d\n", (int)mount_err);
    return false;
  }
  // Existing spool initialization writes a marker. It is not a read-only
  // inventory probe; any future inventory path must bypass this function.
  FILE* f = nullptr;
  if (sd_open_file_for_write(LCD_SD_SPOOL_DIR "/.mounted", &f) != ESP_OK) {
    mkdir(LCD_SD_SPOOL_DIR, 0777);
    sd_open_file_for_write(LCD_SD_SPOOL_DIR "/.mounted", &f);
  }
  if (f) {
    size_t written = 0;
    const esp_err_t write_err = sd_write_chunk(f, (const uint8_t*)"halo", 4, &written);
    const esp_err_t close_err = sd_close_file(f);
    g_sd_ready = write_err == ESP_OK && written == 4 && close_err == ESP_OK;
  }
  Serial.printf("[SD] init %s\n", g_sd_ready ? "OK" : "FAILED");
  return g_sd_ready;
}

// ── SD health, surfaced ────────────────────────────────────────────────────
//
// The card is mounted LAZILY, only from spool paths. That means a unit whose
// card is dead, unseated or stuck looks perfectly healthy indefinitely and only
// reveals itself by losing a photo — silently, with nothing reported. Measured
// 2026-08-21: the bench card refused to mount for an entire session
// (`sdmmc_init_ocr: send_op_cond returned 0x107`) and NOTHING in the firmware
// said so; it surfaced only as `SD spool FAILED ... saved=0 / PHOTO_LOST`.
//
// So: probe once per boot and report the result in LCD_DIAG.
//
// Deliberately probed at PRE-SLEEP, not at boot. A healthy card mounts in a few
// ms, but a broken one costs a driver timeout, and the LCD re-runs setup() on
// every wake — paying that on the wake path would make a dead card slow down
// every single user interaction. At pre-sleep the user is already done.
static bool     g_sd_probed_this_boot = false;
static bool     g_sd_probe_ok = false;
static uint32_t g_sd_probe_ms = 0;

#ifndef HALO_SD_HEALTH_PROBE
#define HALO_SD_HEALTH_PROBE 1
#endif

static void lcd_sd_health_probe(const char* why) {
#if !HALO_SD_HEALTH_PROBE
  (void)why; return;   // A/B: probe compiled out
#else
  // A busy storage owner defers the probe. Never wait for I/O while the main
  // loop is trying to report health, and never unmount somebody else's card.
  SdCardLease lease(0);
  if (!lease || g_sd_probed_this_boot) return;
  g_sd_probed_this_boot = true;
  const bool borrowed_mount = sd_card_is_mounted();
  const uint32_t t0 = millis();
  g_sd_probe_ok = lcd_sd_init();
  if (!borrowed_mount && sd_card_is_mounted()) {
    // Also release our own mount when marker initialization failed.
    const esp_err_t unmount_err = sd_card_Deinit();
    if (unmount_err == ESP_OK) g_sd_ready = false;
    else g_sd_probe_ok = false;
  }
  g_sd_probe_ms = (uint32_t)(millis() - t0);
  // Loud on failure: this is the line that would have saved a session.
  if (g_sd_probe_ok) {
    Serial.printf("[SD_HEALTH] ok=1 probe_ms=%u writes=%lu fails=%lu why=%s\n",
                  (unsigned)g_sd_probe_ms,
                  (unsigned long)g_sd_spool_writes,
                  (unsigned long)g_sd_spool_write_fails,
                  why ? why : "?");
  } else {
    Serial.printf("[SD_HEALTH][WARN] ok=0 probe_ms=%u why=%s — SD spool is UNAVAILABLE; "
                  "a failed upload will LOSE the photo. Card absent, unseated or stuck "
                  "(a stuck card needs a power cycle, not a reset).\n",
                  (unsigned)g_sd_probe_ms, why ? why : "?");
  }
#endif
}

// ─────────────────── Image receive (Sense -> LCD -> SD) ───────────────────
// Frames arrive COBS-framed with CRC16 via UartOtaProtocol, the same transport
// that already moves 2MB firmware images over this link. Chunks are streamed
// straight to the card — never buffered whole in RAM, because a 190KB image
// would not fit in the LCD's ~27KB of free internal heap.

static FILE*    g_img_rx_file = nullptr;
static uint32_t g_img_rx_job = 0;   // the Sense's job_id (metadata only)
static uint32_t g_img_rx_seq = 0;   // LCD-side spool slot; names the files
static uint32_t g_img_rx_expect = 0;
static uint32_t g_img_rx_got = 0;
static uint16_t g_img_rx_next_seq = 0;
static bool     g_img_rx_active = false;
static char     g_img_rx_path[64] = {0};

// Replay metadata for the in-flight transfer, captured from IMG_XFER_BEGIN and
// written beside the image once it lands. Held rather than written immediately
// so a failed transfer leaves no orphan sidecar for the drain to trip over.
static char g_img_rx_meta[320] = {0};
static uint32_t g_img_rx_meta_epoch = 0;   // wall clock from the sender, for age caps

// Persist the sidecar next to <job>.jpg. Without this the spooled image cannot
// be replayed correctly: mode/expiry/quantity exist only in Sense RAM, which
// deep sleep destroys, so the drain would have to guess.
static bool lcd_img_write_meta(uint32_t job_id) {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return false; }
  if (!g_img_rx_meta[0]) return false;
  char p[64];
  snprintf(p, sizeof(p), LCD_SD_SPOOL_DIR "/%lu.json", (unsigned long)job_id);
  FILE* f = fopen(p, "wb");
  if (!f) { Serial.printf("[IMG_RX] meta open failed %s\n", p); return false; }
  const size_t n = strlen(g_img_rx_meta);
  const bool ok = (fwrite(g_img_rx_meta, 1, n, f) == n);
  fclose(f);
  if (!ok) { remove(p); Serial.println("[IMG_RX] meta write failed"); }
  return ok;
}

// Defined further down; declared here because the cap logic below needs it.
static uint32_t lcd_sd_spool_count(uint32_t* total_bytes_out);

// ── Spool bounds ──────────────────────────────────────────────────────
//
// Nothing bounded the spool before this. A device that cannot reach the backend
// for a long stretch keeps adding images and never reclaims any, so the card
// fills and every later capture fails to spool — the failure mode being that the
// NEWEST photos are lost while stale ones sit there forever, which is exactly
// backwards. The card is 480MB (~2,600 images at 180KB), so the cap is not about
// space so much as about not hoarding images that will never be wanted.
#ifndef LCD_SPOOL_MAX_SLOTS
#define LCD_SPOOL_MAX_SLOTS 40           // ~7MB; well beyond any plausible outage
#endif
#ifndef LCD_SPOOL_MAX_AGE_S
#define LCD_SPOOL_MAX_AGE_S (7UL * 24UL * 3600UL)   // a week
#endif

static uint32_t g_spool_evicted_count = 0;
static uint32_t g_spool_evicted_age = 0;

// Drop the oldest slot (lowest number). Used when the cap is hit.
static bool lcd_spool_evict_oldest(const char* why) {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return false; }
  uint32_t oldest = 0;
  DIR* d = opendir(LCD_SD_SPOOL_DIR);
  if (!d) return false;
  struct dirent* e;
  while ((e = readdir(d)) != NULL) {
    const char* dot = strrchr(e->d_name, '.');
    if (!dot || strcmp(dot, ".jpg") != 0) continue;
    const uint32_t s = (uint32_t)strtoul(e->d_name, NULL, 10);
    if (s == 0) continue;
    if (oldest == 0 || s < oldest) oldest = s;
  }
  closedir(d);
  if (!oldest) return false;
  char a[64], b[64];
  snprintf(a, sizeof(a), LCD_SD_SPOOL_DIR "/%lu.jpg",  (unsigned long)oldest);
  snprintf(b, sizeof(b), LCD_SD_SPOOL_DIR "/%lu.json", (unsigned long)oldest);
  remove(a); remove(b);
  Serial.printf("[SPOOL_CAP] evicted slot=%lu reason=%s\n",
                (unsigned long)oldest, why ? why : "");
  return true;
}

// Enforce the count cap before accepting a new image, and drop anything older
// than the age limit. Called at the start of every inbound transfer.
static void lcd_spool_enforce_caps(uint32_t now_epoch) {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return; }
  // Age first: an old image is worth less than a new one, so prefer dropping it
  // over evicting purely by position.
  if (now_epoch > 0) {
    DIR* d = opendir(LCD_SD_SPOOL_DIR);
    if (d) {
      struct dirent* e;
      static char victims[8][64];
      uint32_t nv = 0;
      while ((e = readdir(d)) != NULL && nv < 8) {
        const char* dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".jpg") != 0) continue;
        const uint32_t s = (uint32_t)strtoul(e->d_name, NULL, 10);
        if (s == 0) continue;
        char mp[64];
        snprintf(mp, sizeof(mp), LCD_SD_SPOOL_DIR "/%lu.json", (unsigned long)s);
        FILE* mf = fopen(mp, "rb");
        if (!mf) continue;
        char jb[320]; const size_t n = fread(jb, 1, sizeof(jb) - 1, mf); fclose(mf);
        jb[n] = '\0';
        const char* ep = strstr(jb, "\"epoch\":");
        if (!ep) continue;
        const uint32_t created = (uint32_t)strtoul(ep + 8, NULL, 10);
        if (created == 0 || now_epoch <= created) continue;
        if ((now_epoch - created) > LCD_SPOOL_MAX_AGE_S) {
          snprintf(victims[nv], sizeof(victims[0]), "%lu", (unsigned long)s);
          nv++;
        }
      }
      closedir(d);
      for (uint32_t i = 0; i < nv; i++) {
        char a[64], b[64];
        snprintf(a, sizeof(a), LCD_SD_SPOOL_DIR "/%s.jpg",  victims[i]);
        snprintf(b, sizeof(b), LCD_SD_SPOOL_DIR "/%s.json", victims[i]);
        remove(a); remove(b);
        g_spool_evicted_age++;
        Serial.printf("[SPOOL_CAP] evicted slot=%s reason=older_than_%lus\n",
                      victims[i], (unsigned long)LCD_SPOOL_MAX_AGE_S);
      }
    }
  }
  // Then the count cap.
  uint32_t total = 0;
  while (lcd_sd_spool_count(&total) >= LCD_SPOOL_MAX_SLOTS) {
    if (!lcd_spool_evict_oldest("slot_cap")) break;
    g_spool_evicted_count++;
  }
}

// Allocate a spool slot number: one higher than any file already present.
//
// Files are deliberately NOT named after the Sense's job_id. job_id restarts
// from a low number after a Sense reboot, and lcd_img_rx_end() removes any
// existing file before renaming — so a post-reboot capture that happened to
// reuse a job_id would silently destroy an older photo that had not been
// uploaded yet. The real job_id lives in the sidecar, where collisions are
// harmless.
static uint32_t lcd_spool_alloc_seq() {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return 0; }
  uint32_t max_seq = 0;
  DIR* d = opendir(LCD_SD_SPOOL_DIR);
  if (d) {
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
      const uint32_t s = (uint32_t)strtoul(e->d_name, NULL, 10);
      if (s > max_seq) max_seq = s;
    }
    closedir(d);
  }
  return max_seq + 1;
}

// Begin receiving image `job_id` of `len` bytes. Opens the SD file.
static bool lcd_img_rx_begin(uint32_t job_id, uint32_t len) {
  SdCardLease lease;
  if (!lease) { return false; }
  if (!lcd_sd_init()) {
    Serial.println("[IMG_RX] reject: SD not mounted");
    return false;
  }
  if (g_img_rx_active && g_img_rx_file) {   // stale transfer — abandon it
    sd_close_file(g_img_rx_file);
    remove(g_img_rx_path);
    Serial.printf("[IMG_RX] abandoned stale transfer job=%lu\n", (unsigned long)g_img_rx_job);
  }
  // Bound the spool before taking another image, so a long outage cannot fill
  // the card and start failing the NEWEST captures.
  lcd_spool_enforce_caps(g_img_rx_meta_epoch);
  g_img_rx_seq = lcd_spool_alloc_seq();
  snprintf(g_img_rx_path, sizeof(g_img_rx_path), LCD_SD_SPOOL_DIR "/%lu.part",
           (unsigned long)g_img_rx_seq);
  if (sd_open_file_for_write(g_img_rx_path, &g_img_rx_file) != ESP_OK || !g_img_rx_file) {
    Serial.printf("[IMG_RX] open failed path=%s\n", g_img_rx_path);
    g_img_rx_file = nullptr;
    return false;
  }
  g_img_rx_job = job_id; g_img_rx_expect = len; g_img_rx_got = 0;
  g_img_rx_next_seq = 0; g_img_rx_active = true;
  Serial.printf("[IMG_RX] begin job=%lu len=%lu -> %s\n",
                (unsigned long)job_id, (unsigned long)len, g_img_rx_path);
  return true;
}

// Write one chunk. `seq` must be the next expected sequence — a gap means a
// frame was lost, and silently writing past it would corrupt the image.
static bool lcd_img_rx_chunk(uint16_t seq, const uint8_t* data, size_t len) {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return false; }
  if (!g_img_rx_active || !g_img_rx_file) return false;
  if (seq != g_img_rx_next_seq) {
    Serial.printf("[IMG_RX] seq gap: got %u want %u — aborting job=%lu\n",
                  (unsigned)seq, (unsigned)g_img_rx_next_seq, (unsigned long)g_img_rx_job);
    return false;
  }
  size_t written = 0;
  if (sd_write_chunk(g_img_rx_file, data, len, &written) != ESP_OK || written != len) {
    Serial.printf("[IMG_RX] write failed at %lu\n", (unsigned long)g_img_rx_got);
    return false;
  }
  g_img_rx_got += written;
  g_img_rx_next_seq++;
  return true;
}

// Finish. Only renames .part -> .jpg when the byte count matches exactly, so a
// truncated transfer can never be mistaken for a complete image by the drain.
static bool lcd_img_rx_end() {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return false; }
  if (!g_img_rx_active) return false;
  bool ok = (g_img_rx_file != nullptr) && (g_img_rx_got == g_img_rx_expect);
  if (g_img_rx_file) {
    const esp_err_t close_err = sd_close_file(g_img_rx_file);
    g_img_rx_file = nullptr;
    ok = ok && close_err == ESP_OK;
  }
  char final_path[64];
  snprintf(final_path, sizeof(final_path), LCD_SD_SPOOL_DIR "/%lu.jpg",
           (unsigned long)g_img_rx_seq);
  if (ok) {
    remove(final_path);                       // overwrite any previous attempt
    ok = (rename(g_img_rx_path, final_path) == 0);
    if (!ok) {
      Serial.println("[IMG_RX] rename failed");
    } else {
      // Sidecar AFTER the rename: the .jpg appearing is what the drain treats as
      // "a complete job", so writing metadata first would briefly advertise a
      // job whose image does not exist yet.
      if (!lcd_img_write_meta(g_img_rx_seq)) {
        // An image we cannot describe is worse than no image: the drain would
        // have to guess the mode and could file a check-in as a dish. Drop it
        // rather than upload it wrongly.
        Serial.println("[IMG_RX] no metadata - discarding image to avoid a wrong-mode upload");
        remove(final_path);
        ok = false;
      }
    }
  } else {
    Serial.printf("[IMG_RX] incomplete: got=%lu expect=%lu — discarding\n",
                  (unsigned long)g_img_rx_got, (unsigned long)g_img_rx_expect);
    remove(g_img_rx_path);
  }
  Serial.printf("[IMG_RX] end job=%lu bytes=%lu result=%s\n",
                (unsigned long)g_img_rx_job, (unsigned long)g_img_rx_got,
                ok ? "OK" : "FAIL");
  if (ok) g_sd_spool_writes++; else g_sd_spool_write_fails++;
  g_img_rx_active = false;
  return ok;
}

static void lcd_img_rx_abort(const char* why) {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return; }
  if (!g_img_rx_active) return;
  if (g_img_rx_file) { sd_close_file(g_img_rx_file); g_img_rx_file = nullptr; }
  remove(g_img_rx_path);
  Serial.printf("[IMG_RX] abort job=%lu reason=%s\n", (unsigned long)g_img_rx_job, why ? why : "");
  g_img_rx_active = false;
  g_sd_spool_write_fails++;
}

// How many complete images are spooled, and their total size.
static uint32_t lcd_sd_spool_count(uint32_t* total_bytes_out) {
  SdCardLease lease;
  if (!lease) { if (total_bytes_out) *total_bytes_out = 0; return 0; }
  if (!lcd_sd_init()) { if (total_bytes_out) *total_bytes_out = 0; return 0; }
  uint32_t n = 0, total = 0;
  DIR* d = opendir(LCD_SD_SPOOL_DIR);
  if (!d) { if (total_bytes_out) *total_bytes_out = 0; return 0; }
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    const char* dot = strrchr(e->d_name, '.');
    if (!dot || strcmp(dot, ".jpg") != 0) continue;
    n++;
    char p[96];
    snprintf(p, sizeof(p), LCD_SD_SPOOL_DIR "/%s", e->d_name);
    struct stat st;
    if (stat(p, &st) == 0) total += (uint32_t)st.st_size;
  }
  closedir(d);
  if (total_bytes_out) *total_bytes_out = total;
  return n;
}


// Pump image frames. Called from uart_task while g_img_rx_binary_mode is set,
// exactly like lcd_ota_receive_loop() does for firmware. Returns false when the
// transfer has finished (either way) so the caller can leave binary mode.
static bool g_img_rx_binary_mode = false;
static UartOtaProtocol* g_img_rx_proto = nullptr;

static bool lcd_img_receive_loop() {
  if (!g_img_rx_proto) {
    g_img_rx_proto = new UartOtaProtocol(&senseSerial);
    if (!g_img_rx_proto) { lcd_img_rx_abort("no_proto"); g_img_rx_binary_mode = false; return false; }
    g_img_rx_proto->quiet = true;   // per-frame logs would swamp USB-CDC
  }
  lcd_freeze_wdt_feed();   // inbound transfers are equally long; see send side
  uint8_t type = 0; uint16_t seq = 0;
  static uint8_t frame[MAX_FRAME_SIZE];
  // data_len is IN/OUT: it must carry the buffer CAPACITY on the way in.
  // Passing 0 makes recv_frame reject every chunk as "payload too large".
  size_t len = sizeof(frame);
  if (!g_img_rx_proto->recv_frame(&type, &seq, frame, &len, 3000)) {
    lcd_img_rx_abort("frame_timeout");
    g_img_rx_binary_mode = false;
    return false;
  }
  switch (type) {
    case MSG_IMG_CHUNK:
      if (lcd_img_rx_chunk(seq, frame, len)) {
        g_img_rx_proto->send_frame(MSG_IMG_ACK, seq, nullptr, 0);
      } else {
        g_img_rx_proto->send_frame(MSG_IMG_NACK, seq, nullptr, 0);
        lcd_img_rx_abort("chunk_failed");
        g_img_rx_binary_mode = false;
        return false;
      }
      break;
    case MSG_IMG_END: {
      bool ok = lcd_img_rx_end();
      g_img_rx_proto->send_frame(ok ? MSG_IMG_ACK : MSG_IMG_NACK, seq, nullptr, 0);
      g_img_rx_binary_mode = false;
      return false;
    }
    default:
      // A firmware frame here would mean the two transfers collided. Refuse it
      // loudly rather than write it into a photo.
      Serial.printf("[IMG_RX] unexpected frame type=0x%02X — aborting\n", (unsigned)type);
      lcd_img_rx_abort("wrong_frame_type");
      g_img_rx_binary_mode = false;
      return false;
  }
  return true;
}

// ── Drain: hand a spooled image back to the Sense for upload ──────────
//
// The Sense owns WiFi, TLS, presign and the retry queue. The LCD has the card.
// So a spooled image has to travel back over the same UART to be uploaded.
// ~16s per image, paid only when the device is otherwise idle.
//
// The LCD does NOT upload directly: it has credentials (LcdWifiCreds) but no
// STA/HTTPS path in normal operation, and building one would duplicate the
// Sense's TLS + presign + retry stack and contend with the panel for memory.

static uint32_t lcd_spool_file_size(const char* path) {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return 0; }
  struct stat st;
  if (stat(path, &st) != 0) return 0;
  return (uint32_t)st.st_size;
}

// Find the oldest complete job (lowest slot with BOTH .jpg and .json). A .jpg
// without its sidecar is unreplayable, so it is skipped rather than guessed at.
static bool lcd_spool_oldest(uint32_t* seq_out, uint32_t* len_out) {
  SdCardLease lease;
  if (!lease) { return false; }
  // The card mounts lazily. Without this, a fresh boot answers SPOOL_LIST with
  // count=0 and the drain concludes there is nothing to upload — so spooled
  // photos are silently never drained until something else happens to mount it.
  if (!lcd_sd_init()) return false;
  uint32_t best = 0;
  DIR* d = opendir(LCD_SD_SPOOL_DIR);
  if (!d) return false;
  struct dirent* e;
  while ((e = readdir(d)) != NULL) {
    const char* dot = strrchr(e->d_name, '.');
    if (!dot || strcmp(dot, ".jpg") != 0) continue;
    const uint32_t s = (uint32_t)strtoul(e->d_name, NULL, 10);
    if (s == 0) continue;
    if (best != 0 && s >= best) continue;
    char mp[64];
    snprintf(mp, sizeof(mp), LCD_SD_SPOOL_DIR "/%lu.json", (unsigned long)s);
    FILE* mf = fopen(mp, "rb");
    if (!mf) continue;              // no sidecar: cannot replay it faithfully
    fclose(mf);
    best = s;
  }
  closedir(d);
  if (!best) return false;
  char p[64];
  snprintf(p, sizeof(p), LCD_SD_SPOOL_DIR "/%lu.jpg", (unsigned long)best);
  if (seq_out) *seq_out = best;
  if (len_out) *len_out = lcd_spool_file_size(p);
  return true;
}

// Read a slot's sidecar into `out`. Returns false if missing/oversized.
static bool lcd_spool_read_meta(uint32_t seq, char* out, size_t out_sz) {
  SdCardLease lease;
  if (!lease || !LCD_SD_SPOOL_ENABLED || !sd_card_is_mounted()) { return false; }
  char p[64];
  snprintf(p, sizeof(p), LCD_SD_SPOOL_DIR "/%lu.json", (unsigned long)seq);
  FILE* f = fopen(p, "rb");
  if (!f) return false;
  const size_t n = fread(out, 1, out_sz - 1, f);
  fclose(f);
  if (n == 0) return false;
  out[n] = '\0';
  return true;
}

// Delete a slot (image + sidecar). Called once the Sense confirms the upload.
static bool lcd_spool_delete(uint32_t seq) {
  SdCardLease lease;
  if (!lease) { return false; }
  if (!lcd_sd_init()) return false;   // must not report success on an unmounted card
  char a[64], b[64];
  snprintf(a, sizeof(a), LCD_SD_SPOOL_DIR "/%lu.jpg",  (unsigned long)seq);
  snprintf(b, sizeof(b), LCD_SD_SPOOL_DIR "/%lu.json", (unsigned long)seq);
  const bool ja = (remove(a) == 0);
  remove(b);                        // sidecar may already be gone; not fatal
  Serial.printf("[SPOOL_TX] deleted slot=%lu img=%d\n", (unsigned long)seq, ja ? 1 : 0);
  return ja;
}

// Set by the SPOOL_FETCH handler; the UART task performs the transfer. Doing
// the 16s stream inside the RX handler would block line parsing for its whole
// duration, so the handler only records the intent.
static uint32_t g_spool_tx_slot = 0;
static bool     g_spool_tx_pending = false;
// True for the duration of the outbound stream. Suppresses this board's JSON TX
// so it cannot interleave text into the COBS frames it is sending.
static bool     g_spool_tx_active = false;

// Stream one spooled image to the Sense. Mirrors sense_spool_image_to_lcd():
// lock-step MSG_IMG_CHUNK + ACK, then MSG_IMG_END.
static bool lcd_spool_send_file(uint32_t seq) {
  char p[64];
  snprintf(p, sizeof(p), LCD_SD_SPOOL_DIR "/%lu.jpg", (unsigned long)seq);
  FILE* f = nullptr;
  {
    SdCardLease lease;
    if (!lease || !lcd_sd_init()) { Serial.println("[SPOOL_TX] SD not mounted"); return false; }
    if (sd_open_file_for_read(p, &f) != ESP_OK) {
      Serial.printf("[SPOOL_TX] open failed %s\n", p); return false;
    }
  }
  // The open-stream pin spans UART waits; the task mutex does not. Each SD
  // operation below obtains its own lease. Teardown refuses this live pin.

  // Same trap as the inbound direction: anything left in our RX from the JSON
  // handshake would be decoded as the first COBS byte and shift the whole
  // frame. The Sense settles before receiving, so draining here is safe.
  while (senseSerial.available()) senseSerial.read();

  if (!g_img_rx_proto) {
    g_img_rx_proto = new UartOtaProtocol(&senseSerial);
    if (!g_img_rx_proto) { sd_close_file(f); return false; }
    g_img_rx_proto->quiet = true;
  }

  static uint8_t buf[MAX_CHUNK_SIZE];
  uint16_t seq_no = 0;
  size_t sent = 0, n;
  bool ok = true;
  const uint32_t t0 = millis();
  g_spool_tx_active = true;    // hold off JSON TX for the whole transfer

  while (true) {
    if (sd_read_chunk(f, buf, sizeof(buf), &n) != ESP_OK) { ok = false; break; }
    if (n == 0) break;
    // A full image is ~16-18s of lock-step frames — far longer than a normal
    // uart_task iteration. Feed explicitly so the freeze watchdog cannot reboot
    // us mid-transfer and destroy the photo this exists to preserve.
    lcd_freeze_wdt_feed();
    if (!g_img_rx_proto->send_frame(MSG_IMG_CHUNK, seq_no, buf, n)) { ok = false; break; }
    uint8_t rtype = 0; uint16_t rseq = 0;
    static uint8_t rframe[MAX_FRAME_SIZE];
    size_t rlen = sizeof(rframe);        // IN/OUT: must carry capacity IN
    if (!g_img_rx_proto->recv_frame(&rtype, &rseq, rframe, &rlen, 4000)) {
      Serial.printf("[SPOOL_TX] ack timeout at seq=%u\n", (unsigned)seq_no);
      ok = false; break;
    }
    if (rtype != MSG_IMG_ACK || rseq != seq_no) {
      Serial.printf("[SPOOL_TX] bad ack type=0x%02X seq=%u want=%u\n",
                    (unsigned)rtype, (unsigned)rseq, (unsigned)seq_no);
      ok = false; break;
    }
    sent += n; seq_no++;
  }
  if (sd_close_file(f) != ESP_OK) ok = false;

  if (ok) {
    g_img_rx_proto->send_frame(MSG_IMG_END, seq_no, nullptr, 0);
    uint8_t rtype = 0; uint16_t rseq = 0;
    static uint8_t rframe[MAX_FRAME_SIZE];
    size_t rlen = sizeof(rframe);
    ok = g_img_rx_proto->recv_frame(&rtype, &rseq, rframe, &rlen, 4000)
         && rtype == MSG_IMG_ACK;
  }
  g_spool_tx_active = false;
  const uint32_t ms = millis() - t0;
  Serial.printf("[SPOOL_TX] slot=%lu result=%s bytes=%u ms=%lu rate_Bps=%lu\n",
                (unsigned long)seq, ok ? "OK" : "FAIL", (unsigned)sent,
                (unsigned long)ms, ms ? (unsigned long)(sent * 1000UL / ms) : 0UL);
  return ok;
}

// Self-test: mount, write a known payload, read it back, compare, report.
// Exposed as the `sdtest` USB command so the card can be proven on real
// hardware before anything depends on it.
static void lcd_sd_selftest() {
  SdCardLease lease;
  if (!lease) { return; }
  Serial.println("[SDTEST] begin");
  if (!lcd_sd_init()) {
    Serial.println("[SDTEST] RESULT=FAIL reason=mount");
    return;
  }
  const char* path = LCD_SD_SPOOL_DIR "/selftest.bin";
  // Write a 64KB pattern — big enough to exercise multi-block writes, small
  // enough to be quick. A 4-byte file would not prove much about a 180KB image.
  const size_t TEST_LEN = 64 * 1024;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(TEST_LEN, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)malloc(TEST_LEN);
  if (!buf) { Serial.println("[SDTEST] RESULT=FAIL reason=alloc"); return; }
  for (size_t i = 0; i < TEST_LEN; i++) buf[i] = (uint8_t)(i * 31 + 7);

  uint32_t t0 = millis();
  FILE* f = nullptr;
  if (sd_open_file_for_write(path, &f) != ESP_OK || !f) {
    Serial.println("[SDTEST] RESULT=FAIL reason=open_write"); free(buf); return;
  }
  size_t written = 0, total = 0;
  const size_t CHUNK = 4096;   // mirrors the UART frame cadence we will use
  for (size_t off = 0; off < TEST_LEN; off += CHUNK) {
    size_t n = (TEST_LEN - off < CHUNK) ? (TEST_LEN - off) : CHUNK;
    if (sd_write_chunk(f, buf + off, n, &written) != ESP_OK) {
      Serial.printf("[SDTEST] RESULT=FAIL reason=write off=%u\n", (unsigned)off);
      sd_close_file(f); free(buf); return;
    }
    total += written;
  }
  sd_close_file(f);
  uint32_t write_ms = millis() - t0;

  // Read back and verify byte-for-byte.
  t0 = millis();
  FILE* rf = fopen(path, "rb");
  if (!rf) { Serial.println("[SDTEST] RESULT=FAIL reason=open_read"); free(buf); return; }
  size_t mismatches = 0, read_total = 0;
  uint8_t rbuf[CHUNK];
  for (size_t off = 0; off < TEST_LEN; off += CHUNK) {
    size_t want = (TEST_LEN - off < CHUNK) ? (TEST_LEN - off) : CHUNK;
    size_t got = fread(rbuf, 1, want, rf);
    read_total += got;
    for (size_t i = 0; i < got; i++) if (rbuf[i] != buf[off + i]) mismatches++;
    if (got != want) break;
  }
  fclose(rf);
  uint32_t read_ms = millis() - t0;
  remove(path);
  free(buf);

  bool ok = (total == TEST_LEN) && (read_total == TEST_LEN) && (mismatches == 0);
  Serial.printf("[SDTEST] RESULT=%s wrote=%u read=%u mismatches=%u "
                "write_ms=%lu read_ms=%lu write_kBps=%lu\n",
                ok ? "PASS" : "FAIL",
                (unsigned)total, (unsigned)read_total, (unsigned)mismatches,
                (unsigned long)write_ms, (unsigned long)read_ms,
                write_ms ? (unsigned long)((TEST_LEN / 1024UL) * 1000UL / write_ms) : 0UL);
}

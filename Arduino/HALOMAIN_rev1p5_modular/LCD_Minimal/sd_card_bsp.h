#ifndef SD_CARD_BSP_H
#define SD_CARD_BSP_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Task-only, recursive storage lease. Keep it for the complete lifetime of raw
// POSIX file/directory operations, including fclose/closedir. Never acquire it
// under an LVGL lock or CPU critical section, or retain it across tasks.
class SdCardLease {
 public:
  // UINT32_MAX is reserved for completing an already-open stream's close.
  explicit SdCardLease(uint32_t timeout_ms = 1000);
  ~SdCardLease();
  explicit operator bool() const { return status_ == ESP_OK; }
  esp_err_t status() const { return status_; }
  SdCardLease(const SdCardLease&) = delete;
  SdCardLease& operator=(const SdCardLease&) = delete;
 private:
  esp_err_t status_;
};

esp_err_t sd_card_Init(void);
esp_err_t sd_card_Deinit(void); // refuses teardown while a stream is open
bool sd_card_is_mounted(void);  // hold an outer lease when using this as a guard
float sd_card_get_value(void);
esp_err_t s_example_write_file(const char *path, char *data);
esp_err_t s_example_read_file(const char *path,char *pxbuf,uint32_t *outLen);

// Binary file write functions for video files
esp_err_t sd_write_binary_file(const char *path, const uint8_t *data, size_t data_len);
esp_err_t sd_write_binary_file_stream(const char *path, const uint8_t *data, size_t data_len, size_t *bytes_written);
// Open streams pin the mount between calls; close releases the pin even if the
// final flush fails. Close waits for the storage owner so it cannot orphan a
// live pin on a timeout. Like all file I/O, call it only from task context.
esp_err_t sd_open_file_for_write(const char *path, FILE **file_handle);
esp_err_t sd_open_file_for_read(const char *path, FILE **file_handle);
esp_err_t sd_read_chunk(FILE *file_handle, uint8_t *data, size_t data_len, size_t *bytes_read);
esp_err_t sd_write_chunk(FILE *file_handle, const uint8_t *data, size_t data_len, size_t *bytes_written);
esp_err_t sd_close_file(FILE *file_handle);

#endif

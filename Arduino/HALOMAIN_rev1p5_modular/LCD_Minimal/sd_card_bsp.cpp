#include <stdio.h>
#include <string.h>
#include <Arduino.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_card_bsp.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SDMMC_CMD_PIN   (gpio_num_t)3
#define SDMMC_D0_PIN    (gpio_num_t)5
#define SDMMC_D1_PIN    (gpio_num_t)6
#define SDMMC_D2_PIN    (gpio_num_t)42
#define SDMMC_D3_PIN    (gpio_num_t)2
#define SDMMC_CLK_PIN   (gpio_num_t)4
#define SDlist "/sdcard" //Directory, similar to a standard


// One owner domain for host initialization, every I/O operation and teardown.
// The short critical section initializes only the static mutex, never SD/VFS.
static StaticSemaphore_t sd_mutex_storage;
static SemaphoreHandle_t sd_mutex = nullptr;
static portMUX_TYPE sd_mutex_init_mux = portMUX_INITIALIZER_UNLOCKED;
static sdmmc_card_t *card = nullptr;
static esp_err_t sd_lifecycle_fault = ESP_OK; // uncertain SDK teardown: reboot required
static FILE* sd_open_streams[5] = {}; // matches mount_config.max_files

SdCardLease::SdCardLease(uint32_t timeout_ms) : status_(ESP_ERR_INVALID_STATE) {
  if (xPortInIsrContext()) return;
  portENTER_CRITICAL(&sd_mutex_init_mux);
  if (!sd_mutex) sd_mutex = xSemaphoreCreateRecursiveMutexStatic(&sd_mutex_storage);
  SemaphoreHandle_t mutex = sd_mutex;
  portEXIT_CRITICAL(&sd_mutex_init_mux);
  if (!mutex) { status_ = ESP_ERR_NO_MEM; return; }
  const TickType_t wait = timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  status_ = xSemaphoreTakeRecursive(mutex, wait) == pdTRUE
              ? ESP_OK : ESP_ERR_TIMEOUT;
}

SdCardLease::~SdCardLease() {
  if (status_ == ESP_OK) xSemaphoreGiveRecursive(sd_mutex);
}

static int sd_stream_index(FILE* file) {
  if (!file) return -1;
  for (unsigned i = 0; i < 5; ++i) if (sd_open_streams[i] == file) return (int)i;
  return -1;
}

bool sd_card_is_mounted(void) {
  SdCardLease lease;
  return lease && card != nullptr;
}


esp_err_t sd_card_Init(void)
{
  SdCardLease lease;
  if (!lease) return lease.status();
  if (sd_lifecycle_fault != ESP_OK) return sd_lifecycle_fault;
  if (card) return ESP_OK;
  esp_vfs_fat_sdmmc_mount_config_t mount_config = 
  {
    .format_if_mount_failed = false,     // Never format on mount failure
    .max_files = 5,                      //Maximum number of open files
    .allocation_unit_size = 512,         //Similar to sector size
  };

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;//high speed

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 4;           //4-wire
  slot_config.clk = SDMMC_CLK_PIN;
  slot_config.cmd = SDMMC_CMD_PIN;
  slot_config.d0 = SDMMC_D0_PIN;
  slot_config.d1 = SDMMC_D1_PIN;
  slot_config.d2 = SDMMC_D2_PIN;
  slot_config.d3 = SDMMC_D3_PIN;
  // Mount failure can deinitialize SDMMC internally. No other owner may enter
  // the host until it finishes; publish only a successful, non-null card.
  sdmmc_card_t* mounted_card = nullptr;
  const esp_err_t err = esp_vfs_fat_sdmmc_mount(SDlist, &host, &slot_config,
                                              &mount_config, &mounted_card);
  if (err != ESP_OK) return err;
  if (!mounted_card) return ESP_ERR_INVALID_RESPONSE;
  card = mounted_card;
  sdmmc_card_print_info(stdout, card);
  Serial.print("practical_size:");
  Serial.println(sd_card_get_value());
  return ESP_OK;
}

esp_err_t sd_card_Deinit(void)
{
  SdCardLease lease;
  if (!lease) return lease.status();
  for (FILE* stream : sd_open_streams) if (stream) return ESP_ERR_INVALID_STATE;
  if (!card) return sd_lifecycle_fault;
  // IDF frees the card/host before unregister-path can return an error. Revoke
  // publication before that consuming call; never reuse/retry a possibly freed
  // pointer. A partial teardown leaves VFS state uncertain until a fresh boot.
  sdmmc_card_t* consumed_card = card;
  card = nullptr;
  const esp_err_t err = esp_vfs_fat_sdcard_unmount(SDlist, consumed_card);
  if (err != ESP_OK) sd_lifecycle_fault = err;
  return err;
}

float sd_card_get_value(void)
{
  SdCardLease lease;
  if (!lease) return 0;
  if(card != NULL)
  {
    return (float)(card->csd.capacity)/2048/1024; //G
  }
  else
  return 0;
}

/*write data
path:path
data:data
*/
esp_err_t s_example_write_file(const char *path, char *data)
{
  SdCardLease lease;
  if (!lease) return lease.status();
  esp_err_t err;
  if(card == NULL)
  {
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card); //First check if there is an SD card
  if(err != ESP_OK)
  {
    return err;
  }
  FILE *f = fopen(path, "w"); //Get path address
  if(f == NULL)
  {
    printf("path:Write Wrong path\n");
    return ESP_ERR_NOT_FOUND;
  }
  fprintf(f, data); //write in
  fclose(f);
  return ESP_OK;
}
/*
read data
path:path
*/
esp_err_t s_example_read_file(const char *path,char *pxbuf,uint32_t *outLen)
{
  SdCardLease lease;
  if (!lease) return lease.status();
  esp_err_t err;
  if(card == NULL)
  {
    printf("path:card == NULL\n");
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card); //First check if there is an SD card
  if(err != ESP_OK)
  {
    printf("path:card == NO\n");
    return err;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    printf("path:Read Wrong path\n");
    return ESP_ERR_NOT_FOUND;
  }
  fseek(f, 0, SEEK_END);     //Move the pointer to the back
  uint32_t unlen = ftell(f);
  //fgets(pxbuf, unlen, f); //Read text
  fseek(f, 0, SEEK_SET); //Move the pointer to the front
  uint32_t poutLen = fread((void *)pxbuf,1,unlen,f);
  printf("pxlen: %ld,outLen: %ld\n",unlen,poutLen);
  //*outLen = poutLen;
  fclose(f);
  return ESP_OK;
}

/* Write binary data to file (for video files, images, etc.)
 * path: file path
 * data: binary data buffer
 * data_len: length of data to write
 */
esp_err_t sd_write_binary_file(const char *path, const uint8_t *data, size_t data_len)
{
  SdCardLease lease;
  if (!lease) return lease.status();
  esp_err_t err;
  if(card == NULL)
  {
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card);
  if(err != ESP_OK)
  {
    return err;
  }
  FILE *f = fopen(path, "wb"); // Open in binary write mode
  if(f == NULL)
  {
    printf("sd_write_binary_file: Failed to open file: %s\n", path);
    return ESP_ERR_NOT_FOUND;
  }
  size_t written = fwrite(data, 1, data_len, f);
  fclose(f);
  
  if(written != data_len)
  {
    printf("sd_write_binary_file: Partial write (%zu/%zu bytes)\n", written, data_len);
    return ESP_ERR_INVALID_SIZE;
  }
  
  return ESP_OK;
}

/* Open file for streaming write (for large files like videos)
 * path: file path
 * file_handle: pointer to FILE* to store handle
 */
static esp_err_t sd_open_stream(const char *path, const char *mode, FILE **file_handle)
{
  SdCardLease lease;
  if (!lease) return lease.status();
  if (!file_handle || !path) return ESP_ERR_INVALID_ARG;
  *file_handle = nullptr;
  int slot = -1;
  for (unsigned i = 0; i < 5; ++i) if (!sd_open_streams[i]) { slot = (int)i; break; }
  if (slot < 0) return ESP_ERR_NO_MEM;
  if(card == NULL)
  {
    return ESP_ERR_NOT_FOUND;
  }
  esp_err_t err = sdmmc_get_status(card);
  if(err != ESP_OK)
  {
    return err;
  }
  
  FILE *f = fopen(path, mode);
  if(f == NULL)
  {
    printf("sd_open_file_for_write: Failed to open file: %s\n", path);
    return ESP_ERR_NOT_FOUND;
  }
  
  sd_open_streams[slot] = f;
  *file_handle = f;
  return ESP_OK;
}

esp_err_t sd_open_file_for_write(const char *path, FILE **file_handle) {
  return sd_open_stream(path, "wb", file_handle);
}

esp_err_t sd_open_file_for_read(const char *path, FILE **file_handle) {
  return sd_open_stream(path, "rb", file_handle);
}

esp_err_t sd_read_chunk(FILE *file_handle, uint8_t *data, size_t data_len, size_t *bytes_read) {
  SdCardLease lease;
  if (!lease) return lease.status();
  if (bytes_read) *bytes_read = 0;
  if (sd_stream_index(file_handle) < 0 || (!data && data_len)) return ESP_ERR_INVALID_ARG;
  const size_t got = fread(data, 1, data_len, file_handle);
  if (bytes_read) *bytes_read = got;
  return ferror(file_handle) ? ESP_FAIL : ESP_OK;
}

/* Write a chunk of data to an open file
 * file_handle: file handle from sd_open_file_for_write
 * data: binary data buffer
 * data_len: length of data to write
 * bytes_written: pointer to store number of bytes actually written
 */
esp_err_t sd_write_chunk(FILE *file_handle, const uint8_t *data, size_t data_len, size_t *bytes_written)
{
  SdCardLease lease;
  if (!lease) return lease.status();
  if (bytes_written) *bytes_written = 0;
  if (sd_stream_index(file_handle) < 0 || (!data && data_len))
  {
    return ESP_ERR_INVALID_ARG;
  }
  
  size_t written = fwrite(data, 1, data_len, file_handle);
  if(bytes_written != NULL)
  {
    *bytes_written = written;
  }
  
  if(written != data_len)
  {
    return ESP_ERR_INVALID_SIZE;
  }
  
  return ESP_OK;
}

/* Close an open file
 * file_handle: file handle to close
 */
esp_err_t sd_close_file(FILE *file_handle)
{
  // A timeout here would strand a live mount pin when a caller drops its local
  // FILE*. Wait for serialized cleanup; never called from an ISR/UI lock.
  SdCardLease lease(UINT32_MAX);
  if (!lease) return lease.status();
  const int slot = sd_stream_index(file_handle);
  if(slot < 0)
  {
    return ESP_ERR_INVALID_ARG;
  }
  
  const int result = fclose(file_handle);
  sd_open_streams[slot] = nullptr;
  return result == 0 ? ESP_OK : ESP_FAIL;
}

/* Write binary file with streaming (for very large files)
 * This is a convenience function that opens, writes, and closes
 */
esp_err_t sd_write_binary_file_stream(const char *path, const uint8_t *data, size_t data_len, size_t *bytes_written)
{
  SdCardLease lease;
  if (!lease) return lease.status();
  FILE *f = NULL;
  esp_err_t err = sd_open_file_for_write(path, &f);
  if(err != ESP_OK)
  {
    return err;
  }
  
  size_t written = 0;
  err = sd_write_chunk(f, data, data_len, &written);
  
  const esp_err_t close_err = sd_close_file(f);
  if (err == ESP_OK) err = close_err;
  
  if(bytes_written != NULL)
  {
    *bytes_written = written;
  }
  
  return err;
}

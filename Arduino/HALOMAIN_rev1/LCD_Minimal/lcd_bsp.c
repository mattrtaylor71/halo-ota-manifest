#include "lcd_bsp.h"
#include "esp_lcd_sh8601.h"
#include "lcd_config.h"
#include "cst816.h"
#include "ui.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <stdio.h>
static SemaphoreHandle_t lvgl_mux = NULL; //mutex semaphores
#define LCD_HOST    SPI2_HOST

#define SH8601_ID 0x86
#define CO5300_ID 0xff



static esp_lcd_panel_io_handle_t amoled_panel_io_handle = NULL; 
static esp_lcd_panel_handle_t amoled_panel_handle = NULL;
extern volatile bool g_sleep_transition;
extern esp_err_t esp_lcd_panel_io_wait_tx_done(esp_lcd_panel_io_handle_t io, int timeout_ms) __attribute__((weak));

#define FLUSH_TIMEOUT_MS           150
#define FLUSH_FAIL_RESET_THRESHOLD 10
#define FLUSH_LOG_INTERVAL_MS      500
static uint32_t flush_fail_count = 0;
static volatile uint32_t s_flush_submit_ok = 0;
static volatile uint32_t s_flush_submit_fail = 0;
static volatile int s_flush_soft_fault = 0;
static bool display_reset_requested = false;
static lv_disp_drv_t *s_flush_pending_drv = NULL;
static unsigned long s_flush_start_ms = 0;
static uint32_t flush_outstanding_count = 0;
static unsigned long last_flush_log_ms = 0;
/* Flush stats: 1s window for rate, running average for duration */
static unsigned long flush_stats_sec_start_ms = 0;
static uint32_t flush_count_this_sec = 0;
static uint32_t flushes_per_sec = 0;
static uint32_t avg_flush_ms = 0;
static uint32_t avg_flush_n = 0;
#define AVG_FLUSH_MAX_N 100

void *lvgl_psram_malloc(size_t size) {
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lvgl_psram_free(void *ptr) {
  if (ptr) {
    heap_caps_free(ptr);
  }
}

void *lvgl_psram_realloc(void *ptr, size_t size) {
  return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = 
{
  {0xF0, (uint8_t[]){0x28}, 1, 0},
  {0xF2, (uint8_t[]){0x28}, 1, 0},
  {0x73, (uint8_t[]){0xF0}, 1, 0},
  {0x7C, (uint8_t[]){0xD1}, 1, 0},
  {0x83, (uint8_t[]){0xE0}, 1, 0},
  {0x84, (uint8_t[]){0x61}, 1, 0},
  {0xF2, (uint8_t[]){0x82}, 1, 0},
  {0xF0, (uint8_t[]){0x00}, 1, 0},
  {0xF0, (uint8_t[]){0x01}, 1, 0},
  {0xF1, (uint8_t[]){0x01}, 1, 0},
  {0xB0, (uint8_t[]){0x56}, 1, 0},
  {0xB1, (uint8_t[]){0x4D}, 1, 0},
  {0xB2, (uint8_t[]){0x24}, 1, 0},
  {0xB4, (uint8_t[]){0x87}, 1, 0},
  {0xB5, (uint8_t[]){0x44}, 1, 0},
  {0xB6, (uint8_t[]){0x8B}, 1, 0},
  {0xB7, (uint8_t[]){0x40}, 1, 0},
  {0xB8, (uint8_t[]){0x86}, 1, 0},
  {0xBA, (uint8_t[]){0x00}, 1, 0},
  {0xBB, (uint8_t[]){0x08}, 1, 0},
  {0xBC, (uint8_t[]){0x08}, 1, 0},
  {0xBD, (uint8_t[]){0x00}, 1, 0},
  {0xC0, (uint8_t[]){0x80}, 1, 0},
  {0xC1, (uint8_t[]){0x10}, 1, 0},
  {0xC2, (uint8_t[]){0x37}, 1, 0},
  {0xC3, (uint8_t[]){0x80}, 1, 0},
  {0xC4, (uint8_t[]){0x10}, 1, 0},
  {0xC5, (uint8_t[]){0x37}, 1, 0},
  {0xC6, (uint8_t[]){0xA9}, 1, 0},
  {0xC7, (uint8_t[]){0x41}, 1, 0},
  {0xC8, (uint8_t[]){0x01}, 1, 0},
  {0xC9, (uint8_t[]){0xA9}, 1, 0},
  {0xCA, (uint8_t[]){0x41}, 1, 0},
  {0xCB, (uint8_t[]){0x01}, 1, 0},
  {0xD0, (uint8_t[]){0x91}, 1, 0},
  {0xD1, (uint8_t[]){0x68}, 1, 0},
  {0xD2, (uint8_t[]){0x68}, 1, 0},
  {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
  {0xDD, (uint8_t[]){0x4F}, 1, 0},
  {0xDE, (uint8_t[]){0x4F}, 1, 0},
  {0xF1, (uint8_t[]){0x10}, 1, 0},
  {0xF0, (uint8_t[]){0x00}, 1, 0},
  {0xF0, (uint8_t[]){0x02}, 1, 0},
  {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
  {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
  {0xF0, (uint8_t[]){0x10}, 1, 0},
  {0xF3, (uint8_t[]){0x10}, 1, 0},
  {0xE0, (uint8_t[]){0x07}, 1, 0},
  {0xE1, (uint8_t[]){0x00}, 1, 0},
  {0xE2, (uint8_t[]){0x00}, 1, 0},
  {0xE3, (uint8_t[]){0x00}, 1, 0},
  {0xE4, (uint8_t[]){0xE0}, 1, 0},
  {0xE5, (uint8_t[]){0x06}, 1, 0},
  {0xE6, (uint8_t[]){0x21}, 1, 0},
  {0xE7, (uint8_t[]){0x01}, 1, 0},
  {0xE8, (uint8_t[]){0x05}, 1, 0},
  {0xE9, (uint8_t[]){0x02}, 1, 0},
  {0xEA, (uint8_t[]){0xDA}, 1, 0},
  {0xEB, (uint8_t[]){0x00}, 1, 0},
  {0xEC, (uint8_t[]){0x00}, 1, 0},
  {0xED, (uint8_t[]){0x0F}, 1, 0},
  {0xEE, (uint8_t[]){0x00}, 1, 0},
  {0xEF, (uint8_t[]){0x00}, 1, 0},
  {0xF8, (uint8_t[]){0x00}, 1, 0},
  {0xF9, (uint8_t[]){0x00}, 1, 0},
  {0xFA, (uint8_t[]){0x00}, 1, 0},
  {0xFB, (uint8_t[]){0x00}, 1, 0},
  {0xFC, (uint8_t[]){0x00}, 1, 0},
  {0xFD, (uint8_t[]){0x00}, 1, 0},
  {0xFE, (uint8_t[]){0x00}, 1, 0},
  {0xFF, (uint8_t[]){0x00}, 1, 0},
  {0x60, (uint8_t[]){0x40}, 1, 0},
  {0x61, (uint8_t[]){0x04}, 1, 0},
  {0x62, (uint8_t[]){0x00}, 1, 0},
  {0x63, (uint8_t[]){0x42}, 1, 0},
  {0x64, (uint8_t[]){0xD9}, 1, 0},
  {0x65, (uint8_t[]){0x00}, 1, 0},
  {0x66, (uint8_t[]){0x00}, 1, 0},
  {0x67, (uint8_t[]){0x00}, 1, 0},
  {0x68, (uint8_t[]){0x00}, 1, 0},
  {0x69, (uint8_t[]){0x00}, 1, 0},
  {0x6A, (uint8_t[]){0x00}, 1, 0},
  {0x6B, (uint8_t[]){0x00}, 1, 0},
  {0x70, (uint8_t[]){0x40}, 1, 0},
  {0x71, (uint8_t[]){0x03}, 1, 0},
  {0x72, (uint8_t[]){0x00}, 1, 0},
  {0x73, (uint8_t[]){0x42}, 1, 0},
  {0x74, (uint8_t[]){0xD8}, 1, 0},
  {0x75, (uint8_t[]){0x00}, 1, 0},
  {0x76, (uint8_t[]){0x00}, 1, 0},
  {0x77, (uint8_t[]){0x00}, 1, 0},
  {0x78, (uint8_t[]){0x00}, 1, 0},
  {0x79, (uint8_t[]){0x00}, 1, 0},
  {0x7A, (uint8_t[]){0x00}, 1, 0},
  {0x7B, (uint8_t[]){0x00}, 1, 0},
  {0x80, (uint8_t[]){0x48}, 1, 0},
  {0x81, (uint8_t[]){0x00}, 1, 0},
  {0x82, (uint8_t[]){0x06}, 1, 0},
  {0x83, (uint8_t[]){0x02}, 1, 0},
  {0x84, (uint8_t[]){0xD6}, 1, 0},
  {0x85, (uint8_t[]){0x04}, 1, 0},
  {0x86, (uint8_t[]){0x00}, 1, 0},
  {0x87, (uint8_t[]){0x00}, 1, 0},
  {0x88, (uint8_t[]){0x48}, 1, 0},
  {0x89, (uint8_t[]){0x00}, 1, 0},
  {0x8A, (uint8_t[]){0x08}, 1, 0},
  {0x8B, (uint8_t[]){0x02}, 1, 0},
  {0x8C, (uint8_t[]){0xD8}, 1, 0},
  {0x8D, (uint8_t[]){0x04}, 1, 0},
  {0x8E, (uint8_t[]){0x00}, 1, 0},
  {0x8F, (uint8_t[]){0x00}, 1, 0},
  {0x90, (uint8_t[]){0x48}, 1, 0},
  {0x91, (uint8_t[]){0x00}, 1, 0},
  {0x92, (uint8_t[]){0x0A}, 1, 0},
  {0x93, (uint8_t[]){0x02}, 1, 0},
  {0x94, (uint8_t[]){0xDA}, 1, 0},
  {0x95, (uint8_t[]){0x04}, 1, 0},
  {0x96, (uint8_t[]){0x00}, 1, 0},
  {0x97, (uint8_t[]){0x00}, 1, 0},
  {0x98, (uint8_t[]){0x48}, 1, 0},
  {0x99, (uint8_t[]){0x00}, 1, 0},
  {0x9A, (uint8_t[]){0x0C}, 1, 0},
  {0x9B, (uint8_t[]){0x02}, 1, 0},
  {0x9C, (uint8_t[]){0xDC}, 1, 0},
  {0x9D, (uint8_t[]){0x04}, 1, 0},
  {0x9E, (uint8_t[]){0x00}, 1, 0},
  {0x9F, (uint8_t[]){0x00}, 1, 0},
  {0xA0, (uint8_t[]){0x48}, 1, 0},
  {0xA1, (uint8_t[]){0x00}, 1, 0},
  {0xA2, (uint8_t[]){0x05}, 1, 0},
  {0xA3, (uint8_t[]){0x02}, 1, 0},
  {0xA4, (uint8_t[]){0xD5}, 1, 0},
  {0xA5, (uint8_t[]){0x04}, 1, 0},
  {0xA6, (uint8_t[]){0x00}, 1, 0},
  {0xA7, (uint8_t[]){0x00}, 1, 0},
  {0xA8, (uint8_t[]){0x48}, 1, 0},
  {0xA9, (uint8_t[]){0x00}, 1, 0},
  {0xAA, (uint8_t[]){0x07}, 1, 0},
  {0xAB, (uint8_t[]){0x02}, 1, 0},
  {0xAC, (uint8_t[]){0xD7}, 1, 0},
  {0xAD, (uint8_t[]){0x04}, 1, 0},
  {0xAE, (uint8_t[]){0x00}, 1, 0},
  {0xAF, (uint8_t[]){0x00}, 1, 0},
  {0xB0, (uint8_t[]){0x48}, 1, 0},
  {0xB1, (uint8_t[]){0x00}, 1, 0},
  {0xB2, (uint8_t[]){0x09}, 1, 0},
  {0xB3, (uint8_t[]){0x02}, 1, 0},
  {0xB4, (uint8_t[]){0xD9}, 1, 0},
  {0xB5, (uint8_t[]){0x04}, 1, 0},
  {0xB6, (uint8_t[]){0x00}, 1, 0},
  {0xB7, (uint8_t[]){0x00}, 1, 0},
  {0xB8, (uint8_t[]){0x48}, 1, 0},
  {0xB9, (uint8_t[]){0x00}, 1, 0},
  {0xBA, (uint8_t[]){0x0B}, 1, 0},
  {0xBB, (uint8_t[]){0x02}, 1, 0},
  {0xBC, (uint8_t[]){0xDB}, 1, 0},
  {0xBD, (uint8_t[]){0x04}, 1, 0},
  {0xBE, (uint8_t[]){0x00}, 1, 0},
  {0xBF, (uint8_t[]){0x00}, 1, 0},
  {0xC0, (uint8_t[]){0x10}, 1, 0},
  {0xC1, (uint8_t[]){0x47}, 1, 0},
  {0xC2, (uint8_t[]){0x56}, 1, 0},
  {0xC3, (uint8_t[]){0x65}, 1, 0},
  {0xC4, (uint8_t[]){0x74}, 1, 0},
  {0xC5, (uint8_t[]){0x88}, 1, 0},
  {0xC6, (uint8_t[]){0x99}, 1, 0},
  {0xC7, (uint8_t[]){0x01}, 1, 0},
  {0xC8, (uint8_t[]){0xBB}, 1, 0},
  {0xC9, (uint8_t[]){0xAA}, 1, 0},
  {0xD0, (uint8_t[]){0x10}, 1, 0},
  {0xD1, (uint8_t[]){0x47}, 1, 0},
  {0xD2, (uint8_t[]){0x56}, 1, 0},
  {0xD3, (uint8_t[]){0x65}, 1, 0},
  {0xD4, (uint8_t[]){0x74}, 1, 0},
  {0xD5, (uint8_t[]){0x88}, 1, 0},
  {0xD6, (uint8_t[]){0x99}, 1, 0},
  {0xD7, (uint8_t[]){0x01}, 1, 0},
  {0xD8, (uint8_t[]){0xBB}, 1, 0},
  {0xD9, (uint8_t[]){0xAA}, 1, 0},
  {0xF3, (uint8_t[]){0x01}, 1, 0},
  {0xF0, (uint8_t[]){0x00}, 1, 0},
  {0x21, (uint8_t[]){0x00}, 1, 0},
  {0x11, (uint8_t[]){0x00}, 1, 120},
  {0x29, (uint8_t[]){0x00}, 1, 0},
#ifdef EXAMPLE_Rotate_90
  {0x36, (uint8_t[]){0x60}, 1, 0},  // 90 degrees
#elif defined(EXAMPLE_Rotate_180)
  {0x36, (uint8_t[]){0xC0}, 1, 0},  // 180 degrees
#else
  {0x36, (uint8_t[]){0x00}, 1, 0},  // 0 degrees (normal)
#endif
};

void lcd_lvgl_Init(void)
{
  static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
  static lv_disp_drv_t disp_drv;      // contains callback functions

  const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_PCLK,
                                                               EXAMPLE_PIN_NUM_LCD_DATA0,
                                                               EXAMPLE_PIN_NUM_LCD_DATA1,
                                                               EXAMPLE_PIN_NUM_LCD_DATA2,
                                                               EXAMPLE_PIN_NUM_LCD_DATA3,
                                                               EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * LCD_BIT_PER_PIXEL / 8);
  ESP_ERROR_CHECK_WITHOUT_ABORT(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
  esp_lcd_panel_io_handle_t io_handle = NULL;

  const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS,
                                                                              example_notify_lvgl_flush_ready,
                                                                              &disp_drv);

  sh8601_vendor_config_t vendor_config = 
  {
    .init_cmds = lcd_init_cmds,
    .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
    .flags = 
    {
      .use_qspi_interface = 1,
    },
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
  amoled_panel_io_handle = io_handle;
  printf("[LCD_BSP] panel_io created trans_queue_depth=%d\n", SH8601_PANEL_IO_QSPI_TRANS_QUEUE_DEPTH);
  esp_lcd_panel_handle_t panel_handle = NULL;
  const esp_lcd_panel_dev_config_t panel_config = 
  {
    .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = LCD_BIT_PER_PIXEL,
    .vendor_config = &vendor_config,
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
  amoled_panel_handle = panel_handle;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_reset(panel_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_init(panel_handle));
  //ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_disp_on_off(panel_handle, true));

  lv_init();
  size_t buf_height = EXAMPLE_LVGL_BUF_HEIGHT;
  const size_t min_buf_height = 10;
  size_t buf_pixels = 0;
  size_t buf_bytes = 0;
  lv_color_t *buf1 = NULL;
  lv_color_t *buf2 = NULL;
  while (buf_height >= min_buf_height) {
    buf_pixels = EXAMPLE_LCD_H_RES * buf_height;
    buf_bytes = buf_pixels * sizeof(lv_color_t);
    buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1) {
      printf("[LVGL] PSRAM alloc failed for buf1; falling back to DMA\n");
      buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    }
    if (!buf1) {
      printf("[LVGL] alloc failed for buf1 at height=%u; retrying smaller\n",
             (unsigned)buf_height);
      buf_height /= 2;
      continue;
    }
    buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf2) {
      printf("[LVGL] PSRAM alloc failed for buf2; falling back to DMA\n");
      buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    }
    if (!buf2) {
      printf("[LVGL] alloc failed for buf2 at height=%u; using single buffer\n",
             (unsigned)buf_height);
    }
    break;
  }
  if (!buf1) {
    printf("[LVGL] alloc failed for all buffer sizes; aborting LVGL init\n");
    return;
  }
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_pixels);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.rounder_cb = example_lvgl_rounder_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.user_data = panel_handle;
  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

  #ifdef EXAMPLE_Rotate_180
  // Set LVGL display rotation to 180 degrees (this will auto-transform touch coordinates)
  lv_disp_set_rotation(disp, LV_DISP_ROT_180);
  #endif

  static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.disp = disp;
  indev_drv.read_cb = example_lvgl_touch_cb;
  lv_indev_drv_register(&indev_drv);

  const esp_timer_create_args_t lvgl_tick_timer_args = 
  {
    .callback = &example_increase_lvgl_tick,
    .name = "lvgl_tick"
  };
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  lvgl_mux = xSemaphoreCreateMutex(); //mutex semaphores
  assert(lvgl_mux);
  
  if (example_lvgl_lock(-1)) 
  {   
   ui_init();     /* A widgets example */
    //lv_demo_music();        /* A modern, smartphone-like music player demo. */
    //lv_demo_stress();       /* A stress test for LVGL. */
    //lv_demo_benchmark();    /* A demo to measure the performance of LVGL or to compare different settings. */

    // Release the mutex
    example_lvgl_unlock();
  }
}

void lcd_lvgl_wait_tx_done(uint32_t timeout_ms) {
  if (!amoled_panel_io_handle) {
    return;
  }
  if (esp_lcd_panel_io_wait_tx_done) {
    esp_lcd_panel_io_wait_tx_done(amoled_panel_io_handle, (int)timeout_ms);
  } else {
    // Fallback: small delay to let SPI queue drain on older IDF
    delay(10);
  }
}

void lcd_panel_set_power(bool on) {
  if (amoled_panel_handle) {
    esp_lcd_panel_disp_on_off(amoled_panel_handle, on);
  }
}

void lcd_bsp_check_flush_timeout(void) {
  if (s_flush_pending_drv == NULL) {
    return;
  }
  unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
  if ((now_ms - s_flush_start_ms) < (unsigned long)FLUSH_TIMEOUT_MS) {
    return;
  }
  if (flush_outstanding_count > 0) flush_outstanding_count--;
  printf("[LCD_FLUSH] timeout %lums - forcing flush_ready outstanding=%lu queue_depth=%d\n",
         (unsigned long)FLUSH_TIMEOUT_MS, (unsigned long)flush_outstanding_count, SH8601_PANEL_IO_QSPI_TRANS_QUEUE_DEPTH);
  lv_disp_flush_ready(s_flush_pending_drv);
  s_flush_pending_drv = NULL;
  flush_fail_count++;
  if (flush_fail_count >= FLUSH_FAIL_RESET_THRESHOLD) {
    display_reset_requested = true;
    printf("[LCD_FLUSH] flush_fail_count=%lu >= %d, display reset requested\n",
           (unsigned long)flush_fail_count, FLUSH_FAIL_RESET_THRESHOLD);
  }
}

bool lcd_bsp_display_reset_requested(void) {
  bool r = display_reset_requested;
  display_reset_requested = false;
  return r;
}

void lcd_bsp_reset_flush_fail_count(void) {
  flush_fail_count = 0;
}

unsigned long lcd_bsp_flush_inflight_age_ms(void) {
  if (s_flush_pending_drv == NULL) {
    return 0;
  }
  unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
  return (now_ms >= s_flush_start_ms) ? (now_ms - s_flush_start_ms) : 0;
}

uint32_t lcd_bsp_get_flush_outstanding(void) {
  return flush_outstanding_count;
}

void lcd_bsp_get_flush_submit_stats(uint32_t *ok, uint32_t *fail, int *outstanding, int *soft_fault) {
  if (ok) *ok = s_flush_submit_ok;
  if (fail) *fail = s_flush_submit_fail;
  if (outstanding) *outstanding = (int)flush_outstanding_count;
  if (soft_fault) *soft_fault = s_flush_soft_fault;
}

void lcd_bsp_clear_flush_soft_fault(void) {
  s_flush_soft_fault = 0;
}

uint32_t lcd_bsp_get_flush_fail_count(void) {
  return flush_fail_count;
}

void lcd_bsp_reset_panel(void) {
  if (amoled_panel_handle != NULL) {
    esp_lcd_panel_reset(amoled_panel_handle);
    printf("[LCD_BSP] panel reset done\n");
  }
}

uint32_t lcd_bsp_get_avg_flush_ms(void) {
  return avg_flush_ms;
}

uint32_t lcd_bsp_get_flushes_per_sec(void) {
  return flushes_per_sec;
}

void lcd_panel_deinit(void) {
  if (amoled_panel_handle) {
    esp_lcd_panel_del(amoled_panel_handle);
    amoled_panel_handle = NULL;
  }
  if (amoled_panel_io_handle) {
    esp_lcd_panel_io_del(amoled_panel_io_handle);
    amoled_panel_io_handle = NULL;
  }
  spi_bus_free(LCD_HOST);
}

bool example_lvgl_lock(int timeout_ms)
{
  assert(lvgl_mux && "bsp_display_start must be called first");

  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void example_lvgl_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}

bool lvgl_lock_held_by_current_task(void)
{
  if (lvgl_mux == NULL) {
    return false;
  }
  return (xSemaphoreGetMutexHolder(lvgl_mux) == xTaskGetCurrentTaskHandle());
}

void lvgl_assert_locked(void)
{
#ifndef NDEBUG
  if (!lvgl_lock_held_by_current_task()) {
    printf("[LVGL] ERROR: LVGL API called without holding lvgl lock (call from wrong task?)\n");
    assert(0 && "LVGL must be called with lvgl lock held");
  }
#endif
}

static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}
static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
  if (flush_outstanding_count > 0) {
    flush_outstanding_count--;
  }
  s_flush_pending_drv = NULL;
  lv_disp_flush_ready(disp_driver);
  return false;
}
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  if (g_sleep_transition) {
    lv_disp_flush_ready(drv);
    return;
  }
  if (!drv || !area || !color_map) {
    if (drv) lv_disp_flush_ready(drv);
    return;
  }
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  if (!panel_handle) {
    lv_disp_flush_ready(drv);
    return;
  }
  const int offsetx1 = area->x1;
  const int offsetx2 = area->x2;
  const int offsety1 = area->y1;
  const int offsety2 = area->y2;

  s_flush_pending_drv = drv;
  s_flush_start_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
  flush_outstanding_count++;
  esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
  if (ret != ESP_OK) {
    if (flush_outstanding_count > 0) flush_outstanding_count--;
    s_flush_submit_fail++;
    s_flush_soft_fault = 1;
    printf("[LCD_FLUSH] panel_io draw_bitmap failed err=0x%x outstanding=%lu queue_depth=%d\n",
           (unsigned)ret, (unsigned long)flush_outstanding_count, SH8601_PANEL_IO_QSPI_TRANS_QUEUE_DEPTH);
    s_flush_pending_drv = NULL;
    lv_disp_flush_ready(drv);
    flush_fail_count++;
    if (flush_fail_count >= FLUSH_FAIL_RESET_THRESHOLD) {
      display_reset_requested = true;
      printf("[LCD_FLUSH] flush_fail_count=%lu >= %d, display reset requested\n",
             (unsigned long)flush_fail_count, FLUSH_FAIL_RESET_THRESHOLD);
    }
    return;
  }
  s_flush_submit_ok++;
}
void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
  if (!area) return;
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;

  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  // round the start of coordinate down to the nearest 2M number
  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  // round the end of coordinate up to the nearest 2N+1 number
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  uint16_t tp_x,tp_y;
  uint8_t win = getTouch(&tp_x,&tp_y);
  if (win)
  {
    #ifdef EXAMPLE_Rotate_90
      data->point.x = tp_y;
      data->point.y = (EXAMPLE_LCD_V_RES - tp_x);
    #elif defined(EXAMPLE_Rotate_180)
      // For 180-degree rotation: LVGL handles display rotation automatically,
      // so we use touch coordinates directly without flipping
      data->point.x = tp_x;
      data->point.y = tp_y;
    #else
      data->point.x = tp_x;
      data->point.y = tp_y;
    #endif
    if(data->point.x > EXAMPLE_LCD_H_RES)
    data->point.x = EXAMPLE_LCD_H_RES;
    if(data->point.y > EXAMPLE_LCD_V_RES)
    data->point.y = EXAMPLE_LCD_V_RES;
    data->state = LV_INDEV_STATE_PRESSED;
    //ESP_LOGE("TP","(%d,%d)",data->point.x,data->point.y);
  }
  else
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}
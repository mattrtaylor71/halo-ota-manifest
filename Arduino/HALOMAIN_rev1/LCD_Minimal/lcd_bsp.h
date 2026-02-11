#ifndef LCD_BSP_H
#define LCD_BSP_H
#include "Arduino.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "lvgl.h"
/*#include <lv_demo.h>*/
#include "esp_check.h"
#include "driver/gpio.h"
#include "ui.h"
#ifdef __cplusplus
extern "C" {
#endif 

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area);
static void example_increase_lvgl_tick(void *arg);
static void example_lvgl_port_task(void *arg);
void example_lvgl_unlock(void);
bool example_lvgl_lock(int timeout_ms);
bool lvgl_lock_held_by_current_task(void);
void lvgl_assert_locked(void);
void lcd_lvgl_Init(void);
void lcd_lvgl_wait_tx_done(uint32_t timeout_ms);
void lcd_panel_set_power(bool on);
void lcd_panel_deinit(void);
void lcd_bsp_check_flush_timeout(void);
bool lcd_bsp_display_reset_requested(void);
void lcd_bsp_reset_flush_fail_count(void);
unsigned long lcd_bsp_flush_inflight_age_ms(void);
uint32_t lcd_bsp_get_flush_outstanding(void);
void lcd_bsp_get_flush_submit_stats(uint32_t *ok, uint32_t *fail, int *outstanding, int *soft_fault);
void lcd_bsp_clear_flush_soft_fault(void);
uint32_t lcd_bsp_get_flush_fail_count(void);
void lcd_bsp_reset_panel(void);
uint32_t lcd_bsp_get_avg_flush_ms(void);
uint32_t lcd_bsp_get_flushes_per_sec(void);
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);
#ifdef __cplusplus
}
#endif

#endif
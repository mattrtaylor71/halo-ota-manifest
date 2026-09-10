#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "lcd_config.h"
#include "ui.h"
#include "bidi_switch_knob.h"
#include "navigation.h"

void setup() {
  Serial.begin(115200);

  // Initialize display and LVGL
  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  // Initialize SquareLine UI
  ui_init();

  // Rotate display 180 degrees
  lv_disp_t *disp = lv_disp_get_default();
  lv_disp_set_rotation(disp, LV_DISP_ROT_180);
  Serial.println("Display rotated 180 degrees");

  // Allow LVGL to process initial setup and render first screen
  for (int i = 0; i < 20; i++) {
    lv_timer_handler();
    delay(10);
  }
  
  // First screen in storyboard order should be displayed
  // This will be synced by nav_init() if needed
  Serial.println("Frame_439_1 screen should be displayed");

  // Initialize navigation (includes knob initialization)
  nav_init();

  Serial.println("LVGL + SquareLine UI started.");
}

void loop() {
  // LVGL requires periodic tick handling
  lv_timer_handler();
  // Process delay timers for automatic screen transitions
  nav_process_delay_timers();
  delay(5);
}

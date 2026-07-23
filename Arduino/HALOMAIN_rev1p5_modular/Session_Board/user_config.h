#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// Minimal config for Session_Board — only the I2S pins audio_bsp.c needs.
// (Copied from LCD_Minimal/user_config.h; WiFi/Trepo-API/owner-id defines omitted.)

// I2S DAC (PCM5100A) — unused by the session board (speaker muted via volume 0)
// but audio_bsp.c initializes it; keep the pins so the driver builds/runs.
#define EXAMPLE_I2S_STD_BCLK_PIN    (gpio_num_t)39   // I2S bit clock
#define EXAMPLE_I2S_STD_WS_PIN      (gpio_num_t)40   // I2S word select
#define EXAMPLE_I2S_STD_DOUT_PIN    (gpio_num_t)41   // I2S data out

// PDM microphone (the one we record from)
#define EXAMPLE_I2S_PDM_DATA_PIN    (gpio_num_t)46
#define EXAMPLE_I2S_PDM_CLK_PIN     (gpio_num_t)45

#endif

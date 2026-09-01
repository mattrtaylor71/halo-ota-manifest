#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// I2S PDM Microphone pins for XIAO ESP32S3 Sense
// According to hardware documentation: PDM clock = GPIO42, PDM data = GPIO41
// Note: These pins are also used by camera (XCLK, SIOD) but camera is off by default
#define EXAMPLE_I2S_PDM_DATA_PIN    (gpio_num_t)41  // MTDI / PDM_DATA
#define EXAMPLE_I2S_PDM_CLK_PIN     (gpio_num_t)42  // MTMS / PDM_CLK

// I2S standard pins (for speaker output, not used during mic recording)
// Note: STD_DOUT shares GPIO41 with PDM_DATA, but they're on different I2S controllers (I2S_NUM_1 vs I2S_NUM_0)
// so there's no conflict - speaker output is disabled during recording anyway
#define EXAMPLE_I2S_STD_BCLK_PIN    (gpio_num_t)39
#define EXAMPLE_I2S_STD_WS_PIN      (gpio_num_t)40
#define EXAMPLE_I2S_STD_DOUT_PIN    (gpio_num_t)41

// Quick-Ack API Configuration (OpenAI Realtime API)
#define QUICK_ACK_BASE_URL "https://qq5tn5i3t3.execute-api.us-east-1.amazonaws.com"
#define QUICK_ACK_ENDPOINT "/voice-ack"

#endif


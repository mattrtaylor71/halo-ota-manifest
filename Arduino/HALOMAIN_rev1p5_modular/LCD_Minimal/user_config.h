#ifndef USER_CONFIG_H
#define USER_CONFIG_H


//i2s
#define EXAMPLE_I2S_STD_BCLK_PIN    (gpio_num_t)39//(gpio_num_t)48//(gpio_num_t)39    // I2S bit clock io number
#define EXAMPLE_I2S_STD_WS_PIN      (gpio_num_t)40//(gpio_num_t)38//(gpio_num_t)40  // I2S word select io number
#define EXAMPLE_I2S_STD_DOUT_PIN    (gpio_num_t)41//(gpio_num_t)47//(gpio_num_t)41   // I2S data out io number


#define EXAMPLE_I2S_PDM_DATA_PIN    (gpio_num_t)46
#define EXAMPLE_I2S_PDM_CLK_PIN     (gpio_num_t)45


//encoder 

#define EXAMPLE_ENCODER_ECA_PIN    8
#define EXAMPLE_ENCODER_ECB_PIN    7


//bit

#define SET_BIT(reg,bit) (reg |= ((uint32_t)0x01<<bit))
#define CLEAR_BIT(reg,bit) (reg &= (~((uint32_t)0x01<<bit)))
#define READ_BIT(reg,bit) (((uint32_t)reg>>bit) & 0x01)
#define BIT_EVEN_ALL (0x00ffffff)

// WiFi Configuration
// Set your WiFi network name (SSID) and password here
//#define WIFI_SSID "NETGEAR99"
//#define WIFI_PASSWORD "niftycello688"
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// Trepo API Configuration
// Base URL for Trepo API Gateway
#define TREPO_API_BASE_URL "https://1zc0nh8x48.execute-api.us-east-1.amazonaws.com"
#define TREPO_VOICE_ENDPOINT "/v1/voice"
#define TREPO_LIST_ENDPOINT "/v1/list"

// Quick-Ack API Configuration (OpenAI Realtime API)
#define QUICK_ACK_BASE_URL "https://qq5tn5i3t3.execute-api.us-east-1.amazonaws.com"
#define QUICK_ACK_ENDPOINT "/voice-ack"

// Trepo User/Device Configuration
// ownerId: UUID from new_users.user_id table (must be valid UUID format)
// device: Device identifier string (e.g. "halo-fridge-01")
#define TREPO_OWNER_ID "7d7df434-d942-4037-b054-2d3005ea6abc"
#define TREPO_DEVICE_ID "*"  // Use "*" to fetch from all devices for this owner

#endif
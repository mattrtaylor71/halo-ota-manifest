#include "qr_display.h"
#include <Arduino.h>

// CRITICAL: Include lv_qrcode.h BEFORE lvgl.h
// lv_qrcode.h will force enable LV_USE_QRCODE before including lvgl.h
#include "../third_party/lv_qrcode.h"
// lvgl.h is already included by lv_qrcode.h, but include it here for other LVGL functions
#include <lvgl.h>
#include <esp_log.h>
#include <sstream>
#include <cstring>

static const char *TAG = "QR_DISPLAY";

std::string QRDisplay::generateWifiQRData(const std::string &ssid, const std::string &password) {
    // Format: WIFI:T:WPA;S:<SSID>;P:<PASSWORD>;;
    std::stringstream ss;
    ss << "WIFI:T:WPA;S:" << ssid << ";P:" << password << ";;";
    return ss.str();
}

// Create QR code widget using LVGL's built-in QR widget
lv_obj_t* QRDisplay::createQRCodeWidget(lv_obj_t *parent, const std::string &data, int size) {
    Serial.print("Creating QR code widget, size=");
    Serial.print(size);
    Serial.print(", data_len=");
    Serial.println(data.length());
    Serial.print("QR data: ");
    Serial.println(data.c_str());
    
    ESP_LOGI(TAG, "Creating QR code widget, size=%d, data_len=%d", size, data.length());
    ESP_LOGI(TAG, "QR data: %s", data.c_str());
    
    // Create QR code widget using LVGL's built-in QR widget
    // Standard black QR code on white background for optimal camera readability
    // Phone cameras are optimized for black-on-white QR codes, which provides:
    // - Better auto-focus performance
    // - More reliable auto-exposure
    // - Higher contrast detection by scanner algorithms
    lv_color_t dark_color = lv_color_hex(0x000000);  // Pure black for QR modules
    lv_color_t light_color = lv_color_hex(0xFFFFFF);  // Pure white background
    
    lv_obj_t *qr = lv_qrcode_create(parent, size, dark_color, light_color);
    
    if (qr == nullptr) {
        ESP_LOGE(TAG, "Failed to create QR code widget");
        Serial.println("ERROR: Failed to create QR code widget");
        // Return a simple error label instead
        lv_obj_t *errorLabel = lv_label_create(parent);
        lv_label_set_text(errorLabel, "QR Error");
        lv_obj_center(errorLabel);
        return errorLabel;
    }
    
    // Update QR code with data
    lv_res_t res = lv_qrcode_update(qr, data.c_str(), data.length());
    
    if (res != LV_RES_OK) {
        ESP_LOGE(TAG, "Failed to update QR code, error: %d", res);
        Serial.print("ERROR: Failed to update QR code, error: ");
        Serial.println(res);
        // Keep the widget but log the error
    } else {
        Serial.println("QR code generated successfully");
        ESP_LOGI(TAG, "QR code generated successfully");
    }
    
    // Center the QR code
    lv_obj_center(qr);
    
    return qr;
}

void QRDisplay::updateQRCodeWidget(lv_obj_t *qrWidget, const std::string &data) {
    if (qrWidget == nullptr) {
        ESP_LOGE(TAG, "QR widget is null");
        Serial.println("ERROR: QR widget is null");
        return;
    }
    
    ESP_LOGI(TAG, "Updating QR code widget with new data");
    Serial.println("Updating QR code widget");
    
    // Update QR code data using LVGL's update function
    lv_res_t res = lv_qrcode_update(qrWidget, data.c_str(), data.length());
    
    if (res != LV_RES_OK) {
        ESP_LOGE(TAG, "Failed to update QR code, error: %d", res);
        Serial.print("ERROR: Failed to update QR code, error: ");
        Serial.println(res);
    } else {
        Serial.println("QR code updated successfully");
        ESP_LOGI(TAG, "QR code updated successfully");
    }
}


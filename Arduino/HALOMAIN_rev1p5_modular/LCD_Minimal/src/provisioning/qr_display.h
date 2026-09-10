#ifndef QR_DISPLAY_H
#define QR_DISPLAY_H

#include <string>
#include <Arduino.h>
// Forward declare lv_obj_t instead of including lvgl.h here
// This prevents lvgl.h from being included before LV_USE_QRCODE is forced
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace QRDisplay {
    // Generate Wi-Fi QR code data string (iOS format)
    std::string generateWifiQRData(const std::string &ssid, const std::string &password);
    
    // Show QR code on LVGL screen (returns QR widget object)
    lv_obj_t* createQRCodeWidget(lv_obj_t *parent, const std::string &data, int size);
    
    // Update existing QR code widget
    void updateQRCodeWidget(lv_obj_t *qrWidget, const std::string &data);
}

#endif // QR_DISPLAY_H


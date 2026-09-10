#include "OtaStateDiag.h"
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

static const char* otaStateToString(uint32_t state) {
  switch (state) {
    case 0x0U:  return "ESP_OTA_IMG_NEW";
    case 0x1U:  return "ESP_OTA_IMG_PENDING_VERIFY";
    case 0x2U:  return "ESP_OTA_IMG_VALID";
    case 0x3U:  return "ESP_OTA_IMG_INVALID";
    case 0x4U:  return "ESP_OTA_IMG_ABORTED";
    case 0xFFFFFFFFU: return "ESP_OTA_IMG_UNDEFINED";
    default:    return "(unknown)";
  }
}

void otaStateDiagPrint(void) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

  Serial.println("[OTA_STATE_DIAG] --- partition info ---");
  if (running) {
    Serial.printf("[OTA_STATE_DIAG] running label=%s address=0x%lx size=%lu\n",
                  running->label, (unsigned long)running->address, (unsigned long)running->size);
  } else {
    Serial.println("[OTA_STATE_DIAG] running=(null)");
  }

  if (boot) {
    Serial.printf("[OTA_STATE_DIAG] boot label=%s address=0x%lx\n",
                  boot->label, (unsigned long)boot->address);
  } else {
    Serial.println("[OTA_STATE_DIAG] boot=(null)");
  }

  if (next) {
    Serial.printf("[OTA_STATE_DIAG] next_update label=%s address=0x%lx\n",
                  next->label, (unsigned long)next->address);
  } else {
    Serial.println("[OTA_STATE_DIAG] next_update=(null)");
  }

  Serial.println("[OTA_STATE_DIAG] --- OTA image state (running partition) ---");
  if (running) {
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK) {
      uint32_t raw = (uint32_t)state;
      Serial.printf("[OTA_STATE_DIAG] ota_state raw=%lu (%s)\n",
                    (unsigned long)raw, otaStateToString(raw));
    } else {
      Serial.printf("[OTA_STATE_DIAG] ota_state esp_ota_get_state_partition failed: %s\n", esp_err_to_name(err));
    }
  }

  Serial.println("[OTA_STATE_DIAG] --- compile-time rollback config ---");
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  Serial.printf("[OTA_STATE_DIAG] CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=%d (defined)\n", (int)CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE);
#else
  Serial.println("[OTA_STATE_DIAG] CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE missing (e.g. Arduino build)");
#endif
  Serial.println("[OTA_STATE_DIAG] --- end ---");
}

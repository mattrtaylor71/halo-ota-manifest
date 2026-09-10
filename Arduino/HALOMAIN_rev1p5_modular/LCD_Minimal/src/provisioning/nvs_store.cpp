#include "nvs_store.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_log.h>

static const char *TAG = "NVS_STORE";
static const char *NVS_NAMESPACE = "provisioning";

static const char *KEY_AP_SSID = "ap_ssid";
static const char *KEY_AP_PASS = "ap_pass";
static const char *KEY_HOME_SSID = "home_ssid";
static const char *KEY_HOME_PASS = "home_pass";
static const char *KEY_PROVISIONED = "provisioned";
static const char *KEY_OWNER_ID = "owner_id";

bool NVSStore::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    
    ESP_LOGI(TAG, "NVS initialized");
    return true;
}

bool NVSStore::loadHomeWifiCreds(std::string &ssid, std::string &password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    size_t required_size = 0;
    
    // Get SSID
    err = nvs_get_str(nvs_handle, KEY_HOME_SSID, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs_handle);
        return false;
    }
    char *ssid_buf = (char *)malloc(required_size);
    err = nvs_get_str(nvs_handle, KEY_HOME_SSID, ssid_buf, &required_size);
    if (err != ESP_OK) {
        free(ssid_buf);
        nvs_close(nvs_handle);
        return false;
    }
    ssid = std::string(ssid_buf);
    free(ssid_buf);
    
    // Get password
    required_size = 0;
    err = nvs_get_str(nvs_handle, KEY_HOME_PASS, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs_handle);
        return false;
    }
    char *pass_buf = (char *)malloc(required_size);
    err = nvs_get_str(nvs_handle, KEY_HOME_PASS, pass_buf, &required_size);
    if (err != ESP_OK) {
        free(pass_buf);
        nvs_close(nvs_handle);
        return false;
    }
    password = std::string(pass_buf);
    free(pass_buf);
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded home Wi-Fi creds: SSID=%s", ssid.c_str());
    return true;
}

void NVSStore::saveHomeWifiCreds(const std::string &ssid, const std::string &password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    
    err = nvs_set_str(nvs_handle, KEY_HOME_SSID, ssid.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save home SSID: %s", esp_err_to_name(err));
    }
    
    err = nvs_set_str(nvs_handle, KEY_HOME_PASS, password.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save home password: %s", esp_err_to_name(err));
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Saved home Wi-Fi creds: SSID=%s", ssid.c_str());
}

bool NVSStore::loadApCreds(std::string &ssid, std::string &password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    size_t required_size = 0;
    
    // Get SSID
    err = nvs_get_str(nvs_handle, KEY_AP_SSID, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs_handle);
        return false;
    }
    char *ssid_buf = (char *)malloc(required_size);
    err = nvs_get_str(nvs_handle, KEY_AP_SSID, ssid_buf, &required_size);
    if (err != ESP_OK) {
        free(ssid_buf);
        nvs_close(nvs_handle);
        return false;
    }
    ssid = std::string(ssid_buf);
    free(ssid_buf);
    
    // Get password
    required_size = 0;
    err = nvs_get_str(nvs_handle, KEY_AP_PASS, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs_handle);
        return false;
    }
    char *pass_buf = (char *)malloc(required_size);
    err = nvs_get_str(nvs_handle, KEY_AP_PASS, pass_buf, &required_size);
    if (err != ESP_OK) {
        free(pass_buf);
        nvs_close(nvs_handle);
        return false;
    }
    password = std::string(pass_buf);
    free(pass_buf);
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded AP creds: SSID=%s", ssid.c_str());
    return true;
}

void NVSStore::saveApCreds(const std::string &ssid, const std::string &password) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    
    err = nvs_set_str(nvs_handle, KEY_AP_SSID, ssid.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save AP SSID: %s", esp_err_to_name(err));
    }
    
    err = nvs_set_str(nvs_handle, KEY_AP_PASS, password.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save AP password: %s", esp_err_to_name(err));
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Saved AP creds: SSID=%s", ssid.c_str());
}

bool NVSStore::isProvisioned() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return false;
    }
    
    uint8_t provisioned = 0;
    err = nvs_get_u8(nvs_handle, KEY_PROVISIONED, &provisioned);
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        return false;
    }
    
    return provisioned == 1;
}

void NVSStore::setProvisioned(bool value) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    
    uint8_t provisioned = value ? 1 : 0;
    err = nvs_set_u8(nvs_handle, KEY_PROVISIONED, provisioned);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set provisioned flag: %s", esp_err_to_name(err));
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Set provisioned=%d", provisioned);
}

void NVSStore::clearHomeWifiCreds() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    
    // Erase the home Wi-Fi credential keys
    err = nvs_erase_key(nvs_handle, KEY_HOME_SSID);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase home SSID: %s", esp_err_to_name(err));
    }
    
    err = nvs_erase_key(nvs_handle, KEY_HOME_PASS);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase home password: %s", esp_err_to_name(err));
    }
    
    // Also clear the provisioned flag
    err = nvs_erase_key(nvs_handle, KEY_PROVISIONED);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase provisioned flag: %s", esp_err_to_name(err));
    }
    
    // Also clear the owner ID when clearing Wi-Fi credentials
    // This ensures a clean slate when re-provisioning
    err = nvs_erase_key(nvs_handle, KEY_OWNER_ID);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to erase owner ID: %s", esp_err_to_name(err));
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Cleared home Wi-Fi credentials, provisioned flag, and owner ID");
}

bool NVSStore::loadOwnerId(std::string &ownerId) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }
    
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, KEY_OWNER_ID, NULL, &required_size);
    if (err != ESP_OK || required_size == 0) {
        nvs_close(nvs_handle);
        return false;
    }
    
    char *owner_id_buf = (char *)malloc(required_size);
    err = nvs_get_str(nvs_handle, KEY_OWNER_ID, owner_id_buf, &required_size);
    if (err != ESP_OK) {
        free(owner_id_buf);
        nvs_close(nvs_handle);
        return false;
    }
    
    ownerId = std::string(owner_id_buf);
    free(owner_id_buf);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded owner ID: %s", ownerId.c_str());
    return true;
}

void NVSStore::saveOwnerId(const std::string &ownerId) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    
    err = nvs_set_str(nvs_handle, KEY_OWNER_ID, ownerId.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save owner ID: %s", esp_err_to_name(err));
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Saved owner ID: %s", ownerId.c_str());
}

void NVSStore::clearAllProvisioningData() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }
    
    // Erase all provisioning-related keys
    nvs_erase_key(nvs_handle, KEY_HOME_SSID);
    nvs_erase_key(nvs_handle, KEY_HOME_PASS);
    nvs_erase_key(nvs_handle, KEY_PROVISIONED);
    nvs_erase_key(nvs_handle, KEY_OWNER_ID);
    // Note: We keep AP credentials so the SoftAP SSID/password remain consistent
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Cleared all provisioning data");
}


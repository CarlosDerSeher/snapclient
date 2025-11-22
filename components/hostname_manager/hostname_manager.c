/**
 * @file hostname_manager.c
 * @brief Hostname management implementation
 */

#include "hostname_manager.h"

#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "HOSTNAME";
static const char *NVS_NAMESPACE = "hostname";
static const char *NVS_KEY = "name";

// Mutex for thread-safe NVS access
static SemaphoreHandle_t hostname_mutex = NULL;

/**
 * @brief Validate hostname according to RFC 1123
 */
static bool validate_hostname(const char *hostname) {
    if (!hostname) {
        return false;
    }
    
    size_t len = strlen(hostname);
    
    // Length check: 1-63 characters
    if (len == 0 || len > 63) {
        return false;
    }
    
    // Cannot start or end with hyphen
    if (hostname[0] == '-' || hostname[len - 1] == '-') {
        return false;
    }
    
    // Check each character: must be alphanumeric or hyphen
    for (size_t i = 0; i < len; i++) {
        char c = hostname[i];
        if (!isalnum((unsigned char)c) && c != '-') {
            return false;
        }
    }
    
    return true;
}

esp_err_t hostname_manager_init(void) {
    if (hostname_mutex == NULL) {
        hostname_mutex = xSemaphoreCreateMutex();
        if (hostname_mutex == NULL) {
            ESP_LOGE(TAG, "%s: Failed to create mutex", __func__);
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "%s: Hostname manager initialized", __func__);
    return ESP_OK;
}

esp_err_t hostname_get(char *hostname, size_t max_len) {
    if (!hostname || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Hostname manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Acquire mutex
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t err = ESP_OK;
    nvs_handle_t nvs_handle;
    
    // Try to open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        // Try to read hostname from NVS
        size_t required_size = max_len;
        err = nvs_get_str(nvs_handle, NVS_KEY, hostname, &required_size);
        nvs_close(nvs_handle);
        
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "%s: Hostname from NVS: %s", __func__, hostname);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }
    
    // Fall back to CONFIG default
#ifdef CONFIG_SNAPCLIENT_NAME
    strncpy(hostname, CONFIG_SNAPCLIENT_NAME, max_len - 1);
    hostname[max_len - 1] = '\0';
    ESP_LOGD(TAG, "%s: Hostname from CONFIG: %s", __func__, hostname);
    err = ESP_OK;
#else
    // Ultimate fallback
    strncpy(hostname, "esp32-snapclient", max_len - 1);
    hostname[max_len - 1] = '\0';
    ESP_LOGW(TAG, "%s: Using default hostname: %s", __func__, hostname);
    err = ESP_OK;
#endif
    
    xSemaphoreGive(hostname_mutex);
    return err;
}

esp_err_t hostname_set(const char *hostname) {
    if (!hostname) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate hostname
    if (!validate_hostname(hostname)) {
        ESP_LOGE(TAG, "%s: Invalid hostname: %s", __func__, hostname);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Hostname manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Acquire mutex
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to open NVS: %s", __func__, esp_err_to_name(err));
        xSemaphoreGive(hostname_mutex);
        return err;
    }
    
    // Write hostname to NVS
    err = nvs_set_str(nvs_handle, NVS_KEY, hostname);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    xSemaphoreGive(hostname_mutex);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: Hostname saved to NVS: %s", __func__, hostname);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save hostname: %s", __func__, esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t hostname_clear(void) {
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Hostname manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Acquire mutex
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        // If namespace doesn't exist, that's fine - already cleared
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }
    
    // Erase the key
    err = nvs_erase_key(nvs_handle, NVS_KEY);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(nvs_handle);
        err = ESP_OK;
        ESP_LOGI(TAG, "%s: Hostname cleared from NVS", __func__);
    } else {
        ESP_LOGE(TAG, "%s: Failed to clear hostname: %s", __func__, esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    xSemaphoreGive(hostname_mutex);
    
    return err;
}

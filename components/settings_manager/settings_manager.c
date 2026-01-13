/**
 * @file hostname_manager.c
 * @brief Hostname management implementation
 */

#include "settings_manager.h"

#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "cJSON.h"

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "snapclient";
static const char *NVS_KEY_HOSTNAME = "hostname";
static const char *NVS_KEY_MDNS = "mdns";        // int32 0/1
static const char *NVS_KEY_SERVER_HOST = "server_host"; // string
static const char *NVS_KEY_SERVER_PORT = "server_port"; // int32

// Ethernet static IP settings
static const char *NVS_KEY_ETH_MODE = "eth_mode";       // int32: 0=Disabled, 1=DHCP, 2=Static
static const char *NVS_KEY_ETH_IP = "eth_ip";           // string "192.168.1.100"
static const char *NVS_KEY_ETH_NETMASK = "eth_netmask"; // string "255.255.255.0"
static const char *NVS_KEY_ETH_GATEWAY = "eth_gw";      // string "192.168.1.1"
static const char *NVS_KEY_ETH_DNS = "eth_dns";         // string "8.8.8.8"

// Mutex for thread-safe NVS access
static SemaphoreHandle_t hostname_mutex = NULL;

/**
 * @brief Validate hostname according to RFC 1123
 */
static bool validate_hostname(const char *hostname) {
    if (!hostname) {
        ESP_LOGD(TAG, "%s: hostname=NULL", __func__);
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
            ESP_LOGD(TAG, "%s: invalid char '%c' at pos %u", __func__, c, (unsigned) i);
            return false;
        }
    }
    ESP_LOGD(TAG, "%s: hostname '%s' valid", __func__, hostname);
    return true;
}

/**
 * @brief Validate IPv4 address format
 * @return true if valid IPv4 address (a.b.c.d where each octet is 0-255)
 */
static bool validate_ip_address(const char *ip) {
    if (!ip || strlen(ip) == 0) {
        return false;
    }

    unsigned int a, b, c, d;
    char extra;
    int ret = sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra);

    // Must have exactly 4 octets, no trailing characters
    if (ret != 4) {
        ESP_LOGD(TAG, "%s: invalid format '%s'", __func__, ip);
        return false;
    }

    // Each octet must be 0-255
    if (a > 255 || b > 255 || c > 255 || d > 255) {
        ESP_LOGD(TAG, "%s: octet out of range in '%s'", __func__, ip);
        return false;
    }

    ESP_LOGD(TAG, "%s: IP '%s' valid", __func__, ip);
    return true;
}

esp_err_t settings_manager_init(void) {
    if (hostname_mutex == NULL) {
        hostname_mutex = xSemaphoreCreateMutex();
        if (hostname_mutex == NULL) {
            ESP_LOGE(TAG, "%s: Failed to create mutex", __func__);
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "%s: Snapclient manager initialized", __func__);
    return ESP_OK;
}

esp_err_t settings_get_hostname(char *hostname, size_t max_len) {
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
        err = nvs_get_str(nvs_handle, NVS_KEY_HOSTNAME, hostname, &required_size);
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

esp_err_t settings_set_hostname(const char *hostname) {
    if (!hostname) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate hostname
    if (!validate_hostname(hostname)) {
        ESP_LOGE(TAG, "%s: Invalid hostname: %s", __func__, hostname);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Snapclient manager not initialized", __func__);
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
    err = nvs_set_str(nvs_handle, NVS_KEY_HOSTNAME, hostname);
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

esp_err_t settings_clear_hostname(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
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
    err = nvs_erase_key(nvs_handle, NVS_KEY_HOSTNAME);
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

/* mDNS enabled flag */
esp_err_t settings_get_mdns_enabled(bool *enabled) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!enabled) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, NVS_KEY_MDNS, &v);
        nvs_close(h);
        if (err == ESP_OK) {
            *enabled = (v != 0);
            ESP_LOGD(TAG, "%s: mdns from NVS: %d", __func__, *enabled ? 1 : 0);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }
    // Not present in NVS. Check sdkconfig if available, otherwise default true
#ifndef CONFIG_SNAPSERVER_USE_MDNS
    *enabled = false;
#else
    *enabled = true;
#endif
    ESP_LOGD(TAG, "%s: mdns from CONFIG_SNAPSERVER_USE_MDNS: %d", __func__, *enabled ? 1 : 0);
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_mdns_enabled(bool enabled) {
    ESP_LOGD(TAG, "%s: enabled=%d", __func__, enabled ? 1 : 0);
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    err = nvs_set_i32(h, NVS_KEY_MDNS, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: mdns saved: %d", __func__, enabled ? 1 : 0);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save mdns: %s", __func__, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_clear_mdns_enabled(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Settings manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    err = nvs_erase_key(h, NVS_KEY_MDNS);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
        err = ESP_OK;
        ESP_LOGI(TAG, "%s: mdns cleared from NVS", __func__);
    } else {
        ESP_LOGE(TAG, "%s: Failed to clear mdns: %s", __func__, esp_err_to_name(err));
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    return err;
}

/* Snapserver host/port */
esp_err_t settings_get_server_host(char *host, size_t max_len) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!host || max_len == 0) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t required = max_len;
        err = nvs_get_str(h, NVS_KEY_SERVER_HOST, host, &required);
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "%s: server_host from NVS: %s", __func__, host);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
            xSemaphoreGive(hostname_mutex);
            return err;
        }
    }

    // not present in NVS -> check sdkconfig fallbacks or empty
#ifdef CONFIG_SNAPSERVER_HOST
    strncpy(host, CONFIG_SNAPSERVER_HOST, max_len - 1);
    host[max_len - 1] = '\0';
    ESP_LOGD(TAG, "%s: server_host from CONFIG_SNAPSERVER_HOST: %s", __func__, host);
#else
    host[0] = '\0';
    ESP_LOGD(TAG, "%s: server_host not set (default empty)", __func__);
#endif
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_server_host(const char *host) {
    ESP_LOGD(TAG, "%s: host='%s'", __func__, host ? host : "(null)");
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    if (host == NULL || host[0] == '\0') {
        // erase
        err = nvs_erase_key(h, NVS_KEY_SERVER_HOST);
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_set_str(h, NVS_KEY_SERVER_HOST, host);
        if (err == ESP_OK) err = nvs_commit(h);
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: server_host saved: %s", __func__, host ? host : "(erased)");
    } else {
        ESP_LOGE(TAG, "%s: Failed to save server_host: %s", __func__, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_clear_server_host(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    return settings_set_server_host(NULL);
}

esp_err_t settings_get_server_port(int32_t *port) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!port) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, NVS_KEY_SERVER_PORT, &v);
        nvs_close(h);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
        xSemaphoreGive(hostname_mutex);
        if (err == ESP_OK) {
            *port = v;
            return ESP_OK;
        }
    }

    // not present in NVS -> check sdkconfig fallbacks or default 0
#ifdef CONFIG_SNAPSERVER_PORT
    *port = CONFIG_SNAPSERVER_PORT;
    ESP_LOGD(TAG, "%s: server_port from CONFIG_SNAPSERVER_PORT: %ld", __func__, (long)*port);
#else
    *port = 0;
    ESP_LOGD(TAG, "%s: server_port not set (default 0)", __func__);
#endif
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_server_port(int32_t port) {
    ESP_LOGD(TAG, "%s: port=%ld", __func__, (long)port);
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    err = nvs_set_i32(h, NVS_KEY_SERVER_PORT, port);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: server_port saved: %ld", __func__, (long)port);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save server_port: %s", __func__, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_clear_server_port(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Settings manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    err = nvs_erase_key(h, NVS_KEY_SERVER_PORT);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
        err = ESP_OK;
        ESP_LOGI(TAG, "%s: server_port cleared from NVS", __func__);
    } else {
        ESP_LOGE(TAG, "%s: Failed to clear server_port: %s", __func__, esp_err_to_name(err));
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    return err;
}

/* ============ Ethernet Static IP Settings ============ */

esp_err_t settings_get_eth_mode(int32_t *mode) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!mode) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 1;  // Default to DHCP
        err = nvs_get_i32(h, NVS_KEY_ETH_MODE, &v);
        nvs_close(h);
        if (err == ESP_OK) {
            *mode = v;
            ESP_LOGD(TAG, "%s: eth_mode from NVS: %ld", __func__, (long)*mode);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }

    // Default: DHCP (1)
    *mode = 1;
    ESP_LOGD(TAG, "%s: eth_mode default: %ld", __func__, (long)*mode);
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_eth_mode(int32_t mode) {
    ESP_LOGD(TAG, "%s: mode=%ld", __func__, (long)mode);
    if (mode < 0 || mode > 2) return ESP_ERR_INVALID_ARG;  // 0=Disabled, 1=DHCP, 2=Static
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    err = nvs_set_i32(h, NVS_KEY_ETH_MODE, mode);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: eth_mode saved: %ld", __func__, (long)mode);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save eth_mode: %s", __func__, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_clear_eth_mode(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    err = nvs_erase_key(h, NVS_KEY_ETH_MODE);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
        err = ESP_OK;
        ESP_LOGI(TAG, "%s: eth_mode cleared from NVS", __func__);
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    return err;
}

esp_err_t settings_get_eth_static_ip(char *ip, size_t max_len) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!ip || max_len == 0) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t required = max_len;
        err = nvs_get_str(h, NVS_KEY_ETH_IP, ip, &required);
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "%s: eth_ip from NVS: %s", __func__, ip);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }

    ip[0] = '\0';
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_eth_static_ip(const char *ip) {
    ESP_LOGD(TAG, "%s: ip='%s'", __func__, ip ? ip : "(null)");
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    // Validate IP if not clearing
    if (ip && ip[0] != '\0' && !validate_ip_address(ip)) {
        ESP_LOGE(TAG, "%s: Invalid IP address: %s", __func__, ip);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    if (ip == NULL || ip[0] == '\0') {
        err = nvs_erase_key(h, NVS_KEY_ETH_IP);
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_set_str(h, NVS_KEY_ETH_IP, ip);
        if (err == ESP_OK) err = nvs_commit(h);
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: eth_ip saved: %s", __func__, ip ? ip : "(erased)");
    }
    return err;
}

esp_err_t settings_clear_eth_static_ip(void) {
    return settings_set_eth_static_ip(NULL);
}

esp_err_t settings_get_eth_netmask(char *netmask, size_t max_len) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!netmask || max_len == 0) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t required = max_len;
        err = nvs_get_str(h, NVS_KEY_ETH_NETMASK, netmask, &required);
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "%s: eth_netmask from NVS: %s", __func__, netmask);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }

    // Default netmask
    strncpy(netmask, "255.255.255.0", max_len - 1);
    netmask[max_len - 1] = '\0';
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_eth_netmask(const char *netmask) {
    ESP_LOGD(TAG, "%s: netmask='%s'", __func__, netmask ? netmask : "(null)");
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (netmask && netmask[0] != '\0' && !validate_ip_address(netmask)) {
        ESP_LOGE(TAG, "%s: Invalid netmask: %s", __func__, netmask);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    if (netmask == NULL || netmask[0] == '\0') {
        err = nvs_erase_key(h, NVS_KEY_ETH_NETMASK);
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_set_str(h, NVS_KEY_ETH_NETMASK, netmask);
        if (err == ESP_OK) err = nvs_commit(h);
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: eth_netmask saved: %s", __func__, netmask ? netmask : "(erased)");
    }
    return err;
}

esp_err_t settings_clear_eth_netmask(void) {
    return settings_set_eth_netmask(NULL);
}

esp_err_t settings_get_eth_gateway(char *gw, size_t max_len) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!gw || max_len == 0) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t required = max_len;
        err = nvs_get_str(h, NVS_KEY_ETH_GATEWAY, gw, &required);
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "%s: eth_gateway from NVS: %s", __func__, gw);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }

    gw[0] = '\0';
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_eth_gateway(const char *gw) {
    ESP_LOGD(TAG, "%s: gw='%s'", __func__, gw ? gw : "(null)");
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (gw && gw[0] != '\0' && !validate_ip_address(gw)) {
        ESP_LOGE(TAG, "%s: Invalid gateway: %s", __func__, gw);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    if (gw == NULL || gw[0] == '\0') {
        err = nvs_erase_key(h, NVS_KEY_ETH_GATEWAY);
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_set_str(h, NVS_KEY_ETH_GATEWAY, gw);
        if (err == ESP_OK) err = nvs_commit(h);
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: eth_gateway saved: %s", __func__, gw ? gw : "(erased)");
    }
    return err;
}

esp_err_t settings_clear_eth_gateway(void) {
    return settings_set_eth_gateway(NULL);
}

esp_err_t settings_get_eth_dns(char *dns, size_t max_len) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!dns || max_len == 0) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t required = max_len;
        err = nvs_get_str(h, NVS_KEY_ETH_DNS, dns, &required);
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "%s: eth_dns from NVS: %s", __func__, dns);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }

    dns[0] = '\0';
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_eth_dns(const char *dns) {
    ESP_LOGD(TAG, "%s: dns='%s'", __func__, dns ? dns : "(null)");
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (dns && dns[0] != '\0' && !validate_ip_address(dns)) {
        ESP_LOGE(TAG, "%s: Invalid DNS: %s", __func__, dns);
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    if (dns == NULL || dns[0] == '\0') {
        err = nvs_erase_key(h, NVS_KEY_ETH_DNS);
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_set_str(h, NVS_KEY_ETH_DNS, dns);
        if (err == ESP_OK) err = nvs_commit(h);
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: eth_dns saved: %s", __func__, dns ? dns : "(erased)");
    }
    return err;
}

esp_err_t settings_clear_eth_dns(void) {
    return settings_set_eth_dns(NULL);
}

esp_err_t settings_get_json(char *json_out, size_t max_len) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    
    if (!json_out || max_len == 0) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to create JSON object", __func__);
        return ESP_ERR_NO_MEM;
    }

    // Get hostname
    char hostname[64] = {0};
    if (settings_get_hostname(hostname, sizeof(hostname)) == ESP_OK) {
        cJSON_AddStringToObject(root, "hostname", hostname);
    }

    // Get mdns enabled
    bool mdns = true;
    if (settings_get_mdns_enabled(&mdns) == ESP_OK) {
        cJSON_AddBoolToObject(root, "mdns_enabled", mdns);
    }

    // Get server host
    char host[128] = {0};
    if (settings_get_server_host(host, sizeof(host)) == ESP_OK && host[0] != '\0') {
        cJSON_AddStringToObject(root, "server_host", host);
    }

    // Get server port
    int32_t port = 0;
    if (settings_get_server_port(&port) == ESP_OK && port != 0) {
        cJSON_AddNumberToObject(root, "server_port", port);
    }

    // Add DSP availability flag
#if CONFIG_USE_DSP_PROCESSOR
    cJSON_AddBoolToObject(root, "dsp_available", true);
#else
    cJSON_AddBoolToObject(root, "dsp_available", false);
#endif

    // Get Ethernet mode
    int32_t eth_mode = 1;
    if (settings_get_eth_mode(&eth_mode) == ESP_OK) {
        cJSON_AddNumberToObject(root, "eth_mode", eth_mode);
    }

    // Get Ethernet static IP settings
    char eth_ip[16] = {0};
    if (settings_get_eth_static_ip(eth_ip, sizeof(eth_ip)) == ESP_OK && eth_ip[0] != '\0') {
        cJSON_AddStringToObject(root, "eth_static_ip", eth_ip);
    }

    char eth_netmask[16] = {0};
    if (settings_get_eth_netmask(eth_netmask, sizeof(eth_netmask)) == ESP_OK) {
        cJSON_AddStringToObject(root, "eth_netmask", eth_netmask);
    }

    char eth_gw[16] = {0};
    if (settings_get_eth_gateway(eth_gw, sizeof(eth_gw)) == ESP_OK && eth_gw[0] != '\0') {
        cJSON_AddStringToObject(root, "eth_gateway", eth_gw);
    }

    char eth_dns[16] = {0};
    if (settings_get_eth_dns(eth_dns, sizeof(eth_dns)) == ESP_OK && eth_dns[0] != '\0') {
        cJSON_AddStringToObject(root, "eth_dns", eth_dns);
    }

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    if (strlen(json_str) >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer", __func__);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGV(TAG, "%s: JSON generated: %s", __func__, json_out);
    return ESP_OK;
}

esp_err_t settings_set_from_json(const char *json_in) {
    ESP_LOGD(TAG, "%s: json=%s", __func__, json_in);
    
    if (!json_in) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_in);
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to parse JSON", __func__);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    // Update hostname if present
    cJSON *hostname = cJSON_GetObjectItem(root, "hostname");
    if (cJSON_IsString(hostname) && hostname->valuestring) {
        esp_err_t save_err = settings_set_hostname(hostname->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save hostname", __func__);
            err = save_err;
        }
    }

    // Update mdns_enabled if present
    cJSON *mdns = cJSON_GetObjectItem(root, "mdns_enabled");
    if (cJSON_IsBool(mdns)) {
        esp_err_t save_err = settings_set_mdns_enabled(cJSON_IsTrue(mdns));
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save mdns_enabled", __func__);
            err = save_err;
        }
    }

    // Update server_host if present
    cJSON *host = cJSON_GetObjectItem(root, "server_host");
    if (cJSON_IsString(host) && host->valuestring) {
        esp_err_t save_err = settings_set_server_host(host->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save server_host", __func__);
            err = save_err;
        }
    }

    // Update server_port if present
    cJSON *port = cJSON_GetObjectItem(root, "server_port");
    if (cJSON_IsNumber(port)) {
        esp_err_t save_err = settings_set_server_port((int32_t)port->valueint);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save server_port", __func__);
            err = save_err;
        }
    }

    // Update eth_mode if present
    cJSON *eth_mode = cJSON_GetObjectItem(root, "eth_mode");
    if (cJSON_IsNumber(eth_mode)) {
        esp_err_t save_err = settings_set_eth_mode((int32_t)eth_mode->valueint);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save eth_mode", __func__);
            err = save_err;
        }
    }

    // Update eth_static_ip if present
    cJSON *eth_ip = cJSON_GetObjectItem(root, "eth_static_ip");
    if (cJSON_IsString(eth_ip) && eth_ip->valuestring) {
        esp_err_t save_err = settings_set_eth_static_ip(eth_ip->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save eth_static_ip", __func__);
            err = save_err;
        }
    }

    // Update eth_netmask if present
    cJSON *eth_netmask = cJSON_GetObjectItem(root, "eth_netmask");
    if (cJSON_IsString(eth_netmask) && eth_netmask->valuestring) {
        esp_err_t save_err = settings_set_eth_netmask(eth_netmask->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save eth_netmask", __func__);
            err = save_err;
        }
    }

    // Update eth_gateway if present
    cJSON *eth_gw = cJSON_GetObjectItem(root, "eth_gateway");
    if (cJSON_IsString(eth_gw) && eth_gw->valuestring) {
        esp_err_t save_err = settings_set_eth_gateway(eth_gw->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save eth_gateway", __func__);
            err = save_err;
        }
    }

    // Update eth_dns if present
    cJSON *eth_dns = cJSON_GetObjectItem(root, "eth_dns");
    if (cJSON_IsString(eth_dns) && eth_dns->valuestring) {
        esp_err_t save_err = settings_set_eth_dns(eth_dns->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save eth_dns", __func__);
            err = save_err;
        }
    }

    cJSON_Delete(root);
    return err;
}

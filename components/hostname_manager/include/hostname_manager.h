/**
 * @file hostname_manager.h
 * @brief Hostname management for ESP32 Snapclient
 * 
 * Manages device hostname with NVS storage priority over sdkconfig defaults.
 * Hostname is used for mDNS discovery and Snapcast server identification.
 */

#ifndef __HOSTNAME_MANAGER_H__
#define __HOSTNAME_MANAGER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <stddef.h>

/**
 * @brief Get the current hostname
 * 
 * Returns hostname with the following priority:
 * 1. Value stored in NVS (if set by user)
 * 2. CONFIG_SNAPCLIENT_NAME from sdkconfig (default)
 * 
 * @param[out] hostname Buffer to store hostname
 * @param[in] max_len Maximum length of buffer (including null terminator)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if params invalid
 */
esp_err_t hostname_get(char *hostname, size_t max_len);

/**
 * @brief Set the hostname and store it in NVS
 * 
 * Validates hostname according to RFC 1123:
 * - Length: 1-63 characters
 * - Characters: alphanumeric and hyphens only
 * - Cannot start or end with hyphen
 * 
 * @param[in] hostname New hostname to set
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_ARG if hostname invalid
 *         ESP_ERR_NO_MEM if NVS operation fails
 */
esp_err_t hostname_set(const char *hostname);

/**
 * @brief Clear hostname from NVS (revert to CONFIG default)
 * 
 * @return ESP_OK on success
 */
esp_err_t hostname_clear(void);

/**
 * @brief Initialize hostname manager
 * 
 * Must be called before using other hostname functions.
 * 
 * @return ESP_OK on success
 */
esp_err_t hostname_manager_init(void);

#ifdef __cplusplus
}
#endif

#endif  // __HOSTNAME_MANAGER_H__

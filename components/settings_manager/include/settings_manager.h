/**
 * @file settings_manager.h
 * @brief Settings manager (hostname, mDNS, snapserver)
 *
 * Provides getters/setters persisted to NVS for:
 * - device hostname
 * - snapserver mDNS enabled flag
 * - snapserver host (string)
 * - snapserver port (int)
 */

#ifndef __SETTINGS_MANAGER_H__
#define __SETTINGS_MANAGER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t settings_manager_init(void);

/**
 * @defgroup settings_return_codes Common Return Codes
 *
 * All getter/setter functions in this module share the following return codes:
 *
 * - ESP_OK:              Success. For getters, if the key was not found in NVS
 *                        a sensible default is written to the output parameter
 *                        and ESP_OK is still returned.
 * - ESP_ERR_TIMEOUT:     The NVS mutex could not be acquired within 5 seconds.
 *                        This is transient — the setting was neither read nor
 *                        written. Callers should treat this as a recoverable
 *                        error (e.g. retry or use a compile-time default),
 *                        NOT as a fatal failure.
 * - ESP_ERR_INVALID_ARG: NULL output pointer or invalid input value.
 * - ESP_ERR_INVALID_STATE: settings_manager_init() has not been called.
 *
 * Setters may additionally return NVS-specific errors on write failure.
 * @{
 */

/* Hostname */
esp_err_t settings_get_hostname(char *hostname, size_t max_len);
esp_err_t settings_set_hostname(const char *hostname);
esp_err_t settings_clear_hostname(void);

/* mDNS enabled flag */
esp_err_t settings_get_mdns_enabled(bool *enabled);
esp_err_t settings_set_mdns_enabled(bool enabled);
esp_err_t settings_clear_mdns_enabled(void);

/* Snapserver host/port */
esp_err_t settings_get_server_host(char *host, size_t max_len);
esp_err_t settings_set_server_host(const char *host);
esp_err_t settings_clear_server_host(void);

esp_err_t settings_get_server_port(int32_t *port);
esp_err_t settings_set_server_port(int32_t port);
esp_err_t settings_clear_server_port(void);

/* Ethernet mode and static IP settings
 * Mode values: 0=Disabled, 1=DHCP (default), 2=Static
 */
esp_err_t settings_get_eth_mode(int32_t *mode);
esp_err_t settings_set_eth_mode(int32_t mode);
esp_err_t settings_clear_eth_mode(void);

esp_err_t settings_get_eth_static_ip(char *ip, size_t max_len);
esp_err_t settings_set_eth_static_ip(const char *ip);
esp_err_t settings_clear_eth_static_ip(void);

esp_err_t settings_get_eth_netmask(char *netmask, size_t max_len);
esp_err_t settings_set_eth_netmask(const char *netmask);
esp_err_t settings_clear_eth_netmask(void);

esp_err_t settings_get_eth_gateway(char *gw, size_t max_len);
esp_err_t settings_set_eth_gateway(const char *gw);
esp_err_t settings_clear_eth_gateway(void);

esp_err_t settings_get_eth_dns(char *dns, size_t max_len);
esp_err_t settings_set_eth_dns(const char *dns);
esp_err_t settings_clear_eth_dns(void);

/**
 * Get all settings as a JSON string
 * @param json_out Buffer to store JSON string (caller must allocate)
 * @param max_len Maximum size of output buffer
 * @return ESP_OK on success
 * 
 * Example output:
 * {
 *   "hostname": "esp32-snapclient",
 *   "mdns_enabled": true,
 *   "server_host": "192.168.1.100",
 *   "server_port": 1704
 * }
 */
esp_err_t settings_get_json(char *json_out, size_t max_len);

/**
 * Update settings from a JSON string
 * @param json_in JSON string containing settings to update
 * @return ESP_OK on success
 * 
 * Expected format (all fields optional):
 * {
 *   "hostname": "my-device",
 *   "mdns_enabled": false,
 *   "server_host": "192.168.1.100",
 *   "server_port": 1704
 * }
 */
esp_err_t settings_set_from_json(const char *json_in);

#ifdef __cplusplus
}
#endif

#endif /* __SETTINGS_MANAGER_H__ */

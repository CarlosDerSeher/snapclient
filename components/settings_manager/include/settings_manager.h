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

/* WiFi Resilience Settings */

/* TCP No Delay - disable Nagle's algorithm for lower latency */
esp_err_t settings_get_tcp_nodelay(bool *enabled);
esp_err_t settings_set_tcp_nodelay(bool enabled);
esp_err_t settings_clear_tcp_nodelay(void);

/* Queue Empty Threshold - consecutive empties before hard resync */
esp_err_t settings_get_queue_empty_threshold(int32_t *value);
esp_err_t settings_set_queue_empty_threshold(int32_t value);
esp_err_t settings_clear_queue_empty_threshold(void);

/* Queue Insert Timeout - ms to wait for queue space */
esp_err_t settings_get_queue_insert_timeout(int32_t *value);
esp_err_t settings_set_queue_insert_timeout(int32_t value);
esp_err_t settings_clear_queue_insert_timeout(void);

/* Fast Sync Latency - tolerance in microseconds */
esp_err_t settings_get_fast_sync_latency(int32_t *value);
esp_err_t settings_set_fast_sync_latency(int32_t value);
esp_err_t settings_clear_fast_sync_latency(void);

/* Reconnect Min Delay - initial delay in ms */
esp_err_t settings_get_reconnect_min_delay(int32_t *value);
esp_err_t settings_set_reconnect_min_delay(int32_t value);
esp_err_t settings_clear_reconnect_min_delay(void);

/* Reconnect Max Delay - max backoff delay in ms */
esp_err_t settings_get_reconnect_max_delay(int32_t *value);
esp_err_t settings_set_reconnect_max_delay(int32_t value);
esp_err_t settings_clear_reconnect_max_delay(void);

/* Buffer Headroom - extra buffer capacity as percentage */
esp_err_t settings_get_buffer_headroom(int32_t *value);
esp_err_t settings_set_buffer_headroom(int32_t value);
esp_err_t settings_clear_buffer_headroom(void);

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

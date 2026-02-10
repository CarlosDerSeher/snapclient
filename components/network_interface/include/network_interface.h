/*
 * network_interface.h
 *
 *  Created on: Jan 22, 2025
 *      Author: karl
 */

#ifndef COMPONENTS_NETWORK_INTERFACE_INCLUDE_NETWORK_INTERFACE_H_
#define COMPONENTS_NETWORK_INTERFACE_INCLUDE_NETWORK_INTERFACE_H_

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

#define NETWORK_INTERFACE_DESC_STA "sta"
#define NETWORK_INTERFACE_DESC_ETH \
  "eth"  // this is the default value created by ESP_NETIF_DEFAULT_ETH();
         // if more than 1 Ethernet interface is configured, they are appended
         // with numbers starting from 0, e.g.: eth0, eth1, ...

extern char *ipv6_addr_types_to_str[6];

/** Get netif by description string */
esp_netif_t *network_get_netif_from_desc(const char *desc);

/** Get interface key string */
const char *network_get_ifkey(esp_netif_t *esp_netif);

/** Check if netif is up */
bool network_is_netif_up(esp_netif_t *esp_netif);

/** Check if netif has a valid IP address */
bool network_has_ip(esp_netif_t *esp_netif);

/** Initialize network interfaces (WiFi and/or Ethernet) */
void network_if_init(void);

/*
 * Inter-component coordination via FreeRTOS EventGroups.
 * Used for reconnect requests and playback state signaling.
 *
 * Initialization order: Call network_events_init() before network_if_init()
 * and before any playback or reconnect functions are used.
 */

/** Initialize network event group (call early in startup) */
void network_events_init(void);

/** Check and clear reconnect request (thread-safe, returns true if requested) */
bool network_check_and_clear_reconnect(void);

/** Signal that playback has started (thread-safe)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized */
esp_err_t network_playback_started(void);

/** Signal that playback has stopped (thread-safe)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialized */
esp_err_t network_playback_stopped(void);

#endif /* COMPONENTS_NETWORK_INTERFACE_INCLUDE_NETWORK_INTERFACE_H_ */

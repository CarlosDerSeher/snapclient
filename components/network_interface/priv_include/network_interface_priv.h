/*
 * network_interface_priv.h
 *
 *  Created on: Jan 26, 2025
 *      Author: claude
 */

#ifndef COMPONENTS_NETWORK_INTERFACE_PRIV_INCLUDE_NETWORK_INTERFACE_PRIV_H_
#define COMPONENTS_NETWORK_INTERFACE_PRIV_INCLUDE_NETWORK_INTERFACE_PRIV_H_

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/*
 * Internal functions from network_interface.c - not part of public API.
 * These are only for use within the network_interface component.
 */

/** Get the network event group handle */
EventGroupHandle_t network_get_event_group(void);

/** Request network reconnection */
esp_err_t network_request_reconnect(void);

/** Check if playback is currently active */
bool network_is_playback_active(void);

/** Check if netif matches our interface prefix */
bool network_is_our_netif(const char *prefix, esp_netif_t *netif);

#endif /* COMPONENTS_NETWORK_INTERFACE_PRIV_INCLUDE_NETWORK_INTERFACE_PRIV_H_ */

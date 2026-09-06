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

/** Check if playback is currently active */
bool network_is_playback_active(void);

/** Check if netif matches our interface prefix */
bool network_is_our_netif(const char *prefix, esp_netif_t *netif);

/** Get unified MAC address for all network interfaces */
esp_err_t network_get_unified_mac_internal(uint8_t *mac_out);

/** Suppress WiFi auto-reconnect during Ethernet MAC takeover */
void wifi_suppress_for_takeover(void);

/** Clear WiFi suppression, optionally triggering reconnect */
void wifi_clear_suppression(bool reconnect);

/** Check if WiFi is currently suppressed for takeover */
bool wifi_is_suppressed(void);

/** Set WiFi power save mode: enable=false while audio is playing,
 *  enable=true when idle. No-op unless CONFIG_WIFI_DYNAMIC_POWER_SAVE is set. */
void wifi_set_power_save(bool enable);

#endif /* COMPONENTS_NETWORK_INTERFACE_PRIV_INCLUDE_NETWORK_INTERFACE_PRIV_H_ */

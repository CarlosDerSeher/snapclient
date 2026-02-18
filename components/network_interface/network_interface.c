/*
 * network_interface.c
 *
 *  Created on: Jan 22, 2025
 *      Author: karl
 */

/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
#include "eth_interface.h"
#endif

#include "wifi_interface.h"

static const char *TAG = "NET_IF";

/* types of ipv6 addresses to be displayed on ipv6 events */
const char *ipv6_addr_types_to_str[6] = {
    "ESP_IP6_ADDR_IS_UNKNOWN",      "ESP_IP6_ADDR_IS_GLOBAL",
    "ESP_IP6_ADDR_IS_LINK_LOCAL",   "ESP_IP6_ADDR_IS_SITE_LOCAL",
    "ESP_IP6_ADDR_IS_UNIQUE_LOCAL", "ESP_IP6_ADDR_IS_IPV4_MAPPED_IPV6"};

static bool netif_desc_matches_with(esp_netif_t *netif, void *ctx) {
  return strcmp(ctx, esp_netif_get_desc(netif)) == 0;
}

/**
 * @brief Checks the netif description if it contains specified prefix.
 * All netifs created withing common connect component are prefixed with the
 * module TAG, so it returns true if the specified netif is owned by this module
 */
bool network_is_our_netif(const char *prefix, esp_netif_t *netif) {
  return strncmp(prefix, esp_netif_get_desc(netif), strlen(prefix) - 1) == 0;
}

/**
 */
esp_netif_t *network_get_netif_from_desc(const char *desc) {
  return esp_netif_find_if(netif_desc_matches_with, (void *)desc);
}

const char *network_get_ifkey(esp_netif_t *esp_netif) {
  return esp_netif_get_ifkey(esp_netif);
}

bool network_is_netif_up(esp_netif_t *esp_netif) {
  return esp_netif_is_netif_up(esp_netif);
}

bool network_if_get_ip(esp_netif_ip_info_t *ip) {
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  if (eth_get_ip(ip) == true) {
    return true;
  }
#endif

  if (wifi_get_ip(ip) == true) {
    return true;
  }

  return false;
}

void network_if_init(void) {
  esp_netif_init();
  ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  eth_start();
#endif

  wifi_start();
}

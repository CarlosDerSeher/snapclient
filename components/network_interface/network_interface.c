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
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
#include "eth_interface.h"
#endif

#include "network_interface_priv.h"
#include "wifi_interface.h"

static const char *TAG = "NET_IF";

/* ============ Shared MAC Address for WiFi and Ethernet ============ */
static uint8_t unified_mac_address[ETH_ADDR_LEN] = {0};
static bool unified_mac_initialized = false;
static SemaphoreHandle_t mac_mutex = NULL;

/* ============ Event Group for Inter-Component Coordination ============ */
#define EVENT_PLAYBACK_STARTED_BIT     BIT1
#define EVENT_PLAYBACK_STOPPED_BIT     BIT2
/* BIT3 reserved for eth_interface.c monitor shutdown */

static EventGroupHandle_t network_event_group = NULL;
static bool network_events_initialized = false;

void network_events_init(void) {
    if (network_events_initialized) {
        ESP_LOGW(TAG, "Network events already initialized");
        return;
    }

    network_event_group = xEventGroupCreate();
    if (network_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create network event group - events will not work!");
    } else {
        network_events_initialized = true;
        ESP_LOGI(TAG, "Network events initialized");
    }
}

void network_events_deinit(void) {
    if (network_event_group) {
        vEventGroupDelete(network_event_group);
        network_event_group = NULL;
    }
    network_events_initialized = false;
}

EventGroupHandle_t network_get_event_group(void) {
    return network_event_group;
}

esp_err_t network_playback_started(void) {
    if (!network_event_group) {
        ESP_LOGW(TAG, "network_playback_started: events not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(network_event_group, EVENT_PLAYBACK_STARTED_BIT);
    xEventGroupClearBits(network_event_group, EVENT_PLAYBACK_STOPPED_BIT);

    // Disable WiFi power save during playback for better throughput
    wifi_set_power_save(false);

    return ESP_OK;
}

esp_err_t network_playback_stopped(void) {
    if (!network_event_group) {
        ESP_LOGW(TAG, "network_playback_stopped: events not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(network_event_group, EVENT_PLAYBACK_STOPPED_BIT);
    xEventGroupClearBits(network_event_group, EVENT_PLAYBACK_STARTED_BIT);

    // Re-enable WiFi power save when idle
    wifi_set_power_save(true);

    return ESP_OK;
}

bool network_is_playback_active(void) {
    if (!network_event_group) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(network_event_group);
    return (bits & EVENT_PLAYBACK_STARTED_BIT) != 0;
}

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

/**
 * @brief Check whether the given network interface has a valid IP address assigned.
 */
bool network_has_ip(esp_netif_t *esp_netif) {
  if (!esp_netif) return false;
  if (!esp_netif_is_netif_up(esp_netif)) return false;

  esp_netif_ip_info_t ip_info;
  esp_err_t err = esp_netif_get_ip_info(esp_netif, &ip_info);

#if CONFIG_SNAPCLIENT_CONNECT_IPV6
  // Prefer IPv4 when available
  if (err == ESP_OK && ip_info.ip.addr != 0) {
    return true;
  }
  // Fall back to IPv6 link-local check when IPv4 is not available
  esp_ip6_addr_t ip6;
  if (esp_netif_get_ip6_linklocal(esp_netif, &ip6) == ESP_OK) {
    // Verify the IPv6 address is not all zeros
    if (!ip6_addr_isany(&ip6)) {
      return true;
    }
  }
  return false;
#else
  if (err != ESP_OK) return false;
  return ip_info.ip.addr != 0;
#endif
}

/**
 * @brief Get the unified MAC address for all network interfaces (thread-safe)
 *
 * Reads WiFi's MAC address from eFuse on first call and caches it.
 * All network interfaces (WiFi and Ethernet) should use this MAC.
 *
 * Thread Safety: Uses mutex to protect initialization and read operations.
 * Safe to call from multiple threads concurrently.
 *
 * @param[out] mac_out Buffer to receive ETH_ADDR_LEN-byte MAC address. Must not be NULL.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if mac_out is NULL,
 *         ESP_ERR_NO_MEM if mutex creation fails
 */
esp_err_t network_get_unified_mac_internal(uint8_t *mac_out) {
    if (!mac_out) {
        return ESP_ERR_INVALID_ARG;
    }

    // Create mutex on first use (happens once during early boot before multithreading)
    if (!mac_mutex) {
        mac_mutex = xSemaphoreCreateMutex();
        if (!mac_mutex) {
            ESP_LOGE(TAG, "Failed to create MAC mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Acquire mutex for thread-safe access
    xSemaphoreTake(mac_mutex, portMAX_DELAY);

    if (!unified_mac_initialized) {
        esp_err_t err = esp_read_mac(unified_mac_address, ESP_MAC_WIFI_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read WiFi MAC address: %s", esp_err_to_name(err));
            xSemaphoreGive(mac_mutex);
            return err;
        }

        ESP_LOGI(TAG, "Unified MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                 unified_mac_address[0], unified_mac_address[1],
                 unified_mac_address[2], unified_mac_address[3],
                 unified_mac_address[4], unified_mac_address[5]);

        unified_mac_initialized = true;
    }

    memcpy(mac_out, unified_mac_address, ETH_ADDR_LEN);

    xSemaphoreGive(mac_mutex);
    return ESP_OK;
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

  // Initialize unified MAC address before starting any network interfaces
  uint8_t mac[ETH_ADDR_LEN];
  esp_err_t err = network_get_unified_mac_internal(mac);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CRITICAL: Failed to initialize unified MAC address");
    ESP_ERROR_CHECK(err);  // Fatal error - cannot proceed without MAC
  }

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  eth_start();
#endif

  wifi_start();
}

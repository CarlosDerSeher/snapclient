/*
 * network_interface.h
 *
 *  Created on: Jan 22, 2025
 *      Author: karl
 */

#ifndef COMPONENTS_NETWORK_INTERFACE_INCLUDE_NETWORK_INTERFACE_H_
#define COMPONENTS_NETWORK_INTERFACE_INCLUDE_NETWORK_INTERFACE_H_

#include <stdbool.h>

#include "esp_netif.h"

#define NETWORK_INTERFACE_DESC_STA "sta"
#define NETWORK_INTERFACE_DESC_ETH \
  "eth"  // this is the default value created by ESP_NETIF_DEFAULT_ETH();
         // if more than 1 Ethernet interface is configured, they are appended
         // with numbers starting from 0, e.g.: eth0, eth1, ...

extern char *ipv6_addr_types_to_str[6];

esp_netif_t *network_get_netif_from_desc(const char *desc);
const char *network_get_ifkey(esp_netif_t *esp_netif);
bool network_if_get_ip(esp_netif_ip_info_t *ip);
bool network_is_netif_up(esp_netif_t *esp_netif);
bool network_is_our_netif(const char *prefix, esp_netif_t *netif);
void network_if_init(void);

#endif /* COMPONENTS_NETWORK_INTERFACE_INCLUDE_NETWORK_INTERFACE_H_ */


#ifndef COMPONENTS_NETWORK_INTERFACE_INCLUDE_ETH_INTERFACE_H
#define COMPONENTS_NETWORK_INTERFACE_INCLUDE_ETH_INTERFACE_H

#ifdef __cplusplus
extern "C" {
#endif

bool eth_get_ip(esp_netif_ip_info_t *ip);
void eth_start(void);

#ifdef __cplusplus
}
#endif

#endif  // COMPONENTS_NETWORK_INTERFACE_INCLUDE_ETH_INTERFACE_H

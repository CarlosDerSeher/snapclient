idf_component_register(SRCS "network_interface.c" "eth_interface.c" "wifi_interface.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_wifi esp_eth esp_netif esp_timer nvs_flash improv_wifi)

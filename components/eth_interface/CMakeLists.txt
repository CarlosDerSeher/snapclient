idf_component_register(SRCS "eth_interface.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_eth esp_netif)

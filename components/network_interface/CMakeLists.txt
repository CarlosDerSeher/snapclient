set(SRCS "network_interface.c" "wifi_interface.c")

# Only include Ethernet interface if Ethernet is enabled
if(CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET OR CONFIG_SNAPCLIENT_USE_SPI_ETHERNET)
    list(APPEND SRCS "eth_interface.c")
endif()

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver esp_wifi esp_eth esp_netif esp_timer nvs_flash improv_wifi settings_manager lwip)

set(SRCS "network_interface.c" "wifi_interface.c")

# Only include Ethernet interface if Ethernet is enabled
if(CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET OR CONFIG_SNAPCLIENT_USE_SPI_ETHERNET)
    list(APPEND SRCS "eth_interface.c")
endif()

set(PRIV_DEPS driver esp_wifi esp_eth esp_netif esp_timer nvs_flash improv_wifi settings_manager lwip)

# ping is only needed when Ethernet is enabled (used by eth_interface.c)
if(CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET OR CONFIG_SNAPCLIENT_USE_SPI_ETHERNET)
    list(APPEND PRIV_DEPS ping)
endif()

idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       PRIV_REQUIRES ${PRIV_DEPS})

idf_component_register(SRCS "wifi_interface.c"
                       INCLUDE_DIRS "include"
                       REQUIRES wifi_provisioning esp_event esp_wifi esp_hw_support nvs_flash improv_wifi)


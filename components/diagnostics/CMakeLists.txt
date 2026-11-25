idf_component_register(SRCS "heap_diag.c" "sync_diag.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_app_format lightsnapcast network_interface esp_event esp_netif esp_timer esp_wifi)

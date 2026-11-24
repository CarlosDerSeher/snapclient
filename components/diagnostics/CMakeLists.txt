idf_component_register(SRCS "heap_diag.c" "sync_diag.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES lightsnapcast esp_event esp_netif esp_timer esp_wifi)

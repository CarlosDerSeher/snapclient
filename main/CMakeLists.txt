idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_timer esp_wifi eth_interface nvs_flash wifi_interface audio_board audio_hal audio_sal net_functions opus flac ota_server
                       				 ui_http_server
                       )

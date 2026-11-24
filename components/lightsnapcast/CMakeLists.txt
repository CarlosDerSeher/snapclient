idf_component_register(SRCS "snapcast.c" "player.c" "pcm_pool.c" "snapcast_protocol_parser.c"
                       INCLUDE_DIRS "include"
                       REQUIRES libbuffer json libmedian timefilter esp_wifi driver esp_timer
                       PRIV_REQUIRES network_interface)

idf_component_register(SRCS "snapcast.c" "player.c" "snapcast_custom_parser.c"
                       INCLUDE_DIRS "include"
                       REQUIRES libbuffer json libmedian esp_wifi driver esp_timer)

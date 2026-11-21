idf_component_register(SRCS "main.c" "connection_handler.c" "heap_diag.c"
                       INCLUDE_DIRS "."

                       PRIV_REQUIRES esp_timer esp_wifi nvs_flash audio_board audio_hal audio_sal net_functions opus flac app_update lightsnapcast dsp_processor
                       				 ui_http_server network_interface custom_board settings_manager tas5805m_settings udp_logger
                       )

set_source_files_properties(main.c PROPERTIES COMPILE_FLAGS -Wno-implicit-fallthrough)

set(main_priv_requires
    esp_timer
    esp_wifi
    nvs_flash
    audio_board
    audio_hal
    audio_sal
    net_functions
    opus
    flac
    ota_server
    ui_http_server
    network_interface
    custom_board
    settings_manager
    tas5805m_settings
)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../components/udp_logger/CMakeLists.txt")
    list(APPEND main_priv_requires udp_logger)
endif()

idf_component_register(SRCS "main.c" "connection_handler.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES ${main_priv_requires})

set_source_files_properties(main.c PROPERTIES COMPILE_FLAGS -Wno-implicit-fallthrough)

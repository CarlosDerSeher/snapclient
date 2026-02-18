idf_component_register(
    SRCS "tas5805m_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash custom_board json
)

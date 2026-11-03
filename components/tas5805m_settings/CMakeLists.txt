idf_component_register(
    SRCS "tas5805m_settings.c" "tas5805m_biamp.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash custom_board json
)

idf_component_register(
    SRCS "pcm51xx_settings.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash custom_board json
)

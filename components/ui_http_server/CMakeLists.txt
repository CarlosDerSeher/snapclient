idf_component_register(SRCS "ui_http_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server settings_manager dsp_processor_settings
                       EMBED_TXTFILES html/index.html
                                      html/index.js
                                      html/styles.css
                                      html/general-settings.html
                                      html/dsp-settings.html
                       EMBED_FILES html/favicon.ico)

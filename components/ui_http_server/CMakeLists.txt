idf_component_register(SRCS "ui_http_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES spiffs esp_http_server dsp_processor vfs nvs_flash)

# Create a SPIFFS image from the contents of the 'html' directory
# that fits the partition named 'storage'. FLASH_IN_PROJECT indicates that
# the generated image should be flashed when the entire project is flashed to
# the target with 'idf.py -p PORT flash
spiffs_create_partition_image(storage html FLASH_IN_PROJECT)

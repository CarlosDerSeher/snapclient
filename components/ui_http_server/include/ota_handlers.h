#pragma once

#include "esp_http_server.h"

#if CONFIG_SNAPCLIENT_WEB_OTA
void ota_register_handlers(httpd_handle_t server);
#endif

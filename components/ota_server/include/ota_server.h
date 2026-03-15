#pragma once

#define OTA_LISTEN_PORT 8032
#define OTA_BUFF_SIZE 1024

#include "freertos/event_groups.h"
#include "esp_err.h"

extern const int OTA_CONNECTED_BIT;

void ota_server_task(void *param);
esp_err_t ota_server_start_my(void);

extern EventGroupHandle_t ota_event_group;

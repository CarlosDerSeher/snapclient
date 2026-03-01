#pragma once

#include "esp_err.h"

/**
 * @brief Initialize the volume buttons component.
 *
 * Configures GPIO inputs for volume up/down buttons and starts the polling
 * task. On each button press the current MPD volume is fetched via
 * Server.GetStatus on the snapserver JSON-RPC port (1705), incremented or
 * decremented by VOLUME_BTN_STEP, and written back via Stream.SetProperty.
 *
 * Call this after Wi-Fi is connected and the snapserver is reachable.
 * The snapserver host is taken from CONFIG_SNAPSERVER_HOST.
 *
 * @return ESP_OK on success, or an error code on GPIO/task init failure.
 */
esp_err_t volume_buttons_init(const char *snapserver_host);

/**
 * @brief Stop the volume buttons polling task and release resources.
 */
void volume_buttons_deinit(void);

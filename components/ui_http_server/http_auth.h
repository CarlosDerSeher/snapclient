#pragma once

#include "esp_http_server.h"

/**
 * Check the HTTP Basic Auth header when CONFIG_HTTP_AUTH_ENABLE is set.
 * Sends a 401 challenge and returns an error if the request is not
 * authorized; returns ESP_OK otherwise (always, when auth is disabled).
 */
esp_err_t require_http_auth(httpd_req_t *req);

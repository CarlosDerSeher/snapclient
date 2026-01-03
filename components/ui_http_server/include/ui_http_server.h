#ifndef __UI_HTTP_SERVER_H__
#define __UI_HTTP_SERVER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <esp_err.h>

void init_http_server_task(void);

typedef struct {
  char key[16];
  int32_t int_value;
} URL_t;

/*
 * Save a single integer parameter to NVS under namespace "ui_http".
 */
esp_err_t ui_http_save_param(const char *name, int32_t value);

/*
 * Load a single integer parameter from NVS. Returns ESP_OK on success or
 * ESP_ERR_NVS_NOT_FOUND if not present.
 */
esp_err_t ui_http_load_param(const char *name, int32_t *value);

#ifdef __cplusplus
}
#endif

#endif  // __UI_HTTP_SERVER_H__

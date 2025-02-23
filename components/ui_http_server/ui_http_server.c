/* HTTP Server Example

         This example code is in the Public Domain (or CC0 licensed, at your
   option.)

         Unless required by applicable law or agreed to in writing, this
         software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
         CONDITIONS OF ANY KIND, either express or implied.
*/

#include "ui_http_server.h"

#include <string.h>

#include "dsp_processor.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "UI_HTTP";

static QueueHandle_t xQueueHttp = NULL;
static TaskHandle_t taskHandle = NULL;
static httpd_handle_t server = NULL;

/**
 *
 */
static void SPIFFS_Directory(char *path) {
  DIR *dir = opendir(path);
  assert(dir != NULL);
  while (true) {
    struct dirent *pe = readdir(dir);
    if (!pe) break;
    ESP_LOGI(TAG, "d_name=%s/%s d_ino=%d d_type=%x", path, pe->d_name,
             pe->d_ino, pe->d_type);
  }
  closedir(dir);
}

/**
 *
 */
static esp_err_t SPIFFS_Mount(char *path, char *label, int max_files) {
  esp_err_t ret;

  if (!esp_spiffs_mounted(label)) {
    esp_vfs_spiffs_conf_t conf = {.base_path = path,
                                  .partition_label = label,
                                  .max_files = max_files,
                                  .format_if_mount_failed = true};

    // Use settings defined above to initialize and mount SPIFFS file system.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
      if (ret == ESP_FAIL) {
        ESP_LOGE(TAG, "Failed to mount or format filesystem");
      } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to find SPIFFS partition");
      } else {
        ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
      }
      return ret;
    }
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(label, &total, &used);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)",
             esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
  }

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Mount %s to %s success", path, label);
    SPIFFS_Directory(path);
  }

  return ret;
}

/**
 *
 */
static int find_key_value(char *key, char *parameter, char *value) {
  // char * addr1;
  char *addr1 = strstr(parameter, key);
  if (addr1 == NULL) return 0;
  ESP_LOGD(TAG, "addr1=%s", addr1);

  char *addr2 = addr1 + strlen(key);
  ESP_LOGD(TAG, "addr2=[%s]", addr2);

  char *addr3 = strstr(addr2, "&");
  ESP_LOGD(TAG, "addr3=%p", addr3);
  if (addr3 == NULL) {
    strcpy(value, addr2);
  } else {
    int length = addr3 - addr2;
    ESP_LOGD(TAG, "addr2=%p addr3=%p length=%d", addr2, addr3, length);
    strncpy(value, addr2, length);
    value[length] = 0;
  }
  //	ESP_LOGI(TAG, "key=[%s] value=[%s]", key, value);
  return strlen(value);
}

/**
 *
 */
static esp_err_t Text2Html(httpd_req_t *req, char *filename) {
  //	ESP_LOGI(TAG, "Reading %s", filename);
  FILE *fhtml = fopen(filename, "r");
  if (fhtml == NULL) {
    ESP_LOGE(TAG, "fopen fail. [%s]", filename);
    return ESP_FAIL;
  } else {
    char line[128];
    while (fgets(line, sizeof(line), fhtml) != NULL) {
      size_t linelen = strlen(line);
      // remove EOL (CR or LF)
      for (int i = linelen; i > 0; i--) {
        if (line[i - 1] == 0x0a) {
          line[i - 1] = 0;
        } else if (line[i - 1] == 0x0d) {
          line[i - 1] = 0;
        } else {
          break;
        }
      }
      ESP_LOGD(TAG, "line=[%s]", line);
      if (strlen(line) == 0) continue;
      esp_err_t ret = httpd_resp_sendstr_chunk(req, line);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_resp_sendstr_chunk fail %d", ret);
      }
    }
    fclose(fhtml);
  }
  return ESP_OK;
}

/**
 *
 */
static esp_err_t Image2Html(httpd_req_t *req, char *filename, char *type) {
  FILE *fhtml = fopen(filename, "r");
  if (fhtml == NULL) {
    ESP_LOGE(TAG, "fopen fail. [%s]", filename);
    return ESP_FAIL;
  } else {
    char buffer[64];

    if (strcmp(type, "jpeg") == 0) {
      httpd_resp_sendstr_chunk(req, "<img src=\"data:image/jpeg;base64,");
    } else if (strcmp(type, "jpg") == 0) {
      httpd_resp_sendstr_chunk(req, "<img src=\"data:image/jpeg;base64,");
    } else if (strcmp(type, "png") == 0) {
      httpd_resp_sendstr_chunk(req, "<img src=\"data:image/png;base64,");
    } else {
      ESP_LOGW(TAG, "file type fail. [%s]", type);
      httpd_resp_sendstr_chunk(req, "<img src=\"data:image/png;base64,");
    }
    while (1) {
      size_t bufferSize = fread(buffer, 1, sizeof(buffer), fhtml);
      ESP_LOGD(TAG, "bufferSize=%d", bufferSize);
      if (bufferSize > 0) {
        httpd_resp_send_chunk(req, buffer, bufferSize);
      } else {
        break;
      }
    }
    fclose(fhtml);
    httpd_resp_sendstr_chunk(req, "\">");
  }
  return ESP_OK;
}

/**
 * HTTP get handler
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
  //	ESP_LOGI(TAG, "root_get_handler req->uri=[%s]", req->uri);

  /* Send index.html */
  Text2Html(req, "/html/index.html");

  /* Send Image */
  // Image2Html(req, "/html/ESP-LOGO.txt", "png");

  /* Send empty chunk to signal HTTP response completion */
  httpd_resp_sendstr_chunk(req, NULL);

  return ESP_OK;
}

/*
 * HTTP post handler
 */
static esp_err_t root_post_handler(httpd_req_t *req) {
  //	ESP_LOGI(TAG, "root_post_handler req->uri=[%s]", req->uri);
  URL_t urlBuf;
  int ret = -1;

  memset(&urlBuf, 0, sizeof(URL_t));

  if (find_key_value("gain_1=", (char *)req->uri, urlBuf.str_value)) {
    ESP_LOGD(TAG, "urlBuf.str_value=[%s]", urlBuf.str_value);

    urlBuf.gain_1 = strtof(urlBuf.str_value, NULL);
    ESP_LOGD(TAG, "urlBuf.float_value=%f", urlBuf.gain_1);

    ret = 0;
  } else {
    ESP_LOGD(TAG, "key 'gain_1=' not found");
  }

  if (find_key_value("gain_2=", (char *)req->uri, urlBuf.str_value)) {
    ESP_LOGD(TAG, "urlBuf.str_value=[%s]", urlBuf.str_value);

    urlBuf.gain_2 = strtof(urlBuf.str_value, NULL);
    ESP_LOGD(TAG, "urlBuf.float_value=%f", urlBuf.gain_2);

    ret = 0;
  } else {
    ESP_LOGD(TAG, "key 'gain_2=' not found");
  }

  if (find_key_value("gain_3=", (char *)req->uri, urlBuf.str_value)) {
    ESP_LOGD(TAG, "urlBuf.str_value=[%s]", urlBuf.str_value);

    urlBuf.gain_3 = strtof(urlBuf.str_value, NULL);
    ESP_LOGD(TAG, "urlBuf.float_value=%f", urlBuf.gain_3);

    ret = 0;
  } else {
    ESP_LOGD(TAG, "key 'gain_3=' not found");
  }

  if (ret >= 0) {
    // Send to http_server_task
    if (xQueueSend(xQueueHttp, &urlBuf, portMAX_DELAY) != pdPASS) {
      ESP_LOGE(TAG, "xQueueSend Fail");
    }
  }

  /* Redirect onto root to see the updated file list */
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  // httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_sendstr(req, "post successfully");
  return ESP_OK;
}

/*
 * favicon get handler
 */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
  //	ESP_LOGI(TAG, "favicon_get_handler req->uri=[%s]", req->uri);
  return ESP_OK;
}

/**
 */
esp_err_t stop_server(void) {
  if (server) {
    httpd_stop(server);
    server = NULL;
  }

  return ESP_OK;
}

/*
 * Function to start the web server
 */
esp_err_t start_server(const char *base_path, int port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.max_open_sockets = 2;

  /* Use the URI wildcard matching function in order to
   * allow the same handler to respond to multiple different
   * target URIs which match the wildcard scheme */
  config.uri_match_fn = httpd_uri_match_wildcard;

  ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start file server!");
    return ESP_FAIL;
  }

  /* URI handler for get */
  httpd_uri_t _root_get_handler = {
      .uri = "/", .method = HTTP_GET, .handler = root_get_handler,
      //.user_ctx  = server_data	// Pass server data as context
  };
  httpd_register_uri_handler(server, &_root_get_handler);

  /* URI handler for post */
  httpd_uri_t _root_post_handler = {
      .uri = "/post", .method = HTTP_POST, .handler = root_post_handler,
      //.user_ctx  = server_data	// Pass server data as context
  };
  httpd_register_uri_handler(server, &_root_post_handler);

  /* URI handler for favicon.ico */
  httpd_uri_t _favicon_get_handler = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler,
      //.user_ctx  = server_data	// Pass server data as context
  };
  httpd_register_uri_handler(server, &_favicon_get_handler);

  return ESP_OK;
}

/**
 *
 */
static void http_server_task(void *pvParameters) {
  // Start Server
  ESP_ERROR_CHECK(start_server("/html", CONFIG_WEB_PORT));

  URL_t urlBuf;
  while (1) {
    // Waiting for post
    if (xQueueReceive(xQueueHttp, &urlBuf, portMAX_DELAY) == pdTRUE) {
      filterParams_t filterParams;

      ESP_LOGI(TAG, "str_value=%s gain_1=%f, gain_2=%f, gain_3=%f",
               urlBuf.str_value, urlBuf.gain_1, urlBuf.gain_2, urlBuf.gain_3);

      filterParams.dspFlow = dspfEQBassTreble;
      filterParams.fc_1 = 300.0;
      filterParams.gain_1 = urlBuf.gain_1;
      filterParams.fc_3 = 4000.0;
      filterParams.gain_3 = urlBuf.gain_3;

#if CONFIG_USE_DSP_PROCESSOR
      dsp_processor_update_filter_params(&filterParams);
#endif
    }
  }

  // Never reach here
  ESP_LOGI(TAG, "finish");
  vTaskDelete(NULL);
}

/**
 *
 */
void init_http_server_task(void) {
  // Initialize SPIFFS
  ESP_LOGI(TAG, "Initializing SPIFFS");
  if (SPIFFS_Mount("/html", "storage", 6) != ESP_OK) {
    ESP_LOGE(TAG, "SPIFFS mount failed");
    return;
  }

  // Create Queue
  if (!xQueueHttp) {
    xQueueHttp = xQueueCreate(10, sizeof(URL_t));
    configASSERT(xQueueHttp);
  }

  if (taskHandle) {
    stop_server();
    vTaskDelete(taskHandle);
    taskHandle = NULL;
  }

  xTaskCreatePinnedToCore(http_server_task, "HTTP", 512 * 5, NULL, 2,
                          &taskHandle, tskNO_AFFINITY);
}

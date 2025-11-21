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
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "UI_HTTP";

static QueueHandle_t xQueueHttp = NULL;
static TaskHandle_t taskHandle = NULL;
static httpd_handle_t server = NULL;
static SemaphoreHandle_t nvs_mutex = NULL;

/**
 * List files in SPIFFS directory
 */
static void SPIFFS_Directory(char *path) {
  ESP_LOGD(TAG, "%s: path=%s", __func__, path);
  DIR *dir = opendir(path);
  assert(dir != NULL);
  while (true) {
    struct dirent *pe = readdir(dir);
    if (!pe) break;
    ESP_LOGI(TAG, "%s: d_name=%s/%s d_ino=%d d_type=%x", __func__, path, pe->d_name,
             pe->d_ino, pe->d_type);
  }
  closedir(dir);
}

/**
 * Helper: Generate flow-specific NVS key
 * Format: "flow_<id>_<param>" (e.g., "flow_5_fc_1" for dspfEQBassTreble bass freq)
 * This prevents parameter collisions between different DSP flows
 */
static void make_flow_key(char *out_key, size_t out_size, dspFlows_t flow, const char *param) {
  snprintf(out_key, out_size, "flow_%d_%s", (int)flow, param);
}

/**
 * Mount SPIFFS filesystem
 */
static esp_err_t SPIFFS_Mount(char *path, char *label, int max_files) {
  ESP_LOGD(TAG, "%s: path=%s label=%s max_files=%d", __func__, path, label, max_files);
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
        ESP_LOGE(TAG, "%s: Failed to mount or format filesystem", __func__);
      } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "%s: Failed to find SPIFFS partition", __func__);
      } else {
        ESP_LOGE(TAG, "%s: Failed to initialize SPIFFS (%s)", __func__, esp_err_to_name(ret));
      }
      return ret;
    }
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(label, &total, &used);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to get SPIFFS partition information (%s)", __func__,
             esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "%s: Partition size: total: %d, used: %d", __func__, total, used);
  }

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "%s: Mount %s to %s success", __func__, path, label);
    SPIFFS_Directory(path);
  }

  return ret;
}

/**
 * Convert text file to HTML response
 */
static esp_err_t Text2Html(httpd_req_t *req, char *filename) {
  ESP_LOGD(TAG, "%s: filename=%s", __func__, filename);
  //	ESP_LOGI(TAG, "Reading %s", filename);
  FILE *fhtml = fopen(filename, "r");
  if (fhtml == NULL) {
    ESP_LOGE(TAG, "%s: fopen fail. [%s]", __func__, filename);
    return ESP_FAIL;
  } else {
    char line[128];
    while (fgets(line, sizeof(line), fhtml) != NULL) {
      size_t linelen = strlen(line);
      // Remove EOL (CR or LF) but add back \n for proper line breaks
      for (int i = linelen; i > 0; i--) {
        if (line[i - 1] == 0x0a) {
          line[i - 1] = 0;
        } else if (line[i - 1] == 0x0d) {
          line[i - 1] = 0;
        } else {
          break;
        }
      }
      ESP_LOGV(TAG, "line=[%s]", line);
      if (strlen(line) == 0) {
        // Send empty line as newline to preserve structure
        httpd_resp_sendstr_chunk(req, "\n");
        continue;
      }
      // Send line content
      esp_err_t ret = httpd_resp_sendstr_chunk(req, line);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: httpd_resp_sendstr_chunk fail %d", __func__, ret);
      }
      // Add newline after each line to preserve line breaks
      httpd_resp_sendstr_chunk(req, "\n");
    }
    fclose(fhtml);
  }
  return ESP_OK;
}

/**
 * Find key value in parameter string
 */
static int find_key_value(char *key, char *parameter, char *value) {
  ESP_LOGD(TAG, "%s: key=%s", __func__, key);
  // char * addr1;
  char *addr1 = strstr(parameter, key);
  if (addr1 == NULL) return 0;
  ESP_LOGD(TAG, "%s: addr1=%s", __func__, addr1);

  char *addr2 = addr1 + strlen(key);
  ESP_LOGD(TAG, "%s: addr2=[%s]", __func__, addr2);

  char *addr3 = strstr(addr2, "&");
  ESP_LOGD(TAG, "%s: addr3=%p", __func__, addr3);
  if (addr3 == NULL) {
    strcpy(value, addr2);
  } else {
    int length = addr3 - addr2;
    ESP_LOGD(TAG, "%s: addr2=%p addr3=%p length=%d", __func__, addr2, addr3, length);
    strncpy(value, addr2, length);
    value[length] = 0;
  }
  ESP_LOGD(TAG, "%s: key=[%s] value=[%s]", __func__, key, value);
  return strlen(value);
}

/**
 * Set CORS headers to allow cross-origin requests
 * This enables local development with ?backend parameter
 */
static void set_cors_headers(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
}

/**
 * HTTP get handler
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);

  set_cors_headers(req);

  /* Send index.html */
  Text2Html(req, "/html/index.html");

  /* Send empty chunk to signal HTTP response completion */
  httpd_resp_sendstr_chunk(req, NULL);

  return ESP_OK;
}

/*
 * HTTP post handler
 * Expects a single parameter change in the query string: /post?param=NAME&value=INT
 */
static esp_err_t root_post_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
  URL_t urlBuf;
  int ret = -1;
  char param[16] = {0};
  char valstr[32] = {0};

  set_cors_headers(req);

  memset(&urlBuf, 0, sizeof(URL_t));

  if (find_key_value("param=", (char *)req->uri, param) &&
      find_key_value("value=", (char *)req->uri, valstr)) {
    // Parse integer value; strtol skips leading whitespace
    long v = strtol(valstr, NULL, 10);
    urlBuf.int_value = (int32_t)v;
    strncpy(urlBuf.key, param, sizeof(urlBuf.key) - 1);
    ret = 0;
    ESP_LOGD(TAG, "%s: Received param=%s value=%d", __func__, urlBuf.key, urlBuf.int_value);
  } else {
    ESP_LOGD(TAG, "%s: Invalid post: expected param=NAME&value=INT in URI", __func__);
  }

  if (ret >= 0) {
    // Send to http_server_task with timeout to prevent handler from blocking indefinitely
    if (xQueueSend(xQueueHttp, &urlBuf, pdMS_TO_TICKS(1000)) != pdPASS) {
      ESP_LOGE(TAG, "%s: xQueueSend Fail (queue full or timeout)", __func__);
      httpd_resp_set_status(req, "503 Service Unavailable");
      httpd_resp_sendstr(req, "Queue full, try again");
      return ESP_OK;
    }
  }

  httpd_resp_set_status(req, "200 OK");
  httpd_resp_sendstr(req, "ok");
  return ESP_OK;
}

/*
 * GET parameter handler
 * Returns current parameter value: /get?param=NAME
 * Response format: plain text integer value
 * 
 * This reads from the DSP processor's centralized storage for the active flow
 */
static esp_err_t get_param_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
  char param[16] = {0};
  
  set_cors_headers(req);
  
  if (find_key_value("param=", (char *)req->uri, param)) {
#if CONFIG_USE_DSP_PROCESSOR
    // Get current flow from DSP processor
    dspFlows_t current_flow = dsp_processor_get_current_flow();
    
    // Get parameters for current flow
    filterParams_t params;
    if (dsp_processor_get_params_for_flow(current_flow, &params) == ESP_OK) {
      int32_t value = 0;
      
      // Map parameter name to value
      if (strcmp(param, "fc_1") == 0) {
        value = (int32_t)params.fc_1;
      } else if (strcmp(param, "gain_1") == 0) {
        value = (int32_t)params.gain_1;
      } else if (strcmp(param, "fc_2") == 0) {
        value = (int32_t)params.fc_2;
      } else if (strcmp(param, "gain_2") == 0) {
        value = (int32_t)params.gain_2;
      } else if (strcmp(param, "fc_3") == 0) {
        value = (int32_t)params.fc_3;
      } else if (strcmp(param, "gain_3") == 0) {
        value = (int32_t)params.gain_3;
      } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Unknown parameter");
        return ESP_OK;
      }
      
      char response[32];
      snprintf(response, sizeof(response), "%d", (int)value);
      httpd_resp_set_status(req, "200 OK");
      httpd_resp_set_type(req, "text/plain");
      httpd_resp_sendstr(req, response);
      ESP_LOGD(TAG, "%s: flow=%d %s=%d", __func__, current_flow, param, (int)value);
    } else {
      httpd_resp_set_status(req, "500 Internal Server Error");
      httpd_resp_sendstr(req, "0");
    }
#else
    // Fallback: load from NVS with flow-specific key
    int32_t active_flow_val = 0;
    dspFlows_t current_flow = dspfEQBassTreble;
    if (ui_http_load_param("active_flow", &active_flow_val) == ESP_OK) {
      current_flow = (dspFlows_t)active_flow_val;
    }
    
    char flow_key[32];
    make_flow_key(flow_key, sizeof(flow_key), current_flow, param);
    
    int32_t value = 0;
    if (ui_http_load_param(flow_key, &value) == ESP_OK) {
      char response[32];
      snprintf(response, sizeof(response), "%d", (int)value);
      httpd_resp_set_status(req, "200 OK");
      httpd_resp_set_type(req, "text/plain");
      httpd_resp_sendstr(req, response);
      ESP_LOGD(TAG, "%s: %s=%d", __func__, flow_key, (int)value);
    } else {
      httpd_resp_set_status(req, "404 Not Found");
      httpd_resp_sendstr(req, "0");
      ESP_LOGD(TAG, "%s: %s not found, returning 0", __func__, flow_key);
    }
#endif
  } else {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "error");
  }
  return ESP_OK;
}

/*
 * GET capabilities handler
 * Returns DSP capabilities as JSON: /capabilities
 */
static esp_err_t get_capabilities_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
  
  set_cors_headers(req);
  
#if CONFIG_USE_DSP_PROCESSOR
  char* capabilities_json = dsp_processor_get_capabilities_json();
  if (capabilities_json) {
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, capabilities_json);
    free(capabilities_json);
  } else {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\": \"Failed to generate capabilities\"}");
  }
#else
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"version\": \"1.0\", \"flows\": [], \"current_flow\": \"none\"}");
#endif
  
  return ESP_OK;
}

/*
 * favicon get handler
 * Returns 404 since we don't have a favicon
 */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
  set_cors_headers(req);
  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "No favicon available");
  return ESP_OK;
}

/*
 * OPTIONS handler for CORS preflight requests
 */
static esp_err_t options_handler(httpd_req_t *req) {
  ESP_LOGD(TAG, "%s: uri=%s", __func__, req->uri);
  set_cors_headers(req);
  httpd_resp_set_status(req, "204 No Content");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

/**
 */
esp_err_t stop_server(void) {
  ESP_LOGD(TAG, "%s", __func__);
  if (server) {
    httpd_stop(server);
    server = NULL;
  }

  return ESP_OK;
}

/**
 * Save a single integer parameter to NVS under namespace "ui_http".
 * Thread-safe with mutex protection.
 */
esp_err_t ui_http_save_param(const char *name, int32_t value) {
  ESP_LOGD(TAG, "%s: name=%s value=%d", __func__, name, (int)value);
  
  if (!nvs_mutex) {
    ESP_LOGE(TAG, "%s: NVS mutex not initialized", __func__);
    return ESP_ERR_INVALID_STATE;
  }

  // Acquire mutex with timeout to prevent deadlock
  if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "%s: Failed to acquire NVS mutex (timeout)", __func__);
    return ESP_ERR_TIMEOUT;
  }

  nvs_handle_t h;
  esp_err_t err = nvs_open("ui_http", NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: nvs_open failed: %s", __func__, esp_err_to_name(err));
    xSemaphoreGive(nvs_mutex);
    return err;
  }

  err = nvs_set_i32(h, name, value);
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  
  xSemaphoreGive(nvs_mutex);
  
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to save param '%s': %s", __func__, name, esp_err_to_name(err));
  }
  return err;
}

/**
 * Load a single integer parameter from NVS. Returns ESP_OK on success or
 * ESP_ERR_NVS_NOT_FOUND if not present.
 * Thread-safe with mutex protection.
 */
esp_err_t ui_http_load_param(const char *name, int32_t *value) {
  ESP_LOGD(TAG, "%s: name=%s", __func__, name);
  
  if (!nvs_mutex) {
    ESP_LOGE(TAG, "%s: NVS mutex not initialized", __func__);
    return ESP_ERR_INVALID_STATE;
  }

  // Acquire mutex with timeout to prevent deadlock
  if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "%s: Failed to acquire NVS mutex (timeout)", __func__);
    return ESP_ERR_TIMEOUT;
  }

  nvs_handle_t h;
  esp_err_t err = nvs_open("ui_http", NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "%s: nvs_open failed: %s", __func__, esp_err_to_name(err));
    xSemaphoreGive(nvs_mutex);
    return err;
  }

  int32_t tmp = 0;
  err = nvs_get_i32(h, name, &tmp);
  nvs_close(h);
  
  xSemaphoreGive(nvs_mutex);
  
  if (err == ESP_OK) {
    *value = tmp;
  } else {
    ESP_LOGD(TAG, "%s: nvs_get_i32('%s') -> %s", __func__, name, esp_err_to_name(err));
  }
  return err;
}

/*
 * Function to start the web server
 */
esp_err_t start_server(const char *base_path, int port) {
  ESP_LOGD(TAG, "%s: base_path=%s port=%d", __func__, base_path, port);
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.max_open_sockets = 7;  // Increased from 2 to handle concurrent requests better
  config.lru_purge_enable = true;  // Enable LRU socket purging

  /* Don't use wildcard matching - it doesn't work well with query strings
   * Instead, the HTTP server will strip query strings before matching */
  // config.uri_match_fn = httpd_uri_match_wildcard;

  ESP_LOGI(TAG, "%s: Starting HTTP Server on port: '%d'", __func__, config.server_port);
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to start file server!", __func__);
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

  /* URI handler for get parameter */
  httpd_uri_t _get_param_handler = {
      .uri = "/get", .method = HTTP_GET, .handler = get_param_handler,
  };
  httpd_register_uri_handler(server, &_get_param_handler);

  /* URI handler for capabilities */
  httpd_uri_t _get_capabilities_handler = {
      .uri = "/capabilities", .method = HTTP_GET, .handler = get_capabilities_handler,
  };
  httpd_register_uri_handler(server, &_get_capabilities_handler);

  /* URI handler for favicon.ico */
  httpd_uri_t _favicon_get_handler = {
      .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler,
      //.user_ctx  = server_data	// Pass server data as context
  };
  httpd_register_uri_handler(server, &_favicon_get_handler);

  /* URI handler for OPTIONS (CORS preflight) - specific endpoints */
  httpd_uri_t _options_post_handler = {
      .uri = "/post", .method = HTTP_OPTIONS, .handler = options_handler,
  };
  httpd_register_uri_handler(server, &_options_post_handler);

  httpd_uri_t _options_get_handler = {
      .uri = "/get", .method = HTTP_OPTIONS, .handler = options_handler,
  };
  httpd_register_uri_handler(server, &_options_get_handler);

  httpd_uri_t _options_capabilities_handler = {
      .uri = "/capabilities", .method = HTTP_OPTIONS, .handler = options_handler,
  };
  httpd_register_uri_handler(server, &_options_capabilities_handler);

  return ESP_OK;
}

/**
 * HTTP Server task - manages DSP parameters with flow-specific storage
 */
static void http_server_task(void *pvParameters) {
  ESP_LOGD(TAG, "%s: started", __func__);
  // Start Server
  ESP_ERROR_CHECK(start_server("/html", CONFIG_WEB_PORT));

  // Load last active flow from NVS
  int32_t tmpv;
  dspFlows_t active_flow = dspfEQBassTreble;  // default
  if (ui_http_load_param("active_flow", &tmpv) == ESP_OK) {
    active_flow = (dspFlows_t)tmpv;
    ESP_LOGI(TAG, "%s: Loaded active flow: %d", __func__, active_flow);
  }

  // Load persisted parameters for all flows from NVS
  char key[32];
  for (int flow = 0; flow < 6; flow++) {
    filterParams_t params;
    params.dspFlow = (dspFlows_t)flow;
    
    // Initialize with defaults from DSP processor
#if CONFIG_USE_DSP_PROCESSOR
    dsp_processor_get_params_for_flow((dspFlows_t)flow, &params);
#endif
    
    // Try to load persisted values (flow-specific keys)
    make_flow_key(key, sizeof(key), (dspFlows_t)flow, "fc_1");
    if (ui_http_load_param(key, &tmpv) == ESP_OK) {
      params.fc_1 = (float)tmpv;
    }
    
    make_flow_key(key, sizeof(key), (dspFlows_t)flow, "gain_1");
    if (ui_http_load_param(key, &tmpv) == ESP_OK) {
      params.gain_1 = (float)tmpv;
    }
    
    make_flow_key(key, sizeof(key), (dspFlows_t)flow, "fc_2");
    if (ui_http_load_param(key, &tmpv) == ESP_OK) {
      params.fc_2 = (float)tmpv;
    }
    
    make_flow_key(key, sizeof(key), (dspFlows_t)flow, "gain_2");
    if (ui_http_load_param(key, &tmpv) == ESP_OK) {
      params.gain_2 = (float)tmpv;
    }
    
    make_flow_key(key, sizeof(key), (dspFlows_t)flow, "fc_3");
    if (ui_http_load_param(key, &tmpv) == ESP_OK) {
      params.fc_3 = (float)tmpv;
    }
    
    make_flow_key(key, sizeof(key), (dspFlows_t)flow, "gain_3");
    if (ui_http_load_param(key, &tmpv) == ESP_OK) {
      params.gain_3 = (float)tmpv;
    }
    
    // Store in DSP processor's centralized storage
#if CONFIG_USE_DSP_PROCESSOR
    dsp_processor_set_params_for_flow((dspFlows_t)flow, &params);
#endif
    
    ESP_LOGI(TAG, "%s: Loaded flow %d: fc_1=%.1f gain_1=%.1f fc_3=%.1f gain_3=%.1f", 
             __func__, flow, params.fc_1, params.gain_1, params.fc_3, params.gain_3);
  }

#if CONFIG_USE_DSP_PROCESSOR
  // Switch to the active flow (this applies its parameters)
  dsp_processor_switch_flow(active_flow);
  ESP_LOGI(TAG, "%s: Switched to flow %d", __func__, active_flow);
#endif

  // Get current active parameters
  filterParams_t current_params;
#if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_get_params_for_flow(active_flow, &current_params);
#else
  memset(&current_params, 0, sizeof(filterParams_t));
  current_params.dspFlow = active_flow;
#endif

  URL_t urlBuf;
  while (1) {
    // Waiting for post
    if (xQueueReceive(xQueueHttp, &urlBuf, portMAX_DELAY) == pdTRUE) {
      ESP_LOGI(TAG, "%s: received update: %s = %d", __func__, urlBuf.key, urlBuf.int_value);

      // Handle flow change specially
      if (strcmp(urlBuf.key, "dspFlow") == 0) {
        dspFlows_t new_flow = (dspFlows_t)urlBuf.int_value;
        
        // Save current flow ID to NVS
        if (ui_http_save_param("active_flow", (int32_t)new_flow) != ESP_OK) {
          ESP_LOGW(TAG, "%s: Failed to persist active_flow to NVS", __func__);
        }
        
#if CONFIG_USE_DSP_PROCESSOR
        // Switch to new flow (loads its stored parameters)
        dsp_processor_switch_flow(new_flow);
        // Get the parameters for the new flow
        dsp_processor_get_params_for_flow(new_flow, &current_params);
#else
        current_params.dspFlow = new_flow;
#endif
        
        ESP_LOGI(TAG, "%s: Switched to flow %d", __func__, new_flow);
        continue;
      }

      // Handle parameter updates for current flow
      bool param_recognized = false;
      dspFlows_t current_flow = current_params.dspFlow;
      
      if (strcmp(urlBuf.key, "fc_1") == 0) {
        current_params.fc_1 = (float)urlBuf.int_value;
        param_recognized = true;
      } else if (strcmp(urlBuf.key, "gain_1") == 0) {
        current_params.gain_1 = (float)urlBuf.int_value;
        param_recognized = true;
      } else if (strcmp(urlBuf.key, "fc_2") == 0) {
        current_params.fc_2 = (float)urlBuf.int_value;
        param_recognized = true;
      } else if (strcmp(urlBuf.key, "gain_2") == 0) {
        current_params.gain_2 = (float)urlBuf.int_value;
        param_recognized = true;
      } else if (strcmp(urlBuf.key, "fc_3") == 0) {
        current_params.fc_3 = (float)urlBuf.int_value;
        param_recognized = true;
      } else if (strcmp(urlBuf.key, "gain_3") == 0) {
        current_params.gain_3 = (float)urlBuf.int_value;
        param_recognized = true;
      }

      if (!param_recognized) {
        ESP_LOGW(TAG, "%s: Unknown param '%s' received, ignoring", __func__, urlBuf.key);
        continue;
      }

#if CONFIG_USE_DSP_PROCESSOR
      // Apply updated params to DSP
      dsp_processor_set_params_for_flow(current_flow, &current_params);
#endif

      // Persist with flow-specific key
      make_flow_key(key, sizeof(key), current_flow, urlBuf.key);
      if (ui_http_save_param(key, urlBuf.int_value) != ESP_OK) {
        ESP_LOGW(TAG, "%s: Failed to persist param '%s' to NVS", __func__, key);
      } else {
        ESP_LOGD(TAG, "%s: Saved %s = %d to NVS", __func__, key, urlBuf.int_value);
      }
    }
  }

  // Never reach here
  ESP_LOGI(TAG, "%s: finish", __func__);
  vTaskDelete(NULL);
}

/**
 *
 */
void init_http_server_task(void) {
  ESP_LOGD(TAG, "%s: initializing", __func__);
  
  // Create NVS mutex if not already created
  if (!nvs_mutex) {
    nvs_mutex = xSemaphoreCreateMutex();
    if (!nvs_mutex) {
      ESP_LOGE(TAG, "%s: Failed to create NVS mutex", __func__);
      return;
    }
  }
  
  // Initialize SPIFFS
  ESP_LOGI(TAG, "%s: Initializing SPIFFS", __func__);
  if (SPIFFS_Mount("/html", "storage", 6) != ESP_OK) {
    ESP_LOGE(TAG, "%s: SPIFFS mount failed", __func__);
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

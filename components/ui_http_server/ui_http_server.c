/* HTTP Server Example

         This example code is in the Public Domain (or CC0 licensed, at your
   option.)

         Unless required by applicable law or agreed to in writing, this
         software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
         CONDITIONS OF ANY KIND, either express or implied.
*/

#include "ui_http_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "dsp_processor.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "HTTP";

static QueueHandle_t xQueueHttp;

static esp_netif_t *netInterface = NULL;

extern const char html_index_html_start[] asm("_binary_index_html_start");
extern const char html_index_html_end[] asm("_binary_index_html_end");

#if CONFIG_HTTP_AUTH_ENABLE
static esp_err_t build_expected_auth_header(char *encoded,
                                            size_t encoded_size) {
  char credentials[128];
  size_t output_len = 0;
  int credentials_len =
      snprintf(credentials, sizeof(credentials), "%s:%s",
               CONFIG_HTTP_AUTH_USERNAME, CONFIG_HTTP_AUTH_PASSWORD);

  if ((credentials_len < 0) || (credentials_len >= sizeof(credentials))) {
    ESP_LOGE(TAG, "HTTP auth credentials are too long");
    return ESP_FAIL;
  }

  if (mbedtls_base64_encode((unsigned char *)encoded, encoded_size, &output_len,
                            (const unsigned char *)credentials,
                            credentials_len) != 0) {
    ESP_LOGE(TAG, "failed to encode HTTP auth credentials");
    return ESP_FAIL;
  }

  if (output_len >= encoded_size) {
    return ESP_FAIL;
  }

  encoded[output_len] = '\0';
  return ESP_OK;
}

static esp_err_t require_http_auth(httpd_req_t *req) {
  char header_value[192];
  char expected_value[192];
  int header_len = httpd_req_get_hdr_value_len(req, "Authorization");

  if (build_expected_auth_header(expected_value, sizeof(expected_value)) !=
      ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Auth setup failed");
    return ESP_FAIL;
  }

  if ((header_len <= 0) || (header_len >= sizeof(header_value))) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"snapclient\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_ERR_INVALID_STATE;
  }

  if (httpd_req_get_hdr_value_str(req, "Authorization", header_value,
                                  sizeof(header_value)) != ESP_OK) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"snapclient\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_ERR_INVALID_STATE;
  }

  if (strncmp(header_value, "Basic ", strlen("Basic ")) != 0) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"snapclient\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_ERR_INVALID_STATE;
  }

  if (strcmp(header_value + strlen("Basic "), expected_value) != 0) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"snapclient\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_ERR_INVALID_STATE;
  }

  return ESP_OK;
}
#else
static esp_err_t require_http_auth(httpd_req_t *req) {
  (void)req;
  return ESP_OK;
}
#endif

static float clamp_gain_db(float gain) {
  if (gain < -6.0f) {
    return -6.0f;
  }

  if (gain > 6.0f) {
    return 6.0f;
  }

  return gain;
}

static void get_current_filter_params(filterParams_t *params) {
  memset(params, 0, sizeof(*params));
  params->dspFlow = dspfEQBassTreble;

#if CONFIG_USE_DSP_PROCESSOR
  if (dsp_processor_get_filter_params(params) != ESP_OK) {
    ESP_LOGW(TAG, "using default DSP filter values for UI rendering");
    memset(params, 0, sizeof(*params));
    params->dspFlow = dspfEQBassTreble;
  }
#endif
}

static void replace_template_token(char *line, size_t line_size,
                                   const char *token, const char *value) {
  char buffer[512];
  char *pos = NULL;

  while ((pos = strstr(line, token)) != NULL) {
    size_t prefix_len = pos - line;
    const char *suffix = pos + strlen(token);

    if ((prefix_len + strlen(value) + strlen(suffix) + 1) > sizeof(buffer)) {
      ESP_LOGW(TAG, "template substitution truncated for token %s", token);
      return;
    }

    memcpy(buffer, line, prefix_len);
    buffer[prefix_len] = '\0';
    strcat(buffer, value);
    strcat(buffer, suffix);

    strncpy(line, buffer, line_size - 1);
    line[line_size - 1] = '\0';
  }
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
static esp_err_t Text2Html(httpd_req_t *req) {
  filterParams_t filterParams;
  char gain_1[16];
  char gain_2[16];
  char gain_3[16];
  char line[512];
  size_t line_len = 0;
  const char *cursor = html_index_html_start;

  get_current_filter_params(&filterParams);
  snprintf(gain_1, sizeof(gain_1), "%.2f", filterParams.gain_1);
  snprintf(gain_2, sizeof(gain_2), "%.2f", filterParams.gain_2);
  snprintf(gain_3, sizeof(gain_3), "%.2f", filterParams.gain_3);

  while (cursor < html_index_html_end) {
    char ch = *cursor++;

    if (ch == '\0') {
      break;
    }

    line[line_len++] = ch;

    if ((ch == '\n') || (line_len == (sizeof(line) - 1))) {
      line[line_len] = '\0';

      replace_template_token(line, sizeof(line), "__GAIN_1__", gain_1);
      replace_template_token(line, sizeof(line), "__GAIN_2__", gain_2);
      replace_template_token(line, sizeof(line), "__GAIN_3__", gain_3);

      esp_err_t ret = httpd_resp_send_chunk(req, line, strlen(line));
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_resp_sendstr_chunk fail %d", ret);
        return ret;
      }

      line_len = 0;
    }
  }

  if (line_len > 0) {
    line[line_len] = '\0';

    replace_template_token(line, sizeof(line), "__GAIN_1__", gain_1);
    replace_template_token(line, sizeof(line), "__GAIN_2__", gain_2);
    replace_template_token(line, sizeof(line), "__GAIN_3__", gain_3);

    esp_err_t ret = httpd_resp_send_chunk(req, line, strlen(line));
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "httpd_resp_sendstr_chunk fail %d", ret);
      return ret;
    }
  }

  return ESP_OK;
}

/**
 * HTTP get handler
 */
static esp_err_t root_get_handler(httpd_req_t *req) {
  //	ESP_LOGI(TAG, "root_get_handler req->uri=[%s]", req->uri);

  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }

  /* Send index.html */
  Text2Html(req);

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

  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }

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
#ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
  httpd_resp_set_hdr(req, "Connection", "close");
#endif
  httpd_resp_sendstr(req, "post successfully");
  return ESP_OK;
}

/*
 * favicon get handler
 */
static esp_err_t favicon_get_handler(httpd_req_t *req) {
  //	ESP_LOGI(TAG, "favicon_get_handler req->uri=[%s]", req->uri);
  if (require_http_auth(req) != ESP_OK) {
    return ESP_OK;
  }
  return ESP_OK;
}

/*
 * Function to start the web server
 */
esp_err_t start_server(const char *base_path, int port) {
  httpd_handle_t server = NULL;
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

//// LEDC Stuff
//#define LEDC_TIMER			LEDC_TIMER_0
//#define LEDC_MODE			LEDC_LOW_SPEED_MODE
////#define LEDC_OUTPUT_IO	(5) // Define the output GPIO
//#define LEDC_OUTPUT_IO		CONFIG_BLINK_GPIO // Define the output
// GPIO #define LEDC_CHANNEL		LEDC_CHANNEL_0 #define LEDC_DUTY_RES
// LEDC_TIMER_13_BIT // Set duty resolution to 13 bits #define LEDC_DUTY
//(4095) // Set duty to 50%. ((2 ** 13) - 1) * 50% = 4095 #define LEDC_FREQUENCY
//(5000) // Frequency in Hertz. Set frequency at 5 kHz
//
// static void ledc_init(void)
//{
//	// Prepare and then apply the LEDC PWM timer configuration
//	ledc_timer_config_t ledc_timer = {
//		.speed_mode			= LEDC_MODE,
//		.timer_num			= LEDC_TIMER,
//		.duty_resolution	= LEDC_DUTY_RES,
//		.freq_hz			= LEDC_FREQUENCY,  // Set output
// frequency at 5 kHz 		.clk_cfg			= LEDC_AUTO_CLK
//	};
//	ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
//
//	// Prepare and then apply the LEDC PWM channel configuration
//	ledc_channel_config_t ledc_channel = {
//		.speed_mode			= LEDC_MODE,
//		.channel			= LEDC_CHANNEL,
//		.timer_sel			= LEDC_TIMER,
//		.intr_type			= LEDC_INTR_DISABLE,
//		.gpio_num			= LEDC_OUTPUT_IO,
//		.duty				= 0, // Set duty to 0%
//		.hpoint				= 0
//	};
//	ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
//}

/**
 *
 */
static void http_server_task(void *pvParameters) {
  /* Get the local IP address */
  esp_netif_ip_info_t ip_info;

  ESP_ERROR_CHECK(esp_netif_get_ip_info(netInterface, &ip_info));

  char ipString[64];
  sprintf(ipString, IPSTR, IP2STR(&ip_info.ip));

  ESP_LOGI(TAG, "Start http task=%s", ipString);

  char portString[6];
  sprintf(portString, "%d", CONFIG_WEB_PORT);

  char url[strlen("http://") + strlen(ipString) + strlen(":") +
           strlen(portString) + 1];
  memset(url, 0, sizeof(url));
  strcat(url, ipString);
  strcat(url, ":");
  strcat(url, portString);

  // Set the LEDC peripheral configuration
  //	ledc_init();

  // Set duty to 50%
  //	double maxduty = pow(2, 13) - 1;
  //	float percent = 0.5;
  //	uint32_t duty = maxduty * percent;
  //	ESP_LOGI(TAG, "duty=%"PRIu32, duty);
  //	ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY));
  //	ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
  // Update duty to apply the new value
  //	ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));

  // Start Server
  ESP_LOGI(TAG, "Starting server on %s", url);
  ESP_ERROR_CHECK(start_server("/html", CONFIG_WEB_PORT));

  URL_t urlBuf;
  while (1) {
    //	  ESP_LOGW (TAG, "stack free: %d", uxTaskGetStackHighWaterMark(NULL));

    // Waiting for post
    if (xQueueReceive(xQueueHttp, &urlBuf, portMAX_DELAY) == pdTRUE) {
      filterParams_t filterParams;

      ESP_LOGI(TAG, "str_value=%s gain_1=%f, gain_2=%f, gain_3=%f",
               urlBuf.str_value, urlBuf.gain_1, urlBuf.gain_2, urlBuf.gain_3);

#if CONFIG_USE_DSP_PROCESSOR
      get_current_filter_params(&filterParams);
      filterParams.dspFlow = dspfEQBassTreble;
      filterParams.gain_1 = clamp_gain_db(urlBuf.gain_1);
      filterParams.gain_2 = clamp_gain_db(urlBuf.gain_2);
      filterParams.gain_3 = clamp_gain_db(urlBuf.gain_3);

      if (dsp_processor_update_filter_params(&filterParams) != ESP_OK) {
        ESP_LOGE(TAG, "failed to update DSP filter parameters");
      }
#endif

      // Set duty value
      //			percent = urlBuf.long_value / 100.0;
      //			duty = maxduty * percent;
      //			ESP_LOGI(TAG, "percent=%f duty=%"PRIu32,
      // percent, duty);
      // ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty));
      // Update duty to apply the new value
      //			ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE,
      // LEDC_CHANNEL));
    }
  }

  // Never reach here
  ESP_LOGI(TAG, "finish");
  vTaskDelete(NULL);
}

/**
 *
 */
void init_http_server_task(char *key) {
  if (!key) {
    ESP_LOGE(TAG,
             "key should be \"WIFI_STA_DEF\", \"WIFI_AP_DEF\" or \"ETH_DEF\"");
    return;
  }

  netInterface = esp_netif_get_handle_from_ifkey(key);
  if (!netInterface) {
    ESP_LOGE(TAG, "can't get net interface for %s", key);
    return;
  }

  // Create Queue
  xQueueHttp = xQueueCreate(10, sizeof(URL_t));
  configASSERT(xQueueHttp);

  xTaskCreatePinnedToCore(http_server_task, "HTTP", 512 * 5, NULL, 2, NULL,
                          tskNO_AFFINITY);
}

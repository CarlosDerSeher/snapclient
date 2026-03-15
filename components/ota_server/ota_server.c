// https://github.com/yanbe/esp-idf-ota-template/blob/master/components/ota_server/ota_server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define FIRMWARE_REV " Rev: 0.1"

#include "dsp_processor.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "ota_server.h"
#include "player.h"

extern TaskHandle_t t_http_get_task;

const int OTA_CONNECTED_BIT = BIT0;
static const char *TAG = "OTA";
EventGroupHandle_t ota_event_group;
/*socket*/
static int connect_socket = -1;

void ota_server_task(void *param) {
  // xEventGroupWaitBits(ota_event_group, OTA_CONNECTED_BIT, false, true,
  // portMAX_DELAY);

  // TODO: find a good place to verify app is working properly after OTA
  esp_ota_mark_app_valid_cancel_rollback();

  while (1) {
    ota_server_start_my();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

/*
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
        switch (event->event_id)
        {
        case SYSTEM_EVENT_STA_START:
                esp_wifi_connect();
        printf("Connectiing To SSID:%s : Pass:%s\r\n", CONFIG_STATION_SSID,
CONFIG_STATION_PASSPHRASE); break; case SYSTEM_EVENT_STA_GOT_IP: printf("got
ip:%s",	ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
                xEventGroupSetBits(ota_event_group, OTA_CONNECTED_BIT);
                break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
        printf("SYSTEM_EVENT_STA_DISCONNECTED\r\n");
                esp_wifi_connect();
                xEventGroupClearBits(ota_event_group, OTA_CONNECTED_BIT);
                break;
        default:
                break;
        }
        return ESP_OK;
}

void initialise_wifi(void)
{
        ota_event_group = xEventGroupCreate();

        tcpip_adapter_init();
        ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        wifi_config_t sta_config = {
                .sta = {
                        .ssid = CONFIG_STATION_SSID,
                        .password = CONFIG_STATION_PASSPHRASE,
                        .bssid_set = false
                        }
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_ERROR_CHECK(esp_wifi_start());
}
*/

static int get_socket_error_code(int socket) {
  int result;
  u32_t optlen = sizeof(int);

  int err = getsockopt(socket, SOL_SOCKET, SO_ERROR, &result, &optlen);

  if (err == -1) {
    ESP_LOGE(TAG, "getsockopt failed:%s", strerror(err));
    return -1;
  }
  return result;
}

static int show_socket_error_reason(const char *str, int socket) {
  int err = get_socket_error_code(socket);

  if (err != 0) {
    ESP_LOGW(TAG, "%s socket error %d %s", str, err, strerror(err));
  }

  return err;
}

static void close_socket_if_open(int *socket) {
  if ((socket != NULL) && (*socket >= 0)) {
    close(*socket);
    *socket = -1;
  }
}

static esp_err_t send_http_response(int socket, const char *status,
                                    const char *extra_headers,
                                    const char *body) {
  char response[256];
  const char *headers = extra_headers ? extra_headers : "";
  const char *payload = body ? body : "";
  size_t payload_len = strlen(payload);
  int response_len = snprintf(response, sizeof(response),
                              "HTTP/1.1 %s\r\n"
                              "Connection: close\r\n"
                              "Content-Length: %u\r\n"
                              "%s"
                              "\r\n"
                              "%s",
                              status, (unsigned int)payload_len, headers,
                              payload);

  if ((response_len < 0) || (response_len >= sizeof(response))) {
    ESP_LOGE(TAG, "failed to compose HTTP response");
    return ESP_FAIL;
  }

  if (send(socket, response, response_len, 0) < 0) {
    ESP_LOGW(TAG, "failed to send HTTP response: errno=%d", errno);
    return ESP_FAIL;
  }

  return ESP_OK;
}

static esp_err_t parse_content_length(const char *request, int *content_length) {
  const char *header = strstr(request, "Content-Length:");

  if ((header == NULL) || (content_length == NULL)) {
    return ESP_ERR_NOT_FOUND;
  }

  header += strlen("Content-Length:");
  while (*header == ' ') {
    header++;
  }

  *content_length = atoi(header);
  if (*content_length <= 0) {
    return ESP_ERR_INVALID_SIZE;
  }

  return ESP_OK;
}

#if CONFIG_HTTP_AUTH_ENABLE
static esp_err_t build_expected_auth_header(char *encoded,
                                            size_t encoded_size) {
  char credentials[128];
  size_t output_len = 0;
  int credentials_len =
      snprintf(credentials, sizeof(credentials), "%s:%s",
               CONFIG_HTTP_AUTH_USERNAME, CONFIG_HTTP_AUTH_PASSWORD);

  if ((credentials_len < 0) || (credentials_len >= sizeof(credentials))) {
    ESP_LOGE(TAG, "OTA auth credentials are too long");
    return ESP_FAIL;
  }

  if (mbedtls_base64_encode((unsigned char *)encoded, encoded_size, &output_len,
                            (const unsigned char *)credentials,
                            credentials_len) != 0) {
    ESP_LOGE(TAG, "failed to encode OTA auth credentials");
    return ESP_FAIL;
  }

  if (output_len >= encoded_size) {
    return ESP_FAIL;
  }

  encoded[output_len] = '\0';
  return ESP_OK;
}

static bool request_has_valid_auth(const char *request) {
  char expected_auth[192];
  const char *header = NULL;
  const char *value = NULL;
  const char *line_end = NULL;
  size_t provided_len = 0;

  if (build_expected_auth_header(expected_auth, sizeof(expected_auth)) !=
      ESP_OK) {
    return false;
  }

  header = strstr(request, "\r\nAuthorization:");
  if (header == NULL) {
    header = strstr(request, "Authorization:");
  }
  if (header == NULL) {
    return false;
  }

  value = header + strlen("Authorization:");
  while (*value == ' ') {
    value++;
  }

  if (strncmp(value, "Basic ", strlen("Basic ")) != 0) {
    return false;
  }

  value += strlen("Basic ");
  line_end = strstr(value, "\r\n");
  if (line_end == NULL) {
    return false;
  }

  provided_len = line_end - value;
  while ((provided_len > 0) &&
         ((value[provided_len - 1] == ' ') ||
          (value[provided_len - 1] == '\t'))) {
    provided_len--;
  }

  return (provided_len == strlen(expected_auth)) &&
         (memcmp(value, expected_auth, provided_len) == 0);
}
#else
static bool request_has_valid_auth(const char *request) {
  (void)request;
  return true;
}
#endif

static esp_err_t create_tcp_server() {
#if CONFIG_HTTP_AUTH_ENABLE
  ESP_LOGI(TAG,
           "idf.py build ; curl -u %s:<password> http://snapclient.local:%d/ "
           "--data-binary @build/snapclient.bin",
           CONFIG_HTTP_AUTH_USERNAME, OTA_LISTEN_PORT);
#else
  ESP_LOGI(TAG,
           "idf.py build ; curl snapclient.local:%d --data-binary @- < "
           "build/snapclient.bin",
           OTA_LISTEN_PORT);
#endif
  int server_socket = 0;
  int reuse = 1;
  struct sockaddr_in server_addr;
  server_socket = socket(AF_INET, SOCK_STREAM, 0);

  if (server_socket < 0) {
    show_socket_error_reason("create_server", server_socket);
    return ESP_FAIL;
  }

  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(OTA_LISTEN_PORT);
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(server_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    show_socket_error_reason("bind_server", server_socket);
    close(server_socket);
    return ESP_FAIL;
  }

  if (listen(server_socket, 5) < 0) {
    show_socket_error_reason("listen_server", server_socket);
    close(server_socket);
    return ESP_FAIL;
  }

  struct sockaddr_in client_addr;
  socklen_t socklen = sizeof(client_addr);
  connect_socket = -1;
  connect_socket =
      accept(server_socket, (struct sockaddr *)&client_addr, &socklen);
  close(server_socket);

  if (connect_socket < 0) {
    show_socket_error_reason("accept_server", connect_socket);
    return ESP_FAIL;
  }
  /*connection established，now can send/recv*/
  ESP_LOGI(TAG, "tcp connection established!");
  return ESP_OK;
}

esp_err_t ota_server_start_my(void) {
  uint8_t percent_loaded = 0;
  uint8_t old_percent_loaded = 0xFF;
  int recv_len = 0;
  char ota_buff[OTA_BUFF_SIZE + 1] = {0};
  int content_length = -1;
  int content_received = 0;
  bool ota_started = false;
  bool player_stopped = false;
  bool response_sent = false;
  esp_err_t err = ESP_FAIL;
  esp_ota_handle_t ota_handle = 0;
  const esp_partition_t *update_partition = NULL;
  const char *header_end = NULL;

  err = create_tcp_server();
  if (err != ESP_OK) {
    return err;
  }

  recv_len = recv(connect_socket, ota_buff, OTA_BUFF_SIZE, 0);
  if (recv_len <= 0) {
    ESP_LOGW(TAG, "failed to receive OTA request header: errno=%d", errno);
    goto cleanup;
  }
  ota_buff[recv_len] = '\0';

  if (!request_has_valid_auth(ota_buff)) {
    ESP_LOGW(TAG, "rejecting OTA upload due to missing or invalid credentials");
    send_http_response(connect_socket, "401 Unauthorized",
                       "WWW-Authenticate: Basic realm=\"ota\"\r\n",
                       "Unauthorized");
    response_sent = true;
    err = ESP_ERR_INVALID_STATE;
    goto cleanup;
  }

  header_end = strstr(ota_buff, "\r\n\r\n");
  if (header_end == NULL) {
    ESP_LOGE(TAG, "invalid OTA request: missing HTTP header terminator");
    send_http_response(connect_socket, "400 Bad Request", NULL,
                       "Missing HTTP headers");
    response_sent = true;
    err = ESP_ERR_INVALID_RESPONSE;
    goto cleanup;
  }

  err = parse_content_length(ota_buff, &content_length);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "invalid OTA request: missing or bad Content-Length");
    send_http_response(connect_socket, "400 Bad Request", NULL,
                       "Missing Content-Length");
    response_sent = true;
    goto cleanup;
  }
  ESP_LOGI(TAG, "Detected content length: %d", content_length);

  update_partition = esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    ESP_LOGE(TAG, "no OTA update partition available");
    send_http_response(connect_socket, "500 Internal Server Error", NULL,
                       "No OTA partition");
    response_sent = true;
    err = ESP_FAIL;
    goto cleanup;
  }

  ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx",
           update_partition->subtype, update_partition->address);

  esp_log_level_set("esp_image", ESP_LOG_ERROR);

  if (t_http_get_task != NULL) {
    vTaskDelete(t_http_get_task);
    t_http_get_task = NULL;
  }
  deinit_player();
  player_stopped = true;

  err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    send_http_response(connect_socket, "500 Internal Server Error", NULL,
                       "OTA start failed");
    response_sent = true;
    goto cleanup;
  }
  ota_started = true;

  {
    const char *body_start = header_end + strlen("\r\n\r\n");
    int body_part_len = recv_len - (body_start - ota_buff);

    if (body_part_len > 0) {
      err = esp_ota_write(ota_handle, body_start, body_part_len);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        send_http_response(connect_socket, "500 Internal Server Error", NULL,
                           "OTA write failed");
        response_sent = true;
        goto cleanup;
      }
      content_received += body_part_len;
    }
  }

  while (content_received < content_length) {
    recv_len = recv(connect_socket, ota_buff, OTA_BUFF_SIZE, 0);
    if (recv_len <= 0) {
      ESP_LOGE(TAG, "OTA upload interrupted: errno=%d", errno);
      err = ESP_FAIL;
      break;
    }

    err = esp_ota_write(ota_handle, ota_buff, recv_len);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
      send_http_response(connect_socket, "500 Internal Server Error", NULL,
                         "OTA write failed");
      response_sent = true;
      goto cleanup;
    }

    content_received += recv_len;
    percent_loaded =
        (uint8_t)(((float)content_received / (float)content_length) * 100.0f);

    if (((percent_loaded % 10) == 0) && (percent_loaded != old_percent_loaded)) {
      old_percent_loaded = percent_loaded;
      ESP_LOGI(TAG, "Uploaded %03u%%", percent_loaded);
    }
  }

  if ((err != ESP_OK) || (content_received != content_length)) {
    if (!response_sent) {
      send_http_response(connect_socket, "400 Bad Request", NULL,
                         "Upload interrupted");
      response_sent = true;
    }
    err = ESP_FAIL;
    goto cleanup;
  }

  ESP_LOGI(TAG, "OTA Transferred Finished: %d bytes", content_received);

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    send_http_response(connect_socket, "500 Internal Server Error", NULL,
                       "OTA finalize failed");
    response_sent = true;
    goto cleanup;
  }
  ota_started = false;

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
             esp_err_to_name(err));
    send_http_response(connect_socket, "500 Internal Server Error", NULL,
                       "Boot partition switch failed");
    response_sent = true;
    goto cleanup;
  }

  {
    const esp_partition_t *boot_partition = esp_ota_get_boot_partition();

    ESP_LOGI(TAG,
             "***********************************************************");
    ESP_LOGI(TAG, "OTA Successful");
    ESP_LOGI(TAG, "Next Boot Partition Subtype %d At Offset 0x%lx",
             boot_partition->subtype, boot_partition->address);
    ESP_LOGI(TAG,
             "***********************************************************");
  }

  send_http_response(connect_socket, "200 OK", NULL, "OTA Successful");
  response_sent = true;
  vTaskDelay(pdMS_TO_TICKS(200));
  close_socket_if_open(&connect_socket);

  ESP_LOGI(TAG, "Prepare to restart system..");
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();

cleanup:
  if (ota_started) {
    esp_ota_abort(ota_handle);
  }

  if (!response_sent && (connect_socket >= 0)) {
    send_http_response(connect_socket, "500 Internal Server Error", NULL,
                       "OTA failed");
  }

  close_socket_if_open(&connect_socket);

  if (player_stopped) {
    ESP_LOGW(TAG, "OTA session failed after stopping audio, restarting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
  }

  return err;
}

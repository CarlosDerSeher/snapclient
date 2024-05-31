/*
    Wifi related functionality
    Connect to pre defined wifi

    Must be taken over/merge with wifi provision
*/
#include "wifi_interface.h"

// #include "esp_event.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#if ENABLE_WIFI_PROVISIONING
#include <string.h>  // for memcpy
#include "wifi_provisioning.h"
#endif

static const char *TAG = "WIFI";

static void reset_reason_timer_counter_cb(void *);

static char mac_address[18];

EventGroupHandle_t s_wifi_event_group;

static int s_retry_num = 0;

static esp_netif_t *esp_wifi_netif = NULL;

#if ENABLE_WIFI_PROVISIONING
static esp_timer_handle_t resetReasonTimerHandle = NULL;
static const esp_timer_create_args_t resetReasonTimerArgs = {
    .callback = &reset_reason_timer_counter_cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "rstCnt",
    .skip_unhandled_events = false};

static uint8_t resetReasonCounter = 0;

static void reset_reason_timer_counter_cb(void *args) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: Error (%s) opening NVS handle!", __func__,
             esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "resetting POR reset counter ...");

    resetReasonCounter = 0;

    err |= nvs_set_u8(nvs_handle, "restart_counter", resetReasonCounter);
    err |= nvs_commit(nvs_handle);
    ESP_LOGI(TAG, "%s", (err != ESP_OK) ? "Failed!" : "Done");

    nvs_close(nvs_handle);
  }

  esp_timer_delete(resetReasonTimerHandle);
}
#endif

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */

// Event handler for catching system events
static void event_handler(void *arg, esp_event_base_t event_base, int event_id,
                          void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Connected with IP Address:" IPSTR,
             IP2STR(&event->ip_info.ip));

    s_retry_num = 0;
    // Signal main application to continue execution
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if ((s_retry_num < WIFI_MAXIMUM_RETRY) || (WIFI_MAXIMUM_RETRY == 0)) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "connect to the AP fail");
  }
}

void wifi_init(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());

  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, (esp_event_handler_t)&event_handler, NULL));
  ESP_ERROR_CHECK(
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 (esp_event_handler_t)&event_handler, NULL));

  esp_wifi_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // esp_wifi_set_bandwidth (WIFI_IF_STA, WIFI_BW_HT20);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
  esp_wifi_set_protocol(
      WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  // esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  //   esp_wifi_set_ps(WIFI_PS_NONE);

#if ENABLE_WIFI_PROVISIONING
  esp_reset_reason_t resetReason = esp_reset_reason();
  ESP_LOGI(TAG, "reset reason was: %d", resetReason);
  esp_timer_create(&resetReasonTimerArgs, &resetReasonTimerHandle);
  esp_timer_start_once(resetReasonTimerHandle, 5000000);
  if (resetReason == ESP_RST_POWERON) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error (%s) opening NVS handle!", __func__,
               esp_err_to_name(err));
    } else {
      ESP_LOGI(TAG, "get POR reset counter ...");
      err |= nvs_get_u8(nvs_handle, "restart_counter", &resetReasonCounter);

      ESP_LOGI(TAG, "reset counter %d", resetReasonCounter);

      resetReasonCounter++;

      if (resetReasonCounter > 3) {
        ESP_LOGW(TAG, "resetting WIFI credentials!");

        resetReasonCounter = 0;

        esp_wifi_restore();
        // esp_wifi_set_bandwidth (WIFI_IF_STA, WIFI_BW_HT20);
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
        esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B |
                                               WIFI_PROTOCOL_11G |
                                               WIFI_PROTOCOL_11N);

        esp_timer_stop(resetReasonTimerHandle);
        esp_timer_delete(resetReasonTimerHandle);
      }

      err |= nvs_set_u8(nvs_handle, "restart_counter", resetReasonCounter);
      err |= nvs_commit(nvs_handle);
      ESP_LOGI(TAG, "%s", (err != ESP_OK) ? "Failed!" : "Done");

      nvs_close(nvs_handle);
    }
  }

  /* Start Wi-Fi station */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  wifi_config_t wifi_config;
  ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Starting provisioning");

  improv_init();
#else
  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASSWORD,
              .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg = {.capable = true, .required = false},
          },
  };

  /* Start Wi-Fi station */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");
#endif

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or
   * connection failed for the maximum number of re-tries (WIFI_FAIL_BIT). The
   * bits are set by event_handler() (see above) */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we
   * can test which event actually happened. */
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "connected to ap SSID: %s", wifi_config.sta.ssid);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
             wifi_config.sta.ssid, wifi_config.sta.password);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }

  uint8_t base_mac[6];
  // Get MAC address for WiFi station
  esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
  sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
          base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);

  // ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT,
  // IP_EVENT_STA_GOT_IP, &event_handler));
  // ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
  // &event_handler)); vEventGroupDelete(s_wifi_event_group);
}

esp_netif_t *get_current_netif(void) { return esp_wifi_netif; }

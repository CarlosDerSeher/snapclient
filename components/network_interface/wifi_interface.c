/*
    Wifi related functionality
    Connect to pre defined wifi

    Must be taken over/merge with wifi provision
*/
#include "wifi_interface.h"

#include <string.h>  // for memcpy

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_types.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "network_interface.h"
#include "nvs_flash.h"

#if ENABLE_WIFI_PROVISIONING
#include "wifi_provisioning.h"
#endif

static const char *TAG = "WIFI_IF";

static void reset_reason_timer_counter_cb(void *);

static char mac_address[18];

static int s_retry_num = 0;

static esp_netif_t *esp_wifi_netif = NULL;

static esp_netif_ip_info_t ip_info = {{0}, {0}, {0}};
static bool connected = false;
static SemaphoreHandle_t connIpSemaphoreHandle = NULL;

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
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if ((s_retry_num < WIFI_MAXIMUM_RETRY) || (WIFI_MAXIMUM_RETRY == 0)) {
      xSemaphoreTake(connIpSemaphoreHandle, portMAX_DELAY);
      connected = false;
      xSemaphoreGive(connIpSemaphoreHandle);

      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGV(TAG, "retry to connect to the AP");
    }

    ESP_LOGV(TAG, "connect to the AP fail");
  }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  if (!network_is_our_netif(NETWORK_INTERFACE_DESC_STA, event->esp_netif)) {
    return;
  }

  xSemaphoreTake(connIpSemaphoreHandle, portMAX_DELAY);

  memcpy((void *)&ip_info, (const void *)&event->ip_info,
         sizeof(esp_netif_ip_info_t));
  connected = true;

  xSemaphoreGive(connIpSemaphoreHandle);

  ESP_LOGI(TAG, "Wifi Got IP Address");
  ESP_LOGI(TAG, "~~~~~~~~~~~");
  ESP_LOGI(TAG, "WIFIIP:" IPSTR, IP2STR(&ip_info.ip));
  ESP_LOGI(TAG, "WIFIMASK:" IPSTR, IP2STR(&ip_info.netmask));
  ESP_LOGI(TAG, "WIFIGW:" IPSTR, IP2STR(&ip_info.gw));
  ESP_LOGI(TAG, "~~~~~~~~~~~");

  s_retry_num = 0;
}

static void lost_ip_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data) {
  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  if (!network_is_our_netif(NETWORK_INTERFACE_DESC_STA, event->esp_netif)) {
    return;
  }

  // const esp_netif_ip_info_t *ip_info = &event->ip_info;

  xSemaphoreTake(connIpSemaphoreHandle, portMAX_DELAY);

  memcpy((void *)&ip_info, (const void *)&event->ip_info,
         sizeof(esp_netif_ip_info_t));
  connected = false;

  xSemaphoreGive(connIpSemaphoreHandle);

  ESP_LOGI(TAG, "Wifi Lost IP Address");
}

/**
 */
bool wifi_get_ip(esp_netif_ip_info_t *ip) {
  xSemaphoreTake(connIpSemaphoreHandle, portMAX_DELAY);

  if (ip) {
    memcpy((void *)ip, (const void *)&ip_info, sizeof(esp_netif_ip_info_t));
  }
  bool _connected = connected;

  xSemaphoreGive(connIpSemaphoreHandle);

  return _connected;
}

/**
 */
void wifi_start(void) {
  if (!connIpSemaphoreHandle) {
    connIpSemaphoreHandle = xSemaphoreCreateMutex();
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_netif_inherent_config_t esp_netif_config =
      ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
  // Warning: the interface desc is used in tests to capture actual connection
  // details (IP, gw, mask)
  esp_netif_config.if_desc = NETWORK_INTERFACE_DESC_STA;
  esp_netif_config.route_prio = 128;
  esp_wifi_netif = esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);
  esp_wifi_set_default_wifi_sta_handlers();

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

  ESP_ERROR_CHECK(esp_wifi_set_protocol(
      WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));

  ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40));

  wifi_config_t wifi_config;
  ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, (esp_event_handler_t)&event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &got_ip_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                             &lost_ip_event_handler, NULL));

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
              .threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK,
              .pmf_cfg = {.capable = true, .required = false},
          },
  };

  /* Start Wi-Fi station */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, (esp_event_handler_t)&event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             &got_ip_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                             &lost_ip_event_handler, NULL));

  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished. Trying to connect to %s",
           wifi_config.sta.ssid);
#endif
}

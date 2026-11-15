/*
    Wifi related functionality
    Connect to pre defined wifi

    Must be taken over/merge with wifi provision
*/
#include "wifi_interface.h"

#include <string.h>  // for memcpy

#include "esp_event.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_types.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "network_interface.h"
#include "network_interface_priv.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if ENABLE_WIFI_PROVISIONING
#include "wifi_provisioning.h"
#include "freertos/task.h"
#endif

/* Configurable delay (ms) before deinitializing improv provisioning after
  startup. Default is 3 minutes. Can be overridden by defining
  IMPROV_DEINIT_DELAY_MS or via a sdkconfig CONFIG_IMPROV_DEINIT_DELAY_MS. */
#ifndef IMPROV_DEINIT_DELAY_MS
#ifdef CONFIG_IMPROV_DEINIT_DELAY_MS
#define IMPROV_DEINIT_DELAY_MS CONFIG_IMPROV_DEINIT_DELAY_MS
#else
#define IMPROV_DEINIT_DELAY_MS 3*60000
#endif
#endif

#if defined(CONFIG_WIFI_AUTH_WEP)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WEP
#elif defined(CONFIG_WIFI_AUTH_WPA_PSK)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WPA_PSK
#elif defined(CONFIG_WIFI_AUTH_WPA2_PSK)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WPA2_PSK
#elif defined(CONFIG_WIFI_AUTH_WPA_WPA2_PSK)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WPA_WPA2_PSK
#elif defined(CONFIG_WIFI_AUTH_ENTERPRISE)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_ENTERPRISE
#elif defined(CONFIG_WIFI_AUTH_WPA2_ENTERPRISE)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WPA2_ENTERPRISE
#elif defined(CONFIG_WIFI_AUTH_WPA3_PSK)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WPA3_PSK
#elif defined(CONFIG_WIFI_AUTH_WPA2_WPA3_PSK)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WPA2_WPA3_PSK
#elif defined(CONFIG_WIFI_AUTH_WAPI_PSK)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WAPI_PSK
#elif defined(CONFIG_WIFI_AUTH_OWE)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_OWE
#elif defined(CONFIG_WIFI_AUTH_WPA3_ENT_192)
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WPA3_ENT_192
#else
  #define WIFI_MIN_AUTHTYPE WIFI_AUTH_WPA_WPA2_PSK
#endif

static const char *TAG = "WIFI_IF";

static char mac_address[18];

static int s_retry_num = 0;

/* Power save mode to use while audio is not playing */
static wifi_ps_type_t wifi_idle_ps_mode(void) {
#if defined(CONFIG_WIFI_PS_NONE_MODE)
  return WIFI_PS_NONE;
#elif defined(CONFIG_WIFI_PS_MAX_MODEM_MODE)
  return WIFI_PS_MAX_MODEM;
#else
  return WIFI_PS_MIN_MODEM;
#endif
}

/* Apply a power save mode, skipping the call if it is already active.
 * network_state_cb() re-signals playback on every state callback, so without
 * this guard esp_wifi_set_ps() and its log line would repeat continuously. */
static void wifi_apply_ps_mode(wifi_ps_type_t mode) {
  static wifi_ps_type_t current_mode = WIFI_PS_MIN_MODEM;  // ESP-IDF default

  if (mode == current_mode) {
    return;
  }

  esp_err_t err = esp_wifi_set_ps(mode);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "failed to set wifi power save: %s", esp_err_to_name(err));
    return;
  }

  current_mode = mode;
  ESP_LOGI(TAG, "wifi power save: %s",
           (mode == WIFI_PS_NONE)        ? "NONE"
           : (mode == WIFI_PS_MAX_MODEM) ? "MAX_MODEM"
                                         : "MIN_MODEM");
}

static esp_netif_t *esp_wifi_netif = NULL;

static esp_netif_ip_info_t ip_info = {{0}, {0}, {0}};
static bool connected = false;
static SemaphoreHandle_t connIpSemaphoreHandle = NULL;
static volatile bool wifi_suppressed_for_takeover = false;

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
    xSemaphoreTake(connIpSemaphoreHandle, portMAX_DELAY);
    connected = false;
    xSemaphoreGive(connIpSemaphoreHandle);

    if (wifi_suppressed_for_takeover) {
      ESP_LOGI(TAG, "WiFi disconnected (suppressed for ETH takeover, not reconnecting)");
    } else if ((s_retry_num < WIFI_MAXIMUM_RETRY) || (WIFI_MAXIMUM_RETRY == 0)) {
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGV(TAG, "retry to connect to the AP");
    }

    ESP_LOGV(TAG, "connect to the AP fail");
  }
}

#if ENABLE_WIFI_PROVISIONING
/*
 * Short-lived task to deinitialize Improv provisioning after a delay.
 * Defined at file scope because C does not support nested functions.
 */
static void improv_deinit_task(void *pv) {
  (void)pv;
  vTaskDelay(pdMS_TO_TICKS(IMPROV_DEINIT_DELAY_MS));
  ESP_LOGI(TAG, "Deinitiating improv provisioning (after %u ms)",
           (unsigned)IMPROV_DEINIT_DELAY_MS);
  improv_deinit();
  vTaskDelete(NULL);
}
#endif

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

  // Log WiFi MAC for verification
  uint8_t wifi_mac[ETH_ADDR_LEN];
  if (network_get_unified_mac_internal(wifi_mac) == ESP_OK) {
    ESP_LOGI(TAG, "WiFi MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             wifi_mac[0], wifi_mac[1], wifi_mac[2],
             wifi_mac[3], wifi_mac[4], wifi_mac[5]);
  }

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

void wifi_suppress_for_takeover(void) {
    wifi_suppressed_for_takeover = true;
    ESP_LOGI(TAG, "WiFi suppressed for Ethernet takeover");
}

void wifi_clear_suppression(bool reconnect) {
    wifi_suppressed_for_takeover = false;
    ESP_LOGI(TAG, "WiFi suppression cleared (reconnect=%d)", reconnect);
    if (reconnect) {
        s_retry_num = 0;
        esp_wifi_connect();
    }
}

bool wifi_is_suppressed(void) {
    return wifi_suppressed_for_takeover;
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

#if ENABLE_WIFI_PROVISIONING
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
  wifi_apply_ps_mode(wifi_idle_ps_mode());

  ESP_LOGI(TAG, "Starting provisioning");

  improv_init();
#if ENABLE_WIFI_PROVISIONING
  /* Start a short-lived task that will deinit improv after a configurable
     delay from startup. This ensures we deinit even if we never connect. */
  static bool improv_deinit_task_created = false;
  if (!improv_deinit_task_created) {
    BaseType_t r = xTaskCreate(improv_deinit_task, "improv_deinit", 4096,
                               NULL, tskIDLE_PRIORITY + 1, NULL);
    if (r != pdPASS) {
      ESP_LOGW(TAG, "failed to create improv_deinit task");
    } else {
      improv_deinit_task_created = true;
    }
  }
#endif
#else
  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASSWORD,
              .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
              .threshold.authmode = WIFI_MIN_AUTHTYPE,
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
  wifi_apply_ps_mode(wifi_idle_ps_mode());

  ESP_LOGI(TAG, "wifi_init_sta finished. Trying to connect to %s",
           wifi_config.sta.ssid);
#endif
}

/**
 * Set WiFi power save mode based on playback state.
 * enable=false pins the radio awake during playback for best throughput,
 * enable=true returns it to the configured idle mode.
 */
void wifi_set_power_save(bool enable) {
#if defined(CONFIG_WIFI_DYNAMIC_POWER_SAVE)
  // While Ethernet has taken over, WiFi carries no audio - stay in the idle
  // mode rather than holding the radio awake for an unused link.
  if (enable || wifi_is_suppressed()) {
    wifi_apply_ps_mode(wifi_idle_ps_mode());
  } else {
    wifi_apply_ps_mode(WIFI_PS_NONE);
  }
#else
  (void)enable;  // power save mode is static
#endif
}

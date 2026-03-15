/*
   Network related functions
*/

#include "net_functions.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "netdb.h"

static const char *TAG = "NETF";

extern EventGroupHandle_t s_wifi_event_group;

static const char *ip_protocol_str[] = {"V4", "V6", "MAX"};

void net_mdns_register(const char *clientname) {
  ESP_LOGI(TAG, "Setup mdns");
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set(clientname));
  ESP_ERROR_CHECK(mdns_instance_name_set("ESP32 SNAPcast client OTA"));
  ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 8032, NULL, 0));
}

void mdns_print_results(const mdns_result_t *results) {
  mdns_result_t *r = (mdns_result_t *)results;
  mdns_ip_addr_t *a = NULL;

  while (r) {
    ESP_LOGI(TAG, "Interface: %s", esp_netif_get_desc(r->esp_netif));
    ESP_LOGI(TAG, "Type: %s", ip_protocol_str[r->ip_protocol]);
    if (r->instance_name) {
      ESP_LOGI(TAG, "  PTR : %s", r->instance_name);
    }
    if (r->hostname) {
      ESP_LOGI(TAG, "  SRV : %s.local:%u", r->hostname, r->port);
    }
    if (r->txt_count) {
      ESP_LOGI(TAG, "  TXT : [%u] ", r->txt_count);
      for (int t = 0; t < r->txt_count; t++) {
        ESP_LOGI(TAG, "%s=%s; ", r->txt[t].key, r->txt[t].value);
      }
    }
    a = r->addr;
    while (a) {
      if (a->addr.type == IPADDR_TYPE_V6) {
        ESP_LOGI(TAG, "  AAAA: " IPV6STR, IPV62STR(a->addr.u_addr.ip6));
      } else {
        ESP_LOGI(TAG, "  A   : " IPSTR, IP2STR(&(a->addr.u_addr.ip4)));
      }
      a = a->next;
    }
    r = r->next;
  }
}

uint32_t find_mdns_service(const char *service_name, const char *proto) {
  ESP_LOGI(TAG, "Query PTR: %s.%s.local", service_name, proto);

  mdns_result_t *r = NULL;
  esp_err_t err = mdns_query_ptr(service_name, proto, 3000, 20, &r);
  if (err) {
    ESP_LOGE(TAG, "Query Failed");
    return -1;
  }
  if (!r) {
    ESP_LOGW(TAG, "No results found!");
    return -1;
  }

  if (r->instance_name) {
    printf("  PTR : %s\n", r->instance_name);
  }
  if (r->hostname) {
    printf("  SRV : %s.local:%u\n", r->hostname, r->port);
    return r->port;
  }
  mdns_query_results_free(r);
  return 0;
}

/**
 *
 */
void sntp_cb(struct timeval *tv) {
  struct tm timeinfo = {0};
  time_t now = tv->tv_sec;
  localtime_r(&now, &timeinfo);
  char strftime_buf[64];
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
  ESP_LOGI(TAG, "sntp_cb called :%s", strftime_buf);
}

// void set_time_from_sntp() {
//   xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, false, true,
//                       portMAX_DELAY);
//   // ESP_LOGI(TAG, "clock %");
//   ESP_LOGI(TAG, "Initializing SNTP");
//   sntp_setoperatingmode(SNTP_OPMODE_POLL);
//   sntp_setservername(0, CONFIG_SNTP_SERVER);
//   sntp_init();
//   // sntp_set_time_sync_notification_cb(sntp_cb);
//   setenv("TZ", SNTP_TIMEZONE, 1);
//   tzset();
//
//   time_t now = 0;
//   struct tm timeinfo = {0};
//   int retry = 0;
//   const int retry_count = 10;
//   while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
//     ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry,
//              retry_count);
//     vTaskDelay(2000 / portTICK_PERIOD_MS);
//     time(&now);
//     localtime_r(&now, &timeinfo);
//   }
//   char strftime_buf[64];
//
//   strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
//   ESP_LOGI(TAG, "The current date/time in UTC is: %s", strftime_buf);
// }

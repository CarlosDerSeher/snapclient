/*
 * wifi_provisioning.c
 *
 *  Created on: Apr 28, 2024
 *      Author: karl
 */
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "improv_wrapper.h"
#include "wifi_interface.h"

#include "wifi_provisioning.h"

#define TAG "IMPROV"

#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
#include "driver/uart.h"
#endif

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#include "driver/usb_serial_jtag.h"
#endif

#define TAG "IMPROV"

#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
#define RD_BUF_SIZE (UART_FIFO_LEN)
#define PATTERN_CHR_NUM (3)
#endif

#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
static TaskHandle_t t_improv_uart_task = NULL;
static const int uart_buffer_size = 2 * RD_BUF_SIZE;
static QueueHandle_t uart_queue;
static uart_port_t uart_num = UART_NUM_0;
#endif

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
static TaskHandle_t t_usb_serial_task = NULL;
#define USB_SERIAL_BUF_SIZE 256
#endif

#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
void uart_event_handler(void) {
  uart_event_t event;
  uint8_t dtmp[RD_BUF_SIZE];

  // Waiting for UART event.
  if (xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
    bzero(dtmp, RD_BUF_SIZE);
    // ESP_LOGD(TAG, "uart[%d] event:", uart_num);
    switch (event.type) {
      // Event of UART receving data
      /* We'd better handler data event fast, there would be much more data
      events than other types of events. If we take too much time on data event,
      the queue might be full. */
      case UART_DATA:
        //ESP_LOGD(TAG, "uart data: %d bytes", event.size);
        uart_read_bytes(uart_num, dtmp, event.size, portMAX_DELAY);
        improv_wifi_handle_serial(dtmp, event.size);
        break;
      // Event of HW FIFO overflow detected
      case UART_FIFO_OVF:
        ESP_LOGD(TAG, "hw fifo overflow");

        // If fifo overflow happened, you should consider adding flow control
        // for your application. The ISR has already reset the rx FIFO, As an
        // example, we directly flush the rx buffer here in order to read more
        // data.
        uart_flush_input(uart_num);
        xQueueReset(uart_queue);
        break;
      // Event of UART ring buffer full
      case UART_BUFFER_FULL:
        ESP_LOGD(TAG, "ring buffer full");
        // If buffer full happened, you should consider increasing your buffer
        // size As an example, we directly flush the rx buffer here in order to
        // read more data.
        uart_flush_input(uart_num);
        xQueueReset(uart_queue);
        break;
      case UART_BREAK:
        break;
      // Others
      default:
        ESP_LOGD(TAG, "uart event type: %d", event.type);
        break;
    }
  }
}

static void improv_uart_task(void *pvParameters) {
  while (1) {
    uart_event_handler();
  }
}
#endif

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
static void usb_serial_improv_task(void *pvParameters) {
  uint8_t buf[USB_SERIAL_BUF_SIZE];
  
  ESP_LOGD(TAG, "USB Serial JTAG Improv task started");
  
  while (1) {
    // Read data from USB Serial JTAG
    int len = usb_serial_jtag_read_bytes(buf, USB_SERIAL_BUF_SIZE, pdMS_TO_TICKS(100));
    
    if (len > 0) {
      ESP_LOGD(TAG, "[USB SERIAL DATA]: %d bytes", len);
      improv_wifi_handle_serial(buf, len);
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif

void uart_write(const unsigned char *txData, int length) {
#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
  uart_write_bytes(uart_num, txData, length);
#endif

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
  usb_serial_jtag_write_bytes((const char *)txData, length, pdMS_TO_TICKS(100));
#endif
}

void improv_wifi_scan(unsigned char *scanResponse, int bufLen,
                      uint16_t *count) {
  uint16_t number = 16;
  wifi_ap_record_t ap_info[16];

  memset(ap_info, 0, sizeof(ap_info));
  ESP_LOGD(TAG, "Starting WiFi scan for improv");
  if (esp_wifi_scan_start(NULL, true) == ESP_ERR_WIFI_STATE) {
    wifi_ap_record_t ap_info_tmp;

    do {
      esp_wifi_disconnect();
      vTaskDelay(pdMS_TO_TICKS(500));
    } while (esp_wifi_sta_get_ap_info(&ap_info_tmp) !=
             ESP_ERR_WIFI_NOT_CONNECT);

    esp_wifi_scan_start(NULL, true);
  }
  ESP_LOGD(TAG, "Max AP number ap_info can hold = %u", number);
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(count));
  ESP_LOGD(TAG, "Total APs scanned = %u, actual AP number ap_info holds = %u", *count, number);

  scanResponse[0] = 0;
  for (int i = 0; i < number; i++) {
    char rssiStr[8] = {
        0,
    };
    char cipherStr[8] = {
        0,
    };
    uint16_t neededLen;

    itoa(ap_info[i].rssi, rssiStr, 10);
    if (ap_info[i].authmode != WIFI_AUTH_OPEN) {
      strcat(cipherStr, "YES");
    } else {
      strcat(cipherStr, "NO");
    }
    neededLen = strlen((const char *)ap_info[i].ssid) + strlen(rssiStr) +
                strlen(cipherStr) + 3;

    if ((bufLen - neededLen) > 0) {
      strcat((char *)scanResponse, (char *)ap_info[i].ssid);
      strcat((char *)scanResponse, (char *)",");
      strcat((char *)scanResponse, (char *)rssiStr);
      strcat((char *)scanResponse, (char *)",");
      strcat((char *)scanResponse, (char *)cipherStr);
      strcat((char *)scanResponse, (char *)"\n");

      bufLen -= neededLen;
    }
  }

  ESP_LOGD(TAG, "APs \t\t%s", scanResponse);
}

bool improv_wifi_connect(const char *ssid, const char *password) {
  uint8_t count = 0;
  wifi_ap_record_t apRec;
  esp_err_t err;

  while ((err = esp_wifi_sta_get_ap_info(&apRec)) != ESP_ERR_WIFI_NOT_CONNECT) {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  wifi_config_t wifi_config;
  ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &wifi_config));
  strcpy((char *)wifi_config.sta.ssid, ssid);
  strcpy((char *)wifi_config.sta.password, password);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  esp_wifi_connect();
  while (esp_wifi_sta_get_ap_info(&apRec) != ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (count > 20) {
      esp_wifi_disconnect();
      return false;
    }
    count++;
  }

  return true;
}

bool improv_wifi_is_connected(void) {
  wifi_ap_record_t apRec;

  if (esp_wifi_sta_get_ap_info(&apRec) == ESP_OK) {
    return true;
  }

  return false;
}

void improv_wifi_get_local_ip(uint8_t *address) {
  esp_netif_ip_info_t ip_info;

  /* Wait a short time for an IP to become available. Improv frequently asks
     for the local IP during onboarding; when called too early we should
     wait briefly rather than returning 0.0.0.0 immediately. */
  const TickType_t timeout = pdMS_TO_TICKS(2000); /* 2s */
  TickType_t start = xTaskGetTickCount();

  while (wifi_get_ip(&ip_info) == false) {
    if ((xTaskGetTickCount() - start) > timeout) {
      ESP_LOGW(TAG, "%s: no valid IP available after timeout", __func__);
      /* return all-zero address (caller should handle this) */
      address[0] = 0;
      address[1] = 0;
      address[2] = 0;
      address[3] = 0;
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  address[0] = (uint8_t)(ip_info.ip.addr & 0xFF);
  address[1] = (uint8_t)((ip_info.ip.addr >> 8) & 0xFF);
  address[2] = (uint8_t)((ip_info.ip.addr >> 16) & 0xFF);
  address[3] = (uint8_t)((ip_info.ip.addr >> 24) & 0xFF);

  ESP_LOGI(TAG, "%d.%d.%d.%d", address[0], address[1], address[2], address[3]);
}

void improv_init(void) {
  uint8_t webPortStr[6] = {0};
  uint16_t webPort = CONFIG_WEB_PORT;
  uint8_t urlStr[26] = "http://{LOCAL_IPV4}:";

  utoa(webPort, (char *)webPortStr, 10);
  strcat((char *)urlStr, (char *)webPortStr);

  improv_wifi_create();
  improv_wifi_serialWrite(uart_write);

  // Automatically detect chip family based on build target
  uint8_t chipFamily = CF_ESP32;
#if CONFIG_IDF_TARGET_ESP32S3
  chipFamily = CF_ESP32_S3;
#elif CONFIG_IDF_TARGET_ESP32S2
  chipFamily = CF_ESP32_S2;
#elif CONFIG_IDF_TARGET_ESP32C3
  chipFamily = CF_ESP32_C3;
#elif CONFIG_IDF_TARGET_ESP32
  chipFamily = CF_ESP32;
#endif

  improv_wifi_set_device_info(chipFamily, "esp32_snapclient", "0.0.3",
                              "snapclient", (const char *)urlStr);

  improv_wifi_setCustomConnectWiFi(improv_wifi_connect);
  improv_wifi_setCustomScanWiFi(improv_wifi_scan);
  improv_wifi_setCustomIsConnected(improv_wifi_is_connected);
  improv_wifi_setCustomGetLocalIpCallback(improv_wifi_get_local_ip);

#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
  // Set UART number from config
  uart_num = CONFIG_ESP_CONSOLE_UART_NUM;
  
  // Install UART driver using an event queue here
  ESP_LOGI(TAG, "Installing UART%d driver for Improv", uart_num);
  esp_err_t ret = uart_driver_install(uart_num, uart_buffer_size,
                                      uart_buffer_size, 10, &uart_queue, 0);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
  } else {
    BaseType_t task_ret = xTaskCreatePinnedToCore(&improv_uart_task, "improv_uart", 8 * 1024, NULL, 4,
                          &t_improv_uart_task, tskNO_AFFINITY);
    ESP_LOGD(TAG, "UART Improv task created: %d (handle: %p)", task_ret, t_improv_uart_task);
  }
#endif

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
  // Initialize USB Serial JTAG driver
  ESP_LOGI(TAG, "Initializing USB Serial JTAG for Improv");
  
  usb_serial_jtag_driver_config_t usb_serial_config = {
    .rx_buffer_size = USB_SERIAL_BUF_SIZE * 2,
    .tx_buffer_size = USB_SERIAL_BUF_SIZE * 2,
  };
  
  esp_err_t usb_ret = usb_serial_jtag_driver_install(&usb_serial_config);
  if (usb_ret == ESP_OK) {
    ESP_LOGD(TAG, "USB Serial JTAG driver installed successfully");
    // Create USB Serial task
    BaseType_t usb_task_ret = xTaskCreatePinnedToCore(&usb_serial_improv_task, "usb_improv", 
                              8 * 1024, NULL, 4, &t_usb_serial_task, tskNO_AFFINITY);
    ESP_LOGD(TAG, "USB Serial task creation result: %d (handle: %p)", usb_task_ret, t_usb_serial_task);
  } else {
    ESP_LOGW(TAG, "Failed to install USB Serial JTAG driver: %s", esp_err_to_name(usb_ret));
  }
#endif
}

void improv_deinit(void) {
#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
  if (t_usb_serial_task) {
    vTaskDelete(t_usb_serial_task);
    t_usb_serial_task = NULL;
    usb_serial_jtag_driver_uninstall();
    ESP_LOGI(TAG, "USB Serial JTAG deinitialized");
  }
#endif

#ifdef CONFIG_ESP_CONSOLE_UART_DEFAULT
  if (t_improv_uart_task) {
    vTaskDelete(t_improv_uart_task);
    uart_driver_delete(uart_num);
    t_improv_uart_task = NULL;
    ESP_LOGI(TAG, "UART%d deinitialized", uart_num);
  }
#endif

  improv_wifi_destroy();
}

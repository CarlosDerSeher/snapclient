/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "eth_interface.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
#include "driver/spi_master.h"
#endif

static const char *TAG = "snapclient_eth_init";


/* The event group allows multiple bits for each event, but we only care about
 * two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define ETH_CONNECTED_BIT BIT0
#define ETH_FAIL_BIT BIT1

static EventGroupHandle_t s_eth_event_group;

#if CONFIG_SNAPCLIENT_SPI_ETHERNETS_NUM
#define SPI_ETHERNETS_NUM CONFIG_SNAPCLIENT_SPI_ETHERNETS_NUM
#else
#define SPI_ETHERNETS_NUM 0
#endif

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET
#define INTERNAL_ETHERNETS_NUM 1
#else
#define INTERNAL_ETHERNETS_NUM 0
#endif

#define INIT_SPI_ETH_MODULE_CONFIG(eth_module_config, num)                     \
  do {                                                                         \
    eth_module_config[num].spi_cs_gpio =                                       \
        CONFIG_SNAPCLIENT_ETH_SPI_CS##num##_GPIO;                              \
    eth_module_config[num].int_gpio =                                          \
        CONFIG_SNAPCLIENT_ETH_SPI_INT##num##_GPIO;                             \
    eth_module_config[num].phy_reset_gpio =                                    \
        CONFIG_SNAPCLIENT_ETH_SPI_PHY_RST##num##_GPIO;                         \
    eth_module_config[num].phy_addr = CONFIG_SNAPCLIENT_ETH_SPI_PHY_ADDR##num; \
  } while (0)

typedef struct {
  uint8_t spi_cs_gpio;
  uint8_t int_gpio;
  int8_t phy_reset_gpio;
  uint8_t phy_addr;
  uint8_t *mac_addr;
} spi_eth_module_config_t;

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET
/**
 * @brief Internal ESP32 Ethernet initialization
 *
 * @param[out] mac_out optionally returns Ethernet MAC object
 * @param[out] phy_out optionally returns Ethernet PHY object
 * @return
 *          - esp_eth_handle_t if init succeeded
 *          - NULL if init failed
 */
static esp_eth_handle_t eth_init_internal(esp_eth_mac_t **mac_out,
                                          esp_eth_phy_t **phy_out) {
  esp_eth_handle_t ret = NULL;

  // Init common MAC and PHY configs to default
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

  // Update PHY config based on board specific configuration
  phy_config.phy_addr = CONFIG_SNAPCLIENT_ETH_PHY_ADDR;
  phy_config.reset_gpio_num = CONFIG_SNAPCLIENT_ETH_PHY_RST_GPIO;

  // Init vendor specific MAC config to default
  eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  // Update vendor specific MAC config based on board configuration
  esp32_emac_config.smi_mdc_gpio_num = CONFIG_SNAPCLIENT_ETH_MDC_GPIO;
  esp32_emac_config.smi_mdio_gpio_num = CONFIG_SNAPCLIENT_ETH_MDIO_GPIO;

  // Set clock mode and GPIO
#if CONFIG_ETH_RMII_CLK_INPUT
  esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
#elif CONFIG_ETH_RMII_CLK_OUTPUT
  esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_OUT;
#else
  esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_DEFAULT;
#endif
  esp32_emac_config.clock_config.rmii.clock_gpio = CONFIG_ETH_RMII_CLK_IN_GPIO;

  // Create new ESP32 Ethernet MAC instance
  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);

  // Create new PHY instance based on board configuration
#if CONFIG_SNAPCLIENT_ETH_PHY_IP101
  esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
#elif CONFIG_SNAPCLIENT_ETH_PHY_RTL8201
  esp_eth_phy_t *phy = esp_eth_phy_new_rtl8201(&phy_config);
#elif CONFIG_SNAPCLIENT_ETH_PHY_LAN87XX
  esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
#elif CONFIG_SNAPCLIENT_ETH_PHY_DP83848
  esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
#elif CONFIG_SNAPCLIENT_ETH_PHY_KSZ80XX
  esp_eth_phy_t *phy = esp_eth_phy_new_ksz80xx(&phy_config);
#endif

  // Init Ethernet driver to default and install it
  esp_eth_handle_t eth_handle = NULL;
  esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
  ESP_GOTO_ON_FALSE(esp_eth_driver_install(&config, &eth_handle) == ESP_OK,
                    NULL, err, TAG, "Ethernet driver install failed");

  if (mac_out != NULL) {
    *mac_out = mac;
  }
  if (phy_out != NULL) {
    *phy_out = phy;
  }
  return eth_handle;
err:
  if (eth_handle != NULL) {
    esp_eth_driver_uninstall(eth_handle);
  }
  if (mac != NULL) {
    mac->del(mac);
  }
  if (phy != NULL) {
    phy->del(phy);
  }
  return ret;
}
#endif  // CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET

#if CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
/**
 * @brief SPI bus initialization (to be used by Ethernet SPI modules)
 *
 * @return
 *          - ESP_OK on success
 */
static esp_err_t spi_bus_init(void) {
  esp_err_t ret = ESP_OK;

  // Install GPIO ISR handler to be able to service SPI Eth modules interrupts
  ret = gpio_install_isr_service(0);
  if (ret != ESP_OK) {
    if (ret == ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "GPIO ISR handler has been already installed");
      ret = ESP_OK;  // ISR handler has been already installed so no issues
    } else {
      ESP_LOGE(TAG, "GPIO ISR handler install failed");
      goto err;
    }
  }

  // Init SPI bus
  spi_bus_config_t buscfg = {
      .miso_io_num = CONFIG_SNAPCLIENT_ETH_SPI_MISO_GPIO,
      .mosi_io_num = CONFIG_SNAPCLIENT_ETH_SPI_MOSI_GPIO,
      .sclk_io_num = CONFIG_SNAPCLIENT_ETH_SPI_SCLK_GPIO,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
  };
  ESP_GOTO_ON_ERROR(spi_bus_initialize(CONFIG_SNAPCLIENT_ETH_SPI_HOST, &buscfg,
                                       SPI_DMA_CH_AUTO),
                    err, TAG, "SPI host #%d init failed",
                    CONFIG_SNAPCLIENT_ETH_SPI_HOST);

err:
  return ret;
}

/**
 * @brief Ethernet SPI modules initialization
 *
 * @param[in] spi_eth_module_config specific SPI Ethernet module configuration
 * @param[out] mac_out optionally returns Ethernet MAC object
 * @param[out] phy_out optionally returns Ethernet PHY object
 * @return
 *          - esp_eth_handle_t if init succeeded
 *          - NULL if init failed
 */
static esp_eth_handle_t eth_init_spi(
    spi_eth_module_config_t *spi_eth_module_config, esp_eth_mac_t **mac_out,
    esp_eth_phy_t **phy_out) {
  esp_eth_handle_t ret = NULL;

  // Init common MAC and PHY configs to default
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

  // Update PHY config based on board specific configuration
  phy_config.phy_addr = spi_eth_module_config->phy_addr;
  phy_config.reset_gpio_num = spi_eth_module_config->phy_reset_gpio;

  // Configure SPI interface for specific SPI module
  spi_device_interface_config_t spi_devcfg = {
      .mode = 0,
      .clock_speed_hz = CONFIG_SNAPCLIENT_ETH_SPI_CLOCK_MHZ * 1000 * 1000,
      .queue_size = 20,
      .spics_io_num = spi_eth_module_config->spi_cs_gpio};
  // Init vendor specific MAC config to default, and create new SPI Ethernet MAC
  // instance and new PHY instance based on board configuration
#if CONFIG_SNAPCLIENT_USE_KSZ8851SNL
  eth_ksz8851snl_config_t ksz8851snl_config = ETH_KSZ8851SNL_DEFAULT_CONFIG(
      CONFIG_SNAPCLIENT_ETH_SPI_HOST, &spi_devcfg);
  ksz8851snl_config.int_gpio_num = spi_eth_module_config->int_gpio;
  esp_eth_mac_t *mac =
      esp_eth_mac_new_ksz8851snl(&ksz8851snl_config, &mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_ksz8851snl(&phy_config);
#elif CONFIG_SNAPCLIENT_USE_DM9051
  eth_dm9051_config_t dm9051_config =
      ETH_DM9051_DEFAULT_CONFIG(CONFIG_SNAPCLIENT_ETH_SPI_HOST, &spi_devcfg);
  dm9051_config.int_gpio_num = spi_eth_module_config->int_gpio;
  esp_eth_mac_t *mac = esp_eth_mac_new_dm9051(&dm9051_config, &mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_dm9051(&phy_config);
#elif CONFIG_SNAPCLIENT_USE_W5500
  eth_w5500_config_t w5500_config =
      ETH_W5500_DEFAULT_CONFIG(CONFIG_SNAPCLIENT_ETH_SPI_HOST, &spi_devcfg);
  w5500_config.int_gpio_num = spi_eth_module_config->int_gpio;
  esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
#endif  // CONFIG_SNAPCLIENT_USE_W5500
  // Init Ethernet driver to default and install it
  esp_eth_handle_t eth_handle = NULL;
  esp_eth_config_t eth_config_spi = ETH_DEFAULT_CONFIG(mac, phy);
  ESP_GOTO_ON_FALSE(
      esp_eth_driver_install(&eth_config_spi, &eth_handle) == ESP_OK, NULL, err,
      TAG, "SPI Ethernet driver install failed");

  // The SPI Ethernet module might not have a burned factory MAC address, we can
  // set it manually.
  if (spi_eth_module_config->mac_addr != NULL) {
    ESP_GOTO_ON_FALSE(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR,
                                    spi_eth_module_config->mac_addr) == ESP_OK,
                      NULL, err, TAG, "SPI Ethernet MAC address config failed");
  }

  if (mac_out != NULL) {
    *mac_out = mac;
  }
  if (phy_out != NULL) {
    *phy_out = phy;
  }
  return eth_handle;
err:
  if (eth_handle != NULL) {
    esp_eth_driver_uninstall(eth_handle);
  }
  if (mac != NULL) {
    mac->del(mac);
  }
  if (phy != NULL) {
    phy->del(phy);
  }
  return ret;
}
#endif  // CONFIG_SNAPCLIENT_USE_SPI_ETHERNET

/** Original init function in the example */
esp_err_t original_eth_init(esp_eth_handle_t *eth_handles_out[],
                            uint8_t *eth_cnt_out) {
  esp_err_t ret = ESP_OK;
  esp_eth_handle_t *eth_handles = NULL;
  uint8_t eth_cnt = 0;

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  ESP_GOTO_ON_FALSE(
      eth_handles_out != NULL && eth_cnt_out != NULL, ESP_ERR_INVALID_ARG, err,
      TAG,
      "invalid arguments: initialized handles array or number of interfaces");
  eth_handles = calloc(SPI_ETHERNETS_NUM + INTERNAL_ETHERNETS_NUM,
                       sizeof(esp_eth_handle_t));
  ESP_GOTO_ON_FALSE(eth_handles != NULL, ESP_ERR_NO_MEM, err, TAG, "no memory");

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET
  eth_handles[eth_cnt] = eth_init_internal(NULL, NULL);
  ESP_GOTO_ON_FALSE(eth_handles[eth_cnt], ESP_FAIL, err, TAG,
                    "internal Ethernet init failed");
  eth_cnt++;
#endif  // CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET

#if CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  ESP_GOTO_ON_ERROR(spi_bus_init(), err, TAG, "SPI bus init failed");
  // Init specific SPI Ethernet module configuration from Kconfig (CS GPIO,
  // Interrupt GPIO, etc.)
  spi_eth_module_config_t
      spi_eth_module_config[CONFIG_SNAPCLIENT_SPI_ETHERNETS_NUM] = {0};
  INIT_SPI_ETH_MODULE_CONFIG(spi_eth_module_config, 0);
  // The SPI Ethernet module(s) might not have a burned factory MAC address,
  // hence use manually configured address(es). In this example, Locally
  // Administered MAC address derived from ESP32x base MAC address is used. Note
  // that Locally Administered OUI range should be used only when testing on a
  // LAN under your control!
  uint8_t base_mac_addr[ETH_ADDR_LEN];
  ESP_GOTO_ON_ERROR(esp_efuse_mac_get_default(base_mac_addr), err, TAG,
                    "get EFUSE MAC failed");
  uint8_t local_mac_1[ETH_ADDR_LEN];
  esp_derive_local_mac(local_mac_1, base_mac_addr);
  spi_eth_module_config[0].mac_addr = local_mac_1;
#if CONFIG_SNAPCLIENT_SPI_ETHERNETS_NUM > 1
  INIT_SPI_ETH_MODULE_CONFIG(spi_eth_module_config, 1);
  uint8_t local_mac_2[ETH_ADDR_LEN];
  base_mac_addr[ETH_ADDR_LEN - 1] += 1;
  esp_derive_local_mac(local_mac_2, base_mac_addr);
  spi_eth_module_config[1].mac_addr = local_mac_2;
#endif
#if CONFIG_SNAPCLIENT_SPI_ETHERNETS_NUM > 2
#error Maximum number of supported SPI Ethernet devices is currently limited to 2 by this example.
#endif
  for (int i = 0; i < CONFIG_SNAPCLIENT_SPI_ETHERNETS_NUM; i++) {
    eth_handles[eth_cnt] = eth_init_spi(&spi_eth_module_config[i], NULL, NULL);
    ESP_GOTO_ON_FALSE(eth_handles[eth_cnt], ESP_FAIL, err, TAG,
                      "SPI Ethernet init failed");
    eth_cnt++;
  }
#endif  // CONFIG_ETH_USE_SPI_ETHERNET
#else
  ESP_LOGD(TAG, "no Ethernet device selected to init");
#endif  // CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET ||
        // CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  *eth_handles_out = eth_handles;
  *eth_cnt_out = eth_cnt;

  return ret;
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
err:
  free(eth_handles);
  return ret;
#endif
}

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
  uint8_t mac_addr[6] = {0};
  /* we can get the ethernet driver handle from event data */
  esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

  switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
      esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
      ESP_LOGI(TAG, "Ethernet Link Up");
      ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
               mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4],
               mac_addr[5]);
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "Ethernet Link Down");
      xEventGroupSetBits(s_eth_event_group, ETH_FAIL_BIT);
      break;
    case ETHERNET_EVENT_START:
      ESP_LOGI(TAG, "Ethernet Started");
      break;
    case ETHERNET_EVENT_STOP:
      ESP_LOGI(TAG, "Ethernet Stopped");
      break;
    default:
      break;
  }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  const esp_netif_ip_info_t *ip_info = &event->ip_info;

  ESP_LOGI(TAG, "Ethernet Got IP Address");
  ESP_LOGI(TAG, "~~~~~~~~~~~");
  ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
  ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
  ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
  ESP_LOGI(TAG, "~~~~~~~~~~~");

  xEventGroupSetBits(s_eth_event_group, ETH_CONNECTED_BIT);
}

/** Init function that exposes to the main application */
void eth_init(void) {
  // Initialize Ethernet driver
  uint8_t eth_port_cnt = 0;
  esp_eth_handle_t *eth_handles;
  ESP_ERROR_CHECK(original_eth_init(&eth_handles, &eth_port_cnt));

  // Initialize TCP/IP network interface aka the esp-netif (should be called
  // only once in application)
  ESP_ERROR_CHECK(esp_netif_init());
  // Create default event loop that running in background
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Create instance(s) of esp-netif for Ethernet(s)
  if (eth_port_cnt == 1) {
    // Use ESP_NETIF_DEFAULT_ETH when just one Ethernet interface is used and
    // you don't need to modify default esp-netif configuration parameters.
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(
        esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));
  } else {
    // Use ESP_NETIF_INHERENT_DEFAULT_ETH when multiple Ethernet interfaces are
    // used and so you need to modify esp-netif configuration parameters for
    // each interface (name, priority, etc.).
    esp_netif_inherent_config_t esp_netif_config =
        ESP_NETIF_INHERENT_DEFAULT_ETH();
    esp_netif_config_t cfg_spi = {.base = &esp_netif_config,
                                  .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    char if_key_str[10];
    char if_desc_str[10];
    char num_str[3];
    for (int i = 0; i < eth_port_cnt; i++) {
      itoa(i, num_str, 10);
      strcat(strcpy(if_key_str, "ETH_"), num_str);
      strcat(strcpy(if_desc_str, "eth"), num_str);
      esp_netif_config.if_key = if_key_str;
      esp_netif_config.if_desc = if_desc_str;
      esp_netif_config.route_prio -= i * 5;
      esp_netif_t *eth_netif = esp_netif_new(&cfg_spi);

      // Attach Ethernet driver to TCP/IP stack
      ESP_ERROR_CHECK(
          esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[i])));
    }
  }

  // Register user defined event handers
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                             &eth_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &got_ip_event_handler, NULL));

  // Start Ethernet driver state machine
  for (int i = 0; i < eth_port_cnt; i++) {
    ESP_ERROR_CHECK(esp_eth_start(eth_handles[i]));
  }

  /* Waiting until either the connection is established (ETH_CONNECTED_BIT) or
   * connection failed for the maximum number of re-tries (ETH_FAIL_BIT). The
   * bits are set by event_handler() (see above) */
  s_eth_event_group = xEventGroupCreate();
  //    EventBits_t bits =
  xEventGroupWaitBits(s_eth_event_group, ETH_CONNECTED_BIT, pdFALSE, pdFALSE,
                      portMAX_DELAY);
}

#include "connection_handler.h"

#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "mdns.h"
#include "net_functions.h"
#include "network_interface.h"

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_SERVER_USE_MDNS CONFIG_SNAPSERVER_USE_MDNS
#if !SNAPCAST_SERVER_USE_MDNS
#define SNAPCAST_SERVER_HOST CONFIG_SNAPSERVER_HOST
#define SNAPCAST_SERVER_PORT CONFIG_SNAPSERVER_PORT
#endif

// External variable that need to be accessible
extern struct netconn* lwipNetconn;

static const char* TAG = "CONNECTION_HANDLER";

void setup_network(esp_netif_t** netif) {
  int rc1, rc2 = ERR_OK;
  mdns_result_t* r;
  esp_err_t err = 0;
  uint16_t remotePort = 0;

  while (1) {
    if (lwipNetconn != NULL) {
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;
    }

    ESP_LOGI(TAG, "Wait for network connection");
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    esp_netif_t* eth_netif =
        network_get_netif_from_desc(NETWORK_INTERFACE_DESC_ETH);
#endif
    esp_netif_t* sta_netif =
        network_get_netif_from_desc(NETWORK_INTERFACE_DESC_STA);
    while (1) {
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
      bool ethUp = network_is_netif_up(eth_netif);

      if (ethUp) {
        netif = eth_netif;

        break;
      }
#endif

      bool staUp = network_is_netif_up(sta_netif);
      if (staUp) {
        *netif = sta_netif;

        break;
      }

      vTaskDelay(pdMS_TO_TICKS(1000));
    }

#if SNAPCAST_SERVER_USE_MDNS
    // Find snapcast server
    // Connect to first snapcast server found
    r = NULL;
    err = 0;
    while (!r || err) {
      ESP_LOGI(TAG, "Lookup snapcast service on network");
      esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &r);
      if (err) {
        ESP_LOGE(TAG, "Query Failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }

      if (!r) {
        ESP_LOGW(TAG, "No results found!");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    ESP_LOGI(TAG, "\n~~~~~~~~~~ MDNS Query success ~~~~~~~~~~");
    mdns_print_results(r);
    ESP_LOGI(TAG, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");

    ip_addr_t remote_ip;
    mdns_result_t* re = r;
    while (re) {
      mdns_ip_addr_t* a = re->addr;
#if CONFIG_SNAPCLIENT_CONNECT_IPV6
      if (a->addr.type == IPADDR_TYPE_V6) {
        *netif = re->esp_netif;
        break;
      }

      // TODO: fall back to IPv4 if no IPv6 was available
#else
      if (a->addr.type == IPADDR_TYPE_V4) {
        *netif = re->esp_netif;
        break;
      }
#endif

      re = re->next;
    }

    if (!re) {
      mdns_query_results_free(r);

      ESP_LOGW(TAG, "didn't find any valid IP in MDNS query");

      continue;
    }

    ip_addr_copy(remote_ip, re->addr->addr);
    remotePort = r->port;

    mdns_query_results_free(r);

    ESP_LOGI(TAG, "Found %s:%d", ipaddr_ntoa(&remote_ip), remotePort);
#else
    ip_addr_t remote_ip;

    if (ipaddr_aton(SNAPCAST_SERVER_HOST, &remote_ip) == 0) {
      ESP_LOGE(TAG, "can't convert static server adress to numeric");
      continue;
    }

    remotePort = SNAPCAST_SERVER_PORT;

    ESP_LOGI(TAG, "try connecting to static configuration %s:%d",
             ipaddr_ntoa(&remote_ip), remotePort);
#endif

    if (remote_ip.type == IPADDR_TYPE_V4) {
      lwipNetconn = netconn_new(NETCONN_TCP);

      ESP_LOGV(TAG, "netconn using IPv4");
    } else if (remote_ip.type == IPADDR_TYPE_V6) {
      lwipNetconn = netconn_new(NETCONN_TCP_IPV6);

      ESP_LOGV(TAG, "netconn using IPv6");
    } else {
      ESP_LOGW(TAG, "remote IP has unsupported IP type");

      continue;
    }

    if (lwipNetconn == NULL) {
      ESP_LOGE(TAG, "can't create netconn");

      continue;
    }

#define USE_INTERFACE_BIND

#ifdef USE_INTERFACE_BIND  // use interface to bind connection
    uint8_t netifIdx = esp_netif_get_netif_impl_index(*netif);
    rc1 = netconn_bind_if(lwipNetconn, netifIdx);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "can't bind interface %s", network_get_ifkey(*netif));
    }
#else  // use IP to bind connection
    if (remote_ip.type == IPADDR_TYPE_V4) {
      // rc1 = netconn_bind(lwipNetconn, &ipAddr, 0);
      rc1 = netconn_bind(lwipNetconn, IP4_ADDR_ANY, 0);
    } else {
      rc1 = netconn_bind(lwipNetconn, IP6_ADDR_ANY, 0);
    }

    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "can't bind local IP");
    }
#endif
    rc2 = netconn_connect(lwipNetconn, &remote_ip, remotePort);
    if (rc2 != ERR_OK) {
      ESP_LOGE(TAG, "can't connect to remote %s:%d, err %d",
               ipaddr_ntoa(&remote_ip), remotePort, rc2);

#if !SNAPCAST_SERVER_USE_MDNS
      vTaskDelay(pdMS_TO_TICKS(1000));
#endif
    }

    if (rc1 != ERR_OK || rc2 != ERR_OK) {
      netconn_close(lwipNetconn);
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;

      continue;
    }

    ESP_LOGI(TAG, "netconn connected using %s", network_get_ifkey(*netif));
    break;  // SUCCESS
  }
}

int receive_data(struct netbuf** firstNetBuf, bool isMuted, esp_netif_t* netif,
                 bool* first_receive, int rc1) {
  // delete old netbuf. Restart connection if required
  if (*first_receive) {
    *first_receive = false;
  } else {
    netbuf_delete(*firstNetBuf);

    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "Data error, closing netconn");

      netconn_close(lwipNetconn);
      return -1;
    }
  }

  while (1) {
    int rc2 = netconn_recv(lwipNetconn, firstNetBuf);
    if (rc2 != ERR_OK) {
      ESP_LOGE(TAG, "netconn err %d", rc2);
      if (rc2 == ERR_CONN) {
        netconn_close(lwipNetconn);

        // restart and try to reconnect
        return -1;
      }

      if (*firstNetBuf != NULL) {
        netbuf_delete(*firstNetBuf);

        *firstNetBuf = NULL;
      }
      continue;
    }
    break;
  }

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  if (isMuted) {
    esp_netif_t* eth_netif =
        network_get_netif_from_desc(NETWORK_INTERFACE_DESC_ETH);

    if (netif != eth_netif) {
      bool ethUp = network_is_netif_up(eth_netif);

      if (ethUp) {
        netconn_close(lwipNetconn);

        if (*firstNetBuf != NULL) {
          netbuf_delete(*firstNetBuf);

          *firstNetBuf = NULL;
        }

        // restart and try to reconnect using preferred interface ETH
        return -1;
      }
    }
  }
#endif
  return 0;
}

int fill_buffer(bool* first_netbuf_processed, int* rc1,
                struct netbuf* firstNetBuf, char** start, uint16_t* len) {
  while (1) {
    // currentPos = 0;
    if (!*first_netbuf_processed) {
      netbuf_first(firstNetBuf);
      *first_netbuf_processed = true;
    } else {
      if (netbuf_next(firstNetBuf) < 0) {
        return -1;  // fetch new data from network
      }
    }

    *rc1 = netbuf_data(firstNetBuf, (void**)start, len);
    if (*rc1 == ERR_OK) {
      // ESP_LOGI (TAG, "netconn rx, data len: %d, %d",
      // len, netbuf_len(firstNetBuf));
      return 0;
    } else {
      ESP_LOGE(TAG, "netconn rx, couldn't get data");
      continue;  // try again
    }
    break;  // not reached, defensive programming
  }
  return 0;  // not reached, defensive programming
}

int connection_ensure_byte(connection_t* connection) {
  // iterate until we could read data
  while (1) {
    switch (connection->state) {
      case CONNECTION_INITIALIZED: {
        if (receive_data(&connection->firstNetBuf, *connection->isMuted,
                         connection->netif, &connection->first_receive,
                         connection->rc1) != 0) {
          connection->state = CONNECTION_RESTART_REQUIRED;
          break;  // restart connection
        }
        connection->first_netbuf_processed = false;
        connection->state = CONNECTION_DATA_RECEIVED;
        break;
      }

      case CONNECTION_DATA_RECEIVED: {
        if (fill_buffer(&connection->first_netbuf_processed, &connection->rc1,
                        connection->firstNetBuf, &connection->start,
                        &connection->len) != 0) {
          connection->state = CONNECTION_INITIALIZED;
          break;  // fetch new data from network
        }
        connection->state = CONNECTION_BUFFER_FILLED;
        break;
      }

      case CONNECTION_BUFFER_FILLED: {
        if (connection->len <= 0) {
          connection->state = CONNECTION_DATA_RECEIVED;
          break;
        }
        connection->rc1 = ERR_OK;  // probably not necessary
        // We can read data now!
        return 0;
      }

      case CONNECTION_RESTART_REQUIRED: {
        return -1;
      }
    }
  }
}

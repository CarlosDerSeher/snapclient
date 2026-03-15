#include "connection_handler.h"

#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "mdns.h"
#include "net_functions.h"
#include "network_interface.h"
#include "settings_manager.h"

// External variable that need to be accessible
extern struct netconn* lwipNetconn;

static const char* TAG = "CONNECTION_HANDLER";

void setup_network(esp_netif_t** netif) {
  int rc1, rc2 = ERR_OK;
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
        *netif = eth_netif;

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

    /* Decide at runtime whether to use mDNS or static server config.
     * The settings_manager holds the mdns flag and optional server host/port.
     */
    ip_addr_t remote_ip;
    bool use_mdns = true;
    if (settings_get_mdns_enabled(&use_mdns) != ESP_OK) {
      use_mdns = true;  // default to mdns if error
    }

#ifndef CONFIG_SNAPSERVER_USE_MDNS
    if (use_mdns) {
      ESP_LOGW(TAG,
               "mDNS requested in settings but not compiled in; falling back "
               "to static server settings");
      use_mdns = false;
    }
#endif

    if (use_mdns) {
#if CONFIG_SNAPSERVER_USE_MDNS
      ESP_LOGI(TAG, "Enable mdns");
      mdns_init();
#endif
      // Find snapcast server via mDNS
      mdns_result_t* r = NULL;
      esp_err_t err = 0;
      while (!r || err) {
        ESP_LOGI(TAG, "Lookup snapcast service on network");
        err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &r);
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

      mdns_result_t* re = r;
      while (re) {
        mdns_ip_addr_t* a = re->addr;
        if (a == NULL) {
          // No address in this result, skip to next
          re = re->next;
          continue;
        }
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

      if (!re || !re->addr) {
        mdns_query_results_free(r);

        ESP_LOGW(TAG, "didn't find any valid IP in MDNS query");

        continue;
      }

      ip_addr_copy(remote_ip, re->addr->addr);
      remotePort = r->port;

      mdns_query_results_free(r);

      ESP_LOGI(TAG, "Found %s:%d", ipaddr_ntoa(&remote_ip), remotePort);
    } else {
      // Use static server configuration from settings_manager
      char static_host[128] = {0};
      int32_t static_port = 0;
      if (settings_get_server_host(static_host, sizeof(static_host)) !=
              ESP_OK ||
          static_host[0] == '\0') {
        ESP_LOGW(TAG, "Static server not configured in settings, skipping");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      if (settings_get_server_port(&static_port) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read static server port from settings");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      if (static_port == 0) {
        ESP_LOGW(TAG, "Static server port is 0/unset, skipping");
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      if (ipaddr_aton(static_host, &remote_ip) == 0) {
        ESP_LOGE(TAG, "can't convert static server address to numeric: %s",
                 static_host);
        continue;
      }

      remotePort = (uint16_t)static_port;

      ESP_LOGI(TAG, "try connecting to static configuration %s:%d",
               ipaddr_ntoa(&remote_ip), remotePort);
    }

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

    //    netconn_set_flags(lwipNetconn, TF_NODELAY);

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
    // tcp_nagle_disable(pcb)

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

static int receive_data(struct netbuf** firstNetBuf, bool isMuted,
                        void* before_receive_callback_data,
                        void (*before_receive_callback)(void* data),
                        esp_netif_t* netif, bool* first_receive, int rc1) {
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
    before_receive_callback(before_receive_callback_data);

    // start receive
    int rc2 = netconn_recv(lwipNetconn, firstNetBuf);
    if (rc2 != ERR_OK) {
      if (rc2 == ERR_CONN) {
        netconn_close(lwipNetconn);
        ESP_LOGD(TAG, "netconn connection closed (%d)", rc2);
        // restart and try to reconnect
        return -1;
      } else if (rc2 == ERR_TIMEOUT) {
        ESP_LOGD(TAG, "netconn rx timeout (%d)", rc2);
      } else {
        ESP_LOGE(TAG, "netconn err %d", rc2);
      }

      if (*firstNetBuf != NULL) {
        netbuf_delete(*firstNetBuf);

        *firstNetBuf = NULL;
      }
      continue;
    } else {
      ESP_LOGD(TAG, "netconn rx OK");
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

static int fill_buffer(bool* first_netbuf_processed, int* rc1,
                       struct netbuf* firstNetBuf, char** start,
                       uint16_t* len) {
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
      ESP_LOGD(TAG, "netconn rx, data len: %d, %d", *len,
               netbuf_len(firstNetBuf));
      return 0;
    } else {
      ESP_LOGE(TAG, "netconn rx, couldn't get data");
      continue;  // try again
    }
    break;  // not reached, defensive programming
  }
  return 0;  // not reached, defensive programming
}

static int connection_ensure_byte(connection_t* connection) {
  // iterate until we could read data
  while (1) {
    switch (connection->state) {
      case CONNECTION_INITIALIZED: {
        if (receive_data(&connection->firstNetBuf, *connection->isMuted,
                         connection->before_receive_callback_data,
                         connection->before_receive_callback, connection->netif,
                         &connection->first_receive, connection->rc1) != 0) {
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
        // This case should remain separate.
        // This way, calling the function again will always just yield -1
        return -1;
      }
    }
  }
}

int connection_get_byte(connection_t* connection, char* buffer) {
  if (connection_ensure_byte(connection) != 0) {
    return -1;
  }
  *buffer = *(connection->start);
  connection->start++;
  connection->len--;
  return 0;
}

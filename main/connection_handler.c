#include "connection_handler.h"

#include "esp_log.h"
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
#include "eth_interface.h"
#endif
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
    // Reset netif to ensure clean state for each connection attempt
    // This prevents carrying over interface selection from previous failed attempts
    *netif = NULL;

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
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    int eth_wait_count = 0;
#endif
    while (1) {
      // If an external module has set a preferred/default netif, prefer it
      // when it already has an IP. This helps when `eth_interface.c` sets the
      // default netif to Ethernet — main will then bind/connect using that
      // default instead of falling back to WiFi.
      esp_netif_t *default_netif = esp_netif_get_default_netif();
      if (default_netif != NULL) {
        if (network_has_ip(default_netif)) {
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
          // If WiFi has IP but Ethernet takeover is pending (ETH linked up,
          // waiting for DHCP), wait for ETH instead of connecting via WiFi.
          // This avoids a connect-disconnect-reconnect cycle at boot.
          if (default_netif != eth_netif && eth_is_takeover_pending()) {
            ESP_LOGI(TAG, "WiFi ready but Ethernet takeover pending, waiting for ETH...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
          }
          // Ethernet is enabled but hasn't linked up yet - give it a brief
          // grace period before committing to WiFi. Without this, WiFi gets
          // used first and the subsequent ETH takeover causes a 60s TCP timeout.
          if (default_netif != eth_netif && eth_is_enabled() &&
              !network_has_ip(eth_netif)) {
            if (eth_wait_count < 3) {
              eth_wait_count++;
              ESP_LOGI(TAG, "WiFi ready, waiting for Ethernet link-up (%d/3)...", eth_wait_count);
              vTaskDelay(pdMS_TO_TICKS(1000));
              continue;
            }
            ESP_LOGI(TAG, "Ethernet not up after grace period, proceeding with WiFi");
            eth_wait_count = 0;
          }
#endif
          *netif = default_netif;
          ESP_LOGI(TAG, "Using default netif: %s", network_get_ifkey(*netif));
          break;
        }
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
        else if (default_netif == eth_netif) {
          // Ethernet was explicitly set as default (takeover in progress)
          // but DHCP hasn't completed yet. Wait for it — WiFi won't work
          // because the switch learned our MAC on the Ethernet port.
          ESP_LOGI(TAG, "Default netif %s waiting for IP...",
                   network_get_ifkey(default_netif));
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }
#endif
        // For WiFi or other default netif without IP, fall through to
        // normal Ethernet-priority check below
      }

      // Wait for network with Ethernet priority
      // If WiFi comes up first, wait a bit longer to see if Ethernet comes up
      if (*netif == NULL) {
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
        bool ethUp = network_has_ip(eth_netif);

        if (ethUp) {
          *netif = eth_netif;
          ESP_LOGI(TAG, "Using Ethernet interface");
          break;
        }
#endif

        bool staUp = network_has_ip(sta_netif);
        if (staUp) {
          *netif = sta_netif;
          ESP_LOGI(TAG, "Using WiFi interface");
          break;
        }
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
      // mDNS is already initialized by net_mdns_register() in app_main().
      // Do NOT call mdns_init() here - repeated calls on reconnect corrupt
      // the multicast socket state and cause query failures.

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
          // Found valid IPv6 address - use it with already-selected interface
          // Do NOT overwrite *netif with re->esp_netif!
          // Interface was selected in setup_network() based on Ethernet priority.
          // The mDNS result only tells us the server IP, not which interface to use.
          break;
        }

        // TODO: fall back to IPv4 if no IPv6 was available
#else
        if (a->addr.type == IPADDR_TYPE_V4) {
          // Found valid IPv4 address - use it with already-selected interface
          // Do NOT overwrite *netif with re->esp_netif!
          // Interface was selected in setup_network() based on Ethernet priority.
          // The mDNS result only tells us the server IP, not which interface to use.
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

  // Ethernet preference is handled by eth_interface.c takeover system.
  // It sets the default netif and requests reconnect when ready.
  // The old muted-check-and-force-restart logic was removed because it
  // conflicts with the takeover system and causes infinite reconnect loops.
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
      ESP_LOGD(TAG, "netconn rx, data len: %u, %u", *len,
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

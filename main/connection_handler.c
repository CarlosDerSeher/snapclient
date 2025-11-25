#include "connection_handler.h"

#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "network_interface.h"

// External variable that need to be accessible
extern struct netconn* lwipNetconn;

static const char* TAG = "CONNECTION_HANDLER";

int receive_data(struct netbuf** firstNetBuf, bool isMuted,
                 esp_netif_t* netif) {
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

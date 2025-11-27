#ifndef __CONNECTION_HANDLER_H__
#define __CONNECTION_HANDLER_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"
#include "lwip/api.h"
#include "lwip/netdb.h"

typedef enum {
  CONNECTION_INITIALIZED,
  CONNECTION_DATA_RECEIVED,
  CONNECTION_BUFFER_FILLED,
  CONNECTION_RESTART_REQUIRED,
} connection_state_t;

// Function declarations for connection handling
void setup_network(esp_netif_t** netif);
int receive_data(struct netbuf** firstNetBuf, bool isMuted, esp_netif_t* netif,
                 bool* first_receive, int rc1);
int fill_buffer(bool* first_netbuf_processed, int* rc1,
                struct netbuf* firstNetBuf, char** start, uint16_t* len);
int connection_ensure_byte(connection_state_t* connection_state,
                           struct netbuf** firstNetBuf, bool isMuted,
                           esp_netif_t* netif, bool* first_receive, int* rc1,
                           bool* first_netbuf_processed, char** start,
                           uint16_t* len);
// Add other connection-related functions you plan to move here

#endif  // __CONNECTION_HANDLER_H__

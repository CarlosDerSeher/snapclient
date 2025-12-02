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

typedef struct {
  esp_netif_t* netif;
  bool* isMuted;
  struct netbuf* firstNetBuf;
  // buffer
  char* start;
  uint16_t len;
  // state
  connection_state_t state;
  bool first_receive;
  bool first_netbuf_processed;
  int rc1;
} connection_t;

// Function declarations for connection handling
void setup_network(esp_netif_t** netif);

int connection_get_byte(connection_t* connection, char* buffer);

#endif  // __CONNECTION_HANDLER_H__

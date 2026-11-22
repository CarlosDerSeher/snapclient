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
  // not really connection's concern, but needed.
  bool* isMuted;
  void* before_receive_callback_data;
  void (*before_receive_callback)(void* data);
  // netbuf handling
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
// Stream bytes read by connection_get_byte() since the connection opened
uint64_t connection_bytes_consumed(void);
// When stream byte pos (0-based) arrived from the network, in esp_timer
// microseconds, or -1 if that is no longer known
int64_t connection_arrival_us(uint64_t pos);

#endif  // __CONNECTION_HANDLER_H__

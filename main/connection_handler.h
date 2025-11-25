#ifndef __CONNECTION_HANDLER_H__
#define __CONNECTION_HANDLER_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"
#include "lwip/api.h"
#include "lwip/netdb.h"

// Function declarations for connection handling
int receive_data(struct netbuf** firstNetBuf, bool isMuted, esp_netif_t* netif);
int fill_buffer(bool* first_netbuf_processed, int* rc1,
                struct netbuf* firstNetBuf, char** start, uint16_t* len);

// Add other connection-related functions you plan to move here

#endif  // __CONNECTION_HANDLER_H__

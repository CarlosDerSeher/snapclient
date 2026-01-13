#ifndef __SNAPCAST_H__
#define __SNAPCAST_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum message_type {
  SNAPCAST_MESSAGE_BASE = 0,
  SNAPCAST_MESSAGE_CODEC_HEADER = 1,
  SNAPCAST_MESSAGE_WIRE_CHUNK = 2,
  SNAPCAST_MESSAGE_SERVER_SETTINGS = 3,
  SNAPCAST_MESSAGE_TIME = 4,
  SNAPCAST_MESSAGE_HELLO = 5,
  // SNAPCAST_MESSAGE_STREAM_TAGS = 6,
  SNAPCAST_MESSAGE_CLIENT_INFO = 7,

  SNAPCAST_MESSAGE_FIRST = SNAPCAST_MESSAGE_BASE,
  SNAPCAST_MESSAGE_LAST = SNAPCAST_MESSAGE_CLIENT_INFO,
  SNAPCAST_MESSAGE_INVALID
};

typedef struct tv {
  int32_t sec;
  int32_t usec;
} tv_t;

typedef struct base_message {
  uint16_t type;
  uint16_t id;
  uint16_t refersTo;
  tv_t sent;
  tv_t received;
  uint32_t size;
} base_message_t;

#define BASE_MESSAGE_SIZE 26
#define TIME_MESSAGE_SIZE 8

int base_message_serialize(base_message_t *msg, char *data, uint32_t size);

int base_message_deserialize(base_message_t *msg, const char *data,
                             uint32_t size);

/* Sample Hello message
{
    "Arch": "x86_64",
    "ClientName": "Snapclient",
    "HostName": "my_hostname",
    "ID": "00:11:22:33:44:55",
    "Instance": 1,
    "MAC": "00:11:22:33:44:55",
    "OS": "Arch Linux",
    "SnapStreamProtocolVersion": 2,
    "Version": "0.17.1"
}
*/

typedef struct hello_message {
  char *mac;
  char *hostname;
  char *version;
  char *client_name;
  char *os;
  char *arch;
  int instance;
  char *id;
  int protocol_version;
} hello_message_t;

char *hello_message_serialize(hello_message_t *msg, size_t *size);

typedef struct server_settings_message {
  int32_t buffer_ms;
  int32_t latency;
  uint32_t volume;
  bool muted;
} server_settings_message_t;

int server_settings_message_deserialize(server_settings_message_t *msg,
                                        const char *json_str);

typedef struct codec_header_message {
  char *codec;
  uint32_t size;
  char *payload;
} codec_header_message_t;

int codec_header_message_deserialize(codec_header_message_t *msg,
                                     const char *data, uint32_t size);
void codec_header_message_free(codec_header_message_t *msg);

typedef struct wire_chunk_message {
  tv_t timestamp;
  size_t size;
  char *payload;
} wire_chunk_message_t;

// TODO currently copies, could be made to not copy probably
int wire_chunk_message_deserialize(wire_chunk_message_t *msg, const char *data,
                                   uint32_t size);
void wire_chunk_message_free(wire_chunk_message_t *msg);

typedef struct time_message {
  tv_t latency;
} time_message_t;

int time_message_serialize(time_message_t *msg, char *data, uint32_t size);
int time_message_deserialize(time_message_t *msg, const char *data,
                             uint32_t size);

#ifdef __cplusplus
}
#endif

#endif  // __SNAPCAST_H__

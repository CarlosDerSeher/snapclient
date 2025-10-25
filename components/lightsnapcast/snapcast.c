#include "snapcast.h"

//#ifdef ESP_PLATFORM
// The ESP-IDF changes the include directory for cJSON
#include <cJSON.h>
//#else
//#include "json/cJSON.h"
//#endif

#include <buffer.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/* Logging tag */
static const char *TAG = "libSNAPCAST";

int base_message_serialize(base_message_t *msg, char *data, uint32_t size) {
  write_buffer_t buffer;
  int result = 0;

  buffer_write_init(&buffer, data, size);

  result |= buffer_write_uint16(&buffer, msg->type);
  result |= buffer_write_uint16(&buffer, msg->id);
  result |= buffer_write_uint16(&buffer, msg->refersTo);
  result |= buffer_write_int32(&buffer, msg->sent.sec);
  result |= buffer_write_int32(&buffer, msg->sent.usec);
  result |= buffer_write_int32(&buffer, msg->received.sec);
  result |= buffer_write_int32(&buffer, msg->received.usec);
  result |= buffer_write_uint32(&buffer, msg->size);

  return result;
}

int base_message_deserialize(base_message_t *msg, const char *data,
                             uint32_t size) {
  read_buffer_t buffer;
  int result = 0;

  buffer_read_init(&buffer, data, size);

  result |= buffer_read_uint16(&buffer, &(msg->type));
  result |= buffer_read_uint16(&buffer, &(msg->id));
  result |= buffer_read_uint16(&buffer, &(msg->refersTo));
  result |= buffer_read_int32(&buffer, &(msg->sent.sec));
  result |= buffer_read_int32(&buffer, &(msg->sent.usec));
  result |= buffer_read_int32(&buffer, &(msg->received.sec));
  result |= buffer_read_int32(&buffer, &(msg->received.usec));
  result |= buffer_read_uint32(&buffer, &(msg->size));

  return result;
}

static cJSON *hello_message_to_json(hello_message_t *msg) {
  cJSON *mac;
  cJSON *hostname;
  cJSON *version;
  cJSON *client_name;
  cJSON *os;
  cJSON *arch;
  cJSON *instance;
  cJSON *id;
  cJSON *protocol_version;
  cJSON *json = NULL;

  json = cJSON_CreateObject();
  if (!json) {
    goto error;
  }

  mac = cJSON_CreateString(msg->mac);
  if (!mac) {
    goto error;
  }
  cJSON_AddItemToObject(json, "MAC", mac);

  hostname = cJSON_CreateString(msg->hostname);
  if (!hostname) {
    goto error;
  }
  cJSON_AddItemToObject(json, "HostName", hostname);

  version = cJSON_CreateString(msg->version);
  if (!version) {
    goto error;
  }
  cJSON_AddItemToObject(json, "Version", version);

  client_name = cJSON_CreateString(msg->client_name);
  if (!client_name) {
    goto error;
  }
  cJSON_AddItemToObject(json, "ClientName", client_name);

  os = cJSON_CreateString(msg->os);
  if (!os) {
    goto error;
  }
  cJSON_AddItemToObject(json, "OS", os);

  arch = cJSON_CreateString(msg->arch);
  if (!arch) {
    goto error;
  }
  cJSON_AddItemToObject(json, "Arch", arch);

  instance = cJSON_CreateNumber(msg->instance);
  if (!instance) {
    goto error;
  }
  cJSON_AddItemToObject(json, "Instance", instance);

  id = cJSON_CreateString(msg->id);
  if (!id) {
    goto error;
  }
  cJSON_AddItemToObject(json, "ID", id);

  protocol_version = cJSON_CreateNumber(msg->protocol_version);
  if (!protocol_version) {
    goto error;
  }
  cJSON_AddItemToObject(json, "SnapStreamProtocolVersion", protocol_version);

  goto end;
error:
  cJSON_Delete(json);

end:
  return json;
}

char *hello_message_serialize(hello_message_t *msg, size_t *size) {
  int str_length, prefixed_length;
  cJSON *json;
  char *str = NULL;
  char *prefixed_str = NULL;

  json = hello_message_to_json(msg);
  if (!json) {
    return NULL;
  }

  str = cJSON_PrintUnformatted(json);
  if (!str) {
    return NULL;
  }
  cJSON_Delete(json);

  str_length = strlen(str);
  prefixed_length = str_length + 4;
  prefixed_str = malloc(prefixed_length);
  if (!prefixed_str) {
    return NULL;
  }

  prefixed_str[0] = str_length & 0xff;
  prefixed_str[1] = (str_length >> 8) & 0xff;
  prefixed_str[2] = (str_length >> 16) & 0xff;
  prefixed_str[3] = (str_length >> 24) & 0xff;
  memcpy(&(prefixed_str[4]), str, str_length);
  free(str);
  *size = prefixed_length;

  return prefixed_str;
}

int server_settings_message_deserialize(server_settings_message_t *msg,
                                        const char *json_str) {
  int status = 1;
  cJSON *value = NULL;
  cJSON *json = cJSON_Parse(json_str);
  if (!json) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr) {
      ESP_LOGE(TAG, "Error before: %s", error_ptr);
      goto end;
    }
  }

  if (msg == NULL) {
    status = 2;
    goto end;
  }

  value = cJSON_GetObjectItemCaseSensitive(json, "bufferMs");
  if (cJSON_IsNumber(value)) {
    msg->buffer_ms = value->valueint;
  }

  value = cJSON_GetObjectItemCaseSensitive(json, "latency");
  if (cJSON_IsNumber(value)) {
    msg->latency = value->valueint;
  }

  value = cJSON_GetObjectItemCaseSensitive(json, "volume");
  if (cJSON_IsNumber(value)) {
    msg->volume = value->valueint;
  }

  value = cJSON_GetObjectItemCaseSensitive(json, "muted");
  msg->muted = cJSON_IsTrue(value);
  status = 0;
end:
  cJSON_Delete(json);
  return status;
}

int codec_header_message_deserialize(codec_header_message_t *msg,
                                     const char *data, uint32_t size) {
  read_buffer_t buffer;
  uint32_t string_size;
  int result = 0;

  buffer_read_init(&buffer, data, size);

  result |= buffer_read_uint32(&buffer, &string_size);
  if (result) {
    // Can't allocate the proper size string if we didn't read the size, so
    // fail early
    return 1;
  }

  msg->codec = malloc(string_size + 1);
  if (!msg->codec) {
    return 2;
  }

  result |= buffer_read_buffer(&buffer, msg->codec, string_size);
  // Make sure the codec is a proper C string by terminating it with a null
  // character
  msg->codec[string_size] = '\0';

  result |= buffer_read_uint32(&buffer, &(msg->size));
  if (result) {
    // Can't allocate the proper size string if we didn't read the size, so
    // fail early
    return 1;
  }

  msg->payload = &data[buffer.index];

  return result;
}

int wire_chunk_message_deserialize(wire_chunk_message_t *msg, const char *data,
                                   uint32_t size) {
  read_buffer_t buffer;
  int result = 0;

  buffer_read_init(&buffer, data, size);

  result |= buffer_read_int32(&buffer, &(msg->timestamp.sec));
  result |= buffer_read_int32(&buffer, &(msg->timestamp.usec));
  result |= buffer_read_uint32(&buffer, (uint32_t *)&(msg->size));

  // If there's been an error already (especially for the size bit) return
  // early
  if (result) {
    return result;
  }

  msg->payload = &data[buffer.index];

  // Failed to allocate the memory
  if (!msg->payload) {
    return 2;
  }

  // result |= buffer_read_buffer(&buffer, msg->payload, msg->size);
  return result;
}

void codec_header_message_free(codec_header_message_t *msg) {
  free(msg->codec);
  msg->codec = NULL;
}

void wire_chunk_message_free(wire_chunk_message_t *msg) {}

int time_message_serialize(time_message_t *msg, char *data, uint32_t size) {
  write_buffer_t buffer;
  int result = 0;

  buffer_write_init(&buffer, data, size);

  result |= buffer_write_int32(&buffer, msg->latency.sec);
  result |= buffer_write_int32(&buffer, msg->latency.usec);

  return result;
}

int time_message_deserialize(time_message_t *msg, const char *data,
                             uint32_t size) {
  read_buffer_t buffer;
  int result = 0;

  buffer_read_init(&buffer, data, size);

  result |= buffer_read_int32(&buffer, &(msg->latency.sec));
  result |= buffer_read_int32(&buffer, &(msg->latency.usec));

  return result;
}

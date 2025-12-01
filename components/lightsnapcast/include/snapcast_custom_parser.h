#ifndef __SNAPCAST_CUSTOM_PARSER_H__
#define __SNAPCAST_CUSTOM_PARSER_H__

#include "snapcast.h"
#include "player.h" // needed for coded_type_t


#define BASE_MESSAGE_STATE 0
#define TYPED_MESSAGE_STATE 1

typedef struct decoderData_s {
  uint32_t type;  // should be SNAPCAST_MESSAGE_CODEC_HEADER
                  // or SNAPCAST_MESSAGE_WIRE_CHUNK
  uint8_t *inData;
  tv_t timestamp;
  uint8_t *outData;
  uint32_t bytes;
} decoderData_t;


typedef struct {
  uint32_t state;  // BASE_MESSAGE_STATE or TYPED_MESSAGE_STATE
  uint32_t internalState;
  size_t typedMsgCurrentPos;
} snapcast_custom_parser_t;

typedef enum {
  PARSER_COMPLETE = 0,
  PARSER_INCOMPLETE,
  PARSER_CRITICAL_ERROR,
  PARSER_CONNECTION_ERROR
} parser_return_state_t;

typedef int (*buffer_refill_function_t)(void* connection_data);

typedef int (*wire_chunk_callback_t)(codec_type_t codec,
                                  void* scSet,
                                  pcm_chunk_message_t** pcmData,
                                  wire_chunk_message_t* wire_chnk); 

// Callback function type for time sync message completion
typedef void (*time_sync_callback_t)(base_message_t *base_message_rx,
                                    time_message_t *time_message_rx,
                                    void *time_sync_data,
                                    bool received_codec_header);

typedef int (*server_settings_callback_t)(char* serverSettingsString, void* scSet);

typedef int (*codec_header_callback_t)(char** codecPayload,
                                  uint32_t typedMsgLen,
                                  codec_type_t codec,
                                  snapcastSetting_t* scSet,
                                  void* time_sync_data);


void parser_reset_state(snapcast_custom_parser_t* parser);

parser_return_state_t parse_base_message(snapcast_custom_parser_t *parser,
                                         base_message_t *base_message_rx, char **start, uint16_t* len,
                                         buffer_refill_function_t refill_function, void* connection_data);

parser_return_state_t parse_wire_chunk_message(snapcast_custom_parser_t* parser,
                                               base_message_t* base_message_rx,
                                               char** start,
                                               uint16_t* len,
                                               uint32_t* offset,
                                               bool received_codec_header,
                                               codec_type_t codec,
                                               pcm_chunk_message_t** pcmData,
                                               wire_chunk_message_t* wire_chnk,
                                               uint32_t* payloadOffset,
                                               uint32_t* tmpData,
                                               decoderData_t* decoderChunk,
                                               int32_t* payloadDataShift);

parser_return_state_t parse_codec_header_message(snapcast_custom_parser_t* parser,
                                                 char** start,
                                                 uint16_t* len,
                                                 uint32_t* typedMsgLen,
                                                 uint32_t* offset,
                                                 bool* received_codec_header,
                                                 char** codecString,
                                                 codec_type_t* codec,
                                                 char** codecPayload);

parser_return_state_t parse_sever_settings_message(snapcast_custom_parser_t *parser,
                                                   base_message_t* base_message_rx,
                                                   char** start,
                                                   uint16_t* len,
                                                   uint32_t* typedMsgLen,
                                                   uint32_t* offset,
                                                   char** serverSettingsString);


parser_return_state_t parse_time_message(snapcast_custom_parser_t* parser,
                                         base_message_t* base_message_rx,
                                         time_message_t* time_message_rx,
                                         char** start, uint16_t* len);


void parse_unknown_message(snapcast_custom_parser_t* parser,
                           base_message_t* base_message_rx,
                           char** start,
                           uint16_t* len);

#endif  // __SNAPCAST_CUSTOM_PARSER_H__

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


typedef int (*get_byte_callback_t)(void* connection_data, char* buffer);

typedef struct {
  get_byte_callback_t get_byte_function;
  void* get_byte_context;
  uint32_t state;  // BASE_MESSAGE_STATE or TYPED_MESSAGE_STATE
} snapcast_custom_parser_t;

typedef enum {
  PARSER_COMPLETE = 0,
  PARSER_INCOMPLETE,
  PARSER_CRITICAL_ERROR,
  PARSER_CONNECTION_ERROR,
} parser_return_state_t;

void parser_reset_state(snapcast_custom_parser_t* parser);

parser_return_state_t parse_base_message(snapcast_custom_parser_t* parser,
                                         base_message_t* base_message_rx);

parser_return_state_t parse_wire_chunk_message(snapcast_custom_parser_t* parser,
                                               base_message_t* base_message_rx,
                                               bool received_codec_header,
                                               codec_type_t codec,
                                               pcm_chunk_message_t** pcmData,
                                               wire_chunk_message_t* wire_chnk,
                                               decoderData_t* decoderChunk);

parser_return_state_t parse_codec_header_message(
    snapcast_custom_parser_t* parser, uint32_t* typedMsgLen,
    bool* received_codec_header, char** codecString, codec_type_t* codec,
    char** codecPayload);

parser_return_state_t parse_sever_settings_message(
    snapcast_custom_parser_t* parser, base_message_t* base_message_rx,
    char** serverSettingsString);

parser_return_state_t parse_time_message(snapcast_custom_parser_t* parser,
                                         base_message_t* base_message_rx,
                                         time_message_t* time_message_rx);

parser_return_state_t parse_unknown_message(snapcast_custom_parser_t* parser,
                                            base_message_t* base_message_rx);

#endif  // __SNAPCAST_CUSTOM_PARSER_H__

#ifndef __SNAPCAST_CUSTOM_PARSER_H__
#define __SNAPCAST_CUSTOM_PARSER_H__

#include "snapcast.h"

#define BASE_MESSAGE_STATE 0
#define TYPED_MESSAGE_STATE 1

typedef struct {
  uint32_t state;  // BASE_MESSAGE_STATE or TYPED_MESSAGE_STATE
  uint32_t internalState;
  size_t typedMsgCurrentPos;
} snapcast_custom_parser_t;

// Callback function type for time sync message completion
typedef void (*time_sync_callback_t)(base_message_t *base_message_rx,
                                    time_message_t *time_message_rx,
                                    void *time_sync_data,
                                    bool received_codec_header);

typedef int (*server_settings_callback_t)(char* serverSettingsString, void* scSet);

void parse_base_message(snapcast_custom_parser_t *parser,
                        base_message_t *base_message_rx, const char *start,
                        int64_t *now);


int parse_sever_settings_message(snapcast_custom_parser_t *parser,
                             base_message_t* base_message_rx,
                             char** start,
                             uint16_t* len,
                             uint32_t* typedMsgLen,
                             uint32_t* offset,
                             char** serverSettingsString,
                             void* scSet,
                             server_settings_callback_t callback);

void parse_time_message(snapcast_custom_parser_t* parser,
                        base_message_t* base_message_rx,
                        time_message_t* time_message_rx,
                        char** start,
                        uint16_t* len,
                        void* time_sync_data,
                        bool received_codec_header,
                        time_sync_callback_t callback);

#endif  // __SNAPCAST_CUSTOM_PARSER_H__

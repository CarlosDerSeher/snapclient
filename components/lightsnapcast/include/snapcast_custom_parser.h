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

void parse_base_message(snapcast_custom_parser_t *parser,
                        base_message_t *base_message_rx, const char *start,
                        int64_t *now);

#endif  // __SNAPCAST_CUSTOM_PARSER_H__

#include "snapcast_custom_parser.h"

#include "esp_timer.h"

void parse_base_message(snapcast_custom_parser_t *parser,
                        base_message_t *base_message_rx, const char *start,
                        int64_t *now) {
  switch (parser->internalState) {
    case 0:
      base_message_rx->type = *start & 0xFF;
      parser->internalState++;
      break;

    case 1:
      base_message_rx->type |= (*start & 0xFF) << 8;
      parser->internalState++;
      break;

    case 2:
      base_message_rx->id = *start & 0xFF;
      parser->internalState++;
      break;

    case 3:
      base_message_rx->id |= (*start & 0xFF) << 8;
      parser->internalState++;
      break;

    case 4:
      base_message_rx->refersTo = *start & 0xFF;
      parser->internalState++;
      break;

    case 5:
      base_message_rx->refersTo |= (*start & 0xFF) << 8;
      parser->internalState++;
      break;

    case 6:
      base_message_rx->sent.sec = *start & 0xFF;
      parser->internalState++;
      break;

    case 7:
      base_message_rx->sent.sec |= (*start & 0xFF) << 8;
      parser->internalState++;
      break;

    case 8:
      base_message_rx->sent.sec |= (*start & 0xFF) << 16;
      parser->internalState++;
      break;

    case 9:
      base_message_rx->sent.sec |= (*start & 0xFF) << 24;
      parser->internalState++;
      break;

    case 10:
      base_message_rx->sent.usec = *start & 0xFF;
      parser->internalState++;
      break;

    case 11:
      base_message_rx->sent.usec |= (*start & 0xFF) << 8;
      parser->internalState++;
      break;

    case 12:
      base_message_rx->sent.usec |= (*start & 0xFF) << 16;
      parser->internalState++;
      break;

    case 13:
      base_message_rx->sent.usec |= (*start & 0xFF) << 24;
      parser->internalState++;
      break;

    case 14:
      base_message_rx->received.sec = *start & 0xFF;
      parser->internalState++;
      break;

    case 15:
      base_message_rx->received.sec |= (*start & 0xFF) << 8;
      parser->internalState++;
      break;

    case 16:
      base_message_rx->received.sec |= (*start & 0xFF) << 16;
      parser->internalState++;
      break;

    case 17:
      base_message_rx->received.sec |= (*start & 0xFF) << 24;
      parser->internalState++;
      break;

    case 18:
      base_message_rx->received.usec = *start & 0xFF;
      parser->internalState++;
      break;

    case 19:
      base_message_rx->received.usec |= (*start & 0xFF) << 8;
      parser->internalState++;
      break;

    case 20:
      base_message_rx->received.usec |= (*start & 0xFF) << 16;
      parser->internalState++;
      break;

    case 21:
      base_message_rx->received.usec |= (*start & 0xFF) << 24;
      parser->internalState++;
      break;

    case 22:
      base_message_rx->size = *start & 0xFF;
      parser->internalState++;
      break;

    case 23:
      base_message_rx->size |= (*start & 0xFF) << 8;
      parser->internalState++;
      break;

    case 24:
      base_message_rx->size |= (*start & 0xFF) << 16;
      parser->internalState++;
      break;

    case 25:
      base_message_rx->size |= (*start & 0xFF) << 24;
      parser->internalState = 0;

      *now = esp_timer_get_time();

      base_message_rx->received.sec = *now / 1000000;
      base_message_rx->received.usec =
          *now - base_message_rx->received.sec * 1000000;

      parser->typedMsgCurrentPos = 0;

      // ESP_LOGI(TAG, "BM type %d ts %ld.%ld, refers to %u",
      //          base_message_rx->type,
      //          base_message_rx->received.sec,
      //          base_message_rx->received.usec,
      //          base_message_rx->refersTo);

      // ESP_LOGI(TAG,"%u, %ld.%ld", base_message_rx->type,
      //                   base_message_rx->received.sec,
      //                   base_message_rx->received.usec);
      // ESP_LOGI(TAG,"%u, %llu", base_message_rx->type,
      //		                      1000000ULL *
      //                          (uint64_t)base_message_rx->received.sec
      //                          +
      //                          (uint64_t)base_message_rx->received.usec);

      parser->state = TYPED_MESSAGE_STATE;
      break;
  }
}

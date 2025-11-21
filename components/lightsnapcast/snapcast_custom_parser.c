#include "snapcast_custom_parser.h"

#include "esp_log.h"

static const char *TAG = "SNAPCAST_CUSTOM_PARSER";

void parser_reset_state(snapcast_custom_parser_t* parser) {
  parser->state = BASE_MESSAGE_STATE;
  parser->internalState = 0;
  parser->typedMsgCurrentPos = 0;
}

parser_return_state_t parse_base_message(snapcast_custom_parser_t *parser,
                                         base_message_t *base_message_rx, const char *start) {
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
      parser_reset_state(parser);
      parser->state = TYPED_MESSAGE_STATE;
      return PARSER_COMPLETE;
  }
  return PARSER_INCOMPLETE;
}



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
                                               int32_t* payloadDataShift) {
  switch (parser->internalState) {
    case 0: {
      wire_chnk->timestamp.sec = **start & 0xFF;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 1: {
      wire_chnk->timestamp.sec |= (**start & 0xFF) << 8;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 2: {
      wire_chnk->timestamp.sec |= (**start & 0xFF) << 16;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 3: {
      wire_chnk->timestamp.sec |= (**start & 0xFF) << 24;

      // ESP_LOGI(TAG,
      // "wire chunk time sec: %d",
      // wire_chnk->timestamp.sec);

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 4: {
      wire_chnk->timestamp.usec = (**start & 0xFF);

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 5: {
      wire_chnk->timestamp.usec |= (**start & 0xFF) << 8;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 6: {
      wire_chnk->timestamp.usec |= (**start & 0xFF) << 16;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 7: {
      wire_chnk->timestamp.usec |= (**start & 0xFF) << 24;

      // ESP_LOGI(TAG,
      // "wire chunk time usec: %d",
      // wire_chnk->timestamp.usec);

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 8: {
      wire_chnk->size = (**start & 0xFF);

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 9: {
      wire_chnk->size |= (**start & 0xFF) << 8;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 10: {
      wire_chnk->size |= (**start & 0xFF) << 16;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 11: {
      wire_chnk->size |= (**start & 0xFF) << 24;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      // TODO: we could use wire chunk directly maybe?
      decoderChunk->bytes = wire_chnk->size;
      while (!decoderChunk->inData) {
        decoderChunk->inData =
            (uint8_t *)malloc(decoderChunk->bytes);
        if (!decoderChunk->inData) {
          ESP_LOGW(TAG,
                   "malloc decoderChunk->inData failed, wait "
                   "1ms and try again");

          vTaskDelay(pdMS_TO_TICKS(1));
        }
      }

      *payloadOffset = 0;

#if 0
       ESP_LOGI(TAG, "chunk with size: %u, at time %ld.%ld",
    		   	   	   	   	 wire_chnk->size,
                             wire_chnk->timestamp.sec,
                             wire_chnk->timestamp.usec);
#endif

      if (*len == 0) {
        break;
      }
    }

    case 12: {
      size_t tmp_size;

      if ((base_message_rx->size - parser->typedMsgCurrentPos) <= *len) {
        tmp_size = base_message_rx->size - parser->typedMsgCurrentPos;
      } else {
        tmp_size = *len;
      }

      if (received_codec_header == true) {
        switch (codec) {
          case OPUS:
          case FLAC: {
            memcpy(&decoderChunk->inData[*payloadOffset], *start, tmp_size);
            *payloadOffset += tmp_size;
            decoderChunk->outData = NULL;
            decoderChunk->type = SNAPCAST_MESSAGE_WIRE_CHUNK;

            break;
          }

          case PCM: {
            size_t _tmp = tmp_size;

            *offset = 0;

            if (*pcmData == NULL) {
              if (allocate_pcm_chunk_memory(pcmData, wire_chnk->size) < 0) {
                *pcmData = NULL;
              }

              *tmpData = 0;
              *payloadDataShift = 3;
              *payloadOffset = 0;
            }

            while (_tmp--) {
              *tmpData |= ((uint32_t)(*start)[(*offset)++]
                          << (8 * *payloadDataShift));

              (*payloadDataShift)--;
              if (*payloadDataShift < 0) {
                *payloadDataShift = 3;

                if ((*pcmData) && ((*pcmData)->fragment->payload)) {
                  volatile uint32_t *sample;
                  uint8_t dummy1;
                  uint32_t dummy2 = 0;

                  // TODO: find a more
                  // clever way to do this,
                  // best would be to
                  // actually store it the
                  // right way in the first
                  // place
                  dummy1 = *tmpData >> 24;
                  dummy2 |= (uint32_t)dummy1 << 16;
                  dummy1 = *tmpData >> 16;
                  dummy2 |= (uint32_t)dummy1 << 24;
                  dummy1 = *tmpData >> 8;
                  dummy2 |= (uint32_t)dummy1 << 0;
                  dummy1 = *tmpData >> 0;
                  dummy2 |= (uint32_t)dummy1 << 8;
                  *tmpData = dummy2;

                  sample = (volatile uint32_t *)(&(
                      (*pcmData)->fragment
                          ->payload[*payloadOffset]));
                  *sample = (volatile uint32_t)*tmpData;

                  *payloadOffset += 4;
                }

                *tmpData = 0;
              }
            }

            break;
          }

          default: {
            ESP_LOGE(TAG, "Decoder (1) not supported");

            return PARSER_CRITICAL_ERROR;

            break;
          }
        }
      }

      parser->typedMsgCurrentPos += tmp_size;
      *start += tmp_size;
      // currentPos += tmp_size;
      *len -= tmp_size;

      if (parser->typedMsgCurrentPos >= base_message_rx->size) {
        if (received_codec_header == true) {
          parser_reset_state(parser);
          return PARSER_COMPLETE;
        }

        parser_reset_state(parser);
      }

      break;
    }

    default: {
      ESP_LOGE(TAG,
               "wire chunk decoder "
               "shouldn't get here");

      break;
    }
  }
  return PARSER_INCOMPLETE;
}

parser_return_state_t parse_codec_header_message(snapcast_custom_parser_t* parser,
                                                 char** start,
                                                 uint16_t* len,
                                                 uint32_t* typedMsgLen,
                                                 uint32_t* offset,
                                                 bool* received_codec_header,
                                                 char** codecString,
                                                 codec_type_t* codec,
                                                 char** codecPayload) {
switch (parser->internalState) {
    case 0: {
      *received_codec_header = false;

      *typedMsgLen = **start & 0xFF;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 1: {
      *typedMsgLen |= (**start & 0xFF) << 8;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 2: {
      *typedMsgLen |= (**start & 0xFF) << 16;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 3: {
      *typedMsgLen |= (**start & 0xFF) << 24;

      if (*codecString) {
        free(*codecString);
        *codecString = NULL;
      }

      *codecString =
          malloc(*typedMsgLen + 1);  // allocate memory for
                                     // codec string
      if (*codecString == NULL) {
        ESP_LOGE(TAG,
                 "couldn't get memory "
                 "for codec string");

        return PARSER_CRITICAL_ERROR;
      }

      *offset = 0;
      // ESP_LOGI(TAG,
      // "codec header string is %d long",
      // *typedMsgLen);

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 4: {
      if (*len >= *typedMsgLen) {
        memcpy(&(*codecString)[*offset], *start, *typedMsgLen);

        *offset += *typedMsgLen;

        parser->typedMsgCurrentPos += *typedMsgLen;
        *start += *typedMsgLen;
        // currentPos += *typedMsgLen;
        *len -= *typedMsgLen;
      } else {
        memcpy(&(*codecString)[*offset], *start, *typedMsgLen);

        *offset += *len;

        parser->typedMsgCurrentPos += *len;
        *start += *len;
        // currentPos += *len;
        *len -= *len;
      }

      if (*offset == *typedMsgLen) {
        // NULL terminate string
        (*codecString)[*typedMsgLen] = 0;

        // ESP_LOGI (TAG, "got codec string: %s", tmp);

        if (strcmp(*codecString, "opus") == 0) {
          *codec = OPUS;
        } else if (strcmp(*codecString, "flac") == 0) {
          *codec = FLAC;
        } else if (strcmp(*codecString, "pcm") == 0) {
          *codec = PCM;
        } else {
          *codec = NONE;

          ESP_LOGI(TAG, "Codec : %s not supported",
                   *codecString);
          ESP_LOGI(TAG,
                   "Change encoder codec to "
                   "opus, flac or pcm in "
                   "/etc/snapserver.conf on "
                   "server");

          return PARSER_CRITICAL_ERROR;
        }

        free(*codecString);
        *codecString = NULL;

        parser->internalState++;
      }

      if (*len == 0) {
        break;
      }
    }

    case 5: {
      *typedMsgLen = **start & 0xFF;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 6: {
      *typedMsgLen |= (**start & 0xFF) << 8;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 7: {
      *typedMsgLen |= (**start & 0xFF) << 16;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 8: {
      *typedMsgLen |= (**start & 0xFF) << 24;

      if (*codecPayload) {
        free(*codecPayload);
        *codecPayload = NULL;
      }

      *codecPayload = malloc(*typedMsgLen);  // allocate memory
                                           // for codec payload
      if (*codecPayload == NULL) {
        ESP_LOGE(TAG,
                 "couldn't get memory "
                 "for codec payload");

        return PARSER_CRITICAL_ERROR;
      }

      *offset = 0;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 9: {
      if (*len >= *typedMsgLen) {
        memcpy(&(*codecPayload)[*offset], *start, *typedMsgLen);

        *offset += *typedMsgLen;

        parser->typedMsgCurrentPos += *typedMsgLen;
        *start += *typedMsgLen;
        // currentPos += *typedMsgLen;
        *len -= *typedMsgLen;
      } else {
        memcpy(&(*codecPayload)[*offset], *start, *len);

        *offset += *len;

        parser->typedMsgCurrentPos += *len;
        *start += *len;
        // currentPos += *len;
        *len -= *len;
      }

      if (*offset == *typedMsgLen) {
        *received_codec_header = true;

        // parser->typedMsgCurrentPos previously wasn't reset here, but this can be changed without different behavior
        // because the next base message will reset the parser state anyway.
        parser_reset_state(parser);

        // Handle codec header payload
        return PARSER_COMPLETE;
      }

      break;
    }

    default: {
      ESP_LOGE(TAG,
               "codec header decoder "
               "shouldn't get here");

      break;
    }
  }
  return PARSER_INCOMPLETE;
}

parser_return_state_t parse_sever_settings_message(snapcast_custom_parser_t *parser,
                                                   base_message_t* base_message_rx,
                                                   char** start,
                                                   uint16_t* len,
                                                   uint32_t* typedMsgLen,
                                                   uint32_t* offset,
                                                   char** serverSettingsString) {
  switch (parser->internalState) {
    case 0: {
      *typedMsgLen = **start & 0xFF;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 1: {
      *typedMsgLen |= (**start & 0xFF) << 8;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 2: {
      *typedMsgLen |= (**start & 0xFF) << 16;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 3: {
      *typedMsgLen |= (**start & 0xFF) << 24;

      // ESP_LOGI(TAG,"server settings string is %lu"
      //              " long", *typedMsgLen);

      // now get some memory for server settings
      // string
      *serverSettingsString = malloc(*typedMsgLen + 1);
      if (*serverSettingsString == NULL) {
        ESP_LOGE(TAG,
                 "couldn't get memory for "
                 "server settings string");
      }

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      *offset = 0;

      if (*len == 0) {
        break;
      }
    }

    case 4: {
      size_t tmpSize =
          base_message_rx->size - parser->typedMsgCurrentPos;

      if (*len > 0) {
        if (tmpSize < *len) {
          if (*serverSettingsString) {
            memcpy(&(*serverSettingsString)[*offset], *start,
                   tmpSize);
          }
          *offset += tmpSize;

          *start += tmpSize;
          // currentPos += tmpSize;  // will be
          //  incremented by 1
          //  later so -1 here
          parser->typedMsgCurrentPos += tmpSize;
          *len -= tmpSize;
        } else {
          if (*serverSettingsString) {
            memcpy(&(*serverSettingsString)[*offset], *start, *len);
          }
          *offset += *len;

          *start += *len;
          // currentPos += *len;  // will be incremented
          //  by 1 later so -1
          //  here
          parser->typedMsgCurrentPos += *len;
          *len = 0;
        }
      }

      if (parser->typedMsgCurrentPos >= base_message_rx->size) {
        if (*serverSettingsString) {
          // ESP_LOGI(TAG, "done server settings %lu/%lu",
          //								*offset,
          //								*typedMsgLen);

          // NULL terminate string
          (*serverSettingsString)[*typedMsgLen] = 0;

          // ESP_LOGI(TAG, "got string: %s",
          // *serverSettingsString);

          parser_reset_state(parser);
          return PARSER_COMPLETE; // do callback

        }

        parser_reset_state(parser);
      }

      break;
    }

    default: {
      ESP_LOGE(TAG,
               "server settings decoder "
               "shouldn't get here");

      break;
    }
  }
  return PARSER_INCOMPLETE;
}


//// Leftover from moved code, still needed?
//                case SNAPCAST_MESSAGE_STREAM_TAGS: {
//                  size_t tmpSize = base_message_rx.size -
//                  parser.typedMsgCurrentPos;
//
//                  if (tmpSize < len) {
//                    start += tmpSize;
//                    // currentPos += tmpSize;
//                    parser.typedMsgCurrentPos += tmpSize;
//                    len -= tmpSize;
//                  } else {
//                    start += len;
//                    // currentPos += len;
//
//                    parser.typedMsgCurrentPos += len;
//                    len = 0;
//                  }
//
//                  if (typedMsgCurrentPos >=
//                  base_message_rx.size) {
//                    // ESP_LOGI(TAG,
//                    // "done stream tags with length %d %d
//                    %d",
//                    // base_message_rx.size, currentPos,
//                    // tmpSize);
//
//                    parser.typedMsgCurrentPos = 0;
//                    // currentPos = 0;
//
//                    parser.state = BASE_MESSAGE_STATE;
//                    parser.internalState = 0;
//                  }
//
//                  break;
//                }


parser_return_state_t parse_time_message(snapcast_custom_parser_t* parser,
                                         base_message_t* base_message_rx,
                                         time_message_t* time_message_rx,
                                         char** start, uint16_t* len) {
  switch (parser->internalState) {
    case 0: {
      time_message_rx->latency.sec = **start;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 1: {
      time_message_rx->latency.sec |= (int32_t)**start << 8;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 2: {
      time_message_rx->latency.sec |= (int32_t)**start << 16;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 3: {
      time_message_rx->latency.sec |= (int32_t)**start << 24;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 4: {
      time_message_rx->latency.usec = **start;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 5: {
      time_message_rx->latency.usec |= (int32_t)**start << 8;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 6: {
      time_message_rx->latency.usec |= (int32_t)**start << 16;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;

      parser->internalState++;

      if (*len == 0) {
        break;
      }
    }

    case 7: {
      time_message_rx->latency.usec |= (int32_t)**start << 24;

      parser->typedMsgCurrentPos++;
      (*start)++;
      // currentPos++;
      (*len)--;
      if (parser->typedMsgCurrentPos >= base_message_rx->size) {
        // ESP_LOGI(TAG, "done time message");

        parser_reset_state(parser);

        return PARSER_COMPLETE; // do callback

      } else {
        ESP_LOGE(TAG,
                 "error time message, this "
                 "shouldn't happen! %d %ld",
                 parser->typedMsgCurrentPos, base_message_rx->size);

        parser_reset_state(parser);

      }

      break;
    }

    default: {
      ESP_LOGE(TAG,
               "time message decoder shouldn't "
               "get here %d %ld %ld",
               parser->typedMsgCurrentPos, base_message_rx->size,
               parser->internalState);

      break;
    }
  }
  return PARSER_INCOMPLETE; // no callback
}

void parse_unknown_message(snapcast_custom_parser_t* parser,
                           base_message_t* base_message_rx,
                           char** start,
                           uint16_t* len) {
  parser->typedMsgCurrentPos++;
  (*start)++;
  // currentPos++;
  (*len)--;

  if (parser->typedMsgCurrentPos >= base_message_rx->size) {
    ESP_LOGI(TAG, "done unknown typed message %d",
             base_message_rx->type);

    parser_reset_state(parser);

  }
}
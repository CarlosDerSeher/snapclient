/* Play flac file by audio pipeline
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
#include "eth_interface.h"
#endif

#include "nvs_flash.h"
#include "wifi_interface.h"

// Minimum ESP-IDF stuff only hardware abstraction stuff
#include <wifi_provisioning.h>

#include "board.h"
#include "es8388.h"
#include "esp_netif.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "net_functions.h"

// Web socket server
// #include "websocket_if.h"
// #include "websocket_server.h"

#include <sys/time.h>

#include "driver/i2s_std.h"
#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#endif

// Opus decoder is implemented as a subcomponet from master git repo
#include "opus.h"

// flac decoder is implemented as a subcomponet from master git repo
#include "FLAC/stream_decoder.h"
#include "ota_server.h"
#include "player.h"
#include "snapcast.h"
#include "ui_http_server.h"

static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data);
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data);
static void metadata_callback(const FLAC__StreamDecoder *decoder,
                              const FLAC__StreamMetadata *metadata,
                              void *client_data);
static void error_callback(const FLAC__StreamDecoder *decoder,
                           FLAC__StreamDecoderErrorStatus status,
                           void *client_data);

// #include "ma120.h"

static FLAC__StreamDecoder *flacDecoder = NULL;
static QueueHandle_t decoderReadQHdl = NULL;
static QueueHandle_t decoderWriteQHdl = NULL;
static QueueHandle_t decoderTaskQHdl = NULL;
SemaphoreHandle_t decoderReadSemaphore = NULL;
SemaphoreHandle_t decoderWriteSemaphore = NULL;

const char *VERSION_STRING = "0.0.3";

#define HTTP_TASK_PRIORITY (configMAX_PRIORITIES - 2)  // 9
#define HTTP_TASK_CORE_ID 1                            // 1  // tskNO_AFFINITY

#define OTA_TASK_PRIORITY 6
#define OTA_TASK_CORE_ID tskNO_AFFINITY
// 1  // tskNO_AFFINITY

#define FLAC_DECODER_TASK_PRIORITY 7
#define FLAC_DECODER_TASK_CORE_ID tskNO_AFFINITY
// HTTP_TASK_CORE_ID  // 1  // tskNO_AFFINITY

#define FLAC_TASK_PRIORITY 8
#define FLAC_TASK_CORE_ID tskNO_AFFINITY

#define OPUS_TASK_PRIORITY 8
#define OPUS_TASK_CORE_ID tskNO_AFFINITY

// 1  // tskNO_AFFINITY

TaskHandle_t t_ota_task = NULL;
TaskHandle_t t_http_get_task = NULL;
TaskHandle_t t_flac_decoder_task = NULL;
TaskHandle_t dec_task_handle = NULL;

#define FAST_SYNC_LATENCY_BUF 10000      // in µs
#define NORMAL_SYNC_LATENCY_BUF 1000000  // in µs

struct timeval tdif, tavg;
static audio_board_handle_t board_handle = NULL;

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_SERVER_USE_MDNS CONFIG_SNAPSERVER_USE_MDNS
#if !SNAPCAST_SERVER_USE_MDNS
#define SNAPCAST_SERVER_HOST CONFIG_SNAPSERVER_HOST
#define SNAPCAST_SERVER_PORT CONFIG_SNAPSERVER_PORT
#endif
#define SNAPCAST_CLIENT_NAME CONFIG_SNAPCLIENT_NAME
#define SNAPCAST_USE_SOFT_VOL CONFIG_SNAPCLIENT_USE_SOFT_VOL

/* Logging tag */
static const char *TAG = "SC";

// static QueueHandle_t playerChunkQueueHandle = NULL;
SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

#if CONFIG_USE_DSP_PROCESSOR
#if CONFIG_SNAPCLIENT_DSP_FLOW_STEREO
dspFlows_t dspFlow = dspfStereo;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASSBOOST
dspFlows_t dspFlow = dspfBassBoost;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BIAMP
dspFlows_t dspFlow = dspfBiamp;
#endif
#if CONFIG_SNAPCLIENT_DSP_FLOW_BASS_TREBLE_EQ
dspFlows_t dspFlow = dspfEQBassTreble;
#endif
#endif

typedef struct decoderData_s {
  uint32_t type;  // should be SNAPCAST_MESSAGE_CODEC_HEADER
                  // or SNAPCAST_MESSAGE_WIRE_CHUNK
  uint8_t *inData;
  tv_t timestamp;
  pcm_chunk_message_t *outData;
  uint32_t bytes;
} decoderData_t;

void time_sync_msg_cb(void *args);

static char base_message_serialized[BASE_MESSAGE_SIZE];
static char time_message_serialized[TIME_MESSAGE_SIZE];
static const esp_timer_create_args_t tSyncArgs = {
    .callback = &time_sync_msg_cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "tSyncMsg",
    .skip_unhandled_events = false};

struct netconn *lwipNetconn;

static int id_counter = 0;

static OpusDecoder *opusDecoder = NULL;

/**
 *
 */
void time_sync_msg_cb(void *args) {
  base_message_t base_message_tx;
  //  struct timeval now;
  int64_t now;
  // time_message_t time_message_tx = {{0, 0}};
  int rc1;

  // causes kernel panic, which shouldn't happen though?
  // Isn't it called from timer task instead of ISR?
  // xSemaphoreGive(timeSyncSemaphoreHandle);

  //  result = gettimeofday(&now, NULL);
  ////  ESP_LOGI(TAG, "time of day: %d", (int32_t)now.tv_sec +
  ///(int32_t)now.tv_usec);
  //  if (result) {
  //    ESP_LOGI(TAG, "Failed to gettimeofday");
  //
  //    return;
  //  }

  uint8_t *p_pkt = (uint8_t *)malloc(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);
  if (p_pkt == NULL) {
    ESP_LOGW(
        TAG,
        "%s: Failed to get memory for time sync message. Skipping this round.",
        __func__);

    return;
  }

  memset(p_pkt, 0, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);

  base_message_tx.type = SNAPCAST_MESSAGE_TIME;
  base_message_tx.id = id_counter++;
  base_message_tx.refersTo = 0;
  base_message_tx.received.sec = 0;
  base_message_tx.received.usec = 0;
  now = esp_timer_get_time();
  base_message_tx.sent.sec = now / 1000000;
  base_message_tx.sent.usec = now - base_message_tx.sent.sec * 1000000;
  base_message_tx.size = TIME_MESSAGE_SIZE;
  rc1 = base_message_serialize(&base_message_tx, (char *)&p_pkt[0],
                               BASE_MESSAGE_SIZE);
  if (rc1) {
    ESP_LOGE(TAG, "Failed to serialize base message for time");

    return;
  }

  //  memset(&time_message_tx, 0, sizeof(time_message_tx));
  //  result = time_message_serialize(&time_message_tx,
  //  &p_pkt[BASE_MESSAGE_SIZE],
  //                                  TIME_MESSAGE_SIZE);
  //  if (result) {
  //    ESP_LOGI(TAG, "Failed to serialize time message");
  //
  //    return;
  //  }

  rc1 = netconn_write(lwipNetconn, p_pkt, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE,
                      NETCONN_NOCOPY);
  if (rc1 != ERR_OK) {
    ESP_LOGW(TAG, "error writing timesync msg");

    return;
  }

  free(p_pkt);

  //  ESP_LOGI(TAG, "%s: sent time sync message", __func__);

  //  xSemaphoreGiveFromISR(timeSyncSemaphoreHandle, &xHigherPriorityTaskWoken);
  //  if (xHigherPriorityTaskWoken) {
  //    portYIELD_FROM_ISR();
  //  }
}

/**
 *
 */
void free_flac_data(decoderData_t *pFlacData) {
  if (pFlacData->inData) {
    free(pFlacData->inData);
    pFlacData->inData = NULL;
  }

  if (pFlacData->outData) {
    free(pFlacData->outData);
    pFlacData->outData = NULL;
  }

  if (pFlacData) {
    free(pFlacData);
    pFlacData = NULL;
  }
}

/**
 *
 */
static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data) {
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  decoderData_t *flacData;

  (void)scSet;

  xQueueReceive(decoderReadQHdl, &flacData, portMAX_DELAY);

  // ESP_LOGI(TAG, "in flac read cb %d %p", flacData->bytes, flacData->inData);

  if (flacData->bytes <= 0) {
    free_flac_data(flacData);

    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }

  if (flacData->inData == NULL) {
    free_flac_data(flacData);

    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }

  if (flacData->bytes <= *bytes) {
    memcpy(buffer, flacData->inData, flacData->bytes);
    *bytes = flacData->bytes;
    //      ESP_LOGW(TAG, "read all flac inData %d", *bytes);
  } else {
    memcpy(buffer, flacData->inData, *bytes);
    // ESP_LOGW(TAG, "dind't read all flac inData %d", *bytes);
    flacData->inData += *bytes;
    flacData->bytes -= *bytes;
  }

  free_flac_data(flacData);

  // xQueueSend (flacReadQHdl, &flacData, portMAX_DELAY);

  // xSemaphoreGive(decoderReadSemaphore);

  // ESP_LOGE(TAG, "%s: data processed", __func__);

  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

/**
 *
 */
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data) {
  size_t i;
  decoderData_t *flacData = NULL;  // = &flacOutData;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  int ret = 0;
  uint32_t fragmentCnt = 0;

  (void)decoder;

  // xSemaphoreTake(decoderWriteSemaphore, portMAX_DELAY);

  // xQueueReceive (flacReadQHdl, &flacData, portMAX_DELAY);

  //    ESP_LOGI(TAG, "in flac write cb %d %p", frame->header.blocksize,
  //    flacData);

  if (frame->header.channels != scSet->ch) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different channel count %ld than "
             "previous metadata block %d",
             frame->header.channels, scSet->ch);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (frame->header.bits_per_sample != scSet->bits) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different bps %ld than previous "
             "metadata block %d",
             frame->header.bits_per_sample, scSet->bits);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[0] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [0] is NULL\n");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[1] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [1] is NULL\n");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  flacData = (decoderData_t *)malloc(sizeof(decoderData_t));
  if (flacData == NULL) {
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  memset(flacData, 0, sizeof(decoderData_t));

  flacData->bytes = frame->header.blocksize * frame->header.channels *
                    (frame->header.bits_per_sample / 8);

  ret = allocate_pcm_chunk_memory(&(flacData->outData), flacData->bytes);

  //   ESP_LOGI (TAG, "mem %p %p %d", flacData->outData,
  //   flacData->outData->fragment->payload, flacData->bytes);

  if (ret == 0) {
    pcm_chunk_fragment_t *fragment = flacData->outData->fragment;

    if (fragment->payload != NULL) {
      fragmentCnt = 0;

      for (i = 0; i < frame->header.blocksize; i++) {
        // write little endian
        // flacData->outData[4 * i] = (uint8_t)buffer[0][i];
        // flacData->outData[4 * i + 1] = (uint8_t) (buffer[0][i] >> 8);
        // flacData->outData[4 * i + 2] = (uint8_t)buffer[1][i];
        // flacData->outData[4 * i + 3] = (uint8_t)(buffer[1][i] >> 8);

        // TODO: for now fragmented payload is not supported and the whole
        // chunk is expected to be in the first fragment
        uint32_t tmpData;
        tmpData = ((uint32_t)((buffer[0][i] >> 8) & 0xFF) << 24) |
                  ((uint32_t)((buffer[0][i] >> 0) & 0xFF) << 16) |
                  ((uint32_t)((buffer[1][i] >> 8) & 0xFF) << 8) |
                  ((uint32_t)((buffer[1][i] >> 0) & 0xFF) << 0);

        if (fragment != NULL) {
          volatile uint32_t *test =
              (volatile uint32_t *)(&(fragment->payload[fragmentCnt]));
          *test = (volatile uint32_t)tmpData;
        }

        fragmentCnt += 4;
        if (fragmentCnt >= fragment->size) {
          fragmentCnt = 0;

          fragment = fragment->nextFragment;
        }
      }
    }
  }
  //  else {
  //    flacData->outData = NULL;
  //  }

  xQueueSend(decoderWriteQHdl, &flacData, portMAX_DELAY);

  // ESP_LOGE(TAG, "%s: data processed", __func__);

  // xSemaphoreGive(flacWriteSemaphore);

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 *
 */
void metadata_callback(const FLAC__StreamDecoder *decoder,
                       const FLAC__StreamMetadata *metadata,
                       void *client_data) {
  decoderData_t *flacData;  // = &flacOutData;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  (void)decoder;

  // xQueueReceive (flacReadQHdl, &flacData, portMAX_DELAY);

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    //		ESP_LOGI(TAG, "in flac meta cb");

    flacData = (decoderData_t *)malloc(sizeof(decoderData_t));
    if (flacData == NULL) {
      ESP_LOGE(TAG, "in flac meta cb, malloc failed");

      return;
    }

    memset(flacData, 0, sizeof(decoderData_t));

    // save for later
    scSet->sr = metadata->data.stream_info.sample_rate;
    scSet->ch = metadata->data.stream_info.channels;
    scSet->bits = metadata->data.stream_info.bits_per_sample;

    ESP_LOGI(TAG, "fLaC sampleformat: %ld:%d:%d", scSet->sr, scSet->bits,
             scSet->ch);

    xQueueSend(decoderWriteQHdl, &flacData, portMAX_DELAY);

    // ESP_LOGE(TAG, "%s: data processed", __func__);
  }

  //  xSemaphoreGive(flacReadSemaphore);
}

/**
 *
 */
void error_callback(const FLAC__StreamDecoder *decoder,
                    FLAC__StreamDecoderErrorStatus status, void *client_data) {
  (void)decoder, (void)client_data;

  ESP_LOGE(TAG, "Got error callback: %s\n",
           FLAC__StreamDecoderErrorStatusString[status]);
}

static void flac_decoder_task(void *pvParameters) {
  //  FLAC__bool ok = true;
  FLAC__StreamDecoderInitStatus init_status;
  snapcastSetting_t *scSet = (snapcastSetting_t *)pvParameters;

  if (flacDecoder != NULL) {
    FLAC__stream_decoder_finish(flacDecoder);
    FLAC__stream_decoder_delete(flacDecoder);
    flacDecoder = NULL;
  }

  flacDecoder = FLAC__stream_decoder_new();
  if (flacDecoder == NULL) {
    ESP_LOGE(TAG, "Failed to init flac decoder");
    return;
  }

  init_status = FLAC__stream_decoder_init_stream(
      flacDecoder, read_callback, NULL, NULL, NULL, NULL, write_callback,
      metadata_callback, error_callback, scSet);
  if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
    ESP_LOGE(TAG, "ERROR: initializing decoder: %s\n",
             FLAC__StreamDecoderInitStatusString[init_status]);

    //    ok = false;

    return;
  }

  while (1) {
    FLAC__stream_decoder_process_until_end_of_stream(flacDecoder);
  }
}

/**
 *
 */
void flac_task(void *pvParameters) {
  tv_t currentTimestamp;
  decoderData_t *pFlacData = NULL;
  snapcastSetting_t *scSet = (snapcastSetting_t *)pvParameters;

  while (1) {
    xQueueReceive(decoderTaskQHdl, &pFlacData,
                  portMAX_DELAY);  // get data from tcp task

    if (pFlacData != NULL) {
      currentTimestamp = pFlacData->timestamp;

      //      ESP_LOGE(TAG, "Got timestamp %lld",
      //               (uint64_t)currentTimestamp.sec * 1000000 +
      //                   (uint64_t)currentTimestamp.usec);

      // xSemaphoreTake(decoderReadSemaphore, portMAX_DELAY);

      // send data to flac decoder
      // ESP_LOGE(TAG, "%s: decoderReadQHdl start", __func__);

      xQueueSend(decoderReadQHdl, &pFlacData, portMAX_DELAY);

      // ESP_LOGE(TAG, "%s: decoderReadQHdl done", __func__);
      // and wait until data was
      // processed
      // xSemaphoreTake(decoderReadSemaphore, portMAX_DELAY);
      // need to release mutex
      // afterwards for next round
      // xSemaphoreGive(decoderReadSemaphore);

      //      free(pFlacData->inData);
      //      free(pFlacData);
    } else {
      pcm_chunk_message_t *pcmData = NULL;

      //      xSemaphoreGive(decoderWriteSemaphore);
      // and wait until it is done
      // ESP_LOGE(TAG, "%s: decoderWriteQHdl start", __func__);

      xQueueReceive(decoderWriteQHdl, &pFlacData, portMAX_DELAY);

      // ESP_LOGE(TAG, "%s: decoderWriteQHdl done", __func__);

      if (pFlacData->outData != NULL) {
        pcmData = pFlacData->outData;
        pcmData->timestamp = currentTimestamp;

        size_t decodedSize = pcmData->totalSize;  // pFlacData->bytes;
        scSet->chkInFrames =
            decodedSize / ((size_t)scSet->ch * (size_t)(scSet->bits / 8));
        if (player_send_snapcast_setting(scSet) != pdPASS) {
          ESP_LOGE(TAG,
                   "Failed to "
                   "notify "
                   "sync task "
                   "about "
                   "codec. Did you "
                   "init player?");

          return;
        }

#if CONFIG_USE_DSP_PROCESSOR
        dsp_processor_worker(pcmData->fragment->payload,
                             pcmData->fragment->size, scSet->sr);
#endif

        insert_pcm_chunk(pcmData);

        if (pFlacData->inData) {
          free(pFlacData->inData);
          pFlacData->inData = NULL;
        }

        if (pFlacData) {
          free(pFlacData);
          pFlacData = NULL;
        }
      } else {
        free_flac_data(pFlacData);
      }
    }
  }
}

/**
 *
 */
void opus_decoder_task(void *pvParameters) {
  tv_t currentTimestamp;
  decoderData_t *pOpusData = NULL;
  snapcastSetting_t *scSet = (snapcastSetting_t *)pvParameters;

  while (1) {
    // get data from tcp task
    xQueueReceive(decoderTaskQHdl, &pOpusData, portMAX_DELAY);

    if (pOpusData) {
      currentTimestamp = pOpusData->timestamp;

      // ESP_LOGE(TAG, "%s: Got timestamp %lld", __func__,
      //                                        (uint64_t)currentTimestamp.sec *
      //                                        1000000 +
      //                                        (uint64_t)currentTimestamp.usec);

      if (pOpusData->inData) {
        int frame_size = 0;
        int sample_count = 0;
        int samples_per_frame = 0;
        int frame_count;
        opus_int16 *audio;

        samples_per_frame =
            opus_packet_get_samples_per_frame(pOpusData->inData, scSet->sr);
        if (samples_per_frame < 0) {
          ESP_LOGE(TAG,
                   "couldn't get samples per frame count "
                   "of packet");
        }

        scSet->chkInFrames = samples_per_frame;

        size_t bytes = samples_per_frame * scSet->ch * scSet->bits / 8;

        if (samples_per_frame > 480) {
          ESP_LOGE(TAG, "samples_per_frame: %d, pOpusData->bytes %ld, bytes %u",
                   samples_per_frame, pOpusData->bytes, bytes);
        }

        // TODO: insert some break condition if we wait
        // too long
        while ((audio = (opus_int16 *)malloc(bytes)) == NULL) {
          ESP_LOGE(TAG, "couldn't get memory for audio");

          vTaskDelay(pdMS_TO_TICKS(1));
        }

        frame_size =
            opus_decode(opusDecoder, pOpusData->inData, pOpusData->bytes,
                        (opus_int16 *)audio, samples_per_frame, 0);

        free(pOpusData->inData);
        pOpusData->inData = NULL;

        if (frame_size < 0) {
          ESP_LOGE(TAG, "Decode error : %d \n", frame_size);
        } else {
          pcm_chunk_message_t *pcmData = NULL;

          bytes = frame_size * scSet->ch * scSet->bits / 8;
          if (allocate_pcm_chunk_memory(&pcmData, bytes) < 0) {
            pcmData = NULL;
          } else {
            pcmData->timestamp = currentTimestamp;

            if (pcmData->fragment->payload) {
              volatile uint32_t *sample;
              uint32_t tmpData;
              uint32_t cnt = 0;

              for (int i = 0; i < bytes; i += 4) {
                sample =
                    (volatile uint32_t *)(&(pcmData->fragment->payload[i]));
                tmpData = (((uint32_t)audio[cnt] << 16) & 0xFFFF0000) |
                          (((uint32_t)audio[cnt + 1] << 0) & 0x0000FFFF);
                *sample = (volatile uint32_t)tmpData;

                cnt += 2;
              }
            }

            free(audio);
            audio = NULL;
          }

          if (player_send_snapcast_setting(scSet) != pdPASS) {
            ESP_LOGE(TAG,
                     "Failed to notify "
                     "sync task about "
                     "codec. Did you "
                     "init player?");

            return;
          }

#if CONFIG_USE_DSP_PROCESSOR
          dsp_processor_worker(pcmData->fragment->payload,
                               pcmData->fragment->size, scSet->sr);
#endif

          insert_pcm_chunk(pcmData);
        }
      }

      free(pOpusData);
      pOpusData = NULL;
    }
  }
}

/**
 *
 */
esp_err_t audio_set_mute(bool mute) {
  if (!board_handle) {
    ESP_LOGW(TAG, "audio board not initialized yet");

    return ESP_OK;
  } else {
    return audio_hal_set_mute(board_handle->audio_hal, mute);
  }
}

/**
 *
 */
static void http_get_task(void *pvParameters) {
  char *start;
  base_message_t base_message_rx;
  hello_message_t hello_message;
  wire_chunk_message_t wire_chnk = {{0, 0}, 0, NULL};
  char *hello_message_serialized = NULL;
  int result;
  int64_t now, trx, tdif, ttx;
  time_message_t time_message_rx = {{0, 0}};
  int64_t tmpDiffToServer;
  int64_t lastTimeSync = 0;
  esp_timer_handle_t timeSyncMessageTimer = NULL;
  esp_err_t err = 0;
  server_settings_message_t server_settings_message;
  bool received_header = false;
  mdns_result_t *r;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  // flacData_t flacData = {SNAPCAST_MESSAGE_CODEC_HEADER, NULL, {0, 0}, NULL,
  // 0};
  decoderData_t *pDecData = NULL;
  pcm_chunk_message_t *pcmData = NULL;
  uint8_t *opusData = NULL;
  ip_addr_t remote_ip;
  uint16_t remotePort = 0;
  int rc1 = ERR_OK, rc2 = ERR_OK;
  struct netbuf *firstNetBuf = NULL;
  struct netbuf *newNetBuf = NULL;
  uint16_t len;
  uint64_t timeout = FAST_SYNC_LATENCY_BUF;

  // create a timer to send time sync messages every x µs
  esp_timer_create(&tSyncArgs, &timeSyncMessageTimer);

#if CONFIG_SNAPCLIENT_USE_MDNS
  ESP_LOGI(TAG, "Enable mdns");
  mdns_init();
#endif

  while (1) {
    received_header = false;

    if (reset_latency_buffer() < 0) {
      ESP_LOGE(TAG,
               "reset_diff_buffer: couldn't reset median filter long. STOP");
      return;
    }

    timeout = FAST_SYNC_LATENCY_BUF;

    esp_timer_stop(timeSyncMessageTimer);

    if (opusDecoder != NULL) {
      opus_decoder_destroy(opusDecoder);
      opusDecoder = NULL;
    }

    if (t_flac_decoder_task != NULL) {
      vTaskDelete(t_flac_decoder_task);
      t_flac_decoder_task = NULL;
    }

    if (dec_task_handle != NULL) {
      vTaskDelete(dec_task_handle);
      dec_task_handle = NULL;
    }

    if (flacDecoder != NULL) {
      FLAC__stream_decoder_finish(flacDecoder);
      FLAC__stream_decoder_delete(flacDecoder);
      flacDecoder = NULL;
    }

    if (decoderWriteQHdl != NULL) {
      vQueueDelete(decoderWriteQHdl);
      decoderWriteQHdl = NULL;
    }

    if (decoderReadQHdl != NULL) {
      vQueueDelete(decoderReadQHdl);
      decoderReadQHdl = NULL;
    }

    if (decoderTaskQHdl != NULL) {
      vQueueDelete(decoderTaskQHdl);
      decoderTaskQHdl = NULL;
    }

#if SNAPCAST_SERVER_USE_MDNS
    // Find snapcast server
    // Connect to first snapcast server found
    r = NULL;
    err = 0;
    while (!r || err) {
      ESP_LOGI(TAG, "Lookup snapcast service on network");
      esp_err_t err = mdns_query_ptr("_snapcast", "_tcp", 3000, 20, &r);
      if (err) {
        ESP_LOGE(TAG, "Query Failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }

      if (!r) {
        ESP_LOGW(TAG, "No results found!");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    if (r->addr) {
      ip_addr_copy(remote_ip, (r->addr->addr));
      remote_ip.type = IPADDR_TYPE_V4;
      remotePort = r->port;
      ESP_LOGI(TAG, "Found %s:%d", ipaddr_ntoa(&remote_ip), remotePort);

      mdns_query_results_free(r);
    } else {
      mdns_query_results_free(r);

      ESP_LOGW(TAG, "No IP found in MDNS query");

      continue;
    }
#else
    // configure a failsafe snapserver according to CONFIG values
    struct sockaddr_in servaddr;

    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET, SNAPCAST_SERVER_HOST, &(servaddr.sin_addr.s_addr));
    servaddr.sin_port = htons(SNAPCAST_SERVER_PORT);

    inet_pton(AF_INET, SNAPCAST_SERVER_HOST, &(remote_ip.u_addr.ip4.addr));
    remote_ip.type = IPADDR_TYPE_V4;
    remotePort = SNAPCAST_SERVER_PORT;

    ESP_LOGI(TAG, "try connecting to static configuration %s:%d",
             ipaddr_ntoa(&remote_ip), remotePort);
#endif

    if (lwipNetconn != NULL) {
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;
    }

    lwipNetconn = netconn_new(NETCONN_TCP);
    if (lwipNetconn == NULL) {
      ESP_LOGE(TAG, "can't create netconn");

      continue;
    }

    rc1 = netconn_bind(lwipNetconn, IPADDR_ANY, 0);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "can't bind local IP");
    }

    rc2 = netconn_connect(lwipNetconn, &remote_ip, remotePort);
    if (rc2 != ERR_OK) {
      ESP_LOGE(TAG, "can't connect to remote %s:%d, err %d",
               ipaddr_ntoa(&remote_ip), remotePort, rc2);
    }
    if (rc1 != ERR_OK || rc2 != ERR_OK) {
      netconn_close(lwipNetconn);
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;

      continue;
    }

    ESP_LOGI(TAG, "netconn connected");

    char mac_address[18];
    uint8_t base_mac[6];
    // Get MAC address for WiFi station
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    esp_read_mac(base_mac, ESP_MAC_ETH);
#else
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
#endif
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
            base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);

    now = esp_timer_get_time();

    // init base message
    base_message_rx.type = SNAPCAST_MESSAGE_HELLO;
    base_message_rx.id = 0x0000;
    base_message_rx.refersTo = 0x0000;
    base_message_rx.sent.sec = now / 1000000;
    base_message_rx.sent.usec = now - base_message_rx.sent.sec * 1000000;
    base_message_rx.received.sec = 0;
    base_message_rx.received.usec = 0;
    base_message_rx.size = 0x00000000;

    // init hello message
    hello_message.mac = mac_address;
    hello_message.hostname = SNAPCAST_CLIENT_NAME;
    hello_message.version = (char *)VERSION_STRING;
    hello_message.client_name = "libsnapcast";
    hello_message.os = "esp32";
    hello_message.arch = "xtensa";
    hello_message.instance = 1;
    hello_message.id = mac_address;
    hello_message.protocol_version = 2;

    if (hello_message_serialized == NULL) {
      hello_message_serialized = hello_message_serialize(
          &hello_message, (size_t *)&(base_message_rx.size));
      if (!hello_message_serialized) {
        ESP_LOGE(TAG, "Failed to serialize hello message");
        return;
      }
    }

    result = base_message_serialize(&base_message_rx, base_message_serialized,
                                    BASE_MESSAGE_SIZE);
    if (result) {
      ESP_LOGE(TAG, "Failed to serialize base message");
      return;
    }

    rc1 = netconn_write(lwipNetconn, base_message_serialized, BASE_MESSAGE_SIZE,
                        NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send base message");

      continue;
    }
    rc1 = netconn_write(lwipNetconn, hello_message_serialized,
                        base_message_rx.size, NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send hello message");

      continue;
    }

    ESP_LOGI(TAG, "netconn sent hello message");

    free(hello_message_serialized);
    hello_message_serialized = NULL;

    // init default setting
    scSet.buf_ms = 0;
    scSet.codec = NONE;
    scSet.bits = 0;
    scSet.ch = 0;
    scSet.sr = 0;
    scSet.chkInFrames = 0;
    scSet.volume = 0;
    scSet.muted = true;

    uint64_t startTime, endTime;
    char *p_tmp = NULL;
    int32_t remainderSize = 0;
    size_t currentPos = 0;
    size_t typedMsgCurrentPos = 0;
    uint32_t typedMsgLen = 0;
    uint32_t offset = 0;
    uint32_t payloadOffset = 0;
    uint32_t tmpData = 0;
    int32_t payloadDataShift = 0;

    int16_t pcm_size = 120;

#define BASE_MESSAGE_STATE 0
#define TYPED_MESSAGE_STATE 1

    // 0 ... base message, 1 ... typed message
    uint32_t state = BASE_MESSAGE_STATE;
    uint32_t internalState = 0;

    firstNetBuf = NULL;

#define TEST_DECODER_TASK 1

    decoderWriteSemaphore = xSemaphoreCreateMutex();
    xSemaphoreTake(decoderWriteSemaphore, portMAX_DELAY);

    decoderReadSemaphore = xSemaphoreCreateMutex();
    xSemaphoreGive(decoderReadSemaphore);  // only decoder read callback/task
                                           // can give semaphore

    while (1) {
      rc2 = netconn_recv(lwipNetconn, &firstNetBuf);
      if (rc2 != ERR_OK) {
        if (rc2 == ERR_CONN) {
          netconn_close(lwipNetconn);

          // restart and try to reconnect
          break;
        }

        if (firstNetBuf != NULL) {
          netbuf_delete(firstNetBuf);

          firstNetBuf = NULL;
        }
        continue;
      }

      // now parse the data
      netbuf_first(firstNetBuf);
      do {
        currentPos = 0;

        rc1 = netbuf_data(firstNetBuf, (void **)&start, &len);
        if (rc1 == ERR_OK) {
          // ESP_LOGI (TAG, "netconn rx,"
          // "data len: %d, %d", len, netbuf_len(firstNetBuf) -
          // currentPos);
        } else {
          ESP_LOGE(TAG, "netconn rx, couldn't get data");

          continue;
        }

        while (len > 0) {
          rc1 = ERR_OK;  // probably not necessary

          switch (state) {
            // decode base message
            case BASE_MESSAGE_STATE: {
              switch (internalState) {
                case 0:
                  base_message_rx.type = *start & 0xFF;
                  internalState++;
                  break;

                case 1:
                  base_message_rx.type |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 2:
                  base_message_rx.id = *start & 0xFF;
                  internalState++;
                  break;

                case 3:
                  base_message_rx.id |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 4:
                  base_message_rx.refersTo = *start & 0xFF;
                  internalState++;
                  break;

                case 5:
                  base_message_rx.refersTo |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 6:
                  base_message_rx.sent.sec = *start & 0xFF;
                  internalState++;
                  break;

                case 7:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 8:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 9:
                  base_message_rx.sent.sec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 10:
                  base_message_rx.sent.usec = *start & 0xFF;
                  internalState++;
                  break;

                case 11:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 12:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 13:
                  base_message_rx.sent.usec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 14:
                  base_message_rx.received.sec = *start & 0xFF;
                  internalState++;
                  break;

                case 15:
                  base_message_rx.received.sec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 16:
                  base_message_rx.received.sec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 17:
                  base_message_rx.received.sec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 18:
                  base_message_rx.received.usec = *start & 0xFF;
                  internalState++;
                  break;

                case 19:
                  base_message_rx.received.usec |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 20:
                  base_message_rx.received.usec |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 21:
                  base_message_rx.received.usec |= (*start & 0xFF) << 24;
                  internalState++;
                  break;

                case 22:
                  base_message_rx.size = *start & 0xFF;
                  internalState++;
                  break;

                case 23:
                  base_message_rx.size |= (*start & 0xFF) << 8;
                  internalState++;
                  break;

                case 24:
                  base_message_rx.size |= (*start & 0xFF) << 16;
                  internalState++;
                  break;

                case 25:
                  base_message_rx.size |= (*start & 0xFF) << 24;
                  internalState = 0;

                  now = esp_timer_get_time();

                  base_message_rx.received.sec = now / 1000000;
                  base_message_rx.received.usec =
                      now - base_message_rx.received.sec * 1000000;

                  typedMsgCurrentPos = 0;

                  //                   ESP_LOGI(TAG,"BM type %d ts %d.%d",
                  //                   base_message_rx.type,
                  //                   base_message_rx.received.sec,
                  //                   base_message_rx.received.usec);
                  //                  								ESP_LOGI(TAG,"%d
                  //                  %d.%d", base_message_rx.type,
                  //                   base_message_rx.received.sec,
                  //                   base_message_rx.received.usec);

                  state = TYPED_MESSAGE_STATE;
                  break;
              }

              currentPos++;
              len--;
              start++;

              break;
            }

            // decode typed message
            case TYPED_MESSAGE_STATE: {
              switch (base_message_rx.type) {
                case SNAPCAST_MESSAGE_WIRE_CHUNK: {
                  switch (internalState) {
                    case 0: {
                      wire_chnk.timestamp.sec = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 1: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 2: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 3: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,
                      // "wire chunk time sec: %d",
                      // wire_chnk.timestamp.sec);

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 4: {
                      wire_chnk.timestamp.usec = (*start & 0xFF);

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 5: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 6: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 7: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,
                      // "wire chunk time usec: %d",
                      // wire_chnk.timestamp.usec);

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 8: {
                      wire_chnk.size = (*start & 0xFF);

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 9: {
                      wire_chnk.size |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 10: {
                      wire_chnk.size |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 11: {
                      wire_chnk.size |= (*start & 0xFF) << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      //                      ESP_LOGI(TAG,
                      //                      "chunk with size: %d, at time"
                      //                      " %d.%d", wire_chnk.size,
                      //                      wire_chnk.timestamp.sec,
                      //                      wire_chnk.timestamp.usec);

                      break;
                    }

                    case 12: {
                      size_t tmp_size;

                      if ((base_message_rx.size - typedMsgCurrentPos) <= len) {
                        tmp_size = base_message_rx.size - typedMsgCurrentPos;
                      } else {
                        tmp_size = len;
                      }

                      //                      static double lastChunkTimestamp =
                      //                      0; double timestamp =
                      //                      ((double)wire_chnk.timestamp.sec *
                      //                      1000000.0 +
                      //                      (double)wire_chnk.timestamp.usec)
                      //                      / 1000.0;
                      //
                      //                      ESP_LOGI(TAG, "duration %lfms,
                      //                      length %d", timestamp -
                      //                      lastChunkTimestamp, tmp);
                      //
                      //                      lastChunkTimestamp = timestamp;

                      if (received_header == true) {
                        switch (codec) {
                          case OPUS: {
                            if (opusData == NULL) {
                              // TODO: insert some break condition if we wait
                              // too long
                              while ((opusData = (uint8_t *)malloc(
                                          wire_chnk.size)) == NULL) {
                                ESP_LOGE(TAG, "couldn't memory for opusData");

                                vTaskDelay(pdMS_TO_TICKS(1));
                              }

                              payloadOffset = 0;
                            }

                            memcpy(&opusData[payloadOffset], start, tmp_size);
                            payloadOffset += tmp_size;

                            //                            ESP_LOGE(TAG,"payloadOffset
                            //                            %d, wire_chnk.size
                            //                            %d", payloadOffset,
                            //                            wire_chnk.size);

                            if (payloadOffset >= wire_chnk.size) {
                              pDecData = NULL;
                              while (!pDecData) {
                                pDecData = (decoderData_t *)malloc(
                                    sizeof(decoderData_t));
                                if (!pDecData) {
                                  vTaskDelay(pdMS_TO_TICKS(1));
                                }
                              }

                              // store timestamp for
                              // later use
                              pDecData->timestamp = wire_chnk.timestamp;
                              pDecData->inData = opusData;
                              pDecData->bytes = wire_chnk.size;
                              pDecData->outData = NULL;
                              pDecData->type = SNAPCAST_MESSAGE_WIRE_CHUNK;

                              // send data to separate task which will handle
                              // this
                              xQueueSend(decoderTaskQHdl, &pDecData,
                                         portMAX_DELAY);

                              opusData = NULL;
                              pDecData = NULL;
                            }

                            break;
                          }

                          case FLAC: {
#if TEST_DECODER_TASK
                            pDecData = NULL;
                            while (!pDecData) {
                              pDecData = (decoderData_t *)malloc(
                                  sizeof(decoderData_t));
                              if (!pDecData) {
                                vTaskDelay(pdMS_TO_TICKS(1));
                              }
                            }

                            pDecData->bytes = tmp_size;

                            // store timestamp for
                            // later use
                            pDecData->timestamp = wire_chnk.timestamp;
                            pDecData->inData = NULL;

                            // while ((!pFlacData->inData) && (mallocCnt < 100))
                            // {
                            while (!pDecData->inData) {
                              pDecData->inData =
                                  (uint8_t *)malloc(pDecData->bytes);
                              if (!pDecData->inData) {
                                vTaskDelay(pdMS_TO_TICKS(1));
                              }
                            }

                            if (pDecData->inData) {
                              memcpy(pDecData->inData, start, tmp_size);
                              pDecData->outData = NULL;
                              pDecData->type = SNAPCAST_MESSAGE_WIRE_CHUNK;

                              // send data to separate task which will handle
                              // this
                              xQueueSend(decoderTaskQHdl, &pDecData,
                                         portMAX_DELAY);
                            }
#else
                            flacData.bytes = tmp_size;
                            flacData.timestamp =
                                wire_chnk.timestamp;  // store timestamp for
                                                      // later use
                            flacData.inData = start;
                            pDecData = &flacData;

                            startTime = esp_timer_get_time();

                            xSemaphoreTake(decoderReadSemaphore, portMAX_DELAY);

                            // send data to flac decoder
                            xQueueSend(decoderReadQHdl, &pDecData,
                                       portMAX_DELAY);
                            // and wait until data was
                            // processed
                            xSemaphoreTake(decoderReadSemaphore, portMAX_DELAY);
                            // need to release mutex
                            // afterwards for next round
                            xSemaphoreGive(decoderReadSemaphore);

#if 0  // enable heap memory analyzing
                            {
                              static uint32_t cnt = 0;
                              if (++cnt % 8 == 0) {
                                ESP_LOGI(
                                    TAG, "8bit %d, block %d, 32 bit %d, block %d, waiting %d",
                                    heap_caps_get_free_size(MALLOC_CAP_8BIT),
                                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                                    heap_caps_get_free_size(MALLOC_CAP_32BIT |
                                    MALLOC_CAP_EXEC),
                                    heap_caps_get_largest_free_block(MALLOC_CAP_32BIT
                                    | MALLOC_CAP_EXEC), pcm_chunk_queue_msg_waiting());
                              }
                            }
#endif
#endif

                            break;
                          }

                          case PCM: {
                            size_t _tmp = tmp_size;

                            offset = 0;

                            if (pcmData == NULL) {
                              if (allocate_pcm_chunk_memory(
                                      &pcmData, wire_chnk.size) < 0) {
                                pcmData = NULL;
                              }

                              tmpData = 0;
                              remainderSize = 0;
                              payloadDataShift = 3;
                              payloadOffset = 0;
                            }

                            while (_tmp--) {
                              tmpData |= ((uint32_t)start[offset++]
                                          << (8 * payloadDataShift));

                              payloadDataShift--;
                              if (payloadDataShift < 0) {
                                payloadDataShift = 3;

                                if ((pcmData) && (pcmData->fragment->payload)) {
                                  volatile uint32_t *sample;
                                  uint8_t dummy1;
                                  uint32_t dummy2 = 0;

                                  // TODO: find a more
                                  // clever way to do this,
                                  // best would be to
                                  // actually store it the
                                  // right way in the first
                                  // place
                                  dummy1 = tmpData >> 24;
                                  dummy2 |= (uint32_t)dummy1 << 16;
                                  dummy1 = tmpData >> 16;
                                  dummy2 |= (uint32_t)dummy1 << 24;
                                  dummy1 = tmpData >> 8;
                                  dummy2 |= (uint32_t)dummy1 << 0;
                                  dummy1 = tmpData >> 0;
                                  dummy2 |= (uint32_t)dummy1 << 8;
                                  tmpData = dummy2;

                                  sample = (volatile uint32_t *)(&(
                                      pcmData->fragment
                                          ->payload[payloadOffset]));
                                  *sample = (volatile uint32_t)tmpData;

                                  payloadOffset += 4;
                                }

                                tmpData = 0;
                              }
                            }

                            break;
                          }

                          default: {
                            ESP_LOGE(TAG, "Decoder (1) not supported");

                            return;

                            break;
                          }
                        }
                      }

                      typedMsgCurrentPos += tmp_size;
                      start += tmp_size;
                      currentPos += tmp_size;
                      len -= tmp_size;

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        if (received_header == true) {
                          switch (codec) {
                            case OPUS: {
                              // nothing to do here
                              break;
                            }

                            case FLAC: {
#if TEST_DECODER_TASK
                              pDecData = NULL;  // send NULL so we know to wait
                                                // for decoded data in task

                              // ESP_LOGE(TAG, "%s: flacTaskQHdl start
                              // wireChnk", __func__);
                              xQueueSend(decoderTaskQHdl, &pDecData,
                                         portMAX_DELAY);
                              // ESP_LOGE(TAG, "%s: flacTaskQHdl stop wireChnk",
                              // __func__);
#else
                              xSemaphoreGive(decoderWriteSemaphore);
                              // and wait until it is done
                              xQueueReceive(decoderWriteQHdl, &pDecData,
                                            portMAX_DELAY);

                              if (pDecData->outData != NULL) {
                                pcmData = pDecData->outData;
                                pcmData->timestamp = wire_chnk.timestamp;

                                size_t decodedSize =
                                    pcmData->totalSize;  // pFlacData->bytes;

                                //                                ESP_LOGE(TAG,
                                //                                "decoded size:
                                //                                %d",
                                //                                decodedSize);

                                scSet.chkInFrames =
                                    decodedSize / ((size_t)scSet.ch *
                                                   (size_t)(scSet.bits / 8));
                                if (player_send_snapcast_setting(&scSet) !=
                                    pdPASS) {
                                  ESP_LOGE(TAG,
                                           "Failed to "
                                           "notify "
                                           "sync task "
                                           "about "
                                           "codec. Did you "
                                           "init player?");

                                  return;
                                }

                                endTime = esp_timer_get_time();

#if CONFIG_USE_DSP_PROCESSOR
                                if ((pcmData) && (pcmData->fragment->payload)) {
                                  dsp_processor_worker(
                                      pcmData->fragment->payload,
                                      pcmData->fragment->size, scSet.sr);
                                }

#endif

                                insert_pcm_chunk(pcmData);

                                // ESP_LOGE(TAG, "duration = %lld", endTime -
                                // startTime);

                                pcmData = NULL;
                              }
#endif

                              break;
                            }

                            case PCM: {
                              size_t decodedSize = wire_chnk.size;

                              //                              ESP_LOGW(TAG, "got
                              //                              PCM chunk,
                              //                              typedMsgCurrentPos
                              //                              %d",
                              //                              typedMsgCurrentPos);

                              if (pcmData) {
                                pcmData->timestamp = wire_chnk.timestamp;
                              }

                              scSet.chkInFrames =
                                  decodedSize /
                                  ((size_t)scSet.ch * (size_t)(scSet.bits / 8));

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to notify "
                                         "sync task about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

#if CONFIG_USE_DSP_PROCESSOR
                              if ((pcmData) && (pcmData->fragment->payload)) {
                                dsp_processor_worker(pcmData->fragment->payload,
                                                     pcmData->fragment->size,
                                                     scSet.sr);
                              }
#endif

                              if (pcmData) {
                                insert_pcm_chunk(pcmData);
                              }

                              pcmData = NULL;

                              break;
                            }

                            default: {
                              ESP_LOGE(TAG,
                                       "Decoder (2) not "
                                       "supported");

                              return;

                              break;
                            }
                          }
                        }

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        typedMsgCurrentPos = 0;
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

                  break;
                }

                case SNAPCAST_MESSAGE_CODEC_HEADER: {
                  switch (internalState) {
                    case 0: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      p_tmp = malloc(typedMsgLen + 1);  // allocate memory for
                                                        // codec string
                      if (p_tmp == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec string");

                        return;
                      }

                      offset = 0;
                      // ESP_LOGI(TAG,
                      // "codec header string is %d long",
                      // typedMsgLen);

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 4: {
                      if (len >= typedMsgLen) {
                        memcpy(&p_tmp[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&p_tmp[offset], start, typedMsgLen);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        // NULL terminate string
                        p_tmp[typedMsgLen] = 0;

                        // ESP_LOGI (TAG, "got codec string: %s", tmp);

                        if (strcmp(p_tmp, "opus") == 0) {
                          codec = OPUS;
                        } else if (strcmp(p_tmp, "flac") == 0) {
                          codec = FLAC;
                        } else if (strcmp(p_tmp, "pcm") == 0) {
                          codec = PCM;
                        } else {
                          codec = NONE;

                          ESP_LOGI(TAG, "Codec : %s not supported", p_tmp);
                          ESP_LOGI(TAG,
                                   "Change encoder codec to "
                                   "opus, flac or pcm in "
                                   "/etc/snapserver.conf on "
                                   "server");

                          return;
                        }

                        free(p_tmp);
                        p_tmp = NULL;

                        internalState++;
                      }

                      break;
                    }

                    case 5: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 6: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 7: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 8: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      p_tmp = malloc(typedMsgLen);  // allocate memory for
                                                    // codec string
                      if (p_tmp == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec string");

                        return;
                      }

                      offset = 0;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 9: {
                      if (len >= typedMsgLen) {
                        memcpy(&p_tmp[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&p_tmp[offset], start, typedMsgLen);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        // first ensure everything is set up
                        // correctly and resources are
                        // available
                        if (t_flac_decoder_task != NULL) {
                          vTaskDelete(t_flac_decoder_task);
                          t_flac_decoder_task = NULL;
                        }

                        if (dec_task_handle != NULL) {
                          vTaskDelete(dec_task_handle);
                          dec_task_handle = NULL;
                        }

                        if (flacDecoder != NULL) {
                          FLAC__stream_decoder_finish(flacDecoder);
                          FLAC__stream_decoder_delete(flacDecoder);
                          flacDecoder = NULL;
                        }

                        if (decoderWriteQHdl != NULL) {
                          vQueueDelete(decoderWriteQHdl);
                          decoderWriteQHdl = NULL;
                        }

                        if (decoderReadQHdl != NULL) {
                          vQueueDelete(decoderReadQHdl);
                          decoderReadQHdl = NULL;
                        }

                        if (decoderTaskQHdl != NULL) {
                          vQueueDelete(decoderTaskQHdl);
                          decoderTaskQHdl = NULL;
                        }

                        if (opusDecoder != NULL) {
                          opus_decoder_destroy(opusDecoder);
                          opusDecoder = NULL;
                        }

                        if (codec == OPUS) {
                          decoderTaskQHdl =
                              xQueueCreate(8, sizeof(decoderData_t *));
                          if (decoderTaskQHdl == NULL) {
                            ESP_LOGE(TAG, "Failed to create decoderTaskQHdl");
                            return;
                          }

                          //                          ESP_LOGI(TAG, "OPUS not
                          //                          implemented yet"); return;
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&rate, p_tmp + 4, sizeof(rate));
                          memcpy(&bits, p_tmp + 8, sizeof(bits));
                          memcpy(&channels, p_tmp + 10, sizeof(channels));

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;

                          ESP_LOGI(TAG, "Opus sample format: %ld:%d:%d\n", rate,
                                   bits, channels);

                          int error = 0;

                          opusDecoder =
                              opus_decoder_create(scSet.sr, scSet.ch, &error);
                          if (error != 0) {
                            ESP_LOGI(TAG, "Failed to init opus coder");
                            return;
                          }

                          ESP_LOGI(TAG, "Initialized opus Decoder: %d", error);

                          if (dec_task_handle == NULL) {
                            xTaskCreatePinnedToCore(
                                &opus_decoder_task, "opus_task", 8 * 1024,
                                &scSet, OPUS_TASK_PRIORITY, &dec_task_handle,
                                OPUS_TASK_CORE_ID);
                          }
                        } else if (codec == FLAC) {
                          decoderTaskQHdl =
                              xQueueCreate(8, sizeof(decoderData_t *));
                          if (decoderTaskQHdl == NULL) {
                            ESP_LOGE(TAG, "Failed to create decoderTaskQHdl");
                            return;
                          }

                          decoderReadQHdl =
                              xQueueCreate(1, sizeof(decoderData_t *));
                          if (decoderReadQHdl == NULL) {
                            ESP_LOGE(TAG, "Failed to create flac read queue");
                            return;
                          }

                          decoderWriteQHdl =
                              xQueueCreate(1, sizeof(decoderData_t *));
                          if (decoderWriteQHdl == NULL) {
                            ESP_LOGE(TAG, "Failed to create flac write queue");
                            return;
                          }

                          if (t_flac_decoder_task == NULL) {
                            xTaskCreatePinnedToCore(
                                &flac_decoder_task, "flac_decoder_task",
                                10 * 256, &scSet, FLAC_DECODER_TASK_PRIORITY,
                                &t_flac_decoder_task,
                                FLAC_DECODER_TASK_CORE_ID);
                          }

#if TEST_DECODER_TASK
                          if (dec_task_handle == NULL) {
                            xTaskCreatePinnedToCore(
                                &flac_task, "flac_task", 10 * 256, &scSet,
                                FLAC_TASK_PRIORITY, &dec_task_handle,
                                FLAC_TASK_CORE_ID);
                          }

                          pDecData =
                              (decoderData_t *)malloc(sizeof(decoderData_t));
                          memset(pDecData, 0, sizeof(decoderData_t));

                          pDecData->bytes = typedMsgLen;
                          pDecData->inData = (uint8_t *)malloc(typedMsgLen);
                          memcpy(pDecData->inData, p_tmp, typedMsgLen);
                          pDecData->outData = NULL;
                          pDecData->type = SNAPCAST_MESSAGE_CODEC_HEADER;

                          // ESP_LOGE(TAG, "%s: flacTaskQHdl start codec
                          // header", __func__);

                          // send codec header to flac decoder
                          xQueueSend(decoderTaskQHdl, &pDecData, portMAX_DELAY);

                          // ESP_LOGE(TAG, "sent codec header");

                          // send NULL so we know to wait
                          // for decoded data in task
                          pDecData = NULL;
                          xQueueSend(decoderTaskQHdl, &pDecData, portMAX_DELAY);

                          // ESP_LOGE(TAG, "%s: flacTaskQHdl done codec header",
                          // __func__);

#else

                          if (flacData.outData != NULL) {
                            free(flacData.outData);
                            flacData.outData = NULL;
                          }

                          flacData.bytes = typedMsgLen;
                          flacData.inData = p_tmp;
                          pDecData = &flacData;

                          // TODO: find a smarter way for
                          // this wait for task creation done
                          while (decoderReadQHdl == NULL) {
                            vTaskDelay(10);
                          }

                          xSemaphoreTake(decoderReadSemaphore, portMAX_DELAY);

                          // send data to flac decoder
                          xQueueSend(decoderReadQHdl, &pDecData, portMAX_DELAY);
                          // and wait until data was
                          // processed
                          xSemaphoreTake(decoderReadSemaphore, portMAX_DELAY);
                          // need to release mutex afterwards
                          // for next round
                          xSemaphoreGive(decoderReadSemaphore);
                          // wait until it is done
                          xQueueReceive(decoderWriteQHdl, &pDecData,
                                        portMAX_DELAY);

                          ESP_LOGI(TAG, "fLaC sampleformat: %d:%d:%d", scSet.sr,
                                   scSet.bits, scSet.ch);
#endif
                        } else if (codec == PCM) {
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&channels, p_tmp + 22, sizeof(channels));
                          memcpy(&rate, p_tmp + 24, sizeof(rate));
                          memcpy(&bits, p_tmp + 34, sizeof(bits));

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;

                          ESP_LOGI(TAG, "pcm sampleformat: %ld:%d:%d", scSet.sr,
                                   scSet.bits, scSet.ch);
                        } else {
                          ESP_LOGE(TAG,
                                   "codec header decoder "
                                   "shouldn't get here after "
                                   "codec string was detected");

                          return;
                        }

                        free(p_tmp);
                        p_tmp = NULL;

                        // ESP_LOGI(TAG, "done codec header msg");

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        received_header = true;
                        esp_timer_stop(timeSyncMessageTimer);
                        if (!esp_timer_is_active(timeSyncMessageTimer)) {
                          esp_timer_start_periodic(timeSyncMessageTimer,
                                                   timeout);
                        }
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

                  break;
                }

                case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
                  switch (internalState) {
                    case 0: {
                      while ((netbuf_len(firstNetBuf) - currentPos) <
                             base_message_rx.size) {
                        ESP_LOGI(TAG, "need more data");

                        // we need more data to process
                        rc1 = netconn_recv(lwipNetconn, &newNetBuf);
                        if (rc1 != ERR_OK) {
                          ESP_LOGE(TAG, "rx error for need more data");

                          if (rc1 == ERR_CONN) {
                            // netconn_close(lwipNetconn);
                            // closing later, see first
                            // netconn_recv() in the loop

                            break;
                          }

                          if (newNetBuf != NULL) {
                            netbuf_delete(newNetBuf);

                            newNetBuf = NULL;
                          }

                          continue;
                        }

                        netbuf_chain(firstNetBuf, newNetBuf);
                      }

                      if (rc1 == ERR_OK) {
                        typedMsgLen = *start & 0xFF;

                        typedMsgCurrentPos++;
                        start++;
                        currentPos++;
                        len--;

                        internalState++;
                      } else {
                        ESP_LOGE(TAG, "some error");
                      }

                      break;
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      //                       ESP_LOGI(TAG,
                      //                       "server settings string is %d
                      //                       long", typedMsgLen);

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 4: {
                      // now get some memory for server settings
                      // string at this point there is still
                      // plenty of RAM available, so we use
                      // malloc and netbuf_copy() here
                      p_tmp = malloc(typedMsgLen + 1);

                      if (p_tmp == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory for "
                                 "server settings string");
                      } else {
                        netbuf_copy_partial(firstNetBuf, p_tmp, typedMsgLen,
                                            currentPos);

                        p_tmp[typedMsgLen] = 0;  // NULL terminate string

                        // ESP_LOGI
                        //(TAG, "got string: %s", tmp);

                        result = server_settings_message_deserialize(
                            &server_settings_message, p_tmp);
                        if (result) {
                          ESP_LOGE(TAG,
                                   "Failed to read server "
                                   "settings: %d",
                                   result);
                        } else {
                          // log mute state, buffer, latency
                          ESP_LOGI(TAG, "Buffer length:  %ld",
                                   server_settings_message.buffer_ms);
                          ESP_LOGI(TAG, "Latency:        %ld",
                                   server_settings_message.latency);
                          ESP_LOGI(TAG, "Mute:           %d",
                                   server_settings_message.muted);
                          ESP_LOGI(TAG, "Setting volume: %ld",
                                   server_settings_message.volume);
                        }

                        // Volume setting using ADF HAL
                        // abstraction
                        if (scSet.muted != server_settings_message.muted) {
#if SNAPCAST_USE_SOFT_VOL
                          if (server_settings_message.muted) {
                            dsp_processor_set_volome(0.0);
                          } else {
                            dsp_processor_set_volome(
                                (double)server_settings_message.volume / 100);
                          }
#endif
                          audio_hal_set_mute(board_handle->audio_hal,
                                             server_settings_message.muted);
                        }

                        if (scSet.volume != server_settings_message.volume) {
#if SNAPCAST_USE_SOFT_VOL
                          if (!server_settings_message.muted) {
                            dsp_processor_set_volome(
                                (double)server_settings_message.volume / 100);
                          }
#else
                          audio_hal_set_volume(board_handle->audio_hal,
                                               server_settings_message.volume);
#endif
                        }

                        scSet.cDacLat_ms = server_settings_message.latency;
                        scSet.buf_ms = server_settings_message.buffer_ms;
                        scSet.muted = server_settings_message.muted;
                        scSet.volume = server_settings_message.volume;

                        if (player_send_snapcast_setting(&scSet) != pdPASS) {
                          ESP_LOGE(TAG,
                                   "Failed to notify sync task. "
                                   "Did you init player?");

                          return;
                        }

                        free(p_tmp);
                        p_tmp = NULL;
                      }

                      internalState++;
                      // fall through
                    }

                    case 5: {
                      size_t tmpSize =
                          base_message_rx.size - typedMsgCurrentPos;

                      if (len > 0) {
                        if (tmpSize < len) {
                          start += tmpSize;
                          currentPos += tmpSize;  // will be
                                                  // incremented by 1
                                                  // later so -1 here
                          typedMsgCurrentPos += tmpSize;
                          len -= tmpSize;
                        } else {
                          start += len;
                          currentPos += len;  // will be incremented
                                              // by 1 later so -1
                                              // here
                          typedMsgCurrentPos += len;
                          len = 0;
                        }
                      }

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        // ESP_LOGI(TAG,
                        // "done server settings");

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        typedMsgCurrentPos = 0;
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

                  break;
                }

                case SNAPCAST_MESSAGE_STREAM_TAGS: {
                  size_t tmpSize = base_message_rx.size - typedMsgCurrentPos;

                  if (tmpSize < len) {
                    start += tmpSize;
                    currentPos += tmpSize;
                    typedMsgCurrentPos += tmpSize;
                    len -= tmpSize;
                  } else {
                    start += len;
                    currentPos += len;

                    typedMsgCurrentPos += len;
                    len = 0;
                  }

                  if (typedMsgCurrentPos >= base_message_rx.size) {
                    // ESP_LOGI(TAG,
                    // "done stream tags with length %d %d %d",
                    // base_message_rx.size, currentPos,
                    // tmpSize);

                    typedMsgCurrentPos = 0;
                    // currentPos = 0;

                    state = BASE_MESSAGE_STATE;
                    internalState = 0;
                  }

                  break;
                }

                case SNAPCAST_MESSAGE_TIME: {
                  switch (internalState) {
                    case 0: {
                      time_message_rx.latency.sec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 1: {
                      time_message_rx.latency.sec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 2: {
                      time_message_rx.latency.sec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 3: {
                      time_message_rx.latency.sec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 4: {
                      time_message_rx.latency.usec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 5: {
                      time_message_rx.latency.usec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 6: {
                      time_message_rx.latency.usec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;

                      internalState++;

                      break;
                    }

                    case 7: {
                      time_message_rx.latency.usec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      currentPos++;
                      len--;
                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        // ESP_LOGI(TAG, "done time message");

                        typedMsgCurrentPos = 0;

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;

                        trx =
                            (int64_t)base_message_rx.received.sec * 1000000LL +
                            (int64_t)base_message_rx.received.usec;
                        ttx = (int64_t)base_message_rx.sent.sec * 1000000LL +
                              (int64_t)base_message_rx.sent.usec;
                        tdif = trx - ttx;
                        trx = (int64_t)time_message_rx.latency.sec * 1000000LL +
                              (int64_t)time_message_rx.latency.usec;
                        tmpDiffToServer = (trx - tdif) / 2;

                        int64_t diff;

                        // clear diffBuffer if last update is
                        // older than a minute
                        diff = now - lastTimeSync;
                        if (diff > 60000000LL) {
                          ESP_LOGW(TAG,
                                   "Last time sync older "
                                   "than a minute. "
                                   "Clearing time buffer");

                          reset_latency_buffer();

                          timeout = FAST_SYNC_LATENCY_BUF;

                          esp_timer_stop(timeSyncMessageTimer);
                          if (received_header == true) {
                            if (!esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_start_periodic(timeSyncMessageTimer,
                                                       timeout);
                            }
                          }
                        }

                        player_latency_insert(tmpDiffToServer);

                        // ESP_LOGI(TAG, "Current latency:%lld:",
                        // tmpDiffToServer);

                        // store current time
                        lastTimeSync = now;

                        if (received_header == true) {
                          if (!esp_timer_is_active(timeSyncMessageTimer)) {
                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          }

                          bool is_full = false;
                          latency_buffer_full(&is_full, portMAX_DELAY);
                          if ((is_full == true) &&
                              (timeout < NORMAL_SYNC_LATENCY_BUF)) {
                            timeout = NORMAL_SYNC_LATENCY_BUF;

                            ESP_LOGI(TAG, "latency buffer full");

                            if (esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_stop(timeSyncMessageTimer);
                            }

                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          } else if ((is_full == false) &&
                                     (timeout > FAST_SYNC_LATENCY_BUF)) {
                            timeout = FAST_SYNC_LATENCY_BUF;

                            ESP_LOGI(TAG, "latency buffer not full");

                            if (esp_timer_is_active(timeSyncMessageTimer)) {
                              esp_timer_stop(timeSyncMessageTimer);
                            }

                            esp_timer_start_periodic(timeSyncMessageTimer,
                                                     timeout);
                          }
                        }
                      } else {
                        ESP_LOGE(TAG,
                                 "error time message, this "
                                 "shouldn't happen! %d %ld",
                                 typedMsgCurrentPos, base_message_rx.size);

                        typedMsgCurrentPos = 0;

                        state = BASE_MESSAGE_STATE;
                        internalState = 0;
                      }

                      break;
                    }

                    default: {
                      ESP_LOGE(TAG,
                               "time message decoder shouldn't "
                               "get here %d %ld %ld",
                               typedMsgCurrentPos, base_message_rx.size,
                               internalState);

                      break;
                    }
                  }

                  break;
                }

                default: {
                  typedMsgCurrentPos++;
                  start++;
                  currentPos++;
                  len--;

                  if (typedMsgCurrentPos >= base_message_rx.size) {
                    ESP_LOGI(TAG, "done unknown typed message %d",
                             base_message_rx.type);

                    state = BASE_MESSAGE_STATE;
                    internalState = 0;

                    typedMsgCurrentPos = 0;
                  }

                  break;
                }
              }

              break;
            }

            default: {
              break;
            }
          }

          if (rc1 != ERR_OK) {
            break;
          }
        }
      } while (netbuf_next(firstNetBuf) >= 0);

      netbuf_delete(firstNetBuf);

      if (rc1 != ERR_OK) {
        ESP_LOGE(TAG, "Data error, closing netconn");

        netconn_close(lwipNetconn);

        break;
      }
    }
  }
}

/**
 *
 */
void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  esp_log_level_set("*", ESP_LOG_INFO);

  // if enabled these cause a timer srv stack overflow
  esp_log_level_set("HEADPHONE", ESP_LOG_NONE);
  esp_log_level_set("gpio", ESP_LOG_WARN);
  esp_log_level_set("uart", ESP_LOG_WARN);
  // esp_log_level_set("i2s_std", ESP_LOG_DEBUG);
  // esp_log_level_set("i2s_common", ESP_LOG_DEBUG);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);
  esp_log_level_set("wifi", ESP_LOG_WARN);
  esp_log_level_set("wifi_init", ESP_LOG_WARN);

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  // clang-format off
  // nINT/REFCLKO Function Select Configuration Strap
  //  • When nINTSEL is floated or pulled to
  //    VDD2A, nINT is selected for operation on the
  //    nINT/REFCLKO pin (default).
  //  • When nINTSEL is pulled low to VSS, REF-
  //    CLKO is selected for operation on the nINT/
  //    REFCLKO pin.
  //
  // LAN8720 doesn't stop REFCLK while in reset, so we leave the
  // strap floated. It is connected to IO0 on ESP32 so we get nINT
  // function with a HIGH pin value, which is also perfect during boot.
  // Before initializing LAN8720 (which resets the PHY) we pull the
  // strap low and this results in REFCLK enabled which is needed
  // for MAC unit.
  //
  // clang-format on
  gpio_config_t cfg = {.pin_bit_mask = BIT64(GPIO_NUM_5),
                       .mode = GPIO_MODE_DEF_INPUT,
                       .pull_up_en = GPIO_PULLUP_DISABLE,
                       .pull_down_en = GPIO_PULLDOWN_ENABLE,
                       .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&cfg);
#endif

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  // some codecs need i2s mclk for initialization

  i2s_chan_handle_t tx_chan;

  i2s_chan_config_t tx_chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 2,
      .dma_frame_num = 128,
      .auto_clear = true,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

  board_i2s_pin_t pin_config0;
  get_i2s_pins(I2S_NUM_0, &pin_config0);

  i2s_std_clk_config_t i2s_clkcfg = {
      .sample_rate_hz = 44100,
      .clk_src = I2S_CLK_SRC_APLL,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = i2s_clkcfg,
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = pin_config0
                          .mck_io_num,  // some codecs may require mclk signal,
                                        // this example doesn't need it
              .bclk = pin_config0.bck_io_num,
              .ws = pin_config0.ws_io_num,
              .dout = pin_config0.data_out_num,
              .din = pin_config0.data_in_num,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
  i2s_channel_enable(tx_chan);
#endif

  ESP_LOGI(TAG, "Start codec chip");
  board_handle = audio_board_init();
  if (board_handle) {
    ESP_LOGI(TAG, "Audio board_init done");
  } else {
    ESP_LOGE(TAG,
             "Audio board couldn't be initialized. Check menuconfig if project "
             "is configured right or check your wiring!");

    vTaskDelay(portMAX_DELAY);
  }

  audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE,
                       AUDIO_HAL_CTRL_START);
  audio_hal_set_mute(board_handle->audio_hal,
                     true);  // ensure no noise is sent after firmware crash

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  if (tx_chan) {
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
#endif

  ESP_LOGI(TAG, "init player");
  init_player();

  // ensure there is no noise from DAC
  {
    board_i2s_pin_t pin_config0;
    get_i2s_pins(I2S_NUM_0, &pin_config0);

    gpio_config_t gpioCfg = {
        .pin_bit_mask =
            BIT64(pin_config0.mck_io_num) | BIT64(pin_config0.data_out_num) |
            BIT64(pin_config0.bck_io_num) | BIT64(pin_config0.ws_io_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpioCfg);
    gpio_set_level(pin_config0.mck_io_num, 0);
    gpio_set_level(pin_config0.data_out_num, 0);
    gpio_set_level(pin_config0.bck_io_num, 0);
    gpio_set_level(pin_config0.ws_io_num, 0);

    gpioCfg.pin_bit_mask = BIT64(pin_config0.data_in_num);
    gpioCfg.mode = GPIO_MODE_INPUT;
    gpioCfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&gpioCfg);
  }

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
  eth_init();
  // pass "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF"
  init_http_server_task("ETH_DEF");
#else
  // Enable and setup WIFI in station mode and connect to Access point setup in
  // menu config or set up provisioning mode settable in menuconfig
  wifi_init();
  ESP_LOGI(TAG, "Connected to AP");
  // http server for control operations and user interface
  // pass "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF"
  init_http_server_task("WIFI_STA_DEF");
#endif

  // Enable websocket server
  //  ESP_LOGI(TAG, "Setup ws server");
  //  websocket_if_start();

  net_mdns_register("snapclient");
#ifdef CONFIG_SNAPCLIENT_SNTP_ENABLE
  set_time_from_sntp();
#endif

#if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_init();
#endif

  xTaskCreatePinnedToCore(&ota_server_task, "ota", 14 * 256, NULL,
                          OTA_TASK_PRIORITY, &t_ota_task, OTA_TASK_CORE_ID);

  xTaskCreatePinnedToCore(&http_get_task, "http", 4 * 1024, NULL,
                          HTTP_TASK_PRIORITY, &t_http_get_task,
                          HTTP_TASK_CORE_ID);

  //  while (1) {
  //    // audio_event_iface_msg_t msg;
  //    vTaskDelay(portMAX_DELAY);  //(pdMS_TO_TICKS(5000));
  //
  //    // ma120_read_error(0x20);
  //
  //    esp_err_t ret = 0;  // audio_event_iface_listen(evt, &msg,
  //    portMAX_DELAY); if (ret != ESP_OK) {
  //      ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
  //      continue;
  //    }
  //  }
}

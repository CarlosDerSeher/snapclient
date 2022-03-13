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
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_interface.h"

// Minimum ESP-IDF stuff only hardware abstraction stuff
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
#include "websocket_if.h"
//#include "websocket_server.h"

#include <sys/time.h>

#include "driver/i2s.h"
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

static FLAC__StreamDecoderReadStatus
read_callback (const FLAC__StreamDecoder *decoder, FLAC__byte buffer[],
               size_t *bytes, void *client_data);
static FLAC__StreamDecoderWriteStatus
write_callback (const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
                const FLAC__int32 *const buffer[], void *client_data);
static void metadata_callback (const FLAC__StreamDecoder *decoder,
                               const FLAC__StreamMetadata *metadata,
                               void *client_data);
static void error_callback (const FLAC__StreamDecoder *decoder,
                            FLAC__StreamDecoderErrorStatus status,
                            void *client_data);

//#include "ma120.h"

static FLAC__StreamDecoder *flacDecoder = NULL;
static QueueHandle_t decoderReadQHdl = NULL;
static QueueHandle_t decoderWriteQHdl = NULL;
SemaphoreHandle_t decoderReadSemaphore = NULL;
SemaphoreHandle_t decoderWriteSemaphore = NULL;

const char *VERSION_STRING = "0.0.2";

#define HTTP_TASK_PRIORITY 6
#define HTTP_TASK_CORE_ID 1 // tskNO_AFFINITY

#define OTA_TASK_PRIORITY 6
#define OTA_TASK_CORE_ID 1 // tskNO_AFFINITY

#define FLAC_TASK_PRIORITY 6
#define FLAC_TASK_CORE_ID 1 // tskNO_AFFINITY

xTaskHandle t_ota_task = NULL;
xTaskHandle t_http_get_task = NULL;
xTaskHandle t_flac_decoder_task = NULL;

struct timeval tdif, tavg;
static audio_board_handle_t board_handle = NULL;

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_SERVER_USE_MDNS CONFIG_SNAPSERVER_USE_MDNS
#if !SNAPCAST_SERVER_USE_MDNS
#define SNAPCAST_SERVER_HOST CONFIG_SNAPSERVER_HOST
#define SNAPCAST_SERVER_PORT CONFIG_SNAPSERVER_PORT
#endif
#define SNAPCAST_BUFF_LEN CONFIG_SNAPCLIENT_BUFF_LEN
#define SNAPCAST_CLIENT_NAME CONFIG_SNAPCLIENT_NAME

/* Logging tag */
static const char *TAG = "SC";

extern char mac_address[18];

// static QueueHandle_t playerChunkQueueHandle = NULL;
SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

#if CONFIG_USE_DSP_PROCESSOR
uint8_t dspFlow = dspfStereo; // dspfBiamp; // dspfStereo; // dspfBassBoost;
#endif

typedef struct flacData_s
{
  char *inData;
  pcm_chunk_message_t *outData;
  uint32_t bytes;
} flacData_t;

/**
 *
 */
void
time_sync_msg_cb (void *args)
{
  BaseType_t xHigherPriorityTaskWoken;

  // causes kernel panic, which shouldn't happen though?
  // Isn't it called from timer task instead of ISR?
  // xSemaphoreGive(timeSyncSemaphoreHandle);

  xSemaphoreGiveFromISR (timeSyncSemaphoreHandle, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken)
    {
      portYIELD_FROM_ISR ();
    }
}

static FLAC__StreamDecoderReadStatus
read_callback (const FLAC__StreamDecoder *decoder, FLAC__byte buffer[],
               size_t *bytes, void *client_data)
{
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  flacData_t *flacData;

  (void)scSet;

  xQueueReceive (decoderReadQHdl, &flacData, portMAX_DELAY);

  //  	ESP_LOGI(TAG, "in flac read cb %d %p", flacData->bytes,
  //  flacData->inData);

  if (flacData->bytes <= 0)
    {
      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

  if (flacData->inData == NULL)
    {
      return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

  if (flacData->bytes <= *bytes)
    {
      memcpy (buffer, flacData->inData, flacData->bytes);
      *bytes = flacData->bytes;
      //      ESP_LOGW(TAG, "read all flac inData %d", *bytes);
    }
  else
    {
      memcpy (buffer, flacData->inData, *bytes);
      ESP_LOGW (TAG, "dind't read all flac inData %d", *bytes);
      flacData->inData += *bytes;
      flacData->bytes -= *bytes;
    }

  // xQueueSend (flacReadQHdl, &flacData, portMAX_DELAY);

  xSemaphoreGive (decoderReadSemaphore);

  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static flacData_t flacOutData;

static FLAC__StreamDecoderWriteStatus
write_callback (const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
                const FLAC__int32 *const buffer[], void *client_data)
{
  size_t i;
  flacData_t *flacData = &flacOutData;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  int ret = 0;
  uint32_t tmpData;
  uint32_t fragmentCnt = 0;

  (void)decoder;

  xSemaphoreTake (decoderWriteSemaphore, portMAX_DELAY);

  // xQueueReceive (flacReadQHdl, &flacData, portMAX_DELAY);

  //  ESP_LOGI(TAG, "in flac write cb %d %p", frame->header.blocksize,
  //  flacData);

  if (frame->header.channels != scSet->ch)
    {
      ESP_LOGE (TAG,
                "ERROR: frame header reports different channel count %d than "
                "previous metadata block %d",
                frame->header.channels, scSet->ch);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  if (frame->header.bits_per_sample != scSet->bits)
    {
      ESP_LOGE (TAG,
                "ERROR: frame header reports different bps %d than previous "
                "metadata block %d",
                frame->header.bits_per_sample, scSet->bits);
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  if (buffer[0] == NULL)
    {
      ESP_LOGE (TAG, "ERROR: buffer [0] is NULL\n");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  if (buffer[1] == NULL)
    {
      ESP_LOGE (TAG, "ERROR: buffer [1] is NULL\n");
      return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

  flacData->bytes = frame->header.blocksize * frame->header.channels
                    * (frame->header.bits_per_sample / 8);

  // flacData->outData = (char *)realloc (flacData->outData, flacData->bytes);
  // flacData->outData = (char *)malloc (flacData->bytes);
  ret = allocate_pcm_chunk_memory (&(flacData->outData), flacData->bytes);

  // ESP_LOGI (TAG, "mem %p %p %d", flacData->outData,
  // flacData->outData->fragment->payload, flacData->bytes);

  if (ret == 0)
    {
      pcm_chunk_fragment_t *fragment = flacData->outData->fragment;

      fragmentCnt = 0;

      for (i = 0; i < frame->header.blocksize; i++)
        {
          // write little endian
          // flacData->outData[4 * i] = (uint8_t)buffer[0][i];
          // flacData->outData[4 * i + 1] = (uint8_t) (buffer[0][i] >> 8);
          // flacData->outData[4 * i + 2] = (uint8_t)buffer[1][i];
          // flacData->outData[4 * i + 3] = (uint8_t)(buffer[1][i] >> 8);

          // TODO: for now fragmented payload is not supported and the whole
          // chunk is expected to be in the first fragment
          tmpData = ((uint32_t) ((buffer[0][i] >> 8) & 0xFF) << 24)
                    | ((uint32_t) ((buffer[0][i] >> 0) & 0xFF) << 16)
                    | ((uint32_t) ((buffer[1][i] >> 8) & 0xFF) << 8)
                    | ((uint32_t) ((buffer[1][i] >> 0) & 0xFF) << 0);

          if (fragment != NULL)
            {
              uint32_t *test = (uint32_t *)(&(fragment->payload[fragmentCnt]));
              *test = tmpData;
            }

          fragmentCnt += 4;
          if (fragmentCnt >= fragment->size)
            {
              fragmentCnt = 0;

              fragment = fragment->nextFragment;
            }
        }
    }
  else
    {
      flacData->outData = NULL;
    }

  xQueueSend (decoderWriteQHdl, &flacData, portMAX_DELAY);

  // xSemaphoreGive(flacWriteSemaphore);

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void
metadata_callback (const FLAC__StreamDecoder *decoder,
                   const FLAC__StreamMetadata *metadata, void *client_data)
{
  flacData_t *flacData = &flacOutData;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  (void)decoder;

  // xQueueReceive (flacReadQHdl, &flacData, portMAX_DELAY);

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO)
    {
      //		ESP_LOGI(TAG, "in flac meta cb");

      // save for later
      scSet->sr = metadata->data.stream_info.sample_rate;
      scSet->ch = metadata->data.stream_info.channels;
      scSet->bits = metadata->data.stream_info.bits_per_sample;

      xQueueSend (decoderWriteQHdl, &flacData, portMAX_DELAY);
    }

  //  xSemaphoreGive(flacReadSemaphore);
}

void
error_callback (const FLAC__StreamDecoder *decoder,
                FLAC__StreamDecoderErrorStatus status, void *client_data)
{
  (void)decoder, (void)client_data;

  ESP_LOGE (TAG, "Got error callback: %s\n",
            FLAC__StreamDecoderErrorStatusString[status]);
}

static void
flac_decoder_task (void *pvParameters)
{
  FLAC__bool ok = true;
  FLAC__StreamDecoderInitStatus init_status;
  snapcastSetting_t *scSet = (snapcastSetting_t *)pvParameters;

  if (decoderReadQHdl != NULL)
    {
      vQueueDelete (decoderReadQHdl);
      decoderReadQHdl = NULL;
    }

  decoderReadQHdl = xQueueCreate (1, sizeof (flacData_t *));
  if (decoderReadQHdl == NULL)
    {
      ESP_LOGE (TAG, "Failed to create flac read queue");
      return;
    }

  if (decoderWriteQHdl != NULL)
    {
      vQueueDelete (decoderWriteQHdl);
      decoderWriteQHdl = NULL;
    }

  decoderWriteQHdl = xQueueCreate (1, sizeof (flacData_t *));
  if (decoderWriteQHdl == NULL)
    {
      ESP_LOGE (TAG, "Failed to create flac write queue");
      return;
    }

  if (flacDecoder != NULL)
    {
      FLAC__stream_decoder_finish (flacDecoder);
      FLAC__stream_decoder_delete (flacDecoder);
      flacDecoder = NULL;
    }

  flacDecoder = FLAC__stream_decoder_new ();
  if (flacDecoder == NULL)
    {
      ESP_LOGE (TAG, "Failed to init flac decoder");
      return;
    }

  init_status = FLAC__stream_decoder_init_stream (
      flacDecoder, read_callback, NULL, NULL, NULL, NULL, write_callback,
      metadata_callback, error_callback, scSet);
  if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK)
    {
      ESP_LOGE (TAG, "ERROR: initializing decoder: %s\n",
                FLAC__StreamDecoderInitStatusString[init_status]);
      ok = false;
      return;
    }

  while (1)
    {
      FLAC__stream_decoder_process_until_end_of_stream (flacDecoder);
    }
}

static char base_message_serialized[BASE_MESSAGE_SIZE];
static char time_message_serialized[TIME_MESSAGE_SIZE];
static const esp_timer_create_args_t tSyncArgs
    = { .callback = &time_sync_msg_cb, .name = "tSyncMsg" };

struct netconn *lwipNetconn;

/**
 *
 */
static void
http_get_task (void *pvParameters)
{
  struct sockaddr_in servaddr;
  char *start;
  int sock = -1;
  base_message_t base_message_rx;
  base_message_t base_message_tx;
  hello_message_t hello_message;
  wire_chunk_message_t wire_chnk = { { 0, 0 }, 0, NULL };
  char *hello_message_serialized = NULL;
  int result, size, id_counter;
  struct timeval now, trx, tdif, ttx;
  time_message_t time_message_rx = { { 0, 0 } };
  time_message_t time_message_tx = { { 0, 0 } };
  struct timeval tmpDiffToServer;
  struct timeval lastTimeSync = { 0, 0 };
  esp_timer_handle_t timeSyncMessageTimer = NULL;
  int16_t frameSize = 960; // 960*2: 20ms, 960*1: 10ms
  int16_t *audio = NULL;
  int16_t pcm_size = 120;
  uint16_t channels;
  esp_err_t err = 0;
  codec_header_message_t codec_header_message;
  server_settings_message_t server_settings_message;
  bool received_header = false;
  mdns_result_t *r;
  OpusDecoder *opusDecoder = NULL;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  flacData_t flacData = { NULL, NULL, 0 };
  flacData_t *pFlacData;
  pcm_chunk_message_t *pcmData = NULL;
  char *typedMsg = NULL;
  uint32_t lastTypedMsgSize = 0;
  //  struct netconn *lwipNetconn = NULL;
  ip_addr_t local_ip;
  ip_addr_t remote_ip;
  uint16_t remotePort = 0;
  int rc1 = ERR_OK, rc2 = ERR_OK;
  struct netbuf *firstNetBuf = NULL;
  struct netbuf *newNetBuf = NULL;
  uint16_t len;

  // create a timer to send time sync messages every x µs
  esp_timer_create (&tSyncArgs, &timeSyncMessageTimer);
  timeSyncSemaphoreHandle = xSemaphoreCreateMutex ();
  xSemaphoreGive (timeSyncSemaphoreHandle);

  id_counter = 0;

#if CONFIG_SNAPCLIENT_USE_MDNS
  ESP_LOGI (TAG, "Enable mdns");
  mdns_init ();
#endif

  while (1)
    {
      if (reset_latency_buffer () < 0)
        {
          ESP_LOGE (
              TAG,
              "reset_diff_buffer: couldn't reset median filter long. STOP");

          return;
        }

      esp_timer_stop (timeSyncMessageTimer);
      xSemaphoreGive (timeSyncSemaphoreHandle);

      if (opusDecoder != NULL)
        {
          opus_decoder_destroy (opusDecoder);
          opusDecoder = NULL;
        }

      if (t_flac_decoder_task != NULL)
        {
          vTaskDelete (t_flac_decoder_task);
          t_flac_decoder_task = NULL;
        }

      if (flacDecoder != NULL)
        {
          FLAC__stream_decoder_finish (flacDecoder);
          FLAC__stream_decoder_delete (flacDecoder);
          flacDecoder = NULL;
        }

      if (decoderWriteQHdl != NULL)
        {
          vQueueDelete (decoderWriteQHdl);
          decoderWriteQHdl = NULL;
        }

      if (decoderReadQHdl != NULL)
        {
          vQueueDelete (decoderReadQHdl);
          decoderReadQHdl = NULL;
        }

#if SNAPCAST_SERVER_USE_MDNS
      // Find snapcast server
      // Connect to first snapcast server found
      r = NULL;
      err = 0;
      while (!r || err)
        {
          ESP_LOGI (TAG, "Lookup snapcast service on network");
          esp_err_t err = mdns_query_ptr ("_snapcast", "_tcp", 3000, 20, &r);
          if (err)
            {
              ESP_LOGE (TAG, "Query Failed");
              vTaskDelay (pdMS_TO_TICKS (1000));
            }

          if (!r)
            {
              ESP_LOGW (TAG, "No results found!");
              vTaskDelay (pdMS_TO_TICKS (1000));
            }
        }

      //      char serverAddr[] = "255.255.255.255";
      //      ESP_LOGI (TAG, "Found %s:%d",
      //                inet_ntop (AF_INET, &(r->addr->addr.u_addr.ip4.addr),
      //                           serverAddr, sizeof (serverAddr)),
      //                r->port);

      if (r->addr)
        {
          servaddr.sin_family = AF_INET;
          servaddr.sin_addr.s_addr = r->addr->addr.u_addr.ip4.addr;
          servaddr.sin_port = htons (r->port);

          ip_addr_copy (remote_ip, (r->addr->addr));
          remotePort = r->port;
          ESP_LOGI (TAG, "Found %s:%d", ipaddr_ntoa (&remote_ip), remotePort);

          mdns_query_results_free (r);
        }
      else
        {
          mdns_query_results_free (r);

          ESP_LOGW (TAG, "No IP found in MDNS query");

          continue;
        }
#else
      // configure a failsafe snapserver according to CONFIG values
      servaddr.sin_family = AF_INET;
      inet_pton (AF_INET, SNAPCAST_SERVER_HOST, &(servaddr.sin_addr.s_addr));
      servaddr.sin_port = htons (SNAPCAST_SERVER_PORT);

      inet_pton (AF_INET, SNAPCAST_SERVER_HOST, &(remote_ip.u_addr.ip4.addr));
      remotePort = SNAPCAST_SERVER_PORT;
#endif

      if (lwipNetconn != NULL)
        {
          netconn_delete (lwipNetconn);
          lwipNetconn = NULL;
        }

      lwipNetconn = netconn_new (NETCONN_TCP);
      if (lwipNetconn == NULL)
        {
          ESP_LOGE (TAG, "can't create netconn");

          continue;
        }

      rc1 = netconn_bind (lwipNetconn, IPADDR_ANY, 0);
      if (rc1 != ERR_OK)
        {
          ESP_LOGE (TAG, "can't bind local IP");
        }

      // ipaddr_aton("192.168.1.54", &remote_ip);
      rc2 = netconn_connect (lwipNetconn, &remote_ip, remotePort);
      if (rc2 != ERR_OK)
        {
          ESP_LOGE (TAG, "can't connect to remote %s:%d, err %d",
                    ipaddr_ntoa (&remote_ip), remotePort, rc2);
        }
      if (rc1 != ERR_OK || rc2 != ERR_OK)
        {
          netconn_close (lwipNetconn);
          netconn_delete (lwipNetconn);
          lwipNetconn = NULL;

          continue;
        }

      ESP_LOGI (TAG, "netconn connected");

      result = gettimeofday (&now, NULL);
      if (result)
        {
          ESP_LOGE (TAG, "Failed to gettimeofday\r\n");
          return;
        }

      received_header = false;

      // init base message
      base_message_rx.type = SNAPCAST_MESSAGE_HELLO;
      base_message_rx.id = 0x0000;
      base_message_rx.refersTo = 0x0000;
      base_message_rx.sent.sec = now.tv_sec;
      base_message_rx.sent.usec = now.tv_usec;
      base_message_rx.received.sec = 0;
      base_message_rx.received.usec = 0;
      base_message_rx.size = 0x00000000;

      // init hello message
      hello_message.mac = mac_address;
      hello_message.hostname = SNAPCAST_CLIENT_NAME;
      hello_message.version = (char *)VERSION_STRING;
      hello_message.client_name = SNAPCAST_CLIENT_NAME;
      hello_message.os = "esp32";
      hello_message.arch = "xtensa";
      hello_message.instance = 1;
      hello_message.id = mac_address;
      hello_message.protocol_version = 2;

      if (hello_message_serialized == NULL)
        {
          hello_message_serialized = hello_message_serialize (
              &hello_message, (size_t *)&(base_message_rx.size));
          if (!hello_message_serialized)
            {
              ESP_LOGE (TAG, "Failed to serialize hello message");
              return;
            }
        }

      result = base_message_serialize (
          &base_message_rx, base_message_serialized, BASE_MESSAGE_SIZE);
      if (result)
        {
          ESP_LOGE (TAG, "Failed to serialize base message");
          return;
        }

      rc1 = netconn_write (lwipNetconn, base_message_serialized,
                           BASE_MESSAGE_SIZE, NETCONN_NOCOPY);
      if (rc1 != ERR_OK)
        {
          ESP_LOGE (TAG, "netconn failed to send base message");

          continue;
        }
      rc1 = netconn_write (lwipNetconn, hello_message_serialized,
                           base_message_rx.size, NETCONN_NOCOPY);
      if (rc1 != ERR_OK)
        {
          ESP_LOGE (TAG, "netconn failed to send hello message");

          continue;
        }

      ESP_LOGI (TAG, "netconn sent hello message");

      free (hello_message_serialized);
      hello_message_serialized = NULL;

      // init default setting
      scSet.buf_ms = 0;
      scSet.codec = NONE;
      scSet.bits = 0;
      scSet.ch = 0;
      scSet.sr = 0;
      scSet.chkDur_ms = 0;
      scSet.volume = 0;
      scSet.muted = true;

      uint32_t cntTmp = 0;
      uint64_t startTime, endTime;
      char *tmp;
      int32_t remainderSize = 0;
      size_t currentPos = 0;
      size_t typedMsgCurrentPos = 0;
      uint32_t typedMsgLen = 0;
      uint32_t offset = 0;
      size = 0;
      uint32_t tmpData = 0;
      uint32_t *p_tmpData = NULL;
      int32_t shift = 24;

#define BASE_MESSAGE_STATE 0
#define TYPED_MESSAGE_STATE 1

      uint32_t state
          = BASE_MESSAGE_STATE; // 0 ... base message, 1 ... typed message
      uint32_t internalState = 0;
      uint32_t counter = 0;

      firstNetBuf = NULL;

      decoderWriteSemaphore = xSemaphoreCreateMutex ();
      xSemaphoreTake (decoderWriteSemaphore, portMAX_DELAY);

      decoderReadSemaphore = xSemaphoreCreateMutex ();
      xSemaphoreGive (decoderReadSemaphore); // only decoder read callback/task
                                             // can give semaphore

      while (1)
        {
          rc2 = netconn_recv (lwipNetconn, &firstNetBuf);
          if (rc2 != ERR_OK)
            {
              if (rc2 == ERR_CONN)
                {
                  netconn_close (lwipNetconn);

                  // restart and try to reconnect
                  break;
                }

              if (firstNetBuf != NULL)
                {
                  netbuf_delete (firstNetBuf);

                  firstNetBuf = NULL;
                }
              continue;
            }

          // now parse the data
          netbuf_first (firstNetBuf);
          do
            {
              currentPos = 0;

              rc1 = netbuf_data (firstNetBuf, (void **)&start, &len);
              if (rc1 == ERR_OK)
                {
                  // ESP_LOGI (TAG, "netconn rx,"
                  // "data len: %d, %d", len, netbuf_len(firstNetBuf) -
                  // currentPos);
                }
              else
                {
                  ESP_LOGE (TAG, "netconn rx, couldn't get data");

                  continue;
                }

              while (len > 0)
                {
                  rc1 = ERR_OK; // probably not necessary

                  switch (state)
                    {
                    // decode base message
                    case BASE_MESSAGE_STATE:
                      {
                        switch (internalState)
                          {
                          case 0:
                            result = gettimeofday (&now, NULL);
                            // ESP_LOGI(TAG, "time of day: %ld %ld",
                            // now.tv_sec, now.tv_usec);
                            if (result)
                              {
                                ESP_LOGW (TAG, "Failed to gettimeofday");
                              }

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
                            base_message_rx.received.sec |= (*start & 0xFF)
                                                            << 8;
                            internalState++;
                            break;

                          case 16:
                            base_message_rx.received.sec |= (*start & 0xFF)
                                                            << 16;
                            internalState++;
                            break;

                          case 17:
                            base_message_rx.received.sec |= (*start & 0xFF)
                                                            << 24;
                            internalState++;
                            break;

                          case 18:
                            base_message_rx.received.usec = *start & 0xFF;
                            internalState++;
                            break;

                          case 19:
                            base_message_rx.received.usec |= (*start & 0xFF)
                                                             << 8;
                            internalState++;
                            break;

                          case 20:
                            base_message_rx.received.usec |= (*start & 0xFF)
                                                             << 16;
                            internalState++;
                            break;

                          case 21:
                            base_message_rx.received.usec |= (*start & 0xFF)
                                                             << 24;
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

                            base_message_rx.received.sec = now.tv_sec;
                            base_message_rx.received.usec = now.tv_usec;

                            typedMsgCurrentPos = 0;

                            // ESP_LOGI(TAG,"BM type %d ts %d.%d",
                            // base_message_rx.type,
                            // base_message_rx.received.sec,
                            // base_message_rx.received.usec);
                            //								ESP_LOGI(TAG,"%d
                            //%d.%d", base_message_rx.type,
                            // base_message_rx.received.sec,
                            // base_message_rx.received.usec);

                            state = TYPED_MESSAGE_STATE;
                            break;
                          }

                        currentPos++;
                        len--;
                        start++;

                        break;
                      }

                    // decode typed message
                    case TYPED_MESSAGE_STATE:
                      {
                        switch (base_message_rx.type)
                          {
                          case SNAPCAST_MESSAGE_WIRE_CHUNK:
                            {
                              switch (internalState)
                                {
                                case 0:
                                  {
                                    wire_chnk.timestamp.sec = *start & 0xFF;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 1:
                                  {
                                    wire_chnk.timestamp.sec |= (*start & 0xFF)
                                                               << 8;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 2:
                                  {
                                    wire_chnk.timestamp.sec |= (*start & 0xFF)
                                                               << 16;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 3:
                                  {
                                    wire_chnk.timestamp.sec |= (*start & 0xFF)
                                                               << 24;

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

                                case 4:
                                  {
                                    wire_chnk.timestamp.usec = (*start & 0xFF);

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 5:
                                  {
                                    wire_chnk.timestamp.usec |= (*start & 0xFF)
                                                                << 8;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 6:
                                  {
                                    wire_chnk.timestamp.usec |= (*start & 0xFF)
                                                                << 16;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 7:
                                  {
                                    wire_chnk.timestamp.usec |= (*start & 0xFF)
                                                                << 24;

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

                                case 8:
                                  {
                                    wire_chnk.size = (*start & 0xFF);

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 9:
                                  {
                                    wire_chnk.size |= (*start & 0xFF) << 8;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 10:
                                  {
                                    wire_chnk.size |= (*start & 0xFF) << 16;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 11:
                                  {
                                    wire_chnk.size |= (*start & 0xFF) << 24;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    // ESP_LOGI(TAG,
                                    // "got wire chunk with size: %d, at time"
                                    // " %d.%d", wire_chnk.size,
                                    // wire_chnk.timestamp.sec,
                                    // wire_chnk.timestamp.usec);

                                    break;
                                  }

                                case 12:
                                  {
                                    size_t tmp;

                                    if ((base_message_rx.size
                                         - typedMsgCurrentPos)
                                        <= len)
                                      {
                                        tmp = base_message_rx.size
                                              - typedMsgCurrentPos;
                                      }
                                    else
                                      {
                                        tmp = len;
                                      }

                                    if (received_header == true)
                                      {
                                        switch (codec)
                                          {
                                          case FLAC:
                                            {
                                              flacData.bytes = tmp;
                                              flacData.inData = start;
                                              pFlacData = &flacData;

                                              xSemaphoreTake (
                                                  decoderReadSemaphore,
                                                  portMAX_DELAY);

                                              // send data to flac decoder
                                              xQueueSend (decoderReadQHdl,
                                                          &pFlacData,
                                                          portMAX_DELAY);
                                              // and wait until data was
                                              // processed
                                              xSemaphoreTake (
                                                  decoderReadSemaphore,
                                                  portMAX_DELAY);
                                              // need to release mutex
                                              // afterwards for next round
                                              xSemaphoreGive (
                                                  decoderReadSemaphore);

                                              break;
                                            }

                                          case PCM:
                                            {
                                              if (pcmData == NULL)
                                                {
                                                  if (allocate_pcm_chunk_memory (
                                                          &pcmData,
                                                          wire_chnk.size)
                                                      < 0)
                                                    {
                                                      pcmData = NULL;
                                                    }

                                                  offset = 0;
                                                  remainderSize = 0;
                                                  cntTmp = 0;
                                                }

                                              if (pcmData != NULL)
                                                {
                                                  uint32_t *sample;

                                                  int max = 0, begin = 0;

                                                  while (remainderSize)
                                                    {
                                                      tmpData
                                                          |= ((uint32_t)start
                                                                  [begin++]
                                                              << (8
                                                                  * (remainderSize
                                                                     - 1)));

                                                      remainderSize--;
                                                      if (remainderSize < 0)
                                                        {
                                                          ESP_LOGE (
                                                              TAG,
                                                              "shift < 0 this "
                                                              "shouldn't "
                                                              "happen");

                                                          return;
                                                        }
                                                    }

                                                  // check if we need to write
                                                  // a remaining sample
                                                  if (begin > 0)
                                                    {
                                                      // need to reorder bytes
                                                      // in sample for correct
                                                      // playback
                                                      uint8_t dummy1;
                                                      uint32_t dummy2 = 0;

                                                      // TODO: find a more
                                                      // clever way to do this,
                                                      // best would be to
                                                      // actually store it the
                                                      // right way in the first
                                                      // place
                                                      dummy1 = tmpData >> 24;
                                                      dummy2
                                                          |= (uint32_t)dummy1
                                                             << 16;
                                                      dummy1 = tmpData >> 16;
                                                      dummy2
                                                          |= (uint32_t)dummy1
                                                             << 24;
                                                      dummy1 = tmpData >> 8;
                                                      dummy2
                                                          |= (uint32_t)dummy1
                                                             << 0;
                                                      dummy1 = tmpData >> 0;
                                                      dummy2
                                                          |= (uint32_t)dummy1
                                                             << 8;
                                                      tmpData = dummy2;

                                                      sample = (uint32_t *)(&(
                                                          pcmData->fragment
                                                              ->payload
                                                                  [offset]));
                                                      *sample = tmpData;

                                                      offset += 4;
                                                    }

                                                  remainderSize
                                                      = (tmp - begin) % 4;
                                                  max = (tmp - begin)
                                                        - remainderSize;

                                                  for (int i = begin; i < max;
                                                       i += 4)
                                                    {
                                                      // TODO: for now
                                                      // fragmented payload is
                                                      // not supported and the
                                                      // whole chunk is
                                                      // expected to be in the
                                                      // first fragment
                                                      tmpData
                                                          = ((uint32_t)start[i]
                                                             << 16)
                                                            | ((uint32_t)
                                                                   start[i + 1]
                                                               << 24)
                                                            | ((uint32_t)
                                                                   start[i + 2]
                                                               << 0)
                                                            | ((uint32_t)
                                                                   start[i + 3]
                                                               << 8);

                                                      // ensure 32bit alligned
                                                      // write
                                                      sample = (uint32_t *)(&(
                                                          pcmData->fragment
                                                              ->payload
                                                                  [offset]));
                                                      *sample = tmpData;

                                                      offset += 4;
                                                    }

                                                  tmpData = 0;
                                                  while (remainderSize)
                                                    {
                                                      tmpData
                                                          |= ((uint32_t)
                                                                  start[max++]
                                                              << (8
                                                                  * (remainderSize
                                                                     - 1)));

                                                      remainderSize--;

                                                      if (remainderSize < 0)
                                                        {
                                                          ESP_LOGE (
                                                              TAG,
                                                              "shift < 0 this "
                                                              "shouldn't "
                                                              "happen");

                                                          return;
                                                        }
                                                    }

                                                  remainderSize
                                                      = (tmp - begin) % 4;
                                                  if (remainderSize)
                                                    {
                                                      remainderSize
                                                          = 4
                                                            - remainderSize; // these are the still needed bytes for next round
                                                      tmpData
                                                          <<= (8
                                                               * remainderSize); // shift data to correct position
                                                    }
                                                }

                                              break;
                                            }

                                          default:
                                            {
                                              ESP_LOGE (
                                                  TAG,
                                                  "Decoder (1) not supported");

                                              return;

                                              break;
                                            }
                                          }
                                      }

                                    typedMsgCurrentPos += tmp;
                                    start += tmp;
                                    currentPos += tmp;
                                    len -= tmp;

                                    if (typedMsgCurrentPos
                                        >= base_message_rx.size)
                                      {
                                        // ESP_LOGI(TAG,
                                        //"data remaining %d %d", len,
                                        // currentPos);
                                        // ESP_LOGI(TAG, "got wire chunk with
                                        // size: %d, at time %d.%d",
                                        // wire_chnk.size,
                                        // wire_chnk.timestamp.sec,
                                        // wire_chnk.timestamp.usec);

                                        if (received_header == true)
                                          {
                                            switch (codec)
                                              {
                                              case FLAC:
                                                {
                                                  xSemaphoreGive (
                                                      decoderWriteSemaphore);
                                                  // and wait until it is done
                                                  xQueueReceive (
                                                      decoderWriteQHdl,
                                                      &pFlacData,
                                                      portMAX_DELAY);

                                                  if (pFlacData->outData
                                                      != NULL)
                                                    {
                                                      pcmData
                                                          = pFlacData->outData;
                                                      pcmData->timestamp
                                                          = wire_chnk
                                                                .timestamp;

                                                      size_t decodedSize
                                                          = pcmData
                                                                ->totalSize; // pFlacData->bytes;
                                                      scSet.chkDur_ms
                                                          = (1000UL
                                                             * decodedSize)
                                                            / (uint32_t) (
                                                                  scSet.ch
                                                                  * (scSet.bits
                                                                     / 8))
                                                            / scSet.sr;
                                                      if (player_send_snapcast_setting (
                                                              &scSet)
                                                          != pdPASS)
                                                        {
                                                          ESP_LOGE (
                                                              TAG,
                                                              "Failed to "
                                                              "notify "
                                                              "sync task "
                                                              "about "
                                                              "codec. Did you "
                                                              "init player?");

                                                          return;
                                                        }

#if CONFIG_USE_DSP_PROCESSOR
                                                      dsp_setup_flow (
                                                          500, scSet.sr,
                                                          scSet.chkDur_ms);
                                                      dsp_processor (
                                                          pcm_chunk_message
                                                              .payload,
                                                          pcm_chunk_message
                                                              .size,
                                                          dspFlow);
#endif

                                                      insert_pcm_chunk (
                                                          pcmData);
                                                      pcmData = NULL;
                                                    }

                                                  break;
                                                }

                                              case PCM:
                                                {
                                                  size_t decodedSize
                                                      = pcmData->fragment
                                                            ->size;

                                                  pcmData->timestamp
                                                      = wire_chnk.timestamp;

                                                  scSet.chkDur_ms
                                                      = (1000UL * decodedSize)
                                                        / (uint32_t) (
                                                              scSet.ch
                                                              * (scSet.bits
                                                                 / 8))
                                                        / scSet.sr;
                                                  if (player_send_snapcast_setting (
                                                          &scSet)
                                                      != pdPASS)
                                                    {
                                                      ESP_LOGE (
                                                          TAG,
                                                          "Failed to notify "
                                                          "sync task about "
                                                          "codec. Did you "
                                                          "init player?");

                                                      return;
                                                    }

#if CONFIG_USE_DSP_PROCESSOR
                                                  dsp_setup_flow (
                                                      500, scSet.sr,
                                                      scSet.chkDur_ms);
                                                  dsp_processor (
                                                      pcm_chunk_message
                                                          .payload,
                                                      pcm_chunk_message.size,
                                                      dspFlow);
#endif

                                                  insert_pcm_chunk (pcmData);
                                                  pcmData = NULL;
                                                  break;
                                                }

                                              default:
                                                {
                                                  ESP_LOGE (TAG,
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

                                default:
                                  {
                                    ESP_LOGE (TAG, "wire chunk decoder "
                                                   "shouldn't get here");

                                    break;
                                  }
                                }

                              break;
                            }

                          case SNAPCAST_MESSAGE_CODEC_HEADER:
                            {
                              switch (internalState)
                                {
                                case 0:
                                  {
                                    typedMsgLen = *start & 0xFF;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 1:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 8;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 2:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 16;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 3:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 24;

                                    tmp = malloc (typedMsgLen
                                                  + 1); // allocate memory for
                                                        // codec string
                                    if (tmp == NULL)
                                      {
                                        ESP_LOGE (TAG, "couldn't get memory "
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

                                case 4:
                                  {
                                    if (len >= typedMsgLen)
                                      {
                                        memcpy (&tmp[offset], start,
                                                typedMsgLen);

                                        offset += typedMsgLen;

                                        typedMsgCurrentPos += typedMsgLen;
                                        start += typedMsgLen;
                                        currentPos += typedMsgLen;
                                        len -= typedMsgLen;
                                      }
                                    else
                                      {
                                        memcpy (&tmp[offset], start,
                                                typedMsgLen);

                                        offset += len;

                                        typedMsgCurrentPos += len;
                                        start += len;
                                        currentPos += len;
                                        len -= len;
                                      }

                                    if (offset == typedMsgLen)
                                      {
                                        // NULL terminate string
                                        tmp[typedMsgLen] = 0;

                                        // ESP_LOGI
                                        // (TAG, "got codec string: %s", tmp);

                                        if (strcmp (tmp, "opus") == 0)
                                          {
                                            codec = OPUS;
                                          }
                                        else if (strcmp (tmp, "flac") == 0)
                                          {
                                            codec = FLAC;
                                          }
                                        else if (strcmp (tmp, "pcm") == 0)
                                          {
                                            codec = PCM;
                                          }
                                        else
                                          {
                                            codec = NONE;

                                            ESP_LOGI (
                                                TAG,
                                                "Codec : %s not supported",
                                                tmp);
                                            ESP_LOGI (
                                                TAG, "Change encoder codec to "
                                                     "opus, flac or pcm in "
                                                     "/etc/snapserver.conf on "
                                                     "server");

                                            return;
                                          }

                                        free (tmp);
                                        tmp = NULL;

                                        internalState++;
                                      }

                                    break;
                                  }

                                case 5:
                                  {
                                    typedMsgLen = *start & 0xFF;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 6:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 8;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 7:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 16;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 8:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 24;

                                    tmp = malloc (
                                        typedMsgLen); // allocate memory for
                                                      // codec string
                                    if (tmp == NULL)
                                      {
                                        ESP_LOGE (TAG, "couldn't get memory "
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

                                case 9:
                                  {
                                    if (len >= typedMsgLen)
                                      {
                                        memcpy (&tmp[offset], start,
                                                typedMsgLen);

                                        offset += typedMsgLen;

                                        typedMsgCurrentPos += typedMsgLen;
                                        start += typedMsgLen;
                                        currentPos += typedMsgLen;
                                        len -= typedMsgLen;
                                      }
                                    else
                                      {
                                        memcpy (&tmp[offset], start,
                                                typedMsgLen);

                                        offset += len;

                                        typedMsgCurrentPos += len;
                                        start += len;
                                        currentPos += len;
                                        len -= len;
                                      }

                                    if (offset == typedMsgLen)
                                      {
                                        // first ensure everything is set up
                                        // correctly and resources are
                                        // available
                                        if (t_flac_decoder_task != NULL)
                                          {
                                            vTaskDelete (t_flac_decoder_task);
                                            t_flac_decoder_task = NULL;
                                          }

                                        if (flacDecoder != NULL)
                                          {
                                            FLAC__stream_decoder_finish (
                                                flacDecoder);
                                            FLAC__stream_decoder_delete (
                                                flacDecoder);
                                            flacDecoder = NULL;
                                          }

                                        if (decoderWriteQHdl != NULL)
                                          {
                                            vQueueDelete (decoderWriteQHdl);
                                            decoderWriteQHdl = NULL;
                                          }

                                        if (decoderReadQHdl != NULL)
                                          {
                                            vQueueDelete (decoderReadQHdl);
                                            decoderReadQHdl = NULL;
                                          }

                                        if (codec == OPUS)
                                          {
                                            ESP_LOGI (
                                                TAG,
                                                "OPUS not implemented yet");

                                            return;
                                          }
                                        else if (codec == FLAC)
                                          {
                                            if (t_flac_decoder_task == NULL)
                                              {
                                                xTaskCreatePinnedToCore (
                                                    &flac_decoder_task,
                                                    "flac_decoder_task",
                                                    9 * 256, &scSet,
                                                    FLAC_TASK_PRIORITY,
                                                    &t_flac_decoder_task,
                                                    FLAC_TASK_CORE_ID);
                                              }

                                            if (flacData.outData != NULL)
                                              {
                                                free (flacData.outData);
                                                flacData.outData = NULL;
                                              }

                                            flacData.bytes = typedMsgLen;
                                            flacData.inData = tmp;
                                            pFlacData = &flacData;

                                            // TODO: find a smarter way for
                                            // this wait for task creation done
                                            while (decoderReadQHdl == NULL)
                                              {
                                                vTaskDelay (10);
                                              }

                                            xSemaphoreTake (
                                                decoderReadSemaphore,
                                                portMAX_DELAY);

                                            // send data to flac decoder
                                            xQueueSend (decoderReadQHdl,
                                                        &pFlacData,
                                                        portMAX_DELAY);
                                            // and wait until data was
                                            // processed
                                            xSemaphoreTake (
                                                decoderReadSemaphore,
                                                portMAX_DELAY);
                                            // need to release mutex afterwards
                                            // for next round
                                            xSemaphoreGive (
                                                decoderReadSemaphore);
                                            // wait until it is done
                                            xQueueReceive (decoderWriteQHdl,
                                                           &pFlacData,
                                                           portMAX_DELAY);

                                            ESP_LOGI (
                                                TAG,
                                                "fLaC sampleformat: %d:%d:%d",
                                                scSet.sr, scSet.bits,
                                                scSet.ch);
                                          }
                                        else if (codec == PCM)
                                          {
                                            memcpy (&channels, tmp + 22,
                                                    sizeof (channels));
                                            uint32_t rate;
                                            memcpy (&rate, tmp + 24,
                                                    sizeof (rate));
                                            uint16_t bits;
                                            memcpy (&bits, tmp + 34,
                                                    sizeof (bits));

                                            scSet.codec = codec;
                                            scSet.bits = bits;
                                            scSet.ch = channels;
                                            scSet.sr = rate;

                                            ESP_LOGI (
                                                TAG,
                                                "pcm sampleformat: %d:%d:%d",
                                                scSet.sr, scSet.bits,
                                                scSet.ch);
                                          }
                                        else
                                          {
                                            ESP_LOGE (
                                                TAG,
                                                "codec header decoder "
                                                "shouldn't get here after "
                                                "codec string was detected");

                                            return;
                                          }

                                        free (tmp);
                                        tmp = NULL;

                                        //											  ESP_LOGI(TAG,
                                        //"done codec header msg");

                                        trx.tv_sec = base_message_rx.sent.sec;
                                        trx.tv_usec
                                            = base_message_rx.sent.usec;
                                        // we do this, so uint32_t timvals
                                        // won't overflow if e.g. raspberry
                                        // server is off to far
                                        settimeofday (&trx, NULL);
                                        ESP_LOGI (TAG,
                                                  "syncing clock to server "
                                                  "%ld.%06ld",
                                                  trx.tv_sec, trx.tv_usec);

                                        state = BASE_MESSAGE_STATE;
                                        internalState = 0;

                                        received_header = true;
                                      }

                                    break;
                                  }

                                default:
                                  {
                                    ESP_LOGE (TAG, "codec header decoder "
                                                   "shouldn't get here");

                                    break;
                                  }
                                }

                              break;
                            }

                          case SNAPCAST_MESSAGE_SERVER_SETTINGS:
                            {
                              switch (internalState)
                                {
                                case 0:
                                  {
                                    while (
                                        (netbuf_len (firstNetBuf) - currentPos)
                                        < base_message_rx.size)
                                      {
                                        ESP_LOGI (TAG, "need more data");

                                        // we need more data to process
                                        rc1 = netconn_recv (lwipNetconn,
                                                            &newNetBuf);
                                        if (rc1 != ERR_OK)
                                          {
                                            ESP_LOGE (
                                                TAG,
                                                "rx error for need more data");

                                            if (rc1 == ERR_CONN)
                                              {
                                                // netconn_close(lwipNetconn);
                                                // closing later, see first
                                                // netconn_recv() in the loop

                                                break;
                                              }

                                            if (newNetBuf != NULL)
                                              {
                                                netbuf_delete (newNetBuf);

                                                newNetBuf = NULL;
                                              }

                                            continue;
                                          }

                                        netbuf_chain (firstNetBuf, newNetBuf);
                                      }

                                    if (rc1 == ERR_OK)
                                      {
                                        typedMsgLen = *start & 0xFF;

                                        typedMsgCurrentPos++;
                                        start++;
                                        currentPos++;
                                        len--;

                                        internalState++;
                                      }
                                    else
                                      {
                                        ESP_LOGE (TAG, "some error");
                                      }

                                    break;
                                  }

                                case 1:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 8;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 2:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 16;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 3:
                                  {
                                    typedMsgLen |= (*start & 0xFF) << 24;

                                    // ESP_LOGI(TAG,
                                    // "server settings string is %d long",
                                    // typedMsgLen);

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 4:
                                  {
                                    // now get some memory for server settings
                                    // string at this point there is still
                                    // plenty of RAM available, so we use
                                    // malloc and netbuf_copy() here
                                    tmp = malloc (typedMsgLen + 1);

                                    if (tmp == NULL)
                                      {
                                        ESP_LOGE (TAG,
                                                  "couldn't get memory for "
                                                  "server settings string");
                                      }
                                    else
                                      {
                                        netbuf_copy_partial (firstNetBuf, tmp,
                                                             typedMsgLen,
                                                             currentPos);

                                        tmp[typedMsgLen]
                                            = 0; // NULL terminate string

                                        //									  			  ESP_LOGI
                                        //(TAG, "got string: %s", tmp);

                                        result
                                            = server_settings_message_deserialize (
                                                &server_settings_message, tmp);
                                        if (result)
                                          {
                                            ESP_LOGE (TAG,
                                                      "Failed to read server "
                                                      "settings: %d",
                                                      result);
                                          }
                                        else
                                          {
                                            // log mute state, buffer, latency
                                            ESP_LOGI (TAG,
                                                      "Buffer length:  %d",
                                                      server_settings_message
                                                          .buffer_ms);
                                            ESP_LOGI (TAG,
                                                      "Latency:        %d",
                                                      server_settings_message
                                                          .latency);
                                            ESP_LOGI (
                                                TAG, "Mute:           %d",
                                                server_settings_message.muted);
                                            ESP_LOGI (TAG,
                                                      "Setting volume: %d",
                                                      server_settings_message
                                                          .volume);
                                          }

                                        // Volume setting using ADF HAL
                                        // abstraction
                                        if (scSet.muted
                                            != server_settings_message.muted)
                                          {
                                            audio_hal_set_mute (
                                                board_handle->audio_hal,
                                                server_settings_message.muted);
                                          }
                                        if (scSet.volume
                                            != server_settings_message.volume)
                                          {
                                            audio_hal_set_volume (
                                                board_handle->audio_hal,
                                                server_settings_message
                                                    .volume);
                                          }

                                        scSet.cDacLat_ms
                                            = server_settings_message.latency;
                                        scSet.buf_ms = server_settings_message
                                                           .buffer_ms;
                                        scSet.muted
                                            = server_settings_message.muted;
                                        scSet.volume
                                            = server_settings_message.volume;

                                        if (player_send_snapcast_setting (
                                                &scSet)
                                            != pdPASS)
                                          {
                                            ESP_LOGE (
                                                TAG,
                                                "Failed to notify sync task. "
                                                "Did you init player?");

                                            return;
                                          }

                                        free (tmp);
                                        tmp = NULL;
                                      }

                                    internalState++;

                                    // currentPos++;
                                    // len--;
                                    //
                                    // break;

                                    // intentional fall through
                                  }

                                case 5:
                                  {
                                    size_t tmpSize = base_message_rx.size
                                                     - typedMsgCurrentPos;

                                    if (len > 0)
                                      {
                                        if (tmpSize < len)
                                          {
                                            start += tmpSize;
                                            currentPos
                                                += tmpSize; // will be
                                                            // incremented by 1
                                                            // later so -1 here
                                            typedMsgCurrentPos += tmpSize;
                                            len -= tmpSize;
                                          }
                                        else
                                          {
                                            start += len;
                                            currentPos
                                                += len; // will be incremented
                                                        // by 1 later so -1
                                                        // here
                                            typedMsgCurrentPos += len;
                                            len = 0;
                                          }
                                      }

                                    if (typedMsgCurrentPos
                                        >= base_message_rx.size)
                                      {
                                        // ESP_LOGI(TAG,
                                        // "done server settings");

                                        state = BASE_MESSAGE_STATE;
                                        internalState = 0;

                                        typedMsgCurrentPos = 0;
                                      }

                                    break;
                                  }

                                default:
                                  {
                                    ESP_LOGE (TAG, "server settings decoder "
                                                   "shouldn't get here");

                                    break;
                                  }
                                }

                              break;
                            }

                          case SNAPCAST_MESSAGE_STREAM_TAGS:
                            {
                              size_t tmpSize
                                  = base_message_rx.size - typedMsgCurrentPos;

                              if (tmpSize < len)
                                {
                                  start += tmpSize;
                                  currentPos += tmpSize;
                                  typedMsgCurrentPos += tmpSize;
                                  len -= tmpSize;
                                }
                              else
                                {
                                  start += len;
                                  currentPos += len;

                                  typedMsgCurrentPos += len;
                                  len = 0;
                                }

                              if (typedMsgCurrentPos >= base_message_rx.size)
                                {
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

                          case SNAPCAST_MESSAGE_TIME:
                            {
                              switch (internalState)
                                {
                                case 0:
                                  {
                                    time_message_rx.latency.sec = *start;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 1:
                                  {
                                    time_message_rx.latency.sec
                                        |= (int32_t)*start << 8;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 2:
                                  {
                                    time_message_rx.latency.sec
                                        |= (int32_t)*start << 16;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 3:
                                  {
                                    time_message_rx.latency.sec
                                        |= (int32_t)*start << 24;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 4:
                                  {
                                    time_message_rx.latency.usec = *start;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 5:
                                  {
                                    time_message_rx.latency.usec
                                        |= (int32_t)*start << 8;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 6:
                                  {
                                    time_message_rx.latency.usec
                                        |= (int32_t)*start << 16;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    internalState++;

                                    break;
                                  }

                                case 7:
                                  {
                                    time_message_rx.latency.usec
                                        |= (int32_t)*start << 24;

                                    typedMsgCurrentPos++;
                                    start++;
                                    currentPos++;
                                    len--;

                                    if (typedMsgCurrentPos
                                        >= base_message_rx.size)
                                      {
                                        // ESP_LOGI(TAG,
                                        // "done time message");

                                        typedMsgCurrentPos = 0;

                                        state = BASE_MESSAGE_STATE;
                                        internalState = 0;

                                        // ESP_LOGI(TAG, "BaseTX :"
                                        // "%d %d ", base_message.sent.sec,
                                        // base_message.sent.usec);
                                        // ESP_LOGI(TAG, "BaseRX
                                        // ": %d %d ",
                                        // base_message.received.sec ,
                                        // base_message.received.usec);
                                        // ESP_LOGI(TAG, "baseTX->RX : %d s ",
                                        // (base_message.received.sec
                                        // -
                                        //			base_message.sent.sec));
                                        // ESP_LOGI(TAG,
                                        // "baseTX->RX : %d ms ",
                                        //			(base_message.received.usec
                                        //-
                                        // base_message.sent.usec)/1000);
                                        // ESP_LOGI(TAG,
                                        // "Latency : %d.%d ",
                                        // time_message.latency.sec,
                                        // time_message_rx.latency.usec/1000);

                                        // tv == server to client latency (s2c)
                                        // time_message_rx.latency == client to
                                        // server latency(c2s)
                                        // TODO the fact that I have to do this
                                        // simple conversion means I should
                                        // probably use the timeval struct
                                        // instead of my own
                                        trx.tv_sec
                                            = base_message_rx.received.sec;
                                        trx.tv_usec
                                            = base_message_rx.received.usec;
                                        ttx.tv_sec = base_message_rx.sent.sec;
                                        ttx.tv_usec
                                            = base_message_rx.sent.usec;
                                        timersub (&trx, &ttx, &tdif);

                                        trx.tv_sec
                                            = time_message_rx.latency.sec;
                                        trx.tv_usec
                                            = time_message_rx.latency.usec;

                                        // trx == c2s: client to server
                                        // tdif == s2c: server to client
                                        //                    ESP_LOGI(TAG,
                                        //                    "c2s: %ld %ld",
                                        //                    trx.tv_sec,
                                        //                    trx.tv_usec);
                                        //                    ESP_LOGI(TAG,
                                        //                    "s2c:  %ld %ld",
                                        //                    tdif.tv_sec,
                                        //                    tdif.tv_usec);

                                        timersub (&trx, &tdif,
                                                  &tmpDiffToServer);
                                        if ((tmpDiffToServer.tv_sec / 2) == 0)
                                          {
                                            tmpDiffToServer.tv_sec = 0;
                                            tmpDiffToServer.tv_usec
                                                = (suseconds_t) (
                                                      (int64_t)tmpDiffToServer
                                                          .tv_sec
                                                      * 1000000LL / 2)
                                                  + (int64_t)tmpDiffToServer
                                                            .tv_usec
                                                        / 2;
                                          }
                                        else
                                          {
                                            tmpDiffToServer.tv_sec /= 2;
                                            tmpDiffToServer.tv_usec /= 2;
                                          }

                                        //							                   ESP_LOGI(TAG,
                                        //							                   "Current
                                        // latency: %ld.%06ld",
                                        // tmpDiffToServer.tv_sec,
                                        //							                   tmpDiffToServer.tv_usec);

                                        // TODO: Move the time message sending
                                        // to an own thread maybe following
                                        // code is storing / initializing /
                                        // resetting diff to server algorithm
                                        // we collect a number of latencies and
                                        // apply a median filter. Based on
                                        // these we can get server now
                                        {
                                          struct timeval diff;
                                          int64_t newValue;

                                          // clear diffBuffer if last update is
                                          // older than a minute
                                          timersub (&now, &lastTimeSync,
                                                    &diff);

                                          if (diff.tv_sec > 60)
                                            {
                                              ESP_LOGW (
                                                  TAG, "Last time sync older "
                                                       "than a minute. "
                                                       "Clearing time buffer");

                                              reset_latency_buffer ();
                                            }

                                          newValue
                                              = ((int64_t)
                                                         tmpDiffToServer.tv_sec
                                                     * 1000000LL
                                                 + (int64_t)tmpDiffToServer
                                                       .tv_usec);
                                          player_latency_insert (newValue);

                                          //                    ESP_LOGE(TAG,
                                          //                    "latency %lld",
                                          //                    newValue);

                                          // store current time
                                          lastTimeSync.tv_sec = now.tv_sec;
                                          lastTimeSync.tv_usec = now.tv_usec;

                                          if (xSemaphoreTake (
                                                  timeSyncSemaphoreHandle, 0)
                                              == pdTRUE)
                                            {
                                              ESP_LOGW (
                                                  TAG,
                                                  "couldn't take "
                                                  "timeSyncSemaphoreHandle");
                                            }

                                          uint64_t timeout;
                                          if (latency_buffer_full () > 0)
                                            {
                                              // we give
                                              // timeSyncSemaphoreHandle after
                                              // x µs through timer
                                              // TODO: maybe start a periodic
                                              // timer here, but we need to
                                              // remember if it is already
                                              // running then. also we need to
                                              // stop it if
                                              // reset_latency_buffer() was
                                              // called
                                              timeout = 1000000;
                                            }
                                          else
                                            {
                                              // Do a initial time sync with
                                              // the server at boot we need to
                                              // fill diffBuff fast so we get a
                                              // good estimate of latency
                                              timeout = 100000;
                                            }

                                          esp_timer_start_once (
                                              timeSyncMessageTimer, timeout);
                                        }
                                      }
                                    else
                                      {
                                        ESP_LOGE (TAG,
                                                  "error time message, this "
                                                  "shouldn't happen! %d %d",
                                                  typedMsgCurrentPos,
                                                  base_message_rx.size);

                                        typedMsgCurrentPos = 0;

                                        state = BASE_MESSAGE_STATE;
                                        internalState = 0;
                                      }

                                    break;
                                  }

                                default:
                                  {
                                    ESP_LOGE (TAG,
                                              "time message decoder shouldn't "
                                              "get here %d %d %d",
                                              typedMsgCurrentPos,
                                              base_message_rx.size,
                                              internalState);

                                    break;
                                  }
                                }

                              break;
                            }

                          default:
                            {
                              typedMsgCurrentPos++;
                              start++;
                              currentPos++;
                              len--;

                              if (typedMsgCurrentPos >= base_message_rx.size)
                                {
                                  ESP_LOGI (TAG,
                                            "done unknown typed message %d",
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

                    default:
                      {
                        break;
                      }
                    }

                  if (rc1 != ERR_OK)
                    {
                      break;
                    }
                }
            }
          while (netbuf_next (firstNetBuf) >= 0);

          netbuf_delete (firstNetBuf);

          if (rc1 != ERR_OK)
            {
              ESP_LOGE (TAG, "Data error, closing netconn");

              netconn_close (lwipNetconn);

              break;
            }

          if (received_header == true)
            {
              if (xSemaphoreTake (timeSyncSemaphoreHandle, 0) == pdTRUE)
                {
                  result = gettimeofday (&now, NULL);
                  // ESP_LOGI(TAG, "time of day: %ld %ld", now.tv_sec,
                  // now.tv_usec);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to gettimeofday");
                      continue;
                    }

                  base_message_tx.type = SNAPCAST_MESSAGE_TIME;
                  base_message_tx.id = id_counter++;
                  base_message_tx.refersTo = 0;
                  base_message_tx.received.sec = 0;
                  base_message_tx.received.usec = 0;
                  base_message_tx.sent.sec = now.tv_sec;
                  base_message_tx.sent.usec = now.tv_usec;
                  base_message_tx.size = TIME_MESSAGE_SIZE;

                  result = base_message_serialize (&base_message_tx,
                                                   base_message_serialized,
                                                   BASE_MESSAGE_SIZE);
                  if (result)
                    {
                      ESP_LOGE (TAG,
                                "Failed to serialize base message for time");
                      continue;
                    }

                  memset (&time_message_tx, 0, sizeof (time_message_tx));

                  result = time_message_serialize (&time_message_tx,
                                                   time_message_serialized,
                                                   TIME_MESSAGE_SIZE);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to serialize time message");
                      continue;
                    }

                  rc1 = netconn_write (lwipNetconn, base_message_serialized,
                                       BASE_MESSAGE_SIZE, NETCONN_NOCOPY);
                  if (rc1 != ERR_OK)
                    {
                      ESP_LOGW (TAG, "error writing timesync base msg");

                      continue;
                    }

                  rc1 = netconn_write (lwipNetconn, time_message_serialized,
                                       TIME_MESSAGE_SIZE, NETCONN_NOCOPY);
                  if (rc1 != ERR_OK)
                    {
                      ESP_LOGW (TAG, "error writing timesync msg");

                      continue;
                    }
                }
            }
        }
    }
}

/**
 *
 */
static void
http_get_task_backup (void *pvParameters)
{
  struct sockaddr_in servaddr;
  char *start;
  int sock = -1;
  base_message_t base_message;
  hello_message_t hello_message;
  char *hello_message_serialized = NULL;
  int result, size, id_counter;
  struct timeval now, trx, tdif, ttx;
  time_message_t time_message;
  struct timeval tmpDiffToServer;
  struct timeval lastTimeSync = { 0, 0 };
  esp_timer_handle_t timeSyncMessageTimer = NULL;
  int16_t frameSize = 960; // 960*2: 20ms, 960*1: 10ms
  int16_t *audio = NULL;
  int16_t pcm_size = 120;
  uint16_t channels;
  esp_err_t err = 0;
  codec_header_message_t codec_header_message;
  server_settings_message_t server_settings_message;
  bool received_header = false;
  mdns_result_t *r;
  OpusDecoder *opusDecoder = NULL;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  flacData_t flacData = { NULL, NULL, 0 };
  flacData_t *pFlacData;
  char *typedMsg = NULL;
  uint32_t lastTypedMsgSize = 0;

  // create a timer to send time sync messages every x µs
  esp_timer_create (&tSyncArgs, &timeSyncMessageTimer);
  timeSyncSemaphoreHandle = xSemaphoreCreateMutex ();
  xSemaphoreGive (timeSyncSemaphoreHandle);

  id_counter = 0;

  while (1)
    {
      if (reset_latency_buffer () < 0)
        {
          ESP_LOGE (
              TAG,
              "reset_diff_buffer: couldn't reset median filter long. STOP");

          return;
        }

      esp_timer_stop (timeSyncMessageTimer);
      xSemaphoreGive (timeSyncSemaphoreHandle);

      if (opusDecoder != NULL)
        {
          opus_decoder_destroy (opusDecoder);
          opusDecoder = NULL;
        }

      if (t_flac_decoder_task != NULL)
        {
          vTaskDelete (t_flac_decoder_task);
          t_flac_decoder_task = NULL;
        }

      if (flacDecoder != NULL)
        {
          FLAC__stream_decoder_finish (flacDecoder);
          FLAC__stream_decoder_delete (flacDecoder);
          flacDecoder = NULL;
        }

      if (decoderWriteQHdl != NULL)
        {
          vQueueDelete (decoderWriteQHdl);
          decoderWriteQHdl = NULL;
        }

      if (decoderReadQHdl != NULL)
        {
          vQueueDelete (decoderReadQHdl);
          decoderReadQHdl = NULL;
        }

#if SNAPCAST_SERVER_USE_MDNS
      // Find snapcast server
      // Connect to first snapcast server found
      r = NULL;
      err = 0;
      while (!r || err)
        {
          ESP_LOGI (TAG, "Lookup snapcast service on network");
          esp_err_t err = mdns_query_ptr ("_snapcast", "_tcp", 3000, 20, &r);
          if (err)
            {
              ESP_LOGE (TAG, "Query Failed");
            }

          if (!r)
            {
              ESP_LOGW (TAG, "No results found!");
            }

          vTaskDelay (1000 / portTICK_PERIOD_MS);
        }

      char serverAddr[] = "255.255.255.255";
      ESP_LOGI (TAG, "Found %s:%d",
                inet_ntop (AF_INET, &(r->addr->addr.u_addr.ip4.addr),
                           serverAddr, sizeof (serverAddr)),
                r->port);

      servaddr.sin_family = AF_INET;
      servaddr.sin_addr.s_addr = r->addr->addr.u_addr.ip4.addr;
      servaddr.sin_port = htons (r->port);
      mdns_query_results_free (r);
#else
      // configure a failsafe snapserver according to CONFIG values
      servaddr.sin_family = AF_INET;
      inet_pton (AF_INET, SNAPCAST_SERVER_HOST, &(servaddr.sin_addr.s_addr));
      servaddr.sin_port = htons (SNAPCAST_SERVER_PORT);
#endif
      ESP_LOGI (TAG, "allocate socket");
      sock = socket (AF_INET, SOCK_STREAM, 0);

      if (sock < 0)
        {
          ESP_LOGE (TAG, "... Failed to allocate socket.");
          vTaskDelay (1000 / portTICK_PERIOD_MS);
          continue;
        }
      ESP_LOGI (TAG, "... allocated socket %d", sock);

      ESP_LOGI (TAG, "connect to socket");
      err = connect (sock, (struct sockaddr *)&servaddr,
                     sizeof (struct sockaddr_in));
      if (err < 0)
        {
          ESP_LOGE (TAG, "%s, %d", strerror (errno), errno);

          shutdown (sock, 2);
          closesocket (sock);

          vTaskDelay (4000 / portTICK_PERIOD_MS);

          continue;
        }

      ESP_LOGI (TAG, "... connected");

      result = gettimeofday (&now, NULL);
      if (result)
        {
          ESP_LOGI (TAG, "Failed to gettimeofday\r\n");
          return;
        }

      received_header = false;

      // init base message
      base_message.type = SNAPCAST_MESSAGE_HELLO;
      base_message.id = 0x0000;
      base_message.refersTo = 0x0000;
      base_message.sent.sec = now.tv_sec;
      base_message.sent.usec = now.tv_usec;
      base_message.received.sec = 0;
      base_message.received.usec = 0;
      base_message.size = 0x00000000;

      // init hello message
      hello_message.mac = mac_address;
      hello_message.hostname = SNAPCAST_CLIENT_NAME;
      hello_message.version = (char *)VERSION_STRING;
      hello_message.client_name = SNAPCAST_CLIENT_NAME;
      hello_message.os = "esp32";
      hello_message.arch = "xtensa";
      hello_message.instance = 1;
      hello_message.id = mac_address;
      hello_message.protocol_version = 2;

      if (hello_message_serialized == NULL)
        {
          hello_message_serialized = hello_message_serialize (
              &hello_message, (size_t *)&(base_message.size));
          if (!hello_message_serialized)
            {
              ESP_LOGE (TAG, "Failed to serialize hello message\r\b");
              return;
            }
        }

      result = base_message_serialize (&base_message, base_message_serialized,
                                       BASE_MESSAGE_SIZE);
      if (result)
        {
          ESP_LOGE (TAG, "Failed to serialize base message\r\n");
          return;
        }

      result = send (sock, base_message_serialized, BASE_MESSAGE_SIZE, 0);
      if (result < 0)
        {
          ESP_LOGW (TAG, "error writing base msg to socket: %s",
                    strerror (errno));

          free (hello_message_serialized);
          hello_message_serialized = NULL;

          shutdown (sock, 2);
          closesocket (sock);

          continue;
        }

      result = send (sock, hello_message_serialized, base_message.size, 0);
      if (result < 0)
        {
          ESP_LOGW (TAG, "error writing hello msg to socket: %s",
                    strerror (errno));

          free (hello_message_serialized);
          hello_message_serialized = NULL;

          shutdown (sock, 2);
          closesocket (sock);

          continue;
        }

      free (hello_message_serialized);
      hello_message_serialized = NULL;

      // init default setting
      scSet.buf_ms = 0;
      scSet.codec = NONE;
      scSet.bits = 0;
      scSet.ch = 0;
      scSet.sr = 0;
      scSet.chkDur_ms = 0;
      scSet.volume = 0;
      scSet.muted = true;

      lastTypedMsgSize = 0;
      if (typedMsg)
        {
          free (typedMsg);
          typedMsg = NULL;
        }

      uint64_t startTime, endTime;

      for (;;)
        {
          //    	  ESP_LOGW (TAG, "stack free: %d",
          //    uxTaskGetStackHighWaterMark(NULL));

          size = 0;
          result = 0;
          while (size < BASE_MESSAGE_SIZE)
            {
              result = recv (sock, &(base_message_serialized[size]),
                             BASE_MESSAGE_SIZE - size, 0);
              if (result < 0)
                {
                  break;
                }
              size += result;
            }

          if (result < 0)
            {
              if (errno != 0)
                {
                  ESP_LOGW (TAG, "1: %s, %d", strerror (errno), (int)errno);
                }

              shutdown (sock, 2);
              closesocket (sock);

              break; // stop for(;;) will try to reconnect then
            }

          if (result > 0)
            {
              result = gettimeofday (&now, NULL);
              // ESP_LOGI(TAG, "time of day: %ld %ld", now.tv_sec,
              // now.tv_usec);
              if (result)
                {
                  ESP_LOGW (TAG, "Failed to gettimeofday");
                  continue;
                }

              result = base_message_deserialize (
                  &base_message, base_message_serialized, size);
              if (result)
                {
                  ESP_LOGW (TAG, "Failed to read base message: %d", result);
                  continue;
                }

              base_message.received.usec = now.tv_usec;
              //               ESP_LOGI(TAG,"%d %d : %d %d : %d
              //               %d",base_message.size,
              //                          				base_message.refersTo,
              //              					base_message.sent.sec,
              //              					base_message.sent.usec,
              //              					base_message.received.sec,
              //              					base_message.received.usec
              //              );

              // TODO: ensure this buffer is freed before task gets deleted
              size = 0;
              if (lastTypedMsgSize < base_message.size)
                {
                  //            	  typedMsg = (char *)heap_caps_realloc
                  //            (typedMsg, base_message.size, MALLOC_CAP_8BIT);
                  //				  if (typedMsg == NULL)
                  {
                    //            	  	  ESP_LOGI (TAG, "get memory
                    //            for typed message %d, %d, %d",
                    //            base_message.size,
                    //            heap_caps_get_free_size(MALLOC_CAP_8BIT),
                    //            heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                    typedMsg = (char *)heap_caps_realloc (
                        typedMsg, base_message.size, MALLOC_CAP_8BIT);

                    //            	  	  typedMsg = (char
                    //            *)heap_caps_malloc (base_message.size,
                    //            MALLOC_CAP_8BIT);
                    if (typedMsg == NULL)
                      {
                        ESP_LOGE (
                            TAG,
                            "Couldn't get memory for typed message %d, %d, %d",
                            base_message.size,
                            heap_caps_get_free_size (MALLOC_CAP_8BIT),
                            heap_caps_get_largest_free_block (
                                MALLOC_CAP_8BIT));

                        // dummy read next data to a char variable without
                        // incrementing and drop it (base_message.size)
                        char dummy;
                        start = &dummy;
                        while (size < base_message.size)
                          {
                            result = recv (sock, start, 1, 0);
                            if (result < 0)
                              {
                                ESP_LOGW (TAG,
                                          "Failed to read from server: %d",
                                          result);

                                break;
                              }

                            size++;
                          }

                        continue;
                      }
                    else
                      {
                        lastTypedMsgSize = base_message.size;
                      }
                  }
                }
              start = typedMsg;

              while (size < base_message.size)
                {
                  result = recv (sock, &(start[size]),
                                 base_message.size - size, 0);
                  if (result < 0)
                    {
                      ESP_LOGW (TAG, "Failed to read from server: %d", result);

                      break;
                    }

                  size += result;
                }

              if (result < 0)
                {
                  if (errno != 0)
                    {
                      ESP_LOGI (TAG, "2: %s, %d", strerror (errno),
                                (int)errno);
                    }

                  shutdown (sock, 2);
                  closesocket (sock);

                  break; // stop for(;;) will try to reconnect then
                }

              switch (base_message.type)
                {
                case SNAPCAST_MESSAGE_CODEC_HEADER:
                  result = codec_header_message_deserialize (
                      &codec_header_message, start, size);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to read codec header: %d",
                                result);
                      return;
                    }

                  size = codec_header_message.size;
                  start = codec_header_message.payload;

                  if (opusDecoder != NULL)
                    {
                      opus_decoder_destroy (opusDecoder);
                      opusDecoder = NULL;
                    }

                  if (t_flac_decoder_task != NULL)
                    {
                      vTaskDelete (t_flac_decoder_task);
                      t_flac_decoder_task = NULL;
                    }

                  if (flacDecoder != NULL)
                    {
                      FLAC__stream_decoder_finish (flacDecoder);
                      FLAC__stream_decoder_delete (flacDecoder);
                      flacDecoder = NULL;
                    }

                  if (decoderWriteQHdl != NULL)
                    {
                      vQueueDelete (decoderWriteQHdl);
                      decoderWriteQHdl = NULL;
                    }

                  if (decoderReadQHdl != NULL)
                    {
                      vQueueDelete (decoderReadQHdl);
                      decoderReadQHdl = NULL;
                    }

                  // ESP_LOGI(TAG, "Received codec header message with size
                  // %d", codec_header_message.size);

                  if (strcmp (codec_header_message.codec, "opus") == 0)
                    {
                      uint32_t rate;
                      memcpy (&rate, start + 4, sizeof (rate));
                      uint16_t bits;
                      memcpy (&bits, start + 8, sizeof (bits));
                      memcpy (&channels, start + 10, sizeof (channels));
                      ESP_LOGI (TAG, "%s sampleformat: %d:%d:%d",
                                codec_header_message.codec, rate, bits,
                                channels);

                      if (audio != NULL)
                        {
                          free (audio);
                          audio = NULL;
                        }

                      if (flacData.outData != NULL)
                        {
                          free (flacData.outData);
                          flacData.outData = NULL;
                        }

                      int error = 0;
                      opusDecoder
                          = opus_decoder_create (rate, channels, &error);
                      if (error != 0)
                        {
                          ESP_LOGE (TAG, "Failed to init %s decoder",
                                    codec_header_message.codec);
                          return;
                        }
                      else
                        {
                          ESP_LOGI (TAG, "Initialized %s decoder",
                                    codec_header_message.codec);

                          codec = OPUS;

                          scSet.codec = codec;
                          scSet.bits = bits;
                          scSet.ch = channels;
                          scSet.sr = rate;
                        }
                    }
                  else if (strcmp (codec_header_message.codec, "flac") == 0)
                    {
                      codec = FLAC;

                      if (t_flac_decoder_task == NULL)
                        {
                          xTaskCreatePinnedToCore (
                              &flac_decoder_task, "flac_decoder_task",
                              4 * 4096, &scSet, FLAC_TASK_PRIORITY,
                              &t_flac_decoder_task, FLAC_TASK_CORE_ID);
                        }

                      // check if audio buffer was previously allocated by some
                      // other codec this would happen if codec is changed
                      // while client was running
                      if (audio != NULL)
                        {
                          free (audio);
                          audio = NULL;
                        }

                      if (flacData.outData != NULL)
                        {
                          free (flacData.outData);
                          flacData.outData = NULL;
                        }

                      flacData.bytes = codec_header_message.size;
                      flacData.inData = codec_header_message.payload;
                      pFlacData = &flacData;

                      // wait for task creation done
                      while (decoderReadQHdl == NULL)
                        {
                          vTaskDelay (10);
                        }

                      // send data to flac decoder
                      xQueueSend (decoderReadQHdl, &pFlacData, portMAX_DELAY);
                      // and wait until it is done
                      xQueueReceive (decoderWriteQHdl, &pFlacData,
                                     portMAX_DELAY);

                      ESP_LOGI (TAG, "%s sampleformat: %d:%d:%d",
                                codec_header_message.codec, scSet.sr,
                                scSet.bits, scSet.ch);
                    }
                  else if (strcmp (codec_header_message.codec, "pcm") == 0)
                    {
                      codec = PCM;

                      if (audio != NULL)
                        {
                          free (audio);
                          audio = NULL;
                        }

                      if (flacData.outData != NULL)
                        {
                          free (flacData.outData);
                          flacData.outData = NULL;
                        }

                      memcpy (&channels, start + 22, sizeof (channels));
                      uint32_t rate;
                      memcpy (&rate, start + 24, sizeof (rate));
                      uint16_t bits;
                      memcpy (&bits, start + 34, sizeof (bits));

                      ESP_LOGI (TAG, "%s sampleformat: %d:%d:%d",
                                codec_header_message.codec, rate, bits,
                                channels);

                      scSet.codec = codec;
                      scSet.bits = bits;
                      scSet.ch = channels;
                      scSet.sr = rate;
                    }
                  else
                    {
                      codec = NONE;

                      ESP_LOGI (TAG, "Codec : %s not supported",
                                codec_header_message.codec);
                      ESP_LOGI (TAG,
                                "Change encoder codec to opus / flac / pcm in "
                                "/etc/snapserver.conf "
                                "on server");
                      return;
                    }

                  trx.tv_sec = base_message.sent.sec;
                  trx.tv_usec = base_message.sent.usec;
                  // we do this, so uint32_t timvals won't overflow
                  // if e.g. raspberry server is off to far
                  settimeofday (&trx, NULL);
                  ESP_LOGI (TAG, "syncing clock to server %ld.%06ld",
                            trx.tv_sec, trx.tv_usec);

                  codec_header_message_free (&codec_header_message);

                  received_header = true;

                  break;

                case SNAPCAST_MESSAGE_WIRE_CHUNK:
                  {
                    if (!received_header)
                      {
                        //                        if (typedMsg != NULL)
                        //                          {
                        //                            free (typedMsg);
                        //                            typedMsg = NULL;
                        //                          }

                        continue;
                      }

                    wire_chunk_message_t wire_chunk_message;

                    result = wire_chunk_message_deserialize (
                        &wire_chunk_message, start, size);
                    if (result)
                      {
                        ESP_LOGI (TAG, "Failed to read wire chunk: %d\r\n",
                                  result);

                        wire_chunk_message_free (&wire_chunk_message);
                        break;
                      }

                    //					 ESP_LOGI(TAG, "wire
                    // chnk with size:"
                    //					 "%d, timestamp %d.%d",
                    //					 wire_chunk_message.size,
                    //					 wire_chunk_message.timestamp.sec,
                    //					 wire_chunk_message.timestamp.usec);

                    // store chunk's timestamp, decoder callback
                    // will need it later
                    tv_t timestamp;
                    timestamp = wire_chunk_message.timestamp;

                    switch (codec)
                      {
                      case OPUS:
                        {
                          int frame_size = 0;

                          if (audio == NULL)
                            {
#if CONFIG_USE_PSRAM
                              audio = (int16_t *)heap_caps_malloc (
                                  frameSize * scSet.ch * (scSet.bits / 8),
                                  MALLOC_CAP_8BIT
                                      | MALLOC_CAP_SPIRAM); // 960*2: 20ms,
                                                            // 960*1: 10ms
#else
                              audio = (int16_t *)malloc (
                                  frameSize * scSet.ch
                                  * (scSet.bits
                                     / 8)); // 960*2: 20ms, 960*1: 10ms
#endif
                            }

                          if (audio == NULL)
                            {
                              ESP_LOGE (TAG, "Failed to allocate memory for "
                                             "opus audio decoder");
                            }
                          else
                            {
                              size = wire_chunk_message.size;
                              start = wire_chunk_message.payload;

                              while ((frame_size = opus_decode (
                                          opusDecoder, (unsigned char *)start,
                                          size, (opus_int16 *)audio,
                                          pcm_size / channels, 0))
                                     == OPUS_BUFFER_TOO_SMALL)
                                {
                                  pcm_size = pcm_size * 2;

                                  // 960*2: 20ms, 960*1: 10ms
#if CONFIG_USE_PSRAM
                                  audio = (int16_t *)heap_caps_realloc (
                                      audio,
                                      pcm_size * scSet.ch * (scSet.bits / 8),
                                      MALLOC_CAP_8BIT
                                          | MALLOC_CAP_SPIRAM); // 2 channels +
                                                                // 2 Byte per
                                                                // sample ==
                                                                // int32_t
#else
                                  audio = (int16_t *)realloc (
                                      audio,
                                      pcm_size * scSet.ch * (scSet.bits / 8));
                                  //                    audio = (int16_t,
                                  //                    *)heap_caps_realloc(
                                  //                    				   (int32_t
                                  //                    *)audio, frameSize *
                                  //                    CHANNELS *
                                  //                    (BITS_PER_SAMPLE / 8),
                                  //                    MALLOC_CAP_32BIT);
#endif

                                  ESP_LOGI (TAG,
                                            "OPUS encoding buffer too small, "
                                            "resizing to %d "
                                            "samples per channel",
                                            pcm_size / channels);
                                }

                              if (frame_size < 0)
                                {
                                  ESP_LOGE (
                                      TAG,
                                      "Decode error : %d, %d, %s, %s, %d\n",
                                      frame_size, size, start, (char *)audio,
                                      pcm_size / channels);

                                  free (audio);
                                  audio = NULL;
                                }
                              else
                                {
                                  wire_chunk_message_t pcm_chunk_message;

                                  pcm_chunk_message.size = frame_size
                                                           * scSet.ch
                                                           * (scSet.bits / 8);
                                  pcm_chunk_message.payload = (char *)audio;
                                  pcm_chunk_message.timestamp = timestamp;

                                  scSet.chkDur_ms
                                      = (1000UL * pcm_chunk_message.size)
                                        / (uint32_t) (scSet.ch
                                                      * (scSet.bits / 8))
                                        / scSet.sr;
                                  if (player_send_snapcast_setting (&scSet)
                                      != pdPASS)
                                    {
                                      ESP_LOGE (
                                          TAG,
                                          "Failed to notify sync task about "
                                          "codec. Did you init player?");

                                      return;
                                    }

#if CONFIG_USE_DSP_PROCESSOR
                                  dsp_setup_flow (500, scSet.sr,
                                                  scSet.chkDur_ms);
                                  dsp_processor (pcm_chunk_message.payload,
                                                 pcm_chunk_message.size,
                                                 dspFlow);
#endif

                                  insert_pcm_chunk (&pcm_chunk_message);
                                }
                            }

                          break;
                        }

                      case FLAC:
                        {
                          flacData.bytes = wire_chunk_message.size;
                          flacData.inData = wire_chunk_message.payload;
                          pFlacData = &flacData;

                          //                          startTime =
                          //                          esp_timer_get_time ();
                          // send data to flac decoder
                          xQueueSend (decoderReadQHdl, &pFlacData,
                                      portMAX_DELAY);
                          // and wait until it is done
                          xQueueReceive (decoderWriteQHdl, &pFlacData,
                                         portMAX_DELAY);
                          //                          endTime =
                          //                          esp_timer_get_time ();
                          //				          ESP_LOGW(TAG,
                          //"%lld", endTime - startTime);

                          wire_chunk_message_t pcm_chunk_message;

                          pcm_chunk_message.size = flacData.bytes;
                          pcm_chunk_message.payload = flacData.outData;
                          pcm_chunk_message.timestamp = timestamp;

                          scSet.chkDur_ms
                              = (1000UL * pcm_chunk_message.size)
                                / (uint32_t) (scSet.ch * (scSet.bits / 8))
                                / scSet.sr;
                          if (player_send_snapcast_setting (&scSet) != pdPASS)
                            {
                              ESP_LOGE (TAG,
                                        "Failed to notify sync task about "
                                        "codec. Did you init player?");

                              return;
                            }

#if CONFIG_USE_DSP_PROCESSOR
                          dsp_setup_flow (500, scSet.sr, scSet.chkDur_ms);
                          dsp_processor (pcm_chunk_message.payload,
                                         pcm_chunk_message.size, dspFlow);
#endif

                          //                          int ret;
                          //                          do {
                          //                        	  ret =
                          insert_pcm_chunk (&pcm_chunk_message);
                          //                        	  if (ret < 0) {
                          //                        		  vTaskDelay(10);
                          //                        	  }
                          //                          } while(ret != 0);

                          break;
                        }

                      case PCM:
                        {
                          wire_chunk_message_t pcm_chunk_message;

                          size = wire_chunk_message.size;
                          start = wire_chunk_message.payload;

                          pcm_chunk_message.size = size;
                          pcm_chunk_message.timestamp = timestamp;
                          pcm_chunk_message.payload
                              = wire_chunk_message.payload;

                          scSet.chkDur_ms
                              = (1000UL * pcm_chunk_message.size)
                                / (uint32_t) (scSet.ch * (scSet.bits / 8))
                                / scSet.sr;
                          if (player_send_snapcast_setting (&scSet) != pdPASS)
                            {
                              ESP_LOGE (TAG,
                                        "Failed to notify sync task about "
                                        "codec. Did you init player?");

                              return;
                            }

#if CONFIG_USE_DSP_PROCESSOR
                          dsp_setup_flow (500, scSet.sr, scSet.chkDur_ms);
                          dsp_processor (pcm_chunk_message.payload,
                                         pcm_chunk_message.size, dspFlow);
#endif

                          insert_pcm_chunk (&pcm_chunk_message);

                          break;
                        }

                      default:
                        {
                          ESP_LOGE (TAG, "Decoder not supported");

                          return;

                          break;
                        }
                      }

                    wire_chunk_message_free (&wire_chunk_message);

                    break;
                  }

                case SNAPCAST_MESSAGE_SERVER_SETTINGS:
                  // The first 4 bytes in the buffer are the size of the
                  // string. We don't need this, so we'll shift the entire
                  // buffer over 4 bytes and use the extra room to add a null
                  // character so cJSON can pares it.
                  memmove (start, start + 4, size - 4);
                  start[size - 3] = '\0';
                  result = server_settings_message_deserialize (
                      &server_settings_message, start);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to read server settings: %d",
                                result);
                      return;
                    }
                  // log mute state, buffer, latency
                  ESP_LOGI (TAG, "Buffer length:  %d",
                            server_settings_message.buffer_ms);
                  ESP_LOGI (TAG, "Latency:        %d",
                            server_settings_message.latency);
                  ESP_LOGI (TAG, "Mute:           %d",
                            server_settings_message.muted);
                  ESP_LOGI (TAG, "Setting volume: %d",
                            server_settings_message.volume);

                  // Volume setting using ADF HAL abstraction
                  if (scSet.muted != server_settings_message.muted)
                    {
                      audio_hal_set_mute (board_handle->audio_hal,
                                          server_settings_message.muted);
                    }
                  if (scSet.volume != server_settings_message.volume)
                    {
                      audio_hal_set_volume (board_handle->audio_hal,
                                            server_settings_message.volume);
                    }

                  scSet.cDacLat_ms = server_settings_message.latency;
                  scSet.buf_ms = server_settings_message.buffer_ms;
                  scSet.muted = server_settings_message.muted;
                  scSet.volume = server_settings_message.volume;

                  if (player_send_snapcast_setting (&scSet) != pdPASS)
                    {
                      ESP_LOGE (
                          TAG,
                          "Failed to notify sync task. Did you init player?");

                      return;
                    }

                  break;

                case SNAPCAST_MESSAGE_TIME:
                  result
                      = time_message_deserialize (&time_message, start, size);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to deserialize time message");

                      return;
                    }

                  //            ESP_LOGI(TAG, "BaseTX     : %d %d ",
                  //			base_message.sent.sec ,
                  //			base_message.sent.usec); ESP_LOGI(TAG,
                  //"BaseRX 			: %d %d ",
                  // base_message.received.sec ,
                  // base_message.received.usec); ESP_LOGI(TAG,
                  // "baseTX->RX : %d s ",
                  // (base_message.received.sec
                  // -
                  //			base_message.sent.sec)); ESP_LOGI(TAG,
                  //			"baseTX->RX : %d ms ",
                  //			(base_message.received.usec -
                  //			base_message.sent.usec)/1000);
                  // ESP_LOGI(TAG, 			"Latency : %d.%d ",
                  // time_message.latency.sec,
                  // time_message.latency.usec/1000);

                  // tv == server to client latency (s2c)
                  // time_message.latency == client to server latency(c2s)
                  // TODO the fact that I have to do this simple conversion
                  // means I should probably use the timeval struct instead of
                  // my own
                  trx.tv_sec = base_message.received.sec;
                  trx.tv_usec = base_message.received.usec;
                  ttx.tv_sec = base_message.sent.sec;
                  ttx.tv_usec = base_message.sent.usec;
                  timersub (&trx, &ttx, &tdif);

                  trx.tv_sec = time_message.latency.sec;
                  trx.tv_usec = time_message.latency.usec;

                  // trx == c2s: client to server
                  // tdif == s2c: server to client
                  //                    ESP_LOGI(TAG, "c2s: %ld %ld",
                  //                    trx.tv_sec, trx.tv_usec); ESP_LOGI(TAG,
                  //                    "s2c:  %ld %ld", tdif.tv_sec,
                  //                    tdif.tv_usec);

                  timersub (&trx, &tdif, &tmpDiffToServer);
                  if ((tmpDiffToServer.tv_sec / 2) == 0)
                    {
                      tmpDiffToServer.tv_sec = 0;
                      tmpDiffToServer.tv_usec
                          = (suseconds_t) ((int64_t)tmpDiffToServer.tv_sec
                                           * 1000000LL / 2)
                            + (int64_t)tmpDiffToServer.tv_usec / 2;
                    }
                  else
                    {
                      tmpDiffToServer.tv_sec /= 2;
                      tmpDiffToServer.tv_usec /= 2;
                    }

                  //                   ESP_LOGI(TAG,
                  //                   "Current latency: %ld.%06ld",
                  //                   tmpDiffToServer.tv_sec,
                  //                   tmpDiffToServer.tv_usec);

                  // TODO: Move the time message sending to an own thread maybe
                  // following code is storing / initializing / resetting diff
                  // to server algorithm we collect a number of latencies and
                  // apply a median filter. Based on these we can get server
                  // now
                  {
                    struct timeval diff;
                    int64_t newValue;

                    // clear diffBuffer if last update is older than a minute
                    timersub (&now, &lastTimeSync, &diff);

                    if (diff.tv_sec > 60)
                      {
                        ESP_LOGW (TAG, "Last time sync older than a minute. "
                                       "Clearing time buffer");

                        reset_latency_buffer ();
                      }

                    newValue = ((int64_t)tmpDiffToServer.tv_sec * 1000000LL
                                + (int64_t)tmpDiffToServer.tv_usec);
                    player_latency_insert (newValue);

                    //                    ESP_LOGE(TAG, "latency %lld",
                    //                    newValue);

                    // store current time
                    lastTimeSync.tv_sec = now.tv_sec;
                    lastTimeSync.tv_usec = now.tv_usec;

                    if (xSemaphoreTake (timeSyncSemaphoreHandle, 0) == pdTRUE)
                      {
                        ESP_LOGW (TAG,
                                  "couldn't take timeSyncSemaphoreHandle");
                      }

                    uint64_t timeout;
                    if (latency_buffer_full () > 0)
                      {
                        // we give timeSyncSemaphoreHandle after x µs through
                        // timer
                        // TODO: maybe start a periodic timer here, but we need
                        // to remember if it is already running then. also we
                        // need to stop it if reset_latency_buffer() was called
                        timeout = 1000000;
                      }
                    else
                      {
                        // Do a initial time sync with the server at boot
                        // we need to fill diffBuff fast so we get a good
                        // estimate of latency
                        timeout = 100000;
                      }

                    esp_timer_start_once (timeSyncMessageTimer, timeout);
                  }

                  break;
                }

              //              if (typedMsg != NULL)
              //                {
              //                  free (typedMsg);
              //                  typedMsg = NULL;
              //                }
            }

          if (received_header == true)
            {
              if (xSemaphoreTake (timeSyncSemaphoreHandle, 0) == pdTRUE)
                {
                  result = gettimeofday (&now, NULL);
                  // ESP_LOGI(TAG, "time of day: %ld %ld", now.tv_sec,
                  // now.tv_usec);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to gettimeofday");
                      continue;
                    }

                  base_message.type = SNAPCAST_MESSAGE_TIME;
                  base_message.id = id_counter++;
                  base_message.refersTo = 0;
                  base_message.received.sec = 0;
                  base_message.received.usec = 0;
                  base_message.sent.sec = now.tv_sec;
                  base_message.sent.usec = now.tv_usec;
                  base_message.size = TIME_MESSAGE_SIZE;

                  result = base_message_serialize (&base_message,
                                                   base_message_serialized,
                                                   BASE_MESSAGE_SIZE);
                  if (result)
                    {
                      ESP_LOGE (
                          TAG,
                          "Failed to serialize base message for time\r\n");
                      continue;
                    }

                  memset (&time_message, 0, sizeof (time_message));

                  result = time_message_serialize (&time_message,
                                                   time_message_serialized,
                                                   TIME_MESSAGE_SIZE);
                  if (result)
                    {
                      ESP_LOGI (TAG, "Failed to serialize time message\r\b");
                      continue;
                    }

                  result = send (sock, base_message_serialized,
                                 BASE_MESSAGE_SIZE, 0);
                  if (result < 0)
                    {
                      ESP_LOGW (
                          TAG, "error writing timesync base msg to socket: %s",
                          strerror (errno));

                      shutdown (sock, 2);
                      closesocket (sock);

                      break; // stop for(;;) will try to reconnect then
                    }

                  result = send (sock, time_message_serialized,
                                 TIME_MESSAGE_SIZE, 0);
                  if (result < 0)
                    {
                      ESP_LOGW (TAG,
                                "error writing timesync msg to socket: %s",
                                strerror (errno));

                      shutdown (sock, 2);
                      closesocket (sock);

                      break; // stop for(;;) will try to reconnect then
                    }

                  //                   ESP_LOGI(TAG, "sent time sync message
                  //                   %ld.%06ld", now.tv_sec, now.tv_usec);
                }
            }

          //          endTime = esp_timer_get_time ();
          //          ESP_LOGW(TAG, "%lld", endTime - startTime);
        }
    }
}

void
app_main (void)
{
  esp_err_t ret = nvs_flash_init ();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
      ESP_ERROR_CHECK (nvs_flash_erase ());
      ret = nvs_flash_init ();
    }
  ESP_ERROR_CHECK (ret);

  esp_log_level_set ("*", ESP_LOG_INFO);
  //  esp_log_level_set("c_I2S", ESP_LOG_NONE);

  // if enabled these cause a timer srv stack overflow
  esp_log_level_set ("HEADPHONE", ESP_LOG_NONE);
  esp_log_level_set ("gpio", ESP_LOG_NONE);

  esp_timer_init ();

  ESP_LOGI (TAG, "Start codec chip");
  board_handle = audio_board_init ();
  ESP_LOGI (TAG, "Audio board_init done");

  audio_hal_ctrl_codec (board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH,
                        AUDIO_HAL_CTRL_START);
  audio_hal_set_mute (board_handle->audio_hal,
                      true); // ensure no noise is sent after firmware crash
  i2s_mclk_gpio_select (0, 0);
  // setup_ma120();

#if CONFIG_USE_DSP_PROCESSOR
  dsp_setup_flow (500, 44100, 20); // init with default value
#endif

  ESP_LOGI (TAG, "init player");
  init_player ();

  // Enable and setup WIFI in station mode and connect to Access point setup in
  // menu config or set up provisioning mode settable in menuconfig
  wifi_init ();

  // Enable websocket server
  ESP_LOGI (TAG, "Connected to AP");
  //  ESP_LOGI(TAG, "Setup ws server");
  //  websocket_if_start();

  net_mdns_register ("snapclient");
#ifdef CONFIG_SNAPCLIENT_SNTP_ENABLE
  set_time_from_sntp ();
#endif

  //  xTaskCreatePinnedToCore (&ota_server_task, "ota", 14 * 256, NULL,
  //                           OTA_TASK_PRIORITY, t_ota_task,
  //                           OTA_TASK_CORE_ID);

  //  xTaskCreatePinnedToCore (&http_get_task, "http", 10 * 256, NULL,
  //                           HTTP_TASK_PRIORITY, &t_http_get_task,
  //                           HTTP_TASK_CORE_ID);

  xTaskCreatePinnedToCore (&http_get_task, "http", 3 * 1024, NULL,
                           HTTP_TASK_PRIORITY, &t_http_get_task,
                           HTTP_TASK_CORE_ID);

  while (1)
    {
      // audio_event_iface_msg_t msg;
      vTaskDelay (portMAX_DELAY); //(pdMS_TO_TICKS(5000));

      // ma120_read_error(0x20);

      esp_err_t ret = 0; // audio_event_iface_listen(evt, &msg, portMAX_DELAY);
      if (ret != ESP_OK)
        {
          ESP_LOGE (TAG, "[ * ] Event interface error : %d", ret);
          continue;
        }
    }
}

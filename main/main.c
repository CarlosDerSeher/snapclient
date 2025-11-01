
#include <stdint.h>
#include <string.h>

#include "driver/timer_types_legacy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_pm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"

#include "hal/gpio_types.h"
#include "freertos/idf_additions.h"

#include "board.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "esp_netif.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "mdns.h"
#include "net_functions.h"
#include "network_interface.h"
#include "nvs_flash.h"

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

static bool isCachedChunk = false;
static uint32_t cachedBlocks = 0;

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

static FLAC__StreamDecoder *flacDecoder = NULL;

const char *VERSION_STRING = "0.0.3";

#define HTTP_TASK_PRIORITY 5
#define HTTP_TASK_CORE_ID tskNO_AFFINITY

#define OTA_TASK_PRIORITY 6
#define OTA_TASK_CORE_ID tskNO_AFFINITY
// 1  // tskNO_AFFINITY

TaskHandle_t t_ota_task = NULL;
TaskHandle_t t_http_get_task = NULL;

#define FAST_SYNC_LATENCY_BUF 10000      // in µs
#define NORMAL_SYNC_LATENCY_BUF 1000000  // in µs

struct timeval tdif, tavg;

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

SemaphoreHandle_t idCounterSemaphoreHandle = NULL;

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

typedef struct audioDACdata_s {
  bool mute;
  int volume;
  bool enabled;
} audioDACdata_t;

static audioDACdata_t audioDAC_data;
static QueueHandle_t audioDACQHdl = NULL;
static SemaphoreHandle_t audioDACSemaphore = NULL;

typedef struct decoderData_s {
  uint32_t type;  // should be SNAPCAST_MESSAGE_CODEC_HEADER
                  // or SNAPCAST_MESSAGE_WIRE_CHUNK
  uint8_t *inData;
  tv_t timestamp;
  uint8_t *outData;
  uint32_t bytes;
} decoderData_t;

void time_sync_msg_cb(void *args);

static char base_message_serialized[BASE_MESSAGE_SIZE];

static const esp_timer_create_args_t tSyncArgs = {
    .callback = &time_sync_msg_cb,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "tSyncMsg",
    .skip_unhandled_events = false};

struct netconn *lwipNetconn;

static int id_counter = 0;

static OpusDecoder *opusDecoder = NULL;

static decoderData_t decoderChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
};

static decoderData_t pcmChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
};

/**
 *
 */
void time_sync_msg_cb(void *args) {
  base_message_t base_message_tx;
  //  struct timeval now;
  int64_t now;
  int rc1;

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

  xSemaphoreTake(idCounterSemaphoreHandle, portMAX_DELAY);
  base_message_tx.id = id_counter++;
  xSemaphoreGive(idCounterSemaphoreHandle);

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

  rc1 = netconn_write(lwipNetconn, p_pkt, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE,
                      NETCONN_NOCOPY);
  if (rc1 != ERR_OK) {
    ESP_LOGW(TAG, "error writing timesync msg");

    return;
  }

  free(p_pkt);

  // ESP_LOGI(TAG, "%s: sent time sync message, %u", __func__,
  // base_message_tx.id);
}

/**
 *
 */
static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data) {
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;
  //  decoderData_t *flacData;

  (void)scSet;

  // xQueueReceive(decoderReadQHdl, &flacData, portMAX_DELAY);
  // if (xQueueReceive(decoderReadQHdl, &flacData, pdMS_TO_TICKS(100)))
  if (decoderChunk.inData) {
    //	   ESP_LOGI(TAG, "in flac read cb %ld %p", flacData->bytes,
    // flacData->inData);

    if (decoderChunk.bytes <= 0) {
      //	    free_flac_data(flacData);

      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    isCachedChunk = false;

    //	  if (flacData->inData == NULL) {
    //	    free_flac_data(flacData);
    //
    //	    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    //	  }

    if (decoderChunk.bytes <= *bytes) {
      memcpy(buffer, decoderChunk.inData, decoderChunk.bytes);
      *bytes = decoderChunk.bytes;

      // ESP_LOGW(TAG, "read all flac inData %d", *bytes);

      free(decoderChunk.inData);
      decoderChunk.inData = NULL;
      decoderChunk.bytes = 0;
    } else {
      memcpy(buffer, decoderChunk.inData, *bytes);

      memmove(decoderChunk.inData, decoderChunk.inData + *bytes,
              decoderChunk.bytes - *bytes);
      decoderChunk.bytes -= *bytes;
      decoderChunk.inData =
          (uint8_t *)realloc(decoderChunk.inData, decoderChunk.bytes);

      // ESP_LOGW(TAG, "didn't read all flac inData %d", *bytes);
      //	    flacData->inData += *bytes;
      //	    flacData->bytes -= *bytes;
    }

    // free_flac_data(flacData);

    // xQueueSend (flacReadQHdl, &flacData, portMAX_DELAY);

    // xSemaphoreGive(decoderReadSemaphore);

    // ESP_LOGE(TAG, "%s: data processed", __func__);

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  } else {
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
}

/**
 *
 */
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data) {
  size_t i;
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  size_t bytes = frame->header.blocksize * frame->header.channels *
                 frame->header.bits_per_sample / 8;

  (void)decoder;

  if (isCachedChunk) {
    cachedBlocks += frame->header.blocksize;
  }

  //  ESP_LOGI(TAG, "in flac write cb %ld %d, pcmChunk.bytes %ld",
  //  frame->header.blocksize, bytes, pcmChunk.bytes);

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
    ESP_LOGE(TAG, "ERROR: buffer [0] is NULL");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[1] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [1] is NULL");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  uint8_t * pcmData;
  do {
     pcmData = (uint8_t *)realloc(pcmChunk.outData, pcmChunk.bytes + bytes);
    if (!pcmData) {
      ESP_LOGW(TAG, "%s, failed to allocate PCM chunk payload, try again", __func__);
      vTaskDelay(pdMS_TO_TICKS(5));
      //return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
  } while(!pcmData);
  
  pcmChunk.outData = pcmData;

  for (i = 0; i < frame->header.blocksize; i++) {
    // write little endian
    pcmChunk.outData[pcmChunk.bytes + 4 * i] = (uint8_t)(buffer[0][i]);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 1] = (uint8_t)(buffer[0][i] >> 8);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 2] = (uint8_t)(buffer[1][i]);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 3] = (uint8_t)(buffer[1][i] >> 8);
  }

  pcmChunk.bytes += bytes;

  scSet->chkInFrames = frame->header.blocksize;

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 *
 */
void metadata_callback(const FLAC__StreamDecoder *decoder,
                       const FLAC__StreamMetadata *metadata,
                       void *client_data) {
  snapcastSetting_t *scSet = (snapcastSetting_t *)client_data;

  (void)decoder;

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    // ESP_LOGI(TAG, "in flac meta cb");

    // save for later
    scSet->sr = metadata->data.stream_info.sample_rate;
    scSet->ch = metadata->data.stream_info.channels;
    scSet->bits = metadata->data.stream_info.bits_per_sample;

    ESP_LOGI(TAG, "fLaC sampleformat: %ld:%d:%d", scSet->sr, scSet->bits,
             scSet->ch);

    // ESP_LOGE(TAG, "%s: data processed", __func__);
  }
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

/**
 *
 */
void init_snapcast(QueueHandle_t audioQHdl) {
  audioDACQHdl = audioQHdl;
  audioDACSemaphore = xSemaphoreCreateMutex();
  audioDAC_data.mute = true;
  audioDAC_data.volume = -1;
  audioDAC_data.enabled = false;
}

/**
 *
 */
void audio_dac_enable(bool enabled) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (enabled != audioDAC_data.enabled) {
    audioDAC_data.enabled = enabled;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
}

/**
 *
 */
void audio_set_mute(bool mute) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (mute != audioDAC_data.mute) {
    audioDAC_data.mute = mute;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
}

/**
 *
 */
void audio_set_volume(int volume) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (volume != audioDAC_data.volume) {
    audioDAC_data.volume = volume;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  xSemaphoreGive(audioDACSemaphore);
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
  client_info_t clientInfo = {0, 0};
  int64_t tmpDiffToServer;
  int64_t lastTimeSync = 0;
  esp_timer_handle_t timeSyncMessageTimer = NULL;
  esp_err_t err = 0;
  server_settings_message_t server_settings_message;
  bool received_header = false;
  mdns_result_t *r;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  pcm_chunk_message_t *pcmData = NULL;
  uint16_t remotePort = 0;
  int rc1 = ERR_OK, rc2 = ERR_OK;
  struct netbuf *firstNetBuf = NULL;
  uint16_t len;
  uint64_t timeout = FAST_SYNC_LATENCY_BUF;
  char *codecString = NULL;
  char *codecPayload = NULL;
  char *serverSettingsString = NULL;
  int connected_interface = -1;
  esp_netif_t *netif = NULL;

  // create a timer to send time sync messages every x µs
  esp_timer_create(&tSyncArgs, &timeSyncMessageTimer);

  idCounterSemaphoreHandle = xSemaphoreCreateMutex();
  if (idCounterSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "can't create id Counter Semaphore");

    return;
  }

#if CONFIG_SNAPCLIENT_USE_MDNS
  ESP_LOGI(TAG, "Enable mdns");
  mdns_init();
#endif

  while (1) {
    // do some house keeping
    {
      esp_timer_stop(timeSyncMessageTimer);

      connected_interface = -1;
      received_header = false;
      timeout = FAST_SYNC_LATENCY_BUF;

      xSemaphoreTake(idCounterSemaphoreHandle, portMAX_DELAY);
      id_counter = 0;
      xSemaphoreGive(idCounterSemaphoreHandle);

      if (opusDecoder != NULL) {
        opus_decoder_destroy(opusDecoder);
        opusDecoder = NULL;
      }

      if (flacDecoder != NULL) {
        FLAC__stream_decoder_finish(flacDecoder);
        FLAC__stream_decoder_delete(flacDecoder);
        flacDecoder = NULL;
      }

      if (decoderChunk.inData) {
        free(decoderChunk.inData);
        decoderChunk.inData = NULL;
      }

      if (decoderChunk.outData) {
        free(decoderChunk.outData);
        decoderChunk.outData = NULL;
      }

      if (codecString) {
        free(codecString);
        codecString = NULL;
      }

      if (codecPayload) {
        free(codecPayload);
        codecPayload = NULL;
      }

      if (codecPayload) {
        free(serverSettingsString);
        serverSettingsString = NULL;
      }

      if (lwipNetconn != NULL) {
        netconn_delete(lwipNetconn);
        lwipNetconn = NULL;
      }
    }

    ESP_LOGI(TAG, "Wait for network connection");
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    esp_netif_t *eth_netif =
        network_get_netif_from_desc(NETWORK_INTERFACE_DESC_ETH);
#endif
    esp_netif_t *sta_netif =
        network_get_netif_from_desc(NETWORK_INTERFACE_DESC_STA);
    while (1) {
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
      bool ethUp = network_is_netif_up(eth_netif);

      if (ethUp) {
        netif = eth_netif;

        break;
      }
#endif

      bool staUp = network_is_netif_up(sta_netif);
      if (staUp) {
        netif = sta_netif;

        break;
      }

      vTaskDelay(pdMS_TO_TICKS(1000));
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

    ESP_LOGI(TAG, "\n~~~~~~~~~~ MDNS Query success ~~~~~~~~~~");
    mdns_print_results(r);
    ESP_LOGI(TAG, "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");

    ip_addr_t remote_ip;
    mdns_result_t *re = r;
    while (re) {
      mdns_ip_addr_t *a = re->addr;
#if CONFIG_SNAPCLIENT_CONNECT_IPV6
      if (a->addr.type == IPADDR_TYPE_V6) {
        netif = re->esp_netif;
        break;
      }

      // TODO: fall back to IPv4 if no IPv6 was available
#else
      if (a->addr.type == IPADDR_TYPE_V4) {
        netif = re->esp_netif;
        break;
      }
#endif

      re = re->next;
    }

    if (!re) {
      mdns_query_results_free(r);

      ESP_LOGW(TAG, "didn't find any valid IP in MDNS query");

      continue;
    }

    ip_addr_copy(remote_ip, re->addr->addr);
    remotePort = r->port;

    mdns_query_results_free(r);

    ESP_LOGI(TAG, "Found %s:%d", ipaddr_ntoa(&remote_ip), remotePort);
#else
    ip_addr_t remote_ip;

    if (ipaddr_aton(SNAPCAST_SERVER_HOST, &remote_ip) == 0) {
      ESP_LOGE(TAG, "can't convert static server adress to numeric");
      continue;
    }

    remotePort = SNAPCAST_SERVER_PORT;

    ESP_LOGI(TAG, "try connecting to static configuration %s:%d",
             ipaddr_ntoa(&remote_ip), remotePort);
#endif

    if (remote_ip.type == IPADDR_TYPE_V4) {
      lwipNetconn = netconn_new(NETCONN_TCP);

      ESP_LOGV(TAG, "netconn using IPv4");
    } else if (remote_ip.type == IPADDR_TYPE_V6) {
      lwipNetconn = netconn_new(NETCONN_TCP_IPV6);

      ESP_LOGV(TAG, "netconn using IPv6");
    } else {
      ESP_LOGW(TAG, "remote IP has unsupported IP type");

      continue;
    }

    if (lwipNetconn == NULL) {
      ESP_LOGE(TAG, "can't create netconn");

      continue;
    }

#define USE_INTERFACE_BIND

#ifdef USE_INTERFACE_BIND  // use interface to bind connection
    uint8_t netifIdx = esp_netif_get_netif_impl_index(netif);
    rc1 = netconn_bind_if(lwipNetconn, netifIdx);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "can't bind interface %s", network_get_ifkey(netif));
    }
#else  // use IP to bind connection
    if (remote_ip.type == IPADDR_TYPE_V4) {
      // rc1 = netconn_bind(lwipNetconn, &ipAddr, 0);
      rc1 = netconn_bind(lwipNetconn, IP4_ADDR_ANY, 0);
    } else {
      rc1 = netconn_bind(lwipNetconn, IP6_ADDR_ANY, 0);
    }

    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "can't bind local IP");
    }
#endif
    rc2 = netconn_connect(lwipNetconn, &remote_ip, remotePort);
    if (rc2 != ERR_OK) {
      ESP_LOGE(TAG, "can't connect to remote %s:%d, err %d",
               ipaddr_ntoa(&remote_ip), remotePort, rc2);

#if !SNAPCAST_SERVER_USE_MDNS
      vTaskDelay(pdMS_TO_TICKS(1000));
#endif
    }

    if (rc1 != ERR_OK || rc2 != ERR_OK) {
      netconn_close(lwipNetconn);
      netconn_delete(lwipNetconn);
      lwipNetconn = NULL;

      continue;
    }

    ESP_LOGI(TAG, "netconn connected using %s", network_get_ifkey(netif));

    if (reset_latency_buffer() < 0) {
      ESP_LOGE(TAG,
               "reset_diff_buffer: couldn't reset median filter long. STOP");
      return;
    }

    uint8_t base_mac[6];
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    // Get MAC address for Eth Interface
    char eth_mac_address[18];

    esp_read_mac(base_mac, ESP_MAC_ETH);
    sprintf(eth_mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
            base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    ESP_LOGI(TAG, "eth mac: %s", eth_mac_address);
#endif
    // Get MAC address for WiFi station
    char mac_address[18];
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
            base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    ESP_LOGI(TAG, "sta mac: %s", mac_address);

    now = esp_timer_get_time();

    // init base message
    base_message_rx.type = SNAPCAST_MESSAGE_HELLO;
    xSemaphoreTake(idCounterSemaphoreHandle, portMAX_DELAY);
    base_message_rx.id = id_counter++;
    xSemaphoreGive(idCounterSemaphoreHandle);

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
    scSet.buf_ms = 500;
    scSet.codec = NONE;
    scSet.bits = 16;
    scSet.ch = 2;
    scSet.sr = 44100;
    scSet.chkInFrames = 0;
    scSet.volume = 0;
    scSet.muted = true;

    uint64_t startTime, endTime;
    //    size_t currentPos = 0;
    size_t typedMsgCurrentPos = 0;
    uint32_t typedMsgLen = 0;
    uint32_t offset = 0;
    uint32_t payloadOffset = 0;
    uint32_t tmpData = 0;
    int32_t payloadDataShift = 0;

#define BASE_MESSAGE_STATE 0
#define TYPED_MESSAGE_STATE 1

    // 0 ... base message, 1 ... typed message
    uint32_t state = BASE_MESSAGE_STATE;
    uint32_t internalState = 0;

    firstNetBuf = NULL;

    while (1) {
      rc2 = netconn_recv(lwipNetconn, &firstNetBuf);
      if (rc2 != ERR_OK) {
        ESP_LOGE(TAG, "netconn err %d", rc2);
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

#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
      if (scSet.muted) {
        esp_netif_t *eth_netif =
            network_get_netif_from_desc(NETWORK_INTERFACE_DESC_ETH);

        if (netif != eth_netif) {
          bool ethUp = network_is_netif_up(eth_netif);

          if (ethUp) {
            netconn_close(lwipNetconn);

            if (firstNetBuf != NULL) {
              netbuf_delete(firstNetBuf);

              firstNetBuf = NULL;
            }

            // restart and try to reconnect using preferred interface ETH
            break;
          }
        }
      }
#endif

      // now parse the data
      netbuf_first(firstNetBuf);
      do {
        // currentPos = 0;

        rc1 = netbuf_data(firstNetBuf, (void **)&start, &len);
        if (rc1 == ERR_OK) {
          // ESP_LOGI (TAG, "netconn rx, data len: %d, %d",
          // len, netbuf_len(firstNetBuf));
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

                  // ESP_LOGI(TAG, "BM type %d ts %ld.%ld, refers to %u",
                  //          base_message_rx.type,
                  //          base_message_rx.received.sec,
                  //          base_message_rx.received.usec,
                  //          base_message_rx.refersTo);

                  // ESP_LOGI(TAG,"%u, %ld.%ld", base_message_rx.type,
                  //                   base_message_rx.received.sec,
                  //                   base_message_rx.received.usec);
                  // ESP_LOGI(TAG,"%u, %llu", base_message_rx.type,
                  //		                      1000000ULL *
                  //                          (uint64_t)base_message_rx.received.sec
                  //                          +
                  //                          (uint64_t)base_message_rx.received.usec);

                  state = TYPED_MESSAGE_STATE;
                  break;
              }

              // currentPos++;++;
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
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      wire_chnk.timestamp.sec |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,
                      // "wire chunk time sec: %d",
                      // wire_chnk.timestamp.sec);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      wire_chnk.timestamp.usec = (*start & 0xFF);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      wire_chnk.timestamp.usec |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,
                      // "wire chunk time usec: %d",
                      // wire_chnk.timestamp.usec);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 8: {
                      wire_chnk.size = (*start & 0xFF);

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 9: {
                      wire_chnk.size |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 10: {
                      wire_chnk.size |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 11: {
                      wire_chnk.size |= (*start & 0xFF) << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      // TODO: we could use wire chunk directly maybe?
                      decoderChunk.bytes = wire_chnk.size;
                      while (!decoderChunk.inData) {
                        decoderChunk.inData =
                            (uint8_t *)malloc(decoderChunk.bytes);
                        if (!decoderChunk.inData) {
                          ESP_LOGW(TAG,
                                   "malloc decoderChunk.inData failed, wait "
                                   "1ms and try again");

                          vTaskDelay(pdMS_TO_TICKS(1));
                        }
                      }

                      payloadOffset = 0;

#if 0
                       ESP_LOGI(TAG, "chunk with size: %u, at time %ld.%ld",
                    		   	   	   	   	 wire_chnk.size,
                                             wire_chnk.timestamp.sec,
                                             wire_chnk.timestamp.usec);
#endif

                      if (len == 0) {
                        break;
                      }
                    }

                    case 12: {
                      size_t tmp_size;

                      if ((base_message_rx.size - typedMsgCurrentPos) <= len) {
                        tmp_size = base_message_rx.size - typedMsgCurrentPos;
                      } else {
                        tmp_size = len;
                      }

                      if (received_header == true) {
                        switch (codec) {
                          case OPUS:
                          case FLAC: {
                            memcpy(&decoderChunk.inData[payloadOffset], start,
                                   tmp_size);
                            payloadOffset += tmp_size;
                            decoderChunk.outData = NULL;
                            decoderChunk.type = SNAPCAST_MESSAGE_WIRE_CHUNK;

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
                      // currentPos += tmp_size;
                      len -= tmp_size;

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        if (received_header == true) {
                          switch (codec) {
                            case OPUS: {
                              int frame_size = -1;
                              int samples_per_frame;
                              opus_int16 *audio = NULL;

                              samples_per_frame =
                                  opus_packet_get_samples_per_frame(
                                      decoderChunk.inData, scSet.sr);
                              if (samples_per_frame < 0) {
                                ESP_LOGE(TAG,
                                         "couldn't get samples per frame count "
                                         "of packet");
                              }

                              scSet.chkInFrames = samples_per_frame;

                              // ESP_LOGW(TAG, "%d, %llu, %llu",
                              // samples_per_frame, 1000000ULL *
                              // samples_per_frame / scSet.sr,
                              // 1000000ULL *
                              // wire_chnk.timestamp.sec +
                              // wire_chnk.timestamp.usec);

                              // ESP_LOGW(TAG, "got OPUS decoded chunk size: %ld
                              // " "frames from encoded chunk with size %d,
                              // allocated audio buffer %d", scSet.chkInFrames,
                              // wire_chnk.size, samples_per_frame);

                              size_t bytes;
                              do {
                                bytes = samples_per_frame *
                                        (scSet.ch * scSet.bits >> 3);

                                while ((audio = (opus_int16 *)realloc(
                                            audio, bytes)) == NULL) {
                                  ESP_LOGE(TAG,
                                           "couldn't realloc memory for OPUS "
                                           "audio %d",
                                           bytes);

                                  vTaskDelay(pdMS_TO_TICKS(1));
                                }

                                frame_size = opus_decode(
                                    opusDecoder, decoderChunk.inData,
                                    decoderChunk.bytes, (opus_int16 *)audio,
                                    samples_per_frame, 0);

                                samples_per_frame <<= 1;
                              } while (frame_size < 0);

                              free(decoderChunk.inData);
                              decoderChunk.inData = NULL;

                              pcm_chunk_message_t *new_pcmChunk = NULL;

                              // ESP_LOGW(TAG, "OPUS decode: %d", frame_size);

                              if (allocate_pcm_chunk_memory(&new_pcmChunk,
                                                            bytes) < 0) {
                                pcmData = NULL;
                              } else {
                                new_pcmChunk->timestamp = wire_chnk.timestamp;

                                if (new_pcmChunk->fragment->payload) {
                                  volatile uint32_t *sample;
                                  uint32_t tmpData;
                                  uint32_t cnt = 0;

                                  for (int i = 0; i < bytes; i += 4) {
                                    sample = (volatile uint32_t *)(&(
                                        new_pcmChunk->fragment->payload[i]));
                                    tmpData = (((uint32_t)audio[cnt] << 16) &
                                               0xFFFF0000) |
                                              (((uint32_t)audio[cnt + 1] << 0) &
                                               0x0000FFFF);
                                    *sample = (volatile uint32_t)tmpData;

                                    cnt += 2;
                                  }
                                }

                                free(audio);
                                audio = NULL;

#if CONFIG_USE_DSP_PROCESSOR
                                if (new_pcmChunk->fragment->payload) {
                                  dsp_processor_worker((void *)new_pcmChunk,
                                                       (void *)&scSet);
                                }
#endif

                                insert_pcm_chunk(new_pcmChunk);
                              }

                              if (player_send_snapcast_setting(&scSet) !=
                                  pdPASS) {
                                ESP_LOGE(TAG,
                                         "Failed to notify "
                                         "sync task about "
                                         "codec. Did you "
                                         "init player?");

                                return;
                              }

                              break;
                            }

                            case FLAC: {
                              isCachedChunk = true;
                              cachedBlocks = 0;

                              while (decoderChunk.bytes > 0) {
                                if (FLAC__stream_decoder_process_single(
                                        flacDecoder) == 0) {
                                  ESP_LOGE(
                                      TAG,
                                      "%s: FLAC__stream_decoder_process_single "
                                      "failed",
                                      __func__);

                                  // TODO: should insert some abort condition?
                                  vTaskDelay(pdMS_TO_TICKS(10));
                                }
                              }

                              // alternating chunk sizes need time stamp repair
                              if ((cachedBlocks > 0) && (scSet.sr != 0)) {
                                uint64_t diffUs =
                                    1000000ULL * cachedBlocks / scSet.sr;

                                uint64_t timestamp =
                                    1000000ULL * wire_chnk.timestamp.sec +
                                    wire_chnk.timestamp.usec;

                                timestamp = timestamp - diffUs;

                                wire_chnk.timestamp.sec =
                                    timestamp / 1000000ULL;
                                wire_chnk.timestamp.usec =
                                    timestamp % 1000000ULL;
                              }

                              pcm_chunk_message_t *new_pcmChunk;
                              int32_t ret = allocate_pcm_chunk_memory(
                                  &new_pcmChunk, pcmChunk.bytes);

                              scSet.chkInFrames =
                                  FLAC__stream_decoder_get_blocksize(
                                      flacDecoder);

                              // ESP_LOGE (TAG, "block size: %ld",
                              // scSet.chkInFrames * scSet.bits / 8 * scSet.ch);
                              // ESP_LOGI(TAG, "new_pcmChunk with size %ld",
                              // new_pcmChunk->totalSize);

                              if (ret == 0) {
                                pcm_chunk_fragment_t *fragment =
                                    new_pcmChunk->fragment;
                                uint32_t fragmentCnt = 0;

                                if (fragment->payload != NULL) {
                                  uint32_t frames =
                                      pcmChunk.bytes /
                                      (scSet.ch * (scSet.bits / 8));

                                  for (int i = 0; i < frames; i++) {
                                    // TODO: for now fragmented payload is not
                                    // supported and the whole chunk is expected
                                    // to be in the first fragment
                                    uint32_t tmpData;
                                    memcpy(&tmpData,
                                           &pcmChunk.outData[fragmentCnt],
                                           (scSet.ch * (scSet.bits / 8)));

                                    if (fragment != NULL) {
                                      volatile uint32_t *test =
                                          (volatile uint32_t *)(&(
                                              fragment->payload[fragmentCnt]));
                                      *test = (volatile uint32_t)tmpData;
                                    }

                                    fragmentCnt +=
                                        (scSet.ch * (scSet.bits / 8));
                                    if (fragmentCnt >= fragment->size) {
                                      fragmentCnt = 0;

                                      fragment = fragment->nextFragment;
                                    }
                                  }
                                }

                                new_pcmChunk->timestamp = wire_chnk.timestamp;

#if CONFIG_USE_DSP_PROCESSOR
                                if (new_pcmChunk->fragment->payload) {
                                  dsp_processor_worker((void *)new_pcmChunk,
                                                       (void *)&scSet);
                                }

#endif

                                insert_pcm_chunk(new_pcmChunk);
                              }

                              free(pcmChunk.outData);
                              pcmChunk.outData = NULL;
                              pcmChunk.bytes = 0;

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

                              break;
                            }

                            case PCM: {
                              size_t decodedSize = wire_chnk.size;

                              // ESP_LOGW(TAG, "got PCM chunk,"
                              //               "typedMsgCurrentPos %d",
                              //               typedMsgCurrentPos);

                              if (pcmData) {
                                pcmData->timestamp = wire_chnk.timestamp;
                              }

                              scSet.chkInFrames =
                                  decodedSize /
                                  ((size_t)scSet.ch * (size_t)(scSet.bits / 8));

                              // ESP_LOGW(TAG,
                              //          "got PCM decoded chunk size: %ld
                              //          frames", scSet.chkInFrames);

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
                                dsp_processor_worker((void *)pcmData,
                                                     (void *)&scSet);
                              }
#endif

                              if (pcmData) {
                                insert_pcm_chunk(pcmData);
                              }

                              pcmData = NULL;

                              free(decoderChunk.inData);
                              decoderChunk.inData = NULL;

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
                      received_header = false;

                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      if (codecString) {
                        free(codecString);
                        codecString = NULL;
                      }

                      codecString =
                          malloc(typedMsgLen + 1);  // allocate memory for
                                                    // codec string
                      if (codecString == NULL) {
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
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      if (len >= typedMsgLen) {
                        memcpy(&codecString[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        // currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&codecString[offset], start, typedMsgLen);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        // currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        // NULL terminate string
                        codecString[typedMsgLen] = 0;

                        // ESP_LOGI (TAG, "got codec string: %s", tmp);

                        if (strcmp(codecString, "opus") == 0) {
                          codec = OPUS;
                        } else if (strcmp(codecString, "flac") == 0) {
                          codec = FLAC;
                        } else if (strcmp(codecString, "pcm") == 0) {
                          codec = PCM;
                        } else {
                          codec = NONE;

                          ESP_LOGI(TAG, "Codec : %s not supported",
                                   codecString);
                          ESP_LOGI(TAG,
                                   "Change encoder codec to "
                                   "opus, flac or pcm in "
                                   "/etc/snapserver.conf on "
                                   "server");

                          return;
                        }

                        free(codecString);
                        codecString = NULL;

                        internalState++;
                      }

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 8: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      if (codecPayload) {
                        free(codecPayload);
                        codecPayload = NULL;
                      }

                      codecPayload = malloc(typedMsgLen);  // allocate memory
                                                           // for codec payload
                      if (codecPayload == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory "
                                 "for codec payload");

                        return;
                      }

                      offset = 0;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 9: {
                      if (len >= typedMsgLen) {
                        memcpy(&codecPayload[offset], start, typedMsgLen);

                        offset += typedMsgLen;

                        typedMsgCurrentPos += typedMsgLen;
                        start += typedMsgLen;
                        // currentPos += typedMsgLen;
                        len -= typedMsgLen;
                      } else {
                        memcpy(&codecPayload[offset], start, len);

                        offset += len;

                        typedMsgCurrentPos += len;
                        start += len;
                        // currentPos += len;
                        len -= len;
                      }

                      if (offset == typedMsgLen) {
                        // first ensure everything is set up
                        // correctly and resources are
                        // available

                        if (flacDecoder != NULL) {
                          FLAC__stream_decoder_finish(flacDecoder);
                          FLAC__stream_decoder_delete(flacDecoder);
                          flacDecoder = NULL;
                        }

                        if (opusDecoder != NULL) {
                          opus_decoder_destroy(opusDecoder);
                          opusDecoder = NULL;
                        }

                        if (codec == OPUS) {
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&rate, codecPayload + 4, sizeof(rate));
                          memcpy(&bits, codecPayload + 8, sizeof(bits));
                          memcpy(&channels, codecPayload + 10,
                                 sizeof(channels));

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
                        } else if (codec == FLAC) {
                          decoderChunk.bytes = typedMsgLen;
                          do {
                            decoderChunk.inData =
                                (uint8_t *)malloc(decoderChunk.bytes);
                            vTaskDelay(pdMS_TO_TICKS(1));
                          } while (decoderChunk.inData == NULL);
                          memcpy(decoderChunk.inData, codecPayload,
                                 typedMsgLen);
                          decoderChunk.outData = NULL;
                          decoderChunk.type = SNAPCAST_MESSAGE_CODEC_HEADER;

                          flacDecoder = FLAC__stream_decoder_new();
                          if (flacDecoder == NULL) {
                            ESP_LOGE(TAG, "Failed to init flac decoder");
                            return;
                          }

                          FLAC__StreamDecoderInitStatus init_status =
                              FLAC__stream_decoder_init_stream(
                                  flacDecoder, read_callback, NULL, NULL, NULL,
                                  NULL, write_callback, metadata_callback,
                                  error_callback, &scSet);
                          if (init_status !=
                              FLAC__STREAM_DECODER_INIT_STATUS_OK) {
                            ESP_LOGE(TAG, "ERROR: initializing decoder: %s\n",
                                     FLAC__StreamDecoderInitStatusString
                                         [init_status]);

                            return;
                          }

                          FLAC__stream_decoder_process_until_end_of_metadata(
                              flacDecoder);

                          // ESP_LOGI(TAG, "%s: processed codec header",
                          // __func__);
                        } else if (codec == PCM) {
                          uint16_t channels;
                          uint32_t rate;
                          uint16_t bits;

                          memcpy(&channels, codecPayload + 22,
                                 sizeof(channels));
                          memcpy(&rate, codecPayload + 24, sizeof(rate));
                          memcpy(&bits, codecPayload + 34, sizeof(bits));

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

                        free(codecPayload);
                        codecPayload = NULL;

                        if (player_send_snapcast_setting(&scSet) != pdPASS) {
                          ESP_LOGE(TAG,
                                   "Failed to notify sync task. "
                                   "Did you init player?");

                          return;
                        }

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
                      typedMsgLen = *start & 0xFF;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      typedMsgLen |= (*start & 0xFF) << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      typedMsgLen |= (*start & 0xFF) << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      typedMsgLen |= (*start & 0xFF) << 24;

                      // ESP_LOGI(TAG,"server settings string is %lu"
                      //              " long", typedMsgLen);

                      // now get some memory for server settings
                      // string
                      serverSettingsString = malloc(typedMsgLen + 1);
                      if (serverSettingsString == NULL) {
                        ESP_LOGE(TAG,
                                 "couldn't get memory for "
                                 "server settings string");
                      }

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      offset = 0;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      size_t tmpSize =
                          base_message_rx.size - typedMsgCurrentPos;

                      if (len > 0) {
                        if (tmpSize < len) {
                          if (serverSettingsString) {
                            memcpy(&serverSettingsString[offset], start,
                                   tmpSize);
                          }
                          offset += tmpSize;

                          start += tmpSize;
                          // currentPos += tmpSize;  // will be
                          //  incremented by 1
                          //  later so -1 here
                          typedMsgCurrentPos += tmpSize;
                          len -= tmpSize;
                        } else {
                          if (serverSettingsString) {
                            memcpy(&serverSettingsString[offset], start, len);
                          }
                          offset += len;

                          start += len;
                          // currentPos += len;  // will be incremented
                          //  by 1 later so -1
                          //  here
                          typedMsgCurrentPos += len;
                          len = 0;
                        }
                      }

                      if (typedMsgCurrentPos >= base_message_rx.size) {
                        if (serverSettingsString) {
                          // ESP_LOGI(TAG, "done server settings %lu/%lu",
                          //								offset,
                          //								typedMsgLen);

                          // NULL terminate string
                          serverSettingsString[typedMsgLen] = 0;

                          // ESP_LOGI(TAG, "got string: %s",
                          // serverSettingsString);

                          result = server_settings_message_deserialize(
                              &server_settings_message, serverSettingsString);
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
                            audio_set_mute(server_settings_message.muted);
                          }

                          if (scSet.volume != server_settings_message.volume) {
#if SNAPCAST_USE_SOFT_VOL
                            if (!server_settings_message.muted) {
                              dsp_processor_set_volome(
                                  (double)server_settings_message.volume / 100);
                            }
#else
                            audio_set_volume(server_settings_message.volume);
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

                          free(serverSettingsString);
                          serverSettingsString = NULL;
                        }

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

                  //                case SNAPCAST_MESSAGE_STREAM_TAGS: {
                  //                  size_t tmpSize = base_message_rx.size -
                  //                  typedMsgCurrentPos;
                  //
                  //                  if (tmpSize < len) {
                  //                    start += tmpSize;
                  //                    // currentPos += tmpSize;
                  //                    typedMsgCurrentPos += tmpSize;
                  //                    len -= tmpSize;
                  //                  } else {
                  //                    start += len;
                  //                    // currentPos += len;
                  //
                  //                    typedMsgCurrentPos += len;
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
                  //                    typedMsgCurrentPos = 0;
                  //                    // currentPos = 0;
                  //
                  //                    state = BASE_MESSAGE_STATE;
                  //                    internalState = 0;
                  //                  }
                  //
                  //                  break;
                  //                }

                case SNAPCAST_MESSAGE_TIME: {
                  switch (internalState) {
                    case 0: {
                      time_message_rx.latency.sec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 1: {
                      time_message_rx.latency.sec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 2: {
                      time_message_rx.latency.sec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 3: {
                      time_message_rx.latency.sec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 4: {
                      time_message_rx.latency.usec = *start;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 5: {
                      time_message_rx.latency.usec |= (int32_t)*start << 8;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 6: {
                      time_message_rx.latency.usec |= (int32_t)*start << 16;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
                      len--;

                      internalState++;

                      if (len == 0) {
                        break;
                      }
                    }

                    case 7: {
                      time_message_rx.latency.usec |= (int32_t)*start << 24;

                      typedMsgCurrentPos++;
                      start++;
                      // currentPos++;
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
                  // currentPos++;
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

  board_i2s_pin_t pin_config0;
  get_i2s_pins(I2S_NUM_0, &pin_config0);

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
  audio_board_handle_t board_handle = audio_board_init();
  if (board_handle) {
    ESP_LOGI(TAG, "Audio board_init done");
  } else {
    ESP_LOGE(TAG,
             "Audio board couldn't be initialized. Check menuconfig if project "
             "is configured right or check your wiring!");

    vTaskDelay(portMAX_DELAY);
  }

  audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE,
                       AUDIO_HAL_CTRL_STOP);
  audio_hal_set_mute(board_handle->audio_hal,
                     true);  // ensure no noise is sent after firmware crash

#if CONFIG_AUDIO_BOARD_CUSTOM && CONFIG_DAC_ADAU1961
  if (tx_chan) {
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
#endif

  //  ESP_LOGI(TAG, "init player");
  i2s_std_gpio_config_t i2s_pin_config0 = {
      .mclk = pin_config0.mck_io_num,
      .bclk = pin_config0.bck_io_num,
      .ws = pin_config0.ws_io_num,
      .dout = pin_config0.data_out_num,
      .din = pin_config0.data_in_num,
      .invert_flags =
          {
#if CONFIG_INVERT_MCLK_LEVEL
              .mclk_inv = true,

#else
              .mclk_inv = false,
#endif

#if CONFIG_INVERT_BCLK_LEVEL
              .bclk_inv = true,
#else
              .bclk_inv = false,
#endif

#if CONFIG_INVERT_WORD_SELECT_LEVEL
              .ws_inv = true,
#else
              .ws_inv = false,
#endif
          },
  };

  QueueHandle_t audioQHdl = xQueueCreate(1, sizeof(audioDACdata_t));

  init_snapcast(audioQHdl);
  init_player(i2s_pin_config0, I2S_NUM_0);

  // ensure there is no noise from DAC
  {
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
  }

  /*
  #if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
      CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    eth_init();
    // pass "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF"
    init_http_server_task("ETH_DEF");
  #else
    // Enable and setup WIFI in station mode and connect to Access point setup
  in
    // menu config or set up provisioning mode settable in menuconfig
    wifi_init();
    ESP_LOGI(TAG, "Connected to AP");
    // http server for control operations and user interface
    // pass "WIFI_STA_DEF", "WIFI_AP_DEF", "ETH_DEF"
    init_http_server_task("WIFI_STA_DEF");
  #endif
  */
  network_if_init();

  init_http_server_task();

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

  xTaskCreatePinnedToCore(&http_get_task, "http", 15 * 1024, NULL,
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

  audioDACdata_t dac_data;
  audioDACdata_t dac_data_old = {
      .mute = true,
      .volume = -1,
      .enabled = false,
  };
  
#if CONFIG_PM_ENABLE
    //Configure dynamic frequency scaling:
    //automatic light sleep is enabled if tickless idle support is enabled.
    esp_pm_config_t pmConfig = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, //Maximum CPU frequency
        .min_freq_mhz = 40,  //Minimum CPU frequency
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true
#endif
    };
    esp_pm_configure(&pmConfig);
#endif

  while (1) {
    if (xQueueReceive(audioQHdl, &dac_data, portMAX_DELAY) == pdTRUE) {
      if (dac_data.mute != dac_data_old.mute) {
        audio_hal_set_mute(board_handle->audio_hal, dac_data.mute);
      }
      if (dac_data.volume != dac_data_old.volume) {
        audio_hal_set_volume(board_handle->audio_hal, dac_data.volume);
      }
      if (dac_data.enabled != dac_data_old.enabled) {
        if (dac_data.enabled) { 
          audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
        }
        else {
          audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_STOP);
        }
      }
      dac_data_old = dac_data;
    }
  }
}

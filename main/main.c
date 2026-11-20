#include <stdint.h>
#include <string.h>

#include "board.h"
#include "driver/timer_types_legacy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "mdns.h"
#include "net_functions.h"
#include "network_interface.h"
#include "nvs_flash.h"

#if CONFIG_ESP32_UDP_LOGGER_ENABLED
#include "esp32_udp_logger.h"
#endif

// Web socket server
// #include "websocket_if.h"
// #include "websocket_server.h"

#include <sys/time.h>

#include "driver/i2s_std.h"
#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#include "dsp_processor_settings.h"
#endif

#if !CONFIG_SNAPCLIENT_DISABLE_OPUS
// Opus decoder is implemented as a subcomponet from master git repo
#include "opus.h"
#endif

// flac decoder is implemented as a subcomponet from master git repo
#include "FLAC/stream_decoder.h"
#include "connection_handler.h"
#include "heap_diag.h"
#include "esp_ota_ops.h"
#include "player.h"
#include "settings_manager.h"
#include "snapcast.h"
#include "snapcast_protocol_parser.h"
#include "ui_http_server.h"
#if CONFIG_DAC_TAS5805M
#include "tas5805m_settings.h"
#endif

// true until read_callback() runs for the current wire chunk: a frame decoded
// before that came from data of an earlier chunk still in libFLAC's bitreader
static bool isCachedChunk = false;

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

const char *VERSION_STRING = "0.0.4";

#define HTTP_TASK_PRIORITY 17
#define HTTP_TASK_CORE_ID 1
// Measured peak ~11.7 KB with Opus (its decoder keeps large arrays on the
// stack), ~2.5 KB with FLAC and PCM
#if CONFIG_SNAPCLIENT_DISABLE_OPUS
#define HTTP_TASK_STACK_SIZE (4 * 1024)
#else
#define HTTP_TASK_STACK_SIZE (13 * 1024)
#endif

TaskHandle_t t_main_task = NULL;
TaskHandle_t t_http_get_task = NULL;

// Consecutive FLAC__stream_decoder_process_single() calls that read nothing
// before the decoder is considered stuck
#define FLAC_MAX_IDLE_CALLS 16

// Snapserver sends one encoded frame per wire chunk. The input buffer is
// allocated once per codec header, for the largest possible frame:
// FLAC: a verbatim (uncompressed) frame plus frame and subframe headers
#define FLAC_FRAME_OVERHEAD_BYTES 64
// Opus: TOC byte plus a frame of up to 1275 bytes (RFC 6716, 3.2.1)
#define OPUS_MAX_PACKET_BYTES 1276

#define FAST_SYNC_LATENCY_BUF 10000      // in µs
#define NORMAL_SYNC_LATENCY_BUF 1000000  // in µs

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_USE_SOFT_VOL CONFIG_SNAPCLIENT_USE_SOFT_VOL

/* Logging tag */
static const char *TAG = "SC";


// static QueueHandle_t playerChunkQueueHandle = NULL;
SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

static SemaphoreHandle_t idCounterSemaphoreHandle = NULL;
static SemaphoreHandle_t snapclientStateChangedMutex = NULL;

typedef struct snapcastSetting_s {
  playerSetting_t playerSetting;

  bool muted;
  uint32_t volume;
} snapcastSetting_t;

typedef struct audioDACdata_s {
  bool playerMute;
  bool stateMute;
  int volume;
} audioDACdata_t;

static audioDACdata_t audioDAC_data;
static QueueHandle_t audioDACQHdl = NULL;
static SemaphoreHandle_t audioDACSemaphore = NULL;
static void (*set_volume_cb)(int volume);
static void (*set_mute_cb)(bool mute, bool state);
static SemaphoreHandle_t snapclientStateMux = NULL;
static SemaphoreHandle_t i2sLockMutex = NULL;

void time_sync_msg_cb(void *args);

static char base_message_serialized[BASE_MESSAGE_SIZE];

//static const esp_timer_create_args_t tSyncArgs = {
//    .callback = &time_sync_msg_cb,
//    .dispatch_method = ESP_TIMER_TASK,
//    .name = "tSyncMsg",
//    .skip_unhandled_events = false};

struct netconn *lwipNetconn;

static int id_counter = 0;

#if !CONFIG_SNAPCLIENT_DISABLE_OPUS
static OpusDecoder *opusDecoder = NULL;
#endif

// Payload of the current wire chunk, filled by parse_wire_chunk_message()
static decoderData_t decoderChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
    .capacity = 0,
};

// Not yet read input of the FLAC decoder: the codec header, then the payload
// of each wire chunk
static const uint8_t *flacInput = NULL;
static uint32_t flacInputBytes = 0;
static uint32_t flacMaxBlocksize = 0;  // from STREAMINFO

// FLAC frame decoded by the last FLAC__stream_decoder_process_single() (at
// most one per call); flacChunk is NULL if it was dropped
static pcm_chunk_message_t *flacChunk = NULL;
static uint32_t flacChunkFrames = 0;
static bool flacChunkCached = false;

// FLAC frame timestamps: the wire chunk timestamp of the last anchor plus the
// frames decoded since. A frame that reads a new wire chunk re-anchors.
static bool flacAnchorValid = false;
static int64_t flacAnchorUs = 0;
static uint64_t flacAnchorFrames = 0;

#if !CONFIG_SNAPCLIENT_DISABLE_OPUS
// opus writes 16-bit samples, which PCM chunks in IRAM don't allow: decode
// here, then copy into the chunk with 32-bit writes
static opus_int16 *opusScratch = NULL;
static uint32_t opusScratchBytes = 0;
#endif

// stream the pcm pool was built for; a different one rebuilds it
typedef struct {
  uint32_t slotBytes;
  uint32_t slots;
  int32_t sr;
  uint8_t ch;
  uint8_t bits;
} pcm_pool_key_t;

static pcm_pool_key_t poolKey;        // pool in use
static pcm_pool_key_t poolFailedKey;  // last pool that didn't fit
static codec_type_t poolCodec = NONE;  // codec the pool is for

// FLAC: the pool is built after the first frame, once libFLAC has allocated
// its output buffers
static bool flacPoolPending = false;

static bool pcm_pool_ensure(playerSetting_t *setting);

/**
 *
 */
void time_sync_msg_cb(void *args) {
  base_message_t base_message_tx;
  //  struct timeval now;
  int64_t now;
  int rc1;
  uint8_t p_pkt[BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE];

//  uint8_t *p_pkt = (uint8_t *)malloc(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);
//  if (p_pkt == NULL) {
//    ESP_LOGW(
//        TAG,
//        "%s: Failed to get memory for time sync message. Skipping this round.",
//        __func__);
//
//    return;
//  }

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

//  free(p_pkt);

  // ESP_LOGI(TAG, "%s: sent time sync message, %u", __func__,
  // base_message_tx.id);
}

typedef struct {
  int64_t now;
  int64_t lastTimeSync;
  int64_t lastTimeSyncSent;
  uint64_t timeout;
} time_sync_data_t;

/**
 *
 */
void time_sync_msg_received(base_message_t *base_message_rx,
                            time_message_t *time_message_rx,
                            time_sync_data_t *time_sync_data,
                            bool received_codec_header) {
  int64_t tmpDiffToServer, trx, tdif, ttx, diff;
  trx = (int64_t)base_message_rx->received.sec * 1000000LL +
        (int64_t)base_message_rx->received.usec;
  ttx = (int64_t)base_message_rx->sent.sec * 1000000LL +
        (int64_t)base_message_rx->sent.usec;
  tdif = trx - ttx;  //T4-T3
  ttx = (int64_t)time_message_rx->latency.sec * 1000000LL +
        (int64_t)time_message_rx->latency.usec; // T2-T1
  tmpDiffToServer = (ttx - tdif) / 2; //((T2-T1) - (-T3+T4))/2

  // clear diffBuffer if last update is
  // older than a minute
  diff = time_sync_data->now - time_sync_data->lastTimeSync;
  if (diff > 60000000LL) {
    ESP_LOGW(TAG,
             "Last time sync older "
             "than a minute. "
             "Clearing time buffer");

    reset_latency_buffer();

    time_sync_data->timeout = FAST_SYNC_LATENCY_BUF;

    netconn_set_recvtimeout(lwipNetconn, time_sync_data->timeout / 1000); // timeout in ms                          

    // esp_timer_stop(time_sync_data->timeSyncMessageTimer);
    // if (received_codec_header == true) {
    //   if (!esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
    //     esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
    //                              time_sync_data->timeout);
    //   }
    // }
  }

#if USE_TIMEFILTER
  player_latency_insert(tmpDiffToServer, (tdif + ttx) / 2, trx);
#else
  player_latency_insert(tmpDiffToServer);
#endif

  // ESP_LOGI(TAG, "Current latency:%lld:",
  // tmpDiffToServer);

  // store current time
  time_sync_data->lastTimeSync = time_sync_data->now;

  if (received_codec_header == true) {
    // if (!esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
    //   esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
    //                            time_sync_data->timeout);
    // }

    bool is_full = false;
    latency_buffer_full(&is_full);
    if ((is_full == true) &&
        (time_sync_data->timeout < NORMAL_SYNC_LATENCY_BUF)) {
      time_sync_data->timeout = NORMAL_SYNC_LATENCY_BUF;
      netconn_set_recvtimeout(lwipNetconn, time_sync_data->timeout / 1000); // timeout in ms

      ESP_LOGI(TAG, "latency buffer full");

      // if (esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
      //   esp_timer_stop(time_sync_data->timeSyncMessageTimer);
      // }

      // esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
      //                          time_sync_data->timeout);
    } else if ((is_full == false) &&
               (time_sync_data->timeout > FAST_SYNC_LATENCY_BUF)) {
      time_sync_data->timeout = FAST_SYNC_LATENCY_BUF;
      netconn_set_recvtimeout(lwipNetconn, time_sync_data->timeout / 1000); // timeout in ms

      ESP_LOGI(TAG, "latency buffer not full");

      // if (esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
      //   esp_timer_stop(time_sync_data->timeSyncMessageTimer);
      // }

      // esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
      //                          time_sync_data->timeout);
    }
  }
}

/**
 *
 */
static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data) {
  if (flacInputBytes == 0) {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }

  isCachedChunk = false;

  size_t n = (flacInputBytes < *bytes) ? flacInputBytes : *bytes;
  memcpy(buffer, flacInput, n);
  flacInput += n;
  flacInputBytes -= n;
  *bytes = n;

  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

/**
 *
 */
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data) {
  size_t i;
  playerSetting_t *scSet = (playerSetting_t *)client_data;

  (void)decoder;

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

  flacChunkFrames = frame->header.blocksize;
  flacChunkCached = isCachedChunk;

  // handle_chunk_message() sets the timestamp and submits it. Each frame
  // takes 4 bytes (16-bit stereo), see codec_header_received().
  if (!pcm_pool_ready()) {
    // not built yet, or the buffer didn't fit (logged then)
    flacChunk = NULL;

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }
  if ((size_t)frame->header.blocksize * 4 > pcm_pool_slot_bytes()) {
    flacChunk = NULL;
    ESP_LOGW(TAG, "FLAC frame of %lu frames doesn't fit a pcm chunk of %lu B",
             frame->header.blocksize, pcm_pool_slot_bytes());

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  flacChunk = flacPoolPending ? NULL : player_chunk_get();
  if (flacChunk != NULL) {
    volatile uint32_t *sample = (volatile uint32_t *)flacChunk->payload;

    for (i = 0; i < frame->header.blocksize; i++) {
      // little endian, left channel in the lower half
      sample[i] = ((uint32_t)buffer[0][i] & 0xFFFF) |
                  ((uint32_t)buffer[1][i] << 16);
    }
    flacChunk->totalSize = frame->header.blocksize * 4;
  }

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 *
 */
void metadata_callback(const FLAC__StreamDecoder *decoder,
                       const FLAC__StreamMetadata *metadata,
                       void *client_data) {
  playerSetting_t *scSet = (playerSetting_t *)client_data;

  (void)decoder;

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    // ESP_LOGI(TAG, "in flac meta cb");

    // save for later
    scSet->sr = metadata->data.stream_info.sample_rate;
    scSet->ch = metadata->data.stream_info.channels;
    scSet->bits = metadata->data.stream_info.bits_per_sample;
    flacMaxBlocksize = metadata->data.stream_info.max_blocksize;

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
 * Snapclient state functions and types
 */

typedef enum { STOPPED = 0, IDLE, PLAYING, PAUSED } snapclient_state_t;
typedef enum { STOP = 0, START, RESTART, PAUSE, UNPAUSE } snapclient_commands_t;

typedef struct state_cb_s {
  void (*cb)(void);
  struct state_cb_s *next;
} state_cb_t;

static snapclient_state_t sc_state = STOPPED;
static state_cb_t *state_cb_head = NULL;

/**
 * Set mute state called by player
 */
void player_set_mute(bool mute) {
  set_mute_cb(mute, false);
}

/**
 * Set mute state called by snapclient
 */
void set_mute_state(bool mute) {
  set_mute_cb(mute, true);
}

/**
 * Sends command to http task, e.g. to start snapclient or pause player.
 */
void sc_send_command(snapclient_commands_t command) {
  if (t_http_get_task != NULL) {
    xTaskNotify(t_http_get_task, (uint32_t) command, eSetValueWithOverwrite);
  }
}

/**
 * Callback for player state changes, e.g. to send pause command to snapclient when player is paused.
 */
void on_player_state_paused(bool paused) {
  if (paused) {
    sc_send_command(PAUSE);
  } else {
    sc_send_command(UNPAUSE);
  }
}

void sc_start_snapclient() {
  sc_send_command(START);
}

void sc_restart_snapclient() {
  sc_send_command(RESTART);
}

void sc_stop_snapclient() {
  sc_send_command(STOP);
}

/**
 * Pause snapclient. This will first pause the player. Snapclient is then paused by the player callback.
 */
void sc_pause_snapclient(bool pause) {
  pause_player(pause);
}

/**
 * Get snapclient state
 */
snapclient_state_t sc_get_snapclient_state(void) {
  xSemaphoreTake(snapclientStateMux, portMAX_DELAY);
  snapclient_state_t state = sc_state;
  xSemaphoreGive(snapclientStateMux);
  return state;
}

void sc_call_state_cb(void) {
  state_cb_t *current = state_cb_head;
  while (current != NULL) {
    if (current->cb != NULL) {
      current->cb();
    }
    current = current->next;
  }
}

/**
 * add callback to be called when snapclient state changes, e.g. from not started to started.
 * Callbacks needs to be implemented thread safe as they will be called from http task
 */
void sc_add_state_cb(void (*cb)()) {
  state_cb_t *new_cb = malloc(sizeof(state_cb_t));
  if (new_cb == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for state callback");
    return;
  }
  new_cb->cb = cb;
  new_cb->next = state_cb_head;
  state_cb_head = new_cb;
}


/**
 *
 */
void server_settings_msg_received(
    server_settings_message_t *server_settings_message,
    snapcastSetting_t *scSet, bool playing) {
  // log mute state, buffer, latency
  ESP_LOGI(TAG, "Buffer length:  %ld", server_settings_message->buffer_ms);
  ESP_LOGI(TAG, "Latency:        %ld", server_settings_message->latency);
  ESP_LOGI(TAG, "Mute:           %d", server_settings_message->muted);
  ESP_LOGI(TAG, "Setting volume: %ld", server_settings_message->volume);

  // Volume setting using ADF HAL
  // abstraction
  if (playing && scSet->muted != server_settings_message->muted) {
#if SNAPCAST_USE_SOFT_VOL
    if (server_settings_message->muted) {
      dsp_processor_set_volome(0.0);
    } else {
      dsp_processor_set_volome((double)server_settings_message->volume / 100);
    }
#endif
    set_mute_state(server_settings_message->muted);
  }

  if (playing && scSet->volume != server_settings_message->volume) {
#if SNAPCAST_USE_SOFT_VOL
    if (!server_settings_message->muted) {
      dsp_processor_set_volome((double)server_settings_message->volume / 100);
    }
#else
    set_volume_cb(server_settings_message->volume);
#endif
  }

  scSet->muted = server_settings_message->muted;
  scSet->volume = server_settings_message->volume;

  if (scSet->playerSetting.cDacLat_ms != server_settings_message->latency ||
      scSet->playerSetting.buf_ms != server_settings_message->buffer_ms) {
    scSet->playerSetting.cDacLat_ms = server_settings_message->latency;
    scSet->playerSetting.buf_ms = server_settings_message->buffer_ms;

    // a different buffer may need a different number of chunks
    pcm_pool_ensure(&(scSet->playerSetting));

    if (playing && player_send_snapcast_setting(&(scSet->playerSetting)) != pdPASS) {
      ESP_LOGE(TAG,
               "Failed to notify sync task. "
               "Did you init player?");

      // critical error
      esp_restart();
    }
  }
}

/**
 * Stops the player and frees the pcm pool, so a new decoder gets its memory
 * before the pool of the new stream takes the rest
 */
static void pcm_pool_release(void) {
  stop_player_and_wait();
  if (pcm_pool_destroy() == ESP_OK) {
    memset(&poolKey, 0, sizeof(poolKey));
  }
  memset(&poolFailedKey, 0, sizeof(poolFailedKey));
}

static bool pool_key_equal(const pcm_pool_key_t *a, const pcm_pool_key_t *b) {
  return (a->slotBytes == b->slotBytes) && (a->slots == b->slots) &&
         (a->sr == b->sr) && (a->ch == b->ch) && (a->bits == b->bits);
}

/**
 * Makes sure the pcm pool fits the stream in setting, whose chkInFrames is
 * the largest chunk in frames. Rebuilds it if not, after stopping the player,
 * which still plays the old stream. Returns true if a fitting pool exists.
 */
static bool pcm_pool_ensure(playerSetting_t *setting) {
  pcm_pool_key_t key;

  if ((setting->buf_ms == 0) || (setting->chkInFrames == 0) ||
      (setting->sr <= 0) || (setting->ch == 0) || (setting->bits == 0)) {
    // not known yet
    return false;
  }
  if (flacPoolPending) {
    // built after the first FLAC frame
    return false;
  }

  memset(&key, 0, sizeof(key));
  key.slotBytes =
      (setting->chkInFrames * setting->ch * (setting->bits / 8) + 3) & ~3u;
  key.slots = player_chunk_slots(setting->sr, setting->chkInFrames,
                                 setting->buf_ms);
  key.sr = setting->sr;
  key.ch = setting->ch;
  key.bits = setting->bits;

  if (pcm_pool_ready() && pool_key_equal(&key, &poolKey)) {
    return true;
  }
  if (!pcm_pool_ready() && pool_key_equal(&key, &poolFailedKey)) {
    // didn't fit before, don't try again for every chunk
    return false;
  }

  stop_player_and_wait();

  if (pcm_pool_ready() && (key.slotBytes == poolKey.slotBytes) &&
      (key.slots == poolKey.slots)) {
    // same chunks, only the format changed: the player restarts with it
    pcm_pool_flush();
    poolKey = key;

    return true;
  }

  if (pcm_pool_destroy() != ESP_OK) {
    return false;
  }
  memset(&poolKey, 0, sizeof(poolKey));

  if (pcm_pool_build(key.slotBytes, key.slots) != ESP_OK) {
    poolFailedKey = key;
    ESP_LOGE(TAG,
             "server buffer of %u ms doesn't fit into RAM, playback stays "
             "stopped. Lower the buffer on the server (see README).",
             setting->buf_ms);

    return false;
  }

  memset(&poolFailedKey, 0, sizeof(poolFailedKey));
  poolKey = key;

  return true;
}

#if !CONFIG_SNAPCLIENT_DISABLE_OPUS
static void opus_scratch_free(void) {
  free(opusScratch);
  opusScratch = NULL;
  opusScratchBytes = 0;
}

static void opus_scratch_reserve(uint32_t bytes) {
  if ((opusScratch != NULL) && (opusScratchBytes >= bytes)) {
    return;
  }

  opus_scratch_free();
  opusScratch = (opus_int16 *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  if (opusScratch == NULL) {
    ESP_LOGE(TAG, "no memory for the opus decode buffer (%lu B)", bytes);
    return;
  }
  opusScratchBytes = bytes;
}
#endif

/**
 * Sets the timestamp of the frame decoded by the last
 * FLAC__stream_decoder_process_single() and queues it for playback
 */
static void flac_chunk_submit(playerSetting_t *scSet) {
  if (!flacAnchorValid) {
    // a frame from before the first wire chunk; can't happen after the
    // codec header
    player_chunk_discard(flacChunk);
    flacChunk = NULL;

    return;
  }

  int64_t us =
      flacAnchorUs + (int64_t)(flacAnchorFrames * 1000000ULL / scSet->sr);
  flacAnchorFrames += flacChunkFrames;

  if (flacChunk == NULL) {
    // dropped: no free chunk
    return;
  }

  flacChunk->timestamp.sec = us / 1000000LL;
  flacChunk->timestamp.usec = us % 1000000LL;

#if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_worker(flacChunk->payload, flacChunkFrames, scSet->sr,
                       scSet->ch);
#endif

  player_chunk_submit(flacChunk);
  flacChunk = NULL;
}

/**
 *
 */
void codec_header_received(char *codecPayload, uint32_t codecPayloadLen,
                           codec_type_t codec, playerSetting_t *scSet,
                           time_sync_data_t *time_sync_data) {
  // first ensure everything is set up
  // correctly and resources are
  // available

  if (flacDecoder != NULL) {
    FLAC__stream_decoder_finish(flacDecoder);
    FLAC__stream_decoder_delete(flacDecoder);
    flacDecoder = NULL;
  }

  // the input buffer is sized for the new codec below
  decoder_chunk_reserve(&decoderChunk, 0);

  // largest chunk of the new stream, set below (FLAC) or by the first chunk
  // (Opus, PCM)
  scSet->chkInFrames = 0;
  flacAnchorValid = false;
  flacPoolPending = false;

  // The old decoder is gone; the new one allocates below. Free the pool of
  // another codec first, its memory is needed now. With the same codec (e.g.
  // a reconnect) the new decoder fits where the old one was.
  if (codec != poolCodec) {
    pcm_pool_release();
    poolCodec = codec;
  }

#if !CONFIG_SNAPCLIENT_DISABLE_OPUS
  if (opusDecoder != NULL) {
    opus_decoder_destroy(opusDecoder);
    opusDecoder = NULL;
  }
  opus_scratch_free();

  if (codec == OPUS) {
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;

    memcpy(&rate, codecPayload + 4, sizeof(rate));
    memcpy(&bits, codecPayload + 8, sizeof(bits));
    memcpy(&channels, codecPayload + 10, sizeof(channels));

    scSet->bits = bits;
    scSet->ch = channels;
    scSet->sr = rate;

    ESP_LOGI(TAG, "Opus sample format: %ld:%d:%d\n", rate, bits, channels);

    int error = 0;

    opusDecoder = opus_decoder_create(scSet->sr, scSet->ch, &error);
    if (error != 0) {
      ESP_LOGI(TAG, "Failed to init opus coder");
      //critical error
      esp_restart();
    }

    ESP_LOGI(TAG, "Initialized opus Decoder: %d", error);

    decoder_chunk_reserve(&decoderChunk, OPUS_MAX_PACKET_BYTES);
  } else
#endif
  if (codec == FLAC) {
    flacInput = (const uint8_t *)codecPayload;
    flacInputBytes = codecPayloadLen;
    flacMaxBlocksize = 0;

    flacDecoder = FLAC__stream_decoder_new();
    if (flacDecoder == NULL) {
      ESP_LOGE(TAG, "Failed to init flac decoder");
      //critical error
      esp_restart();
    }

    FLAC__StreamDecoderInitStatus init_status =
        FLAC__stream_decoder_init_stream(
            flacDecoder, read_callback, NULL, NULL, NULL, NULL, write_callback,
            metadata_callback, error_callback, scSet);
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      ESP_LOGE(TAG, "ERROR: initializing decoder: %s\n",
               FLAC__StreamDecoderInitStatusString[init_status]);

      //critical error
      esp_restart();
    }

    FLAC__stream_decoder_process_until_end_of_metadata(flacDecoder);

    // codecPayload is freed after this returns. libFLAC reads the whole
    // header into its bitreader at once, so nothing should be left.
    if (flacInputBytes > 0) {
      ESP_LOGW(TAG, "%lu bytes of the FLAC codec header not read",
               flacInputBytes);
    }
    flacInput = NULL;
    flacInputBytes = 0;

    decoder_chunk_reserve(&decoderChunk,
                          flacMaxBlocksize * scSet->ch * ((scSet->bits + 7) / 8) +
                              FLAC_FRAME_OVERHEAD_BYTES);

    // write_callback() writes 16-bit stereo only
    if ((scSet->bits != 16) || (scSet->ch != 2)) {
      ESP_LOGE(TAG, "FLAC with %d bit, %d channels is not supported",
               scSet->bits, scSet->ch);
    } else {
      // pcm chunks hold one FLAC frame; the pool is built after the first
      // frame (see handle_chunk_message())
      scSet->chkInFrames = flacMaxBlocksize;
      flacPoolPending = true;
    }

    // ESP_LOGI(TAG, "%s: processed codec header",
    // __func__);
  } else if (codec == PCM) {
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;

    memcpy(&channels, codecPayload + 22, sizeof(channels));
    memcpy(&rate, codecPayload + 24, sizeof(rate));
    memcpy(&bits, codecPayload + 34, sizeof(bits));

    scSet->bits = bits;
    scSet->ch = channels;
    scSet->sr = rate;

    ESP_LOGI(TAG, "pcm sampleformat: %ld:%d:%d", scSet->sr, scSet->bits,
             scSet->ch);
  } else {
    ESP_LOGE(TAG,
             "codec header decoder "
             "shouldn't get here after "
             "codec string was detected");

    //critical error
    esp_restart();
  }

  if (player_send_snapcast_setting(scSet) != pdPASS) {
    ESP_LOGE(TAG,
             "Failed to notify sync task. "
             "Did you init player?");

    //critical error
    esp_restart();
  }

  // ESP_LOGI(TAG, "done codec header msg");

  // esp_timer_stop(time_sync_data->timeSyncMessageTimer);
  // if (!esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
  //   esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
  //                            time_sync_data->timeout);
  // }
}

/**
 * Decodes the wire chunk into pcm chunks and queues them for playback
 */
void handle_chunk_message(codec_type_t codec, playerSetting_t *scSet,
                          pcm_chunk_message_t **pcmData,
                          wire_chunk_message_t *wire_chnk) {
  switch (codec) {
#if !CONFIG_SNAPCLIENT_DISABLE_OPUS
    case OPUS: {
      if (decoderChunk.bytes == 0) {
        break;
      }

      int frames = opus_packet_get_nb_samples(decoderChunk.inData,
                                              decoderChunk.bytes, scSet->sr);
      if (frames <= 0) {
        ESP_LOGE(TAG, "invalid opus packet: %d", frames);
        break;
      }

      uint32_t bytes = (uint32_t)frames * scSet->ch * sizeof(opus_int16);
      if ((uint32_t)frames != scSet->chkInFrames) {
        // first packet, or the server changed the frame size
        scSet->chkInFrames = frames;
        // before the pool, which takes what is left
        opus_scratch_reserve(bytes);
        pcm_pool_ensure(scSet);
      }
      if (opusScratch == NULL) {
        break;
      }

      // decode even without a free chunk, the decoder keeps state
      int decoded = opus_decode(opusDecoder, decoderChunk.inData,
                                decoderChunk.bytes, opusScratch, frames, 0);
      if (decoded < 0) {
        ESP_LOGE(TAG, "opus_decode failed: %d", decoded);
        break;
      }

      pcm_chunk_message_t *chunk = player_chunk_get();
      if (chunk == NULL) {
        break;
      }

      bytes = (uint32_t)decoded * scSet->ch * sizeof(opus_int16);
      volatile uint32_t *sample = (volatile uint32_t *)chunk->payload;
      for (uint32_t i = 0, cnt = 0; i < bytes / 4; i++, cnt += 2) {
        sample[i] = (((uint32_t)opusScratch[cnt] << 16) & 0xFFFF0000) |
                    (((uint32_t)opusScratch[cnt + 1] << 0) & 0x0000FFFF);
      }
      chunk->totalSize = bytes;
      chunk->timestamp = wire_chnk->timestamp;

#if CONFIG_USE_DSP_PROCESSOR
      dsp_processor_worker(chunk->payload, decoded, scSet->sr, scSet->ch);
#endif

      player_chunk_submit(chunk);

      break;
    }
#endif

    case FLAC: {
      int64_t wireUs = (int64_t)wire_chnk->timestamp.sec * 1000000LL +
                       (int64_t)wire_chnk->timestamp.usec;
      bool anchorPending = true;

      isCachedChunk = true;

      flacInput = decoderChunk.inData;
      flacInputBytes = decoderChunk.bytes;

      // A frame decoded from data already in libFLAC's bitreader reads
      // nothing, but a wire chunk holds only a few frames
      uint32_t idleCalls = 0;
      while (flacInputBytes > 0) {
        uint32_t bytesBefore = flacInputBytes;

        flacChunk = NULL;
        flacChunkFrames = 0;
        FLAC__bool ok = FLAC__stream_decoder_process_single(flacDecoder);

        if ((flacChunkFrames > 0) && flacPoolPending) {
          // libFLAC has its output buffers now: the pool gets what is left.
          // This frame had no chunk and is dropped.
          flacPoolPending = false;
          pcm_pool_ensure(scSet);
        }

        if (flacChunkFrames > 0) {
          // the first frame that read this wire chunk starts at its
          // timestamp; frames before it continue the previous chunk
          if (!flacChunkCached && anchorPending) {
            flacAnchorUs = wireUs;
            flacAnchorFrames = 0;
            flacAnchorValid = true;
            anchorPending = false;
          }
          flac_chunk_submit(scSet);
        }

        idleCalls = (flacInputBytes == bytesBefore) ? idleCalls + 1 : 0;
        if (!ok || (idleCalls > FLAC_MAX_IDLE_CALLS)) {
          ESP_LOGE(TAG, "%s: FLAC decoder stuck (%s), dropping %lu bytes",
                   __func__,
                   FLAC__stream_decoder_get_resolved_state_string(flacDecoder),
                   flacInputBytes);

          // drop the rest of the chunk and search for the next frame sync
          flacInputBytes = 0;
          FLAC__stream_decoder_flush(flacDecoder);
        }
      }
      flacInput = NULL;

      break;
    }

    case PCM: {
      uint32_t frameBytes = (uint32_t)scSet->ch * (scSet->bits / 8);
      uint32_t frames = (frameBytes > 0) ? (wire_chnk->size / frameBytes) : 0;

      if (frames != scSet->chkInFrames) {
        // first chunk, or the server changed the chunk size: it sizes the
        // pool and is dropped
        player_chunk_discard(*pcmData);
        *pcmData = NULL;

        scSet->chkInFrames = frames;
        pcm_pool_ensure(scSet);

        break;
      }

      if (*pcmData) {
        (*pcmData)->timestamp = wire_chnk->timestamp;
        (*pcmData)->totalSize = wire_chnk->size & ~3u;

#if CONFIG_USE_DSP_PROCESSOR
        dsp_processor_worker((*pcmData)->payload, frames, scSet->sr,
                             scSet->ch);
#endif

        player_chunk_submit(*pcmData);
        *pcmData = NULL;
      }

      break;
    }

    default: {
      // This should not happen, because codec header message should have been
      // parsed before and codec should have been set to a supported value.
      ESP_LOGE(TAG, "Decoder (2) not supported. This should never happen!");
      // critical error
      esp_restart();

      break;
    }
  }
}

void update_state(bool *received_wire_chnk, bool *playback, bool paused) {
  static int64_t last = 0;
  // Read the shared state, not a private copy: a new connection resets
  // sc_state to IDLE, and a copy still saying PLAYING would never set it
  // back (codec stays stopped, Wi-Fi power save stays on)
  snapclient_state_t state = sc_get_snapclient_state();
  if ((paused || state != PLAYING) && (!paused || state != PAUSED) && *received_wire_chnk) {
    xSemaphoreTake(snapclientStateMux, portMAX_DELAY);
    if (paused) {
      sc_state = PAUSED;
      *playback = false;
      ESP_LOGI(TAG, "Set paused");
    }else{
      sc_state = PLAYING;
      ESP_LOGI(TAG, "Set playing");
      *playback = true;
    }
    xSemaphoreGive(snapclientStateMux);
    sc_call_state_cb();
    last = esp_timer_get_time();
    *received_wire_chnk = false;
  }
  else if (state == PLAYING || state == PAUSED) {
    int64_t now = esp_timer_get_time();
    if (now-last > 1000000) { //update once per sec
      if (!(*received_wire_chnk)) {
        xSemaphoreTake(snapclientStateMux, portMAX_DELAY);
        sc_state = IDLE;
        *playback = false;
        xSemaphoreGive(snapclientStateMux);
        sc_call_state_cb();
      ESP_LOGI(TAG, "Set idle");
      }
      last = now;
      *received_wire_chnk = false;
    }
  }

}


/*
 * returns:
 * 0 if a message was (partially) processed sucessfully
 * -1 if network needs restart
 */
int process_data(snapcast_protocol_parser_t *parser,
                 time_sync_data_t *time_sync_data, bool *received_codec_header,
                 codec_type_t *codec, snapcastSetting_t *scSet,
                 pcm_chunk_message_t **pcmData, bool *playback, bool paused) {
  base_message_t base_message_rx;

  static bool received_wire_chnk = false;
  update_state(&received_wire_chnk, playback, paused);

  if (parse_base_message(parser, &base_message_rx) != PARSER_OK) {
    return -1;  // restart connection
  }
  time_sync_data->now = esp_timer_get_time();
  int64_t rx_stamp = time_sync_data->now;
  if (base_message_rx.type == SNAPCAST_MESSAGE_TIME) {
    // Time sync needs the time the reply arrived, not when it got parsed:
    // it waits behind the audio queued before it (up to ~10 ms of decoding),
    // and half of that delay would end up in the server time offset
    int64_t arrived = connection_arrival_us(connection_bytes_consumed() - 1);
    if (arrived > 0) {
      rx_stamp = arrived;
    }
  }
  base_message_rx.received.sec = rx_stamp / 1000000;
  base_message_rx.received.usec = rx_stamp - base_message_rx.received.sec * 1000000;

  switch (base_message_rx.type) {
    case SNAPCAST_MESSAGE_WIRE_CHUNK: {
      wire_chunk_message_t wire_chnk = {{0, 0}, 0, NULL};  // is wire_chnk.payload ever used?
      received_wire_chnk = true;
      // skip this wires chunk message if codec header message was not received yet!
      if (*received_codec_header == false || paused) {
        if (parser_skip_typed_message(parser, &base_message_rx) != PARSER_OK) {
          return -1;
        }
        return 0;
      }

      if (parse_wire_chunk_message(parser, &base_message_rx, *codec, pcmData, &wire_chnk, &decoderChunk) != PARSER_OK) {
        return -1;
      }
      handle_chunk_message(*codec, &(scSet->playerSetting), pcmData, &wire_chnk);
      return 0;
    }

    case SNAPCAST_MESSAGE_CODEC_HEADER: {
      char *codecPayload = NULL;
      uint32_t codecPayloadLen = 0;
      int return_value = 0;
      if (parse_codec_header_message(parser, received_codec_header, codec, &codecPayload, &codecPayloadLen) != PARSER_OK) {
        return_value = -1;
      } else {
        codec_header_received(codecPayload, codecPayloadLen, *codec, &(scSet->playerSetting), time_sync_data);
      }

      // in all cases: free Payload
      if (codecPayload != NULL) {
        free(codecPayload);
      }

      return return_value;
    }

    case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
      server_settings_message_t server_settings_message;
      if (parse_sever_settings_message(parser, &base_message_rx, &server_settings_message) != PARSER_OK) {
        return -1;
      }
      server_settings_msg_received(&server_settings_message, scSet, *playback);
      return 0;
    }

    case SNAPCAST_MESSAGE_TIME: {
      time_message_t time_message_rx;
      if (parse_time_message(parser, &base_message_rx, &time_message_rx) != PARSER_OK) {
        return -1;
      }
      time_sync_msg_received(&base_message_rx, &time_message_rx, time_sync_data, *received_codec_header);
      return 0;
    }

    default: {
      if (parser_skip_typed_message(parser, &base_message_rx) != PARSER_OK) {
        return -1;
      }

      ESP_LOGI(TAG, "done skipping typed message %d", base_message_rx.type);
      return 0;
    }
  }
  // should never reach this
  return 0;
}

typedef struct
{
  time_sync_data_t *time_sync_data;
  bool *received_codec_header;
} before_receive_callback_data_t;


void before_receive_callback(before_receive_callback_data_t *data) {
  //unpack
  time_sync_data_t *time_sync_data = data->time_sync_data;
  bool received_codec_header = *data->received_codec_header;

  time_sync_data->now = esp_timer_get_time();
  // send time sync message
  if ((received_codec_header && (time_sync_data->now - time_sync_data->lastTimeSyncSent) >= time_sync_data->timeout)) {
    time_sync_msg_cb(NULL);
    time_sync_data->lastTimeSyncSent = time_sync_data->now;
    
    // ESP_LOGI(TAG, "time sync sent after %lluus", timeout);
  }
}


void network_state_cb(void) {
  static snapclient_state_t prev_state = STOPPED;
  snapclient_state_t state = sc_get_snapclient_state();
  if (state == PLAYING) {
    network_playback_started();
  } else if (prev_state == PLAYING) {
    network_playback_stopped();
  }
  prev_state = state;
}

/**
 *
 */
static void http_get_task(void *pvParameters) {
  connection_t connection;
  connection.firstNetBuf = NULL;
  connection.rc1 = ERR_OK;
  connection.netif = NULL;
  int rc1;  // for local scope (handshake), independent of connection.rc1
  base_message_t base_message_rx;
  hello_message_t hello_message;
  char *hello_message_serialized = NULL;
  static char device_hostname[64] = {0};  // Buffer for hostname
  int result;
  time_sync_data_t time_sync_data;
  time_sync_data.lastTimeSync = 0;
  time_sync_data.lastTimeSyncSent = 0;
  bool received_codec_header = false;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  pcm_chunk_message_t *pcmData = NULL;
  uint32_t command = STOP;
  bool paused = false;
  bool playback = false;

  // create a timer to send time sync messages every x µs
//  esp_timer_create(&tSyncArgs, &time_sync_data.timeSyncMessageTimer);

  idCounterSemaphoreHandle = xSemaphoreCreateMutex();
  if (idCounterSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "can't create id Counter Semaphore");

    esp_restart();
  }

  while (1) {
    // do some house keeping
    {
//      esp_timer_stop(time_sync_data.timeSyncMessageTimer);

      received_codec_header = false;

      xSemaphoreTake(idCounterSemaphoreHandle, portMAX_DELAY);
      id_counter = 0;
      xSemaphoreGive(idCounterSemaphoreHandle);

#if !CONFIG_SNAPCLIENT_DISABLE_OPUS
      if (opusDecoder != NULL) {
        opus_decoder_destroy(opusDecoder);
        opusDecoder = NULL;
      }
#endif

      if (flacDecoder != NULL) {
        FLAC__stream_decoder_finish(flacDecoder);
        FLAC__stream_decoder_delete(flacDecoder);
        flacDecoder = NULL;
      }

      decoder_chunk_reserve(&decoderChunk, 0);

      // a PCM chunk the connection broke off in
      player_chunk_discard(pcmData);
      pcmData = NULL;
#if !CONFIG_SNAPCLIENT_DISABLE_OPUS
      opus_scratch_free();
#endif

      if (decoderChunk.outData) {
        free(decoderChunk.outData);
        decoderChunk.outData = NULL;
      }
    }

    // block if state = STOPPED
    xSemaphoreTake(snapclientStateMux, portMAX_DELAY);
    if (sc_state == STOPPED) {
      xSemaphoreGive(snapclientStateMux);
      sc_call_state_cb();  // call callbacks here so we can be sure that netconn is closed. Player task could still be running.
      command = STOP;
      while(command != START) {
        xTaskNotifyWait( 0, 0, &command, portMAX_DELAY);
      }
      xSemaphoreTake(snapclientStateMux, portMAX_DELAY);
    }
    sc_state = IDLE;
    xSemaphoreGive(snapclientStateMux);
    sc_call_state_cb();
    playback = false;

    // NETWORK setup ends here ( or before getting mac address )
    setup_network(&connection.netif);

    //if (reset_latency_buffer() < 0) {
    //  ESP_LOGE(TAG,
    //           "reset_diff_buffer: couldn't reset median filter long. STOP");
    //  return;
    //}

    uint8_t base_mac[6];
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    // Get runtime MAC address for Eth Interface (reflects unified MAC if applied)
    char eth_mac_address[18];
    {
      esp_netif_t *eth_nif = network_get_netif_from_desc(NETWORK_INTERFACE_DESC_ETH);
      if (eth_nif && esp_netif_get_mac(eth_nif, base_mac) == ESP_OK) {
        // Use runtime MAC from netif (matches what's on the wire)
      } else {
        esp_read_mac(base_mac, ESP_MAC_ETH);  // fallback to eFuse
      }
    }
    snprintf(eth_mac_address, sizeof(eth_mac_address),
             "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
             base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    ESP_LOGI(TAG, "eth mac: %s", eth_mac_address);
#endif
    // Get MAC address for WiFi station
    char mac_address[18];
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
    snprintf(mac_address, sizeof(mac_address),
             "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
             base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    ESP_LOGI(TAG, "sta mac: %s", mac_address);

    time_sync_data.now = esp_timer_get_time();

    // init base message
    base_message_rx.type = SNAPCAST_MESSAGE_HELLO;
    xSemaphoreTake(idCounterSemaphoreHandle, portMAX_DELAY);
    base_message_rx.id = id_counter++;
    xSemaphoreGive(idCounterSemaphoreHandle);

    base_message_rx.refersTo = 0x0000;
    base_message_rx.sent.sec = time_sync_data.now / 1000000;
    base_message_rx.sent.usec =
        time_sync_data.now - base_message_rx.sent.sec * 1000000;
    base_message_rx.received.sec = 0;
    base_message_rx.received.usec = 0;
    base_message_rx.size = 0x00000000;

    // init hello message
    hello_message.mac = mac_address;

    // Get hostname from NVS or fallback to a sensible default
    if (settings_get_hostname(device_hostname, sizeof(device_hostname)) !=
        ESP_OK) {
      strncpy(device_hostname, "snapclient", sizeof(device_hostname) - 1);
    }
    hello_message.hostname = device_hostname;

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
        // critical error
        esp_restart();
      }
    }

    result = base_message_serialize(&base_message_rx, base_message_serialized,
                                    BASE_MESSAGE_SIZE);
    if (result) {
      ESP_LOGE(TAG, "Failed to serialize base message");
      // critical error
      esp_restart();
    }

    rc1 = netconn_write(lwipNetconn, base_message_serialized, BASE_MESSAGE_SIZE,
                        NETCONN_NOCOPY);
    rc1 |= netconn_write(lwipNetconn, hello_message_serialized,
                        base_message_rx.size, NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send hello message");

      continue;
    }

    ESP_LOGI(TAG, "netconn sent hello message");

    free(hello_message_serialized);
    hello_message_serialized = NULL;

    // init default setting
    scSet.playerSetting.buf_ms = 0;
    scSet.playerSetting.bits = 16;
    scSet.playerSetting.ch = 2;
    scSet.playerSetting.sr = 44100;
    scSet.playerSetting.chkInFrames = 0;
    scSet.muted = true;

    snapcast_protocol_parser_t parser;

    // state machine starts here

    before_receive_callback_data_t before_receive_callback_data;
    before_receive_callback_data.received_codec_header = &received_codec_header;
    before_receive_callback_data.time_sync_data = &time_sync_data;
    connection.before_receive_callback = (void (*)(void *))before_receive_callback;
    connection.before_receive_callback_data = (void*) &before_receive_callback_data;
    connection.isMuted = &scSet.muted;

    connection.firstNetBuf = NULL;
    connection.first_receive = true;
    connection.first_netbuf_processed = false;

    connection.state = CONNECTION_INITIALIZED;

    parser.get_byte_context = &connection;
    parser.get_byte_function = (get_byte_callback_t)(&connection_get_byte);


    // as we need fast time syncs in the beginning we set receive timeout very low
    time_sync_data.timeout = FAST_SYNC_LATENCY_BUF;
    netconn_set_recvtimeout(lwipNetconn, time_sync_data.timeout / 1000); // timeout in ms


    // Main connection loop - state machine + data processing
    while (1) {
      static bool playback_old = false;

      if (playback_old != playback) {
        if (playback) {
          // need to apply settings when starting to play
#if SNAPCAST_USE_SOFT_VOL
          if (!scSet.muted) {
            dsp_processor_set_volome((double)scSet.volume / 100);
          } else {
            dsp_processor_set_volome(0.0);
          }
#else
          set_volume_cb(scSet.volume);
#endif
          set_mute_state(scSet.muted);
        }
        playback_old = playback;
      }

      int result =
          process_data(&parser, &time_sync_data, &received_codec_header, &codec,
                       &scSet, &pcmData, &playback, paused);

      // Handle commands after process_data() so a notification arriving during
      // the blocking recv is acted upon instead of only being drained.
      bool restart = false;
      bool stopped = false;
      if (xTaskNotifyWait(0, 0, &command, 1) == pdTRUE) {
        switch(command) {
          case STOP:
            stop_player_task();  // stop player task for faster teardown
            xSemaphoreTake(snapclientStateMux, portMAX_DELAY);
            sc_state = STOPPED;
            xSemaphoreGive(snapclientStateMux);
            stopped = true;  // close connection and wait for START command
            break;
          case RESTART:
            restart = true;
            break;
          case UNPAUSE:
            paused = false;
            break;
          case PAUSE:
            paused = true;
            break;
          default:
            break;
        }
      //ESP_LOGI(TAG, "http got cb. %s", paused ? "paused" : "playing/idle");
      }

      if (restart || stopped) {
        netconn_close(lwipNetconn);
        netconn_delete(lwipNetconn);
        lwipNetconn = NULL;
      }

      // No back-off when stopping, the outer loop blocks on START anyway
      if (!stopped && (restart || result != 0)) {
        vTaskDelay(pdMS_TO_TICKS(2000)); // back-off before reconnecting
      }

      if (restart || stopped || result != 0) {
        break; // restart connection
      }
    }
  }
}

/**
 *
 */
int init_snapclient(void (*set_volume)(int), void (*set_mute)(bool, bool), i2s_std_gpio_config_t i2s_pin_config0, i2s_port_t I2S_NUM_0, bool (*lock)(bool, TickType_t)) {
  if (set_volume == NULL) {
    ESP_LOGE(TAG, "Volume callback is NULL");

    return -1;
  }
  if (set_mute == NULL) {
    ESP_LOGE(TAG, "Mute callback is NULL");

    return -1;
  }
  set_volume_cb = set_volume;
  set_mute_cb = set_mute;
  if (snapclientStateMux == NULL) {
    snapclientStateMux = xSemaphoreCreateMutex();
  }
  init_player(i2s_pin_config0, I2S_NUM_0, player_set_mute, on_player_state_paused, lock);

  xTaskCreatePinnedToCore(&http_get_task, "http", HTTP_TASK_STACK_SIZE, NULL,
                        HTTP_TASK_PRIORITY, &t_http_get_task,
                        HTTP_TASK_CORE_ID);

  return 0;
}


/**
 *
 */
static void dac_control(audio_board_handle_t board_handle,
                             audioDACdata_t dac_data) {
  static audioDACdata_t dac_data_old = {
      .playerMute = true,
      .stateMute = true,
      .volume = -1,
  };
  static bool muted = true;
  // TODO: can and should we pass audio_hal_handle_t instead of
  // audio_board_handle_t?
  if (dac_data.playerMute != dac_data_old.playerMute ||
      dac_data.stateMute != dac_data_old.stateMute) {
    // if either player or state mute is active, we need to mute the output
    bool mute = dac_data.playerMute || dac_data.stateMute;
    if (mute != muted) {
      muted = mute;
      audio_hal_set_mute(board_handle->audio_hal, muted);
    }
  }
  if (dac_data.volume != dac_data_old.volume) {
    audio_hal_set_volume(board_handle->audio_hal, dac_data.volume);
  }
    dac_data_old = dac_data;
}

/**
 * Set mute state. If set_state is true, it reflects the snapclient state. Otherwise it
 * is coming from player for temporary muting e.g. during startup.
 */
void audio_set_mute(bool mute, bool set_state) {
  xSemaphoreTake(audioDACSemaphore, portMAX_DELAY);
  if (set_state && (mute != audioDAC_data.stateMute)) {
    audioDAC_data.stateMute = mute;
    xQueueOverwrite(audioDACQHdl, &audioDAC_data);
  }
  else if (!set_state && mute != audioDAC_data.playerMute) {
    audioDAC_data.playerMute = mute;
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
 * Callback for snapclient state changes (playing, paused, idle). Used to trigger actions on state changes, e.g. muting the audio output when paused or idle.
 */
void on_sc_state_changed() {
  if (snapclientStateChangedMutex != NULL) {
    xSemaphoreGive(snapclientStateChangedMutex);
  }
  ESP_LOGI(TAG, "main task cb");
}

bool i2s_lock(bool lock, TickType_t wait) {
  if (i2sLockMutex == NULL) {
    return false;
  }
  if (lock) {
    return xSemaphoreTake(i2sLockMutex, wait);
  }
  else {
    return xSemaphoreGive(i2sLockMutex);
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
//  ESP_ERROR_CHECK(nvs_flash_erase());
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
  esp_log_level_set("httpd_uri", ESP_LOG_WARN);
  esp_log_level_set("settings", ESP_LOG_DEBUG);
  esp_log_level_set("dsp_settings", ESP_LOG_DEBUG);
  esp_log_level_set("UI_HTTP", ESP_LOG_WARN);
  esp_log_level_set("dspProc", ESP_LOG_DEBUG);

  t_main_task = xTaskGetCurrentTaskHandle();

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

  // Initialize settings and network early so connection starts during codec init
  settings_manager_init();
  network_events_init();
  network_if_init();
  heap_diag_init();

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

  audioDACQHdl = xQueueCreate(1, sizeof(audioDACdata_t));
  audioDACSemaphore = xSemaphoreCreateMutex();
  audioDAC_data.stateMute = true;
  audioDAC_data.playerMute = true;
  audioDAC_data.volume = -1;

  i2sLockMutex = xSemaphoreCreateBinary();

  init_snapclient(audio_set_volume, audio_set_mute, i2s_pin_config0, I2S_NUM_0, i2s_lock);
  //init_player(i2s_pin_config0, I2S_NUM_0, player_set_mute);
  sc_add_state_cb(on_sc_state_changed);
  sc_add_state_cb(network_state_cb);

  // Create binary semaphore for player state change notification
  snapclientStateChangedMutex = xSemaphoreCreateBinary();
  if (snapclientStateChangedMutex == NULL) {
    ESP_LOGE(TAG, "Failed to create snapclientStateChangedMutex");
    return;
  }

  #if CONFIG_DAC_TAS5805M
  // Apply persisted TAS5805M settings now that the codec has been initialized
  if (tas5805m_settings_init() != ESP_OK) {
    ESP_LOGW(TAG, "Failed to init persisted TAS5805M settings");
  }
  #endif

  // Get hostname for mDNS
  char mdns_hostname[64] = {0};
  if (settings_get_hostname(mdns_hostname, sizeof(mdns_hostname)) != ESP_OK) {
    strncpy(mdns_hostname, "snapclient", sizeof(mdns_hostname) - 1);
  }
  ESP_LOGI(TAG, "Device hostname: %s", mdns_hostname);
  
  #if CONFIG_ESP32_UDP_LOGGER_ENABLED
//  esp32_udp_logger_set_hostname(mdns_hostname);
  esp32_udp_logger_autostart();
  #endif

  #if CONFIG_USE_DSP_PROCESSOR
  dsp_processor_init();  // Must init processor first (creates mutexes/semaphores)
  dsp_settings_init();   // Then settings can restore params into the processor
  #endif

  #if CONFIG_SNAPCLIENT_ENABLE_WEB_UI
  init_http_server();
  #endif

  // Enable websocket server
  //  ESP_LOGI(TAG, "Setup ws server");
  //  websocket_if_start();

  net_mdns_register(mdns_hostname);
#ifdef CONFIG_SNAPCLIENT_SNTP_ENABLE
  set_time_from_sntp();
#endif

#if !CONFIG_SNAPCLIENT_WEB_OTA
  // Booting this far counts as a working image: keep it instead of rolling
  // back to the previous OTA partition on the next reset. With web OTA this
  // is done once the web server runs.
  esp_ota_mark_app_valid_cancel_rollback();
#endif

  sc_start_snapclient();

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

#if CONFIG_PM_ENABLE
  // Configure dynamic frequency scaling:
  // automatic light sleep is enabled if tickless idle support is enabled.
  esp_pm_config_t pmConfig = {
      .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,  // Maximum CPU frequency
      .min_freq_mhz = 40,                               // Minimum CPU frequency
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
      .light_sleep_enable = true
#endif
  };
  esp_pm_configure(&pmConfig);
#endif
  heap_diag_mark("app_main init done");

  audioDACdata_t dac_data;
  snapclient_state_t state = IDLE;
  while (1) {
    if (xQueueReceive(audioDACQHdl, &dac_data, pdMS_TO_TICKS(90)) == pdTRUE) {
      dac_control(board_handle, dac_data);
    }
    if (xSemaphoreTake(snapclientStateChangedMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      snapclient_state_t state_new = sc_get_snapclient_state();
      ESP_LOGI(TAG, "main got cb: %d", state_new);
      if (state_new != state) {
        ESP_LOGI(TAG, "Player state changed: %d -> %d", state, state_new);
        if (state_new == PLAYING) {
          audio_hal_ctrl_codec(board_handle->audio_hal,
                                AUDIO_HAL_CODEC_MODE_DECODE,
                                AUDIO_HAL_CTRL_START);
        } else if (state == PLAYING) {
          audio_hal_ctrl_codec(board_handle->audio_hal,
                                AUDIO_HAL_CODEC_MODE_DECODE,
                                AUDIO_HAL_CTRL_STOP);
        }
        if ((state_new == PLAYING) != (state == PLAYING)) {
          heap_diag_set_playing(state_new == PLAYING);
        }
        state = state_new;
      }
    }
  }
}

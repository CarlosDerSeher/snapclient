#ifndef __PLAYER_H__
#define __PLAYER_H__

#include "driver/i2s_std.h"
#include "esp_types.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "snapcast.h"

#ifdef __cplusplus
extern "C" {
#endif

#define I2S_PORT I2S_NUM_0

// TODO: maybe calculate this dynamically based on chunk duration and buffer
// size?!
#define CHNK_CTRL_CNT 2

#define LATENCY_MEDIAN_FILTER_LEN 199
#define LATENCY_MEDIAN_FILTER_FULL 19

// set to 0 if you do not wish to be the median an average around actual
// median average will be (LATENCY_MEDIAN_FILTER_LEN /
// LATENCY_MEDIAN_AVG_DIVISOR) + 1 samples around median. e.g. if n=4 then
// 2 samples above and below will be added plus the actual median. So in
// reality n+1 samples will be averaged
#define LATENCY_MEDIAN_AVG_DIVISOR 0

#define SHORT_BUFFER_LEN 99
#define MINI_BUFFER_LEN 19

typedef struct pcm_chunk_fragment pcm_chunk_fragment_t;
struct pcm_chunk_fragment {
  size_t size;
  char *payload;
  pcm_chunk_fragment_t *nextFragment;
};

typedef struct pcmData {
  tv_t timestamp;
  size_t totalSize;
  pcm_chunk_fragment_t *fragment;
  uint32_t caps;
} pcm_chunk_message_t;

typedef enum codec_type_e { NONE = 0, PCM, FLAC, OGG, OPUS } codec_type_t;

typedef struct snapcastSetting_s {
  uint32_t buf_ms;
  uint32_t chkInFrames;
  int32_t cDacLat_ms;

  codec_type_t codec;
  int32_t sr;
  uint8_t ch;
  i2s_data_bit_width_t bits;

  bool muted;
  uint32_t volume;

  char *pcmBuf;
  uint32_t pcmBufSize;
} snapcastSetting_t;

int init_player(i2s_std_gpio_config_t pin_config0_, i2s_port_t i2sNum_);
int deinit_player(void);

int32_t allocate_pcm_chunk_memory(pcm_chunk_message_t **pcmChunk, size_t bytes);
int32_t insert_pcm_chunk(pcm_chunk_message_t *pcmChunk);

// int8_t insert_pcm_chunk (wire_chunk_message_t *decodedWireChunk);
int8_t free_pcm_chunk(pcm_chunk_message_t *pcmChunk);

int32_t player_latency_insert(int64_t newValue);
int32_t player_send_snapcast_setting(snapcastSetting_t *setting);
int8_t player_get_snapcast_settings(snapcastSetting_t *setting);

int32_t reset_latency_buffer(void);
int32_t latency_buffer_full(bool *is_full, TickType_t wait);
int32_t get_diff_to_server(int64_t *tDiff);
int32_t server_now(int64_t *sNow, int64_t *diff2Server);

int32_t pcm_chunk_queue_msg_waiting(void);
#ifdef __cplusplus
}
#endif
#endif  // __PLAYER_H__

#ifndef __PLAYER_H__
#define __PLAYER_H__

#include "driver/i2s_std.h"
#include "esp_types.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "pcm_pool.h"
#include "snapcast.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USE_TIMEFILTER  CONFIG_SNAPCLIENT_USE_TIMEFILTER

#define I2S_PORT I2S_NUM_0

// TODO: maybe calculate this dynamically based on chunk duration and buffer
// size?!
#define CHNK_CTRL_CNT 2

//#define LATENCY_MEDIAN_FILTER_LEN 199
#define LATENCY_TIME_FILTER_FULL 29

// set to 0 if you do not wish to be the median an average around actual
// median average will be (LATENCY_MEDIAN_FILTER_LEN /
// LATENCY_MEDIAN_AVG_DIVISOR) + 1 samples around median. e.g. if n=4 then
// 2 samples above and below will be added plus the actual median. So in
// reality n+1 samples will be averaged
#define LATENCY_MEDIAN_AVG_DIVISOR 0

#define LATENCY_MEDIAN_FILTER_LEN 199
#define LATENCY_MEDIAN_FILTER_FULL 19

#define SHORT_BUFFER_LEN 99
#define MINI_BUFFER_LEN 19

typedef enum codec_type_e { NONE = 0, PCM, FLAC, OGG, OPUS } codec_type_t;

typedef struct playerSetting_s {
  uint16_t buf_ms;
  uint16_t chkInFrames;
  int16_t cDacLat_ms;
  int32_t sr;
  uint8_t ch;
  i2s_data_bit_width_t bits;
} playerSetting_t;

int init_player(i2s_std_gpio_config_t pin_config0_, i2s_port_t i2sNum_, void (*set_mute_cb)(bool), void (*cb)(bool),  bool (*lock)(bool, TickType_t));
int deinit_player(void);
int start_player(void);
void pause_player(bool pause);
void stop_player_task(void);

void stop_player_and_wait(void);

// PCM chunks for buf_ms of audio in chunks of chkInFrames frames, including
// the chunk being decoded and the one being played (see pcm_pool.h)
uint32_t player_chunk_slots(int32_t sr, uint32_t chkInFrames, uint32_t buf_ms);

// decoder side of the pcm pool: get a free chunk (NULL: drop the audio), then
// submit it for playback or discard it
pcm_chunk_message_t *player_chunk_get(void);
void player_chunk_submit(pcm_chunk_message_t *chunk);
void player_chunk_discard(pcm_chunk_message_t *chunk);

#if USE_TIMEFILTER
int32_t player_latency_insert(int64_t newValue, int64_t max_error, int64_t time_added);
#else
int32_t player_latency_insert(int64_t newValue);
#endif

int32_t get_diff_to_server(int64_t *tDiff, int64_t now);
int32_t latency_buffer_full(bool *is_full);

int32_t player_send_snapcast_setting(playerSetting_t *setting);

int32_t reset_latency_buffer(void);

int32_t server_now(int64_t *sNow, int64_t *diff2Server);

int32_t pcm_chunk_queue_msg_waiting(void);
void player_get_buffer_fill(uint32_t *waiting, uint32_t *length,
                            uint32_t *chunkUs, uint32_t *dmaUs);
#ifdef __cplusplus
}
#endif
#endif  // __PLAYER_H__

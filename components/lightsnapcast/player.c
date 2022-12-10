/**
 *
 */

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

//#include "lwip/stats.h"

#include "esp_log.h"

#include "esp_wifi.h"

#include "soc/rtc.h"

#include "driver/timer.h"

#include "MedianFilter.h"
#include "board_pins_config.h"
#include "player.h"
#include "snapcast.h"

#include "i2s.h"  // use custom i2s driver instead of IDF version

#include <math.h>

#define SYNC_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define SYNC_TASK_CORE_ID 1  // tskNO_AFFINITY

static const char *TAG = "PLAYER";

/**
 * @brief Pre define APLL parameters, save compute time. They are calculated in
 * player_setup_i2s() | bits_per_sample | rate | sdm0 | sdm1 | sdm2 | odir
 *
 * apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536)/((o_div + 2) * 2)
 * I2S bit clock is (apll_freq / 16)
 */
static int apll_normal_predefine[6] = {0, 0, 0, 0, 0, 0};
static int apll_corr_predefine[][6] = {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}};

static SemaphoreHandle_t latencyBufSemaphoreHandle = NULL;

static bool latencyBuffFull = 0;

static sMedianFilter_t latencyMedianFilter;
static sMedianNode_t latencyMedianLong[LATENCY_MEDIAN_FILTER_LEN];

static sMedianFilter_t shortMedianFilter;
static sMedianNode_t shortMedianBuffer[SHORT_BUFFER_LEN];

static sMedianFilter_t miniMedianFilter;
static sMedianNode_t miniMedianBuffer[MINI_BUFFER_LEN];

static int64_t latencyToServer = 0;

static int8_t currentDir = 0;  //!< current apll direction, see apll_adjust()

static QueueHandle_t pcmChkQHdl = NULL;

static TaskHandle_t playerTaskHandle = NULL;

static QueueHandle_t snapcastSettingQueueHandle = NULL;

static size_t chkInBytes;

static uint32_t i2sDmaBufCnt;
static uint32_t i2sDmaBufMaxLen;

static SemaphoreHandle_t snapcastSettingsMux = NULL;
static snapcastSetting_t currentSnapcastSetting;

static void tg0_timer_init(void);
static void tg0_timer_deinit(void);
static void player_task(void *pvParameters);

/*
#define CONFIG_MASTER_I2S_BCK_PIN 5
#define CONFIG_MASTER_I2S_LRCK_PIN 25
#define CONFIG_MASTER_I2S_DATAOUT_PIN 26
#define CONFIG_SLAVE_I2S_BCK_PIN 26
#define CONFIG_SLAVE_I2S_LRCK_PIN 12
#define CONFIG_SLAVE_I2S_DATAOUT_PIN 5
*/

/**
 *
 */
static esp_err_t player_setup_i2s(i2s_port_t i2sNum,
                                  snapcastSetting_t *setting) {
  int chunkInFrames;
  int __dmaBufCnt;
  int __dmaBufLen;
  const int __dmaBufMaxLen = 1024;
  int m_scale = 8, fi2s_clk;

  chkInBytes =
      (setting->chkDur_ms * setting->sr * setting->ch * (setting->bits / 8)) /
      1000;
  chunkInFrames = chkInBytes / (setting->ch * (setting->bits / 8));

  __dmaBufCnt = 1;
  __dmaBufLen = chunkInFrames;
  while ((__dmaBufLen >= __dmaBufMaxLen) || (__dmaBufCnt <= 1)) {
    if ((__dmaBufLen % 2) == 0) {
      __dmaBufCnt *= 2;
      __dmaBufLen /= 2;
    } else {
      ESP_LOGE(TAG,
               "player_setup_i2s: Can't setup i2s with this configuration");

      return -1;
    }
  }

  i2sDmaBufCnt = __dmaBufCnt * CHNK_CTRL_CNT;
  i2sDmaBufMaxLen = __dmaBufLen;

  fi2s_clk = setting->sr * setting->ch * setting->bits * m_scale;

  apll_normal_predefine[0] = setting->bits;
  apll_normal_predefine[1] = setting->sr;
  if (i2s_apll_calculate_fi2s(
          fi2s_clk, setting->bits, &apll_normal_predefine[2],
          &apll_normal_predefine[3], &apll_normal_predefine[4],
          &apll_normal_predefine[5]) != ESP_OK) {
    ESP_LOGE(TAG, "ERROR, fi2s_clk");
  }

#define UPPER_SR_SCALER 1.0001
#define LOWER_SR_SCALER 0.9999

  apll_corr_predefine[0][0] = setting->bits;
  apll_corr_predefine[0][1] = setting->sr * UPPER_SR_SCALER;
  if (i2s_apll_calculate_fi2s(
          fi2s_clk * UPPER_SR_SCALER, setting->bits, &apll_corr_predefine[0][2],
          &apll_corr_predefine[0][3], &apll_corr_predefine[0][4],
          &apll_corr_predefine[0][5]) != ESP_OK) {
    ESP_LOGE(TAG, "ERROR, fi2s_clk * %f", UPPER_SR_SCALER);
  }
  apll_corr_predefine[1][0] = setting->bits;
  apll_corr_predefine[1][1] = setting->sr * LOWER_SR_SCALER;
  if (i2s_apll_calculate_fi2s(
          fi2s_clk * LOWER_SR_SCALER, setting->bits, &apll_corr_predefine[1][2],
          &apll_corr_predefine[1][3], &apll_corr_predefine[1][4],
          &apll_corr_predefine[1][5]) != ESP_OK) {
    ESP_LOGE(TAG, "ERROR, fi2s_clk * %f", LOWER_SR_SCALER);
  }

  ESP_LOGI(TAG, "player_setup_i2s: dma_buf_len is %d, dma_buf_count is %d",
           i2sDmaBufMaxLen, i2sDmaBufCnt);

  i2s_config_t i2s_config0 = {
      .mode = I2S_MODE_MASTER | I2S_MODE_TX,  // Only TX
      .sample_rate = setting->sr,
      .bits_per_sample = setting->bits,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,  // 2-channels
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .dma_buf_count = i2sDmaBufCnt,
      .dma_buf_len = i2sDmaBufMaxLen,
      .intr_alloc_flags = 1,  // Default interrupt priority
      .use_apll = true,
      .fixed_mclk = 0,
      .tx_desc_auto_clear = true  // Auto clear tx descriptor on underflow
  };

  i2s_pin_config_t pin_config0;
  get_i2s_pins(i2sNum, &pin_config0);

  i2s_custom_driver_uninstall(i2sNum);
  i2s_custom_driver_install(i2sNum, &i2s_config0, 0, NULL);
  i2s_custom_set_pin(i2sNum, &pin_config0);

  return 0;
}

/**
 *
 */
static int destroy_pcm_queue(QueueHandle_t *queueHandle) {
  int ret = pdPASS;
  pcm_chunk_message_t *chnk = NULL;

  if (*queueHandle == NULL) {
    ESP_LOGW(TAG, "no pcm chunk queue created?");
    ret = pdFAIL;
  } else {
    // free all allocated memory
    while (uxQueueMessagesWaiting(*queueHandle)) {
      ret = xQueueReceive(*queueHandle, &chnk, pdMS_TO_TICKS(2000));
      if (ret != pdFAIL) {
        if (chnk != NULL) {
          free_pcm_chunk(chnk);
        }
      }
    }

    // delete the queue
    vQueueDelete(*queueHandle);
    *queueHandle = NULL;

    ret = pdPASS;
  }

  return ret;
}

/**
 * ensure this is called after http_task was killed!
 */
int deinit_player(void) {
  int ret = 0;

  // stop the task
  if (playerTaskHandle == NULL) {
    ESP_LOGW(TAG, "no sync task created?");
  } else {
    vTaskDelete(playerTaskHandle);
  }

  if (snapcastSettingsMux != NULL) {
    vSemaphoreDelete(snapcastSettingsMux);
    snapcastSettingsMux = NULL;
  }

  ret = destroy_pcm_queue(&pcmChkQHdl);

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGW(TAG, "no latency buffer semaphore created?");
  } else {
    vSemaphoreDelete(latencyBufSemaphoreHandle);
    latencyBufSemaphoreHandle = NULL;
  }

  tg0_timer_deinit();

  ESP_LOGI(TAG, "deinit player done");

  return ret;
}

/**
 *  call before http task creation!
 */
int init_player(void) {
  int ret = 0;

  currentSnapcastSetting.buf_ms = 1000;
  currentSnapcastSetting.chkDur_ms = 20;
  currentSnapcastSetting.codec = NONE;
  currentSnapcastSetting.sr = 44100;
  currentSnapcastSetting.ch = 2;
  currentSnapcastSetting.bits = 16;
  currentSnapcastSetting.muted = false;
  currentSnapcastSetting.volume = 70;

  if (snapcastSettingsMux == NULL) {
    snapcastSettingsMux = xSemaphoreCreateMutex();
    xSemaphoreGive(snapcastSettingsMux);
  }

  ret = player_setup_i2s(I2S_NUM_0, &currentSnapcastSetting);
  if (ret < 0) {
    ESP_LOGE(TAG, "player_setup_i2s failed: %d", ret);

    return -1;
  }

  // create semaphore for time diff buffer to server
  if (latencyBufSemaphoreHandle == NULL) {
    latencyBufSemaphoreHandle = xSemaphoreCreateMutex();
  }

  // init diff buff median filter
  latencyMedianFilter.numNodes = LATENCY_MEDIAN_FILTER_LEN;
  latencyMedianFilter.medianBuffer = latencyMedianLong;
  reset_latency_buffer();

  shortMedianFilter.numNodes = SHORT_BUFFER_LEN;
  shortMedianFilter.medianBuffer = shortMedianBuffer;
  MEDIANFILTER_Init(&shortMedianFilter);

  miniMedianFilter.numNodes = MINI_BUFFER_LEN;
  miniMedianFilter.medianBuffer = miniMedianBuffer;
  MEDIANFILTER_Init(&miniMedianFilter);

  tg0_timer_init();

  if (playerTaskHandle == NULL) {
    ESP_LOGI(TAG, "Start player_task");

    xTaskCreatePinnedToCore(player_task, "player", 2048 + 512, NULL,
                            SYNC_TASK_PRIORITY, &playerTaskHandle,
                            SYNC_TASK_CORE_ID);
  }

  ESP_LOGI(TAG, "init player done");

  return 0;
}

/**
 *
 */
int8_t player_set_snapcast_settings(snapcastSetting_t *setting) {
  int8_t ret = pdPASS;

  xSemaphoreTake(snapcastSettingsMux, portMAX_DELAY);

  memcpy(&currentSnapcastSetting, setting, sizeof(snapcastSetting_t));

  xSemaphoreGive(snapcastSettingsMux);

  return ret;
}

/**
 *
 */
int8_t player_get_snapcast_settings(snapcastSetting_t *setting) {
  int8_t ret = pdPASS;

  xSemaphoreTake(snapcastSettingsMux, portMAX_DELAY);

  memcpy(setting, &currentSnapcastSetting, sizeof(snapcastSetting_t));

  xSemaphoreGive(snapcastSettingsMux);

  return ret;
}

int32_t player_latency_insert(int64_t newValue) {
  int64_t medianValue;

  medianValue = MEDIANFILTER_Insert(&latencyMedianFilter, newValue);
  if (xSemaphoreTake(latencyBufSemaphoreHandle, pdMS_TO_TICKS(0)) == pdTRUE) {
    if (MEDIANFILTER_isFull(&latencyMedianFilter)) {
      latencyBuffFull = true;

      //      ESP_LOGI(TAG, "(full) latency median: %lldus", medianValue);
    }
    //    else {
    //      ESP_LOGI(TAG, "(not full) latency median: %lldus", medianValue);
    //    }

    latencyToServer = medianValue;

    xSemaphoreGive(latencyBufSemaphoreHandle);
  } else {
    ESP_LOGW(TAG, "couldn't set latencyToServer = medianValue");
  }

  return 0;
}

/**
 *
 */
int32_t player_send_snapcast_setting(snapcastSetting_t *setting) {
  int ret;
  snapcastSetting_t curSet;
  uint8_t settingChanged = 1;

  if ((playerTaskHandle == NULL) || (snapcastSettingQueueHandle == NULL)) {
    return pdFAIL;
  }

  ret = player_get_snapcast_settings(&curSet);

  if ((curSet.bits != setting->bits) || (curSet.buf_ms != setting->buf_ms) ||
      (curSet.ch != setting->ch) || (curSet.chkDur_ms != setting->chkDur_ms) ||
      (curSet.codec != setting->codec) || (curSet.muted != setting->muted) ||
      (curSet.sr != setting->sr) || (curSet.volume != setting->volume) ||
      (curSet.cDacLat_ms != setting->cDacLat_ms)) {
    // check if it is only volume / mute related setting, which is handled by
    // http_get_task()
    if (((curSet.muted != setting->muted) ||
         (curSet.volume != setting->volume)) &&
        ((curSet.bits == setting->bits) && (curSet.buf_ms == setting->buf_ms) &&
         (curSet.ch == setting->ch) &&
         (curSet.chkDur_ms == setting->chkDur_ms) &&
         (curSet.codec == setting->codec) && (curSet.sr == setting->sr) &&
         (curSet.cDacLat_ms == setting->cDacLat_ms))) {
      // no notify needed, only set changed parameters
      ret = player_set_snapcast_settings(setting);
      if (ret != pdPASS) {
        ESP_LOGE(TAG,
                 "player_send_snapcast_setting: couldn't change "
                 "snapcast setting");
      }
    } else {
      ret = xQueueOverwrite(snapcastSettingQueueHandle, &settingChanged);
      if (ret != pdPASS) {
        ESP_LOGE(TAG,
                 "player_send_snapcast_setting: couldn't notify "
                 "snapcast setting");
      } else {
        // notify successful, so change parameters
        ret = player_set_snapcast_settings(setting);
        if (ret != pdPASS) {
          ESP_LOGE(TAG,
                   "player_send_snapcast_setting: couldn't "
                   "change snapcast setting");
        }
      }
    }
  }

  return pdPASS;
}

/**
 *
 */
int32_t reset_latency_buffer(void) {
  // init diff buff median filter
  if (MEDIANFILTER_Init(&latencyMedianFilter) < 0) {
    ESP_LOGE(TAG, "reset_diff_buffer: couldn't init median filter long. STOP");

    return -2;
  }

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "reset_diff_buffer: latencyBufSemaphoreHandle == NULL");

    return -2;
  }

  if (xSemaphoreTake(latencyBufSemaphoreHandle, portMAX_DELAY) == pdTRUE) {
    latencyBuffFull = false;
    latencyToServer = 0;

    xSemaphoreGive(latencyBufSemaphoreHandle);
  } else {
    ESP_LOGW(TAG, "reset_diff_buffer: can't take semaphore");

    return -1;
  }

  return 0;
}

/**
 *
 */
int32_t latency_buffer_full(bool *is_full, TickType_t wait) {
  if (!is_full) {
    return -3;
  }

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "latency_buffer_full: latencyBufSemaphoreHandle == NULL");

    return -2;
  }

  if (xSemaphoreTake(latencyBufSemaphoreHandle, wait) == pdFALSE) {
    ESP_LOGW(TAG, "latency_buffer_full: can't take semaphore");

    return -1;
  }

  *is_full = latencyBuffFull;

  xSemaphoreGive(latencyBufSemaphoreHandle);

  return 0;
}

/**
 *
 */
int32_t get_diff_to_server(int64_t *tDiff) {
  static int64_t lastDiff = 0;

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "get_diff_to_server: latencyBufSemaphoreHandle == NULL");

    return -2;
  }

  if (xSemaphoreTake(latencyBufSemaphoreHandle, 0) == pdFALSE) {
    *tDiff = lastDiff;

    ESP_LOGW(TAG,
             "get_diff_to_server: can't take semaphore. Old diff retrieved");

    return -1;
  }

  *tDiff = latencyToServer;
  lastDiff = latencyToServer;  // store value, so we can return a value if
                               // semaphore couldn't be taken

  xSemaphoreGive(latencyBufSemaphoreHandle);

  return 0;
}

/**
 *
 */
int32_t server_now(int64_t *sNow, int64_t *diff2Server) {
  int64_t diff, now;

  if (sNow == NULL) {
    return -2;
  }

  now = esp_timer_get_time();

  if (get_diff_to_server(&diff) == -1) {
    ESP_LOGW(TAG,
             "server_now: can't get current diff to server. Retrieved old one");
  }

  if (diff == 0) {
    // ESP_LOGW(TAG, "server_now: diff to server not initialized yet");

    return -1;
  }

  *sNow = now + diff;

  if (diff2Server) {
    *diff2Server = diff;
  }

  // ESP_LOGI(TAG, "now: %lldus", now);
  // ESP_LOGI(TAG, "diff: %lldus", diff);
  // ESP_LOGI(TAG, "serverNow: %lldus", *snow);

  return 0;
}

/*
 * Timer group0 ISR handler
 *
 * Note:
 * We don't call the timer API here because they are not declared with
 * IRAM_ATTR. If we're okay with the timer irq not being serviced while SPI
 * flash cache is disabled, we can allocate this interrupt without the
 * ESP_INTR_FLAG_IRAM flag and use the normal API.
 */
void IRAM_ATTR timer_group0_isr(void *para) {
  timer_spinlock_take(TIMER_GROUP_1);

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  // Retrieve the interrupt status and the counter value
  //   from the timer that reported the interrupt
  uint32_t timer_intr = timer_group_get_intr_status_in_isr(TIMER_GROUP_1);

  // Clear the interrupt
  //   and update the alarm time for the timer with without reload
  if (timer_intr & TIMER_INTR_T1) {
    timer_group_clr_intr_status_in_isr(TIMER_GROUP_1, TIMER_1);

    uint64_t timer_counter_value =
        timer_group_get_counter_value_in_isr(TIMER_GROUP_1, TIMER_1);

    // Notify the task in the task's notification value.
    xTaskNotifyFromISR(playerTaskHandle, (uint32_t)timer_counter_value,
                       eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
  }

  timer_spinlock_give(TIMER_GROUP_1);

  if (xHigherPriorityTaskWoken) {
    portYIELD_FROM_ISR();
  }
}

static void tg0_timer_deinit(void) { timer_deinit(TIMER_GROUP_1, TIMER_1); }

/*
 *
 */
static void tg0_timer_init(void) {
  // Select and initialize basic parameters of the timer
  timer_config_t config = {
      //.divider = 8,		// 100ns ticks
      .divider = 80,  // 80,  // 1µs ticks
      .counter_dir = TIMER_COUNT_UP,
      .counter_en = TIMER_PAUSE,
      .alarm_en = TIMER_ALARM_EN,
      .auto_reload = TIMER_AUTORELOAD_DIS,
  };  // default clock source is APB
  timer_init(TIMER_GROUP_1, TIMER_1, &config);

  // Configure the alarm value and the interrupt on alarm.
  // timer_set_alarm_value(TIMER_GROUP_1, TIMER_1, 0);
  timer_enable_intr(TIMER_GROUP_1, TIMER_1);
  if (timer_isr_register(TIMER_GROUP_1, TIMER_1, timer_group0_isr, NULL,
                         ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
                         NULL) != ESP_OK) {
    ESP_LOGE(TAG, "unable to register timer 1 callback");
  }
}

/**
 *
 */
static void tg0_timer1_start(uint64_t alarm_value) {
  timer_pause(TIMER_GROUP_1, TIMER_1);
  timer_set_alarm_value(TIMER_GROUP_1, TIMER_1, alarm_value);
  timer_set_counter_value(TIMER_GROUP_1, TIMER_1, 0);
  timer_set_alarm(TIMER_GROUP_1, TIMER_1, TIMER_ALARM_EN);
  timer_start(TIMER_GROUP_1, TIMER_1);

  // ESP_LOGI(TAG, "started age timer");
}

// void rtc_clk_apll_enable(bool enable, uint32_t sdm0, uint32_t sdm1, uint32_t
// sdm2, uint32_t o_div); apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 +
// sdm0/65536)/((o_div + 2) * 2) xtal == 40MHz on lyrat v4.3 I2S bit_clock =
// rate * (number of channels) * bits_per_sample
void adjust_apll(int8_t direction) {
  int sdm0, sdm1, sdm2, o_div;

  // only change if necessary
  if (currentDir == direction) {
    return;
  }

  if (direction == 1) {
    // speed up
    sdm0 = apll_corr_predefine[0][2];
    sdm1 = apll_corr_predefine[0][3];
    sdm2 = apll_corr_predefine[0][4];
    o_div = apll_corr_predefine[0][5];
  } else if (direction == -1) {
    // slow down
    sdm0 = apll_corr_predefine[1][2];
    sdm1 = apll_corr_predefine[1][3];
    sdm2 = apll_corr_predefine[1][4];
    o_div = apll_corr_predefine[1][5];
  } else {
    // reset to normal playback speed
    sdm0 = apll_normal_predefine[2];
    sdm1 = apll_normal_predefine[3];
    sdm2 = apll_normal_predefine[4];
    o_div = apll_normal_predefine[5];

    direction = 0;
  }

  rtc_clk_apll_enable(1, sdm0, sdm1, sdm2, o_div);

  currentDir = direction;
}

/**
 *
 */
int8_t free_pcm_chunk_fragments(pcm_chunk_fragment_t *fragment) {
  if (fragment == NULL) {
    ESP_LOGE(TAG, "free_pcm_chunk_fragments() parameter Error");

    return -1;
  }

  // free all fragments recursive
  if (fragment->nextFragment == NULL) {
    if (fragment->payload != NULL) {
      free(fragment->payload);
      fragment->payload = NULL;
    }

    free(fragment);
    fragment = NULL;
  } else {
    free_pcm_chunk_fragments(fragment->nextFragment);
  }

  return 0;
}

/**
 *
 */
int8_t free_pcm_chunk(pcm_chunk_message_t *pcmChunk) {
  if (pcmChunk == NULL) {
    ESP_LOGE(TAG, "free_pcm_chunk() parameter Error");

    return -1;
  }

  free_pcm_chunk_fragments(pcmChunk->fragment);
  pcmChunk->fragment = NULL;  // was freed in free_pcm_chunk_fragments()

  free(pcmChunk);
  pcmChunk = NULL;

  return 0;
}

/**
 *
 */
int32_t allocate_pcm_chunk_memory_caps(pcm_chunk_message_t *pcmChunk,
                                       size_t bytes, uint32_t caps) {
  size_t largestFreeBlock, freeMem;
  int ret = -3;

  // we got valid memory for pcm_chunk_message_t
  // first we try to allocated 32 bit aligned memory for payload
  // check available memory first so we can decide if we need to fragment the
  // data
  freeMem = heap_caps_get_free_size(caps);
  largestFreeBlock = heap_caps_get_largest_free_block(caps);
  if ((freeMem >= bytes) && (largestFreeBlock >= bytes)) {
    // ESP_LOGI(TAG, "32b f %d b %d", freeMem, largestFreeBlock);

    pcmChunk->fragment->payload = (char *)heap_caps_malloc(bytes, caps);
    if (pcmChunk->fragment->payload == NULL) {
      ESP_LOGE(TAG, "Failed to allocate IRAM memory for pcm chunk payload");

      ret = -2;
    } else {
      pcmChunk->totalSize = bytes;
      pcmChunk->fragment->nextFragment = NULL;
      pcmChunk->fragment->size = bytes;

      ret = 0;
    }
  } else {
    // ESP_LOGE (TAG, "couldn't get memory to insert chunk of size %d, IRAM
    // freemem: %d blocksize %d", bytes, freeMem, largestFreeBlock);
  }

  return ret;
}

/**
 *
 */
int32_t allocate_pcm_chunk_memory_caps_fragmented(pcm_chunk_message_t *pcmChunk,
                                                  size_t bytes, uint32_t caps) {
  size_t largestFreeBlock, freeMem;
  int ret = -3;

  // we got valid memory for pcm_chunk_message_t
  // first we try to allocated 32 bit aligned memory for payload
  // check available memory first so we can decide if we need to fragment the
  // data
  freeMem = heap_caps_get_free_size(caps);
  largestFreeBlock = heap_caps_get_largest_free_block(caps);
  if (freeMem >= bytes) {
    // ESP_LOGI(TAG, "32b f %d b %d", freeMem, largestFreeBlock);

    if (largestFreeBlock >= bytes) {
      pcmChunk->fragment->payload = (char *)heap_caps_malloc(bytes, caps);
      if (pcmChunk->fragment->payload == NULL) {
        ESP_LOGE(TAG, "Failed to allocate IRAM memory for pcm chunk payload");

        ret = -2;
      } else {
        pcmChunk->totalSize = bytes;
        pcmChunk->fragment->nextFragment = NULL;
        pcmChunk->fragment->size = bytes;

        ret = 0;
      }
    } else {
      size_t remainingBytes = bytes + (largestFreeBlock % 4);
      size_t needBytes = largestFreeBlock - (largestFreeBlock % 4);
      pcm_chunk_fragment_t *fragment = pcmChunk->fragment;

      pcmChunk->totalSize = 0;

      while (remainingBytes) {
        fragment->payload = (char *)heap_caps_malloc(needBytes, caps);
        if (fragment->payload == NULL) {
          ESP_LOGE(TAG,
                   "Failed to allocate fragmented IRAM memory for "
                   "pcm chunk payload %d %d %d %d",
                   needBytes, remainingBytes, heap_caps_get_free_size(caps),
                   heap_caps_get_largest_free_block(caps));

          ret = -2;

          break;
        } else {
          fragment->size = needBytes;
          remainingBytes -= needBytes;
          pcmChunk->totalSize += needBytes;

          if (remainingBytes > 0) {
            fragment->nextFragment =
                (pcm_chunk_fragment_t *)calloc(1, sizeof(pcm_chunk_fragment_t));
            if (fragment->nextFragment == NULL) {
              ESP_LOGE(TAG,
                       "Failed to fragmented IRAM memory "
                       "for pcm chunk fragment");

              ret = -2;

              break;
            } else {
              fragment = fragment->nextFragment;
              largestFreeBlock = heap_caps_get_largest_free_block(caps);
              if (largestFreeBlock >= remainingBytes) {
                needBytes = remainingBytes;
              } else {
                needBytes = largestFreeBlock - (largestFreeBlock % 4);
              }
            }
          } else {
            ret = 0;
          }
        }
      }
    }
  } else {
    // ESP_LOGE (TAG, "couldn't get memory to insert chunk of size %d, IRAM
    // freemem: %d blocksize %d", decodedWireChunk->size, freeMem,
    // largestFreeBlock);
  }

  return ret;
}

/**
 *
 */
int32_t allocate_pcm_chunk_memory(pcm_chunk_message_t **pcmChunk,
                                  size_t bytes) {
  int ret = -3;

  *pcmChunk = (pcm_chunk_message_t *)calloc(1, sizeof(pcm_chunk_message_t));
  if (*pcmChunk == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for pcm chunk message");

    return -2;
  }

  (*pcmChunk)->fragment =
      (pcm_chunk_fragment_t *)calloc(1, sizeof(pcm_chunk_fragment_t));
  if ((*pcmChunk)->fragment == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for pcm chunk fragment");

    free_pcm_chunk(*pcmChunk);

    return -2;
  }

#if CONFIG_USE_PSRAM
  (*pcmChunk)->fragment->payload =
      (char *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
  if ((*pcmChunk)->fragment->payload == NULL) {
    //  size_t largestFreeBlock, freeMem;
    //	ESP_LOGE (TAG, "Failed to allocate memory for pcm chunk fragment
    // payload"); 	free_pcm_chunk (pcmChunk);
    //  freeMem = heap_caps_get_free_size (MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

    ret = -2;
  } else {
    (*pcmChunk)->fragment->nextFragment = NULL;
    (*pcmChunk)->fragment->size = bytes;

    ret = 0;
  }
#else
  ret = allocate_pcm_chunk_memory_caps(*pcmChunk, bytes,
                                       MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
  if (ret < 0) {
    ret = allocate_pcm_chunk_memory_caps(*pcmChunk, bytes, MALLOC_CAP_8BIT);
    if (ret < 0) {
      //		  ret = allocate_pcm_chunk_memory_caps_fragmented
      //(*pcmChunk, bytes, MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
      if (ret < 0) {
        // allocate_pcm_chunk_memory_caps_fragmented (*pcmChunk, bytes,
        // MALLOC_CAP_8BIT);
      }
    }
  }
#endif

  if (ret < 0) {
    ESP_LOGE(TAG, "couldn't get memory to insert chunk");

    ESP_LOGI(
        TAG, "%d, %d, %d, %d, %d", heap_caps_get_free_size(MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        uxQueueMessagesWaiting(pcmChkQHdl),
        heap_caps_get_free_size(MALLOC_CAP_32BIT | MALLOC_CAP_EXEC),
        heap_caps_get_largest_free_block(MALLOC_CAP_32BIT | MALLOC_CAP_EXEC));

    // simulate a chunk of all samples 0,
    // player_task() will know what to do with this
    (*pcmChunk)->fragment->payload = NULL;
    (*pcmChunk)->totalSize = bytes;
    (*pcmChunk)->fragment->nextFragment = NULL;
    (*pcmChunk)->fragment->size = bytes;

    ret = 0;
  } else {
    // ESP_LOGI (TAG, "got memory for pcm chunk %p %p %d", *pcmChunk,
    // (*pcmChunk)->fragment->payload, bytes);
  }

  return ret;
}

/**
 *
 */
int32_t insert_pcm_chunk(pcm_chunk_message_t *pcmChunk) {
  if (pcmChunk == NULL) {
    ESP_LOGE(TAG, "Parameter Error");

    return -1;
  }

  if (pcmChkQHdl == NULL) {
    ESP_LOGW(TAG, "pcm chunk queue not created");

    return -2;
  }

  if (uxQueueSpacesAvailable(pcmChkQHdl) == 0) {
    pcm_chunk_message_t *element;

    xQueueReceive(pcmChkQHdl, &element, portMAX_DELAY);

    free_pcm_chunk(element);
  }

  if (xQueueSend(pcmChkQHdl, &pcmChunk, pdMS_TO_TICKS(10)) != pdTRUE) {
    ESP_LOGW(TAG, "send: pcmChunkQueue full, messages waiting %d",
             uxQueueMessagesWaiting(pcmChkQHdl));

    free_pcm_chunk(pcmChunk);
  }

  return 0;
}

/**
 *
 */
static void player_task(void *pvParameters) {
  pcm_chunk_message_t *chnk = NULL;
  int64_t serverNow = 0;
  float age;
  BaseType_t ret;
  float chkDur_us;
  char *p_payload = NULL;
  size_t size = 0;
  uint32_t notifiedValue;
  snapcastSetting_t scSet;
  uint8_t scSetChgd = 0;
  uint64_t timer_val;
  int initialSync = 0;
  int64_t avg = 0;
  int dir = 0;
  int64_t buf_us = 0;
  pcm_chunk_fragment_t *fragment = NULL;
  size_t written;
  bool gotSnapserverConfig = false;
  int64_t clientDacLatency_us;
  int64_t diff2Server;
  float outputBufferDacTime = 0;

  memset(&scSet, 0, sizeof(snapcastSetting_t));

  ESP_LOGI(TAG, "started sync task");

  //  stats_init();

  // create message queue to inform task of changed settings
  snapcastSettingQueueHandle = xQueueCreate(1, sizeof(uint8_t));

  initialSync = 0;

  while (1) {
    // ESP_LOGW( TAG, "32b f %d b %d", heap_caps_get_free_size
    //(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block (MALLOC_CAP_8BIT));
    // ESP_LOGW (TAG, "stack free: %d", uxTaskGetStackHighWaterMark(NULL));

    // check if we got changed setting available, if so we need to
    // reinitialize
    ret = xQueueReceive(snapcastSettingQueueHandle, &scSetChgd, 0);
    if (ret == pdTRUE) {
      snapcastSetting_t __scSet;

      player_get_snapcast_settings(&__scSet);

      buf_us = (int64_t)(__scSet.buf_ms) * 1000LL;

      chkDur_us = __scSet.chkDur_ms * 1000LL;
      chkInBytes =
          (__scSet.chkDur_ms * __scSet.sr * __scSet.ch * (__scSet.bits / 8)) /
          1000;
      clientDacLatency_us = (int64_t)__scSet.cDacLat_ms * 1000;

      // this value is highly coupled with I2S DMA buffer
      // size. DMA buffer has a size of 1 chunk (e.g. 20ms)
      // so next chunk we get from queue will be -20ms
      outputBufferDacTime = chkDur_us * CHNK_CTRL_CNT;

      if ((scSet.sr != __scSet.sr) || (scSet.bits != __scSet.bits) ||
          (scSet.ch != __scSet.ch)) {
        i2s_custom_stop(I2S_NUM_0);

        ret = player_setup_i2s(I2S_NUM_0, &currentSnapcastSetting);
        if (ret < 0) {
          ESP_LOGE(TAG, "player_setup_i2s failed: %d", ret);

          return;
        }

        // force adjust_apll() to set playback speed
        currentDir = 1;
        adjust_apll(0);

        i2s_custom_set_clk(I2S_NUM_0, __scSet.sr, __scSet.bits, __scSet.ch);
        initialSync = 0;
      }

      if ((__scSet.buf_ms > 0) && (__scSet.chkDur_ms > 0)) {
        if ((__scSet.buf_ms != scSet.buf_ms) ||
            (__scSet.chkDur_ms != scSet.chkDur_ms)) {
          if (pcmChkQHdl != NULL) {
            destroy_pcm_queue(&pcmChkQHdl);
          }
        }

        if (pcmChkQHdl == NULL) {
          int entries = ceil((float)__scSet.buf_ms / __scSet.chkDur_ms);

          pcmChkQHdl = xQueueCreate(entries, sizeof(pcm_chunk_message_t *));

          ESP_LOGI(TAG, "created new queue with %d", entries);
        }
      }

      ESP_LOGI(TAG,
               "snapserver config changed, buffer %dms, chunk %fms, "
               "sample rate %d, ch %d, bits %d mute %d latency %d",
               __scSet.buf_ms, __scSet.chkDur_ms, __scSet.sr, __scSet.ch,
               __scSet.bits, __scSet.muted, __scSet.cDacLat_ms);

      scSet = __scSet;  // store for next round

      gotSnapserverConfig = true;
    } else if (gotSnapserverConfig == false) {
      //      ESP_LOGW(TAG, "no snapserver config yet, keep waiting");

      vTaskDelay(pdMS_TO_TICKS(100));

      continue;
    }

    // wait for early time syncs to be ready
    bool is_full = false;
    int tmp = latency_buffer_full(&is_full, pdMS_TO_TICKS(1));
    if (tmp < 0) {
      continue;
    } else {
      if (is_full == false) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // ESP_LOGW(TAG, "diff buffer not full");

        continue;
      }
    }

    if (chnk == NULL) {
      if (pcmChkQHdl != NULL) {
        ret = xQueueReceive(pcmChkQHdl, &chnk, pdMS_TO_TICKS(2000));
      } else {
        // ESP_LOGE (TAG, "Couldn't get PCM chunk, pcm queue not created");

        vTaskDelay(pdMS_TO_TICKS(100));

        continue;
      }

      if (ret != pdFAIL) {
        // ESP_LOGW(TAG, "got pcm chunk");
      }
    } else {
      // ESP_LOGW(TAG, "already retrieved chunk needs service");
      ret = pdPASS;
    }

    if (ret != pdFAIL) {
      if (server_now(&serverNow, &diff2Server) >= 0) {
        int64_t chunkStart = (int64_t)chnk->timestamp.sec * 1000000LL +
                             (int64_t)chnk->timestamp.usec;

        age = (float)(serverNow - chunkStart - buf_us + clientDacLatency_us);
        if (initialSync == 1) {
          // on initialSync == 0 (hard sync) we don't have any data in i2s DMA
          // buffer so in that case we don't need to add this
          age += outputBufferDacTime;
        }
      } else {
        // ESP_LOGW(TAG, "couldn't get server now");

        if (chnk != NULL) {
          free_pcm_chunk(chnk);
          chnk = NULL;
        }

        vTaskDelay(pdMS_TO_TICKS(1));

        continue;
      }

      if (age < 0) {  // get initial sync using hardware timer
        if (initialSync == 0) {
          MEDIANFILTER_Init(&shortMedianFilter);
          MEDIANFILTER_Init(&miniMedianFilter);

          timer_set_auto_reload(TIMER_GROUP_1, TIMER_1, TIMER_AUTORELOAD_DIS);
          tg0_timer1_start((uint64_t)(-age+0.5f));  // timer with 1µs ticks

          i2s_custom_stop(I2S_NUM_0);
          i2s_custom_zero_dma_buffer(I2S_NUM_0);

          adjust_apll(0);  // reset to normal playback speed

          uint32_t currentDescriptor = 0, currentDescriptorOffset = 0;
          uint32_t tmpCnt = CHNK_CTRL_CNT;
          while (tmpCnt) {
            if (chnk == NULL) {
              if (pcmChkQHdl != NULL) {
                ret = xQueueReceive(pcmChkQHdl, &chnk, portMAX_DELAY);
              }
            }

            fragment = chnk->fragment;
            p_payload = fragment->payload;
            size = fragment->size;

            i2s_custom_init_dma_tx_queues(I2S_NUM_0, (uint8_t *)p_payload, size,
                                          &written, &currentDescriptor,
                                          &currentDescriptorOffset);
            size -= written;
            p_payload += written;

            if (size == 0) {
              if (fragment->nextFragment != NULL) {
                fragment = fragment->nextFragment;
                p_payload = fragment->payload;
                size = fragment->size;
              } else {
                free_pcm_chunk(chnk);
                chnk = NULL;
              }
            }

            tmpCnt--;
          }

          //          xTaskNotifyStateClear(playerTaskHandle);

          // Wait to be notified of a timer interrupt.
          xTaskNotifyWait(pdFALSE,         // Don't clear bits on entry.
                          pdFALSE,         // Don't clear bits on exit.
                          &notifiedValue,  // Stores the notified value.
                          portMAX_DELAY);
          // or use simple task delay for this
          //           vTaskDelay( pdMS_TO_TICKS(-age / 1000) );

          timer_pause(TIMER_GROUP_1, TIMER_1);

          i2s_custom_start(I2S_NUM_0);

          // get timer value so we can get the real age
          timer_val = (int64_t)notifiedValue;

          // get actual age after alarm
          age = timer_val - (-age);

          // check if we need to write remaining data
          if (size != 0) {
            do {
              written = 0;
              if (i2s_custom_write(I2S_NUM_0, p_payload, (size_t)size, &written,
                                   portMAX_DELAY) != ESP_OK) {
                ESP_LOGE(TAG, "i2s_playback_task: I2S write error");
              }
              if (written < size) {
                ESP_LOGE(TAG,
                         "i2s_playback_task: I2S didn't "
                         "write all data");
              }
              size -= written;
              p_payload += written;

              if (size == 0) {
                if (fragment->nextFragment != NULL) {
                  fragment = fragment->nextFragment;
                  p_payload = fragment->payload;
                  size = fragment->size;
                } else {
                  free_pcm_chunk(chnk);
                  chnk = NULL;

                  break;
                }
              }
            } while (1);
          }

          initialSync = 1;

          ESP_LOGI(TAG, "initial sync age: %fus, chunk duration: %fus", age,
                   chkDur_us);

          continue;
        }
      } else if ((age > 0) && (initialSync == 0)) {
        if (chnk != NULL) {
          free_pcm_chunk(chnk);
          chnk = NULL;
        }

        // get count of chunks we are late for
        uint32_t c = ceil(age / chkDur_us);  // round up
        // now clear all those chunks which are probably late too
        while (c--) {
          ret = xQueueReceive(pcmChkQHdl, &chnk, pdMS_TO_TICKS(1));
          if (ret == pdPASS) {
            free_pcm_chunk(chnk);
            chnk = NULL;
          } else {
            break;
          }
        }

        wifi_ap_record_t ap;
        esp_wifi_sta_get_ap_info(&ap);

        timer_pause(TIMER_GROUP_1, TIMER_1);
        timer_set_auto_reload(TIMER_GROUP_1, TIMER_1, TIMER_AUTORELOAD_DIS);

        ESP_LOGW(TAG,
                 "RESYNCING HARD 1: age %fus, latency %lldus, free %d, "
                 "largest block %d, %d, rssi: %d",
                 age, diff2Server, heap_caps_get_free_size(MALLOC_CAP_32BIT),
                 heap_caps_get_largest_free_block(MALLOC_CAP_32BIT),
                 uxQueueMessagesWaiting(pcmChkQHdl), ap.rssi);

        dir = 0;

        initialSync = 0;

        i2s_custom_stop(I2S_NUM_0);

        continue;
      }

      const bool enableControlLoop = true;

      const int64_t shortOffset = 100;             //µs, softsync
      const int64_t miniOffset = shortOffset / 2;  // 50;    //µs, softsync
      const int64_t hardResyncThreshold = 10000;   //µs, hard sync

      if (initialSync == 1) {
        avg = (int64_t)age;

        int64_t shortMedian, miniMedian;

        shortMedian = MEDIANFILTER_Insert(&shortMedianFilter, avg);
        miniMedian = MEDIANFILTER_Insert(&miniMedianFilter, avg);

        // resync if we are getting late.
        // hopefully being early will get ok
        // through apll speed control
        if ((uxQueueMessagesWaiting(pcmChkQHdl) == 0) ||
            (abs(avg) > hardResyncThreshold)) {
          if (chnk != NULL) {
            free_pcm_chunk(chnk);
            chnk = NULL;
          }

          // get count of chunks we are late for
          uint32_t c = ceil(age / chkDur_us);  // round up
          // now clear all those chunks which are probably late too
          while (c--) {
            ret = xQueueReceive(pcmChkQHdl, &chnk, pdMS_TO_TICKS(1));
            if (ret == pdPASS) {
              free_pcm_chunk(chnk);
              chnk = NULL;
            } else {
              break;
            }
          }

          wifi_ap_record_t ap;
          esp_wifi_sta_get_ap_info(&ap);

          timer_pause(TIMER_GROUP_1, TIMER_1);
          timer_set_auto_reload(TIMER_GROUP_1, TIMER_1, TIMER_AUTORELOAD_DIS);

          i2s_custom_stop(I2S_NUM_0);

          ESP_LOGW(TAG,
                   "RESYNCING HARD 2: age %lldus, latency %lldus, free "
                   "%d, largest block %d, %d, rssi: %d",
                   avg, diff2Server, heap_caps_get_free_size(MALLOC_CAP_32BIT),
                   heap_caps_get_largest_free_block(MALLOC_CAP_32BIT),
                   uxQueueMessagesWaiting(pcmChkQHdl), ap.rssi);

          initialSync = 0;

          continue;
        }

        if ((enableControlLoop == true) &&
            (MEDIANFILTER_isFull(&shortMedianFilter))) {
          if ((shortMedian < -shortOffset) && (miniMedian < -miniOffset) &&
              (avg < -miniOffset)) {  // we are early
            dir = -1;
          } else if ((shortMedian > shortOffset) && (miniMedian > miniOffset) &&
                     (avg > miniOffset)) {  // we are late
            dir = 1;
          }

          adjust_apll(dir);
        }

        const uint32_t tmpCntInit = 1;  // 250  // every 6s
        static uint32_t tmpcnt = 1;
        if (tmpcnt-- == 0) {
          int64_t sec, msec, usec;

          tmpcnt = tmpCntInit;

          sec = diff2Server / 1000000;
          usec = diff2Server - sec * 1000000;
          msec = usec / 1000;
          usec = usec % 1000;
          // ESP_LOGI (TAG, "%d, %lldus, %lldus %llds, %lld.%lldms", dir, age,
          // avg, sec, msec, usec); ESP_LOGI (TAG, "%d, %lldus, %lldus, %lldus",
          // dir, avg, shortMedian, miniMedian);
        }

        dir = 0;

        fragment = chnk->fragment;
        p_payload = fragment->payload;
        size = fragment->size;

        if (p_payload != NULL) {
          do {
            written = 0;
            if (i2s_custom_write(I2S_NUM_0, p_payload, (size_t)size, &written,
                                 portMAX_DELAY) != ESP_OK) {
              ESP_LOGE(TAG, "i2s_playback_task: I2S write error %d", size);
            }
            if (written < size) {
              ESP_LOGE(TAG, "i2s_playback_task: I2S didn't write all data");
            }
            size -= written;
            p_payload += written;

            if (size == 0) {
              if (fragment->nextFragment != NULL) {
                fragment = fragment->nextFragment;
                p_payload = fragment->payload;
                size = fragment->size;

                // ESP_LOGI (TAG, "%s: fragmented", __func__);
              } else {
                free_pcm_chunk(chnk);
                chnk = NULL;

                break;
              }
            }
          } while (1);
        } else {
          // here we have an empty fragment because of memory allocation error.
          // fill DMA with zeros so we don't get out of sync
          written = 0;
          const size_t write_size = 4;
          uint8_t tmpBuf[write_size];

          memset(tmpBuf, 0, sizeof(tmpBuf));

          do {
            if (i2s_custom_write(I2S_NUM_0, tmpBuf, (size_t)write_size,
                                 &written, portMAX_DELAY) != ESP_OK) {
              ESP_LOGE(TAG, "i2s_playback_task: I2S write error %d", size);
            }

            size -= written;
          } while (size);

          free_pcm_chunk(chnk);
          chnk = NULL;
        }
      }
    } else {
      int64_t sec, msec, usec;

      sec = diff2Server / 1000000;
      usec = diff2Server - sec * 1000000;
      msec = usec / 1000;
      usec = usec % 1000;

      if (pcmChkQHdl != NULL) {
        ESP_LOGE(TAG,
                 "Couldn't get PCM chunk, recv: messages waiting %d, "
                 "diff2Server: %llds, %lld.%lldms",
                 uxQueueMessagesWaiting(pcmChkQHdl), sec, msec, usec);
      }

      dir = 0;

      initialSync = 0;

      i2s_custom_stop(I2S_NUM_0);
    }
  }
}

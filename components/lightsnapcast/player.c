/**
 *
 */

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// #include "lwip/stats.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "soc/rtc.h"

#if SOC_I2S_SUPPORTS_APLL
#include "clk_ctrl_os.h"
#endif

#include <math.h>

#include "MedianFilter.h"
#include "driver/gptimer.h"
#include "driver/i2s_std.h"
#include "player.h"
#include "snapcast.h"

#define USE_SAMPLE_INSERTION CONFIG_USE_SAMPLE_INSERTION

#define SYNC_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define SYNC_TASK_CORE_ID 1  // tskNO_AFFINITY

static const char *TAG = "PLAYER";

#if USE_SAMPLE_INSERTION

#define INSERT_SAMPLES \
  1  //!< currently only allowed to be 1 or sync algorithm will break

const uint32_t SHORT_OFFSET = 128;
const uint32_t MINI_OFFSET = 64;

#else
const uint32_t SHORT_OFFSET = 2;
const uint32_t MINI_OFFSET = 1;
#endif

/**
 * @brief Pre define APLL parameters, save compute time. They are calculated in
 * player_setup_i2s() | bits_per_sample | rate | sdm0 | sdm1 | sdm2 | odir
 *
 * apll_freq = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536)/((o_div + 2) * 2)
 * I2S bit clock is (apll_freq / 16)
 */
static uint32_t apll_normal_predefine[6] = {0, 0, 0, 0, 0, 0};
static uint32_t apll_corr_predefine[][6] = {{0, 0, 0, 0, 0, 0},
                                            {0, 0, 0, 0, 0, 0}};

static SemaphoreHandle_t latencyBufSemaphoreHandle = NULL;

static bool latencyBuffFull = 0;

static gptimer_handle_t gptimer = NULL;

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

static uint32_t i2sDmaBufCnt;
static uint32_t i2sDmaBufMaxLen;

static SemaphoreHandle_t snapcastSettingsMux = NULL;
static snapcastSetting_t currentSnapcastSetting;

static void tg0_timer_init(void);
static void tg0_timer_deinit(void);

static bool gpTimerRunning = false;

static void player_task(void *pvParameters);

extern void audio_set_mute(bool mute);

static i2s_chan_handle_t tx_chan = NULL;  // I2S tx channel handler
static bool i2sEnabled = false;

i2s_std_gpio_config_t pin_config0;
i2s_port_t i2sNum;

/**
 *
 */
esp_err_t my_i2s_channel_disable(i2s_chan_handle_t handle) {
  if (tx_chan != NULL) {
    if (i2sEnabled == true) {
      i2sEnabled = false;

      return i2s_channel_disable(handle);
    }
  }

  return ESP_OK;
}

/**
 *
 */
esp_err_t my_i2s_channel_enable(i2s_chan_handle_t handle) {
  if (tx_chan != NULL) {
    if (i2sEnabled == false) {
      i2sEnabled = true;

      return i2s_channel_enable(handle);
    }
  }

  return ESP_OK;
}

/**
 *
 */
static esp_err_t player_setup_i2s(snapcastSetting_t *setting) {
  // ensure save setting
  int32_t sr = setting->sr;
  if (sr == 0) {
    sr = 44100;
  }

  // ensure save setting
  int bits = setting->bits;
  if (bits == 0) {
    bits = I2S_DATA_BIT_WIDTH_16BIT;
  }

  // ensure save setting
  uint32_t chkInFrames = setting->chkInFrames;
  if (chkInFrames == 0) {
    chkInFrames = 1152;
  }

#if USE_SAMPLE_INSERTION
  i2sDmaBufCnt = 22;
  // OPUS has a minimum frame size of 120
  // with DMA buffer set to this value sync algorithm
  // works for all decoders. We set it to 100 so
  // there will be free space for sample stuffing in each round
  i2sDmaBufMaxLen = 100;
#else
  int fi2s_clk;
  const int __dmaBufMaxLen = 1024;
  int __dmaBufCnt;
  int __dmaBufLen;

  __dmaBufCnt = 1;
  __dmaBufLen = chkInFrames;
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

  // check i2s_set_get_apll_freq() how it is done
  fi2s_clk = 2 * sr *
             I2S_MCLK_MULTIPLE_256;  // setting->ch * setting->bits * m_scale;

  apll_normal_predefine[0] = bits;
  apll_normal_predefine[1] = sr;
  if (rtc_clk_apll_coeff_calc(
          fi2s_clk, &apll_normal_predefine[5], &apll_normal_predefine[2],
          &apll_normal_predefine[3], &apll_normal_predefine[4]) == 0) {
    ESP_LOGE(TAG, "ERROR, fi2s_clk");
  }

#define UPPER_SR_SCALER 1.0001
#define LOWER_SR_SCALER 0.9999

  apll_corr_predefine[0][0] = bits;
  apll_corr_predefine[0][1] = sr * UPPER_SR_SCALER;
  if (rtc_clk_apll_coeff_calc(
          fi2s_clk * UPPER_SR_SCALER, &apll_corr_predefine[0][5],
          &apll_corr_predefine[0][2], &apll_corr_predefine[0][3],
          &apll_corr_predefine[0][4]) == 0) {
    ESP_LOGE(TAG, "ERROR, fi2s_clk * %f", UPPER_SR_SCALER);
  }
  apll_corr_predefine[1][0] = bits;
  apll_corr_predefine[1][1] = sr * LOWER_SR_SCALER;
  if (rtc_clk_apll_coeff_calc(
          fi2s_clk * LOWER_SR_SCALER, &apll_corr_predefine[1][5],
          &apll_corr_predefine[1][2], &apll_corr_predefine[1][3],
          &apll_corr_predefine[1][4]) == 0) {
    ESP_LOGE(TAG, "ERROR, fi2s_clk * %f", LOWER_SR_SCALER);
  }
#endif

  if (tx_chan) {
    my_i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }

  i2s_chan_config_t tx_chan_cfg = {
      .id = i2sNum,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = i2sDmaBufCnt,
      .dma_frame_num = i2sDmaBufMaxLen,
      .auto_clear = false,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));


  ESP_LOGI(TAG,
           "player_setup_i2s: dma_buf_len is %ld, dma_buf_count is %ld, sample "
           "rate: %ld, bits: %d",
           i2sDmaBufMaxLen, i2sDmaBufCnt, sr, bits);

  i2s_std_clk_config_t i2s_clkcfg = {
      .sample_rate_hz = sr,
#if USE_SAMPLE_INSERTION
      .clk_src = I2S_CLK_SRC_DEFAULT,
#else
      .clk_src = I2S_CLK_SRC_APLL,
#endif
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
  };
  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = i2s_clkcfg,
#if CONFIG_I2S_USE_MSB_FORMAT
      .slot_cfg =
          I2S_STD_MSB_SLOT_DEFAULT_CONFIG(setting->bits, I2S_SLOT_MODE_STEREO),
#else
      .slot_cfg =
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_STEREO),
#endif
      .gpio_cfg = pin_config0,
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));

  // my_i2s_channel_enable(tx_chan);

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
    playerTaskHandle = NULL;
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
int init_player(i2s_std_gpio_config_t pin_config0_, i2s_port_t i2sNum_) {
  int ret = 0;

  deinit_player();

  pin_config0 = pin_config0_;
  i2sNum = i2sNum_;

  currentSnapcastSetting.buf_ms = 0;
  currentSnapcastSetting.chkInFrames = 0;
  currentSnapcastSetting.codec = NONE;
  currentSnapcastSetting.sr = 0;
  currentSnapcastSetting.ch = 0;
  currentSnapcastSetting.bits = 0;
  currentSnapcastSetting.muted = true;
  currentSnapcastSetting.volume = 0;

  if (snapcastSettingsMux == NULL) {
    snapcastSettingsMux = xSemaphoreCreateMutex();
    xSemaphoreGive(snapcastSettingsMux);
  }

  ret = player_setup_i2s(&currentSnapcastSetting);
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

/**
 *
 */
int32_t player_latency_insert(int64_t newValue) {
  int64_t medianValue;

  medianValue = MEDIANFILTER_Insert(&latencyMedianFilter, newValue);
  if (xSemaphoreTake(latencyBufSemaphoreHandle, pdMS_TO_TICKS(0)) == pdTRUE) {
    if (MEDIANFILTER_isFull(&latencyMedianFilter, LATENCY_MEDIAN_FILTER_FULL)) {
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
      (curSet.ch != setting->ch) ||
      (curSet.chkInFrames != setting->chkInFrames) ||
      (curSet.codec != setting->codec) || (curSet.muted != setting->muted) ||
      (curSet.sr != setting->sr) || (curSet.volume != setting->volume) ||
      (curSet.cDacLat_ms != setting->cDacLat_ms)) {
    ret = player_set_snapcast_settings(setting);
    if (ret != pdPASS) {
      ESP_LOGE(TAG,
               "player_send_snapcast_setting: couldn't change "
               "snapcast setting");
    }

    // check if it is only volume / mute related setting, which is handled by
    // http_get_task()
    if ((((curSet.muted != setting->muted) ||
          (curSet.volume != setting->volume)) &&
         ((curSet.bits == setting->bits) &&
          (curSet.buf_ms == setting->buf_ms) && (curSet.ch == setting->ch) &&
          (curSet.chkInFrames == setting->chkInFrames) &&
          (curSet.codec == setting->codec) && (curSet.sr == setting->sr) &&
          (curSet.cDacLat_ms == setting->cDacLat_ms))) == false) {
      // notify needed
      ret = xQueueOverwrite(snapcastSettingQueueHandle, &settingChanged);
      if (ret != pdPASS) {
        ESP_LOGE(TAG,
                 "player_send_snapcast_setting: couldn't notify "
                 "snapcast setting");
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
    // ESP_LOGW(TAG, "latency_buffer_full: can't take semaphore");

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

    // ESP_LOGW(TAG,
    //          "get_diff_to_server: can't take semaphore. Old diff retrieved");

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
    // ESP_LOGW(TAG,
    //          "server_now: can't get current diff to server. Retrieved old
    //          one");
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
static bool IRAM_ATTR timer_group0_alarm_cb(
    gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata,
    void *user_data) {
  // timer_spinlock_take(TIMER_GROUP_1);

  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  uint64_t timer_counter_value = edata->count_value;

  // Notify the task in the task's notification value.
  xTaskNotifyFromISR(playerTaskHandle, (uint32_t)timer_counter_value,
                     eSetValueWithOverwrite, &xHigherPriorityTaskWoken);

  return xHigherPriorityTaskWoken == pdTRUE;
}

/**
 *
 */
esp_err_t my_gptimer_stop(gptimer_handle_t timer) {
  if (gpTimerRunning == true) {
    gpTimerRunning = false;

    return gptimer_stop(timer);
  }

  return ESP_OK;
}

/**
 *
 */
esp_err_t my_gptimer_start(gptimer_handle_t timer) {
  if (gpTimerRunning == false) {
    gpTimerRunning = true;

    return gptimer_start(timer);
  }

  return ESP_OK;
}

static void tg0_timer_deinit(void) {
  //	timer_deinit(TIMER_GROUP_1, TIMER_1);
  if (gptimer) {
    ESP_ERROR_CHECK(my_gptimer_stop(gptimer));
    ESP_ERROR_CHECK(gptimer_disable(gptimer));
    ESP_ERROR_CHECK(gptimer_del_timer(gptimer));
    gptimer = NULL;
  }
}

/*
 *
 */
static void tg0_timer_init(void) {
  tg0_timer_deinit();

  // Select and initialize basic parameters of the timer
  gptimer_config_t timer_config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = 1000000,  // 1MHz, 1 tick=1us
  };
  ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

  gptimer_event_callbacks_t cbs = {
      .on_alarm = timer_group0_alarm_cb,
  };
  ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));

  ESP_LOGI(TAG, "enable initial sync timer");
  ESP_ERROR_CHECK(gptimer_enable(gptimer));
}

/**
 *
 */
static void tg0_timer1_start(uint64_t alarm_value) {
  //  timer_pause(TIMER_GROUP_1, TIMER_1);
  //  timer_set_alarm_value(TIMER_GROUP_1, TIMER_1, alarm_value);
  //  timer_set_counter_value(TIMER_GROUP_1, TIMER_1, 0);
  //  timer_set_alarm(TIMER_GROUP_1, TIMER_1, TIMER_ALARM_EN);
  //  timer_start(TIMER_GROUP_1, TIMER_1);

  if (gptimer) {
    my_gptimer_stop(gptimer);
    ESP_ERROR_CHECK(gptimer_set_raw_count(gptimer, 0));
    gptimer_alarm_config_t alarm_config1 = {
        .alarm_count = alarm_value,  // period
        .reload_count = 0,
        .flags.auto_reload_on_alarm = false,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config1));
    ESP_ERROR_CHECK(my_gptimer_start(gptimer));
  }

  // ESP_LOGI(TAG, "started age timer");
}

#if !USE_SAMPLE_INSERTION
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

  //  periph_rtc_apll_acquire();
  rtc_clk_apll_coeff_set(o_div, sdm0, sdm1, sdm2);
  // rtc_clk_apll_enable(1, sdm0, sdm1, sdm2, o_div);

  currentDir = direction;
}
#endif

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

  pcmChunk->caps = caps;

  // we got valid memory for pcm_chunk_message_t
  // first we try to allocated 32 bit aligned memory for payload
  // check available memory first so we can decide if we need to fragment the
  // data
  if (caps != 0) {
    freeMem = heap_caps_get_free_size(caps);
    largestFreeBlock = heap_caps_get_largest_free_block(caps);
    if ((freeMem >= bytes) && (largestFreeBlock >= bytes)) {
      // ESP_LOGI(TAG, "32b f %d b %d", freeMem, largestFreeBlock);

      pcmChunk->fragment->payload = (char *)heap_caps_malloc(bytes, caps);
      if (pcmChunk->fragment->payload == NULL) {
        ESP_LOGD(TAG, "Failed to allocate %d bytes of %s for pcm chunk payload",
                 bytes,
                 (caps == (MALLOC_CAP_32BIT | MALLOC_CAP_EXEC)) ? ("IRAM")
                                                                : ("DRAM"));

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
  } else {
    pcmChunk->fragment->payload = (char *)malloc(bytes);
    if (pcmChunk->fragment->payload == NULL) {
      ESP_LOGE(TAG, "Failed to malloc memory for pcm chunk payload");

      ret = -2;
    } else {
      pcmChunk->totalSize = bytes;
      pcmChunk->fragment->nextFragment = NULL;
      pcmChunk->fragment->size = bytes;

      ret = 0;
    }
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

#if CONFIG_SPIRAM && CONFIG_SPIRAM_BOOT_INIT
  ret = allocate_pcm_chunk_memory_caps(*pcmChunk, bytes,
                                       MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
#elif CONFIG_SPIRAM
  ret = allocate_pcm_chunk_memory_caps(*pcmChunk, bytes, 0);
#else
  // TODO: x should probably be dynamically calculated as a fraction of buffer
  // size if allocation fails we try again every 1ms for max. x ms waiting for
  // chunks to finish playback
  uint32_t x = 50;
  for (int i = 0; i < x; i++) {
    ret = allocate_pcm_chunk_memory_caps(*pcmChunk, bytes,
                                         MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
    if (ret < 0) {
      ret = allocate_pcm_chunk_memory_caps(*pcmChunk, bytes, MALLOC_CAP_8BIT);
      //      if (ret < 0) {
      //        //      ret = allocate_pcm_chunk_memory_caps_fragmented
      //        //(*pcmChunk, bytes, MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
      //        if (ret < 0) {
      //          // allocate_pcm_chunk_memory_caps_fragmented (*pcmChunk,
      //          bytes,
      //          // MALLOC_CAP_8BIT);
      //        }
      //      }
    }

    if (ret < 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      break;
    }
  }
#endif

  if (ret < 0) {
    ESP_LOGW(TAG,
             "couldn't get memory to insert chunk, inserting an chunk "
             "containing just 0");

    //    xSemaphoreTake(playerPcmQueueMux, portMAX_DELAY);
    //    ESP_LOGW(
    //        TAG, "%d, %d, %d, %d, %d",
    //        heap_caps_get_free_size(MALLOC_CAP_8BIT),
    //        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
    //        uxQueueMessagesWaiting(pcmChkQHdl),
    //        heap_caps_get_free_size(MALLOC_CAP_32BIT | MALLOC_CAP_EXEC),
    //        heap_caps_get_largest_free_block(MALLOC_CAP_32BIT |
    //        MALLOC_CAP_EXEC));
    //    xSemaphoreGive(playerPcmQueueMux);

    ESP_LOGW(
        TAG, "%d, %d, %d, %d", heap_caps_get_free_size(MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
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

  bool isFull = false;
  latency_buffer_full(&isFull, portMAX_DELAY);
  if (isFull == false) {
    free_pcm_chunk(pcmChunk);

    //    ESP_LOGW(TAG, "%s: wait for initial latency measurement to finish",
    //    __func__);

    return -3;
  }

  if (pcmChkQHdl == NULL) {
    ESP_LOGW(TAG, "pcm chunk queue not created");

    free_pcm_chunk(pcmChunk);

    return -2;
  }

  //  if (uxQueueSpacesAvailable(pcmChkQHdl) == 0) {
  //    pcm_chunk_message_t *element;
  //
  //    xQueueReceive(pcmChkQHdl, &element, portMAX_DELAY);
  //
  //    free_pcm_chunk(element);
  //  }

  // if (xQueueSend(pcmChkQHdl, &pcmChunk, pdMS_TO_TICKS(10)) != pdTRUE) {
  if (xQueueSend(pcmChkQHdl, &pcmChunk, pdMS_TO_TICKS(1)) != pdTRUE) {
    ESP_LOGW(TAG, "send: pcmChunkQueue full, messages waiting %d",
             uxQueueMessagesWaiting(pcmChkQHdl));

    free_pcm_chunk(pcmChunk);
  }

  return 0;
}

/**
 *
 */
int32_t pcm_chunk_queue_msg_waiting(void) {
  int ret = 0;

  if (pcmChkQHdl) {
    ret = uxQueueMessagesWaiting(pcmChkQHdl);
  }

  return ret;
}

/**
 *
 */
static void player_task(void *pvParameters) {
  pcm_chunk_message_t *chnk = NULL;
  int64_t serverNow = 0;
  int64_t age;
  BaseType_t ret;
  int64_t chunkDuration_us = 24000;
  char *p_payload = NULL;
  size_t size = 0;
  uint32_t notifiedValue;
  snapcastSetting_t scSet;
  uint8_t scSetChgd = 0;
  uint64_t timer_val;
  int initialSync = 0;
  int dir = 0;
  int32_t dir_insert_sample = 0;
  int64_t insertedSamplesCounter = 0;
  int64_t buf_us = 0;
  pcm_chunk_fragment_t *fragment = NULL;
  size_t written;
  bool gotSnapserverConfig = false;
  int64_t clientDacLatency_us = 0;
  int64_t diff2Server = 0;
  int64_t outputBufferDacTime_us = 0;
  int64_t dmaDescDuration_us = 0;
  size_t alreadyWritten = 0;

  memset(&scSet, 0, sizeof(snapcastSetting_t));

  ESP_LOGI(TAG, "started sync task");

  //  stats_init();

  // create message queue to inform task of changed settings
  snapcastSettingQueueHandle = xQueueCreate(1, sizeof(uint8_t));

  initialSync = 0;

  audio_set_mute(true);

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

      if ((__scSet.buf_ms > 0) && (__scSet.chkInFrames > 0) &&
          (__scSet.sr > 0)) {
        buf_us = (int64_t)(__scSet.buf_ms) * 1000LL;

        clientDacLatency_us = (int64_t)__scSet.cDacLat_ms * 1000LL;

        if ((scSet.sr != __scSet.sr) || (scSet.bits != __scSet.bits) ||
            (scSet.ch != __scSet.ch)) {
          my_i2s_channel_enable(tx_chan);
          audio_set_mute(true);
          my_i2s_channel_disable(tx_chan);

          ret = player_setup_i2s(&__scSet);
          if (ret < 0) {
            ESP_LOGE(TAG, "player_setup_i2s failed: %d", ret);

            return;
          }

          dmaDescDuration_us =
              1000000LL * (int64_t)i2sDmaBufMaxLen / (int64_t)__scSet.sr;

#if !USE_SAMPLE_INSERTION
          // force adjust_apll() to set playback speed
          currentDir = 1;
          adjust_apll(0);
#endif

          initialSync = 0;
        }

        static uint32_t queueCreatedWithChkInFrames = UINT32_MAX;

        if ((scSet.buf_ms != __scSet.buf_ms) ||
            (queueCreatedWithChkInFrames > __scSet.chkInFrames)) {
          destroy_pcm_queue(&pcmChkQHdl);
        }

        if (pcmChkQHdl == NULL) {
          int entries = ceil(((float)__scSet.sr / (float)__scSet.chkInFrames) *
                             ((float)__scSet.buf_ms / 1000));

          // some chunks are placed in DMA buffer
          // so we can save a little RAM here
          entries -= (i2sDmaBufMaxLen * i2sDmaBufCnt) / __scSet.chkInFrames;

          queueCreatedWithChkInFrames = __scSet.chkInFrames;

          pcmChkQHdl = xQueueCreate(entries, sizeof(pcm_chunk_message_t *));

          ESP_LOGI(TAG, "created new queue with %d", entries);
        }

        if ((scSet.sr != __scSet.sr) || (scSet.bits != __scSet.bits) ||
            (scSet.ch != __scSet.ch) || (scSet.buf_ms != __scSet.buf_ms)) {
          ESP_LOGI(TAG,
                   "snapserver config changed, buffer %ldms, chunk %ld frames, "
                   "sample rate %ld, ch %d, bits %d mute %d latency %ld",
                   __scSet.buf_ms, __scSet.chkInFrames, __scSet.sr, __scSet.ch,
                   __scSet.bits, __scSet.muted, __scSet.cDacLat_ms);
        }

        scSet = __scSet;  // store for next round

        gotSnapserverConfig = true;
      }

    } else if (gotSnapserverConfig == false) {
      // ESP_LOGW(TAG, "no snapserver config yet, keep waiting");

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
        chunkDuration_us =
            1000000LL *
            (int64_t)(chnk->totalSize / ((scSet.bits >> 3) * scSet.ch)) /
            (int64_t)scSet.sr;

        // ESP_LOGI(TAG, "got pcm chunk with size %d", chnk->fragment->size);
      }
    } else {
      // ESP_LOGW(TAG, "already retrieved chunk needs service");
      ret = pdPASS;
    }

    if (ret != pdFAIL) {
      int64_t chunkStart = (int64_t)chnk->timestamp.sec * 1000000LL +
                           (int64_t)chnk->timestamp.usec;

      if (initialSync == 0) {
        if (server_now(&serverNow, &diff2Server) >= 0) {
          age = serverNow - chunkStart - buf_us + clientDacLatency_us;
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
          bool dmaFull = false;

          MEDIANFILTER_Init(&shortMedianFilter);
          MEDIANFILTER_Init(&miniMedianFilter);

          tg0_timer1_start(-age);  // timer with 1µs ticks

          my_i2s_channel_disable(tx_chan);

#if !USE_SAMPLE_INSERTION
          adjust_apll(0);  // reset to normal playback speed
#endif
          while (1) {
            if (chnk == NULL) {
              if (pcmChkQHdl != NULL) {
                ret = xQueueReceive(pcmChkQHdl, &chnk, pdMS_TO_TICKS(100));
                // if (ret != pdFAIL) {
                //   ESP_LOGI(TAG, "got pcm chunk with size %d",
                //            chnk->fragment->size);
                // }
              }
            } else {
              // ESP_LOGI(TAG, "got pcm chunk with size %d",
              // chnk->fragment->size);
            }

            fragment = chnk->fragment;
            p_payload = fragment->payload;
            size = fragment->size;

            ESP_ERROR_CHECK(
                i2s_channel_preload_data(tx_chan, p_payload, size, &written));

            // check if DMA is full at first try here
            if (written != size) {
              dmaFull = true;
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
              }
            }

            if (dmaFull == true) {
              ESP_LOGI(TAG, "DMA completely loaded");

              alreadyWritten = 0;
              chunkStart -=
                  (1000000LL * (int64_t)(i2sDmaBufCnt * i2sDmaBufMaxLen) /
                   (int64_t)scSet.sr);

              break;
            }
          }

          // Wait to be notified of a timer interrupt.
          xTaskNotifyWait(pdFALSE,         // Don't clear bits on entry.
                          pdFALSE,         // Don't clear bits on exit.
                          &notifiedValue,  // Stores the notified value.
                          portMAX_DELAY);
          // or use simple task delay for this
          // vTaskDelay( pdMS_TO_TICKS(-age / 1000) );

          my_gptimer_stop(gptimer);

          my_i2s_channel_enable(tx_chan);

          // get timer value so we can get the real age
          timer_val = (int64_t)notifiedValue;

          // get actual age after alarm
          age = (int64_t)timer_val - (-age);

          initialSync = 1;

          // TODO: use a timer to un-mute non blocking
          vTaskDelay(pdMS_TO_TICKS(2));
          audio_set_mute(scSet.muted);

          ESP_LOGI(TAG, "initial sync age: %lldus, chunk duration: %lldus", age,
                   chunkDuration_us);

          if (size == 0) {
            continue;
          }
        } else if (age >= 0) {
          if (chnk != NULL) {
            free_pcm_chunk(chnk);
            chnk = NULL;
          }

          // get count of chunks we are late for
          uint32_t c = ceil((float)age / (float)chunkDuration_us);  // round up

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

          my_gptimer_stop(gptimer);

          ESP_LOGW(TAG,
                   "RESYNCING HARD 1: age %lldus, latency %lldus, free %d, "
                   "largest block %d, rssi: %d",
                   age, diff2Server, heap_caps_get_free_size(MALLOC_CAP_32BIT),
                   heap_caps_get_largest_free_block(MALLOC_CAP_32BIT), ap.rssi);

          dir = 0;

          insertedSamplesCounter = 0;

          audio_set_mute(true);

          my_i2s_channel_disable(tx_chan);

          continue;
        }
      }

      const bool enableControlLoop = true;

      const int64_t shortOffset = SHORT_OFFSET;  // µs, softsync
      const int64_t miniOffset = MINI_OFFSET;    // µs, softsync
      const int64_t hardResyncThreshold = 2000;  // µs, hard sync

      if (initialSync == 1) {
        if (size == 0) {
          fragment = chnk->fragment;
          p_payload = fragment->payload;
          size = fragment->size;
        }

        if (p_payload != NULL) {
          do {
            written = 0;

#if USE_SAMPLE_INSERTION
            uint32_t sampleSizeInBytes =
                (scSet.bits >> 3) * scSet.ch * INSERT_SAMPLES;

            if ((dir_insert_sample > 0) && (size >= sampleSizeInBytes)) {
              size -= sampleSizeInBytes;
            }
#endif
            int64_t alreadyWrittenTime_us = 0;
            size_t framesToBytes = (scSet.ch + (scSet.bits >> 3));
            while (size) {
              size_t i2sWriteLen;
              size_t tmpSize = i2sDmaBufMaxLen * framesToBytes;

              // #if USE_SAMPLE_INSERTION
              //               if (dir_insert_sample < 0) {
              //         	  tmpSize -= sampleSizeInBytes;
              //               }
              // #endif

              if (size >= tmpSize) {
                i2sWriteLen = i2sDmaBufMaxLen * framesToBytes - alreadyWritten;

                i2s_channel_write(tx_chan, p_payload, i2sWriteLen, &written,
                                  portMAX_DELAY);

                alreadyWrittenTime_us =
                    1000000LL * (int64_t)(alreadyWritten / framesToBytes) /
                    (int64_t)scSet.sr;
                chunkStart += (dmaDescDuration_us - alreadyWrittenTime_us);

                alreadyWritten = 0;

                outputBufferDacTime_us =
                    1000000ULL * i2sDmaBufMaxLen * i2sDmaBufCnt / scSet.sr;
              } else {  // here we are at the end of a chunk
                i2sWriteLen = size;

#if USE_SAMPLE_INSERTION
                size_t insertedSamplesWritten = 0;

                if (i2sWriteLen + sampleSizeInBytes <= i2sDmaBufMaxLen) {
                  if (dir_insert_sample < 0) {
                    if (i2s_channel_write(tx_chan, p_payload, sampleSizeInBytes,
                                          &insertedSamplesWritten,
                                          portMAX_DELAY) != ESP_OK) {
                      ESP_LOGE(TAG, "i2s_playback_task:  I2S write error %d",
                               1);
                    }
                  }

                  dir_insert_sample = 0;
                }
#endif

                i2s_channel_write(tx_chan, p_payload, i2sWriteLen, &written,
                                  portMAX_DELAY);

#if USE_SAMPLE_INSERTION
                alreadyWritten = written + insertedSamplesWritten;
#else
                alreadyWritten = written;
#endif
                alreadyWrittenTime_us =
                    1000000LL * (int64_t)(alreadyWritten / framesToBytes) /
                    (int64_t)scSet.sr;
                chunkStart += alreadyWrittenTime_us;

                outputBufferDacTime_us = (1000000ULL * i2sDmaBufMaxLen *
                                          (i2sDmaBufCnt - 1) / scSet.sr) +
                                         alreadyWrittenTime_us;
              }

              size -= written;
              p_payload += written;
            }

            dir = 0;

            if (size == 0) {
              if (fragment->nextFragment != NULL) {
                fragment = fragment->nextFragment;
                p_payload = fragment->payload;
                size = fragment->size;

                // ESP_LOGI (TAG, "%s: fragmented", __func__);
              } else {
                free_pcm_chunk(chnk);
                chnk = NULL;
                dir = 0;

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
            if (i2s_channel_write(tx_chan, tmpBuf, write_size, &written,
                                  portMAX_DELAY) != ESP_OK) {
              ESP_LOGE(TAG, "i2s_playback_task: I2S write error %d/%d", written,
                       size);
            }

            size -= written;
          } while (size);

          free_pcm_chunk(chnk);
          chnk = NULL;
        }

        if (server_now(&serverNow, &diff2Server) >= 0) {
          age = serverNow - chunkStart - buf_us + clientDacLatency_us +
                outputBufferDacTime_us;

          int64_t shortMedian, miniMedian;

          shortMedian = MEDIANFILTER_Insert(&shortMedianFilter, age);
          miniMedian = MEDIANFILTER_Insert(&miniMedianFilter, age);

          int msgWaiting = uxQueueMessagesWaiting(pcmChkQHdl);

          // resync hard if we are getting very late / early.
          // rest gets tuned in through apll speed control or sample insertion
          if ((msgWaiting == 0) ||
              (MEDIANFILTER_isFull(&shortMedianFilter, 0) &&
               ((shortMedian > hardResyncThreshold) ||
                (shortMedian < -hardResyncThreshold)))) {
            if (chnk != NULL) {
              free_pcm_chunk(chnk);
              chnk = NULL;
            }

            wifi_ap_record_t ap;
            esp_wifi_sta_get_ap_info(&ap);

            ESP_LOGW(TAG,
                     "RESYNCING HARD 2: age %lldus, latency %lldus, free "
                     "%d, largest block %d, %d, rssi: %d",
                     age, diff2Server,
                     heap_caps_get_free_size(MALLOC_CAP_32BIT),
                     heap_caps_get_largest_free_block(MALLOC_CAP_32BIT),
                     msgWaiting, ap.rssi);

            my_gptimer_stop(gptimer);

            audio_set_mute(true);

            my_i2s_channel_disable(tx_chan);

            initialSync = 0;

            insertedSamplesCounter = 0;

            continue;
          }

#if USE_SAMPLE_INSERTION  // insert samples to adjust sync
          if ((enableControlLoop == true) &&
              (MEDIANFILTER_isFull(&shortMedianFilter, 0))) {
            if ((shortMedian < -shortOffset) && (miniMedian < -miniOffset) &&
                (age < -miniOffset)) {  // we are early
              dir = -1;
              dir_insert_sample = -1;
              insertedSamplesCounter += INSERT_SAMPLES;
            } else if ((shortMedian > shortOffset) &&
                       (miniMedian > miniOffset) &&
                       (age > miniOffset)) {  // we are late
              dir = 1;
              dir_insert_sample = 1;
              insertedSamplesCounter -= INSERT_SAMPLES;
            }
          }
#else  // use APLL to adjust sync
          if ((enableControlLoop == true) &&
              (MEDIANFILTER_isFull(&shortMedianFilter, 0))) {
            if ((shortMedian < -shortOffset) && (miniMedian < -miniOffset) &&
                (age < -miniOffset)) {  // we are early
              dir = -1;
            } else if ((shortMedian > shortOffset) &&
                       (miniMedian > miniOffset) &&
                       (age > miniOffset)) {  // we are late
              dir = 1;
            }

            adjust_apll(dir);
          }
#endif

          //        ESP_LOGI(TAG, "%d, %lldus, %lldus, %lldus, q:%d, %lld,
          //        %llu", dir, age,
          //                 shortMedian, miniMedian,
          //                 uxQueueMessagesWaiting(pcmChkQHdl),
          //                 insertedSamplesCounter, chkDur_us);
          //
          // ESP_LOGI(TAG, "%d, %lldus, %lldus, %lldus, q:%d, %lld, %lld", dir,
          //         age, shortMedian, miniMedian,
          //         uxQueueMessagesWaiting(pcmChkQHdl), insertedSamplesCounter,
          //         chunkDuration_us);

          // ESP_LOGI( TAG, "8b f %d b %d",
          // 		   heap_caps_get_free_size(MALLOC_CAP_8BIT |
          //           						   MALLOC_CAP_INTERNAL),
          //           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
          //                                            MALLOC_CAP_INTERNAL));
          // ESP_LOGI( TAG, "32b f %d b %d",
          //           heap_caps_get_free_size(MALLOC_CAP_32BIT |
          //                                   MALLOC_CAP_EXEC),
          //           heap_caps_get_largest_free_block(MALLOC_CAP_32BIT |
          //		 MALLOC_CAP_EXEC));
        } else {
          // ESP_LOGW(TAG, "couldn't get server now");

          if (chnk != NULL) {
            free_pcm_chunk(chnk);
            chnk = NULL;
          }

          vTaskDelay(pdMS_TO_TICKS(1));

          continue;
        }
      }
    } else {
      int64_t sec, msec, usec;

      sec = diff2Server / 1000000;
      usec = diff2Server - sec * 1000000;
      msec = usec / 1000;
      usec = usec % 1000;

      if (pcmChkQHdl != NULL) {
        ESP_LOGV(TAG,
                 "Couldn't get PCM chunk, recv: messages waiting %d, "
                 "diff2Server: %llds, %lld.%lldms",
                 uxQueueMessagesWaiting(pcmChkQHdl), sec, msec, usec);
      }

      dir = 0;

      initialSync = 0;

      audio_set_mute(true);

      my_i2s_channel_disable(tx_chan);
    }
  }
}

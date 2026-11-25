/**
 *
 */

#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#include "driver/i2s_common.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// #include "lwip/stats.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wifi_interface.h"
#include "soc/rtc.h"

#if SOC_I2S_SUPPORTS_APLL
#include "clk_ctrl_os.h"
#endif

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include <math.h>

#include "MedianFilter.h"
#include "TimeFilter.h"
#include "driver/gptimer.h"
#include "driver/i2s_std.h"
#include "player.h"
#include "snapcast.h"

#define USE_SAMPLE_INSERTION CONFIG_USE_SAMPLE_INSERTION

#define SYNC_TASK_PRIORITY 20
#define SYNC_TASK_CORE_ID 1  // tskNO_AFFINITY

static const char *TAG = "PLAYER";

#if USE_SAMPLE_INSERTION

#if CONFIG_PM_ENABLE
esp_pm_lock_handle_t player_pm_lock_handle = NULL;
#endif

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
static SemaphoreHandle_t latencyBufFullSemaphoreHandle = NULL;

static gptimer_handle_t gptimer = NULL;

#if USE_TIMEFILTER
static sTimeFilter_t latencyTimeFilter;

static double latencyToServer = 0;
static double latencyDrift = 0;
static int64_t latencyLastUpdate = 0;
#else
static sMedianFilter_t latencyMedianFilter;
static sMedianNode_t latencyMedianLong[LATENCY_MEDIAN_FILTER_LEN];

static int64_t latencyToServer = 0;
#endif

static sMedianFilter_t shortMedianFilter;
static sMedianNode_t shortMedianBuffer[SHORT_BUFFER_LEN];

static sMedianFilter_t miniMedianFilter;
static sMedianNode_t miniMedianBuffer[MINI_BUFFER_LEN];

static int8_t currentDir = 0;  //!< current apll direction, see apll_adjust()

static TaskHandle_t playerTaskHandle = NULL;

static QueueHandle_t snapcastSettingQueueHandle = NULL;

static uint32_t i2sDmaBufCnt;
static uint32_t i2sDmaBufMaxLen;
static playerSetting_t *scSet;  // should be used only from http_task

// For player_get_buffer_fill(): duration of the last played PCM chunk and of
// all I2S DMA buffers
static volatile int64_t diagChunkUs = 0;
static volatile int64_t diagDmaUs = 0;

static void tg0_timer_init(void);
static void tg0_timer_deinit(void);

static bool gpTimerRunning = false;

/**
 * Latency between the I2S output and the analog output: the client latency set
 * on the server plus the DAC's own filter delay (CONFIG_DAC_GROUP_DELAY_FRAMES,
 * in frames of the current sample rate).
 */
static int64_t client_dac_latency_us(const playerSetting_t *set) {
  int64_t us = (int64_t)set->cDacLat_ms * 1000LL;
#if CONFIG_DAC_GROUP_DELAY_FRAMES > 0
  if (set->sr > 0) {
    us += (int64_t)CONFIG_DAC_GROUP_DELAY_FRAMES * 1000000LL / set->sr;
  }
#endif
  return us;
}

static void player_task(void *pvParameters);

//player state
bool playerStarted = false;
bool playerPaused = false;
static SemaphoreHandle_t playerStateMux = NULL;

static void (*state_cb)(bool) = NULL;
static volatile player_event_cb_t event_cb = NULL;
static void (*audio_set_mute)(bool mute);
static bool (*lock_i2s)(bool, TickType_t) = NULL;

static i2s_chan_handle_t tx_chan = NULL;  // I2S tx channel handler

// Real I2S output timing, written by the DMA callbacks: descriptors sent and
// underruns since the last initial sync, and when the last descriptor
// finished. The sync loop takes the playout time from these instead of
// counting written samples: the I2S clock divider is not exact (-5.5 ppm at
// 44.1 kHz from the 160 MHz PLL), and an underrun plays a descriptor of zeros
// the sample count never sees. Both used to add up to a growing offset that
// the loop could not detect (~0.33 ms/min, 141 ms after a night).
#if CONFIG_I2S_ISR_IRAM_SAFE
#define I2S_CB_ATTR IRAM_ATTR
#else
#define I2S_CB_ATTR
#endif

static uint32_t i2sDescSent = 0;
static uint32_t i2sUnderruns = 0;
static int64_t i2sLastSentUs = 0;
static portMUX_TYPE i2sTimingMux = portMUX_INITIALIZER_UNLOCKED;

static bool I2S_CB_ATTR i2s_on_sent(i2s_chan_handle_t handle,
                                    i2s_event_data_t *event, void *user_ctx) {
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&i2sTimingMux);
  i2sDescSent++;
  i2sLastSentUs = now;
  portEXIT_CRITICAL_ISR(&i2sTimingMux);
  return false;
}

// Called after on_sent when the descriptor just sent had not been refilled,
// so it played zeros (auto_clear)
static bool I2S_CB_ATTR i2s_on_send_q_ovf(i2s_chan_handle_t handle,
                                          i2s_event_data_t *event,
                                          void *user_ctx) {
  portENTER_CRITICAL_ISR(&i2sTimingMux);
  i2sUnderruns++;
  portEXIT_CRITICAL_ISR(&i2sTimingMux);
  return false;
}

static void i2s_timing_reset(void) {
  portENTER_CRITICAL(&i2sTimingMux);
  i2sDescSent = 0;
  i2sUnderruns = 0;
  i2sLastSentUs = 0;
  portEXIT_CRITICAL(&i2sTimingMux);
}

static void i2s_timing_get(uint32_t *descs, uint32_t *underruns,
                           int64_t *lastSentUs) {
  portENTER_CRITICAL(&i2sTimingMux);
  *descs = i2sDescSent;
  *underruns = i2sUnderruns;
  *lastSentUs = i2sLastSentUs;
  portEXIT_CRITICAL(&i2sTimingMux);
}

// RESYNCING HARD 1/2 lines: at most one per second, shared by both. While
// catching up, hard resyncs come ~70 times per second; each line blocks this
// high-priority task ~12 ms on the UART, and the Wi-Fi and heap queries in it
// ran on every resync. Based on #234 by W-Floyd.
#define RESYNC_LOG_INTERVAL_US 1000000LL

static int64_t resyncLogLastUs = 0;
static uint32_t resyncLogSuppressed = 0;

/**
 * True if a RESYNCING HARD line may be printed now; then *suppressed is the
 * number of lines skipped since the last one. Otherwise counts this one as
 * skipped.
 */
static bool resync_log_due(uint32_t *suppressed) {
  int64_t now = esp_timer_get_time();
  if ((resyncLogLastUs != 0) &&
      (now - resyncLogLastUs < RESYNC_LOG_INTERVAL_US)) {
    resyncLogSuppressed++;
    return false;
  }
  resyncLogLastUs = now;
  *suppressed = resyncLogSuppressed;
  resyncLogSuppressed = 0;
  return true;
}

static bool i2sEnabled = false;

i2s_std_gpio_config_t pin_config0;
i2s_port_t i2sNum;

// Function to find the minimum using ternary operator
static int64_t MIN(int64_t x, int64_t y) {
    return (x < y) ? x : y;
}

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
 * This is a dirty hack to ensure smooth audio output without pops/clicks.
 * It was originally developed to suppress es8388 noise between channel
 * initialization and enabling. But the root cause of the noise was eliminated
 * in es8388 driver. However: this function is still useful to suppress
 * pops/clicks, typically when "pcm chunk queue not created" happens. It streams
 * silence to I2S before starting actual audio playback. It's unclear, why this
 * works, but it does.
 */
static void ensure_noiseless(i2s_chan_handle_t tx) {
  // zero buffer is const, so it lives in flash and costs no RAM
  static const uint8_t silence_buf[512] __attribute__((aligned(4))) = {0};

  // We only need to prime I2S once
  static bool i2s_primed = false;
  if (i2s_primed) {
    return;
  }
  i2s_primed = true;

  ESP_LOGI(TAG, "Priming I2S with silence to avoid noise");
  // enabling I2S channel stops noise
  my_i2s_channel_enable(tx);

  // This could be adjusted size according to sample rate, bits, channels
  size_t silence_size = 44100 * 2 * 2 / 10;
  size_t total_written = 0;

  // Write silence
  // While this is not required to stop noise, it somehow prevents pops later.
  while (total_written < silence_size) {
    size_t bytes_written = 0;
    size_t len = silence_size - total_written;
    if (len > sizeof(silence_buf)) {
      len = sizeof(silence_buf);
    }

    if (i2s_channel_write(tx, silence_buf, len, &bytes_written,
                          pdMS_TO_TICKS(200)) != ESP_OK ||
        bytes_written == 0) {
      break;
    }
    total_written += bytes_written;
  }

  // Wait for DMA to flush
  vTaskDelay(pdMS_TO_TICKS(150));

  ESP_LOGI(TAG, "Audio path primed with %d bytes of silence", total_written);

  // Disable I2S channel again. Leaving it enabled may cause pops later.
  my_i2s_channel_disable(tx);
}

/**
 *
 */
static esp_err_t player_setup_i2s(playerSetting_t *setting, bool lock) {

  if (lock_i2s != NULL && lock) {
    if (lock_i2s(true, pdMS_TO_TICKS(10)) != pdTRUE) {
      return -1;
    }
  }

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
  i2sDmaBufCnt = 2;
  // OPUS has a minimum frame size of 120
  // with DMA buffer set to this value sync algorithm
  // works for all decoders. We set it to 100 so
  // there will be free space for sample stuffing in each round
  i2sDmaBufMaxLen = 1023;
#else
  int fi2s_clk;
  const int __dmaBufMaxLen = 1023;
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
  fi2s_clk =
      2 * sr * I2S_MCLK_MULTIPLE_256;  // setting->ch * setting->bits * m_scale;

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
      .auto_clear = true,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

  ESP_LOGI(TAG,
           "player_setup_i2s: dma_buf_len is %ld, dma_buf_count is %ld, sample "
           "rate: %ld, bits: %d",
           i2sDmaBufMaxLen, i2sDmaBufCnt, sr, bits);

  i2s_std_clk_config_t i2s_clkcfg = I2S_STD_CLK_DEFAULT_CONFIG(sr);

#if USE_SAMPLE_INSERTION
  i2s_clkcfg.clk_src = I2S_CLK_SRC_DEFAULT;
#else
  i2s_clkcfg.clk_src = I2S_CLK_SRC_APLL;
#endif

  // Please set the mclk_multiple to I2S_MCLK_MULTIPLE_384
  // while using 24 bits data width Otherwise the sample rate
  // might be imprecise since the BCLK division is not a integer
  if (bits > I2S_DATA_BIT_WIDTH_16BIT) {
    i2s_clkcfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
  }

  i2s_std_config_t tx_std_cfg = {
      .clk_cfg = i2s_clkcfg,
#if CONFIG_I2S_USE_MSB_FORMAT
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_STEREO),
#else
      .slot_cfg =
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_STEREO),
#endif
      .gpio_cfg = pin_config0,
  };
#if CONFIG_I2S_SLOT_32BIT
  // MA120X0P (and similar) only support 32-bit or 24-bit BCLK frames.
  // Override slot width so the ESP32 sends 32 BCLK pulses per channel;
  // 16-bit samples are zero-padded by the IDF automatically.
  tx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
#endif

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));

  i2s_event_callbacks_t i2sCbs = {
      .on_sent = i2s_on_sent,
      .on_send_q_ovf = i2s_on_send_q_ovf,
  };
  ESP_ERROR_CHECK(i2s_channel_register_event_callback(tx_chan, &i2sCbs, NULL));
  // This prevents pops/clicks on some I2S codecs
  ensure_noiseless(tx_chan);

  // my_i2s_channel_enable(tx_chan);

  return 0;
}

/**
 * ensure this is called after http_task was killed!
 */
int deinit_player(void) {
  int ret = 0;

  // must disable i2s before stopping player task or it will hang
  my_i2s_channel_disable(tx_chan);

  //don't take playerStateMux here, otherwise we might block player task from stopping
  //if (playerStateMux != NULL) {
  //  xSemaphoreTake(playerStateMux, pdMS_TO_TICKS(10000));
  //}

  //wait max 10s for task to stop itself
  for(int i = 0; i< 100; i++) {
    if (playerStarted) {
      vTaskDelay(pdMS_TO_TICKS(100));
    } else {
      break;
    }
  }

  // stop the task if still running
  if (playerTaskHandle != NULL) {
    vTaskDelete(playerTaskHandle);
    playerTaskHandle = NULL;
  }
  
  if (tx_chan) {
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
  if (lock_i2s != NULL) {
    lock_i2s(false, 0);
  }

  if (playerStateMux != NULL) {
    vSemaphoreDelete(playerStateMux);
    playerStateMux = NULL;
  }
  if (snapcastSettingQueueHandle != NULL) {
    // delete the queue
    vQueueDelete(snapcastSettingQueueHandle);
    snapcastSettingQueueHandle = NULL;
  }
  pcm_pool_flush();

  if (latencyBufSemaphoreHandle != NULL) {
    vSemaphoreDelete(latencyBufSemaphoreHandle);
    latencyBufSemaphoreHandle = NULL;
  }

  if (latencyBufFullSemaphoreHandle != NULL) {
    vSemaphoreDelete(latencyBufFullSemaphoreHandle);
    latencyBufFullSemaphoreHandle = NULL;
  }


  tg0_timer_deinit();
  
#if CONFIG_PM_ENABLE
  if (player_pm_lock_handle) {
    esp_pm_lock_delete(player_pm_lock_handle);
    player_pm_lock_handle = NULL;
  }
#endif

  ESP_LOGI(TAG, "deinit player done");

  return ret;
}

void player_set_event_cb(player_event_cb_t cb) { event_cb = cb; }

static void player_event(player_event_t event, int64_t value) {
  player_event_cb_t cb = event_cb;
  if (cb != NULL) {
    cb(event, value);
  }
}

void call_state_cb(void) {
  if (state_cb != NULL) {
    xSemaphoreTake(playerStateMux, portMAX_DELAY);
    bool paused = playerPaused;
    xSemaphoreGive(playerStateMux);
    state_cb(paused);
  }
}

/**
 *  call before http task creation!
 */
int init_player(i2s_std_gpio_config_t pin_config0_, i2s_port_t i2sNum_, void (*set_mute_cb)(bool), void (*cb)(bool), bool (*lock)(bool, TickType_t)) {
  if (set_mute_cb == NULL) {
    ESP_LOGE(TAG, "set_mute_cb is NULL");
    return -1;
  }
   
  audio_set_mute = set_mute_cb;
  state_cb = cb; // can be NULL
  lock_i2s = lock; // can be NULL


  deinit_player();

  pin_config0 = pin_config0_;
  i2sNum = i2sNum_;

  // create message queue to inform task of changed settings
  snapcastSettingQueueHandle = xQueueCreate(1, sizeof(playerSetting_t));
  if (snapcastSettingQueueHandle == NULL) {
    ESP_LOGE(TAG, "Failed to create snapcast settings queue");
    return -1;
  }

  if (playerStateMux == NULL) {
    playerStateMux = xSemaphoreCreateMutex();
    xSemaphoreGive(playerStateMux);
  }


  // create semaphore for time diff buffer to server
  if (latencyBufSemaphoreHandle == NULL) {
    latencyBufSemaphoreHandle = xSemaphoreCreateMutex();
  }

  if (latencyBufFullSemaphoreHandle == NULL) {
    latencyBufFullSemaphoreHandle = xSemaphoreCreateBinary();
  }
  xSemaphoreTake(latencyBufFullSemaphoreHandle, 0);

  
#if USE_TIMEFILTER
  // init Kalmann time filter 
  TIMEFILTER_Init(&latencyTimeFilter, 0.01, 0.0, 1.001, 0.75, 100, 2.0);
#else
  // init diff buff median filter
  latencyMedianFilter.numNodes = LATENCY_MEDIAN_FILTER_LEN;
  latencyMedianFilter.medianBuffer = latencyMedianLong;
  reset_latency_buffer();
#endif
  shortMedianFilter.numNodes = SHORT_BUFFER_LEN;
  shortMedianFilter.medianBuffer = shortMedianBuffer;
  MEDIANFILTER_Init(&shortMedianFilter);

  miniMedianFilter.numNodes = MINI_BUFFER_LEN;
  miniMedianFilter.medianBuffer = miniMedianBuffer;
  MEDIANFILTER_Init(&miniMedianFilter);
  
  #if CONFIG_PM_ENABLE
  esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "player", &player_pm_lock_handle);
  #endif

  ESP_LOGI(TAG, "init player done");

  return 0;
}

/**
 * call to start the player task
 */
int start_player() {
  if (xSemaphoreTake(playerStateMux, 0) != pdTRUE) {
    // shutdown in progress, don't start player
    return -1;
  }
  if (playerStarted || playerPaused){
      xSemaphoreGive(playerStateMux);
      return -1;
  }
  if (scSet == NULL || !(( scSet->buf_ms > 0) && (scSet->chkInFrames > 0)) ||
      !pcm_pool_ready()) {
      xSemaphoreGive(playerStateMux);
      return -1;
  }
  playerStarted = true;
  int ret = 0;

  ret = player_setup_i2s(scSet, true);
  if (ret < 0) {
    ESP_LOGE(TAG, "player_setup_i2s failed: %d", ret);
    playerStarted = false;
    xSemaphoreGive(playerStateMux);
    return -1;
  }
  xSemaphoreGive(playerStateMux);

  tg0_timer_init();

#if CONFIG_PM_ENABLE
  ESP_LOGI(TAG, "reset Latency buffer");

  while(reset_latency_buffer()<0) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  
  esp_pm_lock_acquire(player_pm_lock_handle);
#endif
  
  ESP_LOGI(TAG, "Start player_task");

  // The new task starts from the current settings in scSet. A setting still
  // queued was sent to the previous task while it was stopping (e.g. 44.1 kHz
  // before a switch to 48 kHz); applied now it would set I2S to the old rate.
  xQueueReset(snapcastSettingQueueHandle);

  if (xTaskCreatePinnedToCore(player_task, "player", 1024 * 3, (void*) scSet,
                              SYNC_TASK_PRIORITY, &playerTaskHandle,
                              SYNC_TASK_CORE_ID) != pdPASS) {
    // undo the above, or playerStarted stays set and nothing restarts it
    ESP_LOGE(TAG, "Failed to create player task, %u B free",
             heap_caps_get_free_size(MALLOC_CAP_8BIT));
    playerTaskHandle = NULL;
    my_i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
    if (lock_i2s != NULL) {
      lock_i2s(false, 0);
    }
    tg0_timer_deinit();
#if CONFIG_PM_ENABLE
    esp_pm_lock_release(player_pm_lock_handle);
#endif
    playerStarted = false;

    return -1;
  }

  call_state_cb();
  ESP_LOGI(TAG, "start player done");

  return 0;
}

void pause_player(bool pause) {
  xSemaphoreTake(playerStateMux, portMAX_DELAY);
  if (pause != playerPaused) {
    playerPaused = pause;
    xSemaphoreGive(playerStateMux);
    if (pause) {
      stop_player_task();
    }
    else {
      call_state_cb();  // notify state change, e.g. for http task to send pcm
    }
  } else {
    xSemaphoreGive(playerStateMux);
  }
}

void stop_player_task() {
  if (playerTaskHandle != NULL) {
    xTaskNotifyGiveIndexed(playerTaskHandle, 1);
  }
}

#if USE_TIMEFILTER
/**
 *
 */
int32_t player_latency_insert(int64_t newValue, int64_t max_error, int64_t time_added) {
  TIMEFILTER_Insert(&latencyTimeFilter, newValue, max_error, time_added);
  int64_t last_update_ = latencyTimeFilter.last_update_;
  double offset_ = latencyTimeFilter.offset_;
  double drift_ = latencyTimeFilter.use_drift_ ? latencyTimeFilter.drift_ : 0.0;
  if (xSemaphoreTake(latencyBufSemaphoreHandle, pdMS_TO_TICKS(0)) == pdTRUE) {
    if (TIMEFILTER_isFull(&latencyTimeFilter, LATENCY_TIME_FILTER_FULL)) {
      xSemaphoreGive(latencyBufFullSemaphoreHandle);
      //ESP_LOGI(TAG, "offset: %fus, diff: %lld", offset_, newValue - (uint64_t)offset_);
    }
    //else {
      // ESP_LOGI(TAG, "not full: offset: %fus, diff: %lld", offset_, newValue - (uint64_t)offset_);
    //}

    latencyToServer = offset_;
    latencyDrift = drift_;
    latencyLastUpdate = last_update_;
    //ESP_LOGI(TAG, "Timefilter: drift %f, max err %lldus, \n%lldus new val\n%fus offset", drift_, max_error, newValue, offset_);
    xSemaphoreGive(latencyBufSemaphoreHandle);
  } else {
    ESP_LOGW(TAG, "couldn't set latencyToServer = medianValue");
  }

  return 0;
}
#else
/**
 *
 */
int32_t player_latency_insert(int64_t newValue) {
  int64_t medianValue;

  medianValue = MEDIANFILTER_Insert(&latencyMedianFilter, newValue);
  if (xSemaphoreTake(latencyBufSemaphoreHandle, pdMS_TO_TICKS(0)) == pdTRUE) {
    if (MEDIANFILTER_isFull(&latencyMedianFilter, LATENCY_MEDIAN_FILTER_FULL)) {
      xSemaphoreGive(latencyBufFullSemaphoreHandle);

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
#endif

/**
 *
 */
int32_t player_send_snapcast_setting(playerSetting_t *setting) {
  int ret;

  if ((playerTaskHandle != NULL) && (snapcastSettingQueueHandle != NULL)) {
    ret = xQueueOverwrite(snapcastSettingQueueHandle, setting);
    if (ret != pdPASS) {
      ESP_LOGE(TAG,
                "player_send_snapcast_setting: couldn't notify "
                "snapcast setting");
    } else {
                ESP_LOGI(TAG,
                "got settings and notified player_task");
    }
  } else if (scSet == NULL) {
    scSet = setting;
    ESP_LOGI(TAG,
                "got initial settings");
  }

  return pdPASS;
}

#if USE_TIMEFILTER
/**
 *
 */
int32_t reset_latency_buffer(void) {
  // init diff buff median filter
  TIMEFILTER_Reset(&latencyTimeFilter);

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "reset_diff_buffer: latencyBufSemaphoreHandle == NULL");

    return -2;
  }
  xSemaphoreTake(latencyBufFullSemaphoreHandle, pdMS_TO_TICKS(10));
  if (xSemaphoreTake(latencyBufSemaphoreHandle, pdMS_TO_TICKS(100)) == pdTRUE) {
    latencyToServer = 0;
    latencyDrift = 0;
    latencyLastUpdate = 0;

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
int32_t latency_buffer_full(bool *is_full) {
  *is_full = TIMEFILTER_isFull(&latencyTimeFilter, LATENCY_TIME_FILTER_FULL);
  return 0;
}

/**
 *
 */
int32_t get_diff_to_server(int64_t *tDiff, int64_t now) {
  static double lastDiff = 0;
  static double lastDrift = 0;
  static int64_t lastLastUpdate = 0;

  if (latencyBufSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "get_diff_to_server: latencyBufSemaphoreHandle == NULL");

    return -2;
  }

  double dt;
  int64_t offset;
  if (xSemaphoreTake(latencyBufSemaphoreHandle, 0) == pdFALSE) {
    dt = now - lastLastUpdate;
    offset = round(lastDiff + lastDrift * dt);
    *tDiff = offset;

     // ESP_LOGW(TAG,
     //         "get_diff_to_server: can't take semaphore. Old diff retrieved");

    return -1;
  }

  dt = now - latencyLastUpdate;
  offset = round(latencyToServer + latencyDrift * dt);

  *tDiff = offset;
  lastLastUpdate = latencyLastUpdate;
  lastDrift = latencyDrift;
  lastDiff = latencyToServer;  // store value, so we can return a value if
                               // semaphore couldn't be taken

  xSemaphoreGive(latencyBufSemaphoreHandle);

  return 0;
}

#else

/**
 *
 */
int32_t reset_latency_buffer(void) {
  xSemaphoreTake(latencyBufFullSemaphoreHandle, pdMS_TO_TICKS(10));
  
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
int32_t latency_buffer_full(bool *is_full) {
  *is_full = MEDIANFILTER_isFull(&latencyMedianFilter, LATENCY_MEDIAN_FILTER_FULL);
  
  return 0;
}

int32_t get_diff_to_server(int64_t *tDiff, int64_t now) {
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
#endif

/**
 *
 */
int32_t server_now(int64_t *sNow, int64_t *diff2Server) {
  int64_t diff, now;

  if (sNow == NULL) {
    return -2;
  }

  now = esp_timer_get_time();

  if (get_diff_to_server(&diff, now) == -1) {
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
  xTaskNotifyIndexedFromISR(playerTaskHandle, 0, (uint32_t)timer_counter_value,
                     eSetValueWithOverwrite, &xHigherPriorityTaskWoken);

  return xHigherPriorityTaskWoken == pdTRUE;
}

/**
 *
 */
esp_err_t my_gptimer_stop(gptimer_handle_t timer) {
  if (gpTimerRunning == true) {
    gpTimerRunning = false;
    
    esp_err_t ret = 0;
    ret |= gptimer_stop(timer);
    ret |= gptimer_disable(timer);
    
    return ret;
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
  //  timer_deinit(TIMER_GROUP_1, TIMER_1);
  if (gptimer) {
    ESP_ERROR_CHECK(my_gptimer_stop(gptimer));
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

  ESP_LOGI(TAG, "init initial sync timer");
}

/**
 *
 */
static void tg0_timer1_start(uint64_t alarm_value) {
  if (gptimer) {
    my_gptimer_stop(gptimer);
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
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
 * Frames the I2S DMA buffers hold, as player_setup_i2s() sets them up
 */
static uint32_t player_dma_frames(uint32_t chkInFrames) {
#if USE_SAMPLE_INSERTION
  (void)chkInFrames;

  return 2 * 1023;
#else
  // split into CHNK_CTRL_CNT * (power of 2) buffers, the same frames in total
  return chkInFrames * CHNK_CTRL_CNT;
#endif
}

// Chunks on top of the play queue. None: the decoder waits for a free chunk
// (PLAYER_FREE_CHUNK_WAIT_MS), so the chunk being decoded and the one being
// played come out of the queue's share and the rest waits in the TCP window,
// as compressed audio. Measured under bad Wi-Fi: as robust as 2 spare chunks
// at the same buffer, with ~9 KB more heap free.
#define PLAYER_POOL_SPARE_CHUNKS 0

/**
 * Chunks the pool needs for buf_ms: the play queue holds the buffer minus the
 * whole chunks in the I2S DMA buffers (as the old pcm queue did), plus
 * PLAYER_POOL_SPARE_CHUNKS.
 */
uint32_t player_chunk_slots(int32_t sr, uint32_t chkInFrames,
                            uint32_t buf_ms) {
  if ((sr <= 0) || (chkInFrames == 0) || (buf_ms == 0)) {
    return 0;
  }

  uint64_t frames = (uint64_t)sr * buf_ms;
  uint64_t perChunk = (uint64_t)chkInFrames * 1000;
  int64_t queued = (int64_t)((frames + perChunk - 1) / perChunk) -
                   player_dma_frames(chkInFrames) / chkInFrames;
  if (queued < 1) {
    queued = 1;
  }

  return (uint32_t)queued + PLAYER_POOL_SPARE_CHUNKS;
}

// How long the decoder waits for a free chunk while the player runs. The
// server sends audio up to its buffer ahead, which can be a bit more than the
// pool holds (time sync offset, network jitter). Waiting leaves the rest in
// the TCP window; dropping would leave a gap and force a hard resync, and a
// full pool would do that again and again.
#define PLAYER_FREE_CHUNK_WAIT_MS 100

/**
 * A free chunk for the decoder, or NULL if the player can't take one now:
 * paused, no pool, time sync not ready yet, or the buffer stays full. The
 * decoder drops the audio then.
 */
pcm_chunk_message_t *player_chunk_get(void) {
  if (playerPaused || !pcm_pool_ready()) {
    return NULL;
  }

  bool isFull = false;
  latency_buffer_full(&isFull);
  if (isFull == false) {
    // wait for initial latency measurement to finish
    return NULL;
  }

  player_event_cb_t cb = event_cb;
  int64_t waitStart = cb ? esp_timer_get_time() : 0;
  pcm_chunk_message_t *chunk = pcm_pool_get_free(
      playerStarted ? pdMS_TO_TICKS(PLAYER_FREE_CHUNK_WAIT_MS) : 0);
  if (cb) {
    cb(chunk ? PLAYER_EVENT_FREE_CHUNK_WAIT : PLAYER_EVENT_FREE_CHUNK_FAIL,
       esp_timer_get_time() - waitStart);
  }
  if (chunk == NULL) {
    ESP_LOGV(TAG, "no free pcm chunk, %lu waiting for playback",
             pcm_pool_filled_count());
  }

  return chunk;
}

/**
 * Queues a filled chunk for playback and starts the player if needed
 */
void player_chunk_submit(pcm_chunk_message_t *chunk) {
  if (chunk == NULL) {
    return;
  }

  if (!playerStarted) {
    // Drop this chunk and what is left from the last playback. Starting the
    // player resets the latency buffer, and player_chunk_get() returns no
    // chunks until it is full again: a chunk queued now would be followed by
    // a gap.
    pcm_pool_flush();
    pcm_pool_put_free(chunk);
    if (!playerPaused) {
      start_player();
    }

    return;
  }

  pcm_pool_put_filled(chunk);
}

/**
 * Returns an unused chunk to the pool
 */
void player_chunk_discard(pcm_chunk_message_t *chunk) {
  pcm_pool_put_free(chunk);
}

/**
 * Stops the player task and waits until it is gone, so the pool can be
 * rebuilt. The task notices a stop request within PLAYER_STOP_POLL_MS.
 */
void stop_player_and_wait(void) {
  stop_player_task();

  // max 5 s; the initial sync waits up to the server buffer
  for (int i = 0; (i < 500) && playerStarted; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (playerStarted) {
    ESP_LOGE(TAG, "player task didn't stop");
  }
}

/**
 * Diagnostics only: PCM chunks waiting for playback, the pool size, and the
 * duration of one chunk and of the I2S DMA buffers. Not synchronized with the
 * decoder, which may rebuild the pool when the stream changes.
 */
void player_get_buffer_fill(uint32_t *waiting, uint32_t *length,
                            uint32_t *chunkUs, uint32_t *dmaUs) {
  *waiting = pcm_pool_filled_count();
  *length = pcm_pool_slots();
  *chunkUs = (uint32_t)diagChunkUs;
  *dmaUs = (uint32_t)diagDmaUs;
}

/**
 *
 */
int32_t pcm_chunk_queue_msg_waiting(void) {
  return pcm_pool_filled_count();
}

static bool audioCodecCanSleep = false;

// how often player_task checks for a stop request while it waits for audio,
// and how long it waits before it stops on its own
#define PLAYER_STOP_POLL_MS 100
#define PLAYER_IDLE_TIMEOUT_MS 2000

// true once stop_player_task() asked the task to stop
static bool player_stop_requested(void) {
  return ulTaskNotifyTakeIndexed(1, pdTRUE, 0) == pdTRUE;
}

/**
 * Next chunk to play. NULL on a stop request (*stop is set) or after
 * PLAYER_IDLE_TIMEOUT_MS without audio.
 */
static pcm_chunk_message_t *player_next_chunk(bool *stop) {
  for (int waited = 0; waited < PLAYER_IDLE_TIMEOUT_MS;
       waited += PLAYER_STOP_POLL_MS) {
    pcm_chunk_message_t *chunk =
        pcm_pool_get_filled(pdMS_TO_TICKS(PLAYER_STOP_POLL_MS));
    if (chunk != NULL) {
      return chunk;
    }
    if (player_stop_requested()) {
      *stop = true;
      return NULL;
    }
  }

  return NULL;
}

/**
 *
 */
static void player_task(void *pvParameters) {
  pcm_chunk_message_t *chnk = NULL;
  int64_t age;
  int64_t serverNow = 0;
  BaseType_t ret;
  int64_t chunkDuration_us = 24000;
  char *p_payload = NULL;
  size_t size = 0;
  uint32_t notifiedValue;
  playerSetting_t scSet;
  uint64_t timer_val;
  int initialSync = 0;
  int dir = 0;
  int32_t dir_insert_sample = 0;
  int64_t insertedSamplesCounter = 0;
  int64_t buf_us = 0;
  size_t written;
  int64_t clientDacLatency_us = 0;
  int64_t diff2Server = 0;
  int64_t outputBufferDacTime_us = 0;
  int64_t dmaDescDuration_us = 0;
  int64_t playback_start_time_us = 0;
  uint32_t underrunsSeen = 0;
  uint64_t samples_written = 0;  
  // pcm_pool_dropped() at the last initial sync: drops since then are gaps
  uint32_t droppedAtSync = pcm_pool_dropped();
  UBaseType_t uxHighWaterMark;

  memcpy(&scSet, (playerSetting_t*)pvParameters, sizeof(playerSetting_t));

  ESP_LOGI(TAG, "started sync task");

  //  stats_init();

  initialSync = 0;
  
  //audio_hal_ctrl_codec(audio_hal_handle_t audio_hal, audio_hal_codec_mode_t mode, audio_hal_ctrl_t audio_hal_ctrl)

  audio_set_mute(true);

  buf_us = (int64_t)(scSet.buf_ms) * 1000LL;
  clientDacLatency_us = client_dac_latency_us(&scSet);
  ESP_LOGI(TAG, "output latency %lld us (client %d ms, DAC %d frames)",
           clientDacLatency_us, scSet.cDacLat_ms, CONFIG_DAC_GROUP_DELAY_FRAMES);
  dmaDescDuration_us =
              1000000LL * (int64_t)i2sDmaBufMaxLen / (int64_t)scSet.sr;
  diagDmaUs = dmaDescDuration_us * i2sDmaBufCnt;
#if !USE_SAMPLE_INSERTION
  // force adjust_apll() to set playback speed
  currentDir = 1;
  adjust_apll(0);
#endif

  audio_set_mute(false);

  // wait for early time syncs to be ready
  while (xSemaphoreTake(latencyBufFullSemaphoreHandle,
                        pdMS_TO_TICKS(PLAYER_STOP_POLL_MS)) != pdTRUE) {
    if (player_stop_requested()) {
      goto player_exit;
    }
  }
  xSemaphoreGive(latencyBufFullSemaphoreHandle);
  while (1) {
    int64_t tDiff;
    if (get_diff_to_server(&tDiff, esp_timer_get_time())==0) {
      break;
    }
    if (player_stop_requested()) {
      goto player_exit;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  while (1) {
    //ESP_LOGD(TAG, "HIGH: %u", uxTaskGetStackHighWaterMark( NULL ));
    
    // ESP_LOGW( TAG, "32b f %d b %d", heap_caps_get_free_size
    //(MALLOC_CAP_8BIT), heap_caps_get_largest_free_block (MALLOC_CAP_8BIT));
    // ESP_LOGW (TAG, "stack free: %d", uxTaskGetStackHighWaterMark(NULL));

    // check if we got changed setting available, if so we need to
    // reinitialize
    playerSetting_t __scSet;
    ret = xQueueReceive(snapcastSettingQueueHandle, &__scSet, 0);
    if (ret == pdTRUE) {

      if ((__scSet.buf_ms > 0) && (__scSet.chkInFrames > 0) &&
          (__scSet.sr > 0)) {
        buf_us = (int64_t)(__scSet.buf_ms) * 1000LL;

        clientDacLatency_us = client_dac_latency_us(&__scSet);

        if ((scSet.sr != __scSet.sr) || (scSet.bits != __scSet.bits) ||
            (scSet.ch != __scSet.ch)) {
          ESP_LOGI(TAG, "reinitializing i2s with new settings: sample rate %ld, ch %d, bits %d",
                   __scSet.sr, __scSet.ch, __scSet.bits);
          my_i2s_channel_enable(tx_chan);
          audio_set_mute(true);
          my_i2s_channel_disable(tx_chan);

          ret = player_setup_i2s(&__scSet, false);
          if (ret < 0) {
            ESP_LOGE(TAG, "player_setup_i2s failed: %d", ret);

            // A task function must not return: clean up and delete it
            goto player_exit;
          }

          dmaDescDuration_us =
              1000000LL * (int64_t)i2sDmaBufMaxLen / (int64_t)__scSet.sr;
          diagDmaUs = dmaDescDuration_us * i2sDmaBufCnt;

#if !USE_SAMPLE_INSERTION
          // force adjust_apll() to set playback speed
          currentDir = 1;
          adjust_apll(0);
#endif

          initialSync = 0;
        }

        if ((scSet.sr != __scSet.sr) || (scSet.bits != __scSet.bits) ||
            (scSet.ch != __scSet.ch) || (scSet.buf_ms != __scSet.buf_ms)) {
          ESP_LOGI(TAG,
                   "snapserver config changed, buffer %dms, chunk %d frames, "
                   "sample rate %ld, ch %d, bits %d latency %d",
                   __scSet.buf_ms, __scSet.chkInFrames, __scSet.sr, __scSet.ch,
                   __scSet.bits, __scSet.cDacLat_ms);
        }
        audio_set_mute(false);

        scSet = __scSet;  // store for next round

      }

    }

    if (chnk == NULL) {
      bool stop = false;

      chnk = player_next_chunk(&stop);
      ret = (chnk != NULL) ? pdPASS : pdFAIL;

      if (ret != pdFAIL) {
        chunkDuration_us =
            1000000LL *
            (int64_t)(chnk->totalSize / ((scSet.bits >> 3) * scSet.ch)) /
            (int64_t)scSet.sr;
        diagChunkUs = chunkDuration_us;

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
            pcm_pool_put_free(chnk);
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
              bool stop = false;
              chnk = player_next_chunk(&stop);
              if (chnk == NULL) {
                // stop request, or the stream ended before DMA was full
                goto player_exit;
              }
            }

            p_payload = chnk->payload;
            size = chnk->totalSize;

            ESP_ERROR_CHECK(
                i2s_channel_preload_data(tx_chan, p_payload, size, &written));

            // check if DMA is full at first try here
            if (written != size) {
              dmaFull = true;
            }

            size -= written;
            p_payload += written;

            if (size == 0) {
              pcm_pool_put_free(chnk);
              chnk = NULL;
            }

            if (dmaFull == true) {
              ESP_LOGI(TAG, "DMA completely loaded");

              chunkStart -=
                  (1000000LL * (int64_t)(i2sDmaBufCnt * i2sDmaBufMaxLen) /
                   (int64_t)scSet.sr);

              break;
            }
          }

          // Wait to be notified of a timer interrupt.
          xTaskNotifyWaitIndexed(0, pdFALSE,         // Don't clear bits on entry.
                          pdFALSE,         // Don't clear bits on exit.
                          &notifiedValue,  // Stores the notified value.
                          portMAX_DELAY);
          // or use simple task delay for this
          // vTaskDelay( pdMS_TO_TICKS(-age / 1000) );

          my_gptimer_stop(gptimer);

          i2s_timing_reset();
          underrunsSeen = 0;

          my_i2s_channel_enable(tx_chan);

          playback_start_time_us = esp_timer_get_time();
          samples_written = i2sDmaBufCnt * i2sDmaBufMaxLen;

          // get timer value so we can get the real age
          timer_val = (int64_t)notifiedValue;

          // get actual age after alarm
          age = (int64_t)timer_val - (-age);

          initialSync = 1;
          droppedAtSync = pcm_pool_dropped();

          // TODO: use a timer to un-mute non blocking
          vTaskDelay(pdMS_TO_TICKS(2));
          audio_set_mute(false);

          player_event(PLAYER_EVENT_RESYNC_INITIAL, 0);
          ESP_LOGI(TAG, "initial sync age: %lldus, chunk duration: %lldus", age,
                   chunkDuration_us);

          if (size == 0) {
            continue;
          }
        } else if (age >= 0) {
          if (chnk != NULL) {
            pcm_pool_put_free(chnk);
            chnk = NULL;
          }

          my_gptimer_stop(gptimer);

          player_event(PLAYER_EVENT_RESYNC_HARD1, 0);
          uint32_t suppressed;
          if (resync_log_due(&suppressed)) {
            wifi_ap_record_t ap = {0};
            wifi_get_ap_record(&ap);  // rssi 0 when not on Wi-Fi

            ESP_LOGW(TAG,
                     "RESYNCING HARD 1: age %lldus, latency %lldus, free %d, "
                     "largest block %d, rssi: %d, left in queue %d, "
                     "dropped %lu (+%lu suppressed)",
                     age, diff2Server,
                     heap_caps_get_free_size(MALLOC_CAP_32BIT),
                     heap_caps_get_largest_free_block(MALLOC_CAP_32BIT),
                     ap.rssi, pcm_pool_filled_count(),
                     pcm_pool_dropped() - droppedAtSync, suppressed);
          }

          // get count of chunks we are late for
          uint32_t c = ceil((float)age / (float)chunkDuration_us);  // round up

          // now clear all those chunks which are probably late too
          while (c--) {
            chnk = pcm_pool_get_filled(pdMS_TO_TICKS(1));
            if (chnk != NULL) {
              pcm_pool_put_free(chnk);
              chnk = NULL;
            } else {
              break;
            }
          }

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
          p_payload = chnk->payload;
          size = chnk->totalSize;
        }

        do {
            size_t framesToBytes = (scSet.ch * (scSet.bits >> 3));
#if USE_SAMPLE_INSERTION
            uint32_t sampleSizeInBytes =
                framesToBytes * INSERT_SAMPLES;

            if ((dir_insert_sample > 0) && (size >= sampleSizeInBytes)) {
              size -= sampleSizeInBytes;
              dir_insert_sample = 0;
            }
#endif
            i2s_channel_write(tx_chan, p_payload, size, &written, portMAX_DELAY);

            samples_written += (written / framesToBytes);
            size -= written;
            p_payload += written;
            chunkStart += (1000000ll * (written / framesToBytes) / scSet.sr);

#if USE_SAMPLE_INSERTION
            if (dir_insert_sample < 0) {
              if (i2s_channel_write(tx_chan, p_payload - sampleSizeInBytes, sampleSizeInBytes, &written, portMAX_DELAY) != ESP_OK) {
                ESP_LOGE(TAG, "i2s_playback_task:  I2S write error %d", 1);
              }
              else {
                samples_written += (written / framesToBytes);
                dir_insert_sample = 0;
                chunkStart += (1000000ll * (written / framesToBytes) / scSet.sr);
              }
            }
#endif
            if (size == 0) {
              pcm_pool_put_free(chnk);
              chnk = NULL;
              dir = 0;

              break;
            }
          } while (1);

          outputBufferDacTime_us = 1000000ULL * i2sDmaBufMaxLen * i2sDmaBufCnt / scSet.sr;

        if (server_now(&serverNow, &diff2Server) >= 0) {
          {
            // samples written to I2S, minus what is still queued in DMA:
            // the frame that plays now
            int64_t samples_played = samples_written - i2sDmaBufCnt * i2sDmaBufMaxLen;

            // expected play out based on timestamp
            int64_t target_play_local_us = chunkStart - diff2Server + buf_us - outputBufferDacTime_us - clientDacLatency_us;

            // when that frame actually plays: the DMA finished its last
            // descriptor at lastSentUs, after (descs - underruns) descriptors
            // of audio; the few frames in between at the nominal rate
            uint32_t descs, underruns;
            int64_t lastSentUs;
            i2s_timing_get(&descs, &underruns, &lastSentUs);
            int64_t actual_play_local_us;
            if (descs > 0) {
              int64_t framesSent = (int64_t)(descs - underruns) * i2sDmaBufMaxLen;
              actual_play_local_us = lastSentUs + ((samples_played - framesSent) * 1000000LL) / (int64_t)scSet.sr;
            } else {
              actual_play_local_us = playback_start_time_us + (samples_played * 1000000LL) / (int64_t)scSet.sr;
            }
            int64_t error_us = actual_play_local_us - target_play_local_us;

            if (underruns != underrunsSeen) {
              ESP_LOGW(TAG, "I2S underrun: %lu descriptors of silence",
                       underruns - underrunsSeen);
              underrunsSeen = underruns;
            }

            age = error_us;
          }

          int64_t shortMedian, miniMedian;

          shortMedian = MEDIANFILTER_Insert(&shortMedianFilter, age);
          miniMedian = MEDIANFILTER_Insert(&miniMedianFilter, age);

          int msgWaiting = pcm_pool_filled_count();

          // resync hard if we are getting very late / early.
          // rest gets tuned in through apll speed control or sample insertion
          if ((msgWaiting == 0) ||
              (MEDIANFILTER_isFull(&shortMedianFilter, 0) &&
               ((shortMedian > hardResyncThreshold) ||
                (shortMedian < -hardResyncThreshold)))) 
          {
            if (chnk != NULL) {
              pcm_pool_put_free(chnk);
              chnk = NULL;
            }

            player_event(PLAYER_EVENT_RESYNC_HARD2, 0);
            uint32_t suppressed;
            if (resync_log_due(&suppressed)) {
              wifi_ap_record_t ap = {0};
              wifi_get_ap_record(&ap);  // rssi 0 when not on Wi-Fi

              ESP_LOGW(TAG,
                       "RESYNCING HARD 2: age %lldus, latency %lldus, free "
                       "%d, largest block %d, %d, rssi: %d, dropped %lu "
                       "(+%lu suppressed)",
                       age, diff2Server,
                       heap_caps_get_free_size(MALLOC_CAP_32BIT),
                       heap_caps_get_largest_free_block(MALLOC_CAP_32BIT),
                       msgWaiting, ap.rssi, pcm_pool_dropped() - droppedAtSync,
                       suppressed);
            }

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
           ESP_LOGD(TAG, "%d, %lldus, %lldus, %lldus, q:%d, %lld, %lld", dir,
                   age, shortMedian, miniMedian,
                   pcm_pool_filled_count(), insertedSamplesCounter,
                   chunkDuration_us);
          // ESP_LOGI(TAG, "%d, %lldus, %lldus, %lldus, q:%d", dir,
          //         age, shortMedian, miniMedian,
          //         pcm_pool_filled_count());
          // ESP_LOGI( TAG, "8b f %d b %d",
          //       heap_caps_get_free_size(MALLOC_CAP_8BIT |
          //                         MALLOC_CAP_INTERNAL),
          //           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT |
          //                                            MALLOC_CAP_INTERNAL));
          // ESP_LOGI( TAG, "32b f %d b %d",
          //           heap_caps_get_free_size(MALLOC_CAP_32BIT |
          //                                   MALLOC_CAP_EXEC),
          //           heap_caps_get_largest_free_block(MALLOC_CAP_32BIT |
          //     MALLOC_CAP_EXEC));
        } else {
          // ESP_LOGW(TAG, "couldn't get server now");

          if (chnk != NULL) {
            pcm_pool_put_free(chnk);
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

      ESP_LOGV(TAG,
               "Couldn't get PCM chunk, recv: messages waiting %lu, "
               "diff2Server: %llds, %lld.%lldms",
               pcm_pool_filled_count(), sec, msec, usec);
      break;
    }
    if (ulTaskNotifyTakeIndexed(1, pdTRUE, 0) == pdTRUE) {
      break;
    }
  }

player_exit:
  audio_set_mute(true);
  my_i2s_channel_disable(tx_chan);
  if (tx_chan != NULL) {
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }
  if (lock_i2s != NULL) {
    lock_i2s(false, 0);
  }
  xSemaphoreTake(playerStateMux, portMAX_DELAY);

#if CONFIG_PM_ENABLE
  esp_pm_lock_release(player_pm_lock_handle);
#endif

  // chunks queued while the task was ending are flushed by the next
  // player_chunk_submit()
  if (chnk != NULL) {
    pcm_pool_put_free(chnk);
    chnk = NULL;
  }
  pcm_pool_flush();

  tg0_timer_deinit();
  playerStarted = false;
  ESP_LOGI(TAG, "stop player done");
  playerTaskHandle = NULL;
  xSemaphoreGive(playerStateMux);
  call_state_cb();
  vTaskDelete(NULL);
}


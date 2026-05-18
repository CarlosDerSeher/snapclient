#include "bt_audio_task.h"

#ifdef CONFIG_SNAPCLIENT_BT_ENABLED

#include "esp_log.h"
#include "driver/i2s_std.h"

#define TAG "BT_AUDIO_TASK"

static QueueHandle_t bluetooth_pcm_queue;
static SemaphoreHandle_t i2s_task_mutex;
static i2s_chan_handle_t tx_chan = NULL;  // I2S tx channel handler
static i2s_port_t bt_i2sNum;
static i2s_std_gpio_config_t bt_pin_config0;
static TaskHandle_t audio_task_hdl;
void (*bt_audio_set_mute)(bool, bool);
static uint16_t sr = 44100;
static uint8_t ch = 2;
static bool (*lock_i2s)(bool, TickType_t) = NULL;

static void bt_audio_task(void *pvParameters) {
  xSemaphoreTake(i2s_task_mutex, portMAX_DELAY);
  audio_chunk_t *chunk = NULL;
  char* p_payload = NULL;
  size_t size = 0, written = 0;
  bool preload = true;

  while (ulTaskNotifyTake(pdTRUE, 0) != pdTRUE) {
    if (xQueueReceive(bluetooth_pcm_queue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
      p_payload = chunk->payload;
      size = chunk->size;
      while (size > 0) {
        //print_hex(p_payload, size);
        if (preload) {
          if (i2s_channel_preload_data(tx_chan, p_payload, size, &written) != ESP_OK) {
            ESP_LOGE(TAG, "I2S write error");
            break;
          }
          if (size != written) {
            preload = false;
            size -= written;
            p_payload += written;
            ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
            ESP_LOGI(TAG, "Preload done");
          } else {
            size = 0;
          }
        } else {
          if (i2s_channel_write(tx_chan, p_payload, size, &written, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "I2S write error");
            break;
          }
          size -= written;
          p_payload += written;
        }
      }
      free_audio_chunk(chunk);
    }
  }
  bt_audio_set_mute(true, false); //mute player
  if (tx_chan) {
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
    ESP_LOGI(TAG, "tx channel disabled");
  }
  if (lock_i2s != NULL) {
    lock_i2s(false, 0);
  }
  // Clear queue first
  while (uxQueueMessagesWaiting(bluetooth_pcm_queue)) {
    if (xQueueReceive(bluetooth_pcm_queue, &chunk, pdMS_TO_TICKS(20)) != pdFAIL) {
      if (chunk != NULL) {
        free_audio_chunk(chunk);
      }
    }
    else {
      ESP_LOGE(TAG, "%s: can't get pcm chunk", __func__);
    }
  }

  // delete the queue
  vQueueDelete(bluetooth_pcm_queue);
  bluetooth_pcm_queue = NULL;
  ESP_LOGI(TAG, "stopped task");
  xSemaphoreGive(i2s_task_mutex);
  audio_task_hdl = NULL;
  vTaskDelete(NULL);
}


void bt_audio_task_init(i2s_port_t i2sN, i2s_std_gpio_config_t pin_conf, void (*set_mute)(bool, bool), bool (*lock)(bool, TickType_t)) {
  bt_i2sNum = i2sN;
  bt_pin_config0 = pin_conf;
  bt_audio_set_mute = set_mute;
  lock_i2s = lock; // can be NULL

  if (i2s_task_mutex == NULL) {
    i2s_task_mutex = xSemaphoreCreateMutex();
  }
}

QueueHandle_t* bt_audio_task_start() {
  if (xSemaphoreTake(i2s_task_mutex, 0) != pdTRUE) {
    //already running
    return NULL;
  }
  xSemaphoreGive(i2s_task_mutex);
  if (lock_i2s != NULL) {
    lock_i2s(true, portMAX_DELAY);
  }
  bluetooth_pcm_queue = xQueueCreate(2, sizeof(audio_chunk_t *));
  if (tx_chan) {
    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    tx_chan = NULL;
  }

  //one audio chunk is usually 4096 bytes -> 2 channel, 16bit: 1024 samples
  //use 2 dma buffers with 1024 entries each.
  // delay: 2*1024/sr*1e3 = 46.4 ms for 44100 kHz
  i2s_chan_config_t tx_chan_cfg = {
    .id = bt_i2sNum,
    .role = I2S_ROLE_MASTER,
    .dma_desc_num = 2,
    .dma_frame_num = 1024,
    .auto_clear = false,
  };
  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));

  i2s_std_clk_config_t i2s_clkcfg = I2S_STD_CLK_DEFAULT_CONFIG(sr);

  i2s_clkcfg.clk_src = I2S_CLK_SRC_DEFAULT;

  i2s_std_config_t tx_std_cfg = {
    .clk_cfg = i2s_clkcfg,
#if CONFIG_I2S_USE_MSB_FORMAT
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_STEREO),
#else
    .slot_cfg =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
#endif
    .gpio_cfg = bt_pin_config0,
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
  bt_audio_set_mute(false, false); //unmute player
  xTaskCreatePinnedToCore(&bt_audio_task, "bt_audio_task", 2048, NULL, 23, &audio_task_hdl, 1);
  return &bluetooth_pcm_queue;
}

void bt_audio_task_stop() {
  //send notification to task
  ESP_LOGI(TAG, "stopping task...");
  if (audio_task_hdl) {
    xTaskNotifyGive(audio_task_hdl);
  }
  // Wait until stopped
  xSemaphoreTake(i2s_task_mutex, portMAX_DELAY);
  xSemaphoreGive(i2s_task_mutex);
}

void bt_audio_set_rate(uint16_t samplerate, uint8_t channels) {
  xSemaphoreTake(i2s_task_mutex, portMAX_DELAY);
  sr = samplerate;
  ch = channels;
  xSemaphoreGive(i2s_task_mutex);
}

/**
 *
 */
int32_t allocate_audio_chunk_memory_caps(audio_chunk_t *pcmChunk,
                                       size_t bytes, uint32_t caps) {
  size_t largestFreeBlock;
  int ret = -3;
  if (caps != 0) {
    largestFreeBlock = heap_caps_get_largest_free_block(caps);
    if (largestFreeBlock >= bytes) {
      pcmChunk->payload = (char *)heap_caps_malloc(bytes, caps);
      if (pcmChunk->payload == NULL) {
        ESP_LOGD(TAG, "Failed to allocate %d bytes of %s for pcm chunk payload",
                 bytes,
                 (caps == (MALLOC_CAP_32BIT | MALLOC_CAP_EXEC)) ? ("IRAM")
                                                                : ("DRAM"));
        ret = -2;
      } else {
        pcmChunk->size = bytes;
        ret = 0;
      }
    }
  } else {
    pcmChunk->payload = (char *)malloc(bytes);
    if (pcmChunk->payload == NULL) {
      ESP_LOGE(TAG, "Failed to malloc memory for pcm chunk payload");
      ret = -2;
    } else {
      pcmChunk->size = bytes;
      ret = 0;
    }
  }
  return ret;
}

/**
 *
 */
int32_t allocate_audio_chunk_memory(audio_chunk_t **pcmChunk,
                                  size_t bytes) {
  int ret = -3;

  *pcmChunk = (audio_chunk_t *)calloc(1, sizeof(audio_chunk_t));
  if (*pcmChunk == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");

    return -2;
  }

#if CONFIG_SPIRAM && CONFIG_SPIRAM_BOOT_INIT
  ret = allocate_audio_chunk_memory_caps(*pcmChunk, bytes,
                                       MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
#elif CONFIG_SPIRAM
  ret = allocate_audio_chunk_memory_caps(*pcmChunk, bytes, 0);
#else
  // Reduced from 50 to 25 to minimize delay during memory pressure
  // if allocation fails we try again every 1ms for max. x ms waiting for
  // chunks to finish playback
  uint32_t x = 25;
  for (int i = 0; i < x; i++) {
    // Try regular heap first to reduce IRAM pressure from Bluetooth
    ret = allocate_audio_chunk_memory_caps(*pcmChunk, bytes, MALLOC_CAP_8BIT);
    if (ret < 0) {
      // Fallback to IRAM only if regular heap fails
      ret = allocate_audio_chunk_memory_caps(*pcmChunk, bytes,
                                           MALLOC_CAP_32BIT | MALLOC_CAP_EXEC);
    }
    if (ret < 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      break;
    }
  }
#endif

  if (ret < 0) {
    ESP_LOGD(TAG,
             "couldn't get memory to insert chunk, inserting an chunk "
             "containing just 0");
    ESP_LOGD(
        TAG, "%d, %d, %d, %d", heap_caps_get_free_size(MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_32BIT | MALLOC_CAP_EXEC),
        heap_caps_get_largest_free_block(MALLOC_CAP_32BIT | MALLOC_CAP_EXEC));
    free(*pcmChunk);
    *pcmChunk = NULL;
    ret = -1;
  }

  return ret;
}

/**
 *
 */
void free_audio_chunk(audio_chunk_t *pcmChunk) {
  if (pcmChunk == NULL) {
    return;
  }

  // free all pcmChunks recursive

  if (pcmChunk->payload != NULL) {
    free(pcmChunk->payload);
    pcmChunk->payload = NULL;
  }

  free(pcmChunk);
  pcmChunk = NULL;

  return;
}
#endif

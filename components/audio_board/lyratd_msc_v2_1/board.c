/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in
 * which case, it is free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the
 * Software without restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#include "board.h"

#include "audio_mem.h"
#include "esp_log.h"
#include "led_bar_is31x.h"
#include "periph_adc_button.h"
#include "periph_sdcard.h"

static const char *TAG = "AUDIO_BOARD";

static audio_board_handle_t board_handle = 0;

audio_board_handle_t audio_board_init(void) {
  if (board_handle) {
    ESP_LOGW(TAG, "The board has already been initialized!");
    return board_handle;
  }
  board_handle =
      (audio_board_handle_t)audio_calloc(1, sizeof(struct audio_board_handle));
  AUDIO_MEM_CHECK(TAG, board_handle, return NULL);
  board_handle->audio_hal = audio_board_codec_init();

  return board_handle;
}

audio_hal_handle_t audio_board_codec_init(void) {
  audio_hal_codec_config_t audio_codec_cfg = AUDIO_CODEC_DEFAULT_CONFIG();
  audio_hal_handle_t codec_hal =
      audio_hal_init(&audio_codec_cfg, &AUDIO_CODEC_ZL38063_DEFAULT_HANDLE);
  AUDIO_NULL_CHECK(TAG, codec_hal, return NULL);
  return codec_hal;
}

display_service_handle_t audio_board_led_init(void) {
  esp_periph_handle_t led = led_bar_is31x_init();
  AUDIO_NULL_CHECK(TAG, led, return NULL);
  display_service_config_t display = {
      .based_cfg =
          {
              .task_stack = 0,
              .task_prio = 0,
              .task_core = 0,
              .task_func = NULL,
              .service_start = NULL,
              .service_stop = NULL,
              .service_destroy = NULL,
              .service_ioctl = led_bar_is31x_pattern,
              .service_name = "DISPLAY_serv",
              .user_data = NULL,
          },
      .instance = led,
  };

  return display_service_create(&display);
}

esp_err_t audio_board_key_init(esp_periph_set_handle_t set) {
  periph_adc_button_cfg_t adc_btn_cfg = PERIPH_ADC_BUTTON_DEFAULT_CONFIG();
  adc_arr_t adc_btn_tag = ADC_DEFAULT_ARR();
  adc_btn_cfg.arr = &adc_btn_tag;
  adc_btn_cfg.arr_size = 1;
  esp_periph_handle_t adc_btn_handle = periph_adc_button_init(&adc_btn_cfg);
  AUDIO_NULL_CHECK(TAG, adc_btn_handle, return ESP_ERR_ADF_MEMORY_LACK);
  return esp_periph_start(set, adc_btn_handle);
}

esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set,
                                  periph_sdcard_mode_t mode) {
  if (mode >= SD_MODE_8_LINE) {
    ESP_LOGE(TAG, "Please select the correct sd mode!, current mode is %d",
             mode);
    return ESP_FAIL;
  }
  periph_sdcard_cfg_t sdcard_cfg = {
      .root = "/sdcard",
      .card_detect_pin = get_sdcard_intr_gpio(),  // GPIO_NUM_34
      .mode = mode};
  esp_periph_handle_t sdcard_handle = periph_sdcard_init(&sdcard_cfg);
  esp_err_t ret = esp_periph_start(set, sdcard_handle);
  int retry_time = 5;
  bool mount_flag = false;
  while (retry_time--) {
    if (periph_sdcard_is_mounted(sdcard_handle)) {
      mount_flag = true;
      break;
    } else {
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }
  }
  if (mount_flag == false) {
    ESP_LOGE(TAG, "Sdcard mount failed");
    return ESP_FAIL;
  }
  return ret;
}

audio_board_handle_t audio_board_get_handle(void) { return board_handle; }

esp_err_t audio_board_deinit(audio_board_handle_t audio_board) {
  esp_err_t ret = ESP_OK;
  ret = audio_hal_deinit(audio_board->audio_hal);
  audio_free(audio_board);
  board_handle = NULL;
  return ret;
}
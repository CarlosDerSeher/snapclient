/*
 * Princeton Technology PT8211 audio hal
 *
 * Mostly stubs (no I2C or volume control)
 * Configuration of mute/unmute gpio in init (for external amplifier)
 */

#include "pt8211.h"

#include <driver/gpio.h>

#include "board.h"
#include "esp_log.h"

#ifndef CONFIG_PT8211_MUTE_ACTIVE_LOW
#define CONFIG_PT8211_MUTE_ACTIVE_LOW 0
#endif

static const char *TAG = "PT8211";

esp_err_t pt8211_ctrl(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state);
esp_err_t pt8211_config_iface(audio_hal_codec_mode_t mode,
                              audio_hal_codec_i2s_iface_t *iface);

audio_hal_func_t AUDIO_CODEC_PT8211_DEFAULT_HANDLE = {
    .audio_codec_initialize = pt8211_init,
    .audio_codec_deinitialize = pt8211_deinit,
    .audio_codec_ctrl = pt8211_ctrl,
    .audio_codec_config_iface = pt8211_config_iface,
    .audio_codec_set_mute = pt8211_set_mute,
    .audio_codec_set_volume = pt8211_set_volume,
    .audio_codec_get_volume = pt8211_get_volume,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

esp_err_t pt8211_init(audio_hal_codec_config_t *codec_cfg) {
  esp_err_t ret = ESP_OK;

#if CONFIG_PT8211_MUTE_PIN != -1
  gpio_config_t io_conf;

  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << CONFIG_PT8211_MUTE_PIN);
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;

  ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Mute gpio config failed for pin %d", CONFIG_PT8211_MUTE_PIN);
  } else {
    gpio_set_level(CONFIG_PT8211_MUTE_PIN, 0);
    ESP_LOGD(TAG, "Setup mute output %d\n", CONFIG_PT8211_MUTE_PIN);
  }
#else
  ESP_LOGD(TAG, "Mute gpio not specified\n");
#endif

  return ret;
}

esp_err_t pt8211_set_volume(int vol) { return ESP_OK; }

esp_err_t pt8211_get_volume(int *value) { return ESP_OK; }

esp_err_t pt8211_set_mute(bool enable) {
  esp_err_t ret = ESP_OK;

#if CONFIG_PT8211_MUTE_PIN != -1
  ret = gpio_set_level(CONFIG_PT8211_MUTE_PIN,
                       enable ^ CONFIG_PT8211_MUTE_ACTIVE_LOW);
#endif

  return ret;
}

esp_err_t pt8211_get_mute(bool *enabled) { return ESP_OK; }

esp_err_t pt8211_deinit(void) { return gpio_reset_pin(CONFIG_PT8211_MUTE_PIN); }

esp_err_t pt8211_ctrl(audio_hal_codec_mode_t mode,
                      audio_hal_ctrl_t ctrl_state) {
  return ESP_OK;
}

esp_err_t pt8211_config_iface(audio_hal_codec_mode_t mode,
                              audio_hal_codec_i2s_iface_t *iface) {
  return ESP_OK;
}

/*
 * Analog Devices MAX98357 audio hal
 *
 * Mostly stubs (no I2C or volume control)
 * Configuration of mute/unmute gpio in init (connected to XSMT)
 */

#include "max98357.h"

#include <driver/gpio.h>

#include "board.h"
#include "esp_log.h"

static const char *TAG = "MAX98357";

#define PCM5102A_ASSERT(a, format, b, ...) \
  if ((a) != 0) {                          \
    ESP_LOGE(TAG, format, ##__VA_ARGS__);  \
    return b;                              \
  }

esp_err_t max98357_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state);
esp_err_t max98357_config_iface(audio_hal_codec_mode_t mode,
                                audio_hal_codec_i2s_iface_t *iface);

audio_hal_func_t AUDIO_CODEC_MAX98357_DEFAULT_HANDLE = {
    .audio_codec_initialize = max98357_init,
    .audio_codec_deinitialize = max98357_deinit,
    .audio_codec_ctrl = max98357_ctrl,
    .audio_codec_config_iface = max98357_config_iface,
    .audio_codec_set_mute = max98357_set_mute,
    .audio_codec_set_volume = max98357_set_volume,
    .audio_codec_get_volume = max98357_get_volume,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

esp_err_t max98357_init(audio_hal_codec_config_t *codec_cfg) {
  esp_err_t ret;

  gpio_config_t io_conf;

  io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << CONFIG_MAX98357_MUTE_PIN);
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;

  ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Mute gpio config failed for pin %d",
             CONFIG_MAX98357_MUTE_PIN);
  } else {
    gpio_set_level(CONFIG_MAX98357_MUTE_PIN, 0);
    ESP_LOGD(TAG, "Setup mute (SD) output %d\n", CONFIG_MAX98357_MUTE_PIN);
  }

  return ret;
}

esp_err_t max98357_set_volume(int vol) { return ESP_OK; }

esp_err_t max98357_get_volume(int *value) { return ESP_OK; }

esp_err_t max98357_set_mute(bool enable) {
  return gpio_set_level(CONFIG_MAX98357_MUTE_PIN, enable ? 0 : 1);
}

esp_err_t max98357_get_mute(bool *enabled) { return ESP_OK; }

esp_err_t max98357_deinit(void) {
  return gpio_reset_pin(CONFIG_MAX98357_MUTE_PIN);
}

esp_err_t max98357_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state) {
  return ESP_OK;
}

esp_err_t max98357_config_iface(audio_hal_codec_mode_t mode,
                                audio_hal_codec_i2s_iface_t *iface) {
  return ESP_OK;
}

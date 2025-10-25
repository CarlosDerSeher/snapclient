/*
 * TI PCM5102A audio hal
 *
 * Mostly stubs (no I2C or volume control)
 * Configuration of mute/unmute gpio in init (connected to XSMT)
 */

#include "pcm5102a.h"

#include <driver/gpio.h>

#include "board.h"
#include "esp_log.h"

static const char *TAG = "PCM5102A";

#define PCM5102A_ASSERT(a, format, b, ...) \
  if ((a) != 0) {                          \
    ESP_LOGE(TAG, format, ##__VA_ARGS__);  \
    return b;                              \
  }

esp_err_t pcm5102a_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state);
esp_err_t pcm5102a_config_iface(audio_hal_codec_mode_t mode,
                                audio_hal_codec_i2s_iface_t *iface);

audio_hal_func_t AUDIO_CODEC_PCM5102A_DEFAULT_HANDLE = {
    .audio_codec_initialize = pcm5102a_init,
    .audio_codec_deinitialize = pcm5102a_deinit,
    .audio_codec_ctrl = pcm5102a_ctrl,
    .audio_codec_config_iface = pcm5102a_config_iface,
    .audio_codec_set_mute = pcm5102a_set_mute,
    .audio_codec_set_volume = pcm5102a_set_volume,
    .audio_codec_get_volume = pcm5102a_get_volume,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

esp_err_t pcm5102a_init(audio_hal_codec_config_t *codec_cfg) {
  esp_err_t ret;

  gpio_config_t io_conf;

  if (CONFIG_PCM5102A_MUTE_PIN < 0) {
    return ESP_OK;
  }

  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << CONFIG_PCM5102A_MUTE_PIN);
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;

  ret = gpio_config(&io_conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Mute gpio config failed for pin %d",
             CONFIG_PCM5102A_MUTE_PIN);
  } else {
    gpio_set_level(CONFIG_PCM5102A_MUTE_PIN, 0);
    ESP_LOGD(TAG, "Setup mute (XMT) output %d\n", CONFIG_PCM5102A_MUTE_PIN);
  }

  return ret;
}

esp_err_t pcm5102a_set_volume(int vol) { return ESP_OK; }

esp_err_t pcm5102a_get_volume(int *value) { return ESP_OK; }

esp_err_t pcm5102a_set_mute(bool enable) {
  if (CONFIG_PCM5102A_MUTE_PIN < 0) {
    return ESP_OK;
  }

  return gpio_set_level(CONFIG_PCM5102A_MUTE_PIN, enable ? 0 : 1);
}

esp_err_t pcm5102a_get_mute(bool *enabled) { return ESP_OK; }

esp_err_t pcm5102a_deinit(void) {
  if (CONFIG_PCM5102A_MUTE_PIN < 0) {
    return ESP_OK;
  }

  return gpio_reset_pin(CONFIG_PCM5102A_MUTE_PIN);
}

esp_err_t pcm5102a_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state) {
  return ESP_OK;
}

esp_err_t pcm5102a_config_iface(audio_hal_codec_mode_t mode,
                                audio_hal_codec_i2s_iface_t *iface) {
  return ESP_OK;
}

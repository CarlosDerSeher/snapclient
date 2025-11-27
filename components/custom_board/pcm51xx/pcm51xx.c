/*
 * MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 * Copyright (c) 2021 David Douard <david.douard@sdfa3.org>
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

#include "pcm51xx.h"

#include "board.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "pcm51xx_reg_cfg.h"
#include "driver/gpio.h"

static const char *TAG = "PCM51XX";

// Volume range in percentage
#define PCM51XX_VOLUME_MAX 100
#define PCM51XX_VOLUME_MIN 0

// PCM51XX register values
#define PCM51XX_REG_VAL_0DB    0x30  // 48 decimal = 0dB
#define PCM51XX_REG_VAL_MUTE   0xFF  // 255 decimal = mute

// GPIO mute pin configuration
#ifdef CONFIG_PCM51XX_MUTE_PIN
#define PCM51XX_MUTE_PIN CONFIG_PCM51XX_MUTE_PIN
#else
#define PCM51XX_MUTE_PIN 255
#endif

#define PCM51XX_MUTE_PIN_VALID(pin) ((pin) != 255)

#define PCM51XX_ASSERT(a, format, b, ...) \
  if ((a) != 0) {                         \
    ESP_LOGE(TAG, format, ##__VA_ARGS__); \
    return b;                             \
  }

esp_err_t pcm51xx_ctrl(audio_hal_codec_mode_t mode,
                       audio_hal_ctrl_t ctrl_state);
esp_err_t pcm51xx_config_iface(audio_hal_codec_mode_t mode,
                               audio_hal_codec_i2s_iface_t *iface);
static i2c_bus_handle_t i2c_handler;
// CONFIG_DAC_I2C_ADDR is 7-bit address, but i2c_bus functions expect 8-bit (shifted) address
static const int pcm51xx_addr = (CONFIG_DAC_I2C_ADDR << 1);

// State tracking
static struct {
  int volume_percent;     // Current volume in percentage (0-100)
  bool is_muted;          // Current mute state
} pcm51xx_state = {
  .volume_percent = 100,  // Default to 100%
  .is_muted = true,       // Default to muted
};

/*
 * i2c default configuration
 */
static i2c_config_t i2c_cfg = {
    .mode = I2C_MODE_MASTER,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 100000,
};

/*
 * Operate function
 */
audio_hal_func_t AUDIO_CODEC_PCM51XX_DEFAULT_HANDLE = {
    .audio_codec_initialize = pcm51xx_init,
    .audio_codec_deinitialize = pcm51xx_deinit,
    .audio_codec_ctrl = pcm51xx_ctrl,
    .audio_codec_config_iface = pcm51xx_config_iface,
    .audio_codec_set_mute = pcm51xx_set_mute,
    .audio_codec_set_volume = pcm51xx_set_volume,
    .audio_codec_get_volume = pcm51xx_get_volume,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

static esp_err_t pcm51xx_transmit_registers(const pcm51xx_cfg_reg_t *conf_buf,
                                            int size) {
  ESP_LOGD(TAG, "%s: size=%d", __func__, size);
  int i = 0;
  esp_err_t ret = ESP_OK;
  while (i < size) {
    ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr,
                              (unsigned char *)(&conf_buf[i].offset), 1,
                              (unsigned char *)(&conf_buf[i].value), 1);
    i++;
  }
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Fail to load configuration to pcm51xx");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "%s:  write %d reg done", __FUNCTION__, i);
  return ret;
}

esp_err_t pcm51xx_init(audio_hal_codec_config_t *codec_cfg) {
  ESP_LOGD(TAG, "%s: codec_cfg=%p", __func__, codec_cfg);
  esp_err_t ret = ESP_OK;

  ret = get_i2c_pins(I2C_NUM_0, &i2c_cfg);
  ESP_LOGI(TAG, "PCM51XX I2C pins set: SDA=%d, SCL=%d", i2c_cfg.sda_io_num,
           i2c_cfg.scl_io_num);
  i2c_handler = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
  if (i2c_handler == NULL) {
    ESP_LOGW(TAG, "failed to create i2c bus handler\n");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Using pcm51xx chip at address 0x%x", pcm51xx_addr);

  // Initialize GPIO mute pin if configured
  if (PCM51XX_MUTE_PIN_VALID(PCM51XX_MUTE_PIN)) {
    gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << PCM51XX_MUTE_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
      // Initialize to muted state (LOW)
      gpio_set_level(PCM51XX_MUTE_PIN, 0);
      ESP_LOGI(TAG, "PCM51XX GPIO mute pin %d configured", PCM51XX_MUTE_PIN);
    } else {
      ESP_LOGW(TAG, "Failed to configure GPIO mute pin %d", PCM51XX_MUTE_PIN);
    }
  } else {
    ESP_LOGI(TAG, "PCM51XX GPIO mute pin disabled (using register control only)");
  }

  PCM51XX_ASSERT(ret, "Fail to detect pcm51xx PA", ESP_FAIL);
  ret |= pcm51xx_transmit_registers(
      pcm51xx_init_seq, sizeof(pcm51xx_init_seq) / sizeof(pcm51xx_init_seq[0]));

  PCM51XX_ASSERT(ret, "Fail to iniitialize pcm51xx PA", ESP_FAIL);
  return ret;
}

esp_err_t pcm51xx_set_volume(int vol) {
  ESP_LOGD(TAG, "%s: vol=%d%%", __func__, vol);
  // vol is given as percentage (0-100%)
  // Input range: 0% (min) to 100% (max/0dB)
  // 
  // PCM51XX register mapping (1/2dB steps):
  // 0x30 (48 decimal):  0dB    <- 100%
  // 0xFF (255):        -inf (mute) <- 0%
  //
  // Inverted mapping: higher percentage = higher register value (less attenuation)

  // Clamp input to valid range
  if (vol < 0) {
    vol = 0;
  }
  if (vol > 100) {
    vol = 100;
  }

  uint8_t register_value;
  
  if (vol == 0) {
    // 0% = mute
    register_value = 0xFF;
  } else {
    // Map 1-100% to register values 0xFE-0x30
    // Linear mapping: 100% -> 0x30 (48), 1% -> 0xFE (254)
    // Formula: reg_val = 0x30 + (100 - vol) * (0xFE - 0x30) / 99
    //        = 48 + (100 - vol) * 206 / 99
    register_value = 0x30 + ((100 - vol) * 206) / 99;
  }

  uint8_t cmd[2] = {0, 0};
  esp_err_t ret = ESP_OK;

  cmd[1] = register_value;

  cmd[0] = PCM51XX_REG_VOL_L;
  ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &cmd[0], 1, &cmd[1], 1);
  cmd[0] = PCM51XX_REG_VOL_R;
  ret |= i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &cmd[0], 1, &cmd[1], 1);
  
  if (ret == ESP_OK) {
    // Store the volume in state for later retrieval
    pcm51xx_state.volume_percent = vol;
  }
  
  ESP_LOGD(TAG, "Volume set to %d%% (register: 0x%02x)", vol, register_value);
  return ret;
}

esp_err_t pcm51xx_get_volume(int *value) {
  ESP_LOGD(TAG, "%s: value=%p", __func__, value);
  
  if (value == NULL) {
    ESP_LOGE(TAG, "Null pointer provided for volume value");
    return ESP_FAIL;
  }
  
  // Return the stored volume percentage
  *value = pcm51xx_state.volume_percent;
  ESP_LOGD(TAG, "Volume is %d%%", *value);
  
  return ESP_OK;
}

esp_err_t pcm51xx_set_mute(bool enable) {
  ESP_LOGD(TAG, "%s: enable=%d", __func__, enable);
  esp_err_t ret = ESP_OK;
  
  // Control I2C register-based mute
  uint8_t cmd[2] = {PCM51XX_REG_MUTE, 0x00};
  ret |= i2c_bus_read_bytes(i2c_handler, pcm51xx_addr, &cmd[0], 1, &cmd[1], 1);

  if (enable) {
    cmd[1] |= 0x11;
  } else {
    cmd[1] &= (~0x11);
  }
  ret |= i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &cmd[0], 1, &cmd[1], 1);

  // Control GPIO mute pin if configured
  if (PCM51XX_MUTE_PIN_VALID(PCM51XX_MUTE_PIN)) {
    // Set pin HIGH for unmute, LOW for mute
    gpio_set_level(PCM51XX_MUTE_PIN, enable ? 0 : 1);
    ESP_LOGD(TAG, "GPIO mute pin %d set to %d", PCM51XX_MUTE_PIN, enable ? 0 : 1);
  }

  PCM51XX_ASSERT(ret, "Fail to set mute", ESP_FAIL);
  
  // Store mute state
  pcm51xx_state.is_muted = enable;
  
  ESP_LOGI(TAG, "Mute %s (register + GPIO)", enable ? "enabled" : "disabled");
  return ret;
}

esp_err_t pcm51xx_get_mute(bool *enabled) {
  ESP_LOGD(TAG, "%s: enabled=%p", __func__, enabled);
  
  if (enabled == NULL) {
    ESP_LOGE(TAG, "Null pointer provided for mute value");
    return ESP_FAIL;
  }
  
  // Return the stored mute state
  *enabled = pcm51xx_state.is_muted;
  ESP_LOGI(TAG, "Get mute value: %s", *enabled ? "muted" : "unmuted");
  
  return ESP_OK;
}

esp_err_t pcm51xx_deinit(void) {
  ESP_LOGD(TAG, "%s", __func__);
  // TODO
  return ESP_OK;
}

esp_err_t pcm51xx_ctrl(audio_hal_codec_mode_t mode,
                       audio_hal_ctrl_t ctrl_state) {
  ESP_LOGD(TAG, "%s: mode=%d, ctrl_state=%d", __func__, mode, ctrl_state);
  // TODO
  return ESP_OK;
}

esp_err_t pcm51xx_config_iface(audio_hal_codec_mode_t mode,
                               audio_hal_codec_i2s_iface_t *iface) {
  ESP_LOGD(TAG, "%s: mode=%d, iface=%p", __func__, mode, iface);
  // TODO
  return ESP_OK;
}

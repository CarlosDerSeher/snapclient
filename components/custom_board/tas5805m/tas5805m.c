/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in
 * which case, it is free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the
 * Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "tas5805m.h"

#include "esp_log.h"
#include "i2c_bus.h"
#include "tas5805m_reg_cfg.h"

static const char *TAG = "TAS5805M";

/* Default I2C config */

static i2c_config_t i2c_cfg = {
    .mode = I2C_MODE_MASTER,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = I2C_MASTER_FREQ_HZ,
};

/*
 * Operate fuction of PA
 */
audio_hal_func_t AUDIO_CODEC_TAS5805M_DEFAULT_HANDLE = {
    .audio_codec_initialize = tas5805m_init,
    .audio_codec_deinitialize = tas5805m_deinit,
    .audio_codec_ctrl = tas5805m_ctrl,
    .audio_codec_config_iface = tas5805m_config_iface,
    .audio_codec_set_mute = tas5805m_set_mute,
    .audio_codec_set_volume = tas5805m_set_volume,
    .audio_codec_get_volume = tas5805m_get_volume,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

/* Init the I2C Driver */

void i2c_master_init() {
  int i2c_master_port = I2C_MASTER_NUM;

  ESP_ERROR_CHECK(get_i2c_pins(I2C_NUM_0, &i2c_cfg));

  ESP_ERROR_CHECK(i2c_param_config(i2c_master_port, &i2c_cfg));

  ESP_ERROR_CHECK(i2c_driver_install(i2c_master_port, i2c_cfg.mode,
                                     I2C_MASTER_RX_BUF_DISABLE,
                                     I2C_MASTER_TX_BUF_DISABLE, 0));
}

/* Helper Functions */

// Reading of TAS5805M-Register

esp_err_t tas5805m_read_byte(uint8_t register_name, uint8_t *data) {
  int ret;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, TAS5805M_ADDRESS << 1 | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, register_name, ACK_CHECK_EN);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(I2C_TAS5805M_MASTER_NUM, cmd,
                             1000 / portTICK_PERIOD_MS);
  i2c_cmd_link_delete(cmd);

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "I2C ERROR");
  }

  vTaskDelay(1 / portTICK_PERIOD_MS);
  cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, TAS5805M_ADDRESS << 1 | READ_BIT, ACK_CHECK_EN);
  i2c_master_read_byte(cmd, data, NACK_VAL);
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(I2C_TAS5805M_MASTER_NUM, cmd,
                             1000 / portTICK_PERIOD_MS);
  i2c_cmd_link_delete(cmd);

  return ret;
}

// Writing of TAS5805M-Register

esp_err_t tas5805m_write_byte(uint8_t register_name, uint8_t value) {
  int ret = 0;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, TAS5805M_ADDRESS << 1 | WRITE_BIT, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, register_name, ACK_CHECK_EN);
  i2c_master_write_byte(cmd, value, ACK_CHECK_EN);
  i2c_master_stop(cmd);

  ret = i2c_master_cmd_begin(I2C_TAS5805M_MASTER_NUM, cmd,
                             1000 / portTICK_PERIOD_MS);

  // Check if ret is OK
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Fehler bei der I2C-Übertragung: %s", esp_err_to_name(ret));
  }

  i2c_cmd_link_delete(cmd);

  return ret;
}

// Inits the TAS5805M change Settings in Menuconfig to enable Bridge-Mode

esp_err_t tas5805m_init() {
  int ret = 0;
  // Init the I2C-Driver
  i2c_master_init();
  /* Register the PDN pin as output and write 1 to enable the TAS chip */
  /* TAS5805M.INIT() */
  gpio_config_t io_conf;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = TAS5805M_GPIO_PDN_MASK;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_LOGW(TAG, "Power down pin: %d", TAS5805M_GPIO_PDN);
  gpio_config(&io_conf);
  gpio_set_level(TAS5805M_GPIO_PDN, 0);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  gpio_set_level(TAS5805M_GPIO_PDN, 1);
  vTaskDelay(10 / portTICK_PERIOD_MS);

  /* TAS5805M.Begin()*/

  ESP_LOGW(TAG, "Setting to HI Z");

  ESP_ERROR_CHECK(tas5805m_write_byte(TAS5805M_DEVICE_CTRL_2_REGISTER, 0x02));
  vTaskDelay(10 / portTICK_PERIOD_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "TAS5805M_DEVICE_CTRL_2_REGISTER, 0x02 FAILED!!!");
    return ret;
  }

  ESP_LOGW(TAG, "Setting to PLAY");

  ret = tas5805m_write_byte(TAS5805M_DEVICE_CTRL_2_REGISTER, 0x03);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "TAS5805M_DEVICE_CTRL_2_REGISTER, 0x03 FAILED!!");
    return ret;
  }

  // Check if Bridge-Mode is enabled
#ifdef CONFIG_DAC_BRIDGE_MODE
  uint8_t value = 0;
  ret = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, &value);
  if (ret != ESP_OK) return ret;
  value = 0b100;

  ret = tas5805m_write_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, value);
  if (ret != ESP_OK) return ret;
#endif

  return ret;
}

// Setting the Volume

esp_err_t tas5805m_set_volume(int vol) {
  int vol_idx = 0;  // Temp-Variable

  /* Checking if Volume is bigger or smaller than the max values */
  if (vol < TAS5805M_VOLUME_MIN) {
    vol = TAS5805M_VOLUME_MIN;
  }
  if (vol > TAS5805M_VOLUME_MAX) {
    vol = TAS5805M_VOLUME_MAX;
  }
  /* Mapping the Values from 0-100 to 254-0 */
  vol_idx = vol / 5;
  /* Updating the global volume Variable */
  currentVolume = vol_idx;
  /* Writing the Volume to the Register*/
  return tas5805m_write_byte(TAS5805M_DIG_VOL_CTRL_REGISTER,
                             tas5805m_volume[vol_idx]);
}

esp_err_t tas5805m_get_volume(int *vol) {
  esp_err_t ret = ESP_OK;
  uint8_t rxbuf = 0;
  ret = tas5805m_read_byte(TAS5805M_DIG_VOL_CTRL_REGISTER, &rxbuf);
  int i;
  for (i = 0; i < sizeof(tas5805m_volume); i++) {
    if (rxbuf >= tas5805m_volume[i]) break;
  }
  /* Updating the global volume Variable */
  currentVolume = i;
  ESP_LOGI(TAG, "Volume is %d", i * 5);
  *vol = 5 * i;  // Converting it to percent
  return ret;
}

esp_err_t tas5805m_deinit(void) {
  // TODO
  return ESP_OK;
}

esp_err_t tas5805m_set_mute(bool enable) {
  if (enable == true) {
    // Set the Volume to 255 to enable the MUTE
    return tas5805m_write_byte(TAS5805M_DIG_VOL_CTRL_REGISTER,
                               TAS5805M_VOLUME_MUTE);
  } else {
    return tas5805m_write_byte(
        TAS5805M_DIG_VOL_CTRL_REGISTER,
        tas5805m_volume[currentVolume]);  // Restore Volume to its old value
  }
  return ESP_OK;
}

esp_err_t tas5805m_get_mute(bool *enabled) {
  int currentVolume;
  if (tas5805m_get_volume(&currentVolume) != ESP_OK) {
    ESP_LOGW(TAG, "Cant get volume in get-Mute-Function");
  }
  if (currentVolume == TAS5805M_VOLUME_MUTE) {
    *enabled = true;
  } else {
    *enabled = false;
  }
  return ESP_OK;
}

esp_err_t tas5805m_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state) {
  // TODO
  return ESP_OK;
}

esp_err_t tas5805m_config_iface(audio_hal_codec_mode_t mode,
                                audio_hal_codec_i2s_iface_t *iface) {
  // TODO
  return ESP_OK;
}

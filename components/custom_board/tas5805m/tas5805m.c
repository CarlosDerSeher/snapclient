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

// State of TAS5805M (internal to this module)
static TAS5805_STATE tas5805m_state = {
  .volume = 0,
  .state = TAS5805M_CTRL_PLAY,
};

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
    ESP_LOGW(TAG, "%s: I2C ERROR", __func__);
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
  ESP_LOGD(TAG, "%s: Read 0x%02x from register 0x%02x", __func__, *data, register_name);
  return ret;
}

// Writing of TAS5805M-Register
esp_err_t tas5805m_write_byte(uint8_t register_name, uint8_t value) {
  int ret = 0;
  ESP_LOGV(TAG, "%s: Writing 0x%02x to register 0x%02x", __func__, value, register_name);

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
    ESP_LOGE(TAG, "%s: Error communicating over I2C: %s", __func__, esp_err_to_name(ret));
  }

  i2c_cmd_link_delete(cmd);

  return ret;
}

// Inits the TAS5805M change Settings in Menuconfig to enable Bridge-Mode
esp_err_t tas5805m_init() {
  ESP_LOGD(TAG, "%s: Initializing TAS5805M", __func__);
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
  ESP_LOGI(TAG, "%s: Triggering power down pin: %d", __func__, TAS5805M_GPIO_PDN);
  gpio_config(&io_conf);
  gpio_set_level(TAS5805M_GPIO_PDN, 0);
  vTaskDelay(10 / portTICK_PERIOD_MS);
  gpio_set_level(TAS5805M_GPIO_PDN, 1);
  vTaskDelay(10 / portTICK_PERIOD_MS);

  ESP_LOGW(TAG, "%s: Setting to HI Z", __func__);

  ESP_ERROR_CHECK(tas5805m_set_state(TAS5805M_CTRL_HI_Z));
  vTaskDelay(10 / portTICK_PERIOD_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "%s: Set DAC state failed", __func__);
    return ret;
  }

  ESP_LOGW(TAG, "%s: Setting to PLAY (muted)", __func__);

  ESP_ERROR_CHECK(tas5805m_set_state(TAS5805M_CTRL_MUTE | TAS5805M_CTRL_PLAY));
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "%s: Set DAC state failed", __func__);
    return ret;
  }

  // Check if Bridge-Mode is enabled
#if defined(CONFIG_DAC_BRIDGE_MODE_MONO) || defined(CONFIG_DAC_BRIDGE_MODE_LEFT) || defined(CONFIG_DAC_BRIDGE_MODE_RIGHT)
  ESP_LOGD(TAG, "%s: Setting Bridge-Mode", __func__);

  // enable bridge mode
  ret = tas5805m_write_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, 0x04);
  
  // Mixer config
  ret |= tas5805m_write_byte(0x0, 0x0);
  ret |= tas5805m_write_byte(0x7f, 0x8c);
  ret |= tas5805m_write_byte(0x0, 0x29);

  #if defined(CONFIG_DAC_BRIDGE_MODE_MONO)
  ESP_LOGI(TAG, "%s: Defining Bridge-Mode to Mono", __func__);
  // Left mixer input to left ouput (-6 dB)
  ret |= tas5805m_write_byte(0x18, 0x00);
  ret |= tas5805m_write_byte(0x19, 0x40);
  ret |= tas5805m_write_byte(0x1a, 0x26);
  ret |= tas5805m_write_byte(0x1b, 0xe7);

  // Right mixer input to left ouput (-6 dB)
  ret |= tas5805m_write_byte(0x1c, 0x00);
  ret |= tas5805m_write_byte(0x1d, 0x40);
  ret |= tas5805m_write_byte(0x1e, 0x26);
  ret |= tas5805m_write_byte(0x1f, 0xe7);

  #elif defined(CONFIG_DAC_BRIDGE_MODE_LEFT)
  ESP_LOGI(TAG, "%s: Defining Bridge-Mode to Left", __func__);
  // Left mixer input to left ouput (0 dB)
  ret |= tas5805m_write_byte(0x18, 0x00);
  ret |= tas5805m_write_byte(0x19, 0x80);
  ret |= tas5805m_write_byte(0x1a, 0x00);
  ret |= tas5805m_write_byte(0x1b, 0x00);

  // Right mixer input to left ouput (-110 dB)
  ret |= tas5805m_write_byte(0x1c, 0x00);
  ret |= tas5805m_write_byte(0x1d, 0x00);
  ret |= tas5805m_write_byte(0x1e, 0x00);
  ret |= tas5805m_write_byte(0x1f, 0x00);

  #elif defined(CONFIG_DAC_BRIDGE_MODE_RIGHT)
  ESP_LOGI(TAG, "%s: Defining Bridge-Mode to Right", __func__);
  // Left mixer input to left ouput (-110 dB)
  ret |= tas5805m_write_byte(0x18, 0x00);
  ret |= tas5805m_write_byte(0x19, 0x00);
  ret |= tas5805m_write_byte(0x1a, 0x00);
  ret |= tas5805m_write_byte(0x1b, 0x00);

  // Right mixer input to left ouput (0 dB)
  ret |= tas5805m_write_byte(0x1c, 0x00);
  ret |= tas5805m_write_byte(0x1d, 0x80);
  ret |= tas5805m_write_byte(0x1e, 0x00);
  ret |= tas5805m_write_byte(0x1f, 0x00);

  #endif

  // Left mixer input to right ouput (-110 dB as the right output is not used)
  ret |= tas5805m_write_byte(0x20, 0x00);
  ret |= tas5805m_write_byte(0x21, 0x00);
  ret |= tas5805m_write_byte(0x22, 0x00);
  ret |= tas5805m_write_byte(0x23, 0x00);

  // Right mixer input to right ouput (-110 dB as the right output is not used)
  ret |= tas5805m_write_byte(0x24, 0x00);
  ret |= tas5805m_write_byte(0x25, 0x00);
  ret |= tas5805m_write_byte(0x26, 0x00);
  ret |= tas5805m_write_byte(0x27, 0x00);


  // End config
  ret |= tas5805m_write_byte(0x0, 0x0);
  ret |= tas5805m_write_byte(0x7f, 0x0);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Setting Bridge-Mode failed", __func__);
    return ret;
  }
#endif

  return ret;
}

// Getting cached TAS5805 state
esp_err_t tas5805m_get_state(TAS5805_STATE *out_state)
{
  *out_state = tas5805m_state;
  return ESP_OK;
}

// Setting the DAC State of TAS5805M
esp_err_t tas5805m_set_state(TAS5805M_CTRL_STATE state)
{
  ESP_LOGD(TAG, "%s: Setting state to 0x%x", __func__, state);
  esp_err_t ret = tas5805m_write_byte(TAS5805M_DEVICE_CTRL_2_REGISTER, state);
  if (ret == ESP_OK) {
    /* Update in-memory state only after successful device write */
    tas5805m_state.state = state;
  } else {
    ESP_LOGW(TAG, "%s: Failed to set device state (0x%x): %s", __func__, state, esp_err_to_name(ret));
  }
  return ret;
}

// Setting the Volume
esp_err_t tas5805m_set_volume(int vol) {
  ESP_LOGD(TAG, "%s: Setting volume to %d", __func__, vol);
  /* Clamp input percent to [0..100] */
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;

    /* If percent is zero, map to the explicit MUTE register value regardless of reg_min
     * Otherwise map linearly between register min and max. This preserves behaviour when
     * TAS5805M_VOLUME_MIN isn't 0xff while ensuring vol==0 always mutes.
     */
    uint8_t reg_val = 0;
    if (vol == 0) {
      reg_val = (uint8_t)TAS5805M_VOLUME_MUTE;
    } else {
      /* Map linear percent (1..100) to register range (TAS5805M_VOLUME_MIN..TAS5805M_VOLUME_MAX)
       * Note: register ordering may be descending (higher register = quieter). Formula handles that.
       */
      int32_t reg_min = (int32_t)TAS5805M_VOLUME_MIN;
      int32_t reg_max = (int32_t)TAS5805M_VOLUME_MAX;
      int32_t diff = reg_max - reg_min; /* may be negative */
      int32_t numer = diff * vol;
      /* integer rounding toward nearest */
      int32_t adj = (numer >= 0) ? (numer + 50) / 100 : (numer - 50) / 100;
      reg_val = (uint8_t)(reg_min + adj);
    }

  /* Writing the Volume to the Register*/
  esp_err_t ret = tas5805m_write_byte(TAS5805M_DIG_VOL_CTRL_REGISTER, reg_val);
  if (ret == ESP_OK) {
    tas5805m_state.volume = vol;
  } else {
    ESP_LOGW(TAG, "%s: Failed to write volume (reg 0x%02x): %s", __func__, reg_val, esp_err_to_name(ret));
  }
  return ret;
}

// Getting the Volume
esp_err_t tas5805m_get_volume(int *vol) {
  if (vol == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *vol = tas5805m_state.volume;
  ESP_LOGD(TAG, "%s: Getting volume (cached): %d", __func__, *vol);
  return ESP_OK;
}


// Setting the Volume [0..255], 0 is mute, 255 is full blast
esp_err_t tas5805m_set_digital_volume(uint8_t vol)
{
  esp_err_t ret = ESP_OK;
  if (vol < TAS5805M_VOLUME_DIGITAL_MIN)
  {
    vol = TAS5805M_VOLUME_DIGITAL_MIN;
  }
  if (vol > TAS5805M_VOLUME_DIGITAL_MAX)
  {
    vol = TAS5805M_VOLUME_DIGITAL_MAX;
  }

  ret = tas5805m_write_byte(TAS5805M_DIG_VOL_CTRL, vol);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  }

  return ret;
}

// Getting the Volume [0..255], 0 is mute, 255 is full blast
esp_err_t tas5805m_get_digital_volume(uint8_t *vol)
{
  esp_err_t ret = ESP_OK;
  ret = tas5805m_read_byte(TAS5805M_DIG_VOL_CTRL_REGISTER, vol);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  }
  return ret;
}

// Deinit the TAS5805M
esp_err_t tas5805m_deinit(void) {
  ESP_ERROR_CHECK(tas5805m_set_state(TAS5805M_CTRL_HI_Z));
  gpio_set_level(TAS5805M_GPIO_PDN, 0);
  vTaskDelay(6 / portTICK_PERIOD_MS);
  return ESP_OK;
}

// Setting mute state
esp_err_t tas5805m_set_mute(bool enable) {
  ESP_LOGD(TAG, "%s: Setting mute to %d", __func__, enable);
  TAS5805M_CTRL_STATE new_state;
  if (enable) {
    new_state = (TAS5805M_CTRL_STATE)(tas5805m_state.state | TAS5805M_CTRL_MUTE);
  } else {
    new_state = (TAS5805M_CTRL_STATE)(tas5805m_state.state & ~TAS5805M_CTRL_MUTE);
  }
  /* Use existing set_state helper which writes-first and updates cache on success */
  return tas5805m_set_state(new_state);
}

// Getting mute state
esp_err_t tas5805m_get_mute(bool *enabled) {
  bool mute = tas5805m_state.state & TAS5805M_CTRL_MUTE; 
  ESP_LOGD(TAG, "%s: Getting mute: %d", __func__, mute);
  *enabled = mute;
  return ESP_OK;
}

// Control function of TAS5805M
esp_err_t tas5805m_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state) {
  ESP_LOGI(TAG, "%s: Control state: %d", __func__, ctrl_state);
  TAS5805M_CTRL_STATE new_state;

  if (ctrl_state == AUDIO_HAL_CTRL_STOP) {
    ESP_LOGD(TAG, "%s: Setting to DEEP_SLEEP", __func__);
    /* Clear lower 3 bits (state field) then set to DEEP_SLEEP (0x0)
     * This ensures lower bits are reset to 0 as required by the device.
     */
    new_state = (TAS5805M_CTRL_STATE)((tas5805m_state.state & ~0x07) | TAS5805M_CTRL_DEEP_SLEEP);
  } else if (ctrl_state == AUDIO_HAL_CTRL_START ) {
    ESP_LOGD(TAG, "%s: Setting to PLAY", __func__);
    /* Clear lower 3 bits (state field) and set to PLAY (0x3), preserve other flags */
    new_state = (TAS5805M_CTRL_STATE)((tas5805m_state.state & ~0x07) | TAS5805M_CTRL_PLAY);
  } else {
    ESP_LOGW(TAG, "%s: Unknown control state: %d", __func__, ctrl_state);
    return ESP_FAIL;
  }

  return tas5805m_set_state(new_state);
}

esp_err_t tas5805m_config_iface(audio_hal_codec_mode_t mode,
                                audio_hal_codec_i2s_iface_t *iface) {
  // TODO
  return ESP_OK;
}

esp_err_t tas5805m_get_dac_mode(TAS5805M_DAC_MODE *mode)
{
    uint8_t current_value;
    esp_err_t err = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1, &current_value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(err));
        return err;
    }

    if (current_value & (1 << 2)) {
        *mode = TAS5805M_DAC_MODE_PBTL;
    } else {
        *mode = TAS5805M_DAC_MODE_BTL;
    }

    return ESP_OK;
}

esp_err_t tas5805m_set_dac_mode(TAS5805M_DAC_MODE mode)
{
    ESP_LOGD(TAG, "%s: Setting DAC mode to %d", __func__, mode);

    // Read the current value of the register
    uint8_t current_value;
    esp_err_t err = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1, &current_value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(err));
        return err;
    }

    // Update bit 2 based on the mode
    if (mode == TAS5805M_DAC_MODE_PBTL) {
        current_value |= (1 << 2);  // Set bit 2 to 1 (PBTL mode)
    } else {
        current_value &= ~(1 << 2); // Clear bit 2 to 0 (BTL mode)
    }

    // Write the updated value back to the register
    int ret = tas5805m_write_byte(TAS5805M_DEVICE_CTRL_1, current_value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t tas5805m_get_modulation_mode(TAS5805M_MOD_MODE *mode, TAS5805M_SW_FREQ *freq, TAS5805M_BD_FREQ *bd_freq)
{
  // Read the current value of the register
  uint8_t current_value;
  esp_err_t err = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1, &current_value);
  if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(err));
      return err;
  }

  // Extract bits 0-1
  *mode = (current_value & 0b00000011);
  // Extract bits 4-6
  *freq = (current_value & 0b01110000);

  // Read the BD frequency
  err = tas5805m_read_byte(TAS5805M_ANA_CTRL, &current_value);
  if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(err));
      return err;
  }

  *bd_freq = (current_value & 0b01100000);
  return ESP_OK;
}

esp_err_t tas5805m_set_modulation_mode(TAS5805M_MOD_MODE mode, TAS5805M_SW_FREQ freq, TAS5805M_BD_FREQ bd_freq)
{
  ESP_LOGD(TAG, "%s: Setting modulation to %d, FSW: %d, Class-D bandwidth control: %d", __func__, mode, freq, bd_freq);

  // Read the current value of the register
  uint8_t current_value;
  esp_err_t err = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1, &current_value);
  if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(err));
      return err;
  }

  // Clear bits 0-1 and 4-6
  current_value &= ~((0x07 << 4) | (0x03 << 0));
  // Update bit 0-1 based on the mode
  current_value |= mode & 0b00000011;  // Set bits 0-1
  // Update bits 4-6 based on sw freq
  current_value |= freq & 0b01110000;  // Set bits 4-6
  
  // Write the updated value back to the register
  int ret = tas5805m_write_byte(TAS5805M_DEVICE_CTRL_1, current_value);
  if (ret != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  } 

  // Set the BD frequency
  ret = tas5805m_write_byte(TAS5805M_ANA_CTRL, bd_freq);
  if (ret != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  } 

  return ret;
}

esp_err_t tas5805m_get_again(uint8_t *gain)
{
  int ret = ESP_OK;
  ret = tas5805m_read_byte(TAS5805M_AGAIN, gain);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t tas5805m_set_again(uint8_t gain)
{
  // Gain is inverted!
  if (gain < TAS5805M_MAX_GAIN || gain > TAS5805M_MIN_GAIN)
  {
    ESP_LOGE(TAG, "%s: Invalid gain %d", __func__, gain);
    return ESP_ERR_INVALID_ARG;
  }

  uint8_t value = tas5805m_again[gain];
  int ret = tas5805m_write_byte(TAS5805M_AGAIN, value);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t tas5805m_clear_faults()
{
  ESP_LOGD(TAG, "%s: Clearing faults", __func__);
  int ret = tas5805m_write_byte(TAS5805M_FAULT_CLEAR, TAS5805M_ANALOG_FAULT_CLEAR);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t tas5805m_get_faults(TAS5805M_FAULT *fault)
{
  int ret = ESP_OK;

  ret = tas5805m_read_byte(TAS5805M_CHAN_FAULT, &(fault->err0));
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    return ret;
  }

  ret = tas5805m_read_byte(TAS5805M_GLOBAL_FAULT1, &(fault->err1));
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    return ret;
  }

  ret = tas5805m_read_byte(TAS5805M_GLOBAL_FAULT2, &(fault->err2));
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    return ret;
  }

  ret= tas5805m_read_byte(TAS5805M_OT_WARNING, &(fault->ot_warn));
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    return ret;
  }

  return ret;
}

void tas5805m_decode_faults(TAS5805M_FAULT fault)
{
  if (fault.err0) {
    if (fault.err0 & (1 << 0))  
        ESP_LOGW(TAG, "%s: Right channel over current fault", __func__);

    if (fault.err0 & (1 << 1))
        ESP_LOGW(TAG, "%s: Left channel over current fault", __func__);

    if (fault.err0 & (1 << 2)) 
        ESP_LOGW(TAG, "%s: Right channel DC fault", __func__);

    if (fault.err0 & (1 << 3))  
        ESP_LOGW(TAG, "%s: Left channel DC fault", __func__);
  }

  if (fault.err1) {
    if (fault.err1 & (1 << 0))  
        ESP_LOGW(TAG, "%s: PVDD UV fault", __func__);

    if (fault.err1 & (1 << 1))
        ESP_LOGW(TAG, "%s: PVDD OV fault", __func__);

    if (fault.err1 & (1 << 2)) 
        ESP_LOGW(TAG, "%s: Clock fault", __func__);

    if (fault.err1 & (1 << 6))  
        ESP_LOGW(TAG, "%s: The recent BQ is written failed", __func__);

    if (fault.err1 & (1 << 7))  
        ESP_LOGW(TAG, "%s: Indicate OTP CRC check error", __func__);
  }

  if (fault.err2) {
    if (fault.err2 & (1 << 0))  
        ESP_LOGW(TAG, "%s: Over temperature shut down fault", __func__);
  }

  if (fault.ot_warn) {
    if (fault.ot_warn & (1 << 2))  
        ESP_LOGW(TAG, "%s: Over temperature warning", __func__);
  }
}
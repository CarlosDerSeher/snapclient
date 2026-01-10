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
#include <math.h>

static const char *TAG = "TAS5805M";

#define TAS5805M_SET_BOOK_AND_PAGE(BOOK, PAGE) \
    do { \
      tas5805m_write_byte(TAS5805M_REG_PAGE_SET, TAS5805M_REG_PAGE_ZERO); \
      tas5805m_write_byte(TAS5805M_REG_BOOK_SET, BOOK);                   \
      tas5805m_write_byte(TAS5805M_REG_PAGE_SET, PAGE);                   \
    } while (0)

// State of TAS5805M (internal to this module)
static TAS5805_STATE tas5805m_state = {
  .volume = 0,
  .state = TAS5805M_CTRL_PLAY,
  .mixer_mode = MIXER_STEREO,
  .channel_gain_l = 0,
  .channel_gain_r = 0,
};

/* Task handle for fault monitoring */
static TaskHandle_t tas5805m_fault_monitor_task_handle = NULL;

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

/* Fault monitoring task */
static void tas5805m_fault_monitor_task(void *pvParameters) {
  TAS5805M_FAULT fault;
  ESP_LOGI(TAG, "Fault monitoring task started");
  
  while (1) {
    // Read fault registers
    esp_err_t ret = tas5805m_get_faults(&fault);
    if (ret == ESP_OK) {
      // Check if any faults are present
      if (fault.err0 || fault.err1 || fault.err2 || fault.ot_warn) {
        ESP_LOGW(TAG, "Faults detected: err0=0x%02x, err1=0x%02x, err2=0x%02x, ot_warn=0x%02x",
                 fault.err0, fault.err1, fault.err2, fault.ot_warn);
        
        // Decode and log the faults
        tas5805m_decode_faults(fault);
        
        // Clear the faults
        ret = tas5805m_clear_faults();
        if (ret == ESP_OK) {
          ESP_LOGI(TAG, "Faults cleared");
        } else {
          ESP_LOGE(TAG, "Failed to clear faults: %s", esp_err_to_name(ret));
        }
      }
    } else {
      ESP_LOGE(TAG, "Failed to read faults: %s", esp_err_to_name(ret));
    }
    
    // Wait for 1 second
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

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
  ESP_LOGV(TAG, "%s: Read 0x%02x from register 0x%02x", __func__, *data, register_name);
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

esp_err_t tas5805m_write_bytes(uint8_t *reg,
                               int regLen, uint8_t *data, int datalen)
{
  int ret = ESP_OK;
  ESP_LOGV(TAG, "%s: 0x%02x <- [%d] bytes", __func__, *reg, datalen);
  for (int i = 0; i < datalen; i++)
  {
    ESP_LOGV(TAG, "%s: 0x%02x", __func__, data[i]);
  }

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  ret |= i2c_master_start(cmd);
  ret |= i2c_master_write_byte(cmd, TAS5805M_ADDRESS << 1 | WRITE_BIT, ACK_CHECK_EN);
  ret |= i2c_master_write(cmd, reg, regLen, ACK_CHECK_EN);
  ret |= i2c_master_write(cmd, data, datalen, ACK_CHECK_EN);
  ret |= i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(I2C_TAS5805M_MASTER_NUM, cmd, 1000 / portTICK_RATE_MS);

  // Check if ret is OK
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  }

  i2c_cmd_link_delete(cmd);

  return ret;
}

esp_err_t tas5805m_read_bytes(uint8_t *reg, int regLen, uint8_t *data, int datalen)
{
  int ret = ESP_OK;
  ESP_LOGV(TAG, "%s: 0x%02x -> [%d] bytes", __func__, *reg, datalen);

  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  ret |= i2c_master_start(cmd);
  ret |= i2c_master_write_byte(cmd, TAS5805M_ADDRESS << 1 | WRITE_BIT, ACK_CHECK_EN);
  ret |= i2c_master_write(cmd, reg, regLen, ACK_CHECK_EN);
  ret |= i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(I2C_TAS5805M_MASTER_NUM, cmd, 1000 / portTICK_RATE_MS);
  i2c_cmd_link_delete(cmd);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Error during I2C write phase: %s", __func__, esp_err_to_name(ret));
    return ret;
  }

  vTaskDelay(1 / portTICK_PERIOD_MS);
  
  cmd = i2c_cmd_link_create();
  ret |= i2c_master_start(cmd);
  ret |= i2c_master_write_byte(cmd, TAS5805M_ADDRESS << 1 | READ_BIT, ACK_CHECK_EN);
  if (datalen > 1) {
    ret |= i2c_master_read(cmd, data, datalen - 1, ACK_VAL);
  }
  ret |= i2c_master_read_byte(cmd, data + datalen - 1, NACK_VAL);
  ret |= i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(I2C_TAS5805M_MASTER_NUM, cmd, 1000 / portTICK_RATE_MS);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Error during I2C read phase: %s", __func__, esp_err_to_name(ret));
  } else {
    for (int i = 0; i < datalen; i++) {
      ESP_LOGV(TAG, "%s: [%d] = 0x%02x", __func__, i, data[i]);
    }
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

  ESP_LOGI(TAG, "%s: Setting to HI Z", __func__);

  ESP_ERROR_CHECK(tas5805m_set_state(TAS5805M_CTRL_HI_Z));
  
  // Send RESET register flag to reset all registers to default state
	ret = tas5805m_write_byte(TAS5805M_RESET_CTRL_REGISTER, TAS5805M_RESET_CONTROL_PORT | TAS5805M_RESET_DSP);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "%s: Failed to set RESET flag: %s", __func__, esp_err_to_name(ret));
  }

  vTaskDelay(10 / portTICK_PERIOD_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "%s: Set DAC state failed", __func__);
    return ret;
  }

  ESP_LOGI(TAG, "%s: Setting to PLAY (muted)", __func__);

  ESP_ERROR_CHECK(tas5805m_set_state(TAS5805M_CTRL_MUTE | TAS5805M_CTRL_PLAY));
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "%s: Set DAC state failed", __func__);
    return ret;
  }

  /* Bridge-mode configuration removed: bridge mode is now controlled at runtime
     via the UI and persisted through settings. If you need to reintroduce
     compile-time bridge-mode options, re-add the Kconfig choice and the
     corresponding conditional code here. */

  /* Start fault monitoring task */
  BaseType_t task_ret = xTaskCreate(
    tas5805m_fault_monitor_task,
    "tas5805m_faults",
    2048,
    NULL,
    5,
    &tas5805m_fault_monitor_task_handle
  );
  
  if (task_ret != pdPASS) {
    ESP_LOGE(TAG, "%s: Failed to create fault monitoring task", __func__);
    return ESP_FAIL;
  }
  
  ESP_LOGI(TAG, "%s: Fault monitoring task created", __func__);

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

  ret = tas5805m_write_byte(TAS5805M_DIG_VOL_CTRL_REGISTER, vol);
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
  /* Stop fault monitoring task */
  if (tas5805m_fault_monitor_task_handle != NULL) {
    vTaskDelete(tas5805m_fault_monitor_task_handle);
    tas5805m_fault_monitor_task_handle = NULL;
    ESP_LOGI(TAG, "%s: Fault monitoring task deleted", __func__);
  }
  
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
    esp_err_t err = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, &current_value);
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
    esp_err_t err = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, &current_value);
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
    int ret = tas5805m_write_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, current_value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t tas5805m_get_modulation_mode(TAS5805M_MOD_MODE *mode, TAS5805M_SW_FREQ *freq, TAS5805M_BD_FREQ *bd_freq)
{
  // Read the current value of the register
  uint8_t current_value;
  esp_err_t err = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, &current_value);
  if (err != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(err));
      return err;
  }

  // Extract bits 0-1
  *mode = (current_value & 0b00000011);
  // Extract bits 4-6
  *freq = (current_value & 0b01110000);

  // Read the BD frequency
  err = tas5805m_read_byte(TAS5805M_ANA_CTRL_REGISTER, &current_value);
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
  esp_err_t err = tas5805m_read_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, &current_value);
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
  int ret = tas5805m_write_byte(TAS5805M_DEVICE_CTRL_1_REGISTER, current_value);
  if (ret != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  } 

  // Set the BD frequency
  ret = tas5805m_write_byte(TAS5805M_ANA_CTRL_REGISTER, bd_freq);
  if (ret != ESP_OK) {
      ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  } 

  return ret;
}

esp_err_t tas5805m_get_again(uint8_t *gain)
{
  int ret = ESP_OK;
  ret = tas5805m_read_byte(TAS5805M_AGAIN_REGISTER, gain);
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
  int ret = tas5805m_write_byte(TAS5805M_AGAIN_REGISTER, value);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t tas5805m_get_mixer_mode(TAS5805M_MIXER_MODE *mode)
{
  *mode = tas5805m_state.mixer_mode;
  return ESP_OK;
}

esp_err_t tas5805m_set_mixer_mode(TAS5805M_MIXER_MODE mode)
{
  ESP_LOGD(TAG, "%s: Setting mixer mode to %d", __func__, mode);
  
  uint32_t mixer_l_to_l, mixer_r_to_r, mixer_l_to_r, mixer_r_to_l;
  int ret = ESP_OK;

  switch (mode)
  {
  case MIXER_STEREO:
    mixer_l_to_l = TAS5805M_MIXER_VALUE_0DB;
    mixer_l_to_r = TAS5805M_MIXER_VALUE_MUTE;
    mixer_r_to_l = TAS5805M_MIXER_VALUE_MUTE;
    mixer_r_to_r = TAS5805M_MIXER_VALUE_0DB;
    break;

  case MIXER_STEREO_INVERSE:
    mixer_l_to_l = TAS5805M_MIXER_VALUE_MUTE;
    mixer_l_to_r = TAS5805M_MIXER_VALUE_0DB;
    mixer_r_to_l = TAS5805M_MIXER_VALUE_0DB;
    mixer_r_to_r = TAS5805M_MIXER_VALUE_MUTE;
    break;

  case MIXER_MONO:
    mixer_l_to_l = TAS5805M_MIXER_VALUE_MINUS6DB;
    mixer_r_to_r = TAS5805M_MIXER_VALUE_MINUS6DB;
    mixer_l_to_r = TAS5805M_MIXER_VALUE_MINUS6DB;
    mixer_r_to_l = TAS5805M_MIXER_VALUE_MINUS6DB;
    break;

  case MIXER_LEFT:
    mixer_l_to_l = TAS5805M_MIXER_VALUE_0DB;
    mixer_r_to_r = TAS5805M_MIXER_VALUE_MUTE;
    mixer_l_to_r = TAS5805M_MIXER_VALUE_0DB;
    mixer_r_to_l = TAS5805M_MIXER_VALUE_MUTE;
    break;

  case MIXER_RIGHT:
    mixer_l_to_l = TAS5805M_MIXER_VALUE_MUTE;
    mixer_r_to_r = TAS5805M_MIXER_VALUE_0DB;
    mixer_l_to_r = TAS5805M_MIXER_VALUE_MUTE;
    mixer_r_to_l = TAS5805M_MIXER_VALUE_0DB;
    break;

  default:
    ESP_LOGE(TAG, "%s: Invalid mixer mode %d", __func__, mode);
    return ESP_ERR_INVALID_ARG;
  }
    
  ret = ret | tas5805m_set_mixer_gain(TAS5805M_MIXER_CHANNEL_LEFT_TO_LEFT, mixer_l_to_l);
  ret = ret | tas5805m_set_mixer_gain(TAS5805M_MIXER_CHANNEL_RIGHT_TO_RIGHT, mixer_r_to_r);
  ret = ret | tas5805m_set_mixer_gain(TAS5805M_MIXER_CHANNEL_LEFT_TO_RIGHT, mixer_l_to_r);
  ret = ret | tas5805m_set_mixer_gain(TAS5805M_MIXER_CHANNEL_RIGHT_TO_LEFT, mixer_r_to_l);

  tas5805m_state.mixer_mode = mode;
  return ret;
}

esp_err_t tas5805m_set_mixer_gain(TAS5805M_MIXER_CHANNELS channel, uint32_t gain)
{
  ESP_LOGD(TAG, "%s: Setting mixer gain for channel %d to 0x%08x", __func__, channel, (unsigned int)gain);
  uint8_t reg;

  switch (channel)
  {
  case TAS5805M_MIXER_CHANNEL_LEFT_TO_LEFT:
    reg = TAS5805M_REG_LEFT_TO_LEFT_GAIN;
    break;
  case TAS5805M_MIXER_CHANNEL_RIGHT_TO_RIGHT:
    reg = TAS5805M_REG_RIGHT_TO_RIGHT_GAIN;
    break;
  case TAS5805M_MIXER_CHANNEL_LEFT_TO_RIGHT:
    reg = TAS5805M_REG_LEFT_TO_RIGHT_GAIN;
    break;
  case TAS5805M_MIXER_CHANNEL_RIGHT_TO_LEFT:
    reg = TAS5805M_REG_RIGHT_TO_LEFT_GAIN;
    break;
  default:
    ESP_LOGE(TAG, "%s: Invalid mixer channel %d", __func__, channel);
    return ESP_ERR_INVALID_ARG;
  } 

  TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_5, TAS5805M_REG_BOOK_5_MIXER_PAGE);
  int ret = tas5805m_write_bytes(&reg, 1, (uint8_t *)&gain, sizeof(gain));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write register %d: %d", TAS5805M_REG_LEFT_TO_LEFT_GAIN, ret);
  }

  TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_CONTROL_PORT, TAS5805M_REG_PAGE_ZERO); 
  return ret;
}


// Set output channel volume using mixer gain lookup table
esp_err_t tas5805m_set_channel_gain(TAS5805M_EQ_CHANNELS channel, int8_t gain_db)
{
  ESP_LOGD(TAG, "%s: Setting channel %d volume to %d dB", __func__, channel, gain_db);

  if (gain_db < TAS5805M_MIXER_VALUE_MINDB || gain_db > TAS5805M_MIXER_VALUE_MAXDB) {
    ESP_LOGE(TAG, "%s: Invalid gain_db %d, must be between %d and %d", __func__, gain_db, TAS5805M_MIXER_VALUE_MINDB, TAS5805M_MIXER_VALUE_MAXDB);
    return ESP_ERR_INVALID_ARG;
  }

  /* Convert dB to linear gain and then to Q9.23 register format */
  float linear = powf(10.0f, ((float)gain_db) / 20.0f);
  uint32_t reg_value = tas5805m_float_to_q9_23(linear);

  uint8_t reg;
  if (channel == TAS5805M_EQ_CHANNELS_RIGHT) {
    reg = TAS5805M_REG_RIGHT_VOLUME;
  } else {
    // Default to left for any other value (matches other EQ channel helpers)
    reg = TAS5805M_REG_LEFT_VOLUME;
  }

  TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_5, TAS5805M_REG_BOOK_5_VOLUME_PAGE);
  int ret = tas5805m_write_bytes(&reg, 1, (uint8_t *)&reg_value, sizeof(reg_value));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: Failed to write volume register 0x%02x: %s", __func__, reg, esp_err_to_name(ret));
  } else {
    ESP_LOGD(TAG, "%s: Wrote volume register 0x%02x with value 0x%08x", __func__, reg, (unsigned int)reg_value);
  }

  TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_CONTROL_PORT, TAS5805M_REG_PAGE_ZERO);
  if (ret == ESP_OK) {
    if (channel == TAS5805M_EQ_CHANNELS_RIGHT) {
      tas5805m_state.channel_gain_r = gain_db;
    } else {
      tas5805m_state.channel_gain_l = gain_db;
    }
  }

  return ret;
}

esp_err_t tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS channel, int8_t *gain_db)
{
  if (gain_db == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (channel == TAS5805M_EQ_CHANNELS_RIGHT) {
    *gain_db = tas5805m_state.channel_gain_r;
  } else {
    *gain_db = tas5805m_state.channel_gain_l;
  }

  ESP_LOGV(TAG, "%s: Returning cached channel %d gain %d dB", __func__, channel, *gain_db);
  return ESP_OK;
}

esp_err_t tas5805m_clear_faults()
{
  ESP_LOGD(TAG, "%s: Clearing faults", __func__);
  int ret = tas5805m_write_byte(TAS5805M_FAULT_CLEAR_REGISTER, TAS5805M_ANALOG_FAULT_CLEAR);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t tas5805m_get_faults(TAS5805M_FAULT *fault)
{
  int ret = ESP_OK;

  ret = tas5805m_read_byte(TAS5805M_CHAN_FAULT_REGISTER, &(fault->err0));
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    return ret;
  }

  ret = tas5805m_read_byte(TAS5805M_GLOBAL_FAULT1_REGISTER, &(fault->err1));
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    return ret;
  }

  ret = tas5805m_read_byte(TAS5805M_GLOBAL_FAULT2_REGISTER, &(fault->err2));
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(ret));
    return ret;
  }

  ret= tas5805m_read_byte(TAS5805M_OT_WARNING_REGISTER, &(fault->ot_warn));
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

/* EQ-related functions and data: compile only when enabled in Kconfig */
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)

esp_err_t tas5805m_get_eq_mode(TAS5805M_EQ_MODE *mode)
{
  uint8_t value = 0;
  esp_err_t err = tas5805m_read_byte(TAS5805M_DSP_MISC_REGISTER, &value);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "%s: Error during I2C transmission: %s", __func__, esp_err_to_name(err));
    return err;
  }

  // Extract the EQ mode from the value
  *mode = (TAS5805M_EQ_MODE)(value & 0b00001111);
  return ESP_OK;
}

esp_err_t tas5805m_set_eq_mode(TAS5805M_EQ_MODE mode)
{
  ESP_LOGD(TAG, "%s: Setting EQ MODE to %d", __func__, mode);
  return tas5805m_write_byte(TAS5805M_DSP_MISC_REGISTER, (uint8_t)mode);
}

esp_err_t tas5805m_set_eq(bool enable)
{
  ESP_LOGD(TAG, "%s: Setting EQ to %d", __func__, enable);
  return tas5805m_write_byte(TAS5805M_DSP_MISC_REGISTER, enable ? TAS5805M_CTRL_EQ_ON : TAS5805M_CTRL_EQ_OFF);
}

esp_err_t tas5805m_get_eq_gain(int band, int *gain)
{
  return tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
}

esp_err_t tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS channel, int band, int *gain)
{
  switch (channel)
  {
    case TAS5805M_EQ_CHANNELS_RIGHT:
      *gain = tas5805m_state.eq_gain_r[band];
      break;
    default:
      *gain = tas5805m_state.eq_gain_l[band];
      break;
  }
  return ESP_OK;
}

esp_err_t tas5805m_set_eq_gain(int band, int gain) {
  return tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
}

esp_err_t tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS channel, int band, int gain)
{
  if (band < 0 || band >= TAS5805M_EQ_BANDS)
  {
    ESP_LOGE(TAG, "%s: Invalid band %d", __func__, band);
    return ESP_ERR_INVALID_ARG;
  }

  if (gain < TAS5805M_EQ_MIN_DB || gain > TAS5805M_EQ_MAX_DB)
  {
    ESP_LOGE(TAG, "%s: Invalid gain %d", __func__, gain);
    return ESP_ERR_INVALID_ARG;
  }

  int current_page = 0; 
  int ret = ESP_OK;
  ESP_LOGD(TAG, "%s: Setting EQ band %d (%d Hz) to gain %d", __func__, band, tas5805m_eq_bands[band], gain);

  int x = gain + TAS5805M_EQ_MAX_DB;                                 
  int y = band * TAS5805M_EQ_KOEF_PER_BAND * TAS5805M_EQ_REG_PER_KOEF; 
    
  const reg_sequence_eq **eq_maps = (channel == TAS5805M_EQ_CHANNELS_RIGHT) ? tas5805m_eq_registers_right : tas5805m_eq_registers_left;

  for (int i = 0; i < TAS5805M_EQ_KOEF_PER_BAND * TAS5805M_EQ_REG_PER_KOEF; i += TAS5805M_EQ_REG_PER_KOEF) 
  { 
      const reg_sequence_eq *reg_value0 = &eq_maps[x][y + i + 0];
      const reg_sequence_eq *reg_value1 = &eq_maps[x][y + i + 1];
      const reg_sequence_eq *reg_value2 = &eq_maps[x][y + i + 2];
      const reg_sequence_eq *reg_value3 = &eq_maps[x][y + i + 3];

      if (reg_value0 == NULL || reg_value1 == NULL || reg_value2 == NULL || reg_value3 == NULL) {                                        
          ESP_LOGW(TAG, "%s: NULL pointer encountered at row[%d]", __func__, y + i); 
          continue;                                                   
      }                                                               
      
      // Assume all 4 reg values are in the same page, seems to be true for all BQ registers
      if (reg_value0->page != current_page) {                          
        TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_EQ, reg_value0->page); 
        current_page = reg_value0->page;                             
      }                                                               
                
      uint8_t address = reg_value0->offset;
      uint32_t value = reg_value0->value | 
                     (reg_value1->value << 8) | 
                     (reg_value2->value << 16) | 
                     (reg_value3->value << 24);

      ESP_LOGV(TAG, "%s: + %d: w 0x%x 0x%x 0x%x 0x%x 0x%x -> 0x%x", __func__, i, 
             reg_value0->offset, reg_value0->value, 
             reg_value1->value, reg_value2->value, reg_value3->value, (unsigned int)value);
      ret = ret | tas5805m_write_bytes(&address, 1, (uint8_t *)&value, sizeof(value));
      if (ret != ESP_OK) { 
          ESP_LOGE(TAG, "%s: Error writing to register 0x%x", __func__, address); 
      }          
  }   
  
  if (channel == TAS5805M_EQ_CHANNELS_RIGHT)
    tas5805m_state.eq_gain_r[band] = gain;
  else
    tas5805m_state.eq_gain_l[band] = gain;
                                                                      
  TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_CONTROL_PORT, TAS5805M_REG_PAGE_ZERO); 
  return ret;
}

esp_err_t tas5805m_get_eq_profile(TAS5805M_EQ_PROFILE *profile)
{
  return tas5805m_get_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, profile);
}

esp_err_t tas5805m_get_eq_profile_channel(TAS5805M_EQ_CHANNELS channel, TAS5805M_EQ_PROFILE *profile)
{
  *profile = tas5805m_state.eq_profile[channel];
  return ESP_OK;
}

esp_err_t tas5805m_set_eq_profile(TAS5805M_EQ_PROFILE profile) {
  return tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, profile);
}

esp_err_t tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS channel, TAS5805M_EQ_PROFILE profile)
{
  // Apply preset EQ gains for the selected profile
  int current_page = 0; 
  int ret = ESP_OK;
  ESP_LOGD(TAG, "%s: Setting EQ profile to %d", __func__, profile);
  
  const reg_sequence_eq **eq_maps = (channel == TAS5805M_EQ_CHANNELS_RIGHT) ? tas5805m_eq_profile_right_registers : tas5805m_eq_profile_left_registers;

  int x = (uint8_t)profile;
  for (int i = 0; i < TAS5805M_EQ_PROFILE_REG_PER_STEP; i += TAS5805M_EQ_REG_PER_KOEF) 
  { 
    const reg_sequence_eq *reg_value0 = &eq_maps[x][i + 0]; 
    const reg_sequence_eq *reg_value1 = &eq_maps[x][i + 1];
    const reg_sequence_eq *reg_value2 = &eq_maps[x][i + 2];
    const reg_sequence_eq *reg_value3 = &eq_maps[x][i + 3];

    if (reg_value0 == NULL || reg_value1 == NULL || reg_value2 == NULL || reg_value3 == NULL) {                                        
        ESP_LOGW(TAG, "%s: NULL pointer encountered at row[%d]", __func__, i); 
        continue;                                                   
    }                
      
    // Assume all 4 reg values are in the same page, seems to be true for all BQ registers
    if (reg_value0->page != current_page) {                          
        current_page = reg_value0->page;                             
        TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_EQ, reg_value0->page); 
    }                                                               

    uint8_t address = reg_value0->offset;
    uint32_t value = reg_value0->value | 
                     (reg_value1->value << 8) | 
                     (reg_value2->value << 16) | 
                     (reg_value3->value << 24);
   
    // ESP_LOGV(TAG, "%s: + %d: w 0x%x 0x%x 0x%x 0x%x 0x%x -> 0x%x", __func__, i, 
    //        reg_value0->offset, reg_value0->value, 
    //        reg_value1->value, reg_value2->value, reg_value3->value, (unsigned int)value);
    ret = ret | tas5805m_write_bytes(&address, 1, (uint8_t *)&value, sizeof(value));
    // ret = ret | tas5805m_write_byte(reg_value->offset, reg_value->value);
    if (ret != ESP_OK) { 
        ESP_LOGE(TAG, "%s: Error writing to register 0x%x", __func__, address); 
    }     
  }

  // Set the EQ profile
  tas5805m_state.eq_profile[channel] = profile;

  TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_CONTROL_PORT, TAS5805M_REG_PAGE_ZERO); 
  return ret;
}

/* -------------------------
   Biquad coefficient access
   ------------------------- */

/**
 * @brief Get the register offset for a specific biquad coefficient.
 * 
 * Each band has 5 coefficients (B0, B1, B2, A1, A2) stored sequentially.
 * Each coefficient is 4 bytes in Q5.27 format.
 * 
 * @param channel Left or right channel
 * @param band Band index (0-14)
 * @param coef_index Coefficient index (0=B0, 1=B1, 2=B2, 3=A1, 4=A2)
 * @param page Output: page number
 * @param offset Output: register offset
 * @return ESP_OK on success
 */
static esp_err_t tas5805m_get_biquad_register(TAS5805M_EQ_CHANNELS channel, int band, 
                                                int coef_index, uint8_t *page, uint8_t *offset)
{
  if (band < 0 || band >= TAS5805M_EQ_BANDS) {
    ESP_LOGE(TAG, "%s: Invalid band %d", __func__, band);
    return ESP_ERR_INVALID_ARG;
  }
  
  if (coef_index < 0 || coef_index >= TAS5805M_EQ_KOEF_PER_BAND) {
    ESP_LOGE(TAG, "%s: Invalid coefficient index %d", __func__, coef_index);
    return ESP_ERR_INVALID_ARG;
  }

  // Calculate position in the register map
  // We need to look at the data tables to find the pattern
  // From tas5805m_set_eq_gain_channel: gain 0dB is at index TAS5805M_EQ_MAX_DB
  const reg_sequence_eq **eq_maps = (channel == TAS5805M_EQ_CHANNELS_RIGHT) ? 
                                     tas5805m_eq_registers_right : tas5805m_eq_registers_left;
  
  // Use 0dB gain (middle of the range) as reference for current coefficient positions
  int gain_index = TAS5805M_EQ_MAX_DB; // 0dB
  int base_index = band * TAS5805M_EQ_KOEF_PER_BAND * TAS5805M_EQ_REG_PER_KOEF;
  int coef_offset = coef_index * TAS5805M_EQ_REG_PER_KOEF;
  int reg_index = base_index + coef_offset;
  
  const reg_sequence_eq *reg = &eq_maps[gain_index][reg_index];
  *page = reg->page;
  *offset = reg->offset;
  
  return ESP_OK;
}

esp_err_t tas5805m_read_biquad_coefficients(TAS5805M_EQ_CHANNELS channel, int band, 
                                              float *b0, float *b1, float *b2, 
                                              float *a1, float *a2)
{
  if (!b0 || !b1 || !b2 || !a1 || !a2) {
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGD(TAG, "%s: Reading biquad coefficients for channel %d, band %d", 
           __func__, channel, band);

  esp_err_t ret = ESP_OK;
  uint8_t page, offset;
  uint32_t raw_value;
  
  // Read each coefficient
  float *coeffs[] = {b0, b1, b2, a1, a2};
  const char *names[] = {"B0", "B1", "B2", "A1", "A2"};
  
  for (int i = 0; i < TAS5805M_EQ_KOEF_PER_BAND; i++) {
    ret = tas5805m_get_biquad_register(channel, band, i, &page, &offset);
    if (ret != ESP_OK) {
      return ret;
    }
    
    TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_EQ, page);
    
    ret = tas5805m_read_bytes(&offset, 1, (uint8_t *)&raw_value, sizeof(raw_value));
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "%s: Failed to read coefficient %s: %s", 
               __func__, names[i], esp_err_to_name(ret));
      break;
    }
    
    *coeffs[i] = tas5805m_q5_27_to_float(raw_value);
    ESP_LOGD(TAG, "%s: %s = %f (raw: 0x%08X)", __func__, names[i], *coeffs[i], (unsigned int)raw_value);
  }
  
  TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_CONTROL_PORT, TAS5805M_REG_PAGE_ZERO);
  return ret;
}

esp_err_t tas5805m_write_biquad_coefficients(TAS5805M_EQ_CHANNELS channel, int band,
                                               float b0, float b1, float b2,
                                               float a1, float a2)
{
  ESP_LOGD(TAG, "%s: Writing biquad coefficients for channel %d, band %d", 
           __func__, channel, band);
  ESP_LOGD(TAG, "%s: B0=%f, B1=%f, B2=%f, A1=%f, A2=%f", 
           __func__, b0, b1, b2, a1, a2);

  esp_err_t ret = ESP_OK;
  uint8_t current_page = 0;
  uint8_t page, offset;
  uint32_t raw_value;
  
  float coeffs[] = {b0, b1, b2, a1, a2};
  const char *names[] = {"B0", "B1", "B2", "A1", "A2"};
  
  for (int i = 0; i < TAS5805M_EQ_KOEF_PER_BAND; i++) {
    ret = tas5805m_get_biquad_register(channel, band, i, &page, &offset);
    if (ret != ESP_OK) {
      return ret;
    }
    
    if (page != current_page) {
      TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_EQ, page);
      current_page = page;
    }
    
    raw_value = tas5805m_float_to_q5_27(coeffs[i]);
    ESP_LOGD(TAG, "%s: Writing %s = %f -> 0x%08X to offset 0x%02X", 
             __func__, names[i], coeffs[i], (unsigned int)raw_value, offset);
    
    ret = tas5805m_write_bytes(&offset, 1, (uint8_t *)&raw_value, sizeof(raw_value));
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "%s: Failed to write coefficient %s: %s", 
               __func__, names[i], esp_err_to_name(ret));
      break;
    }
  }
  
  TAS5805M_SET_BOOK_AND_PAGE(TAS5805M_REG_BOOK_CONTROL_PORT, TAS5805M_REG_PAGE_ZERO);
  return ret;
}

#endif /* CONFIG_DAC_TAS5805M_EQ_SUPPORT */

/* -------------------------
   Q9.23 conversions
   ------------------------- */

float tas5805m_q9_23_to_float(uint32_t raw)
{
    uint32_t val = tas5805m_swap_endian_32(raw);
    int32_t signed_val = (int32_t)val;
    float result = (float)signed_val / 8388608.0f; // 2^23
    // ESP_LOGD(TAG, "%s: raw=0x%08X, signed_val=%d -> result=%f",
    //          __func__, (unsigned int)raw, signed_val, result);
    return result;
}

uint32_t tas5805m_float_to_q9_23(float value)
{
    if (value > 255.999999f) value = 255.999999f;
    if (value < -256.0f)     value = -256.0f;

    int32_t fixed_val = (int32_t)(value * (1 << 23));
    uint32_t le_val = tas5805m_swap_endian_32((uint32_t)fixed_val);

    // ESP_LOGD(TAG, "%s: value=%f -> fixed_val=%d, le_val=0x%08X",
    //          __func__, value, fixed_val, (unsigned int)le_val);

    return le_val;
}

float tas5805m_q5_27_to_float(uint32_t raw)
{
    uint32_t val = tas5805m_swap_endian_32(raw);
    int32_t signed_val = (int32_t)val;
    float result = (float)signed_val / 134217728.0f; // 2^27
    // ESP_LOGD(TAG, "%s: raw=0x%08X, signed_val=%d -> result=%f",
    //          __func__, (unsigned int)raw, signed_val, result);
    return result;
}

uint32_t tas5805m_float_to_q5_27(float value)
{
    if (value > 15.999999f) value = 15.999999f;
    if (value < -16.0f)     value = -16.0f;

    int32_t fixed_val = (int32_t)(value * (1 << 27));
    uint32_t le_val = tas5805m_swap_endian_32((uint32_t)fixed_val);

    // ESP_LOGD(TAG, "%s: value=%f -> fixed_val=%d, le_val=0x%08X",
    //          __func__, value, fixed_val, (unsigned int)le_val);

    return le_val;
}

// Utility: swap endian for 32-bit values
uint32_t tas5805m_swap_endian_32(uint32_t val)
{
    return ((val & 0xFF) << 24) |
           ((val & 0xFF00) << 8) |
           ((val & 0xFF0000) >> 8) |
           ((val >> 24) & 0xFF);
}
/*
 * TI ADAU1961 audio hal
 *
 * Mostly stubs (no I2C or volume control)
 * Configuration of mute/unmute gpio in init (connected to XSMT)
 */

#include "adau1961.h"
#include "adau1962_reg_addr.h"

#include <driver/gpio.h>
#include <math.h>
#include "board.h"
#include "esp_log.h"

#include "i2c_bus.h"

typedef struct adau1961_cfg_reg_s {
  uint16_t address;
  uint8_t value;
} adau1961_cfg_reg_t;

static const char *TAG = "ADAU1961";

#define ADAU1961_ASSERT(a, format, b, ...) \
  if ((a) != 0) {                          \
    ESP_LOGE(TAG, format, ##__VA_ARGS__);  \
    return b;                              \
  }

static i2c_bus_handle_t i2c_handler = NULL;

static const int adau1961_addr = CONFIG_DAC_I2C_ADDR;

// adau1961_cfg_reg_t

/*
 * i2c default configuration
 */
static i2c_config_t i2c_cfg = {
    .mode = I2C_MODE_MASTER,
    .sda_pullup_en = GPIO_PULLUP_DISABLE,
    .scl_pullup_en = GPIO_PULLUP_DISABLE,
    .master.clk_speed = 400000,
};

esp_err_t adau1961_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state);

esp_err_t adau1961_config_iface(audio_hal_codec_mode_t mode,
                                audio_hal_codec_i2s_iface_t *iface);

/*
 * Operate function
 */
audio_hal_func_t AUDIO_CODEC_ADAU1961_DEFAULT_HANDLE = {
    .audio_codec_initialize = adau1961_init,
    .audio_codec_deinitialize = adau1961_deinit,
    .audio_codec_ctrl = adau1961_ctrl,
    .audio_codec_config_iface = adau1961_config_iface,
    .audio_codec_set_mute = adau1961_set_mute,
    .audio_codec_set_volume = adau1961_set_volume,
    .audio_codec_get_volume = adau1961_get_volume,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

// static esp_err_t adau1961_transmit_registers(const adau1961_cfg_reg_t
// *conf_buf,
//                                            int size) {
//  int i = 0;
//  esp_err_t ret = ESP_OK;
//  while (i < size) {
//    ret = i2c_bus_write_bytes(i2c_handler, adau1961_addr,
//                              (unsigned char *)(&conf_buf[i].offset), 1,
//                              (unsigned char *)(&conf_buf[i].value), 1);
//    i++;
//  }
//  if (ret != ESP_OK) {
//    ESP_LOGE(TAG, "Fail to load configuration to adau1961");
//    return ESP_FAIL;
//  }
//  ESP_LOGI(TAG, "%s:  write %d reg done", __FUNCTION__, i);
//  return ret;
//}

esp_err_t adau1961_init(audio_hal_codec_config_t *codec_cfg) {
  esp_err_t ret = ESP_OK;
  uint8_t data[3];
  uint8_t verify = 0;
  uint8_t regContent;

  ESP_LOGI(TAG, "Power ON CODEC");

  // TODO: configure pins through menuconfig
  gpio_config_t cfg = {.pin_bit_mask = BIT64(GPIO_NUM_39) | BIT64(GPIO_NUM_34),
                       .mode = GPIO_MODE_DEF_INPUT,
                       .pull_up_en = GPIO_PULLUP_DISABLE,
                       .pull_down_en = GPIO_PULLDOWN_DISABLE,
                       .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&cfg);

  ret = get_i2c_pins(I2C_NUM_0, &i2c_cfg);
  i2c_handler = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
  if (i2c_handler == NULL) {
    ESP_LOGW(TAG, "failed to create i2c bus handler\n");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Looking for a adau1961 chip at address 0x%x", adau1961_addr);
  // set clock
  data[0] = (uint8_t)(R0_CLOCK_CTRL >> 8);
  data[1] = (uint8_t)R0_CLOCK_CTRL;
  data[2] = 0x01;  // core clock enable, 256 × f_s
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Found a adau1961 chip at address 0x%x", adau1961_addr);
  }
  ADAU1961_ASSERT(ret, "Fail to detect adau1961 DAC", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R0_CLOCK_CTRL >> 8);
    data[1] = (uint8_t)R0_CLOCK_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R0_CLOCK_CTRL", ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R0_CLOCK_CTRL failed, got %d", data[2]);
    }
  }

  // enable both DACs
  data[0] = (uint8_t)(R36_DAC_CTRL0 >> 8);
  data[1] = (uint8_t)R36_DAC_CTRL0;
  data[2] = 0x03;
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R36_DAC_CTRL0", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R36_DAC_CTRL0 >> 8);
    data[1] = (uint8_t)R36_DAC_CTRL0;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R36_DAC_CTRL0", ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R36_DAC_CTRL0 failed, got %d", data[2]);
    }
  }

  // enable mixer 3 and unmute left DAC input
  data[0] = (uint8_t)(R22_PLAYBACK_MIXER3_LEFT_CTRL0 >> 8);
  data[1] = (uint8_t)R22_PLAYBACK_MIXER3_LEFT_CTRL0;
  data[2] = (1 << 5) | (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R22_PLAYBACK_MIXER3_LEFT_CTRL0", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R22_PLAYBACK_MIXER3_LEFT_CTRL0 >> 8);
    data[1] = (uint8_t)R22_PLAYBACK_MIXER3_LEFT_CTRL0;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R22_PLAYBACK_MIXER3_LEFT_CTRL0",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R22_PLAYBACK_MIXER3_LEFT_CTRL0 failed, got %d",
               data[2]);
    }
  }

  // enable mixer 4 and unmute right DAC input
  data[0] = (uint8_t)(R24_PLAYBACK_MIXER4_RIGHT_CTRL0 >> 8);
  data[1] = (uint8_t)R24_PLAYBACK_MIXER4_RIGHT_CTRL0;
  data[2] = (1 << 6) | (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R24_PLAYBACK_MIXER4_RIGHT_CTRL0", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R24_PLAYBACK_MIXER4_RIGHT_CTRL0 >> 8);
    data[1] = (uint8_t)R24_PLAYBACK_MIXER4_RIGHT_CTRL0;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R24_PLAYBACK_MIXER4_RIGHT_CTRL0",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R24_PLAYBACK_MIXER4_RIGHT_CTRL0 failed, got %d",
               data[2]);
    }
  }

  //#if HEADPHONE_MODE
  // configure headphone in capless mode

  // enable mixer 7, common mode output
  data[0] = (uint8_t)(R28_PLAYBACK_LR_MIXER7_MONO_OUT_CTRL >> 8);
  data[1] = (uint8_t)R28_PLAYBACK_LR_MIXER7_MONO_OUT_CTRL;
  data[2] = (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R28_PLAYBACK_LR_MIXER7_MONO_OUT_CTRL",
                  ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R28_PLAYBACK_LR_MIXER7_MONO_OUT_CTRL >> 8);
    data[1] = (uint8_t)R28_PLAYBACK_LR_MIXER7_MONO_OUT_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R28_PLAYBACK_LR_MIXER7_MONO_OUT_CTRL",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG,
               "verify R28_PLAYBACK_LR_MIXER7_MONO_OUT_CTRL failed, got %d",
               data[2]);
    }
  }

  // headphone output, unmute mono output
  data[0] = (uint8_t)(R33_PLAYBACK_MONO_OUT_CTRL >> 8);
  data[1] = (uint8_t)R33_PLAYBACK_MONO_OUT_CTRL;
  data[2] = (1 << 1) | (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R33_PLAYBACK_MONO_OUT_CTRL", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R33_PLAYBACK_MONO_OUT_CTRL >> 8);
    data[1] = (uint8_t)R33_PLAYBACK_MONO_OUT_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R33_PLAYBACK_MONO_OUT_CTRL", ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R33_PLAYBACK_MONO_OUT_CTRL failed, got %d",
               data[2]);
    }
  }

  // unmute left headphone output, set volume to 6 dB, enable headphone output
  data[0] = (uint8_t)(R29_PLAYBACK_HP_LEFT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R29_PLAYBACK_HP_LEFT_VOL_CTRL;
  data[2] = (0 << 2) | (1 << 1) | (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R29_PLAYBACK_HP_LEFT_VOL_CTRL", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R29_PLAYBACK_HP_LEFT_VOL_CTRL >> 8);
    data[1] = (uint8_t)R29_PLAYBACK_HP_LEFT_VOL_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R29_PLAYBACK_HP_LEFT_VOL_CTRL",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R29_PLAYBACK_HP_LEFT_VOL_CTRL failed, got %d",
               data[2]);
    }
  }

  // unmute right headphone output, set volume to 6 dB, enable headphone output
  data[0] = (uint8_t)(R30_PLAYBACK_HP_RIGHT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R30_PLAYBACK_HP_RIGHT_VOL_CTRL;
  data[2] = (0 << 2) | (1 << 1) | (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R30_PLAYBACK_HP_RIGHT_VOL_CTRL", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R30_PLAYBACK_HP_RIGHT_VOL_CTRL >> 8);
    data[1] = (uint8_t)R30_PLAYBACK_HP_RIGHT_VOL_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R30_PLAYBACK_HP_RIGHT_VOL_CTRL",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R30_PLAYBACK_HP_RIGHT_VOL_CTRL failed, got %d",
               data[2]);
    }
  }

  // mixer 5 enable, The signal from the left channel playback mixer (Mixer 3)
  // can be enabled and boosted in the playback L/R mixer left (Mixer 5).
  data[0] = (uint8_t)(R26_PLAYBACK_LR_MIXER5_LEFT_LINE_OUT_CTRL >> 8);
  data[1] = (uint8_t)R26_PLAYBACK_LR_MIXER5_LEFT_LINE_OUT_CTRL;
  data[2] = (2 << 1) | (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R26_PLAYBACK_LR_MIXER5_LEFT_LINE_OUT_CTRL",
                  ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R26_PLAYBACK_LR_MIXER5_LEFT_LINE_OUT_CTRL >> 8);
    data[1] = (uint8_t)R26_PLAYBACK_LR_MIXER5_LEFT_LINE_OUT_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret,
                    "Fail to read R26_PLAYBACK_LR_MIXER5_LEFT_LINE_OUT_CTRL",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(
          TAG,
          "verify R26_PLAYBACK_LR_MIXER5_LEFT_LINE_OUT_CTRL failed, got %d",
          data[2]);
    }
  }

  // mixer 6 enable, The signal from the right channel playback mixer (Mixer 4)
  // can be enabled and boosted in the playback L/R mixer right (Mixer 6).
  data[0] = (uint8_t)(R27_PLAYBACK_LR_MIXER6_RIGHT_LINE_OUT_CTRL >> 8);
  data[1] = (uint8_t)R27_PLAYBACK_LR_MIXER6_RIGHT_LINE_OUT_CTRL;
  data[2] = (2 << 3) | (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R27_PLAYBACK_LR_MIXER6_RIGHT_LINE_OUT_CTRL",
                  ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R27_PLAYBACK_LR_MIXER6_RIGHT_LINE_OUT_CTRL >> 8);
    data[1] = (uint8_t)R27_PLAYBACK_LR_MIXER6_RIGHT_LINE_OUT_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret,
                    "Fail to read R27_PLAYBACK_LR_MIXER6_RIGHT_LINE_OUT_CTRL",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(
          TAG,
          "verify R27_PLAYBACK_LR_MIXER6_RIGHT_LINE_OUT_CTRL failed, got %d",
          data[2]);
    }
  }

  //  // mute left headphone output, disable headphone output
  //  data[0] = (uint8_t)(R29_PLAYBACK_HP_LEFT_VOL_CTRL >> 8);
  //  data[1] = (uint8_t)R29_PLAYBACK_HP_LEFT_VOL_CTRL;
  //  data[2] = 0x00;
  //  regContent = data[2];
  //  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  //  ADAU1961_ASSERT(ret, "Fail to set R29_PLAYBACK_HP_LEFT_VOL_CTRL",
  //  ESP_FAIL); if (verify) {
  //    data[0] = (uint8_t)(R29_PLAYBACK_HP_LEFT_VOL_CTRL >> 8);
  //    data[1] = (uint8_t)R29_PLAYBACK_HP_LEFT_VOL_CTRL;
  //    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2,
  //    &data[2], 1); ADAU1961_ASSERT(ret, "Fail to read
  //    R29_PLAYBACK_HP_LEFT_VOL_CTRL", ESP_FAIL); if (data[2] != regContent) {
  //      ESP_LOGE(TAG, "verify R29_PLAYBACK_HP_LEFT_VOL_CTRL failed, got %d",
  //      data[2]);
  //    }
  //  }
  //
  //  // mute right headphone output, disable headphone output
  //  data[0] = (uint8_t)(R30_PLAYBACK_HP_RIGHT_VOL_CTRL >> 8);
  //  data[1] = (uint8_t)R30_PLAYBACK_HP_RIGHT_VOL_CTRL;
  //  data[2] = 0x00;
  //  regContent = data[2];
  //  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  //  ADAU1961_ASSERT(ret, "Fail to set R30_PLAYBACK_HP_RIGHT_VOL_CTRL",
  //  ESP_FAIL); if (verify) {
  //    data[0] = (uint8_t)(R30_PLAYBACK_HP_RIGHT_VOL_CTRL >> 8);
  //    data[1] = (uint8_t)R30_PLAYBACK_HP_RIGHT_VOL_CTRL;
  //    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2,
  //    &data[2], 1); ADAU1961_ASSERT(ret, "Fail to read
  //    R30_PLAYBACK_HP_RIGHT_VOL_CTRL", ESP_FAIL); if (data[2] != regContent) {
  //      ESP_LOGE(TAG, "verify R30_PLAYBACK_HP_RIGHT_VOL_CTRL failed, got %d",
  //      data[2]);
  //    }
  //  }

  // unmute left line out channel, volume 0dB
  data[0] = (uint8_t)(R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL;
  data[2] = (0 << 2) | (1 << 1);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL",
                  ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL >> 8);
    data[1] = (uint8_t)R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL failed, got %d",
               data[2]);
    }
  }

  // unmute right line out channel, volume 0dB
  data[0] = (uint8_t)(R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL;
  data[2] = (0 << 2) | (1 << 1);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL",
                  ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL >> 8);
    data[1] = (uint8_t)R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL",
                    ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG,
               "verify R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL failed, got %d",
               data[2]);
    }
  }

  // line output, mute mono output
  data[0] = (uint8_t)(R33_PLAYBACK_MONO_OUT_CTRL >> 8);
  data[1] = (uint8_t)R33_PLAYBACK_MONO_OUT_CTRL;
  data[2] = 0x00;
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R33_PLAYBACK_MONO_OUT_CTRL", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R33_PLAYBACK_MONO_OUT_CTRL >> 8);
    data[1] = (uint8_t)R33_PLAYBACK_MONO_OUT_CTRL;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R33_PLAYBACK_MONO_OUT_CTRL", ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R33_PLAYBACK_MONO_OUT_CTRL failed, got %d",
               data[2]);
    }
  }

  // Playback right and left channel enable
  data[0] = (uint8_t)(R35_PLAYBACK_PWR_MGMT >> 8);
  data[1] = (uint8_t)R35_PLAYBACK_PWR_MGMT;
  data[2] = (1 << 1) | (1 << 0);
  regContent = data[2];
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R35_PLAYBACK_PWR_MGMT", ESP_FAIL);
  if (verify) {
    data[0] = (uint8_t)(R35_PLAYBACK_PWR_MGMT >> 8);
    data[1] = (uint8_t)R35_PLAYBACK_PWR_MGMT;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
                             1);
    ADAU1961_ASSERT(ret, "Fail to read R35_PLAYBACK_PWR_MGMT", ESP_FAIL);
    if (data[2] != regContent) {
      ESP_LOGE(TAG, "verify R35_PLAYBACK_PWR_MGMT failed, got %d", data[2]);
    }
  }

  return ret;
}

esp_err_t adau1961_get_volume(int *vol) {
  esp_err_t ret = ESP_OK;
  uint8_t data[3];
  int8_t volIndB;

  // get current register setting of left channel
  data[0] = (uint8_t)(R29_PLAYBACK_HP_LEFT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R29_PLAYBACK_HP_LEFT_VOL_CTRL;
  ret =
      i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2], 1);
  ADAU1961_ASSERT(ret, "Fail to read R29_PLAYBACK_HP_LEFT_VOL_CTRL", ESP_FAIL);

  //  volIndB = ((data[2] >> 2) & 0x3F) - 57;
  //*vol = pow(10, ((float)volIndB / 20.0)) * 50.0;

  volIndB = ((data[2] >> 2) & 0x3F);
  *vol = (int)volIndB * 100 / 63;

  // we assume all channel are set to the same volume.
  // this is a save assumption, as we are setting them in
  // adau1961_set_volume()

  /*
  // get current register setting of right channel
  data[0] = (uint8_t)(R30_PLAYBACK_HP_RIGHT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R30_PLAYBACK_HP_RIGHT_VOL_CTRL;
  ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
  1); ADAU1961_ASSERT(ret, "Fail to read R30_PLAYBACK_HP_RIGHT_VOL_CTRL",
  ESP_FAIL);

  // get current register setting of line left channel
  data[0] = (uint8_t)(R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL;
  ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
  1); ADAU1961_ASSERT(ret, "Fail to read R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL",
  ESP_FAIL);

  // get current register setting of line right channel
  data[0] = (uint8_t)(R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL;
  ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2],
  1); ADAU1961_ASSERT(ret, "Fail to read R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL",
  ESP_FAIL);
  */

  return ret;
}

esp_err_t adau1961_set_volume(int vol) {
  esp_err_t ret = ESP_OK;
  uint8_t data[3];
  uint8_t volIndB;

  volIndB = ceil((63.0 * (float)vol) / 100.0);
  if (volIndB > 63) {
    volIndB = 63;
  }

  //  if (vol == 0) {
  //    volIndB = 20 * log10(0.07/50.0) + 57.0;
  //  }
  //  else {
  //    if (vol > 100) {
  //      vol = 100;
  //    }
  //
  //    volIndB = 20.0 * log10(vol/50.0) + 57.0;
  //  }

  // get current register setting of HP left channel
  data[0] = (uint8_t)(R29_PLAYBACK_HP_LEFT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R29_PLAYBACK_HP_LEFT_VOL_CTRL;
  ret =
      i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2], 1);
  ADAU1961_ASSERT(ret, "Fail to read R29_PLAYBACK_HP_LEFT_VOL_CTRL", ESP_FAIL);
  data[2] &= ~(0x3F << 2);
  data[2] |= ((uint8_t)volIndB) << 2;
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R29_PLAYBACK_HP_LEFT_VOL_CTRL", ESP_FAIL);

  // get current register setting of HP right channel
  data[0] = (uint8_t)(R30_PLAYBACK_HP_RIGHT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R30_PLAYBACK_HP_RIGHT_VOL_CTRL;
  ret =
      i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2], 1);
  ADAU1961_ASSERT(ret, "Fail to read R30_PLAYBACK_HP_RIGHT_VOL_CTRL", ESP_FAIL);
  data[2] &= ~(0x3F << 2);
  data[2] |= ((uint8_t)volIndB) << 2;
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);

  // get current register setting of line left channel
  data[0] = (uint8_t)(R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL;
  ret =
      i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2], 1);
  ADAU1961_ASSERT(ret, "Fail to read R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL",
                  ESP_FAIL);
  data[2] &= ~(0x3F << 2);
  data[2] |= ((uint8_t)volIndB) << 2;
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R31_PLAYBACK_LINE_OUT_LEFT_VOL_CTRL",
                  ESP_FAIL);

  // get current register setting of line right channel
  data[0] = (uint8_t)(R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL >> 8);
  data[1] = (uint8_t)R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL;
  ret =
      i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2], 1);
  ADAU1961_ASSERT(ret, "Fail to read R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL",
                  ESP_FAIL);
  data[2] &= ~(0x3F << 2);
  data[2] |= ((uint8_t)volIndB) << 2;
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R32_PLAYBACK_LINE_OUT_RIGHT_VOL_CTRL",
                  ESP_FAIL);

  return ret;
}

esp_err_t adau1961_set_mute(bool enable) {
  esp_err_t ret = ESP_OK;
  uint8_t data[3];

  // left channel
  // get mixer 3 control reg
  data[0] = (uint8_t)(R22_PLAYBACK_MIXER3_LEFT_CTRL0 >> 8);
  data[1] = (uint8_t)R22_PLAYBACK_MIXER3_LEFT_CTRL0;
  ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, data, 2, &data[2], 1);
  ADAU1961_ASSERT(ret, "Fail to read R22_PLAYBACK_MIXER3_LEFT_CTRL0", ESP_FAIL);

  data[2] = (!enable << 5);
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R22_PLAYBACK_MIXER3_LEFT_CTRL0", ESP_FAIL);

  // right channel
  // get mixer 4 control reg
  data[0] = (uint8_t)(R24_PLAYBACK_MIXER4_RIGHT_CTRL0 >> 8);
  data[1] = (uint8_t)R24_PLAYBACK_MIXER4_RIGHT_CTRL0;
  ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, data, 2, &data[2], 1);
  ADAU1961_ASSERT(ret, "Fail to read R22_PLAYBACK_MIXER3_LEFT_CTRL0", ESP_FAIL);

  data[2] = (!enable << 6);
  ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
  ADAU1961_ASSERT(ret, "Fail to set R24_PLAYBACK_MIXER4_RIGHT_CTRL0", ESP_FAIL);

  return ret;
}

esp_err_t adau1961_get_mute(bool *enabled) {
  esp_err_t ret = ESP_OK;
  uint8_t data[3];

  data[0] = (uint8_t)(R35_PLAYBACK_PWR_MGMT >> 8);
  data[1] = (uint8_t)R35_PLAYBACK_PWR_MGMT;
  ret =
      i2c_bus_read_bytes(i2c_handler, adau1961_addr, &data[0], 2, &data[2], 1);
  ADAU1961_ASSERT(ret, "Fail to read R35_PLAYBACK_PWR_MGMT", ESP_FAIL);

  *enabled = ((data[2] & 0x03) == 0);

  return ret;
}

esp_err_t adau1961_deinit(void) {
  // TODO
  return ESP_OK;
}

esp_err_t adau1961_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state) 
{
  esp_err_t ret = ESP_OK;
  
  switch (mode) {
    case AUDIO_HAL_CODEC_MODE_ENCODE:
      break;
    case AUDIO_HAL_CODEC_MODE_LINE_IN:
      break;
    case AUDIO_HAL_CODEC_MODE_DECODE:
      break;
    case AUDIO_HAL_CODEC_MODE_BOTH:
      break;
    default:
      break;
  }
  
  if (AUDIO_HAL_CTRL_STOP == ctrl_state) {
    uint8_t data[3];
    
//    // first disable ADC
//    data[0] = (uint8_t)(R19_ADC_CTRL0 >> 8);
//    data[1] = (uint8_t)R19_ADC_CTRL0;
//    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, data, 2, &data[2], 1);
//    data[2] &= ~0x03;
//    ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);

    // turn off DACs
    data[0] = (uint8_t)(R36_DAC_CTRL0 >> 8);
    data[1] = (uint8_t)R36_DAC_CTRL0;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, data, 2, &data[2], 1);
    data[2] &= ~0x03;
    ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
    
    ESP_LOGI(TAG, "disable DAC");
  } else {
    uint8_t data[3];
    
    // turn on DACs
    data[0] = (uint8_t)(R36_DAC_CTRL0 >> 8);
    data[1] = (uint8_t)R36_DAC_CTRL0;
    ret = i2c_bus_read_bytes(i2c_handler, adau1961_addr, data, 2, &data[2], 1);
    data[2] |= 0x03;
    ret = i2c_bus_write_data(i2c_handler, adau1961_addr, data, 3);
    
    ESP_LOGI(TAG, "enable DAC");
  }
  
  return ret;
}

esp_err_t adau1961_config_iface(audio_hal_codec_mode_t mode,
                                audio_hal_codec_i2s_iface_t *iface) {
  // TODO
  return ESP_OK;
}

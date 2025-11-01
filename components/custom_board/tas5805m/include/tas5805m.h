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

#ifndef _TAS5805M_H_
#define _TAS5805M_H_

#include "audio_hal.h"

#include "esp_err.h"
#include "esp_log.h"
#include "board.h"

#ifdef __cplusplus
extern "C"
{
#endif


#define I2C_MASTER_FREQ_HZ          400000                     /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE   0                          /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE   0                          /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS       1000

/* Represented in % */
#define TAS5805M_VOLUME_MIN 0 
#define TAS5805M_VOLUME_MAX 100 

#define TAS5805M_VOLUME_MUTE 255
/* See here for the original Implementation : audio_hal/driver/tas5805m */
/* Its not from me it was developed by Espressif */
/* Volume steps tas5805m_volume[0] => 255 which means mute */ 
  static const uint8_t tas5805m_volume[]
    = { 0xff, 0x9f, 0x8f, 0x7f, 0x6f, 0x5f, 0x5c, 0x5a, 0x58, 0x54, 0x50,
          0x4c, 0x4a, 0x48, 0x44, 0x40, 0x3d, 0x3b, 0x39, 0x37, 0x35 };

  typedef enum
  {
    TAS5805M_CTRL_DEEP_SLEEP = 0x00, // Deep Sleep
    TAS5805M_CTRL_SLEEP = 0x01,      // Sleep
    TAS5805M_CTRL_HI_Z = 0x02,       // Hi-Z
    TAS5805M_CTRL_PLAY = 0x03,       // Play
  } TAS5805M_CTRL_STATE;

  TAS5805M_CTRL_STATE state;
  bool mute;

  /**
   * @brief Initialize TAS5805 codec chip
   *
   * @param cfg configuration of TAS5805
   *
   * @return
   *     - ESP_OK
   *     - ESP_FAIL
   */
  esp_err_t tas5805m_init ();

  /**
   * @brief Deinitialize TAS5805 codec chip
   *
   * @return
   *     - ESP_OK
   *     - ESP_FAIL
   */
  esp_err_t tas5805m_deinit (void);

  /**
   * @brief  Set voice volume
   *
   * @param volume:  voice volume (0~100)
   *
   * @return
   *     - ESP_OK
   *     - ESP_FAIL
   */
  esp_err_t tas5805m_set_volume (int vol);

  /**
   * @brief Get voice volume
   *
   * @param[out] *volume:  voice volume (0~100)
   *
   * @return
   *     - ESP_OK
   *     - ESP_FAIL
   */
  esp_err_t tas5805m_get_volume(int *vol);

  /**
   * @brief Set TAS5805 mute or not
   *        Continuously call should have an interval time determined by
   * tas5805m_set_mute_fade()
   *
   * @param enable enable(1) or disable(0)
   *
   * @return
   *     - ESP_FAIL Parameter error
   *     - ESP_OK   Success
   */
  esp_err_t tas5805m_set_mute (bool enable);

  /**
   * @brief Mute TAS5805M
   *
   * @param value  Time for mute with millisecond.
   * @return
   *     - ESP_FAIL Parameter error
   *     - ESP_OK   Success
   *
   */
  

  /**
   * @brief Get TAS5805 mute status
   *
   *  @return
   *     - ESP_FAIL Parameter error
   *     - ESP_OK   Success
   */
  esp_err_t tas5805m_get_mute (bool *enabled);

  esp_err_t tas5805m_ctrl(audio_hal_codec_mode_t mode,
                        audio_hal_ctrl_t ctrl_state);


  esp_err_t tas5805m_config_iface(audio_hal_codec_mode_t mode,
                               audio_hal_codec_i2s_iface_t *iface);

#ifdef __cplusplus
}
#endif

#endif

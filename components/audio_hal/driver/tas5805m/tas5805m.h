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

#ifdef __cplusplus
extern "C"
{
#endif


  /**
   * @brief Initialize TAS5805 codec chip
   *
   * @param cfg configuration of TAS5805
   *
   * @return
   *     - ESP_OK
   *     - ESP_FAIL
   */
  esp_err_t tas5805m_init (audio_hal_codec_config_t *codec_cfg);

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
  esp_err_t tas5805m_get_volume (int *volume);

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
   * @brief Mute gradually by (value)ms
   *
   * @param value  Time for mute with millisecond.
   * @return
   *     - ESP_FAIL Parameter error
   *     - ESP_OK   Success
   *
   */
  esp_err_t tas5805m_set_mute_fade (int value);

  /**
   * @brief Get TAS5805 mute status
   *
   *  @return
   *     - ESP_FAIL Parameter error
   *     - ESP_OK   Success
   */
  esp_err_t tas5805m_get_mute (int *value);

  /**
   * @brief Set DAMP mode
   *
   * @param value  TAS5805M_DAMP_MODE_BTL or TAS5805M_DAMP_MODE_PBTL
   * @return
   *     - ESP_FAIL Parameter error
   *     - ESP_OK   Success
   *
   */
  esp_err_t tas5805m_set_damp_mode (int value);

#ifdef __cplusplus
}
#endif

#endif

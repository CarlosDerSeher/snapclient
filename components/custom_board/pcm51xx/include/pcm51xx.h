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

#ifndef _PCM51XX_H_
#define _PCM51XX_H_

#include "audio_hal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCM51XX_REG_00 0x00
#define PCM51XX_REG_02 0x02
#define PCM51XX_REG_03 0x03
#define PCM51XX_REG_24 0x24
#define PCM51XX_REG_25 0x25
#define PCM51XX_REG_26 0x26
#define PCM51XX_REG_27 0x27
#define PCM51XX_REG_28 0x28
#define PCM51XX_REG_29 0x29
#define PCM51XX_REG_2A 0x2a
#define PCM51XX_REG_2B 0x2b
#define PCM51XX_REG_35 0x35
#define PCM51XX_REG_7E 0x7e
#define PCM51XX_REG_7F 0x7f

#define PCM51XX_PAGE_00 0x00
#define PCM51XX_PAGE_2A 0x2a

#define PCM51XX_BOOK_00 0x00
#define PCM51XX_BOOK_8C 0x8c

#define PCM51XX_REG_VOL_L 0X3D
#define PCM51XX_REG_VOL_R 0X3E
#define PCM51XX_REG_MUTE 0X03

#define PCM51XX_DAMP_MODE_BTL 0x0
#define PCM51XX_DAMP_MODE_PBTL 0x04

/**
 * @brief Initialize TAS5805 codec chip
 *
 * @param cfg configuration of TAS5805
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t pcm51xx_init(audio_hal_codec_config_t *codec_cfg);

/**
 * @brief Deinitialize TAS5805 codec chip
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t pcm51xx_deinit(void);

/**
 * @brief  Set voice volume
 *
 * @param volume:  voice volume (0~100)
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t pcm51xx_set_volume(int vol);

/**
 * @brief Get voice volume
 *
 * @param[out] *volume:  voice volume (0~100)
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t pcm51xx_get_volume(int *value);

/**
 * @brief Set TAS5805 mute or not
 *        Continuously call should have an interval time determined by
 * pcm51xx_set_mute_fade()
 *
 * @param enable enable(1) or disable(0)
 *
 * @return
 *     - ESP_FAIL Parameter error
 *     - ESP_OK   Success
 */
esp_err_t pcm51xx_set_mute(bool enable);

/**
 * @brief Get TAS5805 mute status
 *
 *  @return
 *     - ESP_FAIL Parameter error
 *     - ESP_OK   Success
 */
esp_err_t pcm51xx_get_mute(bool *enabled);

/**
 * @brief Control codec mode
 *
 * @param mode codec mode
 * @param ctrl_state control state
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t pcm51xx_ctrl(audio_hal_codec_mode_t mode,
                       audio_hal_ctrl_t ctrl_state);

/**
 * @brief Configure codec interface
 *
 * @param mode codec mode
 * @param iface I2S interface configuration
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t pcm51xx_config_iface(audio_hal_codec_mode_t mode,
                               audio_hal_codec_i2s_iface_t *iface);

#ifdef __cplusplus
}
#endif

#endif

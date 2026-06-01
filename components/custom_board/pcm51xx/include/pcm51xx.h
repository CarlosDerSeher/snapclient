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
#define PCM51XX_REG_RESET 0x01
#define PCM51XX_REG_PMOD 0x02
#define PCM51XX_REG_MUTE 0x03
#define PCM51XX_REG_PLL 0x0d
#define PCM51XX_REG_X16INTP 0x22
#define PCM51XX_REG_CLKDET 0x25
#define PCM51XX_REG_26 0x26
#define PCM51XX_REG_27 0x27
#define PCM51XX_REG_28 0x28
#define PCM51XX_REG_29 0x29
#define PCM51XX_REG_DATA_PATH 0x2a
#define PCM51XX_REG_PROCESS_FLOW 0x2b
#define PCM51XX_REG_35 0x35
#define PCM51XX_REG_7E 0x7e
#define PCM51XX_REG_7F 0x7f

#define PCM51XX_PAGE_00 0x00
#define PCM51XX_PAGE_2A 0x2a

#define PCM51XX_BOOK_00 0x00
#define PCM51XX_BOOK_8C 0x8c

#define PCM51XX_REG_VOL_L 0X3D
#define PCM51XX_REG_VOL_R 0X3E

#define PCM51XX_DAMP_MODE_BTL 0x0
#define PCM51XX_DAMP_MODE_PBTL 0x04

/* ------------------------------------------------------------------
 * Register 2 (0x02, page 0) — PMOD: Power mode control
 *
 * Bit 4 (RQST): Request standby state transition.
 * Bit 0 (RQPD): Request power-down state transition.
 *
 * DSP coefficient RAM must be written while the device is in standby
 * or power-down mode (same requirement as the TI PPC3 init sequence:
 * coefficients are loaded before the final "Exit Stand-by" write).
 * ------------------------------------------------------------------*/
#define PCM51XX_PMOD_NORMAL    0x00u   /* normal operation              */
#define PCM51XX_PMOD_STANDBY   0x10u   /* request standby    (RQST=1, bit 4) */
#define PCM51XX_PMOD_POWERDOWN 0x01u   /* request power-down (RQPD=1, bit 0) */

/* ------------------------------------------------------------------
 * Register 43 (0x2B, page 0) — PDSP: Process flow selection
 *
 * Selects which signal processing path is active.  Only flows 0 and 5
 * are well-documented; the others are listed for completeness.
 *
 * PDSP[2:0] | Description
 * ----------|-----------------------------------------------------------
 *  000      | Flow 0 — Standard path, programmable biquads NOT applied
 *  001      | Flow 1 — Post-process path (after interpolation filter)
 *  010      | Flow 2 — Pre-process path  (before interpolation filter)
 *  011      | Flow 3 — Pre- AND post-process path
 *  100      | Flow 4 — (reserved / not documented)
 *  101      | Flow 5 — Fixed process flow: c10-c39 BQ coefficients active
 *  110–111  | Reserved
 * ------------------------------------------------------------------*/
#define PCM51XX_PROC_FLOW_0      0x00u  /* Reserved (do not set) */
#define PCM51XX_PROC_FLOW_1      0x01u  /* 8x/4x/2x FIR interpolation filter with de-emphasis  */
#define PCM51XX_PROC_FLOW_2      0x02u  /* 8x/4x/2x Low latency IIR interpolation filter with de-emphasis   */
#define PCM51XX_PROC_FLOW_3      0x03u  /* High attenuation x8/x4/x2 interpolation filter with de-emphasis */
#define PCM51XX_PROC_FLOW_4      0x04u  /* Reserved           */
#define PCM51XX_PROC_FLOW_5      0x05u  /* Fixed process flow with configurable parameters */
#define PCM51XX_PROC_FLOW_6      0x06u  /* Reserved (do not set) */
#define PCM51XX_PROC_FLOW_7      0x07u  /* 8x Ringing-less low latency FIR interpolation filter without de-emphasis */

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

#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)

#include "pcm51xx_eq_config.h"

/**
 * @brief Convert a floating-point coefficient to PCM5122 Q3.23 DSP format.
 *
 * The PCM5122 stores biquad coefficients as 32-bit two’s complement values
 * as 32-bit words where bits [31:8] hold a 24-bit signed integer (range
 * approximately (-1, +1)); bits [7:0] are always 0x00 (hardware ignores them).
 * float = bits[31:8] / 2^23; unity b0 ≈ 0x7FFFFF00 (= 0.9999999, closest
 * representable value to 1.0).  The returned uint32_t can be written directly
 * via i2c_bus_write_bytes with big-endian byte extraction.
 *
 * @param value  Floating-point coefficient value.
 * @return       PCM5122 I2C-ready 32-bit Q3.23 representation.
 */
uint32_t pcm51xx_float_to_q3_23(float value);

/**
 * @brief Convert a PCM5122 raw Q3.23 register word back to float.
 *
 * @param raw  uint32_t as returned by pcm51xx_read_biquad_coefficients.
 * @return     Floating-point coefficient value.
 */
float pcm51xx_q3_23_to_float(uint32_t raw);

/**
 * @brief Get the current parametric EQ gain for a band.
 *
 * Returns the cached gain value last set with pcm51xx_set_eq_gain().
 *
 * @param band   Band index (0 .. PCM51XX_EQ_BANDS-1).
 * @param[out] gain  Current gain in dB.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad band index.
 */
esp_err_t pcm51xx_get_eq_gain(int band, int *gain);

/**
 * @brief Set the parametric EQ gain for a band and update DSP coefficients.
 *
 * Computes biquad coefficients on-the-fly using the bq_calc library,
 * converts them to PCM5122 Q1.23 format, and writes them to both the
 * A-buffer and B-buffer of the DSP coefficient RAM.
 *
 * PCM5122 uses identical coefficients for both channels — there is no
 * separate left/right channel parameter.
 *
 * @param band  Band index (0 .. PCM51XX_EQ_BANDS-1).
 * @param gain  Desired gain in dB (PCM51XX_EQ_MIN_DB .. PCM51XX_EQ_MAX_DB).
 * @return ESP_OK on success.
 */
esp_err_t pcm51xx_set_eq_gain(int band, int gain);

/**
 * @brief Read raw biquad coefficients from the DSP A-buffer for a band.
 *
 * @param band         Band index (0 .. PCM51XX_EQ_BANDS-1).
 * @param[out] coeffs  Array of PCM51XX_EQ_KOEF_PER_BAND uint32_t values
 *                     in I2C wire format (B0, B1, B2, A1, A2).
 * @return ESP_OK on success.
 */
esp_err_t pcm51xx_read_biquad_coefficients(int band, uint32_t coeffs[PCM51XX_EQ_KOEF_PER_BAND]);

/**
 * @brief Write raw biquad coefficients to both DSP buffers for a band.
 *
 * Updates both A-buffer and B-buffer to ensure consistency across
 * PCM5122 adaptive double-buffering.
 *
 * @param band        Band index (0 .. PCM51XX_EQ_BANDS-1).
 * @param coeffs      Array of PCM51XX_EQ_KOEF_PER_BAND uint32_t values
 *                    in I2C wire format (B0, B1, B2, A1, A2).
 * @return ESP_OK on success.
 */
esp_err_t pcm51xx_write_biquad_coefficients(int band, const uint32_t coeffs[PCM51XX_EQ_KOEF_PER_BAND]);

#endif /* CONFIG_DAC_PCM51XX_EQ_SUPPORT */

#ifdef __cplusplus
}
#endif

#endif

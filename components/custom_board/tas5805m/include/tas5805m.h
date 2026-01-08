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

#include "board.h"
#include "esp_err.h"
#include "esp_log.h"

#include "tas5805m_types.h"

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
#include "tas5805m_eq.h"
#include "tas5805m_eq_profiles.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_MASTER_FREQ_HZ 400000	/*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS 1000

/* Cached state structure */
typedef struct {
	int8_t volume;
	TAS5805M_CTRL_STATE state;
	TAS5805M_MIXER_MODE mixer_mode;

	/* Cached per-output channel gain in dB (-24..24), default 0 dB */
	int8_t channel_gain_l;
	int8_t channel_gain_r;

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
	int8_t eq_gain_l[TAS5805M_EQ_BANDS];
	int8_t eq_gain_r[TAS5805M_EQ_BANDS];
	TAS5805M_EQ_PROFILE eq_profile[2];
#endif
} TAS5805_STATE;

/* @brief Initialize TAS5805 codec chip
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_init();

/**
 * @brief Deinitialize TAS5805 codec chip
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_deinit(void);

/**
 * @brief  Set voice volume
 *
 * @param volume:  voice volume (0~100)
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_volume(int vol);

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
 * @brief  Set device volume)
 *
 * @param volume: digital volume (inverted) (0~255)
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_digital_volume(uint8_t vol);

/**
 * @brief Get device volume
 *
 * @param[out] *volume: digital volume (inverted) (0~255)
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_get_digital_volume(uint8_t *vol);

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
esp_err_t tas5805m_set_mute(bool enable);

/**
 * @brief Get TAS5805 mute status
 *
 *  @return
 *     - ESP_FAIL Parameter error
 *     - ESP_OK   Success
 */
esp_err_t tas5805m_get_mute(bool *enabled);

/**
 * @brief Get cached TAS5805 state
 *
 * @param[out] out_state pointer to TAS5805_STATE to receive cached values
 * @return ESP_OK or error
 */
esp_err_t tas5805m_get_state(TAS5805_STATE *out_state);

/**
 * @brief Set the state of the TAS5805M
 *
 * @param state: The state to set
 *
 */
esp_err_t tas5805m_set_state(TAS5805M_CTRL_STATE state);

/**
 * @brief  Control the TAS5805 codec chip
 *
 * @param mode: codec mode
 * @param ctrl_state: control state
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */	
esp_err_t tas5805m_ctrl(audio_hal_codec_mode_t mode,
						audio_hal_ctrl_t ctrl_state);

/**
 * @brief  Configure the I2S interface of TAS5805 codec chip
 * @param mode: codec mode
 * @param iface: I2S interface configuration		
 * @return
 *    - ESP_OK
 * 	- ESP_FAIL
 */	
esp_err_t tas5805m_config_iface(audio_hal_codec_mode_t mode,
								audio_hal_codec_i2s_iface_t *iface);

/**
 * @brief Get the current DAC mode of the TAS5805M
 *
 * @param mode: Pointer to the mode variable
 *
 */
esp_err_t tas5805m_get_dac_mode(TAS5805M_DAC_MODE *mode);

/**
 * @brief Set the DAC mode of the TAS5805M
 *
 * @param mode: The mode to set
 *
 */
esp_err_t tas5805m_set_dac_mode(TAS5805M_DAC_MODE mode);

/**
 * @brief Get the current modulation mode of the TAS5805M
 *
 * @param mode: Pointer to the mode variable
 * @param freq: Pointer to the DSP frequency variable
 * @param bd_freq: Pointer to the BD frequency variable
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_get_modulation_mode(TAS5805M_MOD_MODE *mode,
									   TAS5805M_SW_FREQ *freq,
									   TAS5805M_BD_FREQ *bd_freq);

/**
 * @brief Set the modulation mode of the TAS5805M
 *
 * @param mode: The mode to set
 * @param freq: The DSP frequency to set
 * @param bd_freq: The BD frequency to set
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_modulation_mode(TAS5805M_MOD_MODE mode,
									   TAS5805M_SW_FREQ freq,
									   TAS5805M_BD_FREQ bd_freq);

/**
 * @brief Get the analog gain of the TAS5805M
 *
 * @param gain: Pointer to the gain variable
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_get_again(uint8_t *gain);

/**
 * @brief Set the analog gain of the TAS5805M
 *
 * @param gain: The gain to set
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_again(uint8_t gain);

/**
 * @brief Get the mixer mode of the TAS5805M
 *
 * @param mode: Pointer to the mode variable
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_get_mixer_mode(TAS5805M_MIXER_MODE *mode);

/**
 * @brief Set the mixer mode of the TAS5805M
 *
 * @param mode: The mode to set
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_mixer_mode(TAS5805M_MIXER_MODE mode);

/**
 * @brief Set the mixer gain of the TAS5805M
 * (4-bytes value, representing decimal in 9.23 format)
 *
 * @param channel: The channel to set the gain for
 * @param gain: The gain to set
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_mixer_gain(TAS5805M_MIXER_CHANNELS channel,
								  uint32_t gain);

/**
 * @brief Set the mixer gain of the TAS5805M
 * (4-bytes value, representing decimal in 9.23 format)
 *
 * @param channel: The channel to set the gain for
 * @param gain: The gain to set
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_channel_gain(TAS5805M_EQ_CHANNELS channel,
								   int8_t gain_db);

/** Get cached per-output channel gain (dB) */
esp_err_t tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS channel, int8_t *gain_db);

/**
 * @brief Get the faults of the TAS5805M
 *
 * @param fault: Pointer to the fault struct
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_get_faults(TAS5805M_FAULT *fault);

/**
 * @brief Clear the faults of the TAS5805M
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_clear_faults();

/**
 * @brief Decode the errors from the TAS5805M
 *
 * @param fault: The fault struct to decode
 *
 */
void tas5805m_decode_faults(TAS5805M_FAULT fault);

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)

/**
 * @brief Get the current EQ mode of the TAS5805M
 *
 * @param mode: Pointer to the mode variable
 *
 */
esp_err_t tas5805m_get_eq_mode(TAS5805M_EQ_MODE *mode);

/**
 * @brief Set the EQ mode of the TAS5805M
 *
 * @param mode: The mode to set
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_eq_mode(TAS5805M_EQ_MODE mode);

/**
 * @brief Get the current EQ gain of the TAS5805M for LEFT channel (applies to
 * both channels, when configures as mirror)
 *
 * @param band: The band to get the gain of
 * @param gain: Pointer to the gain variable
 *
 */
esp_err_t tas5805m_get_eq_gain(int band, int *gain);

/**
 * @brief Set the EQ gain of the TAS5805M for selected channel
 *
 * @param band: The band to set the gain of
 * @param gain: The gain to set
 * @param channel: The channel to set the gain for
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS channel, int band,
									   int *gain);

/**
 * @brief Set the EQ gain of the TAS5805M for LEFT channel (applies to both
 * channels, when configures as mirror)
 *
 * @param band: The band to set the gain of
 * @param gain: The gain to set
 *
 */
esp_err_t tas5805m_set_eq_gain(int band, int gain);

/**
 * @brief Set the EQ gain of the TAS5805M for selected channel
 *
 * @param band: The band to set the gain of
 * @param gain: The gain to set
 * @param channel: The channel to set the gain for
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS channel, int band,
									   int gain);

/**
 * @brief Get the current EQ profile of the TAS5805M
 *
 * @param profile: Pointer to the profile variable
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_get_eq_profile(TAS5805M_EQ_PROFILE *profile);

/**
 * @brief Get the EQ profile of the TAS5805M for a specific channel
 *
 * @param profile: Pointer to the profile variable
 * @param channel: The channel to get the profile for
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_get_eq_profile_channel(TAS5805M_EQ_CHANNELS channel,
										  TAS5805M_EQ_PROFILE *profile);

/**
 * @brief Set the EQ profile of the TAS5805M
 *
 * @param profile: The EQ profile to set
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_eq_profile(TAS5805M_EQ_PROFILE profile);

/**
 * @brief Set the EQ profile of the TAS5805M for a specific channel
 *
 * @param profile: The EQ profile to set
 * @param channel: The channel to set the profile for
 *
 * @return
 *     - ESP_OK
 *     - ESP_FAIL
 */
esp_err_t tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS channel,
										  TAS5805M_EQ_PROFILE profile);

/**
 * @brief Read biquad filter coefficients for a specific channel and band.
 *
 * Reads the 5 biquad coefficients (B0, B1, B2, A1, A2) from the DSP registers
 * and converts them from Q5.27 format to float values.
 *
 * @param channel Left or right channel
 * @param band Band index (0-14)
 * @param b0 Output: B0 coefficient (feedforward)
 * @param b1 Output: B1 coefficient (feedforward)
 * @param b2 Output: B2 coefficient (feedforward)
 * @param a1 Output: A1 coefficient (feedback)
 * @param a2 Output: A2 coefficient (feedback)
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if parameters are NULL or band is out of range
 *     - ESP_FAIL on I2C communication error
 */
esp_err_t tas5805m_read_biquad_coefficients(TAS5805M_EQ_CHANNELS channel, int band,
                                             float *b0, float *b1, float *b2,
                                             float *a1, float *a2);

/**
 * @brief Write biquad filter coefficients for a specific channel and band.
 *
 * Converts float coefficients to Q5.27 format and writes them to the DSP registers.
 *
 * @param channel Left or right channel
 * @param band Band index (0-14)
 * @param b0 B0 coefficient (feedforward)
 * @param b1 B1 coefficient (feedforward)
 * @param b2 B2 coefficient (feedforward)
 * @param a1 A1 coefficient (feedback)
 * @param a2 A2 coefficient (feedback)
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if band is out of range
 *     - ESP_FAIL on I2C communication error
 */
esp_err_t tas5805m_write_biquad_coefficients(TAS5805M_EQ_CHANNELS channel, int band,
                                              float b0, float b1, float b2,
                                              float a1, float a2);

#endif /* CONFIG_DAC_TAS5805M_EQ_SUPPORT */

/**
 * @brief Swap the endianness of a 32-bit integer.
 *
 * @param val Input 32-bit integer.
 * @return 32-bit integer with swapped endianness.
 */
uint32_t tas5805m_swap_endian_32(uint32_t val);

/**
 * @brief Convert a Q9.23 fixed-point value to a float.
 *
 * @param raw Q9.23 value as a 32-bit unsigned integer.
 * @return float-precision floating point representation.
 */
float tas5805m_q9_23_to_float(uint32_t raw);

/**
 * @brief Convert a float to a Q9.23 fixed-point value.
 *
 * @param value float-precision floating point input.
 * @return Q9.23 fixed-point value as a 32-bit unsigned integer.
 */
uint32_t tas5805m_float_to_q9_23(float value);

/**
 * @brief Convert a Q5.27 fixed-point value to a float.
 *
 * @param raw Q5.27 value as a 32-bit unsigned integer.
 * @return float-precision floating point representation.
 */
float tas5805m_q5_27_to_float(uint32_t raw);

/**
 * @brief Convert a float to a Q5.27 fixed-point value.
 *
 * @param value float-precision floating point input.
 * @return Q5.27 fixed-point value as a 32-bit unsigned integer.
 */
uint32_t tas5805m_float_to_q5_27(float value);

#ifdef __cplusplus
}
#endif

#endif

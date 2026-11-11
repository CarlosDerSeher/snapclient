/**
 * @file tas5805m_biamp.h
 * @brief Advanced Bi-Amp Crossover DSP for TAS5805M
 *
 * This module provides biquad filter coefficient calculations and hardware
 * programming for active crossover configurations on the TAS5805M DAC.
 *
 * @section biamp_overview Overview
 *
 * In bi-amp mode, the TAS5805M's stereo outputs are repurposed:
 * - Left channel  -> Lowpass filter  -> Woofer amplifier
 * - Right channel -> Highpass filter -> Tweeter amplifier
 *
 * @section biamp_filters Crossover Filter Types
 *
 * Butterworth:
 * - -3dB at crossover frequency
 * - Maximally flat passband
 * - 180° phase shift at crossover (12dB/oct), 360° (24dB/oct)
 *
 * Linkwitz-Riley:
 * - -6dB at crossover frequency
 * - Flat summed amplitude response when drivers are in-phase
 * - Created by cascading two Butterworth filters
 *
 * @section biamp_slopes Supported Slopes
 *
 * - 12 dB/octave: 1 biquad stage (2nd order)
 * - 24 dB/octave: 2 biquad stages (4th order, standard LR)
 * - 48 dB/octave: 4 biquad stages (8th order)
 *
 * @section biamp_bands Biquad Band Allocation
 *
 * Each channel has 15 biquad bands (0-14). Bi-amp mode allocates them as:
 *
 * LEFT (Woofer):                      RIGHT (Tweeter):
 * - Band 0:  Gain + phase             - Band 0:  Gain + phase
 * - Band 1:  Subsonic HPF             - Band 1:  Time alignment delay
 * - Bands 2-5: Lowpass crossover      - Bands 2-5: Highpass crossover
 * - Bands 6-11: Parametric EQ         - Bands 6-11: Parametric EQ
 * - Band 12: Baffle step compensation - Band 12: Breakup notch filter
 * - Band 13: Bass shelf (loudness)    - Band 13: Air/brilliance shelf
 * - Band 14: Spare (loudness treble)  - Band 14: Treble shelf (loudness)
 */

#ifndef __TAS5805M_BIAMP_H__
#define __TAS5805M_BIAMP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#if CONFIG_DAC_TAS5805M

#include "tas5805m_settings.h"
#include "tas5805m_types.h"

/**
 * @brief Biquad filter coefficient structure
 *
 * Standard direct form I biquad transfer function:
 * H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
 *
 * Note: a0 is normalized to 1.0 and not stored.
 */
typedef struct {
    float b0;  /**< Feedforward coefficient 0 (input gain) */
    float b1;  /**< Feedforward coefficient 1 */
    float b2;  /**< Feedforward coefficient 2 */
    float a1;  /**< Feedback coefficient 1 */
    float a2;  /**< Feedback coefficient 2 */
} tas5805m_biquad_coeffs_t;

/**
 * @brief Supported sample rates for coefficient calculations
 */
typedef enum {
    BIAMP_SAMPLE_RATE_44100 = 44100,
    BIAMP_SAMPLE_RATE_48000 = 48000,
    BIAMP_SAMPLE_RATE_88200 = 88200,
    BIAMP_SAMPLE_RATE_96000 = 96000,
} tas5805m_biamp_sample_rate_t;

/** Default sample rate used when none specified */
#define TAS5805M_BIAMP_DEFAULT_SAMPLE_RATE BIAMP_SAMPLE_RATE_48000

/* ============ Biquad Band Assignments ============ */

#define BIAMP_GAIN_BAND             0   /**< Gain and phase control */
#define BIAMP_SUBSONIC_BAND         1   /**< Subsonic HPF (woofer channel) */
#define BIAMP_TWEETER_DELAY_BAND    1   /**< Time alignment all-pass (tweeter channel) */
#define BIAMP_CROSSOVER_START_BAND  2   /**< First crossover filter band */
#define BIAMP_CROSSOVER_MAX_BANDS   4   /**< Maximum crossover stages (48dB/oct) */
#define BIAMP_PEQ_START_BAND        6   /**< First parametric EQ band */
#define BIAMP_PEQ_BANDS             6   /**< Number of PEQ bands per channel */
#define BIAMP_BAFFLE_STEP_BAND      12  /**< Baffle step compensation (woofer) */
#define BIAMP_NOTCH_BAND            12  /**< Breakup notch filter (tweeter) */
#define BIAMP_AIR_SHELF_BAND        13  /**< Air/brilliance shelf (tweeter) */

/* ============ Filter Coefficient Calculators ============ */

/**
 * @brief Calculate 2nd-order Butterworth lowpass filter coefficients
 *
 * Butterworth filters have maximally flat passband response.
 * Uses bilinear transform with frequency pre-warping.
 *
 * @param fc     Cutoff frequency in Hz (-3dB point)
 * @param fs     Sample rate in Hz
 * @param coeffs Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_butterworth_lpf(float fc, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate 2nd-order Butterworth highpass filter coefficients
 *
 * @param fc     Cutoff frequency in Hz (-3dB point)
 * @param fs     Sample rate in Hz
 * @param coeffs Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_butterworth_hpf(float fc, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate parametric EQ (peaking) filter coefficients
 *
 * Creates a bell-shaped boost or cut centered at the specified frequency.
 * Based on the Audio EQ Cookbook by Robert Bristow-Johnson.
 *
 * @param fc      Center frequency in Hz
 * @param gain_db Gain in dB (negative for cut, positive for boost)
 * @param q       Q factor controlling bandwidth (higher = narrower)
 * @param fs      Sample rate in Hz
 * @param coeffs  Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_peq(float fc, float gain_db, float q, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate gain stage coefficients
 *
 * Creates a simple gain/attenuation with no frequency shaping.
 * Implemented as b0 = linear_gain, all other coefficients zero.
 *
 * @param gain_db Gain in dB
 * @param coeffs  Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if coeffs is NULL
 */
esp_err_t tas5805m_calc_gain(float gain_db, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate unity passthrough coefficients
 *
 * Creates coefficients that pass the signal unchanged (b0=1, all others=0).
 * Use this to disable a biquad band without affecting the signal.
 *
 * @param coeffs Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if coeffs is NULL
 */
esp_err_t tas5805m_calc_passthrough(tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate phase inversion coefficients
 *
 * Creates coefficients that invert the signal polarity (b0=-1).
 * Equivalent to 180° phase shift at all frequencies.
 *
 * @param coeffs Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if coeffs is NULL
 */
esp_err_t tas5805m_calc_phase_invert(tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate low shelf filter coefficients
 *
 * Boosts or cuts frequencies below the shelf frequency.
 * Based on the Audio EQ Cookbook by Robert Bristow-Johnson.
 *
 * @param fc      Shelf transition frequency in Hz
 * @param gain_db Gain in dB for frequencies below fc
 * @param fs      Sample rate in Hz
 * @param coeffs  Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_low_shelf(float fc, float gain_db, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate high shelf filter coefficients
 *
 * Boosts or cuts frequencies above the shelf frequency.
 * Based on the Audio EQ Cookbook by Robert Bristow-Johnson.
 *
 * @param fc      Shelf transition frequency in Hz
 * @param gain_db Gain in dB for frequencies above fc
 * @param fs      Sample rate in Hz
 * @param coeffs  Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_high_shelf(float fc, float gain_db, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Get the number of biquad stages required for a crossover slope
 *
 * @param slope Crossover slope setting (12/24/48 dB/octave)
 * @return Number of cascaded biquad stages (1, 2, or 4)
 */
int tas5805m_biamp_get_biquad_count(tas5805m_biamp_slope_t slope);

/**
 * @brief Calculate baffle step compensation parameters
 *
 * Baffle step is the acoustic phenomenon where low frequencies diffract
 * around the speaker cabinet while high frequencies beam forward. This
 * causes a 6dB level difference between low and high frequencies.
 *
 * The transition frequency depends on baffle width:
 *   f_step = speed_of_sound / (π × baffle_width)
 *
 * Compensation amount depends on room placement:
 * - Freestanding: +6dB bass boost (no boundary reinforcement)
 * - Near wall:    +3dB bass boost (partial reinforcement)
 * - Corner:        0dB (full boundary reinforcement)
 *
 * @param baffle_width_cm Width of speaker baffle in centimeters
 * @param placement       Room placement affecting compensation amount
 * @param step_freq_hz    Output: calculated shelf frequency
 * @param gain_db         Output: calculated bass boost amount
 */
void tas5805m_calc_baffle_step(uint8_t baffle_width_cm,
                                tas5805m_baffle_placement_t placement,
                                float *step_freq_hz, float *gain_db);

/**
 * @brief Calculate first-order all-pass filter for time alignment
 *
 * All-pass filters pass all frequencies at unity gain but introduce
 * frequency-dependent phase shift (group delay). This delays the tweeter
 * signal to align with the woofer's acoustic center.
 *
 * The delay is approximate and most accurate near the crossover frequency
 * where driver alignment matters most.
 *
 * @param delay_mm Physical distance to delay in millimeters
 * @param fs       Sample rate in Hz
 * @param coeffs   Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_allpass_delay(uint8_t delay_mm, float fs, tas5805m_biquad_coeffs_t *coeffs);

/* ============ Full Configuration Apply ============ */

/**
 * @brief Apply complete bi-amp crossover configuration
 *
 * Programs all biquad bands on both channels with the crossover filters,
 * gain stages, PEQ, and speaker compensation filters. This is the main
 * entry point for configuring bi-amp mode.
 *
 * @param settings Complete bi-amp configuration structure
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tas5805m_biamp_apply(const tas5805m_biamp_settings_t *settings);

/**
 * @brief Initialize bi-amp settings structure to defaults
 *
 * Sets sensible defaults:
 * - 2000 Hz crossover, 24dB/oct Linkwitz-Riley
 * - 0dB gain, no phase invert on both outputs
 * - All PEQ bands disabled
 * - No speaker compensation filters
 *
 * @param settings Structure to initialize
 */
void tas5805m_biamp_init_defaults(tas5805m_biamp_settings_t *settings);

/* ============ Individual Band Apply Functions ============ */

/**
 * @brief Apply woofer (low output) gain and phase
 *
 * Updates only band 0 on the left channel. Use this for efficient
 * single-parameter updates instead of full re-apply.
 *
 * @param gain_x2      Gain × 2 for 0.5dB resolution (-48 to +48 = -24dB to +24dB)
 * @param phase_invert 0 = normal polarity, 1 = inverted (180° phase)
 * @param sample_rate  Current sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t tas5805m_biamp_apply_low_gain_phase(int8_t gain_x2, uint8_t phase_invert, uint32_t sample_rate);

/**
 * @brief Apply tweeter (high output) gain and phase
 *
 * Updates only band 0 on the right channel.
 *
 * @param gain_x2      Gain × 2 for 0.5dB resolution
 * @param phase_invert 0 = normal polarity, 1 = inverted
 * @param sample_rate  Current sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t tas5805m_biamp_apply_high_gain_phase(int8_t gain_x2, uint8_t phase_invert, uint32_t sample_rate);

/**
 * @brief Apply subsonic highpass filter
 *
 * Protects the woofer from excessive excursion at very low frequencies.
 * Updates band 1 on the left channel.
 *
 * @param freq        Cutoff frequency in Hz (0 = disabled/passthrough)
 * @param sample_rate Current sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t tas5805m_biamp_apply_subsonic(uint16_t freq, uint32_t sample_rate);

/**
 * @brief Apply tweeter time alignment delay
 *
 * Uses an all-pass filter to delay the tweeter and align it with the
 * woofer's acoustic center. Updates band 1 on the right channel.
 *
 * @param delay_mm    Delay distance in millimeters (0 = disabled)
 * @param sample_rate Current sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t tas5805m_biamp_apply_tweeter_delay(uint8_t delay_mm, uint32_t sample_rate);

/**
 * @brief Apply a single parametric EQ band for the woofer
 *
 * Updates one of the 6 PEQ bands (6-11) on the left channel.
 *
 * @param band_index  PEQ band index (0-5, maps to hardware bands 6-11)
 * @param peq         PEQ band settings (freq, gain, Q)
 * @param sample_rate Current sample rate in Hz
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if band_index out of range
 */
esp_err_t tas5805m_biamp_apply_low_peq(int band_index, const tas5805m_biamp_peq_band_t *peq, uint32_t sample_rate);

/**
 * @brief Apply a single parametric EQ band for the tweeter
 *
 * Updates one of the 6 PEQ bands (6-11) on the right channel.
 *
 * @param band_index  PEQ band index (0-5, maps to hardware bands 6-11)
 * @param peq         PEQ band settings (freq, gain, Q)
 * @param sample_rate Current sample rate in Hz
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if band_index out of range
 */
esp_err_t tas5805m_biamp_apply_high_peq(int band_index, const tas5805m_biamp_peq_band_t *peq, uint32_t sample_rate);

/**
 * @brief Apply baffle step compensation
 *
 * Low shelf boost to compensate for baffle step diffraction loss.
 * Updates band 12 on the left channel.
 *
 * @param width_cm    Baffle width in centimeters (0 = disabled)
 * @param placement   Room placement affecting compensation amount
 * @param sample_rate Current sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t tas5805m_biamp_apply_baffle_step(uint8_t width_cm, tas5805m_baffle_placement_t placement, uint32_t sample_rate);

/**
 * @brief Apply tweeter breakup notch filter
 *
 * Narrow-band cut to suppress the tweeter's breakup resonance peak.
 * Updates band 12 on the right channel.
 *
 * @param freq        Notch center frequency in Hz (0 = disabled)
 * @param gain_x2     Gain × 2 (should be negative for cut)
 * @param q_x10       Q factor × 10 (e.g., 50 = Q of 5.0)
 * @param sample_rate Current sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t tas5805m_biamp_apply_notch(uint16_t freq, int8_t gain_x2, uint8_t q_x10, uint32_t sample_rate);

/**
 * @brief Apply air/brilliance high shelf
 *
 * High frequency shelf at 10kHz for treble presence adjustment.
 * Updates band 13 on the right channel.
 *
 * @param gain_x2     Gain × 2 for 0.5dB resolution (0 = disabled)
 * @param sample_rate Current sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t tas5805m_biamp_apply_air_shelf(int8_t gain_x2, uint32_t sample_rate);

#endif /* CONFIG_DAC_TAS5805M */

#ifdef __cplusplus
}
#endif

#endif /* __TAS5805M_BIAMP_H__ */

/*
 * tas5805m_biamp.h
 * Advanced Bi-Amp Crossover coefficient calculations for TAS5805M
 *
 * This module provides functions to calculate and apply biquad filter
 * coefficients for active crossover configurations on the TAS5805M DAC.
 *
 * In bi-amp mode:
 * - Left channel output -> Low-pass filter -> Woofer
 * - Right channel output -> High-pass filter -> Tweeter
 *
 * Supported crossover types:
 * - Butterworth: -3dB at crossover frequency
 * - Linkwitz-Riley: -6dB at crossover frequency, flat summed response
 *
 * Supported slopes:
 * - 12 dB/octave (1 biquad stage)
 * - 24 dB/octave (2 biquad stages, standard Linkwitz-Riley)
 * - 48 dB/octave (4 biquad stages)
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

/* Biquad filter coefficient structure */
typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
} tas5805m_biquad_coeffs_t;

/* Supported sample rates for coefficient calculations */
typedef enum {
    BIAMP_SAMPLE_RATE_44100 = 44100,
    BIAMP_SAMPLE_RATE_48000 = 48000,
    BIAMP_SAMPLE_RATE_88200 = 88200,
    BIAMP_SAMPLE_RATE_96000 = 96000,
} tas5805m_biamp_sample_rate_t;

#define TAS5805M_BIAMP_DEFAULT_SAMPLE_RATE BIAMP_SAMPLE_RATE_48000

/* Biquad allocation for bi-amp mode (optimal order for headroom):
 *
 * LEFT (Woofer):                    RIGHT (Tweeter):
 * - Band 0: Gain/phase              - Band 0: Gain/phase
 * - Band 1: Subsonic HPF            - Band 1: Passthrough
 * - Bands 2-5: Lowpass crossover    - Bands 2-5: Highpass crossover
 * - Bands 6-11: PEQ (6 bands)       - Bands 6-11: PEQ (6 bands)
 * - Band 12: Passthrough (spare)    - Band 12: Passthrough (spare)
 * - Band 13: Bass shelf (loudness)  - Band 13: Passthrough
 * - Band 14: Passthrough            - Band 14: Treble shelf (loudness)
 */
#define BIAMP_GAIN_BAND             0
#define BIAMP_SUBSONIC_BAND         1   /* Subsonic HPF (woofer only) */
#define BIAMP_CROSSOVER_START_BAND  2
#define BIAMP_CROSSOVER_MAX_BANDS   4   /* Max 4 filter stages (48dB slope) */
#define BIAMP_PEQ_START_BAND        6
#define BIAMP_PEQ_BANDS             6

/**
 * @brief Calculate 2nd-order Butterworth lowpass filter coefficients
 *
 * @param fc Cutoff frequency in Hz
 * @param fs Sample rate in Hz
 * @param coeffs Output coefficients
 * @return ESP_OK on success
 */
esp_err_t tas5805m_calc_butterworth_lpf(float fc, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate 2nd-order Butterworth highpass filter coefficients
 *
 * @param fc Cutoff frequency in Hz
 * @param fs Sample rate in Hz
 * @param coeffs Output coefficients
 * @return ESP_OK on success
 */
esp_err_t tas5805m_calc_butterworth_hpf(float fc, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate parametric EQ (peaking) filter coefficients
 *
 * @param fc Center frequency in Hz
 * @param gain_db Gain in dB (-15 to +15)
 * @param q Q factor (0.5 to 10.0)
 * @param fs Sample rate in Hz
 * @param coeffs Output coefficients
 * @return ESP_OK on success
 */
esp_err_t tas5805m_calc_peq(float fc, float gain_db, float q, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate gain stage coefficients (simple scalar)
 *
 * @param gain_db Gain in dB
 * @param coeffs Output coefficients (passthrough with gain)
 * @return ESP_OK on success
 */
esp_err_t tas5805m_calc_gain(float gain_db, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate passthrough (unity) coefficients
 *
 * @param coeffs Output coefficients
 * @return ESP_OK on success
 */
esp_err_t tas5805m_calc_passthrough(tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate phase inversion coefficients
 *
 * @param coeffs Output coefficients
 * @return ESP_OK on success
 */
esp_err_t tas5805m_calc_phase_invert(tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate low shelf filter coefficients
 *
 * @param fc Shelf frequency in Hz (typically 100-400 Hz for bass)
 * @param gain_db Gain in dB (-12 to +12)
 * @param fs Sample rate in Hz
 * @param coeffs Output coefficients
 * @return ESP_OK on success
 */
esp_err_t tas5805m_calc_low_shelf(float fc, float gain_db, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Calculate high shelf filter coefficients
 *
 * @param fc Shelf frequency in Hz (typically 2-8 kHz for treble)
 * @param gain_db Gain in dB (-12 to +12)
 * @param fs Sample rate in Hz
 * @param coeffs Output coefficients
 * @return ESP_OK on success
 */
esp_err_t tas5805m_calc_high_shelf(float fc, float gain_db, float fs, tas5805m_biquad_coeffs_t *coeffs);

/**
 * @brief Get the number of biquad stages needed for a crossover slope
 *
 * @param slope The crossover slope setting
 * @return Number of biquad stages (1, 2, or 4)
 */
int tas5805m_biamp_get_biquad_count(tas5805m_biamp_slope_t slope);

/**
 * @brief Apply bi-amp crossover configuration to TAS5805M
 *
 * This function calculates all necessary filter coefficients and writes
 * them to the TAS5805M biquad registers. It handles:
 * - Crossover LP/HP filters with specified slope and type
 * - Per-output gain adjustments
 * - Per-output phase inversion
 * - Per-output PEQ bands
 *
 * @param settings Bi-amp configuration settings
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tas5805m_biamp_apply(const tas5805m_biamp_settings_t *settings);

/**
 * @brief Initialize bi-amp settings to defaults
 *
 * @param settings Settings structure to initialize
 */
void tas5805m_biamp_init_defaults(tas5805m_biamp_settings_t *settings);

#endif /* CONFIG_DAC_TAS5805M */

#ifdef __cplusplus
}
#endif

#endif /* __TAS5805M_BIAMP_H__ */

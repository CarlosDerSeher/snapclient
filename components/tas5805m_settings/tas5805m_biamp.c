/**
 * @file tas5805m_biamp.c
 * @brief Advanced Bi-Amp Active Crossover Implementation for TAS5805M
 *
 * This module implements digital biquad filter coefficient calculations for
 * active bi-amplification using the TAS5805M DAC's 15-band parametric EQ.
 * Each channel (left=woofer, right=tweeter) receives independent filter
 * processing computed using the bilinear transform method.
 *
 * @section filter_algorithms Filter Algorithms
 *
 * All filters use the bilinear transform to convert analog filter prototypes
 * to digital IIR filters. The general biquad transfer function is:
 *
 *         b0 + b1*z^-1 + b2*z^-2
 * H(z) = -------------------------
 *         1 + a1*z^-1 + a2*z^-2
 *
 * Filter coefficient formulas are based on:
 * - "Cookbook formulae for audio EQ biquad filter coefficients" by Robert Bristow-Johnson
 * - Butterworth and Linkwitz-Riley crossover design theory
 *
 * @section coefficient_format Coefficient Format
 *
 * The TAS5805M uses Q5.27 fixed-point format for biquad coefficients:
 * - Range: -16.0 to +15.9999999925
 * - Resolution: ~7.45e-9
 * - Conversion: coefficient_fixed = (int32_t)(coefficient_float * 134217728.0)
 *
 * @copyright Copyright (c) 2024
 */

#include "tas5805m_biamp.h"

/* Biamp requires both TAS5805M DAC and EQ support for biquad coefficient writing */
#if CONFIG_DAC_TAS5805M && CONFIG_DAC_TAS5805M_EQ_SUPPORT

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "tas5805m.h"

static const char *TAG = "tas5805m_biamp";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

/* ============================================================================
 * Core Filter Coefficient Calculations
 * ============================================================================ */

/**
 * @brief Calculate 2nd-order Butterworth lowpass filter coefficients
 *
 * Implements the bilinear transform of the analog Butterworth prototype:
 *
 *              1
 * H(s) = ─────────────────
 *        s² + √2·s + 1
 *
 * The bilinear transform uses frequency pre-warping to maintain accurate
 * cutoff frequency mapping from analog to digital domain:
 *
 * K = tan(π·fc/fs) where fc is cutoff frequency, fs is sample rate
 *
 * @param fc    Cutoff frequency in Hz (must be < fs/2)
 * @param fs    Sample rate in Hz
 * @param coeffs Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_butterworth_lpf(float fc, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Pre-warp the cutoff frequency for bilinear transform */
    float w0 = 2.0f * M_PI * fc / fs;
    float K = tanf(w0 / 2.0f);
    float K2 = K * K;
    float sqrt2_K = M_SQRT2 * K;

    /* Calculate normalized coefficients (a0 = 1) */
    float norm = 1.0f / (1.0f + sqrt2_K + K2);

    coeffs->b0 = K2 * norm;
    coeffs->b1 = 2.0f * coeffs->b0;
    coeffs->b2 = coeffs->b0;
    coeffs->a1 = 2.0f * (K2 - 1.0f) * norm;
    coeffs->a2 = (1.0f - sqrt2_K + K2) * norm;

    return ESP_OK;
}

/**
 * @brief Calculate 2nd-order Butterworth highpass filter coefficients
 *
 * Implements the bilinear transform of the analog Butterworth HPF prototype:
 *
 *              s²
 * H(s) = ─────────────────
 *        s² + √2·s + 1
 *
 * Uses the same pre-warping technique as the lowpass variant. The highpass
 * response is obtained by transforming s → 1/s in the lowpass prototype.
 *
 * @param fc    Cutoff frequency in Hz (must be < fs/2)
 * @param fs    Sample rate in Hz
 * @param coeffs Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_butterworth_hpf(float fc, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Pre-warp the cutoff frequency for bilinear transform */
    float w0 = 2.0f * M_PI * fc / fs;
    float K = tanf(w0 / 2.0f);
    float K2 = K * K;
    float sqrt2_K = M_SQRT2 * K;

    /* Calculate normalized coefficients (a0 = 1) */
    float norm = 1.0f / (1.0f + sqrt2_K + K2);

    coeffs->b0 = norm;
    coeffs->b1 = -2.0f * norm;
    coeffs->b2 = norm;
    coeffs->a1 = 2.0f * (K2 - 1.0f) * norm;
    coeffs->a2 = (1.0f - sqrt2_K + K2) * norm;

    return ESP_OK;
}

/**
 * @brief Calculate parametric EQ (peaking) filter coefficients
 *
 * Implements a 2nd-order parametric equalizer using the Audio EQ Cookbook
 * formula. This filter provides symmetric boost/cut around a center frequency.
 *
 * The Q parameter controls bandwidth: BW = fc/Q (in octaves: ~1.4/Q)
 *
 * Common Q values:
 * - Q = 0.5: Very wide (2.8 octaves)
 * - Q = 1.0: Moderate (1.4 octaves)
 * - Q = 2.0: Narrow (0.7 octaves)
 * - Q = 5.0: Very narrow (0.3 octaves)
 *
 * @param fc      Center frequency in Hz (must be < fs/2)
 * @param gain_db Gain in dB (positive = boost, negative = cut)
 * @param q       Quality factor (bandwidth control, must be > 0)
 * @param fs      Sample rate in Hz
 * @param coeffs  Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_peq(float fc, float gain_db, float q, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2 || q <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Zero gain means passthrough - avoid unnecessary computation */
    if (fabsf(gain_db) < 0.01f) {
        return tas5805m_calc_passthrough(coeffs);
    }

    /* A = sqrt(10^(dB/20)) = 10^(dB/40) */
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * M_PI * fc / fs;
    float sin_w0 = sinf(w0);
    float cos_w0 = cosf(w0);
    float alpha = sin_w0 / (2.0f * q);

    /* Peaking EQ coefficients from Audio EQ Cookbook */
    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cos_w0;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cos_w0;
    float a2 = 1.0f - alpha / A;

    /* Normalize by a0 to get standard biquad form */
    coeffs->b0 = b0 / a0;
    coeffs->b1 = b1 / a0;
    coeffs->b2 = b2 / a0;
    coeffs->a1 = a1 / a0;
    coeffs->a2 = a2 / a0;

    return ESP_OK;
}

/**
 * @brief Calculate simple gain stage coefficients
 *
 * Creates a first-order gain element that applies uniform gain across all
 * frequencies. Uses only b0 coefficient with all others set to zero.
 *
 * The transfer function is simply: H(z) = 10^(dB/20)
 *
 * @param gain_db Gain in dB (positive = boost, negative = cut)
 * @param coeffs  Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if coeffs is NULL
 */
esp_err_t tas5805m_calc_gain(float gain_db, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    float linear_gain = powf(10.0f, gain_db / 20.0f);

    coeffs->b0 = linear_gain;
    coeffs->b1 = 0.0f;
    coeffs->b2 = 0.0f;
    coeffs->a1 = 0.0f;
    coeffs->a2 = 0.0f;

    return ESP_OK;
}

/**
 * @brief Calculate unity passthrough coefficients
 *
 * Sets coefficients for a transparent passthrough (unity gain, no filtering).
 * H(z) = 1 (b0=1, all other coefficients = 0)
 *
 * Used to disable filter bands while maintaining DSP chain integrity.
 *
 * @param coeffs Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if coeffs is NULL
 */
esp_err_t tas5805m_calc_passthrough(tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    coeffs->b0 = 1.0f;
    coeffs->b1 = 0.0f;
    coeffs->b2 = 0.0f;
    coeffs->a1 = 0.0f;
    coeffs->a2 = 0.0f;

    return ESP_OK;
}

/**
 * @brief Calculate phase inversion coefficients
 *
 * Creates a 180-degree phase shift (polarity inversion) by setting b0 = -1.
 * H(z) = -1
 *
 * Used for driver polarity correction when physical wiring cannot be changed,
 * or to achieve acoustic phase alignment at the crossover point.
 *
 * @param coeffs Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if coeffs is NULL
 */
esp_err_t tas5805m_calc_phase_invert(tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    coeffs->b0 = -1.0f;
    coeffs->b1 = 0.0f;
    coeffs->b2 = 0.0f;
    coeffs->a1 = 0.0f;
    coeffs->a2 = 0.0f;

    return ESP_OK;
}

/**
 * @brief Calculate low shelf filter coefficients
 *
 * Implements a 2nd-order low shelf filter using the Audio EQ Cookbook formula.
 * Boosts or cuts frequencies below the corner frequency while leaving higher
 * frequencies unaffected.
 *
 * The shelf slope parameter S is fixed at 1.0, providing a moderate transition.
 * At the corner frequency, gain is half the specified value (in dB).
 *
 * Used for baffle step compensation and bass adjustments.
 *
 * @param fc      Corner frequency in Hz (must be < fs/2)
 * @param gain_db Gain in dB (positive = boost, negative = cut)
 * @param fs      Sample rate in Hz
 * @param coeffs  Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_low_shelf(float fc, float gain_db, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Zero gain means passthrough - avoid unnecessary computation */
    if (fabsf(gain_db) < 0.01f) {
        return tas5805m_calc_passthrough(coeffs);
    }

    /* A = sqrt(10^(dB/20)) = 10^(dB/40) */
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * M_PI * fc / fs;
    float sin_w0 = sinf(w0);
    float cos_w0 = cosf(w0);

    /* Shelf slope S=1 simplifies to: alpha = sin(w0)/2 * sqrt(2) */
    float alpha = sin_w0 / 2.0f * M_SQRT2;
    float two_sqrt_A_alpha = 2.0f * sqrtf(A) * alpha;

    /* Low shelf coefficients from Audio EQ Cookbook */
    float b0 = A * ((A + 1.0f) - (A - 1.0f) * cos_w0 + two_sqrt_A_alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w0);
    float b2 = A * ((A + 1.0f) - (A - 1.0f) * cos_w0 - two_sqrt_A_alpha);
    float a0 = (A + 1.0f) + (A - 1.0f) * cos_w0 + two_sqrt_A_alpha;
    float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w0);
    float a2 = (A + 1.0f) + (A - 1.0f) * cos_w0 - two_sqrt_A_alpha;

    /* Normalize by a0 */
    coeffs->b0 = b0 / a0;
    coeffs->b1 = b1 / a0;
    coeffs->b2 = b2 / a0;
    coeffs->a1 = a1 / a0;
    coeffs->a2 = a2 / a0;

    return ESP_OK;
}

/**
 * @brief Calculate high shelf filter coefficients
 *
 * Implements a 2nd-order high shelf filter using the Audio EQ Cookbook formula.
 * Boosts or cuts frequencies above the corner frequency while leaving lower
 * frequencies unaffected.
 *
 * The shelf slope parameter S is fixed at 1.0, providing a moderate transition.
 * At the corner frequency, gain is half the specified value (in dB).
 *
 * Used for air/brilliance adjustments and loudness compensation treble boost.
 *
 * @param fc      Corner frequency in Hz (must be < fs/2)
 * @param gain_db Gain in dB (positive = boost, negative = cut)
 * @param fs      Sample rate in Hz
 * @param coeffs  Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_high_shelf(float fc, float gain_db, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Zero gain means passthrough - avoid unnecessary computation */
    if (fabsf(gain_db) < 0.01f) {
        return tas5805m_calc_passthrough(coeffs);
    }

    /* A = sqrt(10^(dB/20)) = 10^(dB/40) */
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * M_PI * fc / fs;
    float sin_w0 = sinf(w0);
    float cos_w0 = cosf(w0);

    /* Shelf slope S=1 simplifies to: alpha = sin(w0)/2 * sqrt(2) */
    float alpha = sin_w0 / 2.0f * M_SQRT2;
    float two_sqrt_A_alpha = 2.0f * sqrtf(A) * alpha;

    /* High shelf coefficients from Audio EQ Cookbook */
    float b0 = A * ((A + 1.0f) + (A - 1.0f) * cos_w0 + two_sqrt_A_alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w0);
    float b2 = A * ((A + 1.0f) + (A - 1.0f) * cos_w0 - two_sqrt_A_alpha);
    float a0 = (A + 1.0f) - (A - 1.0f) * cos_w0 + two_sqrt_A_alpha;
    float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w0);
    float a2 = (A + 1.0f) - (A - 1.0f) * cos_w0 - two_sqrt_A_alpha;

    /* Normalize by a0 */
    coeffs->b0 = b0 / a0;
    coeffs->b1 = b1 / a0;
    coeffs->b2 = b2 / a0;
    coeffs->a1 = a1 / a0;
    coeffs->a2 = a2 / a0;

    return ESP_OK;
}

/* ============================================================================
 * Specialized Filter Calculations
 * ============================================================================ */

/**
 * @brief Calculate baffle step compensation parameters
 *
 * Baffle step is an acoustic phenomenon where low frequencies radiate
 * omnidirectionally (wrapping around the speaker baffle) while high
 * frequencies beam forward. This causes a 3-6dB step in SPL response.
 *
 * The transition frequency depends on baffle width:
 *   f_step = c / (π × w)
 * where c = 343 m/s (speed of sound at ~20°C) and w = baffle width in meters.
 *
 * Example frequencies for common baffle widths:
 * - 15cm baffle: ~728 Hz
 * - 20cm baffle: ~546 Hz
 * - 30cm baffle: ~364 Hz
 *
 * Room placement affects the required compensation:
 * - Freestanding: Full 6dB boost (no boundary reinforcement)
 * - Near wall: 3dB boost (single boundary adds ~3dB at low frequencies)
 * - Corner: 0dB boost (two boundaries provide full reinforcement)
 *
 * @param baffle_width_cm Baffle width in centimeters (0 = disabled, 5-50cm valid)
 * @param placement       Speaker placement relative to room boundaries
 * @param step_freq_hz    Output: calculated step frequency in Hz (may be NULL)
 * @param gain_db         Output: recommended compensation gain in dB (may be NULL)
 */
void tas5805m_calc_baffle_step(uint8_t baffle_width_cm,
                                tas5805m_baffle_placement_t placement,
                                float *step_freq_hz, float *gain_db)
{
    /* Initialize outputs to disabled state */
    if (step_freq_hz) *step_freq_hz = 0.0f;
    if (gain_db) *gain_db = 0.0f;

    /* Width of 0 means disabled */
    if (baffle_width_cm == 0) {
        return;
    }

    /* Convert cm to meters and clamp to reasonable range */
    float width_m = (float)baffle_width_cm / 100.0f;
    if (width_m < 0.05f) width_m = 0.05f;  /* Minimum 5cm */
    if (width_m > 0.50f) width_m = 0.50f;  /* Maximum 50cm */

    /* Calculate step frequency: f = c / (π × w) */
    float freq = 343.0f / (M_PI * width_m);

    /* Determine compensation gain based on room placement */
    float gain;
    switch (placement) {
        case BAFFLE_PLACEMENT_FREESTANDING:
            gain = 6.0f;  /* Full compensation needed */
            break;
        case BAFFLE_PLACEMENT_NEAR_WALL:
            gain = 3.0f;  /* Wall provides partial bass reinforcement */
            break;
        case BAFFLE_PLACEMENT_CORNER:
        default:
            gain = 0.0f;  /* Corner placement provides full reinforcement */
            break;
    }

    if (step_freq_hz) *step_freq_hz = freq;
    if (gain_db) *gain_db = gain;

    ESP_LOGD(TAG, "Baffle step: width=%dcm placement=%d -> freq=%.1fHz gain=%.1fdB",
             baffle_width_cm, placement, freq, gain);
}

/**
 * @brief Calculate first-order all-pass filter for time alignment
 *
 * Creates frequency-dependent phase shift (group delay) to compensate for
 * physical driver offset. This is an approximation suitable for small delays
 * where pure sample delay would require fractional samples.
 *
 * Transfer function: H(z) = (a + z^-1) / (1 + a·z^-1)
 * where a = (1 - tan(π·fc/fs)) / (1 + tan(π·fc/fs))
 *
 * The group delay at DC approaches 1/(π·fc), so for a desired delay T:
 *   fc = 1 / (π × T)
 *
 * Physical offset to time conversion:
 *   T = d / c where d = offset distance, c = 343000 mm/s
 *
 * Typical tweeter offset values:
 * - 10mm: ~29 µs delay
 * - 25mm: ~73 µs delay
 * - 50mm: ~146 µs delay
 *
 * @note This is a first-order approximation. For precise alignment, physical
 *       driver positioning or dedicated delay DSP blocks are preferable.
 *
 * @param delay_mm Physical offset to compensate in millimeters (0 = passthrough)
 * @param fs       Sample rate in Hz
 * @param coeffs   Output coefficient structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if parameters invalid
 */
esp_err_t tas5805m_calc_allpass_delay(uint8_t delay_mm, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fs <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* No delay requested - return passthrough */
    if (delay_mm == 0) {
        return tas5805m_calc_passthrough(coeffs);
    }

    /* Convert millimeters to seconds: t = d / (343000 mm/s) */
    float delay_sec = (float)delay_mm / 343000.0f;

    /* Calculate corner frequency for desired delay: fc = 1 / (π × t) */
    float fc = 1.0f / (M_PI * delay_sec);

    /* Clamp corner frequency to valid digital filter range */
    if (fc < 20.0f) fc = 20.0f;
    if (fc >= fs / 2.0f) fc = fs / 2.0f - 1.0f;

    /* First-order all-pass coefficient calculation */
    float w0 = 2.0f * M_PI * fc / fs;
    float tan_w0_2 = tanf(w0 / 2.0f);
    float a = (1.0f - tan_w0_2) / (1.0f + tan_w0_2);

    /* Express as biquad with b2=0, a2=0 (first-order in biquad structure) */
    coeffs->b0 = a;
    coeffs->b1 = 1.0f;
    coeffs->b2 = 0.0f;
    coeffs->a1 = a;
    coeffs->a2 = 0.0f;

    ESP_LOGD(TAG, "Tweeter delay: %dmm -> %.1fus, fc=%.0fHz, a=%.6f",
             delay_mm, delay_sec * 1000000.0f, fc, a);

    return ESP_OK;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * @brief Get number of biquad stages required for a crossover slope
 *
 * Crossover slopes are achieved by cascading multiple 2nd-order filter sections:
 * - 12 dB/octave: 1 biquad (2nd order)
 * - 24 dB/octave: 2 biquads (4th order) - Linkwitz-Riley alignment
 * - 48 dB/octave: 4 biquads (8th order) - Steep, brick-wall rolloff
 *
 * @param slope Desired crossover slope
 * @return Number of biquad stages (1, 2, or 4)
 */
int tas5805m_biamp_get_biquad_count(tas5805m_biamp_slope_t slope)
{
    switch (slope) {
        case BIAMP_SLOPE_12DB: return 1;
        case BIAMP_SLOPE_24DB: return 2;
        case BIAMP_SLOPE_48DB: return 4;
        default: return 2;  /* Default to 24 dB/octave */
    }
}

/**
 * @brief Write biquad coefficients to a specific DSP band
 *
 * Internal helper that wraps the low-level coefficient write function.
 * The TAS5805M accepts coefficient writes without inter-write delays.
 *
 * @param channel Target channel (LEFT or RIGHT)
 * @param band    Band index (0-14)
 * @param coeffs  Coefficient structure to write
 * @return ESP_OK on success, error code on I2C failure
 */
static esp_err_t write_biquad_band(TAS5805M_EQ_CHANNELS channel, int band,
                                    const tas5805m_biquad_coeffs_t *coeffs)
{
    return tas5805m_write_biquad_coefficients(channel, band,
                                               coeffs->b0, coeffs->b1, coeffs->b2,
                                               coeffs->a1, coeffs->a2);
}

/**
 * @brief Validate and normalize sample rate to supported value
 *
 * Ensures the sample rate is one of the supported values for coefficient
 * calculations. Invalid rates default to 48000 Hz with a warning log.
 *
 * @param sample_rate Input sample rate to validate
 * @return Validated sample rate (one of 44100, 48000, 88200, 96000)
 */
static uint32_t validate_sample_rate(uint32_t sample_rate)
{
    switch (sample_rate) {
        case BIAMP_SAMPLE_RATE_44100:
        case BIAMP_SAMPLE_RATE_48000:
        case BIAMP_SAMPLE_RATE_88200:
        case BIAMP_SAMPLE_RATE_96000:
            return sample_rate;
        default:
            ESP_LOGW(TAG, "Invalid sample rate %lu, using default %d",
                     (unsigned long)sample_rate, TAS5805M_BIAMP_DEFAULT_SAMPLE_RATE);
            return TAS5805M_BIAMP_DEFAULT_SAMPLE_RATE;
    }
}

/* ============================================================================
 * Full Configuration Application
 * ============================================================================ */

/**
 * @brief Apply complete bi-amp crossover configuration to TAS5805M
 *
 * Configures all 15 biquad bands on both channels for active bi-amplification.
 * This is the main entry point for applying a complete crossover setup.
 *
 * Band allocation per channel:
 *
 * LEFT (Woofer/Low output):
 * - Band 0:  Gain + optional phase inversion
 * - Band 1:  Subsonic highpass filter (rumble protection)
 * - Bands 2-5: Lowpass crossover filter stages
 * - Bands 6-8: Parametric EQ (3 bands for driver correction)
 * - Band 12: Baffle step compensation (low shelf boost)
 * - Bands 13-14: Reserved for loudness compensation
 *
 * RIGHT (Tweeter/High output):
 * - Band 0:  Gain + optional phase inversion
 * - Band 1:  Time alignment (all-pass delay approximation)
 * - Bands 2-5: Highpass crossover filter stages
 * - Bands 6-8: Parametric EQ (3 bands for driver correction)
 * - Band 12: Tweeter breakup notch filter
 * - Band 13: Air/brilliance high shelf
 * - Band 14: Reserved for loudness compensation
 *
 * @param settings Pointer to complete bi-amp settings structure
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if settings is NULL,
 *         or I2C error code on hardware communication failure
 */
esp_err_t tas5805m_biamp_apply(const tas5805m_biamp_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    tas5805m_biquad_coeffs_t coeffs;
    int num_stages = tas5805m_biamp_get_biquad_count(settings->slope);
    float fc = (float)settings->crossover_freq;
    float fs = (float)validate_sample_rate(settings->sample_rate);
    int band;

    ESP_LOGI(TAG, "Applying bi-amp crossover: fc=%dHz, slope=%ddB/oct, type=%s, fs=%.0fHz",
             settings->crossover_freq,
             num_stages * 12,
             settings->type == BIAMP_TYPE_LINKWITZ_RILEY ? "LR" : "BW",
             fs);

    /* ========== LEFT CHANNEL (WOOFER/LOW OUTPUT) ========== */

    /* Band 0: Gain + optional phase inversion */
    float low_total_gain = (float)settings->low_gain / 2.0f;  /* Convert from x2 format */
    if (settings->low_phase_invert) {
        /* Combine gain and phase invert into single coefficient */
        low_total_gain = -powf(10.0f, low_total_gain / 20.0f);
        coeffs.b0 = low_total_gain;
        coeffs.b1 = 0.0f;
        coeffs.b2 = 0.0f;
        coeffs.a1 = 0.0f;
        coeffs.a2 = 0.0f;
    } else {
        ret = tas5805m_calc_gain(low_total_gain, &coeffs);
        if (ret != ESP_OK) return ret;
    }
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, BIAMP_GAIN_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* Band 1: Subsonic highpass filter (placed early for optimal headroom) */
    if (settings->subsonic_freq > 0 && settings->subsonic_freq < fs / 2) {
        ret = tas5805m_calc_butterworth_hpf((float)settings->subsonic_freq, fs, &coeffs);
        if (ret != ESP_OK) return ret;
        ESP_LOGI(TAG, "Applying subsonic HPF at %d Hz (band %d)", settings->subsonic_freq, BIAMP_SUBSONIC_BAND);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
        if (ret != ESP_OK) return ret;
    }
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, BIAMP_SUBSONIC_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* Bands 2-5: Lowpass crossover filter stages */
    ret = tas5805m_calc_butterworth_lpf(fc, fs, &coeffs);
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < num_stages; i++) {
        band = BIAMP_CROSSOVER_START_BAND + i;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Fill unused crossover bands with passthrough */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    for (int i = num_stages; i < BIAMP_CROSSOVER_MAX_BANDS; i++) {
        band = BIAMP_CROSSOVER_START_BAND + i;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Bands 6-8: Parametric EQ for woofer correction */
    for (int i = 0; i < BIAMP_PEQ_BANDS; i++) {
        band = BIAMP_PEQ_START_BAND + i;
        const tas5805m_biamp_peq_band_t *peq = &settings->low_peq[i];

        if (peq->freq > 0 && peq->freq < fs / 2 && peq->gain != 0) {
            float q = (float)peq->q_x10 / 10.0f;
            float gain_db = (float)peq->gain / 2.0f;
            ret = tas5805m_calc_peq((float)peq->freq, gain_db, q, fs, &coeffs);
        } else {
            ret = tas5805m_calc_passthrough(&coeffs);
        }
        if (ret != ESP_OK) return ret;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Band 12: Baffle step compensation (low shelf boost) */
    if (settings->baffle_width_cm > 0) {
        float step_freq, step_gain;
        tas5805m_calc_baffle_step(settings->baffle_width_cm, settings->baffle_placement,
                                   &step_freq, &step_gain);
        if (step_gain > 0.1f && step_freq > 0.0f && step_freq < fs / 2) {
            ret = tas5805m_calc_low_shelf(step_freq, step_gain, fs, &coeffs);
            if (ret != ESP_OK) return ret;
            ESP_LOGI(TAG, "Applying baffle step: width=%dcm freq=%.0fHz gain=+%.1fdB (band %d)",
                     settings->baffle_width_cm, step_freq, step_gain, BIAMP_BAFFLE_STEP_BAND);
        } else {
            ret = tas5805m_calc_passthrough(&coeffs);
            if (ret != ESP_OK) return ret;
        }
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
        if (ret != ESP_OK) return ret;
    }
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, BIAMP_BAFFLE_STEP_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* Bands 13-14: Initialize to passthrough (may be overwritten by loudness) */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    for (band = TAS5805M_LOUDNESS_BASS_BAND; band <= TAS5805M_LOUDNESS_TREBLE_BAND; band++) {
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* ========== RIGHT CHANNEL (TWEETER/HIGH OUTPUT) ========== */

    /* Band 0: Gain + optional phase inversion */
    float high_total_gain = (float)settings->high_gain / 2.0f;  /* Convert from x2 format */
    if (settings->high_phase_invert) {
        /* Combine gain and phase invert into single coefficient */
        high_total_gain = -powf(10.0f, high_total_gain / 20.0f);
        coeffs.b0 = high_total_gain;
        coeffs.b1 = 0.0f;
        coeffs.b2 = 0.0f;
        coeffs.a1 = 0.0f;
        coeffs.a2 = 0.0f;
    } else {
        ret = tas5805m_calc_gain(high_total_gain, &coeffs);
        if (ret != ESP_OK) return ret;
    }
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_GAIN_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* Band 1: Time alignment (all-pass delay approximation) */
    if (settings->tweeter_delay_mm > 0) {
        ret = tas5805m_calc_allpass_delay(settings->tweeter_delay_mm, fs, &coeffs);
        if (ret != ESP_OK) return ret;
        ESP_LOGI(TAG, "Applying tweeter delay: %d mm (band %d)",
                 settings->tweeter_delay_mm, BIAMP_TWEETER_DELAY_BAND);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
        if (ret != ESP_OK) return ret;
    }
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_TWEETER_DELAY_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* Bands 2-5: Highpass crossover filter stages */
    ret = tas5805m_calc_butterworth_hpf(fc, fs, &coeffs);
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < num_stages; i++) {
        band = BIAMP_CROSSOVER_START_BAND + i;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Fill unused crossover bands with passthrough */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    for (int i = num_stages; i < BIAMP_CROSSOVER_MAX_BANDS; i++) {
        band = BIAMP_CROSSOVER_START_BAND + i;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Bands 6-8: Parametric EQ for tweeter correction */
    for (int i = 0; i < BIAMP_PEQ_BANDS; i++) {
        band = BIAMP_PEQ_START_BAND + i;
        const tas5805m_biamp_peq_band_t *peq = &settings->high_peq[i];

        if (peq->freq > 0 && peq->freq < fs / 2 && peq->gain != 0) {
            float q = (float)peq->q_x10 / 10.0f;
            float gain_db = (float)peq->gain / 2.0f;
            ret = tas5805m_calc_peq((float)peq->freq, gain_db, q, fs, &coeffs);
        } else {
            ret = tas5805m_calc_passthrough(&coeffs);
        }
        if (ret != ESP_OK) return ret;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Band 12: Tweeter breakup notch (narrow cut at resonance) */
    if (settings->notch_freq > 0 && settings->notch_gain < 0) {
        float q = (float)settings->notch_q_x10 / 10.0f;
        float gain_db = (float)settings->notch_gain / 2.0f;
        ret = tas5805m_calc_peq((float)settings->notch_freq, gain_db, q, fs, &coeffs);
        if (ret != ESP_OK) return ret;
        ESP_LOGI(TAG, "Applying tweeter breakup notch: %dHz %.1fdB Q=%.1f (band %d)",
                 settings->notch_freq, gain_db, q, BIAMP_NOTCH_BAND);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
        if (ret != ESP_OK) return ret;
    }
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_NOTCH_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* Band 13: Air/brilliance high shelf (fixed 10kHz corner) */
    if (settings->air_gain != 0) {
        float gain_db = (float)settings->air_gain / 2.0f;
        ret = tas5805m_calc_high_shelf(10000.0f, gain_db, fs, &coeffs);
        if (ret != ESP_OK) return ret;
        ESP_LOGI(TAG, "Applying air/brilliance shelf: %.1fdB @ 10kHz (band %d)",
                 gain_db, BIAMP_AIR_SHELF_BAND);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
        if (ret != ESP_OK) return ret;
    }
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_AIR_SHELF_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* Band 14: Initialize to passthrough (may be overwritten by loudness) */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, TAS5805M_LOUDNESS_TREBLE_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Bi-amp crossover applied successfully (subsonic=%dHz)", settings->subsonic_freq);
    return ESP_OK;
}

/* ============================================================================
 * Individual Band Application Functions
 *
 * These functions allow updating single parameters without re-applying the
 * entire crossover configuration. Useful for real-time adjustment of gain,
 * phase, or EQ settings without audible glitches.
 * ============================================================================ */

/**
 * @brief Apply woofer gain and phase settings to Band 0 (left channel)
 *
 * Updates only the gain/phase band without affecting crossover or EQ settings.
 * Phase inversion is combined with gain into a single coefficient.
 *
 * @param gain_x2       Gain in 0.5dB steps (-48 to +48 = -24dB to +24dB)
 * @param phase_invert  Non-zero to invert polarity (180° phase shift)
 * @param sample_rate   Current sample rate (used for validation only)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tas5805m_biamp_apply_low_gain_phase(int8_t gain_x2, uint8_t phase_invert, uint32_t sample_rate)
{
    tas5805m_biquad_coeffs_t coeffs;

    /* Clamp gain to valid range: -24dB to +24dB */
    if (gain_x2 < -48) gain_x2 = -48;
    if (gain_x2 > 48) gain_x2 = 48;

    float gain_db = (float)gain_x2 / 2.0f;
    esp_err_t ret;

    if (phase_invert) {
        /* Combine negative gain with phase inversion */
        float linear_gain = -powf(10.0f, gain_db / 20.0f);
        /* Clamp to Q5.27 representable range */
        if (linear_gain < -16.0f) linear_gain = -16.0f;
        if (linear_gain > 15.999f) linear_gain = 15.999f;
        coeffs.b0 = linear_gain;
        coeffs.b1 = 0.0f;
        coeffs.b2 = 0.0f;
        coeffs.a1 = 0.0f;
        coeffs.a2 = 0.0f;
    } else {
        ret = tas5805m_calc_gain(gain_db, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    return write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, BIAMP_GAIN_BAND, &coeffs);
}

/**
 * @brief Apply tweeter gain and phase settings to Band 0 (right channel)
 *
 * Updates only the gain/phase band without affecting crossover or EQ settings.
 * Phase inversion is combined with gain into a single coefficient.
 *
 * @param gain_x2       Gain in 0.5dB steps (-48 to +48 = -24dB to +24dB)
 * @param phase_invert  Non-zero to invert polarity (180° phase shift)
 * @param sample_rate   Current sample rate (used for validation only)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tas5805m_biamp_apply_high_gain_phase(int8_t gain_x2, uint8_t phase_invert, uint32_t sample_rate)
{
    tas5805m_biquad_coeffs_t coeffs;

    /* Clamp gain to valid range: -24dB to +24dB */
    if (gain_x2 < -48) gain_x2 = -48;
    if (gain_x2 > 48) gain_x2 = 48;

    float gain_db = (float)gain_x2 / 2.0f;
    esp_err_t ret;

    if (phase_invert) {
        /* Combine negative gain with phase inversion */
        float linear_gain = -powf(10.0f, gain_db / 20.0f);
        /* Clamp to Q5.27 representable range */
        if (linear_gain < -16.0f) linear_gain = -16.0f;
        if (linear_gain > 15.999f) linear_gain = 15.999f;
        coeffs.b0 = linear_gain;
        coeffs.b1 = 0.0f;
        coeffs.b2 = 0.0f;
        coeffs.a1 = 0.0f;
        coeffs.a2 = 0.0f;
    } else {
        ret = tas5805m_calc_gain(gain_db, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    return write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_GAIN_BAND, &coeffs);
}

/**
 * @brief Apply subsonic highpass filter to Band 1 (left channel only)
 *
 * Protects woofer from infrasonic content that causes cone excursion without
 * producing audible sound. Uses 2nd-order Butterworth response.
 *
 * Common subsonic frequencies:
 * - 20 Hz: Minimal filtering, preserves deep bass
 * - 30 Hz: Good protection for ported designs
 * - 40 Hz: Aggressive protection for small woofers
 *
 * @param freq        Cutoff frequency in Hz (0 = disabled/passthrough)
 * @param sample_rate Current sample rate
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tas5805m_biamp_apply_subsonic(uint16_t freq, uint32_t sample_rate)
{
    tas5805m_biquad_coeffs_t coeffs;
    float fs = (float)validate_sample_rate(sample_rate);
    esp_err_t ret;

    if (freq > 0 && freq < fs / 2) {
        ret = tas5805m_calc_butterworth_hpf((float)freq, fs, &coeffs);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
    }
    if (ret != ESP_OK) return ret;

    return write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, BIAMP_SUBSONIC_BAND, &coeffs);
}

/**
 * @brief Apply tweeter time alignment delay to Band 1 (right channel only)
 *
 * Compensates for physical driver offset using all-pass filter approximation.
 * This provides frequency-dependent phase shift that approximates a pure delay
 * near the crossover region.
 *
 * @param delay_mm    Physical offset in millimeters (0 = disabled/passthrough)
 * @param sample_rate Current sample rate
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tas5805m_biamp_apply_tweeter_delay(uint8_t delay_mm, uint32_t sample_rate)
{
    tas5805m_biquad_coeffs_t coeffs;
    float fs = (float)validate_sample_rate(sample_rate);
    esp_err_t ret;

    if (delay_mm > 0) {
        ret = tas5805m_calc_allpass_delay(delay_mm, fs, &coeffs);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
    }
    if (ret != ESP_OK) return ret;

    return write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_TWEETER_DELAY_BAND, &coeffs);
}

/**
 * @brief Apply single parametric EQ band for woofer (Bands 6-8, left channel)
 *
 * Allows individual PEQ band updates without affecting other bands.
 * Useful for driver-specific corrections like cone breakup modes.
 *
 * @param band_index PEQ band index (0-2, maps to hardware bands 6-8)
 * @param peq        Pointer to PEQ band settings
 * @param sample_rate Current sample rate
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if band_index invalid
 */
esp_err_t tas5805m_biamp_apply_low_peq(int band_index, const tas5805m_biamp_peq_band_t *peq, uint32_t sample_rate)
{
    if (band_index < 0 || band_index >= BIAMP_PEQ_BANDS || !peq) {
        return ESP_ERR_INVALID_ARG;
    }

    tas5805m_biquad_coeffs_t coeffs;
    float fs = (float)validate_sample_rate(sample_rate);
    esp_err_t ret;
    int band = BIAMP_PEQ_START_BAND + band_index;

    if (peq->freq > 0 && peq->freq < fs / 2 && peq->gain != 0) {
        float q = (float)peq->q_x10 / 10.0f;
        float gain_db = (float)peq->gain / 2.0f;
        ret = tas5805m_calc_peq((float)peq->freq, gain_db, q, fs, &coeffs);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
    }
    if (ret != ESP_OK) return ret;

    return write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
}

/**
 * @brief Apply single parametric EQ band for tweeter (Bands 6-8, right channel)
 *
 * Allows individual PEQ band updates without affecting other bands.
 * Useful for driver-specific corrections like dome resonances.
 *
 * @param band_index PEQ band index (0-2, maps to hardware bands 6-8)
 * @param peq        Pointer to PEQ band settings
 * @param sample_rate Current sample rate
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if band_index invalid
 */
esp_err_t tas5805m_biamp_apply_high_peq(int band_index, const tas5805m_biamp_peq_band_t *peq, uint32_t sample_rate)
{
    if (band_index < 0 || band_index >= BIAMP_PEQ_BANDS || !peq) {
        return ESP_ERR_INVALID_ARG;
    }

    tas5805m_biquad_coeffs_t coeffs;
    float fs = (float)validate_sample_rate(sample_rate);
    esp_err_t ret;
    int band = BIAMP_PEQ_START_BAND + band_index;

    if (peq->freq > 0 && peq->freq < fs / 2 && peq->gain != 0) {
        float q = (float)peq->q_x10 / 10.0f;
        float gain_db = (float)peq->gain / 2.0f;
        ret = tas5805m_calc_peq((float)peq->freq, gain_db, q, fs, &coeffs);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
    }
    if (ret != ESP_OK) return ret;

    return write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, band, &coeffs);
}

/**
 * @brief Apply baffle step compensation to Band 12 (left channel only)
 *
 * Applies low shelf boost to compensate for acoustic baffle diffraction step.
 * The step frequency and gain are calculated based on baffle dimensions and
 * speaker placement relative to room boundaries.
 *
 * @param width_cm    Baffle width in centimeters (0 = disabled)
 * @param placement   Speaker placement (freestanding, near wall, corner)
 * @param sample_rate Current sample rate
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tas5805m_biamp_apply_baffle_step(uint8_t width_cm, tas5805m_baffle_placement_t placement, uint32_t sample_rate)
{
    tas5805m_biquad_coeffs_t coeffs;
    float fs = (float)validate_sample_rate(sample_rate);
    esp_err_t ret;

    if (width_cm > 0) {
        float step_freq, step_gain;
        tas5805m_calc_baffle_step(width_cm, placement, &step_freq, &step_gain);
        if (step_gain > 0.1f && step_freq > 0.0f && step_freq < fs / 2) {
            ret = tas5805m_calc_low_shelf(step_freq, step_gain, fs, &coeffs);
        } else {
            ret = tas5805m_calc_passthrough(&coeffs);
        }
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
    }
    if (ret != ESP_OK) return ret;

    return write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, BIAMP_BAFFLE_STEP_BAND, &coeffs);
}

/**
 * @brief Apply tweeter breakup notch filter to Band 12 (right channel only)
 *
 * Attenuates the tweeter's breakup resonance, typically found between
 * 15-25 kHz depending on dome material and size. Uses a narrow peaking
 * filter with negative gain.
 *
 * Typical notch parameters:
 * - Aluminum dome: ~20-25 kHz, Q=5-10
 * - Silk dome: ~15-18 kHz, Q=3-5
 *
 * @param freq        Center frequency in Hz (0 = disabled)
 * @param gain_x2     Gain in 0.5dB steps (must be negative for notch effect)
 * @param q_x10       Q factor x10 (e.g., 50 = Q of 5.0)
 * @param sample_rate Current sample rate
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tas5805m_biamp_apply_notch(uint16_t freq, int8_t gain_x2, uint8_t q_x10, uint32_t sample_rate)
{
    tas5805m_biquad_coeffs_t coeffs;
    float fs = (float)validate_sample_rate(sample_rate);
    esp_err_t ret;

    if (freq > 0 && gain_x2 < 0) {
        float q = (float)q_x10 / 10.0f;
        float gain_db = (float)gain_x2 / 2.0f;
        ret = tas5805m_calc_peq((float)freq, gain_db, q, fs, &coeffs);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
    }
    if (ret != ESP_OK) return ret;

    return write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_NOTCH_BAND, &coeffs);
}

/**
 * @brief Apply air/brilliance high shelf to Band 13 (right channel only)
 *
 * Adjusts high frequency "air" or "brilliance" using a high shelf filter
 * with fixed 10 kHz corner frequency. Positive gain adds sparkle/detail,
 * negative gain softens the top end.
 *
 * @param gain_x2     Gain in 0.5dB steps (0 = disabled)
 * @param sample_rate Current sample rate
 * @return ESP_OK on success, error code on failure
 */
esp_err_t tas5805m_biamp_apply_air_shelf(int8_t gain_x2, uint32_t sample_rate)
{
    tas5805m_biquad_coeffs_t coeffs;
    float fs = (float)validate_sample_rate(sample_rate);
    esp_err_t ret;

    if (gain_x2 != 0) {
        float gain_db = (float)gain_x2 / 2.0f;
        ret = tas5805m_calc_high_shelf(10000.0f, gain_db, fs, &coeffs);
    } else {
        ret = tas5805m_calc_passthrough(&coeffs);
    }
    if (ret != ESP_OK) return ret;

    return write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_AIR_SHELF_BAND, &coeffs);
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize bi-amp settings structure to safe defaults
 *
 * Sets all parameters to reasonable starting values:
 * - Crossover: 2000 Hz, 24 dB/octave Linkwitz-Riley
 * - Gains: 0 dB on both outputs
 * - Phase: Normal (non-inverted) on both outputs
 * - Subsonic filter: Disabled
 * - PEQ bands: All disabled (freq=0)
 * - Baffle step: Disabled
 * - Time alignment: Disabled
 * - Tweeter notch: Disabled
 * - Air shelf: Disabled
 *
 * @param settings Pointer to settings structure to initialize
 */
void tas5805m_biamp_init_defaults(tas5805m_biamp_settings_t *settings)
{
    if (settings == NULL) return;

    memset(settings, 0, sizeof(tas5805m_biamp_settings_t));

    /* Crossover configuration */
    settings->crossover_freq = TAS5805M_BIAMP_DEFAULT_XOVER_FREQ;
    settings->slope = TAS5805M_BIAMP_DEFAULT_SLOPE;
    settings->type = TAS5805M_BIAMP_DEFAULT_TYPE;
    settings->sample_rate = TAS5805M_BIAMP_DEFAULT_SAMPLE_RATE;
    settings->subsonic_freq = 0;  /* Disabled */

    /* Output levels */
    settings->low_gain = 0;
    settings->low_phase_invert = 0;
    settings->high_gain = 0;
    settings->high_phase_invert = 0;

    /* Initialize PEQ bands to disabled with sensible Q defaults */
    for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS; i++) {
        settings->low_peq[i].freq = 0;     /* Disabled */
        settings->low_peq[i].gain = 0;
        settings->low_peq[i].q_x10 = 14;   /* Q = 1.4 (moderate bandwidth) */

        settings->high_peq[i].freq = 0;    /* Disabled */
        settings->high_peq[i].gain = 0;
        settings->high_peq[i].q_x10 = 14;  /* Q = 1.4 */
    }

    /* Baffle step compensation */
    settings->baffle_width_cm = 0;  /* Disabled */
    settings->baffle_placement = BAFFLE_PLACEMENT_FREESTANDING;

    /* Time alignment */
    settings->tweeter_delay_mm = 0;  /* Disabled */

    /* Tweeter breakup notch */
    settings->notch_freq = 0;       /* Disabled */
    settings->notch_gain = 0;
    settings->notch_q_x10 = 50;     /* Q = 5.0 (narrow notch) */

    /* Air/brilliance shelf */
    settings->air_gain = 0;  /* Disabled */
}

#endif /* CONFIG_DAC_TAS5805M && CONFIG_DAC_TAS5805M_EQ_SUPPORT */

/* Stub functions when TAS5805M is enabled but EQ support is not */
#if CONFIG_DAC_TAS5805M && !CONFIG_DAC_TAS5805M_EQ_SUPPORT

#include "esp_log.h"
static const char *TAG = "tas5805m_biamp";

esp_err_t tas5805m_biamp_apply(const tas5805m_biamp_settings_t *settings) {
    (void)settings;
    ESP_LOGW(TAG, "Biamp not available - EQ support disabled in menuconfig");
    return ESP_ERR_NOT_SUPPORTED;
}

void tas5805m_biamp_init_defaults(tas5805m_biamp_settings_t *settings) {
    if (settings) {
        memset(settings, 0, sizeof(*settings));
    }
}

#endif /* CONFIG_DAC_TAS5805M && !CONFIG_DAC_TAS5805M_EQ_SUPPORT */

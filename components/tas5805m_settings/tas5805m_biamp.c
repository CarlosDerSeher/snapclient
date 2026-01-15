/*
 * tas5805m_biamp.c
 * Advanced Bi-Amp Crossover coefficient calculations for TAS5805M
 */

#include "tas5805m_biamp.h"

#if CONFIG_DAC_TAS5805M

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

/*
 * Calculate 2nd-order Butterworth lowpass filter coefficients
 * Using bilinear transform from analog prototype H(s) = 1/(s² + √2·s + 1)
 */
esp_err_t tas5805m_calc_butterworth_lpf(float fc, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Pre-warp the cutoff frequency */
    float w0 = 2.0f * M_PI * fc / fs;
    float K = tanf(w0 / 2.0f);
    float K2 = K * K;
    float sqrt2_K = M_SQRT2 * K;

    /* Calculate coefficients */
    float norm = 1.0f / (1.0f + sqrt2_K + K2);

    coeffs->b0 = K2 * norm;
    coeffs->b1 = 2.0f * coeffs->b0;
    coeffs->b2 = coeffs->b0;
    coeffs->a1 = 2.0f * (K2 - 1.0f) * norm;
    coeffs->a2 = (1.0f - sqrt2_K + K2) * norm;

    return ESP_OK;
}

/*
 * Calculate 2nd-order Butterworth highpass filter coefficients
 * Using bilinear transform from analog prototype H(s) = s²/(s² + √2·s + 1)
 */
esp_err_t tas5805m_calc_butterworth_hpf(float fc, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Pre-warp the cutoff frequency */
    float w0 = 2.0f * M_PI * fc / fs;
    float K = tanf(w0 / 2.0f);
    float K2 = K * K;
    float sqrt2_K = M_SQRT2 * K;

    /* Calculate coefficients */
    float norm = 1.0f / (1.0f + sqrt2_K + K2);

    coeffs->b0 = norm;
    coeffs->b1 = -2.0f * norm;
    coeffs->b2 = norm;
    coeffs->a1 = 2.0f * (K2 - 1.0f) * norm;
    coeffs->a2 = (1.0f - sqrt2_K + K2) * norm;

    return ESP_OK;
}

/*
 * Calculate parametric EQ (peaking) filter coefficients
 * Based on Audio EQ Cookbook by Robert Bristow-Johnson
 */
esp_err_t tas5805m_calc_peq(float fc, float gain_db, float q, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2 || q <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* If gain is essentially zero, return passthrough */
    if (fabsf(gain_db) < 0.01f) {
        return tas5805m_calc_passthrough(coeffs);
    }

    float A = powf(10.0f, gain_db / 40.0f);  /* sqrt of linear gain */
    float w0 = 2.0f * M_PI * fc / fs;
    float sin_w0 = sinf(w0);
    float cos_w0 = cosf(w0);
    float alpha = sin_w0 / (2.0f * q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cos_w0;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cos_w0;
    float a2 = 1.0f - alpha / A;

    /* Normalize by a0 */
    coeffs->b0 = b0 / a0;
    coeffs->b1 = b1 / a0;
    coeffs->b2 = b2 / a0;
    coeffs->a1 = a1 / a0;
    coeffs->a2 = a2 / a0;

    return ESP_OK;
}

/*
 * Calculate simple gain stage (passthrough with gain)
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

/*
 * Calculate passthrough (unity) coefficients
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

/*
 * Calculate phase inversion coefficients
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

/*
 * Calculate low shelf filter coefficients
 * Based on Audio EQ Cookbook by Robert Bristow-Johnson
 */
esp_err_t tas5805m_calc_low_shelf(float fc, float gain_db, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* If gain is essentially zero, return passthrough */
    if (fabsf(gain_db) < 0.01f) {
        return tas5805m_calc_passthrough(coeffs);
    }

    float A = powf(10.0f, gain_db / 40.0f);  /* sqrt of linear gain */
    float w0 = 2.0f * M_PI * fc / fs;
    float sin_w0 = sinf(w0);
    float cos_w0 = cosf(w0);
    /* S = 1 gives a steep shelf, commonly used value */
    float alpha = sin_w0 / 2.0f * sqrtf((A + 1.0f/A) * (1.0f/1.0f - 1.0f) + 2.0f);
    /* Simplified: alpha = sin_w0 / 2 * sqrt(2) for S=1 */
    alpha = sin_w0 / 2.0f * M_SQRT2;
    float two_sqrt_A_alpha = 2.0f * sqrtf(A) * alpha;

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

/*
 * Calculate high shelf filter coefficients
 * Based on Audio EQ Cookbook by Robert Bristow-Johnson
 */
esp_err_t tas5805m_calc_high_shelf(float fc, float gain_db, float fs, tas5805m_biquad_coeffs_t *coeffs)
{
    if (coeffs == NULL || fc <= 0 || fs <= 0 || fc >= fs / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* If gain is essentially zero, return passthrough */
    if (fabsf(gain_db) < 0.01f) {
        return tas5805m_calc_passthrough(coeffs);
    }

    float A = powf(10.0f, gain_db / 40.0f);  /* sqrt of linear gain */
    float w0 = 2.0f * M_PI * fc / fs;
    float sin_w0 = sinf(w0);
    float cos_w0 = cosf(w0);
    /* S = 1 gives a steep shelf */
    float alpha = sin_w0 / 2.0f * M_SQRT2;
    float two_sqrt_A_alpha = 2.0f * sqrtf(A) * alpha;

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

/*
 * Get the number of biquad stages needed for a crossover slope
 */
int tas5805m_biamp_get_biquad_count(tas5805m_biamp_slope_t slope)
{
    switch (slope) {
        case BIAMP_SLOPE_12DB: return 1;
        case BIAMP_SLOPE_24DB: return 2;
        case BIAMP_SLOPE_48DB: return 4;
        default: return 2;  /* Default to 24dB/octave */
    }
}

/*
 * Write biquad coefficients to a specific band
 */
static esp_err_t write_biquad_band(TAS5805M_EQ_CHANNELS channel, int band,
                                    const tas5805m_biquad_coeffs_t *coeffs)
{
    ESP_LOGD(TAG, "Writing biquad ch=%d band=%d: b0=%.6f b1=%.6f b2=%.6f a1=%.6f a2=%.6f",
             channel, band, coeffs->b0, coeffs->b1, coeffs->b2, coeffs->a1, coeffs->a2);

    return tas5805m_write_biquad_coefficients(channel, band,
                                               coeffs->b0, coeffs->b1, coeffs->b2,
                                               coeffs->a1, coeffs->a2);
}

/*
 * Validate and normalize sample rate to a supported value
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

/*
 * Apply bi-amp crossover configuration to TAS5805M
 */
esp_err_t tas5805m_biamp_apply(const tas5805m_biamp_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
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

    /* === LEFT CHANNEL (LOW/WOOFER OUTPUT) === */

    /* Band 0: Gain + optional phase invert for low output */
    float low_total_gain = (float)settings->low_gain;
    if (settings->low_phase_invert) {
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

    /* Band 1: Subsonic HPF (early in chain for optimal headroom) */
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

    /* Bands 2-5: Lowpass filter stages (Butterworth for both BW and LR) */
    ret = tas5805m_calc_butterworth_lpf(fc, fs, &coeffs);
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < num_stages; i++) {
        band = BIAMP_CROSSOVER_START_BAND + i;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Fill remaining crossover bands with passthrough */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    for (int i = num_stages; i < BIAMP_CROSSOVER_MAX_BANDS; i++) {
        band = BIAMP_CROSSOVER_START_BAND + i;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Bands 6-8: PEQ for low output */
    for (int i = 0; i < BIAMP_PEQ_BANDS; i++) {
        band = BIAMP_PEQ_START_BAND + i;
        const tas5805m_biamp_peq_band_t *peq = &settings->low_peq[i];

        if (peq->freq > 0 && peq->freq < fs / 2 && peq->gain != 0) {
            float q = (float)peq->q_x10 / 10.0f;
            ret = tas5805m_calc_peq((float)peq->freq, (float)peq->gain, q, fs, &coeffs);
        } else {
            ret = tas5805m_calc_passthrough(&coeffs);
        }
        if (ret != ESP_OK) return ret;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Fill remaining bands with passthrough (leave band 13 for loudness bass) */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    for (band = BIAMP_PEQ_START_BAND + BIAMP_PEQ_BANDS; band < TAS5805M_LOUDNESS_BASS_BAND; band++) {
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }
    /* Band 14: passthrough (loudness treble not used on woofer) */
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_LEFT, TAS5805M_LOUDNESS_TREBLE_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* === RIGHT CHANNEL (HIGH/TWEETER OUTPUT) === */

    /* Band 0: Gain + optional phase invert for high output */
    float high_total_gain = (float)settings->high_gain;
    if (settings->high_phase_invert) {
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

    /* Band 1: Passthrough (no subsonic needed for tweeter) */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, BIAMP_SUBSONIC_BAND, &coeffs);
    if (ret != ESP_OK) return ret;

    /* Bands 2-5: Highpass filter stages */
    ret = tas5805m_calc_butterworth_hpf(fc, fs, &coeffs);
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < num_stages; i++) {
        band = BIAMP_CROSSOVER_START_BAND + i;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Fill remaining crossover bands with passthrough */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    for (int i = num_stages; i < BIAMP_CROSSOVER_MAX_BANDS; i++) {
        band = BIAMP_CROSSOVER_START_BAND + i;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Bands 6-8: PEQ for high output */
    for (int i = 0; i < BIAMP_PEQ_BANDS; i++) {
        band = BIAMP_PEQ_START_BAND + i;
        const tas5805m_biamp_peq_band_t *peq = &settings->high_peq[i];

        if (peq->freq > 0 && peq->freq < fs / 2 && peq->gain != 0) {
            float q = (float)peq->q_x10 / 10.0f;
            ret = tas5805m_calc_peq((float)peq->freq, (float)peq->gain, q, fs, &coeffs);
        } else {
            ret = tas5805m_calc_passthrough(&coeffs);
        }
        if (ret != ESP_OK) return ret;
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    /* Fill remaining bands with passthrough (leave band 14 for loudness treble) */
    ret = tas5805m_calc_passthrough(&coeffs);
    if (ret != ESP_OK) return ret;
    for (band = BIAMP_PEQ_START_BAND + BIAMP_PEQ_BANDS; band < TAS5805M_LOUDNESS_TREBLE_BAND; band++) {
        ret = write_biquad_band(TAS5805M_EQ_CHANNELS_RIGHT, band, &coeffs);
        if (ret != ESP_OK) return ret;
    }

    ESP_LOGI(TAG, "Bi-amp crossover applied successfully (subsonic=%dHz)", settings->subsonic_freq);
    return ESP_OK;
}

/*
 * Initialize bi-amp settings to defaults
 */
void tas5805m_biamp_init_defaults(tas5805m_biamp_settings_t *settings)
{
    if (settings == NULL) return;

    memset(settings, 0, sizeof(tas5805m_biamp_settings_t));

    settings->crossover_freq = TAS5805M_BIAMP_DEFAULT_XOVER_FREQ;
    settings->slope = TAS5805M_BIAMP_DEFAULT_SLOPE;
    settings->type = TAS5805M_BIAMP_DEFAULT_TYPE;
    settings->sample_rate = TAS5805M_BIAMP_DEFAULT_SAMPLE_RATE;
    settings->subsonic_freq = 0;  /* Disabled by default */

    settings->low_gain = 0;
    settings->low_phase_invert = 0;

    settings->high_gain = 0;
    settings->high_phase_invert = 0;

    /* Initialize PEQ bands to disabled (freq=0) */
    for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS; i++) {
        settings->low_peq[i].freq = 0;
        settings->low_peq[i].gain = 0;
        settings->low_peq[i].q_x10 = 14;  /* Default Q = 1.4 */

        settings->high_peq[i].freq = 0;
        settings->high_peq[i].gain = 0;
        settings->high_peq[i].q_x10 = 14;
    }
}

#endif /* CONFIG_DAC_TAS5805M */

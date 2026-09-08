#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Biquad filter type.
 *
 * More types will be added as the on-the-fly calculation feature is expanded.
 */
typedef enum {
    BQ_FILTER_EQ_Q_FACTOR = 0, /*!< Parametric EQ (uses gain_db and q) */
    BQ_FILTER_LOW_PASS,        /*!< Second-order low-pass  (gain_db ignored)     */
    BQ_FILTER_HIGH_PASS,       /*!< Second-order high-pass  (gain_db ignored)    */
    BQ_FILTER_HIGH_SHELF,      /*!< Second-order high-shelf (uses gain_db and q) */
    BQ_FILTER_LOW_SHELF,       /*!< Second-order low-shelf  (uses gain_db and q) */
} bq_filter_type_t;

/** Common sample-rate constants. */
#define BQ_SAMPLE_RATE_48K  48000U
#define BQ_SAMPLE_RATE_44K1 44100U

/**
 * @brief Normalised biquad coefficients in standard DSP form.
 *
 * Transfer function:
 *   H(z) = (b0 + b1·z⁻¹ + b2·z⁻²) / (1 + a1·z⁻¹ + a2·z⁻²)
 *
 * Difference equation:
 *   y[n] = b0·x[n] + b1·x[n-1] + b2·x[n-2]
 *          - a1·y[n-1] - a2·y[n-2]
 *
 * Sign convention (Audio EQ Cookbook):
 *   - a1 ≈ −2·cos(ω₀)   (typically negative, ≈ −2 at low frequencies)
 *   - a2 ≈ +1            (typically positive)
 *
 * TAS5805M register convention stores −a1 and −a2 (i.e. it adds the
 * feedback terms rather than subtracting them).  Callers targeting
 * TAS5805M must negate a1 and a2 before writing to the DAC registers.
 */

typedef struct {
    double b0;
    double b1;
    double b2;
    double a1; /*!< Standard-convention a1 (negate for TAS5805M) */
    double a2; /*!< Standard-convention a2 (negate for TAS5805M) */
} bq_coeffs_t;

/**
 * @brief Calculate normalised biquad filter coefficients.
 *
 * All coefficients are divided by a0 so the denominator leading term is 1.
 * Uses the Audio EQ Cookbook (R. Bristow-Johnson) formulas.
 *
 * @param type     Filter topology.
 * @param freq_hz  Centre / cutoff frequency in Hz.  Valid range: 1 – 20 000 Hz.
 * @param gain_db  Gain in dB.  Used by BQ_FILTER_EQ_Q_FACTOR only.
 *                 Ignored (may be 0) for LP / HP filters.
 *                 Valid range for peaking EQ: −24 – +24 dB.
 * @param q        Q factor.  Valid range: 0.1 – 20.
 *                 Controls bandwidth for all filter types.
 * @param fs       Sample rate in Hz.  Typically BQ_SAMPLE_RATE_48K (default)
 *                 or BQ_SAMPLE_RATE_44K1.
 * @param[out] c   Output coefficients.  Must not be NULL.
 *
 * @return  0 on success.
 * @return -1 on invalid argument (NULL output pointer, out-of-range frequency,
 *            Q ≤ 0, or zero sample rate).
 */
int bq_calc(bq_filter_type_t type,
            double freq_hz,
            double gain_db,
            double q,
            uint32_t fs,
            bq_coeffs_t *c);

/**
 * @brief Copy double-precision coefficients to individual float outputs.
 *
 * Convenience helper for callers that work with float (e.g. TAS5805M driver).
 *
 * @param src  Input coefficients (from bq_calc).
 * @param b0   Output float b0 – may not be NULL.
 * @param b1   Output float b1 – may not be NULL.
 * @param b2   Output float b2 – may not be NULL.
 * @param a1   Output float a1 – may not be NULL.
 * @param a2   Output float a2 – may not be NULL.
 */
void bq_coeffs_to_float(const bq_coeffs_t *src,
                        float *b0, float *b1, float *b2,
                        float *a1, float *a2);

#ifdef __cplusplus
}
#endif

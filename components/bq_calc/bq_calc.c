/*
 * bq_calc.c — On-the-fly biquad filter coefficient calculator.
 *
 * Formulas are taken directly from the PPC3/TAS5805M web application
 * (biquad.model.js, "diljith" equations used by the UI):
 *
 *  BQ_FILTER_EQ_Q_FACTOR  →  equalizerQFactorCalc()
 *    g = 10^(gain/20)  [linear amplitude ratio, NOT sqrt]
 *    beta = t0/(2·Q)          for boost (g ≥ 1)
 *    beta = t0/(2·g·Q)        for cut   (g < 1)
 *
 *  BQ_FILTER_LOW_PASS    →  lowPassCalc()  "VARIABLE Q 2"
 *  BQ_FILTER_HIGH_PASS   →  highPassCalc() "VARIABLE Q 2"
 *    Bilinear-transform 2nd-order:
 *    wc = 2π·f,  k = wc/tan(π·f/fs),  A1 = wc/Q
 *    denom = k² + A1·k + wc²
 *
 * All output coefficients are in standard DSP form:
 *   y[n] = b0·x[n] + b1·x[n-1] + b2·x[n-2]
 *          - a1·y[n-1] - a2·y[n-2]
 *
 * a1 ≈ −2, a2 ≈ +1 for typical low-frequency filters.
 * TAS5805M stores −a1 and −a2; negate before writing to registers.
 */

#include "bq_calc.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int bq_calc(bq_filter_type_t type,
            double freq_hz,
            double gain_db,
            double q,
            uint32_t fs,
            bq_coeffs_t *c)
{
    if (!c || freq_hz < 1.0 || freq_hz > 20000.0 || q <= 0.0 || fs == 0) {
        return -1;
    }

    switch (type) {

    /*
     * EQUALIZER (Q FACTOR) — equalizerQFactorCalc() from biquad.model.js.
     *
     * The JS function returns a1/a2 in TAS5805M adding-form convention
     * (A1 ≈ +2, A2 ≈ −1).  We negate them for the standard output form.
     * At 0 dB gain (g = 1) the filter is transparent: H(z) = 1.
     */
    case BQ_FILTER_EQ_Q_FACTOR: {
        double g      = pow(10.0, gain_db / 20.0);   /* linear amplitude */
        double t0     = 2.0 * M_PI * freq_hz / (double)fs;
        double cos_t0 = cos(t0);
        double beta   = (g >= 1.0) ? t0 / (2.0 * q)
                                   : t0 / (2.0 * g * q);

        double a2r    = -0.5 * (1.0 - beta) / (1.0 + beta);
        double a1r    = (0.5 - a2r) * cos_t0;
        double tmp    = (g - 1.0) * (0.25 + 0.5 * a2r);

        c->b0 =  2.0 * (tmp + 0.5);
        c->b1 = -2.0 * a1r;      /* equals a1 in standard form for this EQ */
        c->b2 =  2.0 * (-tmp - a2r);
        c->a1 = -2.0 * a1r;      /* negate JS A1 = 2·a1r for standard form */
        c->a2 = -2.0 * a2r;      /* negate JS A2 = 2·a2r for standard form */
        break;
    }

    /*
     * Low-pass — lowPassCalc() "VARIABLE Q 2" from biquad.model.js.
     *
     * JS returns a1/a2 in TAS5805M adding-form convention; we negate.
     */
    case BQ_FILTER_LOW_PASS: {
        double wc    = 2.0 * M_PI * freq_hz;
        double k     = wc / tan(M_PI * freq_hz / (double)fs);
        double A1_ct = wc / q;
        double wc2   = wc * wc;
        double k2    = k * k;
        double denom = k2 + A1_ct * k + wc2;

        c->b0 =  wc2 / denom;
        c->b1 =  2.0 * wc2 / denom;
        c->b2 =  wc2 / denom;
        c->a1 = -(2.0 * (k2 - wc2)) / denom;   /* negate JS A1 */
        c->a2 = -((A1_ct * k - k2 - wc2)) / denom; /* negate JS A2 */
        break;
    }

    /*
     * High-pass — highPassCalc() "VARIABLE Q 2" from biquad.model.js.
     *
     * Same denominator as low-pass; numerator uses k² instead of wc².
     */
    case BQ_FILTER_HIGH_PASS: {
        double wc    = 2.0 * M_PI * freq_hz;
        double k     = wc / tan(M_PI * freq_hz / (double)fs);
        double A1_ct = wc / q;
        double wc2   = wc * wc;
        double k2    = k * k;
        double denom = k2 + A1_ct * k + wc2;

        c->b0 =  k2 / denom;
        c->b1 = -2.0 * k2 / denom;
        c->b2 =  k2 / denom;
        c->a1 = -(2.0 * (k2 - wc2)) / denom;   /* negate JS A1 */
        c->a2 = -((A1_ct * k - k2 - wc2)) / denom; /* negate JS A2 */
        break;
    }

    /*
     * Low-shelf — lowShelfCalc() from biquad.model.js.
     *
     * ao = (A+1) + (A-1)·cos(wo) + 2·sqrt(A)·alpha   (note: + sign before (A-1))
     *
     * JS returns A1/A2 in TAS5805M adding-form convention; negate for standard:
     * a1 = -2·[(A-1) + (A+1)·cos(wo)] / ao
     * a2 =  [(A+1) + (A-1)·cos(wo) - 2·sqrt(A)·alpha] / ao
     */
    case BQ_FILTER_LOW_SHELF: {
        double A      = sqrt(pow(10.0, gain_db / 20.0));
        double wo     = 2.0 * M_PI * freq_hz / (double)fs;
        double cos_wo = cos(wo);
        double sin_wo = sin(wo);
        double alpha  = sin_wo / (2.0 * q);
        double sqrtA  = sqrt(A);
        double ao     = (A + 1.0) + (A - 1.0) * cos_wo + 2.0 * sqrtA * alpha;

        c->b0 = A * ((A + 1.0) - (A - 1.0) * cos_wo + 2.0 * sqrtA * alpha) / ao;
        c->b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_wo) / ao;
        c->b2 = A * ((A + 1.0) - (A - 1.0) * cos_wo - 2.0 * sqrtA * alpha) / ao;
        c->a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_wo) / ao;
        c->a2 = ((A + 1.0) + (A - 1.0) * cos_wo - 2.0 * sqrtA * alpha) / ao;
        break;
    }

    /*
     * High-shelf — highShelfCalc() from biquad.model.js.
     *
     * A  = sqrt(10^(gain_db/20))
     * wo = 2π·f/fs
     * alpha = sin(wo) / (2·Q)
     * ao = (A+1) - (A-1)·cos(wo) + 2·sqrt(A)·alpha
     *
     * JS returns A1/A2 in TAS5805M adding-form convention (+2, -1 region).
     * JS A1 = -2·[(A-1) - (A+1)·cos(wo)] / ao
     * JS A2 = -[(A+1) - (A-1)·cos(wo) - 2·sqrt(A)·alpha] / ao
     * Standard form (negate JS A1, JS A2):
     * a1 = 2·[(A-1) - (A+1)·cos(wo)] / ao
     * a2 = [(A+1) - (A-1)·cos(wo) - 2·sqrt(A)·alpha] / ao
     */
    case BQ_FILTER_HIGH_SHELF: {
        double A      = sqrt(pow(10.0, gain_db / 20.0));
        double wo     = 2.0 * M_PI * freq_hz / (double)fs;
        double cos_wo = cos(wo);
        double sin_wo = sin(wo);
        double alpha  = sin_wo / (2.0 * q);
        double sqrtA  = sqrt(A);
        double ao     = (A + 1.0) - (A - 1.0) * cos_wo + 2.0 * sqrtA * alpha;

        c->b0 = A * ((A + 1.0) + (A - 1.0) * cos_wo + 2.0 * sqrtA * alpha) / ao;
        c->b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_wo) / ao;
        c->b2 = A * ((A + 1.0) + (A - 1.0) * cos_wo - 2.0 * sqrtA * alpha) / ao;
        c->a1 =  2.0 * ((A - 1.0) - (A + 1.0) * cos_wo) / ao;
        c->a2 = ((A + 1.0) - (A - 1.0) * cos_wo - 2.0 * sqrtA * alpha) / ao;
        break;
    }

    default:
        return -1;
    }

    return 0;
}

void bq_coeffs_to_float(const bq_coeffs_t *src,
                        float *b0, float *b1, float *b2,
                        float *a1, float *a2)
{
    *b0 = (float)src->b0;
    *b1 = (float)src->b1;
    *b2 = (float)src->b2;
    *a1 = (float)src->a1;
    *a2 = (float)src->a2;
}

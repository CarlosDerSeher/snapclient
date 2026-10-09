#pragma once

#include <stdint.h>

#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)

#ifdef __cplusplus
extern "C" {
#endif

/** Number of parametric EQ bands. */
#define PCM51XX_EQ_BANDS    6

/**
 * @brief Global gain range (dB) for all parametric EQ bands.
 */
#define PCM51XX_EQ_MAX_DB   15
#define PCM51XX_EQ_MIN_DB   (-PCM51XX_EQ_MAX_DB)

/** Number of biquad coefficients per band (B0, B1, B2, A1, A2). */
#define PCM51XX_EQ_KOEF_PER_BAND  5

/**
 * @brief Per-band EQ configuration.
 *
 * All parameters required to compute biquad coefficients on-the-fly and to
 * populate the UI schema are stored here.
 */
typedef struct {
    uint16_t          freq_hz;     /*!< Centre / corner frequency (Hz)     */
    float             q;           /*!< Q factor (bandwidth parameter)      */
    int8_t            min_db;      /*!< Minimum gain step (dB, negative)    */
    int8_t            max_db;      /*!< Maximum gain step (dB, positive)    */
    uint8_t           filter_type; /*!< Filter topology — bq_filter_type_t value (see bq_calc.h) */
} pcm51xx_eq_band_cfg_t;

/**
 * @brief EQ band configuration table.
 *
 * Defined in pcm51xx.c.  PCM51XX_EQ_BANDS entries, one per band.
 */
extern const pcm51xx_eq_band_cfg_t pcm51xx_eq_band_cfg[PCM51XX_EQ_BANDS];

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_DAC_PCM51XX_EQ_SUPPORT */

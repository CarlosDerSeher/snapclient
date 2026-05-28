#pragma once

#include <stdint.h>

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)

#ifdef __cplusplus
extern "C" {
#endif

/** Number of parametric EQ bands. */
#define TAS5805M_EQ_BANDS    15

/**
 * @brief Global gain range (dB) used by all bands unless overridden per-band.
 *
 * The LUT was generated for ±15 dB in 1 dB steps.  The macros are kept so
 * that the LUT-indexing arithmetic (gain + TAS5805M_EQ_MAX_DB) continues to
 * work without modification.
 */
#define TAS5805M_EQ_MAX_DB   15
#define TAS5805M_EQ_MIN_DB   (-TAS5805M_EQ_MAX_DB)

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
} tas5805m_eq_band_cfg_t;

/**
 * @brief EQ band configuration table.
 *
 * Defined in tas5805m.c.  15 entries, one per band.
 */
extern const tas5805m_eq_band_cfg_t tas5805m_eq_band_cfg[TAS5805M_EQ_BANDS];

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_DAC_TAS5805M_EQ_SUPPORT */

#pragma once

#include <stdint.h>
#include "tas5805m_types.h"

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)

#ifdef __cplusplus
extern "C" {
#endif

/** Number of parametric EQ bands. */
#define TAS5805M_EQ_BANDS    15

/**
 * @brief Global gain range (dB) for all parametric EQ bands.
 */
#define TAS5805M_EQ_MAX_DB   15
#define TAS5805M_EQ_MIN_DB   (-TAS5805M_EQ_MAX_DB)

/** Number of biquad coefficients per band (B0, B1, B2, A1, A2). */
#define TAS5805M_EQ_KOEF_PER_BAND  5

/** Number of biquad slots used per profile (BQ1 + BQ2 + BQ3). */
#define TAS5805M_EQ_PROFILE_BANDS  3

/** DSP coefficient RAM book number (Book 0xAA). */
#define TAS5805M_REG_BOOK_EQ       0xaa

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

/**
 * @brief Sentinel value for a profile band: write identity (bypass) biquad.
 *
 * When ftype_12 or bq3_ftype is TAS5805M_EQ_PROF_BYPASS the corresponding
 * biquad section is filled with b0=1, b1=b2=a1=a2=0 without calling bq_calc.
 */
#define TAS5805M_EQ_PROF_BYPASS  0xFFU

/**
 * @brief Configuration for one EQ profile (preset loudspeaker response).
 *
 * BQ1 and BQ2 implement a cascaded 4th-order HP or LP filter.  BQ3 is
 * always written as an identity biquad (bypass).
 *
 * ftype holds a bq_filter_type_t value cast to uint8_t, or
 * TAS5805M_EQ_PROF_BYPASS to write identity to all three sections (FLAT).
 * This header does not need to include bq_calc.h.
 */
typedef struct {
    uint8_t  ftype;    /*!< bq_filter_type_t for BQ1/BQ2, or TAS5805M_EQ_PROF_BYPASS */
    uint16_t freq_hz;  /*!< Corner frequency (Hz)                                      */
    float    q;        /*!< Q factor                                                    */
} tas5805m_eq_profile_cfg_t;

/**
 * @brief EQ profile configuration table.
 *
 * Defined in tas5805m.c.  TAS5805M_EQ_PROFILES entries, one per profile.
 */
extern const tas5805m_eq_profile_cfg_t tas5805m_eq_profile_cfg[TAS5805M_EQ_PROFILES];

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_DAC_TAS5805M_EQ_SUPPORT */

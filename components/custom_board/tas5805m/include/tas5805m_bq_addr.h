#pragma once

#include <stdint.h>

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT) && defined(CONFIG_DAC_TAS5805M_EQ_BQ_CALC)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compact BQ coefficient register address table.
 *
 * The DSP coefficient RAM layout is fixed — only the coefficient values
 * change per gain setting. This table stores one (page, base_offset) pair
 * per EQ band. Given that pair, the address of coefficient ci (0..4) is:
 *
 *   abs = base_offset + ci * 4
 *   if (abs < 0x80):  page = base_page,     offset = abs
 *   else:             page = base_page + 1,  offset = abs - 0x78
 *
 * The subtraction 0x78 accounts for the TAS5805M register layout where
 * each page's coefficient area begins at offset 0x08 (i.e. 0x80 in the
 * next page maps to offset 0x08, so delta = 0x80 - 0x08 = 0x78).
 *
 * Address tables were extracted from the LUT headers and verified to be
 * identical across all 31 gain steps (except the corrupt _mf entry for
 * TAS5805M left which has 240 instead of 300 entries).
 */
typedef struct {
    uint8_t page;
    uint8_t base_offset;
} tas5805m_bq_band_addr_t;

/* Compute the exact (page, offset) for coefficient ci of a band. */
static inline void tas5805m_bq_coeff_addr(const tas5805m_bq_band_addr_t *band_addr,
                                           int ci,
                                           uint8_t *page_out,
                                           uint8_t *offset_out)
{
    uint16_t abs = (uint16_t)band_addr->base_offset + (uint16_t)(ci * 4);
    if (abs >= 0x80u) {
        *page_out   = (uint8_t)(band_addr->page + 1u);
        *offset_out = (uint8_t)(abs - 0x78u);
    } else {
        *page_out   = band_addr->page;
        *offset_out = (uint8_t)abs;
    }
}

/* -----------------------------------------------------------------------
 * TAS5805M  (15 bands, left channel then right channel)
 * Extracted from tas5805m_eq_registers_left / _right (ref step: gain=0)
 * ----------------------------------------------------------------------- */
static const tas5805m_bq_band_addr_t tas5805m_bq_addr_left[15] = {
    /* band  0  20 Hz */ {0x24, 0x18},
    /* band  1  32 Hz */ {0x24, 0x2c},
    /* band  2  50 Hz */ {0x24, 0x40},
    /* band  3  80 Hz */ {0x24, 0x54},
    /* band  4 125 Hz */ {0x24, 0x68},
    /* band  5 200 Hz */ {0x24, 0x7c},
    /* band  6 315 Hz */ {0x25, 0x18},
    /* band  7 500 Hz */ {0x25, 0x2c},
    /* band  8 800 Hz */ {0x25, 0x40},
    /* band  9 1.25kHz */ {0x25, 0x54},
    /* band 10 2 kHz  */ {0x25, 0x68},
    /* band 11 3.15kHz */ {0x25, 0x7c},
    /* band 12 5 kHz  */ {0x26, 0x18},
    /* band 13 8 kHz  */ {0x26, 0x2c},
    /* band 14 16 kHz */ {0x26, 0x40},
};

static const tas5805m_bq_band_addr_t tas5805m_bq_addr_right[15] = {
    /* band  0  20 Hz */ {0x26, 0x54},
    /* band  1  32 Hz */ {0x26, 0x68},
    /* band  2  50 Hz */ {0x26, 0x7c},
    /* band  3  80 Hz */ {0x27, 0x18},
    /* band  4 125 Hz */ {0x27, 0x2c},
    /* band  5 200 Hz */ {0x27, 0x40},
    /* band  6 315 Hz */ {0x27, 0x54},
    /* band  7 500 Hz */ {0x27, 0x68},
    /* band  8 800 Hz */ {0x27, 0x7c},
    /* band  9 1.25kHz */ {0x28, 0x18},
    /* band 10 2 kHz  */ {0x28, 0x2c},
    /* band 11 3.15kHz */ {0x28, 0x40},
    /* band 12 5 kHz  */ {0x28, 0x54},
    /* band 13 8 kHz  */ {0x28, 0x68},
    /* band 14 16 kHz */ {0x28, 0x7c},
};

/* -----------------------------------------------------------------------
 * TAS5825M  (15 bands, left channel then right channel)
 * Extracted from tas5825m_eq_registers_left / _right (ref step: gain=0)
 * ----------------------------------------------------------------------- */
static const tas5805m_bq_band_addr_t tas5825m_bq_addr_left[15] = {
    /* band  0  20 Hz */ {0x01, 0x30},
    /* band  1  32 Hz */ {0x01, 0x44},
    /* band  2  50 Hz */ {0x01, 0x58},
    /* band  3  80 Hz */ {0x01, 0x6c},
    /* band  4 125 Hz */ {0x02, 0x08},
    /* band  5 200 Hz */ {0x02, 0x1c},
    /* band  6 315 Hz */ {0x02, 0x30},
    /* band  7 500 Hz */ {0x02, 0x44},
    /* band  8 800 Hz */ {0x02, 0x58},
    /* band  9 1.25kHz */ {0x02, 0x6c},
    /* band 10 2 kHz  */ {0x03, 0x08},
    /* band 11 3.15kHz */ {0x03, 0x1c},
    /* band 12 5 kHz  */ {0x03, 0x30},
    /* band 13 8 kHz  */ {0x03, 0x44},
    /* band 14 16 kHz */ {0x03, 0x58},
};

static const tas5805m_bq_band_addr_t tas5825m_bq_addr_right[15] = {
    /* band  0  20 Hz */ {0x03, 0x6c},
    /* band  1  32 Hz */ {0x04, 0x08},
    /* band  2  50 Hz */ {0x04, 0x1c},
    /* band  3  80 Hz */ {0x04, 0x30},
    /* band  4 125 Hz */ {0x04, 0x44},
    /* band  5 200 Hz */ {0x04, 0x58},
    /* band  6 315 Hz */ {0x04, 0x6c},
    /* band  7 500 Hz */ {0x05, 0x08},
    /* band  8 800 Hz */ {0x05, 0x1c},
    /* band  9 1.25kHz */ {0x05, 0x30},
    /* band 10 2 kHz  */ {0x05, 0x44},
    /* band 11 3.15kHz */ {0x05, 0x58},
    /* band 12 5 kHz  */ {0x05, 0x6c},
    /* band 13 8 kHz  */ {0x06, 0x08},
    /* band 14 16 kHz */ {0x06, 0x1c},
};

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_DAC_TAS5805M_EQ_SUPPORT && CONFIG_DAC_TAS5805M_EQ_BQ_CALC */

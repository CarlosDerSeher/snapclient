#pragma once

#include <stdint.h>
#include "pcm51xx_eq_config.h"

#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PCM5122 BQ coefficient register address table.
 *
 * The DSP coefficient RAM is organised in pages. Each page exposes
 * coefficient slots at register offsets 0x08..0x7B (4 bytes each, 30 slots
 * per page).  A band address stores the (page, base_offset) of coefficient
 * index 0 (B0) for that band; the address of coefficient ci (0..4) is then:
 *
 *   abs = base_offset + ci * 4
 *   if (abs < 0x80):  page = base_page,     offset = abs
 *   else:             page = base_page + 1,  offset = abs - 0x78
 *
 * The subtraction 0x78 accounts for the coefficient area starting at
 * offset 0x08 on each page (0x80 - 0x08 = 0x78).
 *
 * PCM5122 specifics:
 *   - Only ONE address table: both channels share the same BQ coefficients.
 *   - 6 bands (PCM51XX_EQ_BANDS) instead of 15.
 *   - Internal bank switching (A/B double-buffer) is handled automatically
 *     by the device when the process flow is toggled — we write to a single
 *     bank only.
 *
 * Addresses verified from PCM5122 datasheet coefficient memory map:
 *   The 6 programmable BQ sections occupy coefficients c10–c39 (5 per band).
 *   c10 starts at page 44 (0x2C), register 0x30 (decimal 48).
 *   c39 ends  at page 45 (0x2D), register 0x2F (last byte, decimal 47).
 */
typedef struct {
    uint8_t page;
    uint8_t base_offset;
} pcm51xx_bq_band_addr_t;

/** Compute the exact (page, offset) for coefficient ci of a band. */
static inline void pcm51xx_bq_coeff_addr(const pcm51xx_bq_band_addr_t *band_addr,
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
 * PCM5122 EQ BQ sections: c10–c39, page 44 (0x2C) reg 0x30 .. page 45 (0x2D) reg 0x2C.
 * Each band: 5 coefficients × 4 bytes = 20 bytes (B0, B1, B2, A1, A2).
 * ----------------------------------------------------------------------- */
static const pcm51xx_bq_band_addr_t pcm51xx_bq_addr[PCM51XX_EQ_BANDS] = {
    /* band 0 (BQ1)  */ {0x2C, 0x30},  /* c10..c14, page 44 reg 0x30 */
    /* band 1 (BQ2)  */ {0x2C, 0x44},  /* c15..c19, page 44 reg 0x44 */
    /* band 2 (BQ3)  */ {0x2C, 0x58},  /* c20..c24, page 44 reg 0x58 */
    /* band 3 (BQ4)  */ {0x2C, 0x6C},  /* c25..c29, page 44 reg 0x6C */
    /* band 4 (BQ5)  */ {0x2D, 0x08},  /* c30..c34, page 45 reg 0x08 */
    /* band 5 (BQ6)  */ {0x2D, 0x1C},  /* c35..c39, page 45 reg 0x1C */
};

/* -----------------------------------------------------------------------
 * PCM5122 DRC BQ sections: c40–c69.
 *   c40–c59  page 45 (0x2D) reg 0x30..0x7C  (no page crossing)
 *   c60–c69  page 45 base ≥ 0x80 → formula maps to page 46 (0x2E)
 *            base 0x80 → page 46 reg 0x08;  base 0x94 → page 46 reg 0x1C
 *            last byte of c69 = page 46 reg 0x2F  (decimal 47)
 * These BQs are set to safe defaults (1,0,0,0,0) during DSP init and are
 * otherwise left untouched (DRC not used).
 * ----------------------------------------------------------------------- */
#define PCM51XX_DRC_BQ_BANDS  6

static const pcm51xx_bq_band_addr_t pcm51xx_drc_bq_addr[PCM51XX_DRC_BQ_BANDS] = {
    /* band  6 (DRC BQ1) */ {0x2D, 0x30},  /* c40..c44, page 45 reg 0x30 */
    /* band  7 (DRC BQ2) */ {0x2D, 0x44},  /* c45..c49, page 45 reg 0x44 */
    /* band  8 (DRC BQ3) */ {0x2D, 0x58},  /* c50..c54, page 45 reg 0x58 */
    /* band  9 (DRC BQ4) */ {0x2D, 0x6C},  /* c55..c59, page 45 reg 0x6C */
    /* band 10 (DRC BQ5) */ {0x2E, 0x08},  /* c60..c64, page 46 reg 0x08 */
    /* band 11 (DRC BQ6) */ {0x2E, 0x1C},  /* c65..c69, page 46 reg 0x1C */
};

/* Page-0 register used to select the coefficient page on the PCM5122. */
#define PCM51XX_PAGE_SELECT_REG  0x00u

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_DAC_PCM51XX_EQ_SUPPORT */

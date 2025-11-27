/*
 * MIT License
 *
 * Copyright (c) 2021 David Douard <david.douard@sdfa3.org>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in
 * which case, it is free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the
 * Software without restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef _PCM51XX_REG_CFG_
#define _PCM51XX_REG_CFG_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t offset;
  uint8_t value;
} pcm51xx_cfg_reg_t;

static const pcm51xx_cfg_reg_t pcm51xx_init_seq[] = {

    // EXIT SHUTDOWN STATE
    {0x00, 0x00},  // SELECT PAGE 0
    {0x03, 0x00},  // UNMUTE
    {0x2a, 0x11},  // DAC DATA PATH L->ch1, R->ch2
    {0x02, 0x00},  // DISABLE STBY
    {0x0d, 0x10},  // BCK as SRC for PLL
    {0x25, 0x08},  // IGNORE MISSING MCLK
    {0x3d, 0x30},  // DIGITAL VOLUME L
    {0x3e, 0x30},  // DIGITAL VOLUME R
};

#ifdef __cplusplus
}
#endif

#endif

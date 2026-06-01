/*
 * MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
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

#include "pcm51xx.h"

#include <inttypes.h>
#include "board.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "pcm51xx_reg_cfg.h"
#include "driver/gpio.h"

#ifdef CONFIG_DAC_PCM51XX_EQ_SUPPORT
static void pcm51xx_eq_test_task(void *arg);
#endif

static const char *TAG = "PCM51XX";

// Volume range in percentage
#define PCM51XX_VOLUME_MAX 100
#define PCM51XX_VOLUME_MIN 0

// PCM51XX register values
#define PCM51XX_REG_VAL_0DB    0x30  // 48 decimal = 0dB
#define PCM51XX_REG_VAL_MUTE   0xFF  // 255 decimal = mute

// GPIO mute pin configuration
#ifdef CONFIG_PCM51XX_MUTE_PIN
#define PCM51XX_MUTE_PIN CONFIG_PCM51XX_MUTE_PIN
#else
#define PCM51XX_MUTE_PIN 255
#endif

#define PCM51XX_MUTE_PIN_VALID(pin) ((pin) != 255)

#define PCM51XX_ASSERT(a, format, b, ...) \
  if ((a) != 0) {                         \
    ESP_LOGE(TAG, format, ##__VA_ARGS__); \
    return b;                             \
  }

esp_err_t pcm51xx_ctrl(audio_hal_codec_mode_t mode,
                       audio_hal_ctrl_t ctrl_state);
esp_err_t pcm51xx_config_iface(audio_hal_codec_mode_t mode,
                               audio_hal_codec_i2s_iface_t *iface);
static i2c_bus_handle_t i2c_handler;
// CONFIG_DAC_I2C_ADDR is 7-bit address, but i2c_bus functions expect 8-bit (shifted) address
static const int pcm51xx_addr = (CONFIG_DAC_I2C_ADDR << 1);

// State tracking
static struct {
  int volume_percent;     // Current volume in percentage (0-100)
  bool is_muted;          // Current mute state
#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)
  int8_t eq_gain[6];      // Per-band EQ gain in dB (PCM51XX_EQ_BANDS = 6)
#endif
} pcm51xx_state = {
  .volume_percent = 100,  // Default to 100%
  .is_muted = true,       // Default to muted
#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)
  .eq_gain = {0, 0, 0, 0, 0, 0},
#endif
};

/*
 * i2c default configuration
 */
static i2c_config_t i2c_cfg = {
    .mode = I2C_MODE_MASTER,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 100000,
};

/*
 * Operate function
 */
audio_hal_func_t AUDIO_CODEC_PCM51XX_DEFAULT_HANDLE = {
    .audio_codec_initialize = pcm51xx_init,
    .audio_codec_deinitialize = pcm51xx_deinit,
    .audio_codec_ctrl = pcm51xx_ctrl,
    .audio_codec_config_iface = pcm51xx_config_iface,
    .audio_codec_set_mute = pcm51xx_set_mute,
    .audio_codec_set_volume = pcm51xx_set_volume,
    .audio_codec_get_volume = pcm51xx_get_volume,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)
static esp_err_t pcm51xx_dsp_init(void);
#endif

static esp_err_t pcm51xx_transmit_registers(const pcm51xx_cfg_reg_t *conf_buf,
                                            int size) {
  ESP_LOGD(TAG, "%s: size=%d", __func__, size);
  int i = 0;
  esp_err_t ret = ESP_OK;
  while (i < size) {
    ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr,
                              (unsigned char *)(&conf_buf[i].offset), 1,
                              (unsigned char *)(&conf_buf[i].value), 1);
    i++;
  }
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Fail to load configuration to pcm51xx");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "%s:  write %d reg done", __FUNCTION__, i);
  return ret;
}

esp_err_t pcm51xx_init(audio_hal_codec_config_t *codec_cfg) {
  ESP_LOGD(TAG, "%s: codec_cfg=%p", __func__, codec_cfg);
  esp_err_t ret = ESP_OK;

  ret = get_i2c_pins(I2C_NUM_0, &i2c_cfg);
  ESP_LOGI(TAG, "PCM51XX I2C pins set: SDA=%d, SCL=%d", i2c_cfg.sda_io_num,
           i2c_cfg.scl_io_num);
  i2c_handler = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
  if (i2c_handler == NULL) {
    ESP_LOGW(TAG, "failed to create i2c bus handler\n");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Using pcm51xx chip at address 0x%x", pcm51xx_addr);

  // Initialize GPIO mute pin if configured
  if (PCM51XX_MUTE_PIN_VALID(PCM51XX_MUTE_PIN)) {
    gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << PCM51XX_MUTE_PIN),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
      // Initialize to muted state (LOW)
      gpio_set_level(PCM51XX_MUTE_PIN, 0);
      ESP_LOGI(TAG, "PCM51XX GPIO mute pin %d configured", PCM51XX_MUTE_PIN);
    } else {
      ESP_LOGW(TAG, "Failed to configure GPIO mute pin %d", PCM51XX_MUTE_PIN);
    }
  } else {
    ESP_LOGI(TAG, "PCM51XX GPIO mute pin disabled (using register control only)");
  }

  // Reset DAC using PCM51XX_REG_RESET register
  uint8_t reset_cmd[2] = {PCM51XX_REG_RESET, 0x11}; // Write 0x11 to reset register
  ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &reset_cmd[0], 1, &reset_cmd[1], 1);
  PCM51XX_ASSERT(ret, "Failed to reset PCM51XX using register", ESP_FAIL);
  vTaskDelay(pdMS_TO_TICKS(100)); // Delay to allow reset to complete

  reset_cmd[1] = 0x00; // Clear reset flags
  ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &reset_cmd[0], 1, &reset_cmd[1], 1); 
  PCM51XX_ASSERT(ret, "Failed to un-reset PCM51XX using register", ESP_FAIL);
  vTaskDelay(pdMS_TO_TICKS(100)); // Delay to allow reset to complete
  PCM51XX_ASSERT(ret, "Fail to detect pcm51xx PA", ESP_FAIL);

  ret |= pcm51xx_transmit_registers(
      pcm51xx_init_seq, sizeof(pcm51xx_init_seq) / sizeof(pcm51xx_init_seq[0]));
  PCM51XX_ASSERT(ret, "Fail to iniitialize pcm51xx PA", ESP_FAIL);

#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)
  xTaskCreate(pcm51xx_eq_test_task, "pcm51xx_eq_test", 4096, NULL, 5, NULL);
#endif

  return ret;
}

esp_err_t pcm51xx_set_volume(int vol) {
  ESP_LOGD(TAG, "%s: vol=%d%%", __func__, vol);
  // vol is given as percentage (0-100%)
  // Input range: 0% (min) to 100% (max/0dB)
  // 
  // PCM51XX register mapping (1/2dB steps):
  // 0x30 (48 decimal):  0dB    <- 100%
  // 0xFF (255):        -inf (mute) <- 0%
  //
  // Inverted mapping: higher percentage = higher register value (less attenuation)

  // Clamp input to valid range
  if (vol < 0) {
    vol = 0;
  }
  if (vol > 100) {
    vol = 100;
  }

  uint8_t register_value;
  
  if (vol == 0) {
    // 0% = mute
    register_value = 0xFF;
  } else {
    // Map 1-100% to register values 0xFE-0x30
    // Linear mapping: 100% -> 0x30 (48), 1% -> 0xFE (254)
    // Formula: reg_val = 0x30 + (100 - vol) * (0xFE - 0x30) / 99
    //        = 48 + (100 - vol) * 206 / 99
    register_value = 0x30 + ((100 - vol) * 206) / 99;
  }

  uint8_t cmd[2] = {0, 0};
  esp_err_t ret = ESP_OK;

  cmd[1] = register_value;

  cmd[0] = PCM51XX_REG_VOL_L;
  ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &cmd[0], 1, &cmd[1], 1);
  cmd[0] = PCM51XX_REG_VOL_R;
  ret |= i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &cmd[0], 1, &cmd[1], 1);
  
  if (ret == ESP_OK) {
    // Store the volume in state for later retrieval
    pcm51xx_state.volume_percent = vol;
  }
  
  ESP_LOGD(TAG, "Volume set to %d%% (register: 0x%02x)", vol, register_value);
  return ret;
}

esp_err_t pcm51xx_get_volume(int *value) {
  ESP_LOGD(TAG, "%s: value=%p", __func__, value);
  
  if (value == NULL) {
    ESP_LOGE(TAG, "Null pointer provided for volume value");
    return ESP_FAIL;
  }
  
  // Return the stored volume percentage
  *value = pcm51xx_state.volume_percent;
  ESP_LOGD(TAG, "Volume is %d%%", *value);
  
  return ESP_OK;
}

esp_err_t pcm51xx_set_mute(bool enable) {
  ESP_LOGD(TAG, "%s: enable=%d", __func__, enable);
  esp_err_t ret = ESP_OK;
  
  // Control I2C register-based mute
  uint8_t cmd[2] = {PCM51XX_REG_MUTE, 0x00};
  ret |= i2c_bus_read_bytes(i2c_handler, pcm51xx_addr, &cmd[0], 1, &cmd[1], 1);

  if (enable) {
    cmd[1] |= 0x11;
  } else {
    cmd[1] &= (~0x11);
  }
  ret |= i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &cmd[0], 1, &cmd[1], 1);

  // Control GPIO mute pin if configured
  if (PCM51XX_MUTE_PIN_VALID(PCM51XX_MUTE_PIN)) {
    // Set pin HIGH for unmute, LOW for mute
    gpio_set_level(PCM51XX_MUTE_PIN, enable ? 0 : 1);
    ESP_LOGD(TAG, "GPIO mute pin %d set to %d", PCM51XX_MUTE_PIN, enable ? 0 : 1);
  }

  PCM51XX_ASSERT(ret, "Fail to set mute", ESP_FAIL);
  
  // Store mute state
  pcm51xx_state.is_muted = enable;
  
  ESP_LOGI(TAG, "Mute %s (register + GPIO)", enable ? "enabled" : "disabled");
  return ret;
}

esp_err_t pcm51xx_get_mute(bool *enabled) {
  ESP_LOGD(TAG, "%s: enabled=%p", __func__, enabled);
  
  if (enabled == NULL) {
    ESP_LOGE(TAG, "Null pointer provided for mute value");
    return ESP_FAIL;
  }
  
  // Return the stored mute state
  *enabled = pcm51xx_state.is_muted;
  ESP_LOGI(TAG, "Get mute value: %s", *enabled ? "muted" : "unmuted");
  
  return ESP_OK;
}

esp_err_t pcm51xx_deinit(void) {
  ESP_LOGD(TAG, "%s", __func__);
  // TODO
  return ESP_OK;
}

esp_err_t pcm51xx_ctrl(audio_hal_codec_mode_t mode,
                       audio_hal_ctrl_t ctrl_state) {
  ESP_LOGD(TAG, "%s: mode=%d, ctrl_state=%d", __func__, mode, ctrl_state);
  // TODO
  return ESP_OK;
}

esp_err_t pcm51xx_config_iface(audio_hal_codec_mode_t mode,
                               audio_hal_codec_i2s_iface_t *iface) {
  ESP_LOGD(TAG, "%s: mode=%d, iface=%p", __func__, mode, iface);
  // TODO
  return ESP_OK;
}

/* =========================================================================
 * Parametric EQ — compile only when enabled in Kconfig
 * ========================================================================= */
#if defined(CONFIG_DAC_PCM51XX_EQ_SUPPORT)

#include "bq_calc.h"
#include "pcm51xx_bq_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * EQ band configuration table — the single source of truth for all per-band
 * parameters.  Edit this array to change frequencies, Q factors, gain limits
 * or filter topology.
 *
 * 6 bands spread across the audio spectrum for a full-range PCM5122 system.
 */
const pcm51xx_eq_band_cfg_t pcm51xx_eq_band_cfg[PCM51XX_EQ_BANDS] = {
    /* freq_hz   q      min_db  max_db  filter_type */
    {    63,   1.5f,   -15,    15,   BQ_FILTER_EQ_Q_FACTOR },
    {   250,   1.0f,   -15,    15,   BQ_FILTER_EQ_Q_FACTOR },
    {  1000,   0.9f,   -15,    15,   BQ_FILTER_EQ_Q_FACTOR },
    {  4000,   0.8f,   -15,    15,   BQ_FILTER_EQ_Q_FACTOR },
    {  8000,   0.7f,   -15,    15,   BQ_FILTER_EQ_Q_FACTOR },
    { 16000,   0.6f,   -15,    15,   BQ_FILTER_EQ_Q_FACTOR },
};

/*
 * Select a DSP coefficient page on the PCM5122.
 * Write page number to register 0x00 on page 0 first (re-select page 0),
 * then write the target page.  PCM5122 has no "book" register — page select
 * is always register 0x00.
 */
static esp_err_t pcm51xx_select_page(uint8_t page)
{
    uint8_t reg = PCM51XX_PAGE_SELECT_REG;
    return i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &reg, 1, &page, 1);
}

/*
 * Restore register context to page 0 after DSP coefficient writes.
 */
static esp_err_t pcm51xx_restore_page0(void)
{
    return pcm51xx_select_page(0x00u);
}

/*
 * Enter standby mode before writing DSP coefficient RAM.
 *
 * The TI PPC3 init sequence writes all BQ coefficients while the device
 * is in standby and exits standby only after the last coefficient write.
 * The same hold applies to run-time EQ updates: the process flow must be
 * paused so the DSP picks up new coefficients atomically on resume.
 */
static esp_err_t pcm51xx_enter_standby(void)
{
    ESP_LOGD(TAG, "%s", __func__);
    uint8_t reg = PCM51XX_REG_PMOD;
    uint8_t val = PCM51XX_PMOD_STANDBY;
    esp_err_t ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr,
                                        &reg, 1, &val, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: failed", __func__);
    }
    return ret;
}

/*
 * Exit standby mode after DSP coefficient RAM writes are complete.
 */
static esp_err_t pcm51xx_exit_standby(void)
{
    ESP_LOGD(TAG, "%s", __func__);
    uint8_t reg = PCM51XX_REG_PMOD;
    uint8_t val = PCM51XX_PMOD_NORMAL;
    esp_err_t ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr,
                                        &reg, 1, &val, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: failed", __func__);
    }
    return ret;
}

/*
 * Write all 5 coefficients of one biquad band to the PCM5122 DSP RAM.
 * Takes a direct address struct — works for both EQ and DRC bands.
 * Expects coefficients in PCM5122 register format:
 *   [b0, b1/2, b2, −a1/2, −a2]  (Q2.30, range [−2, +2)).
 * Internal A/B bank switching is automatic — we write to a single bank.
 */
static esp_err_t pcm51xx_write_band_by_addr(const pcm51xx_bq_band_addr_t *addr,
                                             const float coeff_f[PCM51XX_EQ_KOEF_PER_BAND])
{
    esp_err_t ret = ESP_OK;
    uint8_t current_page = 0xFF; /* sentinel — force first page select */

    for (int ci = 0; ci < PCM51XX_EQ_KOEF_PER_BAND; ci++) {
        uint8_t pg, off;
        pcm51xx_bq_coeff_addr(addr, ci, &pg, &off);

        if (pg != current_page) {
            ret = pcm51xx_select_page(pg);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "%s: page select 0x%02x failed", __func__, pg);
                return ret;
            }
            current_page = pg;
        }

        uint32_t value = pcm51xx_float_to_q2_30(coeff_f[ci]);
        ESP_LOGD(TAG, "%s: pg=0x%02x off=0x%02x ci=%d val=0x%08x (%.7f)",
                 __func__, pg, off, ci, (unsigned)value, coeff_f[ci]);

        ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &off, 1,
                                  (uint8_t *)&value, sizeof(value));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s: write failed ci=%d off=0x%02x", __func__, ci, off);
            return ret;
        }
    }
    return ret;
}

static esp_err_t pcm51xx_write_band(int band,
                                     const float coeff_f[PCM51XX_EQ_KOEF_PER_BAND])
{
    return pcm51xx_write_band_by_addr(&pcm51xx_bq_addr[band], coeff_f);
}

/* --------------------------------------------------------------------------
 * Public EQ API
 * -------------------------------------------------------------------------- */

/*
 * Write safe default (identity/pass-through) coefficients to every BQ section:
 *   std-form (b0=1, b1=0, b2=0, a1=0, a2=0)
 *   register format  [b0=1.0, b1/2=0.0, b2=0.0, −a1/2=0.0, −a2=0.0]
 *
 * MUST be called while the device is in standby (PMOD_STANDBY).
 * Covers both the 6 EQ BQs (c10–c39) and the 6 DRC BQs (c40–c69).
 */
static esp_err_t pcm51xx_write_all_bq_defaults(void)
{
    ESP_LOGD(TAG, "%s", __func__);
    static const float def[PCM51XX_EQ_KOEF_PER_BAND] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f
    };

    for (int i = 0; i < PCM51XX_EQ_BANDS; i++) {
        esp_err_t ret = pcm51xx_write_band_by_addr(&pcm51xx_bq_addr[i], def);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s: EQ band %d failed", __func__, i);
            return ret;
        }
    }
    for (int i = 0; i < PCM51XX_DRC_BQ_BANDS; i++) {
        esp_err_t ret = pcm51xx_write_band_by_addr(&pcm51xx_drc_bq_addr[i], def);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s: DRC band %d failed", __func__, i);
            return ret;
        }
    }
    return ESP_OK;
}

/*
 * One-time DSP initialisation:
 *   1. Enter standby
 *   2. Select process flow 5 (fixed flow with configurable BQ parameters)
 *   3. Disable x16 interpolation
 *   4. Write safe defaults to all 12 BQ sections (6 EQ + 6 DRC)
 *   5. Restore page 0
 *   6. Exit standby
 */
static esp_err_t pcm51xx_dsp_init(void)
{
    ESP_LOGI(TAG, "%s: configuring DSP + zeroing all BQ bands", __func__);

    esp_err_t ret = pcm51xx_enter_standby();
    if (ret != ESP_OK) return ret;

    /* Process flow MUST be set before BQ coefficient writes:
     * pages 44–46 only map to BQ coefficient RAM once flow 5 is active.
     * Writing them in flow 0 silently discards the data. */
    uint8_t reg, val;
    reg = PCM51XX_REG_PROCESS_FLOW;  val = PCM51XX_PROC_FLOW_5;
    ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &reg, 1, &val, 1);
    if (ret != ESP_OK) { pcm51xx_exit_standby(); return ret; }

    reg = PCM51XX_REG_X16INTP;  val = 0x00;
    ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &reg, 1, &val, 1);
    if (ret != ESP_OK) { pcm51xx_exit_standby(); return ret; }

    ret = pcm51xx_write_all_bq_defaults();
    pcm51xx_restore_page0();

    esp_err_t ret2 = pcm51xx_exit_standby();
    if (ret == ESP_OK) ret = ret2;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s: done (process flow 5, all BQ at identity)", __func__);
    }
    return ret;
}

esp_err_t pcm51xx_get_eq_gain(int band, int *gain)
{
    if (band < 0 || band >= PCM51XX_EQ_BANDS || gain == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *gain = pcm51xx_state.eq_gain[band];
    return ESP_OK;
}

esp_err_t pcm51xx_set_eq_gain(int band, int gain)
{
    if (band < 0 || band >= PCM51XX_EQ_BANDS) {
        ESP_LOGE(TAG, "%s: invalid band %d", __func__, band);
        return ESP_ERR_INVALID_ARG;
    }
    if (gain < PCM51XX_EQ_MIN_DB || gain > PCM51XX_EQ_MAX_DB) {
        ESP_LOGE(TAG, "%s: gain %d out of range [%d, %d]", __func__, gain,
                 PCM51XX_EQ_MIN_DB, PCM51XX_EQ_MAX_DB);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "%s: band=%d (%d Hz) gain=%d dB",
             __func__, band, pcm51xx_eq_band_cfg[band].freq_hz, gain);

    /* Compute biquad coefficients using the Audio EQ Cookbook. */
    bq_coeffs_t calc;
    if (bq_calc((bq_filter_type_t)pcm51xx_eq_band_cfg[band].filter_type,
                (double)pcm51xx_eq_band_cfg[band].freq_hz,
                (double)gain,
                (double)pcm51xx_eq_band_cfg[band].q,
                BQ_SAMPLE_RATE_48K, &calc) != 0) {
        ESP_LOGE(TAG, "%s: bq_calc failed band=%d gain=%d", __func__, band, gain);
        return ESP_FAIL;
    }

    /*
     * PCM5122 register encoding (verified against TI PPC app output):
     *
     *  Register  | Stored value        | Why
     *  ----------|---------------------|---------------------------------------------
     *  b0        | b0                  | as-is; Q1.23 clips to ≤1.0 at high gains
     *  b1        | b1 / 2              | hardware multiplies by 2; fits Q1.23 [-1,+1)
     *  b2        | b2                  | as-is
     *  a1        | −a1 / 2             | negated (same as TAS5805M) AND halved
     *  a2        | −a2                 | negated only (same as TAS5805M)
     *
     * bq_calc output convention: y[n] = b0·x − b1·y[n-1] − b2·y[n-2]  (standard form)
     * so a1 ≈ −2, a2 ≈ +1 for typical low-frequency filters.
     */
    const float coeff_f[PCM51XX_EQ_KOEF_PER_BAND] = {
        (float) calc.b0,          /* b0 */
        (float)(calc.b1 * 0.5),   /* b1/2 */
        (float) calc.b2,          /* b2 */
        (float)(-calc.a1 * 0.5),  /* −a1/2 */
        (float)(-calc.a2),        /* −a2 */
    };

    esp_err_t ret = pcm51xx_enter_standby();
    if (ret != ESP_OK) { return ret; }

    ret = pcm51xx_write_band(band, coeff_f);
    pcm51xx_restore_page0();

    esp_err_t ret2 = pcm51xx_exit_standby();
    if (ret == ESP_OK) { ret = ret2; }

    if (ret == ESP_OK) {
        pcm51xx_state.eq_gain[band] = (int8_t)gain;
    }
    return ret;
}

esp_err_t pcm51xx_read_biquad_coefficients(int band, uint32_t coeffs[PCM51XX_EQ_KOEF_PER_BAND])
{
    if (band < 0 || band >= PCM51XX_EQ_BANDS || coeffs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "%s: band=%d", __func__, band);
    esp_err_t ret = 0;
    
    /* Enter standby to pause the DSP before accessing the coefficient RAM.
     * Without this, some reads return 0 due to bus contention with the DSP. */
    // esp_err_t ret = pcm51xx_enter_standby();
    // if (ret != ESP_OK) return ret;

    uint8_t current_page = 0xFF;

    for (int ci = 0; ci < PCM51XX_EQ_KOEF_PER_BAND; ci++) {
        uint8_t pg, off;
        pcm51xx_bq_coeff_addr(&pcm51xx_bq_addr[band], ci, &pg, &off);

        if (pg != current_page) {
            ret = pcm51xx_select_page(pg);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "%s: page select 0x%02x failed", __func__, pg);
                pcm51xx_restore_page0();
                return ret;
            }
            current_page = pg;
        }

        /* PCM5122 does NOT auto-increment the register address on sequential
         * reads — each I2C read transaction must supply its own register
         * address.  Read the 4 bytes of this coefficient one at a time. */
        uint8_t bytes[4] = {0, 0, 0, 0};
        for (int bi = 0; bi < 4 && ret == ESP_OK; bi++) {
            uint8_t reg_off = off + (uint8_t)bi;
            ret = i2c_bus_read_bytes(i2c_handler, pcm51xx_addr,
                                     &reg_off, 1, &bytes[bi], 1);
        }
        /* Pack into the same little-endian layout that pcm51xx_float_to_q2_30
         * produces: bytes[0] = MSByte (bits 31-24), bytes[3] = 0x00. */
        coeffs[ci] = (uint32_t)bytes[0]
                   | ((uint32_t)bytes[1] <<  8)
                   | ((uint32_t)bytes[2] << 16)
                   | ((uint32_t)bytes[3] << 24);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s: read failed band=%d ci=%d", __func__, band, ci);
            break;
        }
        ESP_LOGD(TAG, "%s: band=%d ci=%d = 0x%08x (%.7f)",
                 __func__, band, ci, (unsigned)coeffs[ci],
                 pcm51xx_q2_30_to_float(coeffs[ci]));
    }

    pcm51xx_restore_page0();
    // esp_err_t ret2 = pcm51xx_exit_standby();
    // if (ret == ESP_OK) ret = ret2;
    return ret;
}

esp_err_t pcm51xx_write_biquad_coefficients(int band, const uint32_t coeffs[PCM51XX_EQ_KOEF_PER_BAND])
{
    if (band < 0 || band >= PCM51XX_EQ_BANDS || coeffs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "%s: band=%d", __func__, band);

    /* Convert raw wire-format values to float for the shared write helper. */
    float coeff_f[PCM51XX_EQ_KOEF_PER_BAND];
    for (int ci = 0; ci < PCM51XX_EQ_KOEF_PER_BAND; ci++) {
        coeff_f[ci] = pcm51xx_q2_30_to_float(coeffs[ci]);
    }

    esp_err_t ret = pcm51xx_enter_standby();
    if (ret != ESP_OK) { return ret; }

    ret = pcm51xx_write_band(band, coeff_f);
    pcm51xx_restore_page0();

    esp_err_t ret2 = pcm51xx_exit_standby();
    if (ret == ESP_OK) { ret = ret2; }
    return ret;
}

/* --------------------------------------------------------------------------
 * Q2.30 coefficient format conversions
 *
 * PCM5122 coefficient format: 32-bit two's complement Q2.30, range [-2, +2).
 * The hardware ignores the lower 8 bits of each coefficient word, so those
 * bits are always written as 0x00.  Each coefficient occupies 4 I2C register
 * bytes, MSByte first.
 *
 * i2c_bus_write_bytes writes bytes in memory order (little-endian on ESP32),
 * so the uint32_t is packed with byte[0] in memory = coefficient MSByte and
 * byte[3] in memory = 0x00 (LSByte, ignored by hardware).
 * -------------------------------------------------------------------------- */

uint32_t pcm51xx_float_to_q2_30(float value)
{
    if (value >  1.9999998f) value =  1.9999998f;
    if (value < -2.0f)       value = -2.0f;

    int32_t q = (int32_t)(value * (float)(1 << 30));
    /* Clear lower 8 bits — hardware ignores them. */
    q &= (int32_t)0xFFFFFF00;

    /* Pack big-endian into little-endian uint32_t so that
     * i2c_bus_write_bytes sends: [bits31-24, bits23-16, bits15-8, 0x00]. */
    uint32_t le_val = (uint32_t)(
        ((uint32_t)((q >> 24) & 0xFF)      ) |   /* byte[0] = MSByte */
        ((uint32_t)((q >> 16) & 0xFF) <<  8) |   /* byte[1]          */
        ((uint32_t)((q >>  8) & 0xFF) << 16)     /* byte[2]          */
        /* byte[3] = 0x00 — implicit (lower 8 bits cleared above)   */
    );
    return le_val;
}

/* --------------------------------------------------------------------------
 * BQ register dump — reads all 6 bands and logs raw hex + decoded floats
 * -------------------------------------------------------------------------- */
static void pcm51xx_dump_bq_coefficients(const char *label)
{
    static const char *coeff_names[PCM51XX_EQ_KOEF_PER_BAND] = {
        "b0    ", "b1/2  ", "b2    ", "-a1/2 ", "-a2   "
    };

    ESP_LOGI(TAG, "=== BQ register dump: %s ===", label);

    for (int band = 0; band < PCM51XX_EQ_BANDS; band++) {
        uint32_t raw[PCM51XX_EQ_KOEF_PER_BAND];
        esp_err_t err = pcm51xx_read_biquad_coefficients(band, raw);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  band %d: read failed (%d)", band, err);
            continue;
        }

        ESP_LOGI(TAG, "  band %d (%5d Hz):", band,
                 pcm51xx_eq_band_cfg[band].freq_hz);

        /* Stored register values (wire format) */
        for (int ci = 0; ci < PCM51XX_EQ_KOEF_PER_BAND; ci++) {
            float fv = pcm51xx_q2_30_to_float(raw[ci]);
            /* Print as signed integer × 10^6 to avoid float format issues */
            int32_t fv_i = (int32_t)(fv * 1000000.0f);
            ESP_LOGI(TAG, "    [%d] %s  raw=0x%08" PRIx32 "  q2.30=%c%ld.%06ld",
                     ci, coeff_names[ci], raw[ci],
                     fv_i < 0 ? '-' : '+',
                     (long)(fv_i < 0 ? -fv_i : fv_i) / 1000000L,
                     (long)(fv_i < 0 ? -fv_i : fv_i) % 1000000L);
        }

        /* Reconstruct standard-form biquad coefficients for reference:
         *   register stores b1/2  → standard b1 = reg_b1 * 2
         *   register stores -a1/2 → standard a1 = reg_a1 * (-2)
         *   register stores -a2   → standard a2 = reg_a2 * (-1)        */
        float stdf[5] = {
             pcm51xx_q2_30_to_float(raw[0]),          /* b0 */
             pcm51xx_q2_30_to_float(raw[1]) * 2.0f,   /* b1 */
             pcm51xx_q2_30_to_float(raw[2]),           /* b2 */
            -pcm51xx_q2_30_to_float(raw[3]) * 2.0f,   /* a1 */
            -pcm51xx_q2_30_to_float(raw[4]),           /* a2 */
        };
        static const char *std_names[5] = { "b0", "b1", "b2", "a1", "a2" };
        for (int si = 0; si < 5; si++) {
            int32_t vi = (int32_t)(stdf[si] * 1000000.0f);
            long av = vi < 0 ? -(long)vi : (long)vi;
            ESP_LOGI(TAG, "    std %s = %c%ld.%06ld",
                     std_names[si], vi < 0 ? '-' : '+',
                     av / 1000000L, av % 1000000L);
        }
    }

    ESP_LOGI(TAG, "=== end dump: %s ===", label);
}

/* --------------------------------------------------------------------------
 * TEST TASK — temporary, remove after EQ validation
 * -------------------------------------------------------------------------- */
static void pcm51xx_eq_test_task(void *arg)
{
    esp_err_t ret = 0;
    // ESP_LOGW(TAG, "===========================");
    // ESP_LOGW(TAG, "Starting PCM51XX EQ test task — dumping default coefficients");
    // pcm51xx_dump_bq_coefficients("after dsp_init (expect identity)");
    
    vTaskDelay(pdMS_TO_TICKS(10000));

    // ESP_LOGW(TAG, "===========================");
    // ESP_LOGW(TAG, "Setting default DSP coefficients");
    // ret = pcm51xx_dsp_init();
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "PCM51XX DSP init failed: %d", ret);
    // }
    // /* All BQ bands should read back as identity (1,0,0,0,0). */
    // pcm51xx_dump_bq_coefficients("after dsp_init");

    // vTaskDelay(pdMS_TO_TICKS(10000));
    
    ESP_LOGW(TAG, "===========================");
    ESP_LOGW(TAG, "Writing default DSP RAM sequence");
    ret = pcm51xx_transmit_registers(
        pcm51xx_dsp_init_seq, sizeof(pcm51xx_dsp_init_seq) / sizeof(pcm51xx_dsp_init_seq[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCM51XX register init sequence failed: %d", ret);
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
    pcm51xx_dump_bq_coefficients("after PPC init sequence");

    // vTaskDelay(pdMS_TO_TICKS(10000));
    // ESP_LOGW(TAG, "===========================");
    // ESP_LOGW(TAG, "Setting default DSP coefficients");
    // ret = pcm51xx_dsp_init();
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "PCM51XX DSP init failed: %d", ret);
    // }
    // /* All BQ bands should read back as identity (1,0,0,0,0). */
    // pcm51xx_dump_bq_coefficients("after dsp_init again");

    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGW(TAG, "===========================");
    ESP_LOGW(TAG, "Bumping up EQ band 5");
    for (uint8_t db = 0; db <= 15; db += 3) {
        ESP_LOGI(TAG, "Setting bands 0,1 gain to %d dB", db);
        ret = pcm51xx_set_eq_gain(0, db);
        ret = pcm51xx_set_eq_gain(1, db);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set EQ gain: %d", ret);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
    pcm51xx_dump_bq_coefficients("after dsp_init again");

    // vTaskDelay(pdMS_TO_TICKS(10000));
    // ESP_LOGW(TAG, "===========================");
    // ESP_LOGW(TAG, "Restoring Process flow 1");
    // pcm51xx_enter_standby();
    // uint8_t reg = PCM51XX_REG_PROCESS_FLOW;
    // uint8_t val = PCM51XX_PROC_FLOW_1;
    // ret = i2c_bus_write_bytes(i2c_handler, pcm51xx_addr, &reg, 1, &val, 1);
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "Failed to set process flow 1: %d", ret);
    // }
    // // pcm51xx_dump_bq_coefficients("after setting process flow 1 (expect non-identity)");
    // pcm51xx_exit_standby();

    vTaskDelete(NULL);
}

float pcm51xx_q2_30_to_float(uint32_t raw)
{
    /* Reconstruct the 32-bit signed Q2.30 value from little-endian layout.
     * byte[0] in memory = bits 31-24 (MSByte, contains sign).
     * byte[3] in memory = bits 7-0  (always 0x00, ignored by hardware). */
    uint32_t b0 = (raw      ) & 0xFF;  /* bits 31-24 */
    uint32_t b1 = (raw >>  8) & 0xFF;  /* bits 23-16 */
    uint32_t b2 = (raw >> 16) & 0xFF;  /* bits 15-8  */
    /* (raw >> 24) & 0xFF is always 0x00 */

    /* Reassemble as signed 32-bit; bit 31 of the result carries the sign. */
    int32_t q = (int32_t)((b0 << 24) | (b1 << 16) | (b2 << 8));
    return (float)q / (float)(1u << 30);
}

#endif /* CONFIG_DAC_PCM51XX_EQ_SUPPORT */

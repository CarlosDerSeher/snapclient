/**
 * @file pcm51xx_settings.h
 * @brief PCM5122 DAC settings persistence and JSON serialization
 *
 * Manages NVS persistence for PCM5122 parametric EQ settings and provides
 * a JSON API for the HTTP configuration interface.
 *
 * All settings are applied eagerly from pcm51xx_settings_init() — the
 * PCM5122 does not require the I2S clock to be present for DSP coefficient
 * RAM or process-flow register writes.  Call pcm51xx_settings_init() once
 * at boot after pcm51xx_init().
 */

#ifndef __PCM51XX_SETTINGS_H__
#define __PCM51XX_SETTINGS_H__

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * NVS namespace and key definitions
 * ------------------------------------------------------------------------- */

/** NVS namespace used by this module. */
#define PCM51XX_NVS_NAMESPACE          "pcm51xx_cfg"

/**
 * NVS key: DSP process flow selection (uint8).
 * Valid values: PCM51XX_PROC_FLOW_1 … PCM51XX_PROC_FLOW_5 (from pcm51xx.h).
 * Default is PCM51XX_PROC_FLOW_5 (parametric EQ active).
 */
#define PCM51XX_NVS_KEY_PROC_FLOW      "proc_flow"

/**
 * NVS key prefix for per-band EQ gain.
 * Full key is formed as PCM51XX_NVS_KEY_EQ_GAIN_PREFIX + band_index,
 * e.g. "eq_gain_0" … "eq_gain_5".
 */
#define PCM51XX_NVS_KEY_EQ_GAIN_PREFIX "eq_gain_"

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialise the PCM5122 settings manager and apply persisted settings.
 *
 * Creates the thread-safety mutex, initialises the DSP (identity BQ
 * coefficients + process flow 5), and then restores the persisted process
 * flow and per-band EQ gains from NVS.
 *
 * Must be called once after pcm51xx_init().
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if mutex creation fails,
 *         or an error from pcm51xx_set_process_flow().
 */
esp_err_t pcm51xx_settings_init(void);

/* -------------------------------------------------------------------------
 * Process flow selection
 * ------------------------------------------------------------------------- */

/**
 * @brief Persist the DSP process flow selection to NVS.
 *
 * @param flow  Process flow value (e.g. PCM51XX_PROC_FLOW_5 for parametric EQ).
 * @return ESP_OK on success.
 */
esp_err_t pcm51xx_settings_save_process_flow(uint8_t flow);

/**
 * @brief Load the DSP process flow selection from NVS.
 *
 * @param[out] flow  Receives the stored value.  Defaults to PCM51XX_PROC_FLOW_5
 *                   if not previously saved.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not yet stored.
 */
esp_err_t pcm51xx_settings_load_process_flow(uint8_t *flow);

/* -------------------------------------------------------------------------
 * Per-band EQ gain
 * ------------------------------------------------------------------------- */

/**
 * @brief Persist the EQ gain for a single band to NVS.
 *
 * @param band     Band index (0 … PCM51XX_EQ_BANDS-1).
 * @param gain_db  Gain in dB (PCM51XX_EQ_MIN_DB … PCM51XX_EQ_MAX_DB).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on bad band index.
 */
esp_err_t pcm51xx_settings_save_eq_gain(int band, int gain_db);

/**
 * @brief Load the EQ gain for a single band from NVS.
 *
 * @param band      Band index (0 … PCM51XX_EQ_BANDS-1).
 * @param[out] gain_db  Receives the stored gain in dB.  Defaults to 0 if not
 *                      previously saved.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not yet stored.
 */
esp_err_t pcm51xx_settings_load_eq_gain(int band, int *gain_db);

/* -------------------------------------------------------------------------
 * JSON API
 * ------------------------------------------------------------------------- */

/**
 * @brief Serialise the current EQ settings to a JSON string.
 *
 * Output format:
 * @code{.json}
 * {
 *   "proc_flow": 5,
 *   "eq_gain_0": 0,
 *   "eq_gain_1": 0,
 *   ...
 *   "eq_gain_5": 0
 * }
 * @endcode
 *
 * @param json_out  Caller-supplied buffer to receive the JSON string.
 * @param max_len   Size of json_out in bytes.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer is too small.
 */
esp_err_t pcm51xx_settings_get_eq_json(char *json_out, size_t max_len);

/**
 * @brief Serialise the EQ parameter schema to a JSON string.
 *
 * Describes each parameter (type, range, label) for the web UI.
 *
 * @param json_out  Caller-supplied buffer.
 * @param max_len   Size of json_out in bytes.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if buffer is too small.
 */
esp_err_t pcm51xx_settings_get_eq_schema_json(char *json_out, size_t max_len);

/**
 * @brief Apply EQ settings from a JSON string and persist them to NVS.
 *
 * Accepts a subset of the keys produced by pcm51xx_settings_get_eq_json().
 * Unknown keys are ignored; any applied key is also written to NVS.
 *
 * @param json_in  Null-terminated JSON string.
 * @return ESP_OK on success, ESP_FAIL if the JSON cannot be parsed.
 */
esp_err_t pcm51xx_settings_set_eq_from_json(const char *json_in);

#ifdef __cplusplus
}
#endif

#endif /* __PCM51XX_SETTINGS_H__ */

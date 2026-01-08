/**
 * @file dsp_processor_settings.h
 * @brief DSP settings persistence and JSON serialization
 *
 * Manages NVS persistence for DSP processor settings including:
 * - Active DSP flow selection
 * - Flow-specific parameters (frequencies, gains)
 * - JSON serialization for HTTP API consumption
 */

#ifndef __DSP_PROCESSOR_SETTINGS_H__
#define __DSP_PROCESSOR_SETTINGS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "dsp_types.h"
#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Initialize DSP settings manager
 * Must be called before any other functions
 */
esp_err_t dsp_settings_init(void);

/**
 * Save active DSP flow selection to NVS
 * @param flow The DSP flow to set as active
 */
esp_err_t dsp_settings_save_active_flow(dspFlows_t flow);

/**
 * Load active DSP flow selection from NVS
 * @param flow Pointer to store the loaded flow
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not set
 */
esp_err_t dsp_settings_load_active_flow(dspFlows_t *flow);

/**
 * Save a flow-specific integer parameter to NVS
 * Format: "flow_<id>_<param>" (e.g., "flow_5_fc_1")
 *
 * @param flow The DSP flow this parameter belongs to
 * @param param_name Parameter name (e.g., "fc_1", "gain_1")
 * @param value Parameter value
 */
esp_err_t dsp_settings_save_flow_param(dspFlows_t flow, const char *param_name,
									   int32_t value);

/**
 * Load a flow-specific integer parameter from NVS
 *
 * @param flow The DSP flow this parameter belongs to
 * @param param_name Parameter name
 * @param value Pointer to store the loaded value
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not set
 */
esp_err_t dsp_settings_load_flow_param(dspFlows_t flow, const char *param_name,
									   int32_t *value);

/**
 * Get all DSP settings as a JSON string
 * Includes active flow and all flow-specific parameters
 *
 * @param json_out Buffer to store JSON string (caller must allocate)
 * @param max_len Maximum size of output buffer
 * @return ESP_OK on success
 *
 * Example output:
 * {
 *   "active_flow": 5,
 *   "flows": {
 *     "5": {"fc_1": 150, "gain_1": 0, "fc_3": 8000, "gain_3": 0}
 *   }
 * }
 */
esp_err_t dsp_settings_get_json(char *json_out, size_t max_len);

/**
 * Update DSP settings from a JSON string
 * Parses JSON and saves values to NVS
 *
 * @param json_in JSON string containing settings to update
 * @return ESP_OK on success
 *
 * Expected format:
 * {
 *   "active_flow": 5,
 *   "flow_5_fc_1": 150,
 *   "flow_5_gain_1": 0
 * }
 */
esp_err_t dsp_settings_set_from_json(const char *json_in);

/**
 * Get current active flow
 * @return Current active DSP flow
 */
dspFlows_t dsp_settings_get_active_flow(void);

/**
 * Get parameters for a specific flow
 * @param flow DSP flow to query
 * @param params Output structure for parameters
 * @return ESP_OK on success
 */
esp_err_t dsp_settings_get_flow_params(dspFlows_t flow, filterParams_t *params);

/**
 * Set parameters for a specific flow
 * Persists to NVS and updates cache
 *
 * @param flow DSP flow to update
 * @param params New parameters
 * @return ESP_OK on success
 */
esp_err_t dsp_settings_set_flow_params(dspFlows_t flow,
									   const filterParams_t *params);

/**
 * Switch active flow
 * Persists to NVS and updates cache
 *
 * @param flow New active flow
 * @return ESP_OK on success
 */
esp_err_t dsp_settings_switch_active_flow(dspFlows_t flow);

#ifdef __cplusplus
}
#endif

#endif /* __DSP_PROCESSOR_SETTINGS_H__ */

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
 * Sink for dsp_settings_write_json(): called with consecutive pieces of the
 * JSON text (not NUL-terminated). Return anything but ESP_OK to stop.
 */
typedef esp_err_t (*dsp_json_write_fn)(void *ctx, const char *data,
									   size_t len);

/**
 * Write the DSP flows shown in the web UI with their parameter schema and
 * current values as JSON, in small pieces, without heap allocation.
 *
 * Output (unformatted, as one line; shown here with one flow and one
 * parameter):
 *
 * {
 * 	"active_flow": 5,
 * 	"flows": [
 * 		{
 * 			"id": "dspfEQBassTreble",
 * 			"name": "Bass & Treble EQ",
 * 			"description": "Simple 2-band equalizer with bass and treble controls",
 * 			"enum_value": 5,
 * 			"parameters": [
 * 				{
 * 					"key": "fc_1",
 * 					"name": "Bass Frequency",
 * 					"unit": "Hz",
 * 					"min": 30,
 * 					"max": 500,
 * 					"default": 150,
 * 					"step": 5,
 * 					"current": 150
 * 				},
 * 				...
 * 			]
 * 		},
 * 		...
 * 	]
 * }
 *
 * "active_flow" and "enum_value" are dspFlows_t values. A flow without
 * adjustable parameters (Stereo) has an empty "parameters" array.
 *
 * @return ESP_OK, or the first error returned by @p write
 */
esp_err_t dsp_settings_write_json(dsp_json_write_fn write, void *ctx);

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

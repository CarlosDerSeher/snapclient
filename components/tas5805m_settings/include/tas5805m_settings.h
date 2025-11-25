/**
 * @file tas5805m_settings.h
 * @brief TAS5805M DAC settings persistence and JSON serialization
 *
 * Manages NVS persistence for TAS5805M DAC settings including:
 * - DAC state (Deep Sleep, Sleep, Hi-Z, Play, Mute)
 * - Digital volume control
 * - Analog gain control
 * - JSON serialization for HTTP API consumption
 * - Communication with tas5805m component
 */

#ifndef __TAS5805M_SETTINGS_H__
#define __TAS5805M_SETTINGS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include "tas5805m.h"

// NVS Configuration
#define TAS5805M_NVS_NAMESPACE      "tas5805m_cfg"
#define TAS5805M_NVS_KEY_STATE      "state"
#define TAS5805M_NVS_KEY_DIGITAL_VOL "dig_vol"
#define TAS5805M_NVS_KEY_ANALOG_GAIN "ana_gain"

// Digital Volume Settings (in 0.5dB steps)
#define TAS5805M_DIGITAL_VOL_MIN    -207    // -103.5 dB
#define TAS5805M_DIGITAL_VOL_MAX    48      // +24 dB
#define TAS5805M_DIGITAL_VOL_STEP   1       // 0.5 dB per step
#define TAS5805M_DIGITAL_VOL_DEFAULT 0      // 0 dB
#define TAS5805M_DIGITAL_VOL_SCALE  0.5     // Display scaling factor

// Analog Gain Settings (in 0.5dB steps)
#define TAS5805M_ANALOG_GAIN_MIN    -31     // -15.5 dB
#define TAS5805M_ANALOG_GAIN_MAX    0       // 0 dB
#define TAS5805M_ANALOG_GAIN_STEP   1       // 0.5 dB per step
#define TAS5805M_ANALOG_GAIN_DEFAULT 0      // 0 dB
#define TAS5805M_ANALOG_GAIN_SCALE  0.5     // Display scaling factor

/**
 * Initialize TAS5805M settings manager
 * Must be called before any other functions
 * @return ESP_OK on success
 */
esp_err_t tas5805m_settings_init(void);

/**
 * Save DAC state to NVS
 * @param state The TAS5805M_CTRL_STATE to persist
 * @return ESP_OK on success
 */
esp_err_t tas5805m_settings_save_state(TAS5805M_CTRL_STATE state);

/**
 * Load DAC state from NVS
 * @param state Pointer to store the loaded state
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not set
 */
esp_err_t tas5805m_settings_load_state(TAS5805M_CTRL_STATE *state);

/**
 * Save digital volume setting to NVS
 * @param vol_half_db Volume in 0.5dB steps (-207 to 48)
 * @return ESP_OK on success
 */
esp_err_t tas5805m_settings_save_digital_volume(int vol_half_db);

/**
 * Load digital volume setting from NVS
 * @param vol_half_db Pointer to store volume in 0.5dB steps
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not set
 */
esp_err_t tas5805m_settings_load_digital_volume(int *vol_half_db);

/**
 * Save analog gain setting to NVS
 * @param gain_half_db Gain in 0.5dB steps (-31 to 0)
 * @return ESP_OK on success
 */
esp_err_t tas5805m_settings_save_analog_gain(int gain_half_db);

/**
 * Load analog gain setting from NVS
 * @param gain_half_db Pointer to store gain in 0.5dB steps
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not set
 */
esp_err_t tas5805m_settings_load_analog_gain(int *gain_half_db);

/**
 * Get current TAS5805M settings as a JSON string
 * Queries the tas5805m component for current state and formats as JSON
 * 
 * @param json_out Buffer to store JSON string (caller must allocate)
 * @param max_len Maximum size of output buffer
 * @return ESP_OK on success
 * 
 * Example output:
 * {
 *   "state": 3,
 *   "state_name": "Play",
 *   "volume": 50,
 *   "muted": false
 * }
 */
esp_err_t tas5805m_settings_get_json(char *json_out, size_t max_len);

/**
 * Update TAS5805M settings from a JSON string
 * Parses JSON and applies changes to the DAC via tas5805m component
 * 
 * @param json_in JSON string containing settings to update
 * @return ESP_OK on success
 * 
 * Expected format:
 * {
 *   "state": 3
 * }
 */
esp_err_t tas5805m_settings_set_from_json(const char *json_in);

/**
 * Get TAS5805M settings schema as JSON
 * Provides UI metadata about available settings and their constraints
 * 
 * @param json_out Buffer to store JSON string (caller must allocate)
 * @param max_len Maximum size of output buffer
 * @return ESP_OK on success
 * 
 * Example output:
 * {
 *   "parameters": [
 *     {
 *       "key": "state",
 *       "name": "DAC State",
 *       "type": "enum",
 *       "values": [
 *         {"value": 0, "name": "Deep Sleep"},
 *         {"value": 1, "name": "Sleep"},
 *         {"value": 2, "name": "Hi-Z"},
 *         {"value": 3, "name": "Play"}
 *       ],
 *       "current": 3
 *     }
 *   ]
 * }
 */
esp_err_t tas5805m_settings_get_schema_json(char *json_out, size_t max_len);

#ifdef __cplusplus
}
#endif

#endif /* __TAS5805M_SETTINGS_H__ */

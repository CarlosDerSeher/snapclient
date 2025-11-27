/*
 * tas5805m_settings.h
 * TAS5805M DAC settings persistence and JSON serialization
 *
 * Notes:
 *  - DAC state and digital volume are treated as read-only by the settings manager
 *    (managed by the TAS5805M driver / application) and are not persisted to NVS.
 */

#ifndef __TAS5805M_SETTINGS_H__
#define __TAS5805M_SETTINGS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#include "tas5805m_types.h"
#include "tas5805m.h"

// NVS Configuration
#define TAS5805M_NVS_NAMESPACE      "tas5805m_cfg"
#define TAS5805M_NVS_KEY_ANALOG_GAIN "ana_gain"
#define TAS5805M_NVS_KEY_DAC_MODE   "dac_mode"
#define TAS5805M_NVS_KEY_MOD_MODE   "mod_mode"
#define TAS5805M_NVS_KEY_SW_FREQ    "sw_freq"
#define TAS5805M_NVS_KEY_BD_FREQ    "bd_freq"
// Mixer mode (persisted)
#define TAS5805M_NVS_KEY_MIXER_MODE  "mixer_mode"
// EQ mode (persisted)
#define TAS5805M_NVS_KEY_EQ_MODE     "eq_mode"

// Digital Volume Settings (in 0.5dB steps) - kept for UI scaling/display
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

/** Initialize TAS5805M settings manager. Must be called before other functions. */
esp_err_t tas5805m_settings_init(void);

/** Save analog gain setting to NVS */
esp_err_t tas5805m_settings_save_analog_gain(int gain_half_db);
/** Load analog gain setting from NVS */
esp_err_t tas5805m_settings_load_analog_gain(int *gain_half_db);

/** Save DAC mode setting to NVS */
esp_err_t tas5805m_settings_save_dac_mode(TAS5805M_DAC_MODE mode);
/** Load DAC mode setting from NVS */
esp_err_t tas5805m_settings_load_dac_mode(TAS5805M_DAC_MODE *mode);

/** Save modulation mode settings to NVS */
esp_err_t tas5805m_settings_save_modulation_mode(TAS5805M_MOD_MODE mode, 
                                                   TAS5805M_SW_FREQ freq,
                                                   TAS5805M_BD_FREQ bd_freq);
/** Load modulation mode settings from NVS */
esp_err_t tas5805m_settings_load_modulation_mode(TAS5805M_MOD_MODE *mode,
                                                   TAS5805M_SW_FREQ *freq,
                                                   TAS5805M_BD_FREQ *bd_freq);

/** Save mixer mode to NVS */
esp_err_t tas5805m_settings_save_mixer_mode(TAS5805M_MIXER_MODE mode);
/** Load mixer mode from NVS */
esp_err_t tas5805m_settings_load_mixer_mode(TAS5805M_MIXER_MODE *mode);

/** Save EQ mode to NVS */
esp_err_t tas5805m_settings_save_eq_mode(TAS5805M_EQ_MODE mode);
/** Load EQ mode from NVS */
esp_err_t tas5805m_settings_load_eq_mode(TAS5805M_EQ_MODE *mode);

/** Get current TAS5805M settings as a JSON string */
esp_err_t tas5805m_settings_get_json(char *json_out, size_t max_len);

/** Update TAS5805M settings from a JSON string */
esp_err_t tas5805m_settings_set_from_json(const char *json_in);

/** Get TAS5805M settings schema as JSON */
esp_err_t tas5805m_settings_get_schema_json(char *json_out, size_t max_len);

/** Apply all persisted TAS5805M settings from NVS to the hardware. */
esp_err_t tas5805m_settings_apply_all(void);

#ifdef __cplusplus
}
#endif

#endif /* __TAS5805M_SETTINGS_H__ */

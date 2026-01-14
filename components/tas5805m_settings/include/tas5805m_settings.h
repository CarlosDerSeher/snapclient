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

#if CONFIG_DAC_TAS5805M

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
// EQ per-band gain keys prefix (final key will be e.g. "eq_gain_l_0" or "eq_gain_r_3")
#define TAS5805M_NVS_KEY_EQ_GAIN_L_PREFIX "eq_gain_l_"
#define TAS5805M_NVS_KEY_EQ_GAIN_R_PREFIX "eq_gain_r_"
// EQ profile/preset keys for left/right channels
#define TAS5805M_NVS_KEY_EQ_PROFILE_L  "eq_profile_l"
#define TAS5805M_NVS_KEY_EQ_PROFILE_R  "eq_profile_r"
// EQ UI mode (controls which UI elements are shown and how values are applied)
#define TAS5805M_NVS_KEY_EQ_UI_MODE    "eq_ui_mode"
// Channel gain NVS keys (single value per output channel, in dB)
#define TAS5805M_NVS_KEY_CHANNEL_GAIN_L "channel_gain_l"
#define TAS5805M_NVS_KEY_CHANNEL_GAIN_R "channel_gain_r"

/** EQ UI modes exposed to the settings UI. These control visibility and apply behavior.
 *  Defined here so the settings module owns the UI contract. Values are persisted to NVS.
 */
typedef enum {
    TAS5805M_EQ_UI_MODE_OFF = 0,
    TAS5805M_EQ_UI_MODE_15_BAND = 1,
    TAS5805M_EQ_UI_MODE_15_BAND_BIAMP = 2,
    TAS5805M_EQ_UI_MODE_PRESETS = 3,
} TAS5805M_EQ_UI_MODE;

/** Convert an EQ UI mode to human-readable name (for schema name fields) */
const char *tas5805m_eq_ui_mode_to_string(TAS5805M_EQ_UI_MODE m);

/** Save/Load the UI mode selection to NVS */
esp_err_t tas5805m_settings_save_eq_ui_mode(TAS5805M_EQ_UI_MODE mode);
esp_err_t tas5805m_settings_load_eq_ui_mode(TAS5805M_EQ_UI_MODE *mode);

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

/** Save per-band EQ gain for a channel to NVS (gain in dB, integer) */
esp_err_t tas5805m_settings_save_eq_gain(TAS5805M_EQ_CHANNELS ch, int band, int gain_db);
/** Load per-band EQ gain for a channel from NVS */
esp_err_t tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS ch, int band, int *gain_db);

/** Save EQ profile/preset for a specific channel to NVS */
esp_err_t tas5805m_settings_save_eq_profile(TAS5805M_EQ_CHANNELS ch, TAS5805M_EQ_PROFILE profile);
/** Load EQ profile/preset for a specific channel from NVS */
esp_err_t tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS ch, TAS5805M_EQ_PROFILE *profile);

/** Save per-output channel gain (single value per channel, in dB) */
esp_err_t tas5805m_settings_save_channel_gain(TAS5805M_EQ_CHANNELS ch, int gain_db);
/** Load per-output channel gain (single value per channel, in dB) */
esp_err_t tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS ch, int *gain_db);

/** Get current TAS5805M settings as a JSON string */
//esp_err_t tas5805m_settings_get_json(char *json_out, size_t max_len);

/** Get TAS5805M settings schema as JSON */
//esp_err_t tas5805m_settings_get_schema_json(char *json_out, size_t max_len);

/** Update TAS5805M settings from a JSON string */
//esp_err_t tas5805m_settings_set_from_json(const char *json_in);

/** Get current TAS5805M DAC-only settings as JSON (excludes EQ) */
esp_err_t tas5805m_settings_get_dac_json(char *json_out, size_t max_len);

/** Get TAS5805M DAC-only schema as JSON (excludes EQ) */
esp_err_t tas5805m_settings_get_dac_schema_json(char *json_out, size_t max_len);

/** Update TAS5805M DAC-only settings from JSON (excludes EQ) */
esp_err_t tas5805m_settings_set_dac_from_json(const char *json_in);

/** Get current TAS5805M EQ-only settings as JSON */
esp_err_t tas5805m_settings_get_eq_json(char *json_out, size_t max_len);

/** Get TAS5805M EQ-only schema as JSON */
esp_err_t tas5805m_settings_get_eq_schema_json(char *json_out, size_t max_len);

/** Update TAS5805M EQ settings from JSON */
esp_err_t tas5805m_settings_set_eq_from_json(const char *json_in);

/** Apply all persisted TAS5805M settings from NVS to the hardware. */
/** Apply settings that are safe to write immediately (before audio playback).
 *  Examples: DAC mode, analog gain, modulation mode, mixer mode.
 */
esp_err_t tas5805m_settings_apply_early(void);

/** Apply settings that require the codec to be running (delayed restore),
 *  e.g. EQ mode, per-band gains, EQ profiles and channel gains.
 */
esp_err_t tas5805m_settings_apply_delayed(void);

#endif /* CONFIG_DAC_TAS5805M */

#ifdef __cplusplus
}
#endif

#endif /* __TAS5805M_SETTINGS_H__ */

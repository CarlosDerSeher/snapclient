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

// Advanced Bi-Amp crossover NVS keys
#define TAS5805M_NVS_KEY_BIAMP_XOVER_FREQ   "biamp_xfreq"
#define TAS5805M_NVS_KEY_BIAMP_SLOPE        "biamp_slope"
#define TAS5805M_NVS_KEY_BIAMP_TYPE         "biamp_type"
#define TAS5805M_NVS_KEY_BIAMP_LOW_GAIN     "biamp_lo_g"
#define TAS5805M_NVS_KEY_BIAMP_LOW_PHASE    "biamp_lo_ph"
#define TAS5805M_NVS_KEY_BIAMP_HIGH_GAIN    "biamp_hi_g"
#define TAS5805M_NVS_KEY_BIAMP_HIGH_PHASE   "biamp_hi_ph"
#define TAS5805M_NVS_KEY_BIAMP_SAMPLE_RATE  "biamp_sr"
// Subsonic filter (high-pass) for speaker protection
#define TAS5805M_NVS_KEY_BIAMP_SUBSONIC_FREQ "biamp_sub_f"
// Per-output PEQ keys (format: biamp_lo_peqN_f, biamp_lo_peqN_g, biamp_lo_peqN_q)
#define TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L "biamp_lo_peq"
#define TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H "biamp_hi_peq"

// Loudness compensation (volume-dependent EQ overlay)
#define TAS5805M_NVS_KEY_LOUDNESS_ENABLED   "loud_en"
#define TAS5805M_NVS_KEY_LOUDNESS_THRESH    "loud_thr"    // 4 bytes for thresholds
#define TAS5805M_NVS_KEY_LOUDNESS_BASS      "loud_bass"   // 5 bytes for bass boost
#define TAS5805M_NVS_KEY_LOUDNESS_TREBLE    "loud_treb"   // 5 bytes for treble boost

// Baffle step compensation (woofer low shelf)
#define TAS5805M_NVS_KEY_BAFFLE_WIDTH       "baffle_w"    // Baffle width in cm
#define TAS5805M_NVS_KEY_BAFFLE_PLACEMENT   "baffle_p"    // Speaker placement type

// Time alignment (tweeter delay)
#define TAS5805M_NVS_KEY_TWEETER_DELAY      "twt_delay"   // Tweeter delay in mm

// Tweeter breakup notch filter
#define TAS5805M_NVS_KEY_NOTCH_FREQ         "twt_notch_f" // Notch frequency in Hz
#define TAS5805M_NVS_KEY_NOTCH_GAIN         "twt_notch_g" // Notch depth (negative dB)
#define TAS5805M_NVS_KEY_NOTCH_Q            "twt_notch_q" // Notch Q factor x10

// Air/Brilliance shelf (tweeter high shelf)
#define TAS5805M_NVS_KEY_AIR_GAIN           "twt_air_g"   // Air shelf gain in dB x2

/** EQ UI modes exposed to the settings UI. These control visibility and apply behavior.
 *  Defined here so the settings module owns the UI contract. Values are persisted to NVS.
 */
typedef enum {
    TAS5805M_EQ_UI_MODE_OFF = 0,
    TAS5805M_EQ_UI_MODE_15_BAND = 1,
    TAS5805M_EQ_UI_MODE_15_BAND_BIAMP = 2,
    TAS5805M_EQ_UI_MODE_PRESETS = 3,
    TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP = 4,
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

/* ============ Advanced Bi-Amp Crossover Types ============ */

/** Crossover filter slope (number of cascaded biquads) */
typedef enum {
    BIAMP_SLOPE_12DB = 1,   // 12 dB/octave (1 biquad)
    BIAMP_SLOPE_24DB = 2,   // 24 dB/octave (2 biquads, Linkwitz-Riley)
    BIAMP_SLOPE_48DB = 4,   // 48 dB/octave (4 biquads)
} tas5805m_biamp_slope_t;

/** Crossover filter type */
typedef enum {
    BIAMP_TYPE_BUTTERWORTH = 0,
    BIAMP_TYPE_LINKWITZ_RILEY = 1,
} tas5805m_biamp_type_t;

/** Baffle step speaker placement - affects compensation amount */
typedef enum {
    BAFFLE_PLACEMENT_FREESTANDING = 0,  // Full 6dB compensation
    BAFFLE_PLACEMENT_NEAR_WALL = 1,     // ~3dB compensation (wall reflection helps)
    BAFFLE_PLACEMENT_CORNER = 2,        // No compensation needed (room gain)
} tas5805m_baffle_placement_t;

/** Per-output PEQ band settings */
typedef struct {
    uint16_t freq;      // Center frequency (20-20000 Hz), 0 = disabled
    int8_t gain;        // Gain * 2 for 0.5 dB resolution (-30 to +30 representing -15 to +15 dB)
    uint8_t q_x10;      // Q factor * 10 (5-100 representing 0.5-10.0)
} tas5805m_biamp_peq_band_t;

#define TAS5805M_BIAMP_PEQ_BANDS 6   // PEQ bands per output

/** Advanced bi-amp crossover settings */
typedef struct {
    // Crossover settings
    uint16_t crossover_freq;        // 20-20000 Hz
    tas5805m_biamp_slope_t slope;   // 12/24/48 dB/oct
    tas5805m_biamp_type_t type;     // Butterworth/Linkwitz-Riley
    uint32_t sample_rate;           // Sample rate in Hz (44100, 48000, 88200, 96000)

    // Speaker protection
    uint16_t subsonic_freq;         // Subsonic HPF frequency (0=off, 20-80 Hz typical)

    // Low output (woofer) settings
    int8_t low_gain;                // Gain * 2 for 0.5 dB resolution (-48 to +48 representing -24 to +24 dB)
    uint8_t low_phase_invert;       // 0=normal, 1=invert
    tas5805m_biamp_peq_band_t low_peq[TAS5805M_BIAMP_PEQ_BANDS];

    // High output (tweeter) settings
    int8_t high_gain;               // Gain * 2 for 0.5 dB resolution (-48 to +48 representing -24 to +24 dB)
    uint8_t high_phase_invert;      // 0=normal, 1=invert
    tas5805m_biamp_peq_band_t high_peq[TAS5805M_BIAMP_PEQ_BANDS];

    // Baffle step compensation (low shelf boost for woofer)
    uint8_t baffle_width_cm;        // 0=disabled, 5-50cm typical
    tas5805m_baffle_placement_t baffle_placement;  // Affects compensation amount

    // Time alignment (delays tweeter to align with woofer)
    uint8_t tweeter_delay_mm;       // 0=disabled, 1-50mm typical

    // Tweeter breakup notch (tames resonance peak at tweeter's upper limit)
    uint16_t notch_freq;            // 0=disabled, 10000-25000 Hz typical
    int8_t notch_gain;              // Gain * 2 for 0.5 dB resolution (0 to -24 representing 0 to -12 dB)
    uint8_t notch_q_x10;            // Q factor * 10 (20-100 representing 2.0-10.0)

    // Air/Brilliance shelf (high shelf for treble sparkle)
    int8_t air_gain;                // Gain * 2 for 0.5 dB resolution (-12 to +12 representing -6 to +6 dB)
} tas5805m_biamp_settings_t;

/** Default bi-amp settings */
#define TAS5805M_BIAMP_DEFAULT_XOVER_FREQ   2000
#define TAS5805M_BIAMP_DEFAULT_SLOPE        BIAMP_SLOPE_24DB
#define TAS5805M_BIAMP_DEFAULT_TYPE         BIAMP_TYPE_LINKWITZ_RILEY

/** Baffle step compensation - uses woofer band 12 (spare) */
#define TAS5805M_BAFFLE_STEP_BAND           12
#define TAS5805M_BAFFLE_DEFAULT_WIDTH       0   // Disabled by default

/** Save advanced bi-amp settings to NVS */
esp_err_t tas5805m_settings_save_biamp(const tas5805m_biamp_settings_t *settings);
/** Load advanced bi-amp settings from NVS */
esp_err_t tas5805m_settings_load_biamp(tas5805m_biamp_settings_t *settings);
/** Apply advanced bi-amp settings to the DAC */
esp_err_t tas5805m_settings_apply_biamp(const tas5805m_biamp_settings_t *settings);

/* ============ Loudness Compensation (Volume-Dependent EQ) ============ */

#define TAS5805M_LOUDNESS_ZONES 5   /** Number of volume zones */

/** Loudness compensation settings - applies bass/treble boost based on volume */
typedef struct {
    uint8_t enabled;                              // 0=off, 1=on
    uint8_t thresholds[TAS5805M_LOUDNESS_ZONES-1]; // 4 thresholds (0-100), e.g. {20,40,60,80}
    int8_t bass_boost[TAS5805M_LOUDNESS_ZONES];   // Bass boost per zone in dB (-12 to +12)
    int8_t treble_boost[TAS5805M_LOUDNESS_ZONES]; // Treble boost per zone in dB (-12 to +12)
} tas5805m_loudness_settings_t;

/** Default loudness thresholds (volume %) */
#define TAS5805M_LOUDNESS_DEFAULT_THRESH {20, 40, 60, 80}
/** Default bass boost per zone (dB) - more boost at lower volumes */
#define TAS5805M_LOUDNESS_DEFAULT_BASS   {6, 4, 2, 1, 0}
/** Default treble boost per zone (dB) */
#define TAS5805M_LOUDNESS_DEFAULT_TREBLE {4, 3, 2, 1, 0}

/** Biquad bands used for loudness shelving filters */
#define TAS5805M_LOUDNESS_BASS_BAND     13  // Low shelf filter band
#define TAS5805M_LOUDNESS_TREBLE_BAND   14  // High shelf filter band

/** Save loudness settings to NVS */
esp_err_t tas5805m_settings_save_loudness(const tas5805m_loudness_settings_t *settings);
/** Load loudness settings from NVS */
esp_err_t tas5805m_settings_load_loudness(tas5805m_loudness_settings_t *settings);
/** Initialize loudness settings to defaults */
void tas5805m_loudness_init_defaults(tas5805m_loudness_settings_t *settings);
/** Apply loudness compensation based on current volume (0-100) */
esp_err_t tas5805m_loudness_apply(int volume);
/** Get current loudness zone (0-4) for a given volume */
int tas5805m_loudness_get_zone(int volume);

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

/* ============ Bi-Amp Preset Export/Import ============ */

/** Preset version for compatibility checking */
#define TAS5805M_BIAMP_PRESET_VERSION 1

/**
 * @brief Export current bi-amp settings to JSON preset format
 *
 * Creates a portable JSON preset that can be shared with 3D speaker models.
 * Includes crossover, per-output gains/PEQ, phase, and loudness settings.
 *
 * @param json_out Buffer to write JSON string
 * @param max_len Maximum buffer size
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tas5805m_biamp_preset_export(char *json_out, size_t max_len);

/**
 * @brief Import bi-amp settings from JSON preset
 *
 * Parses and validates a preset JSON, then applies and saves the settings.
 *
 * @param json_in JSON preset string
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if preset is invalid
 */
esp_err_t tas5805m_biamp_preset_import(const char *json_in);

/**
 * @brief Reset bi-amp settings to factory defaults
 *
 * Clears all bi-amp related NVS keys and re-initializes settings to defaults.
 * This includes crossover, gains, PEQ, phase, loudness, and all other bi-amp
 * parameters.
 *
 * @return ESP_OK on success
 */
esp_err_t tas5805m_biamp_reset_defaults(void);

#endif /* CONFIG_DAC_TAS5805M */

#ifdef __cplusplus
}
#endif

#endif /* __TAS5805M_SETTINGS_H__ */

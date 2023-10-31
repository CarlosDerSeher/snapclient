/*
 * Analog Devices MAX98357 audio hal
 */

#ifndef _MAX98357_H_
#define _MAX98357_H_

#include "audio_hal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize MAX98357 codec chip
 */
esp_err_t max98357_init(audio_hal_codec_config_t *codec_cfg);

/**
 * Deinitialize MAX98357 codec chip
 */
esp_err_t max98357_deinit(void);

/**
 * Set volume - NOT AVAILABLE
 */
esp_err_t max98357_set_volume(int vol);

/**
 * Get volume - NOT AVAILABLE
 */
esp_err_t max98357_get_volume(int *value);

/**
 * Set MAX98357 mute or not
 */
esp_err_t max98357_set_mute(bool enable);

/**
 * Get MAX98357 mute status - NOT IMPLEMENTED
 */
esp_err_t max98357_get_mute(bool *enabled);

#ifdef __cplusplus
}
#endif

#endif

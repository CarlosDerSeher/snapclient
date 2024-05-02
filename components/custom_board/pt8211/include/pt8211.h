/*
 * Princeton Technology PT8211 audio hal
 */

#ifndef _PT8211_H_
#define _PT8211_H_

#include "audio_hal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize PT8211 codec chip
 */
esp_err_t pt8211_init(audio_hal_codec_config_t *codec_cfg);

/**
 * Deinitialize PT8211 codec chip
 */
esp_err_t pt8211_deinit(void);

/**
 * Set volume - NOT AVAILABLE
 */
esp_err_t pt8211_set_volume(int vol);

/**
 * Get volume - NOT AVAILABLE
 */
esp_err_t pt8211_get_volume(int *value);

/**
 * Set PT8211 mute or not
 */
esp_err_t pt8211_set_mute(bool enable);

/**
 * Get PT8211 mute status - NOT IMPLEMENTED
 */
esp_err_t pt8211_get_mute(bool *enabled);

#ifdef __cplusplus
}
#endif

#endif

/**
 * @file tas5805m_settings.c
 * @brief TAS5805M DAC settings persistence and JSON serialization implementation
 */

#include "tas5805m_settings.h"

#if CONFIG_DAC_TAS5805M

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

/* When EQ support is disabled at build time the driver headers may not
 * declare EQ-related constants such as TAS5805M_EQ_BANDS. The settings
 * module intentionally keeps persistence and UI helpers compiled even
 * when driver EQ support is disabled; provide a safe fallback value so
 * those helpers still build without pulling in the driver headers.
 */
#if !defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
#ifndef TAS5805M_EQ_BANDS
#define TAS5805M_EQ_BANDS 0
#endif
#endif

static const char *TAG = "tas5805m_settings";

// Mutex for thread-safe NVS access
static SemaphoreHandle_t tas5805m_settings_mutex = NULL;
// Whether persisted settings have been restored already
static bool tas5805m_settings_restored = false;
// Whether the polling task has been started
static bool tas5805m_settings_poll_started = false;

// Background polling task: waits for TAS5805M to enter PLAY state and then
// triggers a one-time settings application. Exits after a timeout.
static void tas5805m_poll_for_play_task(void *arg)
{
    (void)arg;
    const TickType_t poll_interval = pdMS_TO_TICKS(200);

    ESP_LOGI(TAG, "%s: Polling for codec PLAY state (no timeout)", __func__);

    for (;;) {
        TAS5805_STATE st;
        if (tas5805m_get_state(&st) == ESP_OK) {
            if ((st.state & TAS5805M_CTRL_PLAY) == TAS5805M_CTRL_PLAY) {
                ESP_LOGI(TAG, "%s: Codec entered PLAY — applying delayed persisted settings", __func__);
                // Call delayed apply (requires codec to be running)
                esp_err_t r = tas5805m_settings_apply_delayed();
                if (r == ESP_OK) {
                    tas5805m_settings_restored = true;
                } else {
                    ESP_LOGW(TAG, "%s: tas5805m_settings_apply_delayed() returned %s", __func__, esp_err_to_name(r));
                }
                break;
            }
        }
        vTaskDelay(poll_interval);
    }

    tas5805m_settings_poll_started = false;
    vTaskDelete(NULL);
}

/**
 * Convert TAS5805M_CTRL_STATE enum to human-readable string
 */
static const char* tas5805m_state_to_string(TAS5805M_CTRL_STATE state) {
    // Mask out the mute flag for string conversion
    TAS5805M_CTRL_STATE base_state = state & ~TAS5805M_CTRL_MUTE;
    
    switch (base_state) {
        case TAS5805M_CTRL_DEEP_SLEEP: return "Deep Sleep";
        case TAS5805M_CTRL_SLEEP: return "Sleep";
        case TAS5805M_CTRL_HI_Z: return "Hi-Z";
        case TAS5805M_CTRL_PLAY: 
            return (state & TAS5805M_CTRL_MUTE) ? "Play (Muted)" : "Play";
        default: return "Unknown";
    }
}

static const char* tas5805m_mixer_mode_to_string(TAS5805M_MIXER_MODE mode) {
    switch (mode) {
        case MIXER_UNKNOWN: return "Unknown";
        case MIXER_STEREO: return "Stereo";
        case MIXER_STEREO_INVERSE: return "Stereo (Inverse)";
        case MIXER_MONO: return "Mono";
        case MIXER_RIGHT: return "Right";
        case MIXER_LEFT: return "Left";
        default: return "Unknown";
    }
}

static const char* tas5805m_eq_mode_to_string(TAS5805M_EQ_MODE mode) {
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    switch (mode) {
        case TAS5805M_EQ_MODE_OFF: return "OFF";
        case TAS5805M_EQ_MODE_ON: return "ON";
        case TAS5805M_EQ_MODE_BIAMP: return "BI-AMP";
        case TAS5805M_EQ_MODE_BIAMP_OFF: return "BI-AMP (OFF)";
        default: return "Unknown";
    }
#else
    (void)mode;
    return "OFF";
#endif
}

/* Human readable names for the new EQ UI mode (defined in header) */
const char *tas5805m_eq_ui_mode_to_string(TAS5805M_EQ_UI_MODE m) {
    switch (m) {
        case TAS5805M_EQ_UI_MODE_OFF: return "OFF";
        case TAS5805M_EQ_UI_MODE_15_BAND: return "15-band";
        case TAS5805M_EQ_UI_MODE_15_BAND_BIAMP: return "15-band (bi-amp)";
        case TAS5805M_EQ_UI_MODE_PRESETS: return "EQ Presets";
        default: return "Unknown";
    }
}

/** Save UI mode to NVS */
esp_err_t tas5805m_settings_save_eq_ui_mode(TAS5805M_EQ_UI_MODE mode) {
    ESP_LOGD(TAG, "%s: mode=%d", __func__, (int)mode);
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, TAS5805M_NVS_KEY_EQ_UI_MODE, (int32_t)mode);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Load UI mode from NVS */
esp_err_t tas5805m_settings_load_eq_ui_mode(TAS5805M_EQ_UI_MODE *mode) {
    if (!mode) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, TAS5805M_NVS_KEY_EQ_UI_MODE, &v);
        if (err == ESP_OK) {
            *mode = (TAS5805M_EQ_UI_MODE)v;
            ESP_LOGD(TAG, "%s: Loaded %s=%d from NVS", __func__, TAS5805M_NVS_KEY_EQ_UI_MODE, (int)*mode);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, TAS5805M_NVS_KEY_EQ_UI_MODE);
        } else {
            ESP_LOGW(TAG, "%s: Failed to read '%s' from NVS: %s", __func__, TAS5805M_NVS_KEY_EQ_UI_MODE, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

esp_err_t tas5805m_settings_init(void) {
    if (tas5805m_settings_mutex == NULL) {
        tas5805m_settings_mutex = xSemaphoreCreateMutex();
        if (tas5805m_settings_mutex == NULL) {
            ESP_LOGE(TAG, "%s: Failed to create mutex", __func__);
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "%s: TAS5805M settings manager initialized", __func__);
    /* Apply early settings that are safe to write before the codec starts
       (DAC mode, analog gain, modulation mode, mixer mode). Use the
       dedicated helper so callers and init share the same behavior. */
    if (tas5805m_settings_apply_early() != ESP_OK) {
        ESP_LOGD(TAG, "%s: Early settings apply reported errors (continuing)", __func__);
    }
    /* Start polling task to detect codec START/PLAY and apply persisted
       settings once codec is actually running (I2S clocks present). This
       avoids requiring the driver to call into settings directly. */
    if (!tas5805m_settings_restored && !tas5805m_settings_poll_started) {
        BaseType_t tx = xTaskCreate(tas5805m_poll_for_play_task, "tas5805m_poll_play", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);
        if (tx == pdPASS) {
            tas5805m_settings_poll_started = true;
            ESP_LOGD(TAG, "%s: Started polling task for codec PLAY", __func__);
        } else {
            ESP_LOGW(TAG, "%s: Failed to start polling task; persisted settings may not be applied automatically", __func__);
        }
    }
    return ESP_OK;
}

/* Digital volume persistence removed.
 * Digital volume is treated as read-only / managed by the TAS5805M driver
 * and is not persisted to NVS. Previous save/load functions and the
 * corresponding NVS key were intentionally removed.
 */

esp_err_t tas5805m_settings_save_analog_gain(int gain_half_db) {
    ESP_LOGD(TAG, "%s: gain_half_db=%d", __func__, gain_half_db);
    
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, TAS5805M_NVS_KEY_ANALOG_GAIN, (int32_t)gain_half_db);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

esp_err_t tas5805m_settings_load_analog_gain(int *gain_half_db) {
    if (!gain_half_db) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, TAS5805M_NVS_KEY_ANALOG_GAIN, &v);
        if (err == ESP_OK) {
            *gain_half_db = (int)v;
            ESP_LOGD(TAG, "%s: Loaded analog gain from NVS: %d", __func__, *gain_half_db);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, TAS5805M_NVS_KEY_ANALOG_GAIN);
        } else {
            ESP_LOGW(TAG, "%s: Failed to read analog gain from NVS: %s", __func__, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

esp_err_t tas5805m_settings_save_dac_mode(TAS5805M_DAC_MODE mode) {
    ESP_LOGD(TAG, "%s: mode=%d", __func__, (int)mode);
    
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, TAS5805M_NVS_KEY_DAC_MODE, (int32_t)mode);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: DAC mode saved: %d (%s)", __func__, (int)mode, 
                 mode == TAS5805M_DAC_MODE_BTL ? "BTL" : "PBTL");
    } else {
        ESP_LOGE(TAG, "%s: Failed to save DAC mode: %s", __func__, esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t tas5805m_settings_load_dac_mode(TAS5805M_DAC_MODE *mode) {
    if (!mode) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, TAS5805M_NVS_KEY_DAC_MODE, &v);
        if (err == ESP_OK) {
            *mode = (TAS5805M_DAC_MODE)v;
            ESP_LOGD(TAG, "%s: DAC mode from NVS: %d (%s)", __func__, (int)*mode,
                     *mode == TAS5805M_DAC_MODE_BTL ? "BTL" : "PBTL");
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, TAS5805M_NVS_KEY_DAC_MODE);
        } else {
            ESP_LOGW(TAG, "%s: Failed to read DAC mode from NVS: %s", __func__, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

esp_err_t tas5805m_settings_save_modulation_mode(TAS5805M_MOD_MODE mode, 
                                                   TAS5805M_SW_FREQ freq,
                                                   TAS5805M_BD_FREQ bd_freq) {
    ESP_LOGD(TAG, "%s: mode=%d, freq=%d, bd_freq=%d", __func__, (int)mode, (int)freq, (int)bd_freq);
    
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, TAS5805M_NVS_KEY_MOD_MODE, (int32_t)mode);
        if (err == ESP_OK) {
            err = nvs_set_i32(h, TAS5805M_NVS_KEY_SW_FREQ, (int32_t)freq);
        }
        if (err == ESP_OK) {
            err = nvs_set_i32(h, TAS5805M_NVS_KEY_BD_FREQ, (int32_t)bd_freq);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: Modulation mode saved: mode=%d, freq=%d, bd_freq=%d", 
                 __func__, (int)mode, (int)freq, (int)bd_freq);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save modulation mode: %s", __func__, esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t tas5805m_settings_load_modulation_mode(TAS5805M_MOD_MODE *mode,
                                                   TAS5805M_SW_FREQ *freq,
                                                   TAS5805M_BD_FREQ *bd_freq) {
    if (!mode || !freq || !bd_freq) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v_mode = 0, v_freq = 0, v_bd = 0;
        err = nvs_get_i32(h, TAS5805M_NVS_KEY_MOD_MODE, &v_mode);
        if (err == ESP_OK) {
            err = nvs_get_i32(h, TAS5805M_NVS_KEY_SW_FREQ, &v_freq);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, TAS5805M_NVS_KEY_MOD_MODE);
        }
        if (err == ESP_OK) {
            err = nvs_get_i32(h, TAS5805M_NVS_KEY_BD_FREQ, &v_bd);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, TAS5805M_NVS_KEY_SW_FREQ);
        }
        if (err == ESP_OK) {
            *mode = (TAS5805M_MOD_MODE)v_mode;
            *freq = (TAS5805M_SW_FREQ)v_freq;
            *bd_freq = (TAS5805M_BD_FREQ)v_bd;
            ESP_LOGD(TAG, "%s: Modulation mode from NVS: mode=%d, freq=%d, bd_freq=%d", 
                     __func__, (int)*mode, (int)*freq, (int)*bd_freq);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: Failed to read modulation mode from NVS: %s", __func__, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Save mixer mode to NVS */
esp_err_t tas5805m_settings_save_mixer_mode(TAS5805M_MIXER_MODE mode) {
    ESP_LOGD(TAG, "%s: mode=%d", __func__, (int)mode);

    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, TAS5805M_NVS_KEY_MIXER_MODE, (int32_t)mode);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: Mixer mode saved: %d", __func__, (int)mode);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save mixer mode: %s", __func__, esp_err_to_name(err));
    }

    return err;
}

/** Load mixer mode from NVS */
esp_err_t tas5805m_settings_load_mixer_mode(TAS5805M_MIXER_MODE *mode) {
    if (!mode) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, TAS5805M_NVS_KEY_MIXER_MODE, &v);
        if (err == ESP_OK) {
            *mode = (TAS5805M_MIXER_MODE)v;
            ESP_LOGD(TAG, "%s: Mixer mode from NVS: %d", __func__, (int)*mode);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, TAS5805M_NVS_KEY_MIXER_MODE);
        } else {
            ESP_LOGW(TAG, "%s: Failed to read mixer mode from NVS: %s", __func__, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Save EQ mode to NVS */
esp_err_t tas5805m_settings_save_eq_mode(TAS5805M_EQ_MODE mode) {
    ESP_LOGD(TAG, "%s: mode=%d", __func__, (int)mode);

    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, TAS5805M_NVS_KEY_EQ_MODE, (int32_t)mode);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: EQ mode saved: %d", __func__, (int)mode);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save EQ mode: %s", __func__, esp_err_to_name(err));
    }

    return err;
}

/** Save per-band EQ gain for a specific channel */
esp_err_t tas5805m_settings_save_eq_gain(TAS5805M_EQ_CHANNELS ch, int band, int gain_db) {
    ESP_LOGD(TAG, "%s: ch=%d band=%d gain=%d", __func__, (int)ch, band, gain_db);
    
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    if (band < 0 || band >= TAS5805M_EQ_BANDS) return ESP_ERR_INVALID_ARG;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    char key[32];
    if (ch == TAS5805M_EQ_CHANNELS_LEFT) {
        snprintf(key, sizeof(key), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_L_PREFIX, band);
    } else {
        snprintf(key, sizeof(key), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_R_PREFIX, band);
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, key, (int32_t)gain_db);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Load per-band EQ gain for a specific channel */
esp_err_t tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS ch, int band, int *gain_db) {
    if (!gain_db) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    if (band < 0 || band >= TAS5805M_EQ_BANDS) return ESP_ERR_INVALID_ARG;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    char key[32];
    if (ch == TAS5805M_EQ_CHANNELS_LEFT) {
        snprintf(key, sizeof(key), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_L_PREFIX, band);
    } else {
        snprintf(key, sizeof(key), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_R_PREFIX, band);
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, key, &v);
        if (err == ESP_OK) {
            *gain_db = (int)v;
            ESP_LOGD(TAG, "%s: Loaded %s=%d from NVS", __func__, key, *gain_db);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, key);
        } else {
            ESP_LOGW(TAG, "%s: Failed to read '%s' from NVS: %s", __func__, key, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Save EQ profile/preset for a specific channel */
esp_err_t tas5805m_settings_save_eq_profile(TAS5805M_EQ_CHANNELS ch, TAS5805M_EQ_PROFILE profile) {
    ESP_LOGD(TAG, "%s: ch=%d profile=%d", __func__, (int)ch, (int)profile);

    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const char *key = (ch == TAS5805M_EQ_CHANNELS_LEFT) ? TAS5805M_NVS_KEY_EQ_PROFILE_L : TAS5805M_NVS_KEY_EQ_PROFILE_R;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, key, (int32_t)profile);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Load EQ profile/preset for a specific channel */
esp_err_t tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS ch, TAS5805M_EQ_PROFILE *profile) {
    if (!profile) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const char *key = (ch == TAS5805M_EQ_CHANNELS_LEFT) ? TAS5805M_NVS_KEY_EQ_PROFILE_L : TAS5805M_NVS_KEY_EQ_PROFILE_R;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, key, &v);
        if (err == ESP_OK) {
            *profile = (TAS5805M_EQ_PROFILE)v;
            ESP_LOGD(TAG, "%s: Loaded %s=%d from NVS", __func__, key, (int)*profile);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, key);
        } else {
            ESP_LOGW(TAG, "%s: Failed to read '%s' from NVS: %s", __func__, key, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Save per-output channel gain (single value per channel, in dB) */
esp_err_t tas5805m_settings_save_channel_gain(TAS5805M_EQ_CHANNELS ch, int gain_db) {
    ESP_LOGD(TAG, "%s: ch=%d gain=%d", __func__, (int)ch, gain_db);

    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const char *key = (ch == TAS5805M_EQ_CHANNELS_LEFT) ? TAS5805M_NVS_KEY_CHANNEL_GAIN_L : TAS5805M_NVS_KEY_CHANNEL_GAIN_R;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, key, (int32_t)gain_db);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Load per-output channel gain (single value per channel, in dB) */
esp_err_t tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS ch, int *gain_db) {
    if (!gain_db) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const char *key = (ch == TAS5805M_EQ_CHANNELS_LEFT) ? TAS5805M_NVS_KEY_CHANNEL_GAIN_L : TAS5805M_NVS_KEY_CHANNEL_GAIN_R;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, key, &v);
        if (err == ESP_OK) {
            *gain_db = (int)v;
            ESP_LOGD(TAG, "%s: Loaded %s=%d from NVS", __func__, key, *gain_db);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, key);
        } else {
            ESP_LOGW(TAG, "%s: Failed to read '%s' from NVS: %s", __func__, key, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Load EQ mode from NVS */
esp_err_t tas5805m_settings_load_eq_mode(TAS5805M_EQ_MODE *mode) {
    if (!mode) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, TAS5805M_NVS_KEY_EQ_MODE, &v);
        if (err == ESP_OK) {
            *mode = (TAS5805M_EQ_MODE)v;
            ESP_LOGD(TAG, "%s: EQ mode from NVS: %d", __func__, (int)*mode);
        } else if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGD(TAG, "%s: NVS key '%s' not found", __func__, TAS5805M_NVS_KEY_EQ_MODE);
        } else {
            ESP_LOGW(TAG, "%s: Failed to read EQ mode from NVS: %s", __func__, esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

esp_err_t tas5805m_settings_get_json(char *json_out, size_t max_len) {
    ESP_LOGD(TAG, "%s: max_len=%zu", __func__, max_len);
    
    if (!json_out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Query current state from tas5805m component
    TAS5805_STATE dac_state;
    esp_err_t err = tas5805m_get_state(&dac_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to get DAC state: %s", __func__, esp_err_to_name(err));
        return err;
    }

    // Get DAC mode
    TAS5805M_DAC_MODE dac_mode;
    if (tas5805m_get_dac_mode(&dac_mode) != ESP_OK) {
        dac_mode = TAS5805M_DAC_MODE_BTL;
    }

    // Get modulation mode
    TAS5805M_MOD_MODE mod_mode;
    TAS5805M_SW_FREQ sw_freq;
    TAS5805M_BD_FREQ bd_freq;
    if (tas5805m_get_modulation_mode(&mod_mode, &sw_freq, &bd_freq) != ESP_OK) {
        mod_mode = MOD_MODE_BD;
        sw_freq = SW_FREQ_768K;
        bd_freq = SW_FREQ_80K;
    }

    // Get analog gain
    uint8_t analog_gain;
    if (tas5805m_get_again(&analog_gain) != ESP_OK) {
        analog_gain = 0;
    }

    // Get digital volume (stored in TAS5805_STATE)
    uint8_t digital_volume;
    if (tas5805m_get_digital_volume(&digital_volume) != ESP_OK) {
        digital_volume = TAS5805M_VOLUME_DIGITAL_DEFAULT;
    }

    // Build JSON
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to create JSON root", __func__);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "state", (int)dac_state.state);
    cJSON_AddStringToObject(root, "state_name", tas5805m_state_to_string(dac_state.state));
    cJSON_AddNumberToObject(root, "volume", dac_state.volume);
    
    bool muted = (dac_state.state & TAS5805M_CTRL_MUTE) != 0;
    cJSON_AddBoolToObject(root, "muted", muted);
    
    cJSON_AddNumberToObject(root, "digital_volume", digital_volume);
    cJSON_AddNumberToObject(root, "analog_gain", analog_gain);
    cJSON_AddNumberToObject(root, "dac_mode", (int)dac_mode);
    cJSON_AddStringToObject(root, "dac_mode_name", dac_mode == TAS5805M_DAC_MODE_BTL ? "BTL" : "PBTL");
    cJSON_AddNumberToObject(root, "modulation_mode", (int)mod_mode);
    cJSON_AddNumberToObject(root, "sw_freq", (int)sw_freq);
    cJSON_AddNumberToObject(root, "bd_freq", (int)bd_freq);
    
    /* EQ mode - query driver when available, otherwise default to OFF */
    TAS5805M_EQ_MODE eq_mode_val = TAS5805M_EQ_MODE_OFF;
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    if (tas5805m_get_eq_mode(&eq_mode_val) != ESP_OK) {
        eq_mode_val = TAS5805M_EQ_MODE_OFF;
    }
#endif
    cJSON_AddNumberToObject(root, "eq_mode", (int)eq_mode_val);
    cJSON_AddStringToObject(root, "eq_mode_name", tas5805m_eq_mode_to_string(eq_mode_val));
    /* EQ UI mode (controls which UI elements to show) - prefer persisted value */
    TAS5805M_EQ_UI_MODE ui_mode_val = TAS5805M_EQ_UI_MODE_OFF;
    if (tas5805m_settings_load_eq_ui_mode(&ui_mode_val) != ESP_OK) {
        /* derive from driver eq_mode if not persisted */
        if (eq_mode_val == TAS5805M_EQ_MODE_OFF) ui_mode_val = TAS5805M_EQ_UI_MODE_OFF;
        else if (eq_mode_val == TAS5805M_EQ_MODE_ON) ui_mode_val = TAS5805M_EQ_UI_MODE_15_BAND;
        else ui_mode_val = TAS5805M_EQ_UI_MODE_15_BAND_BIAMP;
    }
    cJSON_AddNumberToObject(root, "eq_ui_mode", (int)ui_mode_val);
    cJSON_AddStringToObject(root, "eq_ui_mode_name", tas5805m_eq_ui_mode_to_string(ui_mode_val));
    /* EQ profile/preset per channel */
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    {
        TAS5805M_EQ_PROFILE prof_l = FLAT, prof_r = FLAT;
        if (tas5805m_settings_restored) {
            if (tas5805m_get_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, &prof_l) == ESP_OK) {
                cJSON_AddNumberToObject(root, "eq_profile_l", (int)prof_l);
            } else {
                if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_LEFT, &prof_l) == ESP_OK) {
                    cJSON_AddNumberToObject(root, "eq_profile_l", (int)prof_l);
                }
            }

            if (tas5805m_get_eq_profile_channel(TAS5805M_EQ_CHANNELS_RIGHT, &prof_r) == ESP_OK) {
                cJSON_AddNumberToObject(root, "eq_profile_r", (int)prof_r);
            } else {
                if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_RIGHT, &prof_r) == ESP_OK) {
                    cJSON_AddNumberToObject(root, "eq_profile_r", (int)prof_r);
                }
            }

            /* Channel gain (left/right) for presets UI */
            int ch_gain = 0;
            int8_t chg = 0;
            if (tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &chg) == ESP_OK) {
                cJSON_AddNumberToObject(root, "channel_gain_l", (int)chg);
            } else if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &ch_gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, "channel_gain_l", (int)ch_gain);
            }
            if (tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &chg) == ESP_OK) {
                cJSON_AddNumberToObject(root, "channel_gain_r", (int)chg);
            } else if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &ch_gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, "channel_gain_r", (int)ch_gain);
            }
        } else {
            /* Not restored yet — use persisted values from NVS so UI reflects saved settings */
            if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_LEFT, &prof_l) == ESP_OK) {
                cJSON_AddNumberToObject(root, "eq_profile_l", (int)prof_l);
            }
            if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_RIGHT, &prof_r) == ESP_OK) {
                cJSON_AddNumberToObject(root, "eq_profile_r", (int)prof_r);
            }
            int ch_gain = 0;
            if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &ch_gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, "channel_gain_l", (int)ch_gain);
            }
            if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &ch_gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, "channel_gain_r", (int)ch_gain);
            }
        }
    }
#endif
    /* Mixer mode from cached state */
    cJSON_AddNumberToObject(root, "mixer_mode", (int)dac_state.mixer_mode);
    cJSON_AddStringToObject(root, "mixer_mode_name", tas5805m_mixer_mode_to_string(dac_state.mixer_mode));

    /* Per-band EQ gains (left/right) so UI can display current values without refresh.
     * Prefer reading current driver state; if driver isn't ready or returns an error
     * fall back to persisted values from NVS so the UI reflects saved settings.
     */
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
        int gain = 0;
        char key_l[32];
        char key_r[32];
        snprintf(key_l, sizeof(key_l), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_L_PREFIX, band);
        snprintf(key_r, sizeof(key_r), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_R_PREFIX, band);

        if (tas5805m_settings_restored) {
            // Left channel: prefer driver value, otherwise load persisted NVS value
            if (tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, key_l, gain);
            } else if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, key_l, gain);
            }

            // Right channel: prefer driver value, otherwise load persisted NVS value
            gain = 0;
            if (tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, key_r, gain);
            } else if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, key_r, gain);
            }
        } else {
            /* Not yet restored; use persisted values only */
            if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, key_l, gain);
            }
            gain = 0;
            if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain) == ESP_OK) {
                cJSON_AddNumberToObject(root, key_r, gain);
            }
        }
    }
#endif

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);
    ESP_LOGD(TAG, "%s: Generated JSON size: %zu bytes (buffer size: %zu)", __func__, json_len, max_len);

    if (json_len >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer (%zu >= %zu)", __func__, json_len, max_len);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGV(TAG, "%s: JSON generated: %s", __func__, json_out);
    return ESP_OK;
}

esp_err_t tas5805m_settings_set_from_json(const char *json_in) {
    ESP_LOGV(TAG, "%s: json=%s", __func__, json_in);
    
    if (!json_in) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_in);
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to parse JSON", __func__);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;

    // Update state if present
    cJSON *state_item = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsNumber(state_item)) {
        TAS5805M_CTRL_STATE new_state = (TAS5805M_CTRL_STATE)state_item->valueint;
        
        // Apply to DAC (do NOT persist state - state is managed by application)
        err = tas5805m_set_state(new_state);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied state %d (%s) to DAC (not persisted)", __func__, 
                     (int)new_state, tas5805m_state_to_string(new_state));
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply state to DAC: %s", 
                     __func__, esp_err_to_name(err));
        }
    }

    // Update digital volume if present (expects raw uint8_t value)
    cJSON *dig_vol_item = cJSON_GetObjectItem(root, "digital_volume");
    if (cJSON_IsNumber(dig_vol_item)) {
        uint8_t vol = (uint8_t)dig_vol_item->valueint;
        
        // Apply to DAC (do NOT persist digital volume - managed by application)
        err = tas5805m_set_digital_volume(vol);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied digital volume %d to DAC (not persisted)", __func__, vol);
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply digital volume: %s", 
                     __func__, esp_err_to_name(err));
        }
    }

    // Update analog gain if present (expects uint8_t 0-31)
    cJSON *ana_gain_item = cJSON_GetObjectItem(root, "analog_gain");
    if (cJSON_IsNumber(ana_gain_item)) {
        uint8_t gain = (uint8_t)ana_gain_item->valueint;
        
        err = tas5805m_set_again(gain);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied analog gain %d to DAC", __func__, gain);
            // Note: We're saving the raw value, conversion to half_db would need lookup table
            tas5805m_settings_save_analog_gain((int)gain);
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply analog gain: %s", 
                     __func__, esp_err_to_name(err));
        }
    }

    // Update DAC mode if present
    cJSON *dac_mode_item = cJSON_GetObjectItem(root, "dac_mode");
    if (cJSON_IsNumber(dac_mode_item)) {
        TAS5805M_DAC_MODE mode = (TAS5805M_DAC_MODE)dac_mode_item->valueint;
        
        err = tas5805m_set_dac_mode(mode);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied DAC mode %d (%s) to DAC", __func__, 
                     (int)mode, mode == TAS5805M_DAC_MODE_BTL ? "BTL" : "PBTL");
            tas5805m_settings_save_dac_mode(mode);
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply DAC mode: %s", 
                     __func__, esp_err_to_name(err));
        }
    }

    // Update modulation mode if any parameter provided. Support partial updates
    // (UI typically sends only the changed parameter). We'll query current
    // modulation settings from the driver and apply a merged update.
    cJSON *mod_mode_item = cJSON_GetObjectItem(root, "modulation_mode");
    cJSON *sw_freq_item = cJSON_GetObjectItem(root, "sw_freq");
    cJSON *bd_freq_item = cJSON_GetObjectItem(root, "bd_freq");

    if (cJSON_IsNumber(mod_mode_item) || cJSON_IsNumber(sw_freq_item) || cJSON_IsNumber(bd_freq_item)) {
        TAS5805M_MOD_MODE cur_mod = MOD_MODE_BD;
        TAS5805M_SW_FREQ cur_sw = SW_FREQ_768K;
        TAS5805M_BD_FREQ cur_bd = SW_FREQ_80K;

        // Read current values where possible
        if (tas5805m_get_modulation_mode(&cur_mod, &cur_sw, &cur_bd) != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to read current modulation mode, using defaults", __func__);
        }

        // Override with provided values
        if (cJSON_IsNumber(mod_mode_item)) {
            cur_mod = (TAS5805M_MOD_MODE)mod_mode_item->valueint;
        }
        if (cJSON_IsNumber(sw_freq_item)) {
            cur_sw = (TAS5805M_SW_FREQ)sw_freq_item->valueint;
        }
        if (cJSON_IsNumber(bd_freq_item)) {
            cur_bd = (TAS5805M_BD_FREQ)bd_freq_item->valueint;
        }

        // Apply merged settings
        err = tas5805m_set_modulation_mode(cur_mod, cur_sw, cur_bd);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied modulation mode: mode=%d, freq=%d, bd_freq=%d",
                     __func__, (int)cur_mod, (int)cur_sw, (int)cur_bd);
            tas5805m_settings_save_modulation_mode(cur_mod, cur_sw, cur_bd);
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply modulation mode: %s",
                     __func__, esp_err_to_name(err));
        }
    }

    // Update mixer mode if present
    cJSON *mixer_mode_item = cJSON_GetObjectItem(root, "mixer_mode");
    if (cJSON_IsNumber(mixer_mode_item)) {
        TAS5805M_MIXER_MODE mode = (TAS5805M_MIXER_MODE)mixer_mode_item->valueint;
        err = tas5805m_set_mixer_mode(mode);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied mixer mode %d to DAC", __func__, (int)mode);
            tas5805m_settings_save_mixer_mode(mode);
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply mixer mode: %s", __func__, esp_err_to_name(err));
        }
    }

    // Update EQ mode if present
    cJSON *eq_mode_item = cJSON_GetObjectItem(root, "eq_mode");
    if (cJSON_IsNumber(eq_mode_item)) {
        TAS5805M_EQ_MODE new_eq = (TAS5805M_EQ_MODE)eq_mode_item->valueint;
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
        err = tas5805m_set_eq_mode(new_eq);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied EQ mode %d (%s) to DAC", __func__, (int)new_eq, tas5805m_eq_mode_to_string(new_eq));
            /* Persist EQ mode */
            esp_err_t serr = tas5805m_settings_save_eq_mode(new_eq);
            if (serr != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to persist EQ mode: %s", __func__, esp_err_to_name(serr));
            }
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply EQ mode: %s", __func__, esp_err_to_name(err));
        }
#else
        ESP_LOGW(TAG, "%s: EQ support disabled in build; ignoring EQ mode change", __func__);
        (void)new_eq;
#endif
    }

    // Update EQ UI selection if present. This controls which UI elements are shown
    // and how values are applied/persisted (15-band vs presets etc.). We persist
    // the UI selection in NVS and map it to the underlying driver EQ mode.
    cJSON *eq_ui_item = cJSON_GetObjectItem(root, "eq_ui_mode");
    if (cJSON_IsNumber(eq_ui_item)) {
        TAS5805M_EQ_UI_MODE ui = (TAS5805M_EQ_UI_MODE)eq_ui_item->valueint;
        ESP_LOGI(TAG, "%s: Requested EQ UI mode %d", __func__, (int)ui);

        // Persist UI selection
        esp_err_t uerr = tas5805m_settings_save_eq_ui_mode(ui);
        if (uerr != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to persist EQ UI mode: %s", __func__, esp_err_to_name(uerr));
        }

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
        TAS5805M_EQ_MODE drv = TAS5805M_EQ_MODE_OFF;
        switch (ui) {
            case TAS5805M_EQ_UI_MODE_OFF: drv = TAS5805M_EQ_MODE_OFF; break;
            case TAS5805M_EQ_UI_MODE_15_BAND: drv = TAS5805M_EQ_MODE_ON; break;
            case TAS5805M_EQ_UI_MODE_15_BAND_BIAMP: drv = TAS5805M_EQ_MODE_BIAMP; break;
            case TAS5805M_EQ_UI_MODE_PRESETS: drv = TAS5805M_EQ_MODE_BIAMP; break;
            default: drv = TAS5805M_EQ_MODE_OFF; break;
        }

        esp_err_t serr = tas5805m_set_eq_mode(drv);
        if (serr == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied driver EQ mode %d for UI selection %d", __func__, (int)drv, (int)ui);
            /* Persist the mapped driver EQ mode as well so both UI and driver
             * mode remain consistent across reboots.
             */
            esp_err_t perr = tas5805m_settings_save_eq_mode(drv);
            if (perr != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to persist driver EQ mode: %s", __func__, esp_err_to_name(perr));
            }
        } else {
            ESP_LOGE(TAG, "%s: Failed to set driver EQ mode: %s", __func__, esp_err_to_name(serr));
        }

        // Immediately apply persisted data according to selected UI mode
        if (ui == TAS5805M_EQ_UI_MODE_15_BAND) {
            for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
                int gain = 0;
                if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                    if (tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain) == ESP_OK) {
                        ESP_LOGI(TAG, "%s: Applied L band %d = %d (15-band)", __func__, band, gain);
                    }
                }
            }
        } else if (ui == TAS5805M_EQ_UI_MODE_15_BAND_BIAMP) {
            for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
                int gain = 0;
                if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                    tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
                }
                if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain) == ESP_OK) {
                    tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, gain);
                }
            }
        } else if (ui == TAS5805M_EQ_UI_MODE_PRESETS) {
            TAS5805M_EQ_PROFILE prof = FLAT;
            if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_LEFT, &prof) == ESP_OK) {
                tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, prof);
            }
            if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_RIGHT, &prof) == ESP_OK) {
                tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_RIGHT, prof);
            }
        }
#else
        ESP_LOGW(TAG, "%s: EQ support disabled; ignoring eq_ui_mode change", __func__);
#endif
    }

    // Update EQ profile/preset per channel if present
    cJSON *eq_prof_l_item = cJSON_GetObjectItem(root, "eq_profile_l");
    if (cJSON_IsNumber(eq_prof_l_item)) {
        TAS5805M_EQ_PROFILE p = (TAS5805M_EQ_PROFILE)eq_prof_l_item->valueint;
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
        esp_err_t serr = tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, p);
        if (serr == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied EQ profile L = %d", __func__, (int)p);
            esp_err_t perr = tas5805m_settings_save_eq_profile(TAS5805M_EQ_CHANNELS_LEFT, p);
            if (perr != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to persist EQ profile L: %s", __func__, esp_err_to_name(perr));
            }
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply EQ profile L: %s", __func__, esp_err_to_name(serr));
        }
#else
        ESP_LOGW(TAG, "%s: EQ support disabled; ignoring eq_profile_l", __func__);
#endif
    }

    cJSON *eq_prof_r_item = cJSON_GetObjectItem(root, "eq_profile_r");
    if (cJSON_IsNumber(eq_prof_r_item)) {
        TAS5805M_EQ_PROFILE p = (TAS5805M_EQ_PROFILE)eq_prof_r_item->valueint;
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
        esp_err_t serr = tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_RIGHT, p);
        if (serr == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied EQ profile R = %d", __func__, (int)p);
            esp_err_t perr = tas5805m_settings_save_eq_profile(TAS5805M_EQ_CHANNELS_RIGHT, p);
            if (perr != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to persist EQ profile R: %s", __func__, esp_err_to_name(perr));
            }
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply EQ profile R: %s", __func__, esp_err_to_name(serr));
        }
#else
        ESP_LOGW(TAG, "%s: EQ support disabled; ignoring eq_profile_r", __func__);
#endif
    }

    /* Channel gain (L/R) handling for presets UI */
    cJSON *ch_gain_l_item = cJSON_GetObjectItem(root, TAS5805M_NVS_KEY_CHANNEL_GAIN_L);
    if (cJSON_IsNumber(ch_gain_l_item)) {
        int gain = ch_gain_l_item->valueint;
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
        esp_err_t serr = tas5805m_set_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, (int8_t)gain);
        if (serr == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied Channel Gain L = %d dB", __func__, gain);
            esp_err_t perr = tas5805m_settings_save_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, gain);
            if (perr != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to persist Channel Gain L: %s", __func__, esp_err_to_name(perr));
            }
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply Channel Gain L: %s", __func__, esp_err_to_name(serr));
        }
#else
        ESP_LOGW(TAG, "%s: EQ support disabled; ignoring channel_gain_l", __func__);
#endif
    }

    cJSON *ch_gain_r_item = cJSON_GetObjectItem(root, TAS5805M_NVS_KEY_CHANNEL_GAIN_R);
    if (cJSON_IsNumber(ch_gain_r_item)) {
        int gain = ch_gain_r_item->valueint;
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
        esp_err_t serr = tas5805m_set_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, (int8_t)gain);
        if (serr == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied Channel Gain R = %d dB", __func__, gain);
            esp_err_t perr = tas5805m_settings_save_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, gain);
            if (perr != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to persist Channel Gain R: %s", __func__, esp_err_to_name(perr));
            }
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply Channel Gain R: %s", __func__, esp_err_to_name(serr));
        }
#else
        ESP_LOGW(TAG, "%s: EQ support disabled; ignoring channel_gain_r", __func__);
#endif
    }

    // Handle per-band EQ gain keys in deterministic order: left then right per band
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
        char key_l[32];
        char key_r[32];
        snprintf(key_l, sizeof(key_l), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_L_PREFIX, band);
        snprintf(key_r, sizeof(key_r), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_R_PREFIX, band);

        cJSON *item_l = cJSON_GetObjectItem(root, key_l);
        if (cJSON_IsNumber(item_l)) {
            int gain = item_l->valueint;
            esp_err_t serr = tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
            if (serr == ESP_OK) {
                ESP_LOGI(TAG, "%s: Applied EQ gain L band %d = %d", __func__, band, gain);
                esp_err_t perr = tas5805m_settings_save_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
                if (perr != ESP_OK) {
                    ESP_LOGW(TAG, "%s: Failed to persist EQ gain L band %d: %s", __func__, band, esp_err_to_name(perr));
                }
            } else {
                ESP_LOGE(TAG, "%s: Failed to apply EQ gain L band %d: %s", __func__, band, esp_err_to_name(serr));
            }
        }

        cJSON *item_r = cJSON_GetObjectItem(root, key_r);
        if (cJSON_IsNumber(item_r)) {
            int gain = item_r->valueint;
            esp_err_t serr = tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, gain);
            if (serr == ESP_OK) {
                ESP_LOGI(TAG, "%s: Applied EQ gain R band %d = %d", __func__, band, gain);
                esp_err_t perr = tas5805m_settings_save_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, gain);
                if (perr != ESP_OK) {
                    ESP_LOGW(TAG, "%s: Failed to persist EQ gain R band %d: %s", __func__, band, esp_err_to_name(perr));
                }
            } else {
                ESP_LOGE(TAG, "%s: Failed to apply EQ gain R band %d: %s", __func__, band, esp_err_to_name(serr));
            }
        }
    }
#else
    (void)root; // keep compiler happy when EQ support disabled
#endif

    cJSON_Delete(root);
    return err;
}

esp_err_t tas5805m_settings_get_schema_json(char *json_out, size_t max_len) {
    ESP_LOGD(TAG, "%s: max_len=%zu", __func__, max_len);
    
    if (!json_out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get current state for schema
    TAS5805_STATE dac_state;
    esp_err_t err = tas5805m_get_state(&dac_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to get DAC state: %s", __func__, esp_err_to_name(err));
        // Continue with default state
        dac_state.state = TAS5805M_CTRL_PLAY;
    }

    // Get current digital volume (raw value)
    uint8_t digital_volume;
    if (tas5805m_get_digital_volume(&digital_volume) != ESP_OK) {
        digital_volume = TAS5805M_VOLUME_DIGITAL_DEFAULT;
    }

    // Get current analog gain (raw value)
    uint8_t analog_gain;
    if (tas5805m_get_again(&analog_gain) != ESP_OK) {
        analog_gain = 0;
    }

    // Get current DAC mode
    TAS5805M_DAC_MODE dac_mode;
    if (tas5805m_get_dac_mode(&dac_mode) != ESP_OK) {
        dac_mode = TAS5805M_DAC_MODE_BTL;
    }

    // Get current modulation mode
    TAS5805M_MOD_MODE mod_mode;
    TAS5805M_SW_FREQ sw_freq;
    TAS5805M_BD_FREQ bd_freq;
    if (tas5805m_get_modulation_mode(&mod_mode, &sw_freq, &bd_freq) != ESP_OK) {
        mod_mode = MOD_MODE_BD;
        sw_freq = SW_FREQ_768K;
        bd_freq = SW_FREQ_80K;
    }
    /* EQ mode - query driver when available, otherwise default to OFF */
    TAS5805M_EQ_MODE eq_mode_val = TAS5805M_EQ_MODE_OFF;
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    if (tas5805m_get_eq_mode(&eq_mode_val) != ESP_OK) {
        eq_mode_val = TAS5805M_EQ_MODE_OFF;
    }
#endif

    // Build schema JSON
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to create JSON root", __func__);
        return ESP_ERR_NO_MEM;
    }

    cJSON *groups = cJSON_CreateArray();
    
    // ===== Volume Group =====
    cJSON *volume_group = cJSON_CreateObject();
    cJSON_AddStringToObject(volume_group, "name", "Volume");
    cJSON_AddStringToObject(volume_group, "description", "Digital and analog volume control");
    
    cJSON *volume_params = cJSON_CreateArray();
    
    // Digital Volume parameter (raw register value 0-255) - READ-ONLY (managed by application)
    cJSON *dig_vol_param = cJSON_CreateObject();
    cJSON_AddStringToObject(dig_vol_param, "key", "digital_volume");
    cJSON_AddStringToObject(dig_vol_param, "name", "Digital Volume");
    cJSON_AddStringToObject(dig_vol_param, "type", "range");
    cJSON_AddStringToObject(dig_vol_param, "unit", "");
    cJSON_AddNumberToObject(dig_vol_param, "min", TAS5805M_VOLUME_DIGITAL_MIN);
    cJSON_AddNumberToObject(dig_vol_param, "max", TAS5805M_VOLUME_DIGITAL_MAX);
    cJSON_AddNumberToObject(dig_vol_param, "step", 1);
    cJSON_AddNumberToObject(dig_vol_param, "default", TAS5805M_VOLUME_DIGITAL_DEFAULT);
    cJSON_AddNumberToObject(dig_vol_param, "current", digital_volume);
    cJSON_AddBoolToObject(dig_vol_param, "readonly", true);
    cJSON_AddItemToArray(volume_params, dig_vol_param);
    
    // Analog Gain parameter (raw register value 0-31)
    cJSON *ana_gain_param = cJSON_CreateObject();
    cJSON_AddStringToObject(ana_gain_param, "key", "analog_gain");
    cJSON_AddStringToObject(ana_gain_param, "name", "Analog Gain");
    cJSON_AddStringToObject(ana_gain_param, "type", "range");
    cJSON_AddStringToObject(ana_gain_param, "unit", "");
    cJSON_AddNumberToObject(ana_gain_param, "min", 0);
    cJSON_AddNumberToObject(ana_gain_param, "max", 31);
    cJSON_AddNumberToObject(ana_gain_param, "step", 1);
    cJSON_AddNumberToObject(ana_gain_param, "default", 0);
    cJSON_AddNumberToObject(ana_gain_param, "current", analog_gain);
    cJSON_AddItemToArray(volume_params, ana_gain_param);
    
    cJSON_AddItemToObject(volume_group, "parameters", volume_params);
    cJSON_AddItemToArray(groups, volume_group);

    // ===== State Group =====
    cJSON *state_group = cJSON_CreateObject();
    cJSON_AddStringToObject(state_group, "name", "State");
    cJSON_AddStringToObject(state_group, "description", "DAC power and operation state");
    
    cJSON *state_params = cJSON_CreateArray();
    
    // State parameter - READ-ONLY (managed by application)
    cJSON *state_param = cJSON_CreateObject();
    cJSON_AddStringToObject(state_param, "key", "state");
    cJSON_AddStringToObject(state_param, "name", "DAC State");
    cJSON_AddStringToObject(state_param, "type", "enum");
    cJSON_AddNumberToObject(state_param, "current", (int)dac_state.state);
    cJSON_AddBoolToObject(state_param, "readonly", true);
    
    // State enum values
    cJSON *state_values = cJSON_CreateArray();
    
    cJSON *val_deep_sleep = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_deep_sleep, "value", TAS5805M_CTRL_DEEP_SLEEP);
    cJSON_AddStringToObject(val_deep_sleep, "name", "Deep Sleep");
    cJSON_AddItemToArray(state_values, val_deep_sleep);
    
    cJSON *val_sleep = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_sleep, "value", TAS5805M_CTRL_SLEEP);
    cJSON_AddStringToObject(val_sleep, "name", "Sleep");
    cJSON_AddItemToArray(state_values, val_sleep);
    
    cJSON *val_hiz = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_hiz, "value", TAS5805M_CTRL_HI_Z);
    cJSON_AddStringToObject(val_hiz, "name", "Hi-Z");
    cJSON_AddItemToArray(state_values, val_hiz);
    
    cJSON *val_play = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_play, "value", TAS5805M_CTRL_PLAY);
    cJSON_AddStringToObject(val_play, "name", "Play");
    cJSON_AddItemToArray(state_values, val_play);
    
    cJSON *val_play_mute = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_play_mute, "value", TAS5805M_CTRL_PLAY_MUTE);
    cJSON_AddStringToObject(val_play_mute, "name", "Play (Muted)");
    cJSON_AddItemToArray(state_values, val_play_mute);
    
    cJSON_AddItemToObject(state_param, "values", state_values);
    cJSON_AddItemToArray(state_params, state_param);
    
    cJSON_AddItemToObject(state_group, "parameters", state_params);
    cJSON_AddItemToArray(groups, state_group);

    // ===== DAC Configuration Group =====
    cJSON *dac_config_group = cJSON_CreateObject();
    cJSON_AddStringToObject(dac_config_group, "name", "DAC Configuration");
    cJSON_AddStringToObject(dac_config_group, "description", "DAC mode and modulation settings");
    
    cJSON *dac_config_params = cJSON_CreateArray();
    
    // DAC Mode parameter
    cJSON *dac_mode_param = cJSON_CreateObject();
    cJSON_AddStringToObject(dac_mode_param, "key", "dac_mode");
    cJSON_AddStringToObject(dac_mode_param, "name", "DAC Mode");
    cJSON_AddStringToObject(dac_mode_param, "type", "enum");
    cJSON_AddNumberToObject(dac_mode_param, "current", (int)dac_mode);
    
    cJSON *dac_mode_values = cJSON_CreateArray();
    
    cJSON *dac_mode_btl = cJSON_CreateObject();
    cJSON_AddNumberToObject(dac_mode_btl, "value", TAS5805M_DAC_MODE_BTL);
    cJSON_AddStringToObject(dac_mode_btl, "name", "BTL (Bridge Tied Load)");
    cJSON_AddItemToArray(dac_mode_values, dac_mode_btl);
    
    cJSON *dac_mode_pbtl = cJSON_CreateObject();
    cJSON_AddNumberToObject(dac_mode_pbtl, "value", TAS5805M_DAC_MODE_PBTL);
    cJSON_AddStringToObject(dac_mode_pbtl, "name", "PBTL (Parallel Load)");
    cJSON_AddItemToArray(dac_mode_values, dac_mode_pbtl);
    
    cJSON_AddItemToObject(dac_mode_param, "values", dac_mode_values);
    cJSON_AddItemToArray(dac_config_params, dac_mode_param);
    
    // Mixer Mode parameter
    cJSON *mixer_mode_param = cJSON_CreateObject();
    cJSON_AddStringToObject(mixer_mode_param, "key", "mixer_mode");
    cJSON_AddStringToObject(mixer_mode_param, "name", "Mixer Mode");
    cJSON_AddStringToObject(mixer_mode_param, "type", "enum");
    cJSON_AddNumberToObject(mixer_mode_param, "current", (int)dac_state.mixer_mode);

    cJSON *mixer_mode_values = cJSON_CreateArray();

    cJSON *mm_unknown = cJSON_CreateObject();
    cJSON_AddNumberToObject(mm_unknown, "value", MIXER_UNKNOWN);
    cJSON_AddStringToObject(mm_unknown, "name", "Unknown");
    cJSON_AddItemToArray(mixer_mode_values, mm_unknown);

    cJSON *mm_stereo = cJSON_CreateObject();
    cJSON_AddNumberToObject(mm_stereo, "value", MIXER_STEREO);
    cJSON_AddStringToObject(mm_stereo, "name", "Stereo");
    cJSON_AddItemToArray(mixer_mode_values, mm_stereo);

    cJSON *mm_stereo_inv = cJSON_CreateObject();
    cJSON_AddNumberToObject(mm_stereo_inv, "value", MIXER_STEREO_INVERSE);
    cJSON_AddStringToObject(mm_stereo_inv, "name", "Stereo (Inverse)");
    cJSON_AddItemToArray(mixer_mode_values, mm_stereo_inv);

    cJSON *mm_mono = cJSON_CreateObject();
    cJSON_AddNumberToObject(mm_mono, "value", MIXER_MONO);
    cJSON_AddStringToObject(mm_mono, "name", "Mono");
    cJSON_AddItemToArray(mixer_mode_values, mm_mono);

    cJSON *mm_right = cJSON_CreateObject();
    cJSON_AddNumberToObject(mm_right, "value", MIXER_RIGHT);
    cJSON_AddStringToObject(mm_right, "name", "Right");
    cJSON_AddItemToArray(mixer_mode_values, mm_right);

    cJSON *mm_left = cJSON_CreateObject();
    cJSON_AddNumberToObject(mm_left, "value", MIXER_LEFT);
    cJSON_AddStringToObject(mm_left, "name", "Left");
    cJSON_AddItemToArray(mixer_mode_values, mm_left);

    cJSON_AddItemToObject(mixer_mode_param, "values", mixer_mode_values);
    cJSON_AddItemToArray(dac_config_params, mixer_mode_param);

    // Modulation Mode parameter
    cJSON *mod_mode_param = cJSON_CreateObject();
    cJSON_AddStringToObject(mod_mode_param, "key", "modulation_mode");
    cJSON_AddStringToObject(mod_mode_param, "name", "Modulation Mode");
    cJSON_AddStringToObject(mod_mode_param, "type", "enum");
    cJSON_AddNumberToObject(mod_mode_param, "current", (int)mod_mode);
    
    cJSON *mod_mode_values = cJSON_CreateArray();
    
    cJSON *mod_bd = cJSON_CreateObject();
    cJSON_AddNumberToObject(mod_bd, "value", MOD_MODE_BD);
    cJSON_AddStringToObject(mod_bd, "name", "BD Mode");
    cJSON_AddItemToArray(mod_mode_values, mod_bd);
    
    cJSON *mod_1spw = cJSON_CreateObject();
    cJSON_AddNumberToObject(mod_1spw, "value", MOD_MODE_1SPW);
    cJSON_AddStringToObject(mod_1spw, "name", "1SPW Mode");
    cJSON_AddItemToArray(mod_mode_values, mod_1spw);
    
    cJSON *mod_hybrid = cJSON_CreateObject();
    cJSON_AddNumberToObject(mod_hybrid, "value", MOD_MODE_HYBRID);
    cJSON_AddStringToObject(mod_hybrid, "name", "Hybrid Mode");
    cJSON_AddItemToArray(mod_mode_values, mod_hybrid);
    
    cJSON_AddItemToObject(mod_mode_param, "values", mod_mode_values);
    cJSON_AddItemToArray(dac_config_params, mod_mode_param);
    
    // Switching Frequency parameter
    cJSON *sw_freq_param = cJSON_CreateObject();
    cJSON_AddStringToObject(sw_freq_param, "key", "sw_freq");
    cJSON_AddStringToObject(sw_freq_param, "name", "Switching Frequency");
    cJSON_AddStringToObject(sw_freq_param, "type", "enum");
    cJSON_AddNumberToObject(sw_freq_param, "current", (int)sw_freq);
    
    cJSON *sw_freq_values = cJSON_CreateArray();
    
    cJSON *freq_768k = cJSON_CreateObject();
    cJSON_AddNumberToObject(freq_768k, "value", SW_FREQ_768K);
    cJSON_AddStringToObject(freq_768k, "name", "768 kHz");
    cJSON_AddItemToArray(sw_freq_values, freq_768k);
    
    cJSON *freq_384k = cJSON_CreateObject();
    cJSON_AddNumberToObject(freq_384k, "value", SW_FREQ_384K);
    cJSON_AddStringToObject(freq_384k, "name", "384 kHz");
    cJSON_AddItemToArray(sw_freq_values, freq_384k);
    
    cJSON *freq_480k = cJSON_CreateObject();
    cJSON_AddNumberToObject(freq_480k, "value", SW_FREQ_480K);
    cJSON_AddStringToObject(freq_480k, "name", "480 kHz");
    cJSON_AddItemToArray(sw_freq_values, freq_480k);
    
    cJSON *freq_576k = cJSON_CreateObject();
    cJSON_AddNumberToObject(freq_576k, "value", SW_FREQ_576K);
    cJSON_AddStringToObject(freq_576k, "name", "576 kHz");
    cJSON_AddItemToArray(sw_freq_values, freq_576k);
    
    cJSON_AddItemToObject(sw_freq_param, "values", sw_freq_values);
    cJSON_AddItemToArray(dac_config_params, sw_freq_param);
    
    // BD Frequency parameter
    cJSON *bd_freq_param = cJSON_CreateObject();
    cJSON_AddStringToObject(bd_freq_param, "key", "bd_freq");
    cJSON_AddStringToObject(bd_freq_param, "name", "BD Frequency");
    cJSON_AddStringToObject(bd_freq_param, "type", "enum");
    cJSON_AddNumberToObject(bd_freq_param, "current", (int)bd_freq);
    
    cJSON *bd_freq_values = cJSON_CreateArray();
    
    cJSON *bd_80k = cJSON_CreateObject();
    cJSON_AddNumberToObject(bd_80k, "value", SW_FREQ_80K);
    cJSON_AddStringToObject(bd_80k, "name", "80 kHz");
    cJSON_AddItemToArray(bd_freq_values, bd_80k);
    
    cJSON *bd_100k = cJSON_CreateObject();
    cJSON_AddNumberToObject(bd_100k, "value", SW_FREQ_100K);
    cJSON_AddStringToObject(bd_100k, "name", "100 kHz");
    cJSON_AddItemToArray(bd_freq_values, bd_100k);
    
    cJSON *bd_120k = cJSON_CreateObject();
    cJSON_AddNumberToObject(bd_120k, "value", SW_FREQ_120K);
    cJSON_AddStringToObject(bd_120k, "name", "120 kHz");
    cJSON_AddItemToArray(bd_freq_values, bd_120k);
    
    cJSON *bd_175k = cJSON_CreateObject();
    cJSON_AddNumberToObject(bd_175k, "value", SW_FREQ_175K);
    cJSON_AddStringToObject(bd_175k, "name", "175 kHz");
    cJSON_AddItemToArray(bd_freq_values, bd_175k);
    
    cJSON_AddItemToObject(bd_freq_param, "values", bd_freq_values);
    cJSON_AddItemToArray(dac_config_params, bd_freq_param);

    cJSON_AddItemToObject(dac_config_group, "parameters", dac_config_params);
    cJSON_AddItemToArray(groups, dac_config_group);

    // ===== Channel Gain Group (always visible) =====
    {
        cJSON *ch_group = cJSON_CreateObject();
        cJSON_AddStringToObject(ch_group, "name", "Mixer Gain");
        cJSON_AddStringToObject(ch_group, "description", "Mixer gain");

        cJSON *ch_params = cJSON_CreateArray();

        int8_t cur_ch_l = 0, cur_ch_r = 0;
        if (tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &cur_ch_l) != ESP_OK) cur_ch_l = 0;
        if (tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &cur_ch_r) != ESP_OK) cur_ch_r = 0;

        cJSON *ch_l_param = cJSON_CreateObject();
        cJSON_AddStringToObject(ch_l_param, "key", TAS5805M_NVS_KEY_CHANNEL_GAIN_L);
        cJSON_AddStringToObject(ch_l_param, "name", "Channel Gain (L)");
        cJSON_AddStringToObject(ch_l_param, "type", "range");
        cJSON_AddStringToObject(ch_l_param, "unit", "dB");
        cJSON_AddNumberToObject(ch_l_param, "min", TAS5805M_MIXER_VALUE_MINDB);
        cJSON_AddNumberToObject(ch_l_param, "max", TAS5805M_MIXER_VALUE_MAXDB);
        cJSON_AddNumberToObject(ch_l_param, "step", 1);
        cJSON_AddNumberToObject(ch_l_param, "default", 0);
        cJSON_AddNumberToObject(ch_l_param, "current", (int)cur_ch_l);
        cJSON_AddItemToArray(ch_params, ch_l_param);

        cJSON *ch_r_param = cJSON_CreateObject();
        cJSON_AddStringToObject(ch_r_param, "key", TAS5805M_NVS_KEY_CHANNEL_GAIN_R);
        cJSON_AddStringToObject(ch_r_param, "name", "Channel Gain (R)");
        cJSON_AddStringToObject(ch_r_param, "type", "range");
        cJSON_AddStringToObject(ch_r_param, "unit", "dB");
        cJSON_AddNumberToObject(ch_r_param, "min", TAS5805M_MIXER_VALUE_MINDB);
        cJSON_AddNumberToObject(ch_r_param, "max", TAS5805M_MIXER_VALUE_MAXDB);
        cJSON_AddNumberToObject(ch_r_param, "step", 1);
        cJSON_AddNumberToObject(ch_r_param, "default", 0);
        cJSON_AddNumberToObject(ch_r_param, "current", (int)cur_ch_r);
        cJSON_AddItemToArray(ch_params, ch_r_param);

        cJSON_AddItemToObject(ch_group, "parameters", ch_params);
        cJSON_AddItemToArray(groups, ch_group);
    }

    // ===== EQ Group =====
    cJSON *eq_group = cJSON_CreateObject();
    cJSON_AddStringToObject(eq_group, "name", "EQ");
    cJSON_AddStringToObject(eq_group, "description", "Equalizer mode");

    cJSON *eq_params = cJSON_CreateArray();

    // Replace legacy EQ mode with a UI-focused EQ selection that controls which UI elements are shown
    cJSON *eq_ui_param = cJSON_CreateObject();
    cJSON_AddStringToObject(eq_ui_param, "key", "eq_ui_mode");
    cJSON_AddStringToObject(eq_ui_param, "name", "EQ Mode");
    cJSON_AddStringToObject(eq_ui_param, "type", "enum");

    // Determine current UI mode: prefer persisted UI selection, otherwise derive from driver eq_mode
    TAS5805M_EQ_UI_MODE cur_ui_mode = TAS5805M_EQ_UI_MODE_OFF;
    if (tas5805m_settings_load_eq_ui_mode(&cur_ui_mode) != ESP_OK) {
        // fallback: map driver eq_mode to a reasonable UI mode
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
        TAS5805M_EQ_MODE drv = TAS5805M_EQ_MODE_OFF;
        if (tas5805m_get_eq_mode(&drv) == ESP_OK) {
            if (drv == TAS5805M_EQ_MODE_OFF) cur_ui_mode = TAS5805M_EQ_UI_MODE_OFF;
            else if (drv == TAS5805M_EQ_MODE_ON) cur_ui_mode = TAS5805M_EQ_UI_MODE_15_BAND;
            else cur_ui_mode = TAS5805M_EQ_UI_MODE_15_BAND_BIAMP;
        }
#endif
    }

    cJSON_AddNumberToObject(eq_ui_param, "current", (int)cur_ui_mode);

    cJSON *eq_ui_values = cJSON_CreateArray();
    cJSON *v_off = cJSON_CreateObject();
    cJSON_AddNumberToObject(v_off, "value", (int)TAS5805M_EQ_UI_MODE_OFF);
    cJSON_AddStringToObject(v_off, "name", "OFF");
    cJSON_AddItemToArray(eq_ui_values, v_off);
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    cJSON *v_15 = cJSON_CreateObject();
    cJSON_AddNumberToObject(v_15, "value", (int)TAS5805M_EQ_UI_MODE_15_BAND);
    cJSON_AddStringToObject(v_15, "name", "15-band");
    cJSON_AddItemToArray(eq_ui_values, v_15);

    cJSON *v_15_bi = cJSON_CreateObject();
    cJSON_AddNumberToObject(v_15_bi, "value", (int)TAS5805M_EQ_UI_MODE_15_BAND_BIAMP);
    cJSON_AddStringToObject(v_15_bi, "name", "15-band (bi-amp)");
    cJSON_AddItemToArray(eq_ui_values, v_15_bi);

    cJSON *v_preset = cJSON_CreateObject();
    cJSON_AddNumberToObject(v_preset, "value", (int)TAS5805M_EQ_UI_MODE_PRESETS);
    cJSON_AddStringToObject(v_preset, "name", "EQ Presets");
    cJSON_AddItemToArray(eq_ui_values, v_preset);
#else
    /* When EQ support is disabled expose only OFF and mark the control readonly
     * so the UI shows the section but doesn't allow changing it.
     */
    cJSON_AddBoolToObject(eq_ui_param, "readonly", true);
#endif

    cJSON_AddItemToObject(eq_ui_param, "values", eq_ui_values);
    cJSON_AddItemToArray(eq_params, eq_ui_param);
    
    // EQ Preset / Profile per channel
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    {
        TAS5805M_EQ_PROFILE cur_prof_l = FLAT, cur_prof_r = FLAT;
        if (tas5805m_get_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, &cur_prof_l) != ESP_OK) cur_prof_l = FLAT;
        if (tas5805m_get_eq_profile_channel(TAS5805M_EQ_CHANNELS_RIGHT, &cur_prof_r) != ESP_OK) cur_prof_r = FLAT;

        cJSON *prof_l_param = cJSON_CreateObject();
        cJSON_AddStringToObject(prof_l_param, "key", "eq_profile_l");
        cJSON_AddStringToObject(prof_l_param, "name", "EQ Preset (L)");
        cJSON_AddStringToObject(prof_l_param, "type", "enum");
        cJSON_AddNumberToObject(prof_l_param, "current", (int)cur_prof_l);

        cJSON *prof_l_values = cJSON_CreateArray();
        cJSON *v;
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", FLAT); cJSON_AddStringToObject(v, "name", "Flat"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_60HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 60 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_70HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 70 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_80HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 80 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_90HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 90 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_100HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 100 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_110HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 110 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_120HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 120 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_130HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 130 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_140HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 140 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_150HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 150 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_60HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 60 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_70HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 70 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_80HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 80 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_90HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 90 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_100HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 100 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_110HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 110 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_120HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 120 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_130HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 130 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_140HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 140 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_150HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 150 Hz Cutoff"); cJSON_AddItemToArray(prof_l_values, v);

        cJSON_AddItemToObject(prof_l_param, "values", prof_l_values);
        cJSON_AddItemToArray(eq_params, prof_l_param);

        // Right channel
        cJSON *prof_r_param = cJSON_CreateObject();
        cJSON_AddStringToObject(prof_r_param, "key", "eq_profile_r");
        cJSON_AddStringToObject(prof_r_param, "name", "EQ Preset (R)");
        cJSON_AddStringToObject(prof_r_param, "type", "enum");
        cJSON_AddNumberToObject(prof_r_param, "current", (int)cur_prof_r);

        // reuse same values array content for right channel
        cJSON *prof_r_values = cJSON_CreateArray();
        // clone by creating new objects (same entries)
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", FLAT); cJSON_AddStringToObject(v, "name", "Flat"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_60HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 60 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_70HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 70 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_80HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 80 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_90HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 90 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_100HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 100 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_110HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 110 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_120HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 120 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_130HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 130 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_140HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 140 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", LF_150HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "LF 150 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_60HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 60 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_70HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 70 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_80HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 80 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_90HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 90 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_100HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 100 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_110HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 110 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_120HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 120 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_130HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 130 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_140HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 140 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", HF_150HZ_CUTOFF); cJSON_AddStringToObject(v, "name", "HF 150 Hz Cutoff"); cJSON_AddItemToArray(prof_r_values, v);

        cJSON_AddItemToObject(prof_r_param, "values", prof_r_values);
        cJSON_AddItemToArray(eq_params, prof_r_param);

    
    }
#else
    // If EQ support disabled, provide readonly placeholders for presets
    cJSON *prof_l_param = cJSON_CreateObject();
    cJSON_AddStringToObject(prof_l_param, "key", "eq_profile_l");
    cJSON_AddStringToObject(prof_l_param, "name", "EQ Preset (L)");
    cJSON_AddStringToObject(prof_l_param, "type", "enum");
    cJSON_AddNumberToObject(prof_l_param, "current", (int)FLAT);
    cJSON *prof_l_vals = cJSON_CreateArray();
    cJSON *pv = cJSON_CreateObject(); cJSON_AddNumberToObject(pv, "value", FLAT); cJSON_AddStringToObject(pv, "name", "Flat"); cJSON_AddItemToArray(prof_l_vals, pv);
    cJSON_AddItemToObject(prof_l_param, "values", prof_l_vals);
    cJSON_AddItemToArray(eq_params, prof_l_param);

    cJSON *prof_r_param = cJSON_CreateObject();
    cJSON_AddStringToObject(prof_r_param, "key", "eq_profile_r");
    cJSON_AddStringToObject(prof_r_param, "name", "EQ Preset (R)");
    cJSON_AddStringToObject(prof_r_param, "type", "enum");
    cJSON_AddNumberToObject(prof_r_param, "current", (int)FLAT);
    cJSON *prof_r_vals = cJSON_CreateArray();
    pv = cJSON_CreateObject(); cJSON_AddNumberToObject(pv, "value", FLAT); cJSON_AddStringToObject(pv, "name", "Flat"); cJSON_AddItemToArray(prof_r_vals, pv);
    cJSON_AddItemToObject(prof_r_param, "values", prof_r_vals);
    cJSON_AddItemToArray(eq_params, prof_r_param);
#endif
    cJSON_AddItemToObject(eq_group, "parameters", eq_params);
    /* Add per-band sliders for left and right channels (if EQ supported) */
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
        int cur_l = 0, cur_r = 0;
        // Try to read current value from driver; if unavailable, fall back to persisted NVS value
        if (tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, &cur_l) != ESP_OK) {
            if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &cur_l) != ESP_OK) {
                cur_l = 0;
            }
        }
        if (tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, &cur_r) != ESP_OK) {
            if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &cur_r) != ESP_OK) {
                cur_r = 0;
            }
        }

        char key_l[32];
        char key_r[32];
        snprintf(key_l, sizeof(key_l), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_L_PREFIX, band);
        snprintf(key_r, sizeof(key_r), "%s%d", TAS5805M_NVS_KEY_EQ_GAIN_R_PREFIX, band);

        // Frequency label for this band (Hz) - use tas5805m_eq_bands
        char freq_label[32] = {0};
        snprintf(freq_label, sizeof(freq_label), "%d Hz", tas5805m_eq_bands[band]);

        cJSON *param_l = cJSON_CreateObject();
        cJSON_AddStringToObject(param_l, "key", key_l);
    // Name uses frequency and channel: "<freq> (L)"
    char name_l[48];
    snprintf(name_l, sizeof(name_l), "%s (L)", freq_label);
    cJSON_AddStringToObject(param_l, "name", name_l);
        cJSON_AddStringToObject(param_l, "type", "range");
        cJSON_AddStringToObject(param_l, "unit", "dB");
        cJSON_AddStringToObject(param_l, "label", freq_label);
        cJSON_AddNumberToObject(param_l, "min", TAS5805M_EQ_MIN_DB);
        cJSON_AddNumberToObject(param_l, "max", TAS5805M_EQ_MAX_DB);
        cJSON_AddNumberToObject(param_l, "step", 1);
        cJSON_AddNumberToObject(param_l, "default", 0);
        cJSON_AddNumberToObject(param_l, "current", cur_l);
        cJSON_AddItemToArray(eq_params, param_l);

        cJSON *param_r = cJSON_CreateObject();
        cJSON_AddStringToObject(param_r, "key", key_r);
    char name_r[48];
    snprintf(name_r, sizeof(name_r), "%s (R)", freq_label);
    cJSON_AddStringToObject(param_r, "name", name_r);
        cJSON_AddStringToObject(param_r, "type", "range");
        cJSON_AddStringToObject(param_r, "unit", "dB");
        cJSON_AddStringToObject(param_r, "label", freq_label);
        cJSON_AddNumberToObject(param_r, "min", TAS5805M_EQ_MIN_DB);
        cJSON_AddNumberToObject(param_r, "max", TAS5805M_EQ_MAX_DB);
        cJSON_AddNumberToObject(param_r, "step", 1);
        cJSON_AddNumberToObject(param_r, "default", 0);
        cJSON_AddNumberToObject(param_r, "current", cur_r);
        cJSON_AddItemToArray(eq_params, param_r);
    }
#endif
    cJSON_AddItemToArray(groups, eq_group);
    
    // End groups
    cJSON_AddItemToObject(root, "groups", groups);

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "%s: Generated schema JSON size: %zu bytes (buffer size: %zu)", __func__, json_len, max_len);

    if (json_len >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer (%zu >= %zu)", __func__, json_len, max_len);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGD(TAG, "%s: Schema JSON generated: %s", __func__, json_out);
    return ESP_OK;
}

/* Apply early settings that are safe to write before audio playback starts.
 * This includes: analog gain, DAC mode, modulation mode, mixer mode.
 */
esp_err_t tas5805m_settings_apply_early(void) {
    ESP_LOGI(TAG, "%s: Applying early TAS5805M settings from NVS", __func__);
    // NOTE: don't call tas5805m_settings_init() here to avoid recursion; caller
    // should ensure init() has been called.

    // Apply analog gain (raw register index)
    int ana_gain = 0;
    if (tas5805m_settings_load_analog_gain(&ana_gain) == ESP_OK) {
        uint8_t gain = (uint8_t)ana_gain;
        ESP_LOGI(TAG, "%s: Restoring analog gain raw=%d", __func__, gain);
        if (tas5805m_set_again(gain) != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to apply saved analog gain", __func__);
        }
    }

    // Apply DAC mode
    TAS5805M_DAC_MODE dac_mode;
    if (tas5805m_settings_load_dac_mode(&dac_mode) == ESP_OK) {
        ESP_LOGI(TAG, "%s: Restoring DAC mode=%d", __func__, (int)dac_mode);
        if (tas5805m_set_dac_mode(dac_mode) != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to apply saved DAC mode", __func__);
        }
    }

    // Apply modulation mode (mode, sw_freq, bd_freq)
    TAS5805M_MOD_MODE mod_mode;
    TAS5805M_SW_FREQ sw_freq;
    TAS5805M_BD_FREQ bd_freq;
    if (tas5805m_settings_load_modulation_mode(&mod_mode, &sw_freq, &bd_freq) == ESP_OK) {
        ESP_LOGI(TAG, "%s: Restoring modulation mode=%d, sw=%d, bd=%d", __func__, (int)mod_mode, (int)sw_freq, (int)bd_freq);
        if (tas5805m_set_modulation_mode(mod_mode, sw_freq, bd_freq) != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to apply saved modulation mode", __func__);
        }
    }
    
    ESP_LOGI(TAG, "%s: Early persisted settings application complete", __func__);
    return ESP_OK;
}

/* Apply settings that require the codec to be running (delayed restore).
* This restores EQ mode, per-band gains, profiles and channel gains.
*/
esp_err_t tas5805m_settings_apply_delayed(void) {
    ESP_LOGI(TAG, "%s: Applying delayed TAS5805M settings from NVS", __func__);
    
    // Apply mixer mode
    TAS5805M_MIXER_MODE mixer_mode;
    if (tas5805m_settings_load_mixer_mode(&mixer_mode) == ESP_OK) {
        ESP_LOGI(TAG, "%s: Restoring mixer mode=%d", __func__, (int)mixer_mode);
        if (tas5805m_set_mixer_mode(mixer_mode) != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to apply saved mixer mode", __func__);
        }
    }

    TAS5805M_EQ_MODE eq_mode = TAS5805M_EQ_MODE_OFF;
    if (tas5805m_settings_load_eq_mode(&eq_mode) == ESP_OK) {
        ESP_LOGI(TAG, "%s: Restoring EQ mode=%d", __func__, (int)eq_mode);
    #if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
        if (tas5805m_set_eq_mode(eq_mode) != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to apply saved EQ mode", __func__);
        }
    #else
        ESP_LOGW(TAG, "%s: EQ support disabled in build; ignoring persisted EQ mode", __func__);
    #endif
    }

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    // Restore EQ based on persisted UI selection (eq_ui_mode). If no UI selection
    // is persisted, derive a reasonable default from the saved driver EQ mode.
    TAS5805M_EQ_UI_MODE ui_mode = TAS5805M_EQ_UI_MODE_OFF;
    if (tas5805m_settings_load_eq_ui_mode(&ui_mode) != ESP_OK) {
        // derive from saved eq_mode
        if (eq_mode == TAS5805M_EQ_MODE_OFF) ui_mode = TAS5805M_EQ_UI_MODE_OFF;
        else if (eq_mode == TAS5805M_EQ_MODE_ON) ui_mode = TAS5805M_EQ_UI_MODE_15_BAND;
        else ui_mode = TAS5805M_EQ_UI_MODE_15_BAND_BIAMP;
    }

    ESP_LOGI(TAG, "%s: Restoring EQ using UI mode %d (%s)", __func__, (int)ui_mode, tas5805m_eq_ui_mode_to_string(ui_mode));

    if (ui_mode == TAS5805M_EQ_UI_MODE_OFF) {
        // nothing to restore
    } else if (ui_mode == TAS5805M_EQ_UI_MODE_15_BAND) {
        // Apply left-channel per-band gains
        for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
            int gain = 0;
            if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                if (tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain) != ESP_OK) {
                    ESP_LOGW(TAG, "%s: Failed to apply saved EQ gain L band %d", __func__, band);
                } else {
                    ESP_LOGI(TAG, "%s: Restored EQ gain L band %d = %d", __func__, band, gain);
                }
            }
        }
    } else if (ui_mode == TAS5805M_EQ_UI_MODE_15_BAND_BIAMP) {
        // Apply both channels per-band gains
        for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
            int gain = 0;
            if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                if (tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain) != ESP_OK) {
                    ESP_LOGW(TAG, "%s: Failed to apply saved EQ gain L band %d", __func__, band);
                } else {
                    ESP_LOGI(TAG, "%s: Restored EQ gain L band %d = %d", __func__, band, gain);
                }
            }
            if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain) == ESP_OK) {
                if (tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, gain) != ESP_OK) {
                    ESP_LOGW(TAG, "%s: Failed to apply saved EQ gain R band %d", __func__, band);
                } else {
                    ESP_LOGI(TAG, "%s: Restored EQ gain R band %d = %d", __func__, band, gain);
                }
            }
        }
    } else if (ui_mode == TAS5805M_EQ_UI_MODE_PRESETS) {
        // Apply persisted EQ profiles for both channels
        TAS5805M_EQ_PROFILE prof = FLAT;
        if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_LEFT, &prof) == ESP_OK) {
            if (tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, prof) != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to apply saved EQ profile L", __func__);
            } else {
                ESP_LOGI(TAG, "%s: Restored EQ profile L = %d", __func__, (int)prof);
            }
        }
        if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_RIGHT, &prof) == ESP_OK) {
            if (tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_RIGHT, prof) != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to apply saved EQ profile R", __func__);
            } else {
                ESP_LOGI(TAG, "%s: Restored EQ profile R = %d", __func__, (int)prof);
            }
        }
        /* Restore per-output channel gain values (if any) */
        int ch_gain = 0;
        if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &ch_gain) == ESP_OK) {
            if (tas5805m_set_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, (int8_t)ch_gain) != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to apply saved Channel Gain L", __func__);
            } else {
                ESP_LOGI(TAG, "%s: Restored Channel Gain L = %d dB", __func__, ch_gain);
            }
        }
        if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &ch_gain) == ESP_OK) {
            if (tas5805m_set_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, (int8_t)ch_gain) != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to apply saved Channel Gain R", __func__);
            } else {
                ESP_LOGI(TAG, "%s: Restored Channel Gain R = %d dB", __func__, ch_gain);
            }
        }
    }
#endif

    ESP_LOGI(TAG, "%s: Delayed persisted settings application complete", __func__);
    return ESP_OK;
}

#endif /* CONFIG_DAC_TAS5805M */

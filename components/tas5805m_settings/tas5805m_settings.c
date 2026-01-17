/**
 * @file tas5805m_settings.c
 * @brief TAS5805M DAC settings persistence and JSON serialization implementation
 */

#include "tas5805m_settings.h"
#include "tas5805m_biamp.h"

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
// Whether I2S clock is available (tas5805m_settings_apply_delayed has been called)
static bool tas5805m_i2s_clock_ready = false;

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
 * Create JSON array with all fault status indicators
 */
static cJSON* tas5805m_create_faults_array(TAS5805M_FAULT fault) {
    cJSON *faults = cJSON_CreateArray();
    if (!faults) {
        return NULL;
    }

    // err0 faults
    cJSON *fault_obj;
    
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "Right channel over current");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err0 & (1 << 0)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "Left channel over current");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err0 & (1 << 1)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "Right channel DC fault");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err0 & (1 << 2)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "Left channel DC fault");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err0 & (1 << 3)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    // err1 faults
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "PVDD undervoltage");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err1 & (1 << 0)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "PVDD overvoltage");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err1 & (1 << 1)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "Clock fault");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err1 & (1 << 2)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "BQ write failed");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err1 & (1 << 6)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "OTP CRC error");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err1 & (1 << 7)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    // err2 faults
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "Over temperature shutdown");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.err2 & (1 << 0)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    // ot_warn
    fault_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(fault_obj, "name", "Over temperature warning");
    cJSON_AddBoolToObject(fault_obj, "active", (fault.ot_warn & (1 << 2)) != 0);
    cJSON_AddItemToArray(faults, fault_obj);
    
    return faults;
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
        case TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP: return "Advanced Bi-Amp";
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

/* ============ Advanced Bi-Amp Settings NVS Functions ============ */

/** Save advanced bi-amp settings to NVS */
esp_err_t tas5805m_settings_save_biamp(const tas5805m_biamp_settings_t *settings) {
    ESP_LOGD(TAG, "%s", __func__);

    if (!settings) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        /* Crossover settings */
        err = nvs_set_u16(h, TAS5805M_NVS_KEY_BIAMP_XOVER_FREQ, settings->crossover_freq);
        if (err == ESP_OK) err = nvs_set_u8(h, TAS5805M_NVS_KEY_BIAMP_SLOPE, (uint8_t)settings->slope);
        if (err == ESP_OK) err = nvs_set_u8(h, TAS5805M_NVS_KEY_BIAMP_TYPE, (uint8_t)settings->type);
        if (err == ESP_OK) err = nvs_set_u32(h, TAS5805M_NVS_KEY_BIAMP_SAMPLE_RATE, settings->sample_rate);

        /* Speaker protection */
        if (err == ESP_OK) err = nvs_set_u16(h, TAS5805M_NVS_KEY_BIAMP_SUBSONIC_FREQ, settings->subsonic_freq);

        /* Low output settings */
        if (err == ESP_OK) err = nvs_set_i8(h, TAS5805M_NVS_KEY_BIAMP_LOW_GAIN, settings->low_gain);
        if (err == ESP_OK) err = nvs_set_u8(h, TAS5805M_NVS_KEY_BIAMP_LOW_PHASE, settings->low_phase_invert);

        /* High output settings */
        if (err == ESP_OK) err = nvs_set_i8(h, TAS5805M_NVS_KEY_BIAMP_HIGH_GAIN, settings->high_gain);
        if (err == ESP_OK) err = nvs_set_u8(h, TAS5805M_NVS_KEY_BIAMP_HIGH_PHASE, settings->high_phase_invert);

        /* Per-output PEQ bands */
        for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS && err == ESP_OK; i++) {
            char key[20];
            snprintf(key, sizeof(key), "%s%d_f", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
            err = nvs_set_u16(h, key, settings->low_peq[i].freq);
            if (err == ESP_OK) {
                snprintf(key, sizeof(key), "%s%d_g", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
                err = nvs_set_i8(h, key, settings->low_peq[i].gain);
            }
            if (err == ESP_OK) {
                snprintf(key, sizeof(key), "%s%d_q", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
                err = nvs_set_u8(h, key, settings->low_peq[i].q_x10);
            }

            if (err == ESP_OK) {
                snprintf(key, sizeof(key), "%s%d_f", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
                err = nvs_set_u16(h, key, settings->high_peq[i].freq);
            }
            if (err == ESP_OK) {
                snprintf(key, sizeof(key), "%s%d_g", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
                err = nvs_set_i8(h, key, settings->high_peq[i].gain);
            }
            if (err == ESP_OK) {
                snprintf(key, sizeof(key), "%s%d_q", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
                err = nvs_set_u8(h, key, settings->high_peq[i].q_x10);
            }
        }

        /* Baffle step compensation */
        if (err == ESP_OK) err = nvs_set_u8(h, TAS5805M_NVS_KEY_BAFFLE_WIDTH, settings->baffle_width_cm);
        if (err == ESP_OK) err = nvs_set_u8(h, TAS5805M_NVS_KEY_BAFFLE_PLACEMENT, (uint8_t)settings->baffle_placement);

        /* Time alignment (tweeter delay) */
        if (err == ESP_OK) err = nvs_set_u8(h, TAS5805M_NVS_KEY_TWEETER_DELAY, settings->tweeter_delay_mm);

        /* Tweeter breakup notch */
        if (err == ESP_OK) err = nvs_set_u16(h, TAS5805M_NVS_KEY_NOTCH_FREQ, settings->notch_freq);
        if (err == ESP_OK) err = nvs_set_i8(h, TAS5805M_NVS_KEY_NOTCH_GAIN, settings->notch_gain);
        if (err == ESP_OK) err = nvs_set_u8(h, TAS5805M_NVS_KEY_NOTCH_Q, settings->notch_q_x10);

        /* Air/Brilliance shelf */
        if (err == ESP_OK) err = nvs_set_i8(h, TAS5805M_NVS_KEY_AIR_GAIN, settings->air_gain);

        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Saved bi-amp settings: xover=%dHz slope=%d sr=%lu notch=%dHz air=%.1fdB",
                     __func__, settings->crossover_freq, settings->slope,
                     (unsigned long)settings->sample_rate, settings->notch_freq, settings->air_gain / 2.0);
        }
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Load advanced bi-amp settings from NVS */
esp_err_t tas5805m_settings_load_biamp(tas5805m_biamp_settings_t *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    /* Initialize to defaults first */
    tas5805m_biamp_init_defaults(settings);

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint16_t u16val;
        uint8_t u8val;
        int8_t i8val;

        /* Crossover settings */
        if (nvs_get_u16(h, TAS5805M_NVS_KEY_BIAMP_XOVER_FREQ, &u16val) == ESP_OK) {
            settings->crossover_freq = u16val;
        }
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_BIAMP_SLOPE, &u8val) == ESP_OK) {
            settings->slope = (tas5805m_biamp_slope_t)u8val;
        }
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_BIAMP_TYPE, &u8val) == ESP_OK) {
            settings->type = (tas5805m_biamp_type_t)u8val;
        }
        uint32_t u32val;
        if (nvs_get_u32(h, TAS5805M_NVS_KEY_BIAMP_SAMPLE_RATE, &u32val) == ESP_OK) {
            settings->sample_rate = u32val;
        }

        /* Speaker protection */
        if (nvs_get_u16(h, TAS5805M_NVS_KEY_BIAMP_SUBSONIC_FREQ, &u16val) == ESP_OK) {
            settings->subsonic_freq = u16val;
        }

        /* Low output settings */
        if (nvs_get_i8(h, TAS5805M_NVS_KEY_BIAMP_LOW_GAIN, &i8val) == ESP_OK) {
            settings->low_gain = i8val;
        }
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_BIAMP_LOW_PHASE, &u8val) == ESP_OK) {
            settings->low_phase_invert = u8val;
        }

        /* High output settings */
        if (nvs_get_i8(h, TAS5805M_NVS_KEY_BIAMP_HIGH_GAIN, &i8val) == ESP_OK) {
            settings->high_gain = i8val;
        }
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_BIAMP_HIGH_PHASE, &u8val) == ESP_OK) {
            settings->high_phase_invert = u8val;
        }

        /* Per-output PEQ bands */
        for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS; i++) {
            char key[20];
            snprintf(key, sizeof(key), "%s%d_f", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
            if (nvs_get_u16(h, key, &u16val) == ESP_OK) settings->low_peq[i].freq = u16val;
            snprintf(key, sizeof(key), "%s%d_g", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
            if (nvs_get_i8(h, key, &i8val) == ESP_OK) settings->low_peq[i].gain = i8val;
            snprintf(key, sizeof(key), "%s%d_q", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
            if (nvs_get_u8(h, key, &u8val) == ESP_OK) settings->low_peq[i].q_x10 = u8val;

            snprintf(key, sizeof(key), "%s%d_f", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
            if (nvs_get_u16(h, key, &u16val) == ESP_OK) settings->high_peq[i].freq = u16val;
            snprintf(key, sizeof(key), "%s%d_g", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
            if (nvs_get_i8(h, key, &i8val) == ESP_OK) settings->high_peq[i].gain = i8val;
            snprintf(key, sizeof(key), "%s%d_q", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
            if (nvs_get_u8(h, key, &u8val) == ESP_OK) settings->high_peq[i].q_x10 = u8val;
        }

        /* Baffle step compensation */
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_BAFFLE_WIDTH, &u8val) == ESP_OK) {
            settings->baffle_width_cm = u8val;
        }
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_BAFFLE_PLACEMENT, &u8val) == ESP_OK) {
            settings->baffle_placement = (tas5805m_baffle_placement_t)u8val;
        }

        /* Time alignment (tweeter delay) */
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_TWEETER_DELAY, &u8val) == ESP_OK) {
            settings->tweeter_delay_mm = u8val;
        }

        /* Tweeter breakup notch */
        if (nvs_get_u16(h, TAS5805M_NVS_KEY_NOTCH_FREQ, &u16val) == ESP_OK) {
            settings->notch_freq = u16val;
        }
        if (nvs_get_i8(h, TAS5805M_NVS_KEY_NOTCH_GAIN, &i8val) == ESP_OK) {
            settings->notch_gain = i8val;
        }
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_NOTCH_Q, &u8val) == ESP_OK) {
            settings->notch_q_x10 = u8val;
        }

        /* Air/Brilliance shelf */
        if (nvs_get_i8(h, TAS5805M_NVS_KEY_AIR_GAIN, &i8val) == ESP_OK) {
            settings->air_gain = i8val;
        }

        nvs_close(h);
        ESP_LOGD(TAG, "%s: Loaded bi-amp settings: xover=%dHz slope=%d sr=%lu notch=%dHz air=%.1fdB",
                 __func__, settings->crossover_freq, settings->slope,
                 (unsigned long)settings->sample_rate, settings->notch_freq, settings->air_gain / 2.0);
        err = ESP_OK;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "%s: No bi-amp settings in NVS, using defaults", __func__);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "%s: Failed to open NVS namespace '%s': %s", __func__, TAS5805M_NVS_NAMESPACE, esp_err_to_name(err));
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Apply advanced bi-amp settings to the DAC */
esp_err_t tas5805m_settings_apply_biamp(const tas5805m_biamp_settings_t *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "%s: Applying bi-amp settings", __func__);
    return tas5805m_biamp_apply(settings);
}

/* ============ Loudness Compensation Functions ============ */

/** Cached loudness settings (loaded at init) */
static tas5805m_loudness_settings_t s_loudness_settings;
static int s_current_volume = 50;  /* Track current volume for zone detection */

/** Initialize loudness settings to defaults */
void tas5805m_loudness_init_defaults(tas5805m_loudness_settings_t *settings) {
    if (!settings) return;

    settings->enabled = 0;  /* Disabled by default */

    /* Default thresholds: 20%, 40%, 60%, 80% */
    settings->thresholds[0] = 20;
    settings->thresholds[1] = 40;
    settings->thresholds[2] = 60;
    settings->thresholds[3] = 80;

    /* Default bass boost: more at lower volumes */
    settings->bass_boost[0] = 6;   /* Zone 0: 0-20% volume */
    settings->bass_boost[1] = 4;   /* Zone 1: 20-40% */
    settings->bass_boost[2] = 2;   /* Zone 2: 40-60% */
    settings->bass_boost[3] = 1;   /* Zone 3: 60-80% */
    settings->bass_boost[4] = 0;   /* Zone 4: 80-100% */

    /* Default treble boost */
    settings->treble_boost[0] = 4;
    settings->treble_boost[1] = 3;
    settings->treble_boost[2] = 2;
    settings->treble_boost[3] = 1;
    settings->treble_boost[4] = 0;
}

/** Save loudness settings to NVS */
esp_err_t tas5805m_settings_save_loudness(const tas5805m_loudness_settings_t *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, TAS5805M_NVS_KEY_LOUDNESS_ENABLED, settings->enabled);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, TAS5805M_NVS_KEY_LOUDNESS_THRESH,
                               settings->thresholds, sizeof(settings->thresholds));
        }
        if (err == ESP_OK) {
            err = nvs_set_blob(h, TAS5805M_NVS_KEY_LOUDNESS_BASS,
                               settings->bass_boost, sizeof(settings->bass_boost));
        }
        if (err == ESP_OK) {
            err = nvs_set_blob(h, TAS5805M_NVS_KEY_LOUDNESS_TREBLE,
                               settings->treble_boost, sizeof(settings->treble_boost));
        }
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);

        if (err == ESP_OK) {
            /* Update cached settings */
            memcpy(&s_loudness_settings, settings, sizeof(s_loudness_settings));
            ESP_LOGI(TAG, "%s: Saved loudness settings (enabled=%d)", __func__, settings->enabled);
        }
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Load loudness settings from NVS */
esp_err_t tas5805m_settings_load_loudness(tas5805m_loudness_settings_t *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    /* Initialize to defaults first */
    tas5805m_loudness_init_defaults(settings);

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t enabled = 0;
        if (nvs_get_u8(h, TAS5805M_NVS_KEY_LOUDNESS_ENABLED, &enabled) == ESP_OK) {
            settings->enabled = enabled;
        }

        size_t len = sizeof(settings->thresholds);
        nvs_get_blob(h, TAS5805M_NVS_KEY_LOUDNESS_THRESH, settings->thresholds, &len);

        len = sizeof(settings->bass_boost);
        nvs_get_blob(h, TAS5805M_NVS_KEY_LOUDNESS_BASS, settings->bass_boost, &len);

        len = sizeof(settings->treble_boost);
        nvs_get_blob(h, TAS5805M_NVS_KEY_LOUDNESS_TREBLE, settings->treble_boost, &len);

        nvs_close(h);
        ESP_LOGD(TAG, "%s: Loaded loudness settings (enabled=%d)", __func__, settings->enabled);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGD(TAG, "%s: No loudness settings in NVS, using defaults", __func__);
        err = ESP_OK;
    }

    /* Update cached settings */
    memcpy(&s_loudness_settings, settings, sizeof(s_loudness_settings));

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

/** Get current loudness zone (0-4) for a given volume (0-100) */
int tas5805m_loudness_get_zone(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    for (int i = 0; i < TAS5805M_LOUDNESS_ZONES - 1; i++) {
        if (volume < s_loudness_settings.thresholds[i]) {
            return i;
        }
    }
    return TAS5805M_LOUDNESS_ZONES - 1;  /* Highest zone */
}

/** Apply loudness compensation based on current volume */
esp_err_t tas5805m_loudness_apply(int volume) {
#if CONFIG_DAC_TAS5805M_EQ_SUPPORT
    /* Get current sample rate from bi-amp settings or use default */
    float fs = 48000.0f;  /* Default sample rate */
    tas5805m_biamp_settings_t biamp;
    if (tas5805m_settings_load_biamp(&biamp) == ESP_OK && biamp.sample_rate > 0) {
        fs = (float)biamp.sample_rate;
    }

    tas5805m_biquad_coeffs_t coeffs;
    esp_err_t ret;

    if (!s_loudness_settings.enabled) {
        /* Loudness disabled: reset bands 13 and 14 to passthrough (0dB, flat response) */
        ESP_LOGI(TAG, "%s: Loudness disabled, resetting bands to passthrough", __func__);

        /* Reset bass band (13) to passthrough on LEFT channel */
        ret = tas5805m_calc_low_shelf(200.0f, 0.0f, fs, &coeffs);
        if (ret == ESP_OK) {
            ret = tas5805m_write_biquad_coefficients(TAS5805M_EQ_CHANNELS_LEFT,
                                                      TAS5805M_LOUDNESS_BASS_BAND,
                                                      coeffs.b0, coeffs.b1, coeffs.b2,
                                                      coeffs.a1, coeffs.a2);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to reset bass band to passthrough", __func__);
        }

        /* Reset treble band (14) to passthrough on RIGHT channel */
        ret = tas5805m_calc_high_shelf(4000.0f, 0.0f, fs, &coeffs);
        if (ret == ESP_OK) {
            ret = tas5805m_write_biquad_coefficients(TAS5805M_EQ_CHANNELS_RIGHT,
                                                      TAS5805M_LOUDNESS_TREBLE_BAND,
                                                      coeffs.b0, coeffs.b1, coeffs.b2,
                                                      coeffs.a1, coeffs.a2);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to reset treble band to passthrough", __func__);
        }

        return ESP_OK;
    }

    /* Loudness enabled: apply boost based on volume zone */
    s_current_volume = volume;
    int zone = tas5805m_loudness_get_zone(volume);
    int8_t bass_db = s_loudness_settings.bass_boost[zone];
    int8_t treble_db = s_loudness_settings.treble_boost[zone];

    ESP_LOGI(TAG, "%s: Volume=%d%% Zone=%d Bass=%+ddB Treble=%+ddB",
             __func__, volume, zone, bass_db, treble_db);

    /* Apply low shelf for bass boost (200 Hz) - LEFT channel only (woofer in bi-amp) */
    ret = tas5805m_calc_low_shelf(200.0f, (float)bass_db, fs, &coeffs);
    if (ret == ESP_OK) {
        ret = tas5805m_write_biquad_coefficients(TAS5805M_EQ_CHANNELS_LEFT,
                                                  TAS5805M_LOUDNESS_BASS_BAND,
                                                  coeffs.b0, coeffs.b1, coeffs.b2,
                                                  coeffs.a1, coeffs.a2);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: Failed to apply bass shelf to woofer", __func__);
    }

    /* Apply high shelf for treble boost (4000 Hz) - RIGHT channel only (tweeter in bi-amp) */
    ret = tas5805m_calc_high_shelf(4000.0f, (float)treble_db, fs, &coeffs);
    if (ret == ESP_OK) {
        ret = tas5805m_write_biquad_coefficients(TAS5805M_EQ_CHANNELS_RIGHT,
                                                  TAS5805M_LOUDNESS_TREBLE_BAND,
                                                  coeffs.b0, coeffs.b1, coeffs.b2,
                                                  coeffs.a1, coeffs.a2);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: Failed to apply treble shelf to tweeter", __func__);
    }

    return ESP_OK;
#else
    /* EQ support not enabled - loudness compensation unavailable */
    (void)volume;
    ESP_LOGD(TAG, "%s: EQ support disabled, loudness compensation not available", __func__);
    return ESP_OK;
#endif /* CONFIG_DAC_TAS5805M_EQ_SUPPORT */
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

    /* Get current modulation mode and frequencies */
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
            case TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP: drv = TAS5805M_EQ_MODE_BIAMP; break;
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
    /* eq_params is referenced from both EQ-enabled and EQ-disabled branches
     * ensure the variable is declared in all compilation configurations. */
    cJSON *eq_params = NULL;
    
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
        /* Preset selectors are placed into a dedicated group so the UI can
         * show/hide the entire presets section when the UI mode is set to
         * 'Presets'. We'll add them to the presets group later. */

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
    
    /* Add per-band sliders for left and right channels (if EQ supported) */
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    // Create sub-groups for left and right channel EQ bands
    cJSON *eq_bands_left = cJSON_CreateObject();
    cJSON_AddStringToObject(eq_bands_left, "name", "Left channel");
    cJSON_AddStringToObject(eq_bands_left, "layout", "eq-bands");
    cJSON_AddStringToObject(eq_bands_left, "channel", "left");
    cJSON *eq_bands_left_params = cJSON_CreateArray();

    cJSON *eq_bands_right = cJSON_CreateObject();
    cJSON_AddStringToObject(eq_bands_right, "name", "Right Channel");
    cJSON_AddStringToObject(eq_bands_right, "layout", "eq-bands");
    cJSON_AddStringToObject(eq_bands_right, "channel", "right");
    cJSON *eq_bands_right_params = cJSON_CreateArray();

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
        cJSON_AddStringToObject(param_l, "name", freq_label);
        cJSON_AddStringToObject(param_l, "type", "range");
        cJSON_AddStringToObject(param_l, "unit", "dB");
        cJSON_AddStringToObject(param_l, "label", freq_label);
        cJSON_AddStringToObject(param_l, "layout", "vertical");
        cJSON_AddStringToObject(param_l, "channel", "L");
        cJSON_AddNumberToObject(param_l, "band", band);
        cJSON_AddNumberToObject(param_l, "min", TAS5805M_EQ_MIN_DB);
        cJSON_AddNumberToObject(param_l, "max", TAS5805M_EQ_MAX_DB);
        cJSON_AddNumberToObject(param_l, "step", 1);
        cJSON_AddNumberToObject(param_l, "default", 0);
        cJSON_AddNumberToObject(param_l, "current", cur_l);
        cJSON_AddItemToArray(eq_bands_left_params, param_l);

        cJSON *param_r = cJSON_CreateObject();
        cJSON_AddStringToObject(param_r, "key", key_r);
        cJSON_AddStringToObject(param_r, "name", freq_label);
        cJSON_AddStringToObject(param_r, "type", "range");
        cJSON_AddStringToObject(param_r, "unit", "dB");
        cJSON_AddStringToObject(param_r, "label", freq_label);
        cJSON_AddStringToObject(param_r, "layout", "vertical");
        cJSON_AddStringToObject(param_r, "channel", "R");
        cJSON_AddNumberToObject(param_r, "band", band);
        cJSON_AddNumberToObject(param_r, "min", TAS5805M_EQ_MIN_DB);
        cJSON_AddNumberToObject(param_r, "max", TAS5805M_EQ_MAX_DB);
        cJSON_AddNumberToObject(param_r, "step", 1);
        cJSON_AddNumberToObject(param_r, "default", 0);
        cJSON_AddNumberToObject(param_r, "current", cur_r);
        cJSON_AddItemToArray(eq_bands_right_params, param_r);
    }

    cJSON_AddItemToObject(eq_bands_left, "parameters", eq_bands_left_params);
    cJSON_AddItemToObject(eq_bands_right, "parameters", eq_bands_right_params);
    
    // Add EQ band sub-groups to main groups array (after EQ group)
    cJSON_AddItemToArray(groups, eq_bands_left);
    cJSON_AddItemToArray(groups, eq_bands_right);
#endif
    
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

/**
 * Get DAC-only schema as JSON string
 * This includes only the basic DAC configuration groups (Volume, State, DAC Configuration)
 */
esp_err_t tas5805m_settings_get_dac_schema_json(char *json_out, size_t max_len) {
    ESP_LOGD(TAG, "%s: max_len=%zu", __func__, max_len);
    
    if (!json_out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get current state for schema
    TAS5805_STATE dac_state;
    esp_err_t err = tas5805m_get_state(&dac_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to get DAC state: %s", __func__, esp_err_to_name(err));
        dac_state.state = TAS5805M_CTRL_PLAY;
    }

    uint8_t digital_volume;
    if (tas5805m_get_digital_volume(&digital_volume) != ESP_OK) {
        digital_volume = TAS5805M_VOLUME_DIGITAL_DEFAULT;
    }

    uint8_t analog_gain;
    if (tas5805m_get_again(&analog_gain) != ESP_OK) {
        analog_gain = 0;
    }

    TAS5805M_DAC_MODE dac_mode;
    if (tas5805m_get_dac_mode(&dac_mode) != ESP_OK) {
        dac_mode = TAS5805M_DAC_MODE_BTL;
    }

    /* Get current modulation mode and frequencies */
    TAS5805M_MOD_MODE mod_mode;
    TAS5805M_SW_FREQ sw_freq;
    TAS5805M_BD_FREQ bd_freq;
    if (tas5805m_get_modulation_mode(&mod_mode, &sw_freq, &bd_freq) != ESP_OK) {
        mod_mode = MOD_MODE_BD;
        sw_freq = SW_FREQ_768K;
        bd_freq = SW_FREQ_80K;
    }

    // Build minimal DAC schema
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to create JSON root", __func__);
        return ESP_ERR_NO_MEM;
    }

    cJSON *groups = cJSON_CreateArray();
    
    // Volume Group
    cJSON *volume_group = cJSON_CreateObject();
    cJSON_AddStringToObject(volume_group, "name", "Volume");
    cJSON_AddStringToObject(volume_group, "description", "Digital and analog volume control");
    
    cJSON *volume_params = cJSON_CreateArray();
    
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

    // State Group
    cJSON *state_group = cJSON_CreateObject();
    cJSON_AddStringToObject(state_group, "name", "State");
    cJSON_AddStringToObject(state_group, "description", "DAC power and operation state");
    
    cJSON *state_params = cJSON_CreateArray();
    
    cJSON *state_param = cJSON_CreateObject();
    cJSON_AddStringToObject(state_param, "key", "state");
    cJSON_AddStringToObject(state_param, "name", "DAC State");
    cJSON_AddStringToObject(state_param, "type", "enum");
    cJSON_AddNumberToObject(state_param, "current", (int)dac_state.state);
    cJSON_AddBoolToObject(state_param, "readonly", true);
    
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

    // Faults Group
    cJSON *faults_group = cJSON_CreateObject();
    cJSON_AddStringToObject(faults_group, "name", "Faults");
    cJSON_AddStringToObject(faults_group, "description", "Current fault status indicators");
    cJSON_AddStringToObject(faults_group, "layout", "faults");
    
    cJSON *faults_params = cJSON_CreateArray();
    
    // Get current faults to populate schema
    TAS5805M_FAULT current_fault = {0};
    tas5805m_get_faults(&current_fault);
    
    // Create fault status parameter (readonly, displayed as list)
    cJSON *faults_param = cJSON_CreateObject();
    cJSON_AddStringToObject(faults_param, "key", "faults");
    cJSON_AddStringToObject(faults_param, "name", "Fault Status");
    cJSON_AddStringToObject(faults_param, "type", "faults-list");
    cJSON_AddBoolToObject(faults_param, "readonly", true);
    
    // Add current fault data
    cJSON *faults_array = tas5805m_create_faults_array(current_fault);
    if (faults_array) {
        cJSON_AddItemToObject(faults_param, "current", faults_array);
    }
    
    cJSON_AddItemToArray(faults_params, faults_param);
    cJSON_AddItemToObject(faults_group, "parameters", faults_params);
    cJSON_AddItemToArray(groups, faults_group);

    // DAC Configuration Group - simplified
    cJSON *dac_config_group = cJSON_CreateObject();
    cJSON_AddStringToObject(dac_config_group, "name", "DAC Configuration");
    cJSON_AddStringToObject(dac_config_group, "description", "DAC mode and mixer settings");
    cJSON_AddStringToObject(dac_config_group, "layout", "dac-config");
    
    cJSON *dac_config_params = cJSON_CreateArray();
    
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
    
    // Mixer Mode
    cJSON *mixer_mode_param = cJSON_CreateObject();
    cJSON_AddStringToObject(mixer_mode_param, "key", "mixer_mode");
    cJSON_AddStringToObject(mixer_mode_param, "name", "Mixer Mode");
    cJSON_AddStringToObject(mixer_mode_param, "type", "enum");
    cJSON_AddNumberToObject(mixer_mode_param, "current", (int)dac_state.mixer_mode);
    
    cJSON *mixer_values = cJSON_CreateArray();
    const char *mixer_names[] = {"Stereo", "Stereo (Inverse)", "Mono", "Right", "Left"};
    for (int i = MIXER_STEREO; i <= MIXER_LEFT; ++i) {
        cJSON *mv = cJSON_CreateObject();
        cJSON_AddNumberToObject(mv, "value", i);
        cJSON_AddStringToObject(mv, "name", mixer_names[i - 1]);
        cJSON_AddItemToArray(mixer_values, mv);
    }
    cJSON_AddItemToObject(mixer_mode_param, "values", mixer_values);
    cJSON_AddItemToArray(dac_config_params, mixer_mode_param);
    
    // Modulation Mode
    cJSON *mod_mode_param = cJSON_CreateObject();
    cJSON_AddStringToObject(mod_mode_param, "key", "modulation_mode");
    cJSON_AddStringToObject(mod_mode_param, "name", "Modulation Mode");
    cJSON_AddStringToObject(mod_mode_param, "type", "enum");
    cJSON_AddNumberToObject(mod_mode_param, "current", (int)mod_mode);
    
    cJSON *mod_values = cJSON_CreateArray();
    cJSON *mod_bd = cJSON_CreateObject();
    cJSON_AddNumberToObject(mod_bd, "value", MOD_MODE_BD);
    cJSON_AddStringToObject(mod_bd, "name", "BD");
    cJSON_AddItemToArray(mod_values, mod_bd);
    
    cJSON *mod_1spw = cJSON_CreateObject();
    cJSON_AddNumberToObject(mod_1spw, "value", MOD_MODE_1SPW);
    cJSON_AddStringToObject(mod_1spw, "name", "1SPW");
    cJSON_AddItemToArray(mod_values, mod_1spw);
    
    cJSON *mod_hybrid = cJSON_CreateObject();
    cJSON_AddNumberToObject(mod_hybrid, "value", MOD_MODE_HYBRID);
    cJSON_AddStringToObject(mod_hybrid, "name", "Hybrid");
    cJSON_AddItemToArray(mod_values, mod_hybrid);
    
    cJSON_AddItemToObject(mod_mode_param, "values", mod_values);
    cJSON_AddItemToArray(dac_config_params, mod_mode_param);
    
    // Switching Frequency
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
    
    // BD Frequency
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

    cJSON_AddItemToObject(root, "groups", groups);

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);
    if (json_len >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer (%zu >= %zu)", __func__, json_len, max_len);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGD(TAG, "%s: DAC schema JSON generated", __func__);
    return ESP_OK;
}

/**
 * Get EQ-only schema as JSON string
 * This includes only the EQ-related groups
 */
esp_err_t tas5805m_settings_get_eq_schema_json(char *json_out, size_t max_len) {
    ESP_LOGD(TAG, "%s: max_len=%zu", __func__, max_len);
    
    if (!json_out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to create JSON root", __func__);
        return ESP_ERR_NO_MEM;
    }

    cJSON *groups = cJSON_CreateArray();

    /* Load UI mode early so it can be used to conditionally include groups */
    TAS5805M_EQ_UI_MODE ui_mode = TAS5805M_EQ_UI_MODE_OFF;
    tas5805m_settings_load_eq_ui_mode(&ui_mode);

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    // ===== EQ Mode Group (first in schema as requested) =====
    {
        TAS5805M_EQ_MODE eq_mode_val = TAS5805M_EQ_MODE_OFF;
        tas5805m_get_eq_mode(&eq_mode_val);

        cJSON *eq_mode_group = cJSON_CreateObject();
        cJSON_AddStringToObject(eq_mode_group, "name", "EQ Mode");
        cJSON_AddStringToObject(eq_mode_group, "description", "Equalizer operation mode");

        cJSON *eq_mode_params = cJSON_CreateArray();

        cJSON *eq_ui_mode_param = cJSON_CreateObject();
        cJSON_AddStringToObject(eq_ui_mode_param, "key", "eq_ui_mode");
        cJSON_AddStringToObject(eq_ui_mode_param, "name", "EQ Mode");
        cJSON_AddStringToObject(eq_ui_mode_param, "type", "enum");

        cJSON_AddNumberToObject(eq_ui_mode_param, "current", (int)ui_mode);

        cJSON *eq_ui_mode_values = cJSON_CreateArray();
        const char *ui_mode_names[] = {"Off", "15-Band", "15-Band Bi-Amp", "Presets", "Advanced Bi-Amp"};
        for (int i = 0; i <= TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP; ++i) {
            cJSON *val = cJSON_CreateObject();
            cJSON_AddNumberToObject(val, "value", i);
            cJSON_AddStringToObject(val, "name", ui_mode_names[i]);
            cJSON_AddItemToArray(eq_ui_mode_values, val);
        }
        cJSON_AddItemToObject(eq_ui_mode_param, "values", eq_ui_mode_values);
        cJSON_AddItemToArray(eq_mode_params, eq_ui_mode_param);

        cJSON_AddItemToObject(eq_mode_group, "parameters", eq_mode_params);
        cJSON_AddItemToArray(groups, eq_mode_group);
    }
#endif

    // ===== Channel Gain Group (hidden in Advanced Bi-Amp mode - uses Low/High gains instead) =====
    if (ui_mode != TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP) {
        cJSON *ch_group = cJSON_CreateObject();
        cJSON_AddStringToObject(ch_group, "name", "Channel Gain");
        cJSON_AddStringToObject(ch_group, "description", "Per-channel mixer gain control");

        cJSON *ch_params = cJSON_CreateArray();

        // Prefer reading current driver state; fall back to NVS for persisted values
        int8_t cur_ch_l = 0, cur_ch_r = 0;
        if (tas5805m_settings_restored) {
            if (tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &cur_ch_l) != ESP_OK) {
                int tmp = 0;
                if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &tmp) == ESP_OK) {
                    cur_ch_l = (int8_t)tmp;
                }
            }
            if (tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &cur_ch_r) != ESP_OK) {
                int tmp = 0;
                if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &tmp) == ESP_OK) {
                    cur_ch_r = (int8_t)tmp;
                }
            }
        } else {
            int tmp = 0;
            if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &tmp) == ESP_OK) {
                cur_ch_l = (int8_t)tmp;
            }
            if (tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &tmp) == ESP_OK) {
                cur_ch_r = (int8_t)tmp;
            }
        }

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

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    /* Create main EQ group and parameters array so subsequent blocks can
     * append parameters (presets, profiles, etc.). This mirrors the DAC
     * schema layout and ensures `eq_params` is defined before use. */
    cJSON *eq_group = cJSON_CreateObject();
    cJSON_AddStringToObject(eq_group, "name", "EQ");
    cJSON_AddStringToObject(eq_group, "description", "Equalizer settings");
    cJSON_AddStringToObject(eq_group, "layout", "eq-controls");
    cJSON *eq_params = cJSON_CreateArray();

    /* EQ Preset / Profile per channel (so UI 'Presets' mode has controls) */
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

        /* Right channel preset selector */
        cJSON *prof_r_param = cJSON_CreateObject();
        cJSON_AddStringToObject(prof_r_param, "key", "eq_profile_r");
        cJSON_AddStringToObject(prof_r_param, "name", "EQ Preset (R)");
        cJSON_AddStringToObject(prof_r_param, "type", "enum");
        cJSON_AddNumberToObject(prof_r_param, "current", (int)cur_prof_r);

        cJSON *prof_r_values = cJSON_CreateArray();
        v = cJSON_CreateObject(); cJSON_AddNumberToObject(v, "value", FLAT); cJSON_AddStringToObject(v, "name", "Flat"); cJSON_AddItemToArray(prof_r_values, v);
        /* clone same list entries for right channel */
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
        /* Create presets group and add both parameters so it can be hidden */
        cJSON *eq_presets_group = cJSON_CreateObject();
        cJSON_AddStringToObject(eq_presets_group, "name", "EQ Presets");
        cJSON_AddStringToObject(eq_presets_group, "description", "Preset profiles for left and right channels");
        cJSON_AddStringToObject(eq_presets_group, "layout", "eq-presets");
        cJSON *eq_presets_params = cJSON_CreateArray();
        cJSON_AddItemToArray(eq_presets_params, prof_l_param);
        cJSON_AddItemToArray(eq_presets_params, prof_r_param);
        cJSON_AddItemToObject(eq_presets_group, "parameters", eq_presets_params);
        cJSON_AddItemToArray(groups, eq_presets_group);
    }
#else
    /* If EQ support disabled provide readonly placeholders */
    cJSON *prof_l_param = cJSON_CreateObject();
    cJSON_AddStringToObject(prof_l_param, "key", "eq_profile_l");
    cJSON_AddStringToObject(prof_l_param, "name", "EQ Preset (L)");
    cJSON_AddStringToObject(prof_l_param, "type", "enum");
    cJSON_AddNumberToObject(prof_l_param, "current", (int)FLAT);
    cJSON *prof_l_vals = cJSON_CreateArray();
    cJSON *pv = cJSON_CreateObject(); cJSON_AddNumberToObject(pv, "value", FLAT); cJSON_AddStringToObject(pv, "name", "Flat"); cJSON_AddItemToArray(prof_l_vals, pv);
    cJSON_AddItemToObject(prof_l_param, "values", prof_l_vals);

    cJSON *prof_r_param = cJSON_CreateObject();
    cJSON_AddStringToObject(prof_r_param, "key", "eq_profile_r");
    cJSON_AddStringToObject(prof_r_param, "name", "EQ Preset (R)");
    cJSON_AddStringToObject(prof_r_param, "type", "enum");
    cJSON_AddNumberToObject(prof_r_param, "current", (int)FLAT);
    cJSON *prof_r_vals = cJSON_CreateArray();
    pv = cJSON_CreateObject(); cJSON_AddNumberToObject(pv, "value", FLAT); cJSON_AddStringToObject(pv, "name", "Flat"); cJSON_AddItemToArray(prof_r_vals, pv);
    cJSON_AddItemToObject(prof_r_param, "values", prof_r_vals);
    /* Create presets group for readonly placeholders when EQ support disabled */
    cJSON *eq_presets_group = cJSON_CreateObject();
    cJSON_AddStringToObject(eq_presets_group, "name", "EQ Presets");
    cJSON_AddStringToObject(eq_presets_group, "description", "Preset profiles (readonly)");
    cJSON_AddStringToObject(eq_presets_group, "layout", "eq-presets");
    cJSON *eq_presets_params = cJSON_CreateArray();
    cJSON_AddItemToArray(eq_presets_params, prof_l_param);
    cJSON_AddItemToArray(eq_presets_params, prof_r_param);
    cJSON_AddItemToObject(eq_presets_group, "parameters", eq_presets_params);
    cJSON_AddItemToArray(groups, eq_presets_group);
#endif

    // EQ Bands Group (Left Channel)
    cJSON *eq_bands_left = cJSON_CreateObject();
    cJSON_AddStringToObject(eq_bands_left, "name", "EQ Bands (Left)");
    cJSON_AddStringToObject(eq_bands_left, "description", "15-band parametric equalizer for left channel");
    cJSON_AddStringToObject(eq_bands_left, "layout", "eq-bands");
    cJSON_AddStringToObject(eq_bands_left, "channel", "left");
    
    cJSON *eq_bands_left_params = cJSON_CreateArray();
    for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
        int gain = 0;
        // Prefer reading current driver state; fall back to NVS
        if (tas5805m_settings_restored) {
            if (tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) != ESP_OK) {
                tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain);
            }
        } else {
            tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain);
        }

        cJSON *band_param = cJSON_CreateObject();
        char key[32];
        char freq_label[32];
        snprintf(key, sizeof(key), "eq_gain_l_%d", band);
        snprintf(freq_label, sizeof(freq_label), "%d Hz", tas5805m_eq_bands[band]);
        
        cJSON_AddStringToObject(band_param, "key", key);
        cJSON_AddStringToObject(band_param, "name", freq_label);
        cJSON_AddStringToObject(band_param, "type", "range");
        cJSON_AddStringToObject(band_param, "unit", "dB");
        cJSON_AddStringToObject(band_param, "label", freq_label);
        cJSON_AddStringToObject(band_param, "layout", "vertical");
        cJSON_AddNumberToObject(band_param, "band", band);
        cJSON_AddNumberToObject(band_param, "min", TAS5805M_EQ_MIN_DB);
        cJSON_AddNumberToObject(band_param, "max", TAS5805M_EQ_MAX_DB);
        cJSON_AddNumberToObject(band_param, "step", 1);
        cJSON_AddNumberToObject(band_param, "default", 0);
        cJSON_AddNumberToObject(band_param, "current", gain);
        
        cJSON_AddItemToArray(eq_bands_left_params, band_param);
    }
    cJSON_AddItemToObject(eq_bands_left, "parameters", eq_bands_left_params);
    cJSON_AddItemToArray(groups, eq_bands_left);

    // EQ Bands Group (Right Channel)
    cJSON *eq_bands_right = cJSON_CreateObject();
    cJSON_AddStringToObject(eq_bands_right, "name", "EQ Bands (Right)");
    cJSON_AddStringToObject(eq_bands_right, "description", "15-band parametric equalizer for right channel");
    cJSON_AddStringToObject(eq_bands_right, "layout", "eq-bands");
    cJSON_AddStringToObject(eq_bands_right, "channel", "right");
    
    cJSON *eq_bands_right_params = cJSON_CreateArray();
    for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
        int gain = 0;
        // Prefer reading current driver state; fall back to NVS
        if (tas5805m_settings_restored) {
            if (tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain) != ESP_OK) {
                tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain);
            }
        } else {
            tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain);
        }

        cJSON *band_param = cJSON_CreateObject();
        char key[32];
        char freq_label[32];
        snprintf(key, sizeof(key), "eq_gain_r_%d", band);
        snprintf(freq_label, sizeof(freq_label), "%d Hz", tas5805m_eq_bands[band]);
        
        cJSON_AddStringToObject(band_param, "key", key);
        cJSON_AddStringToObject(band_param, "name", freq_label);
        cJSON_AddStringToObject(band_param, "type", "range");
        cJSON_AddStringToObject(band_param, "unit", "dB");
        cJSON_AddStringToObject(band_param, "label", freq_label);
        cJSON_AddStringToObject(band_param, "layout", "vertical");
        cJSON_AddNumberToObject(band_param, "band", band);
        cJSON_AddNumberToObject(band_param, "min", TAS5805M_EQ_MIN_DB);
        cJSON_AddNumberToObject(band_param, "max", TAS5805M_EQ_MAX_DB);
        cJSON_AddNumberToObject(band_param, "step", 1);
        cJSON_AddNumberToObject(band_param, "default", 0);
        cJSON_AddNumberToObject(band_param, "current", gain);
        
        cJSON_AddItemToArray(eq_bands_right_params, band_param);
    }
    cJSON_AddItemToObject(eq_bands_right, "parameters", eq_bands_right_params);
    cJSON_AddItemToArray(groups, eq_bands_right);
#endif

    /* ===== Advanced Bi-Amp Crossover Group (only shown in Advanced Bi-Amp mode) ===== */
    if (ui_mode == TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP) {
        tas5805m_biamp_settings_t biamp;
        tas5805m_settings_load_biamp(&biamp);
        tas5805m_loudness_settings_t loudness;
        tas5805m_settings_load_loudness(&loudness);

        cJSON *biamp_group = cJSON_CreateObject();
        cJSON_AddStringToObject(biamp_group, "name", "Advanced Bi-Amp Crossover");
        cJSON_AddStringToObject(biamp_group, "description", "Configure active crossover with per-output PEQ and gain");
        cJSON_AddStringToObject(biamp_group, "layout", "biamp-crossover");

        cJSON *sections = cJSON_CreateArray();

        /* === Section 1: Crossover Settings === */
        {
            cJSON *xover_section = cJSON_CreateObject();
            cJSON_AddStringToObject(xover_section, "name", "Crossover Settings");
            cJSON_AddStringToObject(xover_section, "layout", "form");
            cJSON *xover_params = cJSON_CreateArray();

            cJSON *xover_freq = cJSON_CreateObject();
            cJSON_AddStringToObject(xover_freq, "key", "biamp_xover_freq");
            cJSON_AddStringToObject(xover_freq, "name", "Frequency");
            cJSON_AddStringToObject(xover_freq, "type", "range");
            cJSON_AddStringToObject(xover_freq, "unit", "Hz");
            cJSON_AddNumberToObject(xover_freq, "min", 20);
            cJSON_AddNumberToObject(xover_freq, "max", 20000);
            cJSON_AddNumberToObject(xover_freq, "step", 1);
            cJSON_AddNumberToObject(xover_freq, "current", biamp.crossover_freq);
            cJSON_AddItemToArray(xover_params, xover_freq);

            cJSON *slope = cJSON_CreateObject();
            cJSON_AddStringToObject(slope, "key", "biamp_slope");
            cJSON_AddStringToObject(slope, "name", "Slope");
            cJSON_AddStringToObject(slope, "type", "enum");
            cJSON_AddNumberToObject(slope, "current", (int)biamp.slope);
            cJSON *slope_vals = cJSON_CreateArray();
            cJSON *sv;
            sv = cJSON_CreateObject(); cJSON_AddNumberToObject(sv, "value", BIAMP_SLOPE_12DB); cJSON_AddStringToObject(sv, "name", "12 dB/oct"); cJSON_AddItemToArray(slope_vals, sv);
            sv = cJSON_CreateObject(); cJSON_AddNumberToObject(sv, "value", BIAMP_SLOPE_24DB); cJSON_AddStringToObject(sv, "name", "24 dB/oct (LR)"); cJSON_AddItemToArray(slope_vals, sv);
            sv = cJSON_CreateObject(); cJSON_AddNumberToObject(sv, "value", BIAMP_SLOPE_48DB); cJSON_AddStringToObject(sv, "name", "48 dB/oct"); cJSON_AddItemToArray(slope_vals, sv);
            cJSON_AddItemToObject(slope, "values", slope_vals);
            cJSON_AddItemToArray(xover_params, slope);

            cJSON *xtype = cJSON_CreateObject();
            cJSON_AddStringToObject(xtype, "key", "biamp_type");
            cJSON_AddStringToObject(xtype, "name", "Type");
            cJSON_AddStringToObject(xtype, "type", "enum");
            cJSON_AddNumberToObject(xtype, "current", (int)biamp.type);
            cJSON *type_vals = cJSON_CreateArray();
            cJSON *tv;
            tv = cJSON_CreateObject(); cJSON_AddNumberToObject(tv, "value", BIAMP_TYPE_BUTTERWORTH); cJSON_AddStringToObject(tv, "name", "Butterworth"); cJSON_AddItemToArray(type_vals, tv);
            tv = cJSON_CreateObject(); cJSON_AddNumberToObject(tv, "value", BIAMP_TYPE_LINKWITZ_RILEY); cJSON_AddStringToObject(tv, "name", "Linkwitz-Riley"); cJSON_AddItemToArray(type_vals, tv);
            cJSON_AddItemToObject(xtype, "values", type_vals);
            cJSON_AddItemToArray(xover_params, xtype);

            cJSON *sr = cJSON_CreateObject();
            cJSON_AddStringToObject(sr, "key", "biamp_sample_rate");
            cJSON_AddStringToObject(sr, "name", "Sample Rate");
            cJSON_AddStringToObject(sr, "type", "enum");
            cJSON_AddNumberToObject(sr, "current", (int)biamp.sample_rate);
            cJSON *sr_vals = cJSON_CreateArray();
            cJSON *srv;
            srv = cJSON_CreateObject(); cJSON_AddNumberToObject(srv, "value", BIAMP_SAMPLE_RATE_44100); cJSON_AddStringToObject(srv, "name", "44.1 kHz"); cJSON_AddItemToArray(sr_vals, srv);
            srv = cJSON_CreateObject(); cJSON_AddNumberToObject(srv, "value", BIAMP_SAMPLE_RATE_48000); cJSON_AddStringToObject(srv, "name", "48 kHz"); cJSON_AddItemToArray(sr_vals, srv);
            srv = cJSON_CreateObject(); cJSON_AddNumberToObject(srv, "value", BIAMP_SAMPLE_RATE_88200); cJSON_AddStringToObject(srv, "name", "88.2 kHz"); cJSON_AddItemToArray(sr_vals, srv);
            srv = cJSON_CreateObject(); cJSON_AddNumberToObject(srv, "value", BIAMP_SAMPLE_RATE_96000); cJSON_AddStringToObject(srv, "name", "96 kHz"); cJSON_AddItemToArray(sr_vals, srv);
            cJSON_AddItemToObject(sr, "values", sr_vals);
            cJSON_AddItemToArray(xover_params, sr);

            cJSON_AddItemToObject(xover_section, "parameters", xover_params);
            cJSON_AddItemToArray(sections, xover_section);
        }

        /* === Section 2: Low/High Output Columns === */
        {
            cJSON *columns_section = cJSON_CreateObject();
            cJSON *columns = cJSON_CreateArray();

            /* Low Output (Woofer) Column */
            cJSON *low_col = cJSON_CreateObject();
            cJSON_AddStringToObject(low_col, "name", "Low Output (Woofer)");
            cJSON_AddStringToObject(low_col, "layout", "output-channel");
            cJSON *low_params = cJSON_CreateArray();

            /* Gain slider */
            cJSON *low_gain = cJSON_CreateObject();
            cJSON_AddStringToObject(low_gain, "key", "biamp_low_gain");
            cJSON_AddStringToObject(low_gain, "label", "Gain");
            cJSON_AddStringToObject(low_gain, "name", "Output Gain");
            cJSON_AddStringToObject(low_gain, "type", "range");
            cJSON_AddStringToObject(low_gain, "unit", "dB");
            cJSON_AddNumberToObject(low_gain, "min", -24);
            cJSON_AddNumberToObject(low_gain, "max", 24);
            cJSON_AddNumberToObject(low_gain, "step", 0.5);
            cJSON_AddNumberToObject(low_gain, "decimals", 1);
            cJSON_AddNumberToObject(low_gain, "current", biamp.low_gain / 2.0);
            cJSON_AddItemToArray(low_params, low_gain);

            /* Subsonic HPF slider (woofer only) */
            cJSON *subsonic = cJSON_CreateObject();
            cJSON_AddStringToObject(subsonic, "key", "biamp_subsonic_freq");
            cJSON_AddStringToObject(subsonic, "label", "Subsonic");
            cJSON_AddStringToObject(subsonic, "name", "Subsonic HPF");
            cJSON_AddStringToObject(subsonic, "type", "range");
            cJSON_AddStringToObject(subsonic, "unit", "Hz");
            cJSON_AddNumberToObject(subsonic, "min", 0);
            cJSON_AddNumberToObject(subsonic, "max", 80);
            cJSON_AddNumberToObject(subsonic, "step", 5);
            cJSON_AddNumberToObject(subsonic, "current", biamp.subsonic_freq);
            cJSON_AddItemToArray(low_params, subsonic);

            /* Baffle Step Compensation subgroup (woofer only) */
            {
                cJSON *baffle_group = cJSON_CreateObject();
                cJSON_AddStringToObject(baffle_group, "name", "Baffle Step");
                cJSON_AddStringToObject(baffle_group, "type", "subgroup");
                cJSON *baffle_params = cJSON_CreateArray();

                /* Baffle width (0=disabled, 5-50cm) */
                cJSON *baffle_width = cJSON_CreateObject();
                cJSON_AddStringToObject(baffle_width, "key", "biamp_baffle_width");
                cJSON_AddStringToObject(baffle_width, "name", "Baffle Width");
                cJSON_AddStringToObject(baffle_width, "type", "range");
                cJSON_AddStringToObject(baffle_width, "unit", "cm");
                cJSON_AddNumberToObject(baffle_width, "min", 0);
                cJSON_AddNumberToObject(baffle_width, "max", 50);
                cJSON_AddNumberToObject(baffle_width, "step", 1);
                cJSON_AddNumberToObject(baffle_width, "current", biamp.baffle_width_cm);
                cJSON_AddItemToArray(baffle_params, baffle_width);

                /* Placement type */
                cJSON *placement = cJSON_CreateObject();
                cJSON_AddStringToObject(placement, "key", "biamp_baffle_placement");
                cJSON_AddStringToObject(placement, "name", "Placement");
                cJSON_AddStringToObject(placement, "type", "enum");
                cJSON_AddNumberToObject(placement, "current", (int)biamp.baffle_placement);
                cJSON *place_vals = cJSON_CreateArray();
                cJSON *pv;
                pv = cJSON_CreateObject(); cJSON_AddNumberToObject(pv, "value", BAFFLE_PLACEMENT_FREESTANDING); cJSON_AddStringToObject(pv, "name", "Freestanding (+6dB)"); cJSON_AddItemToArray(place_vals, pv);
                pv = cJSON_CreateObject(); cJSON_AddNumberToObject(pv, "value", BAFFLE_PLACEMENT_NEAR_WALL); cJSON_AddStringToObject(pv, "name", "Near Wall (+3dB)"); cJSON_AddItemToArray(place_vals, pv);
                pv = cJSON_CreateObject(); cJSON_AddNumberToObject(pv, "value", BAFFLE_PLACEMENT_CORNER); cJSON_AddStringToObject(pv, "name", "Corner (0dB)"); cJSON_AddItemToArray(place_vals, pv);
                cJSON_AddItemToObject(placement, "values", place_vals);
                cJSON_AddItemToArray(baffle_params, placement);

                cJSON_AddItemToObject(baffle_group, "parameters", baffle_params);
                cJSON_AddItemToArray(low_params, baffle_group);
            }

            /* PEQ bands as subgroups */
            for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS; i++) {
                char key[32], label[32];
                cJSON *peq_group = cJSON_CreateObject();
                snprintf(label, sizeof(label), "PEQ %d", i + 1);
                cJSON_AddStringToObject(peq_group, "name", label);
                cJSON_AddStringToObject(peq_group, "type", "peq-subgroup");
                cJSON *peq_params = cJSON_CreateArray();

                /* Frequency */
                snprintf(key, sizeof(key), "biamp_low_peq%d_freq", i);
                cJSON *freq = cJSON_CreateObject();
                cJSON_AddStringToObject(freq, "key", key);
                cJSON_AddStringToObject(freq, "name", "Freq");
                cJSON_AddStringToObject(freq, "type", "range");
                cJSON_AddStringToObject(freq, "unit", "Hz");
                cJSON_AddNumberToObject(freq, "min", 20);
                cJSON_AddNumberToObject(freq, "max", 20000);
                cJSON_AddNumberToObject(freq, "step", 1);
                cJSON_AddNumberToObject(freq, "current", biamp.low_peq[i].freq);
                cJSON_AddItemToArray(peq_params, freq);

                /* Gain (stored as x2 for 0.5 dB resolution) */
                snprintf(key, sizeof(key), "biamp_low_peq%d_gain", i);
                cJSON *gain = cJSON_CreateObject();
                cJSON_AddStringToObject(gain, "key", key);
                cJSON_AddStringToObject(gain, "name", "Gain");
                cJSON_AddStringToObject(gain, "type", "range");
                cJSON_AddStringToObject(gain, "unit", "dB");
                cJSON_AddNumberToObject(gain, "min", -15);
                cJSON_AddNumberToObject(gain, "max", 15);
                cJSON_AddNumberToObject(gain, "step", 0.5);
                cJSON_AddNumberToObject(gain, "decimals", 1);
                cJSON_AddNumberToObject(gain, "current", biamp.low_peq[i].gain / 2.0);
                cJSON_AddItemToArray(peq_params, gain);

                /* Q factor (stored as q_x10, display as float) */
                snprintf(key, sizeof(key), "biamp_low_peq%d_q", i);
                cJSON *q = cJSON_CreateObject();
                cJSON_AddStringToObject(q, "key", key);
                cJSON_AddStringToObject(q, "name", "Q");
                cJSON_AddStringToObject(q, "type", "range");
                cJSON_AddNumberToObject(q, "min", 0.5);
                cJSON_AddNumberToObject(q, "max", 10.0);
                cJSON_AddNumberToObject(q, "step", 0.1);
                cJSON_AddNumberToObject(q, "decimals", 1);
                cJSON_AddNumberToObject(q, "current", biamp.low_peq[i].q_x10 / 10.0);
                cJSON_AddItemToArray(peq_params, q);

                cJSON_AddItemToObject(peq_group, "parameters", peq_params);
                cJSON_AddItemToArray(low_params, peq_group);
            }

            /* Phase at the end */
            cJSON *low_phase = cJSON_CreateObject();
            cJSON_AddStringToObject(low_phase, "key", "biamp_low_phase");
            cJSON_AddStringToObject(low_phase, "name", "Phase");
            cJSON_AddStringToObject(low_phase, "type", "radio");
            cJSON_AddNumberToObject(low_phase, "current", biamp.low_phase_invert);
            cJSON *lp_vals = cJSON_CreateArray();
            cJSON *lpv;
            lpv = cJSON_CreateObject(); cJSON_AddNumberToObject(lpv, "value", 0); cJSON_AddStringToObject(lpv, "name", "Normal"); cJSON_AddItemToArray(lp_vals, lpv);
            lpv = cJSON_CreateObject(); cJSON_AddNumberToObject(lpv, "value", 1); cJSON_AddStringToObject(lpv, "name", "Invert"); cJSON_AddItemToArray(lp_vals, lpv);
            cJSON_AddItemToObject(low_phase, "values", lp_vals);
            cJSON_AddItemToArray(low_params, low_phase);

            cJSON_AddItemToObject(low_col, "parameters", low_params);
            cJSON_AddItemToArray(columns, low_col);

            /* High Output (Tweeter) Column */
            cJSON *high_col = cJSON_CreateObject();
            cJSON_AddStringToObject(high_col, "name", "High Output (Tweeter)");
            cJSON_AddStringToObject(high_col, "layout", "output-channel");
            cJSON *high_params = cJSON_CreateArray();

            /* Gain slider */
            cJSON *high_gain = cJSON_CreateObject();
            cJSON_AddStringToObject(high_gain, "key", "biamp_high_gain");
            cJSON_AddStringToObject(high_gain, "label", "Gain");
            cJSON_AddStringToObject(high_gain, "name", "Output Gain");
            cJSON_AddStringToObject(high_gain, "type", "range");
            cJSON_AddStringToObject(high_gain, "unit", "dB");
            cJSON_AddNumberToObject(high_gain, "min", -24);
            cJSON_AddNumberToObject(high_gain, "max", 24);
            cJSON_AddNumberToObject(high_gain, "step", 0.5);
            cJSON_AddNumberToObject(high_gain, "decimals", 1);
            cJSON_AddNumberToObject(high_gain, "current", biamp.high_gain / 2.0);
            cJSON_AddItemToArray(high_params, high_gain);

            /* Tweeter delay slider for time alignment */
            cJSON *twt_delay = cJSON_CreateObject();
            cJSON_AddStringToObject(twt_delay, "key", "biamp_tweeter_delay");
            cJSON_AddStringToObject(twt_delay, "label", "Delay");
            cJSON_AddStringToObject(twt_delay, "name", "Tweeter Delay");
            cJSON_AddStringToObject(twt_delay, "description", "Delays the tweeter to align with the woofer acoustic center. Set to the physical offset between drivers.");
            cJSON_AddStringToObject(twt_delay, "type", "range");
            cJSON_AddStringToObject(twt_delay, "unit", "mm");
            cJSON_AddNumberToObject(twt_delay, "min", 0);
            cJSON_AddNumberToObject(twt_delay, "max", 50);
            cJSON_AddNumberToObject(twt_delay, "step", 1);
            cJSON_AddNumberToObject(twt_delay, "current", biamp.tweeter_delay_mm);
            cJSON_AddItemToArray(high_params, twt_delay);

            /* Breakup Notch subgroup */
            {
                cJSON *notch_group = cJSON_CreateObject();
                cJSON_AddStringToObject(notch_group, "name", "Breakup Notch");
                cJSON_AddStringToObject(notch_group, "type", "subgroup");
                cJSON_AddStringToObject(notch_group, "description", "Tames the resonance peak at the tweeter's upper frequency limit");
                cJSON *notch_params = cJSON_CreateArray();

                /* Notch frequency */
                cJSON *notch_freq = cJSON_CreateObject();
                cJSON_AddStringToObject(notch_freq, "key", "biamp_notch_freq");
                cJSON_AddStringToObject(notch_freq, "name", "Frequency");
                cJSON_AddStringToObject(notch_freq, "type", "range");
                cJSON_AddStringToObject(notch_freq, "unit", "Hz");
                cJSON_AddNumberToObject(notch_freq, "min", 0);
                cJSON_AddNumberToObject(notch_freq, "max", 25000);
                cJSON_AddNumberToObject(notch_freq, "step", 100);
                cJSON_AddNumberToObject(notch_freq, "current", biamp.notch_freq);
                cJSON_AddItemToArray(notch_params, notch_freq);

                /* Notch depth (gain, negative only) */
                cJSON *notch_gain = cJSON_CreateObject();
                cJSON_AddStringToObject(notch_gain, "key", "biamp_notch_gain");
                cJSON_AddStringToObject(notch_gain, "name", "Depth");
                cJSON_AddStringToObject(notch_gain, "type", "range");
                cJSON_AddStringToObject(notch_gain, "unit", "dB");
                cJSON_AddNumberToObject(notch_gain, "min", -12);
                cJSON_AddNumberToObject(notch_gain, "max", 0);
                cJSON_AddNumberToObject(notch_gain, "step", 0.5);
                cJSON_AddNumberToObject(notch_gain, "decimals", 1);
                cJSON_AddNumberToObject(notch_gain, "current", biamp.notch_gain / 2.0);
                cJSON_AddItemToArray(notch_params, notch_gain);

                /* Notch Q */
                cJSON *notch_q = cJSON_CreateObject();
                cJSON_AddStringToObject(notch_q, "key", "biamp_notch_q");
                cJSON_AddStringToObject(notch_q, "name", "Q");
                cJSON_AddStringToObject(notch_q, "type", "range");
                cJSON_AddNumberToObject(notch_q, "min", 2.0);
                cJSON_AddNumberToObject(notch_q, "max", 10.0);
                cJSON_AddNumberToObject(notch_q, "step", 0.1);
                cJSON_AddNumberToObject(notch_q, "decimals", 1);
                cJSON_AddNumberToObject(notch_q, "current", biamp.notch_q_x10 / 10.0);
                cJSON_AddItemToArray(notch_params, notch_q);

                cJSON_AddItemToObject(notch_group, "parameters", notch_params);
                cJSON_AddItemToArray(high_params, notch_group);
            }

            /* PEQ bands as subgroups */
            for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS; i++) {
                char key[32], label[32];
                cJSON *peq_group = cJSON_CreateObject();
                snprintf(label, sizeof(label), "PEQ %d", i + 1);
                cJSON_AddStringToObject(peq_group, "name", label);
                cJSON_AddStringToObject(peq_group, "type", "peq-subgroup");
                cJSON *peq_params = cJSON_CreateArray();

                /* Frequency */
                snprintf(key, sizeof(key), "biamp_high_peq%d_freq", i);
                cJSON *freq = cJSON_CreateObject();
                cJSON_AddStringToObject(freq, "key", key);
                cJSON_AddStringToObject(freq, "name", "Freq");
                cJSON_AddStringToObject(freq, "type", "range");
                cJSON_AddStringToObject(freq, "unit", "Hz");
                cJSON_AddNumberToObject(freq, "min", 20);
                cJSON_AddNumberToObject(freq, "max", 20000);
                cJSON_AddNumberToObject(freq, "step", 1);
                cJSON_AddNumberToObject(freq, "current", biamp.high_peq[i].freq);
                cJSON_AddItemToArray(peq_params, freq);

                /* Gain (stored as x2 for 0.5 dB resolution) */
                snprintf(key, sizeof(key), "biamp_high_peq%d_gain", i);
                cJSON *gain = cJSON_CreateObject();
                cJSON_AddStringToObject(gain, "key", key);
                cJSON_AddStringToObject(gain, "name", "Gain");
                cJSON_AddStringToObject(gain, "type", "range");
                cJSON_AddStringToObject(gain, "unit", "dB");
                cJSON_AddNumberToObject(gain, "min", -15);
                cJSON_AddNumberToObject(gain, "max", 15);
                cJSON_AddNumberToObject(gain, "step", 0.5);
                cJSON_AddNumberToObject(gain, "decimals", 1);
                cJSON_AddNumberToObject(gain, "current", biamp.high_peq[i].gain / 2.0);
                cJSON_AddItemToArray(peq_params, gain);

                /* Q factor (stored as q_x10, display as float) */
                snprintf(key, sizeof(key), "biamp_high_peq%d_q", i);
                cJSON *q = cJSON_CreateObject();
                cJSON_AddStringToObject(q, "key", key);
                cJSON_AddStringToObject(q, "name", "Q");
                cJSON_AddStringToObject(q, "type", "range");
                cJSON_AddNumberToObject(q, "min", 0.5);
                cJSON_AddNumberToObject(q, "max", 10.0);
                cJSON_AddNumberToObject(q, "step", 0.1);
                cJSON_AddNumberToObject(q, "decimals", 1);
                cJSON_AddNumberToObject(q, "current", biamp.high_peq[i].q_x10 / 10.0);
                cJSON_AddItemToArray(peq_params, q);

                cJSON_AddItemToObject(peq_group, "parameters", peq_params);
                cJSON_AddItemToArray(high_params, peq_group);
            }

            /* Phase at the end */
            cJSON *high_phase = cJSON_CreateObject();
            cJSON_AddStringToObject(high_phase, "key", "biamp_high_phase");
            cJSON_AddStringToObject(high_phase, "name", "Phase");
            cJSON_AddStringToObject(high_phase, "type", "radio");
            cJSON_AddNumberToObject(high_phase, "current", biamp.high_phase_invert);
            cJSON *hp_vals = cJSON_CreateArray();
            cJSON *hpv;
            hpv = cJSON_CreateObject(); cJSON_AddNumberToObject(hpv, "value", 0); cJSON_AddStringToObject(hpv, "name", "Normal"); cJSON_AddItemToArray(hp_vals, hpv);
            hpv = cJSON_CreateObject(); cJSON_AddNumberToObject(hpv, "value", 1); cJSON_AddStringToObject(hpv, "name", "Invert"); cJSON_AddItemToArray(hp_vals, hpv);
            cJSON_AddItemToObject(high_phase, "values", hp_vals);
            cJSON_AddItemToArray(high_params, high_phase);

            /* Air/Brilliance shelf slider (after phase) */
            cJSON *air_gain = cJSON_CreateObject();
            cJSON_AddStringToObject(air_gain, "key", "biamp_air_gain");
            cJSON_AddStringToObject(air_gain, "label", "Air");
            cJSON_AddStringToObject(air_gain, "name", "Air / Brilliance");
            cJSON_AddStringToObject(air_gain, "description", "High shelf at 10kHz for treble sparkle and air");
            cJSON_AddStringToObject(air_gain, "type", "range");
            cJSON_AddStringToObject(air_gain, "unit", "dB");
            cJSON_AddNumberToObject(air_gain, "min", -6);
            cJSON_AddNumberToObject(air_gain, "max", 6);
            cJSON_AddNumberToObject(air_gain, "step", 0.5);
            cJSON_AddNumberToObject(air_gain, "decimals", 1);
            cJSON_AddNumberToObject(air_gain, "current", biamp.air_gain / 2.0);
            cJSON_AddItemToArray(high_params, air_gain);

            cJSON_AddItemToObject(high_col, "parameters", high_params);
            cJSON_AddItemToArray(columns, high_col);

            cJSON_AddItemToObject(columns_section, "columns", columns);
            cJSON_AddItemToArray(sections, columns_section);
        }

        /* === Section 3: Loudness Compensation === */
        {
            cJSON *loud_section = cJSON_CreateObject();
            cJSON_AddStringToObject(loud_section, "name", "Loudness Compensation");
            cJSON_AddStringToObject(loud_section, "layout", "loudness");
            cJSON *loud_params = cJSON_CreateArray();

            /* Enabled toggle (always visible) */
            cJSON *loud_en = cJSON_CreateObject();
            cJSON_AddStringToObject(loud_en, "key", "loudness_enabled");
            cJSON_AddStringToObject(loud_en, "name", "Enable");
            cJSON_AddStringToObject(loud_en, "type", "enum");
            cJSON_AddNumberToObject(loud_en, "current", loudness.enabled);
            cJSON *loud_en_vals = cJSON_CreateArray();
            cJSON *lev;
            lev = cJSON_CreateObject(); cJSON_AddNumberToObject(lev, "value", 0); cJSON_AddStringToObject(lev, "name", "Off"); cJSON_AddItemToArray(loud_en_vals, lev);
            lev = cJSON_CreateObject(); cJSON_AddNumberToObject(lev, "value", 1); cJSON_AddStringToObject(lev, "name", "On"); cJSON_AddItemToArray(loud_en_vals, lev);
            cJSON_AddItemToObject(loud_en, "values", loud_en_vals);
            cJSON_AddItemToArray(loud_params, loud_en);

            /* visibleWhen object for conditional params */
            cJSON *visibleWhen = cJSON_CreateObject();
            cJSON_AddNumberToObject(visibleWhen, "loudness_enabled", 1);

            /* Create grouped zones - each zone has threshold (except last), bass, treble */
            for (int i = 0; i < TAS5805M_LOUDNESS_ZONES; i++) {
                cJSON *zone_group = cJSON_CreateObject();
                char zone_name[48];

                /* Calculate volume range for this zone */
                int vol_start = (i == 0) ? 0 : loudness.thresholds[i - 1];
                int vol_end = (i < TAS5805M_LOUDNESS_ZONES - 1) ? loudness.thresholds[i] : 100;
                snprintf(zone_name, sizeof(zone_name), "Zone %d (%d%% - %d%%)", i + 1, vol_start, vol_end);

                cJSON_AddStringToObject(zone_group, "name", zone_name);
                cJSON_AddStringToObject(zone_group, "type", "loudness-zone");
                cJSON_AddNumberToObject(zone_group, "zone_index", i);
                cJSON_AddItemToObject(zone_group, "visibleWhen", cJSON_Duplicate(visibleWhen, 1));

                cJSON *zone_params = cJSON_CreateArray();

                /* Threshold slider (zones 0-3 have thresholds, zone 4 ends at 100%) */
                if (i < TAS5805M_LOUDNESS_ZONES - 1) {
                    char tkey[24];
                    snprintf(tkey, sizeof(tkey), "loudness_thresh_%d", i);
                    cJSON *thresh = cJSON_CreateObject();
                    cJSON_AddStringToObject(thresh, "key", tkey);
                    cJSON_AddStringToObject(thresh, "name", "Upper Limit");
                    cJSON_AddStringToObject(thresh, "type", "range");
                    cJSON_AddStringToObject(thresh, "unit", "%");
                    cJSON_AddNumberToObject(thresh, "min", 0);
                    cJSON_AddNumberToObject(thresh, "max", 100);
                    cJSON_AddNumberToObject(thresh, "step", 5);
                    cJSON_AddNumberToObject(thresh, "current", loudness.thresholds[i]);
                    cJSON_AddItemToArray(zone_params, thresh);
                }

                /* Bass boost */
                char bkey[24];
                snprintf(bkey, sizeof(bkey), "loudness_bass_%d", i);
                cJSON *bass = cJSON_CreateObject();
                cJSON_AddStringToObject(bass, "key", bkey);
                cJSON_AddStringToObject(bass, "name", "Bass");
                cJSON_AddStringToObject(bass, "type", "range");
                cJSON_AddStringToObject(bass, "unit", "dB");
                cJSON_AddNumberToObject(bass, "min", -12);
                cJSON_AddNumberToObject(bass, "max", 12);
                cJSON_AddNumberToObject(bass, "step", 1);
                cJSON_AddNumberToObject(bass, "current", loudness.bass_boost[i]);
                cJSON_AddItemToArray(zone_params, bass);

                /* Treble boost */
                char trkey[24];
                snprintf(trkey, sizeof(trkey), "loudness_treble_%d", i);
                cJSON *treble = cJSON_CreateObject();
                cJSON_AddStringToObject(treble, "key", trkey);
                cJSON_AddStringToObject(treble, "name", "Treble");
                cJSON_AddStringToObject(treble, "type", "range");
                cJSON_AddStringToObject(treble, "unit", "dB");
                cJSON_AddNumberToObject(treble, "min", -12);
                cJSON_AddNumberToObject(treble, "max", 12);
                cJSON_AddNumberToObject(treble, "step", 1);
                cJSON_AddNumberToObject(treble, "current", loudness.treble_boost[i]);
                cJSON_AddItemToArray(zone_params, treble);

                cJSON_AddItemToObject(zone_group, "parameters", zone_params);
                cJSON_AddItemToArray(loud_params, zone_group);
            }

            cJSON_Delete(visibleWhen);
            cJSON_AddItemToObject(loud_section, "parameters", loud_params);
            cJSON_AddItemToArray(sections, loud_section);
        }

        cJSON_AddItemToObject(biamp_group, "sections", sections);
        cJSON_AddItemToArray(groups, biamp_group);
    }

    cJSON_AddItemToObject(root, "groups", groups);

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);
    if (json_len >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer (%zu >= %zu)", __func__, json_len, max_len);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGD(TAG, "%s: EQ schema JSON generated", __func__);
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
    
    // Mark I2S clock as ready (codec is running and can be queried)
    tas5805m_i2s_clock_ready = true;
    
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
    } else if (ui_mode == TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP) {
        // Apply persisted advanced bi-amp crossover settings
        tas5805m_biamp_settings_t biamp;
        if (tas5805m_settings_load_biamp(&biamp) == ESP_OK) {
            ESP_LOGI(TAG, "%s: Restoring advanced bi-amp settings: xover=%dHz slope=%d",
                     __func__, biamp.crossover_freq, biamp.slope);
            if (tas5805m_settings_apply_biamp(&biamp) != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to apply saved bi-amp settings", __func__);
            } else {
                ESP_LOGI(TAG, "%s: Restored advanced bi-amp crossover", __func__);
            }
        }
    }

    // Restore channel gain for all EQ modes (not just presets and advanced biamp)
    // Channel gain is independent of EQ band settings
    // Note: Advanced Bi-Amp handles its own gains internally via crossover filters
    if (ui_mode != TAS5805M_EQ_UI_MODE_OFF && ui_mode != TAS5805M_EQ_UI_MODE_PRESETS &&
        ui_mode != TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP) {
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

    /* Load loudness settings into the static cache and apply if enabled */
    if (tas5805m_settings_load_loudness(&s_loudness_settings) == ESP_OK && s_loudness_settings.enabled) {
        int vol = 50;
        tas5805m_get_volume(&vol);
        ESP_LOGI(TAG, "%s: Restoring loudness compensation (enabled=%d, vol=%d%%)", __func__, s_loudness_settings.enabled, vol);
        tas5805m_loudness_apply(vol);
    }
#endif

    ESP_LOGI(TAG, "%s: Delayed persisted settings application complete", __func__);
    return ESP_OK;
}

/**
 * Get DAC-only settings as JSON string
 */
esp_err_t tas5805m_settings_get_dac_json(char *json_out, size_t max_len) {
    ESP_LOGD(TAG, "%s: max_len=%zu", __func__, max_len);
    
    if (!json_out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Get current state
    TAS5805_STATE dac_state;
    esp_err_t err = tas5805m_get_state(&dac_state);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to get DAC state: %s", __func__, esp_err_to_name(err));
        return err;
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

    // Build JSON
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to create JSON root", __func__);
        return ESP_ERR_NO_MEM;
    }

    // Add DAC-only settings
    cJSON_AddNumberToObject(root, "state", (int)dac_state.state);
    cJSON_AddNumberToObject(root, "digital_volume", digital_volume);
    cJSON_AddNumberToObject(root, "analog_gain", analog_gain);
    cJSON_AddNumberToObject(root, "dac_mode", (int)dac_mode);
    cJSON_AddNumberToObject(root, "mod_mode", (int)mod_mode);
    cJSON_AddNumberToObject(root, "sw_freq", (int)sw_freq);
    cJSON_AddNumberToObject(root, "bd_freq", (int)bd_freq);
    cJSON_AddNumberToObject(root, "mixer_mode", (int)dac_state.mixer_mode);

    // Get and add faults
    TAS5805M_FAULT fault;
    if (tas5805m_get_faults(&fault) == ESP_OK) {
        cJSON *faults_array = tas5805m_create_faults_array(fault);
        if (faults_array) {
            cJSON_AddItemToObject(root, "faults", faults_array);
        }
    }

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);
    if (json_len >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer (%zu >= %zu)", __func__, json_len, max_len);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGD(TAG, "%s: DAC JSON generated: %s", __func__, json_out);
    return ESP_OK;
}

/**
 * Get EQ-only settings as JSON string
 */
esp_err_t tas5805m_settings_get_eq_json(char *json_out, size_t max_len) {
    ESP_LOGD(TAG, "%s: max_len=%zu", __func__, max_len);
    
    if (!json_out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to create JSON root", __func__);
        return ESP_ERR_NO_MEM;
    }

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    // Get current EQ mode
    TAS5805M_EQ_MODE eq_mode_val = TAS5805M_EQ_MODE_OFF;
    if (tas5805m_get_eq_mode(&eq_mode_val) != ESP_OK) {
        eq_mode_val = TAS5805M_EQ_MODE_OFF;
    }
    cJSON_AddNumberToObject(root, "eq_mode", (int)eq_mode_val);

    // Get current EQ UI mode
    TAS5805M_EQ_UI_MODE ui_mode = TAS5805M_EQ_UI_MODE_OFF;
    tas5805m_settings_load_eq_ui_mode(&ui_mode);
    cJSON_AddNumberToObject(root, "eq_ui_mode", (int)ui_mode);

    // Get EQ profiles
    TAS5805M_EQ_PROFILE prof_l = FLAT, prof_r = FLAT;
    tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_LEFT, &prof_l);
    tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_RIGHT, &prof_r);
    cJSON_AddNumberToObject(root, "eq_profile_left", (int)prof_l);
    cJSON_AddNumberToObject(root, "eq_profile_right", (int)prof_r);

    // Get channel gains
    // Prefer reading current driver state; if driver isn't ready or returns an error
    // fall back to persisted values from NVS so the UI reflects saved settings.
    int ch_gain_l = 0, ch_gain_r = 0;
    int8_t chg = 0;
    if (tas5805m_settings_restored) {
        if (tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &chg) == ESP_OK) {
            ch_gain_l = (int)chg;
        } else {
            tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &ch_gain_l);
        }
        if (tas5805m_get_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &chg) == ESP_OK) {
            ch_gain_r = (int)chg;
        } else {
            tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &ch_gain_r);
        }
    } else {
        tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, &ch_gain_l);
        tas5805m_settings_load_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, &ch_gain_r);
    }
    cJSON_AddNumberToObject(root, "channel_gain_left", ch_gain_l);
    cJSON_AddNumberToObject(root, "channel_gain_right", ch_gain_r);

    // Get per-band gains
    // Prefer reading current driver state; if driver isn't ready or returns an error
    // fall back to persisted values from NVS so the UI reflects saved settings.
    for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
        int gain_l = 0, gain_r = 0;
        char key[32];

        // Left channel: prefer driver value, otherwise load persisted NVS value
        if (tas5805m_settings_restored) {
            if (tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, &gain_l) != ESP_OK) {
                tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain_l);
            }
        } else {
            tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain_l);
        }
        snprintf(key, sizeof(key), "eq_gain_l_%d", band);
        cJSON_AddNumberToObject(root, key, gain_l);

        // Right channel: prefer driver value, otherwise load persisted NVS value
        if (tas5805m_settings_restored) {
            if (tas5805m_get_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain_r) != ESP_OK) {
                tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain_r);
            }
        } else {
            tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain_r);
        }
        snprintf(key, sizeof(key), "eq_gain_r_%d", band);
        cJSON_AddNumberToObject(root, key, gain_r);
    }

    // Get loudness enabled state (needed for UI visibility)
    tas5805m_loudness_settings_t loudness;
    tas5805m_settings_load_loudness(&loudness);
    cJSON_AddNumberToObject(root, "loudness_enabled", loudness.enabled);
#else
    // EQ support disabled
    cJSON_AddNumberToObject(root, "eq_mode", 0);
    cJSON_AddNumberToObject(root, "eq_ui_mode", 0);
#endif

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);
    if (json_len >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer (%zu >= %zu)", __func__, json_len, max_len);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGD(TAG, "%s: EQ JSON generated: %s", __func__, json_out);
    return ESP_OK;
}

/**
 * Update DAC-only settings from JSON (excludes EQ settings)
 * Handles: state, digital_volume, analog_gain, dac_mode, modulation_mode, mixer_mode
 */
esp_err_t tas5805m_settings_set_dac_from_json(const char *json_in) {
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

    cJSON_Delete(root);
    return err;
}

/**
 * Update EQ settings from JSON
 */
esp_err_t tas5805m_settings_set_eq_from_json(const char *json_in) {
    if (!json_in) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "%s: Parsing EQ JSON: %s", __func__, json_in);

    cJSON *root = cJSON_Parse(json_in);
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to parse JSON", __func__);
        return ESP_ERR_INVALID_ARG;
    }

#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    bool apply_manual_bq = false;
    bool sync_manual_bq = false;
    
    cJSON *apply_flag = cJSON_GetObjectItem(root, "apply_manual_bq");
    if (apply_flag && cJSON_IsBool(apply_flag)) {
        apply_manual_bq = cJSON_IsTrue(apply_flag);
    }
    
    cJSON *sync_flag = cJSON_GetObjectItem(root, "sync_manual_bq");
    if (sync_flag && cJSON_IsBool(sync_flag)) {
        sync_manual_bq = cJSON_IsTrue(sync_flag);
    }

    // Process EQ settings
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        const char *key = item->string;
        
        if (strcmp(key, "eq_ui_mode") == 0 && cJSON_IsNumber(item)) {
            TAS5805M_EQ_UI_MODE ui_mode = (TAS5805M_EQ_UI_MODE)item->valueint;
            ESP_LOGI(TAG, "%s: Setting EQ UI mode to %d", __func__, (int)ui_mode);
            tas5805m_settings_save_eq_ui_mode(ui_mode);
            
            // Convert UI mode to driver mode and apply
            TAS5805M_EQ_MODE eq_mode = TAS5805M_EQ_MODE_OFF;
            if (ui_mode == TAS5805M_EQ_UI_MODE_OFF) {
                eq_mode = TAS5805M_EQ_MODE_OFF;
            } else if (ui_mode == TAS5805M_EQ_UI_MODE_15_BAND) {
                // 15-band (left channel only) uses standard EQ mode
                eq_mode = TAS5805M_EQ_MODE_ON;
            } else {
                // 15-band biamp, presets, and manual all use BIAMP mode
                eq_mode = TAS5805M_EQ_MODE_BIAMP;
            }
            tas5805m_set_eq_mode(eq_mode);
            tas5805m_settings_save_eq_mode(eq_mode);
            
            // Apply saved state for the new mode
            if (ui_mode == TAS5805M_EQ_UI_MODE_15_BAND) {
                // Apply saved per-band gains for left channel
                ESP_LOGI(TAG, "%s: Applying saved 15-band EQ gains (left channel)", __func__);
                for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
                    int gain = 0;
                    if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                        tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
                    }
                }
            } else if (ui_mode == TAS5805M_EQ_UI_MODE_15_BAND_BIAMP) {
                // Apply saved per-band gains for both channels
                ESP_LOGI(TAG, "%s: Applying saved 15-band biamp EQ gains", __func__);
                for (int band = 0; band < TAS5805M_EQ_BANDS; ++band) {
                    int gain = 0;
                    if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, &gain) == ESP_OK) {
                        tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
                    }
                    if (tas5805m_settings_load_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, &gain) == ESP_OK) {
                        tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, gain);
                    }
                }
            } else if (ui_mode == TAS5805M_EQ_UI_MODE_PRESETS) {
                // Apply saved preset profiles
                TAS5805M_EQ_PROFILE prof_l = FLAT;
                TAS5805M_EQ_PROFILE prof_r = FLAT;
                if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_LEFT, &prof_l) == ESP_OK) {
                    ESP_LOGI(TAG, "%s: Applying saved preset left=%d", __func__, (int)prof_l);
                    tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, prof_l);
                }
                if (tas5805m_settings_load_eq_profile(TAS5805M_EQ_CHANNELS_RIGHT, &prof_r) == ESP_OK) {
                    ESP_LOGI(TAG, "%s: Applying saved preset right=%d", __func__, (int)prof_r);
                    tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_RIGHT, prof_r);
                }
            } else if (ui_mode == TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP) {
                // Apply saved advanced bi-amp crossover settings
                tas5805m_biamp_settings_t biamp;
                if (tas5805m_settings_load_biamp(&biamp) == ESP_OK) {
                    ESP_LOGI(TAG, "%s: Applying saved advanced bi-amp settings", __func__);
                    tas5805m_settings_apply_biamp(&biamp);
                }
            }
        }
        else if (strcmp(key, "eq_profile_l") == 0 && cJSON_IsNumber(item)) {
            TAS5805M_EQ_PROFILE prof = (TAS5805M_EQ_PROFILE)item->valueint;
            ESP_LOGI(TAG, "%s: Setting EQ profile left to %d", __func__, (int)prof);
            tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_LEFT, prof);
            tas5805m_settings_save_eq_profile(TAS5805M_EQ_CHANNELS_LEFT, prof);
        }
        else if (strcmp(key, "eq_profile_r") == 0 && cJSON_IsNumber(item)) {
            TAS5805M_EQ_PROFILE prof = (TAS5805M_EQ_PROFILE)item->valueint;
            ESP_LOGI(TAG, "%s: Setting EQ profile right to %d", __func__, (int)prof);
            tas5805m_set_eq_profile_channel(TAS5805M_EQ_CHANNELS_RIGHT, prof);
            tas5805m_settings_save_eq_profile(TAS5805M_EQ_CHANNELS_RIGHT, prof);
        }
        else if (strcmp(key, "channel_gain_l") == 0 && cJSON_IsNumber(item)) {
            int8_t gain = (int8_t)item->valueint;
            ESP_LOGI(TAG, "%s: Setting channel gain left to %d", __func__, gain);
            tas5805m_set_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, gain);
            tas5805m_settings_save_channel_gain(TAS5805M_EQ_CHANNELS_LEFT, gain);
        }
        else if (strcmp(key, "channel_gain_r") == 0 && cJSON_IsNumber(item)) {
            int8_t gain = (int8_t)item->valueint;
            ESP_LOGI(TAG, "%s: Setting channel gain right to %d", __func__, gain);
            tas5805m_set_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, gain);
            tas5805m_settings_save_channel_gain(TAS5805M_EQ_CHANNELS_RIGHT, gain);
        }
        else if (strncmp(key, "eq_gain_l_", 10) == 0 && cJSON_IsNumber(item)) {
            int band = atoi(key + 10);
            if (band >= 0 && band < TAS5805M_EQ_BANDS) {
                int gain = item->valueint;
                ESP_LOGI(TAG, "%s: Setting EQ gain left band %d to %d", __func__, band, gain);
                tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
                tas5805m_settings_save_eq_gain(TAS5805M_EQ_CHANNELS_LEFT, band, gain);
            }
        }
        else if (strncmp(key, "eq_gain_r_", 10) == 0 && cJSON_IsNumber(item)) {
            int band = atoi(key + 10);
            if (band >= 0 && band < TAS5805M_EQ_BANDS) {
                int gain = item->valueint;
                ESP_LOGI(TAG, "%s: Setting EQ gain right band %d to %d", __func__, band, gain);
                tas5805m_set_eq_gain_channel(TAS5805M_EQ_CHANNELS_RIGHT, band, gain);
                tas5805m_settings_save_eq_gain(TAS5805M_EQ_CHANNELS_RIGHT, band, gain);
            }
        }
        /* Advanced Bi-Amp parameter handling */
        else if (strncmp(key, "biamp_", 6) == 0 && cJSON_IsNumber(item)) {
            /* Load current settings, modify, save, and apply only affected band */
            tas5805m_biamp_settings_t biamp;
            tas5805m_settings_load_biamp(&biamp);

            /* Track what type of change for targeted apply */
            enum {
                BIAMP_CHANGE_NONE = 0,
                BIAMP_CHANGE_CROSSOVER,      /* Requires full apply */
                BIAMP_CHANGE_LOW_GAIN_PHASE,
                BIAMP_CHANGE_HIGH_GAIN_PHASE,
                BIAMP_CHANGE_SUBSONIC,
                BIAMP_CHANGE_TWEETER_DELAY,
                BIAMP_CHANGE_BAFFLE_STEP,
                BIAMP_CHANGE_NOTCH,
                BIAMP_CHANGE_AIR,
                BIAMP_CHANGE_LOW_PEQ,
                BIAMP_CHANGE_HIGH_PEQ
            } change_type = BIAMP_CHANGE_NONE;
            int peq_band_changed = -1;

            if (strcmp(key, "biamp_xover_freq") == 0) {
                /* Clamp crossover frequency to valid range (20Hz - 20000Hz) */
                int freq = item->valueint;
                if (freq < 20) freq = 20;
                if (freq > 20000) freq = 20000;
                biamp.crossover_freq = (uint16_t)freq;
                change_type = BIAMP_CHANGE_CROSSOVER;
                ESP_LOGI(TAG, "%s: Setting bi-amp crossover freq to %d Hz", __func__, biamp.crossover_freq);
            } else if (strcmp(key, "biamp_slope") == 0) {
                biamp.slope = (tas5805m_biamp_slope_t)item->valueint;
                change_type = BIAMP_CHANGE_CROSSOVER;
                ESP_LOGI(TAG, "%s: Setting bi-amp slope to %d", __func__, biamp.slope);
            } else if (strcmp(key, "biamp_type") == 0) {
                biamp.type = (tas5805m_biamp_type_t)item->valueint;
                change_type = BIAMP_CHANGE_CROSSOVER;
                ESP_LOGI(TAG, "%s: Setting bi-amp type to %d", __func__, biamp.type);
            } else if (strcmp(key, "biamp_sample_rate") == 0) {
                biamp.sample_rate = (uint32_t)item->valueint;
                change_type = BIAMP_CHANGE_CROSSOVER;
                ESP_LOGI(TAG, "%s: Setting bi-amp sample rate to %lu Hz", __func__, (unsigned long)biamp.sample_rate);
            } else if (strcmp(key, "biamp_subsonic_freq") == 0) {
                biamp.subsonic_freq = (uint16_t)item->valueint;
                change_type = BIAMP_CHANGE_SUBSONIC;
                ESP_LOGI(TAG, "%s: Setting bi-amp subsonic filter to %d Hz", __func__, biamp.subsonic_freq);
            } else if (strcmp(key, "biamp_low_gain") == 0) {
                biamp.low_gain = (int8_t)(item->valuedouble * 2.0 + (item->valuedouble >= 0 ? 0.5 : -0.5));
                change_type = BIAMP_CHANGE_LOW_GAIN_PHASE;
                ESP_LOGI(TAG, "%s: Setting bi-amp low gain to %.1f dB", __func__, item->valuedouble);
            } else if (strcmp(key, "biamp_low_phase") == 0) {
                biamp.low_phase_invert = (uint8_t)item->valueint;
                change_type = BIAMP_CHANGE_LOW_GAIN_PHASE;
                ESP_LOGI(TAG, "%s: Setting bi-amp low phase to %s", __func__, biamp.low_phase_invert ? "inverted" : "normal");
            } else if (strcmp(key, "biamp_high_gain") == 0) {
                biamp.high_gain = (int8_t)(item->valuedouble * 2.0 + (item->valuedouble >= 0 ? 0.5 : -0.5));
                change_type = BIAMP_CHANGE_HIGH_GAIN_PHASE;
                ESP_LOGI(TAG, "%s: Setting bi-amp high gain to %.1f dB", __func__, item->valuedouble);
            } else if (strcmp(key, "biamp_high_phase") == 0) {
                biamp.high_phase_invert = (uint8_t)item->valueint;
                change_type = BIAMP_CHANGE_HIGH_GAIN_PHASE;
                ESP_LOGI(TAG, "%s: Setting bi-amp high phase to %s", __func__, biamp.high_phase_invert ? "inverted" : "normal");
            } else if (strcmp(key, "biamp_baffle_width") == 0) {
                biamp.baffle_width_cm = (uint8_t)item->valueint;
                change_type = BIAMP_CHANGE_BAFFLE_STEP;
                ESP_LOGI(TAG, "%s: Setting bi-amp baffle width to %d cm", __func__, biamp.baffle_width_cm);
            } else if (strcmp(key, "biamp_baffle_placement") == 0) {
                biamp.baffle_placement = (tas5805m_baffle_placement_t)item->valueint;
                change_type = BIAMP_CHANGE_BAFFLE_STEP;
                ESP_LOGI(TAG, "%s: Setting bi-amp baffle placement to %d", __func__, biamp.baffle_placement);
            } else if (strcmp(key, "biamp_tweeter_delay") == 0) {
                biamp.tweeter_delay_mm = (uint8_t)item->valueint;
                change_type = BIAMP_CHANGE_TWEETER_DELAY;
                ESP_LOGI(TAG, "%s: Setting bi-amp tweeter delay to %d mm", __func__, biamp.tweeter_delay_mm);
            } else if (strcmp(key, "biamp_notch_freq") == 0) {
                biamp.notch_freq = (uint16_t)item->valueint;
                change_type = BIAMP_CHANGE_NOTCH;
                ESP_LOGI(TAG, "%s: Setting bi-amp notch freq to %d Hz", __func__, biamp.notch_freq);
            } else if (strcmp(key, "biamp_notch_gain") == 0) {
                biamp.notch_gain = (int8_t)(item->valuedouble * 2.0 + (item->valuedouble >= 0 ? 0.5 : -0.5));
                change_type = BIAMP_CHANGE_NOTCH;
                ESP_LOGI(TAG, "%s: Setting bi-amp notch gain to %.1f dB", __func__, item->valuedouble);
            } else if (strcmp(key, "biamp_notch_q") == 0) {
                biamp.notch_q_x10 = (uint8_t)(item->valuedouble * 10.0 + 0.5);
                change_type = BIAMP_CHANGE_NOTCH;
                ESP_LOGI(TAG, "%s: Setting bi-amp notch Q to %.1f", __func__, item->valuedouble);
            } else if (strcmp(key, "biamp_air_gain") == 0) {
                biamp.air_gain = (int8_t)(item->valuedouble * 2.0 + (item->valuedouble >= 0 ? 0.5 : -0.5));
                change_type = BIAMP_CHANGE_AIR;
                ESP_LOGI(TAG, "%s: Setting bi-amp air gain to %.1f dB", __func__, item->valuedouble);
            }
            /* Low output PEQ bands */
            else if (strncmp(key, "biamp_low_peq", 13) == 0) {
                int peq_idx = key[13] - '0';
                if (peq_idx >= 0 && peq_idx < TAS5805M_BIAMP_PEQ_BANDS) {
                    peq_band_changed = peq_idx;
                    if (strstr(key, "_freq") != NULL) {
                        biamp.low_peq[peq_idx].freq = (uint16_t)item->valueint;
                        change_type = BIAMP_CHANGE_LOW_PEQ;
                        ESP_LOGI(TAG, "%s: Setting bi-amp low PEQ %d freq to %d Hz", __func__, peq_idx, biamp.low_peq[peq_idx].freq);
                    } else if (strstr(key, "_gain") != NULL) {
                        biamp.low_peq[peq_idx].gain = (int8_t)(item->valuedouble * 2.0 + (item->valuedouble >= 0 ? 0.5 : -0.5));
                        change_type = BIAMP_CHANGE_LOW_PEQ;
                        ESP_LOGI(TAG, "%s: Setting bi-amp low PEQ %d gain to %.1f dB", __func__, peq_idx, item->valuedouble);
                    } else if (strstr(key, "_q") != NULL) {
                        biamp.low_peq[peq_idx].q_x10 = (uint8_t)(item->valuedouble * 10.0 + 0.5);
                        change_type = BIAMP_CHANGE_LOW_PEQ;
                        ESP_LOGI(TAG, "%s: Setting bi-amp low PEQ %d Q to %.1f", __func__, peq_idx, item->valuedouble);
                    }
                }
            }
            /* High output PEQ bands */
            else if (strncmp(key, "biamp_high_peq", 14) == 0) {
                int peq_idx = key[14] - '0';
                if (peq_idx >= 0 && peq_idx < TAS5805M_BIAMP_PEQ_BANDS) {
                    peq_band_changed = peq_idx;
                    if (strstr(key, "_freq") != NULL) {
                        biamp.high_peq[peq_idx].freq = (uint16_t)item->valueint;
                        change_type = BIAMP_CHANGE_HIGH_PEQ;
                        ESP_LOGI(TAG, "%s: Setting bi-amp high PEQ %d freq to %d Hz", __func__, peq_idx, biamp.high_peq[peq_idx].freq);
                    } else if (strstr(key, "_gain") != NULL) {
                        biamp.high_peq[peq_idx].gain = (int8_t)(item->valuedouble * 2.0 + (item->valuedouble >= 0 ? 0.5 : -0.5));
                        change_type = BIAMP_CHANGE_HIGH_PEQ;
                        ESP_LOGI(TAG, "%s: Setting bi-amp high PEQ %d gain to %.1f dB", __func__, peq_idx, item->valuedouble);
                    } else if (strstr(key, "_q") != NULL) {
                        biamp.high_peq[peq_idx].q_x10 = (uint8_t)(item->valuedouble * 10.0 + 0.5);
                        change_type = BIAMP_CHANGE_HIGH_PEQ;
                        ESP_LOGI(TAG, "%s: Setting bi-amp high PEQ %d Q to %.1f", __func__, peq_idx, item->valuedouble);
                    }
                }
            }

            if (change_type != BIAMP_CHANGE_NONE) {
                tas5805m_settings_save_biamp(&biamp);

                /* Apply only if we're in advanced bi-amp mode */
                TAS5805M_EQ_UI_MODE ui_mode = TAS5805M_EQ_UI_MODE_OFF;
                tas5805m_settings_load_eq_ui_mode(&ui_mode);
                if (ui_mode == TAS5805M_EQ_UI_MODE_ADVANCED_BIAMP) {
                    /* Apply only the affected band(s) */
                    switch (change_type) {
                        case BIAMP_CHANGE_CROSSOVER:
                            /* Crossover changes affect multiple bands - do full apply */
                            tas5805m_settings_apply_biamp(&biamp);
                            break;
                        case BIAMP_CHANGE_LOW_GAIN_PHASE:
                            tas5805m_biamp_apply_low_gain_phase(biamp.low_gain, biamp.low_phase_invert, biamp.sample_rate);
                            break;
                        case BIAMP_CHANGE_HIGH_GAIN_PHASE:
                            tas5805m_biamp_apply_high_gain_phase(biamp.high_gain, biamp.high_phase_invert, biamp.sample_rate);
                            break;
                        case BIAMP_CHANGE_SUBSONIC:
                            tas5805m_biamp_apply_subsonic(biamp.subsonic_freq, biamp.sample_rate);
                            break;
                        case BIAMP_CHANGE_TWEETER_DELAY:
                            tas5805m_biamp_apply_tweeter_delay(biamp.tweeter_delay_mm, biamp.sample_rate);
                            break;
                        case BIAMP_CHANGE_BAFFLE_STEP:
                            tas5805m_biamp_apply_baffle_step(biamp.baffle_width_cm, biamp.baffle_placement, biamp.sample_rate);
                            break;
                        case BIAMP_CHANGE_NOTCH:
                            tas5805m_biamp_apply_notch(biamp.notch_freq, biamp.notch_gain, biamp.notch_q_x10, biamp.sample_rate);
                            break;
                        case BIAMP_CHANGE_AIR:
                            tas5805m_biamp_apply_air_shelf(biamp.air_gain, biamp.sample_rate);
                            break;
                        case BIAMP_CHANGE_LOW_PEQ:
                            if (peq_band_changed >= 0) {
                                tas5805m_biamp_apply_low_peq(peq_band_changed, &biamp.low_peq[peq_band_changed], biamp.sample_rate);
                            }
                            break;
                        case BIAMP_CHANGE_HIGH_PEQ:
                            if (peq_band_changed >= 0) {
                                tas5805m_biamp_apply_high_peq(peq_band_changed, &biamp.high_peq[peq_band_changed], biamp.sample_rate);
                            }
                            break;
                        default:
                            break;
                    }
                }
            }
        }
        /* Loudness compensation parameter handling - update static cache directly */
        else if (strncmp(key, "loudness_", 9) == 0 && cJSON_IsNumber(item)) {
            /* Ensure cache is loaded from NVS before modifying */
            tas5805m_settings_load_loudness(&s_loudness_settings);
            bool modified = false;

            if (strcmp(key, "loudness_enabled") == 0) {
                s_loudness_settings.enabled = (uint8_t)item->valueint;
                modified = true;
                ESP_LOGI(TAG, "%s: Setting loudness enabled to %d", __func__, s_loudness_settings.enabled);
            } else if (strncmp(key, "loudness_thresh_", 16) == 0) {
                int idx = key[16] - '0';
                if (idx >= 0 && idx < TAS5805M_LOUDNESS_ZONES - 1) {
                    s_loudness_settings.thresholds[idx] = (uint8_t)item->valueint;
                    modified = true;
                    ESP_LOGI(TAG, "%s: Setting loudness threshold %d to %d%%", __func__, idx, s_loudness_settings.thresholds[idx]);
                }
            } else if (strncmp(key, "loudness_bass_", 14) == 0) {
                int idx = key[14] - '0';
                if (idx >= 0 && idx < TAS5805M_LOUDNESS_ZONES) {
                    s_loudness_settings.bass_boost[idx] = (int8_t)item->valueint;
                    modified = true;
                    ESP_LOGI(TAG, "%s: Setting loudness bass zone %d to %d dB", __func__, idx, s_loudness_settings.bass_boost[idx]);
                }
            } else if (strncmp(key, "loudness_treble_", 16) == 0) {
                int idx = key[16] - '0';
                if (idx >= 0 && idx < TAS5805M_LOUDNESS_ZONES) {
                    s_loudness_settings.treble_boost[idx] = (int8_t)item->valueint;
                    modified = true;
                    ESP_LOGI(TAG, "%s: Setting loudness treble zone %d to %d dB", __func__, idx, s_loudness_settings.treble_boost[idx]);
                }
            }

            if (modified) {
                tas5805m_settings_save_loudness(&s_loudness_settings);
                /* Re-apply loudness with current volume */
                int vol = 50;
                tas5805m_get_volume(&vol);
                tas5805m_loudness_apply(vol);
            }
        }
    }
#endif

    cJSON_Delete(root);
    return ESP_OK;
}

/* ============ Bi-Amp Preset Export/Import ============ */

esp_err_t tas5805m_biamp_preset_export(char *json_out, size_t max_len)
{
    if (!json_out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Load current settings */
    tas5805m_biamp_settings_t biamp;
    tas5805m_settings_load_biamp(&biamp);

    tas5805m_loudness_settings_t loudness;
    tas5805m_settings_load_loudness(&loudness);

    /* Build JSON preset */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    /* Preset metadata */
    cJSON_AddNumberToObject(root, "version", TAS5805M_BIAMP_PRESET_VERSION);
    cJSON_AddStringToObject(root, "type", "biamp_preset");

    /* Crossover settings */
    cJSON *crossover = cJSON_CreateObject();
    cJSON_AddNumberToObject(crossover, "frequency", biamp.crossover_freq);
    cJSON_AddNumberToObject(crossover, "slope", biamp.slope);
    cJSON_AddNumberToObject(crossover, "filter_type", biamp.type);
    cJSON_AddItemToObject(root, "crossover", crossover);

    /* Subsonic filter */
    cJSON_AddNumberToObject(root, "subsonic_freq", biamp.subsonic_freq);

    /* Baffle step compensation */
    cJSON *baffle = cJSON_CreateObject();
    cJSON_AddNumberToObject(baffle, "width_cm", biamp.baffle_width_cm);
    cJSON_AddNumberToObject(baffle, "placement", biamp.baffle_placement);
    cJSON_AddItemToObject(root, "baffle_step", baffle);

    /* Low output (woofer) - gains stored as x2, export as actual dB */
    cJSON *low = cJSON_CreateObject();
    cJSON_AddNumberToObject(low, "gain", biamp.low_gain / 2.0);
    cJSON_AddNumberToObject(low, "phase_invert", biamp.low_phase_invert);
    cJSON *low_peq = cJSON_CreateArray();
    for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS; i++) {
        cJSON *band = cJSON_CreateObject();
        cJSON_AddNumberToObject(band, "freq", biamp.low_peq[i].freq);
        cJSON_AddNumberToObject(band, "gain", biamp.low_peq[i].gain / 2.0);
        cJSON_AddNumberToObject(band, "q", biamp.low_peq[i].q_x10 / 10.0);
        cJSON_AddItemToArray(low_peq, band);
    }
    cJSON_AddItemToObject(low, "peq", low_peq);
    cJSON_AddItemToObject(root, "low_output", low);

    /* High output (tweeter) - gains stored as x2, export as actual dB */
    cJSON *high = cJSON_CreateObject();
    cJSON_AddNumberToObject(high, "gain", biamp.high_gain / 2.0);
    cJSON_AddNumberToObject(high, "phase_invert", biamp.high_phase_invert);
    cJSON_AddNumberToObject(high, "delay_mm", biamp.tweeter_delay_mm);
    cJSON_AddNumberToObject(high, "notch_freq", biamp.notch_freq);
    cJSON_AddNumberToObject(high, "notch_gain", biamp.notch_gain / 2.0);
    cJSON_AddNumberToObject(high, "notch_q", biamp.notch_q_x10 / 10.0);
    cJSON_AddNumberToObject(high, "air_gain", biamp.air_gain / 2.0);
    cJSON *high_peq = cJSON_CreateArray();
    for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS; i++) {
        cJSON *band = cJSON_CreateObject();
        cJSON_AddNumberToObject(band, "freq", biamp.high_peq[i].freq);
        cJSON_AddNumberToObject(band, "gain", biamp.high_peq[i].gain / 2.0);
        cJSON_AddNumberToObject(band, "q", biamp.high_peq[i].q_x10 / 10.0);
        cJSON_AddItemToArray(high_peq, band);
    }
    cJSON_AddItemToObject(high, "peq", high_peq);
    cJSON_AddItemToObject(root, "high_output", high);

    /* Loudness compensation */
    cJSON *loud = cJSON_CreateObject();
    cJSON_AddNumberToObject(loud, "enabled", loudness.enabled);
    cJSON *thresholds = cJSON_CreateArray();
    for (int i = 0; i < TAS5805M_LOUDNESS_ZONES - 1; i++) {
        cJSON_AddItemToArray(thresholds, cJSON_CreateNumber(loudness.thresholds[i]));
    }
    cJSON_AddItemToObject(loud, "thresholds", thresholds);
    cJSON *bass = cJSON_CreateArray();
    cJSON *treble = cJSON_CreateArray();
    for (int i = 0; i < TAS5805M_LOUDNESS_ZONES; i++) {
        cJSON_AddItemToArray(bass, cJSON_CreateNumber(loudness.bass_boost[i]));
        cJSON_AddItemToArray(treble, cJSON_CreateNumber(loudness.treble_boost[i]));
    }
    cJSON_AddItemToObject(loud, "bass_boost", bass);
    cJSON_AddItemToObject(loud, "treble_boost", treble);
    cJSON_AddItemToObject(root, "loudness", loud);

    /* Serialize to string */
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(json_str);
    if (len >= max_len) {
        free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strcpy(json_out, json_str);
    free(json_str);

    ESP_LOGI(TAG, "%s: Exported bi-amp preset (%d bytes)", __func__, len);
    return ESP_OK;
}

esp_err_t tas5805m_biamp_preset_import(const char *json_in)
{
    if (!json_in) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_in);
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to parse JSON", __func__);
        return ESP_ERR_INVALID_ARG;
    }

    /* Verify preset type and version */
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "biamp_preset") != 0) {
        ESP_LOGE(TAG, "%s: Invalid preset type", __func__);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (!version || !cJSON_IsNumber(version) || version->valueint > TAS5805M_BIAMP_PRESET_VERSION) {
        ESP_LOGE(TAG, "%s: Unsupported preset version", __func__);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Load current settings as defaults */
    tas5805m_biamp_settings_t biamp;
    tas5805m_biamp_init_defaults(&biamp);

    tas5805m_loudness_settings_t loudness;
    tas5805m_loudness_init_defaults(&loudness);

    /* Parse crossover settings */
    cJSON *crossover = cJSON_GetObjectItem(root, "crossover");
    if (crossover) {
        cJSON *freq = cJSON_GetObjectItem(crossover, "frequency");
        if (freq && cJSON_IsNumber(freq)) {
            /* Clamp crossover frequency to valid range (20Hz - 20000Hz) */
            int f = freq->valueint;
            if (f < 20) f = 20;
            if (f > 20000) f = 20000;
            biamp.crossover_freq = (uint16_t)f;
        }

        cJSON *slope = cJSON_GetObjectItem(crossover, "slope");
        if (slope && cJSON_IsNumber(slope)) biamp.slope = (tas5805m_biamp_slope_t)slope->valueint;

        cJSON *ftype = cJSON_GetObjectItem(crossover, "filter_type");
        if (ftype && cJSON_IsNumber(ftype)) biamp.type = (tas5805m_biamp_type_t)ftype->valueint;
    }

    /* Parse subsonic filter */
    cJSON *subsonic = cJSON_GetObjectItem(root, "subsonic_freq");
    if (subsonic && cJSON_IsNumber(subsonic)) biamp.subsonic_freq = (uint16_t)subsonic->valueint;

    /* Parse baffle step compensation */
    cJSON *baffle = cJSON_GetObjectItem(root, "baffle_step");
    if (baffle) {
        cJSON *width = cJSON_GetObjectItem(baffle, "width_cm");
        if (width && cJSON_IsNumber(width)) biamp.baffle_width_cm = (uint8_t)width->valueint;

        cJSON *placement = cJSON_GetObjectItem(baffle, "placement");
        if (placement && cJSON_IsNumber(placement)) biamp.baffle_placement = (tas5805m_baffle_placement_t)placement->valueint;
    }

    /* Parse low output - gains in preset are actual dB, store as x2 */
    cJSON *low = cJSON_GetObjectItem(root, "low_output");
    if (low) {
        cJSON *gain = cJSON_GetObjectItem(low, "gain");
        if (gain && cJSON_IsNumber(gain)) {
            biamp.low_gain = (int8_t)(gain->valuedouble * 2.0 + (gain->valuedouble >= 0 ? 0.5 : -0.5));
        }

        cJSON *phase = cJSON_GetObjectItem(low, "phase_invert");
        if (phase && cJSON_IsNumber(phase)) biamp.low_phase_invert = (uint8_t)phase->valueint;

        cJSON *peq = cJSON_GetObjectItem(low, "peq");
        if (peq && cJSON_IsArray(peq)) {
            int idx = 0;
            cJSON *band;
            cJSON_ArrayForEach(band, peq) {
                if (idx >= TAS5805M_BIAMP_PEQ_BANDS) break;
                cJSON *f = cJSON_GetObjectItem(band, "freq");
                cJSON *g = cJSON_GetObjectItem(band, "gain");
                cJSON *q = cJSON_GetObjectItem(band, "q");
                if (f && cJSON_IsNumber(f)) biamp.low_peq[idx].freq = (uint16_t)f->valueint;
                if (g && cJSON_IsNumber(g)) {
                    biamp.low_peq[idx].gain = (int8_t)(g->valuedouble * 2.0 + (g->valuedouble >= 0 ? 0.5 : -0.5));
                }
                if (q && cJSON_IsNumber(q)) biamp.low_peq[idx].q_x10 = (uint8_t)(q->valuedouble * 10.0 + 0.5);
                idx++;
            }
        }
    }

    /* Parse high output - gains in preset are actual dB, store as x2 */
    cJSON *high = cJSON_GetObjectItem(root, "high_output");
    if (high) {
        cJSON *gain = cJSON_GetObjectItem(high, "gain");
        if (gain && cJSON_IsNumber(gain)) {
            biamp.high_gain = (int8_t)(gain->valuedouble * 2.0 + (gain->valuedouble >= 0 ? 0.5 : -0.5));
        }

        cJSON *phase = cJSON_GetObjectItem(high, "phase_invert");
        if (phase && cJSON_IsNumber(phase)) biamp.high_phase_invert = (uint8_t)phase->valueint;

        cJSON *delay = cJSON_GetObjectItem(high, "delay_mm");
        if (delay && cJSON_IsNumber(delay)) biamp.tweeter_delay_mm = (uint8_t)delay->valueint;

        cJSON *notch_freq = cJSON_GetObjectItem(high, "notch_freq");
        if (notch_freq && cJSON_IsNumber(notch_freq)) biamp.notch_freq = (uint16_t)notch_freq->valueint;

        cJSON *notch_gain = cJSON_GetObjectItem(high, "notch_gain");
        if (notch_gain && cJSON_IsNumber(notch_gain)) {
            biamp.notch_gain = (int8_t)(notch_gain->valuedouble * 2.0 + (notch_gain->valuedouble >= 0 ? 0.5 : -0.5));
        }

        cJSON *notch_q = cJSON_GetObjectItem(high, "notch_q");
        if (notch_q && cJSON_IsNumber(notch_q)) biamp.notch_q_x10 = (uint8_t)(notch_q->valuedouble * 10.0 + 0.5);

        cJSON *air_gain_val = cJSON_GetObjectItem(high, "air_gain");
        if (air_gain_val && cJSON_IsNumber(air_gain_val)) {
            biamp.air_gain = (int8_t)(air_gain_val->valuedouble * 2.0 + (air_gain_val->valuedouble >= 0 ? 0.5 : -0.5));
        }

        cJSON *peq = cJSON_GetObjectItem(high, "peq");
        if (peq && cJSON_IsArray(peq)) {
            int idx = 0;
            cJSON *band;
            cJSON_ArrayForEach(band, peq) {
                if (idx >= TAS5805M_BIAMP_PEQ_BANDS) break;
                cJSON *f = cJSON_GetObjectItem(band, "freq");
                cJSON *g = cJSON_GetObjectItem(band, "gain");
                cJSON *q = cJSON_GetObjectItem(band, "q");
                if (f && cJSON_IsNumber(f)) biamp.high_peq[idx].freq = (uint16_t)f->valueint;
                if (g && cJSON_IsNumber(g)) {
                    biamp.high_peq[idx].gain = (int8_t)(g->valuedouble * 2.0 + (g->valuedouble >= 0 ? 0.5 : -0.5));
                }
                if (q && cJSON_IsNumber(q)) biamp.high_peq[idx].q_x10 = (uint8_t)(q->valuedouble * 10.0 + 0.5);
                idx++;
            }
        }
    }

    /* Parse loudness settings */
    cJSON *loud = cJSON_GetObjectItem(root, "loudness");
    if (loud) {
        cJSON *enabled = cJSON_GetObjectItem(loud, "enabled");
        if (enabled && cJSON_IsNumber(enabled)) loudness.enabled = (uint8_t)enabled->valueint;

        cJSON *thresholds = cJSON_GetObjectItem(loud, "thresholds");
        if (thresholds && cJSON_IsArray(thresholds)) {
            int idx = 0;
            cJSON *val;
            cJSON_ArrayForEach(val, thresholds) {
                if (idx >= TAS5805M_LOUDNESS_ZONES - 1) break;
                if (cJSON_IsNumber(val)) loudness.thresholds[idx] = (uint8_t)val->valueint;
                idx++;
            }
        }

        cJSON *bass = cJSON_GetObjectItem(loud, "bass_boost");
        if (bass && cJSON_IsArray(bass)) {
            int idx = 0;
            cJSON *val;
            cJSON_ArrayForEach(val, bass) {
                if (idx >= TAS5805M_LOUDNESS_ZONES) break;
                if (cJSON_IsNumber(val)) loudness.bass_boost[idx] = (int8_t)val->valueint;
                idx++;
            }
        }

        cJSON *treble = cJSON_GetObjectItem(loud, "treble_boost");
        if (treble && cJSON_IsArray(treble)) {
            int idx = 0;
            cJSON *val;
            cJSON_ArrayForEach(val, treble) {
                if (idx >= TAS5805M_LOUDNESS_ZONES) break;
                if (cJSON_IsNumber(val)) loudness.treble_boost[idx] = (int8_t)val->valueint;
                idx++;
            }
        }
    }

    cJSON_Delete(root);

    /* Save and apply settings */
    esp_err_t ret = tas5805m_settings_save_biamp(&biamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to save bi-amp settings", __func__);
        return ret;
    }

    ret = tas5805m_settings_save_loudness(&loudness);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to save loudness settings", __func__);
        return ret;
    }

    /* Apply the settings */
    ret = tas5805m_biamp_apply(&biamp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: Failed to apply bi-amp settings (codec may not be ready)", __func__);
    }

    /* Re-apply loudness with current volume */
    int vol = 50;
    tas5805m_get_volume(&vol);
    tas5805m_loudness_apply(vol);

    ESP_LOGI(TAG, "%s: Imported bi-amp preset successfully", __func__);
    return ESP_OK;
}

/* ============ Bi-Amp Reset to Defaults ============ */

esp_err_t tas5805m_biamp_reset_defaults(void)
{
    ESP_LOGI(TAG, "%s: Resetting bi-amp settings to defaults", __func__);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to open NVS: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

    /* Erase all bi-amp related NVS keys */
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_XOVER_FREQ);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_SLOPE);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_TYPE);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_LOW_GAIN);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_LOW_PHASE);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_HIGH_GAIN);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_HIGH_PHASE);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_SAMPLE_RATE);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BIAMP_SUBSONIC_FREQ);

    /* Erase baffle step keys */
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BAFFLE_WIDTH);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_BAFFLE_PLACEMENT);

    /* Erase tweeter-specific keys */
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_TWEETER_DELAY);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_NOTCH_FREQ);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_NOTCH_GAIN);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_NOTCH_Q);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_AIR_GAIN);

    /* Erase PEQ keys for both channels */
    char key[32];
    for (int i = 0; i < TAS5805M_BIAMP_PEQ_BANDS; i++) {
        snprintf(key, sizeof(key), "%s%d_f", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s%d_g", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s%d_q", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_L, i);
        nvs_erase_key(nvs, key);

        snprintf(key, sizeof(key), "%s%d_f", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s%d_g", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
        nvs_erase_key(nvs, key);
        snprintf(key, sizeof(key), "%s%d_q", TAS5805M_NVS_KEY_BIAMP_PEQ_PREFIX_H, i);
        nvs_erase_key(nvs, key);
    }

    /* Erase loudness keys */
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_LOUDNESS_ENABLED);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_LOUDNESS_THRESH);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_LOUDNESS_BASS);
    nvs_erase_key(nvs, TAS5805M_NVS_KEY_LOUDNESS_TREBLE);

    nvs_commit(nvs);
    nvs_close(nvs);

    /* Initialize bi-amp settings to defaults */
    tas5805m_biamp_settings_t biamp;
    tas5805m_biamp_init_defaults(&biamp);

    /* Initialize loudness settings to defaults */
    tas5805m_loudness_settings_t loudness;
    tas5805m_loudness_init_defaults(&loudness);

    /* Save the default settings */
    ret = tas5805m_settings_save_biamp(&biamp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to save default bi-amp settings", __func__);
        return ret;
    }

    ret = tas5805m_settings_save_loudness(&loudness);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to save default loudness settings", __func__);
        return ret;
    }

    /* Apply the default settings */
    ret = tas5805m_biamp_apply(&biamp);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: Failed to apply bi-amp settings (codec may not be ready)", __func__);
    }

    /* Re-apply loudness with current volume */
    int vol = 50;
    tas5805m_get_volume(&vol);
    tas5805m_loudness_apply(vol);

    ESP_LOGI(TAG, "%s: Bi-amp settings reset to defaults successfully", __func__);
    return ESP_OK;
}

#endif /* CONFIG_DAC_TAS5805M */

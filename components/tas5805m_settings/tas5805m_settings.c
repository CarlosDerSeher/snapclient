/**
 * @file tas5805m_settings.c
 * @brief TAS5805M DAC settings persistence and JSON serialization implementation
 */

#include "tas5805m_settings.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "tas5805m_settings";

// Mutex for thread-safe NVS access
static SemaphoreHandle_t tas5805m_settings_mutex = NULL;

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

esp_err_t tas5805m_settings_init(void) {
    if (tas5805m_settings_mutex == NULL) {
        tas5805m_settings_mutex = xSemaphoreCreateMutex();
        if (tas5805m_settings_mutex == NULL) {
            ESP_LOGE(TAG, "%s: Failed to create mutex", __func__);
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "%s: TAS5805M settings manager initialized", __func__);
    return ESP_OK;
}

esp_err_t tas5805m_settings_save_state(TAS5805M_CTRL_STATE state) {
    ESP_LOGD(TAG, "%s: state=%d", __func__, (int)state);
    
    if (!tas5805m_settings_mutex) {
        ESP_LOGE(TAG, "%s: Not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to open NVS: %s", __func__, esp_err_to_name(err));
        xSemaphoreGive(tas5805m_settings_mutex);
        return err;
    }

    err = nvs_set_i32(h, TAS5805M_NVS_KEY_STATE, (int32_t)state);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    xSemaphoreGive(tas5805m_settings_mutex);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: State saved: %d (%s)", __func__, (int)state, tas5805m_state_to_string(state));
    } else {
        ESP_LOGE(TAG, "%s: Failed to save state: %s", __func__, esp_err_to_name(err));
    }

    return err;
}

esp_err_t tas5805m_settings_load_state(TAS5805M_CTRL_STATE *state) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    
    if (!state) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, TAS5805M_NVS_KEY_STATE, &v);
        nvs_close(h);
        if (err == ESP_OK) {
            *state = (TAS5805M_CTRL_STATE)v;
            ESP_LOGD(TAG, "%s: State from NVS: %d (%s)", __func__, (int)*state, tas5805m_state_to_string(*state));
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }

    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

esp_err_t tas5805m_settings_save_digital_volume(int vol_half_db) {
    ESP_LOGD(TAG, "%s: vol_half_db=%d", __func__, vol_half_db);
    
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, TAS5805M_NVS_KEY_DIGITAL_VOL, (int32_t)vol_half_db);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
}

esp_err_t tas5805m_settings_load_digital_volume(int *vol_half_db) {
    if (!vol_half_db) return ESP_ERR_INVALID_ARG;
    if (!tas5805m_settings_mutex) return ESP_ERR_INVALID_STATE;
    
    if (xSemaphoreTake(tas5805m_settings_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(TAS5805M_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, TAS5805M_NVS_KEY_DIGITAL_VOL, &v);
        if (err == ESP_OK) {
            *vol_half_db = (int)v;
        }
        nvs_close(h);
    }
    
    xSemaphoreGive(tas5805m_settings_mutex);
    return err;
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
        }
        nvs_close(h);
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
        }
        nvs_close(h);
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
        }
        if (err == ESP_OK) {
            err = nvs_get_i32(h, TAS5805M_NVS_KEY_BD_FREQ, &v_bd);
        }
        if (err == ESP_OK) {
            *mode = (TAS5805M_MOD_MODE)v_mode;
            *freq = (TAS5805M_SW_FREQ)v_freq;
            *bd_freq = (TAS5805M_BD_FREQ)v_bd;
            ESP_LOGD(TAG, "%s: Modulation mode from NVS: mode=%d, freq=%d, bd_freq=%d", 
                     __func__, (int)*mode, (int)*freq, (int)*bd_freq);
        }
        nvs_close(h);
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

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    size_t json_len = strlen(json_str);
    ESP_LOGI(TAG, "%s: Generated JSON size: %zu bytes (buffer size: %zu)", __func__, json_len, max_len);

    if (json_len >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer (%zu >= %zu)", __func__, json_len, max_len);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGD(TAG, "%s: JSON generated: %s", __func__, json_out);
    return ESP_OK;
}

esp_err_t tas5805m_settings_set_from_json(const char *json_in) {
    ESP_LOGD(TAG, "%s: json=%s", __func__, json_in);
    
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
        
        // Apply to DAC
        err = tas5805m_set_state(new_state);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied state %d (%s) to DAC", __func__, 
                     (int)new_state, tas5805m_state_to_string(new_state));
            
            // Persist to NVS
            esp_err_t save_err = tas5805m_settings_save_state(new_state);
            if (save_err != ESP_OK) {
                ESP_LOGW(TAG, "%s: Failed to save state to NVS: %s", 
                         __func__, esp_err_to_name(save_err));
            }
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply state to DAC: %s", 
                     __func__, esp_err_to_name(err));
        }
    }

    // Update digital volume if present (expects raw uint8_t value)
    cJSON *dig_vol_item = cJSON_GetObjectItem(root, "digital_volume");
    if (cJSON_IsNumber(dig_vol_item)) {
        uint8_t vol = (uint8_t)dig_vol_item->valueint;
        
        err = tas5805m_set_digital_volume(vol);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied digital volume %d to DAC", __func__, vol);
            // Note: We're saving the raw value, conversion to half_db would need lookup table
            tas5805m_settings_save_digital_volume((int)vol);
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

    // Update modulation mode if present (requires all three parameters)
    cJSON *mod_mode_item = cJSON_GetObjectItem(root, "modulation_mode");
    cJSON *sw_freq_item = cJSON_GetObjectItem(root, "sw_freq");
    cJSON *bd_freq_item = cJSON_GetObjectItem(root, "bd_freq");
    
    if (cJSON_IsNumber(mod_mode_item) && cJSON_IsNumber(sw_freq_item) && cJSON_IsNumber(bd_freq_item)) {
        TAS5805M_MOD_MODE mod_mode = (TAS5805M_MOD_MODE)mod_mode_item->valueint;
        TAS5805M_SW_FREQ sw_freq = (TAS5805M_SW_FREQ)sw_freq_item->valueint;
        TAS5805M_BD_FREQ bd_freq = (TAS5805M_BD_FREQ)bd_freq_item->valueint;
        
        err = tas5805m_set_modulation_mode(mod_mode, sw_freq, bd_freq);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied modulation mode: mode=%d, freq=%d, bd_freq=%d", 
                     __func__, (int)mod_mode, (int)sw_freq, (int)bd_freq);
            tas5805m_settings_save_modulation_mode(mod_mode, sw_freq, bd_freq);
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply modulation mode: %s", 
                     __func__, esp_err_to_name(err));
        }
    }

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
    
    // Digital Volume parameter (raw register value 0-255)
    cJSON *dig_vol_param = cJSON_CreateObject();
    cJSON_AddStringToObject(dig_vol_param, "key", "digital_volume");
    cJSON_AddStringToObject(dig_vol_param, "name", "Digital Volume");
    cJSON_AddStringToObject(dig_vol_param, "type", "range");
    cJSON_AddStringToObject(dig_vol_param, "unit", "register");
    cJSON_AddNumberToObject(dig_vol_param, "min", TAS5805M_VOLUME_DIGITAL_MIN);
    cJSON_AddNumberToObject(dig_vol_param, "max", TAS5805M_VOLUME_DIGITAL_MAX);
    cJSON_AddNumberToObject(dig_vol_param, "step", 1);
    cJSON_AddNumberToObject(dig_vol_param, "default", TAS5805M_VOLUME_DIGITAL_DEFAULT);
    cJSON_AddNumberToObject(dig_vol_param, "current", digital_volume);
    cJSON_AddItemToArray(volume_params, dig_vol_param);
    
    // Analog Gain parameter (raw register value 0-31)
    cJSON *ana_gain_param = cJSON_CreateObject();
    cJSON_AddStringToObject(ana_gain_param, "key", "analog_gain");
    cJSON_AddStringToObject(ana_gain_param, "name", "Analog Gain");
    cJSON_AddStringToObject(ana_gain_param, "type", "range");
    cJSON_AddStringToObject(ana_gain_param, "unit", "register");
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
    
    // State parameter
    cJSON *state_param = cJSON_CreateObject();
    cJSON_AddStringToObject(state_param, "key", "state");
    cJSON_AddStringToObject(state_param, "name", "DAC State");
    cJSON_AddStringToObject(state_param, "type", "enum");
    cJSON_AddNumberToObject(state_param, "current", (int)dac_state.state);
    
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

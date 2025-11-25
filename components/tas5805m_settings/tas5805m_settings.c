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

    // Update digital volume if present
    cJSON *dig_vol_item = cJSON_GetObjectItem(root, "digital_volume");
    if (cJSON_IsNumber(dig_vol_item)) {
        int vol_half_db = dig_vol_item->valueint;
        
        err = tas5805m_set_digital_volume_db(vol_half_db);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied digital volume %.1f dB to DAC", __func__, vol_half_db / 2.0);
            tas5805m_settings_save_digital_volume(vol_half_db);
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply digital volume: %s", 
                     __func__, esp_err_to_name(err));
        }
    }

    // Update analog gain if present
    cJSON *ana_gain_item = cJSON_GetObjectItem(root, "analog_gain");
    if (cJSON_IsNumber(ana_gain_item)) {
        int gain_half_db = ana_gain_item->valueint;
        
        err = tas5805m_set_analog_gain(gain_half_db);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s: Applied analog gain %.1f dB to DAC", __func__, gain_half_db / 2.0);
            tas5805m_settings_save_analog_gain(gain_half_db);
        } else {
            ESP_LOGE(TAG, "%s: Failed to apply analog gain: %s", 
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

    // Get current digital volume
    int dig_vol_half_db = 0;
    if (tas5805m_get_digital_volume_db(&dig_vol_half_db) != ESP_OK) {
        dig_vol_half_db = TAS5805M_DIGITAL_VOL_DEFAULT;
    }

    // Get current analog gain
    int ana_gain_half_db = 0;
    if (tas5805m_get_analog_gain(&ana_gain_half_db) != ESP_OK) {
        ana_gain_half_db = TAS5805M_ANALOG_GAIN_DEFAULT;
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
    
    // Digital Volume parameter
    cJSON *dig_vol_param = cJSON_CreateObject();
    cJSON_AddStringToObject(dig_vol_param, "key", "digital_volume");
    cJSON_AddStringToObject(dig_vol_param, "name", "Digital Volume");
    cJSON_AddStringToObject(dig_vol_param, "type", "range");
    cJSON_AddStringToObject(dig_vol_param, "unit", "dB");
    cJSON_AddNumberToObject(dig_vol_param, "min", TAS5805M_DIGITAL_VOL_MIN);
    cJSON_AddNumberToObject(dig_vol_param, "max", TAS5805M_DIGITAL_VOL_MAX);
    cJSON_AddNumberToObject(dig_vol_param, "step", TAS5805M_DIGITAL_VOL_STEP);
    cJSON_AddNumberToObject(dig_vol_param, "default", TAS5805M_DIGITAL_VOL_DEFAULT);
    cJSON_AddNumberToObject(dig_vol_param, "current", dig_vol_half_db);
    cJSON_AddNumberToObject(dig_vol_param, "scale", TAS5805M_DIGITAL_VOL_SCALE);
    cJSON_AddItemToArray(volume_params, dig_vol_param);
    
    // Analog Gain parameter
    cJSON *ana_gain_param = cJSON_CreateObject();
    cJSON_AddStringToObject(ana_gain_param, "key", "analog_gain");
    cJSON_AddStringToObject(ana_gain_param, "name", "Analog Gain");
    cJSON_AddStringToObject(ana_gain_param, "type", "range");
    cJSON_AddStringToObject(ana_gain_param, "unit", "dB");
    cJSON_AddNumberToObject(ana_gain_param, "min", TAS5805M_ANALOG_GAIN_MIN);
    cJSON_AddNumberToObject(ana_gain_param, "max", TAS5805M_ANALOG_GAIN_MAX);
    cJSON_AddNumberToObject(ana_gain_param, "step", TAS5805M_ANALOG_GAIN_STEP);
    cJSON_AddNumberToObject(ana_gain_param, "default", TAS5805M_ANALOG_GAIN_DEFAULT);
    cJSON_AddNumberToObject(ana_gain_param, "current", ana_gain_half_db);
    cJSON_AddNumberToObject(ana_gain_param, "scale", TAS5805M_ANALOG_GAIN_SCALE);
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
    cJSON *values = cJSON_CreateArray();
    
    cJSON *val_deep_sleep = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_deep_sleep, "value", TAS5805M_CTRL_DEEP_SLEEP);
    cJSON_AddStringToObject(val_deep_sleep, "name", "Deep Sleep");
    cJSON_AddItemToArray(values, val_deep_sleep);
    
    cJSON *val_sleep = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_sleep, "value", TAS5805M_CTRL_SLEEP);
    cJSON_AddStringToObject(val_sleep, "name", "Sleep");
    cJSON_AddItemToArray(values, val_sleep);
    
    cJSON *val_hiz = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_hiz, "value", TAS5805M_CTRL_HI_Z);
    cJSON_AddStringToObject(val_hiz, "name", "Hi-Z");
    cJSON_AddItemToArray(values, val_hiz);
    
    cJSON *val_play = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_play, "value", TAS5805M_CTRL_PLAY);
    cJSON_AddStringToObject(val_play, "name", "Play");
    cJSON_AddItemToArray(values, val_play);
    
    cJSON *val_play_mute = cJSON_CreateObject();
    cJSON_AddNumberToObject(val_play_mute, "value", TAS5805M_CTRL_PLAY_MUTE);
    cJSON_AddStringToObject(val_play_mute, "name", "Play (Muted)");
    cJSON_AddItemToArray(values, val_play_mute);
    
    cJSON_AddItemToObject(state_param, "values", values);
    cJSON_AddItemToArray(state_params, state_param);
    
    cJSON_AddItemToObject(state_group, "parameters", state_params);
    cJSON_AddItemToArray(groups, state_group);

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
